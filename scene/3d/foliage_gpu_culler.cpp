/**************************************************************************/
/*  foliage_gpu_culler.cpp                                                */
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

#include "foliage_gpu_culler.h"

#include "core/config/engine.h"
#include "core/object/callable_mp.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "servers/rendering/rendering_server.h"

#ifdef RD_ENABLED
#include "servers/rendering/renderer_scene_occlusion_cull.h"
#include "servers/rendering/rendering_device.h"
#endif

// Number of instances handled per compute workgroup.
#define FOLIAGE_CULL_GROUP_SIZE 64

// Floats per instance in a 3D MultiMesh buffer: three rows of four.
#define FOLIAGE_TRANSFORM_FLOATS 12

// Floats handed to a cull dispatch: six frustum planes, the camera origin, and
// the node's own global transform (three rows of four), which the occlusion
// test needs to get instances from local space into the depth pyramid's view
// space.
#define FOLIAGE_FRAME_PARAM_FLOATS 39

// Depth pyramid levels the occlusion test can address. HZBuffer halves down to
// a single texel, so this covers buffers far larger than occlusion culling
// ever allocates.
#define FOLIAGE_OCCLUSION_MAX_MIPS 16

#ifdef RD_ENABLED

namespace {

// Tests every instance against the frustum and against one LOD level's distance
// band, then compacts the survivors into that level's MultiMesh buffer.
// Dispatched once per LOD level.
//
// The plane convention matches Plane::is_point_over(): normals point outwards,
// so a sphere is outside when dot(normal, center) - d > radius.
const char *FOLIAGE_CULL_SHADER_GLSL = R"(#version 450

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict readonly buffer SourceTransforms {
	float data[];
} src;

layout(set = 0, binding = 1, std430) restrict buffer Counter {
	uint data[];
} counter;

layout(set = 0, binding = 2, std430) restrict writeonly buffer DestTransforms {
	float data[];
} dst;

// The depth pyramid the renderer filled for this viewport, and everything
// needed to look an instance up in it. It is the previous frame's pyramid: the
// compute pass is queued from the main thread and runs before the viewports are
// drawn, so an instance that has just come out from behind a hill stays culled
// for one more frame. mips[i] holds that level's first texel index, width and
// height; params is (z_near, mip count, orthogonal, enabled).
layout(set = 1, binding = 0, std140) uniform OcclusionParams {
	mat4 view_matrix;
	mat4 projection;
	uvec4 mips[16];
	vec4 params;
} occ;

layout(set = 1, binding = 1, std430) restrict readonly buffer OcclusionDepth {
	float data[];
} hzb;

layout(push_constant, std430) uniform Params {
	vec4 frustum_planes[6];
	vec3 camera_position;
	uint instance_count;
	float range_begin;
	float range_end;
	float instance_radius;
	uint capacity;
} params;

// The same test RendererSceneOcclusionCull::HZBuffer runs per instance on the
// CPU, for one bounding sphere: project it, pick the pyramid level whose texels
// are about as big as its screen footprint, and cull it only if every texel it
// covers holds something closer to the camera than the sphere's nearest point.
//
// Only that one level is sampled, where the CPU walks down to finer ones when
// the coarse answer is inconclusive. Coarser levels hold the farthest depth
// around, so an occluded verdict there implies one at every finer level: this
// culls a little less than the CPU does, never more.
bool is_occluded(vec3 origin, float radius) {
	if (occ.params.w < 0.5) {
		return false; // No pyramid this frame (occlusion culling is off).
	}

	uint mip_count = uint(occ.params.y);
	if (mip_count == 0u) {
		return false;
	}

	vec3 view_pos = (occ.view_matrix * vec4(origin, 1.0)).xyz;
	float z_near = occ.params.x;

	// The matrix carries the node's own scale (the camera's half of it is
	// rigid), so the radius has to come along into view space with it.
	float view_scale = max(length(occ.view_matrix[0].xyz), max(length(occ.view_matrix[1].xyz), length(occ.view_matrix[2].xyz)));
	float view_radius = radius * view_scale;

	// Anything reaching the near plane is kept: its projection says nothing
	// useful, and at that distance it is not worth the risk of being wrong.
	if (-view_pos.z - view_radius <= z_near) {
		return false;
	}

	// Distance to the sphere's nearest point, in whichever metric the buffer
	// holds: distance from the camera, or view depth for an orthogonal one.
	float near_depth = (occ.params.z > 0.5) ? (-view_pos.z - view_radius) : (length(view_pos) - view_radius);
	if (near_depth <= 0.0) {
		return false;
	}

	vec2 rect_min = vec2(1.0);
	vec2 rect_max = vec2(0.0);
	for (int i = 0; i < 8; i++) {
		vec3 corner = view_pos + vec3(
				(i & 1) != 0 ? view_radius : -view_radius,
				(i & 2) != 0 ? view_radius : -view_radius,
				(i & 4) != 0 ? view_radius : -view_radius);
		vec4 clip = occ.projection * vec4(corner, 1.0);
		if (clip.w <= 0.0) {
			return false;
		}
		vec2 uv = (clip.xy / clip.w) * 0.5 + 0.5;
		rect_min = min(rect_min, uv);
		rect_max = max(rect_max, uv);
	}

	rect_min = clamp(rect_min, vec2(0.0), vec2(1.0));
	rect_max = clamp(rect_max, vec2(0.0), vec2(1.0));

	vec2 extent = (rect_max - rect_min) * vec2(occ.mips[0].yz);
	float level = ceil(log2(max(max(extent.x, extent.y), 1.0)));
	uint mip = uint(clamp(level, 0.0, float(mip_count - 1u)));

	uint offset = occ.mips[mip].x;
	int w = int(occ.mips[mip].y);
	int h = int(occ.mips[mip].z);

	// One texel of margin, matching the CPU test.
	int min_x = clamp(int(rect_min.x * float(w)) - 1, 0, w - 1);
	int max_x = clamp(int(rect_max.x * float(w)) + 1, 0, w - 1);
	int min_y = clamp(int(rect_min.y * float(h)) - 1, 0, h - 1);
	int max_y = clamp(int(rect_max.y * float(h)) + 1, 0, h - 1);

	// The chosen level puts the rectangle inside a couple of texels; anything
	// wider means the level picked was too fine, and is left visible rather
	// than turned into an unbounded loop.
	if ((max_x - min_x) > 3 || (max_y - min_y) > 3) {
		return false;
	}

	for (int y = min_y; y <= max_y; y++) {
		for (int x = min_x; x <= max_x; x++) {
			if (hzb.data[offset + uint(y * w + x)] > near_depth) {
				return false; // Something in that texel is farther away: visible.
			}
		}
	}

	return true;
}

void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= params.instance_count) {
		return;
	}

	uint base = index * 12u;
	vec4 row0 = vec4(src.data[base + 0u], src.data[base + 1u], src.data[base + 2u], src.data[base + 3u]);
	vec4 row1 = vec4(src.data[base + 4u], src.data[base + 5u], src.data[base + 6u], src.data[base + 7u]);
	vec4 row2 = vec4(src.data[base + 8u], src.data[base + 9u], src.data[base + 10u], src.data[base + 11u]);

	vec3 origin = vec3(row0.w, row1.w, row2.w);

	// The basis is stored row-major, so its columns are the transformed axes:
	// the longest one is the instance's largest axis scale, which keeps the
	// bounding sphere valid for non-uniformly scaled instances too.
	vec3 col0 = vec3(row0.x, row1.x, row2.x);
	vec3 col1 = vec3(row0.y, row1.y, row2.y);
	vec3 col2 = vec3(row0.z, row1.z, row2.z);
	float scale = max(length(col0), max(length(col1), length(col2)));
	float radius = params.instance_radius * scale;

	// Distance band. Tested against the instance origin rather than its sphere,
	// so that contiguous bands hand an instance to exactly one LOD level.
	float dist = distance(params.camera_position, origin);
	if (dist < params.range_begin) {
		return;
	}
	if (params.range_end > 0.0 && dist >= params.range_end) {
		return;
	}

	for (int i = 0; i < 6; i++) {
		if (dot(params.frustum_planes[i].xyz, origin) - params.frustum_planes[i].w > radius) {
			return;
		}
	}

	// Last, since it is the only test that reads memory.
	if (is_occluded(origin, radius)) {
		return;
	}

	uint out_index = atomicAdd(counter.data[0], 1u);
	if (out_index >= params.capacity) {
		return;
	}

	uint out_base = out_index * 12u;
	dst.data[out_base + 0u] = row0.x;
	dst.data[out_base + 1u] = row0.y;
	dst.data[out_base + 2u] = row0.z;
	dst.data[out_base + 3u] = row0.w;
	dst.data[out_base + 4u] = row1.x;
	dst.data[out_base + 5u] = row1.y;
	dst.data[out_base + 6u] = row1.z;
	dst.data[out_base + 7u] = row1.w;
	dst.data[out_base + 8u] = row2.x;
	dst.data[out_base + 9u] = row2.y;
	dst.data[out_base + 10u] = row2.z;
	dst.data[out_base + 11u] = row2.w;
}
)";

