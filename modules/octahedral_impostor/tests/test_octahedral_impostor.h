/**************************************************************************/
/*  test_octahedral_impostor.h                                            */
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

#include "../octahedral_impostor_material_3d.h"

#ifdef TOOLS_ENABLED
#include "../editor/octahedral_impostor_baker.h"

#include "scene/3d/mesh_instance_3d.h"
#include "scene/resources/3d/primitive_meshes.h"
#endif

#include "core/math/random_number_generator.h"
#include "tests/test_macros.h"

namespace TestOctahedralImpostor {

using Layout = OctahedralImpostorMaterial3D::Layout;

TEST_CASE("[OctahedralImpostor] Octahedral mapping round trip") {
	Ref<RandomNumberGenerator> rng;
	rng.instantiate();
	rng->set_seed(42);
	for (int i = 0; i < 1000; i++) {
		Vector3 dir = Vector3(rng->randf_range(-1, 1), rng->randf_range(-1, 1), rng->randf_range(-1, 1));
		if (dir.length() < 0.01) {
			continue;
		}
		dir.normalize();

		const Vector2 coord = OctahedralImpostorMaterial3D::octahedral_encode(dir, OctahedralImpostorMaterial3D::LAYOUT_FULL_SPHERE);
		CHECK(Math::abs(coord.x) <= 1.0 + CMP_EPSILON);
		CHECK(Math::abs(coord.y) <= 1.0 + CMP_EPSILON);
		CHECK(OctahedralImpostorMaterial3D::octahedral_decode(coord, OctahedralImpostorMaterial3D::LAYOUT_FULL_SPHERE).is_equal_approx(dir));

		// The hemisphere only maps the directions above the horizon.
		if (dir.y > 0.01) {
			const Vector2 hemi_coord = OctahedralImpostorMaterial3D::octahedral_encode(dir, OctahedralImpostorMaterial3D::LAYOUT_HEMISPHERE);
			CHECK(Math::abs(hemi_coord.x) <= 1.0 + CMP_EPSILON);
			CHECK(Math::abs(hemi_coord.y) <= 1.0 + CMP_EPSILON);
			CHECK(OctahedralImpostorMaterial3D::octahedral_decode(hemi_coord, OctahedralImpostorMaterial3D::LAYOUT_HEMISPHERE).is_equal_approx(dir));
		}
	}

	// Views from below are clamped to the horizon.
	const Vector3 below = OctahedralImpostorMaterial3D::octahedral_decode(OctahedralImpostorMaterial3D::octahedral_encode(Vector3(0.6, -0.8, 0.0), OctahedralImpostorMaterial3D::LAYOUT_HEMISPHERE), OctahedralImpostorMaterial3D::LAYOUT_HEMISPHERE);
	CHECK(below.y >= 0.0);
	CHECK(below.y < 0.01);
	CHECK(below.x > 0.99);
}

TEST_CASE("[OctahedralImpostor] Frame directions") {
	// Center of the grid: from above. Corners of the full sphere: from below.
	CHECK(OctahedralImpostorMaterial3D::get_frame_direction(Vector2i(4, 4), 9, OctahedralImpostorMaterial3D::LAYOUT_HEMISPHERE).is_equal_approx(Vector3(0, 1, 0)));
	CHECK(OctahedralImpostorMaterial3D::get_frame_direction(Vector2i(4, 4), 9, OctahedralImpostorMaterial3D::LAYOUT_FULL_SPHERE).is_equal_approx(Vector3(0, 1, 0)));
	CHECK(OctahedralImpostorMaterial3D::get_frame_direction(Vector2i(0, 0), 9, OctahedralImpostorMaterial3D::LAYOUT_FULL_SPHERE).is_equal_approx(Vector3(0, -1, 0)));
	CHECK(OctahedralImpostorMaterial3D::get_frame_direction(Vector2i(8, 8), 9, OctahedralImpostorMaterial3D::LAYOUT_FULL_SPHERE).is_equal_approx(Vector3(0, -1, 0)));
	// Corners of the hemisphere: on the horizon.
	CHECK(OctahedralImpostorMaterial3D::get_frame_direction(Vector2i(0, 0), 9, OctahedralImpostorMaterial3D::LAYOUT_HEMISPHERE).is_equal_approx(Vector3(0, 0, -1)));
	CHECK(OctahedralImpostorMaterial3D::get_frame_direction(Vector2i(8, 0), 9, OctahedralImpostorMaterial3D::LAYOUT_HEMISPHERE).is_equal_approx(Vector3(1, 0, 0)));

	// The shader finds the frames of a view from the encoded direction: each frame must be found
	// back from its own direction.
	for (int layout = 0; layout < OctahedralImpostorMaterial3D::LAYOUT_MAX; layout++) {
		for (int frames : { 8, 12, 16 }) {
			for (int y = 0; y < frames; y++) {
				for (int x = 0; x < frames; x++) {
					const Vector3 dir = OctahedralImpostorMaterial3D::get_frame_direction(Vector2i(x, y), frames, Layout(layout));
					CHECK(dir.is_normalized());
					const Vector2 grid = (OctahedralImpostorMaterial3D::octahedral_encode(dir, Layout(layout)) * 0.5 + Vector2(0.5, 0.5)) * (frames - 1);
					// Some directions of the edges of the full sphere are shared by two frames. The
					// frames of the horizon of the hemisphere are raised a bit by the encoding.
					const bool edge = layout == OctahedralImpostorMaterial3D::LAYOUT_FULL_SPHERE && (x == 0 || y == 0 || x == frames - 1 || y == frames - 1);
					if (!edge) {
						CHECK(grid.distance_to(Vector2(x, y)) < 0.02);
					}
				}
			}
		}
	}
}

TEST_CASE("[OctahedralImpostor] Frame basis") {
	const Vector3 directions[] = {
		Vector3(0, 1, 0),
		Vector3(0, -1, 0),
		Vector3(1, 0, 0),
		Vector3(0, 0, 1),
		Vector3(0.3, 0.9, -0.2).normalized(),
		Vector3(0.001, 0.9995, 0.0).normalized(),
	};
	for (const Vector3 &dir : directions) {
		const Basis basis = OctahedralImpostorMaterial3D::get_frame_basis(dir);
		// X right, Y up, Z toward the viewer: orthonormal and right-handed.
		CHECK(basis.get_column(2).is_equal_approx(dir));
		CHECK(basis.get_column(0).is_normalized());
		CHECK(basis.get_column(1).is_normalized());
		CHECK(Math::is_zero_approx(basis.get_column(0).dot(basis.get_column(1))));
		CHECK(Math::is_zero_approx(basis.get_column(0).dot(dir)));
		CHECK(basis.get_column(0).cross(basis.get_column(1)).is_equal_approx(dir));
	}
	// Side views are upright.
	CHECK(OctahedralImpostorMaterial3D::get_frame_basis(Vector3(0, 0, 1)).get_column(1).is_equal_approx(Vector3(0, 1, 0)));
}

TEST_CASE("[OctahedralImpostor] Material properties") {
	Ref<OctahedralImpostorMaterial3D> material;
	material.instantiate();
	CHECK(material->get_shader_mode() == Shader::MODE_SPATIAL);
	CHECK(material->get_shader_rid().is_valid());

	material->set_frames(1000);
	CHECK(material->get_frames() == OctahedralImpostorMaterial3D::MAX_FRAMES);
	material->set_frames(0);
	CHECK(material->get_frames() == OctahedralImpostorMaterial3D::MIN_FRAMES);

	// Variants of the shader are shared by the materials with the same features.
	Ref<OctahedralImpostorMaterial3D> other;
	other.instantiate();
	CHECK(other->get_shader_rid() == material->get_shader_rid());
	other->set_layout(OctahedralImpostorMaterial3D::LAYOUT_FULL_SPHERE);
	CHECK(other->get_shader_rid() != material->get_shader_rid());
	other->set_layout(OctahedralImpostorMaterial3D::LAYOUT_HEMISPHERE);
	CHECK(other->get_shader_rid() == material->get_shader_rid());

	// The quad turns toward the camera around the bounding sphere.
	material->set_sphere_center(Vector3(1, 2, 3));
	material->set_sphere_radius(2.0);
	const AABB bounds = material->get_bounds();
	CHECK(bounds.get_center().is_equal_approx(Vector3(1, 2, 3)));
	CHECK(bounds.has_point(Vector3(1, 2, 3) + Vector3(2, 2, 2) * 0.99));
}

#ifdef TOOLS_ENABLED
static MeshInstance3D *_make_box(const Vector3 &p_position, const Vector3 &p_size) {
	Ref<BoxMesh> box;
	box.instantiate();
	box->set_size(p_size);
	MeshInstance3D *instance = memnew(MeshInstance3D);
	instance->set_mesh(box);
	instance->set_position(p_position);
	return instance;
}

TEST_CASE("[OctahedralImpostor][Editor] Collected geometry and bounding sphere") {
	Node3D *root = memnew(Node3D);
	// The transform of the root isn't part of the impostor.
	root->set_position(Vector3(100, 0, 0));
	CHECK_FALSE(OctahedralImpostorBaker::has_geometry(root));

	MeshInstance3D *box = _make_box(Vector3(0, 1, 0), Vector3(2, 2, 2));
	root->add_child(box);
	MeshInstance3D *hidden = _make_box(Vector3(50, 0, 0), Vector3(1, 1, 1));
	hidden->set_visible(false);
	root->add_child(hidden);
	MeshInstance3D *shadow_proxy = _make_box(Vector3(50, 0, 0), Vector3(1, 1, 1));
	shadow_proxy->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_SHADOWS_ONLY);
	root->add_child(shadow_proxy);
	MeshInstance3D *far_lod = _make_box(Vector3(50, 0, 0), Vector3(1, 1, 1));
	far_lod->set_visibility_range_begin(30.0);
	root->add_child(far_lod);
	MeshInstance3D *impostor = _make_box(Vector3(50, 0, 0), Vector3(1, 1, 1));
	impostor->set_material_override(memnew(OctahedralImpostorMaterial3D));
	root->add_child(impostor);

