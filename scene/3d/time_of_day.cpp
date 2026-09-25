/**************************************************************************/
/*  time_of_day.cpp                                                       */
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

#include "time_of_day.h"

#include "core/config/engine.h"
#include "core/object/callable_mp.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/world_environment.h"
#include "scene/resources/sky.h"

// The tilt of the Earth's axis, which is how far north and south of the
// equator the sun wanders over a year.
static constexpr double AXIAL_TILT_DEGREES = 23.44;
// Day 80 is the 21st of March, the northern spring equinox: the sun crosses
// the equator, and rises at 6:00 and sets at 18:00 everywhere.
static constexpr int EQUINOX_DAY = 80;

// Targets.

DirectionalLight3D *TimeOfDay::_get_sun() const {
	return sun_light_path.is_empty() ? nullptr : Object::cast_to<DirectionalLight3D>(get_node_or_null(sun_light_path));
}

DirectionalLight3D *TimeOfDay::_get_moon() const {
	return moon_light_path.is_empty() ? nullptr : Object::cast_to<DirectionalLight3D>(get_node_or_null(moon_light_path));
}

WorldEnvironment *TimeOfDay::_get_world_environment() const {
	return world_environment_path.is_empty() ? nullptr : Object::cast_to<WorldEnvironment>(get_node_or_null(world_environment_path));
}

Object *TimeOfDay::get_target_object(TimeOfDayProfile::Target p_target, const NodePath &p_node_path) const {
	switch (p_target) {
		case TimeOfDayProfile::TARGET_SUN:
			return _get_sun();
		case TimeOfDayProfile::TARGET_MOON:
			return _get_moon();
		case TimeOfDayProfile::TARGET_ENVIRONMENT: {
			const WorldEnvironment *world_environment = _get_world_environment();
			return world_environment ? world_environment->get_environment().ptr() : nullptr;
		}
		case TimeOfDayProfile::TARGET_SKY_MATERIAL: {
			const WorldEnvironment *world_environment = _get_world_environment();
			if (!world_environment || world_environment->get_environment().is_null()) {
				return nullptr;
			}
			const Ref<Sky> sky = world_environment->get_environment()->get_sky();
			return sky.is_valid() ? sky->get_material().ptr() : nullptr;
		}
		case TimeOfDayProfile::TARGET_CAMERA_ATTRIBUTES: {
			const WorldEnvironment *world_environment = _get_world_environment();
			return world_environment ? world_environment->get_camera_attributes().ptr() : nullptr;
		}
		case TimeOfDayProfile::TARGET_NODE: {
			if (p_node_path.is_empty()) {
				return nullptr;
			}
			// Subnames in the path reach into the node's resources, like
			// "../Clouds:material_override".
			Ref<Resource> resource;
			Vector<StringName> leftover;
			Node *node = get_node_and_resource(p_node_path, resource, leftover, false);
			if (!node || !leftover.is_empty()) {
				return nullptr;
			}
			return resource.is_valid() ? static_cast<Object *>(resource.ptr()) : node;
		}
		case TimeOfDayProfile::TARGET_MAX:
			break;
	}
	return nullptr;
}

// The sun and the moon.
//
// Both are placed with the usual horizon coordinates formulas: an hour angle
// that turns 15 degrees an hour and is 0 at noon, a declination that follows
// the seasons, and the observer's latitude. Local solar time is used
// throughout, so the sun is always highest at 12:00.

double TimeOfDay::_get_sun_declination() const {
	return Math::deg_to_rad(AXIAL_TILT_DEGREES) * Math::sin(Math::TAU * (day_of_year - EQUINOX_DAY) / 365.0);
}

Vector3 TimeOfDay::_get_celestial_direction(double p_hour_angle, double p_declination) const {
	const double lat = Math::deg_to_rad(latitude);
	const double east = -Math::cos(p_declination) * Math::sin(p_hour_angle);
	const double north = Math::cos(lat) * Math::sin(p_declination) - Math::sin(lat) * Math::cos(p_declination) * Math::cos(p_hour_angle);
	const double up = Math::sin(lat) * Math::sin(p_declination) + Math::cos(lat) * Math::cos(p_declination) * Math::cos(p_hour_angle);
	// East is +X and north is -Z, before north_offset turns the compass.
	return Vector3(east, up, -north).rotated(Vector3(0, 1, 0), Math::deg_to_rad(north_offset)).normalized();
}

Vector3 TimeOfDay::get_sun_direction_at(double p_time) const {
	return _get_celestial_direction(Math::deg_to_rad(15.0 * (p_time - 12.0)), _get_sun_declination());
}

// The moon trails the sun by its phase: at new moon it rides along with the
// sun, at first quarter it is highest at 18:00, and at full moon it is
// opposite the sun, rising at sunset. A full moon stands high in winter and
// low in summer, which is the sun's declination turned around.
Vector3 TimeOfDay::get_moon_direction_at(double p_time) const {
	const double phase = Math::TAU * moon_phase;
	return _get_celestial_direction(Math::deg_to_rad(15.0 * (p_time - 12.0)) - phase, _get_sun_declination() * Math::cos(phase));
}

Vector3 TimeOfDay::get_sun_direction() const {
	return get_sun_direction_at(time);
}

