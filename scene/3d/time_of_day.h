/**************************************************************************/
/*  time_of_day.h                                                         */
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

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/3d/time_of_day_profile.h"
#include "scene/main/node.h"

class DirectionalLight3D;
class WorldEnvironment;

// Runs a day/night cycle over the DirectionalLight3D playing the sun (and,
// optionally, the one playing the moon) and the WorldEnvironment of a scene,
// similar in spirit to CryEngine's Time of Day.
//
// The sun and the moon are placed in the sky from the time, a latitude and a
// day of the year, the way they really move. Everything else comes from a
// TimeOfDayProfile: a list of tracks that tie any number or color property of
// the lights, the Environment, its sky material, its camera attributes or any
// other node to a Curve or a Gradient spanning the day. A second profile can
// be blended over it for weather, which also varies over the day and can fade
// in and out.
//
// Whatever this node changes, it remembers the value it started from, and puts
// it back when the scene is saved in the editor and when it stops driving it,
// so that neither the saved scene nor version control ever sees the cycle.
class TimeOfDay : public Node {
	GDCLASS(TimeOfDay, Node);

	NodePath sun_light_path;
	NodePath moon_light_path;
	NodePath world_environment_path;
	Ref<TimeOfDayProfile> profile;

	// Time.
	double time = 12.0;
	double day_duration = 1440.0;
	bool paused = false;
	bool run_in_editor = false;
	double update_interval = 0.0;

	double update_timer = 0.0;
	bool update_queued = false;

	// Where the sun and the moon are in the sky.
	double latitude = 45.0;
	int day_of_year = 80;
	double north_offset = 0.0;
	double moon_phase = 0.5;
	bool align_curves_to_sun = true;

	// Weather. What the Inspector shows (weather, weather_intensity) is where
	// the weather is headed; weather_layers is what is actually blended in
	// right now, which during a transition is several profiles fading in and
	// out at once. Their weights never add up to more than 1.
	Ref<TimeOfDayProfile> weather;
	double weather_intensity = 1.0;

	struct WeatherLayer {
		Ref<TimeOfDayProfile> profile;
		double weight = 0.0;
		double from_weight = 0.0;
		double to_weight = 0.0;
	};
	LocalVector<WeatherLayer> weather_layers;
	double weather_transition_duration = 0.0;
	double weather_transition_elapsed = 0.0;
	bool weather_transitioning = false;

	// Every profile whose changes have to be applied: the base one, the target
	// weather and whatever weather is still fading out.
	LocalVector<Ref<TimeOfDayProfile>> connected_profiles;

	// One property of one object that this node drives.
	struct DrivenKey {
		ObjectID object;
		StringName property;

		bool operator==(const DrivenKey &p_other) const {
			return object == p_other.object && property == p_other.property;
		}
	};

	struct DrivenKeyHasher {
		static _FORCE_INLINE_ uint32_t hash(const DrivenKey &p_key) {
			const uint32_t h = hash_murmur3_one_64((uint64_t)p_key.object);
			return hash_fmix32(hash_murmur3_one_32(p_key.property.hash(), h));
		}
	};

	// The value a driven property had before this node first changed it.
	struct RestValue {
		Vector<StringName> path;
		Variant value;
		uint64_t pass = 0;
	};
	HashMap<DrivenKey, RestValue, DrivenKeyHasher> rest_values;
	uint64_t apply_pass = 0;

	// Every track of every profile that drives one property, gathered for one
	// update, so the weather can be blended over the base value.
	struct Binding {
		struct Layer {
			uint32_t layer = 0;
			int track = -1;
		};

		DrivenKey key;
		Vector<StringName> path;
		int base_track = -1;
		LocalVector<Layer> layers;
	};

	DirectionalLight3D *_get_sun() const;
	DirectionalLight3D *_get_moon() const;
	WorldEnvironment *_get_world_environment() const;
	// Whether the environment's atmosphere tints the directional lights.
	bool _is_atmosphere_lighting() const;

	double _get_sun_declination() const;
	Vector3 _get_celestial_direction(double p_hour_angle, double p_declination) const;

