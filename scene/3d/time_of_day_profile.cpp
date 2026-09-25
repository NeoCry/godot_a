/**************************************************************************/
/*  time_of_day_profile.cpp                                               */
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

#include "time_of_day_profile.h"

#include "core/config/engine.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/templates/hash_set.h"
#include "scene/3d/time_of_day.h"

static const char *TRACK_TARGET_HINT = "Sun,Moon,Environment,Sky Material,Camera Attributes,Node";

void TimeOfDayProfile::_changed() {
	emit_changed();
}

void TimeOfDayProfile::_on_track_resource_changed() {
	emit_changed();
}

// Several tracks may share one Curve or Gradient, hence the reference counted
// connections: each track holds its own, and the last one to let go of the
// resource disconnects it.
void TimeOfDayProfile::_connect_track_resources(const Track &p_track) {
	const Callable callable = callable_mp(this, &TimeOfDayProfile::_on_track_resource_changed);
	if (p_track.curve.is_valid()) {
		p_track.curve->connect_changed(callable, CONNECT_REFERENCE_COUNTED);
	}
	if (p_track.gradient.is_valid()) {
		p_track.gradient->connect_changed(callable, CONNECT_REFERENCE_COUNTED);
	}
}

void TimeOfDayProfile::_disconnect_track_resources(const Track &p_track) {
	const Callable callable = callable_mp(this, &TimeOfDayProfile::_on_track_resource_changed);
	if (p_track.curve.is_valid()) {
		p_track.curve->disconnect_changed(callable);
	}
	if (p_track.gradient.is_valid()) {
		p_track.gradient->disconnect_changed(callable);
	}
}

void TimeOfDayProfile::set_track_count(int p_count) {
	ERR_FAIL_COND(p_count < 0);
	if (p_count == (int)tracks.size()) {
		return;
	}

	for (uint32_t i = p_count; i < tracks.size(); i++) {
		_disconnect_track_resources(tracks[i]);
	}
	tracks.resize(p_count);

	notify_property_list_changed();
	_changed();
}

int TimeOfDayProfile::get_track_count() const {
	return tracks.size();
}

int TimeOfDayProfile::add_track(Target p_target, const StringName &p_property, const NodePath &p_node_path) {
	ERR_FAIL_INDEX_V(p_target, TARGET_MAX, -1);
	Track track;
	track.target = p_target;
	track.node_path = p_node_path;
	track.property = p_property;
	track.property_path = NodePath(String(p_property)).get_as_property_path().get_subnames();
	tracks.push_back(track);

	notify_property_list_changed();
	_changed();
	return tracks.size() - 1;
}

void TimeOfDayProfile::remove_track(int p_index) {
	ERR_FAIL_INDEX(p_index, (int)tracks.size());
	_disconnect_track_resources(tracks[p_index]);
	tracks.remove_at(p_index);

	notify_property_list_changed();
	_changed();
}

void TimeOfDayProfile::clear_tracks() {
	set_track_count(0);
}

int TimeOfDayProfile::find_track(Target p_target, const StringName &p_property, const NodePath &p_node_path) const {
	for (int i = (int)tracks.size() - 1; i >= 0; i--) {
		const Track &track = tracks[i];
		if (track.target == p_target && track.property == p_property && (p_target != TARGET_NODE || track.node_path == p_node_path)) {
			return i;
		}
	}
	return -1;
}

void TimeOfDayProfile::_swap_tracks(int p_a, int p_b) {
	ERR_FAIL_INDEX(p_a, (int)tracks.size());
	ERR_FAIL_INDEX(p_b, (int)tracks.size());
	SWAP(tracks[p_a], tracks[p_b]);

	notify_property_list_changed();
	_changed();
}

void TimeOfDayProfile::set_track_target(int p_index, Target p_target) {
	ERR_FAIL_INDEX(p_index, (int)tracks.size());
	ERR_FAIL_INDEX(p_target, TARGET_MAX);
	if (tracks[p_index].target == p_target) {
		return;
	}
	tracks[p_index].target = p_target;

	notify_property_list_changed();
	_changed();
}

