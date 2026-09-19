/**************************************************************************/
/*  foliage_spawner_3d.cpp                                                */
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

#include "foliage_spawner_3d.h"

#include "core/core_string_names.h"
#include "core/io/image.h"
#include "core/math/face3.h"
#include "core/math/math_funcs.h"
#include "core/math/random_pcg.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "scene/resources/multimesh.h"
#include "scene/resources/texture.h"

void FoliageSpawner3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_lod_levels", "levels"), &FoliageSpawner3D::set_lod_levels);
	ClassDB::bind_method(D_METHOD("get_lod_levels"), &FoliageSpawner3D::get_lod_levels);
	ClassDB::bind_method(D_METHOD("has_any_mesh"), &FoliageSpawner3D::has_any_mesh);

	ClassDB::bind_method(D_METHOD("set_volume_size", "size"), &FoliageSpawner3D::set_volume_size);
	ClassDB::bind_method(D_METHOD("get_volume_size"), &FoliageSpawner3D::get_volume_size);

	ClassDB::bind_method(D_METHOD("set_cell_size", "size"), &FoliageSpawner3D::set_cell_size);
	ClassDB::bind_method(D_METHOD("get_cell_size"), &FoliageSpawner3D::get_cell_size);

	ClassDB::bind_method(D_METHOD("set_density", "density"), &FoliageSpawner3D::set_density);
	ClassDB::bind_method(D_METHOD("get_density"), &FoliageSpawner3D::get_density);

	ClassDB::bind_method(D_METHOD("set_seed", "seed"), &FoliageSpawner3D::set_seed);
	ClassDB::bind_method(D_METHOD("get_seed"), &FoliageSpawner3D::get_seed);

	ClassDB::bind_method(D_METHOD("set_max_instances", "max_instances"), &FoliageSpawner3D::set_max_instances);
	ClassDB::bind_method(D_METHOD("get_max_instances"), &FoliageSpawner3D::get_max_instances);

	ClassDB::bind_method(D_METHOD("set_max_attempts_factor", "factor"), &FoliageSpawner3D::set_max_attempts_factor);
	ClassDB::bind_method(D_METHOD("get_max_attempts_factor"), &FoliageSpawner3D::get_max_attempts_factor);

	ClassDB::bind_method(D_METHOD("set_min_distance", "min_distance"), &FoliageSpawner3D::set_min_distance);
	ClassDB::bind_method(D_METHOD("get_min_distance"), &FoliageSpawner3D::get_min_distance);

	ClassDB::bind_method(D_METHOD("set_distribution_mask", "mask"), &FoliageSpawner3D::set_distribution_mask);
	ClassDB::bind_method(D_METHOD("get_distribution_mask"), &FoliageSpawner3D::get_distribution_mask);

	ClassDB::bind_method(D_METHOD("set_mask_invert", "invert"), &FoliageSpawner3D::set_mask_invert);
	ClassDB::bind_method(D_METHOD("is_mask_inverted"), &FoliageSpawner3D::is_mask_inverted);

	ClassDB::bind_method(D_METHOD("set_project_on_mesh", "project"), &FoliageSpawner3D::set_project_on_mesh);
	ClassDB::bind_method(D_METHOD("is_projecting_on_mesh"), &FoliageSpawner3D::is_projecting_on_mesh);

	ClassDB::bind_method(D_METHOD("set_ground_mesh_path", "path"), &FoliageSpawner3D::set_ground_mesh_path);
	ClassDB::bind_method(D_METHOD("get_ground_mesh_path"), &FoliageSpawner3D::get_ground_mesh_path);

	ClassDB::bind_method(D_METHOD("set_max_slope_degrees", "degrees"), &FoliageSpawner3D::set_max_slope_degrees);
	ClassDB::bind_method(D_METHOD("get_max_slope_degrees"), &FoliageSpawner3D::get_max_slope_degrees);

	ClassDB::bind_method(D_METHOD("set_align_to_normal", "align"), &FoliageSpawner3D::set_align_to_normal);
	ClassDB::bind_method(D_METHOD("is_aligned_to_normal"), &FoliageSpawner3D::is_aligned_to_normal);

	ClassDB::bind_method(D_METHOD("set_align_to_normal_amount", "amount"), &FoliageSpawner3D::set_align_to_normal_amount);
	ClassDB::bind_method(D_METHOD("get_align_to_normal_amount"), &FoliageSpawner3D::get_align_to_normal_amount);

	ClassDB::bind_method(D_METHOD("set_random_rotation", "random"), &FoliageSpawner3D::set_random_rotation);
	ClassDB::bind_method(D_METHOD("is_random_rotation_enabled"), &FoliageSpawner3D::is_random_rotation_enabled);

	ClassDB::bind_method(D_METHOD("set_random_tilt_degrees", "degrees"), &FoliageSpawner3D::set_random_tilt_degrees);
	ClassDB::bind_method(D_METHOD("get_random_tilt_degrees"), &FoliageSpawner3D::get_random_tilt_degrees);

	ClassDB::bind_method(D_METHOD("set_min_scale", "scale"), &FoliageSpawner3D::set_min_scale);
	ClassDB::bind_method(D_METHOD("get_min_scale"), &FoliageSpawner3D::get_min_scale);

	ClassDB::bind_method(D_METHOD("set_max_scale", "scale"), &FoliageSpawner3D::set_max_scale);
	ClassDB::bind_method(D_METHOD("get_max_scale"), &FoliageSpawner3D::get_max_scale);

	ClassDB::bind_method(D_METHOD("set_cell_material_overlay", "material"), &FoliageSpawner3D::set_cell_material_overlay);
	ClassDB::bind_method(D_METHOD("get_cell_material_overlay"), &FoliageSpawner3D::get_cell_material_overlay);

	ClassDB::bind_method(D_METHOD("set_cell_transparency", "transparency"), &FoliageSpawner3D::set_cell_transparency);
	ClassDB::bind_method(D_METHOD("get_cell_transparency"), &FoliageSpawner3D::get_cell_transparency);

	ClassDB::bind_method(D_METHOD("set_cell_cast_shadow", "setting"), &FoliageSpawner3D::set_cell_cast_shadow);
	ClassDB::bind_method(D_METHOD("get_cell_cast_shadow"), &FoliageSpawner3D::get_cell_cast_shadow);

	ClassDB::bind_method(D_METHOD("set_cell_extra_cull_margin", "margin"), &FoliageSpawner3D::set_cell_extra_cull_margin);
	ClassDB::bind_method(D_METHOD("get_cell_extra_cull_margin"), &FoliageSpawner3D::get_cell_extra_cull_margin);

	ClassDB::bind_method(D_METHOD("set_cell_lod_bias", "bias"), &FoliageSpawner3D::set_cell_lod_bias);
	ClassDB::bind_method(D_METHOD("get_cell_lod_bias"), &FoliageSpawner3D::get_cell_lod_bias);

	ClassDB::bind_method(D_METHOD("set_cell_ignore_occlusion_culling", "enabled"), &FoliageSpawner3D::set_cell_ignore_occlusion_culling);
	ClassDB::bind_method(D_METHOD("is_cell_ignoring_occlusion_culling"), &FoliageSpawner3D::is_cell_ignoring_occlusion_culling);

	ClassDB::bind_method(D_METHOD("set_cell_ignore_screen_space_shadows", "enabled"), &FoliageSpawner3D::set_cell_ignore_screen_space_shadows);
	ClassDB::bind_method(D_METHOD("is_cell_ignoring_screen_space_shadows"), &FoliageSpawner3D::is_cell_ignoring_screen_space_shadows);

	ClassDB::bind_method(D_METHOD("set_cell_gi_mode", "mode"), &FoliageSpawner3D::set_cell_gi_mode);
	ClassDB::bind_method(D_METHOD("get_cell_gi_mode"), &FoliageSpawner3D::get_cell_gi_mode);

	ClassDB::bind_method(D_METHOD("set_debug_show_cells", "enabled"), &FoliageSpawner3D::set_debug_show_cells);
	ClassDB::bind_method(D_METHOD("is_debug_show_cells_enabled"), &FoliageSpawner3D::is_debug_show_cells_enabled);

	ClassDB::bind_method(D_METHOD("get_cell_count"), &FoliageSpawner3D::get_cell_count);
	ClassDB::bind_method(D_METHOD("get_instance_count"), &FoliageSpawner3D::get_instance_count);

	ClassDB::bind_method(D_METHOD("regenerate"), &FoliageSpawner3D::regenerate);
	ClassDB::bind_method(D_METHOD("get_regenerate_button"), &FoliageSpawner3D::_get_regenerate_button);

	ClassDB::bind_method(D_METHOD("_get_cell_data"), &FoliageSpawner3D::_get_cell_data);
	ClassDB::bind_method(D_METHOD("_set_cell_data", "data"), &FoliageSpawner3D::_set_cell_data);

	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "lod_levels", PROPERTY_HINT_ARRAY_TYPE, MAKE_RESOURCE_TYPE_HINT("FoliageLODLevel")), "set_lod_levels", "get_lod_levels");

	ADD_GROUP("Volume", "");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "volume_size", PROPERTY_HINT_RANGE, "0.01,4096,0.01,or_greater,suffix:m"), "set_volume_size", "get_volume_size");

	ADD_GROUP("Chunking", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cell_size", PROPERTY_HINT_RANGE, "1,256,0.5,or_greater,suffix:m"), "set_cell_size", "get_cell_size");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "_cell_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_INTERNAL | PROPERTY_USAGE_STORAGE), "_set_cell_data", "_get_cell_data");

	ADD_GROUP("Distribution", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "density", PROPERTY_HINT_RANGE, "0.0,50.0,0.001,or_greater"), "set_density", "get_density");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed"), "set_seed", "get_seed");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_instances", PROPERTY_HINT_RANGE, "0,65536,1,or_greater"), "set_max_instances", "get_max_instances");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_attempts_factor", PROPERTY_HINT_RANGE, "1,200,1,or_greater"), "set_max_attempts_factor", "get_max_attempts_factor");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "min_distance", PROPERTY_HINT_RANGE, "0.0,100.0,0.001,or_greater,suffix:m"), "set_min_distance", "get_min_distance");

	ADD_GROUP("Mask", "");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "distribution_mask", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_distribution_mask", "get_distribution_mask");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "mask_invert"), "set_mask_invert", "is_mask_inverted");

	ADD_GROUP("Ground Projection", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "project_on_mesh"), "set_project_on_mesh", "is_projecting_on_mesh");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "ground_mesh_path", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "MeshInstance3D"), "set_ground_mesh_path", "get_ground_mesh_path");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_slope_degrees", PROPERTY_HINT_RANGE, "0,90,0.1,suffix:°"), "set_max_slope_degrees", "get_max_slope_degrees");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "align_to_normal"), "set_align_to_normal", "is_aligned_to_normal");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "align_to_normal_amount", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_align_to_normal_amount", "get_align_to_normal_amount");

	ADD_GROUP("Randomization", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "random_rotation"), "set_random_rotation", "is_random_rotation_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "random_tilt_degrees", PROPERTY_HINT_RANGE, "0,90,0.1,suffix:°"), "set_random_tilt_degrees", "get_random_tilt_degrees");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "min_scale", PROPERTY_HINT_RANGE, "0.01,10.0,0.001,or_greater"), "set_min_scale", "get_min_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_scale", PROPERTY_HINT_RANGE, "0.01,10.0,0.001,or_greater"), "set_max_scale", "get_max_scale");

	// This node itself never has any visible geometry (see regenerate()), so
	// its inherited GeometryInstance3D properties are hidden in
	// _validate_property; these cell_* properties are the working equivalents
	// (shared across every LOD level; see FoliageLODLevel for the per-level
	// mesh/material_override/visibility_range_* equivalents), applied to every
	// cell's MultiMeshInstance3D and kept in sync live.
	ADD_GROUP("Cell Rendering", "cell_");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "cell_material_overlay", PROPERTY_HINT_RESOURCE_TYPE, "BaseMaterial3D,ShaderMaterial"), "set_cell_material_overlay", "get_cell_material_overlay");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cell_transparency", PROPERTY_HINT_RANGE, "0.0,1.0,0.01"), "set_cell_transparency", "get_cell_transparency");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cell_cast_shadow", PROPERTY_HINT_ENUM, "Off,On,Double-Sided,Shadows Only"), "set_cell_cast_shadow", "get_cell_cast_shadow");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cell_extra_cull_margin", PROPERTY_HINT_RANGE, "0,16384,0.01,suffix:m"), "set_cell_extra_cull_margin", "get_cell_extra_cull_margin");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cell_lod_bias", PROPERTY_HINT_RANGE, "0.001,128,0.001"), "set_cell_lod_bias", "get_cell_lod_bias");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cell_ignore_occlusion_culling"), "set_cell_ignore_occlusion_culling", "is_cell_ignoring_occlusion_culling");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cell_ignore_screen_space_shadows"), "set_cell_ignore_screen_space_shadows", "is_cell_ignoring_screen_space_shadows");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cell_gi_mode", PROPERTY_HINT_ENUM, "Disabled,Static,Dynamic"), "set_cell_gi_mode", "get_cell_gi_mode");

	ADD_GROUP("Debug", "debug_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug_show_cells"), "set_debug_show_cells", "is_debug_show_cells_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cell_count", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "get_cell_count");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "instance_count", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "get_instance_count");

	ADD_GROUP("", "");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "regenerate_button", PROPERTY_HINT_TOOL_BUTTON, "Regenerate", PROPERTY_USAGE_EDITOR), "", "get_regenerate_button");
}

