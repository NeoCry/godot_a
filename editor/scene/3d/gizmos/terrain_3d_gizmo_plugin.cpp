/**************************************************************************/
/*  terrain_3d_gizmo_plugin.cpp                                           */
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

#include "terrain_3d_gizmo_plugin.h"

#include "editor/settings/editor_settings.h"
#include "scene/3d/terrain_3d.h"
#include "scene/3d/terrain_data.h"

Terrain3DGizmoPlugin::Terrain3DGizmoPlugin() {
	const Color gizmo_color = EDITOR_GET("editors/3d_gizmos/gizmo_colors/terrain_3d");
	create_material("bounds_material", gizmo_color);
	// Distinct, unmissable color for the debug_draw_chunks wireframe, since it
	// needs to stand out from both the bounds outline above and the terrain
	// surface itself.
	create_material("chunks_material", Color(1.0, 0.6, 0.0, 0.5));
}

bool Terrain3DGizmoPlugin::has_gizmo(Node3D *p_spatial) {
	return Object::cast_to<Terrain3D>(p_spatial) != nullptr;
}

String Terrain3DGizmoPlugin::get_gizmo_name() const {
	return "Terrain3D";
}

int Terrain3DGizmoPlugin::get_priority() const {
	return -1;
}

void Terrain3DGizmoPlugin::redraw(EditorNode3DGizmo *p_gizmo) {
	Terrain3D *terrain = Object::cast_to<Terrain3D>(p_gizmo->get_node_3d());

	p_gizmo->clear();

	Ref<TerrainData> data = terrain->get_terrain_data();
	if (data.is_null()) {
		return;
	}

	const float size = data->get_size();
	const Ref<Material> material = get_material("bounds_material", p_gizmo);

	// A flat footprint outline (rather than a full box) since Terrain3D has
	// no fixed height range: heights can be sculpted arbitrarily high or low,
	// so the footprint is the only bound that stays meaningful to show.
	const Vector3 corners[4] = {
		Vector3(0, 0, 0),
		Vector3(size, 0, 0),
		Vector3(size, 0, size),
		Vector3(0, 0, size),
	};
	Vector<Vector3> lines;
	for (int i = 0; i < 4; i++) {
		lines.push_back(corners[i]);
		lines.push_back(corners[(i + 1) % 4]);
	}
	p_gizmo->add_lines(lines, material);
	p_gizmo->add_collision_segments(lines);

	if (terrain->is_debug_draw_chunks_enabled()) {
		const Ref<Material> chunks_material = get_material("chunks_material", p_gizmo);
		Vector<Vector3> chunk_lines;
		for (const AABB &aabb : terrain->get_chunk_local_aabbs()) {
			for (int i = 0; i < 12; i++) {
				Vector3 a, b;
				aabb.get_edge(i, a, b);
				chunk_lines.push_back(a);
				chunk_lines.push_back(b);
			}
		}
		if (!chunk_lines.is_empty()) {
			p_gizmo->add_lines(chunk_lines, chunks_material);
		}
	}
}
