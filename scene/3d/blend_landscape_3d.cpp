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
#include "scene/3d/terrain_data.h"
#include "scene/resources/texture.h"

Mutex BlendLandscape3D::landscapes_mutex;
HashMap<ObjectID, BlendLandscape3D::LandscapeSource> BlendLandscape3D::landscapes;
SelfList<BlendLandscape3D>::List BlendLandscape3D::blend_materials;

/* LANDSCAPES */

bool BlendLandscape3D::LandscapeSource::operator==(const LandscapeSource &p_other) const {
	if (layers != p_other.layers || parameters.size() != p_other.parameters.size()) {
		return false;
	}
	for (const KeyValue<StringName, Variant> &E : parameters) {
		const Variant *other = p_other.parameters.getptr(E.key);
		if (!other || *other != E.value) {
			return false;
		}
	}
	return true;
}

void BlendLandscape3D::set_landscape_source(ObjectID p_landscape, const LandscapeSource &p_source) {
	MutexLock lock(landscapes_mutex);
	const LandscapeSource *current = landscapes.getptr(p_landscape);
	if (current && *current == p_source) {
		// Sculpting and painting call this all the time; the materials only need to hear of changes.
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
		if (E.value.layers & blend_landscape_layers) {
			source = &E.value;
			landscape = E.key;
			break;
		}
	}
	bound_landscape = landscape;

	if (!source) {
		_set_shader_parameter(SNAME("landscape_blend_active"), false);
		// Nothing keeps reading textures that may be about to go.
		for (const StringName &texture : bound_textures) {
			_set_shader_parameter(texture, Variant());
		}
		bound_textures.clear();
		return;
	}

	bound_textures.clear();
	for (const KeyValue<StringName, Variant> &E : source->parameters) {
		_set_shader_parameter(E.key, E.value);
		if (E.value.get_type() == Variant::RID) {
			bound_textures.push_back(E.key);
		}
	}
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
	// The ground is found the way Landscape3D's own shader finds it (see Landscape3D::init_shaders()),
	// from the same textures and per-layer settings, but for parallax occlusion mapping, which has
	// nothing to offset on a surface that is not the ground.
	String code = R"(
// Landscape blend (see BlendLandscape3D): the landscape blended into, as it hands itself over.
uniform bool landscape_blend_active;
uniform mat4 landscape_world_to_local;
uniform mat3 landscape_normal_to_world;
uniform float landscape_height_scale;
uniform int landscape_terrain_quads;
uniform float landscape_vertex_spacing;
uniform sampler2D landscape_heightmap : filter_nearest, repeat_disable;
uniform sampler2D landscape_gradient_map : filter_linear_mipmap_anisotropic, repeat_disable;
uniform sampler2DArray landscape_weight_array : filter_linear, repeat_disable;
uniform sampler2DArray landscape_albedo_array : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2DArray landscape_normal_array : hint_normal, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2DArray landscape_orm_array : filter_linear_mipmap_anisotropic, repeat_enable;
uniform float landscape_layer_uv_scales[MAX_LAYERS];
uniform vec4 landscape_layer_albedo_colors[MAX_LAYERS];
uniform float landscape_layer_roughness[MAX_LAYERS];
uniform float landscape_layer_specular[MAX_LAYERS];
uniform float landscape_layer_ao_strength[MAX_LAYERS];
uniform float landscape_layer_normal_strength[MAX_LAYERS];
uniform float landscape_layer_triplanar[MAX_LAYERS];
uniform float landscape_layer_triplanar_sharpness[MAX_LAYERS];
uniform int landscape_layer_count;
// Further than this (and than parallax reaches), the landscape reads its ground back from its runtime
// virtual texture, its albedo as color and the rest as data.
uniform bool landscape_rvt_active;
uniform sampler2DArray landscape_rvt_albedo : source_color, hint_virtual_texture, repeat_disable;
uniform sampler2DArray landscape_rvt_data : hint_virtual_texture, repeat_disable;
uniform float landscape_ground_near;
uniform bool landscape_ground_pom;
uniform vec2 landscape_ground_pom_fade;

uniform float landscape_blend_height;
uniform float landscape_blend_offset;
uniform float landscape_blend_falloff;
uniform float landscape_blend_normal_strength;
uniform float landscape_blend_slope;
)";
	code = code.replace("MAX_LAYERS", itos(TerrainData::MAX_LAYERS));
	if (p_flags & EXTENSION_NOISE) {
		code += R"(uniform sampler2D landscape_blend_noise : hint_default_white, filter_linear_mipmap, repeat_enable;
uniform float landscape_blend_noise_scale;
uniform float landscape_blend_noise_strength;
)";
	}
	code += R"(
