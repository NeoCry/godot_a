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
#include "editor/editor_string_names.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/inspector/editor_preview_plugins.h"
#include "editor/inspector/editor_resource_preview.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/themes/editor_scale.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/landscape_spline_3d.h"
#include "scene/3d/physics/static_body_3d.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/grid_container.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/menu_button.h"
#include "scene/gui/option_button.h"
#include "scene/gui/popup.h"
#include "scene/gui/popup_menu.h"
#include "scene/gui/separator.h"
#include "scene/gui/spin_box.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "scene/resources/virtual_texture_2d.h"
#include "scene/scene_string_names.h"
#include "servers/physics_3d/physics_server_3d.h"
#include "servers/rendering/rendering_server.h"

namespace {
// The modes each of the toolbar's two lists offers, in order, with the icon
// each is shown with. Both start with Select, which puts the brush away.
struct LandscapeModeEntry {
	Landscape3DEditorPlugin::Mode mode;
	const char *icon;
};

constexpr LandscapeModeEntry SCULPT_MODES[] = {
	{ Landscape3DEditorPlugin::MODE_SELECT, "ToolSelect" },
	{ Landscape3DEditorPlugin::MODE_RAISE, "LandscapeRaise" },
	{ Landscape3DEditorPlugin::MODE_LOWER, "LandscapeLower" },
	{ Landscape3DEditorPlugin::MODE_SMOOTH, "LandscapeSmooth" },
	{ Landscape3DEditorPlugin::MODE_FLATTEN, "LandscapeFlatten" },
	{ Landscape3DEditorPlugin::MODE_HOLE, "LandscapeHole" },
	{ Landscape3DEditorPlugin::MODE_UNHOLE, "LandscapeUnhole" },
};

constexpr LandscapeModeEntry PAINT_MODES[] = {
	{ Landscape3DEditorPlugin::MODE_SELECT, "ToolSelect" },
	{ Landscape3DEditorPlugin::MODE_PAINT, "LandscapePaint" },
	{ Landscape3DEditorPlugin::MODE_ERASE, "LandscapeErase" },
	{ Landscape3DEditorPlugin::MODE_BLEND, "LandscapeBlend" },
};

// How much one notch of the mouse wheel grows or shrinks the brush.
constexpr float LANDSCAPE_BRUSH_RADIUS_WHEEL_FACTOR = 1.1f;

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

////////////////////////////////////////////////////////////////////////////

bool EditorInspectorPluginLandscape3D::can_handle(Object *p_object) {
	return Object::cast_to<Landscape3D>(p_object) != nullptr;
}

bool EditorInspectorPluginLandscape3D::parse_property(Object *p_object, const Variant::Type p_type, const String &p_path, const PropertyHint p_hint, const String &p_hint_text, const BitField<PropertyUsageFlags> p_usage, const bool p_wide) {
	const Landscape3D *landscape = Object::cast_to<Landscape3D>(p_object);
	if (plugin == nullptr || landscape == nullptr || p_path != "layers") {
		return false;
	}

	struct ToolEntry {
		Landscape3DEditorPlugin::Tool tool;
		const char *icon;
		String text;
		String tooltip;
	};
	const ToolEntry tools[] = {
		{ Landscape3DEditorPlugin::TOOL_IMPORT_HEIGHTMAP, "LandscapeImportHeightmap", TTR("Import Heightmap..."),
				TTR("Import a grayscale image as this terrain's heightmap, replacing the current one and resizing the terrain to match the image. Prefer an EXR or HDR heightmap over PNG: Godot always decodes PNG to 8 bits per channel (256 possible heights), while EXR/HDR keep real height precision.") },
		{ Landscape3DEditorPlugin::TOOL_IMPORT_LAYER_MASK, "LandscapeImportLayerMask", TTR("Import Layer Mask..."),
				TTR("Import a grayscale image as where one TerrainLayer shows, for the texturing masks (slopes, peaks, hollows, roads, fields...) a terrain tool usually exports alongside a heightmap. Unlike a heightmap, a mask never resizes the terrain: one authored at a different resolution is resampled to fit.") },
		{ Landscape3DEditorPlugin::TOOL_GENERATE_LAYER_MASK, "LandscapeGenerateLayerMask", TTR("Generate Layer Mask..."),
				TTR("Build a layer's mask from the terrain's own shape instead of an image: where it sits within a height range and how steep it is there. This is how a terrain gets textured by what it is - rock on cliff faces, snow on peaks, sand in the low flats - without painting or authoring a mask by hand.") },
	};

	// Right under the layers, where the settings now in Project Settings > Landscape3D used to be.
	VBoxContainer *tools_box = memnew(VBoxContainer);
	Label *title = memnew(Label(TTR("Terrain Tools")));
	title->set_theme_type_variation(SNAME("HeaderSmall"));
	tools_box->add_child(title);

	const Ref<Theme> theme = EditorNode::get_singleton()->get_editor_theme();
	for (const ToolEntry &entry : tools) {
		Button *button = memnew(Button);
		button->set_text(entry.text);
		button->set_tooltip_text(entry.tooltip);
		button->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
		if (theme.is_valid()) {
			button->set_button_icon(theme->get_icon(entry.icon, EditorStringName(EditorIcons)));
		}
		button->connect(SceneStringName(pressed), callable_mp(plugin, &Landscape3DEditorPlugin::open_tool).bind((int)entry.tool, landscape->get_instance_id()));
		tools_box->add_child(button);
	}

	add_property_editor(p_path, tools_box, true);
	return false;
}

////////////////////////////////////////////////////////////////////////////

void Landscape3DEditorPlugin::_bind_methods() {
}

bool Landscape3DEditorPlugin::handles(Object *p_object) const {
	return Object::cast_to<Landscape3D>(p_object) != nullptr;
}

void Landscape3DEditorPlugin::edit(Object *p_object) {
	_end_stroke();

	terrain = Object::cast_to<Landscape3D>(p_object);

	cursor_on_terrain = false;
	if (cursor_instance.is_valid()) {
		RS::get_singleton()->instance_set_visible(cursor_instance, false);
	}

	_rebuild_mat_layers_menu();
}

void Landscape3DEditorPlugin::make_visible(bool p_visible) {
	if (p_visible) {
		topmenu_bar->show();
	} else {
		_end_stroke();
		topmenu_bar->hide();
		terrain = nullptr;
		cursor_on_terrain = false;
		if (cursor_instance.is_valid()) {
			RS::get_singleton()->instance_set_visible(cursor_instance, false);
		}
	}
}

bool Landscape3DEditorPlugin::_is_paint_mode(Mode p_mode) {
	return p_mode == MODE_PAINT || p_mode == MODE_ERASE || p_mode == MODE_BLEND;
}

Landscape3DEditorPlugin::Mode Landscape3DEditorPlugin::_get_effective_mode() const {
	if (!inverted) {
		return mode;
	}
	// Shift turns the brush around, as in Unreal Engine's landscape tools.
	switch (mode) {
		case MODE_RAISE:
			return MODE_LOWER;
		case MODE_LOWER:
			return MODE_RAISE;
		case MODE_PAINT:
			return MODE_ERASE;
		case MODE_ERASE:
			return MODE_PAINT;
		case MODE_HOLE:
			return MODE_UNHOLE;
		case MODE_UNHOLE:
			return MODE_HOLE;
		default:
			return mode;
	}
}

void Landscape3DEditorPlugin::_set_mode(Mode p_mode) {
	_end_stroke();
	mode = p_mode;

	// The other list goes back to Select: there is only ever one brush.
	sculpt_mode_option->select(MAX(sculpt_mode_option->get_item_index(_is_paint_mode(mode) ? MODE_SELECT : mode), 0));
	paint_mode_option->select(MAX(paint_mode_option->get_item_index(_is_paint_mode(mode) ? mode : MODE_SELECT), 0));
	mask_button->set_disabled(!_is_paint_mode(mode));

	if (mode == MODE_SELECT) {
		cursor_on_terrain = false;
		if (cursor_instance.is_valid()) {
			RS::get_singleton()->instance_set_visible(cursor_instance, false);
		}
	}
	_update_cursor_color();
}

void Landscape3DEditorPlugin::_mode_selected(int p_index, OptionButton *p_option) {
	_set_mode((Mode)p_option->get_item_id(p_index));
}

void Landscape3DEditorPlugin::_set_inverted(bool p_inverted) {
	if (inverted == p_inverted) {
		return;
	}
	inverted = p_inverted;
	_update_cursor_color();
}

void Landscape3DEditorPlugin::_update_theme() {
	for (int i = 0; i < sculpt_mode_option->get_item_count(); i++) {
		sculpt_mode_option->set_item_icon(i, topmenu_bar->get_editor_theme_icon(SCULPT_MODES[i].icon));
	}
	for (int i = 0; i < paint_mode_option->get_item_count(); i++) {
		paint_mode_option->set_item_icon(i, topmenu_bar->get_editor_theme_icon(PAINT_MODES[i].icon));
	}
	mask_button->set_button_icon(topmenu_bar->get_editor_theme_icon(SNAME("LandscapeMask")));
	add_spline_menu->set_button_icon(topmenu_bar->get_editor_theme_icon(SNAME("Path3D")));
	// Swatches are sized for the editor's scale, and the RVT badge drawn onto them.
	layer_color_swatches.clear();
	_rebuild_mat_layers_menu();
}

void Landscape3DEditorPlugin::_update_cursor_color() {
	if (cursor_material.is_null()) {
		return;
	}
	Color color;
	switch (_get_effective_mode()) {
		case MODE_LOWER:
		case MODE_ERASE:
		case MODE_HOLE:
			// Taking away.
			color = Color(1.0, 0.45, 0.35);
			break;
		case MODE_SMOOTH:
		case MODE_FLATTEN:
		case MODE_BLEND:
			// Evening out.
			color = Color(1.0, 0.8, 0.3);
			break;
		case MODE_PAINT:
		case MODE_UNHOLE:
			color = Color(0.45, 0.95, 0.55);
			break;
		default:
			color = Color(0.2, 0.85, 1.0);
			break;
	}
	color.a = 0.9;
	cursor_material->set_albedo(color);
}

void Landscape3DEditorPlugin::_set_brush_radius(double p_value) {
	brush_radius = MAX(0.01f, (float)p_value);
	if (cursor_on_terrain) {
		_update_cursor(cursor_position, cursor_normal, true);
	}
}

void Landscape3DEditorPlugin::_scale_brush_radius(bool p_grow, float p_notches) {
	const float factor = Math::pow(LANDSCAPE_BRUSH_RADIUS_WHEEL_FACTOR, MAX(p_notches, 0.0001f));
	// At least a step of the field, which a small brush would otherwise round back to.
	const float step = brush_radius_spin->get_step();
	const float radius = p_grow ? MAX(brush_radius * factor, brush_radius + step) : MIN(brush_radius / factor, brush_radius - step);
	brush_radius_spin->set_value(CLAMP(radius, (float)brush_radius_spin->get_min(), (float)brush_radius_spin->get_max()));
}

void Landscape3DEditorPlugin::_radius_spin_gui_input(const Ref<InputEvent> &p_event) {
	// The wheel over the field sizes the brush without having to click into it first.
	const Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_null() || !mb->is_pressed() || brush_radius_spin->get_line_edit()->is_editing()) {
		return;
	}
	if (mb->get_button_index() == MouseButton::WHEEL_UP || mb->get_button_index() == MouseButton::WHEEL_DOWN) {
		_scale_brush_radius(mb->get_button_index() == MouseButton::WHEEL_UP, mb->get_factor());
		brush_radius_spin->accept_event();
	}
}

