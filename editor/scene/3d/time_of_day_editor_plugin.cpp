/**************************************************************************/
/*  time_of_day_editor_plugin.cpp                                         */
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

#include "time_of_day_editor_plugin.h"

#include "core/input/input_event.h"
#include "core/object/callable_mp.h"
#include "editor/editor_string_names.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/gui/editor_toaster.h"
#include "editor/themes/editor_scale.h"
#include "scene/3d/time_of_day.h"
#include "scene/gui/button.h"

// The strip's height above the hour labels.
static constexpr float BAND_HEIGHT = 56.0;
// How the path of the sun and the moon is sampled across the day.
static constexpr int PATH_SAMPLES = 96;

static String _format_time(double p_hours) {
	const int minutes = int(Math::round(p_hours * 60.0)) % (24 * 60);
	return vformat("%02d:%02d", minutes / 60, minutes % 60);
}

TimeOfDay *TimeOfDayTimeline::_get_time_of_day() const {
	return ObjectDB::get_instance<TimeOfDay>(time_of_day);
}

Rect2 TimeOfDayTimeline::_get_band_rect() const {
	return Rect2(0, 0, get_size().x, BAND_HEIGHT * EDSCALE);
}

double TimeOfDayTimeline::_get_time_at(float p_x, bool p_snap) const {
	const Rect2 band = _get_band_rect();
	double hours = CLAMP((p_x - band.position.x) / MAX(1.0f, band.size.x), 0.0f, 1.0f) * 24.0;
	// To the minute, or with Ctrl, to the quarter of an hour.
	const double step = p_snap ? 0.25 : 1.0 / 60.0;
	hours = Math::snapped(hours, step);
	// 24:00 is the next midnight, which would send the cursor back to the left.
	return MIN(hours, 24.0 - 1.0 / 60.0);
}

// Where the sky's colors come from: the profile's own sky tracks when it has
// them, otherwise an impression of the sky from how high the sun is.
static int _find_sky_track(const Ref<TimeOfDayProfile> &p_profile, const StringName &p_property) {
	if (p_profile.is_null()) {
		return -1;
	}
	const int track = p_profile->find_track(TimeOfDayProfile::TARGET_SKY_MATERIAL, p_property);
	return (track >= 0 && p_profile->get_track_gradient(track).is_valid()) ? track : -1;
}

static void _get_sky_colors(const TimeOfDay *p_time_of_day, double p_time, int p_top_track, int p_horizon_track, Color &r_top, Color &r_horizon) {
	const Ref<TimeOfDayProfile> profile = p_time_of_day->get_profile();
	const double curve_time = p_time_of_day->get_curve_time(p_time);

	const double elevation = Math::rad_to_deg(Math::asin(CLAMP(double(p_time_of_day->get_sun_direction_at(p_time).y), -1.0, 1.0)));
	const float day_amount = Math::smoothstep(-8.0, 8.0, elevation);
	const float glow = MAX(0.0, 1.0 - Math::abs(elevation) / 8.0);
	r_top = Color(0.02, 0.03, 0.07).lerp(Color(0.33, 0.45, 0.62), day_amount);
	r_horizon = Color(0.04, 0.05, 0.1).lerp(Color(0.62, 0.66, 0.72), day_amount).lerp(Color(0.9, 0.5, 0.28), glow * 0.8f);

	Variant value;
	if (p_top_track >= 0 && profile->sample(p_top_track, curve_time, Variant::COLOR, value)) {
		r_top = value;
	}
	if (p_horizon_track >= 0 && profile->sample(p_horizon_track, curve_time, Variant::COLOR, value)) {
		r_horizon = value;
	}
}

