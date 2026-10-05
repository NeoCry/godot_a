/**************************************************************************/
/*  planar_reflection_probe.cpp                                           */
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

#include "planar_reflection_probe.h"

#include "core/object/class_db.h"
#include "core/os/os.h"

void PlanarReflectionProbe::set_size(const Vector2 &p_size) {
	size = p_size.maxf(0.0);
	RS::get_singleton()->planar_reflection_set_size(get_base(), size);
	update_gizmos();
}

Vector2 PlanarReflectionProbe::get_size() const {
	return size;
}

void PlanarReflectionProbe::set_receive_distance(float p_distance) {
	receive_distance = MAX(p_distance, 0.001f);
	RS::get_singleton()->planar_reflection_set_receive_distance(get_base(), receive_distance);
	update_gizmos();
}

float PlanarReflectionProbe::get_receive_distance() const {
	return receive_distance;
}

void PlanarReflectionProbe::set_resolution_scale(float p_scale) {
	resolution_scale = CLAMP(p_scale, 0.05f, 1.0f);
	RS::get_singleton()->planar_reflection_set_resolution_scale(get_base(), resolution_scale);
}

float PlanarReflectionProbe::get_resolution_scale() const {
	return resolution_scale;
}

void PlanarReflectionProbe::set_max_distance(float p_distance) {
	max_distance = MAX(p_distance, 0.0f);
	RS::get_singleton()->planar_reflection_set_max_distance(get_base(), max_distance);
}

float PlanarReflectionProbe::get_max_distance() const {
	return max_distance;
}

void PlanarReflectionProbe::set_intensity(float p_intensity) {
	intensity = MAX(p_intensity, 0.0f);
	RS::get_singleton()->planar_reflection_set_intensity(get_base(), intensity);
}

float PlanarReflectionProbe::get_intensity() const {
	return intensity;
}

void PlanarReflectionProbe::set_distortion(float p_distortion) {
	distortion = MAX(p_distortion, 0.0f);
	RS::get_singleton()->planar_reflection_set_distortion(get_base(), distortion);
}

float PlanarReflectionProbe::get_distortion() const {
	return distortion;
}

void PlanarReflectionProbe::set_receive_angle(float p_degrees) {
	receive_angle = CLAMP(p_degrees, 0.0f, 90.0f);
	RS::get_singleton()->planar_reflection_set_normal_fade(get_base(), Math::cos(Math::deg_to_rad(receive_angle)));
}

float PlanarReflectionProbe::get_receive_angle() const {
	return receive_angle;
}

void PlanarReflectionProbe::set_edge_fade(float p_fade) {
	edge_fade = CLAMP(p_fade, 0.0f, 1.0f);
	RS::get_singleton()->planar_reflection_set_edge_fade(get_base(), edge_fade);
}

float PlanarReflectionProbe::get_edge_fade() const {
	return edge_fade;
}

void PlanarReflectionProbe::set_clip_bias(float p_bias) {
	clip_bias = p_bias;
	RS::get_singleton()->planar_reflection_set_clip_bias(get_base(), clip_bias);
}

float PlanarReflectionProbe::get_clip_bias() const {
	return clip_bias;
}

void PlanarReflectionProbe::set_enable_shadows(bool p_enable) {
	enable_shadows = p_enable;
	RS::get_singleton()->planar_reflection_set_enable_shadows(get_base(), enable_shadows);
}

bool PlanarReflectionProbe::are_shadows_enabled() const {
	return enable_shadows;
}

void PlanarReflectionProbe::set_mesh_lod_threshold(float p_pixels) {
	mesh_lod_threshold = MAX(p_pixels, 0.0f);
	RS::get_singleton()->planar_reflection_set_mesh_lod_threshold(get_base(), mesh_lod_threshold);
}

float PlanarReflectionProbe::get_mesh_lod_threshold() const {
	return mesh_lod_threshold;
}

void PlanarReflectionProbe::set_cull_mask(uint32_t p_layers) {
	cull_mask = p_layers;
	RS::get_singleton()->planar_reflection_set_cull_mask(get_base(), cull_mask);
}

