/**************************************************************************/
/*  ambient_probe_volume_3d.cpp                                           */
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

#include "ambient_probe_volume_3d.h"

#include "core/math/face3.h"
#include "core/templates/local_vector.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/resources/mesh.h"

// Hard cap per axis to keep a single accidental huge grid from hanging the editor.
static const int AMBIENT_PROBE_VOLUME_MAX_PER_AXIS = 64;
static const int AMBIENT_PROBE_VOLUME_WARN_PROBE_COUNT = 4096;

namespace {
struct OccluderFace {
	Vector3 v0, v1, v2;
	Vector3 center;
	float radius = 0.0f;
};

void gather_occluder_faces(Node *p_node, const Transform3D &p_world_to_local, LocalVector<OccluderFace> &r_faces) {
	MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(p_node);
	if (mi != nullptr && mi->is_visible_in_tree()) {
		Ref<Mesh> mesh = mi->get_mesh();
		if (mesh.is_valid()) {
			const Transform3D xf = p_world_to_local * mi->get_global_transform();
			Vector<Face3> faces = mesh->get_faces();
			for (const Face3 &face : faces) {
				OccluderFace of;
				of.v0 = xf.xform(face.vertex[0]);
				of.v1 = xf.xform(face.vertex[1]);
				of.v2 = xf.xform(face.vertex[2]);
				of.center = (of.v0 + of.v1 + of.v2) / 3.0f;
				of.radius = MAX(of.center.distance_to(of.v0), MAX(of.center.distance_to(of.v1), of.center.distance_to(of.v2)));
				r_faces.push_back(of);
			}
		}
	}

	for (int i = 0; i < p_node->get_child_count(); i++) {
		gather_occluder_faces(p_node->get_child(i), p_world_to_local, r_faces);
	}
}

// Fibonacci sphere: a deterministic, roughly uniform set of `p_count` directions
// covering the full sphere, used to sample ambient occlusion around each probe.
Vector3 fibonacci_sphere_direction(int p_index, int p_count) {
	static const float golden_angle = Math::PI * (3.0f - Math::sqrt(5.0f));
	const float y = p_count > 1 ? 1.0f - (float(p_index) / float(p_count - 1)) * 2.0f : 0.0f;
	const float radius_at_y = Math::sqrt(MAX(0.0f, 1.0f - y * y));
	const float theta = golden_angle * float(p_index);
	return Vector3(Math::cos(theta) * radius_at_y, y, Math::sin(theta) * radius_at_y);
}
} // namespace