float lb_fetch_height(ivec2 p_sample) {
	return texelFetch(landscape_heightmap, clamp(p_sample, ivec2(0), ivec2(landscape_terrain_quads)), 0).r;
}

// The height of the ground between the landscape's samples, in its own space.
float lb_ground_height(vec2 p_sample) {
	vec2 lb_cell = floor(p_sample);
	vec2 lb_f = p_sample - lb_cell;
	ivec2 lb_base = ivec2(lb_cell);
	return mix(mix(lb_fetch_height(lb_base), lb_fetch_height(lb_base + ivec2(1, 0)), lb_f.x), mix(lb_fetch_height(lb_base + ivec2(0, 1)), lb_fetch_height(lb_base + ivec2(1, 1)), lb_f.x), lb_f.y);
}

vec3 lb_triplanar_weights(vec3 p_normal, float p_sharpness) {
	vec3 lb_w = pow(abs(p_normal), vec3(p_sharpness));
	return lb_w / max(dot(lb_w, vec3(1.0)), 0.00001);
}

vec4 lb_sample_triplanar(sampler2DArray p_array, int p_layer, vec3 p_position, vec3 p_weights) {
	vec4 lb_samp = vec4(0.0);
	lb_samp += texture(p_array, vec3(p_position.xy, float(p_layer))) * p_weights.z;
	lb_samp += texture(p_array, vec3(p_position.xz, float(p_layer))) * p_weights.y;
	lb_samp += texture(p_array, vec3(p_position.zy * vec2(-1.0, 1.0), float(p_layer))) * p_weights.x;
	return lb_samp;
}

void lb_accumulate_layer(int p_layer, float p_weight, vec3 p_position, vec3 p_normal, inout vec3 r_albedo, inout vec3 r_normal, inout vec3 r_orm, inout float r_specular, inout float r_normal_strength, inout float r_weight) {
	if (p_weight <= 0.001) {
		return;
	}
	vec3 lb_layer_albedo;
	vec3 lb_normal_tex;
	vec3 lb_layer_orm;
	if (landscape_layer_triplanar[p_layer] > 0.5) {
		vec3 lb_position = (p_position * vec3(1.0, -1.0, 1.0)) / landscape_layer_uv_scales[p_layer];
		vec3 lb_weights = lb_triplanar_weights(p_normal, landscape_layer_triplanar_sharpness[p_layer]);
		lb_layer_albedo = lb_sample_triplanar(landscape_albedo_array, p_layer, lb_position, lb_weights).rgb;
		lb_normal_tex = lb_sample_triplanar(landscape_normal_array, p_layer, lb_position, lb_weights).rgb;
		lb_layer_orm = lb_sample_triplanar(landscape_orm_array, p_layer, lb_position, lb_weights).rgb;
	} else {
		vec3 lb_uv = vec3(p_position.xz / landscape_layer_uv_scales[p_layer], float(p_layer));
		lb_layer_albedo = texture(landscape_albedo_array, lb_uv).rgb;
		lb_normal_tex = texture(landscape_normal_array, lb_uv).rgb;
		lb_layer_orm = texture(landscape_orm_array, lb_uv).rgb;
	}
	lb_layer_albedo *= landscape_layer_albedo_colors[p_layer].rgb;
	lb_layer_orm.r = mix(1.0, lb_layer_orm.r, landscape_layer_ao_strength[p_layer]);
	lb_layer_orm.g = clamp(lb_layer_orm.g * landscape_layer_roughness[p_layer], 0.0, 1.0);

	r_albedo += lb_layer_albedo * p_weight;
	r_normal += lb_normal_tex * p_weight;
	r_orm += lb_layer_orm * p_weight;
	r_specular += landscape_layer_specular[p_layer] * p_weight;
	r_normal_strength += landscape_layer_normal_strength[p_layer] * p_weight;
	r_weight += p_weight;
}
)";
	return code;
}

