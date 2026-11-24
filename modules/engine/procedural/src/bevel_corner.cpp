#include "detail/bevel_corner.hpp"
#include "foundation/inline_array.hpp"
#include "foundation/result.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace atelier::procedural::detail {
namespace {

using math::Vector3d;

// All hull predicates operate in unit-radius corner coordinates, independently
// of the object's translation and bevel radius.
constexpr double plane_epsilon = 256.0 * std::numeric_limits<double>::epsilon();

constexpr std::size_t max_corner_samples = 2048;

bool in_normal_cone(Vector3d direction, std::span<const Vector3d> sides) {
    return std::ranges::all_of(sides, [&](Vector3d side) { return math::dot(side, direction) >= -plane_epsilon; });
}

struct Facet {
    std::array<std::uint32_t, 3> vertices;

    Vector3d normal;
};

struct HorizonEdge {
    std::uint32_t from;

    std::uint32_t to;

    std::uint64_t key() const noexcept { return (std::uint64_t{std::min(from, to)} << 32) | std::max(from, to); }
};

Result<Facet> oriented_facet(const Array<Vector3d>& points, Vector3d inside, std::uint32_t a, std::uint32_t b,
                             std::uint32_t c) {
    const auto ab = points[b] - points[a];
    const auto ac = points[c] - points[a];
    auto normal = math::cross(ab, ac);
    const double length = math::length(normal);
    if (!(length > plane_epsilon * std::max(math::length(ab), math::length(ac))))
        return failure("Bevel corner has an unresolved facet in its sampled normal cone.");
    normal /= length;
    if (math::dot(normal, inside - points[a]) > 0.0) {
        std::swap(b, c);
        normal = -normal;
    }
    return Facet{{a, b, c}, normal};
}

// Grow the hull of the spherical stations plus their common center (point 0).
// The center supplies a tetrahedron even for a planar single-segment cap.
// Only the horizon of the visible facets is retriangulated at each insertion.
Result<Array<Facet>> station_hull(const Array<Vector3d>& points) {
    constexpr std::uint32_t a = 1;
    std::uint32_t b = 0;
    double best = 0.0;
    for (std::uint32_t i = 2; i < points.size(); ++i) {
        const double strength = math::length_squared(math::cross(points[a], points[i]));
        if (strength > best) {
            best = strength;
            b = i;
        }
    }
    if (!(best > plane_epsilon * plane_epsilon))
        return failure("Bevel corner normals cannot resolve a two-dimensional fan.");
    const auto baseline = math::cross(points[a], points[b]) / std::sqrt(best);
    std::uint32_t c = 0;
    best = 0.0;
    for (std::uint32_t i = 1; i < points.size(); ++i) {
        const double distance = std::abs(math::dot(baseline, points[i]));
        if (distance > best) {
            best = distance;
            c = i;
        }
    }
    if (!(best > plane_epsilon))
        return failure("Bevel corner normals cannot resolve a solid angle.");

    const auto inside = (points[a] + points[b] + points[c]) * 0.25;
    Array<Facet> facets;
    facets.reserve(2 * points.size());
    for (const auto indices :
         std::array{std::array{0u, a, b}, std::array{0u, b, c}, std::array{0u, c, a}, std::array{a, b, c}}) {
        ATELIER_TRY_BIND(face, oriented_facet(points, inside, indices[0], indices[1], indices[2]));
        facets.add(*face);
    }

    Array<HorizonEdge> edges;
    for (std::uint32_t i = 1; i < points.size(); ++i) {
        if (i == a || i == b || i == c)
            continue;
        edges.clear();
        std::size_t kept = 0;
        for (const auto& face : facets) {
            if (math::dot(face.normal, points[i] - points[face.vertices[0]]) <= plane_epsilon) {
                facets[kept++] = face;
                continue;
            }
            for (std::uint32_t j = 0; j != 3; ++j)
                edges.add({face.vertices[j], face.vertices[(j + 1) % 3]});
        }
        if (edges.empty())
            return failure("Bevel corner stations coincide at the available angular precision.");
        facets.resize(kept);
        std::ranges::sort(edges, {}, &HorizonEdge::key);
        for (std::size_t j = 0; j < edges.size();) {
            const auto edge = edges[j++];
            if (j < edges.size() && edges[j].key() == edge.key()) {
                if (edge.from != edges[j].to || edge.to != edges[j].from ||
                    (j + 1 < edges.size() && edges[j + 1].key() == edge.key()))
                    return failure("Bevel corner hull has an inconsistent horizon.");
                ++j;
                continue;
            }
            ATELIER_TRY_BIND(face, oriented_facet(points, inside, edge.from, edge.to, i));
            facets.add(*face);
        }
    }
    return facets;
}

} // namespace

