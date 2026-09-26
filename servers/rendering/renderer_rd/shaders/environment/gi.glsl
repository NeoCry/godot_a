#[compute]

#version 450

#VERSION_DEFINES

#ifdef SAMPLE_VOXEL_GI_NEAREST
#extension GL_EXT_samplerless_texture_functions : enable
#endif

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#define M_PI 3.141592

/* Specialization Constants (Toggles) */

layout(constant_id = 0) const bool sc_half_res = false;
layout(constant_id = 1) const bool sc_use_full_projection_matrix = false;
layout(constant_id = 2) const bool sc_use_vrs = false;

#define SDFGI_MAX_CASCADES 8

//set 0 for SDFGI and render buffers

layout(set = 0, binding = 1) uniform texture3D sdf_cascades[SDFGI_MAX_CASCADES];
layout(set = 0, binding = 2) uniform texture3D light_cascades[SDFGI_MAX_CASCADES];
layout(set = 0, binding = 3) uniform texture3D aniso0_cascades[SDFGI_MAX_CASCADES];
layout(set = 0, binding = 4) uniform texture3D aniso1_cascades[SDFGI_MAX_CASCADES];
layout(set = 0, binding = 5) uniform texture3D occlusion_texture;

layout(set = 0, binding = 6) uniform sampler linear_sampler;
layout(set = 0, binding = 7) uniform sampler linear_sampler_with_mipmaps;

// Where each SDFGI probe was placed, one layer per cascade (see MODE_PROBE_PLACEMENT in
// sdfgi_preprocess.glsl): xyz its offset from the grid in voxels, w whether it is usable.
layout(set = 0, binding = 8) uniform texture2DArray sdfgi_probe_state;

struct ProbeCascadeData {
	vec3 position;
	float to_probe;
	ivec3 probe_world_offset;
	float to_cell; // 1/bounds * grid_size
	vec3 pad;
	float exposure_normalization;
};

layout(rgba16f, set = 0, binding = 9) uniform restrict writeonly image2D ambient_buffer;
layout(rgba16f, set = 0, binding = 10) uniform restrict writeonly image2D reflection_buffer;

layout(set = 0, binding = 11) uniform texture2DArray lightprobe_texture;

layout(set = 0, binding = 12) uniform texture2D depth_buffer;
layout(set = 0, binding = 13) uniform texture2D normal_roughness_buffer;
layout(set = 0, binding = 14) uniform utexture2D voxel_gi_buffer;

layout(set = 0, binding = 15, std140) uniform SDFGI {
	vec3 grid_size;
	uint max_cascades;

	bool use_occlusion;
	int probe_axis_size;
	float probe_to_uvw;
	float normal_bias;

	vec3 lightprobe_tex_pixel_size;
	float energy;

	vec3 lightprobe_uv_offset;
	float y_mult;

	vec3 occlusion_clamp;
	// Like normal_bias, in probe units, but towards the camera. Where two surfaces meet, the normal
	// bias runs along the other one, and geometry thinner than a voxel often lands in two of them,
	// one per face, each looking out its own side (and sometimes a voxel into the room). A point
	// next to such an edge can then look its probes up in the voxel of the far face, whose
	// occlusion is what the far side sees, and filtering blends that in: the probes behind the wall
	// show through along every edge of the room. The camera sees the point, so that side is open.
	float view_bias;

	vec3 occlusion_renormalize;
	uint flags; // SDFGI_FLAG_*

	vec3 cascade_probe_size;
	uint pad5;

	ProbeCascadeData cascades[SDFGI_MAX_CASCADES];
}
sdfgi;

#define MAX_VOXEL_GI_INSTANCES 8

struct VoxelGIData {
	mat4 xform; // 64 - 64

	vec3 bounds; // 12 - 76
	float dynamic_range; // 4 - 80

	float bias; // 4 - 84
	float reflection_bias; // 4 - 88
	float normal_bias; // 4 - 92
	bool blend_ambient; // 4 - 96

	uint mipmaps; // 4 - 100
	float anisotropic_strength; // 4 - 104
	float reflection_filter; // 4 - 108
	float exposure_normalization; // 4 - 112
};

layout(set = 0, binding = 16, std140) uniform VoxelGIs {
	VoxelGIData data[MAX_VOXEL_GI_INSTANCES];
}
voxel_gi_instances;

layout(set = 0, binding = 17) uniform texture3D voxel_gi_textures[MAX_VOXEL_GI_INSTANCES];

// Anisotropic (directional) mipmap chains, blended with `voxel_gi_textures` according to
// VoxelGIData.anisotropic_strength, to reduce light leaking through thin geometry.
layout(set = 0, binding = 20) uniform texture3D voxel_gi_aniso_px_textures[MAX_VOXEL_GI_INSTANCES];
layout(set = 0, binding = 21) uniform texture3D voxel_gi_aniso_nx_textures[MAX_VOXEL_GI_INSTANCES];
layout(set = 0, binding = 22) uniform texture3D voxel_gi_aniso_py_textures[MAX_VOXEL_GI_INSTANCES];
layout(set = 0, binding = 23) uniform texture3D voxel_gi_aniso_ny_textures[MAX_VOXEL_GI_INSTANCES];
layout(set = 0, binding = 24) uniform texture3D voxel_gi_aniso_pz_textures[MAX_VOXEL_GI_INSTANCES];
layout(set = 0, binding = 25) uniform texture3D voxel_gi_aniso_nz_textures[MAX_VOXEL_GI_INSTANCES];

layout(set = 0, binding = 18, std140) uniform SceneData {
	mat4x4 inv_projection[2];
	mat4x4 cam_transform;
	vec4 eye_offset[2];

	// This frame's view space -> the previous frame's clip space, and -> the previous
	// frame's view space. Used by the temporal reprojection below.
	mat4x4 reprojection;
	mat4x4 prev_view_from_view;

	ivec2 screen_size;
	float pad1;
	float pad2;
}
scene_data;

#ifdef USE_VRS
layout(r8ui, set = 0, binding = 19) uniform restrict readonly uimage2D vrs_buffer;
#endif

// Temporal history: the `_prev` textures hold what the previous frame wrote and are
// sampled at reprojected coordinates; the images are this frame's half of the ping-pong and
// are written at the current pixel. `depth` is the linear view-space depth each stored
// sample was traced at, which is what rejects history across a disocclusion.
layout(set = 0, binding = 26) uniform texture2D gi_history_ambient_prev;
layout(set = 0, binding = 27) uniform texture2D gi_history_reflection_prev;
layout(set = 0, binding = 28) uniform texture2D gi_history_depth_prev;

layout(rgba16f, set = 0, binding = 29) uniform restrict writeonly image2D gi_history_ambient;
layout(rgba16f, set = 0, binding = 30) uniform restrict writeonly image2D gi_history_reflection;
layout(r32f, set = 0, binding = 31) uniform restrict writeonly image2D gi_history_depth;

// Screen probes (see MODE_SCREEN_PROBE_TRACE below), one texel per probe: xyz where it is
// (camera-relative world space, like everything SDFGI works with here), w its distance from the
// camera, or 0 for a tile with no probe this frame; and the normal of the surface it sits on.
layout(rgba32f, set = 0, binding = 32) uniform restrict image2D screen_probe_position;
layout(rgba16f, set = 0, binding = 33) uniform restrict image2D screen_probe_normal;
// The light the probes gathered, as 9 spherical harmonics coefficients each, in a 3x3 block of
// texels: as their rays found it, and filtered with the neighboring probes.
layout(rgba16f, set = 0, binding = 34) uniform restrict image2D screen_probe_sh;
layout(rgba16f, set = 0, binding = 35) uniform restrict image2D screen_probe_sh_filtered;
// The previous frame's image, before tonemapping, which the probes' screen traces take the light
// they hit from (see SCREEN_PROBE_FLAG_SCREEN_TRACES).
layout(set = 0, binding = 36) uniform texture2D screen_probe_last_frame;

layout(push_constant, std430) uniform Params {
	uint max_voxel_gi_instances;
	bool high_quality_vct;
	bool orthogonal;
	uint view_index;

	vec4 proj_info;

	float z_near;
	float z_far;
	bool temporal_enabled;
	uint trace_slot;

	float temporal_blend;
	bool history_valid;
	uint screen_probe_frame;
	float screen_probe_blend;

	ivec2 screen_probe_grid; // 0 when screen probes are off.
	ivec2 screen_probe_offset; // Where in its tile each probe goes this frame.

	uint screen_probe_flags;
	uint pad2;
	uint pad3;
	uint pad4;
}
params;

// The previous frame's image is there for the screen probes to trace rays against.
#define SCREEN_PROBE_FLAG_SCREEN_TRACES 1

vec2 octahedron_wrap(vec2 v) {
	vec2 signVal;
	signVal.x = v.x >= 0.0 ? 1.0 : -1.0;
	signVal.y = v.y >= 0.0 ? 1.0 : -1.0;
	return (1.0 - abs(v.yx)) * signVal;
}

vec2 octahedron_encode(vec3 n) {
	// https://twitter.com/Stubbesaurus/status/937994790553227264
	n /= (abs(n.x) + abs(n.y) + abs(n.z));
	n.xy = n.z >= 0.0 ? n.xy : octahedron_wrap(n.xy);
	n.xy = n.xy * 0.5 + 0.5;
	return n.xy;
}

vec4 blend_color(vec4 src, vec4 dst) {
	vec4 res;
	float sa = 1.0 - src.a;
	res.a = dst.a * sa + src.a;
	if (res.a == 0.0) {
		res.rgb = vec3(0);
	} else {
		res.rgb = (dst.rgb * dst.a * sa + src.rgb * src.a) / res.a;
	}
	return res;
}

vec3 reconstruct_position(ivec2 screen_pos) {
	if (sc_use_full_projection_matrix) {
		vec4 pos;
		pos.xy = (2.0 * vec2(screen_pos) / vec2(scene_data.screen_size)) - 1.0;
		pos.z = texelFetch(sampler2D(depth_buffer, linear_sampler), screen_pos, 0).r;
		pos.w = 1.0;

		pos = scene_data.inv_projection[params.view_index] * pos;

		return pos.xyz / pos.w;
	} else {
		vec3 pos;
		pos.z = texelFetch(sampler2D(depth_buffer, linear_sampler), screen_pos, 0).r;

		pos.z = pos.z * 2.0 - 1.0;
		if (params.orthogonal) {
			pos.z = -(pos.z * (params.z_far - params.z_near) - (params.z_far + params.z_near)) / 2.0;
		} else {
			pos.z = 2.0 * params.z_near * params.z_far / (params.z_far + params.z_near + pos.z * (params.z_far - params.z_near));
		}
		pos.z = -pos.z;

		pos.xy = vec2(screen_pos) * params.proj_info.xy + params.proj_info.zw;
		if (!params.orthogonal) {
			pos.xy *= pos.z;
		}

		pos.y = -pos.y;

		return pos;
	}
}

vec4 fetch_normal_and_roughness(ivec2 pos) {
	vec4 normal_roughness = texelFetch(sampler2D(normal_roughness_buffer, linear_sampler), pos, 0);
	normal_roughness.xyz = normalize(normal_roughness.xyz * 2.0 - 1.0);
	return normal_roughness;
}

// Probes are occluded by tracing the distance field towards each of them from the pixel, rather
// than through the occlusion volume (rendering/global_illumination/sdfgi/per_pixel_visibility).
#define SDFGI_FLAG_PER_PIXEL_VISIBILITY 1

