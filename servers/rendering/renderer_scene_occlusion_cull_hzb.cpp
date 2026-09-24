/**************************************************************************/
/*  renderer_scene_occlusion_cull_hzb.cpp                                 */
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

#include "renderer_scene_occlusion_cull_hzb.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/object/worker_thread_pool.h"

// Occluders covering less than this many depth-buffer pixels are skipped: they
// can never hide anything the frustum and the depth pyramid's own conservatism
// wouldn't let through anyway, and skipping them is what keeps the cost of a
// terrain-sized pile of occluder chunks proportional to what is actually near
// the camera.
static constexpr float MIN_OCCLUDER_SCREEN_AREA = 1.0f;

// Below this many triangles a frame's occluders are projected on the calling
// thread: splitting them across the worker pool would cost more in scheduling
// than it saves.
static constexpr uint32_t MIN_TRIANGLES_FOR_THREADING = 2048;

// Triangles smaller than this (in square pixels of the depth buffer) are
// dropped instead of rasterized. They can only ever touch a pixel center by
// accident, and dropping them avoids dividing by a near-zero area.
static constexpr float MIN_TRIANGLE_AREA = 0.00001f;

// A triangle straddling the near plane clips into at most 4 vertices, and each
// of the depth buffer's 4 edges can add one more.
static constexpr int MAX_CLIPPED_VERTICES = 12;

// The near plane rectangle of a projection, in view space. Lifted from
// RaycastOcclusionCull, which needs the same rectangle to aim its rays.
//
// NOTE: This assumes a rectangular projection plane, i.e. that:
// - the matrix is a projection across z-axis (i.e. is invertible and columns[0][1], [0][3], [1][0] and [1][3] == 0)
// - the projection plane is rectangular (i.e. columns[0][2] and [1][2] == 0 if columns[2][3] != 0)
static Rect2 _get_viewport_rect(const Projection &p_cam_projection) {
	Size2 half_extents = p_cam_projection.get_viewport_half_extents();
	Point2 bottom_left = -half_extents * Vector2(p_cam_projection.columns[3][0] * p_cam_projection.columns[3][3] + p_cam_projection.columns[2][0] * p_cam_projection.columns[2][3] + 1, p_cam_projection.columns[3][1] * p_cam_projection.columns[3][3] + p_cam_projection.columns[2][1] * p_cam_projection.columns[2][3] + 1);
	return Rect2(bottom_left, 2 * half_extents);
}

static _FORCE_INLINE_ HZBOcclusionCull::ScreenVertex _project_view_position(const Vector3 &p_view, const Projection &p_projection, const Size2i &p_size, const Vector2 &p_jitter) {
	const Vector4 clip = p_projection.xform(Vector4(p_view.x, p_view.y, p_view.z, 1.0f));

	HZBOcclusionCull::ScreenVertex sv;
	sv.inv_w = 1.0f / MAX((float)clip.w, (float)CMP_EPSILON);
	sv.x = ((float)clip.x * sv.inv_w * 0.5f + 0.5f) * p_size.x + p_jitter.x;
	sv.y = ((float)clip.y * sv.inv_w * 0.5f + 0.5f) * p_size.y + p_jitter.y;
	// Linear view depth, which is what the depth buffer holds until
	// _depth_to_distance() turns it into a distance from the camera.
	sv.depth_over_w = -(float)p_view.z * sv.inv_w;
	return sv;
}

////////////////////////////////////////////////////////
// Occluders.

bool HZBOcclusionCull::is_occluder(RID p_rid) {
	return occluder_owner.owns(p_rid);
}

RID HZBOcclusionCull::occluder_allocate() {
	return occluder_owner.allocate_rid();
}

void HZBOcclusionCull::occluder_initialize(RID p_occluder) {
	Occluder *occluder = memnew(Occluder);
	occluder_owner.initialize_rid(p_occluder, occluder);
}

