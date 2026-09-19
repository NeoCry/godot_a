/**************************************************************************/
/*  foliage_painter_3d_gizmo_plugin.h                                     */
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

#include "editor/scene/3d/node_3d_editor_gizmos.h"

// Debug view for FoliagePainter3D: while FoliagePainter3D.debug_show_cells is
// enabled, draws a wireframe box around every non-empty cell's actual
// bounding box (the same AABB the renderer uses to frustum-cull that cell's
// internal MultiMeshInstance3D), so the effect of FoliagePainter3D.cell_size
// can be seen directly. Modeled after FoliageSpawner3DGizmoPlugin; unlike it,
// there is no draggable size to expose as a handle here (cell_size is a
// single scalar edited in the Inspector), so no handle-related overrides are
// needed.
class FoliagePainter3DGizmoPlugin : public EditorNode3DGizmoPlugin {
	GDCLASS(FoliagePainter3DGizmoPlugin, EditorNode3DGizmoPlugin);

public:
	bool has_gizmo(Node3D *p_spatial) override;
	String get_gizmo_name() const override;
	int get_priority() const override;
	void redraw(EditorNode3DGizmo *p_gizmo) override;

	FoliagePainter3DGizmoPlugin();
};
