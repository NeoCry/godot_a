/**************************************************************************/
/*  landscape_spline_3d.cpp                                               */
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

#include "landscape_spline_3d.h"

#include "core/io/image.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/object/worker_thread_pool.h"
#include "core/templates/hash_map.h"
#include "core/templates/hashfuncs.h"
#include "scene/3d/landscape_3d.h"
#include "scene/resources/curve.h"
#include "scene/resources/material.h"
#include "scene/resources/shader.h"
#include "servers/rendering/rendering_server.h"

namespace {

// More rings than this is a mistake (a tiny segment_length on a huge curve),
// not a road: 256k cross-sections is already over 250 km at 1 m.
constexpr int MAX_RINGS = 1 << 18;
// The most consecutive cross-sections simplification merges into one quad,
// however flat and straight the ground: past this, long thin triangles start
// to cost more in interpolation quality than they save.
constexpr int MAX_RING_SPAN = 32;
// Bumped whenever what a chunk's mesh is built from changes meaning, so a
// chunk built by an older version of this file is never mistaken for current.
constexpr uint64_t CHUNK_FORMAT_VERSION = 1;

float smoothstep01(float p_x) {
	const float x = CLAMP(p_x, 0.0f, 1.0f);
	return x * x * (3.0f - 2.0f * x);
}

uint64_t hash_bytes(const void *p_data, size_t p_size, uint64_t p_hash) {
	const uint8_t *bytes = static_cast<const uint8_t *>(p_data);
	size_t i = 0;
	for (; i + 8 <= p_size; i += 8) {
		uint64_t word;
		memcpy(&word, bytes + i, 8);
		p_hash = hash64_murmur3_64(word, p_hash);
	}
	for (; i < p_size; i++) {
		p_hash = hash64_murmur3_64(bytes[i], p_hash);
	}
	return p_hash;
}

uint64_t hash_float(float p_value, uint64_t p_hash) {
	uint32_t bits;
	memcpy(&bits, &p_value, 4);
	return hash64_murmur3_64(bits, p_hash);
}

// Across a river or stream bed, how much of carve_depth to dig at a fraction
// p_x of the way from the centerline to the bank: the full depth across the
// middle half, easing up to nothing at the bank. A flat bottom (rather than a
// V or a parabola) keeps a shallow stream's water visible across most of its
// width instead of only along a thin line down the middle.
float bed_profile(float p_x) {
	if (p_x <= 0.5f) {
		return 1.0f;
	}
	return 1.0f - smoothstep01((p_x - 0.5f) * 2.0f);
}

} // namespace

// Both built-in shaders transform their own vertices (skip_vertex_transform)
// so that they can slide each one a little towards the camera along its view
// ray: that leaves where it lands on screen exactly where it was, and only
// makes it win the depth test against the terrain it lies on. How far scales
// with distance, like depth buffer precision does, so a road stays on top of
// the ground from up close to the horizon, including where the terrain's own
// LOD has pushed the ground a little above it. Orthographic projections (a
// directional light's shadow pass) are left alone, since "towards the camera"
// means nothing useful there.
static const char *road_shader_code = R"(
shader_type spatial;
render_mode blend_mix, depth_draw_opaque, cull_back, diffuse_burley, specular_schlick_ggx, skip_vertex_transform;

// Built-in LandscapeSpline3D road material. UV.x runs 0-1 across the road and
// UV.y along it (1 per road width), vertex COLOR.a is the spline's edge and
// end fade.

uniform vec4 albedo : source_color = vec4(0.18, 0.18, 0.19, 1.0);
uniform vec4 shoulder_color : source_color = vec4(0.36, 0.33, 0.28, 1.0);
// How much of each side of the road, as a fraction of its width, is shoulder.
uniform float shoulder_width : hint_range(0.0, 0.5, 0.01) = 0.1;
uniform float roughness : hint_range(0.0, 1.0, 0.01) = 0.9;
uniform float specular : hint_range(0.0, 1.0, 0.01) = 0.3;
// Variation that breaks up the flat color, in fractions of it.
uniform float grain : hint_range(0.0, 1.0, 0.01) = 0.12;
// Dithered, so the fade stays in the opaque pass (and its depth prepass).
uniform bool use_vertex_alpha = true;
uniform float depth_offset : hint_range(0.0, 0.01, 0.0001) = 0.0005;

float hash21(vec2 p) {
	p = fract(p * vec2(123.34, 456.21));
	p += dot(p, p + 45.32);
	return fract(p.x * p.y);
}

float value_noise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(hash21(i), hash21(i + vec2(1.0, 0.0)), u.x), mix(hash21(i + vec2(0.0, 1.0)), hash21(i + vec2(1.0, 1.0)), u.x), u.y);
}

void vertex() {
	VERTEX = (MODELVIEW_MATRIX * vec4(VERTEX, 1.0)).xyz;
	NORMAL = normalize(MODELVIEW_NORMAL_MATRIX * NORMAL);
	TANGENT = normalize(mat3(MODELVIEW_MATRIX) * TANGENT);
	BINORMAL = normalize(mat3(MODELVIEW_MATRIX) * BINORMAL);
	if (PROJECTION_MATRIX[3][3] == 0.0) {
		VERTEX *= 1.0 - depth_offset;
	}
}

void fragment() {
	float edge = min(UV.x, 1.0 - UV.x);
	float shoulder = 1.0 - smoothstep(shoulder_width * 0.7, shoulder_width, edge);
	float n = value_noise(UV * vec2(24.0, 24.0)) * 0.6 + value_noise(UV * vec2(3.0, 3.0)) * 0.4;
	vec3 color = mix(albedo.rgb, shoulder_color.rgb, shoulder);
	ALBEDO = color * (1.0 + (n - 0.5) * 2.0 * grain);
	ROUGHNESS = roughness;
	SPECULAR = specular;
	if (use_vertex_alpha) {
		ALPHA = COLOR.a;
		ALPHA_HASH_SCALE = 1.0;
	}
}
)";

static const char *water_shader_code = R"(
shader_type spatial;
render_mode blend_mix, depth_draw_opaque, cull_back, diffuse_burley, specular_schlick_ggx, skip_vertex_transform;

// Built-in LandscapeSpline3D river and stream material. UV.x runs 0-1 across
// the water and UV.y along it, downstream (1 per spline width), so ripples
// scroll along UV.y; vertex COLOR.a is the spline's edge and end fade.

uniform vec4 shallow_color : source_color = vec4(0.2, 0.4, 0.38, 1.0);
uniform vec4 deep_color : source_color = vec4(0.02, 0.09, 0.12, 1.0);
// Depth of water, in meters, over which it goes from shallow_color (and
// see-through) to deep_color (and opacity).
uniform float depth_range : hint_range(0.01, 100.0, 0.01) = 3.0;
uniform float opacity : hint_range(0.0, 1.0, 0.01) = 0.9;
// Depth of water, in meters, over which it fades in from nothing at the
// shoreline, hiding where its mesh meets the ground.
uniform float shore_fade : hint_range(0.0, 10.0, 0.01) = 0.3;
// How fast the ripples travel downstream, in spline widths per second.
uniform float flow_speed = 0.35;
// Ripples per spline width.
uniform float wave_scale : hint_range(0.1, 128.0, 0.1) = 4.0;
uniform float wave_strength : hint_range(0.0, 2.0, 0.01) = 0.35;
uniform float roughness : hint_range(0.0, 1.0, 0.01) = 0.03;
uniform float depth_offset : hint_range(0.0, 0.01, 0.0001) = 0.0002;
uniform sampler2D depth_texture : hint_depth_texture, filter_nearest, repeat_disable;

float hash21(vec2 p) {
	p = fract(p * vec2(123.34, 456.21));
	p += dot(p, p + 45.32);
	return fract(p.x * p.y);
}

float value_noise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(hash21(i), hash21(i + vec2(1.0, 0.0)), u.x), mix(hash21(i + vec2(0.0, 1.0)), hash21(i + vec2(1.0, 1.0)), u.x), u.y);
}

float ripples(vec2 p) {
	return value_noise(p) * 0.65 + value_noise(p * 2.3 + vec2(17.0, 5.0)) * 0.35;
}

vec2 ripple_slope(vec2 p) {
	const float e = 0.05;
	float h = ripples(p);
	return vec2(ripples(p + vec2(e, 0.0)) - h, ripples(p + vec2(0.0, e)) - h) / e;
}

void vertex() {
	VERTEX = (MODELVIEW_MATRIX * vec4(VERTEX, 1.0)).xyz;
	NORMAL = normalize(MODELVIEW_NORMAL_MATRIX * NORMAL);
	TANGENT = normalize(mat3(MODELVIEW_MATRIX) * TANGENT);
	BINORMAL = normalize(mat3(MODELVIEW_MATRIX) * BINORMAL);
	if (PROJECTION_MATRIX[3][3] == 0.0) {
		VERTEX *= 1.0 - depth_offset;
	}
}

void fragment() {
	// Two layers moving at different speeds, so the pattern churns rather
	// than visibly sliding along as one piece.
	vec2 p = UV * wave_scale;
	float travel = TIME * flow_speed * wave_scale;
	vec2 slope = ripple_slope(p - vec2(0.0, travel));
	slope += ripple_slope(p * 1.7 + vec2(3.1, 0.0) - vec2(0.0, travel * 1.35)) * 0.5;
	NORMAL_MAP = normalize(vec3(-slope * wave_strength * 0.25, 1.0)) * 0.5 + 0.5;

	float depth_raw = texture(depth_texture, SCREEN_UV).r;
#if CURRENT_RENDERER == RENDERER_COMPATIBILITY
	vec3 ndc = vec3(SCREEN_UV, depth_raw) * 2.0 - 1.0;
#else
	vec3 ndc = vec3(SCREEN_UV * 2.0 - 1.0, depth_raw);
#endif
	vec4 ground = INV_PROJECTION_MATRIX * vec4(ndc, 1.0);
	float water_depth = max(VERTEX.z - ground.z / ground.w, 0.0);

	float deepness = 1.0 - exp(-3.0 * water_depth / max(depth_range, 0.001));
	ALBEDO = mix(shallow_color.rgb, deep_color.rgb, deepness);
	ROUGHNESS = roughness;
	METALLIC = 0.0;
	SPECULAR = 0.5;
	float shore = clamp(water_depth / max(shore_fade, 0.001), 0.0, 1.0);
	ALPHA = mix(0.3, 1.0, deepness) * opacity * shore * COLOR.a;
}
)";

