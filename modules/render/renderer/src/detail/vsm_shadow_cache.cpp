#include "vsm_shadow_cache.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace atelier::renderer::detail {
namespace {

constexpr std::array<float, vsm_cascade_count> cascade_world_spans{16.0f, 48.0f, 128.0f, 384.0f};

constexpr float ortho_half_z_span = 512.0f;

constexpr float ortho_total_z_span = 1024.0f;

constexpr std::size_t max_vsm_pairs_per_pool = 4194304u;

std::uint64_t splitmix64(std::uint64_t x) noexcept {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30u)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27u)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31u);
}

bool same_matrix_bits(const std::array<float, 16>& a, const std::array<float, 16>& b) noexcept {
    return std::memcmp(a.data(), b.data(), sizeof(a)) == 0;
}

bool same_vec3_bits(const math::Vector3& a, const math::Vector3& b) noexcept {
    return std::memcmp(&a, &b, sizeof(math::Vector3)) == 0;
}

std::array<float, 16> make_ortho_view_proj(const math::Vector3& right, const math::Vector3& up,
                                           const math::Vector3& dir, float x0, float x1, float y0, float y1,
                                           float z_near, float z_span) noexcept {
    const float sx = 2.0f / (x1 - x0);
    const float sy = 2.0f / (y1 - y0);
    const float sz = -1.0f / z_span;
    const float tx = -(x1 + x0) / (x1 - x0);
    const float ty = -(y1 + y0) / (y1 - y0);
    const float tz = (z_near + z_span) / z_span;
    return {
        right[0] * sx,
        up[0] * sy,
        dir[0] * sz,
        0.0f,
        right[1] * sx,
        up[1] * sy,
        dir[1] * sz,
        0.0f,
        right[2] * sx,
        up[2] * sy,
        dir[2] * sz,
        0.0f,
        tx,
        ty,
        tz,
        1.0f,
    };
}

std::array<float, 16> make_persp_view_proj(const math::Vector3& pos, const math::Vector3& right,
                                           const math::Vector3& up, const math::Vector3& fwd, float near_z, float far_z,
                                           float tan_half) noexcept {
    const float inv_th = 1.0f / tan_half;
    const float zn_a = -near_z / (far_z - near_z);
    const float zn_b = (near_z * far_z) / (far_z - near_z);
    const float tx = -math::dot(pos, right);
    const float ty = -math::dot(pos, up);
    const float tz = -math::dot(pos, fwd);
    return {
        right[0] * inv_th, up[0] * inv_th, fwd[0] * zn_a,    fwd[0],
        right[1] * inv_th, up[1] * inv_th, fwd[1] * zn_a,    fwd[1],
        right[2] * inv_th, up[2] * inv_th, fwd[2] * zn_a,    fwd[2],
        tx * inv_th,       ty * inv_th,    tz * zn_a + zn_b, tz,
    };
}

std::array<float, 16> make_persp_subpage_vp(const std::array<float, 16>& full_vp, std::uint32_t vx,
                                            std::uint32_t vy) noexcept {
    const float sx = (vx == 0u) ? 1.0f : -1.0f;
    const float sy = (vy == 0u) ? -1.0f : 1.0f;
    std::array<float, 16> out = full_vp;
    for (std::size_t row = 0u; row < 4u; ++row) {
        const float w_r = full_vp[row * 4u + 3u];
        out[row * 4u + 0u] = 2.0f * full_vp[row * 4u + 0u] + sx * w_r;
        out[row * 4u + 1u] = 2.0f * full_vp[row * 4u + 1u] + sy * w_r;
    }
    return out;
}

nri::Rect page_to_rect(std::uint32_t phys_page) noexcept {
    const auto px = static_cast<std::int16_t>((phys_page & 31u) * vsm_page_dim);
    const auto py = static_cast<std::int16_t>((phys_page >> 5u) * vsm_page_dim);
    return nri::Rect{px, py, static_cast<nri::Dim_t>(vsm_page_dim), static_cast<nri::Dim_t>(vsm_page_dim)};
}

void cube_face_basis(std::uint32_t face, math::Vector3& out_r, math::Vector3& out_u, math::Vector3& out_f) noexcept {
    // Orthonormal LH bases (`cross(R, U) == F`!) for 6 90-deg cubemap faces:
    switch (face) {
    case 0u: // +X
        out_f = {1.f, 0.f, 0.f};
        out_u = {0.f, 1.f, 0.f};
        out_r = {0.f, 0.f, -1.f};
        break;
    case 1u: // -X
        out_f = {-1.f, 0.f, 0.f};
        out_u = {0.f, 1.f, 0.f};
        out_r = {0.f, 0.f, 1.f};
        break;
    case 2u: // +Y
        out_f = {0.f, 1.f, 0.f};
        out_u = {0.f, 0.f, -1.f};
        out_r = {1.f, 0.f, 0.f};
        break;
    case 3u: // -Y
        out_f = {0.f, -1.f, 0.f};
        out_u = {0.f, 0.f, 1.f};
        out_r = {1.f, 0.f, 0.f};
        break;
    case 4u: // +Z
        out_f = {0.f, 0.f, 1.f};
        out_u = {0.f, 1.f, 0.f};
        out_r = {1.f, 0.f, 0.f};
        break;
    default: // -Z
        out_f = {0.f, 0.f, -1.f};
        out_u = {0.f, 1.f, 0.f};
        out_r = {-1.f, 0.f, 0.f};
        break;
    }
}

} // namespace

