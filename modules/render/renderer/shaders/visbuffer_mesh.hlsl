// Unified Mesh Shader for Opaque VisBuffer & Masked VisBuffer (Phase 1 and Phase 2 Disocclusion)

#ifndef VIS_MASKED
#define VIS_MASKED 0
#endif

#if !VIS_MASKED
cbuffer ArchetypeRootConstants : register(b0, space0) {
    uint g_frame_cb_srv;
    uint g_frame_cb_idx;
    uint g_material_inputs_srv;
    uint g_material_params_srv;
    uint g_material_surfaces_srv;
    uint g_archetype_index;
    uint g_bin_offset_or_cluster_srv;
    uint g_aux0_srv;
    uint g_aux1_srv;
    uint g_aux2_uav;
    uint g_aux3_uav;
    uint g_aux4_uav;
    uint g_aux5_uav;
    uint g_pass_flags;
    uint g_viewport_packed0; // 0: Phase 1, 1: Phase 2 disoccluded delta
    uint g_viewport_packed1;
};
#endif

struct VisVertex {
    precise float4 position : SV_Position;
    float far_clip : SV_ClipDistance0;
#if VIS_MASKED
    float2 uv : TEXCOORD0;
    float3 world_pos : TEXCOORD1;
    float3 world_normal : TEXCOORD2;
    float4 world_tangent : TEXCOORD3;
#endif
};

struct VisPrimitive {
    bool cull : SV_CullPrimitive;
    uint vis_id : VIS_ID;
#if VIS_MASKED
    uint surface_idx : SURF_ID;
    float3 geom_normal : GEOM_N;
#endif
};

groupshared GpuInstanceRecord gs_inst;
groupshared GpuMeshDescriptor gs_mesh;
groupshared GpuMeshlet gs_ml;
groupshared GpuSurfaceRecord gs_surf;
groupshared uint gs_counts[4]; // [0]=vert_count, [1]=tri_count, [2]=encoded_cluster, [3]=surf_idx
groupshared float3 gs_local_pos[64];
groupshared float3 gs_local_norm[64];
groupshared float4 gs_clip_pos[64];