uint32_t TimeOfDayTimeline::_get_state_hash() const {
	const TimeOfDay *node = _get_time_of_day();
	if (!node) {
		return 0;
	}
	uint32_t hash = hash_murmur3_one_double(node->get_time());
	hash = hash_murmur3_one_double(node->get_latitude(), hash);
	hash = hash_murmur3_one_32(node->get_day_of_year(), hash);
	hash = hash_murmur3_one_double(node->get_north_offset(), hash);
	hash = hash_murmur3_one_double(node->get_moon_phase(), hash);
	hash = hash_murmur3_one_32(node->is_aligning_curves_to_sun(), hash);
	hash = hash_murmur3_one_32(node->get_moon_light_path().is_empty(), hash);
	hash = hash_murmur3_one_32(dragging, hash);
	hash = hash_murmur3_one_32(get_size().x, hash);

	const Ref<TimeOfDayProfile> profile = node->get_profile();
	for (const StringName &property : { SNAME("sky_top_color"), SNAME("sky_horizon_color") }) {
		const int track = _find_sky_track(profile, property);
		if (track < 0) {
			hash = hash_murmur3_one_32(0, hash);
			continue;
		}
		const Ref<Gradient> gradient = profile->get_track_gradient(track);
		for (const float offset : gradient->get_offsets()) {
			hash = hash_murmur3_one_float(offset, hash);
		}
		for (const Color &color : gradient->get_colors()) {
			hash = hash_murmur3_one_32(color.to_rgba32(), hash);
		}
	}
	return hash_fmix32(hash);
}

void TimeOfDayTimeline::_draw_timeline() {
	TimeOfDay *node = _get_time_of_day();
	if (!node) {
		return;
	}
	const Rect2 band = _get_band_rect();
	if (band.size.x < 2.0) {
		return;
	}

	const Ref<Font> font = get_theme_font(SceneStringName(font), SNAME("Label"));
	const int font_size = get_theme_font_size(SceneStringName(font_size), SNAME("Label"));
	const Color font_color = get_theme_color(SceneStringName(font_color), SNAME("Label"));
	const Color accent_color = get_theme_color(SNAME("accent_color"), EditorStringName(Editor));

	// The sky, column by column, its zenith at the top and its horizon at the
	// bottom.
	const Ref<TimeOfDayProfile> profile = node->get_profile();
	const int top_track = _find_sky_track(profile, SNAME("sky_top_color"));
	const int horizon_track = _find_sky_track(profile, SNAME("sky_horizon_color"));
	const float column_width = MAX(1.0f, Math::round(2.0f * EDSCALE));
	for (float x = 0.0f; x < band.size.x; x += column_width) {
		Color top;
		Color horizon;
		_get_sky_colors(node, (x + column_width * 0.5f) / band.size.x * 24.0, top_track, horizon_track, top, horizon);
		const float right = MIN(x + column_width, band.size.x);
		const Vector<Point2> points = { band.position + Point2(x, 0), band.position + Point2(right, 0), band.position + Point2(right, band.size.y), band.position + Point2(x, band.size.y) };
		draw_polygon(points, { top, top, horizon, horizon });
	}

	// The horizon, then the paths of the moon and the sun: -90 degrees at the
	// bottom of the strip, the zenith at the top.
	const float horizon_y = band.position.y + band.size.y * 0.5f;
	draw_line(Point2(band.position.x, horizon_y), Point2(band.get_end().x, horizon_y), Color(1, 1, 1, 0.25), MAX(1.0f, EDSCALE));

	const float line_width = MAX(1.0f, 1.5f * EDSCALE);
	auto elevation_to_y = [&](const Vector3 &p_direction) {
		return band.position.y + band.size.y * (0.5f - Math::asin(CLAMP(p_direction.y, -1.0f, 1.0f)) / Math::PI);
	};
	auto draw_path = [&](bool p_moon, const Color &p_color) {
		Point2 previous;
		for (int i = 0; i <= PATH_SAMPLES; i++) {
			const double hours = 24.0 * i / PATH_SAMPLES;
			const Vector3 direction = p_moon ? node->get_moon_direction_at(hours) : node->get_sun_direction_at(hours);
			const Point2 point = Point2(band.position.x + band.size.x * i / PATH_SAMPLES, elevation_to_y(direction));
			if (i > 0) {
				// Below the horizon the path is only hinted at.
				const bool up = previous.y < horizon_y && point.y < horizon_y;
				draw_line(previous, point, up ? p_color : Color(p_color, p_color.a * 0.3f), line_width, true);
			}
			previous = point;
		}
	};
	const Color moon_color = Color(0.7, 0.8, 1.0, 0.85);
	const Color sun_color = Color(1.0, 0.8, 0.35);
	const bool has_moon = !node->get_moon_light_path().is_empty();
	if (has_moon) {
		draw_path(true, moon_color);
	}
	draw_path(false, sun_color);

	// Sunrise and sunset.
	double sunrise = 0.0;
	double sunset = 0.0;
	if (node->get_sun_events(sunrise, sunset)) {
		const float tick = 5.0f * EDSCALE;
		for (const double event : { sunrise, sunset }) {
			const float x = band.position.x + band.size.x * event / 24.0;
			draw_line(Point2(x, horizon_y - tick), Point2(x, horizon_y + tick), sun_color, line_width);
		}
	}

	// The hours below the strip.
	const float label_y = band.get_end().y + 2.0f * EDSCALE + font->get_ascent(font_size);
	for (int hour = 0; hour <= 24; hour++) {
		const float x = band.position.x + band.size.x * hour / 24.0f;
		const bool major = hour % 6 == 0;
		draw_line(Point2(x, band.get_end().y), Point2(x, band.get_end().y + (major ? 4.0f : 2.0f) * EDSCALE), Color(font_color, major ? 0.8f : 0.4f), MAX(1.0f, EDSCALE));
		if (!major) {
			continue;
		}
		const String text = vformat("%d:00", hour);
		const float width = font->get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x;
		const float text_x = CLAMP(x - width * 0.5f, 0.0f, get_size().x - width);
		draw_string(font, Point2(text_x, label_y), text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, Color(font_color, 0.7f));
	}

	// Now: the sun and moon where they stand, and the time.
	const double time = node->get_time();
	const float now_x = band.position.x + band.size.x * time / 24.0;
	const float radius = 4.0f * EDSCALE;
	if (has_moon) {
		draw_circle(Point2(now_x, elevation_to_y(node->get_moon_direction())), radius * 0.8f, moon_color, true, -1.0f, true);
	}
	draw_circle(Point2(now_x, elevation_to_y(node->get_sun_direction())), radius, sun_color, true, -1.0f, true);
	draw_line(Point2(now_x, band.position.y), Point2(now_x, band.get_end().y), accent_color, MAX(1.0f, 2.0f * EDSCALE));

	const String now_text = _format_time(time);
	const Vector2 text_size = font->get_string_size(now_text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size);
	const float margin = 3.0f * EDSCALE;
	float box_x = now_x + margin;
	if (box_x + text_size.x + margin * 2.0f > band.get_end().x) {
		box_x = now_x - margin - text_size.x - margin * 2.0f;
	}
	const Rect2 box = Rect2(box_x, band.position.y + margin, text_size.x + margin * 2.0f, text_size.y);
	draw_rect(box, Color(0, 0, 0, 0.55));
	draw_string(font, Point2(box.position.x + margin, box.position.y + font->get_ascent(font_size)), now_text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, Color(1, 1, 1));
}

