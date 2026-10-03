#[compute]

#version 450

#VERSION_DEFINES

// Builds one mipmap of a tile of the virtual texture page cache from the one above it. Every tile
// carries a couple of mipmaps of its own: sampling one between them is what blends a page smoothly
// into its parent (whose texels the next mipmap holds, at the same density), and the last ones
// stretch the coarsest page of a texture a little further into the distance.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// Runtime virtual textures also keep the height of the ground in every page, in a cache of its own
// (one channel, HEIGHT_FORMAT): MODE_RESOLVE_HEIGHT turns the depth buffer a page was drawn with into
// it, and MODE_DOWNSAMPLE_HEIGHT builds its mipmaps like the others'.

#if defined(MODE_RESOLVE_HEIGHT)
layout(set = 0, binding = 0) uniform sampler2D depth_buffer;
layout(HEIGHT_FORMAT, set = 0, binding = 1) uniform restrict writeonly image2DArray dest_mip;
#elif defined(MODE_DOWNSAMPLE_HEIGHT)
layout(HEIGHT_FORMAT, set = 0, binding = 0) uniform restrict readonly image2DArray source_mip;
layout(HEIGHT_FORMAT, set = 0, binding = 1) uniform restrict writeonly image2DArray dest_mip;
#else
layout(rgba8, set = 0, binding = 0) uniform restrict readonly image2DArray source_mip;
layout(rgba8, set = 0, binding = 1) uniform restrict writeonly image2DArray dest_mip;
#endif

layout(push_constant, std430) uniform Params {
	ivec2 dest_origin;
	int dest_size;
	int layers;
	// MODE_RESOLVE_HEIGHT: where the page is in the depth buffer, and how its depth maps to the
	// height of the volume it was drawn from, 0 at its bottom and 1 at its top.
	ivec2 source_origin;
	float depth_to_height_scale;
	float depth_to_height_bias;
}
params;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, ivec2(params.dest_size)))) {
		return;
	}
	ivec2 dest = params.dest_origin + pos;

#if defined(MODE_RESOLVE_HEIGHT)
	// Pages are drawn orthographically, so depth is linear in height. Where nothing was drawn, the
	// cleared depth lands below the volume: 0, which reads as no ground at all.
	float depth = texelFetch(depth_buffer, params.source_origin + pos, 0).r;
	float height = clamp(depth * params.depth_to_height_scale + params.depth_to_height_bias, 0.0, 1.0);
	imageStore(dest_mip, ivec3(dest, 0), vec4(height));
#else
	ivec2 src = dest * 2;
	for (int i = 0; i < params.layers; i++) {
		vec4 sum = imageLoad(source_mip, ivec3(src, i));
		sum += imageLoad(source_mip, ivec3(src + ivec2(1, 0), i));
		sum += imageLoad(source_mip, ivec3(src + ivec2(0, 1), i));
		sum += imageLoad(source_mip, ivec3(src + ivec2(1, 1), i));
		imageStore(dest_mip, ivec3(dest, i), sum * 0.25);
	}
#endif
}
