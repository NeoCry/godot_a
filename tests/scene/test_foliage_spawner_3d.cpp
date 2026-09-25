/**************************************************************************/
/*  test_foliage_spawner_3d.cpp                                           */
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

TEST_FORCE_LINK(test_foliage_spawner_3d)

#include "core/io/image.h"
#include "scene/3d/foliage_lod_level.h"
#include "scene/3d/foliage_spawner_3d.h"
#include "scene/3d/landscape_3d.h"
#include "scene/3d/landscape_spline_3d.h"
#include "scene/3d/terrain_data.h"
#include "scene/3d/terrain_layer.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/curve.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/multimesh.h"

namespace TestFoliageSpawner3D {

// 64 x 64 m of flat ground at 1 m spacing, all of it layer 0, as a new
// terrain starts out.
static Ref<TerrainData> make_terrain() {
	Ref<TerrainData> data;
	data.instantiate();
	data->set_resolution(65);
	data->set_vertex_spacing(1.0f);
	return data;
}

static void set_layer_count(Landscape3D *p_landscape, int p_count) {
	TypedArray<TerrainLayer> layers;
	for (int i = 0; i < p_count; i++) {
		Ref<TerrainLayer> layer;
		layer.instantiate();
		layers.push_back(layer);
	}
	p_landscape->set_layers(layers);
}

static Ref<Curve3D> make_line(const Vector3 &p_from, const Vector3 &p_to) {
	Ref<Curve3D> curve;
	curve.instantiate();
	curve->add_point(p_from);
	curve->add_point(p_to);
	return curve;
}

static Ref<Curve3D> make_square(const Vector2 &p_from, const Vector2 &p_to) {
	Ref<Curve3D> curve;
	curve.instantiate();
	curve->add_point(Vector3(p_from.x, 0, p_from.y));
	curve->add_point(Vector3(p_to.x, 0, p_from.y));
	curve->add_point(Vector3(p_to.x, 0, p_to.y));
	curve->add_point(Vector3(p_from.x, 0, p_to.y));
	curve->set_closed(true);
	return curve;
}

// A landscape at the origin, and a spawner fitted to it that places one
// instance per square meter, with no spacing between them to get in the way:
// with nothing ruling any place out, it fills every one of the 64 * 64.
struct FoliageScene {
	Landscape3D *landscape = nullptr;
	FoliageSpawner3D *spawner = nullptr;

	explicit FoliageScene(const Ref<TerrainData> &p_data) {
		landscape = memnew(Landscape3D);
		landscape->set_terrain_data(p_data);
		SceneTree::get_singleton()->get_root()->add_child(landscape);

		spawner = memnew(FoliageSpawner3D);
		SceneTree::get_singleton()->get_root()->add_child(spawner);
		Ref<BoxMesh> mesh;
		mesh.instantiate();
		Ref<FoliageLODLevel> level;
		level.instantiate();
		level->set_mesh(mesh);
		TypedArray<FoliageLODLevel> levels;
		levels.push_back(level);
		spawner->set_lod_levels(levels);
		spawner->set_ground_mesh_path(spawner->get_path_to(landscape));
		spawner->fit_to_ground_mesh();
		spawner->set_density(1.0f);
		spawner->set_min_distance(0.0f);
		spawner->set_max_instances(1000000);
		// Small enough to tell where every instance is from the cell it went
		// into (see get_instances()).
		spawner->set_cell_size(0.5f);
	}

