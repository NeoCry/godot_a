// Sampling virtual textures (see VirtualTextureStorage).
//
// A uniform declared with hint_virtual_texture is bound to its texture's page table: one texel per
// page, at every mipmap, each naming the tile of the page cache that holds the best page there is for
// that spot. The shader compiler turns texture(), textureLod(), textureGrad() and textureSize() on
// such a uniform into the vt_texture*() functions below.
//
// The including shader declares the caches (vt_cache and vt_cache_srgb, one texture2DArray per cache,
// and vt_cache_height, the runtime cache's heights) and the feedback buffer (vt_feedback), and defines
// VT_STAGE_FRAGMENT in its fragment stage and VT_FEEDBACK where sampling should also report the pages
// it wanted.

#define VT_PAGE_SIZE 128.0
#define VT_PAGE_BORDER 4.0
#define VT_TILE_SIZE 136.0
#define VT_TILE_MIPMAPS 3.0
// Tiles keep 4 texels of their neighbors around them, which anisotropic filtering stays within as
// long as it never stretches a footprint more than this many times longer than it is wide.
#define VT_MAX_ANISOTROPY 4.0
#define VT_FEEDBACK_SLOTS 8192u
#define VT_FEEDBACK_SLOT_BITS 13u

// Layer 3 of a runtime virtual texture is the height of the ground, kept in a cache of its own.
#define VT_RUNTIME_HEIGHT_LAYER 3.0

#define VT_FLAG_SRGB 1u
#define VT_FLAG_REPEAT 2u

// Page table entries: tile X (8 bits), tile Y (8), the mipmap of the page it holds (4, 15 while
// nothing is resident), the cache (1) and the texture's ID (11).
#define VT_ENTRY_EMPTY_MIP 15u

vec2 vt_wrap(vec2 p_uv, uint p_flags) {
	return (p_flags & VT_FLAG_REPEAT) != 0u ? fract(p_uv) : clamp(p_uv, vec2(0.0), vec2(0.99999994));
}

vec2 vt_cache_size(uint p_cache) {
	if (p_cache == 0u) {
		return vec2(textureSize(sampler2DArray(vt_cache[0], SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_CLAMP), 0).xy);
	}
	return vec2(textureSize(sampler2DArray(vt_cache[1], SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_CLAMP), 0).xy);
}

int vt_cache_layers(uint p_cache) {
	if (p_cache == 0u) {
		return textureSize(sampler2DArray(vt_cache[0], SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_CLAMP), 0).z;
	}
	return textureSize(sampler2DArray(vt_cache[1], SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_CLAMP), 0).z;
}

// Every cache and view is a binding of its own: branching rather than indexing keeps each lookup's
// descriptor uniform.
vec4 vt_cache_grad(uint p_cache, bool p_srgb, vec3 p_coord, vec2 p_dx, vec2 p_dy) {
	if (p_cache == 0u) {
		if (p_srgb) {
			return textureGrad(sampler2DArray(vt_cache_srgb[0], SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_CLAMP), p_coord, p_dx, p_dy);
		}
		return textureGrad(sampler2DArray(vt_cache[0], SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_CLAMP), p_coord, p_dx, p_dy);
	}
	if (p_coord.z > VT_RUNTIME_HEIGHT_LAYER - 0.5) {
		return vec4(textureGrad(sampler2DArray(vt_cache_height, SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_CLAMP), vec3(p_coord.xy, 0.0), p_dx, p_dy).r, 0.0, 0.0, 1.0);
	}
	if (p_srgb) {
		return textureGrad(sampler2DArray(vt_cache_srgb[1], SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_CLAMP), p_coord, p_dx, p_dy);
	}
	return textureGrad(sampler2DArray(vt_cache[1], SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_CLAMP), p_coord, p_dx, p_dy);
}

vec4 vt_cache_lod(uint p_cache, bool p_srgb, vec3 p_coord, float p_lod) {
	if (p_cache == 0u) {
		if (p_srgb) {
			return textureLod(sampler2DArray(vt_cache_srgb[0], SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_CLAMP), p_coord, p_lod);
		}
		return textureLod(sampler2DArray(vt_cache[0], SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_CLAMP), p_coord, p_lod);
	}
	if (p_coord.z > VT_RUNTIME_HEIGHT_LAYER - 0.5) {
		return vec4(textureLod(sampler2DArray(vt_cache_height, SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_CLAMP), vec3(p_coord.xy, 0.0), p_lod).r, 0.0, 0.0, 1.0);
	}
	if (p_srgb) {
		return textureLod(sampler2DArray(vt_cache_srgb[1], SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_CLAMP), p_coord, p_lod);
	}
	return textureLod(sampler2DArray(vt_cache[1], SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_CLAMP), p_coord, p_lod);
}

