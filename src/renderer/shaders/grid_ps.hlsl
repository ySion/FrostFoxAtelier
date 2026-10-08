row_major float4x4 inverse_view_projection;
row_major float4x4 view_projection;
uint2 guides;
float grid_spacing;
uint grid_plane;

struct GridOutput {
    float4 color : SV_Target;
#if GRID_DEPTH
    float depth : SV_Depth;
#endif
};
float lines(float2 coordinates, float2 pixel_width, float spacing) {
    float2 distance = abs(frac(coordinates / spacing + .5) - .5) * spacing / pixel_width;
    return 1 - smoothstep(.25, 1.25, min(distance.x, distance.y));
}
float pattern(float2 coordinates, float2 pixel_width, float spacing) {
    return max(.16 * lines(coordinates, pixel_width, spacing),
               .32 * lines(coordinates, pixel_width, spacing * 5));
}
GridOutput main(float4 position : SV_Position, float2 uv : TEXCOORD0) {
    float2 ndc = uv * float2(2, -2) + float2(-1, 1);
    float4 near_h = mul(float4(ndc, 0, 1), inverse_view_projection);
    // A point inside the frustum also supports projections with an infinite far plane.
    float4 middle_h = mul(float4(ndc, .5, 1), inverse_view_projection);
    if (abs(near_h.w) < 1e-8 || abs(middle_h.w) < 1e-8) discard;
    float3 start = near_h.xyz / near_h.w;
    float3 ray = middle_h.xyz / middle_h.w - start;
    float ray_length = length(ray);
    uint normal_axis = grid_plane == 0 ? 1 : grid_plane == 1 ? 2 : 0;
    if (ray_length == 0 || abs(ray[normal_axis]) < ray_length * .001) discard;
    float t = -start[normal_axis] / ray[normal_axis];
    if (t <= 0) discard;
    float3 world = start + t * ray;
    if (!all(isfinite(world))) discard;
    float4 clip = mul(float4(world, 1), view_projection);
    if (clip.w <= 0) discard;
    float z = clip.z / clip.w;
    if (z < 0 || z >= 1) discard;

    float2 coordinates = grid_plane == 0 ? world.xz : grid_plane == 1 ? world.xy : world.yz;
    float2 dx = ddx(coordinates), dy = ddy(coordinates);
    float2 pixel_width = max(sqrt(dx * dx + dy * dy), 1e-6);
    // Crossfade decimal grid levels; five minor cells form each major cell.
    float level = max(0, log10(max(pixel_width.x, pixel_width.y) * 8 / grid_spacing));
    float spacing = grid_spacing * pow(10, floor(level));
    float alpha = guides.x != 0 ? lerp(pattern(coordinates, pixel_width, spacing),
        pattern(coordinates, pixel_width, spacing * 10), frac(level)) : 0;
    float3 ink = float3(.045, .075, .09);
    if (guides.y != 0) {
        float2 axis = 1 - smoothstep(.65, 1.65, abs(coordinates) / pixel_width);
        float strength = max(axis.x, axis.y) * .72;
        float3 first = grid_plane == 2 ? float3(.09,.36,.075) : float3(.5,.12,.085);
        float3 second = grid_plane == 1 ? float3(.09,.36,.075) : float3(.06,.16,.46);
        float3 axis_ink = axis.y >= axis.x ? first : second;
        ink = lerp(ink, axis_ink, saturate(strength / max(alpha, 1e-6)));
        alpha = max(alpha, strength);
    }
    alpha *= smoothstep(.008, .035, abs(ray[normal_axis]) / ray_length) *
             (1 - smoothstep(10000, 20000, length(world - start)));
    if (alpha <= 0) discard;
    GridOutput result;
    result.color = float4(ink * alpha, alpha);
#if GRID_DEPTH
    // Geometry on the reference plane wins; this value is tested, never stored.
    result.depth = min(z + 1e-7, 1);
#endif
    return result;
}
