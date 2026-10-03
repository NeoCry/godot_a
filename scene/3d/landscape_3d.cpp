/**************************************************************************/
/*  landscape_3d.cpp                                                      */
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

#include "landscape_3d.h"

#include "core/config/engine.h"
#include "core/core_string_names.h"
#include "core/io/image.h"
#include "core/math/math_funcs_binary.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/foliage_gpu_culler.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/3d/physics/static_body_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/height_map_shape_3d.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "scene/resources/shader.h"
#include "servers/rendering/rendering_server.h"

namespace {
// How much of a brush stamp lands at p_dist from its center: 1 at the center,
// tapering to 0 at the rim. p_falloff is how much of the radius that taper
// takes up - 0 stamps at full strength right up to the rim and stops dead
// (a hard edge), 1 tapers across the whole radius, and anything between keeps
// an inner core at full strength and fades only the outside of it.
float brush_falloff_weight(float p_dist, float p_radius, float p_falloff) {
	const float falloff = CLAMP(p_falloff, 0.0f, 1.0f);
	if (falloff <= 0.0f) {
		return 1.0f;
	}
	const float inner = p_radius * (1.0f - falloff);
	if (p_dist <= inner) {
		return 1.0f;
	}
	const float t = 1.0f - (p_dist - inner) / MAX(p_radius - inner, 0.00001f);
	return t * t * (3.0f - 2.0f * t);
}

// Sets p_target_layer's weight at sample p_index (an index into every array of
// r_layer_weights, one array per layer) to p_new_weight, and when that raises
// it, takes the raised amount out of the other layers, proportionally to their
// current share of "the rest", so the total weight stays roughly constant
// instead of every layer just growing without bound - the same weight-blended-
// layer behavior CryEngine/UE4/5 terrain painting uses. Without this, painting
// a second layer over a first one already at full weight could only ever reach
// an even split between them, never fully replace it.
void set_layer_weight_renormalized(Vector<PackedFloat32Array> &r_layer_weights, int p_target_layer, int p_index, float p_new_weight) {
	PackedFloat32Array &target = r_layer_weights.write[p_target_layer];
	const float new_target = CLAMP(p_new_weight, 0.0f, 1.0f);
	const float delta = new_target - target[p_index];

	if (delta > 0.00001f) {
		const int layer_count = r_layer_weights.size();
		float others_sum = 0.0f;
		for (int i = 0; i < layer_count; i++) {
			if (i != p_target_layer) {
				others_sum += r_layer_weights[i][p_index];
			}
		}
		if (others_sum > 0.00001f) {
			const float scale = MAX(0.0f, (others_sum - delta) / others_sum);
			for (int i = 0; i < layer_count; i++) {
				if (i != p_target_layer) {
					PackedFloat32Array &other = r_layer_weights.write[i];
					other.set(p_index, other[p_index] * scale);
				}
			}
		}
	}
	target.set(p_index, new_target);
}

// Used only when a layer has no texture of its own to say how big its stand-in
// should be, and as a floor under the size the real ones settle on.
constexpr int MIN_LAYER_TEXTURE_SIZE = 4;

// A set of layer images can be handed to the GPU exactly as it is - nothing
// decompressed, resized or converted - when every layer supplies one and they
// already agree on size, format and having mipmaps. This is the only way a set
// of 4K textures is affordable, because it keeps whatever VRAM compression they
// were imported with: expanding a 4096x4096 texture to RGBA8 costs about 67 MB
// per layer per array before mipmaps, against roughly 11-22 MB compressed.
bool can_upload_layer_images_directly(const Vector<Ref<Image>> &p_images) {
	if (p_images.is_empty() || p_images[0].is_null()) {
		return false;
	}
	const Ref<Image> &first = p_images[0];
	if (!first->is_compressed() || !first->has_mipmaps()) {
		return false;
	}
	for (int i = 1; i < p_images.size(); i++) {
		const Ref<Image> &img = p_images[i];
		if (img.is_null() || !img->has_mipmaps() ||
				img->get_format() != first->get_format() ||
				img->get_width() != first->get_width() ||
				img->get_height() != first->get_height()) {
			return false;
		}
	}
	return true;
}

// What size a set is resampled to when it cannot go up as it is: the largest
// source in it, so a high-resolution texture is not thrown away just because
// another layer's is smaller, but never past the terrain's own limit.
int common_layer_texture_size(const Vector<Ref<Image>> &p_images, int p_limit) {
	int size = 0;
	for (int i = 0; i < p_images.size(); i++) {
		if (p_images[i].is_valid()) {
			size = MAX(size, MAX(p_images[i]->get_width(), p_images[i]->get_height()));
		}
	}
	return CLAMP(size, MIN_LAYER_TEXTURE_SIZE, MAX(p_limit, MIN_LAYER_TEXTURE_SIZE));
}

// Stacks one texture per layer into the shared Texture2DArray the shader samples
// (GPU texture arrays require every layer to share one size and format), filling
// in p_default_color for any layer that has no texture of this kind.
Ref<Texture2DArray> build_layer_texture_array(const Vector<Ref<Image>> &p_sources, const Color &p_default_color, Image::Format p_format, bool p_renormalize_mipmaps, int p_size_limit) {
	Vector<Ref<Image>> images;
	if (can_upload_layer_images_directly(p_sources)) {
		images = p_sources;
	} else {
		images.resize(p_sources.size());
		const int size = common_layer_texture_size(p_sources, p_size_limit);
		for (int i = 0; i < p_sources.size(); i++) {
			Ref<Image> img;
			img.instantiate();
			if (p_sources[i].is_valid()) {
				img->copy_internals_from(p_sources[i]);
				if (img->is_compressed()) {
					img->decompress();
				}
				if (img->has_mipmaps()) {
					// The source's chain describes the source's size; a fresh
					// one is generated below once this is at its final size.
					img->clear_mipmaps();
				}
				if (img->get_format() != p_format) {
					img->convert(p_format);
				}
				if (img->get_width() != size || img->get_height() != size) {
					// Lanczos rather than resize()'s default bilinear: bilinear
					// reads four texels wherever it lands, so shrinking a large
					// texture by more than half throws away most of it and
					// keeps whichever few texels it happened to hit, which
					// looks like noise rather than a smaller version.
					img->resize(size, size, Image::INTERPOLATE_LANCZOS);
				}
			} else {
				img->initialize_data(size, size, false, p_format);
				img->fill(p_default_color);
			}
			// The shader samples these with filter_linear_mipmap_anisotropic,
			// so without a mip chain there is nothing for it to filter between:
			// every fragment reads full-resolution texels, and anywhere the
			// texture is minified (which on a terrain is most of the view) that
			// samples far below its own detail and aliases into a shimmering,
			// blocky mess.
			img->generate_mipmaps(p_renormalize_mipmaps);
			images.write[i] = img;
		}
	}

	Ref<Texture2DArray> array;
	array.instantiate();
	array->create_from_images(images);
	return array;
}

bool lod_params_equal(const LandscapeQuadtree::SelectParams &p_a, const LandscapeQuadtree::SelectParams &p_b) {
	if (p_a.camera_position != p_b.camera_position || p_a.orthogonal != p_b.orthogonal || p_a.pixel_scale != p_b.pixel_scale ||
			p_a.pixel_error != p_b.pixel_error || p_a.max_quad_pixels != p_b.max_quad_pixels || p_a.min_level != p_b.min_level ||
			p_a.micro_distance != p_b.micro_distance || p_a.micro_quad_pixels != p_b.micro_quad_pixels ||
			p_a.displacement_bound != p_b.displacement_bound || p_a.frustum_culling != p_b.frustum_culling ||
			p_a.shadows != p_b.shadows || p_a.shadow_distance != p_b.shadow_distance) {
		return false;
	}
	if (p_a.frustum_culling) {
		for (int i = 0; i < 6; i++) {
			if (p_a.frustum[i] != p_b.frustum[i]) {
				return false;
			}
		}
	}
	return true;
}
} // namespace

// TerrainData::MAX_LAYERS (see its declaration) must match the shader
// source's per-layer uniform array sizes: shader uniform arrays are
// fixed-size, and TerrainData packs the same number of layers' weights into
// its weight maps, so the two hard caps have to agree.

