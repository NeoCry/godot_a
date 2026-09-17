/**************************************************************************/
/*  foliage_painter_3d_editor_plugin.cpp                                  */
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

#include "foliage_painter_3d_editor_plugin.h"

#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/menu_button.h"
#include "scene/gui/separator.h"
#include "scene/gui/spin_box.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "scene/scene_string_names.h"
#include "servers/rendering/rendering_server.h"

void FoliagePainter3DEditorPlugin::_bind_methods() {
}

bool FoliagePainter3DEditorPlugin::handles(Object *p_object) const {
	return Object::cast_to<FoliagePainter3D>(p_object) != nullptr;
}

void FoliagePainter3DEditorPlugin::edit(Object *p_object) {
	_end_stroke();

	painter = Object::cast_to<FoliagePainter3D>(p_object);
	face_cache.clear();
	paint_targets.clear();

	if (cursor_instance.is_valid()) {
		RS::get_singleton()->instance_set_visible(cursor_instance, false);
	}

	_rebuild_layers_menu();
}

void FoliagePainter3DEditorPlugin::make_visible(bool p_visible) {
	if (p_visible) {
		topmenu_bar->show();
	} else {
		_end_stroke();
		topmenu_bar->hide();
		painter = nullptr;
		face_cache.clear();
		paint_targets.clear();
		if (cursor_instance.is_valid()) {
			RS::get_singleton()->instance_set_visible(cursor_instance, false);
		}
	}
}

void FoliagePainter3DEditorPlugin::_mode_pressed(int p_mode) {
	mode = (Mode)p_mode;
	_end_stroke();
}

void FoliagePainter3DEditorPlugin::_set_brush_radius(double p_value) {
	brush_radius = MAX(0.01f, (float)p_value);
}

void FoliagePainter3DEditorPlugin::_set_brush_density(double p_value) {
	brush_density = MAX(0.01f, (float)p_value);
}

void FoliagePainter3DEditorPlugin::_rebuild_layers_menu() {
	PopupMenu *popup = layers_menu->get_popup();
	popup->clear();

	if (painter == nullptr) {
		layer_active.clear();
		return;
	}

	// Preserve existing per-layer active flags (by index) so reopening the
	// menu, or a layer being added elsewhere, doesn't reset the user's picks.
	const Vector<bool> old_active = layer_active;
	const int count = painter->get_layer_count();
	layer_active.resize(count);
	for (int i = 0; i < count; i++) {
		layer_active.write[i] = (i < old_active.size()) ? old_active[i] : true;
		Ref<FoliageLayer> layer = painter->get_layer(i);
		const String name = (layer.is_valid() && !layer->get_layer_name().is_empty()) ? layer->get_layer_name() : vformat("Layer %d", i);
		popup->add_check_item(name, i);
		popup->set_item_checked(popup->get_item_count() - 1, layer_active[i]);
	}
}

void FoliagePainter3DEditorPlugin::_layer_menu_id_pressed(int p_id) {
	if (p_id < 0 || p_id >= layer_active.size()) {
		return;
	}
	layer_active.write[p_id] = !layer_active[p_id];

	PopupMenu *popup = layers_menu->get_popup();
	const int idx = popup->get_item_index(p_id);
	if (idx >= 0) {
		popup->set_item_checked(idx, layer_active[p_id]);
	}
}

Vector<int> FoliagePainter3DEditorPlugin::_get_active_layers() const {
	Vector<int> result;
	if (painter == nullptr) {
		return result;
	}
	for (int i = 0; i < layer_active.size() && i < painter->get_layer_count(); i++) {
		if (!layer_active[i]) {
			continue;
		}
		Ref<FoliageLayer> layer = painter->get_layer(i);
		if (layer.is_valid() && layer->get_mesh().is_valid()) {
			result.push_back(i);
		}
	}
	return result;
}

const FoliagePainter3DEditorPlugin::MeshFaceCache *FoliagePainter3DEditorPlugin::_get_face_cache(MeshInstance3D *p_mesh_instance) {
	const ObjectID id = p_mesh_instance->get_instance_id();
	const MeshFaceCache *cached = face_cache.getptr(id);
	if (cached != nullptr) {
		return cached;
	}

	Ref<Mesh> mesh = p_mesh_instance->get_mesh();
	if (mesh.is_null()) {
		return nullptr;
	}

	MeshFaceCache entry;
	Vector<Face3> faces = mesh->get_faces();
	entry.faces.resize(faces.size());
	for (int i = 0; i < faces.size(); i++) {
		entry.faces[i] = faces[i];
	}
	entry.local_aabb = mesh->get_aabb();

	face_cache[id] = entry;
	return face_cache.getptr(id);
}

