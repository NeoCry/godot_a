/**************************************************************************/
/*  foliage_painter_3d_inspector_plugin.cpp                               */
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

#include "foliage_painter_3d_inspector_plugin.h"

#include "core/core_string_names.h"
#include "core/io/resource_loader.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/docks/inspector_dock.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/gui/editor_spin_slider.h"
#include "editor/inspector/editor_resource_picker.h"
#include "editor/themes/editor_scale.h"
#include "scene/3d/foliage_painter_3d.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/grid_container.h"
#include "scene/gui/label.h"
#include "scene/gui/margin_container.h"
#include "scene/gui/menu_button.h"
#include "scene/gui/option_button.h"
#include "scene/gui/panel_container.h"
#include "scene/gui/popup.h"
#include "scene/gui/popup_menu.h"
#include "scene/main/viewport.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "scene/resources/style_box_flat.h"
#include "scene/scene_string_names.h"

namespace {
// Distances as the LOD summary and the LOD window's buttons state them.
String _format_distance(float p_distance) {
	return String::num(p_distance, 2);
}

String _get_fade_mode_name(GeometryInstance3D::VisibilityRangeFadeMode p_mode) {
	switch (p_mode) {
		case GeometryInstance3D::VISIBILITY_RANGE_FADE_SELF:
			return TTR("Self");
		case GeometryInstance3D::VISIBILITY_RANGE_FADE_DEPENDENCIES:
			return TTR("Dependencies");
		default:
			return TTR("Off");
	}
}

String _get_mesh_name(const Ref<Mesh> &p_mesh) {
	if (p_mesh.is_null()) {
		return TTR("No Mesh");
	}
	if (!p_mesh->get_name().is_empty()) {
		return p_mesh->get_name();
	}
	if (!p_mesh->is_built_in()) {
		return p_mesh->get_path().get_file().get_basename();
	}
	return p_mesh->get_class();
}

String _get_range_text(const Ref<FoliageLODLevel> &p_level) {
	const float begin = p_level->get_visibility_range_begin();
	const float end = p_level->get_visibility_range_end();
	if (end > 0.0f) {
		return vformat(TTR("%s - %s m"), _format_distance(begin), _format_distance(end));
	}
	return vformat(TTR("from %s m"), _format_distance(begin));
}
} // namespace

////////////////////////////////////////////////////////////////////////////
// FoliageLODsDialog

String FoliageLODsDialog::get_lod_name(int p_index) {
	return vformat("LOD_%d", p_index);
}

void FoliageLODsDialog::edit(const Ref<FoliageLayer> &p_layer) {
	if (layer.is_valid() && layer->is_connected(CoreStringName(changed), callable_mp(this, &FoliageLODsDialog::_layer_changed))) {
		layer->disconnect(CoreStringName(changed), callable_mp(this, &FoliageLODsDialog::_layer_changed));
	}
	layer = p_layer;
	if (layer.is_null()) {
		return;
	}
	layer->connect(CoreStringName(changed), callable_mp(this, &FoliageLODsDialog::_layer_changed));

	const String layer_name = layer->get_layer_name().is_empty() ? String("FoliageLayer") : layer->get_layer_name();
	set_title(vformat(TTR("LODs: %s"), layer_name));
	_rebuild_rows();
	popup_centered(Size2(900, 0) * EDSCALE);
}

void FoliageLODsDialog::_visibility_changed() {
	if (is_visible()) {
		return;
	}
	// Let go of the layer once the window is closed, so it is not kept alive
	// (or listened to) by a window nobody is looking at.
	fade_popup->hide();
	rendering_popup->hide();
	if (layer.is_valid() && layer->is_connected(CoreStringName(changed), callable_mp(this, &FoliageLODsDialog::_layer_changed))) {
		layer->disconnect(CoreStringName(changed), callable_mp(this, &FoliageLODsDialog::_layer_changed));
	}
	layer.unref();
	popup_row = -1;
}

void FoliageLODsDialog::_layer_changed() {
	if (layer.is_null() || refresh_queued) {
		return;
	}
	// The rows may be rebuilt, and it may be one of their own controls that
	// made the change (Remove, say), so this waits for its signal to be over.
	// It also folds the many changes of a dragged value into one refresh.
	refresh_queued = true;
	callable_mp(this, &FoliageLODsDialog::_refresh).call_deferred();
}

void FoliageLODsDialog::_refresh() {
	refresh_queued = false;
	if (_rows_match()) {
		_update_rows();
	} else {
		_rebuild_rows();
	}
}

bool FoliageLODsDialog::_rows_match() const {
	if (layer.is_null()) {
		return rows.is_empty();
	}
	const TypedArray<FoliageLODLevel> levels = layer->get_lod_levels();
	if ((int)rows.size() != levels.size()) {
		return false;
	}
	for (int i = 0; i < levels.size(); i++) {
		if (rows[i].level != Ref<FoliageLODLevel>(levels[i])) {
			return false;
		}
	}
	return true;
}

