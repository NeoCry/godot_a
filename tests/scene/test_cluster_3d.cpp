/**************************************************************************/
/*  test_cluster_3d.cpp                                                   */
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

TEST_FORCE_LINK(test_cluster_3d)

#include "core/math/triangle_mesh.h"
#include "scene/3d/cluster_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/material.h"
#include "scene/resources/packed_scene.h"
#include "servers/rendering/rendering_server.h"

#ifndef PHYSICS_3D_DISABLED
#include "servers/physics_3d/physics_server_3d.h"
#endif // PHYSICS_3D_DISABLED

namespace TestCluster3D {

static Ref<BoxMesh> make_box(const Vector3 &p_size = Vector3(1, 1, 1)) {
	Ref<BoxMesh> box;
	box.instantiate();
	box->set_size(p_size);
	return box;
}

static Transform3D at(const Vector3 &p_position) {
	return Transform3D(Basis(), p_position);
}

TEST_CASE("[Cluster3D] Adding, changing and removing parts") {
	Cluster3D *cluster = memnew(Cluster3D);
	const Ref<BoxMesh> box = make_box();
	const Ref<BoxMesh> big_box = make_box(Vector3(2, 2, 2));

	CHECK(cluster->get_part_count() == 0);
	CHECK(cluster->add_part(box, at(Vector3(-2, 0, 0))) == 0);
	CHECK(cluster->add_part(box, at(Vector3(2, 0, 0))) == 1);
	CHECK(cluster->add_part(big_box, at(Vector3(0, 3, 0))) == 2);
	CHECK(cluster->get_part_count() == 3);

	CHECK(cluster->get_part_mesh(2) == big_box);
	CHECK(cluster->get_part_transform(1).origin.is_equal_approx(Vector3(2, 0, 0)));
	for (int i = 0; i < cluster->get_part_count(); i++) {
		CHECK(cluster->get_part_instance(i).is_valid());
	}

	// The bounds cover every part where it is.
	const AABB aabb = cluster->get_aabb();
	CHECK(aabb.position.is_equal_approx(Vector3(-2.5, -0.5, -1)));
	CHECK(aabb.get_end().is_equal_approx(Vector3(2.5, 4, 1)));

	cluster->set_part_transform(0, at(Vector3(-5, 0, 0)));
	CHECK(cluster->get_aabb().position.x == doctest::Approx(-5.5));

	cluster->remove_part(0);
	CHECK(cluster->get_part_count() == 2);
	CHECK(cluster->get_part_transform(0).origin.is_equal_approx(Vector3(2, 0, 0)));
	CHECK(cluster->get_aabb().position.x == doctest::Approx(-1));

	cluster->clear_parts();
	CHECK(cluster->get_part_count() == 0);
	CHECK(cluster->get_aabb() == AABB());

	memdelete(cluster);
}

TEST_CASE("[Cluster3D] Parts are properties, saved and loaded with the scene") {
	Node3D *root = memnew(Node3D);
	Cluster3D *cluster = memnew(Cluster3D);
	cluster->set_name("Rocks");
	root->add_child(cluster);
	cluster->set_owner(root);

	const Ref<BoxMesh> box = make_box();
	Ref<StandardMaterial3D> moss;
	moss.instantiate();
	cluster->add_part(box, at(Vector3(1, 0, 0)));
	cluster->add_part(box, Transform3D(Basis(Vector3(0, 1, 0), 0.5), Vector3(0, 0, 2)));
	cluster->set_part_surface_material(1, 0, moss);

	// What the Inspector edits, as an array of parts.
	CHECK(int(cluster->get("part_count")) == 2);
	CHECK(Ref<Mesh>(cluster->get("part_0/mesh")) == box);
	cluster->set("part_0/transform", at(Vector3(4, 0, 0)));
	CHECK(cluster->get_part_transform(0).origin.is_equal_approx(Vector3(4, 0, 0)));

	Ref<PackedScene> packed;
	packed.instantiate();
	REQUIRE(packed->pack(root) == OK);
	Node *loaded_root = packed->instantiate();
	REQUIRE(loaded_root);
	Cluster3D *loaded = Object::cast_to<Cluster3D>(loaded_root->get_node(NodePath("Rocks")));
	REQUIRE(loaded);

	CHECK(loaded->get_part_count() == 2);
	CHECK(loaded->get_part_mesh(0) == box);
	CHECK(loaded->get_part_transform(0).origin.is_equal_approx(Vector3(4, 0, 0)));
	CHECK(loaded->get_part_transform(1).is_equal_approx(cluster->get_part_transform(1)));
	CHECK(loaded->get_part_surface_material(1, 0) == moss);
	CHECK(loaded->get_part_surface_material(0, 0).is_null());
	CHECK(loaded->get_part_active_material(1, 0) == moss);

	memdelete(loaded_root);
	memdelete(root);
}

TEST_CASE("[Cluster3D] A duplicated cluster has parts of its own") {
	Cluster3D *cluster = memnew(Cluster3D);
	const Ref<BoxMesh> box = make_box();
	cluster->add_part(box, at(Vector3(1, 0, 0)));
	cluster->add_part(box, at(Vector3(2, 0, 0)));

	Cluster3D *copy = Object::cast_to<Cluster3D>(cluster->duplicate());
	REQUIRE(copy);
	CHECK(copy->get_part_count() == 2);
	CHECK(copy->get_part_mesh(1) == box);
	CHECK(copy->get_part_transform(1).origin.is_equal_approx(Vector3(2, 0, 0)));
	CHECK(copy->get_part_instance(0) != cluster->get_part_instance(0));

	copy->set_part_transform(0, at(Vector3(9, 0, 0)));
	CHECK(cluster->get_part_transform(0).origin.is_equal_approx(Vector3(1, 0, 0)));

	memdelete(copy);
	memdelete(cluster);
}

TEST_CASE("[Cluster3D] Moving the origin leaves the parts where they are") {
	Cluster3D *cluster = memnew(Cluster3D);
	cluster->set_transform(Transform3D(Basis(Vector3(0, 1, 0), Math::PI * 0.5), Vector3(10, 0, 0)));
	const Ref<BoxMesh> box = make_box();
	cluster->add_part(box, at(Vector3(3, 1, 0)));
	cluster->add_part(box, at(Vector3(5, 1, 0)));

	const Transform3D part_0 = cluster->get_transform() * cluster->get_part_transform(0);
	const Transform3D part_1 = cluster->get_transform() * cluster->get_part_transform(1);

	// To the bottom center of the parts, as the editor's Cluster menu does.
	AABB aabb = cluster->get_aabb();
	Vector3 bottom = aabb.get_center();
	bottom.y = aabb.position.y;
	cluster->move_origin(bottom);

	CHECK((cluster->get_transform() * cluster->get_part_transform(0)).is_equal_approx(part_0));
	CHECK((cluster->get_transform() * cluster->get_part_transform(1)).is_equal_approx(part_1));
	aabb = cluster->get_aabb();
	CHECK(aabb.get_center().x == doctest::Approx(0));
	CHECK(aabb.position.y == doctest::Approx(0));

	memdelete(cluster);
}

TEST_CASE("[Cluster3D] Taking parts from MeshInstance3D nodes and other clusters") {
	Node3D *branch = memnew(Node3D);
	branch->set_position(Vector3(100, 0, 0)); // Its own transform is not taken.

	const Ref<BoxMesh> box = make_box();
	Ref<StandardMaterial3D> red;
	red.instantiate();

	MeshInstance3D *a = memnew(MeshInstance3D);
	a->set_mesh(box);
	a->set_position(Vector3(1, 0, 0));
	branch->add_child(a);

	Node3D *group = memnew(Node3D);
	group->set_position(Vector3(0, 2, 0));
	branch->add_child(group);
	MeshInstance3D *b = memnew(MeshInstance3D);
	b->set_mesh(box);
	b->set_position(Vector3(0, 0, 3));
	b->set_surface_override_material(0, red);
	group->add_child(b);

	Cluster3D *inner = memnew(Cluster3D);
	inner->set_position(Vector3(-4, 0, 0));
	inner->add_part(box, at(Vector3(0, 0, 1)));
	branch->add_child(inner);

	Cluster3D *cluster = memnew(Cluster3D);
	CHECK(cluster->add_parts_from_node(branch, at(Vector3(0, 10, 0))) == 3);
	REQUIRE(cluster->get_part_count() == 3);
	CHECK(cluster->get_part_transform(0).origin.is_equal_approx(Vector3(1, 10, 0)));
	CHECK(cluster->get_part_transform(1).origin.is_equal_approx(Vector3(0, 12, 3)));
	CHECK(cluster->get_part_surface_material(1, 0) == red);
	CHECK(cluster->get_part_transform(2).origin.is_equal_approx(Vector3(-4, 10, 1)));

	memdelete(cluster);
	memdelete(branch);
}

TEST_CASE("[Cluster3D] Snapshots, as the editor undoes and redoes with") {
	Cluster3D *cluster = memnew(Cluster3D);
	const Ref<BoxMesh> box = make_box();
	const Ref<SphereMesh> sphere = memnew(SphereMesh);
	cluster->add_part(box, at(Vector3(1, 0, 0)));
	cluster->add_part(sphere, at(Vector3(2, 0, 0)));
	const RID first_instance = cluster->get_part_instance(0);

	const Array two = cluster->call("_get_parts_snapshot");
	cluster->add_part(box, at(Vector3(3, 0, 0)));
	const Array three = cluster->call("_get_parts_snapshot");

	cluster->call("_set_parts_snapshot", two);
	CHECK(cluster->get_part_count() == 2);
	CHECK(cluster->get_part_mesh(1) == sphere);
	// Parts that stay keep what draws them.
	CHECK(cluster->get_part_instance(0) == first_instance);

	cluster->call("_set_parts_snapshot", three);
	CHECK(cluster->get_part_count() == 3);
	CHECK(cluster->get_part_transform(2).origin.is_equal_approx(Vector3(3, 0, 0)));

	cluster->call("_set_parts_snapshot", Array());
	CHECK(cluster->get_part_count() == 0);

	memdelete(cluster);
}

TEST_CASE("[Cluster3D] Baking the parts into one mesh") {
	Cluster3D *cluster = memnew(Cluster3D);
	const Ref<BoxMesh> box = make_box();
	Ref<StandardMaterial3D> stone;
	stone.instantiate();
	box->set_material(stone);
	const Ref<BoxMesh> other_box = make_box();
	Ref<StandardMaterial3D> moss;
	moss.instantiate();

	cluster->add_part(box, at(Vector3(-2, 0, 0)));
	// Mirrored: its faces must still face out once baked.
	cluster->add_part(box, Transform3D(Basis().scaled(Vector3(-1, 1, 1)), Vector3(2, 0, 0)));
	cluster->add_part(other_box, at(Vector3(0, 0, 4)));
	cluster->set_part_surface_material(2, 0, moss);

	const Ref<ArrayMesh> baked = cluster->bake_mesh(false);
	REQUIRE(baked.is_valid());
	// One surface per material.
	REQUIRE(baked->get_surface_count() == 2);
	CHECK(baked->surface_get_material(0) == stone);
	CHECK(baked->surface_get_material(1) == moss);
	CHECK(baked->get_aabb().position.is_equal_approx(Vector3(-2.5, -0.5, -0.5)));
	CHECK(baked->get_aabb().get_end().is_equal_approx(Vector3(2.5, 0.5, 4.5)));

	const Array box_arrays = box->surface_get_arrays(0);
	const Array stone_arrays = baked->surface_get_arrays(0);
	CHECK(PackedVector3Array(stone_arrays[Mesh::ARRAY_VERTEX]).size() == PackedVector3Array(box_arrays[Mesh::ARRAY_VERTEX]).size() * 2);

	// Every face winds the way the box's own do relative to its normal, the
	// mirrored copy's too.
	const auto count_windings = [](const Array &p_arrays, int &r_with, int &r_against) {
		const PackedVector3Array vertices = p_arrays[Mesh::ARRAY_VERTEX];
		const PackedVector3Array normals = p_arrays[Mesh::ARRAY_NORMAL];
		const PackedInt32Array indices = p_arrays[Mesh::ARRAY_INDEX];
		r_with = 0;
		r_against = 0;
		for (int i = 0; i + 2 < indices.size(); i += 3) {
			const Vector3 a = vertices[indices[i]];
			const Vector3 b = vertices[indices[i + 1]];
			const Vector3 c = vertices[indices[i + 2]];
			if ((b - a).cross(c - a).dot(normals[indices[i]]) > 0) {
				r_with++;
			} else {
				r_against++;
			}
		}
	};
	int box_with, box_against;
	count_windings(box_arrays, box_with, box_against);
	REQUIRE((box_with == 0 || box_against == 0));
	int baked_with, baked_against;
	count_windings(stone_arrays, baked_with, baked_against);
	if (box_with > 0) {
		CHECK(baked_against == 0);
	} else {
		CHECK(baked_with == 0);
	}

	memdelete(cluster);
}

TEST_CASE("[Cluster3D] What lightmap and GI bakers are given") {
	Cluster3D *cluster = memnew(Cluster3D);
	const Ref<BoxMesh> lightmappable = make_box();
	lightmappable->set_add_uv2(true);
	const Ref<BoxMesh> plain = make_box();

	cluster->add_part(plain, at(Vector3(0, 0, 0)));
	cluster->add_part(lightmappable, at(Vector3(1, 0, 0)));

	// Only what LightmapGI can bake: with a UV2.
	const Array bake_meshes = cluster->get_bake_meshes();
	REQUIRE(bake_meshes.size() == 2);
	CHECK(Ref<Mesh>(bake_meshes[0]) == lightmappable);
	CHECK(Transform3D(bake_meshes[1]).origin.is_equal_approx(Vector3(1, 0, 0)));
	CHECK(cluster->get_bake_mesh_instance(0) == cluster->get_part_instance(1));

	cluster->set_gi_mode(GeometryInstance3D::GI_MODE_DYNAMIC);
	CHECK(cluster->get_bake_meshes().is_empty());

	// VoxelGI takes every part, transform first.
	const Array meshes = cluster->get_meshes();
	REQUIRE(meshes.size() == 4);
	CHECK(Ref<Mesh>(meshes[1]) == plain);
	CHECK(Ref<Mesh>(meshes[3]) == lightmappable);

	memdelete(cluster);
}

TEST_CASE("[Cluster3D] Picking the parts at runtime") {
	Cluster3D *cluster = memnew(Cluster3D);
	cluster->add_part(make_box(), at(Vector3(-3, 0, 0)));
	cluster->add_part(make_box(), at(Vector3(3, 0, 0)));

	const Ref<TriangleMesh> triangles = cluster->generate_triangle_mesh();
	REQUIRE(triangles.is_valid());
	Vector3 hit, normal;
	CHECK(triangles->intersect_ray(Vector3(3, 0, 10), Vector3(0, 0, -1), hit, normal));
	CHECK(hit.is_equal_approx(Vector3(3, 0, 0.5)));
	CHECK_FALSE(triangles->intersect_ray(Vector3(0, 0, 10), Vector3(0, 0, -1), hit, normal));

	memdelete(cluster);
}

#ifndef PHYSICS_3D_DISABLED
TEST_CASE("[SceneTree][Cluster3D] Collision, one shape per part") {
	Cluster3D *cluster = memnew(Cluster3D);
	const Ref<BoxMesh> box = make_box();
	cluster->add_part(box, at(Vector3(-2, 0, 0)));
	cluster->add_part(box, at(Vector3(2, 0, 0)));
	cluster->set_use_collision(true);
	CHECK_FALSE(cluster->get_collision_rid().is_valid());

	SceneTree::get_singleton()->get_root()->add_child(cluster);
	const RID body = cluster->get_collision_rid();
	REQUIRE(body.is_valid());
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	CHECK(ps->body_get_shape_count(body) == 2);
	CHECK(ps->body_get_shape_transform(body, 1).origin.is_equal_approx(Vector3(2, 0, 0)));
	// Both parts share the shape made from their mesh.
	CHECK(ps->body_get_shape(body, 0) == ps->body_get_shape(body, 1));

	cluster->set_part_transform(1, at(Vector3(5, 0, 0)));
	CHECK(ps->body_get_shape_transform(body, 1).origin.is_equal_approx(Vector3(5, 0, 0)));

	cluster->add_part(box, at(Vector3(0, 3, 0)));
	CHECK(ps->body_get_shape_count(body) == 3);

	cluster->set_use_collision(false);
	CHECK_FALSE(cluster->get_collision_rid().is_valid());

	memdelete(cluster);
}
#endif // PHYSICS_3D_DISABLED

TEST_CASE("[SceneTree][Cluster3D] Parts follow the cluster's settings and stay in the world with it") {
	Cluster3D *cluster = memnew(Cluster3D);
	cluster->add_part(make_box(), at(Vector3(0, 0, 0)));
	SceneTree::get_singleton()->get_root()->add_child(cluster);

	// Changing what VisualInstance3D and GeometryInstance3D send to the
	// RenderingServer goes to every part; nothing here should error out.
	cluster->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	cluster->set_layer_mask(2);
	cluster->set_visibility_range_end(50);
	cluster->set_custom_aabb(AABB(Vector3(-1, -1, -1), Vector3(2, 2, 2)));
	cluster->set_instance_shader_parameter("tint", Color(1, 0, 0));
	CHECK(Color(RS::get_singleton()->instance_geometry_get_shader_parameter(cluster->get_part_instance(0), "tint")) == Color(1, 0, 0));
	cluster->set_instance_shader_parameter("tint", Variant());
	CHECK(RS::get_singleton()->instance_geometry_get_shader_parameter(cluster->get_part_instance(0), "tint").get_type() != Variant::COLOR);
	cluster->hide();
	cluster->show();
	cluster->add_part(make_box(), at(Vector3(1, 0, 0)));
	CHECK(cluster->get_part_count() == 2);

	SceneTree::get_singleton()->get_root()->remove_child(cluster);
	cluster->add_part(make_box(), at(Vector3(2, 0, 0)));
	SceneTree::get_singleton()->get_root()->add_child(cluster);
	CHECK(cluster->get_part_count() == 3);

	memdelete(cluster);
}

} // namespace TestCluster3D