TimeOfDayProfile::Target TimeOfDayProfile::get_track_target(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, (int)tracks.size(), TARGET_SUN);
	return tracks[p_index].target;
}

void TimeOfDayProfile::set_track_node_path(int p_index, const NodePath &p_node_path) {
	ERR_FAIL_INDEX(p_index, (int)tracks.size());
	if (tracks[p_index].node_path == p_node_path) {
		return;
	}
	tracks[p_index].node_path = p_node_path;

	notify_property_list_changed();
	_changed();
}

NodePath TimeOfDayProfile::get_track_node_path(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, (int)tracks.size(), NodePath());
	return tracks[p_index].node_path;
}

void TimeOfDayProfile::set_track_property(int p_index, const StringName &p_property) {
	ERR_FAIL_INDEX(p_index, (int)tracks.size());
	Track &track = tracks[p_index];
	if (track.property == p_property) {
		return;
	}
	track.property = p_property;
	// NodePath parses "a:b" into a property and a sub-property, and keeps a
	// "shader_parameter/name" style property whole, as one name.
	track.property_path = NodePath(String(p_property)).get_as_property_path().get_subnames();

	notify_property_list_changed();
	_changed();
}

StringName TimeOfDayProfile::get_track_property(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, (int)tracks.size(), StringName());
	return tracks[p_index].property;
}

const Vector<StringName> &TimeOfDayProfile::get_track_property_path(int p_index) const {
	static const Vector<StringName> empty;
	ERR_FAIL_INDEX_V(p_index, (int)tracks.size(), empty);
	return tracks[p_index].property_path;
}

void TimeOfDayProfile::set_track_curve(int p_index, const Ref<Curve> &p_curve) {
	ERR_FAIL_INDEX(p_index, (int)tracks.size());
	Track &track = tracks[p_index];
	if (track.curve == p_curve) {
		return;
	}
	_disconnect_track_resources(track);
	track.curve = p_curve;
	_connect_track_resources(track);

	notify_property_list_changed();
	_changed();
}

Ref<Curve> TimeOfDayProfile::get_track_curve(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, (int)tracks.size(), Ref<Curve>());
	return tracks[p_index].curve;
}

void TimeOfDayProfile::set_track_gradient(int p_index, const Ref<Gradient> &p_gradient) {
	ERR_FAIL_INDEX(p_index, (int)tracks.size());
	Track &track = tracks[p_index];
	if (track.gradient == p_gradient) {
		return;
	}
	_disconnect_track_resources(track);
	track.gradient = p_gradient;
	_connect_track_resources(track);

	notify_property_list_changed();
	_changed();
}

Ref<Gradient> TimeOfDayProfile::get_track_gradient(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, (int)tracks.size(), Ref<Gradient>());
	return tracks[p_index].gradient;
}

double TimeOfDayProfile::hour_to_curve_offset(const Ref<Curve> &p_curve, double p_hour) {
	ERR_FAIL_COND_V(p_curve.is_null(), 0.0);
	return p_curve->get_min_domain() + (p_hour / 24.0) * p_curve->get_domain_range();
}

bool TimeOfDayProfile::is_drivable_type(Variant::Type p_type) {
	return p_type == Variant::FLOAT || p_type == Variant::INT || p_type == Variant::BOOL || p_type == Variant::COLOR;
}

bool TimeOfDayProfile::sample(int p_index, double p_hour, Variant::Type p_type, Variant &r_value) const {
	ERR_FAIL_INDEX_V(p_index, (int)tracks.size(), false);
	const Track &track = tracks[p_index];

	if (p_type == Variant::COLOR) {
		if (track.gradient.is_null()) {
			return false;
		}
		r_value = track.gradient->get_color_at_offset(p_hour / 24.0);
		return true;
	}

	if (track.curve.is_null()) {
		return false;
	}
	const double value = track.curve->sample(hour_to_curve_offset(track.curve, p_hour));
	switch (p_type) {
		case Variant::FLOAT:
			r_value = value;
			return true;
		case Variant::INT:
			r_value = (int64_t)Math::round(value);
			return true;
		case Variant::BOOL:
			r_value = value >= 0.5;
			return true;
		default:
			return false;
	}
}