// Publishes the survivor count into the instance-count field of every surface's
// indirect draw command. The command layout is the standard indexed indirect
// command (index count, instance count, first index, vertex offset, first
// instance), which is what INDIRECT_MULTIMESH_COMMAND_STRIDE describes.
const char *FOLIAGE_APPLY_SHADER_GLSL = R"(#version 450

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict readonly buffer Counter {
	uint data[];
} counter;

layout(set = 0, binding = 1, std430) restrict buffer DrawCommands {
	uint data[];
} commands;

layout(push_constant, std430) uniform Params {
	uint surface_count;
	uint capacity;
	uint pad0;
	uint pad1;
} params;

void main() {
	uint surface = gl_GlobalInvocationID.x;
	if (surface >= params.surface_count) {
		return;
	}
	commands.data[surface * 5u + 1u] = min(counter.data[0], params.capacity);
}
)";

struct CullPushConstant {
	float frustum_planes[6][4];
	float camera_position[3];
	uint32_t instance_count;
	float range_begin;
	float range_end;
	float instance_radius;
	uint32_t capacity;
};

// Mirrors the shader's OcclusionParams uniform block, std140: two mat4s, then
// one uvec4 per pyramid level, then the loose parameters as a vec4.
struct OcclusionUniforms {
	float view_matrix[16];
	float projection[16];
	uint32_t mips[FOLIAGE_OCCLUSION_MAX_MIPS][4];
	float z_near;
	float mip_count;
	float orthogonal;
	float enabled;
};