// Distance field samples per probe, at most, in that mode.
#define SDFGI_VISIBILITY_STEPS 24

// How clear the distance field leaves the segment from p_from to p_to (both in the cascade's
// voxels): 1 where it stays out of the voxels holding geometry, 0 where it goes through one, and in
// between where it only clips one, which keeps the edge of what a probe lights soft. The last voxel
// before the probe is not tested, since probes are only kept clear of geometry by about that much.
float sdfgi_trace_visibility(uint p_cascade, vec3 p_from, vec3 p_to) {
	vec3 ray = p_to - p_from;
	float len = length(ray);
	float end = len - 1.0;
	if (end <= 0.0) {
		return 1.0;
	}

	vec3 dir = ray / len;
	float to_uvw = 1.0 / sdfgi.grid_size.x;
	float visibility = 1.0;
	float t = 0.5;
	for (int i = 0; i < SDFGI_VISIBILITY_STEPS && t < end; i++) {
		// 0 in a voxel with geometry, 1 on its boundary, 2 at the center of a free voxel next to it.
		float sdf = textureLod(sampler3D(sdf_cascades[p_cascade], linear_sampler), (p_from + dir * t) * to_uvw, 0.0).r * 255.0;
		visibility = min(visibility, clamp((sdf - 0.75) * 2.0, 0.0, 1.0));
		if (visibility <= 0.0) {
			break;
		}
		// Geometry is at least about sdf - 2 voxels away. Near it, never step more than half a
		// voxel, so as not to step over a wall one voxel thick.
		t += max(sdf - 2.0, 0.5);
	}
	return visibility;
}

// How visible the probe of p_cascade at p_probe (on the grid, in probe units) is from p_cascade_pos
// (in the same units), according to the occlusion volume.
float sdfgi_probe_occlusion(uint p_cascade, ivec3 p_probe, vec3 p_cascade_pos) {
	vec3 probe_pos = vec3(p_probe);
	ivec3 occ_indexv = abs((sdfgi.cascades[p_cascade].probe_world_offset + p_probe) & ivec3(1, 1, 1)) * ivec3(1, 2, 4);
	vec4 occ_mask = mix(vec4(0.0), vec4(1.0), equal(ivec4(occ_indexv.x | occ_indexv.y), ivec4(0, 1, 2, 3)));

	vec3 occ_pos = clamp(p_cascade_pos, probe_pos - sdfgi.occlusion_clamp, probe_pos + sdfgi.occlusion_clamp) * sdfgi.probe_to_uvw;
	occ_pos.z += float(p_cascade);
	if (occ_indexv.z != 0) { //z bit is on, means index is >=4, so make it switch to the other half of textures
		occ_pos.x += 1.0;
	}

	occ_pos *= sdfgi.occlusion_renormalize;
	float occlusion = dot(textureLod(sampler3D(occlusion_texture, linear_sampler), occ_pos, 0.0), occ_mask);
	// Crush the sliver of visibility that filtering gives a probe hidden a voxel away (as
	// DDGI does with its weights): next to the geometry hiding a much brighter probe, even
	// that much of it outweighs the probes the point really sees.
	return occlusion < 0.2 ? occlusion * occlusion * occlusion * 25.0 : occlusion;
}

// r_visibility is how much of the weight the point would give its probes, before occlusion,
// goes to probes it can actually see: near zero when every probe around it is hidden from it.
void sdfvoxel_gi_process(uint cascade, vec3 cascade_pos, vec3 cam_pos, vec3 cam_normal, vec3 cam_specular_normal, float roughness, out vec3 diffuse_light, out vec3 specular_light, out float r_visibility) {
	cascade_pos += cam_normal * sdfgi.normal_bias;
	// Towards the camera, too: see the view bias in the SDFGI data above.
	cascade_pos += normalize(-cam_pos) * sdfgi.view_bias;

	vec3 base_pos = floor(cascade_pos);
	//cascade_pos += mix(vec3(0.0),vec3(0.01),lessThan(abs(cascade_pos-base_pos),vec3(0.01))) * cam_normal;
	ivec3 probe_base_pos = ivec3(base_pos);

	vec4 diffuse_accum = vec4(0.0);
	vec3 specular_accum;

	ivec3 tex_pos = ivec3(probe_base_pos.xy, int(cascade));
	tex_pos.x += probe_base_pos.z * sdfgi.probe_axis_size;
	tex_pos.xy = tex_pos.xy * (SDFGI_OCT_SIZE + 2) + ivec2(1);

	vec3 diffuse_posf = (vec3(tex_pos) + vec3(octahedron_encode(cam_normal) * float(SDFGI_OCT_SIZE), 0.0)) * sdfgi.lightprobe_tex_pixel_size;

	vec3 specular_posf = (vec3(tex_pos) + vec3(octahedron_encode(cam_specular_normal) * float(SDFGI_OCT_SIZE), 0.0)) * sdfgi.lightprobe_tex_pixel_size;

	specular_accum = vec3(0.0);

	vec4 light_accum = vec4(0.0);
	float weight_accum = 0.0;

	float visible_weight = 0.0;
	float total_weight = 0.0;

	float voxels_to_probes = sdfgi.cascade_probe_size.x / sdfgi.grid_size.x;
	bool per_pixel_visibility = sdfgi.use_occlusion && bool(sdfgi.flags & SDFGI_FLAG_PER_PIXEL_VISIBILITY);
	if (per_pixel_visibility) {
		// Only from a point clear of the geometry. From one inside it (along an edge, where a thin
		// wall was voxelized a voxel into the room) every probe looks hidden, and the occlusion
		// volume, which gives surface voxels what their open side sees, is left to handle it.
		per_pixel_visibility = textureLod(sampler3D(sdf_cascades[cascade], linear_sampler), cascade_pos * sdfgi.probe_to_uvw, 0.0).r * 255.0 >= 1.0;
	}

	for (uint j = 0; j < 8; j++) {
		ivec3 offset = (ivec3(j) >> ivec3(0, 1, 2)) & ivec3(1, 1, 1);
		ivec3 probe_posi = probe_base_pos;
		probe_posi += offset;

		// Compute weight

		// The probe may have been moved off its grid point, out of geometry: interpolation keeps
		// using the grid (so it stays continuous), but whether the probe is in front of the
		// surface is judged from where it really is, and one that could not be placed anywhere
		// usable does not count at all.
		vec4 probe_state = texelFetch(sampler2DArray(sdfgi_probe_state, linear_sampler), ivec3(probe_posi.x + probe_posi.z * sdfgi.probe_axis_size, probe_posi.y, int(cascade)), 0);

		vec3 probe_pos = vec3(probe_posi);
		vec3 probe_to_pos = cascade_pos - probe_pos;
		vec3 probe_dir = normalize(probe_pos + probe_state.xyz * voxels_to_probes - cascade_pos);

		vec3 trilinear = vec3(1.0) - abs(probe_to_pos);
		float weight = trilinear.x * trilinear.y * trilinear.z * max(0.005, dot(cam_normal, probe_dir));
		total_weight += weight;
		weight *= probe_state.w;

		// Compute lightprobe occlusion

		if (sdfgi.use_occlusion) {
			float occlusion = sdfgi_probe_occlusion(cascade, probe_posi, cascade_pos);

			if (per_pixel_visibility && occlusion > 0.0) {
				// The line to the probe, traced, settles what the volume can only tell a voxel at a
				// time. The volume still has the last word on what it knows to be hidden: a point
				// next to geometry sees it only a fraction of a voxel away, where the traced line
				// cannot tell grazing it from going through.
				occlusion = min(occlusion, sdfgi_trace_visibility(cascade, cascade_pos / voxels_to_probes, (probe_pos + probe_state.xyz * voxels_to_probes) / voxels_to_probes));
			}

			visible_weight += weight * occlusion;
			// Occluded probes keep only a token weight, so that a point hidden from all of them
			// still gets a (normalizable) answer, which the fallback in sdfgi_process() then
			// replaces with the next cascade's. It used to be 0.01, but next to a wall with sunlit
			// probes behind it, the probes the point does see can be a thousand times darker, and
			// even that small share of the hidden ones lit the whole wall, in bands where they sit
			// close.
			weight *= max(occlusion, 0.0001);
		} else {
			visible_weight += weight;
		}

		// Compute lightprobe texture position

		vec3 diffuse;
		vec3 pos_uvw = diffuse_posf;
		pos_uvw.xy += vec2(offset.xy) * sdfgi.lightprobe_uv_offset.xy;
		pos_uvw.x += float(offset.z) * sdfgi.lightprobe_uv_offset.z;
		diffuse = textureLod(sampler2DArray(lightprobe_texture, linear_sampler), pos_uvw, 0.0).rgb;

		diffuse_accum += vec4(diffuse * weight * sdfgi.cascades[cascade].exposure_normalization, weight);

		{
			vec3 specular = vec3(0.0);
			vec3 pos_uvw = specular_posf;
			pos_uvw.xy += vec2(offset.xy) * sdfgi.lightprobe_uv_offset.xy;
			pos_uvw.x += float(offset.z) * sdfgi.lightprobe_uv_offset.z;
			if (roughness < 0.99) {
				specular = textureLod(sampler2DArray(lightprobe_texture, linear_sampler), pos_uvw + vec3(0, 0, float(sdfgi.max_cascades)), 0.0).rgb;
			}
			if (roughness > 0.2) {
				specular = mix(specular, textureLod(sampler2DArray(lightprobe_texture, linear_sampler), pos_uvw, 0.0).rgb, (roughness - 0.2) * 1.25);
			}

			specular_accum += specular * weight * sdfgi.cascades[cascade].exposure_normalization;
		}
	}

	if (diffuse_accum.a > 0.0) {
		diffuse_accum.rgb /= diffuse_accum.a;
	}

	diffuse_light = diffuse_accum.rgb;

	if (diffuse_accum.a > 0.0) {
		specular_accum /= diffuse_accum.a;
	}

	specular_light = specular_accum;

	r_visibility = total_weight > 0.0 ? visible_weight / total_weight : 1.0;
}

