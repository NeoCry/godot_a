/**************************************************************************/
/*  cluster_3d_editor_plugin.cpp                                          */
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

#include "cluster_3d_editor_plugin.h"

#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/math/triangle_mesh.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/docks/scene_tree_dock.h"
#include "editor/editor_data.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/scene/3d/node_3d_editor_viewport.h"
#include "editor/settings/editor_settings.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/menu_button.h"
#include "scene/gui/separator.h"
#include "scene/resources/packed_scene.h"

// Gizmo.

Cluster3DGizmoPlugin::Cluster3DGizmoPlugin() {
	create_material("part", Color(0.55, 0.75, 1.0, 0.5));
	create_material("part_selected", Color(1.0, 0.55, 0.15), false, true);
}

bool Cluster3DGizmoPlugin::has_gizmo(Node3D *p_spatial) {
	return Object::cast_to<Cluster3D>(p_spatial) != nullptr;
}

String Cluster3DGizmoPlugin::get_gizmo_name() const {
	return "Cluster3D";
}

int Cluster3DGizmoPlugin::get_priority() const {
	return -1;
}

bool Cluster3DGizmoPlugin::can_be_hidden() const {
	// It is what the cluster is picked with in the viewport.
	return false;
}

static void _add_box_lines(Vector<Vector3> &r_lines, const AABB &p_aabb, const Transform3D &p_transform) {
	for (int i = 0; i < 12; i++) {
		Vector3 a, b;
		p_aabb.get_edge(i, a, b);
		r_lines.push_back(p_transform.xform(a));
		r_lines.push_back(p_transform.xform(b));
	}
}

void Cluster3DGizmoPlugin::redraw(EditorNode3DGizmo *p_gizmo) {
	Cluster3D *cluster = Object::cast_to<Cluster3D>(p_gizmo->get_node_3d());
	p_gizmo->clear();

	const int part_count = cluster->get_part_count();

	// What the cluster, and its parts, are picked with. Each mesh's
	// TriangleMesh is cached by the mesh, so a cluster that repeats a mesh,
	// or a level full of clusters made of the same few, builds it once.
	for (int i = 0; i < part_count; i++) {
		const Ref<Mesh> mesh = cluster->get_part_mesh(i);
		if (mesh.is_null()) {
			continue;
		}
		const Ref<TriangleMesh> triangle_mesh = mesh->generate_triangle_mesh();
		if (triangle_mesh.is_valid()) {
			p_gizmo->add_transformed_collision_triangles(triangle_mesh, cluster->get_part_transform(i));
		}
	}

	Cluster3DEditorPlugin *editor = Cluster3DEditorPlugin::get_singleton();
	if (!p_gizmo->is_selected() || !editor || !editor->is_editing_parts()) {
		return;
	}

	// The parts' bounds, the selected ones' in the selection color.
	Vector<Vector3> lines;
	Vector<Vector3> selected_lines;
	for (int i = 0; i < part_count; i++) {
		const Ref<Mesh> mesh = cluster->get_part_mesh(i);
		const AABB aabb = mesh.is_valid() ? mesh->get_aabb() : AABB(Vector3(-0.1, -0.1, -0.1), Vector3(0.2, 0.2, 0.2));
		_add_box_lines(p_gizmo->is_subgizmo_selected(i) ? selected_lines : lines, aabb, cluster->get_part_transform(i));
	}
	if (!lines.is_empty()) {
		p_gizmo->add_lines(lines, get_material("part", p_gizmo));
	}
	if (!selected_lines.is_empty()) {
		p_gizmo->add_lines(selected_lines, get_material("part_selected", p_gizmo));
	}
}

int Cluster3DGizmoPlugin::subgizmos_intersect_ray(const EditorNode3DGizmo *p_gizmo, Camera3D *p_camera, const Vector2 &p_point) const {
	Cluster3DEditorPlugin *editor = Cluster3DEditorPlugin::get_singleton();
	if (!editor || !editor->is_editing_parts()) {
		return -1;
	}
	const Cluster3D *cluster = Object::cast_to<Cluster3D>(p_gizmo->get_node_3d());
	ERR_FAIL_NULL_V(cluster, -1);

	const Transform3D global_transform = cluster->get_global_transform();
	const Vector3 ray_from = p_camera->project_ray_origin(p_point);
	const Vector3 ray_dir = p_camera->project_ray_normal(p_point);

	int closest = -1;
	real_t closest_distance = 0;
	for (int i = 0; i < cluster->get_part_count(); i++) {
		const Ref<Mesh> mesh = cluster->get_part_mesh(i);
		if (mesh.is_null()) {
			continue;
		}
		const Ref<TriangleMesh> triangle_mesh = mesh->generate_triangle_mesh();
		if (triangle_mesh.is_null()) {
			continue;
		}
		const Transform3D part_transform = global_transform * cluster->get_part_transform(i);
		const Transform3D inverse = part_transform.affine_inverse();
		Vector3 hit, normal;
		if (!triangle_mesh->intersect_ray(inverse.xform(ray_from), inverse.basis.xform(ray_dir).normalized(), hit, normal)) {
			continue;
		}
		const real_t distance = ray_from.distance_to(part_transform.xform(hit));
		if (closest < 0 || distance < closest_distance) {
			closest = i;
			closest_distance = distance;
		}
	}
	return closest;
}

