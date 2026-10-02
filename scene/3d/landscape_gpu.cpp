/**************************************************************************/
/*  landscape_gpu.cpp                                                     */
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

#include "landscape_gpu.h"

#include "core/object/callable_mp.h"
#include "servers/rendering/rendering_server.h"

#ifdef RD_ENABLED
#include "servers/rendering/rendering_device.h"
#endif

// Threads per workgroup, in every pass.
#define LANDSCAPE_GROUP_SIZE 64

#ifdef RD_ENABLED

namespace {

RD::DataFormat get_rd_format(Image::Format p_format) {
	switch (p_format) {
		case Image::FORMAT_R8:
			return RD::DATA_FORMAT_R8_UNORM;
		case Image::FORMAT_RF:
			return RD::DATA_FORMAT_R32_SFLOAT;
		case Image::FORMAT_RGH:
			return RD::DATA_FORMAT_R16G16_SFLOAT;
		case Image::FORMAT_RGBA8:
			return RD::DATA_FORMAT_R8G8B8A8_UNORM;
		default:
			return RD::DATA_FORMAT_MAX;
	}
}

// Selects the patches to draw. Compiled three times, once per pass:
//
// MODE_TRAVERSE runs once per level, top down. Every node of the level's list
// either appends its four children to the list of the level below (the two
// lists alternate between the halves of one buffer), or appends itself to the
// selected patches. Run with pc.reset set, a single thread of it starts a new
// attempt at the selection instead (see rt_dispatch()).
//
// MODE_EMIT runs once over the selected patches: it works out how much coarser
// each neighbor is, culls, and writes the patch into the MultiMesh instance
// buffers.
//
// MODE_APPLY copies how many patches were written into the MultiMeshes'
// indirect draw commands.
//
// should_subdivide() mirrors LandscapeQuadtree::should_subdivide() exactly.
// MODE_EMIT evaluates it again for the neighbors' ancestors, and any
// disagreement with what MODE_TRAVERSE decided would open a crack between two
// patches, so everything it computes is declared precise: without it the
// compiler is free to fuse or reorder the arithmetic differently in each place
// it inlines the function, and a node right at the threshold could come out
// either way.
const char *LANDSCAPE_QUADTREE_SHADER_GLSL = R"(
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

#define PATCH_QUADS 16
#define LEVEL_BIAS 4
#define MAX_EDGE_DELTA 15
#define ORIGIN_SPLIT 1024u
#define INSTANCE_FLOATS 20u
#define INVALID_NODE 0xffffffffu
#define NODE_FLAG_ALL_HOLE 2u
#define FLAG_FRUSTUM_CULLING 1
#define FLAG_SHADOWS 2

#if defined(MODE_TRAVERSE) || defined(MODE_EMIT)

// Mirrors LandscapeGPUParams.
layout(set = 0, binding = 0, std140) uniform Params {
	// Terrain local space; a box is outside when dot(normal, corner) > d for
	// its corner furthest inside.
	vec4 frustum[6];
	// xyz: camera position (terrain local). w: 1 for orthogonal projections.
	vec4 camera;
	// x: pixel scale, y: pixel error, z: max quad pixels, w: micro quad pixels.
	vec4 lod;
	// x: micro distance, y: displacement bound, z: shadow distance, w: vertex spacing.
	vec4 micro;
	// x: quads along the terrain, y: top level, z: lowest level, w: flags.
	ivec4 grid;
	// First node of each level 0 and up.
	uvec4 level_offsets[4];
} params;

layout(set = 0, binding = 1, std430) restrict readonly buffer Nodes {
	vec4 data[];
} nodes;

layout(set = 0, binding = 3, std430) restrict buffer FinalNodes {
	uvec2 data[];
} final_nodes;

#endif

#ifdef MODE_TRAVERSE
layout(set = 0, binding = 2, std430) restrict buffer Lists {
	uint data[];
} lists;
#endif

layout(set = 0, binding = 4, std430) restrict buffer Counters {
	uint level_count[16];
	uint final_count;
	uint visible_count;
	uint shadow_count;
	// Set when the selection ran out of room in a list: it is then missing
	// patches, or has some coarser than the LOD asked for, and neither is
	// stitched to its neighbors.
	uint overflow;
	// The attempt the lists hold.
	uint attempt;
	// Set once an attempt fitted, which leaves its selection in the lists.
	uint done;
	uint pad[2];
} counters;

#ifdef MODE_EMIT
layout(set = 1, binding = 0, std430) restrict writeonly buffer MainInstances {
	float data[];
} main_instances;

layout(set = 1, binding = 1, std430) restrict writeonly buffer ShadowInstances {
	float data[];
} shadow_instances;
#endif

#ifdef MODE_APPLY
layout(set = 1, binding = 0, std430) restrict buffer MainCommands {
	uint data[];
} main_commands;

layout(set = 1, binding = 1, std430) restrict buffer ShadowCommands {
	uint data[];
} shadow_commands;
#endif

layout(push_constant, std430) uniform PushConstant {
	int level;
	uint level_index;
	uint capacity;
	uint attempt;
	uint reset;
	uint pad[3];
} pc;

#if defined(MODE_TRAVERSE) || defined(MODE_EMIT)

int node_samples(int p_level) {
	return p_level >= 0 ? (PATCH_QUADS << p_level) : (PATCH_QUADS >> (-p_level));
}

bool node_inside(int p_level, ivec2 p_node) {
	if (p_node.x < 0 || p_node.y < 0) {
		return false;
	}
	int size = node_samples(p_level);
	return p_node.x * size < params.grid.x && p_node.y * size < params.grid.x;
}

uint level_offset(int p_level) {
	return params.level_offsets[p_level >> 2][p_level & 3];
}

struct Bounds {
	vec3 lo;
	vec3 hi;
	float error;
	uint flags;
};

Bounds node_bounds(int p_level, ivec2 p_node) {
	Bounds b;
	int size = node_samples(p_level);
	ivec2 s0 = p_node * size;
	ivec2 s1 = min(s0 + ivec2(size), ivec2(params.grid.x));

	precise float lowest;
	precise float highest;
	if (p_level >= 0) {
		int row = 1 << (params.grid.y - p_level);
		vec4 d = nodes.data[level_offset(p_level) + uint(p_node.y * row + p_node.x)];
		lowest = d.x;
		highest = d.y;
		b.error = d.z;
		b.flags = uint(d.w);
	} else {
		// Micro levels take the height range of their level 0 node, widened
		// by how far Catmull-Rom interpolation can overshoot it.
		int row = 1 << params.grid.y;
		ivec2 leaf = p_node >> (-p_level);
		vec4 d = nodes.data[level_offset(0) + uint(leaf.y * row + leaf.x)];
		precise float overshoot = (d.y - d.x) * 0.3;
		lowest = d.x - overshoot;
		highest = d.y + overshoot;
		b.error = 0.0;
		b.flags = uint(d.w) & NODE_FLAG_ALL_HOLE;
	}

	precise float spacing = params.micro.w;
	precise float bound = params.micro.y;
	precise vec3 lo = vec3(float(s0.x) * spacing, lowest - bound, float(s0.y) * spacing);
	precise vec3 hi = vec3(float(s1.x) * spacing, highest + bound, float(s1.y) * spacing);
	b.lo = lo;
	b.hi = hi;
	return b;
}

float box_distance(vec3 p_lo, vec3 p_hi) {
	precise vec3 c = params.camera.xyz;
	precise vec3 d = max(max(p_lo - c, c - p_hi), vec3(0.0));
	precise float d2 = d.x * d.x + d.y * d.y + d.z * d.z;
	precise float distance = sqrt(d2);
	return distance;
}

bool should_subdivide(int p_level, ivec2 p_node) {
	if (p_level <= params.grid.z) {
		return false;
	}
	Bounds b = node_bounds(p_level, p_node);
	precise float distance = box_distance(b.lo, b.hi);
	// Every limit is relaxed by the same power of two on later attempts,
	// which leaves the comparisons as exact as on the first.
	precise float divisor = (params.camera.w > 0.5 ? 1.0 : distance) * float(1u << counters.attempt);
	precise float scale = params.lod.x;

	precise float error_pixels = b.error * scale;
	precise float error_limit = params.lod.y * divisor;
	if (error_pixels > error_limit) {
		return true;
	}

	precise float quad = p_level >= 0 ? params.micro.w * float(1 << p_level) : params.micro.w / float(1 << (-p_level));
	precise float quad_pixels = quad * scale;
	precise float quad_limit = params.lod.z * divisor;
	if (quad_pixels > quad_limit) {
		return true;
	}

	precise float micro_limit = params.lod.w * divisor;
	return params.micro.x > 0.0 && distance < params.micro.x && quad_pixels > micro_limit;
}

#endif

#ifdef MODE_TRAVERSE

void main() {
	if (counters.done != 0u) {
		return;
	}
	uint index = gl_GlobalInvocationID.x;
	uint level_index = pc.level_index;

	if (pc.reset != 0u) {
		if (index != 0u) {
			return;
		}
		// The previous attempt fitted: it stands.
		if (pc.attempt > 0u && counters.overflow == 0u) {
			counters.done = 1u;
			return;
		}
		for (uint i = 0u; i < 16u; i++) {
			counters.level_count[i] = 0u;
		}
		// Every list empty but the top level's, which holds the root.
		counters.level_count[level_index] = 1u;
		lists.data[(level_index & 1u) * pc.capacity] = 0u;
		counters.final_count = 0u;
		counters.overflow = 0u;
		counters.attempt = pc.attempt;
		return;
	}

	uint count = min(counters.level_count[level_index], pc.capacity);
	if (index >= count) {
		return;
	}

	uint packed = lists.data[(level_index & 1u) * pc.capacity + index];
	if (packed == INVALID_NODE) {
		return;
	}
	ivec2 node = ivec2(int(packed & 0xffffu), int(packed >> 16u));
	int level = pc.level;
	if (!node_inside(level, node)) {
		return;
	}

	if (should_subdivide(level, node)) {
		uint child_index = level_index - 1u;
		uint base = atomicAdd(counters.level_count[child_index], 4u);
		uint offset = (child_index & 1u) * pc.capacity;
		if (base + 4u <= pc.capacity) {
			uvec2 c = uvec2(node) * 2u;
			lists.data[offset + base + 0u] = (c.x + 0u) | ((c.y + 0u) << 16u);
			lists.data[offset + base + 1u] = (c.x + 1u) | ((c.y + 0u) << 16u);
			lists.data[offset + base + 2u] = (c.x + 0u) | ((c.y + 1u) << 16u);
			lists.data[offset + base + 3u] = (c.x + 1u) | ((c.y + 1u) << 16u);
			return;
		}
		// Out of room: this node is drawn as it is instead, and whatever part
		// of the reserved range still fits is marked unused.
		counters.overflow = 1u;
		for (uint i = base; i < pc.capacity; i++) {
			lists.data[offset + i] = INVALID_NODE;
		}
	}

	uint selected = atomicAdd(counters.final_count, 1u);
	if (selected < pc.capacity) {
		final_nodes.data[selected] = uvec2(packed, uint(level + LEVEL_BIAS));
	} else {
		counters.overflow = 1u;
	}
}

#endif

#ifdef MODE_EMIT

// How many levels coarser the patch across the edge in direction p_dir is:
// the first of its ancestors, from the top, that the traversal did not split.
uint coarser_neighbor(int p_level, ivec2 p_node, ivec2 p_dir) {
	ivec2 neighbor = p_node + p_dir;
	if (!node_inside(p_level, neighbor)) {
		return 0u;
	}
	for (int level = params.grid.y; level > p_level; level--) {
		if (!should_subdivide(level, neighbor >> (level - p_level))) {
			return uint(min(level - p_level, MAX_EDGE_DELTA));
		}
	}
	return 0u;
}

void main() {
	uint index = gl_GlobalInvocationID.x;
	uint count = min(counters.final_count, pc.capacity);
	if (index >= count) {
		return;
	}

	uvec2 selected = final_nodes.data[index];
	ivec2 node = ivec2(int(selected.x & 0xffffu), int(selected.x >> 16u));
	int level = int(selected.y) - LEVEL_BIAS;

	Bounds b = node_bounds(level, node);
	if ((b.flags & NODE_FLAG_ALL_HOLE) != 0u) {
		return;
	}

	uint edges = coarser_neighbor(level, node, ivec2(-1, 0)) |
			(coarser_neighbor(level, node, ivec2(1, 0)) << 4u) |
			(coarser_neighbor(level, node, ivec2(0, -1)) << 8u) |
			(coarser_neighbor(level, node, ivec2(0, 1)) << 12u);

	uvec2 origin = uvec2(node * node_samples(level));
	float data[8];
	data[0] = float(origin.x / ORIGIN_SPLIT);
	data[1] = float(origin.x % ORIGIN_SPLIT);
	data[2] = float(origin.y / ORIGIN_SPLIT);
	data[3] = float(origin.y % ORIGIN_SPLIT);
	data[4] = float(level + LEVEL_BIAS);
	data[5] = float(edges & 0xffu);
	data[6] = float((edges >> 8u) & 0xffu);
	data[7] = 0.0;

	bool in_view = true;
	if ((params.grid.w & FLAG_FRUSTUM_CULLING) != 0) {
		for (int i = 0; i < 6; i++) {
			vec4 plane = params.frustum[i];
			vec3 inner = vec3(plane.x > 0.0 ? b.lo.x : b.hi.x, plane.y > 0.0 ? b.lo.y : b.hi.y, plane.z > 0.0 ? b.lo.z : b.hi.z);
			if (dot(plane.xyz, inner) > plane.w) {
				in_view = false;
				break;
			}
		}
	}

	if (in_view) {
		uint slot = atomicAdd(counters.visible_count, 1u);
		if (slot < pc.capacity) {
			uint base = slot * INSTANCE_FLOATS;
			// Identity transform: the shader places the vertices itself.
			main_instances.data[base + 0u] = 1.0;
			main_instances.data[base + 1u] = 0.0;
			main_instances.data[base + 2u] = 0.0;
			main_instances.data[base + 3u] = 0.0;
			main_instances.data[base + 4u] = 0.0;
			main_instances.data[base + 5u] = 1.0;
			main_instances.data[base + 6u] = 0.0;
			main_instances.data[base + 7u] = 0.0;
			main_instances.data[base + 8u] = 0.0;
			main_instances.data[base + 9u] = 0.0;
			main_instances.data[base + 10u] = 1.0;
			main_instances.data[base + 11u] = 0.0;
			for (uint i = 0u; i < 8u; i++) {
				main_instances.data[base + 12u + i] = data[i];
			}
		}
	}

	if ((params.grid.w & FLAG_SHADOWS) != 0 && (params.micro.z <= 0.0 || box_distance(b.lo, b.hi) <= params.micro.z)) {
		uint slot = atomicAdd(counters.shadow_count, 1u);
		if (slot < pc.capacity) {
			uint base = slot * INSTANCE_FLOATS;
			shadow_instances.data[base + 0u] = 1.0;
			shadow_instances.data[base + 1u] = 0.0;
			shadow_instances.data[base + 2u] = 0.0;
			shadow_instances.data[base + 3u] = 0.0;
			shadow_instances.data[base + 4u] = 0.0;
			shadow_instances.data[base + 5u] = 1.0;
			shadow_instances.data[base + 6u] = 0.0;
			shadow_instances.data[base + 7u] = 0.0;
			shadow_instances.data[base + 8u] = 0.0;
			shadow_instances.data[base + 9u] = 0.0;
			shadow_instances.data[base + 10u] = 1.0;
			shadow_instances.data[base + 11u] = 0.0;
			for (uint i = 0u; i < 8u; i++) {
				shadow_instances.data[base + 12u + i] = data[i];
			}
		}
	}
}

#endif

#ifdef MODE_APPLY

// The indirect command layout is the standard indexed one (index count,
// instance count, first index, vertex offset, first instance), once per
// surface; the patch mesh has one.
void main() {
	if (gl_GlobalInvocationID.x != 0u) {
		return;
	}
	main_commands.data[1] = min(counters.visible_count, pc.capacity);
	shadow_commands.data[1] = min(counters.shadow_count, pc.capacity);
}

#endif
)";

