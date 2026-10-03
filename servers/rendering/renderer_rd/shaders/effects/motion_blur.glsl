/**************************************************************************/
/*  motion_blur.glsl                                                      */
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

// The reconstruction filter of Guertin, McGuire & Nowrouzezahrai, "A Fast and Stable Feature-Aware Motion
// Blur Filter" (HPG 2014). Each pixel gathers N samples along a line through it as long as its neighborhood's
// dominant velocity (NeighborMax), and weights each sample by whether it is in front of or behind the pixel,
// and by whether its own blur, or the pixel's, is long enough to cover the distance between them.
//
// Over McGuire et al. 2012, it samples along two directions, alternating between them: the neighborhood's
// dominant velocity, and the pixel's own velocity (or, when that is too small to have a direction, the
// perpendicular to the dominant one), so that a tile holding motion in several directions still blurs each
// of them along its own path. Samples are weighted by how well their velocity, and the pixel's, line up with
// the direction they were taken along, the center sample's weight scales with the number of samples, so the
// balance between it and the others doesn't depend on N, and the NeighborMax lookup is jittered to break up
// the tile grid.
//
// The source is a copy of the color buffer, and the result is written straight back into the color buffer:
// pixels with nothing moving around them are left untouched.

#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source_color;
layout(set = 0, binding = 1) uniform sampler2D source_velocity_depth; // xy: V in pixels, z: linear depth.
layout(set = 0, binding = 2) uniform sampler2D source_neighbor_max;

layout(rgba16f, set = 1, binding = 0) uniform restrict writeonly image2D dest_color;

layout(push_constant, std430) uniform Params {
	ivec2 size;
	ivec2 tile_count;

	int tile_size;
	int sample_count; // N, always odd.
	int pad[2];
}
params;

// The paper's parameters. GAMMA sets how fast the second sampling direction turns from the perpendicular of
// the dominant velocity to the pixel's own velocity, as that grows past half a pixel, and KAPPA the weight of
// the center sample, N / (KAPPA * |V|).
#define GAMMA 1.5
#define KAPPA 40.0

// How far the NeighborMax lookup is jittered, in tiles along each axis.
#define TILE_JITTER 0.25

// The depth range over which samples go from being in front of the pixel to being behind it, as a fraction of
// the nearer of the two depths, so the classification is the same at any distance from the camera.
#define SOFT_Z_EXTENT 0.1

#define EPSILON 0.01

// Jimenez, "Next Generation Post Processing in Call of Duty: Advanced Warfare" (SIGGRAPH 2014).
float interleaved_gradient_noise(vec2 p_pos) {
	return fract(52.9829189 * fract(dot(p_pos, vec2(0.06711056, 0.00583715))));
}

// Weight 1 at distance 0, falling to 0 at distance 1 / p_inv_radius.
float cone(float p_distance, float p_inv_radius) {
	return clamp(1.0 - p_distance * p_inv_radius, 0.0, 1.0);
}

// Weight 1 up to distance p_radius, with a short smooth falloff around it.
float cylinder(float p_distance, float p_radius) {
	return 1.0 - smoothstep(0.95 * p_radius, 1.05 * p_radius, p_distance);
}

// 1 when depth p_z_a is in front of (or level with) depth p_z_b, fading to 0 as it moves behind it.
float soft_in_front(float p_z_a, float p_z_b) {
	return clamp(1.0 - (p_z_a - p_z_b) / (SOFT_Z_EXTENT * min(p_z_a, p_z_b)), 0.0, 1.0);
}

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.size))) {
		return;
	}

	vec2 pixel = vec2(pos);

	vec2 tile_jitter = vec2(interleaved_gradient_noise(pixel + vec2(19.0, 47.0)), interleaved_gradient_noise(pixel + vec2(71.0, 13.0))) - 0.5;
	tile_jitter *= 2.0 * TILE_JITTER * float(params.tile_size);
	ivec2 tile = clamp(ivec2((pixel + 0.5 + tile_jitter) / float(params.tile_size)), ivec2(0), params.tile_count - 1);

	vec2 v_max = texelFetch(source_neighbor_max, tile, 0).xy;
	float v_max_length = length(v_max);
	if (v_max_length <= 0.5 + EPSILON) {
		// Nothing around this pixel moves by more than half a pixel: it keeps its color.
		return;
	}

	vec4 center_color = texelFetch(source_color, pos, 0);
	vec4 center_velocity_depth = texelFetch(source_velocity_depth, pos, 0);
	vec2 v_c = center_velocity_depth.xy;
	float v_c_length = length(v_c);
	float z_c = center_velocity_depth.z;

	// The two sampling directions: w_n along the dominant velocity, and w_c along the pixel's own one, turning
	// to the perpendicular of w_n (on the side v_c leans to) as v_c becomes too short to point anywhere.
	vec2 w_n = v_max / v_max_length;
	vec2 w_p = vec2(-w_n.y, w_n.x);
	if (dot(w_p, v_c) < 0.0) {
		w_p = -w_p;
	}
	vec2 w_c = normalize(mix(w_p, v_c / max(v_c_length, EPSILON), clamp((v_c_length - 0.5) / GAMMA, 0.0, 1.0)));

	float radius_c = max(v_c_length, 0.5);

	// About one sample per pixel the blur spans, up to the quality setting's N. Both counts are odd, so the
	// middle sample always lands on the pixel itself.
	int sample_count = min(params.sample_count, int(ceil(v_max_length)) * 2 + 1);
	int center_sample = (sample_count - 1) / 2;

	float jitter = interleaved_gradient_noise(pixel) - 0.5;

	float total_weight = float(sample_count) / (KAPPA * radius_c);
	vec4 result = center_color * total_weight;

	for (int i = 0; i < sample_count; i++) {
		if (i == center_sample) {
			continue;
		}

		// Signed distance along the sampling direction, spread evenly over [-|v_max|, |v_max|].
		float t = mix(-1.0, 1.0, (float(i) + jitter + 1.0) / float(sample_count + 1));
		float sample_distance = t * v_max_length;
		vec2 direction = (i & 1) == 1 ? w_c : w_n;

		ivec2 sample_pos = clamp(ivec2(round(pixel + sample_distance * direction)), ivec2(0), params.size - 1);
		vec4 sample_color = texelFetch(source_color, sample_pos, 0);
		vec4 sample_velocity_depth = texelFetch(source_velocity_depth, sample_pos, 0);
		vec2 v_s = sample_velocity_depth.xy;
		float v_s_length = length(v_s);
		float radius_s = max(v_s_length, 0.5);
		float z_s = sample_velocity_depth.z;

		// Is the sample in front of the pixel, or behind it?
		float f = soft_in_front(z_s, z_c);
		float b = soft_in_front(z_c, z_s);

		// How well the pixel's and the sample's motion line up with the direction the sample was taken along.
		float w_a = abs(dot(w_c, direction));
		float w_b = abs(dot(v_s, direction)) / max(v_s_length, EPSILON);

		sample_distance = abs(sample_distance);
		float weight = 0.0;
		// A blurry sample in front of the pixel blurs over it.
		weight += f * cone(sample_distance, 1.0 / radius_s) * w_b;
		// A sample behind a blurry pixel is the background its blur lets through.
		weight += b * cone(sample_distance, 1.0 / radius_c) * w_a;
		// The sample and the pixel are both blurry, and blur over each other.
		weight += cylinder(sample_distance, min(radius_s, radius_c)) * max(w_a, w_b) * 2.0;

		total_weight += weight;
		result += sample_color * weight;
	}

	imageStore(dest_color, pos, result / total_weight);
}
