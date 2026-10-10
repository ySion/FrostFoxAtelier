Texture2D<float4> matcap;
SamplerState matcap_sampler;
Texture2D<float4> diffuse_texture;
Texture2D<float4> opacity_texture;
SamplerState material_sampler;
float4 material_color;

float edge_distance(float value, float dx, float dy) {
    // Normalize before squaring: homogeneous edge gradients scale with 1/w.
    // A constant edge function describes no finite screen-space boundary.
    float scale = max(abs(dx), abs(dy));
    if (scale == 0) return 1e8;
    float2 gradient = float2(dx, dy) / scale;
    return abs(value / scale) * rsqrt(dot(gradient, gradient));
}

float4 main(SceneOutput input) : SV_Target {
    float4 diffuse = 1;
    // Scene Lit's current GBuffer and ray queries are opaque. Its auxiliary
    // depth and selection overlay must describe those same surfaces. Wire-only
    // mode still uses the studio material coverage, even with Scene Lit selected.
    if (display.x != 3 || display.y == 2) {
        diffuse = diffuse_texture.Sample(material_sampler, input.uv) * material_color;
        float opacity = diffuse.a * opacity_texture.Sample(material_sampler, input.uv).r;
        clip(opacity - .5);
    }
    if (display.w == 2) return 0;
    if(display.w==1 && display.x==3 && display.y!=1 && input.selected==0) discard;
    float3 color = 0;
    if (display.w == 0) {
        float3 normal = unit_normal(input.normal);
        if (display.x == 2) {
            float3 eye_normal = unit_normal(mul(normal, (float3x3)view));
            color = matcap.SampleLevel(matcap_sampler, eye_normal.xy * float2(.5, -.5) + .5, 0).rgb;
        } else {
            float light = .3 + .7 * saturate(dot(normal, normalize(float3(.4, .8, -.6))));
            color = input.color.rgb * diffuse.rgb * light;
        }
        if (display.y != 1 && input.selected == 0) return float4(color, 1);
    }
    // Dividing perspective barycentrics by the interpolated clip w recovers
    // affine edge functions, including vertices on/behind w=0. Raw screen
    // barycentrics can be negative there and turn an entire face into a line.
    float3 edge_function = input.edge_coordinates.xyz / input.edge_coordinates.w;
    float3 dx = ddx(edge_function), dy = ddy(edge_function);
    float3 distance = float3(edge_distance(edge_function.x, dx.x, dy.x),
                             edge_distance(edge_function.y, dx.y, dy.y),
                             edge_distance(edge_function.z, dx.z, dy.z));
    distance = float3((input.edge_mask & 1) != 0 ? distance.x : 1e8,
                      (input.edge_mask & 2) != 0 ? distance.y : 1e8,
                      (input.edge_mask & 4) != 0 ? distance.z : 1e8);
    float width = input.selected != 0 ? max(wire_style.x, 2.0) : wire_style.x;
    float edge = 1 - smoothstep(max(0, width * .5 - .5), width * .5 + .5,
                               min(distance.x, min(distance.y, distance.z)));
    float3 ink = input.selected != 0 ? float3(1, .69, 0) :
                  display.w == 1 ? float3(.48, .55, .62) : float3(.025, .032, .04);
    float alpha = edge * (input.selected != 0 || display.w == 1 ? 1 : wire_style.y);
    if (display.w == 1) {
        if (alpha <= 0) discard;
        return float4(ink * alpha, alpha);
    }
    if (display.y == 1 || input.selected != 0) color = lerp(color, ink, alpha);
    return float4(color, 1);
}
