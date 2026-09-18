/**************************************************************************/
/*  foliage_painter_3d_gizmo_plugin.cpp                                   */
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

#include "foliage_painter_3d_gizmo_plugin.h"

#include "editor/settings/editor_settings.h"
#include "scene/3d/foliage_painter_3d.h"

FoliagePainter3DGizmoPlugin::FoliagePainter3DGizmoPlugin() {
	const Color gizmo_color = EDITOR_GET("editors/3d_gizmos/gizmo_colors/foliage_painter");
	create_material("cell_material", gizmo_color);
}

bool FoliagePainter3DGizmoPlugin::has_gizmo(Node3D *p_spatial) {
	return (Object::cast_to<FoliagePainter3D>(p_spatial) != nullptr);
}

String FoliagePainter3DGizmoPlugin::get_gizmo_name() const {
	return "FoliagePainter3D";
}

int FoliagePainter3DGizmoPlugin::get_priority() const {
	return -1;
}

void FoliagePainter3DGizmoPlugin::redraw(EditorNode3DGizmo *p_gizmo) {
	FoliagePainter3D *painter = Object::cast_to<FoliagePainter3D>(p_gizmo->get_node_3d());

	p_gizmo->clear();

	const Ref<Material> material = get_material("cell_material", p_gizmo);

	Vector<Vector3> lines;
	for (int layer = 0; layer < painter->get_layer_count(); layer++) {
		for (const Vector2i &cell : painter->get_layer_cell_coords(layer)) {
			const AABB aabb = painter->get_cell_aabb(layer, cell);
			for (int i = 0; i < 12; i++) {
				Vector3 a, b;
				aabb.get_edge(i, a, b);
				lines.push_back(a);
				lines.push_back(b);
			}
		}
	}

	if (lines.is_empty()) {
		return;
	}

	p_gizmo->add_lines(lines, material);
	p_gizmo->add_collision_segments(lines);
}
