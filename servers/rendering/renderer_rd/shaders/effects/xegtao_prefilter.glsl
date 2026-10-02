///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2016-2021, Intel Corporation
// SPDX-License-Identifier: MIT
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of
// the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
// TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// XeGTAO is based on GTAO/GTSO "Jimenez et al. / Practical Real-Time Strategies for Accurate Indirect Occlusion".
// Implementation: Filip Strugar (filip.strugar@intel.com), Steve Mccalla <stephen.mccalla@intel.com>
// Details:        https://github.com/GameTechDev/XeGTAO (version 1.30, with bent normals)
// Ported to Vulkan GLSL and Godot from XeGTAO.h / XeGTAO.hlsli / vaGTAO.hlsl.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// XeGTAO pass 1 (XeGTAO_PrefilterDepths16x16): converts the hardware depth buffer into a view space depth
// buffer and builds its 5-level MIP chain in one dispatch. Each thread handles a 2x2 block of MIP 0, so
// one 8x8 group covers 16x16 pixels and can reduce them all the way down to MIP 4 through shared memory.
//
// At full resolution MIP 0 is a 1:1 conversion of the depth buffer. At half resolution it is a point
// sampled reduction instead (texel p holds full resolution texel 2p), so that every working pixel stands
// for one real surface whose normal the main pass can fetch directly; the MIP filter's averaging would
// otherwise produce depths of surfaces that aren't there at the silhouettes.

#[compute]

#version 450

#VERSION_DEFINES

#include "xegtao_inc.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source_depth;

layout(r32f, set = 1, binding = 0) uniform restrict writeonly image2D dest_depth_mip0;
layout(r32f, set = 1, binding = 1) uniform restrict writeonly image2D dest_depth_mip1;
layout(r32f, set = 1, binding = 2) uniform restrict writeonly image2D dest_depth_mip2;
layout(r32f, set = 1, binding = 3) uniform restrict writeonly image2D dest_depth_mip3;
layout(r32f, set = 1, binding = 4) uniform restrict writeonly image2D dest_depth_mip4;

layout(push_constant, std430) uniform Params {
	ivec2 source_size;
	ivec2 working_size; // Size of MIP 0.

	float depth_linearize_mul;
	float depth_linearize_add;
	float z_near;
	float z_far;

	bool is_orthogonal;
	int source_scale; // 1 at full resolution, 2 at half resolution.
	float mip_falloff_mul;
	float mip_falloff_add;

	int mip_count;
	int pad[3];
}
params;

shared float scratch_depths[8][8];

float load_depth(ivec2 p_working_pos) {
	ivec2 source_pos = clamp(p_working_pos * params.source_scale, ivec2(0), params.source_size - 1);
	float depth = texelFetch(source_depth, source_pos, 0).x;
	float view_depth = xegtao_linearize_depth(depth, params.is_orthogonal, params.depth_linearize_mul, params.depth_linearize_add, params.z_near, params.z_far);
	// XeGTAO_ClampDepth(), for 32-bit depths.
	return clamp(view_depth, 0.0, 3.402823466e+38);
}

// XeGTAO_DepthMIPFilter(): a weighted average that leans on the most distant sample of the four, which
// introduces a natural thin occluder bias and is more stable under motion than picking any one sample.
float depth_mip_filter(float p_depth0, float p_depth1, float p_depth2, float p_depth3) {
	float max_depth = max(max(p_depth0, p_depth1), max(p_depth2, p_depth3));

	float weight0 = clamp((max_depth - p_depth0) * params.mip_falloff_mul + params.mip_falloff_add, 0.0, 1.0);
	float weight1 = clamp((max_depth - p_depth1) * params.mip_falloff_mul + params.mip_falloff_add, 0.0, 1.0);
	float weight2 = clamp((max_depth - p_depth2) * params.mip_falloff_mul + params.mip_falloff_add, 0.0, 1.0);
	float weight3 = clamp((max_depth - p_depth3) * params.mip_falloff_mul + params.mip_falloff_add, 0.0, 1.0);

	float weight_sum = weight0 + weight1 + weight2 + weight3;
	return (weight0 * p_depth0 + weight1 * p_depth1 + weight2 * p_depth2 + weight3 * p_depth3) / weight_sum;
}

