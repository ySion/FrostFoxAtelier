#include "renderer/renderer.hpp"

#include "detail/geometry_pool.hpp"
#include "detail/gpu_resources.hpp"
#include "detail/lighting_lut.hpp"
#include "detail/material_archetypes.hpp"
#include "detail/render_scene.hpp"
#include "detail/vsm_shadow_cache.hpp"
#include "foundation/result.hpp"
#include "logging/native.hpp"
#include "math/math.hpp"
#include "memory/fatal.hpp"
#include "rdg/graph.hpp"
#include "renderer/raster_viewport.hpp"
#include "renderer/scene_visibility.hpp"
#include "renderer/timing.hpp"
#include "rhi/backend.hpp"
#include "shader_source.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace atelier::renderer {

namespace {

using detail::check_nri;

using detail::extract_camera_eye;

using detail::fp16_to_fp32;

using detail::fp32_to_fp16;

using detail::HostSlotRing;

using detail::make_error;

using detail::OwnedBuffer;

using detail::OwnedTexture;

using detail::ReadbackSlotRing;

struct FrameCancelled {
    std::string_view phase;
};

template <class T>
using CancellableResult = std::expected<T, std::variant<Error, FrameCancelled>>;

CancellableResult<void> check_stop(const std::function<bool()>* requested, std::string_view phase) {
    if (requested && (*requested)())
        return std::unexpected(FrameCancelled{phase});
    return {};
}

bool finite(float value) noexcept {
    return std::isfinite(value);
}

bool valid_color(const ui::Color& c) noexcept {
    return finite(c.r) && finite(c.g) && finite(c.b) && finite(c.a) && c.r >= 0.f && c.g >= 0.f && c.b >= 0.f &&
           c.a >= 0.f && c.r <= 1.f && c.g <= 1.f && c.b <= 1.f && c.a <= 1.f;
}

bool valid_rect(const ui::Rect& r) noexcept {
    return finite(r.x) && finite(r.y) && finite(r.width) && finite(r.height) && r.width >= 0.f && r.height >= 0.f &&
           finite(r.x + r.width) && finite(r.y + r.height);
}

template <class Pixels>
bool valid_pixels(const Pixels& image, unsigned channels) noexcept {
    return image.identity != 0 && image.width > 0 && image.height > 0 && image.width <= ui::max_texture_dimension_px &&
           image.height <= ui::max_texture_dimension_px &&
           std::uint64_t(image.row_pitch) >= std::uint64_t(image.width) * channels &&
           std::uint64_t(image.row_pitch) * image.height <= image.pixels.size();
}

bool valid_instance(const MeshInstance& mesh) noexcept {
    if (!mesh.geometry || !mesh.material_slots || mesh.material_slots->size() != mesh.geometry->materials().size() ||
        !math::is_finite(mesh.world) || mesh.world[3] != 0.f || mesh.world[7] != 0.f || mesh.world[11] != 0.f ||
        mesh.world[15] != 1.f) {
        return false;
    }
    const std::array<float, 9> linear{mesh.world[0], mesh.world[1], mesh.world[2], mesh.world[4], mesh.world[5],
                                      mesh.world[6], mesh.world[8], mesh.world[9], mesh.world[10]};
    return math::normal_transform(linear).has_value();
}

std::uint8_t linear_to_srgb(float v) noexcept {
    const float c = std::clamp(v, 0.0f, 1.0f);
    const float encoded = c <= 0.0031308f ? (c * 12.92f) : (1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f);
    return static_cast<std::uint8_t>(std::lround(encoded * 255.0f));
}

float halton(std::uint32_t index, std::uint32_t base) noexcept {
    float f = 1.0f;
    float r = 0.0f;
    std::uint32_t i = index;
    while (i > 0u) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(i % base);
        i /= base;
    }
    return r;
}

// ============================================================================
// GPU Constant & Root Constant Structures (Exact match with HLSL shaders!)
// ============================================================================

struct alignas(16) GlobalFrameConstants {
    std::array<float, 16> view{};

    std::array<float, 16> inv_view{};

    std::array<float, 16> projection{};

    std::array<float, 16> inv_projection{};

    std::array<float, 16> view_projection{};

    std::array<float, 16> inv_view_projection{};

    std::array<float, 16> unjittered_view_projection{};

    std::array<float, 16> inv_unjittered_vp{};

    std::array<float, 16> prev_unjittered_vp{};

    std::array<float, 16> prev_view{};

    std::array<float, 16> prev_projection{};

    std::array<float, 4> raster_viewport_transform{};

    std::array<float, 4> scene_viewport{};

    std::array<float, 4> prev_scene_viewport{};

    std::array<std::int32_t, 4> scissor_rect{};

    std::array<float, 3> camera_eye{};

    float camera_far_depth = 0.0f;

    std::array<float, 3> environment_radiance{};

    float sky_rotation_rad = 0.0f;

    std::array<std::uint32_t, 2> render_extent{};

    std::array<std::uint32_t, 2> output_extent{};

    std::array<float, 2> jitter_pixels{};

    float exposure_scale = 1.0f;

    float indirect_gain = 1.0f;

    float ao_radius = 0.35f;

    float reflection_distance = 30.0f;

    float reflection_roughness = 0.6f;

    float exposure_ratio = 1.0f;

    float contact_shadow_length = 0.0f;

    float render_delta_seconds = 1.0f / 60.0f;

    std::uint32_t frame_index = 0u;

    std::uint32_t flags = 0u;

    std::uint32_t shading_mode = 0u;

    std::uint32_t wireframe_mode = 0u;

    std::array<float, 2> wire_style{};

    std::uint32_t mesh_table_srv = 0xFFFFFFFFu;

    std::uint32_t part_tri_base_srv = 0xFFFFFFFFu;

    std::uint32_t instances_srv = 0xFFFFFFFFu;

    std::uint32_t selected_mask_srv = 0xFFFFFFFFu;

    std::uint32_t material_slot_map_srv = 0xFFFFFFFFu;

    std::uint32_t material_surfaces_srv = 0xFFFFFFFFu;

    std::uint32_t material_params_srv = 0xFFFFFFFFu;

    std::uint32_t material_inputs_srv = 0xFFFFFFFFu;

    std::uint32_t lights_srv = 0xFFFFFFFFu;

    std::uint32_t light_count = 0u;

    std::uint32_t dir_light_count = 0u;

    std::uint32_t primary_sun_and_gi_cands = 0xFFFFu;

    std::uint32_t tlas_srv = 0xFFFFFFFFu;

    std::uint32_t sky_map_srv = 0xFFFFFFFFu;

    float sky_scale = 1.0f;

    std::uint32_t matcap_srv = 0xFFFFFFFFu;

    std::uint32_t ltc_matrix_srv = 0xFFFFFFFFu;

    std::uint32_t ltc_amp_srv = 0xFFFFFFFFu;

    std::uint32_t charlie_lut_srv = 0xFFFFFFFFu;

    std::uint32_t atmos_trans_srv = 0xFFFFFFFFu;

    std::uint32_t atmos_multiscatter_srv = 0xFFFFFFFFu;

    std::uint32_t smaa_area_srv = 0xFFFFFFFFu;

    std::uint32_t smaa_search_srv = 0xFFFFFFFFu;

    std::uint32_t instance_count = 0u;

    std::uint32_t archetype_count = 0u;

    std::uint32_t masked_archetype_count = 0u;

    std::uint32_t gi_rays_per_probe = 36u;

    std::uint32_t wrc_voxel_keys_srv = 0xFFFFFFFFu;

    std::uint32_t max_opaque_clusters = 0u;

    std::uint32_t masked_cluster_stride = 0u;

    std::uint32_t max_selected_clusters = 0u;

    std::uint32_t max_cull_chunks = 0u;

    std::array<std::array<float, 4>, 9> sky_sh3{};

    std::array<float, 3> lighting_focus{};

    std::uint32_t hzb_srv = 0xFFFFFFFFu;

    std::array<float, 3> prev_camera_eye{};

    std::uint32_t prev_view_z_srv = 0xFFFFFFFFu;

    std::uint32_t wrc_curr_srv = 0xFFFFFFFFu;

    std::uint32_t wrc_hash_keys_srv = 0xFFFFFFFFu;

    std::uint32_t wrc_hash_keys_uav = 0xFFFFFFFFu;

    std::uint32_t froxel_srv = 0xFFFFFFFFu;

    float fog_density = 0.0f;

    float fog_height_falloff = 0.08f;

    float fog_anisotropy = 0.6f;

    std::uint32_t env_sky_mode = 0u;

    std::uint32_t vsm_static_srv = 0xFFFFFFFFu;

    std::uint32_t vsm_dynamic_srv = 0xFFFFFFFFu;

    std::uint32_t vsm_page_table_srv = 0xFFFFFFFFu;

    std::uint32_t vsm_views_srv = 0xFFFFFFFFu;

    std::uint32_t gi_probe_atlas_srv = 0xFFFFFFFFu;

    std::uint32_t gi_probe_mask_srv = 0xFFFFFFFFu;

    std::uint32_t scene_tile8_offset_packed = 0u;

    std::uint32_t scene_tile16_offset_packed = 0u;
};

static_assert(sizeof(GlobalFrameConstants) == 1248);

using detail::GpuLightRecord;

struct ArchetypeRootConsts {
    std::uint32_t frame_cb_srv = 0u;

    std::uint32_t frame_cb_idx = 0u;

    std::uint32_t material_inputs_srv = 0u;

    std::uint32_t material_params_srv = 0u;

    std::uint32_t material_surfaces_srv = 0u;

    std::uint32_t archetype_index = 0u;

    std::uint32_t bin_offset_or_cluster_srv = 0u;

    std::uint32_t aux0_srv = 0u;

    std::uint32_t aux1_srv = 0u;

    std::uint32_t aux2_uav = 0u;

    std::uint32_t aux3_uav = 0u;

    std::uint32_t aux4_uav = 0u;

    std::uint32_t aux5_uav = 0u;

    std::uint32_t pass_flags = 0u;

    std::uint32_t viewport_packed0 = 0u;

    std::uint32_t viewport_packed1 = 0u;
};

static_assert(sizeof(ArchetypeRootConsts) == 64);

struct CullRootConsts {
    std::uint32_t frame_cb_srv = 0u;

    std::uint32_t phase_mode = 0u;

    std::uint32_t opaque_clusters_uav = 0u;

    std::uint32_t masked_clusters_uav = 0u;

    std::uint32_t selected_clusters_uav = 0xFFFFFFFFu;

    std::uint32_t chunks_uav = 0u;

    std::uint32_t phase2_candidates_uav = 0u;

    std::uint32_t counters_uav = 0u;

    std::uint32_t indirect_args_uav = 0u;

    std::uint32_t aux_indirect_uav = 0xFFFFFFFFu;

    std::uint32_t hzb_src_srv = 0xFFFFFFFFu;

    std::uint32_t hzb_dst_uav = 0xFFFFFFFFu;

    std::uint32_t hzb_src_w = 1u;

    std::uint32_t hzb_src_h = 1u;

    std::uint32_t hzb_dst_w = 1u;

    std::uint32_t flags_and_dst_h = 0u;
};

static_assert(sizeof(CullRootConsts) == 64);

struct UnpackRootConsts {
    std::uint32_t frame_cb_srv = 0u;

    std::uint32_t mode = 0u;

    std::uint32_t visbuffer_srv = 0u;

    std::uint32_t opaque_clusters_srv = 0u;

    std::uint32_t masked_clusters_srv = 0u;

    std::uint32_t pixel_meta_uav = 0u;

    std::uint32_t motion_uav = 0u;

    std::uint32_t prev_depth_uav = 0u;

    std::uint32_t geom_normal_uav = 0u;

    std::uint32_t bary_uav = 0u;

    std::uint32_t bin_counts_uav = 0u;

    std::uint32_t bin_offsets_uav = 0u;

    std::uint32_t bin_cursors_uav = 0u;

    std::uint32_t bin_pixels_uav = 0u;

    std::uint32_t bin_indirect_uav = 0u;

    std::uint32_t tile_emissive_uav = 0xFFFFFFFFu;
};

static_assert(sizeof(UnpackRootConsts) == 64);

struct VsmCullRootConsts {
    std::uint32_t frame_cb_srv = 0u;

    std::uint32_t vsm_cull_mode = 0u;

    std::uint32_t page_jobs_srv = 0u;

    std::uint32_t pairs_srv = 0u;

    std::uint32_t pair_count = 0u;

    std::uint32_t max_opaque = 0u;

    std::uint32_t masked_stride = 0u;

    std::uint32_t opaque_clusters_uav = 0u;

    std::uint32_t masked_clusters_uav = 0u;

    std::uint32_t counters_uav = 0u;

    std::uint32_t indirect_args_uav = 0u;

    std::uint32_t pad0 = 0u;
};

static_assert(sizeof(VsmCullRootConsts) == 48);

struct ShadowRootConsts {
    std::uint32_t frame_cb_srv = 0u;

    std::uint32_t shadow_mode = 0u;

    std::uint32_t depth_srv = 0u;

    std::uint32_t geom_normal_srv = 0u;

    std::uint32_t shadow_mask_uav = 0u;

    std::uint32_t ray_counter_uav = 0xFFFFFFFFu;

    std::uint32_t slab0_srv = 0xFFFFFFFFu;

    std::uint32_t cluster_list_srv = 0xFFFFFFFFu;

    std::uint32_t penumbra_tex_uav_or_srv = 0xFFFFFFFFu;

    std::uint32_t penumbra_queue_uav_or_srv = 0xFFFFFFFFu;

    std::uint32_t penumbra_args_uav_or_srv = 0xFFFFFFFFu;

    std::uint32_t max_penumbra_items = 0u;
};

static_assert(sizeof(ShadowRootConsts) == 48);

struct DirectLightRootConsts {
    std::uint32_t frame_cb_srv = 0u;

    std::uint32_t direct_mode = 0u;

    std::uint32_t depth_srv = 0u;

    std::uint32_t slab0_srv = 0u;

    std::uint32_t slab1_srv = 0u;

    std::uint32_t geom_normal_srv = 0u;

    std::uint32_t shadow_mask_srv = 0xFFFFFFFFu;

    std::uint32_t contact_shadow_srv = 0xFFFFFFFFu;

    std::uint32_t ssilvb_srv = 0xFFFFFFFFu;

    std::uint32_t cluster_list_uav_or_srv = 0u;

    std::uint32_t direct_diff_uav = 0u;

    std::uint32_t direct_spec_uav = 0u;

    std::uint32_t cluster_stats_uav = 0xFFFFFFFFu;

    std::uint32_t penumbra_mask_srv = 0xFFFFFFFFu;
};

static_assert(sizeof(DirectLightRootConsts) == 56);

struct GiRootConsts {
    std::uint32_t frame_cb_srv = 0u;

    std::uint32_t gi_mode = 0u;

    std::uint32_t depth_srv = 0u;

    std::uint32_t hzb_srv = 0xFFFFFFFFu;

    std::uint32_t geom_normal_srv = 0u;

    std::uint32_t slab0_srv = 0u;

    std::uint32_t slab1_srv = 0u;

    std::uint32_t motion_srv = 0u;

    std::uint32_t prev_diffuse_srv = 0xFFFFFFFFu;

    std::uint32_t uav0 = 0xFFFFFFFFu;

    std::uint32_t uav1 = 0xFFFFFFFFu;

    std::uint32_t uav2 = 0xFFFFFFFFu;

    std::uint32_t uav3 = 0xFFFFFFFFu;

    std::uint32_t srv_aux0 = 0xFFFFFFFFu;

    std::uint32_t srv_aux1 = 0xFFFFFFFFu;

    std::uint32_t srv_aux2 = 0xFFFFFFFFu;
};

static_assert(sizeof(GiRootConsts) == 64);

struct ComposeRootConsts {
    std::uint32_t frame_cb_srv = 0u;

    std::uint32_t comp_mode = 0u;

    std::uint32_t depth_srv = 0u;

    std::uint32_t slab0_srv = 0u;

    std::uint32_t slab1_srv = 0u;

    std::uint32_t slab2_srv = 0u;

    std::uint32_t tile_emissive_srv = 0u;

    std::uint32_t direct_diff_srv = 0u;

    std::uint32_t direct_spec_srv = 0u;

    std::uint32_t gi_srv = 0u;

    std::uint32_t refl_srv = 0u;

    std::uint32_t motion_srv = 0u;

    std::uint32_t prev_hdr_or_refl_srv = 0xFFFFFFFFu;

    std::uint32_t uav0 = 0u;

    std::uint32_t uav1 = 0xFFFFFFFFu;

    std::uint32_t atrous_step = 1u;
};

static_assert(sizeof(ComposeRootConsts) == 64);

struct PostRootConsts {
    std::uint32_t frame_cb_srv = 0u;

    std::uint32_t post_mode = 0u;

    std::uint32_t src0_srv = 0xFFFFFFFFu;

    std::uint32_t src1_srv = 0xFFFFFFFFu;

    std::uint32_t src2_srv = 0xFFFFFFFFu;

    std::uint32_t src3_srv = 0xFFFFFFFFu;

    std::uint32_t dst0_uav = 0xFFFFFFFFu;

    std::uint32_t dst1_uav = 0xFFFFFFFFu;

    std::uint32_t src_w = 1u;

    std::uint32_t src_h = 1u;

    std::uint32_t dst_w = 1u;

    std::uint32_t dst_h = 1u;

    float bloom_strength = 0.04f;

    std::uint32_t apply_ign_quant = 0u;
};

static_assert(sizeof(PostRootConsts) == 56);

struct WireRootConsts {
    std::uint32_t frame_cb_srv = 0u;

    std::uint32_t opaque_clusters_srv = 0u;

    std::uint32_t masked_clusters_srv = 0u;

    std::uint32_t selected_clusters_srv = 0xFFFFFFFFu;

    std::uint32_t counters_srv = 0u;

    std::uint32_t visbuffer_srv = 0xFFFFFFFFu;

    std::uint32_t wire_flags = 0u;

    std::uint32_t pad0 = 0u;
};

static_assert(sizeof(WireRootConsts) == 32);

struct UiRootConstants {
    std::uint32_t ui_instances_srv = 0u;

    std::uint32_t first_instance = 0u;

    std::array<float, 2> target_size{};
};

struct ViewportRootConstants {
    std::array<float, 4> raster_viewport_transform{1.f, 1.f, 0.f, 0.f};

    std::uint32_t pass_cb_srv = 0u;

    std::uint32_t pass_cb_index = 0u;
};

struct CavityRootConstants {
    std::array<float, 4> raster_viewport_transform{1.f, 1.f, 0.f, 0.f};

    std::uint32_t pass_cb_srv = 0u;

    std::uint32_t pass_cb_index = 0u;

    std::uint32_t normals_srv = 0u;

    std::uint32_t color_srv = 0u;

    std::uint32_t depth_srv = 0u;
};

struct GridConstants {
    std::array<float, 16> inverse_view_projection{};

    std::array<float, 16> view_projection{};

    std::array<std::uint32_t, 2> guides{};

    float grid_spacing = 0.2f;

    std::uint32_t grid_plane = 0u;

    float camera_far_depth = 0.f;

    std::array<float, 3> pad{};
};

static_assert(sizeof(GridConstants) == 160);

struct CavityConstants {
    std::array<float, 16> inverse_projection{};

    std::array<float, 4> viewport{};

    std::array<float, 4> controls{};
};

static_assert(sizeof(CavityConstants) == 96);

struct GpuLitReadbackCounters {
    std::uint32_t direct_shadow_rays = 0u;

    std::uint32_t cluster_cells = 0u;

    std::uint32_t cluster_overflows = 0u;

    std::uint32_t cluster_max_sources = 0u;

    std::uint32_t probe_primary_rays = 0u;

    std::uint32_t reflection_work_items = 0u;

    std::uint32_t gi_nee_shadow_rays = 0u;

    std::uint32_t gi_gather_vis_rays = 0u;

    std::uint32_t receiver_max_sources = 0u;

    std::array<std::uint32_t, 66> receiver_histogram{};
};

static_assert(sizeof(GpuLitReadbackCounters) == 300);

enum class UiShading : std::uint32_t {
    solid = 0,
    coverage = 1,
    image = 2,
    capsule = 3,
    node_shape = 4,
    arc_sector = 5
};

struct UiInstance {
    ui::Rect bounds;

    ui::Rect uv;

    ui::Color tint;

    ui::Color border_color;

    ui::CornerRadii radius;

    ui::Color image_background;

    float border_width = 0.f;

    std::uint32_t shading = 0u;

    std::array<std::uint32_t, 2> clip{};
};

static_assert(sizeof(UiInstance) == 112);

std::array<std::uint32_t, 2> pack_clip(const ui::Clip& c) noexcept {
    return {std::uint32_t(c.left) | (std::uint32_t(c.top) << 16u),
            std::uint32_t(c.right) | (std::uint32_t(c.bottom) << 16u)};
}

bool overlaps_clip(const ui::Rect& bounds, const ui::Clip& c) noexcept {
    return bounds.x < float(c.right) && bounds.x + bounds.width > float(c.left) && bounds.y < float(c.bottom) &&
           bounds.y + bounds.height > float(c.top);
}

ui::CornerRadii fit_radius(ui::CornerRadii r, ui::Rect bounds) noexcept {
    double factor = 1.0;
    const auto fit = [&](double sum, double extent) {
        if (sum > 0.0)
            factor = std::min(factor, extent / sum);
    };
    fit(double(r.top_left) + r.top_right, bounds.width);
    fit(double(r.bottom_left) + r.bottom_right, bounds.width);
    fit(double(r.top_left) + r.bottom_left, bounds.height);
    fit(double(r.top_right) + r.bottom_right, bounds.height);
    return {float(r.top_left * factor), float(r.top_right * factor), float(r.bottom_right * factor),
            float(r.bottom_left * factor)};
}

enum class BuiltinStyle : std::uint8_t { ui, background, grid, grid_depth, blit };

rhi::GraphicsPipelineStateDesc make_builtin_state(BuiltinStyle style) noexcept {
    rhi::GraphicsPipelineStateDesc g{};
    g.color_formats[0] = nri::Format::RGBA8_SRGB;
    g.color_attachment_count = 1u;
    g.cull_mode = nri::CullMode::NONE;
    if (style == BuiltinStyle::grid_depth) {
        g.depth_format = nri::Format::D32_SFLOAT;
        g.depth_test = true;
        g.depth_write = false;
        g.depth_compare = nri::CompareOp::GREATER;
    }
    if (style != BuiltinStyle::background && style != BuiltinStyle::blit)
        g.premultiplied_blend = true;
    return g;
}

struct UiTextureKey {
    std::uint64_t identity = 0u;

    std::uint64_t revision = 0u;

    bool color = false;

    bool operator==(const UiTextureKey&) const noexcept = default;
};

struct UiTextureHash {
    std::size_t operator()(const UiTextureKey& k) const noexcept {
        return std::hash<std::uint64_t>{}(k.identity) ^ (std::hash<std::uint64_t>{}(k.revision) << 1u) ^
               (std::hash<bool>{}(k.color) << 2u);
    }
};

struct SharedUiTexture {
    std::shared_ptr<rhi::Device> device;

    std::shared_ptr<const void> owner;

    OwnedTexture gpu;

    ~SharedUiTexture() {
        if (device && *device)
            gpu.retire(*device);
    }
};

struct SharedUiTextureHub {
    std::mutex mutex;

    std::unordered_map<const rhi::Device*,
                       std::unordered_map<UiTextureKey, std::weak_ptr<SharedUiTexture>, UiTextureHash>>
        by_device;
};

SharedUiTextureHub& ui_texture_hub() {
    static SharedUiTextureHub hub;
    return hub;
}

Result<void> validate_frame(const Frame& frame, bool ui_only) {
    if (ui_only && (!frame.meshes.empty() || !frame.overlay.empty())) {
        return std::unexpected(make_error(rhi::ErrorCode::invalid_argument,
                                          "UI-only renderer cannot draw 3D meshes or viewport overlays"));
    }
    const auto& ui = frame.ui_frame;
    bool good = ui.id != 0 && finite(ui.dpi_scale) && ui.dpi_scale > 0.f && valid_rect(frame.scene_viewport) &&
                frame.scene_viewport.x >= 0.f && frame.scene_viewport.y >= 0.f &&
                frame.scene_viewport.x + frame.scene_viewport.width <= static_cast<float>(ui.extent.width) &&
                frame.scene_viewport.y + frame.scene_viewport.height <= static_cast<float>(ui.extent.height) &&
                frame.meshes.size() <= 0xFFFFFFu && ui.draws.size() <= std::numeric_limits<std::uint32_t>::max();
    good &= frame.view.shading <= Shading::scene_lit && frame.view.wireframe <= Wireframe::only &&
            frame.view.background <= Background::gradient && frame.view.grid_plane <= GridPlane::yz &&
            finite(frame.view.grid_spacing) && frame.view.grid_spacing >= .001f && frame.view.grid_spacing <= 10000.f &&
            finite(frame.view.wire_width) && frame.view.wire_width > 0.f && frame.view.wire_width <= 8.f &&
            finite(frame.view.wire_opacity) && frame.view.wire_opacity >= 0.f && frame.view.wire_opacity <= 1.f;
    const auto& cavity = frame.view.cavity;
    good &= finite(cavity.ridge) && cavity.ridge >= 0.f && cavity.ridge <= 2.f && finite(cavity.valley) &&
            cavity.valley >= 0.f && cavity.valley <= 2.f && finite(cavity.radius) && cavity.radius >= .5f &&
            cavity.radius <= 4.f;
    good &= math::is_finite(frame.camera.view) && math::is_finite(frame.camera.projection) &&
            finite(frame.camera.far_depth) && frame.camera.far_depth >= 0.f && frame.camera.far_depth <= 1.f;

    std::uint64_t total_slots = 0u;
    for (const auto& mesh : frame.meshes) {
        if (!valid_instance(mesh) || !frame.materials) {
            good = false;
            break;
        }
        total_slots += mesh.material_slots->size();
        for (const auto slot : *mesh.material_slots)
            good &= (slot < frame.materials->surfaces().size());
    }
    good &= (total_slots < (1ull << 27u));

    for (const auto& draw : ui.draws) {
        const bool capsule = finite(draw.capsule_width) && draw.capsule_width > 0.f;
        const bool proc = draw.procedural != ui::ProceduralQuad::none;
        good &= draw.procedural <= ui::ProceduralQuad::arc_sector && !(capsule && proc) && valid_rect(draw.bounds) &&
                valid_color(draw.color) && valid_color(draw.border_color) && valid_color(draw.image_background) &&
                finite(draw.border_width) && draw.border_width >= 0.f && finite(draw.capsule_width) &&
                draw.capsule_width >= 0.f && draw.capsule_width <= ui::max_capsule_width_px &&
                !(draw.coverage && draw.image);
        if (capsule || proc) {
            good &= !draw.coverage && !draw.image;
            for (float v : {draw.uv.x, draw.uv.y, draw.uv.width, draw.uv.height})
                good &= finite(v) && std::abs(v) < 1e7f;
            for (float radius :
                 {draw.radius.top_left, draw.radius.top_right, draw.radius.bottom_right, draw.radius.bottom_left})
                good &= finite(radius) && std::abs(radius) < 1e7f;
        } else {
            good &= valid_rect(draw.uv);
            for (float radius :
                 {draw.radius.top_left, draw.radius.top_right, draw.radius.bottom_right, draw.radius.bottom_left})
                good &= finite(radius) && radius >= 0.f;
        }
        if (draw.coverage)
            good &= valid_pixels(*draw.coverage, 1u);
        if (draw.image)
            good &= valid_pixels(*draw.image, 4u);
    }
    if (!good) {
        return std::unexpected(
            make_error(rhi::ErrorCode::invalid_argument, "Invalid renderer frame, camera, shape, color or image"));
    }

    for (const auto& shape : frame.overlay) {
        bool valid_shape = shape.kind <= OverlayShape::Kind::rectangle && finite(shape.width) && shape.width >= 0.f &&
                           shape.width <= 64.f && valid_color(shape.color) && valid_color(shape.border);
        for (float v : shape.points)
            valid_shape &= finite(v) && std::abs(v) < 1e7f;
        if (shape.kind == OverlayShape::Kind::rectangle)
            valid_shape &= shape.points[2] >= 0.f && shape.points[3] >= 0.f;
        if (!valid_shape)
            return std::unexpected(make_error(rhi::ErrorCode::invalid_argument, "Invalid viewport overlay shape"));
    }

    if (frame.view.shading == Shading::scene_lit && frame.view.wireframe != Wireframe::only && !frame.meshes.empty() &&
        frame.scene_viewport.width > 0.f && frame.scene_viewport.height > 0.f) {
        if (!math::is_finite(frame.lighting_focus))
            return std::unexpected(make_error(rhi::ErrorCode::invalid_argument, "Invalid lighting focus"));
        if (const char* set_err = rendering::scene_lighting_error(frame.view.lit))
            return std::unexpected(make_error(rhi::ErrorCode::invalid_argument, set_err));
        if (!math::is_finite(frame.environment) || frame.environment[0] < 0.f || frame.environment[1] < 0.f ||
            frame.environment[2] < 0.f)
            return std::unexpected(make_error(rhi::ErrorCode::invalid_argument, "Invalid environment radiance"));
        for (const auto& light : frame.lights) {
            const bool is_dir = (light.kind == LightKind::directional);
            const bool is_rect = (light.kind == LightKind::rectangle);
            const bool is_spot = (light.kind == LightKind::spot);
            if (light.kind > LightKind::spot || !math::is_finite(light.position) || !math::is_finite(light.direction) ||
                !math::is_finite(light.color) || light.color[0] < 0.f || light.color[1] < 0.f || light.color[2] < 0.f ||
                !finite(light.intensity) || light.intensity < 0.f || !finite(light.range) ||
                (is_dir ? (light.range < 0.f) : (light.range <= 0.f)) || !finite(light.source_radius) ||
                light.source_radius < 0.f || (is_dir && light.source_radius >= 0.5f * std::numbers::pi_v<float>)) {
                return std::unexpected(make_error(rhi::ErrorCode::invalid_argument, "Invalid scene light definition"));
            }
            const auto dot3d = [](const math::Vector3& a, const math::Vector3& b) noexcept -> double {
                return double(a[0]) * double(b[0]) + double(a[1]) * double(b[1]) + double(a[2]) * double(b[2]);
            };
            const double d_len2 = dot3d(light.direction, light.direction);
            if ((is_dir || is_rect || is_spot) && (!std::isfinite(d_len2) || d_len2 <= 1e-12)) {
                return std::unexpected(
                    make_error(rhi::ErrorCode::invalid_argument, "Light direction must be finite and non-zero"));
            }
            if (is_spot) {
                if (!finite(light.spot_inner_radians) || !finite(light.spot_outer_radians) ||
                    light.spot_inner_radians < 0.f || light.spot_outer_radians <= 0.f ||
                    light.spot_inner_radians > light.spot_outer_radians ||
                    light.spot_outer_radians >= 0.5f * std::numbers::pi_v<float>) {
                    return std::unexpected(make_error(rhi::ErrorCode::invalid_argument, "Invalid spot light cone"));
                }
            }
            if (is_rect) {
                if (!math::is_finite(light.right) || !math::is_finite(light.up) || !finite(light.width) ||
                    light.width <= 0.f || !finite(light.height) || light.height <= 0.f) {
                    return std::unexpected(make_error(rhi::ErrorCode::invalid_argument, "Invalid rectangle light"));
                }
                const double r_len2 = dot3d(light.right, light.right);
                const double u_len2 = dot3d(light.up, light.up);
                if (!std::isfinite(r_len2) || r_len2 <= 1e-12 || !std::isfinite(u_len2) || u_len2 <= 1e-12) {
                    return std::unexpected(make_error(rhi::ErrorCode::invalid_argument,
                                                      "Rectangle light axes must be finite and non-zero"));
                }
                const double dr = std::abs(dot3d(light.direction, light.right));
                const double du = std::abs(dot3d(light.direction, light.up));
                const double ru = std::abs(dot3d(light.right, light.up));
                const double dr2 = dr * dr;
                const double du2 = du * du;
                const double ru2 = ru * ru;
                const double rhs_dr = 1e-3 * d_len2 * r_len2;
                const double rhs_du = 1e-3 * d_len2 * u_len2;
                const double rhs_ru = 1e-3 * r_len2 * u_len2;
                if (!std::isfinite(dr2) || !std::isfinite(du2) || !std::isfinite(ru2) || !std::isfinite(rhs_dr) ||
                    !std::isfinite(rhs_du) || !std::isfinite(rhs_ru) || dr2 > rhs_dr || du2 > rhs_du || ru2 > rhs_ru) {
                    return std::unexpected(
                        make_error(rhi::ErrorCode::invalid_argument, "Rectangle light axes must be orthogonal"));
                }
            }
            const double area = is_rect ? (double(light.width) * light.height) : 1.0;
            if (const char* r_err = rendering::analytic_light_range_error(light.color, light.intensity, area))
                return std::unexpected(make_error(rhi::ErrorCode::invalid_argument, r_err));
        }
    }
    return {};
}

struct GridCamera {
    std::array<float, 16> view_projection{};

    std::array<float, 16> inverse{};
};

std::optional<GridCamera> prepare_grid_camera(const Frame& frame) {
    if ((!frame.view.reference_grid && !frame.view.origin_axes) || frame.scene_viewport.width == 0.f ||
        frame.scene_viewport.height == 0.f)
        return {};
    const auto combined = math::Matrix4d(frame.camera.view) * math::Matrix4d(frame.camera.projection);
    const auto inverse = math::try_inverse(combined);
    if (!inverse)
        return {};
    const math::Matrix4 vp(combined);
    const math::Matrix4 inv(*inverse);
    if (!math::is_finite(vp) || !math::is_finite(inv))
        return {};
    return GridCamera{vp.elements, inv.elements};
}

std::optional<std::array<float, 16>> prepare_cavity_projection(const Frame& frame) {
    const auto& c = frame.view.cavity;
    if (!c.enabled || (c.ridge == 0.f && c.valley == 0.f) || frame.view.shading == Shading::scene_lit ||
        frame.view.wireframe == Wireframe::only || frame.meshes.empty() || frame.scene_viewport.width == 0.f ||
        frame.scene_viewport.height == 0.f)
        return {};
    const auto inverse = math::try_inverse(math::Matrix4d(frame.camera.projection));
    if (!inverse)
        return {};
    const math::Matrix4 result(*inverse);
    if (!math::is_finite(result))
        return {};
    return result.elements;
}

} // namespace

// ============================================================================
// MaterialBatch Public Handle
// ============================================================================

MaterialBatch::MaterialBatch() = default;

MaterialBatch::~MaterialBatch() = default;

MaterialBatch::MaterialBatch(MaterialBatch&&) noexcept = default;

MaterialBatch& MaterialBatch::operator=(MaterialBatch&&) noexcept = default;

std::span<const material::ProgramSurfaceRef> MaterialBatch::surfaces() const noexcept {
    return state_ ? std::span<const material::ProgramSurfaceRef>(state_->surfaces)
                  : std::span<const material::ProgramSurfaceRef>{};
}

// ============================================================================
// Renderer::State
// ============================================================================

struct Renderer::State {
    const std::thread::id owner = std::this_thread::get_id();

    std::shared_ptr<rhi::Device> device;

    rhi::WindowSurface surface;

    rdg::Executor executor;

    rdg::LinearArena rdg_arena{1024u * 1024u};

    Options options;

    bool first_encode = true;

    bool first_frame = true;

    bool cancelled = false;

    std::size_t slot_cursor = 0u;

    std::array<rhi::FencePoint, detail::HostSlotRing<UiInstance>::slot_count> slot_fences{};

    std::uint64_t encode_sequence = 0u;

    std::uint32_t frame_index = 0u;

    // Core Studio / VisBuffer / Viewport PSOs:
    rhi::Pipeline ui_pipeline;

    rhi::Pipeline present_blit_pipeline;

    rhi::Pipeline background_pipeline;

    rhi::Pipeline grid_pipeline;

    rhi::Pipeline grid_depth_pipeline;

    rhi::Pipeline cavity_pipeline;

    rhi::Pipeline vis_cull_cs;

    rhi::Pipeline vis_opaque_ms;

    rhi::Pipeline vis_unpack_cs;

    rhi::Pipeline wireframe_visible_ms;

    rhi::Pipeline wireframe_all_ms;

    // Asynchronous Scene Lit PSO group (compiled on Device workers via `preparation(view)`):
    enum class LitStage : std::uint8_t {
        shadow_ray_opaque = 0,
        direct_light = 1,
        gi_four_layer = 2,
        reflections_compose = 3,
        post_taau_unjitter = 4,
        vsm_cull = 5,
        vsm_opaque_ms = 6,
        count = 7
    };

    struct PendingLitPso {
        LitStage stage{};

        rhi::PipelineJob job;
    };

    std::vector<PendingLitPso> lit_pending;

    std::array<rhi::Pipeline, static_cast<std::size_t>(LitStage::count)> lit_pipelines{};

    bool lit_submitted = false;

    std::optional<Error> lit_prep_failure;

    // Subsystems:
    detail::GeometryPool geometry_pool;

    detail::RenderSceneTable render_scenes;

    detail::LightingLutSet lighting_luts;

    std::size_t live_material_batches = 0u;

    std::map<const source::MaterialProgram*, std::weak_ptr<detail::ProgramArchetype>> archetype_cache;

    std::vector<std::weak_ptr<detail::MaterialProgramSet>> program_set_cache;

    std::vector<std::weak_ptr<detail::MaskedCoverageSet>> masked_coverage_cache;

    std::shared_ptr<detail::MaterialProgramSet> active_program_set;

    std::map<std::pair<const material::Image*, assets::AtxRole>, std::shared_ptr<detail::MaterialImageResource>>
        material_images;

    std::size_t pending_material_uploads = 0u;

    // SkyMap GPU cache (cooked to BC6H_UF16 .atx v4 and uploaded on GPU timeline):
    struct CachedSkyMap {
        rendering::SkyMapRef owner;

        OwnedTexture gpu;

        float scale = 1.0f;
    };

    std::optional<CachedSkyMap> cached_sky_map;

    // Persistent Viewport & GPU Picking Targets:
    OwnedTexture matcap;

    OwnedTexture scene_depth;

    OwnedTexture pick_id_tex;

    OwnedBuffer pick_readback;

    ui::Clip pick_scissor{};

    bool has_pick_target = false;

    // Per-`scene_identity` Warm Scene Lit Contexts (4-way LRU preserving VSM, SHaRC, Screen Probes & TAA across SOP dives!):
    static constexpr std::size_t max_warm_lit_contexts = 4u;

    struct LitContextState {
        std::uint64_t scene_identity = 0u;

        std::uint64_t last_used_sequence = 0u;

        detail::VsmShadowCache vsm_cache;

        OwnedTexture hzb_pyramid;

        OwnedTexture prev_diffuse_emissive;

        std::array<OwnedTexture, 2> taa_history{};

        std::array<OwnedTexture, 2> gi_history{};

        std::array<OwnedTexture, 2> refl_history{};

        std::array<OwnedTexture, 2> refl_moments_history{};

        std::array<OwnedTexture, 2> penumbra_history{};

        std::array<OwnedTexture, 2> ssilvb_ao_history{};

        std::array<OwnedTexture, 2> ssilvb_gi_history{};

        std::array<OwnedTexture, 2> unjit_depth_history{};

        std::array<OwnedTexture, 2> geom_normal_history{};

        std::array<OwnedTexture, 2> gi_color_delta{};

        std::array<OwnedTexture, 2> probe_atlas{};

        std::array<OwnedTexture, 2> probe_mask{};

        std::array<OwnedBuffer, 2> wrc_slots{};

        OwnedBuffer wrc_accum;

        OwnedBuffer wrc_hash_keys;

        OwnedBuffer wrc_voxel_keys;

        std::array<OwnedTexture, 2> froxel_history{};

        OwnedTexture froxel_lut;

        bool lit_persistent_ready = false;

        std::uint32_t history_parity = 0u;

        bool has_temporal_history = false;

        bool has_prev_hzb = false;

        bool has_froxel_history = false;

        bool has_penumbra_history = false;

        std::uint64_t prev_shadow_lights_hash = 0u;

        bool prev_indirect_lighting = false;

        bool prev_ssilvb = false;

        bool prev_reflections = false;

        Antialiasing prev_antialiasing = Antialiasing::smaa;

        rendering::SkyMode prev_sky_mode = rendering::SkyMode::none;

        math::Vector3 prev_environment{-1.f, -1.f, -1.f};

        float prev_sky_rotation = 0.0f;

        float prev_exposure_scale = 1.0f;

        const rendering::SkyMap* prev_sky_ptr = nullptr;

        math::Matrix4 prev_unjittered_vp = math::Matrix4::identity();

        math::Matrix4 prev_view = math::Matrix4::identity();

        math::Matrix4 prev_projection = math::Matrix4::identity();

        std::array<float, 4> prev_scene_viewport{0.f, 0.f, 1.f, 1.f};

        std::array<float, 3> prev_camera_eye{0.f, 0.f, 0.f};

        void retire_screen_history(rhi::Device& dev) noexcept {
            hzb_pyramid.retire(dev);
            prev_diffuse_emissive.retire(dev);
            for (std::size_t i = 0; i < 2u; ++i) {
                taa_history[i].retire(dev);
                gi_history[i].retire(dev);
                refl_history[i].retire(dev);
                refl_moments_history[i].retire(dev);
                penumbra_history[i].retire(dev);
                ssilvb_ao_history[i].retire(dev);
                ssilvb_gi_history[i].retire(dev);
                unjit_depth_history[i].retire(dev);
                geom_normal_history[i].retire(dev);
                gi_color_delta[i].retire(dev);
                probe_atlas[i].retire(dev);
                probe_mask[i].retire(dev);
                wrc_slots[i].retire(dev);
                froxel_history[i].retire(dev);
            }
            wrc_accum.retire(dev);
            wrc_hash_keys.retire(dev);
            wrc_voxel_keys.retire(dev);
            froxel_lut.retire(dev);
            lit_persistent_ready = false;
            has_temporal_history = false;
            has_prev_hzb = false;
            has_froxel_history = false;
            has_penumbra_history = false;
            prev_shadow_lights_hash = 0u;
            prev_indirect_lighting = false;
            prev_ssilvb = false;
            prev_reflections = false;
            prev_exposure_scale = 1.0f;
        }

        void retire(rhi::Device& dev) noexcept {
            vsm_cache.retire(dev);
            retire_screen_history(dev);
        }
    };

    std::vector<std::unique_ptr<LitContextState>> lit_contexts;

    bool any_lit_rendered = false;

    std::uint32_t consecutive_occluded_frames = 0u;

    // Host Upload & Async GPU Counter Readback Rings:
    HostSlotRing<GlobalFrameConstants> frame_cb_slots;

    HostSlotRing<GpuLightRecord> light_slots;

    HostSlotRing<GridConstants> grid_cb_slots;

    HostSlotRing<CavityConstants> cavity_cb_slots;

    HostSlotRing<UiInstance> ui_slots;

    ReadbackSlotRing<GpuLitReadbackCounters> lit_counter_readback;

    struct CompletedCounterSample {
        std::uint64_t frame_id = 0u;

        std::uint64_t epoch = 0u;

        bool inspect_enabled = false;

        GpuLitReadbackCounters counters{};
    };

    std::array<CompletedCounterSample, 8> completed_counter_history{};

    std::size_t completed_counter_cursor = 0u;

    std::uint64_t latest_counter_epoch = 0u;

    // UI Texture Cache (shared across main window & PopupHost Renderers on the same rhi::Device):
    struct CachedUiImage {
        std::shared_ptr<SharedUiTexture> shared;

        bool used = false;
    };

    std::unordered_map<UiTextureKey, CachedUiImage, UiTextureHash> ui_images;

    std::vector<UiInstance> ui_instances;

    std::size_t overlay_instances = 0u;

    std::vector<std::pair<std::uint32_t, const ui::DrawQuad*>> ui_textured;

    FrameStats stats{};

    std::optional<Error> failure;

    Microsoft::WRL::ComPtr<ID3D12InfoQueue> debug;

    ~State() {
        const auto res = close();
        if (!res && res.error().cause.code != rhi::ErrorCode::device_lost)
            atelier::fatal(res.error().cause.message);
    }

    Result<void> record_completed_counters() {
        return lit_counter_readback.poll_completed(
            *device, [&](const GpuLitReadbackCounters& c, std::uint64_t fid, std::uint64_t ep, std::uint64_t tag) {
                if (ep <= latest_counter_epoch)
                    return;
                latest_counter_epoch = ep;
                auto& dst = completed_counter_history[completed_counter_cursor % completed_counter_history.size()];
                dst.frame_id = fid;
                dst.epoch = ep;
                dst.inspect_enabled = (tag & 1u) != 0u;
                dst.counters = c;
                if (!dst.inspect_enabled) {
                    dst.counters.receiver_max_sources = 0u;
                    dst.counters.receiver_histogram = {};
                }
                ++completed_counter_cursor;
            });
    }

    Result<void> check() const {
        if (owner != std::this_thread::get_id())
            return std::unexpected(make_error(rhi::ErrorCode::wrong_thread, "Renderer requires owner thread"));
        if (!device || !*device || !surface)
            return std::unexpected(make_error(rhi::ErrorCode::closed, "Renderer is closed"));
        if (cancelled)
            return std::unexpected(
                make_error(rhi::ErrorCode::closed, "Renderer was cancelled; close is required", true));
        if (failure)
            return std::unexpected(*failure);
        return {};
    }

    Result<void> check_debug() {
        if (!debug)
            return {};
        const UINT64 count = debug->GetNumStoredMessagesAllowedByRetrievalFilter();
        std::optional<std::string> first_error;
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T size = 0;
            if (FAILED(debug->GetMessage(i, nullptr, &size)))
                continue;
            std::vector<std::byte> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (SUCCEEDED(debug->GetMessage(i, message, &size)) && message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                first_error.emplace(message->pDescription ? message->pDescription : "D3D12 debug layer error");
                break;
            }
        }
        debug->ClearStoredMessages();
        if (first_error)
            return std::unexpected(make_error(rhi::ErrorCode::execution_failed, std::move(*first_error), true));
        return {};
    }

    Result<void> compile_builtin_vs_ps(rhi::Pipeline& out, const char* name, const char* vs, const char* ps,
                                       const char* ps_entry, BuiltinStyle style) {
        const std::array<rhi::ShaderStageSource, 2> stages{{
            {nri::StageBits::VERTEX_SHADER, vs, "main", name},
            {nri::StageBits::FRAGMENT_SHADER, ps, ps_entry, name},
        }};
        rhi::PipelineCompileDesc desc{};
        desc.kind = rhi::PipelineKind::vertex;
        desc.debug_name = name;
        desc.stages = stages;
        desc.graphics = make_builtin_state(style);
        auto compiled = rhi::BackendAccess::compile_pipeline(*device, desc);
        if (!compiled)
            return std::unexpected(Error{compiled.error(), false});
        out = std::move(*compiled);
        return {};
    }

    Result<void> compile_builtin_cs(rhi::Pipeline& out, const char* name, const char* cs_src) {
        const std::array<rhi::ShaderStageSource, 1> stages{{
            {nri::StageBits::COMPUTE_SHADER, cs_src, "main", name},
        }};
        rhi::PipelineCompileDesc desc{};
        desc.kind = rhi::PipelineKind::compute;
        desc.debug_name = name;
        desc.stages = stages;
        auto compiled = rhi::BackendAccess::compile_pipeline(*device, desc);
        if (!compiled)
            return std::unexpected(Error{compiled.error(), false});
        out = std::move(*compiled);
        return {};
    }

    Result<void> compile_core_pipelines() {
        ATELIER_TRY(
            compile_builtin_vs_ps(ui_pipeline, "editor_ui", shaders::ui_vs, shaders::ui_ps, "main", BuiltinStyle::ui));
        if (options.ui_only)
            return {};
        ATELIER_TRY(compile_builtin_vs_ps(present_blit_pipeline, "present_blit", shaders::viewport_vs,
                                          shaders::present_blit_ps, "present_blit_ps", BuiltinStyle::blit));
        ATELIER_TRY(compile_builtin_cs(vis_cull_cs, "visbuffer_cull_cs", shaders::visbuffer_cull_cs));
        ATELIER_TRY(compile_builtin_cs(vis_unpack_cs, "vis_unpack_classify_cs", shaders::vis_unpack_classify_cs));

        // Global Opaque VisBuffer Mesh+PS PSO (single PSO for all opaque surfaces in the entire engine!)
        {
            const std::array<rhi::ShaderStageSource, 2> stages{{
                {nri::StageBits::MESH_SHADER, shaders::visbuffer_mesh, "vis_ms_main", "vis_opaque_ms"},
                {nri::StageBits::FRAGMENT_SHADER, shaders::visbuffer_mesh, "vis_opaque_ps_main", "vis_opaque_ps"},
            }};
            rhi::PipelineCompileDesc desc{};
            desc.kind = rhi::PipelineKind::mesh;
            desc.debug_name = "vis_opaque_ms_pso";
            desc.stages = stages;
            desc.graphics.color_formats[0] = nri::Format::R32_UINT;
            desc.graphics.color_attachment_count = 1u;
            desc.graphics.depth_format = nri::Format::D32_SFLOAT;
            desc.graphics.depth_test = true;
            desc.graphics.depth_write = true;
            desc.graphics.depth_compare = nri::CompareOp::GREATER;
            desc.graphics.cull_mode = nri::CullMode::NONE;
            auto compiled = rhi::BackendAccess::compile_pipeline(*device, desc);
            if (!compiled)
                return std::unexpected(Error{compiled.error(), false});
            vis_opaque_ms = std::move(*compiled);
        }

        // Global Wireframe / Selection Mesh+PS PSOs (Visible GREATER_EQUAL & X-Ray ALWAYS)
        for (bool all_lines : {false, true}) {
            const std::array<rhi::ShaderStageSource, 2> stages{{
                {nri::StageBits::MESH_SHADER, shaders::wireframe_mesh, "wire_ms_main", "wireframe_ms"},
                {nri::StageBits::FRAGMENT_SHADER, shaders::wireframe_mesh, "wire_ps_main", "wireframe_ps"},
            }};
            rhi::PipelineCompileDesc desc{};
            desc.kind = rhi::PipelineKind::mesh;
            desc.debug_name = all_lines ? "wireframe_all_ms_pso" : "wireframe_visible_ms_pso";
            desc.stages = stages;
            desc.graphics.color_formats[0] = nri::Format::RGBA8_SRGB;
            desc.graphics.color_attachment_count = 1u;
            desc.graphics.depth_format = nri::Format::D32_SFLOAT;
            desc.graphics.depth_test = true;
            desc.graphics.depth_write = false;
            desc.graphics.depth_compare = all_lines ? nri::CompareOp::ALWAYS : nri::CompareOp::GREATER_EQUAL;
            desc.graphics.cull_mode = nri::CullMode::NONE;
            desc.graphics.premultiplied_blend = true;
            auto compiled = rhi::BackendAccess::compile_pipeline(*device, desc);
            if (!compiled)
                return std::unexpected(Error{compiled.error(), false});
            if (all_lines)
                wireframe_all_ms = std::move(*compiled);
            else
                wireframe_visible_ms = std::move(*compiled);
        }
        return {};
    }

    Result<Preparation> poll_lit_preparation(const ViewSettings& view) {
        if (options.ui_only || view.shading != Shading::scene_lit || view.wireframe == Wireframe::only)
            return Preparation{.done = 1u, .total = 1u, .ready = true};
        if (lit_prep_failure)
            return std::unexpected(*lit_prep_failure);

        constexpr std::uint32_t total = static_cast<std::uint32_t>(LitStage::count);
        if (!lit_submitted) {
            const std::array<std::pair<LitStage, const char*>, 6> specs{{
                {LitStage::shadow_ray_opaque, shaders::shadow_ray_query_cs},
                {LitStage::direct_light, shaders::direct_light_cs},
                {LitStage::gi_four_layer, shaders::gi_four_layer_cs},
                {LitStage::reflections_compose, shaders::reflections_atmosphere_cs},
                {LitStage::post_taau_unjitter, shaders::post_taau_unjitter_cs},
                {LitStage::vsm_cull, shaders::vsm_cull_cs},
            }};
            constexpr std::array<const char*, 6> names{{
                "lit_shadow_ray_cs",
                "lit_direct_light_cs",
                "lit_gi_four_layer_cs",
                "lit_reflections_compose_cs",
                "lit_post_taau_unjitter_cs",
                "lit_vsm_cull_cs",
            }};
            for (std::size_t i = 0; i < specs.size(); ++i) {
                const std::array<rhi::ShaderStageSource, 1> stages{{
                    {nri::StageBits::COMPUTE_SHADER, specs[i].second, "main", names[i]},
                }};
                rhi::PipelineCompileDesc desc{};
                desc.kind = rhi::PipelineKind::compute;
                desc.debug_name = names[i];
                desc.stages = stages;
                auto job = rhi::BackendAccess::submit_pipeline(*device, desc);
                if (!job) {
                    lit_prep_failure = Error{job.error(), false};
                    lit_pending.clear();
                    return std::unexpected(*lit_prep_failure);
                }
                lit_pending.push_back({specs[i].first, std::move(*job)});
            }
            {
                const std::array<rhi::ShaderStageSource, 1> ms_stages{{
                    {nri::StageBits::MESH_SHADER, shaders::vsm_mesh, "vsm_ms_main", "lit_vsm_opaque_ms"},
                }};
                rhi::PipelineCompileDesc mdesc{};
                mdesc.kind = rhi::PipelineKind::mesh;
                mdesc.debug_name = "lit_vsm_opaque_ms_pso";
                mdesc.stages = ms_stages;
                mdesc.graphics.color_attachment_count = 0u;
                mdesc.graphics.depth_format = nri::Format::D32_SFLOAT;
                mdesc.graphics.depth_test = true;
                mdesc.graphics.depth_write = true;
                mdesc.graphics.depth_compare = nri::CompareOp::GREATER;
                mdesc.graphics.depth_clamp = true;
                mdesc.graphics.cull_mode = nri::CullMode::NONE;
                auto mjob = rhi::BackendAccess::submit_pipeline(*device, mdesc);
                if (!mjob) {
                    lit_prep_failure = Error{mjob.error(), false};
                    lit_pending.clear();
                    return std::unexpected(*lit_prep_failure);
                }
                lit_pending.push_back({LitStage::vsm_opaque_ms, std::move(*mjob)});
            }
            lit_submitted = true;
        }

        for (auto it = lit_pending.begin(); it != lit_pending.end();) {
            auto taken = rhi::BackendAccess::take_pipeline(*device, it->job);
            if (!taken) {
                ++it;
                continue;
            }
            if (!*taken) {
                lit_prep_failure = Error{taken->error(), false};
                lit_pending.clear();
                return std::unexpected(*lit_prep_failure);
            }
            lit_pipelines[static_cast<std::size_t>(it->stage)] = std::move(**taken);
            it = lit_pending.erase(it);
        }
        const std::uint32_t done = total - static_cast<std::uint32_t>(lit_pending.size());
        return Preparation{.done = done, .total = total, .ready = lit_pending.empty()};
    }

    Result<OwnedTexture> upload_ui_rgba_texture(const std::uint8_t* src_pixels, std::uint32_t width,
                                                std::uint32_t height, std::uint32_t src_row_pitch, unsigned channels,
                                                bool srgb, const char* debug_name) {
        const nri::Format fmt = (channels == 1u) ? nri::Format::R8_UNORM
                                : srgb           ? nri::Format::RGBA8_SRGB
                                                 : nri::Format::RGBA8_UNORM;
        detail::UploadBatch batch(*device, false);
        ATELIER_TRY_BIND(tex, batch.stage_texture_2d(fmt, width, height, src_pixels, src_row_pitch, debug_name));
        if (auto exec = batch.execute(); !exec) {
            tex->retire(*device);
            return std::unexpected(exec.error());
        }
        return std::move(*tex);
    }

    Result<void> ensure_matcap() {
        if (matcap.texture)
            return {};
        constexpr unsigned side = 256u;
        std::vector<std::uint8_t> pixels(side * side * 4u);
        for (unsigned y = 0; y < side; ++y) {
            for (unsigned x = 0; x < side; ++x) {
                float nx = 2.f * (float(x) + .5f) / side - 1.f;
                float ny = 1.f - 2.f * (float(y) + .5f) / side;
                const float radius = std::hypot(nx, ny);
                if (radius > 1.f) {
                    nx /= radius;
                    ny /= radius;
                }
                const float nz = -std::sqrt(std::max(0.f, 1.f - nx * nx - ny * ny));
                const float key = std::max(0.f, -.45f * nx + .65f * ny - .6124f * nz);
                const float fill = std::max(0.f, .65f * nx - .2f * ny - .7331f * nz);
                const float highlight = std::pow(std::max(0.f, -.32f * nx + .46f * ny - .8283f * nz), 40.f);
                const float light = .14f + .57f * key + .15f * fill + .09f * std::pow(1.f + nz, 3.f);
                const std::array tint{1.f, .98f, .95f};
                const auto offset = (std::size_t(y) * side + x) * 4u;
                for (unsigned c = 0; c < 3u; ++c) {
                    const float linear = std::clamp(light * tint[c] + .1f * highlight, 0.f, 1.f);
                    pixels[offset + c] = linear_to_srgb(linear);
                }
                pixels[offset + 3u] = 255u;
            }
        }
        ATELIER_TRY_ASSIGN(matcap,
                           upload_ui_rgba_texture(pixels.data(), side, side, side * 4u, 4u, true, "studio_matcap"));
        return {};
    }

    Result<void> ensure_scene_depth(ui::PixelExtent extent) {
        if (scene_depth.texture && scene_depth.width == extent.width && scene_depth.height == extent.height)
            return {};
        scene_depth.retire(*device);
        ATELIER_TRY_ASSIGN(scene_depth,
                           detail::create_rw_texture(*device, nri::TextureType::TEXTURE_2D, nri::Format::D32_SFLOAT,
                                                     extent.width, extent.height, 1u, 1u,
                                                     nri::TextureUsageBits::DEPTH_STENCIL_ATTACHMENT |
                                                         nri::TextureUsageBits::SHADER_RESOURCE,
                                                     false, "scene_depth"));
        return {};
    }

    Result<void> ensure_pick_target(ui::PixelExtent extent) {
        if (pick_id_tex.texture && pick_id_tex.width == extent.width && pick_id_tex.height == extent.height)
            return {};
        pick_id_tex.retire(*device);
        has_pick_target = false;
        ATELIER_TRY_ASSIGN(pick_id_tex,
                           detail::create_rw_texture(*device, nri::TextureType::TEXTURE_2D, nri::Format::RG32_UINT,
                                                     extent.width, extent.height, 1u, 1u,
                                                     nri::TextureUsageBits::SHADER_RESOURCE |
                                                         nri::TextureUsageBits::SHADER_RESOURCE_STORAGE,
                                                     false, "pick_id_buffer"));
        return {};
    }

    LitContextState* find_lit_context(std::uint64_t scene_identity) noexcept {
        for (auto& ctx : lit_contexts) {
            if (ctx->scene_identity == scene_identity)
                return ctx.get();
        }
        return nullptr;
    }

    LitContextState& acquire_lit_context(std::uint64_t scene_identity, std::uint64_t seq) {
        if (auto* found = find_lit_context(scene_identity)) {
            found->last_used_sequence = seq;
            return *found;
        }
        if (lit_contexts.size() >= max_warm_lit_contexts) {
            auto oldest = std::ranges::min_element(lit_contexts, {}, &LitContextState::last_used_sequence);
            (*oldest)->retire(*device);
            **oldest = LitContextState{};
            (*oldest)->scene_identity = scene_identity;
            (*oldest)->last_used_sequence = seq;
            return **oldest;
        }
        auto created = std::make_unique<LitContextState>();
        created->scene_identity = scene_identity;
        created->last_used_sequence = seq;
        LitContextState& ref = *created;
        lit_contexts.push_back(std::move(created));
        return ref;
    }

    Result<void> ensure_lit_targets(LitContextState& lc, ui::PixelExtent extent) {
        ATELIER_TRY(lighting_luts.ensure(*device));

        if (!lc.lit_persistent_ready) {
            constexpr std::uint64_t wrc_slot_bytes = 262144ull * 32ull;
            constexpr std::uint64_t wrc_accum_bytes = 262144ull * 32ull;
            constexpr std::uint64_t wrc_hash_key_bytes = 262144ull * 4ull;
            constexpr std::uint64_t wrc_voxel_key_bytes = 262144ull * 8ull;
            for (std::size_t i = 0; i < 2u; ++i) {
                ATELIER_TRY_ASSIGN(lc.wrc_slots[i],
                                   detail::create_rw_buffer(*device, wrc_slot_bytes, 16u, false,
                                                            nri::BufferUsageBits::NONE, "sharc_slots"));
            }
            ATELIER_TRY_ASSIGN(lc.wrc_accum, detail::create_rw_buffer(*device, wrc_accum_bytes, 16u, false,
                                                                      nri::BufferUsageBits::NONE, "sharc_accum"));
            ATELIER_TRY_ASSIGN(lc.wrc_hash_keys,
                               detail::create_rw_buffer(*device, wrc_hash_key_bytes, 4u, false,
                                                        nri::BufferUsageBits::NONE, "sharc_hash_keys"));
            ATELIER_TRY_ASSIGN(lc.wrc_voxel_keys,
                               detail::create_rw_buffer(*device, wrc_voxel_key_bytes, 8u, false,
                                                        nri::BufferUsageBits::NONE, "sharc_voxel_keys"));
            lc.lit_persistent_ready = true;
        }

        const std::uint32_t hw = std::max(1u, (extent.width + 1u) / 2u);
        const std::uint32_t hh = std::max(1u, (extent.height + 1u) / 2u);
        if (!lc.taa_history[0].texture || lc.taa_history[0].width != extent.width ||
            lc.taa_history[0].height != extent.height) {
            lc.hzb_pyramid.retire(*device);
            const std::uint32_t mips = std::min<std::uint32_t>(9u, detail::calc_mip_count(hw, hh));
            ATELIER_TRY_ASSIGN(lc.hzb_pyramid,
                               detail::create_rw_texture(*device, nri::TextureType::TEXTURE_2D,
                                                         nri::Format::RG32_SFLOAT, hw, hh, 1u, mips,
                                                         nri::TextureUsageBits::SHADER_RESOURCE |
                                                             nri::TextureUsageBits::SHADER_RESOURCE_STORAGE,
                                                         true, "hzb_pyramid"));

            lc.prev_diffuse_emissive.retire(*device);
            ATELIER_TRY_ASSIGN(lc.prev_diffuse_emissive,
                               detail::create_rw_texture(*device, nri::TextureType::TEXTURE_2D,
                                                         nri::Format::RGBA16_SFLOAT, hw, hh, 1u, 1u,
                                                         nri::TextureUsageBits::SHADER_RESOURCE |
                                                             nri::TextureUsageBits::SHADER_RESOURCE_STORAGE,
                                                         false, "prev_diffuse_emissive"));

            const std::uint32_t probes_x = std::max(1u, (extent.width + 7u) / 8u);
            const std::uint32_t probes_y = std::max(1u, (extent.height + 7u) / 8u);
            const std::uint32_t probe_mask_mips =
                std::min<std::uint32_t>(8u, detail::calc_mip_count(probes_x, probes_y));

            for (std::size_t i = 0; i < 2u; ++i) {
                lc.taa_history[i].retire(*device);
                ATELIER_TRY_ASSIGN(
                    lc.taa_history[i],
                    detail::create_rw_texture(
                        *device, nri::TextureType::TEXTURE_2D, nri::Format::RGBA16_SFLOAT, extent.width, extent.height,
                        1u, 1u, nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE,
                        false, "taa_history"));

                lc.gi_history[i].retire(*device);
                ATELIER_TRY_ASSIGN(
                    lc.gi_history[i],
                    detail::create_rw_texture(
                        *device, nri::TextureType::TEXTURE_2D, nri::Format::RGBA16_SFLOAT, extent.width, extent.height,
                        1u, 1u, nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE,
                        false, "gi_history"));

                lc.refl_history[i].retire(*device);
                ATELIER_TRY_ASSIGN(
                    lc.refl_history[i],
                    detail::create_rw_texture(
                        *device, nri::TextureType::TEXTURE_2D, nri::Format::RGBA16_SFLOAT, extent.width, extent.height,
                        1u, 1u, nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE,
                        false, "refl_history"));

                lc.refl_moments_history[i].retire(*device);
                ATELIER_TRY_ASSIGN(
                    lc.refl_moments_history[i],
                    detail::create_rw_texture(
                        *device, nri::TextureType::TEXTURE_2D, nri::Format::RG16_SFLOAT, extent.width, extent.height,
                        1u, 1u, nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE,
                        false, "refl_moments_history"));

                lc.penumbra_history[i].retire(*device);
                ATELIER_TRY_ASSIGN(
                    lc.penumbra_history[i],
                    detail::create_rw_texture(
                        *device, nri::TextureType::TEXTURE_2D, nri::Format::RGBA16_SFLOAT, extent.width, extent.height,
                        1u, 1u, nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE,
                        false, "penumbra_history"));

                lc.ssilvb_ao_history[i].retire(*device);
                ATELIER_TRY_ASSIGN(
                    lc.ssilvb_ao_history[i],
                    detail::create_rw_texture(
                        *device, nri::TextureType::TEXTURE_2D, nri::Format::RGBA16_SFLOAT, extent.width, extent.height,
                        1u, 1u, nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE,
                        false, "ssilvb_ao_history"));

                lc.ssilvb_gi_history[i].retire(*device);
                ATELIER_TRY_ASSIGN(
                    lc.ssilvb_gi_history[i],
                    detail::create_rw_texture(
                        *device, nri::TextureType::TEXTURE_2D, nri::Format::RGBA16_SFLOAT, extent.width, extent.height,
                        1u, 1u, nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE,
                        false, "ssilvb_gi_history"));

                lc.unjit_depth_history[i].retire(*device);
                ATELIER_TRY_ASSIGN(
                    lc.unjit_depth_history[i],
                    detail::create_rw_texture(
                        *device, nri::TextureType::TEXTURE_2D, nri::Format::R32_SFLOAT, extent.width, extent.height, 1u,
                        1u, nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE,
                        false, "unjit_depth_history"));

                lc.geom_normal_history[i].retire(*device);
                ATELIER_TRY_ASSIGN(lc.geom_normal_history[i],
                                   detail::create_rw_texture(*device, nri::TextureType::TEXTURE_2D,
                                                             nri::Format::R32_UINT, extent.width, extent.height, 1u, 1u,
                                                             nri::TextureUsageBits::SHADER_RESOURCE |
                                                                 nri::TextureUsageBits::SHADER_RESOURCE_STORAGE,
                                                             false, "geom_normal_history"));

                lc.gi_color_delta[i].retire(*device);
                ATELIER_TRY_ASSIGN(
                    lc.gi_color_delta[i],
                    detail::create_rw_texture(
                        *device, nri::TextureType::TEXTURE_2D, nri::Format::R16_SFLOAT, extent.width, extent.height, 1u,
                        1u, nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE,
                        false, "gi_color_delta"));

                lc.probe_atlas[i].retire(*device);
                ATELIER_TRY_ASSIGN(
                    lc.probe_atlas[i],
                    detail::create_rw_texture(
                        *device, nri::TextureType::TEXTURE_2D, nri::Format::RGBA16_SFLOAT, probes_x * 8u, probes_y * 8u,
                        1u, 1u, nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE,
                        false, "screen_probe_atlas"));

                lc.probe_mask[i].retire(*device);
                ATELIER_TRY_ASSIGN(lc.probe_mask[i], detail::create_rw_texture(
                                                         *device, nri::TextureType::TEXTURE_2D, nri::Format::R32_UINT,
                                                         probes_x, probes_y, 1u, probe_mask_mips,
                                                         nri::TextureUsageBits::SHADER_RESOURCE |
                                                             nri::TextureUsageBits::SHADER_RESOURCE_STORAGE,
                                                         true, "screen_probe_mask"));
            }
            lc.has_temporal_history = false;
            lc.has_prev_hzb = false;
            lc.has_penumbra_history = false;
            lc.prev_indirect_lighting = false;
            lc.prev_ssilvb = false;
            lc.prev_reflections = false;
        }
        return {};
    }

    Result<void> ensure_froxel_targets(LitContextState& lc) {
        if (lc.froxel_lut.texture)
            return {};
        for (std::size_t i = 0; i < 2u; ++i) {
            ATELIER_TRY_ASSIGN(lc.froxel_history[i],
                               detail::create_rw_texture(*device, nri::TextureType::TEXTURE_3D,
                                                         nri::Format::RGBA16_SFLOAT, 160u, 90u, 64u, 1u,
                                                         nri::TextureUsageBits::SHADER_RESOURCE |
                                                             nri::TextureUsageBits::SHADER_RESOURCE_STORAGE,
                                                         false, "froxel_history"));
        }
        ATELIER_TRY_ASSIGN(lc.froxel_lut, detail::create_rw_texture(*device, nri::TextureType::TEXTURE_3D,
                                                                    nri::Format::RGBA16_SFLOAT, 160u, 90u, 64u, 1u,
                                                                    nri::TextureUsageBits::SHADER_RESOURCE |
                                                                        nri::TextureUsageBits::SHADER_RESOURCE_STORAGE,
                                                                    false, "froxel_integrated_lut"));
        return {};
    }

    Result<void> ensure_sky_map(const rendering::SkyMapRef& sky) {
        if (!sky) {
            if (cached_sky_map) {
                cached_sky_map->gpu.retire(*device);
                cached_sky_map.reset();
            }
            return {};
        }
        if (cached_sky_map && cached_sky_map->owner == sky && cached_sky_map->gpu.texture)
            return {};

        if (cached_sky_map) {
            cached_sky_map->gpu.retire(*device);
            cached_sky_map.reset();
        }

        const std::filesystem::path tex_cache_dir =
            options.shader_cache.empty() ? std::filesystem::path{}
                                         : (options.shader_cache.parent_path() / L"renderer-texture-cache");
        auto cooked = tex_cache_dir.empty() ? assets::cook_atx_sky_map(*sky)
                                            : assets::load_or_cook_atx_sky_map(*sky, tex_cache_dir);
        if (!cooked)
            return std::unexpected(make_error(rhi::ErrorCode::invalid_argument, "SkyMap ATX cooking failed", true));

        CachedSkyMap next{};
        next.owner = sky;
        next.scale = std::ldexp(1.0f, sky->scale_exponent());

        detail::UploadBatch batch(*device, true);
        ATELIER_TRY_ASSIGN(next.gpu, batch.stage_atx_texture(std::move(cooked), "sky_map_hdr"));
        if (auto exec = batch.execute(); !exec) {
            next.gpu.retire(*device);
            return std::unexpected(exec.error());
        }
        cached_sky_map = std::move(next);
        return {};
    }

    Result<std::shared_ptr<detail::MaterialImageResource>>
    resolve_material_image(material::ImageRef img, assets::AtxRole role, detail::UploadBatch& upload_batch,
                           std::vector<std::pair<const material::Image*, assets::AtxRole>>& newly_staged,
                           assets::AtxTextureRef precooked = {}) {
        const auto key = std::pair{img.get(), role};
        if (const auto found = material_images.find(key); found != material_images.end())
            return found->second;

        auto cooked = std::move(precooked);
        if (!cooked) {
            const std::filesystem::path tex_cache_dir =
                options.shader_cache.empty() ? std::filesystem::path{}
                                             : (options.shader_cache.parent_path() / L"renderer-texture-cache");
            cooked = tex_cache_dir.empty() ? assets::cook_atx_image(*img, role)
                                           : assets::load_or_cook_atx_image(*img, role, tex_cache_dir);
        }
        if (!cooked)
            return std::unexpected(make_error(rhi::ErrorCode::invalid_argument, "Material texture cooking failed"));

        auto res = std::make_shared<detail::MaterialImageResource>(img, role, device);
        res->format = cooked->format;
        if (role == assets::AtxRole::normal_bc5)
            res->toksvig_slope = assets::compute_atx_toksvig_slope(*cooked);

        ATELIER_TRY_ASSIGN(res->gpu, upload_batch.stage_atx_texture(std::move(cooked), "material_atx"));
        material_images.emplace(key, res);
        newly_staged.push_back(key);
        return res;
    }

    template <class Pixels>
    Result<std::uint32_t> resolve_ui_texture(const std::shared_ptr<const Pixels>& img_owner) {
        constexpr bool is_color = std::is_same_v<Pixels, ui::ColorImage>;
        constexpr unsigned channels = is_color ? 4u : 1u;
        const UiTextureKey key{img_owner->identity, img_owner->revision, is_color};
        auto [it, inserted] = ui_images.try_emplace(key);
        auto& entry = it->second;
        if (inserted) {
            auto& hub = ui_texture_hub();
            {
                std::scoped_lock lock(hub.mutex);
                auto& dev_map = hub.by_device[device.get()];
                if (const auto hit = dev_map.find(key); hit != dev_map.end())
                    entry.shared = hit->second.lock();
            }
            if (!entry.shared) {
                auto uploaded = upload_ui_rgba_texture(img_owner->pixels.data(), img_owner->width, img_owner->height,
                                                       img_owner->row_pitch, channels, is_color, "ui_image");
                if (!uploaded) {
                    ui_images.erase(it);
                    Error e = std::move(uploaded).error();
                    e.terminal = true;
                    return std::unexpected(std::move(e));
                }
                auto created = std::make_shared<SharedUiTexture>();
                created->device = device;
                created->owner = img_owner;
                created->gpu = std::move(*uploaded);
                std::scoped_lock lock(hub.mutex);
                auto& dev_map = hub.by_device[device.get()];
                auto& weak_slot = dev_map[key];
                if (auto raced = weak_slot.lock()) {
                    entry.shared = std::move(raced);
                } else {
                    weak_slot = created;
                    entry.shared = std::move(created);
                }
            }
        }
        entry.used = true;
        return entry.shared->gpu.srv_bindless;
    }

    void prepare_ui(const Frame& source) {
        const auto& frame = source.ui_frame;
        ui_instances.clear();
        ui_textured.clear();
        overlay_instances = 0u;
        ui_instances.reserve(frame.draws.size() + source.overlay.size());

        const auto& viewport = source.scene_viewport;
        if (viewport.width > 0.f && viewport.height > 0.f && !source.overlay.empty()) {
            const auto clip = detail::raster_viewport(viewport).scissor;
            for (const auto& shape : source.overlay) {
                UiInstance instance{};
                instance.tint = shape.color;
                instance.clip = pack_clip(clip);
                if (shape.kind == OverlayShape::Kind::segment) {
                    if (shape.color.a == 0.f || shape.width == 0.f)
                        continue;
                    const auto [ax, ay, bx, by] = shape.points;
                    const float pad = shape.width * .5f + 1.f;
                    instance.bounds = {std::min(ax, bx) - pad, std::min(ay, by) - pad, std::abs(bx - ax) + 2.f * pad,
                                       std::abs(by - ay) + 2.f * pad};
                    instance.uv = {ax, ay, bx, by};
                    instance.radius = {shape.width * .5f, 0.f, 0.f, 0.f};
                    instance.shading = std::to_underlying(UiShading::capsule);
                } else {
                    if ((shape.color.a == 0.f && (shape.width == 0.f || shape.border.a == 0.f)) ||
                        shape.points[2] == 0.f || shape.points[3] == 0.f)
                        continue;
                    instance.bounds = {shape.points[0], shape.points[1], shape.points[2], shape.points[3]};
                    instance.uv = {0.f, 0.f, 1.f, 1.f};
                    instance.border_color = shape.border;
                    instance.border_width =
                        std::min(shape.width, std::min(shape.points[2] * .5f, shape.points[3] * .5f));
                }
                if (overlaps_clip(instance.bounds, clip))
                    ui_instances.push_back(instance);
            }
            overlay_instances = ui_instances.size();
        }

        for (const auto& item : frame.draws) {
            const ui::Clip clip{std::clamp(item.clip.left, 0, int(frame.extent.width)),
                                std::clamp(item.clip.top, 0, int(frame.extent.height)),
                                std::clamp(item.clip.right, 0, int(frame.extent.width)),
                                std::clamp(item.clip.bottom, 0, int(frame.extent.height))};
            const bool capsule = item.capsule_width > 0.f;
            const bool proc = item.procedural != ui::ProceduralQuad::none;
            if (clip.left >= clip.right || clip.top >= clip.bottom || item.bounds.width == 0.f ||
                item.bounds.height == 0.f || !overlaps_clip(item.bounds, clip) || (capsule && item.color.a == 0.f) ||
                (!capsule && item.color.a == 0.f && (!item.image || item.image_background.a == 0.f) &&
                 (item.border_width == 0.f || item.border_color.a == 0.f)))
                continue;
            const auto shading = capsule                                             ? UiShading::capsule
                                 : item.procedural == ui::ProceduralQuad::node_shape ? UiShading::node_shape
                                 : item.procedural == ui::ProceduralQuad::arc_sector ? UiShading::arc_sector
                                 : item.coverage                                     ? UiShading::coverage
                                 : item.image                                        ? UiShading::image
                                                                                     : UiShading::solid;
            if (shading == UiShading::coverage || shading == UiShading::image)
                ui_textured.emplace_back(static_cast<std::uint32_t>(ui_instances.size()), &item);
            ui_instances.push_back(
                {item.bounds, item.uv, item.color, item.border_color,
                 capsule ? ui::CornerRadii{item.capsule_width * .5f, 0.f, 0.f, 0.f}
                 : proc  ? item.radius
                         : fit_radius(item.radius, item.bounds),
                 item.image_background,
                 capsule ? 0.f
                         : std::min(item.border_width, std::min(item.bounds.width * .5f, item.bounds.height * .5f)),
                 std::to_underlying(shading), pack_clip(clip)});
        }
    }

    std::uint64_t compute_lit_logical_bytes(const detail::PersistentRenderScene& scene,
                                            const LitContextState& lc) const noexcept {
        std::uint64_t total = geometry_pool.logical_bytes() + lighting_luts.logical_bytes() + scene_depth.byte_size +
                              pick_id_tex.byte_size + lc.hzb_pyramid.byte_size + lc.prev_diffuse_emissive.byte_size +
                              lc.wrc_accum.capacity + lc.wrc_hash_keys.capacity + lc.wrc_voxel_keys.capacity +
                              lc.froxel_lut.byte_size + lc.vsm_cache.resource_bytes() + scene.tlas.byte_size +
                              scene.tlas_scratch.capacity + scene.gpu_instance_buffer.capacity +
                              scene.gpu_material_slot_map.capacity;
        for (std::size_t i = 0; i < 2u; ++i) {
            total += lc.taa_history[i].byte_size + lc.gi_history[i].byte_size + lc.refl_history[i].byte_size +
                     lc.refl_moments_history[i].byte_size + lc.ssilvb_ao_history[i].byte_size +
                     lc.ssilvb_gi_history[i].byte_size + lc.unjit_depth_history[i].byte_size +
                     lc.geom_normal_history[i].byte_size + lc.gi_color_delta[i].byte_size +
                     lc.probe_atlas[i].byte_size + lc.probe_mask[i].byte_size + lc.wrc_slots[i].capacity +
                     lc.froxel_history[i].byte_size;
        }
        for (const auto& [k, img] : material_images) {
            (void)k;
            total += img->gpu.byte_size;
        }
        if (cached_sky_map)
            total += cached_sky_map->gpu.byte_size;
        return total;
    }

    Result<void> close() {
        if (!device || !*device || !surface)
            return {};
        if (owner != std::this_thread::get_id())
            return std::unexpected(make_error(rhi::ErrorCode::wrong_thread, "Renderer close requires owner thread"));
        if (live_material_batches != 0u)
            return std::unexpected(
                make_error(rhi::ErrorCode::busy, "Release material batches before closing Renderer"));

        lit_pending.clear();

        auto idle = device->wait_idle();
        if (!idle && idle.error().code != rhi::ErrorCode::device_lost)
            return std::unexpected(Error{idle.error(), true});

        ui_images.clear();
        material_images.clear();
        pending_material_uploads = 0u;
        active_program_set.reset();
        program_set_cache.clear();
        masked_coverage_cache.clear();
        archetype_cache.clear();

        if (cached_sky_map) {
            cached_sky_map->gpu.retire(*device);
            cached_sky_map.reset();
        }

        render_scenes.clear(*device);
        for (auto& lc : lit_contexts)
            lc->retire(*device);
        lit_contexts.clear();
        geometry_pool.clear(*device);
        lighting_luts.retire(*device);

        matcap.retire(*device);
        scene_depth.retire(*device);
        pick_id_tex.retire(*device);
        pick_readback.retire(*device);
        has_pick_target = false;

        frame_cb_slots.retire_all(*device);
        light_slots.retire_all(*device);
        grid_cb_slots.retire_all(*device);
        cavity_cb_slots.retire_all(*device);
        ui_slots.retire_all(*device);
        lit_counter_readback.retire_all(*device);

        ui_pipeline.reset();
        present_blit_pipeline.reset();
        background_pipeline.reset();
        grid_pipeline.reset();
        grid_depth_pipeline.reset();
        cavity_pipeline.reset();
        vis_cull_cs.reset();
        vis_opaque_ms.reset();
        vis_unpack_cs.reset();
        wireframe_visible_ms.reset();
        wireframe_all_ms.reset();
        for (auto& p : lit_pipelines)
            p.reset();

        auto exec_closed = executor.close();
        if (!exec_closed && exec_closed.error().code != rhi::ErrorCode::device_lost)
            return std::unexpected(Error{exec_closed.error(), true});

        auto surf_closed = surface.close();
        if (!surf_closed && surf_closed.error().code != rhi::ErrorCode::device_lost)
            return std::unexpected(Error{surf_closed.error(), true});

        auto& timeline = rhi::BackendAccess::timeline(*device);
        auto& bindless = rhi::BackendAccess::bindless(*device);
        const auto& core = rhi::BackendAccess::core(*device);
        const auto& rt = rhi::BackendAccess::ray_tracing(*device);
        if (!idle)
            rhi::BackendAccess::reclaimer(*device).force_release_all(bindless, core, &rt);
        else
            static_cast<void>(rhi::BackendAccess::reclaimer(*device).reclaim(timeline, bindless, core, &rt));

        Result<void> dbg = check_debug();
        debug.Reset();

        {
            auto& hub = ui_texture_hub();
            std::scoped_lock lock(hub.mutex);
            if (auto hub_it = hub.by_device.find(device.get()); hub_it != hub.by_device.end()) {
                std::erase_if(hub_it->second, [](const auto& kv) { return kv.second.expired(); });
                if (hub_it->second.empty() || device.use_count() == 1)
                    hub.by_device.erase(hub_it);
            }
        }
        if (device.use_count() == 1) {
            auto dev_closed = device->close();
            if (!dev_closed && dev_closed.error().code != rhi::ErrorCode::device_lost)
                return std::unexpected(Error{dev_closed.error(), true});
        }
        device.reset();
        return dbg;
    }
};

// ============================================================================
// Renderer Public Implementation
// ============================================================================

Renderer::Renderer() = default;

Renderer::~Renderer() = default;

Renderer::Renderer(Renderer&&) noexcept = default;

Renderer& Renderer::operator=(Renderer&&) noexcept = default;

Result<Renderer> Renderer::open(std::shared_ptr<rhi::Device> device, rhi::NativeWindow window, const Options& options) {
    if (!window)
        return std::unexpected(make_error(rhi::ErrorCode::invalid_argument, "Renderer requires an application window"));
    if (!device || !*device)
        return std::unexpected(make_error(rhi::ErrorCode::invalid_argument, "Renderer requires a live rhi::Device"));

    auto build = [&]() -> Result<Renderer> {
        Renderer renderer;
        renderer.state_ = std::make_unique<State>();
        auto& s = *renderer.state_;
        s.device = std::move(device);
        s.options = options;
        if (!s.options.shader_cache.empty())
            s.geometry_pool.set_cache_directory(s.options.shader_cache.parent_path() / L"renderer-geometry-cache");

        rhi::SurfaceOptions surf_opts{};
        surf_opts.srgb = true;
        surf_opts.frame_latency_waitable = options.frame_latency_waitable;
        auto opened_surf = rhi::WindowSurface::open(*s.device, window, surf_opts);
        if (!opened_surf)
            return std::unexpected(Error{opened_surf.error(), false});
        s.surface = std::move(*opened_surf);

        auto opened_exec = rdg::Executor::open(*s.device);
        if (!opened_exec)
            return std::unexpected(Error{opened_exec.error(), false});
        s.executor = std::move(*opened_exec);

        if (s.device->info().debug_layer)
            rhi::BackendAccess::d3d12_device(*s.device).QueryInterface(IID_PPV_ARGS(&s.debug));

        ATELIER_TRY(s.compile_core_pipelines());
        ATELIER_TRY(s.check_debug());
        return renderer;
    }();
    if (!build)
        build.error().terminal = false;
    return build;
}

Result<Renderer> Renderer::open(rhi::NativeWindow window, const Options& options) {
    if (!window)
        return std::unexpected(make_error(rhi::ErrorCode::invalid_argument, "Renderer requires an application window"));
    rhi::DeviceOptions dev_opts{};
    dev_opts.validation = options.validation;
    dev_opts.wait_timeout_ms = options.wait_timeout_ms;
    dev_opts.shader_cache = options.shader_cache;
    auto opened_dev = rhi::Device::open(dev_opts);
    if (!opened_dev)
        return std::unexpected(Error{opened_dev.error(), false});
    auto shared_dev = std::make_shared<rhi::Device>(std::move(*opened_dev));
    return open(std::move(shared_dev), window, options);
}

const std::shared_ptr<rhi::Device>& Renderer::device() const noexcept {
    static const std::shared_ptr<rhi::Device> empty{};
    return state_ ? state_->device : empty;
}

Result<Target> Renderer::target() const {
    if (!state_)
        return std::unexpected(make_error(rhi::ErrorCode::closed, "Renderer is closed"));
    ATELIER_TRY(state_->check());
    auto info = state_->surface.target();
    if (!info)
        return std::unexpected(Error{info.error(), true});
    return Target{{info->width, info->height}, info->drawable};
}

Result<Target> Renderer::sync_window_target() {
    if (!state_)
        return std::unexpected(make_error(rhi::ErrorCode::closed, "Renderer is closed"));
    ATELIER_TRY(state_->check());
    auto info = state_->surface.sync_target();
    if (!info)
        return std::unexpected(Error{info.error(), true});
    return Target{{info->width, info->height}, info->drawable};
}

Result<bool> Renderer::wait_frame(std::uint32_t timeout_ms) {
    if (!state_)
        return std::unexpected(make_error(rhi::ErrorCode::closed, "Renderer is closed"));
    ATELIER_TRY(state_->check());
    auto ready = state_->surface.wait_frame(timeout_ms);
    if (!ready)
        return std::unexpected(Error{ready.error(), true});
    return *ready;
}

Result<Preparation> Renderer::preparation(const ViewSettings& view) {
    if (!state_)
        return std::unexpected(make_error(rhi::ErrorCode::closed, "Renderer is closed"));
    ATELIER_TRY(state_->check());
    return state_->poll_lit_preparation(view);
}

Result<MaterialBatch> Renderer::prepare_materials(std::span<const material::ProgramSurfaceRef> surfaces) {
    if (!state_)
        return std::unexpected(make_error(rhi::ErrorCode::closed, "Renderer is closed"));
    ATELIER_TRY(state_->check());
    if (state_->options.ui_only) {
        return std::unexpected(
            make_error(rhi::ErrorCode::invalid_argument, "UI-only renderer cannot prepare 3D material batches"));
    }
    MaterialBatch batch;
    batch.state_ =
        std::make_unique<detail::MaterialBatchState>(state_.get(), state_->device, state_->live_material_batches);
    ATELIER_TRY(batch.state_->submit(surfaces, state_->archetype_cache, state_->program_set_cache,
                                     state_->masked_coverage_cache));
    return batch;
}

Result<Preparation> Renderer::preparation(MaterialBatch& batch) {
    if (!state_)
        return std::unexpected(make_error(rhi::ErrorCode::closed, "Renderer is closed"));
    ATELIER_TRY(state_->check());
    if (!batch.state_ || batch.state_->owner_renderer != state_.get()) {
        return std::unexpected(
            make_error(rhi::ErrorCode::invalid_argument, "Material batch belongs to another Renderer or is empty"));
    }
    auto& candidate = *batch.state_;
    if (candidate.failure)
        return std::unexpected(*candidate.failure);
    auto prep = candidate.program_set->preparation(*state_->device);
    if (!prep) {
        candidate.failure = prep.error();
        return prep;
    }
    if (prep->ready && !candidate.ready) {
        std::vector<std::pair<const material::Image*, assets::AtxRole>> newly_staged;
        bool upload_ok = false;
        struct RollbackGuard {
            State* st;

            detail::MaterialBatchState* cand;

            std::vector<std::pair<const material::Image*, assets::AtxRole>>* keys;

            bool* ok;

            ~RollbackGuard() {
                if (!*ok) {
                    cand->image_owners.clear();
                    if (cand->device && *cand->device) {
                        cand->surface_buffer.retire(*cand->device);
                        cand->parameter_buffer.retire(*cand->device);
                        cand->input_buffer.retire(*cand->device);
                    }
                    for (const auto& k : *keys)
                        st->material_images.erase(k);
                }
            }
        } rollback{state_.get(), &candidate, &newly_staged, &upload_ok};

        auto uploaded = [&]() -> Result<void> {
            struct AtxPrepJob {
                material::ImageRef image;

                assets::AtxRole role{};

                assets::AtxTextureRef cooked;
            };

            std::vector<AtxPrepJob> uncached_jobs;
            std::vector<std::size_t> req_to_job(candidate.image_requests.size(), SIZE_MAX);
            uncached_jobs.reserve(candidate.image_requests.size());
            for (std::size_t i = 0; i < candidate.image_requests.size(); ++i) {
                const auto& req = candidate.image_requests[i];
                if (!req.image)
                    continue;
                const auto key = std::pair{req.image.get(), req.role};
                if (state_->material_images.find(key) != state_->material_images.end())
                    continue;
                bool already_queued = false;
                for (const auto& existing : uncached_jobs) {
                    if (existing.image.get() == req.image.get() && existing.role == req.role) {
                        already_queued = true;
                        break;
                    }
                }
                if (!already_queued) {
                    req_to_job[i] = uncached_jobs.size();
                    uncached_jobs.push_back(AtxPrepJob{.image = req.image, .role = req.role, .cooked = {}});
                }
            }

            if (!uncached_jobs.empty()) {
                const std::filesystem::path tex_cache_dir =
                    state_->options.shader_cache.empty()
                        ? std::filesystem::path{}
                        : (state_->options.shader_cache.parent_path() / L"renderer-texture-cache");
                if (uncached_jobs.size() > 1u) {
                    std::atomic<std::size_t> next_job{0u};
                    std::atomic<bool> job_failed{false};
                    const auto hw_threads = std::max<std::size_t>(1u, std::thread::hardware_concurrency());
                    const std::size_t worker_count =
                        std::min(uncached_jobs.size(), std::min<std::size_t>(8u, hw_threads));
                    auto cook_worker = [&] {
                        for (;;) {
                            if (job_failed.load(std::memory_order_relaxed))
                                break;
                            const std::size_t j = next_job.fetch_add(1u, std::memory_order_relaxed);
                            if (j >= uncached_jobs.size())
                                break;
                            auto& job = uncached_jobs[j];
                            job.cooked = tex_cache_dir.empty()
                                             ? assets::cook_atx_image(*job.image, job.role)
                                             : assets::load_or_cook_atx_image(*job.image, job.role, tex_cache_dir);
                            if (!job.cooked)
                                job_failed.store(true, std::memory_order_relaxed);
                        }
                    };
                    std::vector<std::jthread> workers;
                    workers.reserve(worker_count - 1u);
                    [&]() noexcept {
                        for (std::size_t t = 1u; t < worker_count; ++t)
                            workers.emplace_back(cook_worker);
                    }();
                    cook_worker();
                } else {
                    auto& job = uncached_jobs[0];
                    job.cooked = tex_cache_dir.empty()
                                     ? assets::cook_atx_image(*job.image, job.role)
                                     : assets::load_or_cook_atx_image(*job.image, job.role, tex_cache_dir);
                }
                for (const auto& job : uncached_jobs) {
                    if (!job.cooked) {
                        return std::unexpected(
                            make_error(rhi::ErrorCode::invalid_argument, "Material texture cooking failed"));
                    }
                }
            }

            detail::UploadBatch shared_batch(*state_->device, false);
            std::vector<detail::ResolvedMaterialImage> resolved;
            resolved.reserve(candidate.image_requests.size());
            candidate.image_owners.reserve(candidate.image_requests.size());
            for (std::size_t i = 0; i < candidate.image_requests.size(); ++i) {
                const auto& req = candidate.image_requests[i];
                assets::AtxTextureRef precooked;
                if (req_to_job[i] != SIZE_MAX)
                    precooked = std::move(uncached_jobs[req_to_job[i]].cooked);
                ATELIER_TRY_BIND(img_res, state_->resolve_material_image(req.image, req.role, shared_batch,
                                                                         newly_staged, std::move(precooked)));
                std::uint32_t role_flags = 0u;
                if ((*img_res)->format == assets::AtxFormat::bc5_rg_unorm)
                    role_flags |= 0x10000u;
                else if ((*img_res)->format == assets::AtxFormat::bc4_r_unorm)
                    role_flags |= 0x20000u;
                if (req.role == assets::AtxRole::normal_bc5)
                    role_flags |= 0x40000u;
                resolved.push_back({(*img_res)->gpu.srv_bindless, role_flags, (*img_res)->toksvig_slope});
                candidate.image_owners.push_back(std::move(*img_res));
            }
            ATELIER_TRY(candidate.stage_upload(resolved, shared_batch));
            ATELIER_TRY(shared_batch.execute());

            std::vector<const material::Image*> to_evict;
            to_evict.reserve(candidate.image_requests.size());
            for (const auto& req : candidate.image_requests) {
                if (req.image && req.image->has_resident_pixels())
                    to_evict.push_back(req.image.get());
            }
            std::ranges::sort(to_evict);
            to_evict.erase(std::unique(to_evict.begin(), to_evict.end()), to_evict.end());
            if (to_evict.size() > 1u) {
                std::atomic<std::size_t> next_evict{0u};
                const auto hw_threads = std::max<std::size_t>(1u, std::thread::hardware_concurrency());
                const std::size_t worker_count = std::min(to_evict.size(), std::min<std::size_t>(8u, hw_threads));
                auto evict_worker = [&]() noexcept {
                    for (;;) {
                        const std::size_t idx = next_evict.fetch_add(1u, std::memory_order_relaxed);
                        if (idx >= to_evict.size())
                            break;
                        to_evict[idx]->evict_cold_pixels();
                    }
                };
                std::vector<std::jthread> workers;
                workers.reserve(worker_count - 1u);
                [&]() noexcept {
                    for (std::size_t t = 1u; t < worker_count; ++t)
                        workers.emplace_back(evict_worker);
                }();
                evict_worker();
            } else if (to_evict.size() == 1u) {
                to_evict[0]->evict_cold_pixels();
            }

            upload_ok = true;
            candidate.ready = true;
            state_->pending_material_uploads += newly_staged.size();
            return {};
        }();
        if (!uploaded) {
            Error e = std::move(uploaded).error();
            e.terminal = true;
            candidate.failure = e;
            state_->failure = e;
            return std::unexpected(std::move(e));
        }
    }
    return prep;
}

Result<FrameResult> Renderer::render(const Frame& frame, Capture* capture) {
    timing::Scope rendering{phases::render};
    const auto began =
        frame.stop_requested ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const auto destination = target();
    if (!destination)
        return std::unexpected(destination.error());

    auto& s = *state_;
    s.stats = FrameStats{};
    s.stats.cached_material_images = s.material_images.size();
    const auto extent = destination->extent;
    if (!destination->drawable || frame.ui_frame.extent != extent)
        return FrameResult{.accepted_extent = {}, .next_extent = extent, .stats = s.stats};

    const auto admitted = wait_frame(0u);
    if (!admitted)
        return std::unexpected(admitted.error());
    if (!*admitted)
        return FrameResult{
            .status = FrameStatus::not_ready, .accepted_extent = {}, .next_extent = extent, .stats = s.stats};

    bool encoding = false;
    auto attempt = [&]() -> CancellableResult<FrameResult> {
        std::optional<timing::Scope> stage{std::in_place, phases::prepare};
        ATELIER_TRY(check_stop(frame.stop_requested, "prepare"));

        if (frame.materials) {
            const auto* batch = frame.materials->state_.get();
            if (!batch || batch->owner_renderer != state_.get()) {
                return std::unexpected(
                    make_error(rhi::ErrorCode::invalid_argument, "Frame has an empty or foreign material batch"));
            }
            if (batch->failure)
                return std::unexpected(*batch->failure);
            if (!batch->ready) {
                return FrameResult{
                    .status = FrameStatus::not_ready, .accepted_extent = {}, .next_extent = extent, .stats = s.stats};
            }
        }

        ATELIER_TRY(validate_frame(frame, s.options.ui_only));
        if (!std::isfinite(frame.render_delta_seconds) || frame.render_delta_seconds < 0.f ||
            frame.render_delta_seconds > .1f) {
            return std::unexpected(
                make_error(rhi::ErrorCode::invalid_argument, "Render delta must be finite and in [0, .1] seconds"));
        }

        if (s.consecutive_occluded_frames > 0u) {
            auto still_occluded = rhi::BackendAccess::test_occluded(s.surface);
            if (!still_occluded)
                return std::unexpected(Error{still_occluded.error(), false});
            if (*still_occluded) {
                s.has_pick_target = false;
                if (s.consecutive_occluded_frames < 0xFFFFFFFFu)
                    ++s.consecutive_occluded_frames;
                if (s.consecutive_occluded_frames == 20u) {
                    for (auto& lc : s.lit_contexts)
                        lc->retire(*s.device);
                    s.lit_contexts.clear();
                    s.scene_depth.retire(*s.device);
                    s.pick_id_tex.retire(*s.device);
                }
                s.executor.release_transients();
                return FrameResult{
                    .status = FrameStatus::skipped, .accepted_extent = {}, .next_extent = extent, .stats = s.stats};
            }
            s.consecutive_occluded_frames = 0u;
        }

        const bool has_viewport = frame.scene_viewport.width > 0.f && frame.scene_viewport.height > 0.f;
        const bool draw_scene = !frame.meshes.empty() && has_viewport;
        const bool is_scene_lit =
            draw_scene && (frame.view.shading == Shading::scene_lit) && (frame.view.wireframe != Wireframe::only);

        if (is_scene_lit) {
            ATELIER_TRY_BIND(lit_prep, s.poll_lit_preparation(frame.view));
            if (!lit_prep->ready) {
                return FrameResult{
                    .status = FrameStatus::not_ready, .accepted_extent = {}, .next_extent = extent, .stats = s.stats};
            }
        }

        const auto grid_cam = prepare_grid_camera(frame);
        const auto cavity_proj = prepare_cavity_projection(frame);
        s.prepare_ui(frame);
        stage.reset();

        Capture pending_capture;
        if (capture) {
            pending_capture.frame_id = frame.ui_frame.id;
            pending_capture.extent = extent;
            pending_capture.rgba.resize(std::size_t(extent.width) * extent.height * 4u);
            pending_capture.depth.resize(std::size_t(extent.width) * extent.height, 0.0f);
            if (is_scene_lit)
                pending_capture.motion.resize(std::size_t(extent.width) * extent.height * 2u, 0.0f);
        }

        stage.emplace(phases::encode);
        encoding = true;

        const bool first_encode = std::exchange(s.first_encode, false);
        if (first_encode) {
            static_cast<void>(ATELIER_LOG(logging::Severity::info, "renderer", "renderer.first_encode_begin",
                                          "First encoding beginning: frame_id={} shading={}", frame.ui_frame.id,
                                          std::to_underlying(frame.view.shading)));
        }

        auto backbuffer = rhi::BackendAccess::acquire_backbuffer(s.surface);
        if (!backbuffer)
            return std::unexpected(Error{backbuffer.error(), true});

        const std::uint64_t seq = ++s.encode_sequence;
        const std::size_t slot_idx = s.slot_cursor % HostSlotRing<UiInstance>::slot_count;
        {
            auto slot_waited =
                rhi::BackendAccess::timeline(*s.device).wait_all(s.slot_fences[slot_idx], s.device->wait_timeout_ms());
            if (!slot_waited)
                return std::unexpected(Error{slot_waited.error(), true});
            ATELIER_TRY(s.record_completed_counters());
        }
        const detail::RasterViewport scene_rv =
            has_viewport ? detail::raster_viewport(frame.scene_viewport) : detail::RasterViewport{};

        // Lazy-compile optional viewport helper PSOs on first demand.
        if (has_viewport && frame.view.shading != Shading::scene_lit && frame.view.background == Background::gradient &&
            !s.background_pipeline) {
            ATELIER_TRY(s.compile_builtin_vs_ps(s.background_pipeline, "viewport_background", shaders::viewport_vs,
                                                shaders::background_ps, "main", BuiltinStyle::background));
        }
        if (grid_cam) {
            if (draw_scene && !s.grid_depth_pipeline) {
                ATELIER_TRY(s.compile_builtin_vs_ps(s.grid_depth_pipeline, "viewport_grid_depth", shaders::viewport_vs,
                                                    shaders::grid_ps, "main", BuiltinStyle::grid_depth));
            } else if (!draw_scene && !s.grid_pipeline) {
                ATELIER_TRY(s.compile_builtin_vs_ps(s.grid_pipeline, "viewport_grid", shaders::viewport_vs,
                                                    shaders::grid_empty_ps, "main", BuiltinStyle::grid));
            }
        }
        if (cavity_proj && !s.cavity_pipeline) {
            ATELIER_TRY(s.compile_builtin_vs_ps(s.cavity_pipeline, "studio_cavity", shaders::viewport_vs,
                                                shaders::cavity_ps, "main", BuiltinStyle::ui));
        }

        s.geometry_pool.begin_frame();
        detail::PersistentRenderScene& scene = s.render_scenes.acquire(*s.device, frame.scene_identity, seq);
        const detail::MaterialBatchState* mat_batch = frame.materials ? frame.materials->state_.get() : nullptr;
        State::LitContextState* lit_ctx = nullptr;

        if (draw_scene) {
            timing::Scope scene_stage{phases::encode_scene};
            ATELIER_TRY(s.ensure_scene_depth(extent));
            ATELIER_TRY(s.ensure_pick_target(extent));
            if (!is_scene_lit) {
                if (auto* existing_lit = s.find_lit_context(frame.scene_identity)) {
                    existing_lit->has_temporal_history = false;
                    existing_lit->has_prev_hzb = false;
                    existing_lit->has_froxel_history = false;
                }
                ATELIER_TRY(s.ensure_matcap());
            } else {
                lit_ctx = &s.acquire_lit_context(frame.scene_identity, seq);
                ATELIER_TRY(s.ensure_lit_targets(*lit_ctx, extent));
            }

            bool prep_cancelled = false;
            const auto masked_bin_cnt =
                static_cast<std::uint32_t>(mat_batch->program_set->masked_archetype_indices.size());
            const bool vsm_scene_active = is_scene_lit && frame.view.lit.shadows && !frame.lights.empty();
            ATELIER_TRY(s.render_scenes.update_scene(*s.device, scene, s.geometry_pool, frame, mat_batch->records,
                                                     masked_bin_cnt, slot_idx, is_scene_lit, vsm_scene_active, seq,
                                                     frame.stop_requested, prep_cancelled, s.stats));
            if (prep_cancelled)
                return std::unexpected(FrameCancelled{"prepare_scene"});
            ATELIER_TRY(s.geometry_pool.commit_frame(*s.device, slot_idx));
            s.geometry_pool.sweep_unused(*s.device, seq);
        } else {
            if (auto* existing_lit = s.find_lit_context(frame.scene_identity)) {
                existing_lit->has_temporal_history = false;
                existing_lit->has_prev_hzb = false;
                existing_lit->has_froxel_history = false;
            }
            scene.instance_count = 0u;
            scene.pending_delta_copies.clear();
            scene.pending_delta_src = nullptr;
            scene.pending_instances_copy = false;
            scene.pending_slot_map_copy = false;
            scene.tlas.retire(*s.device);
            scene.tlas_scratch.retire(*s.device);
            scene.tlas_valid = false;
            scene.tlas_gpu_built = false;
            scene.tlas_scratch_gpu_used = false;
            scene.pending_tlas_build = false;
        }

        // Sweep unreferenced MaterialImages:
        for (auto it = s.material_images.begin(); it != s.material_images.end();) {
            if (it->second.use_count() > 1)
                ++it;
            else
                it = s.material_images.erase(it);
        }
        s.stats.cached_material_images = s.material_images.size();
        s.stats.material_upload_count = std::exchange(s.pending_material_uploads, 0u);

        // Resolve UI textures & upload UI instance slot buffer:
        for (auto& [k, img] : s.ui_images) {
            (void)k;
            img.used = false;
        }
        std::uint32_t ui_instances_srv = rhi::bindless_invalid_index;
        {
            timing::Scope ui_stage{phases::encode_ui};
            if (!s.ui_instances.empty()) {
                for (const auto& [idx, item] : s.ui_textured) {
                    ATELIER_TRY_BIND(tex_srv, item->coverage ? s.resolve_ui_texture(item->coverage)
                                                             : s.resolve_ui_texture(item->image));
                    s.ui_instances[idx].shading |= (*tex_srv << 3u);
                }
                timing::Scope ui_upload{phases::encode_ui_upload};
                ATELIER_TRY_BIND(written_ui, s.ui_slots.write(*s.device, slot_idx, s.ui_instances));
                ui_instances_srv = written_ui->second;
                s.stats.overlay_shapes = s.overlay_instances;
            }
        }
        for (auto it = s.ui_images.begin(); it != s.ui_images.end();) {
            if (it->second.used) {
                ++it;
            } else {
                it = s.ui_images.erase(it);
            }
        }

        std::uint32_t grid_cb_srv = rhi::bindless_invalid_index;
        if (grid_cam) {
            GridConstants gc{};
            gc.inverse_view_projection = grid_cam->inverse;
            gc.view_projection = grid_cam->view_projection;
            gc.guides = {frame.view.reference_grid ? 1u : 0u, frame.view.origin_axes ? 1u : 0u};
            gc.grid_spacing = frame.view.grid_spacing;
            gc.grid_plane = static_cast<std::uint32_t>(frame.view.grid_plane);
            gc.camera_far_depth = frame.camera.far_depth;
            ATELIER_TRY_BIND(written_grid, s.grid_cb_slots.write(*s.device, slot_idx, std::span{&gc, 1u}));
            grid_cb_srv = written_grid->second;
        }

        std::uint32_t cavity_cb_srv = rhi::bindless_invalid_index;
        if (draw_scene && !is_scene_lit && cavity_proj) {
            CavityConstants cc{};
            cc.inverse_projection = *cavity_proj;
            cc.viewport = {frame.scene_viewport.x, frame.scene_viewport.y, frame.scene_viewport.width,
                           frame.scene_viewport.height};
            cc.controls = {frame.view.cavity.ridge, frame.view.cavity.valley, frame.view.cavity.radius, 0.f};
            ATELIER_TRY_BIND(written_cavity, s.cavity_cb_slots.write(*s.device, slot_idx, std::span{&cc, 1u}));
            cavity_cb_srv = written_cavity->second;
        }

        // Prepare Scene Lit lights, VSM page cache, SkyMap, ProbeVolumeStats, and GlobalFrameConstants:
        std::uint32_t lights_srv = rhi::bindless_invalid_index;
        std::uint32_t frame_cb_srv = rhi::bindless_invalid_index;
        detail::VsmFramePlan vsm_plan{};
        std::uint64_t masked_mat_hash = 0u;
        bool use_temporal_jitter = false;
        bool use_two_phase_hzb = false;
        bool use_lit_history = false;
        bool use_gi_history = false;
        bool use_ssilvb_history = false;
        bool use_refl_history = false;
        bool view_is_invertible_affine = false;
        bool enable_contact_shadows = false;
        bool fog_active = false;
        bool commit_lit_history = false;
        bool committed_ray_shadows = false;
        bool use_penumbra_history = false;
        std::uint64_t shadow_lights_hash = 0u;
        std::uint32_t max_opaque_clusters = 256u;
        std::uint32_t masked_stride = 1u;
        std::uint32_t max_selected_clusters = 1u;
        std::uint32_t max_cull_chunks = 64u;
        math::Matrix4 next_prev_unjittered_vp = math::Matrix4::identity();
        math::Matrix4 next_prev_view = math::Matrix4::identity();
        math::Matrix4 next_prev_projection = math::Matrix4::identity();
        std::array<float, 4> next_prev_scene_viewport{0.f, 0.f, 1.f, 1.f};
        std::array<float, 3> next_prev_camera_eye{0.f, 0.f, 0.f};
        float next_prev_exposure_scale = 1.0f;

        const int sc_left = std::clamp(static_cast<int>(scene_rv.scissor.left), 0, static_cast<int>(extent.width));
        const int sc_top = std::clamp(static_cast<int>(scene_rv.scissor.top), 0, static_cast<int>(extent.height));
        const int sc_right =
            std::clamp(static_cast<int>(scene_rv.scissor.right), sc_left, static_cast<int>(extent.width));
        const int sc_bottom =
            std::clamp(static_cast<int>(scene_rv.scissor.bottom), sc_top, static_cast<int>(extent.height));
        const std::uint32_t sc_gx0 = static_cast<std::uint32_t>(std::max(sc_left - 8, 0)) / 8u;
        const std::uint32_t sc_gy0 = static_cast<std::uint32_t>(std::max(sc_top - 8, 0)) / 8u;
        const std::uint32_t sc_gx1 = std::max(
            sc_gx0 + 1u,
            (static_cast<std::uint32_t>(std::min<int>(sc_right + 8, static_cast<int>(extent.width))) + 7u) / 8u);
        const std::uint32_t sc_gy1 = std::max(
            sc_gy0 + 1u,
            (static_cast<std::uint32_t>(std::min<int>(sc_bottom + 8, static_cast<int>(extent.height))) + 7u) / 8u);
        const std::uint32_t sc_gx_cnt = sc_gx1 - sc_gx0;
        const std::uint32_t sc_gy_cnt = sc_gy1 - sc_gy0;

        const std::uint32_t sc_tx0 = static_cast<std::uint32_t>(std::max(sc_left - 16, 0)) / 16u;
        const std::uint32_t sc_ty0 = static_cast<std::uint32_t>(std::max(sc_top - 16, 0)) / 16u;
        const std::uint32_t sc_tx1 = std::max(
            sc_tx0 + 1u,
            (static_cast<std::uint32_t>(std::min<int>(sc_right + 16, static_cast<int>(extent.width))) + 15u) / 16u);
        const std::uint32_t sc_ty1 = std::max(
            sc_ty0 + 1u,
            (static_cast<std::uint32_t>(std::min<int>(sc_bottom + 16, static_cast<int>(extent.height))) + 15u) / 16u);
        const std::uint32_t sc_tx_cnt = sc_tx1 - sc_tx0;
        const std::uint32_t sc_ty_cnt = sc_ty1 - sc_ty0;

        if (draw_scene) {
            const auto masked_cnt_fcb =
                static_cast<std::uint32_t>(mat_batch->program_set->masked_archetype_indices.size());
            max_opaque_clusters = std::clamp<std::uint32_t>(scene.total_opaque_meshlets + 64u, 256u, 0x3FFFFFu);
            if (masked_cnt_fcb > 0u) {
                const std::uint32_t masked_hi =
                    std::max(1u, std::min<std::uint32_t>(0x3FFFFFu, 0x7FFFFEu / masked_cnt_fcb));
                masked_stride = std::clamp<std::uint32_t>(scene.max_masked_bin_meshlets + 64u,
                                                          std::min(256u, masked_hi), masked_hi);
            } else {
                masked_stride = 1u;
            }
            max_selected_clusters = scene.any_selected
                                        ? std::clamp<std::uint32_t>(scene.total_selected_meshlets + 64u, 64u, 0x3FFFFFu)
                                        : 1u;
            max_cull_chunks = std::clamp<std::uint32_t>(scene.total_visible_chunks + 64u, 64u, 0x3FFFFFu);

            bool history_broken = true;
            std::uint32_t uploaded_dir_count = 0u;
            std::uint32_t primary_sun_idx = 0xFFFFu;
            if (is_scene_lit) {
                auto& lc = *lit_ctx;
                const bool want_sky_map = (frame.view.lit.sky_mode == rendering::SkyMode::hdri_sky_map) &&
                                          (frame.view.lit.sky_map != nullptr);
                ATELIER_TRY(s.ensure_sky_map(want_sky_map ? frame.view.lit.sky_map : rendering::SkyMapRef{}));

                const rendering::SkyMap* cur_sky_ptr = want_sky_map ? frame.view.lit.sky_map.get() : nullptr;
                const bool env_changed = (frame.environment[0] != lc.prev_environment[0]) ||
                                         (frame.environment[1] != lc.prev_environment[1]) ||
                                         (frame.environment[2] != lc.prev_environment[2]) ||
                                         (frame.view.lit.sky_mode != lc.prev_sky_mode) ||
                                         (std::abs(frame.view.lit.sky_rotation - lc.prev_sky_rotation) > 1e-5f) ||
                                         (cur_sky_ptr != lc.prev_sky_ptr);
                history_broken = !lc.has_temporal_history || frame.camera_cut ||
                                 (frame.view.lit.antialiasing != lc.prev_antialiasing) || env_changed;
                if (history_broken && s.any_lit_rendered)
                    ++s.stats.scene_lit_history_resets;

                use_temporal_jitter = (frame.view.lit.antialiasing == Antialiasing::taa);

                if (!frame.lights.empty()) {
                    std::vector<GpuLightRecord> gpu_lights;
                    gpu_lights.reserve(frame.lights.size());
                    std::uint32_t shadow_cnt = 0u;
                    float best_sun_lum = 0.0f;

                    auto normalize_3d = [](double x, double y, double z) noexcept -> std::array<double, 3> {
                        const double m = std::max({std::abs(x), std::abs(y), std::abs(z)});
                        if (!(m > 0.0) || !std::isfinite(m))
                            return {0.0, 0.0, 1.0};
                        const double sx = x / m;
                        const double sy = y / m;
                        const double sz = z / m;
                        const double len = std::sqrt(sx * sx + sy * sy + sz * sz);
                        return (len > 0.0) ? std::array<double, 3>{sx / len, sy / len, sz / len}
                                           : std::array<double, 3>{0.0, 0.0, 1.0};
                    };

                    auto pack_gpu_light = [&](const Light& l) -> GpuLightRecord {
                        GpuLightRecord gl{};
                        gl.position = {l.position[0], l.position[1], l.position[2]};
                        gl.kind_and_flags = (static_cast<std::uint32_t>(l.kind) & 3u) |
                                            (l.two_sided ? (1u << 2u) : 0u) | (l.casts_shadow ? (1u << 3u) : 0u);
                        std::array<double, 3> dir_d{l.direction[0], l.direction[1], l.direction[2]};
                        if (l.kind == LightKind::directional || l.kind == LightKind::rectangle ||
                            l.kind == LightKind::spot)
                            dir_d = normalize_3d(dir_d[0], dir_d[1], dir_d[2]);
                        gl.direction = {static_cast<float>(dir_d[0]), static_cast<float>(dir_d[1]),
                                        static_cast<float>(dir_d[2])};
                        gl.range = l.range;
                        gl.color_intensity = {l.color[0] * l.intensity, l.color[1] * l.intensity,
                                              l.color[2] * l.intensity};
                        gl.source_radius = l.source_radius;
                        std::array<double, 3> right_d{l.right[0], l.right[1], l.right[2]};
                        std::array<double, 3> up_d{l.up[0], l.up[1], l.up[2]};
                        if (l.kind == LightKind::rectangle) {
                            right_d = normalize_3d(right_d[0], right_d[1], right_d[2]);
                            const double rd = right_d[0] * dir_d[0] + right_d[1] * dir_d[1] + right_d[2] * dir_d[2];
                            right_d = normalize_3d(right_d[0] - dir_d[0] * rd, right_d[1] - dir_d[1] * rd,
                                                   right_d[2] - dir_d[2] * rd);
                            up_d = normalize_3d(up_d[0], up_d[1], up_d[2]);
                            const double ud = up_d[0] * dir_d[0] + up_d[1] * dir_d[1] + up_d[2] * dir_d[2];
                            const double ur = up_d[0] * right_d[0] + up_d[1] * right_d[1] + up_d[2] * right_d[2];
                            up_d = normalize_3d(up_d[0] - dir_d[0] * ud - right_d[0] * ur,
                                                up_d[1] - dir_d[1] * ud - right_d[1] * ur,
                                                up_d[2] - dir_d[2] * ud - right_d[2] * ur);
                        }
                        gl.right = {static_cast<float>(right_d[0]), static_cast<float>(right_d[1]),
                                    static_cast<float>(right_d[2])};
                        gl.up = {static_cast<float>(up_d[0]), static_cast<float>(up_d[1]), static_cast<float>(up_d[2])};
                        if (l.kind == LightKind::spot) {
                            const float cos_outer = std::cos(l.spot_outer_radians);
                            const float cos_inner = std::cos(l.spot_inner_radians);
                            gl.width = cos_outer;
                            gl.height = 1.0f / std::max(cos_inner - cos_outer, 1e-4f);
                        } else {
                            gl.width = l.width;
                            gl.height = l.height;
                        }
                        return gl;
                    };

                    std::vector<std::uint64_t> gpu_light_keys;
                    gpu_light_keys.reserve(frame.lights.size());
                    auto make_light_key = [](const Light& l, std::size_t orig_idx) noexcept -> std::uint64_t {
                        return (l.stable_key != 0u) ? l.stable_key
                                                    : (0x8000000000000000ull | static_cast<std::uint64_t>(orig_idx));
                    };

                    // Partition stably: [0 .. dir_light_count) directional lights, [dir_light_count .. light_count) local lights
                    for (std::size_t orig_i = 0u; orig_i < frame.lights.size(); ++orig_i) {
                        const auto& l = frame.lights[orig_i];
                        if (l.kind != LightKind::directional)
                            continue;
                        if (l.casts_shadow) {
                            ++shadow_cnt;
                            const float lum =
                                (0.2126f * l.color[0] + 0.7152f * l.color[1] + 0.0722f * l.color[2]) * l.intensity;
                            if (lum > best_sun_lum && gpu_lights.size() < 0xFFFFu) {
                                best_sun_lum = lum;
                                primary_sun_idx = static_cast<std::uint32_t>(gpu_lights.size());
                            }
                        }
                        gpu_lights.push_back(pack_gpu_light(l));
                        gpu_light_keys.push_back(make_light_key(l, orig_i));
                    }
                    uploaded_dir_count = static_cast<std::uint32_t>(gpu_lights.size());
                    for (std::size_t orig_i = 0u; orig_i < frame.lights.size(); ++orig_i) {
                        const auto& l = frame.lights[orig_i];
                        if (l.kind == LightKind::directional)
                            continue;
                        if (l.casts_shadow)
                            ++shadow_cnt;
                        gpu_lights.push_back(pack_gpu_light(l));
                        gpu_light_keys.push_back(make_light_key(l, orig_i));
                    }

                    if (!mat_batch->program_set->masked_archetype_indices.empty()) {
                        masked_mat_hash = 0xcbf29ce484222325ull;
                        auto mix_u64 = [&](std::uint64_t v) noexcept {
                            masked_mat_hash ^= v;
                            masked_mat_hash *= 0x100000001b3ull;
                        };
                        for (std::size_t si = 0u; si < mat_batch->records.size(); ++si) {
                            const auto& sr = mat_batch->records[si];
                            if ((sr.flags & 1u) == 0u)
                                continue;
                            mix_u64(si);
                            mix_u64(reinterpret_cast<std::uintptr_t>(&mat_batch->surfaces[si]->program()));
                            mix_u64(sr.flags);
                            for (std::byte b_val : mat_batch->surfaces[si]->parameters())
                                mix_u64(static_cast<std::uint8_t>(b_val));
                            const std::uint32_t in_cnt =
                                (sr.input_base < mat_batch->inputs.size())
                                    ? (mat_batch->inputs[sr.input_base].input_count_and_role & 0xFFFFu)
                                    : 0u;
                            for (std::uint32_t k = 0u; k < in_cnt && sr.input_base + k < mat_batch->inputs.size();
                                 ++k) {
                                const auto& inp = mat_batch->inputs[sr.input_base + k];
                                mix_u64(inp.texture_srv);
                                mix_u64(inp.address_mode);
                                mix_u64(inp.input_count_and_role);
                                for (std::size_t ch = 0u; ch < 4u; ++ch) {
                                    std::uint32_t ev_bits = 0u;
                                    std::memcpy(&ev_bits, &inp.empty_value[ch], sizeof(ev_bits));
                                    mix_u64(ev_bits);
                                }
                            }
                        }
                    }

                    ATELIER_TRY_ASSIGN(vsm_plan, lc.vsm_cache.prepare_frame(*s.device, slot_idx, seq, frame, scene,
                                                                            masked_mat_hash, gpu_lights, gpu_light_keys,
                                                                            uploaded_dir_count, primary_sun_idx));
                    s.stats.scene_lit_vsm_allocated_pages = vsm_plan.resident_atlas_cells;
                    s.stats.scene_lit_vsm_reused_pages = vsm_plan.reused_faces;
                    s.stats.scene_lit_vsm_static_renders = vsm_plan.static_updates;
                    s.stats.scene_lit_vsm_dynamic_renders = vsm_plan.dynamic_updates;
                    s.stats.scene_lit_vsm_cow_copies = static_cast<std::uint32_t>(vsm_plan.cow_copies.size());
                    s.stats.scene_lit_vsm_evictions = vsm_plan.evictions;

                    ATELIER_TRY_BIND(lb, s.light_slots.write(*s.device, slot_idx, gpu_lights));
                    lights_srv = lb->second;
                    s.stats.scene_lit_light_sources = static_cast<std::uint32_t>(frame.lights.size());
                    s.stats.scene_lit_shadow_sources = shadow_cnt;

                    shadow_lights_hash = 0xcbf29ce484222325ull;
                    auto mix_sl = [&](std::uint64_t v) noexcept {
                        shadow_lights_hash ^= v;
                        shadow_lights_hash *= 0x100000001b3ull;
                    };
                    auto mix_sl_f = [&](float f_val) noexcept {
                        std::uint32_t bits = 0u;
                        std::memcpy(&bits, &f_val, sizeof(bits));
                        mix_sl(bits);
                    };
                    for (std::size_t li = 0u; li < gpu_lights.size(); ++li) {
                        const auto& gl = gpu_lights[li];
                        if ((gl.kind_and_flags & 8u) == 0u)
                            continue;
                        mix_sl(gpu_light_keys[li]);
                        mix_sl(gl.kind_and_flags & 0xFFu);
                        mix_sl_f(gl.position[0]);
                        mix_sl_f(gl.position[1]);
                        mix_sl_f(gl.position[2]);
                        mix_sl_f(gl.direction[0]);
                        mix_sl_f(gl.direction[1]);
                        mix_sl_f(gl.direction[2]);
                        mix_sl_f(gl.range);
                        mix_sl_f(gl.source_radius);
                        mix_sl_f(gl.width);
                        mix_sl_f(gl.height);
                    }
                    use_penumbra_history = use_temporal_jitter && !history_broken && lc.has_penumbra_history &&
                                           (shadow_lights_hash == lc.prev_shadow_lights_hash);
                }
            }

            const std::uint32_t cur_frame_idx = ++s.frame_index;
            std::array<float, 2> jitter_px{0.0f, 0.0f};
            math::Matrix4 jittered_proj = frame.camera.projection;
            if (use_temporal_jitter) {
                const std::uint32_t j_sample = (cur_frame_idx & 7u) + 1u;
                jitter_px = {halton(j_sample, 2u) - 0.5f, halton(j_sample, 3u) - 0.5f};
                const float j_ndc_x = (2.0f * jitter_px[0]) / std::max(frame.scene_viewport.width, 1.0f);
                const float j_ndc_y = (-2.0f * jitter_px[1]) / std::max(frame.scene_viewport.height, 1.0f);
                if (jittered_proj[11] != 0.0f) {
                    jittered_proj[8] += j_ndc_x * jittered_proj[11];
                    jittered_proj[9] += j_ndc_y * jittered_proj[11];
                } else {
                    jittered_proj[12] += j_ndc_x * jittered_proj[15];
                    jittered_proj[13] += j_ndc_y * jittered_proj[15];
                }
            }

            const math::Matrix4d view_d(frame.camera.view);
            const math::Matrix4d proj_d(jittered_proj);
            const math::Matrix4d unjit_proj_d(frame.camera.projection);
            const math::Matrix4d vp_d = view_d * proj_d;
            const math::Matrix4d unjit_vp_d = view_d * unjit_proj_d;

            const auto inv_view = math::try_inverse(view_d);
            const auto inv_proj = math::try_inverse(proj_d);
            const auto inv_vp = math::try_inverse(vp_d);
            const auto inv_unjit_vp = math::try_inverse(unjit_vp_d);
            view_is_invertible_affine = inv_view.has_value() && (frame.camera.view[3] == 0.0f) &&
                                        (frame.camera.view[7] == 0.0f) && (frame.camera.view[11] == 0.0f) &&
                                        (frame.camera.view[15] == 1.0f);

            GlobalFrameConstants fcb{};
            fcb.view = frame.camera.view.elements;
            fcb.inv_view = inv_view ? math::Matrix4(*inv_view).elements : math::Matrix4::identity().elements;
            fcb.projection = jittered_proj.elements;
            fcb.inv_projection = inv_proj ? math::Matrix4(*inv_proj).elements : math::Matrix4::identity().elements;
            fcb.view_projection = math::Matrix4(vp_d).elements;
            fcb.inv_view_projection = inv_vp ? math::Matrix4(*inv_vp).elements : math::Matrix4::identity().elements;
            fcb.unjittered_view_projection = math::Matrix4(unjit_vp_d).elements;
            fcb.inv_unjittered_vp =
                inv_unjit_vp ? math::Matrix4(*inv_unjit_vp).elements : math::Matrix4::identity().elements;
            fcb.prev_unjittered_vp = (lit_ctx && lit_ctx->has_temporal_history) ? lit_ctx->prev_unjittered_vp.elements
                                                                                : fcb.unjittered_view_projection;
            fcb.prev_view = (lit_ctx && lit_ctx->has_prev_hzb) ? lit_ctx->prev_view.elements : fcb.view;
            fcb.prev_projection = (lit_ctx && lit_ctx->has_prev_hzb) ? lit_ctx->prev_projection.elements
                                                                     : frame.camera.projection.elements;

            fcb.raster_viewport_transform = scene_rv.clip_transform;
            fcb.scene_viewport = {frame.scene_viewport.x, frame.scene_viewport.y, frame.scene_viewport.width,
                                  frame.scene_viewport.height};
            fcb.prev_scene_viewport =
                (lit_ctx && lit_ctx->has_temporal_history) ? lit_ctx->prev_scene_viewport : fcb.scene_viewport;
            fcb.scissor_rect = {scene_rv.scissor.left, scene_rv.scissor.top, scene_rv.scissor.right,
                                scene_rv.scissor.bottom};

            const math::Vector3 eye = extract_camera_eye(frame.camera.view);
            fcb.camera_eye = {eye[0], eye[1], eye[2]};
            fcb.camera_far_depth = frame.camera.far_depth;
            fcb.environment_radiance = {frame.environment[0], frame.environment[1], frame.environment[2]};
            fcb.sky_rotation_rad = frame.view.lit.sky_rotation * (std::numbers::pi_v<float> / 180.0f);
            fcb.render_extent = {extent.width, extent.height};
            fcb.output_extent = {extent.width, extent.height};
            fcb.jitter_pixels = jitter_px;
            fcb.exposure_scale = std::clamp(std::exp2(frame.view.lit.exposure_ev), 1e-6f, 1e6f);
            fcb.indirect_gain = frame.view.lit.indirect_gain;
            fcb.ao_radius = frame.view.lit.ao_radius;
            fcb.reflection_distance = frame.view.lit.reflection_distance;
            fcb.reflection_roughness = frame.view.lit.reflection_roughness;
            const bool history_ok = is_scene_lit && !history_broken;
            fcb.exposure_ratio = (history_ok && lit_ctx && lit_ctx->prev_exposure_scale > 0.0f)
                                     ? (fcb.exposure_scale / lit_ctx->prev_exposure_scale)
                                     : 1.0f;
            // Contact shadow active on multi-object outdoor/sun scenes; 0 on analytic single-light verification gates:
            enable_contact_shadows = is_scene_lit && frame.view.lit.shadows && (primary_sun_idx != 0xFFFFu) &&
                                     (frame.meshes.size() > 2u) && (frame.view.lit.contact_shadow_length > 0.0f);
            fcb.contact_shadow_length = enable_contact_shadows ? frame.view.lit.contact_shadow_length : 0.0f;
            fcb.render_delta_seconds = frame.render_delta_seconds;
            fcb.frame_index = cur_frame_idx;

            const bool temporal_aa_active = is_scene_lit && use_temporal_jitter && history_ok;
            const bool sky_map_active = is_scene_lit && (frame.view.lit.sky_mode == rendering::SkyMode::hdri_sky_map) &&
                                        s.cached_sky_map.has_value();
            use_lit_history = history_ok;
            use_gi_history =
                history_ok && frame.view.lit.indirect_lighting && lit_ctx && lit_ctx->prev_indirect_lighting;
            use_ssilvb_history = history_ok && frame.view.lit.ssilvb && lit_ctx && lit_ctx->prev_ssilvb;
            use_refl_history = history_ok && frame.view.lit.reflections && lit_ctx && lit_ctx->prev_reflections;
            use_two_phase_hzb = history_ok && lit_ctx && lit_ctx->has_prev_hzb;
            fcb.flags = (frame.view.lit.shadows ? (1u << 0u) : 0u) |
                        (frame.view.lit.indirect_lighting ? (1u << 1u) : 0u) |
                        (frame.view.lit.ssilvb ? (1u << 2u) : 0u) | (frame.view.lit.reflections ? (1u << 3u) : 0u) |
                        (frame.view.lit.bloom ? (1u << 4u) : 0u) | (sky_map_active ? (1u << 5u) : 0u) |
                        (history_ok ? (1u << 7u) : 0u) |
                        ((frame.view.lit.tonemapper == rendering::Tonemapper::agx) ? (1u << 8u) : 0u) |
                        (frame.view.backface_tint ? (1u << 9u) : 0u) | (frame.view.triangle_edges ? (1u << 10u) : 0u) |
                        (temporal_aa_active ? (1u << 11u) : 0u);

            fcb.shading_mode = static_cast<std::uint32_t>(frame.view.shading);
            fcb.wireframe_mode = static_cast<std::uint32_t>(frame.view.wireframe);
            fcb.wire_style = {frame.view.wire_width, frame.view.wire_opacity};

            fcb.mesh_table_srv = s.geometry_pool.mesh_table_srv();
            fcb.part_tri_base_srv = s.geometry_pool.part_tri_base_srv();
            fcb.instances_srv = scene.instances_srv;
            fcb.selected_mask_srv = scene.selected_mask_srv;
            fcb.material_slot_map_srv = scene.material_slot_map_srv;
            fcb.material_surfaces_srv = mat_batch->surface_buffer.srv_bindless;
            fcb.material_params_srv = mat_batch->parameter_buffer.srv_bindless;
            fcb.material_inputs_srv = mat_batch->input_buffer.srv_bindless;
            fcb.lights_srv = lights_srv;
            fcb.light_count = is_scene_lit ? static_cast<std::uint32_t>(frame.lights.size()) : 0u;
            fcb.dir_light_count = is_scene_lit ? uploaded_dir_count : 0u;
            const std::uint32_t gi_cands = std::clamp<std::uint32_t>(frame.view.lit.direct_candidates, 1u, 32u);
            fcb.primary_sun_and_gi_cands =
                ((is_scene_lit ? primary_sun_idx : 0xFFFFu) & 0xFFFFu) | ((gi_cands & 0xFFFFu) << 16u);
            fcb.tlas_srv = (is_scene_lit && scene.tlas.as) ? scene.tlas.srv_bindless : 0xFFFFFFFFu;
            fcb.sky_map_srv = sky_map_active ? s.cached_sky_map->gpu.srv_bindless : 0xFFFFFFFFu;
            fcb.sky_scale = sky_map_active ? s.cached_sky_map->scale : 1.0f;
            fcb.matcap_srv = s.matcap.srv_bindless;

            if (is_scene_lit) {
                fcb.ltc_matrix_srv = s.lighting_luts.ltc_matrix.srv_bindless;
                fcb.ltc_amp_srv = s.lighting_luts.ltc_amplitude.srv_bindless;
                fcb.charlie_lut_srv = s.lighting_luts.charlie_lut.srv_bindless;
                fcb.atmos_trans_srv = s.lighting_luts.atmos_transmittance.srv_bindless;
                fcb.atmos_multiscatter_srv = s.lighting_luts.atmos_multiscatter.srv_bindless;
                fcb.smaa_area_srv = s.lighting_luts.smaa_area.srv_bindless;
                fcb.smaa_search_srv = s.lighting_luts.smaa_search.srv_bindless;
            }
            fcb.instance_count = scene.instance_count;
            fcb.archetype_count = static_cast<std::uint32_t>(mat_batch->program_set->archetypes.size());
            fcb.masked_archetype_count =
                static_cast<std::uint32_t>(mat_batch->program_set->masked_archetype_indices.size());
            fcb.gi_rays_per_probe = (frame.view.lit.gi_quality == rendering::GiQuality::ultra) ? 64u : 36u;
            fcb.wrc_voxel_keys_srv = is_scene_lit ? lit_ctx->wrc_voxel_keys.srv_bindless : 0xFFFFFFFFu;
            fcb.max_opaque_clusters = max_opaque_clusters;
            fcb.masked_cluster_stride = masked_stride;
            fcb.max_selected_clusters = max_selected_clusters;
            fcb.max_cull_chunks = max_cull_chunks;

            if (sky_map_active && frame.view.lit.sky_map) {
                const auto& sh = frame.view.lit.sky_map->sh3();
                for (std::size_t k = 0u; k < 9u; ++k) {
                    fcb.sky_sh3[k] = {
                        sh[k][0] * frame.environment[0],
                        sh[k][1] * frame.environment[1],
                        sh[k][2] * frame.environment[2],
                        0.0f,
                    };
                }
            }

            fcb.lighting_focus = {frame.lighting_focus[0], frame.lighting_focus[1], frame.lighting_focus[2]};
            fcb.prev_camera_eye = (history_ok && lit_ctx) ? lit_ctx->prev_camera_eye : fcb.camera_eye;
            if (is_scene_lit) {
                auto& lc = *lit_ctx;
                const std::uint32_t fcb_cur_p = lc.history_parity & 1u;
                const std::uint32_t fcb_prev_p = 1u - fcb_cur_p;
                fcb.fog_density = frame.view.lit.fog_enabled ? frame.view.lit.fog_density : 0.0f;
                fcb.fog_height_falloff = frame.view.lit.fog_height_falloff;
                fcb.fog_anisotropy = frame.view.lit.fog_anisotropy;
                if (fcb.fog_density > 0.0f)
                    ATELIER_TRY(s.ensure_froxel_targets(lc));
                fcb.hzb_srv = lc.hzb_pyramid.srv_bindless;
                fcb.prev_view_z_srv = history_ok ? lc.unjit_depth_history[fcb_prev_p].srv_bindless : 0xFFFFFFFFu;
                fcb.wrc_curr_srv = lc.wrc_slots[fcb_cur_p].srv_bindless;
                fcb.wrc_hash_keys_srv = lc.wrc_hash_keys.srv_bindless;
                fcb.wrc_hash_keys_uav = lc.wrc_hash_keys.uav_bindless;
                fcb.froxel_srv =
                    (fcb.fog_density > 0.0f && lc.froxel_lut.texture) ? lc.froxel_lut.srv_bindless : 0xFFFFFFFFu;
                fog_active = (fcb.fog_density > 0.0f && lc.froxel_lut.texture != nullptr);
                fcb.env_sky_mode = static_cast<std::uint32_t>(frame.view.lit.sky_mode);
                if (vsm_plan.active) {
                    fcb.vsm_static_srv = lc.vsm_cache.static_pool().srv_bindless;
                    fcb.vsm_dynamic_srv = lc.vsm_cache.dynamic_pool().srv_bindless;
                    fcb.vsm_page_table_srv = vsm_plan.page_table_srv;
                    fcb.vsm_views_srv = vsm_plan.views_srv;
                }
                if (frame.view.lit.indirect_lighting) {
                    fcb.gi_probe_atlas_srv = lc.probe_atlas[fcb_cur_p].srv_bindless;
                    fcb.gi_probe_mask_srv = lc.probe_mask[fcb_cur_p].srv_bindless;
                }
                fcb.scene_tile8_offset_packed = (sc_gx0 & 0xFFFFu) | ((sc_gy0 & 0xFFFFu) << 16u);
                fcb.scene_tile16_offset_packed = (sc_tx0 & 0xFFFFu) | ((sc_ty0 & 0xFFFFu) << 16u);
            }

            ATELIER_TRY_BIND(written_fcb, s.frame_cb_slots.write(*s.device, slot_idx, std::span{&fcb, 1u}));
            frame_cb_srv = written_fcb->second;

            if (is_scene_lit) {
                commit_lit_history = true;
                next_prev_unjittered_vp = math::Matrix4(unjit_vp_d);
                next_prev_view = frame.camera.view;
                next_prev_projection = jittered_proj;
                next_prev_scene_viewport = fcb.scene_viewport;
                next_prev_camera_eye = fcb.camera_eye;
                next_prev_exposure_scale = fcb.exposure_scale;
            }
        }

        // ====================================================================
        // Build RDG!
        // ====================================================================
        s.rdg_arena.reset();
        rdg::Graph graph(s.rdg_arena);

        auto import_persistent = [&](const char* name, OwnedTexture& tex, rdg::TextureState final_st,
                                     bool discard) -> rdg::TextureHandle {
            const bool used_before =
                (tex.state.stages != nri::StageBits::NONE) || (tex.state.layout != nri::Layout::GENERAL);
            const rdg::TextureState init_st =
                discard ? rdg::TextureState{nri::AccessBits::NONE, nri::Layout::UNDEFINED,
                                            used_before ? nri::StageBits::ALL : nri::StageBits::NONE}
                        : tex.state;
            return graph.import_texture(name, tex.texture, init_st, final_st,
                                        {.rtv = tex.rtv,
                                         .dsv = tex.dsv,
                                         .dsv_readonly = tex.dsv_readonly,
                                         .srv_bindless = tex.srv_bindless,
                                         .uav_bindless = tex.uav_bindless,
                                         .mip_srv_bindless = tex.mip_srv_bindless,
                                         .mip_uav_bindless = tex.mip_uav_bindless},
                                        &tex.state);
        };

        rdg::ImportedTextureViews bb_views{};
        bb_views.rtv = backbuffer->rtv;
        const rdg::TextureHandle bb_handle =
            graph.import_texture("swapchain_backbuffer", backbuffer->texture,
                                 {nri::AccessBits::NONE, nri::Layout::UNDEFINED,
                                  backbuffer->ever_used ? nri::StageBits::ALL : nri::StageBits::NONE},
                                 {nri::AccessBits::NONE, nri::Layout::PRESENT, nri::StageBits::NONE}, bb_views);

        rdg::TextureHandle depth_handle{};
        rdg::TextureHandle pick_id_handle{};
        if (draw_scene) {
            constexpr rdg::TextureState copy_src_state{nri::AccessBits::COPY_SOURCE, nri::Layout::COPY_SOURCE,
                                                       nri::StageBits::COPY};
            depth_handle = import_persistent("scene_depth", s.scene_depth, copy_src_state, true);
            pick_id_handle = import_persistent("pick_id_buffer", s.pick_id_tex, copy_src_state, true);
        }

        auto set_scene_vp = [=](const rdg::PassContext& ctx) {
            const nri::Viewport vp{scene_rv.hardware.x,
                                   scene_rv.hardware.y,
                                   std::max(1.f, scene_rv.hardware.width),
                                   std::max(1.f, scene_rv.hardware.height),
                                   0.0f,
                                   1.0f,
                                   false};
            const int sc_w = std::max(0, scene_rv.scissor.right - scene_rv.scissor.left);
            const int sc_h = std::max(0, scene_rv.scissor.bottom - scene_rv.scissor.top);
            const nri::Rect sc{static_cast<std::int16_t>(std::max(0, scene_rv.scissor.left)),
                               static_cast<std::int16_t>(std::max(0, scene_rv.scissor.top)),
                               static_cast<nri::Dim_t>(sc_w), static_cast<nri::Dim_t>(sc_h)};
            ctx.core().CmdSetViewports(ctx.cmd(), &vp, 1u);
            ctx.core().CmdSetScissors(ctx.cmd(), &sc, 1u);
        };

        constexpr nri::ClearValue zero_color{.color = {.f = {0.0f, 0.0f, 0.0f, 0.0f}}};

        bool backbuffer_cleared = false;
        auto bb_load = [&]() noexcept {
            if (!backbuffer_cleared) {
                backbuffer_cleared = true;
                return nri::LoadOp::CLEAR;
            }
            return nri::LoadOp::LOAD;
        };

        // Pass 1: Clear Backbuffer & Optional Viewport Gradient Background
        graph.add_pass(
            "ViewportBackdrop", rhi::QueueKind::graphics,
            [&](rdg::PassBuilder& b) { b.render_target(bb_handle, bb_load(), nri::StoreOp::STORE, zero_color); },
            [=, &s, &frame](const rdg::PassContext& ctx) -> rhi::Result<void> {
                ctx.begin_rendering();
                if (has_viewport && frame.view.shading != Shading::scene_lit &&
                    frame.view.background == Background::gradient && scene_rv.scissor.right > scene_rv.scissor.left &&
                    scene_rv.scissor.bottom > scene_rv.scissor.top) {
                    set_scene_vp(ctx);
                    ctx.set_pipeline(s.background_pipeline);
                    ViewportRootConstants rc{scene_rv.clip_transform, 0u, 0u};
                    ctx.push_constants(rc, nri::BindPoint::GRAPHICS);
                    ctx.core().CmdDraw(ctx.cmd(), {3u, 1u, 0u, 0u});
                    ++s.stats.viewport_draw_calls;
                }
                ctx.end_rendering();
                return {};
            });

        rdg::TextureHandle lit_motion_handle{};
        ReadbackSlotRing<GpuLitReadbackCounters>::Slot* counter_rb_slot = nullptr;

        if (draw_scene) {
            const auto& prog_set = *mat_batch->program_set;
            const std::uint32_t arch_cnt =
                std::max<std::uint32_t>(1u, static_cast<std::uint32_t>(prog_set.archetypes.size()));
            const std::uint32_t masked_cnt = static_cast<std::uint32_t>(prog_set.masked_archetype_indices.size());
            const std::uint64_t total_px = std::uint64_t(extent.width) * extent.height;
            const std::uint32_t max_phase2_cands =
                use_two_phase_hzb
                    ? std::clamp<std::uint32_t>(max_opaque_clusters + masked_cnt * masked_stride, 256u, 0x7FFFFEu)
                    : 1u;

            constexpr rdg::TextureState cs_srv_state{nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE,
                                                     nri::StageBits::COMPUTE_SHADER};
            const rdg::TextureHandle hzb_handle =
                is_scene_lit ? import_persistent("hzb_pyramid", lit_ctx->hzb_pyramid, cs_srv_state, !use_two_phase_hzb)
                             : rdg::TextureHandle{};

            auto dispatch_grid_2d = [](std::uint32_t groups) noexcept -> nri::DispatchDesc {
                if (groups <= 65535u)
                    return {std::max(1u, groups), 1u, 1u};
                return {1024u, std::min<std::uint32_t>((groups + 1023u) / 1024u, 65535u), 1u};
            };

            // Transient VisBuffer & Parallel Chunk/Cluster Culling Buffers:
            const auto vis_tex = graph.create_texture(
                "visbuffer", {.format = nri::Format::R32_UINT, .width = extent.width, .height = extent.height});
            const auto opaque_clusters = graph.create_buffer(
                "opaque_clusters", {.size = std::uint64_t(max_opaque_clusters) * 8u, .structure_stride = 8u});
            const auto masked_clusters = graph.create_buffer(
                "masked_clusters",
                {.size = std::uint64_t(std::max(1u, masked_cnt)) * masked_stride * 8u, .structure_stride = 8u});
            const auto selected_clusters = graph.create_buffer(
                "selected_clusters", {.size = std::uint64_t(max_selected_clusters) * 8u, .structure_stride = 8u});
            const auto cull_chunks = graph.create_buffer(
                "cull_chunks", {.size = std::uint64_t(max_cull_chunks) * 8u, .structure_stride = 8u});
            const auto chunk_indirect_args =
                graph.create_buffer("chunk_indirect_args", {.size = 16u, .byte_address = true});
            const auto phase2_cands = graph.create_buffer(
                "phase2_cands", {.size = std::uint64_t(max_phase2_cands) * 8u, .structure_stride = 8u});
            const auto cull_counters = graph.create_buffer(
                "cull_counters", {.size = std::uint64_t(8u + 2u * masked_cnt) * 4u, .structure_stride = 4u});
            const auto mesh_indirect_args = graph.create_buffer(
                "mesh_indirect_args", {.size = std::uint64_t(3u + masked_cnt) * 12u, .byte_address = true});
            const auto phase2_cs_indirect =
                use_two_phase_hzb ? graph.create_buffer("phase2_cs_indirect", {.size = 16u, .byte_address = true})
                                  : rdg::BufferHandle{};
            const auto phase2_mesh_indirect =
                use_two_phase_hzb
                    ? graph.create_buffer("phase2_mesh_indirect",
                                          {.size = std::uint64_t(1u + masked_cnt) * 12u, .byte_address = true})
                    : rdg::BufferHandle{};

            const bool allow_cone_cull = view_is_invertible_affine &&
                                         !(frame.view.wireframe == Wireframe::only && !frame.view.hidden_lines) &&
                                         !(frame.view.backface_tint && !is_scene_lit);
            const std::uint32_t cull_mode_flags =
                (use_two_phase_hzb ? 1u : 0u) | (scene.any_selected ? 2u : 0u) | (allow_cone_cull ? 4u : 0u);

            // 1a. Phase 1 Parallel Instance -> 64-Meshlet Chunk Expansion & Frustum + PrevHZB Culling
            graph.add_pass(
                is_scene_lit ? "LitVisCull" : "StudioVisCull", rhi::QueueKind::graphics,
                [&](rdg::PassBuilder& b) {
                    b.write_buffer(cull_chunks);
                    b.write_buffer(chunk_indirect_args);
                    b.write_buffer(cull_counters);
                    if (!scene.pending_delta_copies.empty())
                        b.mark_side_effect();
                },
                [=, &s, &scene](const rdg::PassContext& ctx) -> rhi::Result<void> {
                    detail::RenderSceneTable::record_scene_uploads(*s.device, ctx.cmd(), scene);
                    ctx.set_pipeline(s.vis_cull_cs);

                    CullRootConsts rc{};
                    rc.frame_cb_srv = frame_cb_srv;
                    rc.chunks_uav = ctx.uav(cull_chunks);
                    rc.counters_uav = ctx.uav(cull_counters);
                    rc.aux_indirect_uav = ctx.uav(chunk_indirect_args);

                    nri::GlobalBarrierDesc uav_bar{
                        .before = {nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER},
                        .after = {nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER}};
                    nri::BarrierDesc bg{.globals = &uav_bar,
                                        .globalNum = 1u,
                                        .buffers = nullptr,
                                        .bufferNum = 0u,
                                        .textures = nullptr,
                                        .textureNum = 0u};

                    // Mode 0: Clear counters
                    rc.phase_mode = 0u;
                    ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                    ctx.core().CmdDispatch(ctx.cmd(), {(6u + 2u * masked_cnt + 63u) / 64u, 1u, 1u});
                    ctx.core().CmdBarrier(ctx.cmd(), bg);

                    // Mode 1: Expand frustum-visible instances into 64-meshlet chunks (with G1 LOD selection)
                    rc.phase_mode = 1u;
                    ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                    ctx.core().CmdDispatch(ctx.cmd(), dispatch_grid_2d((scene.instance_count + 63u) / 64u));
                    ctx.core().CmdBarrier(ctx.cmd(), bg);

                    // Mode 5: Clamp chunk count & write 2D indirect dispatch arguments for Mode 6
                    rc.phase_mode = 5u;
                    ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                    ctx.core().CmdDispatch(ctx.cmd(), {1u, 1u, 1u});
                    return {};
                });

            graph.add_pass(
                is_scene_lit ? "LitVisCullChunks" : "StudioVisCullChunks", rhi::QueueKind::graphics,
                [&](rdg::PassBuilder& b) {
                    if (use_two_phase_hzb) {
                        b.read_texture(hzb_handle);
                        b.write_buffer(phase2_cs_indirect);
                    }
                    b.read_buffer(cull_chunks);
                    b.read_buffer(chunk_indirect_args, nri::AccessBits::ARGUMENT_BUFFER, nri::StageBits::INDIRECT);
                    b.write_buffer(opaque_clusters);
                    b.write_buffer(masked_clusters);
                    b.write_buffer(selected_clusters);
                    b.write_buffer(phase2_cands);
                    b.write_buffer(cull_counters);
                    b.write_buffer(mesh_indirect_args);
                },
                [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                    ctx.set_pipeline(s.vis_cull_cs);

                    CullRootConsts rc{};
                    rc.frame_cb_srv = frame_cb_srv;
                    rc.opaque_clusters_uav = ctx.uav(opaque_clusters);
                    rc.masked_clusters_uav = ctx.uav(masked_clusters);
                    rc.selected_clusters_uav = ctx.uav(selected_clusters);
                    rc.chunks_uav = ctx.srv(cull_chunks);
                    rc.phase2_candidates_uav = ctx.uav(phase2_cands);
                    rc.counters_uav = ctx.uav(cull_counters);
                    rc.indirect_args_uav = ctx.uav(mesh_indirect_args);
                    rc.aux_indirect_uav = use_two_phase_hzb ? ctx.uav(phase2_cs_indirect) : 0xFFFFFFFFu;
                    rc.hzb_src_srv = use_two_phase_hzb ? ctx.srv(hzb_handle) : 0xFFFFFFFFu;
                    rc.hzb_src_w = lit_ctx ? lit_ctx->hzb_pyramid.width : 1u;
                    rc.hzb_src_h = lit_ctx ? lit_ctx->hzb_pyramid.height : 1u;
                    rc.flags_and_dst_h = cull_mode_flags;

                    // Mode 6: Parallel 1-thread-per-meshlet exact 6-plane frustum + PrevHZB culling
                    rc.phase_mode = 6u;
                    ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                    ctx.core().CmdDispatchIndirect(ctx.cmd(), *ctx.buffer(chunk_indirect_args), 0u);

                    nri::GlobalBarrierDesc uav_bar{
                        .before = {nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER},
                        .after = {nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER}};
                    nri::BarrierDesc bg{.globals = &uav_bar,
                                        .globalNum = 1u,
                                        .buffers = nullptr,
                                        .bufferNum = 0u,
                                        .textures = nullptr,
                                        .textureNum = 0u};
                    ctx.core().CmdBarrier(ctx.cmd(), bg);

                    // Mode 3: Clamp counters & finalize Phase 1 DrawMeshTasksDesc (+ Phase 2 CS indirect args)
                    rc.phase_mode = 3u;
                    ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                    ctx.core().CmdDispatch(ctx.cmd(), {1u, 1u, 1u});
                    return {};
                });

            // 2. Phase 1 VisBuffer Mesh Shader Rasterization (1 Opaque PSO + M Masked Archetype PSOs!)
            graph.add_pass(
                is_scene_lit ? "LitVisRaster" : "StudioVisRaster", rhi::QueueKind::graphics,
                [&](rdg::PassBuilder& b) {
                    b.read_buffer(opaque_clusters, nri::AccessBits::SHADER_RESOURCE, nri::StageBits::MESH_SHADER);
                    b.read_buffer(masked_clusters, nri::AccessBits::SHADER_RESOURCE, nri::StageBits::MESH_SHADER);
                    b.read_buffer(cull_counters, nri::AccessBits::SHADER_RESOURCE, nri::StageBits::MESH_SHADER);
                    b.read_buffer(mesh_indirect_args, nri::AccessBits::ARGUMENT_BUFFER, nri::StageBits::INDIRECT);
                    b.render_target(vis_tex, nri::LoadOp::CLEAR, nri::StoreOp::STORE, zero_color);
                    b.depth_target(depth_handle, nri::LoadOp::CLEAR, nri::StoreOp::STORE, 0.0f, false);
                },
                [=, &s, &prog_set](const rdg::PassContext& ctx) -> rhi::Result<void> {
                    ctx.begin_rendering();
                    if (scene_rv.scissor.right > scene_rv.scissor.left &&
                        scene_rv.scissor.bottom > scene_rv.scissor.top) {
                        set_scene_vp(ctx);

                        ctx.set_pipeline(s.vis_opaque_ms);
                        ArchetypeRootConsts rc{};
                        rc.frame_cb_srv = frame_cb_srv;
                        rc.material_inputs_srv = mat_batch->input_buffer.srv_bindless;
                        rc.material_params_srv = mat_batch->parameter_buffer.srv_bindless;
                        rc.material_surfaces_srv = mat_batch->surface_buffer.srv_bindless;
                        rc.bin_offset_or_cluster_srv = ctx.srv(opaque_clusters);
                        rc.aux0_srv = ctx.srv(cull_counters);
                        rc.viewport_packed0 = 0u;
                        ctx.push_constants(rc, nri::BindPoint::GRAPHICS);
                        ctx.mesh_shader().CmdDrawMeshTasksIndirect(ctx.cmd(), *ctx.buffer(mesh_indirect_args), 0u, 1u,
                                                                   sizeof(nri::DrawMeshTasksDesc), nullptr, 0u);
                        ++s.stats.scene_draw_calls;

                        for (std::uint32_t m = 0u; m < masked_cnt; ++m) {
                            const std::uint32_t arch_idx = prog_set.masked_archetype_indices[m];
                            const auto& arch_pso = prog_set.archetypes[arch_idx]->pipelines[static_cast<std::size_t>(
                                detail::ProgramArchetype::Stage::masked_vis_ms)];
                            ctx.set_pipeline(arch_pso);
                            rc.archetype_index = m;
                            rc.bin_offset_or_cluster_srv = ctx.srv(masked_clusters);
                            rc.pass_flags = masked_stride;
                            rc.viewport_packed0 = 0u;
                            ctx.push_constants(rc, nri::BindPoint::GRAPHICS);
                            ctx.mesh_shader().CmdDrawMeshTasksIndirect(ctx.cmd(), *ctx.buffer(mesh_indirect_args),
                                                                       std::uint64_t(1u + m) * 12u, 1u,
                                                                       sizeof(nri::DrawMeshTasksDesc), nullptr, 0u);
                            ++s.stats.scene_draw_calls;
                        }
                    }
                    ctx.end_rendering();
                    return {};
                });

            // 2b. Scene Lit HZB Pyramid Build + Two-Phase Disocclusion Culling & Delta Rasterization!
            if (is_scene_lit) {
                auto& lc_hzb = *lit_ctx;
                auto record_hzb_build = [&](const char* pass_name) {
                    std::uint32_t sw = extent.width;
                    std::uint32_t sh = extent.height;
                    for (std::uint32_t m = 0u; m < lc_hzb.hzb_pyramid.mip_count; ++m) {
                        const std::uint32_t dw = std::max(1u, lc_hzb.hzb_pyramid.width >> m);
                        const std::uint32_t dh = std::max(1u, lc_hzb.hzb_pyramid.height >> m);
                        const auto mip_u16 = static_cast<std::uint16_t>(m);
                        graph.add_pass(
                            pass_name, rhi::QueueKind::graphics,
                            [&](rdg::PassBuilder& b) {
                                if (m == 0u) {
                                    b.read_texture(depth_handle);
                                } else {
                                    b.read_texture(
                                        hzb_handle, nri::StageBits::COMPUTE_SHADER, nri::Layout::SHADER_RESOURCE,
                                        rdg::SubresourceRange::single_mip(static_cast<std::uint16_t>(m - 1u)));
                                }
                                b.write_texture(hzb_handle, nri::StageBits::COMPUTE_SHADER,
                                                rdg::SubresourceRange::single_mip(mip_u16));
                            },
                            [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                                ctx.set_pipeline(s.vis_cull_cs);
                                CullRootConsts rc{};
                                rc.frame_cb_srv = frame_cb_srv;
                                rc.phase_mode = 4u;
                                rc.hzb_src_srv = (m == 0u) ? ctx.srv(depth_handle)
                                                           : ctx.srv(hzb_handle, static_cast<std::uint16_t>(m - 1u));
                                rc.hzb_dst_uav = ctx.uav(hzb_handle, mip_u16);
                                rc.hzb_src_w = sw;
                                rc.hzb_src_h = sh;
                                rc.hzb_dst_w = dw;
                                rc.flags_and_dst_h = (dh & 0xFFFFu) | ((m == 0u) ? 0x10000u : 0u);
                                ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                                ctx.core().CmdDispatch(ctx.cmd(), dispatch_grid_2d((dw * dh + 63u) / 64u));
                                return {};
                            });
                        sw = dw;
                        sh = dh;
                    }
                };

                record_hzb_build("LitHzbBuild");

                if (use_two_phase_hzb) {
                    const std::uint32_t hzb_w0 = lc_hzb.hzb_pyramid.width;
                    const std::uint32_t hzb_h0 = lc_hzb.hzb_pyramid.height;
                    graph.add_pass(
                        "LitVisCullPhase2", rhi::QueueKind::graphics,
                        [&](rdg::PassBuilder& b) {
                            b.read_texture(hzb_handle);
                            b.read_buffer(phase2_cands);
                            b.read_buffer(phase2_cs_indirect, nri::AccessBits::ARGUMENT_BUFFER,
                                          nri::StageBits::INDIRECT);
                            b.write_buffer(opaque_clusters);
                            b.write_buffer(masked_clusters);
                            b.write_buffer(selected_clusters);
                            b.write_buffer(cull_counters);
                            b.write_buffer(mesh_indirect_args);
                            b.write_buffer(phase2_mesh_indirect);
                        },
                        [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                            ctx.set_pipeline(s.vis_cull_cs);

                            CullRootConsts rc{};
                            rc.frame_cb_srv = frame_cb_srv;
                            rc.opaque_clusters_uav = ctx.uav(opaque_clusters);
                            rc.masked_clusters_uav = ctx.uav(masked_clusters);
                            rc.selected_clusters_uav = ctx.uav(selected_clusters);
                            rc.phase2_candidates_uav = ctx.srv(phase2_cands);
                            rc.counters_uav = ctx.uav(cull_counters);
                            rc.indirect_args_uav = ctx.uav(mesh_indirect_args);
                            rc.aux_indirect_uav = ctx.uav(phase2_mesh_indirect);
                            rc.hzb_src_srv = ctx.srv(hzb_handle);
                            rc.hzb_src_w = hzb_w0;
                            rc.hzb_src_h = hzb_h0;
                            rc.flags_and_dst_h = cull_mode_flags;

                            // Mode 2: Re-test PrevHZB-occluded candidates against freshly built CurrHZB
                            rc.phase_mode = 2u;
                            ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                            ctx.core().CmdDispatchIndirect(ctx.cmd(), *ctx.buffer(phase2_cs_indirect), 0u);

                            nri::GlobalBarrierDesc uav_bar{
                                .before = {nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER},
                                .after = {nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER}};
                            nri::BarrierDesc bg{.globals = &uav_bar,
                                                .globalNum = 1u,
                                                .buffers = nullptr,
                                                .bufferNum = 0u,
                                                .textures = nullptr,
                                                .textureNum = 0u};
                            ctx.core().CmdBarrier(ctx.cmd(), bg);

                            // Mode 7: Finalize Phase 2 delta DrawMeshTasksDesc & update Wireframe/Selected slots
                            rc.phase_mode = 7u;
                            ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                            ctx.core().CmdDispatch(ctx.cmd(), {1u, 1u, 1u});
                            return {};
                        });

                    graph.add_pass(
                        "LitVisRasterPhase2", rhi::QueueKind::graphics,
                        [&](rdg::PassBuilder& b) {
                            b.read_buffer(opaque_clusters, nri::AccessBits::SHADER_RESOURCE,
                                          nri::StageBits::MESH_SHADER);
                            b.read_buffer(masked_clusters, nri::AccessBits::SHADER_RESOURCE,
                                          nri::StageBits::MESH_SHADER);
                            b.read_buffer(cull_counters, nri::AccessBits::SHADER_RESOURCE, nri::StageBits::MESH_SHADER);
                            b.read_buffer(phase2_mesh_indirect, nri::AccessBits::ARGUMENT_BUFFER,
                                          nri::StageBits::INDIRECT);
                            b.render_target(vis_tex, nri::LoadOp::LOAD, nri::StoreOp::STORE, zero_color);
                            b.depth_target(depth_handle, nri::LoadOp::LOAD, nri::StoreOp::STORE, 0.0f, false);
                        },
                        [=, &s, &prog_set](const rdg::PassContext& ctx) -> rhi::Result<void> {
                            ctx.begin_rendering();
                            if (scene_rv.scissor.right > scene_rv.scissor.left &&
                                scene_rv.scissor.bottom > scene_rv.scissor.top) {
                                set_scene_vp(ctx);

                                ctx.set_pipeline(s.vis_opaque_ms);
                                ArchetypeRootConsts rc{};
                                rc.frame_cb_srv = frame_cb_srv;
                                rc.material_inputs_srv = mat_batch->input_buffer.srv_bindless;
                                rc.material_params_srv = mat_batch->parameter_buffer.srv_bindless;
                                rc.material_surfaces_srv = mat_batch->surface_buffer.srv_bindless;
                                rc.bin_offset_or_cluster_srv = ctx.srv(opaque_clusters);
                                rc.aux0_srv = ctx.srv(cull_counters);
                                rc.viewport_packed0 = 1u;
                                ctx.push_constants(rc, nri::BindPoint::GRAPHICS);
                                ctx.mesh_shader().CmdDrawMeshTasksIndirect(ctx.cmd(), *ctx.buffer(phase2_mesh_indirect),
                                                                           0u, 1u, sizeof(nri::DrawMeshTasksDesc),
                                                                           nullptr, 0u);
                                ++s.stats.scene_draw_calls;

                                for (std::uint32_t m = 0u; m < masked_cnt; ++m) {
                                    const std::uint32_t arch_idx = prog_set.masked_archetype_indices[m];
                                    const auto& arch_pso =
                                        prog_set.archetypes[arch_idx]->pipelines[static_cast<std::size_t>(
                                            detail::ProgramArchetype::Stage::masked_vis_ms)];
                                    ctx.set_pipeline(arch_pso);
                                    rc.archetype_index = m;
                                    rc.bin_offset_or_cluster_srv = ctx.srv(masked_clusters);
                                    rc.pass_flags = masked_stride;
                                    rc.viewport_packed0 = 1u;
                                    ctx.push_constants(rc, nri::BindPoint::GRAPHICS);
                                    ctx.mesh_shader().CmdDrawMeshTasksIndirect(
                                        ctx.cmd(), *ctx.buffer(phase2_mesh_indirect), std::uint64_t(1u + m) * 12u, 1u,
                                        sizeof(nri::DrawMeshTasksDesc), nullptr, 0u);
                                    ++s.stats.scene_draw_calls;
                                }
                            }
                            ctx.end_rendering();
                            return {};
                        });

                    // Rebuild full CurrHZB after Phase 2 delta rasterization so next frame's Phase 1 PrevHZB &
                    // current frame's Screen-Space Hi-Z marches see 100% of disoccluded surfaces!
                    record_hzb_build("LitHzbBuildPhase2");
                }
            }

            if (frame.view.wireframe == Wireframe::only) {
                // Wireframe::only mode: VisBuffer already populated exact visible surface depth (including Masked!).
                graph.add_pass(
                    "WireframePickUnpack", rhi::QueueKind::graphics,
                    [&](rdg::PassBuilder& b) {
                        b.read_texture(vis_tex);
                        b.read_buffer(opaque_clusters);
                        b.read_buffer(masked_clusters);
                        b.write_texture(pick_id_handle);
                    },
                    [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                        ctx.set_pipeline(s.vis_unpack_cs);
                        UnpackRootConsts rc{};
                        rc.frame_cb_srv = frame_cb_srv;
                        rc.mode = 5u;
                        rc.visbuffer_srv = ctx.srv(vis_tex);
                        rc.opaque_clusters_srv = ctx.srv(opaque_clusters);
                        rc.masked_clusters_srv = ctx.srv(masked_clusters);
                        rc.bin_pixels_uav = ctx.uav(pick_id_handle);
                        ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                        ctx.core().CmdDispatch(ctx.cmd(), {(extent.width + 7u) / 8u, (extent.height + 7u) / 8u, 1u});
                        return {};
                    });

                graph.add_pass(
                    "SceneWireframeAndGuides", rhi::QueueKind::graphics,
                    [&](rdg::PassBuilder& b) {
                        b.read_texture(vis_tex, nri::StageBits::FRAGMENT_SHADER);
                        b.read_buffer(opaque_clusters, nri::AccessBits::SHADER_RESOURCE,
                                      nri::StageBits::MESH_SHADER | nri::StageBits::FRAGMENT_SHADER);
                        b.read_buffer(masked_clusters, nri::AccessBits::SHADER_RESOURCE,
                                      nri::StageBits::MESH_SHADER | nri::StageBits::FRAGMENT_SHADER);
                        b.read_buffer(cull_counters, nri::AccessBits::SHADER_RESOURCE, nri::StageBits::MESH_SHADER);
                        b.read_buffer(mesh_indirect_args, nri::AccessBits::ARGUMENT_BUFFER, nri::StageBits::INDIRECT);
                        b.render_target(bb_handle, bb_load(), nri::StoreOp::STORE, zero_color, true);
                        b.depth_target(depth_handle, nri::LoadOp::LOAD, nri::StoreOp::STORE, 0.0f, true);
                    },
                    [=, &s, &frame](const rdg::PassContext& ctx) -> rhi::Result<void> {
                        ctx.begin_rendering();
                        if (scene_rv.scissor.right > scene_rv.scissor.left &&
                            scene_rv.scissor.bottom > scene_rv.scissor.top) {
                            set_scene_vp(ctx);
                            if (grid_cam) {
                                ctx.set_pipeline(s.grid_depth_pipeline);
                                ViewportRootConstants grc{scene_rv.clip_transform, grid_cb_srv, 0u};
                                ctx.push_constants(grc, nri::BindPoint::GRAPHICS);
                                ctx.core().CmdDraw(ctx.cmd(), {3u, 1u, 0u, 0u});
                                ++s.stats.viewport_draw_calls;
                            }
                            ctx.set_pipeline(frame.view.hidden_lines ? s.wireframe_visible_ms : s.wireframe_all_ms);
                            WireRootConsts wrc{};
                            wrc.frame_cb_srv = frame_cb_srv;
                            wrc.opaque_clusters_srv = ctx.srv(opaque_clusters);
                            wrc.masked_clusters_srv = ctx.srv(masked_clusters);
                            wrc.counters_srv = ctx.srv(cull_counters);
                            wrc.visbuffer_srv = ctx.srv(vis_tex);
                            wrc.wire_flags = 4u | (frame.view.hidden_lines ? ((masked_cnt > 0u) ? 8u : 0u) : 16u);
                            ctx.push_constants(wrc, nri::BindPoint::GRAPHICS);
                            ctx.mesh_shader().CmdDrawMeshTasksIndirect(ctx.cmd(), *ctx.buffer(mesh_indirect_args),
                                                                       std::uint64_t(1u + masked_cnt) * 12u, 1u,
                                                                       sizeof(nri::DrawMeshTasksDesc), nullptr, 0u);
                            ++s.stats.scene_draw_calls;
                        }
                        ctx.end_rendering();
                        return {};
                    });
            } else {
                // Fullscreen VisUnpackAndClassify CS -> Analytical Barycentrics + Motion + Per-Archetype Bins!
                const auto motion_tex = graph.create_texture(
                    "motion_vectors",
                    {.format = nri::Format::RG16_SFLOAT, .width = extent.width, .height = extent.height});
                lit_motion_handle = motion_tex;
                const auto prev_depth_tex = graph.create_texture(
                    "expected_prev_depth",
                    {.format = nri::Format::R32_SFLOAT, .width = extent.width, .height = extent.height});
                const auto geom_normal_tex =
                    is_scene_lit ? import_persistent("oct_geom_normal",
                                                     lit_ctx->geom_normal_history[lit_ctx->history_parity & 1u],
                                                     cs_srv_state, true)
                                 : graph.create_texture("oct_geom_normal", {.format = nri::Format::R32_UINT,
                                                                            .width = extent.width,
                                                                            .height = extent.height});
                const auto bary_buf =
                    graph.create_buffer("bary_derivatives", {.size = total_px * 32u, .structure_stride = 32u});
                const auto pixel_meta_buf =
                    graph.create_buffer("shade_pixel_meta", {.size = total_px * 8u, .structure_stride = 8u});
                const auto bin_counts = graph.create_buffer(
                    "shade_bin_counts", {.size = std::uint64_t(arch_cnt) * 4u, .structure_stride = 4u});
                const auto bin_offsets = graph.create_buffer(
                    "shade_bin_offsets", {.size = std::uint64_t(arch_cnt) * 4u, .structure_stride = 4u});
                const auto bin_cursors = graph.create_buffer(
                    "shade_bin_cursors", {.size = std::uint64_t(arch_cnt) * 4u, .structure_stride = 4u});
                const auto bin_pixels =
                    graph.create_buffer("shade_bin_pixels", {.size = total_px * 8u, .structure_stride = 8u});
                const auto bin_indirect = graph.create_buffer(
                    "shade_bin_indirect", {.size = std::uint64_t(arch_cnt) * 12u, .byte_address = true});

                const std::uint32_t tile_em_words =
                    (((extent.width + 7u) / 8u) * ((extent.height + 7u) / 8u) + 31u) / 32u;
                const auto tile_emissive_mask =
                    is_scene_lit ? graph.create_buffer("tile_emissive_mask",
                                                       {.size = std::max<std::uint64_t>(16u, tile_em_words * 4u),
                                                        .structure_stride = 4u})
                                 : rdg::BufferHandle{};
                const auto sss_indirect_args =
                    is_scene_lit ? graph.create_buffer("sss_indirect_args", {.size = 16u, .byte_address = true})
                                 : rdg::BufferHandle{};

                graph.add_pass(
                    is_scene_lit ? "LitVisUnpack" : "StudioVisUnpack", rhi::QueueKind::graphics,
                    [&](rdg::PassBuilder& b) {
                        b.read_texture(vis_tex);
                        b.read_buffer(opaque_clusters);
                        b.read_buffer(masked_clusters);
                        b.write_texture(motion_tex);
                        b.write_texture(prev_depth_tex);
                        b.write_texture(geom_normal_tex);
                        b.write_texture(pick_id_handle);
                        b.write_buffer(bary_buf);
                        b.write_buffer(pixel_meta_buf);
                        b.write_buffer(bin_counts);
                        b.write_buffer(bin_offsets);
                        b.write_buffer(bin_cursors);
                        b.write_buffer(bin_pixels);
                        b.write_buffer(bin_indirect);
                        if (is_scene_lit) {
                            b.write_buffer(tile_emissive_mask);
                            b.write_buffer(sss_indirect_args);
                        }
                    },
                    [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                        ctx.set_pipeline(s.vis_unpack_cs);
                        UnpackRootConsts rc{};
                        rc.frame_cb_srv = frame_cb_srv;
                        rc.visbuffer_srv = ctx.srv(vis_tex);
                        rc.opaque_clusters_srv = ctx.srv(opaque_clusters);
                        rc.masked_clusters_srv = ctx.srv(masked_clusters);
                        rc.pixel_meta_uav = ctx.uav(pixel_meta_buf);
                        rc.motion_uav = ctx.uav(motion_tex);
                        rc.prev_depth_uav = is_scene_lit ? ctx.uav(sss_indirect_args) : 0xFFFFFFFFu;
                        rc.geom_normal_uav = ctx.uav(geom_normal_tex);
                        rc.bary_uav = ctx.uav(bary_buf);
                        rc.bin_counts_uav = ctx.uav(bin_counts);
                        rc.bin_offsets_uav = ctx.uav(bin_offsets);
                        rc.bin_cursors_uav = ctx.uav(bin_cursors);
                        rc.bin_pixels_uav = ctx.uav(bin_pixels);
                        rc.bin_indirect_uav = ctx.uav(bin_indirect);
                        rc.tile_emissive_uav = is_scene_lit ? ctx.uav(tile_emissive_mask) : 0xFFFFFFFFu;

                        nri::GlobalBarrierDesc uav_bar{
                            .before = {nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER},
                            .after = {nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER}};
                        nri::BarrierDesc bg{.globals = &uav_bar,
                                            .globalNum = 1u,
                                            .buffers = nullptr,
                                            .bufferNum = 0u,
                                            .textures = nullptr,
                                            .textureNum = 0u};

                        rc.mode = 0u;
                        ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                        ctx.core().CmdDispatch(ctx.cmd(), {4u, 1u, 1u});
                        ctx.core().CmdBarrier(ctx.cmd(), bg);

                        rc.prev_depth_uav = ctx.uav(prev_depth_tex);

                        const std::uint32_t gx = (extent.width + 7u) / 8u;
                        const std::uint32_t gy = (extent.height + 7u) / 8u;
                        rc.mode = 1u;
                        rc.bin_pixels_uav = ctx.uav(pick_id_handle);
                        ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                        ctx.core().CmdDispatch(ctx.cmd(), {gx, gy, 1u});
                        ctx.core().CmdBarrier(ctx.cmd(), bg);

                        rc.mode = 2u;
                        rc.bin_pixels_uav = ctx.uav(bin_pixels);
                        ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                        ctx.core().CmdDispatch(ctx.cmd(), {1u, 1u, 1u});
                        ctx.core().CmdBarrier(ctx.cmd(), bg);

                        rc.mode = 3u;
                        ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                        ctx.core().CmdDispatch(ctx.cmd(), {gx, gy, 1u});
                        return {};
                    });

                if (!is_scene_lit) {
                    // ========================================================
                    // Studio Mode (`Smooth`, `Flat`, `MatCap` + optional `Cavity`)
                    // ========================================================
                    const auto studio_color = graph.create_texture(
                        "studio_color",
                        {.format = nri::Format::RGBA16_SFLOAT, .width = extent.width, .height = extent.height});
                    const auto cavity_normals =
                        cavity_proj ? graph.create_texture("cavity_normals", {.format = nri::Format::RGBA16_SFLOAT,
                                                                              .width = extent.width,
                                                                              .height = extent.height})
                                    : rdg::TextureHandle{};

                    graph.add_pass(
                        "StudioMaterialShade", rhi::QueueKind::graphics,
                        [&](rdg::PassBuilder& b) {
                            b.read_buffer(bary_buf);
                            b.read_buffer(bin_counts);
                            b.read_buffer(bin_offsets);
                            b.read_buffer(bin_pixels);
                            b.read_buffer(bin_indirect, nri::AccessBits::ARGUMENT_BUFFER, nri::StageBits::INDIRECT);
                            b.write_texture(studio_color);
                            if (cavity_proj)
                                b.write_texture(cavity_normals);
                        },
                        [=, &s, &prog_set](const rdg::PassContext& ctx) -> rhi::Result<void> {
                            ctx.set_pipeline(s.vis_unpack_cs);
                            UnpackRootConsts clr_rc{};
                            clr_rc.frame_cb_srv = frame_cb_srv;
                            clr_rc.mode = 4u;
                            clr_rc.motion_uav = ctx.uav(studio_color);
                            clr_rc.prev_depth_uav = cavity_proj ? ctx.uav(cavity_normals) : 0xFFFFFFFFu;
                            clr_rc.geom_normal_uav = 0xFFFFFFFFu;
                            ctx.push_constants(clr_rc, nri::BindPoint::COMPUTE);
                            ctx.core().CmdDispatch(ctx.cmd(),
                                                   {(extent.width + 7u) / 8u, (extent.height + 7u) / 8u, 1u});

                            nri::GlobalBarrierDesc clr_bar{
                                .before = {nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER},
                                .after = {nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER}};
                            nri::BarrierDesc bg{.globals = &clr_bar,
                                                .globalNum = 1u,
                                                .buffers = nullptr,
                                                .bufferNum = 0u,
                                                .textures = nullptr,
                                                .textureNum = 0u};
                            ctx.core().CmdBarrier(ctx.cmd(), bg);

                            for (std::uint32_t a = 0u; a < static_cast<std::uint32_t>(prog_set.archetypes.size());
                                 ++a) {
                                ctx.set_pipeline(prog_set.archetypes[a]->pipelines[static_cast<std::size_t>(
                                    detail::ProgramArchetype::Stage::studio_bin_cs)]);
                                ArchetypeRootConsts rc{};
                                rc.frame_cb_srv = frame_cb_srv;
                                rc.frame_cb_idx = ctx.srv(bary_buf);
                                rc.material_inputs_srv = mat_batch->input_buffer.srv_bindless;
                                rc.material_params_srv = mat_batch->parameter_buffer.srv_bindless;
                                rc.material_surfaces_srv = mat_batch->surface_buffer.srv_bindless;
                                rc.archetype_index = a;
                                rc.bin_offset_or_cluster_srv = ctx.srv(bin_offsets);
                                rc.aux0_srv = ctx.srv(bin_counts);
                                rc.aux1_srv = ctx.srv(bin_pixels);
                                rc.aux2_uav = ctx.uav(studio_color);
                                rc.aux3_uav = cavity_proj ? ctx.uav(cavity_normals) : 0xFFFFFFFFu;
                                rc.aux4_uav = cavity_proj ? 1u : 0u;
                                ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                                ctx.core().CmdDispatchIndirect(ctx.cmd(), *ctx.buffer(bin_indirect),
                                                               std::uint64_t(a) * 12u);
                            }
                            return {};
                        });

                    if (cavity_proj) {
                        graph.add_pass(
                            "StudioCavityComposite", rhi::QueueKind::graphics,
                            [&](rdg::PassBuilder& b) {
                                b.read_texture(cavity_normals, nri::StageBits::FRAGMENT_SHADER);
                                b.read_texture(studio_color, nri::StageBits::FRAGMENT_SHADER);
                                b.read_texture(depth_handle, nri::StageBits::FRAGMENT_SHADER);
                                b.render_target(bb_handle, bb_load(), nri::StoreOp::STORE, zero_color, true);
                            },
                            [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                                ctx.begin_rendering();
                                if (scene_rv.scissor.right > scene_rv.scissor.left &&
                                    scene_rv.scissor.bottom > scene_rv.scissor.top) {
                                    set_scene_vp(ctx);
                                    ctx.set_pipeline(s.cavity_pipeline);
                                    CavityRootConstants rc{
                                        scene_rv.clip_transform, cavity_cb_srv,         0u,
                                        ctx.srv(cavity_normals), ctx.srv(studio_color), ctx.srv(depth_handle)};
                                    ctx.push_constants(rc, nri::BindPoint::GRAPHICS);
                                    ctx.core().CmdDraw(ctx.cmd(), {3u, 1u, 0u, 0u});
                                    ++s.stats.viewport_draw_calls;
                                }
                                ctx.end_rendering();
                                return {};
                            });
                    } else {
                        // Blit shaded Studio color (alpha > 0 on shaded geometry, preserving gradient backdrop!)
                        graph.add_pass(
                            "StudioComposite", rhi::QueueKind::graphics,
                            [&](rdg::PassBuilder& b) {
                                b.read_texture(studio_color, nri::StageBits::FRAGMENT_SHADER);
                                b.render_target(bb_handle, bb_load(), nri::StoreOp::STORE, zero_color, true);
                            },
                            [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                                ctx.begin_rendering();
                                if (scene_rv.scissor.right > scene_rv.scissor.left &&
                                    scene_rv.scissor.bottom > scene_rv.scissor.top) {
                                    set_scene_vp(ctx);
                                    ctx.set_pipeline(s.present_blit_pipeline);
                                    ViewportRootConstants rc{scene_rv.clip_transform, ctx.srv(studio_color), 0u};
                                    ctx.push_constants(rc, nri::BindPoint::GRAPHICS);
                                    ctx.core().CmdDraw(ctx.cmd(), {3u, 1u, 0u, 0u});
                                }
                                ctx.end_rendering();
                                return {};
                            });
                    }

                    const bool studio_wire =
                        cavity_proj && ((frame.view.wireframe == Wireframe::overlay) || scene.any_selected);
                    if (grid_cam || studio_wire) {
                        const bool sel_only = (frame.view.wireframe != Wireframe::overlay);
                        graph.add_pass(
                            "StudioGuidesAndWireframe", rhi::QueueKind::graphics,
                            [&](rdg::PassBuilder& b) {
                                if (studio_wire) {
                                    if (masked_cnt > 0u)
                                        b.read_texture(vis_tex, nri::StageBits::FRAGMENT_SHADER);
                                    b.read_buffer(opaque_clusters, nri::AccessBits::SHADER_RESOURCE,
                                                  nri::StageBits::MESH_SHADER | nri::StageBits::FRAGMENT_SHADER);
                                    b.read_buffer(masked_clusters, nri::AccessBits::SHADER_RESOURCE,
                                                  nri::StageBits::MESH_SHADER | nri::StageBits::FRAGMENT_SHADER);
                                    if (sel_only) {
                                        b.read_buffer(selected_clusters, nri::AccessBits::SHADER_RESOURCE,
                                                      nri::StageBits::MESH_SHADER);
                                    }
                                    b.read_buffer(cull_counters, nri::AccessBits::SHADER_RESOURCE,
                                                  nri::StageBits::MESH_SHADER);
                                    b.read_buffer(mesh_indirect_args, nri::AccessBits::ARGUMENT_BUFFER,
                                                  nri::StageBits::INDIRECT);
                                }
                                b.render_target(bb_handle, bb_load(), nri::StoreOp::STORE, zero_color, true);
                                b.depth_target(depth_handle, nri::LoadOp::LOAD, nri::StoreOp::STORE, 0.0f, true);
                            },
                            [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                                ctx.begin_rendering();
                                if (scene_rv.scissor.right > scene_rv.scissor.left &&
                                    scene_rv.scissor.bottom > scene_rv.scissor.top) {
                                    set_scene_vp(ctx);
                                    if (grid_cam) {
                                        ctx.set_pipeline(s.grid_depth_pipeline);
                                        ViewportRootConstants grc{scene_rv.clip_transform, grid_cb_srv, 0u};
                                        ctx.push_constants(grc, nri::BindPoint::GRAPHICS);
                                        ctx.core().CmdDraw(ctx.cmd(), {3u, 1u, 0u, 0u});
                                        ++s.stats.viewport_draw_calls;
                                    }
                                    if (studio_wire) {
                                        ctx.set_pipeline(s.wireframe_visible_ms);
                                        WireRootConsts wrc{};
                                        wrc.frame_cb_srv = frame_cb_srv;
                                        wrc.opaque_clusters_srv = ctx.srv(opaque_clusters);
                                        wrc.masked_clusters_srv = ctx.srv(masked_clusters);
                                        wrc.selected_clusters_srv = sel_only ? ctx.srv(selected_clusters) : 0xFFFFFFFFu;
                                        wrc.counters_srv = ctx.srv(cull_counters);
                                        wrc.visbuffer_srv = (masked_cnt > 0u) ? ctx.srv(vis_tex) : 0xFFFFFFFFu;
                                        wrc.wire_flags = (sel_only ? 2u : 0u) | ((masked_cnt > 0u) ? 8u : 0u);
                                        ctx.push_constants(wrc, nri::BindPoint::GRAPHICS);
                                        const std::uint64_t indirect_off =
                                            std::uint64_t((sel_only ? 2u : 1u) + masked_cnt) * 12u;
                                        ctx.mesh_shader().CmdDrawMeshTasksIndirect(
                                            ctx.cmd(), *ctx.buffer(mesh_indirect_args), indirect_off, 1u,
                                            sizeof(nri::DrawMeshTasksDesc), nullptr, 0u);
                                        ++s.stats.scene_draw_calls;
                                    }
                                }
                                ctx.end_rendering();
                                return {};
                            });
                    }
                } else {
                    // ========================================================
                    // Scene Lit Mode: OpenPBR 3x8B Canonical Slab -> 3-Tier Shadows ->
                    // 2.5D Clustered Direct -> 3-Layer GI -> Reflections -> TAAU -> Post!
                    // ========================================================
                    auto& lc = *lit_ctx;
                    const std::uint32_t cur_p = lc.history_parity & 1u;
                    const std::uint32_t prev_p = 1u - cur_p;

                    ATELIER_TRY_ASSIGN(counter_rb_slot, s.lit_counter_readback.acquire_write(*s.device, slot_idx));
                    const auto rb_counters_h =
                        graph.import_buffer("lit_counters_rb", counter_rb_slot->buffer.buffer, {},
                                            {nri::AccessBits::HOST_READ, nri::StageBits::HOST});
                    const auto gpu_counters =
                        graph.create_buffer("lit_gpu_counters", {.size = 32u, .structure_stride = 4u});

                    // ASBuildAndUploads Pass (Scope 1: `scene_lit_gpu_ms[1]`, records TLAS build & counter clear!)
                    graph.add_pass(
                        "ASBuildAndUploads", rhi::QueueKind::graphics,
                        [&](rdg::PassBuilder& b) {
                            b.write_buffer(gpu_counters);
                            b.mark_side_effect();
                        },
                        [=, &s, &scene](const rdg::PassContext& ctx) -> rhi::Result<void> {
                            ctx.set_pipeline(s.vis_cull_cs);
                            CullRootConsts clr_rc{};
                            clr_rc.frame_cb_srv = frame_cb_srv;
                            clr_rc.phase_mode = 0u;
                            clr_rc.counters_uav = 0xFFFFFFFFu;
                            clr_rc.hzb_dst_uav = ctx.uav(gpu_counters);
                            ctx.push_constants(clr_rc, nri::BindPoint::COMPUTE);
                            ctx.core().CmdDispatch(ctx.cmd(), {1u, 1u, 1u});

                            detail::RenderSceneTable::record_tlas_build(*s.device, ctx.cmd(), scene);
                            return {};
                        });

                    // Import persistent temporal history textures & 3D clipmaps so RDG manages exact Enhanced Barriers!
                    const auto prev_diff_h =
                        import_persistent("prev_diffuse_emissive", lc.prev_diffuse_emissive, cs_srv_state, false);
                    const auto gi_hist_prev_h =
                        import_persistent("gi_history_prev", lc.gi_history[prev_p], cs_srv_state, false);
                    const auto gi_hist_cur_h =
                        import_persistent("gi_history_curr", lc.gi_history[cur_p], cs_srv_state, true);
                    const auto refl_hist_prev_h =
                        import_persistent("refl_history_prev", lc.refl_history[prev_p], cs_srv_state, false);
                    const auto refl_hist_cur_h =
                        import_persistent("refl_history_curr", lc.refl_history[cur_p], cs_srv_state, true);
                    const auto refl_moments_prev_h =
                        import_persistent("refl_moments_prev", lc.refl_moments_history[prev_p], cs_srv_state, false);
                    const auto refl_moments_curr_h =
                        import_persistent("refl_moments_curr", lc.refl_moments_history[cur_p], cs_srv_state, true);
                    const bool ssilvb_active = frame.view.lit.ssilvb;
                    const auto ssilvb_ao_prev_h =
                        ssilvb_active
                            ? import_persistent("ssilvb_ao_prev", lc.ssilvb_ao_history[prev_p], cs_srv_state, false)
                            : rdg::TextureHandle{};
                    const auto ssilvb_ao_curr_h =
                        ssilvb_active
                            ? import_persistent("ssilvb_ao_curr", lc.ssilvb_ao_history[cur_p], cs_srv_state, true)
                            : rdg::TextureHandle{};
                    const auto ssilvb_gi_prev_h =
                        ssilvb_active
                            ? import_persistent("ssilvb_gi_prev", lc.ssilvb_gi_history[prev_p], cs_srv_state, false)
                            : rdg::TextureHandle{};
                    const auto ssilvb_gi_curr_h =
                        ssilvb_active
                            ? import_persistent("ssilvb_gi_curr", lc.ssilvb_gi_history[cur_p], cs_srv_state, true)
                            : rdg::TextureHandle{};
                    const auto taa_hist_prev_h =
                        import_persistent("taa_history_prev", lc.taa_history[prev_p], cs_srv_state, false);
                    const auto taa_hist_cur_h =
                        import_persistent("taa_history_curr", lc.taa_history[cur_p], cs_srv_state, true);
                    const auto unjit_depth_prev_h =
                        import_persistent("unjit_depth_prev", lc.unjit_depth_history[prev_p], cs_srv_state, false);
                    const auto unjit_depth_curr_h =
                        import_persistent("unjit_depth_curr", lc.unjit_depth_history[cur_p], cs_srv_state, true);
                    const auto geom_normal_prev_h =
                        import_persistent("geom_normal_prev", lc.geom_normal_history[prev_p], cs_srv_state, false);
                    const auto gi_delta_prev_h =
                        import_persistent("gi_delta_prev", lc.gi_color_delta[prev_p], cs_srv_state, false);
                    const auto gi_delta_cur_h =
                        import_persistent("gi_delta_curr", lc.gi_color_delta[cur_p], cs_srv_state, true);
                    const auto probe_atlas_prev_h =
                        import_persistent("probe_atlas_prev", lc.probe_atlas[prev_p], cs_srv_state, false);
                    const auto probe_atlas_curr_h =
                        import_persistent("probe_atlas_curr", lc.probe_atlas[cur_p], cs_srv_state, true);
                    const auto probe_mask_prev_h =
                        import_persistent("probe_mask_prev", lc.probe_mask[prev_p], cs_srv_state, false);
                    const auto probe_mask_curr_h =
                        import_persistent("probe_mask_curr", lc.probe_mask[cur_p], cs_srv_state, true);
                    const auto froxel_prev_h =
                        fog_active ? import_persistent("froxel_prev", lc.froxel_history[prev_p], cs_srv_state, false)
                                   : rdg::TextureHandle{};
                    const auto froxel_curr_h =
                        fog_active ? import_persistent("froxel_curr", lc.froxel_history[cur_p], cs_srv_state, true)
                                   : rdg::TextureHandle{};
                    const auto froxel_lut_h = fog_active
                                                  ? import_persistent("froxel_lut", lc.froxel_lut, cs_srv_state, true)
                                                  : rdg::TextureHandle{};

                    auto import_persistent_buf = [&](const char* name, OwnedBuffer& buf,
                                                     rdg::BufferState final_st) -> rdg::BufferHandle {
                        return graph.import_buffer(name, buf.buffer, buf.state, final_st,
                                                   {.srv_bindless = buf.srv_bindless, .uav_bindless = buf.uav_bindless},
                                                   &buf.state);
                    };
                    constexpr rdg::BufferState cs_buf_srv_state{nri::AccessBits::SHADER_RESOURCE,
                                                                nri::StageBits::COMPUTE_SHADER};
                    const auto wrc_accum_h = import_persistent_buf("sharc_accum", lc.wrc_accum, cs_buf_srv_state);
                    const auto wrc_hash_keys_h =
                        import_persistent_buf("sharc_hash_keys", lc.wrc_hash_keys, cs_buf_srv_state);
                    const auto wrc_voxel_keys_h =
                        import_persistent_buf("sharc_voxel_keys", lc.wrc_voxel_keys, cs_buf_srv_state);
                    const auto wrc_prev_h = import_persistent_buf("sharc_prev", lc.wrc_slots[prev_p], cs_buf_srv_state);
                    const auto wrc_curr_h = import_persistent_buf("sharc_curr", lc.wrc_slots[cur_p], cs_buf_srv_state);

                    // OpenPBR 3x8B Canonical Slab (`Slab0_SpecNormal`, `Slab1_AlbedoClosure`, `Slab2_Emissive`)
                    const auto slab0 = graph.create_texture(
                        "slab0_spec_normal",
                        {.format = nri::Format::RG32_UINT, .width = extent.width, .height = extent.height});
                    const auto slab1 = graph.create_texture(
                        "slab1_albedo_closure",
                        {.format = nri::Format::RG32_UINT, .width = extent.width, .height = extent.height});
                    const auto slab2 = graph.create_texture(
                        "slab2_emissive",
                        {.format = nri::Format::RGBA16_SFLOAT, .width = extent.width, .height = extent.height});

                    graph.add_pass(
                        "LitMaterialShade", rhi::QueueKind::graphics,
                        [&](rdg::PassBuilder& b) {
                            b.read_buffer(bary_buf);
                            b.read_buffer(bin_counts);
                            b.read_buffer(bin_offsets);
                            b.read_buffer(bin_pixels);
                            b.read_buffer(bin_indirect, nri::AccessBits::ARGUMENT_BUFFER, nri::StageBits::INDIRECT);
                            b.write_texture(slab0);
                            b.write_texture(slab1);
                            b.write_texture(slab2);
                            b.write_buffer(tile_emissive_mask);
                            b.write_buffer(sss_indirect_args);
                        },
                        [=, &s, &prog_set](const rdg::PassContext& ctx) -> rhi::Result<void> {
                            ctx.set_pipeline(s.vis_unpack_cs);
                            UnpackRootConsts clr_rc{};
                            clr_rc.frame_cb_srv = frame_cb_srv;
                            clr_rc.mode = 4u;
                            clr_rc.motion_uav = ctx.uav(slab2);
                            clr_rc.prev_depth_uav = 0xFFFFFFFFu;
                            clr_rc.geom_normal_uav = ctx.uav(slab0);
                            ctx.push_constants(clr_rc, nri::BindPoint::COMPUTE);
                            ctx.core().CmdDispatch(ctx.cmd(),
                                                   {(extent.width + 7u) / 8u, (extent.height + 7u) / 8u, 1u});

                            nri::GlobalBarrierDesc clr_bar{
                                .before = {nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER},
                                .after = {nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER}};
                            nri::BarrierDesc bg{.globals = &clr_bar,
                                                .globalNum = 1u,
                                                .buffers = nullptr,
                                                .bufferNum = 0u,
                                                .textures = nullptr,
                                                .textureNum = 0u};
                            ctx.core().CmdBarrier(ctx.cmd(), bg);

                            for (std::uint32_t a = 0u; a < static_cast<std::uint32_t>(prog_set.archetypes.size());
                                 ++a) {
                                ctx.set_pipeline(prog_set.archetypes[a]->pipelines[static_cast<std::size_t>(
                                    detail::ProgramArchetype::Stage::shade_bin_cs)]);
                                ArchetypeRootConsts rc{};
                                rc.frame_cb_srv = frame_cb_srv;
                                rc.frame_cb_idx = ctx.srv(bary_buf);
                                rc.material_inputs_srv = mat_batch->input_buffer.srv_bindless;
                                rc.material_params_srv = mat_batch->parameter_buffer.srv_bindless;
                                rc.material_surfaces_srv = mat_batch->surface_buffer.srv_bindless;
                                rc.archetype_index = a;
                                rc.bin_offset_or_cluster_srv = ctx.srv(bin_offsets);
                                rc.aux0_srv = ctx.srv(bin_counts);
                                rc.aux1_srv = ctx.srv(bin_pixels);
                                rc.aux2_uav = ctx.uav(slab0);
                                rc.aux3_uav = ctx.uav(slab1);
                                rc.aux4_uav = ctx.uav(slab2);
                                rc.aux5_uav = ctx.uav(tile_emissive_mask);
                                rc.viewport_packed0 = ctx.uav(sss_indirect_args);
                                ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                                ctx.core().CmdDispatchIndirect(ctx.cmd(), *ctx.buffer(bin_indirect),
                                                               std::uint64_t(a) * 12u);
                            }
                            return {};
                        });

                    // Software-Paged D32_FLOAT VSM Physical Pools (128 MiB: Static Base + Copy-on-Write Dynamic Pool)
                    rdg::TextureHandle vsm_static_h{};
                    rdg::TextureHandle vsm_dynamic_h{};
                    if (vsm_plan.active) {
                        vsm_static_h = import_persistent("vsm_static_pool", lc.vsm_cache.static_pool(), cs_srv_state,
                                                         vsm_plan.first_pool_init || vsm_plan.clear_all_static);
                        vsm_dynamic_h = import_persistent("vsm_dynamic_pool", lc.vsm_cache.dynamic_pool(), cs_srv_state,
                                                          vsm_plan.first_pool_init);

                        const nri::Viewport vsm_vp{0.0f,
                                                   0.0f,
                                                   static_cast<float>(detail::vsm_pool_dim),
                                                   static_cast<float>(detail::vsm_pool_dim),
                                                   0.0f,
                                                   1.0f,
                                                   false};
                        const nri::Rect vsm_sc{0, 0, static_cast<nri::Dim_t>(detail::vsm_pool_dim),
                                               static_cast<nri::Dim_t>(detail::vsm_pool_dim)};

                        if ((vsm_plan.first_pool_init || vsm_plan.clear_all_static) &&
                            vsm_plan.static_pair_count == 0u) {
                            graph.add_pass(
                                "LitVsmPoolInitStatic", rhi::QueueKind::graphics,
                                [&](rdg::PassBuilder& b) {
                                    b.depth_target(vsm_static_h, nri::LoadOp::CLEAR, nri::StoreOp::STORE, 0.0f, false);
                                },
                                [=](const rdg::PassContext& ctx) -> rhi::Result<void> {
                                    ctx.begin_rendering();
                                    ctx.end_rendering();
                                    return {};
                                });
                        }
                        if (vsm_plan.first_pool_init) {
                            graph.add_pass(
                                "LitVsmPoolInitDynamic", rhi::QueueKind::graphics,
                                [&](rdg::PassBuilder& b) {
                                    b.depth_target(vsm_dynamic_h, nri::LoadOp::CLEAR, nri::StoreOp::STORE, 0.0f, false);
                                },
                                [=](const rdg::PassContext& ctx) -> rhi::Result<void> {
                                    ctx.begin_rendering();
                                    ctx.end_rendering();
                                    return {};
                                });
                        }

                        if (vsm_plan.static_pair_count > 0u) {
                            const std::uint32_t st_max_op = vsm_plan.static_max_opaque_meshlets;
                            const std::uint32_t st_mk_str = vsm_plan.static_max_masked_meshlets;
                            const auto vsm_st_op = graph.create_buffer(
                                "vsm_static_opaque", {.size = std::uint64_t(st_max_op) * 16u, .structure_stride = 16u});
                            const auto vsm_st_mk = graph.create_buffer(
                                "vsm_static_masked", {.size = std::uint64_t(std::max(1u, masked_cnt)) * st_mk_str * 16u,
                                                      .structure_stride = 16u});
                            const auto vsm_st_cnt =
                                graph.create_buffer("vsm_static_counters", {.size = std::uint64_t(1u + masked_cnt) * 4u,
                                                                            .structure_stride = 4u});
                            const auto vsm_st_ind = graph.create_buffer("vsm_static_indirect",
                                                                        {.size = std::uint64_t(1u + masked_cnt) * 12u,
                                                                         .byte_address = true,
                                                                         .usage = nri::BufferUsageBits::ARGUMENT});

                            graph.add_pass(
                                "LitVsmUpdateStaticCull", rhi::QueueKind::graphics,
                                [&](rdg::PassBuilder& b) {
                                    b.write_buffer(vsm_st_op);
                                    b.write_buffer(vsm_st_mk);
                                    b.write_buffer(vsm_st_cnt);
                                    b.write_buffer(vsm_st_ind);
                                },
                                [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                                    ctx.set_pipeline(
                                        s.lit_pipelines[static_cast<std::size_t>(State::LitStage::vsm_cull)]);
                                    VsmCullRootConsts rc{};
                                    rc.frame_cb_srv = frame_cb_srv;
                                    rc.page_jobs_srv = vsm_plan.static_jobs_srv;
                                    rc.pairs_srv = vsm_plan.static_pairs_srv;
                                    rc.pair_count = vsm_plan.static_pair_count;
                                    rc.max_opaque = st_max_op;
                                    rc.masked_stride = st_mk_str;
                                    rc.opaque_clusters_uav = ctx.uav(vsm_st_op);
                                    rc.masked_clusters_uav = ctx.uav(vsm_st_mk);
                                    rc.counters_uav = ctx.uav(vsm_st_cnt);
                                    rc.indirect_args_uav = ctx.uav(vsm_st_ind);

                                    nri::GlobalBarrierDesc uav_bar{.before = {nri::AccessBits::SHADER_RESOURCE_STORAGE,
                                                                              nri::StageBits::COMPUTE_SHADER},
                                                                   .after = {nri::AccessBits::SHADER_RESOURCE_STORAGE,
                                                                             nri::StageBits::COMPUTE_SHADER}};
                                    nri::BarrierDesc bg{.globals = &uav_bar,
                                                        .globalNum = 1u,
                                                        .buffers = nullptr,
                                                        .bufferNum = 0u,
                                                        .textures = nullptr,
                                                        .textureNum = 0u};

                                    rc.vsm_cull_mode = 0u;
                                    ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                                    ctx.core().CmdDispatch(ctx.cmd(), {(1u + masked_cnt + 63u) / 64u, 1u, 1u});
                                    ctx.core().CmdBarrier(ctx.cmd(), bg);

                                    rc.vsm_cull_mode = 1u;
                                    ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                                    ctx.core().CmdDispatch(ctx.cmd(), dispatch_grid_2d(vsm_plan.static_pair_count));
                                    ctx.core().CmdBarrier(ctx.cmd(), bg);

                                    rc.vsm_cull_mode = 2u;
                                    ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                                    ctx.core().CmdDispatch(ctx.cmd(), {(1u + masked_cnt + 63u) / 64u, 1u, 1u});
                                    return {};
                                });

                            const auto st_clear_rects = vsm_plan.static_clear_rects;
                            const bool st_clear_all = vsm_plan.clear_all_static;
                            const bool st_has_masked = vsm_plan.static_has_masked;
                            const std::uint32_t st_jobs_srv = vsm_plan.static_jobs_srv;
                            graph.add_pass(
                                "LitVsmUpdateStaticRaster", rhi::QueueKind::graphics,
                                [&](rdg::PassBuilder& b) {
                                    b.read_buffer(vsm_st_op, nri::AccessBits::SHADER_RESOURCE,
                                                  nri::StageBits::MESH_SHADER);
                                    b.read_buffer(vsm_st_mk, nri::AccessBits::SHADER_RESOURCE,
                                                  nri::StageBits::MESH_SHADER);
                                    b.read_buffer(vsm_st_cnt, nri::AccessBits::SHADER_RESOURCE,
                                                  nri::StageBits::MESH_SHADER);
                                    b.read_buffer(vsm_st_ind, nri::AccessBits::ARGUMENT_BUFFER,
                                                  nri::StageBits::INDIRECT);
                                    b.depth_target(vsm_static_h, st_clear_all ? nri::LoadOp::CLEAR : nri::LoadOp::LOAD,
                                                   nri::StoreOp::STORE, 0.0f, false);
                                },
                                [=, &s, &prog_set](const rdg::PassContext& ctx) -> rhi::Result<void> {
                                    ctx.begin_rendering();
                                    if (!st_clear_all && !st_clear_rects.empty()) {
                                        nri::ClearAttachmentDesc cd{};
                                        cd.value.depthStencil = {0.0f, 0u};
                                        cd.planes = nri::PlaneBits::DEPTH;
                                        ctx.core().CmdClearAttachments(
                                            ctx.cmd(), &cd, 1u, st_clear_rects.data(),
                                            static_cast<std::uint32_t>(st_clear_rects.size()));
                                    }
                                    ctx.core().CmdSetViewports(ctx.cmd(), &vsm_vp, 1u);
                                    ctx.core().CmdSetScissors(ctx.cmd(), &vsm_sc, 1u);

                                    ctx.set_pipeline(
                                        s.lit_pipelines[static_cast<std::size_t>(State::LitStage::vsm_opaque_ms)]);
                                    ArchetypeRootConsts rc{};
                                    rc.frame_cb_srv = frame_cb_srv;
                                    rc.frame_cb_idx = st_jobs_srv;
                                    rc.material_inputs_srv = mat_batch->input_buffer.srv_bindless;
                                    rc.material_params_srv = mat_batch->parameter_buffer.srv_bindless;
                                    rc.material_surfaces_srv = mat_batch->surface_buffer.srv_bindless;
                                    rc.bin_offset_or_cluster_srv = ctx.srv(vsm_st_op);
                                    rc.aux0_srv = ctx.srv(vsm_st_cnt);
                                    rc.pass_flags = st_mk_str;
                                    ctx.push_constants(rc, nri::BindPoint::GRAPHICS);
                                    ctx.mesh_shader().CmdDrawMeshTasksIndirect(ctx.cmd(), *ctx.buffer(vsm_st_ind), 0u,
                                                                               1u, sizeof(nri::DrawMeshTasksDesc),
                                                                               nullptr, 0u);
                                    ++s.stats.scene_lit_vsm_draws;

                                    if (st_has_masked) {
                                        for (std::uint32_t m = 0u; m < masked_cnt; ++m) {
                                            const std::uint32_t arch_idx = prog_set.masked_archetype_indices[m];
                                            ctx.set_pipeline(
                                                prog_set.archetypes[arch_idx]->pipelines[static_cast<std::size_t>(
                                                    detail::ProgramArchetype::Stage::masked_vsm_ms)]);
                                            rc.archetype_index = m;
                                            rc.bin_offset_or_cluster_srv = ctx.srv(vsm_st_mk);
                                            ctx.push_constants(rc, nri::BindPoint::GRAPHICS);
                                            ctx.mesh_shader().CmdDrawMeshTasksIndirect(
                                                ctx.cmd(), *ctx.buffer(vsm_st_ind), std::uint64_t(1u + m) * 12u, 1u,
                                                sizeof(nri::DrawMeshTasksDesc), nullptr, 0u);
                                            ++s.stats.scene_lit_vsm_draws;
                                        }
                                    }
                                    ctx.end_rendering();
                                    return {};
                                });
                        }

                        if (!vsm_plan.cow_copies.empty()) {
                            const auto cow_list = vsm_plan.cow_copies;
                            graph.add_pass(
                                "LitVsmUpdateCopyCoW", rhi::QueueKind::graphics,
                                [&](rdg::PassBuilder& b) {
                                    b.copy_source_texture(vsm_static_h);
                                    b.copy_dest_texture(vsm_dynamic_h);
                                },
                                [=](const rdg::PassContext& ctx) -> rhi::Result<void> {
                                    for (const auto& cp : cow_list) {
                                        const auto sx =
                                            static_cast<nri::Dim_t>((cp.src_static_page & 31u) * detail::vsm_page_dim);
                                        const auto sy =
                                            static_cast<nri::Dim_t>((cp.src_static_page >> 5u) * detail::vsm_page_dim);
                                        const auto dx =
                                            static_cast<nri::Dim_t>((cp.dst_dynamic_page & 31u) * detail::vsm_page_dim);
                                        const auto dy =
                                            static_cast<nri::Dim_t>((cp.dst_dynamic_page >> 5u) * detail::vsm_page_dim);
                                        const auto pd = static_cast<nri::Dim_t>(detail::vsm_page_dim);
                                        nri::TextureRegionDesc src_r{
                                            sx, sy, 0u, pd, pd, 1u, 0u, 0u, nri::PlaneBits::DEPTH};
                                        nri::TextureRegionDesc dst_r{
                                            dx, dy, 0u, pd, pd, 1u, 0u, 0u, nri::PlaneBits::DEPTH};
                                        ctx.core().CmdCopyTexture(ctx.cmd(), *ctx.texture(vsm_dynamic_h), &dst_r,
                                                                  *ctx.texture(vsm_static_h), &src_r);
                                    }
                                    return {};
                                });
                        }

                        if (vsm_plan.dynamic_pair_count > 0u) {
                            const std::uint32_t dy_max_op = vsm_plan.dynamic_max_opaque_meshlets;
                            const std::uint32_t dy_mk_str = vsm_plan.dynamic_max_masked_meshlets;
                            const auto vsm_dy_op =
                                graph.create_buffer("vsm_dynamic_opaque",
                                                    {.size = std::uint64_t(dy_max_op) * 16u, .structure_stride = 16u});
                            const auto vsm_dy_mk =
                                graph.create_buffer("vsm_dynamic_masked",
                                                    {.size = std::uint64_t(std::max(1u, masked_cnt)) * dy_mk_str * 16u,
                                                     .structure_stride = 16u});
                            const auto vsm_dy_cnt = graph.create_buffer(
                                "vsm_dynamic_counters",
                                {.size = std::uint64_t(1u + masked_cnt) * 4u, .structure_stride = 4u});
                            const auto vsm_dy_ind = graph.create_buffer("vsm_dynamic_indirect",
                                                                        {.size = std::uint64_t(1u + masked_cnt) * 12u,
                                                                         .byte_address = true,
                                                                         .usage = nri::BufferUsageBits::ARGUMENT});

                            graph.add_pass(
                                "LitVsmUpdateDynamicCull", rhi::QueueKind::graphics,
                                [&](rdg::PassBuilder& b) {
                                    b.write_buffer(vsm_dy_op);
                                    b.write_buffer(vsm_dy_mk);
                                    b.write_buffer(vsm_dy_cnt);
                                    b.write_buffer(vsm_dy_ind);
                                },
                                [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                                    ctx.set_pipeline(
                                        s.lit_pipelines[static_cast<std::size_t>(State::LitStage::vsm_cull)]);
                                    VsmCullRootConsts rc{};
                                    rc.frame_cb_srv = frame_cb_srv;
                                    rc.page_jobs_srv = vsm_plan.dynamic_jobs_srv;
                                    rc.pairs_srv = vsm_plan.dynamic_pairs_srv;
                                    rc.pair_count = vsm_plan.dynamic_pair_count;
                                    rc.max_opaque = dy_max_op;
                                    rc.masked_stride = dy_mk_str;
                                    rc.opaque_clusters_uav = ctx.uav(vsm_dy_op);
                                    rc.masked_clusters_uav = ctx.uav(vsm_dy_mk);
                                    rc.counters_uav = ctx.uav(vsm_dy_cnt);
                                    rc.indirect_args_uav = ctx.uav(vsm_dy_ind);

                                    nri::GlobalBarrierDesc uav_bar{.before = {nri::AccessBits::SHADER_RESOURCE_STORAGE,
                                                                              nri::StageBits::COMPUTE_SHADER},
                                                                   .after = {nri::AccessBits::SHADER_RESOURCE_STORAGE,
                                                                             nri::StageBits::COMPUTE_SHADER}};
                                    nri::BarrierDesc bg{.globals = &uav_bar,
                                                        .globalNum = 1u,
                                                        .buffers = nullptr,
                                                        .bufferNum = 0u,
                                                        .textures = nullptr,
                                                        .textureNum = 0u};

                                    rc.vsm_cull_mode = 0u;
                                    ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                                    ctx.core().CmdDispatch(ctx.cmd(), {(1u + masked_cnt + 63u) / 64u, 1u, 1u});
                                    ctx.core().CmdBarrier(ctx.cmd(), bg);

                                    rc.vsm_cull_mode = 1u;
                                    ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                                    ctx.core().CmdDispatch(ctx.cmd(), dispatch_grid_2d(vsm_plan.dynamic_pair_count));
                                    ctx.core().CmdBarrier(ctx.cmd(), bg);

                                    rc.vsm_cull_mode = 2u;
                                    ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                                    ctx.core().CmdDispatch(ctx.cmd(), {(1u + masked_cnt + 63u) / 64u, 1u, 1u});
                                    return {};
                                });

                            const auto dy_clear_rects = vsm_plan.dynamic_clear_rects;
                            const bool dy_has_masked = vsm_plan.dynamic_has_masked;
                            const std::uint32_t dy_jobs_srv = vsm_plan.dynamic_jobs_srv;
                            graph.add_pass(
                                "LitVsmUpdateDynamicRaster", rhi::QueueKind::graphics,
                                [&](rdg::PassBuilder& b) {
                                    b.read_buffer(vsm_dy_op, nri::AccessBits::SHADER_RESOURCE,
                                                  nri::StageBits::MESH_SHADER);
                                    b.read_buffer(vsm_dy_mk, nri::AccessBits::SHADER_RESOURCE,
                                                  nri::StageBits::MESH_SHADER);
                                    b.read_buffer(vsm_dy_cnt, nri::AccessBits::SHADER_RESOURCE,
                                                  nri::StageBits::MESH_SHADER);
                                    b.read_buffer(vsm_dy_ind, nri::AccessBits::ARGUMENT_BUFFER,
                                                  nri::StageBits::INDIRECT);
                                    b.depth_target(vsm_dynamic_h, nri::LoadOp::LOAD, nri::StoreOp::STORE, 0.0f, false);
                                },
                                [=, &s, &prog_set](const rdg::PassContext& ctx) -> rhi::Result<void> {
                                    ctx.begin_rendering();
                                    if (!dy_clear_rects.empty()) {
                                        nri::ClearAttachmentDesc cd{};
                                        cd.value.depthStencil = {0.0f, 0u};
                                        cd.planes = nri::PlaneBits::DEPTH;
                                        ctx.core().CmdClearAttachments(
                                            ctx.cmd(), &cd, 1u, dy_clear_rects.data(),
                                            static_cast<std::uint32_t>(dy_clear_rects.size()));
                                    }
                                    ctx.core().CmdSetViewports(ctx.cmd(), &vsm_vp, 1u);
                                    ctx.core().CmdSetScissors(ctx.cmd(), &vsm_sc, 1u);

                                    ctx.set_pipeline(
                                        s.lit_pipelines[static_cast<std::size_t>(State::LitStage::vsm_opaque_ms)]);
                                    ArchetypeRootConsts rc{};
                                    rc.frame_cb_srv = frame_cb_srv;
                                    rc.frame_cb_idx = dy_jobs_srv;
                                    rc.material_inputs_srv = mat_batch->input_buffer.srv_bindless;
                                    rc.material_params_srv = mat_batch->parameter_buffer.srv_bindless;
                                    rc.material_surfaces_srv = mat_batch->surface_buffer.srv_bindless;
                                    rc.bin_offset_or_cluster_srv = ctx.srv(vsm_dy_op);
                                    rc.aux0_srv = ctx.srv(vsm_dy_cnt);
                                    rc.pass_flags = dy_mk_str;
                                    ctx.push_constants(rc, nri::BindPoint::GRAPHICS);
                                    ctx.mesh_shader().CmdDrawMeshTasksIndirect(ctx.cmd(), *ctx.buffer(vsm_dy_ind), 0u,
                                                                               1u, sizeof(nri::DrawMeshTasksDesc),
                                                                               nullptr, 0u);
                                    ++s.stats.scene_lit_vsm_draws;

                                    if (dy_has_masked) {
                                        for (std::uint32_t m = 0u; m < masked_cnt; ++m) {
                                            const std::uint32_t arch_idx = prog_set.masked_archetype_indices[m];
                                            ctx.set_pipeline(
                                                prog_set.archetypes[arch_idx]->pipelines[static_cast<std::size_t>(
                                                    detail::ProgramArchetype::Stage::masked_vsm_ms)]);
                                            rc.archetype_index = m;
                                            rc.bin_offset_or_cluster_srv = ctx.srv(vsm_dy_mk);
                                            ctx.push_constants(rc, nri::BindPoint::GRAPHICS);
                                            ctx.mesh_shader().CmdDrawMeshTasksIndirect(
                                                ctx.cmd(), *ctx.buffer(vsm_dy_ind), std::uint64_t(1u + m) * 12u, 1u,
                                                sizeof(nri::DrawMeshTasksDesc), nullptr, 0u);
                                            ++s.stats.scene_lit_vsm_draws;
                                        }
                                    }
                                    ctx.end_rendering();
                                    return {};
                                });
                        }
                    }

                    // Layer 0 GI: Full-Res Temporal Signed 32-Sector ONB-GT-VBAO + Spatiotemporal Filter (`scene_lit_gpu_ms[4]`)
                    if (ssilvb_active) {
                        const auto ssilvb_bent_raw = graph.create_texture(
                            "ssilvb_bent_raw",
                            {.format = nri::Format::RGBA16_SFLOAT, .width = extent.width, .height = extent.height});
                        const auto ssilvb_gi_raw = graph.create_texture(
                            "ssilvb_gi_raw",
                            {.format = nri::Format::RGBA16_SFLOAT, .width = extent.width, .height = extent.height});

                        graph.add_pass(
                            "LitSSILVB", rhi::QueueKind::graphics,
                            [&](rdg::PassBuilder& b) {
                                b.read_texture(depth_handle);
                                b.read_texture(hzb_handle);
                                b.read_texture(geom_normal_tex);
                                b.read_texture(motion_tex);
                                if (use_lit_history)
                                    b.read_texture(prev_diff_h);
                                b.write_texture(ssilvb_bent_raw);
                                b.write_texture(ssilvb_gi_raw);
                            },
                            [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                                ctx.set_pipeline(
                                    s.lit_pipelines[static_cast<std::size_t>(State::LitStage::gi_four_layer)]);
                                GiRootConsts rc{};
                                rc.frame_cb_srv = frame_cb_srv;
                                rc.gi_mode = 0u;
                                rc.depth_srv = ctx.srv(depth_handle);
                                rc.hzb_srv = ctx.srv(hzb_handle);
                                rc.geom_normal_srv = ctx.srv(geom_normal_tex);
                                rc.motion_srv = ctx.srv(motion_tex);
                                rc.prev_diffuse_srv = use_lit_history ? ctx.srv(prev_diff_h) : 0xFFFFFFFFu;
                                rc.srv_aux2 = use_ssilvb_history ? 1u : 0u;
                                rc.uav0 = ctx.uav(ssilvb_bent_raw);
                                rc.uav1 = ctx.uav(ssilvb_gi_raw);
                                ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                                ctx.core().CmdDispatch(ctx.cmd(), {sc_gx_cnt, sc_gy_cnt, 1u});
                                return {};
                            });

                        graph.add_pass(
                            "LitSSILVBFilter", rhi::QueueKind::graphics,
                            [&](rdg::PassBuilder& b) {
                                b.read_texture(depth_handle);
                                b.read_texture(geom_normal_tex);
                                b.read_texture(ssilvb_bent_raw);
                                b.read_texture(ssilvb_gi_raw);
                                if (use_ssilvb_history) {
                                    b.read_texture(motion_tex);
                                    b.read_texture(unjit_depth_prev_h);
                                    b.read_texture(geom_normal_prev_h);
                                    b.read_texture(ssilvb_ao_prev_h);
                                    b.read_texture(ssilvb_gi_prev_h);
                                }
                                b.write_texture(ssilvb_ao_curr_h);
                                b.write_texture(ssilvb_gi_curr_h);
                            },
                            [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                                ctx.set_pipeline(
                                    s.lit_pipelines[static_cast<std::size_t>(State::LitStage::gi_four_layer)]);
                                GiRootConsts rc{};
                                rc.frame_cb_srv = frame_cb_srv;
                                rc.gi_mode = 6u;
                                rc.depth_srv = ctx.srv(depth_handle);
                                rc.hzb_srv = use_ssilvb_history ? ctx.srv(geom_normal_prev_h) : 0xFFFFFFFFu;
                                rc.geom_normal_srv = ctx.srv(geom_normal_tex);
                                rc.slab0_srv = use_ssilvb_history ? ctx.srv(ssilvb_ao_prev_h) : 0xFFFFFFFFu;
                                rc.slab1_srv = use_ssilvb_history ? ctx.srv(ssilvb_gi_prev_h) : 0xFFFFFFFFu;
                                rc.motion_srv = use_ssilvb_history ? ctx.srv(motion_tex) : 0xFFFFFFFFu;
                                rc.prev_diffuse_srv = use_ssilvb_history ? ctx.srv(unjit_depth_prev_h) : 0xFFFFFFFFu;
                                rc.srv_aux0 = ctx.srv(ssilvb_bent_raw);
                                rc.srv_aux1 = ctx.srv(ssilvb_gi_raw);
                                rc.uav0 = ctx.uav(ssilvb_ao_curr_h);
                                rc.uav1 = ctx.uav(ssilvb_gi_curr_h);
                                ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                                ctx.core().CmdDispatch(ctx.cmd(), {sc_gx_cnt, sc_gy_cnt, 1u});
                                return {};
                            });
                    }

                    // 3-Layer GI: GI-1.2 ScreenProbes Reproject & Importance-Guided Primary Trace (`gi_update_gpu_ms[0]`) ->
                    // Secondary Hit Shade + Async Compute SHaRC + GI-1.2 Multi-Bounce Cache -> ResolveCells + 7x7 FilterProbes + SH2 (`gi_update_gpu_ms[1]`) ->
                    // Coplanar-Jittered Gather + Adaptive ReprojectGI + Separable FilterGI (`gi_gpu_ms[1]`)
                    const std::uint32_t probes_x = std::max(1u, (extent.width + 7u) / 8u);
                    const std::uint32_t probes_y = std::max(1u, (extent.height + 7u) / 8u);
                    const std::uint64_t total_probes = std::uint64_t(probes_x) * probes_y;
                    const std::uint64_t max_gi_rays = total_probes * 64u;

                    const auto gi_primary_rays_buf =
                        graph.create_buffer("gi_primary_rays", {.size = max_gi_rays * 16u, .structure_stride = 16u});
                    const auto gi_ray_cells_buf =
                        graph.create_buffer("gi_ray_cells", {.size = max_gi_rays * 4u, .structure_stride = 4u});
                    const auto probe_state_buf = graph.create_buffer(
                        "gi_probe_state",
                        {.size = std::max<std::uint64_t>(16ull, total_probes * 8ull), .structure_stride = 8u});
                    const auto probe_atlas_reproj = graph.create_texture(
                        "gi_probe_atlas_reproj",
                        {.format = nri::Format::RGBA16_SFLOAT, .width = probes_x * 8u, .height = probes_y * 8u});
                    const auto probe_atlas_tmp = graph.create_texture(
                        "gi_probe_atlas_tmp",
                        {.format = nri::Format::RGBA16_SFLOAT, .width = probes_x * 8u, .height = probes_y * 8u});
                    const auto probe_sh2_buf = graph.create_buffer(
                        "gi_probe_sh2",
                        {.size = std::max<std::uint64_t>(64ull, total_probes * 64ull), .structure_stride = 64u});
                    const auto gi_hits_buf =
                        graph.create_buffer("gi_ray_hits", {.size = max_gi_rays * 48u, .structure_stride = 48u});
                    const auto gi_counters_buf = graph.create_buffer(
                        "gi_counters", {.size = std::uint64_t(8u + arch_cnt) * 4u, .structure_stride = 4u});
                    const auto gi_arch_counts = graph.create_buffer(
                        "gi_arch_counts", {.size = std::uint64_t(arch_cnt) * 4u, .structure_stride = 4u});
                    const auto gi_arch_offsets = graph.create_buffer(
                        "gi_arch_offsets", {.size = std::uint64_t(arch_cnt) * 4u, .structure_stride = 4u});
                    const auto gi_arch_cursors = graph.create_buffer(
                        "gi_arch_cursors", {.size = std::uint64_t(arch_cnt) * 4u, .structure_stride = 4u});
                    const auto gi_sorted_hits =
                        graph.create_buffer("gi_sorted_hits", {.size = max_gi_rays * 4u, .structure_stride = 4u});
                    const auto gi_indirect = graph.create_buffer(
                        "gi_hit_indirect", {.size = std::uint64_t(arch_cnt) * 12u, .byte_address = true});
                    const auto gi_blur_vis = graph.create_texture(
                        "gi_blur_vis",
                        {.format = nri::Format::RG16_SFLOAT, .width = extent.width, .height = extent.height});
                    const auto gi_denoise_tmp = graph.create_texture(
                        "gi_denoise_tmp",
                        {.format = nri::Format::RGBA16_SFLOAT, .width = extent.width, .height = extent.height});
                    const auto gi_resolved = graph.create_texture(
                        "gi_irradiance",
                        {.format = nri::Format::RGBA16_SFLOAT, .width = extent.width, .height = extent.height});

                    graph.add_pass(
                        "LitGiPrimaryTrace", rhi::QueueKind::graphics,
                        [&](rdg::PassBuilder& b) {
                            b.read_texture(depth_handle);
                            b.read_texture(hzb_handle);
                            b.read_texture(geom_normal_tex);
                            b.read_texture(motion_tex);
                            if (use_lit_history)
                                b.read_texture(prev_diff_h);
                            if (use_gi_history) {
                                b.read_texture(probe_atlas_prev_h);
                                b.read_texture(probe_mask_prev_h);
                                b.read_texture(unjit_depth_prev_h);
                                b.read_texture(geom_normal_prev_h);
                            } else {
                                b.write_buffer(wrc_hash_keys_h);
                                b.write_buffer(wrc_voxel_keys_h);
                            }
                            b.read_buffer(gi_hits_buf);
                            b.write_buffer(wrc_accum_h);
                            b.write_buffer(gi_primary_rays_buf);
                            b.write_buffer(gi_ray_cells_buf);
                            b.write_buffer(probe_state_buf);
                            b.write_texture(probe_atlas_reproj);
                            b.write_buffer(gi_hits_buf);
                            b.write_buffer(gi_counters_buf);
                            b.write_buffer(gi_arch_counts);
                            b.write_buffer(gi_arch_offsets);
                            b.write_buffer(gi_arch_cursors);
                            b.write_buffer(gi_sorted_hits);
                            b.write_buffer(gi_indirect);
                        },
                        [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                            ctx.set_pipeline(s.lit_pipelines[static_cast<std::size_t>(State::LitStage::gi_four_layer)]);
                            nri::GlobalBarrierDesc uav_only_bar{
                                .before = {nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER},
                                .after = {nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER}};
                            nri::BarrierDesc bg_uav{.globals = &uav_only_bar,
                                                    .globalNum = 1u,
                                                    .buffers = nullptr,
                                                    .bufferNum = 0u,
                                                    .textures = nullptr,
                                                    .textureNum = 0u};
                            nri::GlobalBarrierDesc uav_srv_bar{
                                .before = {nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER},
                                .after = {nri::AccessBits::SHADER_RESOURCE_STORAGE | nri::AccessBits::SHADER_RESOURCE,
                                          nri::StageBits::COMPUTE_SHADER}};
                            nri::BarrierDesc bg_uav_srv{.globals = &uav_srv_bar,
                                                        .globalNum = 1u,
                                                        .buffers = nullptr,
                                                        .bufferNum = 0u,
                                                        .textures = nullptr,
                                                        .textureNum = 0u};

                            // Mode 1: Clear GI counters, SHaRC accumulators, & cold-start SHaRC hash keys
                            GiRootConsts rc{};
                            rc.frame_cb_srv = frame_cb_srv;
                            rc.gi_mode = 1u;
                            rc.uav0 = ctx.uav(gi_counters_buf);
                            rc.uav1 = ctx.uav(wrc_accum_h);
                            rc.uav2 = use_gi_history ? 0xFFFFFFFFu : ctx.uav(wrc_voxel_keys_h);
                            ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                            ctx.core().CmdDispatch(ctx.cmd(), {4096u, 1u, 1u});
                            ctx.core().CmdBarrier(ctx.cmd(), bg_uav);

                            // Mode 2: GI-1.2 ReprojectScreenProbes + Defensive CDF SampleScreenProbes + Exact Hi-Z + DXR
                            rc.gi_mode = 2u;
                            rc.depth_srv = ctx.srv(depth_handle);
                            rc.hzb_srv = use_gi_history ? ctx.srv(geom_normal_prev_h) : 0xFFFFFFFFu;
                            rc.geom_normal_srv = ctx.srv(geom_normal_tex);
                            rc.slab0_srv = use_gi_history ? ctx.srv(probe_atlas_prev_h) : 0xFFFFFFFFu;
                            rc.slab1_srv = use_gi_history ? ctx.srv(probe_mask_prev_h) : 0xFFFFFFFFu;
                            rc.motion_srv = ctx.srv(motion_tex);
                            rc.prev_diffuse_srv = use_lit_history ? ctx.srv(prev_diff_h) : 0xFFFFFFFFu;
                            rc.uav0 = ctx.uav(gi_primary_rays_buf);
                            rc.uav1 = ctx.uav(gi_hits_buf);
                            rc.uav2 = ctx.uav(probe_atlas_reproj);
                            rc.uav3 = ctx.uav(gi_counters_buf);
                            rc.srv_aux0 = ctx.uav(probe_state_buf);
                            rc.srv_aux1 = ctx.uav(gi_ray_cells_buf);
                            rc.srv_aux2 = 0u;
                            ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                            ctx.core().CmdDispatch(ctx.cmd(), {probes_x, probes_y, 1u});
                            ctx.core().CmdBarrier(ctx.cmd(), bg_uav);

                            // Mode 3 (sub 0): Prefix-sum GI hit archetype bins
                            rc.gi_mode = 3u;
                            rc.uav0 = ctx.uav(gi_counters_buf);
                            rc.uav1 = ctx.uav(gi_arch_offsets);
                            rc.uav2 = ctx.uav(gi_arch_cursors);
                            rc.uav3 = ctx.uav(gi_indirect);
                            rc.srv_aux0 = ctx.uav(gi_arch_counts);
                            rc.srv_aux1 = 0xFFFFFFFFu;
                            rc.srv_aux2 = 0u;
                            ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                            ctx.core().CmdDispatch(ctx.cmd(), {1u, 1u, 1u});
                            ctx.core().CmdBarrier(ctx.cmd(), bg_uav_srv);

                            // Mode 3 (sub 1): Scatter GI hits into contiguous archetype bins
                            rc.srv_aux0 = ctx.srv(gi_hits_buf);
                            rc.srv_aux1 = ctx.uav(gi_sorted_hits);
                            rc.srv_aux2 = 1u;
                            ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                            const std::uint32_t max_rays_per_probe =
                                (frame.view.lit.gi_quality == rendering::GiQuality::ultra) ? 64u : 36u;
                            const std::uint32_t max_scatter_groups =
                                static_cast<std::uint32_t>((total_probes * max_rays_per_probe + 63u) / 64u);
                            ctx.core().CmdDispatch(ctx.cmd(), dispatch_grid_2d(max_scatter_groups));
                            return {};
                        });

                    graph.add_pass(
                        "LitGiHitShade", rhi::QueueKind::graphics,
                        [&](rdg::PassBuilder& b) {
                            b.read_buffer(gi_hits_buf);
                            b.read_buffer(gi_arch_counts);
                            b.read_buffer(gi_arch_offsets);
                            b.read_buffer(gi_sorted_hits);
                            if (use_gi_history)
                                b.read_buffer(wrc_prev_h);
                            if (vsm_plan.active) {
                                b.read_texture(vsm_static_h);
                                b.read_texture(vsm_dynamic_h);
                            }
                            b.read_buffer(gi_indirect, nri::AccessBits::ARGUMENT_BUFFER, nri::StageBits::INDIRECT);
                            b.write_buffer(wrc_accum_h);
                            b.write_buffer(wrc_hash_keys_h);
                            b.write_buffer(wrc_voxel_keys_h);
                            b.write_buffer(gi_primary_rays_buf);
                            b.write_buffer(gi_counters_buf);
                        },
                        [=, &prog_set, &frame](const rdg::PassContext& ctx) -> rhi::Result<void> {
                            if (frame.view.lit.indirect_lighting) {
                                for (std::uint32_t a = 0u; a < static_cast<std::uint32_t>(prog_set.archetypes.size());
                                     ++a) {
                                    ctx.set_pipeline(prog_set.archetypes[a]->pipelines[static_cast<std::size_t>(
                                        detail::ProgramArchetype::Stage::gi_hit_cs)]);
                                    ArchetypeRootConsts rc{};
                                    rc.frame_cb_srv = frame_cb_srv;
                                    rc.frame_cb_idx = ctx.srv(gi_hits_buf);
                                    rc.material_inputs_srv = mat_batch->input_buffer.srv_bindless;
                                    rc.material_params_srv = mat_batch->parameter_buffer.srv_bindless;
                                    rc.material_surfaces_srv = mat_batch->surface_buffer.srv_bindless;
                                    rc.archetype_index = a;
                                    rc.bin_offset_or_cluster_srv = ctx.srv(gi_arch_offsets);
                                    rc.aux0_srv = ctx.srv(gi_arch_counts);
                                    rc.aux1_srv = ctx.srv(gi_sorted_hits);
                                    rc.aux2_uav = ctx.uav(gi_primary_rays_buf);
                                    rc.aux3_uav = ctx.uav(wrc_accum_h);
                                    rc.aux4_uav = ctx.uav(wrc_voxel_keys_h);
                                    rc.viewport_packed0 = use_gi_history ? ctx.srv(wrc_prev_h) : 0xFFFFFFFFu;
                                    rc.viewport_packed1 = ctx.uav(gi_counters_buf);
                                    ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                                    ctx.core().CmdDispatchIndirect(ctx.cmd(), *ctx.buffer(gi_indirect),
                                                                   std::uint64_t(a) * 12u);
                                }
                            }
                            return {};
                        });

                    // Async Compute Overlap (`QueueKind::compute` runs concurrently with `LitLightCluster .. LitDirectSubsurfaceY`
                    // on `QueueKind::graphics`, touching 100% disjoint resources so D3D12 GBV sees zero simultaneous hazards!):
                    graph.add_pass(
                        "LitGiWrcResolve", rhi::QueueKind::compute,
                        [&](rdg::PassBuilder& b) {
                            b.read_buffer(wrc_accum_h);
                            if (use_gi_history)
                                b.read_buffer(wrc_prev_h);
                            b.write_buffer(wrc_hash_keys_h);
                            b.write_buffer(wrc_voxel_keys_h);
                            b.write_buffer(wrc_curr_h);
                        },
                        [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                            ctx.set_pipeline(s.lit_pipelines[static_cast<std::size_t>(State::LitStage::gi_four_layer)]);
                            nri::GlobalBarrierDesc uav_bar{
                                .before = {nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER},
                                .after = {nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER}};
                            nri::BarrierDesc bg_uav{.globals = &uav_bar,
                                                    .globalNum = 1u,
                                                    .buffers = nullptr,
                                                    .bufferNum = 0u,
                                                    .textures = nullptr,
                                                    .textureNum = 0u};

                            // Mode 4 (sub 0): Read-only `hash_keys`/`voxel_keys` SHaRC + GI-1.2 Resolve & Adjacent-Level Seeding
                            GiRootConsts wrc_rc{};
                            wrc_rc.frame_cb_srv = frame_cb_srv;
                            wrc_rc.gi_mode = 4u;
                            wrc_rc.srv_aux0 = ctx.srv(wrc_accum_h);
                            wrc_rc.srv_aux1 = use_gi_history ? ctx.srv(wrc_prev_h) : 0xFFFFFFFFu;
                            wrc_rc.srv_aux2 = 0u;
                            wrc_rc.uav0 = ctx.uav(wrc_curr_h);
                            wrc_rc.uav1 = ctx.uav(wrc_voxel_keys_h);
                            ctx.push_constants(wrc_rc, nri::BindPoint::COMPUTE);
                            ctx.core().CmdDispatch(ctx.cmd(), {4096u, 1u, 1u});
                            ctx.core().CmdBarrier(ctx.cmd(), bg_uav);

                            // Mode 4 (sub 1): Barrier-separated Stale Slot Eviction (`stale_frames >= 64`)
                            wrc_rc.srv_aux2 = 1u;
                            ctx.push_constants(wrc_rc, nri::BindPoint::COMPUTE);
                            ctx.core().CmdDispatch(ctx.cmd(), {4096u, 1u, 1u});
                            return {};
                        });

                    // Direct Lighting & Shadows on `QueueKind::graphics` (executes concurrently with Async Compute `LitGiWrcResolve`!):
                    const std::uint32_t tiles_16x = (extent.width + 15u) / 16u;
                    const std::uint32_t tiles_16y = (extent.height + 15u) / 16u;
                    const bool run_contact_shadows = enable_contact_shadows;
                    const bool run_ray_shadows = frame.view.lit.shadows && (s.stats.scene_lit_shadow_sources > 0u);
                    committed_ray_shadows = run_ray_shadows;
                    const bool run_cluster_inspect = frame.view.lit.direct_cluster_inspect;

                    const auto cluster_list =
                        graph.create_buffer("light_clusters", {.size = std::uint64_t(tiles_16x) * tiles_16y * 132u * 4u,
                                                               .structure_stride = 4u});
                    const auto receiver_inspect_buf =
                        run_cluster_inspect
                            ? graph.create_buffer("receiver_inspect_hist", {.size = 67u * 4u, .structure_stride = 4u})
                            : rdg::BufferHandle{};
                    const auto contact_shadow_tex =
                        run_contact_shadows ? graph.create_texture("contact_shadow", {.format = nri::Format::R8_UNORM,
                                                                                      .width = extent.width,
                                                                                      .height = extent.height})
                                            : rdg::TextureHandle{};
                    const auto shadow_mask_tex =
                        run_ray_shadows ? graph.create_texture("shadow_mask", {.format = nri::Format::RG32_UINT,
                                                                               .width = extent.width,
                                                                               .height = extent.height})
                                        : rdg::TextureHandle{};
                    const auto penumbra_raw_tex =
                        run_ray_shadows ? graph.create_texture("penumbra_raw", {.format = nri::Format::RG8_UNORM,
                                                                                .width = extent.width,
                                                                                .height = extent.height})
                                        : rdg::TextureHandle{};
                    const auto penumbra_prev_h =
                        run_ray_shadows
                            ? import_persistent("penumbra_hist_prev", lc.penumbra_history[prev_p], cs_srv_state, false)
                            : rdg::TextureHandle{};
                    const auto penumbra_curr_h =
                        run_ray_shadows
                            ? import_persistent("penumbra_hist_curr", lc.penumbra_history[cur_p], cs_srv_state, true)
                            : rdg::TextureHandle{};
                    const auto penumbra_tmp_tex =
                        run_ray_shadows ? graph.create_texture("penumbra_tmp", {.format = nri::Format::RGBA16_SFLOAT,
                                                                                .width = extent.width,
                                                                                .height = extent.height})
                                        : rdg::TextureHandle{};
                    const auto penumbra_filtered_tex =
                        run_ray_shadows
                            ? graph.create_texture("penumbra_filtered", {.format = nri::Format::RGBA16_SFLOAT,
                                                                         .width = extent.width,
                                                                         .height = extent.height})
                            : rdg::TextureHandle{};
                    const std::uint32_t max_penumbra_items =
                        static_cast<std::uint32_t>(std::min<std::uint64_t>(total_px, 0x3FFFFFFFull));
                    const auto penumbra_queue_buf =
                        run_ray_shadows
                            ? graph.create_buffer("penumbra_queue", {.size = std::uint64_t(max_penumbra_items) * 24u,
                                                                     .structure_stride = 24u})
                            : rdg::BufferHandle{};
                    const auto penumbra_indirect_buf =
                        run_ray_shadows
                            ? graph.create_buffer(
                                  "penumbra_indirect",
                                  {.size = 16u, .byte_address = true, .usage = nri::BufferUsageBits::ARGUMENT})
                            : rdg::BufferHandle{};
                    const auto direct_diff = graph.create_texture(
                        "direct_diffuse",
                        {.format = nri::Format::RGBA16_SFLOAT, .width = extent.width, .height = extent.height});
                    const auto direct_spec = graph.create_texture(
                        "direct_specular",
                        {.format = nri::Format::RGBA16_SFLOAT, .width = extent.width, .height = extent.height});
                    const auto scene_hdr = graph.create_texture(
                        "scene_color_hdr",
                        {.format = nri::Format::RGBA16_SFLOAT, .width = extent.width, .height = extent.height});

                    graph.add_pass(
                        "LitLightCluster", rhi::QueueKind::graphics,
                        [&](rdg::PassBuilder& b) {
                            b.read_texture(depth_handle);
                            b.write_buffer(cluster_list);
                            b.write_buffer(gpu_counters);
                        },
                        [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                            ctx.set_pipeline(s.lit_pipelines[static_cast<std::size_t>(State::LitStage::direct_light)]);
                            DirectLightRootConsts rc{};
                            rc.frame_cb_srv = frame_cb_srv;
                            rc.direct_mode = 0u;
                            rc.depth_srv = ctx.srv(depth_handle);
                            rc.cluster_list_uav_or_srv = ctx.uav(cluster_list);
                            rc.cluster_stats_uav = ctx.uav(gpu_counters);
                            ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                            ctx.core().CmdDispatch(ctx.cmd(), {tiles_16x, tiles_16y, 1u});
                            return {};
                        });

                    if (run_cluster_inspect) {
                        graph.add_pass(
                            "LitClusterInspect", rhi::QueueKind::graphics,
                            [&](rdg::PassBuilder& b) {
                                b.read_texture(depth_handle);
                                b.read_texture(slab0);
                                b.write_buffer(receiver_inspect_buf);
                            },
                            [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                                ctx.set_pipeline(
                                    s.lit_pipelines[static_cast<std::size_t>(State::LitStage::direct_light)]);
                                DirectLightRootConsts irc{};
                                irc.frame_cb_srv = frame_cb_srv;
                                irc.direct_mode = 3u;
                                irc.depth_srv = ctx.srv(depth_handle);
                                irc.slab0_srv = ctx.srv(slab0);
                                irc.contact_shadow_srv = 0u;
                                irc.cluster_stats_uav = ctx.uav(receiver_inspect_buf);
                                ctx.push_constants(irc, nri::BindPoint::COMPUTE);
                                ctx.core().CmdDispatch(ctx.cmd(), {1u, 1u, 1u});

                                nri::GlobalBarrierDesc uav_bar{.before = {nri::AccessBits::SHADER_RESOURCE_STORAGE,
                                                                          nri::StageBits::COMPUTE_SHADER},
                                                               .after = {nri::AccessBits::SHADER_RESOURCE_STORAGE,
                                                                         nri::StageBits::COMPUTE_SHADER}};
                                nri::BarrierDesc bg{.globals = &uav_bar,
                                                    .globalNum = 1u,
                                                    .buffers = nullptr,
                                                    .bufferNum = 0u,
                                                    .textures = nullptr,
                                                    .textureNum = 0u};
                                ctx.core().CmdBarrier(ctx.cmd(), bg);

                                irc.contact_shadow_srv = 1u;
                                ctx.push_constants(irc, nri::BindPoint::COMPUTE);
                                ctx.core().CmdDispatch(ctx.cmd(), {tiles_16x, tiles_16y, 1u});
                                return {};
                            });
                    }

                    if (run_contact_shadows || run_ray_shadows) {
                        const bool use_masked_shadow_pso =
                            scene.any_masked_instance && static_cast<bool>(prog_set.ray_shadow_masked_pso());
                        const std::uint32_t gx8 = sc_gx_cnt;
                        const std::uint32_t gy8 = sc_gy_cnt;

                        graph.add_pass(
                            "LitDirectShadowClassify", rhi::QueueKind::graphics,
                            [&](rdg::PassBuilder& b) {
                                b.read_texture(depth_handle);
                                b.read_texture(geom_normal_tex);
                                b.read_texture(slab0);
                                if (run_ray_shadows) {
                                    if (vsm_plan.active) {
                                        b.read_texture(vsm_static_h);
                                        b.read_texture(vsm_dynamic_h);
                                    }
                                    b.read_buffer(cluster_list);
                                    b.write_texture(shadow_mask_tex);
                                    b.write_texture(penumbra_raw_tex);
                                    b.write_buffer(penumbra_queue_buf);
                                    b.write_buffer(penumbra_indirect_buf);
                                }
                                if (run_contact_shadows)
                                    b.write_texture(contact_shadow_tex);
                            },
                            [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                                ctx.set_pipeline(
                                    s.lit_pipelines[static_cast<std::size_t>(State::LitStage::shadow_ray_opaque)]);
                                ShadowRootConsts csrc{};
                                csrc.frame_cb_srv = frame_cb_srv;
                                csrc.depth_srv = ctx.srv(depth_handle);
                                csrc.geom_normal_srv = ctx.srv(geom_normal_tex);
                                csrc.slab0_srv = ctx.srv(slab0);
                                if (run_contact_shadows) {
                                    csrc.shadow_mode = 0u;
                                    csrc.shadow_mask_uav = ctx.uav(contact_shadow_tex);
                                    ctx.push_constants(csrc, nri::BindPoint::COMPUTE);
                                    ctx.core().CmdDispatch(ctx.cmd(), {gx8, gy8, 1u});
                                }
                                if (run_ray_shadows) {
                                    nri::GlobalBarrierDesc uav_bar{.before = {nri::AccessBits::SHADER_RESOURCE_STORAGE,
                                                                              nri::StageBits::COMPUTE_SHADER},
                                                                   .after = {nri::AccessBits::SHADER_RESOURCE_STORAGE,
                                                                             nri::StageBits::COMPUTE_SHADER}};
                                    nri::BarrierDesc bg{.globals = &uav_bar,
                                                        .globalNum = 1u,
                                                        .buffers = nullptr,
                                                        .bufferNum = 0u,
                                                        .textures = nullptr,
                                                        .textureNum = 0u};

                                    csrc.cluster_list_srv = ctx.srv(cluster_list);
                                    csrc.shadow_mask_uav = ctx.uav(shadow_mask_tex);
                                    csrc.penumbra_tex_uav_or_srv = ctx.uav(penumbra_raw_tex);
                                    csrc.penumbra_queue_uav_or_srv = ctx.uav(penumbra_queue_buf);
                                    csrc.penumbra_args_uav_or_srv = ctx.uav(penumbra_indirect_buf);
                                    csrc.max_penumbra_items = max_penumbra_items;

                                    csrc.shadow_mode = 5u;
                                    ctx.push_constants(csrc, nri::BindPoint::COMPUTE);
                                    ctx.core().CmdDispatch(ctx.cmd(), {1u, 1u, 1u});
                                    ctx.core().CmdBarrier(ctx.cmd(), bg);

                                    csrc.shadow_mode = 1u;
                                    ctx.push_constants(csrc, nri::BindPoint::COMPUTE);
                                    ctx.core().CmdDispatch(ctx.cmd(), {gx8, gy8, 1u});
                                    ctx.core().CmdBarrier(ctx.cmd(), bg);

                                    csrc.shadow_mode = 2u;
                                    ctx.push_constants(csrc, nri::BindPoint::COMPUTE);
                                    ctx.core().CmdDispatch(ctx.cmd(), {1u, 1u, 1u});
                                }
                                return {};
                            });

                        if (run_ray_shadows) {
                            graph.add_pass(
                                "LitDirectShadowTrace", rhi::QueueKind::graphics,
                                [&](rdg::PassBuilder& b) {
                                    b.read_texture(depth_handle);
                                    b.read_texture(geom_normal_tex);
                                    b.read_buffer(cluster_list);
                                    b.read_buffer(penumbra_queue_buf);
                                    b.read_buffer(penumbra_indirect_buf,
                                                  nri::AccessBits::ARGUMENT_BUFFER | nri::AccessBits::SHADER_RESOURCE,
                                                  nri::StageBits::INDIRECT | nri::StageBits::COMPUTE_SHADER);
                                    b.write_texture(shadow_mask_tex);
                                    b.write_texture(penumbra_raw_tex);
                                    b.write_buffer(gpu_counters);
                                },
                                [=, &s, &prog_set](const rdg::PassContext& ctx) -> rhi::Result<void> {
                                    if (use_masked_shadow_pso) {
                                        ctx.set_pipeline(prog_set.ray_shadow_masked_pso());
                                        ArchetypeRootConsts src{};
                                        src.frame_cb_srv = frame_cb_srv;
                                        src.frame_cb_idx = ctx.srv(cluster_list);
                                        src.material_inputs_srv = mat_batch->input_buffer.srv_bindless;
                                        src.material_params_srv = mat_batch->parameter_buffer.srv_bindless;
                                        src.material_surfaces_srv = mat_batch->surface_buffer.srv_bindless;
                                        src.archetype_index = ctx.srv(penumbra_indirect_buf);
                                        src.bin_offset_or_cluster_srv = ctx.srv(depth_handle);
                                        src.aux0_srv = ctx.srv(geom_normal_tex);
                                        src.aux1_srv = 0xFFFFFFFFu;
                                        src.aux2_uav = ctx.uav(shadow_mask_tex);
                                        src.aux3_uav = ctx.uav(gpu_counters);
                                        src.aux4_uav = ctx.uav(penumbra_raw_tex);
                                        src.aux5_uav = ctx.srv(penumbra_queue_buf);
                                        src.pass_flags = 3u;
                                        src.viewport_packed0 = max_penumbra_items;
                                        ctx.push_constants(src, nri::BindPoint::COMPUTE);
                                        ctx.core().CmdDispatchIndirect(ctx.cmd(), *ctx.buffer(penumbra_indirect_buf),
                                                                       0u);
                                    } else {
                                        ctx.set_pipeline(s.lit_pipelines[static_cast<std::size_t>(
                                            State::LitStage::shadow_ray_opaque)]);
                                        ShadowRootConsts csrc{};
                                        csrc.frame_cb_srv = frame_cb_srv;
                                        csrc.shadow_mode = 3u;
                                        csrc.depth_srv = ctx.srv(depth_handle);
                                        csrc.geom_normal_srv = ctx.srv(geom_normal_tex);
                                        csrc.cluster_list_srv = ctx.srv(cluster_list);
                                        csrc.shadow_mask_uav = ctx.uav(shadow_mask_tex);
                                        csrc.ray_counter_uav = ctx.uav(gpu_counters);
                                        csrc.penumbra_tex_uav_or_srv = ctx.uav(penumbra_raw_tex);
                                        csrc.penumbra_queue_uav_or_srv = ctx.srv(penumbra_queue_buf);
                                        csrc.penumbra_args_uav_or_srv = ctx.srv(penumbra_indirect_buf);
                                        csrc.max_penumbra_items = max_penumbra_items;
                                        ctx.push_constants(csrc, nri::BindPoint::COMPUTE);
                                        ctx.core().CmdDispatchIndirect(ctx.cmd(), *ctx.buffer(penumbra_indirect_buf),
                                                                       0u);
                                    }
                                    return {};
                                });

                            graph.add_pass(
                                "LitDirectShadowTemporal", rhi::QueueKind::graphics,
                                [&](rdg::PassBuilder& b) {
                                    b.read_texture(depth_handle);
                                    b.read_texture(geom_normal_tex);
                                    b.read_texture(penumbra_raw_tex);
                                    if (use_penumbra_history) {
                                        b.read_texture(motion_tex);
                                        b.read_texture(unjit_depth_prev_h);
                                        b.read_texture(geom_normal_prev_h);
                                        b.read_texture(penumbra_prev_h);
                                    }
                                    b.write_texture(penumbra_curr_h);
                                },
                                [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                                    ctx.set_pipeline(
                                        s.lit_pipelines[static_cast<std::size_t>(State::LitStage::shadow_ray_opaque)]);
                                    ShadowRootConsts trc{};
                                    trc.frame_cb_srv = frame_cb_srv;
                                    trc.shadow_mode = 6u;
                                    trc.depth_srv = ctx.srv(depth_handle);
                                    trc.geom_normal_srv = ctx.srv(geom_normal_tex);
                                    trc.slab0_srv = use_penumbra_history ? ctx.srv(geom_normal_prev_h) : 0xFFFFFFFFu;
                                    trc.cluster_list_srv = use_penumbra_history ? ctx.srv(motion_tex) : 0xFFFFFFFFu;
                                    trc.penumbra_tex_uav_or_srv = ctx.srv(penumbra_raw_tex);
                                    trc.penumbra_queue_uav_or_srv =
                                        use_penumbra_history ? ctx.srv(penumbra_prev_h) : 0xFFFFFFFFu;
                                    trc.shadow_mask_uav = ctx.uav(penumbra_curr_h);
                                    trc.max_penumbra_items = 16u;
                                    ctx.push_constants(trc, nri::BindPoint::COMPUTE);
                                    ctx.core().CmdDispatch(ctx.cmd(), {gx8, gy8, 1u});
                                    return {};
                                });

                            auto add_shadow_atrous = [&](const char* pass_name, rdg::TextureHandle src_t,
                                                         rdg::TextureHandle dst_t, std::uint32_t step) {
                                graph.add_pass(
                                    pass_name, rhi::QueueKind::graphics,
                                    [&](rdg::PassBuilder& b) {
                                        b.read_texture(depth_handle);
                                        b.read_texture(geom_normal_tex);
                                        b.read_texture(slab0);
                                        b.read_texture(src_t);
                                        b.read_buffer(cluster_list);
                                        b.write_texture(dst_t);
                                    },
                                    [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                                        ctx.set_pipeline(s.lit_pipelines[static_cast<std::size_t>(
                                            State::LitStage::shadow_ray_opaque)]);
                                        ShadowRootConsts frc{};
                                        frc.frame_cb_srv = frame_cb_srv;
                                        frc.shadow_mode = 4u;
                                        frc.depth_srv = ctx.srv(depth_handle);
                                        frc.geom_normal_srv = ctx.srv(geom_normal_tex);
                                        frc.slab0_srv = ctx.srv(slab0);
                                        frc.cluster_list_srv = ctx.srv(cluster_list);
                                        frc.penumbra_tex_uav_or_srv = ctx.srv(src_t);
                                        frc.shadow_mask_uav = ctx.uav(dst_t);
                                        frc.max_penumbra_items = step;
                                        ctx.push_constants(frc, nri::BindPoint::COMPUTE);
                                        ctx.core().CmdDispatch(ctx.cmd(), {gx8, gy8, 1u});
                                        return {};
                                    });
                            };

                            add_shadow_atrous("LitDirectShadowFilter1", penumbra_curr_h, penumbra_filtered_tex, 1u);
                            add_shadow_atrous("LitDirectShadowFilter2", penumbra_filtered_tex, penumbra_tmp_tex, 2u);
                            add_shadow_atrous("LitDirectShadowFilter4", penumbra_tmp_tex, penumbra_filtered_tex, 4u);
                        }
                    }

                    graph.add_pass(
                        "LitDirectBrdf", rhi::QueueKind::graphics,
                        [&](rdg::PassBuilder& b) {
                            b.read_texture(depth_handle);
                            b.read_texture(geom_normal_tex);
                            b.read_texture(slab0);
                            b.read_texture(slab1);
                            if (run_ray_shadows) {
                                b.read_texture(shadow_mask_tex);
                                b.read_texture(penumbra_filtered_tex);
                                b.write_buffer(gpu_counters);
                            }
                            if (run_contact_shadows)
                                b.read_texture(contact_shadow_tex);
                            b.read_buffer(cluster_list);
                            b.write_texture(direct_diff);
                            b.write_texture(direct_spec);
                        },
                        [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                            ctx.set_pipeline(s.lit_pipelines[static_cast<std::size_t>(State::LitStage::direct_light)]);
                            DirectLightRootConsts drc{};
                            drc.frame_cb_srv = frame_cb_srv;
                            drc.direct_mode = 1u;
                            drc.depth_srv = ctx.srv(depth_handle);
                            drc.slab0_srv = ctx.srv(slab0);
                            drc.slab1_srv = ctx.srv(slab1);
                            drc.geom_normal_srv = ctx.srv(geom_normal_tex);
                            drc.shadow_mask_srv = run_ray_shadows ? ctx.srv(shadow_mask_tex) : 0xFFFFFFFFu;
                            drc.penumbra_mask_srv = run_ray_shadows ? ctx.srv(penumbra_filtered_tex) : 0xFFFFFFFFu;
                            drc.contact_shadow_srv = run_contact_shadows ? ctx.srv(contact_shadow_tex) : 0xFFFFFFFFu;
                            drc.ssilvb_srv = 0xFFFFFFFFu;
                            drc.cluster_list_uav_or_srv = ctx.srv(cluster_list);
                            drc.direct_diff_uav = ctx.uav(direct_diff);
                            drc.direct_spec_uav = ctx.uav(direct_spec);
                            drc.cluster_stats_uav = run_ray_shadows ? ctx.uav(gpu_counters) : 0xFFFFFFFFu;
                            ctx.push_constants(drc, nri::BindPoint::COMPUTE);
                            ctx.core().CmdDispatch(ctx.cmd(), {sc_tx_cnt, sc_ty_cnt, 1u});
                            return {};
                        });

                    const auto sss_temp = graph.create_texture(
                        "direct_sss_temp",
                        {.format = nri::Format::RGBA16_SFLOAT, .width = extent.width, .height = extent.height});

                    graph.add_pass(
                        "LitDirectSubsurfaceX", rhi::QueueKind::graphics,
                        [&](rdg::PassBuilder& b) {
                            b.read_texture(depth_handle);
                            b.read_texture(slab0);
                            b.read_texture(slab1);
                            b.read_texture(direct_diff);
                            b.read_buffer(sss_indirect_args, nri::AccessBits::ARGUMENT_BUFFER,
                                          nri::StageBits::INDIRECT);
                            b.write_texture(sss_temp);
                        },
                        [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                            ctx.set_pipeline(s.lit_pipelines[static_cast<std::size_t>(State::LitStage::direct_light)]);
                            DirectLightRootConsts src{};
                            src.frame_cb_srv = frame_cb_srv;
                            src.direct_mode = 2u;
                            src.depth_srv = ctx.srv(depth_handle);
                            src.slab0_srv = ctx.srv(slab0);
                            src.slab1_srv = ctx.srv(slab1);
                            src.shadow_mask_srv = ctx.srv(direct_diff);
                            src.contact_shadow_srv = 0u;
                            src.direct_diff_uav = ctx.uav(sss_temp);
                            ctx.push_constants(src, nri::BindPoint::COMPUTE);
                            ctx.core().CmdDispatchIndirect(ctx.cmd(), *ctx.buffer(sss_indirect_args), 0u);
                            return {};
                        });

                    graph.add_pass(
                        "LitDirectSubsurfaceY", rhi::QueueKind::graphics,
                        [&](rdg::PassBuilder& b) {
                            b.read_texture(depth_handle);
                            b.read_texture(slab0);
                            b.read_texture(slab1);
                            b.read_texture(sss_temp);
                            b.read_buffer(sss_indirect_args, nri::AccessBits::ARGUMENT_BUFFER,
                                          nri::StageBits::INDIRECT);
                            b.write_texture(direct_diff);
                        },
                        [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                            ctx.set_pipeline(s.lit_pipelines[static_cast<std::size_t>(State::LitStage::direct_light)]);
                            DirectLightRootConsts src{};
                            src.frame_cb_srv = frame_cb_srv;
                            src.direct_mode = 2u;
                            src.depth_srv = ctx.srv(depth_handle);
                            src.slab0_srv = ctx.srv(slab0);
                            src.slab1_srv = ctx.srv(slab1);
                            src.shadow_mask_srv = ctx.srv(sss_temp);
                            src.contact_shadow_srv = 1u;
                            src.direct_diff_uav = ctx.uav(direct_diff);
                            ctx.push_constants(src, nri::BindPoint::COMPUTE);
                            ctx.core().CmdDispatchIndirect(ctx.cmd(), *ctx.buffer(sss_indirect_args), 0u);
                            return {};
                        });

                    // Join Async Compute (`wrc_curr_h`, `wrc_hash_keys_h`, `wrc_voxel_keys_h`) back onto `QueueKind::graphics`
                    // for GI-1.2 `ResolveCells` + `BlendScreenProbes` -> `PopulateProbeMaskMip` -> `FilterProbes (7x1 + 1x7)` -> `ProjectScreenProbes`!
                    const std::uint32_t probe_mask_mips = lc.probe_mask[cur_p].mip_count;

                    graph.add_pass(
                        "LitGiProbeBlend", rhi::QueueKind::graphics,
                        [&](rdg::PassBuilder& b) {
                            b.read_texture(depth_handle);
                            b.read_texture(geom_normal_tex);
                            b.read_texture(probe_atlas_reproj);
                            b.read_buffer(gi_primary_rays_buf);
                            b.read_buffer(gi_ray_cells_buf);
                            b.read_buffer(probe_state_buf);
                            b.read_buffer(wrc_curr_h);
                            b.read_buffer(wrc_hash_keys_h);
                            b.read_buffer(wrc_voxel_keys_h);
                            b.write_texture(probe_atlas_curr_h);
                            b.write_texture(probe_mask_curr_h, nri::StageBits::COMPUTE_SHADER,
                                            rdg::SubresourceRange::single_mip(0u));
                        },
                        [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                            ctx.set_pipeline(s.lit_pipelines[static_cast<std::size_t>(State::LitStage::gi_four_layer)]);
                            GiRootConsts rc{};
                            rc.frame_cb_srv = frame_cb_srv;
                            rc.gi_mode = 7u;
                            rc.depth_srv = ctx.srv(depth_handle);
                            rc.geom_normal_srv = ctx.srv(geom_normal_tex);
                            rc.slab0_srv = ctx.srv(probe_state_buf);
                            rc.slab1_srv = ctx.srv(probe_atlas_reproj);
                            rc.srv_aux0 = ctx.srv(gi_primary_rays_buf);
                            rc.srv_aux1 = ctx.srv(gi_ray_cells_buf);
                            rc.uav0 = ctx.uav(probe_atlas_curr_h);
                            rc.uav1 = ctx.uav(probe_mask_curr_h, 0u);
                            ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                            ctx.core().CmdDispatch(ctx.cmd(), {probes_x, probes_y, 1u});
                            return {};
                        });

                    for (std::uint32_t m = 1u; m < probe_mask_mips; ++m) {
                        const std::uint32_t src_w = std::max(1u, probes_x >> (m - 1u));
                        const std::uint32_t src_h = std::max(1u, probes_y >> (m - 1u));
                        const std::uint32_t dst_w = std::max(1u, probes_x >> m);
                        const std::uint32_t dst_h = std::max(1u, probes_y >> m);
                        const auto src_mip = static_cast<std::uint16_t>(m - 1u);
                        const auto dst_mip = static_cast<std::uint16_t>(m);
                        graph.add_pass(
                            "LitGiProbeMaskMip", rhi::QueueKind::graphics,
                            [&](rdg::PassBuilder& b) {
                                b.read_texture(probe_mask_curr_h, nri::StageBits::COMPUTE_SHADER,
                                               nri::Layout::SHADER_RESOURCE,
                                               rdg::SubresourceRange::single_mip(src_mip));
                                b.write_texture(probe_mask_curr_h, nri::StageBits::COMPUTE_SHADER,
                                                rdg::SubresourceRange::single_mip(dst_mip));
                            },
                            [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                                ctx.set_pipeline(
                                    s.lit_pipelines[static_cast<std::size_t>(State::LitStage::gi_four_layer)]);
                                GiRootConsts rc{};
                                rc.frame_cb_srv = frame_cb_srv;
                                rc.gi_mode = 8u;
                                rc.srv_aux0 = ctx.srv(probe_mask_curr_h, src_mip);
                                rc.srv_aux1 = src_w;
                                rc.srv_aux2 = src_h;
                                rc.uav0 = ctx.uav(probe_mask_curr_h, dst_mip);
                                rc.uav2 = dst_w;
                                rc.uav3 = dst_h;
                                ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                                ctx.core().CmdDispatch(ctx.cmd(), {(dst_w + 7u) / 8u, (dst_h + 7u) / 8u, 1u});
                                return {};
                            });
                    }

                    auto add_probe_filter = [&](const char* pass_name, rdg::TextureHandle src_atlas,
                                                rdg::TextureHandle dst_atlas, std::uint32_t axis) {
                        graph.add_pass(
                            pass_name, rhi::QueueKind::graphics,
                            [&](rdg::PassBuilder& b) {
                                b.read_texture(depth_handle);
                                b.read_texture(geom_normal_tex);
                                b.read_texture(src_atlas);
                                b.read_texture(probe_mask_curr_h, nri::StageBits::COMPUTE_SHADER,
                                               nri::Layout::SHADER_RESOURCE, rdg::SubresourceRange::single_mip(0u));
                                b.write_texture(dst_atlas);
                            },
                            [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                                ctx.set_pipeline(
                                    s.lit_pipelines[static_cast<std::size_t>(State::LitStage::gi_four_layer)]);
                                GiRootConsts rc{};
                                rc.frame_cb_srv = frame_cb_srv;
                                rc.gi_mode = 9u;
                                rc.depth_srv = ctx.srv(depth_handle);
                                rc.geom_normal_srv = ctx.srv(geom_normal_tex);
                                rc.srv_aux0 = ctx.srv(src_atlas);
                                rc.srv_aux1 = ctx.srv(probe_mask_curr_h, 0u);
                                rc.srv_aux2 = axis;
                                rc.uav0 = ctx.uav(dst_atlas);
                                ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                                ctx.core().CmdDispatch(ctx.cmd(), {probes_x, probes_y, 1u});
                                return {};
                            });
                    };
                    add_probe_filter("LitGiProbeFilterX", probe_atlas_curr_h, probe_atlas_tmp, 0u);
                    add_probe_filter("LitGiProbeFilterY", probe_atlas_tmp, probe_atlas_curr_h, 1u);

                    graph.add_pass(
                        "LitGiProbeProject", rhi::QueueKind::graphics,
                        [&](rdg::PassBuilder& b) {
                            b.read_texture(depth_handle);
                            b.read_texture(geom_normal_tex);
                            b.read_texture(probe_atlas_curr_h);
                            b.read_texture(probe_mask_curr_h, nri::StageBits::COMPUTE_SHADER,
                                           nri::Layout::SHADER_RESOURCE, rdg::SubresourceRange::single_mip(0u));
                            b.write_buffer(probe_sh2_buf);
                        },
                        [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                            ctx.set_pipeline(s.lit_pipelines[static_cast<std::size_t>(State::LitStage::gi_four_layer)]);
                            GiRootConsts rc{};
                            rc.frame_cb_srv = frame_cb_srv;
                            rc.gi_mode = 10u;
                            rc.depth_srv = ctx.srv(depth_handle);
                            rc.geom_normal_srv = ctx.srv(geom_normal_tex);
                            rc.srv_aux0 = ctx.srv(probe_atlas_curr_h);
                            rc.srv_aux1 = ctx.srv(probe_mask_curr_h, 0u);
                            rc.uav0 = ctx.uav(probe_sh2_buf);
                            ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                            ctx.core().CmdDispatch(ctx.cmd(), {probes_x, probes_y, 1u});
                            return {};
                        });

                    graph.add_pass(
                        "LitGiGather", rhi::QueueKind::graphics,
                        [&](rdg::PassBuilder& b) {
                            b.read_texture(depth_handle);
                            b.read_texture(geom_normal_tex);
                            b.read_texture(slab0);
                            b.read_texture(motion_tex);
                            if (ssilvb_active)
                                b.read_texture(ssilvb_ao_curr_h);
                            b.read_texture(probe_mask_curr_h);
                            b.read_buffer(probe_sh2_buf);
                            b.read_buffer(wrc_curr_h);
                            b.read_buffer(wrc_hash_keys_h);
                            b.read_buffer(wrc_voxel_keys_h);
                            if (use_gi_history) {
                                b.read_texture(gi_hist_prev_h);
                                b.read_texture(gi_delta_prev_h);
                                b.read_texture(unjit_depth_prev_h);
                                b.read_texture(geom_normal_prev_h);
                            }
                            b.write_texture(gi_hist_cur_h);
                            b.write_texture(gi_delta_cur_h);
                            b.write_texture(gi_blur_vis);
                        },
                        [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                            ctx.set_pipeline(s.lit_pipelines[static_cast<std::size_t>(State::LitStage::gi_four_layer)]);
                            GiRootConsts rc{};
                            rc.frame_cb_srv = frame_cb_srv;
                            rc.gi_mode = 5u;
                            rc.depth_srv = ctx.srv(depth_handle);
                            rc.hzb_srv = use_gi_history ? ctx.srv(geom_normal_prev_h) : 0xFFFFFFFFu;
                            rc.geom_normal_srv = ctx.srv(geom_normal_tex);
                            rc.slab0_srv = ctx.srv(slab0);
                            rc.slab1_srv = use_gi_history ? ctx.srv(gi_delta_prev_h) : 0xFFFFFFFFu;
                            rc.motion_srv = ctx.srv(motion_tex);
                            rc.prev_diffuse_srv = use_gi_history ? ctx.srv(unjit_depth_prev_h) : 0xFFFFFFFFu;
                            rc.uav0 = ctx.uav(gi_hist_cur_h);
                            rc.uav1 = ctx.uav(gi_delta_cur_h);
                            rc.uav2 = ctx.uav(gi_blur_vis);
                            rc.uav3 = ssilvb_active ? ctx.srv(ssilvb_ao_curr_h) : 0xFFFFFFFFu;
                            rc.srv_aux0 = ctx.srv(probe_sh2_buf);
                            rc.srv_aux1 = use_gi_history ? ctx.srv(gi_hist_prev_h) : 0xFFFFFFFFu;
                            rc.srv_aux2 = ctx.srv(probe_mask_curr_h);
                            ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                            ctx.core().CmdDispatch(ctx.cmd(), {sc_gx_cnt, sc_gy_cnt, 1u});
                            return {};
                        });

                    graph.add_pass(
                        "LitGiDenoiseX", rhi::QueueKind::graphics,
                        [&](rdg::PassBuilder& b) {
                            b.read_texture(depth_handle);
                            b.read_texture(geom_normal_tex);
                            b.read_texture(slab0);
                            b.read_texture(gi_hist_cur_h);
                            b.read_texture(gi_blur_vis);
                            b.write_texture(gi_denoise_tmp);
                        },
                        [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                            ctx.set_pipeline(s.lit_pipelines[static_cast<std::size_t>(State::LitStage::gi_four_layer)]);
                            GiRootConsts rc{};
                            rc.frame_cb_srv = frame_cb_srv;
                            rc.gi_mode = 11u;
                            rc.depth_srv = ctx.srv(depth_handle);
                            rc.geom_normal_srv = ctx.srv(geom_normal_tex);
                            rc.slab0_srv = ctx.srv(slab0);
                            rc.srv_aux0 = ctx.srv(gi_hist_cur_h);
                            rc.srv_aux1 = ctx.srv(gi_blur_vis);
                            rc.srv_aux2 = 0u;
                            rc.uav0 = ctx.uav(gi_denoise_tmp);
                            ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                            ctx.core().CmdDispatch(ctx.cmd(), {sc_gx_cnt, sc_gy_cnt, 1u});
                            return {};
                        });

                    graph.add_pass(
                        "LitGiDenoiseY", rhi::QueueKind::graphics,
                        [&](rdg::PassBuilder& b) {
                            b.read_texture(depth_handle);
                            b.read_texture(geom_normal_tex);
                            b.read_texture(slab0);
                            b.read_texture(gi_denoise_tmp);
                            b.read_texture(gi_blur_vis);
                            if (ssilvb_active) {
                                b.read_texture(ssilvb_ao_curr_h);
                                b.read_texture(ssilvb_gi_curr_h);
                            }
                            b.write_texture(gi_resolved);
                            b.write_texture(gi_hist_cur_h);
                        },
                        [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                            ctx.set_pipeline(s.lit_pipelines[static_cast<std::size_t>(State::LitStage::gi_four_layer)]);
                            GiRootConsts rc{};
                            rc.frame_cb_srv = frame_cb_srv;
                            rc.gi_mode = 11u;
                            rc.depth_srv = ctx.srv(depth_handle);
                            rc.hzb_srv = ssilvb_active ? ctx.srv(ssilvb_gi_curr_h) : 0xFFFFFFFFu;
                            rc.geom_normal_srv = ctx.srv(geom_normal_tex);
                            rc.slab0_srv = ctx.srv(slab0);
                            rc.prev_diffuse_srv = ssilvb_active ? ctx.srv(ssilvb_ao_curr_h) : 0xFFFFFFFFu;
                            rc.srv_aux0 = ctx.srv(gi_denoise_tmp);
                            rc.srv_aux1 = ctx.srv(gi_blur_vis);
                            rc.srv_aux2 = 1u;
                            rc.uav0 = ctx.uav(gi_resolved);
                            rc.uav1 = ctx.uav(gi_hist_cur_h);
                            ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                            ctx.core().CmdDispatch(ctx.cmd(), {sc_gx_cnt, sc_gy_cnt, 1u});
                            return {};
                        });

                    // 3-Tier Coat-Aware Reflections + GI-1.2 Variance-Guided Luma-Karis 3-Step A-Trous (`scene_lit_gpu_ms[5]`)
                    const auto refl_raw = graph.create_texture(
                        "refl_raw",
                        {.format = nri::Format::RGBA16_SFLOAT, .width = extent.width, .height = extent.height});
                    const auto refl_filtered = graph.create_texture(
                        "refl_filtered",
                        {.format = nri::Format::RGBA16_SFLOAT, .width = extent.width, .height = extent.height});
                    const auto refl_moments_tmp =
                        frame.view.lit.reflections
                            ? graph.create_texture(
                                  "refl_moments_tmp",
                                  {.format = nri::Format::RG16_SFLOAT, .width = extent.width, .height = extent.height})
                            : rdg::TextureHandle{};

                    graph.add_pass(
                        "LitReflectionsTrace", rhi::QueueKind::graphics,
                        [&](rdg::PassBuilder& b) {
                            b.read_texture(depth_handle);
                            b.read_texture(hzb_handle);
                            b.read_texture(slab0);
                            b.read_texture(slab1);
                            b.read_texture(geom_normal_tex);
                            if (ssilvb_active)
                                b.read_texture(ssilvb_ao_curr_h);
                            if (frame.view.lit.indirect_lighting) {
                                b.read_texture(probe_atlas_curr_h);
                                b.read_texture(probe_mask_curr_h);
                            }
                            b.read_texture(gi_resolved);
                            b.read_texture(motion_tex);
                            b.read_buffer(wrc_curr_h);
                            b.read_buffer(wrc_hash_keys_h);
                            b.read_buffer(wrc_voxel_keys_h);
                            if (use_lit_history)
                                b.read_texture(taa_hist_prev_h);
                            if (use_refl_history) {
                                b.read_texture(refl_hist_prev_h);
                                b.read_texture(refl_moments_prev_h);
                                b.read_texture(unjit_depth_prev_h);
                                b.read_texture(geom_normal_prev_h);
                            }
                            b.write_texture(refl_raw);
                            b.write_texture(refl_hist_cur_h);
                            b.write_texture(refl_moments_curr_h);
                            b.write_buffer(gpu_counters);
                        },
                        [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                            ctx.set_pipeline(
                                s.lit_pipelines[static_cast<std::size_t>(State::LitStage::reflections_compose)]);
                            ComposeRootConsts rc{};
                            rc.frame_cb_srv = frame_cb_srv;
                            rc.comp_mode = 0u | (use_refl_history ? ((ctx.srv(geom_normal_prev_h) + 1u) << 4u) : 0u);
                            rc.depth_srv = ctx.srv(depth_handle);
                            rc.slab0_srv = ctx.srv(slab0);
                            rc.slab1_srv = ctx.srv(slab1);
                            rc.slab2_srv = ctx.srv(geom_normal_tex);
                            rc.tile_emissive_srv = ssilvb_active ? ctx.srv(ssilvb_ao_curr_h) : 0xFFFFFFFFu;
                            rc.direct_diff_srv = ctx.uav(refl_moments_curr_h);
                            rc.direct_spec_srv = ctx.uav(refl_hist_cur_h);
                            rc.gi_srv = ctx.srv(gi_resolved);
                            rc.motion_srv = ctx.srv(motion_tex);
                            rc.refl_srv = use_refl_history ? ctx.srv(refl_hist_prev_h) : 0xFFFFFFFFu;
                            rc.prev_hdr_or_refl_srv = use_lit_history ? ctx.srv(taa_hist_prev_h) : 0xFFFFFFFFu;
                            rc.uav0 = ctx.uav(refl_raw);
                            rc.uav1 = ctx.uav(gpu_counters);
                            rc.atrous_step = use_refl_history ? ctx.srv(refl_moments_prev_h) : 0xFFFFFFFFu;
                            ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                            ctx.core().CmdDispatch(ctx.cmd(), {sc_gx_cnt, sc_gy_cnt, 1u});
                            return {};
                        });

                    auto add_refl_atrous = [&](const char* pass_name, rdg::TextureHandle src_t,
                                               rdg::TextureHandle dst_t, rdg::TextureHandle mom_src,
                                               rdg::TextureHandle mom_dst, std::uint32_t step) {
                        graph.add_pass(
                            pass_name, rhi::QueueKind::graphics,
                            [&](rdg::PassBuilder& b) {
                                b.read_texture(depth_handle);
                                b.read_texture(slab0);
                                if (step != 0u) {
                                    b.read_texture(slab1);
                                    b.read_texture(geom_normal_tex);
                                }
                                b.read_texture(src_t);
                                if (mom_src.valid())
                                    b.read_texture(mom_src);
                                b.write_texture(dst_t);
                                if (mom_dst.valid())
                                    b.write_texture(mom_dst);
                            },
                            [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                                ctx.set_pipeline(
                                    s.lit_pipelines[static_cast<std::size_t>(State::LitStage::reflections_compose)]);
                                ComposeRootConsts rc{};
                                rc.frame_cb_srv = frame_cb_srv;
                                rc.comp_mode = 1u;
                                rc.depth_srv = ctx.srv(depth_handle);
                                rc.slab0_srv = ctx.srv(slab0);
                                rc.slab1_srv = mom_src.valid() ? ctx.srv(mom_src) : 0xFFFFFFFFu;
                                rc.slab2_srv = (step != 0u) ? ctx.srv(geom_normal_tex) : 0xFFFFFFFFu;
                                rc.gi_srv = (step != 0u) ? ctx.srv(slab1) : 0xFFFFFFFFu;
                                rc.refl_srv = ctx.srv(src_t);
                                rc.uav0 = ctx.uav(dst_t);
                                rc.uav1 = mom_dst.valid() ? ctx.uav(mom_dst) : 0xFFFFFFFFu;
                                rc.atrous_step = step;
                                ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                                ctx.core().CmdDispatch(ctx.cmd(), {sc_gx_cnt, sc_gy_cnt, 1u});
                                return {};
                            });
                    };

                    if (frame.view.lit.reflections) {
                        add_refl_atrous("LitReflectionsFilter1", refl_raw, refl_filtered, refl_moments_curr_h,
                                        refl_moments_tmp, 1u);
                        add_refl_atrous("LitReflectionsFilter2", refl_filtered, refl_raw, refl_moments_tmp,
                                        refl_moments_curr_h, 2u);
                        add_refl_atrous("LitReflectionsFilter4", refl_raw, refl_filtered, refl_moments_curr_h, {}, 4u);
                    } else {
                        add_refl_atrous("LitReflectionsFilter", refl_raw, refl_filtered, {}, {}, 0u);
                    }

                    if (fog_active) {
                        const bool use_froxel_prev = use_lit_history && lc.has_froxel_history;
                        graph.add_pass(
                            "LitFroxelScatter", rhi::QueueKind::graphics,
                            [&](rdg::PassBuilder& b) {
                                if (vsm_plan.active) {
                                    b.read_texture(vsm_static_h);
                                    b.read_texture(vsm_dynamic_h);
                                }
                                if (use_froxel_prev)
                                    b.read_texture(froxel_prev_h);
                                b.write_texture(froxel_curr_h);
                            },
                            [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                                ctx.set_pipeline(
                                    s.lit_pipelines[static_cast<std::size_t>(State::LitStage::reflections_compose)]);
                                ComposeRootConsts rc{};
                                rc.frame_cb_srv = frame_cb_srv;
                                rc.comp_mode = 3u;
                                rc.prev_hdr_or_refl_srv = use_froxel_prev ? ctx.srv(froxel_prev_h) : 0xFFFFFFFFu;
                                rc.uav0 = ctx.uav(froxel_curr_h);
                                ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                                ctx.core().CmdDispatch(ctx.cmd(), {20u, 12u, 64u});
                                return {};
                            });

                        graph.add_pass(
                            "LitFroxelIntegrate", rhi::QueueKind::graphics,
                            [&](rdg::PassBuilder& b) {
                                b.read_texture(froxel_curr_h);
                                b.write_texture(froxel_lut_h);
                            },
                            [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                                ctx.set_pipeline(
                                    s.lit_pipelines[static_cast<std::size_t>(State::LitStage::reflections_compose)]);
                                ComposeRootConsts rc{};
                                rc.frame_cb_srv = frame_cb_srv;
                                rc.comp_mode = 4u;
                                rc.refl_srv = ctx.srv(froxel_curr_h);
                                rc.uav0 = ctx.uav(froxel_lut_h);
                                ctx.push_constants(rc, nri::BindPoint::COMPUTE);
                                ctx.core().CmdDispatch(ctx.cmd(), {20u, 12u, 1u});
                                return {};
                            });
                    }

                    graph.add_pass(
                        "LitDirectCompose", rhi::QueueKind::graphics,
                        [&](rdg::PassBuilder& b) {
                            b.read_texture(depth_handle);
                            b.read_texture(slab0);
                            b.read_texture(slab1);
                            b.read_texture(slab2);
                            b.read_texture(direct_diff);
                            b.read_texture(direct_spec);
                            b.read_texture(gi_resolved);
                            b.read_texture(refl_filtered);
                            if (ssilvb_active)
                                b.read_texture(ssilvb_ao_curr_h);
                            if (fog_active)
                                b.read_texture(froxel_lut_h);
                            b.read_buffer(tile_emissive_mask);
                            b.write_texture(scene_hdr);
                            b.write_texture(prev_diff_h);
                            b.write_texture(unjit_depth_curr_h);
                        },
                        [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                            ctx.set_pipeline(
                                s.lit_pipelines[static_cast<std::size_t>(State::LitStage::reflections_compose)]);
                            ComposeRootConsts crc{};
                            crc.frame_cb_srv = frame_cb_srv;
                            crc.comp_mode = 2u;
                            crc.depth_srv = ctx.srv(depth_handle);
                            crc.slab0_srv = ctx.srv(slab0);
                            crc.slab1_srv = ctx.srv(slab1);
                            crc.slab2_srv = ctx.srv(slab2);
                            crc.tile_emissive_srv = ctx.srv(tile_emissive_mask);
                            crc.direct_diff_srv = ctx.srv(direct_diff);
                            crc.direct_spec_srv = ctx.srv(direct_spec);
                            crc.gi_srv = ctx.srv(gi_resolved);
                            crc.refl_srv = ctx.srv(refl_filtered);
                            crc.motion_srv = ssilvb_active ? ctx.srv(ssilvb_ao_curr_h) : 0xFFFFFFFFu;
                            crc.uav0 = ctx.uav(scene_hdr);
                            crc.uav1 = ctx.uav(prev_diff_h);
                            crc.atrous_step = ctx.uav(unjit_depth_curr_h);
                            ctx.push_constants(crc, nri::BindPoint::COMPUTE);
                            ctx.core().CmdDispatch(ctx.cmd(),
                                                   {(extent.width + 7u) / 8u, (extent.height + 7u) / 8u, 1u});
                            return {};
                        });

                    // Temporal HDR Resolve (`scene_lit_gpu_ms[6]`: `Atelier TAAU`)
                    const auto resolved_hdr = graph.create_texture(
                        "resolved_hdr",
                        {.format = nri::Format::RGBA16_SFLOAT, .width = extent.width, .height = extent.height});

                    graph.add_pass(
                        "LitTemporalResolve", rhi::QueueKind::graphics,
                        [&](rdg::PassBuilder& b) {
                            b.read_texture(scene_hdr);
                            b.read_texture(motion_tex);
                            b.read_texture(depth_handle);
                            b.read_texture(prev_depth_tex);
                            if (use_temporal_jitter && use_lit_history) {
                                b.read_texture(taa_hist_prev_h);
                                b.read_texture(unjit_depth_prev_h);
                            }
                            b.write_texture(resolved_hdr);
                            b.write_texture(taa_hist_cur_h);
                        },
                        [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                            ctx.set_pipeline(
                                s.lit_pipelines[static_cast<std::size_t>(State::LitStage::post_taau_unjitter)]);
                            PostRootConsts prc{};
                            prc.frame_cb_srv = frame_cb_srv;
                            prc.post_mode = 0u;
                            prc.src0_srv = ctx.srv(scene_hdr);
                            prc.src1_srv =
                                (use_temporal_jitter && use_lit_history) ? ctx.srv(taa_hist_prev_h) : 0xFFFFFFFFu;
                            prc.src2_srv = ctx.srv(motion_tex);
                            prc.src3_srv = ctx.srv(depth_handle);
                            prc.src_w = ctx.srv(prev_depth_tex);
                            prc.dst0_uav = ctx.uav(resolved_hdr);
                            prc.dst1_uav = ctx.uav(taa_hist_cur_h);
                            prc.dst_w = extent.width;
                            prc.dst_h = extent.height;
                            ctx.push_constants(prc, nri::BindPoint::COMPUTE);
                            ctx.core().CmdDispatch(ctx.cmd(),
                                                   {(extent.width + 7u) / 8u, (extent.height + 7u) / 8u, 1u});
                            return {};
                        });

                    // Post-Processing (`scene_lit_gpu_ms[7]`, scope `"scene_lit_post"`):
                    // Jiménez 13-Tap Soft-Knee Karis Mipmapped Bloom -> Khronos PBR Neutral / AgX Tonemap -> Authentic Jiménez SMAA!
                    const std::uint32_t bloom_bw0 = std::max(1u, extent.width >> 1u);
                    const std::uint32_t bloom_bh0 = std::max(1u, extent.height >> 1u);
                    const std::uint32_t bloom_mips =
                        std::min<std::uint32_t>(6u, detail::calc_mip_count(bloom_bw0, bloom_bh0));
                    rdg::TextureHandle bloom_down_tex{};
                    rdg::TextureHandle bloom_up_tex{};
                    if (frame.view.lit.bloom) {
                        bloom_down_tex =
                            graph.create_texture("bloom_down", {.format = nri::Format::RGBA16_SFLOAT,
                                                                .width = bloom_bw0,
                                                                .height = bloom_bh0,
                                                                .mip_count = static_cast<std::uint16_t>(bloom_mips)});
                        if (bloom_mips > 1u) {
                            bloom_up_tex = graph.create_texture(
                                "bloom_up", {.format = nri::Format::RGBA16_SFLOAT,
                                             .width = bloom_bw0,
                                             .height = bloom_bh0,
                                             .mip_count = static_cast<std::uint16_t>(bloom_mips - 1u)});
                        }

                        std::uint32_t sw = extent.width;
                        std::uint32_t sh = extent.height;
                        for (std::uint32_t m = 0u; m < bloom_mips; ++m) {
                            const std::uint32_t dw = std::max(1u, bloom_bw0 >> m);
                            const std::uint32_t dh = std::max(1u, bloom_bh0 >> m);
                            const auto mip_u16 = static_cast<std::uint16_t>(m);
                            graph.add_pass(
                                "LitBloomDown", rhi::QueueKind::graphics,
                                [&](rdg::PassBuilder& b) {
                                    b.set_scope("scene_lit_post");
                                    if (m == 0u) {
                                        b.read_texture(resolved_hdr);
                                    } else {
                                        b.read_texture(
                                            bloom_down_tex, nri::StageBits::COMPUTE_SHADER,
                                            nri::Layout::SHADER_RESOURCE,
                                            rdg::SubresourceRange::single_mip(static_cast<std::uint16_t>(m - 1u)));
                                    }
                                    b.write_texture(bloom_down_tex, nri::StageBits::COMPUTE_SHADER,
                                                    rdg::SubresourceRange::single_mip(mip_u16));
                                },
                                [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                                    ctx.set_pipeline(
                                        s.lit_pipelines[static_cast<std::size_t>(State::LitStage::post_taau_unjitter)]);
                                    PostRootConsts prc{};
                                    prc.frame_cb_srv = frame_cb_srv;
                                    prc.post_mode = 1u;
                                    prc.src0_srv = (m == 0u)
                                                       ? ctx.srv(resolved_hdr)
                                                       : ctx.srv(bloom_down_tex, static_cast<std::uint16_t>(m - 1u));
                                    prc.dst0_uav = ctx.uav(bloom_down_tex, mip_u16);
                                    prc.src_w = sw;
                                    prc.src_h = sh;
                                    prc.dst_w = dw;
                                    prc.dst_h = dh;
                                    prc.apply_ign_quant = (m == 0u) ? 1u : 0u;
                                    ctx.push_constants(prc, nri::BindPoint::COMPUTE);
                                    ctx.core().CmdDispatch(ctx.cmd(), {(dw + 7u) / 8u, (dh + 7u) / 8u, 1u});
                                    return {};
                                });
                            sw = dw;
                            sh = dh;
                        }

                        for (int m = static_cast<int>(bloom_mips) - 2; m >= 0; --m) {
                            const auto um = static_cast<std::uint16_t>(m);
                            const auto um_lo = static_cast<std::uint16_t>(m + 1);
                            const bool lo_from_down = (static_cast<std::uint32_t>(m + 1) == bloom_mips - 1u);
                            const std::uint32_t lw = std::max(1u, bloom_bw0 >> (m + 1));
                            const std::uint32_t lh = std::max(1u, bloom_bh0 >> (m + 1));
                            const std::uint32_t dw = std::max(1u, bloom_bw0 >> m);
                            const std::uint32_t dh = std::max(1u, bloom_bh0 >> m);
                            graph.add_pass(
                                "LitBloomUp", rhi::QueueKind::graphics,
                                [&](rdg::PassBuilder& b) {
                                    b.set_scope("scene_lit_post");
                                    if (lo_from_down) {
                                        b.read_texture(bloom_down_tex, nri::StageBits::COMPUTE_SHADER,
                                                       nri::Layout::SHADER_RESOURCE,
                                                       rdg::SubresourceRange::single_mip(um_lo));
                                    } else {
                                        b.read_texture(bloom_up_tex, nri::StageBits::COMPUTE_SHADER,
                                                       nri::Layout::SHADER_RESOURCE,
                                                       rdg::SubresourceRange::single_mip(um_lo));
                                    }
                                    b.read_texture(bloom_down_tex, nri::StageBits::COMPUTE_SHADER,
                                                   nri::Layout::SHADER_RESOURCE, rdg::SubresourceRange::single_mip(um));
                                    b.write_texture(bloom_up_tex, nri::StageBits::COMPUTE_SHADER,
                                                    rdg::SubresourceRange::single_mip(um));
                                },
                                [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                                    ctx.set_pipeline(
                                        s.lit_pipelines[static_cast<std::size_t>(State::LitStage::post_taau_unjitter)]);
                                    PostRootConsts prc{};
                                    prc.frame_cb_srv = frame_cb_srv;
                                    prc.post_mode = 2u;
                                    prc.src0_srv =
                                        lo_from_down ? ctx.srv(bloom_down_tex, um_lo) : ctx.srv(bloom_up_tex, um_lo);
                                    prc.src1_srv = ctx.srv(bloom_down_tex, um);
                                    prc.dst0_uav = ctx.uav(bloom_up_tex, um);
                                    prc.src_w = lw;
                                    prc.src_h = lh;
                                    prc.dst_w = dw;
                                    prc.dst_h = dh;
                                    ctx.push_constants(prc, nri::BindPoint::COMPUTE);
                                    ctx.core().CmdDispatch(ctx.cmd(), {(dw + 7u) / 8u, (dh + 7u) / 8u, 1u});
                                    return {};
                                });
                        }
                    }

                    const bool run_smaa = (frame.view.lit.antialiasing == Antialiasing::smaa);
                    const auto tonemapped_sdr = graph.create_texture(
                        "tonemapped_sdr",
                        {.format = nri::Format::RGBA16_SFLOAT, .width = extent.width, .height = extent.height});
                    const auto smaa_edges = run_smaa
                                                ? graph.create_texture("smaa_edges", {.format = nri::Format::RG8_UNORM,
                                                                                      .width = extent.width,
                                                                                      .height = extent.height})
                                                : rdg::TextureHandle{};
                    const auto smaa_weights =
                        run_smaa ? graph.create_texture("smaa_weights", {.format = nri::Format::RGBA8_UNORM,
                                                                         .width = extent.width,
                                                                         .height = extent.height})
                                 : rdg::TextureHandle{};
                    const auto final_lit_sdr =
                        run_smaa ? graph.create_texture("final_lit_sdr", {.format = nri::Format::RGBA16_SFLOAT,
                                                                          .width = extent.width,
                                                                          .height = extent.height})
                                 : tonemapped_sdr;

                    const rdg::TextureHandle bloom_final_h = (bloom_mips > 1u) ? bloom_up_tex : bloom_down_tex;
                    graph.add_pass(
                        "LitTonemap", rhi::QueueKind::graphics,
                        [&](rdg::PassBuilder& b) {
                            b.set_scope("scene_lit_post");
                            b.read_texture(resolved_hdr);
                            if (frame.view.lit.bloom) {
                                b.read_texture(bloom_final_h, nri::StageBits::COMPUTE_SHADER,
                                               nri::Layout::SHADER_RESOURCE, rdg::SubresourceRange::single_mip(0u));
                            }
                            b.write_texture(tonemapped_sdr);
                        },
                        [=, &s, &frame](const rdg::PassContext& ctx) -> rhi::Result<void> {
                            ctx.set_pipeline(
                                s.lit_pipelines[static_cast<std::size_t>(State::LitStage::post_taau_unjitter)]);
                            PostRootConsts prc{};
                            prc.frame_cb_srv = frame_cb_srv;
                            prc.post_mode = 3u;
                            prc.src0_srv = ctx.srv(resolved_hdr);
                            prc.src1_srv = frame.view.lit.bloom ? ctx.srv(bloom_final_h, 0u) : 0xFFFFFFFFu;
                            prc.dst0_uav = ctx.uav(tonemapped_sdr);
                            prc.dst_w = extent.width;
                            prc.dst_h = extent.height;
                            prc.bloom_strength = 0.06f;
                            prc.apply_ign_quant = run_smaa ? 0u : 1u;
                            ctx.push_constants(prc, nri::BindPoint::COMPUTE);
                            ctx.core().CmdDispatch(ctx.cmd(),
                                                   {(extent.width + 7u) / 8u, (extent.height + 7u) / 8u, 1u});
                            return {};
                        });

                    if (run_smaa) {
                        graph.add_pass(
                            "LitSmaaEdges", rhi::QueueKind::graphics,
                            [&](rdg::PassBuilder& b) {
                                b.set_scope("scene_lit_post");
                                b.read_texture(tonemapped_sdr);
                                b.write_texture(smaa_edges);
                            },
                            [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                                ctx.set_pipeline(
                                    s.lit_pipelines[static_cast<std::size_t>(State::LitStage::post_taau_unjitter)]);
                                PostRootConsts prc{};
                                prc.frame_cb_srv = frame_cb_srv;
                                prc.post_mode = 4u;
                                prc.src0_srv = ctx.srv(tonemapped_sdr);
                                prc.dst0_uav = ctx.uav(smaa_edges);
                                prc.dst_w = extent.width;
                                prc.dst_h = extent.height;
                                ctx.push_constants(prc, nri::BindPoint::COMPUTE);
                                ctx.core().CmdDispatch(ctx.cmd(),
                                                       {(extent.width + 7u) / 8u, (extent.height + 7u) / 8u, 1u});
                                return {};
                            });

                        graph.add_pass(
                            "LitSmaaWeights", rhi::QueueKind::graphics,
                            [&](rdg::PassBuilder& b) {
                                b.set_scope("scene_lit_post");
                                b.read_texture(smaa_edges);
                                b.write_texture(smaa_weights);
                            },
                            [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                                ctx.set_pipeline(
                                    s.lit_pipelines[static_cast<std::size_t>(State::LitStage::post_taau_unjitter)]);
                                PostRootConsts prc{};
                                prc.frame_cb_srv = frame_cb_srv;
                                prc.post_mode = 5u;
                                prc.src0_srv = ctx.srv(smaa_edges);
                                prc.dst0_uav = ctx.uav(smaa_weights);
                                prc.dst_w = extent.width;
                                prc.dst_h = extent.height;
                                ctx.push_constants(prc, nri::BindPoint::COMPUTE);
                                ctx.core().CmdDispatch(ctx.cmd(),
                                                       {(extent.width + 7u) / 8u, (extent.height + 7u) / 8u, 1u});
                                return {};
                            });

                        graph.add_pass(
                            "LitSmaaBlend", rhi::QueueKind::graphics,
                            [&](rdg::PassBuilder& b) {
                                b.set_scope("scene_lit_post");
                                b.read_texture(tonemapped_sdr);
                                b.read_texture(smaa_weights);
                                b.write_texture(final_lit_sdr);
                            },
                            [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                                ctx.set_pipeline(
                                    s.lit_pipelines[static_cast<std::size_t>(State::LitStage::post_taau_unjitter)]);
                                PostRootConsts prc{};
                                prc.frame_cb_srv = frame_cb_srv;
                                prc.post_mode = 6u;
                                prc.src0_srv = ctx.srv(tonemapped_sdr);
                                prc.src1_srv = ctx.srv(smaa_weights);
                                prc.dst0_uav = ctx.uav(final_lit_sdr);
                                prc.dst_w = extent.width;
                                prc.dst_h = extent.height;
                                ctx.push_constants(prc, nri::BindPoint::COMPUTE);
                                ctx.core().CmdDispatch(ctx.cmd(),
                                                       {(extent.width + 7u) / 8u, (extent.height + 7u) / 8u, 1u});
                                return {};
                            });
                    }

                    // Blit quantized SDR to swapchain backbuffer RTV (`scene_lit_post`)
                    graph.add_pass(
                        "LitPresentBlit", rhi::QueueKind::graphics,
                        [&](rdg::PassBuilder& b) {
                            b.set_scope("scene_lit_post");
                            b.read_texture(final_lit_sdr, nri::StageBits::FRAGMENT_SHADER);
                            b.render_target(bb_handle, bb_load(), nri::StoreOp::STORE, zero_color);
                        },
                        [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                            ctx.begin_rendering();
                            if (scene_rv.scissor.right > scene_rv.scissor.left &&
                                scene_rv.scissor.bottom > scene_rv.scissor.top) {
                                set_scene_vp(ctx);
                                ctx.set_pipeline(s.present_blit_pipeline);
                                ViewportRootConstants rc{scene_rv.clip_transform, ctx.srv(final_lit_sdr), 0u};
                                ctx.push_constants(rc, nri::BindPoint::GRAPHICS);
                                ctx.core().CmdDraw(ctx.cmd(), {3u, 1u, 0u, 0u});
                            }
                            ctx.end_rendering();
                            return {};
                        });

                    const bool lit_editing_lines = (frame.view.wireframe == Wireframe::overlay) || scene.any_selected;
                    // Exact 3x3 Ray-Triangle UnjitterDepth CS when jittered AA is active AND guides/selection/capture need unjittered depth!
                    if (use_temporal_jitter && (grid_cam || lit_editing_lines || capture != nullptr)) {
                        const auto unjit_depth_tex = graph.create_texture(
                            "unjittered_depth_tmp",
                            {.format = nri::Format::R32_SFLOAT, .width = extent.width, .height = extent.height});

                        graph.add_pass(
                            "LitUnjitterDepth", rhi::QueueKind::graphics,
                            [&](rdg::PassBuilder& b) {
                                b.set_scope("scene_lit_post");
                                b.read_texture(vis_tex);
                                b.read_buffer(opaque_clusters);
                                b.read_buffer(masked_clusters);
                                b.write_texture(unjit_depth_tex);
                            },
                            [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                                ctx.set_pipeline(
                                    s.lit_pipelines[static_cast<std::size_t>(State::LitStage::post_taau_unjitter)]);
                                PostRootConsts prc{};
                                prc.frame_cb_srv = frame_cb_srv;
                                prc.post_mode = 7u;
                                prc.src0_srv = ctx.srv(vis_tex);
                                prc.src1_srv = ctx.srv(opaque_clusters);
                                prc.src2_srv = ctx.srv(masked_clusters);
                                prc.dst0_uav = ctx.uav(unjit_depth_tex);
                                prc.dst_w = extent.width;
                                prc.dst_h = extent.height;
                                ctx.push_constants(prc, nri::BindPoint::COMPUTE);
                                ctx.core().CmdDispatch(ctx.cmd(),
                                                       {(extent.width + 7u) / 8u, (extent.height + 7u) / 8u, 1u});
                                return {};
                            });

                        graph.add_pass(
                            "LitCopyUnjitterDepth", rhi::QueueKind::graphics,
                            [&](rdg::PassBuilder& b) {
                                b.set_scope("scene_lit_post");
                                b.copy_source_texture(unjit_depth_tex);
                                b.copy_dest_texture(depth_handle);
                            },
                            [=](const rdg::PassContext& ctx) -> rhi::Result<void> {
                                nri::TextureRegionDesc src_r{0u,
                                                             0u,
                                                             0u,
                                                             static_cast<nri::Dim_t>(extent.width),
                                                             static_cast<nri::Dim_t>(extent.height),
                                                             1u,
                                                             0u,
                                                             0u,
                                                             nri::PlaneBits::COLOR};
                                nri::TextureRegionDesc dst_r = src_r;
                                dst_r.planes = nri::PlaneBits::DEPTH;
                                ctx.core().CmdCopyTexture(ctx.cmd(), *ctx.texture(depth_handle), &dst_r,
                                                          *ctx.texture(unjit_depth_tex), &src_r);
                                return {};
                            });
                    }

                    // Copy GPU ray/cluster counters into non-blocking ReadbackSlotRing!
                    graph.add_pass(
                        "LitCounterReadback", rhi::QueueKind::graphics,
                        [&](rdg::PassBuilder& b) {
                            b.copy_source_buffer(gpu_counters);
                            b.copy_source_buffer(gi_counters_buf);
                            if (run_cluster_inspect)
                                b.copy_source_buffer(receiver_inspect_buf);
                            b.copy_dest_buffer(rb_counters_h);
                        },
                        [=](const rdg::PassContext& ctx) -> rhi::Result<void> {
                            // Copy [0..3] & [5] from gpu_counters, then overwrite [4] (gi_primary_rays = gi_counters[1]) & [6] (nee_rays = gi_counters[3]):
                            ctx.core().CmdCopyBuffer(ctx.cmd(), *ctx.buffer(rb_counters_h), 0u,
                                                     *ctx.buffer(gpu_counters), 0u, 32u);
                            ctx.core().CmdCopyBuffer(ctx.cmd(), *ctx.buffer(rb_counters_h), 16u,
                                                     *ctx.buffer(gi_counters_buf), 4u, 4u);
                            ctx.core().CmdCopyBuffer(ctx.cmd(), *ctx.buffer(rb_counters_h), 24u,
                                                     *ctx.buffer(gi_counters_buf), 12u, 4u);
                            if (run_cluster_inspect) {
                                ctx.core().CmdCopyBuffer(ctx.cmd(), *ctx.buffer(rb_counters_h), 32u,
                                                         *ctx.buffer(receiver_inspect_buf), 0u, 67u * 4u);
                            }
                            return {};
                        });

                    if (grid_cam || lit_editing_lines) {
                        const bool sel_only = (frame.view.wireframe != Wireframe::overlay);
                        graph.add_pass(
                            "SceneGuidesAndSelection", rhi::QueueKind::graphics,
                            [&](rdg::PassBuilder& b) {
                                if (lit_editing_lines) {
                                    if (masked_cnt > 0u)
                                        b.read_texture(vis_tex, nri::StageBits::FRAGMENT_SHADER);
                                    b.read_buffer(opaque_clusters, nri::AccessBits::SHADER_RESOURCE,
                                                  nri::StageBits::MESH_SHADER | nri::StageBits::FRAGMENT_SHADER);
                                    b.read_buffer(masked_clusters, nri::AccessBits::SHADER_RESOURCE,
                                                  nri::StageBits::MESH_SHADER | nri::StageBits::FRAGMENT_SHADER);
                                    if (sel_only) {
                                        b.read_buffer(selected_clusters, nri::AccessBits::SHADER_RESOURCE,
                                                      nri::StageBits::MESH_SHADER);
                                    }
                                    b.read_buffer(cull_counters, nri::AccessBits::SHADER_RESOURCE,
                                                  nri::StageBits::MESH_SHADER);
                                    b.read_buffer(mesh_indirect_args, nri::AccessBits::ARGUMENT_BUFFER,
                                                  nri::StageBits::INDIRECT);
                                }
                                b.render_target(bb_handle, bb_load(), nri::StoreOp::STORE, zero_color, true);
                                b.depth_target(depth_handle, nri::LoadOp::LOAD, nri::StoreOp::STORE, 0.0f, true);
                            },
                            [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                                ctx.begin_rendering();
                                if (scene_rv.scissor.right > scene_rv.scissor.left &&
                                    scene_rv.scissor.bottom > scene_rv.scissor.top) {
                                    set_scene_vp(ctx);
                                    if (grid_cam) {
                                        ctx.set_pipeline(s.grid_depth_pipeline);
                                        ViewportRootConstants grc{scene_rv.clip_transform, grid_cb_srv, 0u};
                                        ctx.push_constants(grc, nri::BindPoint::GRAPHICS);
                                        ctx.core().CmdDraw(ctx.cmd(), {3u, 1u, 0u, 0u});
                                        ++s.stats.viewport_draw_calls;
                                    }
                                    if (lit_editing_lines) {
                                        ctx.set_pipeline(s.wireframe_visible_ms);
                                        WireRootConsts wrc{};
                                        wrc.frame_cb_srv = frame_cb_srv;
                                        wrc.opaque_clusters_srv = ctx.srv(opaque_clusters);
                                        wrc.masked_clusters_srv = ctx.srv(masked_clusters);
                                        wrc.selected_clusters_srv = sel_only ? ctx.srv(selected_clusters) : 0xFFFFFFFFu;
                                        wrc.counters_srv = ctx.srv(cull_counters);
                                        wrc.visbuffer_srv = (masked_cnt > 0u) ? ctx.srv(vis_tex) : 0xFFFFFFFFu;
                                        wrc.wire_flags = (use_temporal_jitter ? 1u : 0u) | (sel_only ? 2u : 0u) |
                                                         ((masked_cnt > 0u) ? 8u : 0u);
                                        ctx.push_constants(wrc, nri::BindPoint::GRAPHICS);
                                        const std::uint64_t indirect_off =
                                            std::uint64_t((sel_only ? 2u : 1u) + masked_cnt) * 12u;
                                        ctx.mesh_shader().CmdDrawMeshTasksIndirect(
                                            ctx.cmd(), *ctx.buffer(mesh_indirect_args), indirect_off, 1u,
                                            sizeof(nri::DrawMeshTasksDesc), nullptr, 0u);
                                        ++s.stats.scene_draw_calls;
                                    }
                                }
                                ctx.end_rendering();
                                return {};
                            });
                    }

                    s.stats.scene_lit_resource_bytes = s.compute_lit_logical_bytes(scene, lc);
                }
            }
        } else if (has_viewport && grid_cam) {
            graph.add_pass(
                "ViewportReferenceGrid", rhi::QueueKind::graphics,
                [&](rdg::PassBuilder& b) {
                    b.render_target(bb_handle, bb_load(), nri::StoreOp::STORE, zero_color, true);
                },
                [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                    ctx.begin_rendering();
                    if (scene_rv.scissor.right > scene_rv.scissor.left &&
                        scene_rv.scissor.bottom > scene_rv.scissor.top) {
                        set_scene_vp(ctx);
                        ctx.set_pipeline(s.grid_pipeline);
                        ViewportRootConstants rc{scene_rv.clip_transform, grid_cb_srv, 0u};
                        ctx.push_constants(rc, nri::BindPoint::GRAPHICS);
                        ctx.core().CmdDraw(ctx.cmd(), {3u, 1u, 0u, 0u});
                        ++s.stats.viewport_draw_calls;
                    }
                    ctx.end_rendering();
                    return {};
                });
        }

        // Viewport Overlay + 2D Declarative UI
        if (!s.ui_instances.empty() && ui_instances_srv != rhi::bindless_invalid_index) {
            graph.add_pass(
                "DeclarativeUI", rhi::QueueKind::graphics,
                [&](rdg::PassBuilder& b) {
                    b.render_target(bb_handle, bb_load(), nri::StoreOp::STORE, zero_color, true);
                },
                [=, &s](const rdg::PassContext& ctx) -> rhi::Result<void> {
                    const nri::Viewport vp{
                        0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f,
                        1.0f, false};
                    const nri::Rect sc{0, 0, static_cast<nri::Dim_t>(extent.width),
                                       static_cast<nri::Dim_t>(extent.height)};
                    ctx.core().CmdSetViewports(ctx.cmd(), &vp, 1u);
                    ctx.core().CmdSetScissors(ctx.cmd(), &sc, 1u);
                    ctx.begin_rendering();
                    ctx.set_pipeline(s.ui_pipeline);

                    const auto draw_layer = [&](std::size_t first, std::size_t count, std::uint64_t& calls) {
                        if (count == 0u)
                            return;
                        UiRootConstants rc{ui_instances_srv,
                                           static_cast<std::uint32_t>(first),
                                           {static_cast<float>(extent.width), static_cast<float>(extent.height)}};
                        ctx.push_constants(rc, nri::BindPoint::GRAPHICS);
                        ctx.core().CmdDraw(ctx.cmd(), {6u, static_cast<std::uint32_t>(count), 0u, 0u});
                        ++calls;
                    };
                    draw_layer(0u, s.overlay_instances, s.stats.overlay_draw_calls);
                    draw_layer(s.overlay_instances, s.ui_instances.size() - s.overlay_instances, s.stats.ui_draw_calls);
                    ctx.end_rendering();
                    return {};
                });
        }

        // Optional Diagnostic Readback Pass (`Capture`: `rgba`, `depth`, and `motion` for Scene Lit!)
        nri::Buffer* rb_color = nullptr;
        nri::Buffer* rb_depth = nullptr;
        nri::Buffer* rb_motion = nullptr;
        const std::uint32_t rb_pitch = (extent.width * 4u + 255u) & ~255u;
        const std::uint64_t rb_bytes = std::uint64_t(rb_pitch) * extent.height;
        const auto& core = rhi::BackendAccess::core(*s.device);
        struct CaptureRbGuard {
            const nri::CoreInterface* core;

            nri::Buffer** c;

            nri::Buffer** d;

            nri::Buffer** m;

            void release() noexcept {
                if (*c) {
                    core->DestroyBuffer(*c);
                    *c = nullptr;
                }
                if (*d) {
                    core->DestroyBuffer(*d);
                    *d = nullptr;
                }
                if (*m) {
                    core->DestroyBuffer(*m);
                    *m = nullptr;
                }
            }

            ~CaptureRbGuard() { release(); }
        } rb_guard{&core, &rb_color, &rb_depth, &rb_motion};

        if (capture) {
            nri::BufferDesc rb_desc{};
            rb_desc.size = rb_bytes;
            ATELIER_TRY(
                check_nri(core.CreateCommittedBuffer(rhi::BackendAccess::nri_device(*s.device),
                                                     nri::MemoryLocation::HOST_READBACK, 0.0f, rb_desc, rb_color),
                          rhi::ErrorCode::out_of_memory, "Capture color readback allocation failed", true));
            const auto rb_color_h = graph.import_buffer("capture_color_rb", rb_color, {},
                                                        {nri::AccessBits::HOST_READ, nri::StageBits::HOST});

            rdg::BufferHandle rb_depth_h{};
            if (draw_scene) {
                ATELIER_TRY(
                    check_nri(core.CreateCommittedBuffer(rhi::BackendAccess::nri_device(*s.device),
                                                         nri::MemoryLocation::HOST_READBACK, 0.0f, rb_desc, rb_depth),
                              rhi::ErrorCode::out_of_memory, "Capture depth readback allocation failed", true));
                rb_depth_h = graph.import_buffer("capture_depth_rb", rb_depth, {},
                                                 {nri::AccessBits::HOST_READ, nri::StageBits::HOST});
            }

            rdg::BufferHandle rb_motion_h{};
            if (is_scene_lit && lit_motion_handle.valid()) {
                ATELIER_TRY(
                    check_nri(core.CreateCommittedBuffer(rhi::BackendAccess::nri_device(*s.device),
                                                         nri::MemoryLocation::HOST_READBACK, 0.0f, rb_desc, rb_motion),
                              rhi::ErrorCode::out_of_memory, "Capture motion readback allocation failed", true));
                rb_motion_h = graph.import_buffer("capture_motion_rb", rb_motion, {},
                                                  {nri::AccessBits::HOST_READ, nri::StageBits::HOST});
            }

            graph.add_pass(
                "CaptureReadback", rhi::QueueKind::graphics,
                [&](rdg::PassBuilder& b) {
                    b.copy_source_texture(bb_handle);
                    b.copy_dest_buffer(rb_color_h);
                    if (draw_scene) {
                        b.copy_source_texture(depth_handle);
                        b.copy_dest_buffer(rb_depth_h);
                    }
                    if (is_scene_lit && lit_motion_handle.valid()) {
                        b.copy_source_texture(lit_motion_handle);
                        b.copy_dest_buffer(rb_motion_h);
                    }
                },
                [=](const rdg::PassContext& ctx) -> rhi::Result<void> {
                    nri::TextureDataLayoutDesc layout{0u, rb_pitch, static_cast<std::uint32_t>(rb_bytes)};
                    nri::TextureRegionDesc c_reg{0u,
                                                 0u,
                                                 0u,
                                                 static_cast<nri::Dim_t>(extent.width),
                                                 static_cast<nri::Dim_t>(extent.height),
                                                 1u,
                                                 0u,
                                                 0u,
                                                 nri::PlaneBits::COLOR};
                    ctx.core().CmdReadbackTextureToBuffer(ctx.cmd(), *ctx.buffer(rb_color_h), layout,
                                                          *ctx.texture(bb_handle), c_reg);
                    if (draw_scene) {
                        nri::TextureRegionDesc d_reg = c_reg;
                        d_reg.planes = nri::PlaneBits::DEPTH;
                        ctx.core().CmdReadbackTextureToBuffer(ctx.cmd(), *ctx.buffer(rb_depth_h), layout,
                                                              *ctx.texture(depth_handle), d_reg);
                    }
                    if (is_scene_lit && lit_motion_handle.valid()) {
                        ctx.core().CmdReadbackTextureToBuffer(ctx.cmd(), *ctx.buffer(rb_motion_h), layout,
                                                              *ctx.texture(lit_motion_handle), c_reg);
                    }
                    return {};
                });
        }

        if (first_encode) {
            static_cast<void>(ATELIER_LOG(logging::Severity::info, "renderer", "renderer.first_encode_end",
                                          "First encoding returned: frame_id={} shading={}", frame.ui_frame.id,
                                          std::to_underlying(frame.view.shading)));
        }
        stage.reset();

        rhi::FrameInfo details{};
        {
            timing::Scope submitting{phases::frame};
            ATELIER_TRY(check_stop(frame.stop_requested, "submit"));
            const bool first_frame = std::exchange(s.first_frame, false);
            if (first_frame) {
                static_cast<void>(ATELIER_LOG(logging::Severity::info, "renderer", "renderer.first_frame_begin",
                                              "First submission beginning: frame_id={} shading={}", frame.ui_frame.id,
                                              std::to_underlying(frame.view.shading)));
            }

            const auto submit_start = timing::Clock::now();
            auto exec_res = s.executor.execute(
                graph, {.frame_id = frame.ui_frame.id,
                        .collect_timestamps = is_scene_lit,
                        .trace = frame.command_trace,
                        .trace_scope = frame.command_trace ? frame.trace_scope : std::string_view{}});
            const auto submit_dur = timing::Clock::now() - submit_start;
            if (!exec_res)
                return std::unexpected(Error{exec_res.error(), true});

            // Commit slot ring cursor, FencePoint, active program set, RenderScene delta copies, TLAS, VSM, and temporal history ONLY after GPU submission succeeded!
            s.slot_fences[slot_idx] = *exec_res;
            ++s.slot_cursor;
            if (draw_scene) {
                s.active_program_set = mat_batch->program_set;
                detail::RenderSceneTable::commit_submitted(scene);
            } else {
                s.has_pick_target = false;
            }

            if (draw_scene && is_scene_lit) {
                auto& lc = *lit_ctx;
                if (scene.pending_tlas_build) {
                    scene.tlas_gpu_built = true;
                    scene.tlas_scratch_gpu_used = true;
                    scene.pending_tlas_build = false;
                }
                if (vsm_plan.active)
                    lc.vsm_cache.commit_submitted(frame.scene_identity, masked_mat_hash);
                lc.history_parity = 1u - (lc.history_parity & 1u);
                if (commit_lit_history) {
                    lc.prev_unjittered_vp = next_prev_unjittered_vp;
                    lc.prev_view = next_prev_view;
                    lc.prev_projection = next_prev_projection;
                    lc.prev_scene_viewport = next_prev_scene_viewport;
                    lc.prev_camera_eye = next_prev_camera_eye;
                    lc.prev_exposure_scale = next_prev_exposure_scale;
                    lc.prev_indirect_lighting = frame.view.lit.indirect_lighting;
                    lc.prev_ssilvb = frame.view.lit.ssilvb;
                    lc.prev_reflections = frame.view.lit.reflections;
                    lc.prev_antialiasing = frame.view.lit.antialiasing;
                    lc.prev_sky_mode = frame.view.lit.sky_mode;
                    lc.prev_environment = frame.environment;
                    lc.prev_sky_rotation = frame.view.lit.sky_rotation;
                    lc.prev_sky_ptr = (frame.view.lit.sky_mode == rendering::SkyMode::hdri_sky_map)
                                          ? frame.view.lit.sky_map.get()
                                          : nullptr;
                    lc.has_temporal_history = true;
                    lc.has_prev_hzb = true;
                    lc.has_froxel_history = fog_active;
                    lc.has_penumbra_history = committed_ray_shadows;
                    lc.prev_shadow_lights_hash = shadow_lights_hash;
                    s.any_lit_rendered = true;
                } else {
                    lc.has_froxel_history = false;
                    lc.has_penumbra_history = false;
                }
            }

            if (counter_rb_slot) {
                counter_rb_slot->fence = *exec_res;
                counter_rb_slot->frame_id = frame.ui_frame.id;
                counter_rb_slot->epoch = seq;
                counter_rb_slot->tag = frame.view.lit.direct_cluster_inspect ? 1u : 0u;
                counter_rb_slot->pending = true;
            }

            if (capture) {
                auto waited = s.device->wait_idle();
                if (!waited)
                    return std::unexpected(Error{waited.error(), true});
                const auto* rgba_src = static_cast<const std::uint8_t*>(core.MapBuffer(*rb_color, 0u, rb_bytes));
                const auto* depth_src =
                    rb_depth ? static_cast<const std::uint8_t*>(core.MapBuffer(*rb_depth, 0u, rb_bytes)) : nullptr;
                const auto* motion_src =
                    rb_motion ? static_cast<const std::uint8_t*>(core.MapBuffer(*rb_motion, 0u, rb_bytes)) : nullptr;
                if (!rgba_src || (rb_depth && !depth_src) || (rb_motion && !motion_src)) {
                    if (rgba_src)
                        core.UnmapBuffer(*rb_color);
                    if (depth_src)
                        core.UnmapBuffer(*rb_depth);
                    if (motion_src)
                        core.UnmapBuffer(*rb_motion);
                    return std::unexpected(
                        make_error(rhi::ErrorCode::out_of_memory, "Capture readback MapBuffer failed", true));
                }

                for (std::uint32_t y = 0; y < extent.height; ++y) {
                    std::memcpy(pending_capture.rgba.data() + std::size_t(y) * extent.width * 4u,
                                rgba_src + std::size_t(y) * rb_pitch, extent.width * 4u);
                    if (depth_src) {
                        std::memcpy(pending_capture.depth.data() + std::size_t(y) * extent.width,
                                    depth_src + std::size_t(y) * rb_pitch, extent.width * 4u);
                    }
                    if (motion_src) {
                        const auto* row_h =
                            reinterpret_cast<const std::uint16_t*>(motion_src + std::size_t(y) * rb_pitch);
                        float* dst_m = pending_capture.motion.data() + std::size_t(y) * extent.width * 2u;
                        for (std::uint32_t x = 0; x < extent.width; ++x) {
                            dst_m[x * 2u + 0u] = fp16_to_fp32(row_h[x * 2u + 0u]);
                            dst_m[x * 2u + 1u] = fp16_to_fp32(row_h[x * 2u + 1u]);
                        }
                    }
                }
                core.UnmapBuffer(*rb_color);
                if (rb_depth)
                    core.UnmapBuffer(*rb_depth);
                if (rb_motion)
                    core.UnmapBuffer(*rb_motion);
                rb_guard.release();
            }

            const auto presented = rhi::BackendAccess::present(s.surface, s.options.vsync, &details);
            if (first_frame) {
                static_cast<void>(ATELIER_LOG(logging::Severity::info, "renderer", "renderer.first_frame_end",
                                              "First submission returned: frame_id={} shading={} success={}",
                                              frame.ui_frame.id, std::to_underlying(frame.view.shading),
                                              bool(presented)));
            }
            if (!presented)
                return std::unexpected(Error{presented.error(), true});

            const auto ms_to_ns = [](double ms) { return std::chrono::nanoseconds{std::llround(ms * 1000000.0)}; };
            timing::record(phases::submit, submit_dur);
            timing::record(phases::present, ms_to_ns(details.cpu.present_ms));
            timing::record(phases::wait, ms_to_ns(details.cpu.wait_ms));
            timing::record(phases::recycle, ms_to_ns(details.cpu.recycle_ms));
        }

        // Poll completed non-blocking GPU counters & map completed RDG GPU timestamps into `FrameStats`!
        if (is_scene_lit) {
            ATELIER_TRY(s.record_completed_counters());

            const auto& gpu_prof = s.executor.latest_gpu_profile();
            const State::CompletedCounterSample* matched_sample = nullptr;
            if (s.latest_counter_epoch > 0u) {
                std::uint64_t best_epoch = 0u;
                for (const auto& cand : s.completed_counter_history) {
                    if (cand.epoch == 0u)
                        continue;
                    if (gpu_prof.valid && cand.frame_id == gpu_prof.frame_id) {
                        matched_sample = &cand;
                        break;
                    }
                    if (!gpu_prof.valid && cand.epoch > best_epoch) {
                        best_epoch = cand.epoch;
                        matched_sample = &cand;
                    }
                }
                if (!matched_sample) {
                    for (const auto& cand : s.completed_counter_history) {
                        if (cand.epoch > best_epoch && (!gpu_prof.valid || cand.frame_id <= gpu_prof.frame_id)) {
                            best_epoch = cand.epoch;
                            matched_sample = &cand;
                        }
                    }
                }
            }
            if (matched_sample) {
                const auto& c = matched_sample->counters;
                s.stats.scene_lit_gi_visibility_rays = {c.gi_nee_shadow_rays, c.gi_gather_vis_rays};
                s.stats.scene_lit_shadow_rays =
                    std::uint64_t(c.direct_shadow_rays) + c.gi_nee_shadow_rays + c.gi_gather_vis_rays;
                s.stats.scene_lit_gi_primary_rays = c.probe_primary_rays;
                s.stats.scene_lit_direct_cluster_cells = c.cluster_cells;
                s.stats.scene_lit_direct_cluster_overflows = c.cluster_overflows;
                s.stats.scene_lit_direct_cluster_max_sources = c.cluster_max_sources;
                s.stats.scene_lit_reflection_work_items = c.reflection_work_items;
                if (matched_sample->inspect_enabled) {
                    s.stats.scene_lit_direct_receiver_max_sources = c.receiver_max_sources;
                    s.stats.scene_lit_direct_receiver_histogram = c.receiver_histogram;
                }
            }

            if (gpu_prof.valid) {
                s.stats.scene_lit_gpu_timing_valid = true;
                s.stats.scene_lit_gpu_sample_sequence = gpu_prof.sequence;
                s.stats.scene_lit_gpu_sample_frame_id = gpu_prof.frame_id;
                s.stats.scene_lit_gpu_union_ms = static_cast<float>(std::max(0.0, gpu_prof.union_gpu_ms));

                double gbuffer_ms = 0.0;
                double as_ms = 0.0;
                double cluster_ms = 0.0;
                double shadow_upd_ms = 0.0;
                double direct_shade_ms = 0.0;
                double gi_prim_ms = 0.0;
                double gi_hit_ms = 0.0;
                double gi_gather_ms = 0.0;
                double ssilvb_ms = 0.0;
                double refl_ms = 0.0;
                double temp_ms = 0.0;
                double post_ms = 0.0;
                std::uint32_t mask = 0u;

                for (const auto& ps : gpu_prof.passes) {
                    const double d = std::max(0.0, ps.duration_ms);
                    if (ps.name.starts_with("LitVisCull") || ps.name.starts_with("LitVisRaster") ||
                        ps.name.starts_with("LitHzbBuild") || ps.name == "LitVisUnpack" ||
                        ps.name == "LitMaterialShade") {
                        gbuffer_ms += d;
                        mask |= (1u << 0u);
                    } else if (ps.name == "ASBuildAndUploads") {
                        as_ms += d;
                        mask |= (1u << 1u);
                    } else if (ps.name == "LitLightCluster") {
                        cluster_ms += d;
                        mask |= (1u << 12u) | (1u << 2u);
                    } else if (ps.name.starts_with("LitVsmUpdate") || ps.name.starts_with("LitVsmPoolInit")) {
                        shadow_upd_ms += d;
                        mask |= (1u << 13u) | (1u << 2u);
                    } else if (ps.name.starts_with("LitDirect") || ps.name.starts_with("LitFroxel")) {
                        direct_shade_ms += d;
                        mask |= (1u << 14u) | (1u << 2u);
                    } else if (ps.name == "LitGiPrimaryTrace") {
                        gi_prim_ms += d;
                        mask |= (1u << 10u) | (1u << 8u) | (1u << 3u);
                    } else if (ps.name == "LitGiHitShade" || ps.name.starts_with("LitGiProbe") ||
                               ps.name == "LitGiWrcResolve") {
                        gi_hit_ms += d;
                        mask |= (1u << 11u) | (1u << 8u) | (1u << 3u);
                    } else if (ps.name == "LitGiGather" || ps.name.starts_with("LitGiDenoise")) {
                        gi_gather_ms += d;
                        mask |= (1u << 9u) | (1u << 3u);
                    } else if (ps.name.starts_with("LitSSILVB")) {
                        ssilvb_ms += d;
                        mask |= (1u << 4u);
                    } else if (ps.name.starts_with("LitReflections")) {
                        refl_ms += d;
                        mask |= (1u << 5u);
                    } else if (ps.name == "LitTemporalResolve") {
                        temp_ms += d;
                        mask |= (1u << 6u);
                    } else if (ps.name.starts_with("LitBloom") || ps.name == "LitTonemap" ||
                               ps.name.starts_with("LitSmaa") || ps.name == "LitPresentBlit" ||
                               ps.name == "LitUnjitterDepth" || ps.name == "LitCopyUnjitterDepth") {
                        post_ms += d;
                        mask |= (1u << 7u);
                    }
                }

                const float f_cluster = static_cast<float>(cluster_ms);
                const float f_sh_upd = static_cast<float>(shadow_upd_ms);
                const float f_dir_shade = static_cast<float>(direct_shade_ms);
                const float f_gi_prim = static_cast<float>(gi_prim_ms);
                const float f_gi_hit = static_cast<float>(gi_hit_ms);
                const float f_gi_upd = f_gi_prim + f_gi_hit;
                const float f_gi_gather = static_cast<float>(gi_gather_ms);

                s.stats.scene_lit_direct_cluster_gpu_ms = f_cluster;
                s.stats.scene_lit_vsm_gpu_ms = f_sh_upd;
                s.stats.scene_lit_direct_shading_gpu_ms = f_dir_shade;
                s.stats.scene_lit_gi_update_gpu_ms = {f_gi_prim, f_gi_hit};
                s.stats.scene_lit_gi_gpu_ms = {f_gi_upd, f_gi_gather};
                s.stats.scene_lit_gpu_ms = {
                    static_cast<float>(gbuffer_ms), static_cast<float>(as_ms),     f_cluster + f_sh_upd + f_dir_shade,
                    f_gi_upd + f_gi_gather,         static_cast<float>(ssilvb_ms), static_cast<float>(refl_ms),
                    static_cast<float>(temp_ms),    static_cast<float>(post_ms),
                };
                s.stats.scene_lit_gpu_pass_mask = mask;
            }
        }

        ATELIER_TRY(s.check_debug());
        FrameResult result{
            .accepted_extent = {}, .next_extent = {details.next.width, details.next.height}, .stats = s.stats};
        if (!details.occluded) {
            s.consecutive_occluded_frames = 0u;
            result.status = FrameStatus::accepted;
            result.accepted_id = frame.ui_frame.id;
            result.accepted_extent = extent;
            s.has_pick_target = draw_scene;
            s.pick_scissor = draw_scene ? scene_rv.scissor : ui::Clip{};
            if (capture)
                *capture = std::move(pending_capture);
        } else {
            result.status = FrameStatus::skipped;
            s.has_pick_target = false;
            s.consecutive_occluded_frames = 1u;
        }
        return result;
    }();

    if (attempt)
        return std::move(*attempt);
    if (const auto* cx = std::get_if<FrameCancelled>(&attempt.error())) {
        s.cancelled = true;
        const double elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
        static_cast<void>(ATELIER_LOG(logging::Severity::info, "renderer", "renderer.frame_cancelled",
                                      "Frame cancelled: frame_id={} phase={} elapsed_ms={}", frame.ui_frame.id,
                                      cx->phase, elapsed));
        return FrameResult{
            .status = FrameStatus::cancelled, .accepted_extent = {}, .next_extent = extent, .stats = s.stats};
    }
    Error e = std::get<Error>(std::move(attempt).error());
    if (encoding) {
        e.terminal = true;
        s.failure = e;
    }
    return std::unexpected(std::move(e));
}

Result<std::optional<PickPixelSample>> Renderer::pick_pixel(std::uint32_t x, std::uint32_t y) {
    if (!state_)
        return std::unexpected(make_error(rhi::ErrorCode::closed, "Renderer is closed"));
    ATELIER_TRY(state_->check());
    auto& s = *state_;
    if (!s.has_pick_target || !s.pick_id_tex.texture || !s.scene_depth.texture || x >= s.pick_id_tex.width ||
        y >= s.pick_id_tex.height) {
        return std::nullopt;
    }
    const int ix = static_cast<int>(x);
    const int iy = static_cast<int>(y);
    if (ix < s.pick_scissor.left || ix >= s.pick_scissor.right || iy < s.pick_scissor.top ||
        iy >= s.pick_scissor.bottom) {
        return std::nullopt;
    }

    auto& timeline = rhi::BackendAccess::timeline(*s.device);
    if (s.slot_cursor > 0u) {
        const auto& fence = s.slot_fences[(s.slot_cursor - 1u) % detail::HostSlotRing<UiInstance>::slot_count];
        auto waited = timeline.wait_all(fence, s.device->wait_timeout_ms());
        if (!waited)
            return std::unexpected(Error{waited.error(), true});
    }

    const auto& core = rhi::BackendAccess::core(*s.device);
    nri::Device& nri_dev = rhi::BackendAccess::nri_device(*s.device);
    const nri::DeviceDesc& dd = core.GetDeviceDesc(nri_dev);
    const std::uint32_t row_align = std::max<std::uint32_t>(256u, dd.memoryAlignment.uploadBufferTextureRow);
    const std::uint32_t slice_align = std::max<std::uint32_t>(512u, dd.memoryAlignment.uploadBufferTextureSlice);
    const std::uint64_t needed_bytes = std::max<std::uint64_t>(4096u, std::uint64_t(slice_align) + row_align);

    if (!s.pick_readback.buffer || s.pick_readback.capacity < needed_bytes) {
        s.pick_readback.retire(*s.device);
        nri::BufferDesc b_desc{};
        b_desc.size = needed_bytes;
        ATELIER_TRY(check_nri(core.CreateCommittedBuffer(nri_dev, nri::MemoryLocation::HOST_READBACK, 0.0f, b_desc,
                                                         s.pick_readback.buffer),
                              rhi::ErrorCode::out_of_memory, "Pick readback buffer allocation failed", true));
        s.pick_readback.capacity = needed_bytes;
    }

    nri::Queue* q = timeline.queue(rhi::QueueKind::graphics);
    nri::CommandAllocator* cmd_alloc = nullptr;
    nri::CommandBuffer* cmd = nullptr;
    struct CmdGuard {
        const nri::CoreInterface* core;

        nri::CommandAllocator** alloc;

        nri::CommandBuffer** buf;

        ~CmdGuard() {
            if (*buf)
                core->DestroyCommandBuffer(*buf);
            if (*alloc)
                core->DestroyCommandAllocator(*alloc);
        }
    } guard{&core, &cmd_alloc, &cmd};

    ATELIER_TRY(check_nri(core.CreateCommandAllocator(*q, cmd_alloc), rhi::ErrorCode::out_of_memory,
                          "Pick CommandAllocator failed", true));
    ATELIER_TRY(check_nri(core.CreateCommandBuffer(*cmd_alloc, cmd), rhi::ErrorCode::out_of_memory,
                          "Pick CommandBuffer failed", true));
    ATELIER_TRY(check_nri(core.BeginCommandBuffer(*cmd, nullptr), rhi::ErrorCode::execution_failed,
                          "Pick BeginCommandBuffer failed", true));

    nri::TextureDataLayoutDesc id_layout{0u, row_align, row_align};
    nri::TextureRegionDesc id_reg{static_cast<nri::Dim_t>(x), static_cast<nri::Dim_t>(y), 0u, 1u, 1u, 1u, 0u, 0u,
                                  nri::PlaneBits::COLOR};
    core.CmdReadbackTextureToBuffer(*cmd, *s.pick_readback.buffer, id_layout, *s.pick_id_tex.texture, id_reg);

    nri::TextureDataLayoutDesc z_layout{slice_align, row_align, row_align};
    nri::TextureRegionDesc z_reg = id_reg;
    z_reg.planes = nri::PlaneBits::DEPTH;
    core.CmdReadbackTextureToBuffer(*cmd, *s.pick_readback.buffer, z_layout, *s.scene_depth.texture, z_reg);

    ATELIER_TRY(
        check_nri(core.EndCommandBuffer(*cmd), rhi::ErrorCode::execution_failed, "Pick EndCommandBuffer failed", true));
    auto sub = timeline.submit(rhi::QueueKind::graphics, std::span{&cmd, 1u});
    if (!sub)
        return std::unexpected(Error{sub.error(), true});
    rhi::BackendAccess::reclaimer(*s.device).retire({
        .fence = timeline.current_point(),
        .command_buffer = std::exchange(cmd, nullptr),
        .command_allocator = std::exchange(cmd_alloc, nullptr),
    });
    auto done = timeline.wait(rhi::QueueKind::graphics, *sub, s.device->wait_timeout_ms());
    if (!done)
        return std::unexpected(Error{done.error(), true});

    const auto* mapped = static_cast<const std::uint8_t*>(core.MapBuffer(*s.pick_readback.buffer, 0u, needed_bytes));
    if (!mapped)
        return std::unexpected(make_error(rhi::ErrorCode::out_of_memory, "Pick MapBuffer failed", true));
    std::array<std::uint32_t, 2> id_pair{};
    float depth_val = 0.0f;
    std::memcpy(id_pair.data(), mapped, sizeof(id_pair));
    std::memcpy(&depth_val, mapped + slice_align, sizeof(depth_val));
    core.UnmapBuffer(*s.pick_readback.buffer);

    if (id_pair[0] == 0u)
        return std::nullopt;
    return PickPixelSample{
        .instance_index = id_pair[0] - 1u,
        .global_triangle_id = id_pair[1],
        .depth = depth_val,
    };
}

Result<std::vector<std::uint32_t>> Renderer::pick_box(std::uint32_t x0, std::uint32_t y0, std::uint32_t x1,
                                                      std::uint32_t y1) {
    if (!state_)
        return std::unexpected(make_error(rhi::ErrorCode::closed, "Renderer is closed"));
    ATELIER_TRY(state_->check());
    auto& s = *state_;
    std::vector<std::uint32_t> hits;
    if (!s.has_pick_target || !s.pick_id_tex.texture || s.pick_scissor.right <= s.pick_scissor.left ||
        s.pick_scissor.bottom <= s.pick_scissor.top) {
        return hits;
    }

    const int min_x = std::max(s.pick_scissor.left, static_cast<int>(std::min(x0, x1)));
    const int min_y = std::max(s.pick_scissor.top, static_cast<int>(std::min(y0, y1)));
    const int max_x = std::min(
        {s.pick_scissor.right - 1, static_cast<int>(s.pick_id_tex.width) - 1, static_cast<int>(std::max(x0, x1))});
    const int max_y = std::min(
        {s.pick_scissor.bottom - 1, static_cast<int>(s.pick_id_tex.height) - 1, static_cast<int>(std::max(y0, y1))});
    if (min_x > max_x || min_y > max_y)
        return hits;

    const auto rw = static_cast<std::uint32_t>(max_x - min_x + 1);
    const auto rh = static_cast<std::uint32_t>(max_y - min_y + 1);

    auto& timeline = rhi::BackendAccess::timeline(*s.device);
    if (s.slot_cursor > 0u) {
        const auto& fence = s.slot_fences[(s.slot_cursor - 1u) % detail::HostSlotRing<UiInstance>::slot_count];
        auto waited = timeline.wait_all(fence, s.device->wait_timeout_ms());
        if (!waited)
            return std::unexpected(Error{waited.error(), true});
    }

    const auto& core = rhi::BackendAccess::core(*s.device);
    nri::Device& nri_dev = rhi::BackendAccess::nri_device(*s.device);
    const nri::DeviceDesc& dd = core.GetDeviceDesc(nri_dev);
    const std::uint32_t row_align = std::max<std::uint32_t>(256u, dd.memoryAlignment.uploadBufferTextureRow);
    const auto row_pitch = static_cast<std::uint32_t>(detail::align_up_bytes(std::uint64_t(rw) * 8u, row_align));
    const std::uint64_t read_bytes = std::uint64_t(row_pitch) * rh;
    const std::uint64_t alloc_bytes = std::max<std::uint64_t>(4096u, std::bit_ceil(read_bytes));

    if (!s.pick_readback.buffer || s.pick_readback.capacity < read_bytes) {
        s.pick_readback.retire(*s.device);
        nri::BufferDesc b_desc{};
        b_desc.size = alloc_bytes;
        ATELIER_TRY(check_nri(core.CreateCommittedBuffer(nri_dev, nri::MemoryLocation::HOST_READBACK, 0.0f, b_desc,
                                                         s.pick_readback.buffer),
                              rhi::ErrorCode::out_of_memory, "Pick box readback buffer allocation failed", true));
        s.pick_readback.capacity = alloc_bytes;
    }

    nri::Queue* q = timeline.queue(rhi::QueueKind::graphics);
    nri::CommandAllocator* cmd_alloc = nullptr;
    nri::CommandBuffer* cmd = nullptr;
    struct CmdGuard {
        const nri::CoreInterface* core;

        nri::CommandAllocator** alloc;

        nri::CommandBuffer** buf;

        ~CmdGuard() {
            if (*buf)
                core->DestroyCommandBuffer(*buf);
            if (*alloc)
                core->DestroyCommandAllocator(*alloc);
        }
    } guard{&core, &cmd_alloc, &cmd};

    ATELIER_TRY(check_nri(core.CreateCommandAllocator(*q, cmd_alloc), rhi::ErrorCode::out_of_memory,
                          "Pick box CommandAllocator failed", true));
    ATELIER_TRY(check_nri(core.CreateCommandBuffer(*cmd_alloc, cmd), rhi::ErrorCode::out_of_memory,
                          "Pick box CommandBuffer failed", true));
    ATELIER_TRY(check_nri(core.BeginCommandBuffer(*cmd, nullptr), rhi::ErrorCode::execution_failed,
                          "Pick box BeginCommandBuffer failed", true));

    nri::TextureDataLayoutDesc id_layout{0u, row_pitch, static_cast<std::uint32_t>(read_bytes)};
    nri::TextureRegionDesc id_reg{static_cast<nri::Dim_t>(min_x),
                                  static_cast<nri::Dim_t>(min_y),
                                  0u,
                                  static_cast<nri::Dim_t>(rw),
                                  static_cast<nri::Dim_t>(rh),
                                  1u,
                                  0u,
                                  0u,
                                  nri::PlaneBits::COLOR};
    core.CmdReadbackTextureToBuffer(*cmd, *s.pick_readback.buffer, id_layout, *s.pick_id_tex.texture, id_reg);

    ATELIER_TRY(check_nri(core.EndCommandBuffer(*cmd), rhi::ErrorCode::execution_failed,
                          "Pick box EndCommandBuffer failed", true));
    auto sub = timeline.submit(rhi::QueueKind::graphics, std::span{&cmd, 1u});
    if (!sub)
        return std::unexpected(Error{sub.error(), true});
    rhi::BackendAccess::reclaimer(*s.device).retire({
        .fence = timeline.current_point(),
        .command_buffer = std::exchange(cmd, nullptr),
        .command_allocator = std::exchange(cmd_alloc, nullptr),
    });
    auto done = timeline.wait(rhi::QueueKind::graphics, *sub, s.device->wait_timeout_ms());
    if (!done)
        return std::unexpected(Error{done.error(), true});

    const auto* mapped = static_cast<const std::uint8_t*>(core.MapBuffer(*s.pick_readback.buffer, 0u, read_bytes));
    if (!mapped)
        return std::unexpected(make_error(rhi::ErrorCode::out_of_memory, "Pick box MapBuffer failed", true));

    std::vector<std::uint64_t> seen_words;
    for (std::uint32_t ry = 0u; ry < rh; ++ry) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(mapped + std::size_t(ry) * row_pitch);
        for (std::uint32_t rx = 0u; rx < rw; ++rx) {
            const std::uint32_t id_plus_1 = row[rx * 2u];
            if (id_plus_1 == 0u)
                continue;
            const std::uint32_t inst_idx = id_plus_1 - 1u;
            const std::size_t word_idx = inst_idx >> 6u;
            if (word_idx >= seen_words.size())
                seen_words.resize(word_idx + 1u, 0ull);
            const std::uint64_t bit = 1ull << (inst_idx & 63u);
            if ((seen_words[word_idx] & bit) == 0ull) {
                seen_words[word_idx] |= bit;
                hits.push_back(inst_idx);
            }
        }
    }
    core.UnmapBuffer(*s.pick_readback.buffer);
    std::ranges::sort(hits);
    return hits;
}

void Renderer::release_transients(Dormancy level) noexcept {
    if (!state_ || state_->owner != std::this_thread::get_id() || !state_->device || !*state_->device)
        return;
    auto& s = *state_;
    if (level >= Dormancy::viewport_hidden) {
        for (auto& lc : s.lit_contexts) {
            if (level >= Dormancy::deep)
                lc->retire(*s.device);
            else
                lc->retire_screen_history(*s.device);
        }
        if (level >= Dormancy::deep) {
            s.lit_contexts.clear();
            s.scene_depth.retire(*s.device);
            s.pick_id_tex.retire(*s.device);
            s.has_pick_target = false;
        }
    }
    s.executor.release_transients();
}

Result<void> Renderer::close() {
    if (!state_)
        return {};
    auto result = state_->close();
    if (result)
        state_.reset();
    else if (result.error().terminal)
        state_->failure = result.error();
    return result;
}

} // namespace atelier::renderer
