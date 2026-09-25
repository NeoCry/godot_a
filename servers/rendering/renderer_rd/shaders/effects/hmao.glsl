/**************************************************************************/
/*  hmao.glsl                                                             */
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

// Height map based ambient occlusion, after CRYENGINE's effect of the same name: occlusion is gathered in
// WORLD space from a top-down depth map of the scene around the viewer (see HeightMapAO::render_height_map()),
// not from the screen's own depth buffer. That is the whole point of the technique - a screen space gather
// like GTAO can only see what is on screen, and only within a radius of a few pixels, so it produces contact
// shadows and creases but can never darken a valley because of the mountain beside it, or a courtyard because
// of the buildings around it. Those are exactly the hundreds-of-metres scale occluders this pass handles, at a
// cost that depends on the height map's resolution rather than on the world's complexity.
//
// For each pixel, the world position and normal are recovered from the view's depth/normal buffers, and a few
// directions around it are marched over the height map with exponentially increasing step lengths (so the same
// handful of taps covers both the metre scale near the shading point and the far edge of the map). Each tap
// asks a single question: does the height field rise above the shading point there, and if so, how much of
// this surface's cosine-weighted hemisphere does that occluder block?

#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source_depth;
layout(rgba8, set = 0, binding = 1) uniform restrict readonly image2D source_normal;
// Top-down orthographic depth map of the world around the viewer. Sampled with a nearest, transparent-black
// border sampler: nearest because linear filtering of a depth format isn't guaranteed to be supported, and a
// zero border because zero is the far (lowest) plane under reverse Z, i.e. "no occluder here", which is the
// right answer for anything outside the map.
layout(set = 0, binding = 2) uniform sampler2D height_map;

layout(rg16f, set = 1, binding = 0) uniform restrict writeonly image2D dest_image;

layout(push_constant, std430) uniform Params {
	// Camera transform, as its three basis columns with the origin folded into the w components, which keeps
	// this whole block inside the 128 bytes of push constant space every driver guarantees.
	vec4 cam_basis_x; // xyz: basis column 0, w: origin.x
	vec4 cam_basis_y; // xyz: basis column 1, w: origin.y
	vec4 cam_basis_z; // xyz: basis column 2, w: origin.z

	ivec2 screen_size; // Resolution of this pass (half of full_screen_size unless half size is disabled).
	ivec2 full_screen_size; // Native resolution of source_depth/source_normal.

	float NDC_to_view_mul_x;
	float NDC_to_view_mul_y;
	float NDC_to_view_add_x;
	float NDC_to_view_add_y;

	float depth_linearize_mul;
	float depth_linearize_add;
	uint flags; // Bit 0: orthogonal projection. Bits 8-15: quality level.
	float amount;

	vec2 map_origin; // World XZ of the height map's (0, 0) corner.
	float map_size; // World size of the height map's square footprint.
	float map_bottom; // World Y a height map depth of 0.0 stands for.

	float map_height; // World Y span of the height map, from map_bottom upwards.
	float radius; // How far out, in world units, the gather marches.
	float bias; // Height difference, in world units, below which an occluder is ignored.
	float edge_fade; // Width of the border fade, as a fraction of the map, see EDGE_FADE below.
}
params;

#define FLAG_ORTHOGONAL (1 << 0)
#define QUALITY_SHIFT 8
#define QUALITY_MASK 0xFF

const float M_PI = 3.14159265359;

// Directions swept around each shading point, and taps along each of them. Quality only buys directions and
// taps: everything else about the gather (its radius, the map it reads) is a property of the environment.
const int directions_for_quality[4] = int[4](4, 8, 12, 16);
const int steps_for_quality[4] = int[4](4, 6, 8, 12);

float linearize_depth(float p_depth) {
	if (bool(params.flags & FLAG_ORTHOGONAL)) {
		return mix(params.depth_linearize_mul, params.depth_linearize_add, p_depth);
	}
	return params.depth_linearize_mul / (params.depth_linearize_add - p_depth);
}

// View space position, in the renderer's convention (camera looking down -z), of the pixel at p_uv.
vec3 view_position(vec2 p_uv, float p_linear_depth) {
	vec2 ndc_to_view_mul = vec2(params.NDC_to_view_mul_x, params.NDC_to_view_mul_y);
	vec2 ndc_to_view_add = vec2(params.NDC_to_view_add_x, params.NDC_to_view_add_y);
	if (bool(params.flags & FLAG_ORTHOGONAL)) {
		return vec3(ndc_to_view_mul * p_uv + ndc_to_view_add, -p_linear_depth);
	}
	return vec3((ndc_to_view_mul * p_uv + ndc_to_view_add) * p_linear_depth, -p_linear_depth);
}

vec3 view_to_world(vec3 p_view) {
	vec3 origin = vec3(params.cam_basis_x.w, params.cam_basis_y.w, params.cam_basis_z.w);
	return params.cam_basis_x.xyz * p_view.x + params.cam_basis_y.xyz * p_view.y + params.cam_basis_z.xyz * p_view.z + origin;
}

vec3 view_to_world_direction(vec3 p_view) {
	return params.cam_basis_x.xyz * p_view.x + params.cam_basis_y.xyz * p_view.y + params.cam_basis_z.xyz * p_view.z;
}

// World space height of the top-most surface the height map recorded at p_world_xz. Reverse Z: a depth of 1.0
// is the near (top) plane of the top-down render, 0.0 the far (bottom) one, and everything the map never saw
// reads as the bottom, which can only ever be below the shading point's own surface.
float height_at(vec2 p_world_xz) {
	vec2 uv = (p_world_xz - params.map_origin) / params.map_size;
	return params.map_bottom + texture(height_map, uv).r * params.map_height;
}

