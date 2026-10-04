#[compute]

#version 450

#VERSION_DEFINES

// Culls the instances of GPU shadow caster MultiMeshes for the shadow passes they are drawn in (see
// RenderForwardClustered::_shadow_cull_dispatch()). Each job is one MultiMesh in one pass: the
// instances that pass are compacted into the job's part of a shared buffer, which the pass draws
// them from, and APPLY then writes how many they are into the instance count of the job's indirect
// draw commands, one per surface.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct Job {
	vec4 planes[6]; // In the MultiMesh's own space; normals point outwards.
	vec3 camera_position; // Same.
	uint plane_count;
	float range_begin;
	float range_end; // 0 for no distance range.
	float instance_radius; // Of an instance at unit scale.
	uint instance_count;
	uint source_offset; // In instances.
	uint stride; // In floats.
	uint output_base; // In instances of this job's stride.
	uint capacity;
	uint command_base;
	uint command_count;
	uint pad0;
	uint pad1;
};

layout(set = 0, binding = 0, std430) restrict readonly buffer Jobs {
	Job data[];
}
jobs;

layout(set = 0, binding = 1, std430) restrict buffer Counters {
	uint data[];
}
counters;

#ifdef MODE_APPLY

layout(set = 0, binding = 2, std430) restrict buffer Commands {
	uint data[];
}
commands;

layout(set = 0, binding = 3, std430) restrict readonly buffer CommandJobs {
	uint data[];
}
command_jobs;

layout(push_constant, std430) uniform Params {
	uint command_count;
	uint pad0;
	uint pad1;
	uint pad2;
}
params;

void main() {
	uint command = gl_GlobalInvocationID.x;
	if (command >= params.command_count) {
		return;
	}
	uint job = command_jobs.data[command];
	// Indexed indirect commands: index count, instance count, first index, vertex offset, first
	// instance. Non-indexed ones read the first four the same way.
	commands.data[command * 5u + 1u] = min(counters.data[job], jobs.data[job].capacity);
}

#else

layout(set = 0, binding = 2, std430) restrict writeonly buffer Output {
	float data[];
}
dst;

layout(set = 1, binding = 0, std430) restrict readonly buffer Source {
	float data[];
}
src;

layout(push_constant, std430) uniform Params {
	uint job;
	uint pad0;
	uint pad1;
	uint pad2;
}
params;

void main() {
	Job job = jobs.data[params.job];
	uint index = gl_GlobalInvocationID.x;
	if (index >= job.instance_count) {
		return;
	}

	uint base = (job.source_offset + index) * job.stride;
	vec4 row0 = vec4(src.data[base + 0u], src.data[base + 1u], src.data[base + 2u], src.data[base + 3u]);
	vec4 row1 = vec4(src.data[base + 4u], src.data[base + 5u], src.data[base + 6u], src.data[base + 7u]);
	vec4 row2 = vec4(src.data[base + 8u], src.data[base + 9u], src.data[base + 10u], src.data[base + 11u]);
	vec3 origin = vec3(row0.w, row1.w, row2.w);

	// The basis is stored row-major, so its columns are the transformed axes: the longest one is the
	// instance's largest scale, which keeps the bounding sphere valid for non-uniform scales too.
	float scale = max(length(vec3(row0.x, row1.x, row2.x)), max(length(vec3(row0.y, row1.y, row2.y)), length(vec3(row0.z, row1.z, row2.z))));
	float radius = job.instance_radius * scale;

	if (job.range_end > 0.0) {
		// Tested against the instance's origin rather than its sphere, so that contiguous ranges
		// hand an instance to exactly one of them.
		float dist = distance(job.camera_position, origin);
		if (dist < job.range_begin || dist >= job.range_end) {
			return;
		}
	}

	for (uint i = 0u; i < job.plane_count; i++) {
		if (dot(job.planes[i].xyz, origin) - job.planes[i].w > radius) {
			return;
		}
	}

	uint out_index = atomicAdd(counters.data[params.job], 1u);
	if (out_index >= job.capacity) {
		return;
	}

	uint out_base = (job.output_base + out_index) * job.stride;
	for (uint i = 0u; i < job.stride; i++) {
		dst.data[out_base + i] = src.data[base + i];
	}
}

#endif
