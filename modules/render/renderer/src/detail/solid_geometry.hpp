#pragma once
#include "foundation/map.hpp"
#include "geometry/mesh.hpp"

namespace atelier::renderer::detail {
struct TriangleVolume {
    std::uint32_t closed{}; // Zero or connected closed-component root + 1.
};

// Derived once per cached immutable Mesh, exclusively for DDGI's inside test.
// Exact position adjacency bridges normal/UV seams; it never changes polygon
// wire masks or welds nearby vertices. Open, ambiguous and nonmanifold connected
// components remain opaque surfaces but provide no evidence of a solid interior.
inline std::vector<TriangleVolume> triangle_volumes(const geometry::Mesh& mesh) {
    using Position = std::array<float, 3>;
    using Vector = std::array<double, 3>;
    struct Edge {
        std::uint32_t triangle{};

        std::uint32_t count{};

        std::int64_t direction{};
    };
    const auto vertices = mesh.vertices();
    const auto count = vertices.size() / 3;
    std::vector<TriangleVolume> result(count);
    std::vector<std::uint32_t> parent(count);
    std::vector<std::uint32_t> sizes(count, 1);
    std::vector<int> orientation(count, 1);
    std::vector<bool> eligible(count, true);
    for (std::uint32_t i = 0; i < count; ++i)
        parent[i] = i;
    const auto root = [&](std::uint32_t i) {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    };
    const auto connect = [&](std::uint32_t a, std::uint32_t b) {
        a = root(a);
        b = root(b);
        if (a == b)
            return;
        if (sizes[a] < sizes[b])
            std::swap(a, b);
        parent[b] = a;
        sizes[a] += sizes[b];
    };
    const auto difference = [](const float* a, const float* b) {
        return Vector{double(a[0]) - b[0], double(a[1]) - b[1], double(a[2]) - b[2]};
    };
    const auto cross = [](Vector a, Vector b) {
        return Vector{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    };
    const auto dot = [](Vector a, Vector b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    Map<Position, std::uint32_t> positions;
    Map<std::uint64_t, Edge> edges;
    positions.reserve(vertices.size());
    edges.reserve(vertices.size());
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto* v = vertices.data() + i * 3;
        const auto n = cross(difference(v[1].position, v[0].position), difference(v[2].position, v[0].position));
        const Vector authored{double(v[0].normal[0]) + v[1].normal[0] + v[2].normal[0],
                              double(v[0].normal[1]) + v[1].normal[1] + v[2].normal[1],
                              double(v[0].normal[2]) + v[1].normal[2] + v[2].normal[2]};
        const double alignment = dot(n, authored);
        const double length = std::hypot(n[0], n[1], n[2]);
        eligible[i] = length > 0 && alignment != 0;
        orientation[i] = alignment < 0 ? -1 : 1;
        std::array<std::uint32_t, 3> ids;
        for (std::size_t c = 0; c < 3; ++c) {
            Position p{v[c].position[0], v[c].position[1], v[c].position[2]};
            ids[c] = *positions.try_emplace(p, static_cast<std::uint32_t>(positions.size())).value;
        }
        for (std::size_t c = 0; c < 3; ++c) {
            const auto a = ids[c];
            const auto b = ids[(c + 1) % 3];
            if (a == b)
                eligible[i] = false;
            const auto key = (std::uint64_t(std::min(a, b)) << 32) | std::max(a, b);
            auto& edge = *edges.try_emplace(key, Edge{i}).value;
            // Join every incident face, including a third nonmanifold face.
            connect(i, edge.triangle);
            ++edge.count;
            edge.direction += (a < b ? 1 : -1) * orientation[i];
        }
    }
    std::vector<bool> closed(count, true);
    for (std::uint32_t i = 0; i < count; ++i)
        if (!eligible[i])
            closed[root(i)] = false;
    for (const auto& [key, edge] : edges) {
        (void)key;
        if (edge.count != 2 || edge.direction != 0)
            closed[root(edge.triangle)] = false;
    }
    std::vector<double> volume(count);
    std::vector<double> absolute_volume(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto component = root(i);
        if (!closed[component])
            continue;
        const auto* v = vertices.data() + i * 3;
        const auto* origin = vertices[component * 3].position;
        const double part = dot(difference(v[0].position, origin),
                                cross(difference(v[1].position, origin), difference(v[2].position, origin))) *
                            orientation[i];
        volume[component] += part;
        absolute_volume[component] += std::abs(part);
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto component = root(i);
        // A zero-volume doubled sheet is not a solid. Do not force positive
        // volume: authored inner-shell normals may intentionally enclose a void.
        result[i].closed =
            closed[component] && std::abs(volume[component]) >
                                     64 * std::numeric_limits<double>::epsilon() * absolute_volume[component]
                ? component + 1
                : 0;
    }
    return result;
}
}
