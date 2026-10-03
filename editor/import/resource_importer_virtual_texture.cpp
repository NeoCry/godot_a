/**************************************************************************/
/*  resource_importer_virtual_texture.cpp                                 */
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

#include "resource_importer_virtual_texture.h"

#include "core/io/image_loader.h"
#include "core/math/math_funcs_binary.h"
#include "scene/resources/virtual_texture_2d.h"
#include "servers/rendering/rendering_server_enums.h"

String ResourceImporterVirtualTexture::get_importer_name() const {
	return IMPORTER_NAME;
}

String ResourceImporterVirtualTexture::get_visible_name() const {
	return "VirtualTexture2D";
}

void ResourceImporterVirtualTexture::get_recognized_extensions(List<String> *p_extensions) const {
	ImageLoader::get_recognized_extensions(p_extensions);
}

String ResourceImporterVirtualTexture::get_save_extension() const {
	return "vtex";
}

String ResourceImporterVirtualTexture::get_resource_type() const {
	return "VirtualTexture2D";
}

int ResourceImporterVirtualTexture::get_preset_count() const {
	return 0;
}

String ResourceImporterVirtualTexture::get_preset_name(int p_idx) const {
	return String();
}

void ResourceImporterVirtualTexture::get_import_options(const String &p_path, List<ImportOption> *r_options, int p_preset) const {
	r_options->push_back(ImportOption(PropertyInfo(Variant::INT, "compress/mode", PROPERTY_HINT_ENUM, "Lossless,Lossy,Uncompressed"), VirtualTexture2D::PAGE_FORMAT_LOSSLESS));
	r_options->push_back(ImportOption(PropertyInfo(Variant::FLOAT, "compress/lossy_quality", PROPERTY_HINT_RANGE, "0,1,0.01"), 0.8));
	r_options->push_back(ImportOption(PropertyInfo(Variant::BOOL, "process/normal_map"), false));
	r_options->push_back(ImportOption(PropertyInfo(Variant::BOOL, "process/repeat"), true));
	r_options->push_back(ImportOption(PropertyInfo(Variant::INT, "process/size_limit", PROPERTY_HINT_RANGE, "0,131072,1"), 0));
}

bool ResourceImporterVirtualTexture::get_option_visibility(const String &p_path, const String &p_option, const HashMap<StringName, Variant> &p_options) const {
	if (p_option == "compress/lossy_quality") {
		return int(p_options["compress/mode"]) == VirtualTexture2D::PAGE_FORMAT_LOSSY;
	}
	return true;
}

Size2i ResourceImporterVirtualTexture::get_virtual_size(const Size2i &p_size, int p_size_limit) {
	// Pages are square and a virtual texture's mipmaps halve whole pages, so both sides are rounded to
	// the nearest power of two, and kept within what a page table can address.
	const int min_size = RSE::VIRTUAL_TEXTURE_PAGE_SIZE;
	int max_size = RSE::VIRTUAL_TEXTURE_PAGE_SIZE * 1024;
	if (p_size_limit > 0) {
		max_size = MIN(max_size, MAX(min_size, int(Math::previous_power_of_2(uint32_t(p_size_limit)))));
	}
	Size2i size;
	size.x = CLAMP(int(Math::closest_power_of_2(uint32_t(MAX(p_size.x, 1)))), min_size, max_size);
	size.y = CLAMP(int(Math::closest_power_of_2(uint32_t(MAX(p_size.y, 1)))), min_size, max_size);
	return size;
}

Error ResourceImporterVirtualTexture::import(ResourceUID::ID p_source_id, const String &p_source_file, const String &p_save_path, const HashMap<StringName, Variant> &p_options, List<String> *r_platform_variants, List<String> *r_gen_files, Variant *r_metadata) {
	Ref<Image> image;
	image.instantiate();
	Error err = ImageLoader::load_image(p_source_file, image);
	ERR_FAIL_COND_V_MSG(err != OK || image.is_null() || image->is_empty(), err != OK ? err : ERR_FILE_CORRUPT, vformat("Could not load the image '%s'.", p_source_file));

	if (image->is_compressed()) {
		image->decompress();
	}
	image->clear_mipmaps();
	if (image->get_format() != Image::FORMAT_RGBA8) {
		image->convert(Image::FORMAT_RGBA8);
	}

	const Size2i size = get_virtual_size(image->get_size(), p_options["process/size_limit"]);
	if (size != image->get_size()) {
		image->resize(size.x, size.y, Image::INTERPOLATE_LANCZOS);
	}

	const VirtualTexture2D::PageFormat page_format = VirtualTexture2D::PageFormat(int(p_options["compress/mode"]));
	return VirtualTexture2D::save_to_file(p_save_path + ".vtex", image, page_format, p_options["compress/lossy_quality"], p_options["process/repeat"], p_options["process/normal_map"]);
}
