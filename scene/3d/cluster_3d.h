/**************************************************************************/
/*  cluster_3d.h                                                          */
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

#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/variant/typed_array.h"
#include "scene/3d/visual_instance_3d.h"
#include "scene/property_list_helper.h"
#include "scene/resources/mesh.h"

#ifndef NAVIGATION_3D_DISABLED
class NavigationMesh;
class NavigationMeshSourceGeometryData3D;
#endif // NAVIGATION_3D_DISABLED
#ifndef PHYSICS_3D_DISABLED
class Shape3D;
#endif // PHYSICS_3D_DISABLED

// A level piece put together from meshes: a rock out of a few stones, a
// group of trees, a ruin out of wall pieces. Each part is a mesh with its own
// transform (and, optionally, its own surface materials), relative to the
// cluster, and the cluster is one node: it is selected, moved, duplicated and
// saved as one, and its origin is wherever its pivot makes sense instead of
// wherever the meshes happened to be modeled, unlike a scene the parts would
// otherwise be assembled into.
//
// Parts are drawn with one RenderingServer instance each, on the meshes they
// use, so a cluster costs no more memory than its meshes, keeps their LODs and
// shadow meshes, and its parts are culled and LODed one by one. Everything the
// cluster inherits from GeometryInstance3D (shadows, GI mode, visibility
// range, material override, layers...) applies to all of them: VisualInstance3D
// calls _instance_settings_changed() whenever one of those changes, and the
// cluster copies them all onto its parts' instances.
//
// In the editor, Cluster3DEditorPlugin and Cluster3DGizmoPlugin make the parts
// subgizmos of the cluster: they are picked, moved, rotated, scaled,
// duplicated and deleted right in the viewport.
class Cluster3D : public GeometryInstance3D {
	GDCLASS(Cluster3D, GeometryInstance3D);

public:
	enum CollisionShapeType {
		COLLISION_SHAPE_TRIMESH,
		COLLISION_SHAPE_CONVEX,
	};

private:
	struct Part {
		Ref<Mesh> mesh;
		Transform3D transform;
		// Per surface; a null entry, or no entry, leaves the mesh's own.
		TypedArray<Material> materials;
		RID instance;
	};

	LocalVector<Part> parts;

	// The meshes parts use, with how many use each, so that the cluster hears
	// once of each one changing.
	HashMap<ObjectID, int> mesh_users;

	// The instance shader parameters set on the parts' instances, to put
	// back to their defaults the ones the cluster no longer sets.
	HashSet<StringName> part_shader_parameters;

	mutable AABB aabb;
	mutable bool aabb_dirty = true;
	mutable Ref<TriangleMesh> triangle_mesh;

	static inline PropertyListHelper base_property_helper;
	PropertyListHelper property_helper;

#ifndef PHYSICS_3D_DISABLED
	bool use_collision = false;
	uint32_t collision_layer = 1;
	uint32_t collision_mask = 1;
	real_t collision_priority = 1.0;
	CollisionShapeType collision_shape_type = COLLISION_SHAPE_TRIMESH;

	RID collision_body;
	// Index of each part's shape in collision_body, -1 for none.
	LocalVector<int> collision_shape_indices;
	// Keeps the shapes in collision_body alive; they are shared with every
	// other cluster that uses the same meshes (see _get_collision_shape()).
	LocalVector<Ref<Shape3D>> collision_shapes;

	static Ref<Shape3D> _get_collision_shape(const Ref<Mesh> &p_mesh, CollisionShapeType p_type);
	static void _forget_collision_shapes(ObjectID p_mesh);

	void _create_collision_body();
	void _free_collision_body();
	void _update_collision_shapes();
	void _update_collision_shape_transform(int p_part);
#endif // PHYSICS_3D_DISABLED

#ifndef NAVIGATION_3D_DISABLED
	static Callable _navmesh_source_geometry_parsing_callback;
	static RID _navmesh_source_geometry_parser;
#endif // NAVIGATION_3D_DISABLED

	Transform3D _get_render_transform() const;
	void _part_create_instance(Part &r_part);
	void _part_free_instance(Part &r_part);
	void _part_update_base(Part &r_part);
	void _part_update_materials(const Part &p_part);
	void _part_update_settings(const Part &p_part);
	void _part_update_transform(const Part &p_part, const Transform3D &p_render_transform);
	void _part_set_mesh(Part &r_part, const Ref<Mesh> &p_mesh);
	void _update_all_part_transforms(const Transform3D &p_render_transform);
	void _update_parts_visibility();
	void _geometry_changed();
	void _structure_changed();

	void _mesh_add_user(const Ref<Mesh> &p_mesh);
	void _mesh_remove_user(const Ref<Mesh> &p_mesh);
	void _mesh_changed();