void HZBOcclusionCull::occluder_set_mesh(RID p_occluder, const PackedVector3Array &p_vertices, const PackedInt32Array &p_indices) {
	Occluder *occluder = occluder_owner.get_or_null(p_occluder);
	ERR_FAIL_NULL(occluder);

	occluder->vertices = p_vertices;
	occluder->indices = p_indices;

	occluder->aabb = AABB();
	const Vector3 *vertices = p_vertices.ptr();
	for (int i = 0; i < p_vertices.size(); i++) {
		if (i == 0) {
			occluder->aabb.position = vertices[0];
		} else {
			occluder->aabb.expand_to(vertices[i]);
		}
	}

	for (const InstanceID &E : occluder->users) {
		Scenario *scenario = scenarios.getptr(E.scenario);
		ERR_CONTINUE(!scenario);
		OccluderInstance *instance = scenario->instances.getptr(E.instance);
		ERR_CONTINUE(!instance);
		_update_instance_aabb(*instance);
	}
}

void HZBOcclusionCull::free_occluder(RID p_occluder) {
	Occluder *occluder = occluder_owner.get_or_null(p_occluder);
	ERR_FAIL_NULL(occluder);
	memdelete(occluder);
	occluder_owner.free(p_occluder);
}

////////////////////////////////////////////////////////
// Scenarios.

void HZBOcclusionCull::add_scenario(RID p_scenario) {
	ERR_FAIL_COND(scenarios.has(p_scenario));
	scenarios[p_scenario] = Scenario();
}

void HZBOcclusionCull::remove_scenario(RID p_scenario) {
	Scenario *scenario = scenarios.getptr(p_scenario);
	ERR_FAIL_NULL(scenario);

	for (KeyValue<RID, OccluderInstance> &E : scenario->instances) {
		Occluder *occluder = occluder_owner.get_or_null(E.value.occluder);
		if (occluder) {
			occluder->users.erase(InstanceID(p_scenario, E.key));
		}
	}

	scenarios.erase(p_scenario);
}

void HZBOcclusionCull::_update_instance_aabb(OccluderInstance &r_instance) {
	const Occluder *occluder = occluder_owner.get_or_null(r_instance.occluder);
	if (!occluder || occluder->vertices.is_empty()) {
		r_instance.world_aabb = AABB();
		return;
	}
	r_instance.world_aabb = r_instance.xform.xform(occluder->aabb);
}

void HZBOcclusionCull::scenario_set_instance(RID p_scenario, RID p_instance, RID p_occluder, const Transform3D &p_xform, bool p_enabled) {
	Scenario *scenario = scenarios.getptr(p_scenario);
	ERR_FAIL_NULL(scenario);

	if (!scenario->instances.has(p_instance)) {
		scenario->instances[p_instance] = OccluderInstance();
	}

	OccluderInstance &instance = scenario->instances[p_instance];

	if (instance.removed) {
		instance.removed = false;
		scenario->removed_instances.erase(p_instance);
	}

	if (instance.occluder != p_occluder) {
		Occluder *old_occluder = occluder_owner.get_or_null(instance.occluder);
		if (old_occluder) {
			old_occluder->users.erase(InstanceID(p_scenario, p_instance));
		}

		instance.occluder = p_occluder;

		if (p_occluder.is_valid()) {
			Occluder *occluder = occluder_owner.get_or_null(p_occluder);
			ERR_FAIL_NULL(occluder);
			occluder->users.insert(InstanceID(p_scenario, p_instance));
		}
	}

	instance.xform = p_xform;
	instance.enabled = p_enabled;
	_update_instance_aabb(instance);
}

void HZBOcclusionCull::scenario_remove_instance(RID p_scenario, RID p_instance) {
	Scenario *scenario = scenarios.getptr(p_scenario);
	ERR_FAIL_NULL(scenario);

	OccluderInstance *instance = scenario->instances.getptr(p_instance);
	if (!instance || instance->removed) {
		return;
	}

	Occluder *occluder = occluder_owner.get_or_null(instance->occluder);
	if (occluder) {
		occluder->users.erase(InstanceID(p_scenario, p_instance));
	}

	// Deferred, like the Embree backend does it: instances are commonly
	// removed and re-added within the same frame (any transform change on a
	// hidden occluder does it), and the scenario is only compacted once, right
	// before it is rendered.
	instance->removed = true;
	scenario->removed_instances.push_back(p_instance);
}

////////////////////////////////////////////////////////
// Buffers.

void HZBOcclusionCull::add_buffer(RID p_buffer) {
	ERR_FAIL_COND(buffers.has(p_buffer));
	buffers[p_buffer] = RasterHZBuffer();
}

void HZBOcclusionCull::remove_buffer(RID p_buffer) {
	ERR_FAIL_COND(!buffers.has(p_buffer));
	buffers.erase(p_buffer);
}

