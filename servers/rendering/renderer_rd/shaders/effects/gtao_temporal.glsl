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
// The reprojection math (NDC round-trip through the `reprojection` matrix, depth re-normalized against
// z_near/z_far rather than carried as raw hardware depth) mirrors the proven pattern already used by SSIL's
// last-frame-color sampling (servers/rendering/renderer_rd/shaders/effects/ssil.glsl) for the same codebase
// and camera conventions, so it doesn't have to re-derive NDC/Y-flip conventions from scratch.

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

	float z_near;
	float z_far;
	float history_weight;
	bool history_is_valid;

	float sharpness;
	// float[3], not vec3: a bare vec3 in std430 still gets 16-byte base alignment (only arrays/structs lose
	// the std140 rounding), which would silently pad this block out to 64 bytes against the C++ side's 48.
	float pad[3];
}
params;

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
	// find where it was on screen then (see file header for why this specific formulation is used).
	float ndc_z = clamp((depth - params.z_near) / max(params.z_far - params.z_near, 0.0001) * 2.0 - 1.0, -1.0, 1.0);
	vec4 clip_prev = reprojection_constants.reprojection * vec4(uv * 2.0 - 1.0, ndc_z, 1.0);

	float result_ao = spatial_ao;
	float history_confidence = 0.0;

	if (params.history_is_valid && clip_prev.w > 0.0001) {
		vec2 uv_prev = (clip_prev.xy / clip_prev.w) * 0.5 + 0.5;

		if (all(greaterThanEqual(uv_prev, vec2(0.0))) && all(lessThan(uv_prev, vec2(1.0)))) {
			vec2 history = textureLod(source_history, uv_prev, 0.0).xy;
			float history_ao = history.x;
			float history_depth = history.y;

			float expected_prev_depth = ((clip_prev.z / clip_prev.w) * 0.5 + 0.5) * (params.z_far - params.z_near) + params.z_near;
			float depth_error = abs(history_depth - expected_prev_depth) / max(expected_prev_depth, 0.001);

			// A moving/disoccluded surface won't match the depth the history was recorded at; reject it
			// smoothly rather than with a hard cutoff, to avoid a visible boundary snapping in and out.
			history_confidence = params.history_weight * clamp(1.0 - depth_error * 8.0, 0.0, 1.0);
			result_ao = mix(spatial_ao, history_ao, history_confidence);
		}
	}

	imageStore(dest_history, pos, vec4(result_ao, depth, 0.0, 0.0));
}
