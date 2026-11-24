#pragma once

#include "procedural/geometry.hpp"

#include <chrono>
#include <span>

namespace atelier::procedural {

class Bvh;

// One primitive per triangle with vertex N and uv and primitive material (the mesh's
// slot index). Bitwise-equal positions weld into one point.
[[nodiscard]] Geometry import_mesh(const geometry::Mesh& mesh);

struct GridSettings {
    GridPrimitive primitive_type{GridPrimitive::quads};

    OrientationPlane orientation{OrientationPlane::zx};

    math::Vector2 size{10.f, 10.f};

    math::Vector3 center{};

    std::uint32_t rows{10};

    std::uint32_t columns{10};
};

// Surface or point grid on `orientation` around `center`. Surface modes require finite
// positive `size` and rows/columns >= 2, emitting vertex N and [0,1] vertex uv.
Result<Geometry> grid(const GridSettings& settings);

// rows x columns points with N = +Y, spanning `size` on X and Z around the origin;
// one point on an axis sits at 0. Row r varies Z, column c varies X; point r*columns + c.
Result<Geometry> grid(math::Vector2 size, std::uint32_t rows, std::uint32_t columns);

// `count` points from `start` to `end` inclusive.
Result<Geometry> line(math::Vector3 start, math::Vector3 end, std::uint32_t count);

struct BoxSettings {
    math::Vector3 size{1.f, 1.f, 1.f};

    math::Vector3 center{};

    bool base_on_ground{false};

    std::uint32_t divisions_x{1};

    std::uint32_t divisions_y{1};

    std::uint32_t divisions_z{1};
};

// Closed subdivided box with shared seam/corner points, hard-edge vertex N and per-face [0,1] vertex uv.
// When divisions are 1x1x1 and centered at origin, preserves the exact 8-point bit order and 6-face order.
Result<Geometry> box_primitive(const BoxSettings& settings);

// Origin-centred box with finite positive dimensions, eight shared points in XYZ
// bit order, and six quads (-X,+X,-Y,+Y,-Z,+Z). Vertex N preserves hard edges.
Result<Geometry> box(math::Vector3 size);

// Origin-centred, Y-polar sphere: segments around Y, latitudes intervals pole to
// pole. Shared poles/seam, polar triangles then ring quads then south triangles;
// radial vertex N. Radius is finite and positive, segments >= 3, latitudes >= 2.
Result<Geometry> sphere(float radius, std::uint32_t segments, std::uint32_t latitudes);

struct TubeSettings {
    float radius_top{1.f};

    float radius_bottom{1.f};

    float inner_radius{0.f};

    float height{2.f};

    math::Vector3 center{};

    bool base_on_ground{false};

    std::uint32_t segments{32};

    std::uint32_t rows{1};

    float arc_degrees{360.f};

    bool end_caps{true};
};

// Generalized cylinder, cone, frustum, prism, hollow pipe or sector on Y.
// Preserves the exact 2M-point, (M+2)-face topology on default solid 360-degree 1-row cylinders.
Result<Geometry> tube(const TubeSettings& settings);

// Origin-centred capped solid cylinder on Y, segments >= 3, finite positive radius
// and height. Bottom then top rings, side quads then polygon caps; vertex N.
Result<Geometry> tube(float radius, float height, std::uint32_t segments);

struct WedgeSettings {
    math::Vector3 size{2.f, 1.f, 2.f};

    float peak_ratio{0.5f}; // 0 = right-angle ramp (-X vertical wall), 0.5 = symmetric gabled roof, 1 = +X wall.

    math::Vector3 center{};

    bool base_on_ground{false};

    std::uint32_t divisions_z{1};
};

// Closed watertight triangular prism / ramp / gabled roof with hard-edge vertex N and vertex uv.
Result<Geometry> wedge(const WedgeSettings& settings = {});

struct CircleSettings {
    CirclePrimitive primitive_type{CirclePrimitive::polygon};

