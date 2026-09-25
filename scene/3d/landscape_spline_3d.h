/**************************************************************************/
/*  landscape_spline_3d.h                                                 */
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

#include "core/templates/local_vector.h"
#include "core/variant/typed_array.h"
#include "scene/3d/path_3d.h"
#include "scene/3d/visual_instance_3d.h"

class Curve;
class Landscape3D;
class Shader;
class ShaderMaterial;

// A road, river or stream laid along a spline: a strip of mesh swept along the
// Path3D's curve, draped over (or cut into) a Landscape3D, with a material on
// it. Being a Path3D, its curve is drawn and edited with the ordinary Path3D
// tools; turning on their "Snap to Colliders" option drops new points straight
// onto the terrain. With fill on, a closed curve is a shoreline instead, and
// what it encloses is filled with a level surface: a lake.
//
// Built to cross a whole terrain without costing as much as one:
//  - The strip is cut into chunks of about chunk_length along the spline, each
//    its own RenderingServer instance with its own bounds, so frustum and
//    occlusion culling skip the parts out of view, and mesh LOD (which picks a
//    level per instance, from the distance to its bounds) coarsens distant
//    parts instead of the whole strip being as close as its nearest point.
//  - Each chunk is simplified before it is built: cross-sections that lie
//    within simplify_tolerance of the surface their neighbors already describe
//    are dropped, as are columns across the width, so straight, even stretches
//    cost a handful of triangles however finely segment_length samples them.
//    Its LOD levels are measured, not guessed: every level records the largest
//    distance between its surface and the full one, which is what the renderer
//    compares against the screen to pick a level.
//  - Chunks are rebuilt only when what they are built from changes. Every
//    update regenerates just the chunk's vertex positions (cheap), and a chunk
//    whose positions and settings hash the same as last time keeps its mesh,
//    so editing one end of a long road, or sculpting the terrain under one
//    part of it, only rebuilds the chunks involved. Terrain edits arrive
//    through Landscape3D's terrain_changed signal, which says which region
//    changed. The chunks that do need rebuilding are built in parallel.
//  - Casts no shadow by default: a strip lying on the ground has nothing to
//    shadow, and a long one would otherwise be drawn into every shadow cascade.
//  - Optional per-chunk visibility range, for splines that are not worth
//    drawing at all past some distance.
//  - A filled area is cut into square tiles instead. Tiles wholly inside the
//    shoreline are two triangles each; only those the shoreline crosses are
//    clipped to it, and the shoreline is simplified first, so a lake costs a
//    few triangles per tile plus its outline, however large it is.
class LandscapeSpline3D : public Path3D {
	GDCLASS(LandscapeSpline3D, Path3D);

public:
	// What the spline is. Only chooses the built-in material used when
	// material is empty; everything else the type implies is ordinary
	// properties, which apply_preset() (and the editor, when the type is
	// changed in the Inspector) set to suit it.
	enum SplineType {
		TYPE_ROAD,
		TYPE_RIVER,
		TYPE_STREAM,
		TYPE_LAKE,
		TYPE_MAX,
	};

	// Where the strip's surface is, vertically.
	enum HeightMode {
		// Every vertex sits on the terrain (plus height_offset), across the
		// width as well as along it. For roads and paths laid over uneven
		// ground.
		HEIGHT_MODE_CONFORM,
		// Follows the terrain under the centerline along its length, but stays
		// level across its width (banked by the curve's tilt). For streams: the
		// water runs downhill with the ground, but its surface is flat.
		HEIGHT_MODE_CONFORM_LEVEL,
		// Follows the curve's own height and tilt, ignoring the terrain. For
		// rivers, whose surface has to stay level and smooth where the ground
		// under it does not, and anything raised off the ground.
		HEIGHT_MODE_SPLINE,
	};

	static constexpr int MAX_CROSS_SEGMENTS = 64;

private:
	// One cross-section of the strip, sampled every segment_length along the
	// curve, in this node's local space.
	struct Ring {
		Vector3 center;
		// Across the strip, level and then banked by the curve's tilt.
		Vector3 right;
		// Across the strip, level (no tilt): the direction conformed vertices
		// are spread along before being dropped onto the terrain.
		Vector3 flat_right;
		float distance = 0.0;
		float half_width = 0.0;
		// Vertex alpha from end_fade_length; 1 away from the ends.
		float end_alpha = 1.0;
	};

	// One sample across the width: u runs from 0 at the left edge to 1 at the
	// right one.
	struct Column {
		float u = 0.0;
		float alpha = 1.0;
		// Always kept, by simplification and every LOD level: the edges, and
		// where edge_fade finishes fading in (which linear interpolation
		// across a wider span could not reproduce).
		bool feature = false;
	};

