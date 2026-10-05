/**************************************************************************/
/*  foliage_inspector_plugin.h                                            */
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

#include "core/templates/local_vector.h"
#include "editor/inspector/editor_inspector.h"
#include "scene/3d/foliage_layer.h"
#include "scene/gui/dialogs.h"

class Button;
class CheckBox;
class EditorResourcePicker;
class EditorSpinSlider;
class GridContainer;
class Label;
class MarginContainer;
class MenuButton;
class OptionButton;
class PanelContainer;
class PopupPanel;
class VBoxContainer;

// Edits the LODs of a FoliageLayer or a FoliageSpawner3D (anything with a
// lod_levels array of FoliageLODLevels) in a window of their own, one row per
// LOD: its mesh, the distances it is drawn across, and how it renders. This
// takes the LODs out of the inspector, where each would otherwise be one more
// resource to open, and a layer inside a FoliagePainter3D nests them two
// levels deeper still.
class FoliageLODsDialog : public AcceptDialog {
	GDCLASS(FoliageLODsDialog, AcceptDialog);

	struct Row {
		Ref<FoliageLODLevel> level;
		Label *name_label = nullptr;
		EditorResourcePicker *mesh_picker = nullptr;
		EditorSpinSlider *begin_spin = nullptr;
		EditorSpinSlider *end_spin = nullptr;
		Button *fade_button = nullptr;
		Button *rendering_button = nullptr;
		Button *remove_button = nullptr;
	};

	// What the LODs belong to. By ID, since a FoliageSpawner3D is a node that
	// can be deleted while the window is open.
	ObjectID owner_id;

	Label *hint_label = nullptr;
	GridContainer *grid = nullptr;
	Button *add_button = nullptr;
	LocalVector<Row> rows;
	// Set while the rows are being filled in from the LODs, so that doing so
	// is not taken for an edit.
	bool updating = false;
	bool refresh_queued = false;

	// The parts of a row that are tweaked less often, each in a popup of its
	// own opened from that row (popup_row).
	int popup_row = -1;
	PopupPanel *fade_popup = nullptr;
	EditorSpinSlider *begin_margin_spin = nullptr;
	EditorSpinSlider *end_margin_spin = nullptr;
	OptionButton *fade_mode_option = nullptr;
	PopupPanel *rendering_popup = nullptr;
	EditorResourcePicker *material_picker = nullptr;
	CheckBox *cast_shadows_check = nullptr;

	Object *_get_owner() const;
	TypedArray<FoliageLODLevel> _get_levels() const;
	// The LODs are only ever changed through undo/redo (here, or in the
	// inspector), so its history changing is when the rows may be out of date.
	void _history_changed();
	void _refresh();
	bool _rows_match() const;
	void _rebuild_rows();
	void _update_rows();
	void _update_popups();
	void _update_theme();

	Ref<FoliageLODLevel> _get_row_level(int p_row) const;
	void _set_level_property(int p_row, const StringName &p_property, const Variant &p_value, bool p_merge);

	void _mesh_changed(const Ref<Resource> &p_mesh, int p_row);
	void _begin_changed(double p_value, int p_row);
	void _end_changed(double p_value, int p_row);
	void _show_popup(PopupPanel *p_popup, Button *p_from, int p_row);
	void _fade_pressed(int p_row);
	void _rendering_pressed(int p_row);
	void _begin_margin_changed(double p_value);
	void _end_margin_changed(double p_value);
	void _fade_mode_selected(int p_index);
	void _material_changed(const Ref<Resource> &p_material);
	void _cast_shadows_toggled(bool p_pressed);

	void _add_lod();
	void _remove_lod(int p_row);

	void _visibility_changed();

protected:
	void _notification(int p_what);

public:
	// Opens the window on the LODs of p_owner, a FoliageLayer or a FoliageSpawner3D.
	void edit(Object *p_owner);

	static String get_lod_name(int p_index);

	FoliageLODsDialog();
};

// FoliageLayer::lod_levels and FoliageSpawner3D::lod_levels in the inspector:
// a button that opens FoliageLODsDialog, over a line per LOD saying what it
// draws and where.
class EditorPropertyFoliageLODs : public EditorProperty {
	GDCLASS(EditorPropertyFoliageLODs, EditorProperty);

	FoliageLODsDialog *dialog = nullptr;

