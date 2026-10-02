/**************************************************************************/
/*  xegtao.cpp                                                            */
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

#include "xegtao.h"

#include "core/config/project_settings.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

XeGTAO *XeGTAO::singleton = nullptr;

// XeGTAO's auto-tuned heuristics (XeGTAO.h), matched against a ray traced ground truth. Only the ones that
// change how the scene looks are exposed in Environment; these two trade quality for performance and
// compensate for screen space biases, and are left at the values XeGTAO tuned them to.
static constexpr float XEGTAO_RADIUS_MULTIPLIER = 1.457;
static constexpr float XEGTAO_DEPTH_MIP_SAMPLING_OFFSET = 3.30;

// XE_GTAO_DEPTH_MIP_LEVELS.
static constexpr uint32_t XEGTAO_DEPTH_MIP_LEVELS = 5;

// Slices and steps per slice of the Low, Medium, High and Ultra presets (vaGTAO.hlsl's CSGTAO* entry points).
static constexpr float XEGTAO_SLICE_COUNT[RSE::ENV_XEGTAO_QUALITY_MAX] = { 1, 2, 3, 9 };
static constexpr float XEGTAO_STEPS_PER_SLICE[RSE::ENV_XEGTAO_QUALITY_MAX] = { 2, 2, 3, 3 };

XeGTAO::XeGTAO() {
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
		modes.push_back("\n");
		prefilter.shader.initialize(modes);
		prefilter.shader_version = prefilter.shader.version_create();
		prefilter.pipeline.create_compute_pipeline(prefilter.shader.version_get_shader(prefilter.shader_version, 0));
	}

	{
		Vector<String> modes;
		modes.push_back("\n"); // MAIN_MODE_AO
		modes.push_back("\n#define USE_BENT_NORMALS\n"); // MAIN_MODE_AO_BENT_NORMALS
		main_pass.shader.initialize(modes);
		main_pass.shader_version = main_pass.shader.version_create();
		for (int i = 0; i < MAIN_MODE_MAX; i++) {
			main_pass.pipelines[i].create_compute_pipeline(main_pass.shader.version_get_shader(main_pass.shader_version, i));
		}
	}

	{
		Vector<String> modes;
		modes.push_back("\n"); // DENOISE_MODE_AO
		modes.push_back("\n#define USE_BENT_NORMALS\n"); // DENOISE_MODE_AO_BENT_NORMALS
		denoise.shader.initialize(modes);
		denoise.shader_version = denoise.shader.version_create();
		for (int i = 0; i < DENOISE_MODE_MAX; i++) {
			denoise.pipelines[i].create_compute_pipeline(denoise.shader.version_get_shader(denoise.shader_version, i));
		}
	}

	{
		Vector<String> modes;
		modes.push_back("\n"); // APPLY_MODE_COPY
		modes.push_back("\n#define USE_BENT_NORMALS\n"); // APPLY_MODE_COPY_BENT_NORMALS
		modes.push_back("\n#define MODE_UPSCALE\n"); // APPLY_MODE_UPSCALE
		modes.push_back("\n#define MODE_UPSCALE\n#define USE_BENT_NORMALS\n"); // APPLY_MODE_UPSCALE_BENT_NORMALS
		apply.shader.initialize(modes);
		apply.shader_version = apply.shader.version_create();
		for (int i = 0; i < APPLY_MODE_MAX; i++) {
			apply.pipelines[i].create_compute_pipeline(apply.shader.version_get_shader(apply.shader_version, i));
		}
	}

	set_quality(RSE::EnvironmentXeGTAOQuality(int(GLOBAL_GET("rendering/environment/xegtao/quality"))), GLOBAL_GET("rendering/environment/xegtao/denoise_passes"), GLOBAL_GET("rendering/environment/xegtao/half_size"), GLOBAL_GET("rendering/environment/xegtao/fadeout_from"), GLOBAL_GET("rendering/environment/xegtao/fadeout_to"));
}

