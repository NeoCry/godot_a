/**************************************************************************/
/*  gtao.glsl                                                             */
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

// Ground-Truth Ambient Occlusion (GTAO) horizon search, following Jimenez, Wu, Pesce & Jarabo, "Practical
// Realtime Strategies for Accurate Indirect Occlusion" (Activision, 2016). For each (half-resolution) pixel,
// a few slices (directions) around the view vector search for the maximum horizon angle on both sides; the
// per-slice visibility integral is then solved analytically once the shading normal is projected into the
// slice plane (eq. 7-8 of the paper), rather than accumulated as a weighted sample sum. This pass produces a
// single noisy-but-unbiased visibility estimate per pixel; gtao_temporal.glsl denoises it spatially and
// temporally afterwards, which is what the paper itself relies on for its final quality ("we distribute the
// occlusion integral over both space and time").

#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source_depth_mipmaps;
layout(rgba8, set = 0, binding = 1) uniform restrict readonly image2D source_normal;

layout(r16f, set = 1, binding = 0) uniform restrict writeonly image2D dest_image;

layout(push_constant, std430) uniform Params {
	ivec2 screen_size; // half-resolution size of this pass
	float NDC_to_view_mul_x;
	float NDC_to_view_mul_y;

	float NDC_to_view_add_x;
	float NDC_to_view_add_y;
	bool is_orthogonal;
	int quality;

	float radius;
	float horizon_bias;
	uint frame_index;
	uint mip_count;

	ivec2 full_screen_size; // native resolution of source_normal, independent of screen_size above
	vec2 depth_texture_pixel_size; // 1 / half-resolution size, in the *depth* texture's own space
	float thin_occluder_compensation;
	float pad;
}
params;

#define GTAO_PI 3.14159265359
#define GOLDEN_RATIO 0.61803398875

// Number of horizon-search slices (directions) and steps searched per side of each slice, indexed by the
// GTAO quality preset (Very Low .. Ultra). Each step is sampled on both sides of the slice, so the actual
// per-pixel tap count is 2x this; spatial+temporal denoising in gtao_temporal.glsl is what lets these counts
// stay low without the result looking undersampled.
const int gtao_slice_count[5] = { 1, 2, 2, 3, 3 };
const int gtao_step_count[5] = { 3, 3, 4, 4, 6 };

vec3 NDC_to_view_space(vec2 p_pos, float p_viewspace_depth) {
	vec2 mul = vec2(params.NDC_to_view_mul_x, params.NDC_to_view_mul_y);
	vec2 add = vec2(params.NDC_to_view_add_x, params.NDC_to_view_add_y);
	if (params.is_orthogonal) {
		return vec3(mul * p_pos + add, p_viewspace_depth);
	} else {
		return vec3((mul * p_pos + add) * p_viewspace_depth, p_viewspace_depth);
	}
}

vec3 load_normal(ivec2 p_full_res_pos) {
	vec3 n = normalize(imageLoad(source_normal, p_full_res_pos).xyz * 2.0 - 1.0);
	n.z = -n.z;
	return n;
}

