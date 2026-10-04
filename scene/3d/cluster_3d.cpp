/**************************************************************************/
/*  cluster_3d.cpp                                                        */
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

#include "cluster_3d.h"

#include "core/config/engine.h"
#include "core/math/triangle_mesh.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/mutex.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/resources/3d/importer_mesh.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/material.h"
#include "scene/resources/multimesh.h"
#include "servers/rendering/rendering_server.h"

#ifndef PHYSICS_3D_DISABLED
#include "scene/resources/3d/concave_polygon_shape_3d.h"
#include "scene/resources/3d/convex_polygon_shape_3d.h"
#include "servers/physics_3d/physics_server_3d.h"
#endif // PHYSICS_3D_DISABLED

#ifndef NAVIGATION_3D_DISABLED
#include "scene/resources/3d/navigation_mesh_source_geometry_data_3d.h"
#include "scene/resources/navigation_mesh.h"
#include "servers/navigation_3d/navigation_server_3d.h"
#endif // NAVIGATION_3D_DISABLED

#ifndef NAVIGATION_3D_DISABLED
Callable Cluster3D::_navmesh_source_geometry_parsing_callback;
RID Cluster3D::_navmesh_source_geometry_parser;
#endif // NAVIGATION_3D_DISABLED

// Parts.

Transform3D Cluster3D::_get_render_transform() const {
	if (is_inside_tree() && get_tree()->is_physics_interpolation_enabled()) {
		// Kept up to date by SceneTreeFTI, which then calls fti_update_servers_xform().
		return _get_cached_global_transform_interpolated();
	}
	return get_global_transform();
}

void Cluster3D::_part_create_instance(Part &r_part) {
	RenderingServer *rs = RS::get_singleton();
	r_part.instance = rs->instance_create();
	rs->instance_attach_object_instance_id(r_part.instance, get_instance_id());
	_part_update_base(r_part);
	_part_update_settings(r_part);

	if (is_inside_world()) {
		rs->instance_set_scenario(r_part.instance, get_world_3d()->get_scenario());
		_part_update_transform(r_part, _get_render_transform());
	}
	rs->instance_set_visible(r_part.instance, is_inside_world() && _is_vi_visible());
}

void Cluster3D::_part_free_instance(Part &r_part) {
	if (r_part.instance.is_valid()) {
		RS::get_singleton()->free_rid(r_part.instance);
		r_part.instance = RID();
	}
}

void Cluster3D::_part_update_base(Part &r_part) {
	RS::get_singleton()->instance_set_base(r_part.instance, r_part.mesh.is_valid() ? r_part.mesh->get_rid() : RID());
	_part_update_materials(r_part);
}

void Cluster3D::_part_update_materials(const Part &p_part) {
	if (p_part.mesh.is_null()) {
		return;
	}
	RenderingServer *rs = RS::get_singleton();
	const int surface_count = p_part.mesh->get_surface_count();
	for (int i = 0; i < surface_count; i++) {
		Ref<Material> material = i < p_part.materials.size() ? Ref<Material>(p_part.materials[i]) : Ref<Material>();
		rs->instance_set_surface_override_material(p_part.instance, i, material.is_valid() ? material->get_rid() : RID());
	}
}

void Cluster3D::_part_update_settings(const Part &p_part) {
	RenderingServer *rs = RS::get_singleton();
	const RID part_instance = p_part.instance;

	rs->instance_set_layer_mask(part_instance, get_layer_mask());
	rs->instance_set_pivot_data(part_instance, get_sorting_offset(), is_sorting_use_aabb_center());

	const Ref<Material> override = get_material_override();
	rs->instance_geometry_set_material_override(part_instance, override.is_valid() ? override->get_rid() : RID());
	const Ref<Material> overlay = get_material_overlay();
	rs->instance_geometry_set_material_overlay(part_instance, overlay.is_valid() ? overlay->get_rid() : RID());

	rs->instance_geometry_set_transparency(part_instance, get_transparency());
	rs->instance_geometry_set_visibility_range(part_instance, get_visibility_range_begin(), get_visibility_range_end(), get_visibility_range_begin_margin(), get_visibility_range_end_margin(), (RSE::VisibilityRangeFadeMode)get_visibility_range_fade_mode());
	rs->instance_geometry_set_cast_shadows_setting(part_instance, (RSE::ShadowCastingSetting)get_cast_shadows_setting());
	rs->instance_set_extra_visibility_margin(part_instance, get_extra_cull_margin());
	rs->instance_geometry_set_lod_bias(part_instance, get_lod_bias());

	const GIMode mode = get_gi_mode();
	rs->instance_geometry_set_flag(part_instance, RSE::INSTANCE_FLAG_USE_BAKED_LIGHT, mode == GI_MODE_STATIC);
	rs->instance_geometry_set_flag(part_instance, RSE::INSTANCE_FLAG_USE_DYNAMIC_GI, mode == GI_MODE_DYNAMIC);
	rs->instance_geometry_set_flag(part_instance, RSE::INSTANCE_FLAG_IGNORE_OCCLUSION_CULLING, is_ignoring_occlusion_culling());
	rs->instance_geometry_set_flag(part_instance, RSE::INSTANCE_FLAG_IGNORE_SCREEN_SPACE_SHADOWS, is_ignoring_screen_space_shadows());
	rs->instance_geometry_set_virtual_texture_layers(part_instance, get_virtual_texture_draw_layers(), is_virtual_texture_draw_in_main_pass_enabled());

	// The custom AABB is the cluster's, in its space; each part is given the
	// part of it that is its own space.
	const AABB cluster_custom_aabb = get_custom_aabb();
	rs->instance_set_custom_aabb(part_instance, cluster_custom_aabb.has_volume() ? p_part.transform.affine_inverse().xform(cluster_custom_aabb) : AABB());

	rs->instance_set_visibility_parent(part_instance, _get_visibility_parent_instance());

	const HashMap<StringName, Variant> &shader_parameters = _get_instance_shader_parameters();
	for (const KeyValue<StringName, Variant> &E : shader_parameters) {
		rs->instance_geometry_set_shader_parameter(part_instance, E.key, E.value.get_type() == Variant::OBJECT ? Variant(RID(E.value)) : E.value);
	}
}

void Cluster3D::_part_update_transform(const Part &p_part, const Transform3D &p_render_transform) {
	RS::get_singleton()->instance_set_transform(p_part.instance, p_render_transform * p_part.transform);
}

void Cluster3D::_update_all_part_transforms(const Transform3D &p_render_transform) {
	for (const Part &part : parts) {
		_part_update_transform(part, p_render_transform);
	}
}

