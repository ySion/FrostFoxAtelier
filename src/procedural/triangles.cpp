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
    math::Vector3d area;
    for (std::uint32_t i = 0; i < range.count; ++i) {
        const auto p = point(i);
        if (!math::is_finite(p))
            return failure("Polygon positions must be finite.");
        if (i > 0 && i + 1 < range.count)
            area += math::cross(p - origin, point(i + 1) - origin);
    }
    const auto [extent, tolerance] = polygon_scale(surface, range);
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

double surface_tolerance(double coordinate_scale, double extent) noexcept {
    return 8 * std::numeric_limits<float>::epsilon() * std::max(coordinate_scale, extent);
}

PolygonScale polygon_scale(const Geometry& surface, PrimitiveRange range) noexcept {
    const auto corners = surface.vertex_points();
    const auto first = surface.positions()[corners[range.first]];
    math::Vector3d minimum{first.x, first.y, first.z};
    auto maximum = minimum;
    double coordinate_scale = 0;
    for (std::uint32_t i = 0; i < range.count; ++i) {
        const auto p = surface.positions()[corners[range.first + i]];
        for (std::size_t axis = 0; axis < 3; ++axis) {
            minimum[axis] = std::min(minimum[axis], double{p[axis]});
            maximum[axis] = std::max(maximum[axis], double{p[axis]});
            coordinate_scale = std::max(coordinate_scale, std::abs(double{p[axis]}));
        }
    }
    const double extent = math::length(maximum - minimum);
    return {extent, surface_tolerance(coordinate_scale, extent)};
}

// Ericson, Real-Time Collision Detection 5.1.5: q = a + v (b - a) + w (c - a).
math::Vector2d closest_on_triangle(math::Vector3d p, math::Vector3d a, math::Vector3d b, math::Vector3d c) {
    const auto ab = b - a;
    const auto ac = c - a;
    const auto ap = p - a;
    const double d1 = math::dot(ab, ap);
    const double d2 = math::dot(ac, ap);
    if (d1 <= 0 && d2 <= 0)
        return {0, 0};
    const auto bp = p - b;
    const double d3 = math::dot(ab, bp);
    const double d4 = math::dot(ac, bp);
    if (d3 >= 0 && d4 <= d3)
        return {1, 0};
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0)
        return {d1 / (d1 - d3), 0};
    const auto cp = p - c;
    const double d5 = math::dot(ab, cp);
    const double d6 = math::dot(ac, cp);
    if (d6 >= 0 && d5 <= d6)
        return {0, 1};
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0)
        return {0, d2 / (d2 - d6)};
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0) {
        const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return {1 - w, w};
    }
    const double scale = 1 / (va + vb + vc);
    return {vb * scale, vc * scale};
}

Result<TriangleView> TriangleView::create(const Geometry& surface, bool require_nondegenerate) {
    std::uint32_t size = 0;
    bool has_subtriangles = false;
    for (std::uint32_t p = 0; p < surface.count(Layer::primitive); ++p) {
        const auto range = surface.primitive(p);
        if (range.kind != PrimitiveKind::polygon)
            return failure("Surface operations do not accept polylines.");
        if (range.count > 3 || require_nondegenerate)
            ATELIER_TRY(validate_polygon(surface, range));
        has_subtriangles |= range.count > 3;
        size += range.count - 2; // Bounded by Geometry's total vertex count.
    }
    return TriangleView{surface, size, has_subtriangles};
}

}
