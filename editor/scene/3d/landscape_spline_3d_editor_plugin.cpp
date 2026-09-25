/**************************************************************************/
/*  landscape_spline_3d_editor_plugin.cpp                                 */
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

#include "landscape_spline_3d_editor_plugin.h"

#include "core/object/callable_mp.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "scene/3d/landscape_3d.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/separator.h"
#include "scene/resources/curve.h"

bool LandscapeSpline3DEditorPlugin::handles(Object *p_object) const {
	return Object::cast_to<LandscapeSpline3D>(p_object) != nullptr;
}

void LandscapeSpline3DEditorPlugin::edit(Object *p_object) {
	spline = Object::cast_to<LandscapeSpline3D>(p_object);
}

void LandscapeSpline3DEditorPlugin::make_visible(bool p_visible) {
	toolbar->set_visible(p_visible);
	if (!p_visible) {
		spline = nullptr;
	}
}

void LandscapeSpline3DEditorPlugin::_snap_points_pressed() {
	if (spline == nullptr) {
		return;
	}
	const Ref<Curve3D> curve = spline->get_curve();
	if (curve.is_null() || curve->get_point_count() == 0) {
		EditorNode::get_singleton()->show_warning(TTR("This spline has no points to snap yet. Add some with the Path3D tools first."));
		return;
	}
	if (spline->get_landscape() == nullptr) {
		EditorNode::get_singleton()->show_warning(TTR("No Landscape3D found to snap to. Set landscape_path, or place the spline under the landscape or next to it."));
		return;
	}

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(TTR("Snap Spline Points to Terrain"));
	for (int i = 0; i < curve->get_point_count(); i++) {
		const Vector3 position = curve->get_point_position(i);
		const Vector3 snapped = spline->project_to_landscape(position);
		if (!snapped.is_equal_approx(position)) {
			ur->add_do_method(curve.ptr(), "set_point_position", i, snapped);
			ur->add_undo_method(curve.ptr(), "set_point_position", i, position);
		}
	}
	ur->commit_action();
}

void LandscapeSpline3DEditorPlugin::_apply_pressed() {
	if (spline == nullptr) {
		return;
	}
	Landscape3D *landscape = spline->get_landscape();
	if (landscape == nullptr || landscape->get_terrain_data().is_null()) {
		EditorNode::get_singleton()->show_warning(TTR("No Landscape3D with TerrainData found to apply this spline to. Set landscape_path, or place the spline under the landscape or next to it."));
		return;
	}
	const int layer_count = MIN(landscape->get_layers().size(), TerrainData::MAX_LAYERS);
	const bool paint = spline->get_paint_layer() >= 0 && spline->get_paint_layer() < layer_count && spline->get_paint_strength() > 0.0f;
	if (!spline->is_carve_enabled() && !paint) {
		EditorNode::get_singleton()->show_warning(TTR("Nothing to apply: carving is off and no valid paint layer is set. Enable Carve, or set Paint > Layer to one of the landscape's layers."));
		return;
	}

	// Rings first, so the footprint describes the curve as it is now.
	spline->update_mesh();
	const TypedArray<Rect2i> regions = spline->get_landscape_footprint();
	if (regions.is_empty()) {
		EditorNode::get_singleton()->show_warning(TTR("The spline does not cross the landscape anywhere."));
		return;
	}

	// Only the terrain blocks the spline actually crosses are snapshotted, so
	// the undo history holds a strip along the road, not the whole terrain.
	TypedArray<PackedFloat32Array> before_heights;
	if (spline->is_carve_enabled()) {
		before_heights = landscape->get_height_regions(regions);
	}
	// Painting one layer takes weight from all the others, so undoing it has
	// to restore every layer.
	Vector<TypedArray<PackedFloat32Array>> before_weights;
	if (paint) {
		for (int i = 0; i < layer_count; i++) {
			before_weights.push_back(landscape->get_layer_weight_regions(regions, i));
		}
	}

	spline->apply_to_landscape();

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(TTR("Apply Spline to Landscape"));
	if (spline->is_carve_enabled()) {
		const TypedArray<PackedFloat32Array> after_heights = landscape->get_height_regions(regions);
		ur->add_do_method(landscape, "set_height_regions", regions, after_heights, true);
		ur->add_undo_method(landscape, "set_height_regions", regions, before_heights, true);
	}
	for (int i = 0; i < before_weights.size(); i++) {
		const TypedArray<PackedFloat32Array> after_weights = landscape->get_layer_weight_regions(regions, i);
		ur->add_do_method(landscape, "set_layer_weight_regions", regions, i, after_weights);
		ur->add_undo_method(landscape, "set_layer_weight_regions", regions, i, before_weights[i]);
	}
	// Already applied above; committing only records it.
	ur->commit_action(false);
}