void Cluster3D::_update_parts_visibility() {
	const bool visible = is_inside_world() && _is_vi_visible();
	if (visible) {
		_update_all_part_transforms(_get_render_transform());
	}
	RenderingServer *rs = RS::get_singleton();
	for (const Part &part : parts) {
		rs->instance_set_visible(part.instance, visible);
	}
}

void Cluster3D::_part_set_mesh(Part &r_part, const Ref<Mesh> &p_mesh) {
	if (r_part.mesh == p_mesh) {
		return;
	}
	_mesh_remove_user(r_part.mesh);
	r_part.mesh = p_mesh;
	_mesh_add_user(r_part.mesh);
	_part_update_base(r_part);
}

// Whatever the parts' geometry is, as a whole, changed: their bounds, what
// the editor picks them with, and what the runtime picker does.
void Cluster3D::_geometry_changed() {
	aabb_dirty = true;
	triangle_mesh.unref();
	update_gizmos();
}

// Parts were added or removed, or their meshes changed.
void Cluster3D::_structure_changed() {
	_geometry_changed();
#ifndef PHYSICS_3D_DISABLED
	_update_collision_shapes();
#endif // PHYSICS_3D_DISABLED
	update_configuration_warnings();
}

void Cluster3D::_mesh_add_user(const Ref<Mesh> &p_mesh) {
	if (p_mesh.is_null()) {
		return;
	}
	int &users = mesh_users[p_mesh->get_instance_id()];
	if (users == 0) {
		p_mesh->connect_changed(callable_mp(this, &Cluster3D::_mesh_changed));
	}
	users++;
}

void Cluster3D::_mesh_remove_user(const Ref<Mesh> &p_mesh) {
	if (p_mesh.is_null()) {
		return;
	}
	HashMap<ObjectID, int>::Iterator E = mesh_users.find(p_mesh->get_instance_id());
	ERR_FAIL_COND(!E);
	E->value--;
	if (E->value == 0) {
		p_mesh->disconnect_changed(callable_mp(this, &Cluster3D::_mesh_changed));
		mesh_users.remove(E);
	}
}

void Cluster3D::_mesh_changed() {
	// Not knowing which one changed, start over with all of them: they
	// rarely change outside of the editor.
#ifndef PHYSICS_3D_DISABLED
	for (const KeyValue<ObjectID, int> &E : mesh_users) {
		_forget_collision_shapes(E.key);
	}
#endif // PHYSICS_3D_DISABLED
	for (Part &part : parts) {
		_part_update_base(part);
	}
	_structure_changed();
}

void Cluster3D::set_part_count(int p_count) {
	ERR_FAIL_COND(p_count < 0);
	const int old_count = parts.size();
	if (p_count == old_count) {
		return;
	}
	for (int i = p_count; i < old_count; i++) {
		_mesh_remove_user(parts[i].mesh);
		_part_free_instance(parts[i]);
	}
	parts.resize(p_count);
	for (int i = old_count; i < p_count; i++) {
		parts[i] = Part();
		_part_create_instance(parts[i]);
	}
	_structure_changed();
	notify_property_list_changed();
}

int Cluster3D::get_part_count() const {
	return parts.size();
}

int Cluster3D::add_part(const Ref<Mesh> &p_mesh, const Transform3D &p_transform, const TypedArray<Material> &p_materials) {
	Part part;
	part.mesh = p_mesh;
	part.transform = p_transform;
	part.materials = TypedArray<Material>(p_materials.duplicate());
	_mesh_add_user(part.mesh);
	parts.push_back(part);
	_part_create_instance(parts[parts.size() - 1]);
	_structure_changed();
	notify_property_list_changed();
	return parts.size() - 1;
}

void Cluster3D::remove_part(int p_part) {
	ERR_FAIL_INDEX(p_part, (int)parts.size());
	_mesh_remove_user(parts[p_part].mesh);
	_part_free_instance(parts[p_part]);
	parts.remove_at(p_part);
	_structure_changed();
	notify_property_list_changed();
}

void Cluster3D::clear_parts() {
	set_part_count(0);
}

void Cluster3D::set_part_mesh(int p_part, const Ref<Mesh> &p_mesh) {
	ERR_FAIL_INDEX(p_part, (int)parts.size());
	if (parts[p_part].mesh == p_mesh) {
		return;
	}
	_part_set_mesh(parts[p_part], p_mesh);
	_structure_changed();
}

Ref<Mesh> Cluster3D::get_part_mesh(int p_part) const {
	ERR_FAIL_INDEX_V(p_part, (int)parts.size(), Ref<Mesh>());
	return parts[p_part].mesh;
}

void Cluster3D::set_part_transform(int p_part, const Transform3D &p_transform) {
	ERR_FAIL_INDEX(p_part, (int)parts.size());
	Part &part = parts[p_part];
	if (part.transform == p_transform) {
		return;
	}
	part.transform = p_transform;
	if (is_inside_world()) {
		_part_update_transform(part, _get_render_transform());
	}
	if (get_custom_aabb().has_volume()) {
		_part_update_settings(part);
	}
#ifndef PHYSICS_3D_DISABLED
	_update_collision_shape_transform(p_part);
#endif // PHYSICS_3D_DISABLED
	_geometry_changed();
}

Transform3D Cluster3D::get_part_transform(int p_part) const {
	ERR_FAIL_INDEX_V(p_part, (int)parts.size(), Transform3D());
	return parts[p_part].transform;
}

void Cluster3D::set_part_materials(int p_part, const TypedArray<Material> &p_materials) {
	ERR_FAIL_INDEX(p_part, (int)parts.size());
	Part &part = parts[p_part];
	part.materials = TypedArray<Material>(p_materials.duplicate());
	// Surfaces past the new list go back to the mesh's own materials.
	_part_update_base(part);
}

TypedArray<Material> Cluster3D::get_part_materials(int p_part) const {
	ERR_FAIL_INDEX_V(p_part, (int)parts.size(), TypedArray<Material>());
	return parts[p_part].materials.duplicate();
}

void Cluster3D::set_part_surface_material(int p_part, int p_surface, const Ref<Material> &p_material) {
	ERR_FAIL_INDEX(p_part, (int)parts.size());
	ERR_FAIL_COND(p_surface < 0);
	Part &part = parts[p_part];
	if (p_surface >= part.materials.size()) {
		if (p_material.is_null()) {
			return;
		}
		part.materials.resize(p_surface + 1);
	}
	part.materials[p_surface] = p_material;
	_part_update_materials(part);
}

