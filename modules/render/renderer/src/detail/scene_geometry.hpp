#pragma once
#include "geometry/mesh.hpp"
#include "rhi/rhi.hpp"
#include "result.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace atelier::renderer::detail {
// The one GPU form of an immutable Mesh, shared by every raster pass and ray hit.
// Triangle t keeps the Mesh's order: indices 3t..3t+2 in its corner order and
// faces[t]. Passes address faces by mesh triangle index, a draw's first triangle
// plus SV_PrimitiveID, so a draw may cover any contiguous index range.
struct GpuFace {
    std::array<float, 3> normal; // Geometric normal, oriented by the authored vertex normals.

    std::uint32_t slot_edges; // Material slot in bits 0-28, polygon edge mask in bits 29-31.
};

static_assert(sizeof(GpuFace) == 16 && sizeof(geometry::Vertex) == 48);

inline constexpr std::uint32_t face_slot_bits = 29;

struct SceneGeometry {
    GfxBuffer vertices; // Welded geometry::Vertex, bitwise-identical corners merged.

    GfxBuffer indices; // uint32, three per triangle.

    GfxBuffer faces; // GpuFace per triangle.

    std::uint32_t triangle_count{};

    std::uint64_t generation{}; // Distinct per upload; Scene Lit compares it to rebind its range views.
};

struct WeldedMesh {
    std::vector<geometry::Vertex> vertices;

    std::vector<std::uint32_t> indices;

    std::vector<GpuFace> faces;
};

CancellableResult<WeldedMesh> weld(const geometry::Mesh& mesh, const std::function<bool()>* stop_requested);
}