void FoliageSpawner3D::_validate_property(PropertyInfo &p_property) const {
	if (p_property.name == "multimesh") {
		// Superseded by chunking (see _cell_data): this base-class property is
		// only still written to for scenes saved before chunking existed, so
		// their bake keeps rendering until the next Regenerate. Keep it out of
		// the inspector either way.
		p_property.usage = PROPERTY_USAGE_STORAGE;
		return;
	}

	// This node itself never has any visible geometry (see regenerate()), so
	// its inherited GeometryInstance3D properties would have no effect if set
	// here; hide them in favor of the working cell_* equivalents (see the
	// "Cell Rendering" group in _bind_methods).
	static const char *hidden_geometry_instance_properties[] = {
		"material_override",
		"material_overlay",
		"transparency",
		"cast_shadow",
		"extra_cull_margin",
		"lod_bias",
		"ignore_occlusion_culling",
		"ignore_screen_space_shadows",
		"gi_mode",
		"gi_lightmap_texel_scale",
		"gi_lightmap_scale",
		"visibility_range_begin",
		"visibility_range_begin_margin",
		"visibility_range_end",
		"visibility_range_end_margin",
		"visibility_range_fade_mode",
	};
	for (const char *hidden_name : hidden_geometry_instance_properties) {
		if (p_property.name == hidden_name) {
			p_property.usage = PROPERTY_USAGE_NONE;
			return;
		}
	}
}

