/**************************************************************************/
/*  virtual_texture_storage.h                                             */
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
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid_owner.h"
#include "servers/rendering/renderer_rd/shaders/virtual_texture_downsample.glsl.gen.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/storage/texture_storage.h"

namespace RendererRD {

// Virtual texturing: textures far larger than video memory could hold whole, of which only the pages
// the camera actually sees, at the resolution it sees them at, are kept in a shared cache.
//
// A virtual texture is a grid of PAGE_SIZE x PAGE_SIZE texel pages at every mipmap down to the one
// whose pages cover it in a single row or column. Its page table is a texture with one texel per page
// (and the same mipmaps), each naming the tile of the cache that holds the best page there is for
// that spot: the page itself, or the closest of its ancestors that is resident. Shaders (see
// virtual_texture_inc.glsl) look the page table up at the mipmap they want, sample whichever page it
// names, and, for a few pixels a frame, record the page they wanted in a feedback buffer. That buffer
// is read back a few frames later, and the pages it asks for are streamed in (VIRTUAL_TEXTURE_STREAMED,
// handed over by a callback's owner, from disk usually) or drawn (VIRTUAL_TEXTURE_RUNTIME, from above,
// out of whatever instances draw into the texture's volume; see RendererSceneCull) into tiles that
// nothing has sampled for the longest.
//
// Every tile is a page with PAGE_BORDER texels of its neighbors around it, so that filtering across
// the page's edge stays seamless, plus two mipmaps of its own. Sampling a tile between its own
// mipmaps is what blends a page into its parent smoothly: the parent holds the same texels its first
// mipmap does.
//
// Page table entries are 32 bits: the tile's X and Y in the cache (8 bits each), the mipmap of the
// page it holds (4 bits, 15 while nothing is resident), the cache (1 bit) and the texture's ID
// (11 bits). Keeping the ID in every entry lets any shader that samples a page also report which
// texture it wanted it from, with nothing more bound than the page table.
class VirtualTextureStorage {
public:
	static constexpr int PAGE_SIZE = RSE::VIRTUAL_TEXTURE_PAGE_SIZE;
	static constexpr int PAGE_BORDER = RSE::VIRTUAL_TEXTURE_PAGE_BORDER;
	static constexpr int TILE_SIZE = PAGE_SIZE + PAGE_BORDER * 2;
	static constexpr int TILE_MIPMAPS = 3;
	// 1024 pages a side at most: 131072 texels, and what fits the feedback buffer's page indices.
	static constexpr uint32_t MAX_PAGES = 1024;
	static constexpr int MAX_MIPMAPS = 11;
	static constexpr uint32_t MAX_IDS = 2048;
	static constexpr uint32_t MAX_TILES_PER_SIDE = 128;
	static constexpr uint32_t ENTRY_EMPTY_MIP = 15;
	static constexpr uint32_t FEEDBACK_SLOTS = 8192;
	static constexpr uint32_t FEEDBACK_HEADER_SIZE = 16;
	// Runtime virtual textures: albedo; normal with specular in alpha; ORM, with in alpha the subsurface
	// scattering strength of the opaque surface drawn there (which Landscape3D sets to 1 to mark its own
	// ground), faded by whatever is blended over it.
	static constexpr int RUNTIME_LAYERS = 3;
	// Sampling a runtime virtual texture's layer 3 reads the height of the ground there instead, from 0
	// at the bottom of its volume to 1 at its top (0 too where there is none).
	static constexpr int RUNTIME_HEIGHT_LAYER = 3;

	enum Cache {
		CACHE_STREAMED,
		CACHE_RUNTIME,
		CACHE_MAX,
	};

	// What a hint_virtual_texture uniform with no virtual texture assigned reads as.
	enum NullPageTable {
		NULL_PAGE_TABLE_WHITE,
		NULL_PAGE_TABLE_BLACK,
		NULL_PAGE_TABLE_NORMAL,
		NULL_PAGE_TABLE_TRANSPARENT,
		NULL_PAGE_TABLE_MAX,
	};

private:
	static VirtualTextureStorage *singleton;

