// 3-Layer Indoor/Outdoor Global Illumination System (Anchored to AMD Capsaicin v1.3 GI-1.2 + NVIDIA SHaRC v1.8.3 + Therrien 2023 / XeGTAO):
//   Mode 0:  Layer 0 — Full-Res Signed 32-Sector ONB-GT-VBAO Bitmask (`V_v`-Pole `GTAOFastAcos` + Unwrapped `- n` Shift + Sub-Probe `ShortRangeAO`)
//   Mode 6:  Layer 0 — Full-Res 3x3 Tangent-Plane Bilateral Filter + 1:1 Subpixel Temporal Accumulation (`ssilvb_ao_history` + `ssilvb_gi_history`)
//   Mode 1:  Clear GI counters, SHaRC per-frame accumulators (`wrc_accum`), & cold-start SHaRC hash keys
//   Mode 2:  Layer 1 — `ScreenProbes`: `ReprojectScreenProbes` (3D Parallax + Rotational Pinhole Fill) ->
//                      `SampleScreenProbes` (Defensive 75% Radiance*Cosine + 25% Uniform CDF) -> Exact Hi-Z -> LDS Compacted DXR
//   Mode 3:  Prefix-sum & Scatter GI Hit Archetype Bins for `SecondaryHitShade CS`
//   Mode 4:  Layer 2 — Resolve SHaRC + GI-1.2 Multi-Bounce Surface Cache (Async Compute: Anti-Firefly + Fade Accel + Adjacent-Level + Eviction)
//   Mode 7:  Layer 1 — `ResolveCells` (SHaRC Slot[0] Handoff + Unbiased Empty-Cell Retention) & `BlendScreenProbes`
//   Mode 8:  Layer 1 — `PopulateProbeMaskMip` (Hierarchical Valid-Seed Mipchain for `FindClosestProbe`)
//   Mode 9:  Layer 1 — `FilterProbes` (Separable 7x1 + 1x7 Hit-Parallax & Angle-Backoff Coplanar Probe Filter)
//   Mode 10: Layer 1 — `ProjectScreenProbes` (Exact Discrete 8x8 White-Furnace Orthonormal SH2 Projection)
//   Mode 5:  `InterpolateScreenProbes` (±6px Coplanar Blue-Noise Jitter + Hierarchical `FindClosestProbe`) +
//            `ReprojectGI` (3D Tangent-Plane / PrevGeomNormal Subpixel Gather + `color_delta` Adaptive Temporal Accumulation)
//   Mode 11: `FilterGI` (Separable 9-Tap Shading-Normal & 3D Tangent-Plane Denoiser + Cold Bootstrap + Unbiased Bent-Cone Ratio)

cbuffer GiRootConstants : register(b0, space0) {
    uint g_frame_cb_srv;
    uint g_gi_mode;
    uint g_depth_srv;
    uint g_hzb_srv;
    uint g_geom_normal_srv;
    uint g_slab0_srv;
    uint g_slab1_srv;
    uint g_motion_srv;
    uint g_prev_diffuse_srv;
    uint g_uav0;
    uint g_uav1;
    uint g_uav2;
    uint g_uav3;
    uint g_srv_aux0;
    uint g_srv_aux1;
    uint g_srv_aux2;
};

struct GpuGiRayHit {
    uint probe_and_ray;
    uint instance_id;
    uint global_tri_id;
    uint packed_bary;
    float3 ray_origin;
    float hit_t;
    float3 ray_dir;
    uint surf_idx;
};

float3 gi_reconstruct_world(float2 pxc, float dz, GlobalFrameConstants fcb) {
    float2 ndc = float2((pxc.x - fcb.scene_viewport.x) / max(fcb.scene_viewport.z, 1.0f) * 2.0f - 1.0f,
                        1.0f - (pxc.y - fcb.scene_viewport.y) / max(fcb.scene_viewport.w, 1.0f) * 2.0f);
    precise float4 wp = mul(float4(ndc, dz, 1.0f), fcb.inv_view_projection);
    float ww = abs(wp.w) > 1e-7f ? wp.w : (wp.w >= 0.0f ? 1e-7f : -1e-7f);
    return wp.xyz / ww;
}

row_major float4x4 gi_invert_4x4(row_major float4x4 m, row_major float4x4 fallback_inv) {
    float s0 = m[0][0] * m[1][1] - m[1][0] * m[0][1];
    float s1 = m[0][0] * m[1][2] - m[1][0] * m[0][2];
    float s2 = m[0][0] * m[1][3] - m[1][0] * m[0][3];
    float s3 = m[0][1] * m[1][2] - m[1][1] * m[0][2];
    float s4 = m[0][1] * m[1][3] - m[1][1] * m[0][3];
    float s5 = m[0][2] * m[1][3] - m[1][2] * m[0][3];

    float c0 = m[2][0] * m[3][1] - m[3][0] * m[2][1];
    float c1 = m[2][0] * m[3][2] - m[3][0] * m[2][2];
    float c2 = m[2][0] * m[3][3] - m[3][0] * m[2][3];
    float c3 = m[2][1] * m[3][2] - m[3][1] * m[2][2];
    float c4 = m[2][1] * m[3][3] - m[3][1] * m[2][3];
    float c5 = m[2][2] * m[3][3] - m[3][2] * m[2][3];

    float det = s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
    if (abs(det) < 1e-20f)
        return fallback_inv;

    float inv_det = 1.0f / det;
    row_major float4x4 inv;
    inv[0] = float4( m[1][1] * c5 - m[1][2] * c4 + m[1][3] * c3,
                    -m[0][1] * c5 + m[0][2] * c4 - m[0][3] * c3,
                     m[3][1] * s5 - m[3][2] * s4 + m[3][3] * s3,
                    -m[2][1] * s5 + m[2][2] * s4 - m[2][3] * s3) * inv_det;
    inv[1] = float4(-m[1][0] * c5 + m[1][2] * c2 - m[1][3] * c1,
                     m[0][0] * c5 - m[0][2] * c2 + m[0][3] * c1,
                    -m[3][0] * s5 + m[3][2] * s2 - m[3][3] * s1,
                     m[2][0] * s5 - m[2][2] * s2 + m[2][3] * s1) * inv_det;
    inv[2] = float4( m[1][0] * c4 - m[1][1] * c2 + m[1][3] * c0,
                    -m[0][0] * c4 + m[0][1] * c2 - m[0][3] * c0,
                     m[3][0] * s4 - m[3][1] * s2 + m[3][3] * s0,
                    -m[2][0] * s4 + m[2][1] * s2 - m[2][3] * s0) * inv_det;
    inv[3] = float4(-m[1][0] * c3 + m[1][1] * c1 - m[1][2] * c0,
                     m[0][0] * c3 - m[0][1] * c1 + m[0][2] * c0,
                    -m[3][0] * s3 + m[3][1] * s1 - m[3][2] * s0,
                     m[2][0] * s3 - m[2][1] * s1 + m[2][2] * s0) * inv_det;
    return inv;
}

float3 gi_reconstruct_prev_world(float2 prev_pxc, float prev_dz, row_major float4x4 inv_prev_vp, GlobalFrameConstants fcb) {
    float2 ndc = float2((prev_pxc.x - fcb.prev_scene_viewport.x) / max(fcb.prev_scene_viewport.z, 1.0f) * 2.0f - 1.0f,
                        1.0f - (prev_pxc.y - fcb.prev_scene_viewport.y) / max(fcb.prev_scene_viewport.w, 1.0f) * 2.0f);
    precise float4 wp = mul(float4(ndc, prev_dz, 1.0f), inv_prev_vp);
    float ww = abs(wp.w) > 1e-7f ? wp.w : (wp.w >= 0.0f ? 1e-7f : -1e-7f);
    return wp.xyz / ww;
}

bool gi_px_in_viewport(uint2 px, float4 vp) {
    float2 pxc = float2(px) + 0.5f;
    return pxc.x >= vp.x && pxc.x < vp.x + vp.z && pxc.y >= vp.y && pxc.y < vp.y + vp.w;
}

bool find_screen_probe_anchor(uint2 probe_coord, Texture2D<float> depth_tex, uint2 render_extent, float4 scene_vp,
                              out uint2 anchor_px, out float dz) {
    anchor_px = min(probe_coord * 8u + uint2(4u, 4u), render_extent - 1u);
    dz = gi_px_in_viewport(anchor_px, scene_vp) ? depth_tex.Load(int3(anchor_px, 0)) : 0.0f;
    if (dz <= 0.0f) {
        [unroll] for (uint k = 0u; k < 4u; ++k) {
            uint2 cand = min(probe_coord * 8u + uint2((k & 1u) * 6u + 1u, (k >> 1u) * 6u + 1u), render_extent - 1u);
            if (!gi_px_in_viewport(cand, scene_vp))
                continue;
            float cd = depth_tex.Load(int3(cand, 0));
            if (cd > dz) {
                dz = cd;
                anchor_px = cand;
            }
        }
    }
    return dz > 0.0f;
}