RendererSceneOcclusionCull::HZBuffer *HZBOcclusionCull::buffer_get_ptr(RID p_buffer) {
	return buffers.getptr(p_buffer);
}

void HZBOcclusionCull::buffer_set_scenario(RID p_buffer, RID p_scenario) {
	RasterHZBuffer *buffer = buffers.getptr(p_buffer);
	ERR_FAIL_NULL(buffer);
	ERR_FAIL_COND(p_scenario.is_valid() && !scenarios.has(p_scenario));
	buffer->scenario_rid = p_scenario;
}

void HZBOcclusionCull::buffer_set_size(RID p_buffer, const Vector2i &p_size) {
	RasterHZBuffer *buffer = buffers.getptr(p_buffer);
	ERR_FAIL_NULL(buffer);
	buffer->resize(p_size);
}

RID HZBOcclusionCull::buffer_get_debug_texture(RID p_buffer, bool p_pyramid) {
	RasterHZBuffer *buffer = buffers.getptr(p_buffer);
	ERR_FAIL_NULL_V(buffer, RID());
	return p_pyramid ? buffer->get_debug_pyramid_texture() : buffer->get_debug_texture();
}

Vector2 HZBOcclusionCull::_get_jitter(const Size2i &p_buffer_size) const {
	if (!jitter_enabled || p_buffer_size.x <= 0 || p_buffer_size.y <= 0) {
		return Vector2();
	}

	// Same 9 frame pattern the Embree backend jitters its rays with, expressed
	// directly in depth buffer pixels: subpixel samples at 0, 1/3 and 2/3.
	static const Vector2 pattern[9] = {
		Vector2(0, 0),
		Vector2(-1, -1),
		Vector2(1, -1),
		Vector2(-1, 1),
		Vector2(1, 1),
		Vector2(-0.5f, -0.5f),
		Vector2(0.5f, -0.5f),
		Vector2(-0.5f, 0.5f),
		Vector2(0.5f, 0.5f),
	};

	return pattern[Engine::get_singleton()->get_frames_drawn() % 9] * 0.33f;
}

void HZBOcclusionCull::_gather_render_items(Scenario &p_scenario, const Transform3D &p_cam_transform, const Projection &p_cam_projection, const Size2i &p_buffer_size) {
	render_items.clear();

	for (const RID &instance : p_scenario.removed_instances) {
		p_scenario.instances.erase(instance);
	}
	p_scenario.removed_instances.clear();

	const Vector<Plane> planes = p_cam_projection.get_projection_planes(p_cam_transform);
	// AABB::intersects_convex_shape() also wants the shape's own corners: with
	// none of them it takes every box to be separated from the frustum on
	// every axis and rejects the lot.
	Vector3 frustum_points[8];
	const bool has_frustum_points = p_cam_projection.get_endpoints(p_cam_transform, frustum_points);

	const Transform3D cam_inv_transform = p_cam_transform.affine_inverse();
	const real_t z_near = p_cam_projection.get_z_near();

	uint64_t total_triangles = 0;

	for (KeyValue<RID, OccluderInstance> &E : p_scenario.instances) {
		const OccluderInstance &instance = E.value;
		if (!instance.enabled) {
			continue;
		}

		const Occluder *occluder = occluder_owner.get_or_null(instance.occluder);
		if (!occluder || occluder->indices.size() < 3 || occluder->vertices.is_empty()) {
			continue;
		}

		if (has_frustum_points && !instance.world_aabb.intersects_convex_shape(planes.ptr(), planes.size(), frustum_points, 8)) {
			continue;
		}

		// Screen footprint, in depth buffer pixels: both the cutoff for
		// occluders too small to matter and the priority used to spend the
		// triangle budget on the occluders that hide the most.
		float screen_area = FLT_MAX;
		Vector2 rect_min(FLT_MAX, FLT_MAX);
		Vector2 rect_max(-FLT_MAX, -FLT_MAX);
		bool crosses_near_plane = false;

		for (int i = 0; i < 8; i++) {
			const Vector3 corner = instance.world_aabb.get_endpoint(i);
			const Vector3 view = cam_inv_transform.xform(corner);
			if (-view.z < z_near) {
				crosses_near_plane = true;
				break;
			}
			const Vector3 projected = p_cam_projection.xform(view);
			rect_min = rect_min.min(Vector2(projected.x, projected.y));
			rect_max = rect_max.max(Vector2(projected.x, projected.y));
		}

		if (!crosses_near_plane) {
			// Half the normalized device coordinate range is the full buffer.
			const Vector2 extents = (rect_max - rect_min) * 0.5f * Vector2(p_buffer_size.x, p_buffer_size.y);
			screen_area = MAX(extents.x, 0.0f) * MAX(extents.y, 0.0f);
			if (screen_area < MIN_OCCLUDER_SCREEN_AREA) {
				continue;
			}
		}

		RenderItem item;
		item.occluder = occluder;
		item.xform = instance.xform;
		item.screen_area = screen_area;
		item.triangle_count = occluder->indices.size() / 3;
		render_items.push_back(item);

		total_triangles += item.triangle_count;
	}

	if (triangle_budget <= 0 || total_triangles <= (uint64_t)triangle_budget) {
		return;
	}

	// Over budget: keep rasterizing the occluders that cover the most screen
	// and drop the rest. Dropping an occluder only ever means culling less, so
	// this stays correct, it just gets less effective the further past the
	// budget a scene goes.
	struct RenderItemComparator {
		_FORCE_INLINE_ bool operator()(const RenderItem &a, const RenderItem &b) const {
			return a.screen_area > b.screen_area;
		}
	};
	render_items.sort_custom<RenderItemComparator>();

	uint64_t kept_triangles = 0;
	uint32_t kept_items = 0;
	for (const RenderItem &item : render_items) {
		if (kept_triangles + item.triangle_count > (uint64_t)triangle_budget && kept_items > 0) {
			break;
		}
		kept_triangles += item.triangle_count;
		kept_items++;
	}
	render_items.resize(kept_items);
}

