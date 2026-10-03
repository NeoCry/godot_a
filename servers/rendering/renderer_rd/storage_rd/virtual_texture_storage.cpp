/**************************************************************************/
/*  virtual_texture_storage.cpp                                           */
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

#include "virtual_texture_storage.h"

#include "core/config/project_settings.h"
#include "core/object/callable_mp.h"
#include "core/templates/sort_array.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

VirtualTextureStorage *VirtualTextureStorage::singleton = nullptr;

namespace {

// A tile sampled within this many frames is never evicted to make room: what the feedback buffer
// reports arrives a few frames late, so a page that has just stopped being reported may well still
// be on screen.
constexpr uint64_t TILE_PROTECTED_FRAMES = 4;
// A page asked of a streamed texture's provider that has not been handed over after this many frames
// is asked for again, whenever it is wanted.
constexpr uint64_t REQUEST_TIMEOUT_FRAMES = 600;
constexpr uint32_t MAX_FEEDBACK_READS_IN_FLIGHT = 3;
// Pages drawn again because something changed under them come before missing ones: they are on
// screen, and wrong.
constexpr int STALE_PAGE_SCORE = 1000;
constexpr int COARSEST_PAGE_SCORE = 2000;

uint8_t to_unorm8(float p_value) {
	return uint8_t(CLAMP(Math::round(p_value * 255.0f), 0.0f, 255.0f));
}

} // namespace

uint32_t VirtualTextureStorage::_encode_entry(Cache p_cache, uint32_t p_tile, uint32_t p_mip, uint32_t p_id) const {
	const uint32_t tiles_per_side = cache_sizes[p_cache];
	const uint32_t x = p_tile % tiles_per_side;
	const uint32_t y = p_tile / tiles_per_side;
	return x | (y << 8) | (MIN(p_mip, ENTRY_EMPTY_MIP) << 16) | (uint32_t(p_cache) << 20) | (p_id << 21);
}

uint32_t VirtualTextureStorage::_empty_entry(const VirtualTexture *p_texture) const {
	const uint32_t loading_tile = p_texture->cache == CACHE_STREAMED ? uint32_t(STREAMED_TILE_LOADING) : 0u;
	return _encode_entry(p_texture->cache, loading_tile, ENTRY_EMPTY_MIP, p_texture->id);
}

/* CACHES */

bool VirtualTextureStorage::_ensure_cache(Cache p_cache) {
	CachePool &pool = caches[p_cache];
	if (pool.texture.is_valid()) {
		return true;
	}
	if (!enabled) {
		return false;
	}

	RD *rd = RD::get_singleton();

	if (downsample_shader_version.is_null()) {
		Vector<String> modes;
		modes.push_back("\n");
		downsample_shader.initialize(modes);
		downsample_shader_version = downsample_shader.version_create();
		downsample_pipeline = rd->compute_pipeline_create(downsample_shader.version_get_shader(downsample_shader_version, 0));
	}

	pool.layers = p_cache == CACHE_RUNTIME ? RUNTIME_LAYERS : 1;
	pool.tiles_per_side = cache_sizes[p_cache];

	RD::TextureFormat tf;
	tf.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	tf.width = pool.tiles_per_side * TILE_SIZE;
	tf.height = tf.width;
	tf.array_layers = pool.layers;
	tf.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
	tf.mipmaps = TILE_MIPMAPS;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
	tf.shareable_formats.push_back(RD::DATA_FORMAT_R8G8B8A8_UNORM);
	tf.shareable_formats.push_back(RD::DATA_FORMAT_R8G8B8A8_SRGB);
	pool.texture = rd->texture_create(tf, RD::TextureView());
	ERR_FAIL_COND_V_MSG(pool.texture.is_null(), false, "Could not create the virtual texture page cache.");
	rd->set_resource_name(pool.texture, p_cache == CACHE_RUNTIME ? "Runtime virtual texture cache" : "Streamed virtual texture cache");

	RD::TextureView srgb_view;
	srgb_view.format_override = RD::DATA_FORMAT_R8G8B8A8_SRGB;
	pool.texture_srgb = rd->texture_create_shared(srgb_view, pool.texture);
	for (int i = 0; i < TILE_MIPMAPS; i++) {
		pool.storage_mips[i] = rd->texture_create_shared_from_slice(RD::TextureView(), pool.texture, 0, i, 1, RD::TEXTURE_SLICE_2D_ARRAY, pool.layers);
	}

	const uint32_t tile_count = pool.tiles_per_side * pool.tiles_per_side;
	pool.tiles.resize(tile_count);
	pool.reserved_tiles = p_cache == CACHE_STREAMED ? uint32_t(STREAMED_TILE_RESERVED) : 1u;
	pool.loading_tile = p_cache == CACHE_STREAMED ? uint32_t(STREAMED_TILE_LOADING) : 0u;
	pool.free_tiles.clear();
	for (uint32_t i = tile_count; i > pool.reserved_tiles; i--) {
		pool.free_tiles.push_back(i - 1);
	}
	for (uint32_t i = 0; i < pool.reserved_tiles; i++) {
		pool.tiles[i].pinned = true;
	}

	if (p_cache == CACHE_STREAMED) {
		const Color white[1] = { Color(1, 1, 1, 1) };
		const Color black[1] = { Color(0, 0, 0, 1) };
		const Color normal[1] = { Color(0.5, 0.5, 1, 1) };
		const Color transparent[1] = { Color(0, 0, 0, 0) };
		const Color loading[1] = { Color(0.5, 0.5, 0.5, 1) };
		_fill_tile(p_cache, STREAMED_TILE_WHITE, white);
		_fill_tile(p_cache, STREAMED_TILE_BLACK, black);
		_fill_tile(p_cache, STREAMED_TILE_NORMAL, normal);
		_fill_tile(p_cache, STREAMED_TILE_TRANSPARENT, transparent);
		_fill_tile(p_cache, STREAMED_TILE_LOADING, loading);
	} else {
		// What a page that is not there yet reads as: mid gray (sRGB encoded, as the albedo layer is
		// read back through the sRGB view), a normal straight up the volume with default specular, and
		// no occlusion, full roughness, no metal.
		const Color loading[RUNTIME_LAYERS] = { Color(0.73, 0.73, 0.73, 1), Color(0.5, 0.5, 1, 0.5), Color(1, 1, 0, 0) };
		_fill_tile(p_cache, 0, loading);
	}

	bindings_version++;
	return true;
}

void VirtualTextureStorage::_free_cache(Cache p_cache) {
	CachePool &pool = caches[p_cache];
	if (pool.texture.is_null()) {
		return;
	}
	RD *rd = RD::get_singleton();
	for (int i = 0; i < TILE_MIPMAPS; i++) {
		if (pool.storage_mips[i].is_valid()) {
			rd->free_rid(pool.storage_mips[i]);
			pool.storage_mips[i] = RID();
		}
	}
	if (pool.texture_srgb.is_valid()) {
		rd->free_rid(pool.texture_srgb);
		pool.texture_srgb = RID();
	}
	rd->free_rid(pool.texture);
	pool.texture = RID();
	pool.tiles.clear();
	pool.free_tiles.clear();
	bindings_version++;
}