	// How a tile of a filled area is built (see _partition_fill()).
	enum FillTile : uint8_t {
		FILL_TILE_NONE, // A strip chunk, not part of a fill.
		FILL_TILE_FULL, // Wholly inside the shoreline: one quad.
		FILL_TILE_CLIPPED, // Crossed by the shoreline: clipped to it.
	};

	struct Chunk {
		int first_ring = 0;
		int last_ring = 0;
		// Fills only: which tile, in units of the fill tile size.
		Vector2i tile;
		FillTile fill_tile = FILL_TILE_NONE;
		// Of the positions and settings the mesh was last built from; a chunk
		// that hashes the same is left alone.
		uint64_t hash = 0;
		bool built = false;
		// Set by terrain_changed for chunks lying over the changed region.
		bool dirty = true;
		// What the chunk covers in the landscape's local XZ, padded by one
		// terrain sample, to tell which chunks a terrain edit reaches.
		Rect2 terrain_bounds;
		RID mesh;
		RID instance;
	};

	// Built on worker threads (see _build_chunk), then handed to the
	// RenderingServer on the main thread.
	struct ChunkBuild {
		int chunk_index = 0;
		uint64_t hash = 0;
		// False when the chunk hashed the same as its current mesh.
		bool changed = false;
		bool empty = true;
		Rect2 terrain_bounds;
		Array arrays;
		Dictionary lods;
	};

	// Reads a Landscape3D's heights directly (no per-sample calls), matching
	// the triangles the terrain actually renders rather than interpolating
	// bilinearly, so a conformed vertex lands on the visible surface.
	struct TerrainSampler {
		Vector<uint8_t> data; // Keeps the heightmap buffer alive and unchanged.
		const float *heights = nullptr;
		int resolution = 0;
		float spacing = 1.0;
		Transform3D to_terrain;
		Transform3D from_terrain;

		bool is_valid() const { return heights != nullptr; }
		float height_at(float p_x, float p_z) const;
		// p_local (this node's space) moved along the terrain's up axis onto
		// its surface, plus p_offset.
		Vector3 project(const Vector3 &p_local, float p_offset) const;
	};

	struct BuildContext {
		ChunkBuild *builds = nullptr;
		uint64_t settings_hash = 0;
		bool use_terrain = false;
		TerrainSampler sampler;

		// Fills only. The shoreline, simplified, in the plane's 2D
		// coordinates: x along fill_x, y along fill_y.
		Vector<Vector2> fill_polygon;
		// Height of the surface along rings_up.
		float fill_level = 0.0;
		Rect2 fill_terrain_bounds;
	};

	enum UpdateFlags {
		UPDATE_CHUNKS = 1, // Only the chunks marked dirty.
		UPDATE_ALL_CHUNKS = 2,
		UPDATE_RINGS = 4, // Resample the curve, then every chunk.
	};

	SplineType spline_type = TYPE_ROAD;
	NodePath landscape_path;
	Ref<Material> material;

	float width = 6.0;
	Ref<Curve> width_curve;
	HeightMode height_mode = HEIGHT_MODE_CONFORM;
	float height_offset = 0.05;
	bool smooth = true;
	bool fill = false;

	float segment_length = 1.0;
	int cross_segments = 6;
	float simplify_tolerance = 0.02;
	float chunk_length = 64.0;
	Vector2 uv_scale = Vector2(1, 1);
	float edge_fade = 0.08;
	float end_fade_length = 0.0;

	GeometryInstance3D::ShadowCastingSetting cast_shadow = GeometryInstance3D::SHADOW_CASTING_SETTING_OFF;
	GeometryInstance3D::GIMode gi_mode = GeometryInstance3D::GI_MODE_STATIC;
	uint32_t render_layers = 1;
	float lod_bias = 1.0;
	float visibility_range_end = 0.0;
	float visibility_range_end_margin = 0.0;

	bool carve_enabled = true;
	float carve_depth = 0.0;
	float carve_falloff = 4.0;
	int paint_layer = -1;
	float paint_strength = 1.0;
	float paint_falloff = 2.0;

	LocalVector<Ring> rings;
	LocalVector<Column> columns;
	LocalVector<Chunk> chunks;
	float total_length = 0.0;
	// Distance between consecutive rings (except the last pair, which is
	// shorter): segment_length, unless that would make too many.
	float ring_step = 1.0;
	// Up in this node's space, which the rings were sampled against: the
	// landscape's up axis when there is one.
	Vector3 rings_up = Vector3(0, 1, 0);
	bool rings_closed = false;

