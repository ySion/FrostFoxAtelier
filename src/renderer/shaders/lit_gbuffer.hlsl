struct LitTriangle { LitVertex vertices[3]; uint edge_mask; float3 face_normal; uint material_slot; uint3 reserved; };
StructuredBuffer<LitTriangle> lit_triangles;
uint lit_first_instance;
struct LitRaster {
    float4 position:SV_Position;
    float3 normal:NORMAL;
    float4 previous:TEXCOORD0;
    float2 uv:TEXCOORD1;
    nointerpolation uint material_index:TEXCOORD2;
    nointerpolation uint valid:TEXCOORD3;
    float4 tangent:TEXCOORD4;
    nointerpolation float3 geometric_normal:TEXCOORD6;
};
LitRaster lit_gbuffer_vs(uint vertex:SV_VertexID,uint instance:SV_InstanceID) {
    LitVertex v=lit_triangles[vertex/3].vertices[vertex%3];
    LitInstance i=lit_instances[lit_first_instance+instance]; LitRaster o;
    o.position=mul(mul(float4(v.position,1),i.world),lit_view_projection);
    o.previous=mul(mul(float4(v.position,1),i.previous_world),lit_previous_view_projection);
    o.normal=v.normal.x*i.normal_x.xyz+v.normal.y*i.normal_y.xyz+v.normal.z*i.normal_z.xyz;
    o.uv=v.uv;
    o.tangent=float4(material_transform_tangent(v.tangent.xyz,i.world),v.tangent.w*i.normal_x.w);
    float3 gn=lit_triangles[vertex/3].face_normal;
    o.geometric_normal=gn.x*i.normal_x.xyz+gn.y*i.normal_y.xyz+gn.z*i.normal_z.xyz;
    o.material_index=material_indices[material_offsets[lit_first_instance+instance]+lit_triangles[vertex/3].material_slot];
    o.valid=i.geometry.z;
    return o;
}
struct LitGBuffer {
    float4 normal:SV_Target0;
    float4 base:SV_Target1;
    float4 emissive:SV_Target2;
    float4 motion:SV_Target3;
    float previous_depth:SV_Target4;
    float2 geometry_normal:SV_Target5;
};
LitGBuffer lit_gbuffer_ps(LitRaster i) {
    MaterialSurface surface=material_surfaces[i.material_index];
    float3 view_direction=lit_view_direction(i.position.xy/lit_size_frame.xy);
    if(surface.flags.y==0&&dot(i.geometric_normal,view_direction)<=0)discard;
    float2 dx=ddx(i.uv),dy=ddy(i.uv);
    if(!material_covered(surface,i.uv,dx,dy,0,true))discard;
    MaterialSample material=material_evaluate(surface,i.uv,dx,dy,0,true);
    if(surface.flags.z==0&&dot(i.geometric_normal,view_direction)<=0)material.emissive=0;
    float3 normal=lit_safe_normal(i.normal);
    normal=material_normal(surface,i.uv,dx,dy,0,true,normal,i.tangent,material.roughness);
    float3 geometric_normal=lit_safe_normal(i.geometric_normal);
    if(surface.flags.y!=0&&dot(geometric_normal,view_direction)<0) {geometric_normal=-geometric_normal;normal=-normal;}
    LitGBuffer o;o.geometry_normal=lit_encode_normal(geometric_normal);o.normal=float4(normal,material.roughness);
    o.base=float4(material.base,material.metallic); o.emissive=float4(material.emissive,1);
    float2 current=i.position.xy/lit_size_frame.xy;
    o.motion=float4(lit_window_uv(i.previous)-current,1,i.valid!=0&&i.previous.w>0?1:0);
    o.previous_depth=i.previous.w>0?i.previous.z/i.previous.w:lit_depth_far_value();
    return o;
}
