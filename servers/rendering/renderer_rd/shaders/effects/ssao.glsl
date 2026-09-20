///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Copyright (c) 2016, Intel Corporation
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
// File changes (yyyy-mm-dd)
// 2016-09-07: filip.strugar@intel.com: first commit
// 2020-12-05: clayjohn: convert to Vulkan and Godot
// 2026-09-20: Replaced the Intel ASSAO sampling core with Ground-Truth Ambient Occlusion (GTAO): Jimenez, Wu,
//             Pesce, Jarabo, "Practical Realtime Strategies for Accurate Indirect Occlusion", Activision, 2016.
//             The screen-space setup, depth-mip marching and edge-detection helpers below are still derived
//             from the Intel implementation above; the horizon search and analytic integral are new.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2DArray source_depth_mipmaps;
layout(rgba8, set = 0, binding = 1) uniform restrict readonly image2D source_normal;

layout(rg8, set = 1, binding = 0) uniform restrict writeonly image2D dest_image;

// This push_constant is full - 128 bytes - if you need to add more data, consider adding to the uniform buffer instead
layout(push_constant, std430) uniform Params {
	ivec2 screen_size;
	int pass;
	int quality;

	vec2 half_screen_pixel_size;
	int size_multiplier;
	float detail_intensity;

	vec2 NDC_to_view_mul;
	vec2 NDC_to_view_add;

	vec2 pad;
	vec2 half_screen_pixel_size_x025;

	float radius;
	float intensity;
	float shadow_power;
	float shadow_clamp;

	float fade_out_mul;
	float fade_out_add;
	float horizon_bias;
	float inv_radius_near_limit;

	bool is_orthogonal;
	float pad2;
	float pad3;
	float pad4;

	ivec2 pass_coord_offset;
	vec2 pass_uv_offset;
}
params;

#define GTAO_PI 3.14159265359

// Number of horizon-search slices (directions around the pixel) and steps searched per side of each slice,
// indexed by the SSAO quality preset (Very Low .. Ultra). Each step is sampled on both sides of the slice, so
// the actual per-pixel tap count is 2x this. Spatial diversity beyond this comes from the deinterleaved 2x2
// passes (each gets a different jitter, see `interleaved_gradient_noise` below) which the edge-aware blur and
// interleave passes recombine, approximating many more directions than any single pixel searches on its own.
const int gtao_slice_count[5] = { 1, 2, 2, 3, 4 };
const int gtao_step_count[5] = { 3, 3, 4, 5, 7 };

// Depth mips are only generated (by the SS effects downsampler) once either SSAO or SSIL quality goes above
// Medium; must match `SSEffects::downsample_depth()`'s `use_mips` condition or we'd sample garbage/stale mips.
#define SSAO_DEPTH_MIPS_ENABLE_AT_QUALITY 3
#define SSAO_DEPTH_MIPS_GLOBAL_OFFSET (-4.3) // best noise/quality/performance tradeoff, found empirically
#define SSAO_DEPTH_MIP_LEVELS 4

vec3 NDC_to_view_space(vec2 p_pos, float p_viewspace_depth) {
	if (params.is_orthogonal) {
		return vec3((params.NDC_to_view_mul * p_pos.xy + params.NDC_to_view_add), p_viewspace_depth);
	} else {
		return vec3((params.NDC_to_view_mul * p_pos.xy + params.NDC_to_view_add) * p_viewspace_depth, p_viewspace_depth);
	}
}

// calculate effect radius and fit our screen sampling pattern inside it
void calculate_radius_parameters(const float p_pix_center_length, const vec2 p_pixel_size_at_center, out float r_lookup_radius, out float r_radius) {
	r_radius = params.radius;

	// when too close, on-screen sampling disk will grow beyond screen size; limit this to avoid closeup temporal artifacts
	const float too_close_limit = clamp(p_pix_center_length * params.inv_radius_near_limit, 0.0, 1.0) * 0.8 + 0.2;

	r_radius *= too_close_limit;

	// radius, expressed in half-resolution pixels, that the horizon search steps are distributed across
	r_lookup_radius = r_radius / p_pixel_size_at_center.x;
}

vec4 calculate_edges(const float p_center_z, const float p_left_z, const float p_right_z, const float p_top_z, const float p_bottom_z) {
	// slope-sensitive depth-based edge detection
	vec4 edgesLRTB = vec4(p_left_z, p_right_z, p_top_z, p_bottom_z) - p_center_z;
	vec4 edgesLRTB_slope_adjusted = edgesLRTB + edgesLRTB.yxwz;
	edgesLRTB = min(abs(edgesLRTB), abs(edgesLRTB_slope_adjusted));
	return clamp((1.3 - edgesLRTB / (p_center_z * 0.040)), 0.0, 1.0);
}