// Jimenez et al. 2014, "Next Generation Post Processing in Call of Duty: Advanced Warfare". Rotating each
// pixel's direction set by this hides the low direction count as a fine dither that hmao_upscale.glsl's
// bilateral filter then resolves. It's a pure function of the pixel position, with no frame index mixed in,
// so the result is stable when the camera holds still - this effect has no temporal accumulation to lean on.
float interleaved_gradient_noise(vec2 p_pixel) {
	return fract(52.9829189 * fract(dot(p_pixel, vec2(0.06711056, 0.00583715))));
}

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.screen_size))) {
		return;
	}

	vec2 uv = (vec2(pos) + 0.5) / vec2(params.screen_size);
	float depth = textureLod(source_depth, uv, 0.0).x;
	float linear_depth = linearize_depth(depth);

	// Reverse Z: a depth of exactly 0.0 is the far plane, i.e. a pixel no geometry ever wrote. Nothing to
	// occlude there, and reconstructing a world position from it would place it at the horizon.
	if (depth <= 0.0) {
		imageStore(dest_image, pos, vec4(1.0, linear_depth, 0.0, 0.0));
		return;
	}

	vec3 world_pos = view_to_world(view_position(uv, linear_depth));

	// source_normal is always at native resolution, independent of the resolution this gather runs at.
	ivec2 full_res_pos = clamp(ivec2(uv * vec2(params.full_screen_size)), ivec2(0), params.full_screen_size - ivec2(1));
	vec3 normal = normalize(view_to_world_direction(normalize(imageLoad(source_normal, full_res_pos).xyz * 2.0 - 1.0)));

	vec2 map_uv = (world_pos.xz - params.map_origin) / params.map_size;

	// Distance to the nearest map border, in map-relative units, faded so that occlusion doesn't pop on and
	// off along a hard line as the map (which is centered on the camera) slides over the world.
	float border = min(min(map_uv.x, map_uv.y), min(1.0 - map_uv.x, 1.0 - map_uv.y));
	float edge_fade = smoothstep(0.0, params.edge_fade, border);
	// Below the floor of the map, every tap that falls outside it (which reads as that floor) would sit
	// above this surface and be mistaken for an occluder. Nothing this far down was in the map to begin
	// with, so there is nothing to gather for it either.
	if (edge_fade <= 0.0 || world_pos.y < params.map_bottom) {
		imageStore(dest_image, pos, vec4(1.0, linear_depth, 0.0, 0.0));
		return;
	}

	int direction_count = directions_for_quality[(params.flags >> QUALITY_SHIFT) & QUALITY_MASK];
	int step_count = steps_for_quality[(params.flags >> QUALITY_SHIFT) & QUALITY_MASK];

	// One height map texel is the finest detail the map can hold, so there is nothing to learn from taps any
	// closer than that - they would only re-read the shading point's own surface.
	float min_distance = max(params.map_size / float(textureSize(height_map, 0).x), 0.001);
	float max_distance = max(params.radius, min_distance * 2.0);
	// Exponential step spacing: taps bunch up near the shading point, where a metre of height difference is a
	// steep occluder, and spread out towards the far end, where only mountains still subtend a useful angle.
	float step_ratio = pow(max_distance / min_distance, 1.0 / float(step_count));

	float noise = interleaved_gradient_noise(vec2(pos));
	float angle_step = 2.0 * M_PI / float(direction_count);

	float occlusion = 0.0;
	for (int d = 0; d < direction_count; d++) {
		float angle = (float(d) + noise) * angle_step;
		vec2 direction = vec2(cos(angle), sin(angle));

		float direction_occlusion = 0.0;
		float distance = min_distance;

		for (int s = 0; s < step_count; s++) {
			vec2 tap_xz = world_pos.xz + direction * distance;
			distance *= step_ratio;

			float height = height_at(tap_xz);
			vec3 to_occluder = vec3(tap_xz.x - world_pos.x, height - world_pos.y - params.bias, tap_xz.y - world_pos.z);
			if (to_occluder.y <= 0.0) {
				continue; // Height field is below this surface here: it can't block any of its sky.
			}

			// cos of the angle between the surface normal and the top of the occluder. For a flat surface
			// this is the sine of the occluder's elevation angle, and squaring it gives the exact fraction of
			// a cosine-weighted hemisphere that a wedge rising to that elevation covers. Tilting the normal
			// away from the occluder (a slope's own uphill side, a wall's back face) drives it to zero, which
			// is what keeps slopes from shading themselves.
			float cos_angle = clamp(dot(normal, normalize(to_occluder)), 0.0, 1.0);
			direction_occlusion = max(direction_occlusion, cos_angle * cos_angle);
		}

		// Per direction the deepest occluder wins rather than all of them summing, so a ridge sampled several
		// times along the same direction is counted once, the way it would occlude once.
		occlusion += direction_occlusion;
	}

	occlusion /= float(direction_count);

	float visibility = 1.0 - clamp(params.amount * occlusion, 0.0, 1.0) * edge_fade;

	// The linear depth travels with the AO so the upscale pass can weigh these half resolution taps against
	// full resolution depth without re-reading (and re-linearizing) the depth buffer at this resolution.
	imageStore(dest_image, pos, vec4(visibility, linear_depth, 0.0, 0.0));
}
