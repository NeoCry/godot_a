/**************************************************************************/
/*  gtao.h                                                               */
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

// Self-contained Ground-Truth Ambient Occlusion (GTAO) effect: Jimenez, Wu, Pesce & Jarabo, "Practical
// Realtime Strategies for Accurate Indirect Occlusion" (Activision, 2016). Deliberately independent of
// SSEffects (servers/rendering/renderer_rd/effects/ss_effects.h/.cpp): its own depth downsampler, its own
// horizon-search gather, its own spatial+temporal denoiser, its own upsample — nothing shared with SSIL/SSR
// beyond generic renderer plumbing (the normal/depth G-buffers, camera reprojection matrices) that every
// screen-space effect needs regardless of algorithm.

#include "servers/rendering/renderer_rd/pipeline_deferred_rd.h"
#include "servers/rendering/renderer_rd/shaders/effects/gtao.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/gtao_downsample.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/gtao_temporal.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/gtao_upscale.glsl.gen.h"
#include "servers/rendering/renderer_scene_render.h"

#define RB_SCOPE_GTAO SNAME("rb_gtao")

#define RB_GTAO_DEPTH SNAME("depth")
#define RB_GTAO_RAW SNAME("raw")
#define RB_GTAO_HISTORY_A SNAME("history_a")
#define RB_GTAO_HISTORY_B SNAME("history_b")
#define RB_GTAO_FINAL SNAME("final")

class RenderSceneBuffersRD;

namespace RendererRD {

class GTAO {
private:
	static GTAO *singleton;

public:
	static GTAO *get_singleton() { return singleton; }

	GTAO();
	~GTAO();

	void set_quality(RSE::EnvironmentGTAOQuality p_quality, bool p_half_size, float p_fadeout_from, float p_fadeout_to);

	struct RenderBuffers {
		bool half_size = true;
		int buffer_width = 0;
		int buffer_height = 0;
		uint32_t mip_count = 1;

		// Per-view: generate() is called once per view per frame, so a single shared counter would advance
		// twice as fast (and pick a different ping-pong slot) for the second eye in a multiview/VR frame,
		// leaving one eye's history permanently stale. Frame-to-frame slice rotation and history ping-pong
		// both key off each view's own counter instead.
		uint32_t frame_index[RendererSceneRender::MAX_RENDER_VIEWS] = {};
		bool history_valid[RendererSceneRender::MAX_RENDER_VIEWS] = {};
		Projection last_frame_projections[RendererSceneRender::MAX_RENDER_VIEWS];
		Transform3D last_frame_transform;
	};

	struct Settings {
		float radius = 0.5;
		float intensity = 1.0;
		float power = 1.0;
		float horizon = 0.06;
		float sharpness = 0.9;

		Size2i full_screen_size;
	};

	void allocate_buffers(Ref<RenderSceneBuffersRD> p_render_buffers, RenderBuffers &p_gtao_buffers, const Settings &p_settings);
	void generate(Ref<RenderSceneBuffersRD> p_render_buffers, RenderBuffers &p_gtao_buffers, uint32_t p_view, RID p_normal_buffer, const Projection &p_projection, const Transform3D &p_cam_transform, const Settings &p_settings);

private:
	RSE::EnvironmentGTAOQuality quality = RSE::ENV_GTAO_QUALITY_MEDIUM;
	bool half_size = true;
	float fadeout_from = 50.0;
	float fadeout_to = 300.0;

	// Depth (and depth-derived) reads use nearest filtering throughout, deliberately: bilinear-filtered
	// depth blends foreground and background values right at the silhouette edges the horizon search cares
	// about most, fabricating an intermediate depth that doesn't correspond to any real surface. The
	// temporal history (already-denoised, reprojected to a sub-texel position) is the one place linear
	// filtering is actually wanted.
	RID nearest_sampler;
	RID linear_sampler;

	struct DownsamplePushConstant {
		float pixel_size[2];
		uint32_t is_orthogonal;
		// Only meaningful for DOWNSAMPLE_BASE: whether dest is a genuine half-res reduction of source_depth
		// (half_size enabled) or a same-size copy of it (half_size disabled, working_size == full_size).
		uint32_t half_size;

		float depth_linearize_mul;
		float depth_linearize_add;
		float pad2[2];
	};

	enum DownsampleMode {
		DOWNSAMPLE_BASE,
		DOWNSAMPLE_MIP,
		DOWNSAMPLE_MAX
	};

	struct {
		DownsamplePushConstant push_constant;
		GtaoDownsampleShaderRD shader;
		RID shader_version;
		PipelineDeferredRD pipelines[DOWNSAMPLE_MAX];
	} downsample;

	struct GatherPushConstant {
		int32_t screen_size[2];
		float NDC_to_view_mul_x;
		float NDC_to_view_mul_y;

		float NDC_to_view_add_x;
		float NDC_to_view_add_y;
		uint32_t is_orthogonal;
		int32_t quality;

		float radius;
		float horizon_bias;
		uint32_t frame_index;
		uint32_t mip_count;

		int32_t full_screen_size[2];
		float depth_texture_pixel_size[2];
		float thin_occluder_compensation;
		float pad;
	};

	struct {
		GatherPushConstant push_constant;
		GtaoShaderRD shader;
		RID shader_version;
		PipelineDeferredRD pipeline;
	} gather;

	struct TemporalPushConstant {
		int32_t screen_size[2];
		float pixel_size[2];

		float z_near;
		float z_far;
		float history_weight;
		uint32_t history_is_valid;

		float sharpness;
		float pad[3];
	};

	struct {
		TemporalPushConstant push_constant;
		GtaoTemporalShaderRD shader;
		RID shader_version;
		PipelineDeferredRD pipeline;
		RID reprojection_uniform_buffer;
	} temporal;

	struct UpscalePushConstant {
		int32_t full_screen_size[2];
		int32_t half_screen_size[2];

		uint32_t is_orthogonal;
		float depth_linearize_mul;
		float depth_linearize_add;
		float intensity;

		float power;
		float fade_out_mul;
		float fade_out_add;
		float sharpness;
	};

	struct {
		UpscalePushConstant push_constant;
		GtaoUpscaleShaderRD shader;
		RID shader_version;
		PipelineDeferredRD pipeline;
	} upscale;
};

} // namespace RendererRD
