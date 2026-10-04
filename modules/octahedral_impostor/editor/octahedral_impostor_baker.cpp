/**************************************************************************/
/*  octahedral_impostor_baker.cpp                                         */
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

#include "octahedral_impostor_baker.h"

#include "core/io/config_file.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/math/math_funcs_binary.h"
#include "core/object/class_db.h"
#include "core/object/worker_thread_pool.h"
#include "editor/editor_node.h"
#include "editor/file_system/editor_file_system.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/mesh.h"
#include "scene/resources/multimesh.h"
#include "scene/resources/shader.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/shader_language.h"
#include "servers/rendering/shader_types.h"

// Marks the meshes of the baked billboards, which aren't baked again with the node.
static const char *baked_billboard_meta = "_baked_billboard";

// Full screen pass drawn after the geometry (Forward+): the view space normal of the normal and
// roughness buffer (so it includes the normal maps and custom shaders of any material) and the
// depth of the depth buffer.
static const char *capture_shader_code = R"(
shader_type spatial;
render_mode unshaded, cull_disabled, depth_test_disabled, depth_draw_never, shadows_disabled, fog_disabled;

uniform sampler2D depth_texture : hint_depth_texture, filter_nearest, repeat_disable;
uniform sampler2D normal_roughness_texture : hint_normal_roughness_texture, filter_nearest, repeat_disable;
uniform float depth_near = 0.0;
uniform float depth_range = 1.0;

vec2 octahedral_encode(vec3 n) {
	n /= abs(n.x) + abs(n.y) + abs(n.z);
	vec2 coord = n.xz;
	if (n.y < 0.0) {
		coord = (1.0 - abs(n.zx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.z >= 0.0 ? 1.0 : -1.0);
	}
	return coord;
}

void vertex() {
	POSITION = vec4(VERTEX.xy * 2.0, 1.0, 1.0);
}

void fragment() {
	float depth = texture(depth_texture, SCREEN_UV).r;
	vec4 view = INV_PROJECTION_MATRIX * vec4(SCREEN_UV * 2.0 - 1.0, depth, 1.0);
	float view_depth = -view.z / view.w;
	vec3 normal = normalize(texture(normal_roughness_texture, SCREEN_UV).xyz * 2.0 - 1.0);
	ALBEDO = vec3(octahedral_encode(normal) * 0.5 + 0.5, clamp((view_depth - depth_near) / depth_range, 0.0, 1.0));
	ALPHA = 1.0;
}
)";

// Unshaded quad of a known value, tells whether the data written by unshaded materials is read
// back as is.
static const char *calibration_shader_code = R"(
shader_type spatial;
render_mode unshaded, cull_disabled, depth_test_disabled, depth_draw_never, shadows_disabled, fog_disabled;

void vertex() {
	POSITION = vec4(VERTEX.xy * 2.0, 1.0, 1.0);
}

void fragment() {
	ALBEDO = vec3(0.2);
}
)";

// Code added to the shaders of the materials of the geometry, and of the materials that replace
// them, to capture the data the renderer doesn't keep as their unshaded color (see
// OctahedralImpostorBaker::CaptureMode): the view space normal (octahedral) and the depth, the
// ambient occlusion, roughness and metallic, or the backlight.
static const char *capture_uniforms_code = R"(
// Impostor bake, see OctahedralImpostorBaker.
uniform int impostor_bake_mode = 0;
uniform float impostor_bake_depth_near = 0.0;
uniform float impostor_bake_depth_range = 1.0;
)";

static String _get_capture_code(bool p_normal_map) {
	String code = "	if (impostor_bake_mode == 0) {\n";
	if (p_normal_map) {
		// Like the renderer, after the fragment function.
		code += R"(		vec2 impostor_bake_map = NORMAL_MAP.xy * 2.0 - 1.0;
		vec3 impostor_bake_normal = normalize(mix(NORMAL, TANGENT * impostor_bake_map.x + BINORMAL * impostor_bake_map.y + NORMAL * sqrt(max(0.0, 1.0 - dot(impostor_bake_map, impostor_bake_map))), NORMAL_MAP_DEPTH));
)";
	} else {
		code += "		vec3 impostor_bake_normal = normalize(NORMAL);\n";
	}
	code += R"(		impostor_bake_normal /= abs(impostor_bake_normal.x) + abs(impostor_bake_normal.y) + abs(impostor_bake_normal.z);
		vec2 impostor_bake_coord = impostor_bake_normal.y >= 0.0 ? impostor_bake_normal.xz : (1.0 - abs(impostor_bake_normal.zx)) * vec2(impostor_bake_normal.x >= 0.0 ? 1.0 : -1.0, impostor_bake_normal.z >= 0.0 ? 1.0 : -1.0);
		ALBEDO = vec3(impostor_bake_coord * 0.5 + 0.5, clamp((-VERTEX.z - impostor_bake_depth_near) / impostor_bake_depth_range, 0.0, 1.0));
	} else if (impostor_bake_mode == 1) {
		ALBEDO = vec3(AO, ROUGHNESS, METALLIC);
	} else {
		ALBEDO = BACKLIGHT;
	}
)";
	return code;
}

// Replaces the materials whose shader can't capture its data: mimics the parameters of
// BaseMaterial3D, and the usual parameter names of the shaders.
static String _get_override_shader_code(const String &p_cull_mode) {
	String code = String(R"(
shader_type spatial;
render_mode unshaded, depth_draw_opaque, shadows_disabled, fog_disabled, )") +
			p_cull_mode + ";\n";
	code += capture_uniforms_code;
	code += R"(
uniform vec4 albedo = vec4(1.0);
uniform sampler2D texture_albedo : hint_default_white, filter_linear_mipmap;
uniform bool use_vertex_color = false;
uniform float alpha_scissor_threshold = -1.0;
uniform vec3 uv1_scale = vec3(1.0);
uniform vec3 uv1_offset = vec3(0.0);
uniform vec3 uv2_scale = vec3(1.0);
uniform vec3 uv2_offset = vec3(0.0);
uniform bool use_normal_map = false;
uniform sampler2D texture_normal : hint_default_black, filter_linear_mipmap;
uniform float normal_scale = 1.0;
uniform float roughness = 1.0;
uniform sampler2D texture_roughness : hint_default_white, filter_linear_mipmap;
uniform vec4 roughness_channel = vec4(1.0, 0.0, 0.0, 0.0);
uniform float metallic = 0.0;
uniform sampler2D texture_metallic : hint_default_white, filter_linear_mipmap;
uniform vec4 metallic_channel = vec4(1.0, 0.0, 0.0, 0.0);
uniform sampler2D texture_ambient_occlusion : hint_default_white, filter_linear_mipmap;
uniform vec4 ao_channel = vec4(1.0, 0.0, 0.0, 0.0);
uniform bool ao_on_uv2 = false;
uniform vec4 backlight : source_color = vec4(0.0, 0.0, 0.0, 1.0);
uniform sampler2D texture_backlight : hint_default_black, filter_linear_mipmap;

void vertex() {
	UV = UV * uv1_scale.xy + uv1_offset.xy;
	UV2 = UV2 * uv2_scale.xy + uv2_offset.xy;
}

void fragment() {
	float alpha = albedo.a * texture(texture_albedo, UV).a;
	if (use_vertex_color) {
		alpha *= COLOR.a;
	}
	if (alpha < alpha_scissor_threshold) {
		discard;
	}
	if (use_normal_map) {
		NORMAL_MAP = texture(texture_normal, UV).rgb;
		NORMAL_MAP_DEPTH = normal_scale;
	}
	AO = dot(texture(texture_ambient_occlusion, ao_on_uv2 ? UV2 : UV), ao_channel);
	ROUGHNESS = roughness * dot(texture(texture_roughness, UV), roughness_channel);
	METALLIC = metallic * dot(texture(texture_metallic, UV), metallic_channel);
	BACKLIGHT = backlight.rgb + texture(texture_backlight, UV).rgb;
)";
	code += _get_capture_code(true);
	code += "}\n";
	return code;
}

static ShaderLanguage::DataType _get_global_shader_uniform_type(const StringName &p_variable) {
	const RSE::GlobalShaderParameterType type = RS::get_singleton()->global_shader_parameter_get_type(p_variable);
	return ShaderLanguage::DataType(RS::global_shader_uniform_type_get_shader_datatype(type));
}

// Whether the code is a valid spatial shader, so that the renderer doesn't report the errors of the
// shaders that can't capture their data.
static bool _is_valid_spatial_shader(const String &p_code, String &r_error) {
	ShaderLanguage language;
	ShaderLanguage::ShaderCompileInfo info;
	info.functions = ShaderTypes::get_singleton()->get_functions(RSE::SHADER_SPATIAL);
	info.render_modes = ShaderTypes::get_singleton()->get_modes(RSE::SHADER_SPATIAL);
	info.stencil_modes = ShaderTypes::get_singleton()->get_stencil_modes(RSE::SHADER_SPATIAL);
	info.shader_types = ShaderTypes::get_singleton()->get_types();
	info.global_shader_uniform_type_func = _get_global_shader_uniform_type;
	if (language.compile(p_code, info) != OK) {
		r_error = vformat("line %d: %s", language.get_error_line(), language.get_error_text());
		return false;
	}
	return true;
}

static Vector4 _texture_channel_mask(BaseMaterial3D::TextureChannel p_channel) {
	switch (p_channel) {
		case BaseMaterial3D::TEXTURE_CHANNEL_RED:
			return Vector4(1, 0, 0, 0);
		case BaseMaterial3D::TEXTURE_CHANNEL_GREEN:
			return Vector4(0, 1, 0, 0);
		case BaseMaterial3D::TEXTURE_CHANNEL_BLUE:
			return Vector4(0, 0, 1, 0);
		case BaseMaterial3D::TEXTURE_CHANNEL_ALPHA:
			return Vector4(0, 0, 0, 1);
		default:
			return Vector4(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0, 0);
	}
}

// Value of the first uniform of the shader of the material with one of the names.
static Variant _find_shader_parameter(const Ref<ShaderMaterial> &p_material, const HashSet<StringName> &p_uniforms, std::initializer_list<const char *> p_names, Variant::Type p_type) {
	for (const char *name : p_names) {
		if (!p_uniforms.has(name)) {
			continue;
		}
		Variant value = p_material->get_shader_parameter(name);
		if (value.get_type() == Variant::NIL) {
			value = RS::get_singleton()->shader_get_parameter_default(p_material->get_shader()->get_rid(), name);
		}
		if (value.get_type() == p_type || (p_type == Variant::OBJECT && value.get_type() == Variant::NIL)) {
			return value;
		}
	}
	return Variant();
}

String OctahedralImpostorBaker::make_capture_shader_code(const String &p_code) {
	// Tokens of the code: identifiers (and numbers), and punctuation. The comments and strings are
	// skipped. The code of the shaders sent to the renderer is already preprocessed.
	struct Token {
		int from = 0;
		int to = 0; // Exclusive.
		String text;
	};
	LocalVector<Token> tokens;
	const char32_t *code = p_code.get_data();
	const int length = p_code.length();
	int position = 0;
	while (position < length) {
		const char32_t c = code[position];
		if (c == '/' && position + 1 < length && code[position + 1] == '/') {
			while (position < length && code[position] != '\n') {
				position++;
			}
		} else if (c == '/' && position + 1 < length && code[position + 1] == '*') {
			position += 2;
			while (position + 1 < length && !(code[position] == '*' && code[position + 1] == '/')) {
				position++;
			}
			position += 2;
		} else if (c == '"') {
			position++;
			while (position < length && code[position] != '"') {
				position += code[position] == '\\' ? 2 : 1;
			}
			position++;
		} else if (is_ascii_identifier_char(c)) {
			Token token;
			token.from = position;
			while (position < length && is_ascii_identifier_char(code[position])) {
				position++;
			}
			token.to = position;
			token.text = p_code.substr(token.from, token.to - token.from);
			tokens.push_back(token);
		} else if (is_whitespace(c)) {
			position++;
		} else {
			Token token;
			token.from = position;
			token.to = position + 1;
			token.text = String::chr(c);
			tokens.push_back(token);
			position++;
		}
	}

	// The top level statements and the body of the fragment function.
	int shader_type_end = -1;
	LocalVector<Pair<int, int>> removed_statements; // The render and stencil modes.
	HashSet<String> render_modes;
	int fragment_from = -1;
	int fragment_body_end = -1;
	HashSet<String> fragment_identifiers;
	int depth = 0;
	for (uint32_t i = 0; i < tokens.size(); i++) {
		const String &text = tokens[i].text;
		if (text == "{") {
			depth++;
			continue;
		}
		if (text == "}") {
			depth--;
			continue;
		}
		if (depth != 0) {
			continue;
		}
		if (text == "shader_type" || text == "render_mode" || text == "stencil_mode") {
			uint32_t end = i + 1;
			while (end < tokens.size() && tokens[end].text != ";") {
				if (text == "render_mode" && tokens[end].text != ",") {
					render_modes.insert(tokens[end].text);
				}
				end++;
			}
			if (end >= tokens.size()) {
				return String();
			}
			if (text == "shader_type") {
				shader_type_end = tokens[end].to;
			} else {
				removed_statements.push_back(Pair<int, int>(tokens[i].from, tokens[end].to));
			}
			i = end;
		} else if (text == "fragment" && i > 0 && tokens[i - 1].text == "void" && i + 3 < tokens.size() && tokens[i + 1].text == "(" && tokens[i + 2].text == ")" && tokens[i + 3].text == "{") {
			fragment_from = tokens[i - 1].from;
			int body_depth = 0;
			uint32_t end = i + 3;
			for (; end < tokens.size(); end++) {
				const String &body_text = tokens[end].text;
				if (body_text == "{") {
					body_depth++;
				} else if (body_text == "}") {
					body_depth--;
					if (body_depth == 0) {
						break;
					}
				} else {
					fragment_identifiers.insert(body_text);
				}
			}
			if (end >= tokens.size()) {
				return String();
			}
			fragment_body_end = tokens[end].from;
			i = end;
		}
	}
	// The data is written at the end of the fragment function.
	if (shader_type_end < 0 || fragment_identifiers.has("return")) {
		return String();
	}

	// Unshaded and opaque, transparent surfaces are alpha tested like in the albedo pass. The modes
	// that change the geometry are kept.
	static const char *kept_render_modes[] = { "cull_back", "cull_front", "cull_disabled", "skip_vertex_transform", "world_vertex_coords", "ensure_correct_normals", "particle_trails" };
	String render_mode = "\nrender_mode unshaded, depth_draw_opaque, shadows_disabled, fog_disabled";
	for (const char *mode : kept_render_modes) {
		if (render_modes.has(mode)) {
			render_mode += String(", ") + mode;
		}
	}
	render_mode += ";\n";

	String capture = "\n	// Impostor bake, see OctahedralImpostorBaker.\n";
	if (fragment_identifiers.has("ALPHA") && !fragment_identifiers.has("ALPHA_SCISSOR_THRESHOLD") && !fragment_identifiers.has("ALPHA_HASH_SCALE")) {
		capture += "	ALPHA_SCISSOR_THRESHOLD = 0.5;\n";
	}
	capture += _get_capture_code(fragment_identifiers.has("NORMAL_MAP"));

	// Edits from the end of the code, so that the positions of the previous ones stay valid.
	struct Edit {
		int from = 0;
		int to = 0;
		String text;
	};
	struct EditComparator {
		// At the same position, a removal before an insertion.
		bool operator()(const Edit &p_a, const Edit &p_b) const { return p_a.from != p_b.from ? p_a.from > p_b.from : p_a.to > p_b.to; }
	};
	LocalVector<Edit> edits;
	if (fragment_from >= 0) {
		edits.push_back({ fragment_body_end, fragment_body_end, capture });
		edits.push_back({ fragment_from, fragment_from, String(capture_uniforms_code) + "\n" });
	} else {
		edits.push_back({ length, length, String("\n") + capture_uniforms_code + "\nvoid fragment() {" + capture + "}\n" });
	}
	for (int i = removed_statements.size() - 1; i >= 0; i--) {
		edits.push_back({ removed_statements[i].first, removed_statements[i].second, String() });
	}
	edits.push_back({ shader_type_end, shader_type_end, render_mode });
	edits.sort_custom<EditComparator>();

	String result = p_code;
	for (const Edit &edit : edits) {
		result = result.substr(0, edit.from) + edit.text + result.substr(edit.to);
	}
	return result;
}

