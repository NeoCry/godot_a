/**************************************************************************/
/*  terrain_3d_editor_plugin.cpp                                          */
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

#include "terrain_3d_editor_plugin.h"

#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/physics/static_body_3d.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/menu_button.h"
#include "scene/gui/separator.h"
#include "scene/gui/spin_box.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "scene/scene_string_names.h"
#include "servers/physics_3d/physics_server_3d.h"
#include "servers/rendering/rendering_server.h"

void Terrain3DEditorPlugin::_bind_methods() {
}

bool Terrain3DEditorPlugin::handles(Object *p_object) const {
	return Object::cast_to<Terrain3D>(p_object) != nullptr;
}

void Terrain3DEditorPlugin::edit(Object *p_object) {
	_end_stroke();

	terrain = Object::cast_to<Terrain3D>(p_object);

	if (cursor_instance.is_valid()) {
		RS::get_singleton()->instance_set_visible(cursor_instance, false);
	}

	_rebuild_paint_layer_menu();
}

void Terrain3DEditorPlugin::make_visible(bool p_visible) {
	if (p_visible) {
		topmenu_bar->show();
	} else {
		_end_stroke();
		topmenu_bar->hide();
		terrain = nullptr;
		if (cursor_instance.is_valid()) {
			RS::get_singleton()->instance_set_visible(cursor_instance, false);
		}
	}
}

void Terrain3DEditorPlugin::_mode_pressed(int p_mode) {
	_end_stroke();
	mode = (Mode)p_mode;
}

void Terrain3DEditorPlugin::_set_brush_radius(double p_value) {
	brush_radius = MAX(0.01f, (float)p_value);
}

void Terrain3DEditorPlugin::_set_brush_strength(double p_value) {
	brush_strength = MAX(0.001f, (float)p_value);
}

void Terrain3DEditorPlugin::_rebuild_paint_layer_menu() {
	PopupMenu *popup = paint_layer_menu->get_popup();
	popup->clear();

	if (terrain == nullptr) {
		paint_layer_menu->set_text(TTR("No Layers"));
		return;
	}

	const TypedArray<TerrainLayer> layers = terrain->get_layers();
	if (layers.is_empty()) {
		paint_layer_menu->set_text(TTR("No Layers"));
		return;
	}

	paint_layer_index = CLAMP(paint_layer_index, 0, layers.size() - 1);
	String current_name;
	for (int i = 0; i < layers.size(); i++) {
		Ref<TerrainLayer> layer = layers[i];
		const String name = (layer.is_valid() && !layer->get_layer_name().is_empty()) ? layer->get_layer_name() : vformat("Layer %d", i);
		popup->add_radio_check_item(name, i);
		popup->set_item_checked(i, i == paint_layer_index);
		if (i == paint_layer_index) {
			current_name = name;
		}
	}
	paint_layer_menu->set_text(vformat(TTR("Layer: %s"), current_name));
}

void Terrain3DEditorPlugin::_paint_layer_menu_id_pressed(int p_id) {
	paint_layer_index = p_id;
	_rebuild_paint_layer_menu();
}

bool Terrain3DEditorPlugin::_is_control_mode() const {
	return mode == MODE_PAINT || mode == MODE_HOLE || mode == MODE_UNHOLE;
}

String Terrain3DEditorPlugin::_get_mode_action_name() const {
	switch (mode) {
		case MODE_PAINT:
			return TTR("Paint Terrain Layer");
		case MODE_HOLE:
			return TTR("Cut Terrain Hole");
		case MODE_UNHOLE:
			return TTR("Fill Terrain Hole");
		default:
			return TTR("Sculpt Terrain");
	}
}

