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

#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/variant/typed_array.h"
#include "scene/3d/landscape_gpu.h"
#include "scene/3d/landscape_quadtree.h"
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

// A large, sculptable, texture-splatted heightfield terrain, rendered as a GPU
// driven quadtree (see LandscapeQuadtree and LandscapeGPUQuadtree).
//
// The heightmap, its slopes and its holes live on the GPU as textures, and the
// whole terrain is drawn from one small grid mesh (a patch of
// LandscapeQuadtree::PATCH_QUADS quads a side) instanced once per quadtree node
// that is drawn: the vertex shader reads each instance's node, places its
// vertices on the heightmap and fits its edges to coarser neighbors. Every
// frame a compute pass walks the quadtree and picks those nodes by
// screen-space error (see lod_pixel_error), culls them against the camera's
// frustum and writes them into two indirect MultiMeshes, one drawn by the
// camera and one only into shadow maps; the number of patches drawn is never
// read back to the CPU. Renderers without compute shaders (Compatibility) make
// the same selection on the CPU.
//
// Close to the camera, micro detail refines the ground past the heightmap's
// own resolution (see micro_detail_levels): the heightmap is interpolated
// smoothly there, and each TerrainLayer with a height_texture and a
// displacement moves the surface along it, so pebbles and cracks stand out of
// the ground in silhouette and in shadow rather than only in shading. Lighting
// uses per-pixel normals taken from the full resolution heightmap, so distant,
// coarsely tessellated ground is shaded exactly like close-up ground.
//
// Texture layers (see TerrainLayer) are combined into shared Texture2DArrays
// and blended per-pixel in a single splatting shader, weighted by TerrainData's
// per-layer weight maps. Optionally (see pom_enabled), the same shader also
// offsets each layer's texture lookups using its TerrainLayer.height_texture
// (Parallax Occlusion Mapping), with an optional self-shadowing pass.
//
// Sculpting, hole cutting and painting edit TerrainData directly and then
// refresh only the affected region: its quadtree nodes and the matching part of
// each GPU texture.
class Landscape3D : public Node3D {
	GDCLASS(Landscape3D, Node3D);

public:
	enum SculptOperation {
		SCULPT_RAISE,
		SCULPT_LOWER,
		SCULPT_SMOOTH,
		SCULPT_FLATTEN,
	};

	enum DebugView {
		DEBUG_VIEW_DISABLED,
		DEBUG_VIEW_LOD_LEVELS,
		DEBUG_VIEW_WIREFRAME,
	};

	// The terrain's occluders are built, and larger edits grouped (the
	// editor's undo snapshots, LandscapeSpline3D's writes), in square blocks
	// of this many quads a side.
	static constexpr int BLOCK_QUADS = 32;

private:
	// Simplified stand-in geometry fed to the renderer's occlusion culling
	// (see _rebuild_occluder_block), not drawn by itself.
	struct OccluderBlock {
		RID occluder;
		RID instance;
		Vector3 local_origin;
	};

	enum DrawList {
		DRAW_CAMERA,
		DRAW_SHADOW,
		DRAW_MAX,
	};

	Ref<TerrainData> terrain_data;
	TypedArray<TerrainLayer> layers;

	static inline Ref<Shader> shader;
	static inline Ref<ArrayMesh> patch_mesh;
	// Nodes can be built on worker threads (threaded scene loading).
	static inline Mutex patch_mesh_mutex;
	Ref<ShaderMaterial> material;

	// One MultiMesh per DrawList, both drawing patch_mesh.
	RID multimeshes[DRAW_MAX];
	RID draw_instances[DRAW_MAX];
	// What the MultiMeshes are currently allocated for: filled by the GPU
	// (indirect, LandscapeGPUQuadtree::CAPACITY instances) or by the CPU
	// (grown to whatever the selection needs).
	bool multimeshes_indirect = false;
	int multimesh_capacity[DRAW_MAX] = {};
	// Set for good once the compute shaders have failed to build: the device
	// they failed on is the one every landscape draws with.
	static inline bool gpu_lod_unavailable = false;