XeGTAO::~XeGTAO() {
	prefilter.pipeline.free();
	prefilter.shader.version_free(prefilter.shader_version);

	for (int i = 0; i < MAIN_MODE_MAX; i++) {
		main_pass.pipelines[i].free();
	}
	main_pass.shader.version_free(main_pass.shader_version);

	for (int i = 0; i < DENOISE_MODE_MAX; i++) {
		denoise.pipelines[i].free();
	}
	denoise.shader.version_free(denoise.shader_version);

	for (int i = 0; i < APPLY_MODE_MAX; i++) {
		apply.pipelines[i].free();
	}
	apply.shader.version_free(apply.shader_version);

	RD::get_singleton()->free_rid(nearest_sampler);

	singleton = nullptr;
}

void XeGTAO::set_quality(RSE::EnvironmentXeGTAOQuality p_quality, int p_denoise_passes, bool p_half_size, float p_fadeout_from, float p_fadeout_to) {
	quality = RSE::EnvironmentXeGTAOQuality(CLAMP(int(p_quality), 0, int(RSE::ENV_XEGTAO_QUALITY_MAX) - 1));
	denoise_passes = CLAMP(p_denoise_passes, 0, 3);
	half_size = p_half_size;
	fadeout_from = p_fadeout_from;
	fadeout_to = p_fadeout_to;
}

void XeGTAO::allocate_buffers(Ref<RenderSceneBuffersRD> p_render_buffers, RenderBuffers &p_xegtao_buffers, const Settings &p_settings) {
	Size2i working_size = half_size ? Size2i((p_settings.full_screen_size.x + 1) / 2, (p_settings.full_screen_size.y + 1) / 2) : p_settings.full_screen_size;
	working_size = working_size.maxi(1);

	if (p_xegtao_buffers.half_size != half_size || p_xegtao_buffers.bent_normals != p_settings.bent_normals || p_xegtao_buffers.buffer_width != working_size.x || p_xegtao_buffers.buffer_height != working_size.y) {
		p_render_buffers->clear_context(RB_SCOPE_XEGTAO);
	}

	p_xegtao_buffers.half_size = half_size;
	p_xegtao_buffers.bent_normals = p_settings.bent_normals;
	p_xegtao_buffers.buffer_width = working_size.x;
	p_xegtao_buffers.buffer_height = working_size.y;

	// As many levels as the working size has, up to XeGTAO's 5.
	int max_dim = MAX(working_size.x, working_size.y);
	p_xegtao_buffers.mip_count = MIN(uint32_t(Math::floor(Math::log2(double(max_dim)))) + 1, XEGTAO_DEPTH_MIP_LEVELS);

	uint32_t view_count = p_render_buffers->get_view_count();
	uint32_t usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;

	// As we're not clearing these, and render buffers will return the cached texture if it already exists,
	// we don't first check has_texture here.

	// View space depth. XeGTAO defaults to 16-bit depths, but those lose too much precision over the depth
	// ranges Godot scenes span (and cap out at 65504), so this is its 32-bit variant (XE_GTAO_FP32_DEPTHS).
	p_render_buffers->create_texture(RB_SCOPE_XEGTAO, RB_XEGTAO_DEPTH, RD::DATA_FORMAT_R32_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, working_size, view_count, p_xegtao_buffers.mip_count);
	p_render_buffers->create_texture(RB_SCOPE_XEGTAO, RB_XEGTAO_EDGES, RD::DATA_FORMAT_R8_UNORM, usage_bits, RD::TEXTURE_SAMPLES_1, working_size, view_count);
	// The working AO term, ping-ponged between the denoise passes.
	p_render_buffers->create_texture(RB_SCOPE_XEGTAO, RB_XEGTAO_TERM_A, RD::DATA_FORMAT_R32_UINT, usage_bits, RD::TEXTURE_SAMPLES_1, working_size, view_count);
	p_render_buffers->create_texture(RB_SCOPE_XEGTAO, RB_XEGTAO_TERM_B, RD::DATA_FORMAT_R32_UINT, usage_bits, RD::TEXTURE_SAMPLES_1, working_size, view_count);

	p_render_buffers->create_texture(RB_SCOPE_XEGTAO, RB_XEGTAO_FINAL, RD::DATA_FORMAT_R8_UNORM, usage_bits, RD::TEXTURE_SAMPLES_1);
	if (p_settings.bent_normals) {
		p_render_buffers->create_texture(RB_SCOPE_XEGTAO, RB_XEGTAO_BENT_NORMAL, RD::DATA_FORMAT_R8G8B8A8_UNORM, usage_bits, RD::TEXTURE_SAMPLES_1);
	}
}