void TimeOfDayTimeline::gui_input(const Ref<InputEvent> &p_event) {
	TimeOfDay *node = _get_time_of_day();
	if (!node) {
		return;
	}

	const Ref<InputEventMouseButton> mouse_button = p_event;
	if (mouse_button.is_valid() && mouse_button->get_button_index() == MouseButton::LEFT) {
		if (mouse_button->is_pressed()) {
			dragging = true;
			drag_start_time = node->get_time();
			node->set_time(_get_time_at(mouse_button->get_position().x, mouse_button->is_command_or_control_pressed()));
		} else if (dragging) {
			dragging = false;
			// The time has followed the mouse all along; the action only records
			// where the drag started and ended.
			const double end_time = node->get_time();
			if (end_time != drag_start_time) {
				EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
				undo_redo->create_action(TTR("Change Time of Day"), UndoRedo::MERGE_DISABLE, node);
				undo_redo->add_do_property(node, "time", end_time);
				undo_redo->add_undo_property(node, "time", drag_start_time);
				undo_redo->commit_action(false);
			}
		}
		queue_redraw();
		accept_event();
		return;
	}

	const Ref<InputEventMouseMotion> mouse_motion = p_event;
	if (mouse_motion.is_valid() && dragging) {
		node->set_time(_get_time_at(mouse_motion->get_position().x, mouse_motion->is_command_or_control_pressed()));
		queue_redraw();
		accept_event();
	}
}

