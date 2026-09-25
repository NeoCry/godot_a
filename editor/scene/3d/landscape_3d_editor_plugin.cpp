/**************************************************************************/
/*  landscape_3d_editor_plugin.cpp                                        */
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

#include "landscape_3d_editor_plugin.h"

#include "core/io/image.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/landscape_spline_3d.h"
#include "scene/3d/physics/static_body_3d.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/label.h"
#include "scene/gui/menu_button.h"
#include "scene/gui/option_button.h"
#include "scene/gui/separator.h"
#include "scene/gui/spin_box.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "scene/scene_string_names.h"
#include "servers/physics_3d/physics_server_3d.h"
#include "servers/rendering/rendering_server.h"

void Landscape3DEditorPlugin::_bind_methods() {
}

bool Landscape3DEditorPlugin::handles(Object *p_object) const {
	return Object::cast_to<Landscape3D>(p_object) != nullptr;
}

void Landscape3DEditorPlugin::edit(Object *p_object) {
	_end_stroke();

	terrain = Object::cast_to<Landscape3D>(p_object);

	if (cursor_instance.is_valid()) {
		RS::get_singleton()->instance_set_visible(cursor_instance, false);
	}

	_rebuild_paint_layer_menu();
}

void Landscape3DEditorPlugin::make_visible(bool p_visible) {
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

void Landscape3DEditorPlugin::_mode_pressed(int p_mode) {
	_end_stroke();
	mode = (Mode)p_mode;
}

void Landscape3DEditorPlugin::_set_brush_radius(double p_value) {
	brush_radius = MAX(0.01f, (float)p_value);
}

void Landscape3DEditorPlugin::_set_brush_strength(double p_value) {
	brush_strength = MAX(0.001f, (float)p_value);
}

void Landscape3DEditorPlugin::_set_brush_falloff(double p_value) {
	brush_falloff = CLAMP((float)p_value, 0.0f, 1.0f);
}

void Landscape3DEditorPlugin::_rebuild_paint_layer_menu() {
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

void Landscape3DEditorPlugin::_paint_layer_menu_id_pressed(int p_id) {
	paint_layer_index = p_id;
	_rebuild_paint_layer_menu();
}

void Landscape3DEditorPlugin::_add_spline_menu_id_pressed(int p_id) {
	Node *scene_root = EditorNode::get_singleton()->get_edited_scene();
	if (terrain == nullptr || scene_root == nullptr) {
		return;
	}
	ERR_FAIL_INDEX(p_id, LandscapeSpline3D::TYPE_MAX);
	const LandscapeSpline3D::SplineType type = (LandscapeSpline3D::SplineType)p_id;

	LandscapeSpline3D *spline = memnew(LandscapeSpline3D);
	spline->apply_preset(type);
	// An empty curve rather than none, so the Path3D tools can start adding
	// points straight away instead of first asking to create one.
	Ref<Curve3D> curve;
	curve.instantiate();
	spline->set_curve(curve);
	static const char *names[LandscapeSpline3D::TYPE_MAX] = { "Road", "River", "Stream" };
	spline->set_name(names[type]);

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(vformat(TTR("Add %s"), names[type]));
	ur->add_do_method(terrain, "add_child", spline, true);
	ur->add_do_method(spline, "set_owner", scene_root);
	ur->add_do_reference(spline);
	ur->add_undo_method(terrain, "remove_child", spline);
	ur->commit_action();

	EditorSelection *selection = EditorNode::get_singleton()->get_editor_selection();
	selection->clear();
	selection->add_node(spline);
}

void Landscape3DEditorPlugin::_import_heightmap_pressed() {
	if (terrain == nullptr) {
		return;
	}
	import_file_dialog->popup_file_dialog();
}

void Landscape3DEditorPlugin::_import_file_selected(const String &p_path) {
	pending_import_path = p_path;
	import_height_range_dialog->popup_centered();
}

namespace {
// EXR/HDR decode to one of these (real float or half-float height data, or
// HDR's shared-exponent RGBE); every other loadable format (PNG included,
// since Godot's PNG loader always decodes to 8 bits per channel even for a
// 16-bit source file) only has 256 representable values per channel.
bool _is_high_precision_image_format(Image::Format p_format) {
	switch (p_format) {
		case Image::FORMAT_RF:
		case Image::FORMAT_RGF:
		case Image::FORMAT_RGBF:
		case Image::FORMAT_RGBAF:
		case Image::FORMAT_RH:
		case Image::FORMAT_RGH:
		case Image::FORMAT_RGBH:
		case Image::FORMAT_RGBAH:
		case Image::FORMAT_RGBE9995:
			return true;
		default:
			return false;
	}
}
} // namespace

void Landscape3DEditorPlugin::_do_import_heightmap() {
	if (terrain == nullptr || pending_import_path.is_empty()) {
		return;
	}

	Ref<Image> image = Image::load_from_file(pending_import_path);
	if (image.is_null()) {
		EditorNode::get_singleton()->show_warning(vformat(TTR("Could not load \"%s\" as an image."), pending_import_path));
		return;
	}
	if (image->get_width() != image->get_height()) {
		EditorNode::get_singleton()->show_warning(TTR("The heightmap image must be square (its width must equal its height)."));
		return;
	}

	const float height_min = import_height_min_spin->get_value();
	const float height_max = import_height_max_spin->get_value();

	if (!_is_high_precision_image_format(image->get_format())) {
		const float step = (height_max - height_min) / 255.0f;
		EditorNode::get_singleton()->show_warning(
				vformat(TTR("\"%s\" only has 8-bit precision (256 possible height values). Across the %.2f m range you set, that means about %.4f m between adjacent representable heights, which can show up as visible stepping/terracing. For smooth results, use an EXR or HDR heightmap instead, which store real height precision rather than quantizing to 8 bits."), pending_import_path.get_file(), height_max - height_min, step),
				TTR("Low Heightmap Precision"));
	}

	// A brand-new terrain has nothing to sculpt into yet; create the resource
	// it needs rather than making the user do that by hand first.
	if (terrain->get_terrain_data().is_null()) {
		Ref<TerrainData> new_data;
		new_data.instantiate();
		terrain->set_terrain_data(new_data);
	}

	terrain->get_terrain_data()->import_heightmap(image, height_min, height_max);
}

void Landscape3DEditorPlugin::_import_layer_mask_pressed() {
	if (terrain == nullptr) {
		return;
	}
	if (terrain->get_layers().is_empty()) {
		EditorNode::get_singleton()->show_warning(TTR("This Landscape3D has no TerrainLayers yet. Add at least one layer before importing a mask for it."));
		return;
	}
	mask_file_dialog->popup_file_dialog();
}

bool Landscape3DEditorPlugin::_fill_layer_option(OptionButton *p_option) {
	// The layer list is whatever the terrain holds right now, so it is filled
	// in on every popup rather than once at startup (same reason
	// _rebuild_paint_layer_menu runs on every popup).
	p_option->clear();
	if (terrain == nullptr) {
		return false;
	}
	const TypedArray<TerrainLayer> layers = terrain->get_layers();
	if (layers.is_empty()) {
		return false;
	}
	for (int i = 0; i < layers.size(); i++) {
		Ref<TerrainLayer> layer = layers[i];
		const String name = (layer.is_valid() && !layer->get_layer_name().is_empty()) ? layer->get_layer_name() : vformat("Layer %d", i);
		p_option->add_item(vformat("%d: %s", i, name), i);
	}
	p_option->select(CLAMP(paint_layer_index, 0, layers.size() - 1));
	return true;
}

void Landscape3DEditorPlugin::_mask_file_selected(const String &p_path) {
	// The file dialog is its own window, so the selected node (and its layers)
	// can change while it is open.
	if (!_fill_layer_option(mask_layer_option)) {
		return;
	}
	pending_mask_path = p_path;
	mask_options_dialog->popup_centered();
}

void Landscape3DEditorPlugin::_do_import_layer_mask() {
	if (terrain == nullptr || pending_mask_path.is_empty()) {
		return;
	}
	Ref<TerrainData> terrain_data = terrain->get_terrain_data();
	if (terrain_data.is_null()) {
		EditorNode::get_singleton()->show_warning(TTR("This Landscape3D has no TerrainData to import a mask into. Import a heightmap first, or assign a TerrainData resource."));
		return;
	}

	Ref<Image> image = Image::load_from_file(pending_mask_path);
	if (image.is_null()) {
		EditorNode::get_singleton()->show_warning(vformat(TTR("Could not load \"%s\" as an image."), pending_mask_path));
		return;
	}
	if (image->get_width() != image->get_height()) {
		EditorNode::get_singleton()->show_warning(TTR("The mask image must be square (its width must equal its height)."));
		return;
	}

	const int layer_index = mask_layer_option->get_selected_id();
	if (layer_index < 0 || layer_index >= terrain->get_layers().size()) {
		return;
	}

	terrain_data->import_layer_mask(image, layer_index,
			(TerrainData::MaskChannel)mask_channel_option->get_selected_id(),
			mask_normalize_check->is_pressed());
}

void Landscape3DEditorPlugin::_generate_layer_mask_pressed() {
	if (terrain == nullptr) {
		return;
	}
	Ref<TerrainData> terrain_data = terrain->get_terrain_data();
	if (terrain_data.is_null()) {
		EditorNode::get_singleton()->show_warning(TTR("This Landscape3D has no TerrainData to generate a mask from. Import or sculpt a heightmap first."));
		return;
	}
	if (!_fill_layer_option(generate_layer_option)) {
		EditorNode::get_singleton()->show_warning(TTR("This Landscape3D has no TerrainLayers yet. Add at least one layer before generating a mask for it."));
		return;
	}

	// The height band is meaningless without knowing what the terrain actually
	// spans, and nothing else in the editor shows that, so the dialog states it
	// and starts the band on it rather than on numbers picked out of the air.
	const int resolution = terrain_data->get_resolution();
	const PackedFloat32Array heights = terrain_data->get_height_region(Rect2i(0, 0, resolution, resolution));
	float lowest = 0.0f;
	float highest = 0.0f;
	if (!heights.is_empty()) {
		lowest = heights[0];
		highest = heights[0];
		for (int i = 1; i < heights.size(); i++) {
			lowest = MIN(lowest, heights[i]);
			highest = MAX(highest, heights[i]);
		}
	}
	generate_range_label->set_text(vformat(TTR("This terrain spans %.2f m to %.2f m."), lowest, highest));
	generate_height_min_spin->set_value(lowest);
	generate_height_max_spin->set_value(highest);
	_update_curvature_range_label();

	generate_mask_dialog->popup_centered();
}

void Landscape3DEditorPlugin::_update_curvature_range_label(double p_unused) {
	if (terrain == nullptr) {
		return;
	}
	Ref<TerrainData> terrain_data = terrain->get_terrain_data();
	if (terrain_data.is_null()) {
		return;
	}

	const int resolution = terrain_data->get_resolution();
	const int radius = (int)generate_curvature_radius_spin->get_value();
	float most_hollow = 0.0f;
	float most_raised = 0.0f;
	for (int z = 0; z < resolution; z++) {
		for (int x = 0; x < resolution; x++) {
			const float c = terrain_data->get_curvature(x, z, radius);
			most_hollow = MIN(most_hollow, c);
			most_raised = MAX(most_raised, c);
		}
	}
	generate_curvature_range_label->set_text(vformat(TTR("At this radius the terrain runs %.4f (deepest hollow) to %.4f (sharpest rise)."), most_hollow, most_raised));
}

void Landscape3DEditorPlugin::_do_generate_layer_mask() {
	if (terrain == nullptr) {
		return;
	}
	Ref<TerrainData> terrain_data = terrain->get_terrain_data();
	if (terrain_data.is_null()) {
		return;
	}
	const int layer_index = generate_layer_option->get_selected_id();
	if (layer_index < 0 || layer_index >= terrain->get_layers().size()) {
		return;
	}

	terrain_data->generate_layer_mask(layer_index,
			generate_height_min_spin->get_value(), generate_height_max_spin->get_value(), generate_height_falloff_spin->get_value(),
			generate_slope_min_spin->get_value(), generate_slope_max_spin->get_value(), generate_slope_falloff_spin->get_value(),
			generate_curvature_min_spin->get_value(), generate_curvature_max_spin->get_value(), generate_curvature_falloff_spin->get_value(),
			(int)generate_curvature_radius_spin->get_value(),
			generate_normalize_check->is_pressed());
}

Landscape3DEditorPlugin::DataKind Landscape3DEditorPlugin::_get_mode_data_kind() const {
	switch (mode) {
		case MODE_PAINT:
			return DataKind::WEIGHTS;
		case MODE_HOLE:
		case MODE_UNHOLE:
			return DataKind::HOLE;
		default:
			return DataKind::HEIGHT;
	}
}

String Landscape3DEditorPlugin::_get_mode_action_name() const {
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

Rect2i Landscape3DEditorPlugin::_get_brush_vertex_region(const Vector3 &p_local_position, float p_radius) const {
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

void Landscape3DEditorPlugin::_snapshot_chunk_if_needed(const Vector2i &p_chunk) {
	if (touched_regions.has(p_chunk)) {
		return;
	}
	TouchedChunkRegion snap;
	snap.region = Rect2i(p_chunk.x * Landscape3D::CHUNK_QUADS, p_chunk.y * Landscape3D::CHUNK_QUADS, Landscape3D::CHUNK_QUADS + 1, Landscape3D::CHUNK_QUADS + 1);
	switch (_get_mode_data_kind()) {
		case DataKind::WEIGHTS: {
			const int layer_count = terrain->get_layers().size();
			snap.before_weights.resize(layer_count);
			for (int i = 0; i < layer_count; i++) {
				snap.before_weights.write[i] = terrain->get_layer_weight_region(snap.region, i);
			}
		} break;
		case DataKind::HOLE: {
			snap.before_holes = terrain->get_hole_region(snap.region);
		} break;
		case DataKind::HEIGHT: {
			snap.before_heights = terrain->get_height_region(snap.region);
		} break;
	}
	touched_regions[p_chunk] = snap;
}

void Landscape3DEditorPlugin::_snapshot_region_chunks(const Rect2i &p_vertex_region) {
	if (p_vertex_region.size.x <= 0 || p_vertex_region.size.y <= 0) {
		return;
	}
	const int cx0 = (int)Math::floor((float)p_vertex_region.position.x / Landscape3D::CHUNK_QUADS);
	const int cz0 = (int)Math::floor((float)p_vertex_region.position.y / Landscape3D::CHUNK_QUADS);
	const int cx1 = (int)Math::floor((float)(p_vertex_region.position.x + p_vertex_region.size.x - 1) / Landscape3D::CHUNK_QUADS);
	const int cz1 = (int)Math::floor((float)(p_vertex_region.position.y + p_vertex_region.size.y - 1) / Landscape3D::CHUNK_QUADS);
	for (int cz = cz0; cz <= cz1; cz++) {
		for (int cx = cx0; cx <= cx1; cx++) {
			_snapshot_chunk_if_needed(Vector2i(cx, cz));
		}
	}
}

bool Landscape3DEditorPlugin::_raycast_terrain(Camera3D *p_camera, const Point2 &p_screen_pos, Vector3 &r_local_position, Vector3 &r_world_position, Vector3 &r_world_normal) {
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

void Landscape3DEditorPlugin::_ensure_cursor_instance() {
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

void Landscape3DEditorPlugin::_update_cursor(const Vector3 &p_world_position, const Vector3 &p_world_normal, bool p_visible) {
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

void Landscape3DEditorPlugin::_stamp(const Vector3 &p_local_position) {
	if (terrain == nullptr) {
		return;
	}
	if (mode == MODE_PAINT && terrain->get_layers().is_empty()) {
		return;
	}

	_snapshot_region_chunks(_get_brush_vertex_region(p_local_position, brush_radius));

	switch (mode) {
		case MODE_RAISE: {
			terrain->sculpt(p_local_position, brush_radius, brush_strength, Landscape3D::SCULPT_RAISE, brush_falloff, 0.0f, false);
		} break;
		case MODE_LOWER: {
			terrain->sculpt(p_local_position, brush_radius, brush_strength, Landscape3D::SCULPT_LOWER, brush_falloff, 0.0f, false);
		} break;
		case MODE_SMOOTH: {
			terrain->sculpt(p_local_position, brush_radius, brush_strength, Landscape3D::SCULPT_SMOOTH, brush_falloff, 0.0f, false);
		} break;
		case MODE_FLATTEN: {
			terrain->sculpt(p_local_position, brush_radius, brush_strength, Landscape3D::SCULPT_FLATTEN, brush_falloff, stroke_flatten_height, false);
		} break;
		case MODE_PAINT: {
			terrain->paint_layer(p_local_position, brush_radius, brush_strength, paint_layer_index, brush_falloff);
		} break;
		case MODE_HOLE: {
			terrain->set_hole(p_local_position, brush_radius, true, false);
		} break;
		case MODE_UNHOLE: {
			terrain->set_hole(p_local_position, brush_radius, false, false);
		} break;
	}
}

void Landscape3DEditorPlugin::_begin_stroke() {
	stroke_active = true;
	touched_regions.clear();
	last_stamp_msec = 0;
}

void Landscape3DEditorPlugin::_end_stroke() {
	if (!stroke_active) {
		return;
	}

	if (terrain != nullptr && !touched_regions.is_empty()) {
		const DataKind kind = _get_mode_data_kind();
		EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
		ur->create_action(_get_mode_action_name());
		for (KeyValue<Vector2i, TouchedChunkRegion> &kv : touched_regions) {
			const TouchedChunkRegion &snap = kv.value;
			switch (kind) {
				case DataKind::WEIGHTS: {
					const int layer_count = snap.before_weights.size();
					for (int i = 0; i < layer_count; i++) {
						const PackedFloat32Array after = terrain->get_layer_weight_region(snap.region, i);
						ur->add_do_method(terrain, "set_layer_weight_region", snap.region, i, after);
						ur->add_undo_method(terrain, "set_layer_weight_region", snap.region, i, snap.before_weights[i]);
					}
				} break;
				case DataKind::HOLE: {
					const PackedByteArray after = terrain->get_hole_region(snap.region);
					ur->add_do_method(terrain, "set_hole_region", snap.region, after);
					ur->add_undo_method(terrain, "set_hole_region", snap.region, snap.before_holes);
				} break;
				case DataKind::HEIGHT: {
					const PackedFloat32Array after = terrain->get_height_region(snap.region);
					ur->add_do_method(terrain, "set_height_region", snap.region, after, true);
					ur->add_undo_method(terrain, "set_height_region", snap.region, snap.before_heights, true);
				} break;
			}
		}
		ur->commit_action(false);

		if (kind == DataKind::HEIGHT) {
			terrain->update_collision();
		}
	}

	stroke_active = false;
	touched_regions.clear();
}

void Landscape3DEditorPlugin::_cancel_stroke() {
	if (stroke_active && terrain != nullptr) {
		const DataKind kind = _get_mode_data_kind();
		for (KeyValue<Vector2i, TouchedChunkRegion> &kv : touched_regions) {
			switch (kind) {
				case DataKind::WEIGHTS: {
					for (int i = 0; i < kv.value.before_weights.size(); i++) {
						terrain->set_layer_weight_region(kv.value.region, i, kv.value.before_weights[i]);
					}
				} break;
				case DataKind::HOLE: {
					terrain->set_hole_region(kv.value.region, kv.value.before_holes);
				} break;
				case DataKind::HEIGHT: {
					terrain->set_height_region(kv.value.region, kv.value.before_heights, true);
				} break;
			}
		}
	}
	stroke_active = false;
	touched_regions.clear();
}

bool Landscape3DEditorPlugin::_do_input_action(Camera3D *p_camera, const Point2 &p_screen_pos, bool p_click) {
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

EditorPlugin::AfterGUIInput Landscape3DEditorPlugin::forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) {
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

Landscape3DEditorPlugin::Landscape3DEditorPlugin() {
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
	mode_raise_button->connect(SceneStringName(pressed), callable_mp(this, &Landscape3DEditorPlugin::_mode_pressed).bind((int)MODE_RAISE));

	mode_lower_button = memnew(Button);
	mode_lower_button->set_toggle_mode(true);
	mode_lower_button->set_button_group(mode_button_group);
	mode_lower_button->set_text(TTR("Lower"));
	mode_lower_button->set_tooltip_text(TTR("Lower the terrain height within the brush."));
	toolbar->add_child(mode_lower_button);
	mode_lower_button->connect(SceneStringName(pressed), callable_mp(this, &Landscape3DEditorPlugin::_mode_pressed).bind((int)MODE_LOWER));

	mode_smooth_button = memnew(Button);
	mode_smooth_button->set_toggle_mode(true);
	mode_smooth_button->set_button_group(mode_button_group);
	mode_smooth_button->set_text(TTR("Smooth"));
	mode_smooth_button->set_tooltip_text(TTR("Average the terrain height with its neighbors within the brush."));
	toolbar->add_child(mode_smooth_button);
	mode_smooth_button->connect(SceneStringName(pressed), callable_mp(this, &Landscape3DEditorPlugin::_mode_pressed).bind((int)MODE_SMOOTH));

	mode_flatten_button = memnew(Button);
	mode_flatten_button->set_toggle_mode(true);
	mode_flatten_button->set_button_group(mode_button_group);
	mode_flatten_button->set_text(TTR("Flatten"));
	mode_flatten_button->set_tooltip_text(TTR("Flatten the terrain within the brush towards the height at the start of the stroke."));
	toolbar->add_child(mode_flatten_button);
	mode_flatten_button->connect(SceneStringName(pressed), callable_mp(this, &Landscape3DEditorPlugin::_mode_pressed).bind((int)MODE_FLATTEN));

	mode_paint_button = memnew(Button);
	mode_paint_button->set_toggle_mode(true);
	mode_paint_button->set_button_group(mode_button_group);
	mode_paint_button->set_text(TTR("Paint"));
	mode_paint_button->set_tooltip_text(TTR("Paint the selected texture layer within the brush."));
	toolbar->add_child(mode_paint_button);
	mode_paint_button->connect(SceneStringName(pressed), callable_mp(this, &Landscape3DEditorPlugin::_mode_pressed).bind((int)MODE_PAINT));

	mode_hole_button = memnew(Button);
	mode_hole_button->set_toggle_mode(true);
	mode_hole_button->set_button_group(mode_button_group);
	mode_hole_button->set_text(TTR("Hole"));
	mode_hole_button->set_tooltip_text(TTR("Cut a hole in the terrain within the brush."));
	toolbar->add_child(mode_hole_button);
	mode_hole_button->connect(SceneStringName(pressed), callable_mp(this, &Landscape3DEditorPlugin::_mode_pressed).bind((int)MODE_HOLE));

	mode_unhole_button = memnew(Button);
	mode_unhole_button->set_toggle_mode(true);
	mode_unhole_button->set_button_group(mode_button_group);
	mode_unhole_button->set_text(TTR("Unhole"));
	mode_unhole_button->set_tooltip_text(TTR("Fill in holes within the brush."));
	toolbar->add_child(mode_unhole_button);
	mode_unhole_button->connect(SceneStringName(pressed), callable_mp(this, &Landscape3DEditorPlugin::_mode_pressed).bind((int)MODE_UNHOLE));

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
	brush_radius_spin->connect(SceneStringName(value_changed), callable_mp(this, &Landscape3DEditorPlugin::_set_brush_radius));

	Label *strength_label = memnew(Label);
	strength_label->set_text(TTR("Strength:"));
	toolbar->add_child(strength_label);

	brush_strength_spin = memnew(SpinBox);
	brush_strength_spin->set_min(0.001);
	brush_strength_spin->set_max(1000.0);
	brush_strength_spin->set_step(0.001);
	brush_strength_spin->set_value(brush_strength);
	brush_strength_spin->set_tooltip_text(TTR("Effect applied per brush stamp: meters of height change for Raise/Lower/Flatten, blend amount (0-1 is typical) for Paint, and for Smooth the number of averaging passes - 1 takes the edge off, higher values flatten out progressively larger bumps."));
	toolbar->add_child(brush_strength_spin);
	brush_strength_spin->connect(SceneStringName(value_changed), callable_mp(this, &Landscape3DEditorPlugin::_set_brush_strength));

	toolbar->add_child(memnew(Label(TTR("Falloff:"))));
	brush_falloff_spin = memnew(SpinBox);
	brush_falloff_spin->set_min(0.0);
	brush_falloff_spin->set_max(1.0);
	brush_falloff_spin->set_step(0.01);
	brush_falloff_spin->set_value(brush_falloff);
	brush_falloff_spin->set_tooltip_text(TTR("How much of the brush fades out towards its rim. 1 tapers across the whole radius (softest); lower values hold an inner core at full strength and fade only the outside of it; 0 applies at full strength right up to the rim, leaving a hard edge."));
	toolbar->add_child(brush_falloff_spin);
	brush_falloff_spin->connect(SceneStringName(value_changed), callable_mp(this, &Landscape3DEditorPlugin::_set_brush_falloff));

	toolbar->add_child(memnew(VSeparator));

	paint_layer_menu = memnew(MenuButton);
	paint_layer_menu->set_text(TTR("No Layers"));
	paint_layer_menu->set_tooltip_text(TTR("Choose which TerrainLayer the Paint brush applies."));
	paint_layer_menu->get_popup()->connect(SceneStringName(id_pressed), callable_mp(this, &Landscape3DEditorPlugin::_paint_layer_menu_id_pressed));
	paint_layer_menu->connect("about_to_popup", callable_mp(this, &Landscape3DEditorPlugin::_rebuild_paint_layer_menu));
	toolbar->add_child(paint_layer_menu);

	toolbar->add_child(memnew(VSeparator));

	add_spline_menu = memnew(MenuButton);
	add_spline_menu->set_text(TTR("Add Spline"));
	add_spline_menu->set_tooltip_text(TTR("Add a road, river or stream to this terrain: a LandscapeSpline3D laid along a curve. Place its points with the Path3D tools (turn on their \"Snap to Colliders\" option to drop them onto the terrain), then use \"Apply to Landscape\" to shape the ground under it."));
	add_spline_menu->get_popup()->add_item(TTR("Road"), LandscapeSpline3D::TYPE_ROAD);
	add_spline_menu->get_popup()->add_item(TTR("River"), LandscapeSpline3D::TYPE_RIVER);
	add_spline_menu->get_popup()->add_item(TTR("Stream"), LandscapeSpline3D::TYPE_STREAM);
	add_spline_menu->get_popup()->connect(SceneStringName(id_pressed), callable_mp(this, &Landscape3DEditorPlugin::_add_spline_menu_id_pressed));
	toolbar->add_child(add_spline_menu);

	toolbar->add_child(memnew(VSeparator));

	import_heightmap_button = memnew(Button);
	import_heightmap_button->set_text(TTR("Import Heightmap..."));
	import_heightmap_button->set_tooltip_text(TTR("Import a grayscale image as this terrain's heightmap, replacing the current one and resizing the terrain to match the image. Prefer an EXR or HDR heightmap over PNG: Godot always decodes PNG to 8 bits per channel (256 possible heights), while EXR/HDR keep real height precision."));
	toolbar->add_child(import_heightmap_button);
	import_heightmap_button->connect(SceneStringName(pressed), callable_mp(this, &Landscape3DEditorPlugin::_import_heightmap_pressed));

	import_layer_mask_button = memnew(Button);
	import_layer_mask_button->set_text(TTR("Import Layer Mask..."));
	import_layer_mask_button->set_tooltip_text(TTR("Import a grayscale image as where one TerrainLayer shows, for the texturing masks (slopes, peaks, hollows, roads, fields...) a terrain tool usually exports alongside a heightmap. Unlike a heightmap, a mask never resizes the terrain: one authored at a different resolution is resampled to fit."));
	toolbar->add_child(import_layer_mask_button);
	import_layer_mask_button->connect(SceneStringName(pressed), callable_mp(this, &Landscape3DEditorPlugin::_import_layer_mask_pressed));

	generate_layer_mask_button = memnew(Button);
	generate_layer_mask_button->set_text(TTR("Generate Layer Mask..."));
	generate_layer_mask_button->set_tooltip_text(TTR("Build a layer's mask from the terrain's own shape instead of an image: where it sits within a height range and how steep it is there. This is how a terrain gets textured by what it is - rock on cliff faces, snow on peaks, sand in the low flats - without painting or authoring a mask by hand."));
	toolbar->add_child(generate_layer_mask_button);
	generate_layer_mask_button->connect(SceneStringName(pressed), callable_mp(this, &Landscape3DEditorPlugin::_generate_layer_mask_pressed));

	Node3DEditor::get_singleton()->add_control_to_menu_panel(topmenu_bar);

	import_file_dialog = memnew(EditorFileDialog);
	import_file_dialog->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_FILE);
	import_file_dialog->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
	import_file_dialog->set_title(TTR("Import Heightmap"));
	// Only formats that make sense for height data: EXR/HDR store real float
	// precision (recommended), while PNG is at least lossless, if only 8-bit
	// (256 discrete heights - see the format warning in _do_import_heightmap).
	// Deliberately excludes lossy formats (JPEG, lossy WebP, ...): compression
	// artifacts in a heightmap show up as actual bumps in the terrain surface.
	import_file_dialog->add_filter("*.exr,*.hdr", TTR("High-Precision Heightmap (Recommended)"));
	import_file_dialog->add_filter("*.png", TTR("8-Bit Heightmap"));
	import_file_dialog->connect("file_selected", callable_mp(this, &Landscape3DEditorPlugin::_import_file_selected));
	EditorInterface::get_singleton()->get_base_control()->add_child(import_file_dialog);

	import_height_range_dialog = memnew(ConfirmationDialog);
	import_height_range_dialog->set_title(TTR("Heightmap Height Range"));
	import_height_range_dialog->set_ok_button_text(TTR("Import"));
	import_height_range_dialog->connect(SceneStringName(confirmed), callable_mp(this, &Landscape3DEditorPlugin::_do_import_heightmap));
	EditorInterface::get_singleton()->get_base_control()->add_child(import_height_range_dialog);

	VBoxContainer *import_vbc = memnew(VBoxContainer);
	import_height_range_dialog->add_child(import_vbc);

	import_height_min_spin = memnew(SpinBox);
	import_height_min_spin->set_min(-10000.0);
	import_height_min_spin->set_max(10000.0);
	import_height_min_spin->set_step(0.01);
	import_height_min_spin->set_value(0.0);
	import_vbc->add_margin_child(TTR("Height Min (meters):"), import_height_min_spin);

	import_height_max_spin = memnew(SpinBox);
	import_height_max_spin->set_min(-10000.0);
	import_height_max_spin->set_max(10000.0);
	import_height_max_spin->set_step(0.01);
	import_height_max_spin->set_value(100.0);
	import_vbc->add_margin_child(TTR("Height Max (meters):"), import_height_max_spin);

	mask_file_dialog = memnew(EditorFileDialog);
	mask_file_dialog->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_FILE);
	mask_file_dialog->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
	mask_file_dialog->set_title(TTR("Import Layer Mask"));
	// Masks are far less precision-sensitive than a heightmap (8 bits is 256
	// blend steps between layers, which is plenty), so this accepts the ordinary
	// image formats terrain tools export masks in, PNG included.
	mask_file_dialog->add_filter("*.png,*.exr,*.hdr,*.tga,*.webp", TTR("Mask Image"));
	mask_file_dialog->connect("file_selected", callable_mp(this, &Landscape3DEditorPlugin::_mask_file_selected));
	EditorInterface::get_singleton()->get_base_control()->add_child(mask_file_dialog);

	mask_options_dialog = memnew(ConfirmationDialog);
	mask_options_dialog->set_title(TTR("Import Layer Mask"));
	mask_options_dialog->set_ok_button_text(TTR("Import"));
	mask_options_dialog->connect(SceneStringName(confirmed), callable_mp(this, &Landscape3DEditorPlugin::_do_import_layer_mask));
	EditorInterface::get_singleton()->get_base_control()->add_child(mask_options_dialog);

	VBoxContainer *mask_vbc = memnew(VBoxContainer);
	mask_options_dialog->add_child(mask_vbc);

	mask_layer_option = memnew(OptionButton);
	mask_vbc->add_margin_child(TTR("Apply To Layer:"), mask_layer_option);

	mask_channel_option = memnew(OptionButton);
	mask_channel_option->add_item(TTR("Red (grayscale masks)"), TerrainData::MASK_CHANNEL_RED);
	mask_channel_option->add_item(TTR("Green"), TerrainData::MASK_CHANNEL_GREEN);
	mask_channel_option->add_item(TTR("Blue"), TerrainData::MASK_CHANNEL_BLUE);
	mask_channel_option->add_item(TTR("Alpha"), TerrainData::MASK_CHANNEL_ALPHA);
	mask_channel_option->select(0);
	mask_vbc->add_margin_child(TTR("Read Mask From Channel:"), mask_channel_option);

	mask_normalize_check = memnew(CheckBox);
	mask_normalize_check->set_text(TTR("Take the weight from the other layers"));
	mask_normalize_check->set_pressed(true);
	mask_normalize_check->set_tooltip_text(TTR("On (recommended when importing one mask at a time): where the mask is white this layer replaces whatever else is painted there, the same as painting it at full strength. Turn it off when importing a complete set of masks that already add up across all layers - then supply a mask for every layer, including the first one, so nothing keeps stale weight."));
	mask_vbc->add_margin_child(TTR("Blending:"), mask_normalize_check);

	generate_mask_dialog = memnew(ConfirmationDialog);
	generate_mask_dialog->set_title(TTR("Generate Layer Mask"));
	generate_mask_dialog->set_ok_button_text(TTR("Generate"));
	generate_mask_dialog->connect(SceneStringName(confirmed), callable_mp(this, &Landscape3DEditorPlugin::_do_generate_layer_mask));
	EditorInterface::get_singleton()->get_base_control()->add_child(generate_mask_dialog);

	VBoxContainer *generate_vbc = memnew(VBoxContainer);
	generate_mask_dialog->add_child(generate_vbc);

	generate_layer_option = memnew(OptionButton);
	generate_vbc->add_margin_child(TTR("Apply To Layer:"), generate_layer_option);

	generate_range_label = memnew(Label);
	generate_vbc->add_child(generate_range_label);

	generate_height_min_spin = memnew(SpinBox);
	generate_height_min_spin->set_min(-100000.0);
	generate_height_min_spin->set_max(100000.0);
	generate_height_min_spin->set_step(0.01);
	generate_height_min_spin->set_tooltip_text(TTR("The layer only shows at or above this height."));
	generate_vbc->add_margin_child(TTR("Height From (meters):"), generate_height_min_spin);

	generate_height_max_spin = memnew(SpinBox);
	generate_height_max_spin->set_min(-100000.0);
	generate_height_max_spin->set_max(100000.0);
	generate_height_max_spin->set_step(0.01);
	generate_height_max_spin->set_tooltip_text(TTR("The layer only shows at or below this height."));
	generate_vbc->add_margin_child(TTR("Height To (meters):"), generate_height_max_spin);

	generate_height_falloff_spin = memnew(SpinBox);
	generate_height_falloff_spin->set_min(0.0);
	generate_height_falloff_spin->set_max(100000.0);
	generate_height_falloff_spin->set_step(0.01);
	generate_height_falloff_spin->set_value(5.0);
	generate_height_falloff_spin->set_tooltip_text(TTR("How far beyond each end of the height range the layer fades out over, instead of stopping at a hard line. 0 gives a hard edge."));
	generate_vbc->add_margin_child(TTR("Height Falloff (meters):"), generate_height_falloff_spin);

	generate_slope_min_spin = memnew(SpinBox);
	generate_slope_min_spin->set_min(0.0);
	generate_slope_min_spin->set_max(90.0);
	generate_slope_min_spin->set_step(0.1);
	generate_slope_min_spin->set_value(0.0);
	generate_slope_min_spin->set_tooltip_text(TTR("The layer only shows on ground at least this steep. 0 is flat ground, 90 is a vertical wall."));
	generate_vbc->add_margin_child(TTR("Slope From (degrees):"), generate_slope_min_spin);

	generate_slope_max_spin = memnew(SpinBox);
	generate_slope_max_spin->set_min(0.0);
	generate_slope_max_spin->set_max(90.0);
	generate_slope_max_spin->set_step(0.1);
	generate_slope_max_spin->set_value(90.0);
	generate_slope_max_spin->set_tooltip_text(TTR("The layer only shows on ground at most this steep. Leave at 90 to put no upper limit on steepness."));
	generate_vbc->add_margin_child(TTR("Slope To (degrees):"), generate_slope_max_spin);

	generate_slope_falloff_spin = memnew(SpinBox);
	generate_slope_falloff_spin->set_min(0.0);
	generate_slope_falloff_spin->set_max(90.0);
	generate_slope_falloff_spin->set_step(0.1);
	generate_slope_falloff_spin->set_value(5.0);
	generate_slope_falloff_spin->set_tooltip_text(TTR("How far beyond each end of the slope range the layer fades out over, instead of stopping at a hard line. 0 gives a hard edge."));
	generate_vbc->add_margin_child(TTR("Slope Falloff (degrees):"), generate_slope_falloff_spin);

	generate_curvature_radius_spin = memnew(SpinBox);
	generate_curvature_radius_spin->set_min(1.0);
	generate_curvature_radius_spin->set_max(64.0);
	generate_curvature_radius_spin->set_step(1.0);
	generate_curvature_radius_spin->set_value(2.0);
	generate_curvature_radius_spin->set_tooltip_text(TTR("How far apart, in height samples, the ground is compared against itself to tell a hollow from a rise. Small values catch fine bumps (and the single-sample noise an imported heightmap carries); larger ones pick out broad valleys and ridges."));
	generate_vbc->add_margin_child(TTR("Curvature Radius (samples):"), generate_curvature_radius_spin);
	generate_curvature_radius_spin->connect(SceneStringName(value_changed), callable_mp(this, &Landscape3DEditorPlugin::_update_curvature_range_label));

	generate_curvature_range_label = memnew(Label);
	generate_vbc->add_child(generate_curvature_range_label);

	generate_curvature_min_spin = memnew(SpinBox);
	generate_curvature_min_spin->set_min(-100000.0);
	generate_curvature_min_spin->set_max(100000.0);
	generate_curvature_min_spin->set_step(0.0001);
	generate_curvature_min_spin->set_value(-100000.0);
	generate_curvature_min_spin->set_tooltip_text(TTR("The layer only shows where the ground is at least this curved. Negative is a hollow, 0 is flat or evenly sloping however steep, positive is a rise. Leave at the minimum to put no lower limit on it."));
	generate_vbc->add_margin_child(TTR("Curvature From (hollow ... rise):"), generate_curvature_min_spin);

	generate_curvature_max_spin = memnew(SpinBox);
	generate_curvature_max_spin->set_min(-100000.0);
	generate_curvature_max_spin->set_max(100000.0);
	generate_curvature_max_spin->set_step(0.0001);
	generate_curvature_max_spin->set_value(100000.0);
	generate_curvature_max_spin->set_tooltip_text(TTR("The layer only shows where the ground is at most this curved. Set this negative to catch hollows alone; leave at the maximum to put no upper limit on it."));
	generate_vbc->add_margin_child(TTR("Curvature To (hollow ... rise):"), generate_curvature_max_spin);

	generate_curvature_falloff_spin = memnew(SpinBox);
	generate_curvature_falloff_spin->set_min(0.0);
	generate_curvature_falloff_spin->set_max(100000.0);
	generate_curvature_falloff_spin->set_step(0.0001);
	generate_curvature_falloff_spin->set_value(0.0);
	generate_curvature_falloff_spin->set_tooltip_text(TTR("How far beyond each end of the curvature range the layer fades out over, instead of stopping at a hard line. 0 gives a hard edge."));
	generate_vbc->add_margin_child(TTR("Curvature Falloff:"), generate_curvature_falloff_spin);

	generate_normalize_check = memnew(CheckBox);
	generate_normalize_check->set_text(TTR("Take the weight from the other layers"));
	generate_normalize_check->set_pressed(true);
	generate_normalize_check->set_tooltip_text(TTR("On (recommended): where the rule matches fully, this layer replaces whatever else is there, so generating one rule per layer in order of precedence builds up the whole terrain. Turn it off to set this layer's weights on their own, leaving the other layers untouched."));
	generate_vbc->add_margin_child(TTR("Blending:"), generate_normalize_check);
}

Landscape3DEditorPlugin::~Landscape3DEditorPlugin() {
	if (cursor_instance.is_valid()) {
		RS::get_singleton()->free_rid(cursor_instance);
	}
	if (cursor_mesh.is_valid()) {
		RS::get_singleton()->free_rid(cursor_mesh);
	}
}