Result<void> VsmShadowCache::ensure_pools(rhi::Device& device) {
    if (static_pool_.texture && dynamic_pool_.texture)
        return {};

    static_pool_.retire(device);
    dynamic_pool_.retire(device);
    pools_initialized_ = false;
    invalidate_all();

    constexpr nri::TextureUsageBits vsm_usage =
        nri::TextureUsageBits::DEPTH_STENCIL_ATTACHMENT | nri::TextureUsageBits::SHADER_RESOURCE;

    ATELIER_TRY_ASSIGN(static_pool_,
                       create_rw_texture(device, nri::TextureType::TEXTURE_2D, nri::Format::D32_SFLOAT, vsm_pool_dim,
                                         vsm_pool_dim, 1u, 1u, vsm_usage, false, "vsm_static_pool_d32f"));
    auto dyn = create_rw_texture(device, nri::TextureType::TEXTURE_2D, nri::Format::D32_SFLOAT, vsm_pool_dim,
                                 vsm_pool_dim, 1u, 1u, vsm_usage, false, "vsm_dynamic_pool_d32f");
    if (!dyn) {
        static_pool_.retire(device);
        return std::unexpected(dyn.error());
    }
    dynamic_pool_ = std::move(*dyn);
    return {};
}

void VsmShadowCache::invalidate_all() noexcept {
    static_slots_ = {};
    virtual_to_static_.fill(vsm_page_unmapped);
    cached_scene_identity_ = ~0ull;
    cached_masked_hash_ = 0u;
    cached_sun_key_ = 0u;
    cached_sun_z_snaps_ = {};
    has_sun_z_snaps_ = false;
    pending_commit_ = false;
}

void VsmShadowCache::retire(rhi::Device& device) noexcept {
    static_pool_.retire(device);
    dynamic_pool_.retire(device);
    page_table_ring_.retire_all(device);
    views_ring_.retire_all(device);
    static_jobs_ring_.retire_all(device);
    static_pairs_ring_.retire_all(device);
    dynamic_jobs_ring_.retire_all(device);
    dynamic_pairs_ring_.retire_all(device);
    pools_initialized_ = false;
    invalidate_all();
}

std::uint64_t VsmShadowCache::resource_bytes() const noexcept {
    return static_pool_.byte_size + dynamic_pool_.byte_size;
}

void VsmShadowCache::commit_submitted(std::uint64_t scene_identity, std::uint64_t masked_material_hash) noexcept {
    if (!pending_commit_)
        return;
    pools_initialized_ = true;
    static_slots_ = next_static_slots_;
    virtual_to_static_ = next_virtual_to_static_;
    cached_scene_identity_ = scene_identity;
    cached_masked_hash_ = masked_material_hash;
    cached_sun_dir_ = next_sun_dir_;
    cached_sun_key_ = next_sun_key_;
    cached_sun_z_snaps_ = next_sun_z_snaps_;
    has_sun_z_snaps_ = next_has_sun_z_snaps_;
    pending_commit_ = false;
}

