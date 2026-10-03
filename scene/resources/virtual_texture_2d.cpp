/**************************************************************************/
/*  virtual_texture_2d.cpp                                                */
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

#include "virtual_texture_2d.h"

#include "core/io/compression.h"
#include "core/io/file_access.h"
#include "core/math/math_funcs_binary.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/mutex.h"
#include "core/os/semaphore.h"
#include "core/os/thread.h"
#include "core/templates/hash_map.h"
#include "scene/resources/bit_map.h"
#include "servers/rendering/rendering_server.h"

namespace {

constexpr int PAGE_SIZE = RSE::VIRTUAL_TEXTURE_PAGE_SIZE;
constexpr int PAGE_BORDER = RSE::VIRTUAL_TEXTURE_PAGE_BORDER;
constexpr int TILE_SIZE = PAGE_SIZE + PAGE_BORDER * 2;
constexpr uint32_t MAX_SIZE = 1024 * PAGE_SIZE;
const char FILE_MAGIC[4] = { 'G', 'D', 'V', 'T' };

// A loaded texture's file, as the streaming thread reads it.
class VirtualTexturePageStream : public RefCounted {
	GDSOFTCLASS(VirtualTexturePageStream, RefCounted);

public:
	Mutex mutex;
	Ref<FileAccess> file;
	VirtualTexture2D::Header header;
	RID texture;

	Ref<Image> read_page(int p_mipmap, int p_x, int p_y) {
		const uint32_t index = header.get_page_index(p_mipmap, p_x, p_y);
		if (index >= header.page_offsets.size()) {
			return Ref<Image>();
		}
		Vector<uint8_t> data;
		{
			MutexLock lock(mutex);
			if (file.is_null()) {
				return Ref<Image>();
			}
			data.resize(header.page_sizes[index]);
			file->seek(header.page_offsets[index]);
			if (file->get_buffer(data.ptrw(), data.size()) != uint64_t(data.size())) {
				return Ref<Image>();
			}
		}
		return VirtualTexture2D::decode_page(data, header.page_format);
	}
};

// The pages renderers asked for, streamed in on a thread of their own, oldest request first. Runs
// only while some virtual texture is loaded.
struct PageStreamer {
	struct Request {
		int stream = 0;
		int mipmap = 0;
		int x = 0;
		int y = 0;
	};