Vector3 TimeOfDay::get_moon_direction() const {
	return get_moon_direction_at(time);
}

bool TimeOfDay::is_daytime() const {
	return get_sun_direction().y > 0.0;
}

bool TimeOfDay::get_sun_events(double &r_sunrise, double &r_sunset) const {
	const double lat = Math::deg_to_rad(CLAMP(latitude, -89.9, 89.9));
	const double cos_hour_angle = -Math::tan(lat) * Math::tan(_get_sun_declination());
	if (cos_hour_angle <= -1.0 || cos_hour_angle >= 1.0) {
		// The sun never sets, or never rises.
		return false;
	}
	const double half_day = Math::rad_to_deg(Math::acos(cos_hour_angle)) / 15.0;
	r_sunrise = 12.0 - half_day;
	r_sunset = 12.0 + half_day;
	return true;
}

double TimeOfDay::get_sunrise_time() const {
	double sunrise = -1.0;
	double sunset = -1.0;
	return get_sun_events(sunrise, sunset) ? sunrise : -1.0;
}

double TimeOfDay::get_sunset_time() const {
	double sunrise = -1.0;
	double sunset = -1.0;
	return get_sun_events(sunrise, sunset) ? sunset : -1.0;
}

// Profiles are authored around a 6:00 sunrise and an 18:00 sunset. Once the
// seasons move those, stretching the daylight and the night separately keeps
// the dawn colors at dawn instead of at a fixed hour.
double TimeOfDay::get_curve_time(double p_time) const {
	double sunrise = 0.0;
	double sunset = 0.0;
	if (!align_curves_to_sun || !get_sun_events(sunrise, sunset)) {
		return p_time;
	}
	if (p_time >= sunrise && p_time <= sunset) {
		return 6.0 + (p_time - sunrise) * 12.0 / (sunset - sunrise);
	}
	const double night = 24.0 - (sunset - sunrise);
	return Math::fposmod(18.0 + Math::fposmod(p_time - sunset, 24.0) * 12.0 / night, 24.0);
}

// Time.

bool TimeOfDay::_is_advancing() const {
	if (paused || day_duration <= 0.0) {
		return false;
	}
	return !Engine::get_singleton()->is_editor_hint() || run_in_editor;
}

void TimeOfDay::_update_process() {
	set_process_internal(_is_advancing() || weather_transitioning);
}

// Moves the clock forward, and announces every sunrise, sunset and midnight
// it goes past, in the order they happen.
void TimeOfDay::_advance(double p_hours) {
	if (p_hours <= 0.0) {
		return;
	}
	const double start = time;
	const double end = start + p_hours;
	time = Math::fposmod(end, 24.0);

	double sunrise = 0.0;
	double sunset = 0.0;
	const bool has_sun_events = get_sun_events(sunrise, sunset);

	for (double day = 0.0; day < end; day += 24.0) {
		if (has_sun_events) {
			if (day + sunrise > start && day + sunrise <= end) {
				emit_signal(SNAME("sunrise"));
			}
			if (day + sunset > start && day + sunset <= end) {
				emit_signal(SNAME("sunset"));
			}
		}
		if (day + 24.0 <= end) {
			emit_signal(SNAME("day_passed"));
		}
	}
}

void TimeOfDay::advance_time(double p_hours) {
	ERR_FAIL_COND_MSG(p_hours < 0.0, "Time can only be advanced forward. Set the time directly to go back.");
	_advance(p_hours);
	_queue_update();
}

// Weather.

void TimeOfDay::_set_weather_now() {
	weather_layers.clear();
	if (weather.is_valid() && weather_intensity > 0.0) {
		WeatherLayer layer;
		layer.profile = weather;
		layer.weight = weather_intensity;
		layer.from_weight = weather_intensity;
		layer.to_weight = weather_intensity;
		weather_layers.push_back(layer);
	}
	weather_transitioning = false;

	_update_profile_connections();
	_update_process();
	_queue_update();
}

void TimeOfDay::transition_to_weather(const Ref<TimeOfDayProfile> &p_weather, double p_duration, double p_intensity) {
	weather = p_weather;
	weather_intensity = CLAMP(p_intensity, 0.0, 1.0);
	if (weather.is_valid()) {
		weather->set_context(this);
	}

	if (p_duration <= 0.0) {
		_set_weather_now();
		emit_signal(SNAME("weather_transition_finished"));
		return;
	}

	// Everything already blended in fades out from wherever it is now, except
	// the target weather itself, which carries on from its current weight.
	bool found = false;
	for (WeatherLayer &layer : weather_layers) {
		layer.from_weight = layer.weight;
		if (weather.is_valid() && layer.profile == weather) {
			layer.to_weight = weather_intensity;
			found = true;
		} else {
			layer.to_weight = 0.0;
		}
	}
	if (!found && weather.is_valid()) {
		WeatherLayer layer;
		layer.profile = weather;
		layer.to_weight = weather_intensity;
		weather_layers.push_back(layer);
	}

	weather_transition_duration = p_duration;
	weather_transition_elapsed = 0.0;
	weather_transitioning = true;

	_update_profile_connections();
	_update_process();
	_queue_update();
}

