#pragma once

#include "procedural/geometry.hpp"

#include <array>

namespace atelier::procedural::detail {

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
    static Result<TriangleView> create(const Geometry& surface);

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
