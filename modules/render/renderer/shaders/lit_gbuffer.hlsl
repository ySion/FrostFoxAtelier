// The shared indexed mesh: welded vertices read by SV_VertexID, one record per
// triangle at lit_first_triangle plus the draw's SV_PrimitiveID.
StructuredBuffer<LitVertex> lit_vertices;
StructuredBuffer<LitFace> lit_faces;
StructuredBuffer<uint> lit_triangle_indices;
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
    float3 world_position:TEXCOORD6;
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
    o.world_position=mul(float4(v.position,1),i.world).xyz;
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
LitGBuffer lit_gbuffer_ps(LitRaster i,uint primitive:SV_PrimitiveID,float3 barycentrics:SV_Barycentrics) {
    float2 bx=ddx(barycentrics.yz),by=ddy(barycentrics.yz);
    LitRayTriangle corners=(LitRayTriangle)0;
    corners.face=lit_faces[lit_first_triangle+primitive];
    [unroll] for(uint corner=0;corner<3;++corner)
        corners.vertices[corner]=lit_vertices[lit_triangle_indices[(lit_first_triangle+primitive)*3+corner]];
    MaterialTriangle tri=lit_material_triangle(i.instance,corners);
    MaterialContext center=material_triangle_context(tri,barycentrics.yz);
    MaterialContext dx=material_triangle_delta(tri,barycentrics.yz,bx);
    MaterialContext dy=material_triangle_delta(tri,barycentrics.yz,by);
    LitFace face=lit_faces[lit_first_triangle+primitive];
    LitInstance instance=lit_instances[i.instance];
    MaterialSurface surface=material_surfaces[material_indices[material_offsets[i.instance]+lit_face_slot(face)]];
    float3 face_normal=face.normal.x*instance.normal_x.xyz+face.normal.y*instance.normal_y.xyz+face.normal.z*instance.normal_z.xyz;
    float3 view_direction=lit_view_direction(i.position.xy/lit_size_frame.xy);
    if(surface.flags.y==0&&dot(face_normal,view_direction)<=0)discard;
    MaterialOutput material=material_shade_footprint(surface,center,dx,dy);
    if(surface.flags.x!=0&&!(material.ms_coverage>=0))discard;
    if(surface.flags.z==0&&dot(face_normal,view_direction)<=0)material.ms_emissive=0;
    float3 normal=lit_safe_normal(material.ms_normal_world);
    float3 geometric_normal=lit_safe_normal(face_normal);
    if(surface.flags.y!=0&&dot(geometric_normal,view_direction)<0) {geometric_normal=-geometric_normal;normal=-normal;}
    LitGBuffer o;o.identity=i.identity;o.geometry_normal=lit_encode_normal(geometric_normal);o.normal=float4(normal,saturate(material.ms_roughness));
    o.base=float4(material.ms_base_color,saturate(material.ms_metallic));o.emissive=float4(max(material.ms_emissive,0),1);
    float2 current=i.position.xy/lit_size_frame.xy;
    o.motion=float4(lit_window_uv(i.previous)-current,1,i.valid!=0&&i.previous.w>0?1:0);
    o.previous_depth=i.previous.w>0?i.previous.z/i.previous.w:lit_depth_far_value();
    return o;
}
