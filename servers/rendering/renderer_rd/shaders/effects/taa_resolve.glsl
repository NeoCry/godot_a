///////////////////////////////////////////////////////////////////////////////////
// Copyright(c) 2016-2022 Panos Karabelas
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
// copies of the Software, and to permit persons to whom the Software is furnished
// to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
// FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
// IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
///////////////////////////////////////////////////////////////////////////////////
// File changes (yyyy-mm-dd)
// 2026-09-21: Confidence-driven accumulation, tonemapped YCoCg history clamping and
//             thin-feature detection, replacing the fixed 1/16 blend and the two
//             luminance/velocity heuristics that surrounded it.
// 2025-11-05: Jakub Brzyski: Added dynamic variance, base variance value adjusted to reduce ghosting
// 2022-05-06: Panos Karabelas: first commit
// 2020-12-05: Joan Fons: convert to Vulkan and Godot
///////////////////////////////////////////////////////////////////////////////////

#[compute]

#version 450

#VERSION_DEFINES

// Based on Spartan Engine's TAA implementation (without TAA upscale).
// <https://github.com/PanosK92/SpartanEngine/blob/a8338d0609b85dc32f3732a5c27fb4463816a3b9/Data/shaders/temporal_antialiasing.hlsl>

#define GROUP_SIZE 8
#define FLT_MIN 0.00000001
#define FLT_MAX 32767.0
#define RPC_9 0.11111111111

layout(local_size_x = GROUP_SIZE, local_size_y = GROUP_SIZE, local_size_z = 1) in;

layout(rgba16f, set = 0, binding = 0) uniform restrict readonly image2D color_buffer;
layout(set = 0, binding = 1) uniform sampler2D depth_buffer;
layout(rg16f, set = 0, binding = 2) uniform restrict readonly image2D velocity_buffer;
layout(rg16f, set = 0, binding = 3) uniform restrict readonly image2D last_velocity_buffer;
layout(set = 0, binding = 4) uniform sampler2D history_buffer;
layout(rgba16f, set = 0, binding = 5) uniform restrict writeonly image2D output_buffer;

layout(push_constant, std430) uniform Params {
	vec2 resolution;
	float disocclusion_threshold; // Velocity change, in texels, that is still considered the same surface.
	float disocclusion_scale; // How quickly a larger velocity change converges to "this is a new surface".

	float clamp_scale; // Half-width of the history box along luma, in standard deviations.
	float clamp_scale_chroma; // Same along chroma, where ghosting is most visible.
	float motion_clamp_scale; // How far the box narrows at high velocities.
	float rejection_sensitivity; // How sharply clamped history loses its accumulated samples.

	float max_accumulated_frames; // Upper bound of the per-pixel sample counter.
	float pad0;
	float pad1;
	float pad2;
}
params;

const ivec2 kOffsets3x3[9] = {
	ivec2(-1, -1),
	ivec2(0, -1),
	ivec2(1, -1),
	ivec2(-1, 0),
	ivec2(0, 0),
	ivec2(1, 0),
	ivec2(-1, 1),
	ivec2(0, 1),
	ivec2(1, 1),
};

/*------------------------------------------------------------------------------
						THREAD GROUP SHARED MEMORY (LDS)
------------------------------------------------------------------------------*/

const int kBorderSize = 1;
const int kGroupSize = GROUP_SIZE;
const int kTileDimension = kGroupSize + kBorderSize * 2;
const int kTileDimension2 = kTileDimension * kTileDimension;

const vec3 lumCoeff = vec3(0.299f, 0.587f, 0.114f);

float luminance(vec3 color) {
	return max(dot(color, lumCoeff), 0.0001f);
}

// Luminance-weighted tonemap (Karis). Blending here rather than in linear HDR is what
// keeps a bright sub-pixel sample from dragging the average around: it is an exact
// inverse pair, because luminance is linear, so 1 - luminance(tonemap(c)) == 1/(1 + luminance(c)).
vec3 tonemap(vec3 hdr) {
	return hdr / (1.0f + luminance(hdr));
}

vec3 tonemap_inverse(vec3 sdr) {
	return sdr / max(1.0f - luminance(sdr), 0.0001f);
}