void sdfgi_process(vec3 vertex, vec3 normal, vec3 reflection, float roughness, out vec4 ambient_light, out vec4 reflection_light) {
	//make vertex orientation the world one, but still align to camera
	vertex.y *= sdfgi.y_mult;
	normal.y *= sdfgi.y_mult;
	reflection.y *= sdfgi.y_mult;

	//renormalize
	normal = normalize(normal);
	reflection = normalize(reflection);

	vec3 cam_pos = vertex;
	vec3 cam_normal = normal;

	vec4 light_accum = vec4(0.0);
	float weight_accum = 0.0;

	vec4 light_blend_accum = vec4(0.0);
	float weight_blend_accum = 0.0;

	float blend = -1.0;

	// helper constants, compute once

	uint cascade = 0xFFFFFFFF;
	vec3 cascade_pos;
	vec3 cascade_normal;

	for (uint i = 0; i < sdfgi.max_cascades; i++) {
		cascade_pos = (cam_pos - sdfgi.cascades[i].position) * sdfgi.cascades[i].to_probe;

		if (any(lessThan(cascade_pos, vec3(0.0))) || any(greaterThanEqual(cascade_pos, sdfgi.cascade_probe_size))) {
			continue; //skip cascade
		}

		cascade = i;
		break;
	}

	if (cascade < SDFGI_MAX_CASCADES) {
		ambient_light = vec4(0, 0, 0, 1);
		reflection_light = vec4(0, 0, 0, 1);

		float blend;
		vec3 diffuse, specular;
		float visibility;
		sdfvoxel_gi_process(cascade, cascade_pos, cam_pos, cam_normal, reflection, roughness, diffuse, specular, visibility);

		{
			//process blend
			float blend_from = (float(sdfgi.probe_axis_size - 1) / 2.0) - 2.5;
			float blend_to = blend_from + 2.0;

			vec3 inner_pos = cam_pos * sdfgi.cascades[cascade].to_probe;

			float len = length(inner_pos);

			inner_pos = abs(normalize(inner_pos));
			len *= max(inner_pos.x, max(inner_pos.y, inner_pos.z));

			if (len >= blend_from) {
				blend = smoothstep(blend_from, blend_to, len);
			} else {
				blend = 0.0;
			}
		}

		// When the point can see none of this cascade's probes (a space smaller than the probe
		// spacing, closed off from all of them, or surrounded by probes stuck in geometry), the
		// floor weights would hand it whichever hidden probe happened to be nearest, often lit
		// from the other side of a wall. Lean on the next
		// cascade instead, if it sees the point better. Only then, though: a point that sees just
		// a few of its probes, or only far ones (next to a wall, with the probes behind it
		// carrying most of the interpolation weight), is lit right by those alone, and the next
		// cascade, twice as coarse, is the one more likely to see through the wall.
		float fallback = 1.0 - smoothstep(0.01, 0.05, visibility);

		if (blend > 0.0 || fallback > 0.0) {
			//blend
			if (cascade == sdfgi.max_cascades - 1) {
				ambient_light.a = 1.0 - blend;
				reflection_light.a = 1.0 - blend;

			} else {
				vec3 diffuse2, specular2;
				float visibility2;
				cascade_pos = (cam_pos - sdfgi.cascades[cascade + 1].position) * sdfgi.cascades[cascade + 1].to_probe;
				sdfvoxel_gi_process(cascade + 1, cascade_pos, cam_pos, cam_normal, reflection, roughness, diffuse2, specular2, visibility2);
				float mix_weight = max(blend, fallback * clamp((visibility2 - visibility) * 5.0, 0.0, 1.0));
				diffuse = mix(diffuse, diffuse2, mix_weight);
				specular = mix(specular, specular2, mix_weight);
			}
		}

		ambient_light.rgb = diffuse;

		if (roughness < 0.2) {
			vec3 pos_to_uvw = 1.0 / sdfgi.grid_size;
			vec4 light_accum = vec4(0.0);

			float blend_size = (sdfgi.grid_size.x / float(sdfgi.probe_axis_size - 1)) * 0.5;

			float radius_sizes[SDFGI_MAX_CASCADES];
			cascade = 0xFFFF;

			float base_distance = length(cam_pos);
			for (uint i = 0; i < sdfgi.max_cascades; i++) {
				radius_sizes[i] = (1.0 / sdfgi.cascades[i].to_cell) * (sdfgi.grid_size.x * 0.5 - blend_size);
				if (cascade == 0xFFFF && base_distance < radius_sizes[i]) {
					cascade = i;
				}
			}

			cascade = min(cascade, sdfgi.max_cascades - 1);

			float max_distance = radius_sizes[sdfgi.max_cascades - 1];
			vec3 ray_pos = cam_pos;
			vec3 ray_dir = reflection;

			{
				float prev_radius = cascade > 0 ? radius_sizes[cascade - 1] : 0.0;
				float base_blend = (base_distance - prev_radius) / (radius_sizes[cascade] - prev_radius);
				float bias = (1.0 + base_blend) * 1.1;
				vec3 abs_ray_dir = abs(ray_dir);
				//ray_pos += ray_dir * (bias / sdfgi.cascades[cascade].to_cell); //bias to avoid self occlusion
				ray_pos += (ray_dir * 1.0 / max(abs_ray_dir.x, max(abs_ray_dir.y, abs_ray_dir.z)) + cam_normal * 1.4) * bias / sdfgi.cascades[cascade].to_cell;
			}
			float softness = 0.2 + min(1.0, roughness * 5.0) * 4.0; //approximation to roughness so it does not seem like a hard fade
			uint i = 0;
			bool found = false;
			while (true) {
				if (length(ray_pos) >= max_distance || light_accum.a > 0.99) {
					break;
				}
				if (!found && i >= cascade && length(ray_pos) < radius_sizes[i]) {
					uint next_i = min(i + 1, sdfgi.max_cascades - 1);
					cascade = max(i, cascade); //never go down

					vec3 pos = ray_pos - sdfgi.cascades[i].position;
					pos *= sdfgi.cascades[i].to_cell * pos_to_uvw;

					float fdistance = textureLod(sampler3D(sdf_cascades[i], linear_sampler), pos, 0.0).r * 255.0 - 1.1;

					vec4 hit_light = vec4(0.0);
					if (fdistance < softness) {
						hit_light.rgb = textureLod(sampler3D(light_cascades[i], linear_sampler), pos, 0.0).rgb;
						hit_light.rgb *= 0.5; //approximation given value read is actually meant for anisotropy
						hit_light.a = clamp(1.0 - (fdistance / softness), 0.0, 1.0);
						hit_light.rgb *= hit_light.a;
					}

					fdistance /= sdfgi.cascades[i].to_cell;

					if (i < (sdfgi.max_cascades - 1)) {
						pos = ray_pos - sdfgi.cascades[next_i].position;
						pos *= sdfgi.cascades[next_i].to_cell * pos_to_uvw;

						float fdistance2 = textureLod(sampler3D(sdf_cascades[next_i], linear_sampler), pos, 0.0).r * 255.0 - 1.1;

						vec4 hit_light2 = vec4(0.0);
						if (fdistance2 < softness) {
							hit_light2.rgb = textureLod(sampler3D(light_cascades[next_i], linear_sampler), pos, 0.0).rgb;
							hit_light2.rgb *= 0.5; //approximation given value read is actually meant for anisotropy
							hit_light2.a = clamp(1.0 - (fdistance2 / softness), 0.0, 1.0);
							hit_light2.rgb *= hit_light2.a;
						}

						float prev_radius = i == 0 ? 0.0 : radius_sizes[max(0, i - 1)];
						float blend = clamp((length(ray_pos) - prev_radius) / (radius_sizes[i] - prev_radius), 0.0, 1.0);

						fdistance2 /= sdfgi.cascades[next_i].to_cell;

						hit_light = mix(hit_light, hit_light2, blend);
						fdistance = mix(fdistance, fdistance2, blend);
					}

					light_accum += hit_light;
					ray_pos += ray_dir * fdistance;
					found = true;
				}
				i++;
				if (i == sdfgi.max_cascades) {
					i = 0;
					found = false;
				}
			}

			vec3 light = light_accum.rgb / max(light_accum.a, 0.00001);
			float alpha = min(1.0, light_accum.a);

			float b = min(1.0, roughness * 5.0);

			float sa = 1.0 - b;

			reflection_light.a = alpha * sa + b;
			if (reflection_light.a == 0) {
				specular = vec3(0.0);
			} else {
				specular = (light * alpha * sa + specular * b) / reflection_light.a;
			}
		}

		reflection_light.rgb = specular;

		ambient_light.rgb *= sdfgi.energy;
		reflection_light.rgb *= sdfgi.energy;
	} else {
		ambient_light = vec4(0);
		reflection_light = vec4(0);
	}
}

/* SCREEN PROBES */

// Diffuse light gathered at probes placed on the surfaces the camera sees, after the final gather
// of Unreal Engine's Lumen, rather than looked up per pixel from the SDFGI probe grid
// (rendering/global_illumination/sdfgi/screen_probes). There is a probe per SCREEN_PROBE_TILE
// pixels square, at a different pixel of its tile every frame, and a second one for another surface
// in the tile, if there is one (MODE_SCREEN_PROBE_TRACE). Each traces a hemisphere of rays from where
// it is: against the depth buffer first, for a short distance (see screen_probe_screen_trace()),
// then through the distance field for the first couple of SDFGI probe spacings, taking the light for
// whatever those do not hit from the SDFGI probes, which have traced the rest of the way already.
// The probes are then averaged with their neighbors (MODE_SCREEN_PROBE_FILTER), and every pixel
// interpolates the ones around it, weighted by how well their surfaces match its own, and averages
// that over frames (see main()); where none of them does, it falls back to the SDFGI probes.
//
// Light gathered from exactly where the surface is cannot leak in from behind a wall its own rays
// would hit, as light interpolated between probes up to a probe spacing away can, and it resolves
// contact and corners to the distance field's voxel, or to the pixel on screen, rather than to the
// probe grid.

// How far the probes' rays are traced before the SDFGI probes take over, in probe spacings of the
// cascade they start in. By then a ray is far enough from the surface it left for the probes
// around its end to see what it would, rather than the other side of that surface.
#define SCREEN_PROBE_TRACE_PROBES 2.0
// Distance field steps per ray, at most.
#define SCREEN_PROBE_MAX_STEPS 64
// Where rays start, in voxels of the cascade they start in: off the surface along its normal, then
// towards the camera, which is on the open side of the surface even where the normal runs along
// another one (a floor meeting a wall), and a little along the ray. The surface is voxelized into
// the voxel it is in, which the ray must leave before it can tell geometry from its own surface.
#define SCREEN_PROBE_NORMAL_OFFSET 1.5
#define SCREEN_PROBE_VIEW_OFFSET 1.0
#define SCREEN_PROBE_RAY_OFFSET 0.5
// How far off each other's planes a probe and a pixel (or two probes) may be for one to stand for
// the other, relative to their distance from the camera, and how closely they must face the same way.
#define SCREEN_PROBE_PLANE_TOLERANCE 0.025
#define SCREEN_PROBE_NORMAL_POWER 4.0
// Pixels whose probes weigh less than this between them (interpolation weights times how well
// each fits) make up the difference with the SDFGI probes.
#define SCREEN_PROBE_MIN_WEIGHT 0.25
// Each tile has a second probe (SCREEN_PROBES_PER_TILE, from GI), for a second surface in it: one the
// first probe fits (see screen_probe_fit()) less than this.
#define SCREEN_PROBE_SECOND_FIT 0.25
// A pixel's light that changes by more than SCREEN_PROBE_CHANGE_RELATIVE from its history (relative
// to the larger of the two, plus a small absolute margin for the dark), two frames in a row, keeps
// only SCREEN_PROBE_CHANGE_FRAMES frames of it, and goes on doing so for as long as it keeps changing
// the same way by more than SCREEN_PROBE_TREND_RELATIVE a frame. The states in between are kept in
// the fraction of the frame count the history holds.
#define SCREEN_PROBE_CHANGE_RELATIVE 0.5
#define SCREEN_PROBE_TREND_RELATIVE 0.1
#define SCREEN_PROBE_CHANGE_ABSOLUTE 0.002
#define SCREEN_PROBE_CHANGE_FRAMES 3.0
#define SCREEN_PROBE_STATE_FALLING 0.25
#define SCREEN_PROBE_STATE_SUSPECT 0.5
#define SCREEN_PROBE_STATE_RISING 0.75

// What the SDFGI probe maps hold (see MODE_STORE in sdfgi_integrate.glsl) is 4 / PI times the light
// it stands for. The screen probes take what they sample from them back to radiance, to add it up
// with the light their rays hit, and scale their result like the maps, so that it matches what the
// SDFGI probes give the same pixel.
#define SDFGI_PROBE_MAP_SCALE (4.0 / M_PI)

