/**************************************************************************/
/*  motion_blur.h                                                         */
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

// Motion blur after Guertin, McGuire & Nowrouzezahrai, "A Fast and Stable Feature-Aware Motion Blur Filter"
// (HPG 2014), itself an improvement on McGuire, Hennessy, Bukowski & Osman, "A Reconstruction Filter for
// Plausible Motion Blur" (I3D 2012). It is a post process on the HDR color buffer, run after depth of field and
// before glow, auto exposure and tonemapping, in four passes:
//
// - motion_blur_prepare.glsl: the velocity buffer V (half of each pixel's motion over the exposure, in pixels,
//   clamped to the maximum blur radius r) and linear depth Z, from the renderer's motion vectors, split into the
//   camera's rotation, the camera's movement and the objects' own motion so each can be scaled on its own; plus
//   a copy of the color buffer for the last pass to read.
// - motion_blur_tile_max.glsl: TileMax, the dominant (largest) velocity of each r x r tile, run separably.
// - motion_blur_neighbor_max.glsl: NeighborMax, the dominant velocity of each tile's 3 x 3 neighborhood, with
//   the paper's rule that a diagonal neighbor only counts when its velocity runs along the diagonal.
// - motion_blur.glsl: the reconstruction filter, which gathers samples along the neighborhood's dominant
//   velocity and the pixel's own, and writes the result back into the color buffer.
//
// The tile size is the maximum blur radius, so the cost of a frame does not depend on how long the blur is,
// only on how much of the screen is moving and on the number of samples the quality setting gives each pixel.

#include "servers/rendering/renderer_rd/pipeline_deferred_rd.h"
#include "servers/rendering/renderer_rd/shaders/effects/motion_blur.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/motion_blur_neighbor_max.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/motion_blur_prepare.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/motion_blur_tile_max.glsl.gen.h"
#include "servers/rendering/renderer_scene_render.h"

#define RB_SCOPE_MOTION_BLUR SNAME("rb_motion_blur")

#define RB_MOTION_BLUR_COLOR SNAME("color")
#define RB_MOTION_BLUR_VELOCITY_DEPTH SNAME("velocity_depth")
#define RB_MOTION_BLUR_TILE_MAX_X SNAME("tile_max_x")
#define RB_MOTION_BLUR_TILE_MAX SNAME("tile_max")
#define RB_MOTION_BLUR_NEIGHBOR_MAX SNAME("neighbor_max")

class RenderSceneBuffersRD;
class RenderSceneDataRD;

namespace RendererRD {

class MotionBlur {
private:
	static MotionBlur *singleton;

public:
	static MotionBlur *get_singleton() { return singleton; }

	MotionBlur();
	~MotionBlur();

	void set_quality(RSE::EnvironmentMotionBlurQuality p_quality);

	struct Settings {
		// The fraction of the time between two frames the shutter stays open.
		float intensity = 0.5;
		// The maximum blur radius, as a fraction of the height of the image.
		float max_radius = 0.05;
		float camera_rotation_scale = 1.0;
		float camera_movement_scale = 1.0;
		float object_scale = 1.0;
	};

	// Whether these settings would blur anything at all at this resolution.
	static bool is_active(const Settings &p_settings, const Size2i &p_size);

	// Blurs the color buffer post processing works on (the upscaled one if p_use_upscaled_texture is set, else
	// the internal one) in place, for every view. It must be a storage capable R16G16B16A16_SFLOAT texture.
	void process(Ref<RenderSceneBuffersRD> p_render_buffers, const RenderSceneDataRD *p_scene_data, bool p_use_upscaled_texture, const Settings &p_settings);

private:
	RSE::EnvironmentMotionBlurQuality quality = RSE::ENV_MOTION_BLUR_QUALITY_HIGH;

	RID nearest_sampler;

	// The camera matrices of each view, as the prepare pass reads them (see ViewData there).
	struct ViewData {
		float reprojection[16];
		float reprojection_rotation[16];
		float inv_projection[16];
	};

	struct ViewDataBlock {
		ViewData views[RendererSceneRender::MAX_RENDER_VIEWS];
	};

	RID view_data_buffer;

	struct PreparePushConstant {
		int32_t size[2];
		int32_t source_size[2];

		float velocity_scale;
		float max_radius;
		float camera_rotation_scale;
		float camera_movement_scale;

		float object_scale;
		uint32_t view;
		float max_depth;
		float pad;
	};

	struct {
		PreparePushConstant push_constant;
		MotionBlurPrepareShaderRD shader;
		RID shader_version;
		PipelineDeferredRD pipeline;
	} prepare;

	struct TileMaxPushConstant {
		int32_t source_size[2];
		int32_t dest_size[2];

		int32_t tile_size;
		int32_t pad[3];
	};

	enum TileMaxMode {
		TILE_MAX_MODE_X,
		TILE_MAX_MODE_Y,
		TILE_MAX_MODE_MAX
	};

	struct {
		TileMaxPushConstant push_constant;
		MotionBlurTileMaxShaderRD shader;
		RID shader_version;
		PipelineDeferredRD pipelines[TILE_MAX_MODE_MAX];
	} tile_max;

	struct NeighborMaxPushConstant {
		int32_t size[2];
		int32_t pad[2];
	};

	struct {
		NeighborMaxPushConstant push_constant;
		MotionBlurNeighborMaxShaderRD shader;
		RID shader_version;
		PipelineDeferredRD pipeline;
	} neighbor_max;

	struct GatherPushConstant {
		int32_t size[2];
		int32_t tile_count[2];

		int32_t tile_size;
		int32_t sample_count;
		int32_t pad[2];
	};

	struct {
		GatherPushConstant push_constant;
		MotionBlurShaderRD shader;
		RID shader_version;
		PipelineDeferredRD pipeline;
	} gather;

	static int _get_tile_size(const Settings &p_settings, const Size2i &p_size);
	void _allocate_buffers(Ref<RenderSceneBuffersRD> p_render_buffers, const Size2i &p_size, const Size2i &p_tile_count);
};

} // namespace RendererRD