void FoliagePainter3DEditorPlugin::_collect_mesh_instances(Node *p_node, Vector<MeshInstance3D *> &r_out) const {
	if (p_node == nullptr) {
		return;
	}

	if (p_node->get_internal_mode() == Node::INTERNAL_MODE_DISABLED) {
		MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(p_node);
		if (mi != nullptr && mi->is_visible_in_tree() && mi->get_mesh().is_valid()) {
			r_out.push_back(mi);
		}
	}

	for (int i = 0; i < p_node->get_child_count(); i++) {
		_collect_mesh_instances(p_node->get_child(i), r_out);
	}
}

void FoliagePainter3DEditorPlugin::_refresh_paint_targets() {
	paint_targets.clear();
	if (painter == nullptr || !painter->is_inside_tree()) {
		return;
	}
	Node *root = EditorNode::get_singleton()->get_edited_scene();
	_collect_mesh_instances(root, paint_targets);
}

bool FoliagePainter3DEditorPlugin::_raycast(const Vector3 &p_from, const Vector3 &p_dir, float p_max_dist, Vector3 &r_position, Vector3 &r_normal) {
	bool found = false;
	float best_dist = p_max_dist;
	const Vector3 to_offset = p_dir * p_max_dist;

	for (MeshInstance3D *mi : paint_targets) {
		if (!mi->is_inside_tree()) {
			continue;
		}

		const MeshFaceCache *cache = _get_face_cache(mi);
		if (cache == nullptr || cache->faces.is_empty()) {
			continue;
		}

		const Transform3D gt = mi->get_global_transform();
		const Transform3D gt_inv = gt.affine_inverse();
		const Vector3 local_from = gt_inv.xform(p_from);
		const Vector3 local_to = gt_inv.xform(p_from + to_offset);

		if (!cache->local_aabb.intersects_segment(local_from, local_to)) {
			continue;
		}

		for (const Face3 &face : cache->faces) {
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
				Vector3 n = gt.basis.xform(face.get_plane().normal).normalized();
				if (n.dot(p_dir) > 0.0f) {
					n = -n;
				}
				r_normal = n;
			}
		}
	}

	return found;
}

void FoliagePainter3DEditorPlugin::_ensure_cursor_instance() {
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

	Ref<StandardMaterial3D> mat;
	mat.instantiate();
	mat->set_shading_mode(StandardMaterial3D::SHADING_MODE_UNSHADED);
	mat->set_transparency(StandardMaterial3D::TRANSPARENCY_ALPHA);
	mat->set_flag(StandardMaterial3D::FLAG_DISABLE_DEPTH_TEST, true);
	mat->set_albedo(Color(1.0, 0.85, 0.2, 0.9));
	mat->set_render_priority(Material::RENDER_PRIORITY_MAX);
	RS::get_singleton()->mesh_surface_set_material(cursor_mesh, 0, mat->get_rid());
}

