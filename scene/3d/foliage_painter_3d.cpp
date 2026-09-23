/**************************************************************************/
/*  foliage_painter_3d.cpp                                                */
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

#include "foliage_painter_3d.h"

#include "core/core_string_names.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/multimesh.h"

void FoliagePainter3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_layers", "layers"), &FoliagePainter3D::set_layers);
	ClassDB::bind_method(D_METHOD("get_layers"), &FoliagePainter3D::get_layers);

	ClassDB::bind_method(D_METHOD("set_cell_size", "size"), &FoliagePainter3D::set_cell_size);
	ClassDB::bind_method(D_METHOD("get_cell_size"), &FoliagePainter3D::get_cell_size);

	ClassDB::bind_method(D_METHOD("set_debug_show_cells", "enabled"), &FoliagePainter3D::set_debug_show_cells);
	ClassDB::bind_method(D_METHOD("is_debug_show_cells_enabled"), &FoliagePainter3D::is_debug_show_cells_enabled);

	ClassDB::bind_method(D_METHOD("set_gpu_culling", "enabled"), &FoliagePainter3D::set_gpu_culling);
	ClassDB::bind_method(D_METHOD("is_gpu_culling_enabled"), &FoliagePainter3D::is_gpu_culling_enabled);

	ClassDB::bind_method(D_METHOD("get_layer_count"), &FoliagePainter3D::get_layer_count);
	ClassDB::bind_method(D_METHOD("get_layer", "layer_index"), &FoliagePainter3D::get_layer);

	ClassDB::bind_method(D_METHOD("insert_instance", "layer_index", "cell", "index", "transform"), &FoliagePainter3D::insert_instance);
	ClassDB::bind_method(D_METHOD("remove_instance", "layer_index", "cell", "index"), &FoliagePainter3D::remove_instance);
	ClassDB::bind_method(D_METHOD("add_instance", "layer_index", "cell", "transform"), &FoliagePainter3D::add_instance);

	ClassDB::bind_method(D_METHOD("_get_cell_data"), &FoliagePainter3D::_get_cell_data);
	ClassDB::bind_method(D_METHOD("_set_cell_data", "data"), &FoliagePainter3D::_set_cell_data);

	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "layers", PROPERTY_HINT_ARRAY_TYPE, MAKE_RESOURCE_TYPE_HINT("FoliageLayer")), "set_layers", "get_layers");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "gpu_culling"), "set_gpu_culling", "is_gpu_culling_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cell_size", PROPERTY_HINT_RANGE, "1,256,0.5,or_greater,suffix:m"), "set_cell_size", "get_cell_size");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "_cell_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_INTERNAL | PROPERTY_USAGE_STORAGE), "_set_cell_data", "_get_cell_data");

	ADD_GROUP("Debug", "debug_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug_show_cells"), "set_debug_show_cells", "is_debug_show_cells_enabled");
}

void FoliagePainter3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			// Cell MultiMeshInstance3D nodes are runtime-only (see _get_cell_data);
			// make sure every cell loaded from _cell_data has its full set.
			for (uint32_t li = 0; li < layer_cells.size(); li++) {
				for (KeyValue<Vector2i, FoliageCell> &kv : layer_cells[li]) {
					_sync_cell_lods((int)li, kv.value);
				}
			}
			// Same for the GPU path's nodes, which are not saved either.
			for (int i = 0; i < layers.size(); i++) {
				_mark_gpu_layer_dirty(i);
			}
		} break;

		case NOTIFICATION_INTERNAL_PROCESS: {
			_dispatch_gpu_culling();
		} break;
	}
}

void FoliagePainter3D::_ensure_layer_cells_size() {
	if ((int)layer_cells.size() < layers.size()) {
		layer_cells.resize(layers.size());
	}
}

FoliagePainter3D::FoliageCell &FoliagePainter3D::_get_or_create_cell(int p_layer, const Vector2i &p_cell) {
	_ensure_layer_cells_size();
	HashMap<Vector2i, FoliageCell> &cells = layer_cells[p_layer];
	const bool is_new = !cells.has(p_cell);
	FoliageCell &cell = cells[p_cell];
	if (is_new) {
		_sync_cell_lods(p_layer, cell);
	}
	return cell;
}