void FoliageLODsDialog::_rebuild_rows() {
	for (Row &row : rows) {
		Control *controls[] = { row.name_label, row.mesh_picker, Object::cast_to<Control>(row.begin_spin->get_parent()), row.rendering_button, row.remove_button };
		for (Control *control : controls) {
			grid->remove_child(control);
			control->queue_free();
		}
	}
	rows.clear();

	if (layer.is_null()) {
		return;
	}

	const TypedArray<FoliageLODLevel> levels = layer->get_lod_levels();
	for (int i = 0; i < levels.size(); i++) {
		Row row;
		row.level = levels[i];

		row.name_label = memnew(Label(get_lod_name(i)));
		grid->add_child(row.name_label);

		row.mesh_picker = memnew(EditorResourcePicker);
		row.mesh_picker->set_base_type("Mesh");
		row.mesh_picker->set_custom_minimum_size(Size2(240, 0) * EDSCALE);
		row.mesh_picker->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		row.mesh_picker->connect("resource_changed", callable_mp(this, &FoliageLODsDialog::_mesh_changed).bind(i));
		grid->add_child(row.mesh_picker);

		HBoxContainer *range_box = memnew(HBoxContainer);
		grid->add_child(range_box);

		row.begin_spin = memnew(EditorSpinSlider);
		row.begin_spin->set_label(TTR("Begin"));
		row.begin_spin->set_min(0.0);
		row.begin_spin->set_max(4096.0);
		row.begin_spin->set_step(0.01);
		row.begin_spin->set_allow_greater(true);
		row.begin_spin->set_suffix("m");
		row.begin_spin->set_custom_minimum_size(Size2(120, 0) * EDSCALE);
		row.begin_spin->set_tooltip_text(TTR("The camera distance this LOD starts being drawn at."));
		row.begin_spin->connect(SceneStringName(value_changed), callable_mp(this, &FoliageLODsDialog::_begin_changed).bind(i));
		range_box->add_child(row.begin_spin);

		row.end_spin = memnew(EditorSpinSlider);
		row.end_spin->set_label(TTR("End"));
		row.end_spin->set_min(0.0);
		row.end_spin->set_max(4096.0);
		row.end_spin->set_step(0.01);
		row.end_spin->set_allow_greater(true);
		row.end_spin->set_suffix("m");
		row.end_spin->set_custom_minimum_size(Size2(120, 0) * EDSCALE);
		row.end_spin->set_tooltip_text(TTR("The camera distance this LOD stops being drawn at. 0 draws it out to any distance."));
		row.end_spin->connect(SceneStringName(value_changed), callable_mp(this, &FoliageLODsDialog::_end_changed).bind(i));
		range_box->add_child(row.end_spin);

		row.fade_button = memnew(Button);
		row.fade_button->set_custom_minimum_size(Size2(110, 0) * EDSCALE);
		row.fade_button->connect(SceneStringName(pressed), callable_mp(this, &FoliageLODsDialog::_fade_pressed).bind(i));
		range_box->add_child(row.fade_button);

		row.rendering_button = memnew(Button);
		row.rendering_button->set_custom_minimum_size(Size2(150, 0) * EDSCALE);
		row.rendering_button->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
		row.rendering_button->connect(SceneStringName(pressed), callable_mp(this, &FoliageLODsDialog::_rendering_pressed).bind(i));
		grid->add_child(row.rendering_button);

		row.remove_button = memnew(Button);
		row.remove_button->set_flat(true);
		row.remove_button->set_tooltip_text(vformat(TTR("Remove %s."), get_lod_name(i)));
		row.remove_button->connect(SceneStringName(pressed), callable_mp(this, &FoliageLODsDialog::_remove_lod).bind(i));
		grid->add_child(row.remove_button);

		// An empty slot in the array has nothing to edit but its removal.
		const bool valid = row.level.is_valid();
		row.mesh_picker->set_editable(valid);
		row.begin_spin->set_read_only(!valid);
		row.end_spin->set_read_only(!valid);
		row.fade_button->set_disabled(!valid);
		row.rendering_button->set_disabled(!valid);

		rows.push_back(row);
	}

	_update_theme();
	_update_rows();
}

void FoliageLODsDialog::_update_rows() {
	updating = true;
	for (uint32_t i = 0; i < rows.size(); i++) {
		Row &row = rows[i];
		if (row.level.is_null()) {
			row.name_label->set_text(vformat(TTR("%s (empty)"), get_lod_name(i)));
			continue;
		}
		row.name_label->set_text(get_lod_name(i));
		row.mesh_picker->set_edited_resource_no_check(row.level->get_mesh());
		row.begin_spin->set_value_no_signal(row.level->get_visibility_range_begin());
		row.end_spin->set_value_no_signal(row.level->get_visibility_range_end());

		const GeometryInstance3D::VisibilityRangeFadeMode fade_mode = row.level->get_visibility_range_fade_mode();
		row.fade_button->set_text(vformat(TTR("Fade: %s"), _get_fade_mode_name(fade_mode)));
		row.fade_button->set_tooltip_text(vformat(TTR("How this LOD blends into the next and previous ones: begin margin %s m, end margin %s m, fade mode %s.\nClick to change."),
				_format_distance(row.level->get_visibility_range_begin_margin()), _format_distance(row.level->get_visibility_range_end_margin()), _get_fade_mode_name(fade_mode)));

		String rendering = row.level->is_casting_shadows() ? TTR("Shadows") : TTR("No Shadows");
		if (row.level->get_material_override().is_valid()) {
			rendering += " + " + TTR("Material");
		}
		row.rendering_button->set_text(rendering);
		row.rendering_button->set_tooltip_text(vformat(TTR("Material override: %s. Casts shadows: %s.\nClick to change."),
				row.level->get_material_override().is_valid() ? row.level->get_material_override()->get_class() : TTR("None"),
				row.level->is_casting_shadows() ? TTR("On") : TTR("Off")));
	}
	_update_popups();
	updating = false;
}

void FoliageLODsDialog::_update_popups() {
	const Ref<FoliageLODLevel> level = _get_row_level(popup_row);
	if (level.is_null()) {
		fade_popup->hide();
		rendering_popup->hide();
		return;
	}
	const bool was_updating = updating;
	updating = true;
	begin_margin_spin->set_value_no_signal(level->get_visibility_range_begin_margin());
	end_margin_spin->set_value_no_signal(level->get_visibility_range_end_margin());
	fade_mode_option->select(fade_mode_option->get_item_index(level->get_visibility_range_fade_mode()));
	material_picker->set_edited_resource_no_check(level->get_material_override());
	cast_shadows_check->set_pressed_no_signal(level->is_casting_shadows());
	updating = was_updating;
}

void FoliageLODsDialog::_update_theme() {
	add_button->set_button_icon(get_editor_theme_icon(SNAME("Add")));
	for (Row &row : rows) {
		row.remove_button->set_button_icon(get_editor_theme_icon(SNAME("Remove")));
	}
}

