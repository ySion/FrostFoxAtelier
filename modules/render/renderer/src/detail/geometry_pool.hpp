#pragma once

#include "foundation/map.hpp"
#include "geometry/meshlet.hpp"
#include "gpu_resources.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace atelier::renderer::detail {

struct CookedMeshSummary {
    std::uint32_t vertex_count = 0u;

    std::uint32_t material_slot_count = 1u;

    std::uint32_t lod_count = 1u;

    std::array<float, 3> bounds_center{};

    float bounds_radius = 0.0f;

    std::array<geometry::MeshLodLevel, geometry::max_mesh_lods> lods{};

    Array<geometry::PartRange> lod_parts;

    Array<std::uint32_t> lod0_part_meshlets;
};

// Per-mesh GPU entry in the 5-stream GeometryPool (shared by Mesh Shader VisBuffer and DXR 1.1 RayQuery).
// Bulk CPU CookedMeshlets vectors are released immediately after GPU upload; only CookedMeshSummary (~200B) is kept.
struct GpuGeometryEntry {
    std::weak_ptr<const geometry::Mesh> weak_source;

    CookedMeshSummary summary;

    OwnedBuffer positions; // Stream 0: PackedPosition (12B/v, also ACCELERATION_STRUCTURE_BUILD_INPUT)

    OwnedBuffer attributes; // Stream 1: PackedVertexAttributes (16B/v)

    OwnedBuffer meshlets; // Stream 2: GpuMeshlet (32B/cluster)

    OwnedBuffer meshlet_vertices; // Stream 3a: uint32 unique vertex table

    OwnedBuffer meshlet_triangles; // Stream 3b: ByteAddress packed uint8 local triangles

    OwnedBuffer blas_indices; // Stream 3c: uint32x3 authoritative triangle indices (BLAS + ray hit)

    OwnedBuffer face_meta; // Stream 4: uint32 per-triangle metadata (flip | closed | edge | slot)

    // Multi-part LOD0 BLAS variants keyed by arbitrary-length bitsets of non-opaque and fully-discarded material slots.
    struct BlasVariant {
        Array<std::uint64_t> non_opaque_slot_words;

        Array<std::uint64_t> discarded_slot_words;

        OwnedAccelerationStructure blas;

        std::uint32_t part_table_base = 0u;
    };

    Array<BlasVariant> lod0_blas_variants;

    // Optional Zone 2 (>26m) G1 LOD2 purely OPAQUE proxy BLAS for small opaque clutter / trunks.
    OwnedAccelerationStructure proxy_blas;

    std::uint32_t proxy_part_table_base = 0u;

    std::uint32_t mesh_pool_index = 0u;

    std::uint64_t cook_id = 0u;

    std::uint64_t logical_bytes = 0u;

    std::uint64_t last_used_sequence = 0u;

    bool used_this_frame = false;

    void retire_all(rhi::Device& device) noexcept;
};

// GPU-readable descriptor for a registered mesh inside the global GeometryPool (96B, 16B-aligned).
struct alignas(16) GpuMeshDescriptor {
    std::uint32_t positions_srv = 0u;

    std::uint32_t attributes_srv = 0u;

    std::uint32_t meshlets_srv = 0u;

    std::uint32_t meshlet_vertices_srv = 0u;

    std::uint32_t meshlet_triangles_srv = 0u;

    std::uint32_t blas_indices_srv = 0u;

    std::uint32_t face_meta_srv = 0u;

    std::uint32_t lod_count = 1u;

    std::array<std::uint32_t, 4> lod_first_meshlet{};

    std::array<std::uint32_t, 4> lod_meshlet_count{};

    std::array<float, 4> lod_error{};

    std::array<float, 3> bounds_center{};

    float bounds_radius = 0.0f;
};

static_assert(sizeof(GpuMeshDescriptor) == 96);

class GeometryPool {
public:
    void set_cache_directory(std::filesystem::path dir) { cache_dir_ = std::move(dir); }

    void begin_frame() noexcept;

    [[nodiscard]] Result<GpuGeometryEntry*> resolve(rhi::Device& device, const geometry::MeshRef& mesh,
                                                    std::uint64_t current_sequence);

    [[nodiscard]] Result<std::pair<std::uint64_t, std::uint32_t>>
    ensure_blas(rhi::Device& device, GpuGeometryEntry& entry, Span<const std::uint64_t> non_opaque_slot_words,
                Span<const std::uint64_t> discarded_slot_words, bool want_proxy);

    [[nodiscard]] Result<void> commit_frame(rhi::Device& device, std::size_t slot_index);

    void sweep_unused(rhi::Device& device, std::uint64_t current_sequence) noexcept;

    void clear(rhi::Device& device) noexcept;

    [[nodiscard]] std::uint32_t mesh_table_srv() const noexcept { return mesh_table_srv_; }

    [[nodiscard]] std::uint32_t part_tri_base_srv() const noexcept { return part_tri_base_srv_; }

    [[nodiscard]] std::uint64_t logical_bytes() const noexcept { return total_logical_bytes_; }

    [[nodiscard]] std::uint64_t layout_epoch() const noexcept { return layout_epoch_; }

private:
    static GpuMeshDescriptor make_gpu_descriptor(const GpuGeometryEntry& e) noexcept;

    Result<OwnedAccelerationStructure> build_blas_for_lod(rhi::Device& device, const GpuGeometryEntry& entry,
                                                          std::uint32_t lod_idx,
                                                          Span<const std::uint64_t> non_opaque_slot_words,
                                                          Span<const std::uint64_t> discarded_slot_words,
                                                          bool force_all_opaque, std::uint32_t& out_part_base);

    Map<const geometry::Mesh*, std::unique_ptr<GpuGeometryEntry>> entries_;

    std::filesystem::path cache_dir_;

    Array<GpuMeshDescriptor> mesh_descriptors_;

    Array<std::uint32_t> part_tri_bases_;

    HostSlotRing<GpuMeshDescriptor> mesh_table_slots_;

    HostSlotRing<std::uint32_t> part_tri_base_slots_;

    std::uint32_t mesh_table_srv_ = rhi::bindless_invalid_index;

    std::uint32_t part_tri_base_srv_ = rhi::bindless_invalid_index;

    std::uint64_t total_logical_bytes_ = 0u;

    std::uint64_t layout_epoch_ = 1u;

    std::uint64_t next_cook_id_ = 1u;
};

} // namespace atelier::renderer::detail