void TimeOfDay::_process_weather_transition(double p_delta) {
	weather_transition_elapsed += p_delta;
	const double progress = CLAMP(weather_transition_elapsed / weather_transition_duration, 0.0, 1.0);
	const double blend = Math::smoothstep(0.0, 1.0, progress);
	for (WeatherLayer &layer : weather_layers) {
		layer.weight = Math::lerp(layer.from_weight, layer.to_weight, blend);
	}
	if (progress < 1.0) {
		return;
	}

	for (int i = (int)weather_layers.size() - 1; i >= 0; i--) {
		if (weather_layers[i].to_weight <= 0.0) {
			weather_layers.remove_at(i);
		}
	}
	weather_transitioning = false;
	_update_profile_connections();
	_update_process();
	emit_signal(SNAME("weather_transition_finished"));
}

bool TimeOfDay::is_weather_transitioning() const {
	return weather_transitioning;
}

// Profiles.

void TimeOfDay::_on_profile_changed() {
	_queue_update();
	update_configuration_warnings();
}

void TimeOfDay::_update_profile_connections() {
	LocalVector<Ref<TimeOfDayProfile>> wanted;
	if (profile.is_valid()) {
		wanted.push_back(profile);
	}
	if (weather.is_valid() && !wanted.has(weather)) {
		wanted.push_back(weather);
	}
	for (const WeatherLayer &layer : weather_layers) {
		if (layer.profile.is_valid() && !wanted.has(layer.profile)) {
			wanted.push_back(layer.profile);
		}
	}

	const Callable callable = callable_mp(this, &TimeOfDay::_on_profile_changed);
	for (const Ref<TimeOfDayProfile> &connected : connected_profiles) {
		if (!wanted.has(connected)) {
			connected->disconnect_changed(callable);
		}
	}
	for (const Ref<TimeOfDayProfile> &wanted_profile : wanted) {
		if (!connected_profiles.has(wanted_profile)) {
			wanted_profile->connect_changed(callable);
		}
	}
	connected_profiles = wanted;
}

void TimeOfDay::_update_profile_contexts() {
	if (profile.is_valid()) {
		profile->set_context(this);
	}
	if (weather.is_valid()) {
		weather->set_context(this);
	}
}

// Applying the profiles.

void TimeOfDay::_queue_update() {
	if (!is_inside_tree() || update_queued) {
		return;
	}
	update_queued = true;
	callable_mp(this, &TimeOfDay::_update_deferred).call_deferred();
}

void TimeOfDay::_update_deferred() {
	if (update_queued && is_inside_tree()) {
		_apply();
	}
	update_queued = false;
}

void TimeOfDay::force_update() {
	ERR_FAIL_COND_MSG(!is_inside_tree(), "TimeOfDay can only update the scene while it is inside the scene tree.");
	_apply();
}

void TimeOfDay::_gather_bindings(const Ref<TimeOfDayProfile> &p_profile, int p_layer, LocalVector<Binding> &r_bindings, HashMap<DrivenKey, uint32_t, DrivenKeyHasher> &r_indices) const {
	for (int i = 0; i < p_profile->get_track_count(); i++) {
		const Vector<StringName> &path = p_profile->get_track_property_path(i);
		if (path.is_empty()) {
			continue;
		}
		const Object *object = get_target_object(p_profile->get_track_target(i), p_profile->get_track_node_path(i));
		if (!object) {
			continue;
		}

		DrivenKey key;
		key.object = object->get_instance_id();
		key.property = p_profile->get_track_property(i);

		const uint32_t *existing = r_indices.getptr(key);
		if (!existing) {
			r_indices.insert(key, r_bindings.size());
			Binding binding;
			binding.key = key;
			binding.path = path;
			r_bindings.push_back(binding);
		}
		Binding &binding = r_bindings[existing ? *existing : r_bindings.size() - 1];

		// Within one profile, a later track overrides an earlier one for the
		// same property.
		if (p_layer < 0) {
			binding.base_track = i;
		} else if (!binding.layers.is_empty() && binding.layers[binding.layers.size() - 1].layer == (uint32_t)p_layer) {
			binding.layers[binding.layers.size() - 1].track = i;
		} else {
			Binding::Layer layer;
			layer.layer = p_layer;
			layer.track = i;
			binding.layers.push_back(layer);
		}
	}
}

// The weather blends over the base value as a weighted average: each weather
// layer pulls the value towards what it gives by its weight, and whatever
// weight is left over belongs to the base. Two weathers crossing over each
// other therefore meet halfway without the base showing through, however
// much they differ from it. Numbers and colors mix; booleans and integers,
// which cannot, take the value of whichever has the most weight.
Variant TimeOfDay::_blend_weather(const Binding &p_binding, const Variant &p_base, Variant::Type p_type, double p_hour) const {
	double total_weight = 0.0;
	for (const Binding::Layer &layer : p_binding.layers) {
		total_weight += weather_layers[layer.layer].weight;
	}
	const double scale = total_weight > 1.0 ? 1.0 / total_weight : 1.0;
	const double base_weight = MAX(0.0, 1.0 - total_weight * scale);

	switch (p_type) {
		case Variant::FLOAT: {
			const double base = p_base;
			double value = base * base_weight;
			for (const Binding::Layer &layer : p_binding.layers) {
				const double weight = weather_layers[layer.layer].weight * scale;
				Variant sampled;
				value += weight * (weather_layers[layer.layer].profile->sample(layer.track, p_hour, p_type, sampled) ? double(sampled) : base);
			}
			return value;
		}
		case Variant::COLOR: {
			const Color base = p_base;
			Color value = base * base_weight;
			for (const Binding::Layer &layer : p_binding.layers) {
				const float weight = weather_layers[layer.layer].weight * scale;
				Variant sampled;
				value += (weather_layers[layer.layer].profile->sample(layer.track, p_hour, p_type, sampled) ? Color(sampled) : base) * weight;
			}
			return value;
		}
		default: {
			Variant best = p_base;
			double best_weight = base_weight;
			for (const Binding::Layer &layer : p_binding.layers) {
				const double weight = weather_layers[layer.layer].weight * scale;
				Variant sampled;
				if (weight > best_weight && weather_layers[layer.layer].profile->sample(layer.track, p_hour, p_type, sampled)) {
					best = sampled;
					best_weight = weight;
				}
			}
			return best;
		}
	}
}

