/**************************************************************************/
/*  landscape_3d.h                                                        */
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

#pragma once

#include "core/templates/hash_map.h"
#include "scene/3d/terrain_data.h"
#include "scene/3d/terrain_layer.h"
#include "scene/3d/visual_instance_3d.h"

class ArrayMesh;
class CollisionShape3D;
class HeightMapShape3D;
class Shader;
class ShaderMaterial;
class StaticBody3D;
class Texture2DArray;

// A large, sculptable, texture-splatted heightfield terrain, in the spirit of
// CryEngine's terrain system: a single TerrainData heightmap is split into a
// grid of fixed-size chunks, each its own small ArrayMesh so the renderer's
// normal frustum culling skips off-screen chunks, and each chunk mesh carries
// Godot's native distance-based mesh LOD levels (see CHUNK_QUADS) so nearby
// chunks render at full density while distant ones automatically switch to a
// cheaper, decimated index buffer with no per-frame CPU work. Neighboring
// chunks can be at different LOD levels at any time; a "skirt" apron of
// hidden vertical geometry around each chunk hides the resulting seams.
//
// Texture layers (see TerrainLayer) are combined into shared Texture2DArrays
// and blended per-pixel in a single splatting shader, weighted by TerrainData's
// per-layer weight maps, so any number of layers can overlap smoothly and be
// painted without extra draw calls. Sculpting (raise/lower/smooth/flatten) and
// hole cutting edit TerrainData directly and rebuild just the chunks that
// changed; painting texture layers only re-uploads the affected layers'
// weight textures. Optionally (see pom_enabled), the same shader also offsets
// each layer's texture lookups using its TerrainLayer.height_texture
// (Parallax Occlusion Mapping) for extra apparent depth without more
// geometry, with an optional self-shadowing pass that darkens crevices
// facing away from pom_shadow_light_direction.
class Landscape3D : public Node3D {
	GDCLASS(Landscape3D, Node3D);

public:
	enum SculptOperation {
		SCULPT_RAISE,
		SCULPT_LOWER,
		SCULPT_SMOOTH,
		SCULPT_FLATTEN,
	};

	// Number of quads along each edge of a chunk (so CHUNK_QUADS+1 vertices
	// per edge). Fixed so that every LOD stride (1, 2, 4, ... CHUNK_QUADS)
	// divides it evenly, which is what lets every LOD reuse the same,
	// full-resolution vertex buffer with just a different (strided) index
	// buffer. Not user-configurable: it is an implementation detail of the
	// chunking/LOD scheme, not a terrain authoring parameter.
	static constexpr int CHUNK_QUADS = 32;

private:
	struct Chunk {
		Ref<ArrayMesh> mesh;
		RID instance;
		Vector3 local_origin;
	};

	Ref<TerrainData> terrain_data;
	TypedArray<TerrainLayer> layers;

	HashMap<Vector2i, Chunk> chunks;

	static inline Ref<Shader> shader;
	Ref<ShaderMaterial> material;
	Ref<Texture2DArray> weight_array;
	Ref<Texture2DArray> albedo_array;
	Ref<Texture2DArray> normal_array;
	Ref<Texture2DArray> orm_array;
	Ref<Texture2DArray> height_array;

	float skirt_depth = 2.0;
	float lod_bias = 1.0;

	// Parallax Occlusion Mapping (see TerrainLayer.height_texture/
	// heightmap_scale for the per-layer half of this). Off by default, like
	// BaseMaterial3D's own equivalent heightmap_enabled feature this is
	// modeled on: it only does anything once layers have height textures
	// assigned, but the ray-marching it adds to every fragment isn't free
	// even when they don't, so it stays opt-in.
	bool pom_enabled = false;
	int pom_min_layers = 8;
	int pom_max_layers = 32;
	bool pom_flip_tangent = false;
	bool pom_flip_binormal = false;
	// Self-shadowing needs a light direction to march towards, but a
	// fragment shader (unlike a custom light() processor) has no access to
	// the scene's actual lights - this is a fixed approximation the user
	// points at whatever their main light is, not something that tracks a
	// moving DirectionalLight3D automatically.
	bool pom_self_shadow_enabled = true;
	int pom_shadow_steps = 8;
	Vector3 pom_shadow_light_direction = Vector3(0.5, 0.75, 0.3);

	GeometryInstance3D::ShadowCastingSetting cast_shadow = GeometryInstance3D::SHADOW_CASTING_SETTING_ON;
	GeometryInstance3D::GIMode gi_mode = GeometryInstance3D::GI_MODE_STATIC;

	uint32_t collision_layer = 1;
	uint32_t collision_mask = 1;

	bool debug_draw_chunks = false;

	StaticBody3D *collision_body = nullptr;
	CollisionShape3D *collision_shape_node = nullptr;
	Ref<HeightMapShape3D> collision_shape;

	// get_global_transform() errors when called outside the tree, but
	// set_terrain_data()/set_layers() (and, by extension, everything they
	// rebuild) commonly run before that: scene deserialization sets every
	// saved property before adding the node to the tree. This substitutes an
	// identity transform for that case; NOTIFICATION_TRANSFORM_CHANGED
	// corrects it for real once the node actually enters the tree.
	Transform3D _get_safe_global_transform() const;

