/**************************************************************************/
/*  terrain_3d.cpp                                                        */
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

#include "terrain_3d.h"

#include "core/core_string_names.h"
#include "core/io/image.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/3d/physics/static_body_3d.h"
#include "scene/resources/3d/height_map_shape_3d.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "scene/resources/shader.h"
#include "servers/rendering/rendering_server.h"

namespace {
// Every layer texture is resampled to this size and stacked into a shared
// Texture2DArray (GPU texture arrays require every layer to share one size),
// so this also bounds how much a single terrain's splat textures can cost:
// up to (layer count) * LAYER_TEXTURE_SIZE^2 * 4 channels, times 3 arrays
// (albedo/normal/orm).
constexpr int LAYER_TEXTURE_SIZE = 512;
// Must match the `layer_uv_scales` uniform array size in the shader source
// below: shader uniform arrays are fixed-size, so this is the hard cap on
// how many distinct TerrainLayers a single Terrain3D can blend between.
constexpr int MAX_SHADER_LAYERS = 32;
} // namespace

void Terrain3D::init_shaders() {
	shader.instantiate();
	shader->set_code(R"(
shader_type spatial;
render_mode blend_mix, depth_draw_opaque, cull_back, diffuse_burley, specular_schlick_ggx;

uniform sampler2D control_map : filter_nearest;
uniform sampler2DArray albedo_array : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2DArray normal_array : hint_normal, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2DArray orm_array : filter_linear_mipmap_anisotropic, repeat_enable;
uniform float layer_uv_scales[32];
uniform vec3 terrain_origin = vec3(0.0);
uniform vec2 terrain_size = vec2(1.0, 1.0);

varying vec3 world_pos;

void vertex() {
	world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}

void fragment() {
	vec2 cm_uv = (world_pos.xz - terrain_origin.xz) / terrain_size;
	vec4 control = texture(control_map, cm_uv);
	int base_idx = int(round(control.r * 255.0));
	int overlay_idx = int(round(control.g * 255.0));
	float blend = control.b;

	vec2 base_uv = world_pos.xz / layer_uv_scales[base_idx];
	vec2 overlay_uv = world_pos.xz / layer_uv_scales[overlay_idx];

	vec4 albedo_base = texture(albedo_array, vec3(base_uv, float(base_idx)));
	vec4 albedo_overlay = texture(albedo_array, vec3(overlay_uv, float(overlay_idx)));
	ALBEDO = mix(albedo_base.rgb, albedo_overlay.rgb, blend);

	vec3 normal_base = texture(normal_array, vec3(base_uv, float(base_idx))).rgb;
	vec3 normal_overlay = texture(normal_array, vec3(overlay_uv, float(overlay_idx))).rgb;
	NORMAL_MAP = mix(normal_base, normal_overlay, blend);
	NORMAL_MAP_DEPTH = 1.0;

	vec4 orm_base = texture(orm_array, vec3(base_uv, float(base_idx)));
	vec4 orm_overlay = texture(orm_array, vec3(overlay_uv, float(overlay_idx)));
	vec4 orm = mix(orm_base, orm_overlay, blend);
	AO = orm.r;
	ROUGHNESS = orm.g;
	METALLIC = orm.b;
}
)");
}

void Terrain3D::finish_shaders() {
	shader.unref();
}

