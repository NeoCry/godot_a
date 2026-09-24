/**************************************************************************/
/*  vertex_paint_3d_editor_plugin.cpp                                     */
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

#include "vertex_paint_3d_editor_plugin.h"

#include "core/input/input_event.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/themes/editor_scale.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/color_picker.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/label.h"
#include "scene/gui/option_button.h"
#include "scene/gui/separator.h"
#include "scene/gui/spin_box.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "scene/resources/shader.h"
#include "scene/scene_string_names.h"
#include "servers/rendering/rendering_server.h"

// Source of the unshaded preview material. `view_mode` mirrors the plugin's
// ViewMode minus VIEW_MODE_SHADED, which installs no override at all.
static const char *VERTEX_PAINT_PREVIEW_SHADER = R"(
shader_type spatial;
render_mode unshaded, shadows_disabled, ambient_light_disabled, fog_disabled;

uniform int view_mode = 0;

void fragment() {
	vec4 vertex_color = COLOR;
	vec3 shown;
	if (view_mode == 1) {
		shown = vec3(vertex_color.r);
	} else if (view_mode == 2) {
		shown = vec3(vertex_color.g);
	} else if (view_mode == 3) {
		shown = vec3(vertex_color.b);
	} else if (view_mode == 4) {
		shown = vec3(vertex_color.a);
	} else {
		shown = vertex_color.rgb;
	}
	ALBEDO = shown;
}
)";

// Key a vertex position by, so that a mirrored vertex sitting at -0.0 still
// welds with its +0.0 twin: the two compare equal but hash differently, and a
// HashMap needs equal keys to hash alike.
static Vector3 vertex_paint_weld_key(const Vector3 &p_position) {
	return Vector3(
			p_position.x == 0.0f ? 0.0f : p_position.x,
			p_position.y == 0.0f ? 0.0f : p_position.y,
			p_position.z == 0.0f ? 0.0f : p_position.z);
}

void VertexPaint3DEditorPlugin::_bind_methods() {
	// Called back by the undo/redo history, so it has to be reachable by name.
	ClassDB::bind_method(D_METHOD("_apply_vertex_colors", "mesh", "surface", "indices", "colors"), &VertexPaint3DEditorPlugin::_apply_vertex_colors);
}

bool VertexPaint3DEditorPlugin::handles(Object *p_object) const {
	return Object::cast_to<MeshInstance3D>(p_object) != nullptr;
}

void VertexPaint3DEditorPlugin::edit(Object *p_object) {
	MeshInstance3D *new_instance = Object::cast_to<MeshInstance3D>(p_object);
	if (new_instance == mesh_instance) {
		return;
	}

	_end_stroke();
	// Painting is a modal tool: leaving it armed across a selection change
	// would hijack the next click on an unrelated mesh.
	_set_paint_enabled(false);

	mesh_instance = new_instance;
	mesh_instance_id = (mesh_instance != nullptr) ? mesh_instance->get_instance_id() : ObjectID();
	_invalidate_cache();
	_update_toolbar();
}

void VertexPaint3DEditorPlugin::make_visible(bool p_visible) {
	if (p_visible) {
		topmenu_bar->show();
		_update_toolbar();
	} else {
		_end_stroke();
		_set_paint_enabled(false);
		topmenu_bar->hide();
		mesh_instance = nullptr;
		mesh_instance_id = ObjectID();
		_invalidate_cache();
	}
}

void VertexPaint3DEditorPlugin::_update_toolbar() {
	brush_controls->set_visible(paint_enabled);
	paint_enabled_button->set_pressed_no_signal(paint_enabled);
}

void VertexPaint3DEditorPlugin::_paint_toggled(bool p_pressed) {
	_set_paint_enabled(p_pressed);
}

void VertexPaint3DEditorPlugin::_mode_pressed(int p_mode) {
	_end_stroke();
	mode = (Mode)p_mode;
}

void VertexPaint3DEditorPlugin::_channel_toggled(int p_channel) {
	ERR_FAIL_INDEX(p_channel, 4);
	channel_enabled[p_channel] = channel_buttons[p_channel]->is_pressed();
}

void VertexPaint3DEditorPlugin::_view_mode_selected(int p_index) {
	view_mode = (ViewMode)p_index;
	_update_preview();
}

void VertexPaint3DEditorPlugin::_set_brush_color(const Color &p_color) {
	brush_color = p_color;
}

void VertexPaint3DEditorPlugin::_set_brush_radius(double p_value) {
	brush_radius = MAX(0.001f, (float)p_value);
}

void VertexPaint3DEditorPlugin::_set_brush_strength(double p_value) {
	brush_strength = CLAMP((float)p_value, 0.0f, 1.0f);
}

void VertexPaint3DEditorPlugin::_set_brush_hardness(double p_value) {
	brush_hardness = CLAMP((float)p_value, 0.0f, 1.0f);
}

void VertexPaint3DEditorPlugin::_set_paint_enabled(bool p_enabled) {
	if (p_enabled && !paint_enabled) {
		if (!_prepare_target()) {
			// Either something is missing, or a dialog is now asking the user
			// how to proceed; leave the tool off until that is settled.
			_update_toolbar();
			return;
		}
		paint_enabled = true;
	} else if (!p_enabled && paint_enabled) {
		_end_stroke();
		paint_enabled = false;
		_update_cursor(Vector3(), Vector3(0, 1, 0), false);
	} else {
		_update_toolbar();
		return;
	}

	_update_preview();
	_update_toolbar();
}

bool VertexPaint3DEditorPlugin::_is_mesh_external(const Ref<Mesh> &p_mesh) const {
	const String path = p_mesh->get_path();
	if (path.is_empty()) {
		return false; // Only lives in memory, so nothing else owns it.
	}

	const String base = path.get_slice("::", 0);
	if (base.is_empty()) {
		return false;
	}

	// A resource built into the scene being edited is saved along with it, so
	// it is this node's to change. Anything else (a standalone .tres, or a
	// sub-resource of an imported model, whose edits a reimport would discard)
	// is shared and gets a warning first.
	const Node *edited_scene = EditorNode::get_singleton()->get_edited_scene();
	return edited_scene == nullptr || edited_scene->get_scene_file_path() != base;
}

