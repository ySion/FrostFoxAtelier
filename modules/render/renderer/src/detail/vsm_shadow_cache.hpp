#pragma once

#include "gpu_resources.hpp"
#include "render_scene.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace atelier::renderer::detail {

inline constexpr std::uint32_t vsm_pool_dim = 4096u;

inline constexpr std::uint32_t vsm_page_dim = 128u;

inline constexpr std::uint32_t vsm_pages_per_axis = vsm_pool_dim / vsm_page_dim; // 32

inline constexpr std::uint32_t vsm_static_page_capacity = vsm_pages_per_axis * vsm_pages_per_axis; // 1024 (64 MiB)

inline constexpr std::uint32_t vsm_dynamic_page_capacity = vsm_pages_per_axis * vsm_pages_per_axis; // 1024 (64 MiB)

inline constexpr std::uint32_t vsm_cascade_count = 4u;

inline constexpr std::uint32_t vsm_cascade_grid = 16u;

inline constexpr std::uint32_t vsm_dir_virtual_pages = vsm_cascade_count * vsm_cascade_grid * vsm_cascade_grid; // 1024

inline constexpr std::uint32_t vsm_max_views = 64u;

inline constexpr std::uint32_t vsm_local_view_cap = vsm_max_views - vsm_cascade_count; // 60

inline constexpr std::uint32_t vsm_local_grid = 2u;

inline constexpr std::uint32_t vsm_virtual_page_total =
    vsm_dir_virtual_pages + vsm_local_view_cap * vsm_local_grid * vsm_local_grid; // 1264 (padded to 1280)

inline constexpr std::uint32_t vsm_page_table_size = 1280u;

inline constexpr std::uint32_t vsm_page_unmapped = 0xFFFFFFFFu;

inline constexpr std::uint32_t vsm_page_empty_lit = 0xFFFFFFFEu;

inline constexpr std::uint32_t vsm_job_ortho_pancake = 1u << 0u;

struct alignas(16) GpuLightRecord {
    std::array<float, 3> position{};

    std::uint32_t kind_and_flags = 0u;

    std::array<float, 3> direction{};

    float range = 10.0f;

    std::array<float, 3> color_intensity{};

    float source_radius = 0.0f;

    std::array<float, 3> right{};

    float width = 1.0f;

    std::array<float, 3> up{};

    float height = 1.0f;
};

static_assert(sizeof(GpuLightRecord) == 80);

struct alignas(16) GpuVsmLightView {
    std::array<float, 16> view_proj{};

    std::array<float, 3> light_pos_or_dir{};

    float near_or_zmin = 0.0f;

    std::array<float, 3> right_axis{};

    float far_or_zspan = 1.0f;

    std::array<float, 3> up_axis{};

    float world_span_or_tan_half = 1.0f;

    std::int32_t page_offset_x = 0;

    std::int32_t page_offset_y = 0;

    std::uint32_t virtual_page_base = 0u;

    std::uint32_t flags = 0u; // bit 0: valid, bit 1: is_ortho_clipmap, bits 8..15: grid_dim
};

static_assert(sizeof(GpuVsmLightView) == 128);

struct alignas(16) GpuVsmPageJob {
    std::array<float, 16> page_view_proj{};

    std::uint32_t phys_page = 0u; // [0..1023] inside target 4096x4096 pool

    std::uint32_t flags = 0u; // bit 0: ortho_pancake

    std::uint32_t pad0 = 0u;

    std::uint32_t pad1 = 0u;
};

static_assert(sizeof(GpuVsmPageJob) == 80);

struct GpuVsmPageInstancePair {
    std::uint32_t job_index = 0u;

    std::uint32_t instance_id = 0u;
};

static_assert(sizeof(GpuVsmPageInstancePair) == 8);

struct VsmCopyRegion {
    std::uint32_t src_static_page = 0u; // [0..1023] in vsm_static_pool

    std::uint32_t dst_dynamic_page = 0u; // [0..1023] in vsm_dynamic_pool
};

struct VsmFramePlan {
    bool active = false;

    bool first_pool_init = false;

    std::uint32_t page_table_srv = rhi::bindless_invalid_index;

    std::uint32_t views_srv = rhi::bindless_invalid_index;

    std::uint32_t static_jobs_srv = rhi::bindless_invalid_index;

    std::uint32_t static_pairs_srv = rhi::bindless_invalid_index;

    std::uint32_t dynamic_jobs_srv = rhi::bindless_invalid_index;

    std::uint32_t dynamic_pairs_srv = rhi::bindless_invalid_index;