String TimeOfDayProfile::get_target_name(Target p_target) {
	switch (p_target) {
		case TARGET_SUN:
			return "Sun";
		case TARGET_MOON:
			return "Moon";
		case TARGET_ENVIRONMENT:
			return "Environment";
		case TARGET_SKY_MATERIAL:
			return "Sky Material";
		case TARGET_CAMERA_ATTRIBUTES:
			return "Camera Attributes";
		case TARGET_NODE:
			return "Node";
		case TARGET_MAX:
			break;
	}
	return String();
}

void TimeOfDayProfile::set_context(TimeOfDay *p_context) {
	const ObjectID id = p_context ? p_context->get_instance_id() : ObjectID();
	if (context == id) {
		return;
	}
	context = id;
	// The properties the Inspector suggests come from the new context's targets.
	notify_property_list_changed();
}

Object *TimeOfDayProfile::_get_context_target(const Track &p_track) const {
	const TimeOfDay *time_of_day = ObjectDB::get_instance<TimeOfDay>(context);
	// Only the thread that owns the node may look into its scene. Elsewhere
	// (the editor saves, duplicates and previews resources on worker threads)
	// the Inspector hints fall back on the targets' usual classes.
	if (!time_of_day || !time_of_day->is_accessible_from_caller_thread() || !time_of_day->is_inside_tree()) {
		return nullptr;
	}
	return time_of_day->get_target_object(p_track.target, p_track.node_path);
}

// Without a TimeOfDay to ask, fall back on the classes each target usually is.
static Vector<StringName> _get_target_classes(TimeOfDayProfile::Target p_target) {
	switch (p_target) {
		case TimeOfDayProfile::TARGET_SUN:
		case TimeOfDayProfile::TARGET_MOON:
			return { SNAME("DirectionalLight3D") };
		case TimeOfDayProfile::TARGET_ENVIRONMENT:
			return { SNAME("Environment") };
		case TimeOfDayProfile::TARGET_SKY_MATERIAL:
			return { SNAME("ProceduralSkyMaterial"), SNAME("PhysicalSkyMaterial"), SNAME("PanoramaSkyMaterial") };
		case TimeOfDayProfile::TARGET_CAMERA_ATTRIBUTES:
			return { SNAME("CameraAttributesPractical"), SNAME("CameraAttributesPhysical") };
		default:
			return Vector<StringName>();
	}
}

Variant::Type TimeOfDayProfile::_get_track_value_type(int p_index) const {
	const Track &track = tracks[p_index];
	if (track.property_path.is_empty()) {
		return Variant::NIL;
	}

	const Object *object = _get_context_target(track);
	if (object) {
		bool valid = false;
		const Variant value = object->get_indexed(track.property_path, &valid);
		return valid ? value.get_type() : Variant::NIL;
	}

	if (track.property_path.size() == 1) {
		for (const StringName &class_name : _get_target_classes(track.target)) {
			PropertyInfo info;
			if (ClassDB::get_property_info(class_name, track.property, &info)) {
				return info.type;
			}
		}
	}
	return Variant::NIL;
}

