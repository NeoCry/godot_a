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
#include "core/templates/hash_set.h"
#include "editor/plugins/editor_plugin.h"
#include "editor/scene/3d/foliage_inspector_plugin.h"
#include "scene/3d/foliage_painter_3d.h"

class HBoxContainer;
class MenuButton;
class MeshInstance3D;
class OptionButton;
class SpinBox;
class StandardMaterial3D;
class Landscape3D;

// In-viewport brush tool for FoliagePainter3D: paints, erases, and single-place/
// remove instances of one or more foliage layers directly onto the actual
// triangle geometry of any MeshInstance3D under the cursor (no collision shapes
// required). Laid out after Landscape3DEditorPlugin, and so after Unreal
// Engine's foliage tools: the brushes are one list in the toolbar, holding
// Shift turns a brush around (Paint into Erase, Place Single into Remove
// Single), and the mouse wheel sizes the brush over the ground.
class FoliagePainter3DEditorPlugin : public EditorPlugin {
	GDCLASS(FoliagePainter3DEditorPlugin, EditorPlugin);

public:
	enum Mode {
		// No brush: the viewport selects and moves things as usual.
		MODE_SELECT,
		MODE_PAINT,
		MODE_ERASE,
		MODE_PLACE_SINGLE,
		MODE_REMOVE_SINGLE,
	};

private:
	struct StrokeOp {
		int layer = 0;
		Vector2i cell;
		int index = 0;
		Transform3D transform;
		bool is_insert = true; // true: "do" inserts (undo removes). false: "do" removes (undo inserts).
	};

	FoliagePainter3D *painter = nullptr;

	Ref<EditorInspectorPluginFoliage> inspector_plugin;

	// Toolbar.
	HBoxContainer *topmenu_bar = nullptr;
	OptionButton *mode_option = nullptr;
	SpinBox *brush_radius_spin = nullptr;
	SpinBox *brush_density_spin = nullptr;
	MenuButton *instances_menu = nullptr;

	Mode mode = MODE_SELECT;
	// Shift is held: the brush does the opposite of its mode (see
	// _get_effective_mode()).
	bool inverted = false;
	float brush_radius = 2.0;
	float brush_density = 6.0;
	// The layers the brush leaves alone, by FoliageLayer rather than by index:
	// a layer just added to the list is painted with right away, and taking a
	// layer out or reordering them does not hand its choice to another one.
	HashSet<ObjectID> inactive_layers;

	// Brush cursor overlay (drawn directly through RenderingServer, like GridMap's cursor).
	RID cursor_mesh;
	RID cursor_instance;
	// Kept alive for as long as the plugin exists: mesh_surface_set_material()
	// only stores the material's RID, so if this Ref were local and dropped,
	// the material (and its RID) would be freed while the mesh surface still
	// referenced it, leaving cursor_mesh pointing at a dangling material RID.
	Ref<StandardMaterial3D> cursor_material;
	// Where the cursor last was on the ground, to redraw it when the brush
	// changes under a still mouse.
	bool cursor_on_surface = false;
	Vector3 cursor_position;
	Vector3 cursor_normal;

	// Active stroke (mouse held down).
	bool stroke_active = false;
	// What the stroke does, Shift included, as it started.
	Mode stroke_mode = MODE_SELECT;
	Vector<StrokeOp> stroke_ops;
	uint64_t last_stamp_msec = 0;
	Vector<MeshInstance3D *> paint_targets;
	// Landscape3D has no single conventional Mesh (it is chunked), so it is
	// raycast separately, against its own physics collision body, rather than
	// through the MeshFaceCache mechanism below.
	Vector<Landscape3D *> paint_target_terrains;

	RandomPCG rng;

	// Raycasting against arbitrary MeshInstance3D geometry (no physics required).
	struct MeshFaceCache {
		LocalVector<Face3> faces;
		AABB local_aabb;
	};
	HashMap<ObjectID, MeshFaceCache> face_cache;

	const MeshFaceCache *_get_face_cache(MeshInstance3D *p_mesh_instance);
	void _collect_mesh_instances(Node *p_node, Vector<MeshInstance3D *> &r_out) const;
	void _collect_terrains(Node *p_node, Vector<Landscape3D *> &r_out) const;
	bool _raycast(const Vector3 &p_from, const Vector3 &p_dir, float p_max_dist, Vector3 &r_position, Vector3 &r_normal);
	bool _raycast_screen(Camera3D *p_camera, const Point2 &p_screen_pos, Vector3 &r_position, Vector3 &r_normal);

	void _rebuild_instances_menu();
	void _instances_menu_id_pressed(int p_id);

	Mode _get_effective_mode() const;
	void _set_mode(Mode p_mode);
	void _mode_selected(int p_index);
	void _set_inverted(bool p_inverted);

	void _update_theme();
	void _update_cursor_color();

	void _set_brush_radius(double p_value);
	// Grows or shrinks the brush by p_notches turns of the mouse wheel.
	void _scale_brush_radius(bool p_grow, float p_notches);
	void _radius_spin_gui_input(const Ref<InputEvent> &p_event);
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
