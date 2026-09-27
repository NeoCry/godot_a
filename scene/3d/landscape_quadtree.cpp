/**************************************************************************/
/*  landscape_quadtree.cpp                                                */
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

#include "landscape_quadtree.h"

#include "core/object/worker_thread_pool.h"

namespace {

// Updates covering at least this many nodes of one level are split across
// worker threads; smaller ones (a brush stamp) are not worth the dispatch.
constexpr int PARALLEL_NODE_THRESHOLD = 256;

int floor_div(int p_a, int p_b) {
	return p_a >= 0 ? p_a / p_b : -((-p_a + p_b - 1) / p_b);
}

// The height the surface drawn with vertices every p_stride samples has at
// sample (p_x, p_z). Every quad is split along the diagonal from its +X corner
// to its +Z corner, like the patch mesh (and LandscapeSpline3D's terrain
// sampler). Vertices past the terrain's last sample are drawn clamped onto it,
// which is what makes the last quad of a row narrower when the terrain is not
// a multiple of the stride across.
float level_surface_height(const float *p_heights, int p_resolution, int p_quads, int p_stride, int p_x, int p_z) {
	const int x0 = (p_x / p_stride) * p_stride;
	const int z0 = (p_z / p_stride) * p_stride;
	const int x1 = MIN(x0 + p_stride, p_quads);
	const int z1 = MIN(z0 + p_stride, p_quads);
	const float fx = x1 > x0 ? float(p_x - x0) / float(x1 - x0) : 0.0f;
	const float fz = z1 > z0 ? float(p_z - z0) / float(z1 - z0) : 0.0f;

	const float h00 = p_heights[z0 * p_resolution + x0];
	const float h10 = p_heights[z0 * p_resolution + x1];
	const float h01 = p_heights[z1 * p_resolution + x0];
	const float h11 = p_heights[z1 * p_resolution + x1];
	if (fx + fz <= 1.0f) {
		return h00 + (h10 - h00) * fx + (h01 - h00) * fz;
	}
	return h11 + (h01 - h11) * (1.0f - fx) + (h10 - h11) * (1.0f - fz);
}

float aabb_distance(const Vector3 &p_point, const Vector3 &p_min, const Vector3 &p_max) {
	const float dx = float(MAX(MAX(p_min.x - p_point.x, p_point.x - p_max.x), (real_t)0.0));
	const float dy = float(MAX(MAX(p_min.y - p_point.y, p_point.y - p_max.y), (real_t)0.0));
	const float dz = float(MAX(MAX(p_min.z - p_point.z, p_point.z - p_max.z), (real_t)0.0));
	return Math::sqrt(dx * dx + dy * dy + dz * dz);
}

// Plane::is_point_over() for the box corner furthest inside the plane: if even
// that one is over it, the whole box is.
bool is_outside_plane(const Plane &p_plane, const Vector3 &p_min, const Vector3 &p_max) {
	const Vector3 inner(
			p_plane.normal.x > 0 ? p_min.x : p_max.x,
			p_plane.normal.y > 0 ? p_min.y : p_max.y,
			p_plane.normal.z > 0 ? p_min.z : p_max.z);
	return p_plane.is_point_over(inner);
}

// World size of one quad at a level, micro levels included.
float quad_size(float p_spacing, int p_level) {
	return p_level >= 0 ? p_spacing * float(1 << p_level) : p_spacing / float(1 << -p_level);
}

} // namespace

uint64_t LandscapeQuadtree::_node_key(int p_level, int p_x, int p_z) {
	return uint64_t(p_level + LEVEL_BIAS) | (uint64_t(uint32_t(p_x)) << 8) | (uint64_t(uint32_t(p_z)) << 36);
}

float *LandscapeQuadtree::_node_ptr(int p_level, int p_x, int p_z) {
	return &nodes[(level_offsets[p_level] + uint32_t(p_z * get_level_size(p_level) + p_x)) * NODE_FLOATS];
}

int LandscapeQuadtree::get_node_samples(int p_level) {
	return p_level >= 0 ? PATCH_QUADS << p_level : PATCH_QUADS >> -p_level;
}

