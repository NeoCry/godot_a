/**************************************************************************/
/*  ss_effects.h                                                          */
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

// Ambient occlusion (GTAO) lives in its own dedicated effects/gtao.h/.cpp, not here — see that file for why.

#include "servers/rendering/renderer_rd/pipeline_deferred_rd.h"
#include "servers/rendering/renderer_rd/shaders/effects/screen_space_contact_shadows.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/screen_space_reflection.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/screen_space_reflection_downsample.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/screen_space_reflection_filter.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/screen_space_reflection_hiz.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/screen_space_reflection_resolve.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/subsurface_scattering.glsl.gen.h"

#define RB_SCOPE_SSLF SNAME("rb_sslf")
#define RB_SCOPE_SSR SNAME("rb_ssr")
#define RB_SCOPE_SSCS SNAME("rb_sscs")

#define RB_FINAL SNAME("final")
#define RB_LAST_FRAME SNAME("last_frame")
#define RB_DEINTERLEAVED SNAME("deinterleaved")
#define RB_DEINTERLEAVED_PONG SNAME("deinterleaved_pong")
#define RB_EDGES SNAME("edges")
#define RB_IMPORTANCE_MAP SNAME("importance_map")
#define RB_IMPORTANCE_PONG SNAME("importance_pong")

#define RB_NORMAL_ROUGHNESS SNAME("normal_roughness")
#define RB_HIZ SNAME("hiz")
#define RB_SSR SNAME("ssr")
#define RB_MIP_LEVEL SNAME("mip_level")

#define RB_SSCS SNAME("sscs")

class RenderSceneBuffersRD;

namespace RendererRD {

class CopyEffects;

class SSEffects {
private:
	static SSEffects *singleton;

public:
	static SSEffects *get_singleton() { return singleton; }

	SSEffects();
	~SSEffects();

	/* Last Frame */

	void allocate_last_frame_buffer(Ref<RenderSceneBuffersRD> p_render_buffers, bool p_use_ssilvb, bool p_use_ssr);
	void copy_internal_texture_to_last_frame(Ref<RenderSceneBuffersRD> p_render_buffers, CopyEffects &p_copy_effects);

	/* SS Downsampler */

	/* Screen Space Reflection */
	void ssr_set_half_size(bool p_half_size);

	struct SSRRenderBuffers {
		Size2i size;
		uint32_t mipmaps = 1;
		bool half_size = false;
	};

	void ssr_allocate_buffers(Ref<RenderSceneBuffersRD> p_render_buffers, SSRRenderBuffers &p_ssr_buffers, const RD::DataFormat p_color_format);
	void screen_space_reflection(Ref<RenderSceneBuffersRD> p_render_buffers, SSRRenderBuffers &p_ssr_buffers, const RID *p_normal_roughness_slices, int p_max_steps, float p_fade_in, float p_fade_out, float p_tolerance, const Projection *p_projections, const Projection *p_reprojections, const Vector3 *p_eye_offsets, RendererRD::CopyEffects &p_copy_effects);

	/* subsurface scattering */
	void sss_set_quality(RSE::SubSurfaceScatteringQuality p_quality);
	RSE::SubSurfaceScatteringQuality sss_get_quality() const;
	void sss_set_scale(float p_scale, float p_depth_scale);

	void sub_surface_scattering(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_diffuse, RID p_depth, const Projection &p_camera, const Size2i &p_screen_size);

	/* Screen Space Shadows */
	struct SSCSRenderBuffers {
		Size2i size;
		uint32_t light_count = 0;
	};

	struct SSCSSettings {
		RSE::ScreenSpaceContactShadowsLength quality = RSE::SCREEN_SPACE_CONTACT_SHADOWS_LENGTH_MEDIUM;
		float surface_thickness = 0.01f;
		// Debug-only: colors the output by compute wavefront index instead of computing real
		// shadows, so the wavefront layout of the multi-dispatch (BuildDispatchList-style)
		// coverage can be inspected. Driven by the viewport's debug draw mode, see
		// RSE::VIEWPORT_DEBUG_DRAW_SSCS_WAVE_INDEX.
		bool debug_wave_index = false;
	};

	void sscs_allocate_buffers(Ref<RenderSceneBuffersRD> p_render_buffers, SSCSRenderBuffers &p_sscs_buffers, uint32_t p_contact_shadow_count);
	// p_exclusion_depth_textures (one per view): depth-only textures containing just the
	// GeometryInstance3D.ignore_screen_space_shadows instances, rendered from the same camera as
	// p_projections (see RenderForwardClustered::_render_sscs_exclusion_depth()). A ray-march
	// sample is treated as non-occluding when this buffer's depth matches the main scene depth at
	// that pixel, i.e. the visible surface there actually belongs to an excluded instance.
	void screen_space_contact_shadows(Ref<RenderSceneBuffersRD> p_render_buffers, SSCSRenderBuffers &p_sscs_buffers, const SSCSSettings &p_settings, const Projection *p_projections, const RID *p_exclusion_depth_textures, Vector3 p_light_direction, uint32_t p_light_index, float p_opacity, float p_blur, float p_taa_frame_count);

private:
	/* Settings */

	bool ssr_half_size = false;

	RSE::SubSurfaceScatteringQuality sss_quality = RSE::SUB_SURFACE_SCATTERING_QUALITY_MEDIUM;
	float sss_scale = 0.05;
	float sss_depth_scale = 0.01;