static void _setup_default_material(const Ref<ShaderMaterial> &p_material, LandscapeSpline3D::SplineType p_type) {
	if (p_type == LandscapeSpline3D::TYPE_STREAM) {
		// Shallow, clear and quick: the bed shows through, and the surface is
		// all broken-up little ripples.
		p_material->set_shader_parameter("shallow_color", Color(0.3, 0.46, 0.42));
		p_material->set_shader_parameter("deep_color", Color(0.05, 0.14, 0.15));
		p_material->set_shader_parameter("depth_range", 1.2);
		p_material->set_shader_parameter("opacity", 0.8);
		p_material->set_shader_parameter("shore_fade", 0.12);
		p_material->set_shader_parameter("flow_speed", 0.9);
		p_material->set_shader_parameter("wave_scale", 3.0);
		p_material->set_shader_parameter("wave_strength", 0.7);
	}
}

void LandscapeSpline3D::init_shaders() {
	road_shader.instantiate();
	road_shader->set_code(road_shader_code);
	water_shader.instantiate();
	water_shader->set_code(water_shader_code);

	for (int i = 0; i < TYPE_MAX; i++) {
		default_materials[i].instantiate();
		default_materials[i]->set_shader(i == TYPE_ROAD ? road_shader : water_shader);
		_setup_default_material(default_materials[i], (SplineType)i);
	}
}

void LandscapeSpline3D::finish_shaders() {
	for (int i = 0; i < TYPE_MAX; i++) {
		default_materials[i].unref();
	}
	road_shader.unref();
	water_shader.unref();
}

Ref<Material> LandscapeSpline3D::get_default_material(SplineType p_type) {
	ERR_FAIL_INDEX_V(p_type, TYPE_MAX, Ref<Material>());
	return default_materials[p_type];
}

Ref<Material> LandscapeSpline3D::create_default_material(SplineType p_type) {
	ERR_FAIL_INDEX_V(p_type, TYPE_MAX, Ref<Material>());
	// A shader of its own rather than the shared built-in one, so editing its
	// code changes this material only.
	Ref<Shader> shader;
	shader.instantiate();
	shader->set_code(p_type == TYPE_ROAD ? road_shader_code : water_shader_code);

	Ref<ShaderMaterial> result;
	result.instantiate();
	result->set_shader(shader);
	_setup_default_material(result, p_type);
	return result;
}

Dictionary LandscapeSpline3D::get_preset(SplineType p_type) {
	Dictionary preset;
	switch (p_type) {
		case TYPE_ROAD: {
			preset["width"] = 6.0;
			preset["height_mode"] = HEIGHT_MODE_CONFORM;
			preset["height_offset"] = 0.05;
			preset["cross_segments"] = 6;
			preset["edge_fade"] = 0.08;
			preset["end_fade_length"] = 0.0;
			preset["gi_mode"] = GeometryInstance3D::GI_MODE_STATIC;
			preset["carve_depth"] = 0.0;
			preset["carve_falloff"] = 4.0;
			preset["paint_falloff"] = 2.0;
		} break;
		case TYPE_RIVER: {
			preset["width"] = 16.0;
			preset["height_mode"] = HEIGHT_MODE_SPLINE;
			preset["height_offset"] = 0.0;
			preset["cross_segments"] = 4;
			preset["edge_fade"] = 0.12;
			preset["end_fade_length"] = 8.0;
			preset["gi_mode"] = GeometryInstance3D::GI_MODE_DISABLED;
			preset["carve_depth"] = 2.5;
			preset["carve_falloff"] = 8.0;
			preset["paint_falloff"] = 4.0;
		} break;
		case TYPE_STREAM: {
			preset["width"] = 2.5;
			preset["height_mode"] = HEIGHT_MODE_CONFORM_LEVEL;
			preset["height_offset"] = 0.12;
			preset["cross_segments"] = 2;
			preset["edge_fade"] = 0.25;
			preset["end_fade_length"] = 3.0;
			preset["gi_mode"] = GeometryInstance3D::GI_MODE_DISABLED;
			preset["carve_depth"] = 0.4;
			preset["carve_falloff"] = 1.5;
			preset["paint_falloff"] = 1.0;
		} break;
		default: {
			ERR_FAIL_V_MSG(preset, "Invalid spline type.");
		}
	}
	return preset;
}

void LandscapeSpline3D::apply_preset(SplineType p_type) {
	ERR_FAIL_INDEX(p_type, TYPE_MAX);
	set_spline_type(p_type);
	const Dictionary preset = get_preset(p_type);
	for (const KeyValue<Variant, Variant> &kv : preset) {
		set(kv.key, kv.value);
	}
}

void LandscapeSpline3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_spline_type", "type"), &LandscapeSpline3D::set_spline_type);
	ClassDB::bind_method(D_METHOD("get_spline_type"), &LandscapeSpline3D::get_spline_type);

	ClassDB::bind_method(D_METHOD("set_landscape_path", "path"), &LandscapeSpline3D::set_landscape_path);
	ClassDB::bind_method(D_METHOD("get_landscape_path"), &LandscapeSpline3D::get_landscape_path);

	ClassDB::bind_method(D_METHOD("set_material", "material"), &LandscapeSpline3D::set_material);
	ClassDB::bind_method(D_METHOD("get_material"), &LandscapeSpline3D::get_material);

	ClassDB::bind_method(D_METHOD("set_width", "width"), &LandscapeSpline3D::set_width);
	ClassDB::bind_method(D_METHOD("get_width"), &LandscapeSpline3D::get_width);

	ClassDB::bind_method(D_METHOD("set_width_curve", "curve"), &LandscapeSpline3D::set_width_curve);
	ClassDB::bind_method(D_METHOD("get_width_curve"), &LandscapeSpline3D::get_width_curve);

	ClassDB::bind_method(D_METHOD("set_height_mode", "mode"), &LandscapeSpline3D::set_height_mode);
	ClassDB::bind_method(D_METHOD("get_height_mode"), &LandscapeSpline3D::get_height_mode);

	ClassDB::bind_method(D_METHOD("set_height_offset", "offset"), &LandscapeSpline3D::set_height_offset);
	ClassDB::bind_method(D_METHOD("get_height_offset"), &LandscapeSpline3D::get_height_offset);

	ClassDB::bind_method(D_METHOD("set_smooth", "smooth"), &LandscapeSpline3D::set_smooth);
	ClassDB::bind_method(D_METHOD("is_smooth"), &LandscapeSpline3D::is_smooth);

	ClassDB::bind_method(D_METHOD("set_segment_length", "length"), &LandscapeSpline3D::set_segment_length);
	ClassDB::bind_method(D_METHOD("get_segment_length"), &LandscapeSpline3D::get_segment_length);

	ClassDB::bind_method(D_METHOD("set_cross_segments", "segments"), &LandscapeSpline3D::set_cross_segments);
	ClassDB::bind_method(D_METHOD("get_cross_segments"), &LandscapeSpline3D::get_cross_segments);

	ClassDB::bind_method(D_METHOD("set_simplify_tolerance", "tolerance"), &LandscapeSpline3D::set_simplify_tolerance);
	ClassDB::bind_method(D_METHOD("get_simplify_tolerance"), &LandscapeSpline3D::get_simplify_tolerance);

	ClassDB::bind_method(D_METHOD("set_chunk_length", "length"), &LandscapeSpline3D::set_chunk_length);
	ClassDB::bind_method(D_METHOD("get_chunk_length"), &LandscapeSpline3D::get_chunk_length);

	ClassDB::bind_method(D_METHOD("set_uv_scale", "scale"), &LandscapeSpline3D::set_uv_scale);
	ClassDB::bind_method(D_METHOD("get_uv_scale"), &LandscapeSpline3D::get_uv_scale);

	ClassDB::bind_method(D_METHOD("set_edge_fade", "fade"), &LandscapeSpline3D::set_edge_fade);
	ClassDB::bind_method(D_METHOD("get_edge_fade"), &LandscapeSpline3D::get_edge_fade);

	ClassDB::bind_method(D_METHOD("set_end_fade_length", "length"), &LandscapeSpline3D::set_end_fade_length);
	ClassDB::bind_method(D_METHOD("get_end_fade_length"), &LandscapeSpline3D::get_end_fade_length);

	ClassDB::bind_method(D_METHOD("set_cast_shadow", "setting"), &LandscapeSpline3D::set_cast_shadow);
	ClassDB::bind_method(D_METHOD("get_cast_shadow"), &LandscapeSpline3D::get_cast_shadow);

	ClassDB::bind_method(D_METHOD("set_gi_mode", "mode"), &LandscapeSpline3D::set_gi_mode);
	ClassDB::bind_method(D_METHOD("get_gi_mode"), &LandscapeSpline3D::get_gi_mode);

	ClassDB::bind_method(D_METHOD("set_render_layers", "layers"), &LandscapeSpline3D::set_render_layers);
	ClassDB::bind_method(D_METHOD("get_render_layers"), &LandscapeSpline3D::get_render_layers);

	ClassDB::bind_method(D_METHOD("set_lod_bias", "bias"), &LandscapeSpline3D::set_lod_bias);
	ClassDB::bind_method(D_METHOD("get_lod_bias"), &LandscapeSpline3D::get_lod_bias);

	ClassDB::bind_method(D_METHOD("set_visibility_range_end", "distance"), &LandscapeSpline3D::set_visibility_range_end);
	ClassDB::bind_method(D_METHOD("get_visibility_range_end"), &LandscapeSpline3D::get_visibility_range_end);

	ClassDB::bind_method(D_METHOD("set_visibility_range_end_margin", "margin"), &LandscapeSpline3D::set_visibility_range_end_margin);
	ClassDB::bind_method(D_METHOD("get_visibility_range_end_margin"), &LandscapeSpline3D::get_visibility_range_end_margin);

	ClassDB::bind_method(D_METHOD("set_carve_enabled", "enabled"), &LandscapeSpline3D::set_carve_enabled);
	ClassDB::bind_method(D_METHOD("is_carve_enabled"), &LandscapeSpline3D::is_carve_enabled);

	ClassDB::bind_method(D_METHOD("set_carve_depth", "depth"), &LandscapeSpline3D::set_carve_depth);
	ClassDB::bind_method(D_METHOD("get_carve_depth"), &LandscapeSpline3D::get_carve_depth);

	ClassDB::bind_method(D_METHOD("set_carve_falloff", "falloff"), &LandscapeSpline3D::set_carve_falloff);
	ClassDB::bind_method(D_METHOD("get_carve_falloff"), &LandscapeSpline3D::get_carve_falloff);

	ClassDB::bind_method(D_METHOD("set_paint_layer", "layer"), &LandscapeSpline3D::set_paint_layer);
	ClassDB::bind_method(D_METHOD("get_paint_layer"), &LandscapeSpline3D::get_paint_layer);

	ClassDB::bind_method(D_METHOD("set_paint_strength", "strength"), &LandscapeSpline3D::set_paint_strength);
	ClassDB::bind_method(D_METHOD("get_paint_strength"), &LandscapeSpline3D::get_paint_strength);

	ClassDB::bind_method(D_METHOD("set_paint_falloff", "falloff"), &LandscapeSpline3D::set_paint_falloff);
	ClassDB::bind_method(D_METHOD("get_paint_falloff"), &LandscapeSpline3D::get_paint_falloff);

	ClassDB::bind_method(D_METHOD("get_landscape"), &LandscapeSpline3D::get_landscape);
	ClassDB::bind_method(D_METHOD("project_to_landscape", "local_position"), &LandscapeSpline3D::project_to_landscape);
	ClassDB::bind_method(D_METHOD("update_mesh"), &LandscapeSpline3D::update_mesh);
	ClassDB::bind_method(D_METHOD("get_landscape_footprint"), &LandscapeSpline3D::get_landscape_footprint);
	ClassDB::bind_method(D_METHOD("apply_to_landscape"), &LandscapeSpline3D::apply_to_landscape);
	ClassDB::bind_method(D_METHOD("apply_preset", "type"), &LandscapeSpline3D::apply_preset);
	ClassDB::bind_method(D_METHOD("get_chunk_count"), &LandscapeSpline3D::get_chunk_count);

	ClassDB::bind_static_method("LandscapeSpline3D", D_METHOD("get_preset", "type"), &LandscapeSpline3D::get_preset);
	ClassDB::bind_static_method("LandscapeSpline3D", D_METHOD("get_default_material", "type"), &LandscapeSpline3D::get_default_material);
	ClassDB::bind_static_method("LandscapeSpline3D", D_METHOD("create_default_material", "type"), &LandscapeSpline3D::create_default_material);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "spline_type", PROPERTY_HINT_ENUM, "Road,River,Stream"), "set_spline_type", "get_spline_type");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "landscape_path", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Landscape3D"), "set_landscape_path", "get_landscape_path");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, "BaseMaterial3D,ShaderMaterial"), "set_material", "get_material");

	ADD_GROUP("Shape", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "width", PROPERTY_HINT_RANGE, "0.01,1000,0.01,or_greater,suffix:m"), "set_width", "get_width");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "width_curve", PROPERTY_HINT_RESOURCE_TYPE, "Curve"), "set_width_curve", "get_width_curve");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "height_mode", PROPERTY_HINT_ENUM, "Conform,Conform Level,Spline"), "set_height_mode", "get_height_mode");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "height_offset", PROPERTY_HINT_RANGE, "-10,10,0.001,or_less,or_greater,suffix:m"), "set_height_offset", "get_height_offset");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "smooth"), "set_smooth", "is_smooth");

	ADD_GROUP("Mesh", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "segment_length", PROPERTY_HINT_RANGE, "0.05,100,0.01,or_greater,suffix:m"), "set_segment_length", "get_segment_length");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cross_segments", PROPERTY_HINT_RANGE, vformat("1,%d,1", MAX_CROSS_SEGMENTS)), "set_cross_segments", "get_cross_segments");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "simplify_tolerance", PROPERTY_HINT_RANGE, "0,1,0.001,or_greater,suffix:m"), "set_simplify_tolerance", "get_simplify_tolerance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "chunk_length", PROPERTY_HINT_RANGE, "4,4096,0.1,or_greater,suffix:m"), "set_chunk_length", "get_chunk_length");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "uv_scale", PROPERTY_HINT_LINK), "set_uv_scale", "get_uv_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "edge_fade", PROPERTY_HINT_RANGE, "0,0.5,0.01"), "set_edge_fade", "get_edge_fade");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "end_fade_length", PROPERTY_HINT_RANGE, "0,100,0.1,or_greater,suffix:m"), "set_end_fade_length", "get_end_fade_length");

	ADD_GROUP("Rendering", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cast_shadow", PROPERTY_HINT_ENUM, "Off,On,Double-Sided,Shadows Only"), "set_cast_shadow", "get_cast_shadow");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "gi_mode", PROPERTY_HINT_ENUM, "Disabled,Static,Dynamic"), "set_gi_mode", "get_gi_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "render_layers", PROPERTY_HINT_LAYERS_3D_RENDER), "set_render_layers", "get_render_layers");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lod_bias", PROPERTY_HINT_RANGE, "0.001,128,0.001"), "set_lod_bias", "get_lod_bias");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "visibility_range_end", PROPERTY_HINT_RANGE, "0,4096,0.01,or_greater,suffix:m"), "set_visibility_range_end", "get_visibility_range_end");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "visibility_range_end_margin", PROPERTY_HINT_RANGE, "0,4096,0.01,or_greater,suffix:m"), "set_visibility_range_end_margin", "get_visibility_range_end_margin");

	ADD_GROUP("Carve", "carve_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "carve_enabled"), "set_carve_enabled", "is_carve_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "carve_depth", PROPERTY_HINT_RANGE, "0,50,0.01,or_greater,suffix:m"), "set_carve_depth", "get_carve_depth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "carve_falloff", PROPERTY_HINT_RANGE, "0,100,0.01,or_greater,suffix:m"), "set_carve_falloff", "get_carve_falloff");

	ADD_GROUP("Paint", "paint_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "paint_layer", PROPERTY_HINT_RANGE, vformat("-1,%d,1", TerrainData::MAX_LAYERS - 1)), "set_paint_layer", "get_paint_layer");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "paint_strength", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_paint_strength", "get_paint_strength");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "paint_falloff", PROPERTY_HINT_RANGE, "0,100,0.01,or_greater,suffix:m"), "set_paint_falloff", "get_paint_falloff");

	BIND_ENUM_CONSTANT(TYPE_ROAD);
	BIND_ENUM_CONSTANT(TYPE_RIVER);
	BIND_ENUM_CONSTANT(TYPE_STREAM);
	BIND_ENUM_CONSTANT(TYPE_MAX);

	BIND_ENUM_CONSTANT(HEIGHT_MODE_CONFORM);
	BIND_ENUM_CONSTANT(HEIGHT_MODE_CONFORM_LEVEL);
	BIND_ENUM_CONSTANT(HEIGHT_MODE_SPLINE);
}