void HZBOcclusionCull::buffer_update(RID p_buffer, const Transform3D &p_cam_transform, const Projection &p_cam_projection, bool p_cam_orthogonal) {
	RasterHZBuffer *buffer = buffers.getptr(p_buffer);
	if (!buffer || buffer->is_empty()) {
		return;
	}

	Scenario *scenario = scenarios.getptr(buffer->scenario_rid);
	if (scenario) {
		_gather_render_items(*scenario, p_cam_transform, p_cam_projection, buffer->get_size());
	} else {
		render_items.clear();
	}

	buffer->render(render_items, p_cam_transform, p_cam_projection, p_cam_orthogonal, _get_jitter(buffer->get_size()));
}

HZBOcclusionCull::HZBOcclusionCull() {
	jitter_enabled = GLOBAL_GET("rendering/occlusion_culling/jitter_projection");
	triangle_budget = GLOBAL_GET("rendering/occlusion_culling/occluder_triangle_budget");
}

HZBOcclusionCull::~HZBOcclusionCull() {
	const LocalVector<RID> occluders = occluder_owner.get_owned_list();
	for (const RID &occluder : occluders) {
		free_occluder(occluder);
	}
}

////////////////////////////////////////////////////////
// Rasterization.

void HZBOcclusionCull::RasterHZBuffer::_clear_depth() {
	const Size2i size = sizes[0];
	float *depth = mips[0];
	const int pixel_count = size.x * size.y;
	for (int i = 0; i < pixel_count; i++) {
		depth[i] = FLT_MAX;
	}
}

void HZBOcclusionCull::RasterHZBuffer::_depth_to_distance(const Projection &p_cam_projection, bool p_cam_orthogonal) {
	if (p_cam_orthogonal) {
		// HZBuffer::_is_occluded() compares against view space depth for an
		// orthogonal camera, which is exactly what was rasterized.
		return;
	}

	const Size2i size = sizes[0];
	const float z_near = p_cam_projection.get_z_near();
	if (z_near <= 0.0f) {
		return;
	}

	// Every pixel looks along its own ray, so its view depth has to be scaled
	// by that ray's length to become the distance from the camera position the
	// occlusion test expects. The two squared terms only depend on the pixel's
	// column and row, so they are tabulated instead of recomputed per pixel.
	const Rect2 viewport_rect = _get_viewport_rect(p_cam_projection);

	ray_length_columns.resize(size.x);
	ray_length_rows.resize(size.y);

	for (int x = 0; x < size.x; x++) {
		const float px = viewport_rect.position.x + (x + 0.5f) * viewport_rect.size.x / size.x;
		ray_length_columns[x] = px * px;
	}
	for (int y = 0; y < size.y; y++) {
		const float py = viewport_rect.position.y + (y + 0.5f) * viewport_rect.size.y / size.y;
		ray_length_rows[y] = py * py + z_near * z_near;
	}

	const float inv_z_near = 1.0f / z_near;
	float *depth = mips[0];

	for (int y = 0; y < size.y; y++) {
		float *row = &depth[y * size.x];
		const float row_term = ray_length_rows[y];
		for (int x = 0; x < size.x; x++) {
			if (row[x] == FLT_MAX) {
				continue;
			}
			row[x] *= Math::sqrt(ray_length_columns[x] + row_term) * inv_z_near;
		}
	}
}