void LandscapeSpline3DEditorPlugin::_copy_material_pressed() {
	if (spline == nullptr) {
		return;
	}
	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(TTR("Copy Built-In Spline Material"));
	ur->add_do_property(spline, "material", LandscapeSpline3D::create_default_material(spline->get_spline_type()));
	ur->add_undo_property(spline, "material", spline->get_material());
	ur->commit_action();
}

void LandscapeSpline3DEditorPlugin::_undo_redo_inspector_callback(Object *p_undo_redo, Object *p_edited, const String &p_property, const Variant &p_new_value) {
	LandscapeSpline3D *edited = Object::cast_to<LandscapeSpline3D>(p_edited);
	if (edited == nullptr || p_property != "spline_type") {
		return;
	}
	EditorUndoRedoManager *ur = Object::cast_to<EditorUndoRedoManager>(p_undo_redo);
	ERR_FAIL_NULL(ur);

	const int type = p_new_value;
	if (type < 0 || type >= LandscapeSpline3D::TYPE_MAX || type == edited->get_spline_type()) {
		return;
	}
	// A road, a river and a stream differ in far more than their material:
	// how wide they are, whether they follow the ground or stay level, how
	// deep they cut into it. Picking the type is the one choice a user makes
	// knowingly, so the rest follows it, in the same undo step.
	const Dictionary preset = LandscapeSpline3D::get_preset((LandscapeSpline3D::SplineType)type);
	for (const KeyValue<Variant, Variant> &kv : preset) {
		ur->add_do_property(edited, kv.key, kv.value);
		ur->add_undo_property(edited, kv.key, edited->get(kv.key));
	}
}

LandscapeSpline3DEditorPlugin::LandscapeSpline3DEditorPlugin() {
	toolbar = memnew(HBoxContainer);
	toolbar->hide();

	toolbar->add_child(memnew(VSeparator));

	snap_points_button = memnew(Button);
	snap_points_button->set_theme_type_variation(SceneStringName(FlatButton));
	snap_points_button->set_text(TTR("Snap Points to Terrain"));
	snap_points_button->set_tooltip_text(TTR("Move every point of the curve straight up or down onto the landscape. The curve's own height is what Apply to Landscape shapes the ground to, and what a River (Height Mode: Spline) floats at."));
	snap_points_button->connect(SceneStringName(pressed), callable_mp(this, &LandscapeSpline3DEditorPlugin::_snap_points_pressed));
	toolbar->add_child(snap_points_button);

	apply_button = memnew(Button);
	apply_button->set_theme_type_variation(SceneStringName(FlatButton));
	apply_button->set_text(TTR("Apply to Landscape"));
	apply_button->set_tooltip_text(TTR("Shape the landscape to the spline: cut and fill the ground under it to the curve's height (Carve > Depth deeper for a river bed), blend back into the untouched terrain over Carve > Falloff, and paint Paint > Layer under it. Only the terrain the spline crosses is touched, and the whole change is one undo step."));
	apply_button->connect(SceneStringName(pressed), callable_mp(this, &LandscapeSpline3DEditorPlugin::_apply_pressed));
	toolbar->add_child(apply_button);

	copy_material_button = memnew(Button);
	copy_material_button->set_theme_type_variation(SceneStringName(FlatButton));
	copy_material_button->set_text(TTR("Copy Built-In Material"));
	copy_material_button->set_tooltip_text(TTR("Assign a copy of the built-in road or water material this spline type renders with by default, with its own copy of the shader, ready to tweak."));
	copy_material_button->connect(SceneStringName(pressed), callable_mp(this, &LandscapeSpline3DEditorPlugin::_copy_material_pressed));
	toolbar->add_child(copy_material_button);

	Node3DEditor::get_singleton()->add_control_to_menu_panel(toolbar);

	EditorNode::get_editor_data().add_undo_redo_inspector_hook_callback(callable_mp(this, &LandscapeSpline3DEditorPlugin::_undo_redo_inspector_callback));
}