TimeOfDay::RestValue &TimeOfDay::_touch_rest_value(const DrivenKey &p_key, const Vector<StringName> &p_path, const Variant &p_current) {
	RestValue *rest = rest_values.getptr(p_key);
	if (!rest) {
		RestValue value;
		value.path = p_path;
		value.value = p_current;
		rest = &rest_values.insert(p_key, value)->value;
	}
	rest->pass = apply_pass;
	return *rest;
}

void TimeOfDay::_apply_binding(const Binding &p_binding, double p_hour) {
	Object *object = ObjectDB::get_instance(p_binding.key.object);
	if (!object) {
		return;
	}
	bool valid = false;
	const Variant current = object->get_indexed(p_binding.path, &valid);
	if (!valid || !TimeOfDayProfile::is_drivable_type(current.get_type())) {
		return;
	}
	const Variant::Type type = current.get_type();
	const RestValue &rest = _touch_rest_value(p_binding.key, p_binding.path, current);

	// A property that only the weather drives rests at its own value.
	Variant value = rest.value;
	if (p_binding.base_track >= 0) {
		Variant sampled;
		if (profile->sample(p_binding.base_track, p_hour, type, sampled)) {
			value = sampled;
		}
	}
	if (!p_binding.layers.is_empty()) {
		value = _blend_weather(p_binding, value, type, p_hour);
	}

	if (value != current) {
		object->set_indexed(p_binding.path, value, &valid);
	}
}

void TimeOfDay::_apply_celestial_light(DirectionalLight3D *p_light, const Vector3 &p_direction) {
	if (!p_light || !p_light->is_inside_tree()) {
		return;
	}
	DrivenKey key;
	key.object = p_light->get_instance_id();
	key.property = SNAME("basis");
	_touch_rest_value(key, { key.property }, p_light->get_basis());
	key.property = SNAME("visible");
	_touch_rest_value(key, { key.property }, p_light->is_visible());

	// A directional light shines down its -Z axis, away from the sun.
	const Vector3 up = Math::abs(p_direction.y) > 0.999f ? Vector3(0, 0, -1) : Vector3(0, 1, 0);
	const Basis basis = Basis::looking_at(-p_direction, up);
	if (!p_light->get_global_basis().is_equal_approx(basis)) {
		p_light->set_global_basis(basis);
	}

	// Below the horizon a light would only shine up through the ground, and at
	// no energy it would still darken the sky shaders around where it stands,
	// so it is hidden instead. That also spares its shadow maps all night.
	const bool visible = p_direction.y > 0.0f && p_light->get_param(Light3D::PARAM_ENERGY) > 0.0f;
	if (p_light->is_visible() != visible) {
		p_light->set_visible(visible);
	}
}

void TimeOfDay::_apply() {
	update_queued = false;
	update_timer = 0.0;
	if (!is_inside_tree()) {
		return;
	}
	apply_pass++;

	const double hour = get_curve_time(time);

	LocalVector<Binding> bindings;
	HashMap<DrivenKey, uint32_t, DrivenKeyHasher> indices;
	if (profile.is_valid()) {
		_gather_bindings(profile, -1, bindings, indices);
	}
	for (uint32_t i = 0; i < weather_layers.size(); i++) {
		if (weather_layers[i].profile.is_valid() && weather_layers[i].weight > 0.0) {
			_gather_bindings(weather_layers[i].profile, i, bindings, indices);
		}
	}
	for (const Binding &binding : bindings) {
		_apply_binding(binding, hour);
	}

	// After the tracks, since whether a light shows depends on its energy.
	DirectionalLight3D *sun = _get_sun();
	DirectionalLight3D *moon = _get_moon();
	_apply_celestial_light(sun, get_sun_direction_at(time));
	if (moon != sun) {
		_apply_celestial_light(moon, get_moon_direction_at(time));
	}

	_release_unused_rest_values();
}

// Whatever is no longer driven goes back to how it was: a track that was
// removed, a weather that has faded out, a light that was unassigned.
void TimeOfDay::_release_unused_rest_values() {
	LocalVector<DrivenKey> unused;
	for (const KeyValue<DrivenKey, RestValue> &E : rest_values) {
		if (E.value.pass != apply_pass) {
			unused.push_back(E.key);
		}
	}
	for (const DrivenKey &key : unused) {
		const RestValue &rest = rest_values[key];
		Object *object = ObjectDB::get_instance(key.object);
		if (object) {
			bool valid = false;
			object->set_indexed(rest.path, rest.value, &valid);
		}
		rest_values.erase(key);
	}
}