void LandscapeSpline3D::_validate_property(PropertyInfo &p_property) const {
	if (p_property.name == "paint_layer") {
		// Offer only the layers the landscape actually has.
		const Landscape3D *landscape = _get_landscape();
		if (landscape != nullptr) {
			p_property.hint_string = vformat("-1,%d,1", MAX(landscape->get_layers().size() - 1, 0));
		}
	}
}

void LandscapeSpline3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			_queue_update(UPDATE_RINGS);
		} break;

		case NOTIFICATION_EXIT_TREE: {
			_disconnect_landscape();
		} break;

		case NOTIFICATION_ENTER_WORLD: {
			const RID scenario = _get_scenario();
			for (const Chunk &chunk : chunks) {
				if (chunk.instance.is_valid()) {
					RS::get_singleton()->instance_set_scenario(chunk.instance, scenario);
				}
			}
		} break;

		case NOTIFICATION_EXIT_WORLD: {
			for (const Chunk &chunk : chunks) {
				if (chunk.instance.is_valid()) {
					RS::get_singleton()->instance_set_scenario(chunk.instance, RID());
				}
			}
		} break;

		case NOTIFICATION_TRANSFORM_CHANGED: {
			const Transform3D xform = get_global_transform();
			for (const Chunk &chunk : chunks) {
				if (chunk.instance.is_valid()) {
					RS::get_singleton()->instance_set_transform(chunk.instance, xform);
				}
			}
			// Moving the spline over the terrain changes the ground under it,
			// and turning it relative to the terrain changes which way is up.
			if (!_get_local_up().is_equal_approx(rings_up)) {
				_queue_update(UPDATE_RINGS);
			} else if (height_mode != HEIGHT_MODE_SPLINE && _get_landscape() != nullptr) {
				_queue_update(UPDATE_ALL_CHUNKS);
			}
		} break;

		case NOTIFICATION_VISIBILITY_CHANGED: {
			const bool visible = is_visible_in_tree();
			for (const Chunk &chunk : chunks) {
				if (chunk.instance.is_valid()) {
					RS::get_singleton()->instance_set_visible(chunk.instance, visible);
				}
			}
		} break;
	}
}

// Updating.

void LandscapeSpline3D::_queue_update(uint32_t p_flags) {
	pending_update |= p_flags;
	if (update_queued || !is_inside_tree()) {
		// Entering the tree queues a full update anyway.
		return;
	}
	update_queued = true;
	// Deferred, so a burst of changes in one frame (a gizmo drag, a script
	// setting several properties, a brush stroke under the spline) costs one
	// update, not one each.
	callable_mp(this, &LandscapeSpline3D::_update).call_deferred();
}

void LandscapeSpline3D::update_mesh() {
	_update();
}

void LandscapeSpline3D::_update() {
	update_queued = false;
	if (!is_inside_tree()) {
		return;
	}

	_update_landscape_connection();

	const uint32_t flags = pending_update;
	pending_update = 0;
	if (flags == 0) {
		return;
	}

	if (flags & UPDATE_RINGS) {
		_update_rings();
		_update_columns();
		_partition_chunks();
	}

	const bool all = (flags & (UPDATE_RINGS | UPDATE_ALL_CHUNKS)) != 0;
	LocalVector<ChunkBuild> builds;
	for (uint32_t i = 0; i < chunks.size(); i++) {
		if (all || chunks[i].dirty || !chunks[i].built) {
			ChunkBuild build;
			build.chunk_index = i;
			builds.push_back(build);
		}
		chunks[i].dirty = false;
	}
	if (builds.is_empty()) {
		return;
	}

	BuildContext context;
	context.builds = builds.ptr();
	context.settings_hash = _get_settings_hash();
	context.use_terrain = height_mode != HEIGHT_MODE_SPLINE && _make_terrain_sampler(context.sampler);

	// Building a chunk touches nothing but its own ChunkBuild, so they are
	// all built at once; only handing the results to the RenderingServer,
	// below, has to happen here on the main thread.
	if (builds.size() == 1) {
		_build_chunk(0, &context);
	} else {
		WorkerThreadPool::GroupID group = WorkerThreadPool::get_singleton()->add_template_group_task(this, &LandscapeSpline3D::_build_chunk, &context, builds.size(), -1, true, "LandscapeSpline3D");
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(group);
	}

	for (const ChunkBuild &build : builds) {
		if (build.changed) {
			_commit_chunk(chunks[build.chunk_index], build);
		}
	}
}

Vector3 LandscapeSpline3D::_get_local_up() const {
	const Landscape3D *landscape = _get_landscape();
	if (landscape == nullptr || !is_inside_tree() || !landscape->is_inside_tree()) {
		return Vector3(0, 1, 0);
	}
	const Vector3 up = get_global_transform().basis.inverse().xform(landscape->get_global_transform().basis.get_column(1));
	if (up.length_squared() < CMP_EPSILON2) {
		return Vector3(0, 1, 0);
	}
	return up.normalized();
}