/* Settings */

void OctahedralImpostorBaker::set_type(Type p_type) {
	ERR_FAIL_INDEX(p_type, TYPE_MAX);
	type = p_type;
}

OctahedralImpostorBaker::Type OctahedralImpostorBaker::get_type() const {
	return type;
}

void OctahedralImpostorBaker::set_layout(OctahedralImpostorMaterial3D::Layout p_layout) {
	ERR_FAIL_INDEX(p_layout, OctahedralImpostorMaterial3D::LAYOUT_MAX);
	layout = p_layout;
}

OctahedralImpostorMaterial3D::Layout OctahedralImpostorBaker::get_layout() const {
	return layout;
}

void OctahedralImpostorBaker::set_frames(int p_frames) {
	frames = CLAMP(p_frames, OctahedralImpostorMaterial3D::MIN_FRAMES, OctahedralImpostorMaterial3D::MAX_FRAMES);
}

int OctahedralImpostorBaker::get_frames() const {
	return frames;
}

void OctahedralImpostorBaker::set_billboard_mode(BillboardMode p_mode) {
	ERR_FAIL_INDEX(p_mode, BILLBOARD_MAX);
	billboard_mode = p_mode;
}

OctahedralImpostorBaker::BillboardMode OctahedralImpostorBaker::get_billboard_mode() const {
	return billboard_mode;
}

void OctahedralImpostorBaker::set_cross_planes(int p_planes) {
	cross_planes = CLAMP(p_planes, MIN_CROSS_PLANES, MAX_CROSS_PLANES);
}

int OctahedralImpostorBaker::get_cross_planes() const {
	return cross_planes;
}

// The closest power of two, the textures are powers of two.
static int _round_to_power_of_2(int p_size) {
	int size = MAX(p_size, 1);
	const int upper = int(Math::next_power_of_2(uint32_t(size)));
	return upper - size > size - upper / 2 ? upper / 2 : upper;
}

void OctahedralImpostorBaker::set_atlas_size(int p_size) {
	atlas_size = CLAMP(_round_to_power_of_2(p_size), MIN_ATLAS_SIZE, MAX_ATLAS_SIZE);
}

int OctahedralImpostorBaker::get_atlas_size() const {
	return atlas_size;
}

void OctahedralImpostorBaker::set_supersampling(int p_factor) {
	supersampling = CLAMP(p_factor, 1, MAX_SUPERSAMPLING);
}

int OctahedralImpostorBaker::get_supersampling() const {
	return supersampling;
}

void OctahedralImpostorBaker::set_bake_orm(bool p_enable) {
	bake_orm = p_enable;
}

bool OctahedralImpostorBaker::is_baking_orm() const {
	return bake_orm;
}

void OctahedralImpostorBaker::set_bake_ambient_occlusion(bool p_enable) {
	bake_ambient_occlusion = p_enable;
}

bool OctahedralImpostorBaker::is_baking_ambient_occlusion() const {
	return bake_ambient_occlusion;
}

void OctahedralImpostorBaker::set_bake_translucency(bool p_enable) {
	bake_translucency = p_enable;
}

bool OctahedralImpostorBaker::is_baking_translucency() const {
	return bake_translucency;
}

int OctahedralImpostorBaker::get_frame_size() const {
	return atlas_size / frames;
}

int OctahedralImpostorBaker::get_baked_atlas_size() const {
	return atlas_size;
}

/* Geometry */

bool OctahedralImpostorBaker::_is_baked_geometry(const Geometry &p_geometry) {
	if (Object::cast_to<OctahedralImpostorMaterial3D>(p_geometry.material_override.ptr())) {
		return true;
	}
	for (const Ref<Material> &material : p_geometry.surface_materials) {
		if (Object::cast_to<OctahedralImpostorMaterial3D>(material.ptr())) {
			return true;
		}
	}
	Ref<Mesh> mesh = p_geometry.base_resource;
	if (p_geometry.multimesh) {
		mesh = Ref<MultiMesh>(p_geometry.base_resource)->get_mesh();
	}
	return mesh.is_valid() && mesh->has_meta(baked_billboard_meta);
}

void OctahedralImpostorBaker::_collect_geometry(Node *p_node, const Transform3D &p_transform, bool p_is_root, bool p_first_only, LocalVector<Geometry> &r_geometry) {
	Transform3D transform = p_transform;
	Node3D *node_3d = Object::cast_to<Node3D>(p_node);
	if (node_3d && !p_is_root) {
		if (!node_3d->is_visible()) {
			return;
		}
		transform = p_transform * node_3d->get_transform();
	}

	GeometryInstance3D *geometry_instance = Object::cast_to<GeometryInstance3D>(p_node);
	if (geometry_instance && geometry_instance->get_cast_shadows_setting() != GeometryInstance3D::SHADOW_CASTING_SETTING_SHADOWS_ONLY && geometry_instance->get_visibility_range_begin() <= 0.0) {
		Geometry geometry;
		geometry.node = geometry_instance->get_instance_id();
		geometry.transform = transform;
		geometry.material_override = geometry_instance->get_material_override();
		geometry.material_overlay = geometry_instance->get_material_overlay();
		bool valid = false;

		MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(p_node);
		MultiMeshInstance3D *multimesh_instance = Object::cast_to<MultiMeshInstance3D>(p_node);
		if (mesh_instance) {
			Ref<Mesh> mesh = mesh_instance->get_mesh();
			if (mesh.is_valid() && mesh->get_surface_count() > 0) {
				geometry.base = mesh->get_rid();
				geometry.base_resource = mesh;
				for (int i = 0; i < mesh->get_surface_count(); i++) {
					Ref<Material> surface_override;
					if (i < mesh_instance->get_surface_override_material_count()) {
						surface_override = mesh_instance->get_surface_override_material(i);
					}
					geometry.surface_override_materials.push_back(surface_override);
					geometry.surface_materials.push_back(surface_override.is_valid() ? surface_override : mesh->surface_get_material(i));
				}
				for (int i = 0; i < mesh_instance->get_blend_shape_count(); i++) {
					geometry.blend_shape_weights.push_back(mesh_instance->get_blend_shape_value(i));
				}
				valid = true;
			}
		} else if (multimesh_instance) {
			Ref<MultiMesh> multimesh = multimesh_instance->get_multimesh();
			if (multimesh.is_valid() && multimesh->get_mesh().is_valid() && multimesh->get_instance_count() > 0 && multimesh->get_transform_format() == MultiMesh::TRANSFORM_3D) {
				geometry.base = multimesh->get_rid();
				geometry.base_resource = multimesh;
				geometry.multimesh = true;
				const Ref<Mesh> mesh = multimesh->get_mesh();
				for (int i = 0; i < mesh->get_surface_count(); i++) {
					geometry.surface_materials.push_back(mesh->surface_get_material(i));
				}
				valid = true;
			}
		} else if (p_node->is_class("CSGShape3D") && bool(p_node->call("is_root_shape"))) {
			// The children shapes are part of the mesh of the root shape.
			const Array meshes = p_node->call("get_meshes");
			if (meshes.size() == 2 && Object::cast_to<Mesh>(meshes[1])) {
				Ref<Mesh> mesh = meshes[1];
				geometry.transform = transform * Transform3D(meshes[0]);
				geometry.base = mesh->get_rid();
				geometry.base_resource = mesh;
				for (int i = 0; i < mesh->get_surface_count(); i++) {
					geometry.surface_override_materials.push_back(Ref<Material>());
					geometry.surface_materials.push_back(mesh->surface_get_material(i));
				}
				valid = mesh->get_surface_count() > 0;
			}
		}

		// Skip the impostors and billboards, e.g. of a previous bake of the same node.
		if (valid && _is_baked_geometry(geometry)) {
			valid = false;
		}

		if (valid && p_first_only) {
			r_geometry.push_back(geometry);
			return;
		}
		if (valid) {
			List<PropertyInfo> properties;
			geometry_instance->get_property_list(&properties);
			for (const PropertyInfo &property : properties) {
				if (property.name.begins_with("instance_shader_parameters/")) {
					const StringName name = property.name.trim_prefix("instance_shader_parameters/");
					const Variant value = geometry_instance->get_instance_shader_parameter(name);
					if (value.get_type() != Variant::NIL) {
						geometry.instance_parameters.push_back(Pair<StringName, Variant>(name, value));
					}
				}
			}
			r_geometry.push_back(geometry);
		}
	}

	for (int i = 0; i < p_node->get_child_count(); i++) {
		_collect_geometry(p_node->get_child(i), transform, false, p_first_only, r_geometry);
		if (p_first_only && !r_geometry.is_empty()) {
			return;
		}
	}
}

LocalVector<OctahedralImpostorBaker::Geometry> OctahedralImpostorBaker::collect_geometry(Node3D *p_node) {
	LocalVector<Geometry> geometry;
	ERR_FAIL_NULL_V(p_node, geometry);
	_collect_geometry(p_node, Transform3D(), true, false, geometry);
	return geometry;
}

bool OctahedralImpostorBaker::has_geometry(Node3D *p_node) {
	ERR_FAIL_NULL_V(p_node, false);
	LocalVector<Geometry> geometry;
	_collect_geometry(p_node, Transform3D(), true, true, geometry);
	return !geometry.is_empty();
}

bool OctahedralImpostorBaker::_material_uses_backlight(const Ref<Material> &p_material) {
	const Ref<BaseMaterial3D> base_material = p_material;
	if (base_material.is_valid()) {
		return base_material->get_feature(BaseMaterial3D::FEATURE_BACKLIGHT) && (base_material->get_texture(BaseMaterial3D::TEXTURE_BACKLIGHT).is_valid() || !base_material->get_backlight().is_equal_approx(Color(0, 0, 0)));
	}
	if (p_material.is_valid() && p_material->get_shader_mode() == Shader::MODE_SPATIAL) {
		const RID shader = p_material->get_shader_rid();
		return shader.is_valid() && RS::get_singleton()->shader_get_code(shader).contains("BACKLIGHT");
	}
	return false;
}

bool OctahedralImpostorBaker::has_translucency(const LocalVector<Geometry> &p_geometry) {
	for (const Geometry &geometry : p_geometry) {
		if (geometry.material_override.is_valid()) {
			if (_material_uses_backlight(geometry.material_override)) {
				return true;
			}
			continue;
		}
		for (const Ref<Material> &material : geometry.surface_materials) {
			if (_material_uses_backlight(material)) {
				return true;
			}
		}
	}
	return false;
}

bool OctahedralImpostorBaker::compute_bounding_sphere(const LocalVector<Geometry> &p_geometry, Vector3 &r_center, float &r_radius) {
	AABB bounds;
	bool first = true;
	for (const Geometry &geometry : p_geometry) {
		AABB aabb;
		if (geometry.multimesh) {
			aabb = Ref<MultiMesh>(geometry.base_resource)->get_aabb();
		} else {
			aabb = Ref<Mesh>(geometry.base_resource)->get_aabb();
		}
		aabb = geometry.transform.xform(aabb);
		if (first) {
			bounds = aabb;
			first = false;
		} else {
			bounds.merge_with(aabb);
		}
	}
	if (first) {
		return false;
	}

	// Centered on the bounds, with the radius of the farthest vertex (tighter than the bounds for
	// most shapes, e.g. the corners of the bounds of a tree are empty).
	const Vector3 center = bounds.get_center();
	LocalVector<Vector3> points;
	_collect_points(p_geometry, points);
	real_t radius_squared = 0.0;
	for (const Vector3 &point : points) {
		radius_squared = MAX(radius_squared, point.distance_squared_to(center));
	}

	r_center = center;
	r_radius = Math::sqrt(radius_squared);
	return r_radius > CMP_EPSILON;
}