// Mirrors the shader's Params block, std140.
struct LandscapeGPUParams {
	float frustum[6][4];
	float camera[4];
	float lod[4];
	float micro[4];
	int32_t grid[4];
	uint32_t level_offsets[16];
};

static_assert(sizeof(LandscapeGPUParams) == 224, "LandscapeGPUParams must match the shader's std140 layout.");

struct LandscapePushConstant {
	int32_t level;
	uint32_t level_index;
	uint32_t capacity;
	uint32_t attempt;
	uint32_t reset;
	uint32_t pad[3];
};

// Mirrors the shader's Counters block.
struct LandscapeCounters {
	uint32_t level_count[16];
	uint32_t final_count;
	uint32_t visible_count;
	uint32_t shadow_count;
	uint32_t overflow;
	uint32_t attempt;
	uint32_t done;
	uint32_t pad[2];
};

// Attempts at a selection that fits the lists, each with twice the screen
// space error of the last.
constexpr uint32_t LANDSCAPE_MAX_ATTEMPTS = 4;

static_assert(LandscapeQuadtree::MAX_LEVEL_COUNT <= 16, "The shader's counters hold 16 levels.");
static_assert(LandscapeQuadtree::MAX_TOP_LEVEL < 16, "The shader's level offsets hold 16 levels.");