static bool _is_suggested_property(const PropertyInfo &p_info, TimeOfDayProfile::Target p_target) {
	if (!(p_info.usage & PROPERTY_USAGE_EDITOR)) {
		return false;
	}
	if (p_info.usage & (PROPERTY_USAGE_CATEGORY | PROPERTY_USAGE_GROUP | PROPERTY_USAGE_SUBGROUP | PROPERTY_USAGE_ARRAY | PROPERTY_USAGE_READ_ONLY | PROPERTY_USAGE_INTERNAL)) {
		return false;
	}
	if (!TimeOfDayProfile::is_drivable_type(p_info.type)) {
		return false;
	}

	const String &name = p_info.name;
	if (name.begins_with("_") || name.begins_with("resource_")) {
		return false;
	}
	// Node's own settings (process mode, priorities...) are never worth a curve.
	if (ClassDB::has_property(SNAME("Node"), name, true)) {
		return false;
	}
	// TimeOfDay shows and hides the sun and the moon itself.
	if ((p_target == TimeOfDayProfile::TARGET_SUN || p_target == TimeOfDayProfile::TARGET_MOON) && name == "visible") {
		return false;
	}
	return true;
}

String TimeOfDayProfile::_get_property_suggestions(int p_index) const {
	const Track &track = tracks[p_index];

	List<PropertyInfo> properties;
	const Object *object = _get_context_target(track);
	if (object) {
		object->get_property_list(&properties);
	} else {
		for (const StringName &class_name : _get_target_classes(track.target)) {
			ClassDB::get_property_list(class_name, &properties);
		}
	}

	PackedStringArray names;
	HashSet<String> seen;
	for (const PropertyInfo &info : properties) {
		if (!_is_suggested_property(info, track.target) || seen.has(info.name)) {
			continue;
		}
		seen.insert(info.name);
		names.push_back(info.name);
	}
	return String(",").join(names);
}

// Rounds up to 1, 2 or 5 times a power of ten, for a curve range that is not
// cramped around the value it starts from.
static double _nice_ceil(double p_value) {
	if (p_value <= 0.0) {
		return 1.0;
	}
	const double magnitude = Math::pow(10.0, Math::floor(Math::log(p_value) / Math::log(10.0)));
	for (const double step : { 1.0, 2.0, 5.0, 10.0 }) {
		if (step * magnitude >= p_value * (1.0 - CMP_EPSILON)) {
			return step * magnitude;
		}
	}
	return 10.0 * magnitude;
}

// Picking a property in the Inspector gives the track something to edit right
// away: a flat Curve or Gradient at the value the property has now, laid out
// over 24 hours, rather than an empty slot and a curve spanning 0 to 1.
void TimeOfDayProfile::_create_track_resource(int p_index) {
	Track &track = tracks[p_index];
	if (track.curve.is_valid() || track.gradient.is_valid()) {
		return;
	}
	const Object *object = _get_context_target(track);
	if (!object) {
		return;
	}
	bool valid = false;
	const Variant value = object->get_indexed(track.property_path, &valid);
	if (!valid || !is_drivable_type(value.get_type())) {
		return;
	}

	if (value.get_type() == Variant::COLOR) {
		Ref<Gradient> gradient;
		gradient.instantiate();
		gradient->set_offsets({ 0.0, 1.0 });
		gradient->set_colors({ value, value });
		set_track_gradient(p_index, gradient);
		return;
	}

	const double number = value;
	Ref<Curve> curve;
	curve.instantiate();
	curve->set_max_domain(24.0);
	if (value.get_type() != Variant::BOOL) {
		curve->set_max_value(number > 0.0 ? _nice_ceil(number * 2.0) : 1.0);
		curve->set_min_value(number < 0.0 ? -_nice_ceil(-number * 2.0) : 0.0);
	}
	curve->add_point(Vector2(0.0, number));
	curve->add_point(Vector2(24.0, number));
	set_track_curve(p_index, curve);
}

bool TimeOfDayProfile::_set(const StringName &p_name, const Variant &p_value) {
	const String path = p_name;
	if (!path.begins_with("tracks/")) {
		return false;
	}
	const int index = path.get_slicec('/', 1).to_int();
	const String what = path.get_slicec('/', 2);
	ERR_FAIL_INDEX_V(index, (int)tracks.size(), false);

	if (what == "target") {
		set_track_target(index, Target((int)p_value));
	} else if (what == "node_path") {
		set_track_node_path(index, p_value);
	} else if (what == "property") {
		set_track_property(index, p_value);
		if (Engine::get_singleton()->is_editor_hint()) {
			_create_track_resource(index);
		}
	} else if (what == "curve") {
		set_track_curve(index, p_value);
	} else if (what == "gradient") {
		set_track_gradient(index, p_value);
	} else {
		return false;
	}
	return true;
}