Ref<FoliageLODLevel> FoliageLODsDialog::_get_row_level(int p_row) const {
	if (p_row < 0 || p_row >= (int)rows.size()) {
		return Ref<FoliageLODLevel>();
	}
	return rows[p_row].level;
}

void FoliageLODsDialog::_set_level_property(int p_row, const StringName &p_property, const Variant &p_value, bool p_merge) {
	const Ref<FoliageLODLevel> level = _get_row_level(p_row);
	if (updating || level.is_null()) {
		return;
	}
	const Variant old_value = level->get(p_property);
	if (old_value == p_value) {
		return;
	}

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(vformat(TTR("Set %s %s"), get_lod_name(p_row), String(p_property).capitalize()), p_merge ? UndoRedo::MERGE_ENDS : UndoRedo::MERGE_DISABLE, level.ptr());
	ur->add_do_property(level.ptr(), p_property, p_value);
	ur->add_undo_property(level.ptr(), p_property, old_value);
	ur->commit_action();
}

void FoliageLODsDialog::_mesh_changed(const Ref<Resource> &p_mesh, int p_row) {
	_set_level_property(p_row, SNAME("mesh"), p_mesh, false);
}

void FoliageLODsDialog::_begin_changed(double p_value, int p_row) {
	_set_level_property(p_row, SNAME("visibility_range_begin"), p_value, true);
}

void FoliageLODsDialog::_end_changed(double p_value, int p_row) {
	_set_level_property(p_row, SNAME("visibility_range_end"), p_value, true);
}

void FoliageLODsDialog::_show_popup(PopupPanel *p_popup, Button *p_from, int p_row) {
	popup_row = p_row;
	_update_popups();
	p_popup->set_position(p_from->get_screen_position() + Vector2(0, p_from->get_size().y));
	p_popup->reset_size();
	p_popup->popup();
}

void FoliageLODsDialog::_fade_pressed(int p_row) {
	if (p_row >= 0 && p_row < (int)rows.size()) {
		_show_popup(fade_popup, rows[p_row].fade_button, p_row);
	}
}

void FoliageLODsDialog::_rendering_pressed(int p_row) {
	if (p_row >= 0 && p_row < (int)rows.size()) {
		_show_popup(rendering_popup, rows[p_row].rendering_button, p_row);
	}
}

void FoliageLODsDialog::_begin_margin_changed(double p_value) {
	_set_level_property(popup_row, SNAME("visibility_range_begin_margin"), p_value, true);
}

void FoliageLODsDialog::_end_margin_changed(double p_value) {
	_set_level_property(popup_row, SNAME("visibility_range_end_margin"), p_value, true);
}

void FoliageLODsDialog::_fade_mode_selected(int p_index) {
	_set_level_property(popup_row, SNAME("visibility_range_fade_mode"), fade_mode_option->get_item_id(p_index), false);
}

void FoliageLODsDialog::_material_changed(const Ref<Resource> &p_material) {
	_set_level_property(popup_row, SNAME("material_override"), p_material, false);
}

void FoliageLODsDialog::_cast_shadows_toggled(bool p_pressed) {
	_set_level_property(popup_row, SNAME("cast_shadows"), p_pressed, false);
}

void FoliageLODsDialog::_add_lod() {
	if (layer.is_null()) {
		return;
	}
	const TypedArray<FoliageLODLevel> old_levels = layer->get_lod_levels();
	TypedArray<FoliageLODLevel> new_levels = old_levels.duplicate();

	Ref<FoliageLODLevel> level;
	level.instantiate();

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(TTR("Add LOD"), UndoRedo::MERGE_DISABLE, layer.ptr());

	// The new LOD takes over where the last one ends, fading in across the
	// same margin the last one fades out across.
	const Ref<FoliageLODLevel> last = old_levels.is_empty() ? Ref<FoliageLODLevel>() : Ref<FoliageLODLevel>(old_levels[old_levels.size() - 1]);
	if (last.is_valid()) {
		float last_end = last->get_visibility_range_end();
		float margin = last->get_visibility_range_end_margin();
		if (last_end <= 0.0f) {
			// The last LOD was drawn out to any distance: it now hands over to
			// the new one some way past where it starts.
			last_end = MAX(last->get_visibility_range_begin() * 2.0f, last->get_visibility_range_begin() + 50.0f);
			margin = MAX(margin, Math::round(last_end * 0.1f));
			ur->add_do_property(last.ptr(), "visibility_range_end", last_end);
			ur->add_undo_property(last.ptr(), "visibility_range_end", last->get_visibility_range_end());
			ur->add_do_property(last.ptr(), "visibility_range_end_margin", margin);
			ur->add_undo_property(last.ptr(), "visibility_range_end_margin", last->get_visibility_range_end_margin());
		}
		level->set_visibility_range_begin(MAX(0.0f, last_end - margin));
		level->set_visibility_range_begin_margin(margin);
		level->set_visibility_range_fade_mode(last->get_visibility_range_fade_mode());
		level->set_cast_shadows(last->is_casting_shadows());
	} else {
		level->set_visibility_range_fade_mode(GeometryInstance3D::VISIBILITY_RANGE_FADE_SELF);
	}
	new_levels.push_back(level);

	ur->add_do_property(layer.ptr(), "lod_levels", new_levels);
	ur->add_undo_property(layer.ptr(), "lod_levels", old_levels);
	ur->commit_action();
}

void FoliageLODsDialog::_remove_lod(int p_row) {
	if (layer.is_null()) {
		return;
	}
	const TypedArray<FoliageLODLevel> old_levels = layer->get_lod_levels();
	if (p_row < 0 || p_row >= old_levels.size()) {
		return;
	}
	TypedArray<FoliageLODLevel> new_levels = old_levels.duplicate();
	new_levels.remove_at(p_row);

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(vformat(TTR("Remove %s"), get_lod_name(p_row)), UndoRedo::MERGE_DISABLE, layer.ptr());
	ur->add_do_property(layer.ptr(), "lod_levels", new_levels);
	ur->add_undo_property(layer.ptr(), "lod_levels", old_levels);
	ur->commit_action();
}

