#include "procedural/operations.hpp"
#include "foundation/result.hpp"
#include "procedural/random.hpp"
#include "procedural/timing.hpp"
#include "procedural/triangles.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace atelier::procedural {
namespace {

using detail::widen;

using math::Vector3;

using math::Vector3d;

constexpr std::uint32_t work_limit = 1'048'576;

struct Sum {
    double value{};

    double correction{};

    void add(double x) {
        const double adjusted = x - correction;
        const double next = value + adjusted;
        correction = (next - value) - adjusted;
        value = next;
    }
};

// Boundary halfedges, not fan diagonals. Across each edge, link corners around
// their common point. A closed manifold has exactly one such cycle at each used point.
Result<void> validate_shells(const Geometry& surface) {
    struct Edge {
        std::uint64_t key;

        std::uint32_t corner;
    };
    const auto points = surface.vertex_points();
    std::vector<Edge> edges;
    edges.reserve(points.size());
    std::vector<std::uint32_t> next(points.size());
    for (std::uint32_t p = 0; p < surface.count(Layer::primitive); ++p) {
        const auto face = surface.primitive(p);
        for (std::uint32_t i = 0; i < face.count; ++i) {
            const auto c = face.first + i;
            next[c] = face.first + (i + 1 == face.count ? 0 : i + 1);
            const auto a = points[c];
            const auto b = points[next[c]];
            edges.push_back({(std::uint64_t{std::min(a, b)} << 32) | std::max(a, b), c});
        }
    }
    std::ranges::sort(edges, {}, &Edge::key);
    std::vector<std::uint32_t> around(points.size());
    for (std::size_t i = 0; i < edges.size(); i += 2) {
        if (i + 1 == edges.size() || edges[i].key != edges[i + 1].key ||
            (i + 2 < edges.size() && edges[i].key == edges[i + 2].key))
            return failure("Volume needs exactly two faces per boundary edge.");
        const auto a = edges[i].corner;
        const auto b = edges[i + 1].corner;
        if (points[a] != points[next[b]] || points[b] != points[next[a]])
            return failure("Volume boundary edges must have opposite directions.");
        around[a] = next[b];
        around[b] = next[a];
    }
    std::vector<bool> seen_corner(points.size());
    std::vector<bool> seen_point(surface.count(Layer::point));
    for (std::uint32_t start = 0; start < points.size(); ++start) {
        if (seen_corner[start])
            continue;
        if (seen_point[points[start]])
            return failure("Volume needs a single face fan at each used point.");
        seen_point[points[start]] = true;
        auto c = start;
        do {
            seen_corner[c] = true;
            c = around[c];
        } while (c != start);
    }
    return {};
}

struct Triangle {
    std::array<Vector3d, 3> points;

    Vector3d normal;

    double tolerance;
};

Result<std::vector<Triangle>> prepare(const Geometry& surface) {
    if (!surface.count(Layer::primitive) || surface.count(Layer::instance))
        return failure("Volume requires polygon shells without instances.");
    for (const auto p : surface.positions())
        if (!math::is_finite(p))
            return failure("Volume positions must be finite.");
    ATELIER_TRY_BIND(view, detail::TriangleView::create(surface, true));
    ATELIER_TRY(validate_shells(surface));
    std::vector<Triangle> triangles;
    triangles.reserve(view->size());
    const auto origin = widen(surface.positions()[surface.vertex_points()[0]]);
    Sum volume;
    double tolerance = 0;
    for (const auto fan : *view) {
        Triangle t{};
        if (fan.triangle == 0)
            tolerance = detail::polygon_scale(surface, surface.primitive(fan.primitive)).tolerance;
        for (std::size_t i = 0; i < 3; ++i)
            t.points[i] = widen(surface.positions()[surface.vertex_points()[fan.corners[i]]]);
        const auto a = t.points[0];
        const auto b = t.points[1];
        const auto c = t.points[2];
        const auto cross = math::cross(b - a, c - a);
        const double area = math::length(cross);
        if (!area) // A valid polygon may contain forward collinear corners.
            continue;
        t.normal = cross / area;
        t.tolerance = tolerance;
        volume.add(math::dot(a - origin, math::cross(b - origin, c - origin)));
        triangles.push_back(t);
    }
    if (!(volume.value > 0))
        return failure("Volume needs positive aggregate signed volume; inward cavities are allowed.");
    return triangles;
}

bool inside(std::span<const Triangle> triangles, Vector3 position) {
    const auto q = widen(position);
    Sum angles;
    for (const auto& t : triangles) {
        // Plane distance is only a broad phase. Expanded edge halfplanes would
        // incorrectly exclude distant points along a narrow fan's extended tip.
        if (std::abs(math::dot(q - t.points[0], t.normal)) <= t.tolerance) {
            const auto uv = detail::closest_on_triangle(q, t.points[0], t.points[1], t.points[2]);
            const auto closest = t.points[0] + (t.points[1] - t.points[0]) * uv.x + (t.points[2] - t.points[0]) * uv.y;
            const auto offset = closest - q;
            if (math::dot(offset, offset) <= t.tolerance * t.tolerance)
                return false;
        }
        const auto a = t.points[0] - q;
        const auto b = t.points[1] - q;
        const auto c = t.points[2] - q;
        const double la = math::length(a);
        const double lb = math::length(b);
        const double lc = math::length(c);
        angles.add(2 * std::atan2(math::dot(a, math::cross(b, c)),
                                  la * lb * lc + math::dot(a, b) * lc + math::dot(b, c) * la + math::dot(c, a) * lb));
    }
    // Do not early-accept a partial sum: later inward shells can cancel it.
    return angles.value >= 2 * std::numbers::pi;
}

}