Rect2i Terrain3DEditorPlugin::_get_brush_vertex_region(const Vector3 &p_local_position, float p_radius) const {
	Ref<TerrainData> terrain_data = terrain->get_terrain_data();
	if (terrain_data.is_null()) {
		return Rect2i();
	}
	const float spacing = terrain_data->get_vertex_spacing();
	const int resolution = terrain_data->get_resolution();
	const int rad_idx = (int)Math::ceil(MAX(p_radius, 0.001f) / spacing) + 1;
	const int cx = (int)Math::round(p_local_position.x / spacing);
	const int cz = (int)Math::round(p_local_position.z / spacing);
	const int x0 = CLAMP(cx - rad_idx, 0, resolution - 1);
	const int x1 = CLAMP(cx + rad_idx, 0, resolution - 1);
	const int z0 = CLAMP(cz - rad_idx, 0, resolution - 1);
	const int z1 = CLAMP(cz + rad_idx, 0, resolution - 1);
	if (x1 < x0 || z1 < z0) {
		return Rect2i();
	}
	return Rect2i(x0, z0, x1 - x0 + 1, z1 - z0 + 1);
}

void Terrain3DEditorPlugin::_snapshot_chunk_if_needed(const Vector2i &p_chunk) {
	if (touched_regions.has(p_chunk)) {
		return;
	}
	TouchedChunkRegion snap;
	snap.region = Rect2i(p_chunk.x * Terrain3D::CHUNK_QUADS, p_chunk.y * Terrain3D::CHUNK_QUADS, Terrain3D::CHUNK_QUADS + 1, Terrain3D::CHUNK_QUADS + 1);
	if (_is_control_mode()) {
		snap.before_control = terrain->get_control_region(snap.region);
	} else {
		snap.before_heights = terrain->get_height_region(snap.region);
	}
	touched_regions[p_chunk] = snap;
}

void Terrain3DEditorPlugin::_snapshot_region_chunks(const Rect2i &p_vertex_region) {
	if (p_vertex_region.size.x <= 0 || p_vertex_region.size.y <= 0) {
		return;
	}
	const int cx0 = (int)Math::floor((float)p_vertex_region.position.x / Terrain3D::CHUNK_QUADS);
	const int cz0 = (int)Math::floor((float)p_vertex_region.position.y / Terrain3D::CHUNK_QUADS);
	const int cx1 = (int)Math::floor((float)(p_vertex_region.position.x + p_vertex_region.size.x - 1) / Terrain3D::CHUNK_QUADS);
	const int cz1 = (int)Math::floor((float)(p_vertex_region.position.y + p_vertex_region.size.y - 1) / Terrain3D::CHUNK_QUADS);
	for (int cz = cz0; cz <= cz1; cz++) {
		for (int cx = cx0; cx <= cx1; cx++) {
			_snapshot_chunk_if_needed(Vector2i(cx, cz));
		}
	}
}

bool Terrain3DEditorPlugin::_raycast_terrain(Camera3D *p_camera, const Point2 &p_screen_pos, Vector3 &r_local_position, Vector3 &r_world_position, Vector3 &r_world_normal) {
	if (terrain == nullptr || !terrain->is_inside_tree() || terrain->get_collision_body() == nullptr) {
		return false;
	}
	Ref<World3D> w3d = terrain->get_world_3d();
	if (w3d.is_null()) {
		return false;
	}
	PhysicsDirectSpaceState3D *dss = PhysicsServer3D::get_singleton()->space_get_direct_state(w3d->get_space());
	if (dss == nullptr) {
		return false;
	}

	const Vector3 ray_from = p_camera->project_ray_origin(p_screen_pos);
	const Vector3 ray_dir = p_camera->project_ray_normal(p_screen_pos);

	PS3DT::RayParameters params;
	params.from = ray_from;
	params.to = ray_from + ray_dir * p_camera->get_far();
	params.collide_with_bodies = true;
	params.collide_with_areas = false;

	PS3DT::RayResult result;
	if (!dss->intersect_ray(params, result)) {
		return false;
	}
	if (result.rid != terrain->get_collision_body()->get_rid()) {
		return false;
	}

	r_world_position = result.position;
	r_world_normal = result.normal;
	r_local_position = terrain->get_global_transform().affine_inverse().xform(result.position);
	return true;
}

