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

// Ground-Truth Ambient Occlusion (GTAO) with a visibility bitmask, following Jimenez, Wu, Pesce & Jarabo,
// "Practical Realtime Strategies for Accurate Indirect Occlusion" (Activision, 2016) for the cosine-weighted
// per-slice visibility integral (eq. 7-8), and Therrien, Levesque & Gilet, "Screen Space Indirect Lighting
// with Visibility Bitmask" (2023) for how occlusion is tracked along each slice.
//
// For each (half-resolution) pixel a few slices (planes containing the view vector) are swept. Plain GTAO
// keeps one horizon angle per side of a slice, which assumes every occluder extends back to infinity: a thin
// railing then shadows the entire wall behind it, and the ad-hoc "thin occluder" softening that hides this
// costs accuracy everywhere else. Instead each slice here carries a 32-bit mask over its angular domain, and
// every sample marks only the wedge it actually spans -- from its own depth to that depth plus an assumed
// thickness. Occlusion becomes a union of bounded wedges rather than a single horizon, so several separate
// occluders along one slice are all accounted for and open space behind a thin one stays open.
//
// The visibility integral stays exactly the paper's: each sector the mask left clear contributes its
// cosine-weighted arc, so an unoccluded slice still integrates to the same closed form GTAO solves for
// analytically (see slice_visibility()). That is deliberate — the reference implementation of the bitmask
// paper reduces AO to popcount/sector_count, which drops the cosine weighting the ground-truth estimator
// needs; keeping the arc integral makes the two papers compose instead of one overwriting the other.
//
// With USE_INDIRECT_LIGHT the same traversal also produces screen-space indirect light, which is the other
// half of the bitmask paper and the reason it carries a mask rather than a horizon: the moment a sample claims
// sectors nothing had claimed yet, the arc of exactly those sectors is the solid angle under which this pixel
// sees that occluder, so the light leaving it can be accumulated with the correct weight. Occlusion and
// bounce therefore come out of one sweep and cannot disagree, whereas Godot's SSAO and SSIL ran two
// independent searches over the same depth buffer.
//
// This pass produces a single noisy-but-unbiased estimate per pixel; gtao_temporal.glsl denoises it spatially
// and temporally afterwards, which is what GTAO itself relies on for its final quality ("we distribute the
// occlusion integral over both space and time").

#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source_depth_mipmaps;
layout(rgba8, set = 0, binding = 1) uniform restrict readonly image2D source_normal;

layout(r16f, set = 1, binding = 0) uniform restrict writeonly image2D dest_image;

#ifdef USE_INDIRECT_LIGHT
// Last frame's lit colour, mipmapped. This pass runs before opaque shading, so the current frame's lighting
// does not exist yet; the occluders' radiance can only come from the frame before.
layout(set = 0, binding = 2) uniform sampler2D source_last_frame;
layout(rgba16f, set = 1, binding = 1) uniform restrict writeonly image2D dest_light;

// The same current-NDC to last-frame-clip matrix the temporal pass consumes, filled once per view per frame.
layout(set = 2, binding = 0) uniform ReprojectionConstants {
	mat4 reprojection;
}
reprojection_constants;
#endif

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
	float thickness; // assumed occluder depth, in view-space units
	float depth_linearize_mul;
	float depth_linearize_add;
	float normal_rejection;
}
params;

#define GTAO_PI 3.14159265359
#define GOLDEN_RATIO 0.61803398875

// One 32-bit word of visibility per slice, so a sector spans pi/32 (5.6 degrees) of the slice's domain.
#define GTAO_SECTOR_COUNT 32u

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