    OrientationPlane orientation{OrientationPlane::zx};

    math::Vector2 radius{1.f, 1.f};

    math::Vector3 center{};

    std::uint32_t segments{32};

    float start_degrees{0.f};

    float arc_degrees{360.f};
};

// Planar polygon, closed polyline loop, or open arc polyline on `orientation`.
Result<Geometry> circle(const CircleSettings& settings = {});

struct TorusSettings {
    OrientationPlane orientation{OrientationPlane::zx};

    float major_radius{1.f};

    float minor_radius{0.25f};

    math::Vector3 center{};

    bool base_on_ground{false};

    std::uint32_t major_segments{32};

    std::uint32_t minor_segments{16};

    float arc_degrees{360.f};

    bool end_caps{true};
};

// Closed manifold torus or capped partial-arc pipe elbow (requires minor_radius < major_radius).
Result<Geometry> torus(const TorusSettings& settings = {});

struct GroupSettings {
    StringView group{"selected"};
    GroupCriterion criterion{GroupCriterion::normal};
    math::Vector3 direction{0.f, 1.f, 0.f};
    float max_angle{45.f};
    StringView source_group{};
    StringView source_attribute{};
    float minimum{}, maximum{1.f};
    GroupOperation operation{GroupOperation::replace};
    StringView base_group{};
};

Result<Geometry> build_group(const Geometry& input, const GroupSettings& settings = {});

// Writes/replaces a point bool column using a closed finite AABB. Incompatible
// existing layer/type or an invalid name fails; P and other columns are unchanged.
Result<void> group_box(Geometry& geometry, StringView group, math::Vector3 minimum, math::Vector3 maximum);

// Dense point piece IDs, ordered by each connected component's lowest point index.
// Polygons and open polylines connect at shared point indices. Each unused point forms its own piece.
Result<void> connectivity(Geometry& geometry, StringView attribute = "piece");

// One actual cook, including completed work on failure. Elapsed phases are
// exclusive. An absent receiver avoids clock reads and capacity accounting.
struct BooleanStats {
    std::uint64_t input_faces{};

    std::uint64_t candidate_pairs{}; // Directed 3D AABB overlaps in A->B and B->A.

    std::uint64_t ray_candidates{}; // Non-parallel target faces visited by cell winding rays after BVH pruning.

    std::uint64_t splits{}; // New cells created, including cells later discarded.

    std::uint64_t peak_face_scratch_bytes{}; // Dynamic cell, corner and segment buffers of one source face.

    std::chrono::nanoseconds prepare{};

    std::chrono::nanoseconds arrange{};

    std::chrono::nanoseconds sew{};

    std::chrono::nanoseconds publish{};

    std::uint32_t prepared_input_hits{}; // Bit 0 for A, bit 1 for B, when a prepared surface was reused.
};

// Regularized solid Boolean on outward-oriented closed polygon shells.
Result<Geometry> solid_boolean(const Geometry& a, const Geometry& b, BooleanOperation operation,
                               BooleanStats* stats = nullptr);

struct ResampleSettings {
    ResampleMode mode{ResampleMode::spacing};
    float spacing{1.f};
    std::uint32_t segments{8};
    bool preserve_corners{};
};

// Arc-length lattice per polyline. Preserved turns are additional samples and never restart the lattice.
[[nodiscard]] Result<Geometry> resample(const Geometry& curves, const ResampleSettings& settings = {});

// Coordinates are metres on each source polyline. Distances outside [0, length] are errors.
[[nodiscard]] Result<Geometry> sample_curves(const Geometry& curves, float distance, bool from_end = false);
[[nodiscard]] Result<Geometry> slice_curves(const Geometry& curves, float start, float end, bool end_from_tip = false);
[[nodiscard]] Result<void> read_curve_slices(const Geometry& curves, std::uint32_t primitive,
                                             const Array<math::Vector2>& intervals, Array<math::Vector3>& positions,
                                             Array<std::uint32_t>& offsets);

struct ScatterSettings {
    float density{1}; // Points per unit area before elimination.

