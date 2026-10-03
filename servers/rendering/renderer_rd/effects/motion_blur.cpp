/**************************************************************************/
/*  motion_blur.cpp                                                       */
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

#include "motion_blur.h"

#include "core/config/project_settings.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_data_rd.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

MotionBlur *MotionBlur::singleton = nullptr;

// The number of samples N of the Low, Medium, High and Ultra presets. Odd, so that the middle one is the pixel
// itself, which the reconstruction filter weights on its own.
static constexpr int MOTION_BLUR_SAMPLE_COUNT[RSE::ENV_MOTION_BLUR_QUALITY_MAX] = { 9, 15, 25, 35 };

// The velocity and depth buffer is half precision: keep the depth within its range.
static constexpr float MOTION_BLUR_MAX_DEPTH = 65000.0;

MotionBlur::MotionBlur() {
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

	view_data_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(ViewDataBlock));

	{
		Vector<String> modes;
		modes.push_back("\n");
		prepare.shader.initialize(modes);
		prepare.shader_version = prepare.shader.version_create();
		prepare.pipeline.create_compute_pipeline(prepare.shader.version_get_shader(prepare.shader_version, 0));
	}

	{
		Vector<String> modes;
		modes.push_back("\n#define MODE_TILE_MAX_X\n"); // TILE_MAX_MODE_X
		modes.push_back("\n#define MODE_TILE_MAX_Y\n"); // TILE_MAX_MODE_Y
		tile_max.shader.initialize(modes);
		tile_max.shader_version = tile_max.shader.version_create();
		for (int i = 0; i < TILE_MAX_MODE_MAX; i++) {
			tile_max.pipelines[i].create_compute_pipeline(tile_max.shader.version_get_shader(tile_max.shader_version, i));
		}
	}

	{
		Vector<String> modes;
		modes.push_back("\n");
		neighbor_max.shader.initialize(modes);
		neighbor_max.shader_version = neighbor_max.shader.version_create();
		neighbor_max.pipeline.create_compute_pipeline(neighbor_max.shader.version_get_shader(neighbor_max.shader_version, 0));
	}

	{
		Vector<String> modes;
		modes.push_back("\n");
		gather.shader.initialize(modes);
		gather.shader_version = gather.shader.version_create();
		gather.pipeline.create_compute_pipeline(gather.shader.version_get_shader(gather.shader_version, 0));
	}

	set_quality(RSE::EnvironmentMotionBlurQuality(int(GLOBAL_GET("rendering/environment/motion_blur/quality"))));
}

MotionBlur::~MotionBlur() {
	prepare.pipeline.free();
	prepare.shader.version_free(prepare.shader_version);

	for (int i = 0; i < TILE_MAX_MODE_MAX; i++) {
		tile_max.pipelines[i].free();
	}
	tile_max.shader.version_free(tile_max.shader_version);

	neighbor_max.pipeline.free();
	neighbor_max.shader.version_free(neighbor_max.shader_version);

	gather.pipeline.free();
	gather.shader.version_free(gather.shader_version);

	RD::get_singleton()->free_rid(view_data_buffer);
	RD::get_singleton()->free_rid(nearest_sampler);

	singleton = nullptr;
}

void MotionBlur::set_quality(RSE::EnvironmentMotionBlurQuality p_quality) {
	quality = RSE::EnvironmentMotionBlurQuality(CLAMP(int(p_quality), 0, int(RSE::ENV_MOTION_BLUR_QUALITY_MAX) - 1));
}

// The paper's tile size k is the maximum blur radius r, so that NeighborMax, which looks one tile away, sees
// everything that can blur over a pixel.
int MotionBlur::_get_tile_size(const Settings &p_settings, const Size2i &p_size) {
	return int(Math::round(p_settings.max_radius * float(p_size.y)));
}

bool MotionBlur::is_active(const Settings &p_settings, const Size2i &p_size) {
	if (p_settings.intensity <= 0.0f) {
		return false;
	}
	if (p_settings.camera_rotation_scale == 0.0f && p_settings.camera_movement_scale == 0.0f && p_settings.object_scale == 0.0f) {
		return false;
	}
	// Blurs shorter than a pixel to either side are below what the reconstruction filter acts on.
	return _get_tile_size(p_settings, p_size) >= 1;
}