void FoliagePainter3DEditorPlugin::_update_cursor(const Vector3 &p_position, const Vector3 &p_normal, bool p_visible) {
	if (painter == nullptr) {
		p_visible = false;
	}

	_ensure_cursor_instance();

	if (p_visible && !cursor_instance.is_valid()) {
		const RID scenario = painter->get_world_3d().is_valid() ? painter->get_world_3d()->get_scenario() : RID();
		cursor_instance = RS::get_singleton()->instance_create2(cursor_mesh, scenario);
	}

	if (!cursor_instance.is_valid()) {
		return;
	}

	if (p_visible) {
		const RID scenario = painter->get_world_3d().is_valid() ? painter->get_world_3d()->get_scenario() : RID();
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

Transform3D FoliagePainter3DEditorPlugin::_make_instance_transform(const Ref<FoliageLayer> &p_layer, const Vector3 &p_position, const Vector3 &p_normal) {
	Vector3 n = p_normal;
	if (n.length_squared() < 0.0001f) {
		n = Vector3(0, 1, 0);
	} else {
		n.normalize();
	}

	Vector3 up_target = Vector3(0, 1, 0).lerp(n, p_layer->get_align_to_normal_amount());
	if (up_target.length_squared() < 0.0001f) {
		up_target = Vector3(0, 1, 0);
	} else {
		up_target.normalize();
	}

	const float tilt_degrees = p_layer->get_random_tilt_degrees();
	if (tilt_degrees > 0.0f) {
		Vector3 jitter_axis(rng.random(-1.0f, 1.0f), 0.0f, rng.random(-1.0f, 1.0f));
		if (jitter_axis.length_squared() > 0.0001f) {
			jitter_axis.normalize();
			const float jitter_angle = Math::deg_to_rad(rng.random(0.0f, tilt_degrees));
			up_target = up_target.rotated(jitter_axis, jitter_angle).normalized();
		}
	}

	const float yaw = p_layer->is_random_rotation_enabled() ? rng.random(0.0f, (float)Math::TAU) : 0.0f;
	Basis basis(Vector3(0, 1, 0), yaw);
	basis.rotate_to_align(Vector3(0, 1, 0), up_target);

	const float lo_scale = MIN(p_layer->get_min_scale(), p_layer->get_max_scale());
	const float hi_scale = MAX(p_layer->get_min_scale(), p_layer->get_max_scale());
	const float s = rng.random(lo_scale, hi_scale);
	basis = basis.scaled_local(Vector3(s, s, s));

	const Transform3D gt_inv = painter->get_global_transform().affine_inverse();
	return Transform3D(gt_inv.basis * basis, gt_inv.xform(p_position));
}

void FoliagePainter3DEditorPlugin::_do_insert(int p_layer, const Transform3D &p_transform) {
	const int index = painter->add_instance(p_layer, p_transform);

	StrokeOp op;
	op.layer = p_layer;
	op.index = index;
	op.transform = p_transform;
	op.is_insert = true;
	stroke_ops.push_back(op);
}

void FoliagePainter3DEditorPlugin::_do_remove(int p_layer, int p_index, const Transform3D &p_transform) {
	painter->remove_instance(p_layer, p_index);

	StrokeOp op;
	op.layer = p_layer;
	op.index = p_index;
	op.transform = p_transform;
	op.is_insert = false;
	stroke_ops.push_back(op);
}

void FoliagePainter3DEditorPlugin::_stamp_paint(const Vector3 &p_position, const Vector3 &p_normal) {
	Vector<int> active = _get_active_layers();
	if (active.is_empty()) {
		return;
	}

	Vector3 normal = p_normal;
	if (normal.length_squared() < 0.0001f) {
		normal = Vector3(0, 1, 0);
	} else {
		normal.normalize();
	}
	Vector3 tangent = (Math::abs(normal.y) < 0.99f ? Vector3(0, 1, 0).cross(normal) : Vector3(1, 0, 0).cross(normal));
	tangent.normalize();
	const Vector3 bitangent = normal.cross(tangent);

	const int samples = MAX(1, int(Math::round(brush_density)));
	const float probe_height = brush_radius + 1.0f;

	for (int s = 0; s < samples; s++) {
		const float angle = rng.randf() * (float)Math::TAU;
		const float r = brush_radius * Math::sqrt(rng.randf());
		const Vector3 offset = tangent * (Math::cos(angle) * r) + bitangent * (Math::sin(angle) * r);
		const Vector3 probe_from = p_position + offset + normal * probe_height;

		Vector3 hit_pos, hit_normal;
		if (!_raycast(probe_from, -normal, probe_height * 2.0f + 1.0f, hit_pos, hit_normal)) {
			continue;
		}

		const int layer_idx = active[(int)rng.rand((uint32_t)active.size())];
		Ref<FoliageLayer> layer = painter->get_layer(layer_idx);
		if (layer.is_null() || layer->get_mesh().is_null()) {
			continue;
		}

		const float spacing = layer->get_min_instance_spacing();
		if (spacing > 0.0f) {
			const Transform3D layer_gt = painter->get_global_transform();
			Ref<MultiMesh> mm = layer->get_multimesh();
			const int existing = mm->get_instance_count();
			const float spacing_sq = spacing * spacing;
			bool too_close = false;
			for (int i = 0; i < existing; i++) {
				const Vector3 p = layer_gt.xform(mm->get_instance_transform(i).origin);
				if (p.distance_squared_to(hit_pos) < spacing_sq) {
					too_close = true;
					break;
				}
			}
			if (too_close) {
				continue;
			}
		}

		_do_insert(layer_idx, _make_instance_transform(layer, hit_pos, hit_normal));
	}
}

void FoliagePainter3DEditorPlugin::_stamp_erase(const Vector3 &p_position) {
	Vector<int> active = _get_active_layers();
	if (active.is_empty()) {
		return;
	}

	const Transform3D layer_gt = painter->get_global_transform();
	const float radius_sq = brush_radius * brush_radius;

	for (int layer_idx : active) {
		Ref<FoliageLayer> layer = painter->get_layer(layer_idx);
		if (layer.is_null()) {
			continue;
		}
		Ref<MultiMesh> mm = layer->get_multimesh();
		// Walk backwards so ordered (shift-down) removal never invalidates the
		// index of an instance we haven't visited yet in this same pass.
		for (int i = mm->get_instance_count() - 1; i >= 0; i--) {
			const Transform3D t = mm->get_instance_transform(i);
			const Vector3 p = layer_gt.xform(t.origin);
			if (p.distance_squared_to(p_position) < radius_sq) {
				_do_remove(layer_idx, i, t);
			}
		}
	}
}

void FoliagePainter3DEditorPlugin::_place_single(const Vector3 &p_position, const Vector3 &p_normal) {
	Vector<int> active = _get_active_layers();
	if (active.is_empty()) {
		return;
	}

	const int layer_idx = active[0];
	Ref<FoliageLayer> layer = painter->get_layer(layer_idx);
	if (layer.is_null()) {
		return;
	}

	const Transform3D instance_xf = _make_instance_transform(layer, p_position, p_normal);
	const int index = painter->add_instance(layer_idx, instance_xf);

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(TTR("Place Foliage Instance"));
	ur->add_do_method(painter, "insert_instance", layer_idx, index, instance_xf);
	ur->add_undo_method(painter, "remove_instance", layer_idx, index);
	ur->commit_action(false);
}

void FoliagePainter3DEditorPlugin::_remove_single(const Vector3 &p_position) {
	Vector<int> active = _get_active_layers();
	if (active.is_empty()) {
		return;
	}

	const Transform3D gt = painter->get_global_transform();

	int best_layer = -1;
	int best_index = -1;
	float best_dist_sq = brush_radius * brush_radius;
	Transform3D best_transform;

	for (int layer_idx : active) {
		Ref<FoliageLayer> layer = painter->get_layer(layer_idx);
		if (layer.is_null()) {
			continue;
		}
		Ref<MultiMesh> mm = layer->get_multimesh();
		for (int i = 0; i < mm->get_instance_count(); i++) {
			const Transform3D t = mm->get_instance_transform(i);
			const float d = gt.xform(t.origin).distance_squared_to(p_position);
			if (d < best_dist_sq) {
				best_dist_sq = d;
				best_layer = layer_idx;
				best_index = i;
				best_transform = t;
			}
		}
	}

	if (best_layer == -1) {
		return;
	}

	painter->remove_instance(best_layer, best_index);

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(TTR("Remove Foliage Instance"));
	ur->add_do_method(painter, "remove_instance", best_layer, best_index);
	ur->add_undo_method(painter, "insert_instance", best_layer, best_index, best_transform);
	ur->commit_action(false);
}

void FoliagePainter3DEditorPlugin::_begin_stroke() {
	stroke_active = true;
	stroke_ops.clear();
	face_cache.clear();
	last_stamp_msec = 0;
	_refresh_paint_targets();
}

void FoliagePainter3DEditorPlugin::_end_stroke() {
	if (stroke_active && !stroke_ops.is_empty() && painter != nullptr) {
		EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
		ur->create_action(mode == MODE_ERASE ? TTR("Erase Foliage") : TTR("Paint Foliage"));
		for (const StrokeOp &op : stroke_ops) {
			if (op.is_insert) {
				ur->add_do_method(painter, "insert_instance", op.layer, op.index, op.transform);
			} else {
				ur->add_do_method(painter, "remove_instance", op.layer, op.index);
			}
		}
		for (int i = stroke_ops.size() - 1; i >= 0; i--) {
			const StrokeOp &op = stroke_ops[i];
			if (op.is_insert) {
				ur->add_undo_method(painter, "remove_instance", op.layer, op.index);
			} else {
				ur->add_undo_method(painter, "insert_instance", op.layer, op.index, op.transform);
			}
		}
		ur->commit_action(false);
	}
	stroke_active = false;
	stroke_ops.clear();
}

void FoliagePainter3DEditorPlugin::_cancel_stroke() {
	if (stroke_active && painter != nullptr) {
		for (int i = stroke_ops.size() - 1; i >= 0; i--) {
			const StrokeOp &op = stroke_ops[i];
			if (op.is_insert) {
				painter->remove_instance(op.layer, op.index);
			} else {
				painter->insert_instance(op.layer, op.index, op.transform);
			}
		}
	}
	stroke_active = false;
	stroke_ops.clear();
}

bool FoliagePainter3DEditorPlugin::_do_input_action(Camera3D *p_camera, const Point2 &p_screen_pos, bool p_click) {
	if (!stroke_active && paint_targets.is_empty()) {
		_refresh_paint_targets();
	}

	const Vector3 ray_from = p_camera->project_ray_origin(p_screen_pos);
	const Vector3 ray_dir = p_camera->project_ray_normal(p_screen_pos);

	Vector3 hit_pos, hit_normal;
	const bool hit = _raycast(ray_from, ray_dir, p_camera->get_far(), hit_pos, hit_normal);

	_update_cursor(hit_pos, hit_normal, hit);

	if (!hit) {
		return false;
	}

	if (mode == MODE_PLACE_SINGLE) {
		if (p_click) {
			_place_single(hit_pos, hit_normal);
		}
		return true;
	}
	if (mode == MODE_REMOVE_SINGLE) {
		if (p_click) {
			_remove_single(hit_pos);
		}
		return true;
	}

	if (!stroke_active) {
		return true;
	}

	const uint64_t now = OS::get_singleton()->get_ticks_msec();
	const uint64_t stamp_interval_msec = 60;
	if (!p_click && (now - last_stamp_msec) < stamp_interval_msec) {
		return true;
	}
	last_stamp_msec = now;

	if (mode == MODE_PAINT) {
		_stamp_paint(hit_pos, hit_normal);
	} else if (mode == MODE_ERASE) {
		_stamp_erase(hit_pos);
	}

	return true;
}

EditorPlugin::AfterGUIInput FoliagePainter3DEditorPlugin::forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) {
	if (painter == nullptr || !painter->is_inside_tree()) {
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
			if (mode == MODE_PAINT || mode == MODE_ERASE) {
				_begin_stroke();
			} else {
				_refresh_paint_targets();
			}
			_do_input_action(p_camera, mb->get_position(), true);
			return EditorPlugin::AFTER_GUI_INPUT_STOP;
		} else if (stroke_active) {
			_end_stroke();
			return EditorPlugin::AFTER_GUI_INPUT_STOP;
		}
	}

	Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid()) {
		if (_do_input_action(p_camera, mm->get_position(), false)) {
			return EditorPlugin::AFTER_GUI_INPUT_STOP;
		}
		return EditorPlugin::AFTER_GUI_INPUT_PASS;
	}

	return EditorPlugin::AFTER_GUI_INPUT_PASS;
}

