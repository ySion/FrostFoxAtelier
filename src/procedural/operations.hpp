#pragma once

#include "procedural/geometry.hpp"

#include <span>

namespace atelier::procedural {

// One primitive per triangle with vertex N and uv and primitive material (the mesh's
// slot index). Bitwise-equal positions weld into one point.
[[nodiscard]] Geometry import_mesh(const geometry::Mesh& mesh);

// rows x columns points with N = +Y, spanning `size` on X and Z around the origin;
// one point on an axis sits at 0. Row r varies Z, column c varies X; point r*columns + c.
Result<Geometry> grid(math::Vector2 size, std::uint32_t rows, std::uint32_t columns);

// `count` points from `start` to `end` inclusive.
Result<Geometry> line(math::Vector3 start, math::Vector3 end, std::uint32_t count);

struct ScatterSettings {
    float density{1}; // Points per unit area before elimination.

    std::uint32_t seed{};

    float min_distance{}; // 0 keeps every point.
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

// Applies v * S * R * T. By usage: point columns take the affine transform, vector
// columns its linear part L, normal columns sign(det L) * cofactor(L) normalized (zero
// stays zero, so a singular L still yields plane normals); quaternion orient columns
// are composed with `rotation`; instance transforms are right-multiplied; color and none
// columns stay. A reflection reverses only polygon vertex order. Fails when a
// result is not finite; the geometry is then left partly transformed.
Result<void> transform(Geometry& geometry, math::Vector3 translate, math::Quaternion rotation, math::Vector3 scale);

// Moves an attribute to another layer. Each target element combines the source
// elements it touches (a point the vertices referring to it, a primitive its vertices or
// their points, detail everything); an element touching none takes the default. Sum and
// average need float or float vector columns. Instances only exchange with detail.
Result<void> promote(Geometry& geometry, StringView name, Layer to, Reduce reduce);

// Instances at points: transform S * O * T with S = scale * pscale, T from P, O the
// unit orient when it exists, else the alignment taking +Z to N and +Y toward up (the
// shortest rotation from +Z to N without up, identity without N). `variant` (uint,
// default 0 when absent) picks the reference. Detail attributes carry over; the output
// has no points. Non-finite inputs and out-of-range variants fail.
Result<Geometry> copy_to_points(const Geometry& points, std::span<const Reference> references,
                                StringView variant = "variant");

struct Bounds {
    math::Vector3 minimum;

    math::Vector3 maximum;
};

// Of the points; empty geometry has none.
[[nodiscard]] std::optional<Bounds> bounds(const Geometry& geometry) noexcept;

}
