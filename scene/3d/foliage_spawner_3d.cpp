/**************************************************************************/
/*  foliage_spawner_3d.cpp                                                */
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

#include "foliage_spawner_3d.h"

#include "core/core_string_names.h"
#include "core/io/image.h"
#include "core/math/face3.h"
#include "core/math/math_funcs.h"
#include "core/math/random_pcg.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/landscape_3d.h"
#include "scene/3d/landscape_spline_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/terrain_data.h"
#include "scene/3d/terrain_layer.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "scene/resources/multimesh.h"
#include "scene/resources/texture.h"

namespace {

// Where a Landscape3D has ground to stand on: within its edges, and not over a
// hole, which Landscape3D cuts as every quad with a hole sample at any of its
// corners. Reads TerrainData's hole map directly rather than sample by sample.
struct TerrainGroundSampler {
	Vector<uint8_t> holes; // Keeps the buffer alive and unchanged.
	const uint8_t *hole_data = nullptr;
	int resolution = 0;
	float spacing = 1.0f;

	bool setup(const Ref<TerrainData> &p_data) {
		resolution = p_data->get_resolution();
		spacing = p_data->get_vertex_spacing();
		const Ref<Image> hole_map = p_data->get_hole_map_image();
		if (resolution < 2 || hole_map.is_null()) {
			return false;
		}
		holes = hole_map->get_data();
		if (holes.size() < (int64_t)resolution * resolution) {
			return false;
		}
		hole_data = holes.ptr();
		return true;
	}

	// p_xz in the landscape's local space.
	bool has_ground_at(const Vector2 &p_xz) const {
		const float fx = p_xz.x / spacing;
		const float fz = p_xz.y / spacing;
		// A sliver of tolerance, so that a volume fitted to the terrain (see
		// fit_to_ground_mesh()) does not lose candidates on its very edge to
		// rounding.
		const float last = (float)(resolution - 1) + 0.001f;
		if (!(fx >= -0.001f && fz >= -0.001f && fx <= last && fz <= last)) {
			return false;
		}
		const int ix = CLAMP((int)fx, 0, resolution - 2);
		const int iz = CLAMP((int)fz, 0, resolution - 2);
		const uint8_t *quad = hole_data + (size_t)iz * resolution + ix;
		return quad[0] == 0 && quad[1] == 0 && quad[resolution] == 0 && quad[resolution + 1] == 0;
	}
};

// How much of the ground the chosen TerrainLayers cover at a point: their
// share of the weight every layer the Landscape3D renders holds there. Filtered
// between samples the way Landscape3D's shader filters its weight maps (each
// weight bilinearly, then shared out), so foliage follows what the ground
// looks like. Reads TerrainData's weight maps directly rather than sample by
// sample.
struct TerrainLayerSampler {
	Vector<uint8_t> maps[TerrainData::WEIGHT_MAP_COUNT]; // Keep the buffers alive and unchanged.
	const uint8_t *map_data[TerrainData::WEIGHT_MAP_COUNT] = {};
	// Per channel of each weight map: whether its layer is one the landscape
	// renders, and whether it is one of the chosen ones.
	bool rendered[TerrainData::WEIGHT_MAP_COUNT][TerrainData::LAYERS_PER_WEIGHT_MAP] = {};
	bool chosen[TerrainData::WEIGHT_MAP_COUNT][TerrainData::LAYERS_PER_WEIGHT_MAP] = {};
	int map_count = 0;
	int resolution = 0;
	float spacing = 1.0f;

	bool setup(const Ref<TerrainData> &p_data, int p_layer_count, uint32_t p_mask) {
		resolution = p_data->get_resolution();
		spacing = p_data->get_vertex_spacing();
		// Only the layers the landscape has are rendered; weight painted for
		// any past those never shows, so it has no say here either.
		const int layer_count = CLAMP(p_layer_count, 0, TerrainData::MAX_LAYERS);
		map_count = (layer_count + TerrainData::LAYERS_PER_WEIGHT_MAP - 1) / TerrainData::LAYERS_PER_WEIGHT_MAP;
		if (map_count == 0 || resolution < 2) {
			return false;
		}
		for (int g = 0; g < map_count; g++) {
			const Ref<Image> image = p_data->get_weight_map_image(g);
			if (image.is_null()) {
				return false;
			}
			maps[g] = image->get_data();
			if (maps[g].size() < (int64_t)resolution * resolution * TerrainData::LAYERS_PER_WEIGHT_MAP) {
				return false;
			}
			map_data[g] = maps[g].ptr();
			for (int c = 0; c < TerrainData::LAYERS_PER_WEIGHT_MAP; c++) {
				const int layer = g * TerrainData::LAYERS_PER_WEIGHT_MAP + c;
				rendered[g][c] = layer < layer_count;
				chosen[g][c] = layer < layer_count && (p_mask & (1u << layer)) != 0;
			}
		}
		return true;
	}

	// p_xz in the landscape's local space. 0 where no layer holds any weight.
	float share_at(const Vector2 &p_xz) const {
		const float fx = CLAMP(p_xz.x / spacing, 0.0f, (float)(resolution - 1));
		const float fz = CLAMP(p_xz.y / spacing, 0.0f, (float)(resolution - 1));
		const int ix = MIN((int)fx, resolution - 2);
		const int iz = MIN((int)fz, resolution - 2);
		const float tx = fx - ix;
		const float tz = fz - iz;

		// At each corner of the quad the point is in: the chosen layers'
		// weight, and every rendered layer's.
		const size_t row = (size_t)resolution * TerrainData::LAYERS_PER_WEIGHT_MAP;
		const size_t corner_offsets[4] = { 0, TerrainData::LAYERS_PER_WEIGHT_MAP, row, row + TerrainData::LAYERS_PER_WEIGHT_MAP };
		float chosen_weight[4] = {};
		float total_weight[4] = {};
		for (int g = 0; g < map_count; g++) {
			const uint8_t *quad = map_data[g] + ((size_t)iz * resolution + ix) * TerrainData::LAYERS_PER_WEIGHT_MAP;
			for (int corner = 0; corner < 4; corner++) {
				const uint8_t *weights = quad + corner_offsets[corner];
				for (int c = 0; c < TerrainData::LAYERS_PER_WEIGHT_MAP; c++) {
					if (rendered[g][c]) {
						total_weight[corner] += weights[c];
						if (chosen[g][c]) {
							chosen_weight[corner] += weights[c];
						}
					}
				}
			}
		}

		const float chosen_here = Math::lerp(Math::lerp(chosen_weight[0], chosen_weight[1], tx), Math::lerp(chosen_weight[2], chosen_weight[3], tx), tz);
		const float total_here = Math::lerp(Math::lerp(total_weight[0], total_weight[1], tx), Math::lerp(total_weight[2], total_weight[3], tx), tz);
		return total_here > 0.0f ? chosen_here / total_here : 0.0f;
	}
};

// Whether the segment from p_from to p_to crosses the one from p_a to p_b.
// Half-open about which side of a line a point exactly on it counts as being
// on, so that where a path passes exactly through a vertex of an outline it
// crosses one of the outline's two edges there, never both or neither.
bool segments_cross(const Vector2 &p_from, const Vector2 &p_to, const Vector2 &p_a, const Vector2 &p_b) {
	const Vector2 path = p_to - p_from;
	if ((path.cross(p_a - p_from) > 0.0f) == (path.cross(p_b - p_from) > 0.0f)) {
		return false;
	}
	const Vector2 edge = p_b - p_a;
	return (edge.cross(p_from - p_a) > 0.0f) != (edge.cross(p_to - p_a) > 0.0f);
}

// A LandscapeSpline3D's ground coverage (see LandscapeSpline3D::GroundCoverage),
// set up for testing a great many points against: flattened onto the plane
// square to the spline's up axis, and its edges sorted into a grid of cells
// there, so each point is measured against the few edges near it instead of the
// whole spline.
struct SplineKeepOut {
	Vector3 up;
	Vector3 axis_x;
	Vector3 axis_y;
	bool filled = false;
	// Fills only: the water's surface, along up.
	float level = 0.0f;
	float margin = 0.0f;
	LocalVector<Vector2> points;
	// Strips only: how far from the centerline to keep off at each point, the
	// strip's half width plus the margin.
	LocalVector<float> reach;

	// The grid covers the part of the coverage (grown by its reach) that lies
	// within the volume. For each cell, the edges within reach of it - each by
	// the index of the point it starts at - lie in cell_edges from
	// cell_starts[cell] up to cell_starts[cell + 1].
	float cell_size = 1.0f;
	Vector2i grid_origin;
	Vector2i grid_size;
	LocalVector<uint32_t> cell_starts;
	LocalVector<uint32_t> cell_edges;
	// Fills only: whether the center of each cell lies within the shoreline. A
	// point then only has to count the edges between it and its cell's center
	// to know whether it does too.
	LocalVector<uint8_t> inside;
	// One bit per cell: whether anything in it can be covered at all, i.e.
	// whether any edge reaches into it or, for a fill, its center is inside.
	// Most points are nowhere near the spline, and this turns them away
	// without reading anything as big as cell_starts, which a great many
	// points scattered over a whole terrain would each miss the cache on.
	LocalVector<uint64_t> occupied;

	Vector2 flatten(const Vector3 &p_global) const {
		return Vector2(axis_x.dot(p_global), axis_y.dot(p_global));
	}

	uint32_t get_edge_count() const {
		// A shoreline closes back on its first point; a strip does not.
		return filled ? points.size() : points.size() - 1;
	}

	const Vector2 &get_edge_end(uint32_t p_edge) const {
		return points[p_edge + 1 < points.size() ? p_edge + 1 : 0];
	}

	Vector2i get_cell(const Vector2 &p_point) const {
		return Vector2i((int)Math::floor(p_point.x / cell_size), (int)Math::floor(p_point.y / cell_size));
	}

	float get_distance_squared_to_edge(const Vector2 &p_point, uint32_t p_edge, float &r_t) const {
		const Vector2 &a = points[p_edge];
		const Vector2 along = get_edge_end(p_edge) - a;
		const float length_squared = along.length_squared();
		r_t = length_squared > CMP_EPSILON2 ? CLAMP((p_point - a).dot(along) / length_squared, 0.0f, 1.0f) : 0.0f;
		return p_point.distance_squared_to(a + along * r_t);
	}