void Terrain3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_terrain_data", "data"), &Terrain3D::set_terrain_data);
	ClassDB::bind_method(D_METHOD("get_terrain_data"), &Terrain3D::get_terrain_data);

	ClassDB::bind_method(D_METHOD("set_layers", "layers"), &Terrain3D::set_layers);
	ClassDB::bind_method(D_METHOD("get_layers"), &Terrain3D::get_layers);

	ClassDB::bind_method(D_METHOD("set_skirt_depth", "depth"), &Terrain3D::set_skirt_depth);
	ClassDB::bind_method(D_METHOD("get_skirt_depth"), &Terrain3D::get_skirt_depth);

	ClassDB::bind_method(D_METHOD("set_lod_bias", "bias"), &Terrain3D::set_lod_bias);
	ClassDB::bind_method(D_METHOD("get_lod_bias"), &Terrain3D::get_lod_bias);

	ClassDB::bind_method(D_METHOD("set_cast_shadow", "setting"), &Terrain3D::set_cast_shadow);
	ClassDB::bind_method(D_METHOD("get_cast_shadow"), &Terrain3D::get_cast_shadow);

	ClassDB::bind_method(D_METHOD("set_gi_mode", "mode"), &Terrain3D::set_gi_mode);
	ClassDB::bind_method(D_METHOD("get_gi_mode"), &Terrain3D::get_gi_mode);

	ClassDB::bind_method(D_METHOD("set_collision_layer", "layer"), &Terrain3D::set_collision_layer);
	ClassDB::bind_method(D_METHOD("get_collision_layer"), &Terrain3D::get_collision_layer);

	ClassDB::bind_method(D_METHOD("set_collision_mask", "mask"), &Terrain3D::set_collision_mask);
	ClassDB::bind_method(D_METHOD("get_collision_mask"), &Terrain3D::get_collision_mask);

	ClassDB::bind_method(D_METHOD("set_debug_draw_chunks", "enable"), &Terrain3D::set_debug_draw_chunks);
	ClassDB::bind_method(D_METHOD("is_debug_draw_chunks_enabled"), &Terrain3D::is_debug_draw_chunks_enabled);

	ClassDB::bind_method(D_METHOD("sculpt", "local_position", "radius", "strength", "operation", "flatten_height", "update_collision"), &Terrain3D::sculpt, DEFVAL(0.0f), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("paint_layer", "local_position", "radius", "strength", "layer_index"), &Terrain3D::paint_layer);
	ClassDB::bind_method(D_METHOD("set_hole", "local_position", "radius", "hole", "update_collision"), &Terrain3D::set_hole, DEFVAL(true));

	ClassDB::bind_method(D_METHOD("get_height_region", "region"), &Terrain3D::get_height_region);
	ClassDB::bind_method(D_METHOD("set_height_region", "region", "heights", "update_collision"), &Terrain3D::set_height_region, DEFVAL(true));

	ClassDB::bind_method(D_METHOD("get_control_region", "region"), &Terrain3D::get_control_region);
	ClassDB::bind_method(D_METHOD("set_control_region", "region", "control"), &Terrain3D::set_control_region);

	ClassDB::bind_method(D_METHOD("update_collision"), &Terrain3D::update_collision);

	ClassDB::bind_method(D_METHOD("get_aabb"), &Terrain3D::get_aabb);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "terrain_data", PROPERTY_HINT_RESOURCE_TYPE, "TerrainData"), "set_terrain_data", "get_terrain_data");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "layers", PROPERTY_HINT_ARRAY_TYPE, MAKE_RESOURCE_TYPE_HINT("TerrainLayer")), "set_layers", "get_layers");

	ADD_GROUP("Rendering", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "skirt_depth", PROPERTY_HINT_RANGE, "0.0,100.0,0.01,or_greater,suffix:m"), "set_skirt_depth", "get_skirt_depth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lod_bias", PROPERTY_HINT_RANGE, "0.01,16.0,0.01,or_greater"), "set_lod_bias", "get_lod_bias");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cast_shadow", PROPERTY_HINT_ENUM, "Off,On,Double-Sided,Shadows Only"), "set_cast_shadow", "get_cast_shadow");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "gi_mode", PROPERTY_HINT_ENUM, "Disabled,Static,Dynamic"), "set_gi_mode", "get_gi_mode");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug_draw_chunks"), "set_debug_draw_chunks", "is_debug_draw_chunks_enabled");

	ADD_GROUP("Collision", "collision_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_layer", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_layer", "get_collision_layer");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_mask", "get_collision_mask");

	BIND_ENUM_CONSTANT(SCULPT_RAISE);
	BIND_ENUM_CONSTANT(SCULPT_LOWER);
	BIND_ENUM_CONSTANT(SCULPT_SMOOTH);
	BIND_ENUM_CONSTANT(SCULPT_FLATTEN);
}

void Terrain3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			if (chunks.is_empty() && terrain_data.is_valid()) {
				_rebuild_all_chunks();
			}
			if (terrain_data.is_valid()) {
				update_collision();
			}
		} break;

		case NOTIFICATION_ENTER_WORLD: {
			const RID scenario = get_world_3d().is_valid() ? get_world_3d()->get_scenario() : RID();
			for (KeyValue<Vector2i, Chunk> &kv : chunks) {
				if (kv.value.instance.is_valid()) {
					RS::get_singleton()->instance_set_scenario(kv.value.instance, scenario);
				}
			}
		} break;

		case NOTIFICATION_EXIT_WORLD: {
			for (KeyValue<Vector2i, Chunk> &kv : chunks) {
				if (kv.value.instance.is_valid()) {
					RS::get_singleton()->instance_set_scenario(kv.value.instance, RID());
				}
			}
		} break;

		case NOTIFICATION_TRANSFORM_CHANGED: {
			for (KeyValue<Vector2i, Chunk> &kv : chunks) {
				_update_chunk_transform(kv.value);
			}
			if (material.is_valid()) {
				material->set_shader_parameter("terrain_origin", get_global_transform().origin);
			}
		} break;

		case NOTIFICATION_VISIBILITY_CHANGED: {
			const bool vis = is_visible_in_tree();
			for (KeyValue<Vector2i, Chunk> &kv : chunks) {
				if (kv.value.instance.is_valid()) {
					RS::get_singleton()->instance_set_visible(kv.value.instance, vis);
				}
			}
		} break;
	}
}

Transform3D Terrain3D::_get_safe_global_transform() const {
	return is_inside_tree() ? get_global_transform() : Transform3D();
}

void Terrain3D::_ensure_material() {
	if (material.is_valid()) {
		return;
	}
	material.instantiate();
	material->set_shader(shader);
}