Ref<Curve3D> LandscapeSpline3D::_make_sampling_curve(const Ref<Curve3D> &p_curve) const {
	// A private copy of the curve to sample: fine enough baking for the
	// segment length in use whatever the curve's own bake_interval is, no
	// up vectors (the strip works out its own frame against the terrain's up
	// axis), and - with smooth on - handles filled in wherever the user left
	// a point without any, so points dropped one after another on the ground
	// make a flowing road instead of a polyline with a kink at every point.
	Ref<Curve3D> sampling;
	sampling.instantiate();
	sampling->set_up_vector_enabled(false);
	sampling->set_bake_interval(CLAMP(segment_length * 0.25f, 0.02f, 1.0f));

	const int count = p_curve->get_point_count();
	const bool closed = p_curve->is_closed();
	for (int i = 0; i < count; i++) {
		const Vector3 position = p_curve->get_point_position(i);
		Vector3 in = p_curve->get_point_in(i);
		Vector3 out = p_curve->get_point_out(i);

		if (smooth && in.is_zero_approx() && out.is_zero_approx()) {
			const bool has_prev = closed || i > 0;
			const bool has_next = closed || i < count - 1;
			if (has_prev && has_next) {
				const Vector3 prev = p_curve->get_point_position((i + count - 1) % count);
				const Vector3 next = p_curve->get_point_position((i + 1) % count);
				// Catmull-Rom's tangent direction, with each handle a third of
				// the way to its own neighbor rather than the uniform spline's
				// fixed length, which would overshoot badly next to a much
				// shorter segment.
				const Vector3 direction = next - prev;
				if (!direction.is_zero_approx()) {
					const Vector3 tangent = direction.normalized();
					out = tangent * position.distance_to(next) / 3.0f;
					in = -tangent * position.distance_to(prev) / 3.0f;
				}
			}
			// An open end keeps its zero handle: the curve then leaves it
			// heading for the neighbor's own handle, which is already smooth.
		}

		sampling->add_point(position, in, out);
		sampling->set_point_tilt(i, p_curve->get_point_tilt(i));
	}
	sampling->set_closed(closed);
	return sampling;
}

void LandscapeSpline3D::_update_rings() {
	rings.clear();
	total_length = 0.0f;
	rings_up = _get_local_up();
	rings_closed = false;

	const Ref<Curve3D> source_curve = get_curve();
	if (source_curve.is_null() || source_curve->get_point_count() < 2) {
		return;
	}

	const Ref<Curve3D> sampling = _make_sampling_curve(source_curve);
	const float length = sampling->get_baked_length();
	if (!(length > CMP_EPSILON)) {
		return;
	}
	total_length = length;
	rings_closed = source_curve->is_closed();

	// Rings sit at whole multiples of segment_length from the start, plus
	// one at the very end, rather than spreading evenly over the length:
	// then lengthening or shortening the source_curve's far end leaves every ring
	// before the change exactly where it was, and with them the chunks there
	// (see _build_chunk), instead of nudging every ring along the whole
	// spline and rebuilding all of it.
	ring_step = MAX(segment_length, 0.05f);
	int regular = (int)Math::floor(length / ring_step);
	if (regular > MAX_RINGS - 2) {
		ring_step = length / (MAX_RINGS - 2);
		regular = MAX_RINGS - 2;
	}
	// A regular ring almost on top of the end ring would only add a sliver.
	if (regular > 0 && length - regular * ring_step < ring_step * 0.25f) {
		regular--;
	}
	const int steps = regular + 1;
	rings.resize(steps + 1);
	LocalVector<float> tilts;
	tilts.resize(steps + 1);

	for (int i = 0; i <= steps; i++) {
		const float s = (i == steps) ? length : i * ring_step;
		Ring &ring = rings[i];
		ring.center = sampling->sample_baked(s, true);
		ring.distance = s;
		tilts[i] = sampling->sample_baked_tilt(s);

		const float width_scale = width_curve.is_valid() ? width_curve->sample_baked(s / length) : 1.0f;
		ring.half_width = MAX(width * width_scale, 0.0f) * 0.5f;
		ring.end_alpha = 1.0f;
		if (!rings_closed && end_fade_length > 0.0f) {
			ring.end_alpha = CLAMP(MIN(s, length - s) / end_fade_length, 0.0f, 1.0f);
		}
	}

	// Directions come from the sampled centerline itself rather than the
	// source_curve's own frames: the chord between the neighboring samples is
	// exactly the direction this polyline runs, it never degenerates the way
	// a source_curve frame does on a vertical tangent, and it matches the strip that
	// actually gets built.
	Vector3 flat_right = rings_up.get_any_perpendicular();
	for (int i = 0; i <= steps; i++) {
		const int prev = _wrap_ring(i - 1);
		const int next = _wrap_ring(i + 1);
		const Vector3 forward = rings[next].center - rings[prev].center;
		const Vector3 flat_forward = forward - rings_up * forward.dot(rings_up);
		if (flat_forward.length_squared() > CMP_EPSILON2) {
			// Right-handed, as seen from above: forward x up points right.
			flat_right = flat_forward.normalized().cross(rings_up).normalized();
		}
		Ring &ring = rings[i];
		ring.flat_right = flat_right;
		ring.right = flat_right;
		if (tilts[i] != 0.0f && forward.length_squared() > CMP_EPSILON2) {
			// The same twist around the direction of travel Curve3D applies
			// to its own frames, so the bank matches the gizmo's tilt disk.
			ring.right = flat_right.rotated(forward.normalized(), tilts[i]);
		}
	}
}

int LandscapeSpline3D::_wrap_ring(int p_index) const {
	const int count = rings.size();
	if (count == 0) {
		return 0;
	}
	if (rings_closed && count > 2) {
		// The last ring sits where the first does, so the ring before the
		// first is the one before the last, and vice versa.
		if (p_index < 0) {
			return p_index + count - 1;
		}
		if (p_index > count - 1) {
			return p_index - (count - 1);
		}
		return p_index;
	}
	return CLAMP(p_index, 0, count - 1);
}

void LandscapeSpline3D::_update_columns() {
	columns.clear();
	const int segments = CLAMP(cross_segments, 1, MAX_CROSS_SEGMENTS);
	const float fade = CLAMP(edge_fade, 0.0f, 0.5f);

	LocalVector<float> us;
	for (int j = 0; j <= segments; j++) {
		us.push_back((float)j / segments);
	}
	if (fade > 0.0f) {
		// Where the fade finishes has to be a vertex of its own, or the fade
		// would spread across whatever cross segment it falls in.
		us.push_back(fade);
		us.push_back(1.0f - fade);
	}
	us.sort();

	for (const float u : us) {
		const bool fade_end = fade > 0.0f && (Math::is_equal_approx(u, fade) || Math::is_equal_approx(u, 1.0f - fade));
		const bool feature = u <= 0.0f || u >= 1.0f || fade_end;
		if (!columns.is_empty() && Math::abs(columns[columns.size() - 1].u - u) < 0.0001f) {
			columns[columns.size() - 1].feature = columns[columns.size() - 1].feature || feature;
			continue;
		}
		Column column;
		column.u = u;
		column.alpha = fade > 0.0f ? CLAMP(MIN(u, 1.0f - u) / fade, 0.0f, 1.0f) : 1.0f;
		column.feature = feature;
		columns.push_back(column);
	}
}

void LandscapeSpline3D::_partition_chunks() {
	const int ring_count = rings.size();
	int chunk_count = 0;
	int rings_per_chunk = 1;
	if (ring_count >= 2) {
		rings_per_chunk = MAX(1, (int)Math::round(chunk_length / MAX(ring_step, 0.0001f)));
		chunk_count = (ring_count - 1 + rings_per_chunk - 1) / rings_per_chunk;
	}

	for (uint32_t i = chunk_count; i < chunks.size(); i++) {
		_free_chunk(chunks[i]);
	}
	chunks.resize(chunk_count);

	// Chunks keep their slot (and their mesh and instance) across
	// repartitioning; whether a slot's mesh still matches its new range is up
	// to the hash check.
	for (int i = 0; i < chunk_count; i++) {
		chunks[i].first_ring = i * rings_per_chunk;
		chunks[i].last_ring = MIN((i + 1) * rings_per_chunk, ring_count - 1);
	}
}

uint64_t LandscapeSpline3D::_get_settings_hash() const {
	uint64_t hash = hash64_murmur3_64(CHUNK_FORMAT_VERSION, 0);
	for (const Column &column : columns) {
		hash = hash_float(column.u, hash);
		hash = hash_float(column.alpha, hash);
		hash = hash64_murmur3_64(column.feature ? 1 : 0, hash);
	}
	hash = hash_float(uv_scale.x, hash);
	hash = hash_float(uv_scale.y, hash);
	hash = hash_float(width, hash);
	hash = hash_float(simplify_tolerance, hash);
	hash = hash_float(rings_up.x, hash);
	hash = hash_float(rings_up.y, hash);
	hash = hash_float(rings_up.z, hash);
	return hash;
}

float LandscapeSpline3D::TerrainSampler::height_at(float p_x, float p_z) const {
	const float fx = CLAMP(p_x / spacing, 0.0f, (float)(resolution - 1));
	const float fz = CLAMP(p_z / spacing, 0.0f, (float)(resolution - 1));
	const int ix = MIN((int)fx, resolution - 2);
	const int iz = MIN((int)fz, resolution - 2);
	const float tx = fx - ix;
	const float tz = fz - iz;

	const float *row = heights + (size_t)iz * resolution + ix;
	const float h00 = row[0];
	const float h10 = row[1];
	const float h01 = row[resolution];
	const float h11 = row[resolution + 1];

	// The two triangles Landscape3D splits each quad into, split along the
	// same diagonal (from +X to +Z, see Landscape3D::_rebuild_chunk), rather
	// than a bilinear blend: bilinear bulges above one of them and sags below
	// the other by up to a quarter of the quad's twist, which is enough to
	// bury a road's edge or float it off the ground on uneven terrain.
	if (tx + tz <= 1.0f) {
		return h00 + (h10 - h00) * tx + (h01 - h00) * tz;
	}
	return h11 + (h01 - h11) * (1.0f - tx) + (h10 - h11) * (1.0f - tz);
}

Vector3 LandscapeSpline3D::TerrainSampler::project(const Vector3 &p_local, float p_offset) const {
	Vector3 on_terrain = to_terrain.xform(p_local);
	on_terrain.y = height_at(on_terrain.x, on_terrain.z) + p_offset;
	return from_terrain.xform(on_terrain);
}

bool LandscapeSpline3D::_make_terrain_sampler(TerrainSampler &r_sampler) const {
	return _make_terrain_sampler_for(_get_landscape(), r_sampler);
}