// The light the SDFGI probes of p_cascade around p_pos (camera-relative, y scaled) see coming from
// p_dir (y scaled), as their maps hold it (see SDFGI_PROBE_MAP_SCALE).
vec3 sdfgi_probe_radiance(uint p_cascade, vec3 p_pos, vec3 p_dir) {
	vec3 cascade_pos = (p_pos - sdfgi.cascades[p_cascade].position) * sdfgi.cascades[p_cascade].to_probe;
	cascade_pos = clamp(cascade_pos, vec3(0.0), sdfgi.cascade_probe_size - 0.001);

	ivec3 probe_base_pos = ivec3(floor(cascade_pos));
	// The radiance maps are the layers after the irradiance maps (see sdfvoxel_gi_process()).
	ivec3 tex_pos = ivec3(probe_base_pos.xy, int(p_cascade + sdfgi.max_cascades));
	tex_pos.x += probe_base_pos.z * sdfgi.probe_axis_size;
	tex_pos.xy = tex_pos.xy * (SDFGI_OCT_SIZE + 2) + ivec2(1);
	vec3 radiance_posf = (vec3(tex_pos) + vec3(octahedron_encode(p_dir) * float(SDFGI_OCT_SIZE), 0.0)) * sdfgi.lightprobe_tex_pixel_size;

	vec4 light_accum = vec4(0.0);
	for (uint j = 0; j < 8; j++) {
		ivec3 offset = (ivec3(j) >> ivec3(0, 1, 2)) & ivec3(1, 1, 1);
		ivec3 probe_posi = probe_base_pos + offset;

		vec4 probe_state = texelFetch(sampler2DArray(sdfgi_probe_state, linear_sampler), ivec3(probe_posi.x + probe_posi.z * sdfgi.probe_axis_size, probe_posi.y, int(p_cascade)), 0);
		vec3 trilinear = vec3(1.0) - abs(cascade_pos - vec3(probe_posi));
		float weight = trilinear.x * trilinear.y * trilinear.z * probe_state.w;
		if (sdfgi.use_occlusion && weight > 0.0) {
			weight *= max(sdfgi_probe_occlusion(p_cascade, probe_posi, cascade_pos), 0.0001);
		}

		vec3 pos_uvw = radiance_posf;
		pos_uvw.xy += vec2(offset.xy) * sdfgi.lightprobe_uv_offset.xy;
		pos_uvw.x += float(offset.z) * sdfgi.lightprobe_uv_offset.z;
		light_accum += vec4(textureLod(sampler2DArray(lightprobe_texture, linear_sampler), pos_uvw, 0.0).rgb * weight, weight);
	}

	// The maps are band limited spherical harmonics, which ring below zero around bright spots.
	vec3 light = light_accum.a > 0.0 ? max(light_accum.rgb / light_accum.a, vec3(0.0)) : vec3(0.0);
	return light * sdfgi.cascades[p_cascade].exposure_normalization;
}

// Marches p_dir (unit length, y scaled) from p_from (camera-relative, y scaled) through the distance
// field, from p_cascade on through the cascades around it as it leaves each, for p_max_distance at
// most (y scaled). On a hit, returns true and the light leaving what it hit towards the ray, as
// sdfgi_integrate.glsl works it out for the SDFGI probes. Otherwise returns false, where it stopped,
// and the cascade it stopped in.
bool screen_probe_march(uint p_cascade, vec3 p_from, vec3 p_dir, float p_max_distance, out vec3 r_light, out vec3 r_end, out uint r_end_cascade) {
	vec3 ray_pos = p_from;
	vec3 inv_dir = 1.0 / p_dir;
	vec3 pos_to_uvw = 1.0 / sdfgi.grid_size;
	float traveled = 0.0;
	uint steps = 0;
	bool hit = false;
	uint hit_cascade = p_cascade;
	vec3 uvw = vec3(0.0);

	r_light = vec3(0.0);
	r_end_cascade = p_cascade;

	for (uint j = p_cascade; j < sdfgi.max_cascades; j++) {
		vec3 pos = (ray_pos - sdfgi.cascades[j].position) * sdfgi.cascades[j].to_cell;
		if (any(lessThan(pos, vec3(0.0))) || any(greaterThanEqual(pos, sdfgi.grid_size))) {
			continue; // Already past this cascade.
		}
		r_end_cascade = j;

		// How far the ray can go in this cascade before leaving it, and before it has gone
		// p_max_distance, in its voxels.
		vec3 t0 = -pos * inv_dir;
		vec3 t1 = (sdfgi.grid_size - pos) * inv_dir;
		vec3 tmax = max(t0, t1);
		float max_advance = min(tmax.x, min(tmax.y, tmax.z));
		float budget = (p_max_distance - traveled) * sdfgi.cascades[j].to_cell;
		bool out_of_budget = budget <= max_advance;
		max_advance = min(max_advance, budget);

		float advance = 0.0;
		while (advance < max_advance && steps < SCREEN_PROBE_MAX_STEPS) {
			uvw = (pos + p_dir * advance) * pos_to_uvw;
			float distance = textureLod(sampler3D(sdf_cascades[j], linear_sampler), uvw, 0.0).r * 255.0 - 1.0;
			if (distance < 0.05) {
				hit = true;
				break;
			}
			advance += distance;
			steps++;
		}

		if (hit) {
			hit_cascade = j;
			break;
		}

		advance = min(advance, max_advance);
		ray_pos += p_dir * (advance / sdfgi.cascades[j].to_cell);
		traveled += advance / sdfgi.cascades[j].to_cell;

		if (out_of_budget || steps >= SCREEN_PROBE_MAX_STEPS) {
			break;
		}
	}

	r_end = ray_pos;
	if (!hit) {
		return false;
	}

	// Sample the cascade the ray hit in only from the invocations whose ray hit in it, so that
	// the texture array is always indexed with the loop counter, as sdfgi_integrate.glsl does.
	for (uint j = p_cascade; j < sdfgi.max_cascades; j++) {
		if (j == hit_cascade) {
			const float EPSILON = 0.001;
			vec3 gradient = vec3(
					textureLod(sampler3D(sdf_cascades[j], linear_sampler), uvw + vec3(EPSILON, 0.0, 0.0), 0.0).r - textureLod(sampler3D(sdf_cascades[j], linear_sampler), uvw - vec3(EPSILON, 0.0, 0.0), 0.0).r,
					textureLod(sampler3D(sdf_cascades[j], linear_sampler), uvw + vec3(0.0, EPSILON, 0.0), 0.0).r - textureLod(sampler3D(sdf_cascades[j], linear_sampler), uvw - vec3(0.0, EPSILON, 0.0), 0.0).r,
					textureLod(sampler3D(sdf_cascades[j], linear_sampler), uvw + vec3(0.0, 0.0, EPSILON), 0.0).r - textureLod(sampler3D(sdf_cascades[j], linear_sampler), uvw - vec3(0.0, 0.0, EPSILON), 0.0).r);
			// Deep in geometry (a ray that started in it), the field is flat: take the face the
			// ray came in through.
			vec3 hit_normal = dot(gradient, gradient) > 1e-12 ? normalize(gradient) : -p_dir;

			vec3 hit_light = textureLod(sampler3D(light_cascades[j], linear_sampler), uvw, 0.0).rgb;
			vec4 aniso0 = textureLod(sampler3D(aniso0_cascades[j], linear_sampler), uvw, 0.0);
			vec3 hit_aniso0 = aniso0.rgb;
			vec3 hit_aniso1 = vec3(aniso0.a, textureLod(sampler3D(aniso1_cascades[j], linear_sampler), uvw, 0.0).rg);

			r_light = hit_light * (dot(max(vec3(0.0), (hit_normal * hit_aniso0)), vec3(1.0)) + dot(max(vec3(0.0), (-hit_normal * hit_aniso1)), vec3(1.0)));
			r_light *= sdfgi.cascades[j].exposure_normalization;
		}
	}
	return true;
}

// Real spherical harmonics, bands 0 to 2, in the order sdfgi_integrate.glsl uses.
float sh_basis(uint p_index, vec3 p_dir) {
	switch (p_index) {
		case 0:
			return 0.282095;
		case 1:
			return 0.488603 * p_dir.y;
		case 2:
			return 0.488603 * p_dir.z;
		case 3:
			return 0.488603 * p_dir.x;
		case 4:
			return 1.092548 * p_dir.x * p_dir.y;
		case 5:
			return 1.092548 * p_dir.y * p_dir.z;
		case 6:
			return 0.315392 * (3.0 * p_dir.z * p_dir.z - 1.0);
		case 7:
			return 1.092548 * p_dir.x * p_dir.z;
		default:
			return 0.546274 * (p_dir.x * p_dir.x - p_dir.y * p_dir.y);
	}
}

// The texel of probe p_index of p_tile in the textures that hold a texel per probe.
ivec2 screen_probe_texel(ivec2 p_tile, uint p_index) {
	return ivec2(p_tile.x * SCREEN_PROBES_PER_TILE + int(p_index), p_tile.y);
}

// The texel of the block of the probe at p_texel (see screen_probe_texel()) holding its coefficient p_index.
ivec2 screen_probe_sh_texel(ivec2 p_texel, uint p_index) {
	return p_texel * 3 + ivec2(p_index % 3, p_index / 3);
}

// The light a surface facing p_normal gets from what p_probe gathered (filtered with its neighbors).
vec3 screen_probe_irradiance(ivec2 p_probe, vec3 p_normal) {
	vec3 light = vec3(0.0);
	for (uint i = 0; i < 9; i++) {
		light += imageLoad(screen_probe_sh_filtered, screen_probe_sh_texel(p_probe, i)).rgb * sh_basis(i, p_normal);
	}
	return light;
}

// How well the light gathered at p_probe_pos, on a surface facing p_probe_normal, stands for the
// light at p_pos, on one facing p_normal: 1 on the same plane facing the same way, falling to 0
// p_tolerance off either one's plane, and as their normals part.
float screen_probe_fit(vec3 p_probe_pos, vec3 p_probe_normal, vec3 p_pos, vec3 p_normal, float p_tolerance) {
	vec3 offset = p_pos - p_probe_pos;
	float plane_distance = max(abs(dot(offset, p_probe_normal)), abs(dot(offset, p_normal)));
	float fit = clamp(1.0 - plane_distance / p_tolerance, 0.0, 1.0);
	return fit * fit * pow(max(dot(p_probe_normal, p_normal), 0.0), SCREEN_PROBE_NORMAL_POWER);
}