void VirtualTextureStorage::_upload_tile(Cache p_cache, uint32_t p_tile, int p_layer, const Vector<uint8_t> &p_data) {
	_fill_tile_level(p_cache, p_tile, p_layer, 0, p_data);
}

void VirtualTextureStorage::_fill_tile_level(Cache p_cache, uint32_t p_tile, int p_layer, int p_mipmap, const Vector<uint8_t> &p_data) {
	CachePool &pool = caches[p_cache];
	RD *rd = RD::get_singleton();
	const int size = TILE_SIZE >> p_mipmap;

	// RenderingDevice updates whole layers only, so a tile is staged in a texture of its own and
	// copied into place.
	RD::TextureFormat tf;
	tf.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	tf.width = size;
	tf.height = size;
	tf.usage_bits = RD::TEXTURE_USAGE_CAN_UPDATE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	Vector<Vector<uint8_t>> data;
	data.push_back(p_data);
	RID staging = rd->texture_create(tf, RD::TextureView(), data);
	ERR_FAIL_COND(staging.is_null());

	const uint32_t x = (p_tile % pool.tiles_per_side) * size;
	const uint32_t y = (p_tile / pool.tiles_per_side) * size;
	rd->texture_copy(staging, pool.texture, Vector3(), Vector3(x, y, 0), Vector3(size, size, 1), 0, p_mipmap, 0, p_layer);
	rd->free_rid(staging);
}

void VirtualTextureStorage::_fill_tile(Cache p_cache, uint32_t p_tile, const Color *p_layer_colors) {
	CachePool &pool = caches[p_cache];
	for (int layer = 0; layer < pool.layers; layer++) {
		const Color &c = p_layer_colors[layer];
		const uint8_t texel[4] = { to_unorm8(c.r), to_unorm8(c.g), to_unorm8(c.b), to_unorm8(c.a) };
		for (int mip = 0; mip < TILE_MIPMAPS; mip++) {
			const int size = TILE_SIZE >> mip;
			Vector<uint8_t> data;
			data.resize(size * size * 4);
			uint8_t *w = data.ptrw();
			for (int i = 0; i < size * size; i++) {
				memcpy(w + i * 4, texel, 4);
			}
			_fill_tile_level(p_cache, p_tile, layer, mip, data);
		}
	}
}

uint32_t VirtualTextureStorage::_allocate_tile(Cache p_cache) {
	CachePool &pool = caches[p_cache];
	if (pool.texture.is_null()) {
		return UINT32_MAX;
	}
	if (!pool.free_tiles.is_empty()) {
		const uint32_t tile = pool.free_tiles[pool.free_tiles.size() - 1];
		pool.free_tiles.resize(pool.free_tiles.size() - 1);
		return tile;
	}

	// Evict the page nothing has sampled for the longest, unless everything in the cache is on
	// screen: then the cache is too small for the view, and evicting would only thrash it.
	const uint64_t protect_from = frame > TILE_PROTECTED_FRAMES ? frame - TILE_PROTECTED_FRAMES : 0;
	uint32_t best = UINT32_MAX;
	uint64_t best_used = UINT64_MAX;
	for (uint32_t i = pool.reserved_tiles; i < pool.tiles.size(); i++) {
		const Tile &tile = pool.tiles[i];
		if (tile.pinned || tile.last_used >= protect_from) {
			continue;
		}
		if (tile.last_used < best_used) {
			best_used = tile.last_used;
			best = i;
		}
	}
	if (best == UINT32_MAX) {
		return UINT32_MAX;
	}

	Tile &tile = pool.tiles[best];
	if (tile.id != 0 && ids[tile.id]) {
		_unmap_page(ids[tile.id], tile.page);
	}
	tile = Tile();
	return best;
}

void VirtualTextureStorage::_release_tile(Cache p_cache, uint32_t p_tile) {
	CachePool &pool = caches[p_cache];
	if (p_tile >= pool.tiles.size() || p_tile < pool.reserved_tiles) {
		return;
	}
	pool.tiles[p_tile] = Tile();
	pool.free_tiles.push_back(p_tile);
}

/* PAGE TABLES */

void VirtualTextureStorage::_set_entries(VirtualTexture *p_texture, uint64_t p_page, uint32_t p_entry, bool p_mapping) {
	const uint32_t mip = _page_mip(p_page);
	const uint32_t px = _page_x(p_page);
	const uint32_t py = _page_y(p_page);

	// Mapping a page takes over every entry under it that names a coarser page (or the same one: it
	// may be moving to another tile); unmapping hands the ones that named it to p_entry. Entries that
	// name a finer page keep it either way.
	for (int level = int(mip); level >= 0; level--) {
		const uint32_t shift = mip - uint32_t(level);
		const uint32_t count = 1u << shift;
		const uint32_t x0 = px << shift;
		const uint32_t y0 = py << shift;
		const uint32_t width = p_texture->width >> level;
		LocalVector<uint32_t> &entries = p_texture->entries[level];
		for (uint32_t y = y0; y < y0 + count; y++) {
			uint32_t *row = entries.ptr() + y * width;
			for (uint32_t x = x0; x < x0 + count; x++) {
				const uint32_t entry_mip = (row[x] >> 16) & 0xF;
				if (p_mapping ? entry_mip >= mip : entry_mip == mip) {
					row[x] = p_entry;
				}
			}
		}
		const Rect2i rect(x0, y0, count, count);
		Rect2i &dirty = p_texture->dirty[level];
		dirty = dirty.has_area() ? dirty.merge(rect) : rect;
	}
}

void VirtualTextureStorage::_map_page(VirtualTexture *p_texture, uint64_t p_page, uint32_t p_tile) {
	p_texture->resident[p_page] = p_tile;
	Tile &tile = caches[p_texture->cache].tiles[p_tile];
	tile.id = p_texture->id;
	tile.page = p_page;
	tile.last_used = frame;
	// The coarsest pages are what everything else falls back on: they never leave.
	tile.pinned = _page_mip(p_page) + 1 >= p_texture->mipmaps;
	_set_entries(p_texture, p_page, _encode_entry(p_texture->cache, p_tile, _page_mip(p_page), p_texture->id), true);
}

void VirtualTextureStorage::_unmap_page(VirtualTexture *p_texture, uint64_t p_page) {
	if (!p_texture->resident.erase(p_page)) {
		return;
	}
	p_texture->stale.erase(p_page);

	uint32_t entry = _empty_entry(p_texture);
	uint64_t ancestor = p_page;
	while (_page_mip(ancestor) + 1 < p_texture->mipmaps) {
		ancestor = _page_parent(ancestor);
		const uint32_t *tile = p_texture->resident.getptr(ancestor);
		if (tile) {
			entry = _encode_entry(p_texture->cache, *tile, _page_mip(ancestor), p_texture->id);
			break;
		}
	}
	_set_entries(p_texture, p_page, entry, false);
}

