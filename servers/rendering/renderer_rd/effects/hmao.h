/**************************************************************************/
/*  hmao.h                                                                */
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

// Height Map based Ambient Occlusion, modeled on CRYENGINE's effect of the same name: a top-down depth map of
// the world around the viewer, rendered once per frame, is gathered in world space into a large scale ambient
// occlusion term. It is the long range counterpart to GTAO (effects/gtao.h) - GTAO resolves creases and
// contact darkening within a few pixels of the shading point, this resolves valleys, cliffs, canyons and
// courtyards, hundreds of metres across, including from geometry that is nowhere near the screen. The two are
// independent and meant to be used together; the lighting shader takes whichever of them occludes more.
//
// This class owns both halves of the effect: the height map itself (one shared, world space texture, since it
// is a property of the world around a camera rather than of any one render buffer) and the per-view screen
// space gather that turns it into the ambient occlusion buffer the lighting pass samples.

#include "servers/rendering/renderer_rd/pipeline_deferred_rd.h"
#include "servers/rendering/renderer_rd/shaders/effects/hmao.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/hmao_upscale.glsl.gen.h"
#include "servers/rendering/renderer_scene_render.h"

#define RB_SCOPE_HMAO SNAME("rb_hmao")

#define RB_HMAO_GATHER SNAME("gather")
#define RB_HMAO_FINAL SNAME("final")

class RenderSceneBuffersRD;

namespace RendererRD {

class HeightMapAO {
private:
	static HeightMapAO *singleton;

public:
	static HeightMapAO *get_singleton() { return singleton; }

	HeightMapAO();
	~HeightMapAO();

	void set_quality(RSE::EnvironmentHMAOQuality p_quality, bool p_half_size);

	/* Height map */

	// Creates (or reuses) the depth-only render target the top-down pass draws into, and returns its
	// framebuffer. The renderer is expected to follow this up with a matching height_map_rendered() call.
	RID prepare_height_map(uint32_t p_resolution);

	// Records the world space box the map that was just rendered covers: x/z are its footprint, y the depth
	// range of the orthographic projection, from the far (bottom) plane up to the near (top) one.
	void height_map_rendered(const AABB &p_bounds);
	bool is_height_map_valid() const { return height_map.valid && height_map.texture.is_valid(); }
	// Dropped whenever a frame doesn't produce a map, so the gather can never dress up a stale one (from
	// another viewport, or from before the camera teleported) as this frame's occlusion.
	void invalidate_height_map() { height_map.valid = false; }

	// Called once per frame. The height map is the only thing this effect keeps alive between frames, and
	// nothing tells it when the last viewport using it switched the effect off - so it hands the texture
	// back itself once no frame has drawn into it for a while, and prepare_height_map() makes a new one if
	// the effect ever comes back. The delay is what keeps a viewport that simply didn't redraw, or a camera
	// standing somewhere with nothing around it, from freeing and recreating the map every frame.
	void frame_update();

	/* Screen space gather */

	struct RenderBuffers {
		bool half_size = true;
		int buffer_width = 0;
		int buffer_height = 0;
	};

	struct Settings {
		float amount = 1.0;

		Size2i full_screen_size;
	};

	void allocate_buffers(Ref<RenderSceneBuffersRD> p_render_buffers, RenderBuffers &p_hmao_buffers, const Settings &p_settings);
	void generate(Ref<RenderSceneBuffersRD> p_render_buffers, RenderBuffers &p_hmao_buffers, uint32_t p_view, RID p_normal_buffer, const Projection &p_projection, const Transform3D &p_cam_transform, const Settings &p_settings);

private:
	RSE::EnvironmentHMAOQuality quality = RSE::ENV_HMAO_QUALITY_MEDIUM;
	bool half_size = true;

	// Depth reads are all nearest: linear filtering of a depth format is optional in Vulkan, and blending
	// two heights (or two view depths) across a silhouette invents a surface that isn't there either way.
	// The height map additionally clamps to a transparent black border, which under reverse Z is the far
	// (lowest) plane, so sampling outside the map reads as "nothing above you here".
	RID nearest_sampler;
	RID border_sampler;

	// Frames without a single map render after which the texture is released, see frame_update().
	static constexpr uint32_t HEIGHT_MAP_IDLE_FRAMES = 120;

	struct {
		RID texture;
		RID framebuffer;
		uint32_t resolution = 0;
		AABB bounds;
		bool valid = false;
		bool rendered_since_update = false;
		uint32_t idle_frames = 0;
	} height_map;

	struct GatherPushConstant {
		float cam_basis_x[4];
		float cam_basis_y[4];
		float cam_basis_z[4];

		int32_t screen_size[2];
		int32_t full_screen_size[2];

		float NDC_to_view_mul_x;
		float NDC_to_view_mul_y;
		float NDC_to_view_add_x;
		float NDC_to_view_add_y;

		float depth_linearize_mul;
		float depth_linearize_add;
		uint32_t flags;
		float amount;

		float map_origin[2];
		float map_size;
		float map_bottom;

		float map_height;
		float radius;
		float bias;
		float edge_fade;
	};

	static_assert(sizeof(GatherPushConstant) <= 128, "Push constants are only guaranteed up to 128 bytes.");

	struct {
		GatherPushConstant push_constant;
		HmaoShaderRD shader;
		RID shader_version;
		PipelineDeferredRD pipeline;
	} gather;

	struct UpscalePushConstant {
		int32_t full_screen_size[2];
		int32_t source_size[2];

		uint32_t is_orthogonal;
		float depth_linearize_mul;
		float depth_linearize_add;
		int32_t blur_radius;
	};

	struct {
		UpscalePushConstant push_constant;
		HmaoUpscaleShaderRD shader;
		RID shader_version;
		PipelineDeferredRD pipeline;
	} upscale;

	void _free_height_map();
};

} // namespace RendererRD