void AmbientProbeVolume3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_size", "size"), &AmbientProbeVolume3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &AmbientProbeVolume3D::get_size);

	ClassDB::bind_method(D_METHOD("set_probe_counts", "counts"), &AmbientProbeVolume3D::set_probe_counts);
	ClassDB::bind_method(D_METHOD("get_probe_counts"), &AmbientProbeVolume3D::get_probe_counts);

	ClassDB::bind_method(D_METHOD("set_ray_count", "ray_count"), &AmbientProbeVolume3D::set_ray_count);
	ClassDB::bind_method(D_METHOD("get_ray_count"), &AmbientProbeVolume3D::get_ray_count);

	ClassDB::bind_method(D_METHOD("set_max_distance", "max_distance"), &AmbientProbeVolume3D::set_max_distance);
	ClassDB::bind_method(D_METHOD("get_max_distance"), &AmbientProbeVolume3D::get_max_distance);

	ClassDB::bind_method(D_METHOD("set_ao_strength", "strength"), &AmbientProbeVolume3D::set_ao_strength);
	ClassDB::bind_method(D_METHOD("get_ao_strength"), &AmbientProbeVolume3D::get_ao_strength);

	ClassDB::bind_method(D_METHOD("set_occluder_root", "path"), &AmbientProbeVolume3D::set_occluder_root);
	ClassDB::bind_method(D_METHOD("get_occluder_root"), &AmbientProbeVolume3D::get_occluder_root);

	ClassDB::bind_method(D_METHOD("bake_ao"), &AmbientProbeVolume3D::bake_ao);
	ClassDB::bind_method(D_METHOD("clear_ao"), &AmbientProbeVolume3D::clear_ao);
	ClassDB::bind_method(D_METHOD("is_baked"), &AmbientProbeVolume3D::is_baked);
	ClassDB::bind_method(D_METHOD("get_ao_at", "world_position"), &AmbientProbeVolume3D::get_ao_at);

	ClassDB::bind_method(D_METHOD("get_bake_button"), &AmbientProbeVolume3D::_get_bake_button);
	ClassDB::bind_method(D_METHOD("get_clear_button"), &AmbientProbeVolume3D::_get_clear_button);

	ClassDB::bind_method(D_METHOD("_set_baked_ao", "data"), &AmbientProbeVolume3D::_set_baked_ao);
	ClassDB::bind_method(D_METHOD("_get_baked_ao"), &AmbientProbeVolume3D::_get_baked_ao);

	ADD_GROUP("Volume", "");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "size", PROPERTY_HINT_RANGE, "0.01,4096,0.01,or_greater,suffix:m"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3I, "probe_counts", PROPERTY_HINT_NONE, ""), "set_probe_counts", "get_probe_counts");

	ADD_GROUP("Bake", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "ray_count", PROPERTY_HINT_RANGE, "4,256,1"), "set_ray_count", "get_ray_count");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_distance", PROPERTY_HINT_RANGE, "0.01,1000,0.01,or_greater,suffix:m"), "set_max_distance", "get_max_distance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ao_strength", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_ao_strength", "get_ao_strength");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "occluder_root", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_occluder_root", "get_occluder_root");

	ADD_GROUP("", "");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "baked_ao", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_INTERNAL), "_set_baked_ao", "_get_baked_ao");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "bake_ao_button", PROPERTY_HINT_TOOL_BUTTON, "Bake AO", PROPERTY_USAGE_EDITOR), "", "get_bake_button");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "clear_ao_button", PROPERTY_HINT_TOOL_BUTTON, "Clear AO", PROPERTY_USAGE_EDITOR), "", "get_clear_button");
}

Callable AmbientProbeVolume3D::_get_bake_button() const {
	return Callable(const_cast<AmbientProbeVolume3D *>(this), "bake_ao");
}

Callable AmbientProbeVolume3D::_get_clear_button() const {
	return Callable(const_cast<AmbientProbeVolume3D *>(this), "clear_ao");
}

void AmbientProbeVolume3D::set_size(const Vector3 &p_size) {
	size = p_size.maxf(0.01);
	update_gizmos();
	update_configuration_warnings();
}

Vector3 AmbientProbeVolume3D::get_size() const {
	return size;
}

void AmbientProbeVolume3D::set_probe_counts(const Vector3i &p_counts) {
	const Vector3i new_counts = Vector3i(
			CLAMP(p_counts.x, 1, AMBIENT_PROBE_VOLUME_MAX_PER_AXIS),
			CLAMP(p_counts.y, 1, AMBIENT_PROBE_VOLUME_MAX_PER_AXIS),
			CLAMP(p_counts.z, 1, AMBIENT_PROBE_VOLUME_MAX_PER_AXIS));
	if (new_counts != probe_counts) {
		probe_counts = new_counts;
		clear_ao(); // Stale data does not match the new grid layout.
	}
	update_gizmos();
	update_configuration_warnings();
}

Vector3i AmbientProbeVolume3D::get_probe_counts() const {
	return probe_counts;
}

void AmbientProbeVolume3D::set_ray_count(int p_ray_count) {
	ray_count = CLAMP(p_ray_count, 4, 256);
}

int AmbientProbeVolume3D::get_ray_count() const {
	return ray_count;
}

