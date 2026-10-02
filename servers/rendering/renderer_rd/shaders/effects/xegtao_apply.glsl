/**************************************************************************/
/*  xegtao_apply.glsl                                                     */
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

// Last XeGTAO pass: resolves the denoised working AO term into the full resolution buffers the lighting pass
// samples. At full resolution that is a 1:1 copy; with MODE_UPSCALE (half resolution) each pixel blends its
// 2x2 neighborhood of working pixels by both bilinear position and how well each one's depth matches its
// own, so occlusion doesn't bleed across edges that only exist at full resolution. XeGTAO itself runs at
// full resolution and suggests exactly this ("upgrading the denoiser pass with a bilateral upsample") for
// running it at a lower one.
//
// This is also where the artistic intensity and the distance fade out are applied, once, on the final
// value, and where the bent normal offset is added back onto each pixel's own normal.

#[compute]

#version 450

#VERSION_DEFINES

#include "xegtao_inc.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform usampler2D source_ao_term;
layout(set = 0, binding = 1) uniform sampler2D source_full_depth; // Hardware depth buffer.
#ifdef MODE_UPSCALE
layout(set = 0, binding = 2) uniform sampler2D source_working_depth; // MIP 0 of the prefiltered view space depth.
#endif
#ifdef USE_BENT_NORMALS
layout(rgba8, set = 0, binding = 3) uniform restrict readonly image2D source_normal;
#endif

layout(r8, set = 1, binding = 0) uniform restrict writeonly image2D dest_visibility;
#ifdef USE_BENT_NORMALS
layout(rgba8, set = 1, binding = 1) uniform restrict writeonly image2D dest_bent_normal;
#endif

layout(push_constant, std430) uniform Params {
	ivec2 full_size;
	ivec2 working_size;

	float depth_linearize_mul;
	float depth_linearize_add;
	float z_near;
	float z_far;

	bool is_orthogonal;
	float intensity;
	float fade_out_mul;
	float fade_out_add;
}
params;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.full_size))) {
		return;
	}

	float depth = xegtao_linearize_depth(texelFetch(source_full_depth, pos, 0).x, params.is_orthogonal, params.depth_linearize_mul, params.depth_linearize_add, params.z_near, params.z_far);

#ifdef MODE_UPSCALE
	// Working pixel p stands for full resolution pixel 2p (see xegtao_prefilter.glsl), so this pixel sits
	// exactly on a working pixel when even and halfway between two when odd.
	vec2 working_pos = vec2(pos) * 0.5;
	ivec2 base = ivec2(floor(working_pos));
	vec2 frac = working_pos - vec2(base);

	const ivec2 offsets[4] = ivec2[4](ivec2(0, 0), ivec2(1, 0), ivec2(0, 1), ivec2(1, 1));
	float bilinear_weights[4] = float[4](
			(1.0 - frac.x) * (1.0 - frac.y),
			frac.x * (1.0 - frac.y),
			(1.0 - frac.x) * frac.y,
			frac.x * frac.y);

	// Same relative depth threshold the main pass detects edges with.
	float depth_tolerance = max(depth * 0.011, 1e-4);

	vec4 term_sum = vec4(0.0);
	float weight_sum = 0.0;
	vec4 closest_term = vec4(0.0);
	float closest_depth_error = 1e38;

	for (int i = 0; i < 4; i++) {
		ivec2 tap_pos = min(base + offsets[i], params.working_size - 1);
		vec4 tap_term = xegtao_decode_term(texelFetch(source_ao_term, tap_pos, 0).x);
		float depth_error = abs(texelFetch(source_working_depth, tap_pos, 0).x - depth);

		float weight = bilinear_weights[i] * clamp(1.25 - depth_error / depth_tolerance, 0.0, 1.0);
		term_sum += tap_term * weight;
		weight_sum += weight;

		if (depth_error < closest_depth_error) {
			closest_depth_error = depth_error;
			closest_term = tap_term;
		}
	}

	// No working pixel lies on this surface (a thin feature the working resolution missed): take the
	// closest one in depth rather than blending across the edge.
	vec4 term = (weight_sum > 1e-4) ? term_sum / weight_sum : closest_term;
#else
	vec4 term = xegtao_decode_term(texelFetch(source_ao_term, pos, 0).x);
#endif

	float fade_out = clamp(depth * params.fade_out_mul + params.fade_out_add, 0.0, 1.0);
	float strength = clamp(params.intensity, 0.0, 1.0) * fade_out;

	float obscurance = clamp(params.intensity * (1.0 - term.w), 0.0, 1.0) * fade_out;
	imageStore(dest_visibility, pos, vec4(1.0 - obscurance));

#ifdef USE_BENT_NORMALS
	vec3 normal = xegtao_load_normal(imageLoad(source_normal, pos));
	vec3 bent_normal = normalize(normal + term.xyz * strength);
	// Back to Godot's view space, which looks down -z.
	bent_normal.z = -bent_normal.z;
	imageStore(dest_bent_normal, pos, vec4(bent_normal * 0.5 + 0.5, 1.0));
#endif
}
