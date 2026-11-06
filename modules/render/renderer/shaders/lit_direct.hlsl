// GBuffer receiver helpers shared by light lists and composition.
float3 lit_direct_finite(float3 value) {
    return all(isfinite(value)) ? clamp(value, 0, 65504) : 0;
}
float2 lit_direct_uv(int2 pixel) { return (float2(pixel) + .5) / lit_size_frame.xy; }
bool lit_direct_surface(int2 pixel) {
    if (any(pixel < 0) || any(pixel >= int2(lit_size_frame.xy))) return false;
    float3 normal = lit_normal_roughness.Load(int3(pixel, 0)).xyz;
    return lit_inside(uint2(pixel)) && !lit_is_background_depth(lit_depth.Load(int3(pixel, 0))) &&
        dot(normal, normal) > .25;
}
float3 lit_direct_world(int2 pixel) {
    return lit_world(lit_direct_uv(pixel), lit_depth.Load(int3(pixel, 0)));
}
LitLightingSurface lit_direct_surface_data(uint2 p) {
    LitLightingSurface surface;
    float2 uv=lit_direct_uv(int2(p));
    float4 nr=lit_normal_roughness.Load(int3(p,0)),bm=lit_base_metallic.Load(int3(p,0));
    surface.position=lit_world(uv,lit_depth.Load(int3(p,0)));
    surface.normal=lit_safe_normal(nr.xyz);surface.geometric_normal=lit_geometry_normal_at(p);
    surface.view=lit_view_direction(uv);surface.base=bm.rgb;
    surface.roughness=nr.w;surface.metallic=bm.w;
    return surface;
}
