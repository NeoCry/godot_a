/**************************************************************************/
/*  test_scene_tree_timers.cpp                                            */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_scene_tree_timers)

#include "scene/animation/tween.h"
#include "scene/main/scene_tree.h"
#include "tests/test_tools.h"

namespace TestSceneTreeTimers {

// How many times the handlers below were called.
static int calls = 0;

// The first call runs a frame of the main loop before returning, as the
// editor's progress dialog does (through Main::iteration()) when a scene is
// saved from a timer's timeout.
static void run_nested_frame() {
	calls++;
	if (calls == 1) {
		SceneTree::get_singleton()->process(0.1);
	}
}

TEST_CASE("[SceneTree] A timer's timeout can run a frame of the main loop itself") {
	SceneTree *tree = SceneTree::get_singleton();
	calls = 0;

	Ref<SceneTreeTimer> timer = tree->create_timer(0.05);
	timer->connect(SNAME("timeout"), callable_mp_static(&run_nested_frame));
	Ref<SceneTreeTimer> later = tree->create_timer(10.0);

	ErrorDetector ed;
	tree->process(0.1);

	// The nested frame neither fires the timer again nor takes it out of the
	// list from under the frame that fired it.
	CHECK_FALSE(ed.has_error);
	CHECK(calls == 1);
	// The other timers tick once for the frame, not once more for the nested one.
	CHECK(later->get_time_left() == doctest::Approx(9.9));

	tree->process(0.1);
	CHECK(calls == 1);
	CHECK(later->get_time_left() == doctest::Approx(9.8));
}

TEST_CASE("[SceneTree] A tween's callback can run a frame of the main loop itself") {
	SceneTree *tree = SceneTree::get_singleton();
	calls = 0;

	Ref<Tween> tween = tree->create_tween();
	tween->tween_callback(callable_mp_static(&run_nested_frame));
	tween->tween_interval(1.0);

	ErrorDetector ed;
	tree->process(0.1);

	// The nested frame does not step the tween a second time while it is
	// still in the middle of its step.
	CHECK_FALSE(ed.has_error);
	CHECK(calls == 1);
	CHECK(tween->get_total_time() == doctest::Approx(0.1));
	CHECK(tween->is_running());

	tree->process(1.0);
	CHECK(calls == 1);
	CHECK_FALSE(tween->is_running());
}

} // namespace TestSceneTreeTimers