void Landscape3D::init_shaders() {
	shader.instantiate();
	shader->set_code(R"(
shader_type spatial;
render_mode blend_mix, depth_draw_opaque, cull_back, diffuse_burley, specular_schlick_ggx;

// LandscapeQuadtree::PATCH_QUADS and LEVEL_BIAS.
const int PATCH_QUADS = 16;
const int LEVEL_BIAS = 4;

// The heightmap, one texel per sample. Only ever read with texelFetch: every
// vertex from quadtree level 0 up sits exactly on a sample, and the micro
// levels below that interpolate between samples themselves.
uniform sampler2D heightmap : filter_nearest, repeat_disable;
// The heightmap's slope (dh/dx, dh/dz) at every sample, mipmapped. Shading
// takes its normal from here per pixel, so a distant patch drawn with a quad
// every few dozen samples still shows all the relief of the samples under it;
// and since the mipmaps of a slope are the slopes of the averaged heightmap,
// that relief fades out with distance instead of aliasing.
uniform sampler2D gradient_map : filter_linear_mipmap_anisotropic, repeat_disable;
uniform sampler2D hole_map : filter_nearest, repeat_disable;
// Quads along each side of the heightmap: its resolution minus one.
uniform int terrain_quads = 1;
uniform float vertex_spacing = 1.0;
// The point the patches were selected from, in terrain space. Not
// CAMERA_POSITION_WORLD: shadow passes see the light's viewpoint there, and
// have to place every vertex exactly where the camera's pass does.
uniform vec3 lod_camera_position = vec3(0.0);

// Micro detail: each layer's height_texture moves the surface up and down by
// up to half its TerrainLayer.displacement, fading out towards
// micro_detail_distance.
uniform bool displacement_enabled = false;
uniform float displacement_fade_start = 36.0;
uniform float displacement_fade_end = 48.0;
// How large a displacement texel may be at a given distance, in world units
// per unit of distance: about micro_detail_triangle_size pixels, which is
// what the quadtree splits quads down to there. Sampling finer detail than the
// vertices can follow would only alias.
uniform float displacement_footprint_scale = 0.01;
// Nor finer than the finest quads there are.
uniform float displacement_min_footprint = 0.0625;
uniform float height_texture_size = 1.0;
uniform float layer_displacement[32];

// One weight per layer, four layers packed per RGBA8 array layer (see
// TerrainData). Sampled with normal bilinear filtering - unlike an
// index-based control map, a weight is a continuous quantity, so
// interpolating it between samples is meaningful.
uniform sampler2DArray weight_array : filter_linear, repeat_disable;
uniform sampler2DArray albedo_array : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2DArray normal_array : hint_normal, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2DArray orm_array : filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2DArray height_array : hint_default_white, filter_linear_mipmap, repeat_enable;
uniform float layer_uv_scales[32];
// Per-layer scalar tweaks (see TerrainLayer): albedo_color multiplies the
// albedo texture, roughness multiplies the ORM texture's roughness channel,
// ao_strength fades the ORM texture's occlusion channel towards "no
// occlusion" (not towards black - see accumulate_layer()), normal_strength
// feeds Godot's own NORMAL_MAP_DEPTH, and specular is unrelated to any
// texture (it matches BaseMaterial3D.metallic_specular).
uniform vec4 layer_albedo_colors[32];
uniform float layer_roughness[32];
uniform float layer_specular[32];
uniform float layer_ao_strength[32];
uniform float layer_normal_strength[32];
uniform float layer_heightmap_scale[32];
uniform float layer_height_min[32];
uniform float layer_height_max[32];
// Whether each layer uses Parallax Occlusion Mapping / Triplanar Mapping
// (see TerrainLayer.pom_enabled/triplanar_enabled). pom_enabled below is a
// cheap master switch on top of layer_pom_enabled: both must be true.
// 0.0/1.0, not bool: Godot's Variant system has no packed bool array type
// to upload these as, so they're checked as > 0.5 instead.
uniform float layer_pom_enabled[32];
uniform float layer_triplanar[32];
uniform float layer_triplanar_sharpness[32];
uniform int layer_count = 0;

uniform bool pom_enabled = false;
uniform int pom_min_layers = 8;
uniform int pom_max_layers = 32;
// This shader builds its own tangents from the heightmap's slope, so whether
// either axis needs flipping to point the parallax offset the right way is
// left to these, the equivalent of BaseMaterial3D's
// heightmap_flip_tangent/flip_binormal.
uniform vec2 pom_flip = vec2(1.0, 1.0);
uniform bool pom_self_shadow_enabled = true;
uniform int pom_shadow_steps = 8;
uniform float pom_shadow_strength = 1.0;
// A fragment shader has no access to the scene's actual lights - this is a
// fixed direction the self-shadow ray marches towards.
uniform vec3 pom_shadow_light_direction = vec3(0.5, 0.75, 0.3);
uniform float pom_fade_start = 20.0;
uniform float pom_fade_end = 60.0;

// Landscape3D.DebugView.
uniform int debug_view = 0;

// Runtime virtual texturing (see Landscape3D.virtual_texture_enabled). 0: the layers are blended here,
// on every pixel. 1: they are read back from the virtual texture they were drawn into. 2: this is what
// draws them into it, a page at a time, from straight above.
uniform int rvt_mode = 0;
// The same virtual texture twice: its albedo layer is read through the sRGB view.
uniform sampler2DArray rvt_albedo : source_color, hint_virtual_texture, repeat_disable;
uniform sampler2DArray rvt_data : hint_virtual_texture, repeat_disable;
// Closer than this, the layers are blended here after all, at their own resolution.
uniform float rvt_near_distance = 0.0;
// Where the writer draws the terrain: the bottom of the virtual texture's volume, under anything else
// that draws into it.
uniform float rvt_writer_height = 0.0;

// Terrain space, displacement included.
varying vec3 terrain_position;
varying flat float patch_level;
varying vec2 patch_grid;

float fetch_height(ivec2 p_sample) {
	return texelFetch(heightmap, clamp(p_sample, ivec2(0), ivec2(terrain_quads)), 0).r;
}

bool fetch_hole(ivec2 p_sample) {
	return texelFetch(hole_map, clamp(p_sample, ivec2(0), ivec2(terrain_quads)), 0).r > 0.5;
}

vec4 catmull_rom_weights(float t) {
	float t2 = t * t;
	float t3 = t2 * t;
	return vec4(
			-0.5 * t3 + t2 - 0.5 * t,
			1.5 * t3 - 2.5 * t2 + 1.0,
			-1.5 * t3 + 2.0 * t2 + 0.5 * t,
			0.5 * t3 - 0.5 * t2);
}

// The heightmap at any point: exact on a sample, a Catmull-Rom spline through
// the samples around it anywhere else. The spline passes through every sample,
// so micro levels agree with level 0 wherever their vertices meet, and is
// smooth across them, so the ground they add does not show the heightmap's
// grid the way its plain triangles would up close.
float sample_height(vec2 p_sample) {
	vec2 cell = floor(p_sample);
	vec2 f = p_sample - cell;
	ivec2 base = ivec2(cell);
	if (f.x == 0.0 && f.y == 0.0) {
		return fetch_height(base);
	}
	vec4 wx = catmull_rom_weights(f.x);
	vec4 wz = catmull_rom_weights(f.y);
	float height = 0.0;
	for (int j = 0; j < 4; j++) {
		float row = 0.0;
		for (int i = 0; i < 4; i++) {
			row += wx[i] * fetch_height(base + ivec2(i - 1, j - 1));
		}
		height += wz[j] * row;
	}
	return height;
}

// Remaps a layer's raw height sample from [layer_height_min, ...max] to
// [0, 1] (for a height texture that doesn't already use its full range).
// textureLod(..., 0.0) rather than texture(): pom_offset()/pom_self_shadow()
// call this from inside a data-dependent loop, where automatic mip/LOD
// selection is undefined.
float get_layer_height(int layer_idx, vec2 uv) {
	float h = textureLod(height_array, vec3(uv, float(layer_idx)), 0.0).r;
	float lo = layer_height_min[layer_idx];
	float hi = layer_height_max[layer_idx];
	return clamp((h - lo) / max(hi - lo, 0.0001), 0.0, 1.0);
}

// How far the painted layers' height textures move the surface at a sample,
// blended by the layers' weights there like their colors are. The mipmap is
// chosen from the distance alone, never from the patch being drawn, so that
// two patches sharing a vertex displace it identically whatever their levels.
float sample_displacement(vec2 p_sample, float p_distance) {
	if (!displacement_enabled || p_distance >= displacement_fade_end) {
		return 0.0;
	}
	float fade = 1.0 - smoothstep(displacement_fade_start, displacement_fade_end, p_distance);
	vec2 local_xz = p_sample * vertex_spacing;
	vec2 weight_uv = (p_sample + 0.5) / float(terrain_quads + 1);
	float footprint = max(p_distance * displacement_footprint_scale, displacement_min_footprint);

	float displacement = 0.0;
	float weight_sum = 0.0;
	int group_count = (layer_count + 3) / 4;
	for (int g = 0; g < group_count; g++) {
		vec4 w = textureLod(weight_array, vec3(weight_uv, float(g)), 0.0);
		for (int c = 0; c < 4; c++) {
			int layer = g * 4 + c;
			if (layer >= layer_count) {
				break;
			}
			float weight = w[c];
			weight_sum += weight;
			float amount = layer_displacement[layer];
			if (weight <= 0.001 || amount == 0.0) {
				continue;
			}
			float uv_scale = layer_uv_scales[layer];
			float lod = log2(max(footprint * height_texture_size / uv_scale, 1.0));
			float h = textureLod(height_array, vec3(local_xz / uv_scale, float(layer)), lod).r;
			h = clamp((h - layer_height_min[layer]) / max(layer_height_max[layer] - layer_height_min[layer], 0.0001), 0.0, 1.0);
			displacement += (h - 0.5) * amount * weight;
		}
	}
	return weight_sum > 0.001 ? displacement * fade / weight_sum : 0.0;
}

float surface_height(vec2 p_sample) {
	float height = sample_height(p_sample);
	if (displacement_enabled) {
		vec3 position = vec3(p_sample.x * vertex_spacing, height, p_sample.y * vertex_spacing);
		height += sample_displacement(p_sample, distance(position, lod_camera_position));
	}
	return height;
}

void vertex() {
	if (rvt_mode == 2) {
		// The whole terrain as one quad, flat at the bottom of the virtual texture's volume: seen from
		// straight above, only where it lies matters, and lying under everything else lets whatever
		// else draws into the texture (a road, a decal) cover it.
		float extent = float(terrain_quads) * vertex_spacing;
		VERTEX = vec3(VERTEX.x * extent, rvt_writer_height, VERTEX.z * extent);
		NORMAL = vec3(0.0, 1.0, 0.0);
		terrain_position = VERTEX;
		patch_level = 0.0;
		patch_grid = vec2(0.0);
	} else {
		// The patch this instance draws (see LandscapeQuadtree::write_instance()).
		// COLOR is the instance color here: the patch mesh's own is white.
		vec2 origin = vec2(COLOR.x * 1024.0 + COLOR.y, COLOR.z * 1024.0 + COLOR.w);
		int level = int(INSTANCE_CUSTOM.x + 0.5) - LEVEL_BIAS;
		int edges = int(INSTANCE_CUSTOM.y + 0.5) | (int(INSTANCE_CUSTOM.z + 0.5) << 8);
		float stride = level >= 0 ? float(1 << level) : 1.0 / float(1 << (-level));
		ivec2 grid = ivec2(round(VERTEX.xz));
		float quads = float(terrain_quads);
		// Past the terrain's last sample, vertices collapse onto it.
		vec2 sample_position = min(origin + vec2(grid) * stride, vec2(quads));

		// A vertex on an edge shared with a coarser patch is moved onto that
		// patch's edge. Normally straight onto one of its vertices: the two patches
		// then meet at exactly the same points, computed the same way, so the seam
		// is watertight down to the pixel, and the triangles squeezed out along it
		// have no area left to draw. A patch so much coarser that its vertices lie
		// beyond this edge's own ends (more than PATCH_QUADS apart) cannot be met
		// that way, and this edge is laid along the straight line between the two
		// of its vertices instead. On a corner shared by two such edges, the
		// coarser neighbor wins.
		int snap = 0;
		bool along_z = false;
		if (grid.x == 0 && (edges & 15) > snap) {
			snap = edges & 15;
			along_z = true;
		}
		if (grid.x == PATCH_QUADS && ((edges >> 4) & 15) > snap) {
			snap = (edges >> 4) & 15;
			along_z = true;
		}
		if (grid.y == 0 && ((edges >> 8) & 15) > snap) {
			snap = (edges >> 8) & 15;
			along_z = false;
		}
		if (grid.y == PATCH_QUADS && ((edges >> 12) & 15) > snap) {
			snap = (edges >> 12) & 15;
			along_z = false;
		}

		float height;
		if (snap > 0 && (1 << snap) <= PATCH_QUADS) {
			int step = 1 << snap;
			if (along_z) {
				grid.y = (grid.y / step) * step;
			} else {
				grid.x = (grid.x / step) * step;
			}
			sample_position = min(origin + vec2(grid) * stride, vec2(quads));
			height = surface_height(sample_position);
		} else if (snap > 0) {
			float coarse = stride * float(1 << snap);
			float along = along_z ? sample_position.y : sample_position.x;
			float start = floor(along / coarse) * coarse;
			float end = min(start + coarse, quads);
			vec2 from = along_z ? vec2(sample_position.x, start) : vec2(start, sample_position.y);
			vec2 to = along_z ? vec2(sample_position.x, end) : vec2(end, sample_position.y);
			float from_height = surface_height(from);
			float to_height = surface_height(to);
			height = end > start ? from_height + (to_height - from_height) * ((along - start) / (end - start)) : from_height;
		} else {
			height = surface_height(sample_position);
		}

		VERTEX = vec3(sample_position.x * vertex_spacing, height, sample_position.y * vertex_spacing);
		// Only what lighting from the vertex needs, like shadow normal bias:
		// fragment() replaces it with the per-pixel normal.
		vec2 slope = textureLod(gradient_map, (sample_position + 0.5) / (quads + 1.0), 0.0).rg;
		NORMAL = normalize(vec3(-slope.x, 1.0, -slope.y));

		terrain_position = VERTEX;
		patch_level = float(level);
		patch_grid = vec2(grid);

		// Every triangle touching a hole sample is dropped, as level 0 drops every
		// quad with a hole at a corner: a vertex that is not a number takes its
		// triangles with it, in every pass.
		ivec2 lo = ivec2(floor(sample_position));
		ivec2 hi = ivec2(ceil(sample_position));
		if (fetch_hole(lo) || fetch_hole(hi) || fetch_hole(ivec2(lo.x, hi.y)) || fetch_hole(ivec2(hi.x, lo.y))) {
			VERTEX = vec3(uintBitsToFloat(0x7fc00000u));
		}
	}
}

// Steep parallax mapping with a linear-interpolation refinement between the
// two straddling samples - what makes this Parallax *Occlusion* Mapping
// rather than plain steep parallax. Returns the visible point's height
// (self-shadowing needs it as its starting depth) and writes the displaced
// UV to r_uv. h_scale is layer_heightmap_scale[layer_idx] already adjusted
// for distance fade (see accumulate_layer()).
float pom_offset(int layer_idx, vec2 uv, vec3 view_dir_tangent, float h_scale, out vec2 r_uv) {
	if (h_scale <= 0.0) {
		r_uv = uv;
		return get_layer_height(layer_idx, uv);
	}

	// Fewer steps needed at a glancing angle without a visible quality
	// loss (there's less parallax to resolve), which is where most of a
	// terrain's surface faces at typical camera angles.
	float layer_count_f = mix(float(pom_max_layers), float(pom_min_layers), clamp(view_dir_tangent.z, 0.0, 1.0));
	float layer_h = 1.0 / layer_count_f;

	// max() avoids dividing by ~0 at a near-grazing angle and doubles as
	// offset limiting there.
	vec2 ray_uv = view_dir_tangent.xy / max(view_dir_tangent.z, 0.02);

	// The ray starts at the top of the height volume and steps down/backward
	// until it crosses the actual height field.
	vec2 uv_cur = uv + ray_uv * h_scale;
	vec2 delta_uv = ray_uv * (h_scale * layer_h);
	vec2 uv_prev = uv_cur;
	float h_ray = 1.0;
	float h_map = get_layer_height(layer_idx, uv_cur);
	float h_map_prev = h_map;

	int guard = 0;
	while (h_map < h_ray && guard < 256) {
		uv_prev = uv_cur;
		h_map_prev = h_map;
		uv_cur -= delta_uv;
		h_ray -= layer_h;
		h_map = get_layer_height(layer_idx, uv_cur);
		guard += 1;
	}

	// The ray crossed the height field somewhere between the previous and
	// current samples; their zero-crossing pinpoints where the two meet.
	float h_ray_prev = h_ray + layer_h;
	float f_prev = h_ray_prev - h_map_prev;
	float f_cur = h_ray - h_map;
	float t = clamp(f_prev / max(f_prev - f_cur, 0.000001), 0.0, 1.0);

	r_uv = mix(uv_prev, uv_cur, t);
	return mix(h_map_prev, h_map, t);
}

// Soft self-shadowing with penumbra: marches from the visible point towards
// the light through the same height field, darkening the result wherever
// nearby height detail pokes up above the unoccluded ray.
float pom_self_shadow(int layer_idx, vec2 uv, float h_start, vec3 light_dir_tangent, float h_scale) {
	if (light_dir_tangent.z <= 0.0) {
		return 1.0;
	}

	float layer_h = (1.0 - h_start) / float(pom_shadow_steps);
	if (layer_h <= 0.0) {
		return 1.0;
	}

	vec2 ray_uv = light_dir_tangent.xy / max(light_dir_tangent.z, 0.1);
	vec2 delta_uv = ray_uv * (h_scale * layer_h);

	vec2 uv_l = uv;
	float h_ray = h_start;
	float shadow_factor = 1.0;

	for (int i = 0; i < pom_shadow_steps; i++) {
		uv_l += delta_uv;
		h_ray += layer_h;
		float h_map = get_layer_height(layer_idx, uv_l);
		if (h_map > h_ray) {
			// A blocker roughly 2 steps thick (in height) reads as a full
			// shadow; thinner detail gives a softer penumbra.
			float occ = clamp((h_map - h_ray) / (layer_h * 2.0), 0.0, 1.0) * pom_shadow_strength;
			shadow_factor = min(shadow_factor, 1.0 - occ);
			if (shadow_factor < 0.001) {
				break;
			}
		}
	}
	return shadow_factor;
}

// Triplanar blend weights from a terrain space normal: sharpened, normalized
// absolute components, the same formula as BaseMaterial3D's own triplanar
// mapping.
vec3 triplanar_weights(vec3 normal, float sharpness) {
	vec3 w = pow(abs(normal), vec3(sharpness));
	return w / max(dot(w, vec3(1.0)), 0.00001);
}

// Same axis convention (and Y-flip) as BaseMaterial3D's triplanar_texture().
vec4 sample_triplanar(sampler2DArray tex_array, int layer_idx, vec3 tp_pos, vec3 weights) {
	vec4 samp = vec4(0.0);
	samp += texture(tex_array, vec3(tp_pos.xy, float(layer_idx))) * weights.z;
	samp += texture(tex_array, vec3(tp_pos.xz, float(layer_idx))) * weights.y;
	samp += texture(tex_array, vec3(tp_pos.zy * vec2(-1.0, 1.0), float(layer_idx))) * weights.x;
	return samp;
}

// Accumulates one layer's contribution, weighted, into the running sums -
// skipped entirely for a layer with (near-)zero weight here, so a terrain
// only pays for the layers actually present at a given point.
//
// view_dir_tangent/light_dir_tangent/normal/pom_fade are computed once in
// fragment() and passed in: Godot's shading language only exposes the
// built-ins they come from inside fragment() itself.
void accumulate_layer(int layer_idx, float w, vec3 position, bool pom_active, vec3 view_dir_tangent, vec3 light_dir_tangent, vec3 normal, float pom_fade, inout vec3 albedo_sum, inout vec3 normal_sum, inout vec3 orm_sum, inout float specular_sum, inout float normal_strength_sum, inout float weight_sum) {
	if (w <= 0.001) {
		return;
	}

	vec3 albedo;
	vec3 normal_tex;
	vec3 orm;

	if (layer_triplanar[layer_idx] > 0.5) {
		vec3 tp_pos = (position * vec3(1.0, -1.0, 1.0)) / layer_uv_scales[layer_idx];
		vec3 tp_weights = triplanar_weights(normal, layer_triplanar_sharpness[layer_idx]);
		albedo = sample_triplanar(albedo_array, layer_idx, tp_pos, tp_weights).rgb;
		normal_tex = sample_triplanar(normal_array, layer_idx, tp_pos, tp_weights).rgb;
		orm = sample_triplanar(orm_array, layer_idx, tp_pos, tp_weights).rgb;
	} else {
		vec2 uv = position.xz / layer_uv_scales[layer_idx];
		if (pom_active && layer_pom_enabled[layer_idx] > 0.5) {
			// * 0.01: layer_heightmap_scale is a small
			// BaseMaterial3D.heightmap_scale-alike, not a raw UV-space
			// displacement.
			float h_scale = layer_heightmap_scale[layer_idx] * 0.01 * pom_fade;
			vec2 uv_hit = uv;
			float h_hit = pom_offset(layer_idx, uv, view_dir_tangent, h_scale, uv_hit);
			uv = uv_hit;
			float shadow = 1.0;
			if (pom_self_shadow_enabled) {
				shadow = pom_self_shadow(layer_idx, uv, h_hit, light_dir_tangent, h_scale);
			}
			albedo = texture(albedo_array, vec3(uv, float(layer_idx))).rgb * shadow;
		} else {
			albedo = texture(albedo_array, vec3(uv, float(layer_idx))).rgb;
		}
		normal_tex = texture(normal_array, vec3(uv, float(layer_idx))).rgb;
		orm = texture(orm_array, vec3(uv, float(layer_idx))).rgb;
	}

	albedo *= layer_albedo_colors[layer_idx].rgb;
	// mix(), not multiply: ao_strength = 0 should mean "no occlusion" (1.0,
	// fully lit), not "full occlusion" (0.0, black).
	orm.r = mix(1.0, orm.r, layer_ao_strength[layer_idx]);
	orm.g = clamp(orm.g * layer_roughness[layer_idx], 0.0, 1.0);

	albedo_sum += albedo * w;
	normal_sum += normal_tex * w;
	orm_sum += orm * w;
	specular_sum += layer_specular[layer_idx] * w;
	normal_strength_sum += layer_normal_strength[layer_idx] * w;
	weight_sum += w;
}

vec3 level_color(float p_level) {
	return 0.5 + 0.5 * cos(6.28318 * (p_level * 0.137 + vec3(0.0, 0.33, 0.67)));
}

void fragment() {
	vec3 position = terrain_position;
	vec2 sample_position = position.xz / vertex_spacing;
	if (rvt_mode == 2) {
		// Drawn as one flat quad: the height (which triplanar layers project from) is the
		// heightmap's, between its samples.
		vec2 cell = floor(sample_position);
		vec2 f = sample_position - cell;
		ivec2 base = ivec2(cell);
		position.y = mix(mix(fetch_height(base), fetch_height(base + ivec2(1, 0)), f.x), mix(fetch_height(base + ivec2(0, 1)), fetch_height(base + ivec2(1, 1)), f.x), f.y);
	}
	vec2 map_uv = (sample_position + 0.5) / float(terrain_quads + 1);

	// The surface's frame, per pixel, from the full resolution heightmap
	// whatever the tessellation: the normal from its slope, the tangent along
	// +X (the direction texture U runs in) and the binormal along -Z, the
	// same frame a mesh with those UVs gets.
	vec2 slope = texture(gradient_map, map_uv).rg;
	vec3 normal_local = normalize(vec3(-slope.x, 1.0, -slope.y));
	vec3 tangent_local = normalize(vec3(1.0, slope.x, 0.0));
	vec3 binormal_local = cross(normal_local, tangent_local);
	mat3 model_view = mat3(VIEW_MATRIX) * mat3(MODEL_MATRIX);
	NORMAL = normalize(mat3(VIEW_MATRIX) * (MODEL_NORMAL_MATRIX * normal_local));
	TANGENT = normalize(model_view * tangent_local);
	BINORMAL = normalize(model_view * binormal_local);

	float view_distance = length(VERTEX);

	// How much of the runtime virtual texture shows here, against blending the layers here: all of
	// it, but for what is closer than rvt_near_distance, and wherever parallax (which depends on
	// where the camera is, so it cannot be drawn into a texture ahead of time) has not faded out.
	float rvt_amount = 0.0;
	if (rvt_mode == 1) {
		rvt_amount = rvt_near_distance > 0.0 ? smoothstep(rvt_near_distance * 0.75, rvt_near_distance, view_distance) : 1.0;
		if (pom_enabled) {
			rvt_amount = min(rvt_amount, smoothstep(pom_fade_start, pom_fade_end, view_distance));
		}
	}

	vec3 albedo = vec3(0.6);
	// Tangent space, with normal map strength already applied.
	vec3 normal_tangent = vec3(0.0, 0.0, 1.0);
	vec3 orm = vec3(1.0, 1.0, 0.0);
	float specular = 0.5;

	if (rvt_amount < 1.0) {
		// Parallax depends on the view, which a page drawn from above does not have.
		bool pom_active = pom_enabled && rvt_mode != 2;

		// Shared per-fragment values every layer's accumulate_layer() call needs
		// but can't compute itself: tangent-space directions for POM (with the
		// same BINORMAL negation Godot's own heightmap shader code uses), and how
		// much pom_fade_start/end fades this fragment's parallax depth.
		vec3 view_dir_tangent = vec3(0.0);
		vec3 light_dir_tangent = vec3(0.0);
		if (pom_active) {
			mat3 tbn = mat3(TANGENT * pom_flip.x, -BINORMAL * pom_flip.y, NORMAL);
			view_dir_tangent = normalize(normalize(-VERTEX) * tbn);
			if (pom_self_shadow_enabled) {
				vec3 light_dir_view = normalize((VIEW_MATRIX * vec4(pom_shadow_light_direction, 0.0)).xyz);
				light_dir_tangent = normalize(light_dir_view * tbn);
			}
		}
		float pom_fade = 1.0;
		if (pom_fade_end > pom_fade_start) {
			pom_fade = 1.0 - smoothstep(pom_fade_start, pom_fade_end, view_distance);
		}

		vec3 albedo_sum = vec3(0.0);
		vec3 normal_sum = vec3(0.0);
		vec3 orm_sum = vec3(0.0);
		float specular_sum = 0.0;
		float normal_strength_sum = 0.0;
		float weight_sum = 0.0;

		// Every layer's weight lives in one of ceil(layer_count / 4) array
		// layers, 4 layers (R/G/B/A) per texture fetch.
		int group_count = (layer_count + 3) / 4;
		for (int g = 0; g < group_count; g++) {
			vec4 w = texture(weight_array, vec3(map_uv, float(g)));
			int base_layer = g * 4;
			if (base_layer < layer_count) {
				accumulate_layer(base_layer, w.r, position, pom_active, view_dir_tangent, light_dir_tangent, normal_local, pom_fade, albedo_sum, normal_sum, orm_sum, specular_sum, normal_strength_sum, weight_sum);
			}
			if (base_layer + 1 < layer_count) {
				accumulate_layer(base_layer + 1, w.g, position, pom_active, view_dir_tangent, light_dir_tangent, normal_local, pom_fade, albedo_sum, normal_sum, orm_sum, specular_sum, normal_strength_sum, weight_sum);
			}
			if (base_layer + 2 < layer_count) {
				accumulate_layer(base_layer + 2, w.b, position, pom_active, view_dir_tangent, light_dir_tangent, normal_local, pom_fade, albedo_sum, normal_sum, orm_sum, specular_sum, normal_strength_sum, weight_sum);
			}
			if (base_layer + 3 < layer_count) {
				accumulate_layer(base_layer + 3, w.a, position, pom_active, view_dir_tangent, light_dir_tangent, normal_local, pom_fade, albedo_sum, normal_sum, orm_sum, specular_sum, normal_strength_sum, weight_sum);
			}
		}

		// Normalize so the total always adds up to 1; with no weight at all
		// (no layers yet), plain gray ground.
		if (weight_sum > 0.001) {
			float inv_weight = 1.0 / weight_sum;
			albedo = albedo_sum * inv_weight;
			orm = orm_sum * inv_weight;
			specular = specular_sum * inv_weight;
			// The blended normal map, with its blended strength folded in: what Godot's own
			// mix(NORMAL, TBN * normal_map, NORMAL_MAP_DEPTH) gives, as a unit normal in tangent space.
			vec2 normal_xy = (normal_sum.xy * inv_weight) * 2.0 - 1.0;
			vec3 normal_map = vec3(normal_xy, sqrt(max(0.0, 1.0 - dot(normal_xy, normal_xy))));
			normal_tangent = normalize(mix(vec3(0.0, 0.0, 1.0), normal_map, normal_strength_sum * inv_weight));
		}
	}

	if (rvt_amount > 0.0) {
		vec3 rvt_uv = vec3(position.xz / (float(terrain_quads) * vertex_spacing), 0.0);
		vec4 rvt_color = texture(rvt_albedo, rvt_uv);
		vec4 rvt_normal = texture(rvt_data, vec3(rvt_uv.xy, 1.0));
		vec4 rvt_orm = texture(rvt_data, vec3(rvt_uv.xy, 2.0));
		// Pages are drawn looking down the terrain's Y axis with X to the right and Z down the page:
		// their view space normal (x, y, z) is the terrain's (x, z, -y).
		vec3 page_normal = normalize(rvt_normal.rgb * 2.0 - 1.0);
		vec3 normal_view = normalize(mat3(VIEW_MATRIX) * (MODEL_NORMAL_MATRIX * vec3(page_normal.x, page_normal.z, -page_normal.y)));
		vec3 rvt_tangent = vec3(dot(normal_view, TANGENT), dot(normal_view, BINORMAL), dot(normal_view, NORMAL));
		albedo = mix(albedo, rvt_color.rgb, rvt_amount);
		normal_tangent = normalize(mix(normal_tangent, rvt_tangent, rvt_amount));
		orm = mix(orm, rvt_orm.rgb, rvt_amount);
		specular = mix(specular, rvt_normal.a, rvt_amount);
	}

	ALBEDO = albedo;
	NORMAL_MAP = vec3(normal_tangent.xy * 0.5 + 0.5, 1.0);
	NORMAL_MAP_DEPTH = 1.0;
	AO = orm.r;
	ROUGHNESS = clamp(orm.g, 0.0, 1.0);
	METALLIC = orm.b;
	SPECULAR = specular;

	if (rvt_mode == 2) {
		// Debug views are drawn over the virtual texture, not into it.
	} else if (debug_view == 1) {
		// LOD levels: one color per quadtree level.
		ALBEDO = mix(ALBEDO, level_color(patch_level), 0.7);
	} else if (debug_view == 2) {
		// Wireframe: every triangle's edges, patch borders thicker, colored by
		// level.
		vec2 cell = fract(patch_grid);
		vec2 width = max(fwidth(patch_grid), vec2(0.0001));
		float quad_edge = min(min(cell.x, 1.0 - cell.x) / width.x, min(cell.y, 1.0 - cell.y) / width.y);
		float diagonal = abs(cell.x + cell.y - 1.0) / (width.x + width.y);
		float wire = 1.0 - clamp(min(quad_edge, diagonal), 0.0, 1.0);
		vec2 border_distance = min(patch_grid, vec2(float(PATCH_QUADS)) - patch_grid) / width;
		float border = 1.0 - clamp(min(border_distance.x, border_distance.y) - 1.0, 0.0, 1.0);
		ALBEDO = mix(ALBEDO * 0.35, level_color(patch_level), max(wire * 0.8, border));
	} else if (debug_view == 3) {
		// Virtual texture: tinted where the runtime virtual texture is what shows.
		ALBEDO = mix(ALBEDO, vec3(0.2, 0.9, 0.3), 0.5 * rvt_amount);
	}
}
)");
}