String TimeOfDayTimeline::get_tooltip(const Point2 &p_pos) const {
	return vformat(TTR("%s\nDrag to change the time, hold Ctrl to snap to quarter hours."), _format_time(_get_time_at(p_pos.x, false)));
}

void TimeOfDayTimeline::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			const Ref<Font> font = get_theme_font(SceneStringName(font), SNAME("Label"));
			const int font_size = get_theme_font_size(SceneStringName(font_size), SNAME("Label"));
			set_custom_minimum_size(Size2(0, BAND_HEIGHT * EDSCALE + font->get_height(font_size) + 4.0f * EDSCALE));
		} break;

		case NOTIFICATION_PROCESS: {
			// The time moves under a running preview, and every property the
			// strip shows can change from the Inspector, undo, or scripts.
			const uint32_t state = _get_state_hash();
			if (state != drawn_state) {
				drawn_state = state;
				queue_redraw();
			}
		} break;

		case NOTIFICATION_DRAW: {
			_draw_timeline();
		} break;
	}
}

TimeOfDayTimeline::TimeOfDayTimeline(TimeOfDay *p_time_of_day) {
	time_of_day = p_time_of_day->get_instance_id();
	set_mouse_filter(MOUSE_FILTER_STOP);
	set_default_cursor_shape(CURSOR_HSIZE);
	set_process(true);
}

// TimeOfDayEditor

TimeOfDay *TimeOfDayEditor::_get_time_of_day() const {
	return ObjectDB::get_instance<TimeOfDay>(time_of_day);
}

void TimeOfDayEditor::_update_buttons() {
	const TimeOfDay *node = _get_time_of_day();
	const bool has_profile = node && node->get_profile().is_valid();
	if (create_profile_button->is_disabled() != has_profile) {
		create_profile_button->set_disabled(has_profile);
		create_profile_button->set_tooltip_text(has_profile ? TTR("This TimeOfDay already has a profile. Clear it first to start over from the default one.") : TTR("Give this TimeOfDay a new profile holding a whole default day: the sun, a moonlit night, the sky's colors through dawn and dusk, and a morning fog."));
	}
	if (key_button->is_disabled() == has_profile) {
		key_button->set_disabled(!has_profile);
	}
}

void TimeOfDayEditor::_create_default_profile() {
	TimeOfDay *node = _get_time_of_day();
	if (!node || node->get_profile().is_valid()) {
		return;
	}
	const Ref<TimeOfDayProfile> profile = node->make_default_profile();

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(TTR("Create Default Time of Day Profile"), UndoRedo::MERGE_DISABLE, node);
	undo_redo->add_do_property(node, "profile", profile);
	undo_redo->add_undo_property(node, "profile", Variant());
	undo_redo->commit_action();
}

// What keying can change, so that it can be undone: each Curve's points and
// range, and each Gradient's points, of every track in the profile.
struct TimeOfDayKeySnapshot {
	Ref<Resource> resource;
	Variant points;
	Variant range;
};

static LocalVector<TimeOfDayKeySnapshot> _snapshot_profile(const Ref<TimeOfDayProfile> &p_profile) {
	LocalVector<TimeOfDayKeySnapshot> snapshots;
	LocalVector<Resource *> seen;
	for (int i = 0; i < p_profile->get_track_count(); i++) {
		for (const Ref<Resource> &resource : { Ref<Resource>(p_profile->get_track_curve(i)), Ref<Resource>(p_profile->get_track_gradient(i)) }) {
			if (resource.is_null() || seen.has(resource.ptr())) {
				continue;
			}
			seen.push_back(resource.ptr());
			TimeOfDayKeySnapshot snapshot;
			snapshot.resource = resource;
			if (Object::cast_to<Curve>(resource.ptr())) {
				snapshot.points = resource->get("_data");
				snapshot.range = resource->get("_limits");
			} else {
				snapshot.points = resource->get("offsets");
				snapshot.range = resource->get("colors");
			}
			snapshots.push_back(snapshot);
		}
	}
	return snapshots;
}