static_assert(sizeof(OcclusionUniforms) == 64 + 64 + FOLIAGE_OCCLUSION_MAX_MIPS * 16 + 16, "OcclusionUniforms must match the shader's std140 layout.");

struct ApplyPushConstant {
	uint32_t surface_count;
	uint32_t capacity;
	uint32_t pad0;
	uint32_t pad1;
};

static_assert(sizeof(CullPushConstant) <= 128, "Push constants are limited to 128 bytes.");

} // namespace

bool FoliageCullResources::_ensure_shaders() {
	if (cull_pipeline.is_valid() && apply_pipeline.is_valid()) {
		return true;
	}

	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	ERR_FAIL_NULL_V(rd, false);

	struct Program {
		const char *source;
		const char *name;
		RID *shader;
		RID *pipeline;
	};
	const Program programs[2] = {
		{ FOLIAGE_CULL_SHADER_GLSL, "FoliageGPUCull", &cull_shader, &cull_pipeline },
		{ FOLIAGE_APPLY_SHADER_GLSL, "FoliageGPUCullApply", &apply_shader, &apply_pipeline },
	};

	for (const Program &program : programs) {
		if (program.pipeline->is_valid()) {
			continue;
		}

		String error;
		Vector<uint8_t> spirv = rd->shader_compile_spirv_from_source(RD::SHADER_STAGE_COMPUTE, String(program.source), RD::SHADER_LANGUAGE_GLSL, &error);
		ERR_FAIL_COND_V_MSG(spirv.is_empty(), false, vformat("Could not compile the %s compute shader: %s", program.name, error));

		RD::ShaderStageSPIRVData stage;
		stage.shader_stage = RD::SHADER_STAGE_COMPUTE;
		stage.spirv = spirv;

		Vector<RD::ShaderStageSPIRVData> stages;
		stages.push_back(stage);

		*program.shader = rd->shader_create_from_spirv(stages, program.name);
		ERR_FAIL_COND_V(program.shader->is_null(), false);

		*program.pipeline = rd->compute_pipeline_create(*program.shader);
		ERR_FAIL_COND_V(program.pipeline->is_null(), false);
	}

	return true;
}

