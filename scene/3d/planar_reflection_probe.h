/**************************************************************************/
/*  planar_reflection_probe.h                                             */
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

// A mirror for whatever lies flat in it: water, polished floors, wet roads.
// Every view that sees it draws the scene once more, mirrored across its plane
// (the node's local XZ plane, reflecting towards +Y), and the surfaces near
// that plane reflect that instead of the sky and reflection probes: exact
// reflections of everything on screen and off it, which no screen-space
// technique can give. See RenderingServer.planar_reflection_create().
class PlanarReflectionProbe : public VisualInstance3D {
	GDCLASS(PlanarReflectionProbe, VisualInstance3D);

	Vector2 size = Vector2(20, 20);
	float receive_distance = 1.0;
	float resolution_scale = 0.5;
	float max_distance = 0.0;
	float intensity = 1.0;
	float distortion = 1.0;
	float receive_angle = 60.0;
	float edge_fade = 0.1;
	float clip_bias = 0.02;
	bool enable_shadows = true;
	float mesh_lod_threshold = 4.0;
	uint32_t cull_mask = (1 << 20) - 1;
	uint32_t reflection_mask = (1 << 20) - 1;

protected:
	static void _bind_methods();

public:
	void set_size(const Vector2 &p_size);
	Vector2 get_size() const;

	void set_receive_distance(float p_distance);
	float get_receive_distance() const;

	void set_resolution_scale(float p_scale);
	float get_resolution_scale() const;

	void set_max_distance(float p_distance);
	float get_max_distance() const;

	void set_intensity(float p_intensity);
	float get_intensity() const;

	void set_distortion(float p_distortion);
	float get_distortion() const;

	void set_receive_angle(float p_degrees);
	float get_receive_angle() const;

	void set_edge_fade(float p_fade);
	float get_edge_fade() const;

	void set_clip_bias(float p_bias);
	float get_clip_bias() const;

	void set_enable_shadows(bool p_enable);
	bool are_shadows_enabled() const;

	void set_mesh_lod_threshold(float p_pixels);
	float get_mesh_lod_threshold() const;

	void set_cull_mask(uint32_t p_layers);
	uint32_t get_cull_mask() const;

	void set_reflection_mask(uint32_t p_layers);
	uint32_t get_reflection_mask() const;

	virtual AABB get_aabb() const override;
	PackedStringArray get_configuration_warnings() const override;

	PlanarReflectionProbe();
	~PlanarReflectionProbe();
};
