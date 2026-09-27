/**************************************************************************/
/*  test_landscape_quadtree.cpp                                           */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_landscape_quadtree)

#include "scene/3d/camera_3d.h"
#include "scene/3d/landscape_3d.h"
#include "scene/3d/landscape_quadtree.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"

namespace TestLandscapeQuadtree {

// Rugged ground: a few octaves of waves plus a little hashed noise, so that
// every level of the quadtree has some error to measure.
static LocalVector<float> make_rugged_heights(int p_resolution, uint32_t p_seed = 1) {
	LocalVector<float> heights;
	heights.resize(p_resolution * p_resolution);
	uint32_t state = p_seed;
	for (int z = 0; z < p_resolution; z++) {
		for (int x = 0; x < p_resolution; x++) {
			state = state * 1664525u + 1013904223u;
			const float noise = float(state >> 8) / float(1 << 24);
			heights[z * p_resolution + x] = 10.0f * Math::sin(x * 0.11f) * Math::cos(z * 0.07f) + 3.0f * Math::sin(x * 0.5f + z * 0.3f) + noise;
		}
	}
	return heights;
}

// The surface drawn with a vertex every p_stride samples, split along the
// same diagonal as the patch mesh, vertices past the last sample clamped onto
// it: what LandscapeQuadtree measures its errors against.
static float level_surface(const LocalVector<float> &p_heights, int p_resolution, int p_stride, int p_x, int p_z) {
	const int quads = p_resolution - 1;
	const int x0 = (p_x / p_stride) * p_stride;
	const int z0 = (p_z / p_stride) * p_stride;
	const int x1 = MIN(x0 + p_stride, quads);
	const int z1 = MIN(z0 + p_stride, quads);
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

static LandscapeQuadtree::SelectParams make_params(const Vector3 &p_camera, int p_min_level = 0, float p_micro_distance = 0.0f) {
	LandscapeQuadtree::SelectParams params;
	params.camera_position = p_camera;
	params.pixel_scale = 800.0f;
	params.pixel_error = 2.0f;
	params.max_quad_pixels = 48.0f;
	params.min_level = p_min_level;
	params.micro_distance = p_micro_distance;
	params.micro_quad_pixels = 6.0f;
	params.shadows = true;
	return params;
}

// Which level draws every quad of the finest grid the selection can reach,
// or INT_MIN where nothing does.
struct Coverage {
	int cells = 0;
	int scale = 1;
	LocalVector<int> levels;
	bool overlaps = false;

	Coverage(const LandscapeQuadtree &p_tree, int p_min_level, const LocalVector<LandscapeQuadtree::Patch> &p_patches) {
		scale = 1 << -p_min_level;
		cells = (p_tree.get_resolution() - 1) * scale;
		levels.resize(cells * cells);
		for (int &level : levels) {
			level = INT_MIN;
		}
		for (const LandscapeQuadtree::Patch &patch : p_patches) {
			const int size = LandscapeQuadtree::get_node_samples(patch.level) * scale;
			for (int z = patch.z * size; z < MIN((patch.z + 1) * size, cells); z++) {
				for (int x = patch.x * size; x < MIN((patch.x + 1) * size, cells); x++) {
					if (levels[z * cells + x] != INT_MIN) {
						overlaps = true;
					}
					levels[z * cells + x] = patch.level;
				}
			}
		}
	}

	int at(int p_x, int p_z) const {
		if (p_x < 0 || p_z < 0 || p_x >= cells || p_z >= cells) {
			return INT_MIN;
		}
		return levels[p_z * cells + p_x];
	}

	// How much coarser the patches along one edge of p_patch are, as the
	// selection should have recorded it.
	int coarser_across(const LandscapeQuadtree::Patch &p_patch, int p_dx, int p_dz) const {
		const int size = LandscapeQuadtree::get_node_samples(p_patch.level) * scale;
		const int x0 = p_patch.x * size;
		const int z0 = p_patch.z * size;
		int coarsest = INT_MIN;
		for (int i = 0; i < size; i++) {
			int x = 0;
			int z = 0;
			if (p_dx != 0) {
				x = p_dx < 0 ? x0 - 1 : x0 + size;
				z = z0 + i;
			} else {
				x = x0 + i;
				z = p_dz < 0 ? z0 - 1 : z0 + size;
			}
			coarsest = MAX(coarsest, at(x, z));
		}
		return coarsest == INT_MIN ? 0 : CLAMP(coarsest - p_patch.level, 0, LandscapeQuadtree::MAX_EDGE_DELTA);
	}
};

TEST_CASE("[Landscape3D][Quadtree] Node errors bound the distance to the full resolution surface") {
	// Not a multiple of the patch size, so the last nodes of every level
	// hang over the terrain's edge.
	const int resolution = 97;
	const LocalVector<float> heights = make_rugged_heights(resolution);
	LandscapeQuadtree tree;
	tree.build(heights.ptr(), nullptr, resolution, 1.5f);
	REQUIRE_FALSE(tree.is_empty());
	CHECK(tree.get_top_level() == 3);

	for (int level = 1; level <= tree.get_top_level(); level++) {
		const int size = LandscapeQuadtree::get_node_samples(level);
		for (int z = 0; z < tree.get_level_size(level); z++) {
			for (int x = 0; x < tree.get_level_size(level); x++) {
				if (!tree.is_node_inside(level, x, z)) {
					continue;
				}
				float deviation = 0.0f;
				for (int sz = z * size; sz <= MIN((z + 1) * size, resolution - 1); sz++) {
					for (int sx = x * size; sx <= MIN((x + 1) * size, resolution - 1); sx++) {
						deviation = MAX(deviation, Math::abs(heights[sz * resolution + sx] - level_surface(heights, resolution, 1 << level, sx, sz)));
					}
				}
				const LandscapeQuadtree::NodeBounds bounds = tree.get_node_bounds(level, x, z);
				CHECK(bounds.error + 0.0001f >= deviation);

				// And the node's box holds every sample in it.
				for (int sz = z * size; sz <= MIN((z + 1) * size, resolution - 1); sz++) {
					for (int sx = x * size; sx <= MIN((x + 1) * size, resolution - 1); sx++) {
						const float h = heights[sz * resolution + sx];
						if (h < bounds.min.y || h > bounds.max.y) {
							FAIL("A sample lies outside its node's bounds.");
						}
					}
				}
			}
		}
	}
}

TEST_CASE("[Landscape3D][Quadtree] The selection covers the terrain exactly once, and knows each coarser neighbor") {
	const int resolution = 129;
	const LocalVector<float> heights = make_rugged_heights(resolution, 7);
	LandscapeQuadtree tree;
	tree.build(heights.ptr(), nullptr, resolution, 1.0f);

	const Vector3 cameras[3] = { Vector3(40, 25, 40), Vector3(-30, 60, 64), Vector3(100, 14, 20) };
	for (const Vector3 &camera : cameras) {
		for (int min_level = 0; min_level >= -2; min_level -= 2) {
			const LandscapeQuadtree::SelectParams params = make_params(camera, min_level, min_level < 0 ? 30.0f : 0.0f);
			LandscapeQuadtree::Selection selection;
			tree.select(params, selection);
			REQUIRE(selection.visible.size() > 0);

			const Coverage coverage(tree, min_level, selection.visible);
			CHECK_FALSE(coverage.overlaps);
			bool full = true;
			for (int level : coverage.levels) {
				full = full && level != INT_MIN;
			}
			CHECK(full);

			// The recorded coarser neighbor across every edge is what the
			// shader stitches to: getting it wrong opens a crack.
			bool neighbors_right = true;
			for (const LandscapeQuadtree::Patch &patch : selection.visible) {
				neighbors_right = neighbors_right && int(patch.edges & 15) == coverage.coarser_across(patch, -1, 0);
				neighbors_right = neighbors_right && int((patch.edges >> 4) & 15) == coverage.coarser_across(patch, 1, 0);
				neighbors_right = neighbors_right && int((patch.edges >> 8) & 15) == coverage.coarser_across(patch, 0, -1);
				neighbors_right = neighbors_right && int((patch.edges >> 12) & 15) == coverage.coarser_across(patch, 0, 1);
			}
			CHECK(neighbors_right);

			// Nothing is culled without frustum culling, and everything casts
			// shadows with no shadow distance.
			CHECK(selection.shadow.size() == selection.visible.size());
		}
	}
}

TEST_CASE("[Landscape3D][Quadtree] Patches are split exactly as far as the screen-space error asks") {
	const int resolution = 129;
	const LocalVector<float> heights = make_rugged_heights(resolution, 3);
	LandscapeQuadtree tree;
	tree.build(heights.ptr(), nullptr, resolution, 1.0f);

	const LandscapeQuadtree::SelectParams params = make_params(Vector3(64, 20, 64));
	LandscapeQuadtree::Selection selection;
	tree.select(params, selection);

	bool leaves_fine = true;
	bool ancestors_split = true;
	for (const LandscapeQuadtree::Patch &patch : selection.visible) {
		leaves_fine = leaves_fine && !tree.should_subdivide(params, patch.level, patch.x, patch.z);
		for (int level = patch.level + 1; level <= tree.get_top_level(); level++) {
			const int shift = level - patch.level;
			ancestors_split = ancestors_split && tree.should_subdivide(params, level, patch.x >> shift, patch.z >> shift);
		}
	}
	CHECK(leaves_fine);
	CHECK(ancestors_split);

	// Seen from far enough that not everything is at the finest level, a
	// stricter error splits further: more, smaller patches.
	LandscapeQuadtree::SelectParams distant = params;
	distant.camera_position = Vector3(64, 300, -600);
	LandscapeQuadtree::Selection distant_selection;
	tree.select(distant, distant_selection);
	LandscapeQuadtree::SelectParams strict = distant;
	strict.pixel_error = 0.5f;
	LandscapeQuadtree::Selection strict_selection;
	tree.select(strict, strict_selection);
	CHECK(strict_selection.visible.size() > distant_selection.visible.size());

	// Flat ground needs no detail for its own sake: only the quad size limit
	// splits it, so it ends up with far fewer patches than rugged ground.
	LocalVector<float> flat;
	flat.resize(resolution * resolution);
	for (float &h : flat) {
		h = 3.0f;
	}
	LandscapeQuadtree flat_tree;
	flat_tree.build(flat.ptr(), nullptr, resolution, 1.0f);
	LandscapeQuadtree::Selection flat_selection;
	flat_tree.select(params, flat_selection);
	CHECK(flat_selection.visible.size() < selection.visible.size());
}

TEST_CASE("[Landscape3D][Quadtree] Updating a region gives the same nodes as rebuilding") {
	const int resolution = 97;
	LocalVector<float> heights = make_rugged_heights(resolution, 11);
	LocalVector<uint8_t> holes;
	holes.resize(resolution * resolution);
	memset(holes.ptr(), 0, holes.size());

	LandscapeQuadtree updated;
	updated.build(heights.ptr(), holes.ptr(), resolution, 2.0f);
	LocalVector<float> before;
	for (float value : updated.get_node_data()) {
		before.push_back(value);
	}

	// A mound and a hole, the mound's edge right on a level 0 node border.
	const Rect2i mound(32, 20, 25, 30);
	for (int z = mound.position.y; z < mound.get_end().y; z++) {
		for (int x = mound.position.x; x < mound.get_end().x; x++) {
			heights[z * resolution + x] += 6.0f;
		}
	}
	const Rect2i hole(70, 70, 5, 5);
	for (int z = hole.position.y; z < hole.get_end().y; z++) {
		for (int x = hole.position.x; x < hole.get_end().x; x++) {
			holes[z * resolution + x] = 1;
		}
	}

	LocalVector<Vector2i> ranges;
	updated.update(heights.ptr(), holes.ptr(), mound, &ranges);
	updated.update(heights.ptr(), holes.ptr(), hole, &ranges);

	LandscapeQuadtree rebuilt;
	rebuilt.build(heights.ptr(), holes.ptr(), resolution, 2.0f);

	const LocalVector<float> &after = updated.get_node_data();
	const LocalVector<float> &expected = rebuilt.get_node_data();
	REQUIRE(after.size() == expected.size());
	bool same = true;
	for (uint32_t i = 0; i < after.size(); i++) {
		same = same && after[i] == expected[i];
	}
	CHECK(same);

	// Every node that changed lies in a reported range, so uploading only
	// those keeps the GPU's copy exact.
	LocalVector<bool> reported;
	reported.resize(updated.get_node_count());
	for (uint32_t i = 0; i < reported.size(); i++) {
		reported[i] = false;
	}
	for (const Vector2i &range : ranges) {
		for (int i = range.x; i < range.x + range.y; i++) {
			reported[i] = true;
		}
	}
	bool all_reported = true;
	for (uint32_t node = 0; node < updated.get_node_count(); node++) {
		bool changed = false;
		for (int i = 0; i < LandscapeQuadtree::NODE_FLOATS; i++) {
			changed = changed || before[node * LandscapeQuadtree::NODE_FLOATS + i] != after[node * LandscapeQuadtree::NODE_FLOATS + i];
		}
		all_reported = all_reported && (!changed || reported[node]);
	}
	CHECK(all_reported);
}

TEST_CASE("[Landscape3D][Quadtree] Holes refine the ground around them and are not drawn") {
	const int resolution = 129;
	LocalVector<float> heights;
	heights.resize(resolution * resolution);
	for (float &h : heights) {
		h = 0.0f;
	}
	LocalVector<uint8_t> holes;
	holes.resize(resolution * resolution);
	memset(holes.ptr(), 0, holes.size());
	// Exactly level 0 nodes (1, 1) to (2, 2).
	for (int z = 16; z <= 48; z++) {
		for (int x = 16; x <= 48; x++) {
			holes[z * resolution + x] = 1;
		}
	}

	LandscapeQuadtree tree;
	tree.build(heights.ptr(), holes.ptr(), resolution, 1.0f);
	CHECK((tree.get_node_bounds(0, 1, 1).flags & LandscapeQuadtree::NODE_FLAG_ALL_HOLE) != 0);
	CHECK((tree.get_node_bounds(0, 0, 0).flags & LandscapeQuadtree::NODE_FLAG_HAS_HOLE) != 0);
	// Samples on a node's edge belong to both nodes sharing it.
	CHECK((tree.get_node_bounds(0, 3, 3).flags & LandscapeQuadtree::NODE_FLAG_HAS_HOLE) != 0);
	CHECK((tree.get_node_bounds(0, 5, 5).flags & LandscapeQuadtree::NODE_FLAG_HAS_HOLE) == 0);
	// Flat as the ground is, a node with a hole in it counts its quad size as
	// error, so that the hole's outline is refined up close.
	CHECK(tree.get_node_bounds(1, 0, 0).error >= 2.0f);
	LandscapeQuadtree solid;
	solid.build(heights.ptr(), nullptr, resolution, 1.0f);
	CHECK(solid.get_node_bounds(1, 0, 0).error == 0.0f);

	LandscapeQuadtree::Selection selection;
	tree.select(make_params(Vector3(32, 10, 32)), selection);
	bool hole_skipped = true;
	for (const LandscapeQuadtree::Patch &patch : selection.visible) {
		if (patch.level == 0) {
			hole_skipped = hole_skipped && !(patch.x >= 1 && patch.x <= 2 && patch.z >= 1 && patch.z <= 2);
		}
	}
	CHECK(hole_skipped);
}

TEST_CASE("[Landscape3D][Quadtree] Micro detail splits past the heightmap only near the camera") {
	const int resolution = 129;
	const LocalVector<float> heights = make_rugged_heights(resolution, 5);
	LandscapeQuadtree tree;
	tree.build(heights.ptr(), nullptr, resolution, 1.0f);

	const Vector3 camera(64, heights[64 * resolution + 64] + 2.0f, 64);
	LandscapeQuadtree::Selection selection;

	tree.select(make_params(camera, 0, 0.0f), selection);
	int finest = INT_MAX;
	for (const LandscapeQuadtree::Patch &patch : selection.visible) {
		finest = MIN(finest, patch.level);
	}
	CHECK(finest == 0);

	const float micro_distance = 20.0f;
	LandscapeQuadtree::SelectParams params = make_params(camera, -3, micro_distance);
	params.displacement_bound = 0.25f;
	tree.select(params, selection);
	finest = INT_MAX;
	bool only_near = true;
	for (const LandscapeQuadtree::Patch &patch : selection.visible) {
		finest = MIN(finest, patch.level);
		if (patch.level < 0) {
			// Split from a node reaching within micro_distance (the patch
			// itself may lie a little further).
			const LandscapeQuadtree::NodeBounds parent = tree.get_node_bounds(patch.level + 1, patch.x >> 1, patch.z >> 1, params.displacement_bound);
			const Vector3 closest = camera.clamp(parent.min, parent.max);
			only_near = only_near && closest.distance_to(camera) < micro_distance;
		}
	}
	CHECK(finest == -3);
	CHECK(only_near);
}

TEST_CASE("[Landscape3D][Quadtree] Frustum and shadow distance culling") {
	const int resolution = 129;
	const LocalVector<float> heights = make_rugged_heights(resolution, 9);
	LandscapeQuadtree tree;
	tree.build(heights.ptr(), nullptr, resolution, 1.0f);

	// Looking along +X from the middle of the terrain.
	LandscapeQuadtree::SelectParams params = make_params(Vector3(64, 30, 64));
	Projection projection;
	projection.set_perspective(70.0, 16.0 / 9.0, 0.05, 1000.0);
	const Transform3D camera_transform = Transform3D().looking_at(Vector3(1, -0.2, 0)).translated(params.camera_position);
	const Vector<Plane> planes = projection.get_projection_planes(camera_transform);
	for (int i = 0; i < 6; i++) {
		params.frustum[i] = planes[i];
	}
	params.frustum_culling = true;
	params.shadow_distance = 40.0f;

	LandscapeQuadtree::Selection selection;
	tree.select(params, selection);

	LandscapeQuadtree::SelectParams unculled = params;
	unculled.frustum_culling = false;
	unculled.shadow_distance = 0.0f;
	LandscapeQuadtree::Selection all;
	tree.select(unculled, all);

	CHECK(selection.visible.size() < all.visible.size());
	CHECK(selection.shadow.size() < all.shadow.size());

	bool none_behind = true;
	for (const LandscapeQuadtree::Patch &patch : selection.visible) {
		const LandscapeQuadtree::NodeBounds bounds = tree.get_node_bounds(patch.level, patch.x, patch.z);
		none_behind = none_behind && bounds.max.x >= params.camera_position.x - 1.0f;
	}
	CHECK(none_behind);

	bool shadows_near = true;
	for (const LandscapeQuadtree::Patch &patch : selection.shadow) {
		const LandscapeQuadtree::NodeBounds bounds = tree.get_node_bounds(patch.level, patch.x, patch.z);
		shadows_near = shadows_near && params.camera_position.clamp(bounds.min, bounds.max).distance_to(params.camera_position) <= params.shadow_distance;
	}
	CHECK(shadows_near);

	// The selection itself does not depend on which way the camera looks:
	// every patch drawn is one the unculled selection has too.
	HashSet<uint64_t> all_keys;
	for (const LandscapeQuadtree::Patch &patch : all.visible) {
		all_keys.insert(uint64_t(patch.level + 16) | (uint64_t(patch.x) << 8) | (uint64_t(patch.z) << 32));
	}
	bool subset = true;
	for (const LandscapeQuadtree::Patch &patch : selection.visible) {
		subset = subset && all_keys.has(uint64_t(patch.level + 16) | (uint64_t(patch.x) << 8) | (uint64_t(patch.z) << 32));
	}
	CHECK(subset);
}

TEST_CASE("[Landscape3D][Quadtree] Instances survive half-float storage") {
	// The Compatibility renderer keeps a MultiMesh's instance color and custom
	// data as half floats: every value the shader reads back has to be a whole
	// number no larger than 2048 for that to be exact.
	LandscapeQuadtree::Patch patches[3];
	patches[0].level = 0;
	patches[0].x = 255;
	patches[0].z = 3;
	patches[0].edges = 0xF12A;
	patches[1].level = -4;
	patches[1].x = 4095;
	patches[1].z = 4000;
	patches[1].edges = 0x0001;
	patches[2].level = 8;
	patches[2].edges = 0xFFFF;

	for (const LandscapeQuadtree::Patch &patch : patches) {
		float instance[LandscapeQuadtree::INSTANCE_FLOATS];
		LandscapeQuadtree::write_instance(patch, instance);
		for (int i = 12; i < LandscapeQuadtree::INSTANCE_FLOATS; i++) {
			CHECK(instance[i] == Math::floor(instance[i]));
			CHECK(instance[i] >= 0.0f);
			CHECK(instance[i] <= 2048.0f);
			CHECK(Math::half_to_float(Math::make_half_float(instance[i])) == instance[i]);
		}

		// Decoded the way the terrain shader does.
		const int size = LandscapeQuadtree::get_node_samples(patch.level);
		CHECK(int(instance[12] * LandscapeQuadtree::ORIGIN_SPLIT + instance[13]) == patch.x * size);
		CHECK(int(instance[14] * LandscapeQuadtree::ORIGIN_SPLIT + instance[15]) == patch.z * size);
		CHECK(int(instance[16]) - LandscapeQuadtree::LEVEL_BIAS == patch.level);
		CHECK(uint32_t(int(instance[17]) | (int(instance[18]) << 8)) == patch.edges);
	}
}

TEST_CASE("[SceneTree][Landscape3D] The LOD follows the viewport's camera") {
	Ref<TerrainData> data;
	data.instantiate();
	data->set_resolution(65);
	data->set_vertex_spacing(2.0f);

	Landscape3D *landscape = memnew(Landscape3D);
	landscape->set_terrain_data(data);
	landscape->set_position(Vector3(10, 0, 0));
	Camera3D *camera = memnew(Camera3D);
	SceneTree::get_singleton()->get_root()->add_child(landscape);
	SceneTree::get_singleton()->get_root()->add_child(camera);
	camera->set_position(Vector3(40, 15, 60));
	camera->make_current();

	LandscapeQuadtree::SelectParams params;
	REQUIRE(landscape->get_lod_params(params));
	// In the landscape's own space.
	CHECK(params.camera_position.is_equal_approx(Vector3(30, 15, 60)));
	CHECK(params.frustum_culling);
	CHECK(params.pixel_scale > 0.0f);
	CHECK(params.min_level == -landscape->get_micro_detail_levels());

	LandscapeQuadtree::Selection selection;
	landscape->get_quadtree().select(params, selection);
	CHECK(selection.visible.size() > 0);

	landscape->set_frustum_culling(false);
	REQUIRE(landscape->get_lod_params(params));
	CHECK_FALSE(params.frustum_culling);

	memdelete(camera);
	memdelete(landscape);
}

} // namespace TestLandscapeQuadtree