void Terrain3D::_rebuild_textures() {
	_ensure_material();
	if (terrain_data.is_null()) {
		return;
	}

	control_texture = ImageTexture::create_from_image(terrain_data->get_control_map_image());

	const int layer_count = CLAMP(layers.size(), 0, MAX_SHADER_LAYERS);
	const int array_layers = MAX(layer_count, 1);

	Vector<Ref<Image>> albedo_images;
	Vector<Ref<Image>> normal_images;
	Vector<Ref<Image>> orm_images;
	albedo_images.resize(array_layers);
	normal_images.resize(array_layers);
	orm_images.resize(array_layers);

	PackedFloat32Array uv_scales;
	uv_scales.resize(MAX_SHADER_LAYERS);
	for (int i = 0; i < MAX_SHADER_LAYERS; i++) {
		uv_scales.write[i] = 1.0f;
	}

	auto normalize_image = [](const Ref<Image> &p_src) -> Ref<Image> {
		Ref<Image> img;
		img.instantiate();
		img->copy_internals_from(p_src);
		if (img->is_compressed()) {
			img->decompress();
		}
		if (img->has_mipmaps()) {
			img->clear_mipmaps();
		}
		if (img->get_format() != Image::FORMAT_RGBA8) {
			img->convert(Image::FORMAT_RGBA8);
		}
		if (img->get_width() != LAYER_TEXTURE_SIZE || img->get_height() != LAYER_TEXTURE_SIZE) {
			img->resize(LAYER_TEXTURE_SIZE, LAYER_TEXTURE_SIZE);
		}
		return img;
	};
	auto default_image = [](const Color &p_color) -> Ref<Image> {
		Ref<Image> img;
		img.instantiate();
		img->initialize_data(LAYER_TEXTURE_SIZE, LAYER_TEXTURE_SIZE, false, Image::FORMAT_RGBA8);
		img->fill(p_color);
		return img;
	};

	for (int i = 0; i < array_layers; i++) {
		Ref<TerrainLayer> layer;
		if (i < layers.size()) {
			layer = layers[i];
		}

		Ref<Texture2D> albedo_tex = layer.is_valid() ? layer->get_albedo_texture() : Ref<Texture2D>();
		Ref<Texture2D> normal_tex = layer.is_valid() ? layer->get_normal_texture() : Ref<Texture2D>();
		Ref<Texture2D> orm_tex = layer.is_valid() ? layer->get_orm_texture() : Ref<Texture2D>();

		albedo_images.write[i] = (albedo_tex.is_valid() && albedo_tex->get_image().is_valid()) ? normalize_image(albedo_tex->get_image()) : default_image(Color(0.6, 0.6, 0.6, 1.0));
		normal_images.write[i] = (normal_tex.is_valid() && normal_tex->get_image().is_valid()) ? normalize_image(normal_tex->get_image()) : default_image(Color(0.5, 0.5, 1.0, 1.0));
		orm_images.write[i] = (orm_tex.is_valid() && orm_tex->get_image().is_valid()) ? normalize_image(orm_tex->get_image()) : default_image(Color(1.0, 0.5, 0.0, 1.0));

		if (i < MAX_SHADER_LAYERS) {
			uv_scales.write[i] = layer.is_valid() ? layer->get_uv_scale() : 1.0f;
		}
	}

	albedo_array.instantiate();
	albedo_array->create_from_images(albedo_images);
	normal_array.instantiate();
	normal_array->create_from_images(normal_images);
	orm_array.instantiate();
	orm_array->create_from_images(orm_images);

	material->set_shader_parameter("control_map", control_texture);
	material->set_shader_parameter("albedo_array", albedo_array);
	material->set_shader_parameter("normal_array", normal_array);
	material->set_shader_parameter("orm_array", orm_array);
	material->set_shader_parameter("layer_uv_scales", uv_scales);
	material->set_shader_parameter("terrain_size", Vector2(terrain_data->get_size(), terrain_data->get_size()));
	material->set_shader_parameter("terrain_origin", _get_safe_global_transform().origin);

	for (KeyValue<Vector2i, Chunk> &kv : chunks) {
		if (kv.value.mesh.is_valid()) {
			kv.value.mesh->surface_set_material(0, material);
		}
	}
}

Vector2i Terrain3D::_get_chunk_grid_size() const {
	if (terrain_data.is_null()) {
		return Vector2i();
	}
	const int quads_total = MAX(terrain_data->get_resolution() - 1, 1);
	const int n = (quads_total + CHUNK_QUADS - 1) / CHUNK_QUADS;
	return Vector2i(MAX(n, 1), MAX(n, 1));
}

Rect2i Terrain3D::_get_chunk_range_for_region(const Rect2i &p_vertex_region) const {
	const Vector2i grid = _get_chunk_grid_size();
	if (grid.x <= 0 || grid.y <= 0) {
		return Rect2i();
	}

	const int min_vx = p_vertex_region.position.x;
	const int min_vz = p_vertex_region.position.y;
	const int max_vx = p_vertex_region.position.x + MAX(p_vertex_region.size.x, 1) - 1;
	const int max_vz = p_vertex_region.position.y + MAX(p_vertex_region.size.y, 1) - 1;

	const int cx0 = CLAMP((int)Math::floor((float)min_vx / CHUNK_QUADS), 0, grid.x - 1);
	const int cz0 = CLAMP((int)Math::floor((float)min_vz / CHUNK_QUADS), 0, grid.y - 1);
	const int cx1 = CLAMP((int)Math::floor((float)max_vx / CHUNK_QUADS), 0, grid.x - 1);
	const int cz1 = CLAMP((int)Math::floor((float)max_vz / CHUNK_QUADS), 0, grid.y - 1);

	return Rect2i(cx0, cz0, cx1 - cx0 + 1, cz1 - cz0 + 1);
}

void Terrain3D::_rebuild_all_chunks() {
	_clear_chunks();
	if (terrain_data.is_null()) {
		return;
	}
	const Vector2i grid = _get_chunk_grid_size();
	for (int z = 0; z < grid.y; z++) {
		for (int x = 0; x < grid.x; x++) {
			_rebuild_chunk(Vector2i(x, z));
		}
	}
}

void Terrain3D::_rebuild_chunks_in_region(const Rect2i &p_vertex_region) {
	if (terrain_data.is_null()) {
		return;
	}
	const Rect2i range = _get_chunk_range_for_region(p_vertex_region);
	for (int z = range.position.y; z < range.position.y + range.size.y; z++) {
		for (int x = range.position.x; x < range.position.x + range.size.x; x++) {
			_rebuild_chunk(Vector2i(x, z));
		}
	}
}

