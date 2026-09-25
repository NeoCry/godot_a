/**************************************************************************/
/*  landscape_spline_3d_editor_plugin.h                                   */
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

#include "editor/plugins/editor_plugin.h"
#include "scene/3d/landscape_spline_3d.h"

class Button;
class HBoxContainer;

// The landscape half of editing a LandscapeSpline3D. Its curve is edited by
// Path3DEditorPlugin, which handles every Path3D and so stays active next to
// this one; this adds what only a road or river needs: dropping the points onto
// the terrain, shaping the terrain to the spline (undoably), and a starting
// point for a custom material. It also makes changing spline_type in the
// Inspector bring the rest of that type's preset along, in the same undo step.
class LandscapeSpline3DEditorPlugin : public EditorPlugin {
	GDCLASS(LandscapeSpline3DEditorPlugin, EditorPlugin);

	LandscapeSpline3D *spline = nullptr;

	HBoxContainer *toolbar = nullptr;
	Button *snap_points_button = nullptr;
	Button *apply_button = nullptr;
	Button *copy_material_button = nullptr;

	void _snap_points_pressed();
	void _apply_pressed();
	void _copy_material_pressed();

	void _undo_redo_inspector_callback(Object *p_undo_redo, Object *p_edited, const String &p_property, const Variant &p_new_value);

public:
	virtual String get_plugin_name() const override { return "LandscapeSpline3D"; }
	virtual bool handles(Object *p_object) const override;
	virtual void edit(Object *p_object) override;
	virtual void make_visible(bool p_visible) override;

	LandscapeSpline3DEditorPlugin();
};