	Mutex mutex;
	Semaphore semaphore;
	Thread thread;
	SafeFlag exit;
	HashMap<int, Ref<VirtualTexturePageStream>> streams;
	int next_stream = 1;
	LocalVector<Request> queue;
	uint32_t queue_head = 0;
};

PageStreamer *streamer = nullptr;
Mutex streamer_mutex;

uint32_t page_level_count(uint32_t p_width, uint32_t p_height) {
	return MIN(Math::get_shift_from_power_of_2(p_width / PAGE_SIZE), Math::get_shift_from_power_of_2(p_height / PAGE_SIZE)) + 1;
}

// Renormalizes every texel as a tangent space normal, once a mipmap has averaged them.
void renormalize(const Ref<Image> &p_image) {
	uint8_t *w = p_image->ptrw();
	const int64_t count = int64_t(p_image->get_width()) * p_image->get_height();
	for (int64_t i = 0; i < count; i++) {
		uint8_t *t = w + i * 4;
		Vector3 n(t[0] / 127.5f - 1.0f, t[1] / 127.5f - 1.0f, t[2] / 127.5f - 1.0f);
		n = n.length_squared() > 0.000001f ? n.normalized() : Vector3(0, 0, 1);
		t[0] = uint8_t(CLAMP(Math::round((n.x + 1.0f) * 127.5f), 0.0f, 255.0f));
		t[1] = uint8_t(CLAMP(Math::round((n.y + 1.0f) * 127.5f), 0.0f, 255.0f));
		t[2] = uint8_t(CLAMP(Math::round((n.z + 1.0f) * 127.5f), 0.0f, 255.0f));
	}
}

struct EncodeJob {
	Ref<Image> level;
	uint32_t pages_x = 0;
	bool wrap = true;
	VirtualTexture2D::PageFormat page_format = VirtualTexture2D::PAGE_FORMAT_LOSSLESS;
	float lossy_quality = 0.8;
	Vector<Vector<uint8_t>> encoded;
};

void encode_page(void *p_userdata, uint32_t p_index) {
	EncodeJob *job = static_cast<EncodeJob *>(p_userdata);
	const int level_width = job->level->get_width();
	const int level_height = job->level->get_height();
	const int page_x = int(p_index % job->pages_x);
	const int page_y = int(p_index / job->pages_x);
	const uint8_t *src = job->level->ptr();

	Vector<uint8_t> texels;
	texels.resize(TILE_SIZE * TILE_SIZE * 4);
	uint8_t *dst = texels.ptrw();
	for (int row = 0; row < TILE_SIZE; row++) {
		int sy = page_y * PAGE_SIZE - PAGE_BORDER + row;
		sy = job->wrap ? Math::posmod(sy, level_height) : CLAMP(sy, 0, level_height - 1);
		for (int col = 0; col < TILE_SIZE; col++) {
			int sx = page_x * PAGE_SIZE - PAGE_BORDER + col;
			sx = job->wrap ? Math::posmod(sx, level_width) : CLAMP(sx, 0, level_width - 1);
			memcpy(dst + (row * TILE_SIZE + col) * 4, src + (int64_t(sy) * level_width + sx) * 4, 4);
		}
	}

	Vector<uint8_t> out;
	if (job->page_format == VirtualTexture2D::PAGE_FORMAT_UNCOMPRESSED) {
		out.resize(Compression::get_max_compressed_buffer_size(texels.size(), Compression::MODE_ZSTD));
		const int64_t size = Compression::compress(out.ptrw(), texels.ptr(), texels.size(), Compression::MODE_ZSTD);
		out.resize(MAX(size, 0));
	} else {
		Ref<Image> page = Image::create_from_data(TILE_SIZE, TILE_SIZE, false, Image::FORMAT_RGBA8, texels);
		if (job->page_format == VirtualTexture2D::PAGE_FORMAT_LOSSY) {
			out = Image::webp_lossy_packer(page, job->lossy_quality);
		} else {
			out = Image::webp_lossless_packer(page);
		}
	}
	job->encoded.write[p_index] = out;
}

void stop_streamer_locked() {
	// streamer_mutex is held.
	if (!streamer) {
		return;
	}
	streamer->exit.set();
	streamer->semaphore.post();
	if (streamer->thread.is_started()) {
		streamer->thread.wait_to_finish();
	}
	memdelete(streamer);
	streamer = nullptr;
}

} // namespace

/* HEADER */

uint32_t VirtualTexture2D::Header::get_page_index(int p_mipmap, int p_x, int p_y) const {
	if (p_mipmap < 0 || uint32_t(p_mipmap) >= mipmaps) {
		return UINT32_MAX;
	}
	uint32_t index = 0;
	for (int i = 0; i < p_mipmap; i++) {
		index += ((width / PAGE_SIZE) >> i) * ((height / PAGE_SIZE) >> i);
	}
	const int pages_x = int((width / PAGE_SIZE) >> p_mipmap);
	const int pages_y = int((height / PAGE_SIZE) >> p_mipmap);
	if (p_x < 0 || p_y < 0 || p_x >= pages_x || p_y >= pages_y) {
		return UINT32_MAX;
	}
	return index + uint32_t(p_y * pages_x + p_x);
}