Ref<Material> Cluster3D::get_part_surface_material(int p_part, int p_surface) const {
	ERR_FAIL_INDEX_V(p_part, (int)parts.size(), Ref<Material>());
	const Part &part = parts[p_part];
	return p_surface >= 0 && p_surface < part.materials.size() ? Ref<Material>(part.materials[p_surface]) : Ref<Material>();
}

Ref<Material> Cluster3D::get_part_active_material(int p_part, int p_surface) const {
	ERR_FAIL_INDEX_V(p_part, (int)parts.size(), Ref<Material>());
	if (get_material_override().is_valid()) {
		return get_material_override();
	}
	const Ref<Material> own = get_part_surface_material(p_part, p_surface);
	if (own.is_valid()) {
		return own;
	}
	const Ref<Mesh> &mesh = parts[p_part].mesh;
	if (mesh.is_valid() && p_surface >= 0 && p_surface < mesh->get_surface_count()) {
		return mesh->surface_get_material(p_surface);
	}
	return Ref<Material>();
}

AABB Cluster3D::get_part_aabb(int p_part) const {
	ERR_FAIL_INDEX_V(p_part, (int)parts.size(), AABB());
	const Part &part = parts[p_part];
	return part.mesh.is_valid() ? part.transform.xform(part.mesh->get_aabb()) : AABB(part.transform.origin, Vector3());
}

RID Cluster3D::get_part_instance(int p_part) const {
	ERR_FAIL_INDEX_V(p_part, (int)parts.size(), RID());
	return parts[p_part].instance;
}

Array Cluster3D::_get_parts_snapshot() const {
	Array snapshot;
	snapshot.resize(parts.size() * 3);
	for (uint32_t i = 0; i < parts.size(); i++) {
		snapshot[i * 3 + 0] = parts[i].mesh;
		snapshot[i * 3 + 1] = parts[i].transform;
		snapshot[i * 3 + 2] = parts[i].materials.duplicate();
	}
	return snapshot;
}

void Cluster3D::_set_parts_snapshot(const Array &p_snapshot) {
	ERR_FAIL_COND(p_snapshot.size() % 3 != 0);
	const int count = p_snapshot.size() / 3;
	for (int i = count; i < (int)parts.size(); i++) {
		_mesh_remove_user(parts[i].mesh);
		_part_free_instance(parts[i]);
	}
	const int old_count = parts.size();
	parts.resize(count);
	for (int i = 0; i < count; i++) {
		Part &part = parts[i];
		if (i >= old_count) {
			part = Part();
		}
		part.transform = p_snapshot[i * 3 + 1];
		part.materials = TypedArray<Material>(Array(p_snapshot[i * 3 + 2]).duplicate());
		if (i >= old_count) {
			part.mesh = p_snapshot[i * 3 + 0];
			_mesh_add_user(part.mesh);
			_part_create_instance(part);
		} else {
			_part_set_mesh(part, p_snapshot[i * 3 + 0]);
			_part_update_materials(part);
			_part_update_settings(part);
		}
	}
	if (is_inside_world()) {
		_update_all_part_transforms(_get_render_transform());
	}
	_structure_changed();
	notify_property_list_changed();
}

// The transform of p_node relative to p_root, which it is under, even when
// they are not in the tree (as a scene just instantiated is).
static Transform3D _get_relative_transform(const Node *p_root, const Node *p_node) {
	Transform3D xform;
	for (const Node *n = p_node; n && n != p_root; n = n->get_parent()) {
		const Node3D *n3d = Object::cast_to<Node3D>(n);
		if (n3d) {
			xform = n3d->get_transform() * xform;
			if (n3d->is_set_as_top_level()) {
				break;
			}
		}
	}
	return xform;
}

static void _gather_parts(Cluster3D *p_cluster, const Node *p_root, const Node *p_node, const Transform3D &p_transform, int &r_added) {
	const Transform3D xform = p_transform * _get_relative_transform(p_root, p_node);

	const MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(p_node);
	if (mi && mi->get_mesh().is_valid()) {
		TypedArray<Material> materials;
		const Ref<Material> override = mi->get_material_override();
		const int surface_count = mi->get_mesh()->get_surface_count();
		for (int i = 0; i < surface_count; i++) {
			materials.push_back(override.is_valid() ? override : mi->get_surface_override_material(i));
		}
		// Trailing empty slots say nothing.
		while (!materials.is_empty() && Ref<Material>(materials.back()).is_null()) {
			materials.pop_back();
		}
		p_cluster->add_part(mi->get_mesh(), xform, materials);
		r_added++;
	}

	const Cluster3D *cluster = Object::cast_to<Cluster3D>(p_node);
	if (cluster && cluster != p_cluster) {
		for (int i = 0; i < cluster->get_part_count(); i++) {
			p_cluster->add_part(cluster->get_part_mesh(i), xform * cluster->get_part_transform(i), cluster->get_part_materials(i));
			r_added++;
		}
	}

	const MultiMeshInstance3D *mmi = Object::cast_to<MultiMeshInstance3D>(p_node);
	if (mmi && mmi->get_multimesh().is_valid()) {
		const Ref<MultiMesh> multimesh = mmi->get_multimesh();
		if (multimesh->get_mesh().is_valid() && multimesh->get_transform_format() == MultiMesh::TRANSFORM_3D) {
			const int count = multimesh->get_visible_instance_count() < 0 ? multimesh->get_instance_count() : multimesh->get_visible_instance_count();
			for (int i = 0; i < count; i++) {
				p_cluster->add_part(multimesh->get_mesh(), xform * multimesh->get_instance_transform(i));
				r_added++;
			}
		}
	}

	for (int i = 0; i < p_node->get_child_count(false); i++) {
		_gather_parts(p_cluster, p_root, p_node->get_child(i, false), p_transform, r_added);
	}
}

int Cluster3D::add_parts_from_node(Node *p_node, const Transform3D &p_transform) {
	ERR_FAIL_NULL_V(p_node, 0);
	int added = 0;
	_gather_parts(this, p_node, p_node, p_transform, added);
	return added;
}

void Cluster3D::move_origin(const Vector3 &p_offset) {
	if (p_offset.is_zero_approx()) {
		return;
	}
	for (Part &part : parts) {
		part.transform.origin -= p_offset;
	}
	Transform3D xform = get_transform();
	xform.origin += xform.basis.xform(p_offset);
	// Puts the parts back where they were in the world, when in it.
	set_transform(xform);
	if (is_inside_world()) {
		_update_all_part_transforms(_get_render_transform());
	}
	if (get_custom_aabb().has_volume()) {
		set_custom_aabb(AABB(get_custom_aabb().position - p_offset, get_custom_aabb().size));
	}
#ifndef PHYSICS_3D_DISABLED
	_update_collision_shapes();
#endif // PHYSICS_3D_DISABLED
	_geometry_changed();
	notify_property_list_changed();
}