	// The streamed cache keeps a few tiles of flat color for null page tables, and one for pages that
	// are not there yet; the runtime cache only the latter.
	enum StreamedReservedTile {
		STREAMED_TILE_WHITE,
		STREAMED_TILE_BLACK,
		STREAMED_TILE_NORMAL,
		STREAMED_TILE_TRANSPARENT,
		STREAMED_TILE_LOADING,
		STREAMED_TILE_RESERVED,
	};

	struct Tile {
		uint32_t id = 0; // Of the texture whose page this holds; 0 while free.
		uint64_t page = 0;
		uint64_t last_used = 0;
		bool pinned = false;
	};

	struct CachePool {
		int layers = 0;
		uint32_t tiles_per_side = 0;
		RID texture;
		RID texture_srgb;
		RID storage_mips[TILE_MIPMAPS];
		// Runtime cache only: the height of the ground in every tile, its RUNTIME_HEIGHT_LAYER, kept
		// apart as it needs more precision than a byte (height_format).
		RID height;
		RID height_storage_mips[TILE_MIPMAPS];
		LocalVector<Tile> tiles;
		LocalVector<uint32_t> free_tiles;
		uint32_t reserved_tiles = 0;
		uint32_t loading_tile = 0;
	};

	struct VirtualTexture {
		RID self;
		RID texture;
		uint32_t id = 0;
		RSE::VirtualTextureType type = RSE::VIRTUAL_TEXTURE_STREAMED;
		Cache cache = CACHE_STREAMED;
		// Pages along each side at mipmap 0, and how many mipmaps there are.
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t mipmaps = 0;

		RID page_table;
		LocalVector<uint32_t> entries[MAX_MIPMAPS];
		Rect2i dirty[MAX_MIPMAPS];

		HashMap<uint64_t, uint32_t> resident; // Page to tile.
		// Streamed: asked for, not handed over yet, and the frame it was asked for in.
		HashMap<uint64_t, uint64_t> requested;
		// Runtime: resident, but drawn before something changed under it.
		HashSet<uint64_t> stale;
		// Pages to stream in or draw, best first.
		LocalVector<uint64_t> queue;
		bool coarsest_queued = false;

		Callable request_callback;

		RID scenario;
		Transform3D volume;
		uint32_t layers = 0;

		struct Drawing {
			uint64_t page = 0;
			uint32_t tile = 0;
			Rect2i region;
		};
		LocalVector<Drawing> drawing;
	};

	struct Upload {
		RID texture;
		uint64_t page = 0;
		Ref<Image> image;
	};

	struct Candidate {
		RID texture;
		uint64_t page = 0;
		int score = 0;

		// Best first.
		bool operator<(const Candidate &p_other) const { return score > p_other.score; }
	};

	struct DownsampleTile {
		Cache cache = CACHE_STREAMED;
		uint32_t tile = 0;
	};

	struct DownsamplePushConstant {
		int32_t dest_origin[2];
		int32_t dest_size;
		int32_t layers;
		int32_t source_origin[2];
		float depth_to_height_scale;
		float depth_to_height_bias;
	};

	enum DownsampleMode {
		DOWNSAMPLE_MODE_COLOR,
		DOWNSAMPLE_MODE_HEIGHT,
		DOWNSAMPLE_MODE_RESOLVE_HEIGHT,
		DOWNSAMPLE_MODE_MAX,
	};

	// What runtime pages are drawn into, a grid of tiles of it at a time, before they are copied into
	// the cache. Laid out like render_material() wants it: albedo, normal, ORM, emission and depth
	// outputs, then the depth buffer. Albedo is drawn through an sRGB view, so what is copied out of it
	// is already encoded the way the cache's sRGB view reads it back. The height of the ground comes
	// from the depth buffer: what is drawn with depth (opaque surfaces) is ground, what is blended over
	// it (decals) is not.
	struct RuntimeTarget {
		int grid = 0;
		RID albedo;
		RID albedo_srgb;
		RID normal;
		RID orm;
		RID emission;
		RID depth_output;
		RID depth;
		RID framebuffer;
	};