String BlendLandscape3D::_get_shader_extension_fragment(uint32_t p_flags) const {
	String code = R"(
	// Landscape blend: near the ground of the landscape, the surface takes on the ground's.
	if (landscape_blend_active) {
		vec3 lb_world = (INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xyz;
		vec3 lb_local = (landscape_world_to_local * vec4(lb_world, 1.0)).xyz;
		float lb_quads = float(landscape_terrain_quads);
		vec2 lb_sample = lb_local.xz / landscape_vertex_spacing;

		// How far over the ground this is, in meters.
		float lb_above = (lb_local.y - lb_ground_height(lb_sample)) * landscape_height_scale - landscape_blend_offset;
)";
	if (p_flags & EXTENSION_NOISE) {
		code += R"(		lb_above += (texture(landscape_blend_noise, lb_world.xz / landscape_blend_noise_scale).r - 0.5) * landscape_blend_noise_strength * landscape_blend_height;
)";
	}
	code += R"(		float lb_top = max(landscape_blend_height, 0.0001);
		float lb_amount = 1.0 - smoothstep(lb_top * (1.0 - max(landscape_blend_falloff, 0.001)), lb_top, lb_above);
		// Nothing past the landscape's edges.
		lb_amount *= step(0.0, lb_sample.x) * step(lb_sample.x, lb_quads) * step(0.0, lb_sample.y) * step(lb_sample.y, lb_quads);

		vec3 lb_up = normalize(mat3(VIEW_MATRIX) * (landscape_normal_to_world * vec3(0.0, 1.0, 0.0)));
		lb_amount *= mix(1.0, smoothstep(0.0, 0.7, dot(NORMAL, lb_up)), landscape_blend_slope);

		if (lb_amount > 0.0) {
			// The ground's frame, from the slope of the heightmap, as the landscape's own.
			vec2 lb_slope = texture(landscape_gradient_map, (lb_sample + 0.5) / (lb_quads + 1.0)).rg;
			vec3 lb_ground_normal = normalize(vec3(-lb_slope.x, 1.0, -lb_slope.y));
			vec3 lb_albedo = vec3(0.6);
			vec3 lb_orm = vec3(1.0, 1.0, 0.0);
			float lb_specular = 0.5;

			// Like the landscape, the layers are blended here close to the camera (and where its
			// parallax reaches), and read back from its runtime virtual texture further away.
			// What else was drawn into the virtual texture (a road, a decal) shows all the same.
			float lb_distance = length(VERTEX);
			float lb_rvt = 0.0;
			vec2 lb_rvt_uv = lb_local.xz / (lb_quads * landscape_vertex_spacing);
			vec3 lb_rvt_orm = vec3(1.0, 1.0, 0.0);
			if (landscape_rvt_active) {
				lb_rvt = landscape_ground_near > 0.0 ? smoothstep(landscape_ground_near * 0.75, landscape_ground_near, lb_distance) : 1.0;
				if (landscape_ground_pom) {
					lb_rvt = min(lb_rvt, smoothstep(landscape_ground_pom_fade.x, landscape_ground_pom_fade.y, lb_distance));
				}
				vec4 lb_rvt_orm_ground = texture(landscape_rvt_data, vec3(lb_rvt_uv, 2.0));
				lb_rvt_orm = lb_rvt_orm_ground.rgb;
				// The ORM layer's alpha is how much of it is bare ground.
				lb_rvt = 1.0 - (1.0 - lb_rvt) * lb_rvt_orm_ground.a;
			}

			if (lb_rvt < 1.0) {
				vec3 lb_albedo_sum = vec3(0.0);
				vec3 lb_normal_sum = vec3(0.0);
				vec3 lb_orm_sum = vec3(0.0);
				float lb_specular_sum = 0.0;
				float lb_normal_strength_sum = 0.0;
				float lb_weight_sum = 0.0;
				vec2 lb_map_uv = (lb_sample + 0.5) / (lb_quads + 1.0);
				int lb_group_count = (landscape_layer_count + 3) / 4;
				for (int lb_g = 0; lb_g < lb_group_count; lb_g++) {
					vec4 lb_w = texture(landscape_weight_array, vec3(lb_map_uv, float(lb_g)));
					for (int lb_c = 0; lb_c < 4; lb_c++) {
						int lb_layer = lb_g * 4 + lb_c;
						if (lb_layer < landscape_layer_count) {
							lb_accumulate_layer(lb_layer, lb_w[lb_c], lb_local, lb_ground_normal, lb_albedo_sum, lb_normal_sum, lb_orm_sum, lb_specular_sum, lb_normal_strength_sum, lb_weight_sum);
						}
					}
				}
				if (lb_weight_sum > 0.001) {
					float lb_inv_weight = 1.0 / lb_weight_sum;
					lb_albedo = lb_albedo_sum * lb_inv_weight;
					lb_orm = lb_orm_sum * lb_inv_weight;
					lb_specular = lb_specular_sum * lb_inv_weight;
					// The blended normal map in the ground's frame (tangent along +X, binormal along -Z).
					vec2 lb_normal_xy = (lb_normal_sum.xy * lb_inv_weight) * 2.0 - 1.0;
					vec3 lb_normal_map = vec3(lb_normal_xy, sqrt(max(0.0, 1.0 - dot(lb_normal_xy, lb_normal_xy))));
					lb_normal_map = normalize(mix(vec3(0.0, 0.0, 1.0), lb_normal_map, lb_normal_strength_sum * lb_inv_weight));
					vec3 lb_tangent = normalize(vec3(1.0, lb_slope.x, 0.0));
					vec3 lb_binormal = cross(lb_ground_normal, lb_tangent);
					lb_ground_normal = normalize(lb_tangent * lb_normal_map.x + lb_binormal * lb_normal_map.y + lb_ground_normal * lb_normal_map.z);
				}
			}

			if (lb_rvt > 0.0) {
				vec4 lb_rvt_albedo = texture(landscape_rvt_albedo, vec3(lb_rvt_uv, 0.0));
				vec4 lb_rvt_normal = texture(landscape_rvt_data, vec3(lb_rvt_uv, 1.0));
				// Pages are drawn looking down the landscape: their normal (x, y, z) is its (x, z, -y).
				vec3 lb_page_normal = normalize(lb_rvt_normal.rgb * 2.0 - 1.0);
				lb_albedo = mix(lb_albedo, lb_rvt_albedo.rgb, lb_rvt);
				lb_ground_normal = normalize(mix(lb_ground_normal, vec3(lb_page_normal.x, lb_page_normal.z, -lb_page_normal.y), lb_rvt));
				lb_orm = mix(lb_orm, lb_rvt_orm, lb_rvt);
				lb_specular = mix(lb_specular, lb_rvt_normal.a, lb_rvt);
			}

			vec3 lb_normal = normalize(mat3(VIEW_MATRIX) * (landscape_normal_to_world * lb_ground_normal));
			ALBEDO = mix(ALBEDO, lb_albedo, lb_amount);
			ROUGHNESS = mix(ROUGHNESS, lb_orm.g, lb_amount);
			METALLIC = mix(METALLIC, lb_orm.b, lb_amount);
			AO = mix(AO, lb_orm.r, lb_amount);
			SPECULAR = mix(SPECULAR, lb_specular, lb_amount);
			NORMAL = normalize(mix(NORMAL, lb_normal, lb_amount * landscape_blend_normal_strength));
)";
	if (p_flags & EXTENSION_NORMAL_MAP) {
		code += R"(			// The material's own normal map fades out where the ground's normal takes over.
			NORMAL_MAP_DEPTH *= 1.0 - lb_amount * landscape_blend_normal_strength;
)";
	}
	code += "\t\t}\n\t}\n";
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
