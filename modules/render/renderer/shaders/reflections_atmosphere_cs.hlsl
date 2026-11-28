// 3-Tier Coat-Aware Reflections + 3-State Physical Sky / 160x90x64 Froxel Fog + SceneCompose CS (Step 5 Spec)
//   Mode 0: Reflections Trace + Capsaicin GI-1.2 Dual-Source Virtual-Hit Parallax & Surface-Motion Temporal Accumulation + Luma-Karis Moments
//           - Tier 1 (r <= 0.12) & Tier 2 (0.12 < r < 0.58): Hierarchical Hi-Z -> LDS Compacted DXR RayQuery (SHaRC + Sky)
//           - Tier 3 (r >= 0.58): GT-VBAO Bent Cone + Converged GI Irradiance / Prefiltered Sky
//   Mode 1: Glossy 3-Step Variance-Guided & Contact-Hardening Luma-Karis Ratio A-Trous Filter (`Slab0_SpecNormal` + `refl_moments` + Depth)
//   Mode 3: 160x90x64 Froxel Volumetric Fog Scattering & Temporal Blend (`0.1m .. 120m`)
//   Mode 4: 160x90x64 Froxel Analytical Front-to-Back Integration
//   Mode 2: SceneCompose CS -> SceneColorHDR + FP16-Safe Specular-Free `PrevDiffuseEmissiveRadiance` + `UnjitDepthHistory`

cbuffer ComposeRootConstants : register(b0, space0) {
    uint g_frame_cb_srv;
    uint g_comp_mode;
    uint g_depth_srv;
    uint g_slab0_srv;
    uint g_slab1_srv;
    uint g_slab2_srv;
    uint g_tile_emissive_srv;
    uint g_direct_diff_srv;
    uint g_direct_spec_srv;
    uint g_gi_srv;
    uint g_refl_srv;
    uint g_motion_srv;
    uint g_prev_hdr_or_refl_srv;
    uint g_uav0;
    uint g_uav1;
    uint g_atrous_step;
};

float3 comp_reconstruct_world(float2 pxc, float dz, row_major float4x4 inv_vp, GlobalFrameConstants fcb) {
    float2 ndc = float2((pxc.x - fcb.scene_viewport.x) / max(fcb.scene_viewport.z, 1.0f) * 2.0f - 1.0f,
                        1.0f - (pxc.y - fcb.scene_viewport.y) / max(fcb.scene_viewport.w, 1.0f) * 2.0f);
    precise float4 wp = mul(float4(ndc, dz, 1.0f), inv_vp);
    float ww = abs(wp.w) > 1e-7f ? wp.w : (wp.w >= 0.0f ? 1e-7f : -1e-7f);
    return wp.xyz / ww;
}

float3 eval_compose_iridescence(float cos_theta1, float d_nm, float eta_2, float3 base_f0) {
    float sin2_t1 = max(0.0f, 1.0f - cos_theta1 * cos_theta1);
    float sin2_t2 = sin2_t1 / max(eta_2 * eta_2, 1.0f);
    if (sin2_t2 >= 1.0f)
        return 1.0f;
    float cos_t2 = sqrt(1.0f - sin2_t2);
    float opd = 2.0f * eta_2 * d_nm * cos_t2;
    float3 inv_lambda = float3(1.0f / 650.0f, 1.0f / 550.0f, 1.0f / 450.0f);
    float3 phase = 2.0f * ATELIER_PI * opd * inv_lambda + ATELIER_PI;
    float r12 = (1.0f - eta_2) / (1.0f + eta_2);
    r12 = r12 * r12;
    float f12 = r12 + (1.0f - r12) * pow(1.0f - cos_theta1, 5.0f);
    float3 f23 = base_f0 + (1.0f - base_f0) * pow(1.0f - cos_t2, 5.0f);
    float3 num = f12 + f23 + 2.0f * sqrt(f12 * f23) * cos(phase);
    float3 den = 1.0f + f12 * f23 + 2.0f * sqrt(f12 * f23) * cos(phase);
    return saturate(num / max(den, 1e-4f));
}

float3 sample_bounded_vndf(float3 V_local, float alpha, float2 u) {
    float3 vh = normalize(float3(alpha * V_local.x, alpha * V_local.y, V_local.z));
    float phi = 2.0f * ATELIER_PI * u.x;
    float z = mad(1.0f - u.y, 1.0f + vh.z, -vh.z);
    float sin_t = sqrt(saturate(1.0f - z * z));
    float3 c = float3(sin_t * cos(phi), sin_t * sin(phi), z);
    float3 h = c + vh;
    return normalize(float3(alpha * h.x, alpha * h.y, max(0.0f, h.z)));
}

float froxel_slice_to_vz(float s) {
    return 0.1f * pow(1200.0f, saturate(s * (1.0f / 64.0f)));
}

float froxel_vz_to_slice(float vz) {
    return saturate(log2(clamp(vz, 0.1f, 120.0f) * 10.0f) * (1.0f / 10.2288187f));
}

groupshared uint gs_refl_dir[64];
groupshared uint gs_refl_lane[64];
groupshared float3 gs_refl_orig[64];
groupshared float3 gs_refl_rad[64];
groupshared float gs_refl_dist[64];
groupshared uint gs_refl_comp_mask[2];
groupshared uint gs_refl_work_cnt;
groupshared row_major float4x4 gs_refl_inv_prev_vp;

row_major float4x4 refl_invert_4x4(row_major float4x4 m, row_major float4x4 fallback_inv) {
    float s0 = m[0][0] * m[1][1] - m[1][0] * m[0][1];
    float s1 = m[0][0] * m[1][2] - m[1][0] * m[0][2];
    float s2 = m[0][0] * m[1][3] - m[1][0] * m[0][3];
    float s3 = m[0][1] * m[1][2] - m[1][1] * m[0][2];
    float s4 = m[0][1] * m[1][3] - m[1][1] * m[0][3];
    float s5 = m[0][2] * m[1][3] - m[1][2] * m[0][3];
    float c5 = m[2][2] * m[3][3] - m[3][2] * m[2][3];
    float c4 = m[2][1] * m[3][3] - m[3][1] * m[2][3];
    float c3 = m[2][1] * m[3][2] - m[3][1] * m[2][2];
    float c2 = m[2][0] * m[3][3] - m[3][0] * m[2][3];
    float c1 = m[2][0] * m[3][2] - m[3][0] * m[2][2];
    float c0 = m[2][0] * m[3][1] - m[3][0] * m[2][1];
    float det = s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
    if (abs(det) <= 1e-20f)
        return fallback_inv;
    float inv_det = 1.0f / det;
    row_major float4x4 inv;
    inv[0][0] = ( m[1][1] * c5 - m[1][2] * c4 + m[1][3] * c3) * inv_det;
    inv[0][1] = (-m[0][1] * c5 + m[0][2] * c4 - m[0][3] * c3) * inv_det;
    inv[0][2] = ( m[3][1] * s5 - m[3][2] * s4 + m[3][3] * s3) * inv_det;
    inv[0][3] = (-m[2][1] * s5 + m[2][2] * s4 - m[2][3] * s3) * inv_det;
    inv[1][0] = (-m[1][0] * c5 + m[1][2] * c2 - m[1][3] * c1) * inv_det;
    inv[1][1] = ( m[0][0] * c5 - m[0][2] * c2 + m[0][3] * c1) * inv_det;
    inv[1][2] = (-m[3][0] * s5 + m[3][2] * s2 - m[3][3] * s1) * inv_det;
    inv[1][3] = ( m[2][0] * s5 - m[2][2] * s2 + m[2][3] * s1) * inv_det;
    inv[2][0] = ( m[1][0] * c4 - m[1][1] * c2 + m[1][3] * c0) * inv_det;
    inv[2][1] = (-m[0][0] * c4 + m[0][1] * c2 - m[0][3] * c0) * inv_det;
    inv[2][2] = ( m[3][0] * s4 - m[3][1] * s2 + m[3][3] * s0) * inv_det;
    inv[2][3] = (-m[2][0] * s4 + m[2][1] * s2 - m[2][3] * s0) * inv_det;
    inv[3][0] = (-m[1][0] * c3 + m[1][1] * c1 - m[1][2] * c0) * inv_det;
    inv[3][1] = ( m[0][0] * c3 - m[0][1] * c1 + m[0][2] * c0) * inv_det;
    inv[3][2] = (-m[3][0] * s3 + m[3][1] * s1 - m[3][2] * s0) * inv_det;
    inv[3][3] = ( m[2][0] * s3 - m[2][1] * s1 + m[2][2] * s0) * inv_det;
    return inv;
}

float3 refl_reconstruct_prev_world(float2 prev_pxc, float prev_dz, row_major float4x4 inv_prev_vp, GlobalFrameConstants fcb) {
    float2 ndc = float2((prev_pxc.x - fcb.prev_scene_viewport.x) / max(fcb.prev_scene_viewport.z, 1.0f) * 2.0f - 1.0f,
                        1.0f - (prev_pxc.y - fcb.prev_scene_viewport.y) / max(fcb.prev_scene_viewport.w, 1.0f) * 2.0f);
    precise float4 wp = mul(float4(ndc, prev_dz, 1.0f), inv_prev_vp);
    float ww = abs(wp.w) > 1e-7f ? wp.w : (wp.w >= 0.0f ? 1e-7f : -1e-7f);
    return wp.xyz / ww;
}

float3 refl_eval_filter_normal(uint2 qpx, Texture2D<uint2> slab0_tex) {
    uint2 s0 = slab0_tex.Load(int3(qpx, 0));
    uint prof = (s0.y >> 26u) & 7u;
    if (prof == SLAB_PROFILE_COAT && g_gi_srv != 0xFFFFFFFFu && g_slab2_srv != 0xFFFFFFFFu) {
        Texture2D<uint2> slab1_tex = ResourceDescriptorHeap[atelier_safe_desc(g_gi_srv)];
        OpenPbrSurface surf = unpack_openpbr_slab(s0, slab1_tex.Load(int3(qpx, 0)));
        if (surf.lobe_weight > 0.0f) {
            Texture2D<uint> gn_tex = ResourceDescriptorHeap[atelier_safe_desc(g_slab2_srv)];
            float3 gn = unpack_oct_snorm16(gn_tex.Load(int3(qpx, 0)));
            return eval_coat_normal(surf.shading_normal, gn, surf.lobe_weight);
        }
    }
    return unpack_oct_snorm16(s0.x);
}

uint refl_find_closest_probe_seed(uint2 base_probe, int2 offset, Texture2D<uint> mask_tex, uint2 probes_dim,
                                  uint mip_count) {
    int2 pos = int2(base_probe) + offset;
    if (pos.x < 0 || pos.y < 0 || pos.x >= int(probes_dim.x) || pos.y >= int(probes_dim.y))
        return 0xFFFFFFFFu;
    uint seed0 = mask_tex.Load(int3(pos, 0));
    if (seed0 != 0xFFFFFFFFu)
        return seed0;
    [loop] for (uint mip = 1u; mip < mip_count; ++mip) {
        uint2 mdim = max(uint2(1u, 1u), probes_dim >> mip);
        int2 mpos = clamp(pos >> int(mip), int2(0, 0), int2(mdim) - 1);
        uint mseed = mask_tex.Load(int3(mpos, int(mip)));
        if (mseed != 0xFFFFFFFFu)
            return mseed;
    }
    return 0xFFFFFFFFu;
}

