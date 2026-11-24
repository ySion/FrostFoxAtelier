#include "detail/bevel_offset.hpp"
#include "foundation/map.hpp"
#include "foundation/result.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace atelier::procedural::detail {
namespace {

using math::Vector3d;
using Index = std::uint32_t;
using Mask = std::uint64_t;
using Triple = std::array<Index, 3>;

constexpr double roundoff = 32 * std::numeric_limits<double>::epsilon();
constexpr Index max_split_faces = std::numeric_limits<Mask>::digits;

double independence(Vector3d a, Vector3d b, Vector3d c) {
    return std::abs(math::dot(a, math::cross(b - a, c - a)));
}

Triple starting_triple(std::span<const Vector3d> normals) {
    Triple basis{0, 1, 2};
    if (normals.size() == 3)
        return basis;
    double best = 0;
    if (normals.size() <= 8) {
        for (Index a = 0; a + 2 < normals.size(); ++a)
            for (Index b = a + 1; b + 1 < normals.size(); ++b)
                for (Index c = b + 1; c < normals.size(); ++c) {
                    const double score = independence(normals[a], normals[b], normals[c]);
                    if (score > best) {
                        best = score;
                        basis = {a, b, c};
                    }
                }
        return basis;
    }

    // Many-sided regular finials normally need only one center. Find a
    // rank-revealing basis with linear work before attempting any subdivision.
    for (Index i = 1; i < normals.size(); ++i) {
        const double score = math::length_squared(math::cross(normals[0], normals[i]));
        if (score > best) {
            best = score;
            basis[1] = i;
        }
    }
    for (Index slot : {2u, 0u, 1u, 2u}) {
        const auto a = normals[basis[(slot + 1) % 3]];
        const auto b = normals[basis[(slot + 2) % 3]];
        best = 0;
        for (Index i = 0; i < normals.size(); ++i) {
            const double score = independence(a, b, normals[i]);
            if (score > best) {
                best = score;
                basis[slot] = i;
            }
        }
    }
    return basis;
}

struct Intersection {
    Vector3d center;
    double strength{};
};

Intersection intersect(std::span<const Vector3d> normals, std::span<const double> offsets, Triple t) {
    const auto a = normals[t[0]];
    const auto ba = normals[t[1]] - a;
    const auto ca = normals[t[2]] - a;
    const auto x = math::cross(ba, ca);
    const auto y = math::cross(ca, a);
    const auto z = math::cross(a, ba);
    const double det = math::dot(a, x);
    const double strength = std::abs(det);
    if (!(strength > roundoff * std::sqrt(std::max({math::length_squared(x), math::length_squared(y),
                                                    math::length_squared(z)}))))
        return {};

    // Subtract the first equation before solving. The common radius cancels
    // exactly in both differences, including for nearly parallel supports.
    const auto h = offsets[t[0]];
    const auto center = (x * h + y * (offsets[t[1]] - h) + z * (offsets[t[2]] - h)) / det;
    return math::is_finite(center) ? Intersection{center, strength} : Intersection{};
}

double plane_tolerance(double tolerance, Vector3d point) {
    return tolerance + roundoff * (std::abs(point.x) + std::abs(point.y) + std::abs(point.z));
}

Mask bit(Index face) { return Mask{1} << face; }

struct Candidate {
    Mask active;
    Intersection intersection;
};

// The cells subdivide the convex spherical polygon of the original normals.
// Every rim arc occurs once and each interior diagonal twice in opposite
// directions. This rejects crossing, missing and overlapping cones at a tie.
Result<Array<BevelOffsetCell>> ordered_fan(const Array<Candidate>& candidates, Index count) {
    std::array<Mask, max_split_faces> outgoing{};
    Array<BevelOffsetCell> cells;
    for (const auto& candidate : candidates) {
        // Three independent shared supports determine only one vertex. At the
        // tolerance boundary one triple can see a subset of the full incidence.
        if (std::ranges::any_of(candidates, [&](const Candidate& other) {
                return candidate.active != other.active && (candidate.active & other.active) == candidate.active;
            }))
            continue;
        BevelOffsetCell cell{candidate.intersection.center, {}};
        for (Index i = 0; i < count; ++i)
            if (candidate.active & bit(i))
                cell.faces.add(i);
        auto from = cell.faces.back();
        for (auto to : cell.faces) {
            if (outgoing[from] & bit(to))
                return failure("Bevel corner offset has overlapping normal cones at this precision.");
            outgoing[from] |= bit(to);
            from = to;
        }
        cells.add(std::move(cell));
    }
    for (Index a = 0; a < count; ++a) {
        const auto next = (a + 1) % count;
        if (!(outgoing[a] & bit(next)) || (outgoing[next] & bit(a)))
            return failure("Bevel corner offset lost an incident ridge at this precision.");
        for (Index b = a + 1; b < count; ++b) {
            if (b == next || (b + 1) % count == a)
                continue;
            if (bool(outgoing[a] & bit(b)) != bool(outgoing[b] & bit(a)))
                return failure("Bevel corner offset has an unresolved interior ridge.");
        }
    }
    return cells;
}

} // namespace

