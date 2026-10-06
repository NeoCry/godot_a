/**************************************************************************/
/*  taa_sharpen.glsl                                                      */
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

#[compute]

#version 450

#VERSION_DEFINES

// Sharpens the TAA resolve with AMD's Robust Contrast Adaptive Sharpening (RCAS), from
// FidelityFX FSR 1. It writes the colour buffer only: the history keeps the unsharpened
// resolve, since sharpening fed back into the accumulation would compound every frame.

#define A_GPU
#define A_GLSL

#include "thirdparty/amd-fsr/ffx_a.h"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(rgba16f, set = 0, binding = 0) uniform restrict readonly image2D source_image;
layout(rgba16f, set = 0, binding = 1) uniform restrict writeonly image2D dest_image;

layout(push_constant, std430) uniform Params {
	ivec2 resolution;
	float sharpness; // In stops below full strength, as FsrRcasCon() takes it: 0 is the strongest.
	float pad;
}
params;

#define FSR_RCAS_F
// The resolve still carries some of the noise that screen-space effects leave for TAA to
// average out; this keeps RCAS from picking it out again, as FSR 2 does with the same pass.
#define FSR_RCAS_DENOISE

AF4 FsrRcasLoadF(ASU2 p) {
	return imageLoad(source_image, clamp(p, ASU2(0), params.resolution - ASU2(1)));
}

// RCAS bounds its lobe so that the result stays within [0, 1], which the HDR resolve is not.
// This is the forward half of FSR's simple reversible tonemapper (FsrSrtmF(), which is only
// declared after this callback is needed); main() applies the inverse to the result.
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {
	AF1 scale = ARcpF1(AMax3F1(r, g, b) + AF1_(1.0));
	r *= scale;
	g *= scale;
	b *= scale;
}

#include "thirdparty/amd-fsr/ffx_fsr1.h"

void main() {
	if (any(greaterThanEqual(ivec2(gl_GlobalInvocationID.xy), params.resolution))) {
		return;
	}

	AU4 con;
	FsrRcasCon(con, params.sharpness);

	AF3 color;
	FsrRcasF(color.r, color.g, color.b, gl_GlobalInvocationID.xy, con);
	FsrSrtmInvF(color);

	// Alpha is forced to one, as the plain copy this pass replaces does.
	imageStore(dest_image, ivec2(gl_GlobalInvocationID.xy), AF4(color, AF1_(1.0)));
}