void FoliageSpawner3D::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE) {
		// Cell MultiMeshInstance3D nodes are runtime-only (see _get_cell_data);
		// (re-)create/sync every cell so their settings reflect this node's
		// current cell_* properties, since those may have been deserialized
		// after _cell_data during scene loading (GeometryInstance3D-derived
		// FoliageSpawner3D properties are declared after this node's own).
		for (KeyValue<Vector2i, FoliageCell> &kv : cells) {
			_sync_cell_lods(kv.value);
		}
	}
}

Callable FoliageSpawner3D::_get_regenerate_button() const {
	return Callable(const_cast<FoliageSpawner3D *>(this), "regenerate");
}

void FoliageSpawner3D::_clear_cells() {
	for (KeyValue<Vector2i, FoliageCell> &kv : cells) {
		for (MultiMeshInstance3D *node : kv.value.lod_nodes) {
			if (node != nullptr) {
				remove_child(node);
				node->queue_free();
			}
		}
	}
	cells.clear();
}

FoliageSpawner3D::FoliageCell &FoliageSpawner3D::_get_or_create_cell(const Vector2i &p_cell) {
	const bool is_new = !cells.has(p_cell);
	FoliageCell &cell = cells[p_cell];
	if (is_new) {
		_sync_cell_lods(cell);
	}
	return cell;
}

LocalVector<Transform3D> FoliageSpawner3D::_read_transforms(const Ref<MultiMesh> &p_multimesh) {
	LocalVector<Transform3D> result;
	if (p_multimesh.is_null()) {
		return result;
	}
	const int count = p_multimesh->get_instance_count();
	result.resize(count);
	for (int i = 0; i < count; i++) {
		result[i] = p_multimesh->get_instance_transform(i);
	}
	return result;
}

void FoliageSpawner3D::_write_transforms(const Ref<MultiMesh> &p_multimesh, const LocalVector<Transform3D> &p_transforms) {
	if (p_multimesh.is_null()) {
		return;
	}
	// MultiMesh.instance_count "clears and (re)sizes the buffers" on every
	// set, so this always writes the *entire* array back, never just a diff.
	p_multimesh->set_instance_count((int)p_transforms.size());
	for (uint32_t i = 0; i < p_transforms.size(); i++) {
		p_multimesh->set_instance_transform(i, p_transforms[i]);
	}
}