// YCoCg separates luma from chroma, so the history box can be wide along luma - where
// thin geometry lives - while staying tight along chroma, where ghosting shows up.
vec3 rgb_to_ycocg(vec3 c) {
	return vec3(
			0.25f * c.r + 0.5f * c.g + 0.25f * c.b,
			0.5f * c.r - 0.5f * c.b,
			-0.25f * c.r + 0.5f * c.g - 0.25f * c.b);
}

vec3 ycocg_to_rgb(vec3 c) {
	float t = c.x - c.z;
	return vec3(t + c.y, c.x + c.z, t - c.y);
}

vec3 to_working_space(vec3 hdr) {
	return rgb_to_ycocg(tonemap(hdr));
}

vec3 from_working_space(vec3 c) {
	return tonemap_inverse(ycocg_to_rgb(c));
}

float get_depth(ivec2 thread_id) {
	return texelFetch(depth_buffer, thread_id, 0).r;
}

shared vec3 tile_color[kTileDimension][kTileDimension];
shared float tile_depth[kTileDimension][kTileDimension];

vec3 load_color(uvec2 group_thread_id) {
	group_thread_id += kBorderSize;
	return tile_color[group_thread_id.x][group_thread_id.y];
}

void store_color(uvec2 group_thread_id, vec3 color) {
	tile_color[group_thread_id.x][group_thread_id.y] = color;
}

float load_depth(uvec2 group_thread_id) {
	group_thread_id += kBorderSize;
	return tile_depth[group_thread_id.x][group_thread_id.y];
}

void store_depth(uvec2 group_thread_id, float depth) {
	tile_depth[group_thread_id.x][group_thread_id.y] = depth;
}

void store_color_depth(uvec2 group_thread_id, ivec2 thread_id) {
	// out of bounds clamp
	thread_id = clamp(thread_id, ivec2(0, 0), ivec2(params.resolution) - ivec2(1, 1));

	// Converted once on the way in: the statistics, the clamp and the blend all run in
	// tonemapped YCoCg, so the tile never holds linear HDR.
	store_color(group_thread_id, to_working_space(max(imageLoad(color_buffer, thread_id).rgb, vec3(0.0f))));
	store_depth(group_thread_id, get_depth(thread_id));
}

void populate_group_shared_memory(uvec2 group_id, uint group_index) {
	// Populate group shared memory
	ivec2 group_top_left = ivec2(group_id) * kGroupSize - kBorderSize;
	if (group_index < (kTileDimension2 >> 2)) {
		ivec2 group_thread_id_1 = ivec2(group_index % kTileDimension, group_index / kTileDimension);
		ivec2 group_thread_id_2 = ivec2((group_index + (kTileDimension2 >> 2)) % kTileDimension, (group_index + (kTileDimension2 >> 2)) / kTileDimension);
		ivec2 group_thread_id_3 = ivec2((group_index + (kTileDimension2 >> 1)) % kTileDimension, (group_index + (kTileDimension2 >> 1)) / kTileDimension);
		ivec2 group_thread_id_4 = ivec2((group_index + kTileDimension2 * 3 / 4) % kTileDimension, (group_index + kTileDimension2 * 3 / 4) / kTileDimension);

		store_color_depth(group_thread_id_1, group_top_left + group_thread_id_1);
		store_color_depth(group_thread_id_2, group_top_left + group_thread_id_2);
		store_color_depth(group_thread_id_3, group_top_left + group_thread_id_3);
		store_color_depth(group_thread_id_4, group_top_left + group_thread_id_4);
	}

	// Wait for group threads to load store data.
	groupMemoryBarrier();
	barrier();
}

/*------------------------------------------------------------------------------
								VELOCITY
------------------------------------------------------------------------------*/

void depth_test_min(uvec2 pos, inout float min_depth, inout uvec2 min_pos) {
	float depth = load_depth(pos);

	if (depth < min_depth) {
		min_depth = depth;
		min_pos = pos;
	}
}

