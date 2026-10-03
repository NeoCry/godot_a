/**************************************************************************/
/*  blend_landscape_3d.cpp                                                */
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

#include "blend_landscape_3d.h"

#include "core/object/class_db.h"
#include "scene/resources/texture.h"

Mutex BlendLandscape3D::landscapes_mutex;
HashMap<ObjectID, BlendLandscape3D::LandscapeSource> BlendLandscape3D::landscapes;
SelfList<BlendLandscape3D>::List BlendLandscape3D::blend_materials;

/* LANDSCAPES */

void BlendLandscape3D::set_landscape_source(ObjectID p_landscape, const LandscapeSource &p_source) {
	MutexLock lock(landscapes_mutex);
	const LandscapeSource *current = landscapes.getptr(p_landscape);
	if (current && current->texture == p_source.texture && current->volume.is_equal_approx(p_source.volume) && current->layers == p_source.layers) {
		// Sculpting calls this on every stroke; the materials only need to hear of actual changes.
		return;
	}
	landscapes[p_landscape] = p_source;
	for (SelfList<BlendLandscape3D> *E = blend_materials.first(); E; E = E->next()) {
		E->self()->_bind_landscape_locked();
	}
}

void BlendLandscape3D::remove_landscape_source(ObjectID p_landscape) {
	MutexLock lock(landscapes_mutex);
	if (!landscapes.erase(p_landscape)) {
		return;
	}
	for (SelfList<BlendLandscape3D> *E = blend_materials.first(); E; E = E->next()) {
		E->self()->_bind_landscape_locked();
	}
}

void BlendLandscape3D::_bind_landscape_locked() {
	// The first landscape to appear among those drawing into the layers asked for.
	const LandscapeSource *source = nullptr;
	ObjectID landscape;
	for (const KeyValue<ObjectID, LandscapeSource> &E : landscapes) {
		if ((E.value.layers & blend_landscape_layers) && E.value.texture.is_valid()) {
			source = &E.value;
			landscape = E.key;
			break;
		}
	}
	bound_landscape = landscape;

	if (!source) {
		_set_shader_parameter(SNAME("landscape_blend_active"), false);
		_set_shader_parameter(SNAME("landscape_rvt_albedo"), Variant());
		_set_shader_parameter(SNAME("landscape_rvt_data"), Variant());
		return;
	}

	// Pages are drawn looking down the volume's Y axis, with X to the right: their normals are in the
	// space of that camera (see VirtualTextureStorage::_page_camera()).
	const Basis &basis = source->volume.basis;
	const Vector3 page_x = basis.get_column(0).normalized();
	const Vector3 page_z = basis.get_column(1).normalized();
	const Vector3 page_y = page_z.cross(page_x).normalized();

	_set_shader_parameter(SNAME("landscape_rvt_albedo"), source->texture);
	_set_shader_parameter(SNAME("landscape_rvt_data"), source->texture);
	_set_shader_parameter(SNAME("landscape_world_to_volume"), source->volume.affine_inverse());
	_set_shader_parameter(SNAME("landscape_page_to_world"), Basis(page_x, page_y, page_z));
	_set_shader_parameter(SNAME("landscape_volume_height"), basis.get_column(1).length());
	_set_shader_parameter(SNAME("landscape_blend_active"), true);
}

void BlendLandscape3D::_bind_landscape() {
	MutexLock lock(landscapes_mutex);
	_bind_landscape_locked();
}

ObjectID BlendLandscape3D::get_bound_landscape() const {
	MutexLock lock(landscapes_mutex);
	return bound_landscape;
}

/* SHADER */

uint32_t BlendLandscape3D::_get_shader_extension_flags() const {
	uint32_t extension = EXTENSION_LANDSCAPE_BLEND;
	if (blend_noise_texture.is_valid()) {
		extension |= EXTENSION_NOISE;
	}
	if (get_feature(FEATURE_NORMAL_MAPPING)) {
		extension |= EXTENSION_NORMAL_MAP;
	}
	return extension;
}

