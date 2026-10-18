// Original integration of GTAO (Jimenez/Wu/Pesce/Jarabo, ATVI-TR-16-01,
// equations 5-8), visible GGX sampling (Heitz, JCGT 7(4), 2018), and a
// min/max-cell screen-space ray traversal. No upstream shader is embedded.
// Sources: https://www.activision.com/cdn/research/PracticalRealtimeStrategiesTRfinal.pdf
//          https://jcgt.org/published/0007/04/01/paper.pdf
// The build prepends lit_common.hlsl; every coordinate below is window-local
// unless explicitly converted through the shared viewport/camera contract.

Texture2D<float4> lit_hdr_source;
Texture2D<float2> lit_depth_input;
Texture2D<float2> lit_depth_pyramid[12];
RWTexture2D<float2> lit_depth_output;
uint2 lit_depth_output_size;
uint lit_depth_level_count;
RWTexture2D<float> lit_ao_output;
Texture2D<float> lit_ao_raw;
Texture2D<float2> lit_ao_previous; // AO visibility, effective history length.
RWTexture2D<float2> lit_ao_history_output;
RWTexture2D<float> lit_ao_resolved_output;
// GTAO working depth: view-space z and four reduced levels. Window
// pixels without a surface inside the viewport hold LIT_AO_EMPTY.
RWTexture2D<float> lit_ao_depth_output0;
RWTexture2D<float> lit_ao_depth_output1;
RWTexture2D<float> lit_ao_depth_output2;
RWTexture2D<float> lit_ao_depth_output3;
RWTexture2D<float> lit_ao_depth_output4;
Texture2D<float> lit_ao_depth;
float4 lit_ao_view_x, lit_ao_view_y; // view x = ndc.x*(z*a+b)+(z*c+d); jittered projection, no skew
float4 lit_ao_view_z; // P22,P32,P23,P33 of the row-vector projection
static const float LIT_AO_EMPTY = 3e38;

Texture2D<float2> lit_brdf_lut;
RWTexture2D<float2> lit_brdf_output;
RWStructuredBuffer<uint> lit_reflection_work;
RWStructuredBuffer<uint> lit_reflection_counter;
RWStructuredBuffer<uint> lit_reflection_arguments;
RWTexture2D<float4> lit_reflection_output;
RWTexture2D<uint2> lit_reflection_hit_trace_output;
Texture2D<float4> lit_reflection_trace;
Texture2D<uint2> lit_reflection_hit_trace;
Texture2D<float4> lit_reflection_previous;
Texture2D<float2> lit_reflection_hit_previous; // Continuous source anchor; zero means no witness.
RWTexture2D<float4> lit_reflection_resolved_output;
RWTexture2D<float2> lit_reflection_hit_output;
Texture2D<float2> lit_reflection_moments_previous; // Rec709 E[L], E[L^2]; RG32F.
RWTexture2D<float2> lit_reflection_moments_output;
Texture2D<float2> lit_reflection_moments;
Texture2D<float4> lit_reflection_filter_input;
RWTexture2D<float4> lit_reflection_filter_output;
uint lit_reflection_filter_step; // Separate images at successive steps 1, 2, 4.

Texture2D<float4> lit_reflection;
Texture2D<float4> lit_taa_previous;
Texture2D<float4> lit_history_normal;
Texture2D<float> lit_history_depth;
RWTexture2D<float4> lit_taa_output;
RWTexture2D<float4> lit_history_normal_output;
RWTexture2D<float2> lit_history_geometry_normal_output;
RWTexture2D<float> lit_history_depth_output;
RWTexture2D<float4> lit_history_base_metallic_output;
RWTexture2D<uint2> lit_history_surface_id_output;
Texture2D<float4> lit_taa;
Texture2D<float4> lit_bloom;
Texture2D<float4> lit_bloom_input;
RWTexture2D<float4> lit_bloom_output;
int2 lit_bloom_direction;
float4 lit_bloom_controls; // exposed threshold, soft knee, intensity, exposed clamp
RWTexture2D<float4> lit_tone_output; // Linear SDR; no display transfer here.
Texture2D<float4> lit_display_source; // Final AA output, still linear SDR.

float3 lit_post_finite(float3 color) {
    return all(isfinite(color)) ? clamp(color, 0, 65000) : 0;
}
float lit_post_luminance(float3 color) {
    return dot(color, float3(.2126, .7152, .0722));
}
float2 lit_post_uv(uint2 p) { return (float2(p) + .5) / lit_size_frame.xy; }
bool lit_post_inside_uv(float2 uv) {
    float2 p = uv * lit_size_frame.xy;
    return all(isfinite(uv)) && all(p >= lit_viewport.xy + .5) &&
        all(p <= lit_viewport.xy + lit_viewport.zw - .5) && all(uv >= 0) && all(uv < 1);
}
int2 lit_post_pixel(float2 uv) {
    return clamp(int2(uv * lit_size_frame.xy), int2(0, 0), int2(lit_size_frame.xy) - 1);
}
uint2 lit_reflection_pack_hit(float4 hit) {
    if (hit.w <= 0) return 0;
    uint2 pixel = uint2(lit_post_pixel(hit.xy));
    // Exact texel identity and FP32 distance in the same eight bytes as before.
    // Half-precision UV can select the wrong source across a silhouette at 1440p.
    return uint2(pixel.x | (pixel.y << 16), asuint(hit.w));
}
float4 lit_reflection_unpack_hit(uint2 hit) {
    return float4(lit_post_uv(uint2(hit.x & 65535, hit.x >> 16)), 0, asfloat(hit.y));
}
bool lit_post_surface(uint2 p) {
    return lit_inside(p) && !lit_is_background_depth(lit_depth.Load(int3(p, 0))) &&
        dot(lit_normal_roughness.Load(int3(p, 0)).xyz,
            lit_normal_roughness.Load(int3(p, 0)).xyz) > .25;
}
float3 lit_post_world(uint2 p) {
    return lit_world(lit_post_uv(p), lit_depth.Load(int3(p, 0)));
}
float lit_post_pixel_world_size(float2 uv, float depth) {
    float3 p = lit_world(uv, depth);
    float3 px = lit_world(uv + float2(1.0 / lit_size_frame.x, 0), depth);
    float3 py = lit_world(uv + float2(0, 1.0 / lit_size_frame.y), depth);
    return max(length(px - p), length(py - p));
}
float3 lit_post_ycocg(float3 color) {
    return float3(dot(color, float3(.25, .5, .25)),
                  .5 * (color.r - color.b), .5 * color.g - .25 * (color.r + color.b));
}
float3 lit_post_rgb(float3 color) {
    return float3(color.x + color.y - color.z, color.x + color.z, color.x - color.y - color.z);
}

