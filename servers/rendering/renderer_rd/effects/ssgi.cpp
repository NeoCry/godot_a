/**************************************************************************/
/*  ssgi.cpp                                                              */
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

#include "ssgi.h"

#include "core/config/project_settings.h"
#include "servers/rendering/renderer_rd/effects/ss_effects.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

SSGI *SSGI::singleton = nullptr;

// Rays per working pixel and depth buffer samples per ray, for Very Low to Ultra.
static constexpr uint32_t SSGI_RAY_COUNT[RSE::ENV_SSGI_QUALITY_MAX] = { 1, 1, 2, 3, 4 };
static constexpr uint32_t SSGI_STEP_COUNT[RSE::ENV_SSGI_QUALITY_MAX] = { 12, 20, 24, 32, 48 };

SSGI::SSGI() {
	singleton = this;

	{
		RD::SamplerState nearest_state;
		nearest_state.mag_filter = RD::SAMPLER_FILTER_NEAREST;
		nearest_state.min_filter = RD::SAMPLER_FILTER_NEAREST;
		nearest_state.mip_filter = RD::SAMPLER_FILTER_NEAREST;
		nearest_state.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
		nearest_state.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
		nearest_sampler = RD::get_singleton()->sampler_create(nearest_state);
	}

	{
		Vector<String> modes;
		modes.push_back("\n#define MODE_TRACE\n"); // MODE_TRACE
		modes.push_back("\n#define MODE_PREBLUR\n"); // MODE_PREBLUR
		modes.push_back("\n#define MODE_TEMPORAL\n"); // MODE_TEMPORAL
		modes.push_back("\n#define MODE_DENOISE\n"); // MODE_DENOISE
		modes.push_back("\n#define MODE_APPLY\n"); // MODE_APPLY
		modes.push_back("\n#define MODE_APPLY\n#define MODE_UPSCALE\n"); // MODE_APPLY_UPSCALE
		shader.initialize(modes);
		shader_version = shader.version_create();
		for (int i = 0; i < MODE_MAX; i++) {
			pipelines[i].create_compute_pipeline(shader.version_get_shader(shader_version, i));
		}
	}

	scene_data_ubo = RD::get_singleton()->uniform_buffer_create(sizeof(SceneData));

	set_quality(RSE::EnvironmentSSGIQuality(int(GLOBAL_GET("rendering/environment/ssgi/quality"))), GLOBAL_GET("rendering/environment/ssgi/half_size"), GLOBAL_GET("rendering/environment/ssgi/denoise_passes"), GLOBAL_GET("rendering/environment/ssgi/history_frames"));
}

SSGI::~SSGI() {
	for (int i = 0; i < MODE_MAX; i++) {
		pipelines[i].free();
	}
	shader.version_free(shader_version);

	RD::get_singleton()->free_rid(scene_data_ubo);
	RD::get_singleton()->free_rid(nearest_sampler);

	singleton = nullptr;
}

void SSGI::set_quality(RSE::EnvironmentSSGIQuality p_quality, bool p_half_size, int p_denoise_passes, int p_history_frames) {
	quality = RSE::EnvironmentSSGIQuality(CLAMP(int(p_quality), 0, int(RSE::ENV_SSGI_QUALITY_MAX) - 1));
	half_size = p_half_size;
	denoise_passes = CLAMP(p_denoise_passes, 0, 5);
	history_frames = CLAMP(p_history_frames, 1, 64);
}