bool VertexPaint3DEditorPlugin::_prepare_target() {
	if (mesh_instance == nullptr) {
		EditorNode::get_singleton()->show_warning(TTR("Select a MeshInstance3D to paint vertex colors on."));
		return false;
	}

	Ref<Mesh> mesh = mesh_instance->get_mesh();
	if (mesh.is_null()) {
		EditorNode::get_singleton()->show_warning(TTR("The selected MeshInstance3D has no mesh."));
		return false;
	}

	Ref<ArrayMesh> array_mesh = mesh;
	if (array_mesh.is_null()) {
		// Procedural meshes (BoxMesh, a CSG bake, ...) regenerate themselves
		// from their parameters, so there is nowhere to keep painted colors.
		convert_dialog->popup_centered();
		return false;
	}

	if (_is_mesh_external(array_mesh) && array_mesh->get_instance_id() != external_accepted_mesh_id) {
		external_mesh_dialog->popup_centered();
		return false;
	}

	target_mesh = array_mesh;
	if (!_ensure_vertex_colors()) {
		_invalidate_cache();
		return false;
	}

	_build_cache();
	if (!cache_valid) {
		EditorNode::get_singleton()->show_warning(TTR("The selected mesh has no paintable surfaces."));
		_invalidate_cache();
		return false;
	}

	return true;
}

void VertexPaint3DEditorPlugin::_convert_to_array_mesh() {
	ERR_FAIL_NULL(mesh_instance);
	Ref<Mesh> mesh = mesh_instance->get_mesh();
	ERR_FAIL_COND(mesh.is_null());

	Ref<ArrayMesh> array_mesh;
	array_mesh.instantiate();
	array_mesh->set_name(mesh->get_name());
	for (int i = 0; i < mesh->get_blend_shape_count(); i++) {
		array_mesh->add_blend_shape(mesh->get_blend_shape_name(i));
	}

	for (int i = 0; i < mesh->get_surface_count(); i++) {
		const Array arrays = mesh->surface_get_arrays(i);
		if (arrays.size() != Mesh::ARRAY_MAX) {
			continue;
		}
		array_mesh->add_surface_from_arrays(mesh->surface_get_primitive_type(i), arrays, mesh->surface_get_blend_shape_arrays(i), mesh->surface_get_lods(i));
		array_mesh->surface_set_material(array_mesh->get_surface_count() - 1, mesh->surface_get_material(i));
	}

	if (array_mesh->get_surface_count() == 0) {
		EditorNode::get_singleton()->show_warning(TTR("The selected mesh has no surfaces to convert."));
		return;
	}

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(TTR("Convert to ArrayMesh"), UndoRedo::MERGE_DISABLE, mesh_instance);
	ur->add_do_method(mesh_instance, "set_mesh", array_mesh);
	ur->add_undo_method(mesh_instance, "set_mesh", mesh);
	ur->commit_action();

	_invalidate_cache();
	_set_paint_enabled(true);
}

void VertexPaint3DEditorPlugin::_make_mesh_unique() {
	ERR_FAIL_NULL(mesh_instance);
	Ref<ArrayMesh> mesh = mesh_instance->get_mesh();
	ERR_FAIL_COND(mesh.is_null());

	// Shallow duplicate on purpose: the geometry becomes this node's own, but
	// the surface materials stay shared with whatever else uses them.
	Ref<ArrayMesh> unique_mesh = mesh->duplicate(false);
	ERR_FAIL_COND(unique_mesh.is_null());

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(TTR("Make Mesh Unique"), UndoRedo::MERGE_DISABLE, mesh_instance);
	ur->add_do_method(mesh_instance, "set_mesh", unique_mesh);
	ur->add_undo_method(mesh_instance, "set_mesh", mesh);
	ur->commit_action();

	_invalidate_cache();
	_set_paint_enabled(true);
}

void VertexPaint3DEditorPlugin::_external_dialog_custom_action(const StringName &p_action) {
	if (p_action == StringName("paint_anyway")) {
		external_mesh_dialog->hide();
		_paint_external_anyway();
	}
}

void VertexPaint3DEditorPlugin::_paint_external_anyway() {
	ERR_FAIL_NULL(mesh_instance);
	Ref<Mesh> mesh = mesh_instance->get_mesh();
	ERR_FAIL_COND(mesh.is_null());

	external_accepted_mesh_id = mesh->get_instance_id();
	_set_paint_enabled(true);
}

bool VertexPaint3DEditorPlugin::_ensure_vertex_colors() {
	ERR_FAIL_COND_V(target_mesh.is_null(), false);

	bool needs_colors = false;
	for (int i = 0; i < target_mesh->get_surface_count(); i++) {
		if (!(target_mesh->surface_get_format(i) & Mesh::ARRAY_FORMAT_COLOR)) {
			needs_colors = true;
			break;
		}
	}
	if (!needs_colors) {
		return true;
	}

	// Give every color-less surface an opaque white color array. The rebuild
	// goes through a scratch mesh so that both halves of the undo/redo entry
	// are a plain `_surfaces` assignment, which puts formats, LODs, blend
	// shapes and materials back exactly as they were.
	Ref<ArrayMesh> rebuilt;
	rebuilt.instantiate();
	for (int i = 0; i < target_mesh->get_blend_shape_count(); i++) {
		rebuilt->add_blend_shape(target_mesh->get_blend_shape_name(i));
	}
	rebuilt->set_blend_shape_mode(target_mesh->get_blend_shape_mode());

	for (int i = 0; i < target_mesh->get_surface_count(); i++) {
		Array arrays = target_mesh->surface_get_arrays(i);
		ERR_FAIL_COND_V_MSG(arrays.size() != Mesh::ARRAY_MAX, false, vformat("Could not read surface %d of the mesh to paint.", i));

		const uint64_t format = target_mesh->surface_get_format(i);
		if (!(format & Mesh::ARRAY_FORMAT_COLOR)) {
			const PackedVector3Array vertices = arrays[Mesh::ARRAY_VERTEX];
			PackedColorArray colors;
			colors.resize(vertices.size());
			colors.fill(Color(1, 1, 1, 1));
			arrays[Mesh::ARRAY_COLOR] = colors;
		}

		// Keep the surface's storage layout (attribute compression, 8 bone
		// weights, ...); only the presence of the color array changes.
		const uint64_t flags = format & (Mesh::ARRAY_FLAG_USE_DYNAMIC_UPDATE | Mesh::ARRAY_FLAG_USE_8_BONE_WEIGHTS | Mesh::ARRAY_FLAG_USES_EMPTY_VERTEX_ARRAY | Mesh::ARRAY_FLAG_COMPRESS_ATTRIBUTES);
		rebuilt->add_surface_from_arrays(target_mesh->surface_get_primitive_type(i), arrays, target_mesh->surface_get_blend_shape_arrays(i), target_mesh->surface_get_lods(i), flags);
		rebuilt->surface_set_material(i, target_mesh->surface_get_material(i));
		rebuilt->surface_set_name(i, target_mesh->surface_get_name(i));
	}

	// A surface that would not rebuild would be dropped from the mesh, so bail
	// out rather than quietly handing back less geometry than we were given.
	if (rebuilt->get_surface_count() != target_mesh->get_surface_count()) {
		EditorNode::get_singleton()->show_warning(TTR("Could not add vertex colors to this mesh: one of its surfaces could not be rebuilt."));
		return false;
	}

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(TTR("Add Vertex Colors to Mesh"), UndoRedo::MERGE_DISABLE, mesh_instance);
	ur->add_do_property(target_mesh.ptr(), "_surfaces", rebuilt->get("_surfaces"));
	ur->add_undo_property(target_mesh.ptr(), "_surfaces", target_mesh->get("_surfaces"));
	ur->commit_action();
	target_mesh->set_edited(true);

	return true;
}