void Terrain3D::_clear_chunks() {
	for (KeyValue<Vector2i, Chunk> &kv : chunks) {
		if (kv.value.instance.is_valid()) {
			RS::get_singleton()->free_rid(kv.value.instance);
		}
	}
	chunks.clear();
}

void Terrain3D::_update_chunk_transform(Chunk &p_chunk) {
	if (!p_chunk.instance.is_valid()) {
		return;
	}
	RS::get_singleton()->instance_set_transform(p_chunk.instance, _get_safe_global_transform() * Transform3D(Basis(), p_chunk.local_origin));
}

void Terrain3D::_apply_render_settings_to_chunk(const Chunk &p_chunk) {
	if (!p_chunk.instance.is_valid()) {
		return;
	}
	RS::get_singleton()->instance_geometry_set_cast_shadows_setting(p_chunk.instance, (RSE::ShadowCastingSetting)cast_shadow);

	const bool baked = gi_mode == GeometryInstance3D::GI_MODE_STATIC;
	const bool dynamic = gi_mode == GeometryInstance3D::GI_MODE_DYNAMIC;
	RS::get_singleton()->instance_geometry_set_flag(p_chunk.instance, RSE::INSTANCE_FLAG_USE_BAKED_LIGHT, baked);
	RS::get_singleton()->instance_geometry_set_flag(p_chunk.instance, RSE::INSTANCE_FLAG_USE_DYNAMIC_GI, dynamic);

	RS::get_singleton()->instance_set_visible(p_chunk.instance, is_visible_in_tree());
}