LocalVector<Transform3D> FoliagePainter3D::_read_transforms(const Ref<MultiMesh> &p_multimesh) {
	LocalVector<Transform3D> result;
	if (p_multimesh.is_null()) {
		return result;
	}
	const int count = p_multimesh->get_instance_count();
	result.resize(count);
	for (int i = 0; i < count; i++) {
		result[i] = p_multimesh->get_instance_transform(i);
	}
	return result;
}

void FoliagePainter3D::_write_transforms(const Ref<MultiMesh> &p_multimesh, const LocalVector<Transform3D> &p_transforms) {
	if (p_multimesh.is_null()) {
		return;
	}
	// MultiMesh.instance_count "clears and (re)sizes the buffers" on every
	// set, so this always writes the *entire* array back, never just a diff.
	p_multimesh->set_instance_count((int)p_transforms.size());
	for (uint32_t i = 0; i < p_transforms.size(); i++) {
		p_multimesh->set_instance_transform(i, p_transforms[i]);
	}
}

void FoliagePainter3D::_sync_cell_lods(int p_layer, FoliageCell &p_cell) {
	if (p_layer < 0 || p_layer >= layers.size()) {
		return;
	}
	Ref<FoliageLayer> layer = layers[p_layer];
	if (layer.is_null()) {
		return;
	}

	const TypedArray<FoliageLODLevel> lod_levels = layer->get_lod_levels();
	// Under GPU culling a cell is storage only: one MultiMesh holds the
	// instances (the same one every other LOD level would have copied anyway)
	// and nothing here is rendered, so the per-level copies are not needed.
	const int lod_count = _is_gpu_culling_active() ? MIN(1, lod_levels.size()) : lod_levels.size();

	// Shrink extra LOD multimeshes/nodes if the layer now has fewer LOD
	// levels than this cell was last synced with.
	while ((int)p_cell.lod_multimeshes.size() > lod_count) {
		const int last = p_cell.lod_multimeshes.size() - 1;
		if (last < (int)p_cell.lod_nodes.size()) {
			if (p_cell.lod_nodes[last] != nullptr) {
				remove_child(p_cell.lod_nodes[last]);
				p_cell.lod_nodes[last]->queue_free();
			}
			p_cell.lod_nodes.remove_at(last);
		}
		p_cell.lod_multimeshes.remove_at(last);
	}

	// Grow: new LOD levels start out with the same instance transforms as
	// the rest of the cell (read from any existing multimesh) instead of
	// being empty, so a newly added LOD level doesn't need repainting.
	const LocalVector<Transform3D> existing_transforms = p_cell.lod_multimeshes.is_empty() ? LocalVector<Transform3D>() : _read_transforms(p_cell.lod_multimeshes[0]);
	while ((int)p_cell.lod_multimeshes.size() < lod_count) {
		Ref<MultiMesh> mm;
		mm.instantiate();
		mm->set_transform_format(MultiMesh::TRANSFORM_3D);
		_write_transforms(mm, existing_transforms);
		p_cell.lod_multimeshes.push_back(mm);
	}

	if (_is_gpu_culling_active()) {
		// Nothing is drawn straight from a cell here; the layer's indirect
		// MultiMeshes are, so any node left over from the CPU path has to go.
		while (!p_cell.lod_nodes.is_empty()) {
			MultiMeshInstance3D *node = p_cell.lod_nodes[p_cell.lod_nodes.size() - 1];
			if (node != nullptr) {
				remove_child(node);
				node->queue_free();
			}
			p_cell.lod_nodes.remove_at(p_cell.lod_nodes.size() - 1);
		}

		for (int i = 0; i < lod_count; i++) {
			Ref<FoliageLODLevel> level = lod_levels[i];
			Ref<MultiMesh> mm = p_cell.lod_multimeshes[i];
			if (level.is_valid() && mm.is_valid()) {
				// Still assigned so the MultiMesh can work out its own AABB.
				mm->set_mesh(level->get_mesh());
			}
		}
		return;
	}

	// Ensure every multimesh has a MultiMeshInstance3D to be rendered through.
	while ((int)p_cell.lod_nodes.size() < (int)p_cell.lod_multimeshes.size()) {
		MultiMeshInstance3D *node = memnew(MultiMeshInstance3D);
		add_child(node, false, INTERNAL_MODE_FRONT);
		p_cell.lod_nodes.push_back(node);
	}

	// Apply each LOD level's own mesh/material/visibility range, plus the
	// properties shared by the whole layer, to every node.
	for (int i = 0; i < lod_count; i++) {
		Ref<FoliageLODLevel> level = lod_levels[i];
		MultiMeshInstance3D *node = p_cell.lod_nodes[i];
		Ref<MultiMesh> mm = p_cell.lod_multimeshes[i];
		if (level.is_null() || node == nullptr || mm.is_null()) {
			continue;
		}

		mm->set_mesh(level->get_mesh());

		node->set_multimesh(mm);
		node->set_material_override(level->get_material_override());
		node->set_cast_shadows_setting(layer->is_casting_shadows() ? GeometryInstance3D::SHADOW_CASTING_SETTING_ON : GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		node->set_ignore_screen_space_shadows(layer->is_ignoring_screen_space_shadows());
		node->set_lod_bias(layer->get_lod_bias());
		// Per-layer, like FoliageSpawner3D's cell_gi_mode: defaults to disabled
		// (see FoliageLayer::gi_mode), but can be switched to Static/Dynamic per
		// vegetation type. Shared by every LOD level, same as the properties above.
		node->set_gi_mode(layer->get_gi_mode());
		node->set_visibility_range_begin(level->get_visibility_range_begin());
		node->set_visibility_range_begin_margin(level->get_visibility_range_begin_margin());
		node->set_visibility_range_end(level->get_visibility_range_end());
		node->set_visibility_range_end_margin(level->get_visibility_range_end_margin());
		node->set_visibility_range_fade_mode(level->get_visibility_range_fade_mode());
	}
}

void FoliagePainter3D::_sync_layer_cells(int p_layer) {
	if (p_layer < 0 || p_layer >= (int)layer_cells.size()) {
		return;
	}
	for (KeyValue<Vector2i, FoliageCell> &kv : layer_cells[p_layer]) {
		_sync_cell_lods(p_layer, kv.value);
	}
}

void FoliagePainter3D::_prune_layers_to_size() {
	while ((int)layer_cells.size() > layers.size()) {
		HashMap<Vector2i, FoliageCell> &cells = layer_cells[layer_cells.size() - 1];
		for (KeyValue<Vector2i, FoliageCell> &kv : cells) {
			for (MultiMeshInstance3D *node : kv.value.lod_nodes) {
				if (node != nullptr) {
					remove_child(node);
					node->queue_free();
				}
			}
		}
		layer_cells.remove_at(layer_cells.size() - 1);
	}

	while ((int)gpu_layers.size() > layers.size()) {
		GPULayer *gpu_layer = gpu_layers[gpu_layers.size() - 1];
		if (gpu_layer != nullptr) {
			if (gpu_layer->culler != nullptr) {
				memdelete(gpu_layer->culler);
			}
			for (MultiMeshInstance3D *node : gpu_layer->lod_nodes) {
				if (node != nullptr) {
					remove_child(node);
					node->queue_free();
				}
			}
			memdelete(gpu_layer);
		}
		gpu_layers.remove_at(gpu_layers.size() - 1);
	}
}

void FoliagePainter3D::_on_layer_changed(int p_index) {
	if (p_index >= 0 && p_index < layers.size()) {
		_sync_layer_cells(p_index);
		// A level's mesh or range moves the GPU path's band boundaries and
		// bounding radii, both of which are baked into the culler's setup.
		_mark_gpu_layer_dirty(p_index);
		// A layer's own properties (e.g. its mesh) changing can affect the
		// warnings shown for this node (e.g. "Layer N has no Mesh assigned"),
		// which would otherwise stay stale until something else refreshed them.
		update_configuration_warnings();
		update_gizmos();
	}
}

void FoliagePainter3D::_refresh_layer_instance_count(int p_layer) {
	if (p_layer < 0 || p_layer >= layers.size()) {
		return;
	}
	Ref<FoliageLayer> layer = layers[p_layer];
	if (layer.is_null()) {
		return;
	}

	int total = 0;
	if (p_layer < (int)layer_cells.size()) {
		for (const KeyValue<Vector2i, FoliageCell> &kv : layer_cells[p_layer]) {
			if (!kv.value.lod_multimeshes.is_empty() && kv.value.lod_multimeshes[0].is_valid()) {
				total += kv.value.lod_multimeshes[0]->get_instance_count();
			}
		}
	}
	layer->_set_display_instance_count(total);
}

void FoliagePainter3D::set_layers(const TypedArray<FoliageLayer> &p_layers) {
	// Every non-null entry in `layers` is always connected to _on_layer_changed
	// bound with its own index, so it can be resynced whenever the layer's
	// own properties (mesh, visibility range, etc.) change in the Inspector.
	for (int i = 0; i < layers.size(); i++) {
		Ref<FoliageLayer> old_layer = layers[i];
		if (old_layer.is_valid()) {
			old_layer->disconnect(CoreStringName(changed), callable_mp(this, &FoliagePainter3D::_on_layer_changed).bind(i));
		}
	}

	layers = p_layers;

	for (int i = 0; i < layers.size(); i++) {
		Ref<FoliageLayer> layer = layers[i];
		if (layer.is_valid()) {
			layer->connect(CoreStringName(changed), callable_mp(this, &FoliagePainter3D::_on_layer_changed).bind(i));
		}
	}

	_ensure_layer_cells_size();
	_prune_layers_to_size();
	for (int i = 0; i < layers.size(); i++) {
		_sync_layer_cells(i);
		_mark_gpu_layer_dirty(i);
		_refresh_layer_instance_count(i);
	}
	update_configuration_warnings();
	update_gizmos();
}

TypedArray<FoliageLayer> FoliagePainter3D::get_layers() const {
	return layers;
}

void FoliagePainter3D::set_cell_size(float p_size) {
	cell_size = MAX(p_size, 0.01f);
}

float FoliagePainter3D::get_cell_size() const {
	return cell_size;
}

void FoliagePainter3D::set_debug_show_cells(bool p_enabled) {
	debug_show_cells = p_enabled;
	update_gizmos();
}

bool FoliagePainter3D::is_debug_show_cells_enabled() const {
	return debug_show_cells;
}

int FoliagePainter3D::get_layer_count() const {
	return layers.size();
}

Ref<FoliageLayer> FoliagePainter3D::get_layer(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, layers.size(), Ref<FoliageLayer>());
	return layers[p_index];
}

