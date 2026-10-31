#include "geometry/mesh.hpp"
#include "mikktspace.h"

namespace atelier::geometry {
namespace {
void fallback_tangent(Vertex& vertex) {
    const auto* n = vertex.normal;
    const std::array<double, 3> t =
        std::abs(n[1]) < .99f ? std::array<double, 3>{n[2], 0, -n[0]} : std::array<double, 3>{0, -n[2], n[1]};
    const auto length = std::hypot(t[0], t[1], t[2]);
    for (std::size_t i = 0; i < 3; ++i)
        vertex.tangent[i] = static_cast<float>(t[i] / length);
}
}
void adopt_tangents(std::span<Vertex> vertices) {
    for (auto& vertex : vertices) {
        auto& t = vertex.tangent;
        if (!std::isfinite(t[0]) || !std::isfinite(t[1]) || !std::isfinite(t[2]) || (t[3] != 1 && t[3] != -1))
            throw std::invalid_argument("Authored mesh tangent requires finite XYZ and a handedness of 1 or -1.");
        const auto* n = vertex.normal;
        // Float normals are only nearly unit; without the division a parallel tangent leaves a residual above the fallback threshold.
        const double projection = (double(t[0]) * n[0] + double(t[1]) * n[1] + double(t[2]) * n[2]) /
                                  (double(n[0]) * n[0] + double(n[1]) * n[1] + double(n[2]) * n[2]);
        const std::array<double, 3> orthogonal{t[0] - projection * n[0], t[1] - projection * n[1],
                                               t[2] - projection * n[2]};
        const double length = std::hypot(orthogonal[0], orthogonal[1], orthogonal[2]);
        // Relative to |t|: float rounding of a parallel tangent leaves a residual proportional to its length.
        if (!(length > 1e-6 * std::hypot(double(t[0]), double(t[1]), double(t[2])))) {
            fallback_tangent(vertex);
            continue;
        }
        for (std::size_t i = 0; i < 3; ++i)
            t[i] = static_cast<float>(orthogonal[i] / length);
    }
}
void generate_tangents(std::span<Vertex> vertices) {
    // Fallback also covers isolated degenerate UVs. Never change authored normals.
    for (auto& vertex : vertices) {
        fallback_tangent(vertex);
        vertex.tangent[3] = 1;
    }
    SMikkTSpaceInterface callbacks{};
    callbacks.m_getNumFaces = [](const SMikkTSpaceContext* c) {
        return static_cast<int>(static_cast<std::span<Vertex>*>(c->m_pUserData)->size() / 3);
    };
    callbacks.m_getNumVerticesOfFace = [](const SMikkTSpaceContext*, int) { return 3; };
    callbacks.m_getPosition = [](const SMikkTSpaceContext* c, float* out, int face, int corner) {
        std::copy_n((*static_cast<std::span<Vertex>*>(c->m_pUserData))[std::size_t(face) * 3 + corner].position, 3,
                    out);
    };
    callbacks.m_getNormal = [](const SMikkTSpaceContext* c, float* out, int face, int corner) {
        std::copy_n((*static_cast<std::span<Vertex>*>(c->m_pUserData))[std::size_t(face) * 3 + corner].normal, 3, out);
    };
    callbacks.m_getTexCoord = [](const SMikkTSpaceContext* c, float* out, int face, int corner) {
        std::copy_n((*static_cast<std::span<Vertex>*>(c->m_pUserData))[std::size_t(face) * 3 + corner].uv, 2, out);
    };
    callbacks.m_setTSpaceBasic = [](const SMikkTSpaceContext* c, const float* tangent, float sign, int face,
                                    int corner) {
        auto& vertex = (*static_cast<std::span<Vertex>*>(c->m_pUserData))[std::size_t(face) * 3 + corner];
        const auto* n = vertex.normal;
        const double projection = (double(tangent[0]) * n[0] + double(tangent[1]) * n[1] + double(tangent[2]) * n[2]) /
                                  (double(n[0]) * n[0] + double(n[1]) * n[1] + double(n[2]) * n[2]);
        std::array<double, 3> orthogonal{};
        for (std::size_t i = 0; i < 3; ++i)
            orthogonal[i] = tangent[i] - projection * n[i];
        const double length = std::hypot(orthogonal[0], orthogonal[1], orthogonal[2]);
        // Mikk emits +X for isolated degenerate UVs; retain our fallback if that
        // direction is parallel to the authored normal.
        if (!std::isfinite(length) || length < 1e-8)
            return;
        for (std::size_t i = 0; i < 3; ++i)
            vertex.tangent[i] = static_cast<float>(orthogonal[i] / length);
        vertex.tangent[3] = sign < 0 ? -1.f : 1.f;
    };
    SMikkTSpaceContext context{&callbacks, &vertices};
    if (!genTangSpaceDefault(&context))
        throw std::runtime_error("Cannot generate mesh tangent space.");
}
}
