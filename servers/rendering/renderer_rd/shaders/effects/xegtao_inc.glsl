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

// Shared helpers for the XeGTAO passes (servers/rendering/renderer_rd/effects/xegtao.cpp).
//
// XeGTAO works in its own view space convention: x right, y up, z = positive distance in front of the
// camera. Godot's view space looks down -z instead, so normals coming in from the normal-roughness buffer
// and bent normals going back out to the lighting pass both flip z at the boundary.

#define XEGTAO_PI 3.1415926535897932384626433832795
#define XEGTAO_PI_HALF 1.5707963267948966192313216916398

// The depth mip chain is hard-coded to 5 levels, as in XeGTAO.
#define XEGTAO_DEPTH_MIP_LEVELS 5

// The raw, pre-denoise visibility of a single pixel can overshoot 1 (it only averages out to 1 over a
// neighborhood), so it is stored scaled down by this much until the last denoise pass scales it back.
#define XEGTAO_OCCLUSION_TERM_SCALE 1.5

// The bent normal travels through the denoiser as its offset from the shading normal (bent - normal)
// rather than as a direction. The denoiser blurs that offset like any other quantity, and the apply pass
// adds it back onto each pixel's own (full resolution, unblurred) normal, so the bending stays smooth
// while normal-mapped detail survives. Bent normals never leave the visible hemisphere, so each
// component of the offset stays within +/-sqrt(2); this maps +/-1.43 onto the 8-bit range.
#define XEGTAO_BENT_OFFSET_SCALE 0.35

// The working AO term: bent normal offset in xyz, visibility in w, packed as RGBA8 into one uint so the
// denoiser can fetch a whole 2x2 quad of it with a single gather.
uint xegtao_encode_term(float p_visibility, vec3 p_bent_offset) {
	return packUnorm4x8(vec4(p_bent_offset * XEGTAO_BENT_OFFSET_SCALE + 0.5, p_visibility));
}

vec4 xegtao_decode_term(uint p_packed) {
	vec4 decoded = unpackUnorm4x8(p_packed);
	return vec4((decoded.xyz - 0.5) * (1.0 / XEGTAO_BENT_OFFSET_SCALE), decoded.w);
}

// Converts a hardware (reverse-Z, [0, 1]) depth value into positive linear view space distance.
float xegtao_linearize_depth(float p_depth, bool p_orthogonal, float p_linearize_mul, float p_linearize_add, float p_z_near, float p_z_far) {
	if (p_orthogonal) {
		// Reverse Z: 1.0 is the near plane, 0.0 the far one.
		return mix(p_z_far, p_z_near, p_depth);
	}
	// Optimized version of "-cameraClipNear / (cameraClipFar - projDepth * (cameraClipFar - cameraClipNear)) * cameraClipFar".
	return p_linearize_mul / (p_linearize_add - p_depth);
}

vec3 xegtao_load_normal(vec4 p_normal_roughness) {
	vec3 normal = normalize(p_normal_roughness.xyz * 2.0 - 1.0);
	normal.z = -normal.z;
	return normal;
}