	// p_volume_corners: the eight global corners of the volume to fill, which
	// nothing is tested outside of.
	bool setup(const LandscapeSpline3D::GroundCoverage &p_coverage, float p_margin, const Vector3 *p_volume_corners) {
		up = p_coverage.up;
		// The same plane axes LandscapeSpline3D lays a filled area out on.
		axis_x = Vector3(1, 0, 0) - up * up.x;
		if (axis_x.length_squared() < 0.0001f) {
			axis_x = Vector3(0, 0, 1) - up * up.z;
		}
		axis_x.normalize();
		axis_y = axis_x.cross(up).normalized();
		filled = p_coverage.filled;
		level = p_coverage.level;
		margin = MAX(p_margin, 0.0f);

		const uint32_t count = p_coverage.points.size();
		if (count < (filled ? 3u : 2u)) {
			return false;
		}
		points.resize(count);
		for (uint32_t i = 0; i < count; i++) {
			points[i] = flatten(p_coverage.points[i]);
		}
		float widest = margin;
		if (!filled) {
			reach.resize(count);
			for (uint32_t i = 0; i < count; i++) {
				reach[i] = MAX(p_coverage.half_widths[i], 0.0f) + margin;
				widest = MAX(widest, reach[i]);
			}
		}
		const uint32_t edge_count = get_edge_count();
		float longest = 0.0f;
		for (uint32_t e = 0; e < edge_count; e++) {
			longest = MAX(longest, points[e].distance_to(get_edge_end(e)));
		}

		Rect2 bounds(points[0], Vector2());
		for (const Vector2 &point : points) {
			bounds.expand_to(point);
		}
		bounds = bounds.grow(widest);
		Rect2 volume(flatten(p_volume_corners[0]), Vector2());
		for (int i = 1; i < 8; i++) {
			volume.expand_to(flatten(p_volume_corners[i]));
		}
		if (!bounds.intersects(volume, true)) {
			return false;
		}
		const Rect2 area = bounds.intersection(volume);

		// Cells about as wide as what one edge reaches, so an edge lands in a
		// handful of them and a cell holds a handful of edges. Wider when that
		// would make many more cells than there are edges to fill them (a
		// winding road spread over a whole terrain), with enough of them left to
		// tell a filled area's inside from its outside at a useful resolution.
		const double max_cells = MAX(4.0 * edge_count, 65536.0);
		cell_size = MAX(MAX(widest, longest) * 2.0f, 0.5f);
		cell_size = MAX(cell_size, (float)Math::sqrt((double)area.size.x * (double)area.size.y / max_cells));
		grid_origin = get_cell(area.position);
		grid_size = get_cell(area.get_end()) - grid_origin + Vector2i(1, 1);
		const uint32_t cell_count = grid_size.x * grid_size.y;

		// Twice over the edges: once to count how many land in each cell, and
		// once to put them there.
		cell_starts.resize_initialized(cell_count + 1);
		LocalVector<uint32_t> cell_ends;
		for (int pass = 0; pass < 2; pass++) {
			for (uint32_t e = 0; e < edge_count; e++) {
				const float edge_reach = filled ? margin : MAX(reach[e], reach[e + 1]);
				const Rect2 edge_bounds = Rect2(points[e], Vector2()).expand(get_edge_end(e)).grow(edge_reach);
				const Vector2i from = (get_cell(edge_bounds.position) - grid_origin).maxi(0);
				const Vector2i to = (get_cell(edge_bounds.get_end()) - grid_origin).min(grid_size - Vector2i(1, 1));
				for (int y = from.y; y <= to.y; y++) {
					for (int x = from.x; x <= to.x; x++) {
						const uint32_t cell = y * grid_size.x + x;
						if (pass == 0) {
							cell_starts[cell + 1]++;
						} else {
							cell_edges[cell_ends[cell]++] = e;
						}
					}
				}
			}
			if (pass == 0) {
				for (uint32_t cell = 0; cell < cell_count; cell++) {
					cell_starts[cell + 1] += cell_starts[cell];
				}
				cell_edges.resize(cell_starts[cell_count]);
				cell_ends.resize(cell_count);
				for (uint32_t cell = 0; cell < cell_count; cell++) {
					cell_ends[cell] = cell_starts[cell];
				}
			}
		}

		if (filled) {
			// Each row of cell centers against the shoreline, from where the
			// row crosses it (as LandscapeSpline3D tells which of a lake's
			// tiles are inside it).
			inside.resize(cell_count);
			LocalVector<float> crossings;
			for (int row = 0; row < grid_size.y; row++) {
				const float y = (grid_origin.y + row + 0.5f) * cell_size;
				crossings.clear();
				for (uint32_t e = 0; e < edge_count; e++) {
					const Vector2 &a = points[e];
					const Vector2 &b = get_edge_end(e);
					if ((a.y <= y) != (b.y <= y)) {
						crossings.push_back(a.x + (y - a.y) / (b.y - a.y) * (b.x - a.x));
					}
				}
				crossings.sort();
				uint32_t passed = 0;
				for (int column = 0; column < grid_size.x; column++) {
					const float x = (grid_origin.x + column + 0.5f) * cell_size;
					while (passed < crossings.size() && crossings[passed] < x) {
						passed++;
					}
					inside[row * grid_size.x + column] = passed % 2;
				}
			}
		}

		occupied.resize_initialized((cell_count + 63) / 64);
		for (uint32_t cell = 0; cell < cell_count; cell++) {
			if (cell_starts[cell + 1] > cell_starts[cell] || (filled && inside[cell] != 0)) {
				occupied[cell / 64] |= uint64_t(1) << (cell % 64);
			}
		}
		return true;
	}

	// Whether p_global (an instance's place on the ground) is somewhere the
	// spline keeps foliage off: on a strip, or under a filled area's water,
	// or within margin of either.
	bool covers(const Vector3 &p_global) const {
		const Vector2 point = flatten(p_global);
		const Vector2i cell = get_cell(point) - grid_origin;
		if (cell.x < 0 || cell.y < 0 || cell.x >= grid_size.x || cell.y >= grid_size.y) {
			return false;
		}
		const uint32_t index = cell.y * grid_size.x + cell.x;
		if ((occupied[index / 64] & (uint64_t(1) << (index % 64))) == 0) {
			return false;
		}
		const uint32_t first = cell_starts[index];
		const uint32_t last = cell_starts[index + 1];
		float t = 0.0f;

		if (!filled) {
			// Against the centerline, reaching as far as the strip is wide
			// there: the same measure apply_to_landscape() carves and paints by.
			for (uint32_t i = first; i < last; i++) {
				const uint32_t e = cell_edges[i];
				const float distance_squared = get_distance_squared_to_edge(point, e, t);
				const float edge_reach = Math::lerp(reach[e], reach[e + 1], t);
				if (distance_squared <= edge_reach * edge_reach) {
					return true;
				}
			}
			return false;
		}

		// Under the water: within the shoreline, and below the surface. Not
		// just within the shoreline, since a lake drawn around a hollow rather
		// than carved into the ground only fills it up to its level, and any
		// ground inside the shoreline that stands higher is dry land.
		if (up.dot(p_global) < level) {
			bool is_inside = inside[index] != 0;
			const Vector2 center((grid_origin.x + cell.x + 0.5f) * cell_size, (grid_origin.y + cell.y + 0.5f) * cell_size);
			for (uint32_t i = first; i < last; i++) {
				const uint32_t e = cell_edges[i];
				if (segments_cross(center, point, points[e], get_edge_end(e))) {
					is_inside = !is_inside;
				}
			}
			if (is_inside) {
				return true;
			}
		}
		if (margin > 0.0f) {
			for (uint32_t i = first; i < last; i++) {
				if (get_distance_squared_to_edge(point, cell_edges[i], t) <= margin * margin) {
					return true;
				}
			}
		}
		return false;
	}
};

// Every LandscapeSpline3D in p_spawner's world whose type is one of p_types and
// whose coverage reaches into the volume p_volume_corners bound.
void gather_spline_keep_outs(const Node3D *p_spawner, const Vector3 *p_volume_corners, uint32_t p_types, float p_margin, LocalVector<SplineKeepOut> &r_keep_outs) {
	const Ref<World3D> world = p_spawner->get_world_3d();
	for (Node *node : p_spawner->get_tree()->get_nodes_in_group(LandscapeSpline3D::get_group_name())) {
		LandscapeSpline3D *spline = Object::cast_to<LandscapeSpline3D>(node);
		if (spline == nullptr || !spline->is_inside_tree() || spline->get_world_3d() != world) {
			continue;
		}
		if ((p_types & (1u << spline->get_spline_type())) == 0) {
			continue;
		}
		LandscapeSpline3D::GroundCoverage coverage;
		if (!spline->get_ground_coverage(coverage)) {
			continue;
		}
		r_keep_outs.resize(r_keep_outs.size() + 1);
		if (!r_keep_outs[r_keep_outs.size() - 1].setup(coverage, p_margin, p_volume_corners)) {
			r_keep_outs.resize(r_keep_outs.size() - 1);
		}
	}
}

} // namespace

