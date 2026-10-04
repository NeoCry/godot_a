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
#include "editor/inspector/editor_inspector.h"
#include "editor/plugins/editor_plugin.h"
#include "scene/3d/landscape_3d.h"

class Button;
class CheckBox;
class ConfirmationDialog;
class EditorFileDialog;
class HBoxContainer;
class Label;
class Landscape3DEditorPlugin;
class MenuButton;
class OptionButton;
class PopupPanel;
class SpinBox;
class StandardMaterial3D;

// Puts Landscape3D's import and generation tools into its inspector, right
// under its layers: unlike the brushes in the viewport's toolbar, they are
// used once in a while, on the terrain as a whole.
class EditorInspectorPluginLandscape3D : public EditorInspectorPlugin {
	GDCLASS(EditorInspectorPluginLandscape3D, EditorInspectorPlugin);

	Landscape3DEditorPlugin *plugin = nullptr;

public:
	virtual bool can_handle(Object *p_object) override;
	virtual bool parse_property(Object *p_object, const Variant::Type p_type, const String &p_path, const PropertyHint p_hint, const String &p_hint_text, const BitField<PropertyUsageFlags> p_usage, const bool p_wide = false) override;

	void set_plugin(Landscape3DEditorPlugin *p_plugin) { plugin = p_plugin; }
};

// In-viewport brush tool for Landscape3D: sculpts the heightmap (raise, lower,
// smooth, flatten), paints, erases and blends texture layers, and cuts/fills
// holes, all by raycasting against the terrain's own physics collider (kept in
// sync by Landscape3D::update_collision) rather than needing separate pick
// geometry. Laid out after Unreal Engine's landscape tools: the sculpting and
// the painting brushes each have their own list in the toolbar, holding Shift
// turns a brush around (Raise into Lower, Paint into Erase, Hole into Unhole),
// and the mouse wheel sizes the brush over the terrain. Modeled after
// FoliagePainter3DEditorPlugin's brush workflow.
class Landscape3DEditorPlugin : public EditorPlugin {
	GDCLASS(Landscape3DEditorPlugin, EditorPlugin);

public:
	enum Mode {
		// No brush: the viewport selects and moves things as usual.
		MODE_SELECT,
		// Sculpting.
		MODE_RAISE,
		MODE_LOWER,
		MODE_SMOOTH,
		MODE_FLATTEN,
		MODE_HOLE,
		MODE_UNHOLE,
		// Painting material layers.
		MODE_PAINT,
		MODE_ERASE,
		MODE_BLEND,
	};