math::Vector3d bevel_arc(math::Vector3d a, math::Vector3d b, std::uint32_t index, std::uint32_t segments) noexcept {
    if (index == 0)
        return a;
    if (index == segments)
        return b;
    const double step = std::atan2(math::length(math::cross(a, b)), math::dot(a, b)) / segments;
    // Symmetric weights give identical stations when an incident face traverses
    // the same edge in the opposite order, including at non-midpoint stations.
    const auto point = a * std::sin(step * (segments - index)) + b * std::sin(step * index);
    return point / math::length(point);
}

Result<Array<BevelPlane>> bevel_corner_planes(std::span<const math::Vector3d> normals, std::uint32_t segments) {
    if (normals.size() < 3 || segments == 0 || segments > 8)
        return failure("Bevel corner requires a closed normal fan and 1 to 8 segments.");

    Vector3d sum;
    for (const auto normal : normals)
        sum += normal;
    const double sum_length = math::length(sum);
    if (!(sum_length > plane_epsilon * static_cast<double>(normals.size())))
        return failure("Bevel corner has no resolvable interior normal.");
    const auto center = sum / sum_length;
    InlineArray<Vector3d, 8> sides;
    sides.reserve(normals.size());
    const bool reversed = math::dot(math::cross(normals[0], normals[1]), center) < 0.0;
    for (std::size_t i = 0; i < normals.size(); ++i) {
        auto side = math::cross(normals[i], normals[(i + 1) % normals.size()]);
        const double length = math::length(side);
        if (!(length > plane_epsilon))
            return failure("Bevel corner has indistinguishable or opposite adjacent face normals.");
        side /= reversed ? -length : length;
        if (!(math::dot(side, center) > plane_epsilon))
            return failure("Bevel corner needs an ordered, convex normal fan.");
        sides.add(side);
    }

    if (segments == 1) {
        if (normals.size() == 3) {
            for (std::size_t i = 0; i < 3; ++i) {
                const auto a = normals[(i + 1) % 3] - normals[i];
                const auto b = normals[(i + 2) % 3] - normals[i];
                // A right/obtuse chord triangle closes with the three strips.
                // With t_i = 1 - dot(n_i,p), they impose t_i+t_j >= 1-dot(n_i,n_j).
                // Here the longest constraint is at least the sum of the other two.
                // The remaining corners on face i lie behind its station n_i;
                // an additional plane supporting all stations would be redundant.
                if (math::dot(a, b) <= plane_epsilon * math::length(a) * math::length(b))
                    return Array<BevelPlane>{};
            }
        }
        // Most authored caps have three faces at a corner. Regular pyramids can
        // have arbitrary valence and still share a single planar chord.
        Vector3d area;
        for (std::size_t i = 1; i + 1 < normals.size(); ++i)
            area += math::cross(normals[i] - normals[0], normals[i + 1] - normals[0]);
        const double length = math::length(area);
        if (!(length > plane_epsilon * math::length(normals[1] - normals[0])))
            return failure("Bevel corner chamfer has no resolvable area.");
        const auto direction = area / (math::dot(area, center) < 0.0 ? -length : length);
        if (in_normal_cone(direction, sides) && std::ranges::all_of(normals, [&](Vector3d point) {
                return std::abs(math::dot(direction, point - normals[0])) <= plane_epsilon;
            })) {
            // A planar chord whose normal is on the cone rim is the same
            // plane as that edge's chord. The strip already closes the miter.
            if (std::ranges::any_of(sides, [&](Vector3d side) { return math::dot(side, direction) <= plane_epsilon; }))
                return Array<BevelPlane>{};
            return Array<BevelPlane>{{direction, math::dot(direction, normals[0])}};
        }
    }

    Array<Vector3d> points;
    const auto add_ring = [&](std::uint32_t ring, std::uint32_t rings, std::uint32_t steps) {
        for (std::size_t face = 0; face < normals.size(); ++face) {
            const auto next = normals[(face + 1) % normals.size()];
            for (std::uint32_t j = 0; j < steps; ++j) {
                const auto rim = bevel_arc(normals[face], next, j, steps);
                points.add(bevel_arc(center, rim, ring, rings));
            }
        }
    };
    if (normals.size() == 3) {
        // A triangular lattice needs S² patches before coplanar merging.
        // Its boundary follows the equal-angle edge stations exactly. Positive
        // barycentric triples add only the stations strictly inside the cap.
        points.reserve(1 + (segments + 1) * (segments + 2) / 2);
        points.add({});
        add_ring(1, 1, segments);
        for (std::uint32_t i = 1; i + 1 < segments; ++i) {
            for (std::uint32_t j = 1; i + j < segments; ++j) {
                const auto k = segments - i - j;
                const auto point = normals[0] * double(i) + normals[1] * double(j) + normals[2] * double(k);
                const double length = math::length(point);
                if (!(length > plane_epsilon * segments))
                    return failure("Bevel corner cannot resolve an interior spherical station.");
                points.add(point / length);
            }
        }
    } else {
        // A general polygon has no three-way barycentric lattice. Its rings
        // reduce their station count toward the pole. For one segment the pole
        // also resolves a high-valence corner whose rim is not one planar chord.
        const std::uint32_t rings = (segments + 1) / 2;
        std::uint32_t stations_per_side = 0;
        for (std::uint32_t ring = 1; ring <= rings; ++ring)
            stations_per_side += (ring * segments + rings - 1) / rings;
        constexpr std::size_t fixed = 2;
        if (normals.size() > (max_corner_samples - fixed) / stations_per_side)
            return failure("Bevel corner exceeds its local hull budget of 2048 angular samples.");
        points.reserve(fixed + normals.size() * stations_per_side);
        points.add({});
        points.add(center);
        for (std::uint32_t ring = 1; ring <= rings; ++ring)
            add_ring(ring, rings, (ring * segments + rings - 1) / rings);
    }

    ATELIER_TRY_BIND(hull, station_hull(points));
    Array<BevelPlane> planes;
    planes.reserve(hull->size());
    for (const auto& face : *hull) {
        if (std::ranges::find(face.vertices, 0u) != face.vertices.end() || !in_normal_cone(face.normal, sides))
            continue;
        // The retained solid continues along each incident edge. Only normals
        // in the polar of those inward edge rays can support the corner without
        // also cutting the continuation of another strip. Triangulated coplanar
        // hull facets need just one cut. Refit support to include every station.
        if (std::ranges::any_of(planes, [&](const BevelPlane& plane) {
                return math::length_squared(plane.normal - face.normal) <= plane_epsilon * plane_epsilon;
            }))
            continue;
        double offset = math::dot(face.normal, points[face.vertices[0]]);
        for (const auto point : points)
            offset = std::max(offset, math::dot(face.normal, point));
        planes.add({face.normal, offset});
    }
    if (planes.empty())
        return failure("Bevel corner has no supporting chord inside its incident normal cone.");
    return planes;
}

}