void VirtualTextureStorage::_touch_page(VirtualTexture *p_texture, uint64_t p_page) {
	CachePool &pool = caches[p_texture->cache];
	uint64_t page = p_page;
	while (true) {
		const uint32_t *tile = p_texture->resident.getptr(page);
		if (tile) {
			pool.tiles[*tile].last_used = frame;
		}
		if (_page_mip(page) + 1 >= p_texture->mipmaps) {
			break;
		}
		page = _page_parent(page);
	}
}

uint32_t VirtualTextureStorage::_resident_level(const VirtualTexture *p_texture, uint64_t p_page) const {
	uint64_t page = p_page;
	while (true) {
		if (p_texture->resident.has(page)) {
			return _page_mip(page);
		}
		if (_page_mip(page) + 1 >= p_texture->mipmaps) {
			return ENTRY_EMPTY_MIP;
		}
		page = _page_parent(page);
	}
}

void VirtualTextureStorage::_mark_stale(VirtualTexture *p_texture, const Rect2 &p_uv_rect) {
	if (p_texture->type != RSE::VIRTUAL_TEXTURE_RUNTIME) {
		return;
	}
	const float border = float(PAGE_BORDER) / float(PAGE_SIZE);
	for (const KeyValue<uint64_t, uint32_t> &E : p_texture->resident) {
		const uint32_t mip = _page_mip(E.key);
		const float pages_x = float(p_texture->width >> mip);
		const float pages_y = float(p_texture->height >> mip);
		// Borders included: they show the neighbors' texels.
		const Rect2 page_rect(Point2((_page_x(E.key) - border) / pages_x, (_page_y(E.key) - border) / pages_y),
				Size2((1.0f + border * 2.0f) / pages_x, (1.0f + border * 2.0f) / pages_y));
		if (page_rect.intersects(p_uv_rect, true)) {
			p_texture->stale.insert(E.key);
		}
	}
}

void VirtualTextureStorage::_queue_coarsest(VirtualTexture *p_texture) {
	if (p_texture->coarsest_queued) {
		return;
	}
	// The coarsest pages are what everything falls back on, so they come before anything the feedback
	// asks for, and are asked for whether or not anything has sampled the texture yet.
	const uint32_t mip = p_texture->mipmaps - 1;
	const uint32_t width = p_texture->width >> mip;
	const uint32_t height = p_texture->height >> mip;
	bool all_resident = true;
	for (uint32_t y = 0; y < height; y++) {
		for (uint32_t x = 0; x < width; x++) {
			const uint64_t page = _make_page(mip, x, y);
			if (p_texture->resident.has(page)) {
				continue;
			}
			all_resident = false;
			if (p_texture->type == RSE::VIRTUAL_TEXTURE_STREAMED) {
				if (!p_texture->requested.has(page)) {
					Candidate c;
					c.texture = p_texture->self;
					c.page = page;
					c.score = COARSEST_PAGE_SCORE;
					streamed_queue.insert(0, c);
				}
			} else if (p_texture->queue.find(page) < 0) {
				p_texture->queue.insert(0, page);
			}
		}
	}
	p_texture->coarsest_queued = all_resident;
}

/* FEEDBACK */

bool VirtualTextureStorage::_decode_feedback_key(uint32_t p_key, VirtualTexture *&r_texture, uint64_t &r_page) const {
	const uint32_t id = p_key >> 21;
	if (id == 0 || id >= MAX_IDS || !ids[id]) {
		return false;
	}
	VirtualTexture *texture = ids[id];
	const uint32_t index = p_key & 0x1FFFFF;
	uint32_t offset = 0;
	for (uint32_t mip = 0; mip < texture->mipmaps; mip++) {
		const uint32_t width = texture->width >> mip;
		const uint32_t count = width * (texture->height >> mip);
		if (index < offset + count) {
			const uint32_t local = index - offset;
			r_texture = texture;
			r_page = _make_page(mip, local % width, local / width);
			return true;
		}
		offset += count;
	}
	return false;
}

void VirtualTextureStorage::_feedback_received(const Vector<uint8_t> &p_data) {
	if (!singleton) {
		return;
	}
	if (singleton->feedback_reads_in_flight > 0) {
		singleton->feedback_reads_in_flight--;
	}
	singleton->feedback_data = p_data;
	singleton->feedback_data_ready = true;
}

void VirtualTextureStorage::_read_feedback() {
	RD *rd = RD::get_singleton();
	const uint32_t size = FEEDBACK_HEADER_SIZE + FEEDBACK_SLOTS * sizeof(uint32_t);
	// Whatever the frames since the last read wrote is read back, then the buffer starts over for
	// this one. A frame whose read was skipped is lost, which only delays its pages: whatever is
	// still on screen asks for them again.
	if (feedback_reads_in_flight < MAX_FEEDBACK_READS_IN_FLIGHT) {
		feedback_reads_in_flight++;
		rd->buffer_get_data_async(feedback_buffer, callable_mp_static(&VirtualTextureStorage::_feedback_received), 0, size);
	}
	rd->buffer_clear(feedback_buffer, 0, size);
	const uint32_t header[4] = { uint32_t(frame), 0, 0, 0 };
	rd->buffer_update(feedback_buffer, 0, sizeof(header), header);
}

void VirtualTextureStorage::_process_feedback() {
	if (!feedback_data_ready) {
		return;
	}
	feedback_data_ready = false;

	const uint32_t slot_count = MIN(FEEDBACK_SLOTS, uint32_t(MAX(0, feedback_data.size() - int(FEEDBACK_HEADER_SIZE))) / uint32_t(sizeof(uint32_t)));
	const uint32_t *slots = reinterpret_cast<const uint32_t *>(feedback_data.ptr() + FEEDBACK_HEADER_SIZE);

	LocalVector<Candidate> candidates;
	HashSet<uint64_t> seen;
	for (uint32_t i = 0; i < slot_count; i++) {
		const uint32_t key = slots[i];
		if (key == 0) {
			continue;
		}
		VirtualTexture *texture = nullptr;
		uint64_t page = 0;
		if (!_decode_feedback_key(key, texture, page)) {
			continue;
		}
		_touch_page(texture, page);

		const uint32_t mip = _page_mip(page);
		const uint32_t resident_level = _resident_level(texture, page);
		const uint64_t texture_bits = uint64_t(texture->id) << 44;

		if (resident_level != ENTRY_EMPTY_MIP && texture->type == RSE::VIRTUAL_TEXTURE_RUNTIME) {
			// Whatever stands in for the page on screen is drawn again if something changed under it.
			uint64_t shown = page;
			while (_page_mip(shown) < resident_level) {
				shown = _page_parent(shown);
			}
			if (texture->stale.has(shown) && !seen.has(texture_bits | shown)) {
				seen.insert(texture_bits | shown);
				Candidate c;
				c.texture = texture->self;
				c.page = shown;
				c.score = STALE_PAGE_SCORE + int(resident_level);
				candidates.push_back(c);
			}
		}

		// Every page missing between the one wanted and what stands in for it, coarse ones first: they
		// improve the most of the screen, and are what the finer ones fall back on meanwhile.
		uint64_t missing = page;
		const uint32_t last = MIN(resident_level, texture->mipmaps);
		for (uint32_t level = mip; level < last; level++) {
			if (!seen.has(texture_bits | missing)) {
				seen.insert(texture_bits | missing);
				Candidate c;
				c.texture = texture->self;
				c.page = missing;
				c.score = int(level);
				candidates.push_back(c);
			}
			missing = _page_parent(missing);
		}
	}

	candidates.sort();

	streamed_queue.clear();
	for (VirtualTexture *texture : textures) {
		if (texture->type == RSE::VIRTUAL_TEXTURE_RUNTIME) {
			texture->queue.clear();
		}
		texture->coarsest_queued = false;
	}
	for (const Candidate &c : candidates) {
		VirtualTexture *texture = texture_owner.get_or_null(c.texture);
		if (!texture) {
			continue;
		}
		if (texture->type == RSE::VIRTUAL_TEXTURE_STREAMED) {
			streamed_queue.push_back(c);
		} else {
			texture->queue.push_back(c.page);
		}
	}
}

