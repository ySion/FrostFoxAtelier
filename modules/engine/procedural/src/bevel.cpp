#include "procedural/operations.hpp"

#include "detail/bevel_attributes.hpp"
#include "detail/bevel_clip.hpp"
#include "detail/bevel_corner.hpp"
#include "detail/bevel_offset.hpp"
#include "detail/topology.hpp"
#include "detail/triangles.hpp"
#include "foundation/map.hpp"
#include "foundation/result.hpp"
#include "foundation/set.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>
#include <ranges>

namespace atelier::procedural {
namespace {

using math::Vector3d;
using Index = std::uint32_t;
constexpr Index missing = std::numeric_limits<Index>::max();

std::uint64_t pair_key(Index a, Index b) {
    return (std::uint64_t{std::min(a, b)} << 32) | std::max(a, b);
}

struct Component {
    Array<Index> points;
    Array<Index> faces;
};

struct Source {
    explicit Source(const Geometry& geometry) : surface(geometry) {}
    const Geometry& surface;
    Array<Index> around;
    Array<Index> face_at;
    Array<Index> first_corner;
    Array<Index> local_point;
    Array<Index> face_patch;
    Array<Vector3d> normals;
    Array<Component> components;
};

// Reuse the closed-fan proof and its actual twins: coincident points belonging
// to different shells must never become incident by a position lookup.
Result<Source> read_source(const Geometry& surface) {
    ATELIER_TRY(detail::TriangleView::create(surface, true));
    Source source{surface};
    ATELIER_TRY(detail::validate_shells(surface, &source.around));
    const auto pieces = detail::point_pieces(surface);
    const auto corners = surface.vertex_points();
    source.first_corner.resize(pieces.size(), missing);
    source.local_point.resize(pieces.size());
    source.face_at.resize(corners.size());
    source.face_patch.resize(surface.count(Layer::primitive), missing);
    for (Index i = 0; i < pieces.size(); ++i) {
        if (pieces[i] >= source.components.size())
            source.components.resize(std::size_t{pieces[i]} + 1);
        auto& points = source.components[pieces[i]].points;
        source.local_point[i] = static_cast<Index>(points.size());
        points.add(i);
    }
    for (Index f = 0; f < surface.count(Layer::primitive); ++f) {
        const auto face = surface.primitive(f);
        const auto start = corners[face.first];
        source.components[pieces[start]].faces.add(f);
        const auto anchor = Vector3d(surface.positions()[start]);
        Vector3d area{};
        for (Index i = 0; i < face.count; ++i) {
            const auto c = face.first + i;
            source.face_at[c] = f;
            if (source.first_corner[corners[c]] == missing)
                source.first_corner[corners[c]] = c;
            if (i > 0 && i + 1 < face.count)
                area += math::cross(Vector3d(surface.positions()[corners[c]]) - anchor,
                                    Vector3d(surface.positions()[corners[c + 1]]) - anchor);
        }
        source.normals.add(area / math::length(area)); // TriangleView certified the area.
    }
    if (std::ranges::find(source.first_corner, missing) != source.first_corner.end())
        return failure("Bevel requires closed polygon surfaces without isolated points.");
    return source;
}

struct Support {
    Vector3d normal;
    double offset;
    Index donor;
};

// Proving convexity needs the support value for every distinct face plane.
// A point hierarchy skips whole boxes below that plane on densely tessellated
// shells. Small components use the same comparison in a contiguous scan.
class SupportPoints {
    struct Block {
        Vector3d lo, hi;
        Index begin, end;
        std::size_t after;
    };
    std::span<const Vector3d> points_;
    Array<Index> order_;
    Array<Block> blocks_;