void Landscape3DEditorPlugin::_set_brush_strength(double p_value) {
	brush_strength = MAX(0.001f, (float)p_value);
}

void Landscape3DEditorPlugin::_set_brush_falloff(double p_value) {
	brush_falloff = CLAMP((float)p_value, 0.0f, 1.0f);
}

void Landscape3DEditorPlugin::_mask_button_pressed() {
	// A height band starts out on the heights the terrain actually has, rather than on numbers
	// picked out of the air.
	if (!mask_height_check->is_pressed() && terrain != nullptr && terrain->get_terrain_data().is_valid()) {
		const Vector2 range = terrain->get_terrain_data()->get_height_range();
		mask_height_min_spin->set_value_no_signal(range.x);
		mask_height_max_spin->set_value_no_signal(range.y);
	}

	const Rect2 rect = mask_button->get_screen_rect();
	mask_popup->set_position(rect.position + Vector2(0, rect.size.y));
	mask_popup->reset_size();
	mask_popup->popup();
}

void Landscape3DEditorPlugin::_mask_changed() {
	_update_mask_button();
}

void Landscape3DEditorPlugin::_update_mask_button() {
	const bool height = mask_height_check->is_pressed();
	const bool slope = mask_slope_check->is_pressed();
	if (height && slope) {
		mask_button->set_text(TTR("Mask: Height + Angle"));
	} else if (height) {
		mask_button->set_text(TTR("Mask: Height"));
	} else if (slope) {
		mask_button->set_text(TTR("Mask: Angle"));
	} else {
		mask_button->set_text(TTR("Mask"));
	}
}