Error VirtualTexture2D::read_header(Ref<FileAccess> p_file, Header &r_header) {
	ERR_FAIL_COND_V(p_file.is_null(), ERR_FILE_CANT_OPEN);
	uint8_t magic[4];
	p_file->get_buffer(magic, 4);
	ERR_FAIL_COND_V_MSG(memcmp(magic, FILE_MAGIC, 4) != 0, ERR_FILE_CORRUPT, "Not a virtual texture file.");
	const uint32_t version = p_file->get_32();
	ERR_FAIL_COND_V_MSG(version > FORMAT_VERSION, ERR_FILE_UNRECOGNIZED, "The virtual texture file is from a newer version of the engine; reimport it.");

	r_header = Header();
	r_header.width = p_file->get_32();
	r_header.height = p_file->get_32();
	r_header.mipmaps = p_file->get_32();
	r_header.flags = p_file->get_32();
	r_header.page_format = PageFormat(p_file->get_32());
	ERR_FAIL_COND_V(r_header.width < uint32_t(PAGE_SIZE) || r_header.height < uint32_t(PAGE_SIZE) || r_header.width > MAX_SIZE || r_header.height > MAX_SIZE, ERR_FILE_CORRUPT);
	ERR_FAIL_COND_V(!Math::is_power_of_2(r_header.width) || !Math::is_power_of_2(r_header.height), ERR_FILE_CORRUPT);
	ERR_FAIL_COND_V(r_header.mipmaps != page_level_count(r_header.width, r_header.height), ERR_FILE_CORRUPT);
	ERR_FAIL_COND_V(r_header.page_format > PAGE_FORMAT_UNCOMPRESSED, ERR_FILE_CORRUPT);

	const uint32_t fallback_size = p_file->get_32();
	Vector<uint8_t> fallback_data;
	fallback_data.resize(fallback_size);
	p_file->get_buffer(fallback_data.ptrw(), fallback_size);
	ERR_FAIL_NULL_V(Image::png_unpacker, ERR_UNAVAILABLE);
	r_header.fallback = Image::png_unpacker(fallback_data);
	ERR_FAIL_COND_V(r_header.fallback.is_null() || r_header.fallback->is_empty(), ERR_FILE_CORRUPT);
	if (r_header.fallback->get_format() != Image::FORMAT_RGBA8) {
		r_header.fallback->convert(Image::FORMAT_RGBA8);
	}
	r_header.fallback->generate_mipmaps();

	const uint32_t page_count = p_file->get_32();
	uint32_t expected = 0;
	for (uint32_t i = 0; i < r_header.mipmaps; i++) {
		expected += ((r_header.width / PAGE_SIZE) >> i) * ((r_header.height / PAGE_SIZE) >> i);
	}
	ERR_FAIL_COND_V(page_count != expected, ERR_FILE_CORRUPT);
	r_header.page_offsets.resize(page_count);
	r_header.page_sizes.resize(page_count);
	for (uint32_t i = 0; i < page_count; i++) {
		r_header.page_offsets[i] = p_file->get_64();
		r_header.page_sizes[i] = p_file->get_32();
	}
	ERR_FAIL_COND_V(p_file->eof_reached(), ERR_FILE_CORRUPT);
	return OK;
}

Ref<Image> VirtualTexture2D::decode_page(const Vector<uint8_t> &p_data, PageFormat p_page_format) {
	Ref<Image> page;
	if (p_page_format == PAGE_FORMAT_UNCOMPRESSED) {
		Vector<uint8_t> texels;
		texels.resize(TILE_SIZE * TILE_SIZE * 4);
		const int64_t size = Compression::decompress(texels.ptrw(), texels.size(), p_data.ptr(), p_data.size(), Compression::MODE_ZSTD);
		ERR_FAIL_COND_V(size != texels.size(), Ref<Image>());
		page = Image::create_from_data(TILE_SIZE, TILE_SIZE, false, Image::FORMAT_RGBA8, texels);
	} else {
		ERR_FAIL_NULL_V_MSG(Image::webp_unpacker, Ref<Image>(), "Virtual texture pages are WebP compressed, but this build has no WebP support.");
		page = Image::webp_unpacker(p_data);
		ERR_FAIL_COND_V(page.is_null() || page->is_empty(), Ref<Image>());
		if (page->get_format() != Image::FORMAT_RGBA8) {
			page->convert(Image::FORMAT_RGBA8);
		}
	}
	ERR_FAIL_COND_V(page->get_width() != TILE_SIZE || page->get_height() != TILE_SIZE, Ref<Image>());
	return page;
}

