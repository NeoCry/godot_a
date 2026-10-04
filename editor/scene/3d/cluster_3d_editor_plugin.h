/**************************************************************************/
/*  cluster_3d_editor_plugin.h                                            */
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

#include "editor/plugins/editor_plugin.h"
#include "editor/scene/3d/node_3d_editor_gizmos.h"
#include "scene/3d/cluster_3d.h"

class Button;
class EditorFileDialog;
class HBoxContainer;
class MenuButton;

// Makes a Cluster3D's parts its subgizmos: while Edit Parts is on, clicking
// one of them (or dragging a box around some) selects them, and the 3D
// editor's own transform gizmo, snapping and keyboard modes move, rotate and
// scale them, undoably. The cluster's own transform is left alone, so the
// parts can be put together around its origin wherever that is.
class Cluster3DGizmoPlugin : public EditorNode3DGizmoPlugin {
	GDCLASS(Cluster3DGizmoPlugin, EditorNode3DGizmoPlugin);

public:
	virtual bool has_gizmo(Node3D *p_spatial) override;
	virtual String get_gizmo_name() const override;
	virtual int get_priority() const override;
	virtual bool can_be_hidden() const override;

	virtual void redraw(EditorNode3DGizmo *p_gizmo) override;

	virtual int subgizmos_intersect_ray(const EditorNode3DGizmo *p_gizmo, Camera3D *p_camera, const Vector2 &p_point) const override;
	virtual Vector<int> subgizmos_intersect_frustum(const EditorNode3DGizmo *p_gizmo, const Camera3D *p_camera, const Vector<Plane> &p_frustum) const override;
	virtual Transform3D get_subgizmo_transform(const EditorNode3DGizmo *p_gizmo, int p_id) const override;
	virtual void set_subgizmo_transform(const EditorNode3DGizmo *p_gizmo, int p_id, Transform3D p_transform) override;
	virtual void commit_subgizmos(const EditorNode3DGizmo *p_gizmo, const Vector<int> &p_ids, const Vector<Transform3D> &p_restore, bool p_cancel = false) override;

	Cluster3DGizmoPlugin();
};

// The Cluster menu and Edit Parts toggle in the 3D editor's toolbar, and the
// keys that act on selected parts instead of on the node: Ctrl+D duplicates
// them in place (and selects the copies, ready to be moved away), Delete
// removes them. It also takes meshes and scenes dropped into the viewport
// from the FileSystem dock as parts of the selected cluster (see
// Node3DEditorViewport::drop_data_fw()).
class Cluster3DEditorPlugin : public EditorPlugin {
	GDCLASS(Cluster3DEditorPlugin, EditorPlugin);

	enum MenuOption {
		MENU_ADD_MESH,
		MENU_DUPLICATE,
		MENU_DELETE,
		MENU_SELECT_ALL,
		MENU_REPLACE_MESH,
		MENU_PACK_CHILDREN,
		MENU_UNPACK,
		MENU_ORIGIN_TO_CENTER,
		MENU_ORIGIN_TO_BOTTOM,
		MENU_ORIGIN_TO_SELECTED,
		MENU_SAVE_MESH,
	};

	enum FileDialogMode {
		FILE_DIALOG_ADD,
		FILE_DIALOG_REPLACE,
		FILE_DIALOG_SAVE_MESH,
	};

	static inline Cluster3DEditorPlugin *singleton = nullptr;

	Ref<Cluster3DGizmoPlugin> gizmo_plugin;
	ObjectID cluster_id;
	bool editing_parts = true;

	HBoxContainer *toolbar = nullptr;
	Button *edit_parts_button = nullptr;
	MenuButton *cluster_menu = nullptr;
	EditorFileDialog *file_dialog = nullptr;
	FileDialogMode file_dialog_mode = FILE_DIALOG_ADD;

	Cluster3D *_get_cluster() const;
	Ref<EditorNode3DGizmo> _get_gizmo(Cluster3D *p_cluster) const;
	Vector<int> _get_selected_parts(Cluster3D *p_cluster) const;
	void _select_parts(Object *p_cluster, const Vector<int> &p_parts);

	void _update_theme();
	void _update_menu();
	void _edit_parts_toggled(bool p_pressed);
	void _menu_option(int p_option);
	void _file_selected(const String &p_path);
	void _files_selected(const PackedStringArray &p_paths);

	// Puts p_snapshot (as Cluster3D's _get_parts_snapshot() gives) in the
	// cluster in one undoable action, and selects p_select afterwards.
	void _commit_parts(Cluster3D *p_cluster, const String &p_action, const Array &p_snapshot, const Vector<int> &p_select);

	void _duplicate_selected();
	void _delete_selected();
	void _select_all();
	void _replace_mesh(const String &p_path);
	void _pack_children();
	void _unpack();
	void _move_origin(int p_option);
	void _save_mesh(const String &p_path);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	static Cluster3DEditorPlugin *get_singleton() { return singleton; }

	bool is_editing_parts() const { return editing_parts; }

	// Whether p_files (meshes or scenes) dropped into the viewport while
	// p_node is the one node selected go into it as parts.
	bool can_drop_files(Node *p_node, const PackedStringArray &p_files) const;
	// Adds p_files as parts of p_cluster, the meshes' origins and the
	// scenes' roots at p_global_position, undoably, and selects them.
	void add_files(Cluster3D *p_cluster, const PackedStringArray &p_files, const Vector3 &p_global_position);

	virtual String get_plugin_name() const override { return "Cluster3D"; }
	virtual bool handles(Object *p_object) const override;
	virtual void edit(Object *p_object) override;
	virtual void make_visible(bool p_visible) override;
	virtual EditorPlugin::AfterGUIInput forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) override;

	Cluster3DEditorPlugin();
	~Cluster3DEditorPlugin();
};
