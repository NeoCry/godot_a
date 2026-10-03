/**************************************************************************/
/*  test_virtual_texture_2d.cpp                                           */
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

TEST_FORCE_LINK(test_virtual_texture_2d)

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "scene/resources/virtual_texture_2d.h"
#include "servers/rendering/shader_language.h"
#include "servers/rendering/shader_types.h"
#include "tests/test_utils.h"

#ifdef TOOLS_ENABLED
#include "editor/import/resource_importer_virtual_texture.h"
#endif

namespace TestVirtualTexture2D {

constexpr int PAGE_SIZE = RSE::VIRTUAL_TEXTURE_PAGE_SIZE;
constexpr int PAGE_BORDER = RSE::VIRTUAL_TEXTURE_PAGE_BORDER;
constexpr int TILE_SIZE = PAGE_SIZE + PAGE_BORDER * 2;

// Every texel tells where it is: red is its column, green its row (both below 256).
static Ref<Image> make_gradient_image(int p_width, int p_height, uint8_t p_alpha = 255) {
	Ref<Image> image = Image::create_empty(p_width, p_height, false, Image::FORMAT_RGBA8);
	for (int y = 0; y < p_height; y++) {
		for (int x = 0; x < p_width; x++) {
			image->set_pixel(x, y, Color::from_rgba8(x & 255, y & 255, 128, p_alpha));
		}
	}
	return image;
}

static bool texels_equal(const Ref<Image> &p_a, const Point2i &p_at_a, const Ref<Image> &p_b, const Point2i &p_at_b) {
	return p_a->get_pixelv(p_at_a).to_rgba32() == p_b->get_pixelv(p_at_b).to_rgba32();
}

static Ref<Image> read_page(const String &p_path, int p_mipmap, int p_x, int p_y, VirtualTexture2D::Header *r_header = nullptr) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
	REQUIRE(f.is_valid());
	VirtualTexture2D::Header header;
	REQUIRE(VirtualTexture2D::read_header(f, header) == OK);
	const uint32_t index = header.get_page_index(p_mipmap, p_x, p_y);
	REQUIRE(index < header.page_offsets.size());
	Vector<uint8_t> data;
	data.resize(header.page_sizes[index]);
	f->seek(header.page_offsets[index]);
	REQUIRE(f->get_buffer(data.ptrw(), data.size()) == uint64_t(data.size()));
	if (r_header) {
		*r_header = header;
	}
	return VirtualTexture2D::decode_page(data, header.page_format);
}

TEST_CASE("[VirtualTexture2D] Pages are numbered finest mipmap first, row by row") {
	VirtualTexture2D::Header header;
	header.width = PAGE_SIZE * 4;
	header.height = PAGE_SIZE * 2;
	header.mipmaps = 2;

	CHECK(header.get_page_index(0, 0, 0) == 0u);
	CHECK(header.get_page_index(0, 3, 0) == 3u);
	CHECK(header.get_page_index(0, 1, 1) == 5u);
	CHECK(header.get_page_index(1, 0, 0) == 8u);
	CHECK(header.get_page_index(1, 1, 0) == 9u);

	CHECK(header.get_page_index(0, 4, 0) == UINT32_MAX);
	CHECK(header.get_page_index(1, 0, 1) == UINT32_MAX);
	CHECK(header.get_page_index(2, 0, 0) == UINT32_MAX);
	CHECK(header.get_page_index(-1, 0, 0) == UINT32_MAX);
}

