/**************************************************************************/
/*  test_landscape_3d.cpp                                                 */
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

TEST_FORCE_LINK(test_landscape_3d)

#include "core/templates/rid_owner.h"
#include "scene/3d/landscape_3d.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/3d/physics/static_body_3d.h"
#include "scene/3d/terrain_data.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/height_map_shape_3d.h"
#include "servers/rendering/renderer_scene_occlusion_cull.h"

namespace TestLandscape3D {

// A terrain that is flat except for a tall, flat topped hill running across
// the middle of it, along X.
static Ref<TerrainData> make_hill_terrain(int p_resolution = 33, float p_spacing = 2.0f, float p_height = 8.0f) {
	Ref<TerrainData> data;
	data.instantiate();
	data->set_resolution(p_resolution);
	data->set_vertex_spacing(p_spacing);

	for (int z = 0; z < p_resolution; z++) {
		const bool on_hill = z >= 8 && z <= 24;
		for (int x = 0; x < p_resolution; x++) {
			data->set_height(x, z, on_hill ? p_height : 0.0f);
		}
	}
	return data;
}

// Renders the terrain's occluders from a camera standing in front of the hill
// (at the low end of the terrain, looking along +Z, over the hill), and
// answers whether a given box is hidden by them.
class LandscapeOcclusionTester {
	RID_Owner<int> rids;
	RID buffer;

public:
	Transform3D camera_transform = Transform3D(Basis(Vector3(0, 1, 0), Math::PI), Vector3(32, 2, 4));
	Projection camera_projection;

	explicit LandscapeOcclusionTester(RID p_scenario) {
		buffer = rids.make_rid();
		RendererSceneOcclusionCull::get_singleton()->add_buffer(buffer);
		RendererSceneOcclusionCull::get_singleton()->buffer_set_scenario(buffer, p_scenario);
		RendererSceneOcclusionCull::get_singleton()->buffer_set_size(buffer, Size2i(128, 128));
		camera_projection.set_perspective(70.0, 1.0, 0.05, 1000.0);
	}

	~LandscapeOcclusionTester() {
		RendererSceneOcclusionCull::get_singleton()->remove_buffer(buffer);
		for (const RID &rid : rids.get_owned_list()) {
			rids.free(rid);
		}
	}

	void draw() {
		RendererSceneOcclusionCull::get_singleton()->buffer_update(buffer, camera_transform, camera_projection, false);
	}