bool LandscapeSpline3D::_make_terrain_sampler_for(Landscape3D *p_landscape, TerrainSampler &r_sampler) const {
	Landscape3D *landscape = p_landscape;
	if (landscape == nullptr || !landscape->is_inside_tree() || !is_inside_tree()) {
		return false;
	}
	const Ref<TerrainData> terrain_data = landscape->get_terrain_data();
	if (terrain_data.is_null()) {
		return false;
	}
	const Ref<Image> heightmap = terrain_data->get_heightmap_image();
	if (heightmap.is_null()) {
		return false;
	}
	const int resolution = terrain_data->get_resolution();
	r_sampler.data = heightmap->get_data();
	if (resolution < 2 || r_sampler.data.size() < (int64_t)resolution * resolution * (int64_t)sizeof(float)) {
		return false;
	}
	r_sampler.heights = reinterpret_cast<const float *>(r_sampler.data.ptr());
	r_sampler.resolution = resolution;
	r_sampler.spacing = terrain_data->get_vertex_spacing();
	r_sampler.to_terrain = landscape->get_global_transform().affine_inverse() * get_global_transform();
	r_sampler.from_terrain = r_sampler.to_terrain.affine_inverse();
	return true;
}

void LandscapeSpline3D::_build_chunk(uint32_t p_index, BuildContext *p_context) {
	ChunkBuild &build = p_context->builds[p_index];
	const Chunk &chunk = chunks[build.chunk_index];
	const TerrainSampler &sampler = p_context->sampler;
	const bool use_terrain = p_context->use_terrain;

	const int column_count = columns.size();
	const int ring_count = chunk.last_ring - chunk.first_ring + 1;
	// One extra ring on each side, for the normals along the chunk's ends,
	// so neighboring chunks agree on them and no seam shows in the lighting.
	const int row_count = ring_count + 2;

	LocalVector<Vector3> grid;
	grid.resize(row_count * column_count);
	float widest = 0.0f;
	for (int row = 0; row < row_count; row++) {
		const Ring &ring = rings[_wrap_ring(chunk.first_ring + row - 1)];
		widest = MAX(widest, ring.half_width);

		Vector3 level_center = ring.center + rings_up * height_offset;
		if (use_terrain && height_mode == HEIGHT_MODE_CONFORM_LEVEL) {
			level_center = sampler.project(ring.center, height_offset);
		}
		for (int c = 0; c < column_count; c++) {
			const float lateral = (columns[c].u - 0.5f) * 2.0f * ring.half_width;
			Vector3 &position = grid[row * column_count + c];
			if (use_terrain && height_mode == HEIGHT_MODE_CONFORM) {
				position = sampler.project(ring.center + ring.flat_right * lateral, height_offset);
			} else {
				position = level_center + ring.right * lateral;
			}
		}
	}

	uint64_t hash = p_context->settings_hash;
	hash = hash_bytes(grid.ptr(), grid.size() * sizeof(Vector3), hash);
	for (int r = chunk.first_ring; r <= chunk.last_ring; r++) {
		hash = hash_float(rings[r].distance, hash);
		hash = hash_float(rings[r].end_alpha, hash);
	}
	build.hash = hash;
	if (chunk.built && chunk.hash == hash) {
		build.changed = false;
		return;
	}
	build.changed = true;

	if (use_terrain) {
		// What a terrain edit has to overlap to reach this chunk: every
		// vertex, padded by a sample, since each one reads the samples of the
		// quad it lands in.
		Vector2 min_xz(Math::INF, Math::INF);
		Vector2 max_xz(-Math::INF, -Math::INF);
		for (int row = 1; row < row_count - 1; row++) {
			for (int c = 0; c < column_count; c++) {
				const Vector3 on_terrain = sampler.to_terrain.xform(grid[row * column_count + c]);
				min_xz = min_xz.min(Vector2(on_terrain.x, on_terrain.z));
				max_xz = max_xz.max(Vector2(on_terrain.x, on_terrain.z));
			}
		}
		build.terrain_bounds = Rect2(min_xz, max_xz - min_xz).grow(sampler.spacing);
	}

	if (widest <= CMP_EPSILON || column_count < 2 || ring_count < 2) {
		build.empty = true;
		return;
	}
	build.empty = false;

	// Normals and tangents from the full-resolution grid, before anything is
	// simplified away, so they stay as smooth as the surface really is.
	LocalVector<Vector3> normals;
	LocalVector<Vector3> tangents;
	normals.resize(ring_count * column_count);
	tangents.resize(ring_count * column_count);
	for (int r = 0; r < ring_count; r++) {
		const int row = r + 1;
		const Ring &ring = rings[chunk.first_ring + r];
		for (int c = 0; c < column_count; c++) {
			const Vector3 across = grid[row * column_count + MIN(c + 1, column_count - 1)] - grid[row * column_count + MAX(c - 1, 0)];
			const Vector3 along = grid[(row + 1) * column_count + c] - grid[(row - 1) * column_count + c];
			Vector3 normal = across.cross(along);
			normal = normal.length_squared() > CMP_EPSILON2 ? normal.normalized() : rings_up;
			Vector3 tangent = across - normal * normal.dot(across);
			tangent = tangent.length_squared() > CMP_EPSILON2 ? tangent.normalized() : ring.right;
			normals[r * column_count + c] = normal;
			tangents[r * column_count + c] = tangent;
		}
	}

	auto point = [&](int p_ring, int p_column) -> const Vector3 & {
		return grid[(p_ring + 1) * column_count + p_column];
	};
	auto distance_of = [&](int p_ring) {
		return rings[chunk.first_ring + p_ring].distance;
	};
	// Must survive simplification and every LOD level: the chunk's ends
	// (where it meets its neighbors) and wherever the end fade is ramping,
	// which a longer span could not interpolate.
	auto ring_is_feature = [&](int p_ring) {
		if (p_ring == 0 || p_ring == ring_count - 1) {
			return true;
		}
		const int r = chunk.first_ring + p_ring;
		return rings[r].end_alpha < 1.0f || rings[r - 1].end_alpha < 1.0f || rings[r + 1].end_alpha < 1.0f;
	};

	const float tolerance_squared = simplify_tolerance * simplify_tolerance;

	// Drop cross-sections the neighbors on either side already describe: a
	// ring can go when every one of its vertices lies within
	// simplify_tolerance of the straight line between the matching vertices
	// of the kept rings around it. Greedy from the start, extending each span
	// for as long as everything inside it still fits.
	auto rings_fit = [&](int p_from, int p_to) {
		const float span = distance_of(p_to) - distance_of(p_from);
		if (span <= CMP_EPSILON) {
			return true;
		}
		for (int k = p_from + 1; k < p_to; k++) {
			const float t = (distance_of(k) - distance_of(p_from)) / span;
			for (int c = 0; c < column_count; c++) {
				const Vector3 expected = point(p_from, c).lerp(point(p_to, c), t);
				if (point(k, c).distance_squared_to(expected) > tolerance_squared) {
					return false;
				}
			}
		}
		return true;
	};
	LocalVector<int> kept_rings;
	kept_rings.push_back(0);
	for (int from = 0; from < ring_count - 1;) {
		int to = from + 1;
		while (to + 1 < ring_count && to + 1 - from <= MAX_RING_SPAN && !ring_is_feature(to) && rings_fit(from, to + 1)) {
			to++;
		}
		kept_rings.push_back(to);
		from = to;
	}

	// The same across the width, over the rings that were kept: a column can
	// go when it is within tolerance of the line between its kept neighbors
	// on every ring. On level ground, or in the level height modes, that is
	// every column but the edges and the fade.
	auto columns_fit = [&](int p_from, int p_to) {
		const float span = columns[p_to].u - columns[p_from].u;
		for (int k = p_from + 1; k < p_to; k++) {
			const float t = (columns[k].u - columns[p_from].u) / span;
			for (const int r : kept_rings) {
				const Vector3 expected = point(r, p_from).lerp(point(r, p_to), t);
				if (point(r, k).distance_squared_to(expected) > tolerance_squared) {
					return false;
				}
			}
		}
		return true;
	};
	LocalVector<int> kept_columns;
	kept_columns.push_back(0);
	for (int from = 0; from < column_count - 1;) {
		int to = from + 1;
		while (to + 1 < column_count && !columns[to].feature && columns_fit(from, to + 1)) {
			to++;
		}
		kept_columns.push_back(to);
		from = to;
	}

	const int mesh_rings = kept_rings.size();
	const int mesh_columns = kept_columns.size();
	const int vertex_count = mesh_rings * mesh_columns;

	PackedVector3Array positions;
	PackedVector3Array vertex_normals;
	PackedFloat32Array vertex_tangents;
	PackedVector2Array uvs;
	PackedColorArray colors;
	positions.resize(vertex_count);
	vertex_normals.resize(vertex_count);
	vertex_tangents.resize(vertex_count * 4);
	uvs.resize(vertex_count);
	colors.resize(vertex_count);
	Vector3 *positions_w = positions.ptrw();
	Vector3 *normals_w = vertex_normals.ptrw();
	float *tangents_w = vertex_tangents.ptrw();
	Vector2 *uvs_w = uvs.ptrw();
	Color *colors_w = colors.ptrw();

	const float v_scale = uv_scale.y / MAX(width, 0.001f);
	for (int ri = 0; ri < mesh_rings; ri++) {
		const int r = kept_rings[ri];
		const Ring &ring = rings[chunk.first_ring + r];
		const float v = ring.distance * v_scale;
		for (int ci = 0; ci < mesh_columns; ci++) {
			const int c = kept_columns[ci];
			const int vi = ri * mesh_columns + ci;
			positions_w[vi] = point(r, c);
			normals_w[vi] = normals[r * column_count + c];
			const Vector3 &tangent = tangents[r * column_count + c];
			tangents_w[vi * 4 + 0] = tangent.x;
			tangents_w[vi * 4 + 1] = tangent.y;
			tangents_w[vi * 4 + 2] = tangent.z;
			// Godot's binormal (normal x tangent, times this sign) points the
			// way V decreases (see PlaneMesh). Here normal x tangent points
			// along the spline, the way V increases, hence the flip.
			tangents_w[vi * 4 + 3] = -1.0f;
			uvs_w[vi] = Vector2(columns[c].u * uv_scale.x, v);
			colors_w[vi] = Color(1, 1, 1, columns[c].alpha * ring.end_alpha);
		}
	}

	// Two triangles per quad, clockwise seen from above (Godot's front face):
	// a ring's vertices run left to right, and the next ring lies ahead.
	auto build_indices = [&](const LocalVector<int> &p_rows, const LocalVector<int> &p_columns) {
		PackedInt32Array indices;
		indices.resize((p_rows.size() - 1) * (p_columns.size() - 1) * 6);
		int32_t *w = indices.ptrw();
		int i = 0;
		for (uint32_t a = 0; a + 1 < p_rows.size(); a++) {
			for (uint32_t b = 0; b + 1 < p_columns.size(); b++) {
				const int v00 = p_rows[a] * mesh_columns + p_columns[b];
				const int v01 = p_rows[a] * mesh_columns + p_columns[b + 1];
				const int v10 = p_rows[a + 1] * mesh_columns + p_columns[b];
				const int v11 = p_rows[a + 1] * mesh_columns + p_columns[b + 1];
				w[i++] = v00;
				w[i++] = v10;
				w[i++] = v01;
				w[i++] = v10;
				w[i++] = v11;
				w[i++] = v01;
			}
		}
		return indices;
	};

	LocalVector<int> all_rows;
	for (int ri = 0; ri < mesh_rings; ri++) {
		all_rows.push_back(ri);
	}
	LocalVector<int> all_columns;
	for (int ci = 0; ci < mesh_columns; ci++) {
		all_columns.push_back(ci);
	}

	// LOD levels keep every 2nd, 4th, 8th... remaining ring and column (and
	// every feature), all indexing the same vertices. Each level's threshold
	// is how far its surface strays from the full one at worst, measured on
	// every full-detail vertex against the coarse quad it falls in - the
	// world-space error the renderer weighs against the screen to choose a
	// level, the same measure ImporterMesh's generated LODs carry.
	auto measure_error = [&](const LocalVector<int> &p_rows, const LocalVector<int> &p_columns) {
		float error = 0.0f;
		for (uint32_t a = 0; a + 1 < p_rows.size(); a++) {
			const int r0 = p_rows[a];
			const int r1 = p_rows[a + 1];
			const float d0 = distance_of(kept_rings[r0]);
			const float d_span = MAX(distance_of(kept_rings[r1]) - d0, CMP_EPSILON);
			for (uint32_t b = 0; b + 1 < p_columns.size(); b++) {
				const int c0 = p_columns[b];
				const int c1 = p_columns[b + 1];
				const float u0 = columns[kept_columns[c0]].u;
				const float u_span = MAX(columns[kept_columns[c1]].u - u0, CMP_EPSILON);
				const Vector3 &p00 = positions_w[r0 * mesh_columns + c0];
				const Vector3 &p01 = positions_w[r0 * mesh_columns + c1];
				const Vector3 &p10 = positions_w[r1 * mesh_columns + c0];
				const Vector3 &p11 = positions_w[r1 * mesh_columns + c1];
				for (int ri = r0; ri <= r1; ri++) {
					const float t = (distance_of(kept_rings[ri]) - d0) / d_span;
					const Vector3 left = p00.lerp(p10, t);
					const Vector3 right = p01.lerp(p11, t);
					for (int ci = c0; ci <= c1; ci++) {
						const float s = (columns[kept_columns[ci]].u - u0) / u_span;
						error = MAX(error, positions_w[ri * mesh_columns + ci].distance_to(left.lerp(right, s)));
					}
				}
			}
		}
		return error;
	};

	uint32_t previous_row_count = all_rows.size();
	uint32_t previous_column_count = all_columns.size();
	float previous_error = 0.0f;
	for (int level = 1; level < 16; level++) {
		const int stride = 1 << level;
		LocalVector<int> rows;
		for (int ri = 0; ri < mesh_rings; ri++) {
			if (ri % stride == 0 || ri == mesh_rings - 1 || ring_is_feature(kept_rings[ri])) {
				rows.push_back(ri);
			}
		}
		LocalVector<int> cols;
		for (int ci = 0; ci < mesh_columns; ci++) {
			if (ci % stride == 0 || ci == mesh_columns - 1 || columns[kept_columns[ci]].feature) {
				cols.push_back(ci);
			}
		}
		if (rows.size() == previous_row_count && cols.size() == previous_column_count) {
			break;
		}
		// Strictly increasing, since the thresholds are the keys the levels
		// are stored and ordered under.
		const float error = MAX(measure_error(rows, cols), previous_error * 1.01f + 0.00001f);
		build.lods[error] = build_indices(rows, cols);
		previous_error = error;
		previous_row_count = rows.size();
		previous_column_count = cols.size();
		if (rows.size() <= 2 && cols.size() <= 2) {
			break;
		}
	}

	build.arrays.resize(RSE::ARRAY_MAX);
	build.arrays[RSE::ARRAY_VERTEX] = positions;
	build.arrays[RSE::ARRAY_NORMAL] = vertex_normals;
	build.arrays[RSE::ARRAY_TANGENT] = vertex_tangents;
	build.arrays[RSE::ARRAY_TEX_UV] = uvs;
	build.arrays[RSE::ARRAY_COLOR] = colors;
	build.arrays[RSE::ARRAY_INDEX] = build_indices(all_rows, all_columns);
}