void FoliageSpawner3D::_sync_cell_lods(FoliageCell &p_cell) {
	const int lod_count = lod_levels.size();

	// Shrink extra LOD multimeshes/nodes if lod_levels now has fewer levels
	// than this cell was last synced with.
	while ((int)p_cell.lod_multimeshes.size() > lod_count) {
		const int last = p_cell.lod_multimeshes.size() - 1;
		if (last < (int)p_cell.lod_nodes.size()) {
			if (p_cell.lod_nodes[last] != nullptr) {
				remove_child(p_cell.lod_nodes[last]);
				p_cell.lod_nodes[last]->queue_free();
			}
			p_cell.lod_nodes.remove_at(last);
		}
		p_cell.lod_multimeshes.remove_at(last);
	}

	// Grow: new LOD levels start out with the same instance transforms as
	// the rest of the cell (read from any existing multimesh) instead of
	// being empty, so a newly added LOD level doesn't need a Regenerate.
	const LocalVector<Transform3D> existing_transforms = p_cell.lod_multimeshes.is_empty() ? LocalVector<Transform3D>() : _read_transforms(p_cell.lod_multimeshes[0]);
	while ((int)p_cell.lod_multimeshes.size() < lod_count) {
		Ref<MultiMesh> mm;
		mm.instantiate();
		mm->set_transform_format(MultiMesh::TRANSFORM_3D);
		_write_transforms(mm, existing_transforms);
		p_cell.lod_multimeshes.push_back(mm);
	}

	// Ensure every multimesh has a MultiMeshInstance3D to be rendered through.
	while ((int)p_cell.lod_nodes.size() < (int)p_cell.lod_multimeshes.size()) {
		MultiMeshInstance3D *node = memnew(MultiMeshInstance3D);
		add_child(node, false, INTERNAL_MODE_FRONT);
		p_cell.lod_nodes.push_back(node);
	}

	// Apply each LOD level's own mesh/material/visibility range, plus the
	// cell_* properties shared by the whole spawner, to every node.
	for (int i = 0; i < lod_count; i++) {
		Ref<FoliageLODLevel> level = lod_levels[i];
		MultiMeshInstance3D *node = p_cell.lod_nodes[i];
		Ref<MultiMesh> mm = p_cell.lod_multimeshes[i];
		if (level.is_null() || node == nullptr || mm.is_null()) {
			continue;
		}

		mm->set_mesh(level->get_mesh());

		node->set_multimesh(mm);
		_configure_cell_node(node, level);
	}
}

void FoliageSpawner3D::_configure_cell_node(MultiMeshInstance3D *p_node, const Ref<FoliageLODLevel> &p_level) const {
	// Cells mirror this node's cell_* rendering settings, the same way
	// FoliagePainter3D's FoliageLayer settings are copied to its cell nodes.
	// See the "Cell Rendering" comment above the cell_* fields for why these
	// aren't just the plain inherited GeometryInstance3D properties. The
	// per-level mesh/material_override/visibility_range_* come from p_level
	// instead (see FoliageLODLevel); it is only ever null defensively (a
	// mismatch between lod_nodes and lod_levels), in which case those are
	// simply left unchanged.
	p_node->set_material_overlay(cell_material_overlay);
	p_node->set_transparency(cell_transparency);
	p_node->set_cast_shadows_setting(cell_cast_shadow);
	p_node->set_extra_cull_margin(cell_extra_cull_margin);
	p_node->set_lod_bias(cell_lod_bias);
	p_node->set_ignore_occlusion_culling(cell_ignore_occlusion_culling);
	p_node->set_ignore_screen_space_shadows(cell_ignore_screen_space_shadows);
	p_node->set_gi_mode(cell_gi_mode);
	if (p_level.is_valid()) {
		p_node->set_material_override(p_level->get_material_override());
		p_node->set_visibility_range_begin(p_level->get_visibility_range_begin());
		p_node->set_visibility_range_begin_margin(p_level->get_visibility_range_begin_margin());
		p_node->set_visibility_range_end(p_level->get_visibility_range_end());
		p_node->set_visibility_range_end_margin(p_level->get_visibility_range_end_margin());
		p_node->set_visibility_range_fade_mode(p_level->get_visibility_range_fade_mode());
	}
}

void FoliageSpawner3D::_sync_all_cells_settings() {
	for (KeyValue<Vector2i, FoliageCell> &kv : cells) {
		for (int i = 0; i < (int)kv.value.lod_nodes.size(); i++) {
			if (kv.value.lod_nodes[i] == nullptr) {
				continue;
			}
			Ref<FoliageLODLevel> level = i < lod_levels.size() ? Ref<FoliageLODLevel>(lod_levels[i]) : Ref<FoliageLODLevel>();
			_configure_cell_node(kv.value.lod_nodes[i], level);
		}
	}
}

Array FoliageSpawner3D::_get_cell_data() const {
	Array result;
	for (const KeyValue<Vector2i, FoliageCell> &kv : cells) {
		if (kv.value.lod_multimeshes.is_empty() || kv.value.lod_multimeshes[0].is_null() || kv.value.lod_multimeshes[0]->get_instance_count() == 0) {
			continue;
		}
		Array lod_multimeshes;
		for (const Ref<MultiMesh> &mm : kv.value.lod_multimeshes) {
			lod_multimeshes.push_back(mm);
		}
		Array entry;
		entry.push_back(kv.key);
		entry.push_back(lod_multimeshes);
		result.push_back(entry);
	}
	return result;
}

void FoliageSpawner3D::_set_cell_data(const Array &p_data) {
	_clear_cells();

	for (int i = 0; i < p_data.size(); i++) {
		Array entry = p_data[i];
		if (entry.size() != 2) {
			continue;
		}
		const Vector2i cell_coord = entry[0];
		Array lod_multimeshes = entry[1];
		if (lod_multimeshes.is_empty()) {
			continue;
		}

		FoliageCell cell;
		for (int j = 0; j < lod_multimeshes.size(); j++) {
			Ref<MultiMesh> mm = lod_multimeshes[j];
			if (mm.is_valid()) {
				cell.lod_multimeshes.push_back(mm);
			}
		}
		if (cell.lod_multimeshes.is_empty()) {
			continue;
		}
		cells[cell_coord] = cell;
		_sync_cell_lods(cells[cell_coord]);
	}
}