void FoliageLODsDialog::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			_update_theme();
		} break;
	}
}

FoliageLODsDialog::FoliageLODsDialog() {
	set_title(TTR("LODs"));
	set_ok_button_text(TTR("Close"));
	connect(SceneStringName(visibility_changed), callable_mp(this, &FoliageLODsDialog::_visibility_changed));

	VBoxContainer *vbox = memnew(VBoxContainer);
	add_child(vbox);

	hint_label = memnew(Label);
	hint_label->set_text(TTR("Every LOD draws the same painted instances; at any camera distance, the LOD whose Visibility Range covers it is drawn. An End of 0 draws a LOD out to any distance."));
	hint_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	hint_label->set_custom_minimum_size(Size2(600, 0) * EDSCALE);
	vbox->add_child(hint_label);

	grid = memnew(GridContainer);
	grid->set_columns(5);
	vbox->add_child(grid);

	const String headers[] = { String(), TTR("Mesh"), TTR("Visibility Range"), TTR("Rendering Settings"), String() };
	for (const String &header : headers) {
		Label *label = memnew(Label(header));
		label->set_theme_type_variation(SNAME("HeaderSmall"));
		grid->add_child(label);
	}

	// Under the Mesh column, after the rows (see _rebuild_rows()).
	HBoxContainer *add_box = memnew(HBoxContainer);
	add_box->set_alignment(BoxContainer::ALIGNMENT_CENTER);
	vbox->add_child(add_box);
	add_button = memnew(Button);
	add_button->set_text(TTR("Add LOD"));
	add_button->set_tooltip_text(TTR("Add a LOD after the last one, taking over where it ends."));
	add_button->connect(SceneStringName(pressed), callable_mp(this, &FoliageLODsDialog::_add_lod));
	add_box->add_child(add_button);

	const auto make_spin = [](const String &p_tooltip) {
		EditorSpinSlider *spin = memnew(EditorSpinSlider);
		spin->set_min(0.0);
		spin->set_max(4096.0);
		spin->set_step(0.01);
		spin->set_allow_greater(true);
		spin->set_suffix("m");
		spin->set_custom_minimum_size(Size2(140, 0) * EDSCALE);
		spin->set_tooltip_text(p_tooltip);
		return spin;
	};

	fade_popup = memnew(PopupPanel);
	add_child(fade_popup);
	GridContainer *fade_grid = memnew(GridContainer);
	fade_grid->set_columns(2);
	fade_popup->add_child(fade_grid);

	fade_grid->add_child(memnew(Label(TTR("Begin Margin"))));
	begin_margin_spin = make_spin(TTR("How far before Begin this LOD starts fading in (with a fade mode), or the hysteresis around Begin (without one)."));
	begin_margin_spin->connect(SceneStringName(value_changed), callable_mp(this, &FoliageLODsDialog::_begin_margin_changed));
	fade_grid->add_child(begin_margin_spin);

	fade_grid->add_child(memnew(Label(TTR("End Margin"))));
	end_margin_spin = make_spin(TTR("How far past End this LOD keeps fading out (with a fade mode), or the hysteresis around End (without one)."));
	end_margin_spin->connect(SceneStringName(value_changed), callable_mp(this, &FoliageLODsDialog::_end_margin_changed));
	fade_grid->add_child(end_margin_spin);

	fade_grid->add_child(memnew(Label(TTR("Fade Mode"))));
	fade_mode_option = memnew(OptionButton);
	fade_mode_option->add_item(TTR("Disabled"), GeometryInstance3D::VISIBILITY_RANGE_FADE_DISABLED);
	fade_mode_option->add_item(TTR("Self"), GeometryInstance3D::VISIBILITY_RANGE_FADE_SELF);
	fade_mode_option->add_item(TTR("Dependencies"), GeometryInstance3D::VISIBILITY_RANGE_FADE_DEPENDENCIES);
	fade_mode_option->set_tooltip_text(TTR("Self cross-fades this LOD with its neighbors across the margins. Ignored with the FoliagePainter3D's GPU Culling, which switches LODs without a fade."));
	fade_mode_option->connect(SceneStringName(item_selected), callable_mp(this, &FoliageLODsDialog::_fade_mode_selected));
	fade_grid->add_child(fade_mode_option);

	rendering_popup = memnew(PopupPanel);
	add_child(rendering_popup);
	GridContainer *rendering_grid = memnew(GridContainer);
	rendering_grid->set_columns(2);
	rendering_popup->add_child(rendering_grid);

	rendering_grid->add_child(memnew(Label(TTR("Material Override"))));
	material_picker = memnew(EditorResourcePicker);
	material_picker->set_base_type("BaseMaterial3D,ShaderMaterial");
	material_picker->set_custom_minimum_size(Size2(200, 0) * EDSCALE);
	material_picker->set_tooltip_text(TTR("Drawn instead of the mesh's own materials, for this LOD only."));
	material_picker->connect("resource_changed", callable_mp(this, &FoliageLODsDialog::_material_changed));
	rendering_grid->add_child(material_picker);

	rendering_grid->add_child(memnew(Label(TTR("Cast Shadows"))));
	cast_shadows_check = memnew(CheckBox);
	cast_shadows_check->set_text(TTR("On"));
	cast_shadows_check->set_tooltip_text(TTR("Off drops this LOD out of the shadow passes, usually the cheapest way to make far foliage affordable. It cannot cast shadows where the FoliageLayer's own Cast Shadows is off."));
	cast_shadows_check->connect(SceneStringName(toggled), callable_mp(this, &FoliageLODsDialog::_cast_shadows_toggled));
	rendering_grid->add_child(cast_shadows_check);
}

////////////////////////////////////////////////////////////////////////////
// EditorPropertyFoliageLODs