void OctahedralImpostorBaker::_collect_points(const LocalVector<Geometry> &p_geometry, LocalVector<Vector3> &r_points) {
	// The vertices of the meshes, the corners of the bounds of the instances of the multimeshes.
	for (const Geometry &geometry : p_geometry) {
		if (geometry.multimesh) {
			const Ref<MultiMesh> multimesh = geometry.base_resource;
			const AABB mesh_aabb = multimesh->get_mesh()->get_aabb();
			for (int i = 0; i < multimesh->get_instance_count(); i++) {
				const Transform3D transform = geometry.transform * multimesh->get_instance_transform(i);
				for (int j = 0; j < 8; j++) {
					r_points.push_back(transform.xform(mesh_aabb.get_endpoint(j)));
				}
			}
			continue;
		}

		const Ref<Mesh> mesh = geometry.base_resource;
		bool has_vertices = false;
		for (int i = 0; i < mesh->get_surface_count(); i++) {
			const Array arrays = mesh->surface_get_arrays(i);
			if (arrays.size() != Mesh::ARRAY_MAX) {
				continue;
			}
			const PackedVector3Array vertices = arrays[Mesh::ARRAY_VERTEX];
			for (const Vector3 &vertex : vertices) {
				r_points.push_back(geometry.transform.xform(vertex));
			}
			has_vertices = has_vertices || !vertices.is_empty();
		}
		if (!has_vertices) {
			const AABB aabb = mesh->get_aabb();
			for (int j = 0; j < 8; j++) {
				r_points.push_back(geometry.transform.xform(aabb.get_endpoint(j)));
			}
		}
	}
}

bool OctahedralImpostorBaker::compute_billboard_views(const LocalVector<Geometry> &p_geometry, BillboardMode p_mode, int p_cross_planes, int p_size, LocalVector<View> &r_views, Size2i &r_atlas_size, real_t &r_texel_size) {
	ERR_FAIL_INDEX_V(p_mode, BILLBOARD_MAX, false);
	Vector3 center;
	float radius = 0.0;
	if (!compute_bounding_sphere(p_geometry, center, radius)) {
		return false;
	}
	LocalVector<Vector3> points;
	_collect_points(p_geometry, points);

	// The quad of a billboard turns around the origin of the node, with the view offset to the
	// geometry. The planes of a cross meet on the vertical axis through the center of the geometry.
	const int view_count = p_mode == BILLBOARD_CROSS ? CLAMP(p_cross_planes, MIN_CROSS_PLANES, MAX_CROSS_PLANES) : 1;
	const Vector3 axis = p_mode == BILLBOARD_CROSS ? Vector3(center.x, 0.0, center.z) : Vector3();
	// Smallest extent of a view, e.g. for a flat object seen from the side.
	const real_t min_extent = radius * 0.02;

	real_t bottom = Math::INF;
	real_t top = -Math::INF;
	for (const Vector3 &point : points) {
		bottom = MIN(bottom, point.y);
		top = MAX(top, point.y);
	}
	if (top - bottom < min_extent) {
		bottom = (bottom + top - min_extent) * 0.5;
		top = bottom + min_extent;
	}

	r_views.resize(view_count);
	LocalVector<Vector2> extents; // Left and right of the geometry in each view.
	extents.resize(view_count);
	real_t total_width = 0.0;
	for (int i = 0; i < view_count; i++) {
		// From the front (+Z), then around the vertical axis.
		const real_t angle = Math::PI * i / view_count;
		View &view = r_views[i];
		view.basis = OctahedralImpostorMaterial3D::get_frame_basis(Vector3(Math::sin(angle), 0.0, Math::cos(angle)));
		const Vector3 right = view.basis.get_column(0);
		real_t left = Math::INF;
		real_t right_end = -Math::INF;
		for (const Vector3 &point : points) {
			const real_t x = right.dot(point - axis);
			left = MIN(left, x);
			right_end = MAX(right_end, x);
		}
		if (right_end - left < min_extent) {
			left = (left + right_end - min_extent) * 0.5;
			right_end = left + min_extent;
		}
		extents[i] = Vector2(left, right_end);
		total_width += right_end - left;
	}

	// The views are side by side, the longest side of the textures has p_size pixels (a power of
	// two). Each view has an empty border, and its size is rounded up to whole pixels and to blocks
	// of 4 pixels (the blocks of the compressed formats don't cross the views).
	const int size = CLAMP(_round_to_power_of_2(p_size), MIN_ATLAS_SIZE, MAX_ATLAS_SIZE);
	const int slack = 2 * FRAME_PADDING + 4;
	const real_t texel = MAX(total_width / (size - view_count * slack), (top - bottom) / (size - slack));
	int height = (int(Math::ceil((top - bottom) / texel)) + 2 * FRAME_PADDING + 3) & ~3;
	LocalVector<int> widths;
	widths.resize(view_count);
	int total = 0;
	for (int i = 0; i < view_count; i++) {
		widths[i] = (int(Math::ceil((extents[i].y - extents[i].x) / texel)) + 2 * FRAME_PADDING + 3) & ~3;
		total += widths[i];
	}
	// The sides are powers of two (the longest one has the whole size), with a wider border.
	const int width = total >= height ? size : MIN(int(Math::next_power_of_2(uint32_t(total))), size);
	height = total >= height ? MIN(int(Math::next_power_of_2(uint32_t(height))), size) : size;
	for (int i = 0; total + 4 <= width; i = (i + 1) % view_count) {
		widths[i] += 4;
		total += 4;
	}

	int x = 0;
	for (int i = 0; i < view_count; i++) {
		View &view = r_views[i];
		view.rect = Rect2i(x, 0, widths[i], height);
		view.pixel_center = Vector2(view.rect.position) + Vector2(view.rect.size) * 0.5;
		view.center = axis + view.basis.get_column(0) * ((extents[i].x + extents[i].y) * 0.5) + Vector3(0.0, (bottom + top) * 0.5, 0.0);
		x += widths[i];
	}

	r_atlas_size = Size2i(x, height);
	r_texel_size = texel;
	return true;
}

void OctahedralImpostorBaker::_estimate_surface_parameters(const LocalVector<Geometry> &p_geometry) {
	// Constant roughness and metallic of the impostor without ORM atlas: the mean of the materials.
	float roughness_sum = 0.0;
	float metallic_sum = 0.0;
	int count = 0;
	for (const Geometry &geometry : p_geometry) {
		for (const Ref<Material> &surface_material : geometry.surface_materials) {
			const Ref<BaseMaterial3D> material = geometry.material_override.is_valid() ? geometry.material_override : surface_material;
			if (material.is_valid()) {
				roughness_sum += material->get_roughness();
				metallic_sum += material->get_metallic();
				count++;
			}
		}
	}
	source_roughness = count > 0 ? roughness_sum / count : 1.0;
	source_metallic = count > 0 ? metallic_sum / count : 0.0;
}

/* Rendering */

Ref<ShaderMaterial> OctahedralImpostorBaker::_get_override_material(BakeState &p_state, const Ref<Material> &p_material) {
	const ObjectID id = p_material.is_valid() ? p_material->get_instance_id() : ObjectID();
	const Ref<ShaderMaterial> *cached = p_state.override_materials.getptr(id);
	if (cached) {
		return *cached;
	}

	int cull_mode = 0; // Back, front, disabled.
	Ref<ShaderMaterial> material;
	material.instantiate();

	const Ref<BaseMaterial3D> base_material = p_material;
	const Ref<ShaderMaterial> shader_material = p_material;
	if (base_material.is_valid()) {
		material->set_shader_parameter("albedo", base_material->get_albedo());
		material->set_shader_parameter("texture_albedo", base_material->get_texture(BaseMaterial3D::TEXTURE_ALBEDO));
		material->set_shader_parameter("use_vertex_color", base_material->get_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR));
		switch (base_material->get_transparency()) {
			case BaseMaterial3D::TRANSPARENCY_DISABLED:
				break;
			case BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR:
				material->set_shader_parameter("alpha_scissor_threshold", base_material->get_alpha_scissor_threshold());
				break;
			default:
				material->set_shader_parameter("alpha_scissor_threshold", 0.5);
				break;
		}
		material->set_shader_parameter("uv1_scale", base_material->get_uv1_scale());
		material->set_shader_parameter("uv1_offset", base_material->get_uv1_offset());
		material->set_shader_parameter("uv2_scale", base_material->get_uv2_scale());
		material->set_shader_parameter("uv2_offset", base_material->get_uv2_offset());
		if (base_material->get_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING) && base_material->get_texture(BaseMaterial3D::TEXTURE_NORMAL).is_valid()) {
			material->set_shader_parameter("use_normal_map", true);
			material->set_shader_parameter("texture_normal", base_material->get_texture(BaseMaterial3D::TEXTURE_NORMAL));
			material->set_shader_parameter("normal_scale", base_material->get_normal_scale());
		}
		const bool ambient_occlusion = base_material->get_feature(BaseMaterial3D::FEATURE_AMBIENT_OCCLUSION);
		if (Object::cast_to<ORMMaterial3D>(base_material.ptr())) {
			// The ORM texture alone gives the roughness and the metallic, and the ambient occlusion
			// when it's enabled.
			const Ref<Texture2D> orm_texture = base_material->get_texture(BaseMaterial3D::TEXTURE_ORM);
			material->set_shader_parameter("roughness", 1.0);
			material->set_shader_parameter("metallic", 1.0);
			material->set_shader_parameter("texture_roughness", orm_texture);
			material->set_shader_parameter("roughness_channel", Vector4(0, 1, 0, 0));
			material->set_shader_parameter("texture_metallic", orm_texture);
			material->set_shader_parameter("metallic_channel", Vector4(0, 0, 1, 0));
			if (ambient_occlusion) {
				material->set_shader_parameter("texture_ambient_occlusion", orm_texture);
			}
		} else {
			material->set_shader_parameter("roughness", base_material->get_roughness());
			material->set_shader_parameter("metallic", base_material->get_metallic());
			material->set_shader_parameter("texture_roughness", base_material->get_texture(BaseMaterial3D::TEXTURE_ROUGHNESS));
			material->set_shader_parameter("roughness_channel", _texture_channel_mask(base_material->get_roughness_texture_channel()));
			material->set_shader_parameter("texture_metallic", base_material->get_texture(BaseMaterial3D::TEXTURE_METALLIC));
			material->set_shader_parameter("metallic_channel", _texture_channel_mask(base_material->get_metallic_texture_channel()));
			if (ambient_occlusion) {
				material->set_shader_parameter("texture_ambient_occlusion", base_material->get_texture(BaseMaterial3D::TEXTURE_AMBIENT_OCCLUSION));
				material->set_shader_parameter("ao_channel", _texture_channel_mask(base_material->get_ao_texture_channel()));
				material->set_shader_parameter("ao_on_uv2", base_material->get_flag(BaseMaterial3D::FLAG_AO_ON_UV2));
			}
		}
		if (base_material->get_feature(BaseMaterial3D::FEATURE_BACKLIGHT)) {
			material->set_shader_parameter("backlight", base_material->get_backlight());
			material->set_shader_parameter("texture_backlight", base_material->get_texture(BaseMaterial3D::TEXTURE_BACKLIGHT));
		}
		if (base_material->get_cull_mode() == BaseMaterial3D::CULL_FRONT) {
			cull_mode = 1;
		} else if (base_material->get_cull_mode() == BaseMaterial3D::CULL_DISABLED) {
			cull_mode = 2;
		}
	} else if (shader_material.is_valid() && shader_material->get_shader().is_valid()) {
		const Ref<Shader> shader = shader_material->get_shader();
		HashSet<StringName> uniforms;
		List<PropertyInfo> uniform_list;
		shader->get_shader_uniform_list(&uniform_list);
		for (const PropertyInfo &uniform : uniform_list) {
			uniforms.insert(uniform.name);
		}

		const Variant albedo = _find_shader_parameter(shader_material, uniforms, { "albedo", "albedo_color", "base_color", "color" }, Variant::COLOR);
		if (albedo.get_type() == Variant::COLOR) {
			material->set_shader_parameter("albedo", albedo);
		}
		material->set_shader_parameter("texture_albedo", _find_shader_parameter(shader_material, uniforms, { "texture_albedo", "albedo_texture", "texture_base_color", "base_color_texture", "texture_diffuse", "diffuse_texture", "main_texture" }, Variant::OBJECT));
		const Variant alpha_scissor = _find_shader_parameter(shader_material, uniforms, { "alpha_scissor_threshold", "alpha_scissor", "alpha_threshold", "alpha_cutoff", "alpha_clip" }, Variant::FLOAT);
		if (alpha_scissor.get_type() == Variant::FLOAT) {
			material->set_shader_parameter("alpha_scissor_threshold", alpha_scissor);
		}
		const Variant uv1_scale = _find_shader_parameter(shader_material, uniforms, { "uv1_scale", "uv_scale" }, Variant::VECTOR3);
		if (uv1_scale.get_type() == Variant::VECTOR3) {
			material->set_shader_parameter("uv1_scale", uv1_scale);
		}
		const Variant uv1_offset = _find_shader_parameter(shader_material, uniforms, { "uv1_offset", "uv_offset" }, Variant::VECTOR3);
		if (uv1_offset.get_type() == Variant::VECTOR3) {
			material->set_shader_parameter("uv1_offset", uv1_offset);
		}
		const Variant normal_texture = _find_shader_parameter(shader_material, uniforms, { "texture_normal", "normal_texture", "texture_normal_map", "normal_map", "normalmap" }, Variant::OBJECT);
		if (normal_texture.get_type() == Variant::OBJECT) {
			material->set_shader_parameter("use_normal_map", true);
			material->set_shader_parameter("texture_normal", normal_texture);
			const Variant normal_scale = _find_shader_parameter(shader_material, uniforms, { "normal_scale", "normal_strength", "normal_depth", "normal_map_depth" }, Variant::FLOAT);
			if (normal_scale.get_type() == Variant::FLOAT) {
				material->set_shader_parameter("normal_scale", normal_scale);
			}
		}
		const Variant roughness = _find_shader_parameter(shader_material, uniforms, { "roughness" }, Variant::FLOAT);
		if (roughness.get_type() == Variant::FLOAT) {
			material->set_shader_parameter("roughness", roughness);
		}
		const Variant metallic = _find_shader_parameter(shader_material, uniforms, { "metallic" }, Variant::FLOAT);
		if (metallic.get_type() == Variant::FLOAT) {
			material->set_shader_parameter("metallic", metallic);
		}
		const Variant orm_texture = _find_shader_parameter(shader_material, uniforms, { "texture_orm", "orm_texture" }, Variant::OBJECT);
		if (orm_texture.get_type() == Variant::OBJECT) {
			material->set_shader_parameter("texture_roughness", orm_texture);
			material->set_shader_parameter("roughness_channel", Vector4(0, 1, 0, 0));
			material->set_shader_parameter("texture_metallic", orm_texture);
			material->set_shader_parameter("metallic_channel", Vector4(0, 0, 1, 0));
			material->set_shader_parameter("texture_ambient_occlusion", orm_texture);
		} else {
			material->set_shader_parameter("texture_roughness", _find_shader_parameter(shader_material, uniforms, { "texture_roughness", "roughness_texture" }, Variant::OBJECT));
			material->set_shader_parameter("texture_metallic", _find_shader_parameter(shader_material, uniforms, { "texture_metallic", "metallic_texture" }, Variant::OBJECT));
			material->set_shader_parameter("texture_ambient_occlusion", _find_shader_parameter(shader_material, uniforms, { "texture_ambient_occlusion", "ao_texture", "texture_ao" }, Variant::OBJECT));
		}
		const Variant backlight = _find_shader_parameter(shader_material, uniforms, { "backlight", "backlight_color", "translucency_color" }, Variant::COLOR);
		if (backlight.get_type() == Variant::COLOR) {
			material->set_shader_parameter("backlight", backlight);
		}
		material->set_shader_parameter("texture_backlight", _find_shader_parameter(shader_material, uniforms, { "texture_backlight", "backlight_texture", "texture_translucency", "translucency_texture" }, Variant::OBJECT));
		const String code = shader->get_code();
		if (code.contains("cull_disabled")) {
			cull_mode = 2;
		} else if (code.contains("cull_front")) {
			cull_mode = 1;
		}
	}

	if (p_state.override_shaders[cull_mode].is_null()) {
		static const char *cull_modes[3] = { "cull_back", "cull_front", "cull_disabled" };
		Ref<Shader> shader;
		shader.instantiate();
		shader->set_code(_get_override_shader_code(cull_modes[cull_mode]));
		p_state.override_shaders[cull_mode] = shader;
	}
	material->set_shader(p_state.override_shaders[cull_mode]);
	material->set_shader_parameter("impostor_bake_depth_near", p_state.depth_near);
	material->set_shader_parameter("impostor_bake_depth_range", p_state.depth_range);
	p_state.override_materials.insert(id, material);
	return material;
}