bool LandscapeQuadtree::is_node_inside(int p_level, int p_x, int p_z) const {
	if (p_x < 0 || p_z < 0 || nodes.is_empty()) {
		return false;
	}
	const int64_t size = get_node_samples(p_level);
	return int64_t(p_x) * size < quads && int64_t(p_z) * size < quads;
}

// A level 0 node draws its samples exactly, so it has no error of its own. Its
// height range reaches one sample past its edges, which is what the micro
// levels below it read to interpolate across them.
void LandscapeQuadtree::_compute_leaf(const UpdateContext &p_context, int p_x, int p_z) {
	float *node = _node_ptr(0, p_x, p_z);
	if (!is_node_inside(0, p_x, p_z)) {
		node[0] = node[1] = node[2] = node[3] = 0.0f;
		return;
	}

	const int sx0 = p_x * PATCH_QUADS;
	const int sz0 = p_z * PATCH_QUADS;
	const int sx1 = MIN(sx0 + PATCH_QUADS, quads);
	const int sz1 = MIN(sz0 + PATCH_QUADS, quads);

	float lowest = p_context.heights[sz0 * resolution + sx0];
	float highest = lowest;
	for (int sz = MAX(sz0 - 1, 0); sz <= MIN(sz1 + 1, quads); sz++) {
		const float *row = p_context.heights + sz * resolution;
		for (int sx = MAX(sx0 - 1, 0); sx <= MIN(sx1 + 1, quads); sx++) {
			lowest = MIN(lowest, row[sx]);
			highest = MAX(highest, row[sx]);
		}
	}

	uint32_t flags = 0;
	if (p_context.holes != nullptr) {
		int hole_count = 0;
		for (int sz = sz0; sz <= sz1; sz++) {
			const uint8_t *row = p_context.holes + sz * resolution;
			for (int sx = sx0; sx <= sx1; sx++) {
				hole_count += row[sx] != 0 ? 1 : 0;
			}
		}
		if (hole_count > 0) {
			flags |= NODE_FLAG_HAS_HOLE;
		}
		if (hole_count == (sx1 - sx0 + 1) * (sz1 - sz0 + 1)) {
			flags |= NODE_FLAG_ALL_HOLE;
		}
	}

	node[0] = lowest;
	node[1] = highest;
	node[2] = 0.0f;
	node[3] = float(flags);
}

// Above level 0, the error is how far this node's surface strays from its
// children's, measured at every vertex of their grid (the only points where the
// two can differ most, since both are flat between those), plus the most any
// child strays from the heightmap itself. Adding rather than taking the larger
// makes it a true bound on the distance to the full resolution surface, which
// is what lets the screen-space test promise the popping and cracks between
// levels stay under lod_pixel_error.
//
// A node with holes in it counts at least its own quad size as error, so that
// the jagged outline its coarse vertices give a hole is refined like any other
// detail once it is close enough to show.
void LandscapeQuadtree::_compute_node(const UpdateContext &p_context, int p_x, int p_z) {
	const int level = p_context.level;
	float *node = _node_ptr(level, p_x, p_z);
	if (!is_node_inside(level, p_x, p_z)) {
		node[0] = node[1] = node[2] = node[3] = 0.0f;
		return;
	}

	float lowest = 0.0f;
	float highest = 0.0f;
	float child_error = 0.0f;
	uint32_t has_hole = 0;
	uint32_t all_hole = NODE_FLAG_ALL_HOLE;
	bool first = true;
	for (int c = 0; c < 4; c++) {
		const int cx = p_x * 2 + (c & 1);
		const int cz = p_z * 2 + (c >> 1);
		if (!is_node_inside(level - 1, cx, cz)) {
			continue;
		}
		const float *child = _node_ptr(level - 1, cx, cz);
		if (first) {
			lowest = child[0];
			highest = child[1];
			first = false;
		} else {
			lowest = MIN(lowest, child[0]);
			highest = MAX(highest, child[1]);
		}
		child_error = MAX(child_error, child[2]);
		const uint32_t child_flags = uint32_t(child[3]);
		has_hole |= child_flags & NODE_FLAG_HAS_HOLE;
		all_hole &= child_flags;
	}

	const int stride = 1 << level;
	const int half = stride >> 1;
	const int size = get_node_samples(level);
	const int sx0 = p_x * size;
	const int sz0 = p_z * size;

	float deviation = 0.0f;
	for (int a = 0; a <= 2 * PATCH_QUADS; a++) {
		const int vz = MIN(sz0 + a * half, quads);
		for (int b = 0; b <= 2 * PATCH_QUADS; b++) {
			if (((a | b) & 1) == 0) {
				continue; // One of this node's own vertices.
			}
			const int vx = MIN(sx0 + b * half, quads);
			const float surface = level_surface_height(p_context.heights, resolution, quads, stride, vx, vz);
			deviation = MAX(deviation, Math::abs(p_context.heights[vz * resolution + vx] - surface));
		}
	}

	float error = deviation + child_error;
	if (has_hole) {
		error = MAX(error, quad_size(spacing, level));
	}

	node[0] = lowest;
	node[1] = highest;
	node[2] = error;
	node[3] = float(has_hole | all_hole);
}

