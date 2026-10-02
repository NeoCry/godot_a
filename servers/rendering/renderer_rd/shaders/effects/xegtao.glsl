///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2016-2021, Intel Corporation
// SPDX-License-Identifier: MIT
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of
// the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
// TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// XeGTAO is based on GTAO/GTSO "Jimenez et al. / Practical Real-Time Strategies for Accurate Indirect Occlusion".
// Implementation: Filip Strugar (filip.strugar@intel.com), Steve Mccalla <stephen.mccalla@intel.com>
// Details:        https://github.com/GameTechDev/XeGTAO (version 1.30, with bent normals)
// Ported to Vulkan GLSL and Godot from XeGTAO.h / XeGTAO.hlsli / vaGTAO.hlsl.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// XeGTAO pass 2 (XeGTAO_MainPass): the GTAO horizon search, "Algorithm 1" of Jimenez et al., and with
// USE_BENT_NORMALS also "Algorithm 2", which integrates the bent normal over the same horizons. Writes the
// raw, noisy AO term and the depth edges the denoiser respects.

#[compute]

#version 450

#VERSION_DEFINES

#include "xegtao_inc.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source_depth; // View space depth with MIPs, from the prefilter pass.
layout(rgba8, set = 0, binding = 1) uniform restrict readonly image2D source_normal; // Full resolution normal-roughness buffer.

layout(r32ui, set = 1, binding = 0) uniform restrict writeonly uimage2D dest_ao_term;
layout(r8, set = 1, binding = 1) uniform restrict writeonly image2D dest_edges;

layout(push_constant, std430) uniform Params {
	ivec2 viewport_size;
	vec2 viewport_pixel_size;

	vec2 ndc_to_view_mul;
	vec2 ndc_to_view_add;

	vec2 ndc_to_view_mul_x_pixel_size;
	float effect_radius; // Already multiplied by the radius multiplier.
	float sample_distribution_power;

	float falloff_mul;
	float falloff_add;
	float thin_occluder_compensation;
	float final_value_power;

	float depth_mip_sampling_offset;
	float slice_count;
	float steps_per_slice;
	uint noise_index;

	ivec2 normal_buffer_size;
	int normal_scale; // Full resolution normal texels per working pixel: 1, or 2 at half resolution.
	bool is_orthogonal;
}
params;

// Inputs are screen XY and view space depth, output is view space position.
vec3 compute_viewspace_position(vec2 p_screen_pos, float p_viewspace_depth) {
	vec3 ret;
	ret.xy = params.ndc_to_view_mul * p_screen_pos + params.ndc_to_view_add;
	if (!params.is_orthogonal) {
		ret.xy *= p_viewspace_depth;
	}
	ret.z = p_viewspace_depth;
	return ret;
}

vec4 calculate_edges(float p_center_z, float p_left_z, float p_right_z, float p_top_z, float p_bottom_z) {
	vec4 edges_lrtb = vec4(p_left_z, p_right_z, p_top_z, p_bottom_z) - p_center_z;

	float slope_lr = (edges_lrtb.y - edges_lrtb.x) * 0.5;
	float slope_tb = (edges_lrtb.w - edges_lrtb.z) * 0.5;
	vec4 edges_lrtb_slope_adjusted = edges_lrtb + vec4(slope_lr, -slope_lr, slope_tb, -slope_tb);
	edges_lrtb = min(abs(edges_lrtb), abs(edges_lrtb_slope_adjusted));
	return clamp(1.25 - edges_lrtb / (p_center_z * 0.011), 0.0, 1.0);
}

// 2 bits per edge mean 4 gradient values (0, 0.33, 0.66, 1) for smoother transitions.
float pack_edges(vec4 p_edges_lrtb) {
	p_edges_lrtb = round(clamp(p_edges_lrtb, 0.0, 1.0) * 2.9);
	return dot(p_edges_lrtb, vec4(64.0 / 255.0, 16.0 / 255.0, 4.0 / 255.0, 1.0 / 255.0));
}

// Input [-1, 1] and output [0, PI], from https://seblagarde.wordpress.com/2014/12/01/inverse-trigonometric-functions-gpu-optimization-for-amd-gcn-architecture/
float fast_acos(float p_x) {
	float x = abs(p_x);
	float res = -0.156583 * x + XEGTAO_PI_HALF;
	res *= sqrt(1.0 - x);
	return (p_x >= 0.0) ? res : XEGTAO_PI - res;
}