void VertexPaint3DEditorPlugin::_invalidate_cache() {
	surface_caches.clear();
	groups.clear();
	target_faces.clear();
	target_local_aabb = AABB();
	target_mesh.unref();
	cache_valid = false;
}

bool VertexPaint3DEditorPlugin::_is_cache_current() const {
	if (!cache_valid || target_mesh.is_null() || mesh_instance == nullptr) {
		return false;
	}
	if (mesh_instance->get_mesh().ptr() != target_mesh.ptr()) {
		return false;
	}
	// The mesh is edited in place, so its pointer alone does not say whether
	// the surfaces are still the ones the cache was built from: undoing the
	// step that added the color arrays, for one, leaves the pointer untouched.
	if ((int)surface_caches.size() != target_mesh->get_surface_count()) {
		return false;
	}
	for (uint32_t s = 0; s < surface_caches.size(); s++) {
		const SurfaceCache &sc = surface_caches[s];
		if (sc.vertex_count == 0) {
			continue;
		}
		if (!(target_mesh->surface_get_format(s) & Mesh::ARRAY_FORMAT_COLOR) || target_mesh->surface_get_array_len(s) != sc.vertex_count) {
			return false;
		}
	}
	return true;
}

bool VertexPaint3DEditorPlugin::_ensure_cache_current() {
	if (mesh_instance == nullptr) {
		return false;
	}
	if (_is_cache_current()) {
		return true;
	}
	_invalidate_cache();
	return _prepare_target();
}

void VertexPaint3DEditorPlugin::_build_cache() {
	surface_caches.clear();
	groups.clear();
	target_faces.clear();
	target_local_aabb = AABB();
	cache_valid = false;

	if (target_mesh.is_null()) {
		return;
	}

	const int surface_count = target_mesh->get_surface_count();
	surface_caches.resize(surface_count);

	// Weld groups are keyed on the exact vertex position: a vertex split along
	// a UV or normal seam is a verbatim copy of the one it was split from, so
	// the two positions compare bit-for-bit and need no tolerance.
	HashMap<Vector3, int> group_by_position;
	bool has_aabb = false;

	for (int s = 0; s < surface_count; s++) {
		SurfaceCache &sc = surface_caches[s];

		const uint64_t format = target_mesh->surface_get_format(s);
		if (!(format & Mesh::ARRAY_FORMAT_COLOR)) {
			continue;
		}

		const Array arrays = target_mesh->surface_get_arrays(s);
		if (arrays.size() != Mesh::ARRAY_MAX) {
			continue;
		}
		const PackedVector3Array vertices = arrays[Mesh::ARRAY_VERTEX];
		const PackedColorArray colors = arrays[Mesh::ARRAY_COLOR];
		if (vertices.is_empty() || colors.size() != vertices.size()) {
			continue;
		}

		uint32_t offsets[Mesh::ARRAY_MAX];
		uint32_t vertex_stride = 0;
		uint32_t normal_stride = 0;
		uint32_t attribute_stride = 0;
		uint32_t skin_stride = 0;
		RS::get_singleton()->mesh_surface_make_offsets_from_format(format, vertices.size(), target_mesh->surface_get_array_index_len(s), offsets, vertex_stride, normal_stride, attribute_stride, skin_stride);

		sc.vertex_count = vertices.size();
		sc.attribute_stride = attribute_stride;
		sc.color_offset = offsets[Mesh::ARRAY_COLOR];
		sc.attribute_data = RS::get_singleton()->mesh_get_surface(target_mesh->get_rid(), s).attribute_data;
		if (attribute_stride == 0 || (uint32_t)sc.attribute_data.size() < (uint32_t)sc.vertex_count * attribute_stride) {
			// The buffer does not look the way the format says it should;
			// leave this surface alone rather than corrupting it.
			sc.vertex_count = 0;
			sc.attribute_data.clear();
			continue;
		}

		sc.colors.resize(sc.vertex_count);
		for (int i = 0; i < sc.vertex_count; i++) {
			sc.colors[i] = colors[i];
		}

		LocalVector<int> vertex_group;
		vertex_group.resize(sc.vertex_count);
		for (int i = 0; i < sc.vertex_count; i++) {
			const Vector3 position = vertices[i];
			const Vector3 key = vertex_paint_weld_key(position);

			int group_index = -1;
			const int *existing = group_by_position.getptr(key);
			if (existing != nullptr) {
				group_index = *existing;
			} else {
				group_index = groups.size();
				PaintGroup group;
				group.local_position = position;
				groups.push_back(group);
				group_by_position[key] = group_index;
			}

			VertexRef ref;
			ref.surface = s;
			ref.index = i;
			groups[group_index].refs.push_back(ref);
			vertex_group[i] = group_index;

			if (!has_aabb) {
				target_local_aabb = AABB(position, Vector3());
				has_aabb = true;
			} else {
				target_local_aabb.expand_to(position);
			}
		}

		if (target_mesh->surface_get_primitive_type(s) != Mesh::PRIMITIVE_TRIANGLES) {
			continue;
		}

		const PackedInt32Array indices = arrays[Mesh::ARRAY_INDEX];
		const bool indexed = !indices.is_empty();
		const int corner_count = indexed ? indices.size() : sc.vertex_count;
		for (int t = 0; t + 2 < corner_count; t += 3) {
			int corner[3];
			bool valid = true;
			for (int k = 0; k < 3; k++) {
				corner[k] = indexed ? indices[t + k] : (t + k);
				if (corner[k] < 0 || corner[k] >= sc.vertex_count) {
					valid = false;
					break;
				}
			}
			if (!valid) {
				continue;
			}

			target_faces.push_back(Face3(vertices[corner[0]], vertices[corner[1]], vertices[corner[2]]));

			// Triangle edges double as the adjacency MODE_BLUR averages over.
			for (int k = 0; k < 3; k++) {
				const int a = vertex_group[corner[k]];
				const int b = vertex_group[corner[(k + 1) % 3]];
				if (a == b) {
					continue;
				}
				if (!groups[a].neighbors.has(b)) {
					groups[a].neighbors.push_back(b);
				}
				if (!groups[b].neighbors.has(a)) {
					groups[b].neighbors.push_back(a);
				}
			}
		}
	}

	cache_valid = !groups.is_empty();
}

