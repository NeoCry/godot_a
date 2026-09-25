/**************************************************************************/
/*  hmao.cpp                                                              */
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

#include "hmao.h"

#include "core/config/project_settings.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

HeightMapAO *HeightMapAO::singleton = nullptr;

HeightMapAO::HeightMapAO() {
	singleton = this;

	{
		RD::SamplerState nearest_state;
		nearest_state.mag_filter = RD::SAMPLER_FILTER_NEAREST;
		nearest_state.min_filter = RD::SAMPLER_FILTER_NEAREST;
		nearest_state.mip_filter = RD::SAMPLER_FILTER_NEAREST;
		nearest_state.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
		nearest_state.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
		nearest_sampler = RD::get_singleton()->sampler_create(nearest_state);

		RD::SamplerState border_state;
		border_state.mag_filter = RD::SAMPLER_FILTER_NEAREST;
		border_state.min_filter = RD::SAMPLER_FILTER_NEAREST;
		border_state.mip_filter = RD::SAMPLER_FILTER_NEAREST;
		border_state.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_BORDER;
		border_state.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_BORDER;
		border_state.border_color = RD::SAMPLER_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
		border_sampler = RD::get_singleton()->sampler_create(border_state);
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
		upscale.shader.initialize(modes);
		upscale.shader_version = upscale.shader.version_create();
		upscale.pipeline.create_compute_pipeline(upscale.shader.version_get_shader(upscale.shader_version, 0));
	}

	set_quality(RSE::EnvironmentHMAOQuality(int(GLOBAL_GET("rendering/environment/hmao/quality"))), GLOBAL_GET("rendering/environment/hmao/half_size"));
}

HeightMapAO::~HeightMapAO() {
	_free_height_map();

	gather.pipeline.free();
	gather.shader.version_free(gather.shader_version);

	upscale.pipeline.free();
	upscale.shader.version_free(upscale.shader_version);

	RD::get_singleton()->free_rid(nearest_sampler);
	RD::get_singleton()->free_rid(border_sampler);

	singleton = nullptr;
}

void HeightMapAO::set_quality(RSE::EnvironmentHMAOQuality p_quality, bool p_half_size) {
	quality = p_quality;
	half_size = p_half_size;
}

void HeightMapAO::_free_height_map() {
	if (height_map.texture.is_valid()) {
		// Frees the framebuffer along with it, the way every other render target in the renderer is
		// released: framebuffers are owned by their attachments.
		RD::get_singleton()->free_rid(height_map.texture);
	}
	height_map.texture = RID();
	height_map.framebuffer = RID();
	height_map.resolution = 0;
	height_map.valid = false;
}

RID HeightMapAO::prepare_height_map(uint32_t p_resolution) {
	ERR_FAIL_COND_V(p_resolution == 0, RID());

	if (height_map.texture.is_valid() && height_map.resolution == p_resolution) {
		return height_map.framebuffer;
	}

	_free_height_map();

	RD::TextureFormat tf;
	tf.format = RD::DATA_FORMAT_D32_SFLOAT;
	tf.width = p_resolution;
	tf.height = p_resolution;
	tf.texture_type = RD::TEXTURE_TYPE_2D;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

	height_map.texture = RD::get_singleton()->texture_create(tf, RD::TextureView());
	ERR_FAIL_COND_V(height_map.texture.is_null(), RID());
	RD::get_singleton()->set_resource_name(height_map.texture, "HMAO Height Map");

	Vector<RID> fb_tex;
	fb_tex.push_back(height_map.texture);
	height_map.framebuffer = RD::get_singleton()->framebuffer_create(fb_tex);

	height_map.resolution = p_resolution;

	return height_map.framebuffer;
}

void HeightMapAO::height_map_rendered(const AABB &p_bounds) {
	height_map.bounds = p_bounds;
	height_map.valid = true;
}