// Interpolates the screen probes around the pixel at p_pos (in screen pixels), whose camera-relative
// position, normal and distance from the camera these are, into r_light. Returns how much of it to
// use: 1 where the probes fit the pixel well enough, down to 0 where none of them does.
float screen_probe_gather(ivec2 p_pos, vec3 p_vertex, vec3 p_normal, float p_depth, out vec3 r_light) {
	// This frame's probes sit on a regular grid, SCREEN_PROBE_TILE pixels apart, from the offset.
	vec2 lattice = vec2(p_pos - params.screen_probe_offset) / float(SCREEN_PROBE_TILE);
	ivec2 base = ivec2(floor(lattice));
	vec2 frac = lattice - vec2(base);
	float tolerance = p_depth * SCREEN_PROBE_PLANE_TOLERANCE;

	vec3 light = vec3(0.0);
	float weight_sum = 0.0;
	for (int i = 0; i < 4 * SCREEN_PROBES_PER_TILE; i++) {
		ivec2 offset = ivec2(i & 1, (i >> 1) & 1);
		vec2 bilinear = mix(1.0 - frac, frac, vec2(offset));
		float weight = bilinear.x * bilinear.y;
		if (weight <= 0.0) {
			continue;
		}
		ivec2 probe = screen_probe_texel(clamp(base + offset, ivec2(0), params.screen_probe_grid - 1), uint(i >> 2));
		vec4 probe_position = imageLoad(screen_probe_position, probe);
		if (probe_position.w <= 0.0) {
			continue;
		}
		weight *= screen_probe_fit(probe_position.xyz, imageLoad(screen_probe_normal, probe).xyz, p_vertex, p_normal, tolerance);
		if (weight <= 0.0) {
			continue;
		}
		light += screen_probe_irradiance(probe, p_normal) * weight;
		weight_sum += weight;
	}

	// Spherical harmonics this coarse ring below zero opposite bright light.
	r_light = weight_sum > 0.0 ? max(light / weight_sum, vec3(0.0)) * sdfgi.energy : vec3(0.0);
	return clamp(weight_sum / SCREEN_PROBE_MIN_WEIGHT, 0.0, 1.0);
}

// Screen traces: before the distance field, each ray is traced against the depth buffer, for
// SCREEN_PROBE_SCREEN_TRACE_CELLS voxels of the cascade it starts in. That is where the distance field
// sees least (a ray only starts in it a voxel and a half off the surface, and nothing smaller than
// a voxel is in it at all), and what the ray hits on screen gives it the light the previous frame
// drew there, shadows, detail and all, even from what is not in the distance field (objects out of
// SDFGI, those too small or thin for its voxels).
#define SCREEN_PROBE_SCREEN_TRACE_CELLS 4.0
#define SCREEN_PROBE_SCREEN_STEPS 16
// How far behind the depth buffer a ray may pass for it to count as hitting what is drawn there,
// relative to how far that is from the camera. Farther, it passes behind something whose back is
// not on screen, and the distance field takes the ray over from the start.
#define SCREEN_PROBE_SCREEN_THICKNESS 0.03
// Where the rays start, off the surface towards the camera, relative to how far it is from it: away
// from the surface's own pixels, which would otherwise hit it for want of depth buffer precision.
#define SCREEN_PROBE_SCREEN_BIAS 0.004

// Where p_view (view space) lands on screen, in the pixel coordinates reconstruct_position() takes.
vec2 screen_probe_project(vec3 p_view) {
	vec2 xy = vec2(p_view.x, -p_view.y);
	if (!params.orthogonal) {
		xy /= p_view.z;
	}
	return (xy - params.proj_info.zw) / params.proj_info.xy;
}

// Traces p_dir (view space) from p_from (view space) against the depth buffer, for p_length at most,
// with the samples along it offset by p_jitter (0 to 1) of a step. Returns true where the ray hits
// something drawn on screen that faces it, with the light the previous frame drew there, and false
// where it leaves the screen, passes behind something, or gets to the end without hitting anything.
bool screen_probe_screen_trace(vec3 p_from, vec3 p_dir, float p_length, float p_jitter, out vec3 r_light) {
	r_light = vec3(0.0);

	vec3 to = p_from + p_dir * p_length;
	if (!params.orthogonal && to.z > -params.z_near) {
		// Stop short of the near plane, through which the ray would project to the other side.
		to = p_from + (to - p_from) * ((-params.z_near - p_from.z) / (to.z - p_from.z) * 0.99);
	}

	vec2 from_pixel = screen_probe_project(p_from);
	vec2 to_pixel = screen_probe_project(to);
	// A sample every pixel or so, up to SCREEN_PROBE_SCREEN_STEPS.
	int steps = clamp(int(ceil(length(to_pixel - from_pixel))), 1, SCREEN_PROBE_SCREEN_STEPS);

	for (int i = 0; i < steps; i++) {
		float t = (float(i) + p_jitter) / float(steps);
		ivec2 pixel = ivec2(round(mix(from_pixel, to_pixel, t)));
		if (any(lessThan(pixel, ivec2(0))) || any(greaterThanEqual(pixel, scene_data.screen_size))) {
			return false;
		}

		// The ray's depth there, interpolated as it projects.
		float ray_depth = params.orthogonal ? -mix(p_from.z, to.z, t) : -1.0 / mix(1.0 / p_from.z, 1.0 / to.z, t);
		vec3 surface = reconstruct_position(pixel);
		float behind = ray_depth + surface.z;
		if (behind <= 0.0) {
			continue; // Still in front of what is drawn there.
		}
		if (behind > -surface.z * SCREEN_PROBE_SCREEN_THICKNESS) {
			return false;
		}
		// A ray can only hit the front of something. Behind the depth buffer by a sliver at a
		// surface it does not face, it is running along the one it started on, or one like it.
		if (dot(fetch_normal_and_roughness(pixel).xyz, p_dir) >= 0.0) {
			continue;
		}

		// Where that was on the previous frame's image.
		vec4 prev_clip = scene_data.reprojection * vec4(surface, 1.0);
		if (prev_clip.w <= 1e-6) {
			return false;
		}
		vec2 prev_uv = (prev_clip.xy / prev_clip.w) * 0.5 + 0.5 + 0.5 / vec2(scene_data.screen_size);
		if (any(lessThan(prev_uv, vec2(0.0))) || any(greaterThan(prev_uv, vec2(1.0)))) {
			return false;
		}
		r_light = textureLod(sampler2D(screen_probe_last_frame, linear_sampler), prev_uv, 0.0).rgb;
		return true;
	}
	return false;
}

// The alpha sdfgi_process() gives the ambient light at p_vertex (camera-relative): 1 inside the
// cascades, fading out across the outer edge of the last one.
float sdfgi_ambient_alpha(vec3 p_vertex) {
	vec3 cam_pos = p_vertex;
	cam_pos.y *= sdfgi.y_mult;
	for (uint i = 0; i < sdfgi.max_cascades; i++) {
		vec3 cascade_pos = (cam_pos - sdfgi.cascades[i].position) * sdfgi.cascades[i].to_probe;
		if (any(lessThan(cascade_pos, vec3(0.0))) || any(greaterThanEqual(cascade_pos, sdfgi.cascade_probe_size))) {
			continue;
		}
		if (i < sdfgi.max_cascades - 1) {
			return 1.0;
		}
		float blend_from = (float(sdfgi.probe_axis_size - 1) / 2.0) - 2.5;
		float blend_to = blend_from + 2.0;
		vec3 inner_pos = cam_pos * sdfgi.cascades[i].to_probe;
		float len = length(inner_pos);
		inner_pos = abs(normalize(inner_pos));
		len *= max(inner_pos.x, max(inner_pos.y, inner_pos.z));
		return len >= blend_from ? 1.0 - smoothstep(blend_from, blend_to, len) : 1.0;
	}
	return 0.0;
}

// Combines the 6 anisotropic mip chains of a probe, weighting each axis by how much the
// cone direction points along it (dir*dir sums to 1 for a unit vector), and picking
// whichever of the +/- textures matches the direction's sign on that axis.
vec4 sample_aniso_voxel(uint index, vec3 uvw_pos, vec3 direction, float lod) {
	// Selecting between two texture handles with a ternary operator is not reliably
	// supported across GLSL compilers, so branch on which array to sample instead.
	vec3 dir2 = direction * direction;
	vec4 result = vec4(0.0);
	if (direction.x > 0.0) {
		result += dir2.x * textureLod(sampler3D(voxel_gi_aniso_px_textures[index], linear_sampler_with_mipmaps), uvw_pos, lod);
	} else {
		result += dir2.x * textureLod(sampler3D(voxel_gi_aniso_nx_textures[index], linear_sampler_with_mipmaps), uvw_pos, lod);
	}
	if (direction.y > 0.0) {
		result += dir2.y * textureLod(sampler3D(voxel_gi_aniso_py_textures[index], linear_sampler_with_mipmaps), uvw_pos, lod);
	} else {
		result += dir2.y * textureLod(sampler3D(voxel_gi_aniso_ny_textures[index], linear_sampler_with_mipmaps), uvw_pos, lod);
	}
	if (direction.z > 0.0) {
		result += dir2.z * textureLod(sampler3D(voxel_gi_aniso_pz_textures[index], linear_sampler_with_mipmaps), uvw_pos, lod);
	} else {
		result += dir2.z * textureLod(sampler3D(voxel_gi_aniso_nz_textures[index], linear_sampler_with_mipmaps), uvw_pos, lod);
	}
	return result;
}

// Samples the isotropic mip chain, blending in the anisotropic one according to
// p_aniso_strength (0 = isotropic only, matching the pre-anisotropic-mipmaps behavior
// exactly; 1 = anisotropic only). Keeping this a plain linear blend at trace time (rather
// than pre-blending when the mipmaps are baked) means the slider scales the visible
// effect predictably and can be tweaked without re-baking.
vec4 sample_voxel(texture3D probe, uint index, float p_aniso_strength, vec3 uvw_pos, vec3 direction, float lod) {
	vec4 iso_color = textureLod(sampler3D(probe, linear_sampler_with_mipmaps), uvw_pos, lod);
	if (p_aniso_strength <= 0.0) {
		return iso_color;
	}
	vec4 aniso_color = sample_aniso_voxel(index, uvw_pos, direction, lod);
	return mix(iso_color, aniso_color, p_aniso_strength);
}

// Standard voxel cone trace. `p_step_scale` shortens the march's step as a fraction of the
// cone radius, which is the step the cone is sampled at. At 1.0 a step equals the radius,
// so the cone is sampled barely twice per footprint: that is enough for the diffuse cones,
// which are wide and average many voxels anyway, but on a near-mirror reflection cone the
// steps keep landing on voxel boundaries at a consistent phase and draw a diagonal
// herringbone across the reflection. Smaller values sample the same cone more finely, so
// each sample's contribution is scaled to match and the accumulated result stays put.
vec4 voxel_cone_trace(texture3D probe, uint index, float p_aniso_strength, vec3 cell_size, vec3 pos, vec3 direction, float tan_half_angle, float p_step_scale, float max_distance, float p_bias) {
	float dist = p_bias;
	vec4 color = vec4(0.0);

	while (dist < max_distance && color.a < 1.0) {
		float diameter = max(1.0, 2.0 * tan_half_angle * dist);
		vec3 uvw_pos = (pos + dist * direction) * cell_size;
		float half_diameter = diameter * 0.5;
		//check if outside, then break
		if (any(greaterThan(abs(uvw_pos - 0.5), vec3(0.5f + half_diameter * cell_size)))) {
			break;
		}
		float lod = log2(diameter);
		vec4 scolor = sample_voxel(probe, index, p_aniso_strength, uvw_pos, direction, lod);
		float a = (1.0 - color.a);
		color += a * scolor * p_step_scale;
		dist += half_diameter * p_step_scale;
	}

	return color;
}

vec4 voxel_cone_trace_45_degrees(texture3D probe, uint index, float p_aniso_strength, vec3 cell_size, vec3 pos, vec3 direction, float max_distance, float p_bias) {
	float dist = p_bias;
	vec4 color = vec4(0.0);
	float radius = max(0.5, dist);
	float lod_level = log2(radius * 2.0);

	while (dist < max_distance && color.a < 1.0) {
		vec3 uvw_pos = (pos + dist * direction) * cell_size;

		//check if outside, then break
		if (any(greaterThan(abs(uvw_pos - 0.5), vec3(0.5f + radius * cell_size)))) {
			break;
		}
		vec4 scolor = sample_voxel(probe, index, p_aniso_strength, uvw_pos, direction, lod_level);
		lod_level += 1.0;

		float a = (1.0 - color.a);
		scolor *= a;
		color += scolor;
		dist += radius;
		radius = max(0.5, dist);
	}
	return color;
}

