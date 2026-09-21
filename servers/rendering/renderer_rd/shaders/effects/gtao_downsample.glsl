/**************************************************************************/
/*  gtao_downsample.glsl                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

// Dedicated depth downsampler for GTAO (servers/rendering/renderer_rd/effects/gtao.cpp), decoupled from the
// generic SSIL/SS-effects downsampler. MODE_BASE converts the hardware depth buffer to linear (positive
// distance from the eye) depth at half resolution; MODE_MIP repeatedly halves that to build a small mip
// chain. Each 2x2 reduction keeps the closest (minimum distance) sample rather than averaging, so thin
// foreground occluders survive into the coarser mips instead of being blended away — the horizon search in
// gtao.glsl trades a little bias for never losing an occluder entirely.

#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(push_constant, std430) uniform Params {
	vec2 pixel_size;
	uint is_orthogonal;
	// Only meaningful for MODE_BASE: whether dest is a genuine half-res reduction of source_depth (half_size
	// enabled) or a same-size copy of it (half_size disabled, working_size == full_size).
	uint half_size;

	float depth_linearize_mul;
	float depth_linearize_add;
	vec2 pad2;
}
params;

#ifdef MODE_BASE
layout(set = 0, binding = 0) uniform sampler2D source_depth;
#else
layout(set = 0, binding = 0) uniform sampler2D source_mip;
#endif

layout(r16f, set = 1, binding = 0) uniform restrict writeonly image2D dest_image;

float linearize_depth(float p_depth) {
	if (params.is_orthogonal != 0) {
		return mix(params.depth_linearize_mul, params.depth_linearize_add, p_depth);
	}
	return params.depth_linearize_mul / (params.depth_linearize_add - p_depth);
}

void main() {
	ivec2 dest_pos = ivec2(gl_GlobalInvocationID.xy);
	ivec2 dest_size = imageSize(dest_image);
	if (any(greaterThanEqual(dest_pos, dest_size))) {
		return;
	}

#ifdef MODE_BASE
	float closest;
	if (params.half_size != 0u) {
		// dest is half the resolution of source_depth: gather the 2x2 source block this dest texel reduces.
		vec2 uv = (vec2(dest_pos) * 2.0 + 1.0) * params.pixel_size;
		vec4 depths = textureGather(source_depth, uv);
		closest = min(min(depths.x, depths.y), min(depths.z, depths.w));
	} else {
		// dest is the same resolution as source_depth (half_size disabled): a straight per-texel copy, not a
		// reduction. textureGather at a texel center (rather than a shared 2x2 corner) has no well-defined
		// "which 4 texels" answer, so this reads the matching texel directly instead.
		closest = texelFetch(source_depth, dest_pos, 0).x;
	}
	float result = linearize_depth(closest);
#else
	// source_mip is a view of the previous (twice as large) mip level; params.pixel_size is that level's texel
	// size. This step is always a genuine 2x reduction within the already-fixed working-resolution mip chain,
	// regardless of half_size, so it doesn't need the same branch as MODE_BASE above.
	vec2 uv = (vec2(dest_pos) * 2.0 + 1.0) * params.pixel_size;
	vec4 depths = textureGather(source_mip, uv);
	float result = min(min(depths.x, depths.y), min(depths.z, depths.w));
#endif

	imageStore(dest_image, dest_pos, vec4(result));
}
