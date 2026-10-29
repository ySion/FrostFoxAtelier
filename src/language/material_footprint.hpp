#pragma once
#include <string_view>

namespace atelier::language::detail {
// Private material lowering, not an author-visible numeric/AD library. Checks
// inspect IEEE bits so /O3 fast-math cannot assume the unavailable values away.
// Every arithmetic intermediate that affects a change or the center is precise.
inline constexpr std::string_view material_footprint_scalar_hlsl = R"hlsl(
bool ms_finite(float x) { return (asuint(x) & 0x7f800000u) != 0x7f800000u; }
float ms_unavailable() { return asfloat(0x7fc00000u); }
float ms_checked(precise float x) { return ms_finite(x) ? x : ms_unavailable(); }
float ms_change_add(float a,float b) {
    if (!ms_finite(a) || !ms_finite(b)) return ms_unavailable();
    precise float r=a+b; return ms_checked(r);
}
float ms_change_sub(float a,float b) {
    if (!ms_finite(a) || !ms_finite(b)) return ms_unavailable();
    precise float r=a-b; return ms_checked(r);
}
float ms_change_scale(float a,float d) {
    if (!ms_finite(a) || !ms_finite(d)) return ms_unavailable();
    precise float r=a*d; return ms_checked(r);
}
float ms_change_product(float a,float b,float da,float db) {
    return ms_change_add(ms_change_scale(b,da),ms_change_scale(a,db));
}
float ms_change_quotient(float quotient,float b,float da,float db) {
    if (!ms_finite(quotient) || !ms_finite(b) || b==0.0 || !ms_finite(da) || !ms_finite(db))
        return ms_unavailable();
    precise float numerator=ms_change_sub(da,ms_change_scale(quotient,db));
    precise float r=numerator/b; return ms_checked(r);
}
float ms_change_root(float root,float d) {
    if (!ms_finite(root) || !ms_finite(d)) return ms_unavailable();
    if (root==0.0) return d==0.0 ? 0.0 : ms_unavailable();
    precise float r=d/(2.0*root); return ms_checked(r);
}
ms_Dfloat ms_dadd(ms_Dfloat a,ms_Dfloat b) {
    precise float v=a.v+b.v;
    return ms_seed(v,ms_change_add(a.d0,b.d0),ms_change_add(a.d1,b.d1));
}
ms_Dfloat ms_dsub(ms_Dfloat a,ms_Dfloat b) {
    precise float v=a.v-b.v;
    return ms_seed(v,ms_change_sub(a.d0,b.d0),ms_change_sub(a.d1,b.d1));
}
ms_Dfloat ms_dmul(ms_Dfloat a,ms_Dfloat b) {
    precise float v=a.v*b.v;
    return ms_seed(v,ms_change_product(a.v,b.v,a.d0,b.d0),ms_change_product(a.v,b.v,a.d1,b.d1));
}
ms_Dfloat ms_ddiv(ms_Dfloat a,ms_Dfloat b) {
    precise float v=a.v/b.v;
    return ms_seed(v,ms_change_quotient(v,b.v,a.d0,b.d0),ms_change_quotient(v,b.v,a.d1,b.d1));
}
ms_Dfloat ms_dneg(ms_Dfloat a) {
    return ms_seed(-a.v,ms_change_scale(-1.0,a.d0),ms_change_scale(-1.0,a.d1));
}
ms_Dfloat ms_dsin(ms_Dfloat a) {
    precise float v=sin(a.v), factor=cos(a.v);
    return ms_seed(v,ms_change_scale(factor,a.d0),ms_change_scale(factor,a.d1));
}
ms_Dfloat ms_dcos(ms_Dfloat a) {
    precise float v=cos(a.v), factor=-sin(a.v);
    return ms_seed(v,ms_change_scale(factor,a.d0),ms_change_scale(factor,a.d1));
}
ms_Dfloat ms_dsqrt(ms_Dfloat a) {
    precise float v=sqrt(a.v);
    return ms_seed(v,ms_change_root(v,a.d0),ms_change_root(v,a.d1));
}
ms_Dfloat ms_dfloor(ms_Dfloat a) { return ms_constant(floor(a.v)); }
ms_Dfloat ms_dceil(ms_Dfloat a) { return ms_constant(ceil(a.v)); }
ms_Dfloat ms_dtrunc(ms_Dfloat a) { return ms_constant(trunc(a.v)); }
ms_Dfloat ms_dstep(ms_Dfloat a,ms_Dfloat b) { return ms_constant(step(a.v,b.v)); }
ms_Dfloat ms_dfract(ms_Dfloat a) {
    precise float raw=a.v-floor(a.v);
    precise float v=min(raw,asfloat(0x3f7fffffu));
    // Equality is not the capped region; in particular integers retain d.
    return ms_seed(v,raw>asfloat(0x3f7fffffu)?0.0:a.d0,raw>asfloat(0x3f7fffffu)?0.0:a.d1);
}
ms_Dfloat ms_dabs(ms_Dfloat a) {
    return ms_seed(abs(a.v),a.v>=0.0?a.d0:ms_change_scale(-1.0,a.d0),a.v>=0.0?a.d1:ms_change_scale(-1.0,a.d1));
}
ms_Dfloat ms_dmin(ms_Dfloat a,ms_Dfloat b) {
    return ms_seed(min(a.v,b.v),a.v<=b.v?a.d0:b.d0,a.v<=b.v?a.d1:b.d1);
}
ms_Dfloat ms_dmax(ms_Dfloat a,ms_Dfloat b) {
    return ms_seed(max(a.v,b.v),a.v>=b.v?a.d0:b.d0,a.v>=b.v?a.d1:b.d1);
}
ms_Dfloat ms_dclamp(ms_Dfloat a,ms_Dfloat b,ms_Dfloat c) { return ms_dmax(b,ms_dmin(a,c)); }
ms_Dfloat ms_dlerp(ms_Dfloat a,ms_Dfloat b,ms_Dfloat t) {
    // Differentiate the weighted formula, including both t contributions at
    // endpoints. Guarded operations keep 0*unavailable unavailable, not zero.
    return ms_dadd(ms_dmul(a,ms_dsub(ms_constant(1.0),t)),ms_dmul(b,t));
}
)hlsl";

