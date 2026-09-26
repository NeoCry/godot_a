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
	uint pad4;

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
	float pad2;
	float pad3;
}
params;

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
			ivec3 occ_indexv = abs((sdfgi.cascades[cascade].probe_world_offset + probe_posi) & ivec3(1, 1, 1)) * ivec3(1, 2, 4);
			vec4 occ_mask = mix(vec4(0.0), vec4(1.0), equal(ivec4(occ_indexv.x | occ_indexv.y), ivec4(0, 1, 2, 3)));

			vec3 occ_pos = clamp(cascade_pos, probe_pos - sdfgi.occlusion_clamp, probe_pos + sdfgi.occlusion_clamp) * sdfgi.probe_to_uvw;
			occ_pos.z += float(cascade);
			if (occ_indexv.z != 0) { //z bit is on, means index is >=4, so make it switch to the other half of textures
				occ_pos.x += 1.0;
			}

			occ_pos *= sdfgi.occlusion_renormalize;
			float occlusion = dot(textureLod(sampler3D(occlusion_texture, linear_sampler), occ_pos, 0.0), occ_mask);
			// Crush the sliver of visibility that filtering gives a probe hidden a voxel away (as
			// DDGI does with its weights): next to the geometry hiding a much brighter probe, even
			// that much of it outweighs the probes the point really sees.
			occlusion = occlusion < 0.2 ? occlusion * occlusion * occlusion * 25.0 : occlusion;

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

vec4 fetch_normal_and_roughness(ivec2 pos) {
	vec4 normal_roughness = texelFetch(sampler2D(normal_roughness_buffer, linear_sampler), pos, 0);
	normal_roughness.xyz = normalize(normal_roughness.xyz * 2.0 - 1.0);
	return normal_roughness;
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

	if (trace) {
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
		imageStore(gi_history_ambient, pos, ambient_light);
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