void LandscapeQuadtree::_compute_row(uint32_t p_row, UpdateContext *p_context) {
	const int z = p_context->range.position.y + int(p_row);
	const int x_end = p_context->range.position.x + p_context->range.size.x;
	for (int x = p_context->range.position.x; x < x_end; x++) {
		if (p_context->level == 0) {
			_compute_leaf(*p_context, x, z);
		} else {
			_compute_node(*p_context, x, z);
		}
	}
}

void LandscapeQuadtree::clear() {
	resolution = 0;
	quads = 0;
	top_level = 0;
	level_offsets.clear();
	nodes.clear();
}

void LandscapeQuadtree::build(const float *p_heights, const uint8_t *p_holes, int p_resolution, float p_spacing) {
	clear();
	ERR_FAIL_NULL(p_heights);
	ERR_FAIL_COND(p_resolution < 2);

	resolution = p_resolution;
	quads = p_resolution - 1;
	spacing = p_spacing;

	const int level0_nodes = (quads + PATCH_QUADS - 1) / PATCH_QUADS;
	while ((1 << top_level) < level0_nodes) {
		top_level++;
	}
	ERR_FAIL_COND_MSG(top_level > MAX_TOP_LEVEL, "The terrain is too large for the landscape quadtree.");

	level_offsets.resize(top_level + 1);
	uint32_t total = 0;
	for (int level = 0; level <= top_level; level++) {
		level_offsets[level] = total;
		const uint32_t size = uint32_t(get_level_size(level));
		total += size * size;
	}
	nodes.resize(total * NODE_FLOATS);
	memset(nodes.ptr(), 0, nodes.size() * sizeof(float));

	update(p_heights, p_holes, Rect2i(0, 0, resolution, resolution));
}