void FoliageSpawner3D::set_lod_levels(const TypedArray<FoliageLODLevel> &p_levels) {
	for (int i = 0; i < lod_levels.size(); i++) {
		Ref<FoliageLODLevel> old_level = lod_levels[i];
		if (old_level.is_valid()) {
			old_level->disconnect(CoreStringName(changed), callable_mp(this, &FoliageSpawner3D::_on_lod_level_changed));
		}
	}

	lod_levels = p_levels;

	for (int i = 0; i < lod_levels.size(); i++) {
		Ref<FoliageLODLevel> level = lod_levels[i];
		if (level.is_valid()) {
			level->connect(CoreStringName(changed), callable_mp(this, &FoliageSpawner3D::_on_lod_level_changed));
		}
	}

	for (KeyValue<Vector2i, FoliageCell> &kv : cells) {
		_sync_cell_lods(kv.value);
	}
	update_configuration_warnings();
}

TypedArray<FoliageLODLevel> FoliageSpawner3D::get_lod_levels() const {
	return lod_levels;
}

void FoliageSpawner3D::_on_lod_level_changed() {
	for (KeyValue<Vector2i, FoliageCell> &kv : cells) {
		_sync_cell_lods(kv.value);
	}
	update_configuration_warnings();
}

bool FoliageSpawner3D::has_any_mesh() const {
	for (int i = 0; i < lod_levels.size(); i++) {
		Ref<FoliageLODLevel> level = lod_levels[i];
		if (level.is_valid() && level->get_mesh().is_valid()) {
			return true;
		}
	}
	return false;
}

void FoliageSpawner3D::set_volume_size(const Vector3 &p_size) {
	volume_size = p_size.maxf(0);
	update_gizmos();
	update_configuration_warnings();
}

Vector3 FoliageSpawner3D::get_volume_size() const {
	return volume_size;
}

void FoliageSpawner3D::set_cell_size(float p_size) {
	cell_size = MAX(p_size, 0.01f);
}

float FoliageSpawner3D::get_cell_size() const {
	return cell_size;
}

void FoliageSpawner3D::set_density(float p_density) {
	density = MAX(p_density, 0.0f);
}

float FoliageSpawner3D::get_density() const {
	return density;
}

void FoliageSpawner3D::set_seed(int p_seed) {
	seed = p_seed;
}

int FoliageSpawner3D::get_seed() const {
	return seed;
}

void FoliageSpawner3D::set_max_instances(int p_max_instances) {
	max_instances = MAX(p_max_instances, 0);
}

int FoliageSpawner3D::get_max_instances() const {
	return max_instances;
}

void FoliageSpawner3D::set_max_attempts_factor(int p_factor) {
	max_attempts_factor = MAX(p_factor, 1);
}

int FoliageSpawner3D::get_max_attempts_factor() const {
	return max_attempts_factor;
}

void FoliageSpawner3D::set_min_distance(float p_min_distance) {
	min_distance = MAX(p_min_distance, 0.0f);
}

float FoliageSpawner3D::get_min_distance() const {
	return min_distance;
}

void FoliageSpawner3D::set_distribution_mask(const Ref<Texture2D> &p_mask) {
	distribution_mask = p_mask;
}

Ref<Texture2D> FoliageSpawner3D::get_distribution_mask() const {
	return distribution_mask;
}

void FoliageSpawner3D::set_mask_invert(bool p_invert) {
	mask_invert = p_invert;
}

bool FoliageSpawner3D::is_mask_inverted() const {
	return mask_invert;
}

void FoliageSpawner3D::set_project_on_mesh(bool p_project) {
	project_on_mesh = p_project;
	update_configuration_warnings();
}

bool FoliageSpawner3D::is_projecting_on_mesh() const {
	return project_on_mesh;
}

void FoliageSpawner3D::set_ground_mesh_path(const NodePath &p_path) {
	ground_mesh_path = p_path;
	update_configuration_warnings();
}

NodePath FoliageSpawner3D::get_ground_mesh_path() const {
	return ground_mesh_path;
}

void FoliageSpawner3D::set_max_slope_degrees(float p_degrees) {
	max_slope_degrees = CLAMP(p_degrees, 0.0f, 90.0f);
}

float FoliageSpawner3D::get_max_slope_degrees() const {
	return max_slope_degrees;
}

void FoliageSpawner3D::set_align_to_normal(bool p_align) {
	align_to_normal = p_align;
	notify_property_list_changed();
}

bool FoliageSpawner3D::is_aligned_to_normal() const {
	return align_to_normal;
}

void FoliageSpawner3D::set_align_to_normal_amount(float p_amount) {
	align_to_normal_amount = CLAMP(p_amount, 0.0f, 1.0f);
}

float FoliageSpawner3D::get_align_to_normal_amount() const {
	return align_to_normal_amount;
}

void FoliageSpawner3D::set_random_rotation(bool p_random) {
	random_rotation = p_random;
}

bool FoliageSpawner3D::is_random_rotation_enabled() const {
	return random_rotation;
}

void FoliageSpawner3D::set_random_tilt_degrees(float p_degrees) {
	random_tilt_degrees = CLAMP(p_degrees, 0.0f, 90.0f);
}

float FoliageSpawner3D::get_random_tilt_degrees() const {
	return random_tilt_degrees;
}

void FoliageSpawner3D::set_min_scale(float p_scale) {
	min_scale = MAX(p_scale, 0.001f);
}

float FoliageSpawner3D::get_min_scale() const {
	return min_scale;
}

void FoliageSpawner3D::set_max_scale(float p_scale) {
	max_scale = MAX(p_scale, 0.001f);
}

float FoliageSpawner3D::get_max_scale() const {
	return max_scale;
}

void FoliageSpawner3D::set_cell_material_overlay(const Ref<Material> &p_material) {
	cell_material_overlay = p_material;
	_sync_all_cells_settings();
}

Ref<Material> FoliageSpawner3D::get_cell_material_overlay() const {
	return cell_material_overlay;
}

void FoliageSpawner3D::set_cell_transparency(float p_transparency) {
	cell_transparency = CLAMP(p_transparency, 0.0f, 1.0f);
	_sync_all_cells_settings();
}