// Hierarchical mip search anchored to AMD Capsaicin GI-1.2 `ScreenProbes_FindClosestProbe` (`screen_probes.hlsl:140-185`):
uint find_closest_probe_seed(uint2 base_probe, int2 offset, Texture2D<uint> mask_tex, uint2 probes_dim, uint mip_count) {
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

uint pack_fp16x2(float a, float b) {
    uint lo = f32tof16(clamp(a, -32768.0f, 32768.0f)) & 0xFFFFu;
    uint hi = f32tof16(clamp(b, -32768.0f, 32768.0f)) & 0xFFFFu;
    return lo | (hi << 16u);
}

float2 unpack_fp16x2(uint p) {
    return float2(f16tof32(p & 0xFFFFu), f16tof32(p >> 16u));
}

struct PackedScreenProbeSH2 {
    uint4 u0;
    uint4 u1;
    uint4 u2;
    uint4 u3;
};

void pack_probe_sh2_record(float3 sh[9], float avg_vis, float3 gn, uint valid_flag, out PackedScreenProbeSH2 o) {
    o.u0 = uint4(pack_fp16x2(sh[0].r, sh[0].g), pack_fp16x2(sh[0].b, sh[1].r),
                 pack_fp16x2(sh[1].g, sh[1].b), pack_fp16x2(sh[2].r, sh[2].g));
    o.u1 = uint4(pack_fp16x2(sh[2].b, sh[3].r), pack_fp16x2(sh[3].g, sh[3].b),
                 pack_fp16x2(sh[4].r, sh[4].g), pack_fp16x2(sh[4].b, sh[5].r));
    o.u2 = uint4(pack_fp16x2(sh[5].g, sh[5].b), pack_fp16x2(sh[6].r, sh[6].g),
                 pack_fp16x2(sh[6].b, sh[7].r), pack_fp16x2(sh[7].g, sh[7].b));
    o.u3.x = pack_fp16x2(sh[8].r, sh[8].g);
    o.u3.y = pack_fp16x2(sh[8].b, saturate(avg_vis));
    o.u3.z = pack_oct_snorm16(gn);
    o.u3.w = valid_flag;
}

void unpack_probe_sh2_record(PackedScreenProbeSH2 p, out float3 sh[9], out float avg_vis, out float3 gn, out uint valid_flag) {
    float2 v00 = unpack_fp16x2(p.u0.x), v01 = unpack_fp16x2(p.u0.y), v02 = unpack_fp16x2(p.u0.z), v03 = unpack_fp16x2(p.u0.w);
    float2 v10 = unpack_fp16x2(p.u1.x), v11 = unpack_fp16x2(p.u1.y), v12 = unpack_fp16x2(p.u1.z), v13 = unpack_fp16x2(p.u1.w);
    float2 v20 = unpack_fp16x2(p.u2.x), v21 = unpack_fp16x2(p.u2.y), v22 = unpack_fp16x2(p.u2.z), v23 = unpack_fp16x2(p.u2.w);
    float2 v30 = unpack_fp16x2(p.u3.x), v31 = unpack_fp16x2(p.u3.y);
    sh[0] = float3(v00.x, v00.y, v01.x);
    sh[1] = float3(v01.y, v02.x, v02.y);
    sh[2] = float3(v03.x, v03.y, v10.x);
    sh[3] = float3(v10.y, v11.x, v11.y);
    sh[4] = float3(v12.x, v12.y, v13.x);
    sh[5] = float3(v13.y, v20.x, v20.y);
    sh[6] = float3(v21.x, v21.y, v22.x);
    sh[7] = float3(v22.y, v23.x, v23.y);
    sh[8] = float3(v30.x, v30.y, v31.x);
    avg_vis = v31.y;
    gn = unpack_oct_snorm16(p.u3.z);
    valid_flag = p.u3.w;
}

float3 eval_probe_sh2_irradiance(float3 sh[9], float3 n) {
    float cc[9];
    sh2_get_clamped_cosine_over_pi(n, cc);
    float3 res = 0.0f;
    [unroll] for (uint i = 0u; i < 9u; ++i)
        res += sh[i] * cc[i];
    return max(res, 0.0f);
}

// Eberly [2014] / Intel XeGTAO fast acos(x) on [-1, 1] -> [0, pi] (max error < 0.0068 rad, exact at ±1, branch-cut-free):
float gtvbao_fast_acos(float x) {
    float ax = abs(clamp(x, -1.0f, 1.0f));
    float res = (-0.156583f * ax + 1.5707963f) * sqrt(max(1.0f - ax, 0.0f));
    return (x >= 0.0f) ? res : (ATELIER_PI - res);
}

// UE5 Lumen `ShortRangeAO` sub-probe radius coupling: when `ScreenProbes` GI is active, restrict `GT-VBAO` to the sub-probe
// residual window (`~4.5x ProbeSpacing`) so macro arches/pillars aren't double-darkened; use full `ao_radius` when GI is off:
float gtvbao_effective_radius(float user_ao_radius, float probe_spacing_ws, bool gi_probes_on) {
    float r = clamp(user_ao_radius, 0.02f, 10.0f);
    if (!gi_probes_on)
        return r;
    float short_range_cap = clamp(probe_spacing_ws * 4.5f, 0.14f, 0.30f);
    return min(r, short_range_cap);
}

// Compact shared LDS (~3.7 KiB total -> 12..16 concurrent groups per CU/SM):
groupshared uint4 gs_reproj_accum[64];
groupshared float4 gs_vec4_a[64];
groupshared float3 gs_vec3_dir[64];
groupshared float gs_cdf[64];
groupshared uint gs_comp_dir[64];
groupshared uint gs_comp_slot[64];
groupshared uint gs_comp_mask[2];
groupshared uint gs_probe_flags;
groupshared uint2 gs_best_ppc;
groupshared float3 gs_anchor_P;
groupshared float3 gs_anchor_N;
groupshared float3 gs_prev_P;
groupshared float3 gs_prev_N;
groupshared float3 gs_sh_out[9];
groupshared float gs_nbr_w[6];
groupshared float3 gs_nbr_P[6];
groupshared float3 gs_nbr_N[6];
groupshared uint2 gs_nbr_pc[6];
groupshared row_major float4x4 gs_inv_prev_vp;

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID, uint gindex : SV_GroupIndex, uint3 gid : SV_GroupID) {
    StructuredBuffer<GlobalFrameConstants> fcb_buf = ResourceDescriptorHeap[atelier_safe_desc(g_frame_cb_srv)];
    GlobalFrameConstants fcb = fcb_buf[0];

    uint2 tile8_off = uint2(fcb.scene_tile8_offset_packed & 0xFFFFu, fcb.scene_tile8_offset_packed >> 16u);
    uint2 tile8_px_off = tile8_off * 8u;

    // ========================================================================
    // Mode 0: Layer 0 — Full-Res Signed 32-Sector ONB-GT-VBAO (Therrien 2023 + XeGTAO + Lumen ShortRangeAO)
    // ========================================================================
    if (g_gi_mode == 0u) {
        uint2 fpx = dtid.xy + tile8_px_off;
        if (fpx.x >= fcb.render_extent.x || fpx.y >= fcb.render_extent.y)
            return;

        RWTexture2D<float4> bent_raw_uav = ResourceDescriptorHeap[atelier_safe_desc(g_uav0)];
        RWTexture2D<float4> gi_raw_uav = ResourceDescriptorHeap[atelier_safe_desc(g_uav1)];
        Texture2D<float> depth_tex = ResourceDescriptorHeap[atelier_safe_desc(g_depth_srv)];
        float dz = gi_px_in_viewport(fpx, fcb.scene_viewport) ? depth_tex.Load(int3(fpx, 0)) : 0.0f;
        if (dz <= 0.0f || (fcb.flags & (1u << 2u)) == 0u) {
            bent_raw_uav[fpx] = float4(0.0f, 1.0f, 0.0f, 1.0f);
            gi_raw_uav[fpx] = 0.0f;
            return;
        }

        Texture2D<uint> gn_tex = ResourceDescriptorHeap[atelier_safe_desc(g_geom_normal_srv)];
        float3 N_w = unpack_oct_snorm16(gn_tex.Load(int3(fpx, 0)));
        float3 N_v = safe_unit_normal(mul(N_w, (float3x3)fcb.view));
        float3 P_w = gi_reconstruct_world(float2(fpx) + 0.5f, dz, fcb);
        float3 P_v = mul(float4(P_w, 1.0f), fcb.view).xyz;
        bool is_persp = (abs(fcb.projection[2][3]) > 1e-6f);
        float3 V_v = is_persp ? safe_unit_normal(-P_v) : float3(0.0f, 0.0f, -1.0f);
        float nv_view = dot(N_v, V_v);
        if (nv_view < 1e-3f)
            N_v = safe_unit_normal(N_v + V_v * (1e-3f - nv_view));

        float clip_w = max(abs(P_v.z * fcb.projection[2][3] + fcb.projection[3][3]), 0.1f);
        float proj_scale_w = max(abs(fcb.projection[0][0]) * fcb.scene_viewport.z, 1.0f);
        float proj_scale_h = max(abs(fcb.projection[1][1]) * fcb.scene_viewport.w, 1.0f);
        float probe_spacing_ws = 8.0f * (2.0f * clip_w) / proj_scale_h;
        bool gi_probes_on = (fcb.flags & 2u) != 0u;
        float radius = gtvbao_effective_radius(fcb.ao_radius, probe_spacing_ws, gi_probes_on);
        float max_px_r = gi_probes_on ? 48.0f : 96.0f;
        float px_radius_full = clamp((radius * 0.5f * proj_scale_h) / clip_w, 2.5f, max_px_r);
        float min_s = min(1.15f / px_radius_full, 0.35f);
        float thick_near = clamp(abs(P_v.z) * 0.004f, 0.008f, 0.035f);
        float inv_pz = 1.0f / max(abs(P_v.z), 1e-4f);

        // Exact orthonormal basis around view-pole `V_v` (eliminates off-axis screen-XY azimuthal distortion):
        float3 B1_v, B2_v;
        atelier_build_onb(V_v, B1_v, B2_v);

        uint hzb_w0 = 1u, hzb_h0 = 1u, hzb_mips = 0u;
        Texture2D<float2> hzb_tex = ResourceDescriptorHeap[atelier_safe_desc(g_hzb_srv)];
        if (g_hzb_srv != 0xFFFFFFFFu)
            hzb_tex.GetDimensions(0u, hzb_w0, hzb_h0, hzb_mips);

        uint temporal_phase = ((fcb.flags & (1u << 7u)) != 0u) ? fcb.frame_index : 0u;
        float2 j_ao = atelier_blue_jitter2(fpx, temporal_phase, 0x7F4A7C15u);
        float noise = j_ao.x;
        float noise_step = j_ao.y;

        // Warm temporal history (`g_srv_aux2 != 0`): 1 slice x 4 steps/side (8 taps/px, 96 effective taps over 12 frames);
        // Cold start (`g_srv_aux2 == 0`): 2 slices x 4 steps/side (16 taps/px) for clean single-frame captures!
        uint slice_count = (g_srv_aux2 != 0u) ? 1u : 2u;
        float inv_slices = 1.0f / float(slice_count);

        float vis_num = 0.0f;
        float vis_den = 0.0f;
        float3 bent_accum_v = 0.0f;
        float3 micro_gi = 0.0f;
        SamplerState smp_near = SamplerDescriptorHeap[3]; // nearest_clamp

        [loop] for (uint s = 0u; s < slice_count; ++s) {
            float phi = (float(s) + noise) * (inv_slices * ATELIER_PI);
            float c_phi = cos(phi);
            float s_phi = sin(phi);
            float3 T_v = B1_v * c_phi + B2_v * s_phi;
            float3 plane_n = cross(T_v, V_v);
            float3 N_proj = N_v - plane_n * dot(N_v, plane_n);
            float proj_len2 = dot(N_proj, N_proj);
            if (proj_len2 <= 1e-6f)
                continue;
            float proj_len = sqrt(proj_len2);
            float3 N_slice = N_proj / proj_len;
            float3 T_slice = cross(N_slice, plane_n);
            float cos_n = clamp(dot(N_slice, V_v), -1.0f, 1.0f);
            float sin_n = dot(N_slice, T_v);
            float n_ang = ((sin_n >= 0.0f) ? 1.0f : -1.0f) * gtvbao_fast_acos(cos_n);

            float2 d_scr;
            if (is_persp) {
                d_scr = float2((T_v.x - P_v.x * inv_pz * T_v.z) * proj_scale_w,
                               -(T_v.y - P_v.y * inv_pz * T_v.z) * proj_scale_h);
            } else {
                d_scr = float2(T_v.x * proj_scale_w, -T_v.y * proj_scale_h);
            }
            float2 omega = (dot(d_scr, d_scr) > 1e-12f) ? normalize(d_scr) : float2(c_phi, -s_phi);

            uint bitmask = 0u;
            [loop] for (int side = -1; side <= 1; side += 2) {
                [loop] for (uint st = 0u; st < 4u; ++st) {
                    float step_jit = frac(noise_step + float(s + st * 2u + uint(side > 0)) * 0.6180339887f);
                    float u_s = (float(st) + step_jit) * 0.25f;
                    float frac_r = pow(u_s, 1.75f) * (1.0f - min_s) + min_s;
                    float step_px_full = frac_r * px_radius_full;
                    float2 sp_full = float2(fpx) + 0.5f + omega * (float(side) * step_px_full);
                    if (sp_full.x < fcb.scene_viewport.x || sp_full.x >= fcb.scene_viewport.x + fcb.scene_viewport.z ||
                        sp_full.y < fcb.scene_viewport.y || sp_full.y >= fcb.scene_viewport.y + fcb.scene_viewport.w)
                        continue;

                    int2 qpx = clamp(int2(sp_full), fcb.scissor_rect.xy, max(fcb.scissor_rect.xy, fcb.scissor_rect.zw - 1));
                    float qd = 0.0f;
                    float2 q_center = float2(qpx) + 0.5f;
                    if (g_hzb_srv != 0xFFFFFFFFu && hzb_mips > 0u && step_px_full >= 8.0f) {
                        int mip = clamp(int(floor(log2(step_px_full))) - 3, 0, min(int(hzb_mips) - 1, 2));
                        uint2 mdim = max(uint2(1u, 1u), uint2(hzb_w0, hzb_h0) >> uint(mip));
                        int2 hc = clamp(qpx >> (mip + 1), int2(0, 0), int2(mdim) - 1);
                        qd = hzb_tex.Load(int3(hc, mip)).y;
                    } else {
                        qd = depth_tex.Load(int3(qpx, 0));
                    }
                    if (qd <= 0.0f)
                        continue;

                    float3 Q_w = gi_reconstruct_world(q_center, qd, fcb);
                    float3 diff_f = mul(float4(Q_w, 1.0f), fcb.view).xyz - P_v;
                    float dist = length(diff_f);
                    float falloff = saturate((radius * 1.18f - dist) / max(radius * 0.35f, 1e-3f));
                    if (dist < 1e-4f || falloff <= 0.0f)
                        continue;

                    // Therrien et al. [2023] (`arXiv:2301.11376`) exact `V_v`-pole slice projection:
                    // Because `-V_v * thickness` is strictly orthogonal to `T_v` (`dot(V_v, T_v) == 0`), both front and
                    // back points share identical `x_tv` and lie monotonically in `(0, pi)` on the same side of `V_v`!
                    // Subtracting `n_ang` on the real line (without modulo-2pi `atan2` wrap) eliminates grazing black blotches!
                    float thickness = max(thick_near, min(dist * 0.38f, radius * 0.52f)) * falloff;
                    float x_tv = dot(diff_f, T_v);
                    float y_vf = dot(diff_f, V_v);
                    float y_vb = y_vf - thickness;
                    float len_f2 = x_tv * x_tv + y_vf * y_vf;
                    float len_b2 = x_tv * x_tv + y_vb * y_vb;
                    if (len_f2 <= 1e-10f || len_b2 <= 1e-10f)
                        continue;

                    float sgn_x = (x_tv >= 0.0f) ? 1.0f : -1.0f;
                    float a_vf = sgn_x * gtvbao_fast_acos(y_vf * rsqrt(len_f2));
                    float a_vb = sgn_x * gtvbao_fast_acos(y_vb * rsqrt(len_b2));
                    float a_lo = min(a_vf, a_vb) - n_ang;
                    float a_hi = max(a_vf, a_vb) - n_ang;

                    // Small 0.025 rad (~1.4 deg) horizon grazing tolerance prevents float32/HZB flat-wall self-acne:
                    float u0 = saturate((a_lo + 0.5f * ATELIER_PI) * ATELIER_INV_PI);
                    float u1 = saturate((a_hi + 0.5f * ATELIER_PI) * ATELIER_INV_PI);
                    if (u0 <= 0.008f)
                        u1 = max(0.0f, u1 - 0.008f);
                    if (u1 >= 0.992f)
                        u0 = min(1.0f, u0 + 0.008f);

                    // Unbiased midpoint sector rounding matching `k_gtvbao_sector_cs[b]` (`(b + 0.5) / 32`):
                    uint b0 = min(uint(floor(u0 * 32.0f + 0.5f)), 32u);
                    uint b1 = min(uint(floor(u1 * 32.0f + 0.5f)), 32u);
                    if (b1 <= b0 || b0 >= 32u)
                        continue;
                    uint count_b = b1 - b0;
                    uint new_bits = (count_b >= 32u) ? 0xFFFFFFFFu : (((1u << count_b) - 1u) << b0);
                    uint newly_occ = new_bits & ~bitmask;
                    bitmask |= new_bits;

                    float micro_falloff = saturate(1.0f - dist / max(probe_spacing_ws * 2.0f, 0.08f));
                    if (newly_occ != 0u && micro_falloff > 0.0f && (fcb.flags & (1u << 7u)) != 0u && g_prev_diffuse_srv != 0xFFFFFFFFu) {
                        Texture2D<float4> prev_diff = ResourceDescriptorHeap[atelier_safe_desc(g_prev_diffuse_srv)];
                        float2 prev_px_m = q_center;
                        if (g_motion_srv != 0xFFFFFFFFu) {
                            Texture2D<float2> mot_t = ResourceDescriptorHeap[atelier_safe_desc(g_motion_srv)];
                            prev_px_m += mot_t.Load(int3(qpx, 0)) * float2(fcb.render_extent);
                        }
                        if (prev_px_m.x >= fcb.prev_scene_viewport.x &&
                            prev_px_m.x < fcb.prev_scene_viewport.x + fcb.prev_scene_viewport.z &&
                            prev_px_m.y >= fcb.prev_scene_viewport.y &&
                            prev_px_m.y < fcb.prev_scene_viewport.y + fcb.prev_scene_viewport.w) {
                            float2 pd_span = float2(((fcb.render_extent + 1u) >> 1u) << 1u);
                            float4 pd = prev_diff.SampleLevel(smp_near, prev_px_m / pd_span, 0.0f);
                            if (pd.a > 0.005f) {
                                float3 dir_f = diff_f / dist;
                                float w_hit = float(countbits(newly_occ)) * (1.0f / 32.0f) * saturate(dot(dir_f, N_v));
                                micro_gi += min(pd.rgb * fcb.exposure_ratio, 16.0f) * (w_hit * micro_falloff);
                            }
                        }
                    }
                }
            }

            // Trig-free exact spherical Jacobian `|sin(n + alpha_b)| * cos(alpha_b)` with self-normalizing capacity:
            uint open_bits = ~bitmask;
            float open_slice = 0.0f;
            float cap_slice = 0.0f;
            float wn = 0.0f;
            float wt = 0.0f;
            [loop] for (uint b = 0u; b < 32u; ++b) {
                float2 cs = k_gtvbao_sector_cs[b];
                float w_b = abs(sin_n * cs.x + cos_n * cs.y) * cs.x;
                cap_slice += w_b;
                if ((open_bits & (1u << b)) != 0u) {
                    open_slice += w_b;
                    wn += w_b * cs.x;
                    wt += w_b * cs.y;
                }
            }
            vis_num += proj_len * open_slice;
            vis_den += proj_len * cap_slice;
            bent_accum_v += (N_slice * wn + T_slice * wt) * proj_len;
        }

        float ao_vis = (vis_den > 1e-5f) ? saturate(vis_num / vis_den) : 1.0f;
        float3 bent_v = (dot(bent_accum_v, bent_accum_v) > 1e-6f) ? normalize(bent_accum_v) : N_v;
        float3 bent_w = safe_unit_normal(mul(bent_v, (float3x3)fcb.inv_view));
        bent_raw_uav[fpx] = float4(bent_w, ao_vis);
        gi_raw_uav[fpx] = float4(clamp(micro_gi * inv_slices, 0.0f, 65504.0f), 1.0f);
        return;
    }

    // ========================================================================
    // Mode 6: Layer 0 — Full-Res 3x3 Tangent-Plane Filter + 1:1 Grazing-Safe Temporal Accumulation
    // ========================================================================
    if (g_gi_mode == 6u) {
        if (gindex == 0u && ((fcb.flags & (1u << 7u)) != 0u) && g_slab0_srv != 0xFFFFFFFFu)
            gs_inv_prev_vp = gi_invert_4x4(fcb.prev_unjittered_vp, fcb.inv_unjittered_vp);
        GroupMemoryBarrierWithGroupSync();

        uint2 px = dtid.xy + tile8_px_off;
        if (px.x >= fcb.render_extent.x || px.y >= fcb.render_extent.y)
            return;

        RWTexture2D<float4> bent_out = ResourceDescriptorHeap[atelier_safe_desc(g_uav0)];
        RWTexture2D<float4> gi_out = ResourceDescriptorHeap[atelier_safe_desc(g_uav1)];
        Texture2D<float4> bent_in = ResourceDescriptorHeap[atelier_safe_desc(g_srv_aux0)];
        Texture2D<float4> gi_in = ResourceDescriptorHeap[atelier_safe_desc(g_srv_aux1)];
        Texture2D<float> depth_tex = ResourceDescriptorHeap[atelier_safe_desc(g_depth_srv)];
        float dz = gi_px_in_viewport(px, fcb.scene_viewport) ? depth_tex.Load(int3(px, 0)) : 0.0f;
        if (dz <= 0.0f || (fcb.flags & (1u << 2u)) == 0u) {
            bent_out[px] = float4(0.0f, 1.0f, 0.0f, 1.0f);
            gi_out[px] = 0.0f;
            return;
        }

        Texture2D<uint> gn_tex = ResourceDescriptorHeap[atelier_safe_desc(g_geom_normal_srv)];
        float3 N_w = unpack_oct_snorm16(gn_tex.Load(int3(px, 0)));
        float3 P_w = gi_reconstruct_world(float2(px) + 0.5f, dz, fcb);
        float px_ws = gi_pixel_world_size(P_w, fcb);
        float4 c_bent = bent_in.Load(int3(px, 0));
        float3 c_gi = gi_in.Load(int3(px, 0)).rgb;

        float3 sum_bent = c_bent.xyz;
        float sum_vis = c_bent.w;
        float vis_min = c_bent.w;
        float vis_max = c_bent.w;
        float3 sum_gi = c_gi;
        float w_sum = 1.0f;

        [loop] for (int dy = -1; dy <= 1; ++dy) {
            [loop] for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0)
                    continue;
                int2 q = clamp(int2(px) + int2(dx, dy), fcb.scissor_rect.xy, max(fcb.scissor_rect.xy, fcb.scissor_rect.zw - 1));
                if (!gi_px_in_viewport(uint2(q), fcb.scene_viewport))
                    continue;
                float qz = depth_tex.Load(int3(q, 0));
                if (qz <= 0.0f)
                    continue;
                float3 qN = unpack_oct_snorm16(gn_tex.Load(int3(q, 0)));
                float3 Q_w = gi_reconstruct_world(float2(q) + 0.5f, qz, fcb);
                float wz = exp(-abs(dot(Q_w - P_w, N_w)) / max(0.012f, px_ws * 2.2f));
                float wn = pow(saturate(dot(N_w, qN)), 12.0f);
                float kw = (dx == 0 || dy == 0) ? 0.5f : 0.25f;
                float w = wz * wn * kw;
                if (w <= 1e-4f)
                    continue;
                float4 sb = bent_in.Load(int3(q, 0));
                sum_bent += sb.xyz * w;
                sum_vis += sb.w * w;
                if (w >= 0.10f) {
                    vis_min = min(vis_min, sb.w);
                    vis_max = max(vis_max, sb.w);
                }
                sum_gi += gi_in.Load(int3(q, 0)).rgb * w;
                w_sum += w;
            }
        }

        float3 cur_bent = safe_unit_normal(sum_bent);
        float cur_vis = saturate(sum_vis / w_sum);
        float3 cur_gi = sum_gi / w_sum;
        float hist_cnt = 1.0f;

        // Grazing-safe 1:1 full-res 3D tangent-plane subpixel temporal accumulation (`ssilvb_ao_history` + `ssilvb_gi_history`):
        if ((fcb.flags & (1u << 7u)) != 0u && g_slab0_srv != 0xFFFFFFFFu && g_slab1_srv != 0xFFFFFFFFu &&
            g_prev_diffuse_srv != 0xFFFFFFFFu && g_hzb_srv != 0xFFFFFFFFu && g_motion_srv != 0xFFFFFFFFu) {
            Texture2D<float2> mot_tex = ResourceDescriptorHeap[atelier_safe_desc(g_motion_srv)];
            float2 mot = mot_tex.Load(int3(px, 0));
            float2 prev_fpx = (float2(px) + 0.5f - fcb.jitter_pixels) + mot * float2(fcb.render_extent);

            if (prev_fpx.x >= fcb.prev_scene_viewport.x && prev_fpx.x < fcb.prev_scene_viewport.x + fcb.prev_scene_viewport.z &&
                prev_fpx.y >= fcb.prev_scene_viewport.y && prev_fpx.y < fcb.prev_scene_viewport.y + fcb.prev_scene_viewport.w) {
                float4 cam_clip = mul(float4(P_w, 1.0f), fcb.prev_unjittered_vp);
                float2 cam_prev_px = prev_fpx;
                float cam_exp_dz = dz;
                if (abs(cam_clip.w) > 1e-5f) {
                    cam_prev_px = float2(fcb.prev_scene_viewport.x + (cam_clip.x / cam_clip.w * 0.5f + 0.5f) * fcb.prev_scene_viewport.z,
                                         fcb.prev_scene_viewport.y + (0.5f - cam_clip.y / cam_clip.w * 0.5f) * fcb.prev_scene_viewport.w);
                    cam_exp_dz = cam_clip.z / cam_clip.w;
                }
                bool is_dyn = length(cam_prev_px - prev_fpx) > 1.5f;
                float grazing = 1.0f / max(abs(dot(N_w, safe_unit_normal(fcb.camera_eye - P_w))), 0.08f);

                Texture2D<float> prev_d_tex = ResourceDescriptorHeap[atelier_safe_desc(g_prev_diffuse_srv)];
                Texture2D<uint> prev_gn_tex = ResourceDescriptorHeap[atelier_safe_desc(g_hzb_srv)];
                Texture2D<float4> prev_ao_tex = ResourceDescriptorHeap[atelier_safe_desc(g_slab0_srv)];
                Texture2D<float4> prev_gi_tex = ResourceDescriptorHeap[atelier_safe_desc(g_slab1_srv)];

                int2 p_base = int2(floor(prev_fpx - 0.5f));
                float2 p_frac = saturate((prev_fpx - 0.5f) - float2(p_base));

                float3 p_bent_sum = 0.0f;
                float p_vis_sum = 0.0f;
                float3 p_gi_sum = 0.0f;
                float p_cnt_sum = 0.0f;
                float p_w_sum = 0.0f;

                [loop] for (int py = 0; py <= 1; ++py) {
                    [loop] for (int px_o = 0; px_o <= 1; ++px_o) {
                        int2 qp = p_base + int2(px_o, py);
                        float2 qpc = float2(qp) + 0.5f;
                        if (qp.x < 0 || qp.y < 0 || qp.x >= int(fcb.render_extent.x) || qp.y >= int(fcb.render_extent.y) ||
                            qpc.x < fcb.prev_scene_viewport.x || qpc.x >= fcb.prev_scene_viewport.x + fcb.prev_scene_viewport.z ||
                            qpc.y < fcb.prev_scene_viewport.y || qpc.y >= fcb.prev_scene_viewport.y + fcb.prev_scene_viewport.w)
                            continue;
                        float pdz = prev_d_tex.Load(int3(qp, 0));
                        if (pdz <= 0.0f)
                            continue;
                        float4 pgi = prev_gi_tex.Load(int3(qp, 0));
                        if (pgi.w <= 0.0f)
                            continue;
                        float3 pGN = unpack_oct_snorm16(prev_gn_tex.Load(int3(qp, 0)));
                        if (dot(pGN, N_w) < 0.82f)
                            continue;

                        bool geom_ok = false;
                        if (!is_dyn) {
                            float3 prev_P = gi_reconstruct_prev_world(qpc, pdz, gs_inv_prev_vp, fcb);
                            float plane_err = abs(dot(prev_P - P_w, N_w));
                            float rad_err = length(prev_P - P_w);
                            geom_ok = (plane_err <= max(0.02f, px_ws * 2.8f)) && (rad_err <= max(0.12f, px_ws * 7.0f * grazing));
                        } else {
                            geom_ok = abs(pdz - cam_exp_dz) <= max(1e-4f, max(pdz, cam_exp_dz) * 0.04f * grazing);
                        }
                        if (!geom_ok)
                            continue;

                        float bw = (px_o == 0 ? (1.0f - p_frac.x) : p_frac.x) * (py == 0 ? (1.0f - p_frac.y) : p_frac.y);
                        float w = max(bw, 1e-3f) * pow(saturate(dot(pGN, N_w)), 8.0f);
                        float4 pao = prev_ao_tex.Load(int3(qp, 0));
                        p_bent_sum += pao.xyz * w;
                        p_vis_sum += pao.w * w;
                        p_gi_sum += (pgi.rgb * fcb.exposure_ratio) * w;
                        p_cnt_sum += pgi.w * w;
                        p_w_sum += w;
                    }
                }

                if (p_w_sum > 1e-4f) {
                    float3 prev_bent = safe_unit_normal(p_bent_sum);
                    float prev_vis = clamp(p_vis_sum / p_w_sum, max(0.0f, vis_min - 0.12f), min(1.0f, vis_max + 0.12f));
                    float3 prev_gi = p_gi_sum / p_w_sum;
                    float prev_cnt = p_cnt_sum / p_w_sum;
                    hist_cnt = min(prev_cnt + 1.0f, 12.0f);
                    float alpha_ao = max(1.0f / hist_cnt, 0.08f);
                    cur_vis = lerp(prev_vis, cur_vis, alpha_ao);
                    cur_bent = safe_unit_normal(lerp(prev_bent, cur_bent, alpha_ao));
                    cur_gi = lerp(prev_gi, cur_gi, max(alpha_ao, 0.12f));
                }
            }
        }

        bent_out[px] = float4(cur_bent, saturate(cur_vis));
        gi_out[px] = float4(clamp(cur_gi, 0.0f, 65504.0f), hist_cnt);
        return;
    }

    // ========================================================================
    // Mode 1: Clear GI Counters, SHaRC Accumulators, & Cold-Reset Hash Keys
    // ========================================================================
    if (g_gi_mode == 1u) {
        uint linear_id = gid.x * 64u + gindex;
        RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[atelier_safe_desc(g_uav0)];
        if (linear_id < 8u + fcb.archetype_count)
            counters[linear_id] = 0u;

        RWStructuredBuffer<uint4> wrc_accum = ResourceDescriptorHeap[atelier_safe_desc(g_uav1)];
        if (linear_id < SHARC_CAPACITY) {
            wrc_accum[linear_id * 2u + 0u] = uint4(0u, 0u, 0u, 0u);
            wrc_accum[linear_id * 2u + 1u] = uint4(0u, 0u, 0u, 0u);
            if (g_uav2 != 0xFFFFFFFFu && fcb.wrc_hash_keys_uav != 0xFFFFFFFFu) {
                RWStructuredBuffer<uint> hash_keys = ResourceDescriptorHeap[atelier_safe_desc(fcb.wrc_hash_keys_uav)];
                RWStructuredBuffer<uint2> voxel_keys = ResourceDescriptorHeap[atelier_safe_desc(g_uav2)];
                hash_keys[linear_id] = 0u;
                voxel_keys[linear_id] = uint2(0u, 0u);
            }
        }
        return;
    }

    // ========================================================================
    // Mode 2: Layer 1 — GI-1.2 `ReprojectScreenProbes` + Defensive CDF `SampleScreenProbes` + Hi-Z + DXR
    // ========================================================================
    if (g_gi_mode == 2u) {
        uint2 probes_dim = uint2((fcb.render_extent.x + 7u) / 8u, (fcb.render_extent.y + 7u) / 8u);
        uint2 probe_coord = gid.xy;
        if (probe_coord.x >= probes_dim.x || probe_coord.y >= probes_dim.y)
            return;
        uint probe_idx = probe_coord.y * probes_dim.x + probe_coord.x;
        float2 p_box0 = float2(probe_coord * 8u);
        float2 p_box1 = p_box0 + 8.0f;
        bool probe_in_vp = (p_box1.x > fcb.scene_viewport.x && p_box0.x < fcb.scene_viewport.x + fcb.scene_viewport.z &&
                            p_box1.y > fcb.scene_viewport.y && p_box0.y < fcb.scene_viewport.y + fcb.scene_viewport.w);
        if (!probe_in_vp || (fcb.flags & 2u) == 0u) {
            if (gindex == 0u) {
                RWStructuredBuffer<uint2> probe_state_out = ResourceDescriptorHeap[atelier_safe_desc(g_srv_aux0)];
                probe_state_out[probe_idx] = uint2(0xFFFFFFFFu, 0u);
            }
            return;
        }
        bool in_grid = true;

        uint grid_n = (fcb.gi_rays_per_probe >= 64u) ? 8u : 6u;
        uint total_rays = grid_n * grid_n;

        Texture2D<float> depth_tex = ResourceDescriptorHeap[atelier_safe_desc(g_depth_srv)];
        Texture2D<uint> gn_tex = ResourceDescriptorHeap[atelier_safe_desc(g_geom_normal_srv)];
        RWStructuredBuffer<uint4> probe_rays_out = ResourceDescriptorHeap[atelier_safe_desc(g_uav0)];
        RWTexture2D<float4> reproj_atlas_uav = ResourceDescriptorHeap[atelier_safe_desc(g_uav2)];
        RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[atelier_safe_desc(g_uav3)];
        RWStructuredBuffer<uint2> probe_state_uav = ResourceDescriptorHeap[atelier_safe_desc(g_srv_aux0)];
        RWStructuredBuffer<uint> ray_cells_uav = ResourceDescriptorHeap[atelier_safe_desc(g_srv_aux1)];

        gs_reproj_accum[gindex] = uint4(0u, 0u, 0u, 0u);
        if (gindex < 2u)
            gs_comp_mask[gindex] = 0u;

        if (gindex == 0u) {
            gs_probe_flags = 0u;
            gs_best_ppc = uint2(0u, 0u);
            gs_anchor_P = 0.0f;
            gs_anchor_N = float3(0.0f, 1.0f, 0.0f);
            gs_prev_P = 0.0f;
            gs_prev_N = float3(0.0f, 1.0f, 0.0f);

            if (in_grid) {
                uint2 anchor_0;
                float dz_0;
                bool ok_anchor = find_screen_probe_anchor(probe_coord, depth_tex, fcb.render_extent, fcb.scene_viewport, anchor_0, dz_0);
                bool gi_on = (fcb.flags & 2u) != 0u;
                if (!ok_anchor || !gi_on) {
                    probe_state_uav[probe_idx] = uint2(0xFFFFFFFFu, 0u);
                } else {
                    float3 cur_gn = unpack_oct_snorm16(gn_tex.Load(int3(anchor_0, 0)));
                    float3 cur_p = gi_reconstruct_world(float2(anchor_0) + 0.5f, dz_0, fcb);
                    uint cur_seed = (anchor_0.x & 0xFFFFu) | (anchor_0.y << 16u);
                    float cell_ws = 8.0f * gi_pixel_world_size(cur_p, fcb);

                    bool has_hist = ((fcb.flags & (1u << 7u)) != 0u) && (g_slab0_srv != 0xFFFFFFFFu) &&
                                    (g_slab1_srv != 0xFFFFFFFFu) && (g_hzb_srv != 0xFFFFFFFFu) &&
                                    (fcb.prev_view_z_srv != 0xFFFFFFFFu);
                    bool reproj_ok = false;
                    if (has_hist) {
                        Texture2D<float2> mot_tex = ResourceDescriptorHeap[atelier_safe_desc(g_motion_srv)];
                        float2 prev_uv = ((float2(anchor_0) + 0.5f - fcb.jitter_pixels) / float2(fcb.render_extent)) + mot_tex.Load(int3(anchor_0, 0));
                        if (all(prev_uv >= 0.0f) && all(prev_uv <= 1.0f)) {
                            row_major float4x4 inv_prev_vp = gi_invert_4x4(fcb.prev_unjittered_vp, fcb.inv_unjittered_vp);
                            Texture2D<uint> prev_mask = ResourceDescriptorHeap[atelier_safe_desc(g_slab1_srv)];
                            Texture2D<float> prev_depth = ResourceDescriptorHeap[atelier_safe_desc(fcb.prev_view_z_srv)];
                            Texture2D<uint> prev_gn_tex = ResourceDescriptorHeap[atelier_safe_desc(g_hzb_srv)];

                            int2 center_ppc = clamp(int2(floor((prev_uv * float2(fcb.render_extent) - 3.5f) * 0.125f + 0.5f)),
                                                    int2(0, 0), int2(probes_dim) - 1);
                            float best_score = -1e9f;
                            [loop] for (int dy = -1; dy <= 1; ++dy) {
                                [loop] for (int dx = -1; dx <= 1; ++dx) {
                                    int2 q_ppc = clamp(center_ppc + int2(dx, dy), int2(0, 0), int2(probes_dim) - 1);
                                    uint p_seed = prev_mask.Load(int3(q_ppc, 0));
                                    if (p_seed == 0xFFFFFFFFu)
                                        continue;
                                    uint2 p_px = uint2(p_seed & 0xFFFFu, p_seed >> 16u);
                                    float p_dz = prev_depth.Load(int3(p_px, 0));
                                    if (p_dz <= 0.0f)
                                        continue;
                                    float3 p_gn = unpack_oct_snorm16(prev_gn_tex.Load(int3(p_px, 0)));
                                    float ndot = dot(p_gn, cur_gn);
                                    if (ndot < 0.72f)
                                        continue;
                                    float3 p_pos = gi_reconstruct_prev_world(float2(p_px) + 0.5f, p_dz, inv_prev_vp, fcb);
                                    float plane_err = abs(dot(p_pos - cur_p, cur_gn));
                                    float rad_err = length(p_pos - cur_p);
                                    if (plane_err <= max(0.05f, cell_ws * 0.85f) && rad_err <= max(0.20f, cell_ws * 2.8f)) {
                                        float score = ndot - plane_err / max(0.05f, cell_ws) - 0.15f * rad_err / max(0.20f, cell_ws);
                                        if (score > best_score) {
                                            best_score = score;
                                            reproj_ok = true;
                                            gs_best_ppc = uint2(q_ppc);
                                            gs_prev_P = p_pos;
                                            gs_prev_N = p_gn;
                                        }
                                    }
                                }
                            }
                        }
                    }

                    bool trace_tile = !has_hist;
                    if (has_hist) {
                        if (reproj_ok) {
                            uint quad = (probe_coord.x & 1u) | ((probe_coord.y & 1u) << 1u);
                            trace_tile = (quad == (fcb.frame_index & 3u));
                        } else {
                            uint budget = max(64u, (probes_dim.x * probes_dim.y) >> 2u);
                            uint used_ov = 0u;
                            InterlockedAdd(counters[0u], 1u, used_ov);
                            trace_tile = (used_ov < budget);
                        }
                    }

                    uint pflags = 1u | (trace_tile ? 2u : 0u) | (reproj_ok ? 4u : 0u);
                    gs_probe_flags = pflags;
                    gs_anchor_P = cur_p;
                    gs_anchor_N = cur_gn;
                    probe_state_uav[probe_idx] = uint2(cur_seed, pflags | (total_rays << 8u));
                }
            }
        }
        GroupMemoryBarrierWithGroupSync();

        uint pflags = gs_probe_flags;
        bool probe_valid = (pflags & 1u) != 0u;
        if (!probe_valid)
            return;
        bool tile_active = (pflags & 2u) != 0u;
        bool reproj_ok = (pflags & 4u) != 0u;
        float3 anchor_P = gs_anchor_P;
        float3 anchor_N = gs_anchor_N;
        uint2 cell_xy = uint2(gindex & 7u, gindex >> 3u);

        // Step 1: 3D Parallax `ReprojectScreenProbes` + Rotational Lookup Pinhole Fill (all 64 cells in parallel):
        float4 reproj_cell = float4(0.0f, 0.0f, 0.0f, 16.0f);
        if (reproj_ok) {
            Texture2D<float4> prev_atlas = ResourceDescriptorHeap[atelier_safe_desc(g_slab0_srv)];
            float4 pv = prev_atlas.Load(int3(gs_best_ppc * 8u + cell_xy, 0));
            float3 prev_rad = clamp(pv.rgb * fcb.exposure_ratio, 0.0f, 32.0f);
            float prev_hit = pv.w;
            gs_vec4_a[gindex] = float4(prev_rad, prev_hit);
            if (prev_hit > 0.0f) {
                float3 prev_local = hemi_oct_decode((float2(cell_xy) + 0.5f) * 0.125f);
                float3 prev_dir = atelier_local_to_world(prev_local, gs_prev_N);
                float3 hit_P = gs_prev_P + prev_dir * clamp(prev_hit, 1e-3f, 500.0f);
                float3 rvec = hit_P - anchor_P;
                float rdist = clamp(length(rvec), 1e-3f, 500.0f);
                float3 rdir = rvec / rdist;
                float3 cur_local = atelier_world_to_local(rdir, anchor_N);
                if (cur_local.z > 1e-4f) {
                    uint2 dst_c = min(uint2(hemi_oct_encode(cur_local) * 8.0f), 7u);
                    uint dst_idx = (dst_c.y << 3u) | dst_c.x;
                    uint4 qv = uint4(uint3(round(prev_rad * 65536.0f)), (uint(round(rdist * 64.0f)) << 8u) | 1u);
                    InterlockedAdd(gs_reproj_accum[dst_idx].x, qv.x);
                    InterlockedAdd(gs_reproj_accum[dst_idx].y, qv.y);
                    InterlockedAdd(gs_reproj_accum[dst_idx].z, qv.z);
                    InterlockedAdd(gs_reproj_accum[dst_idx].w, qv.w);
                }
            }
        }
        GroupMemoryBarrierWithGroupSync();

        if (reproj_ok) {
            uint4 acc = gs_reproj_accum[gindex];
            uint cnt = acc.w & 0xFFu;
            if (cnt > 0u) {
                reproj_cell = float4(float3(acc.xyz) * (1.0f / (65536.0f * float(cnt))),
                                     clamp((float(acc.w >> 8u) * (1.0f / 64.0f)) / float(cnt), 1e-3f, 500.0f));
            } else {
                // Rotational backward lookup into `gs_vec4_a` (`prev_atlas`) prevents discrete pinholes AND `RadianceBackup` explosion:
                float3 cur_dir = atelier_local_to_world(hemi_oct_decode((float2(cell_xy) + 0.5f) * 0.125f), anchor_N);
                float3 pl = atelier_world_to_local(cur_dir, gs_prev_N);
                bool filled = false;
                if (pl.z > 1e-4f) {
                    uint2 sc = min(uint2(hemi_oct_encode(pl) * 8.0f), 7u);
                    float4 sv = gs_vec4_a[(sc.y << 3u) | sc.x];
                    if (sv.w > 0.0f) {
                        reproj_cell = float4(sv.rgb, clamp(sv.w, 1e-3f, 500.0f));
                        filled = true;
                    }
                }
                if (!filled) {
                    float3 m_sum = 0.0f;
                    float h_sum = 0.0f;
                    float m_cnt = 0.0f;
                    [loop] for (uint k = 0u; k < 64u; ++k) {
                        float4 kv = gs_vec4_a[k];
                        if (kv.w > 0.0f) {
                            m_sum += kv.rgb;
                            h_sum += kv.w;
                            m_cnt += 1.0f;
                        }
                    }
                    reproj_cell = (m_cnt > 0.0f) ? float4(m_sum / m_cnt, clamp(h_sum / m_cnt, 1e-3f, 500.0f))
                                                 : float4(eval_atelier_sky_diffuse_radiance(anchor_N, fcb), 16.0f);
                }
            }
            reproj_atlas_uav[probe_coord * 8u + cell_xy] = reproj_cell;
        }

        // Step 2: Defensive `75% (Radiance * Cosine) + 25% Uniform` Inclusive Prefix-Sum CDF (`SampleScreenProbes`):
        float cos_cell = hemi_oct_decode((float2(cell_xy) + 0.5f) * 0.125f).z;
        gs_cdf[gindex] = (tile_active && reproj_ok)
                             ? (max(dot(reproj_cell.rgb, float3(0.2126f, 0.7152f, 0.0722f)), 0.0f) * cos_cell)
                             : 0.0f;
        GroupMemoryBarrierWithGroupSync();

        [loop] for (uint stride = 1u; stride < 64u; stride <<= 1u) {
            float add_v = (gindex >= stride) ? gs_cdf[gindex - stride] : 0.0f;
            GroupMemoryBarrierWithGroupSync();
            gs_cdf[gindex] += add_v;
            GroupMemoryBarrierWithGroupSync();
        }
        float cdf_total = gs_cdf[63];
        float uni_cdf = float(gindex + 1u) * (1.0f / 64.0f);
        float def_cdf = (cdf_total > 1e-5f) ? (0.75f * (gs_cdf[gindex] / cdf_total) + 0.25f * uni_cdf) : uni_cdf;
        gs_cdf[gindex] = (gindex == 63u) ? 1.0f : def_cdf;
        GroupMemoryBarrierWithGroupSync();

        bool lane_active = tile_active && (gindex < total_rays);
        float3 ray_dir = anchor_N;
        uint out_slot = probe_idx * 64u + gindex;
        bool needs_dxr = false;

        if (lane_active) {
            uint rot_phase = ((fcb.flags & (1u << 7u)) != 0u) ? (fcb.frame_index >> 2u) : 0u;
            float2 sub_jit = atelier_blue_jitter2(probe_coord * 8u + cell_xy, rot_phase, 0x9E3779B9u);
            uint picked_cell = gindex;
            float2 oct_uv = 0.5f;

            if (reproj_ok) {
                float2 probe_jit = atelier_blue_jitter2(probe_coord, rot_phase, 0x6A09E667u);
                float u_cdf = (float(gindex) + probe_jit.x) / float(total_rays);
                uint lo = 0u, hi = 63u;
                [unroll] for (uint bs = 0u; bs < 6u; ++bs) {
                    uint mid = (lo + hi) >> 1u;
                    if (gs_cdf[mid] > u_cdf)
                        hi = mid;
                    else
                        lo = mid + 1u;
                }
                picked_cell = lo;
                oct_uv = (float2(lo & 7u, lo >> 3u) + sub_jit) * 0.125f;
            } else {
                uint rx = gindex % grid_n;
                uint ry = gindex / grid_n;
                oct_uv = (float2(rx, ry) + sub_jit) / float(grid_n);
                uint2 c_init = min(uint2(oct_uv * 8.0f), 7u);
                picked_cell = (c_init.y << 3u) | c_init.x;
            }
            ray_cells_uav[out_slot] = picked_cell;
            ray_dir = safe_unit_normal(atelier_local_to_world(hemi_oct_decode(oct_uv), anchor_N));

            // Step 3: Exact Perspective Screen-Space Hi-Z March against `PrevDiffuseEmissiveRadiance`:
            bool hit_ss = false;
            if ((fcb.flags & (1u << 7u)) != 0u && fcb.hzb_srv != 0xFFFFFFFFu && g_prev_diffuse_srv != 0xFFFFFFFFu) {
                float3 orig_w = anchor_P + anchor_N * 3.0e-3f;
                float3 orig_v = mul(float4(orig_w, 1.0f), fcb.view).xyz;
                float3 rdir_v = mul(ray_dir, (float3x3)fcb.view);
                float max_d = 3.5f;
                if (abs(fcb.projection[2][3]) > 1e-6f && rdir_v.z < -1e-4f)
                    max_d = min(max_d, max(0.0f, (orig_v.z - 0.12f) / (-rdir_v.z)));

                if (max_d > 0.10f) {
                    float4 c0 = mul(float4(orig_w, 1.0f), fcb.view_projection);
                    float4 c1 = mul(float4(orig_w + ray_dir * max_d, 1.0f), fcb.view_projection);
                    if (c0.w > 0.08f && c1.w > 0.08f) {
                        float2 p0 = float2(fcb.scene_viewport.x + (c0.x / c0.w * 0.5f + 0.5f) * fcb.scene_viewport.z,
                                           fcb.scene_viewport.y + (0.5f - c0.y / c0.w * 0.5f) * fcb.scene_viewport.w);
                        float2 p1 = float2(fcb.scene_viewport.x + (c1.x / c1.w * 0.5f + 0.5f) * fcb.scene_viewport.z,
                                           fcb.scene_viewport.y + (0.5f - c1.y / c1.w * 0.5f) * fcb.scene_viewport.w);
                        float inv_w0 = 1.0f / c0.w;
                        float inv_w1 = 1.0f / c1.w;
                        float z_over_w0 = c0.z * inv_w0;
                        float z_over_w1 = c1.z * inv_w1;

                        Texture2D<float2> hzb_tex = ResourceDescriptorHeap[atelier_safe_desc(fcb.hzb_srv)];
                        Texture2D<float4> prev_diff = ResourceDescriptorHeap[atelier_safe_desc(g_prev_diffuse_srv)];
                        Texture2D<float2> mot_tex = ResourceDescriptorHeap[atelier_safe_desc(g_motion_srv)];
                        SamplerState smp_near = SamplerDescriptorHeap[3];
                        uint2 hzb_dim0 = max(uint2(1u, 1u), (fcb.render_extent + 1u) >> 1u);

                        [loop] for (uint s = 1u; s <= 10u; ++s) {
                            float t = float(s) * 0.1f;
                            float2 sp = lerp(p0, p1, t);
                            if (sp.x < fcb.scene_viewport.x + 2.0f || sp.x >= fcb.scene_viewport.x + fcb.scene_viewport.z - 2.0f ||
                                sp.y < fcb.scene_viewport.y + 2.0f || sp.y >= fcb.scene_viewport.y + fcb.scene_viewport.w - 2.0f)
                                break;
                            float inv_wt = lerp(inv_w0, inv_w1, t);
                            float rz = lerp(z_over_w0, z_over_w1, t);
                            int2 hc = clamp(int2(sp) >> 1, int2(0, 0), int2(hzb_dim0) - 1);
                            float hz_max = hzb_tex.Load(int3(hc, 0)).y;
                            if (hz_max > 0.0f && rz < hz_max) {
                                float sd = depth_tex.Load(int3(int2(sp), 0));
                                if (sd > 0.0f && rz <= sd) {
                                    float raw_world_t = max_d * (t * inv_w1) / max(inv_wt, 1e-6f);
                                    float ray_vz = (abs(fcb.projection[2][3]) > 1e-6f)
                                                       ? (1.0f / max(inv_wt, 1e-5f))
                                                       : abs(orig_v.z + rdir_v.z * raw_world_t);
                                    float2 prev_uv = (sp / float2(fcb.render_extent)) + mot_tex.Load(int3(int2(sp), 0));
                                    if (all(prev_uv >= 0.01f) && all(prev_uv <= 0.99f)) {
                                        float2 pd_span = float2(((fcb.render_extent + 1u) >> 1u) << 1u);
                                        float4 pd = prev_diff.SampleLevel(smp_near, (prev_uv * float2(fcb.render_extent)) / pd_span, 0.0f);
                                        if (pd.a > 0.005f && ray_vz >= pd.a - 0.02f &&
                                            (ray_vz - pd.a) < max(0.08f, pd.a * 0.03f) && raw_world_t > 0.08f) {
                                            probe_rays_out[out_slot] = asuint(float4(min(pd.rgb * fcb.exposure_ratio, 16.0f),
                                                                                     clamp(raw_world_t, 0.08f, 500.0f)));
                                            hit_ss = true;
                                        }
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
            }

            if (!hit_ss) {
                if (fcb.tlas_srv == 0xFFFFFFFFu) {
                    probe_rays_out[out_slot] = asuint(float4(min(eval_atelier_sky(ray_dir, 3.0f, fcb), 32.0f), 500.0f));
                } else {
                    needs_dxr = true;
                }
            }
        }

        // Step 4: 64-Bit LDS Bitmask Order-Preserving Compaction + Inline DXR `RayQuery`:
        if (needs_dxr)
            InterlockedOr(gs_comp_mask[gindex >> 5u], 1u << (gindex & 31u));
        GroupMemoryBarrierWithGroupSync();

        uint cm0 = gs_comp_mask[0];
        uint cm1 = gs_comp_mask[1];
        uint compacted_total = countbits(cm0) + countbits(cm1);
        if (needs_dxr) {
            uint lo_mask = (gindex < 32u) ? ((1u << gindex) - 1u) : 0xFFFFFFFFu;
            uint hi_mask = (gindex > 32u) ? ((1u << (gindex - 32u)) - 1u) : 0u;
            uint dst = countbits(cm0 & lo_mask) + countbits(cm1 & hi_mask);
            gs_comp_dir[dst] = pack_oct_snorm16(ray_dir);
            gs_comp_slot[dst] = gindex;
        }
        GroupMemoryBarrierWithGroupSync();

        if (tile_active && gindex == 0u && compacted_total > 0u)
            InterlockedAdd(counters[1u], compacted_total);

        if (tile_active && gindex < compacted_total) {
            float3 origin = anchor_P + anchor_N * 3.0e-3f;
            float3 dxr_dir = unpack_oct_snorm16(gs_comp_dir[gindex]);
            uint orig_idx = gs_comp_slot[gindex];
            uint dst_slot = probe_idx * 64u + orig_idx;

            RaytracingAccelerationStructure tlas = ResourceDescriptorHeap[atelier_safe_desc(fcb.tlas_srv)];
            RayDesc ray;
            ray.Origin = origin;
            ray.Direction = dxr_dir;
            ray.TMin = 1e-3f;
            ray.TMax = 200.0f;

            RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
            q.TraceRayInline(tlas, RAY_FLAG_FORCE_OPAQUE, 0xFFu, ray);
            q.Proceed();

            if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
                uint inst_id = q.CommittedInstanceID();
                StructuredBuffer<GpuInstanceRecord> instances = ResourceDescriptorHeap[atelier_safe_desc(fcb.instances_srv)];
                StructuredBuffer<GpuMeshDescriptor> meshes = ResourceDescriptorHeap[atelier_safe_desc(fcb.mesh_table_srv)];
                StructuredBuffer<uint> part_bases = ResourceDescriptorHeap[atelier_safe_desc(fcb.part_tri_base_srv)];
                StructuredBuffer<uint> slot_map = ResourceDescriptorHeap[atelier_safe_desc(fcb.material_slot_map_srv)];
                StructuredBuffer<GpuSurfaceRecord> surfaces = ResourceDescriptorHeap[atelier_safe_desc(fcb.material_surfaces_srv)];

                GpuInstanceRecord inst = instances[inst_id];
                GpuMeshDescriptor mesh = meshes[inst.mesh_pool_index];
                uint global_tri = part_bases[inst.part_table_base + q.CommittedGeometryIndex()] + q.CommittedPrimitiveIndex();
                StructuredBuffer<uint> fmeta_s = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(mesh.face_meta_srv))];
                uint surf_idx = slot_map[inst.material_slot_offset + (fmeta_s[global_tri] & 0x07FFFFFFu)];
                uint arch = surfaces[surf_idx].archetype_index;

                float2 b = q.CommittedTriangleBarycentrics();
                GpuGiRayHit hit;
                hit.probe_and_ray = dst_slot;
                hit.instance_id = inst_id;
                hit.global_tri_id = global_tri;
                hit.packed_bary = (f32tof16(b.x) & 0xFFFFu) | (f32tof16(b.y) << 16u);
                hit.ray_origin = origin;
                hit.hit_t = q.CommittedRayT();
                hit.ray_dir = dxr_dir;
                hit.surf_idx = surf_idx;

                RWStructuredBuffer<GpuGiRayHit> hit_buf = ResourceDescriptorHeap[atelier_safe_desc(g_uav1)];
                uint hit_idx = 0u;
                InterlockedAdd(counters[2u], 1u, hit_idx);
                hit_buf[hit_idx] = hit;
                InterlockedAdd(counters[4u + arch], 1u);
            } else {
                probe_rays_out[dst_slot] = asuint(float4(min(eval_atelier_sky(dxr_dir, 3.0f, fcb), 32.0f), 500.0f));
            }
        }
        return;
    }

    // ========================================================================
    // Mode 3: Prefix-Sum & Scatter GI Hit Bins (`g_srv_aux2`: 0 = PrefixSum, 1 = Scatter)
    // ========================================================================
    if (g_gi_mode == 3u) {
        RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[atelier_safe_desc(g_uav0)];
        RWStructuredBuffer<uint> offsets = ResourceDescriptorHeap[atelier_safe_desc(g_uav1)];
        RWStructuredBuffer<uint> cursors = ResourceDescriptorHeap[atelier_safe_desc(g_uav2)];
        RWByteAddressBuffer indirect_args = ResourceDescriptorHeap[atelier_safe_desc(g_uav3)];

        if (g_srv_aux2 == 0u) {
            if (gid.x == 0u && gindex == 0u) {
                uint running = 0u;
                [loop] for (uint a = 0u; a < fcb.archetype_count; ++a) {
                    uint c = counters[4u + a];
                    offsets[a] = running;
                    cursors[a] = running;
                    running += c;
                    uint groups = (c + 63u) >> 6u;
                    uint3 grid = (groups <= 65535u) ? uint3(groups, 1u, 1u) : uint3(1024u, (groups + 1023u) >> 10u, 1u);
                    indirect_args.Store3(a * 12u, grid);
                }
                RWStructuredBuffer<uint> arch_counts = ResourceDescriptorHeap[atelier_safe_desc(g_srv_aux0)];
                [loop] for (uint a2 = 0u; a2 < fcb.archetype_count; ++a2)
                    arch_counts[a2] = counters[4u + a2];
            }
        } else {
            uint linear_id = (gid.y * 1024u + gid.x) * 64u + gindex;
            uint total_hits = counters[2u];
            if (linear_id >= total_hits)
                return;
            StructuredBuffer<GpuGiRayHit> hits = ResourceDescriptorHeap[atelier_safe_desc(g_srv_aux0)];
            StructuredBuffer<GpuSurfaceRecord> surfaces = ResourceDescriptorHeap[atelier_safe_desc(fcb.material_surfaces_srv)];
            uint arch = surfaces[hits[linear_id].surf_idx].archetype_index;
            RWStructuredBuffer<uint> sorted_indices = ResourceDescriptorHeap[atelier_safe_desc(g_srv_aux1)];
            uint dst = 0u;
            InterlockedAdd(cursors[arch], 1u, dst);
            sorted_indices[dst] = linear_id;
        }
        return;
    }

    // ========================================================================
    // Mode 4: Layer 2 — Resolve SHaRC v1.8.3 + GI-1.2 Multi-Bounce Surface Cache (Async Compute)
    // ========================================================================
    if (g_gi_mode == 4u) {
        uint slot = gid.x * 64u + gindex;
        if (slot >= SHARC_CAPACITY)
            return;

        RWStructuredBuffer<uint4> wrc_curr = ResourceDescriptorHeap[atelier_safe_desc(g_uav0)];
        if (fcb.wrc_hash_keys_uav == 0xFFFFFFFFu || g_uav1 == 0xFFFFFFFFu || (fcb.flags & 2u) == 0u) {
            wrc_curr[slot * 2u + 0u] = uint4(0u, 0u, 0u, 0u);
            wrc_curr[slot * 2u + 1u] = uint4(0u, 0u, 0u, 0u);
            return;
        }

        RWStructuredBuffer<uint> hash_keys = ResourceDescriptorHeap[atelier_safe_desc(fcb.wrc_hash_keys_uav)];
        RWStructuredBuffer<uint2> voxel_keys = ResourceDescriptorHeap[atelier_safe_desc(g_uav1)];

        if (g_srv_aux2 != 0u) {
            uint sf_evict = wrc_curr[slot * 2u + 0u].z >> 16u;
            if (sf_evict >= SHARC_STALE_FRAME_MAX) {
                hash_keys[slot] = 0u;
                voxel_keys[slot] = uint2(0u, 0u);
                wrc_curr[slot * 2u + 0u] = uint4(0u, 0u, 0u, 0u);
                wrc_curr[slot * 2u + 1u] = uint4(0u, 0u, 0u, 0u);
            }
            return;
        }

        uint checksum = hash_keys[slot];
        if (checksum == 0u) {
            wrc_curr[slot * 2u + 0u] = uint4(0u, 0u, 0u, 0u);
            wrc_curr[slot * 2u + 1u] = uint4(0u, 0u, 0u, 0u);
            return;
        }

        StructuredBuffer<uint4> wrc_accum = ResourceDescriptorHeap[atelier_safe_desc(g_srv_aux0)];
        bool has_hist = (g_srv_aux1 != 0xFFFFFFFFu) && ((fcb.flags & (1u << 7u)) != 0u);

        uint4 acc0 = wrc_accum[slot * 2u + 0u];
        uint4 acc1 = wrc_accum[slot * 2u + 1u];
        float s_new0 = float(acc0.w);
        float s_new1 = float(acc1.w);

        float3 rad_prev0 = 0.0f, rad_prev1 = 0.0f;
        float s_prev0 = 0.0f, s_prev1 = 0.0f;
        uint af0 = 0u, sf0 = 0u, fm0 = 0u;
        uint af1 = 0u, sf1 = 0u, fm1 = 0u;
        if (has_hist) {
            StructuredBuffer<uint4> wrc_prev = ResourceDescriptorHeap[atelier_safe_desc(g_srv_aux1)];
            sharc_unpack_resolved(wrc_prev[slot * 2u + 0u], rad_prev0, s_prev0, af0, sf0, fm0);
            sharc_unpack_resolved(wrc_prev[slot * 2u + 1u], rad_prev1, s_prev1, af1, sf1, fm1);
            rad_prev0 *= fcb.exposure_ratio;
            rad_prev1 *= fcb.exposure_ratio;
        }

        uint stale_frames = (s_new0 > 0.0f || s_new1 > 0.0f) ? 0u : (sf0 + 1u);
        if (stale_frames >= SHARC_STALE_FRAME_MAX) {
            wrc_curr[slot * 2u + 0u] = sharc_pack_resolved(0.0f, 0.0f, 0u, stale_frames, 0u);
            wrc_curr[slot * 2u + 1u] = uint4(0u, 0u, 0u, 0u);
            return;
        }

        uint bit_off = fcb.frame_index & 31u;
        uint fade_bit = 1u << bit_off;
        uint2 vkey = voxel_keys[slot];
        float3 cam_delta = fcb.camera_eye - fcb.prev_camera_eye;
        bool cam_moved = has_hist && (dot(cam_delta, cam_delta) > 1e-6f);

        float3 res_rad0 = rad_prev0;
        float res_s0 = s_prev0;
        uint res_af0 = min(af0 + 1u, SHARC_ACCUM_FRAME_MAX);
        uint res_fm0 = fm0;
        if (s_new0 <= 0.0f) {
            res_fm0 |= fade_bit;
        } else {
            float3 cur_rad0 = float3(acc0.xyz) / (SHARC_RADIANCE_SCALE * s_new0);
            float luma_cur0 = dot(cur_rad0, float3(0.213f, 0.715f, 0.072f));
            float luma_prv0 = dot(rad_prev0, float3(0.213f, 0.715f, 0.072f));
            if (s_prev0 >= 4.0f) {
                float max_luma0 = max(luma_prv0 * 4.0f, luma_prv0 + 0.50f);
                if (luma_cur0 > max_luma0)
                    cur_rad0 *= max_luma0 / max(luma_cur0, 1e-6f);
            } else if (s_new0 < 4.0f && luma_cur0 > 8.0f) {
                float compr0 = 8.0f + log2(1.0f + luma_cur0 - 8.0f);
                cur_rad0 *= compr0 / luma_cur0;
            }
            uint next_af0 = af0 + 1u;
            if (next_af0 > SHARC_ACCUM_FRAME_MAX) {
                s_prev0 *= float(SHARC_ACCUM_FRAME_MAX) / float(next_af0);
                next_af0 = SHARC_ACCUM_FRAME_MAX;
            }
            res_af0 = next_af0;
            bool fading0 = (s_prev0 > 0.0f) && (luma_cur0 < luma_prv0 * 0.98f);
            res_fm0 = (fm0 & ~fade_bit) | (fading0 ? fade_bit : 0u);
            if (countbits(res_fm0) >= 24u)
                s_prev0 = min(s_prev0, max(s_new0 * 2.0f, 2.0f));
            float inv_tot0 = 1.0f / max(s_prev0 + s_new0, 1e-6f);
            res_rad0 = rad_prev0 * (s_prev0 * inv_tot0) + cur_rad0 * (s_new0 * inv_tot0);
            res_s0 = s_prev0 + s_new0;
        }

        float3 res_rad1 = rad_prev1;
        float res_s1 = s_prev1;
        uint res_af1 = min(af1 + 1u, SHARC_INDIRECT_FRAME_MAX);
        uint res_fm1 = fm1;
        if (s_new1 <= 0.0f) {
            res_fm1 |= fade_bit;
        } else {
            float3 cur_rad1 = float3(acc1.xyz) / (SHARC_RADIANCE_SCALE * s_new1);
            float luma_cur1 = dot(cur_rad1, float3(0.213f, 0.715f, 0.072f));
            float luma_prv1 = dot(rad_prev1, float3(0.213f, 0.715f, 0.072f));
            if (s_prev1 >= 3.0f) {
                float max_luma1 = max(luma_prv1 * 3.5f, luma_prv1 + 0.35f);
                if (luma_cur1 > max_luma1)
                    cur_rad1 *= max_luma1 / max(luma_cur1, 1e-6f);
            } else if (s_new1 < 3.0f && luma_cur1 > 4.0f) {
                float compr1 = 4.0f + log2(1.0f + luma_cur1 - 4.0f);
                cur_rad1 *= compr1 / luma_cur1;
            }
            uint next_af1 = af1 + 1u;
            if (next_af1 > SHARC_INDIRECT_FRAME_MAX) {
                s_prev1 *= float(SHARC_INDIRECT_FRAME_MAX) / float(next_af1);
                next_af1 = SHARC_INDIRECT_FRAME_MAX;
            }
            res_af1 = next_af1;
            bool fading1 = (s_prev1 > 0.0f) && (luma_cur1 < luma_prv1 * 0.98f);
            res_fm1 = (fm1 & ~fade_bit) | (fading1 ? fade_bit : 0u);
            if (countbits(res_fm1) >= 24u)
                s_prev1 = min(s_prev1, max(s_new1 * 2.0f, 2.0f));
            float inv_tot1 = 1.0f / max(s_prev1 + s_new1, 1e-6f);
            res_rad1 = rad_prev1 * (s_prev1 * inv_tot1) + cur_rad1 * (s_new1 * inv_tot1);
            res_s1 = s_prev1 + s_new1;
        }

        if (cam_moved && res_af0 <= 2u && (vkey.x != 0u || vkey.y != 0u)) {
            uint2 adj_key = sharc_adjacent_level_key(vkey, fcb.camera_eye, fcb.prev_camera_eye);
            uint adj_slot = sharc_find_slot_uav(hash_keys, adj_key);
            if (adj_slot != SHARC_INVALID_SLOT && all(voxel_keys[adj_slot] == adj_key)) {
                StructuredBuffer<uint4> wrc_prev = ResourceDescriptorHeap[atelier_safe_desc(g_srv_aux1)];
                float3 adj_r0, adj_r1;
                float adj_s0, adj_s1;
                uint a_af0, a_sf0, a_fm0, a_af1, a_sf1, a_fm1;
                sharc_unpack_resolved(wrc_prev[adj_slot * 2u + 0u], adj_r0, adj_s0, a_af0, a_sf0, a_fm0);
                sharc_unpack_resolved(wrc_prev[adj_slot * 2u + 1u], adj_r1, adj_s1, a_af1, a_sf1, a_fm1);
                if (a_sf0 + 1u < SHARC_STALE_FRAME_MAX) {
                    adj_r0 *= fcb.exposure_ratio;
                    adj_r1 *= fcb.exposure_ratio;
                    if (adj_s0 > 0.0f) {
                        float w0_b = 1.0f / (adj_s0 + res_s0);
                        res_rad0 = adj_r0 * (adj_s0 * w0_b) + res_rad0 * (res_s0 * w0_b);
                        res_s0 += adj_s0;
                    }
                    if (adj_s1 > 0.0f) {
                        float w1_b = 1.0f / (adj_s1 + res_s1);
                        res_rad1 = adj_r1 * (adj_s1 * w1_b) + res_rad1 * (res_s1 * w1_b);
                        res_s1 += adj_s1;
                    }
                }
            }
        }

        wrc_curr[slot * 2u + 0u] = sharc_pack_resolved(res_rad0, res_s0, res_af0, stale_frames, res_fm0);
        wrc_curr[slot * 2u + 1u] = sharc_pack_resolved(res_rad1, res_s1, res_af1, stale_frames, res_fm1);
        return;
    }

    // ========================================================================
    // Mode 7: Layer 1 — GI-1.2 `ResolveCells` (SHaRC Slot[0] Handoff) + `BlendScreenProbes`
    // ========================================================================
    if (g_gi_mode == 7u) {
        uint2 probes_dim = uint2((fcb.render_extent.x + 7u) / 8u, (fcb.render_extent.y + 7u) / 8u);
        uint2 probe_coord = gid.xy;
        bool in_grid = (probe_coord.x < probes_dim.x && probe_coord.y < probes_dim.y);
        uint probe_idx = probe_coord.y * probes_dim.x + probe_coord.x;
        uint2 cell_xy = uint2(gindex & 7u, gindex >> 3u);
        uint2 atlas_px = probe_coord * 8u + cell_xy;

        StructuredBuffer<uint2> probe_state_buf = ResourceDescriptorHeap[atelier_safe_desc(g_slab0_srv)];
        RWTexture2D<float4> atlas_uav = ResourceDescriptorHeap[atelier_safe_desc(g_uav0)];
        RWTexture2D<uint> mask_uav = ResourceDescriptorHeap[atelier_safe_desc(g_uav1)];

        uint2 pstate = in_grid ? probe_state_buf[probe_idx] : uint2(0xFFFFFFFFu, 0u);
        uint seed = pstate.x;
        uint flags = pstate.y & 0xFFu;
        uint total_rays = (pstate.y >> 8u) & 0xFFu;
        bool probe_valid = in_grid && ((flags & 1u) != 0u) && (seed != 0xFFFFFFFFu);
        bool was_traced = probe_valid && ((flags & 2u) != 0u);
        bool reproj_valid = probe_valid && ((flags & 4u) != 0u);

        if (in_grid && gindex == 0u)
            mask_uav[probe_coord] = probe_valid ? seed : 0xFFFFFFFFu;
        if (!in_grid)
            return;
        if (!probe_valid) {
            atlas_uav[atlas_px] = 0.0f;
            return;
        }

        gs_reproj_accum[gindex] = uint4(0u, 0u, 0u, 0u);
        GroupMemoryBarrierWithGroupSync();

        if (was_traced && gindex < total_rays) {
            StructuredBuffer<uint4> probe_rays = ResourceDescriptorHeap[atelier_safe_desc(g_srv_aux0)];
            StructuredBuffer<uint> ray_cells = ResourceDescriptorHeap[atelier_safe_desc(g_srv_aux1)];
            uint rslot = probe_idx * 64u + gindex;
            uint4 rv = probe_rays[rslot];
            uint c_idx = ray_cells[rslot] & 63u;
            float rw_f = asfloat(rv.w);

            float3 ray_rad = 0.0f;
            float hit_t = 16.0f;
            if (rw_f < 0.0f) {
                hit_t = clamp(-rw_f, 1e-3f, 500.0f);
                float2 rg = sharc_unpack_fp16x2(rv.y);
                float2 b0 = sharc_unpack_fp16x2(rv.z);
                float3 direct_rad = clamp(float3(rg.x, rg.y, b0.x), 0.0f, 32.0f);
                ray_rad = direct_rad;
                if (fcb.wrc_curr_srv != 0xFFFFFFFFu) {
                    StructuredBuffer<uint4> wrc_curr = ResourceDescriptorHeap[atelier_safe_desc(fcb.wrc_curr_srv)];
                    uint sh_slot = min(rv.x, SHARC_CAPACITY - 1u);
                    float3 c_rad;
                    float c_snum;
                    uint c_af, c_sf, c_fm;
                    sharc_unpack_resolved(wrc_curr[sh_slot * 2u + 0u], c_rad, c_snum, c_af, c_sf, c_fm);
                    if (c_snum > 0.0f)
                        ray_rad = lerp(direct_rad, clamp(c_rad, 0.0f, 32.0f), saturate((c_snum - 1.0f) * 0.25f));
                }
            } else {
                hit_t = clamp(rw_f, 1e-3f, 500.0f);
                ray_rad = clamp(asfloat(rv.xyz), 0.0f, 32.0f);
            }

            uint4 qv = uint4(uint3(round(ray_rad * 65536.0f)), (uint(round(hit_t * 64.0f)) << 8u) | 1u);
            InterlockedAdd(gs_reproj_accum[c_idx].x, qv.x);
            InterlockedAdd(gs_reproj_accum[c_idx].y, qv.y);
            InterlockedAdd(gs_reproj_accum[c_idx].z, qv.z);
            InterlockedAdd(gs_reproj_accum[c_idx].w, qv.w);
        }
        GroupMemoryBarrierWithGroupSync();

        float4 rep_loaded = float4(0.0f, 0.0f, 0.0f, 16.0f);
        if (reproj_valid) {
            Texture2D<float4> reproj_tex = ResourceDescriptorHeap[atelier_safe_desc(g_slab1_srv)];
            rep_loaded = reproj_tex.Load(int3(atlas_px, 0));
        }
        float cos_cell_b = hemi_oct_decode((float2(cell_xy) + 0.5f) * 0.125f).z;
        float cell_w_b = (reproj_valid && was_traced)
                             ? (max(dot(rep_loaded.rgb, float3(0.2126f, 0.7152f, 0.0722f)), 0.0f) * cos_cell_b)
                             : 0.0f;
        gs_cdf[gindex] = cell_w_b;

        uint4 acc = gs_reproj_accum[gindex];
        uint cnt = acc.w & 0xFFu;
        float3 resolved_rad = (cnt > 0u) ? (float3(acc.xyz) * (1.0f / (65536.0f * float(cnt)))) : 0.0f;
        float resolved_hit = (cnt > 0u) ? ((float(acc.w >> 8u) * (1.0f / 64.0f)) / float(cnt)) : -1.0f;
        gs_vec4_a[gindex] = float4(resolved_rad, resolved_hit);
        GroupMemoryBarrierWithGroupSync();

        [loop] for (uint s_b = 1u; s_b < 64u; s_b <<= 1u) {
            float add_b = (gindex >= s_b) ? gs_cdf[gindex - s_b] : 0.0f;
            GroupMemoryBarrierWithGroupSync();
            gs_cdf[gindex] += add_b;
            GroupMemoryBarrierWithGroupSync();
        }
        float tot_w_b = gs_cdf[63];

        if (!in_grid)
            return;
        if (!probe_valid) {
            atlas_uav[atlas_px] = 0.0f;
            return;
        }

        float4 out_cell = float4(0.0f, 0.0f, 0.0f, 16.0f);
        if (reproj_valid && was_traced) {
            if (cnt == 0u) {
                out_cell = rep_loaded;
            } else {
                float cell_pmf = (tot_w_b > 1e-5f) ? (0.75f * (cell_w_b / tot_w_b) + (0.25f / 64.0f)) : (1.0f / 64.0f);
                float p_visit = min(1.0f, float(max(total_rays, 1u)) * cell_pmf);
                float alpha_rad = clamp(0.18f / max(p_visit, 0.225f), 0.18f, 0.80f);
                float l_prv = dot(rep_loaded.rgb, float3(0.2126f, 0.7152f, 0.0722f));
                float l_new = dot(resolved_rad, float3(0.2126f, 0.7152f, 0.0722f));
                float l_cap = max(l_prv * 6.0f, l_prv + lerp(1.5f, 4.0f, saturate((alpha_rad - 0.18f) * 2.0f)));
                if (l_new > l_cap)
                    resolved_rad *= l_cap / max(l_new, 1e-6f);
                out_cell = float4(lerp(rep_loaded.rgb, resolved_rad, alpha_rad),
                                  clamp(lerp(rep_loaded.w, resolved_hit, max(0.25f, alpha_rad)), 1e-3f, 500.0f));
            }
        } else if (reproj_valid) {
            out_cell = rep_loaded;
        } else if (was_traced) {
            if (cnt > 0u) {
                out_cell = float4(resolved_rad, clamp(resolved_hit, 1e-3f, 500.0f));
            } else {
                float3 n_rad = 0.0f;
                float n_hit = 0.0f;
                float n_cnt = 0.0f;
                [loop] for (int dy = -1; dy <= 1; ++dy) {
                    [loop] for (int dx = -1; dx <= 1; ++dx) {
                        int2 nc = clamp(int2(cell_xy) + int2(dx, dy), int2(0, 0), int2(7, 7));
                        float4 nv = gs_vec4_a[(uint(nc.y) << 3u) | uint(nc.x)];
                        if (nv.w > 0.0f) {
                            n_rad += nv.rgb;
                            n_hit += nv.w;
                            n_cnt += 1.0f;
                        }
                    }
                }
                if (n_cnt > 0.0f) {
                    out_cell = float4(n_rad / n_cnt, clamp(n_hit / n_cnt, 1e-3f, 500.0f));
                } else {
                    Texture2D<uint> gn_tex = ResourceDescriptorHeap[atelier_safe_desc(g_geom_normal_srv)];
                    uint2 seed_px = uint2(seed & 0xFFFFu, seed >> 16u);
                    float3 qN = unpack_oct_snorm16(gn_tex.Load(int3(seed_px, 0)));
                    out_cell = float4(eval_atelier_sky_diffuse_radiance(qN, fcb), 16.0f);
                }
            }
        } else {
            Texture2D<float> depth_tex = ResourceDescriptorHeap[atelier_safe_desc(g_depth_srv)];
            Texture2D<uint> gn_tex = ResourceDescriptorHeap[atelier_safe_desc(g_geom_normal_srv)];
            uint2 seed_px = uint2(seed & 0xFFFFu, seed >> 16u);
            float3 qN = unpack_oct_snorm16(gn_tex.Load(int3(seed_px, 0)));
            float3 qP = gi_reconstruct_world(float2(seed_px) + 0.5f, depth_tex.Load(int3(seed_px, 0)), fcb);
            float3 fb_rad = eval_atelier_sky_diffuse_radiance(qN, fcb);
            if (fcb.wrc_curr_srv != 0xFFFFFFFFu && fcb.wrc_hash_keys_srv != 0xFFFFFFFFu && fcb.wrc_voxel_keys_srv != 0xFFFFFFFFu) {
                StructuredBuffer<uint> hk = ResourceDescriptorHeap[atelier_safe_desc(fcb.wrc_hash_keys_srv)];
                StructuredBuffer<uint2> vk = ResourceDescriptorHeap[atelier_safe_desc(fcb.wrc_voxel_keys_srv)];
                StructuredBuffer<uint4> wc = ResourceDescriptorHeap[atelier_safe_desc(fcb.wrc_curr_srv)];
                float3 wrc_ind = 0.0f;
                if (sharc_query_resolved_srv(hk, vk, wc, 1u, qP, qN, fcb.camera_eye, wrc_ind))
                    fb_rad = clamp(wrc_ind, 0.0f, 32.0f);
            }
            out_cell = float4(fb_rad, 16.0f);
        }

        atlas_uav[atlas_px] = out_cell;
        return;
    }

    // ========================================================================
    // Mode 8: Layer 1 — GI-1.2 `PopulateProbeMaskMip` (Odd-Extent-Safe 2x2 Seed Downsample)
    // ========================================================================
    if (g_gi_mode == 8u) {
        uint2 dst_pos = dtid.xy;
        if (dst_pos.x >= g_uav2 || dst_pos.y >= g_uav3)
            return;

        Texture2D<uint> src_mask = ResourceDescriptorHeap[atelier_safe_desc(g_srv_aux0)];
        RWTexture2D<uint> dst_mask = ResourceDescriptorHeap[atelier_safe_desc(g_uav0)];

        uint max_dx = ((dst_pos.x + 1u == g_uav2) && ((g_srv_aux1 & 1u) != 0u)) ? 3u : 2u;
        uint max_dy = ((dst_pos.y + 1u == g_uav3) && ((g_srv_aux2 & 1u) != 0u)) ? 3u : 2u;
        uint rot = (dst_pos.x + dst_pos.y * 3u + fcb.frame_index) & 3u;
        uint picked = 0xFFFFFFFFu;

        [loop] for (uint iy = 0u; iy < max_dy && picked == 0xFFFFFFFFu; ++iy) {
            [loop] for (uint ix = 0u; ix < max_dx && picked == 0xFFFFFFFFu; ++ix) {
                uint sx = (max_dx == 2u && max_dy == 2u) ? (((ix + (iy << 1u) + rot) & 1u)) : ix;
                uint sy = (max_dx == 2u && max_dy == 2u) ? ((((ix + (iy << 1u) + rot) >> 1u) & 1u)) : iy;
                uint2 sp = min(dst_pos * 2u + uint2(sx, sy), uint2(g_srv_aux1 - 1u, g_srv_aux2 - 1u));
                uint cand = src_mask.Load(int3(sp, 0));
                if (cand != 0xFFFFFFFFu)
                    picked = cand;
            }
        }
        dst_mask[dst_pos] = picked;
        return;
    }

    // ========================================================================
    // Mode 9: Layer 1 — GI-1.2 `FilterProbes` (Separable 7x1 + 1x7 Hit-Parallax & Angle-Backoff Filter)
    // ========================================================================
    if (g_gi_mode == 9u) {
        uint2 probes_dim = uint2((fcb.render_extent.x + 7u) / 8u, (fcb.render_extent.y + 7u) / 8u);
        uint2 probe_coord = gid.xy;
        bool in_grid = (probe_coord.x < probes_dim.x && probe_coord.y < probes_dim.y);
        uint2 cell_xy = uint2(gindex & 7u, gindex >> 3u);
        uint2 atlas_px = probe_coord * 8u + cell_xy;

        Texture2D<float> depth_tex = ResourceDescriptorHeap[atelier_safe_desc(g_depth_srv)];
        Texture2D<uint> gn_tex = ResourceDescriptorHeap[atelier_safe_desc(g_geom_normal_srv)];
        Texture2D<float4> atlas_in = ResourceDescriptorHeap[atelier_safe_desc(g_srv_aux0)];
        Texture2D<uint> mask_tex = ResourceDescriptorHeap[atelier_safe_desc(g_srv_aux1)];
        RWTexture2D<float4> atlas_out = ResourceDescriptorHeap[atelier_safe_desc(g_uav0)];

        if (gindex == 0u) {
            gs_probe_flags = 0u;
            if (in_grid) {
                uint seed_c = mask_tex.Load(int3(probe_coord, 0));
                if (seed_c != 0xFFFFFFFFu) {
                    uint2 c_px = uint2(seed_c & 0xFFFFu, seed_c >> 16u);
                    float c_dz = depth_tex.Load(int3(c_px, 0));
                    if (c_dz > 0.0f) {
                        gs_probe_flags = 1u;
                        gs_anchor_P = gi_reconstruct_world(float2(c_px) + 0.5f, c_dz, fcb);
                        gs_anchor_N = unpack_oct_snorm16(gn_tex.Load(int3(c_px, 0)));
                    }
                }
            }
        }
        GroupMemoryBarrierWithGroupSync();

        bool center_valid = (gs_probe_flags != 0u);
        if (!in_grid)
            return;
        if (!center_valid) {
            atlas_out[atlas_px] = 0.0f;
            return;
        }
        float3 P_c = gs_anchor_P;
        float3 N_c = gs_anchor_N;

        if (center_valid && gindex < 6u) {
            int k_off = (gindex < 3u) ? (int(gindex) - 3) : (int(gindex) - 2);
            int2 step_dir = (g_srv_aux2 == 0u) ? int2(1, 0) : int2(0, 1);
            int2 q_pc = int2(probe_coord) + step_dir * k_off;
            float w_nbr = 0.0f;
            float3 P_n = P_c;
            float3 N_n = N_c;
            uint2 u_qpc = probe_coord;

            if (q_pc.x >= 0 && q_pc.y >= 0 && q_pc.x < int(probes_dim.x) && q_pc.y < int(probes_dim.y)) {
                uint seed_n = mask_tex.Load(int3(q_pc, 0));
                if (seed_n != 0xFFFFFFFFu) {
                    uint2 n_px = uint2(seed_n & 0xFFFFu, seed_n >> 16u);
                    float n_dz = depth_tex.Load(int3(n_px, 0));
                    if (n_dz > 0.0f) {
                        P_n = gi_reconstruct_world(float2(n_px) + 0.5f, n_dz, fcb);
                        N_n = unpack_oct_snorm16(gn_tex.Load(int3(n_px, 0)));
                        float cell_sz = 8.0f * gi_pixel_world_size(P_c, fcb);
                        float ndot = dot(N_c, N_n);
                        float plane_d = max(abs(dot(P_n - P_c, N_c)), abs(dot(P_c - P_n, N_n)));
                        float rad_d = length(P_n - P_c);
                        if (ndot >= 0.78f && plane_d <= max(0.04f, cell_sz * 0.65f) &&
                            rad_d <= max(0.25f, cell_sz * (float(abs(k_off)) * 2.2f))) {
                            w_nbr = exp(-0.20f * float(k_off * k_off)) * pow(saturate(ndot), 8.0f) *
                                    exp(-plane_d / max(0.02f, cell_sz * 0.30f));
                            u_qpc = uint2(q_pc);
                        }
                    }
                }
            }
            gs_nbr_w[gindex] = w_nbr;
            gs_nbr_P[gindex] = P_n;
            gs_nbr_N[gindex] = N_n;
            gs_nbr_pc[gindex] = u_qpc;
        }
        GroupMemoryBarrierWithGroupSync();

        if (!in_grid)
            return;
        if (!center_valid) {
            atlas_out[atlas_px] = 0.0f;
            return;
        }

        float4 c_cell = atlas_in.Load(int3(atlas_px, 0));
        float hit_c = clamp(c_cell.w, 1e-2f, 500.0f);
        float3 dir_c = atelier_local_to_world(hemi_oct_decode((float2(cell_xy) + 0.5f) * 0.125f), N_c);

        float3 rad_sum = c_cell.rgb;
        float hit_sum = hit_c;
        float w_sum = 1.0f;

        [loop] for (uint ni = 0u; ni < 6u; ++ni) {
            float bw = gs_nbr_w[ni];
            if (bw <= 1e-4f)
                continue;
            float3 N_n = gs_nbr_N[ni];
            float3 nl_chk = atelier_world_to_local(dir_c, N_n);
            if (nl_chk.z <= 1e-4f)
                continue;

            uint2 n_pc = gs_nbr_pc[ni];
            uint2 n_c0 = min(uint2(hemi_oct_encode(nl_chk) * 8.0f), 7u);
            float4 n_val0 = atlas_in.Load(int3(n_pc * 8u + n_c0, 0));
            if (n_val0.w <= 0.0f)
                continue;

            float clamped_hit = min(hit_c, max(n_val0.w, 1e-2f));
            float3 P_hit = P_c + dir_c * clamped_hit;
            float3 dir_n = safe_unit_normal(P_hit - gs_nbr_P[ni]);
            float parallax_w = smoothstep(0.75f, 0.965f, dot(dir_c, dir_n));
            if (parallax_w <= 1e-4f)
                continue;

            float3 nl_fin = atelier_world_to_local(dir_n, N_n);
            if (nl_fin.z <= 1e-4f)
                continue;

            uint2 n_cf = min(uint2(hemi_oct_encode(nl_fin) * 8.0f), 7u);
            float4 n_val = atlas_in.Load(int3(n_pc * 8u + n_cf, 0));
            if (n_val.w <= 0.0f)
                continue;

            float w = bw * parallax_w;
            rad_sum += n_val.rgb * w;
            hit_sum += n_val.w * w;
            w_sum += w;
        }

        atlas_out[atlas_px] = float4(rad_sum / w_sum, clamp(hit_sum / w_sum, 1e-3f, 500.0f));
        return;
    }

    // ========================================================================
    // Mode 10: Layer 1 — GI-1.2 `ProjectScreenProbes` (Exact White-Furnace Orthonormal SH2)
    // ========================================================================
    if (g_gi_mode == 10u) {
        uint2 probes_dim = uint2((fcb.render_extent.x + 7u) / 8u, (fcb.render_extent.y + 7u) / 8u);
        uint2 probe_coord = gid.xy;
        bool in_grid = (probe_coord.x < probes_dim.x && probe_coord.y < probes_dim.y);
        uint probe_idx = probe_coord.y * probes_dim.x + probe_coord.x;
        uint2 cell_xy = uint2(gindex & 7u, gindex >> 3u);

        Texture2D<float> depth_tex = ResourceDescriptorHeap[atelier_safe_desc(g_depth_srv)];
        Texture2D<uint> gn_tex = ResourceDescriptorHeap[atelier_safe_desc(g_geom_normal_srv)];
        Texture2D<float4> atlas_in = ResourceDescriptorHeap[atelier_safe_desc(g_srv_aux0)];
        Texture2D<uint> mask_tex = ResourceDescriptorHeap[atelier_safe_desc(g_srv_aux1)];
        RWStructuredBuffer<PackedScreenProbeSH2> sh_out = ResourceDescriptorHeap[atelier_safe_desc(g_uav0)];

        if (gindex == 0u) {
            gs_probe_flags = 0u;
            gs_anchor_N = float3(0.0f, 1.0f, 0.0f);
            gs_anchor_P = 0.0f;
            if (in_grid) {
                uint seed = mask_tex.Load(int3(probe_coord, 0));
                if (seed != 0xFFFFFFFFu) {
                    gs_probe_flags = 1u;
                    uint2 seed_px = uint2(seed & 0xFFFFu, seed >> 16u);
                    gs_anchor_N = unpack_oct_snorm16(gn_tex.Load(int3(seed_px, 0)));
                    float s_dz = (g_depth_srv != 0xFFFFFFFFu) ? depth_tex.Load(int3(seed_px, 0)) : 0.0f;
                    gs_anchor_P = (s_dz > 0.0f) ? gi_reconstruct_world(float2(seed_px) + 0.5f, s_dz, fcb) : fcb.camera_eye;
                }
            }
        }
        GroupMemoryBarrierWithGroupSync();

        bool valid_probe = (gs_probe_flags != 0u);
        if (!in_grid)
            return;
        if (!valid_probe) {
            if (gindex == 0u) {
                PackedScreenProbeSH2 zero_rec = (PackedScreenProbeSH2)0;
                sh_out[probe_idx] = zero_rec;
            }
            return;
        }
        if (valid_probe) {
            float4 cell = atlas_in.Load(int3(probe_coord * 8u + cell_xy, 0));
            float3 local_d = hemi_oct_decode((float2(cell_xy) + 0.5f) * 0.125f);
            float probe_spacing_ws = 8.0f * gi_pixel_world_size(gs_anchor_P, fcb);
            float ao_r = gtvbao_effective_radius(fcb.ao_radius, probe_spacing_ws, (fcb.flags & 2u) != 0u);
            float cell_open = saturate((cell.w - ao_r * 0.83f) / max(ao_r * 0.35f, 1e-3f));
            float open = cell_open * local_d.z * (ATELIER_PROBE_CELL_SH_WEIGHT * ATELIER_INV_PI);
            float3 dir = atelier_local_to_world(local_d, gs_anchor_N);
            gs_vec4_a[gindex] = float4(max(cell.rgb, 0.0f) * ATELIER_PROBE_CELL_SH_WEIGHT, open);
            gs_vec3_dir[gindex] = dir;
        }
        GroupMemoryBarrierWithGroupSync();

        if (valid_probe && gindex < 9u) {
            float3 sum_m = 0.0f;
            [loop] for (uint k = 0u; k < 64u; ++k) {
                float y_c[9];
                sh2_get_coefficients(gs_vec3_dir[k], y_c);
                sum_m += gs_vec4_a[k].rgb * y_c[gindex];
            }
            gs_sh_out[gindex] = sum_m;
        }
        GroupMemoryBarrierWithGroupSync();

        if (in_grid && gindex == 0u) {
            if (!valid_probe) {
                PackedScreenProbeSH2 zero_rec = (PackedScreenProbeSH2)0;
                sh_out[probe_idx] = zero_rec;
            } else {
                float3 sh_c[9];
                [unroll] for (uint m = 0u; m < 9u; ++m)
                    sh_c[m] = gs_sh_out[m];
                float avg_vis = 0.0f;
                [loop] for (uint k = 0u; k < 64u; ++k)
                    avg_vis += gs_vec4_a[k].w;
                PackedScreenProbeSH2 packed_rec;
                pack_probe_sh2_record(sh_c, avg_vis, gs_anchor_N, 1u, packed_rec);
                sh_out[probe_idx] = packed_rec;
            }
        }
        return;
    }

    // ========================================================================
    // Mode 11: GI-1.2 Separable `FilterGI` (`g_srv_aux2`: 0 = Horizontal, 1 = Vertical + Bent-Cone Coupling)
    // ========================================================================
    if (g_gi_mode == 11u) {
        uint2 px = dtid.xy + tile8_px_off;
        if (px.x >= fcb.render_extent.x || px.y >= fcb.render_extent.y)
            return;

        RWTexture2D<float4> dst_uav = ResourceDescriptorHeap[atelier_safe_desc(g_uav0)];
        float2 pxc = float2(px) + 0.5f;
        Texture2D<float> depth_tex = ResourceDescriptorHeap[atelier_safe_desc(g_depth_srv)];
        float dz = gi_px_in_viewport(px, fcb.scene_viewport) ? depth_tex.Load(int3(px, 0)) : 0.0f;
        if (dz <= 0.0f) {
            dst_uav[px] = 0.0f;
            return;
        }

        Texture2D<float4> gi_in = ResourceDescriptorHeap[atelier_safe_desc(g_srv_aux0)];
        Texture2D<float2> bv_tex = ResourceDescriptorHeap[atelier_safe_desc(g_srv_aux1)];
        Texture2D<uint2> slab0_tex = ResourceDescriptorHeap[atelier_safe_desc(g_slab0_srv)];
        Texture2D<uint> gn_tex = ResourceDescriptorHeap[atelier_safe_desc(g_geom_normal_srv)];

        float4 center_gi = gi_in.Load(int3(px, 0));
        float2 bv = bv_tex.Load(int3(px, 0));
        float blur_mask = bv.x;
        float interp_probe_vis = bv.y;

        float3 P = gi_reconstruct_world(pxc, dz, fcb);
        float px_ws = gi_pixel_world_size(P, fcb);
        float3 GN = unpack_oct_snorm16(gn_tex.Load(int3(px, 0)));
        float3 N = unpack_openpbr_slab(slab0_tex.Load(int3(px, 0)), uint2(0u, 0u)).shading_normal;

        float3 filtered_gi = center_gi.rgb;
        if ((fcb.flags & 2u) != 0u && blur_mask > 0.01f) {
            int step_px = (blur_mask > 0.55f) ? 2 : 1;
            int2 axis = (g_srv_aux2 == 0u) ? int2(step_px, 0) : int2(0, step_px);
            float sigma2 = lerp(2.5f, 10.0f, blur_mask);
            float3 sum_gi = center_gi.rgb;
            float w_sum = 1.0f;

            [loop] for (int k = -4; k <= 4; ++k) {
                if (k == 0)
                    continue;
                int2 q = clamp(int2(px) + axis * k, fcb.scissor_rect.xy, max(fcb.scissor_rect.xy, fcb.scissor_rect.zw - 1));
                if (!gi_px_in_viewport(uint2(q), fcb.scene_viewport))
                    continue;
                float qz = depth_tex.Load(int3(q, 0));
                if (qz <= 0.0f)
                    continue;
                float3 Q = gi_reconstruct_world(float2(q) + 0.5f, qz, fcb);
                float3 qGN = unpack_oct_snorm16(gn_tex.Load(int3(q, 0)));
                float3 qN = unpack_openpbr_slab(slab0_tex.Load(int3(q, 0)), uint2(0u, 0u)).shading_normal;

                float plane_d = max(abs(dot(Q - P, GN)), abs(dot(P - Q, qGN)));
                float wz = exp(-plane_d / max(0.015f, px_ws * float(abs(k) * step_px) * 1.8f));
                float wn = pow(saturate(dot(N, qN)), 12.0f) * pow(saturate(dot(GN, qGN)), 4.0f);
                float wk = exp(-float(k * k) / sigma2);
                float w = wk * wz * wn;
                if (w <= 1e-4f)
                    continue;
                sum_gi += gi_in.Load(int3(q, 0)).rgb * w;
                w_sum += w;
            }
            filtered_gi = sum_gi / w_sum;
        }

        if (g_srv_aux2 == 0u) {
            dst_uav[px] = float4(clamp(filtered_gi, 0.0f, 65504.0f), center_gi.a);
            return;
        }

        // Cold/disoccluded history bootstrap (`sample_count < 6`) without recursive steady-state diffusion:
        if ((fcb.flags & 2u) != 0u && center_gi.a > 0.0f && center_gi.a < 6.0f && g_uav1 != 0xFFFFFFFFu) {
            RWTexture2D<float4> hist_uav = ResourceDescriptorHeap[atelier_safe_desc(g_uav1)];
            hist_uav[px] = float4(clamp(filtered_gi, 0.0f, 65504.0f), center_gi.a);
        }

        float3 bent_w = GN;
        float bent_vis = 1.0f;
        float3 micro_gi = 0.0f;
        bool ao_enabled = ((fcb.flags & (1u << 2u)) != 0u) && (g_prev_diffuse_srv != 0xFFFFFFFFu);
        if (ao_enabled) {
            Texture2D<float4> ssilvb = ResourceDescriptorHeap[atelier_safe_desc(g_prev_diffuse_srv)];
            float4 ao_val = ssilvb.Load(int3(px, 0));
            bent_w = safe_unit_normal(ao_val.xyz);
            bent_vis = saturate(ao_val.w);
            if (g_hzb_srv != 0xFFFFFFFFu) {
                Texture2D<float4> ssilvb_gi = ResourceDescriptorHeap[atelier_safe_desc(g_hzb_srv)];
                micro_gi = ssilvb_gi.Load(int3(px, 0)).rgb;
            }
        }

        // Full-res Shading-Normal x Bent-Cone directional coupling + tangent-horizon micro-crevice visibility:
        float nm_horizon_vis = ao_enabled ? lerp(1.0f, 0.5f + 0.5f * saturate(dot(N, GN)), 0.75f) : 1.0f;
        float cone_align = (0.5f + 0.5f * dot(N, bent_w)) / max(0.5f + 0.5f * dot(GN, bent_w), 0.35f);
        float dir_cone_mod = ao_enabled ? lerp(1.0f, clamp(cone_align, 0.55f, 1.12f), saturate((1.0f - bent_vis) * 1.4f)) : 1.0f;

        if ((fcb.flags & 2u) == 0u) {
            float env_ao = clamp(bent_vis * dir_cone_mod * nm_horizon_vis, 0.0f, 1.15f);
            float3 env_irrad = filtered_gi + micro_gi * (1.0f - bent_vis);
            dst_uav[px] = float4(clamp(env_irrad, 0.0f, 65504.0f), env_ao);
            return;
        }

        float vis_ratio = ao_enabled ? clamp(bent_vis / max(interp_probe_vis, 0.08f), 0.0f, 1.18f) : 1.0f;
        float effective_ao = clamp(vis_ratio * dir_cone_mod * nm_horizon_vis, 0.0f, 1.18f);
        float3 final_gi = filtered_gi + micro_gi * (1.0f - bent_vis);
        dst_uav[px] = float4(clamp(final_gi, 0.0f, 65504.0f), effective_ao);
        return;
    }

    // ========================================================================
    // Mode 5: GI-1.2 `InterpolateScreenProbes` (±6px Coplanar Jitter + Mip `FindClosestProbe`) + `ReprojectGI`
    // ========================================================================
    if (gindex == 0u && ((fcb.flags & (1u << 7u)) != 0u))
        gs_inv_prev_vp = gi_invert_4x4(fcb.prev_unjittered_vp, fcb.inv_unjittered_vp);
    GroupMemoryBarrierWithGroupSync();

    uint2 px = dtid.xy + tile8_px_off;
    if (px.x >= fcb.render_extent.x || px.y >= fcb.render_extent.y)
        return;

    RWTexture2D<float4> gi_hist_out = ResourceDescriptorHeap[atelier_safe_desc(g_uav0)];
    RWTexture2D<float> gi_delta_out = ResourceDescriptorHeap[atelier_safe_desc(g_uav1)];
    RWTexture2D<float2> gi_bv_out = ResourceDescriptorHeap[atelier_safe_desc(g_uav2)];

    float2 pxc = float2(px) + 0.5f;
    Texture2D<float> depth_tex = ResourceDescriptorHeap[atelier_safe_desc(g_depth_srv)];
    float dz = gi_px_in_viewport(px, fcb.scene_viewport) ? depth_tex.Load(int3(px, 0)) : 0.0f;
    if (dz <= 0.0f) {
        gi_hist_out[px] = 0.0f;
        gi_delta_out[px] = 0.0f;
        gi_bv_out[px] = float2(0.0f, 1.0f);
        return;
    }

    Texture2D<uint2> slab0_tex = ResourceDescriptorHeap[atelier_safe_desc(g_slab0_srv)];
    float3 N = unpack_openpbr_slab(slab0_tex.Load(int3(px, 0)), uint2(0u, 0u)).shading_normal;

    if ((fcb.flags & 2u) == 0u) {
        float3 env_irr = eval_atelier_sky_diffuse_radiance(N, fcb);
        gi_hist_out[px] = float4(env_irr, 32.0f);
        gi_delta_out[px] = 0.0f;
        gi_bv_out[px] = float2(0.0f, 1.0f);
        return;
    }

    float3 P = gi_reconstruct_world(pxc, dz, fcb);
    float px_ws = gi_pixel_world_size(P, fcb);
    float cell_ws = 8.0f * px_ws;
    Texture2D<uint> gn_tex = ResourceDescriptorHeap[atelier_safe_desc(g_geom_normal_srv)];
    float3 GN = unpack_oct_snorm16(gn_tex.Load(int3(px, 0)));

    float3 bent_w = GN;
    float bent_vis = 1.0f;
    if ((fcb.flags & (1u << 2u)) != 0u && g_uav3 != 0xFFFFFFFFu) {
        Texture2D<float4> ssilvb = ResourceDescriptorHeap[atelier_safe_desc(g_uav3)];
        float4 ao = ssilvb.Load(int3(px, 0));
        bent_w = safe_unit_normal(ao.xyz);
        bent_vis = saturate(ao.w);
    }
    // Preserve full-resolution normal-map detail on `N` while bending the sampling lobe toward open space `(bent_w - GN)`:
    float3 N_eval = safe_unit_normal(N + (bent_w - GN) * ((1.0f - bent_vis) * 0.65f));

    // Coplanar ±6px blue-noise pixel jitter dissolves 8x8 probe grid boundaries without bleeding across corners:
    uint temporal_idx = ((fcb.flags & (1u << 7u)) != 0u) ? fcb.frame_index : 0u;
    float2 u_jit = atelier_blue_jitter2(px, temporal_idx, 0x243F6A88u) * 2.0f - 1.0f;
    float2 sample_px = float2(px);
    int2 j_px = clamp(int2(round(float2(px) + u_jit * 6.0f)), fcb.scissor_rect.xy, max(fcb.scissor_rect.xy, fcb.scissor_rect.zw - 1));
    if (gi_px_in_viewport(uint2(j_px), fcb.scene_viewport)) {
        float j_dz = depth_tex.Load(int3(j_px, 0));
        if (j_dz > 0.0f) {
            float3 j_GN = unpack_oct_snorm16(gn_tex.Load(int3(j_px, 0)));
            float3 j_P = gi_reconstruct_world(float2(j_px) + 0.5f, j_dz, fcb);
            if (dot(j_GN, GN) >= 0.90f && abs(dot(j_P - P, GN)) <= max(0.02f, px_ws * 2.5f))
                sample_px = float2(px) + u_jit * 6.0f;
        }
    }

    uint2 probes_dim = uint2((fcb.render_extent.x + 7u) / 8u, (fcb.render_extent.y + 7u) / 8u);
    Texture2D<uint> probe_mask_tex = ResourceDescriptorHeap[atelier_safe_desc(g_srv_aux2)];
    uint pm_w = 1u, pm_h = 1u, pm_mips = 1u;
    probe_mask_tex.GetDimensions(0u, pm_w, pm_h, pm_mips);

    float2 probe_f = clamp((sample_px - 3.5f) * 0.125f, 0.0f, float2(max(probes_dim, 1u) - 1u));
    uint2 base_p = min(uint2(floor(probe_f)), max(probes_dim, 1u) - 1u);
    float2 frac_p = saturate(probe_f - float2(base_p));
    StructuredBuffer<PackedScreenProbeSH2> probe_sh_buf = ResourceDescriptorHeap[atelier_safe_desc(g_srv_aux0)];

    float3 irrad_sum = 0.0f;
    float probe_vis_sum = 0.0f;
    float w_total = 0.0f;

    [loop] for (int oy = 0; oy <= 1; ++oy) {
        [loop] for (int ox = 0; ox <= 1; ++ox) {
            uint seed = find_closest_probe_seed(base_p, int2(ox, oy), probe_mask_tex, probes_dim, pm_mips);
            if (seed == 0xFFFFFFFFu)
                continue;
            uint2 seed_px = uint2(seed & 0xFFFFu, seed >> 16u);
            uint2 pc = min(seed_px >> 3u, probes_dim - 1u);

            float3 sh[9];
            float p_vis;
            float3 qN;
            uint p_valid;
            unpack_probe_sh2_record(probe_sh_buf[pc.y * probes_dim.x + pc.x], sh, p_vis, qN, p_valid);
            if (p_valid == 0u)
                continue;

            float adz = depth_tex.Load(int3(seed_px, 0));
            if (adz <= 0.0f)
                continue;
            float3 Q = gi_reconstruct_world(float2(seed_px) + 0.5f, adz, fcb);

            float bw = (ox == 0 ? (1.0f - frac_p.x) : frac_p.x) * (oy == 0 ? (1.0f - frac_p.y) : frac_p.y);
            bool direct_cell = all(pc == min(base_p + uint2(ox, oy), probes_dim - 1u));
            float base_w = direct_cell ? max(bw, 0.02f) : max(bw * 0.25f, 0.01f);
            float plane_d = max(abs(dot(Q - P, GN)), abs(dot(P - Q, qN))) / max(0.03f, cell_ws * 0.55f);
            float radial_d = length(Q - P) / max(0.12f, cell_ws * 2.5f);
            float normal_w = pow(saturate(dot(GN, qN) * 0.5f + 0.5f), 4.0f);
            float geom_w = base_w * normal_w * exp(-plane_d * 2.5f - radial_d * 0.5f);
            if (geom_w <= 1e-5f)
                continue;

            irrad_sum += eval_probe_sh2_irradiance(sh, N_eval) * geom_w;
            probe_vis_sum += p_vis * geom_w;
            w_total += geom_w;
        }
    }

    float3 curr_gi = 0.0f;
    float interp_probe_vis = 1.0f;
    if (w_total > 1e-5f) {
        curr_gi = irrad_sum / w_total;
        interp_probe_vis = probe_vis_sum / w_total;
    } else if (fcb.wrc_curr_srv != 0xFFFFFFFFu && fcb.wrc_hash_keys_srv != 0xFFFFFFFFu &&
               fcb.wrc_voxel_keys_srv != 0xFFFFFFFFu) {
        StructuredBuffer<uint> hash_keys = ResourceDescriptorHeap[atelier_safe_desc(fcb.wrc_hash_keys_srv)];
        StructuredBuffer<uint2> voxel_keys = ResourceDescriptorHeap[atelier_safe_desc(fcb.wrc_voxel_keys_srv)];
        StructuredBuffer<uint4> wrc_curr = ResourceDescriptorHeap[atelier_safe_desc(fcb.wrc_curr_srv)];
        float3 wrc_ind = 0.0f;
        if (sharc_query_resolved_srv(hash_keys, voxel_keys, wrc_curr, 1u, P, GN, fcb.camera_eye, wrc_ind))
            curr_gi = wrc_ind;
    }

    // Grazing-Safe Subpixel `ReprojectGI` + `color_delta` Adaptive Temporal Accumulation (`Capsaicin GI-1.2 gi_denoiser.hlsl`):
    float hist_count = 1.0f;
    float next_delta = 0.0f;
    float3 accum_gi = curr_gi;

    if ((fcb.flags & (1u << 7u)) != 0u && g_srv_aux1 != 0xFFFFFFFFu && g_prev_diffuse_srv != 0xFFFFFFFFu &&
        g_hzb_srv != 0xFFFFFFFFu && g_slab1_srv != 0xFFFFFFFFu) {
        Texture2D<float2> mot_tex = ResourceDescriptorHeap[atelier_safe_desc(g_motion_srv)];
        float2 mot = mot_tex.Load(int3(px, 0));
        float2 prev_px_f = (pxc - fcb.jitter_pixels) + mot * float2(fcb.render_extent);

        if (prev_px_f.x >= fcb.prev_scene_viewport.x && prev_px_f.x < fcb.prev_scene_viewport.x + fcb.prev_scene_viewport.z &&
            prev_px_f.y >= fcb.prev_scene_viewport.y && prev_px_f.y < fcb.prev_scene_viewport.y + fcb.prev_scene_viewport.w) {
            float4 cam_clip = mul(float4(P, 1.0f), fcb.prev_unjittered_vp);
            float2 cam_prev_px = prev_px_f;
            float cam_exp_dz = dz;
            if (abs(cam_clip.w) > 1e-5f) {
                cam_prev_px = float2(fcb.prev_scene_viewport.x + (cam_clip.x / cam_clip.w * 0.5f + 0.5f) * fcb.prev_scene_viewport.z,
                                     fcb.prev_scene_viewport.y + (0.5f - cam_clip.y / cam_clip.w * 0.5f) * fcb.prev_scene_viewport.w);
                cam_exp_dz = cam_clip.z / cam_clip.w;
            }
            bool is_dynamic_px = length(cam_prev_px - prev_px_f) > 1.5f;
            float grazing_scale = 1.0f / max(abs(dot(GN, safe_unit_normal(fcb.camera_eye - P))), 0.08f);

            Texture2D<float> prev_depth_tex = ResourceDescriptorHeap[atelier_safe_desc(g_prev_diffuse_srv)];
            Texture2D<uint> prev_gn_tex = ResourceDescriptorHeap[atelier_safe_desc(g_hzb_srv)];
            Texture2D<float4> prev_gi_tex = ResourceDescriptorHeap[atelier_safe_desc(g_srv_aux1)];
            Texture2D<float> prev_delta_tex = ResourceDescriptorHeap[atelier_safe_desc(g_slab1_srv)];

            int2 p_base = int2(floor(prev_px_f - 0.5f));
            float2 p_frac = saturate((prev_px_f - 0.5f) - float2(p_base));

            float3 p_gi_sum = 0.0f;
            float p_cnt_sum = 0.0f;
            float p_del_sum = 0.0f;
            float p_w_sum = 0.0f;

            [loop] for (int py = 0; py <= 1; ++py) {
                [loop] for (int px_o = 0; px_o <= 1; ++px_o) {
                    int2 qp = p_base + int2(px_o, py);
                    if (qp.x < 0 || qp.y < 0 || qp.x >= int(fcb.render_extent.x) || qp.y >= int(fcb.render_extent.y))
                        continue;
                    float pdz = prev_depth_tex.Load(int3(qp, 0));
                    if (pdz <= 0.0f)
                        continue;
                    float4 pgi = prev_gi_tex.Load(int3(qp, 0));
                    if (pgi.w <= 0.0f)
                        continue;
                    float3 pGN = unpack_oct_snorm16(prev_gn_tex.Load(int3(qp, 0)));
                    if (dot(pGN, GN) < 0.75f)
                        continue;

                    bool geom_match = false;
                    if (!is_dynamic_px) {
                        float3 prev_P = gi_reconstruct_prev_world(float2(qp) + 0.5f, pdz, gs_inv_prev_vp, fcb);
                        float plane_err = abs(dot(prev_P - P, GN));
                        float rad_err = length(prev_P - P);
                        geom_match = (plane_err <= max(0.03f, px_ws * 3.0f)) && (rad_err <= max(0.15f, px_ws * 8.0f * grazing_scale));
                    } else {
                        geom_match = abs(pdz - cam_exp_dz) <= max(1e-4f, max(pdz, cam_exp_dz) * 0.04f * grazing_scale);
                    }
                    if (!geom_match)
                        continue;

                    float bw = (px_o == 0 ? (1.0f - p_frac.x) : p_frac.x) * (py == 0 ? (1.0f - p_frac.y) : p_frac.y);
                    float w = max(bw, 1e-3f) * pow(saturate(dot(pGN, GN)), 4.0f);
                    p_gi_sum += (pgi.rgb * fcb.exposure_ratio) * w;
                    p_cnt_sum += pgi.w * w;
                    p_del_sum += (prev_delta_tex.Load(int3(qp, 0)) * fcb.exposure_ratio) * w;
                    p_w_sum += w;
                }
            }

            if (p_w_sum > 1e-4f) {
                float3 prev_gi = p_gi_sum / p_w_sum;
                float prev_cnt = p_cnt_sum / p_w_sum;
                float prev_del = p_del_sum / p_w_sum;

                float l_cur = dot(curr_gi, float3(0.2126f, 0.7152f, 0.0722f));
                float l_prv = dot(prev_gi, float3(0.2126f, 0.7152f, 0.0722f));
                float l_cap = max(l_prv * 5.0f, l_prv + 0.75f);
                float3 safe_curr = curr_gi;
                if (prev_cnt >= 4.0f && l_cur > l_cap)
                    safe_curr *= l_cap / max(l_cur, 1e-6f);

                float signed_diff = dot(safe_curr - prev_gi, float3(0.2126f, 0.7152f, 0.0722f));
                next_delta = lerp(prev_del, signed_diff, 0.20f);
                float ref_luma = max(max(l_cur, l_prv), 0.05f);
                float rel_change = saturate(abs(next_delta) / ref_luma * 2.5f);
                float max_samples = is_dynamic_px ? 8.0f : lerp(32.0f, 4.0f, rel_change);

                hist_count = min(prev_cnt + 1.0f, max_samples);
                float blend = 1.0f / max(hist_count, 1.0f);
                accum_gi = lerp(prev_gi, safe_curr, blend);
            }
        }
    }

    float blur_mask = saturate(1.0f - (hist_count - 1.0f) / 28.0f) * 0.75f + 0.25f;
    gi_hist_out[px] = float4(clamp(accum_gi, 0.0f, 65504.0f), hist_count);
    gi_delta_out[px] = clamp(next_delta, -65504.0f, 65504.0f);
    gi_bv_out[px] = float2(blur_mask, interp_probe_vis);
}
