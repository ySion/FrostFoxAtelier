#pragma once

#include "foundation/map.hpp"
#include "geometry_pool.hpp"
#include "gpu_resources.hpp"
#include "material_archetypes.hpp"
#include "renderer/scene_visibility.hpp"

#include <array>
#include <cstdint>
#include <functional>

namespace atelier::renderer::detail {

// Authoritative GPU instance record (192B = 12 float4s, 16B-aligned).
// `selected` is intentionally decoupled into SelectedBitmaskBuffer so Editor selection clicks mutate 0 bytes here!
struct alignas(16) GpuInstanceRecord {
    std::array<float, 16> world{};

    std::array<float, 16> prev_world{};

    std::array<float, 3> normal_row0{};

    float tangent_sign = 1.0f;

    std::array<float, 3> normal_row1{};

    std::uint32_t mesh_pool_index = 0u;

    std::array<float, 3> normal_row2{};

    std::uint32_t material_slot_offset = 0u;

    std::uint32_t part_table_base = 0u;

    std::uint32_t flags = 0u; // bit 0: has_prev_motion, bit 1: frustum_visible, bit 3: negative_det

    std::uint32_t stable_key_lo = 0u;

    std::uint32_t stable_key_hi = 0u;
};

static_assert(sizeof(GpuInstanceRecord) == 192);

inline constexpr std::uint32_t instance_flag_has_prev_motion = 1u << 0u;

inline constexpr std::uint32_t instance_flag_frustum_visible = 1u << 1u;

inline constexpr std::uint32_t instance_flag_neg_det = 1u << 3u;

inline constexpr std::uint32_t instance_flag_casts_vsm_shadow = 1u << 4u;

inline constexpr std::uint32_t instance_flag_dynamic_shadow = 1u << 5u;

struct VsmInstanceMeta {
    math::Bounds world_bounds{};

    std::uint64_t static_signature = 0u;

    std::uint32_t lod0_meshlets = 0u;

    bool casts_vsm_shadow = true;

    bool is_dynamic_shadow = false;

    bool has_masked_slot = false;
};

// Persistent per-scene_identity world state (preserves main World history across .procgraph SOP preview dives!).
struct PersistentRenderScene {
    std::uint64_t scene_identity = 0u;

    std::uint64_t last_used_sequence = 0u;

    struct TrackedObject {
        math::Matrix4 curr_world{};

        std::uint64_t geometry_cook_id = 0u;

        std::uint64_t last_seen_tick = 0u;

        std::uint8_t dynamic_cooldown = 0u;

        bool in_zone2_proxy = false;

        bool in_tlas_far_cull = false;
    };

    Map<std::uint64_t, TrackedObject> tracked_objects;

    Array<VsmInstanceMeta> vsm_instances;

    std::uint64_t scene_tick = 0u;

    OwnedAccelerationStructure tlas;

    OwnedBuffer tlas_scratch;

    struct PendingCopySpan {
        nri::Buffer* dst = nullptr;

        std::uint64_t dst_offset = 0u;

        std::uint64_t src_offset = 0u;

        std::uint64_t byte_size = 0u;
    };

    HostSlotRing<nri::TopLevelInstance> tlas_instance_slots;

    HostSlotRing<std::uint32_t> selected_mask_slots;

    OwnedBuffer gpu_instance_buffer;

    OwnedBuffer gpu_material_slot_map;

    Array<GpuInstanceRecord> cached_gpu_instances;

    Array<std::uint32_t> cached_slot_map;

    Array<GpuInstanceRecord> staged_gpu_instances;

    Array<std::uint32_t> staged_slot_map;

    HostSlotRing<std::byte> delta_upload_slots;

    nri::Buffer* pending_delta_src = nullptr;

    Array<PendingCopySpan> pending_delta_copies;

    bool gpu_instances_ever_written = false;

    bool gpu_slot_map_ever_written = false;

    bool pending_instances_copy = false;

    bool pending_slot_map_copy = false;

    nri::Buffer* pending_tlas_instance_buf = nullptr;

    std::uint32_t pending_tlas_instance_num = 0u;

    std::uint64_t built_tlas_hash = 0u;

    bool pending_tlas_build = false;

    bool tlas_valid = false;

    bool tlas_gpu_built = false;

    bool tlas_scratch_gpu_used = false;

    std::uint32_t instances_srv = rhi::bindless_invalid_index;

    std::uint32_t selected_mask_srv = rhi::bindless_invalid_index;

    std::uint32_t material_slot_map_srv = rhi::bindless_invalid_index;

    std::uint32_t instance_count = 0u;

    std::uint32_t total_opaque_meshlets = 0u;

    std::uint32_t max_masked_bin_meshlets = 0u;

    std::uint32_t total_selected_meshlets = 0u;

    std::uint32_t total_visible_chunks = 0u;

    bool any_selected = false;

    bool any_masked_instance = false;

    void retire(rhi::Device& device) noexcept;
};

class RenderSceneTable {
public:
    static constexpr std::size_t max_warm_contexts = 4u;

    [[nodiscard]] PersistentRenderScene& acquire(rhi::Device& device, std::uint64_t scene_identity,
                                                 std::uint64_t sequence);

    [[nodiscard]] Result<void> update_scene(rhi::Device& device, PersistentRenderScene& scene, GeometryPool& geo_pool,
                                            const Frame& frame, Span<const GpuSurfaceRecord> surface_records,
                                            std::uint32_t masked_bin_count, std::size_t slot_index,
                                            bool build_ray_scene, bool vsm_active, std::uint64_t current_sequence,
                                            const std::function<bool()>* stop_requested, bool& out_cancelled,
                                            FrameStats& stats);

    static void record_scene_uploads(rhi::Device& device, nri::CommandBuffer& cmd,
                                     PersistentRenderScene& scene) noexcept;

    static void record_tlas_build(rhi::Device& device, nri::CommandBuffer& cmd, PersistentRenderScene& scene) noexcept;

    static void commit_submitted(PersistentRenderScene& scene) noexcept;

    void clear(rhi::Device& device) noexcept;

private:
    Array<std::unique_ptr<PersistentRenderScene>> scenes_;
};

} // namespace atelier::renderer::detail