	bool enabled = false;
	uint32_t cache_sizes[CACHE_MAX] = {};
	uint32_t max_uploads_per_frame = 32;
	uint32_t max_runtime_pages_per_frame = 16;
	uint32_t max_requests_in_flight = 256;

	mutable RID_Owner<VirtualTexture> texture_owner;
	LocalVector<VirtualTexture *> textures;
	VirtualTexture *ids[MAX_IDS] = {};
	LocalVector<uint32_t> free_ids;

	// Streamed pages to ask for, best first, across every texture.
	LocalVector<Candidate> streamed_queue;
	uint32_t requests_in_flight = 0;

	CachePool caches[CACHE_MAX];
	// Bumped whenever a cache's textures change, so that the scene shaders' global uniform sets get
	// rebuilt with the new ones.
	uint64_t bindings_version = 1;

	RID null_page_tables[NULL_PAGE_TABLE_MAX];

	RID feedback_buffer;
	uint32_t feedback_reads_in_flight = 0;
	Vector<uint8_t> feedback_data;
	bool feedback_data_ready = false;

	uint64_t frame = 0;
	uint32_t runtime_pages_this_frame = 0;
	uint32_t uploads_this_frame = 0;

	// See needs_redraw(): how many feedback read backs in a row wanted no page that was missing, how
	// many frames in a row neither drew nor uploaded a page, and whether the last frame asked for no
	// other frame to follow.
	uint32_t quiet_feedback_reads = 0;
	uint32_t frames_without_pages = 0;
	bool settled = false;

	LocalVector<Upload> uploads;
	LocalVector<DownsampleTile> downsample_tiles;
	RuntimeTarget runtime_target;

	VirtualTextureDownsampleShaderRD downsample_shader;
	RID downsample_shader_version;
	RID downsample_pipelines[DOWNSAMPLE_MODE_MAX];
	RD::DataFormat height_format = RD::DATA_FORMAT_R16_UNORM;

	static _FORCE_INLINE_ uint64_t _make_page(uint32_t p_mip, uint32_t p_x, uint32_t p_y) {
		return (uint64_t(p_mip) << 40) | (uint64_t(p_y) << 20) | uint64_t(p_x);
	}
	static _FORCE_INLINE_ uint32_t _page_mip(uint64_t p_page) { return uint32_t(p_page >> 40); }
	static _FORCE_INLINE_ uint32_t _page_x(uint64_t p_page) { return uint32_t(p_page & 0xFFFFF); }
	static _FORCE_INLINE_ uint32_t _page_y(uint64_t p_page) { return uint32_t((p_page >> 20) & 0xFFFFF); }
	static _FORCE_INLINE_ uint64_t _page_parent(uint64_t p_page) { return _make_page(_page_mip(p_page) + 1, _page_x(p_page) >> 1, _page_y(p_page) >> 1); }

	uint32_t _encode_entry(Cache p_cache, uint32_t p_tile, uint32_t p_mip, uint32_t p_id) const;
	uint32_t _empty_entry(const VirtualTexture *p_texture) const;

	bool _ensure_cache(Cache p_cache);
	void _free_cache(Cache p_cache);
	void _fill_tile(Cache p_cache, uint32_t p_tile, const Color *p_layer_colors);
	void _fill_tile_level(Cache p_cache, uint32_t p_tile, int p_layer, int p_mipmap, const Vector<uint8_t> &p_data);
	void _upload_tile(Cache p_cache, uint32_t p_tile, int p_layer, const Vector<uint8_t> &p_data);
	uint32_t _allocate_tile(Cache p_cache);
	void _release_tile(Cache p_cache, uint32_t p_tile);

	void _set_entries(VirtualTexture *p_texture, uint64_t p_page, uint32_t p_entry, bool p_mapping);
	void _map_page(VirtualTexture *p_texture, uint64_t p_page, uint32_t p_tile);
	void _unmap_page(VirtualTexture *p_texture, uint64_t p_page);
	void _touch_page(VirtualTexture *p_texture, uint64_t p_page);
	uint32_t _resident_level(const VirtualTexture *p_texture, uint64_t p_page) const;
	void _mark_stale(VirtualTexture *p_texture, const Rect2 &p_uv_rect);
	void _queue_coarsest(VirtualTexture *p_texture);