void SSGI::allocate_buffers(Ref<RenderSceneBuffersRD> p_render_buffers, RenderBuffers &p_ssgi_buffers, const Settings &p_settings) {
	Size2i full_size = p_settings.full_screen_size.maxi(1);
	// At half size, working pixel p stands for full resolution pixel 2p.
	Size2i working_size = half_size ? Size2i((full_size.x + 1) / 2, (full_size.y + 1) / 2) : full_size;

	if (p_ssgi_buffers.half_size != half_size || p_ssgi_buffers.working_size != working_size || p_ssgi_buffers.full_size != full_size || !p_render_buffers->has_texture(RB_SCOPE_SSGI, RB_SSGI_FINAL)) {
		p_render_buffers->clear_context(RB_SCOPE_SSGI);

		p_ssgi_buffers.half_size = half_size;
		p_ssgi_buffers.working_size = working_size;
		p_ssgi_buffers.full_size = full_size;
		p_ssgi_buffers.history_valid = false;
		p_ssgi_buffers.history_index = 0;

		uint32_t view_count = p_render_buffers->get_view_count();
		uint32_t usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;

		// This frame's rays: the light they found in rgb, the share of them that hit something in alpha.
		p_render_buffers->create_texture(RB_SCOPE_SSGI, RB_SSGI_TRACE, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, working_size, view_count);
		// Ping-ponged between frames, so each frame can reproject the previous one's:
		// - surface: linear depth, then the world space normal, of each working pixel (0 depth where nothing was drawn).
		// - history: the accumulated rays.
		// - meta: how many frames the history holds, and the first two moments of the luminance of the rays.
		p_render_buffers->create_texture(RB_SCOPE_SSGI, RB_SSGI_SURFACE_0, RD::DATA_FORMAT_R32G32B32A32_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, working_size, view_count);
		p_render_buffers->create_texture(RB_SCOPE_SSGI, RB_SSGI_SURFACE_1, RD::DATA_FORMAT_R32G32B32A32_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, working_size, view_count);
		p_render_buffers->create_texture(RB_SCOPE_SSGI, RB_SSGI_HISTORY_0, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, working_size, view_count);
		p_render_buffers->create_texture(RB_SCOPE_SSGI, RB_SSGI_HISTORY_1, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, working_size, view_count);
		p_render_buffers->create_texture(RB_SCOPE_SSGI, RB_SSGI_META_0, RD::DATA_FORMAT_R32G32B32A32_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, working_size, view_count);
		p_render_buffers->create_texture(RB_SCOPE_SSGI, RB_SSGI_META_1, RD::DATA_FORMAT_R32G32B32A32_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, working_size, view_count);
		// The spatial filter's passes, ping-ponged: the light, and the variance of its luminance.
		p_render_buffers->create_texture(RB_SCOPE_SSGI, RB_SSGI_DENOISE_0, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, working_size, view_count);
		p_render_buffers->create_texture(RB_SCOPE_SSGI, RB_SSGI_DENOISE_1, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, working_size, view_count);
		p_render_buffers->create_texture(RB_SCOPE_SSGI, RB_SSGI_VARIANCE_0, RD::DATA_FORMAT_R32_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, working_size, view_count);
		p_render_buffers->create_texture(RB_SCOPE_SSGI, RB_SSGI_VARIANCE_1, RD::DATA_FORMAT_R32_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, working_size, view_count);

		p_render_buffers->create_texture(RB_SCOPE_SSGI, RB_SSGI_FINAL, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, full_size, view_count);
	}
}