void Terrain3DEditorPlugin::_ensure_cursor_instance() {
	if (cursor_mesh.is_valid()) {
		return;
	}

	cursor_mesh = RS::get_singleton()->mesh_create();

	const int segments = 64;
	Vector<Vector3> points;
	points.resize(segments * 2);
	for (int i = 0; i < segments; i++) {
		const float a0 = (float)i / segments * (float)Math::TAU;
		const float a1 = (float)(i + 1) / segments * (float)Math::TAU;
		points.set(i * 2, Vector3(Math::cos(a0), 0.0f, Math::sin(a0)));
		points.set(i * 2 + 1, Vector3(Math::cos(a1), 0.0f, Math::sin(a1)));
	}

	Array d;
	d.resize(RSE::ARRAY_MAX);
	d[RSE::ARRAY_VERTEX] = points;
	RS::get_singleton()->mesh_add_surface_from_arrays(cursor_mesh, RSE::PRIMITIVE_LINES, d);

	// Must be kept alive on the instance (see the member's comment), not just
	// a local Ref, or its RID would be freed while the surface still used it.
	cursor_material.instantiate();
	cursor_material->set_shading_mode(StandardMaterial3D::SHADING_MODE_UNSHADED);
	cursor_material->set_transparency(StandardMaterial3D::TRANSPARENCY_ALPHA);
	cursor_material->set_flag(StandardMaterial3D::FLAG_DISABLE_DEPTH_TEST, true);
	cursor_material->set_albedo(Color(0.2, 0.85, 1.0, 0.9));
	cursor_material->set_render_priority(Material::RENDER_PRIORITY_MAX);
	RS::get_singleton()->mesh_surface_set_material(cursor_mesh, 0, cursor_material->get_rid());
}

void Terrain3DEditorPlugin::_update_cursor(const Vector3 &p_world_position, const Vector3 &p_world_normal, bool p_visible) {
	if (terrain == nullptr) {
		p_visible = false;
	}

	_ensure_cursor_instance();

	if (p_visible && !cursor_instance.is_valid()) {
		const RID scenario = terrain->get_world_3d().is_valid() ? terrain->get_world_3d()->get_scenario() : RID();
		cursor_instance = RS::get_singleton()->instance_create2(cursor_mesh, scenario);
	}

	if (!cursor_instance.is_valid()) {
		return;
	}

	if (p_visible) {
		const RID scenario = terrain->get_world_3d().is_valid() ? terrain->get_world_3d()->get_scenario() : RID();
		RS::get_singleton()->instance_set_scenario(cursor_instance, scenario);

		Vector3 up = p_world_normal;
		if (up.length_squared() < 0.0001f) {
			up = Vector3(0, 1, 0);
		} else {
			up.normalize();
		}
		Basis basis;
		basis.rotate_to_align(Vector3(0, 1, 0), up);
		basis = basis.scaled_local(Vector3(brush_radius, brush_radius, brush_radius));
		RS::get_singleton()->instance_set_transform(cursor_instance, Transform3D(basis, p_world_position));
	}

	RS::get_singleton()->instance_set_visible(cursor_instance, p_visible);
}

void Terrain3DEditorPlugin::_stamp(const Vector3 &p_local_position) {
	if (terrain == nullptr) {
		return;
	}
	if (mode == MODE_PAINT && terrain->get_layers().is_empty()) {
		return;
	}

	_snapshot_region_chunks(_get_brush_vertex_region(p_local_position, brush_radius));

	switch (mode) {
		case MODE_RAISE: {
			terrain->sculpt(p_local_position, brush_radius, brush_strength, Terrain3D::SCULPT_RAISE, 0.0f, false);
		} break;
		case MODE_LOWER: {
			terrain->sculpt(p_local_position, brush_radius, brush_strength, Terrain3D::SCULPT_LOWER, 0.0f, false);
		} break;
		case MODE_SMOOTH: {
			terrain->sculpt(p_local_position, brush_radius, brush_strength, Terrain3D::SCULPT_SMOOTH, 0.0f, false);
		} break;
		case MODE_FLATTEN: {
			terrain->sculpt(p_local_position, brush_radius, brush_strength, Terrain3D::SCULPT_FLATTEN, stroke_flatten_height, false);
		} break;
		case MODE_PAINT: {
			terrain->paint_layer(p_local_position, brush_radius, brush_strength, paint_layer_index);
		} break;
		case MODE_HOLE: {
			terrain->set_hole(p_local_position, brush_radius, true, false);
		} break;
		case MODE_UNHOLE: {
			terrain->set_hole(p_local_position, brush_radius, false, false);
		} break;
	}
}

