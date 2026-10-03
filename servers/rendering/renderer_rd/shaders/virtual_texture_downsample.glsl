#[compute]

#version 450

#VERSION_DEFINES

// Builds one mipmap of a tile of the virtual texture page cache from the one above it. Every tile
// carries a couple of mipmaps of its own: sampling one between them is what blends a page smoothly
// into its parent (whose texels the next mipmap holds, at the same density), and the last ones
// stretch the coarsest page of a texture a little further into the distance.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(rgba8, set = 0, binding = 0) uniform restrict readonly image2DArray source_mip;
layout(rgba8, set = 0, binding = 1) uniform restrict writeonly image2DArray dest_mip;

layout(push_constant, std430) uniform Params {
	ivec2 dest_origin;
	int dest_size;
	int layers;
}
params;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, ivec2(params.dest_size)))) {
		return;
	}

	ivec2 dest = params.dest_origin + pos;
	ivec2 src = dest * 2;
	for (int i = 0; i < params.layers; i++) {
		vec4 sum = imageLoad(source_mip, ivec3(src, i));
		sum += imageLoad(source_mip, ivec3(src + ivec2(1, 0), i));
		sum += imageLoad(source_mip, ivec3(src + ivec2(0, 1), i));
		sum += imageLoad(source_mip, ivec3(src + ivec2(1, 1), i));
		imageStore(dest_mip, ivec3(dest, i), sum * 0.25);
	}
}
