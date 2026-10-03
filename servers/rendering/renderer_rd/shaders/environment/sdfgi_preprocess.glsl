#[compute]

#version 450

#VERSION_DEFINES

#ifdef MODE_JUMPFLOOD_OPTIMIZED
#define GROUP_SIZE 8

layout(local_size_x = GROUP_SIZE, local_size_y = GROUP_SIZE, local_size_z = GROUP_SIZE) in;

#elif defined(MODE_OCCLUSION) || defined(MODE_SCROLL) || defined(MODE_BOX_CARRY)
//buffer layout
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

#else
//grid layout
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

#endif

#if defined(MODE_INITIALIZE_JUMP_FLOOD) || defined(MODE_INITIALIZE_JUMP_FLOOD_HALF)
layout(r16ui, set = 0, binding = 1) uniform restrict readonly uimage3D src_color;
layout(rgba8ui, set = 0, binding = 2) uniform restrict writeonly uimage3D dst_positions;
#endif

#ifdef MODE_UPSCALE_JUMP_FLOOD
layout(r16ui, set = 0, binding = 1) uniform restrict readonly uimage3D src_color;
layout(rgba8ui, set = 0, binding = 2) uniform restrict readonly uimage3D src_positions_half;
layout(rgba8ui, set = 0, binding = 3) uniform restrict writeonly uimage3D dst_positions;
#endif

#if defined(MODE_JUMPFLOOD) || defined(MODE_JUMPFLOOD_OPTIMIZED)
layout(rgba8ui, set = 0, binding = 1) uniform restrict readonly uimage3D src_positions;
layout(rgba8ui, set = 0, binding = 2) uniform restrict writeonly uimage3D dst_positions;
#endif

#ifdef MODE_JUMPFLOOD_OPTIMIZED

shared uvec4 group_positions[(GROUP_SIZE + 2) * (GROUP_SIZE + 2) * (GROUP_SIZE + 2)]; //4x4x4 with margins

void group_store(ivec3 p_pos, uvec4 p_value) {
	uint offset = uint(p_pos.z * (GROUP_SIZE + 2) * (GROUP_SIZE + 2) + p_pos.y * (GROUP_SIZE + 2) + p_pos.x);
	group_positions[offset] = p_value;
}

uvec4 group_load(ivec3 p_pos) {
	uint offset = uint(p_pos.z * (GROUP_SIZE + 2) * (GROUP_SIZE + 2) + p_pos.y * (GROUP_SIZE + 2) + p_pos.x);
	return group_positions[offset];
}

#endif

#ifdef MODE_OCCLUSION

layout(r16ui, set = 0, binding = 1) uniform restrict readonly uimage3D src_color;
layout(r8, set = 0, binding = 2) uniform restrict image3D dst_occlusion[8];
layout(r32ui, set = 0, binding = 3) uniform restrict readonly uimage3D src_facing;

#define OCC_REGION_SIZE (OCCLUSION_SIZE * 2)

// Where each probe was placed (see MODE_PROBE_PLACEMENT): xyz its offset from the grid in voxels,
// w whether it is usable at all.
layout(rgba16f, set = 0, binding = 4) uniform restrict readonly image2DArray probe_state;

shared uint occlusion_facing[(OCC_REGION_SIZE * OCC_REGION_SIZE * OCC_REGION_SIZE) / 4];

uint get_facing(ivec3 p_pos) {
	uint ofs = uint(p_pos.z * OCC_REGION_SIZE * OCC_REGION_SIZE + p_pos.y * OCC_REGION_SIZE + p_pos.x);
	uint v = occlusion_facing[ofs / 4];
	return (v >> ((ofs % 4) * 8)) & 0xFF;
}

// Whether the probe at p_to (region voxel coordinates, where voxel v spans [v, v + 1)) can see
// the point p_from, walking the voxels in between (Amanatides & Woo) and stopping at the first
// solid one. The voxel p_from lies in is not tested, the one the probe is reached through is: a
// probe hidden in geometry on that side cannot see past it.
float occlusion_trace(vec3 p_from, vec3 p_to) {
	vec3 ray = p_to - p_from;
	ivec3 cell = ivec3(floor(p_from));
	ivec3 cell_step = ivec3(sign(ray));
	vec3 t_delta = 1.0 / max(abs(ray), vec3(1e-6));
	vec3 t_max = abs(vec3(cell) + max(vec3(cell_step), vec3(0.0)) - p_from) * t_delta;

	// A segment between two points of the region can cross at most this many voxel boundaries.
	for (int i = 0; i < OCC_REGION_SIZE * 3; i++) {
		if (min(t_max.x, min(t_max.y, t_max.z)) >= 1.0 - 1e-4) {
			return 1.0; // Reached the probe without meeting anything solid.
		}
		if (t_max.x <= t_max.y && t_max.x <= t_max.z) {
			cell.x += cell_step.x;
			t_max.x += t_delta.x;
		} else if (t_max.y <= t_max.z) {
			cell.y += cell_step.y;
			t_max.y += t_delta.y;
		} else {
			cell.z += cell_step.z;
			t_max.z += t_delta.z;
		}
		if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, ivec3(OCC_REGION_SIZE)))) {
			return 1.0;
		}
		if (get_facing(cell) != 0) {
			return 0.0;
		}
	}
	return 1.0;
}

#endif

#ifdef MODE_STORE

layout(rgba8ui, set = 0, binding = 1) uniform restrict readonly uimage3D src_positions;
layout(r16ui, set = 0, binding = 2) uniform restrict readonly uimage3D src_albedo;
layout(r8, set = 0, binding = 3) uniform restrict readonly image3D src_occlusion[8];
layout(r32ui, set = 0, binding = 4) uniform restrict readonly uimage3D src_light;
layout(r32ui, set = 0, binding = 5) uniform restrict readonly uimage3D src_light_aniso;
layout(r32ui, set = 0, binding = 6) uniform restrict readonly uimage3D src_facing;

layout(r8, set = 0, binding = 7) uniform restrict image3D dst_sdf;
layout(r16ui, set = 0, binding = 8) uniform restrict writeonly uimage3D dst_occlusion;

layout(set = 0, binding = 10, std430) restrict buffer DispatchData {
	uint x;
	uint y;
	uint z;
	uint total_count;
}
dispatch_data;

