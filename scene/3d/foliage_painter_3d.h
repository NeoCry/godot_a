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

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/3d/foliage_layer.h"
#include "scene/3d/node_3d.h"

class MultiMesh;
class MultiMeshInstance3D;

// Holds one or more FoliageLayers (vegetation types). Painted instances of
// each layer are spatially chunked into a grid of cells on the XZ plane
// (see cell_size); each non-empty (layer, cell) pair gets one MultiMesh and
// internal MultiMeshInstance3D per FoliageLODLevel of that layer, all
// sharing the same instance transforms (only the mesh/material/visibility
// range differ between LOD levels). Each of those small, tightly-bounded
// nodes is culled by the renderer's normal per-node AABB frustum culling,
// instead of always submitting every instance of a layer in one giant draw
// call. Instances are painted onto arbitrary MeshInstance3D surfaces in the
// editor with FoliagePainter3DEditorPlugin.
class FoliagePainter3D : public Node3D {
	GDCLASS(FoliagePainter3D, Node3D);

	// One MultiMesh + MultiMeshInstance3D per FoliageLODLevel of the cell's
	// layer, index-for-index; every multimesh in lod_multimeshes always has
	// the exact same instance transforms as the others (see _sync_cell_lods).
	struct FoliageCell {
		LocalVector<Ref<MultiMesh>> lod_multimeshes;
		LocalVector<MultiMeshInstance3D *> lod_nodes;
	};

	TypedArray<FoliageLayer> layers;
	float cell_size = 16.0f;

	// One cell map per layer (indexed the same as `layers`).
	LocalVector<HashMap<Vector2i, FoliageCell>> layer_cells;

	void _ensure_layer_cells_size();
	FoliageCell &_get_or_create_cell(int p_layer, const Vector2i &p_cell);
	// Reconciles a cell's lod_multimeshes/lod_nodes count and configuration
	// (mesh, material, visibility range, shared shadow/LOD settings) to
	// match its layer's *current* lod_levels. Safe to call at any time:
	// creates/frees MultiMeshInstance3D children and copies existing
	// transforms into any newly added LOD level as needed.
	void _sync_cell_lods(int p_layer, FoliageCell &p_cell);
	void _sync_layer_cells(int p_layer);
	void _prune_layers_to_size();
	void _on_layer_changed(int p_index);
	void _refresh_layer_instance_count(int p_layer);

	static LocalVector<Transform3D> _read_transforms(const Ref<MultiMesh> &p_multimesh);
	static void _write_transforms(const Ref<MultiMesh> &p_multimesh, const LocalVector<Transform3D> &p_transforms);

	// Internal, storage-only representation of layer_cells' MultiMesh
	// resources, so painted instances are actually saved with the scene
	// (the MultiMeshInstance3D nodes themselves are not; they're recreated
	// from this data instead).
	Array _get_cell_data() const;
	void _set_cell_data(const Array &p_data);

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void set_layers(const TypedArray<FoliageLayer> &p_layers);
	TypedArray<FoliageLayer> get_layers() const;

	void set_cell_size(float p_size);
	float get_cell_size() const;

	int get_layer_count() const;
	Ref<FoliageLayer> get_layer(int p_index) const;

	// Ordered insert/remove so that undo/redo (which replays these calls
	// through EditorUndoRedoManager) can restore the exact original layout.
	// Cells that become empty after a removal are freed automatically.
	void insert_instance(int p_layer, const Vector2i &p_cell, int p_index, const Transform3D &p_transform);
	void remove_instance(int p_layer, const Vector2i &p_cell, int p_index);
	int add_instance(int p_layer, const Vector2i &p_cell, const Transform3D &p_transform);

	// Plain C++ helpers for the editor plugin (not bound to ClassDB: they're
	// only ever called directly from FoliagePainter3DEditorPlugin's C++ code).
	Vector2i get_cell_for_local_position(const Vector3 &p_local_position) const;
	Vector<Vector2i> get_layer_cell_coords(int p_layer) const;
	int get_cell_instance_count(int p_layer, const Vector2i &p_cell) const;
	Transform3D get_cell_instance_transform(int p_layer, const Vector2i &p_cell, int p_index) const;
	// Local-space AABB actually used by the renderer to cull that cell's
	// MultiMeshInstance3D (used by FoliagePainter3DGizmoPlugin's debug view).
	AABB get_cell_aabb(int p_layer, const Vector2i &p_cell) const;

	PackedStringArray get_configuration_warnings() const override;

	FoliagePainter3D();
};
