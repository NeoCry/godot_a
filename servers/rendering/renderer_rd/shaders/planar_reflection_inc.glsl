// Planar reflections (see RendererRD::PlanarReflections): the scene drawn mirrored across each
// plane in sight that reflects it, which the surfaces near that plane reflect instead of the sky and
// reflection probes. Declares the uniform buffer and the two texture arrays at
// PLANAR_REFLECTION_BINDING and the two bindings after it in set 1, so define that first, after the
// scene shaders' samplers.

#define MAX_PLANAR_REFLECTIONS 4

// What the layers' radiance was divided by to fit their format (the Mobile renderer halves it, as it
// does its reflection probes').
#ifndef PLANAR_REFLECTION_MULTIPLIER
#define PLANAR_REFLECTION_MULTIPLIER 1.0
#endif

// Mirrors RendererRD::PlanarReflections::LayerUBO. Everything spatial is in view space.
struct PlanarReflectionData {
	// To the volume of surfaces that reflect the plane: inside it within [-1, 1] on every axis,
	// y running along the plane's normal.
	mat4 view_to_receiver;
	// To the mirrored camera's clip space, as its layer was drawn, and back.
	mat4 view_to_reflection;
	mat4 reflection_to_view;
	// Normal in xyz, and w such that dot(xyz, p) + w is how far p is above the mirror.
	vec4 plane;
	// The mirrored camera in xyz; in w, how many texels of the layer a meter spans one meter in
	// front of it.
	vec4 camera;
	float intensity;
	float distortion;
	float normal_fade;
	float edge_fade;
	uint reflection_mask;
	float max_lod;
	float pad0;
	float pad1;
};

layout(set = 1, binding = PLANAR_REFLECTION_BINDING, std140) uniform PlanarReflections {
	PlanarReflectionData data[MAX_PLANAR_REFLECTIONS];
	uint count;
	uint pad0;
	uint pad1;
	uint pad2;
}
planar_reflections;

// Linear HDR radiance, its mips blurred for rough surfaces, and the depth the layers were drawn with.
layout(set = 1, binding = PLANAR_REFLECTION_BINDING + 1) uniform texture2DArray planar_reflection_color;
layout(set = 1, binding = PLANAR_REFLECTION_BINDING + 2) uniform texture2DArray planar_reflection_depth;

// What a surface at p_vertex, facing p_normal and seen along p_view (towards the camera), reflects of
// the planes it lies on: radiance premultiplied by how much of the reflection they account for, in
// alpha.
//
// A mirror's reflection of a point where the view meets it is what its layer shows right there.
// Ripples and bumps in the surface turn the reflected ray aside, and how far that moves what it
// shows depends on how far away the reflected thing is: the layer's depth there says how far, and
// the ray turned by the surface's own normal is followed that far and looked up where it lands. For
// the same reason the blur of a rough surface grows with that distance, as the cone of directions
// it reflects widens: things standing in the reflection stay sharp where they meet it, and grow
// blurrier the further their reflection reaches from them, as on a real wet floor.
vec4 planar_reflection_compute(vec3 p_vertex, vec3 p_normal, vec3 p_view, float p_roughness, uint p_layer_mask) {
	vec4 result = vec4(0.0);
	uint count = min(planar_reflections.count, uint(MAX_PLANAR_REFLECTIONS));
	for (uint i = 0; i < count; i++) {
		if ((planar_reflections.data[i].reflection_mask & p_layer_mask) == 0u) {
			continue;
		}
		vec3 local = abs((planar_reflections.data[i].view_to_receiver * vec4(p_vertex, 1.0)).xyz);
		if (max(local.x, max(local.y, local.z)) >= 1.0) {
			continue;
		}
		vec3 plane_normal = planar_reflections.data[i].plane.xyz;
		float normal_fade = planar_reflections.data[i].normal_fade;
		float weight = smoothstep(normal_fade, mix(normal_fade, 1.0, 0.3), dot(p_normal, plane_normal));
		float edge = max(planar_reflections.data[i].edge_fade, 1e-4);
		weight *= 1.0 - smoothstep(1.0 - edge, 1.0, max(local.x, local.z));
		weight *= 1.0 - smoothstep(1.0 - edge, 1.0, local.y);
		if (weight <= 0.0) {
			continue;
		}

		// Where the view meets the mirror, and how far behind it lies what the mirror shows there
		// (nothing is drawn at the far plane: the sky, infinitely far).
		vec3 on_plane = p_vertex - plane_normal * (dot(plane_normal, p_vertex) + planar_reflections.data[i].plane.w);
		vec4 clip = planar_reflections.data[i].view_to_reflection * vec4(on_plane, 1.0);
		vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
		float depth = textureLod(sampler2DArray(planar_reflection_depth, SAMPLER_NEAREST_CLAMP), vec3(uv, float(i)), 0.0).r;
		float reach = 100000.0;
		if (depth > 0.0) {
			vec4 reflected = planar_reflections.data[i].reflection_to_view * vec4(uv * 2.0 - 1.0, depth, 1.0);
			reach = distance(reflected.xyz / reflected.w, on_plane);
		}

		// The ray the mirror reflects, turned by the surface's own normal, kept from dipping below
		// the plane.
		vec3 mirrored = reflect(-p_view, plane_normal);
		vec3 turned = mix(mirrored, reflect(-p_view, p_normal), planar_reflections.data[i].distortion);
		turned -= plane_normal * min(dot(turned, plane_normal), 0.0);
		turned = normalize(turned + plane_normal * 1e-4);
		vec3 target = on_plane + turned * reach;
		vec4 target_clip = planar_reflections.data[i].view_to_reflection * vec4(target, 1.0);
		if (target_clip.w <= 0.0) {
			continue;
		}
		uv = target_clip.xy / target_clip.w * 0.5 + 0.5;
		// The layers are drawn a little wider than the view, so only what the ripples throw well
		// past its edges fades out.
		vec2 inside = smoothstep(vec2(0.0), vec2(0.02), uv) * (1.0 - smoothstep(vec2(0.98), vec2(1.0), uv));
		weight *= inside.x * inside.y;
		if (weight <= 0.0) {
			continue;
		}

		// The cone a rough surface reflects, as wide as it has spread over the distance travelled,
		// measured in texels where it lands.
		float spread = p_roughness * p_roughness * reach;
		float texels = spread * planar_reflections.data[i].camera.w / max(distance(target, planar_reflections.data[i].camera.xyz), 1e-3);
		float lod = clamp(log2(max(texels, 1.0)), 0.0, planar_reflections.data[i].max_lod);
		vec3 color = textureLod(sampler2DArray(planar_reflection_color, SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), vec3(uv, float(i)), lod).rgb;

		result.rgb += color * (planar_reflections.data[i].intensity * PLANAR_REFLECTION_MULTIPLIER * weight * (1.0 - result.a));
		result.a += weight * (1.0 - result.a);
	}
	return result;
}
