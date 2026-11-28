// 3-Tier Macro/Meso Shadow System (Software-Paged VSM + Compact PenumbraQueue RayQuery + Screen ContactShadow):
//   Mode 0: Screen-Space ContactShadow CS (perspective-exact ray depth + finite thickness rejection)
//   Mode 5: Clear PenumbraQueue Counter (`penumbra_indirect[12u] = 0`)
//   Mode 1: Conservative Plane-Predicted VSM Classifier + 64-Bit LDS Order-Preserving PenumbraQueue Compaction CS
//   Mode 2: Finalize PenumbraQueue Indirect Dispatch Args (`1024`-group 2D grid)
//   Mode 3: Compact PenumbraQueue DXR 1.1 Inline RayQuery CS (2-spp stratified area penumbra + Opaque/Masked AnyHit)
//   Mode 4: 3x3 Depth/Normal Edge-Preserving Bilateral Penumbra Filter CS (`RG8_UNORM`)

#ifndef RAY_HAS_MASKED
#define RAY_HAS_MASKED 0
#endif

#if !RAY_HAS_MASKED
cbuffer ShadowRootConstants : register(b0, space0) {
    uint g_frame_cb_srv;
    uint g_shadow_mode;
    uint g_depth_srv;
    uint g_geom_normal_srv;
    uint g_shadow_mask_uav;
    uint g_ray_counter_uav;
    uint g_slab0_srv;
    uint g_cluster_list_srv;
    uint g_penumbra_tex_uav_or_srv;
    uint g_penumbra_queue_uav_or_srv;
    uint g_penumbra_args_uav_or_srv;
    uint g_max_penumbra_items;
};
#endif

float3 reconstruct_world_pos_from_depth(uint2 px, float device_z, GlobalFrameConstants fcb) {
    float2 px_c = float2(px) + 0.5f;
    float2 ndc = float2((px_c.x - fcb.scene_viewport.x) / max(fcb.scene_viewport.z, 1.0f) * 2.0f - 1.0f,
                        1.0f - (px_c.y - fcb.scene_viewport.y) / max(fcb.scene_viewport.w, 1.0f) * 2.0f);
    precise float4 wp = mul(float4(ndc, device_z, 1.0f), fcb.inv_view_projection);
    float ww = abs(wp.w) > 1e-7f ? wp.w : (wp.w >= 0.0f ? 1e-7f : -1e-7f);
    return wp.xyz / ww;
}

bool trace_shadow_ray(float3 origin, float3 dir, float t_max, GlobalFrameConstants fcb, inout uint traced_rays) {
    if (fcb.tlas_srv == 0xFFFFFFFFu || t_max <= 1e-4f)
        return true;

    ++traced_rays;
    RaytracingAccelerationStructure tlas = ResourceDescriptorHeap[atelier_safe_desc(fcb.tlas_srv)];
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 1e-4f;
    ray.TMax = t_max;

#if RAY_HAS_MASKED
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(tlas, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFFu, ray);
    StructuredBuffer<GpuInstanceRecord> instances = ResourceDescriptorHeap[atelier_safe_desc(fcb.instances_srv)];
    StructuredBuffer<GpuMeshDescriptor> meshes = ResourceDescriptorHeap[atelier_safe_desc(fcb.mesh_table_srv)];
    StructuredBuffer<uint> part_bases = ResourceDescriptorHeap[atelier_safe_desc(fcb.part_tri_base_srv)];
    StructuredBuffer<uint> slot_map = ResourceDescriptorHeap[atelier_safe_desc(fcb.material_slot_map_srv)];
    StructuredBuffer<GpuSurfaceRecord> surfaces = ResourceDescriptorHeap[atelier_safe_desc(fcb.material_surfaces_srv)];
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            uint inst_id = q.CandidateInstanceID();
            GpuInstanceRecord inst = instances[inst_id];
            GpuMeshDescriptor mesh = meshes[inst.mesh_pool_index];
            uint global_tri = part_bases[inst.part_table_base + q.CandidateGeometryIndex()] + q.CandidatePrimitiveIndex();

            StructuredBuffer<uint> blas_idx = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(mesh.blas_indices_srv))];
            StructuredBuffer<GpuPackedPosition> pos_s = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(mesh.positions_srv))];
            StructuredBuffer<GpuPackedAttributes> attr_s = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(mesh.attributes_srv))];
            StructuredBuffer<uint> fmeta_s = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(mesh.face_meta_srv))];

            uint i0 = blas_idx[global_tri * 3u + 0u];
            uint i1 = blas_idx[global_tri * 3u + 1u];
            uint i2 = blas_idx[global_tri * 3u + 2u];
            uint fmeta = fmeta_s[global_tri];
            uint surf_idx = slot_map[inst.material_slot_offset + (fmeta & 0x07FFFFFFu)];
            GpuSurfaceRecord surf = surfaces[surf_idx];

            float2 b = q.CandidateTriangleBarycentrics();
            float w0 = 1.0f - b.x - b.y;
            GpuPackedAttributes a0 = attr_s[i0], a1 = attr_s[i1], a2 = attr_s[i2];
            float3 lp0 = pos_s[i0].pos, lp1 = pos_s[i1].pos, lp2 = pos_s[i2].pos;
            float2 uv = a0.uv * w0 + a1.uv * b.x + a2.uv * b.y;
            float3 wp = mul(float4(lp0 * w0 + lp1 * b.x + lp2 * b.y, 1.0f), inst.world).xyz;
            float3 local_n0 = unpack_oct_snorm16(a0.oct_normal);
            float3 nw0 = transform_local_normal(local_n0, inst);
            float3 nw1 = transform_local_normal(unpack_oct_snorm16(a1.oct_normal), inst);
            float3 nw2 = transform_local_normal(unpack_oct_snorm16(a2.oct_normal), inst);
            float3 nw = safe_unit_normal(nw0 * w0 + nw1 * b.x + nw2 * b.y);
            float3 gw = reconstruct_geom_normal(lp0, lp1, lp2, local_n0, (fmeta >> 31u) != 0u, inst);
            float4 tw0 = transform_local_tangent(unpack_oct_tangent_sign(a0.oct_tangent_sign), inst);
            float4 tw1 = transform_local_tangent(unpack_oct_tangent_sign(a1.oct_tangent_sign), inst);
            float4 tw2 = transform_local_tangent(unpack_oct_tangent_sign(a2.oct_tangent_sign), inst);
            float4 interp_t = tw0 * w0 + tw1 * b.x + tw2 * b.y;
            float4 tw = float4(interp_t.xyz, interp_t.w < 0.0f ? -1.0f : 1.0f);

            float cov = eval_masked_ray_coverage(surf.masked_cov_index, surf.input_base, surf.parameter_base, uv, wp, nw, gw, tw);
            if (cov >= 0.0f)
                q.CommitNonOpaqueTriangleHit();
        }
    }
    return q.CommittedStatus() == COMMITTED_NOTHING;
