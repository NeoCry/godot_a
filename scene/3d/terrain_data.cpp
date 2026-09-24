/**************************************************************************/
/*  terrain_data.cpp                                                      */
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

#include "terrain_data.h"

#include "core/io/image.h"
#include "core/object/class_db.h"

namespace {
// Layer 0's weight starts at 1 everywhere (weight map group 0's red channel)
// so a freshly created/resized terrain renders as one solid, paintable
// layer instead of nothing; every other layer starts unpainted.
Ref<Image> _make_weight_map(int p_group, int p_resolution) {
	Ref<Image> img;
	img.instantiate();
	img->initialize_data(p_resolution, p_resolution, false, Image::FORMAT_RGBA8);
	img->fill(p_group == 0 ? Color(1, 0, 0, 0) : Color(0, 0, 0, 0));
	return img;
}
} // namespace

void TerrainData::_init_images() {
	heightmap.instantiate();
	heightmap->initialize_data(resolution, resolution, false, Image::FORMAT_RF);
	heightmap->fill(Color(0, 0, 0));

	weight_maps.resize(WEIGHT_MAP_COUNT);
	for (int g = 0; g < WEIGHT_MAP_COUNT; g++) {
		weight_maps.write[g] = _make_weight_map(g, resolution);
	}

	hole_map.instantiate();
	hole_map->initialize_data(resolution, resolution, false, Image::FORMAT_R8);
	hole_map->fill(Color(0, 0, 0));
}

Dictionary TerrainData::_get_storage_data() const {
	Dictionary d;
	d["resolution"] = resolution;
	d["heightmap"] = heightmap;
	Array weight_maps_arr;
	for (int g = 0; g < weight_maps.size(); g++) {
		weight_maps_arr.push_back(weight_maps[g]);
	}
	d["weight_maps"] = weight_maps_arr;
	d["hole_map"] = hole_map;
	return d;
}

void TerrainData::_set_storage_data(const Dictionary &p_data) {
	resolution = CLAMP((int)p_data.get("resolution", resolution), MIN_RESOLUTION, MAX_RESOLUTION);
	Ref<Image> stored_heightmap = p_data.get("heightmap", Ref<Image>());
	Array stored_weight_maps = p_data.get("weight_maps", Array());
	Ref<Image> stored_hole_map = p_data.get("hole_map", Ref<Image>());

	if (stored_heightmap.is_valid() && stored_heightmap->get_width() == resolution && stored_heightmap->get_height() == resolution) {
		heightmap = stored_heightmap;
	} else {
		heightmap.instantiate();
		heightmap->initialize_data(resolution, resolution, false, Image::FORMAT_RF);
		heightmap->fill(Color(0, 0, 0));
	}

	weight_maps.resize(WEIGHT_MAP_COUNT);
	for (int g = 0; g < WEIGHT_MAP_COUNT; g++) {
		Ref<Image> stored_img;
		if (g < stored_weight_maps.size()) {
			stored_img = stored_weight_maps[g];
		}
		if (stored_img.is_valid() && stored_img->get_width() == resolution && stored_img->get_height() == resolution) {
			weight_maps.write[g] = stored_img;
		} else {
			weight_maps.write[g] = _make_weight_map(g, resolution);
		}
	}

	if (stored_hole_map.is_valid() && stored_hole_map->get_width() == resolution && stored_hole_map->get_height() == resolution) {
		hole_map = stored_hole_map;
	} else {
		hole_map.instantiate();
		hole_map->initialize_data(resolution, resolution, false, Image::FORMAT_R8);
		hole_map->fill(Color(0, 0, 0));
	}
}