	CHECK(OctahedralImpostorBaker::has_geometry(root));
	const LocalVector<OctahedralImpostorBaker::Geometry> geometry = OctahedralImpostorBaker::collect_geometry(root);
	REQUIRE(geometry.size() == 1);
	CHECK(geometry[0].node == box->get_instance_id());
	CHECK(geometry[0].transform.origin.is_equal_approx(Vector3(0, 1, 0)));

	Vector3 center;
	float radius = 0.0;
	CHECK(OctahedralImpostorBaker::compute_bounding_sphere(geometry, center, radius));
	CHECK(center.is_equal_approx(Vector3(0, 1, 0)));
	CHECK(radius == doctest::Approx(Math::sqrt(3.0)));

	// The sphere goes through the farthest vertex, tighter than the bounds for round shapes.
	Ref<SphereMesh> sphere_mesh;
	sphere_mesh.instantiate();
	sphere_mesh->set_radius(1.0);
	sphere_mesh->set_height(2.0);
	box->set_mesh(sphere_mesh);
	CHECK(OctahedralImpostorBaker::compute_bounding_sphere(OctahedralImpostorBaker::collect_geometry(root), center, radius));
	CHECK(radius == doctest::Approx(1.0).epsilon(0.01));

	memdelete(root);
}

TEST_CASE("[OctahedralImpostor][Editor] Frame size") {
	Ref<OctahedralImpostorBaker> baker;
	baker.instantiate();
	baker->set_atlas_size(2048);
	baker->set_frames(16);
	CHECK(baker->get_frame_size() == 128);
	CHECK(baker->get_baked_atlas_size() == 2048);
	// Rounded down to a multiple of the frames and of the blocks of compressed textures.
	baker->set_frames(12);
	CHECK(baker->get_frame_size() == 168);
	CHECK(baker->get_baked_atlas_size() == 2016);
	CHECK(baker->get_frame_size() % 4 == 0);
}