// packing/unpacking for edges; 2 bits per edge mean 4 gradient values (0, 0.33, 0.66, 1) for smoother transitions!
float pack_edges(vec4 p_edgesLRTB) {
	p_edgesLRTB = round(clamp(p_edgesLRTB, 0.0, 1.0) * 3.05);
	return dot(p_edgesLRTB, vec4(64.0 / 255.0, 16.0 / 255.0, 4.0 / 255.0, 1.0 / 255.0));
}

vec3 load_normal(ivec2 p_pos) {
	vec3 encoded_normal = normalize(imageLoad(source_normal, p_pos).xyz * 2.0 - 1.0);
	encoded_normal.z = -encoded_normal.z;
	return encoded_normal;
}

// Jimenez et al. 2014, "Next Generation Post Processing in Call of Duty: Advanced Warfare"; used here to jitter
// each pixel's slice angles so neighboring pixels (and the four deinterleaved passes) search different
// directions for the spatial blur to recombine.
float interleaved_gradient_noise(vec2 p_pixel) {
	return fract(52.9829189 * fract(dot(p_pixel, vec2(0.06711056, 0.00583715))));
}

// Computes ground-truth ambient occlusion visibility for one (half-resolution, deinterleaved) pixel, following
// Jimenez et al. 2016, "Practical Realtime Strategies for Accurate Indirect Occlusion":
// - horizons are searched with the view vector (not the normal) as the sphere's pole (eq. 3, Section 4);
// - the visibility term is binary (a hard horizon), not an attenuated "obscurance" (Section 4, eq. 5-6);
// - the resulting per-slice integral is solved analytically once the shading normal is projected into the
//   slice plane (eq. 7-8), instead of being accumulated as a weighted sample sum.
void generate_gtao_visibility(out float r_visibility, out vec4 r_edges, const vec2 p_pos, int p_quality_level) {
	vec2 pos_rounded = trunc(p_pos);
	uvec2 upos = uvec2(pos_rounded);

	float pix_z, pix_left_z, pix_top_z, pix_right_z, pix_bottom_z;

	vec4 valuesUL = textureGather(source_depth_mipmaps, vec3(pos_rounded * params.half_screen_pixel_size, params.pass));
	vec4 valuesBR = textureGather(source_depth_mipmaps, vec3((pos_rounded + vec2(1.0)) * params.half_screen_pixel_size, params.pass));

	// get this pixel's viewspace depth
	pix_z = valuesUL.y;

	// get left right top bottom neighboring pixels for edge detection and the close-range detail term below
	pix_left_z = valuesUL.x;
	pix_top_z = valuesUL.z;
	pix_right_z = valuesBR.z;
	pix_bottom_z = valuesBR.x;

	vec2 normalized_screen_pos = pos_rounded * params.half_screen_pixel_size + params.half_screen_pixel_size_x025;
	vec3 pix_center_pos = NDC_to_view_space(normalized_screen_pos, pix_z);

	// Load this pixel's viewspace normal
	uvec2 full_res_coord = upos * 2 * params.size_multiplier + params.pass_coord_offset.xy;
	vec3 pixel_normal = load_normal(ivec2(full_res_coord));

	const vec2 pixel_size_at_center = NDC_to_view_space(normalized_screen_pos.xy + params.half_screen_pixel_size, pix_center_pos.z).xy - pix_center_pos.xy;

	float pixel_lookup_radius;
	float viewspace_radius;
	calculate_radius_parameters(length(pix_center_pos), pixel_size_at_center, pixel_lookup_radius, viewspace_radius);

	vec4 edgesLRTB = calculate_edges(pix_z, pix_left_z, pix_right_z, pix_top_z, pix_bottom_z);

	// reduce effect radius near the screen edges slightly; ideally, one would render a larger depth buffer (5% on each side) instead
	{
		float near_screen_border = min(min(normalized_screen_pos.x, 1.0 - normalized_screen_pos.x), min(normalized_screen_pos.y, 1.0 - normalized_screen_pos.y));
		near_screen_border = clamp(10.0 * near_screen_border + 0.6, 0.0, 1.0);
		pixel_lookup_radius *= near_screen_border;
	}

	// Move center pixel slightly towards camera to avoid imprecision artifacts due to using of 16bit depth buffer.
	pix_center_pos *= 0.99;

	// Direction from the shading point towards the eye; for orthogonal projections all view rays are parallel,
	// so it's a constant axis instead of pointing back at a (non-existent) eye position.
	vec3 view_vec = params.is_orthogonal ? vec3(0.0, 0.0, 1.0) : normalize(-pix_center_pos);

	int slice_count = gtao_slice_count[p_quality_level];
	int step_count = gtao_step_count[p_quality_level];

	float jitter = interleaved_gradient_noise(vec2(full_res_coord));

	bool use_mips = p_quality_level >= SSAO_DEPTH_MIPS_ENABLE_AT_QUALITY;
	float mip_offset = use_mips ? (log2(max(pixel_lookup_radius, 1.0)) + SSAO_DEPTH_MIPS_GLOBAL_OFFSET) : 0.0;

	float visibility_sum = 0.0;
	int used_slices = 0;

	for (int slice = 0; slice < slice_count; slice++) {
		float phi = (GTAO_PI / float(slice_count)) * (float(slice) + jitter);
		vec2 dir_screen = vec2(cos(phi), sin(phi));

		// The slice plane is spanned by the view vector and this slice's screen-space direction (approximated
		// as a view-space direction directly, which is what makes this "practical": Section 4.1). The shading
		// normal must be projected into that plane for the analytic integral below to be valid (eq. 8).
		vec3 slice_tangent = vec3(dir_screen, 0.0);
		vec3 slice_normal = normalize(cross(slice_tangent, view_vec));
		vec3 normal_in_slice = pixel_normal - slice_normal * dot(pixel_normal, slice_normal);
		float normal_in_slice_len = length(normal_in_slice);

		if (normal_in_slice_len < 0.001) {
			// Normal is (almost) perpendicular to this slice plane; its contribution is ~0 anyway (eq. 8's
			// ||n_x^P|| weight), so skip the horizon search for it entirely.
			continue;
		}

		vec3 normal_in_slice_n = normal_in_slice / normal_in_slice_len;
		vec3 slice_tangent_perp = normalize(slice_tangent - view_vec * dot(slice_tangent, view_vec));
		// Signed angle (gamma) of the projected normal from the view vector, using the slice tangent as the
		// positive-side reference so the two horizon angles below combine with the correct orientation.
		float gamma = atan(dot(normal_in_slice_n, slice_tangent_perp), dot(normal_in_slice_n, view_vec));

		// Horizon cosines for the two sides of the slice (+dir_screen and -dir_screen); 0 is a fully open
		// horizon (theta == pi/2, the "+" clamp in eq. 6), the baseline for an unoccluded surface.
		float horizon_cos[2] = float[2](0.0, 0.0);

		for (int side = 0; side < 2; side++) {
			float side_sign = (side == 0) ? 1.0 : -1.0;

			for (int s = 0; s < step_count; s++) {
				// Bias steps towards the pixel (quadratic distribution) since nearby occluders matter most.
				float t = (float(s) + 0.5) / float(step_count);
				float step_dist = t * t * pixel_lookup_radius;

				vec2 sample_uv = normalized_screen_pos + (dir_screen * side_sign * step_dist) * params.half_screen_pixel_size;
				float sample_mip = use_mips ? clamp(floor(log2(max(step_dist, 1.0))) + mip_offset, 0.0, float(SSAO_DEPTH_MIP_LEVELS - 1)) : 0.0;

				float sample_z = textureLod(source_depth_mipmaps, vec3(sample_uv, params.pass), sample_mip).x;
				vec3 sample_pos = NDC_to_view_space(sample_uv, sample_z);

				vec3 sample_delta = sample_pos - pix_center_pos;
				float sample_dist = length(sample_delta);
				// horizon_bias is a small threshold every raw sample must clear before it can register as an
				// occluder, which suppresses self-occlusion noise from depth-buffer/normal-map precision on
				// near-flat surfaces (bias == 1.0 puts that threshold out of reach, so nothing ever occludes).
				float sample_cos = dot(sample_delta, view_vec) / max(sample_dist, 0.0001) - params.horizon_bias;

				// Fade the sample's influence out smoothly as it nears/exceeds the search radius, so there's no
				// hard cutoff at the radius boundary, and soften thin occluders (a conservative stand-in for
				// the paper's thickness heuristic, Section 4.1, eq. 9): a sample that would lower the horizon
				// back down is only partially accepted, so a thin occluder (a leaf, a wire) doesn't fully
				// re-open the horizon right behind it the way an infinitely thick occluder would.
				float falloff = clamp(1.0 - (sample_dist * sample_dist) / (viewspace_radius * viewspace_radius), 0.0, 1.0);
				sample_cos = mix(horizon_cos[side], sample_cos, falloff);
				horizon_cos[side] = (sample_cos >= horizon_cos[side]) ? sample_cos : mix(horizon_cos[side], sample_cos, 0.15);
			}
		}

		float theta0 = acos(clamp(horizon_cos[0], -1.0, 1.0));
		float theta1 = acos(clamp(horizon_cos[1], -1.0, 1.0));

		// Analytic solution of the inner (per-slice) integral, eq. 7.
		float a0 = -cos(2.0 * theta0 - gamma) + cos(gamma) + 2.0 * theta0 * sin(gamma);
		float a1 = -cos(2.0 * theta1 - gamma) + cos(gamma) + 2.0 * theta1 * sin(gamma);

		visibility_sum += normal_in_slice_len * 0.25 * (a0 + a1);
		used_slices++;
	}

	// Outer (Monte Carlo, stratified by slice) integral over phi, eq. 8.
	float visibility = (used_slices > 0) ? clamp(visibility_sum / float(used_slices), 0.0, 1.0) : 1.0;
	float obscurance = 1.0 - visibility;

	// Additional close-range contact occlusion from the immediately neighboring texels (reuses the same
	// textureGather fetched above for edge detection, so it's free); mirrors what the old detail pass did,
	// mainly helping thin contact creases the coarser slice search can step over.
	{
		vec3 normalized_viewspace_dir = vec3(pix_center_pos.xy / pix_center_pos.zz, 1.0);
		vec3 pixel_left_delta = vec3(-pixel_size_at_center.x, 0.0, 0.0) + normalized_viewspace_dir * (pix_left_z - pix_center_pos.z);
		vec3 pixel_right_delta = vec3(+pixel_size_at_center.x, 0.0, 0.0) + normalized_viewspace_dir * (pix_right_z - pix_center_pos.z);
		vec3 pixel_top_delta = vec3(0.0, -pixel_size_at_center.y, 0.0) + normalized_viewspace_dir * (pix_top_z - pix_center_pos.z);
		vec3 pixel_bottom_delta = vec3(0.0, +pixel_size_at_center.y, 0.0) + normalized_viewspace_dir * (pix_bottom_z - pix_center_pos.z);

		vec4 detail_obscurance;
		detail_obscurance.x = max(0.0, dot(pixel_normal, normalize(pixel_left_delta)));
		detail_obscurance.y = max(0.0, dot(pixel_normal, normalize(pixel_right_delta)));
		detail_obscurance.z = max(0.0, dot(pixel_normal, normalize(pixel_top_delta)));
		detail_obscurance.w = max(0.0, dot(pixel_normal, normalize(pixel_bottom_delta)));

		obscurance += params.detail_intensity * dot(detail_obscurance, edgesLRTB);
	}

	// calculate fadeout (1 close, gradient, 0 far)
	float fade_out = clamp(pix_center_pos.z * params.fade_out_mul + params.fade_out_add, 0.0, 1.0);

	// Reduce the SSAO shadowing if we're on the edge to remove artifacts on edges
	{
		// when there's more than 2 opposite edges, start fading out the occlusion to reduce aliasing artifacts
		float edge_fadeout_factor = clamp((1.0 - edgesLRTB.x - edgesLRTB.y) * 0.35, 0.0, 1.0) + clamp((1.0 - edgesLRTB.z - edgesLRTB.w) * 0.35, 0.0, 1.0);

		fade_out *= clamp(1.0 - edge_fadeout_factor, 0.0, 1.0);
	}

	// strength
	obscurance = params.intensity * obscurance;

	// clamp
	obscurance = min(obscurance, params.shadow_clamp);

	// fadeout
	obscurance *= fade_out;

	// conceptually switch to occlusion with the meaning being visibility (grows with visibility, occlusion == 1 implies full visibility),
	// to be in line with what is more commonly used.
	float occlusion = 1.0 - obscurance;

	// modify the gradient
	// note: this cannot be moved to a later pass because of loss of precision after storing in the render target
	occlusion = pow(clamp(occlusion, 0.0, 1.0), params.shadow_power);

	// outputs!
	r_visibility = occlusion; // Our final 'occlusion' term (0 means fully occluded, 1 means fully lit)
	r_edges = edgesLRTB; // These are used to prevent blurring across edges, 1 means no edge, 0 means edge, 0.5 means half way there, etc.
}

void main() {
	float out_visibility;
	vec4 out_edges;
	ivec2 ssC = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(ssC, params.screen_size))) { //too large, do nothing
		return;
	}

	vec2 uv = vec2(gl_GlobalInvocationID) + vec2(0.5);
	generate_gtao_visibility(out_visibility, out_edges, uv, params.quality);

	imageStore(dest_image, ivec2(gl_GlobalInvocationID.xy), vec4(out_visibility, pack_edges(out_edges), 0.0, 0.0));
}
