/**************************************************************************/
/*  planar_reflection_probe_gizmo_plugin.cpp                              */
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

#include "planar_reflection_probe_gizmo_plugin.h"

#include "core/math/geometry_3d.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/scene/3d/gizmos/gizmo_3d_helper.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/settings/editor_settings.h"
#include "scene/3d/planar_reflection_probe.h"

// Handles: the mirror's half width along X and Z, and the receive distance along Y.
static const Vector3 HANDLE_AXES[3] = { Vector3(1, 0, 0), Vector3(0, 0, 1), Vector3(0, 1, 0) };

PlanarReflectionProbeGizmoPlugin::PlanarReflectionProbeGizmoPlugin() {
	helper.instantiate();
	Color gizmo_color = EDITOR_GET("editors/3d_gizmos/gizmo_colors/reflection_probe");

	create_material("planar_reflection_probe_material", gizmo_color);

	gizmo_color.a = 0.35;
	create_material("planar_reflection_internal_material", gizmo_color);

	create_icon_material("planar_reflection_probe_icon", EditorNode::get_singleton()->get_editor_theme()->get_icon(SNAME("GizmoPlanarReflectionProbe"), EditorStringName(EditorIcons)));
	create_handle_material("handles");
}

bool PlanarReflectionProbeGizmoPlugin::has_gizmo(Node3D *p_spatial) {
	return Object::cast_to<PlanarReflectionProbe>(p_spatial) != nullptr;
}

String PlanarReflectionProbeGizmoPlugin::get_gizmo_name() const {
	return "PlanarReflectionProbe";
}

int PlanarReflectionProbeGizmoPlugin::get_priority() const {
	return -1;
}

String PlanarReflectionProbeGizmoPlugin::get_handle_name(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const {
	switch (p_id) {
		case 0:
			return "Size X";
		case 1:
			return "Size Z";
		case 2:
			return "Receive Distance";
	}
	return "";
}

Variant PlanarReflectionProbeGizmoPlugin::get_handle_value(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const {
	PlanarReflectionProbe *probe = Object::cast_to<PlanarReflectionProbe>(p_gizmo->get_node_3d());
	if (p_id == 2) {
		return probe->get_receive_distance();
	}
	return probe->get_size();
}

void PlanarReflectionProbeGizmoPlugin::begin_handle_action(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) {
	helper->initialize_handle_action(get_handle_value(p_gizmo, p_id, p_secondary), p_gizmo->get_node_3d()->get_global_transform());
}

void PlanarReflectionProbeGizmoPlugin::set_handle(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary, Camera3D *p_camera, const Point2 &p_point) {
	ERR_FAIL_INDEX(p_id, 3);
	PlanarReflectionProbe *probe = Object::cast_to<PlanarReflectionProbe>(p_gizmo->get_node_3d());

	Vector3 segment[2];
	helper->get_segment(p_camera, p_point, segment);

	// How far along the handle's axis the pointer is, in the probe's space.
	const Vector3 axis = HANDLE_AXES[p_id];
	Vector3 on_axis, on_ray;
	Geometry3D::get_closest_points_between_segments(-axis * 16384, axis * 16384, segment[0], segment[1], on_axis, on_ray);
	real_t d = MAX(on_axis.dot(axis), 0.001);
	if (Node3DEditor::get_singleton()->is_snap_enabled()) {
		d = Math::snapped(d, Node3DEditor::get_singleton()->get_translate_snap());
	}

	if (p_id == 2) {
		probe->set_receive_distance(MAX(d, 0.001));
	} else {
		Vector2 size = probe->get_size();
		size[p_id] = MAX(d * 2.0, 0.001);
		probe->set_size(size);
	}
}

void PlanarReflectionProbeGizmoPlugin::commit_handle(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary, const Variant &p_restore, bool p_cancel) {
	PlanarReflectionProbe *probe = Object::cast_to<PlanarReflectionProbe>(p_gizmo->get_node_3d());
	const StringName property = p_id == 2 ? StringName("receive_distance") : StringName("size");

	if (p_cancel) {
		probe->set(property, p_restore);
		return;
	}

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(p_id == 2 ? TTR("Change Planar Reflection Receive Distance") : TTR("Change Planar Reflection Size"));
	ur->add_do_property(probe, property, probe->get(property));
	ur->add_undo_property(probe, property, p_restore);
	ur->commit_action();
}

void PlanarReflectionProbeGizmoPlugin::redraw(EditorNode3DGizmo *p_gizmo) {
	p_gizmo->clear();

	PlanarReflectionProbe *probe = Object::cast_to<PlanarReflectionProbe>(p_gizmo->get_node_3d());
	const Vector2 half = probe->get_size() * 0.5;
	const real_t reach = probe->get_receive_distance();

	// The mirror's outline, and which way it faces.
	Vector<Vector3> lines;
	const Vector3 corners[4] = { Vector3(-half.x, 0, -half.y), Vector3(half.x, 0, -half.y), Vector3(half.x, 0, half.y), Vector3(-half.x, 0, half.y) };
	for (int i = 0; i < 4; i++) {
		lines.push_back(corners[i]);
		lines.push_back(corners[(i + 1) % 4]);
	}
	const real_t arrow = MIN(MIN(half.x, half.y) * 0.5, 1.0);
	lines.push_back(Vector3());
	lines.push_back(Vector3(0, arrow, 0));
	lines.push_back(Vector3(0, arrow, 0));
	lines.push_back(Vector3(arrow * 0.2, arrow * 0.75, 0));
	lines.push_back(Vector3(0, arrow, 0));
	lines.push_back(Vector3(-arrow * 0.2, arrow * 0.75, 0));
	p_gizmo->add_lines(lines, get_material("planar_reflection_probe_material", p_gizmo));

	if (p_gizmo->is_selected()) {
		// The volume of surfaces that reflect it.
		Vector<Vector3> internal_lines;
		AABB volume(Vector3(-half.x, -reach, -half.y), Vector3(half.x * 2.0, reach * 2.0, half.y * 2.0));
		for (int i = 0; i < 12; i++) {
			Vector3 a, b;
			volume.get_edge(i, a, b);
			internal_lines.push_back(a);
			internal_lines.push_back(b);
		}
		p_gizmo->add_lines(internal_lines, get_material("planar_reflection_internal_material", p_gizmo));

		Vector<Vector3> handles;
		handles.push_back(Vector3(half.x, 0, 0));
		handles.push_back(Vector3(0, 0, half.y));
		handles.push_back(Vector3(0, reach, 0));
		p_gizmo->add_handles(handles, get_material("handles"));
	}

	p_gizmo->add_unscaled_billboard(get_material("planar_reflection_probe_icon", p_gizmo), 0.05);
}
