/**************************************************************************/
/*  time_of_day_editor_plugin.h                                           */
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

#include "editor/inspector/editor_inspector.h"
#include "editor/plugins/editor_plugin.h"
#include "scene/gui/box_container.h"

class Button;
class TimeOfDay;

// The whole day at a glance, at the top of TimeOfDay's Inspector: the sky's
// colors hour by hour, the paths of the sun and the moon across it, sunrise
// and sunset, and the current time, which can be dragged along the day.
class TimeOfDayTimeline : public Control {
	GDCLASS(TimeOfDayTimeline, Control);

	ObjectID time_of_day;
	bool dragging = false;
	double drag_start_time = 0.0;

	// Everything the strip is drawn from, so that it only redraws when some of
	// it changes rather than on every frame.
	uint32_t drawn_state = 0;

	TimeOfDay *_get_time_of_day() const;
	uint32_t _get_state_hash() const;
	Rect2 _get_band_rect() const;
	double _get_time_at(float p_x, bool p_snap) const;
	void _draw_timeline();

protected:
	void _notification(int p_what);

public:
	virtual void gui_input(const Ref<InputEvent> &p_event) override;
	virtual String get_tooltip(const Point2 &p_pos) const override;

	TimeOfDayTimeline(TimeOfDay *p_time_of_day);
};

class TimeOfDayEditor : public VBoxContainer {
	GDCLASS(TimeOfDayEditor, VBoxContainer);

	ObjectID time_of_day;
	Button *create_profile_button = nullptr;
	Button *key_button = nullptr;

	TimeOfDay *_get_time_of_day() const;
	void _update_buttons();
	void _create_default_profile();
	void _key_changed_values();

protected:
	void _notification(int p_what);

public:
	TimeOfDayEditor(TimeOfDay *p_time_of_day);
};

class EditorInspectorPluginTimeOfDay : public EditorInspectorPlugin {
	GDCLASS(EditorInspectorPluginTimeOfDay, EditorInspectorPlugin);

public:
	virtual bool can_handle(Object *p_object) override;
	virtual void parse_begin(Object *p_object) override;
};

class TimeOfDayEditorPlugin : public EditorPlugin {
	GDCLASS(TimeOfDayEditorPlugin, EditorPlugin);

public:
	virtual String get_plugin_name() const override { return "TimeOfDay"; }

	TimeOfDayEditorPlugin();
};