	Button *edit_button = nullptr;
	MarginContainer *summary_box = nullptr;
	Label *summary_label = nullptr;
	// What the summary currently shows, to tell when it is out of date (a
	// LOD's own settings change without the array of them changing).
	String shown_summary;

	String _make_summary() const;
	void _edit_pressed();

protected:
	void _notification(int p_what);

public:
	virtual void update_property() override;
	virtual bool is_cache_valid() const override;

	EditorPropertyFoliageLODs(FoliageLODsDialog *p_dialog);
};

// FoliagePainter3D::foliage_layers in the inspector. Each FoliageLayer gets a
// section of its own, folding open on its settings directly, rather than an
// array slot holding a resource that has to be opened in turn; its LODs are
// one more button away (see EditorPropertyFoliageLODs), not two more levels.
class EditorPropertyFoliageLayers : public EditorProperty {
	GDCLASS(EditorPropertyFoliageLayers, EditorProperty);

	enum LayerMenu {
		LAYER_MENU_MOVE_UP,
		LAYER_MENU_MOVE_DOWN,
		LAYER_MENU_DUPLICATE,
		LAYER_MENU_SAVE_AS,
	};

	struct Section {
		Ref<FoliageLayer> layer;
		VBoxContainer *box = nullptr;
		PanelContainer *header = nullptr;
		Button *fold_button = nullptr;
		Label *count_label = nullptr;
		MenuButton *menu_button = nullptr;
		Button *remove_button = nullptr;
		Button *create_button = nullptr;
		EditorInspector *inspector = nullptr;
		// What the header currently shows (see is_cache_valid()).
		String shown_name;
		int shown_count = -1;
	};

	Button *edit = nullptr;
	PanelContainer *container = nullptr;
	VBoxContainer *sections_box = nullptr;
	Button *add_button = nullptr;
	LocalVector<Section> sections;
	Ref<StyleBox> header_style;
	bool dropping = false;

	TypedArray<FoliageLayer> _get_layers() const;
	static String _get_layer_name(const Ref<FoliageLayer> &p_layer, int p_index);
	String _get_fold_key(int p_index) const;
	bool _is_layer_unfolded(int p_index) const;

	bool _sections_match(const TypedArray<FoliageLayer> &p_layers) const;
	void _clear_sections();
	void _rebuild_sections(const TypedArray<FoliageLayer> &p_layers);
	void _update_section(int p_index);
	void _update_header_style();

	void _edit_pressed();
	void _edit_draw();
	void _fold_pressed(int p_index);
	void _add_layer();
	void _create_layer(int p_index);
	void _remove_layer(int p_index);
	void _layer_menu_id_pressed(int p_id, int p_index);
	// Sets the property to p_layers, with p_unfolded saying which of them are
	// open afterwards (fold state follows a layer when it moves in the list).
	void _commit_layers(const TypedArray<FoliageLayer> &p_layers, const Vector<bool> &p_unfolded);
	Vector<bool> _get_unfolded() const;

	bool _is_drop_valid(const Dictionary &p_drag_data) const;
	bool can_drop_data_fw(const Point2 &p_point, const Variant &p_data, Control *p_from) const;
	void drop_data_fw(const Point2 &p_point, const Variant &p_data, Control *p_from);

protected:
	void _notification(int p_what);

public:
	virtual void update_property() override;
	virtual bool is_cache_valid() const override;
	virtual bool is_colored(ColorationMode p_mode) override { return p_mode == COLORATION_CONTAINER_RESOURCE; }
	virtual void update_properties_recursive() override;

	EditorPropertyFoliageLayers();
};

// The inspector editors above, for FoliagePainter3D, its FoliageLayers, and
// FoliageSpawner3D.
class EditorInspectorPluginFoliage : public EditorInspectorPlugin {
	GDCLASS(EditorInspectorPluginFoliage, EditorInspectorPlugin);

	FoliageLODsDialog *lods_dialog = nullptr;

public:
	virtual bool can_handle(Object *p_object) override;
	virtual bool parse_property(Object *p_object, const Variant::Type p_type, const String &p_path, const PropertyHint p_hint, const String &p_hint_text, const BitField<PropertyUsageFlags> p_usage, const bool p_wide = false) override;

	void set_lods_dialog(FoliageLODsDialog *p_dialog) { lods_dialog = p_dialog; }
};
