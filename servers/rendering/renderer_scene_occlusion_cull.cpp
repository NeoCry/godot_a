/**************************************************************************/
/*  renderer_scene_occlusion_cull.cpp                                     */
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

#include "renderer_scene_occlusion_cull.h"

#include "core/config/engine.h"
#include "servers/rendering/rendering_server.h"

RendererSceneOcclusionCull *RendererSceneOcclusionCull::singleton = nullptr;

bool RendererSceneOcclusionCull::HZBuffer::occlusion_jitter_enabled = false;

void RendererSceneOcclusionCull::HZBuffer::clear() {
	if (sizes.is_empty()) {
		return; // Already cleared
	}

	data.clear();
	sizes.clear();
	mips.clear();

	debug_data.clear();
	if (debug_image.is_valid()) {
		debug_image.unref();
	}

	debug_pyramid_data.clear();
	debug_pyramid_size = Size2i();
	if (debug_pyramid_image.is_valid()) {
		debug_pyramid_image.unref();
	}

	ERR_FAIL_NULL(RenderingServer::get_singleton());
	RS::get_singleton()->free_rid(debug_texture);
	if (debug_pyramid_texture.is_valid()) {
		RS::get_singleton()->free_rid(debug_pyramid_texture);
		debug_pyramid_texture = RID();
	}
}

void RendererSceneOcclusionCull::HZBuffer::resize(const Size2i &p_size) {
	occlusion_buffer_size = p_size;

	if (p_size == Size2i()) {
		clear();
		return;
	}

	if (!sizes.is_empty() && p_size == sizes[0]) {
		return; // Size didn't change
	}

	int mip_count = 0;
	int data_size = 0;
	int w = p_size.x;
	int h = p_size.y;

	while (true) {
		data_size += h * w;

		w = MAX(1, w >> 1);
		h = MAX(1, h >> 1);

		mip_count++;

		if (w == 1U && h == 1U) {
			data_size += 1U;
			mip_count++;
			break;
		}
	}

	data.resize(data_size);
	mips.resize(mip_count);
	sizes.resize(mip_count);

	w = p_size.x;
	h = p_size.y;
	float *ptr = data.ptr();

	for (int i = 0; i < mip_count; i++) {
		sizes[i] = Size2i(w, h);
		mips[i] = ptr;

		ptr = &ptr[w * h];
		w = MAX(1, w >> 1);
		h = MAX(1, h >> 1);
	}

	for (int i = 0; i < data_size; i++) {
		data[i] = FLT_MAX;
	}

	debug_data.resize(sizes[0].x * sizes[0].y);
	if (debug_texture.is_valid()) {
		RS::get_singleton()->free_rid(debug_texture);
		debug_texture = RID();
	}

	// Room for the finest level plus the column of coarser ones, padded to the
	// buffer's own aspect ratio: the debug view stretches whatever it is given
	// across the whole viewport, and a canvas of a different shape would show
	// up as a distorted buffer.
	debug_pyramid_size = Size2i(sizes[0].x + 1 + MAX(1, sizes[0].x / 2), sizes[0].y + MAX(1, sizes[0].y / 2));
	debug_pyramid_data.resize(debug_pyramid_size.x * debug_pyramid_size.y);
	if (debug_pyramid_texture.is_valid()) {
		RS::get_singleton()->free_rid(debug_pyramid_texture);
		debug_pyramid_texture = RID();
	}
}

