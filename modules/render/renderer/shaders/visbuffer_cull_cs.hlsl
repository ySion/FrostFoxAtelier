// Stateless Two-Phase HZB Parallel Chunk/Meshlet Culling + HZB Mip Pyramid Builder + Indirect Args Finalizer
// Modes:
//   0: ClearCounters (`cull_counters` & optional `lit_gpu_counters`)
//   1: ExpandInstancesToChunks (1 thread/instance -> G1 LOD selection -> emits 64-meshlet chunks)
//   5: FinalizeChunkIndirectArgs (clamps chunk count & writes 2D `CmdDispatchIndirect` grid)
//   6: CullMeshletChunksPhase1 (1 group = 1 chunk of up to 64 meshlets -> exact 6-plane ellipsoid frustum + backface cone + PrevHZB)
//   3: FinalizePhase1MeshArgs (clamps counters, saves Phase 1 base offsets, writes Phase 1 Mesh & Phase 2 CS indirect args)
//   4: BuildHZBMip (exact 2x2 / odd-border Reversed-Z min/max mip reduction)
//   2: CullPhase2Candidates (re-tests PrevHZB-rejected clusters against freshly built CurrHZB)
//   7: FinalizePhase2MeshArgs (clamps total counters, writes Phase 2 delta Mesh args & updates Wireframe/Selection slots)

cbuffer CullRootConstants : register(b0, space0) {
    uint g_frame_cb_srv;
    uint g_phase_mode;
    uint g_opaque_clusters_uav;
    uint g_masked_clusters_uav;
    uint g_selected_clusters_uav;
    uint g_chunks_uav;
    uint g_phase2_candidates_uav;
    uint g_counters_uav; // [0]: opaque, [1]: p2_cands, [2]: selected, [3]: p1_opaque, [4]: p1_sel, [5]: chunks, [6..6+M): masked, [6+M..6+2M): p1_masked
    uint g_indirect_args_uav;
    uint g_aux_indirect_uav;
    uint g_hzb_src_srv;
    uint g_hzb_dst_uav;
    uint g_hzb_src_w;
    uint g_hzb_src_h;
    uint g_hzb_dst_w;
    uint g_flags_and_dst_h; // Mode 4: bits 0..15 = dst_h, bit 16 = src_is_d32; Cull modes: bit 0 = has_prev_hzb, bit 1 = emit_selected, bit 2 = allow_cone_cull
};

uint3 make_indirect_grid_2d(uint groups) {
    return (groups <= 65535u) ? uint3(groups, 1u, 1u) : uint3(1024u, (groups + 1023u) >> 10u, 1u);
}