Vector<int> Cluster3DGizmoPlugin::subgizmos_intersect_frustum(const EditorNode3DGizmo *p_gizmo, const Camera3D *p_camera, const Vector<Plane> &p_frustum) const {
	Vector<int> parts;
	Cluster3DEditorPlugin *editor = Cluster3DEditorPlugin::get_singleton();
	if (!editor || !editor->is_editing_parts()) {
		return parts;
	}
	const Cluster3D *cluster = Object::cast_to<Cluster3D>(p_gizmo->get_node_3d());
	ERR_FAIL_NULL_V(cluster, parts);

	// A part is in the box when its middle is: dragging a box over a heap of
	// rocks picks the ones it covers, without having to cover all of them.
	const Transform3D global_transform = cluster->get_global_transform();
	for (int i = 0; i < cluster->get_part_count(); i++) {
		const Vector3 center = global_transform.xform(cluster->get_part_aabb(i).get_center());
		bool inside = true;
		for (const Plane &plane : p_frustum) {
			if (plane.is_point_over(center)) {
				inside = false;
				break;
			}
		}
		if (inside) {
			parts.push_back(i);
		}
	}
	return parts;
}

Transform3D Cluster3DGizmoPlugin::get_subgizmo_transform(const EditorNode3DGizmo *p_gizmo, int p_id) const {
	const Cluster3D *cluster = Object::cast_to<Cluster3D>(p_gizmo->get_node_3d());
	ERR_FAIL_NULL_V(cluster, Transform3D());
	// A part removed from the Inspector can still be selected for a moment.
	if (p_id < 0 || p_id >= cluster->get_part_count()) {
		return Transform3D();
	}
	return cluster->get_part_transform(p_id);
}

void Cluster3DGizmoPlugin::set_subgizmo_transform(const EditorNode3DGizmo *p_gizmo, int p_id, Transform3D p_transform) {
	Cluster3D *cluster = Object::cast_to<Cluster3D>(p_gizmo->get_node_3d());
	ERR_FAIL_NULL(cluster);
	if (p_id < 0 || p_id >= cluster->get_part_count()) {
		return;
	}
	cluster->set_part_transform(p_id, p_transform);
}

void Cluster3DGizmoPlugin::commit_subgizmos(const EditorNode3DGizmo *p_gizmo, const Vector<int> &p_ids, const Vector<Transform3D> &p_restore, bool p_cancel) {
	Cluster3D *cluster = Object::cast_to<Cluster3D>(p_gizmo->get_node_3d());
	ERR_FAIL_NULL(cluster);

	if (p_cancel) {
		for (int i = 0; i < p_ids.size(); i++) {
			if (p_ids[i] < cluster->get_part_count()) {
				cluster->set_part_transform(p_ids[i], p_restore[i]);
			}
		}
		return;
	}

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(p_ids.size() == 1 ? TTR("Transform Cluster Part") : TTR("Transform Cluster Parts"), UndoRedo::MERGE_DISABLE, cluster);
	for (int i = 0; i < p_ids.size(); i++) {
		const int id = p_ids[i];
		if (id >= cluster->get_part_count()) {
			continue;
		}
		undo_redo->add_do_method(cluster, "set_part_transform", id, cluster->get_part_transform(id));
		undo_redo->add_undo_method(cluster, "set_part_transform", id, p_restore[i]);
	}
	undo_redo->commit_action();
}

// Editor plugin.

Cluster3D *Cluster3DEditorPlugin::_get_cluster() const {
	return ObjectDB::get_instance<Cluster3D>(cluster_id);
}

Ref<EditorNode3DGizmo> Cluster3DEditorPlugin::_get_gizmo(Cluster3D *p_cluster) const {
	for (const Ref<Node3DGizmo> &gizmo : p_cluster->get_gizmos()) {
		Ref<EditorNode3DGizmo> editor_gizmo = gizmo;
		if (editor_gizmo.is_valid() && editor_gizmo->get_plugin() == gizmo_plugin.ptr()) {
			return editor_gizmo;
		}
	}
	return Ref<EditorNode3DGizmo>();
}

