/**************************************************************************/
/*  test_planar_reflection_probe.cpp                                      */
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

TEST_FORCE_LINK(test_planar_reflection_probe)

#ifndef _3D_DISABLED

#include "scene/3d/planar_reflection_probe.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "servers/rendering/rendering_server.h"

namespace TestPlanarReflectionProbe {

TEST_CASE("[SceneTree][PlanarReflectionProbe] Getters, setters and bounds") {
	PlanarReflectionProbe *probe = memnew(PlanarReflectionProbe);

	SUBCASE("The box spans the mirror and the receive distance on both sides") {
		CHECK(probe->get_aabb().is_equal_approx(AABB(Vector3(-10, -1, -10), Vector3(20, 2, 20))));
		probe->set_size(Vector2(8, 4));
		probe->set_receive_distance(0.25);
		CHECK(probe->get_aabb().is_equal_approx(AABB(Vector3(-4, -0.25, -2), Vector3(8, 0.5, 4))));
	}

	SUBCASE("Values are kept within range") {
		probe->set_size(Vector2(-3, 5));
		CHECK(probe->get_size() == Vector2(0, 5));
		probe->set_receive_distance(-1.0);
		CHECK(probe->get_receive_distance() > 0.0);
		probe->set_resolution_scale(4.0);
		CHECK(probe->get_resolution_scale() == doctest::Approx(1.0));
		probe->set_resolution_scale(0.0);
		CHECK(probe->get_resolution_scale() == doctest::Approx(0.05));
		probe->set_receive_angle(120.0);
		CHECK(probe->get_receive_angle() == doctest::Approx(90.0));
		probe->set_edge_fade(-0.5);
		CHECK(probe->get_edge_fade() == doctest::Approx(0.0));
		probe->set_intensity(-1.0);
		CHECK(probe->get_intensity() == doctest::Approx(0.0));
		probe->set_max_distance(-5.0);
		CHECK(probe->get_max_distance() == doctest::Approx(0.0));
	}

	SUBCASE("Other properties") {
		probe->set_distortion(0.5);
		CHECK(probe->get_distortion() == doctest::Approx(0.5));
		probe->set_clip_bias(-0.1);
		CHECK(probe->get_clip_bias() == doctest::Approx(-0.1));
		probe->set_enable_shadows(false);
		CHECK_FALSE(probe->are_shadows_enabled());
		probe->set_mesh_lod_threshold(2.0);
		CHECK(probe->get_mesh_lod_threshold() == doctest::Approx(2.0));
		probe->set_cull_mask(0b101);
		CHECK(probe->get_cull_mask() == 0b101u);
		probe->set_reflection_mask(0b10);
		CHECK(probe->get_reflection_mask() == 0b10u);
	}

	memdelete(probe);
}

TEST_CASE("[SceneTree][PlanarReflectionProbe] The server places it in its scenario") {
	PlanarReflectionProbe *probe = memnew(PlanarReflectionProbe);
	probe->set_size(Vector2(6, 6));
	probe->set_receive_distance(0.5);
	probe->set_position(Vector3(100, 5, 0));
	SceneTree::get_singleton()->get_root()->add_child(probe);
	// Node3D sends its transform to the server once the frame's transform changes are flushed.
	SceneTree::get_singleton()->flush_transform_notifications();

	const RID scenario = probe->get_world_3d()->get_scenario();
	const ObjectID id = probe->get_instance_id();

	// Its bounds are the mirror and the volume of surfaces that reflect it.
	CHECK(RS::get_singleton()->instances_cull_aabb(AABB(Vector3(102, 5.3, 2), Vector3(0.5, 0.1, 0.5)), scenario).has(id));
	CHECK_FALSE(RS::get_singleton()->instances_cull_aabb(AABB(Vector3(102, 6, 2), Vector3(0.5, 0.5, 0.5)), scenario).has(id));
	CHECK_FALSE(RS::get_singleton()->instances_cull_aabb(AABB(Vector3(104, 5, 0), Vector3(0.5, 0.5, 0.5)), scenario).has(id));

	// They follow its size.
	probe->set_size(Vector2(10, 10));
	CHECK(RS::get_singleton()->instances_cull_aabb(AABB(Vector3(104, 5, 0), Vector3(0.5, 0.5, 0.5)), scenario).has(id));

	memdelete(probe);
	CHECK_FALSE(RS::get_singleton()->instances_cull_aabb(AABB(Vector3(104, 5, 0), Vector3(0.5, 0.5, 0.5)), scenario).has(id));
}

TEST_CASE("[SceneTree][PlanarReflectionProbe] Freeing a planar reflection before or after its instance") {
	RenderingServer *rs = RS::get_singleton();
	const RID scenario = SceneTree::get_singleton()->get_root()->get_world_3d()->get_scenario();
	const RID other_scenario = rs->scenario_create();

	SUBCASE("The planar reflection first") {
		const RID planar_reflection = rs->planar_reflection_create();
		const RID instance = rs->instance_create2(planar_reflection, scenario);
		rs->instance_set_scenario(instance, other_scenario);
		rs->free_rid(planar_reflection);
		rs->instance_set_transform(instance, Transform3D());
		rs->instance_set_scenario(instance, scenario);
		rs->free_rid(instance);
	}

	SUBCASE("The instance first") {
		const RID planar_reflection = rs->planar_reflection_create();
		const RID instance = rs->instance_create2(planar_reflection, scenario);
		rs->planar_reflection_set_size(planar_reflection, Vector2(3, 3));
		rs->free_rid(instance);
		rs->planar_reflection_set_size(planar_reflection, Vector2(4, 4));
		rs->free_rid(planar_reflection);
	}

	SUBCASE("The scenario first") {
		const RID planar_reflection = rs->planar_reflection_create();
		const RID instance = rs->instance_create2(planar_reflection, other_scenario);
		rs->free_rid(other_scenario);
		rs->free_rid(instance);
		rs->free_rid(planar_reflection);
		return;
	}

	rs->free_rid(other_scenario);
}

} // namespace TestPlanarReflectionProbe

#endif // _3D_DISABLED
