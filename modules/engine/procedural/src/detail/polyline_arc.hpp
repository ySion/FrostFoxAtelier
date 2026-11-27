#pragma once

#include "procedural/geometry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace atelier::procedural::detail {

struct ArcEdge {
    std::uint32_t vertex;
    double end;
};

struct ArcStation {
    std::uint32_t a, b; // Source vertex indices, including independent vertex values on a seam.
    double t;
};

struct PolylineArc {
    Span<const math::Vector3> positions;
    Span<const std::uint32_t> points;
    PrimitiveRange primitive;
    Span<const ArcEdge> edges;

    PolylineArc(const Geometry& source, PrimitiveRange primitive, Span<const ArcEdge> edges)
        : positions(source.positions()), points(source.vertex_points()), primitive(primitive), edges(edges) {}

    double length() const { return edges.back().end; }
    double start(std::size_t i) const { return i ? edges[i - 1].end : 0.; }
    bool closed() const { return points[primitive.first] == points[primitive.first + primitive.count - 1]; }
    math::Vector3 position(ArcStation s) const {
        const auto& a = positions[points[s.a]];
        if (s.t == 0.)
            return a;
        const auto& b = positions[points[s.b]];
        if (s.t == 1.)
            return b;
        math::Vector3 p;
        for (std::size_t axis = 0; axis < 3; ++axis)
            p[axis] = static_cast<float>(std::lerp(double(a[axis]), double(b[axis]), s.t));
        return p;
    }
    math::Vector3d delta(std::size_t i) const {
        const auto v = edges[i].vertex;
        return math::Vector3d(positions[points[v + 1]]) - math::Vector3d(positions[points[v]]);
    }
    math::Vector3 tangent(std::size_t i) const { return math::Vector3(*math::try_normalize(delta(i))); }
    std::size_t edge_at(double d) const {
        const auto it = std::ranges::upper_bound(edges, d, {}, &ArcEdge::end);
        return it == edges.end() ? edges.size() - 1 : static_cast<std::size_t>(it - edges.begin());
    }
    ArcStation corner_after(std::size_t i) const {
        const auto v = i + 1 == edges.size() ? primitive.first + primitive.count - 1 : edges[i + 1].vertex;
        return {v, v, 0.};
    }
    ArcStation station(std::size_t i, double d) const {
        if (d == 0.)
            return {primitive.first, primitive.first, 0.};
        if (d == edges[i].end)
            return corner_after(i);
        const auto v = edges[i].vertex;
        const auto begin = start(i);
        return {v, v + 1, (d - begin) / (edges[i].end - begin)};
    }
    // The published float arclength column must also address the exact end of its source.
    double distance(float metres) const { return metres == static_cast<float>(length()) ? length() : double(metres); }
};

// All primitives share one edge allocation. No output-sized allocation is made while measuring.
struct PolylineArcs {
    const Geometry& source;
    Array<ArcEdge> edges{};
    Array<std::size_t> offsets{0};

    Result<void> measure();
    PolylineArc operator[](std::uint32_t p) const {
        return {source, source.primitive(p), edges.view().slice(offsets[p], offsets[p + 1] - offsets[p])};
    }
};

} // namespace atelier::procedural::detail
