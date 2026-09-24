/**************************************************************************/
/*  foliage_lod_level.cpp                                                 */
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

#include "foliage_lod_level.h"

#include "core/object/class_db.h"
#include "scene/resources/mesh.h"

void FoliageLODLevel::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_mesh", "mesh"), &FoliageLODLevel::set_mesh);
	ClassDB::bind_method(D_METHOD("get_mesh"), &FoliageLODLevel::get_mesh);

	ClassDB::bind_method(D_METHOD("set_material_override", "material"), &FoliageLODLevel::set_material_override);
	ClassDB::bind_method(D_METHOD("get_material_override"), &FoliageLODLevel::get_material_override);

	ClassDB::bind_method(D_METHOD("set_cast_shadows", "enable"), &FoliageLODLevel::set_cast_shadows);
	ClassDB::bind_method(D_METHOD("is_casting_shadows"), &FoliageLODLevel::is_casting_shadows);

	ClassDB::bind_method(D_METHOD("set_visibility_range_begin", "distance"), &FoliageLODLevel::set_visibility_range_begin);
	ClassDB::bind_method(D_METHOD("get_visibility_range_begin"), &FoliageLODLevel::get_visibility_range_begin);

	ClassDB::bind_method(D_METHOD("set_visibility_range_begin_margin", "distance"), &FoliageLODLevel::set_visibility_range_begin_margin);
	ClassDB::bind_method(D_METHOD("get_visibility_range_begin_margin"), &FoliageLODLevel::get_visibility_range_begin_margin);

	ClassDB::bind_method(D_METHOD("set_visibility_range_end", "distance"), &FoliageLODLevel::set_visibility_range_end);
	ClassDB::bind_method(D_METHOD("get_visibility_range_end"), &FoliageLODLevel::get_visibility_range_end);

	ClassDB::bind_method(D_METHOD("set_visibility_range_end_margin", "distance"), &FoliageLODLevel::set_visibility_range_end_margin);
	ClassDB::bind_method(D_METHOD("get_visibility_range_end_margin"), &FoliageLODLevel::get_visibility_range_end_margin);

	ClassDB::bind_method(D_METHOD("set_visibility_range_fade_mode", "mode"), &FoliageLODLevel::set_visibility_range_fade_mode);
	ClassDB::bind_method(D_METHOD("get_visibility_range_fade_mode"), &FoliageLODLevel::get_visibility_range_fade_mode);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "mesh", PROPERTY_HINT_RESOURCE_TYPE, "Mesh"), "set_mesh", "get_mesh");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material_override", PROPERTY_HINT_RESOURCE_TYPE, "BaseMaterial3D,ShaderMaterial"), "set_material_override", "get_material_override");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cast_shadows"), "set_cast_shadows", "is_casting_shadows");

	ADD_GROUP("Visibility Range", "visibility_range_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "visibility_range_begin", PROPERTY_HINT_RANGE, "0,4096,0.01,or_greater,suffix:m"), "set_visibility_range_begin", "get_visibility_range_begin");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "visibility_range_begin_margin", PROPERTY_HINT_RANGE, "0,4096,0.01,or_greater,suffix:m"), "set_visibility_range_begin_margin", "get_visibility_range_begin_margin");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "visibility_range_end", PROPERTY_HINT_RANGE, "0,4096,0.01,or_greater,suffix:m"), "set_visibility_range_end", "get_visibility_range_end");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "visibility_range_end_margin", PROPERTY_HINT_RANGE, "0,4096,0.01,or_greater,suffix:m"), "set_visibility_range_end_margin", "get_visibility_range_end_margin");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "visibility_range_fade_mode", PROPERTY_HINT_ENUM, "Disabled,Self,Dependencies"), "set_visibility_range_fade_mode", "get_visibility_range_fade_mode");
}

void FoliageLODLevel::set_mesh(const Ref<Mesh> &p_mesh) {
	mesh = p_mesh;
	emit_changed();
}

Ref<Mesh> FoliageLODLevel::get_mesh() const {
	return mesh;
}

void FoliageLODLevel::set_material_override(const Ref<Material> &p_material) {
	material_override = p_material;
	emit_changed();
}

Ref<Material> FoliageLODLevel::get_material_override() const {
	return material_override;
}

void FoliageLODLevel::set_cast_shadows(bool p_enable) {
	cast_shadows = p_enable;
	emit_changed();
}

bool FoliageLODLevel::is_casting_shadows() const {
	return cast_shadows;
}

void FoliageLODLevel::set_visibility_range_begin(float p_dist) {
	visibility_range_begin = MAX(p_dist, 0.0f);
	emit_changed();
}

float FoliageLODLevel::get_visibility_range_begin() const {
	return visibility_range_begin;
}

void FoliageLODLevel::set_visibility_range_begin_margin(float p_dist) {
	visibility_range_begin_margin = MAX(p_dist, 0.0f);
	emit_changed();
}

float FoliageLODLevel::get_visibility_range_begin_margin() const {
	return visibility_range_begin_margin;
}

void FoliageLODLevel::set_visibility_range_end(float p_dist) {
	visibility_range_end = MAX(p_dist, 0.0f);
	emit_changed();
}

float FoliageLODLevel::get_visibility_range_end() const {
	return visibility_range_end;
}

void FoliageLODLevel::set_visibility_range_end_margin(float p_dist) {
	visibility_range_end_margin = MAX(p_dist, 0.0f);
	emit_changed();
}

float FoliageLODLevel::get_visibility_range_end_margin() const {
	return visibility_range_end_margin;
}

void FoliageLODLevel::set_visibility_range_fade_mode(GeometryInstance3D::VisibilityRangeFadeMode p_mode) {
	visibility_range_fade_mode = p_mode;
	emit_changed();
}

GeometryInstance3D::VisibilityRangeFadeMode FoliageLODLevel::get_visibility_range_fade_mode() const {
	return visibility_range_fade_mode;
}

FoliageLODLevel::FoliageLODLevel() {
}
