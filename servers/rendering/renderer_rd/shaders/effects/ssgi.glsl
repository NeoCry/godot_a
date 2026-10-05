/**************************************************************************/
/*  ssgi.glsl                                                             */
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

// Screen-space global illumination: stochastic screen-space ray traced diffuse indirect light (see
// effects/ssgi.h). The passes, one variant each:
// - MODE_TRACE: traces this frame's rays from every working pixel.
// - MODE_TEMPORAL: accumulates them with what the previous frames found there.
// - MODE_DENOISE: one level of the edge-aware a-trous filter.
// - MODE_APPLY: resolves the result to the full resolution buffer the lighting pass reads, bilaterally with
//   MODE_UPSCALE (half size).
//
// Positions are in the camera's view space throughout (-Z forward, which the projections of all views share),
// with pixel centers at (pixel + 0.5) / size. Normals that are compared across frames are in world space.

#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#define M_PI 3.14159265359

layout(set = 0, binding = 0, std140) uniform SceneData {
	mat4 projection[2];
	mat4 inv_projection[2];
	mat4 reprojection[2]; // This frame's view space to the previous frame's clip space.
	mat4 prev_view_from_view; // This frame's view space to the previous frame's view space.
	mat4 view_to_world; // Rotation only.
}
scene_data;

layout(push_constant, std430) uniform Params {
	ivec2 full_size;
	ivec2 working_size;

	uint view_index;
	int scale; // Full resolution pixels per working pixel, along each axis: 1, or 2 at half size.
	uint frame;
	bool orthogonal;

	uint ray_count;
	uint step_count;
	float max_distance;
	float thickness;

	float z_near;
	float last_frame_max_lod;
	float max_history;
	bool history_valid;

	int step_size;
	float intensity;
	float occlusion;
	float pixel_world_size;
}
params;

float luminance(vec3 p_color) {
	return dot(p_color, vec3(0.2126, 0.7152, 0.0722));
}

// View space position of the point at p_uv on screen, whose hardware depth (reverse Z) is p_depth.
vec3 reconstruct_view(vec2 p_uv, float p_depth) {
	vec4 pos = scene_data.inv_projection[params.view_index] * vec4(p_uv * 2.0 - 1.0, p_depth, 1.0);
	return pos.xyz / pos.w;
}

// Distance from the camera plane of a point whose hardware depth is p_depth. With the projections Godot uses
// (perspective, asymmetric or not, and orthogonal), it does not depend on where on screen the point is.
float linearize_depth(float p_depth) {
	mat4 m = scene_data.inv_projection[params.view_index];
	return -(m[2][2] * p_depth + m[3][2]) / (m[2][3] * p_depth + m[3][3]);
}

// View space position of the point at p_uv on screen that lies p_linear_depth from the camera plane.
vec3 view_position(vec2 p_uv, float p_linear_depth) {
	// Any point along the line of sight through p_uv will do, then slide it along to that depth.
	vec3 pos = reconstruct_view(p_uv, 0.5);
	if (params.orthogonal) {
		return vec3(pos.xy, -p_linear_depth);
	}
	return pos * (p_linear_depth / -pos.z);
}

// The full resolution pixel working pixel p_pos stands for.
ivec2 working_to_full(ivec2 p_pos) {
	return min(p_pos * params.scale, params.full_size - 1);
}

vec2 full_pixel_uv(ivec2 p_pixel) {
	return (vec2(p_pixel) + 0.5) / vec2(params.full_size);
}

#if defined(MODE_TRACE)

layout(set = 0, binding = 1) uniform sampler2D depth_buffer;
layout(set = 0, binding = 2) uniform sampler2D normal_roughness_buffer;
layout(set = 0, binding = 3) uniform sampler2D last_frame;

layout(rgba16f, set = 0, binding = 4) uniform restrict writeonly image2D trace_output;
layout(rgba32f, set = 0, binding = 5) uniform restrict writeonly image2D surface_output;

