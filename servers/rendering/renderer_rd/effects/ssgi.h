/**************************************************************************/
/*  ssgi.h                                                                */
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

// Screen-space global illumination (SSGI): stochastic screen-space ray traced diffuse indirect light.
//
// Every working pixel (full resolution, or one in four at half size) traces a few rays over the hemisphere
// around its normal, distributed by the cosine and drawn anew every frame from a low discrepancy sequence.
// Each ray is marched through the depth buffer in screen space, perspective correctly, with its samples
// packed towards its start, and hits what it passes behind (no thicker than the Environment's ssgi_thickness)
// or crosses. A hit takes the light the previous frame drew there, reprojected, from a mipmap as wide as
// the ray's footprint. A ray that leaves the screen or goes its whole length without hitting anything is a
// miss, and leaves the light from that direction to the ambient light (sky, ambient color, ReflectionProbe,
// VoxelGI, SDFGI, LightmapGI).
//
// The noisy result is then accumulated over frames (reprojected with the camera, rejected across
// disocclusions, and shortened where it falls behind changing light), filtered spatially with an edge-aware
// a-trous filter guided by its variance (after SVGF, Schied et al. 2017), and resolved to full resolution
// (bilaterally, when running at half resolution).
//
// Output, at full resolution:
// - RB_SSGI_FINAL: rgb, the light the rays found on screen (already scaled by the intensity), as the lighting
//   pass adds it before multiplying by the albedo; alpha, how much of the ambient light that light replaces
//   (the share of rays that hit something, scaled by the occlusion).

#include "servers/rendering/renderer_rd/pipeline_deferred_rd.h"
#include "servers/rendering/renderer_rd/shaders/effects/ssgi.glsl.gen.h"
#include "servers/rendering/renderer_scene_render.h"

#define RB_SCOPE_SSGI SNAME("rb_ssgi")

#define RB_SSGI_TRACE SNAME("trace")
#define RB_SSGI_SURFACE_0 SNAME("surface_0")
#define RB_SSGI_SURFACE_1 SNAME("surface_1")
#define RB_SSGI_HISTORY_0 SNAME("history_0")
#define RB_SSGI_HISTORY_1 SNAME("history_1")
#define RB_SSGI_META_0 SNAME("meta_0")
#define RB_SSGI_META_1 SNAME("meta_1")
#define RB_SSGI_DENOISE_0 SNAME("denoise_0")
#define RB_SSGI_DENOISE_1 SNAME("denoise_1")
#define RB_SSGI_VARIANCE_0 SNAME("variance_0")
#define RB_SSGI_VARIANCE_1 SNAME("variance_1")
#define RB_SSGI_FINAL SNAME("final")

class RenderSceneBuffersRD;

namespace RendererRD {

class SSGI {
private:
	static SSGI *singleton;

public:
	static SSGI *get_singleton() { return singleton; }

	SSGI();
	~SSGI();

	void set_quality(RSE::EnvironmentSSGIQuality p_quality, bool p_half_size, int p_denoise_passes, int p_history_frames);

	struct RenderBuffers {
		bool half_size = false;
		Size2i working_size;
		Size2i full_size;

		// Whether the history holds frames this one can reproject (false after the buffers are made).
		bool history_valid = false;
		// Which of the ping-ponged surface, history and meta textures this frame writes.
		uint32_t history_index = 0;
		uint32_t frame = 0;

		// The previous frame's camera, to reproject into.
		Transform3D prev_cam_transform;
		Projection prev_projections[RendererSceneRender::MAX_RENDER_VIEWS];
	};

	struct Settings {
		float intensity = 1.0;
		float max_distance = 4.0;
		float thickness = 0.5;
		float occlusion = 1.0;

		Size2i full_screen_size;
	};

	void allocate_buffers(Ref<RenderSceneBuffersRD> p_render_buffers, RenderBuffers &p_ssgi_buffers, const Settings &p_settings);
	// Traces, accumulates and filters all views. p_projections are the views' projections, from the camera's
	// view space (shared by all views), and p_cam_transform the camera's transform.
	void generate(Ref<RenderSceneBuffersRD> p_render_buffers, RenderBuffers &p_ssgi_buffers, const RID *p_normal_buffers, const Projection *p_projections, const Transform3D &p_cam_transform, const Settings &p_settings);

private:
	RSE::EnvironmentSSGIQuality quality = RSE::ENV_SSGI_QUALITY_MEDIUM;
	bool half_size = true;
	int denoise_passes = 4;
	int history_frames = 24;

	RID nearest_sampler;

	enum Mode {
		MODE_TRACE,
		MODE_TEMPORAL,
		MODE_DENOISE,
		MODE_APPLY,
		MODE_APPLY_UPSCALE,
		MODE_MAX
	};

	struct SceneData {
		float projection[2][16];
		float inv_projection[2][16];
		float reprojection[2][16]; // This frame's view space to the previous frame's clip space.
		float prev_view_from_view[16]; // This frame's view space to the previous frame's view space.
		float view_to_world[16]; // This frame's view space to world space, rotation only.
	};

	struct PushConstant {
		int32_t full_size[2];
		int32_t working_size[2];

		uint32_t view_index;
		int32_t scale;
		uint32_t frame;
		uint32_t orthogonal;

		uint32_t ray_count;
		uint32_t step_count;
		float max_distance;
		float thickness;

		float z_near;
		float last_frame_max_lod;
		float max_history;
		uint32_t history_valid;

		int32_t step_size;
		float intensity;
		float occlusion;
		float pixel_world_size; // World size of a pixel one unit from the camera (or anywhere, orthogonal).
	};

	static_assert(sizeof(PushConstant) <= 128, "Push constants are only guaranteed up to 128 bytes.");

	PushConstant push_constant;
	SsgiShaderRD shader;
	RID shader_version;
	PipelineDeferredRD pipelines[MODE_MAX];
	RID scene_data_ubo;
};

} // namespace RendererRD
