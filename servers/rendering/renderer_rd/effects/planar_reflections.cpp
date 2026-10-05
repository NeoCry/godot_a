/**************************************************************************/
/*  planar_reflections.cpp                                                */
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

#include "planar_reflections.h"

#include "core/io/image.h"
#include "servers/rendering/renderer_rd/cluster_builder_rd.h"
#include "servers/rendering/renderer_rd/effects/copy_effects.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"

using namespace RendererRD;

void PlanarReflections::_free_textures() {
	for (const RID &framebuffer : framebuffers) {
		if (framebuffer.is_valid() && RD::get_singleton()->framebuffer_is_valid(framebuffer)) {
			RD::get_singleton()->free_rid(framebuffer);
		}
	}
	for (const RID &framebuffer : depth_framebuffers) {
		if (framebuffer.is_valid() && RD::get_singleton()->framebuffer_is_valid(framebuffer)) {
			RD::get_singleton()->free_rid(framebuffer);
		}
	}
	framebuffers.clear();
	depth_framebuffers.clear();
	// Freeing a texture frees the views shared from it, and with them the framebuffers made of
	// them; the ones freed above went first so nothing is freed twice.
	for (LocalVector<RID> &mips : color_mips) {
		for (const RID &mip : mips) {
			if (mip.is_valid() && RD::get_singleton()->texture_is_valid(mip)) {
				RD::get_singleton()->free_rid(mip);
			}
		}
	}
	color_mips.clear();
	for (const RID &slice : depth_slices) {
		if (slice.is_valid() && RD::get_singleton()->texture_is_valid(slice)) {
			RD::get_singleton()->free_rid(slice);
		}
	}
	depth_slices.clear();
	if (color.is_valid()) {
		RD::get_singleton()->free_rid(color);
		color = RID();
	}
	if (depth.is_valid()) {
		RD::get_singleton()->free_rid(depth);
		depth = RID();
	}
	layer_capacity = 0;
}

void PlanarReflections::free_data() {
	_free_textures();
	if (cluster_builder != nullptr) {
		memdelete(cluster_builder);
		cluster_builder = nullptr;
	}
	if (uniform_buffer.is_valid()) {
		RD::get_singleton()->free_rid(uniform_buffer);
		uniform_buffer = RID();
	}
	pass_buffers.unref();
	size = Size2i();
	count = 0;
}

PlanarReflections::~PlanarReflections() {
	free_data();
}