	bool _decode_feedback_key(uint32_t p_key, VirtualTexture *&r_texture, uint64_t &r_page) const;
	void _process_feedback();
	void _read_feedback();
	static void _feedback_received(const Vector<uint8_t> &p_data);

	void _request_streamed_pages();
	void _process_uploads();
	void _upload_page_tables();
	void _downsample_tiles();

	bool _ensure_runtime_target();
	void _free_runtime_target();
	// How far above and below a runtime texture's volume its pages are drawn from.
	static void _page_depth_range(const VirtualTexture *p_texture, real_t &r_margin, real_t &r_volume_height);
	void _page_camera(const VirtualTexture *p_texture, uint64_t p_page, RendererTextureStorage::VirtualTextureRenderPage &r_page) const;
	void _resolve_heights(const VirtualTexture *p_texture);

public:
	static VirtualTextureStorage *get_singleton() { return singleton; }

	// How render_material(), which draws runtime pages, writes into its five outputs. A surface with
	// alpha (the feathered edge of a road, a decal) is blended over what was drawn under it: albedo
	// with its alpha, normal and ORM with the same alpha (which the shader writes into theirs too),
	// keeping the alpha those hold under it (specular, subsurface scattering).
	static RD::PipelineColorBlendState get_material_pass_blend_state(bool p_uses_alpha);

	bool is_enabled() const { return enabled; }

	// Creates the virtual texture state behind p_texture, an ordinary texture of the fallback image as
	// far as everything else is concerned. An invalid RID if there is none to be had (the size is
	// unsupported, or too many virtual textures exist): p_texture then stays a plain texture.
	RID virtual_texture_create(RID p_texture, int p_width, int p_height, RSE::VirtualTextureType p_type);
	void virtual_texture_free(RID p_virtual_texture);
	bool owns_virtual_texture(RID p_virtual_texture) const { return texture_owner.owns(p_virtual_texture); }
	// texture_replace() moved the state to another texture RID.
	void virtual_texture_set_owner(RID p_virtual_texture, RID p_texture);

	RID virtual_texture_get_page_table(RID p_virtual_texture) const;
	RID get_null_page_table(NullPageTable p_which) const { return null_page_tables[p_which]; }

	void virtual_texture_set_page_request_callback(RID p_virtual_texture, const Callable &p_callback);
	void virtual_texture_update_page(RID p_virtual_texture, int p_mipmap, int p_x, int p_y, const Ref<Image> &p_image);
	void virtual_texture_set_runtime_volume(RID p_virtual_texture, RID p_scenario, const Transform3D &p_volume, uint32_t p_layers);
	void virtual_texture_invalidate(RID p_virtual_texture, const Rect2 &p_uv_rect);

	void update();
	void get_runtime_pending(LocalVector<RID> &r_textures);
	RID runtime_begin(RID p_virtual_texture, LocalVector<RendererTextureStorage::VirtualTextureRenderPage> &r_pages, RID &r_scenario, uint32_t &r_layers);
	void runtime_end(RID p_virtual_texture);
	void invalidate_world_aabb(RID p_scenario, uint32_t p_layers, const AABB &p_aabb);
	void flush();
	bool needs_redraw();

	// For the scene shaders' global uniform sets.
	uint64_t get_bindings_version() const { return bindings_version; }
	RID get_cache_texture(Cache p_cache, bool p_srgb) const;
	RID get_feedback_buffer() const { return feedback_buffer; }
	// The bindings virtual_texture_inc.glsl's including shaders declare: the caches, their sRGB views,
	// the feedback buffer and the runtime cache's heights, at consecutive bindings from p_first_binding.
	static constexpr int SCENE_UNIFORM_COUNT = 4;
	void get_scene_uniforms(RD::Uniform *r_uniforms, uint32_t p_first_binding) const;

	VirtualTextureStorage();
	~VirtualTextureStorage();
};

} // namespace RendererRD