void VertexPaint3DEditorPlugin::_update_preview() {
	if (!paint_enabled || view_mode == VIEW_MODE_SHADED || mesh_instance == nullptr) {
		_remove_preview();
		return;
	}

	if (preview_shader.is_null()) {
		preview_shader.instantiate();
		preview_shader->set_code(VERTEX_PAINT_PREVIEW_SHADER);
	}
	if (preview_material.is_null()) {
		preview_material.instantiate();
		preview_material->set_shader(preview_shader);
	}
	// VIEW_MODE_SHADED has already returned above, so the remaining modes map
	// onto the shader's 0-based `view_mode` in order.
	preview_material->set_shader_parameter("view_mode", (int)view_mode - 1);

	RS::get_singleton()->instance_geometry_set_material_override(mesh_instance->get_instance(), preview_material->get_rid());
	preview_installed = true;
}

void VertexPaint3DEditorPlugin::_remove_preview() {
	if (!preview_installed) {
		return;
	}
	preview_installed = false;

	// The node may already be gone (deleted while the tool was open), in which
	// case its instance took the override with it.
	MeshInstance3D *previewed = Object::cast_to<MeshInstance3D>(ObjectDB::get_instance(mesh_instance_id));
	if (previewed == nullptr) {
		return;
	}

	const Ref<Material> node_override = previewed->get_material_override();
	RS::get_singleton()->instance_geometry_set_material_override(previewed->get_instance(), node_override.is_valid() ? node_override->get_rid() : RID());
}

void VertexPaint3DEditorPlugin::_ensure_cursor_instance() {
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
	cursor_material->set_albedo(Color(1.0, 0.85, 0.2, 0.9));
	cursor_material->set_render_priority(Material::RENDER_PRIORITY_MAX);
	RS::get_singleton()->mesh_surface_set_material(cursor_mesh, 0, cursor_material->get_rid());
}

void VertexPaint3DEditorPlugin::_update_cursor(const Vector3 &p_position, const Vector3 &p_normal, bool p_visible) {
	if (mesh_instance == nullptr || !paint_enabled) {
		p_visible = false;
	}

	_ensure_cursor_instance();

	if (p_visible && !cursor_instance.is_valid()) {
		const RID scenario = mesh_instance->get_world_3d().is_valid() ? mesh_instance->get_world_3d()->get_scenario() : RID();
		cursor_instance = RS::get_singleton()->instance_create2(cursor_mesh, scenario);
	}

	if (!cursor_instance.is_valid()) {
		return;
	}

	if (p_visible) {
		const RID scenario = mesh_instance->get_world_3d().is_valid() ? mesh_instance->get_world_3d()->get_scenario() : RID();
		RS::get_singleton()->instance_set_scenario(cursor_instance, scenario);

		Vector3 up = p_normal;
		if (up.length_squared() < 0.0001f) {
			up = Vector3(0, 1, 0);
		} else {
			up.normalize();
		}
		Basis basis;
		basis.rotate_to_align(Vector3(0, 1, 0), up);
		basis = basis.scaled_local(Vector3(brush_radius, brush_radius, brush_radius));
		RS::get_singleton()->instance_set_transform(cursor_instance, Transform3D(basis, p_position));
	}

	RS::get_singleton()->instance_set_visible(cursor_instance, p_visible);
}

float VertexPaint3DEditorPlugin::_brush_weight(float p_distance) const {
	if (p_distance >= brush_radius) {
		return 0.0f;
	}
	// `brush_hardness` is the fraction of the radius that gets full strength;
	// past it the weight eases off to zero at the rim.
	const float t = p_distance / brush_radius;
	const float hardness = MIN(brush_hardness, 0.999f);
	const float x = MIN((1.0f - t) / (1.0f - hardness), 1.0f);
	return Math::smoothstep(0.0f, 1.0f, x);
}

Color VertexPaint3DEditorPlugin::_blend_channels(const Color &p_dst, const Color &p_src, float p_weight) const {
	const float weight = CLAMP(p_weight, 0.0f, 1.0f);
	Color result = p_dst;
	for (int i = 0; i < 4; i++) {
		if (channel_enabled[i]) {
			result[i] = Math::lerp(p_dst[i], p_src[i], weight);
		}
	}
	return result;
}

Color VertexPaint3DEditorPlugin::_get_group_color(int p_group) const {
	const PaintGroup &group = groups[p_group];
	if (group.refs.is_empty()) {
		return Color(1, 1, 1, 1);
	}
	const VertexRef &ref = group.refs[0];
	return surface_caches[ref.surface].colors[ref.index];
}

void VertexPaint3DEditorPlugin::_write_group_color(int p_group, const Color &p_color) {
	const PaintGroup &group = groups[p_group];
	for (const VertexRef &ref : group.refs) {
		SurfaceCache &sc = surface_caches[ref.surface];
		if (sc.vertex_count == 0 || sc.colors[ref.index] == p_color) {
			continue;
		}

		if (!sc.stroke_before.has(ref.index)) {
			sc.stroke_before[ref.index] = sc.colors[ref.index];
		}
		sc.colors[ref.index] = p_color;

		uint8_t *write = sc.attribute_data.ptrw() + (int64_t)ref.index * sc.attribute_stride + sc.color_offset;
		// Rounded rather than truncated so that repainting a color with the
		// value it already has cannot walk it down by one step each time.
		write[0] = (uint8_t)CLAMP(Math::round(p_color.r * 255.0f), 0.0f, 255.0f);
		write[1] = (uint8_t)CLAMP(Math::round(p_color.g * 255.0f), 0.0f, 255.0f);
		write[2] = (uint8_t)CLAMP(Math::round(p_color.b * 255.0f), 0.0f, 255.0f);
		write[3] = (uint8_t)CLAMP(Math::round(p_color.a * 255.0f), 0.0f, 255.0f);

		sc.dirty_first = (sc.dirty_first < 0) ? ref.index : MIN(sc.dirty_first, ref.index);
		sc.dirty_last = MAX(sc.dirty_last, ref.index);
	}
}