	bool is_occluded(const AABB &p_aabb) {
		const Vector3 end = p_aabb.position + p_aabb.size;
		const real_t bounds[6] = { p_aabb.position.x, p_aabb.position.y, p_aabb.position.z, end.x, end.y, end.z };

		RendererSceneOcclusionCull::HZBuffer *hzb = RendererSceneOcclusionCull::get_singleton()->buffer_get_ptr(buffer);
		uint64_t occlusion_timeout = 0;
		return hzb->is_occluded(bounds, camera_transform.origin, camera_transform.affine_inverse(), camera_projection, camera_projection.get_z_near(), false, occlusion_timeout);
	}
};

// Stands in for a patch of vegetation: the small, ground hugging box a
// FoliageSpawner3D/FoliagePainter3D cell's multimesh would have as its bounds.
static AABB foliage_cell_at(float p_x, float p_z, float p_ground_height = 0.0f) {
	return AABB(Vector3(p_x - 8.0f, p_ground_height, p_z - 8.0f), Vector3(16.0f, 2.0f, 16.0f));
}

TEST_CASE("[SceneTree][Landscape3D] A hill occludes the vegetation standing behind it") {
	Landscape3D *landscape = memnew(Landscape3D);
	landscape->set_terrain_data(make_hill_terrain());
	SceneTree::get_singleton()->get_root()->add_child(landscape);

	LandscapeOcclusionTester tester(landscape->get_world_3d()->get_scenario());
	tester.draw();

	SUBCASE("Behind the hill") {
		CHECK(tester.is_occluded(foliage_cell_at(32, 56)));
		CHECK(tester.is_occluded(foliage_cell_at(12, 60)));
	}

	SUBCASE("On this side of the hill") {
		CHECK_FALSE(tester.is_occluded(foliage_cell_at(32, 14)));
	}

	SUBCASE("Tall enough to be seen over the hill") {
		// A 30 unit tower behind the hill still pokes out above its ridge.
		CHECK_FALSE(tester.is_occluded(AABB(Vector3(28, 0, 52), Vector3(8, 30, 8))));
	}

	SUBCASE("Looked down on from above the hill") {
		tester.camera_transform.origin = Vector3(32, 40, 4);
		tester.camera_transform.basis = Basis(Vector3(0, 1, 0), Math::PI) * Basis(Vector3(1, 0, 0), Math::deg_to_rad(-20.0));
		tester.draw();

		CHECK_FALSE(tester.is_occluded(foliage_cell_at(32, 56)));
		CHECK_FALSE(tester.is_occluded(foliage_cell_at(32, 32, 8.0f)));
	}

	SUBCASE("With the terrain's occluders turned off") {
		landscape->set_occluder_enabled(false);
		tester.draw();
		CHECK_FALSE(tester.is_occluded(foliage_cell_at(32, 56)));

		landscape->set_occluder_enabled(true);
		tester.draw();
		CHECK(tester.is_occluded(foliage_cell_at(32, 56)));
	}

	SUBCASE("At every occluder detail level") {
		for (int detail = 1; detail <= 32; detail *= 2) {
			landscape->set_occluder_detail(detail);
			CHECK(landscape->get_occluder_detail() == detail);
			tester.draw();

			// However coarse the simplification gets, it must never claim to
			// hide something the terrain does not: a coarser occluder may only
			// ever cull less.
			CHECK_FALSE(tester.is_occluded(foliage_cell_at(32, 14)));
			CHECK_FALSE(tester.is_occluded(AABB(Vector3(28, 0, 52), Vector3(8, 30, 8))));

			// The levels that still have several quads across the hill do keep
			// hiding what is behind it. The coarsest ones flatten it into the
			// surrounding ground (every vertex drops to the lowest sample
			// around it), which is a loss of culling, not of correctness.
			if (detail >= 8) {
				CHECK(tester.is_occluded(foliage_cell_at(32, 56)));
			}
		}
	}

	memdelete(landscape);
}

TEST_CASE("[SceneTree][Landscape3D] Holes in the terrain are not occluding") {
	Landscape3D *landscape = memnew(Landscape3D);
	Ref<TerrainData> data = make_hill_terrain();
	landscape->set_terrain_data(data);
	SceneTree::get_singleton()->get_root()->add_child(landscape);

	LandscapeOcclusionTester tester(landscape->get_world_3d()->get_scenario());
	tester.draw();
	CHECK(tester.is_occluded(foliage_cell_at(32, 56)));

	// Punch a hole straight through the hill, in front of the same cell: it is
	// visible through it now, so it may no longer be culled.
	landscape->set_hole(Vector3(32, 0, 32), 24.0f, true);
	tester.draw();
	CHECK_FALSE(tester.is_occluded(foliage_cell_at(32, 56)));

	memdelete(landscape);
}

TEST_CASE("[SceneTree][Landscape3D] Sculpting the terrain updates its occluders") {
	Landscape3D *landscape = memnew(Landscape3D);
	landscape->set_terrain_data(make_hill_terrain(33, 2.0f, 0.0f));
	SceneTree::get_singleton()->get_root()->add_child(landscape);

	LandscapeOcclusionTester tester(landscape->get_world_3d()->get_scenario());
	tester.draw();

	// Flat ground to start with: nothing to hide behind.
	CHECK_FALSE(tester.is_occluded(foliage_cell_at(32, 56)));

	// Raise a hill between the camera and the cell.
	for (int i = 0; i < 40; i++) {
		landscape->sculpt(Vector3(32, 0, 32), 20.0f, 1.0f, Landscape3D::SCULPT_RAISE);
	}
	tester.draw();
	CHECK(tester.is_occluded(foliage_cell_at(32, 56)));

	memdelete(landscape);
}

// Every collision tile of the landscape, with the heights its shape holds
// checked against the terrain's own. Returns how many samples of the terrain
// no tile covers.
static int check_collision_tiles(Landscape3D *p_landscape, Vector<const HeightMapShape3D *> &r_shapes) {
	const Ref<TerrainData> data = p_landscape->get_terrain_data();
	const int resolution = data->get_resolution();
	const float spacing = data->get_vertex_spacing();
	Vector<uint8_t> covered;
	covered.resize(resolution * resolution);
	covered.fill(0);
	r_shapes.clear();

	StaticBody3D *body = p_landscape->get_collision_body();
	REQUIRE(body != nullptr);
	for (int i = 0; i < body->get_child_count(true); i++) {
		CollisionShape3D *node = Object::cast_to<CollisionShape3D>(body->get_child(i, true));
		REQUIRE(node != nullptr);
		const Ref<HeightMapShape3D> shape = node->get_shape();
		REQUIRE(shape.is_valid());
		r_shapes.push_back(shape.ptr());

		// Square, so that Jolt Physics builds it as a height field.
		const int size = shape->get_map_width();
		CHECK(shape->get_map_depth() == size);
		CHECK(node->get_scale().is_equal_approx(Vector3(spacing, 1, spacing)));
		const Vector3 position = node->get_position() / spacing - Vector3(size - 1, 0, size - 1) * 0.5;
		const Vector2i origin(Math::round(position.x), Math::round(position.z));
		CHECK(origin.x >= 0);
		CHECK(origin.y >= 0);
		CHECK(origin.x + size <= resolution);
		CHECK(origin.y + size <= resolution);

		const Vector<real_t> heights = shape->get_map_data();
		int mismatches = 0;
		for (int z = 0; z < size; z++) {
			for (int x = 0; x < size; x++) {
				const int sx = origin.x + x;
				const int sz = origin.y + z;
				if (sx >= resolution || sz >= resolution) {
					continue;
				}
				covered.write[sz * resolution + sx] = 1;
				if (heights[z * size + x] != data->get_height(sx, sz)) {
					mismatches++;
				}
			}
		}
		CHECK_MESSAGE(mismatches == 0, vformat("Collision tile at %s holds %d stale heights.", origin, mismatches));
	}

	int uncovered = 0;
	for (int i = 0; i < covered.size(); i++) {
		uncovered += covered[i] == 0 ? 1 : 0;
	}
	return uncovered;
}

TEST_CASE("[SceneTree][Landscape3D] Collision is built in tiles that follow edits") {
	// Not a multiple of the tile size, so that the last tiles overlap.
	const int resolution = Landscape3D::COLLISION_TILE_QUADS * 2 + 90;
	Ref<TerrainData> data;
	data.instantiate();
	data->set_resolution(resolution);
	data->set_vertex_spacing(2.0f);
	for (int z = 0; z < resolution; z++) {
		for (int x = 0; x < resolution; x++) {
			data->set_height(x, z, float((x * 7 + z * 13) % 50));
		}
	}

	Landscape3D *landscape = memnew(Landscape3D);
	landscape->set_terrain_data(data);
	SceneTree::get_singleton()->get_root()->add_child(landscape);

	Vector<const HeightMapShape3D *> shapes;
	CHECK(check_collision_tiles(landscape, shapes) == 0);
	CHECK(shapes.size() == 9);
	for (const HeightMapShape3D *shape : shapes) {
		CHECK(shape->get_map_width() == Landscape3D::COLLISION_TILE_QUADS + 1);
	}

	SUBCASE("An edit only rebuilds the tiles under it") {
		Vector<Vector<real_t>> before;
		for (const HeightMapShape3D *shape : shapes) {
			before.push_back(shape->get_map_data());
		}

		// Well inside the first tile.
		landscape->sculpt(Vector3(100, 0, 100), 20.0f, 5.0f, Landscape3D::SCULPT_RAISE);
		CHECK(check_collision_tiles(landscape, shapes) == 0);
		int rebuilt = 0;
		for (int i = 0; i < shapes.size(); i++) {
			rebuilt += shapes[i]->get_map_data().ptr() != before[i].ptr() ? 1 : 0;
		}
		CHECK(rebuilt == 1);
	}

	SUBCASE("Edits that leave collision alone are picked up by update_collision()") {
		// On a corner shared by all four tiles of the far corner, and on the
		// overlap of the last ones.
		const Vector3 corner = Vector3(Landscape3D::COLLISION_TILE_QUADS * 2, 0, Landscape3D::COLLISION_TILE_QUADS * 2) * 2.0f;
		landscape->sculpt(corner, 30.0f, 5.0f, Landscape3D::SCULPT_RAISE, 1.0f, 0.0f, false);
		landscape->sculpt(Vector3(resolution - 20, 0, resolution - 20) * 2.0f, 30.0f, 5.0f, Landscape3D::SCULPT_LOWER, 1.0f, 0.0f, false);
		landscape->update_collision();
		CHECK(check_collision_tiles(landscape, shapes) == 0);
	}

	SUBCASE("Batched region edits") {
		TypedArray<Rect2i> regions;
		TypedArray<PackedFloat32Array> heights;
		for (int i = 0; i < 3; i++) {
			const Rect2i region(i * 250 + 5, i * 250 + 5, 10, 10);
			PackedFloat32Array values;
			values.resize(100);
			values.fill(-3.0f);
			regions.push_back(region);
			heights.push_back(values);
		}
		landscape->set_height_regions(regions, heights, true);
		CHECK(check_collision_tiles(landscape, shapes) == 0);
	}

	SUBCASE("A new resolution lays the tiles out again") {
		data->set_resolution(65);
		CHECK(check_collision_tiles(landscape, shapes) == 0);
		CHECK(shapes.size() == 1);
		CHECK(shapes[0]->get_map_width() == 65);
	}

	memdelete(landscape);
}

} // namespace TestLandscape3D
