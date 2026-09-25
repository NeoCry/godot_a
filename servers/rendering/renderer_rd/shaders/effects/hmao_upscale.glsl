/**************************************************************************/
/*  hmao_upscale.glsl                                                     */
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

// Resolve pass for height map ambient occlusion: a depth-aware blur of hmao.glsl's gather, upsampled to the
// full resolution buffer the lighting shader samples. Two jobs in one filter - it spreads each pixel's few
// rotated directions over its neighborhood (the gather has no temporal accumulation behind it, so this is the
// only place its dither is resolved), and it keeps that blur from crossing depth discontinuities, which at
// this scale would drag a distant valley's occlusion onto the silhouette of whatever stands in front of it.

#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source_full_depth;
layout(set = 0, binding = 1) uniform sampler2D source_ao; // .x = visibility, .y = linear depth

layout(r8, set = 1, binding = 0) uniform restrict writeonly image2D dest_final;

layout(push_constant, std430) uniform Params {
	ivec2 full_screen_size;
	ivec2 source_size;

	uint is_orthogonal;
	float depth_linearize_mul;
	float depth_linearize_add;
	int blur_radius; // In source texels, around the tap the full resolution pixel lands on.
}
params;

float linearize_depth(float p_depth) {
	if (params.is_orthogonal != 0) {
		return mix(params.depth_linearize_mul, params.depth_linearize_add, p_depth);
	}
	return params.depth_linearize_mul / (params.depth_linearize_add - p_depth);
}

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.full_screen_size))) {
		return;
	}

	float ref_depth = linearize_depth(texelFetch(source_full_depth, pos, 0).x);

	// This full resolution pixel's position in source texel-index space. Taps are addressed by integer index
	// so that the bilateral weights below are the only interpolation applied, with no GPU bilinear filtering
	// hiding underneath them.
	vec2 source_pos = (vec2(pos) + 0.5) * (vec2(params.source_size) / vec2(params.full_screen_size)) - 0.5;
	ivec2 base = ivec2(floor(source_pos));

	// Relative tolerance: at 5 m a 10 cm depth gap is an edge, at 500 m it is the same surface seen edge-on.
	float depth_tolerance = max(ref_depth * 0.02, 0.01);

	float ao_sum = 0.0;
	float weight_sum = 0.0;

	for (int y = -params.blur_radius; y <= params.blur_radius; y++) {
		for (int x = -params.blur_radius; x <= params.blur_radius; x++) {
			ivec2 tap_pos = clamp(base + ivec2(x, y), ivec2(0), params.source_size - 1);
			vec2 tap = texelFetch(source_ao, tap_pos, 0).xy;

			// Bilinear-style falloff towards the edge of the kernel, so the filter stays centered on the
			// pixel's own sub-texel position instead of snapping from one source texel to the next.
			vec2 delta = abs(vec2(base + ivec2(x, y)) - source_pos);
			float spatial_weight = max(1.0 - delta.x / float(params.blur_radius + 1), 0.0) * max(1.0 - delta.y / float(params.blur_radius + 1), 0.0);
			float depth_weight = exp2(-abs(tap.y - ref_depth) / depth_tolerance);

			float weight = spatial_weight * depth_weight;
			ao_sum += tap.x * weight;
			weight_sum += weight;
		}
	}

	ivec2 nearest = clamp(ivec2(round(source_pos)), ivec2(0), params.source_size - 1);
	float visibility = (weight_sum > 0.0001) ? (ao_sum / weight_sum) : texelFetch(source_ao, nearest, 0).x;

	imageStore(dest_final, pos, vec4(visibility));
}