void TerrainData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_resolution", "resolution"), &TerrainData::set_resolution);
	ClassDB::bind_method(D_METHOD("get_resolution"), &TerrainData::get_resolution);

	ClassDB::bind_method(D_METHOD("set_vertex_spacing", "spacing"), &TerrainData::set_vertex_spacing);
	ClassDB::bind_method(D_METHOD("get_vertex_spacing"), &TerrainData::get_vertex_spacing);

	ClassDB::bind_method(D_METHOD("get_size"), &TerrainData::get_size);

	ClassDB::bind_method(D_METHOD("get_height", "x", "z"), &TerrainData::get_height);
	ClassDB::bind_method(D_METHOD("set_height", "x", "z", "height"), &TerrainData::set_height);

	ClassDB::bind_method(D_METHOD("get_height_region", "region"), &TerrainData::get_height_region);
	ClassDB::bind_method(D_METHOD("set_height_region", "region", "heights"), &TerrainData::set_height_region);

	ClassDB::bind_method(D_METHOD("get_layer_weight", "x", "z", "layer_index"), &TerrainData::get_layer_weight);
	ClassDB::bind_method(D_METHOD("set_layer_weight", "x", "z", "layer_index", "weight"), &TerrainData::set_layer_weight);

	ClassDB::bind_method(D_METHOD("get_layer_weight_region", "region", "layer_index"), &TerrainData::get_layer_weight_region);
	ClassDB::bind_method(D_METHOD("set_layer_weight_region", "region", "layer_index", "weights"), &TerrainData::set_layer_weight_region);

	ClassDB::bind_method(D_METHOD("is_hole", "x", "z"), &TerrainData::is_hole);
	ClassDB::bind_method(D_METHOD("set_hole", "x", "z", "hole"), &TerrainData::set_hole);

	ClassDB::bind_method(D_METHOD("get_hole_region", "region"), &TerrainData::get_hole_region);
	ClassDB::bind_method(D_METHOD("set_hole_region", "region", "holes"), &TerrainData::set_hole_region);

	ClassDB::bind_method(D_METHOD("get_normal", "x", "z"), &TerrainData::get_normal);

	ClassDB::bind_method(D_METHOD("get_height_at_position", "local_xz"), &TerrainData::get_height_at_position);
	ClassDB::bind_method(D_METHOD("get_normal_at_position", "local_xz"), &TerrainData::get_normal_at_position);

	ClassDB::bind_method(D_METHOD("fill_height", "height"), &TerrainData::fill_height);
	ClassDB::bind_method(D_METHOD("import_heightmap", "image", "height_min", "height_max"), &TerrainData::import_heightmap);
	ClassDB::bind_method(D_METHOD("import_layer_mask", "image", "layer_index", "channel", "normalize"), &TerrainData::import_layer_mask, DEFVAL(MASK_CHANNEL_RED), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("generate_layer_mask", "layer_index", "height_min", "height_max", "height_falloff", "slope_min", "slope_max", "slope_falloff", "normalize"), &TerrainData::generate_layer_mask, DEFVAL(-100000.0), DEFVAL(100000.0), DEFVAL(0.0), DEFVAL(0.0), DEFVAL(90.0), DEFVAL(0.0), DEFVAL(true));

	ClassDB::bind_method(D_METHOD("_get_storage_data"), &TerrainData::_get_storage_data);
	ClassDB::bind_method(D_METHOD("_set_storage_data", "data"), &TerrainData::_set_storage_data);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "resolution", PROPERTY_HINT_RANGE, vformat("%d,%d,1", MIN_RESOLUTION, MAX_RESOLUTION)), "set_resolution", "get_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "vertex_spacing", PROPERTY_HINT_RANGE, "0.01,100.0,0.01,or_greater,suffix:m"), "set_vertex_spacing", "get_vertex_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "_storage_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_INTERNAL | PROPERTY_USAGE_STORAGE), "_set_storage_data", "_get_storage_data");

	BIND_CONSTANT(MAX_LAYERS);

	BIND_ENUM_CONSTANT(MASK_CHANNEL_RED);
	BIND_ENUM_CONSTANT(MASK_CHANNEL_GREEN);
	BIND_ENUM_CONSTANT(MASK_CHANNEL_BLUE);
	BIND_ENUM_CONSTANT(MASK_CHANNEL_ALPHA);
}

void TerrainData::set_resolution(int p_resolution) {
	p_resolution = CLAMP(p_resolution, MIN_RESOLUTION, MAX_RESOLUTION);
	if (p_resolution == resolution && heightmap.is_valid()) {
		return;
	}
	resolution = p_resolution;
	_init_images();
	emit_changed();
}

int TerrainData::get_resolution() const {
	return resolution;
}

void TerrainData::set_vertex_spacing(float p_spacing) {
	vertex_spacing = MAX(p_spacing, 0.01f);
	emit_changed();
}

float TerrainData::get_vertex_spacing() const {
	return vertex_spacing;
}

float TerrainData::get_size() const {
	return (float)(resolution - 1) * vertex_spacing;
}