void AmbientProbeVolume3D::set_max_distance(float p_max_distance) {
	max_distance = MAX(p_max_distance, 0.01f);
}

float AmbientProbeVolume3D::get_max_distance() const {
	return max_distance;
}

void AmbientProbeVolume3D::set_ao_strength(float p_strength) {
	ao_strength = MAX(p_strength, 0.0f);
}

float AmbientProbeVolume3D::get_ao_strength() const {
	return ao_strength;
}

void AmbientProbeVolume3D::set_occluder_root(const NodePath &p_path) {
	occluder_root = p_path;
}

NodePath AmbientProbeVolume3D::get_occluder_root() const {
	return occluder_root;
}

Vector3 AmbientProbeVolume3D::get_local_probe_position(int p_x, int p_y, int p_z) const {
	const Vector3 half = size * 0.5;
	const float fx = probe_counts.x > 1 ? float(p_x) / float(probe_counts.x - 1) : 0.5f;
	const float fy = probe_counts.y > 1 ? float(p_y) / float(probe_counts.y - 1) : 0.5f;
	const float fz = probe_counts.z > 1 ? float(p_z) / float(probe_counts.z - 1) : 0.5f;
	return Vector3(
			Math::lerp(-half.x, half.x, fx),
			Math::lerp(-half.y, half.y, fy),
			Math::lerp(-half.z, half.z, fz));
}

void AmbientProbeVolume3D::_set_baked_ao(const PackedFloat32Array &p_data) {
	baked_ao = p_data;
}

PackedFloat32Array AmbientProbeVolume3D::_get_baked_ao() const {
	return baked_ao;
}

void AmbientProbeVolume3D::clear_ao() {
	baked_ao.clear();
	update_gizmos();
	update_configuration_warnings();
}

bool AmbientProbeVolume3D::is_baked() const {
	return !baked_ao.is_empty();
}

void AmbientProbeVolume3D::bake_ao() {
	ERR_FAIL_COND_MSG(!is_inside_tree(), "AmbientProbeVolume3D must be inside the SceneTree to bake, since it needs to scan the scene for occluding geometry.");

	Node *root = occluder_root.is_empty() ? nullptr : get_node_or_null(occluder_root);
	if (root == nullptr) {
		root = get_tree()->get_edited_scene_root();
	}
	if (root == nullptr) {
		root = get_tree()->get_current_scene();
	}
	ERR_FAIL_NULL_MSG(root, "AmbientProbeVolume3D could not find a scene root to scan for occluders. Set Occluder Root explicitly.");

	const Transform3D gt = get_global_transform();
	const Transform3D world_to_local = gt.affine_inverse();

	LocalVector<OccluderFace> faces;
	gather_occluder_faces(root, world_to_local, faces);

	const int total_probes = probe_counts.x * probe_counts.y * probe_counts.z;
	PackedFloat32Array new_baked_ao;
	new_baked_ao.resize(total_probes);

	LocalVector<const OccluderFace *> candidates;

	int index = 0;
	for (int zi = 0; zi < probe_counts.z; zi++) {
		for (int yi = 0; yi < probe_counts.y; yi++) {
			for (int xi = 0; xi < probe_counts.x; xi++) {
				const Vector3 probe_pos = get_local_probe_position(xi, yi, zi);

				candidates.clear();
				for (const OccluderFace &f : faces) {
					if (probe_pos.distance_to(f.center) <= max_distance + f.radius) {
						candidates.push_back(&f);
					}
				}

				int occluded = 0;
				for (int r = 0; r < ray_count; r++) {
					const Vector3 dir = fibonacci_sphere_direction(r, ray_count);
					const Vector3 ray_to = probe_pos + dir * max_distance;

					for (const OccluderFace *f : candidates) {
						Vector3 hit;
						if (Face3(f->v0, f->v1, f->v2).intersects_segment(probe_pos, ray_to, &hit)) {
							occluded++;
							break;
						}
					}
				}

				const float occlusion = ray_count > 0 ? float(occluded) / float(ray_count) : 0.0f;
				new_baked_ao.write[index] = CLAMP(1.0f - occlusion * ao_strength, 0.0f, 1.0f);
				index++;
			}
		}
	}

	baked_ao = new_baked_ao;
	update_gizmos();
	update_configuration_warnings();
}

