// GPU Meshlet-to-DirtyPage Culling for 128 MiB Copy-on-Write Software-Paged D32_FLOAT VSM
//   Mode 0: Clear VSM cluster counters (Opaque + Masked Archetype Bins)
//   Mode 1: Expand (PageJob, Instance) pairs -> 6-plane Gribb-Hartmann sphere test per LOD0 Meshlet -> emit GpuVsmVisibleCluster
//   Mode 2: Clamp counters & emit 2D-wrapped DrawMeshTasksDesc (1024-group X cap) for Opaque + Masked Archetype PSOs

cbuffer VsmCullRootConstants : register(b0, space0) {
    uint g_frame_cb_srv;
    uint g_vsm_cull_mode;
    uint g_page_jobs_srv;
    uint g_pairs_srv;
    uint g_pair_count;
    uint g_max_opaque;
    uint g_masked_stride;
    uint g_opaque_clusters_uav;
    uint g_masked_clusters_uav;
    uint g_counters_uav;
    uint g_indirect_args_uav;
    uint g_pad0;
};

struct FrustumPlanes6 {
    float4 p[6];
};

FrustumPlanes6 extract_planes6(row_major float4x4 m) {
    float4 c0 = float4(m[0][0], m[1][0], m[2][0], m[3][0]);
    float4 c1 = float4(m[0][1], m[1][1], m[2][1], m[3][1]);
    float4 c2 = float4(m[0][2], m[1][2], m[2][2], m[3][2]);
    float4 c3 = float4(m[0][3], m[1][3], m[2][3], m[3][3]);

    FrustumPlanes6 fp;
    fp.p[0] = c3 + c0; // left:   x + w >= 0
    fp.p[1] = c3 - c0; // right:  w - x >= 0
    fp.p[2] = c3 + c1; // bottom: y + w >= 0
    fp.p[3] = c3 - c1; // top:    w - y >= 0
    fp.p[4] = c2;      // far:    z >= 0 (Reversed-Z)
    fp.p[5] = c3 - c2; // near:   w - z >= 0
    [unroll] for (uint i = 0u; i < 6u; ++i) {
        float inv_len = rsqrt(max(dot(fp.p[i].xyz, fp.p[i].xyz), 1e-20f));
        fp.p[i] *= inv_len;
    }
    return fp;
}

bool sphere_in_vsm_page(FrustumPlanes6 fp, float3 center, float radius, bool ortho_pancake) {
    uint plane_cnt = ortho_pancake ? 5u : 6u; // Ortho cascades pancake tall occluders above near plane instead of clipping!
    [unroll] for (uint i = 0u; i < 6u; ++i) {
        if (i < plane_cnt) {
            float signed_d = dot(fp.p[i].xyz, center) + fp.p[i].w;
            float eps = 1e-4f * (abs(signed_d) + radius + 1.0f);
            if (signed_d + radius < -eps)
                return false;
        }
    }
    return true;
}

