/**************************************************************************/
/*  foliage_gpu_culler.h                                                  */
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

#pragma once

#include "core/math/plane.h"
#include "core/math/transform_3d.h"
#include "core/object/ref_counted.h"
#include "core/templates/local_vector.h"

class Camera3D;
class Node3D;

// Every RenderingDevice object the culling passes need. Split out of
// FoliageGPUCuller so that its fields are only ever touched on the rendering
// thread: the main thread creates one of these, hands it to queued calls, and
// never reads it back. Queued calls keep it alive through their bound
// reference, so it outlives the culler if teardown is still pending.
class FoliageCullResources : public RefCounted {
	GDCLASS(FoliageCullResources, RefCounted);

	friend class FoliageGPUCuller;

	RID cull_shader;
	RID cull_pipeline;
	RID apply_shader;
	RID apply_pipeline;
	RID source_buffer;
	uint32_t source_instance_count = 0;

	// Occlusion side of the cull pass: a copy of the renderer's depth pyramid
	// for the viewport being culled for, plus the camera and mip layout needed
	// to look instances up in it. Rebuilt every frame, since both the pyramid
	// and the camera change.
	RID occlusion_params_buffer;
	RID occlusion_depth_buffer;
	uint32_t occlusion_depth_floats = 0;
	RID occlusion_uniform_set;

	struct LODResources {
		RID counter_buffer;
		RID cull_uniform_set;
		RID apply_uniform_set;
		uint32_t surface_count = 1;
		float range_begin = 0.0f;
		float range_end = 0.0f;
		float instance_radius = 1.0f;
	};
	LocalVector<LODResources> lod_resources;

	bool _ensure_shaders();
	void _free_levels();
	void _free_occlusion();
	void _free_all();

	// Uploads the depth pyramid the renderer built for p_viewport, in the shape
	// the cull shader reads it. Leaves the pass with occlusion switched off
	// (but still bound) when there is no pyramid to hand it.
	void _update_occlusion(RID p_viewport, const Transform3D &p_node_transform, bool p_enabled);

	// Bound to the queued calls, so these run on the rendering thread.
	void rt_setup(const PackedByteArray &p_transform_data, const Array &p_multimeshes, const PackedFloat32Array &p_level_params, const PackedInt32Array &p_surface_counts);
	void rt_cull(const PackedFloat32Array &p_frame_params, RID p_occlusion_viewport, bool p_occlusion_enabled);
};

// GPU-driven culling for foliage drawn through indirect MultiMeshes.
//
// One source buffer holds every generated instance transform. Each frame a
// compute pass runs once per LOD level: every instance is tested against the
// camera frustum and that level's distance band, and the survivors are
// compacted into that level's MultiMesh buffer while an atomic counter tracks
// how many there are. A second, tiny pass writes that counter into the
// instance-count field of the level's indirect draw command, so the GPU - not
// the CPU - decides how much geometry is actually submitted.
//
// This replaces per-node frustum culling with per-instance culling: instead of
// drawing every instance of every cell that intersects the frustum, only the
// instances that are really on screen (and really at that LOD's distance) are
// drawn, from a single draw call per level.
//
// All RenderingDevice work is queued onto the rendering thread through
// RenderingServer::call_on_render_thread; the methods below are called from the
// main thread and only ever hand over copies of their data.
class FoliageGPUCuller {
public:
	// Bounded so the per-frame dispatch count stays predictable; FoliageLayer
	// and FoliageSpawner3D both default to three levels.
	static constexpr int MAX_LOD_LEVELS = 4;

	struct LODLevel {
		RID multimesh; // Must have been allocated with use_indirect enabled.
		float range_begin = 0.0f;
		float range_end = 0.0f; // Zero or less means "no far limit".
		float instance_radius = 1.0f; // Local-space bounding radius at unit scale.
		uint32_t surface_count = 1;
	};

	// True if this build can run the GPU path at all (RenderingDevice backends
	// only; the GL Compatibility renderer cannot draw indirect MultiMeshes).
	static bool is_supported();

	// The camera this node's foliage should be culled against.
	//
	// At runtime that is simply the camera of the viewport the node lives in.
	// In the editor it is not: the edited scene is drawn by the 3D editor's own
	// viewports, which belong to the editor's UI rather than to the scene, so
	// the node's viewport would hand back the scene's game camera (culling
	// against a view nobody is looking through) or nothing at all (culling
	// everything away). So the editor case looks for a viewport that draws the
	// same world instead. r_cached_camera carries the result between frames, to
	// keep that search off the per-frame path.
	static Camera3D *resolve_culling_camera(const Node3D *p_node, ObjectID &r_cached_camera);

	// Queues a frame that culls nothing, so that a missing camera leaves the
	// foliage visible rather than making all of it vanish. Instances still go
	// through their LOD bands, measured from the node's own origin.
	void draw_without_culling();

	// Uploads the instance transforms and (re)builds the per-level resources.
	// Safe to call whenever the instances or the LOD chain change.
	void update_instances(const LocalVector<Transform3D> &p_transforms, const LocalVector<LODLevel> &p_levels);

	// Queues one frame's culling. The frustum planes and camera position must
	// already be in the MultiMeshes' local space.
	//
	// Instances hidden behind an occluder are dropped as well, when the
	// viewport being culled for has an occlusion culling buffer (see
	// Viewport.use_occlusion_culling and Landscape3D.occluder_enabled) and
	// p_occlusion_enabled says to use it. That test needs the instances in
	// world space, hence the node's own transform.
	void cull(const Vector<Plane> &p_frustum_planes, const Vector3 &p_camera_position, RID p_occlusion_viewport = RID(), const Transform3D &p_node_transform = Transform3D(), bool p_occlusion_enabled = false);

	// Drops every GPU resource. Called automatically on destruction.
	void release();

	FoliageGPUCuller() = default;
	~FoliageGPUCuller();

	// Two cullers sharing one set of resources would both queue a free of it,
	// so copying is off: hold them by value or by pointer, never in a container
	// that reseats its elements.
	FoliageGPUCuller(const FoliageGPUCuller &) = delete;
	FoliageGPUCuller &operator=(const FoliageGPUCuller &) = delete;

private:
	Ref<FoliageCullResources> resources;

	// Static so that teardown does not depend on the culler still existing when
	// the rendering thread gets to it.
	static void rt_free(const Ref<FoliageCullResources> &p_resources);
};