RD::Uniform make_uniform(RD::UniformType p_type, int p_binding, RID p_id) {
	RD::Uniform uniform;
	uniform.uniform_type = p_type;
	uniform.binding = p_binding;
	uniform.append_id(p_id);
	return uniform;
}

void free_owned(RenderingDevice *p_rd, RID &r_rid) {
	if (r_rid.is_valid()) {
		p_rd->free_rid(r_rid);
		r_rid = RID();
	}
}

} // namespace

/////////////////////////////////////////////////////////////////////////////
// LandscapeTextureResources

void LandscapeTextureResources::rt_create(RID p_texture, int p_format, int p_width, int p_height, int p_mipmaps, bool p_array, const Array &p_layers) {
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	ERR_FAIL_NULL(rd);

	RD::TextureFormat tf;
	tf.format = get_rd_format(Image::Format(p_format));
	ERR_FAIL_COND(tf.format == RD::DATA_FORMAT_MAX);
	tf.width = p_width;
	tf.height = p_height;
	tf.depth = 1;
	tf.array_layers = p_layers.size();
	tf.mipmaps = p_mipmaps;
	tf.texture_type = p_array ? RD::TEXTURE_TYPE_2D_ARRAY : RD::TEXTURE_TYPE_2D;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;

	Vector<Vector<uint8_t>> data;
	for (int i = 0; i < p_layers.size(); i++) {
		data.push_back(PackedByteArray(p_layers[i]));
	}

	const RID created = rd->texture_create(tf, RD::TextureView(), data);
	ERR_FAIL_COND(created.is_null());

	// The server's texture only ever views this one, so it has to let go of
	// the previous texture before that can be freed.
	RS::get_singleton()->texture_replace(p_texture, RS::get_singleton()->texture_rd_create(created, RSE::TEXTURE_LAYERED_2D_ARRAY));
	free_owned(rd, rd_texture);
	rd_texture = created;
}

