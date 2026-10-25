// The shared indexed mesh: welded vertices read by SV_VertexID, one record per
// triangle at lit_first_triangle plus the draw's SV_PrimitiveID.
StructuredBuffer<LitVertex> lit_vertices;
StructuredBuffer<LitFace> lit_faces;
uint lit_first_triangle;
StructuredBuffer<uint> lit_draw_instances;
uint lit_first_instance;
struct LitRaster {
    precise float4 position:SV_Position;
    float far_clip:SV_ClipDistance;
    float3 normal:NORMAL;
    float4 previous:TEXCOORD0;
    float2 uv:TEXCOORD1;
    nointerpolation uint instance:TEXCOORD2;
    nointerpolation uint valid:TEXCOORD3;
    nointerpolation uint2 identity:TEXCOORD5;
    float4 tangent:TEXCOORD4;
};
LitRaster lit_gbuffer_vs(uint vertex:SV_VertexID,uint instance:SV_InstanceID) {
    LitVertex v=lit_vertices[vertex];
    uint index=lit_draw_instances[lit_first_instance+instance];
    LitInstance i=lit_instances[index]; LitRaster o;
    o.position=raster_position(v.position,i.world,lit_view_projection);
    o.far_clip=o.position.z-lit_far_depth*o.position.w;
    o.previous=raster_position(v.position,i.previous_world,lit_previous_view_projection);
    o.normal=v.normal.x*i.normal_x.xyz+v.normal.y*i.normal_y.xyz+v.normal.z*i.normal_z.xyz;
    o.uv=v.uv;
    o.tangent=float4(material_transform_tangent(v.tangent.xyz,i.world),v.tangent.w*i.normal_x.w);
    o.instance=index;o.valid=i.geometry.z;o.identity=i.geometry.yw;
    o.position=raster_viewport_position(o.position);
    return o;
}
struct LitGBuffer {
    float4 normal:SV_Target0;
    float4 base:SV_Target1;
    float4 emissive:SV_Target2;
    float4 motion:SV_Target3;
    float previous_depth:SV_Target4;
    float2 geometry_normal:SV_Target5;
    uint2 identity:SV_Target6;
};
LitGBuffer lit_gbuffer_ps(LitRaster i,uint primitive:SV_PrimitiveID) {
    LitFace face=lit_faces[lit_first_triangle+primitive];
    LitInstance instance=lit_instances[i.instance];
    MaterialSurface surface=material_surfaces[material_indices[material_offsets[i.instance]+lit_face_slot(face)]];
    float3 face_normal=face.normal.x*instance.normal_x.xyz+face.normal.y*instance.normal_y.xyz+face.normal.z*instance.normal_z.xyz;
    float3 view_direction=lit_view_direction(i.position.xy/lit_size_frame.xy);
    if(surface.flags.y==0&&dot(face_normal,view_direction)<=0)discard;
    float2 dx=ddx(i.uv),dy=ddy(i.uv);
    if(!material_covered(surface,i.uv,dx,dy,0,true))discard;
    MaterialSample material=material_evaluate(surface,i.uv,dx,dy,0,true);
    if(surface.flags.z==0&&dot(face_normal,view_direction)<=0)material.emissive=0;
    float3 normal=lit_safe_normal(i.normal);
    normal=material_normal(surface,i.uv,dx,dy,0,true,normal,i.tangent,material.roughness);
    float3 geometric_normal=lit_safe_normal(face_normal);
    if(surface.flags.y!=0&&dot(geometric_normal,view_direction)<0) {geometric_normal=-geometric_normal;normal=-normal;}
    LitGBuffer o;o.identity=i.identity;o.geometry_normal=lit_encode_normal(geometric_normal);o.normal=float4(normal,material.roughness);
    o.base=float4(material.base,material.metallic); o.emissive=float4(material.emissive,1);
    float2 current=i.position.xy/lit_size_frame.xy;
    o.motion=float4(lit_window_uv(i.previous)-current,1,i.valid!=0&&i.previous.w>0?1:0);
    o.previous_depth=i.previous.w>0?i.previous.z/i.previous.w:lit_depth_far_value();
    return o;
}