void HeightMapAO::allocate_buffers(Ref<RenderSceneBuffersRD> p_render_buffers, RenderBuffers &p_hmao_buffers, const Settings &p_settings) {
	Size2i working_size = half_size ? Size2i((p_settings.full_screen_size.x + 1) / 2, (p_settings.full_screen_size.y + 1) / 2) : p_settings.full_screen_size;
	working_size = working_size.maxi(1);

	if (p_hmao_buffers.half_size != half_size || p_hmao_buffers.buffer_width != working_size.x || p_hmao_buffers.buffer_height != working_size.y) {
		p_render_buffers->clear_context(RB_SCOPE_HMAO);
	}

	p_hmao_buffers.half_size = half_size;
	p_hmao_buffers.buffer_width = working_size.x;
	p_hmao_buffers.buffer_height = working_size.y;

	uint32_t view_count = p_render_buffers->get_view_count();

	// As we're not clearing these, and render buffers will return the cached texture if it already exists,
	// we don't first check has_texture here.
	p_render_buffers->create_texture(RB_SCOPE_HMAO, RB_HMAO_GATHER, RD::DATA_FORMAT_R16G16_SFLOAT, RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, working_size, view_count);
	p_render_buffers->create_texture(RB_SCOPE_HMAO, RB_HMAO_FINAL, RD::DATA_FORMAT_R8_UNORM, RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1);
}