float compute_matrix_max_scale(row_major float4x4 m) {
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

float compute_instance_max_scale(GpuInstanceRecord inst) {
    return compute_matrix_max_scale(inst.world);
}

float affine_sphere_plane_support(float radius_l, float3 plane_n, GpuInstanceRecord inst) {
    float3 g = float3(dot(plane_n, inst.world[0].xyz),
                      dot(plane_n, inst.world[1].xyz),
                      dot(plane_n, inst.world[2].xyz));
    return radius_l * length(g);
}

bool test_homogeneous_plane(float signed_val, float3 plane_n, float radius_l, GpuInstanceRecord inst) {
    float supp = affine_sphere_plane_support(radius_l, plane_n, inst);
    float eps = 1e-4f * (abs(signed_val) + supp + 1.0f);
    return (signed_val + supp) >= -eps;
}

bool meshlet_frustum_visible(float3 center_l, float radius_l, float max_scale, GpuInstanceRecord inst,
                             GlobalFrameConstants fcb, out float3 world_center, out float world_radius) {
    world_center = mul(float4(center_l, 1.0f), inst.world).xyz;
    world_radius = radius_l * max_scale;

    float4 clip = mul(float4(world_center, 1.0f), fcb.view_projection);
    float pad_x = 1.0f + 2.0f / max(fcb.scene_viewport.z, 1.0f);
    float pad_y = 1.0f + 2.0f / max(fcb.scene_viewport.w, 1.0f);

    float3 col0 = float3(fcb.view_projection[0][0], fcb.view_projection[1][0], fcb.view_projection[2][0]);
    float3 col1 = float3(fcb.view_projection[0][1], fcb.view_projection[1][1], fcb.view_projection[2][1]);
    float3 col2 = float3(fcb.view_projection[0][2], fcb.view_projection[1][2], fcb.view_projection[2][2]);
    float3 col3 = float3(fcb.view_projection[0][3], fcb.view_projection[1][3], fcb.view_projection[2][3]);

    if (!test_homogeneous_plane(clip.w * pad_x + clip.x, col3 * pad_x + col0, radius_l, inst))
        return false;
    if (!test_homogeneous_plane(clip.w * pad_x - clip.x, col3 * pad_x - col0, radius_l, inst))
        return false;
    if (!test_homogeneous_plane(clip.w * pad_y + clip.y, col3 * pad_y + col1, radius_l, inst))
        return false;
    if (!test_homogeneous_plane(clip.w * pad_y - clip.y, col3 * pad_y - col1, radius_l, inst))
        return false;
    if (!test_homogeneous_plane(clip.z - fcb.camera_far_depth * clip.w, col2 - fcb.camera_far_depth * col3, radius_l, inst))
        return false;
    if (!test_homogeneous_plane(clip.w - clip.z, col3 - col2, radius_l, inst))
        return false;
    return true;
}

bool hzb_occluded(float3 world_center, float world_radius, row_major float4x4 test_view, row_major float4x4 test_proj,
                  float4 test_viewport, uint hzb_srv, uint2 hzb_mip0_dim, uint2 full_extent) {
    if (hzb_srv == 0xFFFFFFFFu || test_viewport.z < 2.0f || test_viewport.w < 2.0f)
        return false;
    // Reject skewed custom projections; standard perspective and orthographic projections have diagonal XY block:
    if (abs(test_proj[0][1]) > 1e-6f || abs(test_proj[1][0]) > 1e-6f ||
        abs(test_proj[0][3]) > 1e-6f || abs(test_proj[1][3]) > 1e-6f)
        return false;

    float3 cv = mul(float4(world_center, 1.0f), test_view).xyz;
    float r = world_radius * compute_matrix_max_scale(test_view);
    float z_near = cv.z - r;
    float z_far = cv.z + r;

    float2 ndc_min;
    float2 ndc_max;
    bool is_persp = (abs(test_proj[2][3]) > 1e-6f);
    if (is_persp) {
        if (test_proj[2][3] < 0.0f || z_near <= 0.05f || abs(test_proj[3][0]) > 1e-6f ||
            abs(test_proj[3][1]) > 1e-6f || abs(test_proj[3][3]) > 1e-6f)
            return false;
        // Exact Mara & McGuire 2013 view-space 2D tangent slopes for sphere strictly in front of eye (cv.z > r):
        float cz2_r2 = max(cv.z * cv.z - r * r, 1e-6f);
        float dx_disc = sqrt(max(0.0f, cv.x * cv.x + cz2_r2));
        float dy_disc = sqrt(max(0.0f, cv.y * cv.y + cz2_r2));

        float mx0 = (cv.x * cv.z - r * dx_disc) / cz2_r2;
        float mx1 = (cv.x * cv.z + r * dx_disc) / cz2_r2;
        float my0 = (cv.y * cv.z - r * dy_disc) / cz2_r2;
        float my1 = (cv.y * cv.z + r * dy_disc) / cz2_r2;

        float sx = test_proj[0][0] / test_proj[2][3];
        float ox = test_proj[2][0] / test_proj[2][3];
        float sy = test_proj[1][1] / test_proj[2][3];
        float oy = test_proj[2][1] / test_proj[2][3];

        float nx0 = mx0 * sx + ox;
        float nx1 = mx1 * sx + ox;
        float ny0 = my0 * sy + oy;
        float ny1 = my1 * sy + oy;
        ndc_min = float2(min(nx0, nx1), min(ny0, ny1));
        ndc_max = float2(max(nx0, nx1), max(ny0, ny1));
    } else {
        if (abs(test_proj[3][3]) <= 1e-6f)
            return false;
        float inv_w = 1.0f / test_proj[3][3];
        float cx_n = (cv.x * test_proj[0][0] + cv.z * test_proj[2][0] + test_proj[3][0]) * inv_w;
        float cy_n = (cv.y * test_proj[1][1] + cv.z * test_proj[2][1] + test_proj[3][1]) * inv_w;
        float rx_n = r * (abs(test_proj[0][0]) + abs(test_proj[2][0])) * abs(inv_w);
        float ry_n = r * (abs(test_proj[1][1]) + abs(test_proj[2][1])) * abs(inv_w);
        ndc_min = float2(cx_n - rx_n, cy_n - ry_n);
        ndc_max = float2(cx_n + rx_n, cy_n + ry_n);
    }

    // Reject spheres extending outside viewport NDC [-1, 1] (disocclusion border or partially off-viewport):
    if (ndc_min.x < -1.0f || ndc_max.x > 1.0f || ndc_min.y < -1.0f || ndc_max.y > 1.0f)
        return false;
    if ((ndc_max.x - ndc_min.x) > 1.0f || (ndc_max.y - ndc_min.y) > 1.0f)
        return false;

    // Conservative Reversed-Z maximum depth of the sphere (if > 1.0, sphere touches/crosses near clip plane!):
    float wn = z_near * test_proj[2][3] + test_proj[3][3];
    float wf = z_far * test_proj[2][3] + test_proj[3][3];
    if (wn <= 1e-5f || wf <= 1e-5f)
        return false;
    float dzn = (z_near * test_proj[2][2] + test_proj[3][2]) / wn;
    float dzf = (z_far * test_proj[2][2] + test_proj[3][2]) / wf;
    float sphere_near_dz = max(dzn, dzf);
    if (sphere_near_dz > 1.0f || sphere_near_dz <= 0.0f)
        return false;

    // Map NDC AABB to conservative integer full-resolution pixel rectangle (remember +Y NDC is top = min pixel Y!):
    float px_x0 = test_viewport.x + (ndc_min.x * 0.5f + 0.5f) * test_viewport.z - 0.5f;
    float px_x1 = test_viewport.x + (ndc_max.x * 0.5f + 0.5f) * test_viewport.z + 0.5f;
    float px_y0 = test_viewport.y + (0.5f - ndc_max.y * 0.5f) * test_viewport.w - 0.5f;
    float px_y1 = test_viewport.y + (0.5f - ndc_min.y * 0.5f) * test_viewport.w + 0.5f;

    float vp_right = min(test_viewport.x + test_viewport.z, float(full_extent.x));
    float vp_bottom = min(test_viewport.y + test_viewport.w, float(full_extent.y));
    if (px_x0 < test_viewport.x || px_y0 < test_viewport.y || px_x1 >= vp_right || px_y1 >= vp_bottom)
        return false;

    uint2 px_min = uint2(max(int2(floor(float2(px_x0, px_y0))), int2(0, 0)));
    uint2 px_max = min(uint2(max(int2(floor(float2(px_x1, px_y1))), int2(0, 0))), full_extent - 1u);

    Texture2D<float2> hzb = ResourceDescriptorHeap[atelier_safe_desc(hzb_srv)];
    uint hzb_w0 = 0u, hzb_h0 = 0u, hzb_levels = 1u;
    hzb.GetDimensions(0, hzb_w0, hzb_h0, hzb_levels);
    uint max_mip = (hzb_levels > 0u) ? (hzb_levels - 1u) : 0u;

    uint span_px = max(px_max.x - px_min.x, px_max.y - px_min.y);
    uint mip = (span_px <= 1u) ? 0u : min(firstbithigh(span_px - 1u), max_mip);
    uint2 mip_dim = max(uint2(1u, 1u), hzb_mip0_dim >> mip);
    uint2 t_min = min(px_min >> (mip + 1u), mip_dim - 1u);
    uint2 t_max = min(px_max >> (mip + 1u), mip_dim - 1u);
    if ((t_max.x - t_min.x) > 1u || (t_max.y - t_min.y) > 1u) {
        if (mip + 1u <= max_mip) {
            ++mip;
            mip_dim = max(uint2(1u, 1u), hzb_mip0_dim >> mip);
            t_min = min(px_min >> (mip + 1u), mip_dim - 1u);
            t_max = min(px_max >> (mip + 1u), mip_dim - 1u);
        }
        if ((t_max.x - t_min.x) > 1u || (t_max.y - t_min.y) > 1u)
            return false;
    }

    float d00 = hzb.Load(int3(int2(t_min.x, t_min.y), int(mip))).x;
    float d10 = hzb.Load(int3(int2(t_max.x, t_min.y), int(mip))).x;
    float d01 = hzb.Load(int3(int2(t_min.x, t_max.y), int(mip))).x;
    float d11 = hzb.Load(int3(int2(t_max.x, t_max.y), int(mip))).x;
    float min_occluder_z = min(min(d00, d10), min(d01, d11));
    return sphere_near_dz < min_occluder_z - 1e-5f;
}

void emit_visible_cluster(uint inst_id, uint ml_id, bool is_selected, GpuMeshlet ml, GpuInstanceRecord inst,
                          GpuMeshDescriptor mesh, GlobalFrameConstants fcb,
                          RWStructuredBuffer<GpuVisibleCluster> opaque_q,
                          RWStructuredBuffer<GpuVisibleCluster> masked_q,
                          RWStructuredBuffer<GpuVisibleCluster> selected_q,
                          RWStructuredBuffer<uint> counters) {
    GpuVisibleCluster item;
    item.instance_id = inst_id;
    item.meshlet_id = ml_id;

    bool routed_masked = false;
    if (fcb.masked_archetype_count > 0u) {
        StructuredBuffer<uint> face_meta_buf = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(mesh.face_meta_srv))];
        uint mat_slot = face_meta_buf[ml.global_tri_base] & 0x07FFFFFFu;
        StructuredBuffer<uint> slot_map = ResourceDescriptorHeap[atelier_safe_desc(fcb.material_slot_map_srv)];
        StructuredBuffer<GpuSurfaceRecord> surfaces = ResourceDescriptorHeap[atelier_safe_desc(fcb.material_surfaces_srv)];
        uint surf_idx = slot_map[inst.material_slot_offset + mat_slot];
        GpuSurfaceRecord surf = surfaces[surf_idx];
        if ((surf.flags & 8u) != 0u) {
            routed_masked = true;
        } else if ((surf.flags & 1u) != 0u && surf.masked_bin_index < fcb.masked_archetype_count) {
            uint dst_idx = 0u;
            InterlockedAdd(counters[6u + surf.masked_bin_index], 1u, dst_idx);
            if (dst_idx < fcb.masked_cluster_stride)
                masked_q[surf.masked_bin_index * fcb.masked_cluster_stride + dst_idx] = item;
            routed_masked = true;
        }
    }
    if (!routed_masked) {
        uint dst_idx = 0u;
        InterlockedAdd(counters[0u], 1u, dst_idx);
        if (dst_idx < fcb.max_opaque_clusters)
            opaque_q[dst_idx] = item;
    }

    if (is_selected && (g_flags_and_dst_h & 2u) != 0u && g_selected_clusters_uav != 0xFFFFFFFFu) {
        uint sel_idx = 0u;
        InterlockedAdd(counters[2u], 1u, sel_idx);
        if (sel_idx < fcb.max_selected_clusters)
            selected_q[sel_idx] = item;
    }
}