FoliagePainter3DEditorPlugin::FoliagePainter3DEditorPlugin() {
	topmenu_bar = memnew(HBoxContainer);
	topmenu_bar->hide();

	toolbar = memnew(HBoxContainer);
	topmenu_bar->add_child(toolbar);

	mode_button_group.instantiate();

	mode_paint_button = memnew(Button);
	mode_paint_button->set_toggle_mode(true);
	mode_paint_button->set_button_group(mode_button_group);
	mode_paint_button->set_pressed(true);
	mode_paint_button->set_text(TTR("Paint"));
	mode_paint_button->set_tooltip_text(TTR("Paint foliage instances by clicking or dragging over a mesh surface."));
	toolbar->add_child(mode_paint_button);
	mode_paint_button->connect(SceneStringName(pressed), callable_mp(this, &FoliagePainter3DEditorPlugin::_mode_pressed).bind((int)MODE_PAINT));

	mode_erase_button = memnew(Button);
	mode_erase_button->set_toggle_mode(true);
	mode_erase_button->set_button_group(mode_button_group);
	mode_erase_button->set_text(TTR("Erase"));
	mode_erase_button->set_tooltip_text(TTR("Erase foliage instances of the active layers within the brush."));
	toolbar->add_child(mode_erase_button);
	mode_erase_button->connect(SceneStringName(pressed), callable_mp(this, &FoliagePainter3DEditorPlugin::_mode_pressed).bind((int)MODE_ERASE));

	mode_place_single_button = memnew(Button);
	mode_place_single_button->set_toggle_mode(true);
	mode_place_single_button->set_button_group(mode_button_group);
	mode_place_single_button->set_text(TTR("Place Single"));
	mode_place_single_button->set_tooltip_text(TTR("Insert a single foliage instance per click."));
	toolbar->add_child(mode_place_single_button);
	mode_place_single_button->connect(SceneStringName(pressed), callable_mp(this, &FoliagePainter3DEditorPlugin::_mode_pressed).bind((int)MODE_PLACE_SINGLE));

	mode_remove_single_button = memnew(Button);
	mode_remove_single_button->set_toggle_mode(true);
	mode_remove_single_button->set_button_group(mode_button_group);
	mode_remove_single_button->set_text(TTR("Remove Single"));
	mode_remove_single_button->set_tooltip_text(TTR("Delete the single closest foliage instance per click."));
	toolbar->add_child(mode_remove_single_button);
	mode_remove_single_button->connect(SceneStringName(pressed), callable_mp(this, &FoliagePainter3DEditorPlugin::_mode_pressed).bind((int)MODE_REMOVE_SINGLE));

	toolbar->add_child(memnew(VSeparator));

	Label *radius_label = memnew(Label);
	radius_label->set_text(TTR("Radius:"));
	toolbar->add_child(radius_label);

	brush_radius_spin = memnew(SpinBox);
	brush_radius_spin->set_min(0.05);
	brush_radius_spin->set_max(1000.0);
	brush_radius_spin->set_step(0.05);
	brush_radius_spin->set_value(brush_radius);
	brush_radius_spin->set_tooltip_text(TTR("Brush radius, in meters."));
	toolbar->add_child(brush_radius_spin);
	brush_radius_spin->connect(SceneStringName(value_changed), callable_mp(this, &FoliagePainter3DEditorPlugin::_set_brush_radius));

	Label *density_label = memnew(Label);
	density_label->set_text(TTR("Density:"));
	toolbar->add_child(density_label);

	brush_density_spin = memnew(SpinBox);
	brush_density_spin->set_min(0.1);
	brush_density_spin->set_max(200.0);
	brush_density_spin->set_step(0.1);
	brush_density_spin->set_value(brush_density);
	brush_density_spin->set_tooltip_text(TTR("Number of instances placed per paint stamp."));
	toolbar->add_child(brush_density_spin);
	brush_density_spin->connect(SceneStringName(value_changed), callable_mp(this, &FoliagePainter3DEditorPlugin::_set_brush_density));

	toolbar->add_child(memnew(VSeparator));

	layers_menu = memnew(MenuButton);
	layers_menu->set_text(TTR("Layers"));
	layers_menu->set_tooltip_text(TTR("Choose which foliage layers this brush affects."));
	layers_menu->get_popup()->set_hide_on_checkable_item_selection(false);
	layers_menu->get_popup()->connect(SceneStringName(id_pressed), callable_mp(this, &FoliagePainter3DEditorPlugin::_layer_menu_id_pressed));
	layers_menu->connect("about_to_popup", callable_mp(this, &FoliagePainter3DEditorPlugin::_rebuild_layers_menu));
	toolbar->add_child(layers_menu);

	Node3DEditor::get_singleton()->add_control_to_menu_panel(topmenu_bar);
}

FoliagePainter3DEditorPlugin::~FoliagePainter3DEditorPlugin() {
	if (cursor_instance.is_valid()) {
		RS::get_singleton()->free_rid(cursor_instance);
	}
	if (cursor_mesh.is_valid()) {
		RS::get_singleton()->free_rid(cursor_mesh);
	}
}