// Precomputes the falloff of XeGTAO_MainPass() and XeGTAO_DepthMIPFilter(): a sample's weight fades from 1
// to 0 over the last p_falloff_range fraction of p_radius. A zero range would divide by zero; this turns it
// into a cutoff a hair past the radius instead.
static void compute_falloff(float p_radius, float p_falloff_range, float &r_mul, float &r_add) {
	float falloff_range = MAX(p_falloff_range * p_radius, 1e-5f);
	float falloff_from = p_radius * (1.0f - p_falloff_range);
	r_mul = -1.0f / falloff_range;
	r_add = falloff_from / falloff_range + 1.0f;
}

void XeGTAO::generate(Ref<RenderSceneBuffersRD> p_render_buffers, RenderBuffers &p_xegtao_buffers, uint32_t p_view, RID p_normal_buffer, const Projection &p_projection, const Settings &p_settings) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);

	Size2i working_size(p_xegtao_buffers.buffer_width, p_xegtao_buffers.buffer_height);
	Size2i full_size = p_settings.full_screen_size;
	bool is_orthogonal = p_projection.is_orthogonal();
	bool use_half_size = p_xegtao_buffers.half_size;
	bool use_bent_normals = p_xegtao_buffers.bent_normals;
	uint32_t mip_count = p_xegtao_buffers.mip_count;

	// Hardware depth (reverse Z, [0, 1]) to linear view space depth, as in XeGTAO's GTAOUpdateConstants().
	Projection depth_correction;
	depth_correction.set_depth_correction(false);
	Projection linearize_source = depth_correction * p_projection;
	float depth_linearize_mul = -linearize_source.columns[3][2];
	float depth_linearize_add = linearize_source.columns[2][2];
	if (depth_linearize_mul * depth_linearize_add < 0) {
		depth_linearize_add = -depth_linearize_add;
	}
	float z_near = p_projection.get_z_near();
	float z_far = p_projection.get_z_far();

	float effect_radius = p_settings.radius * XEGTAO_RADIUS_MULTIPLIER;

	RID depth_texture = p_render_buffers->get_texture_slice(RB_SCOPE_XEGTAO, RB_XEGTAO_DEPTH, p_view, 0, 1, mip_count);
	RID depth_mip0 = p_render_buffers->get_texture_slice(RB_SCOPE_XEGTAO, RB_XEGTAO_DEPTH, p_view, 0);
	RID edges_texture = p_render_buffers->get_texture_slice(RB_SCOPE_XEGTAO, RB_XEGTAO_EDGES, p_view, 0);
	RID term_textures[2] = {
		p_render_buffers->get_texture_slice(RB_SCOPE_XEGTAO, RB_XEGTAO_TERM_A, p_view, 0),
		p_render_buffers->get_texture_slice(RB_SCOPE_XEGTAO, RB_XEGTAO_TERM_B, p_view, 0),
	};
	RID final_texture = p_render_buffers->get_texture_slice(RB_SCOPE_XEGTAO, RB_XEGTAO_FINAL, p_view, 0);
	RID bent_normal_texture = use_bent_normals ? p_render_buffers->get_texture_slice(RB_SCOPE_XEGTAO, RB_XEGTAO_BENT_NORMAL, p_view, 0) : RID();
	RID depth_hw_texture = p_render_buffers->get_depth_texture(p_view);

	RD::get_singleton()->draw_command_begin_label("Process XeGTAO");
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();

	/* PASS 1: view space depth + MIP chain (XeGTAO_PrefilterDepths16x16) */
	{
		RD::get_singleton()->draw_command_begin_label("Prefilter Depths");

		memset(&prefilter.push_constant, 0, sizeof(PrefilterPushConstant));
		prefilter.push_constant.source_size[0] = full_size.x;
		prefilter.push_constant.source_size[1] = full_size.y;
		prefilter.push_constant.working_size[0] = working_size.x;
		prefilter.push_constant.working_size[1] = working_size.y;
		prefilter.push_constant.depth_linearize_mul = depth_linearize_mul;
		prefilter.push_constant.depth_linearize_add = depth_linearize_add;
		prefilter.push_constant.z_near = z_near;
		prefilter.push_constant.z_far = z_far;
		prefilter.push_constant.is_orthogonal = is_orthogonal;
		prefilter.push_constant.source_scale = use_half_size ? 2 : 1;
		// The MIP filter's falloff is tuned against a somewhat smaller radius than the horizon search's.
		compute_falloff(effect_radius * 0.75f, p_settings.falloff_range, prefilter.push_constant.mip_falloff_mul, prefilter.push_constant.mip_falloff_add);
		prefilter.push_constant.mip_count = mip_count;

		RID shader = prefilter.shader.version_get_shader(prefilter.shader_version, 0);

		RD::Uniform u_source(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, depth_hw_texture }));

		// The shader always writes through 5 bindings; the levels this buffer doesn't have get the last one it
		// does, which the shader never writes to beyond its mip_count.
		RD::Uniform u_mips[XEGTAO_DEPTH_MIP_LEVELS];
		for (uint32_t m = 0; m < XEGTAO_DEPTH_MIP_LEVELS; m++) {
			RID mip = p_render_buffers->get_texture_slice(RB_SCOPE_XEGTAO, RB_XEGTAO_DEPTH, p_view, MIN(m, mip_count - 1));
			u_mips[m] = RD::Uniform(RD::UNIFORM_TYPE_IMAGE, m, Vector<RID>({ mip }));
		}

		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, prefilter.pipeline.get_rid());
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 0, u_source), 0);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 1, u_mips[0], u_mips[1], u_mips[2], u_mips[3], u_mips[4]), 1);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &prefilter.push_constant, sizeof(PrefilterPushConstant));
		// Each thread handles 2x2 pixels, so each 8x8 group handles 16x16.
		RD::get_singleton()->compute_list_dispatch(compute_list, (working_size.x + 15) / 16, (working_size.y + 15) / 16, 1);
		RD::get_singleton()->compute_list_add_barrier(compute_list);

		RD::get_singleton()->draw_command_end_label(); // Prefilter Depths
	}

	/* PASS 2: horizon search (XeGTAO_MainPass) */
	{
		RD::get_singleton()->draw_command_begin_label("Horizon Search");

		// Maps the working texture's [0, 1] coordinates to view space at depth 1 (or, orthogonal, at any
		// depth). This includes the off-center terms of asymmetric (XR) projections, and, at half resolution,
		// the remapping from working pixel p to the full resolution pixel 2p it stands for.
		float ndc_to_view_mul[2];
		float ndc_to_view_add[2];
		if (is_orthogonal) {
			ndc_to_view_mul[0] = 2.0f / p_projection.columns[0][0];
			ndc_to_view_mul[1] = -2.0f / p_projection.columns[1][1];
			ndc_to_view_add[0] = (-1.0f - p_projection.columns[3][0]) / p_projection.columns[0][0];
			ndc_to_view_add[1] = (1.0f - p_projection.columns[3][1]) / p_projection.columns[1][1];
		} else {
			ndc_to_view_mul[0] = 2.0f / p_projection.columns[0][0];
			ndc_to_view_mul[1] = -2.0f / p_projection.columns[1][1];
			ndc_to_view_add[0] = (p_projection.columns[2][0] - 1.0f) / p_projection.columns[0][0];
			ndc_to_view_add[1] = (p_projection.columns[2][1] + 1.0f) / p_projection.columns[1][1];
		}

		int scale = use_half_size ? 2 : 1;
		Vector2 working_to_screen_mul = Vector2(float(working_size.x * scale) / full_size.x, float(working_size.y * scale) / full_size.y);
		Vector2 working_to_screen_add = Vector2((0.5f - 0.5f * scale) / full_size.x, (0.5f - 0.5f * scale) / full_size.y);

		memset(&main_pass.push_constant, 0, sizeof(MainPushConstant));
		main_pass.push_constant.viewport_size[0] = working_size.x;
		main_pass.push_constant.viewport_size[1] = working_size.y;
		main_pass.push_constant.viewport_pixel_size[0] = 1.0f / working_size.x;
		main_pass.push_constant.viewport_pixel_size[1] = 1.0f / working_size.y;
		for (int i = 0; i < 2; i++) {
			main_pass.push_constant.ndc_to_view_mul[i] = ndc_to_view_mul[i] * working_to_screen_mul[i];
			main_pass.push_constant.ndc_to_view_add[i] = ndc_to_view_mul[i] * working_to_screen_add[i] + ndc_to_view_add[i];
			main_pass.push_constant.ndc_to_view_mul_x_pixel_size[i] = main_pass.push_constant.ndc_to_view_mul[i] * main_pass.push_constant.viewport_pixel_size[i];
		}
		main_pass.push_constant.effect_radius = effect_radius;
		main_pass.push_constant.sample_distribution_power = p_settings.sample_distribution_power;
		compute_falloff(effect_radius, p_settings.falloff_range, main_pass.push_constant.falloff_mul, main_pass.push_constant.falloff_add);
		main_pass.push_constant.thin_occluder_compensation = p_settings.thin_occluder_compensation;
		main_pass.push_constant.final_value_power = p_settings.power;
		main_pass.push_constant.depth_mip_sampling_offset = XEGTAO_DEPTH_MIP_SAMPLING_OFFSET;
		main_pass.push_constant.slice_count = XEGTAO_SLICE_COUNT[quality];
		main_pass.push_constant.steps_per_slice = XEGTAO_STEPS_PER_SLICE[quality];
		main_pass.push_constant.noise_index = p_settings.temporal_noise ? (p_xegtao_buffers.frame_index[p_view] % 64) : 0;
		main_pass.push_constant.normal_buffer_size[0] = full_size.x;
		main_pass.push_constant.normal_buffer_size[1] = full_size.y;
		main_pass.push_constant.normal_scale = scale;
		main_pass.push_constant.is_orthogonal = is_orthogonal;

		MainMode mode = use_bent_normals ? MAIN_MODE_AO_BENT_NORMALS : MAIN_MODE_AO;
		RID shader = main_pass.shader.version_get_shader(main_pass.shader_version, mode);

		RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, depth_texture }));
		RD::Uniform u_normal(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ p_normal_buffer }));
		RD::Uniform u_term(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ term_textures[0] }));
		RD::Uniform u_edges(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ edges_texture }));

		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, main_pass.pipelines[mode].get_rid());
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 0, u_depth, u_normal), 0);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 1, u_term, u_edges), 1);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &main_pass.push_constant, sizeof(MainPushConstant));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, working_size.x, working_size.y, 1);
		RD::get_singleton()->compute_list_add_barrier(compute_list);

		RD::get_singleton()->draw_command_end_label(); // Horizon Search
	}

	/* PASS 3: spatial denoise (XeGTAO_Denoise), one pass per denoise level */
	uint32_t final_term = 0;
	{
		RD::get_singleton()->draw_command_begin_label("Denoise");

		DenoiseMode mode = use_bent_normals ? DENOISE_MODE_AO_BENT_NORMALS : DENOISE_MODE_AO;
		RID shader = denoise.shader.version_get_shader(denoise.shader_version, mode);
		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, denoise.pipelines[mode].get_rid());

		// Even with denoising disabled one pass still runs, as the one that brings the term back to its [0, 1]
		// range; its blur weight is then so high that the neighbors make no difference.
		int pass_count = MAX(denoise_passes, 1);
		for (int i = 0; i < pass_count; i++) {
			uint32_t source = i % 2;
			uint32_t dest = 1 - source;

			memset(&denoise.push_constant, 0, sizeof(DenoisePushConstant));
			denoise.push_constant.viewport_size[0] = working_size.x;
			denoise.push_constant.viewport_size[1] = working_size.y;
			denoise.push_constant.viewport_pixel_size[0] = 1.0f / working_size.x;
			denoise.push_constant.viewport_pixel_size[1] = 1.0f / working_size.y;
			denoise.push_constant.blur_beta = denoise_passes == 0 ? 1e4f : 1.2f;
			denoise.push_constant.final_apply = i == pass_count - 1;

			RD::Uniform u_source_term(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, term_textures[source] }));
			RD::Uniform u_edges(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, edges_texture }));
			RD::Uniform u_dest_term(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ term_textures[dest] }));

			RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 0, u_source_term, u_edges), 0);
			RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 1, u_dest_term), 1);
			RD::get_singleton()->compute_list_set_push_constant(compute_list, &denoise.push_constant, sizeof(DenoisePushConstant));
			// Each thread filters 2 horizontally adjacent pixels.
			RD::get_singleton()->compute_list_dispatch_threads(compute_list, (working_size.x + 1) / 2, working_size.y, 1);
			RD::get_singleton()->compute_list_add_barrier(compute_list);

			final_term = dest;
		}

		RD::get_singleton()->draw_command_end_label(); // Denoise
	}

	/* PASS 4: resolve to full resolution, apply intensity and distance fade out */
	{
		if (use_half_size) {
			RD::get_singleton()->draw_command_begin_label("Upscale");
		} else {
			RD::get_singleton()->draw_command_begin_label("Apply");
		}

		memset(&apply.push_constant, 0, sizeof(ApplyPushConstant));
		apply.push_constant.full_size[0] = full_size.x;
		apply.push_constant.full_size[1] = full_size.y;
		apply.push_constant.working_size[0] = working_size.x;
		apply.push_constant.working_size[1] = working_size.y;
		apply.push_constant.depth_linearize_mul = depth_linearize_mul;
		apply.push_constant.depth_linearize_add = depth_linearize_add;
		apply.push_constant.z_near = z_near;
		apply.push_constant.z_far = z_far;
		apply.push_constant.is_orthogonal = is_orthogonal;
		apply.push_constant.intensity = p_settings.intensity;
		float fadeout_range = MAX(fadeout_to - fadeout_from, 0.001f);
		apply.push_constant.fade_out_mul = -1.0f / fadeout_range;
		apply.push_constant.fade_out_add = fadeout_from / fadeout_range + 1.0f;

		ApplyMode mode = use_half_size ? (use_bent_normals ? APPLY_MODE_UPSCALE_BENT_NORMALS : APPLY_MODE_UPSCALE) : (use_bent_normals ? APPLY_MODE_COPY_BENT_NORMALS : APPLY_MODE_COPY);
		RID shader = apply.shader.version_get_shader(apply.shader_version, mode);

		thread_local LocalVector<RD::Uniform> source_uniforms;
		source_uniforms.clear();
		source_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, term_textures[final_term] })));
		source_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, depth_hw_texture })));
		if (use_half_size) {
			source_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest_sampler, depth_mip0 })));
		}
		if (use_bent_normals) {
			source_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 3, Vector<RID>({ p_normal_buffer })));
		}

		thread_local LocalVector<RD::Uniform> dest_uniforms;
		dest_uniforms.clear();
		dest_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ final_texture })));
		if (use_bent_normals) {
			dest_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ bent_normal_texture })));
		}

		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, apply.pipelines[mode].get_rid());
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache_vec(shader, 0, source_uniforms), 0);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache_vec(shader, 1, dest_uniforms), 1);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &apply.push_constant, sizeof(ApplyPushConstant));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, full_size.x, full_size.y, 1);

		RD::get_singleton()->draw_command_end_label(); // Upscale / Apply
	}

	RD::get_singleton()->compute_list_end();
	RD::get_singleton()->draw_command_end_label(); // Process XeGTAO

	p_xegtao_buffers.frame_index[p_view]++;
}
