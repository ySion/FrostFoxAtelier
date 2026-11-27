#pragma once

#include "foundation/array.hpp"
#include "foundation/optional.hpp"
#include "foundation/shared_array.hpp"
#include "procedural/geometry.hpp"
#include "runtime/reflection.hpp"

#include <memory>

#include "procedural/bvh.gen.hpp"

namespace atelier::procedural {

// 32-byte word of a read-only Spatial acceleration buffer. Word 0 stores `{tri_root, seg_root}` in
// `{u0, u1}`; interior/leaf BVH nodes store `{minimum, first, maximum, count}`; leaf items occupy
// two consecutive words (64 bytes).
ASTRUCT(Name = "procedural.SpatialNode", DESC = "只读空间加速缓冲的 32 字节单元。")
struct SpatialNode {
    AGENERATED_BODY(SpatialNode)

    AFIELD(DESC = "下界或首向量。")
    math::Vector3 a{};

    AFIELD(DESC = "首下标或图元号。")
    std::uint32_t u0{};

    AFIELD(DESC = "上界或次向量。")
    math::Vector3 b{};

    AFIELD(DESC = "叶元素数或辅助编码。")
    std::uint32_t u1{};
};

// Immutable surface-triangle and polyline-segment BVH buffer projected as a read-only sequence to
// Source kernels (`[Port] const procedural.Spatial&`).
struct Spatial {
    SharedArray<SpatialNode> words;

    static Result<Spatial> build(const Geometry& geometry);

    static Result<std::shared_ptr<const Spatial>> acquire(const Geometry& geometry);
};

struct Hit {
    std::uint32_t primitive;

    std::uint32_t triangle; // First-corner fan index within primitive.

    float distance;

    math::Vector2 uv; // Barycentric (b1, b2) in triangle, as scatter's sourceprimuv (not polygon coordinates).

    math::Vector3 position;
};

// Bounding volume hierarchy over first-corner fans of convex planar polygons, snapshotted at build;
// zero-area triangles are left out. The nearest-hit ray counts both sides but may miss
// both neighbours of a shared edge; signed_crossings instead reports uncertain intersections.
class Bvh {
public:
    // Fails for polylines or polygons without a simple, convex, resolvable plane.
    static Result<Bvh> build(const Geometry& surface);

    // Reuses a bounded cached hierarchy while `surface` retains the same immutable point and topology columns.
    static Result<std::shared_ptr<const Bvh>> acquire(const Geometry& surface);

    // The nearest triangle along a unit direction within max_distance.
    [[nodiscard]] Optional<Hit> ray(math::Vector3 origin, math::Vector3 direction, float max_distance) const noexcept;

    // The nearest surface point within max_distance.
    [[nodiscard]] Optional<Hit> closest(math::Vector3 point, float max_distance) const noexcept;

    // Whether any polygon's bounded Euclidean surface band contains point. Each polygon
    // uses its own scale-dependent tolerance, not the nearest polygon's tolerance.
    [[nodiscard]] bool on_surface_band(math::Vector3 point) const noexcept;

    // Sum sign(normal dot direction) over all strictly positive ray intersections.
    // Finite point and finite, nonzero direction are required. Nullopt means a relevant
    // intersection's sign could not be certified with outward-rounded double arithmetic.
    // For a closed oriented surface outside its band, this sum is its integer winding.
    [[nodiscard]] Optional<std::int64_t> signed_crossings(math::Vector3 point,
                                                          math::Vector3 direction) const noexcept;

private:
    // A leaf holds triangles [first, first + count); an interior node has count 0 and its
    // children at first and first + 1.
    struct Node {
        math::Vector3 minimum;

        std::uint32_t first;

        math::Vector3 maximum;

        std::uint32_t count;
    };

    struct Triangle {
        math::Vector3 a;

        math::Vector3 b;

        math::Vector3 c;

        std::uint32_t primitive;

        std::uint32_t triangle;
    };

    Array<Node> nodes_;

    Array<Triangle> triangles_;

    // Parallel metadata keeps the existing nearest-hit traversal's node/triangle layout.
    Array<double> node_tolerances_;

    Array<double> polygon_tolerances_;
};

}
