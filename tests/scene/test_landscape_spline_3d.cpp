/**************************************************************************/
/*  test_landscape_spline_3d.cpp                                          */
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

TEST_FORCE_LINK(test_landscape_spline_3d)

#include "core/object/undo_redo.h"
#include "scene/3d/landscape_3d.h"
#include "scene/3d/landscape_spline_3d.h"
#include "scene/3d/landscape_spline_flow.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/3d/physics/static_body_3d.h"
#include "scene/3d/terrain_data.h"
#include "scene/3d/terrain_layer.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/box_shape_3d.h"
#include "scene/resources/curve.h"
#include "scene/resources/material.h"
#include "scene/resources/shader.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/shader_language.h"
#include "servers/rendering/shader_preprocessor.h"
#include "servers/rendering/shader_types.h"

namespace TestLandscapeSpline3D {

// 512 x 512 m at 2 m spacing, with ground that is either flat or, when
// p_wave_height is set, rolls along X (the same at every Z, so the terrain's
// triangles and a bilinear read of it agree exactly).
static Ref<TerrainData> make_terrain(float p_wave_height = 0.0f, int p_resolution = 257, float p_spacing = 2.0f) {
	Ref<TerrainData> data;
	data.instantiate();
	data->set_resolution(p_resolution);
	data->set_vertex_spacing(p_spacing);
	for (int z = 0; z < p_resolution; z++) {
		for (int x = 0; x < p_resolution; x++) {
			data->set_height(x, z, p_wave_height * Math::sin(x * p_spacing / 20.0f));
		}
	}
	return data;
}

static Ref<Curve3D> make_line(const Vector3 &p_from, const Vector3 &p_to, int p_points = 2) {
	Ref<Curve3D> curve;
	curve.instantiate();
	for (int i = 0; i < p_points; i++) {
		curve->add_point(p_from.lerp(p_to, (float)i / (p_points - 1)));
	}
	return curve;
}

// A landscape at the origin with the spline under it, so the spline finds it
// on its own and both share one local space.
struct SplineScene {
	Landscape3D *landscape = nullptr;
	LandscapeSpline3D *spline = nullptr;

	explicit SplineScene(const Ref<TerrainData> &p_data) {
		landscape = memnew(Landscape3D);
		landscape->set_terrain_data(p_data);
		SceneTree::get_singleton()->get_root()->add_child(landscape);
		spline = memnew(LandscapeSpline3D);
		landscape->add_child(spline);
	}