void MotionBlur::_allocate_buffers(Ref<RenderSceneBuffersRD> p_render_buffers, const Size2i &p_size, const Size2i &p_tile_count) {
	bool reallocate = !p_render_buffers->has_texture(RB_SCOPE_MOTION_BLUR, RB_MOTION_BLUR_NEIGHBOR_MAX);
	if (!reallocate) {
		// The tile count changes with the maximum blur radius as well as with the resolution.
		RD::TextureFormat color_format = p_render_buffers->get_texture_format(RB_SCOPE_MOTION_BLUR, RB_MOTION_BLUR_COLOR);
		RD::TextureFormat tile_format = p_render_buffers->get_texture_format(RB_SCOPE_MOTION_BLUR, RB_MOTION_BLUR_NEIGHBOR_MAX);
		reallocate = int(color_format.width) != p_size.x || int(color_format.height) != p_size.y || int(tile_format.width) != p_tile_count.x || int(tile_format.height) != p_tile_count.y;
	}
	if (!reallocate) {
		return;
	}

	p_render_buffers->clear_context(RB_SCOPE_MOTION_BLUR);

	uint32_t view_count = p_render_buffers->get_view_count();
	uint32_t usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;

	p_render_buffers->create_texture(RB_SCOPE_MOTION_BLUR, RB_MOTION_BLUR_COLOR, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_size, view_count);
	p_render_buffers->create_texture(RB_SCOPE_MOTION_BLUR, RB_MOTION_BLUR_VELOCITY_DEPTH, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_size, view_count);
	p_render_buffers->create_texture(RB_SCOPE_MOTION_BLUR, RB_MOTION_BLUR_TILE_MAX_X, RD::DATA_FORMAT_R16G16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, Size2i(p_tile_count.x, p_size.y), view_count);
	p_render_buffers->create_texture(RB_SCOPE_MOTION_BLUR, RB_MOTION_BLUR_TILE_MAX, RD::DATA_FORMAT_R16G16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_tile_count, view_count);
	p_render_buffers->create_texture(RB_SCOPE_MOTION_BLUR, RB_MOTION_BLUR_NEIGHBOR_MAX, RD::DATA_FORMAT_R16G16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_tile_count, view_count);
}