void FoliageSpawner3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_lod_levels", "levels"), &FoliageSpawner3D::set_lod_levels);
	ClassDB::bind_method(D_METHOD("get_lod_levels"), &FoliageSpawner3D::get_lod_levels);
	ClassDB::bind_method(D_METHOD("has_any_mesh"), &FoliageSpawner3D::has_any_mesh);

	ClassDB::bind_method(D_METHOD("set_volume_size", "size"), &FoliageSpawner3D::set_volume_size);
	ClassDB::bind_method(D_METHOD("get_volume_size"), &FoliageSpawner3D::get_volume_size);

	ClassDB::bind_method(D_METHOD("set_cell_size", "size"), &FoliageSpawner3D::set_cell_size);
	ClassDB::bind_method(D_METHOD("get_cell_size"), &FoliageSpawner3D::get_cell_size);

	ClassDB::bind_method(D_METHOD("set_density", "density"), &FoliageSpawner3D::set_density);
	ClassDB::bind_method(D_METHOD("get_density"), &FoliageSpawner3D::get_density);

	ClassDB::bind_method(D_METHOD("set_seed", "seed"), &FoliageSpawner3D::set_seed);
	ClassDB::bind_method(D_METHOD("get_seed"), &FoliageSpawner3D::get_seed);

	ClassDB::bind_method(D_METHOD("set_max_instances", "max_instances"), &FoliageSpawner3D::set_max_instances);
	ClassDB::bind_method(D_METHOD("get_max_instances"), &FoliageSpawner3D::get_max_instances);

	ClassDB::bind_method(D_METHOD("set_max_attempts_factor", "factor"), &FoliageSpawner3D::set_max_attempts_factor);
	ClassDB::bind_method(D_METHOD("get_max_attempts_factor"), &FoliageSpawner3D::get_max_attempts_factor);

	ClassDB::bind_method(D_METHOD("set_min_distance", "min_distance"), &FoliageSpawner3D::set_min_distance);
	ClassDB::bind_method(D_METHOD("get_min_distance"), &FoliageSpawner3D::get_min_distance);

	ClassDB::bind_method(D_METHOD("set_distribution_mask", "mask"), &FoliageSpawner3D::set_distribution_mask);
	ClassDB::bind_method(D_METHOD("get_distribution_mask"), &FoliageSpawner3D::get_distribution_mask);

	ClassDB::bind_method(D_METHOD("set_mask_invert", "invert"), &FoliageSpawner3D::set_mask_invert);
	ClassDB::bind_method(D_METHOD("is_mask_inverted"), &FoliageSpawner3D::is_mask_inverted);

	ClassDB::bind_method(D_METHOD("set_terrain_layer_mask", "mask"), &FoliageSpawner3D::set_terrain_layer_mask);
	ClassDB::bind_method(D_METHOD("get_terrain_layer_mask"), &FoliageSpawner3D::get_terrain_layer_mask);

	ClassDB::bind_method(D_METHOD("set_terrain_layer_mask_invert", "invert"), &FoliageSpawner3D::set_terrain_layer_mask_invert);
	ClassDB::bind_method(D_METHOD("is_terrain_layer_mask_inverted"), &FoliageSpawner3D::is_terrain_layer_mask_inverted);

	ClassDB::bind_method(D_METHOD("set_terrain_layer_threshold", "threshold"), &FoliageSpawner3D::set_terrain_layer_threshold);
	ClassDB::bind_method(D_METHOD("get_terrain_layer_threshold"), &FoliageSpawner3D::get_terrain_layer_threshold);

	ClassDB::bind_method(D_METHOD("set_spline_avoid", "types"), &FoliageSpawner3D::set_spline_avoid);
	ClassDB::bind_method(D_METHOD("get_spline_avoid"), &FoliageSpawner3D::get_spline_avoid);

	ClassDB::bind_method(D_METHOD("set_spline_margin", "margin"), &FoliageSpawner3D::set_spline_margin);
	ClassDB::bind_method(D_METHOD("get_spline_margin"), &FoliageSpawner3D::get_spline_margin);

	ClassDB::bind_method(D_METHOD("set_project_on_mesh", "project"), &FoliageSpawner3D::set_project_on_mesh);
	ClassDB::bind_method(D_METHOD("is_projecting_on_mesh"), &FoliageSpawner3D::is_projecting_on_mesh);

	ClassDB::bind_method(D_METHOD("set_ground_mesh_path", "path"), &FoliageSpawner3D::set_ground_mesh_path);
	ClassDB::bind_method(D_METHOD("get_ground_mesh_path"), &FoliageSpawner3D::get_ground_mesh_path);

	ClassDB::bind_method(D_METHOD("set_max_slope_degrees", "degrees"), &FoliageSpawner3D::set_max_slope_degrees);
	ClassDB::bind_method(D_METHOD("get_max_slope_degrees"), &FoliageSpawner3D::get_max_slope_degrees);

	ClassDB::bind_method(D_METHOD("set_align_to_normal", "align"), &FoliageSpawner3D::set_align_to_normal);
	ClassDB::bind_method(D_METHOD("is_aligned_to_normal"), &FoliageSpawner3D::is_aligned_to_normal);

	ClassDB::bind_method(D_METHOD("set_align_to_normal_amount", "amount"), &FoliageSpawner3D::set_align_to_normal_amount);
	ClassDB::bind_method(D_METHOD("get_align_to_normal_amount"), &FoliageSpawner3D::get_align_to_normal_amount);

	ClassDB::bind_method(D_METHOD("set_random_rotation", "random"), &FoliageSpawner3D::set_random_rotation);
	ClassDB::bind_method(D_METHOD("is_random_rotation_enabled"), &FoliageSpawner3D::is_random_rotation_enabled);

	ClassDB::bind_method(D_METHOD("set_random_tilt_degrees", "degrees"), &FoliageSpawner3D::set_random_tilt_degrees);
	ClassDB::bind_method(D_METHOD("get_random_tilt_degrees"), &FoliageSpawner3D::get_random_tilt_degrees);

	ClassDB::bind_method(D_METHOD("set_min_scale", "scale"), &FoliageSpawner3D::set_min_scale);
	ClassDB::bind_method(D_METHOD("get_min_scale"), &FoliageSpawner3D::get_min_scale);

	ClassDB::bind_method(D_METHOD("set_max_scale", "scale"), &FoliageSpawner3D::set_max_scale);
	ClassDB::bind_method(D_METHOD("get_max_scale"), &FoliageSpawner3D::get_max_scale);

	ClassDB::bind_method(D_METHOD("set_cell_material_overlay", "material"), &FoliageSpawner3D::set_cell_material_overlay);
	ClassDB::bind_method(D_METHOD("get_cell_material_overlay"), &FoliageSpawner3D::get_cell_material_overlay);

	ClassDB::bind_method(D_METHOD("set_cell_transparency", "transparency"), &FoliageSpawner3D::set_cell_transparency);
	ClassDB::bind_method(D_METHOD("get_cell_transparency"), &FoliageSpawner3D::get_cell_transparency);

	ClassDB::bind_method(D_METHOD("set_cell_cast_shadow", "setting"), &FoliageSpawner3D::set_cell_cast_shadow);
	ClassDB::bind_method(D_METHOD("get_cell_cast_shadow"), &FoliageSpawner3D::get_cell_cast_shadow);

	ClassDB::bind_method(D_METHOD("set_cell_extra_cull_margin", "margin"), &FoliageSpawner3D::set_cell_extra_cull_margin);
	ClassDB::bind_method(D_METHOD("get_cell_extra_cull_margin"), &FoliageSpawner3D::get_cell_extra_cull_margin);

	ClassDB::bind_method(D_METHOD("set_cell_lod_bias", "bias"), &FoliageSpawner3D::set_cell_lod_bias);
	ClassDB::bind_method(D_METHOD("get_cell_lod_bias"), &FoliageSpawner3D::get_cell_lod_bias);

	ClassDB::bind_method(D_METHOD("set_cell_ignore_occlusion_culling", "enabled"), &FoliageSpawner3D::set_cell_ignore_occlusion_culling);
	ClassDB::bind_method(D_METHOD("is_cell_ignoring_occlusion_culling"), &FoliageSpawner3D::is_cell_ignoring_occlusion_culling);

	ClassDB::bind_method(D_METHOD("set_cell_ignore_screen_space_shadows", "enabled"), &FoliageSpawner3D::set_cell_ignore_screen_space_shadows);
	ClassDB::bind_method(D_METHOD("is_cell_ignoring_screen_space_shadows"), &FoliageSpawner3D::is_cell_ignoring_screen_space_shadows);

	ClassDB::bind_method(D_METHOD("set_cell_gi_mode", "mode"), &FoliageSpawner3D::set_cell_gi_mode);
	ClassDB::bind_method(D_METHOD("get_cell_gi_mode"), &FoliageSpawner3D::get_cell_gi_mode);

	ClassDB::bind_method(D_METHOD("set_gpu_culling", "enabled"), &FoliageSpawner3D::set_gpu_culling);
	ClassDB::bind_method(D_METHOD("is_gpu_culling_enabled"), &FoliageSpawner3D::is_gpu_culling_enabled);

	ClassDB::bind_method(D_METHOD("_get_gpu_instance_data"), &FoliageSpawner3D::_get_gpu_instance_data);
	ClassDB::bind_method(D_METHOD("_set_gpu_instance_data", "data"), &FoliageSpawner3D::_set_gpu_instance_data);

	ClassDB::bind_method(D_METHOD("set_debug_show_cells", "enabled"), &FoliageSpawner3D::set_debug_show_cells);
	ClassDB::bind_method(D_METHOD("is_debug_show_cells_enabled"), &FoliageSpawner3D::is_debug_show_cells_enabled);

	ClassDB::bind_method(D_METHOD("get_cell_count"), &FoliageSpawner3D::get_cell_count);
	ClassDB::bind_method(D_METHOD("get_instance_count"), &FoliageSpawner3D::get_instance_count);

	ClassDB::bind_method(D_METHOD("regenerate"), &FoliageSpawner3D::regenerate);
	ClassDB::bind_method(D_METHOD("get_regenerate_button"), &FoliageSpawner3D::_get_regenerate_button);

	ClassDB::bind_method(D_METHOD("fit_to_ground_mesh"), &FoliageSpawner3D::fit_to_ground_mesh);
	ClassDB::bind_method(D_METHOD("get_fit_to_ground_mesh_button"), &FoliageSpawner3D::_get_fit_to_ground_mesh_button);

	ClassDB::bind_method(D_METHOD("_get_cell_data"), &FoliageSpawner3D::_get_cell_data);
	ClassDB::bind_method(D_METHOD("_set_cell_data", "data"), &FoliageSpawner3D::_set_cell_data);

	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "lod_levels", PROPERTY_HINT_ARRAY_TYPE, MAKE_RESOURCE_TYPE_HINT("FoliageLODLevel")), "set_lod_levels", "get_lod_levels");

	ADD_GROUP("Volume", "");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "volume_size", PROPERTY_HINT_RANGE, "0.01,4096,0.01,or_greater,suffix:m"), "set_volume_size", "get_volume_size");

	ADD_GROUP("Chunking", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "gpu_culling"), "set_gpu_culling", "is_gpu_culling_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cell_size", PROPERTY_HINT_RANGE, "1,256,0.5,or_greater,suffix:m"), "set_cell_size", "get_cell_size");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "_cell_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_INTERNAL | PROPERTY_USAGE_STORAGE), "_set_cell_data", "_get_cell_data");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "_gpu_instance_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_INTERNAL | PROPERTY_USAGE_STORAGE), "_set_gpu_instance_data", "_get_gpu_instance_data");

	ADD_GROUP("Distribution", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "density", PROPERTY_HINT_RANGE, "0.0,50.0,0.001,or_greater"), "set_density", "get_density");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed"), "set_seed", "get_seed");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_instances", PROPERTY_HINT_RANGE, "0,65536,1,or_greater"), "set_max_instances", "get_max_instances");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_attempts_factor", PROPERTY_HINT_RANGE, "1,200,1,or_greater"), "set_max_attempts_factor", "get_max_attempts_factor");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "min_distance", PROPERTY_HINT_RANGE, "0.0,100.0,0.001,or_greater,suffix:m"), "set_min_distance", "get_min_distance");

	ADD_GROUP("Mask", "");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "distribution_mask", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_distribution_mask", "get_distribution_mask");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "mask_invert"), "set_mask_invert", "is_mask_inverted");

	ADD_GROUP("Ground Projection", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "project_on_mesh"), "set_project_on_mesh", "is_projecting_on_mesh");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "ground_mesh_path", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "MeshInstance3D,Landscape3D"), "set_ground_mesh_path", "get_ground_mesh_path");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_slope_degrees", PROPERTY_HINT_RANGE, "0,90,0.1,suffix:°"), "set_max_slope_degrees", "get_max_slope_degrees");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "align_to_normal"), "set_align_to_normal", "is_aligned_to_normal");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "align_to_normal_amount", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_align_to_normal_amount", "get_align_to_normal_amount");

	// The flags are named after the ground's own layers in _validate_property;
	// these stand in for them while there is no Landscape3D ground to ask.
	ADD_GROUP("Terrain Layers", "terrain_layer_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "terrain_layer_mask", PROPERTY_HINT_FLAGS, "Layer 0,Layer 1,Layer 2,Layer 3,Layer 4,Layer 5,Layer 6,Layer 7"), "set_terrain_layer_mask", "get_terrain_layer_mask");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "terrain_layer_mask_invert"), "set_terrain_layer_mask_invert", "is_terrain_layer_mask_inverted");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "terrain_layer_threshold", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_terrain_layer_threshold", "get_terrain_layer_threshold");

	// One flag per LandscapeSpline3D::SplineType, in order.
	ADD_GROUP("Landscape Splines", "spline_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "spline_avoid", PROPERTY_HINT_FLAGS, "Roads,Rivers,Streams,Lakes"), "set_spline_avoid", "get_spline_avoid");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spline_margin", PROPERTY_HINT_RANGE, "0,100,0.01,or_greater,suffix:m"), "set_spline_margin", "get_spline_margin");

	ADD_GROUP("Randomization", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "random_rotation"), "set_random_rotation", "is_random_rotation_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "random_tilt_degrees", PROPERTY_HINT_RANGE, "0,90,0.1,suffix:°"), "set_random_tilt_degrees", "get_random_tilt_degrees");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "min_scale", PROPERTY_HINT_RANGE, "0.01,10.0,0.001,or_greater"), "set_min_scale", "get_min_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_scale", PROPERTY_HINT_RANGE, "0.01,10.0,0.001,or_greater"), "set_max_scale", "get_max_scale");

	// This node itself never has any visible geometry (see regenerate()), so
	// its inherited GeometryInstance3D properties are hidden in
	// _validate_property; these cell_* properties are the working equivalents
	// (shared across every LOD level; see FoliageLODLevel for the per-level
	// mesh/material_override/visibility_range_* equivalents), applied to every
	// cell's MultiMeshInstance3D and kept in sync live.
	ADD_GROUP("Cell Rendering", "cell_");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "cell_material_overlay", PROPERTY_HINT_RESOURCE_TYPE, "BaseMaterial3D,ShaderMaterial"), "set_cell_material_overlay", "get_cell_material_overlay");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cell_transparency", PROPERTY_HINT_RANGE, "0.0,1.0,0.01"), "set_cell_transparency", "get_cell_transparency");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cell_cast_shadow", PROPERTY_HINT_ENUM, "Off,On,Double-Sided,Shadows Only"), "set_cell_cast_shadow", "get_cell_cast_shadow");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cell_extra_cull_margin", PROPERTY_HINT_RANGE, "0,16384,0.01,suffix:m"), "set_cell_extra_cull_margin", "get_cell_extra_cull_margin");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cell_lod_bias", PROPERTY_HINT_RANGE, "0.001,128,0.001"), "set_cell_lod_bias", "get_cell_lod_bias");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cell_ignore_occlusion_culling"), "set_cell_ignore_occlusion_culling", "is_cell_ignoring_occlusion_culling");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cell_ignore_screen_space_shadows"), "set_cell_ignore_screen_space_shadows", "is_cell_ignoring_screen_space_shadows");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cell_gi_mode", PROPERTY_HINT_ENUM, "Disabled,Static,Dynamic"), "set_cell_gi_mode", "get_cell_gi_mode");

	ADD_GROUP("Debug", "debug_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug_show_cells"), "set_debug_show_cells", "is_debug_show_cells_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cell_count", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "get_cell_count");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "instance_count", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "get_instance_count");

	ADD_GROUP("", "");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "fit_to_ground_mesh_button", PROPERTY_HINT_TOOL_BUTTON, "Fit To Ground Mesh", PROPERTY_USAGE_EDITOR), "", "get_fit_to_ground_mesh_button");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "regenerate_button", PROPERTY_HINT_TOOL_BUTTON, "Regenerate", PROPERTY_USAGE_EDITOR), "", "get_regenerate_button");
}