	bool _is_advancing() const;
	void _update_process();
	void _advance(double p_hours);
	void _process_weather_transition(double p_delta);
	void _set_weather_now();

	void _update_profile_connections();
	void _update_profile_contexts();
	void _on_profile_changed();

	void _queue_update();
	void _update_deferred();
	void _apply();
	void _gather_bindings(const Ref<TimeOfDayProfile> &p_profile, int p_layer, LocalVector<Binding> &r_bindings, HashMap<DrivenKey, uint32_t, DrivenKeyHasher> &r_indices) const;
	void _apply_binding(const Binding &p_binding, double p_hour);
	Variant _blend_weather(const Binding &p_binding, const Variant &p_base, Variant::Type p_type, double p_hour) const;
	void _apply_celestial_light(DirectionalLight3D *p_light, const Vector3 &p_direction);

	RestValue &_touch_rest_value(const DrivenKey &p_key, const Vector<StringName> &p_path, const Variant &p_current);
	void _release_unused_rest_values();
	void _restore_rest_values(bool p_forget);

	void _append_track_warnings(const Ref<TimeOfDayProfile> &p_profile, const String &p_profile_name, PackedStringArray &r_warnings) const;

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void set_sun_light_path(const NodePath &p_path);
	NodePath get_sun_light_path() const;

	void set_moon_light_path(const NodePath &p_path);
	NodePath get_moon_light_path() const;

	void set_world_environment_path(const NodePath &p_path);
	NodePath get_world_environment_path() const;

	void set_profile(const Ref<TimeOfDayProfile> &p_profile);
	Ref<TimeOfDayProfile> get_profile() const;

	void set_time(double p_time);
	double get_time() const;

	void set_day_duration(double p_duration);
	double get_day_duration() const;

	void set_paused(bool p_paused);
	bool is_paused() const;

	void set_run_in_editor(bool p_enabled);
	bool is_running_in_editor() const;

	void set_update_interval(double p_interval);
	double get_update_interval() const;

	void set_latitude(double p_latitude);
	double get_latitude() const;

	void set_day_of_year(int p_day);
	int get_day_of_year() const;

	void set_north_offset(double p_degrees);
	double get_north_offset() const;

	void set_moon_phase(double p_phase);
	double get_moon_phase() const;

	void set_align_curves_to_sun(bool p_enabled);
	bool is_aligning_curves_to_sun() const;

	void set_weather(const Ref<TimeOfDayProfile> &p_weather);
	Ref<TimeOfDayProfile> get_weather() const;

	void set_weather_intensity(double p_intensity);
	double get_weather_intensity() const;

	void advance_time(double p_hours);
	void transition_to_weather(const Ref<TimeOfDayProfile> &p_weather, double p_duration, double p_intensity = 1.0);
	bool is_weather_transitioning() const;

	// Unit vectors pointing from the ground towards the sun and the moon.
	Vector3 get_sun_direction() const;
	Vector3 get_moon_direction() const;
	Vector3 get_sun_direction_at(double p_time) const;
	Vector3 get_moon_direction_at(double p_time) const;
	bool is_daytime() const;

	// The hours the sun rises and sets on day_of_year at latitude. Returns
	// false for a polar day or night, when it does neither.
	bool get_sun_events(double &r_sunrise, double &r_sunset) const;
	double get_sunrise_time() const;
	double get_sunset_time() const;

	// The hour the profile curves are sampled at for p_time: p_time itself, or
	// with align_curves_to_sun, p_time with the actual day stretched so that
	// sunrise lands on 6:00 and sunset on 18:00.
	double get_curve_time(double p_time) const;

	// The object a track of that target drives, or nullptr if it is missing.
	Object *get_target_object(TimeOfDayProfile::Target p_target, const NodePath &p_node_path = NodePath()) const;

	// A default profile, less the sky tracks that the current sky material has
	// no use for (a PhysicalSkyMaterial computes its colors itself).
	Ref<TimeOfDayProfile> make_default_profile() const;

	// Writes into the profile's curves and gradients, at the current time,
	// every driven value that no longer matches what the profile gives. Tweak
	// the lights or the environment by hand, then key them.
	int key_changed_values();

	void force_update();

	PackedStringArray get_configuration_warnings() const override;
};
