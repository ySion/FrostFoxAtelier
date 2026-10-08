// Native SMAA1x / two-independent-frame SMAAT2x, before UI composition.
// CMake prepends lit_common.hlsl and embeds the pinned MIT reference below.
// Color/spatial/history/output are tone-mapped scene-linear SDR, not sRGB bytes.
// Intermediate edge and weight textures: RGBA8_UNORM, no sRGB views.
Texture2D<float4> lit_smaa_color;
Texture2D<float4> lit_smaa_edges;
Texture2D<float4> lit_smaa_weights;
Texture2D<float4> lit_smaa_area;
Texture2D<float4> lit_smaa_search;
Texture2D<float4> lit_smaa_spatial;
Texture2D<float4> lit_smaa_previous;
Texture2D<float> lit_smaa_expected_previous_depth;
Texture2D<float> lit_history_depth;
Texture2D<float4> lit_history_normal;
RWTexture2D<float4> lit_smaa_edges_output;
RWTexture2D<float4> lit_smaa_weights_output;
RWTexture2D<float4> lit_smaa_spatial_output;
RWTexture2D<float4> lit_smaa_output;
float4 lit_smaa_subsample_indices;
uint lit_smaa_temporal_enabled;
// 1x: indices = 0, no projection jitter. T2x: indices (1,1,1,0)
// with engine top-down image jitter (+.25,+.25), then (2,2,2,0)
// with (-.25,-.25). Upstream's table uses the opposite Y convention.

float4 lit_smaa_point(Texture2D<float4> source, float2 uv) {
    uint width, height; source.GetDimensions(width, height);
    int2 p = clamp(int2(uv * float2(width, height)), int2(0, 0), int2(width, height) - 1);
    return source.Load(int3(p, 0));
}

// Reference's custom-language hook replaces implicit derivatives with LOD 0
// and reuses the renderer's linear clamp sampler. The reference stays verbatim.
#define SMAA_CUSTOM_SL 1
#define SMAA_PRESET_HIGH 1
#define SMAA_REPROJECTION 1
#define SMAA_RT_METRICS float4(1.0 / float2(lit_size_frame.xy), float2(lit_size_frame.xy))
#define SMAATexture2D(tex) Texture2D<float4> tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(lit_linear_sampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) lit_smaa_point(tex, coord)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(lit_linear_sampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.SampleLevel(lit_linear_sampler, coord, 0)
#define SMAASamplePoint(tex, coord) lit_smaa_point(tex, coord)
#define SMAASampleOffset(tex, coord, offset) tex.SampleLevel(lit_linear_sampler, coord, 0, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]

// SMAA_REFERENCE_SOURCE

float2 lit_smaa_uv(uint2 p) { return (float2(p) + .5) / float2(lit_size_frame.xy); }
bool lit_smaa_inside_uv(float2 uv) {
    float2 p = uv * float2(lit_size_frame.xy);
    return all(isfinite(uv)) && all(p >= lit_viewport.xy + .5) &&
        all(p <= lit_viewport.xy + lit_viewport.zw - .5) && all(uv >= 0) && all(uv < 1);
}
int2 lit_smaa_clamp_scene_pixel(int2 p) {
    int2 first = max(int2(0, 0), int2(ceil(lit_viewport.xy - .5)));
    int2 last = min(int2(lit_size_frame.xy) - 1,
                    int2(ceil(lit_viewport.xy + lit_viewport.zw - .5)) - 1);
    return clamp(p, first, max(first, last));
}
float3 lit_smaa_sdr(float3 color) { return all(isfinite(color)) ? saturate(color) : 0; }
float lit_smaa_gamma_luma(int2 p) {
    float3 linear_color = lit_smaa_sdr(lit_smaa_color.Load(int3(lit_smaa_clamp_scene_pixel(p), 0)).rgb);
    float3 nonlinear;
    nonlinear.r = linear_color.r <= .0031308 ? 12.92 * linear_color.r : 1.055 * pow(linear_color.r, 1.0 / 2.4) - .055;
    nonlinear.g = linear_color.g <= .0031308 ? 12.92 * linear_color.g : 1.055 * pow(linear_color.g, 1.0 / 2.4) - .055;
    nonlinear.b = linear_color.b <= .0031308 ? 12.92 * linear_color.b : 1.055 * pow(linear_color.b, 1.0 / 2.4) - .055;
    return dot(nonlinear, float3(.2126, .7152, .0722));
}