void FoliageSpawner3D::_validate_property(PropertyInfo &p_property) const {
	if (p_property.name == "terrain_layer_mask") {
		p_property.hint_string = _get_terrain_layer_hint();
		return;
	}

	if (p_property.name == "multimesh") {
		// Superseded by chunking (see _cell_data): this base-class property is
		// only still written to for scenes saved before chunking existed, so
		// their bake keeps rendering until the next Regenerate. Keep it out of
		// the inspector either way.
		p_property.usage = PROPERTY_USAGE_STORAGE;
		return;
	}

	// This node itself never has any visible geometry (see regenerate()), so
	// its inherited GeometryInstance3D properties would have no effect if set
	// here; hide them in favor of the working cell_* equivalents (see the
	// "Cell Rendering" group in _bind_methods).
	static const char *hidden_geometry_instance_properties[] = {
		"material_override",
		"material_overlay",
		"transparency",
		"cast_shadow",
		"extra_cull_margin",
		"lod_bias",
		"ignore_occlusion_culling",
		"ignore_screen_space_shadows",
		"gi_mode",
		"gi_lightmap_texel_scale",
		"gi_lightmap_scale",
		"visibility_range_begin",
		"visibility_range_begin_margin",
		"visibility_range_end",
		"visibility_range_end_margin",
		"visibility_range_fade_mode",
	};
	for (const char *hidden_name : hidden_geometry_instance_properties) {
		if (p_property.name == hidden_name) {
			p_property.usage = PROPERTY_USAGE_NONE;
			return;
		}
	}
}

void FoliageSpawner3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			// Cell MultiMeshInstance3D nodes are runtime-only (see _get_cell_data);
			// (re-)create/sync every cell so their settings reflect this node's
			// current cell_* properties, since those may have been deserialized
			// after _cell_data during scene loading (GeometryInstance3D-derived
			// FoliageSpawner3D properties are declared after this node's own).
			for (KeyValue<Vector2i, FoliageCell> &kv : cells) {
				_sync_cell_lods(kv.value);
			}
			// Same for the GPU path's nodes, which are not saved either. Done
			// here rather than as the properties arrive, because which of the
			// two representations the scene was saved in is only known once
			// all of them have been set.
			if (_is_gpu_culling_active() && gpu_nodes.is_empty()) {
				if (gpu_transforms.is_empty() && !cells.is_empty()) {
					// Saved by the cell path, opened with GPU culling on.
					gpu_transforms = _gather_cell_transforms();
					_clear_cells();
				}
				_rebuild_gpu_instances();
			}
		} break;

		case NOTIFICATION_INTERNAL_PROCESS: {
			_dispatch_gpu_culling();
		} break;
	}
}

Callable FoliageSpawner3D::_get_regenerate_button() const {
	return Callable(const_cast<FoliageSpawner3D *>(this), "regenerate");
}

Callable FoliageSpawner3D::_get_fit_to_ground_mesh_button() const {
	return Callable(const_cast<FoliageSpawner3D *>(this), "fit_to_ground_mesh");
}

void FoliageSpawner3D::_clear_cells() {
	for (KeyValue<Vector2i, FoliageCell> &kv : cells) {
		for (MultiMeshInstance3D *node : kv.value.lod_nodes) {
			if (node != nullptr) {
				remove_child(node);
				node->queue_free();
			}
		}
	}
	cells.clear();
}

FoliageSpawner3D::FoliageCell &FoliageSpawner3D::_get_or_create_cell(const Vector2i &p_cell) {
	const bool is_new = !cells.has(p_cell);
	FoliageCell &cell = cells[p_cell];
	if (is_new) {
		_sync_cell_lods(cell);
	}
	return cell;
}

LocalVector<Transform3D> FoliageSpawner3D::_read_transforms(const Ref<MultiMesh> &p_multimesh) {
	LocalVector<Transform3D> result;
	if (p_multimesh.is_null()) {
		return result;
	}
	const int count = p_multimesh->get_instance_count();
	result.resize(count);
	for (int i = 0; i < count; i++) {
		result[i] = p_multimesh->get_instance_transform(i);
	}
	return result;
}

void FoliageSpawner3D::_write_transforms(const Ref<MultiMesh> &p_multimesh, const LocalVector<Transform3D> &p_transforms) {
	if (p_multimesh.is_null()) {
		return;
	}
	// MultiMesh.instance_count "clears and (re)sizes the buffers" on every
	// set, so this always writes the *entire* array back, never just a diff.
	p_multimesh->set_instance_count((int)p_transforms.size());
	for (uint32_t i = 0; i < p_transforms.size(); i++) {
		p_multimesh->set_instance_transform(i, p_transforms[i]);
	}
}

void FoliageSpawner3D::_sync_cell_lods(FoliageCell &p_cell) {
	const int lod_count = lod_levels.size();

	// Shrink extra LOD multimeshes/nodes if lod_levels now has fewer levels
	// than this cell was last synced with.
	while ((int)p_cell.lod_multimeshes.size() > lod_count) {
		const int last = p_cell.lod_multimeshes.size() - 1;
		if (last < (int)p_cell.lod_nodes.size()) {
			if (p_cell.lod_nodes[last] != nullptr) {
				remove_child(p_cell.lod_nodes[last]);
				p_cell.lod_nodes[last]->queue_free();
			}
			p_cell.lod_nodes.remove_at(last);
		}
		p_cell.lod_multimeshes.remove_at(last);
	}

	// Grow: new LOD levels start out with the same instance transforms as
	// the rest of the cell (read from any existing multimesh) instead of
	// being empty, so a newly added LOD level doesn't need a Regenerate.
	const LocalVector<Transform3D> existing_transforms = p_cell.lod_multimeshes.is_empty() ? LocalVector<Transform3D>() : _read_transforms(p_cell.lod_multimeshes[0]);
	while ((int)p_cell.lod_multimeshes.size() < lod_count) {
		Ref<MultiMesh> mm;
		mm.instantiate();
		mm->set_transform_format(MultiMesh::TRANSFORM_3D);
		_write_transforms(mm, existing_transforms);
		p_cell.lod_multimeshes.push_back(mm);
	}

	// Ensure every multimesh has a MultiMeshInstance3D to be rendered through.
	while ((int)p_cell.lod_nodes.size() < (int)p_cell.lod_multimeshes.size()) {
		MultiMeshInstance3D *node = memnew(MultiMeshInstance3D);
		add_child(node, false, INTERNAL_MODE_FRONT);
		p_cell.lod_nodes.push_back(node);
	}

	// Apply each LOD level's own mesh/material/visibility range, plus the
	// cell_* properties shared by the whole spawner, to every node.
	for (int i = 0; i < lod_count; i++) {
		Ref<FoliageLODLevel> level = lod_levels[i];
		MultiMeshInstance3D *node = p_cell.lod_nodes[i];
		Ref<MultiMesh> mm = p_cell.lod_multimeshes[i];
		if (level.is_null() || node == nullptr || mm.is_null()) {
			continue;
		}

		mm->set_mesh(level->get_mesh());

		node->set_multimesh(mm);
		_configure_cell_node(node, level);
	}
}

void FoliageSpawner3D::_configure_cell_node(MultiMeshInstance3D *p_node, const Ref<FoliageLODLevel> &p_level, bool p_apply_visibility_range) const {
	// Cells mirror this node's cell_* rendering settings, the same way
	// FoliagePainter3D's FoliageLayer settings are copied to its cell nodes.
	// See the "Cell Rendering" comment above the cell_* fields for why these
	// aren't just the plain inherited GeometryInstance3D properties. The
	// per-level mesh/material_override/visibility_range_* come from p_level
	// instead (see FoliageLODLevel); it is only ever null defensively (a
	// mismatch between lod_nodes and lod_levels), in which case those are
	// simply left unchanged.
	p_node->set_material_overlay(cell_material_overlay);
	p_node->set_transparency(cell_transparency);
	// A level that opts out drops from the shadow passes entirely; one that
	// does not still follows the spawner-wide setting, including its mode.
	const bool level_casts = p_level.is_null() || p_level->is_casting_shadows();
	p_node->set_cast_shadows_setting(level_casts ? cell_cast_shadow : SHADOW_CASTING_SETTING_OFF);
	p_node->set_extra_cull_margin(cell_extra_cull_margin);
	p_node->set_lod_bias(cell_lod_bias);
	p_node->set_ignore_occlusion_culling(cell_ignore_occlusion_culling);
	p_node->set_ignore_screen_space_shadows(cell_ignore_screen_space_shadows);
	p_node->set_gi_mode(cell_gi_mode);
	if (p_level.is_valid()) {
		p_node->set_material_override(p_level->get_material_override());
		if (p_apply_visibility_range) {
			p_node->set_visibility_range_begin(p_level->get_visibility_range_begin());
			p_node->set_visibility_range_begin_margin(p_level->get_visibility_range_begin_margin());
			p_node->set_visibility_range_end(p_level->get_visibility_range_end());
			p_node->set_visibility_range_end_margin(p_level->get_visibility_range_end_margin());
			p_node->set_visibility_range_fade_mode(p_level->get_visibility_range_fade_mode());
		}
	}
}

void FoliageSpawner3D::_sync_all_cells_settings() {
	for (KeyValue<Vector2i, FoliageCell> &kv : cells) {
		for (int i = 0; i < (int)kv.value.lod_nodes.size(); i++) {
			if (kv.value.lod_nodes[i] == nullptr) {
				continue;
			}
			Ref<FoliageLODLevel> level = i < lod_levels.size() ? Ref<FoliageLODLevel>(lod_levels[i]) : Ref<FoliageLODLevel>();
			_configure_cell_node(kv.value.lod_nodes[i], level);
		}
	}

	// The GPU path renders through its own nodes instead of the cells', and
	// there are no cells at all while it is on, so without this every cell_*
	// property would silently stop doing anything until the next Regenerate
	// rebuilt the nodes from scratch.
	for (uint32_t i = 0; i < gpu_nodes.size(); i++) {
		if (gpu_nodes[i] == nullptr) {
			continue;
		}
		const int level_index = i < gpu_lod_indices.size() ? gpu_lod_indices[i] : -1;
		Ref<FoliageLODLevel> level = (level_index >= 0 && level_index < lod_levels.size()) ? Ref<FoliageLODLevel>(lod_levels[level_index]) : Ref<FoliageLODLevel>();
		_configure_cell_node(gpu_nodes[i], level, false);
	}
}

bool FoliageSpawner3D::_is_gpu_culling_active() const {
	return gpu_culling && FoliageGPUCuller::is_supported();
}

LocalVector<Transform3D> FoliageSpawner3D::_gather_cell_transforms() const {
	LocalVector<Transform3D> result;
	for (const KeyValue<Vector2i, FoliageCell> &kv : cells) {
		if (kv.value.lod_multimeshes.is_empty() || kv.value.lod_multimeshes[0].is_null()) {
			continue;
		}
		// Every LOD level of a cell holds the same transforms, so the first is
		// as good as any.
		for (const Transform3D &t : _read_transforms(kv.value.lod_multimeshes[0])) {
			result.push_back(t);
		}
	}
	return result;
}