void Terrain3D::_rebuild_chunk(const Vector2i &p_coord) {
	if (terrain_data.is_null()) {
		return;
	}
	_ensure_material();

	const int base_ix = p_coord.x * CHUNK_QUADS;
	const int base_iz = p_coord.y * CHUNK_QUADS;
	const float spacing = terrain_data->get_vertex_spacing();
	const float res_minus_1 = MAX((float)(terrain_data->get_resolution() - 1), 1.0f);

	const int verts_per_side = CHUNK_QUADS + 1;
	const int main_count = verts_per_side * verts_per_side;
	const int perim_count = 4 * CHUNK_QUADS;
	const int total_verts = main_count + perim_count;

	auto main_index = [verts_per_side](int jx, int jz) {
		return jz * verts_per_side + jx;
	};
	auto perim_point = [](int p, int &jx, int &jz) {
		if (p < CHUNK_QUADS) {
			jx = p;
			jz = 0;
		} else if (p < 2 * CHUNK_QUADS) {
			jx = CHUNK_QUADS;
			jz = p - CHUNK_QUADS;
		} else if (p < 3 * CHUNK_QUADS) {
			jx = CHUNK_QUADS - (p - 2 * CHUNK_QUADS);
			jz = CHUNK_QUADS;
		} else {
			jx = 0;
			jz = CHUNK_QUADS - (p - 3 * CHUNK_QUADS);
		}
	};

	PackedVector3Array positions;
	PackedVector3Array normals;
	PackedFloat32Array tangents;
	PackedVector2Array uvs;
	positions.resize(total_verts);
	normals.resize(total_verts);
	tangents.resize(total_verts * 4);
	uvs.resize(total_verts);

	// Fetch this chunk's heights in one bulk call (padded by a 1-sample halo
	// for the central-difference normals/tangents below), instead of the
	// O(chunk_quads^2) individual TerrainData::get_height/get_normal calls
	// this used to make: a single chunk touches on the order of 10k samples,
	// and per-call Image access dominates at that volume (see TerrainData).
	const Rect2i height_fetch_region(base_ix - 1, base_iz - 1, CHUNK_QUADS + 3, CHUNK_QUADS + 3);
	const PackedFloat32Array height_data = terrain_data->get_height_region(height_fetch_region);
	const int height_w = height_fetch_region.size.x;
	auto sample_height = [&](int ix, int iz) {
		return height_data[(iz - height_fetch_region.position.y) * height_w + (ix - height_fetch_region.position.x)];
	};

	for (int jz = 0; jz <= CHUNK_QUADS; jz++) {
		for (int jx = 0; jx <= CHUNK_QUADS; jx++) {
			const int ix = base_ix + jx;
			const int iz = base_iz + jz;
			const int vi = main_index(jx, jz);

			const float h = sample_height(ix, iz);
			const float h_l = sample_height(ix - 1, iz);
			const float h_r = sample_height(ix + 1, iz);
			const float h_d = sample_height(ix, iz - 1);
			const float h_u = sample_height(ix, iz + 1);

			positions.set(vi, Vector3(jx * spacing, h, jz * spacing));
			normals.set(vi, Vector3(h_l - h_r, 2.0f * spacing, h_d - h_u).normalized());

			const float dh_dx = (h_r - h_l) / (2.0f * spacing);
			const Vector3 tangent = Vector3(1.0f, dh_dx, 0.0f).normalized();
			tangents.set(vi * 4 + 0, tangent.x);
			tangents.set(vi * 4 + 1, tangent.y);
			tangents.set(vi * 4 + 2, tangent.z);
			tangents.set(vi * 4 + 3, 1.0f);

			uvs.set(vi, Vector2(ix / res_minus_1, iz / res_minus_1));
		}
	}

	for (int p = 0; p < perim_count; p++) {
		int jx, jz;
		perim_point(p, jx, jz);
		const int top_vi = main_index(jx, jz);
		const int vi = main_count + p;

		positions.set(vi, positions[top_vi] - Vector3(0, skirt_depth, 0));
		normals.set(vi, normals[top_vi]);
		tangents.set(vi * 4 + 0, tangents[top_vi * 4 + 0]);
		tangents.set(vi * 4 + 1, tangents[top_vi * 4 + 1]);
		tangents.set(vi * 4 + 2, tangents[top_vi * 4 + 2]);
		tangents.set(vi * 4 + 3, tangents[top_vi * 4 + 3]);
		uvs.set(vi, uvs[top_vi]);
	}

	// Same reasoning as the height fetch above: one bulk read of this chunk's
	// control data (alpha channel only matters here) instead of one
	// TerrainData::is_hole call - and its own Image access - per quad corner,
	// re-checked again for every coarser LOD level below.
	const Rect2i control_fetch_region(base_ix, base_iz, CHUNK_QUADS + 1, CHUNK_QUADS + 1);
	const PackedColorArray control_data = terrain_data->get_control_region(control_fetch_region);
	const int control_w = control_fetch_region.size.x;
	auto is_hole_at = [&](int jx, int jz) {
		return control_data[jz * control_w + jx].a > 0.5f;
	};

	auto build_indices_for_stride = [&](int stride) -> PackedInt32Array {
		PackedInt32Array idx;
		for (int jz = stride; jz <= CHUNK_QUADS; jz += stride) {
			for (int jx = stride; jx <= CHUNK_QUADS; jx += stride) {
				if (is_hole_at(jx - stride, jz - stride) || is_hole_at(jx, jz - stride) || is_hole_at(jx - stride, jz) || is_hole_at(jx, jz)) {
					continue;
				}
				const int a = main_index(jx - stride, jz - stride);
				const int b = main_index(jx, jz - stride);
				const int c = main_index(jx - stride, jz);
				const int d = main_index(jx, jz);
				idx.push_back(a);
				idx.push_back(b);
				idx.push_back(c);
				idx.push_back(b);
				idx.push_back(d);
				idx.push_back(c);
			}
		}

		// Skirts: a vertical apron around the chunk border that hides small
		// cracks between chunks rendered at different LODs. Emitted with both
		// winding orders so it stays visible under backface culling regardless
		// of which way this particular loop happens to wind.
		const int perim_step_count = perim_count / stride;
		for (int s = 0; s < perim_step_count; s++) {
			const int p0 = s * stride;
			const int p1 = (p0 + stride) % perim_count;
			int jx0, jz0, jx1, jz1;
			perim_point(p0, jx0, jz0);
			perim_point(p1, jx1, jz1);
			const int top_a = main_index(jx0, jz0);
			const int top_b = main_index(jx1, jz1);
			const int bot_a = main_count + p0;
			const int bot_b = main_count + p1;

			idx.push_back(top_a);
			idx.push_back(top_b);
			idx.push_back(bot_a);
			idx.push_back(top_b);
			idx.push_back(bot_b);
			idx.push_back(bot_a);

			idx.push_back(top_a);
			idx.push_back(bot_a);
			idx.push_back(top_b);
			idx.push_back(bot_a);
			idx.push_back(bot_b);
			idx.push_back(top_b);
		}
		return idx;
	};

	const PackedInt32Array base_indices = build_indices_for_stride(1);

	Chunk &chunk = chunks[p_coord];
	chunk.local_origin = Vector3(base_ix * spacing, 0, base_iz * spacing);

	if (base_indices.is_empty()) {
		// Every quad in this chunk is a hole: nothing to draw or collide with.
		if (chunk.instance.is_valid()) {
			RS::get_singleton()->free_rid(chunk.instance);
			chunk.instance = RID();
		}
		chunk.mesh.unref();
		return;
	}

	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = positions;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_TANGENT] = tangents;
	arrays[Mesh::ARRAY_TEX_UV] = uvs;
	arrays[Mesh::ARRAY_INDEX] = base_indices;

	Dictionary lods;
	for (int lod = 1; (1 << lod) <= CHUNK_QUADS; lod++) {
		const int stride = 1 << lod;
		const PackedInt32Array lod_indices = build_indices_for_stride(stride);
		if (lod_indices.is_empty()) {
			continue;
		}
		const float edge_length = spacing * (float)stride / MAX(lod_bias, 0.001f);
		lods[edge_length] = lod_indices;
	}

	Ref<ArrayMesh> array_mesh;
	array_mesh.instantiate();
	array_mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays, TypedArray<Array>(), lods);
	array_mesh->surface_set_material(0, material);
	chunk.mesh = array_mesh;

	if (!chunk.instance.is_valid()) {
		const RID scenario = (is_inside_tree() && get_world_3d().is_valid()) ? get_world_3d()->get_scenario() : RID();
		chunk.instance = RS::get_singleton()->instance_create2(chunk.mesh->get_rid(), scenario);
	} else {
		RS::get_singleton()->instance_set_base(chunk.instance, chunk.mesh->get_rid());
	}

	_update_chunk_transform(chunk);
	_apply_render_settings_to_chunk(chunk);
}

void Terrain3D::_ensure_collision_nodes() {
	if (collision_body != nullptr) {
		return;
	}
	collision_body = memnew(StaticBody3D);
	add_child(collision_body, false, INTERNAL_MODE_FRONT);

	collision_shape.instantiate();

	collision_shape_node = memnew(CollisionShape3D);
	collision_shape_node->set_shape(collision_shape);
	collision_body->add_child(collision_shape_node, false, INTERNAL_MODE_FRONT);
}

void Terrain3D::_on_layers_changed() {
	_rebuild_textures();
}

