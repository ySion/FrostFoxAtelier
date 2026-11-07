#include "procedural/operations.hpp"
#include "foundation/bit_vector.hpp"
#include "foundation/result.hpp"
#include "foundation/tasks.hpp"
#include "procedural/bvh.hpp"
#include "procedural/random.hpp"
#include "procedural/timing.hpp"
#include "detail/triangles.hpp"

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

constexpr std::size_t block_size = 256;

enum class Classification : std::uint8_t { outside, surface, retained };

struct Block {
    std::uint32_t retained{};

    std::array<std::uint32_t, 8> attempts{};

    std::array<std::uint32_t, 8> retries{};

    std::uint32_t fallback{};
};

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
    Array<Edge> edges;
    edges.reserve(points.size());
    Array<std::uint32_t> next;
    next.resize(points.size());
    for (std::uint32_t p = 0; p < surface.count(Layer::primitive); ++p) {
        const auto face = surface.primitive(p);
        for (std::uint32_t i = 0; i < face.count; ++i) {
            const auto c = face.first + i;
            next[c] = face.first + (i + 1 == face.count ? 0 : i + 1);
            const auto a = points[c];
            const auto b = points[next[c]];
            edges.add({(std::uint64_t{std::min(a, b)} << 32) | std::max(a, b), c});
        }
    }
    std::ranges::sort(edges, {}, &Edge::key);
    Array<std::uint32_t> around;
    around.resize(points.size());
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
    BitVector seen_corner(points.size());
    BitVector seen_point(surface.count(Layer::point));
    for (std::uint32_t start = 0; start < points.size(); ++start) {
        if (seen_corner[start])
            continue;
        if (seen_point[points[start]])
            return failure("Volume needs a single face fan at each used point.");
        seen_point.set(points[start]);
        auto c = start;
        do {
            seen_corner.set(c);
            c = around[c];
        } while (c != start);
    }
    return {};
}

struct Triangle {
    std::array<Vector3d, 3> points;
};

Result<Array<Triangle>> prepare(const Geometry& surface) {
    if (!surface.count(Layer::primitive) || surface.count(Layer::instance))
        return failure("Volume requires polygon shells without instances.");
    for (const auto p : surface.positions())
        if (!math::is_finite(p))
            return failure("Volume positions must be finite.");
    ATELIER_TRY_BIND(view, detail::TriangleView::create(surface, true));
    ATELIER_TRY(validate_shells(surface));
    Array<Triangle> triangles;
    triangles.reserve(view->size());
    const auto origin = widen(surface.positions()[surface.vertex_points()[0]]);
    Sum volume;
    for (const auto fan : *view) {
        Triangle t{};
        for (std::size_t i = 0; i < 3; ++i)
            t.points[i] = widen(surface.positions()[surface.vertex_points()[fan.corners[i]]]);
        const auto a = t.points[0];
        const auto b = t.points[1];
        const auto c = t.points[2];
        const auto cross = math::cross(b - a, c - a);
        const double area = math::length(cross);
        if (!area) // A valid polygon may contain forward collinear corners.
            continue;
        volume.add(math::dot(a - origin, math::cross(b - origin, c - origin)));
        triangles.add(t);
    }
    if (!(volume.value > 0))
        return failure("Volume needs positive aggregate signed volume; inward cavities are allowed.");
    return triangles;
}