void FoliageCullResources::_free_levels() {
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	if (rd == nullptr) {
		lod_resources.clear();
		return;
	}
	for (const LODResources &res : lod_resources) {
		if (res.cull_uniform_set.is_valid()) {
			rd->free_rid(res.cull_uniform_set);
		}
		if (res.apply_uniform_set.is_valid()) {
			rd->free_rid(res.apply_uniform_set);
		}
		if (res.counter_buffer.is_valid()) {
			rd->free_rid(res.counter_buffer);
		}
	}
	lod_resources.clear();
}

void FoliageCullResources::_free_occlusion() {
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	if (rd == nullptr) {
		occlusion_uniform_set = RID();
		occlusion_params_buffer = RID();
		occlusion_depth_buffer = RID();
		occlusion_depth_floats = 0;
		return;
	}

	RID *owned[3] = { &occlusion_uniform_set, &occlusion_params_buffer, &occlusion_depth_buffer };
	for (RID *rid : owned) {
		if (rid->is_valid()) {
			rd->free_rid(*rid);
			*rid = RID();
		}
	}
	occlusion_depth_floats = 0;
}

void FoliageCullResources::_free_all() {
	_free_levels();
	_free_occlusion();

	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	if (rd == nullptr) {
		return;
	}

	RID *owned[5] = { &source_buffer, &cull_pipeline, &cull_shader, &apply_pipeline, &apply_shader };
	for (RID *rid : owned) {
		if (rid->is_valid()) {
			rd->free_rid(*rid);
			*rid = RID();
		}
	}
	source_instance_count = 0;
}

void FoliageCullResources::rt_setup(const PackedByteArray &p_transform_data, const Array &p_multimeshes, const PackedFloat32Array &p_level_params, const PackedInt32Array &p_surface_counts) {
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	ERR_FAIL_NULL(rd);

	_free_levels();

	if (source_buffer.is_valid()) {
		rd->free_rid(source_buffer);
		source_buffer = RID();
	}

	source_instance_count = p_transform_data.size() / (FOLIAGE_TRANSFORM_FLOATS * sizeof(float));
	if (source_instance_count == 0 || p_multimeshes.is_empty()) {
		return;
	}

	if (!_ensure_shaders()) {
		return;
	}

	source_buffer = rd->storage_buffer_create(p_transform_data.size(), p_transform_data.span());
	ERR_FAIL_COND(source_buffer.is_null());

	RenderingServer *rs = RenderingServer::get_singleton();

	for (int i = 0; i < p_multimeshes.size(); i++) {
		RID multimesh = p_multimeshes[i];
		RID dest_buffer = rs->multimesh_get_buffer_rd_rid(multimesh);
		RID command_buffer = rs->multimesh_get_command_buffer_rd_rid(multimesh);
		if (dest_buffer.is_null() || command_buffer.is_null()) {
			// The MultiMesh was not allocated with indirect drawing enabled, or
			// has no mesh yet; that level simply does not get culled.
			continue;
		}

		LODResources res;
		res.surface_count = MAX(1, p_surface_counts[i]);
		res.range_begin = p_level_params[i * 3 + 0];
		res.range_end = p_level_params[i * 3 + 1];
		res.instance_radius = p_level_params[i * 3 + 2];

		const uint32_t zero = 0;
		res.counter_buffer = rd->storage_buffer_create(sizeof(uint32_t), Span<uint8_t>(reinterpret_cast<const uint8_t *>(&zero), sizeof(uint32_t)));
		ERR_CONTINUE(res.counter_buffer.is_null());

		{
			Vector<RD::Uniform> uniforms;

			RD::Uniform u_src;
			u_src.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u_src.binding = 0;
			u_src.append_id(source_buffer);
			uniforms.push_back(u_src);

			RD::Uniform u_counter;
			u_counter.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u_counter.binding = 1;
			u_counter.append_id(res.counter_buffer);
			uniforms.push_back(u_counter);

			RD::Uniform u_dst;
			u_dst.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u_dst.binding = 2;
			u_dst.append_id(dest_buffer);
			uniforms.push_back(u_dst);

			res.cull_uniform_set = rd->uniform_set_create(uniforms, cull_shader, 0);
		}

		{
			Vector<RD::Uniform> uniforms;

			RD::Uniform u_counter;
			u_counter.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u_counter.binding = 0;
			u_counter.append_id(res.counter_buffer);
			uniforms.push_back(u_counter);

			RD::Uniform u_commands;
			u_commands.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u_commands.binding = 1;
			u_commands.append_id(command_buffer);
			uniforms.push_back(u_commands);

			res.apply_uniform_set = rd->uniform_set_create(uniforms, apply_shader, 0);
		}

		if (res.cull_uniform_set.is_null() || res.apply_uniform_set.is_null()) {
			if (res.cull_uniform_set.is_valid()) {
				rd->free_rid(res.cull_uniform_set);
			}
			if (res.apply_uniform_set.is_valid()) {
				rd->free_rid(res.apply_uniform_set);
			}
			rd->free_rid(res.counter_buffer);
			ERR_PRINT("Could not create the foliage culling uniform sets.");
			continue;
		}

		lod_resources.push_back(res);
	}
}