	LandscapeQuadtree quadtree;
	LandscapeGPUQuadtree gpu_quadtree;
	LandscapeQuadtree::Selection cpu_selection;
	Vector<float> cpu_instance_data;
	ObjectID lod_camera;
	LandscapeQuadtree::SelectParams last_lod_params;
	bool lod_dirty = true;
	bool frame_hook_connected = false;

	LandscapeGPUTexture height_texture;
	LandscapeGPUTexture gradient_texture;
	LandscapeGPUTexture hole_texture;
	LandscapeGPUTexture weight_texture;
	// Slope of the heightmap (dh/dx, dh/dz) at every sample, FORMAT_RGH with
	// mipmaps: mipmaps of a slope are the slopes of the averaged heightmap, so
	// unlike normals they filter correctly.
	Ref<Image> gradient_image;

	Ref<Texture2DArray> albedo_array;
	Ref<Texture2DArray> normal_array;
	Ref<Texture2DArray> orm_array;
	Ref<Texture2DArray> height_array;
	// The most any layer's displacement moves the surface, up or down.
	float displacement_bound = 0.0f;

	HashMap<Vector2i, OccluderBlock> occluder_blocks;

	float lod_pixel_error = 2.0;
	float lod_max_quad_pixels = 48.0;
	bool frustum_culling = true;
	bool gpu_lod_enabled = true;
	float shadow_distance = 0.0;

	int micro_detail_levels = 2;
	float micro_detail_distance = 48.0;
	float micro_detail_triangle_size = 6.0;

	DebugView debug_view = DEBUG_VIEW_DISABLED;

	// Occlusion culling. Every block hands the renderer a decimated,
	// deliberately pessimistic copy of its own surface as an occluder, so that
	// hills hide whatever stands behind them (foliage especially) without
	// anyone having to author and bake OccluderInstance3D geometry by hand.
	// Only does anything while occlusion culling is actually on.
	bool occluder_enabled = true;
	// Quads per block edge in that simplified surface, always a power of two
	// no larger than BLOCK_QUADS.
	int occluder_detail = 8;

	// Parallax Occlusion Mapping (see TerrainLayer.height_texture/
	// heightmap_scale for the per-layer half of this).
	bool pom_enabled = false;
	int pom_min_layers = 8;
	int pom_max_layers = 32;
	bool pom_flip_tangent = false;
	bool pom_flip_binormal = false;
	// A fragment shader has no access to the scene's actual lights, so
	// self-shadowing marches towards this fixed direction instead.
	bool pom_self_shadow_enabled = true;
	int pom_shadow_steps = 8;
	float pom_shadow_strength = 1.0;
	Vector3 pom_shadow_light_direction = Vector3(0.5, 0.75, 0.3);
	float pom_fade_start = 20.0;
	float pom_fade_end = 60.0;

	GeometryInstance3D::ShadowCastingSetting cast_shadow = GeometryInstance3D::SHADOW_CASTING_SETTING_ON;
	GeometryInstance3D::GIMode gi_mode = GeometryInstance3D::GI_MODE_STATIC;

	uint32_t collision_layer = 1;
	uint32_t collision_mask = 1;

	// The largest a layer texture is kept at when it has to be resampled into
	// the shared Texture2DArray (see _rebuild_layer_textures).
	int layer_texture_size_limit = 2048;

	StaticBody3D *collision_body = nullptr;
	CollisionShape3D *collision_shape_node = nullptr;
	Ref<HeightMapShape3D> collision_shape;

	// get_global_transform() errors when called outside the tree, but
	// set_terrain_data()/set_layers() commonly run before that (scene
	// deserialization sets every saved property first).
	Transform3D _get_safe_global_transform() const;

