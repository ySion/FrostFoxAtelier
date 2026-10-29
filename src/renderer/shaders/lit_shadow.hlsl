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
    nointerpolation uint first_triangle:TEXCOORD2;
    float3 world_position:TEXCOORD3;
    float3 normal:TEXCOORD4;
    float4 tangent:TEXCOORD5;
};
LitShadowRaster lit_shadow_vs(uint vertex:SV_VertexID,uint instance:SV_InstanceID) {
    uint index=lit_shadow_instances[lit_shadow_first_instance+instance];
    LitVertex v=lit_vertices[vertex];
    LitShadowRaster result;
    result.position=raster_position(v.position,lit_instances[index].world,lit_shadow_view_projection);
    result.uv=v.uv;result.instance=index;
    LitInstance object=lit_instances[index];
    result.world_position=mul(float4(v.position,1),object.world).xyz;
    result.normal=v.normal.x*object.normal_x.xyz+v.normal.y*object.normal_y.xyz+v.normal.z*object.normal_z.xyz;
    result.tangent=float4(material_transform_tangent(v.tangent.xyz,object.world),v.tangent.w*object.normal_x.w);
    // VS already consumes draw constants. Avoid making the PS upload the entire
    // shared $Globals block again just to read this one draw-local offset.
    result.first_triangle=lit_first_triangle;
    return result;
}
void lit_shadow_ps(LitShadowRaster input,uint primitive:SV_PrimitiveID) {
    uint slot=lit_face_slot(lit_faces[input.first_triangle+primitive]);
    MaterialSurface surface=material_surfaces[material_indices[material_offsets[input.instance]+slot]];
    if(surface.flags.x==0)return;
    LitInstance object=lit_instances[input.instance];
    float3 n=lit_faces[input.first_triangle+primitive].normal;
    float3 gn=n.x*object.normal_x.xyz+n.y*object.normal_y.xyz+n.z*object.normal_z.xyz;
    MaterialOutput result=material_shade(surface,material_context(input.uv,input.world_position,input.normal,gn,input.tangent));
    if(!(result.ms_coverage>=0))discard;
}