// TerrainData deliberately never calls Image::get_pixel()/set_pixel() (used
// by the very first version of this file): a single brush stamp or chunk
// rebuild touches thousands of samples, and get_pixel/set_pixel's per-call
// dispatch on the image's format is what made sculpting/painting/smoothing
// visibly slow. Every accessor below instead reads/writes the image's raw
// byte buffer directly (heightmap is always FORMAT_RF, one float per pixel;
// each weight map is always FORMAT_RGBA8, one byte per channel/layer; the
// hole map is always FORMAT_R8), which is a simple array index.
// Image::get_data() returns that buffer by reference (free), while a write
// needs one get_data()+set_data() round trip to commit (one copy-on-write of
// the whole image) - cheap by itself, but still worth batching: callers
// doing many edits (TerrainData's own *_region methods, and Landscape3D's
// sculpt/paint_layer/set_hole) do exactly one such round trip per call, not
// one per sample.
float TerrainData::get_height(int p_x, int p_z) const {
	p_x = CLAMP(p_x, 0, resolution - 1);
	p_z = CLAMP(p_z, 0, resolution - 1);
	const Vector<uint8_t> &raw = heightmap->get_data();
	return reinterpret_cast<const float *>(raw.ptr())[p_z * resolution + p_x];
}

void TerrainData::set_height(int p_x, int p_z, float p_height) {
	if (p_x < 0 || p_x >= resolution || p_z < 0 || p_z >= resolution) {
		return;
	}
	Vector<uint8_t> raw = heightmap->get_data();
	reinterpret_cast<float *>(raw.ptrw())[p_z * resolution + p_x] = p_height;
	heightmap->set_data(resolution, resolution, false, Image::FORMAT_RF, raw);
}

PackedFloat32Array TerrainData::get_height_region(const Rect2i &p_region) const {
	const int w = MAX(p_region.size.x, 0);
	const int h = MAX(p_region.size.y, 0);
	PackedFloat32Array result;
	result.resize(w * h);
	if (w == 0 || h == 0) {
		return result;
	}

	const Vector<uint8_t> &raw = heightmap->get_data();
	const float *src = reinterpret_cast<const float *>(raw.ptr());
	float *dst = result.ptrw();

	int i = 0;
	for (int z = 0; z < h; z++) {
		const int sz = CLAMP(p_region.position.y + z, 0, resolution - 1);
		for (int x = 0; x < w; x++) {
			const int sx = CLAMP(p_region.position.x + x, 0, resolution - 1);
			dst[i++] = src[sz * resolution + sx];
		}
	}
	return result;
}

void TerrainData::set_height_region(const Rect2i &p_region, const PackedFloat32Array &p_heights) {
	const int w = MAX(p_region.size.x, 0);
	const int h = MAX(p_region.size.y, 0);
	ERR_FAIL_COND(p_heights.size() < w * h);
	if (w == 0 || h == 0) {
		return;
	}

	Vector<uint8_t> raw = heightmap->get_data();
	float *dst = reinterpret_cast<float *>(raw.ptrw());
	const float *src = p_heights.ptr();

	int i = 0;
	for (int z = 0; z < h; z++) {
		const int dz = p_region.position.y + z;
		if (dz < 0 || dz >= resolution) {
			i += w;
			continue;
		}
		for (int x = 0; x < w; x++) {
			const int dx = p_region.position.x + x;
			if (dx >= 0 && dx < resolution) {
				dst[dz * resolution + dx] = src[i];
			}
			i++;
		}
	}
	heightmap->set_data(resolution, resolution, false, Image::FORMAT_RF, raw);
	emit_changed();
}

float TerrainData::get_layer_weight(int p_x, int p_z, int p_layer_index) const {
	ERR_FAIL_INDEX_V(p_layer_index, MAX_LAYERS, 0.0f);
	p_x = CLAMP(p_x, 0, resolution - 1);
	p_z = CLAMP(p_z, 0, resolution - 1);
	const int group = p_layer_index / LAYERS_PER_WEIGHT_MAP;
	const int channel = p_layer_index % LAYERS_PER_WEIGHT_MAP;
	const Vector<uint8_t> &raw = weight_maps[group]->get_data();
	return raw.ptr()[(p_z * resolution + p_x) * 4 + channel] / 255.0f;
}