	/* SS Downsampler */

	/* Screen Space Reflection */

	enum ScreenSpaceReflectionDownsampleMode {
		SCREEN_SPACE_REFLECTION_DOWNSAMPLE_DEFAULT,
		SCREEN_SPACE_REFLECTION_DOWNSAMPLE_ODD_WIDTH,
		SCREEN_SPACE_REFLECTION_DOWNSAMPLE_ODD_HEIGHT,
		SCREEN_SPACE_REFLECTION_DOWNSAMPLE_ODD_WIDTH_AND_HEIGHT,
		SCREEN_SPACE_REFLECTION_DOWNSAMPLE_MAX
	};

	struct ScreenSpaceReflectionDownsamplePushConstant {
		int32_t screen_size[2];
		int32_t pad[2];
	};

	enum ScreenSpaceReflectionHizMode {
		SCREEN_SPACE_REFLECTION_HIZ_DEFAULT,
		SCREEN_SPACE_REFLECTION_HIZ_ODD_WIDTH,
		SCREEN_SPACE_REFLECTION_HIZ_ODD_HEIGHT,
		SCREEN_SPACE_REFLECTION_HIZ_ODD_WIDTH_AND_HEIGHT,
		SCREEN_SPACE_REFLECTION_HIZ_MAX
	};

	struct ScreenSpaceReflectionHizPushConstant {
		int32_t screen_size[2];
		int32_t pad[2];
	};

	struct ScreenSpaceReflectionSceneData {
		float projection[2][16];
		float inv_projection[2][16];
		float reprojection[2][16];
		float eye_offset[2][4];
	};

	struct ScreenSpaceReflectionPushConstant {
		int32_t screen_size[2];
		int32_t mipmaps;
		int32_t num_steps;
		float distance_fade;
		float curve_fade_in;
		float depth_tolerance;
		int32_t orthogonal;
		uint32_t view_index;
		int32_t pad[3];
	};

	struct ScreenSpaceReflectionFilterPushConstant {
		int32_t screen_size[2];
		uint32_t mip_level;
		int32_t pad;
	};

	struct ScreenSpaceReflectionResolvePushConstant {
		int32_t screen_size[2];
		int32_t pad[2];
	};

	struct ScreenSpaceReflection {
		ScreenSpaceReflectionDownsampleShaderRD downsample_shader;
		RID downsample_shader_version;
		PipelineDeferredRD downsample_pipelines[SCREEN_SPACE_REFLECTION_DOWNSAMPLE_MAX];

		ScreenSpaceReflectionHizShaderRD hiz_shader;
		RID hiz_shader_version;
		PipelineDeferredRD hiz_pipelines[SCREEN_SPACE_REFLECTION_HIZ_MAX];

		ScreenSpaceReflectionShaderRD ssr_shader;
		RID ssr_shader_version;
		PipelineDeferredRD ssr_pipeline;
		RID ubo;

		ScreenSpaceReflectionFilterShaderRD filter_shader;
		RID filter_shader_version;
		PipelineDeferredRD filter_pipeline;

		ScreenSpaceReflectionResolveShaderRD resolve_shader;
		RID resolve_shader_version;
		PipelineDeferredRD resolve_pipeline;
	} ssr;

	/* Screen Space Shadows */

	enum ScreenSpaceContactShadowsMode {
		SCREEN_SPACE_CONTACT_SHADOWS_LOW_QUALITY,
		SCREEN_SPACE_CONTACT_SHADOWS_MEDIUM_QUALITY,
		SCREEN_SPACE_CONTACT_SHADOWS_HIGH_QUALITY,
		SCREEN_SPACE_CONTACT_SHADOWS_MAX
	};

	struct ScreenSpaceContactShadows {
		ScreenSpaceContactShadowsShaderRD sscs_shader;
		RID sscs_shader_version;
		PipelineDeferredRD sscs_pipelines[SCREEN_SPACE_CONTACT_SHADOWS_MAX];
		RID border_sampler;

	} sscs;

	struct ScreenSpaceContactShadowsPushConstant {
		int32_t screen_size[2];
		int32_t light_offset[2];
		float light_coordinates[4];
		float surface_thickness;
		float opacity;
		float blur;
		float taa_frame_count;
		uint32_t debug_wave_index;
	};

	/* Subsurface scattering */

	enum SSSMode {
		SUBSURFACE_SCATTERING_MODE_LOW_QUALITY,
		SUBSURFACE_SCATTERING_MODE_MEDIUM_QUALITY,
		SUBSURFACE_SCATTERING_MODE_HIGH_QUALITY,
		SUBSURFACE_SCATTERING_MODE_MAX
	};

	struct SubSurfaceScatteringPushConstant {
		int32_t screen_size[2];
		float camera_z_far;
		float camera_z_near;

		uint32_t vertical;
		uint32_t orthogonal;
		float unit_size;
		float scale;

		float depth_scale;
		uint32_t pad[3];
	};

	struct SubSurfaceScattering {
		SubSurfaceScatteringPushConstant push_constant;
		SubsurfaceScatteringShaderRD shader;
		RID shader_version;
		PipelineDeferredRD pipelines[SUBSURFACE_SCATTERING_MODE_MAX];
	} sss;
};

} // namespace RendererRD