float FoliageSpawner3D::get_cell_transparency() const {
	return cell_transparency;
}

void FoliageSpawner3D::set_cell_cast_shadow(ShadowCastingSetting p_setting) {
	cell_cast_shadow = p_setting;
	_sync_all_cells_settings();
}

FoliageSpawner3D::ShadowCastingSetting FoliageSpawner3D::get_cell_cast_shadow() const {
	return cell_cast_shadow;
}

void FoliageSpawner3D::set_cell_extra_cull_margin(float p_margin) {
	cell_extra_cull_margin = MAX(p_margin, 0.0f);
	_sync_all_cells_settings();
}

float FoliageSpawner3D::get_cell_extra_cull_margin() const {
	return cell_extra_cull_margin;
}

void FoliageSpawner3D::set_cell_lod_bias(float p_bias) {
	cell_lod_bias = MAX(p_bias, 0.001f);
	_sync_all_cells_settings();
}

float FoliageSpawner3D::get_cell_lod_bias() const {
	return cell_lod_bias;
}

void FoliageSpawner3D::set_cell_ignore_occlusion_culling(bool p_enabled) {
	cell_ignore_occlusion_culling = p_enabled;
	_sync_all_cells_settings();
}

bool FoliageSpawner3D::is_cell_ignoring_occlusion_culling() const {
	return cell_ignore_occlusion_culling;
}

void FoliageSpawner3D::set_cell_ignore_screen_space_shadows(bool p_enabled) {
	cell_ignore_screen_space_shadows = p_enabled;
	_sync_all_cells_settings();
}

bool FoliageSpawner3D::is_cell_ignoring_screen_space_shadows() const {
	return cell_ignore_screen_space_shadows;
}

void FoliageSpawner3D::set_cell_gi_mode(GIMode p_mode) {
	cell_gi_mode = p_mode;
	_sync_all_cells_settings();
}

FoliageSpawner3D::GIMode FoliageSpawner3D::get_cell_gi_mode() const {
	return cell_gi_mode;
}

void FoliageSpawner3D::set_debug_show_cells(bool p_enabled) {
	debug_show_cells = p_enabled;
	update_gizmos();
}

bool FoliageSpawner3D::is_debug_show_cells_enabled() const {
	return debug_show_cells;
}

int FoliageSpawner3D::get_cell_count() const {
	return cells.size();
}

int FoliageSpawner3D::get_instance_count() const {
	int total = 0;
	for (const KeyValue<Vector2i, FoliageCell> &kv : cells) {
		if (!kv.value.lod_multimeshes.is_empty() && kv.value.lod_multimeshes[0].is_valid()) {
			total += kv.value.lod_multimeshes[0]->get_instance_count();
		}
	}
	return total;
}

Ref<Image> FoliageSpawner3D::_get_mask_image() const {
	if (distribution_mask.is_null()) {
		return Ref<Image>();
	}
	Ref<Image> img = distribution_mask->get_image();
	if (img.is_null()) {
		return Ref<Image>();
	}
	if (img->is_compressed()) {
		img = img->duplicate();
		img->decompress();
	}
	if (img->get_width() <= 0 || img->get_height() <= 0) {
		return Ref<Image>();
	}
	return img;
}

bool FoliageSpawner3D::_sample_mask(const Ref<Image> &p_image, const Vector2 &p_uv, RandomPCG &p_rng) const {
	const int w = p_image->get_width();
	const int h = p_image->get_height();
	const int px = CLAMP(int(p_uv.x * w), 0, w - 1);
	const int py = CLAMP(int((1.0f - p_uv.y) * h), 0, h - 1);

	float value = p_image->get_pixel(px, py).get_luminance();
	if (mask_invert) {
		value = 1.0f - value;
	}
	return p_rng.randf() <= value;
}