RID OctahedralImpostorBaker::_get_capture_shader(BakeState &p_state, RID p_shader) {
	const RID *cached = p_state.capture_shaders.getptr(p_shader);
	if (cached) {
		return *cached;
	}

	RS *rs = RS::get_singleton();
	RID capture_shader;
	const String code = make_capture_shader_code(rs->shader_get_code(p_shader));
	String error;
	if (!code.is_empty() && _is_valid_spatial_shader(code, error)) {
		capture_shader = rs->shader_create_from_code(code);
		p_state.owned_rids.push_back(capture_shader);
		// The materials only have the textures they set, the others are the defaults of the shader.
		List<PropertyInfo> parameters;
		rs->get_shader_parameter_list(p_shader, &parameters);
		for (const PropertyInfo &parameter : parameters) {
			if (parameter.type == Variant::OBJECT) {
				const RID texture = rs->shader_get_default_texture_parameter(p_shader, parameter.name);
				if (texture.is_valid()) {
					rs->shader_set_default_texture_parameter(capture_shader, parameter.name, texture);
				}
			}
		}
	} else {
		print_verbose(vformat("Octahedral impostor baker: a shader can't capture the data of its materials (%s), the parameters of the materials are used instead.", code.is_empty() ? String("unsupported code") : error));
	}
	p_state.capture_shaders.insert(p_shader, capture_shader);
	return capture_shader;
}

RID OctahedralImpostorBaker::_get_capture_material(BakeState &p_state, const Ref<Material> &p_material) {
	if (p_material.is_null()) {
		// Like the default material of the renderer.
		if (p_state.default_capture_material.is_null()) {
			p_state.default_capture_material = _get_override_material(p_state, Ref<Material>())->get_rid();
			p_state.capture_material_list.push_back(p_state.default_capture_material);
		}
		return p_state.default_capture_material;
	}
	const RID *cached = p_state.capture_materials.getptr(p_material->get_instance_id());
	if (cached) {
		return *cached;
	}

	// The shader of the material, changed to write the data instead of its color, with the
	// parameters of the material.
	RS *rs = RS::get_singleton();
	RID material;
	const RID shader = p_material->get_shader_mode() == Shader::MODE_SPATIAL ? p_material->get_shader_rid() : RID();
	const RID capture_shader = shader.is_valid() ? _get_capture_shader(p_state, shader) : RID();
	if (capture_shader.is_valid()) {
		material = rs->material_create();
		p_state.owned_rids.push_back(material);
		rs->material_set_shader(material, capture_shader);
		const RID source = p_material->get_rid();
		List<PropertyInfo> parameters;
		rs->get_shader_parameter_list(shader, &parameters);
		for (const PropertyInfo &parameter : parameters) {
			const Variant value = rs->material_get_param(source, parameter.name);
			if (value.get_type() != Variant::NIL) {
				rs->material_set_param(material, parameter.name, value);
			}
		}
		rs->material_set_param(material, "impostor_bake_depth_near", p_state.depth_near);
		rs->material_set_param(material, "impostor_bake_depth_range", p_state.depth_range);
	} else {
		material = _get_override_material(p_state, p_material)->get_rid();
	}
	p_state.capture_materials.insert(p_material->get_instance_id(), material);
	p_state.capture_material_list.push_back(material);
	return material;
}

Error OctahedralImpostorBaker::_setup_impostor_views(BakeState &p_state, const Vector3 &p_center, float p_radius) {
	// The frames fill the atlas, a power of two: they may begin and end between pixels (the shader
	// maps each frame to 1 / frames of the atlas). Each pixel belongs to the frame of its center.
	const real_t frame_size = real_t(atlas_size) / frames;
	ERR_FAIL_COND_V_MSG(frame_size < 4 * FRAME_PADDING, ERR_INVALID_PARAMETER, vformat("The frames of the octahedral impostor are too small (%d pixels): increase the atlas size or reduce the number of frames.", get_frame_size()));
	// No more supersampling than a large viewport allows for a single frame.
	p_state.supersampling = supersampling;
	while (p_state.supersampling > 1 && frame_size * p_state.supersampling > MAX_REGION_SIZE * 2) {
		p_state.supersampling--;
	}
	p_state.atlas_size = Size2i(atlas_size, atlas_size);

	// Grown so that the frames keep an empty border.
	sphere_center = p_center;
	sphere_radius = p_radius * float(frame_size) / float(frame_size - 2 * FRAME_PADDING);
	p_state.texel_world = 2.0 * sphere_radius / frame_size;

	LocalVector<int> edges; // Of the pixels of the frames, along each axis.
	for (int i = 0; i <= frames; i++) {
		edges.push_back(int(Math::floor(i * frame_size + 0.5)));
	}
	p_state.views.resize(frames * frames);
	for (int y = 0; y < frames; y++) {
		for (int x = 0; x < frames; x++) {
			View &view = p_state.views[y * frames + x];
			view.basis = OctahedralImpostorMaterial3D::get_frame_basis(OctahedralImpostorMaterial3D::get_frame_direction(Vector2i(x, y), frames, layout));
			view.center = sphere_center;
			view.rect = Rect2i(edges[x], edges[y], edges[x + 1] - edges[x], edges[y + 1] - edges[y]);
			view.pixel_center = Vector2(x + 0.5, y + 0.5) * frame_size;
		}
	}
	// Regions of whole frames when they fit, the frames across two regions are rendered in parts.
	const int frames_per_region = MAX(1, int(MAX_REGION_SIZE / (frame_size * p_state.supersampling)));
	_setup_regions(p_state, MAX(1, int(Math::floor(frames_per_region * frame_size))));
	return OK;
}

Error OctahedralImpostorBaker::_setup_billboard_views(BakeState &p_state, const Vector3 &p_center, float p_radius) {
	sphere_center = p_center;
	sphere_radius = p_radius;
	ERR_FAIL_COND_V(!compute_billboard_views(p_state.geometry, billboard_mode, cross_planes, atlas_size, p_state.views, p_state.atlas_size, p_state.texel_world), ERR_INVALID_DATA);
	p_state.supersampling = supersampling;
	// A view may be larger than a region: it's rendered in parts.
	_setup_regions(p_state, MAX_REGION_SIZE / p_state.supersampling);
	return OK;
}

void OctahedralImpostorBaker::_setup_regions(BakeState &p_state, int p_region_size) {
	p_state.regions.clear();
	const int region_size = MAX(p_region_size, 1);
	for (int y = 0; y < p_state.atlas_size.y; y += region_size) {
		for (int x = 0; x < p_state.atlas_size.x; x += region_size) {
			Region region;
			region.rect = Rect2i(x, y, MIN(region_size, p_state.atlas_size.x - x), MIN(region_size, p_state.atlas_size.y - y));
			for (uint32_t i = 0; i < p_state.views.size(); i++) {
				if (p_state.views[i].rect.intersects(region.rect)) {
					region.views.push_back(i);
				}
			}
			if (!region.views.is_empty()) {
				p_state.regions.push_back(region);
			}
		}
	}
}

void OctahedralImpostorBaker::_setup_camera(BakeState &p_state, const Size2i &p_size, real_t p_texel, int p_supersampling) {
	// Orthographic camera looking down -Z, its view covers the region from (0, 0) to (width, -height).
	RS *rs = RS::get_singleton();
	const Size2 size = Size2(p_size) * p_texel;
	rs->viewport_set_size(p_state.viewport, p_size.x * p_supersampling, p_size.y * p_supersampling);
	rs->camera_set_transform(p_state.camera, Transform3D(Basis(), Vector3(size.x * 0.5, -size.y * 0.5, p_state.depth_extent + p_state.margin_world)));
	rs->camera_set_orthogonal(p_state.camera, size.y, p_state.margin_world * 0.5, 2.0 * p_state.depth_extent + p_state.margin_world * 2.0);
}

void OctahedralImpostorBaker::_setup_instances(BakeState &p_state, const LocalVector<View> &p_views, const Region &p_region, real_t p_texel) {
	RS *rs = RS::get_singleton();
	for (uint32_t view_index : p_region.views) {
		const View &view = p_views[view_index];
		// The camera looks down -Z: the object is rotated so that the view faces it, with the center
		// of the view at its position in the pixels.
		const Basis to_view = view.basis.transposed();
		const Vector2 view_center = view.pixel_center - Vector2(p_region.rect.position);
		const Vector3 position = Vector3(view_center.x, -view_center.y, 0.0) * p_texel;
		const Transform3D frame_transform = Transform3D(to_view, position - to_view.xform(view.center));

		for (uint32_t i = 0; i < p_state.geometry.size(); i++) {
			const Geometry &geometry = p_state.geometry[i];
			const RID instance = rs->instance_create2(geometry.base, p_state.scenario);
			rs->instance_set_transform(instance, frame_transform * geometry.transform);
			rs->instance_geometry_set_cast_shadows_setting(instance, RSE::SHADOW_CASTING_SETTING_OFF);
			for (int j = 0; j < geometry.blend_shape_weights.size(); j++) {
				rs->instance_set_blend_shape_weight(instance, j, geometry.blend_shape_weights[j]);
			}
			for (const Pair<StringName, Variant> &parameter : geometry.instance_parameters) {
				rs->instance_geometry_set_shader_parameter(instance, parameter.first, parameter.second);
			}
			p_state.instances.push_back(Pair<RID, int>(instance, i));
		}
	}
}

void OctahedralImpostorBaker::_set_pass(BakeState &p_state, Pass p_pass) {
	RS *rs = RS::get_singleton();

	// The albedo is the unshaded color of the real materials, with their transparency.
	rs->viewport_set_debug_draw(p_state.viewport, p_pass == PASS_ALBEDO ? RSE::VIEWPORT_DEBUG_DRAW_UNSHADED : RSE::VIEWPORT_DEBUG_DRAW_DISABLED);

	if (p_state.capture_instance.is_valid()) {
		rs->instance_set_visible(p_state.capture_instance, p_pass == PASS_GEOMETRY);
	}
	// The other data is written by the materials that capture it.
	const bool capture = p_pass == PASS_ORM || p_pass == PASS_TRANSLUCENCY || (p_pass == PASS_GEOMETRY && !p_state.use_normal_buffer);

	for (const Pair<RID, int> &instance : p_state.instances) {
		Geometry &geometry = p_state.geometry[instance.second];
		if (capture) {
			if (geometry.multimesh) {
				// Surfaces of multimeshes can't be overridden separately.
				const Ref<Material> material = geometry.material_override.is_valid() ? geometry.material_override : (geometry.surface_materials.is_empty() ? Ref<Material>() : geometry.surface_materials[0]);
				rs->instance_geometry_set_material_override(instance.first, _get_capture_material(p_state, material));
			} else if (geometry.material_override.is_valid()) {
				rs->instance_geometry_set_material_override(instance.first, _get_capture_material(p_state, geometry.material_override));
			} else {
				rs->instance_geometry_set_material_override(instance.first, RID());
				for (int i = 0; i < geometry.surface_materials.size(); i++) {
					rs->instance_set_surface_override_material(instance.first, i, _get_capture_material(p_state, geometry.surface_materials[i]));
				}
			}
			rs->instance_geometry_set_material_overlay(instance.first, RID());
		} else {
			for (int i = 0; i < geometry.surface_override_materials.size(); i++) {
				const Ref<Material> &material = geometry.surface_override_materials[i];
				rs->instance_set_surface_override_material(instance.first, i, material.is_valid() ? material->get_rid() : RID());
			}
			rs->instance_geometry_set_material_override(instance.first, geometry.material_override.is_valid() ? geometry.material_override->get_rid() : RID());
			rs->instance_geometry_set_material_overlay(instance.first, geometry.material_overlay.is_valid() ? geometry.material_overlay->get_rid() : RID());
		}
	}

	if (capture) {
		CaptureMode mode = CAPTURE_NORMAL_DEPTH;
		if (p_pass == PASS_ORM) {
			mode = CAPTURE_ORM;
		} else if (p_pass == PASS_TRANSLUCENCY) {
			mode = CAPTURE_BACKLIGHT;
		}
		for (const RID &material : p_state.capture_material_list) {
			rs->material_set_param(material, "impostor_bake_mode", int(mode));
		}
	}
}