void VertexPaint3DEditorPlugin::_flush_dirty_surfaces() {
	if (target_mesh.is_null()) {
		return;
	}

	for (uint32_t s = 0; s < surface_caches.size(); s++) {
		SurfaceCache &sc = surface_caches[s];
		if (sc.dirty_first < 0) {
			continue;
		}

		// Colors share the attribute buffer with UVs and custom channels, so
		// the upload covers whole vertices; only one contiguous run of them is
		// sent rather than the entire buffer.
		const int from = sc.dirty_first * (int)sc.attribute_stride;
		const int to = (sc.dirty_last + 1) * (int)sc.attribute_stride;
		target_mesh->surface_update_attribute_region(s, from, sc.attribute_data.slice(from, to));

		sc.dirty_first = -1;
		sc.dirty_last = -1;
	}
}

bool VertexPaint3DEditorPlugin::_raycast(const Vector3 &p_from, const Vector3 &p_dir, float p_max_dist, Vector3 &r_position, Vector3 &r_normal) const {
	if (mesh_instance == nullptr || target_faces.is_empty()) {
		return false;
	}

	const Transform3D gt = mesh_instance->get_global_transform();
	if (Math::is_zero_approx(gt.basis.determinant())) {
		return false; // Flattened to nothing by its scale; there is no surface to hit.
	}

	const Transform3D gt_inv = gt.affine_inverse();
	const Vector3 local_from = gt_inv.xform(p_from);
	const Vector3 local_to = gt_inv.xform(p_from + p_dir * p_max_dist);
	if (!target_local_aabb.intersects_segment(local_from, local_to)) {
		return false;
	}

	bool found = false;
	float best_dist = p_max_dist;
	for (const Face3 &face : target_faces) {
		Vector3 point;
		if (!face.intersects_segment(local_from, local_to, &point)) {
			continue;
		}

		const Vector3 world_point = gt.xform(point);
		const float dist = p_from.distance_to(world_point);
		if (dist < best_dist) {
			best_dist = dist;
			found = true;
			r_position = world_point;
			Vector3 normal = gt.basis.xform(face.get_plane().normal).normalized();
			if (normal.dot(p_dir) > 0.0f) {
				normal = -normal;
			}
			r_normal = normal;
		}
	}

	return found;
}

void VertexPaint3DEditorPlugin::_stamp(const Vector3 &p_world_position) {
	if (!cache_valid) {
		return;
	}

	const float radius_sq = brush_radius * brush_radius;

	if (mode == MODE_BLUR) {
		// Average everything first and only then write: blurring in place
		// would feed a group's new color into its neighbors' averages within
		// the same stamp, smearing the stroke in whatever order the groups
		// happen to be stored in.
		LocalVector<int> touched;
		LocalVector<Color> blurred;

		for (uint32_t g = 0; g < groups.size(); g++) {
			const PaintGroup &group = groups[g];
			if (group.neighbors.is_empty()) {
				continue;
			}
			const float dist_sq = group.world_position.distance_squared_to(p_world_position);
			if (dist_sq > radius_sq) {
				continue;
			}
			const float weight = _brush_weight(Math::sqrt(dist_sq)) * brush_strength;
			if (weight <= 0.0f) {
				continue;
			}

			Color sum;
			for (const int &neighbor : group.neighbors) {
				sum += _get_group_color(neighbor);
			}
			const Color average = sum / (float)group.neighbors.size();

			touched.push_back(g);
			blurred.push_back(_blend_channels(_get_group_color(g), average, weight));
		}

		for (uint32_t i = 0; i < touched.size(); i++) {
			_write_group_color(touched[i], blurred[i]);
		}
	} else {
		const Color source = (mode == MODE_ERASE) ? erase_color : brush_color;
		for (uint32_t g = 0; g < groups.size(); g++) {
			const float dist_sq = groups[g].world_position.distance_squared_to(p_world_position);
			if (dist_sq > radius_sq) {
				continue;
			}
			const float weight = _brush_weight(Math::sqrt(dist_sq)) * brush_strength;
			if (weight <= 0.0f) {
				continue;
			}
			_write_group_color(g, _blend_channels(_get_group_color(g), source, weight));
		}
	}

	_flush_dirty_surfaces();
}

void VertexPaint3DEditorPlugin::_fill_all(const Color &p_color, bool p_respect_channels, const String &p_action_name) {
	if (!paint_enabled || !_ensure_cache_current()) {
		return;
	}

	// Work out every change up front, so an action is only opened when the
	// fill actually has something to do.
	struct SurfaceFill {
		int surface = 0;
		PackedInt32Array indices;
		PackedColorArray before;
		PackedColorArray after;
	};
	LocalVector<SurfaceFill> fills;

	for (uint32_t s = 0; s < surface_caches.size(); s++) {
		const SurfaceCache &sc = surface_caches[s];
		if (sc.vertex_count == 0) {
			continue;
		}

		SurfaceFill fill;
		fill.surface = s;
		for (int i = 0; i < sc.vertex_count; i++) {
			const Color old_color = sc.colors[i];
			const Color new_color = p_respect_channels ? _blend_channels(old_color, p_color, 1.0f) : p_color;
			if (new_color == old_color) {
				continue;
			}
			fill.indices.push_back(i);
			fill.before.push_back(old_color);
			fill.after.push_back(new_color);
		}

		if (!fill.indices.is_empty()) {
			fills.push_back(fill);
		}
	}

	if (fills.is_empty()) {
		return;
	}

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(p_action_name, UndoRedo::MERGE_DISABLE, mesh_instance);
	for (const SurfaceFill &fill : fills) {
		ur->add_do_method(this, "_apply_vertex_colors", target_mesh, fill.surface, fill.indices, fill.after);
		ur->add_undo_method(this, "_apply_vertex_colors", target_mesh, fill.surface, fill.indices, fill.before);
	}
	ur->commit_action();
}