	~SplineScene() {
		memdelete(landscape);
	}
};

static RenderingServerTypes::SurfaceData chunk_surface(LandscapeSpline3D *p_spline, int p_chunk) {
	const RID mesh = p_spline->get_chunk_mesh(p_chunk);
	if (!mesh.is_valid() || RS::get_singleton()->mesh_get_surface_count(mesh) == 0) {
		return RenderingServerTypes::SurfaceData();
	}
	return RS::get_singleton()->mesh_get_surface(mesh, 0);
}

static PackedVector3Array chunk_vertices(LandscapeSpline3D *p_spline, int p_chunk) {
	const RID mesh = p_spline->get_chunk_mesh(p_chunk);
	if (!mesh.is_valid() || RS::get_singleton()->mesh_get_surface_count(mesh) == 0) {
		return PackedVector3Array();
	}
	const Array arrays = RS::get_singleton()->mesh_surface_get_arrays(mesh, 0);
	return arrays[RSE::ARRAY_VERTEX];
}

static int total_vertex_count(LandscapeSpline3D *p_spline) {
	int count = 0;
	for (int i = 0; i < p_spline->get_chunk_count(); i++) {
		count += chunk_surface(p_spline, i).vertex_count;
	}
	return count;
}

TEST_CASE("[SceneTree][LandscapeSpline3D] A long spline is split into chunks with bounds of their own") {
	SplineScene scene(make_terrain());
	scene.spline->set_chunk_length(64.0f);
	scene.spline->set_curve(make_line(Vector3(8, 0, 256), Vector3(504, 0, 256)));
	scene.spline->update_mesh();

	const int chunk_count = scene.spline->get_chunk_count();
	CHECK(chunk_count == 8);

	float lowest_x = Math::INF;
	float highest_x = -Math::INF;
	for (int i = 0; i < chunk_count; i++) {
		CHECK(scene.spline->get_chunk_mesh(i).is_valid());
		CHECK(scene.spline->get_chunk_instance(i).is_valid());
		// Culling and LOD work on each chunk's own bounds, which is the whole
		// point of chunking: none of them may span the whole spline.
		const AABB aabb = chunk_surface(scene.spline, i).aabb;
		CHECK(aabb.size.x > 0.0f);
		CHECK(aabb.size.x <= 64.0f + 0.01f);
		lowest_x = MIN(lowest_x, aabb.position.x);
		highest_x = MAX(highest_x, aabb.position.x + aabb.size.x);
	}
	// Together they still cover all of it.
	CHECK(lowest_x == doctest::Approx(8.0f).epsilon(0.001));
	CHECK(highest_x == doctest::Approx(504.0f).epsilon(0.001));
}

TEST_CASE("[SceneTree][LandscapeSpline3D] Straight, level stretches are simplified away") {
	SplineScene scene(make_terrain());
	scene.spline->set_curve(make_line(Vector3(8, 0, 256), Vector3(504, 0, 256)));
	scene.spline->update_mesh();
	const int simplified = total_vertex_count(scene.spline);

	scene.spline->set_simplify_tolerance(0.0f);
	scene.spline->update_mesh();
	const int full = total_vertex_count(scene.spline);

	// Nearly 500 cross-sections a meter apart, nine columns each, when only
	// what lines up exactly may be dropped; on flat ground along a straight
	// line nothing needs to stay but the chunk ends, a ring every
	// MAX_RING_SPAN, and the edge fade.
	CHECK(simplified > 0);
	CHECK(simplified < 200);
	CHECK(full > simplified * 10);
}

TEST_CASE("[SceneTree][LandscapeSpline3D] Height modes") {
	const float wave = 6.0f;
	Ref<TerrainData> data = make_terrain(wave);
	SplineScene scene(data);
	scene.spline->set_curve(make_line(Vector3(40, 10, 256), Vector3(200, 10, 256)));
	scene.spline->set_height_offset(0.05f);

	SUBCASE("Conform drapes every vertex over the ground") {
		scene.spline->set_height_mode(LandscapeSpline3D::HEIGHT_MODE_CONFORM);
		scene.spline->update_mesh();
		REQUIRE(scene.spline->get_chunk_count() > 0);
		int checked = 0;
		for (int i = 0; i < scene.spline->get_chunk_count(); i++) {
			for (const Vector3 &v : chunk_vertices(scene.spline, i)) {
				// Vertices are stored compressed, to well under a millimeter here.
				CHECK(Math::abs(v.y - (data->get_height_at_position(Vector2(v.x, v.z)) + 0.05f)) < 0.002f);
				checked++;
			}
		}
		// Rolling ground keeps plenty of the cross-sections.
		CHECK(checked > 100);
	}

	SUBCASE("Spline follows the curve's own height, whatever the ground does") {
		scene.spline->set_height_mode(LandscapeSpline3D::HEIGHT_MODE_SPLINE);
		scene.spline->update_mesh();
		REQUIRE(scene.spline->get_chunk_count() > 0);
		for (int i = 0; i < scene.spline->get_chunk_count(); i++) {
			for (const Vector3 &v : chunk_vertices(scene.spline, i)) {
				CHECK(Math::abs(v.y - 10.05f) < 0.002f);
			}
		}
	}

	SUBCASE("Conform Level follows the ground along its length, level across it") {
		// Along Z this time, so the ground rolls across the strip.
		scene.spline->set_curve(make_line(Vector3(123, 10, 40), Vector3(123, 10, 200)));
		scene.spline->set_width(20.0f);
		scene.spline->set_height_mode(LandscapeSpline3D::HEIGHT_MODE_CONFORM_LEVEL);
		scene.spline->update_mesh();
		const float centerline_height = data->get_height_at_position(Vector2(123, 100)) + 0.05f;
		for (int i = 0; i < scene.spline->get_chunk_count(); i++) {
			for (const Vector3 &v : chunk_vertices(scene.spline, i)) {
				CHECK(Math::abs(v.y - centerline_height) < 0.002f);
			}
		}
	}
}

TEST_CASE("[SceneTree][LandscapeSpline3D] Every chunk carries measured LOD levels") {
	SplineScene scene(make_terrain(6.0f));
	scene.spline->set_curve(make_line(Vector3(8, 0, 256), Vector3(504, 0, 256)));
	scene.spline->update_mesh();

	for (int i = 0; i < scene.spline->get_chunk_count(); i++) {
		const RenderingServerTypes::SurfaceData surface = chunk_surface(scene.spline, i);
		REQUIRE(surface.lods.size() > 0);
		float previous = 0.0f;
		for (const RenderingServerTypes::SurfaceData::LOD &lod : surface.lods) {
			// Increasing, and on rolling ground no level is free.
			CHECK(lod.edge_length > previous);
			previous = lod.edge_length;
			CHECK(lod.index_data.size() < surface.index_data.size());
		}
	}
}

TEST_CASE("[SceneTree][LandscapeSpline3D] Editing the far end of a spline leaves the rest of it alone") {
	SplineScene scene(make_terrain(6.0f));
	Ref<Curve3D> curve = make_line(Vector3(8, 0, 256), Vector3(504, 0, 256), 5);
	scene.spline->set_curve(curve);
	scene.spline->update_mesh();

	const int chunk_count = scene.spline->get_chunk_count();
	REQUIRE(chunk_count == 8);
	Vector<uint64_t> before;
	for (int i = 0; i < chunk_count; i++) {
		before.push_back(scene.spline->get_chunk_hash(i));
	}

	// The last point sits at x = 504; with smoothing, moving it also bends the
	// segment before it, which starts at x = 256.
	curve->set_point_position(4, Vector3(504, 0, 270));
	scene.spline->update_mesh();

	REQUIRE(scene.spline->get_chunk_count() == chunk_count);
	for (int i = 0; i < 3; i++) {
		CHECK(scene.spline->get_chunk_hash(i) == before[i]);
	}
	CHECK(scene.spline->get_chunk_hash(chunk_count - 1) != before[chunk_count - 1]);
}

TEST_CASE("[SceneTree][LandscapeSpline3D] Sculpting under part of a spline only rebuilds that part") {
	SplineScene scene(make_terrain());
	scene.spline->set_curve(make_line(Vector3(8, 0, 256), Vector3(504, 0, 256)));
	scene.spline->update_mesh();

	const int chunk_count = scene.spline->get_chunk_count();
	REQUIRE(chunk_count == 8);
	Vector<uint64_t> before;
	for (int i = 0; i < chunk_count; i++) {
		before.push_back(scene.spline->get_chunk_hash(i));
	}

	// Chunk 3 runs from x = 200 to x = 264.
	scene.landscape->sculpt(Vector3(232, 0, 256), 6.0f, 2.0f, Landscape3D::SCULPT_RAISE);
	scene.spline->update_mesh();

	CHECK(scene.spline->get_chunk_hash(3) != before[3]);
	for (int i = 0; i < chunk_count; i++) {
		if (i != 3) {
			CHECK(scene.spline->get_chunk_hash(i) == before[i]);
		}
	}

	// And the rebuilt chunk does follow the new ground.
	bool found_raised = false;
	for (const Vector3 &v : chunk_vertices(scene.spline, 3)) {
		if (v.y > 1.0f) {
			found_raised = true;
		}
	}
	CHECK(found_raised);
}

TEST_CASE("[SceneTree][LandscapeSpline3D] Applying to the landscape shapes only the ground along the spline") {
	Ref<TerrainData> data = make_terrain();
	SplineScene scene(data);
	scene.spline->set_curve(make_line(Vector3(8, 3, 256), Vector3(504, 3, 256)));
	scene.spline->set_width(6.0f);
	scene.spline->set_carve_falloff(4.0f);
	scene.spline->update_mesh();

	const TypedArray<Rect2i> footprint = scene.spline->get_landscape_footprint();
	// 9 x 9 blocks of Landscape3D::CHUNK_QUADS samples; the road crosses one
	// or two rows of them.
	CHECK(footprint.size() > 0);
	CHECK(footprint.size() <= 18);

	SUBCASE("A road flattens the ground to its own height") {
		scene.spline->set_carve_depth(0.0f);
		scene.spline->apply_to_landscape();

		CHECK(data->get_height_at_position(Vector2(250, 256)) == doctest::Approx(3.0f).epsilon(0.001));
		CHECK(data->get_height_at_position(Vector2(250, 258)) == doctest::Approx(3.0f).epsilon(0.001));
		// Kept level for a sample and a half past the edge, so the ground under
		// the road's edge is not already sloping away...
		CHECK(data->get_height_at_position(Vector2(250, 262)) == doctest::Approx(3.0f).epsilon(0.001));
		// ...then blending back to the untouched ground over the falloff.
		const float in_falloff = data->get_height_at_position(Vector2(250, 264));
		CHECK(in_falloff > 0.05f);
		CHECK(in_falloff < 2.95f);
		CHECK(data->get_height_at_position(Vector2(250, 280)) == doctest::Approx(0.0f));

		// Applying again leaves the ground under the road as it is.
		scene.spline->apply_to_landscape();
		CHECK(data->get_height_at_position(Vector2(250, 256)) == doctest::Approx(3.0f).epsilon(0.001));
		CHECK(data->get_height_at_position(Vector2(250, 280)) == doctest::Approx(0.0f));
	}

	SUBCASE("A river digs its bed below the curve") {
		// 8 wide, so its edges land on terrain samples.
		scene.spline->set_width(8.0f);
		scene.spline->set_carve_depth(2.0f);
		scene.spline->apply_to_landscape();

		// Flat across the middle half...
		CHECK(data->get_height_at_position(Vector2(250, 256)) == doctest::Approx(1.0f).epsilon(0.001));
		CHECK(data->get_height_at_position(Vector2(250, 258)) == doctest::Approx(1.0f).epsilon(0.001));
		// ...and its banks rise back to the water's surface at the edges.
		CHECK(data->get_height_at_position(Vector2(250, 252)) == doctest::Approx(3.0f).epsilon(0.001));
		CHECK(data->get_height_at_position(Vector2(250, 260)) == doctest::Approx(3.0f).epsilon(0.001));
	}

	SUBCASE("Painting a layer under it") {
		TypedArray<TerrainLayer> layers;
		for (int i = 0; i < 2; i++) {
			Ref<TerrainLayer> layer;
			layer.instantiate();
			layers.push_back(layer);
		}
		scene.landscape->set_layers(layers);
		scene.spline->set_carve_enabled(false);
		scene.spline->set_paint_layer(1);
		scene.spline->apply_to_landscape();

		// The ground itself is untouched...
		CHECK(data->get_height_at_position(Vector2(250, 256)) == doctest::Approx(0.0f));
		// ...but the road's layer took over under it, and only there.
		CHECK(data->get_layer_weight(125, 128, 1) == doctest::Approx(1.0f));
		CHECK(data->get_layer_weight(125, 128, 0) == doctest::Approx(0.0f));
		CHECK(data->get_layer_weight(125, 150, 1) == doctest::Approx(0.0f));
	}
}

// A closed, square shoreline with corners at p_from and p_to, at the given
// height at each corner.
static Ref<Curve3D> make_square(const Vector2 &p_from, const Vector2 &p_to, const Vector4 &p_heights = Vector4()) {
	Ref<Curve3D> curve;
	curve.instantiate();
	curve->add_point(Vector3(p_from.x, p_heights.x, p_from.y));
	curve->add_point(Vector3(p_to.x, p_heights.y, p_from.y));
	curve->add_point(Vector3(p_to.x, p_heights.z, p_to.y));
	curve->add_point(Vector3(p_from.x, p_heights.w, p_to.y));
	curve->set_closed(true);
	return curve;
}

static float filled_area(LandscapeSpline3D *p_spline) {
	float area = 0.0f;
	for (int i = 0; i < p_spline->get_chunk_count(); i++) {
		const RID mesh = p_spline->get_chunk_mesh(i);
		if (!mesh.is_valid() || RS::get_singleton()->mesh_get_surface_count(mesh) == 0) {
			continue;
		}
		const Array arrays = RS::get_singleton()->mesh_surface_get_arrays(mesh, 0);
		const PackedVector3Array vertices = arrays[RSE::ARRAY_VERTEX];
		const PackedInt32Array indices = arrays[RSE::ARRAY_INDEX];
		for (int t = 0; t + 2 < indices.size(); t += 3) {
			const Vector3 a = vertices[indices[t]];
			const Vector3 b = vertices[indices[t + 1]];
			const Vector3 c = vertices[indices[t + 2]];
			// Positive for a triangle facing up, as all of them must.
			area += (c - a).cross(b - a).y * 0.5f;
		}
	}
	return area;
}

TEST_CASE("[SceneTree][LandscapeSpline3D] A lake fills its shoreline with a level surface") {
	Ref<TerrainData> data = make_terrain(6.0f);
	SplineScene scene(data);
	scene.spline->apply_preset(LandscapeSpline3D::TYPE_LAKE);
	// Straight sides, so the area it has to fill is known exactly.
	scene.spline->set_smooth(false);

	SUBCASE("It covers exactly the area inside the shoreline, in tiles") {
		scene.spline->set_curve(make_square(Vector2(200, 200), Vector2(300, 300)));
		scene.spline->update_mesh();
		// 256 m tiles; the square straddles the corner of four of them.
		CHECK(scene.spline->get_chunk_count() == 4);
		CHECK(filled_area(scene.spline) == doctest::Approx(100.0f * 100.0f).epsilon(0.001));
	}

	SUBCASE("Level with the lowest ground around its shore") {
		scene.spline->set_curve(make_square(Vector2(200, 200), Vector2(300, 300)));
		scene.spline->update_mesh();
		// The ground only rolls along X, so the shore's lowest point is on the
		// sides running along X, a meter apart, like the curve's samples.
		float lowest = Math::INF;
		for (int x = 200; x <= 300; x++) {
			lowest = MIN(lowest, data->get_height_at_position(Vector2(x, 200)));
		}
		REQUIRE(scene.spline->get_chunk_count() > 0);
		for (int i = 0; i < scene.spline->get_chunk_count(); i++) {
			for (const Vector3 &v : chunk_vertices(scene.spline, i)) {
				CHECK(Math::abs(v.y - lowest) < 0.002f);
			}
		}
	}

	SUBCASE("Level with the lowest point of the curve, in Spline mode") {
		scene.spline->set_height_mode(LandscapeSpline3D::HEIGHT_MODE_SPLINE);
		scene.spline->set_height_offset(0.5f);
		scene.spline->set_curve(make_square(Vector2(200, 200), Vector2(300, 300), Vector4(9, 7, 11, 13)));
		scene.spline->update_mesh();
		REQUIRE(scene.spline->get_chunk_count() > 0);
		for (int i = 0; i < scene.spline->get_chunk_count(); i++) {
			for (const Vector3 &v : chunk_vertices(scene.spline, i)) {
				// Within the curve's own interpolation around its corners.
				CHECK(Math::abs(v.y - 7.5f) < 0.01f);
			}
		}
	}

	SUBCASE("The tiles wholly inside a big lake are two triangles each") {
		// A diamond 1800 m across, so the tiles along its shore are cut
		// diagonally, unlike those in its middle.
		Ref<Curve3D> diamond;
		diamond.instantiate();
		diamond->add_point(Vector3(512, 0, -388));
		diamond->add_point(Vector3(1412, 0, 512));
		diamond->add_point(Vector3(512, 0, 1412));
		diamond->add_point(Vector3(-388, 0, 512));
		diamond->set_closed(true);
		scene.spline->set_curve(diamond);
		scene.spline->update_mesh();
		int whole_tiles = 0;
		int cut_tiles = 0;
		for (int i = 0; i < scene.spline->get_chunk_count(); i++) {
			const RenderingServerTypes::SurfaceData surface = chunk_surface(scene.spline, i);
			if (surface.index_count == 6 && surface.vertex_count == 4) {
				whole_tiles++;
			} else if (surface.index_count > 0) {
				cut_tiles++;
			}
		}
		CHECK(whole_tiles >= 4);
		CHECK(cut_tiles > 0);
		CHECK(filled_area(scene.spline) == doctest::Approx(2.0f * 900.0f * 900.0f).epsilon(0.001));
	}

	SUBCASE("Turning fill off makes it a strip along the shoreline again") {
		scene.spline->set_curve(make_square(Vector2(200, 200), Vector2(300, 300)));
		scene.spline->update_mesh();
		scene.spline->set_fill(false);
		scene.spline->update_mesh();
		// 400 m of shoreline in 64 m chunks, 16 m wide.
		CHECK(scene.spline->get_chunk_count() == 7);
		CHECK(filled_area(scene.spline) < 100.0f * 100.0f * 0.8f);
	}
}

TEST_CASE("[SceneTree][LandscapeSpline3D] Applying a lake digs its bed and paints it") {
	Ref<TerrainData> data = make_terrain();
	SplineScene scene(data);
	TypedArray<TerrainLayer> layers;
	for (int i = 0; i < 2; i++) {
		Ref<TerrainLayer> layer;
		layer.instantiate();
		layers.push_back(layer);
	}
	scene.landscape->set_layers(layers);

	scene.spline->apply_preset(LandscapeSpline3D::TYPE_LAKE);
	scene.spline->set_smooth(false);
	scene.spline->set_height_mode(LandscapeSpline3D::HEIGHT_MODE_SPLINE);
	scene.spline->set_carve_depth(3.0f);
	scene.spline->set_carve_falloff(8.0f);
	scene.spline->set_paint_layer(1);
	scene.spline->set_curve(make_square(Vector2(200, 200), Vector2(300, 300), Vector4(5, 5, 5, 5)));
	scene.spline->update_mesh();

	// A 100 m lake on a 512 m terrain: a handful of the 81 blocks.
	const TypedArray<Rect2i> footprint = scene.spline->get_landscape_footprint();
	CHECK(footprint.size() > 0);
	CHECK(footprint.size() <= 16);

	scene.spline->apply_to_landscape();

	// The full depth below the water's level away from the shore...
	CHECK(data->get_height_at_position(Vector2(250, 250)) == doctest::Approx(2.0f).epsilon(0.001));
	// ...shelving up towards it...
	const float near_shore = data->get_height_at_position(Vector2(202, 250));
	CHECK(near_shore > 2.1f);
	CHECK(near_shore < 5.0f);
	// ...a level shore at the water's level just outside it...
	CHECK(data->get_height_at_position(Vector2(198, 250)) == doctest::Approx(5.0f).epsilon(0.001));
	// ...and the untouched ground further out.
	CHECK(data->get_height_at_position(Vector2(150, 250)) == doctest::Approx(0.0f));

	// Its layer is painted over the whole bed, and not far beyond it.
	CHECK(data->get_layer_weight(125, 125, 1) == doctest::Approx(1.0f));
	CHECK(data->get_layer_weight(101, 125, 1) == doctest::Approx(1.0f));
	CHECK(data->get_layer_weight(75, 125, 1) == doctest::Approx(0.0f));
}

TEST_CASE("[SceneTree][LandscapeSpline3D] What a spline covers on the ground") {
	SplineScene scene(make_terrain());
	LandscapeSpline3D::GroundCoverage coverage;

	SUBCASE("A strip: its centerline, and how wide it is along it, in global space") {
		scene.spline->set_curve(make_line(Vector3(8, 3, 256), Vector3(504, 3, 256)));
		scene.spline->set_width(6.0f);
		// Before its first update: the coverage has to be up to date anyway.
		REQUIRE(scene.spline->get_ground_coverage(coverage));
		CHECK_FALSE(coverage.filled);
		CHECK(coverage.up.is_equal_approx(Vector3(0, 1, 0)));
		REQUIRE(coverage.points.size() > 2);
		CHECK(coverage.half_widths.size() == coverage.points.size());
		CHECK(coverage.points[0].is_equal_approx(Vector3(8, 3, 256)));
		CHECK(coverage.points[coverage.points.size() - 1].is_equal_approx(Vector3(504, 3, 256)));
		for (const float half_width : coverage.half_widths) {
			CHECK(half_width == doctest::Approx(3.0f));
		}

		// Moved and scaled up: all of it, the width included.
		scene.spline->set_position(Vector3(0, 0, 10));
		scene.spline->set_scale(Vector3(2, 2, 2));
		REQUIRE(scene.spline->get_ground_coverage(coverage));
		CHECK(coverage.points[0].is_equal_approx(Vector3(16, 6, 522)));
		for (const float half_width : coverage.half_widths) {
			CHECK(half_width == doctest::Approx(6.0f));
		}
	}

	SUBCASE("A filled area: its shoreline and the level of its surface") {
		scene.spline->apply_preset(LandscapeSpline3D::TYPE_LAKE);
		scene.spline->set_smooth(false);
		scene.spline->set_height_mode(LandscapeSpline3D::HEIGHT_MODE_SPLINE);
		scene.spline->set_height_offset(0.5f);
		scene.spline->set_curve(make_square(Vector2(200, 200), Vector2(300, 300), Vector4(9, 7, 11, 13)));
		REQUIRE(scene.spline->get_ground_coverage(coverage));
		CHECK(coverage.filled);
		CHECK(coverage.half_widths.is_empty());
		// About 400 m of shoreline, a point a meter, without the closed curve's
		// repeat of its first point.
		CHECK(coverage.points.size() >= 399);
		CHECK(coverage.points.size() <= 401);
		CHECK_FALSE(coverage.points[0].is_equal_approx(coverage.points[coverage.points.size() - 1]));
		CHECK(coverage.level == doctest::Approx(7.5f).epsilon(0.001));
	}

	SUBCASE("Nothing to cover yet") {
		CHECK_FALSE(scene.spline->get_ground_coverage(coverage));
		CHECK(coverage.points.is_empty());
	}
}

TEST_CASE("[LandscapeSpline3D] The built-in materials' shaders compile") {
	// No GPU here to build them for, but the shader language front end
	// catches everything short of driver-specific trouble.
	for (int type = 0; type < LandscapeSpline3D::TYPE_MAX; type++) {
		const Ref<ShaderMaterial> material = LandscapeSpline3D::get_default_material((LandscapeSpline3D::SplineType)type);
		REQUIRE(material.is_valid());
		REQUIRE(material->get_shader().is_valid());

		String code;
		ShaderPreprocessor preprocessor;
		REQUIRE(preprocessor.preprocess(material->get_shader()->get_code(), "", code) == OK);

		ShaderLanguage::ShaderCompileInfo info;
		info.functions = ShaderTypes::get_singleton()->get_functions(RSE::SHADER_SPATIAL);
		info.render_modes = ShaderTypes::get_singleton()->get_modes(RSE::SHADER_SPATIAL);
		info.stencil_modes = ShaderTypes::get_singleton()->get_stencil_modes(RSE::SHADER_SPATIAL);
		info.shader_types = ShaderTypes::get_singleton()->get_types();

		ShaderLanguage parser;
		const Error err = parser.compile(code, info);
		CHECK_MESSAGE(err == OK, vformat("Spline type %d: %s (line %d)", type, parser.get_error_text(), parser.get_error_line()));

		// And the editable copy is the same shader, but its own.
		const Ref<ShaderMaterial> copy = LandscapeSpline3D::create_default_material((LandscapeSpline3D::SplineType)type);
		REQUIRE(copy.is_valid());
		CHECK(copy->get_shader() != material->get_shader());
		CHECK(copy->get_shader()->get_code() == material->get_shader()->get_code());
	}
}

TEST_CASE("[SceneTree][LandscapeSpline3D] Presets") {
	LandscapeSpline3D *spline = memnew(LandscapeSpline3D);
	spline->apply_preset(LandscapeSpline3D::TYPE_RIVER);
	CHECK(spline->get_spline_type() == LandscapeSpline3D::TYPE_RIVER);
	CHECK(spline->get_height_mode() == LandscapeSpline3D::HEIGHT_MODE_SPLINE);
	CHECK(spline->get_width() == doctest::Approx(16.0f));
	CHECK(spline->get_carve_depth() > 0.0f);

	spline->apply_preset(LandscapeSpline3D::TYPE_ROAD);
	// The road preset is the class's own defaults.
	LandscapeSpline3D *fresh = memnew(LandscapeSpline3D);
	const Dictionary preset = LandscapeSpline3D::get_preset(LandscapeSpline3D::TYPE_ROAD);
	for (const KeyValue<Variant, Variant> &kv : preset) {
		CHECK_MESSAGE(spline->get(kv.key) == fresh->get(kv.key), kv.key);
	}
	memdelete(fresh);
	memdelete(spline);
}

#ifdef TOOLS_ENABLED
static PackedVector3Array global_points(LandscapeSpline3D *p_spline) {
	PackedVector3Array points;
	const Ref<Curve3D> curve = p_spline->get_curve();
	for (int i = 0; i < curve->get_point_count(); i++) {
		points.push_back(p_spline->get_global_transform().xform(curve->get_point_position(i)));
	}
	return points;
}

static bool points_equal_approx(const PackedVector3Array &p_a, const PackedVector3Array &p_b) {
	if (p_a.size() != p_b.size()) {
		return false;
	}
	for (int i = 0; i < p_a.size(); i++) {
		if (!p_a[i].is_equal_approx(p_b[i])) {
			return false;
		}
	}
	return true;
}

// Where the strip is drawn, in global space.
static AABB global_mesh_bounds(LandscapeSpline3D *p_spline) {
	AABB bounds;
	bool empty = true;
	for (int i = 0; i < p_spline->get_chunk_count(); i++) {
		for (const Vector3 &v : chunk_vertices(p_spline, i)) {
			const Vector3 global = p_spline->get_global_transform().xform(v);
			if (empty) {
				bounds = AABB(global, Vector3());
				empty = false;
			} else {
				bounds.expand_to(global);
			}
		}
	}
	return bounds;
}

TEST_CASE("[SceneTree][LandscapeSpline3D] Centering the origin on the curve moves nothing in the world") {
	SplineScene scene(make_terrain(6.0f));
	// Turned and moved on the landscape, so the center has to be found in the
	// spline's own space, and the node moved in its parent's.
	const Transform3D placed(Basis(Vector3(0, 1, 0), 0.3f), Vector3(20, 0, 30));
	scene.spline->set_transform(placed);
	const Ref<Curve3D> curve = make_line(Vector3(100, 2, 150), Vector3(300, 8, 180), 4);
	scene.spline->set_curve(curve);
	Node3D *child = memnew(Node3D);
	child->set_position(Vector3(150, 1, 160));
	scene.spline->add_child(child);
	PathFollow3D *follower = memnew(PathFollow3D);
	scene.spline->add_child(follower);
	follower->set_progress(50.0f);
	scene.spline->update_mesh();

	const PackedVector3Array points_before = global_points(scene.spline);
	const Transform3D child_before = child->get_global_transform();
	const Transform3D follower_before = follower->get_global_transform();
	const AABB mesh_before = global_mesh_bounds(scene.spline);
	REQUIRE(mesh_before.size.x > 100.0f);

	scene.spline->_edit_center_origin();
	scene.spline->update_mesh();

	// Halfway along the line, which is the middle of its bounds.
	CHECK(scene.spline->get_transform().is_equal_approx(Transform3D(placed.basis, placed.xform(Vector3(200, 5, 165)))));
	CHECK(curve->get_point_position(0).is_equal_approx(Vector3(-100, -3, -15)));
	CHECK(curve->get_point_position(3).is_equal_approx(Vector3(100, 3, 15)));

	CHECK(points_equal_approx(global_points(scene.spline), points_before));
	CHECK(child->get_global_transform().is_equal_approx(child_before));
	// Placed along the moved curve again, which is baked anew.
	CHECK(follower->get_global_position().distance_to(follower_before.origin) < 0.001f);
	// Rebuilt around the new origin, the strip lies where it did (to within
	// its vertex compression and simplification).
	const AABB mesh_after = global_mesh_bounds(scene.spline);
	CHECK(mesh_after.position.distance_to(mesh_before.position) < 0.05f);
	CHECK(mesh_after.get_end().distance_to(mesh_before.get_end()) < 0.05f);

	// Centered already: nothing moves, and nothing is rebuilt.
	const Vector3 centered = scene.spline->get_position();
	Vector<uint64_t> hashes;
	for (int i = 0; i < scene.spline->get_chunk_count(); i++) {
		hashes.push_back(scene.spline->get_chunk_hash(i));
	}
	scene.spline->_edit_center_origin();
	scene.spline->update_mesh();
	CHECK(scene.spline->get_position() == centered);
	REQUIRE(scene.spline->get_chunk_count() == hashes.size());
	for (int i = 0; i < hashes.size(); i++) {
		CHECK(scene.spline->get_chunk_hash(i) == hashes[i]);
	}

	// And back where it was, as undoing it does.
	scene.spline->_edit_move_origin(placed.origin);
	CHECK(scene.spline->get_position() == placed.origin);
	CHECK(curve->get_point_position(0).is_equal_approx(Vector3(100, 2, 150)));
	CHECK(points_equal_approx(global_points(scene.spline), points_before));
	CHECK(child->get_position().is_equal_approx(Vector3(150, 1, 160)));

	// Nothing to center on.
	curve->clear_points();
	scene.spline->_edit_center_origin();
	CHECK(scene.spline->get_position() == placed.origin);
}

TEST_CASE("[SceneTree][LandscapeSpline3D] The origin follows the curve through undo and redo") {
	SplineScene scene(make_terrain());
	const Ref<Curve3D> curve = make_line(Vector3(100, 0, 256), Vector3(200, 0, 256), 3);
	scene.spline->set_curve(curve);
	const PackedVector3Array original = global_points(scene.spline);

	// What the editor wraps every edit of the points in (see
	// LandscapeSpline3DEditorPlugin::begin_point_action()): the origin is put
	// back first when undoing, since the edit's own undo is in the space it
	// was made in, and centered last when doing.
	UndoRedo *undo_redo = memnew(UndoRedo);
	auto edit = [&](const Callable &p_do, const Callable &p_undo) {
		undo_redo->create_action("Edit Points");
		undo_redo->add_undo_method(Callable(scene.spline, "_edit_move_origin").bind(scene.spline->get_position()));
		undo_redo->add_do_method(p_do);
		undo_redo->add_undo_method(p_undo);
		undo_redo->add_do_method(Callable(scene.spline, "_edit_center_origin"));
		undo_redo->commit_action();
	};
	const Callable set_position = Callable(curve.ptr(), "set_point_position");

	// The spline starts out at its landscape's origin, far from its points.
	edit(set_position.bind(2, Vector3(300, 0, 256)), set_position.bind(2, Vector3(200, 0, 256)));
	CHECK(scene.spline->get_position().is_equal_approx(Vector3(200, 0, 256)));
	const PackedVector3Array first = global_points(scene.spline);
	CHECK(first[2].is_equal_approx(Vector3(300, 0, 256)));

	// Made in the centered space, like every edit after the first.
	const Vector3 local = curve->get_point_position(2);
	edit(set_position.bind(2, local + Vector3(100, 0, 0)), set_position.bind(2, local));
	CHECK(scene.spline->get_position().is_equal_approx(Vector3(250, 0, 256)));
	const PackedVector3Array second = global_points(scene.spline);
	CHECK(second[2].is_equal_approx(Vector3(400, 0, 256)));

	const Vector3 added = scene.spline->get_global_transform().affine_inverse().xform(Vector3(100, 0, 356));
	edit(Callable(curve.ptr(), "add_point").bind(added, Vector3(), Vector3(), -1), Callable(curve.ptr(), "remove_point").bind(3));
	CHECK(scene.spline->get_position().is_equal_approx(Vector3(250, 0, 306)));
	const PackedVector3Array third = global_points(scene.spline);
	CHECK(third[3].is_equal_approx(Vector3(100, 0, 356)));

	undo_redo->undo();
	CHECK(scene.spline->get_position() == Vector3(250, 0, 256));
	CHECK(points_equal_approx(global_points(scene.spline), second));
	undo_redo->undo();
	CHECK(points_equal_approx(global_points(scene.spline), first));
	undo_redo->undo();
	CHECK(scene.spline->get_position() == Vector3());
	CHECK(points_equal_approx(global_points(scene.spline), original));

	undo_redo->redo();
	CHECK(points_equal_approx(global_points(scene.spline), first));
	undo_redo->redo();
	CHECK(points_equal_approx(global_points(scene.spline), second));
	undo_redo->redo();
	CHECK(scene.spline->get_position().is_equal_approx(Vector3(250, 0, 306)));
	CHECK(points_equal_approx(global_points(scene.spline), third));

	memdelete(undo_redo);
}
#endif // TOOLS_ENABLED

// A straight channel p_columns cells across and p_rows along, p_cell meters
// apart, as deep as p_depth everywhere: across is +X, downstream +Y.
static LandscapeSplineFlow::Grid make_channel(int p_columns, int p_rows, float p_cell, float p_depth = 1.0f) {
	LandscapeSplineFlow::Grid grid;
	grid.columns = p_columns;
	grid.rows = p_rows;
	grid.positions.resize(p_columns * p_rows);
	grid.depths.resize(p_columns * p_rows);
	for (int j = 0; j < p_rows; j++) {
		for (int i = 0; i < p_columns; i++) {
			grid.positions[j * p_columns + i] = Vector2(i * p_cell, j * p_cell);
			grid.depths[j * p_columns + i] = p_depth;
		}
	}
	return grid;
}

TEST_CASE("[LandscapeSplineFlow] An even channel flows straight down, at the same speed throughout") {
	const LandscapeSplineFlow::Grid grid = make_channel(17, 101, 0.5f);
	LandscapeSplineFlow::Field field;
	LandscapeSplineFlow::solve(grid, field);
	REQUIRE(field.current.size() == grid.positions.size());

	float worst = 0.0f;
	float churned = 0.0f;
	float sheltered = 0.0f;
	for (uint32_t k = 0; k < field.current.size(); k++) {
		worst = MAX(worst, field.current[k].distance_to(Vector2(0, 1)));
		churned = MAX(churned, field.turbulence[k]);
		sheltered = MAX(sheltered, field.wake[k]);
	}
	CHECK(worst < 0.02f);
	CHECK(churned < 0.05f);
	CHECK(sheltered < 0.01f);
}

TEST_CASE("[LandscapeSplineFlow] The water parts around a rock, and is held back and churned up by it") {
	const int columns = 17;
	const float cell = 0.5f;
	LandscapeSplineFlow::Grid grid = make_channel(columns, 141, cell);
	// A rock 3 m across in midstream, 25 m down.
	const Vector2 rock(8 * cell, 50 * cell);
	for (int j = 0; j < grid.rows; j++) {
		for (int i = 0; i < columns; i++) {
			if (grid.positions[j * columns + i].distance_to(rock) <= 1.5f) {
				grid.depths[j * columns + i] = 0.0f;
			}
		}
	}
	LandscapeSplineFlow::Field field;
	LandscapeSplineFlow::solve(grid, field);
	auto at = [&](int p_column, int p_row) {
		return p_row * columns + p_column;
	};

	// Faster past it on either side...
	CHECK(field.current[at(3, 50)].length() > 1.1f);
	CHECK(field.current[at(13, 50)].length() > 1.1f);
	// ...turned aside ahead of it...
	CHECK(field.current[at(6, 45)].x < -0.05f);
	CHECK(field.current[at(10, 45)].x > 0.05f);
	// ...piling up against it...
	CHECK(field.turbulence[at(8, 46)] > 0.2f);
	// ...and sheltered in its wake, slowed down and churned up.
	CHECK(field.wake[at(8, 55)] > 0.5f);
	CHECK(field.current[at(8, 55)].y < 0.6f);
	CHECK(field.turbulence[at(8, 56)] > 0.2f);
	// The wake fades and spreads further down, and the current recovers.
	CHECK(field.wake[at(8, 120)] < field.wake[at(8, 55)] * 0.7f);
	CHECK(field.current[at(8, 120)].y > field.current[at(8, 55)].y + 0.2f);
	// Well upstream, nothing of it shows yet.
	CHECK(field.current[at(8, 10)].distance_to(Vector2(0, 1)) < 0.1f);
	CHECK(field.wake[at(8, 10)] < 0.01f);
}

TEST_CASE("[LandscapeSplineFlow] Shallows that span the channel speed it up and break it") {
	const int columns = 17;
	LandscapeSplineFlow::Grid grid = make_channel(columns, 121, 0.5f);
	for (int j = 50; j <= 60; j++) {
		for (int i = 0; i < columns; i++) {
			grid.depths[j * columns + i] = 0.4f;
		}
	}
	LandscapeSplineFlow::Field field;
	LandscapeSplineFlow::solve(grid, field);
	// The same water through a cross-section 0.4 as deep: 2.5 times as fast.
	CHECK(field.current[55 * columns + 8].y > 2.0f);
	CHECK(field.current[20 * columns + 8].y == doctest::Approx(1.0f).epsilon(0.05));
	// Pouring onto them churns it up.
	CHECK(field.turbulence[51 * columns + 8] > 0.3f);
	CHECK(field.turbulence[20 * columns + 8] < 0.05f);
}

TEST_CASE("[LandscapeSplineFlow] A cross-section that is all but dry does not stop the river") {
	const int columns = 17;
	LandscapeSplineFlow::Grid grid = make_channel(columns, 101, 0.5f);
	for (int i = 0; i < columns; i++) {
		grid.depths[40 * columns + i] = i == 8 ? 1.0f : 0.0f;
	}
	LandscapeSplineFlow::Field field;
	LandscapeSplineFlow::solve(grid, field);
	CHECK(field.current[80 * columns + 8].y == doctest::Approx(1.0f).epsilon(0.1));
	CHECK(field.current[20 * columns + 8].y == doctest::Approx(1.0f).epsilon(0.1));
}

TEST_CASE("[LandscapeSplineFlow] Bends carry the fastest water to their outer bank") {
	// Half a circle around the origin, 8 m wide, from 20 m to 28 m out:
	// across points outwards, so the outer bank is the last column.
	const int columns = 17;
	const int rows = 151;
	LandscapeSplineFlow::Grid grid;
	grid.columns = columns;
	grid.rows = rows;
	grid.positions.resize(columns * rows);
	grid.depths.resize(columns * rows);
	for (int j = 0; j < rows; j++) {
		const float angle = j * 0.5f / 24.0f;
		for (int i = 0; i < columns; i++) {
			const float radius = 20.0f + i * 0.5f;
			grid.positions[j * columns + i] = Vector2(Math::cos(angle), Math::sin(angle)) * radius;
			grid.depths[j * columns + i] = 1.0f;
		}
	}
	LandscapeSplineFlow::Field field;
	LandscapeSplineFlow::solve(grid, field);
	const int row = 110;
	CHECK(field.current[row * columns + columns - 2].length() > field.current[row * columns + 1].length() * 1.05f);
}

TEST_CASE("[LandscapeSplineFlow] Packing a field into texels") {
	LandscapeSplineFlow::Field field;
	field.columns = 2;
	field.rows = 3;
	field.current = LocalVector<Vector2>({ Vector2(0, 0), Vector2(0, 0), Vector2(0, 1), Vector2(-1.5f, 0), Vector2(0, 2), Vector2(0, 2) });
	field.turbulence = LocalVector<float>({ 0.0f, 0.0f, 0.5f, 0.0f, 1.0f, 1.0f });
	field.wake = LocalVector<float>({ 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f });
	Vector<uint8_t> texels;
	// Rows 1 and 2, and halfway between them.
	LandscapeSplineFlow::pack_rows(field, 1.0f, 2.0f, 3, texels);
	REQUIRE(texels.size() == 2 * 3 * 4);
	auto decode = [&](int p_column, int p_row, int p_channel) {
		return texels[(p_row * 2 + p_column) * 4 + p_channel] / 255.0f;
	};
	CHECK((decode(0, 0, 1) * 2.0f - 1.0f) * LandscapeSplineFlow::VELOCITY_RANGE == doctest::Approx(1.0f).epsilon(0.03));
	CHECK((decode(1, 0, 0) * 2.0f - 1.0f) * LandscapeSplineFlow::VELOCITY_RANGE == doctest::Approx(-1.5f).epsilon(0.03));
	CHECK((decode(0, 1, 1) * 2.0f - 1.0f) * LandscapeSplineFlow::VELOCITY_RANGE == doctest::Approx(1.5f).epsilon(0.03));
	CHECK(decode(0, 1, 2) == doctest::Approx(0.75f).epsilon(0.01));
	CHECK(decode(1, 0, 3) == doctest::Approx(1.0f));
	CHECK(decode(1, 2, 3) == doctest::Approx(0.0f));
}

// The flow field of p_spline at p_distance along it and p_u across it, as
// decoded by the water shader: current in xy, turbulence in z, wake in w.
static Vector4 spline_flow_at(LandscapeSpline3D *p_spline, float p_distance, float p_u) {
	const float chunk_length = p_spline->get_chunk_length();
	const int chunk = MIN((int)(p_distance / chunk_length), p_spline->get_chunk_count() - 1);
	const float from = chunk * chunk_length;
	const float to = MIN(from + chunk_length, p_spline->get_length());
	const uint32_t tile = p_spline->get_chunk_flow_tile(chunk);
	if (tile == 0) {
		return Vector4();
	}
	LandscapeFlowAtlas *atlas = LandscapeFlowAtlas::get_singleton();
	const Vector4 transform = atlas->get_tile_transform(tile);
	const float size = atlas->get_size();
	const float t = (p_distance - from) / (to - from);
	const int x = (int)Math::round((transform.x + p_u * transform.z) * size - 0.5f);
	const int y = (int)Math::round((transform.y + t * transform.w) * size - 0.5f);
	const Color texel = atlas->get_image()->get_pixel(x, y);
	const float range = LandscapeSplineFlow::VELOCITY_RANGE;
	return Vector4((texel.r * 2.0f - 1.0f) * range, (texel.g * 2.0f - 1.0f) * range, texel.b, texel.a);
}

TEST_CASE("[SceneTree][LandscapeSpline3D] A river's current parts around the rocks standing in it") {
	SplineScene scene(make_terrain());
	scene.spline->apply_preset(LandscapeSpline3D::TYPE_RIVER);
	scene.spline->set_chunk_length(64.0f);
	// 16 m wide and a meter deep over the flat ground, running along +X: its
	// right bank (u = 1) is towards +Z.
	scene.spline->set_curve(make_line(Vector3(100, 1, 256), Vector3(300, 1, 256)));
	scene.spline->update_mesh();
	scene.spline->update_flow();

	REQUIRE(scene.spline->get_chunk_count() == 4);
	for (int i = 0; i < scene.spline->get_chunk_count(); i++) {
		CHECK(scene.spline->get_chunk_flow_tile(i) != 0);
	}
	// UV2 runs across the strip and along each chunk, for the shader to find
	// its place in the tile.
	const Array arrays = RS::get_singleton()->mesh_surface_get_arrays(scene.spline->get_chunk_mesh(0), 0);
	const PackedVector2Array uv2 = arrays[RSE::ARRAY_TEX_UV2];
	REQUIRE(uv2.size() > 0);
	// (Within what the attributes' 16 bit compression keeps of them.)
	for (const Vector2 &uv : uv2) {
		CHECK(uv.x >= -0.001f);
		CHECK(uv.x <= 1.001f);
		CHECK(uv.y >= -0.001f);
		CHECK(uv.y <= 1.001f);
	}
	Vector4 midstream = spline_flow_at(scene.spline, 103.0f, 0.5f);
	CHECK(Vector2(midstream.x, midstream.y).distance_to(Vector2(0, 1)) < 0.1f);

	// A boulder 3 m across standing out of the water, 100 m down.
	StaticBody3D *rock = memnew(StaticBody3D);
	CollisionShape3D *collision = memnew(CollisionShape3D);
	Ref<BoxShape3D> box;
	box.instantiate();
	box->set_size(Vector3(3, 4, 3));
	collision->set_shape(box);
	rock->add_child(collision);
	rock->set_position(Vector3(200, 1, 256));
	SceneTree::get_singleton()->get_root()->add_child(rock);

	scene.spline->update_flow();
	const Vector4 beside = spline_flow_at(scene.spline, 100.0f, 0.5f + 3.0f / 16.0f);
	const Vector4 behind = spline_flow_at(scene.spline, 104.0f, 0.5f);
	CHECK(Vector2(beside.x, beside.y).length() > 1.05f);
	CHECK(behind.y < 0.6f);
	CHECK(behind.w > 0.3f);
	CHECK(behind.z > 0.2f);
	midstream = spline_flow_at(scene.spline, 40.0f, 0.5f);
	CHECK(Vector2(midstream.x, midstream.y).distance_to(Vector2(0, 1)) < 0.1f);

	SUBCASE("Taking the rock away clears its wake") {
		rock->get_parent()->remove_child(rock);
		scene.spline->update_flow();
		const Vector4 clear = spline_flow_at(scene.spline, 104.0f, 0.5f);
		CHECK(clear.w < 0.05f);
		CHECK(clear.y > 0.9f);
	}

	SUBCASE("Turning the flow field off hands its tiles back") {
		const int tiles = LandscapeFlowAtlas::get_singleton()->get_tile_count();
		scene.spline->set_flow_enabled(false);
		for (int i = 0; i < scene.spline->get_chunk_count(); i++) {
			CHECK(scene.spline->get_chunk_flow_tile(i) == 0);
		}
		CHECK(LandscapeFlowAtlas::get_singleton()->get_tile_count() == tiles - 4);
	}

	memdelete(rock);
}

TEST_CASE("[SceneTree][LandscapeSpline3D] Roads and lakes have no flow field") {
	SplineScene scene(make_terrain());
	scene.spline->set_curve(make_line(Vector3(100, 1, 256), Vector3(300, 1, 256)));
	scene.spline->update_mesh();
	scene.spline->update_flow();
	for (int i = 0; i < scene.spline->get_chunk_count(); i++) {
		CHECK(scene.spline->get_chunk_flow_tile(i) == 0);
	}
}

} // namespace TestLandscapeSpline3D