// Baking into one mesh.

namespace {

struct BakedSurface {
	Ref<Material> material;
	uint64_t format = 0;
	PackedVector3Array vertices;
	PackedVector3Array normals;
	PackedFloat32Array tangents;
	PackedColorArray colors;
	PackedVector2Array uvs;
	PackedVector2Array uv2s;
	PackedInt32Array indices;
};

struct BakedPiece {
	int surface = 0;
	Array arrays;
	Transform3D transform;
};

} // namespace

Ref<ArrayMesh> Cluster3D::bake_mesh(bool p_generate_lods) const {
	const uint64_t attributes = Mesh::ARRAY_FORMAT_NORMAL | Mesh::ARRAY_FORMAT_TANGENT | Mesh::ARRAY_FORMAT_COLOR | Mesh::ARRAY_FORMAT_TEX_UV | Mesh::ARRAY_FORMAT_TEX_UV2;

	// One surface per material, and what goes into each.
	LocalVector<BakedSurface> surfaces;
	HashMap<ObjectID, int> surface_by_material;
	int surface_without_material = -1;
	LocalVector<BakedPiece> pieces;

	for (uint32_t i = 0; i < parts.size(); i++) {
		const Ref<Mesh> &mesh = parts[i].mesh;
		if (mesh.is_null()) {
			continue;
		}
		for (int s = 0; s < mesh->get_surface_count(); s++) {
			if (mesh->surface_get_primitive_type(s) != Mesh::PRIMITIVE_TRIANGLES) {
				continue;
			}
			const Ref<Material> material = get_part_active_material(i, s);
			int surface = -1;
			if (material.is_valid()) {
				int *existing = surface_by_material.getptr(material->get_instance_id());
				if (existing) {
					surface = *existing;
				} else {
					surface = surfaces.size();
					surface_by_material[material->get_instance_id()] = surface;
					surfaces.push_back(BakedSurface());
					surfaces[surface].material = material;
				}
			} else {
				if (surface_without_material < 0) {
					surface_without_material = surfaces.size();
					surfaces.push_back(BakedSurface());
				}
				surface = surface_without_material;
			}
			surfaces[surface].format |= mesh->surface_get_format(s) & attributes;

			BakedPiece piece;
			piece.surface = surface;
			piece.arrays = mesh->surface_get_arrays(s);
			piece.transform = parts[i].transform;
			pieces.push_back(piece);
		}
	}

	ERR_FAIL_COND_V_MSG(surfaces.is_empty(), Ref<ArrayMesh>(), "Cluster3D has no triangles to bake.");

	for (const BakedPiece &piece : pieces) {
		BakedSurface &surface = surfaces[piece.surface];
		const PackedVector3Array vertices = piece.arrays[Mesh::ARRAY_VERTEX];
		const PackedVector3Array normals = piece.arrays[Mesh::ARRAY_NORMAL];
		const PackedFloat32Array tangents = piece.arrays[Mesh::ARRAY_TANGENT];
		const PackedColorArray colors = piece.arrays[Mesh::ARRAY_COLOR];
		const PackedVector2Array uvs = piece.arrays[Mesh::ARRAY_TEX_UV];
		const PackedVector2Array uv2s = piece.arrays[Mesh::ARRAY_TEX_UV2];
		PackedInt32Array indices = piece.arrays[Mesh::ARRAY_INDEX];

		const int vertex_count = vertices.size();
		const int first_vertex = surface.vertices.size();
		if (indices.is_empty()) {
			indices.resize(vertex_count);
			for (int v = 0; v < vertex_count; v++) {
				indices.set(v, v);
			}
		}

		const Basis normal_basis = piece.transform.basis.inverse().transposed();
		// A mirroring transform turns faces inside out, unless their winding
		// and the tangents' handedness turn with them.
		const bool mirrored = piece.transform.basis.determinant() < 0;

		for (int v = 0; v < vertex_count; v++) {
			surface.vertices.push_back(piece.transform.xform(vertices[v]));
			if (surface.format & Mesh::ARRAY_FORMAT_NORMAL) {
				surface.normals.push_back(normals.size() == vertex_count ? normal_basis.xform(normals[v]).normalized() : Vector3(0, 1, 0));
			}
			if (surface.format & Mesh::ARRAY_FORMAT_TANGENT) {
				if (tangents.size() == vertex_count * 4) {
					const Vector3 tangent = piece.transform.basis.xform(Vector3(tangents[v * 4 + 0], tangents[v * 4 + 1], tangents[v * 4 + 2])).normalized();
					surface.tangents.push_back(tangent.x);
					surface.tangents.push_back(tangent.y);
					surface.tangents.push_back(tangent.z);
					surface.tangents.push_back(mirrored ? -tangents[v * 4 + 3] : tangents[v * 4 + 3]);
				} else {
					surface.tangents.push_back(1);
					surface.tangents.push_back(0);
					surface.tangents.push_back(0);
					surface.tangents.push_back(1);
				}
			}
			if (surface.format & Mesh::ARRAY_FORMAT_COLOR) {
				surface.colors.push_back(colors.size() == vertex_count ? colors[v] : Color(1, 1, 1, 1));
			}
			if (surface.format & Mesh::ARRAY_FORMAT_TEX_UV) {
				surface.uvs.push_back(uvs.size() == vertex_count ? uvs[v] : Vector2());
			}
			if (surface.format & Mesh::ARRAY_FORMAT_TEX_UV2) {
				surface.uv2s.push_back(uv2s.size() == vertex_count ? uv2s[v] : Vector2());
			}
		}

		for (int t = 0; t + 2 < indices.size(); t += 3) {
			surface.indices.push_back(first_vertex + indices[t + 0]);
			surface.indices.push_back(first_vertex + indices[mirrored ? t + 2 : t + 1]);
			surface.indices.push_back(first_vertex + indices[mirrored ? t + 1 : t + 2]);
		}
	}

	Ref<ImporterMesh> importer_mesh;
	importer_mesh.instantiate();
	for (const BakedSurface &surface : surfaces) {
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = surface.vertices;
		if (surface.format & Mesh::ARRAY_FORMAT_NORMAL) {
			arrays[Mesh::ARRAY_NORMAL] = surface.normals;
		}
		if (surface.format & Mesh::ARRAY_FORMAT_TANGENT) {
			arrays[Mesh::ARRAY_TANGENT] = surface.tangents;
		}
		if (surface.format & Mesh::ARRAY_FORMAT_COLOR) {
			arrays[Mesh::ARRAY_COLOR] = surface.colors;
		}
		if (surface.format & Mesh::ARRAY_FORMAT_TEX_UV) {
			arrays[Mesh::ARRAY_TEX_UV] = surface.uvs;
		}
		if (surface.format & Mesh::ARRAY_FORMAT_TEX_UV2) {
			arrays[Mesh::ARRAY_TEX_UV2] = surface.uv2s;
		}
		arrays[Mesh::ARRAY_INDEX] = surface.indices;
		const String name = surface.material.is_valid() ? surface.material->get_name() : String();
		importer_mesh->add_surface(Mesh::PRIMITIVE_TRIANGLES, arrays, TypedArray<Array>(), Dictionary(), surface.material, name);
	}

	if (p_generate_lods) {
		// The scene importer's default.
		importer_mesh->generate_lods(60.0f, Array());
	}
	importer_mesh->create_shadow_mesh();
	return importer_mesh->get_mesh();
}