#else
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(tlas, RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFFu, ray);
    q.Proceed();
    return q.CommittedStatus() == COMMITTED_NOTHING;
#endif
}

bool evaluate_light_shadow_ray(GpuLightRecord l, float3 wpos, float3 gn, bool is_foliage, bool jitter_penumbra,
                               float ign, float ign2, GlobalFrameConstants fcb, inout uint traced_rays) {
    float3 origin;
    float3 dir;
    float t_max;
    int prep = build_analytic_shadow_ray(l, wpos, gn, is_foliage, jitter_penumbra, ign, ign2, origin, dir, t_max);
    if (prep >= 0)
        return prep != 0;
    return trace_shadow_ray(origin, dir, t_max, fcb, traced_rays);
}

groupshared uint gs_pen_mask[2];
groupshared uint gs_pen_base;
groupshared uint gs_pen_ray_sum;

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID, uint gindex : SV_GroupIndex, uint3 gid : SV_GroupID) {
    StructuredBuffer<GlobalFrameConstants> fcb_buf = ResourceDescriptorHeap[atelier_safe_desc(g_frame_cb_srv)];
    GlobalFrameConstants fcb = fcb_buf[0];

#if RAY_HAS_MASKED
    uint mode = g_pass_flags;
    uint depth_srv = g_bin_offset_or_cluster_srv;
    uint geom_n_srv = g_aux0_srv;
    uint slab0_srv = g_aux1_srv;
    uint cluster_srv = g_frame_cb_idx;
    uint out_uav = g_aux2_uav;
    uint counter_uav = g_aux3_uav;
    uint pen_tex_idx = g_aux4_uav;
    uint pen_queue_idx = g_aux5_uav;
    uint pen_args_idx = g_archetype_index;
    uint max_pen_items = g_viewport_packed0;
#else
    uint mode = g_shadow_mode;
    uint depth_srv = g_depth_srv;
    uint geom_n_srv = g_geom_normal_srv;
    uint slab0_srv = g_slab0_srv;
    uint cluster_srv = g_cluster_list_srv;
    uint out_uav = g_shadow_mask_uav;
    uint counter_uav = g_ray_counter_uav;
    uint pen_tex_idx = g_penumbra_tex_uav_or_srv;
    uint pen_queue_idx = g_penumbra_queue_uav_or_srv;
    uint pen_args_idx = g_penumbra_args_uav_or_srv;
    uint max_pen_items = g_max_penumbra_items;
#endif

#if !RAY_HAS_MASKED
    if (mode == 5u) {
        if (gindex == 0u && gid.x == 0u && gid.y == 0u) {
            RWByteAddressBuffer pen_args = ResourceDescriptorHeap[atelier_safe_desc(pen_args_idx)];
            pen_args.Store4(0u, uint4(0u, 1u, 1u, 0u));
        }
        return;
    }

    if (mode == 2u) {
        if (gindex == 0u && gid.x == 0u && gid.y == 0u) {
            RWByteAddressBuffer pen_args = ResourceDescriptorHeap[atelier_safe_desc(pen_args_idx)];
            uint cnt = min(pen_args.Load(12u), max_pen_items);
            pen_args.Store(12u, cnt);
            uint groups = (cnt + 63u) >> 6u;
            uint gx = (groups == 0u) ? 0u : ((groups <= 65535u) ? groups : 1024u);
            uint gy = (groups == 0u) ? 1u : ((groups <= 65535u) ? 1u : ((groups + 1023u) >> 10u));
            pen_args.Store3(0u, uint3(gx, gy, 1u));
        }
        return;
    }

    uint2 tile8_px_off = uint2(fcb.scene_tile8_offset_packed & 0xFFFFu, fcb.scene_tile8_offset_packed >> 16u) * 8u;

    // ========================================================================
    // Mode 0: Screen-Space ContactShadow CS (Sony Bend Studio Subpixel Edge-Aware + 3-Zone Estimator)
    // ========================================================================
    if (mode == 0u) {
        uint2 px = dtid.xy + tile8_px_off;
        if (px.x >= fcb.render_extent.x || px.y >= fcb.render_extent.y)
            return;

        float2 pxc = float2(px) + 0.5f;
        bool in_vp = (pxc.x >= fcb.scene_viewport.x && pxc.x < fcb.scene_viewport.x + fcb.scene_viewport.z &&
                      pxc.y >= fcb.scene_viewport.y && pxc.y < fcb.scene_viewport.y + fcb.scene_viewport.w);
        Texture2D<float> depth_tex = ResourceDescriptorHeap[atelier_safe_desc(depth_srv)];
        float dz = depth_tex.Load(int3(px, 0));

        RWTexture2D<float> contact_uav = ResourceDescriptorHeap[atelier_safe_desc(out_uav)];
        uint sun_idx = fcb.primary_sun_and_gi_cands & 0xFFFFu;
        if (!in_vp || dz <= 0.0f || fcb.contact_shadow_length <= 0.0f || sun_idx == 0xFFFFu || sun_idx >= fcb.light_count) {
            contact_uav[px] = 1.0f;
            return;
        }
        if (slab0_srv != 0xFFFFFFFFu) {
            Texture2D<uint2> slab0_tex = ResourceDescriptorHeap[atelier_safe_desc(slab0_srv)];
            if (((slab0_tex.Load(int3(px, 0)).y >> 29u) & 1u) == 0u) {
                contact_uav[px] = 1.0f;
                return;
            }
        }

        StructuredBuffer<GpuLightRecord> lights = ResourceDescriptorHeap[atelier_safe_desc(fcb.lights_srv)];
        float3 L = safe_unit_normal(-lights[sun_idx].direction);
        Texture2D<uint> geom_n_tex = ResourceDescriptorHeap[atelier_safe_desc(geom_n_srv)];
        float3 gn = unpack_oct_snorm16(geom_n_tex.Load(int3(px, 0)));
        float ndl_gn = dot(gn, L);
        if (ndl_gn <= 0.03f) {
            contact_uav[px] = 1.0f;
            return;
        }

        float3 wpos = reconstruct_world_pos_from_depth(px, dz, fcb);
        float vz0 = exact_view_z_from_depth(px, dz, fcb);
        float px_span = 2.0f * abs(vz0 * fcb.projection[2][3] + fcb.projection[3][3]) /
                        max(abs(fcb.projection[1][1]) * fcb.scene_viewport.w, 1.0f);
        float3 start_wpos = wpos + gn * max(0.002f, px_span * 1.15f);
        float3 end_wpos = start_wpos + L * fcb.contact_shadow_length;
        float4 c0 = mul(float4(start_wpos, 1.0f), fcb.view_projection);
        float4 c1 = mul(float4(end_wpos, 1.0f), fcb.view_projection);
        if (c0.w <= 1e-4f || c1.w <= 1e-4f) {
            contact_uav[px] = 1.0f;
            return;
        }

        float2 p0_px = float2(fcb.scene_viewport.x + (c0.x / c0.w * 0.5f + 0.5f) * fcb.scene_viewport.z,
                              fcb.scene_viewport.y + (0.5f - c0.y / c0.w * 0.5f) * fcb.scene_viewport.w);
        float2 p1_px = float2(fcb.scene_viewport.x + (c1.x / c1.w * 0.5f + 0.5f) * fcb.scene_viewport.z,
                              fcb.scene_viewport.y + (0.5f - c1.y / c1.w * 0.5f) * fcb.scene_viewport.w);
        float z0 = c0.z / c0.w;
        float z1 = c1.z / c1.w;

        float2 ray_delta = p1_px - p0_px;
        if (dot(ray_delta, ray_delta) < 0.25f) {
            contact_uav[px] = 1.0f;
            return;
        }
        bool x_axis_major = abs(ray_delta.x) >= abs(ray_delta.y);

        float3 gn_v = safe_unit_normal(mul((float3x3)fcb.inv_view, gn));
        float slope_dz = 0.65f * px_span * length(gn_v.xy) / max(abs(gn_v.z), 0.12f);
        float base_thick = clamp(max(abs(vz0) * 0.008f, fcb.contact_shadow_length * 0.22f), 0.008f, 0.22f);
        float edge_thresh = max(0.015f, abs(vz0) * 0.018f);
        float min_bias = max(0.0025f, px_span * 0.85f) + min(slope_dz, base_thick * 0.35f);
        float graze_fade = saturate((ndl_gn - 0.03f) * 6.0f);

        // Deterministic IGN sub-step phase (temporally jittered ONLY when TAA is active so Off/SMAA stay bit-exact!):
        uint cs_t = ((fcb.flags & (1u << 11u)) != 0u) ? (fcb.frame_index & 63u) : 0u;
        float cs_ign = frac(52.9829189f * frac(dot(float2(px) + float(cs_t) * 5.37f, float2(0.06711056f, 0.00583715f))));

        // Bend Studio 3-zone accumulator (`bend_sss_gpu.h`: Hard Contact + 4-Phase Soft Buckets + Smooth Tail Fade):
        const uint k_steps = 24u;
        const uint k_hard_steps = 4u;
        const uint k_fade_steps = 6u;
        const float k_contrast = 3.0f;

        float hard_shadow = 1.0f;
        float4 shadow_val = 1.0f;
        int2 vp_min = fcb.scissor_rect.xy;
        int2 vp_max = max(vp_min, fcb.scissor_rect.zw - 1);

        [loop] for (uint s = 0u; s < k_steps; ++s) {
            float t = (float(s) + 0.15f + 0.75f * cs_ign) / float(k_steps);
            float2 sp = lerp(p0_px, p1_px, t);
            if (sp.x < fcb.scene_viewport.x || sp.x >= fcb.scene_viewport.x + fcb.scene_viewport.z ||
                sp.y < fcb.scene_viewport.y || sp.y >= fcb.scene_viewport.y + fcb.scene_viewport.w)
                break;

            int2 ipx0 = int2(floor(sp));
            if (all(ipx0 == int2(px)))
                continue;

            // Bend Studio minor-axis edge-aware subpixel interpolation (`bend_sss_gpu.h:273-330` in linear ViewZ):
            float minor_coord = x_axis_major ? sp.y : sp.x;
            float bilinear = frac(minor_coord) - 0.5f;
            int step_sign = (bilinear > 0.0f) ? 1 : -1;
            int2 ipx1 = clamp(ipx0 + (x_axis_major ? int2(0, step_sign) : int2(step_sign, 0)), vp_min, vp_max);

            float sd0 = depth_tex.Load(int3(ipx0, 0));
            if (sd0 <= 0.0f)
                continue;
            float svz0 = exact_view_z_from_depth(uint2(ipx0), sd0, fcb);
            float svz = svz0;
            float sd1 = depth_tex.Load(int3(ipx1, 0));
            if (sd1 > 0.0f) {
                float svz1 = exact_view_z_from_depth(uint2(ipx1), sd1, fcb);
                if (abs(svz0 - svz1) <= edge_thresh)
                    svz = lerp(svz0, svz1, abs(bilinear));
            }

            float ray_vz = exact_view_z_from_depth(uint2(ipx0), lerp(z0, z1, t), fcb);
            float diff = ray_vz - svz;
            float thick_t = base_thick * lerp(0.55f, 1.15f, t);
            if (diff > min_bias && diff < thick_t) {
                // Normalized slab distance inside `[min_bias, thick_t]`: `0` = deep inside slab, `1` = back-edge exit:
                float half_w = max(0.5f * (thick_t - min_bias), 1e-4f);
                float mid_d = 0.5f * (min_bias + thick_t);
                float slab_d = saturate(max(0.0f, abs(diff - mid_d) / half_w - 0.35f) / 0.65f);
                if (s < k_hard_steps) {
                    hard_shadow = min(hard_shadow, slab_d);
                } else {
                    float fade = (s >= k_steps - k_fade_steps)
                                     ? (float(s + 1u - (k_steps - k_fade_steps)) / float(k_fade_steps + 1u) * 0.80f)
                                     : 0.0f;
                    uint bucket = s & 3u;
                    float cand = slab_d + fade;
                    if (bucket == 0u) shadow_val.x = min(shadow_val.x, cand);
                    else if (bucket == 1u) shadow_val.y = min(shadow_val.y, cand);
                    else if (bucket == 2u) shadow_val.z = min(shadow_val.z, cand);
                    else shadow_val.w = min(shadow_val.w, cand);
                }
            }
        }

        shadow_val = saturate(shadow_val * k_contrast + (1.0f - k_contrast));
        hard_shadow = saturate(hard_shadow * k_contrast + (1.0f - k_contrast));
        float raw_vis = min(hard_shadow, dot(shadow_val, 0.25f.xxxx));
        contact_uav[px] = lerp(1.0f, raw_vis, graze_fade);
        return;
    }

    // ========================================================================
    // Mode 1: VSM Classifier + 64-Bit LDS Order-Preserving PenumbraQueue Compaction
    //         (100% uniform group barriers — zero early return!)
    // ========================================================================
    if (mode == 1u) {
        if (gindex < 2u)
            gs_pen_mask[gindex] = 0u;
        GroupMemoryBarrierWithGroupSync();

        uint2 px = dtid.xy + tile8_px_off;
        bool in_bounds = (px.x < fcb.render_extent.x && px.y < fcb.render_extent.y);
        bool needs_penumbra = false;
        GpuPenumbraItem pen_item = (GpuPenumbraItem)0;

        if (in_bounds) {
            RWTexture2D<uint2> mask_uav = ResourceDescriptorHeap[atelier_safe_desc(out_uav)];
            RWTexture2D<float2> pen_uav = ResourceDescriptorHeap[atelier_safe_desc(pen_tex_idx)];

            float2 pxc = float2(px) + 0.5f;
            bool in_vp = (pxc.x >= fcb.scene_viewport.x && pxc.x < fcb.scene_viewport.x + fcb.scene_viewport.z &&
                          pxc.y >= fcb.scene_viewport.y && pxc.y < fcb.scene_viewport.y + fcb.scene_viewport.w);
            Texture2D<float> depth_tex = ResourceDescriptorHeap[atelier_safe_desc(depth_srv)];
            float dz = depth_tex.Load(int3(px, 0));

            uint2 vis_bits = uint2(0xFFFFFFFFu, 0xFFFFFFFFu);
            uint2 todo_bits = uint2(0u, 0u);
            float2 soft_pen = float2(1.0f, 1.0f);
            uint area_slot = 0xFFu;
            bool is_foliage = false;

            bool valid_px = in_vp && (dz > 0.0f) && ((fcb.flags & 1u) != 0u) && (fcb.light_count > 0u) &&
                            (fcb.lights_srv != 0xFFFFFFFFu);
            if (valid_px && slab0_srv != 0xFFFFFFFFu) {
                Texture2D<uint2> slab0_tex = ResourceDescriptorHeap[atelier_safe_desc(slab0_srv)];
                uint2 s0 = slab0_tex.Load(int3(px, 0));
                if (((s0.y >> 29u) & 1u) == 0u)
                    valid_px = false;
                else
                    is_foliage = (((s0.y >> 26u) & 7u) == SLAB_PROFILE_FOLIAGE);
            }

            if (valid_px) {
                Texture2D<uint> geom_n_tex = ResourceDescriptorHeap[atelier_safe_desc(geom_n_srv)];
                float3 gn = unpack_oct_snorm16(geom_n_tex.Load(int3(px, 0)));
                float3 wpos = reconstruct_world_pos_from_depth(px, dz, fcb);
                float cam_dist = length(wpos - fcb.camera_eye);
                StructuredBuffer<GpuLightRecord> lights = ResourceDescriptorHeap[atelier_safe_desc(fcb.lights_srv)];

                uint sun_idx = fcb.primary_sun_and_gi_cands & 0xFFFFu;
                uint d_bits = min(fcb.dir_light_count, 4u);
                uint local_bit_cap = 64u - d_bits;

                [loop] for (uint di = 0u; di < d_bits; ++di) {
                    GpuLightRecord l = lights[di];
                    if ((l.kind_and_flags & 8u) == 0u)
                        continue;
                    float far_pcf = 1.0f;
                    int vres = evaluate_vsm_light_interval(l, wpos, gn, is_foliage, cam_dist, fcb, far_pcf);
                    if (vres == 0) {
                        vis_bits.y &= ~(1u << (31u - di));
                        if (di == sun_idx)
                            soft_pen.x = 0.0f;
                    } else if (vres == 2) {
                        if (di == sun_idx) {
                            soft_pen.x = far_pcf;
                            if (far_pcf <= 0.001f)
                                vis_bits.y &= ~(1u << (31u - di));
                        } else if (far_pcf < 0.5f) {
                            vis_bits.y &= ~(1u << (31u - di));
                        }
                    } else if (vres < 0) {
                        todo_bits.y |= (1u << (31u - di));
                    }
                }

                if (cluster_srv != 0xFFFFFFFFu && fcb.light_count > fcb.dir_light_count) {
                    StructuredBuffer<uint> cluster_buf = ResourceDescriptorHeap[atelier_safe_desc(cluster_srv)];
                    uint tiles_x = (fcb.render_extent.x + 15u) >> 4u;
                    uint tile_id = (px.y >> 4u) * tiles_x + (px.x >> 4u);
                    uint c_base = tile_id * 132u;
                    uint c_count = min(cluster_buf[c_base + 0u] & 0x7FFFFFFFu, local_bit_cap);
                    area_slot = select_receiver_primary_area_slot(c_base, c_count, local_bit_cap, wpos, gn, is_foliage,
                                                                  cluster_buf, lights);
                    uint px_z_bit = 1u << depth_to_z_bin(exact_view_z_from_depth(px, dz, fcb));

                    [loop] for (uint it = 0u; it < c_count; ++it) {
                        if ((cluster_buf[c_base + 66u + it] & px_z_bit) == 0u)
                            continue;
                        uint li = cluster_buf[c_base + 2u + it];
                        GpuLightRecord l = lights[li];
                        if ((l.kind_and_flags & 8u) == 0u)
                            continue;
                        float far_pcf = 1.0f;
                        int vres = evaluate_vsm_light_interval(l, wpos, gn, is_foliage, cam_dist, fcb, far_pcf);
                        if (vres == 0) {
                            if (it < 32u)
                                vis_bits.x &= ~(1u << it);
                            else
                                vis_bits.y &= ~(1u << (it - 32u));
                            if (it == area_slot)
                                soft_pen.y = 0.0f;
                        } else if (vres == 2) {
                            if (it == area_slot) {
                                soft_pen.y = far_pcf;
                                if (far_pcf <= 0.001f) {
                                    if (it < 32u)
                                        vis_bits.x &= ~(1u << it);
                                    else
                                        vis_bits.y &= ~(1u << (it - 32u));
                                }
                            } else if (far_pcf < 0.5f) {
                                if (it < 32u)
                                    vis_bits.x &= ~(1u << it);
                                else
                                    vis_bits.y &= ~(1u << (it - 32u));
                            }
                        } else if (vres < 0) {
                            if (it < 32u)
                                todo_bits.x |= (1u << it);
                            else
                                todo_bits.y |= (1u << (it - 32u));
                        }
                    }
                }
            }

            mask_uav[px] = vis_bits;
            pen_uav[px] = soft_pen;

            needs_penumbra = any(todo_bits != 0u);
            if (needs_penumbra) {
                uint q_sun = uint(round(saturate(soft_pen.x) * 255.0f)) & 0xFFu;
                uint q_area = uint(round(saturate(soft_pen.y) * 255.0f)) & 0xFFu;
                pen_item.px_packed = (px.x & 0xFFFFu) | ((px.y & 0xFFFFu) << 16u);
                pen_item.todo_lo = todo_bits.x;
                pen_item.todo_hi = todo_bits.y;
                pen_item.init_vis_lo = vis_bits.x;
                pen_item.init_vis_hi = vis_bits.y;
                pen_item.meta = (area_slot & 0xFFu) | (q_sun << 8u) | (q_area << 16u) | (is_foliage ? (1u << 24u) : 0u);
            }
        }

        if (needs_penumbra)
            InterlockedOr(gs_pen_mask[gindex >> 5u], 1u << (gindex & 31u));
        GroupMemoryBarrierWithGroupSync();

        uint total_pen = countbits(gs_pen_mask[0]) + countbits(gs_pen_mask[1]);
        if (gindex == 0u && total_pen > 0u) {
            RWByteAddressBuffer pen_args = ResourceDescriptorHeap[atelier_safe_desc(pen_args_idx)];
            pen_args.InterlockedAdd(12u, total_pen, gs_pen_base);
        }
        GroupMemoryBarrierWithGroupSync();

        if (needs_penumbra) {
            uint word_idx = gindex >> 5u;
            uint local_bit = gindex & 31u;
            uint lower_mask = (local_bit == 0u) ? 0u : ((1u << local_bit) - 1u);
            uint prefix = (word_idx == 0u) ? countbits(gs_pen_mask[0] & lower_mask)
                                           : (countbits(gs_pen_mask[0]) + countbits(gs_pen_mask[1] & lower_mask));
            uint dst_slot = gs_pen_base + prefix;
            if (dst_slot < max_pen_items) {
                RWStructuredBuffer<GpuPenumbraItem> pen_q = ResourceDescriptorHeap[atelier_safe_desc(pen_queue_idx)];
                pen_q[dst_slot] = pen_item;
            }
        }
        return;
    }

    // ========================================================================
    // Mode 6: Temporal Penumbra Reprojection & Variance-Guided Accumulation CS (`RGBA16_SFLOAT`)
    // ========================================================================
    if (mode == 6u) {
        uint2 px = dtid.xy + tile8_px_off;
        if (px.x >= fcb.render_extent.x || px.y >= fcb.render_extent.y)
            return;

        Texture2D<float2> pen_raw = ResourceDescriptorHeap[atelier_safe_desc(pen_tex_idx)];
        RWTexture2D<float4> pen_hist_out = ResourceDescriptorHeap[atelier_safe_desc(out_uav)];
        float2 raw_c = pen_raw.Load(int3(px, 0));

        float2 pxc = float2(px) + 0.5f;
        bool in_vp = (pxc.x >= fcb.scene_viewport.x && pxc.x < fcb.scene_viewport.x + fcb.scene_viewport.z &&
                      pxc.y >= fcb.scene_viewport.y && pxc.y < fcb.scene_viewport.y + fcb.scene_viewport.w);
        Texture2D<float> depth_tex = ResourceDescriptorHeap[atelier_safe_desc(depth_srv)];
        float dz = depth_tex.Load(int3(px, 0));
        if (!in_vp || dz <= 0.0f) {
            pen_hist_out[px] = float4(raw_c, 1.0f, 0.0f);
            return;
        }

        Texture2D<uint> geom_n_tex = ResourceDescriptorHeap[atelier_safe_desc(geom_n_srv)];
        float3 gn_c = unpack_oct_snorm16(geom_n_tex.Load(int3(px, 0)));
        float3 P_c = reconstruct_world_pos_from_depth(px, dz, fcb);
        float px_ws = gi_pixel_world_size(P_c, fcb);

        uint sun_idx = fcb.primary_sun_and_gi_cands & 0xFFFFu;
        bool has_sun = (fcb.lights_srv != 0xFFFFFFFFu) && (sun_idx < fcb.dir_light_count);
        float3 L_sun = float3(0.0f, 1.0f, 0.0f);
        float sun_back_thresh = -0.1f;
        if (has_sun) {
            StructuredBuffer<GpuLightRecord> lights = ResourceDescriptorHeap[atelier_safe_desc(fcb.lights_srv)];
            GpuLightRecord sun_l = lights[sun_idx];
            L_sun = safe_unit_normal(-sun_l.direction);
            float sun_alpha = (sun_l.source_radius > 1e-4f) ? min(sun_l.source_radius, 0.45f) : 0.0f;
            sun_back_thresh = -0.1f - sin(sun_alpha);
        }
        bool c_sun_front = !has_sun || (dot(gn_c, L_sun) > sun_back_thresh);

        int2 sc_min = fcb.scissor_rect.xy;
        int2 sc_max = max(sc_min, fcb.scissor_rect.zw - 1);
        float2 raw_min = raw_c;
        float2 raw_max = raw_c;
        float2 raw_mean = raw_c;
        float2 n_cnt = float2(1.0f, 1.0f);
        [unroll] for (int dy = -1; dy <= 1; ++dy) {
            [unroll] for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0)
                    continue;
                int2 q = clamp(int2(px) + int2(dx, dy), sc_min, sc_max);
                float qd = depth_tex.Load(int3(q, 0));
                if (qd <= 0.0f)
                    continue;
                float3 qgn = unpack_oct_snorm16(geom_n_tex.Load(int3(q, 0)));
                float3 Q = reconstruct_world_pos_from_depth(uint2(q), qd, fcb);
                if (dot(gn_c, qgn) >= 0.85f && abs(dot(Q - P_c, gn_c)) <= max(0.02f, px_ws * 3.0f)) {
                    float2 rv = pen_raw.Load(int3(q, 0));
                    bool q_sun_front = !has_sun || (dot(qgn, L_sun) > sun_back_thresh);
                    if (c_sun_front == q_sun_front) {
                        raw_min.x = min(raw_min.x, rv.x);
                        raw_max.x = max(raw_max.x, rv.x);
                        raw_mean.x += rv.x;
                        n_cnt.x += 1.0f;
                    }
                    raw_min.y = min(raw_min.y, rv.y);
                    raw_max.y = max(raw_max.y, rv.y);
                    raw_mean.y += rv.y;
                    n_cnt.y += 1.0f;
                }
            }
        }
        raw_mean /= n_cnt;
        float local_var = max(raw_max.x - raw_min.x, raw_max.y - raw_min.y);

        float2 out_vis = raw_c;
        float hist_cnt = 1.0f;
        if (pen_queue_idx != 0xFFFFFFFFu && slab0_srv != 0xFFFFFFFFu && cluster_srv != 0xFFFFFFFFu &&
            fcb.prev_view_z_srv != 0xFFFFFFFFu) {
            Texture2D<float2> mot_tex = ResourceDescriptorHeap[atelier_safe_desc(cluster_srv)];
            float2 prev_px_f = (pxc - fcb.jitter_pixels) + mot_tex.Load(int3(px, 0)) * float2(fcb.render_extent);
            if (prev_px_f.x >= fcb.prev_scene_viewport.x && prev_px_f.x < fcb.prev_scene_viewport.x + fcb.prev_scene_viewport.z &&
                prev_px_f.y >= fcb.prev_scene_viewport.y && prev_px_f.y < fcb.prev_scene_viewport.y + fcb.prev_scene_viewport.w) {
                float4 cam_clip = mul(float4(P_c, 1.0f), fcb.prev_unjittered_vp);
                float2 cam_prev_px = prev_px_f;
                float cam_exp_dz = dz;
                if (abs(cam_clip.w) > 1e-5f) {
                    cam_prev_px = float2(fcb.prev_scene_viewport.x + (cam_clip.x / cam_clip.w * 0.5f + 0.5f) * fcb.prev_scene_viewport.z,
                                         fcb.prev_scene_viewport.y + (0.5f - cam_clip.y / cam_clip.w * 0.5f) * fcb.prev_scene_viewport.w);
                    cam_exp_dz = cam_clip.z / cam_clip.w;
                }
                bool is_dyn = length(cam_prev_px - prev_px_f) > 1.5f;
                float grazing = 1.0f / max(abs(dot(gn_c, safe_unit_normal(fcb.camera_eye - P_c))), 0.08f);

                Texture2D<float> prev_d_tex = ResourceDescriptorHeap[atelier_safe_desc(fcb.prev_view_z_srv)];
                Texture2D<uint> prev_gn_tex = ResourceDescriptorHeap[atelier_safe_desc(slab0_srv)];
                Texture2D<float4> prev_pen_tex = ResourceDescriptorHeap[atelier_safe_desc(pen_queue_idx)];

                int2 p_base = int2(floor(prev_px_f - 0.5f));
                float2 p_frac = saturate((prev_px_f - 0.5f) - float2(p_base));
                float2 p_vis_sum = 0.0f;
                float p_cnt_sum = 0.0f;
                float p_w_sum = 0.0f;

                [unroll] for (int py = 0; py <= 1; ++py) {
                    [unroll] for (int px_o = 0; px_o <= 1; ++px_o) {
                        int2 qp = p_base + int2(px_o, py);
                        if (qp.x < 0 || qp.y < 0 || qp.x >= int(fcb.render_extent.x) || qp.y >= int(fcb.render_extent.y))
                            continue;
                        float pdz = prev_d_tex.Load(int3(qp, 0));
                        if (pdz <= 0.0f)
                            continue;
                        float3 pgn = unpack_oct_snorm16(prev_gn_tex.Load(int3(qp, 0)));
                        if (dot(pgn, gn_c) < 0.80f)
                            continue;
                        if (has_sun && (c_sun_front != (dot(pgn, L_sun) > sun_back_thresh)))
                            continue;
                        if (abs(pdz - cam_exp_dz) > max(1e-4f, max(pdz, cam_exp_dz) * 0.03f * grazing))
                            continue;
                        float4 hp = prev_pen_tex.Load(int3(qp, 0));
                        if (hp.z <= 0.0f)
                            continue;
                        float bw = (px_o == 0 ? (1.0f - p_frac.x) : p_frac.x) * (py == 0 ? (1.0f - p_frac.y) : p_frac.y);
                        float w = max(bw, 1e-3f) * pow(saturate(dot(pgn, gn_c)), 6.0f);
                        p_vis_sum += hp.xy * w;
                        p_cnt_sum += hp.z * w;
                        p_w_sum += w;
                    }
                }

                if (p_w_sum > 1e-4f) {
                    float2 prev_vis = p_vis_sum / p_w_sum;
                    float umbra_keep = is_dyn ? 0.0f : 0.72f;
                    float lo_x = (raw_max.x <= 1e-4f) ? 0.0f : ((raw_min.x >= 0.9999f) ? lerp(1.0f, prev_vis.x, umbra_keep) : max(0.0f, raw_min.x - 0.35f));
                    float lo_y = (raw_max.y <= 1e-4f) ? 0.0f : ((raw_min.y >= 0.9999f) ? lerp(1.0f, prev_vis.y, umbra_keep) : max(0.0f, raw_min.y - 0.35f));
                    float hi_x = (raw_max.x <= 1e-4f) ? (prev_vis.x * umbra_keep) : ((raw_min.x >= 0.9999f) ? 1.0f : min(1.0f, raw_max.x + 0.35f));
                    float hi_y = (raw_max.y <= 1e-4f) ? (prev_vis.y * umbra_keep) : ((raw_min.y >= 0.9999f) ? 1.0f : min(1.0f, raw_max.y + 0.35f));
                    prev_vis = clamp(prev_vis, float2(lo_x, lo_y), float2(hi_x, hi_y));

                    float max_frames = is_dyn ? 3.0f : float(max(max_pen_items, 2u));
                    hist_cnt = min(p_cnt_sum / p_w_sum + 1.0f, max_frames);
                    float alpha = 1.0f / max(hist_cnt, 1.0f);
                    out_vis = lerp(prev_vis, raw_c, alpha);
                    local_var = max(local_var, max(abs(raw_mean.x - prev_vis.x), abs(raw_mean.y - prev_vis.y)));
                }
            }
        }

        float pen_activity = max(local_var, max(out_vis.x * (1.0f - out_vis.x), out_vis.y * (1.0f - out_vis.y)) * 4.0f);
        pen_hist_out[px] = float4(saturate(out_vis), hist_cnt, saturate(pen_activity));
        return;
    }

    // ========================================================================
    // Mode 4: 3D Tangent-Plane Edge-Stopping À-Trous Wavelet Bilateral Penumbra Filter CS (`step = 1, 2, 4`)
    // ========================================================================
    if (mode == 4u) {
        uint2 px = dtid.xy + tile8_px_off;
        if (px.x >= fcb.render_extent.x || px.y >= fcb.render_extent.y)
            return;

        Texture2D<float4> pen_src = ResourceDescriptorHeap[atelier_safe_desc(pen_tex_idx)];
        RWTexture2D<float4> pen_dst = ResourceDescriptorHeap[atelier_safe_desc(out_uav)];
        float4 center = pen_src.Load(int3(px, 0));

        float2 pxc = float2(px) + 0.5f;
        bool in_vp = (pxc.x >= fcb.scene_viewport.x && pxc.x < fcb.scene_viewport.x + fcb.scene_viewport.z &&
                      pxc.y >= fcb.scene_viewport.y && pxc.y < fcb.scene_viewport.y + fcb.scene_viewport.w);
        Texture2D<float> depth_tex = ResourceDescriptorHeap[atelier_safe_desc(depth_srv)];
        float dz = depth_tex.Load(int3(px, 0));
        if (!in_vp || dz <= 0.0f || fcb.light_count == 0u || fcb.lights_srv == 0xFFFFFFFFu) {
            pen_dst[px] = center;
            return;
        }
        int step_px = int(clamp(max_pen_items, 1u, 8u));
        int2 sc_min = fcb.scissor_rect.xy;
        int2 sc_max = max(sc_min, fcb.scissor_rect.zw - 1);
        if (center.w <= 1e-4f) {
            float nbr_act = max(max(pen_src.Load(int3(clamp(int2(px) + int2(-step_px, 0), sc_min, sc_max), 0)).w,
                                    pen_src.Load(int3(clamp(int2(px) + int2(step_px, 0), sc_min, sc_max), 0)).w),
                                max(pen_src.Load(int3(clamp(int2(px) + int2(0, -step_px), sc_min, sc_max), 0)).w,
                                    pen_src.Load(int3(clamp(int2(px) + int2(0, step_px), sc_min, sc_max), 0)).w));
            if (nbr_act <= 1e-4f) {
                pen_dst[px] = center;
                return;
            }
        }

        StructuredBuffer<GpuLightRecord> lights = ResourceDescriptorHeap[atelier_safe_desc(fcb.lights_srv)];
        uint sun_idx = fcb.primary_sun_and_gi_cands & 0xFFFFu;
        bool filter_sun = (sun_idx < fcb.dir_light_count) && (lights[sun_idx].source_radius > 1e-4f);
        GpuLightRecord sun_l = (GpuLightRecord)0;
        if (filter_sun)
            sun_l = lights[sun_idx];

        Texture2D<uint> geom_n_tex = ResourceDescriptorHeap[atelier_safe_desc(geom_n_srv)];
        float3 gn_c = unpack_oct_snorm16(geom_n_tex.Load(int3(px, 0)));
        float3 P_c = reconstruct_world_pos_from_depth(px, dz, fcb);
        bool c_foliage = false;
        if (slab0_srv != 0xFFFFFFFFu) {
            Texture2D<uint2> slab0_tex = ResourceDescriptorHeap[atelier_safe_desc(slab0_srv)];
            c_foliage = (((slab0_tex.Load(int3(px, 0)).y >> 26u) & 7u) == SLAB_PROFILE_FOLIAGE);
        }

        uint d_bits = min(fcb.dir_light_count, 4u);
        uint local_bit_cap = 64u - d_bits;
        uint tiles_x = (fcb.render_extent.x + 15u) >> 4u;
        uint px_area_li = 0xFFFFFFFFu;
        StructuredBuffer<uint> cluster_buf = ResourceDescriptorHeap[atelier_safe_desc(cluster_srv)];
        if (cluster_srv != 0xFFFFFFFFu && fcb.light_count > fcb.dir_light_count) {
            uint tile_id = (px.y >> 4u) * tiles_x + (px.x >> 4u);
            uint c_base = tile_id * 132u;
            uint c_count = min(cluster_buf[c_base + 0u] & 0x7FFFFFFFu, local_bit_cap);
            uint aslot = select_receiver_primary_area_slot(c_base, c_count, local_bit_cap, P_c, gn_c, c_foliage,
                                                           cluster_buf, lights);
            if (aslot != 0xFFu)
                px_area_li = cluster_buf[c_base + 2u + aslot];
        }
        bool filter_area = (px_area_li != 0xFFFFFFFFu);
        if (!filter_sun && !filter_area) {
            pen_dst[px] = center;
            return;
        }
        GpuLightRecord area_l = (GpuLightRecord)0;
        if (filter_area)
            area_l = lights[px_area_li];

        // Skip wide step=4 blur only inside settled pure-lit or pure-umbra regions (`center.w < 0.04`):
        if (step_px >= 4 && center.z >= 10.0f && center.w < 0.04f) {
            pen_dst[px] = center;
            return;
        }

        bool c_sun_front = !filter_sun || is_light_front_horizon(sun_l, P_c, gn_c);
        bool c_area_front = !filter_area || is_light_front_horizon(area_l, P_c, gn_c);
        float px_ws = gi_pixel_world_size(P_c, fcb);
        float plane_tol = max(0.012f, px_ws * float(step_px) * 2.2f);
        float radial_tol = max(0.08f, px_ws * float(step_px) * 4.5f);

        float sum_s = center.x;
        float w_s = 1.0f;
        float sum_a = center.y;
        float w_a = 1.0f;
        float max_act = center.w;

        [unroll] for (int dy = -1; dy <= 1; ++dy) {
            [unroll] for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0)
                    continue;
                int2 q = clamp(int2(px) + int2(dx, dy) * step_px, sc_min, sc_max);
                float qd = depth_tex.Load(int3(q, 0));
                if (qd <= 0.0f)
                    continue;
                float3 qgn = unpack_oct_snorm16(geom_n_tex.Load(int3(q, 0)));
                float ndot = dot(gn_c, qgn);
                if (ndot < 0.75f)
                    continue;
                float3 Q = reconstruct_world_pos_from_depth(uint2(q), qd, fcb);
                float plane_d = max(abs(dot(Q - P_c, gn_c)), abs(dot(P_c - Q, qgn)));
                float rad_d = length(Q - P_c);
                if (rad_d > radial_tol * 2.5f)
                    continue;

                float k_spatial = (dx == 0 || dy == 0) ? 0.5f : 0.25f;
                float kw = k_spatial * exp(-plane_d / plane_tol - 0.25f * rad_d / radial_tol) * pow(saturate(ndot), 12.0f);
                if (kw <= 1e-4f)
                    continue;
                float4 qv = pen_src.Load(int3(q, 0));
                max_act = max(max_act, qv.w * 0.85f);
                if (filter_sun && (c_sun_front == is_light_front_horizon(sun_l, Q, qgn))) {
                    sum_s += qv.x * kw;
                    w_s += kw;
                }
                if (filter_area && (c_area_front == is_light_front_horizon(area_l, Q, qgn))) {
                    bool q_foliage = false;
                    if (slab0_srv != 0xFFFFFFFFu) {
                        Texture2D<uint2> slab0_tex = ResourceDescriptorHeap[atelier_safe_desc(slab0_srv)];
                        q_foliage = (((slab0_tex.Load(int3(q, 0)).y >> 26u) & 7u) == SLAB_PROFILE_FOLIAGE);
                    }
                    uint qt_id = (uint(q.y) >> 4u) * tiles_x + (uint(q.x) >> 4u);
                    uint qb = qt_id * 132u;
                    uint qc = min(cluster_buf[qb + 0u] & 0x7FFFFFFFu, local_bit_cap);
                    uint qslot = select_receiver_primary_area_slot(qb, qc, local_bit_cap, Q, qgn, q_foliage,
                                                                   cluster_buf, lights);
                    bool same_area = (qslot != 0xFFu) && (cluster_buf[qb + 2u + qslot] == px_area_li);
                    if (same_area) {
                        sum_a += qv.y * kw;
                        w_a += kw;
                    }
                }
            }
        }

        float out_s = filter_sun ? (sum_s / w_s) : center.x;
        float out_a = filter_area ? (sum_a / w_a) : center.y;
        pen_dst[px] = float4(out_s, out_a, center.z, max_act);
        return;
    }
