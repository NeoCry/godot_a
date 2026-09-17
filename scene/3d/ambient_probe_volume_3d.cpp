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
#include "core/object/class_db.h"
#include "core/templates/local_vector.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/visual_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "scene/resources/shader.h"

// Hard cap per axis to keep a single accidental huge grid from hanging the editor.
static const int AMBIENT_PROBE_VOLUME_MAX_PER_AXIS = 64;
static const int AMBIENT_PROBE_VOLUME_WARN_PROBE_COUNT = 4096;

// Unshaded spatial shader used by set_debug_preview_on_meshes(): the instance uniform
// is the same one apply_to_instances() sets, so the two features are compatible with
// each other and with a hand-written shader using the same parameter name.
static const char *AMBIENT_PROBE_VOLUME_DEBUG_SHADER_CODE =
		"shader_type spatial;\n"
		"render_mode unshaded;\n"
		"\n"
		"instance uniform float ambient_occlusion : hint_range(0.0, 1.0) = 1.0;\n"
		"\n"
		"void fragment() {\n"
		"\tALBEDO = vec3(ambient_occlusion);\n"
		"}\n";

namespace {
struct OccluderFace {
	Vector3 v0, v1, v2;
	Vector3 center;
	float radius = 0.0f;
};

void add_occluder_mesh(const Ref<Mesh> &p_mesh, const Transform3D &p_xf, LocalVector<OccluderFace> &r_faces) {
	if (p_mesh.is_null()) {
		return;
	}
	Vector<Face3> faces = p_mesh->get_faces();
	for (const Face3 &face : faces) {
		OccluderFace of;
		of.v0 = p_xf.xform(face.vertex[0]);
		of.v1 = p_xf.xform(face.vertex[1]);
		of.v2 = p_xf.xform(face.vertex[2]);
		of.center = (of.v0 + of.v1 + of.v2) / 3.0f;
		of.radius = MAX(of.center.distance_to(of.v0), MAX(of.center.distance_to(of.v1), of.center.distance_to(of.v2)));
		r_faces.push_back(of);
	}
}

void gather_occluder_faces(Node *p_node, const Transform3D &p_world_to_local, LocalVector<OccluderFace> &r_faces) {
	MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(p_node);
	if (mi != nullptr && mi->is_visible_in_tree()) {
		add_occluder_mesh(mi->get_mesh(), p_world_to_local * mi->get_global_transform(), r_faces);
	}

	Node3D *s = Object::cast_to<Node3D>(p_node);
	if (mi == nullptr && s != nullptr && s->is_visible_in_tree()) {
		// Nodes that don't carry a single Mesh of their own but can still contribute
		// baked static geometry (e.g. GridMap) expose it the same way LightmapGI/VoxelGI
		// already look for it: an optional get_bake_meshes() method returning
		// [mesh0, local_xf0, mesh1, local_xf1, ...]. Calling call() on a node that has no
		// such method is a documented no-op (returns a null Variant, no error), so this is
		// safe to try unconditionally on every Node3D.
		Array bake_meshes = s->call("get_bake_meshes");
		if (bake_meshes.size() > 0 && (bake_meshes.size() & 1) == 0) {
			const Transform3D base_xf = p_world_to_local * s->get_global_transform();
			for (int i = 0; i < bake_meshes.size(); i += 2) {
				const Ref<Mesh> mesh = bake_meshes[i];
				const Transform3D local_xf = bake_meshes[i + 1];
				add_occluder_mesh(mesh, base_xf * local_xf, r_faces);
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

	ClassDB::bind_method(D_METHOD("set_apply_target", "path"), &AmbientProbeVolume3D::set_apply_target);
	ClassDB::bind_method(D_METHOD("get_apply_target"), &AmbientProbeVolume3D::get_apply_target);

	ClassDB::bind_method(D_METHOD("set_apply_shader_parameter", "name"), &AmbientProbeVolume3D::set_apply_shader_parameter);
	ClassDB::bind_method(D_METHOD("get_apply_shader_parameter"), &AmbientProbeVolume3D::get_apply_shader_parameter);

	ClassDB::bind_method(D_METHOD("bake_ao"), &AmbientProbeVolume3D::bake_ao);
	ClassDB::bind_method(D_METHOD("clear_ao"), &AmbientProbeVolume3D::clear_ao);
	ClassDB::bind_method(D_METHOD("is_baked"), &AmbientProbeVolume3D::is_baked);
	ClassDB::bind_method(D_METHOD("get_ao_at", "world_position"), &AmbientProbeVolume3D::get_ao_at);
	ClassDB::bind_method(D_METHOD("get_probe_ao", "x", "y", "z"), &AmbientProbeVolume3D::get_probe_ao);
	ClassDB::bind_method(D_METHOD("apply_to_instances"), &AmbientProbeVolume3D::apply_to_instances);

	ClassDB::bind_method(D_METHOD("set_debug_preview_on_meshes", "enabled"), &AmbientProbeVolume3D::set_debug_preview_on_meshes);
	ClassDB::bind_method(D_METHOD("is_debug_preview_on_meshes"), &AmbientProbeVolume3D::is_debug_preview_on_meshes);

	ClassDB::bind_method(D_METHOD("get_bake_button"), &AmbientProbeVolume3D::_get_bake_button);
	ClassDB::bind_method(D_METHOD("get_clear_button"), &AmbientProbeVolume3D::_get_clear_button);
	ClassDB::bind_method(D_METHOD("get_apply_button"), &AmbientProbeVolume3D::_get_apply_button);

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

	ADD_GROUP("Apply", "");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "apply_target", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_apply_target", "get_apply_target");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "apply_shader_parameter"), "set_apply_shader_parameter", "get_apply_shader_parameter");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug_preview_on_meshes"), "set_debug_preview_on_meshes", "is_debug_preview_on_meshes");

	ADD_GROUP("", "");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "baked_ao", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_INTERNAL), "_set_baked_ao", "_get_baked_ao");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "bake_ao_button", PROPERTY_HINT_TOOL_BUTTON, "Bake AO", PROPERTY_USAGE_EDITOR), "", "get_bake_button");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "clear_ao_button", PROPERTY_HINT_TOOL_BUTTON, "Clear AO", PROPERTY_USAGE_EDITOR), "", "get_clear_button");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "apply_to_instances_button", PROPERTY_HINT_TOOL_BUTTON, "Apply To Instances", PROPERTY_USAGE_EDITOR), "", "get_apply_button");
}

