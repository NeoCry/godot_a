/**************************************************************************/
/*  ambient_probe_volume_3d.h                                             */
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

#include "scene/3d/node_3d.h"

class GeometryInstance3D;
class ImageTexture3D;
class Material;
class Shader;
class ShaderMaterial;

// Bakes large-scale ambient occlusion into a grid of probes filling a box volume,
// entirely on the CPU and independent of LightmapGI: it raycasts against the
// triangle geometry of nearby MeshInstance3D nodes (no UV2, lightmap texel bake,
// or LightmapProbe/LightmapGI involvement whatsoever), and stores one AO value
// per probe. Meant for shading things that don't carry (and don't want) their own
// lightmap, such as vegetation: read back baked values with get_ao_at(), e.g. to
// tint MultiMesh instances, or sample the raw data from a script/shader as needed.
class AmbientProbeVolume3D : public Node3D {
	GDCLASS(AmbientProbeVolume3D, Node3D);

	Vector3 size = Vector3(20, 10, 20);
	Vector3i probe_counts = Vector3i(6, 3, 6);
	int ray_count = 48;
	float max_distance = 8.0;
	float ao_strength = 1.0;
	float ao_smoothing = 0.0f;
	NodePath occluder_root;

	NodePath apply_target;
	StringName apply_shader_parameter = StringName("ambient_occlusion");
	bool debug_preview_on_meshes = false;
	bool ao_overlay_enabled = false;
	float ao_overlay_strength = 1.0f;
	Ref<ShaderMaterial> debug_material;
	Ref<ShaderMaterial> overlay_material;
	Ref<ImageTexture3D> ao_volume_texture;

	PackedFloat32Array baked_ao;

	// Diagnostics from the last bake_ao()/apply_to_instances() call, not persisted;
	// -1 means "hasn't run yet". Surfaced via get_configuration_warnings() and
	// printed to the Output panel so a bake/apply that quietly did nothing useful
	// (e.g. no occluder geometry found, or no GeometryInstance3D to apply to) is
	// easy to tell apart from one that worked but simply found no occlusion.
	int last_bake_occluder_face_count = -1;
	int last_apply_instance_count = -1;
	// Distance (local space) from the probe grid's box to the closest occluder geometry
	// found by the last bake, or -1.0 if unknown (nothing baked yet, or no occluders
	// found at all). Lets get_configuration_warnings() tell "everything is too far away
	// to reach" apart from other reasons a bake might find zero occlusion.
	float last_bake_min_occluder_distance = -1.0f;

	Callable _get_bake_button() const;
	Callable _get_clear_button() const;
	Callable _get_apply_button() const;

	void _apply_to_instances(Node *p_node, int &r_count);
	void _set_material_override_recursive(Node *p_node, const Ref<Material> &p_material, int &r_count);
	void _set_material_overlay_recursive(Node *p_node, const Ref<Material> &p_material, int &r_count);
	void _configure_overlay_alpha_scissor(GeometryInstance3D *p_gi);
	Node *_resolve_apply_root() const;
	void _rebuild_ao_volume_texture();
	void _push_ao_volume_uniforms(const Ref<ShaderMaterial> &p_material);
	PackedFloat32Array _smooth_baked_ao(const PackedFloat32Array &p_raw) const;

	void _set_baked_ao(const PackedFloat32Array &p_data);
	PackedFloat32Array _get_baked_ao() const;

protected:
	static void _bind_methods();

public:
	void set_size(const Vector3 &p_size);
	Vector3 get_size() const;

	void set_probe_counts(const Vector3i &p_counts);
	Vector3i get_probe_counts() const;

	void set_ray_count(int p_ray_count);
	int get_ray_count() const;

	void set_max_distance(float p_max_distance);
	float get_max_distance() const;

	void set_ao_strength(float p_strength);
	float get_ao_strength() const;

	// How much bake_ao() box-blurs each probe's raw raycast result against its immediate
	// neighbors (3x3x3, clamped at the grid's edges) before storing it, from 0.0 (off -
	// each probe keeps its own raw value) to 1.0 (fully replaced by the neighborhood
	// average). Raycasting with a limited ray_count is inherently noisy, and a sparse
	// probe grid trilinearly interpolated between very different neighboring values can
	// look blotchy/inconsistent with the actual geometry rather than like soft occlusion;
	// smoothing the baked data itself (once, at bake time) fixes that at the source for
	// every consumer (get_ao_at(), the overlay, apply_to_instances(), etc.) instead of
	// requiring each of them to filter it themselves. Takes effect on the next bake_ao().
	void set_ao_smoothing(float p_smoothing);
	float get_ao_smoothing() const;