void SSGI::generate(Ref<RenderSceneBuffersRD> p_render_buffers, RenderBuffers &p_ssgi_buffers, const RID *p_normal_buffers, const Projection *p_projections, const Transform3D &p_cam_transform, const Settings &p_settings) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);
	TextureStorage *texture_storage = TextureStorage::get_singleton();
	ERR_FAIL_NULL(texture_storage);

	const uint32_t view_count = p_render_buffers->get_view_count();
	const Size2i working_size = p_ssgi_buffers.working_size;
	const Size2i full_size = p_ssgi_buffers.full_size;
	const bool use_half_size = p_ssgi_buffers.half_size;
	const bool is_orthogonal = p_projections[0].is_orthogonal();
	const bool history_valid = p_ssgi_buffers.history_valid;

	// Matrices. The views share the camera's view space (their projections take each eye into account), so a
	// single transform reprojects them all.
	Projection correction;
	correction.set_depth_correction(true);
	Projection projections[RendererSceneRender::MAX_RENDER_VIEWS];
	{
		SceneData scene_data;
		memset(&scene_data, 0, sizeof(SceneData));

		Transform3D prev_view_from_view = history_valid ? p_ssgi_buffers.prev_cam_transform.affine_inverse() * p_cam_transform : Transform3D();
		for (uint32_t v = 0; v < view_count; v++) {
			projections[v] = correction * p_projections[v];
			MaterialStorage::store_camera(projections[v], scene_data.projection[v]);
			MaterialStorage::store_camera(projections[v].inverse(), scene_data.inv_projection[v]);
			Projection prev_projection = history_valid ? p_ssgi_buffers.prev_projections[v] : projections[v];
			MaterialStorage::store_camera(prev_projection * Projection(prev_view_from_view), scene_data.reprojection[v]);
		}
		MaterialStorage::store_transform(prev_view_from_view, scene_data.prev_view_from_view);
		Transform3D view_to_world;
		view_to_world.basis = p_cam_transform.basis.orthonormalized();
		MaterialStorage::store_transform(view_to_world, scene_data.view_to_world);

		RD::get_singleton()->buffer_update(scene_data_ubo, 0, sizeof(SceneData), &scene_data);
	}

	// The previous frame's image, which the rays take the light they hit from. It only exists from the
	// frame after SSGI comes on (see SSEffects::allocate_last_frame_buffer()).
	const bool has_last_frame = p_render_buffers->has_texture(RB_SCOPE_SSLF, RB_LAST_FRAME);
	const uint32_t last_frame_mipmaps = has_last_frame ? p_render_buffers->get_texture_format(RB_SCOPE_SSLF, RB_LAST_FRAME).mipmaps : 1;

	RID linear_mipmap_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	const uint32_t write = p_ssgi_buffers.history_index;
	const uint32_t read = 1 - write;
	const StringName surface_names[2] = { RB_SSGI_SURFACE_0, RB_SSGI_SURFACE_1 };
	const StringName history_names[2] = { RB_SSGI_HISTORY_0, RB_SSGI_HISTORY_1 };
	const StringName meta_names[2] = { RB_SSGI_META_0, RB_SSGI_META_1 };
	const StringName denoise_names[2] = { RB_SSGI_DENOISE_0, RB_SSGI_DENOISE_1 };
	const StringName variance_names[2] = { RB_SSGI_VARIANCE_0, RB_SSGI_VARIANCE_1 };

	memset(&push_constant, 0, sizeof(PushConstant));
	push_constant.full_size[0] = full_size.x;
	push_constant.full_size[1] = full_size.y;
	push_constant.working_size[0] = working_size.x;
	push_constant.working_size[1] = working_size.y;
	push_constant.scale = use_half_size ? 2 : 1;
	push_constant.frame = p_ssgi_buffers.frame;
	push_constant.orthogonal = is_orthogonal;
	push_constant.ray_count = SSGI_RAY_COUNT[quality];
	push_constant.step_count = SSGI_STEP_COUNT[quality];
	push_constant.max_distance = MAX(p_settings.max_distance, 0.001f);
	push_constant.thickness = MAX(p_settings.thickness, 0.001f);
	push_constant.z_near = p_projections[0].get_z_near();
	push_constant.last_frame_max_lod = float(last_frame_mipmaps - 1);
	push_constant.max_history = float(history_frames);
	push_constant.history_valid = history_valid;
	push_constant.intensity = p_settings.intensity;
	push_constant.occlusion = CLAMP(p_settings.occlusion, 0.0f, 1.0f);
	// How big a pixel is a unit away from the camera, from the vertical field of view (or, orthogonal, anywhere).
	push_constant.pixel_world_size = 2.0f / (full_size.y * Math::abs(p_projections[0].columns[1][1]));

	RD::get_singleton()->draw_command_begin_label("SSGI");
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();

	for (uint32_t v = 0; v < view_count; v++) {
		push_constant.view_index = v;

		RID depth_texture = p_render_buffers->get_depth_texture(v);
		RID normal_texture = p_normal_buffers[v];
		RID last_frame = has_last_frame ? p_render_buffers->get_texture_slice(RB_SCOPE_SSLF, RB_LAST_FRAME, v, 0, 1, last_frame_mipmaps) : texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		RID trace = p_render_buffers->get_texture_slice(RB_SCOPE_SSGI, RB_SSGI_TRACE, v, 0);
		RID surface[2] = { p_render_buffers->get_texture_slice(RB_SCOPE_SSGI, surface_names[0], v, 0), p_render_buffers->get_texture_slice(RB_SCOPE_SSGI, surface_names[1], v, 0) };
		RID history[2] = { p_render_buffers->get_texture_slice(RB_SCOPE_SSGI, history_names[0], v, 0), p_render_buffers->get_texture_slice(RB_SCOPE_SSGI, history_names[1], v, 0) };
		RID meta[2] = { p_render_buffers->get_texture_slice(RB_SCOPE_SSGI, meta_names[0], v, 0), p_render_buffers->get_texture_slice(RB_SCOPE_SSGI, meta_names[1], v, 0) };
		RID denoise[2] = { p_render_buffers->get_texture_slice(RB_SCOPE_SSGI, denoise_names[0], v, 0), p_render_buffers->get_texture_slice(RB_SCOPE_SSGI, denoise_names[1], v, 0) };
		RID variance[2] = { p_render_buffers->get_texture_slice(RB_SCOPE_SSGI, variance_names[0], v, 0), p_render_buffers->get_texture_slice(RB_SCOPE_SSGI, variance_names[1], v, 0) };
		RID final = p_render_buffers->get_texture_slice(RB_SCOPE_SSGI, RB_SSGI_FINAL, v, 0);

		RD::Uniform u_scene_data(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, scene_data_ubo);

		/* PASS 1: trace */
		{
			RD::get_singleton()->draw_command_begin_label("Trace");
			RID shader_rid = shader.version_get_shader(shader_version, MODE_TRACE);

			RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, depth_texture }));
			RD::Uniform u_normal(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest_sampler, normal_texture }));
			RD::Uniform u_last_frame(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ linear_mipmap_sampler, last_frame }));
			RD::Uniform u_trace(RD::UNIFORM_TYPE_IMAGE, 4, Vector<RID>({ trace }));
			RD::Uniform u_surface(RD::UNIFORM_TYPE_IMAGE, 5, Vector<RID>({ surface[write] }));

			RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_TRACE].get_rid());
			RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_scene_data, u_depth, u_normal, u_last_frame, u_trace, u_surface), 0);
			RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
			RD::get_singleton()->compute_list_dispatch_threads(compute_list, working_size.x, working_size.y, 1);
			RD::get_singleton()->compute_list_add_barrier(compute_list);
			RD::get_singleton()->draw_command_end_label();
		}

		/* PASS 2: share the rays between the pixels around, into the first denoise texture (free until the
		spatial filter) */
		{
			RD::get_singleton()->draw_command_begin_label("Pre-blur");
			RID shader_rid = shader.version_get_shader(shader_version, MODE_PREBLUR);

			RD::Uniform u_trace(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, trace }));
			RD::Uniform u_surface(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest_sampler, surface[write] }));
			RD::Uniform u_preblur(RD::UNIFORM_TYPE_IMAGE, 3, Vector<RID>({ denoise[0] }));

			RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_PREBLUR].get_rid());
			RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_scene_data, u_trace, u_surface, u_preblur), 0);
			RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
			RD::get_singleton()->compute_list_dispatch_threads(compute_list, working_size.x, working_size.y, 1);
			RD::get_singleton()->compute_list_add_barrier(compute_list);
			RD::get_singleton()->draw_command_end_label();
		}

		/* PASS 3: temporal accumulation */
		{
			RD::get_singleton()->draw_command_begin_label("Temporal Accumulation");
			RID shader_rid = shader.version_get_shader(shader_version, MODE_TEMPORAL);

			RD::Uniform u_trace(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, denoise[0] }));
			RD::Uniform u_surface(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest_sampler, surface[write] }));
			RD::Uniform u_prev_surface(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ nearest_sampler, surface[read] }));
			RD::Uniform u_prev_history(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ nearest_sampler, history[read] }));
			RD::Uniform u_prev_meta(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ nearest_sampler, meta[read] }));
			RD::Uniform u_history(RD::UNIFORM_TYPE_IMAGE, 6, Vector<RID>({ history[write] }));
			RD::Uniform u_meta(RD::UNIFORM_TYPE_IMAGE, 7, Vector<RID>({ meta[write] }));
			RD::Uniform u_variance(RD::UNIFORM_TYPE_IMAGE, 8, Vector<RID>({ variance[0] }));

			RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_TEMPORAL].get_rid());
			RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_scene_data, u_trace, u_surface, u_prev_surface, u_prev_history, u_prev_meta, u_history, u_meta, u_variance), 0);
			RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
			RD::get_singleton()->compute_list_dispatch_threads(compute_list, working_size.x, working_size.y, 1);
			RD::get_singleton()->compute_list_add_barrier(compute_list);
			RD::get_singleton()->draw_command_end_label();
		}

		/* PASS 4: spatial filter, a pass per level, each twice as wide as the one before */
		RID filtered = history[write];
		{
			RD::get_singleton()->draw_command_begin_label("Denoise");
			RID shader_rid = shader.version_get_shader(shader_version, MODE_DENOISE);
			RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_DENOISE].get_rid());

			RID source_color = history[write];
			RID source_variance = variance[0];
			for (int i = 0; i < denoise_passes; i++) {
				uint32_t dest = (i + 1) % 2;
				push_constant.step_size = 1 << i;

				RD::Uniform u_color(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, source_color }));
				RD::Uniform u_variance(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest_sampler, source_variance }));
				RD::Uniform u_surface(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ nearest_sampler, surface[write] }));
				RD::Uniform u_color_out(RD::UNIFORM_TYPE_IMAGE, 4, Vector<RID>({ denoise[dest] }));
				RD::Uniform u_variance_out(RD::UNIFORM_TYPE_IMAGE, 5, Vector<RID>({ variance[dest] }));

				RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_scene_data, u_color, u_variance, u_surface, u_color_out, u_variance_out), 0);
				RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
				RD::get_singleton()->compute_list_dispatch_threads(compute_list, working_size.x, working_size.y, 1);
				RD::get_singleton()->compute_list_add_barrier(compute_list);

				source_color = denoise[dest];
				source_variance = variance[dest];
				filtered = denoise[dest];
			}
			push_constant.step_size = 0;
			RD::get_singleton()->draw_command_end_label();
		}

		/* PASS 5: resolve to full resolution, apply intensity and occlusion */
		{
			// draw_command_begin_label() takes a Span<char>, which a string literal converts to, but not the
			// const char * a ternary between two decays to.
			if (use_half_size) {
				RD::get_singleton()->draw_command_begin_label("Upscale");
			} else {
				RD::get_singleton()->draw_command_begin_label("Apply");
			}
			Mode mode = use_half_size ? MODE_APPLY_UPSCALE : MODE_APPLY;
			RID shader_rid = shader.version_get_shader(shader_version, mode);

			RD::Uniform u_color(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, filtered }));
			RD::Uniform u_surface(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest_sampler, surface[write] }));
			RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ nearest_sampler, depth_texture }));
			RD::Uniform u_normal(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ nearest_sampler, normal_texture }));
			RD::Uniform u_final(RD::UNIFORM_TYPE_IMAGE, 5, Vector<RID>({ final }));

			RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[mode].get_rid());
			RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_scene_data, u_color, u_surface, u_depth, u_normal, u_final), 0);
			RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
			RD::get_singleton()->compute_list_dispatch_threads(compute_list, full_size.x, full_size.y, 1);
			if (v + 1 < view_count) {
				RD::get_singleton()->compute_list_add_barrier(compute_list);
			}
			RD::get_singleton()->draw_command_end_label();
		}
	}

	RD::get_singleton()->compute_list_end();
	RD::get_singleton()->draw_command_end_label(); // SSGI

	p_ssgi_buffers.prev_cam_transform = p_cam_transform;
	for (uint32_t v = 0; v < view_count; v++) {
		p_ssgi_buffers.prev_projections[v] = projections[v];
	}
	p_ssgi_buffers.history_valid = true;
	p_ssgi_buffers.history_index = read;
	p_ssgi_buffers.frame++;
}