void HZBOcclusionCull::RasterHZBuffer::_push_triangle(RasterSlice &r_slice, const ScreenVertex &p_a, const ScreenVertex &p_b, const ScreenVertex &p_c, const Size2i &p_size, int p_band_count, int p_band_height) {
	const ScreenVertex *v0 = &p_a;
	const ScreenVertex *v1 = &p_b;
	const ScreenVertex *v2 = &p_c;

	float area = (v1->x - v0->x) * (v2->y - v0->y) - (v2->x - v0->x) * (v1->y - v0->y);
	if (area < 0.0f) {
		// Rasterized with a single, positive winding, so back faces need no
		// separate code path: the depth test keeps whichever surface is
		// closest to the camera anyway.
		SWAP(v1, v2);
		area = -area;
	}

	// Written as a rejection so that a triangle carrying a NaN (a degenerate
	// occluder mesh is enough) is dropped rather than rasterized.
	if (!(area >= MIN_TRIANGLE_AREA)) {
		return;
	}

	// Pixel centers sit at (x + 0.5, y + 0.5), so these are the first and last
	// centers the triangle's bounds can contain.
	const int min_x = MAX((int)Math::ceil(MIN(v0->x, MIN(v1->x, v2->x)) - 0.5f), 0);
	const int max_x = MIN((int)Math::floor(MAX(v0->x, MAX(v1->x, v2->x)) - 0.5f), p_size.x - 1);
	if (min_x > max_x) {
		return;
	}

	const int min_y = MAX((int)Math::ceil(MIN(v0->y, MIN(v1->y, v2->y)) - 0.5f), 0);
	const int max_y = MIN((int)Math::floor(MAX(v0->y, MAX(v1->y, v2->y)) - 0.5f), p_size.y - 1);
	if (min_y > max_y) {
		return;
	}

	RasterTriangle triangle;
	const ScreenVertex *vertices[3] = { v0, v1, v2 };
	for (int i = 0; i < 3; i++) {
		triangle.x[i] = vertices[i]->x;
		triangle.y[i] = vertices[i]->y;
		triangle.inv_w[i] = vertices[i]->inv_w;
		triangle.depth_over_w[i] = vertices[i]->depth_over_w;
	}
	triangle.min_x = min_x;
	triangle.max_x = max_x;
	triangle.min_y = min_y;
	triangle.max_y = max_y;

	const uint32_t index = r_slice.triangles.size();
	r_slice.triangles.push_back(triangle);

	const int first_band = MIN(min_y / p_band_height, p_band_count - 1);
	const int last_band = MIN(max_y / p_band_height, p_band_count - 1);
	for (int band = first_band; band <= last_band; band++) {
		r_slice.bins[band].push_back(index);
	}
}