uint32_t PlanarReflectionProbe::get_cull_mask() const {
	return cull_mask;
}

void PlanarReflectionProbe::set_reflection_mask(uint32_t p_layers) {
	reflection_mask = p_layers;
	RS::get_singleton()->planar_reflection_set_reflection_mask(get_base(), reflection_mask);
}

uint32_t PlanarReflectionProbe::get_reflection_mask() const {
	return reflection_mask;
}

AABB PlanarReflectionProbe::get_aabb() const {
	return AABB(Vector3(-size.x * 0.5f, -receive_distance, -size.y * 0.5f), Vector3(size.x, receive_distance * 2.0f, size.y));
}

PackedStringArray PlanarReflectionProbe::get_configuration_warnings() const {
	PackedStringArray warnings = VisualInstance3D::get_configuration_warnings();
	const String rendering_method = OS::get_singleton()->get_current_rendering_method();
	if (rendering_method == "gl_compatibility" || rendering_method == "dummy") {
		warnings.push_back(RTR("PlanarReflectionProbe is only supported by the Forward+ and Mobile renderers. Surfaces here reflect the sky and ReflectionProbes instead."));
	}
	return warnings;
}

void PlanarReflectionProbe::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_size", "size"), &PlanarReflectionProbe::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &PlanarReflectionProbe::get_size);

	ClassDB::bind_method(D_METHOD("set_receive_distance", "distance"), &PlanarReflectionProbe::set_receive_distance);
	ClassDB::bind_method(D_METHOD("get_receive_distance"), &PlanarReflectionProbe::get_receive_distance);

	ClassDB::bind_method(D_METHOD("set_resolution_scale", "scale"), &PlanarReflectionProbe::set_resolution_scale);
	ClassDB::bind_method(D_METHOD("get_resolution_scale"), &PlanarReflectionProbe::get_resolution_scale);

	ClassDB::bind_method(D_METHOD("set_max_distance", "distance"), &PlanarReflectionProbe::set_max_distance);
	ClassDB::bind_method(D_METHOD("get_max_distance"), &PlanarReflectionProbe::get_max_distance);

	ClassDB::bind_method(D_METHOD("set_intensity", "intensity"), &PlanarReflectionProbe::set_intensity);
	ClassDB::bind_method(D_METHOD("get_intensity"), &PlanarReflectionProbe::get_intensity);

	ClassDB::bind_method(D_METHOD("set_distortion", "distortion"), &PlanarReflectionProbe::set_distortion);
	ClassDB::bind_method(D_METHOD("get_distortion"), &PlanarReflectionProbe::get_distortion);

	ClassDB::bind_method(D_METHOD("set_receive_angle", "degrees"), &PlanarReflectionProbe::set_receive_angle);
	ClassDB::bind_method(D_METHOD("get_receive_angle"), &PlanarReflectionProbe::get_receive_angle);

	ClassDB::bind_method(D_METHOD("set_edge_fade", "fade"), &PlanarReflectionProbe::set_edge_fade);
	ClassDB::bind_method(D_METHOD("get_edge_fade"), &PlanarReflectionProbe::get_edge_fade);

	ClassDB::bind_method(D_METHOD("set_clip_bias", "bias"), &PlanarReflectionProbe::set_clip_bias);
	ClassDB::bind_method(D_METHOD("get_clip_bias"), &PlanarReflectionProbe::get_clip_bias);

	ClassDB::bind_method(D_METHOD("set_enable_shadows", "enable"), &PlanarReflectionProbe::set_enable_shadows);
	ClassDB::bind_method(D_METHOD("are_shadows_enabled"), &PlanarReflectionProbe::are_shadows_enabled);

	ClassDB::bind_method(D_METHOD("set_mesh_lod_threshold", "pixels"), &PlanarReflectionProbe::set_mesh_lod_threshold);
	ClassDB::bind_method(D_METHOD("get_mesh_lod_threshold"), &PlanarReflectionProbe::get_mesh_lod_threshold);

	ClassDB::bind_method(D_METHOD("set_cull_mask", "layers"), &PlanarReflectionProbe::set_cull_mask);
	ClassDB::bind_method(D_METHOD("get_cull_mask"), &PlanarReflectionProbe::get_cull_mask);

	ClassDB::bind_method(D_METHOD("set_reflection_mask", "layers"), &PlanarReflectionProbe::set_reflection_mask);
	ClassDB::bind_method(D_METHOD("get_reflection_mask"), &PlanarReflectionProbe::get_reflection_mask);

	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "size", PROPERTY_HINT_NONE, "suffix:m"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "receive_distance", PROPERTY_HINT_RANGE, "0.001,100,0.001,or_greater,suffix:m"), "set_receive_distance", "get_receive_distance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "receive_angle", PROPERTY_HINT_RANGE, "0,90,0.1,degrees"), "set_receive_angle", "get_receive_angle");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "intensity", PROPERTY_HINT_RANGE, "0,1,0.01,or_greater"), "set_intensity", "get_intensity");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "distortion", PROPERTY_HINT_RANGE, "0,2,0.01,or_greater"), "set_distortion", "get_distortion");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "edge_fade", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_edge_fade", "get_edge_fade");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "reflection_mask", PROPERTY_HINT_LAYERS_3D_RENDER), "set_reflection_mask", "get_reflection_mask");

	ADD_GROUP("Rendering", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "resolution_scale", PROPERTY_HINT_RANGE, "0.05,1,0.01"), "set_resolution_scale", "get_resolution_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_distance", PROPERTY_HINT_RANGE, "0,16384,0.1,or_greater,exp,suffix:m"), "set_max_distance", "get_max_distance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "clip_bias", PROPERTY_HINT_RANGE, "-1,1,0.001,or_less,or_greater,suffix:m"), "set_clip_bias", "get_clip_bias");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cull_mask", PROPERTY_HINT_LAYERS_3D_RENDER), "set_cull_mask", "get_cull_mask");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enable_shadows"), "set_enable_shadows", "are_shadows_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mesh_lod_threshold", PROPERTY_HINT_RANGE, "0,1024,0.1"), "set_mesh_lod_threshold", "get_mesh_lod_threshold");
}

