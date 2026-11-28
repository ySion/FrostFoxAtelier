// Hardware D32_FLOAT Physical Page Mesh Shader for 128 MiB Copy-on-Write Software-Paged VSM
// Clips strictly to each 128x128 physical page via 4-plane homogeneous SV_ClipDistance0 (zero cross-page bleeding!)

#ifndef VSM_MASKED
#define VSM_MASKED 0
#endif

#if !VSM_MASKED
cbuffer ArchetypeRootConstants : register(b0, space0) {
    uint g_frame_cb_srv;
    uint g_frame_cb_idx; // page_jobs_srv
    uint g_material_inputs_srv;
    uint g_material_params_srv;
    uint g_material_surfaces_srv;
    uint g_archetype_index;
    uint g_bin_offset_or_cluster_srv; // vsm_opaque_clusters / vsm_masked_clusters
    uint g_aux0_srv;                  // vsm_counters
    uint g_aux1_srv;
    uint g_aux2_uav;
    uint g_aux3_uav;
    uint g_aux4_uav;
    uint g_aux5_uav;
    uint g_pass_flags; // masked_stride
    uint g_viewport_packed0;
    uint g_viewport_packed1;
};
#endif

struct VsmVertex {
    precise float4 position : SV_Position;
    float4 page_clip : SV_ClipDistance0;
#if VSM_MASKED
    float2 uv : TEXCOORD0;
    float3 world_pos : TEXCOORD1;
    float3 world_normal : TEXCOORD2;
    float4 world_tangent : TEXCOORD3;
#endif
};

struct VsmPrimitive {
    bool cull : SV_CullPrimitive;
#if VSM_MASKED
    uint surface_idx : SURF_ID;
    float3 geom_normal : GEOM_N;
#endif
};

groupshared GpuVsmPageJob gs_vsm_job;
groupshared GpuInstanceRecord gs_vsm_inst;
groupshared GpuMeshDescriptor gs_vsm_mesh;
groupshared GpuMeshlet gs_vsm_ml;
groupshared uint gs_vsm_counts[3]; // [0]=vert_count, [1]=tri_count, [2]=surf_idx
groupshared float3 gs_vsm_local_pos[64];
groupshared float3 gs_vsm_local_norm[64];
groupshared float4 gs_vsm_page_clip[64];

