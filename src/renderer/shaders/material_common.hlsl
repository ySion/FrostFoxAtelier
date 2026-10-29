#ifndef MATERIAL_PROGRAMS
// Non-material entry points share these helpers but cannot shade a surface.
// There is deliberately no implementation of either shade function without a
// batch's generated program prefix. Any reachable call requires that prefix.
struct MaterialContext {
    float2 ms_uv;
    float3 ms_position_world,ms_normal_world,ms_geometric_normal_world;
    float4 ms_tangent_world;
};
struct MaterialOutput {
    float3 ms_base_color,ms_emissive;
    float ms_roughness,ms_metallic,ms_coverage;
    float3 ms_normal_world;
};
struct MaterialSurface { uint program; uint parameter_base; uint input_base; uint3 flags; };
StructuredBuffer<MaterialSurface> material_surfaces;
StructuredBuffer<uint> material_indices;
StructuredBuffer<uint> material_offsets;
MaterialOutput material_shade(MaterialSurface surface,MaterialContext input);
MaterialOutput material_shade_footprint(MaterialSurface surface,MaterialContext input,MaterialContext dx,MaterialContext dy);
bool material_finite2(float2 value) { return all((asuint(value)&0x7fffffffu)<0x7f800000u); }
#endif
float3 material_transform_tangent(float3 tangent,row_major float4x4 world) {
    float3x3 basis=(float3x3)world;
    float3 largest=max(abs(basis[0]),max(abs(basis[1]),abs(basis[2])));
    float scale=max(largest.x,max(largest.y,largest.z));
    return scale>0?mul(tangent,basis/scale):0;
}
float3 material_unit_normal(float3 value) {
    float scale=max(abs(value.x),max(abs(value.y),abs(value.z)));
    return scale>0?normalize(value/scale):0;
}
MaterialContext material_context(float2 uv,float3 position,float3 normal,float3 geometric_normal,float4 tangent) {
    MaterialContext context;
    context.ms_uv=uv;context.ms_position_world=position;
    context.ms_normal_world=material_unit_normal(normal);
    context.ms_geometric_normal_world=material_unit_normal(geometric_normal);
    context.ms_tangent_world=float4(tangent.xyz,tangent.w<0?-1:1);
    return context;
}
bool material_finite3(float3 value) { return all((asuint(value)&0x7fffffffu)<0x7f800000u); }
float material_unavailable() { return asfloat(0x7fc00000u); }
struct MaterialTriangle {
    float3 position[3],normal[3];
    float2 uv[3];
    float4 tangent[3];
    float3 geometric_normal;
};
MaterialContext material_triangle_context(MaterialTriangle t,float2 bary) {
    float3 w=float3(1-bary.x-bary.y,bary);
    return material_context(t.uv[0]*w.x+t.uv[1]*w.y+t.uv[2]*w.z,
        t.position[0]+(t.position[1]-t.position[0])*bary.x+(t.position[2]-t.position[0])*bary.y,
        t.normal[0]*w.x+t.normal[1]*w.y+t.normal[2]*w.z,t.geometric_normal,
        t.tangent[0]*w.x+t.tangent[1]*w.y+t.tangent[2]*w.z);
}
float3 material_normal_delta(float3 value,float3 delta) {
    if(!material_finite3(value)||!material_finite3(delta))return material_unavailable();
    float scale=max(abs(value.x),max(abs(value.y),abs(value.z)));
    if(scale==0)return all(delta==0)?float3(0,0,0):material_unavailable().xxx;
    precise float3 v=value/scale,d=delta/scale;
    precise float magnitude=length(v);
    precise float3 n=v/magnitude;
    precise float3 result=(d-n*dot(n,d))/magnitude;
    return result;
}
MaterialContext material_triangle_delta(MaterialTriangle t,float2 bary,float2 db) {
    // Derivatives are zero-initialized changes, NOT neighboring Context values.
    // Separate fields keep an unusable normal from poisoning independent UVs.
    MaterialContext d=(MaterialContext)0;
    if(!material_finite2(db)) {
        d.ms_uv=material_unavailable();d.ms_position_world=material_unavailable();
        d.ms_normal_world=material_unavailable();d.ms_tangent_world=float4(material_unavailable().xxx,0);
        return d;
    }
    precise float2 uv=(t.uv[1]-t.uv[0])*db.x+(t.uv[2]-t.uv[0])*db.y;
    precise float3 position=(t.position[1]-t.position[0])*db.x+(t.position[2]-t.position[0])*db.y;
    precise float3 normal=(t.normal[1]-t.normal[0])*db.x+(t.normal[2]-t.normal[0])*db.y;
    precise float3 tangent=(t.tangent[1].xyz-t.tangent[0].xyz)*db.x+(t.tangent[2].xyz-t.tangent[0].xyz)*db.y;
    d.ms_uv=uv;d.ms_position_world=position;
    d.ms_normal_world=material_normal_delta(t.normal[0]*(1-bary.x-bary.y)+t.normal[1]*bary.x+t.normal[2]*bary.y,normal);
    d.ms_tangent_world=float4(tangent,0); // Neither XYZ normalization nor handedness variation.
    return d;
}
void material_ray_barycentrics(MaterialTriangle t,float3 direction,float rho,out float2 d0,out float2 d1) {
    d0=d1=material_unavailable();
    precise float3 edge1=t.position[1]-t.position[0],edge2=t.position[2]-t.position[0];
    if(!material_finite3(t.position[0])||!material_finite3(edge1)||!material_finite3(edge2)||!material_finite3(direction))return;
    float3 extent=max(abs(edge1),abs(edge2));
    float scale=max(extent.x,max(extent.y,extent.z));
    if(scale==0)return;
    precise float3 e1=edge1/scale,e2=edge2/scale,area=cross(e1,e2);
    precise float area2=dot(area,area);
    if(!(area2>0))return; // Degenerate world triangle; a degenerate UV chart is unrelated.
    float3 n=material_unit_normal(area);
    if(rho==0) {d0=d1=0;return;}
    precise float cosine=dot(n,direction);
    if(cosine==0)return;
    // Double cross stays in the plane even when subtraction of nearly parallel
    // unit vectors would leave only a rounding residual along the normal.
    precise float3 projected=cross(n,cross(direction,n));
    // At normal incidence the longest world edge supplies a deterministic axis.
    if(all(projected==0)) {
        float3 e3=e2-e1;
        projected=dot(e1,e1)>=dot(e2,e2)?e1:e2;
        if(dot(e3,e3)>dot(projected,projected))projected=e3;
    }
    float3 axis0=material_unit_normal(projected),axis1=material_unit_normal(cross(n,axis0));
    precise float3 p0=axis0*(rho/abs(cosine)),p1=axis1*rho;
    // No grazing clamp. Overflow remains unavailable and reaches the coarsest mip.
    precise float3 reciprocal0=cross(e2,area)/area2,reciprocal1=cross(area,e1)/area2;
    precise float2 b0=float2(dot(p0/scale,reciprocal0),dot(p0/scale,reciprocal1));
    precise float2 b1=float2(dot(p1/scale,reciprocal0),dot(p1/scale,reciprocal1));
    if(material_finite3(p0)&&material_finite2(b0))d0=b0;
    if(material_finite3(p1)&&material_finite2(b1))d1=b1;
}