void RendererSceneOcclusionCull::HZBuffer::update_mips() {
	// Keep this up to date as a local to be used for occlusion timers.
	occlusion_frame = Engine::get_singleton()->get_frames_drawn();

	if (sizes.is_empty()) {
		return;
	}

	for (uint32_t mip = 1; mip < mips.size(); mip++) {
		for (int y = 0; y < sizes[mip].y; y++) {
			for (int x = 0; x < sizes[mip].x; x++) {
				int prev_x = x * 2;
				int prev_y = y * 2;

				int prev_w = sizes[mip - 1].width;
				int prev_h = sizes[mip - 1].height;

				bool odd_w = (prev_w % 2) != 0;
				bool odd_h = (prev_h % 2) != 0;

#define CHECK_OFFSET(xx, yy) max_depth = MAX(max_depth, mips[mip - 1][MIN(prev_h - 1, prev_y + (yy)) * prev_w + MIN(prev_w - 1, prev_x + (xx))])

				float max_depth = mips[mip - 1][prev_y * sizes[mip - 1].x + prev_x];
				CHECK_OFFSET(0, 1);
				CHECK_OFFSET(1, 0);
				CHECK_OFFSET(1, 1);

				if (odd_w) {
					CHECK_OFFSET(2, 0);
					CHECK_OFFSET(2, 1);
				}

				if (odd_h) {
					CHECK_OFFSET(0, 2);
					CHECK_OFFSET(1, 2);
				}

				if (odd_w && odd_h) {
					CHECK_OFFSET(2, 2);
				}

				mips[mip][y * sizes[mip].x + x] = max_depth;
#undef CHECK_OFFSET
			}
		}
	}
}

RID RendererSceneOcclusionCull::HZBuffer::get_debug_pyramid_texture() {
	if (sizes.is_empty() || sizes[0] == Size2i() || debug_pyramid_size == Size2i()) {
		return RID();
	}

	if (debug_pyramid_image.is_null()) {
		debug_pyramid_image.instantiate();
	}

	uint8_t *ptrw = debug_pyramid_data.ptrw();
	memset(ptrw, 0, debug_pyramid_data.size());

	// Laid out from the top down (row 0 of the buffer is the bottom of the
	// screen, so that is the high end here), with one texel of black between
	// levels so neighbouring ones stay readable as separate images even where
	// both are empty. What is left over is the padding that keeps the canvas
	// at the buffer's aspect ratio.
	int column_top = debug_pyramid_size.y;
	for (uint32_t mip = 0; mip < mips.size(); mip++) {
		const Size2i size = sizes[mip];
		const int origin_x = mip == 0 ? 0 : sizes[0].x + 1;
		int origin_y;
		if (mip == 0) {
			origin_y = debug_pyramid_size.y - size.y;
		} else {
			origin_y = column_top - size.y;
			column_top = origin_y - 1;
		}

		if (origin_y < 0 || origin_x + size.x > debug_pyramid_size.x) {
			break; // Out of room: the levels left are a handful of texels anyway.
		}

		for (int y = 0; y < size.y; y++) {
			const float *row = &mips[mip][y * size.x];
			uint8_t *dst = &ptrw[(origin_y + y) * debug_pyramid_size.x + origin_x];
			for (int x = 0; x < size.x; x++) {
				dst[x] = _depth_to_debug_value(row[x]);
			}
		}
	}

	debug_pyramid_image->set_data(debug_pyramid_size.x, debug_pyramid_size.y, false, Image::FORMAT_L8, debug_pyramid_data);

	if (debug_pyramid_texture.is_null()) {
		debug_pyramid_texture = RS::get_singleton()->texture_2d_create(debug_pyramid_image);
	} else {
		RS::get_singleton()->texture_2d_update(debug_pyramid_texture, debug_pyramid_image);
	}

	return debug_pyramid_texture;
}

RID RendererSceneOcclusionCull::HZBuffer::get_debug_texture() {
	if (sizes.is_empty() || sizes[0] == Size2i()) {
		return RID();
	}

	if (debug_image.is_null()) {
		debug_image.instantiate();
	}

	unsigned char *ptrw = debug_data.ptrw();
	for (int i = 0; i < debug_data.size(); i++) {
		ptrw[i] = _depth_to_debug_value(mips[0][i]);
	}

	debug_image->set_data(sizes[0].x, sizes[0].y, false, Image::FORMAT_L8, debug_data);

	if (debug_texture.is_null()) {
		debug_texture = RS::get_singleton()->texture_2d_create(debug_image);
	} else {
		RenderingServer::get_singleton()->texture_2d_update(debug_texture, debug_image);
	}

	return debug_texture;
}