// Bakers.

bool Cluster3D::_is_mesh_lightmap_ready(const Ref<Mesh> &p_mesh) {
	if (p_mesh.is_null()) {
		return false;
	}
	bool found = false;
	for (int i = 0; i < p_mesh->get_surface_count(); i++) {
		if (p_mesh->surface_get_primitive_type(i) != Mesh::PRIMITIVE_TRIANGLES) {
			continue;
		}
		const uint64_t format = p_mesh->surface_get_format(i);
		if (!(format & Mesh::ARRAY_FORMAT_TEX_UV2) || !(format & Mesh::ARRAY_FORMAT_NORMAL)) {
			return false;
		}
		found = true;
	}
	return found;
}

Array Cluster3D::get_bake_meshes() const {
	Array meshes;
	if (get_gi_mode() != GI_MODE_STATIC) {
		return meshes;
	}
	for (const Part &part : parts) {
		if (_is_mesh_lightmap_ready(part.mesh)) {
			meshes.push_back(part.mesh);
			meshes.push_back(part.transform);
		}
	}
	return meshes;
}

RID Cluster3D::get_bake_mesh_instance(int p_index) const {
	// The same parts get_bake_meshes() gives, in the same order.
	int index = 0;
	for (const Part &part : parts) {
		if (_is_mesh_lightmap_ready(part.mesh)) {
			if (index == p_index) {
				return part.instance;
			}
			index++;
		}
	}
	return RID();
}

Array Cluster3D::get_meshes() const {
	Array meshes;
	for (const Part &part : parts) {
		if (part.mesh.is_valid()) {
			meshes.push_back(part.transform);
			meshes.push_back(part.mesh);
		}
	}
	return meshes;
}

AABB Cluster3D::get_aabb() const {
	if (aabb_dirty) {
		aabb = AABB();
		bool first = true;
		for (uint32_t i = 0; i < parts.size(); i++) {
			const AABB part_aabb = get_part_aabb(i);
			if (first) {
				aabb = part_aabb;
				first = false;
			} else {
				aabb.merge_with(part_aabb);
			}
		}
		aabb_dirty = false;
	}
	return aabb;
}

Ref<TriangleMesh> Cluster3D::generate_triangle_mesh() const {
	if (triangle_mesh.is_valid()) {
		return triangle_mesh;
	}
	Vector<Vector3> faces;
	for (const Part &part : parts) {
		if (part.mesh.is_null()) {
			continue;
		}
		const Vector<Face3> part_faces = part.mesh->get_faces();
		const int first_vertex = faces.size();
		faces.resize(first_vertex + part_faces.size() * 3);
		Vector3 *w = faces.ptrw();
		for (int i = 0; i < part_faces.size(); i++) {
			for (int j = 0; j < 3; j++) {
				w[first_vertex + i * 3 + j] = part.transform.xform(part_faces[i].vertex[j]);
			}
		}
	}
	if (faces.is_empty()) {
		return Ref<TriangleMesh>();
	}
	triangle_mesh.instantiate();
	triangle_mesh->create(faces);
	return triangle_mesh;
}

// Collision.

#ifndef PHYSICS_3D_DISABLED
// Every cluster using a mesh shares the shape made from it, so that a level
// full of the same few rocks keeps one copy of each in the physics server.
// The cache does not keep the shapes alive: the clusters using them do.
static Mutex collision_shape_cache_mutex;
static HashMap<ObjectID, ObjectID> collision_shape_cache[2];

Ref<Shape3D> Cluster3D::_get_collision_shape(const Ref<Mesh> &p_mesh, CollisionShapeType p_type) {
	if (p_mesh.is_null()) {
		return Ref<Shape3D>();
	}
	MutexLock lock(collision_shape_cache_mutex);
	HashMap<ObjectID, ObjectID> &cache = collision_shape_cache[p_type];
	const ObjectID mesh_id = p_mesh->get_instance_id();
	const ObjectID *cached = cache.getptr(mesh_id);
	if (cached) {
		// Null if the last cluster using it let it go meanwhile.
		Ref<Shape3D> shape = Ref<Shape3D>(ObjectDB::get_instance<Shape3D>(*cached));
		if (shape.is_valid()) {
			return shape;
		}
	}
	Ref<Shape3D> shape;
	if (p_type == COLLISION_SHAPE_CONVEX) {
		shape = p_mesh->create_convex_shape(true, false);
	} else {
		shape = p_mesh->create_trimesh_shape();
	}
	if (shape.is_valid()) {
		cache[mesh_id] = shape->get_instance_id();
	} else {
		cache.erase(mesh_id);
	}
	return shape;
}

void Cluster3D::_forget_collision_shapes(ObjectID p_mesh) {
	MutexLock lock(collision_shape_cache_mutex);
	for (HashMap<ObjectID, ObjectID> &cache : collision_shape_cache) {
		cache.erase(p_mesh);
	}
}

void Cluster3D::_create_collision_body() {
	if (collision_body.is_valid() || !use_collision || !is_inside_world()) {
		return;
	}
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	collision_body = ps->body_create();
	ps->body_set_mode(collision_body, PS3DE::BODY_MODE_STATIC);
	ps->body_set_state(collision_body, PS3DE::BODY_STATE_TRANSFORM, get_global_transform());
	ps->body_set_space(collision_body, get_world_3d()->get_space());
	ps->body_attach_object_instance_id(collision_body, get_instance_id());
	ps->body_set_collision_layer(collision_body, collision_layer);
	ps->body_set_collision_mask(collision_body, collision_mask);
	ps->body_set_collision_priority(collision_body, collision_priority);
	_update_collision_shapes();
}