Landscape3D::PaintMask Landscape3DEditorPlugin::_get_paint_mask() const {
	Landscape3D::PaintMask mask;
	if (mask_height_check->is_pressed()) {
		mask.height_min = mask_height_min_spin->get_value();
		mask.height_max = mask_height_max_spin->get_value();
		mask.height_falloff = mask_height_falloff_spin->get_value();
	}
	if (mask_slope_check->is_pressed()) {
		mask.slope_min = mask_slope_min_spin->get_value();
		mask.slope_max = mask_slope_max_spin->get_value();
		mask.slope_falloff = mask_slope_falloff_spin->get_value();
	}
	return mask;
}

String Landscape3DEditorPlugin::_get_layer_name(const Ref<TerrainLayer> &p_layer, int p_index) {
	return (p_layer.is_valid() && !p_layer->get_layer_name().is_empty()) ? p_layer->get_layer_name() : vformat("Layer %d", p_index);
}

bool Landscape3DEditorPlugin::_layer_uses_virtual_textures(const Ref<TerrainLayer> &p_layer) {
	if (p_layer.is_null()) {
		return false;
	}
	const Ref<Texture2D> textures[] = { p_layer->get_albedo_texture(), p_layer->get_normal_texture(), p_layer->get_orm_texture(), p_layer->get_height_texture() };
	for (const Ref<Texture2D> &texture : textures) {
		if (Object::cast_to<VirtualTexture2D>(texture.ptr()) != nullptr) {
			return true;
		}
	}
	return false;
}

Ref<Texture2D> Landscape3DEditorPlugin::_get_layer_icon(const Ref<TerrainLayer> &p_layer) {
	if (p_layer.is_null()) {
		return topmenu_bar->get_editor_theme_icon(SNAME("LandscapeMatLayers"));
	}

	const Ref<Texture2D> albedo = p_layer->get_albedo_texture();
	if (albedo.is_valid()) {
		const Ref<Texture2D> *preview = layer_texture_previews.getptr(albedo->get_instance_id());
		if (preview == nullptr || preview->is_null()) {
			// Its thumbnail is on its way (see _rebuild_mat_layers_menu()).
			return topmenu_bar->get_editor_theme_icon(SNAME("TerrainLayer"));
		}
		// A virtual texture's own thumbnail already carries the badge.
		if (!_layer_uses_virtual_textures(p_layer) || Object::cast_to<VirtualTexture2D>(albedo.ptr()) != nullptr) {
			return *preview;
		}
		Ref<Image> image = (*preview)->get_image();
		if (image.is_null()) {
			return *preview;
		}
		image = image->duplicate();
		add_virtual_texture_badge(image);
		return ImageTexture::create_from_image(image);
	}

	// No texture to show: a swatch of the layer's color.
	const bool rvt = _layer_uses_virtual_textures(p_layer);
	const Color color = p_layer->get_albedo_color();
	const Color key = rvt ? Color(color.r, color.g, color.b, -1.0) : color;
	if (const Ref<Texture2D> *swatch = layer_color_swatches.getptr(key)) {
		return *swatch;
	}
	const int size = MAX(16, (int)Math::round(16 * EDSCALE));
	Ref<Image> image = Image::create_empty(size, size, false, Image::FORMAT_RGBA8);
	image->fill(Color(color.r, color.g, color.b, 1.0));
	if (rvt) {
		add_virtual_texture_badge(image);
	}
	const Ref<Texture2D> swatch = ImageTexture::create_from_image(image);
	layer_color_swatches[key] = swatch;
	return swatch;
}

