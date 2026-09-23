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
	void _free_all();

	// Bound to the queued calls, so these run on the rendering thread.
	void rt_setup(const PackedByteArray &p_transform_data, const Array &p_multimeshes, const PackedFloat32Array &p_level_params, const PackedInt32Array &p_surface_counts);
	void rt_cull(const PackedFloat32Array &p_frame_params);
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

	// Uploads the instance transforms and (re)builds the per-level resources.
	// Safe to call whenever the instances or the LOD chain change.
	void update_instances(const LocalVector<Transform3D> &p_transforms, const LocalVector<LODLevel> &p_levels);

	// Queues one frame's culling. The frustum planes and camera position must
	// already be in the MultiMeshes' local space.
	void cull(const Vector<Plane> &p_frustum_planes, const Vector3 &p_camera_position);

	// Drops every GPU resource. Called automatically on destruction.
	void release();

	~FoliageGPUCuller();

private:
	Ref<FoliageCullResources> resources;

	// Static so that teardown does not depend on the culler still existing when
	// the rendering thread gets to it.
	static void rt_free(const Ref<FoliageCullResources> &p_resources);
};