void voxel_gi_compute(uint index, vec3 position, vec3 normal, vec3 ref_vec, mat3 normal_xform, float roughness, inout vec4 out_spec, inout vec4 out_diff, inout float out_blend) {
	position = (voxel_gi_instances.data[index].xform * vec4(position, 1.0)).xyz;
	ref_vec = normalize((voxel_gi_instances.data[index].xform * vec4(ref_vec, 0.0)).xyz);
	normal = normalize((voxel_gi_instances.data[index].xform * vec4(normal, 0.0)).xyz);

	position += normal * voxel_gi_instances.data[index].normal_bias;

	//this causes corrupted pixels, i have no idea why..
	if (any(bvec2(any(lessThan(position, vec3(0.0))), any(greaterThan(position, voxel_gi_instances.data[index].bounds))))) {
		return;
	}

	mat3 dir_xform = mat3(voxel_gi_instances.data[index].xform) * normal_xform;

	vec3 blendv = abs(position / voxel_gi_instances.data[index].bounds * 2.0 - 1.0);
	float blend = clamp(1.0 - max(blendv.x, max(blendv.y, blendv.z)), 0.0, 1.0);
	//float blend=1.0;

	float max_distance = length(voxel_gi_instances.data[index].bounds);
	vec3 cell_size = 1.0 / voxel_gi_instances.data[index].bounds;

	float aniso_strength = voxel_gi_instances.data[index].anisotropic_strength;

	//irradiance

	vec4 light = vec4(0.0);

	if (params.high_quality_vct) {
		const uint cone_dir_count = 6;
		vec3 cone_dirs[cone_dir_count] = vec3[](
				vec3(0.0, 0.0, 1.0),
				vec3(0.866025, 0.0, 0.5),
				vec3(0.267617, 0.823639, 0.5),
				vec3(-0.700629, 0.509037, 0.5),
				vec3(-0.700629, -0.509037, 0.5),
				vec3(0.267617, -0.823639, 0.5));

		float cone_weights[cone_dir_count] = float[](0.25, 0.15, 0.15, 0.15, 0.15, 0.15);
		float cone_angle_tan = 0.577;

		for (uint i = 0; i < cone_dir_count; i++) {
			vec3 dir = normalize(dir_xform * cone_dirs[i]);
			light += cone_weights[i] * voxel_cone_trace(voxel_gi_textures[index], index, aniso_strength, cell_size, position, dir, cone_angle_tan, 1.0, max_distance, voxel_gi_instances.data[index].bias);
		}
	} else {
		const uint cone_dir_count = 4;
		vec3 cone_dirs[cone_dir_count] = vec3[](
				vec3(0.707107, 0.0, 0.707107),
				vec3(0.0, 0.707107, 0.707107),
				vec3(-0.707107, 0.0, 0.707107),
				vec3(0.0, -0.707107, 0.707107));

		float cone_weights[cone_dir_count] = float[](0.25, 0.25, 0.25, 0.25);
		for (int i = 0; i < cone_dir_count; i++) {
			vec3 dir = normalize(dir_xform * cone_dirs[i]);
			light += cone_weights[i] * voxel_cone_trace_45_degrees(voxel_gi_textures[index], index, aniso_strength, cell_size, position, dir, max_distance, voxel_gi_instances.data[index].bias);
		}
	}

	light.rgb *= voxel_gi_instances.data[index].dynamic_range * voxel_gi_instances.data[index].exposure_normalization;
	if (!voxel_gi_instances.data[index].blend_ambient) {
		light.a = 1.0;
	}

	out_diff += light * blend;

	//radiance
	vec4 irr_light = voxel_cone_trace(voxel_gi_textures[index], index, aniso_strength, cell_size, position, ref_vec, tan(roughness * 0.5 * M_PI * 0.99), 1.0 / (1.0 + voxel_gi_instances.data[index].reflection_filter), max_distance, voxel_gi_instances.data[index].reflection_bias);
	irr_light.rgb *= voxel_gi_instances.data[index].dynamic_range * voxel_gi_instances.data[index].exposure_normalization;
	if (!voxel_gi_instances.data[index].blend_ambient) {
		irr_light.a = 1.0;
	}

	out_spec += irr_light * blend;

	out_blend += blend;
}

void process_gi(ivec2 pos, vec3 vertex, inout vec4 ambient_light, inout vec4 reflection_light) {
	vec4 normal_roughness = fetch_normal_and_roughness(pos);

	vec3 normal = normal_roughness.xyz;

	if (normal.length() > 0.5) {
		//valid normal, can do GI
		float roughness = normal_roughness.w;
		bool dynamic_object = roughness > 0.5;
		if (dynamic_object) {
			roughness = 1.0 - roughness;
		}
		roughness /= (127.0 / 255.0);
		vec3 view = -normalize(mat3(scene_data.cam_transform) * (vertex - scene_data.eye_offset[gl_GlobalInvocationID.z].xyz));
		vertex = mat3(scene_data.cam_transform) * vertex;
		normal = normalize(mat3(scene_data.cam_transform) * normal);
		vec3 reflection = normalize(reflect(-view, normal));

#ifdef USE_SDFGI
		sdfgi_process(vertex, normal, reflection, roughness, ambient_light, reflection_light);
#endif

#ifdef USE_VOXEL_GI_INSTANCES
		{
#ifdef SAMPLE_VOXEL_GI_NEAREST
			uvec2 voxel_gi_tex = texelFetch(voxel_gi_buffer, pos, 0).rg;
#else
			uvec2 voxel_gi_tex = texelFetch(usampler2D(voxel_gi_buffer, linear_sampler), pos, 0).rg;
#endif
			roughness *= roughness;
			//find arbitrary tangent and bitangent, then build a matrix
			vec3 v0 = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
			vec3 tangent = normalize(cross(v0, normal));
			vec3 bitangent = normalize(cross(tangent, normal));
			mat3 normal_mat = mat3(tangent, bitangent, normal);

			vec4 amb_accum = vec4(0.0);
			vec4 spec_accum = vec4(0.0);
			float blend_accum = 0.0;

			for (uint i = 0; i < params.max_voxel_gi_instances; i++) {
				if (any(equal(uvec2(i), voxel_gi_tex))) {
					voxel_gi_compute(i, vertex, normal, reflection, normal_mat, roughness, spec_accum, amb_accum, blend_accum);
				}
			}
			if (blend_accum > 0.0) {
				amb_accum /= blend_accum;
				spec_accum /= blend_accum;
			}

#ifdef USE_SDFGI
			reflection_light = blend_color(spec_accum, reflection_light);
			ambient_light = blend_color(amb_accum, ambient_light);
#else
			reflection_light = spec_accum;
			ambient_light = amb_accum;
#endif
		}
#endif
	}
}

#if defined(MODE_SCREEN_PROBE_TRACE)

// One workgroup per tile, one invocation per ray, tracing the tile's probes one after the other.

// The pixels of the tile the invocations look at between them, 64 spread over all of it: xyz where
// each is (camera-relative), w its distance from the camera, 0 if nothing was drawn there.
shared vec4 screen_probe_candidate_position[64];
shared vec3 screen_probe_candidate_normal[64];
// Which of the candidates each of the tile's probes goes to, -1 for none.
shared int screen_probe_pick[SCREEN_PROBES_PER_TILE];
shared vec3 screen_probe_ray_light[64];
shared vec3 screen_probe_ray_dir[64];

uint screen_probe_hash(uint p_value) {
	p_value ^= p_value >> 16;
	p_value *= 0x7feb352du;
	p_value ^= p_value >> 15;
	p_value *= 0x846ca68bu;
	p_value ^= p_value >> 16;
	return p_value;
}

// The pixel of candidate p_index.
ivec2 screen_probe_candidate_pixel(ivec2 p_tile, int p_index) {
	const int step = SCREEN_PROBE_TILE / 8;
	return p_tile * SCREEN_PROBE_TILE + ivec2(p_index % 8, p_index / 8) * step + params.screen_probe_offset % step;
}