String EditorPropertyFoliageLODs::_make_summary() const {
	const Array levels = get_edited_property_value();
	if (levels.is_empty()) {
		return TTR("No LODs: this FoliageLayer draws nothing.");
	}
	PackedStringArray lines;
	for (int i = 0; i < levels.size(); i++) {
		const Ref<FoliageLODLevel> level = levels[i];
		if (level.is_null()) {
			lines.push_back(vformat(TTR("%s: (empty)"), FoliageLODsDialog::get_lod_name(i)));
			continue;
		}
		lines.push_back(vformat("%s: %s, %s", FoliageLODsDialog::get_lod_name(i), _get_mesh_name(level->get_mesh()), _get_range_text(level)));
	}
	return String("\n").join(lines);
}

void EditorPropertyFoliageLODs::_edit_pressed() {
	FoliageLayer *layer = Object::cast_to<FoliageLayer>(get_edited_object());
	if (dialog != nullptr && layer != nullptr) {
		dialog->edit(Ref<FoliageLayer>(layer));
	}
}

void EditorPropertyFoliageLODs::update_property() {
	const Array levels = get_edited_property_value();
	edit_button->set_text(vformat(TTR("Edit LODs (%d)..."), levels.size()));
	shown_summary = _make_summary();
	summary_label->set_text(shown_summary);
}

bool EditorPropertyFoliageLODs::is_cache_valid() const {
	// A LOD's own mesh or range changing leaves the array of them as it was.
	return EditorProperty::is_cache_valid() && shown_summary == _make_summary();
}

void EditorPropertyFoliageLODs::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			edit_button->set_button_icon(get_editor_theme_icon(SNAME("FoliageLODLevel")));
		} break;
	}
}

EditorPropertyFoliageLODs::EditorPropertyFoliageLODs(FoliageLODsDialog *p_dialog) {
	dialog = p_dialog;

	edit_button = memnew(Button);
	edit_button->set_h_size_flags(SIZE_EXPAND_FILL);
	edit_button->set_clip_text(true);
	edit_button->set_tooltip_text(TTR("Open the LODs of this FoliageLayer in a window: the mesh each one draws, the distances it is drawn across, and how it renders."));
	edit_button->set_disabled(dialog == nullptr);
	edit_button->connect(SceneStringName(pressed), callable_mp(this, &EditorPropertyFoliageLODs::_edit_pressed));
	add_child(edit_button);
	add_focusable(edit_button);

	summary_box = memnew(MarginContainer);
	summary_box->add_theme_constant_override("margin_left", 4 * EDSCALE);
	summary_label = memnew(Label);
	summary_label->set_modulate(Color(1, 1, 1, 0.65));
	summary_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	summary_box->add_child(summary_label);
	add_child(summary_box);
	set_bottom_editor(summary_box);
}

////////////////////////////////////////////////////////////////////////////
// EditorPropertyFoliageLayers

TypedArray<FoliageLayer> EditorPropertyFoliageLayers::_get_layers() const {
	return get_edited_property_value();
}

String EditorPropertyFoliageLayers::_get_layer_name(const Ref<FoliageLayer> &p_layer, int p_index) {
	if (p_layer.is_null()) {
		return vformat(TTR("%d: (empty)"), p_index);
	}
	return p_layer->get_layer_name().is_empty() ? vformat("FoliageLayer %d", p_index) : p_layer->get_layer_name();
}

String EditorPropertyFoliageLayers::_get_fold_key(int p_index) const {
	return vformat("%s/%d", get_edited_property(), p_index);
}

bool EditorPropertyFoliageLayers::_is_layer_unfolded(int p_index) const {
	// The fold state is kept on the edited object, as the inspector does its own sections'.
	Object *object = const_cast<EditorPropertyFoliageLayers *>(this)->get_edited_object();
	return object != nullptr && object->editor_is_section_unfolded(_get_fold_key(p_index));
}

Vector<bool> EditorPropertyFoliageLayers::_get_unfolded() const {
	Vector<bool> unfolded;
	const int count = _get_layers().size();
	unfolded.resize(count);
	for (int i = 0; i < count; i++) {
		unfolded.write[i] = _is_layer_unfolded(i);
	}
	return unfolded;
}

bool EditorPropertyFoliageLayers::_sections_match(const TypedArray<FoliageLayer> &p_layers) const {
	if ((int)sections.size() != p_layers.size()) {
		return false;
	}
	for (int i = 0; i < p_layers.size(); i++) {
		if (sections[i].layer != Ref<FoliageLayer>(p_layers[i])) {
			return false;
		}
	}
	return true;
}

void EditorPropertyFoliageLayers::_clear_sections() {
	for (Section &section : sections) {
		// Queued rather than deleted outright: it may be one of the section's
		// own buttons that asked for this.
		sections_box->remove_child(section.box);
		section.box->queue_free();
	}
	sections.clear();
}

