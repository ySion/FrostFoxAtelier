// Atlas depth uses the same immutable geometry, instance and material tables
// as GBuffer/RT. Shadow coverage is two-sided and samples Mask at LOD0, matching
// lit_accept_candidate rather than camera-facing GBuffer material rejection.
StructuredBuffer<uint> lit_shadow_instances;
uint lit_shadow_first_instance;
row_major float4x4 lit_shadow_view_projection;
struct LitShadowRaster {
    precise float4 position:SV_Position;
    float2 uv:TEXCOORD0;
    nointerpolation uint material_index:TEXCOORD1;
};
LitShadowRaster lit_shadow_vs(uint vertex:SV_VertexID,uint instance:SV_InstanceID) {
    uint index=lit_shadow_instances[lit_shadow_first_instance+instance];
    LitTriangle face=lit_triangles[vertex/3];LitVertex v=face.vertices[vertex%3];
    LitShadowRaster result;
    result.position=raster_position(v.position,lit_instances[index].world,lit_shadow_view_projection);
    result.uv=v.uv;result.material_index=material_indices[material_offsets[index]+face.material_slot];
    return result;
}
void lit_shadow_ps(LitShadowRaster input) {
    if(!material_covered(material_surfaces[input.material_index],input.uv,0,0,0,false))discard;
}
