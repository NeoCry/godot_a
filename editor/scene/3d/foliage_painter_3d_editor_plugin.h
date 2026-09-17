/**************************************************************************/
/*  foliage_painter_3d_editor_plugin.h                                    */
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

#include "core/math/face3.h"
#include "core/math/random_pcg.h"
#include "core/templates/hash_map.h"
#include "editor/plugins/editor_plugin.h"
#include "scene/3d/foliage_painter_3d.h"

class Button;
class ButtonGroup;
class HBoxContainer;
class MenuButton;
class MeshInstance3D;
class SpinBox;
class StandardMaterial3D;

// In-viewport brush tool for FoliagePainter3D: paints, erases, and single-place/
// remove instances of one or more foliage layers directly onto the actual
// triangle geometry of any MeshInstance3D under the cursor (no collision shapes
// required). Modeled after GridMap's paint/erase workflow and Path3D's toolbar.
class FoliagePainter3DEditorPlugin : public EditorPlugin {
	GDCLASS(FoliagePainter3DEditorPlugin, EditorPlugin);

	enum Mode {
		MODE_PAINT,
		MODE_ERASE,
		MODE_PLACE_SINGLE,
		MODE_REMOVE_SINGLE,
	};

	struct StrokeOp {
		int layer = 0;
		Vector2i cell;
		int index = 0;
		Transform3D transform;
		bool is_insert = true; // true: "do" inserts (undo removes). false: "do" removes (undo inserts).
	};

	FoliagePainter3D *painter = nullptr;

	// Toolbar.
	HBoxContainer *topmenu_bar = nullptr;
	HBoxContainer *toolbar = nullptr;
	Ref<ButtonGroup> mode_button_group;
	Button *mode_paint_button = nullptr;
	Button *mode_erase_button = nullptr;
	Button *mode_place_single_button = nullptr;
	Button *mode_remove_single_button = nullptr;
	SpinBox *brush_radius_spin = nullptr;
	SpinBox *brush_density_spin = nullptr;
	MenuButton *layers_menu = nullptr;

	Mode mode = MODE_PAINT;
	float brush_radius = 2.0;
	float brush_density = 6.0;
	Vector<bool> layer_active;

	// Brush cursor overlay (drawn directly through RenderingServer, like GridMap's cursor).
	RID cursor_mesh;
	RID cursor_instance;
	// Kept alive for as long as the plugin exists: mesh_surface_set_material()
	// only stores the material's RID, so if this Ref were local and dropped,
	// the material (and its RID) would be freed while the mesh surface still
	// referenced it, leaving cursor_mesh pointing at a dangling material RID.
	Ref<StandardMaterial3D> cursor_material;

	// Active stroke (mouse held down).
	bool stroke_active = false;
	Vector<StrokeOp> stroke_ops;
	uint64_t last_stamp_msec = 0;
	Vector<MeshInstance3D *> paint_targets;

	RandomPCG rng;

	// Raycasting against arbitrary MeshInstance3D geometry (no physics required).
	struct MeshFaceCache {
		LocalVector<Face3> faces;
		AABB local_aabb;
	};
	HashMap<ObjectID, MeshFaceCache> face_cache;

	const MeshFaceCache *_get_face_cache(MeshInstance3D *p_mesh_instance);
	void _collect_mesh_instances(Node *p_node, Vector<MeshInstance3D *> &r_out) const;
	bool _raycast(const Vector3 &p_from, const Vector3 &p_dir, float p_max_dist, Vector3 &r_position, Vector3 &r_normal);

	void _rebuild_layers_menu();
	void _layer_menu_id_pressed(int p_id);
	void _mode_pressed(int p_mode);
	void _set_brush_radius(double p_value);
	void _set_brush_density(double p_value);

	void _update_cursor(const Vector3 &p_position, const Vector3 &p_normal, bool p_visible);
	void _ensure_cursor_instance();
	void _refresh_paint_targets();

	Vector<int> _get_active_layers() const;
	Transform3D _make_instance_transform(const Ref<FoliageLayer> &p_layer, const Vector3 &p_position, const Vector3 &p_normal);

	void _begin_stroke();
	void _end_stroke();
	void _cancel_stroke();

	void _do_insert(int p_layer, const Transform3D &p_transform);
	void _do_remove(int p_layer, const Vector2i &p_cell, int p_index, const Transform3D &p_transform);

	void _stamp_paint(const Vector3 &p_position, const Vector3 &p_normal);
	void _stamp_erase(const Vector3 &p_position);
	void _place_single(const Vector3 &p_position, const Vector3 &p_normal);
	void _remove_single(const Vector3 &p_position);

	bool _do_input_action(Camera3D *p_camera, const Point2 &p_screen_pos, bool p_click);

protected:
	static void _bind_methods();

public:
	virtual String get_plugin_name() const override { return "FoliagePainter3D"; }
	virtual bool handles(Object *p_object) const override;
	virtual void edit(Object *p_object) override;
	virtual void make_visible(bool p_visible) override;
	virtual EditorPlugin::AfterGUIInput forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) override;

	FoliagePainter3DEditorPlugin();
	~FoliagePainter3DEditorPlugin();
};