/* STREAMED PAGES */

void VirtualTextureStorage::_request_streamed_pages() {
	// Requests whose page never came back are forgotten, so that they can be made again.
	for (VirtualTexture *texture : textures) {
		if (texture->requested.is_empty()) {
			continue;
		}
		LocalVector<uint64_t> expired;
		for (const KeyValue<uint64_t, uint64_t> &E : texture->requested) {
			if (E.value + REQUEST_TIMEOUT_FRAMES < frame) {
				expired.push_back(E.key);
			}
		}
		for (const uint64_t page : expired) {
			texture->requested.erase(page);
			requests_in_flight--;
		}
	}

	HashMap<RID, PackedInt32Array> batches;
	uint32_t consumed = 0;
	for (; consumed < streamed_queue.size(); consumed++) {
		if (requests_in_flight >= max_requests_in_flight) {
			break;
		}
		const Candidate &c = streamed_queue[consumed];
		VirtualTexture *texture = texture_owner.get_or_null(c.texture);
		if (!texture || !texture->request_callback.is_valid()) {
			continue;
		}
		if (texture->resident.has(c.page) || texture->requested.has(c.page)) {
			continue;
		}
		texture->requested[c.page] = frame;
		requests_in_flight++;
		PackedInt32Array &batch = batches[c.texture];
		batch.push_back(int(_page_mip(c.page)));
		batch.push_back(int(_page_x(c.page)));
		batch.push_back(int(_page_y(c.page)));
	}
	if (consumed >= streamed_queue.size()) {
		streamed_queue.clear();
	} else if (consumed > 0) {
		LocalVector<Candidate> remaining;
		remaining.reserve(streamed_queue.size() - consumed);
		for (uint32_t i = consumed; i < streamed_queue.size(); i++) {
			remaining.push_back(streamed_queue[i]);
		}
		streamed_queue = remaining;
	}

	for (const KeyValue<RID, PackedInt32Array> &E : batches) {
		VirtualTexture *texture = texture_owner.get_or_null(E.key);
		if (texture) {
			texture->request_callback.call(E.value);
		}
	}
}

void VirtualTextureStorage::_process_uploads() {
	uint32_t processed = 0;
	uint32_t i = 0;
	for (; i < uploads.size() && processed < max_uploads_per_frame; i++) {
		Upload &upload = uploads[i];
		VirtualTexture *texture = texture_owner.get_or_null(upload.texture);
		if (!texture) {
			continue;
		}
		const uint32_t *resident_tile = texture->resident.getptr(upload.page);
		uint32_t tile = resident_tile ? *resident_tile : _allocate_tile(texture->cache);
		if (tile == UINT32_MAX) {
			// Everything in the cache is on screen. The page is asked for again if it is still wanted
			// once something has left it.
			continue;
		}

		_upload_tile(texture->cache, tile, 0, upload.image->get_data());
		DownsampleTile ds;
		ds.cache = texture->cache;
		ds.tile = tile;
		downsample_tiles.push_back(ds);
		if (resident_tile) {
			caches[texture->cache].tiles[tile].last_used = frame;
		} else {
			_map_page(texture, upload.page, tile);
		}
		processed++;
	}

	if (i >= uploads.size()) {
		uploads.clear();
	} else if (i > 0) {
		LocalVector<Upload> remaining;
		remaining.reserve(uploads.size() - i);
		for (uint32_t j = i; j < uploads.size(); j++) {
			remaining.push_back(uploads[j]);
		}
		uploads = remaining;
	}
}

/* GPU WORK */

void VirtualTextureStorage::_downsample_tiles() {
	if (downsample_tiles.is_empty()) {
		return;
	}
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	RID shader = downsample_shader.version_get_shader(downsample_shader_version, 0);
	ERR_FAIL_COND(shader.is_null());

	for (int level = 1; level < TILE_MIPMAPS; level++) {
		const int size = TILE_SIZE >> level;
		RD::ComputeListID compute_list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(compute_list, downsample_pipeline);
		int bound_cache = -1;
		for (const DownsampleTile &ds : downsample_tiles) {
			CachePool &pool = caches[ds.cache];
			if (pool.texture.is_null()) {
				continue;
			}
			if (bound_cache != int(ds.cache)) {
				RD::Uniform u_source(RD::UNIFORM_TYPE_IMAGE, 0, pool.storage_mips[level - 1]);
				RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 1, pool.storage_mips[level]);
				rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 0, u_source, u_dest), 0);
				bound_cache = int(ds.cache);
			}
			DownsamplePushConstant push_constant;
			push_constant.dest_origin[0] = int32_t((ds.tile % pool.tiles_per_side) * size);
			push_constant.dest_origin[1] = int32_t((ds.tile / pool.tiles_per_side) * size);
			push_constant.dest_size = size;
			push_constant.layers = pool.layers;
			rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(DownsamplePushConstant));
			rd->compute_list_dispatch_threads(compute_list, size, size, 1);
		}
		rd->compute_list_end();
	}
	downsample_tiles.clear();
}

void VirtualTextureStorage::_upload_page_tables() {
	RD *rd = RD::get_singleton();
	for (VirtualTexture *texture : textures) {
		for (uint32_t level = 0; level < texture->mipmaps; level++) {
			Rect2i &dirty = texture->dirty[level];
			if (!dirty.has_area()) {
				continue;
			}
			const uint32_t width = texture->width >> level;
			Vector<uint8_t> data;
			data.resize(dirty.size.x * dirty.size.y * sizeof(uint32_t));
			uint32_t *w = reinterpret_cast<uint32_t *>(data.ptrw());
			for (int y = 0; y < dirty.size.y; y++) {
				memcpy(w + y * dirty.size.x, texture->entries[level].ptr() + (dirty.position.y + y) * width + dirty.position.x, dirty.size.x * sizeof(uint32_t));
			}

			RD::TextureFormat tf;
			tf.format = RD::DATA_FORMAT_R32_UINT;
			tf.width = dirty.size.x;
			tf.height = dirty.size.y;
			tf.usage_bits = RD::TEXTURE_USAGE_CAN_UPDATE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
			Vector<Vector<uint8_t>> staging_data;
			staging_data.push_back(data);
			RID staging = rd->texture_create(tf, RD::TextureView(), staging_data);
			if (staging.is_valid()) {
				rd->texture_copy(staging, texture->page_table, Vector3(), Vector3(dirty.position.x, dirty.position.y, 0), Vector3(dirty.size.x, dirty.size.y, 1), 0, level, 0, 0);
				rd->free_rid(staging);
			}
			dirty = Rect2i();
		}
	}
}