void TimeOfDay::_restore_rest_values(bool p_forget) {
	for (const KeyValue<DrivenKey, RestValue> &E : rest_values) {
		Object *object = ObjectDB::get_instance(E.key.object);
		if (object) {
			bool valid = false;
			object->set_indexed(E.value.path, E.value.value, &valid);
		}
	}
	if (p_forget) {
		rest_values.clear();
	}
}

// Keying.

static void _key_curve(const Ref<Curve> &p_curve, double p_hour, double p_value) {
	const double offset = TimeOfDayProfile::hour_to_curve_offset(p_curve, p_hour);
	// A key within five minutes of the time is the one being edited.
	const double tolerance = p_curve->get_domain_range() * (5.0 / (24.0 * 60.0));

	if (p_value > p_curve->get_max_value()) {
		p_curve->set_max_value(p_value);
	}
	if (p_value < p_curve->get_min_value()) {
		p_curve->set_min_value(p_value);
	}

	for (int i = 0; i < p_curve->get_point_count(); i++) {
		if (Math::abs(p_curve->get_point_position(i).x - offset) <= tolerance) {
			p_curve->set_point_value(i, p_value);
			return;
		}
	}
	// Following the curve's slope where the key goes in bends it as little
	// as possible around the new key.
	const double step = p_curve->get_domain_range() * 0.001;
	const double slope = (p_curve->sample(offset + step) - p_curve->sample(offset - step)) / (2.0 * step);
	p_curve->add_point(Vector2(offset, p_value), slope, slope);
}

static void _key_gradient(const Ref<Gradient> &p_gradient, double p_hour, const Color &p_color) {
	const double offset = p_hour / 24.0;
	const double tolerance = 5.0 / (24.0 * 60.0);
	for (int i = 0; i < p_gradient->get_point_count(); i++) {
		if (Math::abs(p_gradient->get_offset(i) - offset) <= tolerance) {
			p_gradient->set_color(i, p_color);
			return;
		}
	}
	p_gradient->add_point(offset, p_color);
}

static bool _is_same_value(const Variant &p_a, const Variant &p_b) {
	if (p_a.get_type() == Variant::FLOAT && p_b.get_type() == Variant::FLOAT) {
		return Math::is_equal_approx(double(p_a), double(p_b), 1e-5);
	}
	if (p_a.get_type() == Variant::COLOR && p_b.get_type() == Variant::COLOR) {
		return Color(p_a).is_equal_approx(Color(p_b));
	}
	return p_a == p_b;
}

int TimeOfDay::key_changed_values() {
	ERR_FAIL_COND_V_MSG(profile.is_null(), 0, "There is no profile to write keys into.");
	for (const WeatherLayer &layer : weather_layers) {
		ERR_FAIL_COND_V_MSG(layer.weight > 0.0, 0, "The lights and the environment include the weather right now, which must not be written into the profile. Set the weather intensity to 0 first.");
	}

	const double hour = get_curve_time(time);
	int keyed = 0;
	for (int i = 0; i < profile->get_track_count(); i++) {
		Object *object = get_target_object(profile->get_track_target(i), profile->get_track_node_path(i));
		if (!object) {
			continue;
		}
		bool valid = false;
		const Variant current = object->get_indexed(profile->get_track_property_path(i), &valid);
		if (!valid || !TimeOfDayProfile::is_drivable_type(current.get_type())) {
			continue;
		}
		Variant sampled;
		if (!profile->sample(i, hour, current.get_type(), sampled) || _is_same_value(current, sampled)) {
			continue;
		}

		if (current.get_type() == Variant::COLOR) {
			_key_gradient(profile->get_track_gradient(i), hour, current);
		} else {
			_key_curve(profile->get_track_curve(i), hour, current.get_type() == Variant::BOOL ? (bool(current) ? 1.0 : 0.0) : double(current));
		}
		keyed++;
	}

	if (keyed > 0) {
		_queue_update();
	}
	return keyed;
}

Ref<TimeOfDayProfile> TimeOfDay::make_default_profile() const {
	Ref<TimeOfDayProfile> default_profile = TimeOfDayProfile::create_default();

	const Object *sky_material = get_target_object(TimeOfDayProfile::TARGET_SKY_MATERIAL);
	if (sky_material) {
		for (int i = default_profile->get_track_count() - 1; i >= 0; i--) {
			if (default_profile->get_track_target(i) != TimeOfDayProfile::TARGET_SKY_MATERIAL) {
				continue;
			}
			bool valid = false;
			sky_material->get_indexed(default_profile->get_track_property_path(i), &valid);
			if (!valid) {
				default_profile->remove_track(i);
			}
		}
	}
	return default_profile;
}

// Properties.

void TimeOfDay::set_sun_light_path(const NodePath &p_path) {
	sun_light_path = p_path;
	_queue_update();
	update_configuration_warnings();
}

NodePath TimeOfDay::get_sun_light_path() const {
	return sun_light_path;
}