#ifdef USE_BENT_NORMALS
// "Efficiently building a matrix to rotate one vector to another", Moller & Hughes 1999. Only ever called
// with p_from = (0, 0, -1), the one case XeGTAO vouches for. Returns the rotation applied to p_vector.
vec3 rotate_from_to(vec3 p_from, vec3 p_to, vec3 p_vector) {
	float e = dot(p_from, p_to);
	float f = abs(e);

	if (f > 1.0 - 0.0003) {
		return p_vector;
	}

	vec3 v = cross(p_from, p_to);
	float h = 1.0 / (1.0 + e);
	float hvx = h * v.x;
	float hvz = h * v.z;
	float hvxy = hvx * v.y;
	float hvxz = hvx * v.z;
	float hvyz = hvz * v.y;

	// Rows of the rotation matrix.
	vec3 row0 = vec3(e + hvx * v.x, hvxy - v.z, hvxz + v.y);
	vec3 row1 = vec3(hvxy + v.z, e + h * v.y * v.y, hvyz - v.x);
	vec3 row2 = vec3(hvxz - v.y, hvyz + v.x, e + hvz * v.z);

	return vec3(dot(row0, p_vector), dot(row1, p_vector), dot(row2, p_vector));
}
#endif

// From https://www.shadertoy.com/view/3tB3z3, except XeGTAO uses R2 here.
#define XEGTAO_HILBERT_LEVEL 6u
#define XEGTAO_HILBERT_WIDTH (1u << XEGTAO_HILBERT_LEVEL)

uint hilbert_index(uint p_pos_x, uint p_pos_y) {
	uint index = 0u;
	for (uint cur_level = XEGTAO_HILBERT_WIDTH / 2u; cur_level > 0u; cur_level /= 2u) {
		uint region_x = (p_pos_x & cur_level) > 0u ? 1u : 0u;
		uint region_y = (p_pos_y & cur_level) > 0u ? 1u : 0u;
		index += cur_level * cur_level * ((3u * region_x) ^ region_y);
		if (region_y == 0u) {
			if (region_x == 1u) {
				p_pos_x = (XEGTAO_HILBERT_WIDTH - 1u) - p_pos_x;
				p_pos_y = (XEGTAO_HILBERT_WIDTH - 1u) - p_pos_y;
			}

			uint temp = p_pos_x;
			p_pos_x = p_pos_y;
			p_pos_y = temp;
		}
	}
	return index;
}

// Hilbert curve driving the R2 quasi-random sequence, see http://extremelearning.com.au/unreasonable-effectiveness-of-quasirandom-sequences/
// p_temporal_index is always 0 unless a temporal antialiasing pass is there to resolve the extra noise.
vec2 spatio_temporal_noise(uvec2 p_pix_coord, uint p_temporal_index) {
	uint index = hilbert_index(p_pix_coord.x % XEGTAO_HILBERT_WIDTH, p_pix_coord.y % XEGTAO_HILBERT_WIDTH);
	index += 288u * (p_temporal_index % 64u); // 288 was found empirically to work best with a 6 level Hilbert curve.
	return fract(0.5 + float(index) * vec2(0.75487766624669276005, 0.5698402909980532659114));
}

void main() {
	ivec2 pix_coord = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pix_coord, params.viewport_size))) {
		return;
	}

	vec2 normalized_screen_pos = (vec2(pix_coord) + 0.5) * params.viewport_pixel_size;

	// Gathered around the top-left corner of this pixel: x is the left neighbor, y this pixel, z the top
	// neighbor. Offset by one pixel: x is the bottom neighbor and z the right one.
	vec4 values_ul = textureGather(source_depth, vec2(pix_coord) * params.viewport_pixel_size, 0);
	vec4 values_br = textureGatherOffset(source_depth, vec2(pix_coord) * params.viewport_pixel_size, ivec2(1, 1), 0);

	float viewspace_z = values_ul.y;

	float pix_lz = values_ul.x;
	float pix_tz = values_ul.z;
	float pix_rz = values_br.z;
	float pix_bz = values_br.x;

	vec4 edges_lrtb = calculate_edges(viewspace_z, pix_lz, pix_rz, pix_tz, pix_bz);
	imageStore(dest_edges, pix_coord, vec4(pack_edges(edges_lrtb)));

	ivec2 normal_pos = min(pix_coord * params.normal_scale, params.normal_buffer_size - 1);
	vec3 viewspace_normal = xegtao_load_normal(imageLoad(source_normal, normal_pos));

	// Move center pixel slightly towards camera to avoid imprecision artifacts due to depth buffer imprecision;
	// this offset is the one XeGTAO uses for FP32 depths.
	viewspace_z *= 0.99999;

	vec3 pix_center_pos = compute_viewspace_position(normalized_screen_pos, viewspace_z);
	vec3 view_vec = params.is_orthogonal ? vec3(0.0, 0.0, -1.0) : normalize(-pix_center_pos);

	float effect_radius = params.effect_radius;
	float sample_distribution_power = params.sample_distribution_power;
	float thin_occluder_compensation = params.thin_occluder_compensation;
	float falloff_mul = params.falloff_mul;
	float falloff_add = params.falloff_add;

	float slice_count = params.slice_count;
	float steps_per_slice = params.steps_per_slice;

	float visibility = 0.0;