// Integral of cos(theta-gamma)*abs(sin(theta)) from zero to each signed
// horizon, including the projected-normal length (GTAO equation 7/8).
float lit_gtao_arc(float angle, float normal_angle) {
    return .25 * (-cos(2 * angle - normal_angle) + cos(normal_angle) +
                   2 * angle * sin(normal_angle));
}
float lit_ao_window_depth(uint2 q) {
    if (any(q >= lit_size_frame.xy) || !lit_inside(q)) return LIT_AO_EMPTY;
    float device = lit_depth.Load(int3(q, 0));
    if (lit_is_background_depth(device)) return LIT_AO_EMPTY;
    return (lit_ao_view_z.y - device * lit_ao_view_z.w) / (device * lit_ao_view_z.z - lit_ao_view_z.x);
}
// A reduced texel averages the surfaces near the farthest one in its footprint
// (the XeGTAO depth-MIP idea): a thin foreground edge must not stand in for
// the wider background at distant taps. Empty texels never become occluders.
float lit_ao_reduce(float4 z) {
    float farthest = -LIT_AO_EMPTY;
    [unroll] for (uint i = 0; i < 4; ++i) if (z[i] < LIT_AO_EMPTY) farthest = max(farthest, z[i]);
    if (farthest == -LIT_AO_EMPTY) return LIT_AO_EMPTY;
    float range = .75 * lit_controls.z, from = range * (1 - .615), falloff = max(range * .615, 1e-6);
    float sum = 0, weight = 0;
    [unroll] for (uint j = 0; j < 4; ++j) if (z[j] < LIT_AO_EMPTY) {
        float w = saturate(1 - max(0, farthest - z[j] - from) / falloff);
        sum += w * z[j]; weight += w;
    }
    return sum / weight;
}
uint2 lit_ao_level_size(uint level) { return max(lit_size_frame.xy >> level, 1); }
groupshared float lit_ao_depth_tile[64];
// One 8x8 group owns a 16x16 window block and writes all five levels of it.
[numthreads(8, 8, 1)]
void lit_gtao_depth_cs(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID, uint lane : SV_GroupIndex) {
    uint2 base = group.xy * 16 + local.xy * 2;
    float4 z = float4(lit_ao_window_depth(base), lit_ao_window_depth(base + uint2(1, 0)),
                      lit_ao_window_depth(base + uint2(0, 1)), lit_ao_window_depth(base + uint2(1, 1)));
    [unroll] for (uint i = 0; i < 4; ++i) {
        uint2 q = base + uint2(i & 1, i >> 1);
        if (all(q < lit_size_frame.xy)) lit_ao_depth_output0[q] = z[i];
    }
    float reduced = lit_ao_reduce(z);
    uint2 q1 = group.xy * 8 + local.xy;
    if (all(q1 < lit_ao_level_size(1))) lit_ao_depth_output1[q1] = reduced;
    lit_ao_depth_tile[lane] = reduced;
    GroupMemoryBarrierWithGroupSync();
    if (all(local.xy % 2 == 0)) {
        reduced = lit_ao_reduce(float4(lit_ao_depth_tile[lane], lit_ao_depth_tile[lane + 1], lit_ao_depth_tile[lane + 8], lit_ao_depth_tile[lane + 9]));
        uint2 q2 = group.xy * 4 + local.xy / 2;
        if (all(q2 < lit_ao_level_size(2))) lit_ao_depth_output2[q2] = reduced;
    }
    GroupMemoryBarrierWithGroupSync();
    if (all(local.xy % 2 == 0)) lit_ao_depth_tile[lane] = reduced;
    GroupMemoryBarrierWithGroupSync();
    if (all(local.xy % 4 == 0)) {
        reduced = lit_ao_reduce(float4(lit_ao_depth_tile[lane], lit_ao_depth_tile[lane + 2], lit_ao_depth_tile[lane + 16], lit_ao_depth_tile[lane + 18]));
        uint2 q3 = group.xy * 2 + local.xy / 4;
        if (all(q3 < lit_ao_level_size(3))) lit_ao_depth_output3[q3] = reduced;
    }
    GroupMemoryBarrierWithGroupSync();
    if (all(local.xy % 4 == 0)) lit_ao_depth_tile[lane] = reduced;
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0) {
        reduced = lit_ao_reduce(float4(lit_ao_depth_tile[0], lit_ao_depth_tile[4], lit_ao_depth_tile[32], lit_ao_depth_tile[36]));
        if (all(group.xy < lit_ao_level_size(4))) lit_ao_depth_output4[group.xy] = reduced;
    }
}
float2 lit_ao_ndc(float2 uv) {
    return ((uv * lit_size_frame.xy - lit_viewport.xy) / lit_viewport.zw) * float2(2, -2) + float2(-1, 1);
}
float3 lit_ao_view(float2 uv, float z) {
    float2 ndc = lit_ao_ndc(uv);
    return float3(ndc.x * (z * lit_ao_view_x.x + lit_ao_view_x.y) + z * lit_ao_view_x.z + lit_ao_view_x.w,
                  ndc.y * (z * lit_ao_view_y.x + lit_ao_view_y.y) + z * lit_ao_view_y.z + lit_ao_view_y.w, z);
}
// Same integration as before, in view space: every tap is one point load from
// the level matching its distance plus a few multiply-adds, not a matrix inverse.
[numthreads(8, 8, 1)]
void lit_gtao_cs(uint3 id : SV_DispatchThreadID) {
    uint2 p;
    if (!lit_screen_pixel(id.xy, p)) return;
    if (!lit_post_surface(p) || lit_controls.z <= 0 || (lit_effect_flags & 4) == 0) { lit_ao_output[p] = 1; return; }
    float2 uv = lit_post_uv(p);
    float z = lit_ao_depth.Load(int3(p, 0));
    float2 ndc = lit_ao_ndc(uv);
    // Toward the camera along this pixel's ray, for perspective and orthographic projections.
    float3 view_direction = -normalize(float3(ndc.x * lit_ao_view_x.x + lit_ao_view_x.z, ndc.y * lit_ao_view_y.x + lit_ao_view_y.z, 1));
    float3 normal = lit_safe_normal(mul(float4(lit_safe_normal(lit_normal_roughness.Load(int3(p, 0)).xyz), 0), lit_camera_view).xyz);
    float radius = lit_controls.z;
    float2 pixel_axes = abs(float2(z * lit_ao_view_x.x + lit_ao_view_x.y, z * lit_ao_view_y.x + lit_ao_view_y.y)) * 2 / lit_viewport.zw;
    float pixel_size = max(max(pixel_axes.x, pixel_axes.y), 1e-6);
    float pixel_radius = clamp(radius / pixel_size, 1, 64);
    float3 surface = lit_ao_view(uv, z);
    // Bias only the horizon origin; stored low-poly surface normals stay intact.
    float3 center = surface + view_direction * pixel_size * .05;
    uint seed = lit_hash(p.x + p.y * lit_size_frame.x);
    float rotation = frac(lit_random(seed) + (lit_size_frame.z % 1024) * .618033989);
    float step_noise = frac(lit_random(seed) + (lit_size_frame.z % 1024) * .754877666);
    float visibility = 0;
    [unroll] for (uint slice = 0; slice < 3; ++slice) {
        float phi = (slice + rotation) * (LIT_PI / 3);
        float2 screen_direction = float2(cos(phi), sin(phi));
        float3 tangent = lit_ao_view(uv + screen_direction / lit_size_frame.xy, z) - surface;
        tangent = lit_safe_normal(tangent - view_direction * dot(tangent, view_direction));
        float3 slice_axis = lit_safe_normal(cross(tangent, view_direction));
        float3 projected_normal = normal - slice_axis * dot(normal, slice_axis);
        float projected_length = length(projected_normal);
        if (projected_length < 1e-5) continue;
        float normal_angle = atan2(dot(projected_normal, tangent), dot(projected_normal, view_direction));
        // These are the two unoccluded tangent-plane boundaries. Searching
        // maximum cosine gives the nearest horizon on each side of the view.
        float2 baseline = float2(cos(normal_angle - LIT_PI * .5), cos(normal_angle + LIT_PI * .5));
        float2 horizons = baseline;
        [unroll] for (uint step = 0; step < 4; ++step) {
            float fraction = (step + .5 + .5 * frac(step_noise + slice * .618033989)) / 4;
            float offset = max(1.25, fraction * fraction * pixel_radius);
            // XeGTAO's level choice: full resolution up to ~14 px, then one
            // level per octave of tap distance.
            uint level = uint(clamp(round(log2(offset) - 3.3), 0, 4));
            [unroll] for (uint side = 0; side < 2; ++side) {
                float sign_direction = side == 0 ? -1 : 1;
                float2 sample_uv = uv + sign_direction * screen_direction * offset / lit_size_frame.xy;
                if (!lit_post_inside_uv(sample_uv)) continue;
                uint2 sample_pixel = uint2(lit_post_pixel(sample_uv));
                float sample_z = lit_ao_depth.Load(int3(min(sample_pixel >> level, lit_ao_level_size(level) - 1), level));
                if (sample_z >= LIT_AO_EMPTY) continue;
                float3 delta = lit_ao_view(lit_post_uv(sample_pixel), sample_z) - center;
                float distance = length(delta);
                if (distance < 1e-6 || distance >= radius) continue;
                float horizon_cos = clamp(dot(delta / distance, view_direction), -1, 1);
                // Only the outer fifth fades; contact horizons retain the
                // analytic GTAO integral, and DDGI supplies the distant scale.
                float falloff = saturate((radius - distance) / max(radius * .2, 1e-6));
                horizons[side] = max(horizons[side], lerp(baseline[side], horizon_cos, falloff));
            }
        }
        float negative_horizon = max(-acos(clamp(horizons.x, -1, 1)), normal_angle - LIT_PI * .5);
        float positive_horizon = min(acos(clamp(horizons.y, -1, 1)), normal_angle + LIT_PI * .5);
        visibility += projected_length * (lit_gtao_arc(negative_horizon, normal_angle) +
                                          lit_gtao_arc(positive_horizon, normal_angle));
    }
    lit_ao_output[p] = saturate(visibility / 3);
}