void LandscapeQuadtree::update(const float *p_heights, const uint8_t *p_holes, const Rect2i &p_samples, LocalVector<Vector2i> *r_changed_ranges) {
	if (nodes.is_empty() || p_heights == nullptr || p_samples.size.x <= 0 || p_samples.size.y <= 0) {
		return;
	}

	// Level 0 nodes whose height range (which reaches one sample past their
	// edges) takes in any changed sample. Every level above then needs exactly
	// the parents of the level below it: a node's error and bounds only depend
	// on its own samples and its children.
	const int grid = get_level_size(0);
	const int sx0 = p_samples.position.x;
	const int sz0 = p_samples.position.y;
	const int sx1 = p_samples.position.x + p_samples.size.x - 1;
	const int sz1 = p_samples.position.y + p_samples.size.y - 1;
	int x0 = CLAMP(floor_div(sx0 - 1, PATCH_QUADS) - 1, 0, grid - 1);
	int z0 = CLAMP(floor_div(sz0 - 1, PATCH_QUADS) - 1, 0, grid - 1);
	int x1 = CLAMP(floor_div(sx1 + 1, PATCH_QUADS), 0, grid - 1);
	int z1 = CLAMP(floor_div(sz1 + 1, PATCH_QUADS), 0, grid - 1);

	for (int level = 0; level <= top_level; level++) {
		UpdateContext context;
		context.heights = p_heights;
		context.holes = p_holes;
		context.level = level;
		context.range = Rect2i(x0, z0, x1 - x0 + 1, z1 - z0 + 1);

		const int rows = context.range.size.y;
		if (rows * context.range.size.x >= PARALLEL_NODE_THRESHOLD && rows > 1) {
			const WorkerThreadPool::GroupID group = WorkerThreadPool::get_singleton()->add_template_group_task(this, &LandscapeQuadtree::_compute_row, &context, rows, -1, true, "LandscapeQuadtree");
			WorkerThreadPool::get_singleton()->wait_for_group_task_completion(group);
		} else {
			for (int row = 0; row < rows; row++) {
				_compute_row(uint32_t(row), &context);
			}
		}

		if (r_changed_ranges != nullptr) {
			const int size = get_level_size(level);
			for (int z = z0; z <= z1; z++) {
				const int first = int(level_offsets[level]) + z * size + x0;
				const int count = x1 - x0 + 1;
				if (!r_changed_ranges->is_empty()) {
					Vector2i &last = (*r_changed_ranges)[r_changed_ranges->size() - 1];
					if (last.x + last.y == first) {
						last.y += count;
						continue;
					}
				}
				r_changed_ranges->push_back(Vector2i(first, count));
			}
		}

		x0 >>= 1;
		z0 >>= 1;
		x1 >>= 1;
		z1 >>= 1;
	}
}

// Micro levels have no data of their own: they take the height range of the
// level 0 node they lie in, widened by how far a Catmull-Rom interpolation can
// overshoot the samples it passes through (under 30% of their range, in two
// dimensions).
LandscapeQuadtree::NodeBounds LandscapeQuadtree::get_node_bounds(int p_level, int p_x, int p_z, float p_displacement_bound) const {
	NodeBounds bounds;
	ERR_FAIL_COND_V(nodes.is_empty() || p_level > top_level, bounds);

	const int size = get_node_samples(p_level);
	const int sx0 = p_x * size;
	const int sz0 = p_z * size;
	const int sx1 = MIN(sx0 + size, quads);
	const int sz1 = MIN(sz0 + size, quads);

	float lowest = 0.0f;
	float highest = 0.0f;
	if (p_level >= 0) {
		const float *node = &nodes[(level_offsets[p_level] + uint32_t(p_z * get_level_size(p_level) + p_x)) * NODE_FLOATS];
		lowest = node[0];
		highest = node[1];
		bounds.error = node[2];
		bounds.flags = uint32_t(node[3]);
	} else {
		const int shift = -p_level;
		const float *node = &nodes[(level_offsets[0] + uint32_t((p_z >> shift) * get_level_size(0) + (p_x >> shift))) * NODE_FLOATS];
		const float overshoot = (node[1] - node[0]) * 0.3f;
		lowest = node[0] - overshoot;
		highest = node[1] + overshoot;
		bounds.flags = uint32_t(node[3]) & NODE_FLAG_ALL_HOLE;
	}

	bounds.min = Vector3(float(sx0) * spacing, lowest - p_displacement_bound, float(sz0) * spacing);
	bounds.max = Vector3(float(sx1) * spacing, highest + p_displacement_bound, float(sz1) * spacing);
	return bounds;
}

bool LandscapeQuadtree::should_subdivide(const SelectParams &p_params, int p_level, int p_x, int p_z) const {
	if (p_level <= p_params.min_level) {
		return false;
	}

	const NodeBounds bounds = get_node_bounds(p_level, p_x, p_z, p_params.displacement_bound);
	const float distance = aabb_distance(p_params.camera_position, bounds.min, bounds.max);
	// Orthogonal projections draw everything the same size, near or far.
	const float divisor = p_params.orthogonal ? 1.0f : distance;

	// The heightmap's own error, projected to the screen.
	if (bounds.error * p_params.pixel_scale > p_params.pixel_error * divisor) {
		return true;
	}

	const float quad = quad_size(spacing, p_level);
	if (quad * p_params.pixel_scale > p_params.max_quad_pixels * divisor) {
		return true;
	}

	return p_params.micro_distance > 0.0f && distance < p_params.micro_distance && quad * p_params.pixel_scale > p_params.micro_quad_pixels * divisor;
}