void TimeOfDay::set_moon_light_path(const NodePath &p_path) {
	moon_light_path = p_path;
	_queue_update();
	update_configuration_warnings();
}

NodePath TimeOfDay::get_moon_light_path() const {
	return moon_light_path;
}

void TimeOfDay::set_world_environment_path(const NodePath &p_path) {
	world_environment_path = p_path;
	_queue_update();
	update_configuration_warnings();
	if (profile.is_valid()) {
		profile->notify_property_list_changed();
	}
}

NodePath TimeOfDay::get_world_environment_path() const {
	return world_environment_path;
}

void TimeOfDay::set_profile(const Ref<TimeOfDayProfile> &p_profile) {
	if (profile == p_profile) {
		return;
	}
	profile = p_profile;
	if (profile.is_valid()) {
		profile->set_context(this);
	}
	_update_profile_connections();
	_queue_update();
	update_configuration_warnings();
}

Ref<TimeOfDayProfile> TimeOfDay::get_profile() const {
	return profile;
}

void TimeOfDay::set_time(double p_time) {
	time = Math::fposmod(p_time, 24.0);
	_queue_update();
}

double TimeOfDay::get_time() const {
	return time;
}

void TimeOfDay::set_day_duration(double p_duration) {
	day_duration = MAX(0.0, p_duration);
	_update_process();
}

double TimeOfDay::get_day_duration() const {
	return day_duration;
}

void TimeOfDay::set_paused(bool p_paused) {
	paused = p_paused;
	_update_process();
}

bool TimeOfDay::is_paused() const {
	return paused;
}

void TimeOfDay::set_run_in_editor(bool p_enabled) {
	run_in_editor = p_enabled;
	_update_process();
}

bool TimeOfDay::is_running_in_editor() const {
	return run_in_editor;
}

void TimeOfDay::set_update_interval(double p_interval) {
	update_interval = MAX(0.0, p_interval);
}

double TimeOfDay::get_update_interval() const {
	return update_interval;
}

void TimeOfDay::set_latitude(double p_latitude) {
	latitude = CLAMP(p_latitude, -90.0, 90.0);
	_queue_update();
}

double TimeOfDay::get_latitude() const {
	return latitude;
}

void TimeOfDay::set_day_of_year(int p_day) {
	day_of_year = CLAMP(p_day, 1, 365);
	_queue_update();
}

int TimeOfDay::get_day_of_year() const {
	return day_of_year;
}

void TimeOfDay::set_north_offset(double p_degrees) {
	north_offset = p_degrees;
	_queue_update();
}

double TimeOfDay::get_north_offset() const {
	return north_offset;
}

void TimeOfDay::set_moon_phase(double p_phase) {
	moon_phase = CLAMP(p_phase, 0.0, 1.0);
	_queue_update();
}

double TimeOfDay::get_moon_phase() const {
	return moon_phase;
}

void TimeOfDay::set_align_curves_to_sun(bool p_enabled) {
	align_curves_to_sun = p_enabled;
	_queue_update();
}

bool TimeOfDay::is_aligning_curves_to_sun() const {
	return align_curves_to_sun;
}

void TimeOfDay::set_weather(const Ref<TimeOfDayProfile> &p_weather) {
	weather = p_weather;
	if (weather.is_valid()) {
		weather->set_context(this);
	}
	_set_weather_now();
	update_configuration_warnings();
}

Ref<TimeOfDayProfile> TimeOfDay::get_weather() const {
	return weather;
}

void TimeOfDay::set_weather_intensity(double p_intensity) {
	weather_intensity = CLAMP(p_intensity, 0.0, 1.0);
	_set_weather_now();
}

double TimeOfDay::get_weather_intensity() const {
	return weather_intensity;
}

// Warnings.

void TimeOfDay::_append_track_warnings(const Ref<TimeOfDayProfile> &p_profile, const String &p_profile_name, PackedStringArray &r_warnings) const {
	// Enough to point at the problem without burying every other warning.
	static constexpr int MAX_TRACK_WARNINGS = 6;
	int count = 0;

	for (int i = 0; i < p_profile->get_track_count(); i++) {
		const TimeOfDayProfile::Target target = p_profile->get_track_target(i);
		const String track = vformat(RTR("%s track %d (%s: %s)"), p_profile_name, i, TimeOfDayProfile::get_target_name(target), p_profile->get_track_property(i));

		String problem;
		const Object *object = get_target_object(target, p_profile->get_track_node_path(i));
		if (p_profile->get_track_property_path(i).is_empty()) {
			problem = RTR("has no property to drive.");
		} else if (!object) {
			problem = target == TimeOfDayProfile::TARGET_NODE ? RTR("its node path leads nowhere.") : RTR("its target is missing.");
		} else {
			bool valid = false;
			const Variant value = object->get_indexed(p_profile->get_track_property_path(i), &valid);
			if (!valid) {
				problem = vformat(RTR("%s has no such property."), object->get_class());
			} else if (!TimeOfDayProfile::is_drivable_type(value.get_type())) {
				problem = RTR("only numbers, booleans and colors can follow the time of day.");
			} else if (value.get_type() == Variant::COLOR && p_profile->get_track_gradient(i).is_null()) {
				problem = RTR("it drives a color, which needs a Gradient.");
			} else if (value.get_type() != Variant::COLOR && p_profile->get_track_curve(i).is_null()) {
				problem = RTR("it drives a number, which needs a Curve.");
			}
		}
		if (problem.is_empty()) {
			continue;
		}
		if (count == MAX_TRACK_WARNINGS) {
			r_warnings.push_back(vformat(RTR("%s: more tracks have problems."), p_profile_name));
			return;
		}
		r_warnings.push_back(track + ": " + problem);
		count++;
	}
}