void FoliagePainter3D::insert_instance(int p_layer, const Vector2i &p_cell, int p_index, const Transform3D &p_transform) {
	ERR_FAIL_INDEX(p_layer, layers.size());

	FoliageCell &cell = _get_or_create_cell(p_layer, p_cell);
	if (cell.lod_multimeshes.is_empty()) {
		return; // The layer has no LOD levels at all; nothing to store or render.
	}

	LocalVector<Transform3D> transforms = _read_transforms(cell.lod_multimeshes[0]);
	p_index = CLAMP(p_index, 0, (int)transforms.size());
	transforms.insert(p_index, p_transform);

	for (const Ref<MultiMesh> &mm : cell.lod_multimeshes) {
		_write_transforms(mm, transforms);
	}

	_mark_gpu_layer_dirty(p_layer);
	_refresh_layer_instance_count(p_layer);
	update_gizmos();
}

void FoliagePainter3D::remove_instance(int p_layer, const Vector2i &p_cell, int p_index) {
	ERR_FAIL_INDEX(p_layer, layers.size());
	ERR_FAIL_INDEX(p_layer, (int)layer_cells.size());

	FoliageCell *cell = layer_cells[p_layer].getptr(p_cell);
	ERR_FAIL_NULL(cell);
	ERR_FAIL_COND(cell->lod_multimeshes.is_empty());

	LocalVector<Transform3D> transforms = _read_transforms(cell->lod_multimeshes[0]);
	ERR_FAIL_INDEX(p_index, (int)transforms.size());
	transforms.remove_at(p_index);

	for (const Ref<MultiMesh> &mm : cell->lod_multimeshes) {
		_write_transforms(mm, transforms);
	}

	if (transforms.is_empty()) {
		// Free the now-empty cell instead of leaving permanent zero-instance
		// MultiMeshInstance3D nodes behind.
		for (MultiMeshInstance3D *node : cell->lod_nodes) {
			if (node != nullptr) {
				remove_child(node);
				node->queue_free();
			}
		}
		layer_cells[p_layer].erase(p_cell);
	}

	_mark_gpu_layer_dirty(p_layer);
	_refresh_layer_instance_count(p_layer);
	update_gizmos();
}