void FoliageCullResources::_update_occlusion(RID p_viewport, const Transform3D &p_node_transform, bool p_enabled) {
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	ERR_FAIL_NULL(rd);

	// Read on the rendering thread, which is also the thread that fills it, so
	// what comes back is a settled pyramid rather than a half-written one. It
	// is the one from the previous frame: this pass runs before the viewports
	// are drawn again (see the shader's note).
	const RendererSceneOcclusionCull::HZBuffer *hzb = nullptr;
	if (p_enabled && p_viewport.is_valid() && RendererSceneOcclusionCull::get_singleton() != nullptr) {
		hzb = RendererSceneOcclusionCull::get_singleton()->buffer_get_ptr(p_viewport);
	}
	if (hzb != nullptr && (hzb->is_empty() || hzb->get_depth_data_size() == 0)) {
		hzb = nullptr;
	}

	OcclusionUniforms uniforms = {};

	if (hzb != nullptr) {
		// Instance local space to the pyramid camera's view space.
		const Projection view = Projection(hzb->get_camera_transform().affine_inverse() * p_node_transform);
		const Projection &projection = hzb->get_camera_projection();
		for (int column = 0; column < 4; column++) {
			for (int row = 0; row < 4; row++) {
				uniforms.view_matrix[column * 4 + row] = (float)view.columns[column][row];
				uniforms.projection[column * 4 + row] = (float)projection.columns[column][row];
			}
		}

		const uint32_t mip_count = MIN(hzb->get_mip_count(), (uint32_t)FOLIAGE_OCCLUSION_MAX_MIPS);
		uint32_t offset = 0;
		for (uint32_t mip = 0; mip < mip_count; mip++) {
			const Size2i size = hzb->get_mip_size(mip);
			uniforms.mips[mip][0] = offset;
			uniforms.mips[mip][1] = (uint32_t)size.x;
			uniforms.mips[mip][2] = (uint32_t)size.y;
			offset += (uint32_t)(size.x * size.y);
		}

		uniforms.z_near = (float)hzb->get_camera_projection().get_z_near();
		uniforms.mip_count = (float)mip_count;
		uniforms.orthogonal = hzb->is_camera_orthogonal() ? 1.0f : 0.0f;
		uniforms.enabled = 1.0f;
	}

	// The shader always has the set bound, whether or not there is a pyramid
	// behind it, so the buffers are kept even when occlusion is off. One float
	// is enough to stand in for the pyramid then.
	const uint32_t depth_floats = hzb != nullptr ? hzb->get_depth_data_size() : 1;

	if (occlusion_depth_buffer.is_null() || occlusion_depth_floats != depth_floats) {
		if (occlusion_uniform_set.is_valid()) {
			rd->free_rid(occlusion_uniform_set);
			occlusion_uniform_set = RID();
		}
		if (occlusion_depth_buffer.is_valid()) {
			rd->free_rid(occlusion_depth_buffer);
		}
		occlusion_depth_buffer = rd->storage_buffer_create(depth_floats * sizeof(float));
		ERR_FAIL_COND(occlusion_depth_buffer.is_null());
		occlusion_depth_floats = depth_floats;
	}

	if (occlusion_params_buffer.is_null()) {
		occlusion_params_buffer = rd->uniform_buffer_create(sizeof(OcclusionUniforms));
		ERR_FAIL_COND(occlusion_params_buffer.is_null());
	}

	rd->buffer_update(occlusion_params_buffer, 0, sizeof(OcclusionUniforms), &uniforms);
	if (hzb != nullptr) {
		rd->buffer_update(occlusion_depth_buffer, 0, depth_floats * sizeof(float), hzb->get_depth_data());
	}

	if (occlusion_uniform_set.is_null()) {
		Vector<RD::Uniform> set;

		RD::Uniform u_params;
		u_params.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u_params.binding = 0;
		u_params.append_id(occlusion_params_buffer);
		set.push_back(u_params);

		RD::Uniform u_depth;
		u_depth.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u_depth.binding = 1;
		u_depth.append_id(occlusion_depth_buffer);
		set.push_back(u_depth);

		occlusion_uniform_set = rd->uniform_set_create(set, cull_shader, 1);
		ERR_FAIL_COND(occlusion_uniform_set.is_null());
	}
}