// Ray samples are this much more tightly packed at the start of the ray than evenly, which is where the
// light it finds counts most: contact, corners and the objects closest to the surface.
#define SSGI_STEP_DISTRIBUTION 2.0
// How wide a mipmap of the previous frame a hit takes its light from, relative to how far along the ray it
// is on screen: the footprint of a cone about ten degrees wide, which averages away much of the noise of
// sampling single pixels, while the light of nearby objects stays apart.
#define SSGI_FOOTPRINT 0.2
// How far off the surface rays start, in pixels of its depth, so they don't hit the surface they start on
// for want of depth buffer precision; more where it is seen at a grazing angle, where its depth changes the
// most from one pixel to the next.
#define SSGI_NORMAL_OFFSET 1.0
// How far from their start rays take their first sample, in pixels.
#define SSGI_START_PIXELS 2.0

// Jimenez, "Next Generation Post Processing in Call of Duty: Advanced Warfare" (2014).
float interleaved_gradient_noise(vec2 p_pos) {
	return fract(52.9829189 * fract(dot(p_pos, vec2(0.06711056, 0.00583715))));
}

float hash12(vec2 p_pos) {
	vec3 p3 = fract(vec3(p_pos.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

float linear_depth_at(ivec2 p_pixel) {
	float depth = texelFetch(depth_buffer, p_pixel, 0).r;
	// Nothing was drawn where the depth buffer still holds the far plane (0, with reverse Z): no ray hits it.
	return depth > 0.0 ? linearize_depth(depth) : 1e20;
}

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.working_size))) {
		return;
	}

	ivec2 pixel = working_to_full(pos);
	float depth = texelFetch(depth_buffer, pixel, 0).r;
	if (depth <= 0.0) {
		imageStore(trace_output, pos, vec4(0.0));
		imageStore(surface_output, pos, vec4(0.0));
		return;
	}

	vec3 vertex = reconstruct_view(full_pixel_uv(pixel), depth);
	vec3 normal = normalize(texelFetch(normal_roughness_buffer, pixel, 0).xyz * 2.0 - 1.0);
	float view_depth = -vertex.z;
	imageStore(surface_output, pos, vec4(view_depth, normalize(mat3(scene_data.view_to_world) * normal)));

	float pixel_size = params.orthogonal ? params.pixel_world_size : params.pixel_world_size * view_depth;
	float n_dot_v = params.orthogonal ? normal.z : dot(normal, -normalize(vertex));
	vec3 origin = vertex + normal * (pixel_size * SSGI_NORMAL_OFFSET / max(n_dot_v, 0.25));

	// An orthonormal basis around the normal (Duff et al., "Building an Orthonormal Basis, Revisited").
	float basis_sign = normal.z >= 0.0 ? 1.0 : -1.0;
	float basis_a = -1.0 / (basis_sign + normal.z);
	float basis_b = normal.x * normal.y * basis_a;
	vec3 tangent = vec3(1.0 + basis_sign * normal.x * normal.x * basis_a, basis_sign * basis_b, -basis_sign * normal.x);
	vec3 bitangent = vec3(basis_b, basis_sign + normal.y * normal.y * basis_a, -normal.y);

	// Every pixel draws its directions from a 2D low discrepancy sequence (R2, Roberts 2018), each frame going on
	// where the last one left off, starting from a point of its own that varies smoothly from pixel to pixel in
	// one dimension and randomly in the other, so that neighbors cover the hemisphere differently.
	vec2 sequence_start = vec2(interleaved_gradient_noise(vec2(pos)), hash12(vec2(pos)));
	float step_jitter = interleaved_gradient_noise(vec2(pos) + float(params.frame % 64u) * 5.588238);

	mat4 projection = scene_data.projection[params.view_index];
	vec2 screen_size = vec2(params.full_size);

	vec3 light = vec3(0.0);
	float hits = 0.0;

	for (uint r = 0; r < params.ray_count; r++) {
		// (Wrapped, so the index stays exact as a float.)
		vec2 xi = fract(sequence_start + float((params.frame % 4096u) * params.ray_count + r) * vec2(0.7548776662, 0.5698402910));

		// Cosine weighted, so that the light the rays find just averages into what a diffuse surface gets.
		float radius = sqrt(xi.x);
		float phi = 2.0 * M_PI * xi.y;
		vec3 dir = tangent * (radius * cos(phi)) + bitangent * (radius * sin(phi)) + normal * sqrt(max(1.0 - xi.x, 0.0));

		float ray_length = params.max_distance;
		if (!params.orthogonal && dir.z > 0.0) {
			// Stop short of the near plane, through which the ray would project to the other side.
			ray_length = min(ray_length, (-params.z_near * 1.001 - origin.z) / dir.z);
		}
		if (ray_length <= 0.0) {
			continue;
		}
		vec3 end = origin + dir * ray_length;

		vec4 clip_start = projection * vec4(origin, 1.0);
		vec4 clip_end = projection * vec4(end, 1.0);
		// The ray, in pixels. Its depth is interpolated perspective correctly along it: 1 / w (the depth, in
		// perspective) varies linearly on screen.
		vec2 screen_start = (clip_start.xy / clip_start.w * 0.5 + 0.5) * screen_size;
		vec2 screen_end = (clip_end.xy / clip_end.w * 0.5 + 0.5) * screen_size;
		float inv_w_start = 1.0 / clip_start.w;
		float inv_w_end = 1.0 / clip_end.w;
		float depth_start = -origin.z;
		float depth_end = -end.z;

		vec2 delta = screen_end - screen_start;
		float screen_length = length(delta);
		if (screen_length < 1.0) {
			continue; // Shorter than a pixel: there's nothing on screen for it to hit.
		}

		// Clip to the screen: past its edge, there is nothing to hit (a miss).
		float t_max = 1.0;
		if (delta.x > 0.0) {
			t_max = min(t_max, (screen_size.x - screen_start.x) / delta.x);
		} else if (delta.x < 0.0) {
			t_max = min(t_max, -screen_start.x / delta.x);
		}
		if (delta.y > 0.0) {
			t_max = min(t_max, (screen_size.y - screen_start.y) / delta.y);
		} else if (delta.y < 0.0) {
			t_max = min(t_max, -screen_start.y / delta.y);
		}
		float march_length = screen_length * t_max;
		if (march_length <= SSGI_START_PIXELS) {
			continue;
		}

		float prev_t = 0.0;
		float prev_ray_depth = depth_start;
		bool hit = false;
		float hit_t = 0.0;

		for (uint i = 0; i < params.step_count; i++) {
			// A couple of pixels away from the start at least, and packed towards it.
			float f = (float(i) + step_jitter) / float(params.step_count);
			float t = (SSGI_START_PIXELS + (march_length - SSGI_START_PIXELS) * pow(f, SSGI_STEP_DISTRIBUTION)) / screen_length;
			ivec2 sample_pixel = ivec2(screen_start + delta * t);
			if (any(lessThan(sample_pixel, ivec2(0))) || any(greaterThanEqual(sample_pixel, params.full_size))) {
				break;
			}

			float ray_depth = params.orthogonal ? mix(depth_start, depth_end, t) : 1.0 / mix(inv_w_start, inv_w_end, t);
			float scene_depth = linear_depth_at(sample_pixel);
			float epsilon = scene_depth * 0.001;

			// The ray hits what is drawn there when it is behind it, but no further than its thickness, or when
			// it was in front of it at the previous sample (it crossed it between the two). Further behind it
			// than that, it passes behind it.
			if (ray_depth > scene_depth + epsilon && (ray_depth < scene_depth + params.thickness || prev_ray_depth <= scene_depth + epsilon)) {
				// Find where it crossed more precisely, for the light it takes.
				float lo = prev_t;
				float hi = t;
				for (int j = 0; j < 4; j++) {
					float mid = (lo + hi) * 0.5;
					float mid_ray_depth = params.orthogonal ? mix(depth_start, depth_end, mid) : 1.0 / mix(inv_w_start, inv_w_end, mid);
					if (mid_ray_depth > linear_depth_at(ivec2(screen_start + delta * mid))) {
						hi = mid;
					} else {
						lo = mid;
					}
				}
				hit = true;
				hit_t = hi;
				break;
			}

			prev_t = t;
			prev_ray_depth = ray_depth;
		}

		if (!hit) {
			continue;
		}

		vec2 hit_screen = screen_start + delta * hit_t;
		ivec2 hit_pixel = clamp(ivec2(hit_screen), ivec2(0), params.full_size - 1);
		float hit_depth = texelFetch(depth_buffer, hit_pixel, 0).r;
		vec3 hit_vertex = reconstruct_view(full_pixel_uv(hit_pixel), hit_depth);

		// Where that was on the previous frame's image. What it doesn't show (it was off screen) is unknown,
		// so the ray counts as a miss: the ambient light stands in for it.
		vec4 prev_clip = scene_data.reprojection[params.view_index] * vec4(hit_vertex, 1.0);
		if (prev_clip.w <= 0.0) {
			continue;
		}
		vec2 prev_uv = prev_clip.xy / prev_clip.w * 0.5 + 0.5;
		if (any(lessThan(prev_uv, vec2(0.0))) || any(greaterThan(prev_uv, vec2(1.0)))) {
			continue;
		}

		float lod = clamp(log2(max(length(hit_screen - screen_start) * SSGI_FOOTPRINT, 1.0)), 0.0, params.last_frame_max_lod);
		vec3 hit_light = textureLod(last_frame, prev_uv, lod).rgb;

		// A ray only gets light from the front of what it hits. Where what is drawn there faces away from it,
		// the ray hit its back, which the screen doesn't show: it blocks the ambient light, but gives none.
		vec3 hit_normal = normalize(texelFetch(normal_roughness_buffer, hit_pixel, 0).xyz * 2.0 - 1.0);
		hit_light *= smoothstep(-0.05, 0.15, dot(hit_normal, -dir));

		light += max(hit_light, vec3(0.0));
		hits += 1.0;
	}

	float inv_ray_count = 1.0 / float(params.ray_count);
	imageStore(trace_output, pos, vec4(light * inv_ray_count, hits * inv_ray_count));
}