void LandscapeTextureResources::rt_update(int p_layer, const PackedInt32Array &p_regions, const PackedByteArray &p_data) {
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	ERR_FAIL_NULL(rd);
	if (rd_texture.is_null()) {
		return;
	}

	const RD::TextureFormat format = rd->texture_get_format(rd_texture);
	const int pixel_size = RD::get_image_format_pixel_size(format.format);

	// Every region is staged in a texture of its own and copied into place:
	// RenderingDevice updates whole layers only.
	int64_t offset = 0;
	for (int i = 0; i + 4 < p_regions.size(); i += 5) {
		const int mipmap = p_regions[i];
		const int x = p_regions[i + 1];
		const int y = p_regions[i + 2];
		const int w = p_regions[i + 3];
		const int h = p_regions[i + 4];
		const int64_t size = int64_t(w) * h * pixel_size;
		ERR_FAIL_COND(offset + size > p_data.size());

		RD::TextureFormat staging_format;
		staging_format.format = format.format;
		staging_format.width = w;
		staging_format.height = h;
		staging_format.usage_bits = RD::TEXTURE_USAGE_CAN_UPDATE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;

		Vector<Vector<uint8_t>> staging_data;
		staging_data.push_back(p_data.slice(offset, offset + size));
		offset += size;

		RID staging = rd->texture_create(staging_format, RD::TextureView(), staging_data);
		ERR_CONTINUE(staging.is_null());
		rd->texture_copy(staging, rd_texture, Vector3(), Vector3(x, y, 0), Vector3(w, h, 1), 0, mipmap, 0, p_layer);
		rd->free_rid(staging);
	}
}

void LandscapeTextureResources::rt_free() {
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	if (rd != nullptr) {
		free_owned(rd, rd_texture);
	}
	rd_texture = RID();
}

/////////////////////////////////////////////////////////////////////////////
// LandscapeGPUResources

bool LandscapeGPUResources::_ensure_pipelines() {
	if (traverse_pipeline.is_valid() && emit_pipeline.is_valid() && apply_pipeline.is_valid()) {
		return true;
	}

	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	ERR_FAIL_NULL_V(rd, false);

	struct Program {
		const char *define;
		const char *name;
		RID *shader;
		RID *pipeline;
	};
	const Program programs[3] = {
		{ "MODE_TRAVERSE", "LandscapeQuadtreeTraverse", &traverse_shader, &traverse_pipeline },
		{ "MODE_EMIT", "LandscapeQuadtreeEmit", &emit_shader, &emit_pipeline },
		{ "MODE_APPLY", "LandscapeQuadtreeApply", &apply_shader, &apply_pipeline },
	};

	for (const Program &program : programs) {
		if (program.pipeline->is_valid()) {
			continue;
		}

		const String source = String("#version 450\n#define ") + program.define + "\n" + LANDSCAPE_QUADTREE_SHADER_GLSL;
		String error;
		Vector<uint8_t> spirv = rd->shader_compile_spirv_from_source(RD::SHADER_STAGE_COMPUTE, source, RD::SHADER_LANGUAGE_GLSL, &error);
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

bool LandscapeGPUResources::_ensure_buffers() {
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	ERR_FAIL_NULL_V(rd, false);

	if (params_buffer.is_null()) {
		params_buffer = rd->uniform_buffer_create(sizeof(LandscapeGPUParams));
		ERR_FAIL_COND_V(params_buffer.is_null(), false);
	}
	if (list_buffer.is_null()) {
		list_buffer = rd->storage_buffer_create(2 * capacity * sizeof(uint32_t));
		ERR_FAIL_COND_V(list_buffer.is_null(), false);
	}
	if (final_buffer.is_null()) {
		final_buffer = rd->storage_buffer_create(capacity * 2 * sizeof(uint32_t));
		ERR_FAIL_COND_V(final_buffer.is_null(), false);
	}
	if (counter_buffer.is_null()) {
		counter_buffer = rd->storage_buffer_create(sizeof(LandscapeCounters));
		ERR_FAIL_COND_V(counter_buffer.is_null(), false);
	}
	return true;
}

bool LandscapeGPUResources::_ensure_sets() {
	if (traverse_set.is_valid() && emit_set.is_valid() && emit_output_set.is_valid() && apply_set.is_valid() && apply_output_set.is_valid()) {
		return true;
	}
	if (node_buffer.is_null() || instance_buffers[0].is_null() || instance_buffers[1].is_null() || command_buffers[0].is_null() || command_buffers[1].is_null()) {
		return false;
	}

	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	ERR_FAIL_NULL_V(rd, false);
	_free_sets();

	{
		Vector<RD::Uniform> uniforms;
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, params_buffer));
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, node_buffer));
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, list_buffer));
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, final_buffer));
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, counter_buffer));
		traverse_set = rd->uniform_set_create(uniforms, traverse_shader, 0);
	}
	{
		Vector<RD::Uniform> uniforms;
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, params_buffer));
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, node_buffer));
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, final_buffer));
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, counter_buffer));
		emit_set = rd->uniform_set_create(uniforms, emit_shader, 0);
	}
	{
		Vector<RD::Uniform> uniforms;
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, instance_buffers[0]));
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, instance_buffers[1]));
		emit_output_set = rd->uniform_set_create(uniforms, emit_shader, 1);
	}
	{
		Vector<RD::Uniform> uniforms;
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, counter_buffer));
		apply_set = rd->uniform_set_create(uniforms, apply_shader, 0);
	}
	{
		Vector<RD::Uniform> uniforms;
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, command_buffers[0]));
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, command_buffers[1]));
		apply_output_set = rd->uniform_set_create(uniforms, apply_shader, 1);
	}

	if (traverse_set.is_null() || emit_set.is_null() || emit_output_set.is_null() || apply_set.is_null() || apply_output_set.is_null()) {
		_free_sets();
		ERR_FAIL_V_MSG(false, "Could not create the landscape quadtree uniform sets.");
	}
	return true;
}