void HZBOcclusionCull::RasterHZBuffer::_emit_polygon(RasterSlice &r_slice, const ScreenVertex *p_polygon, int p_count, const Size2i &p_size, int p_band_count, int p_band_height) {
	if (p_count < 3) {
		return;
	}

	float min_x = p_polygon[0].x;
	float max_x = p_polygon[0].x;
	float min_y = p_polygon[0].y;
	float max_y = p_polygon[0].y;
	for (int i = 1; i < p_count; i++) {
		min_x = MIN(min_x, p_polygon[i].x);
		max_x = MAX(max_x, p_polygon[i].x);
		min_y = MIN(min_y, p_polygon[i].y);
		max_y = MAX(max_y, p_polygon[i].y);
	}

	if (max_x < 0.0f || max_y < 0.0f || min_x > p_size.x || min_y > p_size.y) {
		return;
	}

	// Clipped to the buffer rectangle rather than just scissored to it: a
	// triangle reaching far outside the screen would otherwise carry pixel
	// coordinates so large that the edge functions lose all precision near
	// zero, which is exactly where the inside test reads them. Both
	// interpolants are affine in screen space, so interpolating them along the
	// clip edges is exact.
	ScreenVertex clipped[MAX_CLIPPED_VERTICES];
	ScreenVertex scratch[MAX_CLIPPED_VERTICES];
	const ScreenVertex *polygon = p_polygon;
	int count = p_count;

	if (min_x < 0.0f || min_y < 0.0f || max_x > p_size.x || max_y > p_size.y) {
		const ScreenVertex *source = p_polygon;
		int source_count = p_count;

		for (int plane = 0; plane < 4; plane++) {
			const int axis = plane < 2 ? 0 : 1;
			const bool keep_greater = (plane % 2) == 0;
			const float limit = keep_greater ? 0.0f : (axis == 0 ? (float)p_size.x : (float)p_size.y);

			ScreenVertex *destination = (plane % 2) == 0 ? clipped : scratch;
			int destination_count = 0;

			for (int i = 0; i < source_count; i++) {
				const ScreenVertex &a = source[i];
				const ScreenVertex &b = source[(i + 1) % source_count];

				const float a_value = axis == 0 ? a.x : a.y;
				const float b_value = axis == 0 ? b.x : b.y;
				const float a_distance = keep_greater ? (a_value - limit) : (limit - a_value);
				const float b_distance = keep_greater ? (b_value - limit) : (limit - b_value);

				if (a_distance >= 0.0f) {
					if (destination_count >= MAX_CLIPPED_VERTICES) {
						break;
					}
					destination[destination_count++] = a;
				}

				if ((a_distance >= 0.0f) != (b_distance >= 0.0f)) {
					if (destination_count >= MAX_CLIPPED_VERTICES) {
						break;
					}
					const float t = a_distance / (a_distance - b_distance);
					ScreenVertex &out = destination[destination_count++];
					out.x = a.x + (b.x - a.x) * t;
					out.y = a.y + (b.y - a.y) * t;
					out.inv_w = a.inv_w + (b.inv_w - a.inv_w) * t;
					out.depth_over_w = a.depth_over_w + (b.depth_over_w - a.depth_over_w) * t;
				}
			}

			source = destination;
			source_count = destination_count;

			if (source_count < 3) {
				return;
			}
		}

		polygon = source;
		count = source_count;
	}

	for (int i = 1; i + 1 < count; i++) {
		_push_triangle(r_slice, polygon[0], polygon[i], polygon[i + 1], p_size, p_band_count, p_band_height);
	}
}