#elif defined(MODE_TEMPORAL)

layout(set = 0, binding = 1) uniform sampler2D trace_input;
layout(set = 0, binding = 2) uniform sampler2D surface_input;
layout(set = 0, binding = 3) uniform sampler2D prev_surface;
layout(set = 0, binding = 4) uniform sampler2D prev_history;
layout(set = 0, binding = 5) uniform sampler2D prev_meta;

layout(rgba16f, set = 0, binding = 6) uniform restrict writeonly image2D history_output;
layout(rgba32f, set = 0, binding = 7) uniform restrict writeonly image2D meta_output;
layout(r32f, set = 0, binding = 8) uniform restrict writeonly image2D variance_output;

// How far apart, relative to their depth, the reprojected point and what the previous frame stored there may
// be for it to be the same surface, and how closely their normals must agree.
#define SSGI_HISTORY_DEPTH_TOLERANCE 0.05
#define SSGI_HISTORY_NORMAL_THRESHOLD 0.8
// Frames of history before the per pixel variance of the luminance can be estimated from its moments over
// time, rather than from the pixels around it.
#define SSGI_VARIANCE_FRAMES 4.0
// A history that is further from what the pixels around find in this frame than this many standard errors
// of their average no longer stands for the light there (it changed, or something moved through it): it is
// cut down to SSGI_ANTILAG_FRAMES, so that it catches up within a few frames.
#define SSGI_ANTILAG_SIGMAS 4.0
#define SSGI_ANTILAG_MARGIN 0.15
#define SSGI_ANTILAG_FRAMES 3.0

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.working_size))) {
		return;
	}

	vec4 surface = texelFetch(surface_input, pos, 0);
	if (surface.x <= 0.0) {
		imageStore(history_output, pos, vec4(0.0));
		imageStore(meta_output, pos, vec4(0.0));
		imageStore(variance_output, pos, vec4(0.0));
		return;
	}

	vec4 current = texelFetch(trace_input, pos, 0);
	float current_luminance = luminance(current.rgb);

	// What this frame's rays found around the pixel, on its own surface.
	vec4 neighborhood = vec4(0.0);
	vec2 luminance_moments = vec2(0.0);
	float neighbors = 0.0;
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			ivec2 q = clamp(pos + ivec2(x, y), ivec2(0), params.working_size - 1);
			float q_depth = texelFetch(surface_input, q, 0).x;
			if (abs(q_depth - surface.x) > surface.x * 0.05) {
				continue;
			}
			vec4 q_light = texelFetch(trace_input, q, 0);
			float q_luminance = luminance(q_light.rgb);
			neighborhood += q_light;
			luminance_moments += vec2(q_luminance, q_luminance * q_luminance);
			neighbors += 1.0;
		}
	}
	neighborhood /= neighbors;
	luminance_moments /= neighbors;
	float spatial_variance = max(luminance_moments.y - luminance_moments.x * luminance_moments.x, 0.0);

	// Reproject into the previous frame.
	vec4 history = vec4(0.0);
	vec4 meta = vec4(0.0);
	float history_weight = 0.0;

	ivec2 pixel = working_to_full(pos);
	vec3 vertex = view_position(full_pixel_uv(pixel), surface.x);
	vec4 prev_clip = scene_data.reprojection[params.view_index] * vec4(vertex, 1.0);
	if (params.history_valid && prev_clip.w > 0.0) {
		vec2 prev_uv = prev_clip.xy / prev_clip.w * 0.5 + 0.5;
		float expected_depth = -(scene_data.prev_view_from_view * vec4(vertex, 1.0)).z;

		// Bilinear filtering by hand, in working pixels (whose centers are the full resolution pixel centers they
		// stand for), dropping the taps of other surfaces.
		vec2 history_pos = (prev_uv * vec2(params.full_size) - 0.5) / float(params.scale);
		ivec2 base = ivec2(floor(history_pos));
		vec2 f = history_pos - vec2(base);
		for (int i = 0; i < 4; i++) {
			ivec2 offset = ivec2(i & 1, i >> 1);
			ivec2 tap = base + offset;
			if (any(lessThan(tap, ivec2(0))) || any(greaterThanEqual(tap, params.working_size))) {
				continue;
			}
			vec4 tap_surface = texelFetch(prev_surface, tap, 0);
			if (tap_surface.x <= 0.0 || abs(tap_surface.x - expected_depth) > expected_depth * SSGI_HISTORY_DEPTH_TOLERANCE || dot(tap_surface.yzw, surface.yzw) < SSGI_HISTORY_NORMAL_THRESHOLD) {
				continue;
			}
			vec2 weights = mix(1.0 - f, f, vec2(offset));
			float weight = weights.x * weights.y;
			history += texelFetch(prev_history, tap, 0) * weight;
			meta += texelFetch(prev_meta, tap, 0) * weight;
			history_weight += weight;
		}
	}

	float frames = 1.0;
	vec2 moments = vec2(current_luminance, current_luminance * current_luminance);
	vec4 result = current;
	if (history_weight > 0.01) {
		history /= history_weight;
		meta /= history_weight;
		frames = min(floor(meta.x + 0.5) + 1.0, params.max_history);

		// Anti-lag. The noise of what this frame finds around the pixel is estimated from how much the rays
		// vary at the pixel over time (its variance per frame): a single frame's neighborhood often has no hit
		// at all where few rays hit, which says little.
		if (meta.x >= SSGI_VARIANCE_FRAMES) {
			float hit_share = clamp(history.a, 0.0, 1.0);
			float hit_error = sqrt(hit_share * (1.0 - hit_share) / (float(params.ray_count) * neighbors));
			float luminance_error = sqrt(max(meta.z - meta.y * meta.y, 0.0) / neighbors);
			float history_luminance = luminance(history.rgb);
			float neighborhood_luminance = luminance(neighborhood.rgb);
			bool lagging = abs(history.a - neighborhood.a) > SSGI_ANTILAG_SIGMAS * hit_error + SSGI_ANTILAG_MARGIN;
			lagging = lagging || abs(history_luminance - neighborhood_luminance) > SSGI_ANTILAG_SIGMAS * luminance_error + SSGI_ANTILAG_MARGIN * max(history_luminance, neighborhood_luminance) + 1e-4;
			if (lagging) {
				frames = min(frames, SSGI_ANTILAG_FRAMES);
			}
		}

		float blend = 1.0 / frames;
		result = mix(history, current, blend);
		moments = mix(meta.yz, moments, blend);
	}

	// The variance of the luminance per frame guides the spatial filter: from the moments over time once there
	// are enough frames of them, from the pixels around until then.
	float variance = frames >= SSGI_VARIANCE_FRAMES ? max(moments.y - moments.x * moments.x, 0.0) : spatial_variance;
	// The accumulated light averages that many frames, so its own variance is that much smaller.
	variance /= frames;

	imageStore(history_output, pos, result);
	imageStore(meta_output, pos, vec4(frames, moments, 0.0));
	imageStore(variance_output, pos, vec4(variance));
}

