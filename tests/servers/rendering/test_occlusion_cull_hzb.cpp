/**************************************************************************/
/*  test_occlusion_cull_hzb.cpp                                           */
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

TEST_FORCE_LINK(test_occlusion_cull_hzb)

#include "core/templates/rid_owner.h"
#include "servers/rendering/renderer_scene_occlusion_cull_hzb.h"

namespace TestOcclusionCullHZB {

// Drives HZBOcclusionCull the way the rendering server does: one scenario, one
// depth buffer, a camera, and occluder instances added to it by hand.
class OcclusionTestScene {
	HZBOcclusionCull cull;
	// The backend only ever uses scenario, instance and buffer RIDs as keys,
	// so any distinct RIDs will do here.
	RID_Owner<int> rids;
	RID scenario;
	RID buffer;
	LocalVector<RID> occluders;

public:
	Transform3D camera_transform;
	Projection camera_projection;
	bool orthogonal = false;

	explicit OcclusionTestScene(const Size2i &p_buffer_size = Size2i(128, 128)) {
		scenario = rids.make_rid();
		buffer = rids.make_rid();

		cull.add_scenario(scenario);
		cull.add_buffer(buffer);
		cull.buffer_set_scenario(buffer, scenario);
		cull.buffer_set_size(buffer, p_buffer_size);

		camera_projection.set_perspective(70.0, 1.0, 0.05, 1000.0);
	}

	~OcclusionTestScene() {
		cull.remove_buffer(buffer);
		cull.remove_scenario(scenario);
		for (const RID &occluder : occluders) {
			cull.free_occluder(occluder);
		}
		for (const RID &rid : rids.get_owned_list()) {
			rids.free(rid);
		}
	}

	struct OccluderHandle {
		RID occluder;
		RID instance;
	};

	OccluderHandle add_occluder(const PackedVector3Array &p_vertices, const PackedInt32Array &p_indices, const Transform3D &p_xform = Transform3D()) {
		OccluderHandle handle;
		handle.occluder = cull.occluder_allocate();
		cull.occluder_initialize(handle.occluder);
		cull.occluder_set_mesh(handle.occluder, p_vertices, p_indices);
		occluders.push_back(handle.occluder);

		handle.instance = rids.make_rid();
		cull.scenario_set_instance(scenario, handle.instance, handle.occluder, p_xform, true);
		return handle;
	}

	void update_instance(const OccluderHandle &p_handle, bool p_enabled, const Transform3D &p_xform = Transform3D()) {
		cull.scenario_set_instance(scenario, p_handle.instance, p_handle.occluder, p_xform, p_enabled);
	}

	void remove_instance(const OccluderHandle &p_handle) {
		cull.scenario_remove_instance(scenario, p_handle.instance);
	}

	void draw() {
		cull.buffer_update(buffer, camera_transform, camera_projection, orthogonal);
	}

