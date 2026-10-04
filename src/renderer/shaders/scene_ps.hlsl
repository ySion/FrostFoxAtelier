Texture2D<float4> matcap;
SamplerState matcap_sampler;

float4 main(SceneOutput input) : SV_Target {
    if (display.w == 2) return 0;
    float3 color = 0;
    if (display.w == 0) {
        float3 normal = unit_normal(input.normal);
        if (display.x == 2) {
            float3 eye_normal = unit_normal(mul(normal, (float3x3)view));
            color = matcap.SampleLevel(matcap_sampler, eye_normal.xy * float2(.5, -.5) + .5, 0).rgb;
        } else {
            float light = .3 + .7 * saturate(dot(normal, normalize(float3(.4, .8, -.6))));
            color = input.color.rgb * light;
        }
        if (display.y != 1 && input.selected == 0) return float4(color, 1);
    }
    // Pixel distances to original edges; the mask omits triangulation diagonals.
    float3 dx = ddx(input.barycentric), dy = ddy(input.barycentric);
    float3 distance = input.barycentric / max(sqrt(dx * dx + dy * dy), 1e-8);
    distance = float3((input.edge_mask & 1) != 0 ? distance.x : 1e8,
                      (input.edge_mask & 2) != 0 ? distance.y : 1e8,
                      (input.edge_mask & 4) != 0 ? distance.z : 1e8);
    float width = input.selected != 0 ? max(wire_style.x, 2.0) : wire_style.x;
    float edge = 1 - smoothstep(max(0, width * .5 - .5), width * .5 + .5,
                               min(distance.x, min(distance.y, distance.z)));
    float3 ink = input.selected != 0 ? float3(1, .42, .035) :
                  display.w == 1 ? float3(.48, .55, .62) : float3(.025, .032, .04);
    float alpha = edge * (input.selected != 0 || display.w == 1 ? 1 : wire_style.y);
    if (display.w == 1) {
        if (alpha <= 0) discard;
        return float4(ink * alpha, alpha);
    }
    if (display.y == 1 || input.selected != 0) color = lerp(color, ink, alpha);
    return float4(color, 1);
}