// Returns velocity with closest depth (3x3 neighborhood)
void get_closest_pixel_velocity_3x3(in uvec2 group_pos, uvec2 group_top_left, out vec2 velocity) {
	float min_depth = 1.0;
	uvec2 min_pos = group_pos;

	depth_test_min(group_pos + kOffsets3x3[0], min_depth, min_pos);
	depth_test_min(group_pos + kOffsets3x3[1], min_depth, min_pos);
	depth_test_min(group_pos + kOffsets3x3[2], min_depth, min_pos);
	depth_test_min(group_pos + kOffsets3x3[3], min_depth, min_pos);
	depth_test_min(group_pos + kOffsets3x3[4], min_depth, min_pos);
	depth_test_min(group_pos + kOffsets3x3[5], min_depth, min_pos);
	depth_test_min(group_pos + kOffsets3x3[6], min_depth, min_pos);
	depth_test_min(group_pos + kOffsets3x3[7], min_depth, min_pos);
	depth_test_min(group_pos + kOffsets3x3[8], min_depth, min_pos);

	// Velocity out
	velocity = imageLoad(velocity_buffer, ivec2(group_top_left + min_pos)).xy;
}

/*------------------------------------------------------------------------------
							  HISTORY SAMPLING
------------------------------------------------------------------------------*/

vec3 sample_catmull_rom_9(sampler2D stex, vec2 uv, vec2 resolution) {
	// Source: https://gist.github.com/TheRealMJP/c83b8c0f46b63f3a88a5986f4fa982b1
	// License: https://gist.github.com/TheRealMJP/bc503b0b87b643d3505d41eab8b332ae

	// We're going to sample a 4x4 grid of texels surrounding the target UV coordinate. We'll do this by rounding
	// down the sample location to get the exact center of our "starting" texel. The starting texel will be at
	// location [1, 1] in the grid, where [0, 0] is the top left corner.
	vec2 sample_pos = uv * resolution;
	vec2 texPos1 = floor(sample_pos - 0.5f) + 0.5f;

	// Compute the fractional offset from our starting texel to our original sample location, which we'll
	// feed into the Catmull-Rom spline function to get our filter weights.
	vec2 f = sample_pos - texPos1;

	// Compute the Catmull-Rom weights using the fractional offset that we calculated earlier.
	// These equations are pre-expanded based on our knowledge of where the texels will be located,
	// which lets us avoid having to evaluate a piece-wise function.
	vec2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
	vec2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
	vec2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
	vec2 w3 = f * f * (-0.5f + 0.5f * f);

	// Work out weighting factors and sampling offsets that will let us use bilinear filtering to
	// simultaneously evaluate the middle 2 samples from the 4x4 grid.
	vec2 w12 = w1 + w2;
	vec2 offset12 = w2 / (w1 + w2);

	// Compute the final UV coordinates we'll use for sampling the texture
	vec2 texPos0 = texPos1 - 1.0f;
	vec2 texPos3 = texPos1 + 2.0f;
	vec2 texPos12 = texPos1 + offset12;

	texPos0 /= resolution;
	texPos3 /= resolution;
	texPos12 /= resolution;

	vec3 result = vec3(0.0f, 0.0f, 0.0f);

	result += textureLod(stex, vec2(texPos0.x, texPos0.y), 0.0).xyz * w0.x * w0.y;
	result += textureLod(stex, vec2(texPos12.x, texPos0.y), 0.0).xyz * w12.x * w0.y;
	result += textureLod(stex, vec2(texPos3.x, texPos0.y), 0.0).xyz * w3.x * w0.y;

	result += textureLod(stex, vec2(texPos0.x, texPos12.y), 0.0).xyz * w0.x * w12.y;
	result += textureLod(stex, vec2(texPos12.x, texPos12.y), 0.0).xyz * w12.x * w12.y;
	result += textureLod(stex, vec2(texPos3.x, texPos12.y), 0.0).xyz * w3.x * w12.y;

	result += textureLod(stex, vec2(texPos0.x, texPos3.y), 0.0).xyz * w0.x * w3.y;
	result += textureLod(stex, vec2(texPos12.x, texPos3.y), 0.0).xyz * w12.x * w3.y;
	result += textureLod(stex, vec2(texPos3.x, texPos3.y), 0.0).xyz * w3.x * w3.y;

	return max(result, 0.0f);
}

/*------------------------------------------------------------------------------
							  HISTORY CLIPPING
------------------------------------------------------------------------------*/

