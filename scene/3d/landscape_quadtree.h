/**************************************************************************/
/*  landscape_quadtree.h                                                  */
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

#pragma once

#include "core/math/plane.h"
#include "core/math/rect2i.h"
#include "core/math/vector3.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"

// The level of detail structure Landscape3D renders its heightmap through: a
// quadtree over the terrain whose every node, whatever its size, is drawn as
// the same PATCH_QUADS x PATCH_QUADS grid of quads. A node one level up covers
// twice the ground in each direction at half the density, so level 0 draws the
// heightmap exactly, one quad per sample, and the single root node draws all of
// it in PATCH_QUADS^2 quads. Below level 0, "micro" levels keep halving the quad
// size past what the heightmap stores: their vertices take a smooth
// (Catmull-Rom) interpolation of it, plus whatever displacement the painted
// TerrainLayers add (see TerrainLayer.displacement).
//
// Which nodes are drawn is decided every frame by screen-space error: every
// level from 1 up stores how far, at most, its surface strays from the full
// resolution heightmap (plus any holes it would blur), and a node is split into
// its four children as long as that error, projected to the screen at the
// node's distance from the camera, is larger than Landscape3D.lod_pixel_error.
// Flat ground is drawn with few, large patches however close it is, rugged
// ground with many small ones even far away. Two more rules split nodes the
// error alone would not: quads are kept from growing past
// Landscape3D.lod_max_quad_pixels on screen, and within
// Landscape3D.micro_detail_distance quads are split down to about
// micro_detail_triangle_size pixels so that there are vertices to displace.
//
// The selection runs on the GPU (see LandscapeGPUQuadtree), which reads this
// node data from a buffer; select() below is the same algorithm on the CPU, for
// renderers without compute shaders and for tests. Neither reads anything the
// camera's orientation decides except frustum culling, so turning the camera
// never changes the tessellation, only which of it is drawn.
//
// Neighboring patches can be several levels apart. Each selected patch records,
// per edge, how much coarser the patch across it is, and the terrain shader
// moves the vertices along that edge onto the coarser patch's own vertices,
// which keeps the surface watertight without skirts or T-junctions.
class LandscapeQuadtree {
public:
	// Quads along each edge of every patch.
	static constexpr int PATCH_QUADS = 16;
	static constexpr int PATCH_SHIFT = 4;
	// The finest micro level makes a patch exactly one heightmap quad wide, so
	// every node origin stays on a whole sample.
	static constexpr int MAX_MICRO_LEVELS = PATCH_SHIFT;
	// Added to a level to store it as a non-negative number.
	static constexpr int LEVEL_BIAS = MAX_MICRO_LEVELS;
	// TerrainData::MAX_RESOLUTION (4097) needs 256 level 0 nodes a side.
	static constexpr int MAX_TOP_LEVEL = 8;
	static constexpr int MAX_LEVEL_COUNT = MAX_TOP_LEVEL + 1 + MAX_MICRO_LEVELS;

	// Per node: lowest height, highest height, screen-space error source (in
	// world units), flags.
	static constexpr int NODE_FLOATS = 4;
	static constexpr uint32_t NODE_FLAG_HAS_HOLE = 1;
	static constexpr uint32_t NODE_FLAG_ALL_HOLE = 2;

	// How much coarser a neighbor can be recorded as, per edge (4 bits).
	static constexpr int MAX_EDGE_DELTA = 15;

	// Every drawn patch is one MultiMesh instance: an identity transform (the
	// shader places the vertices itself), then the instance color and custom
	// data carrying the patch. The Compatibility renderer stores both of those
	// as half floats, which hold whole numbers exactly only up to 2048, so the
	// origin is split into multiples of ORIGIN_SPLIT and a remainder.
	static constexpr int INSTANCE_FLOATS = 20;
	static constexpr int ORIGIN_SPLIT = 1024;

