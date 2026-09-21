/**************************************************************************/
/*  gtao_temporal.glsl                                                   */
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

// Spatial pre-filter + temporal reprojection/accumulation for GTAO, matching the paper's own approach
// (Section 4.1: "we distribute the occlusion integral over both space and time... using an exponential
// accumulation buffer"). A small depth-aware bilateral gather denoises this frame's raw horizon-search
// result, which is then blended with the reprojected history from last frame; disocclusion (a surface that
// wasn't visible last frame, or wasn't there at all) falls back to the spatial-only result instead of
// blending in stale/wrong history.
//
// The reprojection math (NDC round-trip through the `reprojection` matrix) mirrors the proven pattern
// already used by SSIL's last-frame-color sampling (servers/rendering/renderer_rd/shaders/effects/ssil.glsl)
// for the same codebase and camera conventions, so it doesn't have to re-derive NDC/Y-flip conventions from
// scratch. The matrix itself expects a genuine NDC-Z input/output matching the projection's actual hardware
// convention (reverse-Z, Vulkan-remapped to [0,1]) at both ends, not a linear stand-in built from z_near/
// z_far -- the two are related hyperbolically, not linearly, for a perspective projection, and since the
// matrix mixes all four components together, feeding or reading the wrong one corrupts the reprojected UV
// itself, not just the depth-based disocclusion check that reads it back afterwards.

#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source_raw_ao;
layout(set = 0, binding = 1) uniform sampler2D source_depth;
layout(set = 0, binding = 2) uniform sampler2D source_history;

layout(rg16f, set = 1, binding = 0) uniform restrict writeonly image2D dest_history;

layout(set = 2, binding = 0) uniform ReprojectionConstants {
	mat4 reprojection;
}
reprojection_constants;

layout(push_constant, std430) uniform Params {
	ivec2 screen_size;
	vec2 pixel_size;

	bool is_orthogonal;
	float depth_linearize_mul;
	float depth_linearize_add;
	float history_weight;

	bool history_is_valid;
	float sharpness;
}
params;

float linearize_depth(float p_ndc_z) {
	if (params.is_orthogonal) {
		return mix(params.depth_linearize_mul, params.depth_linearize_add, p_ndc_z);
	}
	return params.depth_linearize_mul / max(params.depth_linearize_add - p_ndc_z, 0.0001);
}

// Small cross-shaped bilateral gather: cheap, and only meant to take the edge off this frame's raw
// per-pixel noise before it meets the temporal accumulator (which does the heavy lifting over time).
float spatial_prefilter(ivec2 p_pos, float p_center_ao, float p_center_depth) {
	const ivec2 offsets[4] = ivec2[4](ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1));

	float sum = p_center_ao;
	float weight_sum = 1.0;

	// sharpness in [0,1]: higher respects depth discontinuities more strictly (less cross-edge blending).
	float depth_tolerance = max(p_center_depth * mix(0.05, 0.005, clamp(params.sharpness, 0.0, 1.0)), 0.001);

	for (int i = 0; i < 4; i++) {
		ivec2 tap_pos = clamp(p_pos + offsets[i], ivec2(0), params.screen_size - 1);
		float tap_depth = texelFetch(source_depth, tap_pos, 0).x;
		float tap_ao = texelFetch(source_raw_ao, tap_pos, 0).x;

		float depth_weight = exp2(-abs(tap_depth - p_center_depth) / depth_tolerance);
		sum += tap_ao * depth_weight;
		weight_sum += depth_weight;
	}

	return sum / weight_sum;
}

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.screen_size))) {
		return;
	}

	float depth = texelFetch(source_depth, pos, 0).x;
	float raw_ao = texelFetch(source_raw_ao, pos, 0).x;
	float spatial_ao = spatial_prefilter(pos, raw_ao, depth);

	vec2 uv = (vec2(pos) + 0.5) * params.pixel_size;

	// Round-trip this pixel's position through the accumulated view/projection change since last frame to
	// find where it was on screen then (see file header for why this specific formulation is used). This is
	// the inverse of linearize_depth() above: recovers the hardware-depth-equivalent NDC-Z that would have
	// produced this frame's already-linear `depth`, since that's what the reprojection matrix expects.
	float ndc_z = params.is_orthogonal
			? clamp((depth - params.depth_linearize_mul) / max(params.depth_linearize_add - params.depth_linearize_mul, 0.0001), 0.0, 1.0)
			: clamp(params.depth_linearize_add - params.depth_linearize_mul / max(depth, 0.0001), 0.0, 1.0);
	vec4 clip_prev = reprojection_constants.reprojection * vec4(uv * 2.0 - 1.0, ndc_z, 1.0);

	float result_ao = spatial_ao;
	float history_confidence = 0.0;

	if (params.history_is_valid && clip_prev.w > 0.0001) {
		vec2 uv_prev = (clip_prev.xy / clip_prev.w) * 0.5 + 0.5;

		if (all(greaterThanEqual(uv_prev, vec2(0.0))) && all(lessThan(uv_prev, vec2(1.0)))) {
			vec2 history = textureLod(source_history, uv_prev, 0.0).xy;
			float history_ao = history.x;
			float history_depth = history.y;

			// Same linearize_depth() as above, applied to the reprojected point's own NDC-Z instead of a
			// fresh hardware depth sample (assumes last frame's projection had the same near/far/FOV as this
			// frame's, which only briefly doesn't hold on the rare frame those settings actually change).
			float expected_prev_depth = linearize_depth(clip_prev.z / clip_prev.w);
			float depth_error = abs(history_depth - expected_prev_depth) / max(expected_prev_depth, 0.001);

			// A moving/disoccluded surface won't match the depth the history was recorded at; reject it
			// smoothly rather than with a hard cutoff, to avoid a visible boundary snapping in and out.
			history_confidence = params.history_weight * clamp(1.0 - depth_error * 8.0, 0.0, 1.0);
			result_ao = mix(spatial_ao, history_ao, history_confidence);
		}
	}

	imageStore(dest_history, pos, vec4(result_ao, depth, 0.0, 0.0));
}