Vector<int> Cluster3DEditorPlugin::_get_selected_parts(Cluster3D *p_cluster) const {
	Vector<int> parts;
	Node3DEditor *node_3d_editor = Node3DEditor::get_singleton();
	const Ref<EditorNode3DGizmo> gizmo = _get_gizmo(p_cluster);
	if (!node_3d_editor || gizmo.is_null() || node_3d_editor->get_single_selected_node() != p_cluster || !node_3d_editor->is_current_selected_gizmo(gizmo.ptr())) {
		return parts;
	}
	for (int id : node_3d_editor->get_subgizmo_selection()) {
		if (id >= 0 && id < p_cluster->get_part_count()) {
			parts.push_back(id);
		}
	}
	parts.sort();
	return parts;
}

void Cluster3DEditorPlugin::_select_parts(Object *p_cluster, const Vector<int> &p_parts) {
	Cluster3D *cluster = Object::cast_to<Cluster3D>(p_cluster);
	Node3DEditor *node_3d_editor = Node3DEditor::get_singleton();
	if (!cluster || !node_3d_editor || node_3d_editor->get_single_selected_node() != cluster) {
		return;
	}
	Node3DEditorSelectedItem *selected_item = EditorNode::get_singleton()->get_editor_selection()->get_node_editor_data<Node3DEditorSelectedItem>(cluster);
	if (!selected_item) {
		return;
	}

	selected_item->subgizmos.clear();
	if (editing_parts) {
		for (int id : p_parts) {
			if (id >= 0 && id < cluster->get_part_count()) {
				selected_item->subgizmos.insert(id, cluster->get_part_transform(id));
			}
		}
	}
	if (selected_item->subgizmos.is_empty()) {
		selected_item->gizmo.unref();
	} else {
		selected_item->gizmo = _get_gizmo(cluster);
	}
	cluster->update_gizmos();
	node_3d_editor->update_transform_gizmo();
}

void Cluster3DEditorPlugin::_commit_parts(Cluster3D *p_cluster, const String &p_action, const Array &p_snapshot, const Vector<int> &p_select) {
	const Array before = p_cluster->call("_get_parts_snapshot");
	const Vector<int> selected = _get_selected_parts(p_cluster);

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(p_action, UndoRedo::MERGE_DISABLE, p_cluster);
	undo_redo->add_do_method(p_cluster, "_set_parts_snapshot", p_snapshot);
	undo_redo->add_do_method(this, "_select_parts", p_cluster, p_select);
	undo_redo->add_undo_method(p_cluster, "_set_parts_snapshot", before);
	undo_redo->add_undo_method(this, "_select_parts", p_cluster, selected);
	undo_redo->commit_action();
}

void Cluster3DEditorPlugin::_duplicate_selected() {
	Cluster3D *cluster = _get_cluster();
	if (!cluster) {
		return;
	}
	const Vector<int> selected = _get_selected_parts(cluster);
	if (selected.is_empty()) {
		return;
	}

	// The copies go where the originals are, selected, so that the next drag
	// of the gizmo moves them away.
	const Array before = cluster->call("_get_parts_snapshot");
	Array after = before.duplicate();
	Vector<int> copies;
	for (int id : selected) {
		copies.push_back(after.size() / 3);
		after.push_back(before[id * 3 + 0]);
		after.push_back(before[id * 3 + 1]);
		after.push_back(Array(before[id * 3 + 2]).duplicate());
	}
	_commit_parts(cluster, selected.size() == 1 ? TTR("Duplicate Cluster Part") : TTR("Duplicate Cluster Parts"), after, copies);
}

void Cluster3DEditorPlugin::_delete_selected() {
	Cluster3D *cluster = _get_cluster();
	if (!cluster) {
		return;
	}
	const Vector<int> selected = _get_selected_parts(cluster);
	if (selected.is_empty()) {
		return;
	}

	const Array before = cluster->call("_get_parts_snapshot");
	Array after;
	for (int i = 0; i < cluster->get_part_count(); i++) {
		if (selected.has(i)) {
			continue;
		}
		after.push_back(before[i * 3 + 0]);
		after.push_back(before[i * 3 + 1]);
		after.push_back(before[i * 3 + 2]);
	}
	_commit_parts(cluster, selected.size() == 1 ? TTR("Delete Cluster Part") : TTR("Delete Cluster Parts"), after, Vector<int>());
}