String BlendLandscape3D::_get_shader_extension_uniforms(uint32_t p_flags) const {
	String code = R"(
// Landscape blend (see BlendLandscape3D): the landscape's runtime virtual texture, its albedo read as
// color and the rest as data, and where it was drawn from.
uniform sampler2DArray landscape_rvt_albedo : source_color, hint_virtual_texture, repeat_disable;
uniform sampler2DArray landscape_rvt_data : hint_virtual_texture, repeat_disable;
uniform bool landscape_blend_active;
uniform mat4 landscape_world_to_volume;
uniform mat3 landscape_page_to_world;
uniform float landscape_volume_height;
uniform float landscape_blend_height;
uniform float landscape_blend_offset;
uniform float landscape_blend_falloff;
uniform float landscape_blend_normal_strength;
uniform float landscape_blend_slope;
)";
	if (p_flags & EXTENSION_NOISE) {
		code += R"(uniform sampler2D landscape_blend_noise : hint_default_white, filter_linear_mipmap, repeat_enable;
uniform float landscape_blend_noise_scale;
uniform float landscape_blend_noise_strength;
)";
	}
	return code;
}

String BlendLandscape3D::_get_shader_extension_fragment(uint32_t p_flags) const {
	String code = R"(
	// Landscape blend: near the ground of the landscape, the surface takes on the ground's.
	if (landscape_blend_active) {
		vec3 lb_world = (INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xyz;
		vec3 lb_volume = (landscape_world_to_volume * vec4(lb_world, 1.0)).xyz;
		vec2 lb_uv = lb_volume.xz;
		vec4 lb_albedo = texture(landscape_rvt_albedo, vec3(lb_uv, 0.0));
		vec4 lb_normal_specular = texture(landscape_rvt_data, vec3(lb_uv, 1.0));
		vec3 lb_orm = texture(landscape_rvt_data, vec3(lb_uv, 2.0)).rgb;
		// From 0 at the bottom of the volume to 1 at its top; 0 where there is no ground.
		float lb_ground = texture(landscape_rvt_data, vec3(lb_uv, 3.0)).r;

		// How far over the ground this is, in meters.
		float lb_above = (lb_volume.y - lb_ground) * landscape_volume_height - landscape_blend_offset;
)";
	if (p_flags & EXTENSION_NOISE) {
		code += R"(		lb_above += (texture(landscape_blend_noise, lb_world.xz / landscape_blend_noise_scale).r - 0.5) * landscape_blend_noise_strength * landscape_blend_height;
)";
	}
	code += R"(		float lb_top = max(landscape_blend_height, 0.0001);
		float lb_amount = 1.0 - smoothstep(lb_top * (1.0 - max(landscape_blend_falloff, 0.001)), lb_top, lb_above);
		// Nothing past the landscape's edges, nor where it has no ground (or none drawn yet).
		lb_amount *= step(0.0, lb_uv.x) * step(lb_uv.x, 1.0) * step(0.0, lb_uv.y) * step(lb_uv.y, 1.0) * step(0.000001, lb_ground);

		// Pages' normals are in the space of the camera that drew them, looking down the volume.
		vec3 lb_ground_normal = normalize(mat3(VIEW_MATRIX) * (landscape_page_to_world * normalize(lb_normal_specular.rgb * 2.0 - 1.0)));
		vec3 lb_up = normalize(mat3(VIEW_MATRIX) * landscape_page_to_world[2]);
		lb_amount *= mix(1.0, smoothstep(0.0, 0.7, dot(NORMAL, lb_up)), landscape_blend_slope);

		ALBEDO = mix(ALBEDO, lb_albedo.rgb, lb_amount);
		ROUGHNESS = mix(ROUGHNESS, lb_orm.g, lb_amount);
		METALLIC = mix(METALLIC, lb_orm.b, lb_amount);
		AO = mix(AO, lb_orm.r, lb_amount);
		SPECULAR = mix(SPECULAR, lb_normal_specular.a, lb_amount);
		NORMAL = normalize(mix(NORMAL, lb_ground_normal, lb_amount * landscape_blend_normal_strength));
)";
	if (p_flags & EXTENSION_NORMAL_MAP) {
		code += R"(		// The material's own normal map fades out where the ground's normal takes over.
		NORMAL_MAP_DEPTH *= 1.0 - lb_amount * landscape_blend_normal_strength;
)";
	}
	code += "	}\n";
	return code;
}

