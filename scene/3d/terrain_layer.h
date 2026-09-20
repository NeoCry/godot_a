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

class Texture2D;

// One paintable ground material inside a Terrain3D: the textures used to
// render it (albedo, normal map, and a packed occlusion/roughness/metallic
// map) plus how large one texture tile is in world space. This is pure
// configuration; Terrain3D bakes every layer's textures into shared
// Texture2DArrays and paints this layer's weight into TerrainData's weight
// maps (see TerrainData::set_layer_weight).
class TerrainLayer : public Resource {
	GDCLASS(TerrainLayer, Resource);

	String layer_name = "Layer";
	Ref<Texture2D> albedo_texture;
	Ref<Texture2D> normal_texture;
	Ref<Texture2D> orm_texture;
	float uv_scale = 4.0;

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

	void set_uv_scale(float p_scale);
	float get_uv_scale() const;

	TerrainLayer();
};