void Terrain3D::_on_terrain_data_changed() {
	// Coarse fallback for edits made directly to the TerrainData resource
	// instead of through this node's own sculpt()/paint_layer()/set_hole()
	// (which already know exactly which region to refresh, and refresh only
	// that). This has no way to know what changed, so it rebuilds everything.
	_rebuild_textures();
	_rebuild_all_chunks();
	update_collision();
}

void Terrain3D::set_terrain_data(const Ref<TerrainData> &p_data) {
	if (terrain_data.is_valid()) {
		terrain_data->disconnect_changed(callable_mp(this, &Terrain3D::_on_terrain_data_changed));
	}
	terrain_data = p_data;
	if (terrain_data.is_valid()) {
		terrain_data->connect_changed(callable_mp(this, &Terrain3D::_on_terrain_data_changed));
	}

	_rebuild_textures();
	_rebuild_all_chunks();
	if (is_inside_tree()) {
		update_collision();
	}
	update_configuration_warnings();
}

Ref<TerrainData> Terrain3D::get_terrain_data() const {
	return terrain_data;
}

void Terrain3D::set_layers(const TypedArray<TerrainLayer> &p_layers) {
	for (int i = 0; i < layers.size(); i++) {
		Ref<TerrainLayer> old_layer = layers[i];
		if (old_layer.is_valid()) {
			old_layer->disconnect(CoreStringName(changed), callable_mp(this, &Terrain3D::_on_layers_changed));
		}
	}

	layers = p_layers;

	for (int i = 0; i < layers.size(); i++) {
		Ref<TerrainLayer> layer = layers[i];
		if (layer.is_valid()) {
			layer->connect(CoreStringName(changed), callable_mp(this, &Terrain3D::_on_layers_changed));
		}
	}

	_rebuild_textures();
	update_configuration_warnings();
}

TypedArray<TerrainLayer> Terrain3D::get_layers() const {
	return layers;
}

void Terrain3D::set_skirt_depth(float p_depth) {
	skirt_depth = MAX(p_depth, 0.0f);
	_rebuild_all_chunks();
}

float Terrain3D::get_skirt_depth() const {
	return skirt_depth;
}

void Terrain3D::set_lod_bias(float p_bias) {
	lod_bias = MAX(p_bias, 0.001f);
	_rebuild_all_chunks();
}

float Terrain3D::get_lod_bias() const {
	return lod_bias;
}

void Terrain3D::set_cast_shadow(GeometryInstance3D::ShadowCastingSetting p_setting) {
	cast_shadow = p_setting;
	for (KeyValue<Vector2i, Chunk> &kv : chunks) {
		_apply_render_settings_to_chunk(kv.value);
	}
}

GeometryInstance3D::ShadowCastingSetting Terrain3D::get_cast_shadow() const {
	return cast_shadow;
}

void Terrain3D::set_gi_mode(GeometryInstance3D::GIMode p_mode) {
	gi_mode = p_mode;
	for (KeyValue<Vector2i, Chunk> &kv : chunks) {
		_apply_render_settings_to_chunk(kv.value);
	}
}

GeometryInstance3D::GIMode Terrain3D::get_gi_mode() const {
	return gi_mode;
}

void Terrain3D::set_collision_layer(uint32_t p_layer) {
	collision_layer = p_layer;
	if (collision_body != nullptr) {
		collision_body->set_collision_layer(collision_layer);
	}
}

uint32_t Terrain3D::get_collision_layer() const {
	return collision_layer;
}

void Terrain3D::set_collision_mask(uint32_t p_mask) {
	collision_mask = p_mask;
	if (collision_body != nullptr) {
		collision_body->set_collision_mask(collision_mask);
	}
}

uint32_t Terrain3D::get_collision_mask() const {
	return collision_mask;
}

void Terrain3D::set_debug_draw_chunks(bool p_enable) {
	debug_draw_chunks = p_enable;
	update_gizmos();
}

bool Terrain3D::is_debug_draw_chunks_enabled() const {
	return debug_draw_chunks;
}

