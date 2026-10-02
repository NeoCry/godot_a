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

// XeGTAO pass 3 (XeGTAO_Denoise): edge-aware 3x3 spatial filter, run once per denoise level. Each thread
// filters two horizontally adjacent pixels, sharing the gathered quads between them. The last pass also
// undoes XEGTAO_OCCLUSION_TERM_SCALE, so its output holds plain [0, 1] visibility.

#[compute]

#version 450

#VERSION_DEFINES

#include "xegtao_inc.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform usampler2D source_ao_term;
layout(set = 0, binding = 1) uniform sampler2D source_edges;

layout(r32ui, set = 1, binding = 0) uniform restrict writeonly uimage2D dest_ao_term;

layout(push_constant, std430) uniform Params {
	ivec2 viewport_size;
	vec2 viewport_pixel_size;

	float blur_beta;
	bool final_apply;
	float pad[2];
}
params;

#ifdef USE_BENT_NORMALS
#define AOTermType vec4 // .xyz is the bent normal offset, .w the visibility term.
#else
#define AOTermType float // The visibility term.
#endif

vec4 unpack_edges(float p_packed_value) {
	uint packed_value = uint(p_packed_value * 255.5);
	vec4 edges_lrtb;
	edges_lrtb.x = float((packed_value >> 6) & 0x03) / 3.0;
	edges_lrtb.y = float((packed_value >> 4) & 0x03) / 3.0;
	edges_lrtb.z = float((packed_value >> 2) & 0x03) / 3.0;
	edges_lrtb.w = float((packed_value >> 0) & 0x03) / 3.0;
	return clamp(edges_lrtb, 0.0, 1.0);
}

AOTermType decode_term(uint p_packed_value) {
#ifdef USE_BENT_NORMALS
	return xegtao_decode_term(p_packed_value);
#else
	return unpackUnorm4x8(p_packed_value).w;
#endif
}

void add_sample(AOTermType p_ssao_value, float p_edge_value, inout AOTermType r_sum, inout float r_sum_weight) {
	float weight = p_edge_value;
	r_sum += weight * p_ssao_value;
	r_sum_weight += weight;
}

void output_term(ivec2 p_pix_coord, AOTermType p_value) {
	if (p_pix_coord.x >= params.viewport_size.x) {
		return;
	}

	float scale = params.final_apply ? XEGTAO_OCCLUSION_TERM_SCALE : 1.0;
#ifdef USE_BENT_NORMALS
	uint packed_value = xegtao_encode_term(clamp(p_value.w * scale, 0.0, 1.0), p_value.xyz);
#else
	uint packed_value = xegtao_encode_term(clamp(p_value * scale, 0.0, 1.0), vec3(0.0));
#endif
	imageStore(dest_ao_term, p_pix_coord, uvec4(packed_value));
}

