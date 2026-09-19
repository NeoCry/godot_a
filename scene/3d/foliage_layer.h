/**************************************************************************/
/*  foliage_layer.h                                                       */
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

#include "scene/3d/foliage_lod_level.h"

// A single paintable vegetation type inside a FoliagePainter3D: its
// randomization rules, brush behavior, and a chain of FoliageLODLevels
// (each with its own mesh, material, and visibility/fade distance) that
// share the exact same painted instance transforms. This is pure
// configuration; the painted instance transforms themselves are stored
// (and spatially chunked into cells) by FoliagePainter3D.
class FoliageLayer : public Resource {
	GDCLASS(FoliageLayer, Resource);

	String layer_name = "Layer";
	TypedArray<FoliageLODLevel> lod_levels;

	// Randomization.
	float min_scale = 0.9;
	float max_scale = 1.1;
	bool random_rotation = true;
	float align_to_normal_amount = 0.0;
	float random_tilt_degrees = 0.0;

	// Brush behavior.
	float density = 4.0;
	float min_instance_spacing = 0.5;

	// Shared across every LOD level of this layer.
	bool cast_shadows = true;
	bool ignore_screen_space_shadows = false;
	float lod_bias = 1.0;
	// See FoliageSpawner3D's cell_gi_mode field for why this defaults to disabled.
	GeometryInstance3D::GIMode gi_mode = GeometryInstance3D::GI_MODE_DISABLED;

	// Informational only: kept in sync by FoliagePainter3D (which owns the
	// actual per-cell instance data) purely so the Inspector can show it.
	// Not the source of truth, and not saved with the resource.
	int instance_count = 0;

	void _on_lod_level_changed();

protected:
	static void _bind_methods();

public:
	void set_layer_name(const String &p_name);
	String get_layer_name() const;

	void set_lod_levels(const TypedArray<FoliageLODLevel> &p_levels);
	TypedArray<FoliageLODLevel> get_lod_levels() const;

	// True if at least one LOD level has a Mesh assigned (i.e. this layer
	// actually renders something).
	bool has_any_mesh() const;

	void set_min_scale(float p_scale);
	float get_min_scale() const;

	void set_max_scale(float p_scale);
	float get_max_scale() const;

	void set_random_rotation(bool p_random);
	bool is_random_rotation_enabled() const;

	void set_align_to_normal_amount(float p_amount);
	float get_align_to_normal_amount() const;

	void set_random_tilt_degrees(float p_degrees);
	float get_random_tilt_degrees() const;

	void set_density(float p_density);
	float get_density() const;

	void set_min_instance_spacing(float p_spacing);
	float get_min_instance_spacing() const;

	void set_cast_shadows(bool p_enable);
	bool is_casting_shadows() const;

	void set_ignore_screen_space_shadows(bool p_ignore);
	bool is_ignoring_screen_space_shadows() const;

	void set_lod_bias(float p_bias);
	float get_lod_bias() const;

	void set_gi_mode(GeometryInstance3D::GIMode p_mode);
	GeometryInstance3D::GIMode get_gi_mode() const;

	int get_instance_count() const;
	// Called only by FoliagePainter3D to keep the display-only instance_count
	// property current; does not mark the resource changed/dirty.
	void _set_display_instance_count(int p_count);

	FoliageLayer();
};
