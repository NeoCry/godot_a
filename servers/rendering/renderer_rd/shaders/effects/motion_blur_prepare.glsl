/**************************************************************************/
/*  motion_blur_prepare.glsl                                              */
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

// First pass of the motion blur of Guertin, McGuire & Nowrouzezahrai, "A Fast and Stable Feature-Aware
// Motion Blur Filter" (HPG 2014): builds the velocity buffer V and the linear depth buffer Z the other
// passes read, at the resolution of the color buffer being blurred, and copies that color buffer, since
// the reconstruction filter writes its result back into it.
//
// V follows the paper (and McGuire et al. 2012 before it): half the screen space distance a pixel moved
// over the exposure, since the blur extends that far to either side of it, in pixels and clamped to the
// maximum blur radius r, which is also the tile size. The renderer's motion vectors are split into the
// camera's rotation, the camera's movement and the objects' own motion, so each can be scaled on its own.

#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source_color;
layout(set = 0, binding = 1) uniform sampler2D source_velocity; // Previous minus current UV, at internal resolution.
layout(set = 0, binding = 2) uniform sampler2D source_depth; // Hardware depth (reverse Z), at internal resolution.

layout(rgba16f, set = 1, binding = 0) uniform restrict writeonly image2D dest_color;
layout(rgba16f, set = 1, binding = 1) uniform restrict writeonly image2D dest_velocity_depth;

#define MAX_VIEWS 2

struct ViewData {
	// Current NDC (Z in [-1, 1], reversed) to previous NDC, for the whole camera motion and for its rotation
	// (and projection change) alone.
	mat4 reprojection;
	mat4 reprojection_rotation;
	// Current NDC to view space.
	mat4 inv_projection;
};

layout(set = 2, binding = 0, std140) uniform ViewDataBlock {
	ViewData data[MAX_VIEWS];
}
views;

layout(push_constant, std430) uniform Params {
	ivec2 size;
	ivec2 source_size;

	float velocity_scale;
	float max_radius;
	float camera_rotation_scale;
	float camera_movement_scale;

	float object_scale;
	uint view;
	float max_depth;
	float pad;
}
params;

// Where the point of this pixel was on screen in the previous frame, as a UV offset from where it is now.
vec2 reproject(mat4 p_reprojection, vec4 p_ndc, vec2 p_uv) {
	vec4 previous = p_reprojection * p_ndc;
	if (previous.w <= 0.0) {
		// It was behind the camera: there is no meaningful screen space motion to blur along.
		return vec2(0.0);
	}
	return (previous.xy / previous.w) * 0.5 + 0.5 - p_uv;
}

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.size))) {
		return;
	}

	imageStore(dest_color, pos, texelFetch(source_color, pos, 0));

	// The motion vectors and depth may be at a lower (internal) resolution than the color buffer, when a
	// temporal upscaler produced it.
	ivec2 source_pos = clamp(ivec2((vec2(pos) + 0.5) * vec2(params.source_size) / vec2(params.size)), ivec2(0), params.source_size - 1);
	float depth = texelFetch(source_depth, source_pos, 0).r;
	vec2 velocity = texelFetch(source_velocity, source_pos, 0).xy;

	vec2 uv = (vec2(pos) + 0.5) / vec2(params.size);
	vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);

	vec2 camera_velocity = reproject(views.data[params.view].reprojection, ndc, uv);
	vec2 rotation_velocity = reproject(views.data[params.view].reprojection_rotation, ndc, uv);

	// Where nothing was drawn (the sky) the motion vectors keep their cleared value, and when a temporal
	// upscaler derives the camera's motion itself, only moving objects write theirs and the rest are marked
	// with -1. Either way, the camera's motion is all there is.
	bool camera_only = depth == 0.0 || all(lessThanEqual(velocity, vec2(-1.0)));
	vec2 object_velocity = camera_only ? vec2(0.0) : velocity - camera_velocity;

	vec2 v = rotation_velocity * params.camera_rotation_scale + (camera_velocity - rotation_velocity) * params.camera_movement_scale + object_velocity * params.object_scale;

	// To pixels, over the exposure, and halved.
	v *= vec2(params.size) * params.velocity_scale;
	if (any(isnan(v)) || any(isinf(v))) {
		v = vec2(0.0);
	}
	float v_length = length(v);
	if (v_length > params.max_radius) {
		v *= params.max_radius / v_length;
	}

	vec4 view_position = views.data[params.view].inv_projection * ndc;
	float z = clamp(-view_position.z / view_position.w, 1e-4, params.max_depth);
	if (isnan(z)) {
		z = params.max_depth;
	}

	imageStore(dest_velocity_depth, pos, vec4(v, z, 0.0));
}