int FoliagePainter3D::add_instance(int p_layer, const Vector2i &p_cell, const Transform3D &p_transform) {
	ERR_FAIL_INDEX_V(p_layer, layers.size(), -1);

	FoliageCell &cell = _get_or_create_cell(p_layer, p_cell);
	const int index = cell.lod_multimeshes.is_empty() ? 0 : cell.lod_multimeshes[0]->get_instance_count();
	insert_instance(p_layer, p_cell, index, p_transform);
	return index;
}

Vector2i FoliagePainter3D::get_cell_for_local_position(const Vector3 &p_local_position) const {
	return Vector2i(
			(int)Math::floor(p_local_position.x / cell_size),
			(int)Math::floor(p_local_position.z / cell_size));
}

Vector<Vector2i> FoliagePainter3D::get_layer_cell_coords(int p_layer) const {
	Vector<Vector2i> result;
	if (p_layer < 0 || p_layer >= (int)layer_cells.size()) {
		return result;
	}
	for (const KeyValue<Vector2i, FoliageCell> &kv : layer_cells[p_layer]) {
		if (!kv.value.lod_multimeshes.is_empty() && kv.value.lod_multimeshes[0].is_valid() && kv.value.lod_multimeshes[0]->get_instance_count() > 0) {
			result.push_back(kv.key);
		}
	}
	return result;
}