TEST_CASE("[VirtualTexture2D] Pages hold the texture, its borders and its mipmaps") {
	const String path = TestUtils::get_temp_path("virtual_texture_pages.vtex");
	const Ref<Image> source = make_gradient_image(PAGE_SIZE * 2, PAGE_SIZE * 4);

	SUBCASE("Repeating") {
		REQUIRE(VirtualTexture2D::save_to_file(path, source, VirtualTexture2D::PAGE_FORMAT_UNCOMPRESSED) == OK);

		VirtualTexture2D::Header header;
		const Ref<Image> page = read_page(path, 0, 1, 0, &header);
		CHECK(header.width == uint32_t(PAGE_SIZE * 2));
		CHECK(header.height == uint32_t(PAGE_SIZE * 4));
		// Down to one page across, the narrower side.
		CHECK(header.mipmaps == 2u);
		CHECK(header.page_offsets.size() == 2u * 4u + 1u * 2u);
		CHECK(header.page_format == VirtualTexture2D::PAGE_FORMAT_UNCOMPRESSED);
		CHECK((header.flags & VirtualTexture2D::FLAG_HAS_ALPHA) == 0);

		REQUIRE(page.is_valid());
		CHECK(page->get_size() == Size2i(TILE_SIZE, TILE_SIZE));
		// The page itself.
		CHECK(texels_equal(page, Point2i(PAGE_BORDER, PAGE_BORDER), source, Point2i(PAGE_SIZE, 0)));
		CHECK(texels_equal(page, Point2i(PAGE_BORDER + PAGE_SIZE - 1, PAGE_BORDER + 17), source, Point2i(PAGE_SIZE * 2 - 1, 17)));
		// Its borders come from the opposite edges of the texture, as a repeating texture samples them.
		CHECK(texels_equal(page, Point2i(0, 0), source, Point2i(PAGE_SIZE - PAGE_BORDER, PAGE_SIZE * 4 - PAGE_BORDER)));
		CHECK(texels_equal(page, Point2i(TILE_SIZE - 1, PAGE_BORDER), source, Point2i(PAGE_BORDER - 1, 0)));
		// And from the neighboring page inside it.
		CHECK(texels_equal(page, Point2i(0, PAGE_BORDER + 5), source, Point2i(PAGE_SIZE - PAGE_BORDER, 5)));

		// The coarser mipmap averages the texels it covers.
		const Ref<Image> coarse = read_page(path, 1, 0, 1);
		REQUIRE(coarse.is_valid());
		for (const Point2i &texel : { Point2i(0, 0), Point2i(37, 90), Point2i(PAGE_SIZE - 1, PAGE_SIZE - 1) }) {
			const Color c = coarse->get_pixelv(texel + Point2i(PAGE_BORDER, PAGE_BORDER));
			const Point2i at(texel.x * 2, (texel.y + PAGE_SIZE) * 2);
			CHECK(Math::abs(c.get_r8() - (at.x & 255)) <= 1);
			CHECK(Math::abs(c.get_g8() - (at.y & 255)) <= 1);
		}
	}

	SUBCASE("Clamped") {
		REQUIRE(VirtualTexture2D::save_to_file(path, source, VirtualTexture2D::PAGE_FORMAT_UNCOMPRESSED, 0.8, false) == OK);
		const Ref<Image> page = read_page(path, 0, 1, 0);
		REQUIRE(page.is_valid());
		// Off the edges of the texture, the edge texels repeat.
		CHECK(texels_equal(page, Point2i(0, 0), source, Point2i(PAGE_SIZE - PAGE_BORDER, 0)));
		CHECK(texels_equal(page, Point2i(TILE_SIZE - 1, TILE_SIZE - 1), source, Point2i(PAGE_SIZE * 2 - 1, PAGE_SIZE + PAGE_BORDER - 1)));
	}

	SUBCASE("Lossless") {
		if (Image::webp_unpacker && Image::webp_lossless_packer) {
			REQUIRE(VirtualTexture2D::save_to_file(path, source, VirtualTexture2D::PAGE_FORMAT_LOSSLESS) == OK);
			VirtualTexture2D::Header header;
			const Ref<Image> page = read_page(path, 0, 0, 2, &header);
			CHECK(header.page_format == VirtualTexture2D::PAGE_FORMAT_LOSSLESS);
			REQUIRE(page.is_valid());
			for (int y = 0; y < TILE_SIZE; y += 7) {
				for (int x = 0; x < TILE_SIZE; x += 5) {
					CHECK(texels_equal(page, Point2i(x, y), source, Point2i(Math::posmod(x - PAGE_BORDER, PAGE_SIZE * 2), PAGE_SIZE * 2 - PAGE_BORDER + y)));
				}
			}
		}
	}

	DirAccess::remove_absolute(path);
}

TEST_CASE("[VirtualTexture2D] Only textures a power of two of pages a side are written") {
	const String path = TestUtils::get_temp_path("virtual_texture_invalid.vtex");
	ERR_PRINT_OFF;
	CHECK(VirtualTexture2D::save_to_file(path, make_gradient_image(PAGE_SIZE * 3, PAGE_SIZE), VirtualTexture2D::PAGE_FORMAT_UNCOMPRESSED) == ERR_INVALID_PARAMETER);
	CHECK(VirtualTexture2D::save_to_file(path, make_gradient_image(PAGE_SIZE / 2, PAGE_SIZE / 2), VirtualTexture2D::PAGE_FORMAT_UNCOMPRESSED) == ERR_INVALID_PARAMETER);
	CHECK(VirtualTexture2D::save_to_file(path, Ref<Image>(), VirtualTexture2D::PAGE_FORMAT_UNCOMPRESSED) == ERR_INVALID_PARAMETER);
	ERR_PRINT_ON;
	CHECK_FALSE(FileAccess::exists(path));
}