    void partition(Index begin, Index end) {
        auto lo = points_[order_[begin]];
        auto hi = lo;
        for (Index i = begin + 1; i < end; ++i) {
            const auto p = points_[order_[i]];
            for (unsigned axis = 0; axis < 3; ++axis) {
                lo[axis] = std::min(lo[axis], p[axis]);
                hi[axis] = std::max(hi[axis], p[axis]);
            }
        }
        const auto id = blocks_.size();
        blocks_.add({lo, hi, begin, end, 0});
        if (end - begin > 24) {
            const auto extent = hi - lo;
            unsigned axis = extent.y > extent.x ? 1 : 0;
            if (extent.z > extent[axis])
                axis = 2;
            const auto mid = begin + (end - begin) / 2;
            std::nth_element(order_.begin() + begin, order_.begin() + mid, order_.begin() + end,
                             [&](Index a, Index b) { return points_[a][axis] < points_[b][axis]; });
            blocks_[id].end = begin;
            partition(begin, mid);
            partition(mid, end);
        }
        blocks_[id].after = blocks_.size();
    }

public:
    SupportPoints(std::span<const Vector3d> points, std::size_t planes) : points_(points) {
        if (points.size() * planes <= 262144 || planes < 16)
            return;
        order_.resize(points.size());
        std::iota(order_.begin(), order_.end(), 0u);
        partition(0, static_cast<Index>(points.size()));
    }