#endif

    // ========================================================================
    // Mode 3: Compact PenumbraQueue DXR 1.1 Inline RayQuery CS (64 threads/group)
    // ========================================================================
    if (gindex == 0u)
        gs_pen_ray_sum = 0u;
    GroupMemoryBarrierWithGroupSync();

    ByteAddressBuffer pen_args_ro = ResourceDescriptorHeap[atelier_safe_desc(pen_args_idx)];
    uint pen_count = min(pen_args_ro.Load(12u), max_pen_items);
    uint item_idx = (gid.y * 1024u + gid.x) * 64u + gindex;
    uint traced_rays = 0u;

    if (item_idx < pen_count) {
        StructuredBuffer<GpuPenumbraItem> pen_q = ResourceDescriptorHeap[atelier_safe_desc(pen_queue_idx)];
        GpuPenumbraItem item = pen_q[item_idx];
        uint2 px = uint2(item.px_packed & 0xFFFFu, item.px_packed >> 16u);
        uint2 todo_bits = uint2(item.todo_lo, item.todo_hi);
        uint2 vis_bits = uint2(item.init_vis_lo, item.init_vis_hi);
        uint area_slot = item.meta & 0xFFu;
        float2 soft_pen = float2(float((item.meta >> 8u) & 0xFFu) * (1.0f / 255.0f),
                                 float((item.meta >> 16u) & 0xFFu) * (1.0f / 255.0f));
        bool is_foliage = ((item.meta >> 24u) & 1u) != 0u;

        Texture2D<float> depth_tex = ResourceDescriptorHeap[atelier_safe_desc(depth_srv)];
        Texture2D<uint> geom_n_tex = ResourceDescriptorHeap[atelier_safe_desc(geom_n_srv)];
        float dz = depth_tex.Load(int3(px, 0));
        float3 gn = unpack_oct_snorm16(geom_n_tex.Load(int3(px, 0)));
        float3 wpos = reconstruct_world_pos_from_depth(px, dz, fcb);
        StructuredBuffer<GpuLightRecord> lights = ResourceDescriptorHeap[atelier_safe_desc(fcb.lights_srv)];

        bool temporal_aa = (fcb.flags & (1u << 11u)) != 0u;
        uint t_phase = temporal_aa ? (fcb.frame_index & 63u) : 0u;
        float2 bn0 = atelier_blue_jitter2(px, t_phase, 0x517CC1B7u);
        float2 bn1 = temporal_aa ? frac(bn0 + float2(0.5f, 0.6180339887f)) : frac(bn0 + float2(0.5f, 0.0f));
        float2 bn2 = frac(bn0 + float2(0.0f, 0.5f));
        float2 bn3 = frac(bn0 + float2(0.5f, 0.5f));

        uint sun_idx = fcb.primary_sun_and_gi_cands & 0xFFFFu;
        uint d_bits = min(fcb.dir_light_count, 4u);
        uint local_bit_cap = 64u - d_bits;

        [loop] for (uint di = 0u; di < d_bits; ++di) {
            uint bit_mask = 1u << (31u - di);
            if ((todo_bits.y & bit_mask) == 0u)
                continue;
            GpuLightRecord l = lights[di];
            if (di == sun_idx && l.source_radius > 1e-4f) {
                bool h0 = evaluate_light_shadow_ray(l, wpos, gn, is_foliage, true, bn0.x, bn0.y, fcb, traced_rays);
                float sv = float(h0);
                bool any_h = h0;
                if (!temporal_aa) {
                    bool h1 = evaluate_light_shadow_ray(l, wpos, gn, is_foliage, true, bn1.x, bn1.y, fcb, traced_rays);
                    bool h2 = evaluate_light_shadow_ray(l, wpos, gn, is_foliage, true, bn2.x, bn2.y, fcb, traced_rays);
                    bool h3 = evaluate_light_shadow_ray(l, wpos, gn, is_foliage, true, bn3.x, bn3.y, fcb, traced_rays);
                    sv = 0.25f * (float(h0) + float(h1) + float(h2) + float(h3));
                    any_h = any_h || h1 || h2 || h3;
                }
                soft_pen.x = sv;
                if (!any_h)
                    vis_bits.y &= ~bit_mask;
            } else {
                bool h = evaluate_light_shadow_ray(l, wpos, gn, is_foliage, temporal_aa, bn0.x, bn0.y, fcb, traced_rays);
                if (di == sun_idx)
                    soft_pen.x = h ? 1.0f : 0.0f;
                if (!h)
                    vis_bits.y &= ~bit_mask;
            }
        }

        if (cluster_srv != 0xFFFFFFFFu && fcb.light_count > fcb.dir_light_count) {
            StructuredBuffer<uint> cluster_buf = ResourceDescriptorHeap[atelier_safe_desc(cluster_srv)];
            uint tiles_x = (fcb.render_extent.x + 15u) >> 4u;
            uint tile_id = (px.y >> 4u) * tiles_x + (px.x >> 4u);
            uint c_base = tile_id * 132u;
            uint c_count = min(cluster_buf[c_base + 0u] & 0x7FFFFFFFu, local_bit_cap);

            [loop] for (uint it = 0u; it < c_count; ++it) {
                bool todo = (it < 32u) ? (((todo_bits.x >> it) & 1u) != 0u)
                                       : (((todo_bits.y >> (it - 32u)) & 1u) != 0u);
                if (!todo)
                    continue;
                uint li = cluster_buf[c_base + 2u + it];
                GpuLightRecord l = lights[li];
                uint kind = l.kind_and_flags & 3u;
                if (it == area_slot && (kind == 1u || l.source_radius > 1e-3f)) {
                    float sv = 0.0f;
                    bool any_h = false;
                    if (temporal_aa) {
                        bool h0 = evaluate_light_shadow_ray(l, wpos, gn, is_foliage, true, bn0.x, bn0.y, fcb, traced_rays);
                        bool h1 = evaluate_light_shadow_ray(l, wpos, gn, is_foliage, true, bn1.x, bn1.y, fcb, traced_rays);
                        sv = 0.5f * (float(h0) + float(h1));
                        any_h = h0 || h1;
                    } else {
                        uint hit_sum = 0u;
                        [unroll] for (uint s_i = 0u; s_i < 8u; ++s_i) {
                            float2 b_s = frac(bn0 + float2(float(s_i) * 0.125f, float(s_i) * 0.6180339887f));
                            bool hs = evaluate_light_shadow_ray(l, wpos, gn, is_foliage, true, b_s.x, b_s.y, fcb, traced_rays);
                            hit_sum += hs ? 1u : 0u;
                        }
                        sv = float(hit_sum) * 0.125f;
                        any_h = (hit_sum != 0u);
                    }
                    soft_pen.y = sv;
                    if (!any_h) {
                        if (it < 32u)
                            vis_bits.x &= ~(1u << it);
                        else
                            vis_bits.y &= ~(1u << (it - 32u));
                    }
                } else {
                    bool finite_src = (kind == 1u || l.source_radius > 1e-3f);
                    bool h = evaluate_light_shadow_ray(l, wpos, gn, is_foliage, temporal_aa || finite_src, bn0.x, bn0.y,
                                                       fcb, traced_rays);
                    if (!h) {
                        if (it < 32u)
                            vis_bits.x &= ~(1u << it);
                        else
                            vis_bits.y &= ~(1u << (it - 32u));
                    }
                }
            }
        }

        RWTexture2D<uint2> mask_uav = ResourceDescriptorHeap[atelier_safe_desc(out_uav)];
        RWTexture2D<float2> pen_uav = ResourceDescriptorHeap[atelier_safe_desc(pen_tex_idx)];
        mask_uav[px] = vis_bits;
        pen_uav[px] = soft_pen;
    }

    if (traced_rays > 0u)
        InterlockedAdd(gs_pen_ray_sum, traced_rays);
    GroupMemoryBarrierWithGroupSync();

    if (gindex == 0u && gs_pen_ray_sum > 0u && counter_uav != 0xFFFFFFFFu) {
        RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[atelier_safe_desc(counter_uav)];
        InterlockedAdd(counters[0u], gs_pen_ray_sum);
    }
}