// Original fixed polygon/fan-order compensated sum, used only after eight uncertain rays.
bool winding_inside(std::span<const Triangle> triangles, Vector3 position) {
    const auto q = widen(position);
    Sum angles;
    for (const auto& t : triangles) {
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

Classification classify(const Bvh& bvh, std::span<const Triangle> triangles, Vector3 position, Block& block) {
    if (bvh.on_surface_band(position))
        return Classification::surface;
    // Fixed binary approximations of sqrt(2), sqrt(3), sqrt(5), widened exactly
    // for double predicates. Float-representable directions also let public Geometry
    // construct genuine all-direction degeneracies without a private test switch.
    constexpr float r2 = 0x1.6a09e6p0f;
    constexpr float r3 = 0x1.bb67aep0f;
    constexpr float r5 = 0x1.1e377ap1f;
    constexpr std::array<Vector3, 8> directions{{{1, r2, r3},
                                                 {-r2, 1, r5},
                                                 {r3, -r5, 1},
                                                 {r5, r3, -1},
                                                 {-1, -r3, r2},
                                                 {r2, -1, -r5},
                                                 {-r3, r5, -r2},
                                                 {-r5, -r2, -r3}}};
    for (std::size_t i = 0; i < directions.size(); ++i) {
        ++block.attempts[i];
        if (const auto crossings = bvh.signed_crossings(position, directions[i]))
            return *crossings >= 1 ? Classification::retained : Classification::outside;
        ++block.retries[i];
    }
    ++block.fallback;
    return winding_inside(triangles, position) ? Classification::retained : Classification::outside;
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
    const auto coordinate = [first, widths](std::uint32_t i) {
        return std::array<std::int32_t, 3>{static_cast<std::int32_t>(first[0] + i / (widths[1] * widths[2])),
                                           static_cast<std::int32_t>(first[1] + i / widths[2] % widths[1]),
                                           static_cast<std::int32_t>(first[2] + i % widths[2])};
    };
    const auto cell_seed = [seed = settings.seed](const auto& xyz) {
        return hash(hash(hash(seed, static_cast<std::uint32_t>(xyz[0])), static_cast<std::uint32_t>(xyz[1])),
                    static_cast<std::uint32_t>(xyz[2]));
    };
    const double expected = settings.density * h * h * h;
    if (!std::isfinite(expected) || std::floor(expected) > work_limit / cells)
        return failure("Volume exceeds 1,048,576 candidates before filtering.");
    const auto whole = static_cast<std::uint32_t>(std::floor(expected));
    // Keep zero-count cells in the prefix. A dense single cell may span many blocks.
    Array<std::uint32_t> offsets;
    offsets.resize(static_cast<std::size_t>(cells) + 1);
    for (std::uint32_t i = 0; i < cells; ++i) {
        const auto count = whole + (unit(hash(cell_seed(coordinate(i)), 0)) < expected - whole);
        if (count > work_limit - offsets[i])
            return failure("Volume exceeds 1,048,576 candidates before filtering.");
        offsets[i + 1] = offsets[i] + count;
    }
    const auto candidates = offsets.back();
    Bvh bvh;
    if (candidates) {
        timing::Scope building{phases::build};
        ATELIER_TRY_BIND(built, Bvh::build(surface));
        bvh = std::move(*built);
    }
    Array<Vector3> candidate_positions;
    candidate_positions.resize(candidates);
    Array<Classification> states;
    states.resize(candidates);
    Array<Block> blocks;
    blocks.resize((candidates + block_size - 1) / block_size);
    {
        timing::Scope classification{phases::classify};
        const auto classified = tasks::parallel_for(
            {0, blocks.size()},
            [&bvh, triangles = std::span<const Triangle>{*triangles}, coordinate, cell_seed, origin = settings.origin,
             h, candidates, offsets = std::span<const std::uint32_t>{offsets}, positions = candidate_positions.view(),
             states = std::span{states},
             blocks = std::span{blocks}](tasks::Range range) noexcept -> std::expected<void, tasks::Failure> {
                // A scheduler body may contain several fixed-size logical blocks.
                for (auto b = range.begin; b < range.end; ++b) {
                    Block result;
                    auto i = b * block_size;
                    const auto end = std::min(i + block_size, static_cast<std::size_t>(candidates));
                    auto cell = static_cast<std::uint32_t>(std::upper_bound(offsets.begin(), offsets.end(), i) -
                                                           offsets.begin() - 1);
                    while (i < end) {
                        while (offsets[cell + 1] == i)
                            ++cell;
                        const auto xyz = coordinate(cell);
                        const auto seed = cell_seed(xyz);
                        const auto stop = std::min(end, static_cast<std::size_t>(offsets[cell + 1]));
                        for (; i < stop; ++i) {
                            const auto k = static_cast<std::uint32_t>(i - offsets[cell]);
                            const auto id = hash(seed, k);
                            Vector3 p;
                            for (std::uint32_t axis = 0; axis < 3; ++axis)
                                p[axis] = static_cast<float>(
                                    origin[axis] + (static_cast<double>(xyz[axis]) + unit(hash(id, axis + 1))) * h);
                            if (!math::is_finite(p))
                                return std::unexpected(tasks::Failure{1, i}); // Unrepresentable candidate position.
                            positions[i] = p;
                            states[i] = classify(bvh, triangles, p, result);
                            result.retained += states[i] == Classification::retained;
                        }
                        ++cell;
                    }
                    blocks[b] = result;
                }
                return {};
            });
        if (!classified)
            return failure("Volume candidate position exceeds float range.");
        if (classified != tasks::Completion::completed)
            return failure("Volume was cancelled before all candidates were classified.");
        Block totals;
        for (const auto& block : blocks) {
            for (std::size_t i = 0; i < totals.attempts.size(); ++i) {
                totals.attempts[i] += block.attempts[i];
                totals.retries[i] += block.retries[i];
            }
            totals.fallback += block.fallback;
        }
        // No worker writes the Recorder. Each event count is bounded by candidates.
        if (candidates)
            timing::record(phases::candidate, {}, candidates);
        for (std::size_t i = 0; i < totals.attempts.size(); ++i) {
            if (totals.attempts[i])
                timing::record(phases::attempts[i], {}, totals.attempts[i]);
            if (totals.retries[i])
                timing::record(phases::retries[i], {}, totals.retries[i]);
        }
        if (totals.fallback)
            timing::record(phases::fallback, {}, totals.fallback);
    }
    Array<std::uint32_t> output_offsets;
    output_offsets.resize(blocks.size() + 1);
    for (std::size_t b = 0; b < blocks.size(); ++b)
        output_offsets[b + 1] = output_offsets[b] + blocks[b].retained;
    const auto retained = output_offsets.back();
    Array<Vector3> positions;
    Array<std::uint32_t> ids;
    Array<std::uint32_t> ordinals;
    std::array<Array<std::int32_t>, 3> sources;
    positions.resize(retained);
    ids.resize(retained);
    ordinals.resize(retained);
    for (auto& source : sources)
        source.resize(retained);
    const auto filled = tasks::parallel_for(
        {0, blocks.size()},
        [coordinate, cell_seed, candidates, offsets = std::span<const std::uint32_t>{offsets},
         output_offsets = std::span<const std::uint32_t>{output_offsets},
         candidate_positions = Span<const Vector3>{candidate_positions.view()},
         states = std::span<const Classification>{states}, positions = positions.view(), ids = ids.view(),
         ordinals = ordinals.view(), sources = std::array{sources[0].view(), sources[1].view(), sources[2].view()}](
            tasks::Range range) noexcept -> std::expected<void, tasks::Failure> {
            for (auto b = range.begin; b < range.end; ++b) {
                if (output_offsets[b] == output_offsets[b + 1])
                    continue;
                auto output = output_offsets[b];
                auto i = b * block_size;
                const auto end = std::min(i + block_size, static_cast<std::size_t>(candidates));
                auto cell = static_cast<std::uint32_t>(std::upper_bound(offsets.begin(), offsets.end(), i) -
                                                       offsets.begin() - 1);
                while (i < end) {
                    while (offsets[cell + 1] == i)
                        ++cell;
                    const auto xyz = coordinate(cell);
                    const auto seed = cell_seed(xyz);
                    const auto stop = std::min(end, static_cast<std::size_t>(offsets[cell + 1]));
                    for (; i < stop; ++i) {
                        if (states[i] != Classification::retained)
                            continue;
                        const auto k = static_cast<std::uint32_t>(i - offsets[cell]);
                        positions[output] = candidate_positions[i];
                        ids[output] = hash(seed, k);
                        ordinals[output] = k;
                        for (std::size_t axis = 0; axis < 3; ++axis)
                            sources[axis][output] = xyz[axis];
                        ++output;
                    }
                    ++cell;
                }
            }
            return {};
        });
    if (filled != tasks::Completion::completed)
        return failure("Volume was cancelled before all output columns were filled.");
    ATELIER_TRY_BIND(result, Geometry::points(std::move(positions)));
    ATELIER_TRY(result->set(String::literal<"id">(), Layer::point, SharedArray<std::uint32_t>{std::move(ids)}));
    const String names[]{String::literal<"sourcecellx">(), String::literal<"sourcecelly">(),
                         String::literal<"sourcecellz">()};
    for (std::size_t axis = 0; axis < 3; ++axis)
        ATELIER_TRY(result->set(names[axis], Layer::point, SharedArray<std::int32_t>{std::move(sources[axis])}));
    ATELIER_TRY(
        result->set(String::literal<"sourceordinal">(), Layer::point, SharedArray<std::uint32_t>{std::move(ordinals)}));
    return result;
}

}