	// What the buttons the inspector shows (see EditorInspectorPluginLandscape3D) open.
	enum Tool {
		TOOL_IMPORT_HEIGHTMAP,
		TOOL_IMPORT_LAYER_MASK,
		TOOL_GENERATE_LAYER_MASK,
	};

private:
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
		// One entry per layer in use when the stroke started (painting
		// modes only): painting one layer renormalizes every other layer's
		// weight too (see Landscape3D::paint_layer), so undoing a stroke has
		// to restore all of them, not just the one the user picked.
		Vector<PackedFloat32Array> before_weights;
		PackedByteArray before_holes;
	};

	Landscape3D *terrain = nullptr;
	// The terrain the import and generation dialogs work on: the one whose
	// inspector opened them. Held by ID, since it can be deleted while a
	// dialog is open.
	ObjectID tool_terrain;

	Ref<EditorInspectorPluginLandscape3D> inspector_plugin;

	// Toolbar.
	HBoxContainer *topmenu_bar = nullptr;
	OptionButton *sculpt_mode_option = nullptr;
	OptionButton *paint_mode_option = nullptr;
	SpinBox *brush_radius_spin = nullptr;
	SpinBox *brush_strength_spin = nullptr;
	SpinBox *brush_falloff_spin = nullptr;
	MenuButton *mat_layers_menu = nullptr;
	MenuButton *add_spline_menu = nullptr;

	// Painting only where the ground is within a height band and a slope band.
	Button *mask_button = nullptr;
	PopupPanel *mask_popup = nullptr;
	CheckBox *mask_height_check = nullptr;
	SpinBox *mask_height_min_spin = nullptr;
	SpinBox *mask_height_max_spin = nullptr;
	SpinBox *mask_height_falloff_spin = nullptr;
	CheckBox *mask_slope_check = nullptr;
	SpinBox *mask_slope_min_spin = nullptr;
	SpinBox *mask_slope_max_spin = nullptr;
	SpinBox *mask_slope_falloff_spin = nullptr;

	// Thumbnails of the layers' albedo textures, by texture, as
	// EditorResourcePreview makes them; and plain swatches, by color, for the
	// layers that have none.
	HashMap<ObjectID, Ref<Texture2D>> layer_texture_previews;
	HashMap<Color, Ref<Texture2D>> layer_color_swatches;

	// Heightmap import dialogs.
	EditorFileDialog *import_file_dialog = nullptr;
	ConfirmationDialog *import_height_range_dialog = nullptr;
	SpinBox *import_height_min_spin = nullptr;
	SpinBox *import_height_max_spin = nullptr;
	String pending_import_path;

	// Layer mask import dialogs.
	EditorFileDialog *mask_file_dialog = nullptr;
	ConfirmationDialog *mask_options_dialog = nullptr;
	OptionButton *mask_layer_option = nullptr;
	OptionButton *mask_channel_option = nullptr;
	CheckBox *mask_normalize_check = nullptr;
	String pending_mask_path;

	// Procedural (height/slope) layer mask dialog.
	ConfirmationDialog *generate_mask_dialog = nullptr;
	OptionButton *generate_layer_option = nullptr;
	Label *generate_range_label = nullptr;
	SpinBox *generate_height_min_spin = nullptr;
	SpinBox *generate_height_max_spin = nullptr;
	SpinBox *generate_height_falloff_spin = nullptr;
	SpinBox *generate_slope_min_spin = nullptr;
	SpinBox *generate_slope_max_spin = nullptr;
	SpinBox *generate_slope_falloff_spin = nullptr;
	Label *generate_curvature_range_label = nullptr;
	SpinBox *generate_curvature_min_spin = nullptr;
	SpinBox *generate_curvature_max_spin = nullptr;
	SpinBox *generate_curvature_falloff_spin = nullptr;
	SpinBox *generate_curvature_radius_spin = nullptr;
	CheckBox *generate_normalize_check = nullptr;

	Mode mode = MODE_SELECT;
	// Shift is held: the brush does the opposite of its mode (see
	// _get_effective_mode()).
	bool inverted = false;
	float brush_radius = 10.0;
	float brush_strength = 2.0;
	float brush_falloff = 1.0;
	int paint_layer_index = 0;

	// Brush cursor overlay (drawn directly through RenderingServer, like
	// FoliagePainter3DEditorPlugin's own cursor).
	RID cursor_mesh;
	RID cursor_instance;
	Ref<StandardMaterial3D> cursor_material;
	// Where the cursor last was on the terrain, to redraw it when the brush
	// changes under a still mouse.
	bool cursor_on_terrain = false;
	Vector3 cursor_position;
	Vector3 cursor_normal;

	// Active stroke (mouse held down). Each touched chunk's pre-stroke state
	// is snapshotted the first time the stroke reaches it, so the whole
	// stroke can be undone/redone as one region write per touched chunk.
	bool stroke_active = false;
	// What the stroke does, Shift included, as it started.
	Mode stroke_mode = MODE_SELECT;
	float stroke_flatten_height = 0.0;
	HashMap<Vector2i, TouchedChunkRegion> touched_regions;
	uint64_t last_stamp_msec = 0;

	static bool _is_paint_mode(Mode p_mode);
	Mode _get_effective_mode() const;
	void _set_mode(Mode p_mode);
	void _mode_selected(int p_index, OptionButton *p_option);
	void _set_inverted(bool p_inverted);

	void _update_theme();
	void _update_cursor_color();

	void _set_brush_radius(double p_value);
	// Grows or shrinks the brush by p_notches turns of the mouse wheel.
	void _scale_brush_radius(bool p_grow, float p_notches);
	void _radius_spin_gui_input(const Ref<InputEvent> &p_event);
	void _set_brush_strength(double p_value);
	void _set_brush_falloff(double p_value);

	void _mask_button_pressed();
	void _mask_changed();
	void _update_mask_button();
	Landscape3D::PaintMask _get_paint_mask() const;

	void _rebuild_mat_layers_menu();
	void _mat_layers_menu_id_pressed(int p_id);
	void _layer_preview_done(const String &p_path, const Ref<Texture2D> &p_preview, const Ref<Texture2D> &p_small_preview, ObjectID p_texture);
	// The icon a layer is listed with: its albedo texture's thumbnail, or a
	// swatch of its color, marked RVT when any of its textures is a virtual
	// texture.
	Ref<Texture2D> _get_layer_icon(const Ref<TerrainLayer> &p_layer);
	static bool _layer_uses_virtual_textures(const Ref<TerrainLayer> &p_layer);
	static String _get_layer_name(const Ref<TerrainLayer> &p_layer, int p_index);

	// Adds a LandscapeSpline3D of the chosen type under the terrain and
	// selects it, ready for its points to be placed.
	void _add_spline_menu_id_pressed(int p_id);

	Landscape3D *_get_tool_terrain() const;

	void _import_file_selected(const String &p_path);
	void _do_import_heightmap();

	void _mask_file_selected(const String &p_path);
	void _do_import_layer_mask();

	void _do_generate_layer_mask();
	// Curvature values have no intuitive scale, so the dialog states the range
	// the terrain actually covers at the chosen radius (which changes it).
	void _update_curvature_range_label(double p_unused = 0.0);
	// Fills p_option with p_terrain's layers in use and preselects the one
	// the Paint brush is on; returns false if there are none to offer.
	bool _fill_layer_option(Landscape3D *p_terrain, OptionButton *p_option);

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

	// Opens one of the terrain tools on p_terrain (see Tool).
	void open_tool(int p_tool, ObjectID p_terrain);

	Landscape3DEditorPlugin();
	~Landscape3DEditorPlugin();
};