void OctahedralImpostorBaker::_free_instances(BakeState &p_state) {
	for (const Pair<RID, int> &instance : p_state.instances) {
		RS::get_singleton()->free_rid(instance.first);
	}
	p_state.instances.clear();
}

void OctahedralImpostorBaker::_draw(BakeState &p_state) {
	RS *rs = RS::get_singleton();
	// Draws only the viewport of the bake, not the editor (like the previews of the resources).
	RID root_viewport;
	SceneTree *tree = SceneTree::get_singleton();
	if (tree && tree->get_root()) {
		root_viewport = tree->get_root()->get_viewport_rid();
		rs->viewport_set_active(root_viewport, false);
	}
	rs->viewport_set_update_mode(p_state.viewport, RSE::VIEWPORT_UPDATE_ONCE);
	rs->draw(false);
	if (root_viewport.is_valid()) {
		rs->viewport_set_active(root_viewport, true);
	}
}

bool OctahedralImpostorBaker::_is_output_srgb(BakeState &p_state, Pass p_pass) {
	// The albedo pass renders the colors of lit materials in the unshaded debug mode: some renderers
	// (e.g. Compatibility) keep them in sRGB. The other passes write data with unshaded materials.
	const bool albedo = p_pass == PASS_ALBEDO;
	RS *rs = RS::get_singleton();
	Ref<Material> material;
	if (albedo) {
		Ref<StandardMaterial3D> standard_material;
		standard_material.instantiate();
		standard_material->set_albedo(Color(0.5, 0.5, 0.5)); // 0.214 in linear.
		standard_material->set_specular(0.0);
		standard_material->set_roughness(1.0);
		standard_material->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
		material = standard_material;
	} else {
		Ref<Shader> shader;
		shader.instantiate();
		shader->set_code(calibration_shader_code);
		Ref<ShaderMaterial> shader_material;
		shader_material.instantiate();
		shader_material->set_shader(shader);
		material = shader_material;
	}
	Ref<QuadMesh> quad;
	quad.instantiate();
	const RID instance = rs->instance_create2(quad->get_rid(), p_state.scenario);
	rs->instance_geometry_set_material_override(instance, material->get_rid());
	// Fills the view of the camera.
	rs->instance_set_transform(instance, Transform3D(Basis::from_scale(Vector3(4, 4, 1)), Vector3(0, 0, -1)));
	rs->instance_set_custom_aabb(instance, AABB(Vector3(-1, -1, -1) * 1e6, Vector3(2, 2, 2) * 1e6));

	rs->viewport_set_size(p_state.viewport, 4, 4);
	rs->viewport_set_debug_draw(p_state.viewport, albedo ? RSE::VIEWPORT_DEBUG_DRAW_UNSHADED : RSE::VIEWPORT_DEBUG_DRAW_DISABLED);
	rs->camera_set_transform(p_state.camera, Transform3D());
	rs->camera_set_orthogonal(p_state.camera, 1.0, 0.1, 10.0);
	_draw(p_state);
	const Ref<Image> image = rs->texture_2d_get(rs->viewport_get_texture(p_state.viewport));
	rs->free_rid(instance);
	ERR_FAIL_COND_V(image.is_null() || image->is_empty(), false);
	// About 0.2 when the output is linear, 0.5 when it's sRGB.
	const Color color = image->get_pixel(2, 2);
	print_verbose(vformat("Octahedral impostor baker: output of the renderer for a gray of 0.2 in linear (%s): %s.", albedo ? "albedo" : "data", color));
	return color.r > 0.35;
}

Ref<Image> OctahedralImpostorBaker::_get_render(BakeState &p_state, Pass p_pass) {
	// Linear values, as floats.
	RS *rs = RS::get_singleton();
	Ref<Image> image = rs->texture_2d_get(rs->viewport_get_texture(p_state.viewport));
	ERR_FAIL_COND_V(image.is_null() || image->is_empty(), Ref<Image>());
	image->clear_mipmaps();
	image->convert(Image::FORMAT_RGBAF);
	if (p_state.srgb_output[p_pass == PASS_ALBEDO ? 0 : 1]) {
		float *pixels = reinterpret_cast<float *>(image->ptrw());
		const int64_t count = int64_t(image->get_width()) * image->get_height();
		for (int64_t i = 0; i < count; i++) {
			const Color linear = Color(pixels[i * 4], pixels[i * 4 + 1], pixels[i * 4 + 2]).srgb_to_linear();
			pixels[i * 4] = linear.r;
			pixels[i * 4 + 1] = linear.g;
			pixels[i * 4 + 2] = linear.b;
		}
	}
	return image;
}

void OctahedralImpostorBaker::_render_occlusion_views(BakeState &p_state) {
	// The depth of the geometry seen from directions spread over the sphere (a Fibonacci sphere),
	// with a border of a few pixels around the bounding sphere.
	const int count = OCCLUSION_GRID * OCCLUSION_GRID;
	const int size = OCCLUSION_VIEW_SIZE;
	const real_t radius = sphere_radius * real_t(size) / real_t(size - 2 * FRAME_PADDING);
	p_state.occlusion_texel = 2.0 * radius / size;
	p_state.occlusion_views.resize(count);
	const real_t golden_angle = Math::PI * (3.0 - Math::sqrt(5.0));
	Region region;
	region.rect = Rect2i(0, 0, OCCLUSION_GRID * size, OCCLUSION_GRID * size);
	for (int i = 0; i < count; i++) {
		const real_t y = 1.0 - (2.0 * i + 1.0) / count;
		const real_t ring = Math::sqrt(MAX(1.0 - y * y, 0.0));
		const real_t angle = golden_angle * i;
		View &view = p_state.occlusion_views[i];
		view.basis = OctahedralImpostorMaterial3D::get_frame_basis(Vector3(ring * Math::cos(angle), y, ring * Math::sin(angle)));
		view.center = sphere_center;
		view.rect = Rect2i((i % OCCLUSION_GRID) * size, (i / OCCLUSION_GRID) * size, size, size);
		view.pixel_center = Vector2(view.rect.position) + Vector2(size, size) * 0.5;
		region.views.push_back(i);
	}

	_setup_camera(p_state, region.rect.size, p_state.occlusion_texel, 1);
	_setup_instances(p_state, p_state.occlusion_views, region, p_state.occlusion_texel);
	_set_pass(p_state, PASS_GEOMETRY);
	_draw(p_state);
	_free_instances(p_state);
	const Ref<Image> image = _get_render(p_state, PASS_GEOMETRY);
	ERR_FAIL_COND(image.is_null() || image->get_size() != region.rect.size);

	// The height of the front of the geometry toward the direction of each view, from its center.
	const int pixel_count = image->get_width() * image->get_height();
	p_state.occlusion_width = image->get_width();
	p_state.occlusion_heights.resize(pixel_count);
	float *heights = p_state.occlusion_heights.ptrw();
	const float *pixels = reinterpret_cast<const float *>(image->ptr());
	for (int i = 0; i < pixel_count; i++) {
		const float *pixel = pixels + i * 4;
		const bool covered = pixel[3] >= 0.5 && pixel[2] < 0.9999;
		heights[i] = covered ? float(p_state.depth_extent * (1.0 - 2.0 * pixel[2])) : -Math::INF;
	}
}

void OctahedralImpostorBaker::OcclusionTask::compute_row(uint32_t p_row, void *p_unused) {
	// The cosine weighted fraction of the hemisphere of the normal that isn't blocked by the
	// geometry, from the depth of the occlusion views: a direction is blocked when the front of the
	// geometry toward it is above the point. The point is moved along the normal, and the depth is
	// compared with a slope scaled bias, so that the surface doesn't block itself.
	const LocalVector<View> &views = state->occlusion_views;
	const float *heights = state->occlusion_heights.ptr();
	const int width = state->occlusion_width;
	const real_t texel = state->occlusion_texel;
	const real_t half_size = OCCLUSION_VIEW_SIZE * 0.5;
	for (int x = 0; x < rect.size.x; x++) {
		const int index = p_row * rect.size.x + x;
		const Vector3 &normal = normals[index];
		if (normal.is_zero_approx()) {
			continue;
		}
		const Vector3 position = positions[index] + normal * (texel * 1.5);
		real_t visible = 0.0;
		real_t total = 0.0;
		for (const View &view : views) {
			const Vector3 direction = view.basis.get_column(2);
			const real_t facing = normal.dot(direction);
			if (facing <= 0.0) {
				continue;
			}
			total += facing;
			const Vector3 offset = position - view.center;
			const int view_x = int(Math::floor(offset.dot(view.basis.get_column(0)) / texel + half_size));
			const int view_y = int(Math::floor(half_size - offset.dot(view.basis.get_column(1)) / texel));
			if (view_x < 0 || view_y < 0 || view_x >= OCCLUSION_VIEW_SIZE || view_y >= OCCLUSION_VIEW_SIZE) {
				visible += facing;
				continue;
			}
			const real_t front = heights[(view.rect.position.y + view_y) * width + view.rect.position.x + view_x];
			const real_t slope = MIN(Math::sqrt(MAX(1.0 - facing * facing, 0.0)) / facing, 4.0);
			if (front <= offset.dot(direction) + texel * (1.0 + slope)) {
				visible += facing;
			}
		}
		const real_t visibility = total > 0.0 ? visible / total : 1.0;
		occlusion[(rect.position.y + p_row) * state->atlas_size.x + rect.position.x + x] = uint8_t(Math::fast_ftoi(CLAMP(visibility, 0.0, 1.0) * 255.0));
	}
}