void LandscapeSpline3D::_commit_chunk(Chunk &p_chunk, const ChunkBuild &p_build) {
	p_chunk.hash = p_build.hash;
	p_chunk.built = true;
	p_chunk.terrain_bounds = p_build.terrain_bounds;

	if (p_build.empty) {
		_free_chunk(p_chunk);
		p_chunk.built = true;
		p_chunk.hash = p_build.hash;
		return;
	}

	if (p_chunk.mesh.is_valid()) {
		RS::get_singleton()->mesh_clear(p_chunk.mesh);
	} else {
		p_chunk.mesh = RS::get_singleton()->mesh_create();
	}
	// Compressed: half the vertex memory and bandwidth. Positions quantize to
	// 16 bits across the chunk's own bounds, well under a millimeter over a
	// default chunk_length.
	RS::get_singleton()->mesh_add_surface_from_arrays(p_chunk.mesh, RSE::PRIMITIVE_TRIANGLES, p_build.arrays, Array(), p_build.lods, RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES);
	RS::get_singleton()->mesh_surface_set_material(p_chunk.mesh, 0, _get_material_rid());

	if (!p_chunk.instance.is_valid()) {
		p_chunk.instance = RS::get_singleton()->instance_create2(p_chunk.mesh, _get_scenario());
		RS::get_singleton()->instance_set_transform(p_chunk.instance, is_inside_tree() ? get_global_transform() : Transform3D());
		_apply_render_settings(p_chunk);
	}
}

void LandscapeSpline3D::_free_chunk(Chunk &p_chunk) {
	if (p_chunk.instance.is_valid()) {
		RS::get_singleton()->free_rid(p_chunk.instance);
		p_chunk.instance = RID();
	}
	if (p_chunk.mesh.is_valid()) {
		RS::get_singleton()->free_rid(p_chunk.mesh);
		p_chunk.mesh = RID();
	}
	p_chunk.built = false;
	p_chunk.hash = 0;
}

void LandscapeSpline3D::_clear_chunks() {
	for (Chunk &chunk : chunks) {
		_free_chunk(chunk);
	}
	chunks.clear();
}

RID LandscapeSpline3D::_get_scenario() const {
	if (is_inside_tree() && get_world_3d().is_valid()) {
		return get_world_3d()->get_scenario();
	}
	return RID();
}

RID LandscapeSpline3D::_get_material_rid() const {
	if (material.is_valid()) {
		return material->get_rid();
	}
	const Ref<Material> fallback = get_default_material(spline_type);
	return fallback.is_valid() ? fallback->get_rid() : RID();
}

void LandscapeSpline3D::_apply_material_to_all() {
	const RID material_rid = _get_material_rid();
	for (const Chunk &chunk : chunks) {
		if (chunk.mesh.is_valid()) {
			RS::get_singleton()->mesh_surface_set_material(chunk.mesh, 0, material_rid);
		}
	}
}

void LandscapeSpline3D::_apply_render_settings(const Chunk &p_chunk) const {
	if (!p_chunk.instance.is_valid()) {
		return;
	}
	RenderingServer *rs = RS::get_singleton();
	rs->instance_geometry_set_cast_shadows_setting(p_chunk.instance, (RSE::ShadowCastingSetting)cast_shadow);
	rs->instance_geometry_set_flag(p_chunk.instance, RSE::INSTANCE_FLAG_USE_BAKED_LIGHT, gi_mode == GeometryInstance3D::GI_MODE_STATIC);
	rs->instance_geometry_set_flag(p_chunk.instance, RSE::INSTANCE_FLAG_USE_DYNAMIC_GI, gi_mode == GeometryInstance3D::GI_MODE_DYNAMIC);
	rs->instance_set_layer_mask(p_chunk.instance, render_layers);
	rs->instance_geometry_set_lod_bias(p_chunk.instance, lod_bias);
	const float range_end = MAX(visibility_range_end, 0.0f);
	rs->instance_geometry_set_visibility_range(p_chunk.instance, 0.0f, range_end, 0.0f, range_end > 0.0f ? visibility_range_end_margin : 0.0f,
			(range_end > 0.0f && visibility_range_end_margin > 0.0f) ? RSE::VISIBILITY_RANGE_FADE_SELF : RSE::VISIBILITY_RANGE_FADE_DISABLED);
	rs->instance_set_visible(p_chunk.instance, is_visible_in_tree());
}

void LandscapeSpline3D::_apply_render_settings_to_all() {
	for (const Chunk &chunk : chunks) {
		_apply_render_settings(chunk);
	}
}

// The landscape.

Landscape3D *LandscapeSpline3D::_find_landscape() const {
	if (!is_inside_tree()) {
		return nullptr;
	}
	if (!landscape_path.is_empty()) {
		return Object::cast_to<Landscape3D>(get_node_or_null(landscape_path));
	}
	// The usual layouts: the spline under the landscape, or both under a
	// common parent (possibly with the spline grouped a few levels further
	// down, like World/Roads/Main Road next to World/Landscape3D).
	for (Node *ancestor = get_parent(); ancestor != nullptr; ancestor = ancestor->get_parent()) {
		if (Landscape3D *landscape = Object::cast_to<Landscape3D>(ancestor)) {
			return landscape;
		}
		for (int i = 0; i < ancestor->get_child_count(); i++) {
			if (Landscape3D *landscape = Object::cast_to<Landscape3D>(ancestor->get_child(i))) {
				return landscape;
			}
		}
	}
	return nullptr;
}

Landscape3D *LandscapeSpline3D::_get_landscape() const {
	return Object::cast_to<Landscape3D>(ObjectDB::get_instance(landscape_id));
}

Landscape3D *LandscapeSpline3D::get_landscape() const {
	return _find_landscape();
}

void LandscapeSpline3D::_update_landscape_connection() {
	if (landscape_path.is_empty()) {
		// Searching the tree walks every ancestor's children, so a landscape
		// already found is kept while it stays in the tree. Leaving the tree
		// (being moved elsewhere in it included) forgets it, see
		// NOTIFICATION_EXIT_TREE, and a new landscape_path does as well.
		const Landscape3D *current = _get_landscape();
		if (current != nullptr && current->is_inside_tree()) {
			return;
		}
	}
	Landscape3D *landscape = _find_landscape();
	const ObjectID id = landscape != nullptr ? landscape->get_instance_id() : ObjectID();
	if (id == landscape_id) {
		return;
	}
	_disconnect_landscape();
	landscape_id = id;
	if (landscape != nullptr) {
		landscape->connect(SNAME("terrain_changed"), callable_mp(this, &LandscapeSpline3D::_on_terrain_changed));
	}
	// A different up axis, and different ground under every chunk.
	pending_update |= UPDATE_RINGS;
	update_configuration_warnings();
}