Error VirtualTexture2D::save_to_file(const String &p_path, const Ref<Image> &p_image, PageFormat p_page_format, float p_lossy_quality, bool p_wrap_borders, bool p_normal_map) {
	ERR_FAIL_COND_V(p_image.is_null() || p_image->is_empty(), ERR_INVALID_PARAMETER);
	const uint32_t width = p_image->get_width();
	const uint32_t height = p_image->get_height();
	ERR_FAIL_COND_V_MSG(width < uint32_t(PAGE_SIZE) || height < uint32_t(PAGE_SIZE) || !Math::is_power_of_2(width) || !Math::is_power_of_2(height) || width > MAX_SIZE || height > MAX_SIZE, ERR_INVALID_PARAMETER,
			vformat("A virtual texture's sides must be powers of two from %d to %d texels.", PAGE_SIZE, MAX_SIZE));

	PageFormat page_format = p_page_format;
	if (page_format != PAGE_FORMAT_UNCOMPRESSED && (!Image::webp_lossless_packer || !Image::webp_lossy_packer)) {
		WARN_PRINT("This build has no WebP support: virtual texture pages are stored uncompressed.");
		page_format = PAGE_FORMAT_UNCOMPRESSED;
	}
	ERR_FAIL_NULL_V(Image::png_packer, ERR_UNAVAILABLE);

	Ref<Image> level = p_image->duplicate();
	if (level->is_compressed()) {
		level->decompress();
	}
	level->clear_mipmaps();
	if (level->get_format() != Image::FORMAT_RGBA8) {
		level->convert(Image::FORMAT_RGBA8);
	}

	uint32_t flags = 0;
	if (level->detect_alpha() != Image::ALPHA_NONE) {
		flags |= FLAG_HAS_ALPHA;
	}

	const uint32_t mipmaps = page_level_count(width, height);
	Vector<Vector<uint8_t>> pages;
	for (uint32_t mip = 0; mip < mipmaps; mip++) {
		if (mip > 0) {
			// Exactly half the size: bilinear reads the four texels each new one covers.
			level->resize(level->get_width() / 2, level->get_height() / 2, Image::INTERPOLATE_BILINEAR);
			if (p_normal_map) {
				renormalize(level);
			}
		}
		EncodeJob job;
		job.level = level;
		job.pages_x = level->get_width() / PAGE_SIZE;
		job.wrap = p_wrap_borders;
		job.page_format = page_format;
		job.lossy_quality = p_lossy_quality;
		const uint32_t count = job.pages_x * (level->get_height() / PAGE_SIZE);
		job.encoded.resize(count);
		WorkerThreadPool::GroupID group = WorkerThreadPool::get_singleton()->add_native_group_task(&encode_page, &job, count, -1, true, "Encode virtual texture pages");
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(group);
		for (uint32_t i = 0; i < count; i++) {
			ERR_FAIL_COND_V_MSG(job.encoded[i].is_empty(), ERR_CANT_CREATE, "Could not encode a virtual texture page.");
		}
		pages.append_array(job.encoded);
	}

	// A small copy of the whole texture, for wherever it is not sampled as a virtual texture.
	Ref<Image> fallback = p_image->duplicate();
	if (fallback->is_compressed()) {
		fallback->decompress();
	}
	fallback->clear_mipmaps();
	fallback->convert(Image::FORMAT_RGBA8);
	const float fallback_scale = MIN(1.0f, float(FALLBACK_SIZE) / float(MAX(width, height)));
	fallback->resize(MAX(1, int(width * fallback_scale)), MAX(1, int(height * fallback_scale)), Image::INTERPOLATE_LANCZOS);
	const Vector<uint8_t> fallback_data = Image::png_packer(fallback);
	ERR_FAIL_COND_V(fallback_data.is_empty(), ERR_CANT_CREATE);

	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::WRITE);
	ERR_FAIL_COND_V_MSG(f.is_null(), ERR_CANT_OPEN, vformat("Cannot write virtual texture file '%s'.", p_path));
	f->store_buffer(reinterpret_cast<const uint8_t *>(FILE_MAGIC), 4);
	f->store_32(FORMAT_VERSION);
	f->store_32(width);
	f->store_32(height);
	f->store_32(mipmaps);
	f->store_32(flags);
	f->store_32(page_format);
	f->store_32(fallback_data.size());
	f->store_buffer(fallback_data.ptr(), fallback_data.size());
	f->store_32(pages.size());

	uint64_t offset = f->get_position() + uint64_t(pages.size()) * (sizeof(uint64_t) + sizeof(uint32_t));
	for (const Vector<uint8_t> &page : pages) {
		f->store_64(offset);
		f->store_32(page.size());
		offset += page.size();
	}
	for (const Vector<uint8_t> &page : pages) {
		f->store_buffer(page.ptr(), page.size());
	}
	return f->get_error() == OK || f->get_error() == ERR_FILE_EOF ? OK : ERR_FILE_CANT_WRITE;
}