void Cluster3DEditorPlugin::_select_all() {
	Cluster3D *cluster = _get_cluster();
	if (!cluster) {
		return;
	}
	Vector<int> all;
	for (int i = 0; i < cluster->get_part_count(); i++) {
		all.push_back(i);
	}
	_select_parts(cluster, all);
}

void Cluster3DEditorPlugin::_replace_mesh(const String &p_path) {
	Cluster3D *cluster = _get_cluster();
	if (!cluster) {
		return;
	}
	const Ref<Mesh> mesh = ResourceLoader::load(p_path);
	if (mesh.is_null()) {
		EditorNode::get_singleton()->show_warning(vformat(TTR("%s is not a mesh."), p_path.get_file()));
		return;
	}
	Vector<int> selected = _get_selected_parts(cluster);
	if (selected.is_empty()) {
		for (int i = 0; i < cluster->get_part_count(); i++) {
			selected.push_back(i);
		}
	}

	Array after = cluster->call("_get_parts_snapshot");
	for (int id : selected) {
		after[id * 3 + 0] = mesh;
	}
	_commit_parts(cluster, TTR("Replace Cluster Parts' Mesh"), after, selected);
}

bool Cluster3DEditorPlugin::can_drop_files(Node *p_node, const PackedStringArray &p_files) const {
	if (!editing_parts || !Object::cast_to<Cluster3D>(p_node) || p_files.is_empty()) {
		return false;
	}
	for (const String &path : p_files) {
		const String type = ResourceLoader::get_resource_type(path);
		if (!ClassDB::is_parent_class(type, Mesh::get_class_static()) && !ClassDB::is_parent_class(type, PackedScene::get_class_static())) {
			return false;
		}
	}
	return true;
}

void Cluster3DEditorPlugin::add_files(Cluster3D *p_cluster, const PackedStringArray &p_files, const Vector3 &p_global_position) {
	ERR_FAIL_NULL(p_cluster);

	// Adding to a copy works out what the parts will be, for the undoable
	// action to put in the cluster.
	const Array before = p_cluster->call("_get_parts_snapshot");
	Cluster3D *staging = memnew(Cluster3D);
	staging->call("_set_parts_snapshot", before);

	const Transform3D to_cluster = p_cluster->get_global_transform().affine_inverse();
	PackedStringArray failed;
	for (const String &path : p_files) {
		const Ref<Resource> resource = ResourceLoader::load(path);
		const Ref<Mesh> mesh = resource;
		const Ref<PackedScene> scene = resource;
		if (mesh.is_valid()) {
			staging->add_part(mesh, to_cluster * Transform3D(Basis(), p_global_position));
		} else if (scene.is_valid()) {
			Node *root = scene->instantiate();
			if (!root) {
				failed.push_back(path.get_file());
				continue;
			}
			// As the 3D editor places a scene dropped into the viewport.
			const Node3D *root_3d = Object::cast_to<Node3D>(root);
			const Transform3D placement = root_3d ? Transform3D(root_3d->get_basis(), p_global_position + root_3d->get_position()) : Transform3D(Basis(), p_global_position);
			if (staging->add_parts_from_node(root, to_cluster * placement) == 0) {
				failed.push_back(path.get_file());
			}
			memdelete(root);
		} else {
			failed.push_back(path.get_file());
		}
	}

	const Array after = staging->call("_get_parts_snapshot");
	memdelete(staging);

	if (!failed.is_empty()) {
		EditorNode::get_singleton()->show_warning(vformat(TTR("No meshes to add from %s."), String(", ").join(failed)));
	}
	if (after.size() == before.size()) {
		return;
	}

	Vector<int> added;
	for (int i = before.size() / 3; i < after.size() / 3; i++) {
		added.push_back(i);
	}
	_commit_parts(p_cluster, TTR("Add Cluster Parts"), after, added);
}