void MotionBlur::process(Ref<RenderSceneBuffersRD> p_render_buffers, const RenderSceneDataRD *p_scene_data, bool p_use_upscaled_texture, const Settings &p_settings) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	ERR_FAIL_COND(p_render_buffers.is_null());
	ERR_FAIL_NULL(p_scene_data);

	Size2i size = p_use_upscaled_texture ? p_render_buffers->get_target_size() : p_render_buffers->get_internal_size();
	Size2i source_size = p_render_buffers->get_internal_size();
	int tile_size = _get_tile_size(p_settings, size);
	ERR_FAIL_COND(tile_size < 1);
	Size2i tile_count((size.x + tile_size - 1) / tile_size, (size.y + tile_size - 1) / tile_size);
	uint32_t view_count = p_render_buffers->get_view_count();

	_allocate_buffers(p_render_buffers, size, tile_count);

	// Maps each pixel back to where it was in the previous frame, from its depth and the two frames' cameras.
	// The motion vectors already hold the full motion; this splits the camera's out of it. The depth correction
	// is the one the motion vectors debug view and FSR 2 use, so the NDC are those of the [0, 1] reverse Z depth
	// buffer remapped to [-1, 1], with Y pointing down the image.
	ViewDataBlock view_data;
	memset(&view_data, 0, sizeof(ViewDataBlock));
	Projection correction;
	correction.set_depth_correction(true, true, false);
	for (uint32_t v = 0; v < view_count; v++) {
		// Each eye sits at the same offset from the camera in both frames.
		Transform3D eye_offset(Basis(), p_scene_data->view_eye_offset[v]);
		Transform3D transform = p_scene_data->cam_transform * eye_offset;
		Transform3D prev_transform = p_scene_data->prev_cam_transform * eye_offset;
		// The previous frame's orientation at the current frame's position: what the rotation (and any change of
		// projection) did on its own.
		Transform3D prev_rotation_transform(prev_transform.basis, transform.origin);

		Projection inv_projection = (correction * p_scene_data->view_projection[v]).inverse();
		Projection prev_projection = correction * p_scene_data->prev_view_projection[v];

		MaterialStorage::store_camera(prev_projection * Projection(prev_transform.affine_inverse() * transform) * inv_projection, view_data.views[v].reprojection);
		MaterialStorage::store_camera(prev_projection * Projection(prev_rotation_transform.affine_inverse() * transform) * inv_projection, view_data.views[v].reprojection_rotation);
		MaterialStorage::store_camera(inv_projection, view_data.views[v].inv_projection);
	}
	RD::get_singleton()->buffer_update(view_data_buffer, 0, sizeof(ViewDataBlock), &view_data);

	RD::get_singleton()->draw_command_begin_label("Motion Blur");
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();

	for (uint32_t v = 0; v < view_count; v++) {
		RID color = p_use_upscaled_texture ? p_render_buffers->get_upscaled_texture(v) : p_render_buffers->get_internal_texture(v);
		RID velocity = p_render_buffers->get_velocity_buffer(false, v);
		RID depth = p_render_buffers->get_depth_texture(v);
		RID color_copy = p_render_buffers->get_texture_slice(RB_SCOPE_MOTION_BLUR, RB_MOTION_BLUR_COLOR, v, 0);
		RID velocity_depth = p_render_buffers->get_texture_slice(RB_SCOPE_MOTION_BLUR, RB_MOTION_BLUR_VELOCITY_DEPTH, v, 0);
		RID tile_max_x_texture = p_render_buffers->get_texture_slice(RB_SCOPE_MOTION_BLUR, RB_MOTION_BLUR_TILE_MAX_X, v, 0);
		RID tile_max_texture = p_render_buffers->get_texture_slice(RB_SCOPE_MOTION_BLUR, RB_MOTION_BLUR_TILE_MAX, v, 0);
		RID neighbor_max_texture = p_render_buffers->get_texture_slice(RB_SCOPE_MOTION_BLUR, RB_MOTION_BLUR_NEIGHBOR_MAX, v, 0);

		/* PASS 1: velocity and depth buffers, and the copy of the color buffer */
		{
			memset(&prepare.push_constant, 0, sizeof(PreparePushConstant));
			prepare.push_constant.size[0] = size.x;
			prepare.push_constant.size[1] = size.y;
			prepare.push_constant.source_size[0] = source_size.x;
			prepare.push_constant.source_size[1] = source_size.y;
			// Half the motion over the time the shutter is open.
			prepare.push_constant.velocity_scale = p_settings.intensity * 0.5f;
			prepare.push_constant.max_radius = float(tile_size);
			prepare.push_constant.camera_rotation_scale = p_settings.camera_rotation_scale;
			prepare.push_constant.camera_movement_scale = p_settings.camera_movement_scale;
			prepare.push_constant.object_scale = p_settings.object_scale;
			prepare.push_constant.view = v;
			prepare.push_constant.max_depth = MOTION_BLUR_MAX_DEPTH;

			RID shader = prepare.shader.version_get_shader(prepare.shader_version, 0);

			RD::Uniform u_color(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, color }));
			RD::Uniform u_velocity(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, velocity }));
			RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest_sampler, depth }));
			RD::Uniform u_dest_color(RD::UNIFORM_TYPE_IMAGE, 0, color_copy);
			RD::Uniform u_dest_velocity_depth(RD::UNIFORM_TYPE_IMAGE, 1, velocity_depth);
			RD::Uniform u_view_data(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, view_data_buffer);

			RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, prepare.pipeline.get_rid());
			RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 0, u_color, u_velocity, u_depth), 0);
			RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 1, u_dest_color, u_dest_velocity_depth), 1);
			RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 2, u_view_data), 2);
			RD::get_singleton()->compute_list_set_push_constant(compute_list, &prepare.push_constant, sizeof(PreparePushConstant));
			RD::get_singleton()->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);
			RD::get_singleton()->compute_list_add_barrier(compute_list);
		}

		/* PASS 2: TileMax, along rows and then along columns */
		{
			Size2i tile_max_x_size(tile_count.x, size.y);

			memset(&tile_max.push_constant, 0, sizeof(TileMaxPushConstant));
			tile_max.push_constant.source_size[0] = size.x;
			tile_max.push_constant.source_size[1] = size.y;
			tile_max.push_constant.dest_size[0] = tile_max_x_size.x;
			tile_max.push_constant.dest_size[1] = tile_max_x_size.y;
			tile_max.push_constant.tile_size = tile_size;

			RID shader = tile_max.shader.version_get_shader(tile_max.shader_version, TILE_MAX_MODE_X);

			RD::Uniform u_source(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, velocity_depth }));
			RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 0, tile_max_x_texture);

			RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, tile_max.pipelines[TILE_MAX_MODE_X].get_rid());
			RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 0, u_source), 0);
			RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 1, u_dest), 1);
			RD::get_singleton()->compute_list_set_push_constant(compute_list, &tile_max.push_constant, sizeof(TileMaxPushConstant));
			RD::get_singleton()->compute_list_dispatch_threads(compute_list, tile_max_x_size.x, tile_max_x_size.y, 1);
			RD::get_singleton()->compute_list_add_barrier(compute_list);

			tile_max.push_constant.source_size[0] = tile_max_x_size.x;
			tile_max.push_constant.source_size[1] = tile_max_x_size.y;
			tile_max.push_constant.dest_size[0] = tile_count.x;
			tile_max.push_constant.dest_size[1] = tile_count.y;

			shader = tile_max.shader.version_get_shader(tile_max.shader_version, TILE_MAX_MODE_Y);

			u_source = RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, tile_max_x_texture }));
			u_dest = RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 0, tile_max_texture);

			RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, tile_max.pipelines[TILE_MAX_MODE_Y].get_rid());
			RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 0, u_source), 0);
			RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 1, u_dest), 1);
			RD::get_singleton()->compute_list_set_push_constant(compute_list, &tile_max.push_constant, sizeof(TileMaxPushConstant));
			RD::get_singleton()->compute_list_dispatch_threads(compute_list, tile_count.x, tile_count.y, 1);
			RD::get_singleton()->compute_list_add_barrier(compute_list);
		}

		/* PASS 3: NeighborMax */
		{
			memset(&neighbor_max.push_constant, 0, sizeof(NeighborMaxPushConstant));
			neighbor_max.push_constant.size[0] = tile_count.x;
			neighbor_max.push_constant.size[1] = tile_count.y;

			RID shader = neighbor_max.shader.version_get_shader(neighbor_max.shader_version, 0);

			RD::Uniform u_source(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, tile_max_texture }));
			RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 0, neighbor_max_texture);

			RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, neighbor_max.pipeline.get_rid());
			RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 0, u_source), 0);
			RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 1, u_dest), 1);
			RD::get_singleton()->compute_list_set_push_constant(compute_list, &neighbor_max.push_constant, sizeof(NeighborMaxPushConstant));
			RD::get_singleton()->compute_list_dispatch_threads(compute_list, tile_count.x, tile_count.y, 1);
			RD::get_singleton()->compute_list_add_barrier(compute_list);
		}

		/* PASS 4: reconstruction filter, back into the color buffer */
		{
			memset(&gather.push_constant, 0, sizeof(GatherPushConstant));
			gather.push_constant.size[0] = size.x;
			gather.push_constant.size[1] = size.y;
			gather.push_constant.tile_count[0] = tile_count.x;
			gather.push_constant.tile_count[1] = tile_count.y;
			gather.push_constant.tile_size = tile_size;
			gather.push_constant.sample_count = MOTION_BLUR_SAMPLE_COUNT[quality];

			RID shader = gather.shader.version_get_shader(gather.shader_version, 0);

			RD::Uniform u_color(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, color_copy }));
			RD::Uniform u_velocity_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, velocity_depth }));
			RD::Uniform u_neighbor_max(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest_sampler, neighbor_max_texture }));
			RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 0, color);

			RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, gather.pipeline.get_rid());
			RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 0, u_color, u_velocity_depth, u_neighbor_max), 0);
			RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 1, u_dest), 1);
			RD::get_singleton()->compute_list_set_push_constant(compute_list, &gather.push_constant, sizeof(GatherPushConstant));
			RD::get_singleton()->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);

			if (v + 1 < view_count) {
				RD::get_singleton()->compute_list_add_barrier(compute_list);
			}
		}
	}

	RD::get_singleton()->compute_list_end();
	RD::get_singleton()->draw_command_end_label();
}