Ref<RenderSceneBuffersRD> PlanarReflections::begin(uint32_t p_count, const Size2i &p_size) {
	count = MIN(p_count, MAX_LAYERS);
	const Size2i new_size = p_size.maxi(8);
	const bool resized = new_size != size;
	size = new_size;

	if (resized || layer_capacity < count || color.is_null()) {
		_free_textures();
		// Never fewer layers than were needed before: a plane drifting in and out of sight would
		// otherwise reallocate everything as it does.
		layer_capacity = MAX(count, MAX(layer_capacity, 1u));

		CopyEffects *copy_effects = CopyEffects::get_singleton();
		// The same formats as reflection probes, so the same pipelines draw both. The blur needs to
		// write to the mips as storage images unless it is done by rasterization.
		const bool probe_storage = !copy_effects->get_raster_effects().has_flag(CopyEffects::RASTER_EFFECT_OCTMAP);
		use_storage = !copy_effects->get_raster_effects().has_flag(CopyEffects::RASTER_EFFECT_GAUSSIAN_BLUR);
		mipmaps = MIN(MAX_MIPMAPS, (uint32_t)Image::get_image_required_mipmaps(size.x, size.y, Image::FORMAT_RGBAH) + 1);

		RD::TextureFormat tf;
		tf.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
		tf.array_layers = layer_capacity;
		tf.width = size.x;
		tf.height = size.y;
		tf.mipmaps = mipmaps;
		tf.format = LightStorage::get_reflection_probe_color_format();
		tf.usage_bits = LightStorage::get_reflection_probe_color_usage_bits(probe_storage) | (use_storage ? RD::TEXTURE_USAGE_STORAGE_BIT : 0);
		color = RD::get_singleton()->texture_create(tf, RD::TextureView());
		RD::get_singleton()->set_resource_name(color, "Planar reflections");

		tf.mipmaps = 1;
		tf.format = LightStorage::get_reflection_probe_depth_format();
		tf.usage_bits = LightStorage::get_reflection_probe_depth_usage_bits();
		depth = RD::get_singleton()->texture_create(tf, RD::TextureView());
		RD::get_singleton()->set_resource_name(depth, "Planar reflections depth");

		color_mips.resize(layer_capacity);
		depth_slices.resize(layer_capacity);
		framebuffers.resize(layer_capacity);
		depth_framebuffers.resize(layer_capacity);
		for (uint32_t layer = 0; layer < layer_capacity; layer++) {
			color_mips[layer].resize(mipmaps);
			for (uint32_t mip = 0; mip < mipmaps; mip++) {
				color_mips[layer][mip] = RD::get_singleton()->texture_create_shared_from_slice(RD::TextureView(), color, layer, mip);
			}
			depth_slices[layer] = RD::get_singleton()->texture_create_shared_from_slice(RD::TextureView(), depth, layer, 0);
			framebuffers[layer] = RendererSceneRenderRD::get_singleton()->reflection_probe_create_framebuffer(color_mips[layer][0], depth_slices[layer]);
			Vector<RID> depth_attachment;
			depth_attachment.push_back(depth_slices[layer]);
			depth_framebuffers[layer] = RD::get_singleton()->framebuffer_create(depth_attachment);
		}
	}

	if (resized || pass_buffers.is_null()) {
		if (pass_buffers.is_null()) {
			// Not RendererSceneRenderRD::render_buffers_create(), which would give them the forward
			// renderers' data for a full view; like the reflection atlas', they get none, which is
			// what takes them down the reduced path. They do keep the view's storage and format
			// choices, and with them its luminance multiplier.
			pass_buffers.instantiate();
			pass_buffers->set_can_be_storage(RendererSceneRenderRD::get_singleton()->_render_buffers_can_be_storage());
			pass_buffers->set_preferred_data_format(RendererSceneRenderRD::get_singleton()->_render_buffers_get_preferred_color_format());
			pass_buffers->set_max_cluster_elements(LightStorage::get_singleton()->get_max_cluster_elements());
		}
		pass_buffers->configure_for_reflections(size);
		if (cluster_builder != nullptr) {
			memdelete(cluster_builder);
			cluster_builder = nullptr;
		}
	}
	return pass_buffers;
}

void PlanarReflections::set_layer(uint32_t p_layer, const RendererSceneRender::PlanarReflectionLayer &p_data) {
	ERR_FAIL_UNSIGNED_INDEX(p_layer, MAX_LAYERS);
	layers[p_layer] = p_data;
}

const RendererSceneRender::PlanarReflectionLayer &PlanarReflections::get_layer(uint32_t p_layer) const {
	static const RendererSceneRender::PlanarReflectionLayer none;
	ERR_FAIL_UNSIGNED_INDEX_V(p_layer, MAX_LAYERS, none);
	return layers[p_layer];
}

RID PlanarReflections::get_framebuffer(uint32_t p_layer) const {
	ERR_FAIL_UNSIGNED_INDEX_V(p_layer, framebuffers.size(), RID());
	return framebuffers[p_layer];
}

RID PlanarReflections::get_depth_framebuffer(uint32_t p_layer) const {
	ERR_FAIL_UNSIGNED_INDEX_V(p_layer, depth_framebuffers.size(), RID());
	return depth_framebuffers[p_layer];
}

ClusterBuilderRD *PlanarReflections::get_cluster_builder(ClusterBuilderSharedDataRD *p_shared) {
	if (cluster_builder == nullptr) {
		cluster_builder = memnew(ClusterBuilderRD);
		cluster_builder->set_shared(p_shared);
		cluster_builder->setup(size, LightStorage::get_singleton()->get_max_cluster_elements(), RID(), RID(), RID());
	}
	return cluster_builder;
}

void PlanarReflections::finish_layer(uint32_t p_layer) {
	ERR_FAIL_UNSIGNED_INDEX(p_layer, color_mips.size());
	CopyEffects *copy_effects = CopyEffects::get_singleton();
	ERR_FAIL_NULL(copy_effects);

	RD::get_singleton()->draw_command_begin_label("Blur Planar Reflection");
	for (uint32_t mip = 1; mip < mipmaps; mip++) {
		const Size2i mip_size = Size2i(size.x >> mip, size.y >> mip).maxi(1);
		const RID source = color_mips[p_layer][mip - 1];
		const RID dest = color_mips[p_layer][mip];
		if (use_storage) {
			copy_effects->gaussian_blur(source, dest, Rect2i(Point2i(), mip_size), mip_size);
		} else {
			copy_effects->gaussian_blur_raster(source, dest, Rect2i(Point2i(), mip_size), mip_size);
		}
	}
	RD::get_singleton()->draw_command_end_label();
}