void FoliageCullResources::rt_cull(const PackedFloat32Array &p_frame_params, RID p_occlusion_viewport, bool p_occlusion_enabled) {
	if (source_instance_count == 0 || lod_resources.is_empty()) {
		return;
	}

	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	ERR_FAIL_NULL(rd);
	ERR_FAIL_COND(p_frame_params.size() < FOLIAGE_FRAME_PARAM_FLOATS);

	const float *r = p_frame_params.ptr();

	Transform3D node_transform;
	node_transform.basis.set_column(0, Vector3(r[27], r[31], r[35]));
	node_transform.basis.set_column(1, Vector3(r[28], r[32], r[36]));
	node_transform.basis.set_column(2, Vector3(r[29], r[33], r[37]));
	node_transform.origin = Vector3(r[30], r[34], r[38]);

	_update_occlusion(p_occlusion_viewport, node_transform, p_occlusion_enabled);
	if (occlusion_uniform_set.is_null()) {
		return;
	}

	// The counters restart from zero every frame; the previous frame's values
	// have already been consumed by its draw commands.
	for (const LODResources &res : lod_resources) {
		rd->buffer_clear(res.counter_buffer, 0, sizeof(uint32_t));
	}

	RD::ComputeListID compute_list = rd->compute_list_begin();

	rd->compute_list_bind_compute_pipeline(compute_list, cull_pipeline);
	rd->compute_list_bind_uniform_set(compute_list, occlusion_uniform_set, 1);
	for (const LODResources &res : lod_resources) {
		CullPushConstant pc = {};
		memcpy(pc.frustum_planes, r, sizeof(float) * 24);
		pc.camera_position[0] = r[24];
		pc.camera_position[1] = r[25];
		pc.camera_position[2] = r[26];
		pc.instance_count = source_instance_count;
		pc.range_begin = res.range_begin;
		pc.range_end = res.range_end;
		pc.instance_radius = res.instance_radius;
		pc.capacity = source_instance_count;

		rd->compute_list_bind_uniform_set(compute_list, res.cull_uniform_set, 0);
		rd->compute_list_set_push_constant(compute_list, &pc, sizeof(CullPushConstant));
		rd->compute_list_dispatch(compute_list, Math::division_round_up(source_instance_count, (uint32_t)FOLIAGE_CULL_GROUP_SIZE), 1, 1);
	}

	// The counters written above are read back by the pass below.
	rd->compute_list_add_barrier(compute_list);

	rd->compute_list_bind_compute_pipeline(compute_list, apply_pipeline);
	for (const LODResources &res : lod_resources) {
		ApplyPushConstant pc = {};
		pc.surface_count = res.surface_count;
		pc.capacity = source_instance_count;

		rd->compute_list_bind_uniform_set(compute_list, res.apply_uniform_set, 0);
		rd->compute_list_set_push_constant(compute_list, &pc, sizeof(ApplyPushConstant));
		rd->compute_list_dispatch(compute_list, Math::division_round_up(res.surface_count, (uint32_t)FOLIAGE_CULL_GROUP_SIZE), 1, 1);
	}

	rd->compute_list_end();
}