void VertexPaint3DEditorPlugin::_begin_stroke() {
	if (mesh_instance == nullptr) {
		return;
	}

	// The mesh can change under the tool between strokes (swapped in the
	// inspector, or reverted by undoing the step that armed it).
	if (!_ensure_cache_current()) {
		_set_paint_enabled(false);
		return;
	}

	stroke_active = true;
	last_stamp_msec = 0;

	// Cache world positions once per stroke: the node cannot move while the
	// mouse is held down, so a stamp then costs one distance test per group.
	const Transform3D gt = mesh_instance->get_global_transform();
	for (PaintGroup &group : groups) {
		group.world_position = gt.xform(group.local_position);
	}
	for (SurfaceCache &sc : surface_caches) {
		sc.stroke_before.clear();
	}
}

void VertexPaint3DEditorPlugin::_end_stroke() {
	if (!stroke_active) {
		return;
	}
	stroke_active = false;
	_flush_dirty_surfaces();

	if (target_mesh.is_null()) {
		return;
	}

	struct SurfaceStroke {
		int surface = 0;
		PackedInt32Array indices;
		PackedColorArray before;
		PackedColorArray after;
	};
	LocalVector<SurfaceStroke> strokes;

	for (uint32_t s = 0; s < surface_caches.size(); s++) {
		SurfaceCache &sc = surface_caches[s];
		if (sc.stroke_before.is_empty()) {
			continue;
		}

		SurfaceStroke stroke;
		stroke.surface = s;
		for (const KeyValue<int, Color> &entry : sc.stroke_before) {
			stroke.indices.push_back(entry.key);
			stroke.before.push_back(entry.value);
			stroke.after.push_back(sc.colors[entry.key]);
		}
		sc.stroke_before.clear();

		if (!stroke.indices.is_empty()) {
			strokes.push_back(stroke);
		}
	}

	if (strokes.is_empty()) {
		return;
	}

	String action_name;
	switch (mode) {
		case MODE_BLUR:
			action_name = TTR("Blur Vertex Colors");
			break;
		case MODE_ERASE:
			action_name = TTR("Erase Vertex Colors");
			break;
		default:
			action_name = TTR("Paint Vertex Colors");
			break;
	}

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(action_name, UndoRedo::MERGE_DISABLE, mesh_instance);
	for (const SurfaceStroke &stroke : strokes) {
		ur->add_do_method(this, "_apply_vertex_colors", target_mesh, stroke.surface, stroke.indices, stroke.after);
		ur->add_undo_method(this, "_apply_vertex_colors", target_mesh, stroke.surface, stroke.indices, stroke.before);
	}
	// The stroke is already on the mesh; replaying it would only be busywork.
	ur->commit_action(false);
	target_mesh->set_edited(true);
}

void VertexPaint3DEditorPlugin::_cancel_stroke() {
	if (!stroke_active) {
		return;
	}
	stroke_active = false;

	for (uint32_t s = 0; s < surface_caches.size(); s++) {
		SurfaceCache &sc = surface_caches[s];
		for (const KeyValue<int, Color> &entry : sc.stroke_before) {
			const int index = entry.key;
			sc.colors[index] = entry.value;

			uint8_t *write = sc.attribute_data.ptrw() + (int64_t)index * sc.attribute_stride + sc.color_offset;
			write[0] = (uint8_t)CLAMP(Math::round(entry.value.r * 255.0f), 0.0f, 255.0f);
			write[1] = (uint8_t)CLAMP(Math::round(entry.value.g * 255.0f), 0.0f, 255.0f);
			write[2] = (uint8_t)CLAMP(Math::round(entry.value.b * 255.0f), 0.0f, 255.0f);
			write[3] = (uint8_t)CLAMP(Math::round(entry.value.a * 255.0f), 0.0f, 255.0f);

			sc.dirty_first = (sc.dirty_first < 0) ? index : MIN(sc.dirty_first, index);
			sc.dirty_last = MAX(sc.dirty_last, index);
		}
		sc.stroke_before.clear();
	}

	_flush_dirty_surfaces();
}

void VertexPaint3DEditorPlugin::_apply_vertex_colors(const Ref<ArrayMesh> &p_mesh, int p_surface, const PackedInt32Array &p_indices, const PackedColorArray &p_colors) {
	ERR_FAIL_COND(p_mesh.is_null());
	ERR_FAIL_INDEX(p_surface, p_mesh->get_surface_count());
	ERR_FAIL_COND(p_indices.size() != p_colors.size());
	if (p_indices.is_empty()) {
		return;
	}

	const uint64_t format = p_mesh->surface_get_format(p_surface);
	ERR_FAIL_COND(!(format & Mesh::ARRAY_FORMAT_COLOR));

	const int vertex_count = p_mesh->surface_get_array_len(p_surface);
	uint32_t offsets[Mesh::ARRAY_MAX];
	uint32_t vertex_stride = 0;
	uint32_t normal_stride = 0;
	uint32_t attribute_stride = 0;
	uint32_t skin_stride = 0;
	RS::get_singleton()->mesh_surface_make_offsets_from_format(format, vertex_count, p_mesh->surface_get_array_index_len(p_surface), offsets, vertex_stride, normal_stride, attribute_stride, skin_stride);
	ERR_FAIL_COND(attribute_stride == 0);

	// When the mesh being restored is the one currently loaded for painting,
	// its cached buffer is authoritative and has to stay in step.
	const bool is_live_target = target_mesh == p_mesh && p_surface < (int)surface_caches.size() && surface_caches[p_surface].vertex_count == vertex_count;

	Vector<uint8_t> attribute_data = is_live_target ? surface_caches[p_surface].attribute_data : RS::get_singleton()->mesh_get_surface(p_mesh->get_rid(), p_surface).attribute_data;
	ERR_FAIL_COND((uint32_t)attribute_data.size() < (uint32_t)vertex_count * attribute_stride);

	uint8_t *attribute_bytes = attribute_data.ptrw();
	const uint32_t color_offset = offsets[Mesh::ARRAY_COLOR];
	int first = -1;
	int last = -1;

	for (int i = 0; i < p_indices.size(); i++) {
		const int index = p_indices[i];
		if (index < 0 || index >= vertex_count) {
			continue;
		}
		const Color color = p_colors[i];

		uint8_t *write = attribute_bytes + (int64_t)index * attribute_stride + color_offset;
		write[0] = (uint8_t)CLAMP(Math::round(color.r * 255.0f), 0.0f, 255.0f);
		write[1] = (uint8_t)CLAMP(Math::round(color.g * 255.0f), 0.0f, 255.0f);
		write[2] = (uint8_t)CLAMP(Math::round(color.b * 255.0f), 0.0f, 255.0f);
		write[3] = (uint8_t)CLAMP(Math::round(color.a * 255.0f), 0.0f, 255.0f);

		if (is_live_target) {
			surface_caches[p_surface].colors[index] = color;
		}

		first = (first < 0) ? index : MIN(first, index);
		last = MAX(last, index);
	}

	if (first < 0) {
		return;
	}

	const int from = first * (int)attribute_stride;
	const int to = (last + 1) * (int)attribute_stride;
	p_mesh->surface_update_attribute_region(p_surface, from, attribute_data.slice(from, to));

	if (is_live_target) {
		surface_caches[p_surface].attribute_data = attribute_data;
	}
	p_mesh->set_edited(true);
}

