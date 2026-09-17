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

#include "core/templates/local_vector.h"
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

	probe_size = EDITOR_GET("editors/3d_gizmos/gizmo_settings/ambient_probe_volume_probe_size");

	// Baked probes are previewed as small solid spheres tinted by their baked AO
	// (white = fully lit, black = fully occluded), similar to how Unity visualizes
	// light probes, so it's obvious at a glance whether baking actually did anything.
	Ref<StandardMaterial3D> probe_mat = memnew(StandardMaterial3D);
	probe_mat->set_shading_mode(StandardMaterial3D::SHADING_MODE_UNSHADED);
	probe_mat->set_flag(StandardMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
	probe_mat->set_flag(StandardMaterial3D::FLAG_SRGB_VERTEX_COLOR, false);
	probe_mat->set_flag(StandardMaterial3D::FLAG_DISABLE_FOG, true);
	add_material("probe_material", probe_mat);
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

	// Only draw the per-probe preview (crosses or spheres) while this node is selected:
	// with dozens to thousands of probes, having them permanently visible would clutter
	// the viewport for every other object once a volume is baked. The box outline above
	// stays visible at all times so the volume itself can still be found/resized.
	if (!p_gizmo->is_selected()) {
		return;
	}

	const Vector3i counts = volume->get_probe_counts();
	// Skip the per-probe preview for very dense grids: drawing tens of thousands of
	// shapes every redraw would noticeably slow down the editor viewport for no benefit,
	// the box outline above is enough feedback for those cases.
	const int64_t total_probes = int64_t(counts.x) * int64_t(counts.y) * int64_t(counts.z);
	if (total_probes > 4096) {
		return;
	}

	if (volume->is_baked()) {
		// Baked: draw each probe as a small sphere tinted by its baked AO value, so it's
		// obvious whether baking actually produced any variation (as opposed to every
		// probe silently staying at the default "fully lit" value).
		const int stack_count = 6;
		const int sector_count = 8;
		const float sector_step = (Math::PI * 2.0) / sector_count;
		const float stack_step = Math::PI / stack_count;
		const float radius = probe_size * 0.5f;

		if (!Math::is_zero_approx(radius)) {
			LocalVector<Vector3> vertices;
			LocalVector<Color> colors;
			LocalVector<int> indices;

			for (int zi = 0; zi < counts.z; zi++) {
				for (int yi = 0; yi < counts.y; yi++) {
					for (int xi = 0; xi < counts.x; xi++) {
						const Vector3 center = volume->get_local_probe_position(xi, yi, zi);
						const float ao = volume->get_probe_ao(xi, yi, zi);
						const Color color(ao, ao, ao, 1.0);
						const int vertex_base = vertices.size();

						for (int i = 0; i <= stack_count; i++) {
							const float stack_angle = Math::PI / 2 - i * stack_step;
							const float xy = radius * Math::cos(stack_angle);
							const float z = radius * Math::sin(stack_angle);

							for (int j = 0; j <= sector_count; j++) {
								const float sector_angle = j * sector_step;
								const float x = xy * Math::cos(sector_angle);
								const float y = xy * Math::sin(sector_angle);
								vertices.push_back(center + Vector3(x, z, y));
								colors.push_back(color);
							}
						}

						for (int i = 0; i < stack_count; i++) {
							int k1 = i * (sector_count + 1);
							int k2 = k1 + sector_count + 1;
							for (int j = 0; j < sector_count; j++, k1++, k2++) {
								if (i != 0) {
									indices.push_back(vertex_base + k1);
									indices.push_back(vertex_base + k2);
									indices.push_back(vertex_base + k1 + 1);
								}
								if (i != (stack_count - 1)) {
									indices.push_back(vertex_base + k1 + 1);
									indices.push_back(vertex_base + k2);
									indices.push_back(vertex_base + k2 + 1);
								}
							}
						}
					}
				}
			}

			Array array;
			array.resize(Mesh::ARRAY_MAX);
			array[Mesh::ARRAY_VERTEX] = Vector<Vector3>(vertices);
			array[Mesh::ARRAY_INDEX] = Vector<int>(indices);
			array[Mesh::ARRAY_COLOR] = Vector<Color>(colors);

			Ref<ArrayMesh> mesh;
			mesh.instantiate();
			mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, array, Array(), Dictionary(), 0); // No compression.
			mesh->surface_set_material(0, get_material("probe_material", p_gizmo));

			p_gizmo->add_mesh(mesh);
		}
	} else {
		// Not baked yet: small crosses previewing where "Bake AO" will sample each probe.
		// Kept out of the collision segments above so they don't interfere with picking the volume.
		Vector<Vector3> probe_lines;
		const float cross_size = MIN(aabb.size.x, MIN(aabb.size.y, aabb.size.z)) * 0.02 + 0.02;
		for (int xi = 0; xi < counts.x; xi++) {
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
}