void TerrainData::set_layer_weight(int p_x, int p_z, int p_layer_index, float p_weight) {
	ERR_FAIL_INDEX(p_layer_index, MAX_LAYERS);
	if (p_x < 0 || p_x >= resolution || p_z < 0 || p_z >= resolution) {
		return;
	}
	const int group = p_layer_index / LAYERS_PER_WEIGHT_MAP;
	const int channel = p_layer_index % LAYERS_PER_WEIGHT_MAP;
	Vector<uint8_t> raw = weight_maps[group]->get_data();
	raw.ptrw()[(p_z * resolution + p_x) * 4 + channel] = (uint8_t)CLAMP(Math::round(p_weight * 255.0f), 0.0f, 255.0f);
	weight_maps.write[group]->set_data(resolution, resolution, false, Image::FORMAT_RGBA8, raw);
}

PackedFloat32Array TerrainData::get_layer_weight_region(const Rect2i &p_region, int p_layer_index) const {
	const int w = MAX(p_region.size.x, 0);
	const int h = MAX(p_region.size.y, 0);
	PackedFloat32Array result;
	result.resize(w * h);
	if (w == 0 || h == 0) {
		return result;
	}
	ERR_FAIL_INDEX_V(p_layer_index, MAX_LAYERS, result);

	const int group = p_layer_index / LAYERS_PER_WEIGHT_MAP;
	const int channel = p_layer_index % LAYERS_PER_WEIGHT_MAP;
	const Vector<uint8_t> &raw = weight_maps[group]->get_data();
	const uint8_t *src = raw.ptr();
	float *dst = result.ptrw();

	int i = 0;
	for (int z = 0; z < h; z++) {
		const int sz = CLAMP(p_region.position.y + z, 0, resolution - 1);
		for (int x = 0; x < w; x++) {
			const int sx = CLAMP(p_region.position.x + x, 0, resolution - 1);
			dst[i++] = src[(sz * resolution + sx) * 4 + channel] / 255.0f;
		}
	}
	return result;
}

void TerrainData::set_layer_weight_region(const Rect2i &p_region, int p_layer_index, const PackedFloat32Array &p_weights) {
	const int w = MAX(p_region.size.x, 0);
	const int h = MAX(p_region.size.y, 0);
	ERR_FAIL_COND(p_weights.size() < w * h);
	if (w == 0 || h == 0) {
		return;
	}
	ERR_FAIL_INDEX(p_layer_index, MAX_LAYERS);

	const int group = p_layer_index / LAYERS_PER_WEIGHT_MAP;
	const int channel = p_layer_index % LAYERS_PER_WEIGHT_MAP;
	Vector<uint8_t> raw = weight_maps[group]->get_data();
	uint8_t *dst = raw.ptrw();
	const float *src = p_weights.ptr();

	int i = 0;
	for (int z = 0; z < h; z++) {
		const int dz = p_region.position.y + z;
		if (dz < 0 || dz >= resolution) {
			i += w;
			continue;
		}
		for (int x = 0; x < w; x++) {
			const int dx = p_region.position.x + x;
			if (dx >= 0 && dx < resolution) {
				dst[(dz * resolution + dx) * 4 + channel] = (uint8_t)CLAMP(Math::round(src[i] * 255.0f), 0.0f, 255.0f);
			}
			i++;
		}
	}
	weight_maps.write[group]->set_data(resolution, resolution, false, Image::FORMAT_RGBA8, raw);
	emit_changed();
}

bool TerrainData::is_hole(int p_x, int p_z) const {
	p_x = CLAMP(p_x, 0, resolution - 1);
	p_z = CLAMP(p_z, 0, resolution - 1);
	const Vector<uint8_t> &raw = hole_map->get_data();
	return raw.ptr()[p_z * resolution + p_x] > 0;
}

void TerrainData::set_hole(int p_x, int p_z, bool p_hole) {
	if (p_x < 0 || p_x >= resolution || p_z < 0 || p_z >= resolution) {
		return;
	}
	Vector<uint8_t> raw = hole_map->get_data();
	raw.ptrw()[p_z * resolution + p_x] = p_hole ? 255 : 0;
	hole_map->set_data(resolution, resolution, false, Image::FORMAT_R8, raw);
}