	~FoliageScene() {
		memdelete(spawner);
		memdelete(landscape);
	}
};

// Where p_spawner's instances are on the landscape (global XZ, which is the
// landscape's own space here), to within half a cell's diagonal: the headless
// renderer keeps no MultiMesh transforms to read back, but the cells are laid
// out from them. One entry per instance, at its cell's center.
static constexpr float CELL_TOLERANCE = 0.36f;

static LocalVector<Vector2> get_instances(FoliageSpawner3D *p_spawner) {
	LocalVector<Vector2> result;
	const Transform3D xform = p_spawner->get_global_transform();
	const float size = p_spawner->get_cell_size();
	const Array cells = p_spawner->call("_get_cell_data");
	for (int i = 0; i < cells.size(); i++) {
		const Array cell = cells[i];
		const Vector2i coord = cell[0];
		const Array lods = cell[1];
		const Ref<MultiMesh> multimesh = lods[0];
		const Vector3 center = xform.xform(Vector3((coord.x + 0.5f) * size, 0.0f, (coord.y + 0.5f) * size));
		for (int j = 0; j < multimesh->get_instance_count(); j++) {
			result.push_back(Vector2(center.x, center.z));
		}
	}
	return result;
}

static int count_instances_in(const LocalVector<Vector2> &p_instances, const Rect2 &p_area) {
	int count = 0;
	for (const Vector2 &instance : p_instances) {
		count += p_area.has_point(instance) ? 1 : 0;
	}
	return count;
}

TEST_CASE("[SceneTree][FoliageSpawner3D] Growing only on the chosen terrain layers") {
	// Layer 1 painted over the half of the terrain towards -X, layer 0 left on
	// the other half.
	Ref<TerrainData> data = make_terrain();
	for (int z = 0; z < 65; z++) {
		for (int x = 0; x < 32; x++) {
			data->set_layer_weight(x, z, 0, 0.0f);
			data->set_layer_weight(x, z, 1, 1.0f);
		}
	}
	FoliageScene scene(data);
	set_layer_count(scene.landscape, 2);

	scene.spawner->regenerate();
	const int everywhere = scene.spawner->get_instance_count();
	CHECK(everywhere == 64 * 64);

	SUBCASE("Only where the chosen layer is painted") {
		scene.spawner->set_terrain_layer_mask(1 << 0);
		scene.spawner->regenerate();
		const LocalVector<Vector2> instances = get_instances(scene.spawner);
		REQUIRE(instances.size() > 0);
		for (const Vector2 &instance : instances) {
			// Layer 0 fades in between the samples at x = 31 and x = 32.
			CHECK(instance.x > 31.0f - CELL_TOLERANCE);
		}
		// Just as densely as before over the half it grows on: what the layers
		// rule out is left bare, not crowded into what is left.
		CHECK(scene.spawner->get_instance_count() > everywhere * 0.46f);
		CHECK(scene.spawner->get_instance_count() < everywhere * 0.55f);
	}

	SUBCASE("Everywhere but where the chosen layer is painted") {
		scene.spawner->set_terrain_layer_mask(1 << 0);
		scene.spawner->set_terrain_layer_mask_invert(true);
		scene.spawner->regenerate();
		const LocalVector<Vector2> instances = get_instances(scene.spawner);
		REQUIRE(instances.size() > 0);
		for (const Vector2 &instance : instances) {
			CHECK(instance.x < 32.0f + CELL_TOLERANCE);
		}
	}

	SUBCASE("Several layers at once") {
		scene.spawner->set_terrain_layer_mask((1 << 0) | (1 << 1));
		scene.spawner->regenerate();
		CHECK(scene.spawner->get_instance_count() == everywhere);
	}

	SUBCASE("A layer the landscape does not have") {
		// Weight painted for a layer past the landscape's own is never rendered,
		// so it counts for nothing.
		for (int z = 0; z < 65; z++) {
			for (int x = 0; x < 65; x++) {
				data->set_layer_weight(x, z, 2, 1.0f);
			}
		}
		scene.spawner->set_terrain_layer_mask(1 << 2);
		scene.spawner->regenerate();
		CHECK(scene.spawner->get_instance_count() == 0);
		CHECK_FALSE(scene.spawner->get_configuration_warnings().is_empty());
	}
}

TEST_CASE("[SceneTree][FoliageSpawner3D] Thinning out across a blend between terrain layers") {
	// Layer 1 rising steadily from nothing at x = 0 to everything at x = 64.
	Ref<TerrainData> data = make_terrain();
	for (int z = 0; z < 65; z++) {
		for (int x = 0; x < 65; x++) {
			data->set_layer_weight(x, z, 0, 1.0f - x / 64.0f);
			data->set_layer_weight(x, z, 1, x / 64.0f);
		}
	}
	FoliageScene scene(data);
	set_layer_count(scene.landscape, 2);
	scene.spawner->set_terrain_layer_mask(1 << 1);
	const Rect2 low_quarter(0, 0, 16, 64);
	const Rect2 high_quarter(48, 0, 16, 64);

	SUBCASE("Density follows the layer's share of the ground") {
		scene.spawner->set_terrain_layer_threshold(0.0f);
		scene.spawner->regenerate();
		const LocalVector<Vector2> instances = get_instances(scene.spawner);
		// Half of the ground's places, on average...
		CHECK(instances.size() > 64 * 64 * 0.45f);
		CHECK(instances.size() < 64 * 64 * 0.55f);
		// ...and seven times as many in the quarter where the layer covers
		// seven eighths of the ground on average as in the one where it covers
		// an eighth.
		const int low = count_instances_in(instances, low_quarter);
		const int high = count_instances_in(instances, high_quarter);
		CHECK(low > 0);
		CHECK(high > low * 5);
		CHECK(high < low * 9);
	}

	SUBCASE("Nothing below the threshold") {
		scene.spawner->set_terrain_layer_threshold(0.5f);
		scene.spawner->regenerate();
		const LocalVector<Vector2> instances = get_instances(scene.spawner);
		REQUIRE(instances.size() > 0);
		for (const Vector2 &instance : instances) {
			CHECK(instance.x > 32.0f - 1.0f - CELL_TOLERANCE);
		}
		// Rising from none at x = 32 to full density at x = 64.
		CHECK(instances.size() > 64 * 16 * 0.85f);
		CHECK(instances.size() < 64 * 16 * 1.15f);
	}

	SUBCASE("Only where nothing else is painted") {
		// The layer alone from x = 48 on.
		for (int z = 0; z < 65; z++) {
			for (int x = 48; x < 65; x++) {
				data->set_layer_weight(x, z, 0, 0.0f);
				data->set_layer_weight(x, z, 1, 1.0f);
			}
		}
		scene.spawner->set_terrain_layer_threshold(1.0f);
		scene.spawner->regenerate();
		const LocalVector<Vector2> instances = get_instances(scene.spawner);
		for (const Vector2 &instance : instances) {
			CHECK(instance.x > 48.0f - CELL_TOLERANCE);
		}
		// All of it, at full density.
		CHECK(instances.size() > 64 * 16 * 0.9f);
		CHECK(instances.size() < 64 * 16 * 1.1f);
	}
}

TEST_CASE("[SceneTree][FoliageSpawner3D] Keeping off roads") {
	FoliageScene scene(make_terrain());
	LandscapeSpline3D *road = memnew(LandscapeSpline3D);
	scene.landscape->add_child(road);
	// Across the whole terrain along X, 6 m wide; its rounded ends lie past
	// the terrain's edges.
	road->set_curve(make_line(Vector3(-8, 0, 32), Vector3(72, 0, 32)));
	road->set_width(6.0f);
	scene.spawner->regenerate();

	SUBCASE("Nothing on the road, and right up to its edges beside it") {
		const LocalVector<Vector2> instances = get_instances(scene.spawner);
		for (const Vector2 &instance : instances) {
			CHECK(Math::abs(instance.y - 32.0f) > 3.0f - CELL_TOLERANCE);
		}
		CHECK(count_instances_in(instances, Rect2(0, 35.5f, 64, 1.0f)) > 0);
		CHECK(count_instances_in(instances, Rect2(0, 27.5f, 64, 1.0f)) > 0);
		// And only the road's share of the ground is lost.
		CHECK(instances.size() > 64 * 58 * 0.95f);
		CHECK(instances.size() < 64 * 58 * 1.05f);
	}

	SUBCASE("Further off with a margin") {
		scene.spawner->set_spline_margin(2.0f);
		scene.spawner->regenerate();
		for (const Vector2 &instance : get_instances(scene.spawner)) {
			CHECK(Math::abs(instance.y - 32.0f) > 5.0f - CELL_TOLERANCE);
		}
	}

	SUBCASE("Growing on a kind of spline left out of spline_avoid") {
		scene.spawner->set_spline_avoid(scene.spawner->get_spline_avoid() & ~(1u << LandscapeSpline3D::TYPE_ROAD));
		scene.spawner->regenerate();
		CHECK(scene.spawner->get_instance_count() == 64 * 64);
	}

	SUBCASE("A river is kept off the same way, when it is a kind avoided") {
		road->set_spline_type(LandscapeSpline3D::TYPE_RIVER);
		scene.spawner->set_spline_avoid(1u << LandscapeSpline3D::TYPE_RIVER);
		scene.spawner->regenerate();
		for (const Vector2 &instance : get_instances(scene.spawner)) {
			CHECK(Math::abs(instance.y - 32.0f) > 3.0f - CELL_TOLERANCE);
		}
	}

	SUBCASE("Following the road when it moves") {
		road->set_position(Vector3(0, 0, -10));
		scene.spawner->regenerate();
		for (const Vector2 &instance : get_instances(scene.spawner)) {
			CHECK(Math::abs(instance.y - 22.0f) > 3.0f - CELL_TOLERANCE);
		}
	}
}

TEST_CASE("[SceneTree][FoliageSpawner3D] Keeping out of a lake's water") {
	// A hollow 2 m deep in the middle of the terrain, with an island in it.
	Ref<TerrainData> data = make_terrain();
	for (int z = 20; z <= 44; z++) {
		for (int x = 20; x <= 44; x++) {
			const bool island = x >= 30 && x <= 34 && z >= 30 && z <= 34;
			data->set_height(x, z, island ? 1.0f : -2.0f);
		}
	}
	FoliageScene scene(data);
	// The hollow's sides are steeper than foliage grows on by default, which
	// would get in the way of telling what the lake does.
	scene.spawner->set_max_slope_degrees(90.0f);
	LandscapeSpline3D *lake = memnew(LandscapeSpline3D);
	scene.landscape->add_child(lake);
	lake->apply_preset(LandscapeSpline3D::TYPE_LAKE);
	lake->set_smooth(false);
	// Drawn well around the hollow, on level ground: its water stands at the
	// ground's level, and fills the hollow only.
	lake->set_curve(make_square(Vector2(12, 12), Vector2(52, 52)));
	scene.spawner->regenerate();

	SUBCASE("Nothing under the water, but on the island and the dry ground inside the shoreline") {
		const LocalVector<Vector2> instances = get_instances(scene.spawner);
		// The ground drops below the water's level just past x = 19 and
		// z = 19, and comes back up just before x = 45 and z = 45; the island
		// stands above it between about 29.7 and 34.3.
		const Rect2 water = Rect2(19, 19, 26, 26).grow(-CELL_TOLERANCE);
		const Rect2 island = Rect2(29.5f, 29.5f, 5, 5).grow(CELL_TOLERANCE);
		for (const Vector2 &instance : instances) {
			CHECK_FALSE((water.has_point(instance) && !island.has_point(instance)));
		}
		CHECK(count_instances_in(instances, Rect2(30.5f, 30.5f, 3, 3)) > 0);
		CHECK(count_instances_in(instances, Rect2(13, 13, 5, 38)) > 0);
		CHECK(count_instances_in(instances, Rect2(0, 0, 11, 64)) > 0);
	}

	SUBCASE("A margin keeps it off both sides of the shoreline") {
		scene.spawner->set_spline_margin(3.0f);
		scene.spawner->regenerate();
		const Rect2 shoreline(12, 12, 40, 40);
		for (const Vector2 &instance : get_instances(scene.spawner)) {
			// How far the instance is from the shoreline, inside or out.
			const float from_shore = shoreline.has_point(instance)
					? MIN(MIN(instance.x - 12.0f, 52.0f - instance.x), MIN(instance.y - 12.0f, 52.0f - instance.y))
					: instance.distance_to(instance.clamp(Vector2(12, 12), Vector2(52, 52)));
			CHECK(from_shore > 3.0f - CELL_TOLERANCE);
		}
	}

	SUBCASE("Water plants on the lake's bed, with lakes left out of spline_avoid") {
		scene.spawner->set_spline_avoid(scene.spawner->get_spline_avoid() & ~(1u << LandscapeSpline3D::TYPE_LAKE));
		scene.spawner->regenerate();
		CHECK(scene.spawner->get_instance_count() == 64 * 64);
	}
}

TEST_CASE("[SceneTree][FoliageSpawner3D] Nothing stands over a hole in the landscape, or past its edges") {
	FoliageScene scene(make_terrain());
	scene.landscape->set_hole(Vector3(32, 0, 32), 6.0f, true);
	// Twice the terrain's size along X and Z, around it.
	scene.spawner->set_volume_size(Vector3(128, 1, 128));
	scene.spawner->regenerate();

	const LocalVector<Vector2> instances = get_instances(scene.spawner);
	for (const Vector2 &instance : instances) {
		CHECK(Rect2(0, 0, 64, 64).grow(CELL_TOLERANCE).has_point(instance));
		CHECK(instance.distance_to(Vector2(32, 32)) > 6.0f - CELL_TOLERANCE);
	}
	// The terrain's quarter of the volume's places, less the hole: its samples
	// out to 6 m, and every quad around them.
	CHECK(instances.size() > (64 * 64 - Math::PI * 7.5f * 7.5f) * 0.95f);
	CHECK(instances.size() < (64 * 64 - Math::PI * 6.0f * 6.0f) * 1.05f);
}

TEST_CASE("[SceneTree][FoliageSpawner3D] A distribution mask thins foliage out rather than moving it") {
	FoliageScene scene(make_terrain());
	// Black over the half towards -X, white over the other.
	Ref<Image> image = Image::create_empty(2, 1, false, Image::FORMAT_L8);
	image->set_pixel(0, 0, Color(0, 0, 0));
	image->set_pixel(1, 0, Color(1, 1, 1));
	scene.spawner->set_distribution_mask(ImageTexture::create_from_image(image));
	scene.spawner->regenerate();

	const LocalVector<Vector2> instances = get_instances(scene.spawner);
	for (const Vector2 &instance : instances) {
		CHECK(instance.x > 32.0f - CELL_TOLERANCE);
	}
	CHECK(instances.size() > 64 * 32 * 0.9f);
	CHECK(instances.size() < 64 * 32 * 1.1f);
}

} // namespace TestFoliageSpawner3D