/* STREAMING */

void VirtualTexture2D::_streamer_thread_func(void *p_userdata) {
	PageStreamer *s = static_cast<PageStreamer *>(p_userdata);
	while (true) {
		s->semaphore.wait();
		if (s->exit.is_set()) {
			break;
		}

		PageStreamer::Request request;
		Ref<VirtualTexturePageStream> page_stream;
		{
			MutexLock lock(s->mutex);
			if (s->queue_head >= s->queue.size()) {
				continue;
			}
			request = s->queue[s->queue_head++];
			if (s->queue_head >= s->queue.size()) {
				s->queue.clear();
				s->queue_head = 0;
			} else if (s->queue_head > 1024) {
				LocalVector<PageStreamer::Request> remaining;
				for (uint32_t i = s->queue_head; i < s->queue.size(); i++) {
					remaining.push_back(s->queue[i]);
				}
				s->queue = remaining;
				s->queue_head = 0;
			}
			Ref<VirtualTexturePageStream> *found = s->streams.getptr(request.stream);
			if (found) {
				page_stream = *found;
			}
		}
		if (page_stream.is_null()) {
			continue;
		}

		// An empty image tells the renderer to stop waiting for a page that could not be read.
		Ref<Image> page = page_stream->read_page(request.mipmap, request.x, request.y);
		RS::get_singleton()->texture_virtual_update_page(page_stream->texture, request.mipmap, request.x, request.y, page);
	}
}

void VirtualTexture2D::_request_pages(const PackedInt32Array &p_pages, int p_stream) {
	MutexLock global_lock(streamer_mutex);
	if (!streamer) {
		return;
	}
	MutexLock lock(streamer->mutex);
	if (!streamer->streams.has(p_stream)) {
		return;
	}
	for (int i = 0; i + 2 < p_pages.size(); i += 3) {
		PageStreamer::Request request;
		request.stream = p_stream;
		request.mipmap = p_pages[i];
		request.x = p_pages[i + 1];
		request.y = p_pages[i + 2];
		streamer->queue.push_back(request);
		streamer->semaphore.post();
	}
}

void VirtualTexture2D::finish() {
	MutexLock lock(streamer_mutex);
	stop_streamer_locked();
}

/* TEXTURE */

void VirtualTexture2D::_clear() {
	if (stream != 0) {
		MutexLock global_lock(streamer_mutex);
		if (streamer) {
			bool empty = false;
			{
				MutexLock lock(streamer->mutex);
				Ref<VirtualTexturePageStream> *found = streamer->streams.getptr(stream);
				if (found) {
					MutexLock stream_lock((*found)->mutex);
					(*found)->file.unref();
				}
				streamer->streams.erase(stream);
				empty = streamer->streams.is_empty();
			}
			// No virtual texture is left to stream: the thread goes until one is loaded again.
			if (empty) {
				stop_streamer_locked();
			}
		}
		stream = 0;
	}
}