PlanarReflectionProbe::PlanarReflectionProbe() {
	RID planar_reflection = RS::get_singleton()->planar_reflection_create();
	set_base(planar_reflection);
	RS::get_singleton()->planar_reflection_set_size(planar_reflection, size);
	RS::get_singleton()->planar_reflection_set_receive_distance(planar_reflection, receive_distance);
	RS::get_singleton()->planar_reflection_set_resolution_scale(planar_reflection, resolution_scale);
	RS::get_singleton()->planar_reflection_set_max_distance(planar_reflection, max_distance);
	RS::get_singleton()->planar_reflection_set_intensity(planar_reflection, intensity);
	RS::get_singleton()->planar_reflection_set_distortion(planar_reflection, distortion);
	RS::get_singleton()->planar_reflection_set_normal_fade(planar_reflection, Math::cos(Math::deg_to_rad(receive_angle)));
	RS::get_singleton()->planar_reflection_set_edge_fade(planar_reflection, edge_fade);
	RS::get_singleton()->planar_reflection_set_clip_bias(planar_reflection, clip_bias);
	RS::get_singleton()->planar_reflection_set_enable_shadows(planar_reflection, enable_shadows);
	RS::get_singleton()->planar_reflection_set_mesh_lod_threshold(planar_reflection, mesh_lod_threshold);
	RS::get_singleton()->planar_reflection_set_cull_mask(planar_reflection, cull_mask);
	RS::get_singleton()->planar_reflection_set_reflection_mask(planar_reflection, reflection_mask);
	set_disable_scale(true);
}

PlanarReflectionProbe::~PlanarReflectionProbe() {
	ERR_FAIL_NULL(RenderingServer::get_singleton());
	RS::get_singleton()->free_rid(get_base());
}
