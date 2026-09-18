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
#include "scene/3d/multimesh_instance_3d.h"

class Image;
class Mesh;
class RandomPCG;
class Texture2D;

// Scatters instances of a mesh inside a configurable box volume, similar in spirit
// to Unreal Engine's Procedural Foliage Spawner: it fills the volume according to
// density/spacing rules, an optional grayscale distribution mask, and can project
// the instances onto the actual surface geometry of another mesh below them (e.g. terrain).
//
// Generated instances are spatially chunked into a grid of cells on the local XZ
// plane (see cell_size), the same technique FoliagePainter3D uses for painted
// instances: each non-empty cell gets its own small MultiMesh and internal
// MultiMeshInstance3D child with a tight bounding box, so the renderer's normal
// per-node AABB frustum culling naturally skips whole cells that are off-screen,
// instead of always submitting every instance in one giant draw call.
class FoliageSpawner3D : public MultiMeshInstance3D {
	GDCLASS(FoliageSpawner3D, MultiMeshInstance3D);

	struct FoliageCell {
		Ref<MultiMesh> multimesh;
		MultiMeshInstance3D *node = nullptr;
	};

	Ref<Mesh> mesh;

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
	// are the real, working equivalents: they're copied onto every cell's
	// MultiMeshInstance3D by _configure_cell_node, and (unlike every other
	// FoliageSpawner3D setting) apply immediately to already-generated cells
	// instead of waiting for the next Regenerate.
	Ref<Material> cell_material_override;
	Ref<Material> cell_material_overlay;
	float cell_transparency = 0.0f;
	ShadowCastingSetting cell_cast_shadow = SHADOW_CASTING_SETTING_ON;
	float cell_extra_cull_margin = 0.0f;
	float cell_lod_bias = 1.0f;
	bool cell_ignore_occlusion_culling = false;
	bool cell_ignore_screen_space_shadows = false;
	GIMode cell_gi_mode = GI_MODE_DISABLED;
	float cell_visibility_range_begin = 0.0f;
	float cell_visibility_range_begin_margin = 0.0f;
	float cell_visibility_range_end = 0.0f;
	float cell_visibility_range_end_margin = 0.0f;
	VisibilityRangeFadeMode cell_visibility_range_fade_mode = VISIBILITY_RANGE_FADE_DISABLED;

	// Debug.
	bool debug_show_cells = false;

	Ref<Image> _get_mask_image() const;
	bool _sample_mask(const Ref<Image> &p_image, const Vector2 &p_uv, RandomPCG &p_rng) const;
	Callable _get_regenerate_button() const;

	void _clear_cells();
	FoliageCell &_get_or_create_cell(const Vector2i &p_cell);
	void _sync_cell_node(FoliageCell &p_cell);
	void _configure_cell_node(MultiMeshInstance3D *p_node) const;
	void _sync_all_cells_settings();

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
	void set_mesh(const Ref<Mesh> &p_mesh);
	Ref<Mesh> get_mesh() const;

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

	void set_cell_material_override(const Ref<Material> &p_material);
	Ref<Material> get_cell_material_override() const;

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

	void set_cell_visibility_range_begin(float p_dist);
	float get_cell_visibility_range_begin() const;

	void set_cell_visibility_range_begin_margin(float p_dist);
	float get_cell_visibility_range_begin_margin() const;

	void set_cell_visibility_range_end(float p_dist);
	float get_cell_visibility_range_end() const;

	void set_cell_visibility_range_end_margin(float p_dist);
	float get_cell_visibility_range_end_margin() const;

	void set_cell_visibility_range_fade_mode(VisibilityRangeFadeMode p_mode);
	VisibilityRangeFadeMode get_cell_visibility_range_fade_mode() const;

	void set_debug_show_cells(bool p_enabled);
	bool is_debug_show_cells_enabled() const;

	int get_cell_count() const;
	int get_instance_count() const;

	// Plain C++ helper for the editor gizmo's debug_show_cells wireframe; not
	// bound to ClassDB, like FoliagePainter3D's own editor-only helpers.
	Vector<AABB> get_cell_local_aabbs() const;

	void regenerate();

	virtual AABB get_aabb() const override;
	PackedStringArray get_configuration_warnings() const override;

	FoliageSpawner3D();
};