Error VirtualTexture2D::load(const String &p_path) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
	ERR_FAIL_COND_V_MSG(f.is_null(), ERR_CANT_OPEN, vformat("Unable to open virtual texture file '%s'.", p_path));
	Header new_header;
	Error err = read_header(f, new_header);
	ERR_FAIL_COND_V(err != OK, err);

	_clear();
	header = new_header;
	alpha_cache.unref();

	RID new_texture = RS::get_singleton()->texture_virtual_create(header.width, header.height, RSE::VIRTUAL_TEXTURE_STREAMED, header.fallback);
	if (texture.is_valid()) {
		RS::get_singleton()->texture_replace(texture, new_texture);
	} else {
		texture = new_texture;
	}
	RS::get_singleton()->texture_set_path(texture, p_path);

	Ref<VirtualTexturePageStream> page_stream;
	page_stream.instantiate();
	page_stream->file = f;
	page_stream->header = header;
	page_stream->texture = texture;
	{
		MutexLock global_lock(streamer_mutex);
		if (!streamer) {
			streamer = memnew(PageStreamer);
			streamer->thread.start(&VirtualTexture2D::_streamer_thread_func, streamer);
		}
		MutexLock lock(streamer->mutex);
		stream = streamer->next_stream++;
		streamer->streams[stream] = page_stream;
	}
	RS::get_singleton()->texture_virtual_set_page_request_callback(texture, callable_mp_static(&VirtualTexture2D::_request_pages).bind(stream));

	path_to_file = p_path;
	notify_property_list_changed();
	emit_changed();
	return OK;
}

String VirtualTexture2D::get_load_path() const {
	return path_to_file;
}

void VirtualTexture2D::reload_from_file() {
	String path = get_path();
	if (!path.is_resource_file()) {
		return;
	}
	path = ResourceLoader::path_remap(path); // Remap for translation.
	path = ResourceLoader::import_remap(path); // Remap for import.
	if (!path.is_resource_file()) {
		return;
	}
	load(path);
}

Ref<Image> VirtualTexture2D::_read_page(int p_mipmap, int p_x, int p_y) const {
	Ref<VirtualTexturePageStream> page_stream;
	{
		MutexLock global_lock(streamer_mutex);
		if (streamer) {
			MutexLock lock(streamer->mutex);
			Ref<VirtualTexturePageStream> *found = streamer->streams.getptr(stream);
			if (found) {
				page_stream = *found;
			}
		}
	}
	ERR_FAIL_COND_V(page_stream.is_null(), Ref<Image>());
	return page_stream->read_page(p_mipmap, p_x, p_y);
}

Image::Format VirtualTexture2D::get_format() const {
	return Image::FORMAT_RGBA8;
}

int VirtualTexture2D::get_width() const {
	return header.width;
}

int VirtualTexture2D::get_height() const {
	return header.height;
}

RID VirtualTexture2D::get_rid() const {
	if (!texture.is_valid()) {
		texture = RS::get_singleton()->texture_2d_placeholder_create();
	}
	return texture;
}

bool VirtualTexture2D::has_alpha() const {
	return header.flags & FLAG_HAS_ALPHA;
}

bool VirtualTexture2D::is_pixel_opaque(int p_x, int p_y) const {
	if (alpha_cache.is_null() && header.fallback.is_valid()) {
		Ref<Image> img = header.fallback->duplicate();
		img->clear_mipmaps();
		alpha_cache.instantiate();
		alpha_cache->create_from_image_alpha(img);
	}
	if (alpha_cache.is_valid() && header.width > 0 && header.height > 0) {
		const int aw = int(alpha_cache->get_size().width);
		const int ah = int(alpha_cache->get_size().height);
		if (aw == 0 || ah == 0) {
			return true;
		}
		const int x = CLAMP(p_x * aw / int(header.width), 0, aw - 1);
		const int y = CLAMP(p_y * ah / int(header.height), 0, ah - 1);
		return alpha_cache->get_bit(x, y);
	}
	return true;
}