	// Whether a part's mesh can go to LightmapGI as it is: every triangle
	// surface has normals and a UV2.
	static bool _is_mesh_lightmap_ready(const Ref<Mesh> &p_mesh);

	Array _get_parts_snapshot() const;
	void _set_parts_snapshot(const Array &p_snapshot);

protected:
	void _notification(int p_what);
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;

	bool _set(const StringName &p_name, const Variant &p_value) { return property_helper.property_set_value(p_name, p_value); }
	bool _get(const StringName &p_name, Variant &r_ret) const { return property_helper.property_get_value(p_name, r_ret); }
	void _get_property_list(List<PropertyInfo> *p_list) const { property_helper.get_property_list(p_list); }
	bool _property_can_revert(const StringName &p_name) const { return property_helper.property_can_revert(p_name); }
	bool _property_get_revert(const StringName &p_name, Variant &r_property) const { return property_helper.property_get_revert(p_name, r_property); }

	virtual void _instance_settings_changed() override;
	virtual void fti_update_servers_xform() override;
	virtual void add_child_notify(Node *p_child) override;
	virtual void remove_child_notify(Node *p_child) override;

public:
	void set_part_count(int p_count);
	int get_part_count() const;

	int add_part(const Ref<Mesh> &p_mesh, const Transform3D &p_transform = Transform3D(), const TypedArray<Material> &p_materials = TypedArray<Material>());
	void remove_part(int p_part);
	void clear_parts();

	void set_part_mesh(int p_part, const Ref<Mesh> &p_mesh);
	Ref<Mesh> get_part_mesh(int p_part) const;

	void set_part_transform(int p_part, const Transform3D &p_transform);
	Transform3D get_part_transform(int p_part) const;

	void set_part_materials(int p_part, const TypedArray<Material> &p_materials);
	TypedArray<Material> get_part_materials(int p_part) const;

	void set_part_surface_material(int p_part, int p_surface, const Ref<Material> &p_material);
	Ref<Material> get_part_surface_material(int p_part, int p_surface) const;
	// What the part's surface is drawn with: material_override, else the
	// part's own surface material, else the mesh's.
	Ref<Material> get_part_active_material(int p_part, int p_surface) const;

	AABB get_part_aabb(int p_part) const;
	RID get_part_instance(int p_part) const;

	// Adds a part for every MeshInstance3D, Cluster3D part and MultiMesh
	// instance in p_node's branch, p_node included, placed where they are
	// relative to p_node and then moved by p_transform. Returns how many.
	int add_parts_from_node(Node *p_node, const Transform3D &p_transform = Transform3D());

	// Moves the cluster's origin by p_offset, in its own space, without
	// moving any of its parts in the world.
	void move_origin(const Vector3 &p_offset);

	// All the parts as one mesh: their triangle surfaces merged by material,
	// in the cluster's space, with LODs and a shadow mesh generated for it.
	Ref<ArrayMesh> bake_mesh(bool p_generate_lods = true) const;

	// [mesh, transform, ...], as LightmapGI, AmbientProbeVolume3D and other
	// bakers look for; only parts that can be lightmapped, when gi_mode is
	// static.
	Array get_bake_meshes() const;
	RID get_bake_mesh_instance(int p_index) const;
	// [transform, mesh, ...], as VoxelGI and GPUParticlesCollisionSDF3D look for.
	Array get_meshes() const;

#ifndef PHYSICS_3D_DISABLED
	void set_use_collision(bool p_enable);
	bool is_using_collision() const;

	void set_collision_layer(uint32_t p_layer);
	uint32_t get_collision_layer() const;

	void set_collision_mask(uint32_t p_mask);
	uint32_t get_collision_mask() const;

	void set_collision_layer_value(int p_layer_number, bool p_value);
	bool get_collision_layer_value(int p_layer_number) const;

	void set_collision_mask_value(int p_layer_number, bool p_value);
	bool get_collision_mask_value(int p_layer_number) const;

	void set_collision_priority(real_t p_priority);
	real_t get_collision_priority() const;

	void set_collision_shape_type(CollisionShapeType p_type);
	CollisionShapeType get_collision_shape_type() const;

	RID get_collision_rid() const { return collision_body; }
#endif // PHYSICS_3D_DISABLED

	virtual AABB get_aabb() const override;
	virtual Ref<TriangleMesh> generate_triangle_mesh() const override;

	PackedStringArray get_configuration_warnings() const override;

#ifndef NAVIGATION_3D_DISABLED
	static void navmesh_parse_init();
	static void navmesh_parse_source_geometry(const Ref<NavigationMesh> &p_navigation_mesh, Ref<NavigationMeshSourceGeometryData3D> p_source_geometry_data, Node *p_node);
#endif // NAVIGATION_3D_DISABLED

	Cluster3D();
	~Cluster3D();
};

VARIANT_ENUM_CAST(Cluster3D::CollisionShapeType);