void Cluster3DEditorPlugin::_pack_children() {
	Cluster3D *cluster = _get_cluster();
	if (!cluster) {
		return;
	}
	Node *edited_scene = EditorNode::get_singleton()->get_edited_scene();
	ERR_FAIL_NULL(edited_scene);

	const Array before = cluster->call("_get_parts_snapshot");
	Cluster3D *staging = memnew(Cluster3D);
	staging->call("_set_parts_snapshot", before);

	// Every child branch with a mesh in it becomes parts, and goes.
	Vector<Node *> packed;
	for (int i = 0; i < cluster->get_child_count(false); i++) {
		Node3D *child = Object::cast_to<Node3D>(cluster->get_child(i, false));
		if (!child) {
			continue;
		}
		if (staging->add_parts_from_node(child, child->get_transform()) > 0) {
			packed.push_back(child);
		}
	}

	const Array after = staging->call("_get_parts_snapshot");
	memdelete(staging);

	if (packed.is_empty()) {
		EditorNode::get_singleton()->show_warning(TTR("There are no MeshInstance3D nodes (or scenes with meshes) under this Cluster3D to pack."));
		return;
	}

	Vector<int> added;
	for (int i = before.size() / 3; i < after.size() / 3; i++) {
		added.push_back(i);
	}

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(TTR("Pack Child Meshes into Cluster"), UndoRedo::MERGE_DISABLE, cluster);
	// In the order they are in, so that undoing puts each back where it was.
	for (Node *child : packed) {
		List<Node *> owned;
		child->get_owned_by(child->get_owner(), &owned);
		Array owners;
		for (Node *node : owned) {
			owners.push_back(node);
		}
		undo_redo->add_do_method(cluster, "remove_child", child);
		undo_redo->add_undo_method(cluster, "add_child", child, true);
		undo_redo->add_undo_method(cluster, "move_child", child, child->get_index(false));
		undo_redo->add_undo_method(SceneTreeDock::get_singleton(), "_set_owners", edited_scene, owners);
		undo_redo->add_undo_reference(child);
	}
	const Vector<int> selected = _get_selected_parts(cluster);
	undo_redo->add_do_method(cluster, "_set_parts_snapshot", after);
	undo_redo->add_do_method(this, "_select_parts", cluster, added);
	undo_redo->add_undo_method(cluster, "_set_parts_snapshot", before);
	undo_redo->add_undo_method(this, "_select_parts", cluster, selected);
	undo_redo->commit_action();
}

void Cluster3DEditorPlugin::_unpack() {
	Cluster3D *cluster = _get_cluster();
	if (!cluster || cluster->get_part_count() == 0) {
		return;
	}
	Node *edited_scene = EditorNode::get_singleton()->get_edited_scene();
	ERR_FAIL_NULL(edited_scene);

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(TTR("Unpack Cluster Parts"), UndoRedo::MERGE_DISABLE, cluster);
	for (int i = 0; i < cluster->get_part_count(); i++) {
		const Ref<Mesh> mesh = cluster->get_part_mesh(i);
		if (mesh.is_null()) {
			continue;
		}
		MeshInstance3D *mesh_instance = memnew(MeshInstance3D);
		mesh_instance->set_mesh(mesh);
		mesh_instance->set_transform(cluster->get_part_transform(i));
		const TypedArray<Material> materials = cluster->get_part_materials(i);
		for (int s = 0; s < materials.size() && s < mesh->get_surface_count(); s++) {
			mesh_instance->set_surface_override_material(s, materials[s]);
		}
		String name = mesh->get_name();
		if (name.is_empty() && mesh->get_path().is_resource_file()) {
			name = mesh->get_path().get_file().get_basename();
		}
		mesh_instance->set_name(name.is_empty() ? String("Part") : Node::adjust_name_casing(name));

		undo_redo->add_do_method(cluster, "add_child", mesh_instance, true);
		undo_redo->add_do_method(mesh_instance, "set_owner", edited_scene);
		undo_redo->add_do_reference(mesh_instance);
		undo_redo->add_undo_method(cluster, "remove_child", mesh_instance);
	}
	const Vector<int> selected = _get_selected_parts(cluster);
	undo_redo->add_do_method(cluster, "_set_parts_snapshot", Array());
	undo_redo->add_do_method(this, "_select_parts", cluster, Vector<int>());
	undo_redo->add_undo_method(cluster, "_set_parts_snapshot", cluster->call("_get_parts_snapshot"));
	undo_redo->add_undo_method(this, "_select_parts", cluster, selected);
	undo_redo->commit_action();
}

void Cluster3DEditorPlugin::_move_origin(int p_option) {
	Cluster3D *cluster = _get_cluster();
	if (!cluster || cluster->get_part_count() == 0) {
		return;
	}

	Vector3 origin;
	const AABB aabb = cluster->get_aabb();
	switch (p_option) {
		case MENU_ORIGIN_TO_CENTER: {
			origin = aabb.get_center();
		} break;
		case MENU_ORIGIN_TO_BOTTOM: {
			// Where a rock or a tree meets the ground it is placed on.
			origin = aabb.get_center();
			origin.y = aabb.position.y;
		} break;
		case MENU_ORIGIN_TO_SELECTED: {
			const Vector<int> selected = _get_selected_parts(cluster);
			if (selected.is_empty()) {
				EditorNode::get_singleton()->show_warning(TTR("Select a part to put the origin at, with Edit Parts on."));
				return;
			}
			origin = cluster->get_part_transform(selected[0]).origin;
		} break;
	}

	if (origin.is_zero_approx()) {
		return;
	}

	const Vector<int> selected = _get_selected_parts(cluster);
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(TTR("Move Cluster Origin"), UndoRedo::MERGE_DISABLE, cluster);
	undo_redo->add_do_method(cluster, "move_origin", origin);
	undo_redo->add_do_method(this, "_select_parts", cluster, selected);
	undo_redo->add_undo_method(cluster, "move_origin", -origin);
	undo_redo->add_undo_method(this, "_select_parts", cluster, selected);
	undo_redo->commit_action();
}