TEST_CASE("[OctahedralImpostor][Editor] Billboard settings") {
	Ref<OctahedralImpostorBaker> baker;
	baker.instantiate();
	CHECK(baker->get_type() == OctahedralImpostorBaker::TYPE_OCTAHEDRAL_IMPOSTOR);
	baker->set_type(OctahedralImpostorBaker::TYPE_BILLBOARD);
	CHECK(baker->get_type() == OctahedralImpostorBaker::TYPE_BILLBOARD);
	baker->set_billboard_mode(OctahedralImpostorBaker::BILLBOARD_CROSS);
	CHECK(baker->get_billboard_mode() == OctahedralImpostorBaker::BILLBOARD_CROSS);
	baker->set_cross_planes(100);
	CHECK(baker->get_cross_planes() == OctahedralImpostorBaker::MAX_CROSS_PLANES);
	baker->set_cross_planes(0);
	CHECK(baker->get_cross_planes() == OctahedralImpostorBaker::MIN_CROSS_PLANES);
}

TEST_CASE("[OctahedralImpostor][Editor] Billboard views") {
	Node3D *root = memnew(Node3D);
	// A tall box, off the origin of the node.
	root->add_child(_make_box(Vector3(1, 2, 0.5), Vector3(1, 4, 2)));
	const LocalVector<OctahedralImpostorBaker::Geometry> geometry = OctahedralImpostorBaker::collect_geometry(root);

	SUBCASE("Turned toward the camera") {
		LocalVector<OctahedralImpostorBaker::View> views;
		Size2i atlas_size;
		real_t texel = 0.0;
		REQUIRE(OctahedralImpostorBaker::compute_billboard_views(geometry, OctahedralImpostorBaker::BILLBOARD_FIXED_Y, 2, 512, views, atlas_size, texel));
		REQUIRE(views.size() == 1);
		const OctahedralImpostorBaker::View &view = views[0];
		// The view from the front (+Z), centered on the geometry, on the plane through the origin.
		CHECK(view.basis.get_column(2).is_equal_approx(Vector3(0, 0, 1)));
		CHECK(view.basis.get_column(1).is_equal_approx(Vector3(0, 1, 0)));
		CHECK(view.center.is_equal_approx(Vector3(1, 2, 0)));
		// The textures fit the shape: the longest side is the size, in blocks of 4 pixels.
		CHECK(atlas_size.y == 512);
		CHECK(atlas_size.x < 256);
		CHECK(atlas_size.x % 4 == 0);
		CHECK(view.rect == Rect2i(Point2i(), atlas_size));
		// The view contains the geometry with a border.
		const Size2 world_size = Size2(view.rect.size) * texel;
		CHECK(world_size.x > 1.0);
		CHECK(world_size.y > 4.0);
		CHECK(world_size.x - 1.0 < 20.0 * texel);
		CHECK(world_size.y - 4.0 < 20.0 * texel);
	}

	SUBCASE("Crossed planes") {
		LocalVector<OctahedralImpostorBaker::View> views;
		Size2i atlas_size;
		real_t texel = 0.0;
		REQUIRE(OctahedralImpostorBaker::compute_billboard_views(geometry, OctahedralImpostorBaker::BILLBOARD_CROSS, 3, 1024, views, atlas_size, texel));
		REQUIRE(views.size() == 3);
		CHECK(MAX(atlas_size.x, atlas_size.y) == 1024);
		int width = 0;
		for (uint32_t i = 0; i < views.size(); i++) {
			const OctahedralImpostorBaker::View &view = views[i];
			// Side by side, vertical planes at equal angles.
			CHECK(view.rect.position == Point2i(width, 0));
			CHECK(view.rect.size.y == atlas_size.y);
			width += view.rect.size.x;
			const Vector3 direction = view.basis.get_column(2);
			CHECK(Math::is_zero_approx(direction.y));
			CHECK(direction.angle_to(Vector3(0, 0, 1)) == doctest::Approx(Math::PI * i / 3.0));
			// Through the vertical axis of the geometry.
			CHECK(view.center.y == doctest::Approx(2.0));
			CHECK(Math::is_zero_approx((view.center - Vector3(1, 2, 0.5)).dot(direction)));
		}
		CHECK(width == atlas_size.x);
		// The front view is as wide as the box, the side view as deep.
		CHECK(views[0].rect.size.x < views[1].rect.size.x);
	}

	memdelete(root);
}

TEST_CASE("[OctahedralImpostor][Editor] Baked billboards are skipped") {
	Node3D *root = memnew(Node3D);
	MeshInstance3D *box = _make_box(Vector3(), Vector3(1, 1, 1));
	root->add_child(box);
	MeshInstance3D *billboard = memnew(MeshInstance3D);
	Ref<QuadMesh> quad;
	quad.instantiate();
	quad->set_meta("_baked_billboard", true);
	billboard->set_mesh(quad);
	root->add_child(billboard);

	const LocalVector<OctahedralImpostorBaker::Geometry> geometry = OctahedralImpostorBaker::collect_geometry(root);
	REQUIRE(geometry.size() == 1);
	CHECK(geometry[0].node == box->get_instance_id());

	memdelete(root);
}
#endif

} // namespace TestOctahedralImpostor