// Traces the probe that goes to candidate p_pick (-1 for none) into p_texel of the probe textures,
// or clears the texel. Every invocation of the workgroup calls it with the same arguments.
void screen_probe_trace(ivec2 p_texel, int p_pick) {
	uint ray = gl_LocalInvocationIndex;

	vec4 position = vec4(0.0);
	vec3 normal = vec3(0.0, 1.0, 0.0);
	uint cascade = SDFGI_MAX_CASCADES;
	vec3 cascade_vertex = vec3(0.0);
	if (p_pick >= 0) {
		position = screen_probe_candidate_position[p_pick];
		normal = screen_probe_candidate_normal[p_pick];

		// The cascade to start the rays in: the first one around the probe, as for a pixel in
		// sdfgi_process().
		cascade_vertex = position.xyz;
		cascade_vertex.y *= sdfgi.y_mult;
		for (uint i = 0; i < sdfgi.max_cascades; i++) {
			vec3 cascade_pos = (cascade_vertex - sdfgi.cascades[i].position) * sdfgi.cascades[i].to_probe;
			if (all(greaterThanEqual(cascade_pos, vec3(0.0))) && all(lessThan(cascade_pos, sdfgi.cascade_probe_size))) {
				cascade = i;
				break;
			}
		}
	}

	if (cascade >= SDFGI_MAX_CASCADES) {
		// No probe, or none SDFGI covers.
		if (ray == 0) {
			imageStore(screen_probe_position, p_texel, vec4(0.0));
			imageStore(screen_probe_normal, p_texel, vec4(0.0));
		}
		if (ray < 9) {
			imageStore(screen_probe_sh, screen_probe_sh_texel(p_texel, ray), vec4(0.0));
		}
		return;
	}

	// The rays are stratified over the hemisphere: each invocation takes one of 8x8 cells of equal
	// solid angle, and a point in it that changes every frame (the same one in every cell of the
	// probe, so that between them they keep covering the hemisphere evenly).
	uint seed = screen_probe_hash(uint(p_texel.x + p_texel.y * params.screen_probe_grid.x * SCREEN_PROBES_PER_TILE) ^ screen_probe_hash(params.screen_probe_frame));
	vec2 jitter = vec2(uvec2(seed, seed >> 16) & uvec2(0xFFFF)) / 65536.0;
	vec2 cell = (vec2(gl_LocalInvocationID.xy) + jitter) / 8.0;
	float cos_theta = cell.x;
	float sin_theta = sqrt(max(0.0, 1.0 - cos_theta * cos_theta));
	float phi = cell.y * 2.0 * M_PI;

	// An orthonormal basis around the normal (Duff et al., "Building an Orthonormal Basis, Revisited").
	float basis_sign = normal.z >= 0.0 ? 1.0 : -1.0;
	float basis_a = -1.0 / (basis_sign + normal.z);
	float basis_b = normal.x * normal.y * basis_a;
	vec3 tangent = vec3(1.0 + basis_sign * normal.x * normal.x * basis_a, basis_sign * basis_b, -basis_sign * normal.x);
	vec3 bitangent = vec3(basis_b, basis_sign + normal.y * normal.y * basis_a, -normal.y);
	vec3 ray_dir = normalize(tangent * (cos(phi) * sin_theta) + bitangent * (sin(phi) * sin_theta) + normal * cos_theta);

	float cell_size = 1.0 / sdfgi.cascades[cascade].to_cell;

	vec3 light;
	bool hit = false;
	if (bool(params.screen_probe_flags & SCREEN_PROBE_FLAG_SCREEN_TRACES)) {
		vec3 view_vertex = position.xyz * mat3(scene_data.cam_transform);
		vec3 screen_from = params.orthogonal ? view_vertex + vec3(0.0, 0.0, position.w * SCREEN_PROBE_SCREEN_BIAS) : view_vertex * (1.0 - SCREEN_PROBE_SCREEN_BIAS);
		float screen_jitter = float(screen_probe_hash(seed + ray) & 0xFFFFu) / 65536.0;
		hit = screen_probe_screen_trace(screen_from, ray_dir * mat3(scene_data.cam_transform), SCREEN_PROBE_SCREEN_TRACE_CELLS * cell_size, screen_jitter, light);
	}

	if (!hit) {
		// March in the cascades' space, where y is scaled.
		vec3 y_scale = vec3(1.0, sdfgi.y_mult, 1.0);
		vec3 cascade_ray_dir = normalize(ray_dir * y_scale);
		vec3 view_dir = -normalize(position.xyz);
		vec3 from = cascade_vertex + (normalize(normal * y_scale) * SCREEN_PROBE_NORMAL_OFFSET + normalize(view_dir * y_scale) * SCREEN_PROBE_VIEW_OFFSET + cascade_ray_dir * SCREEN_PROBE_RAY_OFFSET) * cell_size;
		float max_distance = SCREEN_PROBE_TRACE_PROBES / sdfgi.cascades[cascade].to_probe;

		vec3 end;
		uint end_cascade;
		if (!screen_probe_march(cascade, from, cascade_ray_dir, max_distance, light, end, end_cascade)) {
			light = sdfgi_probe_radiance(end_cascade, end, cascade_ray_dir) / SDFGI_PROBE_MAP_SCALE;
		}
	}

	screen_probe_ray_light[ray] = light;
	screen_probe_ray_dir[ray] = ray_dir;

	memoryBarrierShared();
	barrier();

	if (ray < 9) {
		// Project the rays onto spherical harmonics, each standing for 2 PI / 64 of solid angle
		// (none go below the surface, which is left dark), and convolve them with the cosine lobe
		// (over PI), so that evaluating them for a normal gives the irradiance of a surface facing
		// that way, over PI: what the lighting multiplies by the albedo.
		vec3 coefficient = vec3(0.0);
		for (uint i = 0; i < 64; i++) {
			coefficient += screen_probe_ray_light[i] * sh_basis(ray, screen_probe_ray_dir[i]);
		}
		float band_convolution = ray == 0 ? 1.0 : (ray < 4 ? 2.0 / 3.0 : 0.25);
		coefficient *= (2.0 * M_PI / 64.0) * band_convolution * SDFGI_PROBE_MAP_SCALE;
		imageStore(screen_probe_sh, screen_probe_sh_texel(p_texel, ray), vec4(coefficient, 0.0));
	}
	if (ray == 0) {
		imageStore(screen_probe_position, p_texel, position);
		imageStore(screen_probe_normal, p_texel, vec4(normal, 0.0));
	}

	// The next probe reuses the ray arrays.
	memoryBarrierShared();
	barrier();
}

void main() {
	ivec2 tile = ivec2(gl_WorkGroupID.xy);
	uint ray = gl_LocalInvocationIndex;

	ivec2 candidate = screen_probe_candidate_pixel(tile, int(ray));
	vec4 candidate_position = vec4(0.0);
	vec3 candidate_normal = vec3(0.0);
	// Nothing was drawn where the depth buffer still holds the far plane (0, with reverse Z).
	if (all(lessThan(candidate, scene_data.screen_size)) && texelFetch(sampler2D(depth_buffer, linear_sampler), candidate, 0).r > 0.0) {
		vec3 view_vertex = reconstruct_position(candidate);
		candidate_position = vec4(mat3(scene_data.cam_transform) * view_vertex, -view_vertex.z);
		candidate_normal = normalize(mat3(scene_data.cam_transform) * fetch_normal_and_roughness(candidate).xyz);
	}
	screen_probe_candidate_position[ray] = candidate_position;
	screen_probe_candidate_normal[ray] = candidate_normal;

	memoryBarrierShared();
	barrier();

	if (ray == 0) {
		// The first probe goes to the candidate nearest to where this frame puts probes in their tiles.
		// The second goes to the nearest of those the first does not stand for (see
		// screen_probe_fit()), if any: another surface, in front of or behind that one (the edge of an
		// object), or at an angle to it (a corner), whose pixels would otherwise find no probe to take
		// their light from in the frames the first lands on the other surface.
		ivec2 target = tile * SCREEN_PROBE_TILE + params.screen_probe_offset;
		int first = -1;
		float first_distance = 1e20;
		for (int i = 0; i < 64; i++) {
			if (screen_probe_candidate_position[i].w > 0.0) {
				vec2 to_target = vec2(screen_probe_candidate_pixel(tile, i) - target);
				float distance = dot(to_target, to_target);
				if (distance < first_distance) {
					first = i;
					first_distance = distance;
				}
			}
		}
		int second = -1;
		if (first >= 0) {
			vec4 first_position = screen_probe_candidate_position[first];
			vec3 first_normal = screen_probe_candidate_normal[first];
			float second_distance = 1e20;
			for (int i = 0; i < 64; i++) {
				if (screen_probe_candidate_position[i].w > 0.0 && screen_probe_fit(first_position.xyz, first_normal, screen_probe_candidate_position[i].xyz, screen_probe_candidate_normal[i], first_position.w * SCREEN_PROBE_PLANE_TOLERANCE) < SCREEN_PROBE_SECOND_FIT) {
					vec2 to_target = vec2(screen_probe_candidate_pixel(tile, i) - target);
					float distance = dot(to_target, to_target);
					if (distance < second_distance) {
						second = i;
						second_distance = distance;
					}
				}
			}
		}
		screen_probe_pick[0] = first;
		screen_probe_pick[1] = second;
	}

	memoryBarrierShared();
	barrier();

	for (uint i = 0; i < SCREEN_PROBES_PER_TILE; i++) {
		screen_probe_trace(screen_probe_texel(tile, i), screen_probe_pick[i]);
	}
}

#elif defined(MODE_SCREEN_PROBE_FILTER)

// Averages each probe with those around it that stand for it well, which lets 64 rays a probe do.
void main() {
	ivec2 probe = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(probe, params.screen_probe_grid * ivec2(SCREEN_PROBES_PER_TILE, 1)))) {
		return;
	}
	ivec2 tile = ivec2(probe.x / SCREEN_PROBES_PER_TILE, probe.y);

	vec3 sh[9];
	for (uint i = 0; i < 9; i++) {
		sh[i] = vec3(0.0);
	}

	vec4 position = imageLoad(screen_probe_position, probe);
	if (position.w > 0.0) {
		vec3 normal = imageLoad(screen_probe_normal, probe).xyz;
		float tolerance = position.w * SCREEN_PROBE_PLANE_TOLERANCE;
		float weight_sum = 0.0;
		for (int i = 0; i < 9 * SCREEN_PROBES_PER_TILE; i++) {
			ivec2 other_tile = tile + ivec2(i % 3, (i / 3) % 3) - 1;
			if (any(lessThan(other_tile, ivec2(0))) || any(greaterThanEqual(other_tile, params.screen_probe_grid))) {
				continue;
			}
			ivec2 other = screen_probe_texel(other_tile, uint(i / 9));
			vec4 other_position = imageLoad(screen_probe_position, other);
			if (other_position.w <= 0.0) {
				continue;
			}
			float weight = screen_probe_fit(other_position.xyz, imageLoad(screen_probe_normal, other).xyz, position.xyz, normal, tolerance);
			if (weight <= 0.0) {
				continue;
			}
			for (uint c = 0; c < 9; c++) {
				sh[c] += imageLoad(screen_probe_sh, screen_probe_sh_texel(other, c)).rgb * weight;
			}
			weight_sum += weight;
		}
		// A probe fits itself perfectly, so weight_sum is at least 1.
		for (uint i = 0; i < 9; i++) {
			sh[i] /= weight_sum;
		}
	}

	for (uint i = 0; i < 9; i++) {
		imageStore(screen_probe_sh_filtered, screen_probe_sh_texel(probe, i), vec4(sh[i], 0.0));
	}
}

#else

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);

	uint vrs_x, vrs_y;
#ifdef USE_VRS
	if (sc_use_vrs) {
		ivec2 vrs_pos;

		// Currently we use a 16x16 texel, possibly some day make this configurable.
		if (sc_half_res) {
			vrs_pos = pos >> 3;
		} else {
			vrs_pos = pos >> 4;
		}

		uint vrs_texel = imageLoad(vrs_buffer, vrs_pos).r;
		// note, valid values for vrs_x and vrs_y are 1, 2 and 4.
		vrs_x = 1 << ((vrs_texel >> 2) & 3);
		vrs_y = 1 << (vrs_texel & 3);

		if (mod(pos.x, vrs_x) != 0) {
			return;
		}

		if (mod(pos.y, vrs_y) != 0) {
			return;
		}
	}