void LandscapeGPUResources::_free_sets() {
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	if (rd == nullptr) {
		return;
	}
	// A uniform set is freed along with any buffer it uses, so one may
	// already be gone.
	RID *sets[5] = { &traverse_set, &emit_set, &emit_output_set, &apply_set, &apply_output_set };
	for (RID *set : sets) {
		if (set->is_valid() && rd->uniform_set_is_valid(*set)) {
			rd->free_rid(*set);
		}
		*set = RID();
	}
}

void LandscapeGPUResources::_free_all() {
	_free_sets();

	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	if (rd == nullptr) {
		return;
	}
	// Pipelines before the shaders they were made from.
	RID *owned[11] = { &traverse_pipeline, &emit_pipeline, &apply_pipeline, &traverse_shader, &emit_shader, &apply_shader, &params_buffer, &node_buffer, &list_buffer, &final_buffer, &counter_buffer };
	for (RID *rid : owned) {
		free_owned(rd, *rid);
	}
	node_count = 0;
	for (int i = 0; i < 2; i++) {
		instance_buffers[i] = RID();
		command_buffers[i] = RID();
	}
}

void LandscapeGPUResources::rt_set_nodes(const PackedFloat32Array &p_nodes) {
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	ERR_FAIL_NULL(rd);

	_free_sets();
	free_owned(rd, node_buffer);
	node_count = uint32_t(p_nodes.size() / LandscapeQuadtree::NODE_FLOATS);
	if (node_count == 0) {
		return;
	}
	const Vector<uint8_t> bytes = p_nodes.to_byte_array();
	node_buffer = rd->storage_buffer_create(bytes.size(), bytes.span());
	ERR_FAIL_COND(node_buffer.is_null());
}

void LandscapeGPUResources::rt_update_nodes(const PackedInt32Array &p_ranges, const PackedFloat32Array &p_data) {
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	ERR_FAIL_NULL(rd);
	if (node_buffer.is_null()) {
		return;
	}

	int64_t consumed = 0;
	for (int i = 0; i + 1 < p_ranges.size(); i += 2) {
		const uint32_t first = uint32_t(p_ranges[i]);
		const uint32_t count = uint32_t(p_ranges[i + 1]);
		ERR_FAIL_COND(first + count > node_count);
		const int64_t floats = int64_t(count) * LandscapeQuadtree::NODE_FLOATS;
		ERR_FAIL_COND(consumed + floats > p_data.size());
		rd->buffer_update(node_buffer, first * LandscapeQuadtree::NODE_FLOATS * sizeof(float), floats * sizeof(float), p_data.ptr() + consumed);
		consumed += floats;
	}
}

void LandscapeGPUResources::rt_set_outputs(RID p_main_multimesh, RID p_shadow_multimesh, uint32_t p_capacity) {
	_free_sets();

	if (p_capacity != capacity) {
		// The lists hold as many nodes as the MultiMeshes hold patches.
		RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
		ERR_FAIL_NULL(rd);
		free_owned(rd, list_buffer);
		free_owned(rd, final_buffer);
		capacity = p_capacity;
	}

	RenderingServer *rs = RenderingServer::get_singleton();
	const RID multimeshes[2] = { p_main_multimesh, p_shadow_multimesh };
	for (int i = 0; i < 2; i++) {
		instance_buffers[i] = multimeshes[i].is_valid() ? rs->multimesh_get_buffer_rd_rid(multimeshes[i]) : RID();
		command_buffers[i] = multimeshes[i].is_valid() ? rs->multimesh_get_command_buffer_rd_rid(multimeshes[i]) : RID();
	}
}

