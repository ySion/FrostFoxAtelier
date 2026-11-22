#pragma once

#include "procedural/geometry.hpp"

#include <array>
#include <span>

namespace atelier::procedural::detail {

// Shared float-input distance uncertainty; no absolute world-unit floor.
double surface_tolerance(double coordinate_scale, double extent) noexcept;

struct PolygonScale {
    double extent; // AABB diagonal; independent of the first corner.

    double tolerance;
};

// Uses all original corners, not a fan piece; the polygon owns one distance band.
PolygonScale polygon_scale(std::span<const math::Vector3> corners) noexcept;

PolygonScale polygon_scale(const Geometry& surface, PrimitiveRange range) noexcept;

// Conservative shared planar/convex/simple-boundary admission for a single polygon primitive.
Result<void> validate_polygon(std::span<const math::Vector3> corners);

Result<void> validate_polygon(const Geometry& surface, PrimitiveRange range);

// Barycentric (v,w) of the closest point on a nondegenerate bounded triangle.
math::Vector2d closest_on_triangle(math::Vector3d p, math::Vector3d a, math::Vector3d b, math::Vector3d c);

// Original vertex (corner) indices, not point indices or a flattened triangle number.
inline std::array<std::uint32_t, 3> fan_corners(PrimitiveRange range, std::uint32_t triangle) noexcept {
    return {range.first, range.first + triangle + 1, range.first + triangle + 2};
}

struct SurfaceTriangle {
    std::uint32_t primitive;

    std::uint32_t triangle;

    std::array<std::uint32_t, 3> corners;
};

// Validated first-corner fans without a second topology buffer. Borrowed geometry must
// remain unchanged and alive throughout iteration. This is a module-internal view.
class TriangleView {
public:
    // Volume requires every face (including triangles) to have resolvable area.
    // Zero-area fan pieces of an otherwise valid polygon remain allowed.
    static Result<TriangleView> create(const Geometry& surface, bool require_nondegenerate = false);

    static Result<TriangleView> create(const Geometry&& surface, bool require_nondegenerate = false) = delete;

    struct Iterator {
        const Geometry* surface;

        std::uint32_t primitive;

        std::uint32_t triangle{};

        SurfaceTriangle operator*() const noexcept {
            return {primitive, triangle, fan_corners(surface->primitive(primitive), triangle)};
        }

        Iterator& operator++() noexcept {
            if (++triangle == surface->primitive(primitive).count - 2) {
                ++primitive;
                triangle = 0;
            }
            return *this;
        }

        bool operator==(const Iterator&) const = default;
    };

    Iterator begin() const noexcept { return {surface_, 0}; }

    Iterator end() const noexcept { return {surface_, surface_->count(Layer::primitive)}; }

    std::uint32_t size() const noexcept { return size_; }

    bool has_subtriangles() const noexcept { return has_subtriangles_; }

private:
    TriangleView(const Geometry& surface, std::uint32_t size, bool has_subtriangles)
        : surface_(&surface), size_(size), has_subtriangles_(has_subtriangles) {}

    const Geometry* surface_;

    std::uint32_t size_;

    bool has_subtriangles_;
};

}