#endif

	if (sc_half_res) {
		pos <<= 1;
	}

	if (any(greaterThanEqual(pos, scene_data.screen_size))) { //too large, do nothing
		return;
	}

	vec4 ambient_light = vec4(0.0);
	vec4 reflection_light = vec4(0.0);

	vec3 vertex = reconstruct_position(pos);

	// `pos` is in screen pixels, which is what reconstruct_position() and process_gi() want;
	// the GI buffers are half that when sc_half_res is set.
	ivec2 out_pos = sc_half_res ? (pos >> 1) : pos;

	// TEMPORAL REPROJECTION
	//
	// reconstruct_position() returns view space with -z pointing away from the camera, so
	// the linear depth to compare on is -z.
	float linear_depth = -vertex.z;
	bool history_valid = false;
	vec4 history_ambient = vec4(0.0);
	vec4 history_reflection = vec4(0.0);

	// The write below happens from the first frame after an allocation, but there is nothing
	// worth reading back until a frame has actually written it.
	if (params.temporal_enabled && params.history_valid) {
		vec4 prev_clip = scene_data.reprojection * vec4(vertex, 1.0);
		if (prev_clip.w > 1e-6) { // Behind the previous frame's camera otherwise.
			vec2 prev_uv = (prev_clip.xy / prev_clip.w) * 0.5 + 0.5;

			// reconstruct_position()'s single-view fast path (the one active whenever
			// temporal accumulation runs; the full-matrix path is multiview-only, and this
			// feature is off for multiview) treats `screen_pos` as a corner-based coordinate:
			// `pos.xy = screen_pos * proj_info.xy + proj_info.zw` round-trips exactly against
			// UV = screen_pos / size, not the texel-center UV = (screen_pos + 0.5) / size that
			// hardware texture sampling -- and the point-sampled depth fetch below -- assume.
			// Verified directly: for a bit-for-bit static camera, forward-projecting `vertex`
			// through scene_data.reprojection landed prev_uv exactly on out_pos/screen_size,
			// half a texel short of (out_pos+0.5)/screen_size, in both axes. Nothing else in
			// this file converts a reconstructed `vertex` back into UV space, so nothing else
			// needed to know; this reprojection is the first thing that does, and every
			// resample of a reused value applied the same half-texel-short offset again, which
			// is what read as the history "shifting" and blurring a completely static image.
			prev_uv += 0.5 / vec2(scene_data.screen_size);

			if (all(greaterThanEqual(prev_uv, vec2(0.0))) && all(lessThanEqual(prev_uv, vec2(1.0)))) {
				// The depth this point had in the previous frame's view, against the depth the
				// previous frame actually stored there. They disagree when something else was
				// in front of this point back then, which is what a disocclusion looks like.
				float expected_depth = -(scene_data.prev_view_from_view * vec4(vertex, 1.0)).z;

				// Bilinear filtering by hand, with each of the 4 taps checked against
				// expected_depth individually and dropped if it disagrees, then the weights
				// renormalised over whichever taps survived. Letting the sampler do the
				// filtering instead costs accuracy exactly at a depth edge: it blends across
				// the discontinuity and drags a background value onto a foreground pixel (or
				// the reverse) using a single depth check for all 4 taps, which is itself
				// sampled the same blended way and so tends to land between the two surfaces
				// and pass. This is what the remaining edge speckle in the moving-camera test
				// was, on top of the half-texel offset above.
				ivec2 history_size = textureSize(sampler2D(gi_history_depth_prev, linear_sampler), 0);
				vec2 tap_pos = prev_uv * vec2(history_size) - 0.5;
				ivec2 tap_base = ivec2(floor(tap_pos));
				vec2 tap_frac = tap_pos - vec2(tap_base);

				float tap_weights[4] = float[](
						(1.0 - tap_frac.x) * (1.0 - tap_frac.y),
						tap_frac.x * (1.0 - tap_frac.y),
						(1.0 - tap_frac.x) * tap_frac.y,
						tap_frac.x * tap_frac.y);
				ivec2 tap_offsets[4] = ivec2[](ivec2(0, 0), ivec2(1, 0), ivec2(0, 1), ivec2(1, 1));

				float weight_sum = 0.0;
				for (int t = 0; t < 4; t++) {
					if (tap_weights[t] <= 0.0) {
						continue;
					}
					ivec2 tap = clamp(tap_base + tap_offsets[t], ivec2(0), history_size - 1);
					float stored_depth = texelFetch(sampler2D(gi_history_depth_prev, linear_sampler), tap, 0).r;
					if (stored_depth <= 0.0 || abs(stored_depth - expected_depth) >= expected_depth * 0.05) {
						continue; // Nothing was stored here yet, or it belongs to another surface.
					}
					history_ambient += tap_weights[t] * texelFetch(sampler2D(gi_history_ambient_prev, linear_sampler), tap, 0);
					history_reflection += tap_weights[t] * texelFetch(sampler2D(gi_history_reflection_prev, linear_sampler), tap, 0);
					weight_sum += tap_weights[t];
				}

				if (weight_sum > 0.0) {
					history_valid = true;
					history_ambient /= weight_sum;
					history_reflection /= weight_sum;
				}
			}
		}
	}

	// Trace only this frame's half of the checkerboard, so the cone tracing is spread over
	// two frames. A pixel whose history is missing or was rejected is always traced, so the
	// first frame and every disocclusion are correct rather than merely cheap.
	//
	// Two phases rather than four on purpose. Reuse chains: a pixel that takes history is
	// reusing a value that may itself have been reused, and every link resamples it at a
	// fractional offset, so a sharp feature creeps along the direction of travel and smears.
	// Measured against the same camera path traced in full, going from four phases to two
	// cut the pixels differing by more than 40/255 from 0.31% to 0.05%, for half the saving
	// instead of three quarters.
	uint slot = uint((out_pos.x + out_pos.y) & 1);
	bool trace = !params.temporal_enabled || !history_valid || slot == params.trace_slot;

	bool screen_probes = false;
	// What the history keeps of the ambient light: all of it, but with screen probes the alpha, which
	// the lighting takes from the ambient buffer and is worked out afresh every frame, is how many
	// frames it averages instead.
	vec4 ambient_history = vec4(0.0);
#ifdef USE_SDFGI
	screen_probes = params.screen_probe_grid.x > 0;
	if (screen_probes) {
		// Screen probes: the diffuse light of every pixel comes from the probes around it, every
		// frame, averaged with its history. Only the reflections are traced on the checkerboard.
		vec3 world_vertex = mat3(scene_data.cam_transform) * vertex;
		vec3 world_normal = normalize(mat3(scene_data.cam_transform) * fetch_normal_and_roughness(pos).xyz);
		vec3 probe_light;
		float coverage = screen_probe_gather(pos, world_vertex, world_normal, linear_depth, probe_light);
		if (trace || coverage < 1.0) {
			// Where the probes around do not fit the pixel well enough, the SDFGI probes make up
			// the difference.
			process_gi(pos, vertex, ambient_light, reflection_light);
		}
		ambient_light.rgb = mix(ambient_light.rgb, probe_light, coverage);

		// The light each frame gathers is noisy, and each pixel averages it over up to
		// 1 / screen_probe_blend frames, fewer while its history is young (after a disocclusion),
		// so that it settles quickly and then keeps still. Light that changes suddenly and by
		// much (switched on or off) cuts the history short to catch up within a few frames, and
		// keeps it short while it goes on changing that way (as the SDFGI probes, and the light
		// they bounce, settle behind it). It takes two frames in a row of such a change to tell it
		// from the odd frame whose probes happened to land badly for the pixel (at the edge of an
		// object far away, which no probe of its own covers in every frame). Light that changes
		// gradually is only ever followed at the full length: steady rather than quick.
		float frames = 1.0;
		float state = 0.0;
		if (history_valid) {
			frames = floor(history_ambient.a);
			float history_state = history_ambient.a - frames;
			float current = dot(ambient_light.rgb, vec3(0.2126, 0.7152, 0.0722));
			float previous = dot(history_ambient.rgb, vec3(0.2126, 0.7152, 0.0722));
			float change = current - previous;
			float scale = max(current, previous);
			bool sudden = abs(change) > scale * SCREEN_PROBE_CHANGE_RELATIVE + SCREEN_PROBE_CHANGE_ABSOLUTE;
			bool going_on = abs(change) > scale * SCREEN_PROBE_TREND_RELATIVE + SCREEN_PROBE_CHANGE_ABSOLUTE;

			if (sudden && abs(history_state - SCREEN_PROBE_STATE_SUSPECT) < 0.125) {
				frames = min(frames, SCREEN_PROBE_CHANGE_FRAMES);
				state = change < 0.0 ? SCREEN_PROBE_STATE_FALLING : SCREEN_PROBE_STATE_RISING;
			} else if (going_on && abs(history_state - (change < 0.0 ? SCREEN_PROBE_STATE_FALLING : SCREEN_PROBE_STATE_RISING)) < 0.125) {
				frames = min(frames, SCREEN_PROBE_CHANGE_FRAMES);
				state = history_state;
			} else if (sudden) {
				state = SCREEN_PROBE_STATE_SUSPECT;
			}

			frames = min(frames + 1.0, 1.0 / params.screen_probe_blend);
			ambient_light.rgb = mix(history_ambient.rgb, ambient_light.rgb, 1.0 / frames);
			reflection_light = trace ? mix(history_reflection, reflection_light, params.temporal_blend) : history_reflection;
		}
		ambient_light.a = sdfgi_ambient_alpha(world_vertex);
		// Where it is in that, in the fraction of the frame count.
		ambient_history = vec4(ambient_light.rgb, frames + state);
	}
#endif

	if (screen_probes) {
		// Done above.
	} else if (trace) {
		process_gi(pos, vertex, ambient_light, reflection_light);
		if (history_valid) {
			ambient_light = mix(history_ambient, ambient_light, params.temporal_blend);
			reflection_light = mix(history_reflection, reflection_light, params.temporal_blend);
		}
	} else {
		ambient_light = history_ambient;
		reflection_light = history_reflection;
	}

	pos = out_pos;

	imageStore(ambient_buffer, pos, ambient_light);
	imageStore(reflection_buffer, pos, reflection_light);

	if (params.temporal_enabled) {
		imageStore(gi_history_ambient, pos, screen_probes ? ambient_history : ambient_light);
		imageStore(gi_history_reflection, pos, reflection_light);
		imageStore(gi_history_depth, pos, vec4(linear_depth));
	}

#ifdef USE_VRS
	if (sc_use_vrs) {
		if (vrs_x > 1) {
			imageStore(ambient_buffer, pos + ivec2(1, 0), ambient_light);
			imageStore(reflection_buffer, pos + ivec2(1, 0), reflection_light);
		}

		if (vrs_x > 2) {
			imageStore(ambient_buffer, pos + ivec2(2, 0), ambient_light);
			imageStore(reflection_buffer, pos + ivec2(2, 0), reflection_light);

			imageStore(ambient_buffer, pos + ivec2(3, 0), ambient_light);
			imageStore(reflection_buffer, pos + ivec2(3, 0), reflection_light);
		}

		if (vrs_y > 1) {
			imageStore(ambient_buffer, pos + ivec2(0, 1), ambient_light);
			imageStore(reflection_buffer, pos + ivec2(0, 1), reflection_light);
		}

		if (vrs_y > 1 && vrs_x > 1) {
			imageStore(ambient_buffer, pos + ivec2(1, 1), ambient_light);
			imageStore(reflection_buffer, pos + ivec2(1, 1), reflection_light);
		}

		if (vrs_y > 1 && vrs_x > 2) {
			imageStore(ambient_buffer, pos + ivec2(2, 1), ambient_light);
			imageStore(reflection_buffer, pos + ivec2(2, 1), reflection_light);

			imageStore(ambient_buffer, pos + ivec2(3, 1), ambient_light);
			imageStore(reflection_buffer, pos + ivec2(3, 1), reflection_light);
		}

		if (vrs_y > 2) {
			imageStore(ambient_buffer, pos + ivec2(0, 2), ambient_light);
			imageStore(reflection_buffer, pos + ivec2(0, 2), reflection_light);
			imageStore(ambient_buffer, pos + ivec2(0, 3), ambient_light);
			imageStore(reflection_buffer, pos + ivec2(0, 3), reflection_light);
		}

		if (vrs_y > 2 && vrs_x > 1) {
			imageStore(ambient_buffer, pos + ivec2(1, 2), ambient_light);
			imageStore(reflection_buffer, pos + ivec2(1, 2), reflection_light);
			imageStore(ambient_buffer, pos + ivec2(1, 3), ambient_light);
			imageStore(reflection_buffer, pos + ivec2(1, 3), reflection_light);
		}

		if (vrs_y > 2 && vrs_x > 2) {
			imageStore(ambient_buffer, pos + ivec2(2, 2), ambient_light);
			imageStore(reflection_buffer, pos + ivec2(2, 2), reflection_light);
			imageStore(ambient_buffer, pos + ivec2(2, 3), ambient_light);
			imageStore(reflection_buffer, pos + ivec2(2, 3), reflection_light);

			imageStore(ambient_buffer, pos + ivec2(3, 2), ambient_light);
			imageStore(reflection_buffer, pos + ivec2(3, 2), reflection_light);
			imageStore(ambient_buffer, pos + ivec2(3, 3), ambient_light);
			imageStore(reflection_buffer, pos + ivec2(3, 3), reflection_light);
		}
	}
#endif
}

#endif // MODE_SCREEN_PROBE_TRACE / MODE_SCREEN_PROBE_FILTER