Callable AmbientProbeVolume3D::_get_bake_button() const {
	return Callable(const_cast<AmbientProbeVolume3D *>(this), "bake_ao");
}

Callable AmbientProbeVolume3D::_get_clear_button() const {
	return Callable(const_cast<AmbientProbeVolume3D *>(this), "clear_ao");
}

Callable AmbientProbeVolume3D::_get_apply_button() const {
	return Callable(const_cast<AmbientProbeVolume3D *>(this), "apply_to_instances");
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

void AmbientProbeVolume3D::set_apply_target(const NodePath &p_path) {
	apply_target = p_path;
}

NodePath AmbientProbeVolume3D::get_apply_target() const {
	return apply_target;
}

void AmbientProbeVolume3D::set_apply_shader_parameter(const StringName &p_name) {
	apply_shader_parameter = p_name;
}

StringName AmbientProbeVolume3D::get_apply_shader_parameter() const {
	return apply_shader_parameter;
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

float AmbientProbeVolume3D::get_probe_ao(int p_x, int p_y, int p_z) const {
	if (baked_ao.is_empty()) {
		return 1.0f;
	}
	if (p_x < 0 || p_x >= probe_counts.x || p_y < 0 || p_y >= probe_counts.y || p_z < 0 || p_z >= probe_counts.z) {
		return 1.0f;
	}
	const int i = p_x + probe_counts.x * (p_y + probe_counts.y * p_z);
	return baked_ao[i];
}

void AmbientProbeVolume3D::_set_baked_ao(const PackedFloat32Array &p_data) {
	baked_ao = p_data;
}

PackedFloat32Array AmbientProbeVolume3D::_get_baked_ao() const {
	return baked_ao;
}

void AmbientProbeVolume3D::clear_ao() {
	baked_ao.clear();
	last_bake_occluder_face_count = -1;
	last_bake_min_occluder_distance = -1.0f;
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
	last_bake_occluder_face_count = int(faces.size());

	// How far (in local space) the closest occluder vertex is from the probe grid's own
	// box: if that's farther than Max Distance, no probe could ever reach it no matter
	// how the rest of the bake is tuned, which is the single most common setup mistake
	// (volume placed/sized without regard for the scene's actual scale).
	last_bake_min_occluder_distance = -1.0f;
	if (!faces.is_empty()) {
		const Vector3 probe_half = size * 0.5;
		const AABB probe_box(-probe_half, size);

		Vector3 occ_min = faces[0].v0;
		Vector3 occ_max = faces[0].v0;
		for (const OccluderFace &f : faces) {
			occ_min = occ_min.min(f.v0.min(f.v1.min(f.v2)));
			occ_max = occ_max.max(f.v0.max(f.v1.max(f.v2)));
		}

		const Vector3 delta(
				MAX(MAX(occ_min.x - probe_box.get_end().x, probe_box.position.x - occ_max.x), 0.0f),
				MAX(MAX(occ_min.y - probe_box.get_end().y, probe_box.position.y - occ_max.y), 0.0f),
				MAX(MAX(occ_min.z - probe_box.get_end().z, probe_box.position.z - occ_max.z), 0.0f));
		last_bake_min_occluder_distance = delta.length();
	}

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

	print_line(vformat("AmbientProbeVolume3D \"%s\": baked %d probes against %d occluder triangle(s) found under \"%s\" (closest occluder is %s from this volume's box; Max Distance is %.2f).",
			String(get_name()), total_probes, faces.size(), root->get_name(),
			faces.is_empty() ? String("n/a") : vformat("%.2fm", last_bake_min_occluder_distance), max_distance));
	if (faces.is_empty()) {
		WARN_PRINT(vformat("AmbientProbeVolume3D \"%s\": found no occluder geometry under Occluder Root (\"%s\"). Only visible MeshInstance3D nodes (and nodes like GridMap that provide baked meshes) are used as occluders; make sure Occluder Root actually contains some, and that Max Distance/the probe grid are close enough to reach them.",
				String(get_name()), root->get_name()));
	} else if (last_bake_min_occluder_distance > max_distance) {
		WARN_PRINT(vformat("AmbientProbeVolume3D \"%s\": the closest occluder geometry is about %.2fm away from this volume's box, but Max Distance is only %.2f — no probe can reach it. Increase Max Distance to at least %.2f, and/or move/resize this volume (Size) so its box actually overlaps or sits closer to your geometry.",
				String(get_name()), last_bake_min_occluder_distance, max_distance, last_bake_min_occluder_distance));
	}

	// If the debug preview is currently on, its instances are already sampling
	// get_ao_at() via a material - but the actual per-instance shader parameter values
	// were only pushed once, when it was toggled on, so a re-bake would otherwise leave
	// it showing stale results until toggled off and back on.
	if (debug_preview_on_meshes) {
		set_debug_preview_on_meshes(true);
	}

	update_gizmos();
	update_configuration_warnings();
}

Node *AmbientProbeVolume3D::_resolve_apply_root() const {
	Node *root = apply_target.is_empty() ? nullptr : get_node_or_null(apply_target);
	if (root == nullptr) {
		root = get_tree()->get_edited_scene_root();
	}
	if (root == nullptr) {
		root = get_tree()->get_current_scene();
	}
	return root;
}

void AmbientProbeVolume3D::_apply_to_instances(Node *p_node, int &r_count) {
	GeometryInstance3D *gi = Object::cast_to<GeometryInstance3D>(p_node);
	if (gi != nullptr) {
		gi->set_instance_shader_parameter(apply_shader_parameter, get_ao_at(gi->get_global_position()));
		r_count++;
	}

	for (int i = 0; i < p_node->get_child_count(); i++) {
		_apply_to_instances(p_node->get_child(i), r_count);
	}
}

void AmbientProbeVolume3D::apply_to_instances() {
	ERR_FAIL_COND_MSG(!is_inside_tree(), "AmbientProbeVolume3D must be inside the SceneTree to apply, since it needs to scan the scene for GeometryInstance3D nodes.");
	ERR_FAIL_COND_MSG(!is_baked(), "AmbientProbeVolume3D has not been baked yet; press Bake AO first.");

	Node *root = _resolve_apply_root();
	ERR_FAIL_NULL_MSG(root, "AmbientProbeVolume3D could not find a scene root to apply to. Set Apply Target explicitly.");

	int count = 0;
	_apply_to_instances(root, count);
	last_apply_instance_count = count;

	print_line(vformat("AmbientProbeVolume3D \"%s\": set instance shader parameter \"%s\" on %d GeometryInstance3D node(s) under \"%s\".",
			String(get_name()), String(apply_shader_parameter), count, root->get_name()));
	if (count == 0) {
		WARN_PRINT(vformat("AmbientProbeVolume3D \"%s\": found no GeometryInstance3D under Apply Target (\"%s\") to apply to.",
				String(get_name()), root->get_name()));
	} else {
		WARN_PRINT(vformat("AmbientProbeVolume3D \"%s\": this only has a visible effect on meshes whose own shader declares \"instance uniform float %s : hint_range(0, 1) = 1.0;\" and actually uses it (e.g. multiplied into ALBEDO). A default/unmodified StandardMaterial3D will not show any difference — press Debug Preview On Meshes instead if you just want to SEE the baked AO on the real geometry without writing a shader.",
				String(get_name()), String(apply_shader_parameter)));
	}
}

void AmbientProbeVolume3D::_set_debug_material_recursive(Node *p_node, const Ref<Material> &p_material, int &r_count) {
	GeometryInstance3D *gi = Object::cast_to<GeometryInstance3D>(p_node);
	if (gi != nullptr) {
		if (p_material.is_valid()) {
			gi->set_instance_shader_parameter(apply_shader_parameter, get_ao_at(gi->get_global_position()));
		}
		gi->set_material_override(p_material);
		r_count++;
	}

	for (int i = 0; i < p_node->get_child_count(); i++) {
		_set_debug_material_recursive(p_node->get_child(i), p_material, r_count);
	}
}

void AmbientProbeVolume3D::set_debug_preview_on_meshes(bool p_enabled) {
	debug_preview_on_meshes = p_enabled;

	if (!is_inside_tree()) {
		return;
	}

	Node *root = _resolve_apply_root();
	if (root == nullptr) {
		return;
	}

	Ref<Material> material;
	if (p_enabled) {
		if (debug_material.is_null()) {
			Ref<Shader> shader;
			shader.instantiate();
			shader->set_code(AMBIENT_PROBE_VOLUME_DEBUG_SHADER_CODE);
			debug_material.instantiate();
			debug_material->set_shader(shader);
		}
		material = debug_material;
	}

	int count = 0;
	_set_debug_material_recursive(root, material, count);

	print_line(vformat("AmbientProbeVolume3D \"%s\": debug AO preview %s on %d GeometryInstance3D node(s) under \"%s\"%s.",
			String(get_name()), p_enabled ? "enabled" : "disabled", count, root->get_name(),
			(p_enabled && !is_baked()) ? " (not baked yet, so everything will show as fully lit)" : ""));
}

bool AmbientProbeVolume3D::is_debug_preview_on_meshes() const {
	return debug_preview_on_meshes;
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
	} else {
		bool any_occlusion = false;
		for (int64_t i = 0; i < baked_ao.size(); i++) {
			if (baked_ao[i] < 0.999f) {
				any_occlusion = true;
				break;
			}
		}
		if (!any_occlusion) {
			if (last_bake_occluder_face_count == 0) {
				warnings.push_back(RTR("The last bake found zero occluder triangles: Occluder Root doesn't contain any visible MeshInstance3D (or GridMap-like) geometry, so every probe was left fully lit. Check the Output panel for the exact message printed by the last bake, and make sure Occluder Root points at a node that actually contains your level geometry."));
			} else if (last_bake_min_occluder_distance > max_distance) {
				warnings.push_back(vformat(RTR("The last bake found %d occluder triangle(s), but the closest one is about %.2fm away from this volume's box while Max Distance is only %.2f — nothing was reachable. Increase Max Distance to at least %.2f, and/or move/resize this volume (Size) to actually overlap or sit closer to your geometry, then bake again."), last_bake_occluder_face_count, last_bake_min_occluder_distance, max_distance, last_bake_min_occluder_distance));
			} else {
				warnings.push_back(vformat(RTR("The last bake found %d occluder triangle(s) within reach (closest one about %.2fm away, Max Distance %.2f), but no probe ended up occluded by any of them (every probe is fully lit). This can happen if the geometry is very thin, single-sided in a way that misses rays, or if the probe grid just doesn't line up with it — try increasing Ray Count, or moving/resizing this volume so more probes sit near actual surfaces. It does not mean nothing is receiving the baked data — see the class description for how baked AO needs to be applied (e.g. FoliageSpawner3D.ambient_occlusion_volume, or Apply To Instances)."), last_bake_occluder_face_count, last_bake_min_occluder_distance, max_distance));
			}
		}
	}

	if (!apply_target.is_empty()) {
		Node *target = is_inside_tree() ? get_node_or_null(apply_target) : nullptr;
		if (target == nullptr) {
			warnings.push_back(RTR("Apply Target does not point to a valid node. Assign one, or clear the path."));
		}
	}

	if (debug_preview_on_meshes) {
		warnings.push_back(RTR("Debug Preview On Meshes is enabled: affected GeometryInstance3D nodes are being rendered with a generated grayscale debug material instead of their own, for diagnostics only. Turn it back off once you're done checking the bake."));
	}

	return warnings;
}

AmbientProbeVolume3D::AmbientProbeVolume3D() {
}
