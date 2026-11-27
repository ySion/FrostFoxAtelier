// Deterministic 16x16x32 3D Frustum + Z-Bitmask Clustered Direct Lighting + OpenPBR v1.1 6-Profile BRDF + LTC
// Modes:
//   0: Build 16x16 Tile Light List (256-thread parallel Gribb-Hartmann 2D frustum + 32-bit Z-Bitmask)
//   1: Evaluate Direct Lighting (Diffuse + Specular separated so Diffuse feeds PrevDiffuseEmissiveRadiance & SSSSS!)
//   2: Indirect-gated Jiménez Separable Subsurface Scattering (SSSSS Horizontal / Vertical)
//   3: Optional Receiver Light-Support Histogram Diagnostic (`direct_cluster_inspect`)

cbuffer DirectLightRootConstants : register(b0, space0) {
    uint g_frame_cb_srv;
    uint g_direct_mode;
    uint g_depth_srv;
    uint g_slab0_srv;
    uint g_slab1_srv;
    uint g_geom_normal_srv;
    uint g_shadow_mask_srv;
    uint g_contact_shadow_srv;
    uint g_ssilvb_srv;
    uint g_cluster_list_uav_or_srv;
    uint g_direct_diff_uav;
    uint g_direct_spec_uav;
    uint g_cluster_stats_uav;
    uint g_penumbra_mask_srv;
};

float3 eval_schlick_f82(float cos_theta, float3 f0, float3 f82_tint) {
    float x = saturate(1.0f - cos_theta);
    float x2 = x * x;
    float x5 = x2 * x2 * x;
    float3 f_schlick = f0 + (1.0f - f0) * x5;
    if (all(f82_tint >= 0.999f))
        return f_schlick;
    // OpenPBR v1.1 §3.4 Lazanyi-Schlick F82 edge tint:
    const float mu_bar = 1.0f / 7.0f;
    const float om_bar = 6.0f / 7.0f;
    const float om_bar5 = om_bar * om_bar * om_bar * om_bar * om_bar;
    const float om_bar6 = om_bar5 * om_bar;
    float3 f_bar = f0 + (1.0f - f0) * om_bar5;
    float3 b = (f_bar * (1.0f - f82_tint)) / max(mu_bar * om_bar6, 1e-5f);
    return saturate(f_schlick - b * cos_theta * x5 * x);
}

// Belcour & Barla 2017 analytical thin-film spectral phase interference (OpenPBR Profile 4: Iridescent):
float3 eval_thin_film_iridescence(float cos_theta1, float d_nm, float eta_2, float3 base_f0) {
    float sin2_t1 = max(0.0f, 1.0f - cos_theta1 * cos_theta1);
    float sin2_t2 = sin2_t1 / max(eta_2 * eta_2, 1.0f);
    if (sin2_t2 >= 1.0f) return 1.0f;
    float cos_t2 = sqrt(1.0f - sin2_t2);
    // Optical path difference in nanometers:
    float opd = 2.0f * eta_2 * d_nm * cos_t2;
    // RGB representative wavelengths (650nm, 550nm, 450nm):
    float3 inv_lambda = float3(1.0f / 650.0f, 1.0f / 550.0f, 1.0f / 450.0f);
    float3 phase = 2.0f * ATELIER_PI * opd * inv_lambda + ATELIER_PI;
    float r12 = ((1.0f - eta_2) / (1.0f + eta_2));
    r12 = r12 * r12;
    float f12 = r12 + (1.0f - r12) * pow(1.0f - cos_theta1, 5.0f);
    float3 f23 = base_f0 + (1.0f - base_f0) * pow(1.0f - cos_t2, 5.0f);
    float3 num = f12 + f23 + 2.0f * sqrt(f12 * f23) * cos(phase);
    float3 den = 1.0f + f12 * f23 + 2.0f * sqrt(f12 * f23) * cos(phase);
    return saturate(num / max(den, 1e-4f));
}

// OpenPBR v1.1 Energy-Preserving Oren-Nayar (EON) diffuse factor:
float eval_eon_diffuse(float nl, float nv, float lv, float rough) {
    if (rough <= 1e-3f)
        return ATELIER_INV_PI;
    float s = lv - nl * nv;
    float stinv = (s > 0.0f) ? (s / max(max(nl, nv), 1e-4f)) : s;
    float A = 1.0f / (1.0f + (0.5f - 2.0f / (3.0f * ATELIER_PI)) * rough);
    float B = rough * A;
    return max(0.0f, ATELIER_INV_PI * (A + B * stinv));
}

// NaN-free Chan 2018 Aperture Micro-Shadow (Step 5 §2.2.3):
float eval_chan_micro_shadow(float nl, float v_eff) {
    if (v_eff >= 0.999f)
        return 1.0f;
    float cos_ap = sqrt(saturate(1.0f - v_eff));
    float ms = saturate((nl - cos_ap + 0.15f) / max(1.0f - cos_ap, 0.15f));
    return ms * ms;
}

// Heitz 2016 LTC Vector Edge Integral on S^2:
float3 ltc_integrate_edge_vec(float3 v1, float3 v2) {
    float x = clamp(dot(v1, v2), -0.999999f, 1.0f);
    float y = abs(x);
    float a = 0.8543985f + (0.4965155f + 0.0145206f * y) * y;
    float b = 3.4175940f + (4.1616724f + y) * y;
    float v = a / b;
    float theta_sintheta = (x > 0.0f) ? v : (0.5f * rsqrt(max(1.0f - x * x, 1e-7f)) - v);
    return cross(v1, v2) * theta_sintheta;
}

float3 safe_s2_vertex(float3 v) {
    float len2 = dot(v, v);
    return (len2 > 1e-12f) ? (v * rsqrt(len2)) : float3(0.0f, 0.0f, 1.0f);
}

// Streaming Sutherland-Hodgman clip of convex planar quad L[0..3] to z >= 0 on S^2 (zero VGPR array, zero NaN):
float3 ltc_clipped_polygon_vector(float3 L0, float3 L1, float3 L2, float3 L3) {
    float3 P[4] = {L0, L1, L2, L3};
    float3 first_v = 0.0f;
    float3 prev_v = 0.0f;
    float3 sum = 0.0f;
    uint n = 0u;
    [unroll] for (uint i = 0u; i < 4u; ++i) {
        float3 a = P[i];
        float3 b = P[(i + 1u) & 3u];
        bool a_in = (a.z >= 0.0f);
        bool b_in = (b.z >= 0.0f);
        if (a_in) {
            float3 u = safe_s2_vertex(a);
            if (n == 0u) first_v = u;
            else sum += ltc_integrate_edge_vec(prev_v, u);
            prev_v = u;
            ++n;
        }
        if (a_in != b_in) {
            float t = a_in ? (a.z / max(a.z - b.z, 1e-7f)) : ((-a.z) / max(b.z - a.z, 1e-7f));
            float3 u = safe_s2_vertex(lerp(a, b, saturate(t)));
            if (n == 0u) first_v = u;
            else sum += ltc_integrate_edge_vec(prev_v, u);
            prev_v = u;
            ++n;
        }
    }
    if (n < 3u)
        return 0.0f;
    sum += ltc_integrate_edge_vec(prev_v, first_v);
    return sum;
}