	void _ensure_material();
	void _rebuild_textures();
	void _rebuild_all_chunks();
	void _rebuild_chunks_in_region(const Rect2i &p_vertex_region);
	void _rebuild_chunk(const Vector2i &p_coord);
	void _clear_chunks();
	void _update_chunk_transform(Chunk &p_chunk);
	void _apply_render_settings_to_chunk(const Chunk &p_chunk);
	Vector2i _get_chunk_grid_size() const;
	Rect2i _get_chunk_range_for_region(const Rect2i &p_vertex_region) const;

	void _ensure_collision_nodes();
	void _on_layers_changed();
	void _on_terrain_data_changed();

	// sculpt()/paint_layer()/set_hole()/set_height_region()/set_layer_weight_region()/
	// set_hole_region() already know exactly which region they touched and refresh precisely
	// that; _on_terrain_data_changed() is a coarse full-terrain rebuild meant
	// only for edits made directly to a TerrainData resource (bypassing this
	// node's own methods, e.g. from a script, or another Landscape3D sharing the
	// same resource). Without suppressing it here, every self-driven edit
	// would trigger both the precise update AND a full rebuild of every
	// chunk/texture/collision sample, which is what made brush strokes slow.
	// Disconnecting (rather than Object::set_block_signals(), which would
	// silence the signal for every listener) leaves other nodes sharing this
	// TerrainData properly notified.
	void _disconnect_terrain_data_changed();
	void _connect_terrain_data_changed();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	static void init_shaders();
	static void finish_shaders();

	void set_terrain_data(const Ref<TerrainData> &p_data);
	Ref<TerrainData> get_terrain_data() const;

	void set_layers(const TypedArray<TerrainLayer> &p_layers);
	TypedArray<TerrainLayer> get_layers() const;

	void set_skirt_depth(float p_depth);
	float get_skirt_depth() const;

	void set_lod_bias(float p_bias);
	float get_lod_bias() const;

	void set_cast_shadow(GeometryInstance3D::ShadowCastingSetting p_setting);
	GeometryInstance3D::ShadowCastingSetting get_cast_shadow() const;

	void set_gi_mode(GeometryInstance3D::GIMode p_mode);
	GeometryInstance3D::GIMode get_gi_mode() const;

	void set_collision_layer(uint32_t p_layer);
	uint32_t get_collision_layer() const;

	void set_collision_mask(uint32_t p_mask);
	uint32_t get_collision_mask() const;

	void set_debug_draw_chunks(bool p_enable);
	bool is_debug_draw_chunks_enabled() const;

	void set_pom_enabled(bool p_enable);
	bool is_pom_enabled() const;

	void set_pom_min_layers(int p_layers);
	int get_pom_min_layers() const;

	void set_pom_max_layers(int p_layers);
	int get_pom_max_layers() const;

	void set_pom_flip_tangent(bool p_flip);
	bool get_pom_flip_tangent() const;

	void set_pom_flip_binormal(bool p_flip);
	bool get_pom_flip_binormal() const;

	void set_pom_self_shadow_enabled(bool p_enable);
	bool is_pom_self_shadow_enabled() const;

	void set_pom_shadow_steps(int p_steps);
	int get_pom_shadow_steps() const;

	void set_pom_shadow_light_direction(const Vector3 &p_direction);
	Vector3 get_pom_shadow_light_direction() const;

	// Sculpting/painting API. Positions are in this node's local space
	// (XZ plane, Y up). Also directly usable at runtime (e.g. for explosion
	// craters), not just from the editor brush.
	void sculpt(const Vector3 &p_local_position, float p_radius, float p_strength, SculptOperation p_operation, float p_flatten_height = 0.0, bool p_update_collision = true);
	void paint_layer(const Vector3 &p_local_position, float p_radius, float p_strength, int p_layer_index);
	void set_hole(const Vector3 &p_local_position, float p_radius, bool p_hole, bool p_update_collision = true);

	PackedFloat32Array get_height_region(const Rect2i &p_region) const;
	void set_height_region(const Rect2i &p_region, const PackedFloat32Array &p_heights, bool p_update_collision = true);

	PackedFloat32Array get_layer_weight_region(const Rect2i &p_region, int p_layer_index) const;
	void set_layer_weight_region(const Rect2i &p_region, int p_layer_index, const PackedFloat32Array &p_weights);

	PackedByteArray get_hole_region(const Rect2i &p_region) const;
	void set_hole_region(const Rect2i &p_region, const PackedByteArray &p_holes);

	void update_collision();

	// Plain C++ helpers for the editor plugin/gizmo; not bound to ClassDB.
	Vector2i local_position_to_index(const Vector3 &p_local_position) const;
	Vector<AABB> get_chunk_local_aabbs() const;
	StaticBody3D *get_collision_body() const { return collision_body; }

	AABB get_aabb() const;
	PackedStringArray get_configuration_warnings() const override;

	Landscape3D();
	~Landscape3D();
};

VARIANT_ENUM_CAST(Landscape3D::SculptOperation)
