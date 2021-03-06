%include "FAST/Visualization/Renderer.i"

%shared_ptr(fast::ImageRenderer)

namespace fast {

class ImageRenderer : public Renderer {
    public:
    	static SharedPointer<ImageRenderer> New();
        void addInputConnection(ProcessObjectPort port);
	private:
    	ImageRenderer();
};

%template(ImageRendererPtr) SharedPointer<ImageRenderer>;

} // end namespace fast