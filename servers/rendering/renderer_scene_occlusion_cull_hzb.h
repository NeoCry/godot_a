/**************************************************************************/
/*  renderer_scene_occlusion_cull_hzb.h                                   */
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

#include "core/math/projection.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid_owner.h"
#include "servers/rendering/renderer_scene_occlusion_cull.h"

// Occlusion culling backend implementing the classic hierarchical Z-buffer
// (Greene/Kass/Miller): every occluder in view is scan-converted into a small
// depth buffer on the CPU, that buffer is reduced into a "farthest depth"
// mip pyramid (see HZBuffer::update_mips), and each instance's bounding box is
// then tested against the coarsest pyramid level that still covers its screen
// footprint, descending only where the test is inconclusive (see
// HZBuffer::_is_occluded, shared with every other backend).
//
// This is the same depth pyramid the Embree backend (RaycastOcclusionCull, in
// modules/raycast) feeds, only filled by rasterizing triangles instead of by
// casting one ray per depth pixel: the work per occluder triangle is
// proportional to the handful of depth-buffer pixels it actually covers
// instead of to the whole buffer, which is what makes it cheap enough to keep
// a terrain's worth of occluders on screen at all times (see Landscape3D's
// occluder_enabled, which feeds the heightfield in as occluder geometry). It
// also has no third-party dependency, so it is available in every build and on
// every platform, and is the default backend (see the
// "rendering/occlusion_culling/backend" project setting).
//
// Depth values stored in the buffer are distances from the camera position, to
// match what HZBuffer::_is_occluded compares them against: the euclidean
// distance to the closest point of the tested AABB for a perspective camera,
// and the view-space depth for an orthogonal one.
class HZBOcclusionCull : public RendererSceneOcclusionCull {
public:
	// A triangle that has been projected, clipped to the depth buffer and is
	// ready to be scan-converted. Positions are in pixels, with (0, 0) at the
	// bottom-left corner of the buffer (matching HZBuffer::_is_occluded's
	// normalized device coordinate mapping).
	struct RasterTriangle {
		float x[3];
		float y[3];
		float inv_w[3];
		float depth_over_w[3];
		int min_x;
		int min_y;
		int max_x;
		int max_y;
	};

	// A vertex after projection. inv_w and depth_over_w are the usual pair of
	// screen-space linear interpolants used for perspective-correct depth:
	// both are affine in screen space, so the view depth at a pixel is their
	// quotient (and for an orthogonal projection, where w is always 1, this
	// degenerates into plain linear interpolation of the depth).
	struct ScreenVertex {
		float x;
		float y;
		float inv_w;
		float depth_over_w;
	};

	// An occluder mesh vertex, cached while an occluder is being set up so
	// that vertices shared by several triangles are only transformed once.
	struct ProjectedVertex {
		Vector3 view;
		ScreenVertex screen;
		bool in_front;
	};

private:
	struct InstanceID {
		RID scenario;
		RID instance;

		static uint32_t hash(const InstanceID &p_ins) {
			uint32_t h = hash_murmur3_one_64(p_ins.scenario.get_id());
			return hash_fmix32(hash_murmur3_one_64(p_ins.instance.get_id(), h));
		}
		bool operator==(const InstanceID &rhs) const {
			return instance == rhs.instance && scenario == rhs.scenario;
		}

		InstanceID() {}
		InstanceID(RID s, RID i) :
				scenario(s), instance(i) {}
	};

	struct Occluder {
		PackedVector3Array vertices;
		PackedInt32Array indices;
		AABB aabb;
		HashSet<InstanceID, InstanceID> users;
	};

	struct OccluderInstance {
		RID occluder;
		Transform3D xform;
		AABB world_aabb;
		bool enabled = true;
		bool removed = false;
	};

	struct Scenario {
		HashMap<RID, OccluderInstance> instances;
		LocalVector<RID> removed_instances;
	};

public:
	// One occluder instance selected for rendering in the current frame, along
	// with the screen area used to prioritize it against the triangle budget.
	struct RenderItem {
		const Occluder *occluder = nullptr;
		Transform3D xform;
		float screen_area = 0.0f;
		uint32_t triangle_count = 0;
	};