struct ProcessVoxel {
	uint position; // xyz 7 bit packed, extra 11 bits for neighbors.
	uint albedo; //rgb bits 0-15 albedo, bits 16-21 are normal bits (set if geometry exists toward that side), extra 11 bits for neighbors
	uint light; //rgbe8985 encoded total saved light, extra 2 bits for neighbors
	uint light_aniso; //55555 light anisotropy, extra 2 bits for neighbors
	//total neighbors: 26
};

layout(set = 0, binding = 11, std430) restrict buffer writeonly ProcessVoxels {
	ProcessVoxel data[];
}
dst_process_voxels;

shared ProcessVoxel store_positions[4 * 4 * 4];
shared uint store_position_count;
shared uint store_from_index;

// The cascade's light, cleared around the box when only the cells around it are updated.
layout(r32ui, set = 0, binding = 12) uniform restrict writeonly uimage3D dst_light;
layout(rgba8, set = 0, binding = 13) uniform restrict writeonly image3D dst_aniso0;
layout(rg8, set = 0, binding = 14) uniform restrict writeonly image3D dst_aniso1;
#endif

#ifdef MODE_SCROLL

layout(r16ui, set = 0, binding = 1) uniform restrict writeonly uimage3D dst_albedo;
layout(r32ui, set = 0, binding = 2) uniform restrict writeonly uimage3D dst_facing;
layout(r32ui, set = 0, binding = 3) uniform restrict writeonly uimage3D dst_light;
layout(r32ui, set = 0, binding = 4) uniform restrict writeonly uimage3D dst_light_aniso;

layout(set = 0, binding = 5, std430) restrict buffer readonly DispatchData {
	uint x;
	uint y;
	uint z;
	uint total_count;
}
dispatch_data;

struct ProcessVoxel {
	uint position; // xyz 7 bit packed, extra 11 bits for neighbors.
	uint albedo; //rgb bits 0-15 albedo, bits 16-21 are normal bits (set if geometry exists toward that side), extra 11 bits for neighbors
	uint light; //rgbe8985 encoded total saved light, extra 2 bits for neighbors
	uint light_aniso; //55555 light anisotropy, extra 2 bits for neighbors
	//total neighbors: 26
};

layout(set = 0, binding = 6, std430) restrict buffer readonly ProcessVoxels {
	ProcessVoxel data[];
}
src_process_voxels;

#endif

#ifdef MODE_SCROLL_OCCLUSION

layout(r8, set = 0, binding = 1) uniform restrict image3D dst_occlusion[8];
layout(r16ui, set = 0, binding = 2) uniform restrict readonly uimage3D src_occlusion;

#endif

#ifdef MODE_BOX_LOWER_SDF

layout(r8, set = 0, binding = 1) uniform restrict image3D dst_sdf;

#endif

#ifdef MODE_BOX_CARRY

layout(r16ui, set = 0, binding = 1) uniform restrict writeonly uimage3D dst_albedo;
layout(r32ui, set = 0, binding = 2) uniform restrict writeonly uimage3D dst_facing;
layout(r32ui, set = 0, binding = 3) uniform restrict writeonly uimage3D dst_light;
layout(r32ui, set = 0, binding = 4) uniform restrict writeonly uimage3D dst_light_aniso;

// How many voxels the list held before this update.
layout(set = 0, binding = 5, std430) restrict buffer readonly SrcDispatchData {
	uint x;
	uint y;
	uint z;
	uint total_count;
}
src_dispatch_data;

struct ProcessVoxel {
	uint position; // xyz 7 bit packed, extra 11 bits for neighbors.
	uint albedo; //rgb bits 0-15 albedo, bits 16-21 are normal bits (set if geometry exists toward that side), extra 11 bits for neighbors
	uint light; //rgbe8985 encoded total saved light, extra 2 bits for neighbors
	uint light_aniso; //55555 light anisotropy, extra 2 bits for neighbors
	//total neighbors: 26
};

// A copy of the list before this update.
layout(set = 0, binding = 6, std430) restrict buffer readonly SrcProcessVoxels {
	ProcessVoxel data[];
}
src_process_voxels;

// The list being rebuilt (MODE_STORE adds the voxels around the box after this).
layout(set = 0, binding = 7, std430) restrict buffer DispatchData {
	uint x;
	uint y;
	uint z;
	uint total_count;
}
dispatch_data;

layout(set = 0, binding = 8, std430) restrict buffer writeonly ProcessVoxels {
	ProcessVoxel data[];
}
dst_process_voxels;

shared uint carry_count;
shared uint carry_from_index;

#endif

layout(push_constant, std430) uniform Params {
	ivec3 scroll;

	int grid_size;

	ivec3 probe_offset;
	int step_size;

	bool half_size;
	uint occlusion_index;
	int cascade;
	float min_distance; // MODE_PROBE_PLACEMENT: clearance to keep probes at, in voxels.

	// Cells voxelized again this frame for dynamic objects (empty when box_from == box_to).
	ivec3 box_from;
	uint pad;
	ivec3 box_to;
	uint pad2;

	// The cells this pass works on, at the resolution it works at (see GI::SDFGI::_update_box()).
	ivec3 region_from;
	bool box_update; // Only the cells around the box are updated, the rest of the cascade is kept.
	ivec3 region_to;
	uint pad3;
}
params;

// Around the box of a box update (see GI::SDFGI::_update_box()), the voxels within
// BOX_VOXEL_MARGIN are put in the list again, the distance is computed again within
// BOX_SDF_MARGIN (both defined by GI), and the light is cleared within this.
#define BOX_LIGHT_MARGIN 1

bool in_box(ivec3 p_pos, int p_margin) {
	return all(greaterThanEqual(p_pos, params.box_from - ivec3(p_margin))) && all(lessThan(p_pos, params.box_to + ivec3(p_margin)));
}

bool in_region(ivec3 p_pos) {
	return all(greaterThanEqual(p_pos, params.region_from)) && all(lessThan(p_pos, params.region_to));
}

// The cells whose occlusion a box update computes again (those of the probes around the box,
// see MODE_OCCLUSION), and whose geometry the probe placement around the box looks at.
bool in_box_occlusion_region(ivec3 p_pos) {
	ivec3 from = (params.box_from / OCCLUSION_SIZE) * OCCLUSION_SIZE - ivec3(OCCLUSION_SIZE);
	ivec3 to = ((params.box_to + ivec3(OCCLUSION_SIZE - 1)) / OCCLUSION_SIZE) * OCCLUSION_SIZE + ivec3(OCCLUSION_SIZE);
	return all(greaterThanEqual(p_pos, from)) && all(lessThan(p_pos, to));
}

