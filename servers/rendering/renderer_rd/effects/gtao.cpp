/**************************************************************************/
/*  gtao.cpp                                                             */
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

#include "gtao.h"

#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

GTAO *GTAO::singleton = nullptr;

// Reprojection matrix, stored as a plain float array rather than relying on sizeof(Projection) — same
// approach SSIL takes for its own last-frame-projection UBO (SSILProjectionUniforms).
struct GTAOReprojectionUniforms {
	float reprojection[16];
};

static void store_projection(const Projection &p_projection, float *p_array) {
	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 4; j++) {
			p_array[i * 4 + j] = p_projection.columns[i][j];
		}
	}
}

GTAO::GTAO() {
	singleton = this;

	{
		RD::SamplerState nearest_state;
		nearest_state.mag_filter = RD::SAMPLER_FILTER_NEAREST;
		nearest_state.min_filter = RD::SAMPLER_FILTER_NEAREST;
		nearest_state.mip_filter = RD::SAMPLER_FILTER_NEAREST;
		nearest_state.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
		nearest_state.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
		nearest_sampler = RD::get_singleton()->sampler_create(nearest_state);

		RD::SamplerState linear_state;
		linear_state.mag_filter = RD::SAMPLER_FILTER_LINEAR;
		linear_state.min_filter = RD::SAMPLER_FILTER_LINEAR;
		linear_state.mip_filter = RD::SAMPLER_FILTER_LINEAR;
		linear_state.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
		linear_state.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
		linear_sampler = RD::get_singleton()->sampler_create(linear_state);
	}

	{
		Vector<String> modes;
		modes.push_back("\n#define MODE_BASE\n");
		modes.push_back("\n");
		downsample.shader.initialize(modes);
		downsample.shader_version = downsample.shader.version_create();
		for (int i = 0; i < DOWNSAMPLE_MAX; i++) {
			downsample.pipelines[i].create_compute_pipeline(downsample.shader.version_get_shader(downsample.shader_version, i));
		}
	}

	{
		Vector<String> modes;
		modes.push_back("\n");
		gather.shader.initialize(modes);
		gather.shader_version = gather.shader.version_create();
		gather.pipeline.create_compute_pipeline(gather.shader.version_get_shader(gather.shader_version, 0));
	}

	{
		Vector<String> modes;
		modes.push_back("\n");
		temporal.shader.initialize(modes);
		temporal.shader_version = temporal.shader.version_create();
		temporal.pipeline.create_compute_pipeline(temporal.shader.version_get_shader(temporal.shader_version, 0));

		temporal.reprojection_uniform_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(GTAOReprojectionUniforms));
	}

	{
		Vector<String> modes;
		modes.push_back("\n");
		upscale.shader.initialize(modes);
		upscale.shader_version = upscale.shader.version_create();
		upscale.pipeline.create_compute_pipeline(upscale.shader.version_get_shader(upscale.shader_version, 0));
	}
}

GTAO::~GTAO() {
	for (int i = 0; i < DOWNSAMPLE_MAX; i++) {
		downsample.pipelines[i].free();
	}
	downsample.shader.version_free(downsample.shader_version);

	gather.pipeline.free();
	gather.shader.version_free(gather.shader_version);

	temporal.pipeline.free();
	temporal.shader.version_free(temporal.shader_version);
	RD::get_singleton()->free_rid(temporal.reprojection_uniform_buffer);

	upscale.pipeline.free();
	upscale.shader.version_free(upscale.shader_version);

	RD::get_singleton()->free_rid(nearest_sampler);
	RD::get_singleton()->free_rid(linear_sampler);

	singleton = nullptr;
}

void GTAO::set_quality(RSE::EnvironmentGTAOQuality p_quality, bool p_half_size, float p_fadeout_from, float p_fadeout_to) {
	quality = p_quality;
	half_size = p_half_size;
	fadeout_from = p_fadeout_from;
	fadeout_to = p_fadeout_to;
}

