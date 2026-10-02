/**************************************************************************/
/*  landscape_gpu.h                                                       */
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
#include "core/object/ref_counted.h"
#include "core/templates/safe_refcount.h"
#include "scene/3d/landscape_quadtree.h"

// Render thread state of a LandscapeGPUTexture: the RenderingDevice texture
// behind it. Only ever touched from queued calls, like FoliageCullResources.
class LandscapeTextureResources : public RefCounted {
	GDCLASS(LandscapeTextureResources, RefCounted);

	friend class LandscapeGPUTexture;

	RID rd_texture;

	void rt_create(RID p_texture, int p_format, int p_width, int p_height, int p_mipmaps, bool p_array, const Array &p_layers);
	void rt_update(int p_layer, const PackedInt32Array &p_regions, const PackedByteArray &p_data);
	void rt_free();
};

// A texture Landscape3D keeps in step with an Image it edits a region at a
// time: the heightmap while sculpting, the weight maps while painting. On
// RenderingDevice renderers it is a texture of Landscape3D's own that only the
// changed region (of every mipmap) is copied into, so that a brush stroke on a
// 4097 x 4097 terrain uploads a few kilobytes rather than the whole map every
// time. The Compatibility renderer has no partial texture updates, so there it
// is an ordinary texture that flush() uploads whole, at most once a frame.
class LandscapeGPUTexture {
	RID texture;
	Ref<LandscapeTextureResources> resources;
	Image::Format format = Image::FORMAT_MAX;
	int width = 0;
	int height = 0;
	int layer_count = 0;
	bool mipmaps = false;
	bool array = false;
	// Compatibility renderer: the latest image of every layer changed since
	// the last flush().
	LocalVector<Ref<Image>> pending_layers;

public:
	// Whether partial updates are available (RenderingDevice renderers).
	static bool supports_partial_updates();

	// Every image must share one size, format and mipmap setting, and an
	// array needs at least two. Only FORMAT_R8, FORMAT_RF, FORMAT_RGH and
	// FORMAT_RGBA8 are supported.
	void create(const Vector<Ref<Image>> &p_layers, bool p_array);
	// p_image holds the whole layer, already edited; p_region is the part of
	// it that changed, in pixels of its largest mipmap.
	void update(int p_layer, const Ref<Image> &p_image, const Rect2i &p_region);
	void flush();
	void free();

	RID get_rid() const { return texture; }
	bool is_valid() const { return texture.is_valid(); }
	int get_layer_count() const { return layer_count; }
	Size2i get_size() const { return Size2i(width, height); }

	LandscapeGPUTexture() = default;
	~LandscapeGPUTexture();
	LandscapeGPUTexture(const LandscapeGPUTexture &) = delete;
	LandscapeGPUTexture &operator=(const LandscapeGPUTexture &) = delete;
};

// Render thread state of a LandscapeGPUQuadtree.
class LandscapeGPUResources : public RefCounted {
	GDCLASS(LandscapeGPUResources, RefCounted);

	friend class LandscapeGPUQuadtree;

	RID traverse_shader;
	RID traverse_pipeline;
	RID emit_shader;
	RID emit_pipeline;
	RID apply_shader;
	RID apply_pipeline;

	RID params_buffer;
	RID node_buffer;
	uint32_t node_count = 0;
	// Nodes each list holds, and patches each MultiMesh does.
	uint32_t capacity = 0;
	RID list_buffer;
	RID final_buffer;
	RID counter_buffer;

	RID traverse_set;
	RID emit_set;
	RID emit_output_set;
	RID apply_set;
	RID apply_output_set;

	// Owned by the MultiMeshes.
	RID instance_buffers[2];
	RID command_buffers[2];

	// Set once the compute shaders failed to build, so that Landscape3D falls
	// back to selecting on the CPU.
	SafeFlag failed;
	// Set while the counters of a selection are being read back.
	SafeFlag counters_pending;
	// Raised when a selection did not fit the lists at the first attempt.
	SafeNumeric<uint32_t> wanted_capacity;

	bool _ensure_pipelines();
	bool _ensure_buffers();
	bool _ensure_sets();
	void _free_sets();
	void _free_all();

	void rt_set_nodes(const PackedFloat32Array &p_nodes);
	void rt_update_nodes(const PackedInt32Array &p_ranges, const PackedFloat32Array &p_data);
	void rt_set_outputs(RID p_main_multimesh, RID p_shadow_multimesh, uint32_t p_capacity);
	void rt_dispatch(const PackedByteArray &p_params, int p_top_level, int p_min_level);
	static void _rt_counters_read(const PackedByteArray &p_data, const Ref<LandscapeGPUResources> &p_resources, uint32_t p_capacity);
};

// Screen-space error LOD selection on the GPU, the compute counterpart of
// LandscapeQuadtree::select().
//
// The quadtree is walked top down one level per dispatch: every node of the
// level's list either appends its four children to the next level's list, or
// itself to the list of selected patches. A last pass then works out, for
// every selected patch, how much coarser each of its neighbors is (by
// re-evaluating the same split test down towards them, which is why it is
// written to give bit-identical answers wherever it runs), culls it against
// the frustum and the shadow distance, and writes it straight into the two
// MultiMeshes' instance buffers: one drawn by the camera, one only into
// shadow maps. A single thread finally copies both counts into the
// MultiMeshes' indirect draw commands, so how many patches are drawn is never
// known to (or sent from) the CPU.
//
// A selection that does not fit the lists is made again with a coarser LOD,
// within the same frame (see rt_dispatch()), and the counters are read back a
// few frames later to grow the lists when that happens.
//
// Like FoliageGPUCuller, every RenderingDevice call is queued onto the
// rendering thread; the methods below only hand it copies of their data.
class LandscapeGPUQuadtree {
	Ref<LandscapeGPUResources> resources;
	uint32_t capacity = INITIAL_CAPACITY;

	static void rt_free(const Ref<LandscapeGPUResources> &p_resources);

public:
	// Patches either output list holds to start with, and at most; also the
	// sizes of the MultiMeshes the lists are written into.
	static constexpr uint32_t INITIAL_CAPACITY = 16384;
	static constexpr uint32_t MAX_CAPACITY = 131072;

	// True if this renderer has compute shaders and indirect MultiMeshes.
	static bool is_supported();

	bool has_failed() const;

	void set_nodes(const LandscapeQuadtree &p_quadtree);
	void update_nodes(const LandscapeQuadtree &p_quadtree, const LocalVector<Vector2i> &p_ranges);
	// Both MultiMeshes must have been allocated for get_capacity() instances,
	// with colors, custom data and indirect drawing, and have their mesh set.
	void set_outputs(RID p_main_multimesh, RID p_shadow_multimesh);
	// Takes effect with the next set_outputs().
	void set_capacity(uint32_t p_capacity);
	uint32_t get_capacity() const { return capacity; }
	// More than get_capacity() once a selection has not fitted: the
	// MultiMeshes should then be allocated again for this many patches.
	uint32_t get_wanted_capacity() const;
	void dispatch(const LandscapeQuadtree &p_quadtree, const LandscapeQuadtree::SelectParams &p_params);
	void release();

	LandscapeGPUQuadtree() = default;
	~LandscapeGPUQuadtree();
	LandscapeGPUQuadtree(const LandscapeGPUQuadtree &) = delete;
	LandscapeGPUQuadtree &operator=(const LandscapeGPUQuadtree &) = delete;
};