	uint32_t pending_update = 0;
	bool update_queued = false;
	ObjectID landscape_id;

	static inline Ref<Shader> road_shader;
	static inline Ref<Shader> water_shader;
	static inline Ref<ShaderMaterial> default_materials[TYPE_MAX];

	void _queue_update(uint32_t p_flags);
	void _update();
	void _update_rings();
	void _update_columns();
	void _partition_chunks();

	bool _is_filled() const;
	float _get_fill_tile_size() const;
	// The level of a filled area along rings_up: the lowest point of its
	// shoreline, on the ground or on the curve (see HeightMode), plus
	// height_offset when p_with_offset.
	float _get_fill_level(const TerrainSampler &p_sampler, bool p_use_terrain, bool p_with_offset) const;
	// The shoreline in the fill plane's 2D coordinates, closed implicitly
	// (its last point is not a repeat of its first).
	Vector<Vector2> _get_fill_shoreline(bool p_simplified) const;
	void _partition_fill();
	void _build_fill_chunk(uint32_t p_index, BuildContext *p_context);
	// The fill plane's axes, perpendicular to rings_up: UV.x runs along the
	// first, UV.y along the second.
	Vector3 fill_x = Vector3(1, 0, 0);
	Vector3 fill_y = Vector3(0, 0, 1);
	// Set by _partition_fill(): the simplified shoreline the tiles are
	// clipped to, and the tiles' size (normally four chunk_lengths, larger
	// for an area that would otherwise need a great many of them).
	Vector<Vector2> fill_shoreline;
	float fill_tile_size = 256.0;
	void _free_chunk(Chunk &p_chunk);
	void _clear_chunks();

	Landscape3D *_find_landscape() const;
	Landscape3D *_get_landscape() const;
	void _update_landscape_connection();
	void _disconnect_landscape();
	void _on_terrain_changed(const Rect2i &p_region);
	bool _make_terrain_sampler(TerrainSampler &r_sampler) const;
	bool _make_terrain_sampler_for(Landscape3D *p_landscape, TerrainSampler &r_sampler) const;
	Vector3 _get_local_up() const;

	Ref<Curve3D> _make_sampling_curve(const Ref<Curve3D> &p_curve) const;
	int _wrap_ring(int p_index) const;
	uint64_t _get_settings_hash() const;
	void _build_chunk(uint32_t p_index, BuildContext *p_context);
	void _commit_chunk(Chunk &p_chunk, const ChunkBuild &p_build);
	void _apply_render_settings(const Chunk &p_chunk) const;
	void _apply_render_settings_to_all();
	void _apply_material_to_all();
	RID _get_material_rid() const;
	RID _get_scenario() const;
	void _on_shape_changed();
	void _on_curve_changed();

	// A ring as apply_to_landscape() sees it, in the landscape's local space.
	struct RingOnTerrain {
		Vector2 center; // XZ.
		float height = 0.0; // The curve's own, not the ground's.
		Vector2 side; // Unit, XZ, pointing right.
		float half_width = 0.0;
		// Height gained per unit of distance to the right, from the tilt.
		float bank = 0.0;
		// How far from the centerline anything is changed.
		float reach = 0.0;
	};
	// Where apply_to_landscape() writes: per terrain sample, how far it is from
	// the centerline and where along it.
	struct FootprintSample {
		float distance = Math::INF;
		float lateral = 0.0;
		float along = 0.0; // Fractional ring index.
		bool inside = false; // Fills only: within the shoreline.
	};
	struct FootprintBlock {
		Rect2i region;
		LocalVector<FootprintSample> samples;
	};
	// How far past the strip's edges the ground is kept level before the
	// carve falloff starts (see apply_to_landscape()).
	float _get_carve_shoulder(const Landscape3D *p_landscape) const;
	void _compute_footprint(Landscape3D *p_landscape, LocalVector<RingOnTerrain> &r_rings, LocalVector<FootprintBlock> &r_blocks) const;
	// The filled area's counterpart: every sample within the shoreline or
	// within reach of it, with its distance to the shoreline, and the base
	// level (without height_offset) in the landscape's local space.
	void _compute_fill_footprint(Landscape3D *p_landscape, float &r_level, LocalVector<FootprintBlock> &r_blocks) const;
	void _apply_fill_to_landscape(Landscape3D *p_landscape, bool p_carve, bool p_paint);

protected:
	static void _bind_methods();
	void _notification(int p_what);
	void _validate_property(PropertyInfo &p_property) const;

public:
	static void init_shaders();
	static void finish_shaders();