[numthreads(8, 8, 1)]
void lit_smaa_edges_cs(uint3 id : SV_DispatchThreadID) {
    uint2 p = id.xy;
    if (any(p >= lit_size_frame.xy)) return;
    // Explicit zero stores replace reference's discard + per-frame clear.
    if (!lit_inside(p)) { lit_smaa_edges_output[p] = 0; return; }
    int2 q = int2(p);
    float center = lit_smaa_gamma_luma(q);
    float left = lit_smaa_gamma_luma(q + int2(-1, 0));
    float top = lit_smaa_gamma_luma(q + int2(0, -1));
    float2 delta = abs(center - float2(left, top));
    float2 edges = step(float2(SMAA_THRESHOLD, SMAA_THRESHOLD), delta);
    if (dot(edges, float2(1, 1)) == 0) { lit_smaa_edges_output[p] = 0; return; }
    float right = lit_smaa_gamma_luma(q + int2(1, 0));
    float bottom = lit_smaa_gamma_luma(q + int2(0, 1));
    float2 max_delta = max(delta, abs(center - float2(right, bottom)));
    float left_left = lit_smaa_gamma_luma(q + int2(-2, 0));
    float top_top = lit_smaa_gamma_luma(q + int2(0, -2));
    max_delta = max(max_delta, abs(float2(left, top) - float2(left_left, top_top)));
    float final_delta = max(max_delta.x, max_delta.y);
    edges *= step(final_delta, SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR * delta);
    lit_smaa_edges_output[p] = float4(edges, 0, 0);
}

[numthreads(8, 8, 1)]
void lit_smaa_weights_cs(uint3 id : SV_DispatchThreadID) {
    uint2 p = id.xy;
    if (any(p >= lit_size_frame.xy)) return;
    if (!lit_inside(p) || dot(lit_smaa_edges.Load(int3(p, 0)).rg, float2(1, 1)) == 0) {
        lit_smaa_weights_output[p] = 0; return;
    }
    float2 uv = lit_smaa_uv(p), pixcoord;
    float4 offsets[3];
    SMAABlendingWeightCalculationVS(uv, pixcoord, offsets);
    lit_smaa_weights_output[p] = SMAABlendingWeightCalculationPS(uv, pixcoord, offsets,
        lit_smaa_edges, lit_smaa_area, lit_smaa_search, lit_smaa_subsample_indices);
}

[numthreads(8, 8, 1)]
void lit_smaa_neighborhood_cs(uint3 id : SV_DispatchThreadID) {
    uint2 p = id.xy;
    if (any(p >= lit_size_frame.xy)) return;
    if (!lit_inside(p)) { lit_smaa_spatial_output[p] = 0; return; }
    float2 uv = lit_smaa_uv(p); float4 offset;
    SMAANeighborhoodBlendingVS(uv, offset);
    float4 spatial = SMAANeighborhoodBlendingPS(uv, offset, lit_smaa_color, lit_smaa_weights, lit_motion);
    spatial.rgb = lit_smaa_sdr(spatial.rgb);
    // T2x uses reference's antialiased velocity-magnitude alpha. One-frame
    // output stays opaque. Never feed the temporal-resolved image back here.
    if (lit_smaa_temporal_enabled == 0) spatial.a = 1;
    lit_smaa_spatial_output[p] = spatial;
}

