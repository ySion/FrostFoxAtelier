#pragma once

#include "procedural/geometry.hpp"

#include <optional>
#include <vector>

namespace atelier::procedural {

struct Hit {
    std::uint32_t primitive;
    float distance;
    math::Vector2 uv;        // Barycentric (b1, b2): position = (1 - b1 - b2) a + b1 b + b2 c, as scatter's sourceprimuv.
    math::Vector3 position;
};

// Bounding volume hierarchy over the triangle primitives of a geometry, snapshotted at build;
// zero-area triangles are left out. Triangles count from both sides, and a ray through a
// shared edge may miss both neighbours.
class Bvh {
public:
    // Fails for primitives other than triangles.
    static Result<Bvh> build(const Geometry& surface);
    // The nearest triangle along a unit direction within max_distance.
    [[nodiscard]] std::optional<Hit> ray(math::Vector3 origin, math::Vector3 direction, float max_distance) const noexcept;
    // The nearest surface point within max_distance.
    [[nodiscard]] std::optional<Hit> closest(math::Vector3 point, float max_distance) const noexcept;
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
        math::Vector3 a, b, c;
        std::uint32_t primitive;
    };
    std::vector<Node> nodes_;
    std::vector<Triangle> triangles_;
};

}