#ifdef MODE_PROBE_PLACEMENT

layout(r32ui, set = 0, binding = 1) uniform restrict readonly uimage3D src_facing;
layout(rgba16f, set = 0, binding = 2) uniform restrict writeonly image2DArray dst_probe_state;

// A probe whose rays mostly hit the back of surfaces is inside geometry (DDGI uses the same test).
#define PLACEMENT_INSIDE_RATIO 0.25
#define PLACEMENT_RAYS 64
#define PLACEMENT_CANDIDATE_RAYS 16
// How far a probe may be moved, as a fraction of the probe spacing (per axis).
#define PLACEMENT_MAX_OFFSET 0.45

const float PI = 3.14159265f;
const float GOLDEN_ANGLE = PI * (3.0 - sqrt(5.0));

const ivec3 facing_dirs[6] = ivec3[](ivec3(1, 0, 0), ivec3(0, 1, 0), ivec3(0, 0, 1), ivec3(-1, 0, 0), ivec3(0, -1, 0), ivec3(0, 0, -1));

vec3 spherical_fibonacci(uint p_index, uint p_count) {
	float z = 1.0 - (2.0 * float(p_index) + 1.0) / float(p_count);
	float r = sqrt(max(0.0, 1.0 - z * z));
	float phi = float(p_index) * GOLDEN_ANGLE;
	return vec3(r * cos(phi), r * sin(phi), z);
}

uint placement_facing(ivec3 p_cell) {
	if (any(lessThan(p_cell, ivec3(0))) || any(greaterThanEqual(p_cell, ivec3(params.grid_size)))) {
		return 0; // Nothing is known outside the cascade.
	}
	return imageLoad(src_facing, p_cell).r;
}

// Walks the voxels along p_dir from p_from (Amanatides & Woo) until p_max_dist, returning the
// distance at which the first solid voxel is entered (-1 when there is none), and its facing bits.
float placement_trace(vec3 p_from, vec3 p_dir, float p_max_dist, out uint r_facing) {
	r_facing = 0;
	ivec3 cell = ivec3(floor(p_from));
	ivec3 cell_step = ivec3(sign(p_dir));
	vec3 t_delta = 1.0 / max(abs(p_dir), vec3(1e-6));
	vec3 t_max = abs(vec3(cell) + max(vec3(cell_step), vec3(0.0)) - p_from) * t_delta;

	for (int i = 0; i < OCCLUSION_SIZE * 8; i++) {
		float t = min(t_max.x, min(t_max.y, t_max.z));
		if (t >= p_max_dist) {
			return -1.0;
		}
		if (t_max.x <= t_max.y && t_max.x <= t_max.z) {
			cell.x += cell_step.x;
			t_max.x += t_delta.x;
		} else if (t_max.y <= t_max.z) {
			cell.y += cell_step.y;
			t_max.y += t_delta.y;
		} else {
			cell.z += cell_step.z;
			t_max.z += t_delta.z;
		}
		uint facing = placement_facing(cell);
		if (facing != 0) {
			r_facing = facing;
			return t;
		}
	}
	return -1.0;
}

struct PlacementSample {
	float backface_ratio; // Share of the rays whose first hit is the back of a surface.
	float clearance; // How close the nearest hit is, measured the way the probe rays are biased (L-infinity).
};

// What a probe at p_pos would see of the geometry around it, within the probe's occlusion
// region: rays stop at its boundary (p_region_min, p_region_max), so the result only depends on
// geometry that the occlusion pass also looks at for this probe.
PlacementSample placement_evaluate(vec3 p_pos, vec3 p_region_min, vec3 p_region_max, uint p_ray_count) {
	PlacementSample s;
	if (placement_facing(ivec3(floor(p_pos))) != 0) {
		s.backface_ratio = 1.0; // In a solid voxel: as inside as it gets.
		s.clearance = 0.0;
		return s;
	}

	float backfaces = 0.0;
	s.clearance = 1e10;
	for (uint i = 0; i < p_ray_count; i++) {
		vec3 dir = spherical_fibonacci(i, p_ray_count);
		vec3 exit = max((p_region_min - p_pos) / dir, (p_region_max - p_pos) / dir);
		float max_dist = min(exit.x, min(exit.y, exit.z));

		uint facing;
		float t = placement_trace(p_pos, dir, max_dist, facing);
		if (t < 0.0) {
			continue;
		}

		// A front face has a normal pointing back towards the ray. Thin geometry is marked as
		// facing both ways, and counts as front from either side.
		bool front = false;
		for (int k = 0; k < 6; k++) {
			if (bool(facing & (1 << k)) && dot(vec3(facing_dirs[k]), dir) < 0.0) {
				front = true;
			}
		}
		if (!front) {
			backfaces += 1.0;
		}
		s.clearance = min(s.clearance, t * max(abs(dir.x), max(abs(dir.y), abs(dir.z))));
	}
	s.backface_ratio = backfaces / float(p_ray_count);
	return s;
}

// Whether nothing solid lies on the segment between two points (the voxel p_from is in excluded).
bool placement_segment_clear(vec3 p_from, vec3 p_to) {
	vec3 ray = p_to - p_from;
	float len = length(ray);
	if (len < 1e-4) {
		return true;
	}
	uint facing;
	return placement_trace(p_from, ray / len, len, facing) < 0.0 && placement_facing(ivec3(floor(p_to))) == 0;
}

// Lower is better: never inside, then as few backfaces and as much clearance as possible,
// and among equals, the least movement.
float placement_score(PlacementSample p_sample, float p_offset_length) {
	float inside = p_sample.backface_ratio > PLACEMENT_INSIDE_RATIO ? 100.0 : 0.0;
	return inside + p_sample.backface_ratio * 4.0 + max(0.0, params.min_distance - p_sample.clearance) * 2.0 + p_offset_length * 0.05;
}

#endif