void VertexPaint3DEditorPlugin::_fill_pressed() {
	_fill_all(brush_color, true, TTR("Fill Vertex Colors"));
}

void VertexPaint3DEditorPlugin::_clear_pressed() {
	// A reset puts every channel back to opaque white, whatever the channel
	// mask says, so it is a dependable way back to a clean mesh.
	_fill_all(erase_color, false, TTR("Reset Vertex Colors"));
}

bool VertexPaint3DEditorPlugin::_do_input_action(Camera3D *p_camera, const Point2 &p_screen_pos, bool p_click) {
	const Vector3 ray_from = p_camera->project_ray_origin(p_screen_pos);
	const Vector3 ray_dir = p_camera->project_ray_normal(p_screen_pos);

	Vector3 hit_pos;
	Vector3 hit_normal;
	const bool hit = _raycast(ray_from, ray_dir, p_camera->get_far(), hit_pos, hit_normal);

	_update_cursor(hit_pos, hit_normal, hit);

	if (!hit || !stroke_active) {
		return hit;
	}

	const uint64_t now = OS::get_singleton()->get_ticks_msec();
	const uint64_t stamp_interval_msec = 16;
	if (!p_click && (now - last_stamp_msec) < stamp_interval_msec) {
		return true;
	}
	last_stamp_msec = now;

	_stamp(hit_pos);
	return true;
}