    std::uint32_t static_pair_count = 0u;

    std::uint32_t dynamic_pair_count = 0u;

    std::uint32_t static_max_opaque_meshlets = 0u;

    std::uint32_t static_max_masked_meshlets = 0u;

    std::uint32_t dynamic_max_opaque_meshlets = 0u;

    std::uint32_t dynamic_max_masked_meshlets = 0u;

    bool static_has_masked = false;

    bool dynamic_has_masked = false;

    bool clear_all_static = false;

    std::vector<nri::Rect> static_clear_rects;

    std::vector<nri::Rect> dynamic_clear_rects;

    std::vector<VsmCopyRegion> cow_copies;

    std::uint32_t mapped_faces = 0u;

    std::uint32_t reused_faces = 0u;

    std::uint32_t static_updates = 0u;

    std::uint32_t dynamic_updates = 0u;

    std::uint32_t evictions = 0u;

    std::uint32_t resident_atlas_cells = 0u;
};

class VsmShadowCache {
public:
    [[nodiscard]] Result<void> ensure_pools(rhi::Device& device);

    [[nodiscard]] Result<VsmFramePlan> prepare_frame(rhi::Device& device, std::size_t slot_index,
                                                     std::uint64_t sequence, const Frame& frame,
                                                     PersistentRenderScene& scene,
                                                     std::uint64_t masked_material_hash,
                                                     std::span<GpuLightRecord> gpu_lights,
                                                     std::span<const std::uint64_t> gpu_light_keys,
                                                     std::uint32_t dir_light_count, std::uint32_t primary_sun_idx);

    void commit_submitted(std::uint64_t scene_identity, std::uint64_t masked_material_hash) noexcept;

    void invalidate_all() noexcept;

    void retire(rhi::Device& device) noexcept;

    [[nodiscard]] OwnedTexture& static_pool() noexcept { return static_pool_; }

    [[nodiscard]] OwnedTexture& dynamic_pool() noexcept { return dynamic_pool_; }

    [[nodiscard]] bool pools_initialized() const noexcept { return pools_initialized_; }

    [[nodiscard]] std::uint64_t resource_bytes() const noexcept;

private:
    struct StaticSlot {
        bool allocated = false;

        bool valid_baked = false;

        bool has_masked_casters = false;

        std::uint32_t virtual_page = vsm_page_unmapped;

        std::uint64_t last_used_seq = 0u;

        // Identity of baked virtual page content:
        std::uint64_t light_key = 0u;

        std::int32_t page_ix = 0;

        std::int32_t page_iy = 0;

        float z_snap = 0.0f;

        std::array<float, 16> page_vp_bits{};

        std::uint64_t static_sig_sum = 0u;

        std::uint64_t static_sig_xor = 0u;
    };

    OwnedTexture static_pool_;

    OwnedTexture dynamic_pool_;

    bool pools_initialized_ = false;

    std::uint64_t cached_scene_identity_ = ~0ull;

    std::uint64_t cached_masked_hash_ = 0u;

    math::Vector3 cached_sun_dir_{0.f, -1.f, 0.f};

    std::uint64_t cached_sun_key_ = 0u;

    std::array<float, vsm_cascade_count> cached_sun_z_snaps_{};

    bool has_sun_z_snaps_ = false;

    std::array<StaticSlot, vsm_static_page_capacity> static_slots_{};

    std::array<std::uint32_t, vsm_page_table_size> virtual_to_static_{};

    // Pending state built in prepare_frame, committed only on successful GPU submit:
    std::array<StaticSlot, vsm_static_page_capacity> next_static_slots_{};

    std::array<std::uint32_t, vsm_page_table_size> next_virtual_to_static_{};

    math::Vector3 next_sun_dir_{0.f, -1.f, 0.f};

    std::uint64_t next_sun_key_ = 0u;

    std::array<float, vsm_cascade_count> next_sun_z_snaps_{};

    bool next_has_sun_z_snaps_ = false;

    bool pending_commit_ = false;

    HostSlotRing<std::uint32_t> page_table_ring_;

    HostSlotRing<GpuVsmLightView> views_ring_;

    HostSlotRing<GpuVsmPageJob> static_jobs_ring_;

    HostSlotRing<GpuVsmPageInstancePair> static_pairs_ring_;

    HostSlotRing<GpuVsmPageJob> dynamic_jobs_ring_;

    HostSlotRing<GpuVsmPageInstancePair> dynamic_pairs_ring_;
};

} // namespace atelier::renderer::detail