void Landscape3D::finish_shaders() {
	shader.unref();
	patch_mesh.unref();
}

void Landscape3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_terrain_data", "data"), &Landscape3D::set_terrain_data);
	ClassDB::bind_method(D_METHOD("get_terrain_data"), &Landscape3D::get_terrain_data);

	ClassDB::bind_method(D_METHOD("set_layers", "layers"), &Landscape3D::set_layers);
	ClassDB::bind_method(D_METHOD("get_layers"), &Landscape3D::get_layers);

	ClassDB::bind_method(D_METHOD("set_lod_pixel_error", "pixels"), &Landscape3D::set_lod_pixel_error);
	ClassDB::bind_method(D_METHOD("get_lod_pixel_error"), &Landscape3D::get_lod_pixel_error);

	ClassDB::bind_method(D_METHOD("set_lod_max_quad_pixels", "pixels"), &Landscape3D::set_lod_max_quad_pixels);
	ClassDB::bind_method(D_METHOD("get_lod_max_quad_pixels"), &Landscape3D::get_lod_max_quad_pixels);

	ClassDB::bind_method(D_METHOD("set_frustum_culling", "enabled"), &Landscape3D::set_frustum_culling);
	ClassDB::bind_method(D_METHOD("is_frustum_culling_enabled"), &Landscape3D::is_frustum_culling_enabled);

	ClassDB::bind_method(D_METHOD("set_gpu_lod_enabled", "enabled"), &Landscape3D::set_gpu_lod_enabled);
	ClassDB::bind_method(D_METHOD("is_gpu_lod_enabled"), &Landscape3D::is_gpu_lod_enabled);

	ClassDB::bind_method(D_METHOD("set_shadow_distance", "distance"), &Landscape3D::set_shadow_distance);
	ClassDB::bind_method(D_METHOD("get_shadow_distance"), &Landscape3D::get_shadow_distance);

	ClassDB::bind_method(D_METHOD("set_micro_detail_levels", "levels"), &Landscape3D::set_micro_detail_levels);
	ClassDB::bind_method(D_METHOD("get_micro_detail_levels"), &Landscape3D::get_micro_detail_levels);

	ClassDB::bind_method(D_METHOD("set_micro_detail_distance", "distance"), &Landscape3D::set_micro_detail_distance);
	ClassDB::bind_method(D_METHOD("get_micro_detail_distance"), &Landscape3D::get_micro_detail_distance);

	ClassDB::bind_method(D_METHOD("set_micro_detail_triangle_size", "pixels"), &Landscape3D::set_micro_detail_triangle_size);
	ClassDB::bind_method(D_METHOD("get_micro_detail_triangle_size"), &Landscape3D::get_micro_detail_triangle_size);

	ClassDB::bind_method(D_METHOD("set_debug_view", "view"), &Landscape3D::set_debug_view);
	ClassDB::bind_method(D_METHOD("get_debug_view"), &Landscape3D::get_debug_view);

	ClassDB::bind_method(D_METHOD("set_virtual_texture_enabled", "enabled"), &Landscape3D::set_virtual_texture_enabled);
	ClassDB::bind_method(D_METHOD("is_virtual_texture_enabled"), &Landscape3D::is_virtual_texture_enabled);

	ClassDB::bind_method(D_METHOD("set_virtual_texture_texel_size", "size"), &Landscape3D::set_virtual_texture_texel_size);
	ClassDB::bind_method(D_METHOD("get_virtual_texture_texel_size"), &Landscape3D::get_virtual_texture_texel_size);

	ClassDB::bind_method(D_METHOD("set_virtual_texture_near_distance", "distance"), &Landscape3D::set_virtual_texture_near_distance);
	ClassDB::bind_method(D_METHOD("get_virtual_texture_near_distance"), &Landscape3D::get_virtual_texture_near_distance);

	ClassDB::bind_method(D_METHOD("set_virtual_texture_layers", "layers"), &Landscape3D::set_virtual_texture_layers);
	ClassDB::bind_method(D_METHOD("get_virtual_texture_layers"), &Landscape3D::get_virtual_texture_layers);

	ClassDB::bind_method(D_METHOD("get_virtual_texture"), &Landscape3D::get_virtual_texture);
	ClassDB::bind_method(D_METHOD("get_virtual_texture_volume"), &Landscape3D::get_virtual_texture_volume);
	ClassDB::bind_method(D_METHOD("get_virtual_texture_size"), &Landscape3D::get_virtual_texture_size);

	ClassDB::bind_method(D_METHOD("set_occluder_enabled", "enabled"), &Landscape3D::set_occluder_enabled);
	ClassDB::bind_method(D_METHOD("is_occluder_enabled"), &Landscape3D::is_occluder_enabled);

	ClassDB::bind_method(D_METHOD("set_occluder_detail", "detail"), &Landscape3D::set_occluder_detail);
	ClassDB::bind_method(D_METHOD("get_occluder_detail"), &Landscape3D::get_occluder_detail);

	ClassDB::bind_method(D_METHOD("set_cast_shadow", "setting"), &Landscape3D::set_cast_shadow);
	ClassDB::bind_method(D_METHOD("get_cast_shadow"), &Landscape3D::get_cast_shadow);

	ClassDB::bind_method(D_METHOD("set_gi_mode", "mode"), &Landscape3D::set_gi_mode);
	ClassDB::bind_method(D_METHOD("get_gi_mode"), &Landscape3D::get_gi_mode);

	ClassDB::bind_method(D_METHOD("set_collision_layer", "layer"), &Landscape3D::set_collision_layer);
	ClassDB::bind_method(D_METHOD("get_collision_layer"), &Landscape3D::get_collision_layer);

	ClassDB::bind_method(D_METHOD("set_collision_mask", "mask"), &Landscape3D::set_collision_mask);
	ClassDB::bind_method(D_METHOD("get_collision_mask"), &Landscape3D::get_collision_mask);

	ClassDB::bind_method(D_METHOD("set_layer_texture_size_limit", "size"), &Landscape3D::set_layer_texture_size_limit);
	ClassDB::bind_method(D_METHOD("get_layer_texture_size_limit"), &Landscape3D::get_layer_texture_size_limit);

	ClassDB::bind_method(D_METHOD("set_pom_enabled", "enable"), &Landscape3D::set_pom_enabled);
	ClassDB::bind_method(D_METHOD("is_pom_enabled"), &Landscape3D::is_pom_enabled);

	ClassDB::bind_method(D_METHOD("set_pom_min_layers", "layers"), &Landscape3D::set_pom_min_layers);
	ClassDB::bind_method(D_METHOD("get_pom_min_layers"), &Landscape3D::get_pom_min_layers);

	ClassDB::bind_method(D_METHOD("set_pom_max_layers", "layers"), &Landscape3D::set_pom_max_layers);
	ClassDB::bind_method(D_METHOD("get_pom_max_layers"), &Landscape3D::get_pom_max_layers);

	ClassDB::bind_method(D_METHOD("set_pom_flip_tangent", "flip"), &Landscape3D::set_pom_flip_tangent);
	ClassDB::bind_method(D_METHOD("get_pom_flip_tangent"), &Landscape3D::get_pom_flip_tangent);

	ClassDB::bind_method(D_METHOD("set_pom_flip_binormal", "flip"), &Landscape3D::set_pom_flip_binormal);
	ClassDB::bind_method(D_METHOD("get_pom_flip_binormal"), &Landscape3D::get_pom_flip_binormal);

	ClassDB::bind_method(D_METHOD("set_pom_self_shadow_enabled", "enable"), &Landscape3D::set_pom_self_shadow_enabled);
	ClassDB::bind_method(D_METHOD("is_pom_self_shadow_enabled"), &Landscape3D::is_pom_self_shadow_enabled);

	ClassDB::bind_method(D_METHOD("set_pom_shadow_steps", "steps"), &Landscape3D::set_pom_shadow_steps);
	ClassDB::bind_method(D_METHOD("get_pom_shadow_steps"), &Landscape3D::get_pom_shadow_steps);

	ClassDB::bind_method(D_METHOD("set_pom_shadow_strength", "strength"), &Landscape3D::set_pom_shadow_strength);
	ClassDB::bind_method(D_METHOD("get_pom_shadow_strength"), &Landscape3D::get_pom_shadow_strength);

	ClassDB::bind_method(D_METHOD("set_pom_shadow_light_direction", "direction"), &Landscape3D::set_pom_shadow_light_direction);
	ClassDB::bind_method(D_METHOD("get_pom_shadow_light_direction"), &Landscape3D::get_pom_shadow_light_direction);

	ClassDB::bind_method(D_METHOD("set_pom_fade_start", "distance"), &Landscape3D::set_pom_fade_start);
	ClassDB::bind_method(D_METHOD("get_pom_fade_start"), &Landscape3D::get_pom_fade_start);

	ClassDB::bind_method(D_METHOD("set_pom_fade_end", "distance"), &Landscape3D::set_pom_fade_end);
	ClassDB::bind_method(D_METHOD("get_pom_fade_end"), &Landscape3D::get_pom_fade_end);

	ClassDB::bind_method(D_METHOD("sculpt", "local_position", "radius", "strength", "operation", "falloff", "flatten_height", "update_collision"), &Landscape3D::sculpt, DEFVAL(1.0f), DEFVAL(0.0f), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("paint_layer", "local_position", "radius", "strength", "layer_index", "falloff"), &Landscape3D::paint_layer, DEFVAL(1.0f));
	ClassDB::bind_method(D_METHOD("set_hole", "local_position", "radius", "hole", "update_collision"), &Landscape3D::set_hole, DEFVAL(true));

	ClassDB::bind_method(D_METHOD("get_height_region", "region"), &Landscape3D::get_height_region);
	ClassDB::bind_method(D_METHOD("set_height_region", "region", "heights", "update_collision"), &Landscape3D::set_height_region, DEFVAL(true));

	ClassDB::bind_method(D_METHOD("get_layer_weight_region", "region", "layer_index"), &Landscape3D::get_layer_weight_region);
	ClassDB::bind_method(D_METHOD("set_layer_weight_region", "region", "layer_index", "weights"), &Landscape3D::set_layer_weight_region);

	ClassDB::bind_method(D_METHOD("get_hole_region", "region"), &Landscape3D::get_hole_region);
	ClassDB::bind_method(D_METHOD("set_hole_region", "region", "holes"), &Landscape3D::set_hole_region);

	ClassDB::bind_method(D_METHOD("get_height_regions", "regions"), &Landscape3D::get_height_regions);
	ClassDB::bind_method(D_METHOD("set_height_regions", "regions", "heights", "update_collision"), &Landscape3D::set_height_regions, DEFVAL(true));

	ClassDB::bind_method(D_METHOD("get_layer_weight_regions", "regions", "layer_index"), &Landscape3D::get_layer_weight_regions);
	ClassDB::bind_method(D_METHOD("set_layer_weight_regions", "regions", "layer_index", "weights"), &Landscape3D::set_layer_weight_regions);
	ClassDB::bind_method(D_METHOD("paint_layer_regions", "regions", "layer_index", "weights"), &Landscape3D::paint_layer_regions);

	ClassDB::bind_method(D_METHOD("update_collision"), &Landscape3D::update_collision);

	ClassDB::bind_method(D_METHOD("get_aabb"), &Landscape3D::get_aabb);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "terrain_data", PROPERTY_HINT_RESOURCE_TYPE, "TerrainData"), "set_terrain_data", "get_terrain_data");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "layers", PROPERTY_HINT_ARRAY_TYPE, MAKE_RESOURCE_TYPE_HINT("TerrainLayer")), "set_layers", "get_layers");

	ADD_GROUP("Level of Detail", "lod_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lod_pixel_error", PROPERTY_HINT_RANGE, "0.25,16,0.05,or_greater,suffix:px"), "set_lod_pixel_error", "get_lod_pixel_error");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lod_max_quad_pixels", PROPERTY_HINT_RANGE, "4,512,1,or_greater,suffix:px"), "set_lod_max_quad_pixels", "get_lod_max_quad_pixels");

	ADD_GROUP("Micro Detail", "micro_detail_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "micro_detail_levels", PROPERTY_HINT_RANGE, vformat("0,%d,1", LandscapeQuadtree::MAX_MICRO_LEVELS)), "set_micro_detail_levels", "get_micro_detail_levels");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "micro_detail_distance", PROPERTY_HINT_RANGE, "0,1024,0.1,or_greater,suffix:m"), "set_micro_detail_distance", "get_micro_detail_distance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "micro_detail_triangle_size", PROPERTY_HINT_RANGE, "1,64,0.5,or_greater,suffix:px"), "set_micro_detail_triangle_size", "get_micro_detail_triangle_size");

	ADD_GROUP("Rendering", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cast_shadow", PROPERTY_HINT_ENUM, "Off,On,Double-Sided,Shadows Only"), "set_cast_shadow", "get_cast_shadow");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shadow_distance", PROPERTY_HINT_RANGE, "0,16384,1,or_greater,suffix:m"), "set_shadow_distance", "get_shadow_distance");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "gi_mode", PROPERTY_HINT_ENUM, "Disabled,Static,Dynamic"), "set_gi_mode", "get_gi_mode");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "frustum_culling"), "set_frustum_culling", "is_frustum_culling_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "gpu_lod_enabled"), "set_gpu_lod_enabled", "is_gpu_lod_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "layer_texture_size_limit", PROPERTY_HINT_RANGE, "16,8192,1"), "set_layer_texture_size_limit", "get_layer_texture_size_limit");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "debug_view", PROPERTY_HINT_ENUM, "Disabled,LOD Levels,Wireframe,Virtual Texture"), "set_debug_view", "get_debug_view");

	ADD_GROUP("Virtual Texture", "virtual_texture_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "virtual_texture_enabled"), "set_virtual_texture_enabled", "is_virtual_texture_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "virtual_texture_texel_size", PROPERTY_HINT_RANGE, "0.001,1,0.001,or_greater,suffix:m"), "set_virtual_texture_texel_size", "get_virtual_texture_texel_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "virtual_texture_near_distance", PROPERTY_HINT_RANGE, "0,256,0.1,or_greater,suffix:m"), "set_virtual_texture_near_distance", "get_virtual_texture_near_distance");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "virtual_texture_layers", PROPERTY_HINT_LAYERS_3D_RENDER), "set_virtual_texture_layers", "get_virtual_texture_layers");

	ADD_GROUP("Occlusion Culling", "occluder_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "occluder_enabled"), "set_occluder_enabled", "is_occluder_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "occluder_detail", PROPERTY_HINT_ENUM, "1x1 Quad (Fastest):1,2x2 Quads:2,4x4 Quads:4,8x8 Quads:8,16x16 Quads:16,32x32 Quads (Most Accurate):32"), "set_occluder_detail", "get_occluder_detail");

	ADD_GROUP("Parallax Occlusion Mapping", "pom_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "pom_enabled"), "set_pom_enabled", "is_pom_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "pom_min_layers", PROPERTY_HINT_RANGE, "1,64,1"), "set_pom_min_layers", "get_pom_min_layers");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "pom_max_layers", PROPERTY_HINT_RANGE, "1,64,1"), "set_pom_max_layers", "get_pom_max_layers");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "pom_flip_tangent"), "set_pom_flip_tangent", "get_pom_flip_tangent");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "pom_flip_binormal"), "set_pom_flip_binormal", "get_pom_flip_binormal");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "pom_self_shadow_enabled"), "set_pom_self_shadow_enabled", "is_pom_self_shadow_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "pom_shadow_steps", PROPERTY_HINT_RANGE, "1,32,1"), "set_pom_shadow_steps", "get_pom_shadow_steps");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "pom_shadow_strength", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_pom_shadow_strength", "get_pom_shadow_strength");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "pom_shadow_light_direction"), "set_pom_shadow_light_direction", "get_pom_shadow_light_direction");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "pom_fade_start", PROPERTY_HINT_RANGE, "0,4096,0.1,or_greater,suffix:m"), "set_pom_fade_start", "get_pom_fade_start");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "pom_fade_end", PROPERTY_HINT_RANGE, "0,4096,0.1,or_greater,suffix:m"), "set_pom_fade_end", "get_pom_fade_end");

	ADD_GROUP("Collision", "collision_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_layer", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_layer", "get_collision_layer");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_mask", "get_collision_mask");

	ADD_SIGNAL(MethodInfo("terrain_changed", PropertyInfo(Variant::RECT2I, "region")));

	BIND_ENUM_CONSTANT(SCULPT_RAISE);
	BIND_ENUM_CONSTANT(SCULPT_LOWER);
	BIND_ENUM_CONSTANT(SCULPT_SMOOTH);
	BIND_ENUM_CONSTANT(SCULPT_FLATTEN);

	BIND_ENUM_CONSTANT(DEBUG_VIEW_DISABLED);
	BIND_ENUM_CONSTANT(DEBUG_VIEW_LOD_LEVELS);
	BIND_ENUM_CONSTANT(DEBUG_VIEW_WIREFRAME);
	BIND_ENUM_CONSTANT(DEBUG_VIEW_VIRTUAL_TEXTURE);
}