int FoliagePainter3D::get_cell_instance_count(int p_layer, const Vector2i &p_cell) const {
	if (p_layer < 0 || p_layer >= (int)layer_cells.size()) {
		return 0;
	}
	const FoliageCell *cell = layer_cells[p_layer].getptr(p_cell);
	if (cell == nullptr || cell->lod_multimeshes.is_empty() || cell->lod_multimeshes[0].is_null()) {
		return 0;
	}
	return cell->lod_multimeshes[0]->get_instance_count();
}

Transform3D FoliagePainter3D::get_cell_instance_transform(int p_layer, const Vector2i &p_cell, int p_index) const {
	if (p_layer < 0 || p_layer >= (int)layer_cells.size()) {
		return Transform3D();
	}
	const FoliageCell *cell = layer_cells[p_layer].getptr(p_cell);
	if (cell == nullptr || cell->lod_multimeshes.is_empty() || cell->lod_multimeshes[0].is_null()) {
		return Transform3D();
	}
	return cell->lod_multimeshes[0]->get_instance_transform(p_index);
}

AABB FoliagePainter3D::get_cell_aabb(int p_layer, const Vector2i &p_cell) const {
	if (p_layer < 0 || p_layer >= (int)layer_cells.size()) {
		return AABB();
	}
	const FoliageCell *cell = layer_cells[p_layer].getptr(p_cell);
	if (cell == nullptr) {
		return AABB();
	}

	AABB result;
	bool first = true;
	for (const Ref<MultiMesh> &mm : cell->lod_multimeshes) {
		if (mm.is_null()) {
			continue;
		}
		if (first) {
			result = mm->get_aabb();
			first = false;
		} else {
			result.merge_with(mm->get_aabb());
		}
	}
	return result;
}

bool FoliagePainter3D::_is_gpu_culling_active() const {
	return gpu_culling && FoliageGPUCuller::is_supported();
}

void FoliagePainter3D::_clear_gpu_layers() {
	for (GPULayer *gpu_layer : gpu_layers) {
		if (gpu_layer == nullptr) {
			continue;
		}
		// Released before the MultiMeshes it reads from, since its uniform sets
		// reference their buffers.
		if (gpu_layer->culler != nullptr) {
			memdelete(gpu_layer->culler);
		}
		for (MultiMeshInstance3D *node : gpu_layer->lod_nodes) {
			if (node != nullptr) {
				remove_child(node);
				node->queue_free();
			}
		}
		memdelete(gpu_layer);
	}
	gpu_layers.clear();
	set_process_internal(false);
}

