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

#include "scene/3d/landscape_3d.h"
#include "scene/3d/landscape_spline_3d.h"
#include "scene/3d/terrain_data.h"
#include "scene/3d/terrain_layer.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
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

} // namespace TestLandscapeSpline3D