Ref<Image> VirtualTexture2D::get_image() const {
	if (header.width == 0 || stream == 0) {
		return Ref<Image>();
	}
	Ref<Image> image = Image::create_empty(header.width, header.height, false, Image::FORMAT_RGBA8);
	const int pages_x = header.width / PAGE_SIZE;
	const int pages_y = header.height / PAGE_SIZE;
	for (int y = 0; y < pages_y; y++) {
		for (int x = 0; x < pages_x; x++) {
			Ref<Image> page = _read_page(0, x, y);
			ERR_FAIL_COND_V(page.is_null(), Ref<Image>());
			image->blit_rect(page, Rect2i(PAGE_BORDER, PAGE_BORDER, PAGE_SIZE, PAGE_SIZE), Point2i(x * PAGE_SIZE, y * PAGE_SIZE));
		}
	}
	return image;
}

Ref<Image> VirtualTexture2D::get_fallback_image() const {
	if (header.fallback.is_null()) {
		return Ref<Image>();
	}
	return header.fallback->duplicate();
}

int VirtualTexture2D::get_page_mipmap_count() const {
	return header.mipmaps;
}

Vector2i VirtualTexture2D::get_page_count(int p_mipmap) const {
	ERR_FAIL_INDEX_V(p_mipmap, int(header.mipmaps), Vector2i());
	return Vector2i((header.width / PAGE_SIZE) >> p_mipmap, (header.height / PAGE_SIZE) >> p_mipmap);
}

Ref<Image> VirtualTexture2D::get_page_image(int p_mipmap, int p_x, int p_y) const {
	ERR_FAIL_COND_V(header.get_page_index(p_mipmap, p_x, p_y) == UINT32_MAX, Ref<Image>());
	return _read_page(p_mipmap, p_x, p_y);
}

VirtualTexture2D::PageFormat VirtualTexture2D::get_page_format() const {
	return header.page_format;
}

void VirtualTexture2D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load", "path"), &VirtualTexture2D::load);
	ClassDB::bind_method(D_METHOD("get_load_path"), &VirtualTexture2D::get_load_path);
	ClassDB::bind_method(D_METHOD("get_fallback_image"), &VirtualTexture2D::get_fallback_image);
	ClassDB::bind_method(D_METHOD("get_page_mipmap_count"), &VirtualTexture2D::get_page_mipmap_count);
	ClassDB::bind_method(D_METHOD("get_page_count", "mipmap"), &VirtualTexture2D::get_page_count);
	ClassDB::bind_method(D_METHOD("get_page_image", "mipmap", "x", "y"), &VirtualTexture2D::get_page_image);
	ClassDB::bind_method(D_METHOD("get_page_format"), &VirtualTexture2D::get_page_format);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "load_path", PROPERTY_HINT_FILE, "*.vtex"), "load", "get_load_path");

	BIND_ENUM_CONSTANT(PAGE_FORMAT_LOSSLESS);
	BIND_ENUM_CONSTANT(PAGE_FORMAT_LOSSY);
	BIND_ENUM_CONSTANT(PAGE_FORMAT_UNCOMPRESSED);
}

VirtualTexture2D::VirtualTexture2D() {}

VirtualTexture2D::~VirtualTexture2D() {
	_clear();
	if (texture.is_valid()) {
		ERR_FAIL_NULL(RenderingServer::get_singleton());
		RS::get_singleton()->free_rid(texture);
	}
}

/* LOADER */

Ref<Resource> ResourceFormatLoaderVirtualTexture2D::load(const String &p_path, const String &p_original_path, Error *r_error, bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) {
	Ref<VirtualTexture2D> texture;
	texture.instantiate();
	Error err = texture->load(p_path);
	if (r_error) {
		*r_error = err;
	}
	if (err != OK) {
		return Ref<Resource>();
	}
	return texture;
}

void ResourceFormatLoaderVirtualTexture2D::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back("vtex");
}

bool ResourceFormatLoaderVirtualTexture2D::handles_type(const String &p_type) const {
	return p_type == "VirtualTexture2D";
}

String ResourceFormatLoaderVirtualTexture2D::get_resource_type(const String &p_path) const {
	if (p_path.has_extension("vtex")) {
		return "VirtualTexture2D";
	}
	return "";
}
