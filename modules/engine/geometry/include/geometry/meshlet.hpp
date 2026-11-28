#pragma once

#include "geometry/mesh.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <type_traits>

namespace atelier::geometry {

inline constexpr std::uint32_t meshlet_max_vertices = 64u;

inline constexpr std::uint32_t meshlet_max_triangles = 124u;

inline constexpr std::uint32_t max_mesh_lods = 4u;

// Stream 0: 12B/vertex position stream shared directly with DXR 1.1 BLAS.
struct PackedPosition {
    float x{};

    float y{};

    float z{};
};

// Stream 1: 16B/vertex attribute stream (octahedral normal + octahedral tangent w/ sign + FP32 UV).
struct PackedVertexAttributes {
    std::uint32_t oct_normal{}; // R16G16_SNORM unit octahedral normal

    std::uint32_t oct_tangent_sign{}; // bits 0..15: snorm16 tx, bits 16..30: snorm15 ty, bit 31: handedness < 0

    float u{};

    float v{};
};

// 32B GPU meshlet header (8 DWORDs, 16B-aligned, 2 meshlets per 64B GPU cache line).
struct alignas(16) GpuMeshlet {
    std::uint32_t vertex_offset{}; // Offset in meshlet_vertices (uint32 elements)

    std::uint32_t triangle_offset{}; // Byte offset in meshlet_triangles (4B-aligned uint8 triplets)

    std::uint32_t global_tri_base{}; // Authoritative triangle index in FaceMetaStream / BLASIndexStream

    std::uint32_t
        counts{}; // bits 0..7: vert_count, bits 8..15: tri_count, bits 16..25: oct_cone_axis_s5, bits 26..31: cone_cutoff_u6

    float center[3]{}; // Mesh-local bounding sphere center

    float radius{}; // Mesh-local bounding sphere radius
};

// Stream 4: 4B per-triangle metadata unified across VisBuffer raster and DXR RayQuery hits:
// bit 31: winding_flipped | bit 30: manifold_closed | bits 29..27: edge_mask | bits 26..0: material_slot
inline constexpr std::uint32_t face_meta_winding_bit = 1u << 31u;

inline constexpr std::uint32_t face_meta_manifold_closed_bit = 1u << 30u;

inline constexpr std::uint32_t face_meta_edge_shift = 27u;

inline constexpr std::uint32_t face_meta_material_mask = (1u << 27u) - 1u;

[[nodiscard]] constexpr std::uint32_t pack_face_meta(std::uint32_t material_slot, std::uint8_t edge_mask,
                                                     bool manifold_closed, bool winding_flipped = false) noexcept {
    return (winding_flipped ? face_meta_winding_bit : 0u) | (manifold_closed ? face_meta_manifold_closed_bit : 0u) |
           ((static_cast<std::uint32_t>(edge_mask) & 0x7u) << face_meta_edge_shift) |
           (material_slot & face_meta_material_mask);
}

struct PartRange {
    std::uint32_t material_slot{};

    std::uint32_t global_tri_base{}; // Base into FaceMetaStream & BLASIndexStream (in triangles)

    std::uint32_t triangle_count{};
};

struct MeshLodLevel {
    std::uint32_t first_meshlet{};

    std::uint32_t meshlet_count{};

    std::uint32_t first_part{};

    std::uint32_t part_count{};

    std::uint32_t first_global_tri{};

    std::uint32_t triangle_count{};

    float error{};
};

// Immutable 5-stream GPU-ready Meshlet + discrete LOD (G1) representation of a Mesh.
struct CookedMeshlets {
    Array<PackedPosition> positions;

    Array<PackedVertexAttributes> attributes;

    Array<GpuMeshlet> meshlets;

    Array<std::uint32_t> meshlet_vertices;

    Array<std::uint8_t> meshlet_triangles; // 4B-padded per meshlet

    Array<std::uint32_t> blas_indices; // 3 * total_triangles, exact global_tri_base order

    Array<std::uint32_t> face_meta; // 1 * total_triangles, exact global_tri_base order

    Array<PartRange> lod_parts; // Partitioned per (lod, non-empty material-slot bucket)

    std::array<MeshLodLevel, max_mesh_lods> lods{};

    std::uint32_t lod_count{1u};
};

using CookedMeshletsRef = std::shared_ptr<const CookedMeshlets>;

[[nodiscard]] std::uint32_t pack_oct_normal_snorm16(float nx, float ny, float nz) noexcept;

[[nodiscard]] std::array<float, 3> unpack_oct_normal_snorm16(std::uint32_t packed) noexcept;

[[nodiscard]] std::uint32_t pack_oct_tangent_sign(float tx, float ty, float tz, float sign) noexcept;

// Cooks LOD0..LOD3 meshlets (64v/124t) with locked inter-material boundaries and unified
// raster/ray primitive ordering. Thread-safe and exception-free; CPU OOM triggers atelier::fatal.
[[nodiscard]] CookedMeshletsRef cook_meshlets(const Mesh& mesh);

// Loads validated `.amsh` cooked meshlets from `cache_dir` on digest hit, or cooks and atomically
// persists `.amsh` on cache miss. Empty `cache_dir` bypasses disk I/O.
[[nodiscard]] CookedMeshletsRef load_or_cook_meshlets(const Mesh& mesh, const std::filesystem::path& cache_dir);

static_assert(sizeof(PackedPosition) == 12 && std::is_trivially_copyable_v<PackedPosition>);

static_assert(sizeof(PackedVertexAttributes) == 16 && std::is_trivially_copyable_v<PackedVertexAttributes>);

static_assert(sizeof(GpuMeshlet) == 32 && std::is_trivially_copyable_v<GpuMeshlet>);

} // namespace atelier::geometry
