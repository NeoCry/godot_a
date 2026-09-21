/**************************************************************************/
/*  terrain_layer.cpp                                                     */
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

#include "terrain_layer.h"

#include "core/object/class_db.h"
#include "scene/resources/texture.h"

void TerrainLayer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_layer_name", "name"), &TerrainLayer::set_layer_name);
	ClassDB::bind_method(D_METHOD("get_layer_name"), &TerrainLayer::get_layer_name);

	ClassDB::bind_method(D_METHOD("set_albedo_texture", "texture"), &TerrainLayer::set_albedo_texture);
	ClassDB::bind_method(D_METHOD("get_albedo_texture"), &TerrainLayer::get_albedo_texture);

	ClassDB::bind_method(D_METHOD("set_normal_texture", "texture"), &TerrainLayer::set_normal_texture);
	ClassDB::bind_method(D_METHOD("get_normal_texture"), &TerrainLayer::get_normal_texture);

	ClassDB::bind_method(D_METHOD("set_orm_texture", "texture"), &TerrainLayer::set_orm_texture);
	ClassDB::bind_method(D_METHOD("get_orm_texture"), &TerrainLayer::get_orm_texture);

	ClassDB::bind_method(D_METHOD("set_height_texture", "texture"), &TerrainLayer::set_height_texture);
	ClassDB::bind_method(D_METHOD("get_height_texture"), &TerrainLayer::get_height_texture);

	ClassDB::bind_method(D_METHOD("set_heightmap_scale", "scale"), &TerrainLayer::set_heightmap_scale);
	ClassDB::bind_method(D_METHOD("get_heightmap_scale"), &TerrainLayer::get_heightmap_scale);

	ClassDB::bind_method(D_METHOD("set_uv_scale", "scale"), &TerrainLayer::set_uv_scale);
	ClassDB::bind_method(D_METHOD("get_uv_scale"), &TerrainLayer::get_uv_scale);

	ClassDB::bind_method(D_METHOD("set_albedo_color", "color"), &TerrainLayer::set_albedo_color);
	ClassDB::bind_method(D_METHOD("get_albedo_color"), &TerrainLayer::get_albedo_color);

	ClassDB::bind_method(D_METHOD("set_roughness", "roughness"), &TerrainLayer::set_roughness);
	ClassDB::bind_method(D_METHOD("get_roughness"), &TerrainLayer::get_roughness);

	ClassDB::bind_method(D_METHOD("set_specular", "specular"), &TerrainLayer::set_specular);
	ClassDB::bind_method(D_METHOD("get_specular"), &TerrainLayer::get_specular);

	ClassDB::bind_method(D_METHOD("set_ao_strength", "strength"), &TerrainLayer::set_ao_strength);
	ClassDB::bind_method(D_METHOD("get_ao_strength"), &TerrainLayer::get_ao_strength);

	ClassDB::bind_method(D_METHOD("set_normal_strength", "strength"), &TerrainLayer::set_normal_strength);
	ClassDB::bind_method(D_METHOD("get_normal_strength"), &TerrainLayer::get_normal_strength);

	ClassDB::bind_method(D_METHOD("set_height_min", "height_min"), &TerrainLayer::set_height_min);
	ClassDB::bind_method(D_METHOD("get_height_min"), &TerrainLayer::get_height_min);

	ClassDB::bind_method(D_METHOD("set_height_max", "height_max"), &TerrainLayer::set_height_max);
	ClassDB::bind_method(D_METHOD("get_height_max"), &TerrainLayer::get_height_max);

	ClassDB::bind_method(D_METHOD("set_pom_enabled", "enable"), &TerrainLayer::set_pom_enabled);
	ClassDB::bind_method(D_METHOD("is_pom_enabled"), &TerrainLayer::is_pom_enabled);

	ClassDB::bind_method(D_METHOD("set_triplanar_enabled", "enable"), &TerrainLayer::set_triplanar_enabled);
	ClassDB::bind_method(D_METHOD("is_triplanar_enabled"), &TerrainLayer::is_triplanar_enabled);

	ClassDB::bind_method(D_METHOD("set_triplanar_sharpness", "sharpness"), &TerrainLayer::set_triplanar_sharpness);
	ClassDB::bind_method(D_METHOD("get_triplanar_sharpness"), &TerrainLayer::get_triplanar_sharpness);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "layer_name"), "set_layer_name", "get_layer_name");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "albedo_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_albedo_texture", "get_albedo_texture");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "albedo_color"), "set_albedo_color", "get_albedo_color");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "normal_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_normal_texture", "get_normal_texture");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "normal_strength", PROPERTY_HINT_RANGE, "-16,16,0.01"), "set_normal_strength", "get_normal_strength");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "orm_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_orm_texture", "get_orm_texture");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "roughness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_roughness", "get_roughness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "specular", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_specular", "get_specular");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ao_strength", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_ao_strength", "get_ao_strength");
	ADD_GROUP("Parallax Occlusion Mapping", "pom_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "pom_enabled"), "set_pom_enabled", "is_pom_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "height_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_height_texture", "get_height_texture");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "heightmap_scale", PROPERTY_HINT_RANGE, "-16,16,0.001"), "set_heightmap_scale", "get_heightmap_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "height_min", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_height_min", "get_height_min");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "height_max", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_height_max", "get_height_max");
	ADD_GROUP("Triplanar", "triplanar_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "triplanar_enabled"), "set_triplanar_enabled", "is_triplanar_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "triplanar_sharpness", PROPERTY_HINT_RANGE, "0,150,0.01"), "set_triplanar_sharpness", "get_triplanar_sharpness");
	ADD_GROUP("", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "uv_scale", PROPERTY_HINT_RANGE, "0.01,256.0,0.01,or_greater,suffix:m"), "set_uv_scale", "get_uv_scale");
}