void lit_smaa_previous_plane(float2 uv, float3 position, float3 normal, float expected_depth,
                               out float tolerance, out float2 gradient) {
    // Match the shared device-Z convention and compare history taps against
    // the projected surface plane, not the same center depth on a tilted floor.
    // A world-space tolerance plus relative FP32 roundoff remains meaningful
    // for reverse-Z far surfaces; an absolute device-Z epsilon does not.
    gradient = 0; tolerance = abs(expected_depth) * (8 * 1.1920929e-7);
    float depth = lit_depth.Load(int3(int2(uv * float2(lit_size_frame.xy)), 0));
    float pixel_size = max(length(lit_world(uv + float2(1.0 / lit_size_frame.x, 0), depth) - position),
        length(lit_world(uv + float2(0, 1.0 / lit_size_frame.y), depth) - position));
    float scale = max(pixel_size, .001);
    float3 tangent, bitangent; lit_basis(normal, tangent, bitangent);
    float4 a = mul(float4(position, 1), lit_previous_view_projection);
    float4 b = mul(float4(position + tangent * scale, 1), lit_previous_view_projection);
    float4 c = mul(float4(position + bitangent * scale, 1), lit_previous_view_projection);
    float4 extent = mul(float4(position + lit_view_direction(uv) * .02, 1), lit_previous_view_projection);
    if (a.w <= 1e-8) return;
    if (extent.w > 1e-8) tolerance += abs(extent.z / extent.w - a.z / a.w);
    if (b.w <= 1e-8 || c.w <= 1e-8) return;
    float2 ab = lit_window_uv(b) - lit_window_uv(a), ac = lit_window_uv(c) - lit_window_uv(a);
    float db = b.z / b.w - a.z / a.w, dc = c.z / c.w - a.z / a.w;
    float determinant = ab.x * ac.y - ab.y * ac.x;
    if (abs(determinant) < 1e-16) return;
    gradient = float2(db * ac.y - dc * ab.y, dc * ab.x - db * ac.x) / determinant;
    if (!all(isfinite(gradient))) gradient = 0;
}
bool lit_smaa_history(float2 uv, float expected_depth, float3 normal, float tolerance,
                       float2 gradient, out float4 history, out float coverage) {
    history = 0; coverage = 0;
    if (!lit_smaa_inside_uv(uv) || !isfinite(expected_depth) || lit_is_background_depth(expected_depth) ||
        expected_depth < 0 || expected_depth > 1) return false;
    float2 sample_position = uv * float2(lit_size_frame.xy) - .5;
    int2 base = int2(floor(sample_position)); float2 fraction = frac(sample_position);
    [unroll] for (int y = 0; y < 2; ++y) [unroll] for (int x = 0; x < 2; ++x) {
        int2 q = base + int2(x, y);
        if (any(q < 0) || any(q >= int2(lit_size_frame.xy)) || !lit_inside(uint2(q))) continue;
        float previous_depth = lit_history_depth.Load(int3(q, 0));
        float3 previous_normal = lit_history_normal.Load(int3(q, 0)).xyz;
        if (dot(previous_normal, previous_normal) < .25 || !isfinite(previous_depth)) continue;
        float projected_depth = expected_depth + dot(lit_smaa_uv(uint2(q)) - uv, gradient);
        if (lit_is_background_depth(previous_depth) || previous_depth < 0 || previous_depth > 1 ||
            abs(previous_depth - projected_depth) > tolerance ||
            dot(normal, lit_safe_normal(previous_normal)) < .94) continue;
        float4 sample_color = lit_smaa_previous.Load(int3(q, 0));
        if (!all(isfinite(sample_color))) continue;
        float weight = (x == 0 ? 1 - fraction.x : fraction.x) * (y == 0 ? 1 - fraction.y : fraction.y);
        history += sample_color * weight; coverage += weight;
    }
    if (coverage < .1) return false;
    history /= coverage;
    return true;
}

[numthreads(8, 8, 1)]
void lit_smaa_resolve_cs(uint3 id : SV_DispatchThreadID) {
    uint2 p = id.xy;
    if (any(p >= lit_size_frame.xy)) return;
    if (!lit_inside(p)) { lit_smaa_output[p] = 0; return; }
    float4 current = lit_smaa_spatial.Load(int3(p, 0));
    float3 color = lit_smaa_sdr(current.rgb);
    float4 motion = lit_motion.Load(int3(p, 0));
    float3 normal = lit_normal_roughness.Load(int3(p, 0)).xyz;
    if (lit_smaa_temporal_enabled != 0 && lit_size_frame.w != 0 && motion.w > .5 &&
        all(isfinite(motion)) && !lit_is_background_depth(lit_depth.Load(int3(p, 0))) && dot(normal, normal) >= .25) {
        float2 uv = lit_smaa_uv(p), previous_uv = uv + motion.xy;
        float3 position = lit_world(uv, lit_depth.Load(int3(p, 0)));
        normal = lit_safe_normal(normal);
        float expected_depth = lit_smaa_expected_previous_depth.Load(int3(p, 0));
        float tolerance; float2 gradient;
        lit_smaa_previous_plane(uv, position, normal, expected_depth, tolerance, gradient);
        float4 previous; float coverage;
        if (lit_smaa_history(previous_uv, expected_depth,
                             normal, tolerance, gradient, previous, coverage)) {
            // Original T2x motion-magnitude weighting, with history capped at
            // one previous spatial frame. Local geometric validity and coverage
            // control confidence; changing lights retain the short two-frame
            // blend instead of globally turning history off.
            float delta = abs(current.a * current.a - previous.a * previous.a) / 5.0;
            float weight = .5 * saturate(1 - sqrt(delta) * SMAA_REPROJECTION_WEIGHT_SCALE);
            weight *= saturate(coverage);
            color = lerp(color, lit_smaa_sdr(previous.rgb), weight);
        }
    }
    lit_smaa_output[p] = float4(lit_smaa_sdr(color), 1);
}