[outputtopology("triangle")]
[numthreads(128, 1, 1)]
void vsm_ms_main(uint gtid : SV_GroupThreadID, uint3 gid : SV_GroupID,
                 out vertices VsmVertex out_verts[64],
                 out primitives VsmPrimitive out_prims[124],
                 out indices uint3 out_Triangles[124]) {
    StructuredBuffer<GlobalFrameConstants> fcb_buf = ResourceDescriptorHeap[atelier_safe_desc(g_frame_cb_srv)];
    GlobalFrameConstants fcb = fcb_buf[0];

    if (gtid == 0u) {
        StructuredBuffer<uint> counters = ResourceDescriptorHeap[atelier_safe_desc(g_aux0_srv)];
        uint group_linear = gid.y * 1024u + gid.x;
#if VSM_MASKED
        uint visible_count = counters[1u + g_archetype_index];
        uint cluster_base = g_archetype_index * g_pass_flags;
#else
        uint visible_count = counters[0u];
        uint cluster_base = 0u;
#endif
        if (group_linear < visible_count) {
            StructuredBuffer<GpuVsmVisibleCluster> clusters = ResourceDescriptorHeap[atelier_safe_desc(g_bin_offset_or_cluster_srv)];
            GpuVsmVisibleCluster vc = clusters[cluster_base + group_linear];

            StructuredBuffer<GpuVsmPageJob> jobs = ResourceDescriptorHeap[atelier_safe_desc(g_frame_cb_idx)];
            StructuredBuffer<GpuInstanceRecord> instances = ResourceDescriptorHeap[atelier_safe_desc(fcb.instances_srv)];
            StructuredBuffer<GpuMeshDescriptor> meshes = ResourceDescriptorHeap[atelier_safe_desc(fcb.mesh_table_srv)];
            GpuInstanceRecord inst = instances[vc.instance_id];
            GpuMeshDescriptor mesh = meshes[inst.mesh_pool_index];
            StructuredBuffer<GpuMeshlet> meshlets = ResourceDescriptorHeap[atelier_safe_desc(mesh.meshlets_srv)];
            GpuMeshlet ml = meshlets[vc.meshlet_id];

            gs_vsm_job = jobs[vc.page_job_idx];
            gs_vsm_inst = inst;
            gs_vsm_mesh = mesh;
            gs_vsm_ml = ml;
            gs_vsm_counts[0] = ml.counts & 0xFFu;
            gs_vsm_counts[1] = (ml.counts >> 8u) & 0xFFu;
            gs_vsm_counts[2] = vc.surface_idx;
        } else {
            gs_vsm_counts[0] = 0u;
            gs_vsm_counts[1] = 0u;
            gs_vsm_counts[2] = 0u;
        }
    }

    GroupMemoryBarrierWithGroupSync();

    uint vert_count = gs_vsm_counts[0];
    uint tri_count = gs_vsm_counts[1];
    uint surf_idx = gs_vsm_counts[2];
    SetMeshOutputCounts(vert_count, tri_count);
    if (vert_count == 0u && tri_count == 0u)
        return;

    GpuVsmPageJob job = gs_vsm_job;
    GpuInstanceRecord inst = gs_vsm_inst;
    GpuMeshDescriptor mesh = gs_vsm_mesh;
    GpuMeshlet ml = gs_vsm_ml;

    uint px = job.phys_page & 31u;
    uint py = (job.phys_page >> 5u) & 31u;
    float ax_bias = (2.0f * float(px) + 1.0f) * (1.0f / 32.0f) - 1.0f;
    float ay_bias = 1.0f - (2.0f * float(py) + 1.0f) * (1.0f / 32.0f);

    if (gtid < vert_count) {
        StructuredBuffer<GpuPackedPosition> pos_stream = ResourceDescriptorHeap[atelier_safe_desc(mesh.positions_srv)];
        StructuredBuffer<uint> ml_verts = ResourceDescriptorHeap[atelier_safe_desc(mesh.meshlet_vertices_srv)];
        uint gv = ml_verts[ml.vertex_offset + gtid];
        float3 lpos = pos_stream[gv].pos;
        gs_vsm_local_pos[gtid] = lpos;

        precise float4 wpos = mul(float4(lpos, 1.0f), inst.world);
        precise float4 page_cpos = mul(wpos, job.page_view_proj);
        // Note: Near-plane pancaking is performed per-pixel by hardware `depth_clamp = true` (`DepthClipEnable = FALSE`)
        // so triangles straddling `z_near` preserve exact unwarped plane equations across the frustum!
        gs_vsm_page_clip[gtid] = page_cpos;

        VsmVertex v = (VsmVertex)0;
        v.page_clip = float4(page_cpos.x + page_cpos.w, page_cpos.w - page_cpos.x,
                             page_cpos.y + page_cpos.w, page_cpos.w - page_cpos.y);
        v.position = float4(page_cpos.x * (1.0f / 32.0f) + page_cpos.w * ax_bias,
                            page_cpos.y * (1.0f / 32.0f) + page_cpos.w * ay_bias,
                            page_cpos.z, page_cpos.w);
#if VSM_MASKED
        StructuredBuffer<GpuPackedAttributes> attr_stream = ResourceDescriptorHeap[atelier_safe_desc(mesh.attributes_srv)];
        GpuPackedAttributes attr = attr_stream[gv];
        float3 lnorm = unpack_oct_snorm16(attr.oct_normal);
        gs_vsm_local_norm[gtid] = lnorm;
        v.uv = attr.uv;
        v.world_pos = wpos.xyz;
        v.world_normal = transform_local_normal(lnorm, inst);
        v.world_tangent = transform_local_tangent(unpack_oct_tangent_sign(attr.oct_tangent_sign), inst);
#endif
        out_verts[gtid] = v;
    }

    GroupMemoryBarrierWithGroupSync();

    if (gtid < tri_count) {
        ByteAddressBuffer ml_tris = ResourceDescriptorHeap[atelier_safe_desc(mesh.meshlet_triangles_srv)];
        uint byte_off = ml.triangle_offset + gtid * 3u;
        uint aligned = byte_off & ~3u;
        uint2 raw2 = ml_tris.Load2(aligned);
        uint sh = (byte_off & 3u) * 8u;
        uint packed3 = (sh == 0u) ? raw2.x : ((raw2.x >> sh) | (raw2.y << (32u - sh)));
        uint i0 = packed3 & 0xFFu;
        uint i1 = (packed3 >> 8u) & 0xFFu;
        uint i2 = (packed3 >> 16u) & 0xFFu;

        float4 c0 = gs_vsm_page_clip[i0];
        float4 c1 = gs_vsm_page_clip[i1];
        float4 c2 = gs_vsm_page_clip[i2];
        bool culled = (c0.w <= 0.0f && c1.w <= 0.0f && c2.w <= 0.0f) ||
                      (c0.x < -c0.w && c1.x < -c1.w && c2.x < -c2.w) ||
                      (c0.x > c0.w && c1.x > c1.w && c2.x > c2.w) ||
                      (c0.y < -c0.w && c1.y < -c1.w && c2.y < -c2.w) ||
                      (c0.y > c0.w && c1.y > c1.w && c2.y > c2.w) ||
                      (c0.z < 0.0f && c1.z < 0.0f && c2.z < 0.0f);

        VsmPrimitive prim = (VsmPrimitive)0;
        prim.cull = culled;
#if VSM_MASKED
        StructuredBuffer<uint> face_meta_buf = ResourceDescriptorHeap[atelier_safe_desc(mesh.face_meta_srv)];
        bool winding_flipped = (face_meta_buf[ml.global_tri_base + gtid] >> 31u) != 0u;
        prim.surface_idx = surf_idx;
        prim.geom_normal = reconstruct_geom_normal(gs_vsm_local_pos[i0], gs_vsm_local_pos[i1], gs_vsm_local_pos[i2],
                                                   gs_vsm_local_norm[i0], winding_flipped, inst);
#endif
        out_prims[gtid] = prim;
        out_Triangles[gtid] = uint3(i0, i1, i2);
    }
}
