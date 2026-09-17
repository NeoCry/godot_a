/**************************************************************************/
/*  foliage_layer.cpp                                                     */
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

#include "foliage_layer.h"

#include "scene/resources/mesh.h"

void FoliageLayer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_layer_name", "name"), &FoliageLayer::set_layer_name);
	ClassDB::bind_method(D_METHOD("get_layer_name"), &FoliageLayer::get_layer_name);

	ClassDB::bind_method(D_METHOD("set_mesh", "mesh"), &FoliageLayer::set_mesh);
	ClassDB::bind_method(D_METHOD("get_mesh"), &FoliageLayer::get_mesh);

	ClassDB::bind_method(D_METHOD("set_material_override", "material"), &FoliageLayer::set_material_override);
	ClassDB::bind_method(D_METHOD("get_material_override"), &FoliageLayer::get_material_override);

	ClassDB::bind_method(D_METHOD("get_multimesh"), &FoliageLayer::get_multimesh);

	ClassDB::bind_method(D_METHOD("set_min_scale", "scale"), &FoliageLayer::set_min_scale);
	ClassDB::bind_method(D_METHOD("get_min_scale"), &FoliageLayer::get_min_scale);

	ClassDB::bind_method(D_METHOD("set_max_scale", "scale"), &FoliageLayer::set_max_scale);
	ClassDB::bind_method(D_METHOD("get_max_scale"), &FoliageLayer::get_max_scale);

	ClassDB::bind_method(D_METHOD("set_random_rotation", "random"), &FoliageLayer::set_random_rotation);
	ClassDB::bind_method(D_METHOD("is_random_rotation_enabled"), &FoliageLayer::is_random_rotation_enabled);

	ClassDB::bind_method(D_METHOD("set_align_to_normal_amount", "amount"), &FoliageLayer::set_align_to_normal_amount);
	ClassDB::bind_method(D_METHOD("get_align_to_normal_amount"), &FoliageLayer::get_align_to_normal_amount);

	ClassDB::bind_method(D_METHOD("set_random_tilt_degrees", "degrees"), &FoliageLayer::set_random_tilt_degrees);
	ClassDB::bind_method(D_METHOD("get_random_tilt_degrees"), &FoliageLayer::get_random_tilt_degrees);

	ClassDB::bind_method(D_METHOD("set_density", "density"), &FoliageLayer::set_density);
	ClassDB::bind_method(D_METHOD("get_density"), &FoliageLayer::get_density);

	ClassDB::bind_method(D_METHOD("set_min_instance_spacing", "spacing"), &FoliageLayer::set_min_instance_spacing);
	ClassDB::bind_method(D_METHOD("get_min_instance_spacing"), &FoliageLayer::get_min_instance_spacing);

	ClassDB::bind_method(D_METHOD("set_cast_shadows", "enable"), &FoliageLayer::set_cast_shadows);
	ClassDB::bind_method(D_METHOD("is_casting_shadows"), &FoliageLayer::is_casting_shadows);

	ClassDB::bind_method(D_METHOD("set_visibility_range_begin", "distance"), &FoliageLayer::set_visibility_range_begin);
	ClassDB::bind_method(D_METHOD("get_visibility_range_begin"), &FoliageLayer::get_visibility_range_begin);

	ClassDB::bind_method(D_METHOD("set_visibility_range_begin_margin", "distance"), &FoliageLayer::set_visibility_range_begin_margin);
	ClassDB::bind_method(D_METHOD("get_visibility_range_begin_margin"), &FoliageLayer::get_visibility_range_begin_margin);

	ClassDB::bind_method(D_METHOD("set_visibility_range_end", "distance"), &FoliageLayer::set_visibility_range_end);
	ClassDB::bind_method(D_METHOD("get_visibility_range_end"), &FoliageLayer::get_visibility_range_end);

	ClassDB::bind_method(D_METHOD("set_visibility_range_end_margin", "distance"), &FoliageLayer::set_visibility_range_end_margin);
	ClassDB::bind_method(D_METHOD("get_visibility_range_end_margin"), &FoliageLayer::get_visibility_range_end_margin);

	ClassDB::bind_method(D_METHOD("set_visibility_range_fade_mode", "mode"), &FoliageLayer::set_visibility_range_fade_mode);
	ClassDB::bind_method(D_METHOD("get_visibility_range_fade_mode"), &FoliageLayer::get_visibility_range_fade_mode);

	ClassDB::bind_method(D_METHOD("get_instance_count"), &FoliageLayer::get_instance_count);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "layer_name"), "set_layer_name", "get_layer_name");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "mesh", PROPERTY_HINT_RESOURCE_TYPE, "Mesh"), "set_mesh", "get_mesh");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material_override", PROPERTY_HINT_RESOURCE_TYPE, "BaseMaterial3D,ShaderMaterial"), "set_material_override", "get_material_override");

	ADD_GROUP("Randomization", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "min_scale", PROPERTY_HINT_RANGE, "0.01,10.0,0.001,or_greater"), "set_min_scale", "get_min_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_scale", PROPERTY_HINT_RANGE, "0.01,10.0,0.001,or_greater"), "set_max_scale", "get_max_scale");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "random_rotation"), "set_random_rotation", "is_random_rotation_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "align_to_normal_amount", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_align_to_normal_amount", "get_align_to_normal_amount");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "random_tilt_degrees", PROPERTY_HINT_RANGE, "0,90,0.1,suffix:°"), "set_random_tilt_degrees", "get_random_tilt_degrees");

	ADD_GROUP("Brush", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "density", PROPERTY_HINT_RANGE, "0.01,50.0,0.01,or_greater"), "set_density", "get_density");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "min_instance_spacing", PROPERTY_HINT_RANGE, "0.0,50.0,0.01,or_greater,suffix:m"), "set_min_instance_spacing", "get_min_instance_spacing");

	ADD_GROUP("Rendering", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cast_shadows"), "set_cast_shadows", "is_casting_shadows");

	ADD_GROUP("Visibility Range", "visibility_range_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "visibility_range_begin", PROPERTY_HINT_RANGE, "0,4096,0.01,or_greater,suffix:m"), "set_visibility_range_begin", "get_visibility_range_begin");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "visibility_range_begin_margin", PROPERTY_HINT_RANGE, "0,4096,0.01,or_greater,suffix:m"), "set_visibility_range_begin_margin", "get_visibility_range_begin_margin");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "visibility_range_end", PROPERTY_HINT_RANGE, "0,4096,0.01,or_greater,suffix:m"), "set_visibility_range_end", "get_visibility_range_end");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "visibility_range_end_margin", PROPERTY_HINT_RANGE, "0,4096,0.01,or_greater,suffix:m"), "set_visibility_range_end_margin", "get_visibility_range_end_margin");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "visibility_range_fade_mode", PROPERTY_HINT_ENUM, "Disabled,Self,Dependencies"), "set_visibility_range_fade_mode", "get_visibility_range_fade_mode");
}

void FoliageLayer::set_layer_name(const String &p_name) {
	layer_name = p_name;
	emit_changed();
}

String FoliageLayer::get_layer_name() const {
	return layer_name;
}

void FoliageLayer::set_mesh(const Ref<Mesh> &p_mesh) {
	mesh = p_mesh;
	multimesh->set_mesh(mesh);
	emit_changed();
}

Ref<Mesh> FoliageLayer::get_mesh() const {
	return mesh;
}

void FoliageLayer::set_material_override(const Ref<Material> &p_material) {
	material_override = p_material;
	emit_changed();
}

Ref<Material> FoliageLayer::get_material_override() const {
	return material_override;
}

Ref<MultiMesh> FoliageLayer::get_multimesh() const {
	return multimesh;
}

void FoliageLayer::set_min_scale(float p_scale) {
	min_scale = MAX(p_scale, 0.001f);
}

float FoliageLayer::get_min_scale() const {
	return min_scale;
}

void FoliageLayer::set_max_scale(float p_scale) {
	max_scale = MAX(p_scale, 0.001f);
}

float FoliageLayer::get_max_scale() const {
	return max_scale;
}

void FoliageLayer::set_random_rotation(bool p_random) {
	random_rotation = p_random;
}

bool FoliageLayer::is_random_rotation_enabled() const {
	return random_rotation;
}

void FoliageLayer::set_align_to_normal_amount(float p_amount) {
	align_to_normal_amount = CLAMP(p_amount, 0.0f, 1.0f);
}

float FoliageLayer::get_align_to_normal_amount() const {
	return align_to_normal_amount;
}

void FoliageLayer::set_random_tilt_degrees(float p_degrees) {
	random_tilt_degrees = CLAMP(p_degrees, 0.0f, 90.0f);
}

float FoliageLayer::get_random_tilt_degrees() const {
	return random_tilt_degrees;
}

void FoliageLayer::set_density(float p_density) {
	density = MAX(p_density, 0.01f);
}

float FoliageLayer::get_density() const {
	return density;
}

void FoliageLayer::set_min_instance_spacing(float p_spacing) {
	min_instance_spacing = MAX(p_spacing, 0.0f);
}

float FoliageLayer::get_min_instance_spacing() const {
	return min_instance_spacing;
}

void FoliageLayer::set_cast_shadows(bool p_enable) {
	cast_shadows = p_enable;
	emit_changed();
}

bool FoliageLayer::is_casting_shadows() const {
	return cast_shadows;
}

void FoliageLayer::set_visibility_range_begin(float p_dist) {
	visibility_range_begin = MAX(p_dist, 0.0f);
	emit_changed();
}

float FoliageLayer::get_visibility_range_begin() const {
	return visibility_range_begin;
}

void FoliageLayer::set_visibility_range_begin_margin(float p_dist) {
	visibility_range_begin_margin = MAX(p_dist, 0.0f);
	emit_changed();
}

float FoliageLayer::get_visibility_range_begin_margin() const {
	return visibility_range_begin_margin;
}

void FoliageLayer::set_visibility_range_end(float p_dist) {
	visibility_range_end = MAX(p_dist, 0.0f);
	emit_changed();
}

float FoliageLayer::get_visibility_range_end() const {
	return visibility_range_end;
}

void FoliageLayer::set_visibility_range_end_margin(float p_dist) {
	visibility_range_end_margin = MAX(p_dist, 0.0f);
	emit_changed();
}

float FoliageLayer::get_visibility_range_end_margin() const {
	return visibility_range_end_margin;
}

void FoliageLayer::set_visibility_range_fade_mode(GeometryInstance3D::VisibilityRangeFadeMode p_mode) {
	visibility_range_fade_mode = p_mode;
	emit_changed();
}

GeometryInstance3D::VisibilityRangeFadeMode FoliageLayer::get_visibility_range_fade_mode() const {
	return visibility_range_fade_mode;
}

int FoliageLayer::get_instance_count() const {
	return multimesh->get_instance_count();
}

FoliageLayer::FoliageLayer() {
	multimesh.instantiate();
	multimesh->set_transform_format(MultiMesh::TRANSFORM_3D);
	multimesh->set_instance_count(0);
}
