/**************************************************************************/
/*  foliage_spawner_3d.h                                                  */
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
#include "core/templates/local_vector.h"
#include "scene/3d/foliage_gpu_culler.h"
#include "scene/3d/foliage_lod_level.h"
#include "scene/3d/multimesh_instance_3d.h"

class Image;
class RandomPCG;
class Texture2D;

// Scatters instances of a mesh inside a configurable box volume, similar in spirit
// to Unreal Engine's Procedural Foliage Spawner: it fills the volume according to
// density/spacing rules, an optional grayscale distribution mask, and can project
// the instances onto the actual surface geometry of another mesh below them (e.g. terrain).
//
// Generated instances are spatially chunked into a grid of cells on the local XZ
// plane (see cell_size), the same technique FoliagePainter3D uses for painted
// instances: each non-empty cell gets one small MultiMesh and internal
// MultiMeshInstance3D child per FoliageLODLevel of lod_levels, all with a tight
// bounding box, so the renderer's normal per-node AABB frustum culling naturally
// skips whole cells that are off-screen, instead of always submitting every
// instance in one giant draw call.
class FoliageSpawner3D : public MultiMeshInstance3D {
	GDCLASS(FoliageSpawner3D, MultiMeshInstance3D);

	// One MultiMesh + MultiMeshInstance3D per FoliageLODLevel of lod_levels,
	// index-for-index; every multimesh in lod_multimeshes always has the exact
	// same instance transforms as the others (see _sync_cell_lods).
	struct FoliageCell {
		LocalVector<Ref<MultiMesh>> lod_multimeshes;
		LocalVector<MultiMeshInstance3D *> lod_nodes;
	};

	TypedArray<FoliageLODLevel> lod_levels;

	// Volume.
	Vector3 volume_size = Vector3(10, 4, 10);

	// Chunking.
	float cell_size = 16.0f;
	HashMap<Vector2i, FoliageCell> cells;

	// Distribution.
	float density = 1.0;
	int seed = 0;
	int max_instances = 2048;
	int max_attempts_factor = 30;
	float min_distance = 0.5;

	// Mask.
	Ref<Texture2D> distribution_mask;
	bool mask_invert = false;

	// Ground projection.
	bool project_on_mesh = true;
	NodePath ground_mesh_path;
	float max_slope_degrees = 45.0;
	bool align_to_normal = true;
	float align_to_normal_amount = 1.0;

	// Randomization.
	bool random_rotation = true;
	float random_tilt_degrees = 0.0;
	float min_scale = 0.9;
	float max_scale = 1.1;

	// Cell rendering. FoliageSpawner3D itself never has any visible geometry
	// (its inherited MultiMeshInstance3D::multimesh is always cleared; see
	// regenerate()), so its own inherited GeometryInstance3D properties
	// (material_override, cast_shadow, gi_mode, visibility_range_*, etc.) are
	// hidden in _validate_property and have no effect. These cell_* properties
	// are the real, working equivalents, shared across every LOD level (the
	// per-level mesh/material_override/visibility_range_* equivalents live on
	// FoliageLODLevel instead): they're copied onto every cell's
	// MultiMeshInstance3D by _configure_cell_node, and (unlike every other
	// FoliageSpawner3D setting) apply immediately to already-generated cells
	// instead of waiting for the next Regenerate.
	Ref<Material> cell_material_overlay;
	float cell_transparency = 0.0f;
	ShadowCastingSetting cell_cast_shadow = SHADOW_CASTING_SETTING_ON;
	float cell_extra_cull_margin = 0.0f;
	float cell_lod_bias = 1.0f;
	bool cell_ignore_occlusion_culling = false;
	bool cell_ignore_screen_space_shadows = false;
	GIMode cell_gi_mode = GI_MODE_DISABLED;

	// GPU-driven culling. When enabled, cells are not used at all: every
	// generated instance lives in one flat array that is uploaded once, and a
	// compute pass picks the visible ones per frame (see FoliageGPUCuller).
	// Each LOD level then owns a single indirect MultiMesh sized for the worst
	// case, instead of one small MultiMesh per cell.
	bool gpu_culling = false;
	LocalVector<Transform3D> gpu_transforms;
	LocalVector<Ref<MultiMesh>> gpu_multimeshes;
	LocalVector<MultiMeshInstance3D *> gpu_nodes;
	// Which lod_levels entry each of the nodes above was built from. Levels
	// without a mesh are skipped, so the indices are not one to one.
	LocalVector<int> gpu_lod_indices;
	FoliageGPUCuller gpu_culler;
	ObjectID gpu_culling_camera;

	// Debug.
	bool debug_show_cells = false;

	void _on_lod_level_changed();

	bool _is_gpu_culling_active() const;
	void _clear_gpu_instances();
	void _rebuild_gpu_instances();
	void _dispatch_gpu_culling();
	// Both paths store the same instances, just differently: flat for the GPU,
	// chunked into cells for the renderer. These convert between the two so
	// that toggling gpu_culling keeps whatever was generated.
	LocalVector<Transform3D> _gather_cell_transforms() const;
	void _rebuild_cells_from_transforms(const LocalVector<Transform3D> &p_transforms);
	PackedFloat32Array _get_gpu_instance_data() const;
	void _set_gpu_instance_data(const PackedFloat32Array &p_data);