void LandscapeGPUResources::rt_dispatch(const PackedByteArray &p_params, int p_top_level, int p_min_level) {
	if (failed.is_set()) {
		return;
	}
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	ERR_FAIL_NULL(rd);
	ERR_FAIL_COND(p_params.size() != sizeof(LandscapeGPUParams));
	ERR_FAIL_COND(p_top_level - p_min_level + 1 > 16 || p_min_level > 0);

	if (!_ensure_pipelines()) {
		failed.set();
		return;
	}
	if (!_ensure_buffers() || !_ensure_sets()) {
		return;
	}

	if (capacity == 0) {
		return;
	}
	const uint32_t top_index = uint32_t(p_top_level - p_min_level);

	rd->buffer_update(params_buffer, 0, sizeof(LandscapeGPUParams), p_params.ptr());

	const LandscapeCounters counters = {};
	rd->buffer_update(counter_buffer, 0, sizeof(LandscapeCounters), &counters);

	RD::ComputeListID compute_list = rd->compute_list_begin();

	rd->compute_list_bind_compute_pipeline(compute_list, traverse_pipeline);
	rd->compute_list_bind_uniform_set(compute_list, traverse_set, 0);
	// A selection that does not fit the lists would have to leave patches out,
	// and holes in the ground where they were. So whenever one does not fit,
	// it is made again, with the screen space error relaxed: coarser ground for
	// a frame or two, until the lists have grown to fit (see
	// get_wanted_capacity()). Once an attempt fits, the next ones do nothing.
	for (uint32_t attempt = 0; attempt < LANDSCAPE_MAX_ATTEMPTS; attempt++) {
		LandscapePushConstant reset = {};
		reset.level_index = top_index;
		reset.capacity = capacity;
		reset.attempt = attempt;
		reset.reset = 1;
		rd->compute_list_set_push_constant(compute_list, &reset, sizeof(LandscapePushConstant));
		rd->compute_list_dispatch(compute_list, 1, 1, 1);
		rd->compute_list_add_barrier(compute_list);

		for (int level = p_top_level; level >= p_min_level; level--) {
			LandscapePushConstant push_constant = {};
			push_constant.level = level;
			push_constant.level_index = uint32_t(level - p_min_level);
			push_constant.capacity = capacity;
			push_constant.attempt = attempt;
			// No more nodes than the level has, nor than a list holds.
			const uint32_t level_nodes = level >= 0 ? MIN(1u << (2 * (p_top_level - level)), capacity) : capacity;
			rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(LandscapePushConstant));
			rd->compute_list_dispatch(compute_list, Math::division_round_up(level_nodes, (uint32_t)LANDSCAPE_GROUP_SIZE), 1, 1);
			// Each level reads the list the one before it wrote.
			rd->compute_list_add_barrier(compute_list);
		}
	}

	LandscapePushConstant push_constant = {};
	push_constant.capacity = capacity;

	rd->compute_list_bind_compute_pipeline(compute_list, emit_pipeline);
	rd->compute_list_bind_uniform_set(compute_list, emit_set, 0);
	rd->compute_list_bind_uniform_set(compute_list, emit_output_set, 1);
	rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(LandscapePushConstant));
	rd->compute_list_dispatch(compute_list, Math::division_round_up(capacity, (uint32_t)LANDSCAPE_GROUP_SIZE), 1, 1);

	rd->compute_list_add_barrier(compute_list);

	rd->compute_list_bind_compute_pipeline(compute_list, apply_pipeline);
	rd->compute_list_bind_uniform_set(compute_list, apply_set, 0);
	rd->compute_list_bind_uniform_set(compute_list, apply_output_set, 1);
	rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(LandscapePushConstant));
	rd->compute_list_dispatch(compute_list, 1, 1, 1);

	rd->compute_list_end();

	// Whether that took more than one attempt only shows a few frames later;
	// one look at a time is plenty to grow the lists by.
	if (!counters_pending.is_set()) {
		counters_pending.set();
		const Callable callback = callable_mp_static(&LandscapeGPUResources::_rt_counters_read).bind(Ref<LandscapeGPUResources>(this), capacity);
		if (rd->buffer_get_data_async(counter_buffer, callback, 0, sizeof(LandscapeCounters)) != OK) {
			counters_pending.clear();
		}
	}
}

void LandscapeGPUResources::_rt_counters_read(const PackedByteArray &p_data, const Ref<LandscapeGPUResources> &p_resources, uint32_t p_capacity) {
	ERR_FAIL_COND(p_resources.is_null());
	p_resources->counters_pending.clear();
	ERR_FAIL_COND(p_data.size() < int64_t(sizeof(LandscapeCounters)));

	LandscapeCounters counters;
	memcpy(&counters, p_data.ptr(), sizeof(LandscapeCounters));
	const bool fitted_first = counters.attempt == 0 && counters.overflow == 0;
	if (!fitted_first) {
		// Grown a step at a time: the counts of an attempt that ran out of
		// room only say that more was needed, not how much.
		const uint32_t wanted = MIN(p_capacity * 2, LandscapeGPUQuadtree::MAX_CAPACITY);
		if (wanted > p_resources->wanted_capacity.get()) {
			p_resources->wanted_capacity.set(wanted);
		}
	}
}

#else // !RD_ENABLED

void LandscapeTextureResources::rt_create(RID p_texture, int p_format, int p_width, int p_height, int p_mipmaps, bool p_array, const Array &p_layers) {}
void LandscapeTextureResources::rt_update(int p_layer, const PackedInt32Array &p_regions, const PackedByteArray &p_data) {}
void LandscapeTextureResources::rt_free() {}

bool LandscapeGPUResources::_ensure_pipelines() {
	return false;
}
bool LandscapeGPUResources::_ensure_buffers() {
	return false;
}
bool LandscapeGPUResources::_ensure_sets() {
	return false;
}
void LandscapeGPUResources::_free_sets() {}
void LandscapeGPUResources::_free_all() {}
void LandscapeGPUResources::rt_set_nodes(const PackedFloat32Array &p_nodes) {}
void LandscapeGPUResources::rt_update_nodes(const PackedInt32Array &p_ranges, const PackedFloat32Array &p_data) {}
void LandscapeGPUResources::rt_set_outputs(RID p_main_multimesh, RID p_shadow_multimesh, uint32_t p_capacity) {}
void LandscapeGPUResources::rt_dispatch(const PackedByteArray &p_params, int p_top_level, int p_min_level) {}
void LandscapeGPUResources::_rt_counters_read(const PackedByteArray &p_data, const Ref<LandscapeGPUResources> &p_resources, uint32_t p_capacity) {}