#elif defined(MODE_DENOISE)

layout(set = 0, binding = 1) uniform sampler2D color_input;
layout(set = 0, binding = 2) uniform sampler2D variance_input;
layout(set = 0, binding = 3) uniform sampler2D surface_input;

layout(rgba16f, set = 0, binding = 4) uniform restrict writeonly image2D color_output;
layout(r32f, set = 0, binding = 5) uniform restrict writeonly image2D variance_output;

// Edge stopping: how far off the plane of the pixel (relative to its depth, per pixel of the filter's step)
// a neighbor may be, how closely their normals must agree, and how many standard deviations of the noise
// their luminance may differ by.
#define SSGI_PLANE_TOLERANCE 0.01
#define SSGI_NORMAL_POWER 32.0
#define SSGI_LUMINANCE_SIGMAS 4.0

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.working_size))) {
		return;
	}

	vec4 surface = texelFetch(surface_input, pos, 0);
	vec4 center = texelFetch(color_input, pos, 0);
	float center_variance = texelFetch(variance_input, pos, 0).r;
	if (surface.x <= 0.0) {
		imageStore(color_output, pos, vec4(0.0));
		imageStore(variance_output, pos, vec4(0.0));
		return;
	}

	vec3 vertex = mat3(scene_data.view_to_world) * view_position(full_pixel_uv(working_to_full(pos)), surface.x);
	float center_luminance = luminance(center.rgb);

	// The variance is noisy itself: blur it a little before using it to tell noise from detail.
	float blurred_variance = 0.0;
	{
		const float kernel[2] = float[](0.5, 0.25);
		for (int y = -1; y <= 1; y++) {
			for (int x = -1; x <= 1; x++) {
				ivec2 q = clamp(pos + ivec2(x, y), ivec2(0), params.working_size - 1);
				blurred_variance += texelFetch(variance_input, q, 0).r * kernel[abs(x)] * kernel[abs(y)];
			}
		}
	}
	float luminance_scale = 1.0 / (SSGI_LUMINANCE_SIGMAS * sqrt(blurred_variance) + 1e-4);
	float plane_scale = 1.0 / (surface.x * SSGI_PLANE_TOLERANCE * float(params.step_size));

	// A 3x3 B-spline kernel, with holes of step_size pixels between its taps.
	const float kernel[2] = float[](0.5, 0.25);
	vec4 sum = center * 0.25;
	float weight_sum = 0.25;
	float variance_sum = center_variance * 0.0625;

	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			if (x == 0 && y == 0) {
				continue;
			}
			ivec2 q = pos + ivec2(x, y) * params.step_size;
			if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, params.working_size))) {
				continue;
			}
			vec4 q_surface = texelFetch(surface_input, q, 0);
			if (q_surface.x <= 0.0) {
				continue;
			}
			vec4 q_color = texelFetch(color_input, q, 0);

			vec3 q_vertex = mat3(scene_data.view_to_world) * view_position(full_pixel_uv(working_to_full(q)), q_surface.x);
			float plane_distance = abs(dot(q_vertex - vertex, surface.yzw));
			float weight = kernel[abs(x)] * kernel[abs(y)];
			weight *= exp(-plane_distance * plane_scale);
			weight *= pow(max(dot(q_surface.yzw, surface.yzw), 0.0), SSGI_NORMAL_POWER);
			weight *= exp(-abs(luminance(q_color.rgb) - center_luminance) * luminance_scale);

			sum += q_color * weight;
			weight_sum += weight;
			variance_sum += texelFetch(variance_input, q, 0).r * weight * weight;
		}
	}

	imageStore(color_output, pos, sum / weight_sum);
	imageStore(variance_output, pos, vec4(variance_sum / (weight_sum * weight_sum)));
}