void Terrain3D::sculpt(const Vector3 &p_local_position, float p_radius, float p_strength, SculptOperation p_operation, float p_flatten_height, bool p_update_collision) {
	ERR_FAIL_COND(terrain_data.is_null());

	const float spacing = terrain_data->get_vertex_spacing();
	const int resolution = terrain_data->get_resolution();
	const float radius = MAX(p_radius, 0.001f);
	const int rad_idx = (int)Math::ceil(radius / spacing) + 1;
	const int cx = (int)Math::round(p_local_position.x / spacing);
	const int cz = (int)Math::round(p_local_position.z / spacing);
	const int x0 = CLAMP(cx - rad_idx, 0, resolution - 1);
	const int x1 = CLAMP(cx + rad_idx, 0, resolution - 1);
	const int z0 = CLAMP(cz - rad_idx, 0, resolution - 1);
	const int z1 = CLAMP(cz + rad_idx, 0, resolution - 1);
	if (x1 < x0 || z1 < z0) {
		return;
	}

	const Rect2i region(x0, z0, x1 - x0 + 1, z1 - z0 + 1);
	// Bulk fetch-modify-commit instead of one TerrainData::get_height/
	// set_height call per touched sample: a single stamp can touch
	// thousands of samples at a large brush radius, and each of those calls
	// used to mean its own Image access (see TerrainData for why that matters).
	PackedFloat32Array heights = terrain_data->get_height_region(region);
	const int region_w = region.size.x;

	// SMOOTH averages each vertex with its neighbors; it needs to read from an
	// unmodified snapshot of the touched (padded by one ring) region rather
	// than sampling this same stamp's own in-progress writes.
	const Rect2i snapshot_region(x0 - 1, z0 - 1, x1 - x0 + 3, z1 - z0 + 3);
	PackedFloat32Array before;
	if (p_operation == SCULPT_SMOOTH) {
		before = terrain_data->get_height_region(snapshot_region);
	}
	const int snapshot_w = snapshot_region.size.x;
	auto sample_before = [&](int x, int z) -> float {
		x = CLAMP(x, snapshot_region.position.x, snapshot_region.position.x + snapshot_region.size.x - 1);
		z = CLAMP(z, snapshot_region.position.y, snapshot_region.position.y + snapshot_region.size.y - 1);
		return before[(z - snapshot_region.position.y) * snapshot_w + (x - snapshot_region.position.x)];
	};

	for (int z = z0; z <= z1; z++) {
		for (int x = x0; x <= x1; x++) {
			const float dx = (x - cx) * spacing;
			const float dz = (z - cz) * spacing;
			const float dist = Math::sqrt(dx * dx + dz * dz);
			if (dist > radius) {
				continue;
			}
			const float t = 1.0f - dist / radius;
			const float falloff = t * t * (3.0f - 2.0f * t);
			const float amount = p_strength * falloff;

			const int local_idx = (z - z0) * region_w + (x - x0);
			float h = heights[local_idx];
			switch (p_operation) {
				case SCULPT_RAISE: {
					h += amount;
				} break;
				case SCULPT_LOWER: {
					h -= amount;
				} break;
				case SCULPT_FLATTEN: {
					h = Math::lerp(h, p_flatten_height, CLAMP(amount, 0.0f, 1.0f));
				} break;
				case SCULPT_SMOOTH: {
					float sum = 0.0f;
					for (int nz = -1; nz <= 1; nz++) {
						for (int nx = -1; nx <= 1; nx++) {
							sum += sample_before(x + nx, z + nz);
						}
					}
					h = Math::lerp(h, sum / 9.0f, CLAMP(amount, 0.0f, 1.0f));
				} break;
			}
			heights.set(local_idx, h);
		}
	}

	terrain_data->set_height_region(region, heights);
	_rebuild_chunks_in_region(region);
	if (p_update_collision) {
		update_collision();
	}
}

void Terrain3D::paint_layer(const Vector3 &p_local_position, float p_radius, float p_strength, int p_layer_index) {
	ERR_FAIL_COND(terrain_data.is_null());
	ERR_FAIL_INDEX(p_layer_index, layers.size());
	ERR_FAIL_INDEX(p_layer_index, MAX_SHADER_LAYERS);

	const float spacing = terrain_data->get_vertex_spacing();
	const int resolution = terrain_data->get_resolution();
	const float radius = MAX(p_radius, 0.001f);
	const int rad_idx = (int)Math::ceil(radius / spacing) + 1;
	const int cx = (int)Math::round(p_local_position.x / spacing);
	const int cz = (int)Math::round(p_local_position.z / spacing);
	const int x0 = CLAMP(cx - rad_idx, 0, resolution - 1);
	const int x1 = CLAMP(cx + rad_idx, 0, resolution - 1);
	const int z0 = CLAMP(cz - rad_idx, 0, resolution - 1);
	const int z1 = CLAMP(cz + rad_idx, 0, resolution - 1);
	if (x1 < x0 || z1 < z0) {
		return;
	}

	const Rect2i region(x0, z0, x1 - x0 + 1, z1 - z0 + 1);
	// Bulk fetch-modify-commit, same reasoning as sculpt() above.
	PackedColorArray control = terrain_data->get_control_region(region);
	const int region_w = region.size.x;

	for (int z = z0; z <= z1; z++) {
		for (int x = x0; x <= x1; x++) {
			const float dx = (x - cx) * spacing;
			const float dz = (z - cz) * spacing;
			const float dist = Math::sqrt(dx * dx + dz * dz);
			if (dist > radius) {
				continue;
			}
			const float t = 1.0f - dist / radius;
			const float amount = p_strength * t * t * (3.0f - 2.0f * t);

			const int local_idx = (z - z0) * region_w + (x - x0);
			Color c = control[local_idx];
			int base = (int)Math::round(c.r * 255.0f);
			int overlay = (int)Math::round(c.g * 255.0f);
			float blend = c.b;

			if (base == p_layer_index) {
				blend = MAX(blend - amount, 0.0f);
			} else if (overlay == p_layer_index) {
				blend = MIN(blend + amount, 1.0f);
			} else {
				overlay = p_layer_index;
				blend = MIN(blend + amount, 1.0f);
			}
			if (blend >= 1.0f) {
				base = overlay;
				blend = 0.0f;
			}
			control.set(local_idx, Color(base / 255.0f, overlay / 255.0f, blend, c.a));
		}
	}

	terrain_data->set_control_region(region, control);
	if (control_texture.is_valid()) {
		control_texture->update(terrain_data->get_control_map_image());
	}
}