EditorPlugin::AfterGUIInput VertexPaint3DEditorPlugin::forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) {
	if (!paint_enabled || mesh_instance == nullptr || !mesh_instance->is_inside_tree()) {
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

VertexPaint3DEditorPlugin::VertexPaint3DEditorPlugin() {
	topmenu_bar = memnew(HBoxContainer);
	topmenu_bar->hide();

	toolbar = memnew(HBoxContainer);
	topmenu_bar->add_child(toolbar);

	paint_enabled_button = memnew(Button);
	paint_enabled_button->set_toggle_mode(true);
	paint_enabled_button->set_text(TTR("Vertex Paint"));
	paint_enabled_button->set_tooltip_text(TTR("Paint vertex colors onto this mesh. While enabled, dragging in the viewport paints instead of selecting."));
	toolbar->add_child(paint_enabled_button);
	paint_enabled_button->connect(SceneStringName(toggled), callable_mp(this, &VertexPaint3DEditorPlugin::_paint_toggled));

	// Everything past the toggle only makes sense while painting, and a
	// MeshInstance3D is a common enough selection that leaving it all on
	// screen would crowd the viewport toolbar for no reason.
	brush_controls = memnew(HBoxContainer);
	brush_controls->hide();
	toolbar->add_child(brush_controls);

	brush_controls->add_child(memnew(VSeparator));

	mode_button_group.instantiate();

	mode_paint_button = memnew(Button);
	mode_paint_button->set_toggle_mode(true);
	mode_paint_button->set_button_group(mode_button_group);
	mode_paint_button->set_pressed(true);
	mode_paint_button->set_text(TTR("Paint"));
	mode_paint_button->set_tooltip_text(TTR("Blend the brush color into the vertex colors under the brush."));
	brush_controls->add_child(mode_paint_button);
	mode_paint_button->connect(SceneStringName(pressed), callable_mp(this, &VertexPaint3DEditorPlugin::_mode_pressed).bind((int)MODE_PAINT));

	mode_blur_button = memnew(Button);
	mode_blur_button->set_toggle_mode(true);
	mode_blur_button->set_button_group(mode_button_group);
	mode_blur_button->set_text(TTR("Blur"));
	mode_blur_button->set_tooltip_text(TTR("Average each vertex color with its neighbors', softening the edges of painted areas."));
	brush_controls->add_child(mode_blur_button);
	mode_blur_button->connect(SceneStringName(pressed), callable_mp(this, &VertexPaint3DEditorPlugin::_mode_pressed).bind((int)MODE_BLUR));

	mode_erase_button = memnew(Button);
	mode_erase_button->set_toggle_mode(true);
	mode_erase_button->set_button_group(mode_button_group);
	mode_erase_button->set_text(TTR("Erase"));
	mode_erase_button->set_tooltip_text(TTR("Blend the vertex colors under the brush back toward opaque white."));
	brush_controls->add_child(mode_erase_button);
	mode_erase_button->connect(SceneStringName(pressed), callable_mp(this, &VertexPaint3DEditorPlugin::_mode_pressed).bind((int)MODE_ERASE));

	brush_controls->add_child(memnew(VSeparator));

	color_button = memnew(ColorPickerButton);
	color_button->set_pick_color(brush_color);
	color_button->set_custom_minimum_size(Size2(48, 0) * EDSCALE);
	color_button->set_tooltip_text(TTR("Color the brush paints with."));
	brush_controls->add_child(color_button);
	color_button->connect("color_changed", callable_mp(this, &VertexPaint3DEditorPlugin::_set_brush_color));

	Label *radius_label = memnew(Label);
	radius_label->set_text(TTR("Radius:"));
	brush_controls->add_child(radius_label);

	brush_radius_spin = memnew(SpinBox);
	brush_radius_spin->set_min(0.001);
	brush_radius_spin->set_max(1000.0);
	brush_radius_spin->set_step(0.01);
	brush_radius_spin->set_value(brush_radius);
	brush_radius_spin->set_tooltip_text(TTR("Brush radius, in meters."));
	brush_controls->add_child(brush_radius_spin);
	brush_radius_spin->connect(SceneStringName(value_changed), callable_mp(this, &VertexPaint3DEditorPlugin::_set_brush_radius));

	Label *strength_label = memnew(Label);
	strength_label->set_text(TTR("Strength:"));
	brush_controls->add_child(strength_label);

	brush_strength_spin = memnew(SpinBox);
	brush_strength_spin->set_min(0.0);
	brush_strength_spin->set_max(1.0);
	brush_strength_spin->set_step(0.01);
	brush_strength_spin->set_value(brush_strength);
	brush_strength_spin->set_tooltip_text(TTR("How far each stamp moves a vertex color toward the brush color."));
	brush_controls->add_child(brush_strength_spin);
	brush_strength_spin->connect(SceneStringName(value_changed), callable_mp(this, &VertexPaint3DEditorPlugin::_set_brush_strength));

	Label *hardness_label = memnew(Label);
	hardness_label->set_text(TTR("Hardness:"));
	brush_controls->add_child(hardness_label);

	brush_hardness_spin = memnew(SpinBox);
	brush_hardness_spin->set_min(0.0);
	brush_hardness_spin->set_max(1.0);
	brush_hardness_spin->set_step(0.01);
	brush_hardness_spin->set_value(brush_hardness);
	brush_hardness_spin->set_tooltip_text(TTR("Fraction of the brush radius painted at full strength before the falloff starts. 1 gives a hard-edged brush."));
	brush_controls->add_child(brush_hardness_spin);
	brush_hardness_spin->connect(SceneStringName(value_changed), callable_mp(this, &VertexPaint3DEditorPlugin::_set_brush_hardness));

	brush_controls->add_child(memnew(VSeparator));

	// Per-channel locks, so a mesh can carry independent masks (wetness, wear,
	// blend weights) in R, G, B and A without one brush stroke disturbing the
	// channels it is not meant to touch.
	const String channel_names[4] = { "R", "G", "B", "A" };
	const String channel_tooltips[4] = {
		TTR("Let the brush write the red channel."),
		TTR("Let the brush write the green channel."),
		TTR("Let the brush write the blue channel."),
		TTR("Let the brush write the alpha channel."),
	};
	for (int i = 0; i < 4; i++) {
		channel_buttons[i] = memnew(Button);
		channel_buttons[i]->set_toggle_mode(true);
		channel_buttons[i]->set_pressed(channel_enabled[i]);
		channel_buttons[i]->set_text(channel_names[i]);
		channel_buttons[i]->set_tooltip_text(channel_tooltips[i]);
		brush_controls->add_child(channel_buttons[i]);
		channel_buttons[i]->connect(SceneStringName(pressed), callable_mp(this, &VertexPaint3DEditorPlugin::_channel_toggled).bind(i));
	}

	brush_controls->add_child(memnew(VSeparator));

	Label *view_label = memnew(Label);
	view_label->set_text(TTR("View:"));
	brush_controls->add_child(view_label);

	view_mode_button = memnew(OptionButton);
	view_mode_button->add_item(TTR("Shaded"), VIEW_MODE_SHADED);
	view_mode_button->add_item(TTR("Vertex Colors"), VIEW_MODE_RGB);
	view_mode_button->add_item(TTR("Red"), VIEW_MODE_R);
	view_mode_button->add_item(TTR("Green"), VIEW_MODE_G);
	view_mode_button->add_item(TTR("Blue"), VIEW_MODE_B);
	view_mode_button->add_item(TTR("Alpha"), VIEW_MODE_A);
	view_mode_button->select(view_mode);
	view_mode_button->set_tooltip_text(TTR("What the viewport shows while painting. Anything but Shaded temporarily displays the raw vertex colors, without changing the mesh's material."));
	brush_controls->add_child(view_mode_button);
	view_mode_button->connect(SceneStringName(item_selected), callable_mp(this, &VertexPaint3DEditorPlugin::_view_mode_selected));

	brush_controls->add_child(memnew(VSeparator));

	fill_button = memnew(Button);
	fill_button->set_text(TTR("Fill"));
	fill_button->set_tooltip_text(TTR("Set every vertex of the mesh to the brush color, in the enabled channels."));
	brush_controls->add_child(fill_button);
	fill_button->connect(SceneStringName(pressed), callable_mp(this, &VertexPaint3DEditorPlugin::_fill_pressed));

	clear_button = memnew(Button);
	clear_button->set_text(TTR("Reset"));
	clear_button->set_tooltip_text(TTR("Reset every vertex of the mesh to opaque white, in all channels."));
	brush_controls->add_child(clear_button);
	clear_button->connect(SceneStringName(pressed), callable_mp(this, &VertexPaint3DEditorPlugin::_clear_pressed));

	Node3DEditor::get_singleton()->add_control_to_menu_panel(topmenu_bar);

	convert_dialog = memnew(ConfirmationDialog);
	convert_dialog->set_title(TTR("Convert to ArrayMesh"));
	convert_dialog->set_text(TTR("Vertex colors can only be stored on an ArrayMesh, and this node uses a mesh that rebuilds itself from its own properties.\n\nConvert it to an ArrayMesh? The node will keep its current shape, but will no longer follow that mesh's properties."));
	convert_dialog->set_ok_button_text(TTR("Convert"));
	convert_dialog->connect(SceneStringName(confirmed), callable_mp(this, &VertexPaint3DEditorPlugin::_convert_to_array_mesh));
	EditorInterface::get_singleton()->get_base_control()->add_child(convert_dialog);

	external_mesh_dialog = memnew(ConfirmationDialog);
	external_mesh_dialog->set_title(TTR("Mesh Is Not Local to This Scene"));
	external_mesh_dialog->set_text(TTR("This mesh is stored outside the scene being edited, so painting it would also change every other place that uses it. If it came from an imported model, the next reimport would discard the painted colors.\n\nMake the mesh unique to this node first? Its materials stay shared."));
	external_mesh_dialog->set_ok_button_text(TTR("Make Unique"));
	external_mesh_dialog->connect(SceneStringName(confirmed), callable_mp(this, &VertexPaint3DEditorPlugin::_make_mesh_unique));
	external_mesh_dialog->add_button(TTR("Paint Anyway"), true, "paint_anyway");
	external_mesh_dialog->connect("custom_action", callable_mp(this, &VertexPaint3DEditorPlugin::_external_dialog_custom_action));
	EditorInterface::get_singleton()->get_base_control()->add_child(external_mesh_dialog);
}

VertexPaint3DEditorPlugin::~VertexPaint3DEditorPlugin() {
	if (cursor_instance.is_valid()) {
		RS::get_singleton()->free_rid(cursor_instance);
	}
	if (cursor_mesh.is_valid()) {
		RS::get_singleton()->free_rid(cursor_mesh);
	}
}