inline constexpr std::string_view material_footprint_sample_hlsl = R"hlsl(
bool ms_finite2(float2 v) { return ms_finite(v.x) && ms_finite(v.y); }
ms_Dfloat4 ms_dsample(uint environment,uint slot,ms_Dfloat2 uv) {
    ms_Dfloat4 r;
    r.d0=r.d1=(float4)ms_unavailable();
    // Invalid center coordinates are an invalid author result, never a
    // repaired coordinate or a request to sample some arbitrary border.
    if (!ms_finite2(uv.v)) { r.v=(float4)ms_unavailable(); return r; }
    r.v=::atelier_material_sample_grad(environment,slot,uv.v,uv.d0,uv.d1);
    // The bridge must choose the actual coarsest mip for this center sample.
    // Do not send these invalid directions on to offset/neighbor evaluation.
    if (!ms_finite2(uv.d0) || !ms_finite2(uv.d1)) return r;
    precise float2 p0=uv.v+uv.d0, p1=uv.v+uv.d1;
    if (ms_finite2(p0)) {
        precise float4 s=::atelier_material_sample_grad(environment,slot,p0,uv.d0,uv.d1);
        r.d0=float4(ms_change_sub(s.x,r.v.x),ms_change_sub(s.y,r.v.y),ms_change_sub(s.z,r.v.z),ms_change_sub(s.w,r.v.w));
    }
    if (ms_finite2(p1)) {
        precise float4 s=::atelier_material_sample_grad(environment,slot,p1,uv.d0,uv.d1);
        r.d1=float4(ms_change_sub(s.x,r.v.x),ms_change_sub(s.y,r.v.y),ms_change_sub(s.z,r.v.z),ms_change_sub(s.w,r.v.w));
    }
    return r;
}
)hlsl";
}