Result<Geometry> volume_scatter(const Geometry& surface, const VolumeScatterSettings& settings) {
    timing::Scope execution{phases::volume};
    if (!std::isfinite(settings.density) || settings.density < 0 || !std::isfinite(settings.cell_size) ||
        settings.cell_size <= 0 || !math::is_finite(settings.origin))
        return failure("Volume density must be finite and non-negative, cell size positive, and origin finite.");
    ATELIER_TRY_BIND(triangles, prepare(surface));
    // Preparation validated a nonempty shell and every input point. Only used
    // points bound its cells; loose points cannot spend the sampling budget.
    const auto corners = surface.vertex_points();
    const auto& points = surface.positions();
    Bounds box{points[corners[0]], points[corners[0]]};
    for (const auto index : corners)
        for (std::size_t axis = 0; axis < 3; ++axis) {
            box.minimum[axis] = std::min(box.minimum[axis], points[index][axis]);
            box.maximum[axis] = std::max(box.maximum[axis], points[index][axis]);
        }
    const double h = settings.cell_size;
    std::array<std::int64_t, 3> first{};
    std::array<std::uint32_t, 3> widths{};
    std::uint32_t cells = 1;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const double lo = std::floor((double{box.minimum[axis]} - settings.origin[axis]) / h);
        const double hi = std::ceil((double{box.maximum[axis]} - settings.origin[axis]) / h) - 1;
        if (lo < std::numeric_limits<std::int32_t>::min() || hi > std::numeric_limits<std::int32_t>::max() || hi < lo)
            return failure("Volume cell coordinates must fit int32 and span a nonempty volume.");
        const double width = hi - lo + 1;
        if (width > work_limit / cells)
            return failure("Volume exceeds 1,048,576 intersecting cells.");
        first[axis] = static_cast<std::int64_t>(lo);
        widths[axis] = static_cast<std::uint32_t>(width);
        cells *= widths[axis];
    }
    const auto coordinate = [&](std::uint32_t i) {
        return std::array<std::int32_t, 3>{static_cast<std::int32_t>(first[0] + i / (widths[1] * widths[2])),
                                           static_cast<std::int32_t>(first[1] + i / widths[2] % widths[1]),
                                           static_cast<std::int32_t>(first[2] + i % widths[2])};
    };
    const auto cell_seed = [&](const auto& xyz) {
        return hash(hash(hash(settings.seed, static_cast<std::uint32_t>(xyz[0])), static_cast<std::uint32_t>(xyz[1])),
                    static_cast<std::uint32_t>(xyz[2]));
    };
    const double expected = settings.density * h * h * h;
    if (!std::isfinite(expected) || std::floor(expected) > work_limit / cells)
        return failure("Volume exceeds 1,048,576 candidates before filtering.");
    const auto whole = static_cast<std::uint32_t>(std::floor(expected));
    std::vector<std::uint32_t> counts(cells);
    std::uint32_t candidates = 0;
    for (std::uint32_t i = 0; i < cells; ++i) {
        counts[i] = whole + (unit(hash(cell_seed(coordinate(i)), 0)) < expected - whole);
        if (counts[i] > work_limit - candidates)
            return failure("Volume exceeds 1,048,576 candidates before filtering.");
        candidates += counts[i];
    }
    Array<Vector3> positions;
    Array<std::uint32_t> ids;
    Array<std::uint32_t> ordinals;
    std::array<Array<std::int32_t>, 3> sources;
    positions.reserve(candidates);
    ids.reserve(candidates);
    ordinals.reserve(candidates);
    for (auto& source : sources)
        source.reserve(candidates);
    {
        timing::Scope classification{phases::classify};
        for (std::uint32_t i = 0; i < cells; ++i) {
            const auto xyz = coordinate(i);
            const auto seed = cell_seed(xyz);
            for (std::uint32_t k = 0; k < counts[i]; ++k) {
                const auto id = hash(seed, k);
                Vector3 p;
                for (std::uint32_t axis = 0; axis < 3; ++axis)
                    p[axis] = static_cast<float>(settings.origin[axis] +
                                                 (static_cast<double>(xyz[axis]) + unit(hash(id, axis + 1))) * h);
                if (!math::is_finite(p))
                    return failure("Volume candidate position exceeds float range.");
                if (!inside(*triangles, p))
                    continue;
                positions.add(p);
                ids.add(id);
                ordinals.add(k);
                for (std::size_t axis = 0; axis < 3; ++axis)
                    sources[axis].add(xyz[axis]);
            }
        }
    }
    ATELIER_TRY_BIND(result, Geometry::points(std::move(positions)));
    ATELIER_TRY(result->set(String{"id"}, Layer::point, SharedArray<std::uint32_t>{std::move(ids)}));
    constexpr const char* names[]{"sourcecellx", "sourcecelly", "sourcecellz"};
    for (std::size_t axis = 0; axis < 3; ++axis)
        ATELIER_TRY(
            result->set(String{names[axis]}, Layer::point, SharedArray<std::int32_t>{std::move(sources[axis])}));
    ATELIER_TRY(result->set(String{"sourceordinal"}, Layer::point, SharedArray<std::uint32_t>{std::move(ordinals)}));
    return result;
}

}