#else // !RD_ENABLED

// Without a RenderingDevice there is nothing to drive: is_supported() returns
// false, so none of this is ever queued in the first place.
bool FoliageCullResources::_ensure_shaders() { return false; }
void FoliageCullResources::_free_levels() {}
void FoliageCullResources::_free_all() {}
void FoliageCullResources::rt_setup(const PackedByteArray &p_transform_data, const Array &p_multimeshes, const PackedFloat32Array &p_level_params, const PackedInt32Array &p_surface_counts) {}
void FoliageCullResources::_free_occlusion() {}
void FoliageCullResources::_update_occlusion(RID p_viewport, const Transform3D &p_node_transform, bool p_enabled) {}
void FoliageCullResources::rt_cull(const PackedFloat32Array &p_frame_params, RID p_occlusion_viewport, bool p_occlusion_enabled) {}

#endif // RD_ENABLED

bool FoliageGPUCuller::is_supported() {
#ifdef RD_ENABLED
	RenderingServer *rs = RenderingServer::get_singleton();
	return rs != nullptr && rs->get_rendering_device() != nullptr;
#else
	return false;
#endif
}

void FoliageGPUCuller::update_instances(const LocalVector<Transform3D> &p_transforms, const LocalVector<LODLevel> &p_levels) {
	if (!is_supported()) {
		return;
	}

	if (resources.is_null()) {
		resources.instantiate();
	}

	// The transforms are packed here, on the main thread, so the rendering
	// thread only ever sees a self-contained copy of them.
	PackedByteArray transform_data;
	transform_data.resize(p_transforms.size() * FOLIAGE_TRANSFORM_FLOATS * sizeof(float));
	{
		float *w = reinterpret_cast<float *>(transform_data.ptrw());
		for (uint32_t i = 0; i < p_transforms.size(); i++) {
			const Transform3D &t = p_transforms[i];
			float *dst = w + i * FOLIAGE_TRANSFORM_FLOATS;
			dst[0] = t.basis.rows[0][0];
			dst[1] = t.basis.rows[0][1];
			dst[2] = t.basis.rows[0][2];
			dst[3] = t.origin.x;
			dst[4] = t.basis.rows[1][0];
			dst[5] = t.basis.rows[1][1];
			dst[6] = t.basis.rows[1][2];
			dst[7] = t.origin.y;
			dst[8] = t.basis.rows[2][0];
			dst[9] = t.basis.rows[2][1];
			dst[10] = t.basis.rows[2][2];
			dst[11] = t.origin.z;
		}
	}

	Array multimeshes;
	PackedFloat32Array level_params;
	PackedInt32Array surface_counts;
	for (uint32_t i = 0; i < p_levels.size() && i < (uint32_t)MAX_LOD_LEVELS; i++) {
		multimeshes.push_back(p_levels[i].multimesh);
		level_params.push_back(p_levels[i].range_begin);
		level_params.push_back(p_levels[i].range_end);
		level_params.push_back(p_levels[i].instance_radius);
		surface_counts.push_back((int32_t)p_levels[i].surface_count);
	}

	RenderingServer::get_singleton()->call_on_render_thread(
			callable_mp(resources.ptr(), &FoliageCullResources::rt_setup).bind(transform_data, multimeshes, level_params, surface_counts));
}

