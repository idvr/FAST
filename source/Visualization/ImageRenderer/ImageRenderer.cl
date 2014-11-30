__constant sampler_t sampler = CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP | CLK_FILTER_NEAREST;

__kernel void renderToTextureFloat(
        __read_only image2d_t image,
        __write_only image2d_t texture,
        __private float level,
        __private float window
        ) {
    const int x = get_global_id(0);
    const int y = get_global_id(1);

    // TODO components support
    float value = read_imagef(image, sampler, (int2)(x,y)).x;

    value = (value - level + window/2) / window;
    value = clamp(value, 0.0f, 1.0f);
    write_imagef(texture, (int2)(x,get_global_size(1)-y-1), (float4)(value,value,value,1.0));
}

__kernel void renderToTextureUint(
        __read_only image2d_t image,
        __write_only image2d_t texture,
        __private float level,
        __private float window
        ) {
    const int x = get_global_id(0);
    const int y = get_global_id(1);

    // TODO components support
    float value = (float)read_imageui(image, sampler, (int2)(x,y)).x;

    value = (value - level + window/2) / window;
    value = clamp(value, 0.0f, 1.0f);
    write_imagef(texture, (int2)(x,get_global_size(1)-y-1), (float4)(value,value,value,1.0));
}

__kernel void renderToTextureInt(
        __read_only image2d_t image,
        __write_only image2d_t texture,
        __private float level,
        __private float window
        ) {
    const int x = get_global_id(0);
    const int y = get_global_id(1);

    // TODO components support
    float value = (float)read_imagei(image, sampler, (int2)(x,y)).x;

    value = (value - level + window/2) / window;
    value = clamp(value, 0.0f, 1.0f);
    write_imagef(texture, (int2)(x,get_global_size(1)-y-1), (float4)(value,value,value,1.0));
}