PackedStringArray TimeOfDay::get_configuration_warnings() const {
	PackedStringArray warnings = Node::get_configuration_warnings();

	const DirectionalLight3D *sun = _get_sun();
	if (sun_light_path.is_empty()) {
		warnings.push_back(RTR("No sun is assigned. Set Sun Light Path to the DirectionalLight3D that plays the sun."));
	} else if (!sun) {
		warnings.push_back(RTR("Sun Light Path doesn't lead to a DirectionalLight3D."));
	}
	if (!moon_light_path.is_empty()) {
		const DirectionalLight3D *moon = _get_moon();
		if (!moon) {
			warnings.push_back(RTR("Moon Light Path doesn't lead to a DirectionalLight3D."));
		} else if (moon == sun) {
			warnings.push_back(RTR("The sun and the moon need two different DirectionalLight3D nodes."));
		}
	}
	if (!world_environment_path.is_empty()) {
		const WorldEnvironment *world_environment = _get_world_environment();
		if (!world_environment) {
			warnings.push_back(RTR("World Environment Path doesn't lead to a WorldEnvironment."));
		} else if (world_environment->get_environment().is_null()) {
			warnings.push_back(RTR("The WorldEnvironment has no Environment to drive."));
		}
	}

	if (profile.is_null()) {
		warnings.push_back(RTR("No profile is assigned, so only the sun and the moon move. Create one, or start from the default profile in the TimeOfDay Inspector."));
	} else {
		_append_track_warnings(profile, RTR("Profile"), warnings);
	}
	if (weather.is_valid() && weather != profile) {
		_append_track_warnings(weather, RTR("Weather"), warnings);
	}

	return warnings;
}

void TimeOfDay::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			_update_profile_contexts();
			_update_process();
			_queue_update();
		} break;

		case NOTIFICATION_EXIT_TREE: {
			// In the editor, leaving the tree (being deleted, or the scene being
			// closed) hands the scene back as it was. In a running game there is
			// nobody to hand it back to, and a node that is only moved around
			// would flicker.
			if (Engine::get_singleton()->is_editor_hint()) {
				_restore_rest_values(true);
			}
		} break;

		case NOTIFICATION_INTERNAL_PROCESS: {
			const double delta = get_process_delta_time();
			if (_is_advancing()) {
				_advance(delta * 24.0 / day_duration);
			}
			if (weather_transitioning) {
				_process_weather_transition(delta);
			}
			update_timer += delta;
			if (update_timer >= update_interval) {
				_apply();
			}
		} break;

#ifdef TOOLS_ENABLED
		case NOTIFICATION_EDITOR_PRE_SAVE: {
			_restore_rest_values(false);
		} break;

		case NOTIFICATION_EDITOR_POST_SAVE: {
			if (is_inside_tree()) {
				_apply();
			}
		} break;
#endif // TOOLS_ENABLED
	}
}