void GTAO::allocate_buffers(Ref<RenderSceneBuffersRD> p_render_buffers, RenderBuffers &p_gtao_buffers, const Settings &p_settings) {
	Size2i working_size = half_size ? Size2i((p_settings.full_screen_size.x + 1) / 2, (p_settings.full_screen_size.y + 1) / 2) : p_settings.full_screen_size;
	working_size = working_size.maxi(1);

	if (p_gtao_buffers.half_size != half_size || p_gtao_buffers.buffer_width != working_size.x || p_gtao_buffers.buffer_height != working_size.y) {
		p_render_buffers->clear_context(RB_SCOPE_GTAO);
		for (uint32_t v = 0; v < RendererSceneRender::MAX_RENDER_VIEWS; v++) {
			p_gtao_buffers.history_valid[v] = false;
		}
	}

	p_gtao_buffers.half_size = half_size;
	p_gtao_buffers.buffer_width = working_size.x;
	p_gtao_buffers.buffer_height = working_size.y;

	int min_dim = MIN(working_size.x, working_size.y);
	p_gtao_buffers.mip_count = CLAMP(uint32_t(Math::floor(Math::log2(double(MAX(min_dim, 1))))) + 1, 1u, 4u);

	uint32_t view_count = p_render_buffers->get_view_count();

	// As we're not clearing these, and render buffers will return the cached texture if it already exists,
	// we don't first check has_texture here.
	p_render_buffers->create_texture(RB_SCOPE_GTAO, RB_GTAO_DEPTH, RD::DATA_FORMAT_R16_SFLOAT, RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, working_size, view_count, p_gtao_buffers.mip_count);
	p_render_buffers->create_texture(RB_SCOPE_GTAO, RB_GTAO_RAW, RD::DATA_FORMAT_R16_SFLOAT, RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, working_size, view_count);
	p_render_buffers->create_texture(RB_SCOPE_GTAO, RB_GTAO_HISTORY_A, RD::DATA_FORMAT_R16G16_SFLOAT, RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, working_size, view_count);
	p_render_buffers->create_texture(RB_SCOPE_GTAO, RB_GTAO_HISTORY_B, RD::DATA_FORMAT_R16G16_SFLOAT, RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, working_size, view_count);
	p_render_buffers->create_texture(RB_SCOPE_GTAO, RB_GTAO_FINAL, RD::DATA_FORMAT_R8_UNORM, RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1);
}