float vsm_matrix_max_scale(row_major float4x4 m) {
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

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID, uint gindex : SV_GroupIndex, uint3 gid : SV_GroupID) {
    StructuredBuffer<GlobalFrameConstants> fcb_buf = ResourceDescriptorHeap[atelier_safe_desc(g_frame_cb_srv)];
    GlobalFrameConstants fcb = fcb_buf[0];

    if (g_vsm_cull_mode == 0u) {
        RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[atelier_safe_desc(g_counters_uav)];
        uint idx = dtid.x;
        if (idx < 1u + fcb.masked_archetype_count)
            counters[idx] = 0u;
        return;
    }

    if (g_vsm_cull_mode == 2u) {
        RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[atelier_safe_desc(g_counters_uav)];
        RWByteAddressBuffer indirect = ResourceDescriptorHeap[atelier_safe_desc(g_indirect_args_uav)];
        uint idx = dtid.x;
        if (idx == 0u) {
            uint cnt = min(counters[0u], g_max_opaque);
            counters[0u] = cnt;
            uint gx = (cnt == 0u) ? 0u : min(cnt, 1024u);
            uint gy = (cnt == 0u) ? 0u : ((cnt + 1023u) / 1024u);
            indirect.Store3(0u, uint3(gx, gy, 1u));
        } else if (idx <= fcb.masked_archetype_count) {
            uint m = idx - 1u;
            uint cnt = min(counters[1u + m], g_masked_stride);
            counters[1u + m] = cnt;
            uint gx = (cnt == 0u) ? 0u : min(cnt, 1024u);
            uint gy = (cnt == 0u) ? 0u : ((cnt + 1023u) / 1024u);
            indirect.Store3((1u + m) * 12u, uint3(gx, gy, 1u));
        }
        return;
    }

    // Mode 1: Grid-stride over (PageJob, Instance) pairs (64 threads per group test all LOD0 Meshlets)!
    StructuredBuffer<uint2> pairs = ResourceDescriptorHeap[atelier_safe_desc(g_pairs_srv)];
    StructuredBuffer<GpuVsmPageJob> jobs = ResourceDescriptorHeap[atelier_safe_desc(g_page_jobs_srv)];
    StructuredBuffer<GpuInstanceRecord> instances = ResourceDescriptorHeap[atelier_safe_desc(fcb.instances_srv)];
    StructuredBuffer<GpuMeshDescriptor> meshes = ResourceDescriptorHeap[atelier_safe_desc(fcb.mesh_table_srv)];
    StructuredBuffer<uint> slot_map = ResourceDescriptorHeap[atelier_safe_desc(fcb.material_slot_map_srv)];
    StructuredBuffer<GpuSurfaceRecord> surfaces = ResourceDescriptorHeap[atelier_safe_desc(fcb.material_surfaces_srv)];
    RWStructuredBuffer<GpuVsmVisibleCluster> opaque_out = ResourceDescriptorHeap[atelier_safe_desc(g_opaque_clusters_uav)];
    RWStructuredBuffer<GpuVsmVisibleCluster> masked_out = ResourceDescriptorHeap[atelier_safe_desc(g_masked_clusters_uav)];
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[atelier_safe_desc(g_counters_uav)];

    [loop] for (uint pair_idx = gid.y * 1024u + gid.x; pair_idx < g_pair_count; pair_idx += 1024u * 65535u) {
        uint2 pair = pairs[pair_idx];
        uint job_idx = pair.x;
        uint inst_id = pair.y;
        if (inst_id >= fcb.instance_count)
            continue;

        GpuVsmPageJob job = jobs[job_idx];
        bool ortho_pancake = (job.flags & 1u) != 0u;
        FrustumPlanes6 fp = extract_planes6(job.page_view_proj);

        GpuInstanceRecord inst = instances[inst_id];
        GpuMeshDescriptor mesh = meshes[inst.mesh_pool_index];
        float max_scale = vsm_matrix_max_scale(inst.world);

        uint first_ml = mesh.lod_first_meshlet.x;
        uint ml_count = mesh.lod_meshlet_count.x;
        StructuredBuffer<GpuMeshlet> meshlets = ResourceDescriptorHeap[atelier_safe_desc(mesh.meshlets_srv)];
        StructuredBuffer<uint> face_meta_buf = ResourceDescriptorHeap[atelier_safe_desc(mesh.face_meta_srv)];

        [loop] for (uint m_off = gindex; m_off < ml_count; m_off += 64u) {
            uint ml_id = first_ml + m_off;
            GpuMeshlet ml = meshlets[ml_id];
            float3 wc = mul(float4(ml.center, 1.0f), inst.world).xyz;
            float wr = ml.radius * max_scale;
            if (!sphere_in_vsm_page(fp, wc, wr, ortho_pancake))
                continue;

            uint mat_slot = face_meta_buf[ml.global_tri_base] & 0x07FFFFFFu;
            uint surf_idx = slot_map[inst.material_slot_offset + mat_slot];
            GpuSurfaceRecord surf = surfaces[surf_idx];
            if ((surf.flags & 8u) != 0u)
                continue;

            GpuVsmVisibleCluster vc;
            vc.page_job_idx = job_idx;
            vc.instance_id = inst_id;
            vc.meshlet_id = ml_id;
            vc.surface_idx = surf_idx;

            if ((surf.flags & 1u) != 0u && surf.masked_bin_index < fcb.masked_archetype_count) {
                uint dst_slot = 0u;
                InterlockedAdd(counters[1u + surf.masked_bin_index], 1u, dst_slot);
                if (dst_slot < g_masked_stride)
                    masked_out[surf.masked_bin_index * g_masked_stride + dst_slot] = vc;
            } else {
                uint dst_slot = 0u;
                InterlockedAdd(counters[0u], 1u, dst_slot);
                if (dst_slot < g_max_opaque)
                    opaque_out[dst_slot] = vc;
            }
        }
    }
}