/* PROPERTIES */

void BlendLandscape3D::set_blend_landscape_layers(uint32_t p_layers) {
	{
		MutexLock lock(landscapes_mutex);
		blend_landscape_layers = p_layers;
	}
	_bind_landscape();
}

uint32_t BlendLandscape3D::get_blend_landscape_layers() const {
	return blend_landscape_layers;
}

void BlendLandscape3D::set_blend_height(float p_height) {
	blend_height = MAX(p_height, 0.0f);
	_set_shader_parameter(SNAME("landscape_blend_height"), blend_height);
}

float BlendLandscape3D::get_blend_height() const {
	return blend_height;
}

void BlendLandscape3D::set_blend_offset(float p_offset) {
	blend_offset = p_offset;
	_set_shader_parameter(SNAME("landscape_blend_offset"), blend_offset);
}

float BlendLandscape3D::get_blend_offset() const {
	return blend_offset;
}

void BlendLandscape3D::set_blend_falloff(float p_falloff) {
	blend_falloff = CLAMP(p_falloff, 0.0f, 1.0f);
	_set_shader_parameter(SNAME("landscape_blend_falloff"), blend_falloff);
}

float BlendLandscape3D::get_blend_falloff() const {
	return blend_falloff;
}

void BlendLandscape3D::set_blend_normal_strength(float p_strength) {
	blend_normal_strength = CLAMP(p_strength, 0.0f, 1.0f);
	_set_shader_parameter(SNAME("landscape_blend_normal_strength"), blend_normal_strength);
}

float BlendLandscape3D::get_blend_normal_strength() const {
	return blend_normal_strength;
}

void BlendLandscape3D::set_blend_slope(float p_slope) {
	blend_slope = CLAMP(p_slope, 0.0f, 1.0f);
	_set_shader_parameter(SNAME("landscape_blend_slope"), blend_slope);
}

float BlendLandscape3D::get_blend_slope() const {
	return blend_slope;
}

void BlendLandscape3D::set_blend_noise_texture(const Ref<Texture2D> &p_texture) {
	const bool had_noise = blend_noise_texture.is_valid();
	blend_noise_texture = p_texture;
	_set_shader_parameter(SNAME("landscape_blend_noise"), p_texture.is_valid() ? Variant(p_texture->get_rid()) : Variant());
	if (had_noise != p_texture.is_valid()) {
		_shader_extension_changed();
	}
}

Ref<Texture2D> BlendLandscape3D::get_blend_noise_texture() const {
	return blend_noise_texture;
}

void BlendLandscape3D::set_blend_noise_scale(float p_scale) {
	blend_noise_scale = MAX(p_scale, 0.001f);
	_set_shader_parameter(SNAME("landscape_blend_noise_scale"), blend_noise_scale);
}

float BlendLandscape3D::get_blend_noise_scale() const {
	return blend_noise_scale;
}

void BlendLandscape3D::set_blend_noise_strength(float p_strength) {
	blend_noise_strength = MAX(p_strength, 0.0f);
	_set_shader_parameter(SNAME("landscape_blend_noise_strength"), blend_noise_strength);
}

float BlendLandscape3D::get_blend_noise_strength() const {
	return blend_noise_strength;
}

