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

void TerrainData::_init_images() {
	heightmap.instantiate();
	heightmap->initialize_data(resolution, resolution, false, Image::FORMAT_RF);
	heightmap->fill(Color(0, 0, 0));

	control_map.instantiate();
	control_map->initialize_data(resolution, resolution, false, Image::FORMAT_RGBA8);
	control_map->fill(Color(0, 0, 0, 0));
}

Dictionary TerrainData::_get_storage_data() const {
	Dictionary d;
	d["resolution"] = resolution;
	d["heightmap"] = heightmap;
	d["control_map"] = control_map;
	return d;
}

void TerrainData::_set_storage_data(const Dictionary &p_data) {
	resolution = CLAMP((int)p_data.get("resolution", resolution), MIN_RESOLUTION, MAX_RESOLUTION);
	Ref<Image> stored_heightmap = p_data.get("heightmap", Ref<Image>());
	Ref<Image> stored_control_map = p_data.get("control_map", Ref<Image>());

	if (stored_heightmap.is_valid() && stored_heightmap->get_width() == resolution && stored_heightmap->get_height() == resolution) {
		heightmap = stored_heightmap;
	} else {
		heightmap.instantiate();
		heightmap->initialize_data(resolution, resolution, false, Image::FORMAT_RF);
		heightmap->fill(Color(0, 0, 0));
	}

	if (stored_control_map.is_valid() && stored_control_map->get_width() == resolution && stored_control_map->get_height() == resolution) {
		control_map = stored_control_map;
	} else {
		control_map.instantiate();
		control_map->initialize_data(resolution, resolution, false, Image::FORMAT_RGBA8);
		control_map->fill(Color(0, 0, 0, 0));
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

	ClassDB::bind_method(D_METHOD("get_control", "x", "z"), &TerrainData::get_control);
	ClassDB::bind_method(D_METHOD("set_control", "x", "z", "control"), &TerrainData::set_control);

	ClassDB::bind_method(D_METHOD("get_control_region", "region"), &TerrainData::get_control_region);
	ClassDB::bind_method(D_METHOD("set_control_region", "region", "control"), &TerrainData::set_control_region);

	ClassDB::bind_method(D_METHOD("is_hole", "x", "z"), &TerrainData::is_hole);

	ClassDB::bind_method(D_METHOD("get_normal", "x", "z"), &TerrainData::get_normal);

	ClassDB::bind_method(D_METHOD("get_height_at_position", "local_xz"), &TerrainData::get_height_at_position);
	ClassDB::bind_method(D_METHOD("get_normal_at_position", "local_xz"), &TerrainData::get_normal_at_position);

	ClassDB::bind_method(D_METHOD("fill_height", "height"), &TerrainData::fill_height);
	ClassDB::bind_method(D_METHOD("import_heightmap", "image", "height_min", "height_max"), &TerrainData::import_heightmap);

	ClassDB::bind_method(D_METHOD("_get_storage_data"), &TerrainData::_get_storage_data);
	ClassDB::bind_method(D_METHOD("_set_storage_data", "data"), &TerrainData::_set_storage_data);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "resolution", PROPERTY_HINT_RANGE, vformat("%d,%d,1", MIN_RESOLUTION, MAX_RESOLUTION)), "set_resolution", "get_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "vertex_spacing", PROPERTY_HINT_RANGE, "0.01,100.0,0.01,or_greater,suffix:m"), "set_vertex_spacing", "get_vertex_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "_storage_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_INTERNAL | PROPERTY_USAGE_STORAGE), "_set_storage_data", "_get_storage_data");
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

float TerrainData::get_height(int p_x, int p_z) const {
	p_x = CLAMP(p_x, 0, resolution - 1);
	p_z = CLAMP(p_z, 0, resolution - 1);
	return heightmap->get_pixel(p_x, p_z).r;
}

void TerrainData::set_height(int p_x, int p_z, float p_height) {
	if (p_x < 0 || p_x >= resolution || p_z < 0 || p_z >= resolution) {
		return;
	}
	heightmap->set_pixel(p_x, p_z, Color(p_height, 0, 0));
}

PackedFloat32Array TerrainData::get_height_region(const Rect2i &p_region) const {
	PackedFloat32Array result;
	const int w = MAX(p_region.size.x, 0);
	const int h = MAX(p_region.size.y, 0);
	result.resize(w * h);
	float *w_ptr = result.ptrw();
	int i = 0;
	for (int z = 0; z < h; z++) {
		for (int x = 0; x < w; x++) {
			w_ptr[i++] = get_height(p_region.position.x + x, p_region.position.y + z);
		}
	}
	return result;
}

void TerrainData::set_height_region(const Rect2i &p_region, const PackedFloat32Array &p_heights) {
	const int w = MAX(p_region.size.x, 0);
	const int h = MAX(p_region.size.y, 0);
	ERR_FAIL_COND(p_heights.size() < w * h);
	const float *r_ptr = p_heights.ptr();
	int i = 0;
	for (int z = 0; z < h; z++) {
		for (int x = 0; x < w; x++) {
			set_height(p_region.position.x + x, p_region.position.y + z, r_ptr[i++]);
		}
	}
	emit_changed();
}

Color TerrainData::get_control(int p_x, int p_z) const {
	p_x = CLAMP(p_x, 0, resolution - 1);
	p_z = CLAMP(p_z, 0, resolution - 1);
	return control_map->get_pixel(p_x, p_z);
}

void TerrainData::set_control(int p_x, int p_z, const Color &p_control) {
	if (p_x < 0 || p_x >= resolution || p_z < 0 || p_z >= resolution) {
		return;
	}
	control_map->set_pixel(p_x, p_z, p_control);
}

PackedColorArray TerrainData::get_control_region(const Rect2i &p_region) const {
	PackedColorArray result;
	const int w = MAX(p_region.size.x, 0);
	const int h = MAX(p_region.size.y, 0);
	result.resize(w * h);
	Color *w_ptr = result.ptrw();
	int i = 0;
	for (int z = 0; z < h; z++) {
		for (int x = 0; x < w; x++) {
			w_ptr[i++] = get_control(p_region.position.x + x, p_region.position.y + z);
		}
	}
	return result;
}

void TerrainData::set_control_region(const Rect2i &p_region, const PackedColorArray &p_control) {
	const int w = MAX(p_region.size.x, 0);
	const int h = MAX(p_region.size.y, 0);
	ERR_FAIL_COND(p_control.size() < w * h);
	const Color *r_ptr = p_control.ptr();
	int i = 0;
	for (int z = 0; z < h; z++) {
		for (int x = 0; x < w; x++) {
			set_control(p_region.position.x + x, p_region.position.y + z, r_ptr[i++]);
		}
	}
	emit_changed();
}

bool TerrainData::is_hole(int p_x, int p_z) const {
	return get_control(p_x, p_z).a > 0.5f;
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

	for (int z = 0; z < resolution; z++) {
		for (int x = 0; x < resolution; x++) {
			const float t = p_image->get_pixel(x, z).r;
			set_height(x, z, Math::lerp(p_height_min, p_height_max, t));
		}
	}
	emit_changed();
}

Ref<Image> TerrainData::get_heightmap_image() const {
	return heightmap;
}

Ref<Image> TerrainData::get_control_map_image() const {
	return control_map;
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