void Cluster3D::_free_collision_body() {
	if (collision_body.is_valid()) {
		PhysicsServer3D::get_singleton()->free_rid(collision_body);
		collision_body = RID();
	}
	collision_shapes.clear();
	collision_shape_indices.clear();
}

void Cluster3D::_update_collision_shapes() {
	if (collision_body.is_null()) {
		return;
	}
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	ps->body_clear_shapes(collision_body);
	collision_shapes.clear();
	collision_shape_indices.resize(parts.size());
	for (uint32_t i = 0; i < parts.size(); i++) {
		const Ref<Shape3D> shape = _get_collision_shape(parts[i].mesh, collision_shape_type);
		if (shape.is_null()) {
			collision_shape_indices[i] = -1;
			continue;
		}
		collision_shape_indices[i] = collision_shapes.size();
		ps->body_add_shape(collision_body, shape->get_rid(), parts[i].transform);
		collision_shapes.push_back(shape);
	}
}

void Cluster3D::_update_collision_shape_transform(int p_part) {
	if (collision_body.is_null() || p_part >= (int)collision_shape_indices.size() || collision_shape_indices[p_part] < 0) {
		return;
	}
	PhysicsServer3D::get_singleton()->body_set_shape_transform(collision_body, collision_shape_indices[p_part], parts[p_part].transform);
}

void Cluster3D::set_use_collision(bool p_enable) {
	if (use_collision == p_enable) {
		return;
	}
	use_collision = p_enable;
	if (use_collision) {
		_create_collision_body();
	} else {
		_free_collision_body();
	}
	notify_property_list_changed();
}

bool Cluster3D::is_using_collision() const {
	return use_collision;
}

void Cluster3D::set_collision_layer(uint32_t p_layer) {
	collision_layer = p_layer;
	if (collision_body.is_valid()) {
		PhysicsServer3D::get_singleton()->body_set_collision_layer(collision_body, collision_layer);
	}
}

uint32_t Cluster3D::get_collision_layer() const {
	return collision_layer;
}

void Cluster3D::set_collision_mask(uint32_t p_mask) {
	collision_mask = p_mask;
	if (collision_body.is_valid()) {
		PhysicsServer3D::get_singleton()->body_set_collision_mask(collision_body, collision_mask);
	}
}

uint32_t Cluster3D::get_collision_mask() const {
	return collision_mask;
}

void Cluster3D::set_collision_layer_value(int p_layer_number, bool p_value) {
	ERR_FAIL_COND_MSG(p_layer_number < 1, "Collision layer number must be between 1 and 32 inclusive.");
	ERR_FAIL_COND_MSG(p_layer_number > 32, "Collision layer number must be between 1 and 32 inclusive.");
	uint32_t layer = get_collision_layer();
	if (p_value) {
		layer |= 1 << (p_layer_number - 1);
	} else {
		layer &= ~(1 << (p_layer_number - 1));
	}
	set_collision_layer(layer);
}

bool Cluster3D::get_collision_layer_value(int p_layer_number) const {
	ERR_FAIL_COND_V_MSG(p_layer_number < 1, false, "Collision layer number must be between 1 and 32 inclusive.");
	ERR_FAIL_COND_V_MSG(p_layer_number > 32, false, "Collision layer number must be between 1 and 32 inclusive.");
	return get_collision_layer() & (1 << (p_layer_number - 1));
}

void Cluster3D::set_collision_mask_value(int p_layer_number, bool p_value) {
	ERR_FAIL_COND_MSG(p_layer_number < 1, "Collision layer number must be between 1 and 32 inclusive.");
	ERR_FAIL_COND_MSG(p_layer_number > 32, "Collision layer number must be between 1 and 32 inclusive.");
	uint32_t mask = get_collision_mask();
	if (p_value) {
		mask |= 1 << (p_layer_number - 1);
	} else {
		mask &= ~(1 << (p_layer_number - 1));
	}
	set_collision_mask(mask);
}

bool Cluster3D::get_collision_mask_value(int p_layer_number) const {
	ERR_FAIL_COND_V_MSG(p_layer_number < 1, false, "Collision layer number must be between 1 and 32 inclusive.");
	ERR_FAIL_COND_V_MSG(p_layer_number > 32, false, "Collision layer number must be between 1 and 32 inclusive.");
	return get_collision_mask() & (1 << (p_layer_number - 1));
}

void Cluster3D::set_collision_priority(real_t p_priority) {
	collision_priority = p_priority;
	if (collision_body.is_valid()) {
		PhysicsServer3D::get_singleton()->body_set_collision_priority(collision_body, collision_priority);
	}
}

real_t Cluster3D::get_collision_priority() const {
	return collision_priority;
}

void Cluster3D::set_collision_shape_type(CollisionShapeType p_type) {
	ERR_FAIL_INDEX(p_type, 2);
	if (collision_shape_type == p_type) {
		return;
	}
	collision_shape_type = p_type;
	_update_collision_shapes();
}

Cluster3D::CollisionShapeType Cluster3D::get_collision_shape_type() const {
	return collision_shape_type;
}
#endif // PHYSICS_3D_DISABLED

// Navigation.

#ifndef NAVIGATION_3D_DISABLED
void Cluster3D::navmesh_parse_init() {
	ERR_FAIL_NULL(NavigationServer3D::get_singleton());
	if (!_navmesh_source_geometry_parser.is_valid()) {
		_navmesh_source_geometry_parsing_callback = callable_mp_static(&Cluster3D::navmesh_parse_source_geometry);
		_navmesh_source_geometry_parser = NavigationServer3D::get_singleton()->source_geometry_parser_create();
		NavigationServer3D::get_singleton()->source_geometry_parser_set_callback(_navmesh_source_geometry_parser, _navmesh_source_geometry_parsing_callback);
	}
}

