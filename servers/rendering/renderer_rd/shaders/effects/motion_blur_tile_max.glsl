/**************************************************************************/
/*  motion_blur_tile_max.glsl                                             */
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

// TileMax of Guertin et al. 2014 (and McGuire et al. 2012): the dominant velocity, the one with the largest
// magnitude, of each r x r tile of the velocity buffer. Run separably, as a 1 x r reduction of each row
// (MODE_TILE_MAX_X) and then an r x 1 reduction of each column of that (MODE_TILE_MAX_Y), so that every
// invocation only reads r texels, however large the tiles are.

#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source_velocity;

layout(rg16f, set = 1, binding = 0) uniform restrict writeonly image2D dest_tile_max;

layout(push_constant, std430) uniform Params {
	ivec2 source_size;
	ivec2 dest_size;

	int tile_size;
	int pad[3];
}
params;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.dest_size))) {
		return;
	}

#ifdef MODE_TILE_MAX_Y
	ivec2 base = ivec2(pos.x, pos.y * params.tile_size);
	ivec2 stride = ivec2(0, 1);
	int count = min(params.tile_size, params.source_size.y - base.y);
#else
	ivec2 base = ivec2(pos.x * params.tile_size, pos.y);
	ivec2 stride = ivec2(1, 0);
	int count = min(params.tile_size, params.source_size.x - base.x);
#endif

	vec2 dominant = vec2(0.0);
	float dominant_length_squared = 0.0;
	for (int i = 0; i < count; i++) {
		vec2 v = texelFetch(source_velocity, base + stride * i, 0).xy;
		float length_squared = dot(v, v);
		if (length_squared > dominant_length_squared) {
			dominant = v;
			dominant_length_squared = length_squared;
		}
	}

	imageStore(dest_tile_max, pos, vec4(dominant, 0.0, 0.0));
}