// Based on "Temporal Reprojection Anti-Aliasing" - https://github.com/playdeadgames/temporal
//
// Centre/extent form rather than the per-axis clip-and-rescale one. The latter divides by a
// component of the offset that an earlier axis may already have driven to zero, and a flat
// region of the frame - a clear sky, an unlit wall - makes the box degenerate on every axis at
// once, so that division is 0/0 and the whole resolve turns to NaN. Here the only division is
// guarded, and a zero extent simply pulls the history all the way onto the neighbourhood.
vec3 clip_aabb(vec3 aabb_min, vec3 aabb_max, vec3 q) {
	vec3 center = 0.5f * (aabb_max + aabb_min);
	vec3 extents = 0.5f * (aabb_max - aabb_min);
	vec3 offset = q - center;

	vec3 units = extents / max(abs(offset), vec3(FLT_MIN));
	float t = min(units.x, min(units.y, units.z));

	return center + offset * clamp(t, 0.0f, 1.0f);
}

struct Neighbourhood {
	vec3 mean;
	vec3 deviation;
};

// Statistics of the 3x3 neighbourhood, in tonemapped YCoCg.
Neighbourhood gather_neighbourhood_3x3(uvec2 group_pos) {
	vec3 sum = vec3(0.0f);
	vec3 sum_squared = vec3(0.0f);

	for (int i = 0; i < 9; i++) {
		vec3 s = load_color(group_pos + kOffsets3x3[i]);
		sum += s;
		sum_squared += s * s;
	}

	Neighbourhood n;
	n.mean = sum * RPC_9;
	n.deviation = sqrt(max(sum_squared * RPC_9 - n.mean * n.mean, vec3(0.0f)));

	return n;
}

// Clip history to the neighbourhood of the current sample.
vec3 clip_history_3x3(Neighbourhood n, vec3 color_history, float velocity_texels) {
	// Reprojection error grows with motion, so the box narrows - but never to zero. Collapsing
	// it, as a velocity-gated box does, forces the history onto the neighbourhood mean, which
	// is nothing but a 3x3 blur of the current frame.
	float motion = mix(1.0f, params.motion_clamp_scale, clamp(velocity_texels * 0.0625f, 0.0f, 1.0f));

	vec3 gamma = params.clamp_scale * motion * vec3(1.0f, params.clamp_scale_chroma, params.clamp_scale_chroma);

	// Deliberately NOT intersected with the neighbourhood's own min/max. The frame where the
	// jitter misses a thin feature is the frame where no sample around it carries that feature,
	// so clipping to the samples present would discard precisely the history worth keeping -
	// and would make the widening above a no-op. The box is bounded in tonemapped luma instead,
	// which is all the inverse tonemap needs to stay well conditioned.
	vec3 color_min = n.mean - gamma * n.deviation;
	vec3 color_max = n.mean + gamma * n.deviation;
	color_min.x = max(color_min.x, 0.0f);
	color_max.x = min(color_max.x, 0.999f);

	return clip_aabb(color_min, color_max, color_history);
}

/*------------------------------------------------------------------------------
									TAA
------------------------------------------------------------------------------*/

// This is "velocity disocclusion" as described by https://www.elopezr.com/temporal-aa-and-the-quest-for-the-holy-trail/.
// We use texel space, so our scale and threshold differ.
float get_factor_disocclusion(vec2 uv_reprojected, vec2 velocity) {
	ivec2 pos_previous = clamp(ivec2(uv_reprojected * params.resolution), ivec2(0, 0), ivec2(params.resolution) - ivec2(1, 1));
	vec2 velocity_previous = imageLoad(last_velocity_buffer, pos_previous).xy;
	vec2 velocity_texels = velocity * params.resolution;
	vec2 prev_velocity_texels = velocity_previous * params.resolution;
	float disocclusion = length(prev_velocity_texels - velocity_texels) - params.disocclusion_threshold;
	return clamp(disocclusion * params.disocclusion_scale, 0.0, 1.0);
}