	// The built-in material a spline of this type renders with when it has
	// none of its own. Shared: edit a copy (see create_default_material()).
	static Ref<Material> get_default_material(SplineType p_type);
	// A new, independent copy of that material, with its own copy of the
	// shader, to assign to material and tweak.
	static Ref<Material> create_default_material(SplineType p_type);
	// Property name -> value for everything apply_preset() sets.
	static Dictionary get_preset(SplineType p_type);
	void apply_preset(SplineType p_type);

	void set_spline_type(SplineType p_type);
	SplineType get_spline_type() const;

	void set_landscape_path(const NodePath &p_path);
	NodePath get_landscape_path() const;

	void set_material(const Ref<Material> &p_material);
	Ref<Material> get_material() const;

	void set_width(float p_width);
	float get_width() const;

	void set_width_curve(const Ref<Curve> &p_curve);
	Ref<Curve> get_width_curve() const;

	void set_height_mode(HeightMode p_mode);
	HeightMode get_height_mode() const;

	void set_height_offset(float p_offset);
	float get_height_offset() const;

	void set_smooth(bool p_smooth);
	bool is_smooth() const;

	void set_fill(bool p_fill);
	bool is_fill() const;

	void set_segment_length(float p_length);
	float get_segment_length() const;

	void set_cross_segments(int p_segments);
	int get_cross_segments() const;

	void set_simplify_tolerance(float p_tolerance);
	float get_simplify_tolerance() const;

	void set_chunk_length(float p_length);
	float get_chunk_length() const;

	void set_uv_scale(const Vector2 &p_scale);
	Vector2 get_uv_scale() const;

	void set_edge_fade(float p_fade);
	float get_edge_fade() const;

	void set_end_fade_length(float p_length);
	float get_end_fade_length() const;

	void set_cast_shadow(GeometryInstance3D::ShadowCastingSetting p_setting);
	GeometryInstance3D::ShadowCastingSetting get_cast_shadow() const;

	void set_gi_mode(GeometryInstance3D::GIMode p_mode);
	GeometryInstance3D::GIMode get_gi_mode() const;

	void set_render_layers(uint32_t p_layers);
	uint32_t get_render_layers() const;

	void set_lod_bias(float p_bias);
	float get_lod_bias() const;

	void set_visibility_range_end(float p_distance);
	float get_visibility_range_end() const;

	void set_visibility_range_end_margin(float p_margin);
	float get_visibility_range_end_margin() const;

	void set_carve_enabled(bool p_enabled);
	bool is_carve_enabled() const;

	void set_carve_depth(float p_depth);
	float get_carve_depth() const;

	void set_carve_falloff(float p_falloff);
	float get_carve_falloff() const;

	void set_paint_layer(int p_layer);
	int get_paint_layer() const;

	void set_paint_strength(float p_strength);
	float get_paint_strength() const;

	void set_paint_falloff(float p_falloff);
	float get_paint_falloff() const;

	// The Landscape3D this spline sits on: landscape_path's target, or when
	// that is empty, the nearest Landscape3D that is an ancestor of this node
	// or a direct child of one of its ancestors.
	Landscape3D *get_landscape() const;

	// p_local_position (in this node's space) moved vertically onto the
	// landscape's surface; unchanged if there is no landscape under it.
	Vector3 project_to_landscape(const Vector3 &p_local_position) const;

	// Brings the mesh up to date now instead of at the end of the frame.
	void update_mesh();

	// The terrain regions (in heightmap samples, Landscape3D::CHUNK_QUADS on a
	// side) apply_to_landscape() changes, e.g. to snapshot them for undo.
	TypedArray<Rect2i> get_landscape_footprint() const;
	// Shapes the landscape to the spline: cuts or fills the ground under it to
	// the curve's own height (and carve_depth below that for a river bed),
	// blending back to the untouched terrain over carve_falloff, and paints
	// paint_layer under it. Uses the curve's heights, not the terrain's, so
	// place the points on the ground first (see project_to_landscape()).
	void apply_to_landscape();

	int get_chunk_count() const;
	// Plain C++ helpers for tests and tools; not bound.
	RID get_chunk_mesh(int p_index) const;
	RID get_chunk_instance(int p_index) const;
	// Changes exactly when the chunk's mesh is rebuilt.
	uint64_t get_chunk_hash(int p_index) const;
	float get_length() const { return total_length; }

	PackedStringArray get_configuration_warnings() const override;

	LandscapeSpline3D();
	~LandscapeSpline3D();
};

VARIANT_ENUM_CAST(LandscapeSpline3D::SplineType)
VARIANT_ENUM_CAST(LandscapeSpline3D::HeightMode)
