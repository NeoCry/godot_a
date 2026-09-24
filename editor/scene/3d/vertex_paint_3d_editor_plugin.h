/**************************************************************************/
/*  vertex_paint_3d_editor_plugin.h                                       */
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
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "editor/plugins/editor_plugin.h"

class ArrayMesh;
class Button;
class ButtonGroup;
class ColorPickerButton;
class ConfirmationDialog;
class HBoxContainer;
class Mesh;
class MeshInstance3D;
class OptionButton;
class Shader;
class ShaderMaterial;
class SpinBox;
class StandardMaterial3D;

// In-viewport vertex color brush for MeshInstance3D: paints, blurs and erases
// the ARRAY_COLOR channel of the selected mesh's surfaces directly, with a
// live preview of the result. Modeled after FoliagePainter3DEditorPlugin's
// brush workflow.
//
// Strokes are written straight into the surface's attribute buffer through
// ArrayMesh::surface_update_attribute_region() rather than by rebuilding the
// surface from arrays: vertex colors are a fixed 4-byte RGBA8 field, so
// patching them in place keeps the mesh's index buffer, LODs, blend shapes and
// skinning data bit-for-bit intact, and stays fast enough to paint at
// interactive rates on dense meshes.
class VertexPaint3DEditorPlugin : public EditorPlugin {
	GDCLASS(VertexPaint3DEditorPlugin, EditorPlugin);

	enum Mode {
		MODE_PAINT,
		MODE_BLUR,
		MODE_ERASE,
	};

	// What the viewport shows while painting. Anything but VIEW_MODE_SHADED
	// installs an unshaded override material so the raw vertex colors are
	// visible even when the real material ignores them.
	enum ViewMode {
		VIEW_MODE_SHADED,
		VIEW_MODE_RGB,
		VIEW_MODE_R,
		VIEW_MODE_G,
		VIEW_MODE_B,
		VIEW_MODE_A,
	};

	// One vertex of one surface. A weld group can span surfaces, because
	// surfaces of the same mesh share the mesh's local space.
	struct VertexRef {
		int surface = 0;
		int index = 0;
	};

	// Every vertex sitting at one position. Exporters split vertices wherever
	// a UV, normal or material seam runs, so a brush that touched raw vertex
	// indices would paint only one side of each seam and leave a visible
	// crack; painting whole weld groups keeps strokes continuous across them.
	struct PaintGroup {
		Vector3 local_position;
		Vector3 world_position; // Refreshed at stroke start, so a stamp needs no per-vertex transform.
		LocalVector<VertexRef> refs;
		LocalVector<int> neighbors; // Weld groups sharing a triangle edge with this one, for MODE_BLUR.
	};

	struct SurfaceCache {
		int vertex_count = 0;
		uint32_t attribute_stride = 0;
		uint32_t color_offset = 0;
		LocalVector<Color> colors; // Working copy of this surface's vertex colors.
		// Copy of the surface's whole attribute buffer. Colors share it with
		// UVs and custom channels, so a contiguous region upload has to carry
		// those bytes along unchanged; keeping the buffer here avoids reading
		// it back off the GPU once per stamp.
		Vector<uint8_t> attribute_data;
		HashMap<int, Color> stroke_before; // Pre-stroke color of every vertex the stroke has touched so far.
		int dirty_first = -1;
		int dirty_last = -1;
	};

	MeshInstance3D *mesh_instance = nullptr;
	// Kept next to the pointer so cleanup that outlives a selection change
	// (removing the preview override) can tell whether the node still exists.
	ObjectID mesh_instance_id;
	Ref<ArrayMesh> target_mesh;
	LocalVector<SurfaceCache> surface_caches;
	LocalVector<PaintGroup> groups;
	// Triangles of the target mesh, in its local space, used to place the
	// brush on the surface under the cursor without needing any collider.
	LocalVector<Face3> target_faces;
	AABB target_local_aabb;
	bool cache_valid = false;

	// Toolbar.
	HBoxContainer *topmenu_bar = nullptr;
	HBoxContainer *toolbar = nullptr;
	Button *paint_enabled_button = nullptr;
	HBoxContainer *brush_controls = nullptr;
	Ref<ButtonGroup> mode_button_group;
	Button *mode_paint_button = nullptr;
	Button *mode_blur_button = nullptr;
	Button *mode_erase_button = nullptr;
	ColorPickerButton *color_button = nullptr;
	SpinBox *brush_radius_spin = nullptr;
	SpinBox *brush_strength_spin = nullptr;
	SpinBox *brush_hardness_spin = nullptr;
	Button *channel_buttons[4] = {};
	OptionButton *view_mode_button = nullptr;
	Button *fill_button = nullptr;
	Button *clear_button = nullptr;