float3 calc_cap_representative_dir(float3 L_center, float3 R, float sin_sigma, float cos_sigma) {
    float d_lr = dot(L_center, R);
    if (d_lr >= cos_sigma)
        return R;
    float3 s_vec = R - d_lr * L_center;
    float s_len2 = dot(s_vec, s_vec);
    float3 tangent;
    if (s_len2 > 1e-12f) {
        tangent = s_vec * rsqrt(s_len2);
    } else {
        float3 up_ref = abs(L_center.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
        tangent = normalize(cross(up_ref, L_center));
    }
    return safe_unit_normal(cos_sigma * L_center + sin_sigma * tangent);
}

void eval_analytic_brdf(OpenPbrSurface s, float3 N, float3 GN, float3 V, float3 L_diff, float3 L_spec,
                        float half_sin_sigma, float micro_vis, uint charlie_srv, out float3 out_diff,
                        out float3 out_spec) {
    out_diff = 0.0f;
    out_spec = 0.0f;

    float nl_raw = dot(N, L_diff);
    // OpenPBR Profile 3: ThinFoliage energy-conserving diffuse transmission through back side (-N):
    if (s.profile_id == SLAB_PROFILE_FOLIAGE && nl_raw < 0.0f) {
        float back_nl = saturate(-nl_raw);
        float3 f0_b = lerp(s.spec_f0_dielectric.xxx, s.base_color, s.metallic);
        out_diff = (1.0f - s.metallic) * (1.0f - f0_b) * (0.5f * s.lobe_weight) * s.base_color * s.f82_or_lobe_color * ATELIER_INV_PI * back_nl;
        return;
    }

    float nv = max(dot(N, V), 1e-4f);
    float nl_d = saturate(nl_raw);
    float nl_s = saturate(dot(N, L_spec));
    bool has_coat = (s.profile_id == SLAB_PROFILE_COAT && s.lobe_weight > 0.0f);
    float3 N_coat = has_coat ? eval_coat_normal(N, GN, s.lobe_weight) : N;
    float nl_c = has_coat ? saturate(dot(N_coat, L_spec)) : 0.0f;
    if (nl_d <= 0.0f && nl_s <= 0.0f && nl_c <= 0.0f)
        return;

    float3 H_d = safe_unit_normal(V + L_diff);
    float vh_d = saturate(dot(V, H_d));
    float nh_d = saturate(dot(N, H_d));
    float lv_d = dot(L_diff, V);

    float3 f0 = lerp(s.spec_f0_dielectric.xxx, s.base_color, s.metallic);
    float3 f82 = (s.profile_id == SLAB_PROFILE_STANDARD_F82) ? lerp(1.0f.xxx, s.f82_or_lobe_color, s.metallic) : 1.0f.xxx;

    float3 diff = 0.0f;
    if (nl_d > 0.0f) {
        float3 F_d = eval_schlick_f82(vh_d, f0, f82);
        if (s.profile_id == SLAB_PROFILE_IRIDESCENT && s.lobe_weight > 0.0f)
            F_d = lerp(F_d, eval_thin_film_iridescence(vh_d, s.lobe_param0, s.lobe_param1, f0), s.lobe_weight);
        diff = (1.0f - F_d) * s.base_color * (1.0f - s.metallic) * eval_eon_diffuse(nl_d, nv, lv_d, s.diffuse_roughness) * nl_d;
        if (s.profile_id == SLAB_PROFILE_FOLIAGE)
            diff *= (1.0f - 0.5f * s.lobe_weight);
    }

    float3 spec = 0.0f;
    float a = max(s.roughness * s.roughness, 0.0025f);
    float3 H_s = safe_unit_normal(V + L_spec);
    float vh_s = saturate(dot(V, H_s));
    if (nl_s > 0.0f) {
        float nh_s = saturate(dot(N, H_s));
        float a2 = a * a;
        float a_p = saturate(a + half_sin_sigma);
        float base_norm = (a2) / max(a_p * a_p, 1e-6f);
        float d_den = nh_s * nh_s * (a2 - 1.0f) + 1.0f;
        float D = (a2 / max(ATELIER_PI * d_den * d_den, 1e-7f)) * base_norm;
        float gv = 2.0f * nv / (nv + sqrt(a2 + (1.0f - a2) * nv * nv));
        float gl = 2.0f * nl_s / (nl_s + sqrt(a2 + (1.0f - a2) * nl_s * nl_s));

        float3 F_s = eval_schlick_f82(vh_s, f0, f82);
        if (s.profile_id == SLAB_PROFILE_IRIDESCENT && s.lobe_weight > 0.0f)
            F_s = lerp(F_s, eval_thin_film_iridescence(vh_s, s.lobe_param0, s.lobe_param1, f0), s.lobe_weight);
        spec = (D * gv * gl * F_s) / max(4.0f * nl_s * nv, 1e-5f) * nl_s;
    }

    // Profile 1: Dielectric Clearcoat (evaluated independently of bumped base `nl_s`!)
    if (has_coat) {
        float cF = 0.04f + 0.96f * pow(1.0f - vh_s, 5.0f);
        float3 coat_sub = eval_openpbr_coat_darkening(s.base_color, s.roughness, s.lobe_weight, s.lobe_param1, s.f82_or_lobe_color);
        spec *= (1.0f - s.lobe_weight * cF) * coat_sub;
        if (nl_c > 0.0f) {
            float nv_c = max(dot(N_coat, V), 1e-4f);
            float nh_c = saturate(dot(N_coat, H_s));
            float ca = max(s.lobe_param0 * s.lobe_param0, 0.0025f);
            float ca2 = ca * ca;
            float ca_p = saturate(ca + half_sin_sigma);
            float coat_norm = ca2 / max(ca_p * ca_p, 1e-6f);
            float cd_den = nh_c * nh_c * (ca2 - 1.0f) + 1.0f;
            float cD = (ca2 / max(ATELIER_PI * cd_den * cd_den, 1e-7f)) * coat_norm;
            float cgv = 2.0f * nv_c / (nv_c + sqrt(ca2 + (1.0f - ca2) * nv_c * nv_c));
            float cgl = 2.0f * nl_c / (nl_c + sqrt(ca2 + (1.0f - ca2) * nl_c * nl_c));
            float coat_spec = (cD * cgv * cgl * cF) / max(4.0f * nl_c * nv_c, 1e-5f) * nl_c;
            spec += s.lobe_weight * coat_spec;
        }
        if (nl_d > 0.0f) {
            float cF_d = 0.04f + 0.96f * pow(1.0f - vh_d, 5.0f);
            diff *= (1.0f - s.lobe_weight * cF_d) * coat_sub;
        }
    }

    // Profile 2: Fuzz / Velvet Sheen (Estevez & Kulla Charlie NDF evaluated on L_diff)
    if (s.profile_id == SLAB_PROFILE_FUZZ && s.lobe_weight > 0.0f && nl_d > 0.0f) {
        float fr = saturate(max(s.lobe_param0, 0.07f));
        float inv_r = 1.0f / fr;
        float sin_h = sqrt(max(1.0f - nh_d * nh_d, 1e-6f));
        float d_ch = (2.0f + inv_r) * pow(sin_h, inv_r) * (0.5f * ATELIER_INV_PI);
        float v_ch = 1.0f / max(4.0f * (nl_d + nv - nl_d * nv), 0.1f);
        Texture2D<float> charlie_lut = ResourceDescriptorHeap[atelier_safe_desc(charlie_srv)];
        SamplerState smp = SamplerDescriptorHeap[0];
        float2 uv_v = float2(fr, saturate(nv)) * (31.0f / 32.0f) + (0.5f / 32.0f);
        float2 uv_l = float2(fr, saturate(nl_d)) * (31.0f / 32.0f) + (0.5f / 32.0f);
        float e_fuzz = max(charlie_lut.SampleLevel(smp, uv_v, 0.0f), charlie_lut.SampleLevel(smp, uv_l, 0.0f));
        float fuzz_atten = saturate(1.0f - s.lobe_weight * e_fuzz);
        diff *= fuzz_atten;
        spec = spec * fuzz_atten + s.lobe_weight * s.f82_or_lobe_color * (d_ch * v_ch * nl_d);
    }

    float ms_d = eval_chan_micro_shadow(nl_d, micro_vis);
    float ms_s = eval_chan_micro_shadow(max(nl_d, max(nl_s, nl_c)), micro_vis);
    out_diff = diff * ms_d;
    out_spec = spec * ms_s;
}

void eval_rectangle_ltc(OpenPbrSurface s, float3 P, float3 N, float3 GN, bool has_geom_normal, bool has_normal_tilt,
                        float3 V, GpuLightRecord l, GlobalFrameConstants fcb, float micro_vis,
                        out float3 out_diff, out float3 out_spec) {
    out_diff = 0.0f;
    out_spec = 0.0f;

    float3 to_center = l.position - P;
    float d_center = length(to_center);
    float extent = 0.5f * (l.width + l.height);
    if (d_center >= l.range + extent)
        return;

    // Exact planar facing check:
    if ((l.kind_and_flags & 4u) == 0u && dot(P - l.position, l.direction) <= 0.0f)
        return;

    float win_t = saturate(1.0f - pow(max(d_center - extent, 0.0f) / max(l.range, 1e-3f), 4.0f));
    float window = win_t * win_t;
    if (window <= 0.0f)
        return;

    float nv = max(dot(N, V), 1e-4f);
    float3 T1 = V - N * dot(V, N);
    float t1_len = length(T1);
    if (t1_len > 1e-5f) {
        T1 /= t1_len;
    } else {
        float3 up_ref = abs(N.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
        T1 = normalize(cross(up_ref, N));
    }
    float3 T2 = cross(N, T1);

    float3 hx = l.right * (0.5f * l.width);
    float3 hy = l.up * (0.5f * l.height);
    float3 wp0 = to_center - hx - hy;
    float3 wp1 = to_center + hx - hy;
    float3 wp2 = to_center + hx + hy;
    float3 wp3 = to_center - hx + hy;

    float gn_vis = 1.0f;
    if (has_geom_normal) {
        float max_gn = max(max(dot(wp0, GN), dot(wp1, GN)), max(dot(wp2, GN), dot(wp3, GN)));
        if (max_gn <= 0.0f)
            return;
        if (has_normal_tilt)
            gn_vis = saturate((max_gn / max(d_center, 1e-4f)) * 20.0f);
    }

    // Transform quad into surface tangent frame (T1, T2, N):
    float3 lp0 = float3(dot(wp0, T1), dot(wp0, T2), dot(wp0, N));
    float3 lp1 = float3(dot(wp1, T1), dot(wp1, T2), dot(wp1, N));
    float3 lp2 = float3(dot(wp2, T1), dot(wp2, T2), dot(wp2, N));
    float3 lp3 = float3(dot(wp3, T1), dot(wp3, T2), dot(wp3, N));
    bool base_behind = (lp0.z <= 0.0f && lp1.z <= 0.0f && lp2.z <= 0.0f && lp3.z <= 0.0f);
    bool has_coat = (s.profile_id == SLAB_PROFILE_COAT && s.lobe_weight > 0.0f);
    float3 N_coat = has_coat ? eval_coat_normal(N, GN, s.lobe_weight) : N;
    float max_coat = has_coat ? max(max(dot(wp0, N_coat), dot(wp1, N_coat)), max(dot(wp2, N_coat), dot(wp3, N_coat))) : 0.0f;
    if (base_behind && (!has_coat || max_coat <= 0.0f)) {
        if (s.profile_id == SLAB_PROFILE_FOLIAGE && s.lobe_weight > 0.0f) {
            float back_form = abs(ltc_clipped_polygon_vector(-lp0, -lp3, -lp2, -lp1).z);
            float3 f0_b = lerp(s.spec_f0_dielectric.xxx, s.base_color, s.metallic);
            out_diff = (1.0f - s.metallic) * (1.0f - f0_b) * (0.5f * s.lobe_weight) * s.base_color *
                       s.f82_or_lobe_color * back_form * (l.color_intensity * window);
        }
        return;
    }

    Texture2D<float4> ltc_mat_tex = ResourceDescriptorHeap[atelier_safe_desc(fcb.ltc_matrix_srv)];
    Texture2D<float2> ltc_amp_tex = ResourceDescriptorHeap[atelier_safe_desc(fcb.ltc_amp_srv)];
    SamplerState smp = SamplerDescriptorHeap[0];

    float3 diff = 0.0f;
    float3 spec = 0.0f;
    float diff_form = 0.0f;
    float3 L_avg_ts = float3(0.0f, 0.0f, 1.0f);
    if (!base_behind) {
        // 1. Clipped cosine polygon diffuse integral + average incident half-vector (1 - F) factor:
        float3 diff_vec = ltc_clipped_polygon_vector(lp0, lp1, lp2, lp3);
        diff_form = abs(diff_vec.z);

        float3 f0 = lerp(s.spec_f0_dielectric.xxx, s.base_color, s.metallic);
        L_avg_ts = (dot(diff_vec, diff_vec) > 1e-12f) ? normalize(diff_vec * (diff_vec.z < 0.0f ? -1.0f : 1.0f))
                                                      : float3(0.0f, 0.0f, 1.0f);
        float3 V_ts = float3(sqrt(max(0.0f, 1.0f - nv * nv)), 0.0f, nv);
        float3 H_avg_ts = safe_unit_normal(V_ts + L_avg_ts);
        float vh_avg = saturate(dot(V_ts, H_avg_ts));
        if (s.profile_id == SLAB_PROFILE_IRIDESCENT && s.lobe_weight > 0.0f)
            f0 = lerp(f0, eval_thin_film_iridescence(vh_avg, s.lobe_param0, s.lobe_param1, f0), s.lobe_weight);
        float3 F_diff = f0 + (1.0f - f0) * pow(1.0f - vh_avg, 5.0f);
        if (s.diffuse_roughness > 1e-3f)
            diff_form *= eval_eon_diffuse(saturate(L_avg_ts.z), nv, dot(L_avg_ts, V_ts), s.diffuse_roughness) * ATELIER_PI;

        // 2. Specular LTC integral using exact fitted 64x64 LUT (u = roughness, v = sqrt(1 - NoV)):
        float2 ltc_uv = float2(s.roughness, sqrt(max(0.0f, 1.0f - nv)));
        ltc_uv = ltc_uv * (63.0f / 64.0f) + (0.5f / 64.0f);
        float4 m = ltc_mat_tex.SampleLevel(smp, ltc_uv, 0.0f);
        float2 amp = ltc_amp_tex.SampleLevel(smp, ltc_uv, 0.0f);

        float3 sp0 = float3(m.x * lp0.x + m.z * lp0.z, lp0.y, m.y * lp0.x + m.w * lp0.z);
        float3 sp1 = float3(m.x * lp1.x + m.z * lp1.z, lp1.y, m.y * lp1.x + m.w * lp1.z);
        float3 sp2 = float3(m.x * lp2.x + m.z * lp2.z, lp2.y, m.y * lp2.x + m.w * lp2.z);
        float3 sp3 = float3(m.x * lp3.x + m.z * lp3.z, lp3.y, m.y * lp3.x + m.w * lp3.z);
        float spec_form = abs(ltc_clipped_polygon_vector(sp0, sp1, sp2, sp3).z);

        float3 f_spec = f0 * max(amp.x - amp.y, 0.0f) + amp.y;
        if (s.profile_id == SLAB_PROFILE_STANDARD_F82 && s.metallic > 0.0f && any(s.f82_or_lobe_color < 0.999f)) {
            float3 f82 = lerp(1.0f.xxx, s.f82_or_lobe_color, s.metallic);
            float3 f_sch = f0 + (1.0f - f0) * pow(1.0f - vh_avg, 5.0f);
            f_spec *= eval_schlick_f82(vh_avg, f0, f82) / max(f_sch, 1e-4f);
            F_diff = eval_schlick_f82(vh_avg, f0, f82);
        }

        diff = (1.0f - F_diff) * s.base_color * (1.0f - s.metallic) * diff_form;
        if (s.profile_id == SLAB_PROFILE_FOLIAGE)
            diff *= (1.0f - 0.5f * s.lobe_weight);
        spec = f_spec * spec_form;
    }

    float coat_l_z = 0.0f;
    if (has_coat) {
        float nv_c = max(dot(N_coat, V), 1e-4f);
        float3 T1_c = V - N_coat * dot(V, N_coat);
        float t1c_len = length(T1_c);
        if (t1c_len > 1e-5f) {
            T1_c /= t1c_len;
        } else {
            float3 up_c = abs(N_coat.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
            T1_c = normalize(cross(up_c, N_coat));
        }
        float3 T2_c = cross(N_coat, T1_c);
        float3 clp0 = float3(dot(wp0, T1_c), dot(wp0, T2_c), dot(wp0, N_coat));
        float3 clp1 = float3(dot(wp1, T1_c), dot(wp1, T2_c), dot(wp1, N_coat));
        float3 clp2 = float3(dot(wp2, T1_c), dot(wp2, T2_c), dot(wp2, N_coat));
        float3 clp3 = float3(dot(wp3, T1_c), dot(wp3, T2_c), dot(wp3, N_coat));

        float2 c_uv = float2(s.lobe_param0, sqrt(max(0.0f, 1.0f - nv_c))) * (63.0f / 64.0f) + (0.5f / 64.0f);
        float4 cm = ltc_mat_tex.SampleLevel(smp, c_uv, 0.0f);
        float2 camp = ltc_amp_tex.SampleLevel(smp, c_uv, 0.0f);
        float c_form = 0.0f;
        if (max_coat > 0.0f) {
            float3 cp0 = float3(cm.x * clp0.x + cm.z * clp0.z, clp0.y, cm.y * clp0.x + cm.w * clp0.z);
            float3 cp1 = float3(cm.x * clp1.x + cm.z * clp1.z, clp1.y, cm.y * clp1.x + cm.w * clp1.z);
            float3 cp2 = float3(cm.x * clp2.x + cm.z * clp2.z, clp2.y, cm.y * clp2.x + cm.w * clp2.z);
            float3 cp3 = float3(cm.x * clp3.x + cm.z * clp3.z, clp3.y, cm.y * clp3.x + cm.w * clp3.z);
            c_form = abs(ltc_clipped_polygon_vector(cp0, cp1, cp2, cp3).z);
            coat_l_z = saturate(dot(safe_unit_normal(to_center), N_coat));
        }
        float c_f = 0.04f * max(camp.x - camp.y, 0.0f) + camp.y;
        float c_atten = saturate(1.0f - s.lobe_weight * c_f);
        float3 coat_sub = eval_openpbr_coat_darkening(s.base_color, s.roughness, s.lobe_weight, s.lobe_param1, s.f82_or_lobe_color);
        diff *= c_atten * coat_sub;
        spec = spec * c_atten * coat_sub + (s.lobe_weight * c_f) * c_form;
    } else if (s.profile_id == SLAB_PROFILE_FUZZ && s.lobe_weight > 0.0f && !base_behind) {
        Texture2D<float> charlie_lut = ResourceDescriptorHeap[atelier_safe_desc(fcb.charlie_lut_srv)];
        float fr = saturate(max(s.lobe_param0, 0.07f));
        float2 uv_v = float2(fr, saturate(nv)) * (31.0f / 32.0f) + (0.5f / 32.0f);
        float2 uv_l = float2(fr, saturate(L_avg_ts.z)) * (31.0f / 32.0f) + (0.5f / 32.0f);
        float e_fuzz = max(charlie_lut.SampleLevel(smp, uv_v, 0.0f), charlie_lut.SampleLevel(smp, uv_l, 0.0f));
        float fuzz_atten = saturate(1.0f - s.lobe_weight * e_fuzz);
        diff *= fuzz_atten;
        spec = spec * fuzz_atten + (s.lobe_weight * e_fuzz) * s.f82_or_lobe_color * diff_form;
    }

    float ms = eval_chan_micro_shadow(max(base_behind ? 0.0f : saturate(L_avg_ts.z), coat_l_z), micro_vis);
    float3 radiance = l.color_intensity * (window * ms * gn_vis);

    out_diff = diff * radiance;
    out_spec = spec * radiance;
}

// ============================================================================
// Mode 0: 16x16x32 3D Frustum + Z-Bitmask Light Cluster Builder & Inline Shadow Fallback
// ============================================================================
bool trace_direct_fallback_shadow(GpuLightRecord l, float3 wpos, float3 gn, bool is_foliage, bool jitter_penumbra,
                                  float ign, float ign2, GlobalFrameConstants fcb, inout uint fallback_rays) {
    if (fcb.tlas_srv == 0xFFFFFFFFu)
        return true;
    float3 origin;
    float3 dir;
    float t_max;
    int prep = build_analytic_shadow_ray(l, wpos, gn, is_foliage, jitter_penumbra, ign, ign2, origin, dir, t_max);
    if (prep >= 0)
        return prep != 0;
    ++fallback_rays;
    RaytracingAccelerationStructure tlas = ResourceDescriptorHeap[atelier_safe_desc(fcb.tlas_srv)];
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 1e-4f;
    ray.TMax = t_max;
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(tlas, RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFFu, ray);
    q.Proceed();
    return q.CommittedStatus() == COMMITTED_NOTHING;
}

float compute_view_max_scale(row_major float4x4 m) {
    float3 sx = m[0].xyz;
    float3 sy = m[1].xyz;
    float3 sz = m[2].xyz;
    float sx2 = dot(sx, sx);
    float sy2 = dot(sy, sy);
    float sz2 = dot(sz, sz);
    float max_r2 = max(sx2, max(sy2, sz2));
    float ortho_err = abs(dot(sx, sy)) + abs(dot(sx, sz)) + abs(dot(sy, sz));
    return (ortho_err <= 1e-4f * max_r2) ? sqrt(max_r2) : sqrt(sx2 + sy2 + sz2);
}

float4 normalize_view_plane(float4 p) {
    return p * rsqrt(max(dot(p.xyz, p.xyz), 1e-12f));
}

groupshared uint gs_z_mask;
groupshared uint gs_min_vz_key;
groupshared uint gs_max_vz_key;
groupshared float4 gs_tile_planes[4];
groupshared float gs_view_scale;
groupshared uint gs_use_2d_planes;
groupshared uint gs_pass_mask[8];
groupshared uint gs_batch_span[256];

[numthreads(16, 16, 1)]
void main(uint3 dtid : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint gindex : SV_GroupIndex) {
    StructuredBuffer<GlobalFrameConstants> fcb_buf = ResourceDescriptorHeap[atelier_safe_desc(g_frame_cb_srv)];
    GlobalFrameConstants fcb = fcb_buf[0];

    uint2 px = dtid.xy;
    uint tiles_x = (fcb.render_extent.x + 15u) / 16u;
    uint tile_id = gid.y * tiles_x + gid.x;

    if (g_direct_mode == 0u) {
        if (gindex == 0u) {
            gs_z_mask = 0u;
            gs_min_vz_key = 0xFFFFFFFFu;
            gs_max_vz_key = 0u;
        }
        GroupMemoryBarrierWithGroupSync();

        if (px.x < fcb.render_extent.x && px.y < fcb.render_extent.y) {
            float2 pxc = float2(px) + 0.5f;
            bool in_vp = (pxc.x >= fcb.scene_viewport.x && pxc.x < fcb.scene_viewport.x + fcb.scene_viewport.z &&
                          pxc.y >= fcb.scene_viewport.y && pxc.y < fcb.scene_viewport.y + fcb.scene_viewport.w);
            if (in_vp) {
                Texture2D<float> depth_tex = ResourceDescriptorHeap[atelier_safe_desc(g_depth_srv)];
                float dz = depth_tex.Load(int3(px, 0));
                if (dz > 0.0f) {
                    float vz = exact_view_z_from_depth(px, dz, fcb);
                    uint zbin = depth_to_z_bin(vz);
                    uint vz_key = float_to_sortable_uint(vz);
                    InterlockedOr(gs_z_mask, 1u << zbin);
                    InterlockedMin(gs_min_vz_key, vz_key);
                    InterlockedMax(gs_max_vz_key, vz_key);
                }
            }
        }
        GroupMemoryBarrierWithGroupSync();

        RWStructuredBuffer<uint> cluster_buf = ResourceDescriptorHeap[atelier_safe_desc(g_cluster_list_uav_or_srv)];
        uint base = tile_id * 132u;

        if (gs_z_mask == 0u) {
            if (gindex == 0u) {
                cluster_buf[base + 0u] = 0u;
                cluster_buf[base + 1u] = 0u;
            }
            return;
        }

        float min_vz = sortable_uint_to_float(gs_min_vz_key);
        float max_vz = sortable_uint_to_float(gs_max_vz_key);

        if (gindex == 0u) {
            float tile_x0 = max(float(gid.x * 16u), fcb.scene_viewport.x);
            float tile_x1 = min(float((gid.x + 1u) * 16u), fcb.scene_viewport.x + fcb.scene_viewport.z);
            float tile_y0 = max(float(gid.y * 16u), fcb.scene_viewport.y);
            float tile_y1 = min(float((gid.y + 1u) * 16u), fcb.scene_viewport.y + fcb.scene_viewport.w);
            float inv_vw = 2.0f / max(fcb.scene_viewport.z, 1.0f);
            float inv_vh = 2.0f / max(fcb.scene_viewport.w, 1.0f);
            float ndc_x0 = (tile_x0 - fcb.scene_viewport.x) * inv_vw - 1.0f;
            float ndc_x1 = (tile_x1 - fcb.scene_viewport.x) * inv_vw - 1.0f;
            float ndc_y1 = 1.0f - (tile_y0 - fcb.scene_viewport.y) * inv_vh;
            float ndc_y0 = 1.0f - (tile_y1 - fcb.scene_viewport.y) * inv_vh;

            float4 col_x = float4(fcb.projection[0][0], fcb.projection[1][0], fcb.projection[2][0], fcb.projection[3][0]);
            float4 col_y = float4(fcb.projection[0][1], fcb.projection[1][1], fcb.projection[2][1], fcb.projection[3][1]);
            float4 col_w = float4(fcb.projection[0][3], fcb.projection[1][3], fcb.projection[2][3], fcb.projection[3][3]);

            bool valid_w = (abs(col_w.x) <= 1e-6f && abs(col_w.y) <= 1e-6f &&
                            (min_vz * col_w.z + col_w.w) > 1e-5f && (max_vz * col_w.z + col_w.w) > 1e-5f);
            gs_use_2d_planes = valid_w ? 1u : 0u;
            gs_view_scale = compute_view_max_scale(fcb.view);
            if (valid_w) {
                gs_tile_planes[0] = normalize_view_plane(col_x - ndc_x0 * col_w);
                gs_tile_planes[1] = normalize_view_plane(ndc_x1 * col_w - col_x);
                gs_tile_planes[2] = normalize_view_plane(col_y - ndc_y0 * col_w);
                gs_tile_planes[3] = normalize_view_plane(ndc_y1 * col_w - col_y);
            }
        }
        GroupMemoryBarrierWithGroupSync();

        uint count = 0u;
        bool overflow = false;
        uint local_count = (fcb.light_count > fcb.dir_light_count && fcb.lights_srv != 0xFFFFFFFFu)
                               ? (fcb.light_count - fcb.dir_light_count)
                               : 0u;
        if (local_count > 0u) {
            StructuredBuffer<GpuLightRecord> lights = ResourceDescriptorHeap[atelier_safe_desc(fcb.lights_srv)];
            bool use_2d = (gs_use_2d_planes != 0u);
            float v_scale = gs_view_scale;
            float4 p0 = gs_tile_planes[0];
            float4 p1 = gs_tile_planes[1];
            float4 p2 = gs_tile_planes[2];
            float4 p3 = gs_tile_planes[3];

            [loop] for (uint batch = 0u; batch < local_count; batch += 256u) {
                if (gindex < 8u)
                    gs_pass_mask[gindex] = 0u;
                GroupMemoryBarrierWithGroupSync();

                uint local_idx = batch + gindex;
                if (local_idx < local_count) {
                    uint li = fcb.dir_light_count + local_idx;
                    GpuLightRecord l = lights[li];
                    uint kind = l.kind_and_flags & 3u;
                    float support_r = l.range + ((kind == 1u) ? (0.5f * (l.width + l.height)) : l.source_radius);
                    float r_vs = (support_r * 1.001f + 1e-3f) * v_scale;
                    float3 lv = mul(float4(l.position, 1.0f), fcb.view).xyz;
                    bool pass_tile = (lv.z + r_vs >= min_vz) && (lv.z - r_vs <= max_vz);
                    if (pass_tile && use_2d) {
                        float4 lv4 = float4(lv, 1.0f);
                        pass_tile = (dot(lv4, p0) >= -r_vs) && (dot(lv4, p1) >= -r_vs) &&
                                    (dot(lv4, p2) >= -r_vs) && (dot(lv4, p3) >= -r_vs);
                    }
                    if (pass_tile) {
                        uint b_lo = depth_to_z_bin(lv.z - r_vs);
                        uint b_hi = depth_to_z_bin(lv.z + r_vs);
                        uint span_bits = (b_hi - b_lo >= 31u) ? 0xFFFFFFFFu : (((1u << (b_hi - b_lo + 1u)) - 1u) << b_lo);
                        if ((span_bits & gs_z_mask) != 0u) {
                            gs_batch_span[gindex] = span_bits;
                            InterlockedOr(gs_pass_mask[gindex >> 5u], 1u << (gindex & 31u));
                        }
                    }
                }
                GroupMemoryBarrierWithGroupSync();

                if (gindex == 0u) {
                    [unroll] for (uint w = 0u; w < 8u; ++w) {
                        uint mask = gs_pass_mask[w];
                        while (mask != 0u) {
                            uint bit = firstbitlow(mask);
                            mask &= (mask - 1u);
                            uint slot = (w << 5u) + bit;
                            if (count < 64u) {
                                cluster_buf[base + 2u + count] = fcb.dir_light_count + batch + slot;
                                cluster_buf[base + 66u + count] = gs_batch_span[slot];
                                ++count;
                            } else {
                                overflow = true;
                            }
                        }
                    }
                }
                if (batch + 256u < local_count)
                    GroupMemoryBarrierWithGroupSync();
            }
        }

        if (gindex == 0u) {
            cluster_buf[base + 0u] = count | (overflow ? 0x80000000u : 0u);
            cluster_buf[base + 1u] = gs_z_mask;

            if (g_cluster_stats_uav != 0xFFFFFFFFu) {
                RWStructuredBuffer<uint> c_stats = ResourceDescriptorHeap[atelier_safe_desc(g_cluster_stats_uav)];
                InterlockedAdd(c_stats[1u], 1u);
                if (overflow) InterlockedAdd(c_stats[2u], 1u);
                InterlockedMax(c_stats[3u], count);
            }
        }
        return;
    }

    if (g_direct_mode == 3u) {
        // Mode 3: Optional Receiver Light-Support Histogram Diagnostic (`direct_cluster_inspect`)
        RWStructuredBuffer<uint> hist_buf = ResourceDescriptorHeap[atelier_safe_desc(g_cluster_stats_uav)];
        if (g_contact_shadow_srv == 0u) {
            if (gid.x == 0u && gid.y == 0u && gindex < 67u)
                hist_buf[gindex] = 0u;
            return;
        }
        if (gindex < 66u)
            gs_batch_span[gindex] = 0u;
        if (gindex == 0u)
            gs_z_mask = 0u;
        GroupMemoryBarrierWithGroupSync();

        if (px.x < fcb.render_extent.x && px.y < fcb.render_extent.y) {
            float2 pxc = float2(px) + 0.5f;
            bool in_vp = (pxc.x >= fcb.scene_viewport.x && pxc.x < fcb.scene_viewport.x + fcb.scene_viewport.z &&
                          pxc.y >= fcb.scene_viewport.y && pxc.y < fcb.scene_viewport.y + fcb.scene_viewport.w);
            if (in_vp) {
                Texture2D<float> depth_tex = ResourceDescriptorHeap[atelier_safe_desc(g_depth_srv)];
                float dz = depth_tex.Load(int3(px, 0));
                if (dz > 0.0f) {
                    Texture2D<uint2> slab0_tex = ResourceDescriptorHeap[atelier_safe_desc(g_slab0_srv)];
                    uint2 s0 = slab0_tex.Load(int3(px, 0));
                    if (((s0.y >> 29u) & 1u) != 0u) {
                        float2 ndc = float2((pxc.x - fcb.scene_viewport.x) / max(fcb.scene_viewport.z, 1.0f) * 2.0f - 1.0f,
                                            1.0f - (pxc.y - fcb.scene_viewport.y) / max(fcb.scene_viewport.w, 1.0f) * 2.0f);
                        precise float4 wp_h = mul(float4(ndc, dz, 1.0f), fcb.inv_view_projection);
                        float ww = abs(wp_h.w) > 1e-7f ? wp_h.w : (wp_h.w >= 0.0f ? 1e-7f : -1e-7f);
                        float3 P = wp_h.xyz / ww;
                        uint src_cnt = 0u;
                        if (fcb.light_count > fcb.dir_light_count && fcb.lights_srv != 0xFFFFFFFFu) {
                            StructuredBuffer<GpuLightRecord> lights = ResourceDescriptorHeap[atelier_safe_desc(fcb.lights_srv)];
                            [loop] for (uint li = fcb.dir_light_count; li < fcb.light_count; ++li) {
                                GpuLightRecord l = lights[li];
                                uint kind = l.kind_and_flags & 3u;
                                float sr = l.range + ((kind == 1u) ? (0.5f * (l.width + l.height)) : l.source_radius);
                                float3 d = l.position - P;
                                if (dot(d, d) < sr * sr)
                                    ++src_cnt;
                            }
                        }
                        InterlockedAdd(gs_batch_span[min(src_cnt, 65u)], 1u);
                        InterlockedMax(gs_z_mask, src_cnt);
                    }
                }
            }
        }
        GroupMemoryBarrierWithGroupSync();

        if (gindex == 0u && gs_z_mask > 0u)
            InterlockedMax(hist_buf[0u], gs_z_mask);
        if (gindex < 66u && gs_batch_span[gindex] > 0u)
            InterlockedAdd(hist_buf[1u + gindex], gs_batch_span[gindex]);
        return;
    }

    if (px.x >= fcb.render_extent.x || px.y >= fcb.render_extent.y)
        return;

    if (g_direct_mode == 2u) {
        // Indirect-gated Jiménez Separable SSSSS (bilateral Gaussian along X/Y for Profile 5: Subsurface)
        Texture2D<uint2> slab0_tex = ResourceDescriptorHeap[atelier_safe_desc(g_slab0_srv)];
        Texture2D<float4> src_diff = ResourceDescriptorHeap[atelier_safe_desc(g_shadow_mask_srv)];
        RWTexture2D<float4> dst_diff = ResourceDescriptorHeap[atelier_safe_desc(g_direct_diff_uav)];
        float4 center = src_diff.Load(int3(px, 0));
        uint2 s0_raw = slab0_tex.Load(int3(px, 0));
        if (((s0_raw.y >> 29u) & 1u) == 0u || ((s0_raw.y >> 26u) & 7u) != SLAB_PROFILE_SUBSURFACE) {
            dst_diff[px] = center;
            return;
        }
        Texture2D<uint2> slab1_tex = ResourceDescriptorHeap[atelier_safe_desc(g_slab1_srv)];
        OpenPbrSurface s = unpack_openpbr_slab(s0_raw, slab1_tex.Load(int3(px, 0)));
        if (s.lobe_weight <= 1e-3f) {
            dst_diff[px] = center;
            return;
        }
        Texture2D<float> depth_tex = ResourceDescriptorHeap[atelier_safe_desc(g_depth_srv)];
        float dz = depth_tex.Load(int3(px, 0));
        if (dz <= 0.0f) {
            dst_diff[px] = center;
            return;
        }
        float vz = exact_view_z_from_depth(px, dz, fcb);
        float radius_m = max(0.002f, s.lobe_param0 * 0.05f);
        float clip_w = max(abs(vz * fcb.projection[2][3] + fcb.projection[3][3]), 1e-4f);
        float px_radius = clamp((radius_m * abs(fcb.projection[1][1]) * 0.5f * fcb.scene_viewport.w) / clip_w, 1.0f, 12.0f);
        int2 axis = (g_contact_shadow_srv == 0u) ? int2(1, 0) : int2(0, 1);
        int2 sc_min = fcb.scissor_rect.xy;
        int2 sc_max = max(sc_min, fcb.scissor_rect.zw - 1);

        float3 accum = center.rgb * 0.40f;
        float3 w_sum = 0.40f;
        const float offsets[4] = {-2.0f, -1.0f, 1.0f, 2.0f};
        const float weights[4] = {0.10f, 0.20f, 0.20f, 0.10f};
        [unroll] for (uint k = 0u; k < 4u; ++k) {
            int2 q = clamp(int2(px) + axis * int(round(offsets[k] * px_radius * 0.5f)), sc_min, sc_max);
            uint2 qs0 = slab0_tex.Load(int3(q, 0));
            if (((qs0.y >> 29u) & 1u) == 0u || ((qs0.y >> 26u) & 7u) != SLAB_PROFILE_SUBSURFACE)
                continue;
            float qd = depth_tex.Load(int3(q, 0));
            if (qd > 0.0f) {
                float qvz = exact_view_z_from_depth(uint2(q), qd, fcb);
                float depth_w = exp(-abs(qvz - vz) / max(radius_m * 2.0f, 1e-3f));
                float norm_w = saturate(dot(s.shading_normal, unpack_oct_snorm16(qs0.x)) * 0.5f + 0.5f);
                float3 tint_w = weights[k] * (depth_w * norm_w) * lerp(0.25f.xxx, 1.0f.xxx, s.f82_or_lobe_color);
                accum += src_diff.Load(int3(q, 0)).rgb * tint_w;
                w_sum += tint_w;
            }
        }
        dst_diff[px] = float4(lerp(center.rgb, accum / max(w_sum, 1e-4f), s.lobe_weight), center.a);
        return;
    }

    // Mode 1: Direct Lighting Evaluation
    RWTexture2D<float4> out_diff_uav = ResourceDescriptorHeap[atelier_safe_desc(g_direct_diff_uav)];
    RWTexture2D<float4> out_spec_uav = ResourceDescriptorHeap[atelier_safe_desc(g_direct_spec_uav)];

    float2 pxc = float2(px) + 0.5f;
    bool in_vp = (pxc.x >= fcb.scene_viewport.x && pxc.x < fcb.scene_viewport.x + fcb.scene_viewport.z &&
                  pxc.y >= fcb.scene_viewport.y && pxc.y < fcb.scene_viewport.y + fcb.scene_viewport.w);
    Texture2D<float> depth_tex = ResourceDescriptorHeap[atelier_safe_desc(g_depth_srv)];
    float dz = depth_tex.Load(int3(px, 0));
    if (!in_vp || dz <= 0.0f || fcb.light_count == 0u || fcb.lights_srv == 0xFFFFFFFFu) {
        out_diff_uav[px] = 0.0f;
        out_spec_uav[px] = 0.0f;
        return;
    }

    Texture2D<uint2> slab0_tex = ResourceDescriptorHeap[atelier_safe_desc(g_slab0_srv)];
    uint2 s0_px = slab0_tex.Load(int3(px, 0));
    if (((s0_px.y >> 29u) & 1u) == 0u) {
        out_diff_uav[px] = 0.0f;
        out_spec_uav[px] = 0.0f;
        return;
    }
    Texture2D<uint2> slab1_tex = ResourceDescriptorHeap[atelier_safe_desc(g_slab1_srv)];
    OpenPbrSurface surf = unpack_openpbr_slab(s0_px, slab1_tex.Load(int3(px, 0)));

    float2 ndc = float2((pxc.x - fcb.scene_viewport.x) / max(fcb.scene_viewport.z, 1.0f) * 2.0f - 1.0f,
                        1.0f - (pxc.y - fcb.scene_viewport.y) / max(fcb.scene_viewport.w, 1.0f) * 2.0f);
    precise float4 wp_h = mul(float4(ndc, dz, 1.0f), fcb.inv_view_projection);
    float ww = abs(wp_h.w) > 1e-7f ? wp_h.w : (wp_h.w >= 0.0f ? 1e-7f : -1e-7f);
    float3 P = wp_h.xyz / ww;
    float3 eye_pos = mul(float4(P, 1.0f), fcb.view).xyz;
    float3 eye_ray_vs = (fcb.projection[2][3] != 0.0f) ? -eye_pos : float3(0.0f, 0.0f, -1.0f);
    float3 V = safe_unit_normal(mul(eye_ray_vs, (float3x3)fcb.inv_view));
    float3 N = surf.shading_normal;
    float3 GN = N;
    if (g_geom_normal_srv != 0xFFFFFFFFu) {
        Texture2D<uint> gn_tex = ResourceDescriptorHeap[atelier_safe_desc(g_geom_normal_srv)];
        GN = unpack_oct_snorm16(gn_tex.Load(int3(px, 0)));
    }
    bool is_foliage = (surf.profile_id == SLAB_PROFILE_FOLIAGE);
    float ndg_cos = dot(N, GN);
    bool has_geom_normal = (ndg_cos > 0.0f) && !is_foliage;
    bool has_normal_tilt = has_geom_normal && (ndg_cos <= 0.9995f);

    uint2 shadow_bits = uint2(0xFFFFFFFFu, 0xFFFFFFFFu);
    if (g_shadow_mask_srv != 0xFFFFFFFFu) {
        Texture2D<uint2> sm = ResourceDescriptorHeap[atelier_safe_desc(g_shadow_mask_srv)];
        shadow_bits = sm.Load(int3(px, 0));
    }
    float2 penumbra_vis = float2(1.0f, 1.0f);
    if (g_penumbra_mask_srv != 0xFFFFFFFFu) {
        Texture2D<float4> pm = ResourceDescriptorHeap[atelier_safe_desc(g_penumbra_mask_srv)];
        penumbra_vis = pm.Load(int3(px, 0)).xy;
    }
    float contact_vis = 1.0f;
    if (g_contact_shadow_srv != 0xFFFFFFFFu) {
        Texture2D<float> cs = ResourceDescriptorHeap[atelier_safe_desc(g_contact_shadow_srv)];
        contact_vis = cs.Load(int3(px, 0));
    }
    // Chan 2018 Aperture Micro-Shadow consumes strictly deterministic material-baked micro-cavity AO (`surf.micro_ao`);
    // centimeter-scale solar geometry occlusion is handled directionally along `L` by `ContactShadow CS`!
    float micro_vis = surf.micro_ao;

    StructuredBuffer<GpuLightRecord> lights = ResourceDescriptorHeap[atelier_safe_desc(fcb.lights_srv)];
    StructuredBuffer<uint> cluster_buf = ResourceDescriptorHeap[atelier_safe_desc(g_cluster_list_uav_or_srv)];
    uint c_base = tile_id * 132u;
    uint c_header = cluster_buf[c_base + 0u];
    uint c_count = c_header & 0x7FFFFFFFu;
    bool c_overflow = (c_header & 0x80000000u) != 0u;
    uint px_z_bit = 1u << depth_to_z_bin(exact_view_z_from_depth(px, dz, fcb));

    uint primary_sun_idx = fcb.primary_sun_and_gi_cands & 0xFFFFu;
    uint d_bits = min(fcb.dir_light_count, 4u);
    uint local_bit_cap = 64u - d_bits;
    uint tile_area_slot = (g_penumbra_mask_srv != 0xFFFFFFFFu && fcb.light_count > fcb.dir_light_count)
                              ? find_tile_primary_area_slot(c_base, c_count, local_bit_cap, cluster_buf, lights)
                              : 0xFFu;
    bool shadows_on = (fcb.flags & 1u) != 0u;
    bool jitter_penumbra = (fcb.flags & (1u << 11u)) != 0u;
    float ign = frac(52.9829189f * frac(dot(float2(px) + float(fcb.frame_index & 63u) * 7.13f, float2(0.06711056f, 0.00583715f))));
    float ign2 = frac(ign * 17.371f + 0.381966f);
    uint fallback_rays = 0u;
    float3 R_view = reflect(-V, N);

    float3 sum_diff = 0.0f;
    float3 sum_spec = 0.0f;

    // 1. Evaluate partitioned Directional lights [0 .. fcb.dir_light_count):
    [loop] for (uint di = 0u; di < fcb.dir_light_count; ++di) {
        GpuLightRecord l = lights[di];
        float3 L_diff = safe_unit_normal(-l.direction);
        float alpha = (l.source_radius > 1e-4f) ? min(l.source_radius, 0.45f) : 0.0f;
        float sin_s = sin(alpha);
        float gn_vis = has_geom_normal ? saturate((dot(GN, L_diff) + sin_s) * 10.0f + 1.0f) : 1.0f;
        if (gn_vis <= 0.0f)
            continue;

        float s_vis = 1.0f;
        if (shadows_on && (l.kind_and_flags & 8u) != 0u) {
            if (di == primary_sun_idx && g_penumbra_mask_srv != 0xFFFFFFFFu) {
                s_vis = penumbra_vis.x;
            } else if (di < d_bits) {
                s_vis = (((shadow_bits.y >> (31u - di)) & 1u) != 0u) ? 1.0f : 0.0f;
            } else {
                s_vis = trace_direct_fallback_shadow(l, P, GN, is_foliage, jitter_penumbra, ign, ign2, fcb, fallback_rays)
                            ? 1.0f
                            : 0.0f;
            }
        }
        float c_vis = (di == primary_sun_idx) ? (is_foliage ? lerp(1.0f, contact_vis, 0.5f) : contact_vis) : 1.0f;
        float vis = min(s_vis, c_vis);
        if (vis <= 0.0f)
            continue;

        float3 L_spec = L_diff;
        float half_sin_sigma = 0.0f;
        if (alpha > 0.0f) {
            L_spec = calc_cap_representative_dir(L_diff, R_view, sin_s, cos(alpha));
            half_sin_sigma = 0.5f * sin_s;
        }
        float3 d_part, s_part;
        eval_analytic_brdf(surf, N, GN, V, L_diff, L_spec, half_sin_sigma, micro_vis, fcb.charlie_lut_srv, d_part, s_part);
        sum_diff += d_part * l.color_intensity * (vis * gn_vis);
        sum_spec += s_part * l.color_intensity * (vis * gn_vis);
    }

    // 2. Evaluate clustered local analytic lights [0 .. c_count) + monotonic overflow tail [overflow_tail_start .. fcb.light_count):
    uint overflow_tail_start = (c_overflow && c_count > 0u) ? (cluster_buf[c_base + 2u + c_count - 1u] + 1u)
                                                            : (c_overflow ? fcb.dir_light_count : fcb.light_count);
    uint overflow_tail_count = (overflow_tail_start < fcb.light_count) ? (fcb.light_count - overflow_tail_start) : 0u;
    uint iter_count = c_count + overflow_tail_count;
    [loop] for (uint it = 0u; it < iter_count; ++it) {
        bool from_cluster = (it < c_count);
        if (from_cluster && (cluster_buf[c_base + 66u + it] & px_z_bit) == 0u)
            continue;
        uint li = from_cluster ? cluster_buf[c_base + 2u + it] : (overflow_tail_start + (it - c_count));
        GpuLightRecord l = lights[li];
        uint kind = l.kind_and_flags & 3u;
        bool need_fallback_shadow = false;
        float local_vis = 1.0f;
        if (shadows_on && (l.kind_and_flags & 8u) != 0u) {
            if (it == tile_area_slot) {
                local_vis = penumbra_vis.y;
                if (local_vis <= 0.0f)
                    continue;
            } else if (it < local_bit_cap) {
                uint bit_ok = (it < 32u) ? ((shadow_bits.x >> it) & 1u) : ((shadow_bits.y >> (it - 32u)) & 1u);
                if (bit_ok == 0u)
                    continue;
            } else {
                need_fallback_shadow = true;
            }
        }

        if (kind == 1u) {
            float3 d_ltc, s_ltc;
            eval_rectangle_ltc(surf, P, N, GN, has_geom_normal, has_normal_tilt, V, l, fcb, micro_vis, d_ltc, s_ltc);
            if (any(d_ltc > 0.0f) || any(s_ltc > 0.0f)) {
                if (need_fallback_shadow &&
                    !trace_direct_fallback_shadow(l, P, GN, is_foliage, jitter_penumbra, ign, ign2, fcb, fallback_rays))
                    continue;
                sum_diff += d_ltc * local_vis;
                sum_spec += s_ltc * local_vis;
            }
        } else {
            float3 to_l = l.position - P;
            float dist2 = dot(to_l, to_l);
            float dist = sqrt(max(dist2, 1e-8f));
            if (dist >= l.range + l.source_radius)
                continue;
            float3 L_diff = to_l / dist;
            float spot_atten = 1.0f;
            if (kind == 3u) {
                float spot_t = saturate((dot(-L_diff, l.direction) - l.width) * l.height);
                if (spot_t <= 0.0f)
                    continue;
                spot_atten = spot_t * spot_t;
            }
            float sin_s = (l.source_radius > 1e-4f) ? min(l.source_radius / max(dist, 1e-4f), 0.9999f) : 0.0f;
            float gn_vis = has_geom_normal ? saturate((dot(GN, L_diff) + sin_s) * 10.0f + 1.0f) : 1.0f;
            if (gn_vis <= 0.0f)
                continue;
            float r_eff = max(dist - l.source_radius, 0.0f);
            float win_t = saturate(1.0f - pow(r_eff / max(l.range, 1e-3f), 4.0f));
            float atten = (win_t * win_t * spot_atten) / max(dist2, max(0.0025f, l.source_radius * l.source_radius));
            if (atten <= 0.0f)
                continue;

            float3 L_spec = L_diff;
            float half_sin_sigma = 0.0f;
            if (sin_s > 0.0f) {
                float cos_s = sqrt(max(0.0f, 1.0f - sin_s * sin_s));
                L_spec = calc_cap_representative_dir(L_diff, R_view, sin_s, cos_s);
                half_sin_sigma = 0.5f * sin_s;
            }

            float3 d_p, s_p;
            eval_analytic_brdf(surf, N, GN, V, L_diff, L_spec, half_sin_sigma, micro_vis, fcb.charlie_lut_srv, d_p, s_p);
            if (any(d_p > 0.0f) || any(s_p > 0.0f)) {
                if (need_fallback_shadow &&
                    !trace_direct_fallback_shadow(l, P, GN, is_foliage, jitter_penumbra, ign, ign2, fcb, fallback_rays))
                    continue;
                float3 rad = l.color_intensity * (atten * gn_vis * local_vis);
                sum_diff += d_p * rad;
                sum_spec += s_p * rad;
            }
        }
    }

    uint wave_fb_rays = WaveActiveSum(fallback_rays);
    if (WaveIsFirstLane() && wave_fb_rays > 0u && g_cluster_stats_uav != 0xFFFFFFFFu) {
        RWStructuredBuffer<uint> c_stats = ResourceDescriptorHeap[atelier_safe_desc(g_cluster_stats_uav)];
        InterlockedAdd(c_stats[0u], wave_fb_rays);
    }

    out_diff_uav[px] = float4(clamp(sum_diff * fcb.exposure_scale, 0.0f, 65504.0f), 1.0f);
    out_spec_uav[px] = float4(clamp(sum_spec * fcb.exposure_scale, 0.0f, 65504.0f), 1.0f);
}