    std::uint32_t seed{};

    float min_distance{}; // 0 keeps every point.

    StringView group{}; // Empty selects the entire surface; otherwise an existing primitive bool column.
};

// Points on first-corner fans of convex planar polygons; polylines fail. Primitive p,
// fan triangle j uses s = hash(seed, p) for j=0, hash(hash(seed, p), j) otherwise.
// Count is area*density, fraction rounded up probabilistically using hash(s, 0).
// Positions draw hash(s, 2*k+1/2*k+2); output order is p/j/local ordinal k. Work order
// and threads cannot change these values. With min_distance, stable sequential
// elimination follows generation. Outputs P, N, id = hash(p, k) for j=0 or
// hash(hash(p, j), k) otherwise (a seed, not identity), sourceprim, sourceordinal,
// sourceprimuv (b1,b2 within j). Only inputs with >3-corner polygons add sourcetri;
// pure triangles retain their six-column schema and values.
Result<Geometry> scatter(const Geometry& surface, const ScatterSettings& settings);

struct VolumeScatterSettings {
    float density{1}; // Candidates per cubic metre before signed coverage filtering.

    std::uint32_t seed{};

    math::Vector3 origin{};

    float cell_size{1};

    bool cache_bvh{}; // Reuse `Bvh::acquire(surface)` across evaluations of an unchanged shell.
};

// Closed oriented manifold shells, with positive aggregate signed volume. Fixed grid
// candidates in cell x/y/z/ordinal order; retains winding >= 0.5, excluding the surface
// uncertainty band. Serial BVH signed crossings, with compensated solid angles only
// after all eight fixed directions are uncertain; no parity, approximation or mesh repair.
// Both AABB-intersecting cells and pre-filter candidates are limited to 1,048,576.
// Outputs P, id (nonunique), sourcecellx/y/z (int32), sourceordinal (uint32).
Result<Geometry> volume_scatter(const Geometry& surface, const VolumeScatterSettings& settings);

// XYZ Euler angles in degrees, applied X then Y then Z.
[[nodiscard]] math::Quaternion euler_to_quaternion(math::Vector3 euler_degrees) noexcept;

struct TransformSettings {
    math::Vector3 translate{};

    math::Vector3 rotate_degrees{};

    math::Vector3 scale{1.f, 1.f, 1.f};

    math::Vector3 pivot{};
};

Result<void> transform(Geometry& geometry, const TransformSettings& settings);

// Applies v * S * R * T. By usage: point columns take the affine transform, vector
// columns its linear part L, normal columns sign(det L) * cofactor(L) normalized (zero
// stays zero, so a singular L still yields plane normals); quaternion orient columns
// are composed with `rotation`; instance transforms are right-multiplied; color and none
// columns stay. A reflection reverses only polygon vertex order. Fails when a
// result is not finite; the geometry is then left partly transformed.
Result<void> transform(Geometry& geometry, math::Vector3 translate, math::Quaternion rotation, math::Vector3 scale);

struct MatchSizeSettings {
    AxisJustify justify_x{AxisJustify::center};

    AxisJustify justify_y{AxisJustify::min};

    AxisJustify justify_z{AxisJustify::center};

    math::Vector3 target_position{};

    bool scale_to_fit{false};

    bool uniform_scale{true};

    math::Vector3 target_size{1.f, 1.f, 1.f};
};

// Aligns and optionally scales `geometry` to `reference` bounds (when non-null and non-empty)
// or to `target_position` and `target_size`. Empty point geometry passes through unchanged.
Result<Geometry> match_size(const Geometry& geometry, const MatchSizeSettings& settings = {},
                            const Geometry* reference = nullptr);

// Moves an attribute to another layer. Each target element combines the source
// elements it touches (a point the vertices referring to it, a primitive its vertices or
// their points, detail everything); an element touching none takes the default. Sum and
// average need float or float vector columns. Instances only exchange with detail.
Result<void> promote(Geometry& geometry, StringView name, Layer to, Reduce reduce);

struct RaySettings {
    RayMode mode{RayMode::project};