#ifdef VT_FEEDBACK
// Records the page a pixel wanted. Only one pixel in every 4x4 block reports in a frame, a different
// one each frame, and pages are written into a small hash table, so that the thousands of pixels
// wanting the same page cost one slot. What does not fit is reported again by a later frame.
void vt_write_feedback(uint p_entry, vec2 p_uv, ivec2 p_pages, int p_mip) {
	uvec2 cell = uvec2(gl_FragCoord.xy) & uvec2(3u);
	if (cell.x + cell.y * 4u != ((vt_feedback.frame * 7u) & 15u) || gl_HelperInvocation) {
		return;
	}
	uint id = p_entry >> 21u;
	if (id == 0u) {
		return;
	}
	uvec2 pages = uvec2(max(p_pages >> p_mip, ivec2(1)));
	uvec2 page = min(uvec2(p_uv * vec2(pages)), pages - 1u);
	// Pages are numbered across every mipmap, finest first.
	uint first = (4u * uint(p_pages.x * p_pages.y) - 4u * pages.x * pages.y) / 3u;
	uint key = (id << 21u) | (first + page.y * pages.x + page.x);
	uint slot = (key * 2654435761u) >> (32u - VT_FEEDBACK_SLOT_BITS);
	for (uint i = 0u; i < 4u; i++) {
		uint index = (slot + i) & (VT_FEEDBACK_SLOTS - 1u);
		uint current = vt_feedback.slots[index];
		if (current == key) {
			return;
		}
		if (current == 0u) {
			current = atomicCompSwap(vt_feedback.slots[index], 0u, key);
			if (current == 0u || current == key) {
				return;
			}
		}
	}
}
#endif

// Samples a virtual texture at p_lod, its mipmap, from the page table on. With p_grad, the hardware
// filters with the given gradients (in UV) instead, which must agree with p_lod.
vec4 vt_sample(utexture2D p_page_table, vec2 p_uv, float p_layer, float p_lod, bool p_grad, vec2 p_dx, vec2 p_dy, uint p_flags) {
	ivec2 pages = textureSize(usampler2D(p_page_table, SAMPLER_NEAREST_CLAMP), 0);
	int mipmaps = textureQueryLevels(usampler2D(p_page_table, SAMPLER_NEAREST_CLAMP));
	vec2 uv = vt_wrap(p_uv, p_flags);

	int mip = clamp(int(floor(p_lod)), 0, mipmaps - 1);
	ivec2 mip_pages = max(pages >> mip, ivec2(1));
	uint entry = texelFetch(usampler2D(p_page_table, SAMPLER_NEAREST_CLAMP), min(ivec2(uv * vec2(mip_pages)), mip_pages - 1), mip).r;

#ifdef VT_FEEDBACK
	vt_write_feedback(entry, uv, pages, mip);
#endif

	// Wherever the page at the mipmap wanted is not resident, the entry names its closest resident
	// ancestor (or a tile of flat color, while there is none), which is sampled instead.
	uint resident_mip = (entry >> 16u) & 0xFu;
	vec2 resident_pages = vec2(max(pages >> int(min(resident_mip, 14u)), ivec2(1)));
	vec2 in_page = uv * resident_pages;
	in_page -= floor(in_page);
	vec2 tile = vec2(float(entry & 0xFFu), float((entry >> 8u) & 0xFFu));
	uint cache = (entry >> 20u) & 1u;
	vec2 cache_size = vt_cache_size(cache);
	vec3 coord = vec3((tile * VT_TILE_SIZE + VT_PAGE_BORDER + in_page * VT_PAGE_SIZE) / cache_size, p_layer);
	bool srgb = (p_flags & VT_FLAG_SRGB) != 0u;

	if (p_grad) {
		// The tile's own mipmaps take the hardware's filtering from this page into its parent's texels,
		// and a little past the texture's coarsest page.
		vec2 scale = resident_pages * VT_PAGE_SIZE / cache_size;
		return vt_cache_grad(cache, srgb, coord, p_dx * scale, p_dy * scale);
	}
	float resident_lod = resident_mip == VT_ENTRY_EMPTY_MIP ? 0.0 : float(resident_mip);
	return vt_cache_lod(cache, srgb, coord, clamp(p_lod - resident_lod, 0.0, VT_TILE_MIPMAPS - 1.0));
}

// The mipmap to sample for these gradients. Where the footprint is more anisotropic than tile borders
// allow, its short axis is lengthened (which is what picks the mipmap), so that the hardware, given the
// adjusted gradients, filters at the same mipmap and no further than the border along the long axis.
float vt_gradients_lod(utexture2D p_page_table, inout vec2 r_dx, inout vec2 r_dy) {
	vec2 texels = vec2(textureSize(usampler2D(p_page_table, SAMPLER_NEAREST_CLAMP), 0)) * VT_PAGE_SIZE;
	vec2 tdx = r_dx * texels;
	vec2 tdy = r_dy * texels;
	float len_x = dot(tdx, tdx);
	float len_y = dot(tdy, tdy);
	float major = max(len_x, len_y);
	float minor = min(len_x, len_y);
	float target_minor = max(minor, major / (VT_MAX_ANISOTROPY * VT_MAX_ANISOTROPY));
	if (minor > 0.0 && target_minor > minor) {
		float lengthen = sqrt(target_minor / minor);
		if (len_x < len_y) {
			r_dx *= lengthen;
		} else {
			r_dy *= lengthen;
		}
	}
	return 0.5 * log2(max(target_minor, 1e-20));
}