void FoliageSpawner3D::_rebuild_cells_from_transforms(const LocalVector<Transform3D> &p_transforms) {
	_clear_cells();

	// Group instances by their chunking cell (see class comment) before
	// building each cell's MultiMesh, instead of one MultiMesh for everything.
	const float chunk_cell_size = MAX(cell_size, 0.01f);
	HashMap<Vector2i, LocalVector<Transform3D>> cell_transforms;

	for (uint32_t i = 0; i < p_transforms.size(); i++) {
		const Vector3 &origin = p_transforms[i].origin;
		const Vector2i cc(int(Math::floor(origin.x / chunk_cell_size)), int(Math::floor(origin.z / chunk_cell_size)));
		cell_transforms[cc].push_back(p_transforms[i]);
	}

	for (KeyValue<Vector2i, LocalVector<Transform3D>> &kv : cell_transforms) {
		FoliageCell &fc = _get_or_create_cell(kv.key);
		for (const Ref<MultiMesh> &mm : fc.lod_multimeshes) {
			_write_transforms(mm, kv.value);
		}
	}
}

void FoliageSpawner3D::_clear_gpu_instances() {
	// Released first, and so queued first: its uniform sets reference the
	// MultiMeshes' buffers, which must not be freed before them.
	gpu_culler.release();

	for (MultiMeshInstance3D *node : gpu_nodes) {
		if (node != nullptr) {
			remove_child(node);
			node->queue_free();
		}
	}
	gpu_nodes.clear();
	gpu_multimeshes.clear();
	gpu_lod_indices.clear();
	set_process_internal(false);
}

void FoliageSpawner3D::_rebuild_gpu_instances() {
	_clear_gpu_instances();

	if (!_is_gpu_culling_active() || gpu_transforms.is_empty()) {
		return;
	}

	const int lod_count = MIN(lod_levels.size(), FoliageGPUCuller::MAX_LOD_LEVELS);
	if (lod_count == 0) {
		return;
	}

	const int instance_count = (int)gpu_transforms.size();

	// Taken from where the instances actually are rather than from volume_size,
	// so that shrinking the volume after generating cannot leave the bounds too
	// small and have the renderer cull foliage that is still there.
	AABB bounds(gpu_transforms[0].origin, Vector3());
	for (uint32_t i = 1; i < gpu_transforms.size(); i++) {
		bounds.expand_to(gpu_transforms[i].origin);
	}

	LocalVector<FoliageGPUCuller::LODLevel> culler_levels;
	float previous_range_end = 0.0f;

	for (int i = 0; i < lod_count; i++) {
		Ref<FoliageLODLevel> level = lod_levels[i];
		if (level.is_null() || level->get_mesh().is_null()) {
			continue;
		}

		const AABB mesh_aabb = level->get_mesh()->get_aabb();
		// Distance from the instance origin to the farthest corner of the mesh,
		// since a mesh is rarely centered on its origin.
		const Vector3 farthest(
				MAX(Math::abs(mesh_aabb.position.x), Math::abs(mesh_aabb.position.x + mesh_aabb.size.x)),
				MAX(Math::abs(mesh_aabb.position.y), Math::abs(mesh_aabb.position.y + mesh_aabb.size.y)),
				MAX(Math::abs(mesh_aabb.position.z), Math::abs(mesh_aabb.position.z + mesh_aabb.size.z)));
		const float instance_radius = farthest.length();

		Ref<MultiMesh> mm;
		mm.instantiate();
		// Order matters: the indirect flag has to be set before the buffers are
		// allocated, and the mesh after, since that is what builds the draw
		// command buffer the compute pass writes into.
		mm->set_use_indirect(true);
		mm->set_transform_format(MultiMesh::TRANSFORM_3D);
		mm->set_instance_count(instance_count);
		mm->set_mesh(level->get_mesh());
		// How many instances survive culling is only known on the GPU, so the
		// bounds are stated up front: every instance origin, plus its reach.
		mm->set_custom_aabb(bounds.grow(instance_radius * MAX(1.0f, max_scale)));

		MultiMeshInstance3D *node = memnew(MultiMeshInstance3D);
		node->set_multimesh(mm);
		_configure_cell_node(node, level, false);
		add_child(node, false, INTERNAL_MODE_FRONT);

		FoliageGPUCuller::LODLevel culler_level;
		culler_level.multimesh = mm->get_rid();
		// Bands are walked forward and clamped so that an instance lands in
		// exactly one level: overlapping ranges would otherwise draw it twice,
		// and the GPU path has no cross-fade to blend them with.
		culler_level.range_begin = MAX(level->get_visibility_range_begin(), previous_range_end);
		culler_level.range_end = level->get_visibility_range_end();
		if (culler_level.range_end > 0.0f) {
			culler_level.range_end = MAX(culler_level.range_end, culler_level.range_begin);
			previous_range_end = culler_level.range_end;
		}
		culler_level.instance_radius = instance_radius;
		culler_level.surface_count = MAX(1, level->get_mesh()->get_surface_count());

		gpu_multimeshes.push_back(mm);
		gpu_nodes.push_back(node);
		gpu_lod_indices.push_back(i);
		culler_levels.push_back(culler_level);
	}

	if (culler_levels.is_empty()) {
		return;
	}

	gpu_culler.update_instances(gpu_transforms, culler_levels);
	set_process_internal(true);
}

void FoliageSpawner3D::_dispatch_gpu_culling() {
	if (gpu_nodes.is_empty() || !is_inside_tree()) {
		return;
	}

	Camera3D *camera = FoliageGPUCuller::resolve_culling_camera(this, gpu_culling_camera);
	if (camera == nullptr) {
		// Better to draw everything than to have the foliage vanish because
		// there is nothing to cull against.
		gpu_culler.draw_without_culling();
		return;
	}

	// The compute pass works in the MultiMeshes' local space, which is this
	// node's own space, so the frustum is brought over rather than every
	// instance being transformed into world space.
	const Transform3D to_local = get_global_transform().affine_inverse();

	Vector<Plane> planes = camera->get_frustum();
	for (int i = 0; i < planes.size(); i++) {
		planes.write[i] = to_local.xform(planes[i]);
	}

	// Occlusion culling runs per viewport, so the one to test against is the
	// viewport the culling camera draws into (in the editor, that is the
	// editor's own viewport rather than this node's).
	const Viewport *camera_viewport = camera->get_viewport();
	const RID occlusion_viewport = camera_viewport != nullptr ? camera_viewport->get_viewport_rid() : RID();

	gpu_culler.cull(planes, to_local.xform(camera->get_global_position()), occlusion_viewport, get_global_transform(), !cell_ignore_occlusion_culling);
}

PackedFloat32Array FoliageSpawner3D::_get_gpu_instance_data() const {
	PackedFloat32Array result;
	if (gpu_transforms.is_empty()) {
		return result;
	}

	result.resize(gpu_transforms.size() * 12);
	float *w = result.ptrw();
	for (uint32_t i = 0; i < gpu_transforms.size(); i++) {
		const Transform3D &t = gpu_transforms[i];
		float *dst = w + i * 12;
		dst[0] = t.basis.rows[0][0];
		dst[1] = t.basis.rows[0][1];
		dst[2] = t.basis.rows[0][2];
		dst[3] = t.origin.x;
		dst[4] = t.basis.rows[1][0];
		dst[5] = t.basis.rows[1][1];
		dst[6] = t.basis.rows[1][2];
		dst[7] = t.origin.y;
		dst[8] = t.basis.rows[2][0];
		dst[9] = t.basis.rows[2][1];
		dst[10] = t.basis.rows[2][2];
		dst[11] = t.origin.z;
	}
	return result;
}

void FoliageSpawner3D::_set_gpu_instance_data(const PackedFloat32Array &p_data) {
	gpu_transforms.clear();

	const int count = p_data.size() / 12;
	gpu_transforms.resize(count);
	const float *r = p_data.ptr();
	for (int i = 0; i < count; i++) {
		const float *src = r + i * 12;
		// The nine-scalar constructor fills the basis rows in order, matching
		// the row-major layout a MultiMesh buffer uses.
		gpu_transforms[i] = Transform3D(
				Basis(src[0], src[1], src[2],
						src[4], src[5], src[6],
						src[8], src[9], src[10]),
				Vector3(src[3], src[7], src[11]));
	}

	_rebuild_gpu_instances();
}

Array FoliageSpawner3D::_get_cell_data() const {
	Array result;
	for (const KeyValue<Vector2i, FoliageCell> &kv : cells) {
		if (kv.value.lod_multimeshes.is_empty() || kv.value.lod_multimeshes[0].is_null() || kv.value.lod_multimeshes[0]->get_instance_count() == 0) {
			continue;
		}
		Array lod_multimeshes;
		for (const Ref<MultiMesh> &mm : kv.value.lod_multimeshes) {
			lod_multimeshes.push_back(mm);
		}
		Array entry;
		entry.push_back(kv.key);
		entry.push_back(lod_multimeshes);
		result.push_back(entry);
	}
	return result;
}

void FoliageSpawner3D::_set_cell_data(const Array &p_data) {
	_clear_cells();

	for (int i = 0; i < p_data.size(); i++) {
		Array entry = p_data[i];
		if (entry.size() != 2) {
			continue;
		}
		const Vector2i cell_coord = entry[0];
		Array lod_multimeshes = entry[1];
		if (lod_multimeshes.is_empty()) {
			continue;
		}

		FoliageCell cell;
		for (int j = 0; j < lod_multimeshes.size(); j++) {
			Ref<MultiMesh> mm = lod_multimeshes[j];
			if (mm.is_valid()) {
				cell.lod_multimeshes.push_back(mm);
			}
		}
		if (cell.lod_multimeshes.is_empty()) {
			continue;
		}
		cells[cell_coord] = cell;
		_sync_cell_lods(cells[cell_coord]);
	}
}

void FoliageSpawner3D::set_lod_levels(const TypedArray<FoliageLODLevel> &p_levels) {
	for (int i = 0; i < lod_levels.size(); i++) {
		Ref<FoliageLODLevel> old_level = lod_levels[i];
		if (old_level.is_valid()) {
			old_level->disconnect(CoreStringName(changed), callable_mp(this, &FoliageSpawner3D::_on_lod_level_changed));
		}
	}

	lod_levels = p_levels;

	for (int i = 0; i < lod_levels.size(); i++) {
		Ref<FoliageLODLevel> level = lod_levels[i];
		if (level.is_valid()) {
			level->connect(CoreStringName(changed), callable_mp(this, &FoliageSpawner3D::_on_lod_level_changed));
		}
	}

	for (KeyValue<Vector2i, FoliageCell> &kv : cells) {
		_sync_cell_lods(kv.value);
	}
	_rebuild_gpu_instances();
	update_configuration_warnings();
}

TypedArray<FoliageLODLevel> FoliageSpawner3D::get_lod_levels() const {
	return lod_levels;
}

void FoliageSpawner3D::_on_lod_level_changed() {
	for (KeyValue<Vector2i, FoliageCell> &kv : cells) {
		_sync_cell_lods(kv.value);
	}
	// A level's mesh or range changing moves the GPU path's band boundaries and
	// bounding radii, both of which are baked into the culler's setup.
	_rebuild_gpu_instances();
	update_configuration_warnings();
}

bool FoliageSpawner3D::has_any_mesh() const {
	for (int i = 0; i < lod_levels.size(); i++) {
		Ref<FoliageLODLevel> level = lod_levels[i];
		if (level.is_valid() && level->get_mesh().is_valid()) {
			return true;
		}
	}
	return false;
}

void FoliageSpawner3D::set_volume_size(const Vector3 &p_size) {
	volume_size = p_size.maxf(0);
	update_gizmos();
	update_configuration_warnings();
}

Vector3 FoliageSpawner3D::get_volume_size() const {
	return volume_size;
}

void FoliageSpawner3D::set_cell_size(float p_size) {
	cell_size = MAX(p_size, 0.01f);
}

float FoliageSpawner3D::get_cell_size() const {
	return cell_size;
}

void FoliageSpawner3D::set_density(float p_density) {
	density = MAX(p_density, 0.0f);
}

float FoliageSpawner3D::get_density() const {
	return density;
}

