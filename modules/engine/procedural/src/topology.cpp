#include "detail/topology.hpp"
#include "foundation/bit_vector.hpp"

#include <algorithm>
#include <limits>
#include <numeric>

namespace atelier::procedural::detail {

// Boundary halfedges, not fan diagonals. Across each edge, link corners around
// their common point. A closed manifold has exactly one such cycle at each used point.
Result<void> validate_shells(const Geometry& surface, Array<std::uint32_t>* next_around_point) {
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
            return failure("Closed solid needs exactly two faces per boundary edge.");
        const auto a = edges[i].corner;
        const auto b = edges[i + 1].corner;
        if (points[a] != points[next[b]] || points[b] != points[next[a]])
            return failure("Solid boundary edges must have opposite directions.");
        around[a] = next[b];
        around[b] = next[a];
    }
    BitVector seen_corner(points.size());
    BitVector seen_point(surface.count(Layer::point));
    for (std::uint32_t start = 0; start < points.size(); ++start) {
        if (seen_corner[start])
            continue;
        if (seen_point[points[start]])
            return failure("Closed solid needs a single face fan at each used point.");
        seen_point.set(points[start]);
        auto c = start;
        do {
            seen_corner.set(c);
            c = around[c];
        } while (c != start);
    }
    if (next_around_point)
        *next_around_point = std::move(around);
    return {};
}

Array<std::uint32_t> point_pieces(const Geometry& geometry) {
    Array<std::uint32_t> piece;
    const auto point_count = geometry.count(Layer::point);
    if (!geometry.count(Layer::primitive)) {
        piece.reserve(point_count);
        for (std::uint32_t point = 0; point < point_count; ++point)
            piece.add(point);
        return piece;
    }
    constexpr auto unassigned = std::numeric_limits<std::uint32_t>::max();
    piece.resize(point_count, unassigned);
    Array<std::uint32_t> parent;
    parent.resize(piece.size());
    std::iota(parent.begin(), parent.end(), 0u);
    Array<std::uint8_t> rank;
    rank.resize(piece.size(), 0);
    const auto root_of = [&](std::uint32_t point) {
        while (parent[point] != point) {
            parent[point] = parent[parent[point]];
            point = parent[point];
        }
        return point;
    };
    const auto points = geometry.vertex_points();
    for (std::uint32_t p = 0; p < geometry.count(Layer::primitive); ++p) {
        const auto range = geometry.primitive(p);
        auto root = root_of(points[range.first]);
        for (std::uint32_t i = 1; i < range.count; ++i) {
            auto other = root_of(points[range.first + i]);
            if (root == other)
                continue;
            if (rank[root] < rank[other])
                std::swap(root, other);
            parent[other] = root;
            if (rank[root] == rank[other])
                ++rank[root];
        }
    }

    // Keep the root's assigned ID in the output itself. First occurrence in point order
    // defines the ID, regardless of the representative chosen by union by rank.
    std::uint32_t next_piece = 0;
    for (std::uint32_t point = 0; point < piece.size(); ++point) {
        const auto root = root_of(point);
        if (piece[root] == unassigned)
            piece[root] = next_piece++;
        piece[point] = piece[root];
    }
    return piece;
}

}