void FoliageSpawner3D::regenerate() {
	// Instances now live in per-cell MultiMeshes (see _cell_data); clear any
	// pre-chunking bake still sitting in the inherited (and now unused)
	// MultiMeshInstance3D::multimesh, and start every regeneration from a
	// clean slate of cells.
	set_multimesh(Ref<MultiMesh>());
	_clear_cells();

	if (!has_any_mesh()) {
		update_configuration_warnings();
		return;
	}

	const Vector3 half = volume_size * 0.5f;
	if (half.x <= 0.0f || half.z <= 0.0f) {
		update_configuration_warnings();
		return;
	}

	const double area = double(volume_size.x) * double(volume_size.z);
	int target_count = int(Math::round(area * double(density)));
	target_count = CLAMP(target_count, 0, max_instances);

	RandomPCG rng;
	rng.seed(uint64_t(uint32_t(seed)));

	Ref<Image> mask_image = _get_mask_image();

	const Transform3D gt = get_global_transform();
	const Transform3D gt_inv = gt.affine_inverse();

	LocalVector<Face3> local_faces;
	HashMap<Vector2i, LocalVector<uint32_t>> face_grid;
	float face_cell_size = 1.0f;

	if (project_on_mesh) {
		MeshInstance3D *ground = Object::cast_to<MeshInstance3D>(is_inside_tree() ? get_node_or_null(ground_mesh_path) : nullptr);
		Ref<Mesh> ground_mesh = ground != nullptr ? ground->get_mesh() : Ref<Mesh>();
		Vector<Face3> faces = ground_mesh.is_valid() ? ground_mesh->get_faces() : Vector<Face3>();

		if (faces.is_empty()) {
			update_configuration_warnings();
			return;
		}

		const Transform3D ground_to_local = gt_inv * ground->get_global_transform();
		local_faces.resize(faces.size());
		for (int i = 0; i < faces.size(); i++) {
			local_faces[i] = Face3(
					ground_to_local.xform(faces[i].vertex[0]),
					ground_to_local.xform(faces[i].vertex[1]),
					ground_to_local.xform(faces[i].vertex[2]));
		}

		face_cell_size = MAX((volume_size.x + volume_size.z) * 0.03125f, 0.25f);
		const int cell_min_x = int(Math::floor(-half.x / face_cell_size));
		const int cell_max_x = int(Math::floor(half.x / face_cell_size));
		const int cell_min_z = int(Math::floor(-half.z / face_cell_size));
		const int cell_max_z = int(Math::floor(half.z / face_cell_size));

		for (uint32_t i = 0; i < local_faces.size(); i++) {
			const Face3 &face = local_faces[i];
			const float min_x = MIN(face.vertex[0].x, MIN(face.vertex[1].x, face.vertex[2].x));
			const float max_x = MAX(face.vertex[0].x, MAX(face.vertex[1].x, face.vertex[2].x));
			const float min_z = MIN(face.vertex[0].z, MIN(face.vertex[1].z, face.vertex[2].z));
			const float max_z = MAX(face.vertex[0].z, MAX(face.vertex[1].z, face.vertex[2].z));

			if (max_x < -half.x || min_x > half.x || max_z < -half.z || min_z > half.z) {
				continue; // Outside the volume's footprint; cannot be sampled.
			}

			const int cx0 = MAX(cell_min_x, int(Math::floor(min_x / face_cell_size)));
			const int cx1 = MIN(cell_max_x, int(Math::floor(max_x / face_cell_size)));
			const int cz0 = MAX(cell_min_z, int(Math::floor(min_z / face_cell_size)));
			const int cz1 = MIN(cell_max_z, int(Math::floor(max_z / face_cell_size)));

			for (int cx = cx0; cx <= cx1; cx++) {
				for (int cz = cz0; cz <= cz1; cz++) {
					face_grid[Vector2i(cx, cz)].push_back(i);
				}
			}
		}
	}

	const float spacing_cell_size = MAX(min_distance, 0.001f);
	const float min_distance_sq = min_distance * min_distance;
	HashMap<Vector2i, LocalVector<Vector2>> grid;

	LocalVector<Transform3D> transforms;

	const int max_attempts = target_count > 0 ? target_count * MAX(max_attempts_factor, 1) : 0;
	int attempts = 0;

	while ((int)transforms.size() < target_count && attempts < max_attempts) {
		attempts++;

		const float lx = rng.random(-half.x, half.x);
		const float lz = rng.random(-half.z, half.z);

		if (mask_image.is_valid()) {
			const Vector2 uv((lx / volume_size.x) + 0.5f, (lz / volume_size.z) + 0.5f);
			if (!_sample_mask(mask_image, uv, rng)) {
				continue;
			}
		}

		const Vector2i cell(int(Math::floor(lx / spacing_cell_size)), int(Math::floor(lz / spacing_cell_size)));

		if (min_distance > 0.0f) {
			bool too_close = false;
			for (int cx = -1; cx <= 1 && !too_close; cx++) {
				for (int cz = -1; cz <= 1 && !too_close; cz++) {
					const LocalVector<Vector2> *bucket = grid.getptr(cell + Vector2i(cx, cz));
					if (bucket == nullptr) {
						continue;
					}
					for (const Vector2 &p : *bucket) {
						if (p.distance_squared_to(Vector2(lx, lz)) < min_distance_sq) {
							too_close = true;
							break;
						}
					}
				}
			}
			if (too_close) {
				continue;
			}
		}

		Vector3 local_pos;
		Vector3 world_normal(0, 1, 0);

		if (project_on_mesh) {
			const Vector2i fcell(int(Math::floor(lx / face_cell_size)), int(Math::floor(lz / face_cell_size)));
			const LocalVector<uint32_t> *face_indices = face_grid.getptr(fcell);
			if (face_indices == nullptr) {
				continue;
			}

			const Vector3 seg_from(lx, half.y, lz);
			const Vector3 seg_to(lx, -half.y, lz);

			bool hit_found = false;
			Vector3 best_point;
			Vector3 best_normal(0, 1, 0);
			float best_y = 0.0f;

			for (uint32_t face_index : *face_indices) {
				const Face3 &face = local_faces[face_index];
				Vector3 point;
				if (!face.intersects_segment(seg_from, seg_to, &point)) {
					continue;
				}
				if (!hit_found || point.y > best_y) {
					hit_found = true;
					best_y = point.y;
					best_point = point;
					Vector3 n = face.get_plane().normal;
					if (n.dot(Vector3(0, -1, 0)) > 0.0f) {
						n = -n;
					}
					best_normal = n;
				}
			}

			if (!hit_found) {
				continue;
			}

			const Vector3 hit_world_normal = gt.basis.xform(best_normal).normalized();
			if (max_slope_degrees < 90.0f) {
				const float angle = Math::rad_to_deg(Math::acos(CLAMP(hit_world_normal.dot(Vector3(0, 1, 0)), -1.0f, 1.0f)));
				if (angle > max_slope_degrees) {
					continue;
				}
			}

			local_pos = best_point;
			world_normal = hit_world_normal;
		} else {
			const float ly = rng.random(-half.y, half.y);
			local_pos = Vector3(lx, ly, lz);
		}

		Vector3 up_target(0, 1, 0);
		if (align_to_normal) {
			up_target = Vector3(0, 1, 0).lerp(world_normal, align_to_normal_amount);
			if (up_target.length_squared() < 0.0001f) {
				up_target = Vector3(0, 1, 0);
			} else {
				up_target.normalize();
			}
		}

		if (random_tilt_degrees > 0.0f) {
			Vector3 jitter_axis(rng.random(-1.0f, 1.0f), 0.0f, rng.random(-1.0f, 1.0f));
			if (jitter_axis.length_squared() > 0.0001f) {
				jitter_axis.normalize();
				const float jitter_angle = Math::deg_to_rad(rng.random(0.0f, random_tilt_degrees));
				up_target = up_target.rotated(jitter_axis, jitter_angle).normalized();
			}
		}

		const float yaw = random_rotation ? rng.random(0.0f, (float)Math::TAU) : 0.0f;
		Basis basis(Vector3(0, 1, 0), yaw);
		basis.rotate_to_align(Vector3(0, 1, 0), up_target);

		const float lo_scale = MIN(min_scale, max_scale);
		const float hi_scale = MAX(min_scale, max_scale);
		const float s = rng.random(lo_scale, hi_scale);
		basis = basis.scaled_local(Vector3(s, s, s));

		transforms.push_back(Transform3D(gt_inv.basis * basis, local_pos));

		if (min_distance > 0.0f) {
			grid[cell].push_back(Vector2(lx, lz));
		}
	}

	// Group instances by their chunking cell (see class comment) before
	// building each cell's MultiMesh, instead of one MultiMesh for everything.
	const float chunk_cell_size = MAX(cell_size, 0.01f);
	HashMap<Vector2i, LocalVector<Transform3D>> cell_transforms;

	for (uint32_t i = 0; i < transforms.size(); i++) {
		const Vector3 &origin = transforms[i].origin;
		const Vector2i cc(int(Math::floor(origin.x / chunk_cell_size)), int(Math::floor(origin.z / chunk_cell_size)));
		cell_transforms[cc].push_back(transforms[i]);
	}

	for (KeyValue<Vector2i, LocalVector<Transform3D>> &kv : cell_transforms) {
		FoliageCell &fc = _get_or_create_cell(kv.key);
		for (const Ref<MultiMesh> &mm : fc.lod_multimeshes) {
			_write_transforms(mm, kv.value);
		}
	}

	update_gizmos();
	update_configuration_warnings();
	// Refreshes the read-only cell_count/instance_count Inspector display.
	notify_property_list_changed();
}