void Cluster3DEditorPlugin::_save_mesh(const String &p_path) {
	Cluster3D *cluster = _get_cluster();
	if (!cluster) {
		return;
	}
	const Ref<ArrayMesh> mesh = cluster->bake_mesh(true);
	if (mesh.is_null()) {
		EditorNode::get_singleton()->show_warning(TTR("This Cluster3D has no triangles to save as a mesh."));
		return;
	}
	EditorNode::get_singleton()->save_resource_in_path(mesh, p_path);
}

void Cluster3DEditorPlugin::_menu_option(int p_option) {
	Cluster3D *cluster = _get_cluster();
	if (!cluster) {
		return;
	}

	switch (p_option) {
		case MENU_ADD_MESH:
		case MENU_REPLACE_MESH: {
			file_dialog_mode = p_option == MENU_ADD_MESH ? FILE_DIALOG_ADD : FILE_DIALOG_REPLACE;
			file_dialog->set_file_mode(p_option == MENU_ADD_MESH ? EditorFileDialog::FILE_MODE_OPEN_FILES : EditorFileDialog::FILE_MODE_OPEN_FILE);
			file_dialog->set_title(p_option == MENU_ADD_MESH ? TTR("Add Meshes or Scenes as Parts") : TTR("Replace Selected Parts' Mesh"));
			file_dialog->clear_filters();
			List<String> extensions;
			ResourceLoader::get_recognized_extensions_for_type(Mesh::get_class_static(), &extensions);
			if (p_option == MENU_ADD_MESH) {
				ResourceLoader::get_recognized_extensions_for_type(PackedScene::get_class_static(), &extensions);
			}
			HashSet<String> added;
			for (const String &extension : extensions) {
				if (!added.has(extension)) {
					added.insert(extension);
					file_dialog->add_filter("*." + extension, extension.to_upper());
				}
			}
			file_dialog->popup_file_dialog();
		} break;
		case MENU_DUPLICATE: {
			_duplicate_selected();
		} break;
		case MENU_DELETE: {
			_delete_selected();
		} break;
		case MENU_SELECT_ALL: {
			_select_all();
		} break;
		case MENU_PACK_CHILDREN: {
			_pack_children();
		} break;
		case MENU_UNPACK: {
			_unpack();
		} break;
		case MENU_ORIGIN_TO_CENTER:
		case MENU_ORIGIN_TO_BOTTOM:
		case MENU_ORIGIN_TO_SELECTED: {
			_move_origin(p_option);
		} break;
		case MENU_SAVE_MESH: {
			file_dialog_mode = FILE_DIALOG_SAVE_MESH;
			file_dialog->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
			file_dialog->set_title(TTR("Save Cluster as One Mesh"));
			file_dialog->clear_filters();
			Ref<ArrayMesh> sample;
			sample.instantiate();
			List<String> extensions;
			ResourceSaver::get_recognized_extensions(sample, &extensions);
			for (const String &extension : extensions) {
				file_dialog->add_filter("*." + extension, extension.to_upper());
			}
			String scene_path = EditorNode::get_singleton()->get_edited_scene() ? EditorNode::get_singleton()->get_edited_scene()->get_scene_file_path() : String();
			const String file = String(cluster->get_name()).to_snake_case() + ".res";
			file_dialog->set_current_path(scene_path.is_empty() ? "res://" + file : scene_path.get_base_dir().path_join(file));
			file_dialog->popup_file_dialog();
		} break;
	}
}

void Cluster3DEditorPlugin::_file_selected(const String &p_path) {
	switch (file_dialog_mode) {
		case FILE_DIALOG_ADD: {
			PackedStringArray paths;
			paths.push_back(p_path);
			_files_selected(paths);
		} break;
		case FILE_DIALOG_REPLACE: {
			_replace_mesh(p_path);
		} break;
		case FILE_DIALOG_SAVE_MESH: {
			_save_mesh(p_path);
		} break;
	}
}

void Cluster3DEditorPlugin::_files_selected(const PackedStringArray &p_paths) {
	Cluster3D *cluster = _get_cluster();
	if (!cluster || file_dialog_mode != FILE_DIALOG_ADD) {
		return;
	}
	add_files(cluster, p_paths, cluster->get_global_transform().origin);
}