void Landscape3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			if (terrain_data.is_valid() && quadtree.is_empty()) {
				_rebuild_terrain();
			}
			if (terrain_data.is_valid()) {
				update_collision();
			}
			_connect_frame_hook();
		} break;

		case NOTIFICATION_EXIT_TREE: {
			_disconnect_frame_hook();
		} break;

		case NOTIFICATION_ENTER_WORLD: {
			_update_draw_instances();
			_update_writer_instance();
			_update_virtual_texture_volume();
			const RID scenario = get_world_3d().is_valid() ? get_world_3d()->get_scenario() : RID();
			const bool visible = is_visible_in_tree();
			for (KeyValue<Vector2i, OccluderBlock> &kv : occluder_blocks) {
				if (kv.value.instance.is_valid()) {
					RS::get_singleton()->instance_set_scenario(kv.value.instance, scenario);
					RS::get_singleton()->instance_set_visible(kv.value.instance, visible);
				}
			}
			lod_dirty = true;
		} break;

		case NOTIFICATION_EXIT_WORLD: {
			for (int i = 0; i < DRAW_MAX; i++) {
				if (draw_instances[i].is_valid()) {
					RS::get_singleton()->instance_set_scenario(draw_instances[i], RID());
				}
			}
			if (writer_instance.is_valid()) {
				RS::get_singleton()->instance_set_scenario(writer_instance, RID());
			}
			for (KeyValue<Vector2i, OccluderBlock> &kv : occluder_blocks) {
				if (kv.value.instance.is_valid()) {
					RS::get_singleton()->instance_set_scenario(kv.value.instance, RID());
				}
			}
		} break;

		case NOTIFICATION_TRANSFORM_CHANGED: {
			_update_draw_instances();
			_update_occluder_transforms();
			_update_writer_instance();
			_update_virtual_texture_volume();
			lod_dirty = true;
			// Whatever sits on the surface (see LandscapeSpline3D) has to follow
			// it wherever it moved.
			_emit_terrain_changed(_get_full_region());
		} break;

		case NOTIFICATION_VISIBILITY_CHANGED: {
			_update_draw_instances();
			_update_writer_instance();
			const bool visible = is_visible_in_tree();
			for (KeyValue<Vector2i, OccluderBlock> &kv : occluder_blocks) {
				if (kv.value.instance.is_valid()) {
					RS::get_singleton()->instance_set_visible(kv.value.instance, visible);
				}
			}
			lod_dirty = true;
		} break;
	}
}

Transform3D Landscape3D::_get_safe_global_transform() const {
	return is_inside_tree() ? get_global_transform() : Transform3D();
}

void Landscape3D::_ensure_patch_mesh() {
	MutexLock lock(patch_mesh_mutex);
	if (patch_mesh.is_valid()) {
		return;
	}

	// The mesh every patch is drawn with: a flat grid whose vertices only
	// carry their grid coordinates (in X and Z); the shader places them.
	constexpr int quads = LandscapeQuadtree::PATCH_QUADS;
	constexpr int side = quads + 1;

	PackedVector3Array vertices;
	PackedVector3Array normals;
	PackedFloat32Array tangents;
	PackedColorArray colors;
	vertices.resize(side * side);
	normals.resize(side * side);
	tangents.resize(side * side * 4);
	colors.resize(side * side);
	for (int z = 0; z < side; z++) {
		for (int x = 0; x < side; x++) {
			const int i = z * side + x;
			vertices.set(i, Vector3(x, 0, z));
			normals.set(i, Vector3(0, 1, 0));
			tangents.set(i * 4 + 0, 1.0f);
			tangents.set(i * 4 + 1, 0.0f);
			tangents.set(i * 4 + 2, 0.0f);
			tangents.set(i * 4 + 3, 1.0f);
			// White, so that the vertex shader's COLOR is exactly the instance
			// color it reads the patch from.
			colors.set(i, Color(1, 1, 1, 1));
		}
	}

	// Every quad split along the diagonal from its +X corner to its +Z corner,
	// like the heightmap's own quads everywhere else they are triangulated
	// (LandscapeQuadtree's error, LandscapeSpline3D's terrain sampler).
	PackedInt32Array indices;
	indices.resize(quads * quads * 6);
	int n = 0;
	for (int z = 0; z < quads; z++) {
		for (int x = 0; x < quads; x++) {
			const int a = z * side + x;
			const int b = a + 1;
			const int c = a + side;
			const int d = c + 1;
			indices.set(n++, a);
			indices.set(n++, b);
			indices.set(n++, c);
			indices.set(n++, b);
			indices.set(n++, d);
			indices.set(n++, c);
		}
	}

	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = vertices;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_TANGENT] = tangents;
	arrays[Mesh::ARRAY_COLOR] = colors;
	arrays[Mesh::ARRAY_INDEX] = indices;

	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	patch_mesh = mesh;
}

void Landscape3D::_ensure_material() {
	if (material.is_valid()) {
		return;
	}
	material.instantiate();
	material->set_shader(shader);
	writer_material.instantiate();
	writer_material->set_shader(shader);
	writer_material->set_shader_parameter(SNAME("rvt_mode"), 2);
	_update_pom_params();
	_update_micro_detail_params();
	material->set_shader_parameter(SNAME("debug_view"), int(debug_view));
}

void Landscape3D::_set_shader_parameter(const StringName &p_name, const Variant &p_value) {
	material->set_shader_parameter(p_name, p_value);
	writer_material->set_shader_parameter(p_name, p_value);
}

bool Landscape3D::_is_virtual_texture_active() const {
	return virtual_texture_enabled && terrain_data.is_valid() && RS::get_singleton()->is_virtual_texturing_supported();
}

void Landscape3D::_update_virtual_texture() {
	_ensure_material();
	if (!_is_virtual_texture_active() || quadtree.is_empty()) {
		_free_virtual_texture();
		return;
	}

	// Pages are square, and the virtual texture a power of two of them a side: the finest that keeps
	// a texel no larger than virtual_texture_texel_size, within what a page table can address.
	const int page_size = RSE::VIRTUAL_TEXTURE_PAGE_SIZE;
	const float extent = float(terrain_data->get_resolution() - 1) * terrain_data->get_vertex_spacing();
	const int64_t texels = int64_t(Math::ceil(extent / MAX(virtual_texture_texel_size, 0.001f)));
	const uint32_t pages = CLAMP(uint32_t(Math::next_power_of_2(uint32_t(MAX(int64_t(1), (texels + page_size - 1) / page_size)))), 1u, 1024u);
	const int size = int(pages) * page_size;

	RenderingServer *rs = RS::get_singleton();
	if (virtual_texture.is_null() || size != virtual_texture_size) {
		_free_virtual_texture();
		virtual_texture = rs->texture_virtual_create(size, size, RSE::VIRTUAL_TEXTURE_RUNTIME, Ref<Image>());
		virtual_texture_size = size;
	}
	material->set_shader_parameter(SNAME("rvt_albedo"), virtual_texture);
	material->set_shader_parameter(SNAME("rvt_data"), virtual_texture);
	material->set_shader_parameter(SNAME("rvt_mode"), 1);
	material->set_shader_parameter(SNAME("rvt_near_distance"), virtual_texture_near_distance);
	_update_writer_instance();
	_update_virtual_texture_volume();
}

void Landscape3D::_free_virtual_texture() {
	RenderingServer *rs = RS::get_singleton();
	if (writer_instance.is_valid()) {
		rs->free_rid(writer_instance);
		writer_instance = RID();
	}
	if (virtual_texture.is_valid()) {
		rs->free_rid(virtual_texture);
		virtual_texture = RID();
	}
	virtual_texture_size = 0;
	if (material.is_valid()) {
		material->set_shader_parameter(SNAME("rvt_mode"), 0);
		material->set_shader_parameter(SNAME("rvt_albedo"), Variant());
		material->set_shader_parameter(SNAME("rvt_data"), Variant());
	}
}

void Landscape3D::_update_writer_instance() {
	RenderingServer *rs = RS::get_singleton();
	if (virtual_texture.is_null()) {
		if (writer_instance.is_valid()) {
			rs->free_rid(writer_instance);
			writer_instance = RID();
		}
		return;
	}

	{
		MutexLock lock(patch_mesh_mutex);
		if (writer_mesh.is_null()) {
			// One quad over the whole terrain (the shader scales it), facing up like the patches.
			PackedVector3Array vertices = { Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(0, 0, 1), Vector3(1, 0, 1) };
			PackedVector3Array normals = { Vector3(0, 1, 0), Vector3(0, 1, 0), Vector3(0, 1, 0), Vector3(0, 1, 0) };
			PackedFloat32Array tangents = { 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1 };
			PackedColorArray colors = { Color(1, 1, 1), Color(1, 1, 1), Color(1, 1, 1), Color(1, 1, 1) };
			PackedInt32Array indices = { 0, 1, 2, 1, 3, 2 };
			Array arrays;
			arrays.resize(Mesh::ARRAY_MAX);
			arrays[Mesh::ARRAY_VERTEX] = vertices;
			arrays[Mesh::ARRAY_NORMAL] = normals;
			arrays[Mesh::ARRAY_TANGENT] = tangents;
			arrays[Mesh::ARRAY_COLOR] = colors;
			arrays[Mesh::ARRAY_INDEX] = indices;
			Ref<ArrayMesh> mesh;
			mesh.instantiate();
			mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
			writer_mesh = mesh;
		}
	}

	if (writer_instance.is_null()) {
		writer_instance = rs->instance_create2(writer_mesh->get_rid(), RID());
		rs->instance_geometry_set_material_override(writer_instance, writer_material->get_rid());
		rs->instance_geometry_set_cast_shadows_setting(writer_instance, RSE::SHADOW_CASTING_SETTING_OFF);
	}
	// Drawn only into the virtual texture, never by a camera.
	rs->instance_geometry_set_virtual_texture_layers(writer_instance, virtual_texture_layers, false);
	const RID scenario = (is_inside_tree() && get_world_3d().is_valid()) ? get_world_3d()->get_scenario() : RID();
	rs->instance_set_scenario(writer_instance, scenario);
	rs->instance_set_transform(writer_instance, _get_safe_global_transform());
	rs->instance_set_custom_aabb(writer_instance, get_aabb());
	rs->instance_set_visible(writer_instance, is_inside_tree() && is_visible_in_tree());
}

Transform3D Landscape3D::_get_virtual_texture_local_volume() const {
	// The terrain, and some room above it for whatever else draws into the texture (roads lie a
	// little over the ground).
	const AABB aabb = get_aabb();
	const float extent = float(terrain_data->get_resolution() - 1) * terrain_data->get_vertex_spacing();
	const float margin = MAX(aabb.size.y * 0.05f, 1.0f);
	return Transform3D(Basis::from_scale(Vector3(extent, aabb.size.y + margin * 2.0f, extent)), Vector3(0, aabb.position.y - margin, 0));
}

void Landscape3D::_update_virtual_texture_volume() {
	if (virtual_texture.is_null() || terrain_data.is_null() || quadtree.is_empty()) {
		return;
	}
	const Transform3D local = _get_virtual_texture_local_volume();
	const RID scenario = (is_inside_tree() && get_world_3d().is_valid()) ? get_world_3d()->get_scenario() : RID();
	RS::get_singleton()->texture_virtual_set_runtime_volume(virtual_texture, scenario, _get_safe_global_transform() * local, virtual_texture_layers);
	// The writer lies at the bottom of the volume, under everything else drawn into it.
	writer_material->set_shader_parameter(SNAME("rvt_writer_height"), local.origin.y + local.basis.get_column(1).y * 0.001f);
	if (writer_instance.is_valid()) {
		RS::get_singleton()->instance_set_custom_aabb(writer_instance, get_aabb());
	}
}

void Landscape3D::_invalidate_virtual_texture(const Rect2i &p_samples) {
	if (virtual_texture.is_null() || terrain_data.is_null()) {
		return;
	}
	// A sample's normal reaches its neighbors, and filtering one more.
	const Rect2i samples = p_samples.grow(2);
	const float quads = float(MAX(terrain_data->get_resolution() - 1, 1));
	RS::get_singleton()->texture_virtual_invalidate(virtual_texture, Rect2(samples.position.x / quads, samples.position.y / quads, samples.size.x / quads, samples.size.y / quads));
}

void Landscape3D::_invalidate_virtual_texture_all() {
	if (virtual_texture.is_valid()) {
		RS::get_singleton()->texture_virtual_invalidate(virtual_texture, Rect2(0, 0, 1, 1));
	}
}

bool Landscape3D::_is_gpu_lod_active() {
	if (gpu_quadtree.has_failed()) {
		gpu_lod_unavailable = true;
	}
	return gpu_lod_enabled && !gpu_lod_unavailable && LandscapeGPUQuadtree::is_supported();
}

void Landscape3D::_allocate_multimeshes(bool p_indirect) {
	_ensure_patch_mesh();
	RenderingServer *rs = RS::get_singleton();

	for (int i = 0; i < DRAW_MAX; i++) {
		// Fresh ones rather than reallocated: a MultiMesh only builds its
		// indirect draw command when it is given a mesh, which it skips when
		// that is the mesh it already has.
		if (multimeshes[i].is_valid()) {
			rs->free_rid(multimeshes[i]);
		}
		multimeshes[i] = rs->multimesh_create();
		if (p_indirect) {
			rs->multimesh_allocate_data(multimeshes[i], gpu_quadtree.get_capacity(), RSE::MULTIMESH_TRANSFORM_3D, true, true, true);
		}
		multimesh_capacity[i] = p_indirect ? int(gpu_quadtree.get_capacity()) : 0;
		rs->multimesh_set_mesh(multimeshes[i], patch_mesh->get_rid());
		if (draw_instances[i].is_valid()) {
			rs->instance_set_base(draw_instances[i], multimeshes[i]);
		}
	}
	multimeshes_indirect = p_indirect;

	if (p_indirect) {
		gpu_quadtree.set_outputs(multimeshes[DRAW_CAMERA], multimeshes[DRAW_SHADOW]);
	} else {
		gpu_quadtree.release();
	}
	_update_draw_aabb();
	lod_dirty = true;
}

void Landscape3D::_ensure_draw_instances() {
	_ensure_material();
	if (multimeshes[DRAW_CAMERA].is_null()) {
		_allocate_multimeshes(_is_gpu_lod_active());
	}

	RenderingServer *rs = RS::get_singleton();
	for (int i = 0; i < DRAW_MAX; i++) {
		if (draw_instances[i].is_valid()) {
			continue;
		}
		draw_instances[i] = rs->instance_create2(multimeshes[i], RID());
		rs->instance_geometry_set_material_override(draw_instances[i], material->get_rid());
		// The instance covers the whole terrain; only the patches the
		// selection keeps are drawn, so testing it as a whole against the
		// occlusion buffer can never help.
		rs->instance_geometry_set_flag(draw_instances[i], RSE::INSTANCE_FLAG_IGNORE_OCCLUSION_CULLING, true);
	}
	_update_draw_instances();
}

void Landscape3D::_free_draw_instances() {
	RenderingServer *rs = RS::get_singleton();
	for (int i = 0; i < DRAW_MAX; i++) {
		if (draw_instances[i].is_valid()) {
			rs->free_rid(draw_instances[i]);
			draw_instances[i] = RID();
		}
		if (multimeshes[i].is_valid()) {
			rs->free_rid(multimeshes[i]);
			multimeshes[i] = RID();
		}
		multimesh_capacity[i] = 0;
	}
	gpu_quadtree.release();
	multimeshes_indirect = false;
}

