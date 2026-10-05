/**************************************************************************/
/*  planar_reflections.h                                                  */
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

#include "servers/rendering/renderer_rd/storage_rd/render_buffer_custom_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_scene_render.h"

class ClusterBuilderRD;
class ClusterBuilderSharedDataRD;

#define RB_SCOPE_PLANAR_REFLECTIONS SNAME("planar_reflections")

namespace RendererRD {

// The planar reflections a view shows (see RendererSceneRender::PlanarReflectionLayer), kept with
// its render buffers: one layer per plane in sight, drawn each frame before the view itself.
//
// Each layer is drawn like a reflection probe's face is, through the forward renderers' reduced
// path (no screen-space effects, GI buffers, post-processing or tonemapping), straight into a layer
// of a linear HDR texture array that keeps the mirrored camera's depth beside it, at the size of the
// view times the probe's resolution_scale. Then its mip chain is blurred, for rough surfaces. The
// view's own passes sample it from the planar_reflections uniform buffer and the two arrays, see
// planar_reflection_inc.glsl, and render buffers that show none of them bind empty stand-ins.
//
// Every layer shares the textures' size and the formats of reflection probes, so the pipelines
// compiled for those draw these too.
class PlanarReflections : public RenderBufferCustomDataRD {
	GDCLASS(PlanarReflections, RenderBufferCustomDataRD);

public:
	static constexpr uint32_t MAX_LAYERS = 4;
	// Mips of the color array, for blurring what rough surfaces reflect.
	static constexpr uint32_t MAX_MIPMAPS = 7;

	// Mirrored by PlanarReflectionData in planar_reflection_inc.glsl (std140). Everything spatial is
	// in the view space of the camera sampling it.
	struct LayerUBO {
		float view_to_receiver[16];
		float view_to_reflection[16];
		float reflection_to_view[16];
		// Normal in xyz, and w such that dot(xyz, p) + w is how far p is above the mirror.
		float plane[4];
		// The mirrored camera in xyz, and in w how many texels of the layer a meter spans one meter
		// in front of it.
		float camera[4];
		float intensity;
		float distortion;
		float normal_fade;
		float edge_fade;
		uint32_t reflection_mask;
		float max_lod;
		float pad[2];
	};

	struct UBO {
		LayerUBO layers[MAX_LAYERS];
		uint32_t count;
		uint32_t pad[3];
	};

private:
	Size2i size;
	uint32_t layer_capacity = 0;
	uint32_t count = 0;
	uint32_t mipmaps = 1;
	bool use_storage = false;

	RID color;
	RID depth;
	// Per layer: its mips (mip 0 first), its depth, and the framebuffers drawing into them.
	LocalVector<LocalVector<RID>> color_mips;
	LocalVector<RID> depth_slices;
	LocalVector<RID> framebuffers;
	LocalVector<RID> depth_framebuffers;

	Ref<RenderSceneBuffersRD> pass_buffers;
	ClusterBuilderRD *cluster_builder = nullptr;
	RendererSceneRender::PlanarReflectionLayer layers[MAX_LAYERS];
	RID uniform_buffer;

	static inline RID empty_uniform_buffer;

	void _free_textures();

public:
	virtual void configure(RenderSceneBuffersRD *p_render_buffers) override {}
	virtual void free_data() override;

	// Readies p_count layers p_size large, and returns the render buffers their passes are drawn
	// with.
	Ref<RenderSceneBuffersRD> begin(uint32_t p_count, const Size2i &p_size);
	void set_layer(uint32_t p_layer, const RendererSceneRender::PlanarReflectionLayer &p_data);
	const RendererSceneRender::PlanarReflectionLayer &get_layer(uint32_t p_layer) const;
	// Blurs the layer's mip chain, once it has been drawn.
	void finish_layer(uint32_t p_layer);
	void clear() { count = 0; }

	uint32_t get_count() const { return count; }
	Size2i get_size() const { return size; }
	RID get_framebuffer(uint32_t p_layer) const;
	RID get_depth_framebuffer(uint32_t p_layer) const;
	ClusterBuilderRD *get_cluster_builder(ClusterBuilderSharedDataRD *p_shared);

	RID get_color_texture() const { return color; }
	RID get_depth_texture() const { return depth; }
	// The uniform buffer the view's passes read, made out for its camera, once a frame before
	// they are drawn.
	RID update_uniform_buffer(const Transform3D &p_camera_transform);
	RID get_uniform_buffer() const { return uniform_buffer; }

	// What a view's passes bind at PLANAR_REFLECTION_BINDING and the two bindings after it: its own
	// planar reflections when it shows any, empty stand-ins otherwise.
	static void append_uniforms(LocalVector<RD::Uniform> &r_uniforms, uint32_t p_first_binding, const Ref<RenderSceneBuffersRD> &p_render_buffers, bool p_show);

	// What views without planar reflections bind: no layers.
	static RID get_empty_uniform_buffer();
	static void free_empty_uniform_buffer();

	~PlanarReflections();
};

} // namespace RendererRD
