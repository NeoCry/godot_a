/**************************************************************************/
/*  ambient_probe_volume_3d.cpp                                           */
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

#include "ambient_probe_volume_3d.h"

#include "core/templates/local_vector.h"
#include "scene/3d/lightmap_probe.h"
#include "scene/main/scene_tree.h"

// Total probe count above this is still allowed, but triggers a configuration
// warning: LightmapGI's bake time and its dynamic-object probe lookup cost both
// scale with the total number of probes in the scene.
static const int AMBIENT_PROBE_VOLUME_WARN_PROBE_COUNT = 4096;
// Hard cap per axis to keep a single accidental huge grid from hanging the editor.
static const int AMBIENT_PROBE_VOLUME_MAX_PER_AXIS = 64;

static const StringName AMBIENT_PROBE_VOLUME_GENERATED_META = StringName("_ambient_probe_volume_generated");

void AmbientProbeVolume3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_size", "size"), &AmbientProbeVolume3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &AmbientProbeVolume3D::get_size);

	ClassDB::bind_method(D_METHOD("set_probe_counts", "counts"), &AmbientProbeVolume3D::set_probe_counts);
	ClassDB::bind_method(D_METHOD("get_probe_counts"), &AmbientProbeVolume3D::get_probe_counts);

	ClassDB::bind_method(D_METHOD("generate_probes"), &AmbientProbeVolume3D::generate_probes);
	ClassDB::bind_method(D_METHOD("clear_probes"), &AmbientProbeVolume3D::clear_probes);
	ClassDB::bind_method(D_METHOD("get_generated_probe_count"), &AmbientProbeVolume3D::get_generated_probe_count);

	ClassDB::bind_method(D_METHOD("get_generate_probes_button"), &AmbientProbeVolume3D::_get_generate_button);
	ClassDB::bind_method(D_METHOD("get_clear_probes_button"), &AmbientProbeVolume3D::_get_clear_button);

	ADD_GROUP("Volume", "");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "size", PROPERTY_HINT_RANGE, "0.01,4096,0.01,or_greater,suffix:m"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3I, "probe_counts", PROPERTY_HINT_NONE, ""), "set_probe_counts", "get_probe_counts");

	ADD_GROUP("", "");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "generate_probes_button", PROPERTY_HINT_TOOL_BUTTON, "Generate Probes", PROPERTY_USAGE_EDITOR), "", "get_generate_probes_button");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "clear_probes_button", PROPERTY_HINT_TOOL_BUTTON, "Clear Probes", PROPERTY_USAGE_EDITOR), "", "get_clear_probes_button");
}

Callable AmbientProbeVolume3D::_get_generate_button() const {
	return Callable(const_cast<AmbientProbeVolume3D *>(this), "generate_probes");
}

Callable AmbientProbeVolume3D::_get_clear_button() const {
	return Callable(const_cast<AmbientProbeVolume3D *>(this), "clear_probes");
}

void AmbientProbeVolume3D::set_size(const Vector3 &p_size) {
	size = p_size.maxf(0.01);
	update_gizmos();
	update_configuration_warnings();
}

Vector3 AmbientProbeVolume3D::get_size() const {
	return size;
}

void AmbientProbeVolume3D::set_probe_counts(const Vector3i &p_counts) {
	probe_counts = Vector3i(
			CLAMP(p_counts.x, 1, AMBIENT_PROBE_VOLUME_MAX_PER_AXIS),
			CLAMP(p_counts.y, 1, AMBIENT_PROBE_VOLUME_MAX_PER_AXIS),
			CLAMP(p_counts.z, 1, AMBIENT_PROBE_VOLUME_MAX_PER_AXIS));
	update_gizmos();
	update_configuration_warnings();
}

Vector3i AmbientProbeVolume3D::get_probe_counts() const {
	return probe_counts;
}

Vector3 AmbientProbeVolume3D::get_local_probe_position(int p_x, int p_y, int p_z) const {
	const Vector3 half = size * 0.5;
	const float fx = probe_counts.x > 1 ? float(p_x) / float(probe_counts.x - 1) : 0.5f;
	const float fy = probe_counts.y > 1 ? float(p_y) / float(probe_counts.y - 1) : 0.5f;
	const float fz = probe_counts.z > 1 ? float(p_z) / float(probe_counts.z - 1) : 0.5f;
	return Vector3(
			Math::lerp(-half.x, half.x, fx),
			Math::lerp(-half.y, half.y, fy),
			Math::lerp(-half.z, half.z, fz));
}

void AmbientProbeVolume3D::clear_probes() {
	LocalVector<Node *> to_remove;
	for (int i = 0; i < get_child_count(); i++) {
		Node *child = get_child(i);
		if (child->has_meta(AMBIENT_PROBE_VOLUME_GENERATED_META)) {
			to_remove.push_back(child);
		}
	}
	for (Node *n : to_remove) {
		remove_child(n);
		n->queue_free();
	}
	update_configuration_warnings();
}

void AmbientProbeVolume3D::generate_probes() {
	ERR_FAIL_COND_MSG(!is_inside_tree() || get_owner() == nullptr,
			"AmbientProbeVolume3D must be part of a saved scene (added below a scene root) before probes can be generated, otherwise the generated LightmapProbe nodes would not be saved with it.");

	clear_probes();

	Node *probe_owner = get_owner();

	int index = 0;
	for (int xi = 0; xi < probe_counts.x; xi++) {
		for (int yi = 0; yi < probe_counts.y; yi++) {
			for (int zi = 0; zi < probe_counts.z; zi++) {
				LightmapProbe *probe = memnew(LightmapProbe);
				probe->set_name(vformat("GeneratedProbe%d", index));
				probe->set_position(get_local_probe_position(xi, yi, zi));
				probe->set_meta(AMBIENT_PROBE_VOLUME_GENERATED_META, true);

				add_child(probe);
				probe->set_owner(probe_owner);

				index++;
			}
		}
	}

	update_gizmos();
	update_configuration_warnings();
}

int AmbientProbeVolume3D::get_generated_probe_count() const {
	int count = 0;
	for (int i = 0; i < get_child_count(); i++) {
		if (get_child(i)->has_meta(AMBIENT_PROBE_VOLUME_GENERATED_META)) {
			count++;
		}
	}
	return count;
}

PackedStringArray AmbientProbeVolume3D::get_configuration_warnings() const {
	PackedStringArray warnings = Node3D::get_configuration_warnings();

	if (size.x <= 0.0 || size.y <= 0.0 || size.z <= 0.0) {
		warnings.push_back(RTR("Size must be greater than zero on every axis."));
	}

	const int64_t total_probes = int64_t(probe_counts.x) * int64_t(probe_counts.y) * int64_t(probe_counts.z);
	if (total_probes > AMBIENT_PROBE_VOLUME_WARN_PROBE_COUNT) {
		warnings.push_back(vformat(RTR("Probe Counts would generate %d probes, which may noticeably increase LightmapGI bake times and the per-object cost of finding nearby probes at runtime. Consider lowering Probe Counts or splitting this volume."), total_probes));
	}

	if (get_generated_probe_count() == 0) {
		warnings.push_back(RTR("No probes have been generated yet. Press Generate Probes, then bake lighting with a LightmapGI node so the probes actually store ambient light and occlusion data."));
	}

	return warnings;
}

AmbientProbeVolume3D::AmbientProbeVolume3D() {
}