bool TimeOfDayProfile::_get(const StringName &p_name, Variant &r_ret) const {
	const String path = p_name;
	if (!path.begins_with("tracks/")) {
		return false;
	}
	const int index = path.get_slicec('/', 1).to_int();
	const String what = path.get_slicec('/', 2);
	ERR_FAIL_INDEX_V(index, (int)tracks.size(), false);
	const Track &track = tracks[index];

	if (what == "target") {
		r_ret = track.target;
	} else if (what == "node_path") {
		r_ret = track.node_path;
	} else if (what == "property") {
		r_ret = track.property;
	} else if (what == "curve") {
		r_ret = track.curve;
	} else if (what == "gradient") {
		r_ret = track.gradient;
	} else {
		return false;
	}
	return true;
}

void TimeOfDayProfile::_get_property_list(List<PropertyInfo> *p_list) const {
	for (uint32_t i = 0; i < tracks.size(); i++) {
		const Track &track = tracks[i];
		const String prefix = "tracks/" + itos(i) + "/";

		p_list->push_back(PropertyInfo(Variant::INT, prefix + "target", PROPERTY_HINT_ENUM, TRACK_TARGET_HINT));
		if (track.target == TARGET_NODE) {
			p_list->push_back(PropertyInfo(Variant::NODE_PATH, prefix + "node_path"));
		}
		p_list->push_back(PropertyInfo(Variant::STRING_NAME, prefix + "property", PROPERTY_HINT_ENUM_SUGGESTION, _get_property_suggestions(i)));

		// Offer only what the property can use, a Curve for numbers and a
		// Gradient for colors, and both while its type is unknown. Neither is
		// ever hidden while it holds something, since a hidden property is not
		// saved either.
		const Variant::Type type = _get_track_value_type(i);
		const bool unknown = !is_drivable_type(type);
		if (unknown || type != Variant::COLOR || track.curve.is_valid()) {
			p_list->push_back(PropertyInfo(Variant::OBJECT, prefix + "curve", PROPERTY_HINT_RESOURCE_TYPE, Curve::get_class_static()));
		}
		if (unknown || type == Variant::COLOR || track.gradient.is_valid()) {
			p_list->push_back(PropertyInfo(Variant::OBJECT, prefix + "gradient", PROPERTY_HINT_RESOURCE_TYPE, Gradient::get_class_static()));
		}
	}
}

bool TimeOfDayProfile::_property_can_revert(const StringName &p_name) const {
	const String path = p_name;
	if (!path.begins_with("tracks/")) {
		return false;
	}
	const int index = path.get_slicec('/', 1).to_int();
	const String what = path.get_slicec('/', 2);
	if (index < 0 || index >= (int)tracks.size()) {
		return false;
	}
	const Track &track = tracks[index];

	if (what == "target") {
		return track.target != TARGET_SUN;
	} else if (what == "node_path") {
		return !track.node_path.is_empty();
	} else if (what == "property") {
		return track.property != StringName();
	} else if (what == "curve") {
		return track.curve.is_valid();
	} else if (what == "gradient") {
		return track.gradient.is_valid();
	}
	return false;
}

bool TimeOfDayProfile::_property_get_revert(const StringName &p_name, Variant &r_property) const {
	const String path = p_name;
	if (!path.begins_with("tracks/")) {
		return false;
	}
	const String what = path.get_slicec('/', 2);

	if (what == "target") {
		r_property = TARGET_SUN;
	} else if (what == "node_path") {
		r_property = NodePath();
	} else if (what == "property") {
		r_property = StringName();
	} else if (what == "curve" || what == "gradient") {
		r_property = Variant();
	} else {
		return false;
	}
	return true;
}