[outputtopology("triangle")]
[numthreads(128, 1, 1)]
void vis_ms_main(uint gtid : SV_GroupThreadID, uint3 gid : SV_GroupID,
                 out vertices VisVertex out_verts[64],
                 out primitives VisPrimitive out_prims[124],
                 out indices uint3 out_Triangles[124]) {
    StructuredBuffer<GlobalFrameConstants> fcb_buf = ResourceDescriptorHeap[atelier_safe_desc(g_frame_cb_srv)];
    GlobalFrameConstants fcb = fcb_buf[0];

    if (gtid == 0u) {
        StructuredBuffer<uint> counters = ResourceDescriptorHeap[atelier_safe_desc(g_aux0_srv)];
        uint group_linear = gid.y * 1024u + gid.x;
#if VIS_MASKED
        uint visible_count = counters[6u + g_archetype_index];
        uint base_local = (g_viewport_packed0 != 0u) ? counters[6u + fcb.masked_archetype_count + g_archetype_index] : 0u;
        uint cluster_base = g_archetype_index * g_pass_flags;
#else
        uint visible_count = counters[0u];
        uint base_local = (g_viewport_packed0 != 0u) ? counters[3u] : 0u;
        uint cluster_base = 0u;
#endif
        uint cluster_local = base_local + group_linear;
        if (cluster_local < visible_count) {
            StructuredBuffer<GpuVisibleCluster> clusters = ResourceDescriptorHeap[atelier_safe_desc(g_bin_offset_or_cluster_srv)];
            GpuVisibleCluster vc = clusters[cluster_base + cluster_local];

            StructuredBuffer<GpuInstanceRecord> instances = ResourceDescriptorHeap[atelier_safe_desc(fcb.instances_srv)];
            StructuredBuffer<GpuMeshDescriptor> meshes = ResourceDescriptorHeap[atelier_safe_desc(fcb.mesh_table_srv)];
            GpuInstanceRecord inst = instances[vc.instance_id];
            GpuMeshDescriptor mesh = meshes[inst.mesh_pool_index];

            StructuredBuffer<GpuMeshlet> meshlets = ResourceDescriptorHeap[atelier_safe_desc(mesh.meshlets_srv)];
            GpuMeshlet ml = meshlets[vc.meshlet_id];

            StructuredBuffer<uint> face_meta_buf = ResourceDescriptorHeap[atelier_safe_desc(mesh.face_meta_srv)];
            uint mat_slot = face_meta_buf[ml.global_tri_base] & 0x07FFFFFFu;
            StructuredBuffer<uint> slot_map = ResourceDescriptorHeap[atelier_safe_desc(fcb.material_slot_map_srv)];
            StructuredBuffer<GpuSurfaceRecord> surfaces = ResourceDescriptorHeap[atelier_safe_desc(fcb.material_surfaces_srv)];
            uint surf_idx = slot_map[inst.material_slot_offset + mat_slot];

            gs_inst = inst;
            gs_mesh = mesh;
            gs_ml = ml;
            gs_surf = surfaces[surf_idx];
            gs_counts[0] = ml.counts & 0xFFu;
            gs_counts[1] = (ml.counts >> 8u) & 0xFFu;
#if VIS_MASKED
            gs_counts[2] = (0x800000u | (cluster_base + cluster_local)) + 1u;
#else
            gs_counts[2] = (cluster_base + cluster_local) + 1u;
#endif
            gs_counts[3] = surf_idx;
        } else {
            gs_counts[0] = 0u;
            gs_counts[1] = 0u;
            gs_counts[2] = 0u;
            gs_counts[3] = 0u;
        }
    }

    GroupMemoryBarrierWithGroupSync();

    uint vert_count = gs_counts[0];
    uint tri_count = gs_counts[1];
    uint encoded_cluster = gs_counts[2];
    uint surf_idx = gs_counts[3];
    SetMeshOutputCounts(vert_count, tri_count);

    GpuInstanceRecord inst = gs_inst;
    GpuMeshDescriptor mesh = gs_mesh;
    GpuMeshlet ml = gs_ml;
    GpuSurfaceRecord surf = gs_surf;

    if (gtid < vert_count) {
        StructuredBuffer<GpuPackedPosition> pos_stream = ResourceDescriptorHeap[atelier_safe_desc(mesh.positions_srv)];
        StructuredBuffer<GpuPackedAttributes> attr_stream = ResourceDescriptorHeap[atelier_safe_desc(mesh.attributes_srv)];
        StructuredBuffer<uint> ml_verts = ResourceDescriptorHeap[atelier_safe_desc(mesh.meshlet_vertices_srv)];
        uint gv = ml_verts[ml.vertex_offset + gtid];
        float3 lpos = pos_stream[gv].pos;
        GpuPackedAttributes attr = attr_stream[gv];
        float3 lnorm = unpack_oct_snorm16(attr.oct_normal);

        gs_local_pos[gtid] = lpos;
        gs_local_norm[gtid] = lnorm;

        precise float4 wpos = mul(float4(lpos, 1.0f), inst.world);
        precise float4 cpos = mul(wpos, fcb.view_projection);
        VisVertex v = (VisVertex)0;
        v.far_clip = cpos.z - fcb.camera_far_depth * cpos.w;
        v.position = raster_viewport_position(cpos, fcb.raster_viewport_transform);
        gs_clip_pos[gtid] = cpos;
#if VIS_MASKED
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

        StructuredBuffer<uint> face_meta_buf = ResourceDescriptorHeap[atelier_safe_desc(mesh.face_meta_srv)];
        uint fmeta = face_meta_buf[ml.global_tri_base + gtid];
        bool winding_flipped = (fmeta >> 31u) != 0u;

        float3 p0 = gs_local_pos[i0];
        float3 p1 = gs_local_pos[i1];
        float3 p2 = gs_local_pos[i2];
        float3 gn_world = reconstruct_geom_normal(p0, p1, p2, gs_local_norm[i0], winding_flipped, inst);

        float3 w_p0 = mul(float4(p0, 1.0f), inst.world).xyz;
        float3 eye_pos = mul(float4(w_p0, 1.0f), fcb.view).xyz;
        float3 eye_gn = mul((float3x3)fcb.inv_view, gn_world);
        float3 eye_dir = (fcb.projection[2][3] != 0.0f) ? -eye_pos : float3(0.0f, 0.0f, -1.0f);
        bool backfacing = dot(eye_gn, eye_dir) <= 0.0f;
        bool double_sided = (surf.flags & 2u) != 0u;
        bool tint_backfaces = (fcb.flags & (1u << 9u)) != 0u && fcb.shading_mode != 3u;

        bool culled = (!double_sided && !tint_backfaces && backfacing);
        if (gs_clip_pos[i0].w <= 0.0f && gs_clip_pos[i1].w <= 0.0f && gs_clip_pos[i2].w <= 0.0f)
            culled = true;

        VisPrimitive prim = (VisPrimitive)0;
        prim.cull = culled;
        prim.vis_id = (encoded_cluster << 8u) | ((gtid & 0x7Fu) << 1u) | (backfacing ? 0u : 1u);
#if VIS_MASKED
        prim.surface_idx = surf_idx;
        prim.geom_normal = gn_world;
#endif
        out_prims[gtid] = prim;
        out_Triangles[gtid] = uint3(i0, i1, i2);
    }
}

#if !VIS_MASKED
[earlydepthstencil]
uint vis_opaque_ps_main(VisVertex vtx, VisPrimitive prim) : SV_Target0 {
    return prim.vis_id;
}
#endif