void main() {
#ifdef MODE_SCROLL

	// Pixel being shaded
	int index = int(gl_GlobalInvocationID.x);
	if (index >= dispatch_data.total_count) { //too big
		return;
	}

	ivec3 read_pos = (ivec3(src_process_voxels.data[index].position) >> ivec3(0, 7, 14)) & ivec3(0x7F);
	ivec3 write_pos = read_pos + params.scroll;

	if (any(lessThan(write_pos, ivec3(0))) || any(greaterThanEqual(write_pos, ivec3(params.grid_size)))) {
		return; // Fits outside the 3D texture, don't do anything.
	}

	if (all(greaterThanEqual(write_pos, params.box_from)) && all(lessThan(write_pos, params.box_to))) {
		return; // Voxelized again this frame (a dynamic object moved there): what it held is out of date.
	}

	uint albedo = ((src_process_voxels.data[index].albedo & 0x7FFF) << 1) | 1; //add solid bit
	imageStore(dst_albedo, write_pos, uvec4(albedo));

	uint facing = (src_process_voxels.data[index].albedo >> 15) & 0x3F; //6 anisotropic facing bits
	imageStore(dst_facing, write_pos, uvec4(facing));

	uint light = src_process_voxels.data[index].light & 0x3fffffff; //30 bits of RGBE8985
	imageStore(dst_light, write_pos, uvec4(light));

	uint light_aniso = src_process_voxels.data[index].light_aniso & 0x3fffffff; //30 bits of 6 anisotropic 5 bits values
	imageStore(dst_light_aniso, write_pos, uvec4(light_aniso));

#endif

#ifdef MODE_SCROLL_OCCLUSION

	// The region is the cells left after the scroll (or those around the box in a box update).
	ivec3 pos = ivec3(gl_GlobalInvocationID.xyz) + params.region_from;
	if (any(greaterThanEqual(pos, params.region_to))) { //too large, do nothing
		return;
	}

	ivec3 read_pos = pos + max(ivec3(0), -params.scroll);
	ivec3 write_pos = pos + max(ivec3(0), params.scroll);

	read_pos.z += params.cascade * params.grid_size;
	uint occlusion = imageLoad(src_occlusion, read_pos).r;
	read_pos.x += params.grid_size;
	occlusion |= imageLoad(src_occlusion, read_pos).r << 16;

	const uint occlusion_shift[8] = uint[](4, 8, 12, 0, 20, 24, 28, 16); // Channel i lands in r, g, b, a of B4G4R4A4 (see create()).

	for (uint i = 0; i < 8; i++) {
		float o = float((occlusion >> occlusion_shift[i]) & 0xF) / 15.0;
		imageStore(dst_occlusion[i], write_pos, vec4(o));
	}

#endif

#ifdef MODE_INITIALIZE_JUMP_FLOOD

	ivec3 pos = ivec3(gl_GlobalInvocationID.xyz) + params.region_from;
	if (any(greaterThanEqual(pos, params.region_to))) {
		return;
	}

	uint c = imageLoad(src_color, pos).r;
	uvec4 v;
	if (bool(c & 0x1)) {
		//bit set means this is solid
		v.xyz = uvec3(pos);
		v.w = 255; //not zero means used
	} else {
		v.xyz = uvec3(0);
		v.w = 0; // zero means unused
	}

	imageStore(dst_positions, pos, v);
#endif

#ifdef MODE_INITIALIZE_JUMP_FLOOD_HALF

	ivec3 pos = ivec3(gl_GlobalInvocationID.xyz) + params.region_from;
	if (any(greaterThanEqual(pos, params.region_to))) {
		return;
	}
	ivec3 base_pos = pos * 2;

	//since we store in half size, lets kind of randomize what we store, so
	//the half size jump flood has a bit better chance to find something
	uvec4 closest[8];
	int closest_count = 0;

	for (uint i = 0; i < 8; i++) {
		ivec3 src_pos = base_pos + ((ivec3(i) >> ivec3(0, 1, 2)) & ivec3(1, 1, 1));
		uint c = imageLoad(src_color, src_pos).r;
		if (bool(c & 1)) {
			uvec4 v = uvec4(uvec3(src_pos), 255);
			closest[closest_count] = v;
			closest_count++;
		}
	}

	if (closest_count == 0) {
		imageStore(dst_positions, pos, uvec4(0));
	} else {
		ivec3 indexv = (pos & ivec3(1, 1, 1)) * ivec3(1, 2, 4);
		int index = (indexv.x | indexv.y | indexv.z) % closest_count;
		imageStore(dst_positions, pos, closest[index]);
	}

#endif

#ifdef MODE_JUMPFLOOD

	//regular jumpflood, efficient for large steps, inefficient for small steps
	ivec3 pos = ivec3(gl_GlobalInvocationID.xyz) + params.region_from;
	if (any(greaterThanEqual(pos, params.region_to))) {
		return;
	}

	vec3 posf = vec3(pos);

	if (params.half_size) {
		posf = posf * 2.0 + 0.5;
	}

	uvec4 p = imageLoad(src_positions, pos);

	if (!params.half_size && p == uvec4(uvec3(pos), 255)) {
		imageStore(dst_positions, pos, p);
		return; //points to itself and valid, nothing better can be done, just pass
	}

	float p_dist;

	if (p.w != 0) {
		p_dist = distance(posf, vec3(p.xyz));
	} else {
		p_dist = 0.0; //should not matter
	}

	const uint offset_count = 26;
	const ivec3 offsets[offset_count] = ivec3[](
			ivec3(-1, -1, -1),
			ivec3(-1, -1, 0),
			ivec3(-1, -1, 1),
			ivec3(-1, 0, -1),
			ivec3(-1, 0, 0),
			ivec3(-1, 0, 1),
			ivec3(-1, 1, -1),
			ivec3(-1, 1, 0),
			ivec3(-1, 1, 1),
			ivec3(0, -1, -1),
			ivec3(0, -1, 0),
			ivec3(0, -1, 1),
			ivec3(0, 0, -1),
			ivec3(0, 0, 1),
			ivec3(0, 1, -1),
			ivec3(0, 1, 0),
			ivec3(0, 1, 1),
			ivec3(1, -1, -1),
			ivec3(1, -1, 0),
			ivec3(1, -1, 1),
			ivec3(1, 0, -1),
			ivec3(1, 0, 0),
			ivec3(1, 0, 1),
			ivec3(1, 1, -1),
			ivec3(1, 1, 0),
			ivec3(1, 1, 1));

	for (uint i = 0; i < offset_count; i++) {
		ivec3 ofs = pos + offsets[i] * params.step_size;
		if (!in_region(ofs)) {
			continue;
		}
		uvec4 q = imageLoad(src_positions, ofs);

		if (q.w == 0) {
			continue; //was not initialized yet, ignore
		}

		float q_dist = distance(posf, vec3(q.xyz));
		if (p.w == 0 || q_dist < p_dist) {
			p = q; //just replace because current is unused
			p_dist = q_dist;
		}
	}

	imageStore(dst_positions, pos, p);
#endif

#ifdef MODE_JUMPFLOOD_OPTIMIZED
	//optimized version using shared compute memory

	ivec3 group_offset = ivec3(gl_WorkGroupID.xyz) % params.step_size;
	ivec3 group_pos = params.region_from + group_offset + (ivec3(gl_WorkGroupID.xyz) / params.step_size) * ivec3(GROUP_SIZE * params.step_size);

	//load data into local group memory

	if (all(lessThan(ivec3(gl_LocalInvocationID.xyz), ivec3((GROUP_SIZE + 2) / 2)))) {
		//use this thread for loading, this method uses less threads for this but its simpler and less divergent
		ivec3 base_pos = ivec3(gl_LocalInvocationID.xyz) * 2;
		for (uint i = 0; i < 8; i++) {
			ivec3 load_pos = base_pos + ((ivec3(i) >> ivec3(0, 1, 2)) & ivec3(1, 1, 1));
			ivec3 load_global_pos = group_pos + (load_pos - ivec3(1)) * params.step_size;
			uvec4 q;
			if (in_region(load_global_pos)) {
				q = imageLoad(src_positions, load_global_pos);
			} else {
				q = uvec4(0); //unused
			}

			group_store(load_pos, q);
		}
	}

	//sync
	groupMemoryBarrier();
	barrier();

	ivec3 global_pos = group_pos + ivec3(gl_LocalInvocationID.xyz) * params.step_size;

	if (!in_region(global_pos)) {
		return; //do nothing else, end here because outside range
	}

	ivec3 local_pos = ivec3(gl_LocalInvocationID.xyz) + ivec3(1);

	const uint offset_count = 27;
	const ivec3 offsets[offset_count] = ivec3[](
			ivec3(-1, -1, -1),
			ivec3(-1, -1, 0),
			ivec3(-1, -1, 1),
			ivec3(-1, 0, -1),
			ivec3(-1, 0, 0),
			ivec3(-1, 0, 1),
			ivec3(-1, 1, -1),
			ivec3(-1, 1, 0),
			ivec3(-1, 1, 1),
			ivec3(0, -1, -1),
			ivec3(0, -1, 0),
			ivec3(0, -1, 1),
			ivec3(0, 0, -1),
			ivec3(0, 0, 0),
			ivec3(0, 0, 1),
			ivec3(0, 1, -1),
			ivec3(0, 1, 0),
			ivec3(0, 1, 1),
			ivec3(1, -1, -1),
			ivec3(1, -1, 0),
			ivec3(1, -1, 1),
			ivec3(1, 0, -1),
			ivec3(1, 0, 0),
			ivec3(1, 0, 1),
			ivec3(1, 1, -1),
			ivec3(1, 1, 0),
			ivec3(1, 1, 1));

	//only makes sense if point is inside screen
	uvec4 closest = uvec4(0);
	float closest_dist = 0.0;

	vec3 posf = vec3(global_pos);

	if (params.half_size) {
		posf = posf * 2.0 + 0.5;
	}

	for (uint i = 0; i < offset_count; i++) {
		uvec4 point = group_load(local_pos + offsets[i]);

		if (point.w == 0) {
			continue; //was not initialized yet, ignore
		}

		float dist = distance(posf, vec3(point.xyz));
		if (closest.w == 0 || dist < closest_dist) {
			closest = point;
			closest_dist = dist;
		}
	}

	imageStore(dst_positions, global_pos, closest);

#endif

#ifdef MODE_UPSCALE_JUMP_FLOOD

	ivec3 pos = ivec3(gl_GlobalInvocationID.xyz) + params.region_from;
	if (any(greaterThanEqual(pos, params.region_to))) {
		return;
	}

	uint c = imageLoad(src_color, pos).r;
	uvec4 v;
	if (bool(c & 1)) {
		//bit set means this is solid
		v.xyz = uvec3(pos);
		v.w = 255; //not zero means used
	} else {
		v = imageLoad(src_positions_half, pos >> 1);
		float d = length(vec3(ivec3(v.xyz) - pos));

		ivec3 vbase = ivec3(v.xyz - (v.xyz & uvec3(1)));

		//search around if there is a better candidate from the same block
		for (int i = 0; i < 8; i++) {
			ivec3 bits = ((ivec3(i) >> ivec3(0, 1, 2)) & ivec3(1, 1, 1));
			ivec3 p = vbase + bits;

			float d2 = length(vec3(p - pos));
			if (d2 < d) { //check valid distance before test so we avoid a read
				uint c2 = imageLoad(src_color, p).r;
				if (bool(c2 & 1)) {
					v.xyz = uvec3(p);
					d = d2;
				}
			}
		}

		//could validate better position..
	}

	imageStore(dst_positions, pos, v);

#endif

#ifdef MODE_OCCLUSION

	// One group per probe, working on the (2 * OCCLUSION_SIZE)^3 voxels around it: the eight probe
	// cells the probe is a corner of. Each voxel stores, in the channel for this probe's index
	// parity, whether the probe can see it (see sdfvoxel_gi_process() in gi.glsl for the lookup).
	//
	// This used to propagate visibility outwards from the probe as a wavefront, each voxel taking
	// the average of its three neighbors towards the probe. That behaves like diffusion: it seeps
	// around corners and fans out through doorways, so the probe was partly "visible" well into
	// places it has no line of sight to. Walking the actual line from each voxel to the probe does
	// not, and costs about the same, from the same shared-memory copy of the geometry.

	uint invocation_idx = uint(gl_LocalInvocationID.x);
	ivec3 region = ivec3(gl_WorkGroupID);

	ivec3 region_offset = -ivec3(OCCLUSION_SIZE);
	region_offset += region * OCCLUSION_SIZE * 2;
	region_offset += params.probe_offset * OCCLUSION_SIZE;

	bool region_out_of_bounds = false;

	// Only probes whose region holds newly voxelized cells need their occlusion computed again (the
	// rest was scrolled in by MODE_SCROLL_OCCLUSION), unless the whole cascade is new.
	bool has_box = any(lessThan(params.box_from, params.box_to));
	if (params.scroll != ivec3(0) || has_box) {
		ivec3 region_offset_to = region_offset + ivec3(OCCLUSION_SIZE * 2);
		region_out_of_bounds = true;

		if (params.scroll != ivec3(0)) {
			//validate scroll region
			uvec3 scroll_mask = uvec3(notEqual(params.scroll, ivec3(0))); //save which axes acre scrolling
			ivec3 scroll_from = mix(ivec3(0), ivec3(params.grid_size) + params.scroll, lessThan(params.scroll, ivec3(0)));
			ivec3 scroll_to = mix(ivec3(params.grid_size), params.scroll, greaterThan(params.scroll, ivec3(0)));

			if ((uvec3(lessThanEqual(region_offset_to, scroll_from)) | uvec3(greaterThanEqual(region_offset, scroll_to))) * scroll_mask != scroll_mask) { //not all axes that scroll are out
				region_out_of_bounds = false;
			}
		}

		if (has_box && all(lessThan(region_offset, params.box_to)) && all(greaterThan(region_offset_to, params.box_from))) {
			region_out_of_bounds = false; // Around cells voxelized again for dynamic objects.
		}
	}

#define OCC_HALF_SIZE (OCCLUSION_SIZE / 2)

	ivec3 local_ofs = ivec3(uvec3(invocation_idx % OCC_HALF_SIZE, (invocation_idx % (OCC_HALF_SIZE * OCC_HALF_SIZE)) / OCC_HALF_SIZE, invocation_idx / (OCC_HALF_SIZE * OCC_HALF_SIZE))) * 4;

	if (!region_out_of_bounds) {
		for (int i = 0; i < 16; i++) { //skip x, so it can be packed

			ivec3 offset = local_ofs + ((ivec3(i * 4) >> ivec3(0, 2, 4)) & ivec3(3, 3, 3));

			uint facing_pack = 0;
			for (int j = 0; j < 4; j++) {
				ivec3 foffset = region_offset + offset + ivec3(j, 0, 0);
				if (all(greaterThanEqual(foffset, ivec3(0))) && all(lessThan(foffset, ivec3(params.grid_size)))) {
					uint f = imageLoad(src_facing, foffset).r;
					facing_pack |= f << (j * 8);
				}
			}

			occlusion_facing[(offset.z * (OCCLUSION_SIZE * 2 * OCCLUSION_SIZE * 2) + offset.y * (OCCLUSION_SIZE * 2) + offset.x) / 4] = facing_pack;
		}
	}

	//sync occlusion saved
	groupMemoryBarrier();
	barrier();

	// There are no more barriers after this, so we can early return.
	if (region_out_of_bounds) {
		return;
	}

	// The probe sits on the corner shared by the region's eight middle voxels, unless the
	// placement pass moved it. One that could not be placed anywhere usable is seen by nothing.
	ivec3 probe_cell = region * 2 + params.probe_offset;
	int probe_axis = params.grid_size / OCCLUSION_SIZE + 1;
	vec4 state = imageLoad(probe_state, ivec3(probe_cell.x + probe_cell.z * probe_axis, probe_cell.y, params.cascade));
	vec3 probe_pos = vec3(OCCLUSION_SIZE) + state.xyz;
	bool probe_valid = state.w > 0.5;

	const ivec3 facing_dirs[6] = ivec3[](ivec3(1, 0, 0), ivec3(0, 1, 0), ivec3(0, 0, 1), ivec3(-1, 0, 0), ivec3(0, -1, 0), ivec3(0, 0, -1));

	for (int i = 0; i < 64; i++) {
		ivec3 local_offset = local_ofs + ((ivec3(i) >> ivec3(0, 2, 4)) & ivec3(3, 3, 3));
		ivec3 offset = region_offset + local_offset;

		if (any(lessThan(offset, ivec3(0))) || any(greaterThanEqual(offset, ivec3(params.grid_size)))) {
			continue;
		}

		float occ = 0.0;
		uint facing = get_facing(local_offset);

		if (!probe_valid) {
			// Nothing sees a probe stuck in geometry.
		} else if (facing == 0) {
			occ = occlusion_trace(vec3(local_offset) + vec3(0.5), probe_pos);
		} else {
			// A surface voxel. Shading points on it look it up (after their normal bias) mostly
			// from its open side, so give it what the probe looks like from the free voxels it
			// faces, rather than from inside the geometry, where nothing is visible. Take the
			// least visible of them: geometry thinner than a voxel faces both ways, and the
			// average of its two sides would leave every probe half visible through it, which
			// points near the wall pick up through filtering (the light that used to seep in
			// along the edges of thin walls, floors and ceilings).
			occ = 1.0;
			bool any_side = false;
			for (int k = 0; k < 6; k++) {
				if (!bool(facing & (1 << k))) {
					continue;
				}
				ivec3 side = local_offset + facing_dirs[k];
				if (any(lessThan(side, ivec3(0))) || any(greaterThanEqual(side, ivec3(OCC_REGION_SIZE))) || get_facing(side) != 0) {
					continue;
				}
				occ = min(occ, occlusion_trace(vec3(side) + vec3(0.5), probe_pos));
				any_side = true;
			}
			if (!any_side) {
				occ = 0.0;
			}
		}

		imageStore(dst_occlusion[params.occlusion_index], offset, vec4(occ));
	}

#endif

#ifdef MODE_PROBE_PLACEMENT

	// Probe relocation and classification. The probe grid knows nothing of the geometry, so plenty
	// of probes end up inside walls, floors or terrain, or so close to a surface that the bias
	// their rays start with carries those rays past it. Such probes see the wrong side of the
	// wall and spread that light onto everything around them. Here each probe is moved, within a
	// fraction of the grid spacing, to a nearby spot that is outside geometry and clear of it, or
	// marked unusable when there is none; the probe rays, the occlusion pass and every place that
	// samples the probes then use the result. SDFGI geometry only changes when a cascade is
	// revoxelized, so this runs then rather than every frame, and a probe stays where it was put.

	int probe_axis = params.grid_size / OCCLUSION_SIZE + 1;
	ivec3 probe_cell = ivec3(gl_GlobalInvocationID.xyz);
	if (any(greaterThanEqual(probe_cell, ivec3(probe_axis)))) {
		return;
	}

	if (params.box_update) {
		// Only probes that look at the box can be placed differently: the rest are kept.
		ivec3 probe_grid = probe_cell * OCCLUSION_SIZE;
		if (any(greaterThanEqual(probe_grid - ivec3(OCCLUSION_SIZE), params.box_to)) || any(lessThanEqual(probe_grid + ivec3(OCCLUSION_SIZE), params.box_from))) {
			return;
		}
	}

	vec3 grid_pos = vec3(probe_cell * OCCLUSION_SIZE);
	vec3 region_min = grid_pos - vec3(OCCLUSION_SIZE);
	vec3 region_max = grid_pos + vec3(OCCLUSION_SIZE);

	vec3 best_offset = vec3(0.0);
	bool valid = true;

	PlacementSample here = placement_evaluate(grid_pos, region_min, region_max, PLACEMENT_RAYS);
	bool inside = here.backface_ratio > PLACEMENT_INSIDE_RATIO;

	if (inside || here.clearance < params.min_distance) {
		float max_offset = PLACEMENT_MAX_OFFSET * float(OCCLUSION_SIZE);
		float best_score = inside ? 1e10 : placement_score(here, 0.0);

		for (int m = 0; m < 2; m++) {
			float magnitude = m == 0 ? max_offset * 0.5 : max_offset;
			for (int i = 0; i < 27; i++) {
				ivec3 dir = ivec3(i % 3, (i / 3) % 3, i / 9) - ivec3(1);
				if (dir == ivec3(0)) {
					continue;
				}
				vec3 offset = vec3(dir) * magnitude;
				vec3 candidate = grid_pos + offset;
				// A probe outside geometry may only move where it can see from where it was,
				// or it could hop through a wall into the next room.
				if (!inside && !placement_segment_clear(grid_pos, candidate)) {
					continue;
				}
				PlacementSample s = placement_evaluate(candidate, region_min, region_max, PLACEMENT_CANDIDATE_RAYS);
				float score = placement_score(s, length(offset));
				if (score < best_score) {
					best_score = score;
					best_offset = offset;
				}
			}
		}

		if (best_offset != vec3(0.0)) {
			// Confirm with the full ray count what the candidate rays only estimated.
			PlacementSample chosen = placement_evaluate(grid_pos + best_offset, region_min, region_max, PLACEMENT_RAYS);
			if (chosen.backface_ratio > PLACEMENT_INSIDE_RATIO) {
				best_offset = vec3(0.0);
				valid = !inside;
			}
		} else if (inside) {
			valid = false; // Nowhere nearby to go.
		}
	}

	imageStore(dst_probe_state, ivec3(probe_cell.x + probe_cell.z * probe_axis, probe_cell.y, params.cascade), vec4(best_offset, valid ? 1.0 : 0.0));

#endif

#ifdef MODE_STORE

	ivec3 local = ivec3(gl_LocalInvocationID.xyz);
	// The whole cascade, or in a box update, the cells around the box.
	ivec3 pos = ivec3(gl_GlobalInvocationID.xyz) + params.region_from;
	uvec4 p;

	bool solid = false;
	bool store_occlusion = true;

	if (!params.box_update) {
		// store SDF
		p = imageLoad(src_positions, pos);

		float d;
		if (ivec3(p.xyz) == pos) {
			//solid block
			d = 0;
			solid = true;
		} else {
			//distance block
			d = 1.0 + length(vec3(p.xyz) - vec3(pos));
		}

		d /= 255.0;

		imageStore(dst_sdf, pos, vec4(d));
	} else if (all(lessThan(pos, params.region_to))) {
		// Only the cells around the box are updated (see GI::SDFGI::_update_box()), the rest of
		// the distance field is lowered by MODE_BOX_LOWER_SDF.
		if (in_box(pos, BOX_SDF_MARGIN)) {
			// The jump flood only knew of the voxels within twice the margin of the box (or
			// more), so those beyond are no nearer than the boundary of that (where it is not
			// the cascade's).
			p = imageLoad(src_positions, pos);

			float d;
			if (p.w != 0 && ivec3(p.xyz) == pos) {
				d = 0.0;
				solid = true;
			} else {
				ivec3 known_from = params.box_from - ivec3(2 * BOX_SDF_MARGIN);
				ivec3 known_to = params.box_to + ivec3(2 * BOX_SDF_MARGIN);
				float bound = 255.0;
				for (int i = 0; i < 3; i++) {
					if (known_from[i] > 0) {
						bound = min(bound, float(pos[i] - known_from[i] + 1));
					}
					if (known_to[i] < params.grid_size) {
						bound = min(bound, float(known_to[i] - pos[i]));
					}
				}
				d = 1.0 + (p.w != 0 ? min(length(vec3(p.xyz) - vec3(pos)), bound) : bound);
			}

			imageStore(dst_sdf, pos, vec4(min(d, 255.0) / 255.0));
		}

		// The voxels around the box are put in the list again, the rest are still in it.
		solid = solid && in_box(pos, BOX_VOXEL_MARGIN);
		store_occlusion = in_box_occlusion_region(pos);

		if (in_box(pos, BOX_LIGHT_MARGIN)) {
			// Light of voxels that are gone, or written next to a voxel by one that may no longer
			// be the nearest. The voxels around the box light it again this frame.
			imageStore(dst_light, pos, uvec4(0));
			imageStore(dst_aniso0, pos, vec4(0.0));
			imageStore(dst_aniso1, pos, vec4(0.0));
		}
	} else {
		store_occlusion = false; // Past the region (the group size rounds it up).
	}

	// STORE OCCLUSION

	if (store_occlusion) {
		uint occlusion = 0;
		const uint occlusion_shift[8] = uint[](4, 8, 12, 0, 20, 24, 28, 16); // Channel i lands in r, g, b, a of B4G4R4A4 (see create()).
		for (int i = 0; i < 8; i++) {
			float occ = imageLoad(src_occlusion[i], pos).r;
			occlusion |= uint(clamp(occ * 15.0, 0.0, 15.0)) << occlusion_shift[i];
		}
		{
			ivec3 occ_pos = pos;
			occ_pos.z += params.cascade * params.grid_size;
			imageStore(dst_occlusion, occ_pos, uvec4(occlusion & 0xFFFF));
			occ_pos.x += params.grid_size;
			imageStore(dst_occlusion, occ_pos, uvec4(occlusion >> 16));
		}
	}

	// STORE POSITIONS

	if (local == ivec3(0)) {
		store_position_count = 0; //base one stores as zero, the others wait
	}

	groupMemoryBarrier();
	barrier();

	if (solid) {
		uint index = atomicAdd(store_position_count, 1);
		// At least do the conversion work in parallel
		store_positions[index].position = uint(pos.x | (pos.y << 7) | (pos.z << 14));

		//see around which voxels point to this one, add them to the list
		uint bit_index = 0;
		uint neighbour_bits = 0;
		for (int i = -1; i <= 1; i++) {
			for (int j = -1; j <= 1; j++) {
				for (int k = -1; k <= 1; k++) {
					if (i == 0 && j == 0 && k == 0) {
						continue;
					}
					ivec3 npos = pos + ivec3(i, j, k);
					if (all(greaterThanEqual(npos, ivec3(0))) && all(lessThan(npos, ivec3(params.grid_size)))) {
						p = imageLoad(src_positions, npos);
						if (ivec3(p.xyz) == pos) {
							neighbour_bits |= (1 << bit_index);
						}
					}
					bit_index++;
				}
			}
		}

		uint rgb = imageLoad(src_albedo, pos).r;
		uint facing = imageLoad(src_facing, pos).r;

		store_positions[index].albedo = rgb >> 1; //store as it comes (555) to avoid precision loss (and move away the alpha bit)
		store_positions[index].albedo |= (facing & 0x3F) << 15; // store facing in bits 15-21

		store_positions[index].albedo |= neighbour_bits << 21; //store lower 11 bits of neighbors with remaining albedo
		store_positions[index].position |= (neighbour_bits >> 11) << 21; //store 11 bits more of neighbors with position

		store_positions[index].light = imageLoad(src_light, pos).r;
		store_positions[index].light_aniso = imageLoad(src_light_aniso, pos).r;
		//add neighbors
		store_positions[index].light |= (neighbour_bits >> 22) << 30; //store 2 bits more of neighbors with light
		store_positions[index].light_aniso |= (neighbour_bits >> 24) << 30; //store 2 bits more of neighbors with aniso
	}

	groupMemoryBarrier();
	barrier();

	// global increment only once per group, to reduce pressure

	if (local == ivec3(0) && store_position_count > 0) {
		store_from_index = atomicAdd(dispatch_data.total_count, store_position_count);
		uint group_count = (store_from_index + store_position_count - 1) / 64 + 1;
		atomicMax(dispatch_data.x, group_count);
	}

	groupMemoryBarrier();
	barrier();

	uint read_index = uint(local.z * 4 * 4 + local.y * 4 + local.x);
	uint write_index = store_from_index + read_index;

	if (read_index < store_position_count) {
		dst_process_voxels.data[write_index] = store_positions[read_index];
	}

	if (pos == ivec3(0)) {
		//this thread clears y and z
		dispatch_data.y = 1;
		dispatch_data.z = 1;
	}
#endif

#ifdef MODE_BOX_LOWER_SDF

	// Away from the box of a box update (see GI::SDFGI::_update_box()), geometry can only have
	// come closer by being in the box now, so the distance is lowered to the box's where that is
	// less. Where geometry left the box, the distance stays what it was: too short, which tracing
	// only takes as a reason for shorter steps, since it is at least BOX_SDF_MARGIN here.
	ivec3 pos = ivec3(gl_GlobalInvocationID.xyz);
	if (any(greaterThanEqual(pos, ivec3(params.grid_size))) || in_box(pos, BOX_SDF_MARGIN)) {
		return; // Computed again by MODE_STORE.
	}

	vec3 to_box = max(vec3(params.box_from - pos), max(vec3(pos - (params.box_to - ivec3(1))), vec3(0.0)));
	float d = floor(1.0 + length(to_box));
	if (d < imageLoad(dst_sdf, pos).r * 255.0 - 0.5) {
		imageStore(dst_sdf, pos, vec4(d / 255.0));
	}

#endif

#ifdef MODE_BOX_CARRY

	// A box update (see GI::SDFGI::_update_box()) rebuilds the voxel list from a copy of it. The
	// voxels around the box are left out, as MODE_STORE puts them in again as they are now, and
	// those in the region the update works in are written to the render textures, the way
	// MODE_SCROLL writes them all: that is what the jump flood, occlusion and probe placement
	// read, and what MODE_STORE takes their stored light from. The box itself was voxelized again.

	uint index = gl_GlobalInvocationID.x;
	uint local_index = gl_LocalInvocationID.x;

	if (local_index == 0) {
		carry_count = 0;
	}

	groupMemoryBarrier();
	barrier();

	bool keep = false;
	ProcessVoxel voxel;
	uint keep_index = 0;

	if (index < src_dispatch_data.total_count) {
		voxel = src_process_voxels.data[index];
		ivec3 pos = (ivec3(voxel.position) >> ivec3(0, 7, 14)) & ivec3(0x7F);

		if (in_region(pos) && !in_box(pos, 0)) {
			imageStore(dst_albedo, pos, uvec4(((voxel.albedo & 0x7FFF) << 1) | 1)); // Add the solid bit.
			imageStore(dst_facing, pos, uvec4((voxel.albedo >> 15) & 0x3F));
			imageStore(dst_light, pos, uvec4(voxel.light & 0x3fffffff));
			imageStore(dst_light_aniso, pos, uvec4(voxel.light_aniso & 0x3fffffff));
		}

		keep = !in_box(pos, BOX_VOXEL_MARGIN);
		if (keep) {
			keep_index = atomicAdd(carry_count, 1);
		}
	}

	groupMemoryBarrier();
	barrier();

	if (local_index == 0 && carry_count > 0) {
		carry_from_index = atomicAdd(dispatch_data.total_count, carry_count);
		atomicMax(dispatch_data.x, (carry_from_index + carry_count - 1) / 64 + 1);
	}

	groupMemoryBarrier();
	barrier();

	if (keep) {
		dst_process_voxels.data[carry_from_index + keep_index] = voxel;
	}

#endif
}