void TimeOfDayProfile::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_track_count", "count"), &TimeOfDayProfile::set_track_count);
	ClassDB::bind_method(D_METHOD("get_track_count"), &TimeOfDayProfile::get_track_count);

	ClassDB::bind_method(D_METHOD("add_track", "target", "property", "node_path"), &TimeOfDayProfile::add_track, DEFVAL(NodePath()));
	ClassDB::bind_method(D_METHOD("remove_track", "index"), &TimeOfDayProfile::remove_track);
	ClassDB::bind_method(D_METHOD("clear_tracks"), &TimeOfDayProfile::clear_tracks);
	ClassDB::bind_method(D_METHOD("find_track", "target", "property", "node_path"), &TimeOfDayProfile::find_track, DEFVAL(NodePath()));

	ClassDB::bind_method(D_METHOD("set_track_target", "index", "target"), &TimeOfDayProfile::set_track_target);
	ClassDB::bind_method(D_METHOD("get_track_target", "index"), &TimeOfDayProfile::get_track_target);
	ClassDB::bind_method(D_METHOD("set_track_node_path", "index", "node_path"), &TimeOfDayProfile::set_track_node_path);
	ClassDB::bind_method(D_METHOD("get_track_node_path", "index"), &TimeOfDayProfile::get_track_node_path);
	ClassDB::bind_method(D_METHOD("set_track_property", "index", "property"), &TimeOfDayProfile::set_track_property);
	ClassDB::bind_method(D_METHOD("get_track_property", "index"), &TimeOfDayProfile::get_track_property);
	ClassDB::bind_method(D_METHOD("set_track_curve", "index", "curve"), &TimeOfDayProfile::set_track_curve);
	ClassDB::bind_method(D_METHOD("get_track_curve", "index"), &TimeOfDayProfile::get_track_curve);
	ClassDB::bind_method(D_METHOD("set_track_gradient", "index", "gradient"), &TimeOfDayProfile::set_track_gradient);
	ClassDB::bind_method(D_METHOD("get_track_gradient", "index"), &TimeOfDayProfile::get_track_gradient);

	ClassDB::bind_static_method("TimeOfDayProfile", D_METHOD("create_default"), &TimeOfDayProfile::create_default);

	// Lets the Inspector reorder tracks without losing the parts it is not showing.
	ClassDB::bind_method(D_METHOD("_swap_tracks", "a", "b"), &TimeOfDayProfile::_swap_tracks);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "track_count", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_ARRAY, "Tracks,tracks/,page_size=64,numbered,swap_method=_swap_tracks,add_button_text=" + String(TTRC("Add Track"))), "set_track_count", "get_track_count");

	BIND_ENUM_CONSTANT(TARGET_SUN);
	BIND_ENUM_CONSTANT(TARGET_MOON);
	BIND_ENUM_CONSTANT(TARGET_ENVIRONMENT);
	BIND_ENUM_CONSTANT(TARGET_SKY_MATERIAL);
	BIND_ENUM_CONSTANT(TARGET_CAMERA_ATTRIBUTES);
	BIND_ENUM_CONSTANT(TARGET_NODE);
}

// The default day.

// A curve over the 24 hours of the day through p_points (in hours), with
// Fritsch-Butland tangents: smooth, but never overshooting its points, so an
// energy that rests at 0 through the night does not dip below it before dawn.
// The ends are flat, which keeps the slope continuous across midnight.
static Ref<Curve> _make_day_curve(const Vector<Vector2> &p_points, real_t p_min_value, real_t p_max_value) {
	Ref<Curve> curve;
	curve.instantiate();
	curve->set_max_domain(24.0);
	curve->set_max_value(p_max_value);
	curve->set_min_value(p_min_value);

	const int count = p_points.size();
	for (int i = 0; i < count; i++) {
		real_t tangent = 0.0;
		if (i > 0 && i < count - 1) {
			const Vector2 &previous = p_points[i - 1];
			const Vector2 &point = p_points[i];
			const Vector2 &next = p_points[i + 1];
			const real_t h0 = point.x - previous.x;
			const real_t h1 = next.x - point.x;
			const real_t d0 = (point.y - previous.y) / h0;
			const real_t d1 = (next.y - point.y) / h1;
			if (d0 * d1 > 0.0) {
				const real_t w0 = 2.0 * h1 + h0;
				const real_t w1 = h1 + 2.0 * h0;
				tangent = (w0 + w1) / (w0 / d0 + w1 / d1);
			}
		}
		curve->add_point(p_points[i], tangent, tangent);
	}
	return curve;
}

