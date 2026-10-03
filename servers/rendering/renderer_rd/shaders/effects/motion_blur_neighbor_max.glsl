/**************************************************************************/
/*  motion_blur_neighbor_max.glsl                                         */
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

// NeighborMax of Guertin et al. 2014: the dominant velocity among each tile and its 8 neighbors, which bounds
// how far, and mainly along which direction, anything can blur into the tile. As the paper improves on McGuire
// et al. 2012, a diagonal neighbor only takes part when its velocity runs along the diagonal (within 45
// degrees of it), since only then can its blur, at most one tile long, cross into this tile, which it touches
// at a single corner. Otherwise a fast object would spread its blur direction over a 3 x 3 tile square around
// it rather than along the path it actually sweeps.

#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source_tile_max;

layout(rg16f, set = 1, binding = 0) uniform restrict writeonly image2D dest_neighbor_max;

layout(push_constant, std430) uniform Params {
	ivec2 size;
	ivec2 pad;
}
params;

#define COS_45 0.70710678118

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.size))) {
		return;
	}

	vec2 dominant = vec2(0.0);
	float dominant_length_squared = 0.0;
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			ivec2 tile = pos + ivec2(x, y);
			if (any(lessThan(tile, ivec2(0))) || any(greaterThanEqual(tile, params.size))) {
				continue;
			}

			vec2 v = texelFetch(source_tile_max, tile, 0).xy;
			float length_squared = dot(v, v);
			if (length_squared <= dominant_length_squared) {
				continue;
			}

			// The blur is symmetric, so a velocity pointing away from this tile along the diagonal reaches it as
			// well as one pointing towards it.
			if (x != 0 && y != 0 && abs(dot(v, vec2(x, y) * COS_45)) <= COS_45 * sqrt(length_squared)) {
				continue;
			}

			dominant = v;
			dominant_length_squared = length_squared;
		}
	}

	imageStore(dest_neighbor_max, pos, vec4(dominant, 0.0, 0.0));
}