void LandscapeSpline3D::_disconnect_landscape() {
	Landscape3D *landscape = _get_landscape();
	if (landscape != nullptr && landscape->is_connected(SNAME("terrain_changed"), callable_mp(this, &LandscapeSpline3D::_on_terrain_changed))) {
		landscape->disconnect(SNAME("terrain_changed"), callable_mp(this, &LandscapeSpline3D::_on_terrain_changed));
	}
	landscape_id = ObjectID();
}

void LandscapeSpline3D::_on_terrain_changed(const Rect2i &p_region) {
	if (!_get_local_up().is_equal_approx(rings_up)) {
		// The landscape turned under the spline.
		_queue_update(UPDATE_RINGS);
		return;
	}
	if (height_mode == HEIGHT_MODE_SPLINE) {
		// Nothing here is built from the terrain.
		return;
	}
	const Landscape3D *landscape = _get_landscape();
	if (landscape == nullptr || landscape->get_terrain_data().is_null()) {
		return;
	}
	const float spacing = landscape->get_terrain_data()->get_vertex_spacing();
	const Rect2 changed = Rect2(Vector2(p_region.position) * spacing, Vector2(p_region.size) * spacing).grow(spacing);

	bool any = false;
	for (Chunk &chunk : chunks) {
		if (!chunk.built || chunk.terrain_bounds.intersects(changed, true)) {
			chunk.dirty = true;
			any = true;
		}
	}
	if (any) {
		_queue_update(UPDATE_CHUNKS);
	}
}

Vector3 LandscapeSpline3D::project_to_landscape(const Vector3 &p_local_position) const {
	TerrainSampler sampler;
	if (!_make_terrain_sampler_for(_find_landscape(), sampler)) {
		return p_local_position;
	}
	return sampler.project(p_local_position, 0.0f);
}

// Shaping the landscape.

void LandscapeSpline3D::_compute_footprint(Landscape3D *p_landscape, LocalVector<RingOnTerrain> &r_rings, LocalVector<FootprintBlock> &r_blocks) const {
	r_rings.clear();
	r_blocks.clear();
	const Ref<TerrainData> terrain_data = p_landscape->get_terrain_data();
	if (terrain_data.is_null() || rings.size() < 2) {
		return;
	}
	const int resolution = terrain_data->get_resolution();
	const float spacing = terrain_data->get_vertex_spacing();
	const Transform3D to_terrain = p_landscape->get_global_transform().affine_inverse() * get_global_transform();

	const bool paint = paint_layer >= 0 && paint_layer < p_landscape->get_layers().size() && paint_strength > 0.0f;
	const float falloff = MAX(carve_enabled ? _get_carve_shoulder(p_landscape) + carve_falloff : 0.0f, paint ? paint_falloff : 0.0f);

	r_rings.resize(rings.size());
	for (uint32_t i = 0; i < rings.size(); i++) {
		const Ring &ring = rings[i];
		RingOnTerrain &on_terrain = r_rings[i];
		const Vector3 center = to_terrain.xform(ring.center);
		const Vector3 side = to_terrain.basis.xform(ring.flat_right);
		const Vector3 banked = to_terrain.basis.xform(ring.right);
		const Vector2 side_xz(side.x, side.z);
		const float banked_xz = Vector2(banked.x, banked.z).length();

		on_terrain.center = Vector2(center.x, center.z);
		on_terrain.height = center.y;
		on_terrain.side = side_xz.length_squared() > CMP_EPSILON2 ? side_xz.normalized() : Vector2(1, 0);
		on_terrain.half_width = ring.half_width * side_xz.length();
		on_terrain.bank = banked_xz > CMP_EPSILON ? banked.y / banked_xz : 0.0f;
		on_terrain.reach = on_terrain.half_width + falloff;
	}

	HashMap<Vector2i, uint32_t> block_indices;
	const int block_size = Landscape3D::CHUNK_QUADS;
	for (uint32_t i = 0; i + 1 < r_rings.size(); i++) {
		const RingOnTerrain &a = r_rings[i];
		const RingOnTerrain &b = r_rings[i + 1];
		const float reach = MAX(a.reach, b.reach);
		const Vector2 lo = a.center.min(b.center) - Vector2(reach, reach);
		const Vector2 hi = a.center.max(b.center) + Vector2(reach, reach);
		const int x0 = CLAMP((int)Math::floor(lo.x / spacing), 0, resolution - 1);
		const int z0 = CLAMP((int)Math::floor(lo.y / spacing), 0, resolution - 1);
		const int x1 = CLAMP((int)Math::ceil(hi.x / spacing), 0, resolution - 1);
		const int z1 = CLAMP((int)Math::ceil(hi.y / spacing), 0, resolution - 1);
		if (hi.x < 0.0f || hi.y < 0.0f || lo.x > (resolution - 1) * spacing || lo.y > (resolution - 1) * spacing) {
			continue;
		}

		const Vector2 segment = b.center - a.center;
		const float length_squared = segment.length_squared();
		for (int z = z0; z <= z1; z++) {
			for (int x = x0; x <= x1; x++) {
				const Vector2 p(x * spacing, z * spacing);
				const float t = length_squared > CMP_EPSILON2 ? CLAMP((p - a.center).dot(segment) / length_squared, 0.0f, 1.0f) : 0.0f;
				const Vector2 closest = a.center + segment * t;
				const float distance = p.distance_to(closest);
				if (distance > Math::lerp(a.reach, b.reach, t)) {
					continue;
				}

				const Vector2i block_coord(x / block_size, z / block_size);
				uint32_t block_index;
				if (const uint32_t *existing = block_indices.getptr(block_coord)) {
					block_index = *existing;
				} else {
					block_index = r_blocks.size();
					block_indices.insert(block_coord, block_index);
					FootprintBlock block;
					block.region.position = block_coord * block_size;
					block.region.size = Vector2i(MIN(block_size, resolution - block.region.position.x), MIN(block_size, resolution - block.region.position.y));
					block.samples.resize(block.region.size.x * block.region.size.y);
					r_blocks.push_back(block);
				}

				FootprintBlock &block = r_blocks[block_index];
				FootprintSample &sample = block.samples[(z - block.region.position.y) * block.region.size.x + (x - block.region.position.x)];
				if (distance < sample.distance) {
					const Vector2 side = a.side.lerp(b.side, t);
					sample.distance = distance;
					sample.lateral = (p - closest).dot(side) >= 0.0f ? distance : -distance;
					sample.along = i + t;
				}
			}
		}
	}
}

float LandscapeSpline3D::_get_carve_shoulder(const Landscape3D *p_landscape) const {
	const Ref<TerrainData> terrain_data = p_landscape->get_terrain_data();
	return terrain_data.is_valid() ? terrain_data->get_vertex_spacing() * 1.5f : 0.0f;
}

TypedArray<Rect2i> LandscapeSpline3D::get_landscape_footprint() const {
	TypedArray<Rect2i> regions;
	Landscape3D *landscape = get_landscape();
	if (landscape == nullptr) {
		return regions;
	}
	LocalVector<RingOnTerrain> terrain_rings;
	LocalVector<FootprintBlock> blocks;
	_compute_footprint(landscape, terrain_rings, blocks);
	for (const FootprintBlock &block : blocks) {
		regions.push_back(block.region);
	}
	return regions;
}

void LandscapeSpline3D::apply_to_landscape() {
	ERR_FAIL_COND_MSG(!is_inside_tree(), "A LandscapeSpline3D must be inside the scene tree to apply itself to a landscape.");
	Landscape3D *landscape = get_landscape();
	ERR_FAIL_NULL_MSG(landscape, "No Landscape3D to apply this spline to. Set landscape_path, or place the spline under the landscape or next to it.");
	ERR_FAIL_COND_MSG(landscape->get_terrain_data().is_null(), "The Landscape3D has no TerrainData to apply this spline to.");

	// The rings have to describe the curve as it is now, not as of the last
	// frame's update.
	if (pending_update & UPDATE_RINGS) {
		_update();
	}

	const bool paint = paint_layer >= 0 && paint_layer < landscape->get_layers().size() && paint_strength > 0.0f;
	if (!carve_enabled && !paint) {
		return;
	}

	LocalVector<RingOnTerrain> terrain_rings;
	LocalVector<FootprintBlock> blocks;
	_compute_footprint(landscape, terrain_rings, blocks);
	if (blocks.is_empty()) {
		return;
	}
	TypedArray<Rect2i> regions;
	for (const FootprintBlock &block : blocks) {
		regions.push_back(block.region);
	}

	const int last_segment = terrain_rings.size() - 2;
	struct Placement {
		float surface = 0.0; // Design height at the centerline.
		float bank = 0.0;
		float half_width = 0.0;
	};
	auto place = [&](const FootprintSample &p_sample) {
		const int i = CLAMP((int)Math::floor(p_sample.along), 0, last_segment);
		const float t = CLAMP(p_sample.along - i, 0.0f, 1.0f);
		const RingOnTerrain &a = terrain_rings[i];
		const RingOnTerrain &b = terrain_rings[i + 1];
		Placement placement;
		placement.surface = Math::lerp(a.height, b.height, t);
		placement.bank = Math::lerp(a.bank, b.bank, t);
		placement.half_width = Math::lerp(a.half_width, b.half_width, t);
		return placement;
	};

	if (carve_enabled) {
		// Cut and fill to the curve's own height across the strip (following
		// its bank), carve_depth further down in the middle for a bed, then
		// blend back into the untouched ground over carve_falloff beyond the
		// edge. The strip's own height and not the terrain's, so what ends up
		// under the strip does not depend on what the ground was before, and
		// applying again leaves it as it is; only the falloff, being a blend
		// with whatever is there, eases a little further each time.
		//
		// The strip's own surface also carries on level past its edges for a
		// terrain sample and a half before the falloff starts. The terrain is
		// triangles between samples, so the ground under the strip's edge is
		// interpolated from samples up to that far outside it; were those
		// already easing back towards the old ground, a road draped over the
		// result would sag or lift along both edges, and keep far more of its
		// cross-sections through simplification than the level road it is.
		const float shoulder = _get_carve_shoulder(landscape);
		TypedArray<PackedFloat32Array> heights = landscape->get_height_regions(regions);
		for (uint32_t b = 0; b < blocks.size(); b++) {
			PackedFloat32Array block_heights = heights[b];
			float *h = block_heights.ptrw();
			const LocalVector<FootprintSample> &samples = blocks[b].samples;
			for (uint32_t s = 0; s < samples.size(); s++) {
				const FootprintSample &sample = samples[s];
				if (sample.distance == Math::INF) {
					continue;
				}
				const Placement placement = place(sample);
				const float d = sample.distance;
				float target;
				float blend;
				if (d <= placement.half_width && placement.half_width > CMP_EPSILON) {
					target = placement.surface + placement.bank * sample.lateral - carve_depth * bed_profile(d / placement.half_width);
					blend = 1.0f;
				} else {
					const float edge = SIGN(sample.lateral) * placement.half_width;
					target = placement.surface + placement.bank * edge;
					const float beyond = d - placement.half_width - shoulder;
					if (beyond <= 0.0f) {
						blend = 1.0f;
					} else {
						blend = carve_falloff > 0.0f ? 1.0f - smoothstep01(beyond / carve_falloff) : 0.0f;
					}
				}
				if (blend > 0.0f) {
					h[s] = Math::lerp(h[s], target, blend);
				}
			}
			heights[b] = block_heights;
		}
		landscape->set_height_regions(regions, heights, true);
	}

	if (paint) {
		TypedArray<PackedFloat32Array> weights;
		for (const FootprintBlock &block : blocks) {
			PackedFloat32Array block_weights;
			block_weights.resize(block.samples.size());
			float *w = block_weights.ptrw();
			for (uint32_t s = 0; s < block.samples.size(); s++) {
				const FootprintSample &sample = block.samples[s];
				w[s] = 0.0f;
				if (sample.distance == Math::INF) {
					continue;
				}
				const Placement placement = place(sample);
				const float beyond = sample.distance - placement.half_width;
				float amount = 1.0f;
				if (beyond > 0.0f) {
					amount = paint_falloff > 0.0f ? 1.0f - smoothstep01(beyond / paint_falloff) : 0.0f;
				}
				w[s] = amount * paint_strength;
			}
			weights.push_back(block_weights);
		}
		landscape->paint_layer_regions(regions, paint_layer, weights);
	}
}

