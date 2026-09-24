/**************************************************************************/
/*  foliage_lod_level.h                                                   */
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

#include "scene/3d/visual_instance_3d.h"

class Mesh;
class Material;

// One level of detail inside a FoliageLayer: its own mesh, optional material
// override, and the camera-distance range it should be visible/fade across.
// A FoliageLayer normally holds several of these (e.g. LOD_0, LOD_1, LOD_2)
// which FoliagePainter3D renders as parallel MultiMeshInstance3D nodes per
// cell, all sharing the exact same painted instance transforms - only the
// mesh/material/visibility range differ, so at any given camera distance the
// renderer shows whichever level's visibility range currently covers it (and
// cross-fades between two levels if their ranges overlap with a margin).
class FoliageLODLevel : public Resource {
	GDCLASS(FoliageLODLevel, Resource);

	Ref<Mesh> mesh;
	Ref<Material> material_override;

	bool cast_shadows = true;

	float visibility_range_begin = 0.0;
	float visibility_range_begin_margin = 0.0;
	float visibility_range_end = 0.0;
	float visibility_range_end_margin = 0.0;
	GeometryInstance3D::VisibilityRangeFadeMode visibility_range_fade_mode = GeometryInstance3D::VISIBILITY_RANGE_FADE_DISABLED;

protected:
	static void _bind_methods();

public:
	void set_mesh(const Ref<Mesh> &p_mesh);
	Ref<Mesh> get_mesh() const;

	void set_material_override(const Ref<Material> &p_material);
	Ref<Material> get_material_override() const;

	// Narrows the owner's shadow setting for this level only: a level can drop
	// out of the shadow passes, but cannot cast where the owner casts nothing.
	// Turning it off for the distant levels is usually the cheapest way to make
	// foliage shadows affordable, since those are the levels that cover most of
	// the ground while contributing the least.
	void set_cast_shadows(bool p_enable);
	bool is_casting_shadows() const;

	void set_visibility_range_begin(float p_dist);
	float get_visibility_range_begin() const;

	void set_visibility_range_begin_margin(float p_dist);
	float get_visibility_range_begin_margin() const;

	void set_visibility_range_end(float p_dist);
	float get_visibility_range_end() const;

	void set_visibility_range_end_margin(float p_dist);
	float get_visibility_range_end_margin() const;

	void set_visibility_range_fade_mode(GeometryInstance3D::VisibilityRangeFadeMode p_mode);
	GeometryInstance3D::VisibilityRangeFadeMode get_visibility_range_fade_mode() const;

	FoliageLODLevel();
};
