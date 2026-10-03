/**************************************************************************/
/*  test_blend_landscape_3d.cpp                                           */
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

TEST_FORCE_LINK(test_blend_landscape_3d)

#include "scene/3d/blend_landscape_3d.h"
#include "scene/3d/landscape_3d.h"
#include "scene/3d/terrain_data.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/image_texture.h"
#include "tests/test_tools.h"

namespace TestBlendLandscape3D {

static Ref<ImageTexture> make_texture() {
	Ref<Image> image = Image::create_empty(4, 4, false, Image::FORMAT_RGBA8);
	image->fill(Color(0.5, 0.5, 0.5));
	return ImageTexture::create_from_image(image);
}

TEST_CASE("[SceneTree][BlendLandscape3D] The shader compiles with whatever the material uses") {
	Ref<BlendLandscape3D> material;
	material.instantiate();

	SUBCASE("Plain") {
	}
	SUBCASE("Normal map") {
		material->set_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING, true);
		material->set_texture(BaseMaterial3D::TEXTURE_NORMAL, make_texture());
	}
	SUBCASE("Noise") {
		material->set_blend_noise_texture(make_texture());
	}
	SUBCASE("Transparent and unshaded, with a normal map and noise") {
		material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
		material->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
		material->set_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING, true);
		material->set_blend_noise_texture(make_texture());
	}
	SUBCASE("Triplanar") {
		material->set_flag(BaseMaterial3D::FLAG_UV1_USE_TRIPLANAR, true);
		material->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, make_texture());
	}

	ErrorDetector errors;
	CHECK(material->get_shader_rid().is_valid());
	CHECK_FALSE(errors.has_error);

	// Its own shader, not the one a StandardMaterial3D with the same settings gets.
	Ref<StandardMaterial3D> standard;
	standard.instantiate();
	CHECK(standard->get_shader_rid() != material->get_shader_rid());
}

TEST_CASE("[SceneTree][BlendLandscape3D] Materials blend into the landscape drawing into their layers") {
	const ObjectID first = ObjectID(uint64_t(0x7fff0001));
	const ObjectID second = ObjectID(uint64_t(0x7fff0002));
	BlendLandscape3D::LandscapeSource source;
	source.parameters[SNAME("landscape_vertex_spacing")] = 2.0;
	source.layers = 2;

	Ref<BlendLandscape3D> material;
	material.instantiate();
	CHECK(material->get_blend_landscape_layers() == 1u);
	CHECK(material->get_bound_landscape().is_null());

	BlendLandscape3D::set_landscape_source(first, source);
	CHECK_MESSAGE(material->get_bound_landscape().is_null(), "Not in the material's layers.");
	material->set_blend_landscape_layers(3);
	CHECK(material->get_bound_landscape() == first);

	// The first landscape to appear stays the one blended into.
	source.layers = 1;
	source.parameters[SNAME("landscape_vertex_spacing")] = 4.0;
	BlendLandscape3D::set_landscape_source(second, source);
	CHECK(material->get_bound_landscape() == first);

	// A material made now binds right away.
	Ref<BlendLandscape3D> later;
	later.instantiate();
	CHECK(later->get_bound_landscape() == second);

	BlendLandscape3D::remove_landscape_source(first);
	CHECK(material->get_bound_landscape() == second);
	BlendLandscape3D::remove_landscape_source(second);
	CHECK(material->get_bound_landscape().is_null());
	CHECK(later->get_bound_landscape().is_null());
}

TEST_CASE("[SceneTree][BlendLandscape3D] A landscape in the tree is blended into, virtual texture or not") {
	Ref<TerrainData> data;
	data.instantiate();
	data->set_resolution(33);
	Ref<BlendLandscape3D> material;
	material.instantiate();

	Landscape3D *landscape = memnew(Landscape3D);
	landscape->set_terrain_data(data);
	CHECK_MESSAGE(material->get_bound_landscape().is_null(), "Not in the tree yet.");

	// The dummy renderer has no virtual texturing: the material blends the landscape's layers itself.
	SceneTree::get_singleton()->get_root()->add_child(landscape);
	CHECK(material->get_bound_landscape() == landscape->get_instance_id());

	landscape->set_virtual_texture_layers(2);
	CHECK(material->get_bound_landscape().is_null());
	material->set_blend_landscape_layers(2);
	CHECK(material->get_bound_landscape() == landscape->get_instance_id());

	SceneTree::get_singleton()->get_root()->remove_child(landscape);
	CHECK(material->get_bound_landscape().is_null());
	memdelete(landscape);
}

TEST_CASE("[SceneTree][BlendLandscape3D] Settings are kept in range") {
	Ref<BlendLandscape3D> material;
	material.instantiate();
	material->set_blend_height(-1.0);
	CHECK(material->get_blend_height() == 0.0f);
	material->set_blend_falloff(2.0);
	CHECK(material->get_blend_falloff() == 1.0f);
	material->set_blend_normal_strength(-0.5);
	CHECK(material->get_blend_normal_strength() == 0.0f);
	material->set_blend_slope(0.25);
	CHECK(material->get_blend_slope() == doctest::Approx(0.25f));
	material->set_blend_offset(-0.5);
	CHECK(material->get_blend_offset() == doctest::Approx(-0.5f));
	material->set_blend_noise_scale(0.0);
	CHECK(material->get_blend_noise_scale() > 0.0f);
}

} // namespace TestBlendLandscape3D
