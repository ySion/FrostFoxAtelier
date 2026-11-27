// Temporal Anti-Aliasing / Upscaling (`Atelier TAAU`), Jiménez 13-Tap Soft-Knee Karis 6-Mip Bloom,
// Khronos PBR Neutral / AgX Tonemap, Authentic Jiménez SMAA (`SearchTex` + `AreaTex` + Diag/Corners),
// and Exact 3x3 Ray-Triangle `UnjitterDepth CS` (Step 5 Definitive Specification)!
//   Mode 0: Atelier TAAU Resolve CS (YCoCg chroma-preserving ray-clip + bounded inverse-Reinhard + PrevDepth disocclusion)
//   Mode 1: Bloom Downsample (13-tap 5-box Jiménez filter; Mip 0 applies quadratic soft-knee + per-box Karis anti-firefly weight)
//   Mode 2: Bloom Normalized Tent Upsample (`lerp(hi_mip, blur, 0.68)` — zero energy explosion!)
//   Mode 3: Exposure EV + Additive Bloom + Khronos PBR Neutral / AgX Tonemap (+ 0.99-LSB static IGN sRGB quantization)
//   Mode 4: SMAA Luma Edge Detection (with `2.0 * delta` local contrast adaptation)
//   Mode 5: SMAA Blending Weight Calculation (exact `SearchTex` + `AreaTex` ortho & diagonal search + corner rounding)
//   Mode 6: SMAA Neighborhood Blending + Static 0.99-LSB IGN Quantization
//   Mode 7: Exact 3x3 Ray-Triangle `UnjitterDepth CS` (NaN-safe, near-plane exact, zero Masked cutout dilation!)

cbuffer PostRootConstants : register(b0, space0) {
    uint g_frame_cb_srv;
    uint g_post_mode;
    uint g_src0_srv;
    uint g_src1_srv;
    uint g_src2_srv;
    uint g_src3_srv;
    uint g_dst0_uav;
    uint g_dst1_uav;
    uint g_src_w;
    uint g_src_h;
    uint g_dst_w;
    uint g_dst_h;
    float g_bloom_strength;
    uint g_apply_ign_quant;
};

bool post_finite_f(float x) {
    return (asuint(x) & 0x7f800000u) != 0x7f800000u;
}

float3 rgb_to_ycocg(float3 c) {
    return float3(0.25f * c.r + 0.5f * c.g + 0.25f * c.b, 0.5f * c.r - 0.5f * c.b, -0.25f * c.r + 0.5f * c.g - 0.25f * c.b);
}

float3 ycocg_to_rgb(float3 y) {
    float tmp = y.x - y.z;
    return max(0.0f, float3(tmp + y.y, y.x + y.z, tmp - y.y));
}