void BlendLandscape3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_blend_landscape_layers", "layers"), &BlendLandscape3D::set_blend_landscape_layers);
	ClassDB::bind_method(D_METHOD("get_blend_landscape_layers"), &BlendLandscape3D::get_blend_landscape_layers);
	ClassDB::bind_method(D_METHOD("set_blend_height", "height"), &BlendLandscape3D::set_blend_height);
	ClassDB::bind_method(D_METHOD("get_blend_height"), &BlendLandscape3D::get_blend_height);
	ClassDB::bind_method(D_METHOD("set_blend_offset", "offset"), &BlendLandscape3D::set_blend_offset);
	ClassDB::bind_method(D_METHOD("get_blend_offset"), &BlendLandscape3D::get_blend_offset);
	ClassDB::bind_method(D_METHOD("set_blend_falloff", "falloff"), &BlendLandscape3D::set_blend_falloff);
	ClassDB::bind_method(D_METHOD("get_blend_falloff"), &BlendLandscape3D::get_blend_falloff);
	ClassDB::bind_method(D_METHOD("set_blend_normal_strength", "strength"), &BlendLandscape3D::set_blend_normal_strength);
	ClassDB::bind_method(D_METHOD("get_blend_normal_strength"), &BlendLandscape3D::get_blend_normal_strength);
	ClassDB::bind_method(D_METHOD("set_blend_slope", "slope"), &BlendLandscape3D::set_blend_slope);
	ClassDB::bind_method(D_METHOD("get_blend_slope"), &BlendLandscape3D::get_blend_slope);
	ClassDB::bind_method(D_METHOD("set_blend_noise_texture", "texture"), &BlendLandscape3D::set_blend_noise_texture);
	ClassDB::bind_method(D_METHOD("get_blend_noise_texture"), &BlendLandscape3D::get_blend_noise_texture);
	ClassDB::bind_method(D_METHOD("set_blend_noise_scale", "scale"), &BlendLandscape3D::set_blend_noise_scale);
	ClassDB::bind_method(D_METHOD("get_blend_noise_scale"), &BlendLandscape3D::get_blend_noise_scale);
	ClassDB::bind_method(D_METHOD("set_blend_noise_strength", "strength"), &BlendLandscape3D::set_blend_noise_strength);
	ClassDB::bind_method(D_METHOD("get_blend_noise_strength"), &BlendLandscape3D::get_blend_noise_strength);

	ADD_GROUP("Landscape Blend", "blend_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "blend_landscape_layers", PROPERTY_HINT_LAYERS_3D_RENDER), "set_blend_landscape_layers", "get_blend_landscape_layers");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "blend_height", PROPERTY_HINT_RANGE, "0,4,0.01,or_greater,suffix:m"), "set_blend_height", "get_blend_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "blend_offset", PROPERTY_HINT_RANGE, "-2,2,0.01,or_less,or_greater,suffix:m"), "set_blend_offset", "get_blend_offset");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "blend_falloff", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_blend_falloff", "get_blend_falloff");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "blend_normal_strength", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_blend_normal_strength", "get_blend_normal_strength");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "blend_slope", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_blend_slope", "get_blend_slope");
	ADD_SUBGROUP("Noise", "blend_noise_");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "blend_noise_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_blend_noise_texture", "get_blend_noise_texture");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "blend_noise_scale", PROPERTY_HINT_RANGE, "0.01,64,0.01,or_greater,suffix:m"), "set_blend_noise_scale", "get_blend_noise_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "blend_noise_strength", PROPERTY_HINT_RANGE, "0,4,0.01,or_greater"), "set_blend_noise_strength", "get_blend_noise_strength");
}

BlendLandscape3D::BlendLandscape3D() :
		landscape_element(this) {
	set_blend_height(blend_height);
	set_blend_offset(blend_offset);
	set_blend_falloff(blend_falloff);
	set_blend_normal_strength(blend_normal_strength);
	set_blend_slope(blend_slope);
	set_blend_noise_scale(blend_noise_scale);
	set_blend_noise_strength(blend_noise_strength);

	MutexLock lock(landscapes_mutex);
	blend_materials.add(&landscape_element);
	_bind_landscape_locked();
}

BlendLandscape3D::~BlendLandscape3D() {
	MutexLock lock(landscapes_mutex);
	blend_materials.remove(&landscape_element);
}
