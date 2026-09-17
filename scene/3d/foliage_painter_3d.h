/**************************************************************************/
/*  foliage_painter_3d.h                                                  */
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
#include "scene/3d/foliage_layer.h"
#include "scene/3d/node_3d.h"

class MultiMeshInstance3D;

// Holds one or more FoliageLayers (vegetation types) and renders each one
// through its own internal MultiMeshInstance3D. Instances are painted onto
// arbitrary MeshInstance3D surfaces in the editor with FoliagePainter3DEditorPlugin;
// this node itself only stores the resulting per-layer MultiMesh data and keeps
// it in sync with the layer configuration (mesh, material, visibility range).
class FoliagePainter3D : public Node3D {
	GDCLASS(FoliagePainter3D, Node3D);

	TypedArray<FoliageLayer> layers;
	LocalVector<MultiMeshInstance3D *> layer_nodes;

	void _update_layer_nodes();
	void _sync_layer_node(int p_index);
	void _on_layer_changed(int p_index);

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void set_layers(const TypedArray<FoliageLayer> &p_layers);
	TypedArray<FoliageLayer> get_layers() const;

	int get_layer_count() const;
	Ref<FoliageLayer> get_layer(int p_index) const;

	// Ordered insert/remove so that undo/redo (which replays these calls
	// through EditorUndoRedoManager) can restore the exact original layout.
	void insert_instance(int p_layer, int p_index, const Transform3D &p_transform);
	void remove_instance(int p_layer, int p_index);
	int add_instance(int p_layer, const Transform3D &p_transform);

	int get_layer_instance_count(int p_layer) const;
	Transform3D get_layer_instance_transform(int p_layer, int p_index) const;

	PackedStringArray get_configuration_warnings() const override;

	FoliagePainter3D();
};