float luma_rec709(float3 c) {
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

float3 bloom_soft_knee(float3 c) {
    c = max(c, 0.0f);
    const float thresh = 0.80f;
    const float knee = 0.50f;
    float br = max(c.r, max(c.g, c.b));
    if (br <= 1e-5f)
        return 0.0f;
    float rq = clamp(br - thresh + knee, 0.0f, 2.0f * knee);
    rq = (rq * rq) * (1.0f / (4.0f * knee));
    return c * (max(rq, br - thresh) / br);
}

float karis_weight(float3 avg) {
    return 1.0f / (1.0f + luma_rec709(avg));
}

float2 smaa_decode_diag_bilinear2(float2 e) {
    e.r = e.r * abs(5.0f * e.r - 3.75f);
    return round(e);
}

float4 smaa_decode_diag_bilinear4(float4 e) {
    e.rb = e.rb * abs(5.0f * e.rb - 3.75f);
    return round(e);
}

// ============================================================================
// Authentic Jiménez SMAA Preset High Search & Area Helpers
// ============================================================================
float smaa_search_length(Texture2D<float> search_tex, SamplerState smp, float2 e, float offset) {
    float2 scale = float2(66.0f, 33.0f) * float2(0.5f, -1.0f) + float2(-1.0f, 1.0f);
    float2 bias = float2(66.0f, 33.0f) * float2(offset, 1.0f) + float2(0.5f, -0.5f);
    return search_tex.SampleLevel(smp, mad(scale, e, bias) / float2(64.0f, 16.0f), 0.0f);
}

float smaa_search_x_left(Texture2D<float2> edges_tex, Texture2D<float> search_tex, SamplerState smp,
                         float2 uv, float end_x, float2 inv_rt) {
    float2 e = float2(0.0f, 1.0f);
    [loop] while (uv.x > end_x && e.g > 0.8281f && e.r == 0.0f) {
        e = edges_tex.SampleLevel(smp, uv, 0.0f);
        uv.x -= 2.0f * inv_rt.x;
    }
    float off = mad(-(255.0f / 127.0f), smaa_search_length(search_tex, smp, e, 0.0f), 3.25f);
    return mad(inv_rt.x, off, uv.x);
}

float smaa_search_x_right(Texture2D<float2> edges_tex, Texture2D<float> search_tex, SamplerState smp,
                          float2 uv, float end_x, float2 inv_rt) {
    float2 e = float2(0.0f, 1.0f);
    [loop] while (uv.x < end_x && e.g > 0.8281f && e.r == 0.0f) {
        e = edges_tex.SampleLevel(smp, uv, 0.0f);
        uv.x += 2.0f * inv_rt.x;
    }
    float off = mad(-(255.0f / 127.0f), smaa_search_length(search_tex, smp, e, 0.5f), 3.25f);
    return mad(-inv_rt.x, off, uv.x);
}

float smaa_search_y_up(Texture2D<float2> edges_tex, Texture2D<float> search_tex, SamplerState smp,
                       float2 uv, float end_y, float2 inv_rt) {
    float2 e = float2(1.0f, 0.0f);
    [loop] while (uv.y > end_y && e.r > 0.8281f && e.g == 0.0f) {
        e = edges_tex.SampleLevel(smp, uv, 0.0f);
        uv.y -= 2.0f * inv_rt.y;
    }
    float off = mad(-(255.0f / 127.0f), smaa_search_length(search_tex, smp, e.gr, 0.0f), 3.25f);
    return mad(inv_rt.y, off, uv.y);
}

float smaa_search_y_down(Texture2D<float2> edges_tex, Texture2D<float> search_tex, SamplerState smp,
                         float2 uv, float end_y, float2 inv_rt) {
    float2 e = float2(1.0f, 0.0f);
    [loop] while (uv.y < end_y && e.r > 0.8281f && e.g == 0.0f) {
        e = edges_tex.SampleLevel(smp, uv, 0.0f);
        uv.y += 2.0f * inv_rt.y;
    }
    float off = mad(-(255.0f / 127.0f), smaa_search_length(search_tex, smp, e.gr, 0.5f), 3.25f);
    return mad(-inv_rt.y, off, uv.y);
}

float2 smaa_area(Texture2D<float2> area_tex, SamplerState smp, float2 sqrt_d, float e1, float e2) {
    float2 uv = mad(float2(16.0f, 16.0f), round(4.0f * float2(e1, e2)), sqrt_d);
    return area_tex.SampleLevel(smp, (uv + 0.5f) / float2(160.0f, 560.0f), 0.0f);
}

float2 smaa_search_diag1(Texture2D<float2> edges_tex, SamplerState smp, float2 tc, float2 dir, float2 inv_rt, out float2 e) {
    float4 coord = float4(tc, -1.0f, 1.0f);
    float3 t = float3(inv_rt, 1.0f);
    e = 0.0f;
    [loop] while (coord.z < 7.0f && coord.w > 0.9f) {
        coord.xyz = mad(t, float3(dir, 1.0f), coord.xyz);
        e = edges_tex.SampleLevel(smp, coord.xy, 0.0f);
        coord.w = dot(e, float2(0.5f, 0.5f));
    }
    return coord.zw;
}

float2 smaa_search_diag2(Texture2D<float2> edges_tex, SamplerState smp, float2 tc, float2 dir, float2 inv_rt, out float2 e) {
    float4 coord = float4(tc, -1.0f, 1.0f);
    coord.x += 0.25f * inv_rt.x;
    float3 t = float3(inv_rt, 1.0f);
    e = 0.0f;
    [loop] while (coord.z < 7.0f && coord.w > 0.9f) {
        coord.xyz = mad(t, float3(dir, 1.0f), coord.xyz);
        e = edges_tex.SampleLevel(smp, coord.xy, 0.0f);
        e = smaa_decode_diag_bilinear2(e);
        coord.w = dot(e, float2(0.5f, 0.5f));
    }
    return coord.zw;
}

float2 smaa_area_diag(Texture2D<float2> area_tex, SamplerState smp, float2 dist, float2 e) {
    float2 uv = mad(float2(20.0f, 20.0f), e, dist);
    uv = (uv + 0.5f) / float2(160.0f, 560.0f);
    uv.x += 0.5f;
    return area_tex.SampleLevel(smp, uv, 0.0f);
}

float2 smaa_calc_diag_weights(Texture2D<float2> edges_tex, Texture2D<float2> area_tex, SamplerState smp,
                              float2 tc, float2 e, float2 inv_rt) {
    float2 weights = 0.0f;
    float4 d;
    float2 end;
    if (e.r > 0.0f) {
        d.xz = smaa_search_diag1(edges_tex, smp, tc, float2(-1.0f, 1.0f), inv_rt, end);
        d.x += float(end.y > 0.9f);
    } else {
        d.xz = float2(0.0f, 0.0f);
    }
    d.yw = smaa_search_diag1(edges_tex, smp, tc, float2(1.0f, -1.0f), inv_rt, end);
    if (d.x + d.y > 2.0f) {
        float4 coords = mad(float4(-d.x + 0.25f, d.x, d.y, -d.y - 0.25f), inv_rt.xyxy, tc.xyxy);
        float4 c;
        c.xy = edges_tex.SampleLevel(smp, coords.xy + float2(-inv_rt.x, 0.0f), 0.0f);
        c.zw = edges_tex.SampleLevel(smp, coords.zw + float2(inv_rt.x, 0.0f), 0.0f);
        c.yxwz = smaa_decode_diag_bilinear4(c);
        float2 cc = mad(float2(2.0f, 2.0f), c.xz, c.yw);
        cc = lerp(cc, 0.0f, step(0.9f, d.zw));
        weights += smaa_area_diag(area_tex, smp, d.xy, cc);
    }

    d.xz = smaa_search_diag2(edges_tex, smp, tc, float2(-1.0f, -1.0f), inv_rt, end);
    if (edges_tex.SampleLevel(smp, tc + float2(inv_rt.x, 0.0f), 0.0f).r > 0.0f) {
        d.yw = smaa_search_diag2(edges_tex, smp, tc, float2(1.0f, 1.0f), inv_rt, end);
        d.y += float(end.y > 0.9f);
    } else {
        d.yw = float2(0.0f, 0.0f);
    }
    if (d.x + d.y > 2.0f) {
        float4 coords = mad(float4(-d.x, -d.x, d.y, d.y), inv_rt.xyxy, tc.xyxy);
        float4 c;
        c.x = edges_tex.SampleLevel(smp, coords.xy + float2(-inv_rt.x, 0.0f), 0.0f).g;
        c.y = edges_tex.SampleLevel(smp, coords.xy + float2(0.0f, -inv_rt.y), 0.0f).r;
        c.zw = edges_tex.SampleLevel(smp, coords.zw + float2(inv_rt.x, 0.0f), 0.0f).gr;
        float2 cc = mad(float2(2.0f, 2.0f), c.xz, c.yw);
        cc = lerp(cc, 0.0f, step(0.9f, d.zw));
        weights += smaa_area_diag(area_tex, smp, d.xy, cc).gr;
    }
    return weights;
}

void smaa_detect_horiz_corner(Texture2D<float2> edges_tex, SamplerState smp, inout float2 weights,
                              float4 texcoord, float2 d, float2 inv_rt) {
    float2 lr = step(d.xy, d.yx);
    float2 rounding = (1.0f - 0.25f) * lr;
    rounding /= max(lr.x + lr.y, 1e-5f);
    float2 factor = float2(1.0f, 1.0f);
    factor.x -= rounding.x * edges_tex.SampleLevel(smp, texcoord.xy + float2(0.0f, inv_rt.y), 0.0f).r;
    factor.x -= rounding.y * edges_tex.SampleLevel(smp, texcoord.zw + float2(inv_rt.x, inv_rt.y), 0.0f).r;
    factor.y -= rounding.x * edges_tex.SampleLevel(smp, texcoord.xy + float2(0.0f, -2.0f * inv_rt.y), 0.0f).r;
    factor.y -= rounding.y * edges_tex.SampleLevel(smp, texcoord.zw + float2(inv_rt.x, -2.0f * inv_rt.y), 0.0f).r;
    weights *= saturate(factor);
}

void smaa_detect_vert_corner(Texture2D<float2> edges_tex, SamplerState smp, inout float2 weights,
                             float4 texcoord, float2 d, float2 inv_rt) {
    float2 lr = step(d.xy, d.yx);
    float2 rounding = (1.0f - 0.25f) * lr;
    rounding /= max(lr.x + lr.y, 1e-5f);
    float2 factor = float2(1.0f, 1.0f);
    factor.x -= rounding.x * edges_tex.SampleLevel(smp, texcoord.xy + float2(inv_rt.x, 0.0f), 0.0f).g;
    factor.x -= rounding.y * edges_tex.SampleLevel(smp, texcoord.zw + float2(inv_rt.x, inv_rt.y), 0.0f).g;
    factor.y -= rounding.x * edges_tex.SampleLevel(smp, texcoord.xy + float2(-2.0f * inv_rt.x, 0.0f), 0.0f).g;
    factor.y -= rounding.y * edges_tex.SampleLevel(smp, texcoord.zw + float2(-2.0f * inv_rt.x, inv_rt.y), 0.0f).g;
    weights *= saturate(factor);
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    StructuredBuffer<GlobalFrameConstants> fcb_buf = ResourceDescriptorHeap[atelier_safe_desc(g_frame_cb_srv)];
    GlobalFrameConstants fcb = fcb_buf[0];

    uint2 px = dtid.xy;
    if (px.x >= g_dst_w || px.y >= g_dst_h)
        return;

    SamplerState lin_clamp = SamplerDescriptorHeap[0];

    // ========================================================================
    // Mode 0: Atelier TAAU (YCoCg Ray-Clip + Bounded Inverse-Reinhard + PrevDepth Disocclusion)
    // ========================================================================
    if (g_post_mode == 0u) {
        Texture2D<float4> curr_hdr = ResourceDescriptorHeap[atelier_safe_desc(g_src0_srv)];
        Texture2D<float2> mot_tex = ResourceDescriptorHeap[atelier_safe_desc(g_src2_srv)];
        Texture2D<float> depth_tex = ResourceDescriptorHeap[atelier_safe_desc(g_src3_srv)];
        RWTexture2D<float4> out_hdr = ResourceDescriptorHeap[atelier_safe_desc(g_dst0_uav)];
        RWTexture2D<float4> hist_out = ResourceDescriptorHeap[atelier_safe_desc(g_dst1_uav)];

        float3 center = max(curr_hdr.Load(int3(px, 0)).rgb, 0.0f);
        float2 pxc = float2(px) + 0.5f;
        bool in_vp = (pxc.x >= fcb.scene_viewport.x && pxc.x < fcb.scene_viewport.x + fcb.scene_viewport.z &&
                      pxc.y >= fcb.scene_viewport.y && pxc.y < fcb.scene_viewport.y + fcb.scene_viewport.w);
        if (!in_vp || (fcb.flags & (1u << 7u)) == 0u || g_src1_srv == 0xFFFFFFFFu) {
            out_hdr[px] = float4(center, 1.0f);
            hist_out[px] = float4(center, 1.0f);
            return;
        }

        int2 sc_min = fcb.scissor_rect.xy;
        int2 sc_max = max(sc_min, fcb.scissor_rect.zw - 1);
        float best_z = -1.0f;
        int2 best_px = int2(px);
        float3 m1 = 0.0f;
        float3 m2 = 0.0f;
        float max_hdr_luma = 0.0f;
        [unroll] for (int dy = -1; dy <= 1; ++dy) {
            [unroll] for (int dx = -1; dx <= 1; ++dx) {
                int2 q = clamp(int2(px) + int2(dx, dy), sc_min, sc_max);
                float qz = depth_tex.Load(int3(q, 0));
                if (qz > best_z) {
                    best_z = qz;
                    best_px = q;
                }
                float3 s_hdr = max(curr_hdr.Load(int3(q, 0)).rgb, 0.0f);
                float l = luma_rec709(s_hdr);
                max_hdr_luma = max(max_hdr_luma, l);
                float3 ycc = rgb_to_ycocg(s_hdr / (1.0f + l));
                m1 += ycc;
                m2 += ycc * ycc;
            }
        }

        // Do not accumulate temporal history on pure black non-sky background (`scene-lit.md:21`):
        bool sky_bg_on = (((fcb.flags & (1u << 5u)) != 0u) && (fcb.sky_map_srv != 0xFFFFFFFFu)) || (fcb.env_sky_mode == 1u);
        if (best_z <= 0.0f && !sky_bg_on) {
            out_hdr[px] = float4(center, 1.0f);
            hist_out[px] = float4(center, 1.0f);
            return;
        }

        float2 mot = mot_tex.Load(int3(best_px, 0));
        float2 prev_uv = (pxc / float2(g_dst_w, g_dst_h)) + mot;
        float2 prev_px = prev_uv * float2(g_dst_w, g_dst_h);
        if (prev_px.x < fcb.prev_scene_viewport.x || prev_px.x >= fcb.prev_scene_viewport.x + fcb.prev_scene_viewport.z ||
            prev_px.y < fcb.prev_scene_viewport.y || prev_px.y >= fcb.prev_scene_viewport.y + fcb.prev_scene_viewport.w) {
            out_hdr[px] = float4(center, 1.0f);
            hist_out[px] = float4(center, 1.0f);
            return;
        }

        float disocc_boost = 0.0f;
        if (best_z > 0.0f && g_src_w != 0xFFFFFFFFu && fcb.prev_view_z_srv != 0xFFFFFFFFu) {
            Texture2D<float> exp_pz_tex = ResourceDescriptorHeap[atelier_safe_desc(g_src_w)];
            Texture2D<float> act_pz_tex = ResourceDescriptorHeap[atelier_safe_desc(fcb.prev_view_z_srv)];
            SamplerState pt_smp = SamplerDescriptorHeap[3];
            float exp_pz = exp_pz_tex.Load(int3(best_px, 0));
            float act_pz = act_pz_tex.SampleLevel(pt_smp, prev_uv, 0.0f);
            if (act_pz <= 0.0f || abs(act_pz - exp_pz) > max(2e-5f, max(act_pz, exp_pz) * 0.08f))
                disocc_boost = 0.55f;
        }

        Texture2D<float4> prev_taa = ResourceDescriptorHeap[atelier_safe_desc(g_src1_srv)];
        float3 hist_rgb = max(prev_taa.SampleLevel(lin_clamp, prev_uv, 0.0f).rgb * fcb.exposure_ratio, 0.0f);
        float3 hist_ycc = rgb_to_ycocg(hist_rgb / (1.0f + luma_rec709(hist_rgb)));

        // Chroma-preserving YCoCg ray-clip toward 3x3 neighborhood mean `mu`:
        float3 mu = m1 * (1.0f / 9.0f);
        float3 sigma = sqrt(max(0.0f, m2 * (1.0f / 9.0f) - mu * mu));
        float3 delta_h = hist_ycc - mu;
        float3 unit_d = abs(delta_h) / max(sigma * 1.25f, 1e-5f);
        float max_unit = max(unit_d.x, max(unit_d.y, unit_d.z));
        float3 clipped_ycc = (max_unit > 1.0f) ? (mu + delta_h / max_unit) : hist_ycc;

        float3 clipped_tm = ycocg_to_rgb(clipped_ycc);
        float min_denom = max(1.0f / (1.0f + max_hdr_luma), 1e-3f);
        float3 clipped_hdr = clipped_tm / max(1.0f - luma_rec709(clipped_tm), min_denom);

        float vel_px = length(mot * float2(g_dst_w, g_dst_h));
        float alpha = saturate(lerp(0.08f, 0.25f, saturate(vel_px * 0.2f)) + disocc_boost);
        float3 resolved = lerp(clipped_hdr, center, alpha);
        out_hdr[px] = float4(resolved, 1.0f);
        hist_out[px] = float4(resolved, 1.0f);
        return;
    }

    // ========================================================================
    // Mode 1: 13-Tap Jiménez Soft-Knee Karis 6-Mip Bloom Downsample
    // ========================================================================
    if (g_post_mode == 1u) {
        Texture2D<float4> src = ResourceDescriptorHeap[atelier_safe_desc(g_src0_srv)];
        RWTexture2D<float4> dst = ResourceDescriptorHeap[atelier_safe_desc(g_dst0_uav)];
        float2 uv = (float2(px) + 0.5f) / float2(g_dst_w, g_dst_h);
        float2 d = 1.0f / float2(g_src_w, g_src_h);

        float3 a = src.SampleLevel(lin_clamp, uv + float2(-2.0f, -2.0f) * d, 0.0f).rgb;
        float3 b = src.SampleLevel(lin_clamp, uv + float2( 0.0f, -2.0f) * d, 0.0f).rgb;
        float3 c = src.SampleLevel(lin_clamp, uv + float2( 2.0f, -2.0f) * d, 0.0f).rgb;
        float3 d0 = src.SampleLevel(lin_clamp, uv + float2(-1.0f, -1.0f) * d, 0.0f).rgb;
        float3 e0 = src.SampleLevel(lin_clamp, uv + float2( 1.0f, -1.0f) * d, 0.0f).rgb;
        float3 f = src.SampleLevel(lin_clamp, uv + float2(-2.0f,  0.0f) * d, 0.0f).rgb;
        float3 g = src.SampleLevel(lin_clamp, uv, 0.0f).rgb;
        float3 h = src.SampleLevel(lin_clamp, uv + float2( 2.0f,  0.0f) * d, 0.0f).rgb;
        float3 i = src.SampleLevel(lin_clamp, uv + float2(-1.0f,  1.0f) * d, 0.0f).rgb;
        float3 j = src.SampleLevel(lin_clamp, uv + float2( 1.0f,  1.0f) * d, 0.0f).rgb;
        float3 k = src.SampleLevel(lin_clamp, uv + float2(-2.0f,  2.0f) * d, 0.0f).rgb;
        float3 l = src.SampleLevel(lin_clamp, uv + float2( 0.0f,  2.0f) * d, 0.0f).rgb;
        float3 m = src.SampleLevel(lin_clamp, uv + float2( 2.0f,  2.0f) * d, 0.0f).rgb;

        if (g_apply_ign_quant != 0u) {
            a = bloom_soft_knee(a); b = bloom_soft_knee(b); c = bloom_soft_knee(c);
            d0 = bloom_soft_knee(d0); e0 = bloom_soft_knee(e0);
            f = bloom_soft_knee(f); g = bloom_soft_knee(g); h = bloom_soft_knee(h);
            i = bloom_soft_knee(i); j = bloom_soft_knee(j);
            k = bloom_soft_knee(k); l = bloom_soft_knee(l); m = bloom_soft_knee(m);

            float3 b0 = (d0 + e0 + i + j) * 0.25f;
            float3 b1 = (a + b + f + g) * 0.25f;
            float3 b2 = (b + c + g + h) * 0.25f;
            float3 b3 = (f + g + k + l) * 0.25f;
            float3 b4 = (g + h + l + m) * 0.25f;
            float w0 = 0.50f * karis_weight(b0);
            float w1 = 0.125f * karis_weight(b1);
            float w2 = 0.125f * karis_weight(b2);
            float w3 = 0.125f * karis_weight(b3);
            float w4 = 0.125f * karis_weight(b4);
            float w_sum = max(w0 + w1 + w2 + w3 + w4, 1e-5f);
            float3 res = (b0 * w0 + b1 * w1 + b2 * w2 + b3 * w3 + b4 * w4) / w_sum;
            dst[px] = float4(res, 1.0f);
        } else {
            float3 res = (d0 + e0 + i + j) * 0.125f +
                         (a + c + k + m) * 0.03125f +
                         (b + f + h + l) * 0.0625f +
                         g * 0.125f;
            dst[px] = float4(res, 1.0f);
        }
        return;
    }

    // ========================================================================
    // Mode 2: Normalized Unit-Gain Bloom Tent Upsample
    // ========================================================================
    if (g_post_mode == 2u) {
        Texture2D<float4> lo_mip = ResourceDescriptorHeap[atelier_safe_desc(g_src0_srv)];
        Texture2D<float4> hi_mip = ResourceDescriptorHeap[atelier_safe_desc(g_src1_srv)];
        RWTexture2D<float4> dst = ResourceDescriptorHeap[atelier_safe_desc(g_dst0_uav)];
        float2 uv = (float2(px) + 0.5f) / float2(g_dst_w, g_dst_h);
        float2 d = 1.0f / float2(g_src_w, g_src_h);
        float3 blur = lo_mip.SampleLevel(lin_clamp, uv + float2(-d.x, -d.y), 0.0f).rgb +
                      lo_mip.SampleLevel(lin_clamp, uv + float2( 0.0f, -d.y), 0.0f).rgb * 2.0f +
                      lo_mip.SampleLevel(lin_clamp, uv + float2( d.x, -d.y), 0.0f).rgb +
                      lo_mip.SampleLevel(lin_clamp, uv + float2(-d.x,  0.0f), 0.0f).rgb * 2.0f +
                      lo_mip.SampleLevel(lin_clamp, uv, 0.0f).rgb * 4.0f +
                      lo_mip.SampleLevel(lin_clamp, uv + float2( d.x,  0.0f), 0.0f).rgb * 2.0f +
                      lo_mip.SampleLevel(lin_clamp, uv + float2(-d.x,  d.y), 0.0f).rgb +
                      lo_mip.SampleLevel(lin_clamp, uv + float2( 0.0f,  d.y), 0.0f).rgb * 2.0f +
                      lo_mip.SampleLevel(lin_clamp, uv + float2( d.x,  d.y), 0.0f).rgb;
        blur *= (1.0f / 16.0f);
        dst[px] = float4(lerp(hi_mip.Load(int3(px, 0)).rgb, blur, 0.68f), 1.0f);
        return;
    }

    // ========================================================================
    // Mode 3: Exposure EV + Additive Bloom + Khronos PBR Neutral (default) / AgX Tonemap
    // ========================================================================
    if (g_post_mode == 3u) {
        Texture2D<float4> hdr_tex = ResourceDescriptorHeap[atelier_safe_desc(g_src0_srv)];
        RWTexture2D<float4> sdr_uav = ResourceDescriptorHeap[atelier_safe_desc(g_dst0_uav)];
        float2 pxc = float2(px) + 0.5f;
        bool in_vp = (pxc.x >= fcb.scene_viewport.x && pxc.x < fcb.scene_viewport.x + fcb.scene_viewport.z &&
                      pxc.y >= fcb.scene_viewport.y && pxc.y < fcb.scene_viewport.y + fcb.scene_viewport.w);
        if (!in_vp) {
            sdr_uav[px] = 0.0f;
            return;
        }

        // `hdr_tex` (`resolved_hdr` <- `scene_hdr`) is already pre-exposed by `fcb.exposure_scale`:
        float3 hdr = max(hdr_tex.Load(int3(px, 0)).rgb, 0.0f);
        if ((fcb.flags & (1u << 4u)) != 0u && g_src1_srv != 0xFFFFFFFFu) {
            Texture2D<float4> bloom_tex = ResourceDescriptorHeap[atelier_safe_desc(g_src1_srv)];
            float2 uv = pxc / float2(g_dst_w, g_dst_h);
            hdr += bloom_tex.SampleLevel(lin_clamp, uv, 0.0f).rgb * g_bloom_strength;
        }
        float3 sdr = ((fcb.flags & (1u << 8u)) != 0u) ? tonemap_agx(hdr) : tonemap_khronos_pbr_neutral(hdr);
        if (g_apply_ign_quant != 0u)
            sdr = quantize_srgb_ign(sdr, px);
        sdr_uav[px] = float4(sdr, 1.0f);
        return;
    }

    // ========================================================================
    // Modes 4, 5, 6: Authentic Jiménez SMAA (`SearchTex` + `AreaTex` + Diag/Corners)
    // ========================================================================
    float2 pxc_smaa = float2(px) + 0.5f;
    bool in_vp_smaa = (pxc_smaa.x >= fcb.scene_viewport.x && pxc_smaa.x < fcb.scene_viewport.x + fcb.scene_viewport.z &&
                       pxc_smaa.y >= fcb.scene_viewport.y && pxc_smaa.y < fcb.scene_viewport.y + fcb.scene_viewport.w);
    int2 sc_min = fcb.scissor_rect.xy;
    int2 sc_max = max(sc_min, fcb.scissor_rect.zw - 1);

    if (g_post_mode == 4u) {
        RWTexture2D<float2> edges_uav = ResourceDescriptorHeap[atelier_safe_desc(g_dst0_uav)];
        if (!in_vp_smaa) {
            edges_uav[px] = 0.0f;
            return;
        }
        Texture2D<float4> sdr = ResourceDescriptorHeap[atelier_safe_desc(g_src0_srv)];
        const float threshold = 0.05f;
        int2 ipx = int2(px);
        float L = sqrt(max(luma_rec709(sdr.Load(int3(ipx, 0)).rgb), 0.0f));
        float Lleft = sqrt(max(luma_rec709(sdr.Load(int3(clamp(ipx + int2(-1, 0), sc_min, sc_max), 0)).rgb), 0.0f));
        float Ltop = sqrt(max(luma_rec709(sdr.Load(int3(clamp(ipx + int2(0, -1), sc_min, sc_max), 0)).rgb), 0.0f));
        float2 delta = abs(L - float2(Lleft, Ltop));
        float2 edges = step(threshold.xx, delta);
        if (dot(edges, 1.0f) == 0.0f) {
            edges_uav[px] = 0.0f;
            return;
        }
        float Lright = sqrt(max(luma_rec709(sdr.Load(int3(clamp(ipx + int2(1, 0), sc_min, sc_max), 0)).rgb), 0.0f));
        float Lbottom = sqrt(max(luma_rec709(sdr.Load(int3(clamp(ipx + int2(0, 1), sc_min, sc_max), 0)).rgb), 0.0f));
        float Lleftleft = sqrt(max(luma_rec709(sdr.Load(int3(clamp(ipx + int2(-2, 0), sc_min, sc_max), 0)).rgb), 0.0f));
        float Ltoptop = sqrt(max(luma_rec709(sdr.Load(int3(clamp(ipx + int2(0, -2), sc_min, sc_max), 0)).rgb), 0.0f));
        float2 delta_rb = abs(L - float2(Lright, Lbottom));
        float2 delta_lltt = abs(float2(Lleft, Ltop) - float2(Lleftleft, Ltoptop));
        float2 max_delta = max(max(delta, delta_rb), delta_lltt);
        float final_delta = max(max_delta.x, max_delta.y);
        edges *= step(final_delta.xx, 2.0f * delta);
        edges_uav[px] = edges;
        return;
    }

    if (g_post_mode == 5u) {
        RWTexture2D<float4> weights_uav = ResourceDescriptorHeap[atelier_safe_desc(g_dst0_uav)];
        if (!in_vp_smaa) {
            weights_uav[px] = 0.0f;
            return;
        }
        Texture2D<float2> edges_tex = ResourceDescriptorHeap[atelier_safe_desc(g_src0_srv)];
        Texture2D<float2> area_tex = ResourceDescriptorHeap[atelier_safe_desc(fcb.smaa_area_srv)];
        Texture2D<float> search_tex = ResourceDescriptorHeap[atelier_safe_desc(fcb.smaa_search_srv)];

        float2 e = edges_tex.Load(int3(px, 0));
        if (dot(e, 1.0f) == 0.0f) {
            weights_uav[px] = 0.0f;
            return;
        }

        float2 inv_rt = 1.0f / float2(g_dst_w, g_dst_h);
        float2 tc = pxc_smaa * inv_rt;
        float4 weights = 0.0f;

        if (e.g > 0.0f) {
            weights.rg = smaa_calc_diag_weights(edges_tex, area_tex, lin_clamp, tc, e, inv_rt);
            if (dot(weights.rg, 1.0f) == 0.0f) {
                float2 d;
                float3 coords;
                coords.x = smaa_search_x_left(edges_tex, search_tex, lin_clamp, tc + float2(-0.25f, -0.125f) * inv_rt,
                                              tc.x - 32.0f * inv_rt.x, inv_rt);
                coords.y = tc.y - 0.25f * inv_rt.y;
                d.x = coords.x;
                float e1 = edges_tex.SampleLevel(lin_clamp, coords.xy, 0.0f).r;

                coords.z = smaa_search_x_right(edges_tex, search_tex, lin_clamp, tc + float2(1.25f, -0.125f) * inv_rt,
                                               tc.x + 32.0f * inv_rt.x, inv_rt);
                d.y = coords.z;
                d = abs(round(mad(float2(g_dst_w, g_dst_w), d, -pxc_smaa.xx)));

                float2 sqrt_d = sqrt(d);
                float e2 = edges_tex.SampleLevel(lin_clamp, float2(coords.z + inv_rt.x, coords.y), 0.0f).r;
                weights.rg = smaa_area(area_tex, lin_clamp, sqrt_d, e1, e2);
                coords.y = tc.y;
                smaa_detect_horiz_corner(edges_tex, lin_clamp, weights.rg, coords.xyzy, d, inv_rt);
            } else {
                e.r = 0.0f;
            }
        }

        if (e.r > 0.0f) {
            float2 d;
            float3 coords;
            coords.y = smaa_search_y_up(edges_tex, search_tex, lin_clamp, tc + float2(-0.125f, -0.25f) * inv_rt,
                                        tc.y - 32.0f * inv_rt.y, inv_rt);
            coords.x = tc.x - 0.25f * inv_rt.x;
            d.x = coords.y;
            float e1 = edges_tex.SampleLevel(lin_clamp, coords.xy, 0.0f).g;

            coords.z = smaa_search_y_down(edges_tex, search_tex, lin_clamp, tc + float2(-0.125f, 1.25f) * inv_rt,
                                          tc.y + 32.0f * inv_rt.y, inv_rt);
            d.y = coords.z;
            d = abs(round(mad(float2(g_dst_h, g_dst_h), d, -pxc_smaa.yy)));

            float2 sqrt_d = sqrt(d);
            float e2 = edges_tex.SampleLevel(lin_clamp, float2(coords.x, coords.z + inv_rt.y), 0.0f).g;
            weights.ba = smaa_area(area_tex, lin_clamp, sqrt_d, e1, e2);
            coords.x = tc.x;
            smaa_detect_vert_corner(edges_tex, lin_clamp, weights.ba, coords.xyxz, d, inv_rt);
        }

        weights_uav[px] = weights;
        return;
    }

    if (g_post_mode == 6u) {
        RWTexture2D<float4> out_uav = ResourceDescriptorHeap[atelier_safe_desc(g_dst0_uav)];
        if (!in_vp_smaa) {
            out_uav[px] = 0.0f;
            return;
        }
        Texture2D<float4> sdr_tex = ResourceDescriptorHeap[atelier_safe_desc(g_src0_srv)];
        Texture2D<float4> w_tex = ResourceDescriptorHeap[atelier_safe_desc(g_src1_srv)];
        int2 ipx = int2(px);
        float4 w_tl = w_tex.Load(int3(ipx, 0));
        float4 a;
        a.x = w_tex.Load(int3(clamp(ipx + int2(1, 0), sc_min, sc_max), 0)).a; // Right (+X)
        a.y = w_tex.Load(int3(clamp(ipx + int2(0, 1), sc_min, sc_max), 0)).g; // Bottom (+Y)
        a.wz = w_tl.xz;                                                        // Top (-Y) & Left (-X)
        float3 color = sdr_tex.Load(int3(ipx, 0)).rgb;
        if (dot(a, 1.0f) >= 1e-5f) {
            bool horiz = max(a.x, a.z) > max(a.y, a.w);
            float4 blending_offset = horiz ? float4(a.x, 0.0f, a.z, 0.0f) : float4(0.0f, a.y, 0.0f, a.w);
            float2 blending_weight = horiz ? a.xz : a.yw;
            blending_weight /= max(blending_weight.x + blending_weight.y, 1e-5f);
            float2 texel = 1.0f / float2(g_dst_w, g_dst_h);
            float2 uv_min = (float2(sc_min) + 0.5f) * texel;
            float2 uv_max = (float2(sc_max) + 0.5f) * texel;
            float2 base_uv = pxc_smaa * texel;
            float2 uv0 = clamp(base_uv + blending_offset.xy * texel, uv_min, uv_max);
            float2 uv1 = clamp(base_uv - blending_offset.zw * texel, uv_min, uv_max);
            color = sdr_tex.SampleLevel(lin_clamp, uv0, 0.0f).rgb * blending_weight.x +
                    sdr_tex.SampleLevel(lin_clamp, uv1, 0.0f).rgb * blending_weight.y;
        }
        out_uav[px] = float4(quantize_srgb_ign(color, px), 1.0f);
        return;
    }

    // ========================================================================
    // Mode 7: Exact 3x3 Ray-Triangle `UnjitterDepth CS` (zero scene redraw, zero Masked dilation!)
    // ========================================================================
    if (g_post_mode == 7u) {
        Texture2D<uint> vis_tex = ResourceDescriptorHeap[atelier_safe_desc(g_src0_srv)];
        StructuredBuffer<GpuVisibleCluster> opaque_q = ResourceDescriptorHeap[atelier_safe_desc(g_src1_srv)];
        StructuredBuffer<GpuVisibleCluster> masked_q = ResourceDescriptorHeap[atelier_safe_desc(g_src2_srv)];
        RWTexture2D<float> unjit_z_uav = ResourceDescriptorHeap[atelier_safe_desc(g_dst0_uav)];

        float2 pxc = float2(px) + 0.5f;
        bool in_vp = (pxc.x >= fcb.scene_viewport.x && pxc.x < fcb.scene_viewport.x + fcb.scene_viewport.z &&
                      pxc.y >= fcb.scene_viewport.y && pxc.y < fcb.scene_viewport.y + fcb.scene_viewport.w);
        if (!in_vp) {
            unjit_z_uav[px] = 0.0f;
            return;
        }

        float2 ndc = float2((pxc.x - fcb.scene_viewport.x) / max(fcb.scene_viewport.z, 1.0f) * 2.0f - 1.0f,
                            1.0f - (pxc.y - fcb.scene_viewport.y) / max(fcb.scene_viewport.w, 1.0f) * 2.0f);

        StructuredBuffer<GpuInstanceRecord> instances = ResourceDescriptorHeap[atelier_safe_desc(fcb.instances_srv)];
        StructuredBuffer<GpuMeshDescriptor> meshes = ResourceDescriptorHeap[atelier_safe_desc(fcb.mesh_table_srv)];

        float best_z = 0.0f;
        uint tested[9];
        uint tested_cnt = 0u;

        [unroll] for (int dy = -1; dy <= 1; ++dy) {
            [unroll] for (int dx = -1; dx <= 1; ++dx) {
                int2 q = clamp(int2(px) + int2(dx, dy), sc_min, sc_max);
                uint vis = vis_tex.Load(int3(q, 0));
                if (vis == VISBUFFER_EMPTY)
                    continue;
                uint cluster_idx = (vis >> 8u) - 1u;
                bool is_masked_tri = (cluster_idx & 0x800000u) != 0u;
                if (is_masked_tri) {
                    // Prevent 1px silhouette dilation into alpha-cutout holes on `Coverage::mask` triangles!
                    float2 q_unjit = float2(q) + 0.5f - fcb.jitter_pixels;
                    if (abs(q_unjit.x - pxc.x) > 0.5001f || abs(q_unjit.y - pxc.y) > 0.5001f)
                        continue;
                }

                uint tri_key = vis >> 1u;
                bool seen = false;
                [loop] for (uint k = 0u; k < tested_cnt; ++k) {
                    if (tested[k] == tri_key) {
                        seen = true;
                        break;
                    }
                }
                if (seen)
                    continue;
                tested[tested_cnt++] = tri_key;

                uint local_tri = (vis >> 1u) & 0x7Fu;
                GpuVisibleCluster vc;
                if (is_masked_tri)
                    vc = masked_q[cluster_idx & 0x7FFFFFu];
                else
                    vc = opaque_q[cluster_idx];
                GpuInstanceRecord inst = instances[vc.instance_id];
                GpuMeshDescriptor mesh = meshes[inst.mesh_pool_index];
                StructuredBuffer<GpuMeshlet> meshlets = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(mesh.meshlets_srv))];
                uint global_tri = meshlets[vc.meshlet_id].global_tri_base + local_tri;

                StructuredBuffer<uint> blas_idx = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(mesh.blas_indices_srv))];
                StructuredBuffer<GpuPackedPosition> pos_s = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(mesh.positions_srv))];
                float3 lp0 = pos_s[blas_idx[global_tri * 3u + 0u]].pos;
                float3 lp1 = pos_s[blas_idx[global_tri * 3u + 1u]].pos;
                float3 lp2 = pos_s[blas_idx[global_tri * 3u + 2u]].pos;

                precise float4 c0 = mul(mul(float4(lp0, 1.0f), inst.world), fcb.unjittered_view_projection);
                precise float4 c1 = mul(mul(float4(lp1, 1.0f), inst.world), fcb.unjittered_view_projection);
                precise float4 c2 = mul(mul(float4(lp2, 1.0f), inst.world), fcb.unjittered_view_projection);

                float2 s0 = c0.xy - ndc * c0.w;
                float2 s1 = c1.xy - ndc * c1.w;
                float2 s2 = c2.xy - ndc * c2.w;
                float2 d1 = s1 - s0;
                float2 d2 = s2 - s0;
                float det = d1.x * d2.y - d1.y * d2.x;
                if (!(abs(det) > 1e-20f) || !post_finite_f(det))
                    continue;
                float inv_det = 1.0f / det;
                float b1 = (-s0.x * d2.y + s0.y * d2.x) * inv_det;
                float b2 = (d1.x * (-s0.y) - d1.y * (-s0.x)) * inv_det;
                float b0 = 1.0f - b1 - b2;
                if (b0 >= -1e-5f && b1 >= -1e-5f && b2 >= -1e-5f) {
                    float cz = b0 * c0.z + b1 * c1.z + b2 * c2.z;
                    float cw = b0 * c0.w + b1 * c1.w + b2 * c2.w;
                    if (cw > 1e-6f) {
                        float dz_cand = cz / cw;
                        if (post_finite_f(dz_cand) && dz_cand >= fcb.camera_far_depth - 1e-6f && dz_cand <= 1.0f + 1e-5f)
                            best_z = max(best_z, saturate(dz_cand));
                    }
                }
            }
        }
        unjit_z_uav[px] = best_z;
    }
}
