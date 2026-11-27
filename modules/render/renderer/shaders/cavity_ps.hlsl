#ifndef ATELIER_SAFE_DESC_DEFINED
#define ATELIER_SAFE_DESC_DEFINED 1
uint atelier_safe_desc(uint idx) {
    return (idx < 262144u) ? idx : 0u;
}
#endif

cbuffer ViewportRootConstants : register(b0, space0) {
    float4 g_raster_viewport_transform;
    uint g_pass_cb_srv;
    uint g_pass_cb_index;
    uint g_cavity_normals_srv;
    uint g_cavity_color_srv;
    uint g_cavity_depth_srv;
};

struct CavityConstants {
    row_major float4x4 inverse_projection;
    float4 viewport;
    float4 controls;
};

bool cavity_inside(int2 p, float4 viewport) {
    float2 center = float2(p) + .5;
    return all(center >= viewport.xy) && all(center < viewport.xy + viewport.zw);
}

float3 cavity_position(float2 pixel, float depth, float4 viewport, row_major float4x4 inv_proj) {
    float2 uv = (pixel - viewport.xy) / viewport.zw;
    float4 position = mul(float4(uv * float2(2, -2) + float2(-1, 1), depth, 1), inv_proj);
    return position.xyz / position.w;
}

float cavity_length(float3 value) {
    float scale = max(abs(value.x), max(abs(value.y), abs(value.z)));
    return scale > 0 ? scale * length(value / scale) : 0;
}

float cavity_curvature(int2 p, float3 position, float3 normal, float pixel_size, int radius,
                       CavityConstants cb, Texture2D<float4> cavity_normals, Texture2D<float> cavity_depth) {
    if (radius == 0) return 0;
    static const int2 directions[4] = { int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1) };
    float result = 0;
    [unroll] for (uint i = 0; i < 4; ++i) {
        int2 q = p + directions[i] * radius;
        if (!cavity_inside(q, cb.viewport)) continue;
        float4 other = cavity_normals.Load(int3(q, 0));
        if (other.w == 0) continue;
        if (!all(isfinite(other.xyz)) || dot(other.xyz, other.xyz) < .5) continue;
        float3 q_normal = normalize(other.xyz);
        float3 delta = cavity_position(float2(q) + .5, cavity_depth.Load(int3(q, 0)), cb.viewport, cb.inverse_projection) - position;
        float tolerance = pixel_size * radius * 4;
        if (!all(isfinite(delta)) || abs(dot(delta, normal)) > tolerance || abs(dot(delta, q_normal)) > tolerance) continue;
        result += dot((q_normal.xy - normal.xy) * float2(1, -1), float2(directions[i]));
    }
    return result * .25;
}

float4 main(float4 screen : SV_Position) : SV_Target {
    StructuredBuffer<CavityConstants> cb_buf = ResourceDescriptorHeap[atelier_safe_desc(g_pass_cb_srv)];
    CavityConstants cb = cb_buf[g_pass_cb_index];
    Texture2D<float4> cavity_normals = ResourceDescriptorHeap[atelier_safe_desc(g_cavity_normals_srv)];
    Texture2D<float4> cavity_color = ResourceDescriptorHeap[atelier_safe_desc(g_cavity_color_srv)];
    Texture2D<float> cavity_depth = ResourceDescriptorHeap[atelier_safe_desc(g_cavity_depth_srv)];

    int2 p = int2(screen.xy);
    if (!cavity_inside(p, cb.viewport)) discard;
    float4 sample = cavity_normals.Load(int3(p, 0));
    if (sample.w == 0) discard;
    float3 color = cavity_color.Load(int3(p, 0)).rgb;
    if (!all(isfinite(sample.xyz)) || dot(sample.xyz, sample.xyz) < .5) return float4(color, 1);
    float depth = cavity_depth.Load(int3(p, 0));
    float3 position = cavity_position(screen.xy, depth, cb.viewport, cb.inverse_projection);
    float3 normal = normalize(sample.xyz);
    float pixel_size = max(cavity_length(cavity_position(screen.xy + float2(1, 0), depth, cb.viewport, cb.inverse_projection) - position),
                           cavity_length(cavity_position(screen.xy + float2(0, 1), depth, cb.viewport, cb.inverse_projection) - position));
    if (!all(isfinite(position)) || !isfinite(pixel_size) || pixel_size <= 0) return float4(color, 1);
    int lower = int(floor(cb.controls.z)), upper = int(ceil(cb.controls.z));
    float curvature = cavity_curvature(p, position, normal, pixel_size, lower, cb, cavity_normals, cavity_depth);
    if (upper != lower)
        curvature = lerp(curvature, cavity_curvature(p, position, normal, pixel_size, upper, cb, cavity_normals, cavity_depth), frac(cb.controls.z));
    float ridge = saturate(max(curvature - .002, 0) * cb.controls.x * 1.5) * .35;
    float valley = saturate(max(-curvature - .002, 0) * cb.controls.y * 1.5) * .5;
    return float4(lerp(color * (1 - valley), float3(1, 1, 1), ridge), 1);
}