void FoliageSpawner3D::set_seed(int p_seed) {
	seed = p_seed;
}

int FoliageSpawner3D::get_seed() const {
	return seed;
}

void FoliageSpawner3D::set_max_instances(int p_max_instances) {
	max_instances = MAX(p_max_instances, 0);
}

int FoliageSpawner3D::get_max_instances() const {
	return max_instances;
}

void FoliageSpawner3D::set_max_attempts_factor(int p_factor) {
	max_attempts_factor = MAX(p_factor, 1);
}

int FoliageSpawner3D::get_max_attempts_factor() const {
	return max_attempts_factor;
}

void FoliageSpawner3D::set_min_distance(float p_min_distance) {
	min_distance = MAX(p_min_distance, 0.0f);
}

float FoliageSpawner3D::get_min_distance() const {
	return min_distance;
}

void FoliageSpawner3D::set_distribution_mask(const Ref<Texture2D> &p_mask) {
	distribution_mask = p_mask;
}

Ref<Texture2D> FoliageSpawner3D::get_distribution_mask() const {
	return distribution_mask;
}

void FoliageSpawner3D::set_mask_invert(bool p_invert) {
	mask_invert = p_invert;
}

bool FoliageSpawner3D::is_mask_inverted() const {
	return mask_invert;
}

void FoliageSpawner3D::set_terrain_layer_mask(uint32_t p_mask) {
	terrain_layer_mask = p_mask;
	update_configuration_warnings();
}

uint32_t FoliageSpawner3D::get_terrain_layer_mask() const {
	return terrain_layer_mask;
}

void FoliageSpawner3D::set_terrain_layer_mask_invert(bool p_invert) {
	terrain_layer_mask_invert = p_invert;
}

bool FoliageSpawner3D::is_terrain_layer_mask_inverted() const {
	return terrain_layer_mask_invert;
}

void FoliageSpawner3D::set_terrain_layer_threshold(float p_threshold) {
	terrain_layer_threshold = CLAMP(p_threshold, 0.0f, 1.0f);
}

float FoliageSpawner3D::get_terrain_layer_threshold() const {
	return terrain_layer_threshold;
}

void FoliageSpawner3D::set_spline_avoid(uint32_t p_types) {
	spline_avoid = p_types;
}

uint32_t FoliageSpawner3D::get_spline_avoid() const {
	return spline_avoid;
}

void FoliageSpawner3D::set_spline_margin(float p_margin) {
	spline_margin = MAX(p_margin, 0.0f);
}

float FoliageSpawner3D::get_spline_margin() const {
	return spline_margin;
}

void FoliageSpawner3D::set_project_on_mesh(bool p_project) {
	project_on_mesh = p_project;
	update_configuration_warnings();
}

bool FoliageSpawner3D::is_projecting_on_mesh() const {
	return project_on_mesh;
}

void FoliageSpawner3D::set_ground_mesh_path(const NodePath &p_path) {
	ground_mesh_path = p_path;
	update_configuration_warnings();
	// terrain_layer_mask's flags are named after the ground's layers.
	notify_property_list_changed();
}

NodePath FoliageSpawner3D::get_ground_mesh_path() const {
	return ground_mesh_path;
}

void FoliageSpawner3D::set_max_slope_degrees(float p_degrees) {
	max_slope_degrees = CLAMP(p_degrees, 0.0f, 90.0f);
}

float FoliageSpawner3D::get_max_slope_degrees() const {
	return max_slope_degrees;
}

void FoliageSpawner3D::set_align_to_normal(bool p_align) {
	align_to_normal = p_align;
	notify_property_list_changed();
}

bool FoliageSpawner3D::is_aligned_to_normal() const {
	return align_to_normal;
}

void FoliageSpawner3D::set_align_to_normal_amount(float p_amount) {
	align_to_normal_amount = CLAMP(p_amount, 0.0f, 1.0f);
}

float FoliageSpawner3D::get_align_to_normal_amount() const {
	return align_to_normal_amount;
}

void FoliageSpawner3D::set_random_rotation(bool p_random) {
	random_rotation = p_random;
}

bool FoliageSpawner3D::is_random_rotation_enabled() const {
	return random_rotation;
}

void FoliageSpawner3D::set_random_tilt_degrees(float p_degrees) {
	random_tilt_degrees = CLAMP(p_degrees, 0.0f, 90.0f);
}

float FoliageSpawner3D::get_random_tilt_degrees() const {
	return random_tilt_degrees;
}

void FoliageSpawner3D::set_min_scale(float p_scale) {
	min_scale = MAX(p_scale, 0.001f);
}

float FoliageSpawner3D::get_min_scale() const {
	return min_scale;
}

void FoliageSpawner3D::set_max_scale(float p_scale) {
	max_scale = MAX(p_scale, 0.001f);
}

float FoliageSpawner3D::get_max_scale() const {
	return max_scale;
}

void FoliageSpawner3D::set_cell_material_overlay(const Ref<Material> &p_material) {
	cell_material_overlay = p_material;
	_sync_all_cells_settings();
}

Ref<Material> FoliageSpawner3D::get_cell_material_overlay() const {
	return cell_material_overlay;
}

void FoliageSpawner3D::set_cell_transparency(float p_transparency) {
	cell_transparency = CLAMP(p_transparency, 0.0f, 1.0f);
	_sync_all_cells_settings();
}

float FoliageSpawner3D::get_cell_transparency() const {
	return cell_transparency;
}

void FoliageSpawner3D::set_cell_cast_shadow(ShadowCastingSetting p_setting) {
	cell_cast_shadow = p_setting;
	_sync_all_cells_settings();
}

FoliageSpawner3D::ShadowCastingSetting FoliageSpawner3D::get_cell_cast_shadow() const {
	return cell_cast_shadow;
}

void FoliageSpawner3D::set_cell_extra_cull_margin(float p_margin) {
	cell_extra_cull_margin = MAX(p_margin, 0.0f);
	_sync_all_cells_settings();
}

float FoliageSpawner3D::get_cell_extra_cull_margin() const {
	return cell_extra_cull_margin;
}

void FoliageSpawner3D::set_cell_lod_bias(float p_bias) {
	cell_lod_bias = MAX(p_bias, 0.001f);
	_sync_all_cells_settings();
}

float FoliageSpawner3D::get_cell_lod_bias() const {
	return cell_lod_bias;
}

void FoliageSpawner3D::set_cell_ignore_occlusion_culling(bool p_enabled) {
	cell_ignore_occlusion_culling = p_enabled;
	_sync_all_cells_settings();
}

bool FoliageSpawner3D::is_cell_ignoring_occlusion_culling() const {
	return cell_ignore_occlusion_culling;
}

void FoliageSpawner3D::set_cell_ignore_screen_space_shadows(bool p_enabled) {
	cell_ignore_screen_space_shadows = p_enabled;
	_sync_all_cells_settings();
}

bool FoliageSpawner3D::is_cell_ignoring_screen_space_shadows() const {
	return cell_ignore_screen_space_shadows;
}

void FoliageSpawner3D::set_cell_gi_mode(GIMode p_mode) {
	cell_gi_mode = p_mode;
	_sync_all_cells_settings();
}

FoliageSpawner3D::GIMode FoliageSpawner3D::get_cell_gi_mode() const {
	return cell_gi_mode;
}

void FoliageSpawner3D::set_debug_show_cells(bool p_enabled) {
	debug_show_cells = p_enabled;
	update_gizmos();
}

bool FoliageSpawner3D::is_debug_show_cells_enabled() const {
	return debug_show_cells;
}

int FoliageSpawner3D::get_cell_count() const {
	// The GPU path culls per instance instead of per cell, so it has none.
	return cells.size();
}

int FoliageSpawner3D::get_instance_count() const {
	if (_is_gpu_culling_active()) {
		// How many of these survive culling is only known on the GPU; this is
		// the number that was generated.
		return (int)gpu_transforms.size();
	}

	int total = 0;
	for (const KeyValue<Vector2i, FoliageCell> &kv : cells) {
		if (!kv.value.lod_multimeshes.is_empty() && kv.value.lod_multimeshes[0].is_valid()) {
			total += kv.value.lod_multimeshes[0]->get_instance_count();
		}
	}
	return total;
}

Ref<Image> FoliageSpawner3D::_get_mask_image() const {
	if (distribution_mask.is_null()) {
		return Ref<Image>();
	}
	Ref<Image> img = distribution_mask->get_image();
	if (img.is_null()) {
		return Ref<Image>();
	}
	if (img->is_compressed()) {
		img = img->duplicate();
		img->decompress();
	}
	if (img->get_width() <= 0 || img->get_height() <= 0) {
		return Ref<Image>();
	}
	return img;
}

bool FoliageSpawner3D::_sample_mask(const Ref<Image> &p_image, const Vector2 &p_uv, RandomPCG &p_rng) const {
	const int w = p_image->get_width();
	const int h = p_image->get_height();
	const int px = CLAMP(int(p_uv.x * w), 0, w - 1);
	const int py = CLAMP(int((1.0f - p_uv.y) * h), 0, h - 1);

	float value = p_image->get_pixel(px, py).get_luminance();
	if (mask_invert) {
		value = 1.0f - value;
	}
	return p_rng.randf() <= value;
}

bool FoliageSpawner3D::_sample_terrain_layers(float p_share, RandomPCG &p_rng) const {
	const float share = terrain_layer_mask_invert ? 1.0f - p_share : p_share;
	// Nothing below the threshold, then a chance that rises to certainty where
	// the chosen layers cover the ground completely: across a blend between
	// layers, the foliage thins out along with its layers' textures rather
	// than stopping at a line.
	float chance;
	if (terrain_layer_threshold >= 1.0f) {
		chance = share >= 1.0f ? 1.0f : 0.0f;
	} else {
		chance = CLAMP((share - terrain_layer_threshold) / (1.0f - terrain_layer_threshold), 0.0f, 1.0f);
	}
	if (chance >= 1.0f) {
		return true;
	}
	if (chance <= 0.0f) {
		return false;
	}
	return p_rng.randf() < chance;
}

String FoliageSpawner3D::_get_terrain_layer_hint() const {
	const Landscape3D *terrain = is_inside_tree() ? Object::cast_to<Landscape3D>(get_node_or_null(ground_mesh_path)) : nullptr;
	const TypedArray<TerrainLayer> terrain_layers = terrain != nullptr ? terrain->get_layers() : TypedArray<TerrainLayer>();
	if (terrain_layers.is_empty()) {
		return "Layer 0,Layer 1,Layer 2,Layer 3,Layer 4,Layer 5,Layer 6,Layer 7";
	}

	// Named as the Landscape3D editor names them, and numbered, since layers
	// left with the same default name would be told apart by nothing else.
	PackedStringArray names;
	for (int i = 0; i < MIN(terrain_layers.size(), TerrainData::MAX_LAYERS); i++) {
		const Ref<TerrainLayer> layer = terrain_layers[i];
		String name = (layer.is_valid() && !layer->get_layer_name().is_empty()) ? layer->get_layer_name() : vformat("Layer %d", i);
		// A comma would start the next flag, a colon give this one a value.
		name = name.replace(",", " ").replace(":", " ");
		names.push_back(vformat("%d. %s", i, name));
	}
	return String(",").join(names);
}

