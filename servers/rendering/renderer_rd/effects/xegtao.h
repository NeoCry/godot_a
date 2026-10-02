/**************************************************************************/
/*  xegtao.h                                                              */
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

// Intel's XeGTAO (https://github.com/GameTechDev/XeGTAO, version 1.30), a ground-truth ambient occlusion
// implementation of Jimenez, Wu, Pesce & Jarabo, "Practical Realtime Strategies for Accurate Indirect
// Occlusion" (Activision, 2016), including its directional component (bent normals). The passes follow
// XeGTAO's own: a depth prefilter that builds a view space depth MIP chain, the main horizon search, and an
// edge-aware spatial denoiser run once per denoise level; plus a last pass of our own that resolves the
// result to full resolution (bilaterally, when running at half resolution) and applies the artistic
// intensity and distance fade out. Like XeGTAO, there is no temporal filter of its own: the noise pattern
// only changes from frame to frame when a temporal antialiasing pass is there to accumulate it.
//
// Outputs, both at full resolution:
// - RB_XEGTAO_FINAL: visibility (1 - occlusion), sampled by the lighting pass as ambient occlusion.
// - RB_XEGTAO_BENT_NORMAL: the view space bent normal (the average unoccluded direction) encoded as
//   normal * 0.5 + 0.5, used by the lighting pass for indirect diffuse lighting and specular occlusion.
//   Only allocated while bent normals are enabled.

#include "servers/rendering/renderer_rd/pipeline_deferred_rd.h"
#include "servers/rendering/renderer_rd/shaders/effects/xegtao.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/xegtao_apply.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/xegtao_denoise.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/xegtao_prefilter.glsl.gen.h"
#include "servers/rendering/renderer_scene_render.h"

#define RB_SCOPE_XEGTAO SNAME("rb_xegtao")

#define RB_XEGTAO_DEPTH SNAME("depth")
#define RB_XEGTAO_EDGES SNAME("edges")
#define RB_XEGTAO_TERM_A SNAME("term_a")
#define RB_XEGTAO_TERM_B SNAME("term_b")
#define RB_XEGTAO_FINAL SNAME("final")
#define RB_XEGTAO_BENT_NORMAL SNAME("bent_normal")

class RenderSceneBuffersRD;

namespace RendererRD {

class XeGTAO {
private:
	static XeGTAO *singleton;

public:
	static XeGTAO *get_singleton() { return singleton; }

	XeGTAO();
	~XeGTAO();

	void set_quality(RSE::EnvironmentXeGTAOQuality p_quality, int p_denoise_passes, bool p_half_size, float p_fadeout_from, float p_fadeout_to);

	struct RenderBuffers {
		bool half_size = false;
		bool bent_normals = false;
		int buffer_width = 0;
		int buffer_height = 0;
		uint32_t mip_count = 1;
		// Per view, so both eyes of a multiview frame step through the same noise sequence.
		uint32_t frame_index[RendererSceneRender::MAX_RENDER_VIEWS] = {};
	};

	struct Settings {
		float radius = 0.5;
		float intensity = 1.0;
		float power = 2.2;
		float falloff_range = 0.615;
		float sample_distribution_power = 2.0;
		float thin_occluder_compensation = 0.0;
		bool bent_normals = true;
		// Whether a temporal antialiasing pass will accumulate the result: only then does the noise pattern
		// change from frame to frame, since without one that would read as flicker.
		bool temporal_noise = false;

		Size2i full_screen_size;
	};

	void allocate_buffers(Ref<RenderSceneBuffersRD> p_render_buffers, RenderBuffers &p_xegtao_buffers, const Settings &p_settings);
	void generate(Ref<RenderSceneBuffersRD> p_render_buffers, RenderBuffers &p_xegtao_buffers, uint32_t p_view, RID p_normal_buffer, const Projection &p_projection, const Settings &p_settings);

private:
	RSE::EnvironmentXeGTAOQuality quality = RSE::ENV_XEGTAO_QUALITY_HIGH;
	int denoise_passes = 2;
	bool half_size = false;
	float fadeout_from = 50.0;
	float fadeout_to = 300.0;

	// Point filtering throughout, as XeGTAO requires: linear filtering would interpolate between
	// neighboring depths on the same MIP level, and the working AO term is a packed integer anyway.
	RID nearest_sampler;

	struct PrefilterPushConstant {
		int32_t source_size[2];
		int32_t working_size[2];

		float depth_linearize_mul;
		float depth_linearize_add;
		float z_near;
		float z_far;

		uint32_t is_orthogonal;
		int32_t source_scale;
		float mip_falloff_mul;
		float mip_falloff_add;

		int32_t mip_count;
		int32_t pad[3];
	};

	struct {
		PrefilterPushConstant push_constant;
		XegtaoPrefilterShaderRD shader;
		RID shader_version;
		PipelineDeferredRD pipeline;
	} prefilter;

	struct MainPushConstant {
		int32_t viewport_size[2];
		float viewport_pixel_size[2];

		float ndc_to_view_mul[2];
		float ndc_to_view_add[2];

		float ndc_to_view_mul_x_pixel_size[2];
		float effect_radius;
		float sample_distribution_power;

		float falloff_mul;
		float falloff_add;
		float thin_occluder_compensation;
		float final_value_power;

		float depth_mip_sampling_offset;
		float slice_count;
		float steps_per_slice;
		uint32_t noise_index;

		int32_t normal_buffer_size[2];
		int32_t normal_scale;
		uint32_t is_orthogonal;
	};

	static_assert(sizeof(MainPushConstant) <= 128, "Push constants are only guaranteed up to 128 bytes.");

	enum MainMode {
		MAIN_MODE_AO,
		MAIN_MODE_AO_BENT_NORMALS,
		MAIN_MODE_MAX
	};

	struct {
		MainPushConstant push_constant;
		XegtaoShaderRD shader;
		RID shader_version;
		PipelineDeferredRD pipelines[MAIN_MODE_MAX];
	} main_pass;

	struct DenoisePushConstant {
		int32_t viewport_size[2];
		float viewport_pixel_size[2];

		float blur_beta;
		uint32_t final_apply;
		float pad[2];
	};

	enum DenoiseMode {
		DENOISE_MODE_AO,
		DENOISE_MODE_AO_BENT_NORMALS,
		DENOISE_MODE_MAX
	};

	struct {
		DenoisePushConstant push_constant;
		XegtaoDenoiseShaderRD shader;
		RID shader_version;
		PipelineDeferredRD pipelines[DENOISE_MODE_MAX];
	} denoise;

	struct ApplyPushConstant {
		int32_t full_size[2];
		int32_t working_size[2];

		float depth_linearize_mul;
		float depth_linearize_add;
		float z_near;
		float z_far;

		uint32_t is_orthogonal;
		float intensity;
		float fade_out_mul;
		float fade_out_add;
	};

	enum ApplyMode {
		APPLY_MODE_COPY,
		APPLY_MODE_COPY_BENT_NORMALS,
		APPLY_MODE_UPSCALE,
		APPLY_MODE_UPSCALE_BENT_NORMALS,
		APPLY_MODE_MAX
	};

	struct {
		ApplyPushConstant push_constant;
		XegtaoApplyShaderRD shader;
		RID shader_version;
		PipelineDeferredRD pipelines[APPLY_MODE_MAX];
	} apply;
};

} // namespace RendererRD