void Cluster3DEditorPlugin::_edit_parts_toggled(bool p_pressed) {
	editing_parts = p_pressed;
	EditorSettings::get_singleton()->set_project_metadata("cluster_3d", "edit_parts", editing_parts);
	Cluster3D *cluster = _get_cluster();
	if (cluster) {
		if (!editing_parts) {
			_select_parts(cluster, Vector<int>());
		}
		cluster->update_gizmos();
	}
	_update_menu();
}

void Cluster3DEditorPlugin::_update_menu() {
	PopupMenu *popup = cluster_menu->get_popup();
	for (int option : { MENU_DUPLICATE, MENU_DELETE, MENU_SELECT_ALL, MENU_ORIGIN_TO_SELECTED }) {
		popup->set_item_disabled(popup->get_item_index(option), !editing_parts);
	}
}

void Cluster3DEditorPlugin::_update_theme() {
	edit_parts_button->set_button_icon(toolbar->get_editor_theme_icon(SNAME("ToolSelect")));
	PopupMenu *popup = cluster_menu->get_popup();
	const struct {
		MenuOption option;
		const char *icon;
	} icons[] = {
		{ MENU_ADD_MESH, "Add" },
		{ MENU_DUPLICATE, "Duplicate" },
		{ MENU_DELETE, "Remove" },
		{ MENU_SELECT_ALL, "ListSelect" },
		{ MENU_REPLACE_MESH, "Reload" },
		{ MENU_PACK_CHILDREN, "Group" },
		{ MENU_UNPACK, "Ungroup" },
		{ MENU_ORIGIN_TO_CENTER, "EditorPivot" },
		{ MENU_ORIGIN_TO_BOTTOM, "EditorPivot" },
		{ MENU_ORIGIN_TO_SELECTED, "EditorPivot" },
		{ MENU_SAVE_MESH, "ArrayMesh" },
	};
	for (const auto &icon : icons) {
		popup->set_item_icon(popup->get_item_index(icon.option), toolbar->get_editor_theme_icon(icon.icon));
	}
}

void Cluster3DEditorPlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			editing_parts = EditorSettings::get_singleton()->get_project_metadata("cluster_3d", "edit_parts", true);
			edit_parts_button->set_pressed_no_signal(editing_parts);
			_update_menu();
		} break;
	}
}

EditorPlugin::AfterGUIInput Cluster3DEditorPlugin::forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) {
	const Ref<InputEventKey> key = p_event;
	if (key.is_null() || !key->is_pressed() || !editing_parts) {
		return EditorPlugin::AFTER_GUI_INPUT_PASS;
	}
	Cluster3D *cluster = _get_cluster();
	if (!cluster || _get_selected_parts(cluster).is_empty()) {
		// Then the keys are for the node, as usual.
		return EditorPlugin::AFTER_GUI_INPUT_PASS;
	}

	if (ED_IS_SHORTCUT("scene_tree/duplicate", p_event)) {
		_duplicate_selected();
	} else if (!key->is_echo() && (ED_IS_SHORTCUT("scene_tree/delete", p_event) || ED_IS_SHORTCUT("scene_tree/delete_no_confirm", p_event))) {
		_delete_selected();
	} else {
		return EditorPlugin::AFTER_GUI_INPUT_PASS;
	}

	// Keeps the Scene dock from duplicating or deleting the node too.
	Node3DEditor::get_singleton()->accept_event();
	return EditorPlugin::AFTER_GUI_INPUT_STOP;
}

bool Cluster3DEditorPlugin::handles(Object *p_object) const {
	return Object::cast_to<Cluster3D>(p_object) != nullptr;
}

void Cluster3DEditorPlugin::edit(Object *p_object) {
	Cluster3D *cluster = Object::cast_to<Cluster3D>(p_object);
	cluster_id = cluster ? cluster->get_instance_id() : ObjectID();
}

void Cluster3DEditorPlugin::make_visible(bool p_visible) {
	toolbar->set_visible(p_visible);
	if (!p_visible) {
		cluster_id = ObjectID();
	}
}

void Cluster3DEditorPlugin::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_select_parts", "cluster", "parts"), &Cluster3DEditorPlugin::_select_parts);
}

