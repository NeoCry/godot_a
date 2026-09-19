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
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/resources/multimesh.h"

void FoliagePainter3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_layers", "layers"), &FoliagePainter3D::set_layers);
	ClassDB::bind_method(D_METHOD("get_layers"), &FoliagePainter3D::get_layers);

	ClassDB::bind_method(D_METHOD("set_cell_size", "size"), &FoliagePainter3D::set_cell_size);
	ClassDB::bind_method(D_METHOD("get_cell_size"), &FoliagePainter3D::get_cell_size);

	ClassDB::bind_method(D_METHOD("get_layer_count"), &FoliagePainter3D::get_layer_count);
	ClassDB::bind_method(D_METHOD("get_layer", "layer_index"), &FoliagePainter3D::get_layer);

	ClassDB::bind_method(D_METHOD("insert_instance", "layer_index", "cell", "index", "transform"), &FoliagePainter3D::insert_instance);
	ClassDB::bind_method(D_METHOD("remove_instance", "layer_index", "cell", "index"), &FoliagePainter3D::remove_instance);
	ClassDB::bind_method(D_METHOD("add_instance", "layer_index", "cell", "transform"), &FoliagePainter3D::add_instance);

	ClassDB::bind_method(D_METHOD("_get_cell_data"), &FoliagePainter3D::_get_cell_data);
	ClassDB::bind_method(D_METHOD("_set_cell_data", "data"), &FoliagePainter3D::_set_cell_data);

	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "layers", PROPERTY_HINT_ARRAY_TYPE, MAKE_RESOURCE_TYPE_HINT("FoliageLayer")), "set_layers", "get_layers");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cell_size", PROPERTY_HINT_RANGE, "1,256,0.5,or_greater,suffix:m"), "set_cell_size", "get_cell_size");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "_cell_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_INTERNAL | PROPERTY_USAGE_STORAGE), "_set_cell_data", "_get_cell_data");
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
	const int lod_count = lod_levels.size();

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
}

void FoliagePainter3D::_on_layer_changed(int p_index) {
	if (p_index >= 0 && p_index < layers.size()) {
		_sync_layer_cells(p_index);
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
	}

	return warnings;
}

FoliagePainter3D::FoliagePainter3D() {
}