void Landscape3DEditorPlugin::_rebuild_mat_layers_menu() {
	PopupMenu *popup = mat_layers_menu->get_popup();
	popup->clear();

	const Ref<Texture2D> menu_icon = topmenu_bar->get_editor_theme_icon(SNAME("LandscapeMatLayers"));
	if (terrain == nullptr || terrain->get_layers().is_empty()) {
		mat_layers_menu->set_text(TTR("MatLayers: None"));
		mat_layers_menu->set_button_icon(menu_icon);
		return;
	}

	const TypedArray<TerrainLayer> layers = terrain->get_layers();
	const int used = terrain->get_used_layer_count();
	paint_layer_index = CLAMP(paint_layer_index, 0, MAX(used - 1, 0));

	// Thumbnails come from EditorResourcePreview, which makes them in the
	// background and keeps them; one it already has is handed back right away.
	for (int i = 0; i < layers.size(); i++) {
		const Ref<TerrainLayer> layer = layers[i];
		const Ref<Texture2D> albedo = layer.is_valid() ? layer->get_albedo_texture() : Ref<Texture2D>();
		if (albedo.is_valid() && !layer_texture_previews.has(albedo->get_instance_id())) {
			layer_texture_previews[albedo->get_instance_id()] = Ref<Texture2D>();
			EditorResourcePreview::get_singleton()->queue_edited_resource_preview(albedo, callable_mp(this, &Landscape3DEditorPlugin::_layer_preview_done).bind(albedo->get_instance_id()));
		}
	}

	String current_name;
	Ref<Texture2D> current_icon = menu_icon;
	for (int i = 0; i < layers.size(); i++) {
		const Ref<TerrainLayer> layer = layers[i];
		const String name = _get_layer_name(layer, i);
		const Ref<Texture2D> icon = _get_layer_icon(layer);
		popup->add_icon_radio_check_item(icon, vformat("%d. %s", i, name), i);
		const int index = popup->get_item_index(i);
		popup->set_item_checked(index, i == paint_layer_index);

		String tooltip;
		if (i >= used) {
			// Kept, but neither drawn nor painted.
			popup->set_item_disabled(index, true);
			tooltip = vformat(TTR("Not used: Project Settings > Landscape3D > Material Layers > Max Layers allows %d layers."), terrain->get_max_material_layers());
		} else if (_layer_uses_virtual_textures(layer)) {
			tooltip = TTR("RVT: some of this layer's textures are virtual textures (converted with FileSystem > Convert to Virtual Texture). They are put back together at full resolution to build the terrain's texture arrays.");
		}
		popup->set_item_tooltip(index, tooltip);

		if (i == paint_layer_index) {
			current_name = name;
			current_icon = icon;
		}
	}
	mat_layers_menu->set_text(vformat(TTR("MatLayers: %s"), current_name));
	mat_layers_menu->set_button_icon(current_icon);
}

void Landscape3DEditorPlugin::_layer_preview_done(const String &p_path, const Ref<Texture2D> &p_preview, const Ref<Texture2D> &p_small_preview, ObjectID p_texture) {
	if (p_small_preview.is_null() && p_preview.is_null()) {
		return;
	}
	layer_texture_previews[p_texture] = p_small_preview.is_valid() ? p_small_preview : p_preview;
	// Deferred: a preview EditorResourcePreview already has arrives while the menu is being built.
	callable_mp(this, &Landscape3DEditorPlugin::_rebuild_mat_layers_menu).call_deferred();
}