    bool outside(const Support& plane, double tolerance) const {
        const auto beyond = [&](Vector3d p) { return math::dot(plane.normal, p) - plane.offset > tolerance; };
        if (blocks_.empty())
            return std::ranges::any_of(points_, beyond);
        const auto n = plane.normal;
        for (std::size_t i = 0; i < blocks_.size();) {
            const auto& b = blocks_[i];
            const Vector3d far{n.x < 0 ? b.lo.x : b.hi.x, n.y < 0 ? b.lo.y : b.hi.y, n.z < 0 ? b.lo.z : b.hi.z};
            if (!beyond(far)) {
                i = b.after;
                continue;
            }
            for (Index j = b.begin; j < b.end; ++j)
                if (beyond(points_[order_[j]]))
                    return true;
            ++i;
        }
        return false;
    }
};

struct Cut {
    Vector3d normal;
    double offset;
    Index donor;
    Index feature;
};

struct Piece {
    Vector3d origin;
    double tolerance;
    double clip_tolerance;
    detail::BevelHull hull;
    Array<Support> supports;
    Array<Cut> cuts;
    Set<std::uint64_t> contacts;
    Index features{};
};

Result<Piece> prepare(Source& source, const Component& component) {
    Piece piece{};
    const auto& surface = source.surface;
    const auto& positions = surface.positions();
    auto lo = Vector3d(positions[component.points.front()]);
    auto hi = lo;
    for (auto p : component.points) {
        for (unsigned i = 0; i < 3; ++i) {
            lo[i] = std::min(lo[i], double{positions[p][i]});
            hi[i] = std::max(hi[i], double{positions[p][i]});
        }
    }
    piece.origin = (lo + hi) * 0.5;
    double magnitude = 0;
    for (unsigned i = 0; i < 3; ++i)
        magnitude = std::max({magnitude, std::abs(lo[i]), std::abs(hi[i])});
    const double extent = math::length(hi - lo);
    piece.tolerance = detail::surface_tolerance(magnitude, extent);
    // Intersect the represented double planes in the local frame. Float storage
    // uncertainty is checked independently before publishing the surface.
    piece.clip_tolerance = 256 * std::numeric_limits<double>::epsilon() * extent;
    auto& hull = piece.hull;
    hull.points.reserve(component.points.size());
    for (auto p : component.points)
        hull.points.add(Vector3d(positions[p]) - piece.origin);
    const auto corners = surface.vertex_points();
    Array<Index> pending;
    for (auto seed : component.faces) {
        if (source.face_patch[seed] != missing)
            continue;
        const auto patch = static_cast<Index>(piece.supports.size());
        const auto n = source.normals[seed];
        const auto h = math::dot(n, hull.points[source.local_point[corners[surface.primitive(seed).first]]]);
        piece.supports.add({n, h, seed});
        source.face_patch[seed] = patch;
        pending.add(seed);
        while (!pending.empty()) {
            const auto face = surface.primitive(pending.back());
            pending.pop();
            for (Index i = 0; i < face.count; ++i) {
                const auto other = source.face_at[source.around[face.first + i]];
                if (source.face_patch[other] != missing || math::dot(n, source.normals[other]) < 1.0 - 1e-12)
                    continue;
                const auto adjacent = surface.primitive(other);
                bool coplanar = true;
                for (Index j = 0; j < adjacent.count; ++j) {
                    const auto p = hull.points[source.local_point[corners[adjacent.first + j]]];
                    if (std::abs(math::dot(n, p) - h) > piece.tolerance) {
                        coplanar = false;
                        break;
                    }
                }
                if (coplanar) {
                    source.face_patch[other] = patch;
                    pending.add(other);
                }
            }
        }
    }
    // Check every support against its own component before clipping. A support
    // query prunes entire point boxes only when even their far corner is inside.
    const SupportPoints queries(hull.points, piece.supports.size());
    for (const auto& support : piece.supports)
        if (queries.outside(support, piece.tolerance))
            return failure("Bevel requires outward-facing convex components; primitive " +
                           std::to_string(support.donor) + " is not a supporting face.");
    // A support can contain many authored faces (material and UV seams).
    // Keep all of them; they only share the identity of their geometric plane.
    for (auto f : component.faces) {
        detail::BevelFace face{{}, f, true, source.face_patch[f]};
        const auto range = surface.primitive(f);
        face.points.reserve(range.count);
        for (Index i = 0; i < range.count; ++i)
            face.points.add(source.local_point[corners[range.first + i]]);
        hull.faces.add(std::move(face));
    }
    piece.features = static_cast<Index>(piece.supports.size());
    return piece;
}

// Center of the common radius arc in a section normal to the ridge. Both
// side planes move inward by r. Correct its axial anchor to the two supports.
Vector3d edge_center(const Support& a, const Support& b, Vector3d anchor, double radius) {
    const auto sum = a.normal + b.normal;
    const auto diff = a.normal - b.normal;
    const auto da = a.offset - math::dot(a.normal, anchor) - radius;
    const auto db = b.offset - math::dot(b.normal, anchor) - radius;
    return anchor + sum * ((da + db) / math::dot(sum, sum)) + diff * ((da - db) / math::dot(diff, diff));
}

Result<void> edge_cuts(Piece& piece, Index a, Index b, Vector3d center, const BevelSettings& settings,
                       Index corner_feature = missing) {
    const bool original_ridge = corner_feature == missing;
    if ((original_ridge && std::uint64_t{piece.features} + settings.segments > element_limit) ||
        piece.cuts.size() + settings.segments > element_limit)
        return failure("Bevel edge strips exceed the geometry element limit.");
    const auto& left = piece.supports[a];
    const auto& right = piece.supports[b];
    auto previous = left.normal;
    Index previous_feature = a;
    for (Index i = 1; i <= settings.segments; ++i) {
        const auto next = detail::bevel_arc(left.normal, right.normal, i, settings.segments);
        const auto normal = (previous + next) / math::length(previous + next);
        const auto feature = original_ridge ? piece.features++ : corner_feature;
        piece.cuts.add({normal, math::dot(normal, center) + settings.radius * math::dot(normal, previous),
                        std::min(left.donor, right.donor), feature});
        if (original_ridge)
            piece.contacts.add(pair_key(previous_feature, feature));
        previous_feature = feature;
        previous = next;
    }
    if (original_ridge)
        piece.contacts.add(pair_key(previous_feature, b));
    return {};
}

struct Vertex {
    Vector3d position;
    Array<Index> patches;
};

Result<void> corner_cuts(Piece& piece, const Vertex& vertex, const BevelSettings& settings) {
    Array<Vector3d> normals;
    Array<double> offsets;
    for (auto id : vertex.patches) {
        const auto& s = piece.supports[id];
        normals.add(s.normal);
        offsets.add(s.offset - math::dot(s.normal, vertex.position) - settings.radius);
    }
    ATELIER_TRY_BIND(cells, detail::bevel_offset_cells(normals, offsets, piece.clip_tolerance * 16));
    const auto feature = piece.features;
    const auto before = piece.cuts.size();
    Set<std::uint64_t> diagonals;
    Array<Vector3d> cone;
    for (const auto& cell : *cells) {
        const auto center = vertex.position + cell.center;
        cone.clear();
        auto donor = missing;
        for (std::size_t i = 0; i < cell.faces.size(); ++i) {
            const auto j = cell.faces[i];
            const auto k = cell.faces[(i + 1) % cell.faces.size()];
            const auto a = vertex.patches[j];
            cone.add(normals[j]);
            donor = std::min(donor, piece.supports[a].donor);
            // Eroding an asymmetric multi-plane vertex can introduce a short
            // edge between two formerly nonadjacent supports. Its fillet joins
            // the two separate spherical patches exactly like an original ridge.
            if (k != (j + 1) % normals.size() && j != (k + 1) % normals.size() && diagonals.add(pair_key(j, k))) {
                ATELIER_TRY(edge_cuts(piece, a, vertex.patches[k], center, settings, feature));
            }
        }
        ATELIER_TRY_BIND(planes, detail::bevel_corner_planes(cone, settings.segments));
        if (piece.cuts.size() + planes->size() > element_limit)
            return failure("Bevel corner cuts exceed the geometry element limit.");
        for (const auto& plane : *planes)
            piece.cuts.add(
                {plane.normal, math::dot(plane.normal, center) + settings.radius * plane.offset, donor, feature});
    }
    if (piece.cuts.size() != before) {
        if (piece.features >= element_limit)
            return failure("Bevel corner patches exceed the geometry element limit.");
        ++piece.features;
    }
    return {};
}

Result<void> select_cuts(Piece& piece, const Source& source, const Component& component,
                         const BevelSettings& settings) {
    const double threshold = settings.angle_degrees * (std::numbers::pi / 180);
    Map<std::uint64_t, bool> ridges;
    Array<Vertex> corners;
    Array<Index> ring;
    for (auto p : component.points) {
        ring.clear();
        const auto start = source.first_corner[p];
        auto c = start;
        do {
            const auto patch = source.face_patch[source.face_at[c]];
            if (ring.empty() || ring.back() != patch)
                ring.add(patch);
            c = source.around[c];
        } while (c != start);
        if (ring.size() > 1 && ring.front() == ring.back())
            ring.pop();
        if (ring.size() < 2)
            continue;
        const auto anchor = piece.hull.points[source.local_point[p]];
        bool all_selected = true;
        for (std::size_t i = 0; i < ring.size(); ++i) {
            const auto a = ring[i];
            const auto b = ring[(i + 1) % ring.size()];
            const auto key = pair_key(a, b);
            const auto [selected, inserted] = ridges.try_emplace(key, false);
            if (inserted) {
                const auto& left = piece.supports[a];
                const auto& right = piece.supports[b];
                const double angle = std::atan2(math::length(math::cross(left.normal, right.normal)),
                                                math::dot(left.normal, right.normal));
                *selected = angle + 8 * std::numeric_limits<double>::epsilon() >= threshold;
                if (*selected) {
                    if (angle <= 1e-12 || std::numbers::pi - angle <= 1e-12)
                        return failure("Bevel ridge angle cannot resolve a finite arc.");
                    const auto center = edge_center(left, right, anchor, settings.radius);
                    ATELIER_TRY(edge_cuts(piece, a, b, center, settings));
                } else {
                    piece.contacts.add(key);
                }
            }
            all_selected &= *selected;
        }
        if (ring.size() >= 3 && all_selected)
            corners.add({anchor, ring});
    }
    for (const auto& corner : corners)
        ATELIER_TRY(corner_cuts(piece, corner, settings));
    return {};
}

// Different source triangles can represent the same support with slightly
// different float planes. A cut through their common vertex can then create
// a sub-resolution edge. Contract only actual incident edges inside this
// component; the subsequent feature and shell proofs must still succeed.
Result<void> resolve_short_edges(Piece& piece) {
    auto& hull = piece.hull;
    Array<Index> parent;
    const auto limit = piece.tolerance * piece.tolerance;
    const auto root = [&](Index id) {
        while (parent[id] != id) {
            parent[id] = parent[parent[id]];
            id = parent[id];
        }
        return id;
    };
    for (const auto& face : hull.faces) {
        auto prev = face.points.back();
        for (auto id : face.points) {
            if (math::length_squared(hull.points[id] - hull.points[prev]) <= limit) {
                if (parent.empty()) {
                    parent.resize(hull.points.size());
                    std::iota(parent.begin(), parent.end(), 0u);
                }
                const auto a = root(id), b = root(prev);
                parent[std::max(a, b)] = std::min(a, b);
            }
            prev = id;
        }
    }
    if (parent.empty())
        return {};
    std::size_t count = 0;
    for (auto& face : hull.faces) {
        std::size_t n = 0;
        for (auto id : face.points) {
            const auto rep = root(id);
            if (math::length_squared(hull.points[id] - hull.points[rep]) > limit)
                return failure("Bevel radius leaves a chain of edges unresolved at float precision.");
            if (!n || face.points[n - 1] != rep)
                face.points[n++] = rep;
        }
        if (n > 1 && face.points[n - 1] == face.points.front())
            --n;
        if (n < 3)
            continue;
        face.points.resize(n);
        if (&hull.faces[count] != &face)
            hull.faces[count] = std::move(face);
        ++count;
    }
    hull.faces.resize(count);
    return {};
}

Result<void> cut_piece(Piece& piece) {
    for (const auto& cut : piece.cuts) {
        auto clipped = piece.hull.clip(cut.normal, cut.offset, piece.clip_tolerance, cut.donor, cut.feature);
        if (!clipped) {
            clipped.error().message = "Bevel radius could not preserve the solid at primitive " +
                                      std::to_string(cut.donor) + ": " + clipped.error().message;
            return std::unexpected(std::move(clipped.error()));
        }
    }
    ATELIER_TRY(resolve_short_edges(piece));
    // A cap can still exist after colliding with a remote cut. Require the
    // original face / ridge incidence along every strip and unselected edge.
    Map<std::uint64_t, Index> neighbor;
    Array<std::uint8_t> seen;
    seen.resize(piece.features, 0);
    for (const auto& face : piece.hull.faces) {
        seen[face.feature] = 1;
        auto previous = face.points.back();
        for (auto point : face.points) {
            const auto [other, inserted] = neighbor.try_emplace(pair_key(previous, point), face.feature);
            if (!inserted && *other != face.feature)
                piece.contacts.remove(pair_key(*other, face.feature));
            previous = point;
        }
    }
    const auto lost = std::ranges::find(seen, std::uint8_t{0});
    if (lost != seen.end() || !piece.contacts.empty()) {
        const auto feature = lost != seen.end() ? static_cast<Index>(lost - seen.begin())
                                                : static_cast<Index>(std::ranges::min(piece.contacts) >> 32);
        const auto donor = feature < piece.supports.size()
                               ? piece.supports[feature].donor
                               : std::ranges::find(piece.cuts, feature, &Cut::feature)->donor;
        return failure("Bevel radius collapses a face or ridge near source primitive " + std::to_string(donor) + ".");
    }
    return {};
}

struct Output {
    Array<math::Vector3> positions;
    Array<Index> points;
    Array<Index> offsets{0};
    Array<Index> face_donors;
    Array<std::uint8_t> original;
    Array<Index> point_donors;