void OctahedralImpostorBaker::_read_region(BakeState &p_state, Pass p_pass, const Region &p_region) {
	Ref<Image> image = _get_render(p_state, p_pass);
	ERR_FAIL_COND(image.is_null());
	if (p_pass == PASS_ALBEDO) {
		p_state.region_albedo = image;
	}
	ERR_FAIL_COND(p_state.region_albedo.is_null());

	const int ss = p_state.supersampling;
	const int atlas_width = p_state.atlas_size.x;
	const int width = image->get_width();
	ERR_FAIL_COND(width != p_region.rect.size.x * ss || image->get_height() != p_region.rect.size.y * ss);
	const float *data = reinterpret_cast<const float *>(image->ptr());
	const float *albedo = reinterpret_cast<const float *>(p_state.region_albedo->ptr());
	const float sample_weight = 1.0 / (ss * ss);

	// Output and data mask of the pass. The ORM pass also gives the mean roughness and metallic
	// without ORM atlas.
	uint8_t *output = nullptr;
	uint8_t *mask = nullptr;
	switch (p_pass) {
		case PASS_ALBEDO:
			output = albedo_image->ptrw();
			mask = p_state.coverage_mask.ptrw();
			break;
		case PASS_GEOMETRY:
			output = normal_depth_image->ptrw();
			mask = p_state.geometry_mask.ptrw();
			break;
		case PASS_ORM:
			if (orm_image.is_valid()) {
				output = orm_image->ptrw();
				mask = p_state.orm_mask.ptrw();
			}
			break;
		case PASS_TRANSLUCENCY:
			output = translucency_image->ptrw();
			mask = p_state.translucency_mask.ptrw();
			break;
		default:
			ERR_FAIL();
	}
	const uint8_t *occlusion = p_state.ambient_occlusion && p_pass == PASS_ORM ? p_state.occlusion.ptr() : nullptr;

	// Positions and normals of the pixels of the region in the space of the node, for the ambient
	// occlusion of the geometry.
	const bool compute_occlusion = p_pass == PASS_GEOMETRY && p_state.ambient_occlusion;
	LocalVector<Vector3> positions;
	LocalVector<Vector3> normals;
	if (compute_occlusion) {
		positions.resize_initialized(p_region.rect.get_area());
		normals.resize_initialized(p_region.rect.get_area());
	}

	for (uint32_t view_index : p_region.views) {
		const View &view = p_state.views[view_index];
		// The part of the view in the region.
		const Rect2i area = view.rect.intersection(p_region.rect);
		for (int py = area.position.y; py < area.get_end().y; py++) {
			for (int px = area.position.x; px < area.get_end().x; px++) {
				const int origin_x = (px - p_region.rect.position.x) * ss;
				const int origin_y = (py - p_region.rect.position.y) * ss;
				float sum[4] = { 0, 0, 0, 0 };
				float weight = 0.0;
				Vector3 normal_sum;
				for (int sy = 0; sy < ss; sy++) {
					for (int sx = 0; sx < ss; sx++) {
						const int offset = ((origin_y + sy) * width + origin_x + sx) * 4;
						const float *pixel = data + offset;
						const float alpha = CLAMP(albedo[offset + 3], 0.0f, 1.0f);
						if (p_pass == PASS_ALBEDO) {
							// Premultiplied by the coverage (transparent background).
							for (int c = 0; c < 4; c++) {
								sum[c] += pixel[c];
							}
						} else if (p_pass == PASS_GEOMETRY) {
							// Skips the background of the pass (the edges of alpha tested surfaces may
							// differ a bit from the albedo pass), and the surfaces that don't write
							// their depth (e.g. alpha blended).
							if (alpha <= 0.0 || pixel[3] < 0.5 || pixel[2] >= 0.9999) {
								continue;
							}
							normal_sum += OctahedralImpostorMaterial3D::octahedral_decode(Vector2(pixel[0], pixel[1]) * 2.0 - Vector2(1, 1), OctahedralImpostorMaterial3D::LAYOUT_FULL_SPHERE) * alpha;
							sum[3] += pixel[2] * alpha;
							weight += alpha;
						} else if (alpha > 0.0 && pixel[3] >= 0.5) {
							for (int c = 0; c < 3; c++) {
								sum[c] += pixel[c] * alpha;
							}
							weight += alpha;
						}
					}
				}

				const int index = py * atlas_width + px;
				uint8_t *out = output ? output + index * 4 : nullptr;
				if (p_pass == PASS_ALBEDO) {
					const float alpha = CLAMP(sum[3] * sample_weight, 0.0f, 1.0f);
					Color color;
					if (sum[3] > 0.0) {
						color = Color(sum[0] / sum[3], sum[1] / sum[3], sum[2] / sum[3]).clamp().linear_to_srgb();
					}
					out[0] = uint8_t(Math::fast_ftoi(color.r * 255.0));
					out[1] = uint8_t(Math::fast_ftoi(color.g * 255.0));
					out[2] = uint8_t(Math::fast_ftoi(color.b * 255.0));
					out[3] = uint8_t(Math::fast_ftoi(alpha * 255.0));
					mask[index] = out[3] > 0;
				} else if (p_pass == PASS_GEOMETRY) {
					if (weight > 0.0 && !normal_sum.is_zero_approx()) {
						const Vector3 view_normal = normal_sum.normalized();
						const Vector3 object_normal = view.basis.xform(view_normal);
						// Billboards: in the tangent space of the quad (the space of the view).
						const Vector3 normal = (p_state.billboard ? view_normal : object_normal) * 0.5 + Vector3(0.5, 0.5, 0.5);
						const float depth = CLAMP(sum[3] / weight, 0.0f, 1.0f);
						out[0] = uint8_t(Math::fast_ftoi(CLAMP(normal.x, 0.0f, 1.0f) * 255.0));
						out[1] = uint8_t(Math::fast_ftoi(CLAMP(normal.y, 0.0f, 1.0f) * 255.0));
						out[2] = uint8_t(Math::fast_ftoi(CLAMP(normal.z, 0.0f, 1.0f) * 255.0));
						out[3] = uint8_t(Math::fast_ftoi(depth * 255.0));
						mask[index] = 1;
						if (compute_occlusion) {
							// The center of the pixel, at the depth of the surface.
							const Vector2 offset = (Vector2(px, py) + Vector2(0.5, 0.5) - view.pixel_center) * p_state.texel_world;
							const int local_index = (py - p_region.rect.position.y) * p_region.rect.size.x + px - p_region.rect.position.x;
							positions[local_index] = view.center + view.basis.get_column(0) * offset.x - view.basis.get_column(1) * offset.y + view.basis.get_column(2) * (p_state.depth_extent * (1.0 - 2.0 * depth));
							normals[local_index] = object_normal;
						}
					}
				} else if (weight > 0.0) {
					float values[3];
					for (int c = 0; c < 3; c++) {
						values[c] = CLAMP(sum[c] / weight, 0.0f, 1.0f);
					}
					if (p_pass == PASS_ORM) {
						for (int c = 0; c < 3; c++) {
							p_state.orm_sum[c] += sum[c];
						}
						p_state.orm_weight += weight;
						if (occlusion) {
							// The ambient occlusion of the materials and of the geometry.
							values[0] *= occlusion[index] / 255.0f;
						}
					}
					if (out) {
						for (int c = 0; c < 3; c++) {
							out[c] = uint8_t(Math::fast_ftoi(values[c] * 255.0));
						}
						out[3] = 255;
						mask[index] = 1;
					}
				}
			}
		}
	}

	if (compute_occlusion) {
		OcclusionTask task;
		task.state = &p_state;
		task.rect = p_region.rect;
		task.positions = positions.ptr();
		task.normals = normals.ptr();
		task.occlusion = p_state.occlusion.ptrw();
		const WorkerThreadPool::GroupID group = WorkerThreadPool::get_singleton()->add_template_group_task(&task, &OcclusionTask::compute_row, (void *)nullptr, p_region.rect.size.y, -1, true, SNAME("OctahedralImpostorOcclusion"));
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(group);
	}
}

void OctahedralImpostorBaker::_dilate(Ref<Image> &p_image, const Vector<uint8_t> &p_mask, const LocalVector<View> &p_views, bool p_keep_alpha) {
	// Fills the empty pixels of each view with the mean of their filled neighbors, ring by ring,
	// so that filtering and mipmaps don't bring the background color in, without crossing views.
	const int size = p_image->get_width();
	uint8_t *data = p_image->ptrw();
	Vector<uint8_t> valid = p_mask;
	uint8_t *valid_ptr = valid.ptrw();
	Vector<uint8_t> queued;
	queued.resize_initialized(size * p_image->get_height());
	uint8_t *queued_ptr = queued.ptrw();
	LocalVector<int> ring;
	LocalVector<int> next_ring;
	LocalVector<uint8_t> ring_colors;
	const int channels = p_keep_alpha ? 3 : 4;

	for (const View &view : p_views) {
		const int x0 = view.rect.position.x;
		const int y0 = view.rect.position.y;
		const int x1 = view.rect.get_end().x;
		const int y1 = view.rect.get_end().y;

		ring.clear();
		for (int y = y0; y < y1; y++) {
			for (int x = x0; x < x1; x++) {
				const int index = y * size + x;
				if (valid_ptr[index]) {
					continue;
				}
				bool border = false;
				for (int dy = -1; dy <= 1 && !border; dy++) {
					for (int dx = -1; dx <= 1; dx++) {
						const int nx = x + dx;
						const int ny = y + dy;
						if (nx >= x0 && nx < x1 && ny >= y0 && ny < y1 && valid_ptr[ny * size + nx]) {
							border = true;
							break;
						}
					}
				}
				if (border) {
					ring.push_back(index);
					queued_ptr[index] = 1;
				}
			}
		}

		while (!ring.is_empty()) {
			ring_colors.resize(ring.size() * 4);
			for (uint32_t i = 0; i < ring.size(); i++) {
				const int x = ring[i] % size;
				const int y = ring[i] / size;
				int sum[4] = { 0, 0, 0, 0 };
				int count = 0;
				for (int dy = -1; dy <= 1; dy++) {
					for (int dx = -1; dx <= 1; dx++) {
						const int nx = x + dx;
						const int ny = y + dy;
						if (nx < x0 || nx >= x1 || ny < y0 || ny >= y1 || !valid_ptr[ny * size + nx]) {
							continue;
						}
						const uint8_t *neighbor = data + (ny * size + nx) * 4;
						for (int c = 0; c < 4; c++) {
							sum[c] += neighbor[c];
						}
						count++;
					}
				}
				for (int c = 0; c < 4; c++) {
					ring_colors[i * 4 + c] = count > 0 ? uint8_t((sum[c] + count / 2) / count) : 0;
				}
			}

			next_ring.clear();
			for (uint32_t i = 0; i < ring.size(); i++) {
				uint8_t *pixel = data + ring[i] * 4;
				for (int c = 0; c < channels; c++) {
					pixel[c] = ring_colors[i * 4 + c];
				}
				valid_ptr[ring[i]] = 1;
			}
			for (uint32_t i = 0; i < ring.size(); i++) {
				const int x = ring[i] % size;
				const int y = ring[i] / size;
				for (int dy = -1; dy <= 1; dy++) {
					for (int dx = -1; dx <= 1; dx++) {
						const int nx = x + dx;
						const int ny = y + dy;
						if (nx < x0 || nx >= x1 || ny < y0 || ny >= y1) {
							continue;
						}
						const int index = ny * size + nx;
						if (!valid_ptr[index] && !queued_ptr[index]) {
							queued_ptr[index] = 1;
							next_ring.push_back(index);
						}
					}
				}
			}
			SWAP(ring, next_ring);
		}
	}
}

void OctahedralImpostorBaker::_cleanup(BakeState &p_state) {
	RS *rs = RS::get_singleton();
	_free_instances(p_state);
	if (p_state.capture_instance.is_valid()) {
		rs->free_rid(p_state.capture_instance);
	}
	// The materials before their shaders.
	for (int i = int(p_state.owned_rids.size()) - 1; i >= 0; i--) {
		rs->free_rid(p_state.owned_rids[i]);
	}
	if (p_state.camera.is_valid()) {
		rs->free_rid(p_state.camera);
	}
	if (p_state.viewport.is_valid()) {
		rs->free_rid(p_state.viewport);
	}
	if (p_state.scenario.is_valid()) {
		rs->free_rid(p_state.scenario);
	}
	p_state = BakeState();
}

Error OctahedralImpostorBaker::bake(Node3D *p_node, bool p_show_progress) {
	ERR_FAIL_NULL_V(p_node, ERR_INVALID_PARAMETER);
	RS *rs = RS::get_singleton();
	const String rendering_method = rs->get_current_rendering_method();
	ERR_FAIL_COND_V_MSG(rendering_method.is_empty() || rendering_method == "dummy", ERR_UNAVAILABLE, "Baking an octahedral impostor or a billboard requires a renderer.");

	BakeState state;
	state.geometry = collect_geometry(p_node);
	ERR_FAIL_COND_V_MSG(state.geometry.is_empty(), ERR_INVALID_DATA, vformat("Node '%s' has no visible geometry to bake an octahedral impostor or a billboard from.", p_node->get_name()));

	Vector3 center;
	float radius = 0.0;
	ERR_FAIL_COND_V_MSG(!compute_bounding_sphere(state.geometry, center, radius), ERR_INVALID_DATA, "The geometry to bake an octahedral impostor or a billboard from is empty.");

	state.billboard = type == TYPE_BILLBOARD;
	const Error setup_error = state.billboard ? _setup_billboard_views(state, center, radius) : _setup_impostor_views(state, center, radius);
	if (setup_error != OK) {
		return setup_error;
	}
	// The geometry is in front of the camera from all the views (in the bounding sphere of an
	// impostor, centered on its views).
	for (const View &view : state.views) {
		state.depth_extent = MAX(state.depth_extent, view.center.distance_to(sphere_center) + sphere_radius);
	}
	state.margin_world = state.depth_extent * 0.1;
	state.depth_near = state.margin_world;
	state.depth_range = 2.0 * state.depth_extent;
	_estimate_surface_parameters(state.geometry);
	state.ambient_occlusion = bake_orm && bake_ambient_occlusion;
	state.translucency = bake_translucency && has_translucency(state.geometry);

	const Size2i atlas = state.atlas_size;
	const int pixel_count = atlas.x * atlas.y;
	albedo_image = Image::create_empty(atlas.x, atlas.y, false, Image::FORMAT_RGBA8);
	normal_depth_image = Image::create_empty(atlas.x, atlas.y, false, Image::FORMAT_RGBA8);
	if (state.billboard) {
		// A flat normal map where nothing is baked.
		normal_depth_image->fill(Color(0.5, 0.5, 1.0));
	}
	orm_image = bake_orm ? Image::create_empty(atlas.x, atlas.y, false, Image::FORMAT_RGBA8) : Ref<Image>();
	translucency_image = state.translucency ? Image::create_empty(atlas.x, atlas.y, false, Image::FORMAT_RGBA8) : Ref<Image>();
	state.coverage_mask.resize_initialized(pixel_count);
	state.geometry_mask.resize_initialized(pixel_count);
	if (bake_orm) {
		state.orm_mask.resize_initialized(pixel_count);
	}
	if (state.translucency) {
		state.translucency_mask.resize_initialized(pixel_count);
	}
	if (state.ambient_occlusion) {
		// Not occluded where nothing is computed.
		state.occlusion.resize(pixel_count);
		memset(state.occlusion.ptrw(), 255, pixel_count);
	}

	// Rendering setup: a scenario of its own, without environment nor lights, rendered to an HDR
	// viewport so that the data isn't quantized (the encoding of the output is calibrated below).
	state.use_normal_buffer = rendering_method == "forward_plus";
	state.scenario = rs->scenario_create();
	state.viewport = rs->viewport_create();
	rs->viewport_set_update_mode(state.viewport, RSE::VIEWPORT_UPDATE_DISABLED);
	rs->viewport_set_scenario(state.viewport, state.scenario);
	rs->viewport_set_transparent_background(state.viewport, true);
	rs->viewport_set_use_hdr_2d(state.viewport, true);
	rs->viewport_set_disable_2d(state.viewport, true);
	rs->viewport_set_msaa_3d(state.viewport, RSE::VIEWPORT_MSAA_DISABLED);
	rs->viewport_set_screen_space_aa(state.viewport, RSE::VIEWPORT_SCREEN_SPACE_AA_DISABLED);
	rs->viewport_set_use_debanding(state.viewport, false);
	rs->viewport_set_scaling_3d_mode(state.viewport, RSE::VIEWPORT_SCALING_3D_MODE_BILINEAR);
	rs->viewport_set_scaling_3d_scale(state.viewport, 1.0);
	rs->viewport_set_mesh_lod_threshold(state.viewport, 0.0);
	rs->viewport_set_use_occlusion_culling(state.viewport, false);
	rs->viewport_set_active(state.viewport, true);
	state.camera = rs->camera_create();
	rs->viewport_attach_camera(state.viewport, state.camera);

	if (state.use_normal_buffer) {
		Ref<QuadMesh> quad;
		quad.instantiate();
		state.capture_mesh = quad;
		Ref<Shader> capture_shader;
		capture_shader.instantiate();
		capture_shader->set_code(capture_shader_code);
		state.capture_material.instantiate();
		state.capture_material->set_shader(capture_shader);
		state.capture_material->set_render_priority(Material::RENDER_PRIORITY_MAX);
		state.capture_material->set_shader_parameter("depth_near", state.depth_near);
		state.capture_material->set_shader_parameter("depth_range", state.depth_range);
		state.capture_instance = rs->instance_create2(quad->get_rid(), state.scenario);
		rs->instance_geometry_set_material_override(state.capture_instance, state.capture_material->get_rid());
		rs->instance_geometry_set_cast_shadows_setting(state.capture_instance, RSE::SHADOW_CASTING_SETTING_OFF);
		rs->instance_set_custom_aabb(state.capture_instance, AABB(Vector3(-1, -1, -1) * 1e6, Vector3(2, 2, 2) * 1e6));
		rs->instance_set_visible(state.capture_instance, false);
	}

	// Some renderers output sRGB values (without HDR 2D, or when they tonemap in the scene shaders).
	state.srgb_output[0] = _is_output_srgb(state, PASS_ALBEDO);
	state.srgb_output[1] = _is_output_srgb(state, PASS_GEOMETRY);

	// The ORM pass also gives the mean roughness and metallic of the result without ORM atlas.
	LocalVector<Pass> passes;
	passes.push_back(PASS_ALBEDO);
	passes.push_back(PASS_GEOMETRY);
	passes.push_back(PASS_ORM);
	if (state.translucency) {
		passes.push_back(PASS_TRANSLUCENCY);
	}

	// All the views of a region are rendered at once, side by side as in the atlas, seen by an
	// orthographic camera looking down -Z.
	const int steps = state.regions.size() * passes.size() + (state.ambient_occlusion ? 1 : 0);

	EditorProgress *progress = nullptr;
	if (p_show_progress && EditorNode::get_singleton()) {
		progress = memnew(EditorProgress("bake_octahedral_impostor", state.billboard ? TTR("Bake Billboard") : TTR("Bake Octahedral Impostor"), steps + 1, true));
	}

	Error err = OK;
	int step = 0;
	if (state.ambient_occlusion) {
		if (progress && progress->step(TTR("Rendering the views for the ambient occlusion..."), step, false)) {
			err = ERR_SKIP;
		} else {
			_render_occlusion_views(state);
			// Without them (an error was reported), the ambient occlusion is the one of the materials.
			state.ambient_occlusion = !state.occlusion_heights.is_empty();
		}
		step++;
	}
	for (uint32_t region_index = 0; region_index < state.regions.size() && err == OK; region_index++) {
		const Region &region = state.regions[region_index];
		_setup_camera(state, region.rect.size, state.texel_world, state.supersampling);
		_setup_instances(state, state.views, region, state.texel_world);
		for (Pass pass : passes) {
			if (progress && progress->step(vformat(TTR("Rendering views (%d/%d)..."), step + 1, steps), step, false)) {
				err = ERR_SKIP;
				break;
			}
			_set_pass(state, pass);
			_draw(state);
			_read_region(state, pass, region);
			step++;
		}
		_free_instances(state);
	}
	const Vector<uint8_t> coverage_mask = state.coverage_mask;
	const Vector<uint8_t> geometry_mask = state.geometry_mask;
	const Vector<uint8_t> orm_mask = state.orm_mask;
	const Vector<uint8_t> translucency_mask = state.translucency_mask;
	const LocalVector<View> views(state.views);
	const real_t texel_world = state.texel_world;
	if (state.orm_weight > 0.0) {
		// The mean of the rendered materials, more accurate than their parameters.
		source_roughness = state.orm_sum[1] / state.orm_weight;
		source_metallic = state.orm_sum[2] / state.orm_weight;
	}
	_cleanup(state);

	if (err == OK) {
		if (progress) {
			progress->step(TTR("Filling the borders of the views..."), steps, false);
		}
		_dilate(albedo_image, coverage_mask, views, true);
		// Also fills the covered pixels without geometry data (e.g. alpha blended surfaces).
		_dilate(normal_depth_image, geometry_mask, views, false);
		if (type == TYPE_BILLBOARD) {
			// A normal map, without depth.
			normal_depth_image->convert(Image::FORMAT_RGB8);
		}
		if (orm_image.is_valid()) {
			_dilate(orm_image, orm_mask, views, false);
			orm_image->convert(Image::FORMAT_RGB8);
		}
		if (translucency_image.is_valid()) {
			_dilate(translucency_image, translucency_mask, views, false);
			translucency_image->convert(Image::FORMAT_RGB8);
		}
		baked_type = type;
		baked_frames = type == TYPE_OCTAHEDRAL_IMPOSTOR ? frames : 0;
		baked_layout = layout;
		baked_billboard_mode = billboard_mode;
		baked_views = views;
		baked_texel_size = texel_world;
	} else {
		albedo_image.unref();
		normal_depth_image.unref();
		orm_image.unref();
		translucency_image.unref();
	}
	if (progress) {
		memdelete(progress);
	}
	return err;
}

