#include "FAST/Data/Image.hpp"
#include "FAST/Data/Segmentation.hpp"
#include "FAST/Data/PointSet.hpp"
#include "FAST/DeviceManager.hpp"
#include "FAST/Algorithms/GaussianSmoothingFilter/GaussianSmoothingFilter.hpp"
#include "FAST/Algorithms/ImageGradient/ImageGradient.hpp"
#include "FAST/Utility.hpp"
#include "FAST/SceneGraph.hpp"
#include <boost/shared_array.hpp>
#include <queue>
#include "FAST/Algorithms/UltrasoundVesselDetection/UltrasoundVesselDetection.hpp"
#include "FAST/Algorithms/ImageCropper/ImageCropper.hpp"
#include "FAST/Algorithms/ImageClassifier/ImageClassifier.hpp"

namespace fast {

ProcessObjectPort UltrasoundVesselDetection::getOutputImagePort() {
    mCreateSegmentation = true;
    return getOutputPort(0);
}

ProcessObjectPort UltrasoundVesselDetection::getPointSetPort() {
    return getOutputPort(1);
}

UltrasoundVesselDetection::UltrasoundVesselDetection() {
    createInputPort<Image>(0);
    createOutputPort<Segmentation>(0, OUTPUT_DEPENDS_ON_INPUT, 0);
    createOutputPort<PointSet>(1, OUTPUT_DEPENDS_ON_INPUT, 0);
    createOpenCLProgram(std::string(FAST_SOURCE_DIR) + "Algorithms/UltrasoundVesselDetection/UltrasoundVesselDetection.cl");
    mCreateSegmentation = false;
}

struct Candidate {
	float score;
	VesselCrossSection::pointer crossSection;
};

class CandidateComparison {
	public:
		bool operator() (const Candidate& lhs, const Candidate& rhs) const {
			return lhs.score < rhs.score;
		}
};

void UltrasoundVesselDetection::execute() {
    Image::pointer input = getStaticInputData<Image>();
    if(input->getDimensions() != 2) {
        throw Exception("The UltrasoundVesselDetection algorithm is only for 2D");
    }

    // Create kernel
    std::string buildOptions = "";
    if(input->getDataType() == TYPE_FLOAT) {
        buildOptions = "-DTYPE_FLOAT";
    } else if(input->getDataType() == TYPE_INT8 || input->getDataType() == TYPE_INT16) {
        buildOptions = "-DTYPE_INT";
    } else {
        buildOptions = "-DTYPE_UINT";
    }
    OpenCLDevice::pointer device = getMainDevice();
    cl::Program program = getOpenCLProgram(device, "", buildOptions);
    cl::Kernel kernel(program, "vesselDetection");

    // Run GaussianSmoothing on input
    GaussianSmoothingFilter::pointer filter = GaussianSmoothingFilter::New();
    filter->setInputData(input);
    filter->setMaskSize(11);
    filter->setStandardDeviation(3);
    filter->update();
    Image::pointer smoothedImage = filter->getOutputData<Image>();

    // Run ImageGradient on input
    ImageGradient::pointer imageGradient = ImageGradient::New();
    imageGradient->setInputConnection(filter->getOutputPort());
    imageGradient->update();
    Image::pointer gradients = imageGradient->getOutputData<Image>();
    OpenCLImageAccess::pointer inputImageAccess = input->getOpenCLImageAccess(ACCESS_READ, device);
    OpenCLImageAccess::pointer imageAccess = smoothedImage->getOpenCLImageAccess(ACCESS_READ, device);
    OpenCLImageAccess::pointer gradientAccess = gradients->getOpenCLImageAccess(ACCESS_READ, device);

    // Create output image
    cl::Image2D result = cl::Image2D(
            device->getContext(),
            CL_MEM_WRITE_ONLY,
            getOpenCLImageFormat(device, CL_MEM_OBJECT_IMAGE2D, TYPE_FLOAT, 4),
            input->getWidth(), input->getHeight()
    );

    //reportInfo() << "Minimum intensity of smoothed image is: " << smoothedImage->calculateMinimumIntensity() << Reporter::end;
    //reportInfo() << "Maximum intensity of smoothed image is: " << smoothedImage->calculateMaximumIntensity() << Reporter::end;

    // Run vessel detection kernel on smoothed image and gradient
    kernel.setArg(0, *inputImageAccess->get2DImage());
    kernel.setArg(1, *gradientAccess->get2DImage());
    kernel.setArg(2, result);
    kernel.setArg(3, input->getSpacing().x());

    const float minimumDepthInMM = 5;
    const float spacing = input->getSpacing().y();
    //const float maximumDepthInMM = 20;
    const float maximumDepthInMM = input->getHeight()*spacing*0.85;
    uint startPosY = round(minimumDepthInMM/spacing);
    uint endPosY = round(maximumDepthInMM/spacing);

    device->getCommandQueue().enqueueNDRangeKernel(
            kernel,
            cl::NDRange(0, startPosY),
            cl::NDRange(input->getWidth(), endPosY-startPosY),
            cl::NullRange
    );

    // Get result back
    boost::shared_array<float> data(new float[input->getWidth()*input->getHeight()*4]);
    device->getCommandQueue().enqueueReadImage(
            result,
            CL_TRUE,
            createOrigoRegion(),
            createRegion(input->getWidth(),input->getHeight(),1),
            0,0,
            data.get()
    );

	AffineTransformation::pointer transform = SceneGraph::getAffineTransformationFromData(input);
	transform->scale(input->getSpacing());

	// TODO sort cross sections after score, then

    // Find best ellipses
	std::priority_queue<Candidate, std::vector<Candidate>, CandidateComparison> candidates;
    for(uint x = 0; x < input->getWidth(); x++) {
        for(uint y = startPosY; y < endPosY; y++) {
            uint i = x + y*input->getWidth();

            if(data[i*4] > 0.5) { // If score is higher than a threshold
				float posY = floor(data[i*4+3]/input->getWidth());
				float posX = data[i*4+3]-posY*input->getWidth();
				Vector3f voxelPosition(posX, posY, 0);
				Vector3f position = transform->multiply(voxelPosition);
				VesselCrossSection::pointer crossSection = VesselCrossSection::New();
				crossSection->create(position, voxelPosition.head(2), data[i*4+1], data[i*4+2]*data[i*4+1]);
				Candidate candidate;
				candidate.score = data[i*4];
				candidate.crossSection = crossSection;
				candidates.push(candidate);

            }
        }
    }

	mCrossSections.clear();
	// Go through all candidates
	while(!candidates.empty()) {
		Candidate next = candidates.top();
		candidates.pop();

		// Check if valid
		bool invalid = false;
		for(VesselCrossSection::pointer crossSection : mCrossSections) {
			VesselCrossSectionAccess::pointer access = crossSection->getAccess(ACCESS_READ);
			Vector2f imageCenter = access->getImageCenterPosition();
			float majorRadius = access->getMajorRadius();
			VesselCrossSectionAccess::pointer access2 = next.crossSection->getAccess(ACCESS_READ);
			if((access2->getImageCenterPosition() - imageCenter).norm() < majorRadius) {
				invalid = true;
				break;
			}
		}
		if(!invalid) {
			mCrossSections.push_back(next.crossSection);
		}
	}

	ImageClassifier::pointer classifier = ImageClassifier::New();
	std::string modelFile = "/home/smistad/workspace/caffe-test/source/models/vessel_ultrasound_lenet/deploy.prototxt";
	std::string trainingFile = "/home/smistad/workspace/caffe-test/source/models/vessel_ultrasound_lenet/snapshot_iter_90.caffemodel";
	std::string meanFile = "/home/smistad/workspace/caffe-test/source/models/vessel_ultrasound_lenet/mean.binaryproto";
	classifier->loadModel(modelFile, trainingFile, meanFile);
    Vector3ui imageSize = input->getSize();
	const int frameSize = 40; // Nr if pixels to include around vessel

	std::vector<VesselCrossSection::pointer> acceptedVessels;
	// Create sub images and send to classifier
    for(VesselCrossSection::pointer crossSection : mCrossSections) {
        VesselCrossSectionAccess::pointer access = crossSection->getAccess(ACCESS_READ);
        Vector2f imageCenter = access->getImageCenterPosition();

        // Radius in pixels
        float majorRadius = access->getMajorRadius();
        float minorRadius = access->getMinorRadius();

        Vector2i offset(
                        round(imageCenter.x() - majorRadius) - frameSize,
                        round(imageCenter.y() - minorRadius) - frameSize
        );
        Vector2ui size(
                        2*majorRadius + 2*frameSize,
                        2*minorRadius + 2*frameSize
        );

        // Clamp to image bounds
        if(offset.x() < 0)
                offset.x() = 0;
        if(offset.y() < 0)
                offset.y() = 0;
        if(offset.x() + size.x() > imageSize.x())
                size.x() = imageSize.x() - offset.x();
        if(offset.y() + size.y() > imageSize.y())
                size.y() = imageSize.y() - offset.y();

        std::cout << "Center: " << imageCenter.transpose() << std::endl;
        std::cout << "Major radius: " << majorRadius << std::endl;
        std::cout << "Minor radius: " << minorRadius << std::endl;
        std::cout << "Offset: " << offset.transpose() << std::endl;
        std::cout << "Size: " << size.transpose() << std::endl;

        ImageCropper::pointer cropper = ImageCropper::New();
        cropper->setInputData(input);
        cropper->setOffset(offset.cast<uint>());
        cropper->setSize(size);

        classifier->setLabels({"Not vessel", "Vessel"});
        classifier->setInputConnection(cropper->getOutputPort());
        classifier->update();

        if(classifier->getResult()["Vessel"] > 0.9) {
        	std::cout << "accepted" << std::endl;
        	acceptedVessels.push_back(crossSection);
        }
    }

    if(mCreateSegmentation) {
        Segmentation::pointer segmentation = getStaticOutputData<Segmentation>(0);
        segmentation->createFromImage(input);


        OpenCLDevice::pointer device = getMainDevice();

        // Copy contents
        OpenCLImageAccess::pointer writeAccess = segmentation->getOpenCLImageAccess(ACCESS_READ_WRITE, device);
        cl::Image2D* outputData = writeAccess->get2DImage();
        // Create all zero data
        boost::shared_array<uchar> zeroData(new uchar[input->getWidth()*input->getHeight()]());
        device->getCommandQueue().enqueueWriteImage(
                *outputData,
                CL_TRUE,
                createOrigoRegion(),
                createRegion(input->getWidth(), input->getHeight(), 1),
                0, 0,
                zeroData.get()
                );
        cl::Kernel kernel(program, "createSegmentation");

        for(VesselCrossSection::pointer crossSection : acceptedVessels) {
        //if(acceptedVessels.size() > 0) {
            VesselCrossSectionAccess::pointer access = crossSection->getAccess(ACCESS_READ);
            Vector2f imageCenter = access->getImageCenterPosition();
            std::cout << imageCenter.transpose() << std::endl;
            std::cout << access->getMajorRadius() << std::endl;
            std::cout << access->getMinorRadius() << std::endl;
            kernel.setArg(0, *outputData);
            kernel.setArg(1, imageCenter.x());
            kernel.setArg(2, imageCenter.y());
            kernel.setArg(3, access->getMajorRadius());
            kernel.setArg(4, access->getMinorRadius());

            device->getCommandQueue().enqueueNDRangeKernel(
                    kernel,
                    cl::NullRange,
                    cl::NDRange(input->getWidth(), input->getHeight()),
                    cl::NullRange
            );
        }
    }
}

std::vector<VesselCrossSection::pointer> UltrasoundVesselDetection::getCrossSections() {
	return mCrossSections;
}

float UltrasoundVesselDetection::getDetectedRadius() const {
    return mDetectedRadius;
}

float UltrasoundVesselDetection::getDetectedFlattening() const {
    return mDetectedFlattening;
}

} // end namespace fast