Result<Array<BevelOffsetCell>> bevel_offset_cells(std::span<const Vector3d> normals, std::span<const double> offsets,
                                                 double tolerance) {
    if (normals.size() < 3 || normals.size() != offsets.size())
        return failure("Bevel corner needs an offset distance for each of its incident supports.");
    const auto seed = intersect(normals, offsets, starting_triple(normals));
    if (seed.strength == 0)
        return failure("Bevel corner support planes do not resolve an offset center.");
    const auto count = static_cast<Index>(normals.size());
    const double margin = plane_tolerance(tolerance, seed.center);
    Index face = 0;
    while (face < count && std::abs(math::dot(normals[face], seed.center) - offsets[face]) <= margin)
        ++face;
    if (face == count) {
        BevelOffsetCell cell{seed.center, {}};
        cell.faces.reserve(count);
        for (Index i = 0; i < count; ++i)
            cell.faces.add(i);
        Array<BevelOffsetCell> cells;
        cells.add(std::move(cell));
        return cells;
    }

    // Only split corners enumerate triples: at 64 supports there are 41,664
    // candidates, each tested against at most 64 half-spaces before publication.
    if (count > max_split_faces)
        return failure("Bevel nonconcurrent corner exceeds its local budget of 64 offset supports.");
    Array<Candidate> candidates;
    Map<Mask, Index> by_active;
    for (Index a = 0; a + 2 < count; ++a)
        for (Index b = a + 1; b + 1 < count; ++b)
            for (Index c = b + 1; c < count; ++c) {
                const auto hit = intersect(normals, offsets, {a, b, c});
                if (hit.strength == 0)
                    continue;
                const auto distance = plane_tolerance(tolerance, hit.center);
                Mask active = 0;
                Index i = 0;
                for (; i < count; ++i) {
                    const double residual = math::dot(normals[i], hit.center) - offsets[i];
                    if (residual > distance)
                        break;
                    if (residual >= -distance)
                        active |= bit(i);
                }
                const auto triple = bit(a) | bit(b) | bit(c);
                if (i != count || (active & triple) != triple)
                    continue;
                const auto [entry, inserted] = by_active.try_emplace(active, static_cast<Index>(candidates.size()));
                if (inserted) {
                    // A resolved n-sided cone has at most n-2 cells. Bound the
                    // extra possible near-tie subsets admitted by the tolerance.
                    if (candidates.size() == 4u * count)
                        return failure("Bevel corner offset support sets are ambiguous at this precision.");
                    candidates.add({active, hit});
                } else if (hit.strength > candidates[*entry].intersection.strength) {
                    candidates[*entry].intersection = hit;
                }
            }
    if (candidates.empty())
        return failure("Bevel corner inward-offset supports have no resolvable feasible vertex.");
    return ordered_fan(candidates, count);
}

}