void Landscape3DEditorPlugin::_mat_layers_menu_id_pressed(int p_id) {
	paint_layer_index = p_id;
	_rebuild_mat_layers_menu();
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
	// A lake's curve is its shoreline, which goes all the way around.
	curve->set_closed(type == LandscapeSpline3D::TYPE_LAKE);
	spline->set_curve(curve);
	static const char *names[LandscapeSpline3D::TYPE_MAX] = { "Road", "River", "Stream", "Lake" };
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

Landscape3D *Landscape3DEditorPlugin::_get_tool_terrain() const {
	return ObjectDB::get_instance<Landscape3D>(tool_terrain);
}

void Landscape3DEditorPlugin::open_tool(int p_tool, ObjectID p_terrain) {
	Landscape3D *target = ObjectDB::get_instance<Landscape3D>(p_terrain);
	if (target == nullptr) {
		return;
	}
	tool_terrain = p_terrain;

	switch ((Tool)p_tool) {
		case TOOL_IMPORT_HEIGHTMAP: {
			import_file_dialog->popup_file_dialog();
		} break;

		case TOOL_IMPORT_LAYER_MASK: {
			if (target->get_layers().is_empty()) {
				EditorNode::get_singleton()->show_warning(TTR("This Landscape3D has no TerrainLayers yet. Add at least one layer before importing a mask for it."));
				return;
			}
			mask_file_dialog->popup_file_dialog();
		} break;

		case TOOL_GENERATE_LAYER_MASK: {
			Ref<TerrainData> terrain_data = target->get_terrain_data();
			if (terrain_data.is_null()) {
				EditorNode::get_singleton()->show_warning(TTR("This Landscape3D has no TerrainData to generate a mask from. Import or sculpt a heightmap first."));
				return;
			}
			if (!_fill_layer_option(target, generate_layer_option)) {
				EditorNode::get_singleton()->show_warning(TTR("This Landscape3D has no TerrainLayers yet. Add at least one layer before generating a mask for it."));
				return;
			}

			// The height band is meaningless without knowing what the terrain actually
			// spans, and nothing else in the editor shows that, so the dialog states it
			// and starts the band on it rather than on numbers picked out of the air.
			const Vector2 range = terrain_data->get_height_range();
			generate_range_label->set_text(vformat(TTR("This terrain spans %.2f m to %.2f m."), range.x, range.y));
			generate_height_min_spin->set_value(range.x);
			generate_height_max_spin->set_value(range.y);
			_update_curvature_range_label();

			generate_mask_dialog->popup_centered();
		} break;
	}
}

void Landscape3DEditorPlugin::_import_file_selected(const String &p_path) {
	pending_import_path = p_path;
	import_height_range_dialog->popup_centered();
}

void Landscape3DEditorPlugin::_do_import_heightmap() {
	Landscape3D *target = _get_tool_terrain();
	if (target == nullptr || pending_import_path.is_empty()) {
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
	if (target->get_terrain_data().is_null()) {
		Ref<TerrainData> new_data;
		new_data.instantiate();
		target->set_terrain_data(new_data);
	}

	target->get_terrain_data()->import_heightmap(image, height_min, height_max);
}

bool Landscape3DEditorPlugin::_fill_layer_option(Landscape3D *p_terrain, OptionButton *p_option) {
	// The layer list is whatever the terrain holds right now, so it is filled
	// in on every popup rather than once at startup (same reason
	// _rebuild_mat_layers_menu runs on every popup).
	p_option->clear();
	if (p_terrain == nullptr) {
		return false;
	}
	const TypedArray<TerrainLayer> layers = p_terrain->get_layers();
	const int used = p_terrain->get_used_layer_count();
	if (used == 0) {
		return false;
	}
	for (int i = 0; i < used; i++) {
		p_option->add_item(vformat("%d: %s", i, _get_layer_name(layers[i], i)), i);
	}
	p_option->select(CLAMP(paint_layer_index, 0, used - 1));
	return true;
}

void Landscape3DEditorPlugin::_mask_file_selected(const String &p_path) {
	// The file dialog is its own window, so the terrain (and its layers) can
	// change while it is open.
	if (!_fill_layer_option(_get_tool_terrain(), mask_layer_option)) {
		return;
	}
	pending_mask_path = p_path;
	mask_options_dialog->popup_centered();
}

void Landscape3DEditorPlugin::_do_import_layer_mask() {
	Landscape3D *target = _get_tool_terrain();
	if (target == nullptr || pending_mask_path.is_empty()) {
		return;
	}
	Ref<TerrainData> terrain_data = target->get_terrain_data();
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
	if (layer_index < 0 || layer_index >= target->get_used_layer_count()) {
		return;
	}

	terrain_data->import_layer_mask(image, layer_index,
			(TerrainData::MaskChannel)mask_channel_option->get_selected_id(),
			mask_normalize_check->is_pressed());
}

void Landscape3DEditorPlugin::_update_curvature_range_label(double p_unused) {
	Landscape3D *target = _get_tool_terrain();
	if (target == nullptr) {
		return;
	}
	Ref<TerrainData> terrain_data = target->get_terrain_data();
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
	Landscape3D *target = _get_tool_terrain();
	if (target == nullptr) {
		return;
	}
	Ref<TerrainData> terrain_data = target->get_terrain_data();
	if (terrain_data.is_null()) {
		return;
	}
	const int layer_index = generate_layer_option->get_selected_id();
	if (layer_index < 0 || layer_index >= target->get_used_layer_count()) {
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
	// Shift only ever turns a brush into another one working on the same data.
	switch (mode) {
		case MODE_PAINT:
		case MODE_ERASE:
		case MODE_BLEND:
			return DataKind::WEIGHTS;
		case MODE_HOLE:
		case MODE_UNHOLE:
			return DataKind::HOLE;
		default:
			return DataKind::HEIGHT;
	}
}

String Landscape3DEditorPlugin::_get_mode_action_name() const {
	switch (stroke_mode) {
		case MODE_PAINT:
			return TTR("Paint Terrain Layer");
		case MODE_ERASE:
			return TTR("Erase Terrain Layer");
		case MODE_BLEND:
			return TTR("Blend Terrain Layers");
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
	// One sample further out than the brush reaches: blending reads its neighbors.
	const int rad_idx = (int)Math::ceil(MAX(p_radius, 0.001f) / spacing) + 2;
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
	snap.region = Rect2i(p_chunk.x * Landscape3D::BLOCK_QUADS, p_chunk.y * Landscape3D::BLOCK_QUADS, Landscape3D::BLOCK_QUADS + 1, Landscape3D::BLOCK_QUADS + 1);
	switch (_get_mode_data_kind()) {
		case DataKind::WEIGHTS: {
			const int layer_count = terrain->get_used_layer_count();
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
	const int cx0 = (int)Math::floor((float)p_vertex_region.position.x / Landscape3D::BLOCK_QUADS);
	const int cz0 = (int)Math::floor((float)p_vertex_region.position.y / Landscape3D::BLOCK_QUADS);
	const int cx1 = (int)Math::floor((float)(p_vertex_region.position.x + p_vertex_region.size.x - 1) / Landscape3D::BLOCK_QUADS);
	const int cz1 = (int)Math::floor((float)(p_vertex_region.position.y + p_vertex_region.size.y - 1) / Landscape3D::BLOCK_QUADS);
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
	cursor_material->set_render_priority(Material::RENDER_PRIORITY_MAX);
	_update_cursor_color();
	RS::get_singleton()->mesh_surface_set_material(cursor_mesh, 0, cursor_material->get_rid());
}

void Landscape3DEditorPlugin::_update_cursor(const Vector3 &p_world_position, const Vector3 &p_world_normal, bool p_visible) {
	if (terrain == nullptr || mode == MODE_SELECT) {
		p_visible = false;
	}
	cursor_on_terrain = p_visible;
	cursor_position = p_world_position;
	cursor_normal = p_world_normal;

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
	const Mode effective_mode = _get_effective_mode();
	if (_is_paint_mode(effective_mode) && paint_layer_index >= terrain->get_used_layer_count()) {
		return;
	}

	_snapshot_region_chunks(_get_brush_vertex_region(p_local_position, brush_radius));

	switch (effective_mode) {
		case MODE_SELECT: {
		} break;
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
		case MODE_HOLE: {
			terrain->set_hole(p_local_position, brush_radius, true, false);
		} break;
		case MODE_UNHOLE: {
			terrain->set_hole(p_local_position, brush_radius, false, false);
		} break;
		case MODE_PAINT: {
			terrain->paint_layer(p_local_position, brush_radius, brush_strength, paint_layer_index, brush_falloff, Landscape3D::PAINT_ADD, _get_paint_mask());
		} break;
		case MODE_ERASE: {
			terrain->paint_layer(p_local_position, brush_radius, brush_strength, paint_layer_index, brush_falloff, Landscape3D::PAINT_ERASE, _get_paint_mask());
		} break;
		case MODE_BLEND: {
			terrain->paint_layer(p_local_position, brush_radius, brush_strength, paint_layer_index, brush_falloff, Landscape3D::PAINT_BLEND, _get_paint_mask());
		} break;
	}
}

void Landscape3DEditorPlugin::_begin_stroke() {
	stroke_active = true;
	stroke_mode = _get_effective_mode();
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
		// Heights go through one batched call each way, which refreshes the
		// collision tiles under the whole stroke once, not once per block.
		TypedArray<Rect2i> height_regions;
		TypedArray<PackedFloat32Array> heights_before;
		TypedArray<PackedFloat32Array> heights_after;
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
					height_regions.push_back(snap.region);
					heights_before.push_back(snap.before_heights);
					heights_after.push_back(terrain->get_height_region(snap.region));
				} break;
			}
		}
		if (!height_regions.is_empty()) {
			ur->add_do_method(terrain, "set_height_regions", height_regions, heights_after, true);
			ur->add_undo_method(terrain, "set_height_regions", height_regions, heights_before, true);
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
		TypedArray<Rect2i> height_regions;
		TypedArray<PackedFloat32Array> heights_before;
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
					height_regions.push_back(kv.value.region);
					heights_before.push_back(kv.value.before_heights);
				} break;
			}
		}
		if (!height_regions.is_empty()) {
			terrain->set_height_regions(height_regions, heights_before, true);
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
	if (terrain == nullptr || !terrain->is_inside_tree() || mode == MODE_SELECT) {
		return EditorPlugin::AFTER_GUI_INPUT_PASS;
	}

	Ref<InputEventKey> k = p_event;
	if (k.is_valid() && k->get_keycode() == Key::SHIFT) {
		// Shown on the cursor as soon as it is held, before anything is painted.
		_set_inverted(k->is_pressed());
		return EditorPlugin::AFTER_GUI_INPUT_PASS;
	}
	if (k.is_valid() && k->is_pressed() && !k->is_echo() && k->get_keycode() == Key::ESCAPE && stroke_active) {
		_cancel_stroke();
		_update_cursor(Vector3(), Vector3(0, 1, 0), false);
		return EditorPlugin::AFTER_GUI_INPUT_STOP;
	}

	Ref<InputEventWithModifiers> with_modifiers = p_event;
	if (with_modifiers.is_valid()) {
		_set_inverted(with_modifiers->is_shift_pressed());
	}

	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && (mb->get_button_index() == MouseButton::WHEEL_UP || mb->get_button_index() == MouseButton::WHEEL_DOWN)) {
		// Over the terrain, the wheel sizes the brush. Ctrl (or Alt) and the
		// wheel still zoom (or change the field of view), as does the wheel
		// anywhere else, and while looking around with the right or middle
		// button held.
		const bool navigating = mb->get_button_mask().has_flag(MouseButtonMask::RIGHT) || mb->get_button_mask().has_flag(MouseButtonMask::MIDDLE);
		if (!mb->is_pressed() || navigating || mb->is_command_or_control_pressed() || mb->is_alt_pressed() || mb->is_meta_pressed()) {
			return EditorPlugin::AFTER_GUI_INPUT_PASS;
		}
		Vector3 local_pos, world_pos, world_normal;
		if (!_raycast_terrain(p_camera, mb->get_position(), local_pos, world_pos, world_normal)) {
			return EditorPlugin::AFTER_GUI_INPUT_PASS;
		}
		_scale_brush_radius(mb->get_button_index() == MouseButton::WHEEL_UP, mb->get_factor());
		_update_cursor(world_pos, world_normal, true);
		return EditorPlugin::AFTER_GUI_INPUT_STOP;
	}

	if (mb.is_valid() && mb->get_button_index() == MouseButton::LEFT) {
		if (mb->is_pressed()) {
			if (_get_effective_mode() == MODE_FLATTEN) {
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
	inspector_plugin.instantiate();
	inspector_plugin->set_plugin(this);
	add_inspector_plugin(inspector_plugin);

	topmenu_bar = memnew(HBoxContainer);
	topmenu_bar->hide();
	topmenu_bar->connect(SceneStringName(theme_changed), callable_mp(this, &Landscape3DEditorPlugin::_update_theme));

	// Two lists of brushes, as Unreal Engine's landscape tools have a Sculpt
	// and a Paint mode: picking one in either puts the other back on Select.
	topmenu_bar->add_child(memnew(Label(TTR("Sculpt:"))));
	sculpt_mode_option = memnew(OptionButton);
	sculpt_mode_option->set_flat(true);
	sculpt_mode_option->set_tooltip_text(TTR("Sculpting brush. Select puts the brush away.\nHold Shift to turn Raise into Lower (and back), and Hole into Unhole.\nThe mouse wheel over the terrain sizes the brush; Ctrl + wheel zooms."));
	const String sculpt_names[] = { TTR("Select"), TTR("Raise"), TTR("Lower"), TTR("Smooth"), TTR("Flatten"), TTR("Hole"), TTR("Unhole") };
	for (int i = 0; i < (int)std_size(SCULPT_MODES); i++) {
		sculpt_mode_option->add_item(sculpt_names[i], SCULPT_MODES[i].mode);
	}
	sculpt_mode_option->set_item_tooltip(1, TTR("Raise the terrain height within the brush."));
	sculpt_mode_option->set_item_tooltip(2, TTR("Lower the terrain height within the brush."));
	sculpt_mode_option->set_item_tooltip(3, TTR("Average the terrain height with its neighbors within the brush."));
	sculpt_mode_option->set_item_tooltip(4, TTR("Flatten the terrain within the brush towards the height at the start of the stroke."));
	sculpt_mode_option->set_item_tooltip(5, TTR("Cut a hole in the terrain within the brush."));
	sculpt_mode_option->set_item_tooltip(6, TTR("Fill in holes within the brush."));
	sculpt_mode_option->connect(SceneStringName(item_selected), callable_mp(this, &Landscape3DEditorPlugin::_mode_selected).bind(sculpt_mode_option));
	topmenu_bar->add_child(sculpt_mode_option);

	topmenu_bar->add_child(memnew(Label(TTR("Paint:"))));
	paint_mode_option = memnew(OptionButton);
	paint_mode_option->set_flat(true);
	paint_mode_option->set_tooltip_text(TTR("Material layer brush, painting the layer chosen in MatLayers. Select puts the brush away.\nHold Shift to turn Paint into Erase (and back).\nThe mouse wheel over the terrain sizes the brush; Ctrl + wheel zooms."));
	const String paint_names[] = { TTR("Select"), TTR("Paint"), TTR("Erase"), TTR("Blend") };
	for (int i = 0; i < (int)std_size(PAINT_MODES); i++) {
		paint_mode_option->add_item(paint_names[i], PAINT_MODES[i].mode);
	}
	paint_mode_option->set_item_tooltip(1, TTR("Paint the chosen material layer within the brush, taking its weight from the other layers."));
	paint_mode_option->set_item_tooltip(2, TTR("Erase the chosen material layer within the brush, bringing back the layers it was painted over."));
	paint_mode_option->set_item_tooltip(3, TTR("Soften the edges between all the material layers within the brush."));
	paint_mode_option->connect(SceneStringName(item_selected), callable_mp(this, &Landscape3DEditorPlugin::_mode_selected).bind(paint_mode_option));
	topmenu_bar->add_child(paint_mode_option);

	topmenu_bar->add_child(memnew(VSeparator));

	topmenu_bar->add_child(memnew(Label(TTR("Radius:"))));
	brush_radius_spin = memnew(SpinBox);
	brush_radius_spin->set_min(0.1);
	brush_radius_spin->set_max(10000.0);
	brush_radius_spin->set_step(0.1);
	brush_radius_spin->set_value(brush_radius);
	brush_radius_spin->set_suffix("m");
	brush_radius_spin->set_tooltip_text(TTR("Brush radius, in meters. Turn the mouse wheel over the terrain, or over this field, to change it."));
	topmenu_bar->add_child(brush_radius_spin);
	brush_radius_spin->connect(SceneStringName(value_changed), callable_mp(this, &Landscape3DEditorPlugin::_set_brush_radius));
	brush_radius_spin->connect(SceneStringName(gui_input), callable_mp(this, &Landscape3DEditorPlugin::_radius_spin_gui_input));

	topmenu_bar->add_child(memnew(Label(TTR("Strength:"))));
	brush_strength_spin = memnew(SpinBox);
	brush_strength_spin->set_min(0.001);
	brush_strength_spin->set_max(1000.0);
	brush_strength_spin->set_step(0.001);
	brush_strength_spin->set_value(brush_strength);
	brush_strength_spin->set_tooltip_text(TTR("Effect applied per brush stamp: meters of height change for Raise/Lower/Flatten, blend amount (0-1 is typical) for Paint, Erase and Blend, and for Smooth the number of averaging passes - 1 takes the edge off, higher values flatten out progressively larger bumps."));
	topmenu_bar->add_child(brush_strength_spin);
	brush_strength_spin->connect(SceneStringName(value_changed), callable_mp(this, &Landscape3DEditorPlugin::_set_brush_strength));

	topmenu_bar->add_child(memnew(Label(TTR("Falloff:"))));
	brush_falloff_spin = memnew(SpinBox);
	brush_falloff_spin->set_min(0.0);
	brush_falloff_spin->set_max(1.0);
	brush_falloff_spin->set_step(0.01);
	brush_falloff_spin->set_value(brush_falloff);
	brush_falloff_spin->set_tooltip_text(TTR("How much of the brush fades out towards its rim. 1 tapers across the whole radius (softest); lower values hold an inner core at full strength and fade only the outside of it; 0 applies at full strength right up to the rim, leaving a hard edge."));
	topmenu_bar->add_child(brush_falloff_spin);
	brush_falloff_spin->connect(SceneStringName(value_changed), callable_mp(this, &Landscape3DEditorPlugin::_set_brush_falloff));

	mask_button = memnew(Button);
	mask_button->set_flat(true);
	mask_button->set_text(TTR("Mask"));
	mask_button->set_disabled(true);
	mask_button->set_tooltip_text(TTR("Paint, Erase and Blend only where the ground is within a range of heights and of slope angles - rock only on steep faces, snow only up high."));
	mask_button->connect(SceneStringName(pressed), callable_mp(this, &Landscape3DEditorPlugin::_mask_button_pressed));
	topmenu_bar->add_child(mask_button);

	mask_popup = memnew(PopupPanel);
	topmenu_bar->add_child(mask_popup);
	GridContainer *mask_grid = memnew(GridContainer);
	mask_grid->set_columns(4);
	mask_popup->add_child(mask_grid);
	mask_grid->add_child(memnew(Control));
	mask_grid->add_child(memnew(Label(TTR("From"))));
	mask_grid->add_child(memnew(Label(TTR("To"))));
	mask_grid->add_child(memnew(Label(TTR("Falloff"))));

	const auto make_mask_spin = [&](double p_min, double p_max, double p_step, double p_value, const String &p_suffix, const String &p_tooltip) {
		SpinBox *spin = memnew(SpinBox);
		spin->set_min(p_min);
		spin->set_max(p_max);
		spin->set_step(p_step);
		spin->set_value(p_value);
		spin->set_suffix(p_suffix);
		spin->set_tooltip_text(p_tooltip);
		spin->set_custom_minimum_size(Size2(90 * EDSCALE, 0));
		spin->connect(SceneStringName(value_changed), callable_mp(this, &Landscape3DEditorPlugin::_mask_changed).unbind(1));
		mask_grid->add_child(spin);
		return spin;
	};

	mask_height_check = memnew(CheckBox);
	mask_height_check->set_text(TTR("Height"));
	mask_height_check->set_tooltip_text(TTR("Only paint where the ground's height, in the terrain's own space, is within this range."));
	mask_height_check->connect(SceneStringName(toggled), callable_mp(this, &Landscape3DEditorPlugin::_mask_changed).unbind(1));
	mask_grid->add_child(mask_height_check);
	mask_height_min_spin = make_mask_spin(-100000.0, 100000.0, 0.01, 0.0, "m", TTR("The lowest ground painted on."));
	mask_height_max_spin = make_mask_spin(-100000.0, 100000.0, 0.01, 100.0, "m", TTR("The highest ground painted on."));
	mask_height_falloff_spin = make_mask_spin(0.0, 100000.0, 0.01, 1.0, "m", TTR("How far beyond each end of the height range the paint fades out, instead of stopping at a hard line."));

	mask_slope_check = memnew(CheckBox);
	mask_slope_check->set_text(TTR("Angle"));
	mask_slope_check->set_tooltip_text(TTR("Only paint where the ground's slope is within this range: 0 is flat ground, 90 a vertical wall."));
	mask_slope_check->connect(SceneStringName(toggled), callable_mp(this, &Landscape3DEditorPlugin::_mask_changed).unbind(1));
	mask_grid->add_child(mask_slope_check);
	mask_slope_min_spin = make_mask_spin(0.0, 90.0, 0.1, 0.0, U"°", TTR("The flattest ground painted on."));
	mask_slope_max_spin = make_mask_spin(0.0, 90.0, 0.1, 90.0, U"°", TTR("The steepest ground painted on."));
	mask_slope_falloff_spin = make_mask_spin(0.0, 90.0, 0.1, 5.0, U"°", TTR("How far beyond each end of the slope range the paint fades out, instead of stopping at a hard line."));

	topmenu_bar->add_child(memnew(VSeparator));

	mat_layers_menu = memnew(MenuButton);
	mat_layers_menu->set_flat(false);
	mat_layers_menu->set_text(TTR("MatLayers: None"));
	mat_layers_menu->set_tooltip_text(TTR("The material layer (TerrainLayer) Paint and Erase apply. Layers marked RVT use virtual textures; layers past the Project Settings > Landscape3D > Material Layers > Max Layers limit are listed but not used."));
	mat_layers_menu->get_popup()->connect(SceneStringName(id_pressed), callable_mp(this, &Landscape3DEditorPlugin::_mat_layers_menu_id_pressed));
	mat_layers_menu->connect("about_to_popup", callable_mp(this, &Landscape3DEditorPlugin::_rebuild_mat_layers_menu));
	topmenu_bar->add_child(mat_layers_menu);

	topmenu_bar->add_child(memnew(VSeparator));

	add_spline_menu = memnew(MenuButton);
	add_spline_menu->set_text(TTR("Add Spline"));
	add_spline_menu->set_tooltip_text(TTR("Add a road, river, stream or lake to this terrain: a LandscapeSpline3D laid along a curve (for a lake, around its shoreline). Place its points with the Path3D tools (turn on their \"Snap to Colliders\" option to drop them onto the terrain), then use \"Apply to Landscape\" to shape the ground under it."));
	add_spline_menu->get_popup()->add_item(TTR("Road"), LandscapeSpline3D::TYPE_ROAD);
	add_spline_menu->get_popup()->add_item(TTR("River"), LandscapeSpline3D::TYPE_RIVER);
	add_spline_menu->get_popup()->add_item(TTR("Stream"), LandscapeSpline3D::TYPE_STREAM);
	add_spline_menu->get_popup()->add_item(TTR("Lake"), LandscapeSpline3D::TYPE_LAKE);
	add_spline_menu->get_popup()->connect(SceneStringName(id_pressed), callable_mp(this, &Landscape3DEditorPlugin::_add_spline_menu_id_pressed));
	topmenu_bar->add_child(add_spline_menu);

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