RID PlanarReflections::update_uniform_buffer(const Transform3D &p_camera_transform) {
	if (uniform_buffer.is_null()) {
		uniform_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(UBO));
	}

	UBO ubo;
	memset(&ubo, 0, sizeof(UBO));
	ubo.count = count;

	const Transform3D world_to_view = p_camera_transform.affine_inverse();
	// As the view's own projection is made out for the shaders (see RenderSceneDataRD), so clip
	// space lines up with the layers' texels the way it does with the screen's.
	Projection correction;
	correction.set_depth_correction(true);

	for (uint32_t i = 0; i < count; i++) {
		const RendererSceneRender::PlanarReflectionLayer &layer = layers[i];
		LayerUBO &out = ubo.layers[i];

		MaterialStorage::store_camera(Projection(layer.receiver_transform * p_camera_transform), out.view_to_receiver);
		const Projection view_to_reflection = correction * layer.camera_projection * Projection(layer.camera_transform.affine_inverse() * p_camera_transform);
		MaterialStorage::store_camera(view_to_reflection, out.view_to_reflection);
		MaterialStorage::store_camera(view_to_reflection.inverse(), out.reflection_to_view);

		const Vector3 normal = world_to_view.basis.xform(layer.plane.normal).normalized();
		const Vector3 point = world_to_view.xform(layer.plane.get_center());
		out.plane[0] = normal.x;
		out.plane[1] = normal.y;
		out.plane[2] = normal.z;
		out.plane[3] = -normal.dot(point);

		const Vector3 camera = world_to_view.xform(layer.camera_transform.origin);
		out.camera[0] = camera.x;
		out.camera[1] = camera.y;
		out.camera[2] = camera.z;
		out.camera[3] = layer.camera_projection.columns[1][1] * 0.5f * size.y;

		out.intensity = layer.intensity;
		out.distortion = layer.distortion;
		out.normal_fade = layer.normal_fade;
		out.edge_fade = layer.edge_fade;
		out.reflection_mask = layer.reflection_mask;
		out.max_lod = float(mipmaps - 1);
	}

	RD::get_singleton()->buffer_update(uniform_buffer, 0, sizeof(UBO), &ubo);
	return uniform_buffer;
}

RID PlanarReflections::get_empty_uniform_buffer() {
	if (empty_uniform_buffer.is_null()) {
		UBO ubo;
		memset(&ubo, 0, sizeof(UBO));
		Vector<uint8_t> data;
		data.resize(sizeof(UBO));
		memcpy(data.ptrw(), &ubo, sizeof(UBO));
		empty_uniform_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(UBO), data);
	}
	return empty_uniform_buffer;
}

void PlanarReflections::free_empty_uniform_buffer() {
	if (empty_uniform_buffer.is_valid()) {
		RD::get_singleton()->free_rid(empty_uniform_buffer);
		empty_uniform_buffer = RID();
	}
}

void PlanarReflections::append_uniforms(LocalVector<RD::Uniform> &r_uniforms, uint32_t p_first_binding, const Ref<RenderSceneBuffersRD> &p_render_buffers, bool p_show) {
	Ref<PlanarReflections> planar;
	if (p_show && p_render_buffers.is_valid() && p_render_buffers->has_custom_data(RB_SCOPE_PLANAR_REFLECTIONS)) {
		planar = p_render_buffers->get_custom_data(RB_SCOPE_PLANAR_REFLECTIONS);
		if (planar->get_count() == 0 || planar->get_uniform_buffer().is_null() || planar->get_color_texture().is_null()) {
			planar.unref();
		}
	}
	TextureStorage *texture_storage = TextureStorage::get_singleton();

	RD::Uniform buffer;
	buffer.binding = p_first_binding;
	buffer.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
	buffer.append_id(planar.is_valid() ? planar->get_uniform_buffer() : get_empty_uniform_buffer());
	r_uniforms.push_back(buffer);

	RD::Uniform color_texture;
	color_texture.binding = p_first_binding + 1;
	color_texture.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
	color_texture.append_id(planar.is_valid() ? planar->get_color_texture() : texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK));
	r_uniforms.push_back(color_texture);

	RD::Uniform depth_texture;
	depth_texture.binding = p_first_binding + 2;
	depth_texture.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
	depth_texture.append_id(planar.is_valid() ? planar->get_depth_texture() : texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_DEPTH));
	r_uniforms.push_back(depth_texture);
}