void TerrainLayer::set_layer_name(const String &p_name) {
	layer_name = p_name;
	emit_changed();
}

String TerrainLayer::get_layer_name() const {
	return layer_name;
}

void TerrainLayer::set_albedo_texture(const Ref<Texture2D> &p_texture) {
	albedo_texture = p_texture;
	emit_changed();
}

Ref<Texture2D> TerrainLayer::get_albedo_texture() const {
	return albedo_texture;
}

void TerrainLayer::set_normal_texture(const Ref<Texture2D> &p_texture) {
	normal_texture = p_texture;
	emit_changed();
}

Ref<Texture2D> TerrainLayer::get_normal_texture() const {
	return normal_texture;
}

void TerrainLayer::set_orm_texture(const Ref<Texture2D> &p_texture) {
	orm_texture = p_texture;
	emit_changed();
}

Ref<Texture2D> TerrainLayer::get_orm_texture() const {
	return orm_texture;
}

void TerrainLayer::set_uv_scale(float p_scale) {
	uv_scale = MAX(p_scale, 0.01f);
	emit_changed();
}

float TerrainLayer::get_uv_scale() const {
	return uv_scale;
}

void TerrainLayer::set_albedo_color(const Color &p_color) {
	albedo_color = p_color;
	emit_changed();
}

Color TerrainLayer::get_albedo_color() const {
	return albedo_color;
}

void TerrainLayer::set_roughness(float p_roughness) {
	roughness = CLAMP(p_roughness, 0.0f, 1.0f);
	emit_changed();
}

float TerrainLayer::get_roughness() const {
	return roughness;
}

void TerrainLayer::set_specular(float p_specular) {
	specular = CLAMP(p_specular, 0.0f, 1.0f);
	emit_changed();
}

float TerrainLayer::get_specular() const {
	return specular;
}

void TerrainLayer::set_ao_strength(float p_strength) {
	ao_strength = CLAMP(p_strength, 0.0f, 1.0f);
	emit_changed();
}

float TerrainLayer::get_ao_strength() const {
	return ao_strength;
}

void TerrainLayer::set_height_texture(const Ref<Texture2D> &p_texture) {
	height_texture = p_texture;
	emit_changed();
}

Ref<Texture2D> TerrainLayer::get_height_texture() const {
	return height_texture;
}

void TerrainLayer::set_heightmap_scale(float p_scale) {
	heightmap_scale = p_scale;
	emit_changed();
}

float TerrainLayer::get_heightmap_scale() const {
	return heightmap_scale;
}

void TerrainLayer::set_normal_strength(float p_strength) {
	normal_strength = p_strength;
	emit_changed();
}

float TerrainLayer::get_normal_strength() const {
	return normal_strength;
}

void TerrainLayer::set_height_min(float p_min) {
	height_min = CLAMP(p_min, 0.0f, 1.0f);
	emit_changed();
}

float TerrainLayer::get_height_min() const {
	return height_min;
}

void TerrainLayer::set_height_max(float p_max) {
	height_max = CLAMP(p_max, 0.0f, 1.0f);
	emit_changed();
}

float TerrainLayer::get_height_max() const {
	return height_max;
}

void TerrainLayer::set_pom_enabled(bool p_enable) {
	pom_enabled = p_enable;
	emit_changed();
}

bool TerrainLayer::is_pom_enabled() const {
	return pom_enabled;
}

void TerrainLayer::set_triplanar_enabled(bool p_enable) {
	triplanar_enabled = p_enable;
	emit_changed();
}

bool TerrainLayer::is_triplanar_enabled() const {
	return triplanar_enabled;
}

void TerrainLayer::set_triplanar_sharpness(float p_sharpness) {
	// Negative or very high values can produce NaNs (same reasoning as
	// BaseMaterial3D.uv1_triplanar_sharpness's own clamp).
	triplanar_sharpness = CLAMP(p_sharpness, 0.0f, 150.0f);
	emit_changed();
}

float TerrainLayer::get_triplanar_sharpness() const {
	return triplanar_sharpness;
}

TerrainLayer::TerrainLayer() {
}