void TimeOfDay::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_sun_light_path", "path"), &TimeOfDay::set_sun_light_path);
	ClassDB::bind_method(D_METHOD("get_sun_light_path"), &TimeOfDay::get_sun_light_path);
	ClassDB::bind_method(D_METHOD("set_moon_light_path", "path"), &TimeOfDay::set_moon_light_path);
	ClassDB::bind_method(D_METHOD("get_moon_light_path"), &TimeOfDay::get_moon_light_path);
	ClassDB::bind_method(D_METHOD("set_world_environment_path", "path"), &TimeOfDay::set_world_environment_path);
	ClassDB::bind_method(D_METHOD("get_world_environment_path"), &TimeOfDay::get_world_environment_path);
	ClassDB::bind_method(D_METHOD("set_profile", "profile"), &TimeOfDay::set_profile);
	ClassDB::bind_method(D_METHOD("get_profile"), &TimeOfDay::get_profile);

	ClassDB::bind_method(D_METHOD("set_time", "time"), &TimeOfDay::set_time);
	ClassDB::bind_method(D_METHOD("get_time"), &TimeOfDay::get_time);
	ClassDB::bind_method(D_METHOD("set_day_duration", "duration"), &TimeOfDay::set_day_duration);
	ClassDB::bind_method(D_METHOD("get_day_duration"), &TimeOfDay::get_day_duration);
	ClassDB::bind_method(D_METHOD("set_paused", "paused"), &TimeOfDay::set_paused);
	ClassDB::bind_method(D_METHOD("is_paused"), &TimeOfDay::is_paused);
	ClassDB::bind_method(D_METHOD("set_run_in_editor", "enabled"), &TimeOfDay::set_run_in_editor);
	ClassDB::bind_method(D_METHOD("is_running_in_editor"), &TimeOfDay::is_running_in_editor);
	ClassDB::bind_method(D_METHOD("set_update_interval", "interval"), &TimeOfDay::set_update_interval);
	ClassDB::bind_method(D_METHOD("get_update_interval"), &TimeOfDay::get_update_interval);

	ClassDB::bind_method(D_METHOD("set_latitude", "latitude"), &TimeOfDay::set_latitude);
	ClassDB::bind_method(D_METHOD("get_latitude"), &TimeOfDay::get_latitude);
	ClassDB::bind_method(D_METHOD("set_day_of_year", "day"), &TimeOfDay::set_day_of_year);
	ClassDB::bind_method(D_METHOD("get_day_of_year"), &TimeOfDay::get_day_of_year);
	ClassDB::bind_method(D_METHOD("set_north_offset", "degrees"), &TimeOfDay::set_north_offset);
	ClassDB::bind_method(D_METHOD("get_north_offset"), &TimeOfDay::get_north_offset);
	ClassDB::bind_method(D_METHOD("set_moon_phase", "phase"), &TimeOfDay::set_moon_phase);
	ClassDB::bind_method(D_METHOD("get_moon_phase"), &TimeOfDay::get_moon_phase);
	ClassDB::bind_method(D_METHOD("set_align_curves_to_sun", "enabled"), &TimeOfDay::set_align_curves_to_sun);
	ClassDB::bind_method(D_METHOD("is_aligning_curves_to_sun"), &TimeOfDay::is_aligning_curves_to_sun);

	ClassDB::bind_method(D_METHOD("set_weather", "weather"), &TimeOfDay::set_weather);
	ClassDB::bind_method(D_METHOD("get_weather"), &TimeOfDay::get_weather);
	ClassDB::bind_method(D_METHOD("set_weather_intensity", "intensity"), &TimeOfDay::set_weather_intensity);
	ClassDB::bind_method(D_METHOD("get_weather_intensity"), &TimeOfDay::get_weather_intensity);

	ClassDB::bind_method(D_METHOD("advance_time", "hours"), &TimeOfDay::advance_time);
	ClassDB::bind_method(D_METHOD("transition_to_weather", "weather", "duration", "intensity"), &TimeOfDay::transition_to_weather, DEFVAL(1.0));
	ClassDB::bind_method(D_METHOD("is_weather_transitioning"), &TimeOfDay::is_weather_transitioning);

	ClassDB::bind_method(D_METHOD("get_sun_direction"), &TimeOfDay::get_sun_direction);
	ClassDB::bind_method(D_METHOD("get_moon_direction"), &TimeOfDay::get_moon_direction);
	ClassDB::bind_method(D_METHOD("is_daytime"), &TimeOfDay::is_daytime);
	ClassDB::bind_method(D_METHOD("get_sunrise_time"), &TimeOfDay::get_sunrise_time);
	ClassDB::bind_method(D_METHOD("get_sunset_time"), &TimeOfDay::get_sunset_time);

	ClassDB::bind_method(D_METHOD("key_changed_values"), &TimeOfDay::key_changed_values);
	ClassDB::bind_method(D_METHOD("force_update"), &TimeOfDay::force_update);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "sun_light_path", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "DirectionalLight3D"), "set_sun_light_path", "get_sun_light_path");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "moon_light_path", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "DirectionalLight3D"), "set_moon_light_path", "get_moon_light_path");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "world_environment_path", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "WorldEnvironment"), "set_world_environment_path", "get_world_environment_path");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "profile", PROPERTY_HINT_RESOURCE_TYPE, TimeOfDayProfile::get_class_static()), "set_profile", "get_profile");

	ADD_GROUP("Time", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "time", PROPERTY_HINT_RANGE, "0,24,0.01,suffix:h"), "set_time", "get_time");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "day_duration", PROPERTY_HINT_RANGE, "1,86400,0.1,or_greater,exp,suffix:s"), "set_day_duration", "get_day_duration");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "paused"), "set_paused", "is_paused");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "run_in_editor"), "set_run_in_editor", "is_running_in_editor");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "update_interval", PROPERTY_HINT_RANGE, "0,1,0.01,or_greater,suffix:s"), "set_update_interval", "get_update_interval");

	ADD_GROUP("Sun And Moon", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "latitude", PROPERTY_HINT_RANGE, "-90,90,0.1,degrees"), "set_latitude", "get_latitude");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "day_of_year", PROPERTY_HINT_RANGE, "1,365,1"), "set_day_of_year", "get_day_of_year");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "north_offset", PROPERTY_HINT_RANGE, "-180,180,0.1,degrees"), "set_north_offset", "get_north_offset");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "moon_phase", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_moon_phase", "get_moon_phase");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "align_curves_to_sun"), "set_align_curves_to_sun", "is_aligning_curves_to_sun");

	ADD_GROUP("Weather", "weather_");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "weather", PROPERTY_HINT_RESOURCE_TYPE, TimeOfDayProfile::get_class_static()), "set_weather", "get_weather");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "weather_intensity", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_weather_intensity", "get_weather_intensity");

	ADD_SIGNAL(MethodInfo("sunrise"));
	ADD_SIGNAL(MethodInfo("sunset"));
	ADD_SIGNAL(MethodInfo("day_passed"));
	ADD_SIGNAL(MethodInfo("weather_transition_finished"));
}