vec4 vt_texture_grad(utexture2D p_page_table, vec2 p_uv, vec2 p_dx, vec2 p_dy, uint p_flags) {
	vec2 dx = p_dx;
	vec2 dy = p_dy;
	float lod = vt_gradients_lod(p_page_table, dx, dy);
	return vt_sample(p_page_table, p_uv, 0.0, lod, true, dx, dy, p_flags);
}

vec4 vt_texture_grad_array(utexture2D p_page_table, vec3 p_uv, vec2 p_dx, vec2 p_dy, uint p_flags) {
	vec2 dx = p_dx;
	vec2 dy = p_dy;
	float lod = vt_gradients_lod(p_page_table, dx, dy);
	return vt_sample(p_page_table, p_uv.xy, p_uv.z, lod, true, dx, dy, p_flags);
}

vec4 vt_texture_lod(utexture2D p_page_table, vec2 p_uv, float p_lod, uint p_flags) {
	return vt_sample(p_page_table, p_uv, 0.0, p_lod, false, vec2(0.0), vec2(0.0), p_flags);
}

vec4 vt_texture_lod_array(utexture2D p_page_table, vec3 p_uv, float p_lod, uint p_flags) {
	return vt_sample(p_page_table, p_uv.xy, p_uv.z, p_lod, false, vec2(0.0), vec2(0.0), p_flags);
}

#ifdef VT_STAGE_FRAGMENT

vec4 vt_texture(utexture2D p_page_table, vec2 p_uv, uint p_flags) {
	return vt_texture_grad(p_page_table, p_uv, dFdx(p_uv), dFdy(p_uv), p_flags);
}

vec4 vt_texture_array(utexture2D p_page_table, vec3 p_uv, uint p_flags) {
	return vt_texture_grad_array(p_page_table, p_uv, dFdx(p_uv.xy), dFdy(p_uv.xy), p_flags);
}

vec4 vt_texture_bias(utexture2D p_page_table, vec2 p_uv, float p_bias, uint p_flags) {
	float scale = exp2(p_bias);
	return vt_texture_grad(p_page_table, p_uv, dFdx(p_uv) * scale, dFdy(p_uv) * scale, p_flags);
}

vec4 vt_texture_bias_array(utexture2D p_page_table, vec3 p_uv, float p_bias, uint p_flags) {
	float scale = exp2(p_bias);
	return vt_texture_grad_array(p_page_table, p_uv, dFdx(p_uv.xy) * scale, dFdy(p_uv.xy) * scale, p_flags);
}

#else

// Outside fragment shaders there are no derivatives: texture() reads the finest mipmap, as it would.

vec4 vt_texture(utexture2D p_page_table, vec2 p_uv, uint p_flags) {
	return vt_texture_lod(p_page_table, p_uv, 0.0, p_flags);
}

vec4 vt_texture_array(utexture2D p_page_table, vec3 p_uv, uint p_flags) {
	return vt_texture_lod_array(p_page_table, p_uv, 0.0, p_flags);
}

vec4 vt_texture_bias(utexture2D p_page_table, vec2 p_uv, float p_bias, uint p_flags) {
	return vt_texture_lod(p_page_table, p_uv, max(p_bias, 0.0), p_flags);
}

vec4 vt_texture_bias_array(utexture2D p_page_table, vec3 p_uv, float p_bias, uint p_flags) {
	return vt_texture_lod_array(p_page_table, p_uv, max(p_bias, 0.0), p_flags);
}

#endif

ivec2 vt_texture_size(utexture2D p_page_table, int p_lod) {
	ivec2 texels = textureSize(usampler2D(p_page_table, SAMPLER_NEAREST_CLAMP), 0) * int(VT_PAGE_SIZE);
	return max(texels >> p_lod, ivec2(1));
}

ivec3 vt_texture_size_array(utexture2D p_page_table, int p_lod) {
	int mipmaps = textureQueryLevels(usampler2D(p_page_table, SAMPLER_NEAREST_CLAMP));
	uint entry = texelFetch(usampler2D(p_page_table, SAMPLER_NEAREST_CLAMP), ivec2(0), mipmaps - 1).r;
	return ivec3(vt_texture_size(p_page_table, p_lod), vt_cache_layers((entry >> 20u) & 1u));
}