// Antiderivative of the GTAO integrand cos(theta - gamma) * |sin theta|, normalized so that it is 0 at
// theta = 0 on both sides of V. This is the same closed form as the paper's per-slice inner integral (eq. 7),
// only evaluated at one bound instead of over a fixed arc: the integral of any arc [a, b] in the slice is
// then arc_bound(b) - arc_bound(a), including arcs that straddle V. Taking a horizon at signed angle h on
// each side recovers the original expression exactly, which is what lets the sector sum below stay
// numerically identical to plain GTAO wherever occlusion happens to reach all the way to grazing.
//
// cos(2 * theta - gamma) is passed in rather than computed here so slice_visibility() can advance it with a
// rotation instead of paying for a cosine at each of the 33 sector bounds.
float arc_bound(float p_theta, float p_cos_double, float p_cos_gamma, float p_sin_gamma) {
	float f = -p_cos_double + p_cos_gamma + 2.0 * p_theta * p_sin_gamma;
	return (p_theta < 0.0) ? -0.25 * f : 0.25 * f;
}

// arc_bound() for a one-off angle, where there is no sequence to advance a rotation along.
float arc_bound_at(float p_theta, float p_gamma, float p_cos_gamma, float p_sin_gamma) {
	return arc_bound(p_theta, cos(2.0 * p_theta - p_gamma), p_cos_gamma, p_sin_gamma);
}

// Karis' weighted average (http://graphicrants.blogspot.com/2013/12/tone-mapping.html): compressing each
// sample before it is summed and expanding the sum afterwards keeps one very bright occluder from turning
// into a firefly that the temporal pass then smears over several frames.
vec3 tonemap_for_average(vec3 p_color) {
	return p_color / (1.0 + dot(p_color, vec3(0.299, 0.587, 0.114)));
}

vec3 untonemap_average(vec3 p_color) {
	return p_color / max(1.0 - dot(p_color, vec3(0.299, 0.587, 0.114)), 0.0001);
}

// Mark every sector an occluder covers, given its angular extent already mapped to [0,1] across the slice's
// domain. Both ends round to the nearest sector boundary: the bitmask paper's own listing floors the near end
// and ceils the far one, which over-covers by up to a sector per sample and, measured over contiguous
// occlusion, darkens the result by ~3% everywhere — exactly the kind of uniform bias that survives temporal
// accumulation. Rounding instead leaves a residual bias of ~0.25% with half the RMS error.
uint occlusion_bits(float p_u_min, float p_u_max) {
	int sectors = int(GTAO_SECTOR_COUNT);
	int first = clamp(int(floor(p_u_min * float(sectors) + 0.5)), 0, sectors);
	int last = clamp(int(floor(p_u_max * float(sectors) + 0.5)), 0, sectors);
	int width = last - first;

	if (width <= 0) {
		return 0u;
	}

	// width >= 1 keeps both shifts inside [0,31]; a shift by the full word width is undefined.
	return (0xFFFFFFFFu >> uint(sectors - width)) << uint(first);
}