	Ref<Image> _get_mask_image() const;
	bool _sample_mask(const Ref<Image> &p_image, const Vector2 &p_uv, RandomPCG &p_rng) const;
	Callable _get_regenerate_button() const;
	Callable _get_fit_to_ground_mesh_button() const;

	void _clear_cells();
	FoliageCell &_get_or_create_cell(const Vector2i &p_cell);
	// Reconciles a cell's lod_multimeshes/lod_nodes count and configuration
	// (mesh, material, visibility range, shared cell_* settings) to match
	// lod_levels. Safe to call at any time: creates/frees MultiMeshInstance3D
	// children and copies existing transforms into any newly added LOD level.
	void _sync_cell_lods(FoliageCell &p_cell);
	// The GPU path leaves the visibility range off: one node covers the whole
	// volume there, so the distance banding is the compute pass's job instead.
	void _configure_cell_node(MultiMeshInstance3D *p_node, const Ref<FoliageLODLevel> &p_level, bool p_apply_visibility_range = true) const;
	void _sync_all_cells_settings();

	static LocalVector<Transform3D> _read_transforms(const Ref<MultiMesh> &p_multimesh);
	static void _write_transforms(const Ref<MultiMesh> &p_multimesh, const LocalVector<Transform3D> &p_transforms);

	// Internal, storage-only representation of cells' MultiMesh resources, so
	// generated instances are actually saved with the scene (the
	// MultiMeshInstance3D nodes themselves are not; they're recreated from this
	// data instead, the same way FoliagePainter3D persists its own cells).
	Array _get_cell_data() const;
	void _set_cell_data(const Array &p_data);

protected:
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;
	void _notification(int p_what);

public:
	void set_lod_levels(const TypedArray<FoliageLODLevel> &p_levels);
	TypedArray<FoliageLODLevel> get_lod_levels() const;

	// True if at least one LOD level has a Mesh assigned (i.e. this spawner
	// actually renders something).
	bool has_any_mesh() const;

	void set_volume_size(const Vector3 &p_size);
	Vector3 get_volume_size() const;

	void set_cell_size(float p_size);
	float get_cell_size() const;

	void set_density(float p_density);
	float get_density() const;

	void set_seed(int p_seed);
	int get_seed() const;

	void set_max_instances(int p_max_instances);
	int get_max_instances() const;

	void set_max_attempts_factor(int p_factor);
	int get_max_attempts_factor() const;

	void set_min_distance(float p_min_distance);
	float get_min_distance() const;

	void set_distribution_mask(const Ref<Texture2D> &p_mask);
	Ref<Texture2D> get_distribution_mask() const;

	void set_mask_invert(bool p_invert);
	bool is_mask_inverted() const;

	void set_project_on_mesh(bool p_project);
	bool is_projecting_on_mesh() const;

	void set_ground_mesh_path(const NodePath &p_path);
	NodePath get_ground_mesh_path() const;

	void set_max_slope_degrees(float p_degrees);
	float get_max_slope_degrees() const;

	void set_align_to_normal(bool p_align);
	bool is_aligned_to_normal() const;

	void set_align_to_normal_amount(float p_amount);
	float get_align_to_normal_amount() const;

	void set_random_rotation(bool p_random);
	bool is_random_rotation_enabled() const;

	void set_random_tilt_degrees(float p_degrees);
	float get_random_tilt_degrees() const;

	void set_min_scale(float p_scale);
	float get_min_scale() const;

	void set_max_scale(float p_scale);
	float get_max_scale() const;

	void set_cell_material_overlay(const Ref<Material> &p_material);
	Ref<Material> get_cell_material_overlay() const;

	void set_cell_transparency(float p_transparency);
	float get_cell_transparency() const;

	void set_cell_cast_shadow(ShadowCastingSetting p_setting);
	ShadowCastingSetting get_cell_cast_shadow() const;

	void set_cell_extra_cull_margin(float p_margin);
	float get_cell_extra_cull_margin() const;

	void set_cell_lod_bias(float p_bias);
	float get_cell_lod_bias() const;

	void set_cell_ignore_occlusion_culling(bool p_enabled);
	bool is_cell_ignoring_occlusion_culling() const;

	void set_cell_ignore_screen_space_shadows(bool p_enabled);
	bool is_cell_ignoring_screen_space_shadows() const;

	void set_cell_gi_mode(GIMode p_mode);
	GIMode get_cell_gi_mode() const;

	void set_gpu_culling(bool p_enabled);
	bool is_gpu_culling_enabled() const;

	void set_debug_show_cells(bool p_enabled);
	bool is_debug_show_cells_enabled() const;

	int get_cell_count() const;
	int get_instance_count() const;

	// Plain C++ helper for the editor gizmo's debug_show_cells wireframe; not
	// bound to ClassDB, like FoliagePainter3D's own editor-only helpers.
	Vector<AABB> get_cell_local_aabbs() const;

	void regenerate();

	// Moves this node onto the center of the ground (a MeshInstance3D's Mesh or
	// a Landscape3D's height field) and resizes the volume to enclose it, so
	// the volume does not have to be dialed in by hand.
	void fit_to_ground_mesh();

	virtual AABB get_aabb() const override;
	PackedStringArray get_configuration_warnings() const override;

	FoliageSpawner3D();
};