void Terrain3DEditorPlugin::_begin_stroke() {
	stroke_active = true;
	touched_regions.clear();
	last_stamp_msec = 0;
}

void Terrain3DEditorPlugin::_end_stroke() {
	if (!stroke_active) {
		return;
	}

	if (terrain != nullptr && !touched_regions.is_empty()) {
		const bool control_mode = _is_control_mode();
		EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
		ur->create_action(_get_mode_action_name());
		for (KeyValue<Vector2i, TouchedChunkRegion> &kv : touched_regions) {
			const TouchedChunkRegion &snap = kv.value;
			if (control_mode) {
				const PackedColorArray after = terrain->get_control_region(snap.region);
				ur->add_do_method(terrain, "set_control_region", snap.region, after);
				ur->add_undo_method(terrain, "set_control_region", snap.region, snap.before_control);
			} else {
				const PackedFloat32Array after = terrain->get_height_region(snap.region);
				ur->add_do_method(terrain, "set_height_region", snap.region, after, true);
				ur->add_undo_method(terrain, "set_height_region", snap.region, snap.before_heights, true);
			}
		}
		ur->commit_action(false);

		if (!control_mode) {
			terrain->update_collision();
		}
	}

	stroke_active = false;
	touched_regions.clear();
}

void Terrain3DEditorPlugin::_cancel_stroke() {
	if (stroke_active && terrain != nullptr) {
		const bool control_mode = _is_control_mode();
		for (KeyValue<Vector2i, TouchedChunkRegion> &kv : touched_regions) {
			if (control_mode) {
				terrain->set_control_region(kv.value.region, kv.value.before_control);
			} else {
				terrain->set_height_region(kv.value.region, kv.value.before_heights, true);
			}
		}
	}
	stroke_active = false;
	touched_regions.clear();
}

bool Terrain3DEditorPlugin::_do_input_action(Camera3D *p_camera, const Point2 &p_screen_pos, bool p_click) {
	Vector3 local_pos, world_pos, world_normal;
	const bool hit = _raycast_terrain(p_camera, p_screen_pos, local_pos, world_pos, world_normal);

	_update_cursor(world_pos, world_normal, hit);

	if (!hit || !stroke_active) {
		return hit;
	}

	const uint64_t now = OS::get_singleton()->get_ticks_msec();
	const uint64_t stamp_interval_msec = 60;
	if (!p_click && (now - last_stamp_msec) < stamp_interval_msec) {
		return true;
	}
	last_stamp_msec = now;

	_stamp(local_pos);
	return true;
}