// Jimenez et al. 2014, "Next Generation Post Processing in Call of Duty: Advanced Warfare".
float interleaved_gradient_noise(vec2 p_pixel) {
	return fract(52.9829189 * fract(dot(p_pixel, vec2(0.06711056, 0.00583715))));
}

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.screen_size))) {
		return;
	}

	vec2 uv = (vec2(pos) + 0.5) * params.depth_texture_pixel_size;
	float depth = textureLod(source_depth_mipmaps, uv, 0.0).x;
	vec3 view_pos = NDC_to_view_space(uv, depth);

	// source_normal is always at native (full) resolution, independent of whether this gather itself runs at
	// half resolution (screen_size == full_screen_size / 2) or full resolution (screen_size ==
	// full_screen_size, half_size disabled) — derive the matching texel from the resolution-independent uv
	// rather than assuming a fixed 2x ratio.
	ivec2 full_res_pos = clamp(ivec2(uv * vec2(params.full_screen_size)), ivec2(0), params.full_screen_size - ivec2(1));
	vec3 N = load_normal(full_res_pos);

	vec2 pixel_size_at_center = NDC_to_view_space(uv + params.depth_texture_pixel_size, view_pos.z).xy - view_pos.xy;
	float pixel_radius = params.radius / max(pixel_size_at_center.x, 0.00001);

	vec3 V = params.is_orthogonal ? vec3(0.0, 0.0, 1.0) : normalize(-view_pos);

	int quality = clamp(params.quality, 0, 4);
	int slice_count = gtao_slice_count[quality];
	int step_count = gtao_step_count[quality];

	// Per-pixel spatial jitter, plus a per-frame rotation cycling through 6 offsets (golden-ratio spaced),
	// matching the paper's "alternating between 6 different rotations" — gtao_temporal.glsl accumulates the
	// results of consecutive frames, so different frames must search different slice angles for the
	// temporal history to actually add new information instead of repeating the same noise.
	float jitter = fract(interleaved_gradient_noise(vec2(full_res_pos)) + GOLDEN_RATIO * float(params.frame_index % 6u));

	float visibility_sum = 0.0;
	int used_slices = 0;

	for (int slice = 0; slice < slice_count; slice++) {
		float phi = (GTAO_PI / float(slice_count)) * (float(slice) + jitter);
		vec2 dir_screen = vec2(cos(phi), sin(phi));

		vec3 slice_tangent = vec3(dir_screen, 0.0);
		vec3 slice_normal = normalize(cross(slice_tangent, V));
		vec3 normal_in_slice = N - slice_normal * dot(N, slice_normal);
		float normal_in_slice_len = length(normal_in_slice);

		if (normal_in_slice_len < 0.001) {
			continue;
		}

		vec3 normal_in_slice_n = normal_in_slice / normal_in_slice_len;
		vec3 slice_tangent_perp = normalize(slice_tangent - V * dot(slice_tangent, V));
		float gamma = atan(dot(normal_in_slice_n, slice_tangent_perp), dot(normal_in_slice_n, V));

		float horizon_cos[2] = float[2](0.0, 0.0);

		for (int side = 0; side < 2; side++) {
			float side_sign = (side == 0) ? 1.0 : -1.0;

			for (int s = 0; s < step_count; s++) {
				float t = (float(s) + 0.5) / float(step_count);
				float step_dist = t * t * pixel_radius;

				vec2 sample_uv = uv + (dir_screen * side_sign * step_dist) * params.depth_texture_pixel_size;
				// mip N covers 2^N base-level texels, so a sample step_dist texels away is reasonably
				// represented by that same mip level.
				float sample_mip = clamp(floor(log2(max(step_dist, 1.0))), 0.0, float(params.mip_count - 1));

				float sample_z = textureLod(source_depth_mipmaps, sample_uv, sample_mip).x;
				vec3 sample_pos = NDC_to_view_space(sample_uv, sample_z);

				vec3 sample_delta = sample_pos - view_pos;
				float sample_dist = length(sample_delta);
				float sample_cos = dot(sample_delta, V) / max(sample_dist, 0.0001) - params.horizon_bias;

				// Fade out smoothly at the search radius, and soften thin occluders (a conservative stand-in
				// for the paper's thickness heuristic, Section 4.1, eq. 9): a sample that would lower the
				// horizon is only partially accepted, so a thin occluder doesn't fully re-open the horizon
				// right behind it the way an infinitely thick one would.
				float falloff = clamp(1.0 - (sample_dist * sample_dist) / (params.radius * params.radius), 0.0, 1.0);
				sample_cos = mix(horizon_cos[side], sample_cos, falloff);
				horizon_cos[side] = (sample_cos >= horizon_cos[side]) ? sample_cos : mix(horizon_cos[side], sample_cos, params.thin_occluder_compensation);
			}
		}

		float theta0 = acos(clamp(horizon_cos[0], -1.0, 1.0));
		float theta1 = acos(clamp(horizon_cos[1], -1.0, 1.0));

		// Clamp each horizon angle to the surface's own tangent plane (gamma +/- pi/2): a raw acos result
		// beyond that would count contributions from behind the surface, which the search has no way to
		// exclude on its own. Without this, a perfectly flat, unoccluded surface only integrates to full
		// visibility when V happens to be near-parallel to N (gamma near 0, where the tangent-plane bound
		// coincides with the search's own default range); as gamma grows toward grazing angles the two
		// diverge and the unclamped formula reports spurious self-occlusion that gets worse the more
		// glancing the view angle is — exactly the reported symptom.
		theta0 = min(theta0, GTAO_PI * 0.5 + gamma);
		theta1 = min(theta1, GTAO_PI * 0.5 - gamma);

		float a0 = -cos(2.0 * theta0 - gamma) + cos(gamma) + 2.0 * theta0 * sin(gamma);
		float a1 = -cos(2.0 * theta1 - gamma) + cos(gamma) + 2.0 * theta1 * sin(gamma);

		visibility_sum += normal_in_slice_len * 0.25 * (a0 + a1);
		used_slices++;
	}

	float visibility = (used_slices > 0) ? clamp(visibility_sum / float(used_slices), 0.0, 1.0) : 1.0;

	imageStore(dest_image, pos, vec4(visibility));
}