// Logical level zero reads the original FP32 depth. Each reduced level is a
// separate RG32F image; no image is bound as both an SRV and UAV.
void lit_reduce_depth(uint2 p, bool device_source) {
    if (any(p >= lit_depth_output_size)) return;
    uint width, height;
    if (device_source) { width = lit_size_frame.x; height = lit_size_frame.y; }
    else lit_depth_input.GetDimensions(width, height);
    float2 range = float2(1, 0);
    [unroll] for (uint y = 0; y < 2; ++y) [unroll] for (uint x = 0; x < 2; ++x) {
        uint2 q = p * 2 + uint2(x, y);
        if (q.x >= width || q.y >= height) continue;
        float2 sample_range;
        if (device_source) {
            float z = lit_inside(q) ? lit_depth.Load(int3(q, 0)) : lit_depth_far_value();
            sample_range = float2(z, z);
        } else sample_range = lit_depth_input.Load(int3(q, 0));
        range.x = min(range.x, sample_range.x);
        range.y = max(range.y, sample_range.y);
    }
    lit_depth_output[p] = range;
}
[numthreads(8, 8, 1)]
void lit_depth_first_cs(uint3 id : SV_DispatchThreadID) { lit_reduce_depth(id.xy, true); }
[numthreads(8, 8, 1)]
void lit_depth_pyramid_cs(uint3 id : SV_DispatchThreadID) { lit_reduce_depth(id.xy, false); }

float lit_post_smith_g1(float cosine, float alpha_squared) {
    return 2 * cosine / max(cosine + sqrt(alpha_squared + (1 - alpha_squared) * cosine * cosine), 1e-6);
}
float lit_post_smith_g2(float nv, float nl, float alpha_squared) {
    float v = sqrt(alpha_squared + (1 - alpha_squared) * nv * nv);
    float l = sqrt(alpha_squared + (1 - alpha_squared) * nl * nl);
    return 2 * nv * nl / max(nl * v + nv * l, 1e-6);
}
// Heitz's ellipsoid/projected-disk construction, written against our isotropic
// roughness convention: perceptual roughness^2 is the GGX alpha parameter.
float3 lit_post_visible_ggx(float3 view_local, float alpha, float2 random) {
    float3 stretched_view = lit_safe_normal(float3(view_local.xy * alpha, view_local.z));
    float3 axis_x = abs(stretched_view.z) < .99999 ?
        lit_safe_normal(float3(-stretched_view.y, stretched_view.x, 0)) : float3(1, 0, 0);
    float3 axis_y = cross(stretched_view, axis_x);
    float phi = random.y * (2 * LIT_PI);
    float2 disk = sqrt(random.x) * float2(cos(phi), sin(phi));
    float visible_fraction = (1 + stretched_view.z) * .5;
    disk.y = lerp(sqrt(max(0, 1 - disk.x * disk.x)), disk.y, visible_fraction);
    float3 hemisphere = disk.x * axis_x + disk.y * axis_y +
        sqrt(max(0, 1 - dot(disk, disk))) * stretched_view;
    return lit_safe_normal(float3(alpha * hemisphere.xy, max(hemisphere.z, 0)));
}
float2 lit_post_brdf_integral(float nv, float roughness) {
    float alpha = max(roughness * roughness, .0025);
    float alpha_squared = alpha * alpha;
    float3 v = float3(sqrt(max(0, 1 - nv * nv)), 0, nv);
    float2 integral = 0;
    [loop] for (uint sample_id = 0; sample_id < 128; ++sample_id) {
        float2 random = float2((sample_id + .5) / 128, reversebits(sample_id) * 2.3283064365386963e-10);
        float3 h = lit_post_visible_ggx(v, alpha, random);
        float3 l = reflect(-v, h);
        if (l.z <= 0) continue;
        float weight = lit_post_smith_g2(nv, l.z, alpha_squared) /
                       max(lit_post_smith_g1(nv, alpha_squared), 1e-6);
        float fresnel = pow(1 - saturate(dot(v, h)), 5);
        integral += float2(1 - fresnel, fresnel) * weight;
    }
    return integral / 128;
}
[numthreads(8, 8, 1)]
void lit_brdf_lut_cs(uint3 id : SV_DispatchThreadID) {
    uint width, height;
    lit_brdf_output.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    float2 uv = (id.xy + .5) / float2(width, height);
    lit_brdf_output[id.xy] = lit_post_brdf_integral(max(uv.x, 1e-4), uv.y);
}
float3 lit_post_environment(uint2 p) {
    float4 nr = lit_normal_roughness.Load(int3(p, 0));
    float4 bm = lit_base_metallic.Load(int3(p, 0));
    float nv = saturate(dot(lit_safe_normal(nr.xyz), lit_view_direction(lit_post_uv(p))));
    float2 integrated_brdf = lit_brdf_lut.SampleLevel(lit_linear_sampler, float2(nv, nr.w), 0);
    float3 f0 = lerp(.04.xxx, bm.rgb, saturate(bm.w));
    // First-batch environment is spatially uniform RGB radiance. Its normalized
    // GGX prefilter is EXACTLY the same constant at every direction/roughness;
    // the generated LUT retains roughness-dependent masking and Fresnel energy.
    return lit_post_finite(lit_environment.rgb * (f0 * integrated_brdf.x + integrated_brdf.y));
}
float3 lit_reflection_finite(float3 value) {
    // A control-variate observation can be negative even though its integral
    // is physical. Keep that sign through reconstruction; only the final HDR
    // combination uses the nonnegative color boundary. Bound FP16 storage.
    return all(isfinite(value)) ? clamp(value, -65000, 65000) : 0;
}
bool lit_reflection_same_material(uint2 p, int2 q) {
    // This signal is already receiver-material modulated. Base and metallic
    // uniquely determine F0; exact stored parameters keep adjacent flat-color
    // low-poly materials separate without another arbitrary color threshold.
    return all(lit_base_metallic.Load(int3(p, 0)) == lit_base_metallic.Load(int3(q, 0)));
}
[numthreads(1, 1, 1)]
void lit_ssr_reset_cs(uint3 id : SV_DispatchThreadID) { lit_reflection_counter[0] = 0; }
bool lit_ssr_eligible(uint2 p, float4 nr) {
    return (lit_effect_flags & 8) != 0 && nr.w <= lit_temporal.y && lit_controls.w > 0 &&
        dot(lit_safe_normal(nr.xyz), lit_view_direction(lit_post_uv(p))) > .005;
}
[numthreads(8, 8, 1)]
void lit_ssr_classify_cs(uint3 id : SV_DispatchThreadID) {
    uint2 p = id.xy;
    if (any(p >= lit_size_frame.xy)) return;
    lit_reflection_hit_trace_output[p] = 0;
    if (!lit_post_surface(p)) { lit_reflection_output[p] = 0; return; }
    lit_reflection_output[p] = float4(lit_post_environment(p), 0);
    float4 nr = lit_normal_roughness.Load(int3(p, 0));
    // Compact only pixels with a visible, sufficiently smooth specular lobe.
    // Work allocation has a strict one-entry-per-window-pixel capacity contract.
    if (!lit_ssr_eligible(p, nr)) return;
    uint index;
    InterlockedAdd(lit_reflection_counter[0], 1, index);
    lit_reflection_work[index] = p.y * lit_size_frame.x + p.x;
}
[numthreads(1, 1, 1)]
void lit_ssr_prepare_cs(uint3 id : SV_DispatchThreadID) {
    lit_reflection_arguments[0] = (lit_reflection_counter[0] + 63) / 64;
    lit_reflection_arguments[1] = 1;
    lit_reflection_arguments[2] = 1;
}

