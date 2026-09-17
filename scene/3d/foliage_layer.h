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

#include "scene/3d/visual_instance_3d.h"
#include "scene/resources/multimesh.h"

class Mesh;
class Material;

// A single paintable vegetation type inside a FoliagePainter3D: the mesh to
// scatter, its randomization rules, and the visibility range it should fade
// out at. Each layer owns its own MultiMesh, which is where the actual
// painted instance transforms are stored (and saved with the scene).
class FoliageLayer : public Resource {
	GDCLASS(FoliageLayer, Resource);

	String layer_name = "Layer";
	Ref<Mesh> mesh;
	Ref<Material> material_override;
	Ref<MultiMesh> multimesh;

	// Randomization.
	float min_scale = 0.9;
	float max_scale = 1.1;
	bool random_rotation = true;
	float align_to_normal_amount = 0.0;
	float random_tilt_degrees = 0.0;

	// Brush behavior.
	float density = 4.0;
	float min_instance_spacing = 0.5;

	bool cast_shadows = true;

	// Visibility range (per vegetation type), mirrors GeometryInstance3D.
	float visibility_range_begin = 0.0;
	float visibility_range_begin_margin = 0.0;
	float visibility_range_end = 0.0;
	float visibility_range_end_margin = 0.0;
	GeometryInstance3D::VisibilityRangeFadeMode visibility_range_fade_mode = GeometryInstance3D::VISIBILITY_RANGE_FADE_DISABLED;

protected:
	static void _bind_methods();

public:
	void set_layer_name(const String &p_name);
	String get_layer_name() const;

	void set_mesh(const Ref<Mesh> &p_mesh);
	Ref<Mesh> get_mesh() const;

	void set_material_override(const Ref<Material> &p_material);
	Ref<Material> get_material_override() const;

	Ref<MultiMesh> get_multimesh() const;

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

	int get_instance_count() const;

	FoliageLayer();
};