void Cluster3D::navmesh_parse_source_geometry(const Ref<NavigationMesh> &p_navigation_mesh, Ref<NavigationMeshSourceGeometryData3D> p_source_geometry_data, Node *p_node) {
	Cluster3D *cluster = Object::cast_to<Cluster3D>(p_node);
	if (cluster == nullptr) {
		return;
	}

	const NavigationMesh::ParsedGeometryType parsed_geometry_type = p_navigation_mesh->get_parsed_geometry_type();
#ifndef PHYSICS_3D_DISABLED
	const bool nav_collision = parsed_geometry_type == NavigationMesh::PARSED_GEOMETRY_STATIC_COLLIDERS && cluster->is_using_collision() && (cluster->get_collision_layer() & p_navigation_mesh->get_collision_mask());
#else
	const bool nav_collision = false;
#endif // PHYSICS_3D_DISABLED
	if (parsed_geometry_type == NavigationMesh::PARSED_GEOMETRY_MESH_INSTANCES || parsed_geometry_type == NavigationMesh::PARSED_GEOMETRY_BOTH || nav_collision) {
		const Transform3D global_transform = cluster->get_global_transform();
		for (const Part &part : cluster->parts) {
			if (part.mesh.is_valid()) {
				p_source_geometry_data->add_mesh(part.mesh, global_transform * part.transform);
			}
		}
	}
}
#endif // NAVIGATION_3D_DISABLED

// Node.

void Cluster3D::_instance_settings_changed() {
	const HashMap<StringName, Variant> &shader_parameters = _get_instance_shader_parameters();
	RenderingServer *rs = RS::get_singleton();
	for (const StringName &name : part_shader_parameters) {
		if (shader_parameters.has(name)) {
			continue;
		}
		for (const Part &part : parts) {
			rs->instance_geometry_set_shader_parameter(part.instance, name, rs->instance_geometry_get_shader_parameter_default_value(part.instance, name));
		}
	}
	part_shader_parameters.clear();
	for (const KeyValue<StringName, Variant> &E : shader_parameters) {
		part_shader_parameters.insert(E.key);
	}

	for (const Part &part : parts) {
		_part_update_settings(part);
	}
}

void Cluster3D::fti_update_servers_xform() {
	GeometryInstance3D::fti_update_servers_xform();
	if (_is_vi_visible()) {
		_update_all_part_transforms(_get_cached_global_transform_interpolated());
	}
}

void Cluster3D::add_child_notify(Node *p_child) {
	GeometryInstance3D::add_child_notify(p_child);
	if (Engine::get_singleton()->is_editor_hint()) {
		update_configuration_warnings();
	}
}

void Cluster3D::remove_child_notify(Node *p_child) {
	GeometryInstance3D::remove_child_notify(p_child);
	if (Engine::get_singleton()->is_editor_hint()) {
		update_configuration_warnings();
	}
}

void Cluster3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_WORLD: {
			const RID scenario = get_world_3d()->get_scenario();
			RenderingServer *rs = RS::get_singleton();
			for (const Part &part : parts) {
				rs->instance_set_scenario(part.instance, scenario);
			}
			_update_parts_visibility();
#ifndef PHYSICS_3D_DISABLED
			_create_collision_body();
#endif // PHYSICS_3D_DISABLED
		} break;

		case NOTIFICATION_EXIT_WORLD: {
			RenderingServer *rs = RS::get_singleton();
			for (const Part &part : parts) {
				rs->instance_set_scenario(part.instance, RID());
				rs->instance_set_visible(part.instance, false);
			}
#ifndef PHYSICS_3D_DISABLED
			_free_collision_body();
#endif // PHYSICS_3D_DISABLED
		} break;

		case NOTIFICATION_VISIBILITY_CHANGED: {
			_update_parts_visibility();
		} break;

		case NOTIFICATION_TRANSFORM_CHANGED: {
			// As VisualInstance3D does for its own instance: with physics
			// interpolation on, fti_update_servers_xform() does it instead.
			if (_is_vi_visible() && !(is_inside_tree() && get_tree()->is_physics_interpolation_enabled())) {
				_update_all_part_transforms(get_global_transform());
			}
#ifndef PHYSICS_3D_DISABLED
			if (collision_body.is_valid()) {
				PhysicsServer3D::get_singleton()->body_set_state(collision_body, PS3DE::BODY_STATE_TRANSFORM, get_global_transform());
			}
#endif // PHYSICS_3D_DISABLED
		} break;

		case NOTIFICATION_RESET_PHYSICS_INTERPOLATION: {
			if (_is_vi_visible() && is_inside_tree()) {
				RenderingServer *rs = RS::get_singleton();
				for (const Part &part : parts) {
					rs->instance_teleport(part.instance);
				}
			}
		} break;
	}
}

PackedStringArray Cluster3D::get_configuration_warnings() const {
	PackedStringArray warnings = GeometryInstance3D::get_configuration_warnings();

	if (parts.is_empty()) {
		warnings.push_back(RTR("This Cluster3D has no parts yet. With it selected, drag meshes or scenes from the FileSystem dock into the 3D viewport, use the Cluster menu's Add Mesh, or put MeshInstance3D nodes under it and use the Cluster menu's Pack Child Meshes."));
	} else {
		int without_mesh = 0;
		for (const Part &part : parts) {
			if (part.mesh.is_null()) {
				without_mesh++;
			}
		}
		if (without_mesh > 0) {
			warnings.push_back(vformat(RTR("%d of this Cluster3D's parts have no mesh, and draw nothing."), without_mesh));
		}
	}

	int mesh_children = 0;
	for (int i = 0; i < get_child_count(false); i++) {
		if (Object::cast_to<MeshInstance3D>(get_child(i, false))) {
			mesh_children++;
		}
	}
	if (mesh_children > 0) {
		warnings.push_back(vformat(RTR("%d MeshInstance3D children are not parts of this Cluster3D, only nodes under it. Use the Cluster menu's Pack Child Meshes in the 3D editor to make them parts."), mesh_children));
	}

	return warnings;
}

void Cluster3D::_validate_property(PropertyInfo &p_property) const {
#ifndef PHYSICS_3D_DISABLED
	if (!use_collision && (p_property.name == "collision_layer" || p_property.name == "collision_mask" || p_property.name == "collision_priority" || p_property.name == "collision_shape_type")) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
#endif // PHYSICS_3D_DISABLED
}