	struct SelectParams {
		// Terrain local space.
		Vector3 camera_position;
		bool orthogonal = false;
		// How many pixels one unit spans at a distance of one unit
		// (perspective), or at any distance (orthogonal).
		float pixel_scale = 1.0f;
		float pixel_error = 2.0f;
		float max_quad_pixels = 48.0f;
		// Zero, or minus the number of micro levels.
		int min_level = 0;
		// Zero turns micro detail subdivision off.
		float micro_distance = 0.0f;
		float micro_quad_pixels = 6.0f;
		// Most any displacement can move the surface up or down.
		float displacement_bound = 0.0f;
		bool frustum_culling = false;
		// Terrain local space, normals pointing out.
		Plane frustum[6];
		bool shadows = false;
		// Zero means no limit.
		float shadow_distance = 0.0f;
	};

	struct NodeBounds {
		Vector3 min;
		Vector3 max;
		float error = 0.0f;
		uint32_t flags = 0;
	};

	struct Patch {
		int32_t level = 0;
		int32_t x = 0;
		int32_t z = 0;
		// How many levels coarser the neighbor across each edge is, 4 bits
		// each: -X, +X, -Z, +Z.
		uint32_t edges = 0;
	};

	struct Selection {
		LocalVector<Patch> visible;
		LocalVector<Patch> shadow;
	};

private:
	int resolution = 0;
	int quads = 0;
	float spacing = 1.0f;
	int top_level = 0;
	LocalVector<uint32_t> level_offsets;
	LocalVector<float> nodes;

	struct UpdateContext {
		const float *heights = nullptr;
		const uint8_t *holes = nullptr;
		int level = 0;
		Rect2i range;
	};

	// Scratch space for select(), kept to avoid reallocating every frame.
	mutable LocalVector<Patch> select_stack;
	mutable LocalVector<Patch> select_leaves;
	mutable HashSet<uint64_t> select_subdivided;

	static uint64_t _node_key(int p_level, int p_x, int p_z);
	float *_node_ptr(int p_level, int p_x, int p_z);
	void _compute_leaf(const UpdateContext &p_context, int p_x, int p_z);
	void _compute_node(const UpdateContext &p_context, int p_x, int p_z);
	void _compute_row(uint32_t p_row, UpdateContext *p_context);
	int _coarser_neighbor(const Patch &p_patch, int p_dx, int p_dz) const;

public:
	// Rebuilds every node from scratch. p_holes may be null.
	void build(const float *p_heights, const uint8_t *p_holes, int p_resolution, float p_spacing);
	// Refreshes every node the given samples (inclusive rectangle, in
	// samples) reach, and adds the ranges of node indices it rewrote to
	// r_changed_ranges as (first index, count).
	void update(const float *p_heights, const uint8_t *p_holes, const Rect2i &p_samples, LocalVector<Vector2i> *r_changed_ranges = nullptr);
	void clear();
	bool is_empty() const { return nodes.is_empty(); }

	int get_resolution() const { return resolution; }
	float get_spacing() const { return spacing; }
	int get_top_level() const { return top_level; }
	// Nodes per axis at a level of 0 or more (the grid is padded to a power of
	// two; see is_node_inside()).
	int get_level_size(int p_level) const { return 1 << (top_level - p_level); }
	uint32_t get_level_offset(int p_level) const { return level_offsets[p_level]; }
	uint32_t get_node_count() const { return nodes.size() / NODE_FLOATS; }
	const LocalVector<float> &get_node_data() const { return nodes; }

	// Samples a node spans along each axis; one or more at every level.
	static int get_node_samples(int p_level);
	// Whether the node has any ground inside the terrain at all.
	bool is_node_inside(int p_level, int p_x, int p_z) const;
	NodeBounds get_node_bounds(int p_level, int p_x, int p_z, float p_displacement_bound = 0.0f) const;
	bool should_subdivide(const SelectParams &p_params, int p_level, int p_x, int p_z) const;

	void select(const SelectParams &p_params, Selection &r_selection) const;

	// Fills INSTANCE_FLOATS floats.
	static void write_instance(const Patch &p_patch, float *r_instance);
};