// The camera's list casts no shadows; the shadow list is its own MultiMesh
// drawn only into shadow maps, since it also has to hold patches outside the
// camera's view that shade what is inside it. Both show the same selection,
// so the two never disagree about where the ground is.
void Landscape3D::_update_draw_instances() {
	RenderingServer *rs = RS::get_singleton();
	const RID scenario = (is_inside_tree() && get_world_3d().is_valid()) ? get_world_3d()->get_scenario() : RID();
	const Transform3D xform = _get_safe_global_transform();
	const bool visible = is_inside_tree() && is_visible_in_tree();

	for (int i = 0; i < DRAW_MAX; i++) {
		if (draw_instances[i].is_null()) {
			continue;
		}
		rs->instance_set_scenario(draw_instances[i], scenario);
		rs->instance_set_transform(draw_instances[i], xform);
		if (i == DRAW_CAMERA) {
			rs->instance_set_visible(draw_instances[i], visible && cast_shadow != GeometryInstance3D::SHADOW_CASTING_SETTING_SHADOWS_ONLY);
			rs->instance_geometry_set_cast_shadows_setting(draw_instances[i], RSE::SHADOW_CASTING_SETTING_OFF);
			rs->instance_geometry_set_flag(draw_instances[i], RSE::INSTANCE_FLAG_USE_BAKED_LIGHT, gi_mode == GeometryInstance3D::GI_MODE_STATIC);
			rs->instance_geometry_set_flag(draw_instances[i], RSE::INSTANCE_FLAG_USE_DYNAMIC_GI, gi_mode == GeometryInstance3D::GI_MODE_DYNAMIC);
		} else {
			rs->instance_set_visible(draw_instances[i], visible && cast_shadow != GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
			rs->instance_geometry_set_cast_shadows_setting(draw_instances[i], RSE::SHADOW_CASTING_SETTING_SHADOWS_ONLY);
		}
	}
}

void Landscape3D::_update_draw_aabb() {
	if (quadtree.is_empty()) {
		return;
	}
	const AABB aabb = get_aabb();
	for (int i = 0; i < DRAW_MAX; i++) {
		if (multimeshes[i].is_valid()) {
			RS::get_singleton()->multimesh_set_custom_aabb(multimeshes[i], aabb);
		}
	}
}

void Landscape3D::_connect_frame_hook() {
	RenderingServer *rs = RS::get_singleton();
	if (frame_hook_connected || rs == nullptr) {
		return;
	}
	// Right before the frame is drawn, once every node has moved: the camera
	// the patches are selected for is where it will be drawn from, rather than
	// where it was a frame ago (which would open gaps at the edges of the view
	// while it turns).
	rs->connect(SNAME("frame_pre_draw"), callable_mp(this, &Landscape3D::_on_frame_pre_draw));
	frame_hook_connected = true;
}

void Landscape3D::_disconnect_frame_hook() {
	if (!frame_hook_connected) {
		return;
	}
	RS::get_singleton()->disconnect(SNAME("frame_pre_draw"), callable_mp(this, &Landscape3D::_on_frame_pre_draw));
	frame_hook_connected = false;
}

void Landscape3D::_on_frame_pre_draw() {
	if (!is_inside_tree() || quadtree.is_empty() || multimeshes[DRAW_CAMERA].is_null()) {
		return;
	}

	height_texture.flush();
	gradient_texture.flush();
	hole_texture.flush();
	weight_texture.flush();

	if (!is_visible_in_tree()) {
		return;
	}

	const bool gpu = _is_gpu_lod_active();
	if (gpu != multimeshes_indirect) {
		// Either switched in the inspector, or the compute shaders turned out
		// not to build on this device.
		_allocate_multimeshes(gpu);
		if (gpu) {
			gpu_quadtree.set_nodes(quadtree);
		}
	} else if (gpu && gpu_quadtree.get_wanted_capacity() > gpu_quadtree.get_capacity()) {
		// A selection did not fit: until the MultiMeshes are this large, the
		// GPU makes do with a coarser one.
		gpu_quadtree.set_capacity(gpu_quadtree.get_wanted_capacity());
		_allocate_multimeshes(true);
	}

	LandscapeQuadtree::SelectParams params;
	if (!_make_lod_params(params)) {
		return;
	}
	if (!lod_dirty && lod_params_equal(params, last_lod_params)) {
		// Nothing moved: last frame's patches are still in place.
		return;
	}
	last_lod_params = params;
	lod_dirty = false;

	material->set_shader_parameter(SNAME("lod_camera_position"), params.camera_position);
	// Displacement is sampled no finer than what micro_detail_triangle_size
	// pixels cover at each vertex's distance, which an orthogonal projection
	// makes the same everywhere.
	const float min_footprint = terrain_data->get_vertex_spacing() / float(1 << micro_detail_levels);
	const float pixel_footprint = micro_detail_triangle_size / MAX(params.pixel_scale, 0.0001f);
	material->set_shader_parameter(SNAME("displacement_footprint_scale"), params.orthogonal ? 0.0f : pixel_footprint);
	material->set_shader_parameter(SNAME("displacement_min_footprint"), params.orthogonal ? MAX(min_footprint, pixel_footprint) : min_footprint);

	if (gpu) {
		gpu_quadtree.dispatch(quadtree, params);
	} else {
		quadtree.select(params, cpu_selection);
		_write_cpu_instances(DRAW_CAMERA, cpu_selection.visible);
		_write_cpu_instances(DRAW_SHADOW, cpu_selection.shadow);
	}
}

bool Landscape3D::_make_lod_params(LandscapeQuadtree::SelectParams &r_params) {
	if (quadtree.is_empty() || terrain_data.is_null()) {
		return false;
	}

	const Transform3D global = _get_safe_global_transform();
	const Transform3D to_local = global.affine_inverse();

	r_params.pixel_error = lod_pixel_error;
	r_params.max_quad_pixels = lod_max_quad_pixels;
	r_params.min_level = -micro_detail_levels;
	const bool micro = micro_detail_distance > 0.0f && (micro_detail_levels > 0 || displacement_bound > 0.0f);
	r_params.micro_distance = micro ? micro_detail_distance : 0.0f;
	r_params.micro_quad_pixels = micro_detail_triangle_size;
	r_params.displacement_bound = micro ? displacement_bound : 0.0f;
	r_params.shadows = cast_shadow != GeometryInstance3D::SHADOW_CASTING_SETTING_OFF;
	r_params.shadow_distance = shadow_distance;

	Camera3D *camera = is_inside_tree() ? FoliageGPUCuller::resolve_culling_camera(this, lod_camera) : nullptr;
	if (camera == nullptr || camera->get_viewport() == nullptr) {
		// Nothing to look through: select as if from high above the middle of
		// the terrain, which gives an even, coarse cover.
		const LandscapeQuadtree::NodeBounds root = quadtree.get_node_bounds(quadtree.get_top_level(), 0, 0);
		const Vector3 size = root.max - root.min;
		r_params.camera_position = (root.min + root.max) * 0.5 + Vector3(0, MAX(size.x, size.z), 0);
		r_params.orthogonal = false;
		r_params.pixel_scale = 500.0f;
		r_params.frustum_culling = false;
		return true;
	}

	r_params.camera_position = to_local.xform(camera->get_camera_transform().origin);
	r_params.orthogonal = camera->get_projection() == Camera3D::PROJECTION_ORTHOGONAL;

	// Pixels one unit spans at a distance of one unit: half the viewport's
	// height over the tangent of half the vertical field of view, which is
	// what the projection's Y scale holds whichever axis the camera keeps.
	// Orthogonal projections have no distance, and hold pixels per unit.
	const Projection projection = camera->get_camera_projection();
	const float viewport_height = MAX(camera->get_viewport()->get_visible_rect().size.y, (real_t)1.0);
	r_params.pixel_scale = 0.5f * viewport_height * float(projection.columns[1][1]);
	if (r_params.orthogonal) {
		// Errors are measured in terrain space; a scaled terrain shows them
		// that much larger.
		const Vector3 scale = global.basis.get_scale_abs();
		r_params.pixel_scale *= float(MAX(scale.x, MAX(scale.y, scale.z)));
	}

	// The editor draws the scene through several viewports at once, and
	// patches culled for one would leave holes in the others.
	r_params.frustum_culling = frustum_culling && !Engine::get_singleton()->is_editor_hint();
	if (r_params.frustum_culling) {
		const Vector<Plane> planes = camera->get_frustum();
		if (planes.size() >= 6) {
			for (int i = 0; i < 6; i++) {
				r_params.frustum[i] = to_local.xform(planes[i]);
			}
		} else {
			r_params.frustum_culling = false;
		}
	}
	return true;
}

void Landscape3D::_write_cpu_instances(DrawList p_list, const LocalVector<LandscapeQuadtree::Patch> &p_patches) {
	RenderingServer *rs = RS::get_singleton();
	const int count = int(p_patches.size());
	if (count > multimesh_capacity[p_list]) {
		const int capacity = int(Math::next_power_of_2(uint32_t(MAX(count, 64))));
		rs->multimesh_allocate_data(multimeshes[p_list], capacity, RSE::MULTIMESH_TRANSFORM_3D, true, true, false);
		multimesh_capacity[p_list] = capacity;
	}

	const int capacity = multimesh_capacity[p_list];
	if (capacity > 0) {
		// The buffer always holds every instance the MultiMesh has room for;
		// the ones past the visible count are simply not drawn.
		cpu_instance_data.resize(capacity * LandscapeQuadtree::INSTANCE_FLOATS);
		float *instances = cpu_instance_data.ptrw();
		for (int i = 0; i < count; i++) {
			LandscapeQuadtree::write_instance(p_patches[i], instances + i * LandscapeQuadtree::INSTANCE_FLOATS);
		}
		rs->multimesh_set_buffer(multimeshes[p_list], cpu_instance_data);
	}
	rs->multimesh_set_visible_instances(multimeshes[p_list], count);
}

void Landscape3D::_clear_terrain() {
	_free_virtual_texture();
	quadtree.clear();
	height_texture.free();
	gradient_texture.free();
	hole_texture.free();
	weight_texture.free();
	gradient_image.unref();
	_free_draw_instances();
	_clear_occluders();
}

void Landscape3D::_rebuild_terrain() {
	_ensure_material();
	if (terrain_data.is_null()) {
		_clear_terrain();
		return;
	}

	const Ref<Image> heightmap = terrain_data->get_heightmap_image();
	const Ref<Image> hole_map = terrain_data->get_hole_map_image();
	ERR_FAIL_COND(heightmap.is_null() || hole_map.is_null());
	const int resolution = terrain_data->get_resolution();

	quadtree.build(reinterpret_cast<const float *>(heightmap->ptr()), hole_map->ptr(), resolution, terrain_data->get_vertex_spacing());
	_rebuild_gradients();

	Vector<Ref<Image>> images;
	images.push_back(heightmap);
	height_texture.create(images, false);
	images.write[0] = hole_map;
	hole_texture.create(images, false);
	images.write[0] = gradient_image;
	gradient_texture.create(images, false);
	// Uploaded afresh: the weights may have changed along with everything else.
	weight_texture.free();
	_rebuild_weight_texture();

	_ensure_draw_instances();
	if (multimeshes_indirect) {
		gpu_quadtree.set_nodes(quadtree);
	}

	_update_material_params();
	_update_micro_detail_params();
	_update_draw_aabb();
	_rebuild_all_occluders();
	_update_virtual_texture();
	_invalidate_virtual_texture_all();
	lod_dirty = true;
}

void Landscape3D::_refresh_region(const Rect2i &p_samples, bool p_heights_changed, bool p_holes_changed) {
	Vector<Rect2i> regions;
	regions.push_back(p_samples);
	_refresh_regions(regions, p_heights_changed, p_holes_changed);
}

void Landscape3D::_refresh_regions(const Vector<Rect2i> &p_regions, bool p_heights_changed, bool p_holes_changed) {
	if (terrain_data.is_null() || quadtree.is_empty()) {
		return;
	}

	const Rect2i full = _get_full_region();
	const Ref<Image> heightmap = terrain_data->get_heightmap_image();
	const Ref<Image> hole_map = terrain_data->get_hole_map_image();
	const float *heights = reinterpret_cast<const float *>(heightmap->ptr());
	const uint8_t *holes = hole_map->ptr();

	LocalVector<Vector2i> changed_nodes;
	HashSet<Vector2i> blocks;
	for (const Rect2i &samples : p_regions) {
		const Rect2i region = samples.intersection(full);
		if (region.size.x <= 0 || region.size.y <= 0) {
			continue;
		}
		quadtree.update(heights, holes, region, multimeshes_indirect ? &changed_nodes : nullptr);
		if (p_heights_changed) {
			height_texture.update(0, heightmap, region);
			// A height's slope reaches one sample past it on every side.
			_compute_gradients(region);
			gradient_texture.update(0, gradient_image, region.grow(1));
		}
		if (p_holes_changed) {
			hole_texture.update(0, hole_map, region);
		}
		_add_blocks_in_region(region, blocks);
		_invalidate_virtual_texture(region);
	}

	if (multimeshes_indirect) {
		gpu_quadtree.update_nodes(quadtree, changed_nodes);
	}
	for (const Vector2i &block : blocks) {
		_rebuild_occluder_block(block);
	}
	_update_draw_aabb();
	_update_virtual_texture_volume();
	lod_dirty = true;
}

void Landscape3D::_rebuild_gradients() {
	const int resolution = terrain_data->get_resolution();
	gradient_image = Image::create_empty(resolution, resolution, true, Image::FORMAT_RGH);
	_compute_gradients(Rect2i(0, 0, resolution, resolution));
}

// Central differences, one-sided along the terrain's edges, then every
// mipmap texel above the changed ones as the average of the four below it.
void Landscape3D::_compute_gradients(const Rect2i &p_samples) {
	const int resolution = terrain_data->get_resolution();
	if (gradient_image.is_null() || gradient_image->get_width() != resolution) {
		return;
	}
	const Rect2i region = p_samples.grow(1).intersection(Rect2i(0, 0, resolution, resolution));
	if (region.size.x <= 0 || region.size.y <= 0) {
		return;
	}

	const float spacing = terrain_data->get_vertex_spacing();
	const float *heights = reinterpret_cast<const float *>(terrain_data->get_heightmap_image()->ptr());
	uint8_t *pixels = gradient_image->ptrw();
	uint16_t *base = reinterpret_cast<uint16_t *>(pixels);
	const Point2i end = region.get_end();
	for (int z = region.position.y; z < end.y; z++) {
		const int z0 = MAX(z - 1, 0);
		const int z1 = MIN(z + 1, resolution - 1);
		for (int x = region.position.x; x < end.x; x++) {
			const int x0 = MAX(x - 1, 0);
			const int x1 = MIN(x + 1, resolution - 1);
			const float dx = x1 > x0 ? (heights[z * resolution + x1] - heights[z * resolution + x0]) / (float(x1 - x0) * spacing) : 0.0f;
			const float dz = z1 > z0 ? (heights[z1 * resolution + x] - heights[z0 * resolution + x]) / (float(z1 - z0) * spacing) : 0.0f;
			base[(z * resolution + x) * 2 + 0] = Math::make_half_float(dx);
			base[(z * resolution + x) * 2 + 1] = Math::make_half_float(dz);
		}
	}

	int x0 = region.position.x;
	int z0 = region.position.y;
	int x1 = end.x - 1;
	int z1 = end.y - 1;
	for (int mipmap = 1; mipmap <= gradient_image->get_mipmap_count(); mipmap++) {
		int64_t src_offset = 0;
		int64_t src_size = 0;
		int src_width = 0;
		int src_height = 0;
		int64_t dst_offset = 0;
		int64_t dst_size = 0;
		int dst_width = 0;
		int dst_height = 0;
		gradient_image->get_mipmap_offset_size_and_dimensions(mipmap - 1, src_offset, src_size, src_width, src_height);
		gradient_image->get_mipmap_offset_size_and_dimensions(mipmap, dst_offset, dst_size, dst_width, dst_height);
		const uint16_t *src = reinterpret_cast<const uint16_t *>(pixels + src_offset);
		uint16_t *dst = reinterpret_cast<uint16_t *>(pixels + dst_offset);

		x0 >>= 1;
		z0 >>= 1;
		x1 = MIN(x1 >> 1, dst_width - 1);
		z1 = MIN(z1 >> 1, dst_height - 1);
		for (int z = z0; z <= z1; z++) {
			for (int x = x0; x <= x1; x++) {
				float sum_x = 0.0f;
				float sum_z = 0.0f;
				for (int j = 0; j < 2; j++) {
					const int sz = MIN(z * 2 + j, src_height - 1);
					for (int i = 0; i < 2; i++) {
						const int sx = MIN(x * 2 + i, src_width - 1);
						sum_x += Math::half_to_float(src[(sz * src_width + sx) * 2 + 0]);
						sum_z += Math::half_to_float(src[(sz * src_width + sx) * 2 + 1]);
					}
				}
				dst[(z * dst_width + x) * 2 + 0] = Math::make_half_float(sum_x * 0.25f);
				dst[(z * dst_width + x) * 2 + 1] = Math::make_half_float(sum_z * 0.25f);
			}
		}
	}
}

void Landscape3D::_rebuild_weight_texture() {
	_ensure_material();
	if (terrain_data.is_null()) {
		weight_texture.free();
		return;
	}

	// Only as many weight maps as there are layers to weigh, not all
	// TerrainData::WEIGHT_MAP_COUNT of them: at 4097 x 4097 each one is 64 MB.
	// But at least two, since the rendering server cannot wrap a texture
	// array of a single layer around a RenderingDevice texture.
	const int layer_count = CLAMP(int(layers.size()), 0, TerrainData::MAX_LAYERS);
	const int group_count = CLAMP((layer_count + TerrainData::LAYERS_PER_WEIGHT_MAP - 1) / TerrainData::LAYERS_PER_WEIGHT_MAP, 2, TerrainData::WEIGHT_MAP_COUNT);
	if (weight_texture.is_valid() && weight_texture.get_layer_count() == group_count && weight_texture.get_size() == Size2i(terrain_data->get_resolution(), terrain_data->get_resolution())) {
		return;
	}

	Vector<Ref<Image>> images;
	for (int g = 0; g < group_count; g++) {
		images.push_back(terrain_data->get_weight_map_image(g));
	}
	weight_texture.create(images, true);
	_set_shader_parameter(SNAME("weight_array"), weight_texture.get_rid());
}

void Landscape3D::_upload_weight_region(const Rect2i &p_region, int p_first_layer, int p_layer_count) {
	if (terrain_data.is_null() || !weight_texture.is_valid() || p_layer_count <= 0) {
		return;
	}
	const int first_group = MAX(p_first_layer, 0) / TerrainData::LAYERS_PER_WEIGHT_MAP;
	const int last_group = MIN((p_first_layer + p_layer_count - 1) / TerrainData::LAYERS_PER_WEIGHT_MAP, weight_texture.get_layer_count() - 1);
	for (int g = first_group; g <= last_group; g++) {
		weight_texture.update(g, terrain_data->get_weight_map_image(g), p_region);
	}
	_invalidate_virtual_texture(p_region);
}

void Landscape3D::_update_material_params() {
	_ensure_material();
	if (terrain_data.is_null()) {
		return;
	}
	_set_shader_parameter(SNAME("heightmap"), height_texture.get_rid());
	_set_shader_parameter(SNAME("gradient_map"), gradient_texture.get_rid());
	_set_shader_parameter(SNAME("hole_map"), hole_texture.get_rid());
	_set_shader_parameter(SNAME("weight_array"), weight_texture.get_rid());
	_set_shader_parameter(SNAME("terrain_quads"), terrain_data->get_resolution() - 1);
	_set_shader_parameter(SNAME("vertex_spacing"), terrain_data->get_vertex_spacing());
}

void Landscape3D::_update_micro_detail_params() {
	if (material.is_null()) {
		return;
	}
	material->set_shader_parameter(SNAME("displacement_enabled"), micro_detail_distance > 0.0f && displacement_bound > 0.0f);
	material->set_shader_parameter(SNAME("displacement_fade_start"), micro_detail_distance * 0.75f);
	material->set_shader_parameter(SNAME("displacement_fade_end"), micro_detail_distance);
	lod_dirty = true;
}

void Landscape3D::_update_pom_params() {
	if (material.is_null()) {
		return;
	}
	material->set_shader_parameter(SNAME("pom_enabled"), pom_enabled);
	material->set_shader_parameter(SNAME("pom_min_layers"), pom_min_layers);
	material->set_shader_parameter(SNAME("pom_max_layers"), pom_max_layers);
	material->set_shader_parameter(SNAME("pom_flip"), Vector2(pom_flip_tangent ? -1.0f : 1.0f, pom_flip_binormal ? -1.0f : 1.0f));
	material->set_shader_parameter(SNAME("pom_self_shadow_enabled"), pom_self_shadow_enabled);
	material->set_shader_parameter(SNAME("pom_shadow_steps"), pom_shadow_steps);
	material->set_shader_parameter(SNAME("pom_shadow_strength"), pom_shadow_strength);
	material->set_shader_parameter(SNAME("pom_shadow_light_direction"), pom_shadow_light_direction);
	material->set_shader_parameter(SNAME("pom_fade_start"), pom_fade_start);
	material->set_shader_parameter(SNAME("pom_fade_end"), pom_fade_end);
}

void Landscape3D::_rebuild_layer_textures() {
	_ensure_material();

	const int layer_count = CLAMP(int(layers.size()), 0, TerrainData::MAX_LAYERS);
	const int array_layers = MAX(layer_count, 1);

	Vector<Ref<Image>> albedo_images;
	Vector<Ref<Image>> normal_images;
	Vector<Ref<Image>> orm_images;
	Vector<Ref<Image>> height_images;
	albedo_images.resize(array_layers);
	normal_images.resize(array_layers);
	orm_images.resize(array_layers);
	height_images.resize(array_layers);

	PackedFloat32Array uv_scales;
	uv_scales.resize(TerrainData::MAX_LAYERS);
	PackedColorArray albedo_colors;
	albedo_colors.resize(TerrainData::MAX_LAYERS);
	PackedFloat32Array roughness_values;
	roughness_values.resize(TerrainData::MAX_LAYERS);
	PackedFloat32Array specular_values;
	specular_values.resize(TerrainData::MAX_LAYERS);
	PackedFloat32Array ao_strength_values;
	ao_strength_values.resize(TerrainData::MAX_LAYERS);
	PackedFloat32Array normal_strength_values;
	normal_strength_values.resize(TerrainData::MAX_LAYERS);
	PackedFloat32Array heightmap_scale_values;
	heightmap_scale_values.resize(TerrainData::MAX_LAYERS);
	PackedFloat32Array height_min_values;
	height_min_values.resize(TerrainData::MAX_LAYERS);
	PackedFloat32Array height_max_values;
	height_max_values.resize(TerrainData::MAX_LAYERS);
	// Godot's Variant system has no packed bool array type, so these two
	// per-layer switches are encoded as 0.0/1.0 (checked as > 0.5 in the
	// shader).
	PackedFloat32Array pom_enabled_values;
	pom_enabled_values.resize(TerrainData::MAX_LAYERS);
	PackedFloat32Array triplanar_values;
	triplanar_values.resize(TerrainData::MAX_LAYERS);
	PackedFloat32Array triplanar_sharpness_values;
	triplanar_sharpness_values.resize(TerrainData::MAX_LAYERS);
	PackedFloat32Array displacement_values;
	displacement_values.resize(TerrainData::MAX_LAYERS);
	for (int i = 0; i < TerrainData::MAX_LAYERS; i++) {
		uv_scales.write[i] = 1.0f;
		albedo_colors.write[i] = Color(1, 1, 1);
		roughness_values.write[i] = 1.0f;
		specular_values.write[i] = 0.5f;
		ao_strength_values.write[i] = 1.0f;
		normal_strength_values.write[i] = 1.0f;
		heightmap_scale_values.write[i] = 5.0f;
		height_min_values.write[i] = 0.0f;
		height_max_values.write[i] = 1.0f;
		pom_enabled_values.write[i] = 0.0f;
		triplanar_values.write[i] = 0.0f;
		triplanar_sharpness_values.write[i] = 1.0f;
		displacement_values.write[i] = 0.0f;
	}

	displacement_bound = 0.0f;

	auto source_image = [](const Ref<Texture2D> &p_texture) -> Ref<Image> {
		return p_texture.is_valid() ? p_texture->get_image() : Ref<Image>();
	};
	for (int i = 0; i < array_layers; i++) {
		Ref<TerrainLayer> layer;
		if (i < layers.size()) {
			layer = layers[i];
		}

		Ref<Texture2D> albedo_tex = layer.is_valid() ? layer->get_albedo_texture() : Ref<Texture2D>();
		Ref<Texture2D> normal_tex = layer.is_valid() ? layer->get_normal_texture() : Ref<Texture2D>();
		Ref<Texture2D> orm_tex = layer.is_valid() ? layer->get_orm_texture() : Ref<Texture2D>();
		Ref<Texture2D> height_tex = layer.is_valid() ? layer->get_height_texture() : Ref<Texture2D>();

		albedo_images.write[i] = source_image(albedo_tex);
		normal_images.write[i] = source_image(normal_tex);
		orm_images.write[i] = source_image(orm_tex);
		height_images.write[i] = source_image(height_tex);

		if (i < TerrainData::MAX_LAYERS) {
			uv_scales.write[i] = layer.is_valid() ? layer->get_uv_scale() : 1.0f;
			albedo_colors.write[i] = layer.is_valid() ? layer->get_albedo_color() : Color(1, 1, 1);
			roughness_values.write[i] = layer.is_valid() ? layer->get_roughness() : 1.0f;
			specular_values.write[i] = layer.is_valid() ? layer->get_specular() : 0.5f;
			ao_strength_values.write[i] = layer.is_valid() ? layer->get_ao_strength() : 1.0f;
			normal_strength_values.write[i] = layer.is_valid() ? layer->get_normal_strength() : 1.0f;
			heightmap_scale_values.write[i] = layer.is_valid() ? layer->get_heightmap_scale() : 5.0f;
			height_min_values.write[i] = layer.is_valid() ? layer->get_height_min() : 0.0f;
			height_max_values.write[i] = layer.is_valid() ? layer->get_height_max() : 1.0f;
			pom_enabled_values.write[i] = (layer.is_valid() && layer->is_pom_enabled()) ? 1.0f : 0.0f;
			triplanar_values.write[i] = (layer.is_valid() && layer->is_triplanar_enabled()) ? 1.0f : 0.0f;
			triplanar_sharpness_values.write[i] = layer.is_valid() ? layer->get_triplanar_sharpness() : 1.0f;
			// Displacement needs something to displace by: a layer without a
			// height texture would only lift its whole area by a constant.
			if (layer.is_valid() && height_tex.is_valid() && i < layer_count) {
				displacement_values.write[i] = layer->get_displacement();
				displacement_bound = MAX(displacement_bound, Math::abs(layer->get_displacement()) * 0.5f);
			}
		}
	}

	albedo_array = build_layer_texture_array(albedo_images, Color(0.6, 0.6, 0.6, 1.0), Image::FORMAT_RGBA8, false, layer_texture_size_limit);
	// Mipmaps are renormalized: averaging two normals shortens the result, and
	// a normal map whose vectors are not unit length reads as a flattened,
	// washed-out surface at distance.
	normal_array = build_layer_texture_array(normal_images, Color(0.5, 0.5, 1.0, 1.0), Image::FORMAT_RGBA8, true, layer_texture_size_limit);
	orm_array = build_layer_texture_array(orm_images, Color(1.0, 0.5, 0.0, 1.0), Image::FORMAT_RGBA8, false, layer_texture_size_limit);
	// Single-channel, unlike the other three: POM and displacement only ever
	// read one value per sample. White = a height of 1.0 = zero parallax depth
	// (see pom_offset()), so a layer with no height_texture is unaffected by
	// POM even while it is enabled on the node.
	height_array = build_layer_texture_array(height_images, Color(1, 1, 1), Image::FORMAT_R8, false, layer_texture_size_limit);

	_set_shader_parameter(SNAME("albedo_array"), albedo_array);
	_set_shader_parameter(SNAME("normal_array"), normal_array);
	_set_shader_parameter(SNAME("orm_array"), orm_array);
	_set_shader_parameter(SNAME("height_array"), height_array);
	_set_shader_parameter(SNAME("height_texture_size"), float(MAX(height_array->get_width(), 1)));
	_set_shader_parameter(SNAME("layer_uv_scales"), uv_scales);
	_set_shader_parameter(SNAME("layer_albedo_colors"), albedo_colors);
	_set_shader_parameter(SNAME("layer_roughness"), roughness_values);
	_set_shader_parameter(SNAME("layer_specular"), specular_values);
	_set_shader_parameter(SNAME("layer_ao_strength"), ao_strength_values);
	_set_shader_parameter(SNAME("layer_normal_strength"), normal_strength_values);
	_set_shader_parameter(SNAME("layer_heightmap_scale"), heightmap_scale_values);
	_set_shader_parameter(SNAME("layer_height_min"), height_min_values);
	_set_shader_parameter(SNAME("layer_height_max"), height_max_values);
	_set_shader_parameter(SNAME("layer_pom_enabled"), pom_enabled_values);
	_set_shader_parameter(SNAME("layer_triplanar"), triplanar_values);
	_set_shader_parameter(SNAME("layer_triplanar_sharpness"), triplanar_sharpness_values);
	_set_shader_parameter(SNAME("layer_displacement"), displacement_values);
	_set_shader_parameter(SNAME("layer_count"), layer_count);

	_update_micro_detail_params();
	_update_draw_aabb();
	_invalidate_virtual_texture_all();
}

Vector2i Landscape3D::_get_block_grid_size() const {
	if (terrain_data.is_null()) {
		return Vector2i();
	}
	const int quads_total = MAX(terrain_data->get_resolution() - 1, 1);
	const int n = (quads_total + BLOCK_QUADS - 1) / BLOCK_QUADS;
	return Vector2i(MAX(n, 1), MAX(n, 1));
}

void Landscape3D::_add_blocks_in_region(const Rect2i &p_samples, HashSet<Vector2i> &r_blocks) const {
	const Vector2i grid = _get_block_grid_size();
	if (p_samples.size.x <= 0 || p_samples.size.y <= 0 || grid.x <= 0 || grid.y <= 0) {
		return;
	}
	// An edit reaches further than the blocks its samples belong to: a block
	// shares its border row and column of samples with its neighbors, and its
	// occluder takes the lowest sample up to one occluder stride away.
	// Looking only at the edited samples left the block across a border
	// stale whenever an edit started right on it.
	const Rect2i region = p_samples.grow(MAX(_get_occluder_stride(), 1));
	const Point2i end = region.get_end() - Point2i(1, 1);
	const int bx0 = CLAMP((int)Math::floor((float)region.position.x / BLOCK_QUADS), 0, grid.x - 1);
	const int bz0 = CLAMP((int)Math::floor((float)region.position.y / BLOCK_QUADS), 0, grid.y - 1);
	const int bx1 = CLAMP((int)Math::floor((float)end.x / BLOCK_QUADS), 0, grid.x - 1);
	const int bz1 = CLAMP((int)Math::floor((float)end.y / BLOCK_QUADS), 0, grid.y - 1);
	for (int z = bz0; z <= bz1; z++) {
		for (int x = bx0; x <= bx1; x++) {
			r_blocks.insert(Vector2i(x, z));
		}
	}
}

int Landscape3D::_get_occluder_stride() const {
	const int detail = CLAMP(occluder_detail, 1, BLOCK_QUADS);
	int quads = 1;
	while (quads * 2 <= detail) {
		quads *= 2;
	}
	return BLOCK_QUADS / quads;
}

void Landscape3D::_free_occluder_block(OccluderBlock &p_block) {
	if (p_block.instance.is_valid()) {
		RS::get_singleton()->free_rid(p_block.instance);
		p_block.instance = RID();
	}
	if (p_block.occluder.is_valid()) {
		RS::get_singleton()->free_rid(p_block.occluder);
		p_block.occluder = RID();
	}
}

void Landscape3D::_clear_occluders() {
	for (KeyValue<Vector2i, OccluderBlock> &kv : occluder_blocks) {
		_free_occluder_block(kv.value);
	}
	occluder_blocks.clear();
}

void Landscape3D::_rebuild_all_occluders() {
	_clear_occluders();
	if (!occluder_enabled || terrain_data.is_null()) {
		return;
	}
	const Vector2i grid = _get_block_grid_size();
	for (int z = 0; z < grid.y; z++) {
		for (int x = 0; x < grid.x; x++) {
			_rebuild_occluder_block(Vector2i(x, z));
		}
	}
}

void Landscape3D::_update_occluder_transforms() {
	const Transform3D global = _get_safe_global_transform();
	for (KeyValue<Vector2i, OccluderBlock> &kv : occluder_blocks) {
		if (kv.value.instance.is_valid()) {
			RS::get_singleton()->instance_set_transform(kv.value.instance, global * Transform3D(Basis(), kv.value.local_origin));
		}
	}
}

// Builds a block's occluder: the same surface, decimated to a grid of
// occluder_detail x occluder_detail quads, with every vertex pushed down to
// the lowest heightmap sample within one occluder quad of it.
//
// That minimum is what makes the simplification safe. Occlusion culling may
// only ever claim less occlusion than the real geometry provides - an occluder
// poking out above the surface it stands in for would hide things that are in
// fact visible - and taking the minimum over a full stride in each direction
// guarantees the opposite: every point of the terrain inside an occluder quad
// is within one stride of both of that quad's ends along each axis, so both
// ends sit at or below it, and so does everything the quad interpolates
// between them. The cost is that narrow crevices flatten out and sharp ridges
// lose a little height, which only ever costs some occlusion. Micro detail
// displacement is left out for the same reason: it can only dig below the
// heightmap by as much as it rises above it, and the occluder never stands
// above the heightmap's lowest nearby sample.
//
// Neighboring blocks agree on the shared edge vertices (the window is centered
// on the vertex, not on the block), so the per-block occluders join up into
// one continuous surface with no cracks for the depth buffer to leak through.
void Landscape3D::_rebuild_occluder_block(const Vector2i &p_block) {
	if (!occluder_enabled || terrain_data.is_null()) {
		OccluderBlock *existing = occluder_blocks.getptr(p_block);
		if (existing != nullptr) {
			_free_occluder_block(*existing);
			occluder_blocks.erase(p_block);
		}
		return;
	}

	const int stride = _get_occluder_stride();
	const int steps = BLOCK_QUADS / stride;
	const int base_ix = p_block.x * BLOCK_QUADS;
	const int base_iz = p_block.y * BLOCK_QUADS;
	const float spacing = terrain_data->get_vertex_spacing();
	// A displacement sinking the surface below the heightmap has to be
	// allowed for: the occluder may never stand above what is drawn.
	const float sink = micro_detail_distance > 0.0f ? displacement_bound : 0.0f;

	const Rect2i height_region(base_ix - stride, base_iz - stride, BLOCK_QUADS + 2 * stride + 1, BLOCK_QUADS + 2 * stride + 1);
	const PackedFloat32Array heights = terrain_data->get_height_region(height_region);
	const int height_w = height_region.size.x;

	PackedVector3Array vertices;
	vertices.resize((steps + 1) * (steps + 1));
	Vector3 *vertices_ptr = vertices.ptrw();

	for (int jz = 0; jz <= steps; jz++) {
		for (int jx = 0; jx <= steps; jx++) {
			// The vertex sits on the block's sample (jx * stride, jz * stride),
			// which the padded fetch holds one stride further in, so the
			// window around it starts back at (jx * stride, jz * stride).
			const int local_x = jx * stride;
			const int local_z = jz * stride;
			float lowest = heights[local_z * height_w + local_x];
			for (int sz = 0; sz <= 2 * stride; sz++) {
				const float *row = &heights.ptr()[(local_z + sz) * height_w + local_x];
				for (int sx = 0; sx <= 2 * stride; sx++) {
					lowest = MIN(lowest, row[sx]);
				}
			}
			vertices_ptr[jz * (steps + 1) + jx] = Vector3(local_x * spacing, lowest - sink, local_z * spacing);
		}
	}

	const Rect2i hole_region(base_ix, base_iz, BLOCK_QUADS + 1, BLOCK_QUADS + 1);
	const PackedByteArray holes = terrain_data->get_hole_region(hole_region);
	const int hole_w = hole_region.size.x;
	const int last_sample = terrain_data->get_resolution() - 1;

	// A quad that covers any hole at all is dropped: you can see through a
	// hole, so nothing standing behind one may be culled because of it. So
	// is a quad past the terrain's last sample, where the last block of a
	// terrain whose size is not a multiple of BLOCK_QUADS runs out of ground.
	auto quad_is_open = [&](int p_jx, int p_jz) {
		if (base_ix + (p_jx + 1) * stride > last_sample || base_iz + (p_jz + 1) * stride > last_sample) {
			return true;
		}
		for (int sz = p_jz * stride; sz <= (p_jz + 1) * stride; sz++) {
			for (int sx = p_jx * stride; sx <= (p_jx + 1) * stride; sx++) {
				if (holes[sz * hole_w + sx] != 0) {
					return true;
				}
			}
		}
		return false;
	};

	PackedInt32Array indices;
	for (int jz = 0; jz < steps; jz++) {
		for (int jx = 0; jx < steps; jx++) {
			if (quad_is_open(jx, jz)) {
				continue;
			}
			const int a = jz * (steps + 1) + jx;
			const int b = a + 1;
			const int c = a + steps + 1;
			const int d = c + 1;
			indices.push_back(a);
			indices.push_back(b);
			indices.push_back(c);
			indices.push_back(b);
			indices.push_back(d);
			indices.push_back(c);
		}
	}

	if (indices.is_empty()) {
		OccluderBlock *existing = occluder_blocks.getptr(p_block);
		if (existing != nullptr) {
			_free_occluder_block(*existing);
			occluder_blocks.erase(p_block);
		}
		return;
	}

	OccluderBlock &block = occluder_blocks[p_block];
	block.local_origin = Vector3(base_ix * spacing, 0, base_iz * spacing);
	if (block.occluder.is_null()) {
		block.occluder = RS::get_singleton()->occluder_create();
	}
	RS::get_singleton()->occluder_set_mesh(block.occluder, vertices, indices);

	if (block.instance.is_null()) {
		const RID scenario = (is_inside_tree() && get_world_3d().is_valid()) ? get_world_3d()->get_scenario() : RID();
		block.instance = RS::get_singleton()->instance_create2(block.occluder, scenario);
	} else {
		RS::get_singleton()->instance_set_base(block.instance, block.occluder);
	}

	RS::get_singleton()->instance_set_transform(block.instance, _get_safe_global_transform() * Transform3D(Basis(), block.local_origin));
	RS::get_singleton()->instance_set_visible(block.instance, is_visible_in_tree());
}

void Landscape3D::_ensure_collision_nodes() {
	if (collision_body != nullptr) {
		return;
	}
	collision_body = memnew(StaticBody3D);
	collision_body->set_collision_layer(collision_layer);
	collision_body->set_collision_mask(collision_mask);
	add_child(collision_body, false, INTERNAL_MODE_FRONT);
}

bool Landscape3D::_layout_collision_tiles() {
	_ensure_collision_nodes();

	const int quads = terrain_data->get_resolution() - 1;
	const int tile_quads = MAX(MIN(COLLISION_TILE_QUADS, quads), 1);
	const int per_side = (quads + tile_quads - 1) / tile_quads;
	const bool new_layout = tile_quads != collision_tile_quads || per_side != collision_tiles_per_side;
	if (new_layout) {
		_clear_collision_tiles();
		collision_tile_quads = tile_quads;
		collision_tiles_per_side = per_side;
		collision_tiles.resize(per_side * per_side);
		for (int z = 0; z < per_side; z++) {
			for (int x = 0; x < per_side; x++) {
				CollisionTile &tile = collision_tiles[z * per_side + x];
				// The last tile along each axis is moved back to end on the
				// terrain's edge, overlapping the one before it, rather than
				// cut short: Jolt Physics only builds a height field from a
				// square heightmap, and a far slower triangle mesh otherwise.
				tile.origin = Vector2i(MIN(x * tile_quads, quads - tile_quads), MIN(z * tile_quads, quads - tile_quads));
				tile.shape.instantiate();
				tile.shape->set_map_width(tile_quads + 1);
				tile.shape->set_map_depth(tile_quads + 1);
				tile.node = memnew(CollisionShape3D);
				tile.node->set_shape(tile.shape);
				collision_body->add_child(tile.node, false, INTERNAL_MODE_FRONT);
			}
		}
	}

	// HeightMapShape3D is centered on its node, one unit per quad.
	const float spacing = terrain_data->get_vertex_spacing();
	const float half = float(tile_quads) * 0.5f;
	for (CollisionTile &tile : collision_tiles) {
		tile.node->set_position(Vector3((float(tile.origin.x) + half) * spacing, 0, (float(tile.origin.y) + half) * spacing));
		tile.node->set_scale(Vector3(spacing, 1.0f, spacing));
	}
	return new_layout;
}

void Landscape3D::_clear_collision_tiles() {
	for (CollisionTile &tile : collision_tiles) {
		if (tile.node != nullptr) {
			tile.node->get_parent()->remove_child(tile.node);
			memdelete(tile.node);
		}
	}
	collision_tiles.clear();
	collision_tile_quads = 0;
	collision_tiles_per_side = 0;
}

void Landscape3D::_update_collision_tile(CollisionTile &p_tile, bool p_only_if_changed) {
	const int resolution = terrain_data->get_resolution();
	const int size = collision_tile_quads + 1;
	const Ref<Image> heightmap = terrain_data->get_heightmap_image();
	ERR_FAIL_COND(heightmap.is_null());
	const float *heights = reinterpret_cast<const float *>(heightmap->ptr());

	if (p_only_if_changed) {
		// Comparing is far cheaper than having the physics engine rebuild a
		// shape that would come out the same.
		const Vector<real_t> current = p_tile.shape->get_map_data();
		if (current.size() == size * size) {
			const real_t *r = current.ptr();
			bool same = true;
			for (int z = 0; z < size && same; z++) {
				const float *row = heights + (p_tile.origin.y + z) * resolution + p_tile.origin.x;
				const real_t *shape_row = r + z * size;
				for (int x = 0; x < size; x++) {
					if (shape_row[x] != row[x]) {
						same = false;
						break;
					}
				}
			}
			if (same) {
				return;
			}
		}
	}

	Vector<real_t> tile_heights;
	tile_heights.resize(size * size);
	real_t *w = tile_heights.ptrw();
	for (int z = 0; z < size; z++) {
		const float *row = heights + (p_tile.origin.y + z) * resolution + p_tile.origin.x;
		for (int x = 0; x < size; x++) {
			w[z * size + x] = row[x];
		}
	}
	p_tile.shape->set_map_data(tile_heights);
}

void Landscape3D::_update_collision_regions(const Vector<Rect2i> &p_regions) {
	if (terrain_data.is_null()) {
		return;
	}
	if (_layout_collision_tiles()) {
		for (CollisionTile &tile : collision_tiles) {
			_update_collision_tile(tile, false);
		}
		return;
	}
	const Size2i tile_size(collision_tile_quads + 1, collision_tile_quads + 1);
	for (CollisionTile &tile : collision_tiles) {
		const Rect2i tile_samples(tile.origin, tile_size);
		for (const Rect2i &region : p_regions) {
			if (tile_samples.intersects(region)) {
				_update_collision_tile(tile, false);
				break;
			}
		}
	}
}

void Landscape3D::_on_layers_changed() {
	const float previous_bound = displacement_bound;
	_rebuild_layer_textures();
	_rebuild_weight_texture();
	if (displacement_bound != previous_bound && terrain_data.is_valid() && occluder_enabled && micro_detail_distance > 0.0f) {
		// The occluders sit below the deepest displacement.
		_rebuild_all_occluders();
	}
	lod_dirty = true;
}

void Landscape3D::_on_terrain_data_changed() {
	// Coarse fallback for edits made directly to the TerrainData resource
	// instead of through this node's own sculpt()/paint_layer()/set_hole()
	// (which already know exactly which region to refresh, and refresh only
	// that). This has no way to know what changed, so it rebuilds everything.
	_rebuild_terrain();
	update_collision();
	_emit_terrain_changed(_get_full_region());
}

void Landscape3D::_disconnect_terrain_data_changed() {
	if (terrain_data.is_valid()) {
		terrain_data->disconnect_changed(callable_mp(this, &Landscape3D::_on_terrain_data_changed));
	}
}

void Landscape3D::_connect_terrain_data_changed() {
	if (terrain_data.is_valid()) {
		terrain_data->connect_changed(callable_mp(this, &Landscape3D::_on_terrain_data_changed));
	}
}

Rect2i Landscape3D::_get_full_region() const {
	if (terrain_data.is_null()) {
		return Rect2i();
	}
	return Rect2i(0, 0, terrain_data->get_resolution(), terrain_data->get_resolution());
}

void Landscape3D::_emit_terrain_changed(const Rect2i &p_region) {
	emit_signal(SNAME("terrain_changed"), p_region);
}

void Landscape3D::set_terrain_data(const Ref<TerrainData> &p_data) {
	_disconnect_terrain_data_changed();
	terrain_data = p_data;
	_connect_terrain_data_changed();

	// A different terrain may have a different resolution: whatever was
	// built for the previous one goes.
	_clear_terrain();
	_rebuild_terrain();
	if (is_inside_tree()) {
		update_collision();
	}
	update_configuration_warnings();
	_emit_terrain_changed(_get_full_region());
}

Ref<TerrainData> Landscape3D::get_terrain_data() const {
	return terrain_data;
}

void Landscape3D::set_layers(const TypedArray<TerrainLayer> &p_layers) {
	for (int i = 0; i < layers.size(); i++) {
		Ref<TerrainLayer> old_layer = layers[i];
		if (old_layer.is_valid()) {
			old_layer->disconnect(CoreStringName(changed), callable_mp(this, &Landscape3D::_on_layers_changed));
		}
	}

	layers = p_layers;

	for (int i = 0; i < layers.size(); i++) {
		Ref<TerrainLayer> layer = layers[i];
		if (layer.is_valid()) {
			layer->connect(CoreStringName(changed), callable_mp(this, &Landscape3D::_on_layers_changed));
		}
	}

	_on_layers_changed();
	update_configuration_warnings();
}

TypedArray<TerrainLayer> Landscape3D::get_layers() const {
	return layers;
}

void Landscape3D::set_lod_pixel_error(float p_pixels) {
	lod_pixel_error = MAX(p_pixels, 0.05f);
	lod_dirty = true;
}

float Landscape3D::get_lod_pixel_error() const {
	return lod_pixel_error;
}

void Landscape3D::set_lod_max_quad_pixels(float p_pixels) {
	lod_max_quad_pixels = MAX(p_pixels, 1.0f);
	lod_dirty = true;
}

float Landscape3D::get_lod_max_quad_pixels() const {
	return lod_max_quad_pixels;
}

void Landscape3D::set_frustum_culling(bool p_enabled) {
	frustum_culling = p_enabled;
	lod_dirty = true;
}

bool Landscape3D::is_frustum_culling_enabled() const {
	return frustum_culling;
}

void Landscape3D::set_gpu_lod_enabled(bool p_enabled) {
	gpu_lod_enabled = p_enabled;
	// The MultiMeshes are switched over on the next frame.
	lod_dirty = true;
}

bool Landscape3D::is_gpu_lod_enabled() const {
	return gpu_lod_enabled;
}

void Landscape3D::set_shadow_distance(float p_distance) {
	shadow_distance = MAX(p_distance, 0.0f);
	lod_dirty = true;
}

float Landscape3D::get_shadow_distance() const {
	return shadow_distance;
}

void Landscape3D::set_micro_detail_levels(int p_levels) {
	micro_detail_levels = CLAMP(p_levels, 0, LandscapeQuadtree::MAX_MICRO_LEVELS);
	_update_micro_detail_params();
}

int Landscape3D::get_micro_detail_levels() const {
	return micro_detail_levels;
}

void Landscape3D::set_micro_detail_distance(float p_distance) {
	const bool was_displacing = micro_detail_distance > 0.0f;
	micro_detail_distance = MAX(p_distance, 0.0f);
	_update_micro_detail_params();
	if ((micro_detail_distance > 0.0f) != was_displacing && terrain_data.is_valid() && occluder_enabled && displacement_bound > 0.0f) {
		// The occluders sit below the deepest displacement, while there is any.
		_rebuild_all_occluders();
	}
}

float Landscape3D::get_micro_detail_distance() const {
	return micro_detail_distance;
}

void Landscape3D::set_micro_detail_triangle_size(float p_pixels) {
	micro_detail_triangle_size = MAX(p_pixels, 0.5f);
	lod_dirty = true;
}

float Landscape3D::get_micro_detail_triangle_size() const {
	return micro_detail_triangle_size;
}

void Landscape3D::set_debug_view(DebugView p_view) {
	debug_view = p_view;
	if (material.is_valid()) {
		material->set_shader_parameter(SNAME("debug_view"), int(debug_view));
	}
}

Landscape3D::DebugView Landscape3D::get_debug_view() const {
	return debug_view;
}

void Landscape3D::set_virtual_texture_enabled(bool p_enabled) {
	if (virtual_texture_enabled == p_enabled) {
		return;
	}
	virtual_texture_enabled = p_enabled;
	_update_virtual_texture();
}

bool Landscape3D::is_virtual_texture_enabled() const {
	return virtual_texture_enabled;
}

void Landscape3D::set_virtual_texture_texel_size(float p_size) {
	p_size = MAX(p_size, 0.001f);
	if (virtual_texture_texel_size == p_size) {
		return;
	}
	virtual_texture_texel_size = p_size;
	_update_virtual_texture();
}

float Landscape3D::get_virtual_texture_texel_size() const {
	return virtual_texture_texel_size;
}

void Landscape3D::set_virtual_texture_near_distance(float p_distance) {
	virtual_texture_near_distance = MAX(p_distance, 0.0f);
	if (material.is_valid() && virtual_texture.is_valid()) {
		material->set_shader_parameter(SNAME("rvt_near_distance"), virtual_texture_near_distance);
	}
}

float Landscape3D::get_virtual_texture_near_distance() const {
	return virtual_texture_near_distance;
}

void Landscape3D::set_virtual_texture_layers(uint32_t p_layers) {
	// The terrain itself draws into the texture through these: with none, nothing would.
	p_layers = p_layers ? p_layers : 1u;
	if (virtual_texture_layers == p_layers) {
		return;
	}
	virtual_texture_layers = p_layers;
	_update_writer_instance();
	_update_virtual_texture_volume();
}

uint32_t Landscape3D::get_virtual_texture_layers() const {
	return virtual_texture_layers;
}

RID Landscape3D::get_virtual_texture() const {
	return virtual_texture;
}

Transform3D Landscape3D::get_virtual_texture_volume() const {
	if (virtual_texture.is_null() || terrain_data.is_null() || quadtree.is_empty()) {
		return Transform3D();
	}
	return _get_safe_global_transform() * _get_virtual_texture_local_volume();
}

int Landscape3D::get_virtual_texture_size() const {
	return virtual_texture_size;
}

void Landscape3D::set_occluder_enabled(bool p_enabled) {
	if (occluder_enabled == p_enabled) {
		return;
	}
	occluder_enabled = p_enabled;
	_rebuild_all_occluders();
}

bool Landscape3D::is_occluder_enabled() const {
	return occluder_enabled;
}

void Landscape3D::set_occluder_detail(int p_detail) {
	int detail = CLAMP(p_detail, 1, BLOCK_QUADS);
	// Snap to a power of two, so the occluder grid lines up with the block's
	// own samples however it was set (the Inspector only offers those, but a
	// script can set anything).
	int snapped = 1;
	while (snapped * 2 <= detail) {
		snapped *= 2;
	}
	if (occluder_detail == snapped) {
		return;
	}
	occluder_detail = snapped;
	_rebuild_all_occluders();
}

int Landscape3D::get_occluder_detail() const {
	return occluder_detail;
}

void Landscape3D::set_cast_shadow(GeometryInstance3D::ShadowCastingSetting p_setting) {
	cast_shadow = p_setting;
	_update_draw_instances();
	lod_dirty = true;
}

GeometryInstance3D::ShadowCastingSetting Landscape3D::get_cast_shadow() const {
	return cast_shadow;
}

void Landscape3D::set_gi_mode(GeometryInstance3D::GIMode p_mode) {
	gi_mode = p_mode;
	_update_draw_instances();
}

GeometryInstance3D::GIMode Landscape3D::get_gi_mode() const {
	return gi_mode;
}

void Landscape3D::set_collision_layer(uint32_t p_layer) {
	collision_layer = p_layer;
	if (collision_body != nullptr) {
		collision_body->set_collision_layer(collision_layer);
	}
}

uint32_t Landscape3D::get_collision_layer() const {
	return collision_layer;
}

void Landscape3D::set_collision_mask(uint32_t p_mask) {
	collision_mask = p_mask;
	if (collision_body != nullptr) {
		collision_body->set_collision_mask(collision_mask);
	}
}

uint32_t Landscape3D::get_collision_mask() const {
	return collision_mask;
}

void Landscape3D::set_layer_texture_size_limit(int p_size) {
	layer_texture_size_limit = CLAMP(p_size, 16, 8192);
	_rebuild_layer_textures();
}

int Landscape3D::get_layer_texture_size_limit() const {
	return layer_texture_size_limit;
}

void Landscape3D::set_pom_enabled(bool p_enable) {
	pom_enabled = p_enable;
	_update_pom_params();
}

bool Landscape3D::is_pom_enabled() const {
	return pom_enabled;
}

void Landscape3D::set_pom_min_layers(int p_layers) {
	pom_min_layers = MAX(p_layers, 1);
	_update_pom_params();
}

int Landscape3D::get_pom_min_layers() const {
	return pom_min_layers;
}

void Landscape3D::set_pom_max_layers(int p_layers) {
	pom_max_layers = MAX(p_layers, 1);
	_update_pom_params();
}

int Landscape3D::get_pom_max_layers() const {
	return pom_max_layers;
}

void Landscape3D::set_pom_flip_tangent(bool p_flip) {
	pom_flip_tangent = p_flip;
	_update_pom_params();
}

bool Landscape3D::get_pom_flip_tangent() const {
	return pom_flip_tangent;
}

void Landscape3D::set_pom_flip_binormal(bool p_flip) {
	pom_flip_binormal = p_flip;
	_update_pom_params();
}

bool Landscape3D::get_pom_flip_binormal() const {
	return pom_flip_binormal;
}

void Landscape3D::set_pom_self_shadow_enabled(bool p_enable) {
	pom_self_shadow_enabled = p_enable;
	_update_pom_params();
}

bool Landscape3D::is_pom_self_shadow_enabled() const {
	return pom_self_shadow_enabled;
}

void Landscape3D::set_pom_shadow_steps(int p_steps) {
	pom_shadow_steps = MAX(p_steps, 1);
	_update_pom_params();
}

int Landscape3D::get_pom_shadow_steps() const {
	return pom_shadow_steps;
}

void Landscape3D::set_pom_shadow_strength(float p_strength) {
	pom_shadow_strength = CLAMP(p_strength, 0.0f, 1.0f);
	_update_pom_params();
}

float Landscape3D::get_pom_shadow_strength() const {
	return pom_shadow_strength;
}

void Landscape3D::set_pom_shadow_light_direction(const Vector3 &p_direction) {
	pom_shadow_light_direction = p_direction;
	_update_pom_params();
}

Vector3 Landscape3D::get_pom_shadow_light_direction() const {
	return pom_shadow_light_direction;
}

void Landscape3D::set_pom_fade_start(float p_distance) {
	pom_fade_start = MAX(p_distance, 0.0f);
	_update_pom_params();
}

float Landscape3D::get_pom_fade_start() const {
	return pom_fade_start;
}

void Landscape3D::set_pom_fade_end(float p_distance) {
	pom_fade_end = MAX(p_distance, 0.0f);
	_update_pom_params();
}

float Landscape3D::get_pom_fade_end() const {
	return pom_fade_end;
}

void Landscape3D::sculpt(const Vector3 &p_local_position, float p_radius, float p_strength, SculptOperation p_operation, float p_falloff, float p_flatten_height, bool p_update_collision) {
	ERR_FAIL_COND(terrain_data.is_null());

	const float spacing = terrain_data->get_vertex_spacing();
	const int resolution = terrain_data->get_resolution();
	const float radius = MAX(p_radius, 0.001f);
	const int rad_idx = (int)Math::ceil(radius / spacing) + 1;
	const int cx = (int)Math::round(p_local_position.x / spacing);
	const int cz = (int)Math::round(p_local_position.z / spacing);
	const int x0 = CLAMP(cx - rad_idx, 0, resolution - 1);
	const int x1 = CLAMP(cx + rad_idx, 0, resolution - 1);
	const int z0 = CLAMP(cz - rad_idx, 0, resolution - 1);
	const int z1 = CLAMP(cz + rad_idx, 0, resolution - 1);
	if (x1 < x0 || z1 < z0) {
		return;
	}

	const Rect2i region(x0, z0, x1 - x0 + 1, z1 - z0 + 1);
	// Bulk fetch-modify-commit instead of one TerrainData::get_height/
	// set_height call per touched sample: a single stamp can touch
	// thousands of samples at a large brush radius, and each of those calls
	// used to mean its own Image access (see TerrainData for why that matters).
	PackedFloat32Array heights = terrain_data->get_height_region(region);
	const int region_w = region.size.x;

	// Smoothing is the one operation strength cannot simply scale: averaging a
	// vertex with its neighbors is one pass whatever fraction of it is applied,
	// so once the blend reaches that average there is nothing left for more
	// strength to do - which is why the brush used to feel stuck on weak however
	// high it was set. Strength is the number of averaging passes here, run over
	// the snapshot before any of it is blended in, so the result keeps getting
	// smoother as it rises. Each pass reads one ring further out, hence the
	// matching padding.
	const int smooth_passes = (p_operation == SCULPT_SMOOTH) ? CLAMP((int)Math::round(p_strength), 1, 32) : 1;
	const Rect2i snapshot_region(x0 - smooth_passes, z0 - smooth_passes,
			(x1 - x0 + 1) + smooth_passes * 2, (z1 - z0 + 1) + smooth_passes * 2);
	const int snapshot_w = snapshot_region.size.x;
	const int snapshot_h = snapshot_region.size.y;
	PackedFloat32Array before;
	if (p_operation == SCULPT_SMOOTH) {
		// get_height_region() clamps its reads to the terrain, so the padding
		// beyond an edge repeats that edge rather than reading as a cliff down
		// to zero.
		before = terrain_data->get_height_region(snapshot_region);
		PackedFloat32Array next;
		next.resize(before.size());
		for (int pass = 0; pass < smooth_passes; pass++) {
			const float *src = before.ptr();
			float *dst = next.ptrw();
			for (int z = 0; z < snapshot_h; z++) {
				for (int x = 0; x < snapshot_w; x++) {
					float sum = 0.0f;
					for (int nz = -1; nz <= 1; nz++) {
						for (int nx = -1; nx <= 1; nx++) {
							const int sx = CLAMP(x + nx, 0, snapshot_w - 1);
							const int sz = CLAMP(z + nz, 0, snapshot_h - 1);
							sum += src[sz * snapshot_w + sx];
						}
					}
					dst[z * snapshot_w + x] = sum / 9.0f;
				}
			}
			SWAP(before, next);
		}
	}
	auto sample_before = [&](int p_x, int p_z) -> float {
		const int x = CLAMP(p_x, snapshot_region.position.x, snapshot_region.position.x + snapshot_w - 1);
		const int z = CLAMP(p_z, snapshot_region.position.y, snapshot_region.position.y + snapshot_h - 1);
		return before[(z - snapshot_region.position.y) * snapshot_w + (x - snapshot_region.position.x)];
	};

	for (int z = z0; z <= z1; z++) {
		for (int x = x0; x <= x1; x++) {
			const float dx = (x - cx) * spacing;
			const float dz = (z - cz) * spacing;
			const float dist = Math::sqrt(dx * dx + dz * dz);
			if (dist > radius) {
				continue;
			}
			const float falloff = brush_falloff_weight(dist, radius, p_falloff);
			const float amount = p_strength * falloff;

			const int local_idx = (z - z0) * region_w + (x - x0);
			float h = heights[local_idx];
			switch (p_operation) {
				case SCULPT_RAISE: {
					h += amount;
				} break;
				case SCULPT_LOWER: {
					h -= amount;
				} break;
				case SCULPT_FLATTEN: {
					h = Math::lerp(h, p_flatten_height, CLAMP(amount, 0.0f, 1.0f));
				} break;
				case SCULPT_SMOOTH: {
					// Strength already went into how smooth sample_before() is
					// (see smooth_passes above), so here it only decides how
					// much of that to take below full strength - past 1 the
					// passes carry it, not the blend.
					h = Math::lerp(h, sample_before(x, z), CLAMP(p_strength, 0.0f, 1.0f) * falloff);
				} break;
			}
			heights.set(local_idx, h);
		}
	}

	_disconnect_terrain_data_changed();
	terrain_data->set_height_region(region, heights);
	_connect_terrain_data_changed();
	_refresh_region(region, true, false);
	if (p_update_collision) {
		_update_collision_regions({ region });
	}
	_emit_terrain_changed(region);
}

void Landscape3D::paint_layer(const Vector3 &p_local_position, float p_radius, float p_strength, int p_layer_index, float p_falloff) {
	ERR_FAIL_COND(terrain_data.is_null());
	ERR_FAIL_INDEX(p_layer_index, layers.size());
	ERR_FAIL_INDEX(p_layer_index, TerrainData::MAX_LAYERS);

	const float spacing = terrain_data->get_vertex_spacing();
	const int resolution = terrain_data->get_resolution();
	const float radius = MAX(p_radius, 0.001f);
	const int rad_idx = (int)Math::ceil(radius / spacing) + 1;
	const int cx = (int)Math::round(p_local_position.x / spacing);
	const int cz = (int)Math::round(p_local_position.z / spacing);
	const int x0 = CLAMP(cx - rad_idx, 0, resolution - 1);
	const int x1 = CLAMP(cx + rad_idx, 0, resolution - 1);
	const int z0 = CLAMP(cz - rad_idx, 0, resolution - 1);
	const int z1 = CLAMP(cz + rad_idx, 0, resolution - 1);
	if (x1 < x0 || z1 < z0) {
		return;
	}

	const Rect2i region(x0, z0, x1 - x0 + 1, z1 - z0 + 1);
	const int region_w = region.size.x;

	// Every layer's weight lives at this same point (see TerrainData and the
	// class description), so painting one has to read - and, below, write
	// back - all of them: raising the target layer's weight only means
	// anything relative to how much weight the others hold at that point.
	const int layer_count = layers.size();
	Vector<PackedFloat32Array> layer_weights;
	layer_weights.resize(layer_count);
	for (int i = 0; i < layer_count; i++) {
		layer_weights.write[i] = terrain_data->get_layer_weight_region(region, i);
	}

	for (int z = z0; z <= z1; z++) {
		for (int x = x0; x <= x1; x++) {
			const float dx = (x - cx) * spacing;
			const float dz = (z - cz) * spacing;
			const float dist = Math::sqrt(dx * dx + dz * dz);
			if (dist > radius) {
				continue;
			}
			const float amount = p_strength * brush_falloff_weight(dist, radius, p_falloff);

			const int local_idx = (z - z0) * region_w + (x - x0);
			set_layer_weight_renormalized(layer_weights, p_layer_index, local_idx, layer_weights[p_layer_index][local_idx] + amount);
		}
	}

	_disconnect_terrain_data_changed();
	for (int i = 0; i < layer_count; i++) {
		terrain_data->set_layer_weight_region(region, i, layer_weights[i]);
	}
	_connect_terrain_data_changed();

	_upload_weight_region(region, 0, layer_count);
}

void Landscape3D::set_hole(const Vector3 &p_local_position, float p_radius, bool p_hole, bool p_update_collision) {
	ERR_FAIL_COND(terrain_data.is_null());

	const float spacing = terrain_data->get_vertex_spacing();
	const int resolution = terrain_data->get_resolution();
	const float radius = MAX(p_radius, 0.001f);
	const int rad_idx = (int)Math::ceil(radius / spacing) + 1;
	const int cx = (int)Math::round(p_local_position.x / spacing);
	const int cz = (int)Math::round(p_local_position.z / spacing);
	const int x0 = CLAMP(cx - rad_idx, 0, resolution - 1);
	const int x1 = CLAMP(cx + rad_idx, 0, resolution - 1);
	const int z0 = CLAMP(cz - rad_idx, 0, resolution - 1);
	const int z1 = CLAMP(cz + rad_idx, 0, resolution - 1);
	if (x1 < x0 || z1 < z0) {
		return;
	}

	const Rect2i region(x0, z0, x1 - x0 + 1, z1 - z0 + 1);
	// Bulk fetch-modify-commit, same reasoning as sculpt() above.
	PackedByteArray holes = terrain_data->get_hole_region(region);
	const int region_w = region.size.x;

	for (int z = z0; z <= z1; z++) {
		for (int x = x0; x <= x1; x++) {
			const float dx = (x - cx) * spacing;
			const float dz = (z - cz) * spacing;
			if (Math::sqrt(dx * dx + dz * dz) > radius) {
				continue;
			}
			const int local_idx = (z - z0) * region_w + (x - x0);
			holes.set(local_idx, p_hole ? 1 : 0);
		}
	}

	_disconnect_terrain_data_changed();
	terrain_data->set_hole_region(region, holes);
	_connect_terrain_data_changed();
	_refresh_region(region, false, true);
	// p_update_collision is deliberately ignored: holes never affect the
	// collision shape (see the class description).
}

PackedFloat32Array Landscape3D::get_height_region(const Rect2i &p_region) const {
	ERR_FAIL_COND_V(terrain_data.is_null(), PackedFloat32Array());
	return terrain_data->get_height_region(p_region);
}

void Landscape3D::set_height_region(const Rect2i &p_region, const PackedFloat32Array &p_heights, bool p_update_collision) {
	ERR_FAIL_COND(terrain_data.is_null());
	_disconnect_terrain_data_changed();
	terrain_data->set_height_region(p_region, p_heights);
	_connect_terrain_data_changed();
	_refresh_region(p_region, true, false);
	if (p_update_collision) {
		_update_collision_regions({ p_region });
	}
	_emit_terrain_changed(p_region);
}

PackedFloat32Array Landscape3D::get_layer_weight_region(const Rect2i &p_region, int p_layer_index) const {
	ERR_FAIL_COND_V(terrain_data.is_null(), PackedFloat32Array());
	return terrain_data->get_layer_weight_region(p_region, p_layer_index);
}

void Landscape3D::set_layer_weight_region(const Rect2i &p_region, int p_layer_index, const PackedFloat32Array &p_weights) {
	ERR_FAIL_COND(terrain_data.is_null());
	_disconnect_terrain_data_changed();
	terrain_data->set_layer_weight_region(p_region, p_layer_index, p_weights);
	_connect_terrain_data_changed();
	_upload_weight_region(p_region, p_layer_index, 1);
}

PackedByteArray Landscape3D::get_hole_region(const Rect2i &p_region) const {
	ERR_FAIL_COND_V(terrain_data.is_null(), PackedByteArray());
	return terrain_data->get_hole_region(p_region);
}

void Landscape3D::set_hole_region(const Rect2i &p_region, const PackedByteArray &p_holes) {
	ERR_FAIL_COND(terrain_data.is_null());
	_disconnect_terrain_data_changed();
	terrain_data->set_hole_region(p_region, p_holes);
	_connect_terrain_data_changed();
	_refresh_region(p_region, false, true);
}

TypedArray<PackedFloat32Array> Landscape3D::get_height_regions(const TypedArray<Rect2i> &p_regions) const {
	TypedArray<PackedFloat32Array> result;
	ERR_FAIL_COND_V(terrain_data.is_null(), result);
	result.resize(p_regions.size());
	for (int i = 0; i < p_regions.size(); i++) {
		result[i] = terrain_data->get_height_region(p_regions[i]);
	}
	return result;
}

void Landscape3D::set_height_regions(const TypedArray<Rect2i> &p_regions, const TypedArray<PackedFloat32Array> &p_heights, bool p_update_collision) {
	ERR_FAIL_COND(terrain_data.is_null());
	ERR_FAIL_COND_MSG(p_regions.size() != p_heights.size(), "Every region needs exactly one array of heights.");

	Vector<Rect2i> regions;
	regions.resize(p_regions.size());
	_disconnect_terrain_data_changed();
	for (int i = 0; i < p_regions.size(); i++) {
		regions.write[i] = p_regions[i];
		terrain_data->set_height_region(regions[i], p_heights[i]);
	}
	_connect_terrain_data_changed();

	_refresh_regions(regions, true, false);
	if (p_update_collision) {
		_update_collision_regions(regions);
	}
	for (const Rect2i &region : regions) {
		_emit_terrain_changed(region);
	}
}

TypedArray<PackedFloat32Array> Landscape3D::get_layer_weight_regions(const TypedArray<Rect2i> &p_regions, int p_layer_index) const {
	TypedArray<PackedFloat32Array> result;
	ERR_FAIL_COND_V(terrain_data.is_null(), result);
	ERR_FAIL_INDEX_V(p_layer_index, TerrainData::MAX_LAYERS, result);
	result.resize(p_regions.size());
	for (int i = 0; i < p_regions.size(); i++) {
		result[i] = terrain_data->get_layer_weight_region(p_regions[i], p_layer_index);
	}
	return result;
}

void Landscape3D::set_layer_weight_regions(const TypedArray<Rect2i> &p_regions, int p_layer_index, const TypedArray<PackedFloat32Array> &p_weights) {
	ERR_FAIL_COND(terrain_data.is_null());
	ERR_FAIL_INDEX(p_layer_index, TerrainData::MAX_LAYERS);
	ERR_FAIL_COND_MSG(p_regions.size() != p_weights.size(), "Every region needs exactly one array of weights.");

	_disconnect_terrain_data_changed();
	for (int i = 0; i < p_regions.size(); i++) {
		terrain_data->set_layer_weight_region(p_regions[i], p_layer_index, p_weights[i]);
	}
	_connect_terrain_data_changed();

	for (int i = 0; i < p_regions.size(); i++) {
		_upload_weight_region(p_regions[i], p_layer_index, 1);
	}
}

void Landscape3D::paint_layer_regions(const TypedArray<Rect2i> &p_regions, int p_layer_index, const TypedArray<PackedFloat32Array> &p_weights) {
	ERR_FAIL_COND(terrain_data.is_null());
	ERR_FAIL_INDEX(p_layer_index, layers.size());
	ERR_FAIL_INDEX(p_layer_index, TerrainData::MAX_LAYERS);
	ERR_FAIL_COND_MSG(p_regions.size() != p_weights.size(), "Every region needs exactly one array of weights.");

	const int layer_count = MIN(layers.size(), TerrainData::MAX_LAYERS);
	Vector<PackedFloat32Array> layer_weights;
	layer_weights.resize(layer_count);

	_disconnect_terrain_data_changed();
	for (int r = 0; r < p_regions.size(); r++) {
		const Rect2i region = p_regions[r];
		const PackedFloat32Array targets = p_weights[r];
		const int sample_count = MAX(region.size.x, 0) * MAX(region.size.y, 0);
		ERR_CONTINUE_MSG(targets.size() < sample_count, "Too few weights for their region.");

		// Same reasoning as paint_layer(): raising one layer only means
		// anything relative to every other layer's weight at the same sample.
		for (int i = 0; i < layer_count; i++) {
			layer_weights.write[i] = terrain_data->get_layer_weight_region(region, i);
		}
		for (int idx = 0; idx < sample_count; idx++) {
			const float current = layer_weights[p_layer_index][idx];
			if (targets[idx] > current) {
				set_layer_weight_renormalized(layer_weights, p_layer_index, idx, targets[idx]);
			}
		}
		for (int i = 0; i < layer_count; i++) {
			terrain_data->set_layer_weight_region(region, i, layer_weights[i]);
		}
	}
	_connect_terrain_data_changed();

	for (int r = 0; r < p_regions.size(); r++) {
		_upload_weight_region(p_regions[r], 0, layer_count);
	}
}

void Landscape3D::update_collision() {
	if (terrain_data.is_null()) {
		_clear_collision_tiles();
		return;
	}
	// Only the tiles whose heights differ from what their shapes hold are
	// rebuilt, so that this stays cheap after an edit to a small part of a
	// large terrain, whichever way that edit was made.
	const bool new_layout = _layout_collision_tiles();
	for (CollisionTile &tile : collision_tiles) {
		_update_collision_tile(tile, !new_layout);
	}
}

Vector2i Landscape3D::local_position_to_index(const Vector3 &p_local_position) const {
	if (terrain_data.is_null()) {
		return Vector2i();
	}
	const float spacing = terrain_data->get_vertex_spacing();
	return Vector2i((int)Math::round(p_local_position.x / spacing), (int)Math::round(p_local_position.z / spacing));
}

AABB Landscape3D::get_aabb() const {
	if (quadtree.is_empty()) {
		return AABB();
	}
	// The root's height range, widened by the most micro detail can add to it:
	// Catmull-Rom interpolation overshooting the samples (under 30% of their
	// range) and displacement.
	const LandscapeQuadtree::NodeBounds root = quadtree.get_node_bounds(quadtree.get_top_level(), 0, 0, displacement_bound);
	const float margin = MAX(float(root.max.y - root.min.y - 2.0f * displacement_bound) * 0.3f, 0.001f);
	return AABB(root.min - Vector3(0, margin, 0), root.max - root.min + Vector3(0, 2.0f * margin, 0));
}

PackedStringArray Landscape3D::get_configuration_warnings() const {
	PackedStringArray warnings = Node3D::get_configuration_warnings();

	if (terrain_data.is_null()) {
		warnings.push_back(RTR("No TerrainData resource assigned. Assign one to sculpt and render this terrain."));
	}
	if (layers.is_empty()) {
		warnings.push_back(RTR("No TerrainLayer entries configured. The terrain will render as flat gray until at least one layer is added."));
	}

	return warnings;
}

Landscape3D::Landscape3D() {
	// Draw instance transforms and occluders are kept in sync from
	// NOTIFICATION_TRANSFORM_CHANGED, which Node3D only sends to nodes that
	// opt in.
	set_notify_transform(true);
}

Landscape3D::~Landscape3D() {
	_disconnect_frame_hook();
	_clear_occluders();
	_free_draw_instances();
	_free_virtual_texture();
	if (terrain_data.is_valid()) {
		terrain_data->disconnect_changed(callable_mp(this, &Landscape3D::_on_terrain_data_changed));
	}
}