void HeightMapAO::generate(Ref<RenderSceneBuffersRD> p_render_buffers, RenderBuffers &p_hmao_buffers, uint32_t p_view, RID p_normal_buffer, const Projection &p_projection, const Transform3D &p_cam_transform, const Settings &p_settings) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	ERR_FAIL_COND(!is_height_map_valid());

	Size2i working_size(p_hmao_buffers.buffer_width, p_hmao_buffers.buffer_height);
	Size2i full_size = p_settings.full_screen_size;
	bool is_orthogonal = p_projection.is_orthogonal();

	// Depth linearization constants, derived exactly as effects/gtao.cpp derives its own (which in turn
	// mirrors shaders/effects/ss_effects_downsample.glsl): both passes below read hardware depth directly.
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

	RID depth_texture = p_render_buffers->get_depth_texture(p_view);
	RID gather_texture = p_render_buffers->get_texture_slice(RB_SCOPE_HMAO, RB_HMAO_GATHER, p_view, 0);
	RID final_texture = p_render_buffers->get_texture_slice(RB_SCOPE_HMAO, RB_HMAO_FINAL, p_view, 0);

	const AABB &bounds = height_map.bounds;
	float map_size = MAX(bounds.size.x, 0.001);

	RD::get_singleton()->draw_command_begin_label("Process Height Map Ambient Occlusion");
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();

	/* PASS 1: world space gather over the height map */
	{
		RD::get_singleton()->draw_command_begin_label("Gather Height Map Occlusion");

		float tan_half_fov_x = 1.0 / p_projection.columns[0][0];
		float tan_half_fov_y = 1.0 / p_projection.columns[1][1];

		memset(&gather.push_constant, 0, sizeof(GatherPushConstant));

		// The camera transform, packed as three basis columns carrying the origin in their w components.
		Vector3 basis_columns[3] = { p_cam_transform.basis.get_column(0), p_cam_transform.basis.get_column(1), p_cam_transform.basis.get_column(2) };
		for (int i = 0; i < 3; i++) {
			gather.push_constant.cam_basis_x[i] = basis_columns[0][i];
			gather.push_constant.cam_basis_y[i] = basis_columns[1][i];
			gather.push_constant.cam_basis_z[i] = basis_columns[2][i];
		}
		gather.push_constant.cam_basis_x[3] = p_cam_transform.origin.x;
		gather.push_constant.cam_basis_y[3] = p_cam_transform.origin.y;
		gather.push_constant.cam_basis_z[3] = p_cam_transform.origin.z;

		gather.push_constant.screen_size[0] = working_size.x;
		gather.push_constant.screen_size[1] = working_size.y;
		gather.push_constant.full_screen_size[0] = full_size.x;
		gather.push_constant.full_screen_size[1] = full_size.y;

		gather.push_constant.NDC_to_view_mul_x = tan_half_fov_x * 2.0;
		gather.push_constant.NDC_to_view_mul_y = tan_half_fov_y * -2.0;
		gather.push_constant.NDC_to_view_add_x = tan_half_fov_x * -1.0;
		gather.push_constant.NDC_to_view_add_y = tan_half_fov_y;

		gather.push_constant.depth_linearize_mul = depth_linearize_mul;
		gather.push_constant.depth_linearize_add = depth_linearize_add;
		gather.push_constant.flags = (is_orthogonal ? 1 : 0) | (uint32_t(CLAMP(int(quality), 0, int(RSE::ENV_HMAO_QUALITY_MAX) - 1)) << 8);
		gather.push_constant.amount = p_settings.amount;

		gather.push_constant.map_origin[0] = bounds.position.x;
		gather.push_constant.map_origin[1] = bounds.position.z;
		gather.push_constant.map_size = map_size;
		gather.push_constant.map_bottom = bounds.position.y;
		gather.push_constant.map_height = bounds.size.y;

		// The gather reaches from the shading point out to the edge of a map centered on it, which is what
		// ties Environment.hmao_range to the scale of the occluders this effect can see at all.
		gather.push_constant.radius = map_size * 0.5;
		// One height map texel is the finest height detail the map holds, and within a texel it holds the
		// tallest thing that covered it - so anything within half a texel of the shading point's own height
		// is as likely to be the surface it stands on as a real occluder.
		gather.push_constant.bias = CLAMP(map_size / float(MAX(height_map.resolution, 1u)) * 0.5, 0.05, 4.0);
		gather.push_constant.edge_fade = 0.1;

		RID shader = gather.shader.version_get_shader(gather.shader_version, 0);

		RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, depth_texture }));
		RD::Uniform u_normal(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ p_normal_buffer }));
		RD::Uniform u_height_map(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ border_sampler, height_map.texture }));
		RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ gather_texture }));

		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, gather.pipeline.get_rid());
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 0, u_depth, u_normal, u_height_map), 0);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 1, u_dest), 1);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &gather.push_constant, sizeof(GatherPushConstant));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, working_size.x, working_size.y, 1);
		RD::get_singleton()->compute_list_add_barrier(compute_list);

		RD::get_singleton()->draw_command_end_label(); // Gather Height Map Occlusion
	}

	/* PASS 2: depth aware blur, upscaled to the buffer the lighting pass samples */
	{
		RD::get_singleton()->draw_command_begin_label("Resolve Height Map Occlusion");

		memset(&upscale.push_constant, 0, sizeof(UpscalePushConstant));
		upscale.push_constant.full_screen_size[0] = full_size.x;
		upscale.push_constant.full_screen_size[1] = full_size.y;
		upscale.push_constant.source_size[0] = working_size.x;
		upscale.push_constant.source_size[1] = working_size.y;
		upscale.push_constant.is_orthogonal = is_orthogonal;
		upscale.push_constant.depth_linearize_mul = depth_linearize_mul;
		upscale.push_constant.depth_linearize_add = depth_linearize_add;
		upscale.push_constant.blur_radius = 1;

		RID shader = upscale.shader.version_get_shader(upscale.shader_version, 0);

		RD::Uniform u_full_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, depth_texture }));
		RD::Uniform u_source(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, gather_texture }));
		RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ final_texture }));

		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, upscale.pipeline.get_rid());
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 0, u_full_depth, u_source), 0);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 1, u_dest), 1);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &upscale.push_constant, sizeof(UpscalePushConstant));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, full_size.x, full_size.y, 1);

		RD::get_singleton()->draw_command_end_label(); // Resolve Height Map Occlusion
	}

	RD::get_singleton()->compute_list_end();
	RD::get_singleton()->draw_command_end_label(); // Process Height Map Ambient Occlusion
}