#elif defined(MODE_APPLY)

layout(set = 0, binding = 1) uniform sampler2D color_input;
layout(set = 0, binding = 2) uniform sampler2D surface_input;
layout(set = 0, binding = 3) uniform sampler2D depth_buffer;
layout(set = 0, binding = 4) uniform sampler2D normal_roughness_buffer;

layout(rgba16f, set = 0, binding = 5) uniform restrict writeonly image2D final_output;

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.full_size))) {
		return;
	}

	float depth = texelFetch(depth_buffer, pixel, 0).r;
	if (depth <= 0.0) {
		imageStore(final_output, pixel, vec4(0.0));
		return;
	}

#ifdef MODE_UPSCALE
	// Each working pixel stands for the full resolution pixel at twice its coordinates. Blend the (up to) four
	// around this one by position, and by how well each one's surface matches its own, so that light doesn't
	// bleed across edges only the full resolution shows.
	float linear_depth = linearize_depth(depth);
	vec3 normal = normalize(mat3(scene_data.view_to_world) * normalize(texelFetch(normal_roughness_buffer, pixel, 0).xyz * 2.0 - 1.0));

	vec2 working_pos = vec2(pixel) / float(params.scale);
	ivec2 base = ivec2(floor(working_pos));
	vec2 f = working_pos - vec2(base);

	vec4 color = vec4(0.0);
	float weight_sum = 0.0;
	vec4 best_color = vec4(0.0);
	float best_difference = 1e20;
	for (int i = 0; i < 4; i++) {
		ivec2 offset = ivec2(i & 1, i >> 1);
		ivec2 tap = min(base + offset, params.working_size - 1);
		vec4 tap_surface = texelFetch(surface_input, tap, 0);
		if (tap_surface.x <= 0.0) {
			continue;
		}
		vec4 tap_color = texelFetch(color_input, tap, 0);
		float difference = abs(tap_surface.x - linear_depth) / linear_depth;
		if (difference < best_difference) {
			best_difference = difference;
			best_color = tap_color;
		}
		vec2 bilinear = mix(1.0 - f, f, vec2(offset));
		float weight = bilinear.x * bilinear.y;
		weight *= exp(-difference * 50.0);
		weight *= pow(max(dot(tap_surface.yzw, normal), 0.0), 8.0);
		color += tap_color * weight;
		weight_sum += weight;
	}
	// Where none of them is on this pixel's surface, take the one closest in depth.
	color = weight_sum > 1e-3 ? color / weight_sum : best_color;
#else
	vec4 color = texelFetch(color_input, pixel, 0);
#endif

	imageStore(final_output, pixel, vec4(max(color.rgb, vec3(0.0)) * params.intensity, clamp(color.a, 0.0, 1.0) * params.occlusion));
}

#endif