PackedByteArray TerrainData::get_hole_region(const Rect2i &p_region) const {
	const int w = MAX(p_region.size.x, 0);
	const int h = MAX(p_region.size.y, 0);
	PackedByteArray result;
	result.resize(w * h);
	if (w == 0 || h == 0) {
		return result;
	}

	const Vector<uint8_t> &raw = hole_map->get_data();
	const uint8_t *src = raw.ptr();
	uint8_t *dst = result.ptrw();

	int i = 0;
	for (int z = 0; z < h; z++) {
		const int sz = CLAMP(p_region.position.y + z, 0, resolution - 1);
		for (int x = 0; x < w; x++) {
			const int sx = CLAMP(p_region.position.x + x, 0, resolution - 1);
			dst[i++] = src[sz * resolution + sx] > 0 ? 1 : 0;
		}
	}
	return result;
}

void TerrainData::set_hole_region(const Rect2i &p_region, const PackedByteArray &p_holes) {
	const int w = MAX(p_region.size.x, 0);
	const int h = MAX(p_region.size.y, 0);
	ERR_FAIL_COND(p_holes.size() < w * h);
	if (w == 0 || h == 0) {
		return;
	}

	Vector<uint8_t> raw = hole_map->get_data();
	uint8_t *dst = raw.ptrw();
	const uint8_t *src = p_holes.ptr();

	int i = 0;
	for (int z = 0; z < h; z++) {
		const int dz = p_region.position.y + z;
		if (dz < 0 || dz >= resolution) {
			i += w;
			continue;
		}
		for (int x = 0; x < w; x++) {
			const int dx = p_region.position.x + x;
			if (dx >= 0 && dx < resolution) {
				dst[dz * resolution + dx] = src[i] != 0 ? 255 : 0;
			}
			i++;
		}
	}
	hole_map->set_data(resolution, resolution, false, Image::FORMAT_R8, raw);
	emit_changed();
}

Vector3 TerrainData::get_normal(int p_x, int p_z) const {
	const float h_l = get_height(p_x - 1, p_z);
	const float h_r = get_height(p_x + 1, p_z);
	const float h_d = get_height(p_x, p_z - 1);
	const float h_u = get_height(p_x, p_z + 1);
	return Vector3(h_l - h_r, 2.0f * vertex_spacing, h_d - h_u).normalized();
}

float TerrainData::get_height_at_position(const Vector2 &p_local_xz) const {
	const float fx = p_local_xz.x / vertex_spacing;
	const float fz = p_local_xz.y / vertex_spacing;
	const int ix0 = (int)Math::floor(fx);
	const int iz0 = (int)Math::floor(fz);
	const float tx = fx - ix0;
	const float tz = fz - iz0;

	const float h00 = get_height(ix0, iz0);
	const float h10 = get_height(ix0 + 1, iz0);
	const float h01 = get_height(ix0, iz0 + 1);
	const float h11 = get_height(ix0 + 1, iz0 + 1);

	return Math::lerp(Math::lerp(h00, h10, tx), Math::lerp(h01, h11, tx), tz);
}

Vector3 TerrainData::get_normal_at_position(const Vector2 &p_local_xz) const {
	const float fx = p_local_xz.x / vertex_spacing;
	const float fz = p_local_xz.y / vertex_spacing;
	const int ix0 = (int)Math::floor(fx);
	const int iz0 = (int)Math::floor(fz);
	const float tx = fx - ix0;
	const float tz = fz - iz0;

	const Vector3 n00 = get_normal(ix0, iz0);
	const Vector3 n10 = get_normal(ix0 + 1, iz0);
	const Vector3 n01 = get_normal(ix0, iz0 + 1);
	const Vector3 n11 = get_normal(ix0 + 1, iz0 + 1);

	return n00.lerp(n10, tx).lerp(n01.lerp(n11, tx), tz).normalized();
}

void TerrainData::fill_height(float p_height) {
	heightmap->fill(Color(p_height, 0, 0));
	emit_changed();
}