vec3 temporal_antialiasing(uvec2 pos_group_top_left, uvec2 pos_group, uvec2 pos_screen, vec2 uv, sampler2D tex_history, out float accumulated_frames) {
	// Get the velocity of the current pixel
	vec2 velocity = imageLoad(velocity_buffer, ivec2(pos_screen)).xy;

	// Get reprojected uv
	vec2 uv_reprojected = uv + velocity;

	// Get input color
	vec3 color_input = load_color(pos_group);

	// Velocity of the closest depth in the neighbourhood. It measures how trustworthy the
	// reprojection is around here; the reprojection itself stays on this texel's own velocity,
	// which is the more accurate one for a thin feature.
	vec2 velocity_closest = vec2(0.0);
	get_closest_pixel_velocity_3x3(pos_group, pos_group_top_left, velocity_closest);

	Neighbourhood neighbourhood = gather_neighbourhood_3x3(pos_group);

	// Get history color (catmull-rom reduces a lot of the blurring that you get under motion).
	// Sanitised on the way in: the history feeds itself, so a single non-finite sample - from the
	// scene buffer, or from a Catmull-Rom overshoot - would otherwise persist indefinitely.
	vec3 history_linear = sample_catmull_rom_9(tex_history, uv_reprojected, params.resolution);
	if (any(isnan(history_linear)) || any(isinf(history_linear))) {
		history_linear = vec3(0.0f);
	}
	vec3 color_history = to_working_space(history_linear);

	float velocity_texels = length(velocity_closest * params.resolution);
	vec3 color_clipped = clip_history_3x3(neighbourhood, color_history, velocity_texels);

	// How far the clamp had to move the history, in local standard deviations. Near zero means
	// the history agreed with this frame; large means it describes something no longer here.
	// This is the measurement that separates "history is wrong" from "signal is high frequency":
	// a grass blade is high variance but its history still lands inside the box.
	// The floor matters: on a near-flat neighbourhood the deviation approaches zero, and without
	// it any trivial disagreement would divide out to a huge rejection and throw away history
	// that was perfectly good. 0.01 is one percent of the tonemapped range.
	float rejection = length(color_clipped - color_history) / max(length(neighbourhood.deviation), 0.01f);

	// Rejection alone must not stop the accumulation, or thin geometry never resolves: a grass
	// blade the jitter catches one frame and misses the next disagrees with its own history
	// every frame, which is exactly the signal that needs averaging rather than discarding. So
	// the loss of trust is gated on evidence that the history may describe different geometry -
	// screen motion, or a velocity discontinuity across the reprojection. On a still pixel there
	// is no such evidence, and the clamp above has already bounded how far the history can be
	// off, so continuing to accumulate cannot drift.
	float reprojection_risk = max(clamp(velocity_texels * 0.125f, 0.0f, 1.0f),
			get_factor_disocclusion(uv_reprojected, velocity));

	float trust = 1.0f - reprojection_risk * (1.0f - exp2(-params.rejection_sensitivity * rejection));

	// If the re-projected UV is off screen there is no history to trust at all.
	if (any(lessThan(uv_reprojected, vec2(0.0f))) || any(greaterThan(uv_reprojected, vec2(1.0f)))) {
		trust = 0.0f;
	}

	// The sample counter rides in the history's alpha. It grows by one per frame where the
	// history held up and falls back towards one where it did not, so blend = 1/n converges in
	// a single frame after a disocclusion yet keeps averaging far longer than a fixed 1/16 once
	// a pixel is stable - which is what sub-pixel geometry needs to resolve at all.
	float frames_previous = textureLod(tex_history, uv_reprojected, 0.0f).a * params.max_accumulated_frames;
	accumulated_frames = clamp(1.0f + frames_previous * trust, 1.0f, params.max_accumulated_frames);
	float blend_factor = 1.0f / accumulated_frames;

	return from_working_space(mix(color_clipped, color_input, blend_factor));
}

void main() {
	populate_group_shared_memory(gl_WorkGroupID.xy, gl_LocalInvocationIndex);

	// Out of bounds check
	if (any(greaterThanEqual(vec2(gl_GlobalInvocationID.xy), params.resolution))) {
		return;
	}

	const uvec2 pos_group = gl_LocalInvocationID.xy;
	const uvec2 pos_group_top_left = gl_WorkGroupID.xy * kGroupSize - kBorderSize;
	const uvec2 pos_screen = gl_GlobalInvocationID.xy;
	const vec2 uv = (gl_GlobalInvocationID.xy + 0.5f) / params.resolution;

	float accumulated_frames = 1.0f;
	vec3 result = temporal_antialiasing(pos_group_top_left, pos_group, pos_screen, uv, history_buffer, accumulated_frames);

	// Clamp to prevent NaNs
	result = clamp(result, vec3(0.0f), vec3(FLT_MAX));

	// Alpha carries the sample counter into the next frame: TAA::process() copies this texture
	// to the history buffer as-is, and to the colour buffer with alpha forced to one.
	imageStore(output_buffer, ivec2(gl_GlobalInvocationID.xy), vec4(result, accumulated_frames / params.max_accumulated_frames));
}