	ConfirmationDialog *convert_dialog = nullptr;
	ConfirmationDialog *external_mesh_dialog = nullptr;
	// Mesh the user already chose to paint in place despite it living in an
	// external file, so the warning is not raised again for it.
	ObjectID external_accepted_mesh_id;

	bool paint_enabled = false;
	Mode mode = MODE_PAINT;
	Color brush_color = Color(1, 0, 0, 1);
	Color erase_color = Color(1, 1, 1, 1);
	float brush_radius = 0.5;
	float brush_strength = 0.5;
	float brush_hardness = 0.5;
	bool channel_enabled[4] = { true, true, true, true };
	ViewMode view_mode = VIEW_MODE_RGB;

	// Brush cursor overlay (drawn directly through RenderingServer, like
	// FoliagePainter3DEditorPlugin's own cursor).
	RID cursor_mesh;
	RID cursor_instance;
	// Kept alive for as long as the plugin exists: mesh_surface_set_material()
	// only stores the material's RID, so if this Ref were local and dropped,
	// the material (and its RID) would be freed while the mesh surface still
	// referenced it, leaving cursor_mesh pointing at a dangling material RID.
	Ref<StandardMaterial3D> cursor_material;

	// Viewport-only vertex color preview. Installed straight on the instance
	// through RenderingServer instead of MeshInstance3D::set_material_override()
	// so it never becomes part of the scene and can never be saved.
	Ref<Shader> preview_shader;
	Ref<ShaderMaterial> preview_material;
	bool preview_installed = false;

	// Active stroke (mouse held down).
	bool stroke_active = false;
	uint64_t last_stamp_msec = 0;

	void _mode_pressed(int p_mode);
	void _paint_toggled(bool p_pressed);
	void _channel_toggled(int p_channel);
	void _view_mode_selected(int p_index);
	void _set_brush_color(const Color &p_color);
	void _set_brush_radius(double p_value);
	void _set_brush_strength(double p_value);
	void _set_brush_hardness(double p_value);
	void _update_toolbar();

	void _set_paint_enabled(bool p_enabled);
	bool _prepare_target();
	void _convert_to_array_mesh();
	void _make_mesh_unique();
	void _paint_external_anyway();
	void _external_dialog_custom_action(const StringName &p_action);
	bool _is_mesh_external(const Ref<Mesh> &p_mesh) const;

	bool _ensure_vertex_colors();
	void _build_cache();
	void _invalidate_cache();
	bool _is_cache_current() const;
	bool _ensure_cache_current();

	void _update_preview();
	void _remove_preview();

	void _ensure_cursor_instance();
	void _update_cursor(const Vector3 &p_position, const Vector3 &p_normal, bool p_visible);

	float _brush_weight(float p_distance) const;
	Color _get_group_color(int p_group) const;
	Color _blend_channels(const Color &p_dst, const Color &p_src, float p_weight) const;
	void _write_group_color(int p_group, const Color &p_color);
	void _flush_dirty_surfaces();

	bool _raycast(const Vector3 &p_from, const Vector3 &p_dir, float p_max_dist, Vector3 &r_position, Vector3 &r_normal) const;

	void _stamp(const Vector3 &p_world_position);
	void _fill_all(const Color &p_color, bool p_respect_channels, const String &p_action_name);
	void _fill_pressed();
	void _clear_pressed();

	void _begin_stroke();
	void _end_stroke();
	void _cancel_stroke();

	bool _do_input_action(Camera3D *p_camera, const Point2 &p_screen_pos, bool p_click);

	void _apply_vertex_colors(const Ref<ArrayMesh> &p_mesh, int p_surface, const PackedInt32Array &p_indices, const PackedColorArray &p_colors);

protected:
	static void _bind_methods();

public:
	virtual String get_plugin_name() const override { return "VertexPaint"; }
	virtual bool handles(Object *p_object) const override;
	virtual void edit(Object *p_object) override;
	virtual void make_visible(bool p_visible) override;
	virtual EditorPlugin::AfterGUIInput forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) override;

	VertexPaint3DEditorPlugin();
	~VertexPaint3DEditorPlugin();
};
