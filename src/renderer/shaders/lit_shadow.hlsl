// Atlas depth uses the same immutable geometry, instance and material tables
// as GBuffer/RT. Shadow coverage is two-sided and samples Mask at LOD0, matching
// lit_accept_candidate rather than camera-facing GBuffer material rejection.
StructuredBuffer<uint> lit_shadow_instances;
uint lit_shadow_first_instance;
row_major float4x4 lit_shadow_view_projection;
struct LitShadowRaster {
    precise float4 position:SV_Position;
    float2 uv:TEXCOORD0;
    nointerpolation uint instance:TEXCOORD1;
};
LitShadowRaster lit_shadow_vs(uint vertex:SV_VertexID,uint instance:SV_InstanceID) {
    uint index=lit_shadow_instances[lit_shadow_first_instance+instance];
    LitVertex v=lit_vertices[vertex];
    LitShadowRaster result;
    result.position=raster_position(v.position,lit_instances[index].world,lit_shadow_view_projection);
    result.uv=v.uv;result.instance=index;
    return result;
}
void lit_shadow_ps(LitShadowRaster input,uint primitive:SV_PrimitiveID) {
    uint slot=lit_face_slot(lit_faces[lit_first_triangle+primitive]);
    if(!material_covered(material_surfaces[material_indices[material_offsets[input.instance]+slot]],input.uv,0,0,0,false))discard;
}