// Cosine-weighted arc this slice still sees: the sum of the sectors the bitmask left clear.
//
// The domain is [gamma - pi/2, gamma + pi/2]. Its width is pi for every gamma, and its ends are precisely the
// surface's own tangent plane, so mapping the sectors onto it carries GTAO's tangent-plane clamp for free
// instead of needing it applied to each horizon afterwards.
float slice_visibility(uint p_occlusion, float p_gamma) {
	float sector_arc = GTAO_PI / float(GTAO_SECTOR_COUNT);
	float cos_gamma = cos(p_gamma);
	float sin_gamma = sin(p_gamma);

	float theta = p_gamma - GTAO_PI * 0.5;

	// arc_bound()'s only transcendental term is cos(2 * theta - gamma), and consecutive sector bounds differ
	// by a constant 2 * sector_arc, so one plane rotation per sector replaces 33 cosines per slice.
	float angle = 2.0 * theta - p_gamma;
	float cos_double = cos(angle);
	float sin_double = sin(angle);
	float cos_step = cos(2.0 * sector_arc);
	float sin_step = sin(2.0 * sector_arc);

	float prev = arc_bound(theta, cos_double, cos_gamma, sin_gamma);
	float visible = 0.0;

	for (uint s = 0u; s < GTAO_SECTOR_COUNT; s++) {
		float next_cos = cos_double * cos_step - sin_double * sin_step;
		sin_double = sin_double * cos_step + cos_double * sin_step;
		cos_double = next_cos;
		theta += sector_arc;

		float bound = arc_bound(theta, cos_double, cos_gamma, sin_gamma);
		if ((p_occlusion & (1u << s)) == 0u) {
			visible += bound - prev;
		}
		prev = bound;
	}

	return visible;
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

	// NDC_to_view_space returns z as POSITIVE distance from the eye, so the camera looks toward +z here and
	// the direction back toward it is -z (matching normalize(-view_pos) in the perspective case, and
	// load_normal()'s own z flip).
	vec3 V = params.is_orthogonal ? vec3(0.0, 0.0, -1.0) : normalize(-view_pos);

	// Orthonormal basis of the plane perpendicular to V. Slices are swept around the VIEW VECTOR in this
	// basis rather than around the screen's own axis: the two coincide only at the exact centre of the
	// screen, and anywhere else a screen-uniform sweep covers the azimuth around V non-uniformly. That
	// biases the estimate by several percent in a pattern that drifts smoothly across the viewport (a flat,
	// unoccluded floor reads ~0.96 low-centre but ~1.09 toward the side edges), which survives both slice
	// jitter and temporal accumulation because it is systematic rather than noise.
	// V never approaches +/-X (that would need V.z == 0, i.e. a ray perpendicular to the view axis), so this
	// reference axis never degenerates.
	vec3 slice_basis_u = normalize(vec3(1.0, 0.0, 0.0) - V * V.x);
	vec3 slice_basis_v = cross(V, slice_basis_u);

	// This pixel's ray direction normalized to z = 1, used to map a view-space slice direction back to the
	// screen-space direction its samples have to step along. Orthogonal projections don't scale x/y with
	// depth, so there is no such term for them.
	vec2 ray_xy = params.is_orthogonal ? vec2(0.0) : view_pos.xy / max(view_pos.z, 0.0001);

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

#ifdef USE_INDIRECT_LIGHT
	vec3 light_sum = vec3(0.0);
	float light_weight_sum = 0.0;
#endif

	for (int slice = 0; slice < slice_count; slice++) {
		float phi = (GTAO_PI / float(slice_count)) * (float(slice) + jitter);
		vec3 slice_tangent = slice_basis_u * cos(phi) + slice_basis_v * sin(phi);

		// The screen-space direction whose samples stay inside this slice's plane. Dividing by the
		// NDC_to_view muls also carries their (negative for Y) sign, so the UV/view-space Y flip falls out
		// of the mapping instead of having to be applied by hand. Never degenerate: it could only vanish if
		// slice_tangent were parallel to this pixel's ray, and it is perpendicular to V by construction.
		vec2 dir_screen = normalize(vec2(
				(slice_tangent.x - ray_xy.x * slice_tangent.z) / params.NDC_to_view_mul_x,
				(slice_tangent.y - ray_xy.y * slice_tangent.z) / params.NDC_to_view_mul_y));

		vec3 slice_normal = normalize(cross(slice_tangent, V));
		vec3 normal_in_slice = N - slice_normal * dot(N, slice_normal);
		float normal_in_slice_len = length(normal_in_slice);

		if (normal_in_slice_len < 0.001) {
			continue;
		}

		vec3 normal_in_slice_n = normal_in_slice / normal_in_slice_len;
		// slice_tangent is perpendicular to V by construction, so it is already the in-slice reference axis
		// gamma is measured against. Clamped to the visible hemisphere: a normal-mapped (or near-grazing)
		// pixel can report a shading normal tipped slightly away from V, which would otherwise push the
		// tangent-plane bounds below zero.
		float gamma = clamp(atan(dot(normal_in_slice_n, slice_tangent), dot(normal_in_slice_n, V)),
				-GTAO_PI * 0.5, GTAO_PI * 0.5);

#ifdef USE_INDIRECT_LIGHT
		float cos_gamma = cos(gamma);
		float sin_gamma = sin(gamma);
#endif

		// One mask for the whole slice, not one per side: the domain [gamma - pi/2, gamma + pi/2] spans both
		// sides of V, and a sample taken along +slice_tangent always lands at a positive angle while one taken
		// along -slice_tangent always lands at a negative one, so the two sides fill disjoint halves of it.
		uint occlusion = 0u;

		for (int side = 0; side < 2; side++) {
			float side_sign = (side == 0) ? 1.0 : -1.0;

			for (int s = 0; s < step_count; s++) {
				float t = (float(s) + 0.5) / float(step_count);
				// At least one texel of separation per step. pixel_radius is the search radius expressed in
				// texels and shrinks with distance (a 1 m radius is only a handful of texels across at
				// 50 m), so without this the quadratic ramp puts the first samples less than a texel out.
				// The depth sampler is NEAREST, so those land back on the centre texel and contribute a
				// delta of ~0: a wasted tap that carries no occlusion information, and more of them are
				// wasted the more steps the quality level asks for (step 0 sits at 1/(2*step_count) of the
				// ramp, so it lands closer in the more steps there are).
				float step_dist = max(t * t * pixel_radius, float(s) + 1.0);

				vec2 sample_uv = uv + (dir_screen * side_sign * step_dist) * params.depth_texture_pixel_size;
				// Deliberately conservative, step-POSITION-based mip schedule (t*t*step_count is independent
				// of pixel_radius, unlike step_dist itself) that only reaches the coarser mips for the last
				// one or two farthest steps, regardless of how large pixel_radius is: the depth mip chain is
				// built with min-reduction (gtao_downsample.glsl biases every level toward whichever nearby
				// depth is closest, to keep thin occluders from disappearing), so sampling a coarse mip near
				// a real silhouette smears its occlusion well past the actual edge. Selecting by raw
				// step_dist instead reaches the coarsest available mip after only a small fraction of the
				// search at realistic radius/distance combinations (e.g. a 1m radius a few meters from the
				// camera is already hundreds of texels), which is what caused that smearing to dominate over
				// genuine concave detail.
				float sample_mip = clamp(floor(log2(max(t * t * float(step_count) * 0.5, 1.0))), 0.0, float(params.mip_count - 1));

				float sample_z = textureLod(source_depth_mipmaps, sample_uv, sample_mip).x;

				vec3 front_delta = NDC_to_view_space(sample_uv, sample_z) - view_pos;

				// Past the search radius the occluder is not this pixel's to account for. A hard cut is what the
				// radius means, and the step ramp already ends there, so the only sample rejected here is one
				// that landed across a depth discontinuity. Fading the wedge out towards the radius instead
				// measured worse: it costs ~17% accuracy against ray-traced reference and only takes the worst
				// frame-to-frame step at the boundary from 4.2% to 3.1%, which is the sector quantum rather than
				// the cut itself.
				float front_dist_sq = dot(front_delta, front_delta);
				if (front_dist_sq > params.radius * params.radius) {
					continue;
				}

				// The occluder's far side: the same screen ray, `thickness` deeper. Reconstructing it through
				// NDC_to_view_space keeps it exact for off-centre pixels and under orthogonal projection, where
				// "deeper along this ray" and "along the centre pixel's view vector" — which is what the bitmask
				// paper pushes along — are not the same direction.
				vec3 back_delta = NDC_to_view_space(sample_uv, sample_z + params.thickness) - view_pos;

				// Signed angles within the slice, measured from V towards slice_tangent, in the same frame gamma
				// is expressed in. atan() of the in-slice components rather than acos() of the full 3D angle:
				// pixel quantization drifts each sample slightly out of the slice plane, and only the in-slice
				// angle belongs in a mask whose sectors partition that same plane. A sample past the
				// perpendicular yields |angle| > pi/2, which the clamp below folds onto grazing.
				float theta_front = atan(dot(front_delta, slice_tangent), dot(front_delta, V));
				float theta_back = atan(dot(back_delta, slice_tangent), dot(back_delta, V));

				// Narrow the wedge at the end facing V, where the shading surface itself would otherwise register
				// as its own occluder through depth quantization or a normal map disagreeing with the geometry.
				// theta_back is always the more grazing of the two, so biasing theta_front narrows the wedge
				// rather than sliding it.
				theta_front += (theta_front < 0.0) ? -params.horizon_bias : params.horizon_bias;

				float u_min = clamp((min(theta_front, theta_back) - gamma) / GTAO_PI + 0.5, 0.0, 1.0);
				float u_max = clamp((max(theta_front, theta_back) - gamma) / GTAO_PI + 0.5, 0.0, 1.0);

#ifdef USE_INDIRECT_LIGHT
				uint wedge = occlusion_bits(u_min, u_max);
				uint claimed = wedge & ~occlusion;
				occlusion |= wedge;

				// Sectors already occluded were claimed by a nearer sample along this slice, which is the one
				// this pixel actually sees; only what this sample is first to cover contributes light.
				if (claimed != 0u) {
					// Solid angle of the claimed sectors. Summing them exactly would mean another
					// 32-iteration sweep per sample, so instead take the cosine-weighted arc spanning the
					// claimed run, from its first set sector to past its last, and scale by how densely that
					// span is actually filled. Since samples are visited nearest-first and each side of a
					// slice fills its own half of the domain outwards, the claimed sectors are normally one
					// contiguous run, and then this is exact rather than an approximation; only a hole left
					// inside the span by an earlier sample costs anything.
					//
					// Scaling the whole wedge by the same bit fraction instead, as is tempting, is much
					// worse: it treats every sector of the wedge as carrying equal weight when the cosine
					// term varies strongly across the domain, which measured up to 4.6 sectors of error and
					// a 148% 95th-percentile relative error against the exact sum.
					int claimed_first = findLSB(claimed);
					int claimed_last = findMSB(claimed);
					float sector_to_theta = GTAO_PI / float(GTAO_SECTOR_COUNT);
					float theta_a = float(claimed_first) * sector_to_theta + (gamma - GTAO_PI * 0.5);
					float theta_b = float(claimed_last + 1) * sector_to_theta + (gamma - GTAO_PI * 0.5);
					float span_arc = arc_bound_at(theta_b, gamma, cos_gamma, sin_gamma) - arc_bound_at(theta_a, gamma, cos_gamma, sin_gamma);
					float solid_angle = span_arc * float(bitCount(claimed)) / float(claimed_last - claimed_first + 1);

					// Where this occluder was on screen last frame. The round trip goes through the same
					// reverse-Z inverse gtao_temporal.glsl uses: the matrix mixes all four components, so
					// feeding it a linear depth stand-in corrupts the reprojected UV itself, not just a
					// depth comparison downstream.
					float ndc_z = params.is_orthogonal
							? clamp((sample_z - params.depth_linearize_mul) / max(params.depth_linearize_add - params.depth_linearize_mul, 0.0001), 0.0, 1.0)
							: clamp(params.depth_linearize_add - params.depth_linearize_mul / max(sample_z, 0.0001), 0.0, 1.0);
					vec4 clip_prev = reprojection_constants.reprojection * vec4(sample_uv * 2.0 - 1.0, ndc_z, 1.0);

					if (clip_prev.w > 0.0001) {
						vec2 uv_prev = (clip_prev.xy / clip_prev.w) * 0.5 + 0.5;

						if (all(greaterThanEqual(uv_prev, vec2(0.0))) && all(lessThan(uv_prev, vec2(1.0)))) {
							// Which level to read the occluder's outgoing radiance from. Some blur is wanted:
							// point-sampling one lit pixel carries the variance of its own specular and
							// shadowing into every pixel it lights. But a fixed coarse level is not, and a
							// fixed level 5 — 1/32 resolution, where one texel spans a large part of the
							// screen — means every occluder returns nearly the same heavily blurred colour, so
							// the result reads as a blurred copy of the frame rather than localized bounce.
							// Scaling with the occluder's screen distance instead reads a near occluder
							// sharply and a far one blurrier. Half the log, not the whole log: each sample
							// stands for the surface between itself and its neighbours, so the footprint to
							// average over is the sample SPACING, and under the quadratic step ramp above
							// that spacing grows as the square root of the distance. The constant folds in
							// both that ramp's own scale and the step being in this pass's (possibly half)
							// resolution rather than the last frame's.
							float light_mip = clamp(0.5 * log2(step_dist) + 1.0, 1.0, 4.0);
							vec3 occluder_light = textureLod(source_last_frame, uv_prev, light_mip).rgb;

							ivec2 sample_full_res = clamp(ivec2(sample_uv * vec2(params.full_screen_size)), ivec2(0), params.full_screen_size - ivec2(1));
							vec3 occluder_normal = load_normal(sample_full_res);

							// Only a surface turned towards this pixel can light it. The last-frame buffer
							// holds radiance towards the camera rather than towards us, so rejecting what
							// faces away is a correction for that, not part of the integral.
							float facing = -dot(occluder_normal, front_delta) * inversesqrt(max(front_dist_sq, 0.0001));
							float rejection = mix(1.0, smoothstep(0.0, 0.1, facing), params.normal_rejection);

							float light_weight = rejection * solid_angle;
							light_sum += tonemap_for_average(occluder_light) * light_weight;
							light_weight_sum += light_weight;
						}
					}
				}
#else
				occlusion |= occlusion_bits(u_min, u_max);
#endif
			}
		}

		visibility_sum += normal_in_slice_len * slice_visibility(occlusion, gamma);
		used_slices++;
	}

	// Deliberately not clamped to 1 here, only to a sane upper bound. With a handful of slices per frame the
	// per-frame estimate overshoots and undershoots around the true value; clipping the overshoots while
	// letting the undershoots through biases the temporal average downward, as a faint view-dependent
	// darkening of surfaces that are in fact completely unoccluded. gtao_upscale.glsl already clamps once
	// where intensity/power are applied, which is the right place for it.
	float visibility = (used_slices > 0) ? clamp(visibility_sum / float(used_slices), 0.0, 2.0) : 1.0;

	imageStore(dest_image, pos, vec4(visibility));

#ifdef USE_INDIRECT_LIGHT
	// The compressed samples are averaged before being expanded, then the integral's own weight is applied.
	// Expanding the integral directly does not work: the weights within one slice sum to the whole domain arc
	// (1.0 head-on, up to 1.57 at grazing), so the integral's luminance crosses 1 exactly where occlusion is
	// highest, and 1/(1 - luminance) then explodes — measured at 11477 against a correct 12.6 for a
	// radiance of 10 at gamma = 45 degrees. That is what put blown-out haloes in every corner, and only a
	// surface facing the camera head-on (gamma = 0, weights summing to exactly 1) escaped it.
	//
	// The average's luminance is below 1 by construction, so the expansion recovers the occluders' radiance
	// exactly; the weight then puts it back on the same per-slice scale the visibility above uses.
	vec3 light = vec3(0.0);
	if (used_slices > 0 && light_weight_sum > 0.0001) {
		light = untonemap_average(light_sum / light_weight_sum) * (light_weight_sum / float(used_slices));
	}
	imageStore(dest_light, pos, vec4(light, 0.0));
#endif
}
