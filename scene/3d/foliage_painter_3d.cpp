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
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/resources/multimesh.h"

void FoliagePainter3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_layers", "layers"), &FoliagePainter3D::set_layers);
	ClassDB::bind_method(D_METHOD("get_layers"), &FoliagePainter3D::get_layers);

	ClassDB::bind_method(D_METHOD("get_layer_count"), &FoliagePainter3D::get_layer_count);
	ClassDB::bind_method(D_METHOD("get_layer", "layer_index"), &FoliagePainter3D::get_layer);

	ClassDB::bind_method(D_METHOD("insert_instance", "layer_index", "index", "transform"), &FoliagePainter3D::insert_instance);
	ClassDB::bind_method(D_METHOD("remove_instance", "layer_index", "index"), &FoliagePainter3D::remove_instance);
	ClassDB::bind_method(D_METHOD("add_instance", "layer_index", "transform"), &FoliagePainter3D::add_instance);

	ClassDB::bind_method(D_METHOD("get_layer_instance_count", "layer_index"), &FoliagePainter3D::get_layer_instance_count);
	ClassDB::bind_method(D_METHOD("get_layer_instance_transform", "layer_index", "index"), &FoliagePainter3D::get_layer_instance_transform);

	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "layers", PROPERTY_HINT_ARRAY_TYPE, MAKE_RESOURCE_TYPE_HINT("FoliageLayer")), "set_layers", "get_layers");
}

void FoliagePainter3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			_update_layer_nodes();
		} break;
	}
}

void FoliagePainter3D::_update_layer_nodes() {
	// Grow or shrink the pool of internal MultiMeshInstance3D children to match
	// the layer count, preserving existing nodes (and their RenderingServer
	// instances) for indices that still exist.
	while ((int)layer_nodes.size() > layers.size()) {
		MultiMeshInstance3D *extra = layer_nodes[layer_nodes.size() - 1];
		layer_nodes.remove_at(layer_nodes.size() - 1);
		if (extra != nullptr) {
			remove_child(extra);
			extra->queue_free();
		}
	}

	while ((int)layer_nodes.size() < layers.size()) {
		MultiMeshInstance3D *mmi = memnew(MultiMeshInstance3D);
		// Baked GI is generally not worth it for scattered foliage (see FoliageSpawner3D).
		mmi->set_gi_mode(GeometryInstance3D::GI_MODE_DISABLED);
		add_child(mmi, false, INTERNAL_MODE_FRONT);
		layer_nodes.push_back(mmi);
	}

	for (int i = 0; i < layers.size(); i++) {
		_sync_layer_node(i);
	}
}

void FoliagePainter3D::_sync_layer_node(int p_index) {
	ERR_FAIL_INDEX(p_index, layers.size());
	ERR_FAIL_INDEX(p_index, (int)layer_nodes.size());

	Ref<FoliageLayer> layer = layers[p_index];
	MultiMeshInstance3D *mmi = layer_nodes[p_index];
	if (layer.is_null() || mmi == nullptr) {
		return;
	}

	mmi->set_name(layer->get_layer_name().is_empty() ? String("Layer") : layer->get_layer_name());
	mmi->set_multimesh(layer->get_multimesh());
	mmi->set_material_override(layer->get_material_override());
	mmi->set_cast_shadows_setting(layer->is_casting_shadows() ? GeometryInstance3D::SHADOW_CASTING_SETTING_ON : GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	mmi->set_visibility_range_begin(layer->get_visibility_range_begin());
	mmi->set_visibility_range_begin_margin(layer->get_visibility_range_begin_margin());
	mmi->set_visibility_range_end(layer->get_visibility_range_end());
	mmi->set_visibility_range_end_margin(layer->get_visibility_range_end_margin());
	mmi->set_visibility_range_fade_mode(layer->get_visibility_range_fade_mode());
}

void FoliagePainter3D::_on_layer_changed(int p_index) {
	if (p_index >= 0 && p_index < layers.size() && p_index < (int)layer_nodes.size()) {
		_sync_layer_node(p_index);
	}
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

	_update_layer_nodes();
	update_configuration_warnings();
}

TypedArray<FoliageLayer> FoliagePainter3D::get_layers() const {
	return layers;
}

int FoliagePainter3D::get_layer_count() const {
	return layers.size();
}

Ref<FoliageLayer> FoliagePainter3D::get_layer(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, layers.size(), Ref<FoliageLayer>());
	return layers[p_index];
}

void FoliagePainter3D::insert_instance(int p_layer, int p_index, const Transform3D &p_transform) {
	ERR_FAIL_INDEX(p_layer, layers.size());
	Ref<FoliageLayer> layer = layers[p_layer];
	ERR_FAIL_COND(layer.is_null());

	Ref<MultiMesh> mm = layer->get_multimesh();
	int count = mm->get_instance_count();
	p_index = CLAMP(p_index, 0, count);

	mm->set_instance_count(count + 1);
	for (int i = count; i > p_index; i--) {
		mm->set_instance_transform(i, mm->get_instance_transform(i - 1));
	}
	mm->set_instance_transform(p_index, p_transform);
}

void FoliagePainter3D::remove_instance(int p_layer, int p_index) {
	ERR_FAIL_INDEX(p_layer, layers.size());
	Ref<FoliageLayer> layer = layers[p_layer];
	ERR_FAIL_COND(layer.is_null());

	Ref<MultiMesh> mm = layer->get_multimesh();
	int count = mm->get_instance_count();
	ERR_FAIL_INDEX(p_index, count);

	for (int i = p_index; i < count - 1; i++) {
		mm->set_instance_transform(i, mm->get_instance_transform(i + 1));
	}
	mm->set_instance_count(count - 1);
}

int FoliagePainter3D::add_instance(int p_layer, const Transform3D &p_transform) {
	ERR_FAIL_INDEX_V(p_layer, layers.size(), -1);
	Ref<FoliageLayer> layer = layers[p_layer];
	ERR_FAIL_COND_V(layer.is_null(), -1);

	int index = layer->get_multimesh()->get_instance_count();
	insert_instance(p_layer, index, p_transform);
	return index;
}

int FoliagePainter3D::get_layer_instance_count(int p_layer) const {
	ERR_FAIL_INDEX_V(p_layer, layers.size(), 0);
	Ref<FoliageLayer> layer = layers[p_layer];
	ERR_FAIL_COND_V(layer.is_null(), 0);
	return layer->get_multimesh()->get_instance_count();
}

Transform3D FoliagePainter3D::get_layer_instance_transform(int p_layer, int p_index) const {
	ERR_FAIL_INDEX_V(p_layer, layers.size(), Transform3D());
	Ref<FoliageLayer> layer = layers[p_layer];
	ERR_FAIL_COND_V(layer.is_null(), Transform3D());
	return layer->get_multimesh()->get_instance_transform(p_index);
}

PackedStringArray FoliagePainter3D::get_configuration_warnings() const {
	PackedStringArray warnings = Node3D::get_configuration_warnings();

	if (layers.is_empty()) {
		warnings.push_back(RTR("No foliage layers configured. Add at least one layer with a Mesh, then use the foliage brush in the 3D viewport to paint instances."));
	}

	for (int i = 0; i < layers.size(); i++) {
		Ref<FoliageLayer> layer = layers[i];
		if (layer.is_valid() && layer->get_mesh().is_null()) {
			warnings.push_back(vformat(RTR("Layer %d (\"%s\") has no Mesh assigned."), i, layer->get_layer_name()));
		}
	}

	return warnings;
}

FoliagePainter3D::FoliagePainter3D() {
}
