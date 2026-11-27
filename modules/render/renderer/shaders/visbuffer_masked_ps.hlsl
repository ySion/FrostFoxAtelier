// Masked Coverage Pixel Shader for VisBuffer & Software-Paged D32_FLOAT VSM (per unique Masked `.source` archetype)

#ifndef VSM_MASKED
#define VSM_MASKED 0
#endif

#if VSM_MASKED
struct VsmVertex {
    precise float4 position : SV_Position;
    float4 page_clip : SV_ClipDistance0;
    float2 uv : TEXCOORD0;
    float3 world_pos : TEXCOORD1;
    float3 world_normal : TEXCOORD2;
    float4 world_tangent : TEXCOORD3;
};

struct VsmPrimitive {
    bool cull : SV_CullPrimitive;
    uint surface_idx : SURF_ID;
    float3 geom_normal : GEOM_N;
};

void vsm_ps_main(VsmVertex vtx, VsmPrimitive prim) {
    StructuredBuffer<GpuSurfaceRecord> surfaces = ResourceDescriptorHeap[atelier_safe_desc(g_material_surfaces_srv)];
    ByteAddressBuffer material_parameters = ResourceDescriptorHeap[atelier_safe_desc(g_material_params_srv)];
    GpuSurfaceRecord surf = surfaces[prim.surface_idx];

    ArchCov::ms_Context ctx;
    ctx.ms_uv = vtx.uv;
    ctx.ms_position_world = vtx.world_pos;
    ctx.ms_normal_world = safe_unit_normal(vtx.world_normal);
    ctx.ms_geometric_normal_world = safe_unit_normal(prim.geom_normal);
    ctx.ms_tangent_world = float4(vtx.world_tangent.xyz, vtx.world_tangent.w < 0.0f ? -1.0f : 1.0f);

    float cov = ArchCov::ms_coverage(surf.input_base, ctx, ArchCov::ms_load_parameters(material_parameters, surf.parameter_base));
    if (!(cov >= 0.0f))
        discard;
}
#else
struct VisVertex {
    precise float4 position : SV_Position;
    float far_clip : SV_ClipDistance0;
    float2 uv : TEXCOORD0;
    float3 world_pos : TEXCOORD1;
    float3 world_normal : TEXCOORD2;
    float4 world_tangent : TEXCOORD3;
};

struct VisPrimitive {
    bool cull : SV_CullPrimitive;
    uint vis_id : VIS_ID;
    uint surface_idx : SURF_ID;
    float3 geom_normal : GEOM_N;
};

uint vis_ps_main(VisVertex vtx, VisPrimitive prim) : SV_Target0 {
    StructuredBuffer<GpuSurfaceRecord> surfaces = ResourceDescriptorHeap[atelier_safe_desc(g_material_surfaces_srv)];
    ByteAddressBuffer material_parameters = ResourceDescriptorHeap[atelier_safe_desc(g_material_params_srv)];
    GpuSurfaceRecord surf = surfaces[prim.surface_idx];

    ArchCov::ms_Context ctx;
    ctx.ms_uv = vtx.uv;
    ctx.ms_position_world = vtx.world_pos;
    ctx.ms_normal_world = safe_unit_normal(vtx.world_normal);
    ctx.ms_geometric_normal_world = safe_unit_normal(prim.geom_normal);
    ctx.ms_tangent_world = float4(vtx.world_tangent.xyz, vtx.world_tangent.w < 0.0f ? -1.0f : 1.0f);

    float cov = ArchCov::ms_coverage(surf.input_base, ctx, ArchCov::ms_load_parameters(material_parameters, surf.parameter_base));
    if (!(cov >= 0.0f))
        discard;
    return prim.vis_id;
}
#endif