void HZBOcclusionCull::RasterHZBuffer::_setup_slice(uint32_t p_slice, const SetupThreadData *p_data) {
	RasterSlice &slice = slices[p_slice];
	slice.triangles.clear();
	for (LocalVector<uint32_t> &bin : slice.bins) {
		bin.clear();
	}

	const Size2i size = sizes[0];
	const uint32_t first_item = p_data->slice_first_item[p_slice];
	const uint32_t last_item = p_data->slice_first_item[p_slice + 1];

	for (uint32_t i = first_item; i < last_item; i++) {
		const RenderItem &item = p_data->items[i];
		const Occluder *occluder = item.occluder;

		const int vertex_count = occluder->vertices.size();
		if (slice.vertices.size() < (uint32_t)vertex_count) {
			slice.vertices.resize(vertex_count);
		}

		const Transform3D view_xform = p_data->cam_inv_transform * item.xform;
		const Vector3 *source_vertices = occluder->vertices.ptr();
		ProjectedVertex *vertices = slice.vertices.ptr();

		for (int v = 0; v < vertex_count; v++) {
			ProjectedVertex &vertex = vertices[v];
			vertex.view = view_xform.xform(source_vertices[v]);
			vertex.in_front = -vertex.view.z >= p_data->z_near;
			if (vertex.in_front) {
				vertex.screen = _project_view_position(vertex.view, p_data->cam_projection, size, p_data->jitter);
			}
		}

		const int *indices = occluder->indices.ptr();
		const int index_count = occluder->indices.size();

		for (int t = 0; t + 2 < index_count; t += 3) {
			const int i0 = indices[t];
			const int i1 = indices[t + 1];
			const int i2 = indices[t + 2];
			if (i0 < 0 || i1 < 0 || i2 < 0 || i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count) {
				continue;
			}

			const ProjectedVertex &v0 = vertices[i0];
			const ProjectedVertex &v1 = vertices[i1];
			const ProjectedVertex &v2 = vertices[i2];

			if (v0.in_front && v1.in_front && v2.in_front) {
				const ScreenVertex polygon[3] = { v0.screen, v1.screen, v2.screen };
				_emit_polygon(slice, polygon, 3, size, p_data->band_count, p_data->band_height);
				continue;
			}

			if (!v0.in_front && !v1.in_front && !v2.in_front) {
				continue;
			}

			// Straddles the near plane: clip it there in view space, where the
			// projection is still well behaved, then project what is left.
			const Vector3 view_triangle[3] = { v0.view, v1.view, v2.view };
			Vector3 clipped[4];
			int clipped_count = 0;

			for (int e = 0; e < 3; e++) {
				const Vector3 &a = view_triangle[e];
				const Vector3 &b = view_triangle[(e + 1) % 3];
				const float a_distance = -a.z - p_data->z_near;
				const float b_distance = -b.z - p_data->z_near;

				if (a_distance >= 0.0f && clipped_count < 4) {
					clipped[clipped_count++] = a;
				}
				if ((a_distance >= 0.0f) != (b_distance >= 0.0f) && clipped_count < 4) {
					clipped[clipped_count++] = a.lerp(b, a_distance / (a_distance - b_distance));
				}
			}

			if (clipped_count < 3) {
				continue;
			}

			ScreenVertex polygon[4];
			for (int v = 0; v < clipped_count; v++) {
				polygon[v] = _project_view_position(clipped[v], p_data->cam_projection, size, p_data->jitter);
			}
			_emit_polygon(slice, polygon, clipped_count, size, p_data->band_count, p_data->band_height);
		}
	}
}

void HZBOcclusionCull::RasterHZBuffer::_rasterize_band(uint32_t p_band, const RasterThreadData *p_data) {
	const Size2i size = sizes[0];
	const int y_begin = p_band * p_data->band_height;
	const int y_end = MIN(y_begin + p_data->band_height, size.y);
	if (y_begin >= y_end) {
		return;
	}

	float *depth = mips[0];

	for (uint32_t s = 0; s < p_data->slice_count; s++) {
		const RasterSlice &slice = slices[s];
		const LocalVector<uint32_t> &bin = slice.bins[p_band];

		for (uint32_t i = 0; i < bin.size(); i++) {
			const RasterTriangle &triangle = slice.triangles[bin[i]];

			const int y_from = MAX(triangle.min_y, y_begin);
			const int y_to = MIN(triangle.max_y, y_end - 1);
			if (y_from > y_to) {
				continue;
			}

			// Standard half space rasterization: one edge function per vertex,
			// each linear in screen space and stepped incrementally along the
			// scanline. Their sum is the (positive) doubled triangle area, so
			// they double as unnormalized barycentric weights.
			const float a0 = triangle.y[1] - triangle.y[2];
			const float b0 = triangle.x[2] - triangle.x[1];
			const float c0 = triangle.x[1] * triangle.y[2] - triangle.x[2] * triangle.y[1];

			const float a1 = triangle.y[2] - triangle.y[0];
			const float b1 = triangle.x[0] - triangle.x[2];
			const float c1 = triangle.x[2] * triangle.y[0] - triangle.x[0] * triangle.y[2];

			const float a2 = triangle.y[0] - triangle.y[1];
			const float b2 = triangle.x[1] - triangle.x[0];
			const float c2 = triangle.x[0] * triangle.y[1] - triangle.x[1] * triangle.y[0];

			const float first_x = triangle.min_x + 0.5f;

			for (int y = y_from; y <= y_to; y++) {
				const float center_y = y + 0.5f;
				float e0 = a0 * first_x + b0 * center_y + c0;
				float e1 = a1 * first_x + b1 * center_y + c1;
				float e2 = a2 * first_x + b2 * center_y + c2;

				float *row = &depth[y * size.x];

				for (int x = triangle.min_x; x <= triangle.max_x; x++, e0 += a0, e1 += a1, e2 += a2) {
					if (e0 < 0.0f || e1 < 0.0f || e2 < 0.0f) {
						continue;
					}

					// Perspective correct depth. The common 1 / (2 * area)
					// factor of the barycentric weights cancels out in the
					// division, so it is left out of both sums.
					const float inv_w = e0 * triangle.inv_w[0] + e1 * triangle.inv_w[1] + e2 * triangle.inv_w[2];
					if (inv_w <= 0.0f) {
						continue;
					}

					const float depth_over_w = e0 * triangle.depth_over_w[0] + e1 * triangle.depth_over_w[1] + e2 * triangle.depth_over_w[2];
					const float pixel_depth = depth_over_w / inv_w;

					if (pixel_depth < row[x]) {
						row[x] = pixel_depth;
					}
				}
			}
		}
	}
}