TEST_CASE("[SceneTree][VirtualTexture2D] A loaded texture streams its pages") {
	const String path = TestUtils::get_temp_path("virtual_texture_load.vtex");
	const Ref<Image> source = make_gradient_image(PAGE_SIZE * 4, PAGE_SIZE * 2, 200);
	REQUIRE(VirtualTexture2D::save_to_file(path, source, VirtualTexture2D::PAGE_FORMAT_UNCOMPRESSED) == OK);

	Ref<VirtualTexture2D> texture;
	texture.instantiate();
	REQUIRE(texture->load(path) == OK);
	CHECK(texture->get_load_path() == path);
	CHECK(texture->get_width() == PAGE_SIZE * 4);
	CHECK(texture->get_height() == PAGE_SIZE * 2);
	CHECK(texture->has_alpha());
	CHECK(texture->get_rid().is_valid());
	CHECK(texture->get_page_format() == VirtualTexture2D::PAGE_FORMAT_UNCOMPRESSED);
	CHECK(texture->get_page_mipmap_count() == 2);
	CHECK(texture->get_page_count(0) == Vector2i(4, 2));
	CHECK(texture->get_page_count(1) == Vector2i(2, 1));

	// The fallback is the whole texture, small.
	const Ref<Image> fallback = texture->get_fallback_image();
	REQUIRE(fallback.is_valid());
	CHECK(fallback->get_width() == VirtualTexture2D::FALLBACK_SIZE);
	CHECK(fallback->get_height() == VirtualTexture2D::FALLBACK_SIZE / 2);

	const Ref<Image> page = texture->get_page_image(1, 1, 0);
	REQUIRE(page.is_valid());
	CHECK(page->get_size() == Size2i(TILE_SIZE, TILE_SIZE));
	ERR_PRINT_OFF;
	CHECK(texture->get_page_image(1, 0, 1).is_null());
	ERR_PRINT_ON;

	// Put back together, the pages are the texture.
	const Ref<Image> image = texture->get_image();
	REQUIRE(image.is_valid());
	REQUIRE(image->get_size() == source->get_size());
	CHECK(image->get_data() == source->get_data());

	texture.unref();
	DirAccess::remove_absolute(path);
}

#ifdef TOOLS_ENABLED
TEST_CASE("[VirtualTexture2D] Imported textures are resized to powers of two") {
	CHECK(ResourceImporterVirtualTexture::get_virtual_size(Size2i(1000, 300), 0) == Size2i(1024, 256));
	CHECK(ResourceImporterVirtualTexture::get_virtual_size(Size2i(4096, 4096), 0) == Size2i(4096, 4096));
	// At least a page a side.
	CHECK(ResourceImporterVirtualTexture::get_virtual_size(Size2i(50, 2048), 0) == Size2i(PAGE_SIZE, 2048));
	// Within what a page table addresses, and the size limit.
	CHECK(ResourceImporterVirtualTexture::get_virtual_size(Size2i(400000, 512), 0) == Size2i(PAGE_SIZE * 1024, 512));
	CHECK(ResourceImporterVirtualTexture::get_virtual_size(Size2i(8192, 3000), 2048) == Size2i(2048, 2048));
	CHECK(ResourceImporterVirtualTexture::get_virtual_size(Size2i(8192, 3000), 3000) == Size2i(2048, 2048));
}
#endif // TOOLS_ENABLED

static Error compile_shader(const String &p_code, RSE::ShaderMode p_mode = RSE::SHADER_SPATIAL, String *r_error = nullptr) {
	ShaderLanguage::ShaderCompileInfo info;
	info.functions = ShaderTypes::get_singleton()->get_functions(p_mode);
	info.render_modes = ShaderTypes::get_singleton()->get_modes(p_mode);
	info.stencil_modes = ShaderTypes::get_singleton()->get_stencil_modes(p_mode);
	info.shader_types = ShaderTypes::get_singleton()->get_types();

	ShaderLanguage parser;
	const Error err = parser.compile(p_code, info);
	if (r_error) {
		*r_error = parser.get_error_text();
	}
	return err;
}