void FoliageSpawner3D::regenerate() {
	// Instances now live in per-cell MultiMeshes (see _cell_data); clear any
	// pre-chunking bake still sitting in the inherited (and now unused)
	// MultiMeshInstance3D::multimesh, and start every regeneration from a
	// clean slate of cells.
	set_multimesh(Ref<MultiMesh>());
	_clear_cells();
	gpu_transforms.clear();
	_clear_gpu_instances();

	if (!has_any_mesh()) {
		update_configuration_warnings();
		return;
	}

	const Vector3 half = volume_size * 0.5f;
	if (half.x <= 0.0f || half.z <= 0.0f) {
		update_configuration_warnings();
		return;
	}

	// One place for every 1 / density square meters of the footprint, each of
	// which gets an instance unless something rules it out (see the loop).
	const double area = double(volume_size.x) * double(volume_size.z);
	const int64_t place_count = (int64_t)CLAMP(Math::round(area * double(density)), 0.0, 1e15);

	RandomPCG rng;
	rng.seed(uint64_t(uint32_t(seed)));

	Ref<Image> mask_image = _get_mask_image();

	const Transform3D gt = get_global_transform();
	const Transform3D gt_inv = gt.affine_inverse();

	LocalVector<Face3> local_faces;
	HashMap<Vector2i, LocalVector<uint32_t>> face_grid;
	float face_cell_size = 1.0f;

	// Landscape3D has no single conventional Mesh (it is chunked), so it is
	// sampled directly through TerrainData's height field below instead of
	// through the face_grid raycasting mechanism built for MeshInstance3D.
	Node *ground_node = is_inside_tree() ? get_node_or_null(ground_mesh_path) : nullptr;
	MeshInstance3D *ground_mesh_instance = Object::cast_to<MeshInstance3D>(ground_node);
	Landscape3D *ground_terrain = Object::cast_to<Landscape3D>(ground_node);
	Ref<TerrainData> ground_terrain_data = ground_terrain != nullptr ? ground_terrain->get_terrain_data() : Ref<TerrainData>();

	if (project_on_mesh && ground_terrain != nullptr) {
		if (ground_terrain_data.is_null()) {
			update_configuration_warnings();
			return;
		}
	} else if (project_on_mesh) {
		Ref<Mesh> ground_mesh = ground_mesh_instance != nullptr ? ground_mesh_instance->get_mesh() : Ref<Mesh>();
		Vector<Face3> faces = ground_mesh.is_valid() ? ground_mesh->get_faces() : Vector<Face3>();

		if (faces.is_empty()) {
			update_configuration_warnings();
			return;
		}

		const Transform3D ground_to_local = gt_inv * ground_mesh_instance->get_global_transform();
		local_faces.resize(faces.size());
		for (int i = 0; i < faces.size(); i++) {
			local_faces[i] = Face3(
					ground_to_local.xform(faces[i].vertex[0]),
					ground_to_local.xform(faces[i].vertex[1]),
					ground_to_local.xform(faces[i].vertex[2]));
		}

		face_cell_size = MAX((volume_size.x + volume_size.z) * 0.03125f, 0.25f);
		const int cell_min_x = int(Math::floor(-half.x / face_cell_size));
		const int cell_max_x = int(Math::floor(half.x / face_cell_size));
		const int cell_min_z = int(Math::floor(-half.z / face_cell_size));
		const int cell_max_z = int(Math::floor(half.z / face_cell_size));

		for (uint32_t i = 0; i < local_faces.size(); i++) {
			const Face3 &face = local_faces[i];
			const float min_x = MIN(face.vertex[0].x, MIN(face.vertex[1].x, face.vertex[2].x));
			const float max_x = MAX(face.vertex[0].x, MAX(face.vertex[1].x, face.vertex[2].x));
			const float min_z = MIN(face.vertex[0].z, MIN(face.vertex[1].z, face.vertex[2].z));
			const float max_z = MAX(face.vertex[0].z, MAX(face.vertex[1].z, face.vertex[2].z));

			if (max_x < -half.x || min_x > half.x || max_z < -half.z || min_z > half.z) {
				continue; // Outside the volume's footprint; cannot be sampled.
			}

			const int cx0 = MAX(cell_min_x, int(Math::floor(min_x / face_cell_size)));
			const int cx1 = MIN(cell_max_x, int(Math::floor(max_x / face_cell_size)));
			const int cz0 = MAX(cell_min_z, int(Math::floor(min_z / face_cell_size)));
			const int cz1 = MIN(cell_max_z, int(Math::floor(max_z / face_cell_size)));

			for (int cx = cx0; cx <= cx1; cx++) {
				for (int cz = cz0; cz <= cz1; cz++) {
					face_grid[Vector2i(cx, cz)].push_back(i);
				}
			}
		}
	}

	// Over a Landscape3D: where it has ground at all, and how much of that
	// ground the chosen layers cover.
	const bool has_terrain = ground_terrain != nullptr && ground_terrain_data.is_valid();
	Transform3D terrain_gt;
	Transform3D local_to_terrain;
	TerrainGroundSampler terrain_ground;
	TerrainLayerSampler terrain_layers;
	bool use_terrain_ground = false;
	bool use_terrain_layers = false;
	if (has_terrain) {
		terrain_gt = ground_terrain->get_global_transform();
		local_to_terrain = terrain_gt.affine_inverse() * gt;
		use_terrain_ground = project_on_mesh && terrain_ground.setup(ground_terrain_data);
		use_terrain_layers = terrain_layer_mask != 0 && terrain_layers.setup(ground_terrain_data, ground_terrain->get_layers().size(), terrain_layer_mask);
	}

	// The roads, rivers, streams and lakes to keep off.
	LocalVector<SplineKeepOut> spline_keep_outs;
	if (spline_avoid != 0 && is_inside_tree()) {
		Vector3 volume_corners[8];
		for (int i = 0; i < 8; i++) {
			volume_corners[i] = gt.xform(Vector3((i & 1) ? half.x : -half.x, (i & 2) ? half.y : -half.y, (i & 4) ? half.z : -half.z));
		}
		gather_spline_keep_outs(this, volume_corners, spline_avoid, spline_margin, spline_keep_outs);
	}

	const float spacing_cell_size = MAX(min_distance, 0.001f);
	const float min_distance_sq = min_distance * min_distance;
	HashMap<Vector2i, LocalVector<Vector2>> grid;

	LocalVector<Transform3D> transforms;

	// Every attempt either settles one of the places - an instance grows there,
	// or something rules the place out and it is left bare - or finds it too
	// close to an instance already standing (see min_distance) and tries
	// another. Only that last kind is a retry, and it is what the attempt
	// budget is for. A place left bare is not made up for elsewhere: that would
	// crowd every instance into whatever is left, so that density would mean
	// more instances per square meter the less of the volume they may grow in.
	const int64_t max_attempts = MIN(place_count, (int64_t)max_instances) * MAX(max_attempts_factor, 1);
	int64_t places_settled = 0;
	int64_t attempts = 0;

	while ((int)transforms.size() < max_instances && places_settled < place_count && attempts < max_attempts) {
		attempts++;

		const float lx = rng.random(-half.x, half.x);
		const float lz = rng.random(-half.z, half.z);

		if (mask_image.is_valid()) {
			const Vector2 uv((lx / volume_size.x) + 0.5f, (lz / volume_size.z) + 0.5f);
			if (!_sample_mask(mask_image, uv, rng)) {
				places_settled++;
				continue;
			}
		}

		// The spot on the terrain under this point, along the terrain's own up
		// axis: exactly where the instance stands once projected onto it.
		Vector2 terrain_xz;
		if (has_terrain) {
			const Vector3 terrain_local = local_to_terrain.xform(Vector3(lx, 0.0f, lz));
			terrain_xz = Vector2(terrain_local.x, terrain_local.z);
			if (use_terrain_ground && !terrain_ground.has_ground_at(terrain_xz)) {
				places_settled++;
				continue;
			}
			if (use_terrain_layers && !_sample_terrain_layers(terrain_layers.share_at(terrain_xz), rng)) {
				places_settled++;
				continue;
			}
		}

		const Vector2i cell(int(Math::floor(lx / spacing_cell_size)), int(Math::floor(lz / spacing_cell_size)));

		if (min_distance > 0.0f) {
			bool too_close = false;
			for (int cx = -1; cx <= 1 && !too_close; cx++) {
				for (int cz = -1; cz <= 1 && !too_close; cz++) {
					const LocalVector<Vector2> *bucket = grid.getptr(cell + Vector2i(cx, cz));
					if (bucket == nullptr) {
						continue;
					}
					for (const Vector2 &p : *bucket) {
						if (p.distance_squared_to(Vector2(lx, lz)) < min_distance_sq) {
							too_close = true;
							break;
						}
					}
				}
			}
			if (too_close) {
				continue;
			}
		}

		Vector3 local_pos;
		Vector3 world_normal(0, 1, 0);

		if (project_on_mesh && ground_terrain != nullptr) {
			const float height = ground_terrain_data->get_height_at_position(terrain_xz);
			const Vector3 terrain_normal = ground_terrain_data->get_normal_at_position(terrain_xz);
			const Vector3 hit_world_normal = terrain_gt.basis.xform(terrain_normal).normalized();

			if (max_slope_degrees < 90.0f) {
				const float angle = Math::rad_to_deg(Math::acos(CLAMP(hit_world_normal.dot(Vector3(0, 1, 0)), -1.0f, 1.0f)));
				if (angle > max_slope_degrees) {
					places_settled++;
					continue;
				}
			}

			local_pos = gt_inv.xform(terrain_gt.xform(Vector3(terrain_xz.x, height, terrain_xz.y)));
			world_normal = hit_world_normal;
		} else if (project_on_mesh) {
			const Vector2i fcell(int(Math::floor(lx / face_cell_size)), int(Math::floor(lz / face_cell_size)));
			const LocalVector<uint32_t> *face_indices = face_grid.getptr(fcell);
			if (face_indices == nullptr) {
				places_settled++;
				continue;
			}

			const Vector3 seg_from(lx, half.y, lz);
			const Vector3 seg_to(lx, -half.y, lz);

			bool hit_found = false;
			Vector3 best_point;
			Vector3 best_normal(0, 1, 0);
			float best_y = 0.0f;

			for (uint32_t face_index : *face_indices) {
				const Face3 &face = local_faces[face_index];
				Vector3 point;
				if (!face.intersects_segment(seg_from, seg_to, &point)) {
					continue;
				}
				if (!hit_found || point.y > best_y) {
					hit_found = true;
					best_y = point.y;
					best_point = point;
					Vector3 n = face.get_plane().normal;
					if (n.dot(Vector3(0, -1, 0)) > 0.0f) {
						n = -n;
					}
					best_normal = n;
				}
			}

			if (!hit_found) {
				places_settled++;
				continue;
			}

			const Vector3 hit_world_normal = gt.basis.xform(best_normal).normalized();
			if (max_slope_degrees < 90.0f) {
				const float angle = Math::rad_to_deg(Math::acos(CLAMP(hit_world_normal.dot(Vector3(0, 1, 0)), -1.0f, 1.0f)));
				if (angle > max_slope_degrees) {
					places_settled++;
					continue;
				}
			}

			local_pos = best_point;
			world_normal = hit_world_normal;
		} else {
			const float ly = rng.random(-half.y, half.y);
			local_pos = Vector3(lx, ly, lz);
		}

		// Only now that it is known where on the ground the instance stands:
		// a lake keeps foliage off the ground under its water, not off dry
		// ground inside its shoreline.
		if (!spline_keep_outs.is_empty()) {
			const Vector3 global_pos = gt.xform(local_pos);
			bool kept_out = false;
			for (const SplineKeepOut &keep_out : spline_keep_outs) {
				if (keep_out.covers(global_pos)) {
					kept_out = true;
					break;
				}
			}
			if (kept_out) {
				places_settled++;
				continue;
			}
		}

		Vector3 up_target(0, 1, 0);
		if (align_to_normal) {
			up_target = Vector3(0, 1, 0).lerp(world_normal, align_to_normal_amount);
			if (up_target.length_squared() < 0.0001f) {
				up_target = Vector3(0, 1, 0);
			} else {
				up_target.normalize();
			}
		}

		if (random_tilt_degrees > 0.0f) {
			Vector3 jitter_axis(rng.random(-1.0f, 1.0f), 0.0f, rng.random(-1.0f, 1.0f));
			if (jitter_axis.length_squared() > 0.0001f) {
				jitter_axis.normalize();
				const float jitter_angle = Math::deg_to_rad(rng.random(0.0f, random_tilt_degrees));
				up_target = up_target.rotated(jitter_axis, jitter_angle).normalized();
			}
		}

		const float yaw = random_rotation ? rng.random(0.0f, (float)Math::TAU) : 0.0f;
		Basis basis(Vector3(0, 1, 0), yaw);
		basis.rotate_to_align(Vector3(0, 1, 0), up_target);

		const float lo_scale = MIN(min_scale, max_scale);
		const float hi_scale = MAX(min_scale, max_scale);
		const float s = rng.random(lo_scale, hi_scale);
		basis = basis.scaled_local(Vector3(s, s, s));

		transforms.push_back(Transform3D(gt_inv.basis * basis, local_pos));
		places_settled++;

		if (min_distance > 0.0f) {
			grid[cell].push_back(Vector2(lx, lz));
		}
	}

	if (_is_gpu_culling_active()) {
		// Chunking exists to give the renderer something small to frustum-cull;
		// with the compute pass doing that per instance, the whole spawner is
		// one flat array feeding one MultiMesh per LOD level.
		gpu_transforms = transforms;
		_rebuild_gpu_instances();
	} else {
		_rebuild_cells_from_transforms(transforms);
	}

	update_gizmos();
	update_configuration_warnings();
	// Refreshes the read-only cell_count/instance_count Inspector display.
	notify_property_list_changed();
}

