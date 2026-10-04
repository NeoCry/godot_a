/**************************************************************************/
/*  octahedral_impostor_baker.h                                           */
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

#include "../octahedral_impostor_material_3d.h"

#include "core/io/image.h"
#include "core/object/ref_counted.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"

class Mesh;
class Node;
class Node3D;
class Shader;
class ShaderMaterial;

// Bakes the views of a node and its visible descendants with the rendering server of the editor
// (one draw per region of the atlas, all the views of a region rendered at once as rotated copies
// of the geometry), into either:
//
// - The atlases of an octahedral impostor (frames x frames views, view (x, y) at the tile of
//   column x and row y):
//   - Albedo: sRGB color and coverage (alpha).
//   - Normal and depth: object space normal in RGB, depth in A from the front (0) to the back (1)
//     of the bounding sphere along the view.
//   - ORM (optional): ambient occlusion, roughness and metallic.
// - The textures of a billboard: a quad that turns toward the camera with a single view of the
//   object, or crossed planes with a view each (side by side in the textures), drawn with a
//   StandardMaterial3D:
//   - Albedo: sRGB color and coverage (alpha).
//   - Normal: tangent space normal map of the quad (RGB).
//   - ORM (optional): ambient occlusion, roughness and metallic.
class OctahedralImpostorBaker : public RefCounted {
	GDCLASS(OctahedralImpostorBaker, RefCounted);

public:
	enum Type {
		TYPE_OCTAHEDRAL_IMPOSTOR,
		TYPE_BILLBOARD,
		TYPE_MAX
	};

	enum BillboardMode {
		BILLBOARD_FIXED_Y, // Turns around the vertical axis toward the camera.
		BILLBOARD_ENABLED, // Faces the camera.
		BILLBOARD_CROSS, // Static vertical planes that cross on the vertical axis of the object.
		BILLBOARD_MAX
	};

	static constexpr int MIN_ATLAS_SIZE = 64;
	static constexpr int MAX_ATLAS_SIZE = 8192;
	static constexpr int MAX_SUPERSAMPLING = 4;
	static constexpr int MIN_CROSS_PLANES = 2;
	static constexpr int MAX_CROSS_PLANES = 4;
	// Empty border of the frames, in pixels of the atlas.
	static constexpr int FRAME_PADDING = 2;
	// Largest viewport rendered at once, the atlas is rendered in several regions above it.
	static constexpr int MAX_REGION_SIZE = 2048;

	struct Geometry {
		ObjectID node;
		RID base;
		Ref<Resource> base_resource; // Keeps the mesh or multimesh alive during the bake.
		Transform3D transform; // Relative to the baked node.
		bool multimesh = false;
		Vector<Ref<Material>> surface_override_materials; // May contain null materials.
		Vector<Ref<Material>> surface_materials; // Materials of the surfaces without the material override.
		Ref<Material> material_override;
		Ref<Material> material_overlay;
		Vector<float> blend_shape_weights;
		Vector<Pair<StringName, Variant>> instance_parameters;
	};

	// A view of the object in the atlas, seen by an orthographic camera.
	struct View {
		Basis basis; // Columns: right and up in the image, toward the viewer.
		Vector3 center; // Point of the space of the node at the center of the view.
		Rect2i rect; // Pixels of the view in the atlas.
	};

private:
	enum Pass {
		PASS_ALBEDO,
		PASS_GEOMETRY,
		PASS_ORM,
		PASS_MAX
	};

	// Settings.
	Type type = TYPE_OCTAHEDRAL_IMPOSTOR;
	OctahedralImpostorMaterial3D::Layout layout = OctahedralImpostorMaterial3D::LAYOUT_HEMISPHERE;
	int frames = 12;
	BillboardMode billboard_mode = BILLBOARD_FIXED_Y;
	int cross_planes = 2;
	int atlas_size = 2048;
	int supersampling = 2;
	bool bake_orm = false;