float AmbientProbeVolume3D::get_ao_at(const Vector3 &p_world_position) const {
	if (baked_ao.is_empty()) {
		return 1.0f;
	}

	const Vector3 local_pos = get_global_transform().affine_inverse().xform(p_world_position);
	const Vector3 half = size * 0.5;
	const Vector3 unit = Vector3(
			(local_pos.x + half.x) / MAX(size.x, 0.0001f),
			(local_pos.y + half.y) / MAX(size.y, 0.0001f),
			(local_pos.z + half.z) / MAX(size.z, 0.0001f));

	if (unit.x < 0.0f || unit.x > 1.0f || unit.y < 0.0f || unit.y > 1.0f || unit.z < 0.0f || unit.z > 1.0f) {
		return 1.0f;
	}

	const Vector3 grid_pos = Vector3(
			unit.x * float(probe_counts.x - 1),
			unit.y * float(probe_counts.y - 1),
			unit.z * float(probe_counts.z - 1));

	const int x0 = CLAMP(int(Math::floor(grid_pos.x)), 0, probe_counts.x - 1);
	const int y0 = CLAMP(int(Math::floor(grid_pos.y)), 0, probe_counts.y - 1);
	const int z0 = CLAMP(int(Math::floor(grid_pos.z)), 0, probe_counts.z - 1);
	const int x1 = MIN(x0 + 1, probe_counts.x - 1);
	const int y1 = MIN(y0 + 1, probe_counts.y - 1);
	const int z1 = MIN(z0 + 1, probe_counts.z - 1);

	const float tx = grid_pos.x - float(x0);
	const float ty = grid_pos.y - float(y0);
	const float tz = grid_pos.z - float(z0);

	auto sample = [&](int p_x, int p_y, int p_z) -> float {
		const int i = p_x + probe_counts.x * (p_y + probe_counts.y * p_z);
		return baked_ao[i];
	};

	const float c00 = Math::lerp(sample(x0, y0, z0), sample(x1, y0, z0), tx);
	const float c10 = Math::lerp(sample(x0, y1, z0), sample(x1, y1, z0), tx);
	const float c01 = Math::lerp(sample(x0, y0, z1), sample(x1, y0, z1), tx);
	const float c11 = Math::lerp(sample(x0, y1, z1), sample(x1, y1, z1), tx);
	const float c0 = Math::lerp(c00, c10, ty);
	const float c1 = Math::lerp(c01, c11, ty);
	return Math::lerp(c0, c1, tz);
}

PackedStringArray AmbientProbeVolume3D::get_configuration_warnings() const {
	PackedStringArray warnings = Node3D::get_configuration_warnings();

	if (size.x <= 0.0 || size.y <= 0.0 || size.z <= 0.0) {
		warnings.push_back(RTR("Size must be greater than zero on every axis."));
	}

	const int64_t total_probes = int64_t(probe_counts.x) * int64_t(probe_counts.y) * int64_t(probe_counts.z);
	if (total_probes > AMBIENT_PROBE_VOLUME_WARN_PROBE_COUNT) {
		warnings.push_back(vformat(RTR("Probe Counts would generate %d probes, which may noticeably increase bake time and, if sampled every frame, its runtime cost. Consider lowering Probe Counts or splitting this volume."), total_probes));
	}

	if (!is_baked()) {
		warnings.push_back(RTR("Not baked yet. Press Bake AO after placing the occluding geometry it should test against."));
	}

	return warnings;
}

AmbientProbeVolume3D::AmbientProbeVolume3D() {
}
