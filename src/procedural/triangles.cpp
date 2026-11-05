#include "procedural/triangles.hpp"
#include "foundation/result.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace atelier::procedural::detail {
namespace {

// The shared numerical policy for every polygon consumer. Distance uncertainty is
// eight float epsilons at the input coordinate/extent scale, not a world-unit floor.
// Projected turns must be nonnegative: together with exactly one full turn this
// gives a simple convex boundary. Allowing even small negative turns would let
// opposite loops cancel. This conservatively rejects rounding-induced concavity;
// forward collinear corners remain valid. Triangles keep their degeneracy policy.
Result<void> validate_polygon(const Geometry& surface, PrimitiveRange range) {
    const auto corners = surface.vertex_points();
    const auto point = [&](std::uint32_t i) {
        const auto p = surface.positions()[corners[range.first + i]];
        return math::Vector3d{p.x, p.y, p.z};
    };
    const auto origin = point(0);
    double coordinate_scale = 0;
    double extent = 0;
    math::Vector3d area;
    for (std::uint32_t i = 0; i < range.count; ++i) {
        const auto p = point(i);
        if (!math::is_finite(p))
            return failure("Polygon positions must be finite.");
        coordinate_scale = std::max(coordinate_scale, std::max(std::abs(p.x), std::max(std::abs(p.y), std::abs(p.z))));
        extent = std::max(extent, math::length(p - origin));
        if (i > 0 && i + 1 < range.count)
            area += math::cross(p - origin, point(i + 1) - origin);
    }
    const double tolerance = 8 * std::numeric_limits<float>::epsilon() * std::max(coordinate_scale, extent);
    const double area_length = math::length(area);
    if (!(area_length > tolerance * extent))
        return failure("A polygon needs a resolvable plane and area.");
    const auto normal = area / area_length;
    for (std::uint32_t i = 0; i < range.count; ++i)
        if (std::abs(math::dot(point(i) - origin, normal)) > tolerance)
            return failure("Surface polygons must be planar at their float coordinate precision.");
    const auto edge = [&](std::uint32_t i) {
        const auto e = point(i + 1 == range.count ? 0 : i + 1) - point(i);
        return e - normal * math::dot(e, normal);
    };
    auto previous = edge(range.count - 1);
    double previous_length = math::length(previous);
    double turns = 0;
    for (std::uint32_t i = 0; i < range.count; ++i) {
        const auto next = edge(i);
        const double next_length = math::length(next);
        if (!(previous_length > tolerance) || !(next_length > tolerance))
            return failure("A polygon edge is unresolved at its float coordinate precision.");
        const double cross = math::dot(math::cross(previous, next), normal);
        const double dot = math::dot(previous, next);
        const double turn_tolerance = tolerance * (previous_length + next_length);
        if (cross < 0 || (dot <= 0 && cross <= turn_tolerance))
            return failure("Surface polygons must be convex without backtracking edges.");
        turns += std::atan2(cross, dot);
        previous = next;
        previous_length = next_length;
    }
    if (std::abs(turns - 2 * std::numbers::pi) > 64 * std::numeric_limits<double>::epsilon() * range.count)
        return failure("Surface polygons must have a simple boundary.");
    return {};
}

}

Result<TriangleView> TriangleView::create(const Geometry& surface) {
    std::uint32_t size = 0;
    bool has_subtriangles = false;
    for (std::uint32_t p = 0; p < surface.count(Layer::primitive); ++p) {
        const auto range = surface.primitive(p);
        if (range.kind != PrimitiveKind::polygon)
            return failure("Surface operations do not accept polylines.");
        if (range.count > 3) {
            ATELIER_TRY(validate_polygon(surface, range));
            has_subtriangles = true;
        }
        size += range.count - 2; // Bounded by Geometry's total vertex count.
    }
    return TriangleView{surface, size, has_subtriangles};
}

}