	void set_occluder_root(const NodePath &p_path);
	NodePath get_occluder_root() const;

	void set_apply_target(const NodePath &p_path);
	NodePath get_apply_target() const;

	void set_apply_shader_parameter(const StringName &p_name);
	StringName get_apply_shader_parameter() const;

	// Local-space position of probe (p_x, p_y, p_z) in the [0, probe_counts) grid;
	// used both to bake and by the editor gizmo preview.
	Vector3 get_local_probe_position(int p_x, int p_y, int p_z) const;

	// Baked ambient occlusion at probe (p_x, p_y, p_z) in the [0, probe_counts) grid
	// (1.0 = fully lit, 0.0 = fully occluded). Returns 1.0 if out of range or not baked
	// yet. For an arbitrary world-space position, use get_ao_at() instead, which
	// interpolates between the probes surrounding it.
	float get_probe_ao(int p_x, int p_y, int p_z) const;

	void bake_ao();
	void clear_ao();
	bool is_baked() const;

	// Recursively walks apply_target (or the current scene root if unset), and for
	// every GeometryInstance3D found, sets its instance shader parameter named
	// apply_shader_parameter to get_ao_at() at that instance's position. This is how
	// baked AO actually reaches a mesh's shading: nothing samples it automatically,
	// so the mesh's shader must declare a matching "instance uniform float
	// <apply_shader_parameter> : hint_range(0, 1) = 1.0;" and use it (e.g. multiply
	// it into ALBEDO or ALPHA). Requires the volume to be baked and inside the tree.
	void apply_to_instances();

	// Debug aid: when enabled, forces every GeometryInstance3D under apply_target (or
	// the current scene root) to render fully unshaded, tinted grayscale by the baked
	// AO at each rendered *fragment's* world position (via a generated material_override
	// sampling a 3D texture built from baked_ao) - no custom shader required, and with
	// real per-pixel detail even across a single huge mesh (e.g. an entire imported
	// building), unlike apply_to_instances()'s necessarily one-value-per-node result.
	// This is the most direct way to see whether baking actually did anything to the
	// real geometry, independent of whatever the mesh's own material does with the data.
	// Disabling it clears material_override on every instance it finds; it does not
	// remember or restore whatever material_override those instances had before. This
	// is a diagnostic override (see set_ao_overlay_enabled() for a real, shippable effect).
	void set_debug_preview_on_meshes(bool p_enabled);
	bool is_debug_preview_on_meshes() const;

	// The actual, shippable way to make baked AO visible on ordinary meshes without
	// writing a shader or touching their material: when enabled, every GeometryInstance3D
	// under apply_target gets a generated material_overlay (not material_override, so the
	// mesh's own material/textures are untouched) that draws pure black with alpha =
	// (1 - AO) at each fragment's world position, standard alpha-blended on top. Since
	// blending gives final = base * ao + black * (1 - ao) = base * ao, this is a correct
	// multiplicative darkening of whatever was already rendered, not an approximation.
	// Costs one extra draw per affected instance (the overlay pass), same as any other
	// use of material_overlay. ao_overlay_strength scales how strong the darkening is.
	// For an instance whose surface 0 uses a BaseMaterial3D with alpha scissor/hash/depth
	// pre-pass transparency and an albedo texture (the common setup for cutout foliage
	// cards), the overlay samples that same texture/threshold and discards to match, so it
	// doesn't paint over the parts the base material already treats as fully transparent.
	void set_ao_overlay_enabled(bool p_enabled);
	bool is_ao_overlay_enabled() const;

	void set_ao_overlay_strength(float p_strength);
	float get_ao_overlay_strength() const;

	// Trilinearly sampled ambient occlusion (1.0 = fully lit, 0.0 = fully occluded)
	// at a world-space position. Returns 1.0 if this volume hasn't been baked yet,
	// or if p_world_position falls outside its box.
	float get_ao_at(const Vector3 &p_world_position) const;

	PackedStringArray get_configuration_warnings() const override;

	AmbientProbeVolume3D();
};
