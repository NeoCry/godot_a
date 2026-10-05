/**************************************************************************/
/*  test_foliage_painter_3d.cpp                                           */
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

TEST_FORCE_LINK(test_foliage_painter_3d)

#include "core/config/engine.h"
#include "scene/3d/foliage_layer.h"
#include "scene/3d/foliage_painter_3d.h"

namespace TestFoliagePainter3D {

static Ref<FoliageLayer> make_layer(const String &p_name) {
	Ref<FoliageLayer> layer;
	layer.instantiate();
	layer->set_layer_name(p_name);
	return layer;
}

static TypedArray<FoliageLayer> make_list(const Vector<Ref<FoliageLayer>> &p_layers) {
	TypedArray<FoliageLayer> list;
	for (const Ref<FoliageLayer> &layer : p_layers) {
		list.push_back(layer);
	}
	return list;
}

// Paints p_count instances of layer p_layer into the cell at p_cell. Only how
// many there are is checked: the headless renderer keeps no MultiMesh
// transforms to read back.
static void paint(FoliagePainter3D *p_painter, int p_layer, const Vector2i &p_cell, int p_count) {
	for (int i = 0; i < p_count; i++) {
		p_painter->add_instance(p_layer, p_cell, Transform3D(Basis(), Vector3(p_cell.x, 0, p_cell.y) * p_painter->get_cell_size()));
	}
}

// Painted with two layers: A has 2 instances in cell (0, 0), B has 3 in (2, 0).
struct PaintedScene {
	FoliagePainter3D *painter = nullptr;
	Ref<FoliageLayer> a = make_layer("A");
	Ref<FoliageLayer> b = make_layer("B");

	PaintedScene() {
		painter = memnew(FoliagePainter3D);
		painter->set_foliage_layers(make_list({ a, b }));
		paint(painter, 0, Vector2i(0, 0), 2);
		paint(painter, 1, Vector2i(2, 0), 3);
	}

	~PaintedScene() {
		memdelete(painter);
	}
};

TEST_CASE("[SceneTree][FoliagePainter3D] Reordering the foliage layers keeps each one's instances") {
	PaintedScene scene;
	scene.painter->set_foliage_layers(make_list({ scene.b, scene.a }));

	CHECK(scene.painter->get_cell_instance_count(0, Vector2i(2, 0)) == 3);
	CHECK(scene.painter->get_cell_instance_count(0, Vector2i(0, 0)) == 0);
	CHECK(scene.painter->get_cell_instance_count(1, Vector2i(0, 0)) == 2);
	CHECK(scene.painter->get_cell_instance_count(1, Vector2i(2, 0)) == 0);
	CHECK(scene.b->get_instance_count() == 3);
	CHECK(scene.a->get_instance_count() == 2);
}

TEST_CASE("[SceneTree][FoliagePainter3D] Removing a foliage layer takes its instances with it, and undoing it brings them back") {
	PaintedScene scene;

	// Removed instances are only held on to for undo in the editor.
	const bool was_editor_hint = Engine::get_singleton()->is_editor_hint();
	Engine::get_singleton()->set_editor_hint(true);

	scene.painter->set_foliage_layers(make_list({ scene.b }));
	CHECK(scene.painter->get_layer_count() == 1);
	CHECK(scene.painter->get_cell_instance_count(0, Vector2i(2, 0)) == 3);
	CHECK(scene.painter->get_layer_cell_coords(0).size() == 1);

	// What undo does: put the old list back.
	scene.painter->set_foliage_layers(make_list({ scene.a, scene.b }));
	CHECK(scene.painter->get_cell_instance_count(0, Vector2i(0, 0)) == 2);
	CHECK(scene.painter->get_cell_instance_count(1, Vector2i(2, 0)) == 3);
	CHECK(scene.a->get_instance_count() == 2);

	Engine::get_singleton()->set_editor_hint(was_editor_hint);
}

TEST_CASE("[SceneTree][FoliagePainter3D] A new foliage layer starts out empty") {
	PaintedScene scene;
	const Ref<FoliageLayer> c = make_layer("C");
	scene.painter->set_foliage_layers(make_list({ scene.a, scene.b, c }));

	CHECK(scene.painter->get_layer_cell_coords(2).is_empty());
	CHECK(scene.painter->get_cell_instance_count(0, Vector2i(0, 0)) == 2);
	CHECK(scene.painter->get_cell_instance_count(1, Vector2i(2, 0)) == 3);
}

TEST_CASE("[SceneTree][FoliagePainter3D] Replacing the foliage layer in a slot keeps the instances painted in it") {
	PaintedScene scene;
	// As making the layer unique, or picking another resource for it, does.
	const Ref<FoliageLayer> a_copy = scene.a->duplicate(true);
	scene.painter->set_foliage_layers(make_list({ a_copy, scene.b }));

	CHECK(scene.painter->get_cell_instance_count(0, Vector2i(0, 0)) == 2);
	CHECK(scene.painter->get_cell_instance_count(1, Vector2i(2, 0)) == 3);
	CHECK(a_copy->get_instance_count() == 2);
}

#ifndef DISABLE_DEPRECATED
TEST_CASE("[SceneTree][FoliagePainter3D] Scenes saved with the old layers property still load") {
	FoliagePainter3D *painter = memnew(FoliagePainter3D);
	painter->set("layers", make_list({ make_layer("A"), make_layer("B") }));
	CHECK(painter->get_layer_count() == 2);
	CHECK(painter->get_foliage_layers().size() == 2);
	memdelete(painter);
}
#endif // DISABLE_DEPRECATED

} // namespace TestFoliagePainter3D