Cluster3DEditorPlugin::Cluster3DEditorPlugin() {
	singleton = this;

	gizmo_plugin.instantiate();
	Node3DEditor::get_singleton()->add_gizmo_plugin(gizmo_plugin);

	toolbar = memnew(HBoxContainer);
	toolbar->hide();
	toolbar->connect(SceneStringName(theme_changed), callable_mp(this, &Cluster3DEditorPlugin::_update_theme));
	add_control_to_container(CONTAINER_SPATIAL_EDITOR_MENU, toolbar);

	toolbar->add_child(memnew(VSeparator));

	edit_parts_button = memnew(Button);
	edit_parts_button->set_theme_type_variation(SceneStringName(FlatButton));
	edit_parts_button->set_toggle_mode(true);
	edit_parts_button->set_pressed(editing_parts);
	edit_parts_button->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	edit_parts_button->set_text(TTR("Edit Parts"));
	edit_parts_button->set_tooltip_text(TTR("Pick the cluster's parts in the viewport instead of the cluster as a whole.") + "\n" + TTR("Click: Select a part") + "\n" + TTR("Shift+Click: Select more parts") + "\n" + TTR("Drag a box: Select the parts whose middle is in it") + "\n" + vformat(TTR("%s: Duplicate the selected parts in place"), ED_GET_SHORTCUT("scene_tree/duplicate")->get_as_text()) + "\n" + vformat(TTR("%s: Delete the selected parts"), ED_GET_SHORTCUT("scene_tree/delete")->get_as_text()) + "\n" + TTR("Click empty space: Deselect the parts, so that the gizmo moves the whole cluster") + "\n" + TTR("Drop meshes or scenes from the FileSystem dock: Add them as parts"));
	edit_parts_button->connect(SceneStringName(toggled), callable_mp(this, &Cluster3DEditorPlugin::_edit_parts_toggled));
	toolbar->add_child(edit_parts_button);

	cluster_menu = memnew(MenuButton);
	cluster_menu->set_flat(false);
	cluster_menu->set_theme_type_variation("FlatMenuButton");
	cluster_menu->set_text(TTR("Cluster"));
	cluster_menu->set_switch_on_hover(true);
	// The shortcuts are shown, but forward_3d_gui_input() handles them, and
	// only while parts are selected.
	cluster_menu->set_disable_shortcuts(true);
	toolbar->add_child(cluster_menu);

	PopupMenu *popup = cluster_menu->get_popup();
	popup->add_item(TTR("Add Meshes or Scenes..."), MENU_ADD_MESH);
	popup->add_separator();
	popup->add_shortcut(ED_GET_SHORTCUT("scene_tree/duplicate"), MENU_DUPLICATE);
	popup->set_item_text(-1, TTR("Duplicate Selected Parts"));
	popup->add_shortcut(ED_GET_SHORTCUT("scene_tree/delete"), MENU_DELETE);
	popup->set_item_text(-1, TTR("Delete Selected Parts"));
	popup->add_item(TTR("Select All Parts"), MENU_SELECT_ALL);
	popup->add_item(TTR("Replace Mesh of Selected Parts..."), MENU_REPLACE_MESH);
	popup->set_item_tooltip(-1, TTR("Every part's, when none is selected."));
	popup->add_separator();
	popup->add_item(TTR("Pack Child Meshes"), MENU_PACK_CHILDREN);
	popup->set_item_tooltip(-1, TTR("Makes the MeshInstance3D nodes (and scenes with meshes) under the cluster its parts, where they are, and removes them."));
	popup->add_item(TTR("Unpack to MeshInstance3D Nodes"), MENU_UNPACK);
	popup->set_item_tooltip(-1, TTR("Turns every part into a MeshInstance3D child of the cluster, where it is."));
	popup->add_separator(TTR("Origin"));
	popup->add_item(TTR("Origin to Center of Parts"), MENU_ORIGIN_TO_CENTER);
	popup->add_item(TTR("Origin to Bottom Center of Parts"), MENU_ORIGIN_TO_BOTTOM);
	popup->add_item(TTR("Origin to Selected Part"), MENU_ORIGIN_TO_SELECTED);
	popup->add_separator();
	popup->add_item(TTR("Save as One Mesh..."), MENU_SAVE_MESH);
	popup->set_item_tooltip(-1, TTR("Merges the parts into one mesh, by material, with LODs and a shadow mesh, and saves it, to use as any other mesh."));
	popup->connect(SceneStringName(id_pressed), callable_mp(this, &Cluster3DEditorPlugin::_menu_option));

	file_dialog = memnew(EditorFileDialog);
	file_dialog->connect("file_selected", callable_mp(this, &Cluster3DEditorPlugin::_file_selected));
	file_dialog->connect("files_selected", callable_mp(this, &Cluster3DEditorPlugin::_files_selected));
	toolbar->add_child(file_dialog);
}

Cluster3DEditorPlugin::~Cluster3DEditorPlugin() {
	if (singleton == this) {
		singleton = nullptr;
	}
}