/* Results */

void OctahedralImpostorBaker::apply_to_material(const Ref<OctahedralImpostorMaterial3D> &p_material) const {
	ERR_FAIL_COND(p_material.is_null());
	ERR_FAIL_COND_MSG(baked_frames == 0, "No octahedral impostor was baked.");
	p_material->set_layout(baked_layout);
	p_material->set_frames(baked_frames);
	p_material->set_sphere_center(sphere_center);
	p_material->set_sphere_radius(sphere_radius);
	if (orm_image.is_valid()) {
		// Multipliers of the ORM atlas.
		p_material->set_roughness(1.0);
		p_material->set_metallic(1.0);
	} else {
		p_material->set_roughness(source_roughness);
		p_material->set_metallic(source_metallic);
	}
	if (translucency_image.is_valid() && !p_material->is_backlight_enabled()) {
		// The backlight color is added to the translucency atlas.
		p_material->set_backlight(Color(0, 0, 0));
		p_material->set_backlight_enabled(true);
	}
}

Ref<OctahedralImpostorMaterial3D> OctahedralImpostorBaker::create_material() const {
	ERR_FAIL_COND_V_MSG(albedo_image.is_null(), Ref<OctahedralImpostorMaterial3D>(), "Nothing was baked.");
	ERR_FAIL_COND_V_MSG(baked_type != TYPE_OCTAHEDRAL_IMPOSTOR, Ref<OctahedralImpostorMaterial3D>(), "The last bake is a billboard, use create_mesh() instead.");
	Ref<OctahedralImpostorMaterial3D> material;
	material.instantiate();
	apply_to_material(material);

	const Ref<Image> images[OctahedralImpostorMaterial3D::TEXTURE_MAX] = { albedo_image, normal_depth_image, orm_image, translucency_image };
	for (int i = 0; i < OctahedralImpostorMaterial3D::TEXTURE_MAX; i++) {
		if (images[i].is_null()) {
			continue;
		}
		Ref<Image> image = images[i]->duplicate();
		image->generate_mipmaps();
		material->set_texture(OctahedralImpostorMaterial3D::TextureParam(i), ImageTexture::create_from_image(image));
	}
	return material;
}

void OctahedralImpostorBaker::_apply_to_billboard_material(const Ref<BaseMaterial3D> &p_material, const Ref<Texture2D> p_textures[OctahedralImpostorMaterial3D::TEXTURE_MAX]) const {
	switch (baked_billboard_mode) {
		case BILLBOARD_FIXED_Y:
			p_material->set_billboard_mode(BaseMaterial3D::BILLBOARD_FIXED_Y);
			break;
		case BILLBOARD_ENABLED:
			p_material->set_billboard_mode(BaseMaterial3D::BILLBOARD_ENABLED);
			break;
		default:
			p_material->set_billboard_mode(BaseMaterial3D::BILLBOARD_DISABLED);
			break;
	}
	// Like the object, the quad follows the scale of the node (e.g. of the instances of a multimesh).
	p_material->set_flag(BaseMaterial3D::FLAG_BILLBOARD_KEEP_SCALE, baked_billboard_mode != BILLBOARD_CROSS);

	p_material->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, p_textures[OctahedralImpostorMaterial3D::TEXTURE_ALBEDO]);
	const Ref<Texture2D> &normal_texture = p_textures[OctahedralImpostorMaterial3D::TEXTURE_NORMAL_DEPTH];
	p_material->set_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING, normal_texture.is_valid());
	p_material->set_texture(BaseMaterial3D::TEXTURE_NORMAL, normal_texture);

	const Ref<Texture2D> &orm_texture = p_textures[OctahedralImpostorMaterial3D::TEXTURE_ORM];
	if (orm_texture.is_valid()) {
		// Multipliers of the ORM texture.
		p_material->set_roughness(1.0);
		p_material->set_metallic(1.0);
	} else {
		p_material->set_roughness(source_roughness);
		p_material->set_metallic(source_metallic);
	}
	p_material->set_feature(BaseMaterial3D::FEATURE_AMBIENT_OCCLUSION, orm_texture.is_valid());

	const Ref<Texture2D> &backlight_texture = p_textures[OctahedralImpostorMaterial3D::TEXTURE_BACKLIGHT];
	if (backlight_texture.is_valid() && !p_material->get_feature(BaseMaterial3D::FEATURE_BACKLIGHT)) {
		// The backlight color is added to the translucency texture.
		p_material->set_backlight(Color(0, 0, 0));
		p_material->set_feature(BaseMaterial3D::FEATURE_BACKLIGHT, true);
	}
	p_material->set_texture(BaseMaterial3D::TEXTURE_BACKLIGHT, backlight_texture);

	if (Object::cast_to<ORMMaterial3D>(p_material.ptr())) {
		p_material->set_texture(BaseMaterial3D::TEXTURE_ORM, orm_texture);
		return;
	}
	p_material->set_texture(BaseMaterial3D::TEXTURE_AMBIENT_OCCLUSION, orm_texture);
	p_material->set_texture(BaseMaterial3D::TEXTURE_ROUGHNESS, orm_texture);
	p_material->set_texture(BaseMaterial3D::TEXTURE_METALLIC, orm_texture);
	if (orm_texture.is_valid()) {
		p_material->set_ao_texture_channel(BaseMaterial3D::TEXTURE_CHANNEL_RED);
		p_material->set_roughness_texture_channel(BaseMaterial3D::TEXTURE_CHANNEL_GREEN);
		p_material->set_metallic_texture_channel(BaseMaterial3D::TEXTURE_CHANNEL_BLUE);
	}
}

static Ref<BaseMaterial3D> _create_billboard_material() {
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR);
	material->set_alpha_scissor_threshold(0.5);
	// The back of the planes of a cross is visible, and the quads of the other billboards face the
	// camera even in the shadow passes (they can be seen from the back by the lights).
	material->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
	// The views don't tile.
	material->set_flag(BaseMaterial3D::FLAG_USE_TEXTURE_REPEAT, false);
	return material;
}

Ref<Mesh> OctahedralImpostorBaker::_make_mesh(const Ref<Resource> &p_mesh, const Ref<Texture2D> p_textures[OctahedralImpostorMaterial3D::TEXTURE_MAX]) const {
	if (baked_type == TYPE_OCTAHEDRAL_IMPOSTOR) {
		Ref<PrimitiveMesh> mesh = p_mesh;
		if (mesh.is_null()) {
			Ref<QuadMesh> quad;
			quad.instantiate();
			mesh = quad;
		}
		Ref<OctahedralImpostorMaterial3D> material = mesh->get_material();
		if (material.is_null()) {
			material.instantiate();
		}
		apply_to_material(material);
		for (int i = 0; i < OctahedralImpostorMaterial3D::TEXTURE_MAX; i++) {
			material->set_texture(OctahedralImpostorMaterial3D::TextureParam(i), p_textures[i]);
		}
		mesh->set_material(material);
		// The quad turns toward the camera: culled with the bounds of the bounding sphere.
		mesh->set_custom_aabb(material->get_bounds());
		// E.g. a billboard before.
		mesh->set_meta(baked_billboard_meta, Variant());
		return mesh;
	}

	ERR_FAIL_COND_V(baked_views.is_empty(), Ref<Mesh>());
	if (baked_billboard_mode != BILLBOARD_CROSS) {
		Ref<QuadMesh> mesh = p_mesh;
		if (mesh.is_null()) {
			mesh.instantiate();
		}
		Ref<BaseMaterial3D> material = mesh->get_material();
		if (material.is_null()) {
			material = _create_billboard_material();
		}
		_apply_to_billboard_material(material, p_textures);

		// In the space of the billboard (right, up, toward the camera), the quad is offset from the
		// origin of the node like the geometry in the view.
		const View &view = baked_views[0];
		const Size2 size = Size2(view.rect.size) * baked_texel_size;
		const Vector3 offset = Vector3(view.basis.get_column(0).dot(view.center), view.basis.get_column(1).dot(view.center), 0.0);
		mesh->set_orientation(PlaneMesh::FACE_Z);
		mesh->set_size(size);
		mesh->set_center_offset(offset);
		mesh->set_material(material);

		// Bounds of the quad turned toward any camera.
		const real_t reach = Math::abs(offset.x) + size.x * 0.5; // From the vertical axis.
		AABB bounds;
		if (baked_billboard_mode == BILLBOARD_FIXED_Y) {
			bounds = AABB(Vector3(-reach, offset.y - size.y * 0.5, -reach), Vector3(reach * 2.0, size.y, reach * 2.0));
		} else {
			const real_t extent = Vector2(reach, Math::abs(offset.y) + size.y * 0.5).length();
			bounds = AABB(Vector3(-extent, -extent, -extent), Vector3(extent, extent, extent) * 2.0);
		}
		mesh->set_custom_aabb(bounds);
		mesh->set_meta(baked_billboard_meta, true);
		return mesh;
	}

	// Crossed planes, each with its view of the atlas, through the centers of the views.
	Ref<ArrayMesh> mesh = p_mesh;
	Ref<BaseMaterial3D> material;
	if (mesh.is_valid() && mesh->get_surface_count() > 0) {
		material = mesh->surface_get_material(0);
	}
	if (mesh.is_null()) {
		mesh.instantiate();
	}
	if (material.is_null()) {
		material = _create_billboard_material();
	}
	_apply_to_billboard_material(material, p_textures);

	const Vector2 texture_size = Vector2(albedo_image->get_size());
	PackedVector3Array vertices;
	PackedVector3Array normals;
	PackedFloat32Array tangents;
	PackedVector2Array uvs;
	PackedInt32Array indices;
	for (const View &view : baked_views) {
		const Vector3 right = view.basis.get_column(0);
		const Vector3 half_width = right * (view.rect.size.x * baked_texel_size * 0.5);
		const Vector3 half_height = view.basis.get_column(1) * (view.rect.size.y * baked_texel_size * 0.5);
		const Vector2 uv_begin = Vector2(view.rect.position) / texture_size;
		const Vector2 uv_end = Vector2(view.rect.get_end()) / texture_size;
		const int first = vertices.size();
		// Top left, top right, bottom right, bottom left: clockwise seen from the view (front faces).
		vertices.push_back(view.center - half_width + half_height);
		vertices.push_back(view.center + half_width + half_height);
		vertices.push_back(view.center + half_width - half_height);
		vertices.push_back(view.center - half_width - half_height);
		uvs.push_back(uv_begin);
		uvs.push_back(Vector2(uv_end.x, uv_begin.y));
		uvs.push_back(uv_end);
		uvs.push_back(Vector2(uv_begin.x, uv_end.y));
		for (int i = 0; i < 4; i++) {
			normals.push_back(view.basis.get_column(2));
			// The binormal (normal x tangent) is up in the view.
			tangents.push_back(right.x);
			tangents.push_back(right.y);
			tangents.push_back(right.z);
			tangents.push_back(1.0);
		}
		const int quad_indices[6] = { 0, 1, 2, 0, 2, 3 };
		for (int index : quad_indices) {
			indices.push_back(first + index);
		}
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = vertices;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_TANGENT] = tangents;
	arrays[Mesh::ARRAY_TEX_UV] = uvs;
	arrays[Mesh::ARRAY_INDEX] = indices;
	mesh->clear_surfaces();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	mesh->surface_set_material(0, material);
	mesh->set_custom_aabb(AABB());
	mesh->set_meta(baked_billboard_meta, true);
	return mesh;
}