void EditorPropertyFoliageLayers::_rebuild_sections(const TypedArray<FoliageLayer> &p_layers) {
	_clear_sections();

	for (int i = 0; i < p_layers.size(); i++) {
		Section section;
		section.layer = p_layers[i];

		section.box = memnew(VBoxContainer);
		section.box->add_theme_constant_override("separation", 0);
		sections_box->add_child(section.box);

		section.header = memnew(PanelContainer);
		if (header_style.is_valid()) {
			section.header->add_theme_style_override(SceneStringName(panel), header_style);
		}
		section.box->add_child(section.header);

		HBoxContainer *header_hbox = memnew(HBoxContainer);
		section.header->add_child(header_hbox);

		section.fold_button = memnew(Button);
		section.fold_button->set_flat(true);
		section.fold_button->set_h_size_flags(SIZE_EXPAND_FILL);
		section.fold_button->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
		section.fold_button->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
		section.fold_button->connect(SceneStringName(pressed), callable_mp(this, &EditorPropertyFoliageLayers::_fold_pressed).bind(i));
		SET_DRAG_FORWARDING_CD(section.fold_button, EditorPropertyFoliageLayers);
		header_hbox->add_child(section.fold_button);

		section.count_label = memnew(Label);
		section.count_label->set_modulate(Color(1, 1, 1, 0.65));
		section.count_label->set_tooltip_text(TTR("How many instances of this FoliageLayer are painted."));
		section.count_label->set_mouse_filter(MOUSE_FILTER_STOP);
		header_hbox->add_child(section.count_label);

		section.menu_button = memnew(MenuButton);
		section.menu_button->set_flat(true);
		section.menu_button->set_button_icon(get_editor_theme_icon(SNAME("GuiTabMenuHl")));
		section.menu_button->set_tooltip_text(TTR("More options for this FoliageLayer."));
		PopupMenu *popup = section.menu_button->get_popup();
		popup->add_icon_item(get_editor_theme_icon(SNAME("MoveUp")), TTR("Move Up"), LAYER_MENU_MOVE_UP);
		popup->add_icon_item(get_editor_theme_icon(SNAME("MoveDown")), TTR("Move Down"), LAYER_MENU_MOVE_DOWN);
		popup->add_icon_item(get_editor_theme_icon(SNAME("Duplicate")), TTR("Duplicate"), LAYER_MENU_DUPLICATE);
		popup->add_separator();
		popup->add_icon_item(get_editor_theme_icon(SNAME("Save")), TTR("Save As..."), LAYER_MENU_SAVE_AS);
		popup->set_item_disabled(popup->get_item_index(LAYER_MENU_MOVE_UP), i == 0);
		popup->set_item_disabled(popup->get_item_index(LAYER_MENU_MOVE_DOWN), i == p_layers.size() - 1);
		popup->set_item_disabled(popup->get_item_index(LAYER_MENU_DUPLICATE), section.layer.is_null());
		popup->set_item_disabled(popup->get_item_index(LAYER_MENU_SAVE_AS), section.layer.is_null());
		popup->connect(SceneStringName(id_pressed), callable_mp(this, &EditorPropertyFoliageLayers::_layer_menu_id_pressed).bind(i));
		section.menu_button->set_disabled(is_read_only());
		header_hbox->add_child(section.menu_button);

		section.remove_button = memnew(Button);
		section.remove_button->set_flat(true);
		section.remove_button->set_button_icon(get_editor_theme_icon(SNAME("Remove")));
		section.remove_button->set_tooltip_text(TTR("Remove this FoliageLayer, along with the instances painted with it."));
		section.remove_button->set_disabled(is_read_only());
		section.remove_button->connect(SceneStringName(pressed), callable_mp(this, &EditorPropertyFoliageLayers::_remove_layer).bind(i));
		header_hbox->add_child(section.remove_button);

		if (section.layer.is_null()) {
			// An empty slot (left over from the generic array editor) can only
			// be filled in or removed.
			MarginContainer *margin = memnew(MarginContainer);
			margin->add_theme_constant_override("margin_left", 8 * EDSCALE);
			section.box->add_child(margin);
			section.create_button = memnew(Button);
			section.create_button->set_text(TTR("New FoliageLayer"));
			section.create_button->set_button_icon(get_editor_theme_icon(SNAME("Add")));
			section.create_button->set_disabled(is_read_only());
			section.create_button->connect(SceneStringName(pressed), callable_mp(this, &EditorPropertyFoliageLayers::_create_layer).bind(i));
			margin->add_child(section.create_button);
		}

		sections.push_back(section);
	}
}

void EditorPropertyFoliageLayers::_update_section(int p_index) {
	Section &section = sections[p_index];
	const bool unfolded = section.layer.is_valid() && _is_layer_unfolded(p_index);

	section.shown_name = _get_layer_name(section.layer, p_index);
	section.fold_button->set_text(section.shown_name);
	section.fold_button->add_theme_font_override(SceneStringName(font), get_theme_font(SNAME("bold"), EditorStringName(EditorFonts)));
	section.fold_button->set_button_icon(get_editor_theme_icon(unfolded ? SNAME("GuiTreeArrowDown") : SNAME("GuiTreeArrowRight")));
	section.fold_button->set_disabled(section.layer.is_null());

	section.shown_count = section.layer.is_valid() ? section.layer->get_instance_count() : 0;
	section.count_label->set_text(section.layer.is_valid() ? vformat(TTR("%d instances"), section.shown_count) : String());

	if (unfolded && section.inspector == nullptr) {
		// The layer's own settings, right under its name. Its LODs come up as
		// a single button here, opening FoliageLODsDialog.
		section.inspector = memnew(EditorInspector);
		section.inspector->set_vertical_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
		section.inspector->set_show_categories(false, false);
		section.inspector->set_use_doc_hints(true);
		section.inspector->set_hide_script(true);
		section.inspector->set_hide_metadata(true);
		EditorInspector *parent_inspector = get_parent_inspector();
		if (parent_inspector) {
			section.inspector->set_root_inspector(parent_inspector->get_root_inspector());
		}
		section.inspector->set_property_name_style(InspectorDock::get_singleton()->get_property_name_style());
		section.inspector->set_read_only(is_read_only());
		section.inspector->set_use_folding(is_using_folding());
		section.inspector->set_draw_focus_border(false);
		section.inspector->set_focus_mode(FocusMode::FOCUS_NONE);
		section.box->add_child(section.inspector);
		section.inspector->edit(section.layer.ptr());
		section.inspector->set_category_color_level(get_sub_inspector_color_level());
	} else if (!unfolded && section.inspector != nullptr) {
		section.box->remove_child(section.inspector);
		section.inspector->queue_free();
		section.inspector = nullptr;
	}
}

void EditorPropertyFoliageLayers::_update_header_style() {
	Ref<StyleBoxFlat> style;
	style.instantiate();
	style->set_bg_color(get_theme_color(SNAME("prop_subsection"), EditorStringName(Editor)));
	style->set_corner_radius_all(int(3 * EDSCALE));
	style->set_content_margin_all(2 * EDSCALE);
	header_style = style;
	for (Section &section : sections) {
		section.header->add_theme_style_override(SceneStringName(panel), header_style);
	}
}

