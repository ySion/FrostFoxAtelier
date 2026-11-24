#include "detail/bevel_clip.hpp"
#include "foundation/map.hpp"
#include "foundation/result.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace atelier::procedural::detail {
namespace {

constexpr std::uint8_t inside = 1, on_plane = 2, outside = 4;

std::uint64_t edge_key(std::uint32_t a, std::uint32_t b) {
    return (std::uint64_t{std::min(a, b)} << 32) | std::max(a, b);
}

struct SectionEdge {
    std::uint32_t from, to;

    std::uint64_t key() const { return edge_key(from, to); }
};

// The section inherits the opposite winding to the retained side faces. A face
// partition can contribute a pair of interior halfedges; only single uses bound
// the section. Keeping the intermediate IDs preserves collinear seam vertices.
Result<Array<std::uint32_t>> section_loop(Array<SectionEdge>& edges) {
    std::ranges::sort(edges, {}, &SectionEdge::key);
    Map<std::uint32_t, std::uint32_t> next;
    auto start = std::numeric_limits<std::uint32_t>::max();
    for (std::size_t i = 0; i < edges.size();) {
        const auto edge = edges[i++];
        if (i < edges.size() && edges[i].key() == edge.key()) {
            const auto other = edges[i++];
            if (other.from != edge.to || (i < edges.size() && edges[i].key() == edge.key()))
                return failure("Bevel clipping found inconsistent uses of a section edge.");
            continue;
        }
        if (!next.try_emplace(edge.to, edge.from).inserted)
            return failure("Bevel clipping found a branching section at this precision.");
        start = std::min(start, edge.to);
    }
    if (next.size() < 3)
        return failure("Bevel clipping needs a section with at least three resolvable edges.");

    Array<std::uint32_t> loop;
    loop.reserve(next.size());
    auto point = start;
    do {
        const auto* successor = next.find(point);
        if (!successor || loop.size() == next.size())
            return failure("Bevel clipping could not close the section boundary.");
        loop.add(point);
        point = *successor;
    } while (point != start);
    if (loop.size() != next.size())
        return failure("Bevel clipping found disconnected section boundaries on one convex component.");
    return loop;
}

} // namespace

Result<bool> BevelHull::clip(math::Vector3d normal, double offset, double tolerance, std::uint32_t donor,
                             std::uint32_t feature) {
    const auto initial_points = points.size();
    const auto distance = [&](std::uint32_t index) {
        if (index >= initial_points) // Intersections created by this cut lie on its plane by construction.
            return 0.0;
        const double d = math::dot(points[index], normal) - offset;
        return std::abs(d) <= tolerance ? 0.0 : d;
    };
    Array<std::uint8_t> face_sides;
    face_sides.reserve(faces.size());
    std::uint8_t hull_sides = 0;
    for (const auto& face : faces) {
        std::uint8_t sides = 0;
        for (const auto point : face.points) {
            const double d = distance(point);
            sides |= d < 0 ? inside : (d > 0 ? outside : on_plane);
        }
        hull_sides |= sides;
        face_sides.add(sides);
    }
    if (!(hull_sides & outside))
        return false;
    if (!(hull_sides & inside))
        return failure("Bevel radius removes the convex component at this precision.");

    Map<std::uint64_t, std::uint32_t> intersections;
    const auto cross_edge = [&](std::uint32_t a, std::uint32_t b, double da, double db) -> Result<std::uint32_t> {
        const auto key = edge_key(a, b);
        if (const auto* existing = intersections.find(key))
            return *existing;
        if (points.size() >= element_limit)
            return failure("Bevel clipping exceeds the point limit.");
        // Start at the closer endpoint to avoid subtractive cancellation near old vertices.
        if (std::abs(da) > std::abs(db)) {
            std::swap(a, b);
            std::swap(da, db);
        }
        const auto intersection = points[a] + (points[b] - points[a]) * (da / (da - db));
        const auto index = static_cast<std::uint32_t>(points.size());
        points.add(intersection);
        intersections.try_emplace(key, index);
        return index;
    };

    std::size_t kept_faces = 0;
    std::uint64_t kept_corners = 0;
    Array<SectionEdge> section;
    for (std::size_t i = 0; i < faces.size(); ++i) {
        const auto sides = face_sides[i];
        if (!(sides & inside))
            continue;
        auto& face = faces[i];
        if (sides & outside) {
            Array<std::uint32_t> clipped;
            clipped.reserve(face.points.size() + 1); // A convex polygon gains at most one vertex per cut.
            auto a = face.points.back();
            double da = distance(a);
            for (const auto b : face.points) {
                const double db = distance(b);
                if ((da < 0 && db > 0) || (da > 0 && db < 0)) {
                    ATELIER_TRY_BIND(intersection, cross_edge(a, b, da, db));
                    clipped.add(*intersection);
                }
                if (db <= 0)
                    clipped.add(b);
                a = b;
                da = db;
            }
            if (clipped.size() < 3)
                return failure("Bevel clipping collapsed a retained face at this precision.");
            face.points = std::move(clipped);
        }
        if (sides & (on_plane | outside)) {
            auto a = face.points.back();
            bool a_on = distance(a) == 0;
            for (const auto b : face.points) {
                const bool b_on = distance(b) == 0;
                if (a_on && b_on)
                    section.add({a, b});
                a = b;
                a_on = b_on;
            }
        }
        kept_corners += face.points.size();
        if (kept_corners > element_limit)
            return failure("Bevel clipping exceeds the corner limit.");
        if (kept_faces != i)
            faces[kept_faces] = std::move(face);
        ++kept_faces;
    }
    if (kept_faces >= element_limit)
        return failure("Bevel clipping exceeds the primitive limit.");
    ATELIER_TRY_BIND(cap, section_loop(section));
    if (kept_corners + cap->size() > element_limit)
        return failure("Bevel clipping exceeds the corner limit.");
    math::Vector3d area{};
    const auto origin = points[cap->front()];
    for (std::size_t i = 1; i + 1 < cap->size(); ++i)
        area += math::cross(points[(*cap)[i]] - origin, points[(*cap)[i + 1]] - origin);
    if (!(math::dot(area, normal) > 0))
        return failure("Bevel clipping needs an outward section with positive area.");
    faces.resize(kept_faces);
    faces.add({std::move(*cap), donor, false, feature});
    return true;
}

}