#ifdef USE_BENT_NORMALS
	vec3 bent_normal = vec3(0.0);
#else
	vec3 bent_normal = viewspace_normal;
#endif

	// See "Algorithm 1" in https://www.activision.com/cdn/research/Practical_Real_Time_Strategies_for_Accurate_Indirect_Occlusion_NEW%20VERSION_COLOR.pdf
	{
		vec2 local_noise = spatio_temporal_noise(uvec2(pix_coord), params.noise_index);
		float noise_slice = local_noise.x;
		float noise_sample = local_noise.y;

		// If the offset is under approx pixel size (pixel_too_close_threshold), push it out to the minimum distance.
		const float pixel_too_close_threshold = 1.3;

		// Approx view space pixel size at pix_coord.
		vec2 pixel_dir_rb_viewspace_size_at_center_z = params.ndc_to_view_mul_x_pixel_size;
		if (!params.is_orthogonal) {
			pixel_dir_rb_viewspace_size_at_center_z *= viewspace_z;
		}

		float screenspace_radius = effect_radius / pixel_dir_rb_viewspace_size_at_center_z.x;

		// Fade out for small screen radii.
		visibility += clamp((10.0 - screenspace_radius) / 100.0, 0.0, 1.0) * 0.5;

		// This is the min distance to start sampling from to avoid sampling from the center pixel (no useful data obtained from sampling center pixel).
		float min_s = pixel_too_close_threshold / screenspace_radius;

		for (float slice_index = 0.0; slice_index < slice_count; slice_index++) {
			float slice_k = (slice_index + noise_slice) / slice_count;
			// Lines 5, 6 from the paper.
			float phi = slice_k * XEGTAO_PI;
			float cos_phi = cos(phi);
			float sin_phi = sin(phi);
			vec2 omega = vec2(cos_phi, -sin_phi);

			// Convert to screen units (pixels) for later use.
			omega *= screenspace_radius;

			// Line 8 from the paper.
			vec3 direction_vec = vec3(cos_phi, sin_phi, 0.0);

			// Line 9 from the paper.
			vec3 ortho_direction_vec = direction_vec - (dot(direction_vec, view_vec) * view_vec);

			// Line 10 from the paper.
			// axis_vec is orthogonal to direction_vec and view_vec, used to define projected_normal_vec.
			vec3 axis_vec = normalize(cross(ortho_direction_vec, view_vec));

			// Line 11 from the paper.
			vec3 projected_normal_vec = viewspace_normal - axis_vec * dot(viewspace_normal, axis_vec);

			// Line 13 from the paper.
			float sign_norm = sign(dot(ortho_direction_vec, projected_normal_vec));

			// Line 14 from the paper.
			float projected_normal_vec_length = length(projected_normal_vec);
			float cos_norm = clamp(dot(projected_normal_vec, view_vec) / max(projected_normal_vec_length, 1e-6), 0.0, 1.0);

			// Line 15 from the paper.
			float n = sign_norm * fast_acos(cos_norm);

			// This is a lower weight target; not using -1 as in the original paper because it is under horizon,
			// so a 'weight' has different meaning based on the normal.
			float low_horizon_cos0 = cos(n + XEGTAO_PI_HALF);
			float low_horizon_cos1 = cos(n - XEGTAO_PI_HALF);

			// Lines 17, 18 from the paper, manually unrolled the 'side' loop.
			float horizon_cos0 = low_horizon_cos0;
			float horizon_cos1 = low_horizon_cos1;

			for (float step_index = 0.0; step_index < steps_per_slice; step_index++) {
				// R1 sequence (http://extremelearning.com.au/unreasonable-effectiveness-of-quasirandom-sequences/).
				float step_base_noise = float(slice_index + step_index * steps_per_slice) * 0.6180339887498948482;
				float step_noise = fract(noise_sample + step_base_noise);

				// Approx line 20 from the paper, with added noise.
				float s = (step_index + step_noise) / steps_per_slice;

				// Additional distribution modifier.
				s = pow(s, sample_distribution_power);

				// Avoid sampling center pixel.
				s += min_s;

				// Approx lines 21-22 from the paper, unrolled.
				vec2 sample_offset = s * omega;

				float sample_offset_length = length(sample_offset);

				// Sampling has to use a point (or point_point_linear) sampler: linear filtering would interpolate
				// between neighboring depth values on the same MIP level.
				float mip_level = clamp(log2(sample_offset_length) - params.depth_mip_sampling_offset, 0.0, float(XEGTAO_DEPTH_MIP_LEVELS));

				// Snap to pixel center (more correct direction math, avoids artifacts due to sampling pos not matching
				// depth texel center - messes up slope - but adds other artifacts due to them being pushed off the slice).
				sample_offset = round(sample_offset) * params.viewport_pixel_size;

				vec2 sample_screen_pos0 = normalized_screen_pos + sample_offset;
				float sz0 = textureLod(source_depth, sample_screen_pos0, mip_level).x;
				vec3 sample_pos0 = compute_viewspace_position(sample_screen_pos0, sz0);

				vec2 sample_screen_pos1 = normalized_screen_pos - sample_offset;
				float sz1 = textureLod(source_depth, sample_screen_pos1, mip_level).x;
				vec3 sample_pos1 = compute_viewspace_position(sample_screen_pos1, sz1);

				vec3 sample_delta0 = sample_pos0 - pix_center_pos;
				vec3 sample_delta1 = sample_pos1 - pix_center_pos;
				float sample_dist0 = length(sample_delta0);
				float sample_dist1 = length(sample_delta1);

				// Approx lines 23, 24 from the paper, unrolled.
				vec3 sample_horizon_vec0 = sample_delta0 / sample_dist0;
				vec3 sample_horizon_vec1 = sample_delta1 / sample_dist1;

				// Any sample out of radius should be discarded - also use falloff range for smooth transitions; this is a
				// modified idea from "4.3 Implementation details, Bounding the sampling area". XeGTAO's own thickness
				// heuristic discards samples behind the center sooner.
				float falloff_base0 = length(vec3(sample_delta0.x, sample_delta0.y, sample_delta0.z * (1.0 + thin_occluder_compensation)));
				float falloff_base1 = length(vec3(sample_delta1.x, sample_delta1.y, sample_delta1.z * (1.0 + thin_occluder_compensation)));
				float weight0 = clamp(falloff_base0 * falloff_mul + falloff_add, 0.0, 1.0);
				float weight1 = clamp(falloff_base1 * falloff_mul + falloff_add, 0.0, 1.0);

				// Sample horizon cos.
				float shc0 = dot(sample_horizon_vec0, view_vec);
				float shc1 = dot(sample_horizon_vec1, view_vec);

				// Discard unwanted samples.
				shc0 = mix(low_horizon_cos0, shc0, weight0);
				shc1 = mix(low_horizon_cos1, shc1, weight1);

				horizon_cos0 = max(horizon_cos0, shc0);
				horizon_cos1 = max(horizon_cos1, shc1);
			}

			// XeGTAO's fudge for a slight overdarkening on high slopes (in its training set, 0.05 is close to disabled).
			projected_normal_vec_length = mix(projected_normal_vec_length, 1.0, 0.05);

			// Line ~27, unrolled.
			float h0 = -fast_acos(horizon_cos1);
			float h1 = fast_acos(horizon_cos0);

			float iarc0 = (cos_norm + 2.0 * h0 * sin(n) - cos(2.0 * h0 - n)) / 4.0;
			float iarc1 = (cos_norm + 2.0 * h1 * sin(n) - cos(2.0 * h1 - n)) / 4.0;
			float local_visibility = projected_normal_vec_length * (iarc0 + iarc1);
			visibility += local_visibility;

#ifdef USE_BENT_NORMALS
			// See "Algorithm 2 Extension that computes bent normals b."
			float t0 = (6.0 * sin(h0 - n) - sin(3.0 * h0 - n) + 6.0 * sin(h1 - n) - sin(3.0 * h1 - n) + 16.0 * sin(n) - 3.0 * (sin(h0 + n) + sin(h1 + n))) / 12.0;
			float t1 = (-cos(3.0 * h0 - n) - cos(3.0 * h1 - n) + 8.0 * cos(n) - 3.0 * (cos(h0 + n) + cos(h1 + n))) / 12.0;
			vec3 local_bent_normal = vec3(direction_vec.x * t0, direction_vec.y * t0, -t1);
			local_bent_normal = rotate_from_to(vec3(0.0, 0.0, -1.0), view_vec, local_bent_normal) * projected_normal_vec_length;
			bent_normal += local_bent_normal;
#endif
		}

		visibility /= slice_count;
		visibility = pow(visibility, params.final_value_power);
		// Disallow total occlusion (which wouldn't make any sense anyhow since pixel is visible, but also helps with packing bent normals).
		visibility = max(0.03, visibility);

#ifdef USE_BENT_NORMALS
		bent_normal = normalize(bent_normal);
#endif
	}

	visibility = clamp(visibility / XEGTAO_OCCLUSION_TERM_SCALE, 0.0, 1.0);
	imageStore(dest_ao_term, pix_coord, uvec4(xegtao_encode_term(visibility, bent_normal - viewspace_normal)));
}
