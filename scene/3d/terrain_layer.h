/**************************************************************************/
/*  terrain_layer.h                                                       */
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

#pragma once

#include "core/io/resource.h"
#include "core/math/color.h"

class Texture2D;

// One paintable ground material inside a Landscape3D: the textures used to
// render it (albedo, normal map, a packed occlusion/roughness/metallic map,
// and an optional heightmap for Parallax Occlusion Mapping - see
// Landscape3D.pom_enabled) plus scalar tweaks on top of them and how large
// one texture tile is in world space. This is pure configuration;
// Landscape3D bakes every layer's textures into shared Texture2DArrays and
// paints this layer's weight into TerrainData's weight maps (see
// TerrainData::set_layer_weight).
class TerrainLayer : public Resource {
	GDCLASS(TerrainLayer, Resource);

	String layer_name = "Layer";
	Ref<Texture2D> albedo_texture;
	Ref<Texture2D> normal_texture;
	Ref<Texture2D> orm_texture;
	Ref<Texture2D> height_texture;
	float uv_scale = 4.0;
	// See Landscape3D.pom_enabled; matches BaseMaterial3D.heightmap_scale's
	// range/default and its *0.01 internal scale factor "to improve
	// heightmap scale usability" (i.e. so typical values stay small).
	float heightmap_scale = 5.0;

	// Scalar tweaks on top of the ORM texture's occlusion/roughness channels
	// (or, with no texture assigned, on top of its flat default), the same
	// "value multiplies the texture, or stands alone with none assigned"
	// convention as BaseMaterial3D's own albedo_color/roughness/metallic.
	Color albedo_color = Color(1, 1, 1);
	float roughness = 1.0;
	// Matches BaseMaterial3D.metallic_specular: specular reflectance for a
	// dielectric surface at normal incidence, unrelated to the ORM texture's
	// metallic channel.
	float specular = 0.5;
	// 0 fades occlusion out entirely (this layer always reads as fully lit);
	// 1 uses orm_texture's occlusion channel as authored. Blended via
	// mix(1.0, occlusion, ao_strength), not a multiply: multiplying would
	// send ao_strength = 0 to full *occlusion* (black) instead of none.
	float ao_strength = 1.0;
	// Matches BaseMaterial3D.normal_scale exactly (same range/default),
	// applied via Godot's own NORMAL_MAP_DEPTH rather than by hand-decoding
	// and rescaling the normal map.
	float normal_strength = 1.0;

	// Remaps height_texture's sampled value from [height_min, height_max] to
	// [0, 1] before use, for a source texture that doesn't already use its
	// full range - see Landscape3D.pom_enabled.
	float height_min = 0.0;
	float height_max = 1.0;
	// Per-layer Parallax Occlusion Mapping switch (Landscape3D.pom_enabled
	// is a cheap master switch on top of this: both must be true). Off by
	// default even when height_texture is set, so assigning a height
	// texture to preview it doesn't silently turn on a per-fragment cost.
	bool pom_enabled = false;

	// Projects albedo/normal/orm from world-space X/Y/Z planes and blends
	// between them by surface normal instead of using a single UV, avoiding
	// the stretching a steep slope (a cliff face) would otherwise show with
	// ordinary top-down UVs. Mutually exclusive with pom_enabled on the same
	// layer (matches BaseMaterial3D, which doesn't support both either).
	bool triplanar_enabled = false;
	// Higher values sharpen the transition between the three projections;
	// matches BaseMaterial3D.uv1_triplanar_sharpness's range and default.
	float triplanar_sharpness = 1.0;

protected:
	static void _bind_methods();

public:
	void set_layer_name(const String &p_name);
	String get_layer_name() const;

	void set_albedo_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_albedo_texture() const;

	void set_normal_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_normal_texture() const;

	void set_orm_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_orm_texture() const;

	void set_height_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_height_texture() const;

	void set_heightmap_scale(float p_scale);
	float get_heightmap_scale() const;

	void set_uv_scale(float p_scale);
	float get_uv_scale() const;

	void set_albedo_color(const Color &p_color);
	Color get_albedo_color() const;

	void set_roughness(float p_roughness);
	float get_roughness() const;

	void set_specular(float p_specular);
	float get_specular() const;

	void set_ao_strength(float p_strength);
	float get_ao_strength() const;

	void set_normal_strength(float p_strength);
	float get_normal_strength() const;

	void set_height_min(float p_min);
	float get_height_min() const;

	void set_height_max(float p_max);
	float get_height_max() const;

	void set_pom_enabled(bool p_enable);
	bool is_pom_enabled() const;

	void set_triplanar_enabled(bool p_enable);
	bool is_triplanar_enabled() const;

	void set_triplanar_sharpness(float p_sharpness);
	float get_triplanar_sharpness() const;

	TerrainLayer();
};