void HZBOcclusionCull::RasterHZBuffer::render(const LocalVector<RenderItem> &p_items, const Transform3D &p_cam_transform, const Projection &p_cam_projection, bool p_cam_orthogonal, const Vector2 &p_jitter) {
	if (is_empty()) {
		return;
	}

	_clear_depth();

	const Size2i size = sizes[0];

	uint64_t total_triangles = 0;
	for (const RenderItem &item : p_items) {
		total_triangles += item.triangle_count;
	}

	if (total_triangles > 0) {
		uint32_t slice_count = 1;
		if (total_triangles >= MIN_TRIANGLES_FOR_THREADING) {
			slice_count = CLAMP((uint32_t)WorkerThreadPool::get_singleton()->get_thread_count(), 1u, p_items.size());
		}

		const int band_count = slice_count == 1 ? 1 : CLAMP((int)slice_count * 2, 1, size.y);
		const int band_height = (size.y + band_count - 1) / band_count;

		if (slices.size() < slice_count) {
			slices.resize(slice_count);
		}
		for (uint32_t s = 0; s < slice_count; s++) {
			if (slices[s].bins.size() != (uint32_t)band_count) {
				slices[s].bins.resize(band_count);
			}
		}

		// Split the frame's occluders into one slice of whole occluders per
		// worker thread, balanced by triangle count so that every thread gets
		// a comparable amount of scan conversion to set up.
		LocalVector<uint32_t> slice_first_item;
		slice_first_item.resize(slice_count + 1);
		slice_first_item[0] = 0;
		slice_first_item[slice_count] = p_items.size();

		uint64_t accumulated_triangles = 0;
		uint32_t next_slice = 1;
		for (uint32_t i = 0; i < p_items.size() && next_slice < slice_count; i++) {
			accumulated_triangles += p_items[i].triangle_count;
			while (next_slice < slice_count && accumulated_triangles * slice_count >= total_triangles * next_slice) {
				slice_first_item[next_slice++] = i + 1;
			}
		}
		for (uint32_t s = next_slice; s < slice_count; s++) {
			slice_first_item[s] = p_items.size();
		}

		SetupThreadData setup_data;
		setup_data.items = p_items.ptr();
		setup_data.slice_first_item = slice_first_item.ptr();
		setup_data.cam_inv_transform = p_cam_transform.affine_inverse();
		setup_data.cam_projection = p_cam_projection;
		setup_data.jitter = p_jitter;
		setup_data.z_near = p_cam_projection.get_z_near();
		setup_data.band_count = band_count;
		setup_data.band_height = band_height;

		if (slice_count == 1) {
			_setup_slice(0, &setup_data);
		} else {
			WorkerThreadPool::GroupID group_task = WorkerThreadPool::get_singleton()->add_template_group_task(this, &RasterHZBuffer::_setup_slice, &setup_data, slice_count, -1, true, SNAME("HZBOcclusionCullSetup"));
			WorkerThreadPool::get_singleton()->wait_for_group_task_completion(group_task);
		}

		RasterThreadData raster_data;
		raster_data.band_height = band_height;
		raster_data.slice_count = slice_count;

		if (band_count == 1) {
			_rasterize_band(0, &raster_data);
		} else {
			WorkerThreadPool::GroupID group_task = WorkerThreadPool::get_singleton()->add_template_group_task(this, &RasterHZBuffer::_rasterize_band, &raster_data, band_count, -1, true, SNAME("HZBOcclusionCullRaster"));
			WorkerThreadPool::get_singleton()->wait_for_group_task_completion(group_task);
		}

		_depth_to_distance(p_cam_projection, p_cam_orthogonal);
	}

	debug_tex_range = p_cam_projection.get_z_far();

	set_camera(p_cam_transform, p_cam_projection, p_cam_orthogonal);
	update_mips();
}