EditorPlugin::AfterGUIInput Terrain3DEditorPlugin::forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) {
	if (terrain == nullptr || !terrain->is_inside_tree()) {
		return EditorPlugin::AFTER_GUI_INPUT_PASS;
	}

	Ref<InputEventKey> k = p_event;
	if (k.is_valid() && k->is_pressed() && !k->is_echo() && k->get_keycode() == Key::ESCAPE && stroke_active) {
		_cancel_stroke();
		_update_cursor(Vector3(), Vector3(0, 1, 0), false);
		return EditorPlugin::AFTER_GUI_INPUT_STOP;
	}

	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->get_button_index() == MouseButton::LEFT) {
		if (mb->is_pressed()) {
			if (mode == MODE_FLATTEN) {
				Vector3 local_pos, world_pos, world_normal;
				if (_raycast_terrain(p_camera, mb->get_position(), local_pos, world_pos, world_normal)) {
					stroke_flatten_height = local_pos.y;
				}
			}
			_begin_stroke();
			_do_input_action(p_camera, mb->get_position(), true);
			return EditorPlugin::AFTER_GUI_INPUT_STOP;
		} else if (stroke_active) {
			_end_stroke();
			return EditorPlugin::AFTER_GUI_INPUT_STOP;
		}
	}

	Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid()) {
		// Always refresh the brush cursor preview on hover, but only actually
		// claim the event while a stroke is being dragged (left button held).
		// Otherwise orbit/pan/freelook (driven by mouse motion with other
		// buttons held) would get blocked by this tool.
		_do_input_action(p_camera, mm->get_position(), false);
		if (stroke_active) {
			return EditorPlugin::AFTER_GUI_INPUT_STOP;
		}
		return EditorPlugin::AFTER_GUI_INPUT_PASS;
	}

	return EditorPlugin::AFTER_GUI_INPUT_PASS;
}