    math::Vector3 direction{0.f, -1.f, 0.f};

    float max_distance{1000.f};

    bool normal{};

    bool remove_misses{};
};

// Projects points onto convex polygon surfaces along `direction` or to the closest point,
// writing hitprim, hittri, hitprimuv and hitdist (-1 on miss). Reuses `Bvh::acquire(surface)`.
Result<Geometry> ray(const Geometry& points, const Geometry& surface, const RaySettings& settings = {});

// Instances at points: transform S * O * T with S = scale * pscale, T from P, O the
// unit orient when it exists, else the alignment taking +Z to N and +Y toward up (the
// shortest rotation from +Z to N without up, identity without N). `variant` (uint,
// default 0 when absent) picks the reference. Detail attributes carry over; the output
// has no points. Non-finite inputs and out-of-range variants fail.
Result<Geometry> copy_to_points(const Geometry& points, std::span<const Reference> references,
                                StringView variant = "variant");

// Real geometry copies in selected target-point order, using the same placement as
// copy_to_points. group is a point bool column (empty selects all); variant picks
// an ordered source. Sources cannot contain instances. Only selected sources join
// their schemas; missing columns default, incompatible schemas fail. Explicit target
// point columns broadcast unchanged, overriding compatible columns (P is forbidden).
// Detail takes the first selected source in source-port order, then target fallback.
// Preflights layer counts, fills each output buffer once, and never mutates inputs.
Result<Geometry> realize(std::span<const Geometry> sources, const Geometry& points, StringView group = {},
                         StringView variant = "variant", std::span<const String> target_attributes = {});

struct ExtrudeSettings {
    StringView group{};

    float distance{0.5f};

    float inset{0.f};

    StringView distance_attribute{};

    StringView inset_attribute{};

    bool individual{true};

    bool output_back{true};

    StringView front_group{};

    StringView side_group{};
};

// Extrudes selected convex planar polygons along their face normals (or shared region vertex normals
// when individual is false) with optional planar bisector inset, hard-edge vertex N, metric side uv,
// primitive/detail attribute inheritance, and optional back caps for watertight solid shells.
Result<Geometry> extrude(const Geometry& surface, const ExtrudeSettings& settings = {});

struct BevelSettings {
    float radius{0.1f};
    float angle_degrees{30.f};
    std::uint32_t segments{1};
};

// Rounds convex ridges on closed, outward-facing convex components. Radius is measured
// in world units; at a right angle it equals the setback on each incident face.
Result<Geometry> bevel(const Geometry& surface, const BevelSettings& settings = {});

struct SweepSettings {
    SweepProfile profile{SweepProfile::rectangle};

    float width{1.f};

    float height{1.f};

    float radius{0.5f};

    std::uint32_t segments{8};

    math::Vector3 up{0.f, 1.f, 0.f};

    bool end_caps{true};

    StringView scale_attribute = StringView{"pscale"};
};

// Sweeps a rectangular, tubular, or custom XY cross-section along every open polyline in `backbone`
// using parallel-transport + `up`-stabilized mitered frames, optional point `scale_attribute` modulation,
// end caps, vertex N, and arc-length/ring vertex uv. Warped 3D quads split cleanly along their diagonal
// so every output face satisfies the shared planar convex polygon tolerance.
Result<Geometry> sweep(const Geometry& backbone, const SweepSettings& settings = {},
                       const Geometry* cross_section = nullptr);

// Of the points; empty geometry has none.
[[nodiscard]] std::optional<math::Bounds> bounds(const Geometry& geometry) noexcept;

}
