/**************************************************************************/
/*  gtao_upscale.glsl                                                    */
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

// Depth-aware bilateral upsample of the half-resolution, temporally-accumulated GTAO result (Jimenez et al.
// 2016, Section 4.1: "we compute our ambient occlusion on half-resolution, which is later upsampled to full
// resolution") to the full-resolution single-channel buffer the lighting shader samples. Each full-res pixel
// blends its 4 neighboring half-res texels weighted by both bilinear position and how closely each texel's
// stored depth matches this pixel's own (full-resolution) depth, so the effect doesn't bleed across edges
// that only exist at full resolution (e.g. a thin railing that the half-res buffer missed entirely).

#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source_full_depth;
layout(set = 0, binding = 1) uniform sampler2D source_history; // .x = AO, .y = half-res linear depth

layout(r8, set = 1, binding = 0) uniform restrict writeonly image2D dest_final;

layout(push_constant, std430) uniform Params {
	ivec2 full_screen_size;
	ivec2 half_screen_size;

	uint is_orthogonal;
	float depth_linearize_mul;
	float depth_linearize_add;
	float intensity;

	float power;
	float fade_out_mul;
	float fade_out_add;
	float sharpness;
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

	// This full-res pixel's position in half-res texel-index space, and the surrounding 2x2 neighborhood
	// of half-res texel centers, addressed directly by integer index (texelFetch) rather than through a
	// sampler, so this bilateral blend is the only interpolation applied — no separate GPU-bilinear pass
	// hiding underneath it.
	vec2 half_pos = (vec2(pos) + 0.5) * (vec2(params.half_screen_size) / vec2(params.full_screen_size)) - 0.5;
	ivec2 base = ivec2(floor(half_pos));
	vec2 frac = half_pos - vec2(base);

	float bilinear_weights[4] = float[4](
			(1.0 - frac.x) * (1.0 - frac.y),
			frac.x * (1.0 - frac.y),
			(1.0 - frac.x) * frac.y,
			frac.x * frac.y);
	ivec2 offsets[4] = ivec2[4](ivec2(0, 0), ivec2(1, 0), ivec2(0, 1), ivec2(1, 1));

	float ao_sum = 0.0;
	float weight_sum = 0.0;

	// sharpness in [0,1]: higher respects depth discontinuities more strictly (less cross-edge bleeding).
	float depth_tolerance = max(ref_depth * mix(0.05, 0.005, clamp(params.sharpness, 0.0, 1.0)), 0.001);

	for (int i = 0; i < 4; i++) {
		ivec2 tap_pos = clamp(base + offsets[i], ivec2(0), params.half_screen_size - 1);

		vec2 tap = texelFetch(source_history, tap_pos, 0).xy;
		float depth_weight = exp2(-abs(tap.y - ref_depth) / depth_tolerance);
		float weight = bilinear_weights[i] * depth_weight;

		ao_sum += tap.x * weight;
		weight_sum += weight;
	}

	ivec2 nearest_half_pos = clamp(ivec2(round(half_pos)), ivec2(0), params.half_screen_size - 1);
	float visibility = (weight_sum > 0.0001) ? (ao_sum / weight_sum) : texelFetch(source_history, nearest_half_pos, 0).x;

	// Artistic shaping (intensity/power/distance fadeout) is applied here, once, on the final resolved
	// value — the gather and temporal passes both work in physically meaningful [0,1] visibility.
	float obscurance = 1.0 - visibility;
	float fade_out = clamp(ref_depth * params.fade_out_mul + params.fade_out_add, 0.0, 1.0);
	obscurance = clamp(params.intensity * obscurance, 0.0, 1.0) * fade_out;
	float occlusion = pow(clamp(1.0 - obscurance, 0.0, 1.0), params.power);

	imageStore(dest_final, pos, vec4(occlusion));
}
