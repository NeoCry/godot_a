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

#include "core/object/callable_mp.h"
#include "servers/rendering/rendering_server.h"

#ifdef RD_ENABLED
#include "servers/rendering/rendering_device.h"
#endif

// Number of instances handled per compute workgroup.
#define FOLIAGE_CULL_GROUP_SIZE 64

// Floats per instance in a 3D MultiMesh buffer: three rows of four.
#define FOLIAGE_TRANSFORM_FLOATS 12

// Floats handed to a cull dispatch: six frustum planes plus the camera origin.
#define FOLIAGE_FRAME_PARAM_FLOATS 27

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

layout(push_constant, std430) uniform Params {
	vec4 frustum_planes[6];
	vec3 camera_position;
	uint instance_count;
	float range_begin;
	float range_end;
	float instance_radius;
	uint capacity;
} params;

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

void FoliageCullResources::_free_all() {
	_free_levels();

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

void FoliageCullResources::rt_cull(const PackedFloat32Array &p_frame_params) {
	if (source_instance_count == 0 || lod_resources.is_empty()) {
		return;
	}

	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	ERR_FAIL_NULL(rd);
	ERR_FAIL_COND(p_frame_params.size() < FOLIAGE_FRAME_PARAM_FLOATS);

	const float *r = p_frame_params.ptr();

	// The counters restart from zero every frame; the previous frame's values
	// have already been consumed by its draw commands.
	for (const LODResources &res : lod_resources) {
		rd->buffer_clear(res.counter_buffer, 0, sizeof(uint32_t));
	}

	RD::ComputeListID compute_list = rd->compute_list_begin();

	rd->compute_list_bind_compute_pipeline(compute_list, cull_pipeline);
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
void FoliageCullResources::rt_cull(const PackedFloat32Array &p_frame_params) {}

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

void FoliageGPUCuller::cull(const Vector<Plane> &p_frustum_planes, const Vector3 &p_camera_position) {
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
	}

	RenderingServer::get_singleton()->call_on_render_thread(
			callable_mp(resources.ptr(), &FoliageCullResources::rt_cull).bind(frame_params));
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
