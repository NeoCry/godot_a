/**************************************************************************/
/*  blend_landscape_3d.h                                                  */
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

#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/self_list.h"
#include "scene/resources/material.h"

// A StandardMaterial3D that blends into the ground of a Landscape3D where it meets it: near the ground,
// the surface takes on the landscape's own albedo, normal, roughness, metallic and occlusion, found
// the way the landscape finds them itself: its layers blended right there close to the camera, read
// back from its runtime virtual texture further away. The height of the ground under every pixel
// comes from the landscape's heightmap. So a rock, a wall or a tree's roots seem to grow out of the
// terrain whatever their pivot, rotation or scale, wherever they stand on it, and however the
// terrain is painted under them.
//
// Every BlendLandscape3D binds itself to a landscape: the first whose virtual texture layers share a
// bit with its blend_landscape_layers (see set_landscape_source(), which Landscape3D calls).
class BlendLandscape3D : public StandardMaterial3D {
	GDCLASS(BlendLandscape3D, StandardMaterial3D);

public:
	// What a landscape hands over to the materials that blend into it: its virtual texture layers,
	// which pick the materials, and the uniforms of theirs that read its ground (see
	// _get_shader_extension_uniforms()), by name.
	struct LandscapeSource {
		uint32_t layers = 0;
		HashMap<StringName, Variant> parameters;

		bool operator==(const LandscapeSource &p_other) const;
	};

private:
	enum ExtensionFlags {
		EXTENSION_LANDSCAPE_BLEND = 1,
		EXTENSION_NOISE = 2,
		EXTENSION_NORMAL_MAP = 4,
	};

	uint32_t blend_landscape_layers = 1;
	float blend_height = 0.3;
	float blend_offset = 0.0;
	float blend_falloff = 0.7;
	float blend_normal_strength = 1.0;
	float blend_slope = 0.0;
	Ref<Texture2D> blend_noise_texture;
	float blend_noise_scale = 2.0;
	float blend_noise_strength = 0.5;

	SelfList<BlendLandscape3D> landscape_element;
	ObjectID bound_landscape;
	// The texture uniforms a landscape set, cleared when it goes away.
	LocalVector<StringName> bound_textures;

	static Mutex landscapes_mutex;
	// By landscape, in the order they appeared.
	static HashMap<ObjectID, LandscapeSource> landscapes;
	static SelfList<BlendLandscape3D>::List blend_materials;

	// landscapes_mutex is held.
	void _bind_landscape_locked();
	void _bind_landscape();

protected:
	static void _bind_methods();

	virtual uint32_t _get_shader_extension_flags() const override;
	virtual String _get_shader_extension_uniforms(uint32_t p_flags) const override;
	virtual String _get_shader_extension_fragment(uint32_t p_flags) const override;

public:
	// The landscape's runtime virtual texture is where p_source says, or there is none anymore.
	static void set_landscape_source(ObjectID p_landscape, const LandscapeSource &p_source);
	static void remove_landscape_source(ObjectID p_landscape);

	void set_blend_landscape_layers(uint32_t p_layers);
	uint32_t get_blend_landscape_layers() const;

	void set_blend_height(float p_height);
	float get_blend_height() const;

	void set_blend_offset(float p_offset);
	float get_blend_offset() const;

	void set_blend_falloff(float p_falloff);
	float get_blend_falloff() const;

	void set_blend_normal_strength(float p_strength);
	float get_blend_normal_strength() const;

	void set_blend_slope(float p_slope);
	float get_blend_slope() const;

	void set_blend_noise_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_blend_noise_texture() const;

	void set_blend_noise_scale(float p_scale);
	float get_blend_noise_scale() const;

	void set_blend_noise_strength(float p_strength);
	float get_blend_noise_strength() const;

	// The landscape this material blends into, if any.
	ObjectID get_bound_landscape() const;

	BlendLandscape3D();
	~BlendLandscape3D();
};