	static void _ensure_patch_mesh();
	void _ensure_material();
	void _ensure_draw_instances();
	void _free_draw_instances();
	void _update_draw_instances();
	void _update_draw_aabb();
	bool _is_gpu_lod_active();
	void _allocate_multimeshes(bool p_indirect);

	void _connect_frame_hook();
	void _disconnect_frame_hook();
	void _on_frame_pre_draw();
	bool _make_lod_params(LandscapeQuadtree::SelectParams &r_params);
	void _write_cpu_instances(DrawList p_list, const LocalVector<LandscapeQuadtree::Patch> &p_patches);

	void _rebuild_terrain();
	void _clear_terrain();
	void _refresh_region(const Rect2i &p_samples, bool p_heights_changed, bool p_holes_changed);
	void _refresh_regions(const Vector<Rect2i> &p_regions, bool p_heights_changed, bool p_holes_changed);
	void _compute_gradients(const Rect2i &p_samples);
	void _rebuild_gradients();
	void _rebuild_weight_texture();
	void _upload_weight_region(const Rect2i &p_region, int p_first_layer, int p_layer_count);
	void _rebuild_layer_textures();
	void _update_material_params();
	void _update_micro_detail_params();
	void _update_pom_params();

	Rect2i _get_full_region() const;
	void _emit_terrain_changed(const Rect2i &p_region);

	Vector2i _get_block_grid_size() const;
	void _add_blocks_in_region(const Rect2i &p_samples, HashSet<Vector2i> &r_blocks) const;
	int _get_occluder_stride() const;
	void _rebuild_occluder_block(const Vector2i &p_block);
	void _free_occluder_block(OccluderBlock &p_block);
	void _rebuild_all_occluders();
	void _clear_occluders();
	void _update_occluder_transforms();

	void _ensure_collision_nodes();
	void _on_layers_changed();
	void _on_terrain_data_changed();

	// sculpt()/paint_layer()/set_hole() and the region setters already know
	// exactly which region they touched and refresh precisely that;
	// _on_terrain_data_changed() is a coarse full rebuild meant only for edits
	// made directly to a TerrainData resource. Disconnecting (rather than
	// Object::set_block_signals(), which would silence the signal for every
	// listener) leaves other nodes sharing this TerrainData properly notified.
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

	void set_lod_pixel_error(float p_pixels);
	float get_lod_pixel_error() const;

	void set_lod_max_quad_pixels(float p_pixels);
	float get_lod_max_quad_pixels() const;

	void set_frustum_culling(bool p_enabled);
	bool is_frustum_culling_enabled() const;

	void set_gpu_lod_enabled(bool p_enabled);
	bool is_gpu_lod_enabled() const;

	void set_shadow_distance(float p_distance);
	float get_shadow_distance() const;

	void set_micro_detail_levels(int p_levels);
	int get_micro_detail_levels() const;

	void set_micro_detail_distance(float p_distance);
	float get_micro_detail_distance() const;

	void set_micro_detail_triangle_size(float p_pixels);
	float get_micro_detail_triangle_size() const;

	void set_debug_view(DebugView p_view);
	DebugView get_debug_view() const;

	void set_occluder_enabled(bool p_enabled);
	bool is_occluder_enabled() const;

	void set_occluder_detail(int p_detail);
	int get_occluder_detail() const;

	void set_cast_shadow(GeometryInstance3D::ShadowCastingSetting p_setting);
	GeometryInstance3D::ShadowCastingSetting get_cast_shadow() const;

	void set_gi_mode(GeometryInstance3D::GIMode p_mode);
	GeometryInstance3D::GIMode get_gi_mode() const;

	void set_collision_layer(uint32_t p_layer);
	uint32_t get_collision_layer() const;

	void set_collision_mask(uint32_t p_mask);
	uint32_t get_collision_mask() const;

	void set_layer_texture_size_limit(int p_size);
	int get_layer_texture_size_limit() const;

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