	bool is_occluded(const AABB &p_aabb) {
		const Vector3 end = p_aabb.position + p_aabb.size;
		const real_t bounds[6] = { p_aabb.position.x, p_aabb.position.y, p_aabb.position.z, end.x, end.y, end.z };

		RendererSceneOcclusionCull::HZBuffer *hzb = cull.buffer_get_ptr(buffer);
		uint64_t occlusion_timeout = 0;
		return hzb->is_occluded(bounds, camera_transform.origin, camera_transform.affine_inverse(), camera_projection, camera_projection.get_z_near(), orthogonal, occlusion_timeout);
	}
};

// An axis aligned quad facing the camera (which looks down -Z from the
// origin), centered on the -Z axis.
static void make_wall(float p_z, float p_half_extent, PackedVector3Array &r_vertices, PackedInt32Array &r_indices, float p_center_x = 0.0f) {
	r_vertices.clear();
	r_vertices.push_back(Vector3(p_center_x - p_half_extent, -p_half_extent, p_z));
	r_vertices.push_back(Vector3(p_center_x + p_half_extent, -p_half_extent, p_z));
	r_vertices.push_back(Vector3(p_center_x + p_half_extent, p_half_extent, p_z));
	r_vertices.push_back(Vector3(p_center_x - p_half_extent, p_half_extent, p_z));

	r_indices.clear();
	r_indices.push_back(0);
	r_indices.push_back(1);
	r_indices.push_back(2);
	r_indices.push_back(0);
	r_indices.push_back(2);
	r_indices.push_back(3);
}

static AABB box_at(const Vector3 &p_center, float p_half_size = 0.5f) {
	return AABB(p_center - Vector3(p_half_size, p_half_size, p_half_size), Vector3(p_half_size, p_half_size, p_half_size) * 2.0f);
}

TEST_CASE("[HZBOcclusionCull] A scene without occluders occludes nothing") {
	OcclusionTestScene scene;
	scene.draw();

	CHECK_FALSE(scene.is_occluded(box_at(Vector3(0, 0, -20))));
	CHECK_FALSE(scene.is_occluded(box_at(Vector3(0, 0, -1))));
	CHECK_FALSE(scene.is_occluded(box_at(Vector3(5, 3, -400))));
}

TEST_CASE("[HZBOcclusionCull] A wall hides what stands behind it") {
	OcclusionTestScene scene;

	PackedVector3Array vertices;
	PackedInt32Array indices;
	make_wall(-10.0f, 20.0f, vertices, indices);
	scene.add_occluder(vertices, indices);
	scene.draw();

	SUBCASE("Behind the wall") {
		CHECK(scene.is_occluded(box_at(Vector3(0, 0, -20))));
		CHECK(scene.is_occluded(box_at(Vector3(2, -1, -50))));
		// Just behind it, which is where the interpolated depth has to be
		// right rather than merely in the right ballpark.
		CHECK(scene.is_occluded(box_at(Vector3(0, 0, -10.75), 0.25f)));
	}

	SUBCASE("In front of the wall") {
		CHECK_FALSE(scene.is_occluded(box_at(Vector3(0, 0, -5))));
		CHECK_FALSE(scene.is_occluded(box_at(Vector3(0, 0, -9.25), 0.25f)));
	}

	SUBCASE("Straddling the wall") {
		// Its closest point is in front of the wall, so it stays visible.
		CHECK_FALSE(scene.is_occluded(AABB(Vector3(-1, -1, -12), Vector3(2, 2, 4))));
	}
}

TEST_CASE("[HZBOcclusionCull] An occluder only hides what it actually covers") {
	OcclusionTestScene scene;

	// Narrow wall: at 10 units away it spans a little over a quarter of the
	// screen's width, so it cannot hide anything off to the side.
	PackedVector3Array vertices;
	PackedInt32Array indices;
	make_wall(-10.0f, 2.0f, vertices, indices);
	scene.add_occluder(vertices, indices);
	scene.draw();

	CHECK(scene.is_occluded(box_at(Vector3(0, 0, -20), 0.25f)));
	CHECK_FALSE(scene.is_occluded(box_at(Vector3(8, 0, -20), 0.25f)));
	CHECK_FALSE(scene.is_occluded(box_at(Vector3(0, 8, -20), 0.25f)));
	// Big enough that the wall only covers its middle.
	CHECK_FALSE(scene.is_occluded(box_at(Vector3(0, 0, -20), 8.0f)));
}

TEST_CASE("[HZBOcclusionCull] An occluder hides what is on its own side of the screen") {
	OcclusionTestScene scene;

	// Moved up out of the center by its instance transform, so this also
	// catches a depth buffer written upside down or mirrored, which a scene
	// symmetric about the view axis would not.
	PackedVector3Array vertices;
	PackedInt32Array indices;
	make_wall(-10.0f, 2.0f, vertices, indices);
	scene.add_occluder(vertices, indices, Transform3D(Basis(), Vector3(3, 6, 0)));
	scene.draw();

	CHECK(scene.is_occluded(box_at(Vector3(6, 12, -20), 0.25f)));
	CHECK_FALSE(scene.is_occluded(box_at(Vector3(6, -12, -20), 0.25f)));
	CHECK_FALSE(scene.is_occluded(box_at(Vector3(-6, 12, -20), 0.25f)));
	CHECK_FALSE(scene.is_occluded(box_at(Vector3(0, 0, -20), 0.25f)));
}

TEST_CASE("[HZBOcclusionCull] A gap between occluders is not occluding") {
	OcclusionTestScene scene;

	PackedVector3Array vertices;
	PackedInt32Array indices;
	make_wall(-10.0f, 4.0f, vertices, indices, -6.0f);
	scene.add_occluder(vertices, indices);
	make_wall(-10.0f, 4.0f, vertices, indices, 6.0f);
	scene.add_occluder(vertices, indices);
	scene.draw();

	// Dead center, where neither wall reaches.
	CHECK_FALSE(scene.is_occluded(box_at(Vector3(0, 0, -20), 0.25f)));
	// Straight behind one of them.
	CHECK(scene.is_occluded(box_at(Vector3(-12, 0, -20), 0.25f)));
}

TEST_CASE("[HZBOcclusionCull] Occluders crossing the near plane are clipped, not dropped") {
	OcclusionTestScene scene;

	// A ground plane running from well behind the camera to well in front of
	// it, 2 units below it: the terrain case, and the one that makes every
	// triangle straddle the near plane.
	PackedVector3Array vertices;
	vertices.push_back(Vector3(-200, -2, 200));
	vertices.push_back(Vector3(200, -2, 200));
	vertices.push_back(Vector3(200, -2, -200));
	vertices.push_back(Vector3(-200, -2, -200));

	PackedInt32Array indices;
	indices.push_back(0);
	indices.push_back(1);
	indices.push_back(2);
	indices.push_back(0);
	indices.push_back(2);
	indices.push_back(3);

	scene.add_occluder(vertices, indices);

	// Tilted down, so the ground fills the lower half of the view.
	scene.camera_transform.basis = Basis(Vector3(1, 0, 0), Math::deg_to_rad(-30.0));
	scene.draw();

	// Under the ground, far ahead.
	CHECK(scene.is_occluded(box_at(Vector3(0, -10, -40), 1.0f)));
	// Above the ground, in plain view.
	CHECK_FALSE(scene.is_occluded(box_at(Vector3(0, 2, -40), 1.0f)));
}

TEST_CASE("[HZBOcclusionCull] Occluders outside the frustum are ignored") {
	OcclusionTestScene scene;

	PackedVector3Array vertices;
	PackedInt32Array indices;
	// Behind the camera.
	make_wall(10.0f, 20.0f, vertices, indices);
	scene.add_occluder(vertices, indices);
	scene.draw();

	CHECK_FALSE(scene.is_occluded(box_at(Vector3(0, 0, -20))));
}

TEST_CASE("[HZBOcclusionCull] Disabling or removing an occluder stops it occluding") {
	OcclusionTestScene scene;

	PackedVector3Array vertices;
	PackedInt32Array indices;
	make_wall(-10.0f, 20.0f, vertices, indices);

	const OcclusionTestScene::OccluderHandle wall = scene.add_occluder(vertices, indices);
	scene.draw();
	CHECK(scene.is_occluded(box_at(Vector3(0, 0, -20))));

	SUBCASE("Removed") {
		scene.remove_instance(wall);
		scene.draw();
		CHECK_FALSE(scene.is_occluded(box_at(Vector3(0, 0, -20))));
	}

	SUBCASE("Disabled") {
		scene.update_instance(wall, false);
		scene.draw();
		CHECK_FALSE(scene.is_occluded(box_at(Vector3(0, 0, -20))));
	}

	SUBCASE("Moved behind the camera") {
		scene.update_instance(wall, true, Transform3D(Basis(), Vector3(0, 0, 100)));
		scene.draw();
		CHECK_FALSE(scene.is_occluded(box_at(Vector3(0, 0, -20))));
	}

	SUBCASE("Moved and moved back") {
		scene.update_instance(wall, true, Transform3D(Basis(), Vector3(0, 0, 100)));
		scene.draw();
		scene.update_instance(wall, true);
		scene.draw();
		CHECK(scene.is_occluded(box_at(Vector3(0, 0, -20))));
	}
}

TEST_CASE("[HZBOcclusionCull] An orthogonal camera occludes as well") {
	OcclusionTestScene scene;
	scene.orthogonal = true;
	scene.camera_projection.set_orthogonal(20.0, 1.0, 0.05, 1000.0);

	PackedVector3Array vertices;
	PackedInt32Array indices;
	make_wall(-10.0f, 4.0f, vertices, indices);
	scene.add_occluder(vertices, indices);
	scene.draw();

	CHECK(scene.is_occluded(box_at(Vector3(0, 0, -20), 0.5f)));
	CHECK_FALSE(scene.is_occluded(box_at(Vector3(0, 0, -5), 0.5f)));
	// Outside the wall, which an orthogonal projection does not widen with
	// distance.
	CHECK_FALSE(scene.is_occluded(box_at(Vector3(7, 0, -20), 0.5f)));
}

TEST_CASE("[HZBOcclusionCull] The closest surface is the one that occludes") {
	OcclusionTestScene scene;

	// Two walls, added far one first, so the depth buffer has to keep the
	// nearest of the two rather than the last one drawn.
	PackedVector3Array vertices;
	PackedInt32Array indices;
	make_wall(-30.0f, 20.0f, vertices, indices);
	scene.add_occluder(vertices, indices);
	make_wall(-10.0f, 20.0f, vertices, indices);
	scene.add_occluder(vertices, indices);
	scene.draw();

	// Between the two walls: hidden by the near one.
	CHECK(scene.is_occluded(box_at(Vector3(0, 0, -20))));
	// In front of both.
	CHECK_FALSE(scene.is_occluded(box_at(Vector3(0, 0, -5))));
}

} // namespace TestOcclusionCullHZB
