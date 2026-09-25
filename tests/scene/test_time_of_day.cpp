/**************************************************************************/
/*  test_time_of_day.cpp                                                  */
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

TEST_FORCE_LINK(test_time_of_day)

#include "core/object/callable_mp.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/node_3d.h"
#include "scene/3d/time_of_day.h"
#include "scene/3d/time_of_day_profile.h"
#include "scene/3d/world_environment.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/sky_material.h"
#include "scene/resources/environment.h"
#include "scene/resources/material.h"
#include "scene/resources/sky.h"
#include "tests/signal_watcher.h"

namespace TestTimeOfDay {

// Signal records for SIGNAL_CHECK: emitted once, or twice, without arguments.
static Array once() {
	return { {} };
}

static Array twice() {
	return { {}, {} };
}

// A curve over the whole day that runs in a straight line from p_start at
// midnight to p_end at the next midnight, so any hour is easy to predict.
static Ref<Curve> make_line(double p_start, double p_end, double p_max_domain = 24.0) {
	Ref<Curve> curve;
	curve.instantiate();
	curve->set_max_domain(p_max_domain);
	curve->set_max_value(MAX(p_start, p_end) + 1.0);
	curve->set_min_value(MIN(0.0, MIN(p_start, p_end)));
	curve->add_point(Vector2(0, p_start), 0, 0, Curve::TANGENT_LINEAR, Curve::TANGENT_LINEAR);
	curve->add_point(Vector2(p_max_domain, p_end), 0, 0, Curve::TANGENT_LINEAR, Curve::TANGENT_LINEAR);
	return curve;
}

static Ref<Curve> make_constant(double p_value) {
	return make_line(p_value, p_value);
}

static Ref<Gradient> make_colors(const Color &p_midnight, const Color &p_next_midnight) {
	Ref<Gradient> gradient;
	gradient.instantiate();
	gradient->set_offsets({ 0.0, 1.0 });
	gradient->set_colors({ p_midnight, p_next_midnight });
	return gradient;
}

class ChangeCounter : public Object {
	GDSOFTCLASS(ChangeCounter, Object);

public:
	int changes = 0;
	void count() { changes++; }
};

// A small scene: a sun, a moon and an environment with a procedural sky,
// driven by a TimeOfDay that sits next to them.
struct TestScene {
	Node3D *root = nullptr;
	DirectionalLight3D *sun = nullptr;
	DirectionalLight3D *moon = nullptr;
	WorldEnvironment *world_environment = nullptr;
	Ref<Environment> environment;
	Ref<ProceduralSkyMaterial> sky_material;
	TimeOfDay *time_of_day = nullptr;

	TestScene() {
		root = memnew(Node3D);

		sun = memnew(DirectionalLight3D);
		sun->set_name("Sun");
		root->add_child(sun);

		moon = memnew(DirectionalLight3D);
		moon->set_name("Moon");
		root->add_child(moon);

		sky_material.instantiate();
		Ref<Sky> sky;
		sky.instantiate();
		sky->set_material(sky_material);
		environment.instantiate();
		environment->set_background(Environment::BG_SKY);
		environment->set_sky(sky);
		world_environment = memnew(WorldEnvironment);
		world_environment->set_name("WorldEnvironment");
		world_environment->set_environment(environment);
		root->add_child(world_environment);

		time_of_day = memnew(TimeOfDay);
		time_of_day->set_name("TimeOfDay");
		time_of_day->set_sun_light_path(NodePath("../Sun"));
		time_of_day->set_moon_light_path(NodePath("../Moon"));
		time_of_day->set_world_environment_path(NodePath("../WorldEnvironment"));
		root->add_child(time_of_day);

		SceneTree::get_singleton()->get_root()->add_child(root);
		update();
	}

	~TestScene() {
		memdelete(root);
	}