float3 lit_post_project(float3 world) {
    float4 clip = mul(float4(world, 1), lit_view_projection);
    return float3(lit_window_uv(clip), clip.z / clip.w);
}
float2 lit_ssr_cell_range(float2 uv, uint level, out float2 grid_size) {
    uint2 size = lit_size_frame.xy;
    if (level != 0) lit_depth_pyramid[NonUniformResourceIndex(level - 1)].GetDimensions(size.x, size.y);
    // Ceil-half images retain two-parent-pixels per cell. Their UV boundaries
    // must follow that footprint instead of stretching odd-sized last cells.
    grid_size = float2(lit_size_frame.xy) / float(1u << level);
    int2 cell = clamp(int2(floor(uv * grid_size)), int2(0, 0), int2(size) - 1);
    if (level == 0) {
        float z = lit_inside(uint2(cell)) ? lit_depth.Load(int3(cell, 0)) : lit_depth_far_value();
        return float2(z, z);
    }
    return lit_depth_pyramid[NonUniformResourceIndex(level - 1)].Load(int3(cell, 0));
}
float lit_ssr_cell_exit(float2 origin, float2 direction, float time, float2 grid_size) {
    float2 position = origin + direction * time;
    float2 cell = floor(position * grid_size);
    float2 boundary = (cell + float2(direction.x >= 0 ? 1 : 0, direction.y >= 0 ? 1 : 0)) / grid_size;
    float tx = abs(direction.x) > 1e-9 ? (boundary.x - origin.x) / direction.x : 1e30;
    float ty = abs(direction.y) > 1e-9 ? (boundary.y - origin.y) / direction.y : 1e30;
    return max(time + 1e-7, min(tx, ty));
}
bool lit_ssr_intersect(float3 world_origin, float3 ray, float3 view_direction,
                       out float4 hit, out float confidence) {
    hit = 0; confidence = 0;
    float max_distance = lit_controls.w;
    float4 start_clip = mul(float4(world_origin, 1), lit_view_projection);
    float4 end_clip = mul(float4(world_origin + ray * max_distance, 1), lit_view_projection);
    if (start_clip.w <= 1e-6) return false;
    // A segment crossing the perspective near plane must be clipped before
    // division; orthographic w is constant, so this branch has no assumptions.
    if (end_clip.w <= 1e-5) {
        float fraction = (start_clip.w - 1e-5) / max(start_clip.w - end_clip.w, 1e-6);
        max_distance *= saturate(fraction) * .999;
        end_clip = mul(float4(world_origin + ray * max_distance, 1), lit_view_projection);
    }
    float3 origin = float3(lit_window_uv(start_clip), start_clip.z / start_clip.w);
    float3 end = float3(lit_window_uv(end_clip), end_clip.z / end_clip.w);
    float3 direction = end - origin;
    if (!all(isfinite(direction)) || length(direction.xy * lit_size_frame.xy) < 1) return false;
    float2 viewport_min = (lit_viewport.xy + .5) / lit_size_frame.xy;
    float2 viewport_max = (lit_viewport.xy + lit_viewport.zw - .5) / lit_size_frame.xy;
    float end_time = 1;
    [unroll] for (uint axis = 0; axis < 2; ++axis) {
        if (direction[axis] > 1e-9) end_time = min(end_time, (viewport_max[axis] - origin[axis]) / direction[axis]);
        if (direction[axis] < -1e-9) end_time = min(end_time, (viewport_min[axis] - origin[axis]) / direction[axis]);
    }
    if (direction.z > 1e-9) end_time = min(end_time, (1 - origin.z) / direction.z);
    if (direction.z < -1e-9) end_time = min(end_time, -origin.z / direction.z);
    if (end_time <= 0 || lit_depth_level_count == 0) return false;
    float world_thickness = max(.025, lit_controls.y * 4);
    float3 thickness_projected = lit_post_project(world_origin + view_direction * world_thickness);
    float depth_thickness = clamp(abs(thickness_projected.z - origin.z), 1e-6, .02);
    uint highest_level = min(lit_depth_level_count, 12u) - 1;
    uint level = min(highest_level, 3u);
    float time = max(1.5 / max(length(direction.xy * lit_size_frame.xy), 1), 1e-6);
    [loop] for (uint iteration = 0; iteration < 96 && time < end_time; ++iteration) {
        float3 position = origin + direction * time;
        float2 grid_size;
        float2 range = lit_ssr_cell_range(position.xy, level, grid_size);
        float exit_time = min(end_time, lit_ssr_cell_exit(origin.xy, direction.xy, time, grid_size));
        float next_z = origin.z + direction.z * exit_time;
        float ray_min = min(position.z, next_z), ray_max = max(position.z, next_z);
        bool overlap = !lit_is_background_depth(range.y) && ray_max >= range.x - depth_thickness &&
                       ray_min <= range.y + depth_thickness;
        if (overlap && level > 0) { --level; continue; }
        if (overlap) {
            float candidate_time = abs(direction.z) > 1e-9 ? (range.y - origin.z) / direction.z : time;
            candidate_time = clamp(candidate_time, time, exit_time);
            float3 candidate = origin + direction * candidate_time;
            if (lit_post_inside_uv(candidate.xy)) {
                int2 q = lit_post_pixel(candidate.xy);
                float z = lit_depth.Load(int3(q, 0));
                float3 surface = lit_world(candidate.xy, z);
                float along_ray = dot(surface - world_origin, ray);
                float error = length(surface - world_origin - ray * along_ray);
                float thickness = world_thickness + along_ray * .002;
                float3 hit_normal = lit_safe_normal(lit_normal_roughness.Load(int3(q, 0)).xyz);
                if (!lit_is_background_depth(z) && along_ray > max(lit_controls.y * 8, .04) &&
                    along_ray < max_distance && error < thickness && dot(hit_normal, -ray) > .025) {
                    float2 local = (candidate.xy * lit_size_frame.xy - lit_viewport.xy) / lit_viewport.zw;
                    float edge = min(min(local.x, local.y), min(1 - local.x, 1 - local.y));
                    confidence = saturate(edge * 12) * (1 - smoothstep(.75, 1, along_ray / max_distance)) *
                                 (1 - smoothstep(.25, 1, error / thickness));
                    hit = float4(candidate.xy, z, along_ray);
                    return confidence > 0;
                }
            }
        }
        // Empty cells advance to their exact projected boundary; occupied cells
        // descend to pixel depth. Both extrema participate in every rejection.
        time = exit_time + max(1e-7, 1e-3 / max(length(direction.xy * lit_size_frame.xy), 1));
        level = min(level + 1, highest_level);
    }
    return false;
}
[numthreads(64, 1, 1)]
void lit_ssr_trace_cs(uint3 id : SV_DispatchThreadID) {
    if (id.x >= lit_reflection_counter[0]) return;
    uint packed = lit_reflection_work[id.x];
    uint2 p = uint2(packed % lit_size_frame.x, packed / lit_size_frame.x);
    float2 uv = lit_post_uv(p);
    float4 nr = lit_normal_roughness.Load(int3(p, 0));
    float4 bm = lit_base_metallic.Load(int3(p, 0));
    float3 n = lit_safe_normal(nr.xyz), v = lit_view_direction(uv);
    float3 tangent, bitangent; lit_basis(n, tangent, bitangent);
    float3 local_v = float3(dot(v, tangent), dot(v, bitangent), max(dot(v, n), 1e-4));
    uint seed = lit_hash(packed);
    float alpha = max(nr.w * nr.w, .0025);
    // Scrambled R2 samples cover the lobe over time rather than repeatedly
    // drawing independent frame hashes. Surface history handles disocclusion.
    float2 sample = frac(float2(lit_random(seed), lit_random(seed)) +
                        (lit_size_frame.z % 1024) * float2(.754877666, .569840296));
    float3 local_h = lit_post_visible_ggx(local_v, alpha, sample);
    float3 h = local_h.x * tangent + local_h.y * bitangent + local_h.z * n;
    float3 ray = lit_safe_normal(reflect(-v, h));
    float nl = saturate(dot(n, ray)), nv = max(saturate(dot(n, v)), 1e-4);
    if (nl <= 0 || dot(lit_geometry_normal_at(p),ray)<=0) return;
    float3 origin = lit_post_world(p) + lit_geometry_normal_at(p) * max(lit_controls.y * 2, .005);
    float4 hit; float confidence;
    if (!lit_ssr_intersect(origin, ray, v, hit, confidence)) return;
    int2 q = lit_post_pixel(hit.xy);
    float3 incoming = lit_post_finite(lit_hdr_source.Load(int3(q, 0)).rgb);
    float3 f0 = lerp(.04.xxx, bm.rgb, saturate(bm.w));
    float3 fresnel = f0 + (1 - f0) * pow(1 - saturate(dot(v, h)), 5);
    float weight = lit_post_smith_g2(nv, nl, alpha * alpha) /
                   max(lit_post_smith_g1(nv, alpha * alpha), 1e-6);
    // Control-variate estimator: replace only the sampled environment term.
    // HDR source contains no final reflection, UI, grid or display exposure.
    float3 reflection = lit_post_environment(p) +
        confidence * (incoming - lit_environment.rgb) * fresnel * weight;
    lit_reflection_output[p] = float4(lit_reflection_finite(reflection), confidence);
    lit_reflection_hit_trace_output[p] = lit_reflection_pack_hit(hit);
}

