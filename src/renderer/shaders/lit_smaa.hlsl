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
// Apply the same reference neighborhood kernel to color and motion separately.
// This retains antialiased velocity while preserving our float4 coverage lane;
// the upstream temporal decoder only receives float2 motion.
#define SMAA_REPROJECTION 0
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
float2 lit_smaa_surface_motion(float4 motion) {
    // T2x combines complementary subpixel samples on the unjittered grid.
    // Signal denoisers still consume the original jitter-aware GBuffer motion.
    // Coverage, not correspondence validity, makes this linear across silhouettes.
    float2 result = motion.xy - motion.z * (lit_jitter.zw - lit_jitter.xy) / float2(lit_size_frame.xy);
    // FP16 motion storage and FP32 projection leave subpixel cancellation noise.
    // The reference's sqrt velocity rejection amplifies it on stationary edges.
    // Limit the T2x-only noise floor to 1/256 display pixel, in both consumers.
    float2 pixels = result * float2(lit_size_frame.xy);
    return dot(pixels, pixels) <= (1.0 / 256) * (1.0 / 256) ? 0 : result;
}
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
    float4 spatial = SMAANeighborhoodBlendingPS(uv, offset, lit_smaa_color, lit_smaa_weights);
    spatial.rgb = lit_smaa_sdr(spatial.rgb);
    // T2x uses reference's antialiased velocity-magnitude alpha. One-frame
    // output stays opaque. Never feed the temporal-resolved image back here.
    spatial.a = 1;
    if (lit_smaa_temporal_enabled != 0) {
        float4 motion = SMAANeighborhoodBlendingPS(uv, offset, lit_motion, lit_smaa_weights);
        spatial.a = sqrt(5 * length(lit_smaa_surface_motion(motion)));
    }
    lit_smaa_spatial_output[p] = spatial;
}

[numthreads(8, 8, 1)]
void lit_smaa_resolve_cs(uint3 id : SV_DispatchThreadID) {
    uint2 p = id.xy;
    if (any(p >= lit_size_frame.xy)) return;
    if (!lit_inside(p)) { lit_smaa_output[p] = 0; return; }
    float4 current = lit_smaa_spatial.Load(int3(p, 0));
    float3 color = lit_smaa_sdr(current.rgb);
    float2 motion = lit_smaa_surface_motion(lit_motion.Load(int3(p, 0)));
    float2 previous_uv = lit_smaa_uv(p) + motion;
    if (lit_smaa_temporal_enabled != 0 && lit_size_frame.w != 0 &&
        all(isfinite(motion)) && lit_smaa_inside_uv(previous_uv)) {
        // SMAAResolvePS's two independent point samples and velocity-alpha
        // weighting. Keep this small adapter because our decoder needs motion.z
        // coverage, which the reference resolve's float2 interface discards.
        // Foreground/background differences are intentional subpixel coverage;
        // treating them as invalid history makes stationary silhouettes flicker.
        float4 previous = lit_smaa_point(lit_smaa_previous, previous_uv);
        if (all(isfinite(current)) && all(isfinite(previous))) {
            float delta = abs(current.a * current.a - previous.a * previous.a) / 5.0;
            float weight = .5 * saturate(1 - sqrt(delta) * SMAA_REPROJECTION_WEIGHT_SCALE);
            color = lerp(color, lit_smaa_sdr(previous.rgb), weight);
        }
    }
    lit_smaa_output[p] = float4(lit_smaa_sdr(color), 1);
}
