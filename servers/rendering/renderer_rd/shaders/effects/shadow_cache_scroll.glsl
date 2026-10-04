#[vertex]

#version 450

#VERSION_DEFINES

void main() {
	vec2 base_arr[4] = vec2[](vec2(0.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0), vec2(1.0, 0.0));
	gl_Position = vec4(base_arr[gl_VertexIndex] * 2.0 - 1.0, 0.0, 1.0);
}

#[fragment]

#version 450

#VERSION_DEFINES

// Copies a shadow map's depths texel for texel from source_offset away, and moves them by
// depth_offset, for when the shadow map's frustum has moved by whole texels and depth steps.

layout(set = 0, binding = 0) uniform sampler2D source_depth;

layout(push_constant, std430) uniform Params {
	ivec2 source_offset;
	float depth_offset;
	float pad;
}
params;

void main() {
	float depth = texelFetch(source_depth, ivec2(gl_FragCoord.xy) + params.source_offset, 0).r;
	// Depths are reversed: 0 is the far plane, which is also what a texel with nothing in it holds,
	// and has to stay that way. What ends up past the far plane can't shadow anything anymore, and
	// what ends up past the near plane is pancaked onto it, like when it was drawn.
	if (depth > 0.0) {
		depth = clamp(depth - params.depth_offset, 0.0, 1.0);
	}
	gl_FragDepth = depth;
}