#endif // RD_ENABLED

/////////////////////////////////////////////////////////////////////////////
// LandscapeGPUTexture

bool LandscapeGPUTexture::supports_partial_updates() {
#ifdef RD_ENABLED
	RenderingServer *rs = RenderingServer::get_singleton();
	return rs != nullptr && rs->get_rendering_device() != nullptr;
#else
	return false;
#endif
}

void LandscapeGPUTexture::create(const Vector<Ref<Image>> &p_layers, bool p_array) {
	free();
	ERR_FAIL_COND(p_layers.is_empty() || p_layers[0].is_null());

	const Ref<Image> &first = p_layers[0];
	format = first->get_format();
	width = first->get_width();
	height = first->get_height();
	mipmaps = first->has_mipmaps();
	layer_count = p_layers.size();
	array = p_array;
	ERR_FAIL_COND(!array && layer_count != 1);
	// The rendering server cannot wrap a single layer RenderingDevice texture
	// as an array.
	ERR_FAIL_COND_MSG(array && layer_count < 2, "A LandscapeGPUTexture array needs two layers or more.");
	for (const Ref<Image> &layer : p_layers) {
		ERR_FAIL_COND(layer.is_null() || layer->get_format() != format || layer->get_width() != width || layer->get_height() != height || layer->has_mipmaps() != mipmaps);
	}

	RenderingServer *rs = RenderingServer::get_singleton();
	if (!supports_partial_updates()) {
		texture = array ? rs->texture_2d_layered_create(p_layers, RSE::TEXTURE_LAYERED_2D_ARRAY) : rs->texture_2d_create(first);
		pending_layers.resize(layer_count);
		return;
	}

	// The server's texture stands in until the rendering thread has created
	// the real one, then becomes a view of it.
	texture = array ? rs->texture_2d_layered_placeholder_create(RSE::TEXTURE_LAYERED_2D_ARRAY) : rs->texture_2d_placeholder_create();
	resources.instantiate();

	Array layer_data;
	for (const Ref<Image> &layer : p_layers) {
		layer_data.push_back(layer->get_data());
	}
	rs->call_on_render_thread(callable_mp(resources.ptr(), &LandscapeTextureResources::rt_create).bind(texture, int(format), width, height, mipmaps ? first->get_mipmap_count() + 1 : 1, array, layer_data));
}

void LandscapeGPUTexture::update(int p_layer, const Ref<Image> &p_image, const Rect2i &p_region) {
	ERR_FAIL_COND(texture.is_null());
	ERR_FAIL_INDEX(p_layer, layer_count);
	ERR_FAIL_COND(p_image.is_null() || p_image->get_width() != width || p_image->get_height() != height || p_image->get_format() != format || p_image->has_mipmaps() != mipmaps);

	if (resources.is_null()) {
		// Compatibility renderer: the whole layer goes up on the next flush().
		pending_layers[p_layer] = p_image;
		return;
	}

	const Rect2i region = p_region.intersection(Rect2i(0, 0, width, height));
	if (region.size.x <= 0 || region.size.y <= 0) {
		return;
	}

	// The changed pixels of every mipmap: a texel of mipmap m averages the
	// block of texels of mipmap 0 it covers, so the region halves with it.
	const int pixel_size = Image::get_format_pixel_size(format);
	const int mipmap_count = mipmaps ? p_image->get_mipmap_count() + 1 : 1;
	PackedInt32Array regions;
	PackedByteArray data;
	const uint8_t *src = p_image->ptr();
	for (int mipmap = 0; mipmap < mipmap_count; mipmap++) {
		int64_t mip_offset = 0;
		int64_t mip_size = 0;
		int mip_width = 0;
		int mip_height = 0;
		p_image->get_mipmap_offset_size_and_dimensions(mipmap, mip_offset, mip_size, mip_width, mip_height);

		const int x0 = MIN(region.position.x >> mipmap, mip_width - 1);
		const int y0 = MIN(region.position.y >> mipmap, mip_height - 1);
		const int x1 = MIN((region.position.x + region.size.x - 1) >> mipmap, mip_width - 1);
		const int y1 = MIN((region.position.y + region.size.y - 1) >> mipmap, mip_height - 1);
		const int w = x1 - x0 + 1;
		const int h = y1 - y0 + 1;

		regions.push_back(mipmap);
		regions.push_back(x0);
		regions.push_back(y0);
		regions.push_back(w);
		regions.push_back(h);

		const int64_t row_bytes = int64_t(w) * pixel_size;
		const int64_t start = data.size();
		data.resize(start + row_bytes * h);
		uint8_t *dst = data.ptrw() + start;
		for (int y = 0; y < h; y++) {
			memcpy(dst + y * row_bytes, src + mip_offset + (int64_t(y0 + y) * mip_width + x0) * pixel_size, row_bytes);
		}
	}

	RenderingServer::get_singleton()->call_on_render_thread(callable_mp(resources.ptr(), &LandscapeTextureResources::rt_update).bind(p_layer, regions, data));
}

void LandscapeGPUTexture::flush() {
	if (resources.is_valid() || texture.is_null()) {
		return;
	}
	for (int i = 0; i < int(pending_layers.size()); i++) {
		if (pending_layers[i].is_null()) {
			continue;
		}
		// A copy of the image, since the one being edited keeps changing
		// while the rendering thread may still be reading it; its pixel data
		// is shared until the next edit.
		Ref<Image> copy;
		copy.instantiate();
		copy->copy_internals_from(pending_layers[i]);
		RenderingServer::get_singleton()->texture_2d_update(texture, copy, i);
		pending_layers[i].unref();
	}
}

