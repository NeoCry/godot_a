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

// The actual terrain "world" data for a Terrain3D: a square grid of height
// samples (a heightmap) plus a per-vertex control map that drives texture
// splatting. Both are stored as Images (a single-channel float heightmap and
// an RGBA8 control map) so they can be resized, imported from/exported to
// disk, and turned into GPU textures with the normal Image/Texture machinery.
//
// The control map's channels are: R = base TerrainLayer index, G = overlay
// TerrainLayer index, B = blend factor between them (0 = pure base, 1 = pure
// overlay), A = hole flag. A brush paints a new layer in by setting it as the
// overlay and raising blend towards 1; once blend reaches 1 the overlay
// becomes the new base (see Terrain3D::paint_layer), so any number of layers
// can be painted over time while every vertex only ever blends between two of
// them at once.
class TerrainData : public Resource {
	GDCLASS(TerrainData, Resource);

	int resolution = 513;
	float vertex_spacing = 1.0;

	Ref<Image> heightmap; // FORMAT_RF, resolution x resolution, height in world units (Y).
	Ref<Image> control_map; // FORMAT_RGBA8, resolution x resolution.

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

	// Control map accessor as a Color for convenience: r = base layer index /
	// 255, g = overlay layer index / 255, b = blend, a = hole flag (0 or 1).
	Color get_control(int p_x, int p_z) const;
	void set_control(int p_x, int p_z, const Color &p_control);

	PackedColorArray get_control_region(const Rect2i &p_region) const;
	void set_control_region(const Rect2i &p_region, const PackedColorArray &p_control);

	bool is_hole(int p_x, int p_z) const;

	Vector3 get_normal(int p_x, int p_z) const;

	// Gameplay-facing queries, in the terrain's local space (XZ plane, Y up),
	// bilinearly filtered between samples.
	float get_height_at_position(const Vector2 &p_local_xz) const;
	Vector3 get_normal_at_position(const Vector2 &p_local_xz) const;

	void fill_height(float p_height);
	void import_heightmap(const Ref<Image> &p_image, float p_height_min, float p_height_max);

	// Plain C++ helpers for Terrain3D's mesh building and texture upload; not
	// bound to ClassDB, like FoliagePainter3D's own editor-only helpers.
	Ref<Image> get_heightmap_image() const;
	Ref<Image> get_control_map_image() const;

	// A HeightMapShape3D-compatible column-major sample array (see
	// HeightMapShape3D::set_map_data): same coordinate order, one real_t per
	// sample, no format conversion needed by the caller.
	Vector<real_t> get_collision_heights() const;

	TerrainData();
};