	// Lets queued updates through, the way a frame would.
	void update() {
		SceneTree::get_singleton()->process(0.0);
	}
};

TEST_CASE("[TimeOfDay] The sun moves across the sky like the real one") {
	TimeOfDay *time_of_day = memnew(TimeOfDay);
	// The defaults: 45 degrees north, at the spring equinox.

	SUBCASE("It rises in the east, is highest in the south at noon and sets in the west") {
		Vector3 sunrise = time_of_day->get_sun_direction_at(6.0);
		CHECK(sunrise.is_equal_approx(Vector3(1, 0, 0)));

		Vector3 noon = time_of_day->get_sun_direction_at(12.0);
		CHECK(noon.is_equal_approx(Vector3(0, Math::SQRT12, Math::SQRT12)));

		Vector3 sunset = time_of_day->get_sun_direction_at(18.0);
		CHECK(sunset.is_equal_approx(Vector3(-1, 0, 0)));

		CHECK(time_of_day->get_sun_direction_at(0.0).is_equal_approx(Vector3(0, -Math::SQRT12, -Math::SQRT12)));
		CHECK(time_of_day->get_sunrise_time() == doctest::Approx(6.0));
		CHECK(time_of_day->get_sunset_time() == doctest::Approx(18.0));
	}

	SUBCASE("Summer days are long and the noon sun high") {
		time_of_day->set_day_of_year(172);
		// About fifteen and a half hours of daylight, centered on noon.
		CHECK(time_of_day->get_sunrise_time() == doctest::Approx(4.287).epsilon(0.001));
		CHECK(time_of_day->get_sunset_time() == doctest::Approx(19.713).epsilon(0.001));
		const double noon_elevation = Math::rad_to_deg(Math::asin(time_of_day->get_sun_direction_at(12.0).y));
		CHECK(noon_elevation == doctest::Approx(45.0 + 23.44).epsilon(0.001));
	}

	SUBCASE("Close to the pole, the summer sun never sets") {
		time_of_day->set_latitude(80.0);
		time_of_day->set_day_of_year(172);
		CHECK(time_of_day->get_sunrise_time() == -1.0);
		CHECK(time_of_day->get_sunset_time() == -1.0);
		CHECK(time_of_day->get_sun_direction_at(0.0).y > 0.0);
	}

	SUBCASE("The north offset turns the compass") {
		time_of_day->set_north_offset(90.0);
		CHECK(time_of_day->get_sun_direction_at(6.0).is_equal_approx(Vector3(0, 0, -1)));
	}

	SUBCASE("The full moon stands opposite the sun, and the new moon rides with it") {
		time_of_day->set_moon_phase(0.5);
		CHECK(time_of_day->get_moon_direction_at(0.0).is_equal_approx(time_of_day->get_sun_direction_at(12.0)));
		CHECK(time_of_day->get_moon_direction_at(18.0).is_equal_approx(time_of_day->get_sun_direction_at(6.0)));

		time_of_day->set_moon_phase(0.0);
		CHECK(time_of_day->get_moon_direction_at(9.0).is_equal_approx(time_of_day->get_sun_direction_at(9.0)));
	}

	memdelete(time_of_day);
}

TEST_CASE("[TimeOfDay] Curves follow the sunrise and the sunset through the seasons") {
	TimeOfDay *time_of_day = memnew(TimeOfDay);

	// At the equinox the real day already matches the 6:00 to 18:00 the
	// curves are made for.
	for (double hour : { 0.0, 5.0, 6.0, 12.0, 17.5, 23.0 }) {
		CHECK(time_of_day->get_curve_time(hour) == doctest::Approx(hour));
	}

	time_of_day->set_day_of_year(172);
	const double sunrise = time_of_day->get_sunrise_time();
	const double sunset = time_of_day->get_sunset_time();
	CHECK(time_of_day->get_curve_time(sunrise) == doctest::Approx(6.0));
	CHECK(time_of_day->get_curve_time(12.0) == doctest::Approx(12.0));
	CHECK(time_of_day->get_curve_time(sunset) == doctest::Approx(18.0));
	// Three quarters through the night is 3:00, whatever the night's length.
	CHECK(time_of_day->get_curve_time(Math::fposmod(sunset + (24.0 - sunset + sunrise) * 0.75, 24.0)) == doctest::Approx(3.0));

	// Just before sunrise the night is almost over.
	CHECK(time_of_day->get_curve_time(sunrise - 0.001) == doctest::Approx(6.0).epsilon(0.001));

	time_of_day->set_align_curves_to_sun(false);
	CHECK(time_of_day->get_curve_time(sunrise) == doctest::Approx(sunrise));

	memdelete(time_of_day);
}

TEST_CASE("[TimeOfDayProfile] Tracks") {
	Ref<TimeOfDayProfile> profile;
	profile.instantiate();

	const int energy = profile->add_track(TimeOfDayProfile::TARGET_SUN, "light_energy");
	const int fog = profile->add_track(TimeOfDayProfile::TARGET_ENVIRONMENT, "fog_density");
	const int lamp = profile->add_track(TimeOfDayProfile::TARGET_NODE, "light_energy", NodePath("../Lamp"));
	CHECK(profile->get_track_count() == 3);

	CHECK(profile->find_track(TimeOfDayProfile::TARGET_SUN, "light_energy") == energy);
	CHECK(profile->find_track(TimeOfDayProfile::TARGET_ENVIRONMENT, "fog_density") == fog);
	CHECK(profile->find_track(TimeOfDayProfile::TARGET_NODE, "light_energy", NodePath("../Lamp")) == lamp);
	CHECK(profile->find_track(TimeOfDayProfile::TARGET_NODE, "light_energy", NodePath("../Other")) == -1);
	CHECK(profile->find_track(TimeOfDayProfile::TARGET_MOON, "light_energy") == -1);

	SUBCASE("A curve spans the day whatever its domain") {
		profile->set_track_curve(energy, make_line(0.0, 2.0));
		Variant value;
		CHECK(profile->sample(energy, 6.0, Variant::FLOAT, value));
		CHECK(double(value) == doctest::Approx(0.5));

		profile->set_track_curve(energy, make_line(0.0, 2.0, 1.0));
		CHECK(profile->sample(energy, 6.0, Variant::FLOAT, value));
		CHECK(double(value) == doctest::Approx(0.5));

		CHECK(profile->sample(energy, 18.0, Variant::INT, value));
		CHECK(int64_t(value) == 2);
		CHECK(profile->sample(energy, 3.0, Variant::BOOL, value));
		CHECK_FALSE(bool(value));
		CHECK(profile->sample(energy, 9.0, Variant::BOOL, value));
		CHECK(bool(value));

		// Without a Gradient there is no color to give.
		CHECK_FALSE(profile->sample(energy, 6.0, Variant::COLOR, value));
	}

	SUBCASE("A gradient spans the day") {
		profile->set_track_gradient(fog, make_colors(Color(0, 0, 0), Color(1, 1, 1)));
		Variant value;
		CHECK(profile->sample(fog, 12.0, Variant::COLOR, value));
		CHECK(Color(value).is_equal_approx(Color(0.5, 0.5, 0.5)));
		CHECK_FALSE(profile->sample(fog, 12.0, Variant::FLOAT, value));
	}

	SUBCASE("Tracks are properties, for the Inspector and for saving") {
		CHECK(int(profile->get("tracks/0/target")) == TimeOfDayProfile::TARGET_SUN);
		CHECK(StringName(profile->get("tracks/1/property")) == StringName("fog_density"));
		CHECK(NodePath(profile->get("tracks/2/node_path")) == NodePath("../Lamp"));

		Ref<Curve> curve = make_constant(3.0);
		profile->set("tracks/1/curve", curve);
		CHECK(profile->get_track_curve(fog) == curve);

		List<PropertyInfo> properties;
		profile->get_property_list(&properties);
		bool has_node_path = false;
		bool has_sun_node_path = false;
		for (const PropertyInfo &info : properties) {
			has_node_path = has_node_path || info.name == "tracks/2/node_path";
			has_sun_node_path = has_sun_node_path || info.name == "tracks/0/node_path";
		}
		CHECK(has_node_path);
		// Only node tracks need a path.
		CHECK_FALSE(has_sun_node_path);

		CHECK(profile->property_can_revert("tracks/1/curve"));
		CHECK(profile->property_get_revert("tracks/1/curve") == Variant());
	}

	SUBCASE("Reordering and removing tracks keeps them whole") {
		Ref<Curve> curve = make_constant(1.0);
		profile->set_track_curve(fog, curve);
		profile->call("_swap_tracks", fog, energy);
		CHECK(profile->get_track_property(energy) == StringName("fog_density"));
		CHECK(profile->get_track_curve(energy) == curve);
		CHECK(profile->get_track_target(fog) == TimeOfDayProfile::TARGET_SUN);

		profile->remove_track(energy);
		CHECK(profile->get_track_count() == 2);
		CHECK(profile->get_track_target(0) == TimeOfDayProfile::TARGET_SUN);
		CHECK(profile->get_track_curve(0).is_null());

		profile->clear_tracks();
		CHECK(profile->get_track_count() == 0);
	}

	SUBCASE("Editing a track's curve changes the profile") {
		Ref<Curve> curve = make_constant(1.0);
		profile->set_track_curve(energy, curve);
		profile->set_track_curve(fog, curve);

		ChangeCounter changes;
		profile->connect_changed(callable_mp(&changes, &ChangeCounter::count));
		curve->set_point_value(0, 0.5);
		CHECK(changes.changes == 1);

		// A curve dropped by one track still reaches the profile through the
		// other track that shares it.
		profile->set_track_curve(fog, Ref<Curve>());
		changes.changes = 0;
		curve->set_point_value(0, 0.25);
		CHECK(changes.changes == 1);

		profile->set_track_curve(energy, Ref<Curve>());
		changes.changes = 0;
		curve->set_point_value(0, 0.75);
		CHECK(changes.changes == 0);
		profile->disconnect_changed(callable_mp(&changes, &ChangeCounter::count));
	}
}

TEST_CASE("[TimeOfDayProfile] The default profile") {
	Ref<TimeOfDayProfile> profile = TimeOfDayProfile::create_default();
	CHECK(profile->get_track_count() > 0);

	const int sun_energy = profile->find_track(TimeOfDayProfile::TARGET_SUN, "light_energy");
	REQUIRE(sun_energy >= 0);
	Variant value;
	CHECK(profile->sample(sun_energy, 12.0, Variant::FLOAT, value));
	CHECK(double(value) == doctest::Approx(1.0));
	CHECK(profile->sample(sun_energy, 0.0, Variant::FLOAT, value));
	CHECK(double(value) == doctest::Approx(0.0));

	// The curves never overshoot their keys, so an energy resting at 0 all
	// night does not go negative on its way up.
	for (int i = 0; i < profile->get_track_count(); i++) {
		const Ref<Curve> curve = profile->get_track_curve(i);
		if (curve.is_null()) {
			CHECK(profile->get_track_gradient(i).is_valid());
			continue;
		}
		for (double hour = 0.0; hour <= 24.0; hour += 0.05) {
			CHECK(profile->sample(i, hour, Variant::FLOAT, value));
			CHECK(double(value) >= 0.0);
		}
	}
}

TEST_CASE("[SceneTree][TimeOfDay] The sun and the moon lights follow the sky") {
	TestScene scene;
	TimeOfDay *time_of_day = scene.time_of_day;

	time_of_day->set_time(9.0);
	scene.update();

	// A directional light shines down its -Z axis, so +Z points at the sun.
	CHECK(scene.sun->get_global_basis().get_column(2).is_equal_approx(time_of_day->get_sun_direction()));
	CHECK(scene.moon->get_global_basis().get_column(2).is_equal_approx(time_of_day->get_moon_direction()));
	CHECK(scene.sun->is_visible());
	// The full moon is below the horizon in the morning.
	CHECK_FALSE(scene.moon->is_visible());
	CHECK(time_of_day->is_daytime());

	time_of_day->set_time(23.0);
	scene.update();
	CHECK_FALSE(scene.sun->is_visible());
	CHECK(scene.moon->is_visible());
	CHECK_FALSE(time_of_day->is_daytime());

	SUBCASE("A light at no energy is hidden, even above the horizon") {
		scene.moon->set_param(Light3D::PARAM_ENERGY, 0.0);
		time_of_day->force_update();
		CHECK_FALSE(scene.moon->is_visible());
	}

	SUBCASE("Unassigning a light gives it back its own rotation and visibility") {
		time_of_day->set_sun_light_path(NodePath());
		scene.update();
		CHECK(scene.sun->get_basis().is_equal_approx(Basis()));
		CHECK(scene.sun->is_visible());
	}
}

TEST_CASE("[SceneTree][TimeOfDay] A profile drives the lights, the environment, the sky and other nodes") {
	TestScene scene;
	TimeOfDay *time_of_day = scene.time_of_day;

	OmniLight3D *lamp = memnew(OmniLight3D);
	lamp->set_name("Lamp");
	scene.root->add_child(lamp);

	Ref<TimeOfDayProfile> profile;
	profile.instantiate();
	int track = profile->add_track(TimeOfDayProfile::TARGET_SUN, "light_energy");
	profile->set_track_curve(track, make_line(0.0, 4.0));
	track = profile->add_track(TimeOfDayProfile::TARGET_SUN, "light_color");
	profile->set_track_gradient(track, make_colors(Color(1, 0, 0), Color(0, 0, 1)));
	track = profile->add_track(TimeOfDayProfile::TARGET_ENVIRONMENT, "fog_density");
	profile->set_track_curve(track, make_line(0.0, 0.24));
	track = profile->add_track(TimeOfDayProfile::TARGET_ENVIRONMENT, "fog_enabled");
	profile->set_track_curve(track, make_line(0.0, 1.0));
	track = profile->add_track(TimeOfDayProfile::TARGET_ENVIRONMENT, "ssr_max_steps");
	profile->set_track_curve(track, make_line(0.0, 96.0));
	track = profile->add_track(TimeOfDayProfile::TARGET_SKY_MATERIAL, "sky_top_color");
	profile->set_track_gradient(track, make_colors(Color(0, 0, 0), Color(0, 1, 0)));
	track = profile->add_track(TimeOfDayProfile::TARGET_NODE, "light_energy", NodePath("../Lamp"));
	profile->set_track_curve(track, make_line(8.0, 0.0));
	time_of_day->set_profile(profile);

	time_of_day->set_time(6.0);
	scene.update();
	CHECK(scene.sun->get_param(Light3D::PARAM_ENERGY) == doctest::Approx(1.0));
	CHECK(scene.sun->get_color().is_equal_approx(Color(0.75, 0, 0.25)));
	CHECK(scene.environment->get_fog_density() == doctest::Approx(0.06));
	CHECK_FALSE(scene.environment->is_fog_enabled());
	CHECK(scene.environment->get_ssr_max_steps() == 24);
	CHECK(scene.sky_material->get_sky_top_color().is_equal_approx(Color(0, 0.25, 0)));
	CHECK(lamp->get_param(Light3D::PARAM_ENERGY) == doctest::Approx(6.0));

	time_of_day->set_time(18.0);
	scene.update();
	CHECK(scene.sun->get_param(Light3D::PARAM_ENERGY) == doctest::Approx(3.0));
	CHECK(scene.environment->is_fog_enabled());
	CHECK(lamp->get_param(Light3D::PARAM_ENERGY) == doctest::Approx(2.0));

	SUBCASE("Editing a curve shows at once") {
		profile->get_track_curve(0)->set_point_value(0, 2.0);
		scene.update();
		CHECK(scene.sun->get_param(Light3D::PARAM_ENERGY) == doctest::Approx(3.5));
	}

	SUBCASE("A removed track hands its property back") {
		const double original_fog = Ref<Environment>(memnew(Environment))->get_fog_density();
		profile->remove_track(profile->find_track(TimeOfDayProfile::TARGET_ENVIRONMENT, "fog_density"));
		scene.update();
		CHECK(scene.environment->get_fog_density() == doctest::Approx(original_fog));
	}

	SUBCASE("A swapped environment is driven instead, and the old one handed back") {
		const Color original_top = Ref<ProceduralSkyMaterial>(memnew(ProceduralSkyMaterial))->get_sky_top_color();
		Ref<Environment> old_environment = scene.environment;
		Ref<ProceduralSkyMaterial> old_sky_material = scene.sky_material;

		Ref<Environment> new_environment;
		new_environment.instantiate();
		scene.world_environment->set_environment(new_environment);
		time_of_day->force_update();
		CHECK(new_environment->get_fog_density() == doctest::Approx(0.18));
		CHECK(old_environment->get_fog_density() == doctest::Approx(Ref<Environment>(memnew(Environment))->get_fog_density()));
		CHECK(old_sky_material->get_sky_top_color().is_equal_approx(original_top));
	}

	SUBCASE("Nested properties reach into sub-resources") {
		Ref<StandardMaterial3D> material;
		material.instantiate();
		MeshInstance3D *mesh = memnew(MeshInstance3D);
		mesh->set_name("Mesh");
		mesh->set_material_override(material);
		scene.root->add_child(mesh);

		track = profile->add_track(TimeOfDayProfile::TARGET_NODE, "material_override:albedo_color", NodePath("../Mesh"));
		profile->set_track_gradient(track, make_colors(Color(0, 0, 0), Color(1, 1, 1)));
		track = profile->add_track(TimeOfDayProfile::TARGET_NODE, "emission_energy_multiplier", NodePath("../Mesh:material_override"));
		profile->set_track_curve(track, make_line(0.0, 8.0));
		scene.update();
		CHECK(material->get_albedo().is_equal_approx(Color(0.75, 0.75, 0.75)));
		CHECK(material->get_emission_energy_multiplier() == doctest::Approx(6.0));
	}
}

TEST_CASE("[SceneTree][TimeOfDay] Weather") {
	TestScene scene;
	TimeOfDay *time_of_day = scene.time_of_day;

	Ref<TimeOfDayProfile> profile;
	profile.instantiate();
	int track = profile->add_track(TimeOfDayProfile::TARGET_ENVIRONMENT, "fog_density");
	profile->set_track_curve(track, make_constant(0.01));
	track = profile->add_track(TimeOfDayProfile::TARGET_ENVIRONMENT, "fog_light_color");
	profile->set_track_gradient(track, make_colors(Color(1, 1, 1), Color(1, 1, 1)));
	time_of_day->set_profile(profile);

	Ref<TimeOfDayProfile> fog;
	fog.instantiate();
	track = fog->add_track(TimeOfDayProfile::TARGET_ENVIRONMENT, "fog_density");
	fog->set_track_curve(track, make_constant(0.05));
	track = fog->add_track(TimeOfDayProfile::TARGET_ENVIRONMENT, "fog_light_color");
	fog->set_track_gradient(track, make_colors(Color(0, 0, 0), Color(0, 0, 0)));
	// Only the weather drives these two.
	track = fog->add_track(TimeOfDayProfile::TARGET_ENVIRONMENT, "volumetric_fog_density");
	fog->set_track_curve(track, make_constant(0.1));
	track = fog->add_track(TimeOfDayProfile::TARGET_ENVIRONMENT, "fog_enabled");
	fog->set_track_curve(track, make_constant(1.0));

	Ref<TimeOfDayProfile> storm;
	storm.instantiate();
	track = storm->add_track(TimeOfDayProfile::TARGET_ENVIRONMENT, "fog_density");
	storm->set_track_curve(track, make_constant(0.09));

	scene.environment->set_volumetric_fog_density(0.02);
	scene.update();
	CHECK(scene.environment->get_fog_density() == doctest::Approx(0.01));

	SUBCASE("It blends over the base by its intensity") {
		time_of_day->set_weather(fog);
		time_of_day->set_weather_intensity(0.5);
		scene.update();
		CHECK(scene.environment->get_fog_density() == doctest::Approx(0.03));
		CHECK(scene.environment->get_fog_light_color().is_equal_approx(Color(0.5, 0.5, 0.5)));
		// Undriven by the base, it blends over its own value.
		CHECK(scene.environment->get_volumetric_fog_density() == doctest::Approx(0.06));
		// A boolean takes the value of whichever weighs more.
		CHECK_FALSE(scene.environment->is_fog_enabled());

		time_of_day->set_weather_intensity(0.75);
		scene.update();
		CHECK(scene.environment->is_fog_enabled());

		time_of_day->set_weather(Ref<TimeOfDayProfile>());
		scene.update();
		CHECK(scene.environment->get_fog_density() == doctest::Approx(0.01));
		CHECK(scene.environment->get_volumetric_fog_density() == doctest::Approx(0.02));
		CHECK_FALSE(scene.environment->is_fog_enabled());
	}

	SUBCASE("It fades from one weather to the next") {
		time_of_day->set_weather(fog);
		scene.update();
		CHECK(scene.environment->get_fog_density() == doctest::Approx(0.05));

		SIGNAL_WATCH(time_of_day, "weather_transition_finished");
		time_of_day->transition_to_weather(storm, 2.0);
		CHECK(time_of_day->is_weather_transitioning());
		CHECK(time_of_day->get_weather() == storm);

		SceneTree::get_singleton()->process(1.0);
		// Halfway through, fog and storm weigh the same, and the base does not
		// show through between them.
		CHECK(scene.environment->get_fog_density() == doctest::Approx(0.07));
		SIGNAL_CHECK_FALSE("weather_transition_finished");

		SceneTree::get_singleton()->process(1.0);
		CHECK(scene.environment->get_fog_density() == doctest::Approx(0.09));
		CHECK_FALSE(time_of_day->is_weather_transitioning());
		SIGNAL_CHECK("weather_transition_finished", once());
		// The fog's own properties went back to their resting values.
		CHECK(scene.environment->get_volumetric_fog_density() == doctest::Approx(0.02));
		SIGNAL_UNWATCH(time_of_day, "weather_transition_finished");
	}

	SUBCASE("Clearing the weather slowly") {
		time_of_day->set_weather(storm);
		scene.update();
		time_of_day->transition_to_weather(Ref<TimeOfDayProfile>(), 4.0);
		SceneTree::get_singleton()->process(2.0);
		CHECK(scene.environment->get_fog_density() == doctest::Approx(0.05));
		SceneTree::get_singleton()->process(2.0);
		CHECK(scene.environment->get_fog_density() == doctest::Approx(0.01));
	}
}

TEST_CASE("[SceneTree][TimeOfDay] Time passes") {
	TestScene scene;
	TimeOfDay *time_of_day = scene.time_of_day;
	// An hour a second.
	time_of_day->set_day_duration(24.0);
	time_of_day->set_time(5.5);

	SIGNAL_WATCH(time_of_day, "sunrise");
	SIGNAL_WATCH(time_of_day, "sunset");
	SIGNAL_WATCH(time_of_day, "day_passed");

	SceneTree::get_singleton()->process(1.0);
	CHECK(time_of_day->get_time() == doctest::Approx(6.5));
	SIGNAL_CHECK("sunrise", once());
	SIGNAL_CHECK_FALSE("sunset");
	// The light moved with the clock.
	CHECK(scene.sun->get_global_basis().get_column(2).is_equal_approx(time_of_day->get_sun_direction_at(6.5)));

	SUBCASE("Skipping ahead still announces what it skips, in order") {
		time_of_day->advance_time(24.0 + 12.0);
		CHECK(time_of_day->get_time() == doctest::Approx(18.5));
		SIGNAL_CHECK("sunset", twice());
		SIGNAL_CHECK("sunrise", once());
		SIGNAL_CHECK("day_passed", once());
	}

	SUBCASE("Paused, it stands still") {
		time_of_day->set_paused(true);
		SceneTree::get_singleton()->process(1.0);
		CHECK(time_of_day->get_time() == doctest::Approx(6.5));
	}

	SUBCASE("Setting the time is a jump, not a passage") {
		time_of_day->set_time(20.0);
		scene.update();
		SIGNAL_CHECK_FALSE("sunset");
		CHECK_FALSE(scene.sun->is_visible());
	}

	SIGNAL_UNWATCH(time_of_day, "sunrise");
	SIGNAL_UNWATCH(time_of_day, "sunset");
	SIGNAL_UNWATCH(time_of_day, "day_passed");
}

TEST_CASE("[SceneTree][TimeOfDay] Keying hand-made changes into the profile") {
	TestScene scene;
	TimeOfDay *time_of_day = scene.time_of_day;

	Ref<TimeOfDayProfile> profile;
	profile.instantiate();
	int track = profile->add_track(TimeOfDayProfile::TARGET_SUN, "light_energy");
	profile->set_track_curve(track, make_constant(1.0));
	track = profile->add_track(TimeOfDayProfile::TARGET_SKY_MATERIAL, "sky_horizon_color");
	profile->set_track_gradient(track, make_colors(Color(1, 1, 1), Color(1, 1, 1)));
	track = profile->add_track(TimeOfDayProfile::TARGET_ENVIRONMENT, "fog_density");
	profile->set_track_curve(track, make_constant(0.01));
	time_of_day->set_profile(profile);
	time_of_day->set_time(9.0);
	scene.update();

	// Tweaked by hand, as in the Inspector.
	scene.sun->set_param(Light3D::PARAM_ENERGY, 0.3);
	scene.sky_material->set_sky_horizon_color(Color(1, 0.5, 0.25));

	CHECK(time_of_day->key_changed_values() == 2);
	Variant value;
	CHECK(profile->sample(0, 9.0, Variant::FLOAT, value));
	CHECK(double(value) == doctest::Approx(0.3));
	CHECK(profile->sample(1, 9.0, Variant::COLOR, value));
	CHECK(Color(value).is_equal_approx(Color(1, 0.5, 0.25)));
	// The keys already there are left as they were.
	CHECK(profile->sample(0, 0.0, Variant::FLOAT, value));
	CHECK(double(value) == doctest::Approx(1.0));
	CHECK(profile->sample(0, 24.0, Variant::FLOAT, value));
	CHECK(double(value) == doctest::Approx(1.0));
	// Nothing that was left alone gets a key.
	CHECK(profile->get_track_curve(2)->get_point_count() == 2);

	// Keying again at the same time edits that key rather than adding another.
	scene.sun->set_param(Light3D::PARAM_ENERGY, 0.4);
	CHECK(time_of_day->key_changed_values() == 1);
	CHECK(profile->get_track_curve(0)->get_point_count() == 3);
	CHECK(profile->sample(0, 9.0, Variant::FLOAT, value));
	CHECK(double(value) == doctest::Approx(0.4));

	// Values beyond the curve's range widen it instead of being clamped.
	scene.sun->set_param(Light3D::PARAM_ENERGY, 7.5);
	time_of_day->key_changed_values();
	CHECK(profile->sample(0, 9.0, Variant::FLOAT, value));
	CHECK(double(value) == doctest::Approx(7.5));
}

TEST_CASE("[Editor][TimeOfDay] The saved scene never contains the cycle") {
	TestScene scene;
	TimeOfDay *time_of_day = scene.time_of_day;
	const float original_energy = scene.sun->get_param(Light3D::PARAM_ENERGY);
	// The sun was made unrotated; TimeOfDay has turned it since.
	const Basis original_basis;
	const double original_fog = scene.environment->get_fog_density();

	Ref<TimeOfDayProfile> profile;
	profile.instantiate();
	int track = profile->add_track(TimeOfDayProfile::TARGET_SUN, "light_energy");
	profile->set_track_curve(track, make_constant(0.25));
	track = profile->add_track(TimeOfDayProfile::TARGET_ENVIRONMENT, "fog_density");
	profile->set_track_curve(track, make_constant(0.2));
	time_of_day->set_profile(profile);
	time_of_day->set_time(10.0);
	scene.update();
	CHECK(scene.sun->get_param(Light3D::PARAM_ENERGY) == doctest::Approx(0.25));

	scene.root->propagate_notification(Node::NOTIFICATION_EDITOR_PRE_SAVE);
	CHECK(scene.sun->get_param(Light3D::PARAM_ENERGY) == doctest::Approx(original_energy));
	CHECK(scene.sun->get_basis().is_equal_approx(original_basis));
	CHECK(scene.environment->get_fog_density() == doctest::Approx(original_fog));

	scene.root->propagate_notification(Node::NOTIFICATION_EDITOR_POST_SAVE);
	CHECK(scene.sun->get_param(Light3D::PARAM_ENERGY) == doctest::Approx(0.25));
	CHECK(scene.environment->get_fog_density() == doctest::Approx(0.2));

	SUBCASE("Time doesn't pass in the editor unless asked to") {
		time_of_day->set_day_duration(24.0);
		SceneTree::get_singleton()->process(1.0);
		CHECK(time_of_day->get_time() == doctest::Approx(10.0));

		time_of_day->set_run_in_editor(true);
		SceneTree::get_singleton()->process(1.0);
		CHECK(time_of_day->get_time() == doctest::Approx(11.0));
	}

	SUBCASE("Removing the node from the scene hands everything back") {
		scene.root->remove_child(time_of_day);
		CHECK(scene.sun->get_param(Light3D::PARAM_ENERGY) == doctest::Approx(original_energy));
		CHECK(scene.environment->get_fog_density() == doctest::Approx(original_fog));
		memdelete(time_of_day);
	}

	SUBCASE("Picking a property gives the track a curve to start from") {
		track = profile->add_track(TimeOfDayProfile::TARGET_SUN, StringName());
		profile->set("tracks/" + itos(track) + "/property", "shadow_opacity");
		const Ref<Curve> curve = profile->get_track_curve(track);
		REQUIRE(curve.is_valid());
		CHECK(curve->get_max_domain() == doctest::Approx(24.0));
		CHECK(curve->sample(12.0) == doctest::Approx(scene.sun->get_param(Light3D::PARAM_SHADOW_OPACITY)));

		track = profile->add_track(TimeOfDayProfile::TARGET_SKY_MATERIAL, StringName());
		profile->set("tracks/" + itos(track) + "/property", "sky_horizon_color");
		CHECK(profile->get_track_gradient(track).is_valid());
		CHECK(profile->get_track_curve(track).is_null());
	}
}

} // namespace TestTimeOfDay