void store_mip(int p_mip, ivec2 p_pos, float p_depth) {
	ivec2 mip_size = max(params.working_size >> p_mip, ivec2(1));
	if (p_mip >= params.mip_count || any(greaterThanEqual(p_pos, mip_size))) {
		return;
	}

	switch (p_mip) {
		case 0:
			imageStore(dest_depth_mip0, p_pos, vec4(p_depth));
			break;
		case 1:
			imageStore(dest_depth_mip1, p_pos, vec4(p_depth));
			break;
		case 2:
			imageStore(dest_depth_mip2, p_pos, vec4(p_depth));
			break;
		case 3:
			imageStore(dest_depth_mip3, p_pos, vec4(p_depth));
			break;
		case 4:
			imageStore(dest_depth_mip4, p_pos, vec4(p_depth));
			break;
	}
}

void main() {
	// No early out anywhere in here: every thread of the group has to reach every barrier.
	ivec2 base_coord = ivec2(gl_GlobalInvocationID.xy);
	ivec2 group_thread = ivec2(gl_LocalInvocationID.xy);

	// MIP 0
	ivec2 pix_coord = base_coord * 2;
	float depth0 = load_depth(pix_coord + ivec2(0, 0));
	float depth1 = load_depth(pix_coord + ivec2(1, 0));
	float depth2 = load_depth(pix_coord + ivec2(0, 1));
	float depth3 = load_depth(pix_coord + ivec2(1, 1));
	store_mip(0, pix_coord + ivec2(0, 0), depth0);
	store_mip(0, pix_coord + ivec2(1, 0), depth1);
	store_mip(0, pix_coord + ivec2(0, 1), depth2);
	store_mip(0, pix_coord + ivec2(1, 1), depth3);

	// MIP 1
	float dm1 = depth_mip_filter(depth0, depth1, depth2, depth3);
	store_mip(1, base_coord, dm1);
	scratch_depths[group_thread.x][group_thread.y] = dm1;

	memoryBarrierShared();
	barrier();

	// MIP 2
	if (all(equal(group_thread % 2, ivec2(0)))) {
		float in_tl = scratch_depths[group_thread.x + 0][group_thread.y + 0];
		float in_tr = scratch_depths[group_thread.x + 1][group_thread.y + 0];
		float in_bl = scratch_depths[group_thread.x + 0][group_thread.y + 1];
		float in_br = scratch_depths[group_thread.x + 1][group_thread.y + 1];

		float dm2 = depth_mip_filter(in_tl, in_tr, in_bl, in_br);
		store_mip(2, base_coord / 2, dm2);
		scratch_depths[group_thread.x][group_thread.y] = dm2;
	}

	memoryBarrierShared();
	barrier();

	// MIP 3
	if (all(equal(group_thread % 4, ivec2(0)))) {
		float in_tl = scratch_depths[group_thread.x + 0][group_thread.y + 0];
		float in_tr = scratch_depths[group_thread.x + 2][group_thread.y + 0];
		float in_bl = scratch_depths[group_thread.x + 0][group_thread.y + 2];
		float in_br = scratch_depths[group_thread.x + 2][group_thread.y + 2];

		float dm3 = depth_mip_filter(in_tl, in_tr, in_bl, in_br);
		store_mip(3, base_coord / 4, dm3);
		scratch_depths[group_thread.x][group_thread.y] = dm3;
	}

	memoryBarrierShared();
	barrier();

	// MIP 4
	if (all(equal(group_thread % 8, ivec2(0)))) {
		float in_tl = scratch_depths[group_thread.x + 0][group_thread.y + 0];
		float in_tr = scratch_depths[group_thread.x + 4][group_thread.y + 0];
		float in_bl = scratch_depths[group_thread.x + 0][group_thread.y + 4];
		float in_br = scratch_depths[group_thread.x + 4][group_thread.y + 4];

		float dm4 = depth_mip_filter(in_tl, in_tr, in_bl, in_br);
		store_mip(4, base_coord / 8, dm4);
	}
}
