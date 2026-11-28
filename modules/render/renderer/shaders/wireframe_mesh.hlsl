// Mesh Shader Wireframe / Selection Overlay + Viewport Linear-to-sRGB RTV Blit

#ifndef WIRE_BLIT_ONLY
#define WIRE_BLIT_ONLY 0
#endif

#if WIRE_BLIT_ONLY
cbuffer BlitRootConstants : register(b0, space0) {
    float4 g_raster_viewport_transform;
    uint g_src_color_srv;
    uint g_pad0;
};

float4 present_blit_ps(float4 screen : SV_Position) : SV_Target0 {
    Texture2D<float4> src = ResourceDescriptorHeap[atelier_safe_desc(g_src_color_srv)];
    float4 s = src.Load(int3(int2(screen.xy), 0));
    if (s.a <= 0.0f)
        discard;
    return float4(s.rgb, 1.0f);
}
#else
cbuffer WireRootConstants : register(b0, space0) {
    uint g_frame_cb_srv;
    uint g_opaque_clusters_srv;
    uint g_masked_clusters_srv;
    uint g_selected_clusters_srv;
    uint g_counters_srv;
    uint g_visbuffer_srv;
    uint g_wire_flags; // bit 0: use_unjittered_vp, bit 1: selected_only, bit 2: wire_only_mode, bit 3: check_masked_cutout, bit 4: xray_all_lines
    uint g_pad0;
};

struct WireVertex {
    precise float4 position : SV_Position;
    float far_clip : SV_ClipDistance0;
    float clip_w : TEXCOORD0;
};

struct WirePrimitive {
    bool cull : SV_CullPrimitive;
    uint edge_sel_inst : WIRE_META; // bits 0..2: edge_mask, bit 3: is_sel, bit 4: check_cutout, bits 8..31: instance_id
};

groupshared GpuInstanceRecord gs_w_inst;
groupshared GpuMeshDescriptor gs_w_mesh;
groupshared GpuMeshlet gs_w_ml;
groupshared GpuSurfaceRecord gs_w_surf;
groupshared uint gs_w_counts[4]; // [0]=vert_count, [1]=tri_count, [2]=is_selected, [3]=instance_id
groupshared float3 gs_w_lpos[64];
groupshared float3 gs_w_lnorm[64];
groupshared float4 gs_w_cpos[64];

float wire_edge_distance(float value, float dx, float dy) {
    float scale = max(abs(dx), abs(dy));
    if (scale == 0.0f)
        return 1e8f;
    float2 grad = float2(dx, dy) / scale;
    return abs(value / scale) * rsqrt(dot(grad, grad));
}

bool vis_matches_instance(uint vis, uint expected_inst) {
    if (vis == VISBUFFER_EMPTY)
        return false;
    uint cluster_idx = (vis >> 8u) - 1u;
    GpuVisibleCluster vc;
    if ((cluster_idx & 0x800000u) != 0u) {
        StructuredBuffer<GpuVisibleCluster> mq = ResourceDescriptorHeap[atelier_safe_desc(g_masked_clusters_srv)];
        vc = mq[cluster_idx & 0x7FFFFFu];
    } else {
        StructuredBuffer<GpuVisibleCluster> oq = ResourceDescriptorHeap[atelier_safe_desc(g_opaque_clusters_srv)];
        vc = oq[cluster_idx];
    }
    return vc.instance_id == expected_inst;
}

