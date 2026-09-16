/**************************************************************************/
/*  ambient_probe_volume_3d_gizmo_plugin.cpp                              */
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

#include "ambient_probe_volume_3d_gizmo_plugin.h"

#include "editor/scene/3d/gizmos/gizmo_3d_helper.h"
#include "editor/settings/editor_settings.h"
#include "scene/3d/ambient_probe_volume_3d.h"

AmbientProbeVolume3DGizmoPlugin::AmbientProbeVolume3DGizmoPlugin() {
	helper.instantiate();
	Color gizmo_color = EDITOR_GET("editors/3d_gizmos/gizmo_colors/ambient_probe_volume");
	create_material("shape_material", gizmo_color);
	gizmo_color.a = 0.15;
	create_material("shape_material_internal", gizmo_color);

	create_handle_material("handles");
}

bool AmbientProbeVolume3DGizmoPlugin::has_gizmo(Node3D *p_spatial) {
	return (Object::cast_to<AmbientProbeVolume3D>(p_spatial) != nullptr);
}

String AmbientProbeVolume3DGizmoPlugin::get_gizmo_name() const {
	return "AmbientProbeVolume3D";
}

int AmbientProbeVolume3DGizmoPlugin::get_priority() const {
	return -1;
}

String AmbientProbeVolume3DGizmoPlugin::get_handle_name(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const {
	return helper->box_get_handle_name(p_id);
}

Variant AmbientProbeVolume3DGizmoPlugin::get_handle_value(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const {
	return Vector3(p_gizmo->get_node_3d()->call("get_size"));
}

void AmbientProbeVolume3DGizmoPlugin::begin_handle_action(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) {
	helper->initialize_handle_action(get_handle_value(p_gizmo, p_id, p_secondary), p_gizmo->get_node_3d()->get_global_transform());
}

void AmbientProbeVolume3DGizmoPlugin::set_handle(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary, Camera3D *p_camera, const Point2 &p_point) {
	AmbientProbeVolume3D *volume = Object::cast_to<AmbientProbeVolume3D>(p_gizmo->get_node_3d());
	Vector3 size = volume->get_size();

	Vector3 sg[2];
	helper->get_segment(p_camera, p_point, sg);

	Vector3 position;
	helper->box_set_handle(sg, p_id, size, position);
	volume->set_size(size);
	volume->set_global_position(position);
}

void AmbientProbeVolume3DGizmoPlugin::commit_handle(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary, const Variant &p_restore, bool p_cancel) {
	helper->box_commit_handle(TTR("Change Ambient Probe Volume Size"), p_cancel, p_gizmo->get_node_3d(), nullptr, "global_position", "size");
}

void AmbientProbeVolume3DGizmoPlugin::redraw(EditorNode3DGizmo *p_gizmo) {
	AmbientProbeVolume3D *volume = Object::cast_to<AmbientProbeVolume3D>(p_gizmo->get_node_3d());

	p_gizmo->clear();

	const Ref<Material> material = get_material("shape_material", p_gizmo);
	Ref<Material> handles_material = get_material("handles");

	Vector<Vector3> lines;
	AABB aabb;
	aabb.size = volume->get_size();
	aabb.position = aabb.size / -2;

	for (int i = 0; i < 12; i++) {
		Vector3 a, b;
		aabb.get_edge(i, a, b);
		lines.push_back(a);
		lines.push_back(b);
	}

	Vector<Vector3> handles = helper->box_get_handles(volume->get_size());

	p_gizmo->add_lines(lines, material);
	p_gizmo->add_collision_segments(lines);
	p_gizmo->add_handles(handles, handles_material);

	// Small crosses previewing where "Bake AO" will sample each probe.
	// Kept out of the collision segments above so they don't interfere with picking the volume.
	Vector<Vector3> probe_lines;
	const Vector3i counts = volume->get_probe_counts();
	// Skip the per-probe preview for very dense grids: drawing tens of thousands of
	// crosses every redraw would noticeably slow down the editor viewport for no benefit,
	// the box outline above is enough feedback for those cases.
	const int64_t total_probes = int64_t(counts.x) * int64_t(counts.y) * int64_t(counts.z);
	const float cross_size = MIN(aabb.size.x, MIN(aabb.size.y, aabb.size.z)) * 0.02 + 0.02;
	for (int xi = 0; total_probes <= 4096 && xi < counts.x; xi++) {
		for (int yi = 0; yi < counts.y; yi++) {
			for (int zi = 0; zi < counts.z; zi++) {
				const Vector3 p = volume->get_local_probe_position(xi, yi, zi);
				probe_lines.push_back(p - Vector3(cross_size, 0, 0));
				probe_lines.push_back(p + Vector3(cross_size, 0, 0));
				probe_lines.push_back(p - Vector3(0, cross_size, 0));
				probe_lines.push_back(p + Vector3(0, cross_size, 0));
				probe_lines.push_back(p - Vector3(0, 0, cross_size));
				probe_lines.push_back(p + Vector3(0, 0, cross_size));
			}
		}
	}
	p_gizmo->add_lines(probe_lines, material);
}