void GTAO::generate(Ref<RenderSceneBuffersRD> p_render_buffers, RenderBuffers &p_gtao_buffers, uint32_t p_view, RID p_normal_buffer, const Projection &p_projection, const Transform3D &p_cam_transform, const Settings &p_settings) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);

	Size2i working_size(p_gtao_buffers.buffer_width, p_gtao_buffers.buffer_height);
	Size2i full_size = p_settings.full_screen_size;
	bool is_orthogonal = p_projection.is_orthogonal();

	// Depth linearization constants, shared by the downsample base pass and the final upscale (both read
	// hardware depth directly); see servers/rendering/renderer_rd/shaders/effects/ss_effects_downsample.glsl
	// for the equivalent, proven derivation this mirrors.
	Projection depth_correction;
	depth_correction.set_depth_correction(false);
	Projection linearize_source = depth_correction * p_projection;
	float depth_linearize_mul = -linearize_source.columns[3][2];
	float depth_linearize_add = linearize_source.columns[2][2];
	if (depth_linearize_mul * depth_linearize_add < 0) {
		depth_linearize_add = -depth_linearize_add;
	}
	if (is_orthogonal) {
		depth_linearize_mul = p_projection.get_z_near();
		depth_linearize_add = p_projection.get_z_far();
	}

	RID depth_texture = p_render_buffers->get_texture_slice(RB_SCOPE_GTAO, RB_GTAO_DEPTH, p_view, 0, 1, p_gtao_buffers.mip_count);
	RID raw_texture = p_render_buffers->get_texture_slice(RB_SCOPE_GTAO, RB_GTAO_RAW, p_view, 0);
	RID final_texture = p_render_buffers->get_texture_slice(RB_SCOPE_GTAO, RB_GTAO_FINAL, p_view, 0);

	bool read_a = (p_gtao_buffers.frame_index[p_view] % 2) == 0;
	RID history_read = p_render_buffers->get_texture_slice(RB_SCOPE_GTAO, read_a ? RB_GTAO_HISTORY_A : RB_GTAO_HISTORY_B, p_view, 0);
	RID history_write = p_render_buffers->get_texture_slice(RB_SCOPE_GTAO, read_a ? RB_GTAO_HISTORY_B : RB_GTAO_HISTORY_A, p_view, 0);

	RD::get_singleton()->draw_command_begin_label("Process Ground-Truth Ambient Occlusion");
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();

	/* PASS 1: depth downsample + mip chain */
	{
		RD::get_singleton()->draw_command_begin_label("Downsample Depth");
		RID depth_hw_texture = p_render_buffers->get_depth_texture(p_view);

		memset(&downsample.push_constant, 0, sizeof(DownsamplePushConstant));
		downsample.push_constant.pixel_size[0] = 1.0 / full_size.x;
		downsample.push_constant.pixel_size[1] = 1.0 / full_size.y;
		downsample.push_constant.is_orthogonal = is_orthogonal;
		downsample.push_constant.half_size = p_gtao_buffers.half_size;
		downsample.push_constant.depth_linearize_mul = depth_linearize_mul;
		downsample.push_constant.depth_linearize_add = depth_linearize_add;

		RID base_shader = downsample.shader.version_get_shader(downsample.shader_version, DOWNSAMPLE_BASE);
		RID mip0 = p_render_buffers->get_texture_slice(RB_SCOPE_GTAO, RB_GTAO_DEPTH, p_view, 0);

		RD::Uniform u_source(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, depth_hw_texture }));
		RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ mip0 }));

		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, downsample.pipelines[DOWNSAMPLE_BASE].get_rid());
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(base_shader, 0, u_source), 0);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(base_shader, 1, u_dest), 1);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &downsample.push_constant, sizeof(DownsamplePushConstant));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, working_size.x, working_size.y, 1);
		RD::get_singleton()->compute_list_add_barrier(compute_list);

		RID mip_shader = downsample.shader.version_get_shader(downsample.shader_version, DOWNSAMPLE_MIP);
		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, downsample.pipelines[DOWNSAMPLE_MIP].get_rid());

		for (uint32_t m = 1; m < p_gtao_buffers.mip_count; m++) {
			Size2i prev_size = Size2i(MAX(1, working_size.x >> (m - 1)), MAX(1, working_size.y >> (m - 1)));
			Size2i mip_size = Size2i(MAX(1, working_size.x >> m), MAX(1, working_size.y >> m));

			downsample.push_constant.pixel_size[0] = 1.0 / prev_size.x;
			downsample.push_constant.pixel_size[1] = 1.0 / prev_size.y;

			RID prev_mip = p_render_buffers->get_texture_slice(RB_SCOPE_GTAO, RB_GTAO_DEPTH, p_view, m - 1);
			RID this_mip = p_render_buffers->get_texture_slice(RB_SCOPE_GTAO, RB_GTAO_DEPTH, p_view, m);

			RD::Uniform u_mip_source(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, prev_mip }));
			RD::Uniform u_mip_dest(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ this_mip }));

			RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(mip_shader, 0, u_mip_source), 0);
			RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(mip_shader, 1, u_mip_dest), 1);
			RD::get_singleton()->compute_list_set_push_constant(compute_list, &downsample.push_constant, sizeof(DownsamplePushConstant));
			RD::get_singleton()->compute_list_dispatch_threads(compute_list, mip_size.x, mip_size.y, 1);
			RD::get_singleton()->compute_list_add_barrier(compute_list);
		}
		RD::get_singleton()->draw_command_end_label(); // Downsample Depth
	}

	/* PASS 2: horizon search gather */
	{
		RD::get_singleton()->draw_command_begin_label("Horizon Search");

		float tan_half_fov_x = 1.0 / p_projection.columns[0][0];
		float tan_half_fov_y = 1.0 / p_projection.columns[1][1];

		memset(&gather.push_constant, 0, sizeof(GatherPushConstant));
		gather.push_constant.screen_size[0] = working_size.x;
		gather.push_constant.screen_size[1] = working_size.y;
		gather.push_constant.NDC_to_view_mul_x = tan_half_fov_x * 2.0;
		gather.push_constant.NDC_to_view_mul_y = tan_half_fov_y * -2.0;
		gather.push_constant.NDC_to_view_add_x = tan_half_fov_x * -1.0;
		gather.push_constant.NDC_to_view_add_y = tan_half_fov_y;
		gather.push_constant.is_orthogonal = is_orthogonal;
		gather.push_constant.quality = CLAMP(int(quality), 0, 4);
		gather.push_constant.radius = p_settings.radius;
		gather.push_constant.horizon_bias = p_settings.horizon;
		gather.push_constant.frame_index = p_gtao_buffers.frame_index[p_view];
		gather.push_constant.mip_count = p_gtao_buffers.mip_count;
		gather.push_constant.full_screen_size[0] = full_size.x;
		gather.push_constant.full_screen_size[1] = full_size.y;
		gather.push_constant.depth_texture_pixel_size[0] = 1.0 / working_size.x;
		gather.push_constant.depth_texture_pixel_size[1] = 1.0 / working_size.y;
		gather.push_constant.thin_occluder_compensation = 0.15;

		RID shader = gather.shader.version_get_shader(gather.shader_version, 0);

		RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, depth_texture }));
		RD::Uniform u_normal(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ p_normal_buffer }));
		RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ raw_texture }));

		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, gather.pipeline.get_rid());
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 0, u_depth, u_normal), 0);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 1, u_dest), 1);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &gather.push_constant, sizeof(GatherPushConstant));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, working_size.x, working_size.y, 1);
		RD::get_singleton()->compute_list_add_barrier(compute_list);
		RD::get_singleton()->draw_command_end_label(); // Horizon Search
	}

	/* PASS 3: spatial pre-filter + temporal reprojection/accumulation */
	{
		RD::get_singleton()->draw_command_begin_label("Temporal Accumulation");

		Projection correction;
		correction.set_depth_correction(true);
		Projection projection = correction * p_projection;
		Projection reprojection = p_gtao_buffers.last_frame_projections[p_view] * Projection(p_gtao_buffers.last_frame_transform.affine_inverse()) * Projection(p_cam_transform) * projection.inverse();

		p_gtao_buffers.last_frame_projections[p_view] = projection;

		GTAOReprojectionUniforms reprojection_uniforms;
		store_projection(reprojection, reprojection_uniforms.reprojection);
		RD::get_singleton()->buffer_update(temporal.reprojection_uniform_buffer, 0, sizeof(GTAOReprojectionUniforms), &reprojection_uniforms);

		memset(&temporal.push_constant, 0, sizeof(TemporalPushConstant));
		temporal.push_constant.screen_size[0] = working_size.x;
		temporal.push_constant.screen_size[1] = working_size.y;
		temporal.push_constant.pixel_size[0] = 1.0 / working_size.x;
		temporal.push_constant.pixel_size[1] = 1.0 / working_size.y;
		temporal.push_constant.z_near = p_projection.get_z_near();
		temporal.push_constant.z_far = p_projection.get_z_far();
		temporal.push_constant.history_weight = 0.9;
		temporal.push_constant.history_is_valid = p_gtao_buffers.history_valid[p_view];
		temporal.push_constant.sharpness = p_settings.sharpness;

		RID shader = temporal.shader.version_get_shader(temporal.shader_version, 0);

		RD::Uniform u_raw(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, raw_texture }));
		RID depth_mip0 = p_render_buffers->get_texture_slice(RB_SCOPE_GTAO, RB_GTAO_DEPTH, p_view, 0);
		RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, depth_mip0 }));
		RD::Uniform u_history(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ linear_sampler, history_read }));
		RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ history_write }));
		RD::Uniform u_reprojection(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, Vector<RID>({ temporal.reprojection_uniform_buffer }));

		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, temporal.pipeline.get_rid());
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 0, u_raw, u_depth, u_history), 0);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 1, u_dest), 1);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 2, u_reprojection), 2);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &temporal.push_constant, sizeof(TemporalPushConstant));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, working_size.x, working_size.y, 1);
		RD::get_singleton()->compute_list_add_barrier(compute_list);
		RD::get_singleton()->draw_command_end_label(); // Temporal Accumulation
	}

	/* PASS 4: bilateral upsample to full resolution + artistic shaping */
	{
		RD::get_singleton()->draw_command_begin_label("Upscale");
		RID depth_hw_texture = p_render_buffers->get_depth_texture(p_view);

		memset(&upscale.push_constant, 0, sizeof(UpscalePushConstant));
		upscale.push_constant.full_screen_size[0] = full_size.x;
		upscale.push_constant.full_screen_size[1] = full_size.y;
		upscale.push_constant.half_screen_size[0] = working_size.x;
		upscale.push_constant.half_screen_size[1] = working_size.y;
		upscale.push_constant.is_orthogonal = is_orthogonal;
		upscale.push_constant.depth_linearize_mul = depth_linearize_mul;
		upscale.push_constant.depth_linearize_add = depth_linearize_add;
		upscale.push_constant.intensity = p_settings.intensity;
		upscale.push_constant.power = p_settings.power;
		upscale.push_constant.fade_out_mul = -1.0 / (fadeout_to - fadeout_from);
		upscale.push_constant.fade_out_add = fadeout_from / (fadeout_to - fadeout_from) + 1.0;
		upscale.push_constant.sharpness = p_settings.sharpness;

		RID shader = upscale.shader.version_get_shader(upscale.shader_version, 0);

		RD::Uniform u_full_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, depth_hw_texture }));
		RD::Uniform u_history(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, history_write }));
		RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ final_texture }));

		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, upscale.pipeline.get_rid());
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 0, u_full_depth, u_history), 0);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 1, u_dest), 1);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &upscale.push_constant, sizeof(UpscalePushConstant));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, full_size.x, full_size.y, 1);
		RD::get_singleton()->draw_command_end_label(); // Upscale
	}

	RD::get_singleton()->compute_list_end();
	RD::get_singleton()->draw_command_end_label(); // GTAO

	p_gtao_buffers.last_frame_transform = p_cam_transform;
	p_gtao_buffers.history_valid[p_view] = true;
	p_gtao_buffers.frame_index[p_view]++;
}