void EditorPropertyFoliageLayers::update_property() {
	const TypedArray<FoliageLayer> layers = _get_layers();
	edit->set_text(vformat(TTR("FoliageLayer (size %d)"), layers.size()));

	const bool unfolded = get_edited_object()->editor_is_section_unfolded(get_edited_property());
	if (edit->is_pressed() != unfolded) {
		edit->set_pressed(unfolded);
	}

	if (!unfolded) {
		if (container) {
			set_bottom_editor(nullptr);
			sections.clear();
			memdelete(container);
			container = nullptr;
			sections_box = nullptr;
			add_button = nullptr;
		}
		return;
	}

	if (!container) {
		container = memnew(PanelContainer);
		add_child(container);
		set_bottom_editor(container);

		VBoxContainer *vbox = memnew(VBoxContainer);
		vbox->set_theme_type_variation(SNAME("EditorPropertyContainer"));
		container->add_child(vbox);

		sections_box = memnew(VBoxContainer);
		sections_box->set_h_size_flags(SIZE_EXPAND_FILL);
		vbox->add_child(sections_box);

		add_button = memnew(EditorInspectorActionButton(TTRC("Add FoliageLayer"), SNAME("Add")));
		add_button->set_tooltip_text(TTR("Add a new FoliageLayer to paint with. A FoliageLayer saved as a resource can also be dropped here."));
		add_button->set_disabled(is_read_only());
		add_button->connect(SceneStringName(pressed), callable_mp(this, &EditorPropertyFoliageLayers::_add_layer));
		SET_DRAG_FORWARDING_CD(add_button, EditorPropertyFoliageLayers);
		vbox->add_child(add_button);
	}

	if (!_sections_match(layers)) {
		_rebuild_sections(layers);
	}
	sections_box->set_visible(!sections.is_empty());
	for (int i = 0; i < (int)sections.size(); i++) {
		_update_section(i);
	}
}

bool EditorPropertyFoliageLayers::is_cache_valid() const {
	if (!EditorProperty::is_cache_valid()) {
		return false;
	}
	// A layer's name or instance count changing leaves the array as it was.
	for (int i = 0; i < (int)sections.size(); i++) {
		const Section &section = sections[i];
		if (section.layer.is_valid() && (section.shown_name != _get_layer_name(section.layer, i) || section.shown_count != section.layer->get_instance_count())) {
			return false;
		}
	}
	return true;
}

void EditorPropertyFoliageLayers::update_properties_recursive() {
	update_property();
	for (Section &section : sections) {
		if (section.inspector) {
			section.inspector->update_properties_recursive();
		}
	}
}

void EditorPropertyFoliageLayers::_edit_draw() {
	if (dropping) {
		const Color color = get_theme_color(SNAME("accent_color"), EditorStringName(Editor));
		edit->draw_rect(Rect2(Point2(), edit->get_size()), color, false);
	}
}

void EditorPropertyFoliageLayers::_edit_pressed() {
	get_edited_object()->editor_set_section_unfold(get_edited_property(), edit->is_pressed());
	update_property();
}

void EditorPropertyFoliageLayers::_fold_pressed(int p_index) {
	if (p_index < 0 || p_index >= (int)sections.size() || sections[p_index].layer.is_null()) {
		return;
	}
	get_edited_object()->editor_set_section_unfold(_get_fold_key(p_index), !_is_layer_unfolded(p_index));
	_update_section(p_index);
}

void EditorPropertyFoliageLayers::_commit_layers(const TypedArray<FoliageLayer> &p_layers, const Vector<bool> &p_unfolded) {
	Object *object = get_edited_object();
	const int count = MAX(p_unfolded.size(), _get_layers().size());
	for (int i = 0; i < count; i++) {
		object->editor_set_section_unfold(_get_fold_key(i), i < p_unfolded.size() && p_unfolded[i]);
	}
	emit_changed(get_edited_property(), p_layers);
}

void EditorPropertyFoliageLayers::_add_layer() {
	TypedArray<FoliageLayer> layers = _get_layers().duplicate();
	Ref<FoliageLayer> layer;
	layer.instantiate();
	layer->set_layer_name(vformat("Layer %d", layers.size() + 1));
	layers.push_back(layer);

	// A new layer opens on its settings, ready to be given a mesh.
	Vector<bool> unfolded = _get_unfolded();
	unfolded.push_back(true);
	get_edited_object()->editor_set_section_unfold(get_edited_property(), true);
	_commit_layers(layers, unfolded);
}

void EditorPropertyFoliageLayers::_create_layer(int p_index) {
	TypedArray<FoliageLayer> layers = _get_layers().duplicate();
	if (p_index < 0 || p_index >= layers.size()) {
		return;
	}
	Ref<FoliageLayer> layer;
	layer.instantiate();
	layer->set_layer_name(vformat("Layer %d", p_index + 1));
	layers[p_index] = layer;

	Vector<bool> unfolded = _get_unfolded();
	unfolded.write[p_index] = true;
	_commit_layers(layers, unfolded);
}

void EditorPropertyFoliageLayers::_remove_layer(int p_index) {
	TypedArray<FoliageLayer> layers = _get_layers().duplicate();
	if (p_index < 0 || p_index >= layers.size()) {
		return;
	}
	layers.remove_at(p_index);
	Vector<bool> unfolded = _get_unfolded();
	unfolded.remove_at(p_index);
	_commit_layers(layers, unfolded);
}