groupshared GpuInstanceRecord gs_cull_inst;
groupshared GpuMeshDescriptor gs_cull_mesh;
groupshared uint4 gs_cull_meta; // x: inst_id, y: first_ml, z: chunk_len, w: is_selected
groupshared float gs_cull_max_scale;
groupshared float gs_cull_cone_alpha;
groupshared uint gs_masked_sum[64];

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID, uint gtid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
    StructuredBuffer<GlobalFrameConstants> fcb_buf = ResourceDescriptorHeap[atelier_safe_desc(g_frame_cb_srv)];
    GlobalFrameConstants fcb = fcb_buf[0];

    if (g_phase_mode == 0u) {
        if (g_counters_uav != 0xFFFFFFFFu) {
            uint counter_words = 6u + 2u * fcb.masked_archetype_count;
            if (dtid.x < counter_words) {
                RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[atelier_safe_desc(g_counters_uav)];
                counters[dtid.x] = 0u;
            }
        }
        if (g_hzb_dst_uav != 0xFFFFFFFFu && dtid.x < 8u) {
            RWStructuredBuffer<uint> lit_cnt = ResourceDescriptorHeap[atelier_safe_desc(g_hzb_dst_uav)];
            lit_cnt[dtid.x] = 0u;
        }
        return;
    }

    if (g_phase_mode == 5u) {
        if (dtid.x == 0u) {
            RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[atelier_safe_desc(g_counters_uav)];
            RWByteAddressBuffer chunk_args = ResourceDescriptorHeap[atelier_safe_desc(g_aux_indirect_uav)];
            uint chunks = min(counters[5u], fcb.max_cull_chunks);
            counters[5u] = chunks;
            chunk_args.Store3(0u, make_indirect_grid_2d(chunks));
        }
        return;
    }

    if (g_phase_mode == 3u) {
        // Finalize Phase 1 DrawMeshTasksDesc {x, y, z} + Phase 2 CS indirect args in a single synchronized group:
        // Slot 0: Opaque; Slots 1..M: Masked[0..M-1]; Slot 1+M: Combined Visible Wireframe; Slot 2+M: Selected Only!
        RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[atelier_safe_desc(g_counters_uav)];
        RWByteAddressBuffer args = ResourceDescriptorHeap[atelier_safe_desc(g_indirect_args_uav)];
        uint total_draws = 1u + fcb.masked_archetype_count;
        uint max_p2 = fcb.max_opaque_clusters + fcb.masked_archetype_count * fcb.masked_cluster_stride;

        uint local_masked_sum = 0u;
        [loop] for (uint m = gtid; m < fcb.masked_archetype_count; m += 64u) {
            uint mc = min(counters[6u + m], fcb.masked_cluster_stride);
            counters[6u + m] = mc;
            counters[6u + fcb.masked_archetype_count + m] = mc;
            args.Store3((1u + m) * 12u, make_indirect_grid_2d(mc));
            local_masked_sum += mc;
        }
        gs_masked_sum[gtid] = local_masked_sum;
        GroupMemoryBarrierWithGroupSync();

        if (gtid == 0u) {
            uint op = min(counters[0u], fcb.max_opaque_clusters);
            counters[0u] = op;
            counters[3u] = op;
            args.Store3(0u, make_indirect_grid_2d(op));

            uint sel = min(counters[2u], fcb.max_selected_clusters);
            counters[2u] = sel;
            counters[4u] = sel;
            args.Store3((total_draws + 1u) * 12u, make_indirect_grid_2d(sel));

            uint p2 = min(counters[1u], max_p2);
            counters[1u] = p2;
            if ((g_flags_and_dst_h & 1u) != 0u && g_aux_indirect_uav != 0xFFFFFFFFu) {
                RWByteAddressBuffer p2_cs_args = ResourceDescriptorHeap[atelier_safe_desc(g_aux_indirect_uav)];
                p2_cs_args.Store3(0u, make_indirect_grid_2d((p2 + 63u) >> 6u));
            }

            uint sum_cnt = op;
            [unroll] for (uint t = 0u; t < 64u; ++t)
                sum_cnt += gs_masked_sum[t];
            args.Store3(total_draws * 12u, make_indirect_grid_2d(sum_cnt));
        }
        return;
    }

    if (g_phase_mode == 7u) {
        // Finalize Phase 2 delta DrawMeshTasksDesc + update Combined & Selected wireframe indirect slots in 1 group:
        RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[atelier_safe_desc(g_counters_uav)];
        RWByteAddressBuffer main_args = ResourceDescriptorHeap[atelier_safe_desc(g_indirect_args_uav)];
        RWByteAddressBuffer p2_args = ResourceDescriptorHeap[atelier_safe_desc(g_aux_indirect_uav)];
        uint total_draws = 1u + fcb.masked_archetype_count;

        uint local_masked_sum = 0u;
        [loop] for (uint m = gtid; m < fcb.masked_archetype_count; m += 64u) {
            uint mc_tot = min(counters[6u + m], fcb.masked_cluster_stride);
            counters[6u + m] = mc_tot;
            uint mc_p1 = counters[6u + fcb.masked_archetype_count + m];
            p2_args.Store3((1u + m) * 12u, make_indirect_grid_2d(mc_tot > mc_p1 ? (mc_tot - mc_p1) : 0u));
            local_masked_sum += mc_tot;
        }
        gs_masked_sum[gtid] = local_masked_sum;
        GroupMemoryBarrierWithGroupSync();

        if (gtid == 0u) {
            uint op_tot = min(counters[0u], fcb.max_opaque_clusters);
            counters[0u] = op_tot;
            uint op_p1 = counters[3u];
            p2_args.Store3(0u, make_indirect_grid_2d(op_tot > op_p1 ? (op_tot - op_p1) : 0u));

            uint sel_tot = min(counters[2u], fcb.max_selected_clusters);
            counters[2u] = sel_tot;
            main_args.Store3((total_draws + 1u) * 12u, make_indirect_grid_2d(sel_tot));

            uint sum_tot = op_tot;
            [unroll] for (uint t = 0u; t < 64u; ++t)
                sum_tot += gs_masked_sum[t];
            main_args.Store3(total_draws * 12u, make_indirect_grid_2d(sum_tot));
        }
        return;
    }

    if (g_phase_mode == 4u) {
        // Build 2x2 + odd-edge/corner min/max Reversed-Z HZB Mip Pyramid (x = min_depth, y = max_depth)
        uint dst_h = g_flags_and_dst_h & 0xFFFFu;
        bool src_is_d32 = (g_flags_and_dst_h & 0x10000u) != 0u;
        uint linear_px = (gid.y * 1024u + gid.x) * 64u + gtid;
        uint x = linear_px % max(g_hzb_dst_w, 1u);
        uint y = linear_px / max(g_hzb_dst_w, 1u);
        if (x >= g_hzb_dst_w || y >= dst_h)
            return;

        RWTexture2D<float2> dst = ResourceDescriptorHeap[atelier_safe_desc(g_hzb_dst_uav)];
        uint2 s0 = uint2(min(x * 2u, g_hzb_src_w - 1u), min(y * 2u, g_hzb_src_h - 1u));
        uint2 s1 = uint2(min(x * 2u + 1u, g_hzb_src_w - 1u), min(y * 2u, g_hzb_src_h - 1u));
        uint2 s2 = uint2(min(x * 2u, g_hzb_src_w - 1u), min(y * 2u + 1u, g_hzb_src_h - 1u));
        uint2 s3 = uint2(min(x * 2u + 1u, g_hzb_src_w - 1u), min(y * 2u + 1u, g_hzb_src_h - 1u));
        uint2 s_extra = uint2(min(x * 2u + 2u, g_hzb_src_w - 1u), min(y * 2u + 2u, g_hzb_src_h - 1u));
        bool odd_x = ((g_hzb_src_w & 1u) != 0u) && (x + 1u == g_hzb_dst_w);
        bool odd_y = ((g_hzb_src_h & 1u) != 0u) && (y + 1u == dst_h);

        if (src_is_d32) {
            Texture2D<float> d_src = ResourceDescriptorHeap[atelier_safe_desc(g_hzb_src_srv)];
            float a = d_src.Load(int3(s0, 0));
            float b = d_src.Load(int3(s1, 0));
            float c = d_src.Load(int3(s2, 0));
            float d = d_src.Load(int3(s3, 0));
            float mn = min(min(a, b), min(c, d));
            float mx = max(max(a, b), max(c, d));
            if (odd_x) {
                float e0 = d_src.Load(int3(s_extra.x, s0.y, 0));
                float e1 = d_src.Load(int3(s_extra.x, s2.y, 0));
                mn = min(mn, min(e0, e1));
                mx = max(mx, max(e0, e1));
            }
            if (odd_y) {
                float e0 = d_src.Load(int3(s0.x, s_extra.y, 0));
                float e1 = d_src.Load(int3(s1.x, s_extra.y, 0));
                mn = min(mn, min(e0, e1));
                mx = max(mx, max(e0, e1));
            }
            if (odd_x && odd_y) {
                float ec = d_src.Load(int3(s_extra.x, s_extra.y, 0));
                mn = min(mn, ec);
                mx = max(mx, ec);
            }
            dst[uint2(x, y)] = float2(mn, mx);
        } else {
            Texture2D<float2> h_src = ResourceDescriptorHeap[atelier_safe_desc(g_hzb_src_srv)];
            float2 r0 = h_src.Load(int3(s0, 0));
            float2 r1 = h_src.Load(int3(s1, 0));
            float2 r2 = h_src.Load(int3(s2, 0));
            float2 r3 = h_src.Load(int3(s3, 0));
            float mn = min(min(r0.x, r1.x), min(r2.x, r3.x));
            float mx = max(max(r0.y, r1.y), max(r2.y, r3.y));
            if (odd_x) {
                float2 e0 = h_src.Load(int3(s_extra.x, s0.y, 0));
                float2 e1 = h_src.Load(int3(s_extra.x, s2.y, 0));
                mn = min(mn, min(e0.x, e1.x));
                mx = max(mx, max(e0.y, e1.y));
            }
            if (odd_y) {
                float2 e0 = h_src.Load(int3(s0.x, s_extra.y, 0));
                float2 e1 = h_src.Load(int3(s1.x, s_extra.y, 0));
                mn = min(mn, min(e0.x, e1.x));
                mx = max(mx, max(e0.y, e1.y));
            }
            if (odd_x && odd_y) {
                float2 ec = h_src.Load(int3(s_extra.x, s_extra.y, 0));
                mn = min(mn, ec.x);
                mx = max(mx, ec.y);
            }
            dst[uint2(x, y)] = float2(mn, mx);
        }
        return;
    }

    StructuredBuffer<GpuInstanceRecord> instances = ResourceDescriptorHeap[atelier_safe_desc(fcb.instances_srv)];
    StructuredBuffer<GpuMeshDescriptor> meshes = ResourceDescriptorHeap[atelier_safe_desc(fcb.mesh_table_srv)];
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[atelier_safe_desc(g_counters_uav)];

    if (g_phase_mode == 1u) {
        // Mode 1: 1 thread per instance -> check CPU frustum bit -> select discrete G1 LOD -> emit 64-meshlet chunks
        uint inst_id = (gid.y * 1024u + gid.x) * 64u + gtid;
        if (inst_id >= fcb.instance_count)
            return;

        GpuInstanceRecord inst = instances[inst_id];
        if ((inst.flags & 2u) == 0u)
            return;

        StructuredBuffer<uint> sel_mask = ResourceDescriptorHeap[atelier_safe_desc(fcb.selected_mask_srv)];
        bool is_selected = ((sel_mask[inst_id >> 5u] >> (inst_id & 31u)) & 1u) != 0u;

        GpuMeshDescriptor mesh = meshes[inst.mesh_pool_index];
        uint chosen_lod = 0u;
        if (mesh.lod_count > 1u && !is_selected && fcb.wireframe_mode == 0u && fcb.instance_count > 16u) {
            float max_scale = compute_instance_max_scale(inst);
            float3 w_bounds_center = mul(float4(mesh.bounds_center, 1.0f), inst.world).xyz;
            float surf_dist = max(length(w_bounds_center - fcb.camera_eye) - mesh.bounds_radius * max_scale, 1e-2f);
            bool is_persp = (fcb.projection[2][3] != 0.0f);
            float proj_scale = abs(fcb.projection[1][1]) * 0.5f * fcb.scene_viewport.w;
            float px_per_meter = is_persp ? (proj_scale / surf_dist) : proj_scale;
            [unroll] for (uint l = 1u; l < 4u; ++l) {
                if (l < mesh.lod_count && mesh.lod_error[l] * max_scale * px_per_meter <= 1.25f)
                    chosen_lod = l;
            }
        }

        uint first_ml = mesh.lod_first_meshlet[chosen_lod];
        uint ml_count = mesh.lod_meshlet_count[chosen_lod];
        if (ml_count == 0u)
            return;

        uint num_chunks = (ml_count + 63u) >> 6u;
        uint base_chunk = 0u;
        InterlockedAdd(counters[5u], num_chunks, base_chunk);

        RWStructuredBuffer<uint2> chunks_buf = ResourceDescriptorHeap[atelier_safe_desc(g_chunks_uav)];
        uint sel_bit = is_selected ? 0x80000000u : 0u;
        [loop] for (uint c = 0u; c < num_chunks; ++c) {
            uint dst_c = base_chunk + c;
            if (dst_c >= fcb.max_cull_chunks)
                break;
            uint c_first = first_ml + c * 64u;
            uint c_len = min(64u, ml_count - c * 64u);
            chunks_buf[dst_c] = uint2(inst_id, (c_first & 0x01FFFFFFu) | ((c_len - 1u) << 25u) | sel_bit);
        }
        return;
    }

    RWStructuredBuffer<GpuVisibleCluster> opaque_q = ResourceDescriptorHeap[atelier_safe_desc(g_opaque_clusters_uav)];
    RWStructuredBuffer<GpuVisibleCluster> masked_q = ResourceDescriptorHeap[atelier_safe_desc(g_masked_clusters_uav)];
    RWStructuredBuffer<GpuVisibleCluster> selected_q = ResourceDescriptorHeap[atelier_safe_desc(g_selected_clusters_uav)];

    if (g_phase_mode == 6u) {
        // Mode 6: Parallel Meshlet Culling (1 group = 1 chunk, 1 thread = 1 meshlet!)
        uint chunk_id = gid.y * 1024u + gid.x;
        if (gtid == 0u) {
            uint valid_chunks = counters[5u];
            if (chunk_id < valid_chunks) {
                StructuredBuffer<uint2> chunks_buf = ResourceDescriptorHeap[atelier_safe_desc(g_chunks_uav)];
                uint2 ch = chunks_buf[chunk_id];
                GpuInstanceRecord inst = instances[ch.x];
                gs_cull_inst = inst;
                gs_cull_mesh = meshes[inst.mesh_pool_index];
                gs_cull_meta = uint4(ch.x, ch.y & 0x01FFFFFFu, ((ch.y >> 25u) & 63u) + 1u, ch.y >> 31u);
                gs_cull_max_scale = compute_instance_max_scale(inst);
                float3 r0 = inst.world[0].xyz;
                float3 r1 = inst.world[1].xyz;
                float3 r2 = inst.world[2].xyz;
                float q_diag = max(abs(dot(r0, inst.normal_row0)),
                                   max(abs(dot(r1, inst.normal_row1)), abs(dot(r2, inst.normal_row2))));
                float q_off = abs(dot(r0, inst.normal_row1)) + abs(dot(r0, inst.normal_row2)) +
                              abs(dot(r1, inst.normal_row0)) + abs(dot(r1, inst.normal_row2)) +
                              abs(dot(r2, inst.normal_row0)) + abs(dot(r2, inst.normal_row1));
                gs_cull_cone_alpha = max(0.0f, q_diag + q_off);
            } else {
                gs_cull_meta = uint4(0u, 0u, 0u, 0u);
                gs_cull_cone_alpha = 0.0f;
            }
        }
        GroupMemoryBarrierWithGroupSync();

        uint4 meta = gs_cull_meta;
        if (gtid >= meta.z)
            return;

        uint inst_id = meta.x;
        uint ml_id = meta.y + gtid;
        bool is_selected = (meta.w != 0u);
        GpuInstanceRecord inst = gs_cull_inst;
        GpuMeshDescriptor mesh = gs_cull_mesh;
        StructuredBuffer<GpuMeshlet> meshlets = ResourceDescriptorHeap[atelier_safe_desc(mesh.meshlets_srv)];
        GpuMeshlet ml = meshlets[ml_id];

        float3 w_center;
        float w_radius;
        if (!meshlet_frustum_visible(ml.center, ml.radius, gs_cull_max_scale, inst, fcb, w_center, w_radius))
            return;

        // Affine-invariant local-space backface normal-cone culling (handles non-uniform scale, shear, mirror det<0, & ortho!):
        uint cutoff_u6 = (ml.counts >> 26u) & 63u;
        if ((g_flags_and_dst_h & 4u) != 0u && cutoff_u6 < 63u && gs_cull_cone_alpha > 0.0f) {
            float2 oct_c = float2(int((ml.counts >> 16u) & 31u) - 15, int((ml.counts >> 21u) & 31u) - 15) * (1.0f / 15.0f);
            float3 cone_axis = oct_decode(oct_c);
            float cone_cutoff = float(cutoff_u6) * (1.0f / 63.0f);
            bool cone_backfacing = false;
            if (fcb.projection[2][3] != 0.0f) {
                float3 dw = w_center - fcb.inv_view[3].xyz;
                float3 v = float3(dot(dw, inst.normal_row0), dot(dw, inst.normal_row1), dot(dw, inst.normal_row2));
                float len_v = length(v);
                float bound = cone_cutoff * len_v + gs_cull_cone_alpha * ml.radius;
                cone_backfacing = dot(v, cone_axis) > bound + 1e-4f * (len_v + bound);
            } else {
                float3 fw = fcb.inv_view[2].xyz;
                float3 v = float3(dot(fw, inst.normal_row0), dot(fw, inst.normal_row1), dot(fw, inst.normal_row2));
                float len_v = length(v);
                cone_backfacing = (len_v > 0.0f) && (dot(v, cone_axis) > cone_cutoff * len_v + 1e-4f * len_v);
            }
            if (cone_backfacing) {
                StructuredBuffer<uint> face_meta_buf = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(mesh.face_meta_srv))];
                uint mat_slot = face_meta_buf[ml.global_tri_base] & 0x07FFFFFFu;
                StructuredBuffer<uint> slot_map = ResourceDescriptorHeap[atelier_safe_desc(fcb.material_slot_map_srv)];
                StructuredBuffer<GpuSurfaceRecord> surfaces = ResourceDescriptorHeap[atelier_safe_desc(fcb.material_surfaces_srv)];
                GpuSurfaceRecord surf = surfaces[slot_map[inst.material_slot_offset + mat_slot]];
                if ((surf.flags & 2u) == 0u)
                    return;
            }
        }

        if ((g_flags_and_dst_h & 1u) != 0u && g_phase2_candidates_uav != 0xFFFFFFFFu &&
            hzb_occluded(w_center, w_radius, fcb.prev_view, fcb.prev_projection, fcb.prev_scene_viewport,
                         g_hzb_src_srv, uint2(g_hzb_src_w, g_hzb_src_h), fcb.render_extent)) {
            uint max_p2 = fcb.max_opaque_clusters + fcb.masked_archetype_count * fcb.masked_cluster_stride;
            uint p2_idx = 0u;
            InterlockedAdd(counters[1u], 1u, p2_idx);
            if (p2_idx < max_p2) {
                RWStructuredBuffer<GpuVisibleCluster> phase2_q = ResourceDescriptorHeap[atelier_safe_desc(g_phase2_candidates_uav)];
                GpuVisibleCluster cand = {inst_id, ml_id | (is_selected ? 0x80000000u : 0u)};
                phase2_q[p2_idx] = cand;
                return;
            }
        }

        emit_visible_cluster(inst_id, ml_id, is_selected, ml, inst, mesh, fcb, opaque_q, masked_q, selected_q, counters);
        return;
    }

    if (g_phase_mode == 2u) {
        // Mode 2: Phase 2 HZB Re-Test of PrevHZB-rejected clusters against freshly built CurrHZB
        uint idx = (gid.y * 1024u + gid.x) * 64u + gtid;
        if (idx >= counters[1u])
            return;

        StructuredBuffer<GpuVisibleCluster> phase2_cands = ResourceDescriptorHeap[atelier_safe_desc(g_phase2_candidates_uav)];
        GpuVisibleCluster cand = phase2_cands[idx];
        uint inst_id = cand.instance_id;
        uint ml_id = cand.meshlet_id & 0x7FFFFFFFu;
        bool is_selected = (cand.meshlet_id >> 31u) != 0u;

        GpuInstanceRecord inst = instances[inst_id];
        GpuMeshDescriptor mesh = meshes[inst.mesh_pool_index];
        StructuredBuffer<GpuMeshlet> meshlets = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(mesh.meshlets_srv))];
        GpuMeshlet ml = meshlets[ml_id];

        float max_scale = compute_instance_max_scale(inst);
        float3 w_center = mul(float4(ml.center, 1.0f), inst.world).xyz;
        float w_radius = ml.radius * max_scale;

        if (hzb_occluded(w_center, w_radius, fcb.view, fcb.projection, fcb.scene_viewport, g_hzb_src_srv,
                         uint2(g_hzb_src_w, g_hzb_src_h), fcb.render_extent))
            return;

        emit_visible_cluster(inst_id, ml_id, is_selected, ml, inst, mesh, fcb, opaque_q, masked_q, selected_q, counters);
    }
}