Terrain3DEditorPlugin::Terrain3DEditorPlugin() {
	topmenu_bar = memnew(HBoxContainer);
	topmenu_bar->hide();

	toolbar = memnew(HBoxContainer);
	topmenu_bar->add_child(toolbar);

	mode_button_group.instantiate();

	mode_raise_button = memnew(Button);
	mode_raise_button->set_toggle_mode(true);
	mode_raise_button->set_button_group(mode_button_group);
	mode_raise_button->set_pressed(true);
	mode_raise_button->set_text(TTR("Raise"));
	mode_raise_button->set_tooltip_text(TTR("Raise the terrain height within the brush."));
	toolbar->add_child(mode_raise_button);
	mode_raise_button->connect(SceneStringName(pressed), callable_mp(this, &Terrain3DEditorPlugin::_mode_pressed).bind((int)MODE_RAISE));

	mode_lower_button = memnew(Button);
	mode_lower_button->set_toggle_mode(true);
	mode_lower_button->set_button_group(mode_button_group);
	mode_lower_button->set_text(TTR("Lower"));
	mode_lower_button->set_tooltip_text(TTR("Lower the terrain height within the brush."));
	toolbar->add_child(mode_lower_button);
	mode_lower_button->connect(SceneStringName(pressed), callable_mp(this, &Terrain3DEditorPlugin::_mode_pressed).bind((int)MODE_LOWER));

	mode_smooth_button = memnew(Button);
	mode_smooth_button->set_toggle_mode(true);
	mode_smooth_button->set_button_group(mode_button_group);
	mode_smooth_button->set_text(TTR("Smooth"));
	mode_smooth_button->set_tooltip_text(TTR("Average the terrain height with its neighbors within the brush."));
	toolbar->add_child(mode_smooth_button);
	mode_smooth_button->connect(SceneStringName(pressed), callable_mp(this, &Terrain3DEditorPlugin::_mode_pressed).bind((int)MODE_SMOOTH));

	mode_flatten_button = memnew(Button);
	mode_flatten_button->set_toggle_mode(true);
	mode_flatten_button->set_button_group(mode_button_group);
	mode_flatten_button->set_text(TTR("Flatten"));
	mode_flatten_button->set_tooltip_text(TTR("Flatten the terrain within the brush towards the height at the start of the stroke."));
	toolbar->add_child(mode_flatten_button);
	mode_flatten_button->connect(SceneStringName(pressed), callable_mp(this, &Terrain3DEditorPlugin::_mode_pressed).bind((int)MODE_FLATTEN));

	mode_paint_button = memnew(Button);
	mode_paint_button->set_toggle_mode(true);
	mode_paint_button->set_button_group(mode_button_group);
	mode_paint_button->set_text(TTR("Paint"));
	mode_paint_button->set_tooltip_text(TTR("Paint the selected texture layer within the brush."));
	toolbar->add_child(mode_paint_button);
	mode_paint_button->connect(SceneStringName(pressed), callable_mp(this, &Terrain3DEditorPlugin::_mode_pressed).bind((int)MODE_PAINT));

	mode_hole_button = memnew(Button);
	mode_hole_button->set_toggle_mode(true);
	mode_hole_button->set_button_group(mode_button_group);
	mode_hole_button->set_text(TTR("Hole"));
	mode_hole_button->set_tooltip_text(TTR("Cut a hole in the terrain within the brush."));
	toolbar->add_child(mode_hole_button);
	mode_hole_button->connect(SceneStringName(pressed), callable_mp(this, &Terrain3DEditorPlugin::_mode_pressed).bind((int)MODE_HOLE));

	mode_unhole_button = memnew(Button);
	mode_unhole_button->set_toggle_mode(true);
	mode_unhole_button->set_button_group(mode_button_group);
	mode_unhole_button->set_text(TTR("Unhole"));
	mode_unhole_button->set_tooltip_text(TTR("Fill in holes within the brush."));
	toolbar->add_child(mode_unhole_button);
	mode_unhole_button->connect(SceneStringName(pressed), callable_mp(this, &Terrain3DEditorPlugin::_mode_pressed).bind((int)MODE_UNHOLE));

	toolbar->add_child(memnew(VSeparator));

	Label *radius_label = memnew(Label);
	radius_label->set_text(TTR("Radius:"));
	toolbar->add_child(radius_label);

	brush_radius_spin = memnew(SpinBox);
	brush_radius_spin->set_min(0.1);
	brush_radius_spin->set_max(10000.0);
	brush_radius_spin->set_step(0.1);
	brush_radius_spin->set_value(brush_radius);
	brush_radius_spin->set_tooltip_text(TTR("Brush radius, in meters."));
	toolbar->add_child(brush_radius_spin);
	brush_radius_spin->connect(SceneStringName(value_changed), callable_mp(this, &Terrain3DEditorPlugin::_set_brush_radius));

	Label *strength_label = memnew(Label);
	strength_label->set_text(TTR("Strength:"));
	toolbar->add_child(strength_label);

	brush_strength_spin = memnew(SpinBox);
	brush_strength_spin->set_min(0.001);
	brush_strength_spin->set_max(1000.0);
	brush_strength_spin->set_step(0.001);
	brush_strength_spin->set_value(brush_strength);
	brush_strength_spin->set_tooltip_text(TTR("Effect applied per brush stamp: meters of height change for Raise/Lower/Smooth/Flatten, or blend amount (0-1 is typical) for Paint/Hole/Unhole."));
	toolbar->add_child(brush_strength_spin);
	brush_strength_spin->connect(SceneStringName(value_changed), callable_mp(this, &Terrain3DEditorPlugin::_set_brush_strength));

	toolbar->add_child(memnew(VSeparator));

	paint_layer_menu = memnew(MenuButton);
	paint_layer_menu->set_text(TTR("No Layers"));
	paint_layer_menu->set_tooltip_text(TTR("Choose which TerrainLayer the Paint brush applies."));
	paint_layer_menu->get_popup()->connect(SceneStringName(id_pressed), callable_mp(this, &Terrain3DEditorPlugin::_paint_layer_menu_id_pressed));
	paint_layer_menu->connect("about_to_popup", callable_mp(this, &Terrain3DEditorPlugin::_rebuild_paint_layer_menu));
	toolbar->add_child(paint_layer_menu);

	Node3DEditor::get_singleton()->add_control_to_menu_panel(topmenu_bar);
}

Terrain3DEditorPlugin::~Terrain3DEditorPlugin() {
	if (cursor_instance.is_valid()) {
		RS::get_singleton()->free_rid(cursor_instance);
	}
	if (cursor_mesh.is_valid()) {
		RS::get_singleton()->free_rid(cursor_mesh);
	}
}