void TerrainData::import_heightmap(const Ref<Image> &p_image, float p_height_min, float p_height_max) {
	ERR_FAIL_COND_MSG(p_image.is_null(), "Cannot import a null heightmap image.");
	ERR_FAIL_COND_MSG(p_image->get_width() != p_image->get_height(), "Heightmap image must be square.");
	ERR_FAIL_COND_MSG(Image::is_format_compressed(p_image->get_format()), "Heightmap image must not use a compressed format.");

	set_resolution(p_image->get_width());

	// Build the whole heightmap in one local buffer and commit it with a
	// single set_height_region() call, instead of one set_height() call per
	// sample: set_height() commits via its own get_data()+set_data() round
	// trip (a full copy-on-write of the whole heightmap) every time it's
	// called, which is fine for a handful of calls but is O(resolution^4)
	// total work across a resolution x resolution loop - unnoticeable at the
	// tiny sizes used while testing this, but enough to make the editor
	// freeze for a very long time (with nothing actually failing, hence no
	// error) on a realistically-sized heightmap image.
	PackedFloat32Array heights;
	heights.resize(resolution * resolution);
	float *dst = heights.ptrw();
	for (int z = 0; z < resolution; z++) {
		for (int x = 0; x < resolution; x++) {
			const float t = p_image->get_pixel(x, z).r;
			dst[z * resolution + x] = Math::lerp(p_height_min, p_height_max, t);
		}
	}
	set_height_region(Rect2i(0, 0, resolution, resolution), heights);
}

void TerrainData::import_layer_mask(const Ref<Image> &p_image, int p_layer_index, MaskChannel p_channel, bool p_normalize) {
	ERR_FAIL_COND_MSG(p_image.is_null(), "Cannot import a null layer mask image.");
	ERR_FAIL_INDEX(p_layer_index, MAX_LAYERS);
	ERR_FAIL_INDEX(p_channel, LAYERS_PER_WEIGHT_MAP);
	ERR_FAIL_COND_MSG(p_image->get_width() != p_image->get_height(), "Layer mask image must be square.");
	ERR_FAIL_COND_MSG(Image::is_format_compressed(p_image->get_format()), "Layer mask image must not use a compressed format.");

	// A mask authored at a different size than this terrain is resampled onto
	// it, rather than resizing the terrain the way import_heightmap() does:
	// the heightmap defines the terrain, but a mask only says where a layer
	// shows on it, so it is the mask that has to give.
	Ref<Image> mask = p_image;
	if (mask->get_width() != resolution) {
		Ref<Image> resized;
		resized.instantiate();
		resized->copy_from(p_image);
		resized->resize(resolution, resolution, Image::INTERPOLATE_BILINEAR);
		mask = resized;
	}

	PackedFloat32Array values;
	values.resize(resolution * resolution);
	float *values_w = values.ptrw();
	for (int z = 0; z < resolution; z++) {
		for (int x = 0; x < resolution; x++) {
			const Color c = mask->get_pixel(x, z);
			float m;
			switch (p_channel) {
				case MASK_CHANNEL_GREEN:
					m = c.g;
					break;
				case MASK_CHANNEL_BLUE:
					m = c.b;
					break;
				case MASK_CHANNEL_ALPHA:
					m = c.a;
					break;
				default:
					m = c.r;
					break;
			}
			values_w[z * resolution + x] = m;
		}
	}

	_apply_layer_mask(values.ptr(), p_layer_index, p_normalize);
}

// Fades to 0 over p_falloff on either side of the p_lo..p_hi band, instead of
// cutting off at its edges - the difference between a usable generated mask and
// one with a visible contour line around it. MIN() of the two edges (rather
// than a product) keeps a band narrower than its own falloff behaving sanely.
static float _mask_band(float p_value, float p_lo, float p_hi, float p_falloff) {
	if (p_falloff <= 0.0f) {
		return (p_value >= p_lo && p_value <= p_hi) ? 1.0f : 0.0f;
	}
	const float rising = Math::smoothstep(p_lo - p_falloff, p_lo, p_value);
	const float falling = 1.0f - Math::smoothstep(p_hi, p_hi + p_falloff, p_value);
	return MIN(rising, falling);
}

void TerrainData::generate_layer_mask(int p_layer_index,
		float p_height_min, float p_height_max, float p_height_falloff,
		float p_slope_min, float p_slope_max, float p_slope_falloff,
		bool p_normalize) {
	ERR_FAIL_INDEX(p_layer_index, MAX_LAYERS);

	PackedFloat32Array values;
	values.resize(resolution * resolution);
	float *values_w = values.ptrw();

	for (int z = 0; z < resolution; z++) {
		for (int x = 0; x < resolution; x++) {
			float m = _mask_band(get_height(x, z), p_height_min, p_height_max, p_height_falloff);
			if (m > 0.0f) {
				// Only worth the normal (four more height reads) once the
				// height band has already let this sample through.
				const Vector3 n = get_normal(x, z);
				const float slope_deg = Math::rad_to_deg(Math::acos(CLAMP(n.y, -1.0f, 1.0f)));
				m *= _mask_band(slope_deg, p_slope_min, p_slope_max, p_slope_falloff);
			}
			values_w[z * resolution + x] = m;
		}
	}

	_apply_layer_mask(values.ptr(), p_layer_index, p_normalize);
}