float4 sample_probe_oct_bilerp(Texture2D<float4> atlas_tex, uint2 pc, float2 oct_uv, float spread_cells) {
    float2 cf = oct_uv * 8.0f - 0.5f;
    int2 c0 = int2(floor(cf));
    float2 f = saturate(cf - float2(c0));
    int2 p00 = clamp(c0, int2(0, 0), int2(7, 7));
    int2 p10 = clamp(c0 + int2(1, 0), int2(0, 0), int2(7, 7));
    int2 p01 = clamp(c0 + int2(0, 1), int2(0, 0), int2(7, 7));
    int2 p11 = clamp(c0 + int2(1, 1), int2(0, 0), int2(7, 7));
    int2 tile = int2(pc * 8u);
    float4 v00 = atlas_tex.Load(int3(tile + p00, 0));
    float4 v10 = atlas_tex.Load(int3(tile + p10, 0));
    float4 v01 = atlas_tex.Load(int3(tile + p01, 0));
    float4 v11 = atlas_tex.Load(int3(tile + p11, 0));
    float4 center = lerp(lerp(v00, v10, f.x), lerp(v01, v11, f.x), f.y);
    if (spread_cells > 1e-3f) {
        int2 cr = int2(round(cf));
        float3 cross_sum = atlas_tex.Load(int3(tile + clamp(cr + int2(-1, 0), int2(0, 0), int2(7, 7)), 0)).rgb +
                           atlas_tex.Load(int3(tile + clamp(cr + int2(1, 0), int2(0, 0), int2(7, 7)), 0)).rgb +
                           atlas_tex.Load(int3(tile + clamp(cr + int2(0, -1), int2(0, 0), int2(7, 7)), 0)).rgb +
                           atlas_tex.Load(int3(tile + clamp(cr + int2(0, 1), int2(0, 0), int2(7, 7)), 0)).rgb;
        center.rgb = lerp(center.rgb, cross_sum * 0.25f, spread_cells);
    }
    return center;
}

