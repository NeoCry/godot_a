/**************************************************************************/
/*  virtual_texture_2d.h                                                  */
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

#include "core/io/resource_loader.h"
#include "scene/resources/texture.h"

class BitMap;

// A texture streamed from disk a page at a time (see RenderingServer.texture_virtual_create()): only
// the pages a camera actually samples, at the resolution it samples them at, are ever loaded, so a
// 16K texture costs a few megabytes of video memory rather than a gigabyte.
//
// Made by converting an imported texture (FileSystem dock, Convert to Virtual Texture), which keeps
// every reference to it: materials that sample it through a hint_virtual_texture uniform stream it
// (StandardMaterial3D adds the hint by itself), and everything else sees its fallback, a small copy
// of the whole texture.
//
// The .vtex file holds the texture, resized to powers of two, as pages of
// RenderingServer.VIRTUAL_TEXTURE_PAGE_SIZE texels a side with RenderingServer.VIRTUAL_TEXTURE_PAGE_BORDER
// texels of their neighbors around them, at every mipmap down to one page across, each compressed on
// its own, plus the fallback image.
class VirtualTexture2D : public Texture2D {
	GDCLASS(VirtualTexture2D, Texture2D);

public:
	enum PageFormat {
		PAGE_FORMAT_LOSSLESS, // WebP, lossless.
		PAGE_FORMAT_LOSSY, // WebP.
		PAGE_FORMAT_UNCOMPRESSED, // Raw RGBA8, Zstandard compressed.
	};

	static constexpr uint32_t FORMAT_VERSION = 1;
	static constexpr int FALLBACK_SIZE = 256;

	enum Flags {
		FLAG_HAS_ALPHA = 1,
	};

	struct Header {
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t mipmaps = 0;
		uint32_t flags = 0;
		PageFormat page_format = PAGE_FORMAT_LOSSLESS;
		Ref<Image> fallback;
		// Where each page is, finest mipmap first, row by row.
		LocalVector<uint64_t> page_offsets;
		LocalVector<uint32_t> page_sizes;

		uint32_t get_page_index(int p_mipmap, int p_x, int p_y) const;
	};

	// Writes p_image (RGBA8, both sides a power of two of at least one page) as a .vtex file.
	// p_wrap_borders takes the texels around pages at the texture's edges from its opposite edge, as a
	// repeating texture samples them, rather than repeating the edge. p_normal_map renormalizes every
	// mipmap's texels as normals.
	static Error save_to_file(const String &p_path, const Ref<Image> &p_image, PageFormat p_page_format, float p_lossy_quality = 0.8, bool p_wrap_borders = true, bool p_normal_map = false);
	static Error read_header(Ref<FileAccess> p_file, Header &r_header);
	static Ref<Image> decode_page(const Vector<uint8_t> &p_data, PageFormat p_page_format);

	// Pages a provider was asked for, from the rendering thread; see _streamer_thread_func().
	static void _request_pages(const PackedInt32Array &p_pages, int p_stream);

	static void finish();

private:
	String path_to_file;
	Header header;
	mutable RID texture;
	int stream = 0;
	mutable Ref<BitMap> alpha_cache;

	void _clear();
	Ref<Image> _read_page(int p_mipmap, int p_x, int p_y) const;

	static void _streamer_thread_func(void *p_userdata);

protected:
	static void _bind_methods();
	virtual void reload_from_file() override;

public:
	Error load(const String &p_path);
	String get_load_path() const;

	virtual Image::Format get_format() const override;
	virtual int get_width() const override;
	virtual int get_height() const override;
	virtual RID get_rid() const override;
	virtual bool has_alpha() const override;
	virtual bool is_pixel_opaque(int p_x, int p_y) const override;
	// The whole texture at full resolution, put back together from its pages.
	virtual Ref<Image> get_image() const override;

	Ref<Image> get_fallback_image() const;
	int get_page_mipmap_count() const;
	Vector2i get_page_count(int p_mipmap) const;
	// One page with its border, as the renderer receives it.
	Ref<Image> get_page_image(int p_mipmap, int p_x, int p_y) const;
	PageFormat get_page_format() const;

	VirtualTexture2D();
	~VirtualTexture2D();
};

VARIANT_ENUM_CAST(VirtualTexture2D::PageFormat);

class ResourceFormatLoaderVirtualTexture2D : public ResourceFormatLoader {
	GDSOFTCLASS(ResourceFormatLoaderVirtualTexture2D, ResourceFormatLoader);

public:
	virtual Ref<Resource> load(const String &p_path, const String &p_original_path = "", Error *r_error = nullptr, bool p_use_sub_threads = false, float *r_progress = nullptr, CacheMode p_cache_mode = CACHE_MODE_REUSE) override;
	virtual void get_recognized_extensions(List<String> *p_extensions) const override;
	virtual bool handles_type(const String &p_type) const override;
	virtual String get_resource_type(const String &p_path) const override;
};