float lit_post_previous_depth_tolerance(float3 position, float3 view_direction) {
    float4 a = mul(float4(position, 1), lit_previous_view_projection);
    float4 b = mul(float4(position + view_direction * .035, 1), lit_previous_view_projection);
    if (abs(a.w) < 1e-6 || abs(b.w) < 1e-6) return 0;
    return max(abs(a.z / a.w) * (8 * 1.192092896e-7), abs(a.z / a.w - b.z / b.w));
}
float2 lit_post_previous_depth_gradient(float3 position, float3 normal) {
    // Device depth is affine in projected XY for a plane. Compute that plane's
    // gradient in the PREVIOUS image, including its actual viewport/projection.
    // Comparing every bilinear tap against the center depth would reject a
    // perfectly static tilted floor even with accurate motion-depth encoding.
    float3 tangent, bitangent; lit_basis(normal, tangent, bitangent);
    float4 a = mul(float4(position, 1), lit_previous_view_projection);
    float4 b = mul(float4(position + tangent, 1), lit_previous_view_projection);
    float4 c = mul(float4(position + bitangent, 1), lit_previous_view_projection);
    if (abs(a.w) < 1e-6 || abs(b.w) < 1e-6 || abs(c.w) < 1e-6) return 0;
    float2 ab = lit_window_uv(b) - lit_window_uv(a);
    float2 ac = lit_window_uv(c) - lit_window_uv(a);
    float db = b.z / b.w - a.z / a.w, dc = c.z / c.w - a.z / a.w;
    float determinant = ab.x * ac.y - ab.y * ac.x;
    if (abs(determinant) < 1e-16) return 0;
    float2 gradient = float2(db * ac.y - dc * ab.y, dc * ab.x - db * ac.x) / determinant;
    return all(isfinite(gradient)) ? gradient : 0;
}
bool lit_post_valid_history(uint2 current_pixel, float2 previous_uv, float expected_depth, float3 current_normal,
                             float depth_tolerance, float2 depth_gradient) {
    if (lit_size_frame.w == 0 || !lit_post_inside_uv(previous_uv)) return false;
    int2 q = lit_post_pixel(previous_uv);
    if (!lit_history_surface_matches(current_pixel, uint2(q))) return false;
    float previous_depth = lit_history_depth.Load(int3(q, 0));
    float3 previous_normal = lit_safe_normal(lit_history_normal.Load(int3(q, 0)).xyz);
    float tap_depth = expected_depth + dot(lit_post_uv(q) - previous_uv, depth_gradient);
    return !lit_is_background_depth(previous_depth) && abs(previous_depth - tap_depth) <= depth_tolerance &&
           dot(current_normal, previous_normal) >= .94;
}
bool lit_post_sample_history(Texture2D<float4> source, uint2 p, float2 previous_uv, float expected_depth,
                              float3 normal, float tolerance, float2 depth_gradient, out float4 history) {
    history = 0;
    if (lit_size_frame.w == 0 || !lit_post_inside_uv(previous_uv)) return false;
    float2 previous_pixel = previous_uv * lit_size_frame.xy - .5;
    int2 base_pixel = int2(floor(previous_pixel)); float2 fraction = frac(previous_pixel);
    float weight_sum = 0;
    [unroll] for (int y = 0; y < 2; ++y) [unroll] for (int x = 0; x < 2; ++x) {
        int2 q = base_pixel + int2(x, y);
        if (any(q < 0) || any(q >= int2(lit_size_frame.xy))) continue;
        float2 q_uv = lit_post_uv(q);
        float tap_depth = expected_depth + dot(q_uv - previous_uv, depth_gradient);
        if (!lit_post_valid_history(p, q_uv, tap_depth, normal, tolerance, depth_gradient)) continue;
        float weight = (x == 0 ? 1 - fraction.x : fraction.x) * (y == 0 ? 1 - fraction.y : fraction.y);
        history += source.Load(int3(q, 0)) * weight; weight_sum += weight;
    }
    if (weight_sum < .1) return false;
    history /= weight_sum;
    return true;
}
float lit_post_response(float2 motion) {
    // Lighting changes shorten useful support in each signal; they do not
    // cancel every pixel's temporal reconstruction on a moving-light frame.
    float motion_pixels = length(motion * lit_size_frame.xy);
    return lerp(1, .65, saturate(motion_pixels / 12));
}
bool lit_post_reflection_history(uint2 p, float2 previous_uv, float expected_depth, float3 normal,
                                 float roughness, float tolerance, float2 gradient,
                                 out float4 signal, out float2 moments, out float support) {
    signal = 0; moments = 0; support = 0;
    if (lit_size_frame.w == 0 || !lit_post_inside_uv(previous_uv)) return false;
    float2 pixel = previous_uv * lit_size_frame.xy - .5;
    int2 base = int2(floor(pixel)); float2 fraction = frac(pixel);
    [unroll] for (int y = 0; y < 2; ++y) [unroll] for (int x = 0; x < 2; ++x) {
        int2 q = base + int2(x, y);
        if (any(q < 0) || any(q >= int2(lit_size_frame.xy))) continue;
        float2 q_uv = lit_post_uv(q);
        float tap_depth = expected_depth + dot(q_uv - previous_uv, gradient);
        if (!lit_post_valid_history(p, q_uv, tap_depth, normal, tolerance, gradient) ||
            abs(lit_history_normal.Load(int3(q, 0)).w - roughness) > .08) continue;
        float4 sample = lit_reflection_previous.Load(int3(q, 0));
        float2 sample_moments = lit_reflection_moments_previous.Load(int3(q, 0));
        if (sample.a < 1 || !all(isfinite(sample)) || !all(isfinite(sample_moments))) continue;
        float weight = (x == 0 ? 1 - fraction.x : fraction.x) * (y == 0 ? 1 - fraction.y : fraction.y);
        signal += sample * weight; moments += sample_moments * weight; support += weight;
    }
    if (support < .1) return false;
    signal /= support; moments /= support;
    return true;
}
float lit_post_geometry_motion(uint2 p) {
    // Compare actual correspondence against the same WORLD point held still.
    // Camera motion and projection jitter therefore do not count as object motion.
    float4 motion = lit_motion.Load(int3(p, 0));
    if (motion.w <= .5) return 1;
    float3 position = lit_post_world(p);
    float4 stationary = mul(float4(position, 1), lit_previous_view_projection);
    if (stationary.w <= 1e-6) return 1;
    float error_pixels = length((lit_post_uv(p) + motion.xy - lit_window_uv(stationary)) * lit_size_frame.xy);
    float tolerance = lit_post_previous_depth_tolerance(position, lit_view_direction(lit_post_uv(p)));
    float depth_error = abs(stationary.z / stationary.w - lit_previous_device_depth(p, motion));
    return max(smoothstep(.25, 2, error_pixels), saturate(depth_error / max(tolerance, 1e-8) - 1));
}
bool lit_post_previous_hit_present(float2 hit, out float source_motion, out float2 current_hit) {
    source_motion = 0;
    current_hit = 0;
    if (all(hit == 0)) return true;
    if (!lit_post_inside_uv(hit)) return false;
    int2 old_pixel = lit_post_pixel(hit);
    // Reconstruct the continuous anchor on the previous source plane from R32
    // depth. Repeated snapping to texel centers drifts under the two T2x phases.
    float depth = lit_history_depth.Load(int3(old_pixel, 0));
    if (lit_is_background_depth(depth)) return false;
    float2 local = (lit_post_uv(old_pixel) * lit_size_frame.xy - lit_viewport.xy) / lit_viewport.zw;
    float4 previous_world = mul(float4(local * float2(2, -2) + float2(-1, 1), depth, 1),
                                 lit_previous_inverse_view_projection);
    float3 position = previous_world.xyz / previous_world.w;
    float3 old_normal = lit_safe_normal(lit_history_normal.Load(int3(old_pixel, 0)).xyz);
    depth += dot(hit - lit_post_uv(old_pixel), lit_post_previous_depth_gradient(position,lit_decode_normal(lit_history_geometry_normal.Load(int3(old_pixel,0)))));
    local = (hit * lit_size_frame.xy - lit_viewport.xy) / lit_viewport.zw;
    previous_world = mul(float4(local * float2(2, -2) + float2(-1, 1), depth, 1), lit_previous_inverse_view_projection);
    position = previous_world.xyz / previous_world.w;
    float4 clip = mul(float4(position, 1), lit_view_projection);
    if (clip.w <= 1e-6 || !lit_post_inside_uv(lit_window_uv(clip))) return false;
    int2 current_pixel = lit_post_pixel(lit_window_uv(clip));
    if (!lit_post_surface(current_pixel)) return false;
    // The source can retain identical geometry while its material/emission or
    // object identity changes. Geometry alone cannot validate its old radiance.
    if (!lit_history_surface_matches(uint2(current_pixel), uint2(old_pixel))) return false;
    float3 normal = lit_safe_normal(lit_normal_roughness.Load(int3(current_pixel, 0)).xyz);
    float tolerance = max(.035, lit_post_pixel_world_size(lit_post_uv(current_pixel),
        lit_depth.Load(int3(current_pixel, 0))) * .5);
    if (dot(normal, old_normal) < .94 || abs(dot(lit_post_world(current_pixel)-position,lit_geometry_normal_at(current_pixel))) > tolerance)
        return false;
    if (lit_temporal.w > 0) source_motion = lit_post_geometry_motion(current_pixel);
    current_hit = lit_window_uv(clip);
    return true;
}
// Each 8x8 group reconstructs its 12x12 neighborhood once; the 5x5 support
// then reads world positions, normals and raw AO from LDS with unchanged math.
groupshared float4 lit_ao_tile_position[144]; // world xyz, raw AO; w < 0 marks a non-surface
groupshared float3 lit_ao_tile_normal[144];
[numthreads(8, 8, 1)]
void lit_gtao_resolve_cs(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID, uint lane : SV_GroupIndex) {
    int2 tile_origin = int2(lit_screen_dispatch.xy + group.xy * 8) - 2;
    [unroll] for (uint i = 0; i < 3; ++i) {
        uint index = lane + i * 64;
        if (index < 144) {
            int2 q = tile_origin + int2(index % 12, index / 12);
            bool surface = all(q >= 0) && all(q < int2(lit_size_frame.xy)) && lit_post_surface(uint2(q));
            lit_ao_tile_position[index] = surface ? float4(lit_post_world(uint2(q)), saturate(lit_ao_raw.Load(int3(q, 0)))) : float4(0, 0, 0, -1);
            lit_ao_tile_normal[index] = surface ? lit_safe_normal(lit_normal_roughness.Load(int3(q, 0)).xyz) : 0;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    uint2 p;
    if (!lit_screen_pixel(group.xy * 8 + local.xy, p)) return;
    if (!lit_post_surface(p) || (lit_effect_flags & 4) == 0 || lit_controls.z <= 0) {
        lit_ao_resolved_output[p] = 1; lit_ao_history_output[p] = float2(1, 0); return;
    }
    float2 uv = lit_post_uv(p);
    uint center = (local.x + 2) + (local.y + 2) * 12;
    float3 position = lit_ao_tile_position[center].xyz;
    float3 normal = lit_ao_tile_normal[center];
    float3 geometric = lit_geometry_normal_at(p);
    float pixel_world = max(lit_post_pixel_world_size(uv, lit_depth.Load(int3(p, 0))), .001);
    float sum = 0, second = 0, weight_sum = 0;
    [unroll] for (int y = -2; y <= 2; ++y) [unroll] for (int x = -2; x <= 2; ++x) {
        uint index = center + x + y * 12;
        float4 q_position = lit_ao_tile_position[index];
        if (q_position.w < 0) continue;
        float normal_dot = dot(normal, lit_ao_tile_normal[index]);
        float plane_distance = abs(dot(q_position.xyz - position, geometric));
        if (normal_dot < .95 || plane_distance > max(.005, pixel_world * .5)) continue;
        float weight = pow(saturate(normal_dot), 32) * exp(-.35 * (x * x + y * y));
        float value = q_position.w;
        sum += value * weight; second += value * value * weight; weight_sum += weight;
    }
    float current = weight_sum > 0 ? sum / weight_sum : lit_ao_tile_position[center].w;
    float sigma = sqrt(max(0, second / max(weight_sum, 1e-6) - current * current));
    float4 motion = lit_motion.Load(int3(p, 0));
    float2 previous_uv = uv + motion.xy;
    float expected_depth = lit_previous_device_depth(p, motion);
    float tolerance = lit_post_previous_depth_tolerance(position, lit_view_direction(uv));
    float2 gradient = lit_post_previous_depth_gradient(position,lit_geometry_normal_at(p));
    float2 history = 0; float support = 0;
    if ((lit_history_invalid & 4) == 0 && lit_size_frame.w != 0 && motion.w > .5 && lit_post_inside_uv(previous_uv)) {
        float2 pixel = previous_uv * lit_size_frame.xy - .5;
        int2 base = int2(floor(pixel)); float2 fraction = frac(pixel);
        [unroll] for (int y = 0; y < 2; ++y) [unroll] for (int x = 0; x < 2; ++x) {
            int2 q = base + int2(x, y);
            if (any(q < 0) || any(q >= int2(lit_size_frame.xy))) continue;
            float2 q_uv = lit_post_uv(q);
            if (!lit_post_valid_history(p, q_uv, expected_depth + dot(q_uv - previous_uv, gradient),
                                        normal, tolerance, gradient)) continue;
            float2 sample = lit_ao_previous.Load(int3(q, 0));
            if (sample.y < 1 || !all(isfinite(sample))) continue;
            float weight = (x == 0 ? 1 - fraction.x : fraction.x) * (y == 0 ? 1 - fraction.y : fraction.y);
            history += sample * weight; support += weight;
        }
    }
    float count = 1;
    if (support >= .1) {
        history /= support;
        float previous_count = min(history.y * support * lit_post_response(motion.xy), 31);
        // A newly revealed/moving occluder can change AO on an unchanged plane.
        // Bound that local disagreement without reacting to unrelated lights.
        if (abs(history.x - current) > max(.15, sigma * 3)) previous_count = min(previous_count, 4);
        float history_weight = previous_count / (previous_count + 1);
        float clipped = clamp(history.x, current - max(.06, sigma * 3), current + max(.06, sigma * 3));
        current = lerp(current, clipped, history_weight); count = previous_count + 1;
    }
    current = saturate(current);
    lit_ao_resolved_output[p] = current; lit_ao_history_output[p] = float2(current, count);
}
[numthreads(8, 8, 1)]
void lit_reflection_resolve_cs(uint3 id : SV_DispatchThreadID) {
    uint2 p = id.xy;
    if (any(p >= lit_size_frame.xy)) return;
    float4 center = lit_reflection_trace.Load(int3(p, 0));
    float4 hit = lit_reflection_unpack_hit(lit_reflection_hit_trace.Load(int3(p, 0)));
    lit_reflection_hit_output[p] = hit.w > 0 ? hit.xy : 0;
    if (!lit_post_surface(p)) {
        lit_reflection_resolved_output[p] = 0; lit_reflection_moments_output[p] = 0; return;
    }
    float4 nr = lit_normal_roughness.Load(int3(p, 0));
    if (!lit_ssr_eligible(p, nr)) {
        // Environment-only rendering is deterministic and material-local;
        // neither spatial pooling nor stale SSR history participates.
        float3 environment = lit_post_environment(p);
        float luma = lit_post_luminance(environment);
        lit_reflection_resolved_output[p] = float4(environment, 1);
        lit_reflection_moments_output[p] = float2(luma, luma * luma);
        lit_reflection_hit_output[p] = 0;
        return;
    }
    float2 uv = lit_post_uv(p);
    float3 normal = lit_safe_normal(nr.xyz), position = lit_post_world(p);
    float pixel_world = max(lit_post_pixel_world_size(uv, lit_depth.Load(int3(p, 0))), .001);
    float3 sum = center.rgb; float weight_sum = 1;
    float3 environment = lit_post_environment(p);
    float2 witness = hit.w > 0 ? hit.xy : 0;
    float witness_score = hit.w > 0 ? length(center.rgb - environment) : 0;
    float center_luma = lit_post_luminance(center.rgb);
    float2 current_moments = float2(center_luma, center_luma * center_luma);
    [unroll] for (int y = -1; y <= 1; ++y) [unroll] for (int x = -1; x <= 1; ++x) {
        if (x == 0 && y == 0) continue;
        int2 q = int2(p) + int2(x, y);
        if (any(q < 0) || any(q >= int2(lit_size_frame.xy)) || !lit_post_surface(q) ||
            !lit_reflection_same_material(p, q)) continue;
        float4 neighbor_normal = lit_normal_roughness.Load(int3(q, 0));
        if (!lit_ssr_eligible(q, neighbor_normal)) continue;
        if (dot(normal, lit_safe_normal(neighbor_normal.xyz)) < .96 ||
            abs(nr.w - neighbor_normal.w) > .08 ||
            abs(dot(lit_post_world(q)-position,lit_geometry_normal_at(p))) > max(.005, pixel_world * .5)) continue;
        float3 value = lit_reflection_trace.Load(int3(q, 0)).rgb;
        // Mirrors keep their current sharp sample. Rough lobes can pool nearby
        // independent stochastic samples without blending geometric hard edges.
        float weight = smoothstep(.04, .2, nr.w) / (1 + x * x + y * y);
        float score = length(value - environment) * weight;
        if (score > witness_score) {
            float4 candidate = lit_reflection_unpack_hit(lit_reflection_hit_trace.Load(int3(q, 0)));
            if (candidate.w > 0) { witness = candidate.xy; witness_score = score; }
        }
        sum += value * weight; weight_sum += weight;
        float luma = lit_post_luminance(value);
        current_moments += float2(luma, luma * luma) * weight;
    }
    float3 current = sum / weight_sum;
    current_moments /= weight_sum;
    float spatial_variance = max(0, current_moments.y - current_moments.x * current_moments.x);
    float4 motion = lit_motion.Load(int3(p, 0));
    float2 previous_uv = uv + motion.xy;
    float tolerance = lit_post_previous_depth_tolerance(position, lit_view_direction(uv));
    float2 gradient = lit_post_previous_depth_gradient(position,lit_geometry_normal_at(p));
    float expected_depth = lit_previous_device_depth(p, motion);
    float4 previous_signal; float2 previous_moments; float support;
    bool valid = (lit_history_invalid & 2) == 0 && (lit_effect_flags & 8) != 0 && motion.w > .5 &&
        lit_post_reflection_history(p, previous_uv, expected_depth, normal, nr.w, tolerance,
                                    gradient, previous_signal, previous_moments, support);
    float geometry_response = 0;
    float2 previous_witness = 0;
    if (valid && nr.w >= .04) {
        float source_motion;
        float2 previous_hit = lit_reflection_hit_previous.Load(int3(lit_post_pixel(previous_uv), 0));
        valid = lit_post_previous_hit_present(previous_hit, source_motion, previous_witness);
        if (lit_temporal.w > 0) geometry_response = max(source_motion, lit_post_geometry_motion(p));
    }
    if (valid && hit.w > 0) {
        int2 hit_pixel = lit_post_pixel(hit.xy);
        float4 hit_motion = lit_motion.Load(int3(hit_pixel, 0));
        float2 previous_hit_uv = hit.xy + hit_motion.xy;
        float3 hit_normal = lit_safe_normal(lit_normal_roughness.Load(int3(hit_pixel, 0)).xyz);
        float3 hit_position = lit_world(hit.xy, lit_depth.Load(int3(hit_pixel, 0)));
        float hit_tolerance = lit_post_previous_depth_tolerance(hit_position, lit_view_direction(hit.xy));
        float2 hit_gradient = lit_post_previous_depth_gradient(hit_position,lit_geometry_normal_at(hit_pixel));
        valid = hit_motion.w > .5 && lit_post_valid_history(uint2(hit_pixel), previous_hit_uv,
            lit_previous_device_depth(hit_pixel, hit_motion), hit_normal, hit_tolerance, hit_gradient);
        if (lit_temporal.w > 0) geometry_response = max(geometry_response, lit_post_geometry_motion(hit_pixel));
        // Independent GGX rays estimate one rough-lobe integral and need not
        // hit the same pixel or even share hit/miss status. Reproject the CURRENT
        // hit to reject real disocclusion. Only near-delta lobes require the
        // previous ray association in addition to surface correspondence.
        if (valid && nr.w < .04) {
            float2 previous_hit = lit_reflection_hit_previous.Load(int3(lit_post_pixel(previous_uv), 0));
            float hit_uv_error = length((previous_hit - previous_hit_uv) * lit_size_frame.xy);
            valid = any(previous_hit != 0) && hit_uv_error < 4 &&
                lit_post_valid_history(uint2(hit_pixel), previous_hit, lit_previous_device_depth(hit_pixel, hit_motion) +
                    dot(previous_hit - previous_hit_uv, hit_gradient), hit_normal, hit_tolerance, hit_gradient);
        }
    } else if (valid && nr.w < .04) {
        valid = all(lit_reflection_hit_previous.Load(int3(lit_post_pixel(previous_uv), 0)) == 0);
    }
    float3 result = current;
    float count = 1;
    float retained_contribution = 0;
    float2 result_moments = current_moments;
    if (valid) {
        float3 previous = previous_signal.rgb;
        float temporal_variance = max(0, previous_moments.y - previous_moments.x * previous_moments.x);
        float sigma = sqrt(max(spatial_variance, temporal_variance));
        float previous_count = min(previous_signal.a * support * lit_post_response(motion.xy),
                                   lerp(63, 23, saturate(lit_temporal.z)));
        previous_count = min(previous_count, lerp(63, 2, geometry_response));
        float previous_luma = lit_post_luminance(previous);
        // Keep radiance variance across independent rough-lobe hits and misses.
        // Every moment and clipping extent uses the SAME Rec709 scalar units.
        float range = max(sigma * 4, .01);
        // Project TOWARD the current RGB mean. An equal RGB translation can
        // turn stale red into red-minus-cyan (or create white in a red-only
        // scene); the final nonnegative HDR boundary then exposes that chroma.
        // This affine contraction preserves signed observations and needs no
        // division by either color's luminance. Static rough-lobe misses retain
        // the same temporal-variance support instead of hard-rejecting history.
        float contraction = min(1, range / max(abs(previous_luma - current_moments.x), 1e-6));
        previous = lerp(current, previous, contraction);
        float clipped_mean = lerp(current_moments.x, previous_moments.x, contraction);
        previous_moments = float2(clipped_mean,
            temporal_variance * contraction * contraction + clipped_mean * clipped_mean);
        // Count is reconstruction confidence, not an independent-sample proof.
        previous_count *= contraction;
        float disagreement = abs(previous_luma - current_moments.x);
        if (disagreement > max(.1, sigma * 6)) previous_count = min(previous_count, 7);
        float history_weight = previous_count / (previous_count + 1);
        retained_contribution = history_weight * contraction;
        result = lerp(current, previous, history_weight);
        result_moments = lerp(current_moments, previous_moments, history_weight);
        count = previous_count + 1;
    }
    lit_reflection_resolved_output[p] = float4(lit_reflection_finite(result), count);
    lit_reflection_moments_output[p] = result_moments;
    // Rough radiance pools neighboring rays. Keep a representative contributing
    // source through stochastic misses, advancing it into THIS frame's domain.
    // This metadata changes no estimator weight and is not a hit/miss validity test.
    if (nr.w >= .04 && valid && any(previous_witness != 0) &&
        (1 - retained_contribution) * witness_score / weight_sum <
            retained_contribution * length(previous_signal.rgb - environment)) witness = previous_witness;
    lit_reflection_hit_output[p] = witness;
}

[numthreads(8, 8, 1)]
void lit_reflection_filter_cs(uint3 id : SV_DispatchThreadID) {
    uint2 p = id.xy;
    if (any(p >= lit_size_frame.xy)) return;
    float4 center = lit_reflection_filter_input.Load(int3(p, 0));
    lit_reflection_filter_output[p] = center;
    if (!lit_post_surface(p) || (lit_effect_flags & 8) == 0) return;
    float4 nr = lit_normal_roughness.Load(int3(p, 0));
    if (!lit_ssr_eligible(p, nr)) return;
    // True mirrors retain their association and sharp signal, including edges.
    if (nr.w < .04) return;
    float3 normal = lit_safe_normal(nr.xyz), position = lit_post_world(p);
    float pixel_world = max(lit_post_pixel_world_size(lit_post_uv(p), lit_depth.Load(int3(p, 0))), .001);
    float2 moments = lit_reflection_moments.Load(int3(p, 0));
    float variance = max(0, moments.y - moments.x * moments.x);
    float sigma = sqrt(variance);
    float relative_noise = saturate(sigma / max(abs(moments.x), .02));
    float distance = asfloat(lit_reflection_hit_trace.Load(int3(p, 0)).y);
    float footprint_distance = distance > 0 ? distance : lit_controls.w * .15;
    float footprint = clamp(nr.w * nr.w * footprint_distance / pixel_world * .1, 1, 8);
    float radius = max(1, footprint * lerp(.35, 1, relative_noise));
    uint step = max(1u, lit_reflection_filter_step);
    if (step > radius) return;
    float3 sum = center.rgb; float weight_sum = 1;
    float center_luma = lit_post_luminance(center.rgb);
    [unroll] for (int y = -1; y <= 1; ++y) [unroll] for (int x = -1; x <= 1; ++x) {
        if (x == 0 && y == 0) continue;
        int2 q = int2(p) + int2(x, y) * int(step);
        if (any(q < 0) || any(q >= int2(lit_size_frame.xy)) || !lit_post_surface(q) ||
            !lit_reflection_same_material(p, q)) continue;
        float4 q_nr = lit_normal_roughness.Load(int3(q, 0));
        if (!lit_ssr_eligible(q, q_nr)) continue;
        float normal_dot = dot(normal, lit_safe_normal(q_nr.xyz));
        float plane_distance = abs(dot(lit_post_world(q)-position,lit_geometry_normal_at(p)));
        if (normal_dot < .96 || abs(nr.w - q_nr.w) > .08 ||
            plane_distance > max(.005, pixel_world * .5)) continue;
        float4 sample = lit_reflection_filter_input.Load(int3(q, 0));
        float2 q_moments = lit_reflection_moments.Load(int3(q, 0));
        float q_variance = max(0, q_moments.y - q_moments.x * q_moments.x);
        float signal_sigma = sqrt(variance / max(center.a, 1) + q_variance / max(sample.a, 1));
        float range = max(.01, signal_sigma * 4 + .05 * max(abs(center_luma), abs(lit_post_luminance(sample.rgb))));
        float weight = exp(-abs(lit_post_luminance(sample.rgb) - center_luma) / range) *
                       pow(saturate(normal_dot), 32) / (1 + x * x + y * y);
        float q_distance = asfloat(lit_reflection_hit_trace.Load(int3(q, 0)).y);
        // Rough-lobe hit/miss is a random variable, not a geometry boundary.
        // Distance guides only two valid hits and grows with the lobe width.
        if (distance > 0 && q_distance > 0) {
            float distance_range = max(.25, (distance + q_distance) * nr.w * .5);
            weight *= exp(-abs(distance - q_distance) / distance_range);
        }
        sum += sample.rgb * weight; weight_sum += weight;
    }
    lit_reflection_filter_output[p] = float4(lit_reflection_finite(sum / weight_sum), center.a);
}

float3 lit_post_current_hdr(uint2 p) {
    return lit_post_finite(lit_hdr_source.Load(int3(p, 0)).rgb + lit_reflection.Load(int3(p, 0)).rgb);
}
[numthreads(8, 8, 1)]
void lit_taa_cs(uint3 id : SV_DispatchThreadID) {
    uint2 p = id.xy;
    if (any(p >= lit_size_frame.xy)) return;
    float depth = lit_depth.Load(int3(p, 0));
    float4 nr = lit_normal_roughness.Load(int3(p, 0));
    lit_history_depth_output[p] = lit_inside(p) ? depth : lit_depth_far_value();
    lit_history_normal_output[p] = lit_inside(p) ? nr : 0;
    lit_history_geometry_normal_output[p]=lit_inside(p)?lit_geometry_normal.Load(int3(p,0)):0;
    lit_history_base_metallic_output[p]=lit_inside(p)?lit_base_metallic.Load(int3(p,0)):0;
    lit_history_surface_id_output[p]=lit_inside(p)?lit_surface_id.Load(int3(p,0)):0;
    if (!lit_inside(p)) { lit_taa_output[p] = 0; return; }
    float3 current = lit_post_current_hdr(p);
    if (!lit_post_surface(p)) { lit_taa_output[p] = float4(current, 1); return; }
    float2 uv = lit_post_uv(p);
    float3 position = lit_world(uv, depth), normal = lit_safe_normal(nr.xyz);
    float4 motion = lit_motion.Load(int3(p, 0));
    float2 previous_uv = uv + motion.xy;
    float tolerance = lit_post_previous_depth_tolerance(position, lit_view_direction(uv));
    float2 gradient = lit_post_previous_depth_gradient(position,lit_geometry_normal_at(p));
    float expected_depth = lit_previous_device_depth(p, motion);
    if ((lit_history_invalid & 8) != 0 || (lit_effect_flags & 16) == 0 || motion.w <= .5) {
        lit_taa_output[p] = float4(current, 1); return;
    }
    float3 center_encoded = lit_post_ycocg(current), minimum = center_encoded, maximum = center_encoded;
    float3 moment1 = center_encoded, moment2 = center_encoded * center_encoded; float count = 1;
    float pixel_world = max(lit_post_pixel_world_size(uv, depth), .001);
    [unroll] for (int y = -1; y <= 1; ++y) [unroll] for (int x = -1; x <= 1; ++x) {
        if (x == 0 && y == 0) continue;
        int2 q = int2(p) + int2(x, y);
        if (any(q < 0) || any(q >= int2(lit_size_frame.xy)) || !lit_post_surface(q)) continue;
        float3 q_normal = lit_safe_normal(lit_normal_roughness.Load(int3(q, 0)).xyz);
        if (dot(normal, q_normal) < .94 ||
            abs(dot(lit_post_world(q)-position,lit_geometry_normal_at(p))) > max(.005, pixel_world * .5)) continue;
        float3 encoded = lit_post_ycocg(lit_post_current_hdr(q));
        minimum = min(minimum, encoded); maximum = max(maximum, encoded);
        moment1 += encoded; moment2 += encoded * encoded; ++count;
    }
    float3 mean = moment1 / count;
    float3 sigma = sqrt(max(0, moment2 / count - mean * mean));
    minimum = max(minimum, mean - sigma * 1.25);
    maximum = min(maximum, mean + sigma * 1.25);
    // Signal reconstruction already distinguishes stochastic variance from
    // disocclusion. Do not squeeze its legitimate mean into this frame's tiny
    // anti-aliasing neighborhood. Geometric history validation remains strict.
    float2 reflection_moments = lit_reflection_moments.Load(int3(p, 0));
    float reflection_variance = max(0, reflection_moments.y - reflection_moments.x * reflection_moments.x);
    float reflection_count = max(1, lit_reflection.Load(int3(p, 0)).a);
    float reflection_sigma = sqrt(reflection_variance / reflection_count);
    // Conservative change of basis: RGB chroma can exceed Rec709 luminance,
    // especially for blue emitters. The persistent moments stay in Rec709.
    float3 signal_extent = reflection_sigma * float3(4, 7, 7);
    minimum -= signal_extent; maximum += signal_extent;
    // The shared footprint reconstruction validates each tap against the
    // projected plane, preserving low-poly edges without rejecting plane slope.
    float4 history_signal;
    if (!lit_post_sample_history(lit_taa_previous, p, previous_uv, expected_depth, normal,
                                tolerance, gradient, history_signal)) {
        lit_taa_output[p] = float4(current, 1); return;
    }
    float3 history = history_signal.rgb;
    float history_weight = min(lit_temporal.x, .95) * lit_post_response(motion.xy) *
                           lerp(1, .9, saturate(lit_temporal.z));
    float3 clipped = lit_post_rgb(clamp(lit_post_ycocg(history), minimum, maximum));
    lit_taa_output[p] = float4(lit_post_finite(lerp(current, clipped, history_weight)), 1);
}

float3 lit_post_bright(float3 color) {
    float exposure = max(lit_controls.x, 1e-6);
    float3 exposed = min(lit_post_finite(color) * exposure, max(lit_bloom_controls.w, 0));
    float brightness = max(exposed.r, max(exposed.g, exposed.b));
    float threshold = max(lit_bloom_controls.x, 0), knee = max(lit_bloom_controls.y, 1e-5);
    float soft = clamp(brightness - threshold + knee, 0, 2 * knee);
    soft = soft * soft / (4 * knee);
    float contribution = max(brightness - threshold, soft) / max(brightness, 1e-5);
    return exposed * contribution / exposure;
}
[numthreads(8, 8, 1)]
void lit_bloom_downsample_cs(uint3 id : SV_DispatchThreadID) {
    uint width, height; lit_bloom_output.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    float3 sum = 0;
    // Four-by-four bright extraction uses actual neighboring scene radiance;
    // the empty window margin contributes zero instead of a replicated glow.
    [unroll] for (uint y = 0; y < 4; ++y) [unroll] for (uint x = 0; x < 4; ++x) {
        uint2 q = id.xy * 4 + uint2(x, y);
        if (lit_inside(q)) sum += lit_post_bright(lit_taa.Load(int3(q, 0)).rgb);
    }
    lit_bloom_output[id.xy] = float4(lit_post_finite(sum / 16), 1);
}
[numthreads(8, 8, 1)]
void lit_bloom_blur_cs(uint3 id : SV_DispatchThreadID) {
    uint width, height; lit_bloom_output.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    float3 sum = 0;
    // Normalized, separable radius-four Gaussian; ping-pong quarter-resolution
    // images preserve the whole-resource state contract and keep work bounded.
    [unroll] for (int offset = -4; offset <= 4; ++offset) {
        int2 q = int2(id.xy) + lit_bloom_direction * offset;
        if (any(q < 0) || q.x >= int(width) || q.y >= int(height)) continue;
        float weight = offset == 0 ? .227027027 : abs(offset) == 1 ? .194594595 :
                       abs(offset) == 2 ? .121621622 : abs(offset) == 3 ? .054054054 : .016216216;
        sum += lit_bloom_input.Load(int3(q, 0)).rgb * weight;
    }
    lit_bloom_output[id.xy] = float4(lit_post_finite(sum), 1);
}
float4 lit_post_tonemap(uint2 p) {
    if (!lit_inside(p)) return 0;
    float2 uv = lit_post_uv(p);
    float3 bloom = (lit_effect_flags & 32) != 0 ? lit_bloom.SampleLevel(lit_linear_sampler, uv, 0).rgb : 0;
    float3 color = lit_post_finite(lit_taa.Load(int3(p, 0)).rgb +
                                  bloom * clamp(lit_bloom_controls.z, 0, 1));
    color = lit_post_finite(color * max(lit_controls.x, 0));
    // Bounded filmic rational curve in scene-linear RGB. Output remains LINEAR
    // SDR; the sRGB RTV performs the one and only display transfer encoding.
    float3 mapped = saturate((color * (2.51 * color + .03)) /
                            (color * (2.43 * color + .59) + .14));
    return float4(mapped, 1);
}
[numthreads(8, 8, 1)]
void lit_tonemap_cs(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= lit_size_frame.xy)) return;
    lit_tone_output[id.xy] = lit_post_tonemap(id.xy);
}
float4 lit_display_ps(float4 position : SV_Position, float2 input_uv : TEXCOORD0) : SV_Target {
    // Viewport raster clips this pass; the sRGB RTV encodes exactly once.
    return lit_display_source.Load(int3(uint2(position.xy), 0));
}