/* RUNTIME PAGES */

bool VirtualTextureStorage::_ensure_runtime_target() {
	if (runtime_target.framebuffer.is_valid() && RD::get_singleton()->framebuffer_is_valid(runtime_target.framebuffer)) {
		return true;
	}
	_free_runtime_target();

	RD *rd = RD::get_singleton();
	runtime_target.grid = MAX(1, int(Math::ceil(Math::sqrt(float(max_runtime_pages_per_frame)))));
	const uint32_t size = runtime_target.grid * TILE_SIZE;

	RD::TextureFormat tf;
	tf.width = size;
	tf.height = size;

	tf.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	tf.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
	tf.shareable_formats.push_back(RD::DATA_FORMAT_R8G8B8A8_UNORM);
	tf.shareable_formats.push_back(RD::DATA_FORMAT_R8G8B8A8_SRGB);
	runtime_target.albedo = rd->texture_create(tf, RD::TextureView());
	RD::TextureView srgb_view;
	srgb_view.format_override = RD::DATA_FORMAT_R8G8B8A8_SRGB;
	runtime_target.albedo_srgb = rd->texture_create_shared(srgb_view, runtime_target.albedo);
	tf.shareable_formats.clear();

	runtime_target.normal = rd->texture_create(tf, RD::TextureView());
	runtime_target.orm = rd->texture_create(tf, RD::TextureView());

	tf.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT;
	runtime_target.emission = rd->texture_create(tf, RD::TextureView());
	tf.format = RD::DATA_FORMAT_R16_SFLOAT;
	runtime_target.depth_output = rd->texture_create(tf, RD::TextureView());

	tf.format = rd->texture_is_format_supported_for_usage(RD::DATA_FORMAT_D32_SFLOAT, RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ? RD::DATA_FORMAT_D32_SFLOAT : RD::DATA_FORMAT_X8_D24_UNORM_PACK32;
	tf.usage_bits = RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	runtime_target.depth = rd->texture_create(tf, RD::TextureView());

	ERR_FAIL_COND_V(runtime_target.albedo_srgb.is_null() || runtime_target.normal.is_null() || runtime_target.orm.is_null() || runtime_target.emission.is_null() || runtime_target.depth_output.is_null() || runtime_target.depth.is_null(), false);

	Vector<RID> attachments;
	attachments.push_back(runtime_target.albedo_srgb);
	attachments.push_back(runtime_target.normal);
	attachments.push_back(runtime_target.orm);
	attachments.push_back(runtime_target.emission);
	attachments.push_back(runtime_target.depth_output);
	attachments.push_back(runtime_target.depth);
	runtime_target.framebuffer = rd->framebuffer_create(attachments);
	return runtime_target.framebuffer.is_valid();
}

void VirtualTextureStorage::_free_runtime_target() {
	RD *rd = RD::get_singleton();
	// The framebuffer goes with its attachments.
	RID *rids[] = { &runtime_target.albedo_srgb, &runtime_target.albedo, &runtime_target.normal, &runtime_target.orm, &runtime_target.emission, &runtime_target.depth_output, &runtime_target.depth };
	for (RID *rid : rids) {
		if (rid->is_valid()) {
			rd->free_rid(*rid);
			*rid = RID();
		}
	}
	runtime_target.framebuffer = RID();
}

void VirtualTextureStorage::_page_camera(const VirtualTexture *p_texture, uint64_t p_page, RendererTextureStorage::VirtualTextureRenderPage &r_page) const {
	const uint32_t mip = _page_mip(p_page);
	const float pages_x = float(p_texture->width >> mip);
	const float pages_y = float(p_texture->height >> mip);
	const float border = float(PAGE_BORDER) / float(PAGE_SIZE);
	const float u0 = (float(_page_x(p_page)) - border) / pages_x;
	const float u1 = (float(_page_x(p_page)) + 1.0f + border) / pages_x;
	const float v0 = (float(_page_y(p_page)) - border) / pages_y;
	const float v1 = (float(_page_y(p_page)) + 1.0f + border) / pages_y;

	// The volume maps a unit cube to the world: X runs along U, Z along V, and Y from the bottom of what
	// is drawn to the top. Pages are drawn looking straight down its Y axis, with U to the right and
	// V down the image, as a texture's rows run.
	const Transform3D &volume = p_texture->volume;
	const Vector3 axis_x = volume.basis.get_column(0);
	const Vector3 axis_y = volume.basis.get_column(1);
	const Vector3 axis_z = volume.basis.get_column(2);
	const real_t length_x = MAX(axis_x.length(), (real_t)CMP_EPSILON);
	const real_t length_y = MAX(axis_y.length(), (real_t)CMP_EPSILON);
	const real_t length_z = MAX(axis_z.length(), (real_t)CMP_EPSILON);
	const Vector3 cam_x = axis_x / length_x;
	const Vector3 cam_z = axis_y / length_y;
	const Vector3 cam_y = cam_z.cross(cam_x).normalized();

	const real_t margin = MAX(length_y * 0.01, 0.01);
	const Vector3 top = volume.xform(Vector3((u0 + u1) * 0.5f, 1.0f, (v0 + v1) * 0.5f)) + cam_z * margin;
	r_page.cam_transform = Transform3D(Basis(cam_x, cam_y, cam_z), top);

	const real_t half_width = (u1 - u0) * 0.5f * length_x;
	const real_t half_height = (v1 - v0) * 0.5f * length_z;
	r_page.cam_projection.set_orthogonal(-half_width, half_width, -half_height, half_height, margin * 0.5, length_y + margin * 2.0);

	AABB aabb;
	for (int i = 0; i < 8; i++) {
		const Vector3 corner = volume.xform(Vector3((i & 1) ? u1 : u0, (i & 2) ? 1.0f : 0.0f, (i & 4) ? v1 : v0));
		if (i == 0) {
			aabb.position = corner;
		} else {
			aabb.expand_to(corner);
		}
	}
	r_page.world_aabb = aabb;
}

/* API */

RD::PipelineColorBlendState VirtualTextureStorage::get_material_pass_blend_state(bool p_uses_alpha) {
	RD::PipelineColorBlendState state = RD::PipelineColorBlendState::create_disabled(5);
	if (!p_uses_alpha) {
		return state;
	}
	for (int i = 0; i < RUNTIME_LAYERS; i++) {
		RD::PipelineColorBlendState::Attachment &attachment = state.attachments.write[i];
		attachment.enable_blend = true;
		attachment.src_color_blend_factor = RD::BLEND_FACTOR_SRC_ALPHA;
		attachment.dst_color_blend_factor = RD::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		if (i == 0) {
			attachment.src_alpha_blend_factor = RD::BLEND_FACTOR_ONE;
			attachment.dst_alpha_blend_factor = RD::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		} else {
			attachment.src_alpha_blend_factor = RD::BLEND_FACTOR_ZERO;
			attachment.dst_alpha_blend_factor = RD::BLEND_FACTOR_ONE;
		}
	}
	return state;
}

RID VirtualTextureStorage::virtual_texture_create(RID p_texture, int p_width, int p_height, RSE::VirtualTextureType p_type) {
	if (!enabled) {
		return RID();
	}
	ERR_FAIL_COND_V_MSG(p_width < PAGE_SIZE || p_height < PAGE_SIZE || !Math::is_power_of_2(uint32_t(p_width)) || !Math::is_power_of_2(uint32_t(p_height)), RID(),
			vformat("A virtual texture's size must be a power of two no smaller than %d, not %dx%d.", PAGE_SIZE, p_width, p_height));
	const uint32_t width = uint32_t(p_width) / PAGE_SIZE;
	const uint32_t height = uint32_t(p_height) / PAGE_SIZE;
	ERR_FAIL_COND_V_MSG(width > MAX_PAGES || height > MAX_PAGES, RID(), vformat("A virtual texture can be at most %d texels a side.", MAX_PAGES * PAGE_SIZE));
	ERR_FAIL_COND_V_MSG(free_ids.is_empty(), RID(), vformat("Too many virtual textures exist at once (%d at most).", MAX_IDS - 1));

	const Cache cache = p_type == RSE::VIRTUAL_TEXTURE_RUNTIME ? CACHE_RUNTIME : CACHE_STREAMED;
	if (!_ensure_cache(cache)) {
		return RID();
	}

	VirtualTexture texture;
	texture.texture = p_texture;
	texture.type = p_type;
	texture.cache = cache;
	texture.width = width;
	texture.height = height;
	// Down to the mipmap whose pages cover the texture in one row or column.
	texture.mipmaps = MIN(Math::get_shift_from_power_of_2(width), Math::get_shift_from_power_of_2(height)) + 1;
	texture.id = free_ids[free_ids.size() - 1];
	free_ids.resize(free_ids.size() - 1);

	const uint32_t empty = _empty_entry(&texture);
	Vector<uint8_t> data;
	for (uint32_t level = 0; level < texture.mipmaps; level++) {
		texture.entries[level].resize((width >> level) * (height >> level));
		for (uint32_t &e : texture.entries[level]) {
			e = empty;
		}
		const int offset = data.size();
		data.resize(offset + texture.entries[level].size() * sizeof(uint32_t));
		memcpy(data.ptrw() + offset, texture.entries[level].ptr(), texture.entries[level].size() * sizeof(uint32_t));
	}

	RD::TextureFormat tf;
	tf.format = RD::DATA_FORMAT_R32_UINT;
	tf.width = width;
	tf.height = height;
	tf.mipmaps = texture.mipmaps;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
	Vector<Vector<uint8_t>> initial;
	initial.push_back(data);
	texture.page_table = RD::get_singleton()->texture_create(tf, RD::TextureView(), initial);
	if (texture.page_table.is_null()) {
		free_ids.push_back(texture.id);
		ERR_FAIL_V_MSG(RID(), "Could not create a virtual texture's page table.");
	}
	RD::get_singleton()->set_resource_name(texture.page_table, "Virtual texture page table");

	RID rid = texture_owner.make_rid();
	VirtualTexture *ptr = texture_owner.get_or_null(rid);
	*ptr = std::move(texture);
	ptr->self = rid;
	ids[ptr->id] = ptr;
	textures.push_back(ptr);
	return rid;
}

void VirtualTextureStorage::virtual_texture_free(RID p_virtual_texture) {
	VirtualTexture *texture = texture_owner.get_or_null(p_virtual_texture);
	if (!texture) {
		return;
	}
	for (const KeyValue<uint64_t, uint32_t> &E : texture->resident) {
		_release_tile(texture->cache, E.value);
	}
	for (const VirtualTexture::Drawing &d : texture->drawing) {
		if (!texture->resident.has(d.page)) {
			_release_tile(texture->cache, d.tile);
		}
	}
	requests_in_flight -= MIN(requests_in_flight, uint32_t(texture->requested.size()));
	if (texture->page_table.is_valid()) {
		RD::get_singleton()->free_rid(texture->page_table);
	}
	ids[texture->id] = nullptr;
	free_ids.push_back(texture->id);
	textures.erase(texture);
	texture_owner.free(p_virtual_texture);
}

void VirtualTextureStorage::virtual_texture_set_owner(RID p_virtual_texture, RID p_texture) {
	VirtualTexture *texture = texture_owner.get_or_null(p_virtual_texture);
	ERR_FAIL_NULL(texture);
	texture->texture = p_texture;
}

RID VirtualTextureStorage::virtual_texture_get_page_table(RID p_virtual_texture) const {
	const VirtualTexture *texture = texture_owner.get_or_null(p_virtual_texture);
	return texture ? texture->page_table : RID();
}

void VirtualTextureStorage::virtual_texture_set_page_request_callback(RID p_virtual_texture, const Callable &p_callback) {
	VirtualTexture *texture = texture_owner.get_or_null(p_virtual_texture);
	ERR_FAIL_NULL(texture);
	texture->request_callback = p_callback;
	// Whatever was asked of a previous callback will not come back.
	requests_in_flight -= MIN(requests_in_flight, uint32_t(texture->requested.size()));
	texture->requested.clear();
	texture->coarsest_queued = false;
}

void VirtualTextureStorage::virtual_texture_update_page(RID p_virtual_texture, int p_mipmap, int p_x, int p_y, const Ref<Image> &p_image) {
	// Pages can arrive after their texture is gone: providers stream them on threads of their own.
	VirtualTexture *texture = texture_owner.get_or_null(p_virtual_texture);
	if (!texture) {
		return;
	}
	ERR_FAIL_COND(texture->type != RSE::VIRTUAL_TEXTURE_STREAMED);
	ERR_FAIL_COND(p_mipmap < 0 || uint32_t(p_mipmap) >= texture->mipmaps);
	ERR_FAIL_COND(p_x < 0 || p_y < 0 || uint32_t(p_x) >= (texture->width >> p_mipmap) || uint32_t(p_y) >= (texture->height >> p_mipmap));

	const uint64_t page = _make_page(p_mipmap, p_x, p_y);
	if (texture->requested.erase(page)) {
		requests_in_flight -= MIN(requests_in_flight, 1u);
	}
	if (p_image.is_null() || p_image->is_empty()) {
		// The provider gave up on it.
		return;
	}
	ERR_FAIL_COND_MSG(p_image->get_width() != TILE_SIZE || p_image->get_height() != TILE_SIZE,
			vformat("A virtual texture page must be %dx%d texels: the page and %d texels of its neighbors around it.", TILE_SIZE, TILE_SIZE, PAGE_BORDER));

	Ref<Image> image = p_image;
	if (image->has_mipmaps() || image->get_format() != Image::FORMAT_RGBA8) {
		image = image->duplicate();
		if (image->is_compressed()) {
			image->decompress();
		}
		image->clear_mipmaps();
		image->convert(Image::FORMAT_RGBA8);
	}

	Upload upload;
	upload.texture = p_virtual_texture;
	upload.page = page;
	upload.image = image;
	uploads.push_back(upload);
}

void VirtualTextureStorage::virtual_texture_set_runtime_volume(RID p_virtual_texture, RID p_scenario, const Transform3D &p_volume, uint32_t p_layers) {
	VirtualTexture *texture = texture_owner.get_or_null(p_virtual_texture);
	ERR_FAIL_NULL(texture);
	ERR_FAIL_COND(texture->type != RSE::VIRTUAL_TEXTURE_RUNTIME);

	// Only where the volume lies across its texture matters to what is drawn into it: how far up and
	// down it reaches changes what is in it only if something was cut off, which this leaves to
	// whoever drew it. A landscape being sculpted moves its top and bottom all the time.
	const Transform3D &old = texture->volume;
	const Vector3 up = p_volume.basis.get_column(1).normalized();
	const Vector3 offset = p_volume.origin - old.origin;
	const bool moved = !p_volume.basis.get_column(0).is_equal_approx(old.basis.get_column(0)) ||
			!p_volume.basis.get_column(2).is_equal_approx(old.basis.get_column(2)) ||
			!(offset - up * offset.dot(up)).is_zero_approx() ||
			!up.is_equal_approx(old.basis.get_column(1).normalized());
	const bool redraw = moved || texture->scenario != p_scenario || texture->layers != p_layers;

	texture->scenario = p_scenario;
	texture->volume = p_volume;
	texture->layers = p_layers;
	if (redraw) {
		_mark_stale(texture, Rect2(0, 0, 1, 1));
	}
}

void VirtualTextureStorage::virtual_texture_invalidate(RID p_virtual_texture, const Rect2 &p_uv_rect) {
	VirtualTexture *texture = texture_owner.get_or_null(p_virtual_texture);
	ERR_FAIL_NULL(texture);
	if (texture->type == RSE::VIRTUAL_TEXTURE_RUNTIME) {
		_mark_stale(texture, p_uv_rect);
		return;
	}

	// A streamed texture's pages are dropped, so that they are asked for again.
	LocalVector<uint64_t> dropped;
	for (const KeyValue<uint64_t, uint32_t> &E : texture->resident) {
		const uint32_t mip = _page_mip(E.key);
		const float pages_x = float(texture->width >> mip);
		const float pages_y = float(texture->height >> mip);
		const Rect2 page_rect(Point2(_page_x(E.key) / pages_x, _page_y(E.key) / pages_y), Size2(1.0f / pages_x, 1.0f / pages_y));
		if (page_rect.intersects(p_uv_rect, true)) {
			dropped.push_back(E.key);
		}
	}
	for (const uint64_t page : dropped) {
		const uint32_t tile = texture->resident[page];
		_unmap_page(texture, page);
		_release_tile(texture->cache, tile);
	}
	texture->coarsest_queued = false;
}

void VirtualTextureStorage::invalidate_world_aabb(RID p_scenario, uint32_t p_layers, const AABB &p_aabb) {
	for (VirtualTexture *texture : textures) {
		if (texture->type != RSE::VIRTUAL_TEXTURE_RUNTIME || texture->scenario != p_scenario || !(texture->layers & p_layers)) {
			continue;
		}
		const Transform3D to_volume = texture->volume.affine_inverse();
		Rect2 uv_rect;
		for (int i = 0; i < 8; i++) {
			const Vector3 local = to_volume.xform(p_aabb.get_endpoint(i));
			const Point2 uv(local.x, local.z);
			if (i == 0) {
				uv_rect.position = uv;
			} else {
				uv_rect.expand_to(uv);
			}
		}
		if (uv_rect.intersects(Rect2(0, 0, 1, 1), true)) {
			_mark_stale(texture, uv_rect);
		}
	}
}

void VirtualTextureStorage::update() {
	frame++;
	runtime_pages_this_frame = 0;
	if (!enabled) {
		return;
	}
	_process_feedback();
	_read_feedback();
	for (VirtualTexture *texture : textures) {
		_queue_coarsest(texture);
	}
	_request_streamed_pages();
	_process_uploads();
}

void VirtualTextureStorage::get_runtime_pending(LocalVector<RID> &r_textures) {
	for (VirtualTexture *texture : textures) {
		if (texture->type == RSE::VIRTUAL_TEXTURE_RUNTIME && !texture->queue.is_empty() && texture->scenario.is_valid()) {
			r_textures.push_back(texture->texture);
		}
	}
}

RID VirtualTextureStorage::runtime_begin(RID p_virtual_texture, LocalVector<RendererTextureStorage::VirtualTextureRenderPage> &r_pages, RID &r_scenario, uint32_t &r_layers) {
	r_pages.clear();
	VirtualTexture *texture = texture_owner.get_or_null(p_virtual_texture);
	if (!texture || texture->type != RSE::VIRTUAL_TEXTURE_RUNTIME || texture->scenario.is_null() || texture->queue.is_empty()) {
		return RID();
	}
	if (runtime_pages_this_frame >= max_runtime_pages_per_frame || !_ensure_runtime_target()) {
		return RID();
	}

	const uint32_t slots = MIN(uint32_t(runtime_target.grid * runtime_target.grid), max_runtime_pages_per_frame - runtime_pages_this_frame);
	CachePool &pool = caches[CACHE_RUNTIME];
	texture->drawing.clear();

	HashSet<uint64_t> taken;
	uint32_t consumed = 0;
	for (; consumed < texture->queue.size() && texture->drawing.size() < slots; consumed++) {
		const uint64_t page = texture->queue[consumed];
		if (taken.has(page)) {
			continue;
		}
		taken.insert(page);
		const uint32_t *resident_tile = texture->resident.getptr(page);
		if (resident_tile && !texture->stale.has(page)) {
			continue;
		}
		uint32_t tile = resident_tile ? *resident_tile : _allocate_tile(CACHE_RUNTIME);
		if (tile == UINT32_MAX) {
			break;
		}
		if (!resident_tile) {
			// Held for the page until runtime_end() maps it.
			Tile &t = pool.tiles[tile];
			t.id = texture->id;
			t.page = page;
			t.last_used = frame;
		}

		const uint32_t slot = texture->drawing.size();
		VirtualTexture::Drawing drawing;
		drawing.page = page;
		drawing.tile = tile;
		drawing.region = Rect2i((slot % runtime_target.grid) * TILE_SIZE, (slot / runtime_target.grid) * TILE_SIZE, TILE_SIZE, TILE_SIZE);
		texture->drawing.push_back(drawing);

		RendererTextureStorage::VirtualTextureRenderPage render_page;
		_page_camera(texture, page, render_page);
		render_page.region = drawing.region;
		r_pages.push_back(render_page);
	}

	if (consumed >= texture->queue.size()) {
		texture->queue.clear();
	} else if (consumed > 0) {
		LocalVector<uint64_t> remaining;
		remaining.reserve(texture->queue.size() - consumed);
		for (uint32_t i = consumed; i < texture->queue.size(); i++) {
			remaining.push_back(texture->queue[i]);
		}
		texture->queue = remaining;
	}

	if (texture->drawing.is_empty()) {
		return RID();
	}
	runtime_pages_this_frame += texture->drawing.size();
	r_scenario = texture->scenario;
	r_layers = texture->layers;
	return runtime_target.framebuffer;
}

void VirtualTextureStorage::runtime_end(RID p_virtual_texture) {
	VirtualTexture *texture = texture_owner.get_or_null(p_virtual_texture);
	if (!texture) {
		return;
	}
	CachePool &pool = caches[CACHE_RUNTIME];
	RD *rd = RD::get_singleton();
	const RID sources[RUNTIME_LAYERS] = { runtime_target.albedo, runtime_target.normal, runtime_target.orm };
	for (const VirtualTexture::Drawing &drawing : texture->drawing) {
		const Vector3 to((drawing.tile % pool.tiles_per_side) * TILE_SIZE, (drawing.tile / pool.tiles_per_side) * TILE_SIZE, 0);
		const Vector3 from(drawing.region.position.x, drawing.region.position.y, 0);
		for (int layer = 0; layer < RUNTIME_LAYERS; layer++) {
			rd->texture_copy(sources[layer], pool.texture, from, to, Vector3(TILE_SIZE, TILE_SIZE, 1), 0, 0, 0, layer);
		}
		DownsampleTile ds;
		ds.cache = CACHE_RUNTIME;
		ds.tile = drawing.tile;
		downsample_tiles.push_back(ds);

		if (texture->resident.has(drawing.page)) {
			pool.tiles[drawing.tile].last_used = frame;
		} else {
			_map_page(texture, drawing.page, drawing.tile);
		}
		texture->stale.erase(drawing.page);
	}
	texture->drawing.clear();
}

void VirtualTextureStorage::flush() {
	if (!enabled) {
		return;
	}
	_downsample_tiles();
	_upload_page_tables();
}

RID VirtualTextureStorage::get_cache_texture(Cache p_cache, bool p_srgb) const {
	const CachePool &pool = caches[p_cache];
	return p_srgb ? pool.texture_srgb : pool.texture;
}

void VirtualTextureStorage::get_scene_uniforms(RD::Uniform *r_uniforms, uint32_t p_first_binding) const {
	// A cache that does not exist yet (no virtual texture of its kind does) reads as white.
	const RID stand_in = TextureStorage::get_singleton()->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_WHITE);
	for (int srgb = 0; srgb < 2; srgb++) {
		RD::Uniform &u = r_uniforms[srgb];
		u = RD::Uniform();
		u.binding = p_first_binding + srgb;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		for (int i = 0; i < CACHE_MAX; i++) {
			const RID texture = get_cache_texture(Cache(i), srgb != 0);
			u.append_id(texture.is_valid() ? texture : stand_in);
		}
	}
	RD::Uniform &u = r_uniforms[2];
	u = RD::Uniform();
	u.binding = p_first_binding + 2;
	u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	u.append_id(feedback_buffer);
}

