struct LitTriangle { LitVertex vertices[3]; uint edge_mask; float3 face_normal; };
StructuredBuffer<LitTriangle> lit_triangles;
uint lit_first_instance;
struct LitRaster {
    float4 position:SV_Position;
    float3 normal:NORMAL;
    float4 previous:TEXCOORD0;
    nointerpolation float4 base_roughness:TEXCOORD1;
    nointerpolation float4 emissive_metallic:TEXCOORD2;
    nointerpolation uint valid:TEXCOORD3;
};
LitRaster lit_gbuffer_vs(uint vertex:SV_VertexID,uint instance:SV_InstanceID) {
    LitVertex v=lit_triangles[vertex/3].vertices[vertex%3];
    LitInstance i=lit_instances[lit_first_instance+instance]; LitRaster o;
    o.position=mul(mul(float4(v.position,1),i.world),lit_view_projection);
    o.previous=mul(mul(float4(v.position,1),i.previous_world),lit_previous_view_projection);
    o.normal=v.normal.x*i.normal_x.xyz+v.normal.y*i.normal_y.xyz+v.normal.z*i.normal_z.xyz;
    o.base_roughness=i.base_roughness; o.emissive_metallic=i.emissive_metallic; o.valid=i.geometry.z;
    return o;
}
struct LitGBuffer { float4 normal:SV_Target0; float4 base:SV_Target1; float4 emissive:SV_Target2; float4 motion:SV_Target3; };
LitGBuffer lit_gbuffer_ps(LitRaster i) {
    LitGBuffer o; o.normal=float4(lit_safe_normal(i.normal),i.base_roughness.w);
    o.base=float4(i.base_roughness.rgb,i.emissive_metallic.w); o.emissive=float4(i.emissive_metallic.rgb,1);
    float2 current=i.position.xy/lit_size_frame.xy;
    o.motion=float4(lit_window_uv(i.previous)-current,i.previous.z/max(i.previous.w,1e-8)-i.position.z,i.valid!=0&&i.previous.w>0?1:0);
    return o;
}
