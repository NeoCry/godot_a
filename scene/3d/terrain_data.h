/**************************************************************************/
/*  terrain_data.h                                                        */
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

#include "core/io/resource.h"
#include "core/math/rect2i.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"

class Image;

// The actual terrain "world" data for a Landscape3D: a square grid of height
// samples (a heightmap) plus, for texture splatting, one continuous 0-1
// "how much of this TerrainLayer shows here" weight per layer, plus a hole
// flag. All are stored as Images (a single-channel float heightmap, one
// RGBA8 image per 4 layers' weights, and a single-channel hole map) so they
// can be resized, imported from/exported to disk, and turned into GPU
// textures with the normal Image/Texture machinery.
//
// Every layer's weight is independent (not just a blend between two "slots"
// like a typical 2-layer control map): Landscape3D's shader samples all of
// them and mixes each layer's material in proportion to its share of the
// total weight at that point, the same "weight-blended" model CryEngine and
// UE4/5's Landscape layers use. This is what lets weights be sampled with
// normal bilinear filtering (unlike an index-based control map, where
// filtering would average together unrelated layer indices) for smooth
// blending at any brush or geometry density, and lets any number of layers
// overlap smoothly at a single point instead of only ever blending pairwise.
// Painting a layer (see Landscape3D::paint_layer) raises its weight and
// proportionally lowers every other layer's, keeping the total roughly
// constant, so repeatedly painting one layer converges on it fully replacing
// the others rather than capping out at an even split.
class TerrainData : public Resource {
	GDCLASS(TerrainData, Resource);

	int resolution = 513;
	float vertex_spacing = 1.0;

	Ref<Image> heightmap; // FORMAT_RF, resolution x resolution, height in world units (Y).
	Vector<Ref<Image>> weight_maps; // WEIGHT_MAP_COUNT x (FORMAT_RGBA8, resolution x resolution).
	Ref<Image> hole_map; // FORMAT_R8, resolution x resolution.

	void _init_images();

	// Internal, storage-only representation (see FoliagePainter3D::_get_cell_data
	// for the same pattern): keeps the Inspector from showing raw Image editors
	// for what's really bulk terrain data.
	Dictionary _get_storage_data() const;
	void _set_storage_data(const Dictionary &p_data);

protected:
	static void _bind_methods();

public:
	static constexpr int MIN_RESOLUTION = 2;
	static constexpr int MAX_RESOLUTION = 4097;

	// Hard cap on distinct TerrainLayers (must match Landscape3D's shader:
	// layer_uv_scales' fixed uniform array size). Weights for every layer up
	// to this count are always allocated, packed 4 per RGBA8 weight map.
	static constexpr int MAX_LAYERS = 32;
	static constexpr int LAYERS_PER_WEIGHT_MAP = 4;
	static constexpr int WEIGHT_MAP_COUNT = MAX_LAYERS / LAYERS_PER_WEIGHT_MAP;

	void set_resolution(int p_resolution);
	int get_resolution() const;

	void set_vertex_spacing(float p_spacing);
	float get_vertex_spacing() const;

	// World-space size of the terrain along X/Z (resolution is vertices, so
	// there is one fewer quad-span than there are vertices per side).
	float get_size() const;

	float get_height(int p_x, int p_z) const;
	void set_height(int p_x, int p_z, float p_height);

	PackedFloat32Array get_height_region(const Rect2i &p_region) const;
	void set_height_region(const Rect2i &p_region, const PackedFloat32Array &p_heights);

	// How much of TerrainLayer p_layer_index shows at this sample, from 0 to
	// 1 (see the class description for how this combines with every other
	// layer's weight at the same point).
	float get_layer_weight(int p_x, int p_z, int p_layer_index) const;
	void set_layer_weight(int p_x, int p_z, int p_layer_index, float p_weight);

	PackedFloat32Array get_layer_weight_region(const Rect2i &p_region, int p_layer_index) const;
	void set_layer_weight_region(const Rect2i &p_region, int p_layer_index, const PackedFloat32Array &p_weights);

	bool is_hole(int p_x, int p_z) const;
	void set_hole(int p_x, int p_z, bool p_hole);

	PackedByteArray get_hole_region(const Rect2i &p_region) const;
	void set_hole_region(const Rect2i &p_region, const PackedByteArray &p_holes);

	Vector3 get_normal(int p_x, int p_z) const;

	// Gameplay-facing queries, in the terrain's local space (XZ plane, Y up),
	// bilinearly filtered between samples.
	float get_height_at_position(const Vector2 &p_local_xz) const;
	Vector3 get_normal_at_position(const Vector2 &p_local_xz) const;

	void fill_height(float p_height);
	void import_heightmap(const Ref<Image> &p_image, float p_height_min, float p_height_max);

	// Plain C++ helpers for Landscape3D's mesh building and texture upload; not
	// bound to ClassDB, like FoliagePainter3D's own editor-only helpers.
	Ref<Image> get_heightmap_image() const;
	Ref<Image> get_weight_map_image(int p_group) const;
	Ref<Image> get_hole_map_image() const;

	// A HeightMapShape3D-compatible column-major sample array (see
	// HeightMapShape3D::set_map_data): same coordinate order, one real_t per
	// sample, no format conversion needed by the caller.
	Vector<real_t> get_collision_heights() const;

	TerrainData();
};
