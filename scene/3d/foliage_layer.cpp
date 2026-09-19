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

#include "core/core_string_names.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"

void FoliageLayer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_layer_name", "name"), &FoliageLayer::set_layer_name);
	ClassDB::bind_method(D_METHOD("get_layer_name"), &FoliageLayer::get_layer_name);

	ClassDB::bind_method(D_METHOD("set_lod_levels", "levels"), &FoliageLayer::set_lod_levels);
	ClassDB::bind_method(D_METHOD("get_lod_levels"), &FoliageLayer::get_lod_levels);
	ClassDB::bind_method(D_METHOD("has_any_mesh"), &FoliageLayer::has_any_mesh);

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

	ClassDB::bind_method(D_METHOD("set_ignore_screen_space_shadows", "ignore"), &FoliageLayer::set_ignore_screen_space_shadows);
	ClassDB::bind_method(D_METHOD("is_ignoring_screen_space_shadows"), &FoliageLayer::is_ignoring_screen_space_shadows);

	ClassDB::bind_method(D_METHOD("set_lod_bias", "bias"), &FoliageLayer::set_lod_bias);
	ClassDB::bind_method(D_METHOD("get_lod_bias"), &FoliageLayer::get_lod_bias);

	ClassDB::bind_method(D_METHOD("set_gi_mode", "mode"), &FoliageLayer::set_gi_mode);
	ClassDB::bind_method(D_METHOD("get_gi_mode"), &FoliageLayer::get_gi_mode);

	ClassDB::bind_method(D_METHOD("get_instance_count"), &FoliageLayer::get_instance_count);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "layer_name"), "set_layer_name", "get_layer_name");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "lod_levels", PROPERTY_HINT_ARRAY_TYPE, MAKE_RESOURCE_TYPE_HINT("FoliageLODLevel")), "set_lod_levels", "get_lod_levels");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "instance_count", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "get_instance_count");

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
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ignore_screen_space_shadows"), "set_ignore_screen_space_shadows", "is_ignoring_screen_space_shadows");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lod_bias", PROPERTY_HINT_RANGE, "0.001,128,0.001"), "set_lod_bias", "get_lod_bias");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "gi_mode", PROPERTY_HINT_ENUM, "Disabled,Static,Dynamic"), "set_gi_mode", "get_gi_mode");
}

void FoliageLayer::set_layer_name(const String &p_name) {
	layer_name = p_name;
	emit_changed();
}

String FoliageLayer::get_layer_name() const {
	return layer_name;
}

void FoliageLayer::set_lod_levels(const TypedArray<FoliageLODLevel> &p_levels) {
	for (int i = 0; i < lod_levels.size(); i++) {
		Ref<FoliageLODLevel> old_level = lod_levels[i];
		if (old_level.is_valid()) {
			old_level->disconnect(CoreStringName(changed), callable_mp(this, &FoliageLayer::_on_lod_level_changed));
		}
	}

	lod_levels = p_levels;

	for (int i = 0; i < lod_levels.size(); i++) {
		Ref<FoliageLODLevel> level = lod_levels[i];
		if (level.is_valid()) {
			level->connect(CoreStringName(changed), callable_mp(this, &FoliageLayer::_on_lod_level_changed));
		}
	}

	emit_changed();
}

TypedArray<FoliageLODLevel> FoliageLayer::get_lod_levels() const {
	return lod_levels;
}

void FoliageLayer::_on_lod_level_changed() {
	emit_changed();
}

bool FoliageLayer::has_any_mesh() const {
	for (int i = 0; i < lod_levels.size(); i++) {
		Ref<FoliageLODLevel> level = lod_levels[i];
		if (level.is_valid() && level->get_mesh().is_valid()) {
			return true;
		}
	}
	return false;
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

void FoliageLayer::set_ignore_screen_space_shadows(bool p_ignore) {
	ignore_screen_space_shadows = p_ignore;
	emit_changed();
}

bool FoliageLayer::is_ignoring_screen_space_shadows() const {
	return ignore_screen_space_shadows;
}

void FoliageLayer::set_lod_bias(float p_bias) {
	lod_bias = MAX(p_bias, 0.001f);
	emit_changed();
}

float FoliageLayer::get_lod_bias() const {
	return lod_bias;
}

void FoliageLayer::set_gi_mode(GeometryInstance3D::GIMode p_mode) {
	gi_mode = p_mode;
	emit_changed();
}

GeometryInstance3D::GIMode FoliageLayer::get_gi_mode() const {
	return gi_mode;
}

int FoliageLayer::get_instance_count() const {
	return instance_count;
}

void FoliageLayer::_set_display_instance_count(int p_count) {
	if (instance_count == p_count) {
		return;
	}
	instance_count = p_count;
	// Refreshes an open Inspector without marking the resource changed/dirty
	// (this is derived data owned by FoliagePainter3D, not saved here).
	notify_property_list_changed();
}

FoliageLayer::FoliageLayer() {
	TypedArray<FoliageLODLevel> defaults;

	Ref<FoliageLODLevel> lod0;
	lod0.instantiate();
	lod0->set_visibility_range_end(30.0f);
	lod0->set_visibility_range_end_margin(5.0f);
	lod0->set_visibility_range_fade_mode(GeometryInstance3D::VISIBILITY_RANGE_FADE_SELF);
	defaults.push_back(lod0);

	Ref<FoliageLODLevel> lod1;
	lod1.instantiate();
	lod1->set_visibility_range_begin(25.0f);
	lod1->set_visibility_range_begin_margin(5.0f);
	lod1->set_visibility_range_end(80.0f);
	lod1->set_visibility_range_end_margin(10.0f);
	lod1->set_visibility_range_fade_mode(GeometryInstance3D::VISIBILITY_RANGE_FADE_SELF);
	defaults.push_back(lod1);

	Ref<FoliageLODLevel> lod2;
	lod2.instantiate();
	lod2->set_visibility_range_begin(70.0f);
	lod2->set_visibility_range_begin_margin(10.0f);
	lod2->set_visibility_range_fade_mode(GeometryInstance3D::VISIBILITY_RANGE_FADE_SELF);
	defaults.push_back(lod2);

	set_lod_levels(defaults);
}