// Lumen-style Screen-Probes directional rough specular (`8x8` octahedral radiance atlas along GGX dominant lobe):
bool sample_screen_probes_rough_specular(uint2 px, float3 P, float3 GN, float3 N_refl, float3 R, float3 bent_w,
                                         float bent_vis, float r_trace, Texture2D<float> depth_tex,
                                         GlobalFrameConstants fcb, out float3 out_rad, out float out_dist) {
    out_rad = 0.0f;
    out_dist = 4.0f;
    if ((fcb.flags & 2u) == 0u || fcb.gi_probe_atlas_srv == 0xFFFFFFFFu || fcb.gi_probe_mask_srv == 0xFFFFFFFFu ||
        g_slab2_srv == 0xFFFFFFFFu)
        return false;

    float alpha_r = r_trace * r_trace;
    float dom_f = (1.0f - alpha_r) * (sqrt(max(1.0f - alpha_r, 0.0f)) + alpha_r);
    float3 R_dom = safe_unit_normal(lerp(N_refl, R, dom_f));
    float3 R_eval = safe_unit_normal(R_dom + (bent_w - GN) * ((1.0f - bent_vis) * 0.35f * r_trace));
    float spread = saturate((r_trace - 0.65f) * 2.2f) * 0.45f;

    float px_ws = gi_pixel_world_size(P, fcb);
    float cell_ws = 8.0f * px_ws;
    Texture2D<uint> gn_tex = ResourceDescriptorHeap[atelier_safe_desc(g_slab2_srv)];
    Texture2D<float4> atlas_tex = ResourceDescriptorHeap[atelier_safe_desc(fcb.gi_probe_atlas_srv)];
    Texture2D<uint> mask_tex = ResourceDescriptorHeap[atelier_safe_desc(fcb.gi_probe_mask_srv)];

    uint pm_w = 1u, pm_h = 1u, pm_mips = 1u;
    mask_tex.GetDimensions(0u, pm_w, pm_h, pm_mips);
    uint2 probes_dim = uint2((fcb.render_extent.x + 7u) / 8u, (fcb.render_extent.y + 7u) / 8u);

    uint t_idx = ((fcb.flags & (1u << 7u)) != 0u) ? fcb.frame_index : 0u;
    float2 u_jit = atelier_blue_jitter2(px, t_idx, 0x9E3779B9u) * 2.0f - 1.0f;
    float2 sample_px = float2(px);
    int2 j_px = clamp(int2(round(float2(px) + u_jit * 4.0f)), fcb.scissor_rect.xy,
                      max(fcb.scissor_rect.xy, fcb.scissor_rect.zw - 1));
    float2 j_pxc = float2(j_px) + 0.5f;
    if (j_pxc.x >= fcb.scene_viewport.x && j_pxc.x < fcb.scene_viewport.x + fcb.scene_viewport.z &&
        j_pxc.y >= fcb.scene_viewport.y && j_pxc.y < fcb.scene_viewport.y + fcb.scene_viewport.w) {
        float j_dz = depth_tex.Load(int3(j_px, 0));
        if (j_dz > 0.0f) {
            float3 j_GN = unpack_oct_snorm16(gn_tex.Load(int3(j_px, 0)));
            float3 j_P = comp_reconstruct_world(j_pxc, j_dz, fcb.inv_view_projection, fcb);
            if (dot(j_GN, GN) >= 0.90f && abs(dot(j_P - P, GN)) <= max(0.02f, px_ws * 2.5f))
                sample_px = float2(px) + u_jit * 4.0f;
        }
    }

    float2 probe_f = clamp((sample_px - 3.5f) * 0.125f, 0.0f, float2(max(probes_dim, 1u) - 1u));
    uint2 base_p = min(uint2(floor(probe_f)), max(probes_dim, 1u) - 1u);
    float2 frac_p = saturate(probe_f - float2(base_p));

    float3 rad_sum = 0.0f;
    float dist_sum = 0.0f;
    float w_total = 0.0f;

    [loop] for (int oy = 0; oy <= 1; ++oy) {
        [loop] for (int ox = 0; ox <= 1; ++ox) {
            uint seed = refl_find_closest_probe_seed(base_p, int2(ox, oy), mask_tex, probes_dim, pm_mips);
            if (seed == 0xFFFFFFFFu)
                continue;
            uint2 seed_px = uint2(seed & 0xFFFFu, seed >> 16u);
            uint2 pc = min(seed_px >> 3u, probes_dim - 1u);
            float adz = depth_tex.Load(int3(seed_px, 0));
            if (adz <= 0.0f)
                continue;

            float3 qN = unpack_oct_snorm16(gn_tex.Load(int3(seed_px, 0)));
            float3 Q = comp_reconstruct_world(float2(seed_px) + 0.5f, adz, fcb.inv_view_projection, fcb);

            float bw = (ox == 0 ? (1.0f - frac_p.x) : frac_p.x) * (oy == 0 ? (1.0f - frac_p.y) : frac_p.y);
            bool direct_cell = all(pc == min(base_p + uint2(ox, oy), probes_dim - 1u));
            float base_w = direct_cell ? max(bw, 0.02f) : max(bw * 0.25f, 0.01f);
            float plane_d = max(abs(dot(Q - P, GN)), abs(dot(P - Q, qN))) / max(0.03f, cell_ws * 0.55f);
            float radial_d = length(Q - P) / max(0.12f, cell_ws * 2.5f);
            float normal_w = pow(saturate(dot(GN, qN) * 0.5f + 0.5f), 4.0f);
            float horizon_w = saturate(dot(R_eval, qN) * 2.5f + 0.35f);
            float geom_w = base_w * normal_w * horizon_w * exp(-plane_d * 2.5f - radial_d * 0.5f);
            if (geom_w <= 1e-5f)
                continue;

            float3 nl0 = atelier_world_to_local(R_eval, qN);
            nl0 = safe_unit_normal(float3(nl0.xy, max(nl0.z, 0.04f)));
            uint2 c_init = min(uint2(hemi_oct_encode(nl0) * 8.0f), 7u);
            float init_d = atlas_tex.Load(int3(pc * 8u + c_init, 0)).w;
            float3 dir_p = R_eval;
            if (init_d > 0.05f && init_d < 490.0f) {
                float3 P_hit = P + R_eval * clamp(init_d, 0.15f, 50.0f);
                dir_p = safe_unit_normal(lerp(R_eval, safe_unit_normal(P_hit - Q), 0.65f));
            }
            float3 nl_p = atelier_world_to_local(dir_p, qN);
            nl_p = safe_unit_normal(float3(nl_p.xy, max(nl_p.z, 0.04f)));
            float4 cell_val = sample_probe_oct_bilerp(atlas_tex, pc, hemi_oct_encode(nl_p), spread);

            rad_sum += max(cell_val.rgb, 0.0f) * geom_w;
            dist_sum += clamp(cell_val.w, 0.1f, 50.0f) * geom_w;
            w_total += geom_w;
        }
    }

    if (w_total <= 1e-5f)
        return false;
    out_rad = rad_sum / w_total;
    out_dist = dist_sum / w_total;
    return true;
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID, uint gindex : SV_GroupIndex, uint3 gid : SV_GroupID) {
    StructuredBuffer<GlobalFrameConstants> fcb_buf = ResourceDescriptorHeap[atelier_safe_desc(g_frame_cb_srv)];
    GlobalFrameConstants fcb = fcb_buf[0];
    uint comp_mode = g_comp_mode & 7u;
    uint prev_gn_srv = (g_comp_mode >= 16u) ? ((g_comp_mode >> 4u) - 1u) : 0xFFFFFFFFu;

    // ========================================================================
    // Mode 3: 160x90x64 Froxel Volumetric Fog Scattering & Temporal Blend
    // ========================================================================
    if (comp_mode == 3u) {
        uint3 fc = uint3(dtid.xy, gid.z);
        if (fc.x >= 160u || fc.y >= 90u || fc.z >= 64u)
            return;

        RWTexture3D<float4> froxel_curr = ResourceDescriptorHeap[atelier_safe_desc(g_uav0)];
        float z0 = froxel_slice_to_vz(float(fc.z));
        float z1 = froxel_slice_to_vz(float(fc.z) + 1.0f);
        uint temporal_phase = ((fcb.flags & (1u << 7u)) != 0u) ? (fcb.frame_index & 31u) : 0u;
        float jitter = frac(52.9829189f * frac(dot(float2(fc.xy), float2(0.06711056f, 0.00583715f)) + float(fc.z) * 0.193f + float(temporal_phase) * 0.381966f));
        float vz = lerp(z0, z1, jitter);

        float2 uv = (float2(fc.xy) + 0.5f) / float2(160.0f, 90.0f);
        float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
        bool is_ortho = (abs(fcb.projection[2][3]) <= 1e-6f);
        float2 frustum_xy = float2(ndc.x / max(abs(fcb.projection[0][0]), 1e-4f),
                                   ndc.y / max(abs(fcb.projection[1][1]), 1e-4f));
        float3 ray_vs = is_ortho ? float3(frustum_xy, vz) : float3(frustum_xy * vz, vz);
        float3 P = mul(float4(ray_vs, 1.0f), fcb.inv_view).xyz;
        float3 V_dir = is_ortho ? safe_unit_normal(mul(float3(0.0f, 0.0f, 1.0f), (float3x3)fcb.inv_view))
                                : safe_unit_normal(P - fcb.camera_eye);

        float sigma_t = max(fcb.fog_density, 0.0f) * exp(-max(0.0f, P.y) * max(fcb.fog_height_falloff, 0.0f));
        float3 in_scat = 0.0f;
        if (sigma_t > 1e-6f) {
            const float g = clamp(fcb.fog_anisotropy, -0.9f, 0.9f);
            float g2 = g * g;
            bool shadows_on = (fcb.flags & 1u) != 0u;

            if (fcb.light_count > 0u && fcb.lights_srv != 0xFFFFFFFFu) {
                StructuredBuffer<GpuLightRecord> lights = ResourceDescriptorHeap[atelier_safe_desc(fcb.lights_srv)];
                [loop] for (uint di = 0u; di < fcb.dir_light_count; ++di) {
                    GpuLightRecord l = lights[di];
                    float3 L = safe_unit_normal(-l.direction);
                    float vis = 1.0f;
                    if (shadows_on && (l.kind_and_flags & 8u) != 0u) {
                        float vsm_vis = sample_vsm_sun_visibility(l, P, 0.02f, fcb);
                        if (vsm_vis >= 0.0f) {
                            vis = vsm_vis;
                        } else if (fcb.tlas_srv != 0xFFFFFFFFu) {
                            RaytracingAccelerationStructure tlas = ResourceDescriptorHeap[atelier_safe_desc(fcb.tlas_srv)];
                            RayDesc sray = {P, 1e-2f, L, 1e4f};
                            RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> sq;
                            sq.TraceRayInline(tlas, RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFFu, sray);
                            sq.Proceed();
                            vis = (sq.CommittedStatus() == COMMITTED_NOTHING) ? 1.0f : 0.0f;
                        }
                    }
                    if (vis > 0.0f) {
                        float cos_t = dot(V_dir, -L);
                        float phase = (1.0f - g2) / (4.0f * ATELIER_PI * pow(max(1.0f + g2 - 2.0f * g * cos_t, 1e-3f), 1.5f));
                        in_scat += l.color_intensity * (phase * vis);
                    }
                }

                uint local_end = min(fcb.light_count, fcb.dir_light_count + 64u);
                [loop] for (uint li = fcb.dir_light_count; li < local_end; ++li) {
                    GpuLightRecord l = lights[li];
                    float3 to_l = l.position - P;
                    float d2 = dot(to_l, to_l);
                    if (d2 >= l.range * l.range)
                        continue;
                    float d = sqrt(max(d2, 0.04f));
                    float3 L_dir = to_l / d;
                    uint kind = l.kind_and_flags & 3u;
                    float emit_scale = 1.0f;
                    if (kind == 1u) {
                        float cos_emit = dot(-L_dir, l.direction);
                        if (cos_emit <= 0.0f && (l.kind_and_flags & 4u) == 0u)
                            continue;
                        emit_scale = max(abs(cos_emit), 0.0f) * (l.width * l.height);
                    } else if (kind == 3u) {
                        float spot_t = saturate((dot(-L_dir, l.direction) - l.width) * l.height);
                        if (spot_t <= 0.0f)
                            continue;
                        emit_scale = spot_t * spot_t;
                    }
                    float wt = saturate(1.0f - pow(d / max(l.range, 1e-3f), 4.0f));
                    float cos_t = dot(V_dir, -L_dir);
                    float phase = (1.0f - g2) / (4.0f * ATELIER_PI * pow(max(1.0f + g2 - 2.0f * g * cos_t, 1e-3f), 1.5f));
                    in_scat += l.color_intensity * (emit_scale * wt * wt / max(d2, 0.04f)) * phase;
                }
            }

            in_scat *= fcb.exposure_scale;
            float3 sky_amb = eval_atelier_sky_diffuse_radiance(float3(0.0f, 1.0f, 0.0f), fcb);
            in_scat += sky_amb * (0.25f * ATELIER_INV_PI);
            in_scat *= sigma_t * 0.90f; // scattering albedo = 0.90
        }

        float4 curr_val = float4(clamp(in_scat, 0.0f, 65504.0f), sigma_t);
        if ((fcb.flags & (1u << 7u)) != 0u && g_prev_hdr_or_refl_srv != 0xFFFFFFFFu) {
            float4 prev_c = mul(float4(P, 1.0f), fcb.prev_unjittered_vp);
            if (prev_c.w > 0.1f) {
                float2 p_uv = float2(prev_c.x / prev_c.w * 0.5f + 0.5f, 0.5f - prev_c.y / prev_c.w * 0.5f);
                float3 prev_vs = mul(float4(P, 1.0f), fcb.prev_view).xyz;
                float p_w = froxel_vz_to_slice(prev_vs.z);
                if (all(p_uv >= 0.0f) && all(p_uv <= 1.0f) && prev_vs.z >= 0.1f && prev_vs.z <= 120.0f) {
                    Texture3D<float4> froxel_prev = ResourceDescriptorHeap[atelier_safe_desc(g_prev_hdr_or_refl_srv)];
                    SamplerState smp = SamplerDescriptorHeap[0];
                    float4 hist_val = froxel_prev.SampleLevel(smp, float3(p_uv, p_w), 0.0f);
                    if (all(isfinite(hist_val))) {
                        hist_val.rgb *= fcb.exposure_ratio;
                        curr_val = lerp(hist_val, curr_val, 0.12f);
                    }
                }
            }
        }
        froxel_curr[fc] = curr_val;
        return;
    }

    // ========================================================================
    // Mode 4: 160x90x64 Froxel Front-to-Back Analytical Integration
    // ========================================================================
    if (comp_mode == 4u) {
        uint2 fxy = dtid.xy;
        if (fxy.x >= 160u || fxy.y >= 90u)
            return;
        Texture3D<float4> froxel_src = ResourceDescriptorHeap[atelier_safe_desc(g_refl_srv)];
        RWTexture3D<float4> froxel_lut = ResourceDescriptorHeap[atelier_safe_desc(g_uav0)];
        float2 uv = (float2(fxy) + 0.5f) / float2(160.0f, 90.0f);
        float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
        float ray_cos = (abs(fcb.projection[2][3]) <= 1e-6f)
                            ? 1.0f
                            : length(float3(ndc.x / max(abs(fcb.projection[0][0]), 1e-4f),
                                            ndc.y / max(abs(fcb.projection[1][1]), 1e-4f), 1.0f));

        float3 accum_l = 0.0f;
        float accum_t = 1.0f;
        [loop] for (uint s = 0u; s < 64u; ++s) {
            float4 sc = froxel_src.Load(int4(int(fxy.x), int(fxy.y), int(s), 0));
            float ds = (froxel_slice_to_vz(float(s) + 1.0f) - froxel_slice_to_vz(float(s))) * ray_cos;
            float ext = max(sc.a, 0.0f);
            float step_t = exp(-ext * ds);
            float3 sint = (ext > 1e-6f) ? (sc.rgb * (1.0f - step_t) / ext) : (sc.rgb * ds);
            accum_l += accum_t * sint;
            accum_t *= step_t;
            froxel_lut[uint3(fxy, s)] = float4(accum_l, accum_t);
        }
        return;
    }

    // ========================================================================
    // Mode 0: 3-Tier Coat-Aware Reflections (Uniform Group Flow for LDS Compaction!)
    // ========================================================================
    uint2 tile8_px_off = uint2(fcb.scene_tile8_offset_packed & 0xFFFFu, fcb.scene_tile8_offset_packed >> 16u) * 8u;

    if (comp_mode == 0u) {
        uint2 px = dtid.xy + tile8_px_off;
        bool in_bounds = (px.x < fcb.render_extent.x && px.y < fcb.render_extent.y);
        float2 pxc = float2(px) + 0.5f;
        bool in_vp = in_bounds && (pxc.x >= fcb.scene_viewport.x && pxc.x < fcb.scene_viewport.x + fcb.scene_viewport.z &&
                                   pxc.y >= fcb.scene_viewport.y && pxc.y < fcb.scene_viewport.y + fcb.scene_viewport.w);
        bool refl_on = ((fcb.flags & (1u << 3u)) != 0u);

        if (gindex < 2u)
            gs_refl_comp_mask[gindex] = 0u;
        if (gindex == 0u) {
            gs_refl_work_cnt = 0u;
            if (refl_on && (fcb.flags & (1u << 7u)) != 0u && g_refl_srv != 0xFFFFFFFFu)
                gs_refl_inv_prev_vp = refl_invert_4x4(fcb.prev_unjittered_vp, fcb.inv_unjittered_vp);
        }
        gs_refl_rad[gindex] = 0.0f;
        gs_refl_dist[gindex] = 50.0f;
        GroupMemoryBarrierWithGroupSync();

        Texture2D<float> depth_tex = ResourceDescriptorHeap[atelier_safe_desc(g_depth_srv)];
        float dz = in_vp ? depth_tex.Load(int3(px, 0)) : 0.0f;

        bool valid_surf = false;
        bool needs_dxr = false;
        bool hit_screen = false;
        float r_trace = 1.0f;
        float hit_dist = 50.0f;
        float3 P = 0.0f;
        float3 GN_surf = float3(0.0f, 1.0f, 0.0f);
        float3 N_refl = float3(0.0f, 1.0f, 0.0f);
        float3 R = float3(0.0f, 1.0f, 0.0f);
        float3 refl_rad = 0.0f;

        float tier_blend = 0.0f;
        float3 probe_fallback_rad = 0.0f;
        float probe_fallback_dist = 4.0f;

        if (dz > 0.0f) {
            Texture2D<uint2> slab0_tex = ResourceDescriptorHeap[atelier_safe_desc(g_slab0_srv)];
            uint2 s0_raw = slab0_tex.Load(int3(px, 0));
            uint prof_id = (s0_raw.y >> 26u) & 7u;
            uint2 s1_raw = uint2(0u, 0u);
            if (prof_id == SLAB_PROFILE_COAT && g_slab1_srv != 0xFFFFFFFFu) {
                Texture2D<uint2> slab1_tex = ResourceDescriptorHeap[atelier_safe_desc(g_slab1_srv)];
                s1_raw = slab1_tex.Load(int3(px, 0));
            }
            OpenPbrSurface surf = unpack_openpbr_slab(s0_raw, s1_raw);
            if ((surf.flags & 1u) != 0u) {
                valid_surf = true;
                float3 GN = surf.shading_normal;
                if (g_slab2_srv != 0xFFFFFFFFu) {
                    Texture2D<uint> gn_tex = ResourceDescriptorHeap[atelier_safe_desc(g_slab2_srv)];
                    GN = unpack_oct_snorm16(gn_tex.Load(int3(px, 0)));
                }
                GN_surf = GN;

                // Single-trace DielectricCoat rule: read exact `Slab1` coat roughness & curved-mesh-safe coat normal!
                if (prof_id == SLAB_PROFILE_COAT && surf.lobe_weight > 0.0f) {
                    r_trace = surf.lobe_param0;
                    N_refl = eval_coat_normal(surf.shading_normal, GN, surf.lobe_weight);
                } else {
                    r_trace = surf.roughness;
                    N_refl = surf.shading_normal;
                }

                P = comp_reconstruct_world(pxc, dz, fcb.inv_view_projection, fcb);
                float3 eye_pos = mul(float4(P, 1.0f), fcb.view).xyz;
                float3 V = safe_unit_normal(
                    mul((abs(fcb.projection[2][3]) > 1e-6f ? -eye_pos : float3(0.0f, 0.0f, -1.0f)), (float3x3)fcb.inv_view));
                R = reflect(-V, N_refl);
                if (dot(R, N_refl) < 1e-3f)
                    R = safe_unit_normal(R + N_refl * (1e-3f - dot(R, N_refl)));
                if (dot(R, GN) < 1e-3f)
                    R = safe_unit_normal(R + GN * (1e-3f - dot(R, GN)));

                float3 bent_w = N_refl;
                float bent_vis = 1.0f;
                if ((fcb.flags & (1u << 2u)) != 0u && g_tile_emissive_srv != 0xFFFFFFFFu) {
                    Texture2D<float4> ssilvb = ResourceDescriptorHeap[atelier_safe_desc(g_tile_emissive_srv)];
                    float4 ao = ssilvb.Load(int3(px, 0));
                    bent_w = safe_unit_normal(ao.xyz);
                    bent_vis = saturate(ao.w);
                }

                float rough_cut = clamp(fcb.reflection_roughness, 0.0f, 0.80f);
                float blend_lo = max(0.16f, rough_cut - 0.12f);
                if (!refl_on || r_trace >= rough_cut) {
                    // Tier 3 (`r >= rough_cut` or Reflections Off): 0 rays, Screen-Probes directional octahedral lobe -> GI -> Sky!
                    float3 R_bent = safe_unit_normal(lerp(bent_w, R, saturate(bent_vis * (1.0f - 0.5f * r_trace) + 0.15f)));
                    hit_dist = 4.0f;
                    if (!sample_screen_probes_rough_specular(px, P, GN, N_refl, R, bent_w, bent_vis, r_trace, depth_tex,
                                                             fcb, refl_rad, hit_dist)) {
                        if (g_gi_srv != 0xFFFFFFFFu && (fcb.flags & 2u) != 0u) {
                            Texture2D<float4> gi_tex = ResourceDescriptorHeap[atelier_safe_desc(g_gi_srv)];
                            refl_rad = gi_tex.Load(int3(px, 0)).rgb;
                        } else {
                            refl_rad = eval_atelier_sky(R_bent, r_trace * 6.0f, fcb) * bent_vis;
                        }
                    }
                } else {
                    if (r_trace > blend_lo && rough_cut > blend_lo + 1e-3f) {
                        tier_blend = smoothstep(blend_lo, rough_cut, r_trace);
                        if (!sample_screen_probes_rough_specular(px, P, GN, N_refl, R, bent_w, bent_vis, r_trace,
                                                                 depth_tex, fcb, probe_fallback_rad, probe_fallback_dist)) {
                            if (g_gi_srv != 0xFFFFFFFFu && (fcb.flags & 2u) != 0u) {
                                Texture2D<float4> gi_tex = ResourceDescriptorHeap[atelier_safe_desc(g_gi_srv)];
                                probe_fallback_rad = gi_tex.Load(int3(px, 0)).rgb;
                            } else {
                                float3 R_b = safe_unit_normal(lerp(bent_w, R, saturate(bent_vis * (1.0f - 0.5f * r_trace) + 0.15f)));
                                probe_fallback_rad = eval_atelier_sky(R_b, r_trace * 6.0f, fcb) * bent_vis;
                            }
                        }
                    }
                    // Tier 1 (`r <= 0.12` mirror) & Tier 2 (`0.12 < r < rough_cut` Bounded VNDF):
                    if (r_trace > 0.12f) {
                        float3 T, B;
                        atelier_build_onb(N_refl, T, B);
                        float3 V_loc = float3(dot(V, T), dot(V, B), max(dot(V, N_refl), 1e-4f));
                        uint t_phase = ((fcb.flags & (1u << 7u)) != 0u) ? fcb.frame_index : 0u;
                        float2 u = atelier_blue_jitter2(px, t_phase, 0x517CC1B7u);
                        float3 H_loc = sample_bounded_vndf(V_loc, r_trace * r_trace, u);
                        float3 H_w = normalize(T * H_loc.x + B * H_loc.y + N_refl * H_loc.z);
                        float3 cand_R = reflect(-V, H_w);
                        if (dot(cand_R, GN) > 1e-3f && dot(cand_R, N_refl) > 1e-3f)
                            R = cand_R;
                    }

                    refl_rad = eval_atelier_sky(R, r_trace * 5.0f, fcb);

                    // Hierarchical Hi-Z Screen-Space March with Near-Plane Ray Clipping & Exact Perspective Hit Distance:
                    if ((fcb.flags & (1u << 7u)) != 0u && g_prev_hdr_or_refl_srv != 0xFFFFFFFFu) {
                        float3 orig_w = P + GN * 3e-3f;
                        float3 orig_v = mul(float4(orig_w, 1.0f), fcb.view).xyz;
                        float3 R_v = mul(R, (float3x3)fcb.view);
                        float max_d = min(fcb.reflection_distance, 35.0f);
                        if (abs(fcb.projection[2][3]) > 1e-6f && R_v.z < -1e-4f)
                            max_d = min(max_d, max(0.05f, (orig_v.z - 0.12f) / (-R_v.z)));

                        float4 c0 = mul(float4(orig_w, 1.0f), fcb.view_projection);
                        float4 c1 = mul(float4(orig_w + R * max_d, 1.0f), fcb.view_projection);
                        if (c0.w > 0.08f && c1.w > 0.08f) {
                            float2 p0 = float2(fcb.scene_viewport.x + (c0.x / c0.w * 0.5f + 0.5f) * fcb.scene_viewport.z,
                                               fcb.scene_viewport.y + (0.5f - c0.y / c0.w * 0.5f) * fcb.scene_viewport.w);
                            float2 p1 = float2(fcb.scene_viewport.x + (c1.x / c1.w * 0.5f + 0.5f) * fcb.scene_viewport.z,
                                               fcb.scene_viewport.y + (0.5f - c1.y / c1.w * 0.5f) * fcb.scene_viewport.w);
                            float zow0 = c0.z / c0.w;
                            float zow1 = c1.z / c1.w;
                            float inv_w0 = 1.0f / c0.w;
                            float inv_w1 = 1.0f / c1.w;

                            Texture2D<float2> hzb_tex = ResourceDescriptorHeap[atelier_safe_desc(fcb.hzb_srv)];
                            uint hzb_w0 = 1u, hzb_h0 = 1u, hzb_mips = 0u;
                            if (fcb.hzb_srv != 0xFFFFFFFFu)
                                hzb_tex.GetDimensions(0u, hzb_w0, hzb_h0, hzb_mips);
                            int max_hzb_mip = min(int(hzb_mips) - 1, 3);
                            uint2 hzb_dim0 = max(uint2(1u, 1u), uint2(hzb_w0, hzb_h0));
                            float ray_len_px = max(length(p1 - p0), 1.0f);
                            float t = min(1.5f / ray_len_px, 0.08f);
                            int mip = 0;

                            [loop] for (uint iter = 0u; iter < 24u && t <= 1.0f; ++iter) {
                                float2 sp = lerp(p0, p1, t);
                                if (sp.x < fcb.scene_viewport.x || sp.x >= fcb.scene_viewport.x + fcb.scene_viewport.z ||
                                    sp.y < fcb.scene_viewport.y || sp.y >= fcb.scene_viewport.y + fcb.scene_viewport.w)
                                    break;
                                float rz = lerp(zow0, zow1, t);

                                if (max_hzb_mip >= 0 && mip > 0) {
                                    uint2 mdim = max(uint2(1u, 1u), hzb_dim0 >> uint(mip));
                                    int2 hc = clamp(int2(sp) >> (mip + 1), int2(0, 0), int2(mdim) - 1);
                                    float hz_max = hzb_tex.Load(int3(hc, mip)).y;
                                    if (hz_max > 0.0f && rz <= hz_max) {
                                        --mip;
                                        continue;
                                    }
                                    t += float(1u << uint(mip + 1)) / ray_len_px;
                                    mip = min(mip + 1, max_hzb_mip);
                                } else {
                                    float sd = depth_tex.Load(int3(int2(sp), 0));
                                    if (sd > 0.0f && rz <= sd) {
                                        if ((sd - rz) <= max(sd * 0.03f, 2e-4f)) {
                                            Texture2D<float2> mot_tex = ResourceDescriptorHeap[atelier_safe_desc(g_motion_srv)];
                                            float2 prev_px = sp + mot_tex.Load(int3(int2(sp), 0)) * float2(fcb.render_extent);
                                            if (prev_px.x >= fcb.prev_scene_viewport.x + 0.5f &&
                                                prev_px.x <= fcb.prev_scene_viewport.x + fcb.prev_scene_viewport.z - 0.5f &&
                                                prev_px.y >= fcb.prev_scene_viewport.y + 0.5f &&
                                                prev_px.y <= fcb.prev_scene_viewport.y + fcb.prev_scene_viewport.w - 0.5f) {
                                                Texture2D<float4> prev_hdr = ResourceDescriptorHeap[atelier_safe_desc(g_prev_hdr_or_refl_srv)];
                                                SamplerState smp = SamplerDescriptorHeap[0];
                                                float2 prev_uv = prev_px / float2(fcb.render_extent);
                                                refl_rad = min(prev_hdr.SampleLevel(smp, prev_uv, 0.0f).rgb * fcb.exposure_ratio, 32.0f);
                                                float inv_wt = lerp(inv_w0, inv_w1, t);
                                                hit_dist = clamp(max_d * (t * inv_w1) / max(inv_wt, 1e-6f), 0.02f, max_d);
                                                hit_screen = true;
                                            }
                                        }
                                        break;
                                    }
                                    t += 2.0f / ray_len_px;
                                    if (max_hzb_mip >= 1)
                                        mip = 1;
                                }
                            }
                        }
                    }

                    if (!hit_screen && fcb.tlas_srv != 0xFFFFFFFFu && fcb.reflection_distance > 0.05f)
                        needs_dxr = true;

                    if (g_uav1 != 0xFFFFFFFFu)
                        InterlockedAdd(gs_refl_work_cnt, 1u);
                }
            }
        }
        if (needs_dxr)
            InterlockedOr(gs_refl_comp_mask[gindex >> 5u], 1u << (gindex & 31u));
        GroupMemoryBarrierWithGroupSync();

        if (gindex == 0u && gs_refl_work_cnt > 0u && g_uav1 != 0xFFFFFFFFu) {
            RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[atelier_safe_desc(g_uav1)];
            InterlockedAdd(counters[5u], gs_refl_work_cnt);
        }

        // Off-Screen 64-Bit LDS Bitmask Compacted DXR RayQuery Fallback:
        uint rm0 = gs_refl_comp_mask[0];
        uint rm1 = gs_refl_comp_mask[1];
        uint total_dxr = countbits(rm0) + countbits(rm1);
        if (needs_dxr) {
            uint lo_mask = (gindex < 32u) ? ((1u << gindex) - 1u) : 0xFFFFFFFFu;
            uint hi_mask = (gindex > 32u) ? ((1u << (gindex - 32u)) - 1u) : 0u;
            uint dst = countbits(rm0 & lo_mask) + countbits(rm1 & hi_mask);
            gs_refl_orig[dst] = P + GN_surf * 3e-3f;
            gs_refl_dir[dst] = pack_oct_snorm16(R);
            gs_refl_lane[dst] = gindex;
        }
        GroupMemoryBarrierWithGroupSync();

        if (gindex < total_dxr) {
            float3 ro = gs_refl_orig[gindex];
            float3 rd = unpack_oct_snorm16(gs_refl_dir[gindex]);
            uint src_lane = gs_refl_lane[gindex];

            RaytracingAccelerationStructure tlas = ResourceDescriptorHeap[atelier_safe_desc(fcb.tlas_srv)];
            RayDesc ray = {ro, 2e-3f, rd, min(fcb.reflection_distance, 80.0f)};
            RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
            q.TraceRayInline(tlas, RAY_FLAG_FORCE_OPAQUE, 0xFFu, ray);
            q.Proceed();

            float3 hit_rad = eval_atelier_sky(rd, 2.0f, fcb);
            float dxr_dist = 50.0f;
            if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
                uint inst_id = q.CommittedInstanceID();
                StructuredBuffer<GpuInstanceRecord> instances = ResourceDescriptorHeap[atelier_safe_desc(fcb.instances_srv)];
                StructuredBuffer<GpuMeshDescriptor> meshes = ResourceDescriptorHeap[atelier_safe_desc(fcb.mesh_table_srv)];
                StructuredBuffer<uint> part_bases = ResourceDescriptorHeap[atelier_safe_desc(fcb.part_tri_base_srv)];
                GpuInstanceRecord inst = instances[inst_id];
                GpuMeshDescriptor mesh = meshes[inst.mesh_pool_index];
                uint global_tri = part_bases[inst.part_table_base + q.CommittedGeometryIndex()] + q.CommittedPrimitiveIndex();

                StructuredBuffer<uint> blas_idx = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(mesh.blas_indices_srv))];
                StructuredBuffer<GpuPackedPosition> pos_s = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(mesh.positions_srv))];
                StructuredBuffer<GpuPackedAttributes> attr_s = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(mesh.attributes_srv))];
                StructuredBuffer<uint> fmeta_s = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(mesh.face_meta_srv))];

                uint i0 = blas_idx[global_tri * 3u + 0u];
                uint i1 = blas_idx[global_tri * 3u + 1u];
                uint i2 = blas_idx[global_tri * 3u + 2u];
                uint fmeta = fmeta_s[global_tri];
                bool wf = (fmeta >> 31u) != 0u;
                bool closed = (fmeta & (1u << 30u)) != 0u;
                StructuredBuffer<uint> slot_map = ResourceDescriptorHeap[atelier_safe_desc(fcb.material_slot_map_srv)];
                StructuredBuffer<GpuSurfaceRecord> surfaces = ResourceDescriptorHeap[atelier_safe_desc(fcb.material_surfaces_srv)];
                uint sidx = slot_map[inst.material_slot_offset + (fmeta & 0x07FFFFFFu)];
                uint sflags = surfaces[sidx].flags;

                float2 b = q.CommittedTriangleBarycentrics();
                float w0 = 1.0f - b.x - b.y;
                float3 lp0 = pos_s[i0].pos, lp1 = pos_s[i1].pos, lp2 = pos_s[i2].pos;
                float3 hit_wp = mul(float4(lp0 * w0 + lp1 * b.x + lp2 * b.y, 1.0f), inst.world).xyz;
                float3 hit_gn = reconstruct_geom_normal(lp0, lp1, lp2, unpack_oct_snorm16(attr_s[i0].oct_normal), wf, inst);
                bool bf = dot(hit_gn, rd) > 0.0f;
                float3 hit_n = bf ? -hit_gn : hit_gn;
                float hit_t = q.CommittedRayT();
                dxr_dist = clamp(hit_t, 0.02f, 50.0f);

                hit_rad = 0.0f;
                bool valid_hit_side = !(bf && (sflags & 6u) == 0u && closed);
                bool hit_sharc_ok = false;
                if (valid_hit_side && fcb.wrc_curr_srv != 0xFFFFFFFFu && fcb.wrc_hash_keys_srv != 0xFFFFFFFFu &&
                    fcb.wrc_voxel_keys_srv != 0xFFFFFFFFu && (fcb.flags & 2u) != 0u) {
                    StructuredBuffer<uint> hash_keys = ResourceDescriptorHeap[atelier_safe_desc(fcb.wrc_hash_keys_srv)];
                    StructuredBuffer<uint2> voxel_keys = ResourceDescriptorHeap[atelier_safe_desc(fcb.wrc_voxel_keys_srv)];
                    StructuredBuffer<uint4> wrc_curr = ResourceDescriptorHeap[atelier_safe_desc(fcb.wrc_curr_srv)];
                    float vsz_r = sharc_get_voxel_size(sharc_get_level(hit_wp, fcb.camera_eye));
                    uint r_t = ((fcb.flags & (1u << 7u)) != 0u) ? fcb.frame_index : 0u;
                    float2 r_jit = atelier_blue_jitter2(uint2(src_lane, gid.x ^ (gid.y << 10u)), r_t, 0x85EBCA6Bu);
                    float3 j_pos = sharc_jitter_query_pos(hit_wp, hit_n, vsz_r, hit_t, r_jit);
                    hit_sharc_ok = sharc_query_resolved_srv(hash_keys, voxel_keys, wrc_curr, 0u, j_pos, hit_n, fcb.camera_eye, hit_rad);
                    if (!hit_sharc_ok)
                        hit_sharc_ok = sharc_query_resolved_srv(hash_keys, voxel_keys, wrc_curr, 0u, hit_wp, hit_n, fcb.camera_eye, hit_rad);
                }
                if (valid_hit_side && !hit_sharc_ok)
                    hit_rad = eval_atelier_sky_diffuse_radiance(hit_n, fcb) * 0.16f;
            }
            gs_refl_rad[src_lane] = hit_rad;
            gs_refl_dist[src_lane] = dxr_dist;
        }
        GroupMemoryBarrierWithGroupSync();

        if (needs_dxr) {
            refl_rad = gs_refl_rad[gindex];
            hit_dist = gs_refl_dist[gindex];
        }
        if (tier_blend > 0.0f) {
            refl_rad = lerp(refl_rad, probe_fallback_rad, tier_blend);
            hit_dist = lerp(hit_dist, probe_fallback_dist, tier_blend);
        }

        if (in_bounds) {
            RWTexture2D<float4> refl_uav = ResourceDescriptorHeap[atelier_safe_desc(g_uav0)];
            RWTexture2D<float4> hist_uav = ResourceDescriptorHeap[atelier_safe_desc(g_direct_spec_srv)];
            RWTexture2D<float2> mom_uav = ResourceDescriptorHeap[atelier_safe_desc(g_direct_diff_srv)];
            if (!valid_surf) {
                refl_uav[px] = float4(0.0f, 0.0f, 0.0f, -1.0f);
                if (g_direct_spec_srv != 0xFFFFFFFFu)
                    hist_uav[px] = 0.0f;
                if (g_direct_diff_srv != 0xFFFFFFFFu)
                    mom_uav[px] = float2(0.0f, 50.0f);
            } else {
                float cur_luma_k = atelier_luma(atelier_karis_compress(refl_rad));
                float accum_m2 = cur_luma_k * cur_luma_k;
                float accum_dist = hit_dist;
                float hist_frames = 1.0f;

                // AMD Capsaicin GI-1.2 Dual-Source Virtual-Hit Parallax + Surface-Motion Reprojection (2x2 Bilateral Gather):
                if (refl_on && (fcb.flags & (1u << 7u)) != 0u && g_refl_srv != 0xFFFFFFFFu &&
                    fcb.prev_view_z_srv != 0xFFFFFFFFu && prev_gn_srv != 0xFFFFFFFFu) {
                    float px_ws = gi_pixel_world_size(P, fcb);
                    float refl_grazing = 1.0f / max(abs(dot(GN_surf, safe_unit_normal(fcb.camera_eye - P))), 0.10f);
                    Texture2D<float> prev_d_tex = ResourceDescriptorHeap[atelier_safe_desc(fcb.prev_view_z_srv)];
                    Texture2D<uint> prev_gn_tex = ResourceDescriptorHeap[atelier_safe_desc(prev_gn_srv)];
                    Texture2D<float4> prev_r_tex = ResourceDescriptorHeap[atelier_safe_desc(g_refl_srv)];
                    bool has_prev_mom = (g_atrous_step != 0xFFFFFFFFu);

                    Texture2D<float2> mot_tex = ResourceDescriptorHeap[atelier_safe_desc(g_motion_srv)];
                    float2 mot = mot_tex.Load(int3(px, 0));
                    float2 prev_surf_px = (pxc - fcb.jitter_pixels) + mot * float2(fcb.render_extent);
                    float4 cam_c = mul(float4(P, 1.0f), fcb.prev_unjittered_vp);
                    float2 cam_px = prev_surf_px;
                    float cam_exp_dz = dz;
                    if (abs(cam_c.w) > 1e-5f) {
                        cam_px = float2(fcb.prev_scene_viewport.x + (cam_c.x / cam_c.w * 0.5f + 0.5f) * fcb.prev_scene_viewport.z,
                                        fcb.prev_scene_viewport.y + (0.5f - cam_c.y / cam_c.w * 0.5f) * fcb.prev_scene_viewport.w);
                        cam_exp_dz = cam_c.z / cam_c.w;
                    }
                    bool is_dyn = length(cam_px - prev_surf_px) > 1.5f;

                    // 1. Surface-motion candidate (`prev_surf_px` 2x2 bilateral gather, zero sky/silhouette darkening):
                    float surf_w = 0.0f;
                    float4 prev_surf_r = 0.0f;
                    float2 prev_surf_m = float2(accum_m2, hit_dist);

                    if (prev_surf_px.x >= fcb.prev_scene_viewport.x && prev_surf_px.x < fcb.prev_scene_viewport.x + fcb.prev_scene_viewport.z &&
                        prev_surf_px.y >= fcb.prev_scene_viewport.y && prev_surf_px.y < fcb.prev_scene_viewport.y + fcb.prev_scene_viewport.w) {
                        int2 s_base = int2(floor(prev_surf_px - 0.5f));
                        float2 s_frac = saturate((prev_surf_px - 0.5f) - float2(s_base));
                        float4 s_r_sum = 0.0f;
                        float2 s_m_sum = 0.0f;
                        float s_w_sum = 0.0f;

                        [loop] for (int sy = 0; sy <= 1; ++sy) {
                            [loop] for (int sx = 0; sx <= 1; ++sx) {
                                int2 qp = s_base + int2(sx, sy);
                                float2 qpc = float2(qp) + 0.5f;
                                if (qp.x < 0 || qp.y < 0 || qp.x >= int(fcb.render_extent.x) || qp.y >= int(fcb.render_extent.y) ||
                                    qpc.x < fcb.prev_scene_viewport.x || qpc.x >= fcb.prev_scene_viewport.x + fcb.prev_scene_viewport.z ||
                                    qpc.y < fcb.prev_scene_viewport.y || qpc.y >= fcb.prev_scene_viewport.y + fcb.prev_scene_viewport.w)
                                    continue;
                                float pd_s = prev_d_tex.Load(int3(qp, 0));
                                if (pd_s <= 0.0f)
                                    continue;
                                float4 pr = prev_r_tex.Load(int3(qp, 0));
                                if (pr.w <= 0.0f)
                                    continue;
                                float3 pGN = unpack_oct_snorm16(prev_gn_tex.Load(int3(qp, 0)));
                                if (dot(pGN, GN_surf) < 0.80f)
                                    continue;

                                bool ok_s = false;
                                if (!is_dyn) {
                                    float3 prev_P_s = refl_reconstruct_prev_world(qpc, pd_s, gs_refl_inv_prev_vp, fcb);
                                    ok_s = (abs(dot(prev_P_s - P, GN_surf)) <= max(0.03f, px_ws * 3.5f)) &&
                                           (length(prev_P_s - P) <= max(0.15f, px_ws * 8.0f * refl_grazing));
                                } else {
                                    ok_s = (abs(cam_c.w) > 1e-5f) &&
                                           (abs(pd_s - cam_exp_dz) <= max(1e-4f, max(pd_s, cam_exp_dz) * 0.04f * refl_grazing));
                                }
                                if (!ok_s)
                                    continue;

                                float bw = (sx == 0 ? (1.0f - s_frac.x) : s_frac.x) * (sy == 0 ? (1.0f - s_frac.y) : s_frac.y);
                                float w = max(bw, 1e-3f) * pow(saturate(dot(pGN, GN_surf)), 6.0f);
                                s_r_sum += float4(pr.rgb * fcb.exposure_ratio, pr.w) * w;
                                if (has_prev_mom) {
                                    Texture2D<float2> pm_tex = ResourceDescriptorHeap[atelier_safe_desc(g_atrous_step)];
                                    s_m_sum += pm_tex.Load(int3(qp, 0)) * w;
                                } else {
                                    s_m_sum += float2(accum_m2, hit_dist) * w;
                                }
                                s_w_sum += w;
                            }
                        }
                        if (s_w_sum > 1e-4f) {
                            prev_surf_r = s_r_sum / s_w_sum;
                            prev_surf_m = s_m_sum / s_w_sum;
                            surf_w = 1.0f;
                        }
                    }

                    // 2. Virtual reflected hit-point parallax candidate (`P_virt = E + I * (||P - E|| + d_hit)`):
                    float virt_w = 0.0f;
                    float4 prev_virt_r = 0.0f;
                    float2 prev_virt_m = prev_surf_m;
                    float d_for_virt = (surf_w > 0.0f && hit_dist >= 49.0f) ? prev_surf_m.y : hit_dist;
                    float nm_flat = smoothstep(0.990f, 0.999f, dot(N_refl, GN_surf));
                    if (!is_dyn && nm_flat > 1e-3f && r_trace < 0.48f && abs(fcb.projection[2][3]) > 1e-6f && d_for_virt > 0.01f) {
                        float3 cam_ray = P - fcb.camera_eye;
                        float surf_d = max(length(cam_ray), 1e-4f);
                        float3 P_virt = fcb.camera_eye + (cam_ray / surf_d) * (surf_d + d_for_virt);
                        float4 vc = mul(float4(P_virt, 1.0f), fcb.prev_unjittered_vp);
                        if (vc.w > 0.08f) {
                            float2 prev_virt_px = float2(fcb.prev_scene_viewport.x + (vc.x / vc.w * 0.5f + 0.5f) * fcb.prev_scene_viewport.z,
                                                         fcb.prev_scene_viewport.y + (0.5f - vc.y / vc.w * 0.5f) * fcb.prev_scene_viewport.w);
                            if (prev_virt_px.x >= fcb.prev_scene_viewport.x && prev_virt_px.x < fcb.prev_scene_viewport.x + fcb.prev_scene_viewport.z &&
                                prev_virt_px.y >= fcb.prev_scene_viewport.y && prev_virt_px.y < fcb.prev_scene_viewport.y + fcb.prev_scene_viewport.w) {
                                int2 v_base = int2(floor(prev_virt_px - 0.5f));
                                float2 v_frac = saturate((prev_virt_px - 0.5f) - float2(v_base));
                                float4 v_r_sum = 0.0f;
                                float2 v_m_sum = 0.0f;
                                float v_w_sum = 0.0f;

                                [loop] for (int vy = 0; vy <= 1; ++vy) {
                                    [loop] for (int vx = 0; vx <= 1; ++vx) {
                                        int2 qp = v_base + int2(vx, vy);
                                        float2 qpc = float2(qp) + 0.5f;
                                        if (qp.x < 0 || qp.y < 0 || qp.x >= int(fcb.render_extent.x) || qp.y >= int(fcb.render_extent.y) ||
                                            qpc.x < fcb.prev_scene_viewport.x || qpc.x >= fcb.prev_scene_viewport.x + fcb.prev_scene_viewport.z ||
                                            qpc.y < fcb.prev_scene_viewport.y || qpc.y >= fcb.prev_scene_viewport.y + fcb.prev_scene_viewport.w)
                                            continue;
                                        float pd_v = prev_d_tex.Load(int3(qp, 0));
                                        if (pd_v <= 0.0f)
                                            continue;
                                        float4 prv = prev_r_tex.Load(int3(qp, 0));
                                        if (prv.w <= 0.0f)
                                            continue;
                                        // Strict coplanar reflector check (`pGN` matches `GN_surf` AND lies on `(P, GN_surf)` plane):
                                        float3 pGN = unpack_oct_snorm16(prev_gn_tex.Load(int3(qp, 0)));
                                        if (dot(pGN, GN_surf) < 0.99f)
                                            continue;
                                        float3 prev_P_v = refl_reconstruct_prev_world(qpc, pd_v, gs_refl_inv_prev_vp, fcb);
                                        if (abs(dot(prev_P_v - P, GN_surf)) > max(0.025f, px_ws * 3.0f))
                                            continue;

                                        float bw = (vx == 0 ? (1.0f - v_frac.x) : v_frac.x) * (vy == 0 ? (1.0f - v_frac.y) : v_frac.y);
                                        float w = max(bw, 1e-3f);
                                        v_r_sum += float4(prv.rgb * fcb.exposure_ratio, prv.w) * w;
                                        if (has_prev_mom) {
                                            Texture2D<float2> pm_tex = ResourceDescriptorHeap[atelier_safe_desc(g_atrous_step)];
                                            v_m_sum += pm_tex.Load(int3(qp, 0)) * w;
                                        } else {
                                            v_m_sum += prev_surf_m * w;
                                        }
                                        v_w_sum += w;
                                    }
                                }
                                if (v_w_sum > 1e-4f) {
                                    prev_virt_r = v_r_sum / v_w_sum;
                                    prev_virt_m = v_m_sum / v_w_sum;
                                    virt_w = nm_flat * (1.0f - saturate((r_trace - 0.06f) / 0.38f));
                                }
                            }
                        }
                    }

                    // Dual-source blend between parallax-corrected virtual reprojection and surface motion:
                    if (surf_w > 0.0f || virt_w > 0.0f) {
                        float w_s = surf_w * (1.0f - 0.75f * virt_w);
                        float w_v = virt_w;
                        float w_tot = max(w_s + w_v, 1e-5f);
                        float3 hist_rgb = (prev_surf_r.rgb * w_s + prev_virt_r.rgb * w_v) / w_tot;
                        float hist_cnt = (prev_surf_r.w * w_s + prev_virt_r.w * w_v) / w_tot;
                        float2 hist_m = (prev_surf_m * w_s + prev_virt_m * w_v) / w_tot;

                        // Anti-firefly outlier clamp on 1-spp Bounded VNDF glossy samples once history matures:
                        if (r_trace > 0.12f && hist_cnt >= 3.0f) {
                            float l_cur = atelier_luma(refl_rad);
                            float l_hst = atelier_luma(hist_rgb);
                            float cap_l = max(8.0f * fcb.exposure_scale, l_hst * 6.0f + 0.5f * fcb.exposure_scale);
                            if (l_cur > cap_l)
                                refl_rad *= cap_l / max(l_cur, 1e-5f);
                            cur_luma_k = atelier_luma(atelier_karis_compress(refl_rad));
                        }

                        float max_frames = (r_trace <= 0.12f) ? 8.0f : 16.0f;
                        hist_frames = min(hist_cnt + 1.0f, max_frames);
                        float vel_px = length(mot * float2(fcb.render_extent));
                        float base_alpha = (r_trace <= 0.12f)
                                               ? lerp(0.20f, 0.70f, saturate(vel_px * 0.25f * (1.0f - virt_w)))
                                               : max(1.0f / hist_frames, 0.08f);

                        // Blend in hue-preserving Luma-Karis space for `Tier 2` glossy stability:
                        if (r_trace > 0.12f) {
                            float3 k_hst = atelier_karis_compress(hist_rgb);
                            float3 k_cur = atelier_karis_compress(refl_rad);
                            refl_rad = atelier_karis_expand(lerp(k_hst, k_cur, base_alpha));
                        } else {
                            refl_rad = lerp(hist_rgb, refl_rad, base_alpha);
                        }
                        accum_m2 = lerp(hist_m.x, cur_luma_k * cur_luma_k, max(base_alpha, 0.12f));
                        accum_dist = lerp(hist_m.y, hit_dist, max(base_alpha, 0.20f));
                    }
                }
                float3 safe_refl = clamp(refl_rad, 0.0f, 65504.0f);
                if (g_direct_spec_srv != 0xFFFFFFFFu)
                    hist_uav[px] = float4(safe_refl, hist_frames);
                if (g_direct_diff_srv != 0xFFFFFFFFu)
                    mom_uav[px] = float2(clamp(accum_m2, 0.0f, 65504.0f), clamp(accum_dist, 0.02f, 50.0f));
                refl_uav[px] = float4(safe_refl, r_trace);
            }
        }
        return;
    }

    uint2 px = (comp_mode == 1u) ? (dtid.xy + tile8_px_off) : dtid.xy;
    if (px.x >= fcb.render_extent.x || px.y >= fcb.render_extent.y)
        return;

    float2 pxc = float2(px) + 0.5f;
    bool in_vp = (pxc.x >= fcb.scene_viewport.x && pxc.x < fcb.scene_viewport.x + fcb.scene_viewport.z &&
                  pxc.y >= fcb.scene_viewport.y && pxc.y < fcb.scene_viewport.y + fcb.scene_viewport.w);
    Texture2D<float> depth_tex = ResourceDescriptorHeap[atelier_safe_desc(g_depth_srv)];
    float dz = in_vp ? depth_tex.Load(int3(px, 0)) : 0.0f;

    // ========================================================================
    // Mode 1: GI-1.2 Variance-Guided & Contact-Hardening Luma-Karis Ratio A-Trous Filter
    // ========================================================================
    if (comp_mode == 1u) {
        Texture2D<float4> src_r = ResourceDescriptorHeap[atelier_safe_desc(g_refl_srv)];
        RWTexture2D<float4> dst_r = ResourceDescriptorHeap[atelier_safe_desc(g_uav0)];
        bool has_src_mom = (g_slab1_srv != 0xFFFFFFFFu);
        bool has_dst_mom = (g_uav1 != 0xFFFFFFFFu);
        float4 center = src_r.Load(int3(px, 0));
        float2 c_mom = float2(0.0f, 10.0f);
        if (has_src_mom) {
            Texture2D<float2> src_m = ResourceDescriptorHeap[atelier_safe_desc(g_slab1_srv)];
            c_mom = src_m.Load(int3(px, 0));
        }

        float rough = center.a;
        float rough_cut_f = clamp(fcb.reflection_roughness, 0.14f, 0.80f);
        if (!in_vp || dz <= 0.0f || g_atrous_step == 0u || rough <= 0.12f || rough >= rough_cut_f) {
            dst_r[px] = center;
            if (has_dst_mom) {
                RWTexture2D<float2> dst_m = ResourceDescriptorHeap[atelier_safe_desc(g_uav1)];
                dst_m[px] = c_mom;
            }
            return;
        }

        Texture2D<uint2> slab0_tex = ResourceDescriptorHeap[atelier_safe_desc(g_slab0_srv)];
        float3 N = refl_eval_filter_normal(px, slab0_tex);
        float3 P_c = comp_reconstruct_world(pxc, dz, fcb.inv_view_projection, fcb);
        float px_ws = gi_pixel_world_size(P_c, fcb);
        int step_sz = int(g_atrous_step);

        // Contact-hardening specular cone footprint (`hit_dist * alpha / px_ws`):
        float hit_dist = clamp(c_mom.y, 0.02f, 50.0f);
        float cone_tan = max(rough * rough, 0.02f);
        float footprint_px = clamp((hit_dist * cone_tan) / max(px_ws, 1e-4f), 0.50f, 18.0f);
        float lobe_sharp = lerp(72.0f, 12.0f, saturate((rough - 0.12f) / max(rough_cut_f - 0.12f, 0.10f)));

        float3 c_karis = atelier_karis_compress(center.rgb);
        float luma_c = atelier_luma(c_karis);
        float var_c = max(0.0f, c_mom.x - luma_c * luma_c);
        float sigma_luma = max(sqrt(var_c), 0.04f * luma_c + 0.015f);

        float3 sum_k = c_karis;
        float sum_m2 = c_mom.x;
        float sum_dist = hit_dist;
        float wsum = 1.0f;

        [loop] for (int dy = -1; dy <= 1; ++dy) {
            [loop] for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0)
                    continue;
                int2 off_px = int2(dx, dy) * step_sz;
                int2 q = clamp(int2(px) + off_px, fcb.scissor_rect.xy,
                               max(fcb.scissor_rect.xy, fcb.scissor_rect.zw - 1));
                float qd = depth_tex.Load(int3(q, 0));
                if (qd <= 0.0f)
                    continue;
                float4 qr = src_r.Load(int3(q, 0));
                if (qr.a < 0.0f || qr.a >= rough_cut_f)
                    continue;
                float3 qN = refl_eval_filter_normal(uint2(q), slab0_tex);
                float3 Q = comp_reconstruct_world(float2(q) + 0.5f, qd, fcb.inv_view_projection, fcb);

                float dist2_px = float(off_px.x * off_px.x + off_px.y * off_px.y);
                float w_cone = exp(-0.5f * dist2_px / max(footprint_px * footprint_px, 0.25f));
                float wn = pow(saturate(dot(N, qN)), lobe_sharp);
                float wr = exp(-abs(rough - qr.a) * 10.0f);
                float wz = exp(-abs(dot(Q - P_c, N)) / max(0.015f, px_ws * float(step_sz) * 2.5f));

                float3 q_karis = atelier_karis_compress(qr.rgb);
                float luma_q = atelier_luma(q_karis);
                float wl = exp(-abs(luma_q - luma_c) / max(sigma_luma * 4.0f, 1e-3f));

                float kw = (dx == 0 || dy == 0) ? 0.5f : 0.25f;
                float w = w_cone * wn * wr * wz * wl * kw;
                if (w <= 1e-5f)
                    continue;

                sum_k += q_karis * w;
                if (has_dst_mom) {
                    if (has_src_mom) {
                        Texture2D<float2> src_m = ResourceDescriptorHeap[atelier_safe_desc(g_slab1_srv)];
                        float2 qm = src_m.Load(int3(q, 0));
                        sum_m2 += qm.x * w;
                        sum_dist += qm.y * w;
                    } else {
                        sum_m2 += (luma_q * luma_q) * w;
                    }
                }
                wsum += w;
            }
        }

        float3 out_rgb = atelier_karis_expand(sum_k / wsum);
        dst_r[px] = float4(clamp(out_rgb, 0.0f, 65504.0f), rough);
        if (has_dst_mom) {
            RWTexture2D<float2> dst_m = ResourceDescriptorHeap[atelier_safe_desc(g_uav1)];
            dst_m[px] = float2(clamp(sum_m2 / wsum, 0.0f, 65504.0f), clamp(sum_dist / wsum, 0.02f, 50.0f));
        }
        return;
    }

    // ========================================================================
    // Mode 2: SceneCompose CS -> SceneColorHDR + Specular-Free PrevDiffuseEmissiveRadiance + UnjitDepthHistory
    // ========================================================================
    RWTexture2D<float4> hdr_uav = ResourceDescriptorHeap[atelier_safe_desc(g_uav0)];
    if (!in_vp) {
        hdr_uav[px] = 0.0f;
        if ((px.x & 1u) == 0u && (px.y & 1u) == 0u && g_uav1 != 0xFFFFFFFFu) {
            RWTexture2D<float4> prev_diff_uav = ResourceDescriptorHeap[atelier_safe_desc(g_uav1)];
            prev_diff_uav[px >> 1u] = 0.0f;
        }
        if (g_atrous_step != 0xFFFFFFFFu) {
            RWTexture2D<float> unjit_hist = ResourceDescriptorHeap[atelier_safe_desc(g_atrous_step)];
            unjit_hist[px] = 0.0f;
        }
        return;
    }

    if (dz <= 0.0f) {
        // Background pixel: display sky when `sky_map` is bound OR `PhysicalAtmosphere` (`env_sky_mode == 1`) is active; otherwise black!
        float3 bg = 0.0f;
        bool show_sky_bg = (((fcb.flags & (1u << 5u)) != 0u) && (fcb.sky_map_srv != 0xFFFFFFFFu)) || (fcb.env_sky_mode == 1u);
        if (show_sky_bg) {
            float3 far_p = comp_reconstruct_world(pxc, 0.25f, fcb.inv_unjittered_vp, fcb);
            float3 dir = (abs(fcb.projection[2][3]) > 1e-6f)
                             ? safe_unit_normal(far_p - fcb.camera_eye)
                             : safe_unit_normal(mul(float3(0.0f, 0.0f, 1.0f), (float3x3)fcb.inv_view));
            bg = eval_atelier_sky(dir, 0.0f, fcb);
        }
        if (fcb.fog_density > 0.0f && fcb.froxel_srv != 0xFFFFFFFFu) {
            Texture3D<float4> froxel_lut = ResourceDescriptorHeap[atelier_safe_desc(fcb.froxel_srv)];
            SamplerState smp = SamplerDescriptorHeap[0];
            float2 vp_uv = (pxc - fcb.scene_viewport.xy) / max(fcb.scene_viewport.zw, 1.0f);
            float4 fog = froxel_lut.SampleLevel(smp, float3(vp_uv, 63.5f / 64.0f), 0.0f);
            bg = bg * fog.a + fog.rgb;
        }
        hdr_uav[px] = float4(clamp(bg, 0.0f, 65504.0f), 1.0f);
        if ((px.x & 1u) == 0u && (px.y & 1u) == 0u && g_uav1 != 0xFFFFFFFFu) {
            RWTexture2D<float4> prev_diff_uav = ResourceDescriptorHeap[atelier_safe_desc(g_uav1)];
            prev_diff_uav[px >> 1u] = 0.0f; // 0.0 alpha = sky/invalid sentinel (FP16-safe, zero +INF!)
        }
        if (g_atrous_step != 0xFFFFFFFFu) {
            RWTexture2D<float> unjit_hist = ResourceDescriptorHeap[atelier_safe_desc(g_atrous_step)];
            unjit_hist[px] = 0.0f;
        }
        return;
    }

    float3 P = comp_reconstruct_world(pxc, dz, fcb.inv_view_projection, fcb);
    if (g_atrous_step != 0xFFFFFFFFu) {
        RWTexture2D<float> unjit_hist = ResourceDescriptorHeap[atelier_safe_desc(g_atrous_step)];
        float4 unjit_c = mul(float4(P, 1.0f), fcb.unjittered_view_projection);
        unjit_hist[px] = (abs(unjit_c.w) > 1e-6f) ? saturate(unjit_c.z / unjit_c.w) : dz;
    }

    Texture2D<uint2> slab0_tex = ResourceDescriptorHeap[atelier_safe_desc(g_slab0_srv)];
    Texture2D<uint2> slab1_tex = ResourceDescriptorHeap[atelier_safe_desc(g_slab1_srv)];
    OpenPbrSurface surf = unpack_openpbr_slab(slab0_tex.Load(int3(px, 0)), slab1_tex.Load(int3(px, 0)));

    float3 eye_pos = mul(float4(P, 1.0f), fcb.view).xyz;
    float3 V = safe_unit_normal(
        mul((abs(fcb.projection[2][3]) > 1e-6f ? -eye_pos : float3(0.0f, 0.0f, -1.0f)), (float3x3)fcb.inv_view));
    float nv = max(dot(surf.shading_normal, V), 1e-4f);

    Texture2D<float4> dir_diff_tex = ResourceDescriptorHeap[atelier_safe_desc(g_direct_diff_srv)];
    Texture2D<float4> dir_spec_tex = ResourceDescriptorHeap[atelier_safe_desc(g_direct_spec_srv)];
    Texture2D<float4> gi_tex = ResourceDescriptorHeap[atelier_safe_desc(g_gi_srv)];
    Texture2D<float4> refl_tex = ResourceDescriptorHeap[atelier_safe_desc(g_refl_srv)];

    float3 direct_diff = dir_diff_tex.Load(int3(px, 0)).rgb;
    float3 direct_spec = dir_spec_tex.Load(int3(px, 0)).rgb;
    float4 gi_sample = gi_tex.Load(int3(px, 0));
    float3 gi_irrad = gi_sample.rgb;
    float gi_ao_vis = gi_sample.a;
    float4 refl_sample = refl_tex.Load(int3(px, 0));
    float3 refl_rad = refl_sample.rgb;
    float refl_r = refl_sample.a;

    float ssilvb_vis = 1.0f;
    if ((fcb.flags & (1u << 2u)) != 0u && g_motion_srv != 0xFFFFFFFFu) {
        Texture2D<float4> ssilvb = ResourceDescriptorHeap[atelier_safe_desc(g_motion_srv)];
        ssilvb_vis = saturate(ssilvb.Load(int3(px, 0)).w);
    }
    // Lagarde & de Rousiers [SIGGRAPH 2014] Specular AO without double-occluding ray-traced reflections or clipping grazing rays:
    float rough_cut_c = clamp(fcb.reflection_roughness, 0.0f, 0.80f);
    float traced_w = (((fcb.flags & (1u << 3u)) != 0u) && refl_r >= 0.0f)
                         ? (1.0f - smoothstep(max(0.16f, rough_cut_c - 0.12f), max(0.17f, rough_cut_c), refl_r))
                         : 0.0f;
    float macro_sao_vis = ((fcb.flags & 2u) != 0u) ? lerp(1.0f, ssilvb_vis, 0.65f) : ssilvb_vis;
    float ao_spec = lerp(min(surf.micro_ao, macro_sao_vis), surf.micro_ao, traced_w);
    float spec_ao = saturate(pow(max(nv + ao_spec, 1e-4f), exp2(-16.0f * surf.roughness - 1.0f)) - 1.0f + ao_spec);

    Texture2D<float2> ltc_amp_tex = ResourceDescriptorHeap[atelier_safe_desc(fcb.ltc_amp_srv)];
    SamplerState smp = SamplerDescriptorHeap[0];
    float2 ltc_uv = float2(surf.roughness, sqrt(max(0.0f, 1.0f - nv))) * (63.0f / 64.0f) + (0.5f / 64.0f);
    float2 brdf_ab = ltc_amp_tex.SampleLevel(smp, ltc_uv, 0.0f);
    float3 f0 = lerp(surf.spec_f0_dielectric.xxx, surf.base_color, surf.metallic);
    if (surf.profile_id == SLAB_PROFILE_IRIDESCENT && surf.lobe_weight > 0.0f)
        f0 = lerp(f0, eval_compose_iridescence(nv, surf.lobe_param0, surf.lobe_param1, f0), surf.lobe_weight);
    float3 env_spec_brdf = f0 * max(brdf_ab.x - brdf_ab.y, 0.0f) + brdf_ab.y;
    if (surf.profile_id == SLAB_PROFILE_STANDARD_F82 && surf.metallic > 0.0f && any(surf.f82_or_lobe_color < 0.999f)) {
        float3 f82 = lerp(1.0f.xxx, surf.f82_or_lobe_color, surf.metallic);
        float x = saturate(1.0f - nv);
        float x5 = x * x * x * x * x;
        float3 f_sch = f0 + (1.0f - f0) * x5;
        const float mu_bar = 1.0f / 7.0f;
        const float om_bar = 6.0f / 7.0f;
        const float om_bar5 = om_bar * om_bar * om_bar * om_bar * om_bar;
        float3 f_bar = f0 + (1.0f - f0) * om_bar5;
        float3 b_f82 = (f_bar * (1.0f - f82)) / max(mu_bar * om_bar5 * om_bar, 1e-5f);
        float3 f_laz = saturate(f_sch - b_f82 * nv * x5 * x);
        env_spec_brdf *= f_laz / max(f_sch, 1e-4f);
    }
    // Turquin [2019] white-furnace-preserving multiple-scattering microfacet GGX energy compensation:
    float e_ss = clamp(brdf_ab.x, 0.05f, 1.0f);
    env_spec_brdf = saturate(env_spec_brdf * (1.0f + f0 * ((1.0f - e_ss) / e_ss)));

    float3 diffuse_albedo = surf.base_color * (1.0f - surf.metallic);
    if (surf.profile_id == SLAB_PROFILE_FOLIAGE)
        diffuse_albedo *= (1.0f - 0.5f * surf.lobe_weight) + 0.5f * surf.lobe_weight * surf.f82_or_lobe_color;

    // Jimenez et al. [SIGGRAPH 2016] analytic multi-bounce albedo-preserving indirect occlusion (`GTAOMultiBounce`):
    float raw_diff_vis = gi_ao_vis * surf.micro_ao;
    float3 diff_ao_mb = raw_diff_vis.xxx;
    if (raw_diff_vis < 1.0f) {
        float xv = saturate(raw_diff_vis);
        float3 mb_a =  2.0404f * diffuse_albedo - 0.3324f;
        float3 mb_b = -4.7951f * diffuse_albedo + 0.6417f;
        float3 mb_c =  2.7552f * diffuse_albedo + 0.6903f;
        diff_ao_mb = max(xv.xxx, ((xv * mb_a + mb_b) * xv + mb_c) * xv);
    }

    float3 indirect_diff = diffuse_albedo * saturate(1.0f - env_spec_brdf) * gi_irrad * diff_ao_mb;
    float3 indirect_spec = refl_rad * env_spec_brdf * spec_ao;

    if (surf.profile_id == SLAB_PROFILE_COAT && surf.lobe_weight > 0.0f) {
        float2 coat_uv = float2(surf.lobe_param0, sqrt(max(0.0f, 1.0f - nv))) * (63.0f / 64.0f) + (0.5f / 64.0f);
        float2 coat_ab = ltc_amp_tex.SampleLevel(smp, coat_uv, 0.0f);
        float coat_brdf = 0.04f * max(coat_ab.x - coat_ab.y, 0.0f) + coat_ab.y;
        float coat_atten = saturate(1.0f - surf.lobe_weight * coat_brdf);
        float3 coat_sub = eval_openpbr_coat_darkening(surf.base_color, surf.roughness, surf.lobe_weight, surf.lobe_param1, surf.f82_or_lobe_color);
        float coat_spec_ao = saturate(pow(max(nv + ao_spec, 1e-4f), exp2(-16.0f * surf.lobe_param0 - 1.0f)) - 1.0f + ao_spec);
        float3 R_base = reflect(-V, surf.shading_normal);
        float3 base_env = ((fcb.flags & 2u) != 0u) ? gi_irrad : (eval_atelier_sky(R_base, surf.roughness * 6.0f, fcb) * ssilvb_vis);
        indirect_diff *= coat_atten * coat_sub;
        indirect_spec = base_env * env_spec_brdf * (coat_atten * spec_ao) * coat_sub +
                        refl_rad * (surf.lobe_weight * coat_brdf * coat_spec_ao);
    } else if (surf.profile_id == SLAB_PROFILE_FUZZ && surf.lobe_weight > 0.0f) {
        Texture2D<float> charlie_lut = ResourceDescriptorHeap[atelier_safe_desc(fcb.charlie_lut_srv)];
        float2 ch_uv = float2(saturate(max(surf.lobe_param0, 0.07f)), saturate(nv)) * (31.0f / 32.0f) + (0.5f / 32.0f);
        float e_fuzz = charlie_lut.SampleLevel(smp, ch_uv, 0.0f);
        float fuzz_atten = saturate(1.0f - surf.lobe_weight * e_fuzz);
        float3 fuzz_ao_mb = raw_diff_vis.xxx;
        if (raw_diff_vis < 1.0f) {
            float xv = saturate(raw_diff_vis);
            float3 f_alb = surf.f82_or_lobe_color;
            fuzz_ao_mb = max(xv.xxx, ((xv * (2.0404f * f_alb - 0.3324f) + (-4.7951f * f_alb + 0.6417f)) * xv + (2.7552f * f_alb + 0.6903f)) * xv);
        }
        indirect_diff = indirect_diff * fuzz_atten +
                        (surf.lobe_weight * e_fuzz) * surf.f82_or_lobe_color * gi_irrad * fuzz_ao_mb;
        indirect_spec *= fuzz_atten;
    }

    float3 emissive = 0.0f;
    uint tiles_x = (fcb.render_extent.x + 7u) / 8u;
    uint tile_idx = (px.y / 8u) * tiles_x + (px.x / 8u);
    StructuredBuffer<uint> tile_em_mask = ResourceDescriptorHeap[atelier_safe_desc(g_tile_emissive_srv)];
    if (((tile_em_mask[tile_idx >> 5u] >> (tile_idx & 31u)) & 1u) != 0u) {
        Texture2D<float4> slab2_tex = ResourceDescriptorHeap[atelier_safe_desc(g_slab2_srv)];
        emissive = slab2_tex.Load(int3(px, 0)).rgb;
    }

    float3 total_hdr = direct_diff + direct_spec + indirect_diff * fcb.indirect_gain + indirect_spec + emissive;
    if (fcb.fog_density > 0.0f && fcb.froxel_srv != 0xFFFFFFFFu && abs(eye_pos.z) > 0.1f) {
        Texture3D<float4> froxel_lut = ResourceDescriptorHeap[atelier_safe_desc(fcb.froxel_srv)];
        float2 vp_uv = (pxc - fcb.scene_viewport.xy) / max(fcb.scene_viewport.zw, 1.0f);
        float fw = (froxel_vz_to_slice(abs(eye_pos.z)) * 63.0f + 0.5f) / 64.0f;
        float4 fog = froxel_lut.SampleLevel(smp, float3(vp_uv, fw), 0.0f);
        total_hdr = total_hdr * fog.a + fog.rgb;
    }
    hdr_uav[px] = float4(clamp(total_hdr, 0.0f, 65504.0f), 1.0f);

    if ((px.x & 1u) == 0u && (px.y & 1u) == 0u && g_uav1 != 0xFFFFFFFFu) {
        RWTexture2D<float4> prev_diff_uav = ResourceDescriptorHeap[atelier_safe_desc(g_uav1)];
        float3 diff_emit = clamp(direct_diff + indirect_diff + emissive, 0.0f, 65000.0f);
        prev_diff_uav[px >> 1u] = float4(diff_emit, clamp(abs(eye_pos.z), 0.01f, 65000.0f));
    }
}