void FoliageSpawner3D::set_gpu_culling(bool p_enabled) {
	if (gpu_culling == p_enabled) {
		return;
	}
	gpu_culling = p_enabled;

	// The two paths hold the same instances in different shapes - flat for the
	// GPU, chunked into cells for the renderer - so toggling converts between
	// them instead of throwing the generated foliage away and waiting for the
	// next Regenerate.
	if (gpu_culling) {
		if (gpu_transforms.is_empty()) {
			gpu_transforms = _gather_cell_transforms();
		}
		_clear_cells();
		_rebuild_gpu_instances();
	} else {
		if (cells.is_empty() && !gpu_transforms.is_empty()) {
			_rebuild_cells_from_transforms(gpu_transforms);
		}
		_clear_gpu_instances();
		gpu_transforms.clear();
	}

	update_gizmos();
	update_configuration_warnings();
	notify_property_list_changed();
}

bool FoliageSpawner3D::is_gpu_culling_enabled() const {
	return gpu_culling;
}

void FoliageSpawner3D::fit_to_ground_mesh() {
	Node *ground_node = is_inside_tree() ? get_node_or_null(ground_mesh_path) : nullptr;
	MeshInstance3D *ground_mesh_instance = Object::cast_to<MeshInstance3D>(ground_node);
	Landscape3D *ground_terrain = Object::cast_to<Landscape3D>(ground_node);
	ERR_FAIL_COND_MSG(ground_mesh_instance == nullptr && ground_terrain == nullptr, "Ground Mesh Path does not point to a MeshInstance3D or Landscape3D, so there is nothing to fit the volume to.");

	// The ground's own bounds, in the ground node's local space.
	AABB ground_aabb;
	Transform3D ground_global_transform;

	if (ground_terrain != nullptr) {
		Ref<TerrainData> ground_terrain_data = ground_terrain->get_terrain_data();
		ERR_FAIL_COND_MSG(ground_terrain_data.is_null(), "The Landscape3D referenced by Ground Mesh Path has no TerrainData assigned.");

		// Not Landscape3D::get_aabb(): that one pads its height by thousands of
		// meters so it stays valid through sculpting without being recomputed,
		// which would make for an absurdly tall volume here. The heightmap's
		// real range is worth the one-off scan. The terrain starts at the
		// landscape's own origin and extends towards +X/+Z from there.
		const float terrain_size = ground_terrain_data->get_size();
		const Vector2 height_range = ground_terrain_data->get_height_range();
		ground_aabb = AABB(Vector3(0, height_range.x, 0), Vector3(terrain_size, height_range.y - height_range.x, terrain_size));
		ground_global_transform = ground_terrain->get_global_transform();
	} else {
		Ref<Mesh> ground_mesh = ground_mesh_instance->get_mesh();
		ERR_FAIL_COND_MSG(ground_mesh.is_null(), "The MeshInstance3D referenced by Ground Mesh Path has no Mesh assigned.");

		ground_aabb = ground_mesh->get_aabb();
		ground_global_transform = ground_mesh_instance->get_global_transform();
	}

	// Measured in this node's own space, so a rotated spawner fits the ground
	// in its own frame rather than to an axis-aligned box around it.
	const Transform3D global_transform = get_global_transform();
	const Transform3D ground_to_local = global_transform.affine_inverse() * ground_global_transform;
	AABB local_aabb = ground_to_local.xform(ground_aabb);

	// A ground with no height of its own (a PlaneMesh is perfectly flat, and so
	// is a Landscape3D that has not been sculpted yet) would leave the volume
	// with nothing to show and nothing for a MeshInstance3D's downward
	// projection ray to travel through, so give it a little.
	const real_t min_height = 1.0;
	if (local_aabb.size.y < min_height) {
		local_aabb.position.y -= (min_height - local_aabb.size.y) * 0.5;
		local_aabb.size.y = min_height;
	}

	// The volume is centered on this node's origin, so the node moves onto the
	// center of the ground and the volume then only has to carry its size.
	set_global_position(global_transform.xform(local_aabb.get_center()));
	set_volume_size(local_aabb.size);
}

AABB FoliageSpawner3D::get_aabb() const {
	const Vector3 sz = volume_size.abs();
	AABB box(sz * -0.5f, sz);
	// Cell nodes sit at this node's own local origin (see _sync_cell_lods), so
	// their MultiMesh AABBs are already in the same local space as `box`.
	for (const KeyValue<Vector2i, FoliageCell> &kv : cells) {
		for (const Ref<MultiMesh> &mm : kv.value.lod_multimeshes) {
			if (mm.is_valid()) {
				box.merge_with(mm->get_aabb());
			}
		}
	}
	return box;
}

Vector<AABB> FoliageSpawner3D::get_cell_local_aabbs() const {
	Vector<AABB> result;
	result.resize(cells.size());
	int i = 0;
	for (const KeyValue<Vector2i, FoliageCell> &kv : cells) {
		AABB cell_aabb;
		bool first = true;
		for (const Ref<MultiMesh> &mm : kv.value.lod_multimeshes) {
			if (mm.is_null()) {
				continue;
			}
			if (first) {
				cell_aabb = mm->get_aabb();
				first = false;
			} else {
				cell_aabb.merge_with(mm->get_aabb());
			}
		}
		result.write[i++] = cell_aabb;
	}
	return result;
}

PackedStringArray FoliageSpawner3D::get_configuration_warnings() const {
	PackedStringArray warnings = MultiMeshInstance3D::get_configuration_warnings();

	if (!has_any_mesh()) {
		warnings.push_back(RTR("No Mesh assigned on any LOD level. Add a Mesh via Lod Levels and press Regenerate to scatter instances."));
	}

	if (project_on_mesh) {
		Node *ground = is_inside_tree() ? get_node_or_null(ground_mesh_path) : nullptr;
		MeshInstance3D *ground_mesh_instance = Object::cast_to<MeshInstance3D>(ground);
		Landscape3D *ground_terrain = Object::cast_to<Landscape3D>(ground);
		if (ground_mesh_instance == nullptr && ground_terrain == nullptr) {
			warnings.push_back(RTR("Project On Mesh is enabled, but Ground Mesh Path does not point to a MeshInstance3D or Landscape3D. Assign one, or disable Project On Mesh."));
		} else if (ground_mesh_instance != nullptr && ground_mesh_instance->get_mesh().is_null()) {
			warnings.push_back(RTR("The MeshInstance3D referenced by Ground Mesh Path has no Mesh assigned."));
		} else if (ground_terrain != nullptr && ground_terrain->get_terrain_data().is_null()) {
			warnings.push_back(RTR("The Landscape3D referenced by Ground Mesh Path has no TerrainData assigned."));
		}
	}

	if (terrain_layer_mask != 0) {
		const Landscape3D *terrain = is_inside_tree() ? Object::cast_to<Landscape3D>(get_node_or_null(ground_mesh_path)) : nullptr;
		if (terrain == nullptr || terrain->get_terrain_data().is_null()) {
			warnings.push_back(RTR("Terrain Layer Mask only applies to a Landscape3D (with TerrainData) referenced by Ground Mesh Path, so it is ignored."));
		} else {
			const int layer_count = MIN(terrain->get_layers().size(), TerrainData::MAX_LAYERS);
			const uint32_t existing_layers = layer_count >= 32 ? 0xFFFFFFFF : (1u << layer_count) - 1;
			if ((terrain_layer_mask & existing_layers) == 0) {
				warnings.push_back(RTR("None of the layers chosen in Terrain Layer Mask exist on the Landscape3D referenced by Ground Mesh Path."));
			}
		}
	}

	if (has_any_mesh() && cells.is_empty() && gpu_transforms.is_empty()) {
		warnings.push_back(RTR("No instances have been generated yet (or none matched the current settings). Press Regenerate after adjusting Density, Min Distance, the Distribution Mask, the Terrain Layers, the Landscape Splines, or the ground projection settings."));
	}

	if (gpu_culling && !FoliageGPUCuller::is_supported()) {
		warnings.push_back(RTR("GPU Culling needs a RenderingDevice-based renderer (Forward+ or Mobile); the Compatibility renderer cannot draw indirect MultiMeshes. Falling back to cell chunking."));
	}

	if (gpu_culling && lod_levels.size() > FoliageGPUCuller::MAX_LOD_LEVELS) {
		warnings.push_back(vformat(RTR("GPU Culling only drives the first %d LOD levels; the rest are not rendered."), FoliageGPUCuller::MAX_LOD_LEVELS));
	}

	if (volume_size.x <= 0.0f || volume_size.y <= 0.0f || volume_size.z <= 0.0f) {
		warnings.push_back(RTR("Volume Size must be greater than zero on every axis."));
	}

	return warnings;
}

FoliageSpawner3D::FoliageSpawner3D() {
	// cell_gi_mode already defaults to GI_MODE_DISABLED (see the field
	// declaration): baked global illumination (LightmapGI probes, VoxelGI,
	// SDFGI) is generally not worth its cost for grass and other small
	// scattered foliage, the visual difference is minor at that scale,
	// LightmapGI would otherwise try to lightmap every scattered instance,
	// and dynamic per-instance GI probe lookups add up with thousands of
	// MultiMesh instances. Users who do want it can switch Cell GI Mode back
	// to Dynamic in the inspector.

	// Off every kind of spline by default: most foliage has no business on a
	// road or under water.
	spline_avoid = (1u << LandscapeSpline3D::TYPE_MAX) - 1;

	TypedArray<FoliageLODLevel> defaults;

	Ref<FoliageLODLevel> lod0;
	lod0.instantiate();
	lod0->set_visibility_range_end(30.0f);
	lod0->set_visibility_range_end_margin(5.0f);
	lod0->set_visibility_range_fade_mode(GeometryInstance3D::VISIBILITY_RANGE_FADE_SELF);
	defaults.push_back(lod0);

	Ref<FoliageLODLevel> lod1;
	lod1.instantiate();
	lod1->set_visibility_range_begin(25.0f);
	lod1->set_visibility_range_begin_margin(5.0f);
	lod1->set_visibility_range_end(80.0f);
	lod1->set_visibility_range_end_margin(10.0f);
	lod1->set_visibility_range_fade_mode(GeometryInstance3D::VISIBILITY_RANGE_FADE_SELF);
	defaults.push_back(lod1);

	Ref<FoliageLODLevel> lod2;
	lod2.instantiate();
	lod2->set_visibility_range_begin(70.0f);
	lod2->set_visibility_range_begin_margin(10.0f);
	lod2->set_visibility_range_fade_mode(GeometryInstance3D::VISIBILITY_RANGE_FADE_SELF);
	defaults.push_back(lod2);

	set_lod_levels(defaults);
}