void EditorPropertyFoliageLayers::_layer_menu_id_pressed(int p_id, int p_index) {
	TypedArray<FoliageLayer> layers = _get_layers().duplicate();
	if (p_index < 0 || p_index >= layers.size()) {
		return;
	}
	Vector<bool> unfolded = _get_unfolded();

	switch (p_id) {
		case LAYER_MENU_MOVE_UP:
		case LAYER_MENU_MOVE_DOWN: {
			const int other = p_id == LAYER_MENU_MOVE_UP ? p_index - 1 : p_index + 1;
			if (other < 0 || other >= layers.size()) {
				return;
			}
			const Variant moved = layers[p_index];
			layers[p_index] = layers[other];
			layers[other] = moved;
			const bool moved_unfolded = unfolded[p_index];
			unfolded.write[p_index] = unfolded[other];
			unfolded.write[other] = moved_unfolded;
			_commit_layers(layers, unfolded);
		} break;

		case LAYER_MENU_DUPLICATE: {
			const Ref<FoliageLayer> layer = layers[p_index];
			if (layer.is_null()) {
				return;
			}
			// Its LODs are copied along with it, so the two can be told apart
			// later; the meshes and materials they draw are shared.
			Ref<FoliageLayer> copy = layer->duplicate(true);
			copy->set_layer_name(vformat(TTR("%s (Copy)"), _get_layer_name(layer, p_index)));
			layers.insert(p_index + 1, copy);
			unfolded.insert(p_index + 1, true);
			_commit_layers(layers, unfolded);
		} break;

		case LAYER_MENU_SAVE_AS: {
			const Ref<FoliageLayer> layer = layers[p_index];
			if (layer.is_valid()) {
				EditorNode::get_singleton()->save_resource_as(layer);
			}
		} break;
	}
}

bool EditorPropertyFoliageLayers::_is_drop_valid(const Dictionary &p_drag_data) const {
	if (is_read_only()) {
		return false;
	}
	const String drop_type = p_drag_data.get("type", "");
	if (drop_type == "resource") {
		const Ref<FoliageLayer> layer = p_drag_data.get("resource", Variant());
		return layer.is_valid();
	}
	if (drop_type == "files") {
		const PackedStringArray files = p_drag_data.get("files", PackedStringArray());
		if (files.is_empty()) {
			return false;
		}
		for (const String &file : files) {
			const String type = EditorFileSystem::get_singleton()->get_file_type(file);
			if (type.is_empty() || !ClassDB::is_parent_class(type, FoliageLayer::get_class_static())) {
				return false;
			}
		}
		return true;
	}
	return false;
}

bool EditorPropertyFoliageLayers::can_drop_data_fw(const Point2 &p_point, const Variant &p_data, Control *p_from) const {
	return _is_drop_valid(p_data);
}

void EditorPropertyFoliageLayers::drop_data_fw(const Point2 &p_point, const Variant &p_data, Control *p_from) {
	const Dictionary drag_data = p_data;
	TypedArray<FoliageLayer> layers = _get_layers().duplicate();
	Vector<bool> unfolded = _get_unfolded();

	const String drop_type = drag_data.get("type", "");
	if (drop_type == "resource") {
		const Ref<FoliageLayer> layer = drag_data.get("resource", Variant());
		if (layer.is_valid()) {
			layers.push_back(layer);
			unfolded.push_back(false);
		}
	} else if (drop_type == "files") {
		const PackedStringArray files = drag_data.get("files", PackedStringArray());
		for (const String &file : files) {
			const Ref<FoliageLayer> layer = ResourceLoader::load(file);
			if (layer.is_valid()) {
				layers.push_back(layer);
				unfolded.push_back(false);
			}
		}
	}

	if (layers.size() != _get_layers().size()) {
		_commit_layers(layers, unfolded);
	}
}

void EditorPropertyFoliageLayers::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			_update_header_style();
			if (container) {
				// The icons of the sections are picked when they are built.
				for (Section &section : sections) {
					section.menu_button->set_button_icon(get_editor_theme_icon(SNAME("GuiTabMenuHl")));
					section.remove_button->set_button_icon(get_editor_theme_icon(SNAME("Remove")));
				}
				for (int i = 0; i < (int)sections.size(); i++) {
					_update_section(i);
				}
			}
		} break;

		case NOTIFICATION_DRAG_BEGIN: {
			if (is_visible_in_tree() && _is_drop_valid(get_viewport()->gui_get_drag_data())) {
				dropping = true;
				edit->queue_redraw();
			}
		} break;

		case NOTIFICATION_DRAG_END: {
			if (dropping) {
				dropping = false;
				edit->queue_redraw();
			}
		} break;
	}
}

EditorPropertyFoliageLayers::EditorPropertyFoliageLayers() {
	edit = memnew(Button);
	edit->set_accessibility_name(TTRC("Edit"));
	edit->set_h_size_flags(SIZE_EXPAND_FILL);
	edit->set_clip_text(true);
	edit->set_theme_type_variation(SNAME("EditorInspectorButton"));
	edit->set_toggle_mode(true);
	edit->connect(SceneStringName(pressed), callable_mp(this, &EditorPropertyFoliageLayers::_edit_pressed));
	edit->connect(SceneStringName(draw), callable_mp(this, &EditorPropertyFoliageLayers::_edit_draw));
	SET_DRAG_FORWARDING_CD(edit, EditorPropertyFoliageLayers);
	add_child(edit);
	add_focusable(edit);

	has_borders = true;
}

////////////////////////////////////////////////////////////////////////////
// EditorInspectorPluginFoliagePainter3D

bool EditorInspectorPluginFoliagePainter3D::can_handle(Object *p_object) {
	return Object::cast_to<FoliagePainter3D>(p_object) != nullptr || Object::cast_to<FoliageLayer>(p_object) != nullptr;
}

bool EditorInspectorPluginFoliagePainter3D::parse_property(Object *p_object, const Variant::Type p_type, const String &p_path, const PropertyHint p_hint, const String &p_hint_text, const BitField<PropertyUsageFlags> p_usage, const bool p_wide) {
	if (Object::cast_to<FoliagePainter3D>(p_object) != nullptr && p_path == "foliage_layers") {
		add_property_editor(p_path, memnew(EditorPropertyFoliageLayers), false, "FoliageLayers");
		return true;
	}
	if (Object::cast_to<FoliageLayer>(p_object) != nullptr && p_path == "lod_levels") {
		add_property_editor(p_path, memnew(EditorPropertyFoliageLODs(lods_dialog)), false, TTR("LODs"));
		return true;
	}
	return false;
}