Ref<Mesh> OctahedralImpostorBaker::create_mesh() const {
	ERR_FAIL_COND_V_MSG(albedo_image.is_null(), Ref<Mesh>(), "Nothing was baked.");
	const Ref<Image> images[OctahedralImpostorMaterial3D::TEXTURE_MAX] = { albedo_image, normal_depth_image, orm_image, translucency_image };
	Ref<Texture2D> textures[OctahedralImpostorMaterial3D::TEXTURE_MAX];
	for (int i = 0; i < OctahedralImpostorMaterial3D::TEXTURE_MAX; i++) {
		if (images[i].is_null()) {
			continue;
		}
		Ref<Image> image = images[i]->duplicate();
		// The normal map of a billboard is renormalized in its mipmaps.
		image->generate_mipmaps(baked_type == TYPE_BILLBOARD && i == OctahedralImpostorMaterial3D::TEXTURE_NORMAL_DEPTH);
		textures[i] = ImageTexture::create_from_image(image);
	}
	return _make_mesh(Ref<Resource>(), textures);
}

static void _write_texture_import_settings(const String &p_path, bool p_alpha_test, bool p_normal_map) {
	const String config_path = p_path + ".import";
	Ref<ConfigFile> config;
	config.instantiate();
	// Keeps the settings changed by the user after a previous bake.
	if (FileAccess::exists(config_path)) {
		config->load(config_path);
	}
	config->set_value("remap", "importer", "texture");
	config->set_value("remap", "type", "CompressedTexture2D");
	if (!config->has_section_key("params", "compress/mode")) {
		config->set_value("params", "compress/mode", 2); // VRAM Compressed.
		config->set_value("params", "compress/high_quality", true);
	}
	// The normals of an impostor are in object space, not a normal map. The normals of a billboard
	// are a normal map.
	config->set_value("params", "compress/normal_map", p_normal_map ? 1 : 2);
	config->set_value("params", "roughness/mode", 1);
	config->set_value("params", "mipmaps/generate", true);
	if (p_alpha_test && !config->has_section_key("params", "mipmaps/preserve_alpha_test_coverage")) {
		config->set_value("params", "mipmaps/preserve_alpha_test_coverage", true);
	}
	// The baker fills the borders of the views.
	config->set_value("params", "process/fix_alpha_border", false);
	config->set_value("params", "detect_3d/compress_to", 0);
	config->save(config_path);
}

Ref<Mesh> OctahedralImpostorBaker::save(const String &p_path) {
	ERR_FAIL_COND_V_MSG(albedo_image.is_null(), Ref<Mesh>(), "Nothing was baked.");
	ERR_FAIL_COND_V(p_path.is_empty(), Ref<Mesh>());
	EditorFileSystem *file_system = EditorFileSystem::get_singleton();
	ERR_FAIL_COND_V_MSG(file_system && file_system->is_importing(), Ref<Mesh>(), "Can't save an octahedral impostor or a billboard while the editor is importing files, try again when the import is done.");

	const bool billboard = baked_type == TYPE_BILLBOARD;
	const String base_path = p_path.get_basename();
	const Ref<Image> images[OctahedralImpostorMaterial3D::TEXTURE_MAX] = { albedo_image, normal_depth_image, orm_image, translucency_image };
	static const char *suffixes[OctahedralImpostorMaterial3D::TEXTURE_MAX] = { "_albedo.png", "_normal_depth.png", "_orm.png", "_translucency.png" };
	static const char *billboard_suffixes[OctahedralImpostorMaterial3D::TEXTURE_MAX] = { "_albedo.png", "_normal.png", "_orm.png", "_translucency.png" };
	Vector<String> texture_paths;
	for (int i = 0; i < OctahedralImpostorMaterial3D::TEXTURE_MAX; i++) {
		texture_paths.push_back(base_path + (billboard ? billboard_suffixes[i] : suffixes[i]));
	}
	Vector<String> saved_paths;
	for (int i = 0; i < OctahedralImpostorMaterial3D::TEXTURE_MAX; i++) {
		if (images[i].is_null()) {
			continue;
		}
		const String &path = texture_paths[i];
		_write_texture_import_settings(path, i == OctahedralImpostorMaterial3D::TEXTURE_ALBEDO, billboard && i == OctahedralImpostorMaterial3D::TEXTURE_NORMAL_DEPTH);
		const Error err = images[i]->save_png(path);
		ERR_FAIL_COND_V_MSG(err != OK, Ref<Mesh>(), vformat("Can't save the texture of the octahedral impostor or billboard to '%s'.", path));
		saved_paths.push_back(path);
	}

	if (file_system) {
		for (const String &path : saved_paths) {
			file_system->update_file(path);
		}
		file_system->reimport_files(saved_paths);
	}

	Ref<Texture2D> textures[OctahedralImpostorMaterial3D::TEXTURE_MAX];
	for (int i = 0; i < OctahedralImpostorMaterial3D::TEXTURE_MAX; i++) {
		if (images[i].is_valid()) {
			textures[i] = ResourceLoader::load(texture_paths[i], "Texture2D");
			ERR_FAIL_COND_V_MSG(textures[i].is_null(), Ref<Mesh>(), vformat("Can't load the texture of the octahedral impostor or billboard '%s'.", texture_paths[i]));
		}
	}

	// A rebake updates the existing mesh in place: the scenes that use it follow, and the settings
	// of its material that aren't baked (e.g. transparency, backlight) are kept.
	Ref<Resource> existing;
	if (ResourceLoader::exists(p_path)) {
		existing = ResourceLoader::load(p_path);
	}
	const Ref<Mesh> mesh = _make_mesh(existing, textures);
	ERR_FAIL_COND_V(mesh.is_null(), Ref<Mesh>());

	const Error err = ResourceSaver::save(mesh, p_path);
	ERR_FAIL_COND_V_MSG(err != OK, Ref<Mesh>(), vformat("Can't save the mesh of the octahedral impostor or billboard to '%s'.", p_path));
	if (mesh->get_path() != p_path) {
		mesh->set_path(p_path, true);
	}
	if (file_system) {
		file_system->update_file(p_path);
	}
	return mesh;
}

Error OctahedralImpostorBaker::_bake_bind(Node *p_node, bool p_show_progress) {
	Node3D *node = Object::cast_to<Node3D>(p_node);
	ERR_FAIL_NULL_V_MSG(node, ERR_INVALID_PARAMETER, "The node to bake an octahedral impostor or a billboard from must be a Node3D.");
	return bake(node, p_show_progress);
}

void OctahedralImpostorBaker::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_type", "type"), &OctahedralImpostorBaker::set_type);
	ClassDB::bind_method(D_METHOD("get_type"), &OctahedralImpostorBaker::get_type);
	ClassDB::bind_method(D_METHOD("set_layout", "layout"), &OctahedralImpostorBaker::set_layout);
	ClassDB::bind_method(D_METHOD("get_layout"), &OctahedralImpostorBaker::get_layout);
	ClassDB::bind_method(D_METHOD("set_frames", "frames"), &OctahedralImpostorBaker::set_frames);
	ClassDB::bind_method(D_METHOD("get_frames"), &OctahedralImpostorBaker::get_frames);
	ClassDB::bind_method(D_METHOD("set_billboard_mode", "mode"), &OctahedralImpostorBaker::set_billboard_mode);
	ClassDB::bind_method(D_METHOD("get_billboard_mode"), &OctahedralImpostorBaker::get_billboard_mode);
	ClassDB::bind_method(D_METHOD("set_cross_planes", "planes"), &OctahedralImpostorBaker::set_cross_planes);
	ClassDB::bind_method(D_METHOD("get_cross_planes"), &OctahedralImpostorBaker::get_cross_planes);
	ClassDB::bind_method(D_METHOD("set_atlas_size", "size"), &OctahedralImpostorBaker::set_atlas_size);
	ClassDB::bind_method(D_METHOD("get_atlas_size"), &OctahedralImpostorBaker::get_atlas_size);
	ClassDB::bind_method(D_METHOD("set_supersampling", "factor"), &OctahedralImpostorBaker::set_supersampling);
	ClassDB::bind_method(D_METHOD("get_supersampling"), &OctahedralImpostorBaker::get_supersampling);
	ClassDB::bind_method(D_METHOD("set_bake_orm", "enable"), &OctahedralImpostorBaker::set_bake_orm);
	ClassDB::bind_method(D_METHOD("is_baking_orm"), &OctahedralImpostorBaker::is_baking_orm);
	ClassDB::bind_method(D_METHOD("set_bake_ambient_occlusion", "enable"), &OctahedralImpostorBaker::set_bake_ambient_occlusion);
	ClassDB::bind_method(D_METHOD("is_baking_ambient_occlusion"), &OctahedralImpostorBaker::is_baking_ambient_occlusion);
	ClassDB::bind_method(D_METHOD("set_bake_translucency", "enable"), &OctahedralImpostorBaker::set_bake_translucency);
	ClassDB::bind_method(D_METHOD("is_baking_translucency"), &OctahedralImpostorBaker::is_baking_translucency);
	ClassDB::bind_method(D_METHOD("get_frame_size"), &OctahedralImpostorBaker::get_frame_size);

	ClassDB::bind_method(D_METHOD("bake", "node", "show_progress"), &OctahedralImpostorBaker::_bake_bind, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("get_albedo_image"), &OctahedralImpostorBaker::get_albedo_image);
	ClassDB::bind_method(D_METHOD("get_normal_depth_image"), &OctahedralImpostorBaker::get_normal_depth_image);
	ClassDB::bind_method(D_METHOD("get_orm_image"), &OctahedralImpostorBaker::get_orm_image);
	ClassDB::bind_method(D_METHOD("get_translucency_image"), &OctahedralImpostorBaker::get_translucency_image);
	ClassDB::bind_method(D_METHOD("get_sphere_center"), &OctahedralImpostorBaker::get_sphere_center);
	ClassDB::bind_method(D_METHOD("get_sphere_radius"), &OctahedralImpostorBaker::get_sphere_radius);
	ClassDB::bind_method(D_METHOD("create_material"), &OctahedralImpostorBaker::create_material);
	ClassDB::bind_method(D_METHOD("create_mesh"), &OctahedralImpostorBaker::create_mesh);
	ClassDB::bind_method(D_METHOD("save", "path"), &OctahedralImpostorBaker::save);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "type", PROPERTY_HINT_ENUM, "Octahedral Impostor,Billboard"), "set_type", "get_type");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "layout", PROPERTY_HINT_ENUM, "Hemisphere,Full Sphere"), "set_layout", "get_layout");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "frames", PROPERTY_HINT_RANGE, itos(OctahedralImpostorMaterial3D::MIN_FRAMES) + "," + itos(OctahedralImpostorMaterial3D::MAX_FRAMES) + ",1"), "set_frames", "get_frames");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "billboard_mode", PROPERTY_HINT_ENUM, "Y-Billboard,Enabled,Cross"), "set_billboard_mode", "get_billboard_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cross_planes", PROPERTY_HINT_RANGE, itos(MIN_CROSS_PLANES) + "," + itos(MAX_CROSS_PLANES) + ",1"), "set_cross_planes", "get_cross_planes");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "atlas_size", PROPERTY_HINT_RANGE, itos(MIN_ATLAS_SIZE) + "," + itos(MAX_ATLAS_SIZE) + ",1,suffix:px"), "set_atlas_size", "get_atlas_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "supersampling", PROPERTY_HINT_RANGE, "1," + itos(MAX_SUPERSAMPLING) + ",1"), "set_supersampling", "get_supersampling");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "bake_orm"), "set_bake_orm", "is_baking_orm");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "bake_ambient_occlusion"), "set_bake_ambient_occlusion", "is_baking_ambient_occlusion");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "bake_translucency"), "set_bake_translucency", "is_baking_translucency");

	BIND_ENUM_CONSTANT(TYPE_OCTAHEDRAL_IMPOSTOR);
	BIND_ENUM_CONSTANT(TYPE_BILLBOARD);
	BIND_ENUM_CONSTANT(TYPE_MAX);

	BIND_ENUM_CONSTANT(BILLBOARD_FIXED_Y);
	BIND_ENUM_CONSTANT(BILLBOARD_ENABLED);
	BIND_ENUM_CONSTANT(BILLBOARD_CROSS);
	BIND_ENUM_CONSTANT(BILLBOARD_MAX);
}

OctahedralImpostorBaker::OctahedralImpostorBaker() {
}
