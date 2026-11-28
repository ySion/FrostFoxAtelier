// Fullscreen VisUnpackAndClassify CS + Prefix-Sum Binning Args Builder
// Modes:
//   0: Clear bin counts (strided over arbitrary archetype_count) & tile emissive bitmask
//   1: Unpack VisBuffer -> MotionVectors (RG16F) + PrevDepth (R32F) + OctGeomNormal (R32_UINT) + BaryDerivatives
//      + per-pixel `{arch + 1, instance_id}` metadata AND count pixels per active `.source` MaterialProgram archetype
//   2: Prefix-sum archetype counts & write 2D `CmdDispatchIndirect` arguments (4K-safe!)
//   3: Scatter `{packed_px, instance_id}` (`uint2`) into contiguous per-archetype Shading Bins
//   4: Fast UAV clear helper for Studio/Slab targets

cbuffer UnpackRootConstants : register(b0, space0) {
    uint g_frame_cb_srv;
    uint g_mode;
    uint g_visbuffer_srv;
    uint g_opaque_clusters_srv;
    uint g_masked_clusters_srv;
    uint g_pixel_meta_uav;
    uint g_motion_uav;
    uint g_prev_depth_uav;
    uint g_geom_normal_uav;
    uint g_bary_uav;
    uint g_bin_counts_uav;
    uint g_bin_offsets_uav;
    uint g_bin_cursors_uav;
    uint g_bin_pixels_uav;
    uint g_bin_indirect_uav;
    uint g_tile_emissive_uav;
};

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID, uint gindex : SV_GroupIndex, uint3 gid : SV_GroupID) {
    StructuredBuffer<GlobalFrameConstants> fcb_buf = ResourceDescriptorHeap[atelier_safe_desc(g_frame_cb_srv)];
    GlobalFrameConstants fcb = fcb_buf[0];

    if (g_mode == 0u) {
        uint idx = gid.x * 64u + gindex;
        RWStructuredBuffer<uint> counts = ResourceDescriptorHeap[atelier_safe_desc(g_bin_counts_uav)];
        uint total_arch = max(fcb.archetype_count, 1u);
        for (uint a = idx; a < total_arch; a += 256u)
            counts[a] = 0u;
        if (g_tile_emissive_uav != 0xFFFFFFFFu) {
            uint tile_words = (((fcb.render_extent.x + 7u) / 8u) * ((fcb.render_extent.y + 7u) / 8u) + 31u) / 32u;
            RWStructuredBuffer<uint> em_mask = ResourceDescriptorHeap[atelier_safe_desc(g_tile_emissive_uav)];
            for (uint w = idx; w < tile_words; w += 256u)
                em_mask[w] = 0u;
        }
        if (g_prev_depth_uav != 0xFFFFFFFFu && idx == 0u) {
            RWByteAddressBuffer sss_args = ResourceDescriptorHeap[atelier_safe_desc(g_prev_depth_uav)];
            sss_args.Store3(0u, uint3(0u, 1u, 1u));
        }
        return;
    }

    if (g_mode == 2u) {
        if (gid.x == 0u && gindex == 0u) {
            RWStructuredBuffer<uint> counts = ResourceDescriptorHeap[atelier_safe_desc(g_bin_counts_uav)];
            RWStructuredBuffer<uint> offsets = ResourceDescriptorHeap[atelier_safe_desc(g_bin_offsets_uav)];
            RWStructuredBuffer<uint> cursors = ResourceDescriptorHeap[atelier_safe_desc(g_bin_cursors_uav)];
            RWByteAddressBuffer indirect_args = ResourceDescriptorHeap[atelier_safe_desc(g_bin_indirect_uav)];
            uint running = 0u;
            [loop] for (uint a = 0u; a < fcb.archetype_count; ++a) {
                uint c = counts[a];
                offsets[a] = running;
                cursors[a] = running;
                running += c;
                uint groups = (c + 63u) >> 6u;
                uint3 grid = (groups <= 65535u) ? uint3(groups, 1u, 1u) : uint3(1024u, (groups + 1023u) >> 10u, 1u);
                indirect_args.Store3(a * 12u, grid);
            }
        }
        return;
    }

    uint2 px = dtid.xy;
    if (px.x >= fcb.render_extent.x || px.y >= fcb.render_extent.y)
        return;

    if (g_mode == 4u) {
        if (g_motion_uav != 0xFFFFFFFFu) {
            RWTexture2D<float4> c0 = ResourceDescriptorHeap[atelier_safe_desc(g_motion_uav)];
            c0[px] = 0.0f;
        }
        if (g_prev_depth_uav != 0xFFFFFFFFu) {
            RWTexture2D<float4> c1 = ResourceDescriptorHeap[atelier_safe_desc(g_prev_depth_uav)];
            c1[px] = 0.0f;
        }
        if (g_geom_normal_uav != 0xFFFFFFFFu) {
            RWTexture2D<uint2> u0 = ResourceDescriptorHeap[atelier_safe_desc(g_geom_normal_uav)];
            u0[px] = uint2(0u, 0u);
        }
        return;
    }

    if (g_mode == 5u) {
        Texture2D<uint> vis_pick_tex = ResourceDescriptorHeap[atelier_safe_desc(g_visbuffer_srv)];
        RWTexture2D<uint2> pick_uav = ResourceDescriptorHeap[atelier_safe_desc(g_bin_pixels_uav)];
        uint vis_p = vis_pick_tex.Load(int3(px, 0));
        if (vis_p == VISBUFFER_EMPTY) {
            pick_uav[px] = uint2(0u, 0u);
            return;
        }
        uint c_idx = (vis_p >> 8u) - 1u;
        uint l_tri = (vis_p >> 1u) & 0x7Fu;
        GpuVisibleCluster vc_p;
        if ((c_idx & 0x800000u) != 0u) {
            StructuredBuffer<GpuVisibleCluster> mq_p = ResourceDescriptorHeap[atelier_safe_desc(g_masked_clusters_srv)];
            vc_p = mq_p[c_idx & 0x7FFFFFu];
        } else {
            StructuredBuffer<GpuVisibleCluster> oq_p = ResourceDescriptorHeap[atelier_safe_desc(g_opaque_clusters_srv)];
            vc_p = oq_p[c_idx];
        }
        StructuredBuffer<GpuInstanceRecord> insts_p = ResourceDescriptorHeap[atelier_safe_desc(fcb.instances_srv)];
        StructuredBuffer<GpuMeshDescriptor> meshes_p = ResourceDescriptorHeap[atelier_safe_desc(fcb.mesh_table_srv)];
        GpuInstanceRecord inst_p = insts_p[vc_p.instance_id];
        GpuMeshDescriptor mesh_p = meshes_p[inst_p.mesh_pool_index];
        StructuredBuffer<GpuMeshlet> mls_p = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(mesh_p.meshlets_srv))];
        pick_uav[px] = uint2(vc_p.instance_id + 1u, mls_p[vc_p.meshlet_id].global_tri_base + l_tri);
        return;
    }

    uint px_linear = px.y * fcb.render_extent.x + px.x;

    if (g_mode == 3u) {
        RWStructuredBuffer<uint2> pixel_meta = ResourceDescriptorHeap[atelier_safe_desc(g_pixel_meta_uav)];
        uint2 pmeta = pixel_meta[px_linear];
        if (pmeta.x == 0u)
            return;
        uint arch = pmeta.x - 1u;
        RWStructuredBuffer<uint> cursors = ResourceDescriptorHeap[atelier_safe_desc(g_bin_cursors_uav)];
        RWStructuredBuffer<uint2> bin_pixels = ResourceDescriptorHeap[atelier_safe_desc(g_bin_pixels_uav)];
        uint slot = 0u;
        InterlockedAdd(cursors[arch], 1u, slot);
        bin_pixels[slot] = uint2((px.x & 0xFFFFu) | (px.y << 16u), pmeta.y);
        return;
    }

    // Mode 1: Unpack VisBuffer + MotionVectors + Analytical Barycentrics + Archetype Count
    Texture2D<uint> vis_tex = ResourceDescriptorHeap[atelier_safe_desc(g_visbuffer_srv)];
    uint vis = vis_tex.Load(int3(px, 0));

    RWTexture2D<float2> motion_uav = ResourceDescriptorHeap[atelier_safe_desc(g_motion_uav)];
    RWTexture2D<float> prev_z_uav = ResourceDescriptorHeap[atelier_safe_desc(g_prev_depth_uav)];
    RWTexture2D<uint> geom_n_uav = ResourceDescriptorHeap[atelier_safe_desc(g_geom_normal_uav)];
    RWStructuredBuffer<GpuBaryDerivatives> bary_uav = ResourceDescriptorHeap[atelier_safe_desc(g_bary_uav)];
    RWStructuredBuffer<uint2> pixel_meta = ResourceDescriptorHeap[atelier_safe_desc(g_pixel_meta_uav)];

    if (vis == VISBUFFER_EMPTY) {
        float2 sky_motion = 0.0f;
        if ((fcb.flags & (1u << 5u)) != 0u && fcb.prev_unjittered_vp[2][3] != 0.0f && fcb.unjittered_view_projection[2][3] != 0.0f) {
            float2 px_center = float2(px) + 0.5f;
            if (px_center.x >= fcb.scene_viewport.x && px_center.x < fcb.scene_viewport.x + fcb.scene_viewport.z &&
                px_center.y >= fcb.scene_viewport.y && px_center.y < fcb.scene_viewport.y + fcb.scene_viewport.w) {
                float2 ndc = float2((px_center.x - fcb.scene_viewport.x) / fcb.scene_viewport.z * 2.0f - 1.0f,
                                    1.0f - (px_center.y - fcb.scene_viewport.y) / fcb.scene_viewport.w * 2.0f);
                float4 world_far = mul(float4(ndc, 0.5f, 1.0f), fcb.inv_unjittered_vp);
                float3 far_pt = (abs(world_far.w) > 1e-6f) ? (world_far.xyz / world_far.w) : world_far.xyz;
                float3 dir = safe_unit_normal(far_pt - fcb.camera_eye);
                float4 prev_clip = mul(float4(dir, 0.0f), fcb.prev_unjittered_vp);
                if (prev_clip.w > 1e-4f) {
                    float2 prev_px = float2(fcb.prev_scene_viewport.x + (prev_clip.x / prev_clip.w * 0.5f + 0.5f) * fcb.prev_scene_viewport.z,
                                            fcb.prev_scene_viewport.y + (0.5f - prev_clip.y / prev_clip.w * 0.5f) * fcb.prev_scene_viewport.w);
                    sky_motion = clamp((prev_px - px_center) / float2(fcb.render_extent), -2.0f, 2.0f);
                } else {
                    sky_motion = float2(-4.0f, -4.0f);
                }
            }
        }
        motion_uav[px] = sky_motion;
        prev_z_uav[px] = 0.0f;
        geom_n_uav[px] = 0u;
        pixel_meta[px_linear] = uint2(0u, 0u);
        if (g_bin_pixels_uav != 0xFFFFFFFFu) {
            RWTexture2D<uint2> pick_uav = ResourceDescriptorHeap[atelier_safe_desc(g_bin_pixels_uav)];
            pick_uav[px] = uint2(0u, 0u);
        }
        return;
    }

    uint cluster_idx = (vis >> 8u) - 1u;
    uint local_tri = (vis >> 1u) & 0x7Fu;
    bool is_front = (vis & 1u) != 0u;

    GpuVisibleCluster vc;
    if ((cluster_idx & 0x800000u) != 0u) {
        StructuredBuffer<GpuVisibleCluster> mq = ResourceDescriptorHeap[atelier_safe_desc(g_masked_clusters_srv)];
        vc = mq[cluster_idx & 0x7FFFFFu];
    } else {
        StructuredBuffer<GpuVisibleCluster> oq = ResourceDescriptorHeap[atelier_safe_desc(g_opaque_clusters_srv)];
        vc = oq[cluster_idx];
    }

    StructuredBuffer<GpuInstanceRecord> instances = ResourceDescriptorHeap[atelier_safe_desc(fcb.instances_srv)];
    StructuredBuffer<GpuMeshDescriptor> meshes = ResourceDescriptorHeap[atelier_safe_desc(fcb.mesh_table_srv)];
    GpuInstanceRecord inst = instances[vc.instance_id];
    GpuMeshDescriptor mesh = meshes[inst.mesh_pool_index];

    StructuredBuffer<GpuMeshlet> meshlets = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(mesh.meshlets_srv))];
    GpuMeshlet ml = meshlets[vc.meshlet_id];
    uint global_tri_id = ml.global_tri_base + local_tri;

    StructuredBuffer<uint> blas_idx = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(mesh.blas_indices_srv))];
    uint gi0 = blas_idx[global_tri_id * 3u + 0u];
    uint gi1 = blas_idx[global_tri_id * 3u + 1u];
    uint gi2 = blas_idx[global_tri_id * 3u + 2u];

    StructuredBuffer<GpuPackedPosition> pos_stream = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(mesh.positions_srv))];
    StructuredBuffer<GpuPackedAttributes> attr_stream = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(mesh.attributes_srv))];
    StructuredBuffer<uint> face_meta_buf = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(mesh.face_meta_srv))];

    float3 lp0 = pos_stream[gi0].pos;
    float3 lp1 = pos_stream[gi1].pos;
    float3 lp2 = pos_stream[gi2].pos;
    uint fmeta = face_meta_buf[global_tri_id];
    bool winding_flipped = (fmeta >> 31u) != 0u;
    uint edge_mask = ((fcb.flags & (1u << 10u)) != 0u) ? 7u : ((fmeta >> 27u) & 7u);
    uint mat_slot = fmeta & 0x07FFFFFFu;

    StructuredBuffer<uint> slot_map = ResourceDescriptorHeap[atelier_safe_desc(fcb.material_slot_map_srv)];
    uint surf_idx = slot_map[inst.material_slot_offset + mat_slot];

    precise float4 wp0 = mul(float4(lp0, 1.0f), inst.world);
    precise float4 wp1 = mul(float4(lp1, 1.0f), inst.world);
    precise float4 wp2 = mul(float4(lp2, 1.0f), inst.world);

    precise float4 c0 = mul(wp0, fcb.view_projection);
    precise float4 c1 = mul(wp1, fcb.view_projection);
    precise float4 c2 = mul(wp2, fcb.view_projection);

    float2 px_center = float2(px) + 0.5f;
    float2 ndc = float2((px_center.x - fcb.scene_viewport.x) / max(fcb.scene_viewport.z, 1.0f) * 2.0f - 1.0f,
                        1.0f - (px_center.y - fcb.scene_viewport.y) / max(fcb.scene_viewport.w, 1.0f) * 2.0f);

    // Exact perspective-correct 2x2 homogeneous barycentric solver with simplex projection:
    float2 s0 = c0.xy - ndc * c0.w;
    float2 s1 = c1.xy - ndc * c1.w;
    float2 s2 = c2.xy - ndc * c2.w;
    float2 d1 = s1 - s0;
    float2 d2 = s2 - s0;
    float det = d1.x * d2.y - d1.y * d2.x;
    bool valid_det = (abs(det) > 1e-20f) && atelier_finite_f(det);
    float inv_det = valid_det ? (1.0f / det) : 0.0f;

    float2 bary;
    bary.x = (-s0.x * d2.y + s0.y * d2.x) * inv_det;
    bary.y = (d1.x * (-s0.y) - d1.y * (-s0.x)) * inv_det;
    bary = valid_det ? max(bary, 0.0f) : float2(1.0f / 3.0f, 1.0f / 3.0f);
    float bary_sum = bary.x + bary.y;
    if (bary_sum > 1.0f)
        bary /= bary_sum;
    float w0 = 1.0f - bary.x - bary.y;

    float clip_w = max(w0 * c0.w + bary.x * c1.w + bary.y * c2.w, 1e-6f);
    float dndc_dx = 2.0f / max(fcb.scene_viewport.z, 1.0f);
    float dndc_dy = -2.0f / max(fcb.scene_viewport.w, 1.0f);
    float2 unavail = (float2)asfloat(0x7fc00000u);
    float2 db_dx = valid_det ? ((dndc_dx * clip_w * inv_det) * float2(d2.y, -d1.y)) : unavail;
    float2 db_dy = valid_det ? ((dndc_dy * clip_w * inv_det) * float2(-d2.x, d1.x)) : unavail;

    // Current & previous unjittered window UV for MotionVectors + PrevDepth:
    float3 p_curr = wp0.xyz + (wp1.xyz - wp0.xyz) * bary.x + (wp2.xyz - wp0.xyz) * bary.y;
    float3 p_prev = ((inst.flags & 1u) != 0u)
                        ? mul(float4(lp0 * w0 + lp1 * bary.x + lp2 * bary.y, 1.0f), inst.prev_world).xyz
                        : p_curr;

    float4 curr_unjit = mul(float4(p_curr, 1.0f), fcb.unjittered_view_projection);
    float4 prev_unjit = mul(float4(p_prev, 1.0f), fcb.prev_unjittered_vp);
    float2 motion = 0.0f;
    float prev_depth = 0.0f;
    if (abs(curr_unjit.w) > 1e-6f && prev_unjit.w > 1e-6f) {
        float2 curr_win_px = float2(fcb.scene_viewport.x + (curr_unjit.x / curr_unjit.w * 0.5f + 0.5f) * fcb.scene_viewport.z,
                                    fcb.scene_viewport.y + (0.5f - curr_unjit.y / curr_unjit.w * 0.5f) * fcb.scene_viewport.w);
        float2 prev_win_px = float2(fcb.prev_scene_viewport.x + (prev_unjit.x / prev_unjit.w * 0.5f + 0.5f) * fcb.prev_scene_viewport.z,
                                    fcb.prev_scene_viewport.y + (0.5f - prev_unjit.y / prev_unjit.w * 0.5f) * fcb.prev_scene_viewport.w);
        motion = clamp((prev_win_px - curr_win_px) / float2(fcb.render_extent), -2.0f, 2.0f);
        prev_depth = saturate(prev_unjit.z / prev_unjit.w);
    }
    motion_uav[px] = motion;
    prev_z_uav[px] = prev_depth;

    float3 vn0 = unpack_oct_snorm16(attr_stream[gi0].oct_normal);
    float3 gn = reconstruct_geom_normal(lp0, lp1, lp2, vn0, winding_flipped, inst);
    if (!is_front)
        gn = -gn;
    geom_n_uav[px] = pack_oct_snorm16(gn);

    GpuBaryDerivatives bd;
    bd.bary = bary;
    bd.db_dx = db_dx;
    bd.db_dy = db_dy;
    bd.surface_and_flags = (surf_idx & 0x07FFFFFFu) | (winding_flipped ? (1u << 27u) : 0u) |
                           (is_front ? 0u : (1u << 28u)) | (edge_mask << 29u);
    bd.global_tri_id = global_tri_id;
    bary_uav[px_linear] = bd;

    StructuredBuffer<GpuSurfaceRecord> surfaces = ResourceDescriptorHeap[atelier_safe_desc(fcb.material_surfaces_srv)];
    uint arch = surfaces[surf_idx].archetype_index;
    pixel_meta[px_linear] = uint2(arch + 1u, vc.instance_id);
    if (g_bin_pixels_uav != 0xFFFFFFFFu) {
        RWTexture2D<uint2> pick_uav = ResourceDescriptorHeap[atelier_safe_desc(g_bin_pixels_uav)];
        pick_uav[px] = uint2(vc.instance_id + 1u, global_tri_id);
    }

    RWStructuredBuffer<uint> counts = ResourceDescriptorHeap[atelier_safe_desc(g_bin_counts_uav)];
    InterlockedAdd(counts[arch], 1u);
}