	class RasterHZBuffer : public HZBuffer {
		// Per-slice scratch storage. The setup pass splits the frame's
		// occluder triangles into one slice per worker thread; each slice
		// projects and clips its own triangles into its own array and bins
		// them into the horizontal bands of the depth buffer they touch, so
		// the rasterization pass that follows can hand each band to a single
		// thread and let it write its rows without any synchronization.
		struct RasterSlice {
			LocalVector<RasterTriangle> triangles;
			LocalVector<LocalVector<uint32_t>> bins;
			LocalVector<ProjectedVertex> vertices;
		};

		struct SetupThreadData {
			const RenderItem *items = nullptr;
			const uint32_t *slice_first_item = nullptr;
			Transform3D cam_inv_transform;
			Projection cam_projection;
			Vector2 jitter;
			real_t z_near = 0.0f;
			int band_count = 1;
			int band_height = 1;
		};

		struct RasterThreadData {
			int band_height = 1;
			uint32_t slice_count = 0;
		};

		LocalVector<RasterSlice> slices;
		LocalVector<float> ray_length_columns;
		LocalVector<float> ray_length_rows;

		void _setup_slice(uint32_t p_slice, const SetupThreadData *p_data);
		void _rasterize_band(uint32_t p_band, const RasterThreadData *p_data);
		void _clear_depth();
		void _depth_to_distance(const Projection &p_cam_projection, bool p_cam_orthogonal);

		static void _emit_polygon(RasterSlice &r_slice, const ScreenVertex *p_polygon, int p_count, const Size2i &p_size, int p_band_count, int p_band_height);
		static void _push_triangle(RasterSlice &r_slice, const ScreenVertex &p_a, const ScreenVertex &p_b, const ScreenVertex &p_c, const Size2i &p_size, int p_band_count, int p_band_height);

	public:
		RID scenario_rid;

		// Rasterizes every occluder in p_items into the depth buffer and
		// rebuilds the depth pyramid from it.
		void render(const LocalVector<RenderItem> &p_items, const Transform3D &p_cam_transform, const Projection &p_cam_projection, bool p_cam_orthogonal, const Vector2 &p_jitter);

		Size2i get_size() const { return sizes.is_empty() ? Size2i() : sizes[0]; }
	};

private:
	RID_PtrOwner<Occluder> occluder_owner;
	HashMap<RID, Scenario> scenarios;
	HashMap<RID, RasterHZBuffer> buffers;

	bool jitter_enabled = false;
	int triangle_budget = 0;

	LocalVector<RenderItem> render_items;

	void _update_instance_aabb(OccluderInstance &r_instance);
	void _gather_render_items(Scenario &p_scenario, const Transform3D &p_cam_transform, const Projection &p_cam_projection, const Size2i &p_buffer_size);
	Vector2 _get_jitter(const Size2i &p_buffer_size) const;

public:
	virtual bool is_occluder(RID p_rid) override;
	virtual RID occluder_allocate() override;
	virtual void occluder_initialize(RID p_occluder) override;
	virtual void occluder_set_mesh(RID p_occluder, const PackedVector3Array &p_vertices, const PackedInt32Array &p_indices) override;
	virtual void free_occluder(RID p_occluder) override;

	virtual void add_scenario(RID p_scenario) override;
	virtual void remove_scenario(RID p_scenario) override;
	virtual void scenario_set_instance(RID p_scenario, RID p_instance, RID p_occluder, const Transform3D &p_xform, bool p_enabled) override;
	virtual void scenario_remove_instance(RID p_scenario, RID p_instance) override;

	virtual void add_buffer(RID p_buffer) override;
	virtual void remove_buffer(RID p_buffer) override;
	virtual HZBuffer *buffer_get_ptr(RID p_buffer) override;
	virtual void buffer_set_scenario(RID p_buffer, RID p_scenario) override;
	virtual void buffer_set_size(RID p_buffer, const Vector2i &p_size) override;
	virtual void buffer_update(RID p_buffer, const Transform3D &p_cam_transform, const Projection &p_cam_projection, bool p_cam_orthogonal) override;

	virtual RID buffer_get_debug_texture(RID p_buffer, bool p_pyramid) override;

	HZBOcclusionCull();
	~HZBOcclusionCull();
};