struct DayColorKey {
	real_t hour = 0.0;
	Color color;
};

static Ref<Gradient> _make_day_gradient(const Vector<DayColorKey> &p_keys) {
	Vector<float> offsets;
	Vector<Color> colors;
	for (const DayColorKey &key : p_keys) {
		offsets.push_back(key.hour / 24.0);
		colors.push_back(key.color);
	}
	Ref<Gradient> gradient;
	gradient.instantiate();
	gradient->set_offsets(offsets);
	gradient->set_colors(colors);
	return gradient;
}

Ref<TimeOfDayProfile> TimeOfDayProfile::create_default() {
	Ref<TimeOfDayProfile> profile;
	profile.instantiate();

	// The keys assume sunrise at 6:00 and sunset at 18:00. TimeOfDay stretches
	// the day to the real ones when align_curves_to_sun is on.

	// The sun keeps its energy all the way down to the horizon. A low sun
	// already lights the ground at a grazing angle, which dims it enough, and
	// sky materials draw the sun's disk and glow from the light's energy: a
	// sun fading out as it sets would show up as a dark smudge in a bright
	// sky. TimeOfDay hides it once it is below the horizon.
	int track = profile->add_track(TARGET_SUN, SNAME("light_energy"));
	profile->set_track_curve(track, _make_day_curve({ Vector2(0.0, 0.0), Vector2(5.9, 0.0), Vector2(6.0, 1.0), Vector2(12.0, 1.0), Vector2(18.0, 1.0), Vector2(18.1, 0.0), Vector2(24.0, 0.0) }, 0.0, 1.2));

	const Color sun_low = Color(1.0, 0.36, 0.14);
	track = profile->add_track(TARGET_SUN, SNAME("light_color"));
	profile->set_track_gradient(track, _make_day_gradient({
												   { 0.0, sun_low },
												   { 6.0, Color(1.0, 0.42, 0.18) },
												   { 6.75, Color(1.0, 0.63, 0.37) },
												   { 8.0, Color(1.0, 0.86, 0.7) },
												   { 10.0, Color(1.0, 0.96, 0.9) },
												   { 12.0, Color(1.0, 0.98, 0.95) },
												   { 14.0, Color(1.0, 0.96, 0.9) },
												   { 16.0, Color(1.0, 0.86, 0.7) },
												   { 17.25, Color(1.0, 0.63, 0.37) },
												   { 18.0, Color(1.0, 0.42, 0.18) },
												   { 24.0, sun_low },
										   }));

	// The moon takes over once the sun is down.
	track = profile->add_track(TARGET_MOON, SNAME("light_energy"));
	profile->set_track_curve(track, _make_day_curve({ Vector2(0.0, 0.12), Vector2(4.75, 0.12), Vector2(5.75, 0.0), Vector2(18.25, 0.0), Vector2(19.25, 0.12), Vector2(24.0, 0.12) }, 0.0, 0.5));

	track = profile->add_track(TARGET_MOON, SNAME("light_color"));
	profile->set_track_gradient(track, _make_day_gradient({ { 0.0, Color(0.62, 0.72, 1.0) } }));

	// ProceduralSkyMaterial: a deep blue night, a warm glow along the horizon
	// at dawn and dusk, and the material's own colors through the day.
	const Color night_top = Color(0.012, 0.018, 0.045);
	track = profile->add_track(TARGET_SKY_MATERIAL, SNAME("sky_top_color"));
	profile->set_track_gradient(track, _make_day_gradient({
												   { 0.0, night_top },
												   { 4.5, Color(0.02, 0.03, 0.07) },
												   { 5.25, Color(0.06, 0.08, 0.17) },
												   { 6.0, Color(0.19, 0.23, 0.4) },
												   { 7.0, Color(0.3, 0.4, 0.58) },
												   { 9.0, Color(0.385, 0.454, 0.55) },
												   { 15.0, Color(0.385, 0.454, 0.55) },
												   { 17.0, Color(0.3, 0.38, 0.54) },
												   { 18.0, Color(0.2, 0.2, 0.38) },
												   { 18.75, Color(0.07, 0.07, 0.16) },
												   { 19.5, Color(0.02, 0.03, 0.07) },
												   { 24.0, night_top },
										   }));

	const Vector<DayColorKey> horizon = {
		{ 0.0, Color(0.025, 0.03, 0.06) },
		{ 4.75, Color(0.05, 0.06, 0.11) },
		{ 5.5, Color(0.35, 0.24, 0.26) },
		{ 6.0, Color(0.72, 0.44, 0.26) },
		{ 6.75, Color(0.8, 0.7, 0.58) },
		{ 8.0, Color(0.66, 0.67, 0.68) },
		{ 12.0, Color(0.6463, 0.6558, 0.6708) },
		{ 16.0, Color(0.66, 0.67, 0.68) },
		{ 17.25, Color(0.82, 0.66, 0.5) },
		{ 18.0, Color(0.74, 0.4, 0.22) },
		{ 18.5, Color(0.45, 0.25, 0.28) },
		{ 19.25, Color(0.06, 0.06, 0.12) },
		{ 24.0, Color(0.025, 0.03, 0.06) },
	};
	track = profile->add_track(TARGET_SKY_MATERIAL, SNAME("sky_horizon_color"));
	profile->set_track_gradient(track, _make_day_gradient(horizon));
	track = profile->add_track(TARGET_SKY_MATERIAL, SNAME("ground_horizon_color"));
	profile->set_track_gradient(track, _make_day_gradient(horizon));

	const Color night_ground = Color(0.01, 0.01, 0.012);
	track = profile->add_track(TARGET_SKY_MATERIAL, SNAME("ground_bottom_color"));
	profile->set_track_gradient(track, _make_day_gradient({
												   { 0.0, night_ground },
												   { 5.5, night_ground },
												   { 7.0, Color(0.2, 0.169, 0.133) },
												   { 17.0, Color(0.2, 0.169, 0.133) },
												   { 18.5, night_ground },
												   { 24.0, night_ground },
										   }));

	// Fog picks up the color of the light around it, and thickens in the cool
	// of the early morning.
	const Color night_fog = Color(0.02, 0.025, 0.05);
	track = profile->add_track(TARGET_ENVIRONMENT, SNAME("fog_light_color"));
	profile->set_track_gradient(track, _make_day_gradient({
												   { 0.0, night_fog },
												   { 5.0, Color(0.05, 0.055, 0.09) },
												   { 6.0, Color(0.75, 0.5, 0.35) },
												   { 8.0, Color(0.518, 0.553, 0.608) },
												   { 16.0, Color(0.518, 0.553, 0.608) },
												   { 17.75, Color(0.72, 0.52, 0.38) },
												   { 18.75, Color(0.2, 0.14, 0.18) },
												   { 20.0, night_fog },
												   { 24.0, night_fog },
										   }));

	track = profile->add_track(TARGET_ENVIRONMENT, SNAME("fog_density"));
	profile->set_track_curve(track, _make_day_curve({ Vector2(0.0, 0.004), Vector2(5.5, 0.006), Vector2(7.0, 0.005), Vector2(10.0, 0.0015), Vector2(16.0, 0.0015), Vector2(19.0, 0.003), Vector2(24.0, 0.004) }, 0.0, 0.01));

	profile->set_name("Default Day");
	return profile;
}