void TerrainData::_apply_layer_mask(const float *p_mask, int p_layer_index, bool p_normalize) {
	const int target_group = p_layer_index / LAYERS_PER_WEIGHT_MAP;
	const int target_channel = p_layer_index % LAYERS_PER_WEIGHT_MAP;

	// Every layer's weight at a sample sits in one of these maps, and
	// normalizing has to read all of them, so this walks all the raw buffers
	// at once and commits each exactly once at the end - rather than going
	// through set_layer_weight_region() per layer, which copies a whole map
	// per call (see the note above get_height()).
	Vector<uint8_t> raws[WEIGHT_MAP_COUNT];
	uint8_t *dst[WEIGHT_MAP_COUNT];
	for (int g = 0; g < WEIGHT_MAP_COUNT; g++) {
		raws[g] = weight_maps[g]->get_data();
		dst[g] = raws[g].ptrw();
	}

	for (int z = 0; z < resolution; z++) {
		for (int x = 0; x < resolution; x++) {
			const float m = CLAMP(p_mask[z * resolution + x], 0.0f, 1.0f);

			const int i = (z * resolution + x) * LAYERS_PER_WEIGHT_MAP;
			if (p_normalize) {
				// Take what this layer gains out of the others, keeping their
				// proportions to each other - the same bookkeeping
				// Landscape3D::paint_layer() does per brush stamp. Without it,
				// a mask at full white would only tie with whatever is already
				// painted there (a 50/50 blend) instead of replacing it, which
				// is never what a "this is where the road goes" mask means.
				float others_sum = 0.0f;
				for (int g = 0; g < WEIGHT_MAP_COUNT; g++) {
					for (int ch = 0; ch < LAYERS_PER_WEIGHT_MAP; ch++) {
						if (g != target_group || ch != target_channel) {
							others_sum += dst[g][i + ch];
						}
					}
				}
				others_sum /= 255.0f;

				if (others_sum > 0.00001f) {
					const float scale = (1.0f - m) / others_sum;
					for (int g = 0; g < WEIGHT_MAP_COUNT; g++) {
						for (int ch = 0; ch < LAYERS_PER_WEIGHT_MAP; ch++) {
							if (g != target_group || ch != target_channel) {
								dst[g][i + ch] = (uint8_t)CLAMP(Math::round(dst[g][i + ch] * scale), 0.0f, 255.0f);
							}
						}
					}
				} else if (p_layer_index != 0) {
					// This layer held every bit of weight here and the mask now
					// gives some of it back, but no other layer holds a
					// proportion to hand it to. Layer 0 is what a terrain is
					// fully covered in before anything is painted, so it takes
					// the remainder - otherwise the sample would end up holding
					// no weight at all, which renders as black rather than as
					// any layer.
					dst[0][i] = (uint8_t)CLAMP(Math::round((1.0f - m) * 255.0f), 0.0f, 255.0f);
				}
			}
			dst[target_group][i + target_channel] = (uint8_t)CLAMP(Math::round(m * 255.0f), 0.0f, 255.0f);
		}
	}

	for (int g = 0; g < WEIGHT_MAP_COUNT; g++) {
		weight_maps.write[g]->set_data(resolution, resolution, false, Image::FORMAT_RGBA8, raws[g]);
	}
	emit_changed();
}

Ref<Image> TerrainData::get_heightmap_image() const {
	return heightmap;
}

Ref<Image> TerrainData::get_weight_map_image(int p_group) const {
	ERR_FAIL_INDEX_V(p_group, WEIGHT_MAP_COUNT, Ref<Image>());
	return weight_maps[p_group];
}

Ref<Image> TerrainData::get_hole_map_image() const {
	return hole_map;
}

Vector<real_t> TerrainData::get_collision_heights() const {
	Vector<real_t> result;
	result.resize(resolution * resolution);
	real_t *w = result.ptrw();
	int i = 0;
	for (int z = 0; z < resolution; z++) {
		for (int x = 0; x < resolution; x++) {
			w[i++] = get_height(x, z);
		}
	}
	return result;
}

TerrainData::TerrainData() {
	_init_images();
}