void Terrain3D::set_hole(const Vector3 &p_local_position, float p_radius, bool p_hole, bool p_update_collision) {
	ERR_FAIL_COND(terrain_data.is_null());

	const float spacing = terrain_data->get_vertex_spacing();
	const int resolution = terrain_data->get_resolution();
	const float radius = MAX(p_radius, 0.001f);
	const int rad_idx = (int)Math::ceil(radius / spacing) + 1;
	const int cx = (int)Math::round(p_local_position.x / spacing);
	const int cz = (int)Math::round(p_local_position.z / spacing);
	const int x0 = CLAMP(cx - rad_idx, 0, resolution - 1);
	const int x1 = CLAMP(cx + rad_idx, 0, resolution - 1);
	const int z0 = CLAMP(cz - rad_idx, 0, resolution - 1);
	const int z1 = CLAMP(cz + rad_idx, 0, resolution - 1);
	if (x1 < x0 || z1 < z0) {
		return;
	}

	const Rect2i region(x0, z0, x1 - x0 + 1, z1 - z0 + 1);
	// Bulk fetch-modify-commit, same reasoning as sculpt() above.
	PackedColorArray control = terrain_data->get_control_region(region);
	const int region_w = region.size.x;

	for (int z = z0; z <= z1; z++) {
		for (int x = x0; x <= x1; x++) {
			const float dx = (x - cx) * spacing;
			const float dz = (z - cz) * spacing;
			if (Math::sqrt(dx * dx + dz * dz) > radius) {
				continue;
			}
			const int local_idx = (z - z0) * region_w + (x - x0);
			Color c = control[local_idx];
			c.a = p_hole ? 1.0f : 0.0f;
			control.set(local_idx, c);
		}
	}

	terrain_data->set_control_region(region, control);
	if (control_texture.is_valid()) {
		control_texture->update(terrain_data->get_control_map_image());
	}
	_rebuild_chunks_in_region(region);
	if (p_update_collision) {
		update_collision();
	}
}

PackedFloat32Array Terrain3D::get_height_region(const Rect2i &p_region) const {
	ERR_FAIL_COND_V(terrain_data.is_null(), PackedFloat32Array());
	return terrain_data->get_height_region(p_region);
}

void Terrain3D::set_height_region(const Rect2i &p_region, const PackedFloat32Array &p_heights, bool p_update_collision) {
	ERR_FAIL_COND(terrain_data.is_null());
	terrain_data->set_height_region(p_region, p_heights);
	_rebuild_chunks_in_region(p_region);
	if (p_update_collision) {
		update_collision();
	}
}

PackedColorArray Terrain3D::get_control_region(const Rect2i &p_region) const {
	ERR_FAIL_COND_V(terrain_data.is_null(), PackedColorArray());
	return terrain_data->get_control_region(p_region);
}

void Terrain3D::set_control_region(const Rect2i &p_region, const PackedColorArray &p_control) {
	ERR_FAIL_COND(terrain_data.is_null());
	terrain_data->set_control_region(p_region, p_control);
	if (control_texture.is_valid()) {
		control_texture->update(terrain_data->get_control_map_image());
	}
	_rebuild_chunks_in_region(p_region);
}

void Terrain3D::update_collision() {
	if (terrain_data.is_null()) {
		return;
	}
	_ensure_collision_nodes();

	const int res = terrain_data->get_resolution();
	collision_shape->set_map_width(res);
	collision_shape->set_map_depth(res);
	collision_shape->set_map_data(terrain_data->get_collision_heights());

	const float spacing = terrain_data->get_vertex_spacing();
	const float half = (float)(res - 1) * 0.5f * spacing;
	collision_shape_node->set_position(Vector3(half, 0, half));
	collision_shape_node->set_scale(Vector3(spacing, 1.0f, spacing));

	collision_body->set_collision_layer(collision_layer);
	collision_body->set_collision_mask(collision_mask);
}

Vector2i Terrain3D::local_position_to_index(const Vector3 &p_local_position) const {
	if (terrain_data.is_null()) {
		return Vector2i();
	}
	const float spacing = terrain_data->get_vertex_spacing();
	return Vector2i((int)Math::round(p_local_position.x / spacing), (int)Math::round(p_local_position.z / spacing));
}

Vector<AABB> Terrain3D::get_chunk_local_aabbs() const {
	Vector<AABB> result;
	for (const KeyValue<Vector2i, Chunk> &kv : chunks) {
		if (kv.value.mesh.is_null()) {
			continue;
		}
		AABB aabb = kv.value.mesh->get_aabb();
		aabb.position += kv.value.local_origin;
		result.push_back(aabb);
	}
	return result;
}

AABB Terrain3D::get_aabb() const {
	if (terrain_data.is_null()) {
		return AABB();
	}
	const float size = terrain_data->get_size();
	// Height bounds aren't tracked separately; pad generously above/below so
	// the bound stays valid after sculpting without needing to be recomputed
	// from the full heightmap on every edit.
	return AABB(Vector3(0, -4096, 0), Vector3(size, 8192, size));
}

PackedStringArray Terrain3D::get_configuration_warnings() const {
	PackedStringArray warnings = Node3D::get_configuration_warnings();

	if (terrain_data.is_null()) {
		warnings.push_back(RTR("No TerrainData resource assigned. Assign one to sculpt and render this terrain."));
	}
	if (layers.is_empty()) {
		warnings.push_back(RTR("No TerrainLayer entries configured. The terrain will render as flat gray until at least one layer is added."));
	}

	return warnings;
}

Terrain3D::Terrain3D() {
	// Chunk instance transforms and the shader's terrain_origin uniform are
	// kept in sync from NOTIFICATION_TRANSFORM_CHANGED, which Node3D only
	// sends to nodes that opt in.
	set_notify_transform(true);
}

Terrain3D::~Terrain3D() {
	_clear_chunks();
	if (terrain_data.is_valid()) {
		terrain_data->disconnect_changed(callable_mp(this, &Terrain3D::_on_terrain_data_changed));
	}
}