[outputtopology("triangle")]
[numthreads(128, 1, 1)]
void wire_ms_main(uint gtid : SV_GroupThreadID, uint3 gid : SV_GroupID,
                  out vertices WireVertex out_verts[64],
                  out primitives WirePrimitive out_prims[124],
                  out indices uint3 out_tris[124]) {
    StructuredBuffer<GlobalFrameConstants> fcb_buf = ResourceDescriptorHeap[atelier_safe_desc(g_frame_cb_srv)];
    GlobalFrameConstants fcb = fcb_buf[0];

    if (gtid == 0u) {
        StructuredBuffer<uint> counters = ResourceDescriptorHeap[atelier_safe_desc(g_counters_srv)];
        uint linear_id = gid.y * 1024u + gid.x;
        bool group_active = false;
        GpuVisibleCluster vc = (GpuVisibleCluster)0;
        bool is_sel = false;

        if ((g_wire_flags & 2u) != 0u) {
            // Dedicated Selected-Only queue (Slot 2 + M): launches ONLY selected clusters!
            uint sel_cnt = counters[2u];
            if (linear_id < sel_cnt && g_selected_clusters_srv != 0xFFFFFFFFu) {
                StructuredBuffer<GpuVisibleCluster> sq = ResourceDescriptorHeap[atelier_safe_desc(g_selected_clusters_srv)];
                vc = sq[linear_id];
                is_sel = true;
                group_active = true;
            }
        } else {
            uint opaque_cnt = counters[0u];
            uint total_cnt = opaque_cnt;
            [loop] for (uint m = 0u; m < fcb.masked_archetype_count; ++m)
                total_cnt += counters[6u + m];
            if (linear_id < total_cnt) {
                if (linear_id < opaque_cnt) {
                    StructuredBuffer<GpuVisibleCluster> oq = ResourceDescriptorHeap[atelier_safe_desc(g_opaque_clusters_srv)];
                    vc = oq[linear_id];
                } else {
                    uint rem = linear_id - opaque_cnt;
                    StructuredBuffer<GpuVisibleCluster> mq = ResourceDescriptorHeap[atelier_safe_desc(g_masked_clusters_srv)];
                    [loop] for (uint m2 = 0u; m2 < fcb.masked_archetype_count; ++m2) {
                        uint mc = counters[6u + m2];
                        if (rem < mc) {
                            vc = mq[m2 * fcb.masked_cluster_stride + rem];
                            break;
                        }
                        rem -= mc;
                    }
                }
                StructuredBuffer<uint> sel_mask = ResourceDescriptorHeap[atelier_safe_desc(fcb.selected_mask_srv)];
                is_sel = ((sel_mask[vc.instance_id >> 5u] >> (vc.instance_id & 31u)) & 1u) != 0u;
                group_active = true;
            }
        }

        if (group_active) {
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
            GpuSurfaceRecord surf = surfaces[slot_map[inst.material_slot_offset + mat_slot]];

            gs_w_inst = inst;
            gs_w_mesh = mesh;
            gs_w_ml = ml;
            gs_w_surf = surf;
            gs_w_counts[0] = ml.counts & 0xFFu;
            gs_w_counts[1] = (ml.counts >> 8u) & 0xFFu;
            gs_w_counts[2] = is_sel ? 1u : 0u;
            gs_w_counts[3] = vc.instance_id;
        } else {
            gs_w_counts[0] = 0u;
            gs_w_counts[1] = 0u;
            gs_w_counts[2] = 0u;
            gs_w_counts[3] = 0u;
        }
    }

    GroupMemoryBarrierWithGroupSync();

    uint vert_count = gs_w_counts[0];
    uint tri_count = gs_w_counts[1];
    bool is_selected = gs_w_counts[2] != 0u;
    uint inst_id = gs_w_counts[3];
    SetMeshOutputCounts(vert_count, tri_count);

    GpuInstanceRecord inst = gs_w_inst;
    GpuMeshDescriptor mesh = gs_w_mesh;
    GpuMeshlet ml = gs_w_ml;
    GpuSurfaceRecord surf = gs_w_surf;
    row_major float4x4 active_vp = ((g_wire_flags & 1u) != 0u) ? fcb.unjittered_view_projection : fcb.view_projection;

    if (gtid < vert_count) {
        StructuredBuffer<GpuPackedPosition> pos_s = ResourceDescriptorHeap[atelier_safe_desc(mesh.positions_srv)];
        StructuredBuffer<GpuPackedAttributes> attr_s = ResourceDescriptorHeap[atelier_safe_desc(mesh.attributes_srv)];
        StructuredBuffer<uint> ml_verts = ResourceDescriptorHeap[atelier_safe_desc(mesh.meshlet_vertices_srv)];
        uint gv = ml_verts[ml.vertex_offset + gtid];
        float3 lpos = pos_s[gv].pos;
        float3 lnorm = unpack_oct_snorm16(attr_s[gv].oct_normal);
        gs_w_lpos[gtid] = lpos;
        gs_w_lnorm[gtid] = lnorm;

        precise float4 wpos = mul(float4(lpos, 1.0f), inst.world);
        precise float4 cpos = mul(wpos, active_vp);
        gs_w_cpos[gtid] = cpos;

        WireVertex v;
        v.far_clip = cpos.z - fcb.camera_far_depth * cpos.w;
        v.clip_w = cpos.w;
        v.position = raster_viewport_position(cpos, fcb.raster_viewport_transform);
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
        uint edge_mask = ((fcb.flags & (1u << 10u)) != 0u) ? 7u : ((fmeta >> 27u) & 7u);

        float3 p0 = gs_w_lpos[i0], p1 = gs_w_lpos[i1], p2 = gs_w_lpos[i2];
        float3 gn_world = reconstruct_geom_normal(p0, p1, p2, gs_w_lnorm[i0], winding_flipped, inst);
        float3 w_p0 = mul(float4(p0, 1.0f), inst.world).xyz;
        float3 eye_pos = mul(float4(w_p0, 1.0f), fcb.view).xyz;
        float3 eye_gn = mul((float3x3)fcb.inv_view, gn_world);
        float3 eye_dir = (fcb.projection[2][3] != 0.0f) ? -eye_pos : float3(0.0f, 0.0f, -1.0f);
        bool backfacing = dot(eye_gn, eye_dir) <= 0.0f;
        bool double_sided = (surf.flags & 2u) != 0u;
        bool tint_bf = (fcb.flags & (1u << 9u)) != 0u && fcb.shading_mode != 3u;
        bool xray_all = (g_wire_flags & 16u) != 0u;

        bool culled = (!xray_all && !double_sided && !tint_bf && backfacing) || (edge_mask == 0u);
        if (gs_w_cpos[i0].w <= 0.0f && gs_w_cpos[i1].w <= 0.0f && gs_w_cpos[i2].w <= 0.0f)
            culled = true;

        bool check_cutout = ((surf.flags & 1u) != 0u) && ((g_wire_flags & 8u) != 0u) && !xray_all;

        WirePrimitive prim;
        prim.cull = culled;
        prim.edge_sel_inst = edge_mask | (is_selected ? 8u : 0u) | (check_cutout ? 16u : 0u) | ((inst_id & 0xFFFFFFu) << 8u);
        out_prims[gtid] = prim;
        out_tris[gtid] = uint3(i0, i1, i2);
    }
}