void FoliagePainter3D::_mark_gpu_layer_dirty(int p_layer) {
	if (!_is_gpu_culling_active() || p_layer < 0 || p_layer >= layers.size()) {
		return;
	}
	while ((int)gpu_layers.size() <= p_layer) {
		gpu_layers.push_back(memnew(GPULayer));
	}
	gpu_layers[p_layer]->dirty = true;
	// The rebuild itself happens in internal process, which has to be running
	// for it (and the per-frame cull dispatch) to ever get there.
	set_process_internal(true);
}

void FoliagePainter3D::_rebuild_gpu_layer(int p_layer) {
	if (!_is_gpu_culling_active() || p_layer < 0 || p_layer >= layers.size()) {
		return;
	}

	while ((int)gpu_layers.size() <= p_layer) {
		gpu_layers.push_back(memnew(GPULayer));
	}

	GPULayer *gpu_layer = gpu_layers[p_layer];
	gpu_layer->dirty = false;

	// Torn down before the MultiMeshes below are replaced, since the culler's
	// uniform sets reference their buffers.
	if (gpu_layer->culler != nullptr) {
		memdelete(gpu_layer->culler);
		gpu_layer->culler = nullptr;
	}
	for (MultiMeshInstance3D *node : gpu_layer->lod_nodes) {
		if (node != nullptr) {
			remove_child(node);
			node->queue_free();
		}
	}
	gpu_layer->lod_nodes.clear();
	gpu_layer->lod_multimeshes.clear();

	Ref<FoliageLayer> layer = layers[p_layer];
	if (layer.is_null()) {
		return;
	}

	// Every instance of this layer, gathered out of its cells into the flat
	// array the compute pass reads.
	LocalVector<Transform3D> transforms;
	AABB bounds;
	bool first = true;
	if (p_layer < (int)layer_cells.size()) {
		for (const KeyValue<Vector2i, FoliageCell> &kv : layer_cells[p_layer]) {
			if (kv.value.lod_multimeshes.is_empty() || kv.value.lod_multimeshes[0].is_null()) {
				continue;
			}
			const LocalVector<Transform3D> cell_transforms = _read_transforms(kv.value.lod_multimeshes[0]);
			for (const Transform3D &t : cell_transforms) {
				if (first) {
					bounds = AABB(t.origin, Vector3());
					first = false;
				} else {
					bounds.expand_to(t.origin);
				}
				transforms.push_back(t);
			}
		}
	}

	if (transforms.is_empty()) {
		return;
	}

	const TypedArray<FoliageLODLevel> lod_levels = layer->get_lod_levels();
	const int lod_count = MIN(lod_levels.size(), FoliageGPUCuller::MAX_LOD_LEVELS);

	LocalVector<FoliageGPUCuller::LODLevel> culler_levels;
	float previous_range_end = 0.0f;

	for (int i = 0; i < lod_count; i++) {
		Ref<FoliageLODLevel> level = lod_levels[i];
		if (level.is_null() || level->get_mesh().is_null()) {
			continue;
		}

		const AABB mesh_aabb = level->get_mesh()->get_aabb();
		// Distance from the instance origin to the farthest corner of the mesh,
		// since a mesh is rarely centered on its origin.
		const Vector3 farthest(
				MAX(Math::abs(mesh_aabb.position.x), Math::abs(mesh_aabb.position.x + mesh_aabb.size.x)),
				MAX(Math::abs(mesh_aabb.position.y), Math::abs(mesh_aabb.position.y + mesh_aabb.size.y)),
				MAX(Math::abs(mesh_aabb.position.z), Math::abs(mesh_aabb.position.z + mesh_aabb.size.z)));
		const float instance_radius = farthest.length();

		Ref<MultiMesh> mm;
		mm.instantiate();
		// Order matters: the indirect flag has to be set before the buffers are
		// allocated, and the mesh after, since that is what builds the draw
		// command buffer the compute pass writes into.
		mm->set_use_indirect(true);
		mm->set_transform_format(MultiMesh::TRANSFORM_3D);
		mm->set_instance_count((int)transforms.size());
		mm->set_mesh(level->get_mesh());
		// How many instances survive culling is only known on the GPU, so the
		// bounds are stated up front: every instance origin, plus its reach.
		mm->set_custom_aabb(bounds.grow(instance_radius * MAX(1.0f, layer->get_max_scale())));

		MultiMeshInstance3D *node = memnew(MultiMeshInstance3D);
		node->set_multimesh(mm);
		node->set_material_override(level->get_material_override());
		node->set_cast_shadows_setting(layer->is_casting_shadows() ? GeometryInstance3D::SHADOW_CASTING_SETTING_ON : GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		node->set_ignore_screen_space_shadows(layer->is_ignoring_screen_space_shadows());
		node->set_lod_bias(layer->get_lod_bias());
		node->set_gi_mode(layer->get_gi_mode());
		// No visibility range: one node covers every instance of the layer, so
		// the distance banding is the compute pass's job instead.
		add_child(node, false, INTERNAL_MODE_FRONT);

		FoliageGPUCuller::LODLevel culler_level;
		culler_level.multimesh = mm->get_rid();
		// Bands are walked forward and clamped so that an instance lands in
		// exactly one level: overlapping ranges would otherwise draw it twice,
		// and the GPU path has no cross-fade to blend them with.
		culler_level.range_begin = MAX(level->get_visibility_range_begin(), previous_range_end);
		culler_level.range_end = level->get_visibility_range_end();
		if (culler_level.range_end > 0.0f) {
			culler_level.range_end = MAX(culler_level.range_end, culler_level.range_begin);
			previous_range_end = culler_level.range_end;
		}
		culler_level.instance_radius = instance_radius;
		culler_level.surface_count = MAX(1, level->get_mesh()->get_surface_count());

		gpu_layer->lod_multimeshes.push_back(mm);
		gpu_layer->lod_nodes.push_back(node);
		culler_levels.push_back(culler_level);
	}

	if (culler_levels.is_empty()) {
		return;
	}

	gpu_layer->culler = memnew(FoliageGPUCuller);
	gpu_layer->culler->update_instances(transforms, culler_levels);
	set_process_internal(true);
}

void FoliagePainter3D::_dispatch_gpu_culling() {
	if (gpu_layers.is_empty() || !is_inside_tree()) {
		return;
	}

	// Coalesced here rather than done per insert/remove, so a brush stroke that
	// touches the same layer many times in one frame rebuilds its buffer once.
	for (uint32_t i = 0; i < gpu_layers.size(); i++) {
		if (gpu_layers[i] != nullptr && gpu_layers[i]->dirty) {
			_rebuild_gpu_layer((int)i);
		}
	}

	Camera3D *camera = FoliageGPUCuller::resolve_culling_camera(this, gpu_culling_camera);
	if (camera == nullptr) {
		// Better to draw everything than to have the foliage vanish because
		// there is nothing to cull against.
		for (GPULayer *gpu_layer : gpu_layers) {
			if (gpu_layer != nullptr && gpu_layer->culler != nullptr) {
				gpu_layer->culler->draw_without_culling();
			}
		}
		return;
	}

	// The compute pass works in the MultiMeshes' local space, which is this
	// node's own space, so the frustum is brought over rather than every
	// instance being transformed into world space.
	const Transform3D to_local = get_global_transform().affine_inverse();

	Vector<Plane> planes = camera->get_frustum();
	for (int i = 0; i < planes.size(); i++) {
		planes.write[i] = to_local.xform(planes[i]);
	}
	const Vector3 camera_position = to_local.xform(camera->get_global_position());

	for (GPULayer *gpu_layer : gpu_layers) {
		if (gpu_layer != nullptr && gpu_layer->culler != nullptr) {
			gpu_layer->culler->cull(planes, camera_position);
		}
	}
}

void FoliagePainter3D::set_gpu_culling(bool p_enabled) {
	if (gpu_culling == p_enabled) {
		return;
	}
	gpu_culling = p_enabled;

	// Cells are the source of truth either way, so nothing painted is lost;
	// only what renders them changes. _sync_layer_cells drops the per-cell
	// nodes (or brings them back), and the GPU layers are rebuilt from the
	// cells that are already there.
	_clear_gpu_layers();
	for (int i = 0; i < layers.size(); i++) {
		_sync_layer_cells(i);
		_mark_gpu_layer_dirty(i);
	}

	update_gizmos();
	update_configuration_warnings();
	notify_property_list_changed();
}

bool FoliagePainter3D::is_gpu_culling_enabled() const {
	return gpu_culling;
}

Array FoliagePainter3D::_get_cell_data() const {
	Array result;
	for (uint32_t li = 0; li < layer_cells.size(); li++) {
		for (const KeyValue<Vector2i, FoliageCell> &kv : layer_cells[li]) {
			if (kv.value.lod_multimeshes.is_empty() || kv.value.lod_multimeshes[0].is_null() || kv.value.lod_multimeshes[0]->get_instance_count() == 0) {
				continue;
			}
			Array lod_multimeshes;
			for (const Ref<MultiMesh> &mm : kv.value.lod_multimeshes) {
				lod_multimeshes.push_back(mm);
			}
			Array entry;
			entry.push_back((int)li);
			entry.push_back(kv.key);
			entry.push_back(lod_multimeshes);
			result.push_back(entry);
		}
	}
	return result;
}

void FoliagePainter3D::_set_cell_data(const Array &p_data) {
	for (uint32_t li = 0; li < layer_cells.size(); li++) {
		for (KeyValue<Vector2i, FoliageCell> &kv : layer_cells[li]) {
			for (MultiMeshInstance3D *node : kv.value.lod_nodes) {
				if (node != nullptr) {
					remove_child(node);
					node->queue_free();
				}
			}
		}
	}
	layer_cells.clear();
	_ensure_layer_cells_size();

	for (int i = 0; i < p_data.size(); i++) {
		Array entry = p_data[i];
		if (entry.size() != 3) {
			continue;
		}
		const int layer_idx = entry[0];
		const Vector2i cell_coord = entry[1];
		Array lod_multimeshes = entry[2];
		if (layer_idx < 0 || lod_multimeshes.is_empty()) {
			continue;
		}
		if (layer_idx >= (int)layer_cells.size()) {
			layer_cells.resize(layer_idx + 1);
		}

		FoliageCell cell;
		for (int j = 0; j < lod_multimeshes.size(); j++) {
			Ref<MultiMesh> mm = lod_multimeshes[j];
			if (mm.is_valid()) {
				cell.lod_multimeshes.push_back(mm);
			}
		}
		if (cell.lod_multimeshes.is_empty()) {
			continue;
		}
		layer_cells[layer_idx][cell_coord] = cell;
		_sync_cell_lods(layer_idx, layer_cells[layer_idx][cell_coord]);
	}

	for (int i = 0; i < layers.size(); i++) {
		_mark_gpu_layer_dirty(i);
		_refresh_layer_instance_count(i);
	}
	update_gizmos();
}

PackedStringArray FoliagePainter3D::get_configuration_warnings() const {
	PackedStringArray warnings = Node3D::get_configuration_warnings();

	if (layers.is_empty()) {
		warnings.push_back(RTR("No foliage layers configured. Add at least one layer with a Mesh, then use the foliage brush in the 3D viewport to paint instances."));
	}

	for (int i = 0; i < layers.size(); i++) {
		Ref<FoliageLayer> layer = layers[i];
		if (layer.is_valid() && !layer->has_any_mesh()) {
			warnings.push_back(vformat(RTR("Layer %d (\"%s\") has no Mesh assigned on any of its LOD levels."), i, layer->get_layer_name()));
		}
		if (gpu_culling && layer.is_valid() && layer->get_lod_levels().size() > FoliageGPUCuller::MAX_LOD_LEVELS) {
			warnings.push_back(vformat(RTR("GPU Culling only drives the first %d LOD levels of layer %d; the rest are not rendered."), FoliageGPUCuller::MAX_LOD_LEVELS, i));
		}
	}

	if (gpu_culling && !FoliageGPUCuller::is_supported()) {
		warnings.push_back(RTR("GPU Culling needs a RenderingDevice-based renderer (Forward+ or Mobile); the Compatibility renderer cannot draw indirect MultiMeshes. Falling back to cell chunking."));
	}

	return warnings;
}

FoliagePainter3D::FoliagePainter3D() {
}

FoliagePainter3D::~FoliagePainter3D() {
	for (GPULayer *gpu_layer : gpu_layers) {
		if (gpu_layer == nullptr) {
			continue;
		}
		if (gpu_layer->culler != nullptr) {
			memdelete(gpu_layer->culler);
		}
		memdelete(gpu_layer);
	}
}