void main() {
	// We're computing 2 horizontal pixels at a time (performance optimization).
	ivec2 pix_coord_base = ivec2(gl_GlobalInvocationID.xy) * ivec2(2, 1);
	if (any(greaterThanEqual(pix_coord_base, params.viewport_size))) {
		return;
	}

	float blur_amount = params.final_apply ? params.blur_beta : params.blur_beta / 5.0;
	const float diag_weight = 0.85 * 0.5;

	vec4 edges_c_lrtb[2];
	float weight_tl[2];
	float weight_tr[2];
	float weight_bl[2];
	float weight_br[2];

	// Gather edge and visibility quads, used later. A gather at the top-left corner of a pixel returns, in
	// order, its left neighbor, itself, its top neighbor and its top-left neighbor.
	vec2 gather_center = vec2(pix_coord_base) * params.viewport_pixel_size;
	vec4 edges_q0 = textureGather(source_edges, gather_center, 0);
	vec4 edges_q1 = textureGatherOffset(source_edges, gather_center, ivec2(2, 0), 0);
	vec4 edges_q2 = textureGatherOffset(source_edges, gather_center, ivec2(1, 2), 0);

	uvec4 packed_q0 = textureGather(source_ao_term, gather_center, 0);
	uvec4 packed_q1 = textureGatherOffset(source_ao_term, gather_center, ivec2(2, 0), 0);
	uvec4 packed_q2 = textureGatherOffset(source_ao_term, gather_center, ivec2(0, 2), 0);
	uvec4 packed_q3 = textureGatherOffset(source_ao_term, gather_center, ivec2(2, 2), 0);

	AOTermType vis_q0[4] = AOTermType[4](decode_term(packed_q0.x), decode_term(packed_q0.y), decode_term(packed_q0.z), decode_term(packed_q0.w));
	AOTermType vis_q1[4] = AOTermType[4](decode_term(packed_q1.x), decode_term(packed_q1.y), decode_term(packed_q1.z), decode_term(packed_q1.w));
	AOTermType vis_q2[4] = AOTermType[4](decode_term(packed_q2.x), decode_term(packed_q2.y), decode_term(packed_q2.z), decode_term(packed_q2.w));
	AOTermType vis_q3[4] = AOTermType[4](decode_term(packed_q3.x), decode_term(packed_q3.y), decode_term(packed_q3.z), decode_term(packed_q3.w));

	for (int side = 0; side < 2; side++) {
		ivec2 pix_coord = ivec2(pix_coord_base.x + side, pix_coord_base.y);

		vec4 edges_l_lrtb = unpack_edges((side == 0) ? edges_q0.x : edges_q0.y);
		vec4 edges_t_lrtb = unpack_edges((side == 0) ? edges_q0.z : edges_q1.w);
		vec4 edges_r_lrtb = unpack_edges((side == 0) ? edges_q1.x : edges_q1.y);
		vec4 edges_b_lrtb = unpack_edges((side == 0) ? edges_q2.w : edges_q2.z);

		edges_c_lrtb[side] = unpack_edges((side == 0) ? edges_q0.y : edges_q1.x);

		// Edges aren't perfectly symmetrical: edge detection algorithm does not guarantee that a left edge on the
		// right pixel will match the right edge on the left pixel (although they will match in majority of cases).
		// This line further enforces the symmetricity, creating a slightly sharper blur. Works real nice with TAA.
		edges_c_lrtb[side] *= vec4(edges_l_lrtb.y, edges_r_lrtb.x, edges_t_lrtb.w, edges_b_lrtb.z);

		// This allows some small amount of AO leaking from neighbors if there are 3 or 4 edges; this reduces both
		// spatial and temporal aliasing.
		const float leak_threshold = 2.5;
		const float leak_strength = 0.5;
		float edginess = (clamp(4.0 - leak_threshold - dot(edges_c_lrtb[side], vec4(1.0)), 0.0, 1.0) / (4.0 - leak_threshold)) * leak_strength;
		edges_c_lrtb[side] = clamp(edges_c_lrtb[side] + edginess, 0.0, 1.0);

		// For diagonals; used by first and second pass.
		weight_tl[side] = diag_weight * (edges_c_lrtb[side].x * edges_l_lrtb.z + edges_c_lrtb[side].z * edges_t_lrtb.x);
		weight_tr[side] = diag_weight * (edges_c_lrtb[side].z * edges_t_lrtb.y + edges_c_lrtb[side].y * edges_r_lrtb.z);
		weight_bl[side] = diag_weight * (edges_c_lrtb[side].w * edges_b_lrtb.x + edges_c_lrtb[side].x * edges_l_lrtb.w);
		weight_br[side] = diag_weight * (edges_c_lrtb[side].y * edges_r_lrtb.w + edges_c_lrtb[side].w * edges_b_lrtb.y);

		// First pass.
		AOTermType ssao_value = (side == 0) ? vis_q0[1] : vis_q1[0];
		AOTermType ssao_value_l = (side == 0) ? vis_q0[0] : vis_q0[1];
		AOTermType ssao_value_t = (side == 0) ? vis_q0[2] : vis_q1[3];
		AOTermType ssao_value_r = (side == 0) ? vis_q1[0] : vis_q1[1];
		AOTermType ssao_value_b = (side == 0) ? vis_q2[2] : vis_q3[3];
		AOTermType ssao_value_tl = (side == 0) ? vis_q0[3] : vis_q0[2];
		AOTermType ssao_value_br = (side == 0) ? vis_q3[3] : vis_q3[2];
		AOTermType ssao_value_tr = (side == 0) ? vis_q1[3] : vis_q1[2];
		AOTermType ssao_value_bl = (side == 0) ? vis_q2[3] : vis_q2[2];

		float sum_weight = blur_amount;
		AOTermType sum = ssao_value * sum_weight;

		add_sample(ssao_value_l, edges_c_lrtb[side].x, sum, sum_weight);
		add_sample(ssao_value_r, edges_c_lrtb[side].y, sum, sum_weight);
		add_sample(ssao_value_t, edges_c_lrtb[side].z, sum, sum_weight);
		add_sample(ssao_value_b, edges_c_lrtb[side].w, sum, sum_weight);

		add_sample(ssao_value_tl, weight_tl[side], sum, sum_weight);
		add_sample(ssao_value_tr, weight_tr[side], sum, sum_weight);
		add_sample(ssao_value_bl, weight_bl[side], sum, sum_weight);
		add_sample(ssao_value_br, weight_br[side], sum, sum_weight);

		output_term(pix_coord, sum / sum_weight);
	}
}