void Cluster3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_part_count", "count"), &Cluster3D::set_part_count);
	ClassDB::bind_method(D_METHOD("get_part_count"), &Cluster3D::get_part_count);
	ClassDB::bind_method(D_METHOD("add_part", "mesh", "transform", "materials"), &Cluster3D::add_part, DEFVAL(Transform3D()), DEFVAL(TypedArray<Material>()));
	ClassDB::bind_method(D_METHOD("remove_part", "part"), &Cluster3D::remove_part);
	ClassDB::bind_method(D_METHOD("clear_parts"), &Cluster3D::clear_parts);

	ClassDB::bind_method(D_METHOD("set_part_mesh", "part", "mesh"), &Cluster3D::set_part_mesh);
	ClassDB::bind_method(D_METHOD("get_part_mesh", "part"), &Cluster3D::get_part_mesh);
	ClassDB::bind_method(D_METHOD("set_part_transform", "part", "transform"), &Cluster3D::set_part_transform);
	ClassDB::bind_method(D_METHOD("get_part_transform", "part"), &Cluster3D::get_part_transform);
	ClassDB::bind_method(D_METHOD("set_part_materials", "part", "materials"), &Cluster3D::set_part_materials);
	ClassDB::bind_method(D_METHOD("get_part_materials", "part"), &Cluster3D::get_part_materials);
	ClassDB::bind_method(D_METHOD("set_part_surface_material", "part", "surface", "material"), &Cluster3D::set_part_surface_material);
	ClassDB::bind_method(D_METHOD("get_part_surface_material", "part", "surface"), &Cluster3D::get_part_surface_material);
	ClassDB::bind_method(D_METHOD("get_part_active_material", "part", "surface"), &Cluster3D::get_part_active_material);
	ClassDB::bind_method(D_METHOD("get_part_aabb", "part"), &Cluster3D::get_part_aabb);
	ClassDB::bind_method(D_METHOD("get_part_instance", "part"), &Cluster3D::get_part_instance);

	ClassDB::bind_method(D_METHOD("add_parts_from_node", "node", "transform"), &Cluster3D::add_parts_from_node, DEFVAL(Transform3D()));
	ClassDB::bind_method(D_METHOD("move_origin", "offset"), &Cluster3D::move_origin);
	ClassDB::bind_method(D_METHOD("bake_mesh", "generate_lods"), &Cluster3D::bake_mesh, DEFVAL(true));

	ClassDB::bind_method(D_METHOD("get_bake_meshes"), &Cluster3D::get_bake_meshes);
	ClassDB::bind_method(D_METHOD("get_bake_mesh_instance", "index"), &Cluster3D::get_bake_mesh_instance);
	ClassDB::bind_method(D_METHOD("get_meshes"), &Cluster3D::get_meshes);

	// For undo and redo in the editor.
	ClassDB::bind_method(D_METHOD("_get_parts_snapshot"), &Cluster3D::_get_parts_snapshot);
	ClassDB::bind_method(D_METHOD("_set_parts_snapshot", "snapshot"), &Cluster3D::_set_parts_snapshot);

	ADD_ARRAY_COUNT("Parts", "part_count", "set_part_count", "get_part_count", "part_");

#ifndef PHYSICS_3D_DISABLED
	ClassDB::bind_method(D_METHOD("set_use_collision", "enable"), &Cluster3D::set_use_collision);
	ClassDB::bind_method(D_METHOD("is_using_collision"), &Cluster3D::is_using_collision);
	ClassDB::bind_method(D_METHOD("set_collision_layer", "layer"), &Cluster3D::set_collision_layer);
	ClassDB::bind_method(D_METHOD("get_collision_layer"), &Cluster3D::get_collision_layer);
	ClassDB::bind_method(D_METHOD("set_collision_mask", "mask"), &Cluster3D::set_collision_mask);
	ClassDB::bind_method(D_METHOD("get_collision_mask"), &Cluster3D::get_collision_mask);
	ClassDB::bind_method(D_METHOD("set_collision_layer_value", "layer_number", "value"), &Cluster3D::set_collision_layer_value);
	ClassDB::bind_method(D_METHOD("get_collision_layer_value", "layer_number"), &Cluster3D::get_collision_layer_value);
	ClassDB::bind_method(D_METHOD("set_collision_mask_value", "layer_number", "value"), &Cluster3D::set_collision_mask_value);
	ClassDB::bind_method(D_METHOD("get_collision_mask_value", "layer_number"), &Cluster3D::get_collision_mask_value);
	ClassDB::bind_method(D_METHOD("set_collision_priority", "priority"), &Cluster3D::set_collision_priority);
	ClassDB::bind_method(D_METHOD("get_collision_priority"), &Cluster3D::get_collision_priority);
	ClassDB::bind_method(D_METHOD("set_collision_shape_type", "type"), &Cluster3D::set_collision_shape_type);
	ClassDB::bind_method(D_METHOD("get_collision_shape_type"), &Cluster3D::get_collision_shape_type);
	ClassDB::bind_method(D_METHOD("get_collision_rid"), &Cluster3D::get_collision_rid);

	ADD_GROUP("Collision", "collision_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_collision"), "set_use_collision", "is_using_collision");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_shape_type", PROPERTY_HINT_ENUM, "Trimesh,Convex"), "set_collision_shape_type", "get_collision_shape_type");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_layer", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_layer", "get_collision_layer");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_mask", "get_collision_mask");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "collision_priority"), "set_collision_priority", "get_collision_priority");
	ADD_GROUP("", "");

	BIND_ENUM_CONSTANT(COLLISION_SHAPE_TRIMESH);
	BIND_ENUM_CONSTANT(COLLISION_SHAPE_CONVEX);
#endif // PHYSICS_3D_DISABLED

	base_property_helper.set_prefix("part_");
	base_property_helper.set_array_length_getter(&Cluster3D::get_part_count);
	base_property_helper.register_property(PropertyInfo(Variant::OBJECT, "mesh", PROPERTY_HINT_RESOURCE_TYPE, Mesh::get_class_static()), Ref<Mesh>(), &Cluster3D::set_part_mesh, &Cluster3D::get_part_mesh);
	base_property_helper.register_property(PropertyInfo(Variant::TRANSFORM3D, "transform", PROPERTY_HINT_NONE, "suffix:m"), Transform3D(), &Cluster3D::set_part_transform, &Cluster3D::get_part_transform);
	base_property_helper.register_property(PropertyInfo(Variant::ARRAY, "materials", PROPERTY_HINT_ARRAY_TYPE, MAKE_RESOURCE_TYPE_HINT("BaseMaterial3D,ShaderMaterial")), TypedArray<Material>(), &Cluster3D::set_part_materials, &Cluster3D::get_part_materials);
	PropertyListHelper::register_base_helper(get_class_static(), &base_property_helper);
}

Cluster3D::Cluster3D() {
	property_helper.setup_for_instance(base_property_helper, this);
}

Cluster3D::~Cluster3D() {
	for (Part &part : parts) {
		_mesh_remove_user(part.mesh);
		_part_free_instance(part);
	}
	parts.clear();
#ifndef PHYSICS_3D_DISABLED
	_free_collision_body();
#endif // PHYSICS_3D_DISABLED
}