namespace {

Camera3D *find_camera_for_world(Node *p_node, const Ref<World3D> &p_world, const Viewport *p_exclude) {
	Viewport *viewport = Object::cast_to<Viewport>(p_node);
	if (viewport != nullptr && viewport != p_exclude && viewport->find_world_3d() == p_world) {
		Camera3D *camera = viewport->get_camera_3d();
		if (camera != nullptr) {
			return camera;
		}
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		Camera3D *camera = find_camera_for_world(p_node->get_child(i), p_world, p_exclude);
		if (camera != nullptr) {
			return camera;
		}
	}
	return nullptr;
}

} // namespace

Camera3D *FoliageGPUCuller::resolve_culling_camera(const Node3D *p_node, ObjectID &r_cached_camera) {
	ERR_FAIL_NULL_V(p_node, nullptr);
	if (!p_node->is_inside_tree()) {
		return nullptr;
	}

	Viewport *own_viewport = p_node->get_viewport();

	if (!Engine::get_singleton()->is_editor_hint()) {
		return own_viewport != nullptr ? own_viewport->get_camera_3d() : nullptr;
	}

	Camera3D *cached = Object::cast_to<Camera3D>(ObjectDB::get_instance(r_cached_camera));
	if (cached != nullptr && cached->is_inside_tree()) {
		return cached;
	}

	// The node's own viewport is skipped so that a game camera sitting in the
	// edited scene does not win over the editor viewport actually being
	// looked through. Several editor viewports can match in a split layout;
	// the first one found is used.
	Camera3D *found = find_camera_for_world(p_node->get_tree()->get_root(), p_node->get_world_3d(), own_viewport);
	r_cached_camera = found != nullptr ? found->get_instance_id() : ObjectID();
	return found;
}

void FoliageGPUCuller::draw_without_culling() {
	// Six degenerate planes: dot(0, origin) - 0 > radius is never true, so the
	// frustum test keeps everything. The LOD bands still apply, measured from
	// the node's own origin.
	Vector<Plane> planes;
	planes.resize(6);
	cull(planes, Vector3());
}

void FoliageGPUCuller::cull(const Vector<Plane> &p_frustum_planes, const Vector3 &p_camera_position, RID p_occlusion_viewport, const Transform3D &p_node_transform, bool p_occlusion_enabled) {
	if (resources.is_null() || p_frustum_planes.size() < 6) {
		return;
	}

	PackedFloat32Array frame_params;
	frame_params.resize(FOLIAGE_FRAME_PARAM_FLOATS);
	{
		float *w = frame_params.ptrw();
		for (int i = 0; i < 6; i++) {
			const Plane &p = p_frustum_planes[i];
			w[i * 4 + 0] = p.normal.x;
			w[i * 4 + 1] = p.normal.y;
			w[i * 4 + 2] = p.normal.z;
			w[i * 4 + 3] = p.d;
		}
		w[24] = p_camera_position.x;
		w[25] = p_camera_position.y;
		w[26] = p_camera_position.z;

		// Rows of the node's global transform, the same layout the instance
		// transforms use.
		for (int row = 0; row < 3; row++) {
			w[27 + row * 4 + 0] = p_node_transform.basis.rows[row][0];
			w[27 + row * 4 + 1] = p_node_transform.basis.rows[row][1];
			w[27 + row * 4 + 2] = p_node_transform.basis.rows[row][2];
			w[27 + row * 4 + 3] = p_node_transform.origin[row];
		}
	}

	RenderingServer::get_singleton()->call_on_render_thread(
			callable_mp(resources.ptr(), &FoliageCullResources::rt_cull).bind(frame_params, p_occlusion_viewport, p_occlusion_enabled));
}

void FoliageGPUCuller::release() {
	if (resources.is_null()) {
		return;
	}

	// The bound reference keeps the resources alive until the rendering thread
	// has actually freed them, even though this drops the culler's own.
	RenderingServer::get_singleton()->call_on_render_thread(
			callable_mp_static(&FoliageGPUCuller::rt_free).bind(resources));
	resources.unref();
}

void FoliageGPUCuller::rt_free(const Ref<FoliageCullResources> &p_resources) {
	if (p_resources.is_valid()) {
		p_resources->_free_all();
	}
}

FoliageGPUCuller::~FoliageGPUCuller() {
	release();
}