float4 wire_ps_main(WireVertex vtx, WirePrimitive prim, float3 barycentrics : SV_Barycentrics) : SV_Target0 {
    StructuredBuffer<GlobalFrameConstants> fcb_buf = ResourceDescriptorHeap[atelier_safe_desc(g_frame_cb_srv)];
    GlobalFrameConstants fcb = fcb_buf[0];

    if ((prim.edge_sel_inst & 16u) != 0u && g_visbuffer_srv != 0xFFFFFFFFu) {
        Texture2D<uint> vis_tex = ResourceDescriptorHeap[atelier_safe_desc(g_visbuffer_srv)];
        uint expected_inst = prim.edge_sel_inst >> 8u;
        int2 ipx = int2(vtx.position.xy);
        if ((g_wire_flags & 1u) == 0u) {
            if (!vis_matches_instance(vis_tex.Load(int3(ipx, 0)), expected_inst))
                discard;
        } else {
            bool any_match = false;
            int2 sc_max = max(fcb.scissor_rect.xy, fcb.scissor_rect.zw - 1);
            [unroll] for (int dy = -1; dy <= 1; ++dy) {
                [unroll] for (int dx = -1; dx <= 1; ++dx) {
                    int2 q = clamp(ipx + int2(dx, dy), fcb.scissor_rect.xy, sc_max);
                    if (vis_matches_instance(vis_tex.Load(int3(q, 0)), expected_inst))
                        any_match = true;
                }
            }
            if (!any_match)
                discard;
        }
    }

    float3 ef = barycentrics / max(abs(vtx.clip_w), 1e-6f);
    float3 dx = ddx(ef);
    float3 dy = ddy(ef);
    uint em = prim.edge_sel_inst & 7u;
    bool is_sel = (prim.edge_sel_inst & 8u) != 0u;

    float3 dist = float3((em & 1u) != 0u ? wire_edge_distance(ef.x, dx.x, dy.x) : 1e8f,
                         (em & 2u) != 0u ? wire_edge_distance(ef.y, dx.y, dy.y) : 1e8f,
                         (em & 4u) != 0u ? wire_edge_distance(ef.z, dx.z, dy.z) : 1e8f);

    float width = is_sel ? max(fcb.wire_style.x, 2.0f) : fcb.wire_style.x;
    float edge = 1.0f - smoothstep(max(0.0f, width * 0.5f - 0.5f), width * 0.5f + 0.5f, min(dist.x, min(dist.y, dist.z)));
    bool wire_only = (g_wire_flags & 4u) != 0u;
    float3 ink = is_sel ? float3(1.0f, 0.69f, 0.0f) : (wire_only ? float3(0.48f, 0.55f, 0.62f) : float3(0.025f, 0.032f, 0.04f));
    float alpha = edge * ((is_sel || wire_only) ? 1.0f : fcb.wire_style.y);
    if (alpha <= 0.0f)
        discard;
    return float4(ink * alpha, alpha);
}
#endif