VirtualTextureStorage::VirtualTextureStorage() {
	singleton = this;

	enabled = GLOBAL_GET("rendering/virtual_texturing/enabled");
	cache_sizes[CACHE_STREAMED] = CLAMP(int(GLOBAL_GET("rendering/virtual_texturing/streamed_cache_size")), 8, int(MAX_TILES_PER_SIDE));
	cache_sizes[CACHE_RUNTIME] = CLAMP(int(GLOBAL_GET("rendering/virtual_texturing/runtime_cache_size")), 8, int(MAX_TILES_PER_SIDE));
	max_uploads_per_frame = MAX(1, int(GLOBAL_GET("rendering/virtual_texturing/max_page_uploads_per_frame")));
	max_runtime_pages_per_frame = CLAMP(int(GLOBAL_GET("rendering/virtual_texturing/max_runtime_pages_per_frame")), 1, 64);

	for (uint32_t i = MAX_IDS - 1; i > 0; i--) {
		free_ids.push_back(i);
	}

	RD *rd = RD::get_singleton();

	const uint32_t feedback_size = FEEDBACK_HEADER_SIZE + FEEDBACK_SLOTS * sizeof(uint32_t);
	Vector<uint8_t> zeros;
	zeros.resize(feedback_size);
	zeros.fill(0);
	feedback_buffer = rd->storage_buffer_create(feedback_size, zeros);
	rd->set_resource_name(feedback_buffer, "Virtual texture feedback");

	// Null page tables name the streamed cache's tiles of flat color. Until that cache exists (until
	// some streamed virtual texture does), the scene shaders read a white stand-in for it instead.
	const StreamedReservedTile null_tiles[NULL_PAGE_TABLE_MAX] = { STREAMED_TILE_WHITE, STREAMED_TILE_BLACK, STREAMED_TILE_NORMAL, STREAMED_TILE_TRANSPARENT };
	for (int i = 0; i < NULL_PAGE_TABLE_MAX; i++) {
		const uint32_t entry = _encode_entry(CACHE_STREAMED, null_tiles[i], ENTRY_EMPTY_MIP, 0);
		Vector<uint8_t> data;
		data.resize(sizeof(uint32_t));
		memcpy(data.ptrw(), &entry, sizeof(uint32_t));
		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R32_UINT;
		tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT;
		Vector<Vector<uint8_t>> initial;
		initial.push_back(data);
		null_page_tables[i] = rd->texture_create(tf, RD::TextureView(), initial);
	}
}

VirtualTextureStorage::~VirtualTextureStorage() {
	RD *rd = RD::get_singleton();

	LocalVector<RID> owned;
	for (VirtualTexture *texture : textures) {
		owned.push_back(texture->self);
	}
	for (const RID &rid : owned) {
		virtual_texture_free(rid);
	}

	_free_runtime_target();
	for (int i = 0; i < CACHE_MAX; i++) {
		_free_cache(Cache(i));
	}
	for (int i = 0; i < NULL_PAGE_TABLE_MAX; i++) {
		if (null_page_tables[i].is_valid()) {
			rd->free_rid(null_page_tables[i]);
		}
	}
	if (feedback_buffer.is_valid()) {
		rd->free_rid(feedback_buffer);
	}
	if (downsample_shader_version.is_valid()) {
		downsample_shader.version_free(downsample_shader_version);
	}
	singleton = nullptr;
}