	void set_pom_shadow_strength(float p_strength);
	float get_pom_shadow_strength() const;

	void set_pom_shadow_light_direction(const Vector3 &p_direction);
	Vector3 get_pom_shadow_light_direction() const;

	void set_pom_fade_start(float p_distance);
	float get_pom_fade_start() const;

	void set_pom_fade_end(float p_distance);
	float get_pom_fade_end() const;

	// Sculpting/painting API. Positions are in this node's local space
	// (XZ plane, Y up). Also directly usable at runtime (e.g. for explosion
	// craters), not just from the editor brush.
	// p_falloff shapes the stamp from its center to its rim: 0 is a hard edge,
	// 1 tapers across the whole radius. For SCULPT_SMOOTH, p_strength is how
	// many averaging passes to smooth by rather than a per-stamp amount - see
	// the comment in sculpt() for why that operation cannot use one.
	void sculpt(const Vector3 &p_local_position, float p_radius, float p_strength, SculptOperation p_operation, float p_falloff = 1.0, float p_flatten_height = 0.0, bool p_update_collision = true);
	void paint_layer(const Vector3 &p_local_position, float p_radius, float p_strength, int p_layer_index, float p_falloff = 1.0);
	void set_hole(const Vector3 &p_local_position, float p_radius, bool p_hole, bool p_update_collision = true);

	PackedFloat32Array get_height_region(const Rect2i &p_region) const;
	void set_height_region(const Rect2i &p_region, const PackedFloat32Array &p_heights, bool p_update_collision = true);

	PackedFloat32Array get_layer_weight_region(const Rect2i &p_region, int p_layer_index) const;
	void set_layer_weight_region(const Rect2i &p_region, int p_layer_index, const PackedFloat32Array &p_weights);

	PackedByteArray get_hole_region(const Rect2i &p_region) const;
	void set_hole_region(const Rect2i &p_region, const PackedByteArray &p_holes);

	// Batched forms of the region accessors above, for edits that touch many
	// small, scattered regions at once - LandscapeSpline3D writing a road or
	// river bed along its whole length, and the editor undoing that - where
	// one call per region would refresh the same quadtree nodes, collision
	// and GPU textures once per region.
	TypedArray<PackedFloat32Array> get_height_regions(const TypedArray<Rect2i> &p_regions) const;
	void set_height_regions(const TypedArray<Rect2i> &p_regions, const TypedArray<PackedFloat32Array> &p_heights, bool p_update_collision = true);
	TypedArray<PackedFloat32Array> get_layer_weight_regions(const TypedArray<Rect2i> &p_regions, int p_layer_index) const;
	void set_layer_weight_regions(const TypedArray<Rect2i> &p_regions, int p_layer_index, const TypedArray<PackedFloat32Array> &p_weights);
	// Raises p_layer_index's weight to at least the matching value in
	// p_weights (0-1) at every sample of each region, taking what it gains out
	// of the other layers the same way paint_layer() does.
	void paint_layer_regions(const TypedArray<Rect2i> &p_regions, int p_layer_index, const TypedArray<PackedFloat32Array> &p_weights);

	void update_collision();

	// Plain C++ helpers for the editor plugin/gizmo and tests; not bound to
	// ClassDB.
	Vector2i local_position_to_index(const Vector3 &p_local_position) const;
	StaticBody3D *get_collision_body() const { return collision_body; }
	const LandscapeQuadtree &get_quadtree() const { return quadtree; }
	// The parameters the next frame's patch selection will use, as seen from
	// the camera it currently follows. False if there is nothing to select.
	bool get_lod_params(LandscapeQuadtree::SelectParams &r_params) { return _make_lod_params(r_params); }

	AABB get_aabb() const;
	PackedStringArray get_configuration_warnings() const override;

	Landscape3D();
	~Landscape3D();
};

VARIANT_ENUM_CAST(Landscape3D::SculptOperation)
VARIANT_ENUM_CAST(Landscape3D::DebugView)