void LandscapeGPUTexture::free() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (texture.is_valid() && rs != nullptr) {
		rs->free_rid(texture);
	}
	texture = RID();
	if (resources.is_valid() && rs != nullptr) {
		// Queued after the view above is freed, so the texture it viewed is
		// no longer in use by then.
		rs->call_on_render_thread(callable_mp(resources.ptr(), &LandscapeTextureResources::rt_free));
	}
	resources.unref();
	pending_layers.clear();
	layer_count = 0;
	width = 0;
	height = 0;
}

LandscapeGPUTexture::~LandscapeGPUTexture() {
	free();
}

/////////////////////////////////////////////////////////////////////////////
// LandscapeGPUQuadtree

bool LandscapeGPUQuadtree::is_supported() {
#ifdef RD_ENABLED
	RenderingServer *rs = RenderingServer::get_singleton();
	return rs != nullptr && rs->get_rendering_device() != nullptr;
#else
	return false;
#endif
}

bool LandscapeGPUQuadtree::has_failed() const {
	return resources.is_valid() && resources->failed.is_set();
}

void LandscapeGPUQuadtree::set_nodes(const LandscapeQuadtree &p_quadtree) {
	if (!is_supported()) {
		return;
	}
	if (resources.is_null()) {
		resources.instantiate();
	}

	const LocalVector<float> &data = p_quadtree.get_node_data();
	PackedFloat32Array nodes;
	nodes.resize(data.size());
	if (data.size() > 0) {
		memcpy(nodes.ptrw(), data.ptr(), data.size() * sizeof(float));
	}
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp(resources.ptr(), &LandscapeGPUResources::rt_set_nodes).bind(nodes));
}

void LandscapeGPUQuadtree::update_nodes(const LandscapeQuadtree &p_quadtree, const LocalVector<Vector2i> &p_ranges) {
	if (resources.is_null() || p_ranges.is_empty()) {
		return;
	}

	const LocalVector<float> &data = p_quadtree.get_node_data();
	PackedInt32Array ranges;
	PackedFloat32Array values;
	for (const Vector2i &range : p_ranges) {
		ranges.push_back(range.x);
		ranges.push_back(range.y);
		const int64_t start = values.size();
		const int64_t floats = int64_t(range.y) * LandscapeQuadtree::NODE_FLOATS;
		values.resize(start + floats);
		memcpy(values.ptrw() + start, data.ptr() + int64_t(range.x) * LandscapeQuadtree::NODE_FLOATS, floats * sizeof(float));
	}
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp(resources.ptr(), &LandscapeGPUResources::rt_update_nodes).bind(ranges, values));
}

void LandscapeGPUQuadtree::set_outputs(RID p_main_multimesh, RID p_shadow_multimesh) {
	if (!is_supported()) {
		return;
	}
	if (resources.is_null()) {
		resources.instantiate();
	}
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp(resources.ptr(), &LandscapeGPUResources::rt_set_outputs).bind(p_main_multimesh, p_shadow_multimesh, capacity));
}

void LandscapeGPUQuadtree::set_capacity(uint32_t p_capacity) {
	capacity = CLAMP(p_capacity, INITIAL_CAPACITY, MAX_CAPACITY);
}

uint32_t LandscapeGPUQuadtree::get_wanted_capacity() const {
	if (resources.is_null()) {
		return capacity;
	}
	return MAX(capacity, resources->wanted_capacity.get());
}

void LandscapeGPUQuadtree::dispatch(const LandscapeQuadtree &p_quadtree, const LandscapeQuadtree::SelectParams &p_params) {
	if (resources.is_null() || p_quadtree.is_empty()) {
		return;
	}

#ifdef RD_ENABLED
	LandscapeGPUParams params = {};
	for (int i = 0; i < 6; i++) {
		params.frustum[i][0] = p_params.frustum[i].normal.x;
		params.frustum[i][1] = p_params.frustum[i].normal.y;
		params.frustum[i][2] = p_params.frustum[i].normal.z;
		params.frustum[i][3] = p_params.frustum[i].d;
	}
	params.camera[0] = p_params.camera_position.x;
	params.camera[1] = p_params.camera_position.y;
	params.camera[2] = p_params.camera_position.z;
	params.camera[3] = p_params.orthogonal ? 1.0f : 0.0f;
	params.lod[0] = p_params.pixel_scale;
	params.lod[1] = p_params.pixel_error;
	params.lod[2] = p_params.max_quad_pixels;
	params.lod[3] = p_params.micro_quad_pixels;
	params.micro[0] = p_params.micro_distance;
	params.micro[1] = p_params.displacement_bound;
	params.micro[2] = p_params.shadow_distance;
	params.micro[3] = p_quadtree.get_spacing();
	params.grid[0] = p_quadtree.get_resolution() - 1;
	params.grid[1] = p_quadtree.get_top_level();
	params.grid[2] = p_params.min_level;
	params.grid[3] = (p_params.frustum_culling ? 1 : 0) | (p_params.shadows ? 2 : 0);
	for (int level = 0; level <= p_quadtree.get_top_level(); level++) {
		params.level_offsets[level] = p_quadtree.get_level_offset(level);
	}

	PackedByteArray bytes;
	bytes.resize(sizeof(LandscapeGPUParams));
	memcpy(bytes.ptrw(), &params, sizeof(LandscapeGPUParams));

	RenderingServer::get_singleton()->call_on_render_thread(callable_mp(resources.ptr(), &LandscapeGPUResources::rt_dispatch).bind(bytes, p_quadtree.get_top_level(), p_params.min_level));
#endif
}

void LandscapeGPUQuadtree::release() {
	if (resources.is_null()) {
		return;
	}
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&LandscapeGPUQuadtree::rt_free).bind(resources));
	resources.unref();
}

void LandscapeGPUQuadtree::rt_free(const Ref<LandscapeGPUResources> &p_resources) {
	if (p_resources.is_valid()) {
		p_resources->_free_all();
	}
}

LandscapeGPUQuadtree::~LandscapeGPUQuadtree() {
	release();
}