	// Results.
	Ref<Image> albedo_image;
	Ref<Image> normal_depth_image;
	Ref<Image> orm_image;
	Vector3 sphere_center;
	float sphere_radius = 0.0;
	float source_roughness = 1.0;
	float source_metallic = 0.0;
	int baked_frames = 0;
	OctahedralImpostorMaterial3D::Layout baked_layout = OctahedralImpostorMaterial3D::LAYOUT_HEMISPHERE;
	Type baked_type = TYPE_OCTAHEDRAL_IMPOSTOR;
	BillboardMode baked_billboard_mode = BILLBOARD_FIXED_Y;
	LocalVector<View> baked_views;
	real_t baked_texel_size = 0.0;

	// Views rendered by a draw.
	struct Region {
		Rect2i rect; // Pixels of the atlas.
		LocalVector<uint32_t> views;
	};

	// State of a bake.
	struct BakeState {
		bool billboard = false; // Normals of the views (normal map) instead of the object, no depth.
		Size2i atlas_size;
		real_t texel_world = 0.0; // Size of a pixel of the atlas in the space of the node.
		int supersampling = 1;
		real_t margin_world = 0.0; // Distance from the camera to the front of the geometry.
		real_t depth_extent = 0.0; // Distance from the centers of the views to the front of the geometry.
		real_t depth_near = 0.0; // Depth of the front of the geometry, seen from the camera.
		real_t depth_range = 1.0;
		bool use_normal_buffer = false;
		bool srgb_output[2] = {}; // Albedo pass, other passes.
		LocalVector<View> views;
		LocalVector<Region> regions;
		LocalVector<Geometry> geometry;
		RID scenario;
		RID viewport;
		RID camera;
		RID capture_instance;
		Ref<Mesh> capture_mesh;
		Ref<ShaderMaterial> capture_material;
		Ref<Shader> override_shaders[3]; // By cull mode.
		HashMap<ObjectID, Ref<ShaderMaterial>> override_materials;
		Ref<ShaderMaterial> default_override_material;
		LocalVector<Pair<RID, int>> instances; // Instance and index of its geometry.
		// Rendered albedo of the current region, weights the other maps.
		Ref<Image> region_albedo;
		// Pixels of the atlases with coverage, geometry data and ORM data, for the dilation.
		Vector<uint8_t> coverage_mask;
		Vector<uint8_t> geometry_mask;
		Vector<uint8_t> orm_mask;
	};

	static bool _is_baked_geometry(const Geometry &p_geometry);
	static void _collect_geometry(Node *p_node, const Transform3D &p_transform, bool p_is_root, bool p_first_only, LocalVector<Geometry> &r_geometry);
	static void _collect_points(const LocalVector<Geometry> &p_geometry, LocalVector<Vector3> &r_points);
	void _estimate_surface_parameters(const LocalVector<Geometry> &p_geometry);

	Error _setup_impostor_views(BakeState &p_state, const Vector3 &p_center, float p_radius);
	Error _setup_billboard_views(BakeState &p_state, const Vector3 &p_center, float p_radius);
	static void _setup_regions(BakeState &p_state, int p_region_size);

	Ref<ShaderMaterial> _get_override_material(BakeState &p_state, const Ref<Material> &p_material);
	void _setup_instances(BakeState &p_state, const Region &p_region);
	void _setup_camera(BakeState &p_state, const Region &p_region);
	void _set_pass(BakeState &p_state, Pass p_pass);
	void _free_instances(BakeState &p_state);
	void _draw(BakeState &p_state);
	bool _is_output_srgb(BakeState &p_state, Pass p_pass);
	void _read_region(BakeState &p_state, Pass p_pass, const Region &p_region);
	void _cleanup(BakeState &p_state);
	static void _dilate(Ref<Image> &p_image, const Vector<uint8_t> &p_mask, const LocalVector<View> &p_views, bool p_keep_alpha);