AABB FoliageSpawner3D::get_aabb() const {
	const Vector3 sz = volume_size.abs();
	AABB box(sz * -0.5f, sz);
	// Cell nodes sit at this node's own local origin (see _sync_cell_lods), so
	// their MultiMesh AABBs are already in the same local space as `box`.
	for (const KeyValue<Vector2i, FoliageCell> &kv : cells) {
		for (const Ref<MultiMesh> &mm : kv.value.lod_multimeshes) {
			if (mm.is_valid()) {
				box.merge_with(mm->get_aabb());
			}
		}
	}
	return box;
}

Vector<AABB> FoliageSpawner3D::get_cell_local_aabbs() const {
	Vector<AABB> result;
	result.resize(cells.size());
	int i = 0;
	for (const KeyValue<Vector2i, FoliageCell> &kv : cells) {
		AABB cell_aabb;
		bool first = true;
		for (const Ref<MultiMesh> &mm : kv.value.lod_multimeshes) {
			if (mm.is_null()) {
				continue;
			}
			if (first) {
				cell_aabb = mm->get_aabb();
				first = false;
			} else {
				cell_aabb.merge_with(mm->get_aabb());
			}
		}
		result.write[i++] = cell_aabb;
	}
	return result;
}

PackedStringArray FoliageSpawner3D::get_configuration_warnings() const {
	PackedStringArray warnings = MultiMeshInstance3D::get_configuration_warnings();

	if (!has_any_mesh()) {
		warnings.push_back(RTR("No Mesh assigned on any LOD level. Add a Mesh via Lod Levels and press Regenerate to scatter instances."));
	}

	if (project_on_mesh) {
		MeshInstance3D *ground = Object::cast_to<MeshInstance3D>(is_inside_tree() ? get_node_or_null(ground_mesh_path) : nullptr);
		if (ground == nullptr) {
			warnings.push_back(RTR("Project On Mesh is enabled, but Ground Mesh Path does not point to a MeshInstance3D. Assign one, or disable Project On Mesh."));
		} else if (ground->get_mesh().is_null()) {
			warnings.push_back(RTR("The MeshInstance3D referenced by Ground Mesh Path has no Mesh assigned."));
		}
	}

	if (has_any_mesh() && cells.is_empty()) {
		warnings.push_back(RTR("No instances have been generated yet (or none matched the current settings). Press Regenerate after adjusting Density, Min Distance, the Distribution Mask, or the ground projection settings."));
	}

	if (volume_size.x <= 0.0f || volume_size.y <= 0.0f || volume_size.z <= 0.0f) {
		warnings.push_back(RTR("Volume Size must be greater than zero on every axis."));
	}

	return warnings;
}

FoliageSpawner3D::FoliageSpawner3D() {
	// cell_gi_mode already defaults to GI_MODE_DISABLED (see the field
	// declaration): baked global illumination (LightmapGI probes, VoxelGI,
	// SDFGI) is generally not worth its cost for grass and other small
	// scattered foliage, the visual difference is minor at that scale,
	// LightmapGI would otherwise try to lightmap every scattered instance,
	// and dynamic per-instance GI probe lookups add up with thousands of
	// MultiMesh instances. Users who do want it can switch Cell GI Mode back
	// to Dynamic in the inspector.

	TypedArray<FoliageLODLevel> defaults;

	Ref<FoliageLODLevel> lod0;
	lod0.instantiate();
	lod0->set_visibility_range_end(30.0f);
	lod0->set_visibility_range_end_margin(5.0f);
	lod0->set_visibility_range_fade_mode(GeometryInstance3D::VISIBILITY_RANGE_FADE_SELF);
	defaults.push_back(lod0);

	Ref<FoliageLODLevel> lod1;
	lod1.instantiate();
	lod1->set_visibility_range_begin(25.0f);
	lod1->set_visibility_range_begin_margin(5.0f);
	lod1->set_visibility_range_end(80.0f);
	lod1->set_visibility_range_end_margin(10.0f);
	lod1->set_visibility_range_fade_mode(GeometryInstance3D::VISIBILITY_RANGE_FADE_SELF);
	defaults.push_back(lod1);

	Ref<FoliageLODLevel> lod2;
	lod2.instantiate();
	lod2->set_visibility_range_begin(70.0f);
	lod2->set_visibility_range_begin_margin(10.0f);
	lod2->set_visibility_range_fade_mode(GeometryInstance3D::VISIBILITY_RANGE_FADE_SELF);
	defaults.push_back(lod2);

	set_lod_levels(defaults);
}
