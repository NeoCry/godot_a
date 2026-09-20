/**************************************************************************/
/*  landscape_3d_editor_plugin.h                                          */
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
#include "editor/plugins/editor_plugin.h"
#include "scene/3d/landscape_3d.h"

class Button;
class ButtonGroup;
class ConfirmationDialog;
class EditorFileDialog;
class HBoxContainer;
class MenuButton;
class SpinBox;
class StandardMaterial3D;

// In-viewport brush tool for Landscape3D: sculpts the heightmap (raise, lower,
// smooth, flatten), paints texture layers, and cuts/fills holes, all by
// raycasting against the terrain's own physics collider (kept in sync by
// Landscape3D::update_collision) rather than needing separate pick geometry.
// Modeled after FoliagePainter3DEditorPlugin's brush workflow.
class Landscape3DEditorPlugin : public EditorPlugin {
	GDCLASS(Landscape3DEditorPlugin, EditorPlugin);

	enum Mode {
		MODE_RAISE,
		MODE_LOWER,
		MODE_SMOOTH,
		MODE_FLATTEN,
		MODE_PAINT,
		MODE_HOLE,
		MODE_UNHOLE,
	};

	// Which of TerrainData's independent pieces of per-sample data a mode's
	// brush strokes touch, and so which one needs snapshotting for undo/redo.
	enum class DataKind {
		HEIGHT,
		WEIGHTS,
		HOLE,
	};

	struct TouchedChunkRegion {
		Rect2i region;
		PackedFloat32Array before_heights;
		// One entry per layer that existed when the stroke started (PAINT
		// mode only): painting one layer renormalizes every other layer's
		// weight too (see Landscape3D::paint_layer), so undoing a stroke has
		// to restore all of them, not just the one the user picked.
		Vector<PackedFloat32Array> before_weights;
		PackedByteArray before_holes;
	};

	Landscape3D *terrain = nullptr;

	// Toolbar.
	HBoxContainer *topmenu_bar = nullptr;
	HBoxContainer *toolbar = nullptr;
	Ref<ButtonGroup> mode_button_group;
	Button *mode_raise_button = nullptr;
	Button *mode_lower_button = nullptr;
	Button *mode_smooth_button = nullptr;
	Button *mode_flatten_button = nullptr;
	Button *mode_paint_button = nullptr;
	Button *mode_hole_button = nullptr;
	Button *mode_unhole_button = nullptr;
	SpinBox *brush_radius_spin = nullptr;
	SpinBox *brush_strength_spin = nullptr;
	MenuButton *paint_layer_menu = nullptr;
	Button *import_heightmap_button = nullptr;

	// Heightmap import dialogs.
	EditorFileDialog *import_file_dialog = nullptr;
	ConfirmationDialog *import_height_range_dialog = nullptr;
	SpinBox *import_height_min_spin = nullptr;
	SpinBox *import_height_max_spin = nullptr;
	String pending_import_path;

	Mode mode = MODE_RAISE;
	float brush_radius = 10.0;
	float brush_strength = 2.0;
	int paint_layer_index = 0;

	// Brush cursor overlay (drawn directly through RenderingServer, like
	// FoliagePainter3DEditorPlugin's own cursor).
	RID cursor_mesh;
	RID cursor_instance;
	Ref<StandardMaterial3D> cursor_material;

	// Active stroke (mouse held down). Each touched chunk's pre-stroke state
	// is snapshotted the first time the stroke reaches it, so the whole
	// stroke can be undone/redone as one region write per touched chunk.
	bool stroke_active = false;
	float stroke_flatten_height = 0.0;
	HashMap<Vector2i, TouchedChunkRegion> touched_regions;
	uint64_t last_stamp_msec = 0;

	void _mode_pressed(int p_mode);
	void _set_brush_radius(double p_value);
	void _set_brush_strength(double p_value);

	void _rebuild_paint_layer_menu();
	void _paint_layer_menu_id_pressed(int p_id);

	void _import_heightmap_pressed();
	void _import_file_selected(const String &p_path);
	void _do_import_heightmap();

	DataKind _get_mode_data_kind() const;
	String _get_mode_action_name() const;
	Rect2i _get_brush_vertex_region(const Vector3 &p_local_position, float p_radius) const;
	void _snapshot_chunk_if_needed(const Vector2i &p_chunk);
	void _snapshot_region_chunks(const Rect2i &p_vertex_region);

	bool _raycast_terrain(Camera3D *p_camera, const Point2 &p_screen_pos, Vector3 &r_local_position, Vector3 &r_world_position, Vector3 &r_world_normal);

	void _update_cursor(const Vector3 &p_world_position, const Vector3 &p_world_normal, bool p_visible);
	void _ensure_cursor_instance();

	void _stamp(const Vector3 &p_local_position);

	void _begin_stroke();
	void _end_stroke();
	void _cancel_stroke();

	bool _do_input_action(Camera3D *p_camera, const Point2 &p_screen_pos, bool p_click);

protected:
	static void _bind_methods();

public:
	virtual String get_plugin_name() const override { return "Landscape3D"; }
	virtual bool handles(Object *p_object) const override;
	virtual void edit(Object *p_object) override;
	virtual void make_visible(bool p_visible) override;
	virtual EditorPlugin::AfterGUIInput forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) override;

	Landscape3DEditorPlugin();
	~Landscape3DEditorPlugin();
};
