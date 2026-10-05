/**************************************************************************/
/*  landscape_spline_flow.h                                               */
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

#include "core/io/image.h"
#include "core/math/rect2i.h"
#include "core/math/vector2.h"
#include "core/math/vector4.h"
#include "core/object/object_id.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/templates/vector.h"
#include "scene/resources/image_texture.h"

// The current of a river or stream, worked out over a grid laid along its
// spline: what LandscapeSpline3D's built-in water reads its current from.
//
// The water is treated as a thin sheet whose depth varies from place to
// place, flowing without friction and without swirling (potential flow, over
// depth): the discharge h * v has no divergence and v is the gradient of a
// potential, so div(h grad(phi)) = 0. That is what makes it part around
// rocks and run faster past them, run faster over shallows that span the
// channel and slower through pools, and keep moving at the same speed across
// a channel whose bed only shelves towards the banks (the bank's own
// slowdown is the material's bank_flow, which this does not double).
// Potential flow has neither wakes nor the outer bank's faster water in a
// bend, so both are put back on top of it:
//  - Bends: the free vortex potential flow makes of a bend runs fastest along
//    the inner bank; real rivers carry their fastest water towards the outer
//    one, from a little past where the bend starts. The speeds across each
//    cross-section are tilted towards the outer bank by a lagged curvature,
//    keeping the discharge through it.
//  - Wakes: wherever the current leaves an obstacle behind (it flows away
//    from a solid's face) a wake starts, which is carried downstream with the
//    current, spreads sideways and fades, so a pebble's dies out within a few
//    meters and a boulder's reaches far. The water in it is slowed.
//  - Turbulence: where the water piles up against an obstacle, where it
//    pours over a shallow one, where it is sheltered in a wake and along the
//    shear between a wake and the faster water beside it, carried downstream
//    the same way. The shader raises foam and choppier ripples from it.
namespace LandscapeSplineFlow {

// How fast the current can be packed, as a multiple of the midstream speed
// (the shader's FLOW_RANGE).
constexpr float VELOCITY_RANGE = 3.0f;
// Less water than this, in meters, is dry ground.
constexpr float MIN_DEPTH = 0.02f;

struct Grid {
	// Columns run across the channel, from its left edge (u = 0) to its
	// right one (u = 1), rows along it, from upstream to downstream. Every
	// array is rows * columns, row after row.
	int columns = 0;
	int rows = 0;
	// Where each cell is in the water's plane, in meters.
	LocalVector<Vector2> positions;
	// How deep the water is there, in meters; MIN_DEPTH or less where it is
	// dry (a bank, a rock or a cliff standing out of it).
	LocalVector<float> depths;
};

struct Field {
	int columns = 0;
	int rows = 0;
	// The current, across the channel (towards u = 1) and along it
	// (downstream), as multiples of the typical speed in midstream.
	LocalVector<Vector2> current;
	// 0-1: how churned up the water is.
	LocalVector<float> turbulence;
	// 0-1: how sheltered from the current it is, behind an obstacle.
	LocalVector<float> wake;
};

void solve(const Grid &p_grid, Field &r_field);

// RGBA8 texels for the field at rows p_from_row to p_to_row (fractional,
// inclusive), resampled to p_rows rows: the current in RG (as
// 0.5 + 0.5 * current / VELOCITY_RANGE), turbulence in B and wake in A.
void pack_rows(const Field &p_field, float p_from_row, float p_to_row, int p_rows, Vector<uint8_t> &r_texels);

} // namespace LandscapeSplineFlow

// The flow fields of every spline, each cut into a tile per chunk, packed
// together into one texture, so that every water material samples the same
// one whatever spline it is on (the tile it reads is an instance uniform of
// the chunk). Tiles are packed in shelves; when one does not fit, the atlas
// is repacked, and grown if that is not enough, and the owners of the tiles
// that moved are told to look them up again. Main thread only.
class LandscapeFlowAtlas {
public:
	static constexpr int MIN_SIZE = 256;
	static constexpr int MAX_SIZE = 4096;

private:
	struct Tile {
		ObjectID owner;
		Rect2i rect;
		Vector<uint8_t> texels;
	};

	struct Shelf {
		int y = 0;
		int height = 0;
		int used_width = 0;
	};

	static inline LandscapeFlowAtlas *singleton = nullptr;

	HashMap<uint32_t, Tile> tiles;
	LocalVector<Shelf> shelves;
	uint32_t next_id = 1;
	int size = 0;
	Ref<Image> image;
	Ref<ImageTexture> texture;
	bool upload_queued = false;
	bool resized = false;

	bool _place(Tile &p_tile);
	bool _repack(int p_size);
	void _write(const Tile &p_tile);
	void _queue_upload();
	static void _upload();
	void _notify_moved(const HashSet<ObjectID> &p_owners);

public:
	static LandscapeFlowAtlas *get_singleton();
	static bool exists() { return singleton != nullptr; }
	static void finish();

	// A tile of p_width x p_height texels (RGBA8, see
	// LandscapeSplineFlow::pack_rows()). 0 when it cannot fit even in the
	// largest atlas.
	uint32_t allocate(ObjectID p_owner, int p_width, int p_height, const Vector<uint8_t> &p_texels);
	// Replaces a tile's texels where it is. False, changing nothing, when the
	// new ones are not the same size.
	bool update(uint32_t p_tile, int p_width, int p_height, const Vector<uint8_t> &p_texels);
	void release(uint32_t p_tile);
	// Where the tile is, as the shader's flow_tile: the atlas UV of its first
	// texel's center in xy, and how far UV2 0-1 reaches across it in zw.
	Vector4 get_tile_transform(uint32_t p_tile) const;
	bool has_tile(uint32_t p_tile) const { return tiles.has(p_tile); }
	// The atlas texture, which keeps its RID however it is resized.
	RID get_texture() const;
	int get_size() const { return size; }
	// What the texture holds, or is about to once the upload queued at the
	// end of the frame has run. For tests and tools.
	Ref<Image> get_image() const { return image; }
	int get_tile_count() const { return tiles.size(); }

	LandscapeFlowAtlas() = default;
	~LandscapeFlowAtlas();
};