    Result<void> face(std::span<const Index> ids, Index donor, bool retained) {
        if (face_donors.size() >= element_limit || points.size() + ids.size() > element_limit)
            return failure("Bevel output exceeds the geometry element limit.");
        for (auto id : ids)
            points.add(id);
        offsets.add(static_cast<Index>(points.size()));
        face_donors.add(donor);
        original.add(retained);
        return {};
    }

    Result<void> append(const Piece& piece) {
        Array<Index> compact;
        compact.resize(piece.hull.points.size(), missing);
        Array<Index> ids;
        Array<math::Vector3> polygon;
        for (const auto& patch : piece.hull.faces) {
            ids.clear();
            polygon.clear();
            for (auto p : patch.points) {
                auto& id = compact[p];
                if (id == missing) {
                    if (positions.size() == element_limit)
                        return failure("Bevel output exceeds the point limit.");
                    id = static_cast<Index>(positions.size());
                    positions.add(math::Vector3(piece.hull.points[p] + piece.origin));
                    point_donors.add(patch.donor);
                } else {
                    point_donors[id] = std::min(point_donors[id], patch.donor);
                }
                ids.add(id);
                polygon.add(positions[id]);
            }
            auto valid = detail::validate_polygon(polygon);
            if (valid) {
                ATELIER_TRY(face(ids, patch.donor, patch.original));
                continue;
            }
            const auto unresolved = [&] {
                return failure("Bevel radius is unresolved at output float precision near source primitive " +
                               std::to_string(patch.donor) + ": " + valid.error().message);
            };
            if (ids.size() == 3)
                return unresolved();
            // A shared intersection on a subdivided edge can round to either
            // side of its exact line. Retain every boundary ID and triangle-fan
            // the certified convex double face around an interior float point.
            // Each triangle must remain resolvable and have the original winding.
            const auto anchor = piece.hull.points[patch.points.front()];
            Vector3d area{}, sum{};
            for (std::size_t i = 0; i < ids.size(); ++i) {
                const auto relative = piece.hull.points[patch.points[i]] - anchor;
                sum += relative;
                area += math::cross(relative, piece.hull.points[patch.points[(i + 1) % ids.size()]] - anchor);
            }
            const auto center = math::Vector3(piece.origin + anchor + sum / static_cast<double>(ids.size()));
            if (positions.size() >= element_limit)
                return failure("Bevel output exceeds the point limit.");
            const auto center_id = static_cast<Index>(positions.size());
            positions.add(center);
            point_donors.add(patch.donor);
            for (std::size_t i = 0; i < ids.size(); ++i) {
                const auto j = (i + 1) % ids.size();
                const std::array triangle{center, polygon[i], polygon[j]};
                if (math::dot(area, math::cross(Vector3d(polygon[i]) - Vector3d(center),
                                                Vector3d(polygon[j]) - Vector3d(center))) <= 0 ||
                    !detail::validate_polygon(triangle))
                    return unresolved();
                ATELIER_TRY(face(std::array{center_id, ids[i], ids[j]}, patch.donor, patch.original));
            }
        }
        // A point on an authored chart has an exact source there. Prefer that
        // chart to projections onto a generated strip's donor. Original faces
        // retain source order; the lowest touching primitive breaks ties.
        for (const auto& patch : std::views::reverse(piece.hull.faces))
            if (patch.original)
                for (auto p : patch.points)
                    point_donors[compact[p]] = patch.donor;
        return {};
    }
};

} // namespace

Result<Geometry> bevel(const Geometry& surface, const BevelSettings& settings) {
    if (!std::isfinite(settings.radius) || settings.radius < 0)
        return failure("Bevel radius must be finite and nonnegative.");
    if (!std::isfinite(settings.angle_degrees) || settings.angle_degrees < 0 || settings.angle_degrees > 180)
        return failure("Bevel angle_degrees must be between 0 and 180.");
    if (settings.segments < 1 || settings.segments > 8)
        return failure("Bevel segments must be between 1 and 8.");
    if (settings.radius == 0)
        return surface;
    if (surface.count(Layer::instance))
        return failure("Bevel requires realized polygon surfaces; realize instances first.");
    if (surface.count(Layer::point) == 0)
        return surface;

    ATELIER_TRY_BIND(source, read_source(surface));
    Output output;
    bool changed = false;
    for (const auto& component : source->components) {
        ATELIER_TRY_BIND(piece, prepare(*source, component));
        ATELIER_TRY(select_cuts(*piece, *source, component, settings));
        if (piece->cuts.empty() && source->components.size() == 1)
            return surface;
        if (!piece->cuts.empty()) {
            changed = true;
            ATELIER_TRY(cut_piece(*piece));
        }
        ATELIER_TRY(output.append(*piece));
    }
    if (!changed)
        return surface;
    ATELIER_TRY_BIND(result, Geometry::polygons(std::move(output.positions), std::move(output.points), output.offsets));
    ATELIER_TRY(detail::validate_shells(*result));
    ATELIER_TRY(detail::bevel_attributes(surface, *result, output.face_donors, output.original, output.point_donors));
    return result;
}

} // namespace atelier::procedural
