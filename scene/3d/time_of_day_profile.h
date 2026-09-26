/**************************************************************************/
/*  time_of_day_profile.h                                                 */
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
#include "core/templates/local_vector.h"
#include "scene/resources/curve.h"
#include "scene/resources/gradient.h"

class TimeOfDay;

// What a TimeOfDay node does to the things it drives over one day: a list of
// tracks, each of which ties one property of one target (the sun, the moon,
// the Environment, its sky material, its camera attributes, or any node) to a
// Curve for numbers or a Gradient for colors, spread over the 24 hours of the
// day.
//
// Weather uses the very same class. A weather profile only lists the tracks it
// changes (a denser fog, a dimmer sun), and TimeOfDay blends it over the base
// profile by the weather's intensity, so it can still vary over the day too.
class TimeOfDayProfile : public Resource {
	GDCLASS(TimeOfDayProfile, Resource);

public:
	enum Target {
		TARGET_SUN,
		TARGET_MOON,
		TARGET_ENVIRONMENT,
		TARGET_SKY_MATERIAL,
		TARGET_CAMERA_ATTRIBUTES,
		TARGET_NODE,
		TARGET_MAX,
	};

private:
	struct Track {
		Target target = TARGET_SUN;
		// Only used by TARGET_NODE, relative to the TimeOfDay node.
		NodePath node_path;
		StringName property;
		// The property split the way Object::get_indexed() wants it, so
		// "material_override:albedo_color" reaches into a sub-resource.
		Vector<StringName> property_path;
		Ref<Curve> curve;
		Ref<Gradient> gradient;
	};

	LocalVector<Track> tracks;

	// The TimeOfDay node this profile was last given to. Only ever used to
	// list the real properties of its targets in the Inspector, since a
	// resource cannot otherwise know what it is going to drive. Evaluating a
	// profile never needs it.
	ObjectID context;

	void _on_track_resource_changed();
	void _connect_track_resources(const Track &p_track);
	void _disconnect_track_resources(const Track &p_track);
	void _changed();

	Object *_get_context_target(const Track &p_track) const;
	Variant::Type _get_track_value_type(int p_index) const;
	String _get_property_suggestions(int p_index) const;
	void _create_track_resource(int p_index);

	void _swap_tracks(int p_a, int p_b);

protected:
	static void _bind_methods();
	bool _set(const StringName &p_name, const Variant &p_value);
	bool _get(const StringName &p_name, Variant &r_ret) const;
	void _get_property_list(List<PropertyInfo> *p_list) const;
	bool _property_can_revert(const StringName &p_name) const;
	bool _property_get_revert(const StringName &p_name, Variant &r_property) const;

public:
	void set_track_count(int p_count);
	int get_track_count() const;

	int add_track(Target p_target, const StringName &p_property, const NodePath &p_node_path = NodePath());
	void remove_track(int p_index);
	void clear_tracks();
	// Returns the index of the last track driving that property, since a later
	// track overrides an earlier one, or -1.
	int find_track(Target p_target, const StringName &p_property, const NodePath &p_node_path = NodePath()) const;

	void set_track_target(int p_index, Target p_target);
	Target get_track_target(int p_index) const;

	void set_track_node_path(int p_index, const NodePath &p_node_path);
	NodePath get_track_node_path(int p_index) const;

	void set_track_property(int p_index, const StringName &p_property);
	StringName get_track_property(int p_index) const;
	const Vector<StringName> &get_track_property_path(int p_index) const;

	void set_track_curve(int p_index, const Ref<Curve> &p_curve);
	Ref<Curve> get_track_curve(int p_index) const;

	void set_track_gradient(int p_index, const Ref<Gradient> &p_gradient);
	Ref<Gradient> get_track_gradient(int p_index) const;

	// Samples track p_index at p_hour (0 to 24) as a value of p_type: its
	// Gradient for a color, its Curve for anything else (rounded for integers,
	// thresholded at 0.5 for booleans). Returns false if the track has nothing
	// to give for that type.
	bool sample(int p_index, double p_hour, Variant::Type p_type, Variant &r_value) const;

	void set_context(TimeOfDay *p_context);

	// Maps an hour of the day onto a curve's own domain, so that a curve whose
	// X axis runs from 0 to 24 and one left at the default 0 to 1 both span a
	// whole day.
	static double hour_to_curve_offset(const Ref<Curve> &p_curve, double p_hour);
	static bool is_drivable_type(Variant::Type p_type);
	static String get_target_name(Target p_target);

	// A full day for a ProceduralSkyMaterial sky: the sun, a moonlit night,
	// the sky's colors through dawn and dusk, and a fog that thickens around
	// sunrise.
	static Ref<TimeOfDayProfile> create_default();
};

VARIANT_ENUM_CAST(TimeOfDayProfile::Target);