void TimeOfDayEditor::_key_changed_values() {
	TimeOfDay *node = _get_time_of_day();
	if (!node || node->get_profile().is_null()) {
		return;
	}
	if (node->get_weather().is_valid() && node->get_weather_intensity() > 0.0) {
		EditorToaster::get_singleton()->popup_str(TTR("The weather is part of what the lights and the environment show right now. Set the weather intensity to 0 before keying, so that it doesn't end up in the profile."), EditorToaster::SEVERITY_WARNING);
		return;
	}

	const Ref<TimeOfDayProfile> profile = node->get_profile();
	const LocalVector<TimeOfDayKeySnapshot> before = _snapshot_profile(profile);
	const int keyed = node->key_changed_values();
	if (keyed <= 0) {
		EditorToaster::get_singleton()->popup_str(TTR("Nothing to key: everything the profile drives already matches it at this time. Change the lights or the environment by hand first, then key the changes."));
		return;
	}
	const LocalVector<TimeOfDayKeySnapshot> after = _snapshot_profile(profile);

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(vformat(TTRN("Key %d Time of Day Value", "Key %d Time of Day Values", keyed), keyed), UndoRedo::MERGE_DISABLE, node);
	for (uint32_t i = 0; i < before.size() && i < after.size(); i++) {
		const TimeOfDayKeySnapshot &old_state = before[i];
		const TimeOfDayKeySnapshot &new_state = after[i];
		if (old_state.points == new_state.points && old_state.range == new_state.range) {
			continue;
		}
		Resource *resource = new_state.resource.ptr();
		const bool is_curve = Object::cast_to<Curve>(resource) != nullptr;
		const StringName points_property = is_curve ? SNAME("_data") : SNAME("offsets");
		const StringName range_property = is_curve ? SNAME("_limits") : SNAME("colors");
		undo_redo->add_do_property(resource, range_property, new_state.range);
		undo_redo->add_do_property(resource, points_property, new_state.points);
		undo_redo->add_undo_property(resource, range_property, old_state.range);
		undo_redo->add_undo_property(resource, points_property, old_state.points);
	}
	// The keys are in already.
	undo_redo->commit_action(false);
}

void TimeOfDayEditor::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_PROCESS: {
			_update_buttons();
		} break;
	}
}

TimeOfDayEditor::TimeOfDayEditor(TimeOfDay *p_time_of_day) {
	time_of_day = p_time_of_day->get_instance_id();

	TimeOfDayTimeline *timeline = memnew(TimeOfDayTimeline(p_time_of_day));
	add_child(timeline);

	HBoxContainer *buttons = memnew(HBoxContainer);
	add_child(buttons);

	create_profile_button = memnew(Button);
	create_profile_button->set_text(TTR("Create Default Profile"));
	create_profile_button->set_h_size_flags(SIZE_EXPAND_FILL);
	create_profile_button->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	create_profile_button->connect(SceneStringName(pressed), callable_mp(this, &TimeOfDayEditor::_create_default_profile));
	buttons->add_child(create_profile_button);

	key_button = memnew(Button);
	key_button->set_text(TTR("Key Changed Values"));
	key_button->set_tooltip_text(TTR("Write what has been changed by hand on the sun, the moon, the environment or any other driven node into the profile, as keys at the current time."));
	key_button->set_h_size_flags(SIZE_EXPAND_FILL);
	key_button->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	key_button->connect(SceneStringName(pressed), callable_mp(this, &TimeOfDayEditor::_key_changed_values));
	buttons->add_child(key_button);

	// Starts out the opposite of what it should be, so the first update sets
	// both the state and the tooltip.
	create_profile_button->set_disabled(p_time_of_day->get_profile().is_null());
	key_button->set_disabled(p_time_of_day->get_profile().is_valid());
	_update_buttons();
	set_process(true);
}

// EditorInspectorPluginTimeOfDay

bool EditorInspectorPluginTimeOfDay::can_handle(Object *p_object) {
	return Object::cast_to<TimeOfDay>(p_object) != nullptr;
}

void EditorInspectorPluginTimeOfDay::parse_begin(Object *p_object) {
	TimeOfDay *node = Object::cast_to<TimeOfDay>(p_object);
	add_custom_control(memnew(TimeOfDayEditor(node)));
}

TimeOfDayEditorPlugin::TimeOfDayEditorPlugin() {
	Ref<EditorInspectorPluginTimeOfDay> plugin;
	plugin.instantiate();
	add_inspector_plugin(plugin);
}
