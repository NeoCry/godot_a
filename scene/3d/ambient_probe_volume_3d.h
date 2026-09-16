/**************************************************************************/
/*  ambient_probe_volume_3d.h                                             */
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

#include "scene/3d/node_3d.h"

// Fills a configurable box volume with a regular grid of LightmapProbe children,
// so large-scale, baked ambient-occlusion/indirect-light coverage (e.g. for shading
// vegetation that doesn't carry its own lightmap) can be authored declaratively
// instead of hand-placing every LightmapProbe. The probes themselves are baked by
// a LightmapGI node exactly like any other LightmapProbe: this node only automates
// their placement within its bounds.
class AmbientProbeVolume3D : public Node3D {
	GDCLASS(AmbientProbeVolume3D, Node3D);

	Vector3 size = Vector3(20, 10, 20);
	Vector3i probe_counts = Vector3i(6, 3, 6);

	Callable _get_generate_button() const;
	Callable _get_clear_button() const;

protected:
	static void _bind_methods();

public:
	void set_size(const Vector3 &p_size);
	Vector3 get_size() const;

	void set_probe_counts(const Vector3i &p_counts);
	Vector3i get_probe_counts() const;

	// Local-space position of probe (p_x, p_y, p_z) in the [0, probe_counts) grid;
	// used both to generate LightmapProbe children and by the editor gizmo preview.
	Vector3 get_local_probe_position(int p_x, int p_y, int p_z) const;

	void generate_probes();
	void clear_probes();
	int get_generated_probe_count() const;

	PackedStringArray get_configuration_warnings() const override;

	AmbientProbeVolume3D();
};