Result<VsmFramePlan> VsmShadowCache::prepare_frame(rhi::Device& device, std::size_t slot_index, std::uint64_t sequence,
                                                   const Frame& frame, PersistentRenderScene& scene,
                                                   std::uint64_t masked_material_hash,
                                                   std::span<GpuLightRecord> gpu_lights,
                                                   std::span<const std::uint64_t> gpu_light_keys,
                                                   std::uint32_t dir_light_count, std::uint32_t primary_sun_idx) {
    VsmFramePlan plan{};
    pending_commit_ = false;

    const bool shadows_enabled = frame.view.lit.shadows && !gpu_lights.empty() && (scene.instance_count > 0u);
    if (!shadows_enabled)
        return plan;

    bool any_shadow_light = false;
    for (const auto& gl : gpu_lights) {
        if ((gl.kind_and_flags & 8u) != 0u) {
            any_shadow_light = true;
            break;
        }
    }
    if (!any_shadow_light)
        return plan;

    ATELIER_TRY(ensure_pools(device));
    plan.active = true;
    plan.first_pool_init = !pools_initialized_;

    next_static_slots_ = static_slots_;
    next_virtual_to_static_ = virtual_to_static_;
    next_sun_dir_ = cached_sun_dir_;
    next_sun_key_ = cached_sun_key_;
    next_sun_z_snaps_ = cached_sun_z_snaps_;
    next_has_sun_z_snaps_ = has_sun_z_snaps_;

    const bool full_reset = !pools_initialized_ || (frame.scene_identity != cached_scene_identity_);
    if (full_reset) {
        next_static_slots_ = {};
        next_virtual_to_static_.fill(vsm_page_unmapped);
        next_sun_z_snaps_ = {};
        next_has_sun_z_snaps_ = false;
        plan.clear_all_static = true;
    } else if (masked_material_hash != cached_masked_hash_) {
        for (std::uint32_t s = 0u; s < vsm_static_page_capacity; ++s) {
            if (next_static_slots_[s].allocated && next_static_slots_[s].has_masked_casters)
                next_static_slots_[s].valid_baked = false;
        }
    }

    std::array<GpuVsmLightView, vsm_max_views> views{};
    std::uint32_t used_views = vsm_cascade_count;

    const math::Vector3 cam_eye = extract_camera_eye(frame.camera.view);
    const math::Vector3 cam_fwd{frame.camera.view[2], frame.camera.view[6], frame.camera.view[10]};
    const float fwd_len = std::sqrt(math::dot(cam_fwd, cam_fwd));
    const math::Vector3 unit_cam_fwd = (fwd_len > 1e-5f) ? (cam_fwd * (1.0f / fwd_len)) : math::Vector3{0.f, 0.f, 1.f};

    // 1. Configure Primary Sun 4-Cascade Orthographic Clipmap (Views 0..3):
    const bool has_sun_vsm = (primary_sun_idx < dir_light_count) && (primary_sun_idx < gpu_lights.size()) &&
                             ((gpu_lights[primary_sun_idx].kind_and_flags & 8u) != 0u);
    math::Vector3 sun_r{1.f, 0.f, 0.f};
    math::Vector3 sun_u{0.f, 1.f, 0.f};
    math::Vector3 sun_d{0.f, -1.f, 0.f};
    std::array<float, vsm_cascade_count> sun_z_snaps{};

    if (has_sun_vsm) {
        auto& sun_rec = gpu_lights[primary_sun_idx];
        sun_rec.kind_and_flags = (sun_rec.kind_and_flags & 0xFFFF00FFu) | (0u << 8u);
        sun_d = {sun_rec.direction[0], sun_rec.direction[1], sun_rec.direction[2]};
        const float dl = std::sqrt(math::dot(sun_d, sun_d));
        sun_d = (dl > 1e-6f) ? (sun_d * (1.0f / dl)) : math::Vector3{0.f, -1.f, 0.f};
        const math::Vector3 up_ref =
            (std::abs(sun_d[1]) < 0.99f) ? math::Vector3{0.f, 1.f, 0.f} : math::Vector3{0.f, 0.f, 1.f};
        sun_r = math::cross(up_ref, sun_d);
        const float rl = std::sqrt(math::dot(sun_r, sun_r));
        sun_r = (rl > 1e-6f) ? (sun_r * (1.0f / rl)) : math::Vector3{1.f, 0.f, 0.f};
        sun_u = math::cross(sun_d, sun_r);

        const std::uint64_t raw_sun_id = (primary_sun_idx < gpu_light_keys.size()) ? gpu_light_keys[primary_sun_idx]
                                                                                   : std::uint64_t(primary_sun_idx);
        const std::uint64_t sun_key = 0x53554E5F434C4950ull ^ raw_sun_id;
        const bool sun_rotated = full_reset || (sun_key != cached_sun_key_) || !same_vec3_bits(sun_d, cached_sun_dir_);
        if (sun_rotated) {
            for (std::uint32_t vp = 0u; vp < vsm_dir_virtual_pages; ++vp) {
                const std::uint32_t sp = next_virtual_to_static_[vp];
                if (sp < vsm_static_page_capacity)
                    next_static_slots_[sp].valid_baked = false;
            }
        }
        next_sun_dir_ = sun_d;
        next_sun_key_ = sun_key;

        for (std::uint32_t c = 0u; c < vsm_cascade_count; ++c) {
            const float span = cascade_world_spans[c];
            const float page_sz = span / float(vsm_cascade_grid);
            const math::Vector3 anchor = cam_eye + unit_cam_fwd * (span * 0.25f);
            const auto cx_page = static_cast<std::int32_t>(std::floor(math::dot(anchor, sun_r) / page_sz));
            const auto cy_page = static_cast<std::int32_t>(std::floor(math::dot(anchor, sun_u) / page_sz));
            const std::int32_t ix_min = cx_page - 8;
            const std::int32_t iy_min = cy_page - 8;
            const float raw_z = math::dot(anchor, sun_d);
            float z_snap = std::floor(raw_z / 32.0f) * 32.0f;
            if (has_sun_z_snaps_ && !sun_rotated && std::abs(raw_z - cached_sun_z_snaps_[c]) <= 48.0f)
                z_snap = cached_sun_z_snaps_[c];
            sun_z_snaps[c] = z_snap;
            next_sun_z_snaps_[c] = z_snap;
            const float z_near = z_snap - ortho_half_z_span;
            const float x0 = float(ix_min) * page_sz;
            const float x1 = float(ix_min + 16) * page_sz;
            const float y0 = float(iy_min) * page_sz;
            const float y1 = float(iy_min + 16) * page_sz;

            GpuVsmLightView& v = views[c];
            v.view_proj = make_ortho_view_proj(sun_r, sun_u, sun_d, x0, x1, y0, y1, z_near, ortho_total_z_span);
            v.light_pos_or_dir = {sun_d[0], sun_d[1], sun_d[2]};
            v.near_or_zmin = z_near;
            v.right_axis = {sun_r[0], sun_r[1], sun_r[2]};
            v.far_or_zspan = ortho_total_z_span;
            v.up_axis = {sun_u[0], sun_u[1], sun_u[2]};
            v.world_span_or_tan_half = span;
            v.page_offset_x = ix_min;
            v.page_offset_y = iy_min;
            v.virtual_page_base = c * 256u;
            v.flags = 1u | 2u | (vsm_cascade_grid << 8u);
        }
        next_has_sun_z_snaps_ = true;
    } else {
        next_has_sun_z_snaps_ = false;
    }

    // Mark unmapped default for all non-sun lights first:
    for (std::size_t li = 0u; li < gpu_lights.size(); ++li) {
        if (has_sun_vsm && li == primary_sun_idx)
            continue;
        gpu_lights[li].kind_and_flags = (gpu_lights[li].kind_and_flags & 0xFFFF00FFu) | (0xFFu << 8u);
    }

    // 2. Configure Local Shadow-Casting Light Views (Views 4..63, 2x2 = 4 pages/view):
    std::array<std::uint64_t, vsm_max_views> view_keys{};
    if (has_sun_vsm) {
        for (std::uint32_t c = 0u; c < vsm_cascade_count; ++c)
            view_keys[c] = next_sun_key_ ^ (std::uint64_t(c) << 32u);
    }

    for (std::size_t li = dir_light_count; li < gpu_lights.size(); ++li) {
        auto& l = gpu_lights[li];
        if ((l.kind_and_flags & 8u) == 0u)
            continue;
        const std::uint32_t kind = l.kind_and_flags & 3u;
        const std::uint32_t needed_views = (kind == 1u || kind == 3u) ? 1u : 6u;
        if (used_views + needed_views > vsm_max_views)
            continue;

        const std::uint32_t base_v = used_views;
        l.kind_and_flags = (l.kind_and_flags & 0xFFFF00FFu) | (base_v << 8u);
        used_views += needed_views;

        const math::Vector3 lpos{l.position[0], l.position[1], l.position[2]};
        const float ext = (kind == 1u) ? (0.5f * (l.width + l.height)) : l.source_radius;
        const float near_z = (kind == 1u) ? 0.03f : std::clamp(std::max(0.03f, l.source_radius * 0.85f), 0.03f, 0.25f);
        const float far_z = std::max(near_z + 0.1f, l.range + ext);
        const std::uint64_t base_key = (li < gpu_light_keys.size())
                                           ? gpu_light_keys[li]
                                           : (0x8000000000000000ull | static_cast<std::uint64_t>(li));

        if (kind == 1u) {
            math::Vector3 fwd{l.direction[0], l.direction[1], l.direction[2]};
            math::Vector3 right{l.right[0], l.right[1], l.right[2]};
            const float fl = std::sqrt(math::dot(fwd, fwd));
            fwd = (fl > 1e-6f) ? (fwd * (1.0f / fl)) : math::Vector3{0.f, 0.f, 1.f};
            const float rl = std::sqrt(math::dot(right, right));
            right = (rl > 1e-6f) ? (right * (1.0f / rl)) : math::Vector3{1.f, 0.f, 0.f};
            // Ensure exact LH orthonormal basis (`cross(R, U) == F` => `U = cross(F, R)`):
            const math::Vector3 up = math::cross(fwd, right);
            constexpr float rect_tan_half = 1.90f; // ~124.5 deg FOV
            GpuVsmLightView& v = views[base_v];
            v.view_proj = make_persp_view_proj(lpos, right, up, fwd, near_z, far_z, rect_tan_half);
            v.light_pos_or_dir = {lpos[0], lpos[1], lpos[2]};
            v.near_or_zmin = near_z;
            v.right_axis = {right[0], right[1], right[2]};
            v.far_or_zspan = far_z;
            v.up_axis = {up[0], up[1], up[2]};
            v.world_span_or_tan_half = rect_tan_half;
            v.virtual_page_base = vsm_dir_virtual_pages + (base_v - vsm_cascade_count) * 4u;
            v.flags = 1u | (vsm_local_grid << 8u);
            view_keys[base_v] = base_key ^ 0x5245435400000000ull;
        } else if (kind == 3u) {
            math::Vector3 fwd{l.direction[0], l.direction[1], l.direction[2]};
            const float fl = std::sqrt(math::dot(fwd, fwd));
            fwd = (fl > 1e-6f) ? (fwd * (1.0f / fl)) : math::Vector3{0.f, 0.f, 1.f};
            const math::Vector3 ref_up =
                (std::abs(fwd[1]) < 0.99f) ? math::Vector3{0.f, 1.f, 0.f} : math::Vector3{1.f, 0.f, 0.f};
            math::Vector3 right = math::cross(ref_up, fwd);
            const float rl = std::sqrt(math::dot(right, right));
            right = (rl > 1e-6f) ? (right * (1.0f / rl)) : math::Vector3{1.f, 0.f, 0.f};
            const math::Vector3 up = math::cross(fwd, right);
            const float cos_outer = std::clamp(l.width, 0.06f, 0.9999f);
            const float sin_outer = std::sqrt(std::max(0.0f, 1.0f - cos_outer * cos_outer));
            const float spot_tan_half = std::clamp((sin_outer / cos_outer) * 1.06f, 0.05f, 16.0f);
            GpuVsmLightView& v = views[base_v];
            v.view_proj = make_persp_view_proj(lpos, right, up, fwd, near_z, far_z, spot_tan_half);
            v.light_pos_or_dir = {lpos[0], lpos[1], lpos[2]};
            v.near_or_zmin = near_z;
            v.right_axis = {right[0], right[1], right[2]};
            v.far_or_zspan = far_z;
            v.up_axis = {up[0], up[1], up[2]};
            v.world_span_or_tan_half = spot_tan_half;
            v.virtual_page_base = vsm_dir_virtual_pages + (base_v - vsm_cascade_count) * 4u;
            v.flags = 1u | (vsm_local_grid << 8u);
            view_keys[base_v] = base_key ^ 0x53504F5400000000ull;
        } else {
            for (std::uint32_t f = 0u; f < 6u; ++f) {
                math::Vector3 r{};
                math::Vector3 u{};
                math::Vector3 fw{};
                cube_face_basis(f, r, u, fw);
                GpuVsmLightView& v = views[base_v + f];
                v.view_proj = make_persp_view_proj(lpos, r, u, fw, near_z, far_z, 1.0f);
                v.light_pos_or_dir = {lpos[0], lpos[1], lpos[2]};
                v.near_or_zmin = near_z;
                v.right_axis = {r[0], r[1], r[2]};
                v.far_or_zspan = far_z;
                v.up_axis = {u[0], u[1], u[2]};
                v.world_span_or_tan_half = 1.0f;
                v.virtual_page_base = vsm_dir_virtual_pages + (base_v + f - vsm_cascade_count) * 4u;
                v.flags = 1u | (vsm_local_grid << 8u);
                view_keys[base_v + f] = base_key ^ (std::uint64_t(f + 1u) << 48u);
            }
        }
    }

    // 3. Bin Shadow-Casting Instances into Active Virtual Pages (`O(1)` analytical page range per instance!):
    struct VirtualPageWork {
        bool active = false;

        bool ortho_pancake = false;

        std::uint64_t light_key = 0u;

        std::int32_t page_ix = 0;

        std::int32_t page_iy = 0;

        float z_snap = 0.0f;

        std::array<float, 16> page_vp{};

        std::vector<std::uint32_t> static_insts;

        std::vector<std::uint32_t> dynamic_insts;

        std::uint64_t static_sig_sum = 0u;

        std::uint64_t static_sig_xor = 0u;

        bool has_static_masked = false;

        bool has_dynamic_masked = false;
    };

    std::vector<VirtualPageWork> vpages(vsm_page_table_size);

    if (has_sun_vsm) {
        for (std::uint32_t c = 0u; c < vsm_cascade_count; ++c) {
            const GpuVsmLightView& cv = views[c];
            const float page_sz = cv.world_span_or_tan_half / float(vsm_cascade_grid);
            const float z_near = cv.near_or_zmin;
            for (std::int32_t ly = 0; ly < 16; ++ly) {
                for (std::int32_t lx = 0; lx < 16; ++lx) {
                    const std::int32_t ix = cv.page_offset_x + lx;
                    const std::int32_t iy = cv.page_offset_y + ly;
                    const std::uint32_t vx = static_cast<std::uint32_t>(ix) & 15u;
                    const std::uint32_t vy = static_cast<std::uint32_t>(iy) & 15u;
                    const std::uint32_t vp_idx = cv.virtual_page_base + vy * 16u + vx;
                    auto& w = vpages[vp_idx];
                    w.active = true;
                    w.ortho_pancake = true;
                    w.light_key = view_keys[c];
                    w.page_ix = ix;
                    w.page_iy = iy;
                    w.z_snap = sun_z_snaps[c];
                    w.page_vp =
                        make_ortho_view_proj(sun_r, sun_u, sun_d, float(ix) * page_sz, float(ix + 1) * page_sz,
                                             float(iy) * page_sz, float(iy + 1) * page_sz, z_near, ortho_total_z_span);
                }
            }
        }
    }

    for (std::uint32_t v_idx = vsm_cascade_count; v_idx < used_views; ++v_idx) {
        const GpuVsmLightView& lv = views[v_idx];
        if ((lv.flags & 1u) == 0u)
            continue;
        for (std::uint32_t vy = 0u; vy < 2u; ++vy) {
            for (std::uint32_t vx = 0u; vx < 2u; ++vx) {
                const std::uint32_t vp_idx = lv.virtual_page_base + vy * 2u + vx;
                auto& w = vpages[vp_idx];
                w.active = true;
                w.ortho_pancake = false;
                w.light_key = view_keys[v_idx];
                w.page_ix = static_cast<std::int32_t>(vx);
                w.page_iy = static_cast<std::int32_t>(vy);
                w.page_vp = make_persp_subpage_vp(lv.view_proj, vx, vy);
            }
        }
    }

    const math::Vector3 abs_sun_r{std::abs(sun_r[0]), std::abs(sun_r[1]), std::abs(sun_r[2])};
    const math::Vector3 abs_sun_u{std::abs(sun_u[0]), std::abs(sun_u[1]), std::abs(sun_u[2])};
    const math::Vector3 abs_sun_d{std::abs(sun_d[0]), std::abs(sun_d[1]), std::abs(sun_d[2])};

    for (std::uint32_t i = 0u; i < scene.instance_count; ++i) {
        const auto& meta = scene.vsm_instances[i];
        if (!meta.casts_vsm_shadow)
            continue;
        const math::Bounds& wb = meta.world_bounds;
        const math::Vector3 c_pt = (wb.minimum + wb.maximum) * 0.5f;
        const math::Vector3 h_ext = (wb.maximum - wb.minimum) * 0.5f;

        if (has_sun_vsm) {
            const float cx = math::dot(c_pt, sun_r);
            const float ex = math::dot(abs_sun_r, h_ext);
            const float cy = math::dot(c_pt, sun_u);
            const float ey = math::dot(abs_sun_u, h_ext);
            const float cz = math::dot(c_pt, sun_d);
            const float ez = math::dot(abs_sun_d, h_ext);

            for (std::uint32_t c = 0u; c < vsm_cascade_count; ++c) {
                const GpuVsmLightView& cv = views[c];
                const float z_far = cv.near_or_zmin + ortho_total_z_span;
                if (cz - ez > z_far || cz + ez < cv.near_or_zmin - 10000.0f)
                    continue;
                const float page_sz = cv.world_span_or_tan_half / float(vsm_cascade_grid);
                const float pad = 1e-4f * page_sz;
                const auto ix0 = static_cast<std::int32_t>(std::floor((cx - ex - pad) / page_sz));
                const auto ix1 = static_cast<std::int32_t>(std::floor((cx + ex + pad) / page_sz));
                const auto iy0 = static_cast<std::int32_t>(std::floor((cy - ey - pad) / page_sz));
                const auto iy1 = static_cast<std::int32_t>(std::floor((cy + ey + pad) / page_sz));
                const std::int32_t cl_x0 = std::max(ix0, cv.page_offset_x);
                const std::int32_t cl_x1 = std::min(ix1, cv.page_offset_x + 15);
                const std::int32_t cl_y0 = std::max(iy0, cv.page_offset_y);
                const std::int32_t cl_y1 = std::min(iy1, cv.page_offset_y + 15);
                for (std::int32_t iy = cl_y0; iy <= cl_y1; ++iy) {
                    const std::uint32_t vy = static_cast<std::uint32_t>(iy) & 15u;
                    for (std::int32_t ix = cl_x0; ix <= cl_x1; ++ix) {
                        const std::uint32_t vx = static_cast<std::uint32_t>(ix) & 15u;
                        auto& w = vpages[cv.virtual_page_base + vy * 16u + vx];
                        if (meta.is_dynamic_shadow) {
                            w.dynamic_insts.push_back(i);
                            w.has_dynamic_masked |= meta.has_masked_slot;
                        } else {
                            w.static_insts.push_back(i);
                            w.has_static_masked |= meta.has_masked_slot;
                            w.static_sig_sum += splitmix64(meta.static_signature);
                            w.static_sig_xor ^= splitmix64(meta.static_signature ^ 0xd6e8feb86659fd93ull);
                        }
                    }
                }
            }
        }

        for (std::uint32_t v_idx = vsm_cascade_count; v_idx < used_views; ++v_idx) {
            const GpuVsmLightView& lv = views[v_idx];
            const math::Vector3 lpos{lv.light_pos_or_dir[0], lv.light_pos_or_dir[1], lv.light_pos_or_dir[2]};
            const math::Vector3 rv{lv.right_axis[0], lv.right_axis[1], lv.right_axis[2]};
            const math::Vector3 uv{lv.up_axis[0], lv.up_axis[1], lv.up_axis[2]};
            const math::Vector3 fv = math::cross(rv, uv);
            const math::Vector3 rel = c_pt - lpos;
            const float vz_c = math::dot(rel, fv);
            const float vz_e = std::abs(fv[0]) * h_ext[0] + std::abs(fv[1]) * h_ext[1] + std::abs(fv[2]) * h_ext[2];
            if (vz_c + vz_e <= lv.near_or_zmin || vz_c - vz_e >= lv.far_or_zspan)
                continue;

            const float vx_c = math::dot(rel, rv);
            const float vx_e = std::abs(rv[0]) * h_ext[0] + std::abs(rv[1]) * h_ext[1] + std::abs(rv[2]) * h_ext[2];
            const float vy_c = math::dot(rel, uv);
            const float vy_e = std::abs(uv[0]) * h_ext[0] + std::abs(uv[1]) * h_ext[1] + std::abs(uv[2]) * h_ext[2];
            const float th = lv.world_span_or_tan_half;
            const float z_max = vz_c + vz_e;
            const float pad_xy = 1e-4f * th * std::max(z_max, 1.0f);
            if (vx_c + vx_e + pad_xy < -th * z_max || vx_c - vx_e - pad_xy > th * z_max ||
                vy_c + vy_e + pad_xy < -th * z_max || vy_c - vy_e - pad_xy > th * z_max)
                continue;

            std::uint32_t px0 = 0u;
            std::uint32_t px1 = 1u;
            std::uint32_t py0 = 0u;
            std::uint32_t py1 = 1u;
            const float z_min = vz_c - vz_e;
            if (z_min > lv.near_or_zmin) {
                const float x_lo = vx_c - vx_e;
                const float x_hi = vx_c + vx_e;
                const float y_lo = vy_c - vy_e;
                const float y_hi = vy_c + vy_e;
                const float ndc_x0 = x_lo / ((x_lo >= 0.f ? z_max : z_min) * th);
                const float ndc_x1 = x_hi / ((x_hi >= 0.f ? z_min : z_max) * th);
                const float ndc_y0 = y_lo / ((y_lo >= 0.f ? z_max : z_min) * th);
                const float ndc_y1 = y_hi / ((y_hi >= 0.f ? z_min : z_max) * th);
                if (ndc_x1 < -1.0f - 1e-4f || ndc_x0 > 1.0f + 1e-4f || ndc_y1 < -1.0f - 1e-4f || ndc_y0 > 1.0f + 1e-4f)
                    continue;
                px0 = (ndc_x0 > 1e-4f) ? 1u : 0u;
                px1 = (ndc_x1 < -1e-4f) ? 0u : 1u;
                // Note: ndc_y in [0..1] is top page vy=0, ndc_y in [-1..0] is bottom page vy=1!
                py0 = (ndc_y1 < -1e-4f) ? 1u : 0u;
                py1 = (ndc_y0 > 1e-4f) ? 0u : 1u;
            }

            for (std::uint32_t vy = py0; vy <= py1; ++vy) {
                for (std::uint32_t vx = px0; vx <= px1; ++vx) {
                    auto& w = vpages[lv.virtual_page_base + vy * 2u + vx];
                    if (meta.is_dynamic_shadow) {
                        w.dynamic_insts.push_back(i);
                        w.has_dynamic_masked |= meta.has_masked_slot;
                    } else {
                        w.static_insts.push_back(i);
                        w.has_static_masked |= meta.has_masked_slot;
                        w.static_sig_sum += splitmix64(meta.static_signature);
                        w.static_sig_xor ^= splitmix64(meta.static_signature ^ 0xd6e8feb86659fd93ull);
                    }
                }
            }
        }
    }

    // 4. Allocate/Reuse Static Base Physical Pages + Copy-on-Write Dynamic Physical Pages:
    std::array<std::uint32_t, vsm_page_table_size> gpu_page_table{};
    gpu_page_table.fill(vsm_page_unmapped);

    std::vector<GpuVsmPageJob> static_jobs;
    std::vector<GpuVsmPageInstancePair> static_pairs;
    std::vector<GpuVsmPageJob> dynamic_jobs;
    std::vector<GpuVsmPageInstancePair> dynamic_pairs;
    std::uint64_t static_opaque_mls = 0u;
    std::uint64_t static_masked_mls = 0u;
    std::uint64_t dynamic_opaque_mls = 0u;
    std::uint64_t dynamic_masked_mls = 0u;
    std::uint32_t next_dynamic_phys = 0u;

    auto alloc_static_slot = [&](std::uint32_t vp_idx) -> std::uint32_t {
        const std::uint32_t existing = next_virtual_to_static_[vp_idx];
        if (existing < vsm_static_page_capacity)
            return existing;
        for (std::uint32_t s = 0u; s < vsm_static_page_capacity; ++s) {
            if (!next_static_slots_[s].allocated) {
                next_static_slots_[s].allocated = true;
                next_static_slots_[s].virtual_page = vp_idx;
                next_virtual_to_static_[vp_idx] = s;
                return s;
            }
        }
        // Evict oldest LRU slot not touched in this frame (`last_used_seq < sequence`):
        std::uint32_t victim = vsm_page_unmapped;
        std::uint64_t oldest = ~0ull;
        for (std::uint32_t s = 0u; s < vsm_static_page_capacity; ++s) {
            if (next_static_slots_[s].last_used_seq < sequence && next_static_slots_[s].last_used_seq < oldest) {
                oldest = next_static_slots_[s].last_used_seq;
                victim = s;
            }
        }
        if (victim < vsm_static_page_capacity) {
            ++plan.evictions;
            const std::uint32_t old_vp = next_static_slots_[victim].virtual_page;
            if (old_vp < vsm_page_table_size && next_virtual_to_static_[old_vp] == victim)
                next_virtual_to_static_[old_vp] = vsm_page_unmapped;
            next_static_slots_[victim] = StaticSlot{};
            next_static_slots_[victim].allocated = true;
            next_static_slots_[victim].virtual_page = vp_idx;
            next_virtual_to_static_[vp_idx] = victim;
            return victim;
        }
        return vsm_page_unmapped;
    };

    for (std::uint32_t vp = 0u; vp < vsm_page_table_size; ++vp) {
        const auto& w = vpages[vp];
        if (!w.active)
            continue;

        const bool has_static = !w.static_insts.empty();
        const bool has_dynamic = !w.dynamic_insts.empty();

        if (!has_static && !has_dynamic) {
            // Release any previously held static physical page if this virtual page scrolled onto empty air!
            const std::uint32_t old_sp = next_virtual_to_static_[vp];
            if (old_sp < vsm_static_page_capacity) {
                next_static_slots_[old_sp] = StaticSlot{};
                next_virtual_to_static_[vp] = vsm_page_unmapped;
            }
            gpu_page_table[vp] = vsm_page_empty_lit;
            continue;
        }

        std::uint32_t static_phys = vsm_page_unmapped;
        if (has_static) {
            static_phys = alloc_static_slot(vp);
            if (static_phys < vsm_static_page_capacity) {
                auto& sl = next_static_slots_[static_phys];
                sl.last_used_seq = sequence;

                const bool cache_hit = sl.valid_baked && (sl.light_key == w.light_key) && (sl.page_ix == w.page_ix) &&
                                       (sl.page_iy == w.page_iy) && (sl.static_sig_sum == w.static_sig_sum) &&
                                       (sl.static_sig_xor == w.static_sig_xor) &&
                                       (!w.ortho_pancake || sl.z_snap == w.z_snap) &&
                                       same_matrix_bits(sl.page_vp_bits, w.page_vp);

                if (cache_hit) {
                    ++plan.resident_atlas_cells;
                    ++plan.reused_faces;
                } else if (static_pairs.size() + w.static_insts.size() <= max_vsm_pairs_per_pool) {
                    ++plan.resident_atlas_cells;
                    const auto job_idx = static_cast<std::uint32_t>(static_jobs.size());
                    GpuVsmPageJob job{};
                    job.page_view_proj = w.page_vp;
                    job.phys_page = static_phys;
                    job.flags = w.ortho_pancake ? vsm_job_ortho_pancake : 0u;
                    static_jobs.push_back(job);
                    if (!plan.clear_all_static)
                        plan.static_clear_rects.push_back(page_to_rect(static_phys));
                    for (std::uint32_t inst_id : w.static_insts) {
                        static_pairs.push_back({job_idx, inst_id});
                        const auto& im = scene.vsm_instances[inst_id];
                        if (im.has_masked_slot)
                            static_masked_mls += im.lod0_meshlets;
                        else
                            static_opaque_mls += im.lod0_meshlets;
                    }
                    plan.static_has_masked |= w.has_static_masked;
                    ++plan.static_updates;

                    sl.valid_baked = true;
                    sl.has_masked_casters = w.has_static_masked;
                    sl.light_key = w.light_key;
                    sl.page_ix = w.page_ix;
                    sl.page_iy = w.page_iy;
                    sl.z_snap = w.z_snap;
                    sl.page_vp_bits = w.page_vp;
                    sl.static_sig_sum = w.static_sig_sum;
                    sl.static_sig_xor = w.static_sig_xor;
                } else {
                    sl = StaticSlot{};
                    next_virtual_to_static_[vp] = vsm_page_unmapped;
                    static_phys = vsm_page_unmapped;
                }
            }
        } else {
            const std::uint32_t old_sp = next_virtual_to_static_[vp];
            if (old_sp < vsm_static_page_capacity) {
                next_static_slots_[old_sp] = StaticSlot{};
                next_virtual_to_static_[vp] = vsm_page_unmapped;
            }
        }

        if (has_dynamic) {
            if (next_dynamic_phys < vsm_dynamic_page_capacity &&
                (!has_static || static_phys < vsm_static_page_capacity) &&
                (dynamic_pairs.size() + w.dynamic_insts.size() <= max_vsm_pairs_per_pool)) {
                const std::uint32_t dyn_phys = next_dynamic_phys++;
                ++plan.resident_atlas_cells;
                if (static_phys < vsm_static_page_capacity) {
                    plan.cow_copies.push_back({static_phys, dyn_phys});
                } else {
                    plan.dynamic_clear_rects.push_back(page_to_rect(dyn_phys));
                }
                const auto job_idx = static_cast<std::uint32_t>(dynamic_jobs.size());
                GpuVsmPageJob job{};
                job.page_view_proj = w.page_vp;
                job.phys_page = dyn_phys;
                job.flags = w.ortho_pancake ? vsm_job_ortho_pancake : 0u;
                dynamic_jobs.push_back(job);
                for (std::uint32_t inst_id : w.dynamic_insts) {
                    dynamic_pairs.push_back({job_idx, inst_id});
                    const auto& im = scene.vsm_instances[inst_id];
                    if (im.has_masked_slot)
                        dynamic_masked_mls += im.lod0_meshlets;
                    else
                        dynamic_opaque_mls += im.lod0_meshlets;
                }
                plan.dynamic_has_masked |= w.has_dynamic_masked;
                ++plan.dynamic_updates;
                gpu_page_table[vp] = vsm_static_page_capacity + dyn_phys;
            } else {
                gpu_page_table[vp] = vsm_page_unmapped; // Fallback cleanly to PenumbraQueue RayQuery!
            }
        } else {
            gpu_page_table[vp] = static_phys;
        }

        if (gpu_page_table[vp] < vsm_static_page_capacity + vsm_dynamic_page_capacity)
            ++plan.mapped_faces;
    }

    ATELIER_TRY_BIND(pt_w, page_table_ring_.write(device, slot_index, gpu_page_table));
    plan.page_table_srv = pt_w->second;

    ATELIER_TRY_BIND(vw_w,
                     views_ring_.write(device, slot_index, std::span<const GpuVsmLightView>{views.data(), used_views}));
    plan.views_srv = vw_w->second;

    if (!static_jobs.empty() && !static_pairs.empty()) {
        ATELIER_TRY_BIND(sj_w, static_jobs_ring_.write(device, slot_index, static_jobs));
        ATELIER_TRY_BIND(sp_w, static_pairs_ring_.write(device, slot_index, static_pairs));
        plan.static_jobs_srv = sj_w->second;
        plan.static_pairs_srv = sp_w->second;
        plan.static_pair_count = static_cast<std::uint32_t>(static_pairs.size());
        plan.static_max_opaque_meshlets = static_cast<std::uint32_t>(
            std::clamp<std::uint64_t>(static_opaque_mls + static_masked_mls, 64u, 0x3FFFFFu));
        plan.static_max_masked_meshlets =
            static_cast<std::uint32_t>(std::clamp<std::uint64_t>(static_masked_mls, 64u, 0x1FFFFFu));
    }

    if (!dynamic_jobs.empty() && !dynamic_pairs.empty()) {
        ATELIER_TRY_BIND(dj_w, dynamic_jobs_ring_.write(device, slot_index, dynamic_jobs));
        ATELIER_TRY_BIND(dp_w, dynamic_pairs_ring_.write(device, slot_index, dynamic_pairs));
        plan.dynamic_jobs_srv = dj_w->second;
        plan.dynamic_pairs_srv = dp_w->second;
        plan.dynamic_pair_count = static_cast<std::uint32_t>(dynamic_pairs.size());
        plan.dynamic_max_opaque_meshlets = static_cast<std::uint32_t>(
            std::clamp<std::uint64_t>(dynamic_opaque_mls + dynamic_masked_mls, 64u, 0x3FFFFFu));
        plan.dynamic_max_masked_meshlets =
            static_cast<std::uint32_t>(std::clamp<std::uint64_t>(dynamic_masked_mls, 64u, 0x1FFFFFu));
    }

    pending_commit_ = true;
    return plan;
}

} // namespace atelier::renderer::detail