	void _apply_to_billboard_material(const Ref<BaseMaterial3D> &p_material, const Ref<Texture2D> p_textures[OctahedralImpostorMaterial3D::TEXTURE_MAX]) const;
	// Mesh of the result with these textures. Updates the mesh in place if it has the right type.
	Ref<Mesh> _make_mesh(const Ref<Resource> &p_mesh, const Ref<Texture2D> p_textures[OctahedralImpostorMaterial3D::TEXTURE_MAX]) const;

	Error _bake_bind(Node *p_node, bool p_show_progress);

protected:
	static void _bind_methods();

public:
	void set_type(Type p_type);
	Type get_type() const;

	void set_layout(OctahedralImpostorMaterial3D::Layout p_layout);
	OctahedralImpostorMaterial3D::Layout get_layout() const;

	void set_frames(int p_frames);
	int get_frames() const;

	void set_billboard_mode(BillboardMode p_mode);
	BillboardMode get_billboard_mode() const;

	void set_cross_planes(int p_planes);
	int get_cross_planes() const;

	void set_atlas_size(int p_size);
	int get_atlas_size() const;

	void set_supersampling(int p_factor);
	int get_supersampling() const;

	void set_bake_orm(bool p_enable);
	bool is_baking_orm() const;

	// Size of a frame and of the atlas of an octahedral impostor that is baked with the current
	// settings (the atlas is rounded down to a multiple of the frames).
	int get_frame_size() const;
	int get_baked_atlas_size() const;

	// Geometry that is baked for the node: its visible meshes, multimeshes and CSG shapes, and
	// those of its descendants. Hidden nodes, shadow-only geometry, far levels of detail
	// (visibility range begin > 0), impostors and baked billboards are skipped.
	static LocalVector<Geometry> collect_geometry(Node3D *p_node);
	static bool has_geometry(Node3D *p_node);
	// Sphere centered on the bounds of the geometry, through its farthest vertex.
	static bool compute_bounding_sphere(const LocalVector<Geometry> &p_geometry, Vector3 &r_center, float &r_radius);
	// Views of a billboard of the geometry, fitted to its vertices, and the size of its textures
	// (the longest side is at most p_size) and of their pixels in the space of the node.
	static bool compute_billboard_views(const LocalVector<Geometry> &p_geometry, BillboardMode p_mode, int p_cross_planes, int p_size, LocalVector<View> &r_views, Size2i &r_atlas_size, real_t &r_texel_size);

	Error bake(Node3D *p_node, bool p_show_progress = false);

	Ref<Image> get_albedo_image() const { return albedo_image; }
	Ref<Image> get_normal_depth_image() const { return normal_depth_image; }
	Ref<Image> get_orm_image() const { return orm_image; }
	Vector3 get_sphere_center() const { return sphere_center; }
	float get_sphere_radius() const { return sphere_radius; }
	Type get_baked_type() const { return baked_type; }
	// Size of a pixel of the textures of the last bake, in the space of the node.
	real_t get_texel_size() const { return baked_texel_size; }

	// Sets the baked parameters of a material (without its textures).
	void apply_to_material(const Ref<OctahedralImpostorMaterial3D> &p_material) const;
	// Material of an octahedral impostor with the baked images as textures, e.g. for previews.
	Ref<OctahedralImpostorMaterial3D> create_material() const;
	// Mesh of the result (impostor or billboard) with the baked images as textures, e.g. for previews.
	Ref<Mesh> create_mesh() const;
	// Saves the textures next to the mesh file ("<name>_albedo.png", ...), imports them, and saves
	// the mesh of the impostor or of the billboard with its material. A mesh already loaded from
	// this path is updated in place.
	Ref<Mesh> save(const String &p_path);

	OctahedralImpostorBaker();
};

VARIANT_ENUM_CAST(OctahedralImpostorBaker::Type)
VARIANT_ENUM_CAST(OctahedralImpostorBaker::BillboardMode)