// The patch across one edge is either no coarser than this one (then it is the
// one fitting its edge to this one, or they already match), or it is the
// coarser node that contains the point just past this edge. Every node the
// selection visited and split is recorded, so walking down from the root
// towards that point finds it as the first one that was not split.
int LandscapeQuadtree::_coarser_neighbor(const Patch &p_patch, int p_dx, int p_dz) const {
	const int nx = p_patch.x + p_dx;
	const int nz = p_patch.z + p_dz;
	if (!is_node_inside(p_patch.level, nx, nz)) {
		return 0;
	}
	for (int level = top_level; level > p_patch.level; level--) {
		const int shift = level - p_patch.level;
		if (!select_subdivided.has(_node_key(level, nx >> shift, nz >> shift))) {
			return MIN(level - p_patch.level, MAX_EDGE_DELTA);
		}
	}
	return 0;
}

void LandscapeQuadtree::select(const SelectParams &p_params, Selection &r_selection) const {
	r_selection.visible.clear();
	r_selection.shadow.clear();
	if (nodes.is_empty()) {
		return;
	}

	select_stack.clear();
	select_leaves.clear();
	select_subdivided.clear();

	Patch root;
	root.level = top_level;
	select_stack.push_back(root);
	while (!select_stack.is_empty()) {
		const Patch node = select_stack[select_stack.size() - 1];
		select_stack.resize(select_stack.size() - 1);
		if (!is_node_inside(node.level, node.x, node.z)) {
			continue;
		}
		if (!should_subdivide(p_params, node.level, node.x, node.z)) {
			select_leaves.push_back(node);
			continue;
		}
		select_subdivided.insert(_node_key(node.level, node.x, node.z));
		for (int c = 0; c < 4; c++) {
			Patch child;
			child.level = node.level - 1;
			child.x = node.x * 2 + (c & 1);
			child.z = node.z * 2 + (c >> 1);
			select_stack.push_back(child);
		}
	}

	for (Patch &leaf : select_leaves) {
		const NodeBounds bounds = get_node_bounds(leaf.level, leaf.x, leaf.z, p_params.displacement_bound);
		if (bounds.flags & NODE_FLAG_ALL_HOLE) {
			continue;
		}

		leaf.edges = uint32_t(_coarser_neighbor(leaf, -1, 0)) |
				(uint32_t(_coarser_neighbor(leaf, 1, 0)) << 4) |
				(uint32_t(_coarser_neighbor(leaf, 0, -1)) << 8) |
				(uint32_t(_coarser_neighbor(leaf, 0, 1)) << 12);

		bool in_view = true;
		if (p_params.frustum_culling) {
			for (int i = 0; i < 6; i++) {
				if (is_outside_plane(p_params.frustum[i], bounds.min, bounds.max)) {
					in_view = false;
					break;
				}
			}
		}
		if (in_view) {
			r_selection.visible.push_back(leaf);
		}

		if (p_params.shadows && (p_params.shadow_distance <= 0.0f || aabb_distance(p_params.camera_position, bounds.min, bounds.max) <= p_params.shadow_distance)) {
			r_selection.shadow.push_back(leaf);
		}
	}
}

void LandscapeQuadtree::write_instance(const Patch &p_patch, float *r_instance) {
	// Identity rows: the vertices are placed in terrain space by the shader.
	static const float identity[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
	memcpy(r_instance, identity, sizeof(identity));

	const int size = get_node_samples(p_patch.level);
	const int ox = p_patch.x * size;
	const int oz = p_patch.z * size;
	r_instance[12] = float(ox / ORIGIN_SPLIT);
	r_instance[13] = float(ox % ORIGIN_SPLIT);
	r_instance[14] = float(oz / ORIGIN_SPLIT);
	r_instance[15] = float(oz % ORIGIN_SPLIT);
	r_instance[16] = float(p_patch.level + LEVEL_BIAS);
	r_instance[17] = float(p_patch.edges & 0xff);
	r_instance[18] = float((p_patch.edges >> 8) & 0xff);
	r_instance[19] = 0.0f;
}