// Properties.

void LandscapeSpline3D::set_spline_type(SplineType p_type) {
	ERR_FAIL_INDEX(p_type, TYPE_MAX);
	spline_type = p_type;
	_apply_material_to_all();
}

LandscapeSpline3D::SplineType LandscapeSpline3D::get_spline_type() const {
	return spline_type;
}

void LandscapeSpline3D::set_landscape_path(const NodePath &p_path) {
	landscape_path = p_path;
	_disconnect_landscape();
	_queue_update(UPDATE_RINGS);
	update_configuration_warnings();
}

NodePath LandscapeSpline3D::get_landscape_path() const {
	return landscape_path;
}

void LandscapeSpline3D::set_material(const Ref<Material> &p_material) {
	material = p_material;
	_apply_material_to_all();
}

Ref<Material> LandscapeSpline3D::get_material() const {
	return material;
}

void LandscapeSpline3D::set_width(float p_width) {
	width = MAX(p_width, 0.0f);
	_queue_update(UPDATE_RINGS);
}

float LandscapeSpline3D::get_width() const {
	return width;
}

void LandscapeSpline3D::_on_shape_changed() {
	_queue_update(UPDATE_RINGS);
}

void LandscapeSpline3D::_on_curve_changed() {
	_queue_update(UPDATE_RINGS);
	update_configuration_warnings();
}

void LandscapeSpline3D::set_width_curve(const Ref<Curve> &p_curve) {
	if (width_curve.is_valid()) {
		width_curve->disconnect_changed(callable_mp(this, &LandscapeSpline3D::_on_shape_changed));
	}
	width_curve = p_curve;
	if (width_curve.is_valid()) {
		width_curve->connect_changed(callable_mp(this, &LandscapeSpline3D::_on_shape_changed));
	}
	_queue_update(UPDATE_RINGS);
}

Ref<Curve> LandscapeSpline3D::get_width_curve() const {
	return width_curve;
}

void LandscapeSpline3D::set_height_mode(HeightMode p_mode) {
	ERR_FAIL_INDEX(p_mode, 3);
	height_mode = p_mode;
	_queue_update(UPDATE_ALL_CHUNKS);
	update_configuration_warnings();
}

LandscapeSpline3D::HeightMode LandscapeSpline3D::get_height_mode() const {
	return height_mode;
}

void LandscapeSpline3D::set_height_offset(float p_offset) {
	height_offset = p_offset;
	_queue_update(UPDATE_ALL_CHUNKS);
}

float LandscapeSpline3D::get_height_offset() const {
	return height_offset;
}

void LandscapeSpline3D::set_smooth(bool p_smooth) {
	smooth = p_smooth;
	_queue_update(UPDATE_RINGS);
}

bool LandscapeSpline3D::is_smooth() const {
	return smooth;
}

void LandscapeSpline3D::set_segment_length(float p_length) {
	segment_length = MAX(p_length, 0.05f);
	_queue_update(UPDATE_RINGS);
}

float LandscapeSpline3D::get_segment_length() const {
	return segment_length;
}

void LandscapeSpline3D::set_cross_segments(int p_segments) {
	cross_segments = CLAMP(p_segments, 1, MAX_CROSS_SEGMENTS);
	_queue_update(UPDATE_RINGS);
}

int LandscapeSpline3D::get_cross_segments() const {
	return cross_segments;
}

void LandscapeSpline3D::set_simplify_tolerance(float p_tolerance) {
	simplify_tolerance = MAX(p_tolerance, 0.0f);
	_queue_update(UPDATE_ALL_CHUNKS);
}

float LandscapeSpline3D::get_simplify_tolerance() const {
	return simplify_tolerance;
}

void LandscapeSpline3D::set_chunk_length(float p_length) {
	chunk_length = MAX(p_length, 1.0f);
	_queue_update(UPDATE_RINGS);
}

float LandscapeSpline3D::get_chunk_length() const {
	return chunk_length;
}

void LandscapeSpline3D::set_uv_scale(const Vector2 &p_scale) {
	uv_scale = p_scale;
	_queue_update(UPDATE_ALL_CHUNKS);
}

Vector2 LandscapeSpline3D::get_uv_scale() const {
	return uv_scale;
}

void LandscapeSpline3D::set_edge_fade(float p_fade) {
	edge_fade = CLAMP(p_fade, 0.0f, 0.5f);
	_queue_update(UPDATE_RINGS);
}

float LandscapeSpline3D::get_edge_fade() const {
	return edge_fade;
}

void LandscapeSpline3D::set_end_fade_length(float p_length) {
	end_fade_length = MAX(p_length, 0.0f);
	_queue_update(UPDATE_RINGS);
}

float LandscapeSpline3D::get_end_fade_length() const {
	return end_fade_length;
}

void LandscapeSpline3D::set_cast_shadow(GeometryInstance3D::ShadowCastingSetting p_setting) {
	cast_shadow = p_setting;
	_apply_render_settings_to_all();
}

GeometryInstance3D::ShadowCastingSetting LandscapeSpline3D::get_cast_shadow() const {
	return cast_shadow;
}

void LandscapeSpline3D::set_gi_mode(GeometryInstance3D::GIMode p_mode) {
	gi_mode = p_mode;
	_apply_render_settings_to_all();
}

GeometryInstance3D::GIMode LandscapeSpline3D::get_gi_mode() const {
	return gi_mode;
}

void LandscapeSpline3D::set_render_layers(uint32_t p_layers) {
	render_layers = p_layers;
	_apply_render_settings_to_all();
}

uint32_t LandscapeSpline3D::get_render_layers() const {
	return render_layers;
}

void LandscapeSpline3D::set_lod_bias(float p_bias) {
	lod_bias = MAX(p_bias, 0.001f);
	_apply_render_settings_to_all();
}

float LandscapeSpline3D::get_lod_bias() const {
	return lod_bias;
}

void LandscapeSpline3D::set_visibility_range_end(float p_distance) {
	visibility_range_end = MAX(p_distance, 0.0f);
	_apply_render_settings_to_all();
}

float LandscapeSpline3D::get_visibility_range_end() const {
	return visibility_range_end;
}

void LandscapeSpline3D::set_visibility_range_end_margin(float p_margin) {
	visibility_range_end_margin = MAX(p_margin, 0.0f);
	_apply_render_settings_to_all();
}

float LandscapeSpline3D::get_visibility_range_end_margin() const {
	return visibility_range_end_margin;
}

void LandscapeSpline3D::set_carve_enabled(bool p_enabled) {
	carve_enabled = p_enabled;
}

bool LandscapeSpline3D::is_carve_enabled() const {
	return carve_enabled;
}

void LandscapeSpline3D::set_carve_depth(float p_depth) {
	carve_depth = MAX(p_depth, 0.0f);
}

float LandscapeSpline3D::get_carve_depth() const {
	return carve_depth;
}

void LandscapeSpline3D::set_carve_falloff(float p_falloff) {
	carve_falloff = MAX(p_falloff, 0.0f);
}

float LandscapeSpline3D::get_carve_falloff() const {
	return carve_falloff;
}

void LandscapeSpline3D::set_paint_layer(int p_layer) {
	paint_layer = CLAMP(p_layer, -1, TerrainData::MAX_LAYERS - 1);
}

int LandscapeSpline3D::get_paint_layer() const {
	return paint_layer;
}

void LandscapeSpline3D::set_paint_strength(float p_strength) {
	paint_strength = CLAMP(p_strength, 0.0f, 1.0f);
}

float LandscapeSpline3D::get_paint_strength() const {
	return paint_strength;
}

void LandscapeSpline3D::set_paint_falloff(float p_falloff) {
	paint_falloff = MAX(p_falloff, 0.0f);
}

float LandscapeSpline3D::get_paint_falloff() const {
	return paint_falloff;
}

int LandscapeSpline3D::get_chunk_count() const {
	return chunks.size();
}

RID LandscapeSpline3D::get_chunk_mesh(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, (int)chunks.size(), RID());
	return chunks[p_index].mesh;
}

RID LandscapeSpline3D::get_chunk_instance(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, (int)chunks.size(), RID());
	return chunks[p_index].instance;
}

uint64_t LandscapeSpline3D::get_chunk_hash(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, (int)chunks.size(), 0);
	return chunks[p_index].hash;
}

PackedStringArray LandscapeSpline3D::get_configuration_warnings() const {
	PackedStringArray warnings = Path3D::get_configuration_warnings();

	const Ref<Curve3D> source_curve = get_curve();
	if (source_curve.is_null() || source_curve->get_point_count() < 2) {
		warnings.push_back(RTR("Add at least two points to the source_curve (with the Path3D tools in the 3D editor's toolbar) to lay this spline out."));
	}
	if (is_inside_tree() && height_mode != HEIGHT_MODE_SPLINE) {
		const Landscape3D *landscape = get_landscape();
		if (landscape == nullptr || landscape->get_terrain_data().is_null()) {
			warnings.push_back(RTR("No Landscape3D with TerrainData found to conform to, so the mesh follows the source_curve's own height. Set landscape_path, or place this node under the landscape or next to it."));
		}
	}

	return warnings;
}

LandscapeSpline3D::LandscapeSpline3D() {
	set_notify_transform(true);
	connect(SNAME("curve_changed"), callable_mp(this, &LandscapeSpline3D::_on_curve_changed));
}

LandscapeSpline3D::~LandscapeSpline3D() {
	_clear_chunks();
}