TEST_CASE("[SceneTree][VirtualTexture2D] Shaders sample virtual textures with the texture functions") {
	String error;
	CHECK_MESSAGE(compile_shader(R"(
shader_type spatial;
uniform sampler2D albedo_vt : source_color, hint_virtual_texture;
uniform sampler2DArray layers_vt : hint_virtual_texture, repeat_enable;
uniform sampler2D plain;

vec4 sample_it(sampler2D tex, vec2 uv) {
	return texture(tex, uv);
}

void vertex() {
	VERTEX.y += texture(albedo_vt, UV).r;
}

void fragment() {
	ALBEDO = sample_it(albedo_vt, UV).rgb;
	ALBEDO += textureLod(albedo_vt, UV, 1.0).rgb + textureGrad(albedo_vt, UV, vec2(0.01), vec2(0.01)).rgb;
	ALBEDO += texture(albedo_vt, UV, 0.5).rgb + texture(layers_vt, vec3(UV, 1.0)).rgb * float(textureSize(albedo_vt, 0).x);
	ALBEDO += texelFetch(plain, ivec2(0), 0).rgb;
}
)",
						  RSE::SHADER_SPATIAL, &error) == OK,
			error);

	const char *unsupported[] = {
		"ALBEDO = texelFetch(albedo_vt, ivec2(0), 0).rgb;",
		"ALBEDO = textureGather(albedo_vt, UV).rgb;",
		"ALBEDO = vec3(textureQueryLod(albedo_vt, UV), 0.0);",
		"ALBEDO = vec3(float(textureQueryLevels(albedo_vt)));",
		"ALBEDO = textureProj(albedo_vt, vec3(UV, 1.0)).rgb;",
	};
	for (const char *line : unsupported) {
		CHECK_MESSAGE(compile_shader(vformat("shader_type spatial;\nuniform sampler2D albedo_vt : hint_virtual_texture;\nvoid fragment() {\n%s\n}\n", line)) == ERR_PARSE_ERROR, line);
	}
}

TEST_CASE("[SceneTree][VirtualTexture2D] Where virtual textures can be declared") {
	// Spatial shaders' 2D textures and texture arrays only.
	CHECK(compile_shader("shader_type spatial;\nuniform sampler3D v : hint_virtual_texture;\n") == ERR_PARSE_ERROR);
	CHECK(compile_shader("shader_type spatial;\nuniform samplerCube v : hint_virtual_texture;\n") == ERR_PARSE_ERROR);
	CHECK(compile_shader("shader_type spatial;\nuniform sampler2D v[2] : hint_virtual_texture;\n") == ERR_PARSE_ERROR);
	CHECK(compile_shader("shader_type spatial;\ninstance uniform sampler2D v : hint_virtual_texture;\n") == ERR_PARSE_ERROR);
	CHECK(compile_shader("shader_type spatial;\nuniform sampler2D v : hint_virtual_texture, hint_virtual_texture;\n") == ERR_PARSE_ERROR);
	CHECK(compile_shader("shader_type canvas_item;\nuniform sampler2D v : hint_virtual_texture;\n", RSE::SHADER_CANVAS_ITEM) == ERR_PARSE_ERROR);
	CHECK(compile_shader("shader_type spatial;\nuniform sampler2D v : hint_virtual_texture, filter_nearest;\n") == OK);
}

TEST_CASE("[SceneTree][VirtualTexture2D] Functions take either virtual textures or textures that are not") {
	// A sampler argument is a page table or a texture, not both.
	CHECK(compile_shader(R"(
shader_type spatial;
uniform sampler2D vt : hint_virtual_texture;
uniform sampler2D plain;
vec4 sample_it(sampler2D tex) {
	return texture(tex, vec2(0.5));
}
void fragment() {
	ALBEDO = sample_it(vt).rgb + sample_it(plain).rgb;
}
)") == ERR_PARSE_ERROR);

	// Nor can a function given a virtual texture read it in a way virtual textures can't be, even
	// through another function.
	CHECK(compile_shader(R"(
shader_type spatial;
uniform sampler2D vt : hint_virtual_texture;
vec4 fetch(sampler2D tex) {
	return texelFetch(tex, ivec2(0), 0);
}
vec4 forward(sampler2D tex) {
	return fetch(tex);
}
void fragment() {
	ALBEDO = forward(vt).rgb;
}
)") == ERR_PARSE_ERROR);

	// It can for textures that are not virtual.
	CHECK(compile_shader(R"(
shader_type spatial;
uniform sampler2D plain;
vec4 fetch(sampler2D tex) {
	return texelFetch(tex, ivec2(0), 0);
}
void fragment() {
	ALBEDO = fetch(plain).rgb;
}
)") == OK);
}

} // namespace TestVirtualTexture2D
