RWTexture2D<float4> lit_direct_diffuse_raw_output;
RWTexture2D<float4> lit_direct_specular_raw_output;
// Raw alpha describes stochastic support, not RGB validity. A deterministic
// receiver keeps its current radiance with zero reconstructable history count.
uint lit_direct_stochastic;
Texture2D<float4> lit_direct_diffuse_raw;
Texture2D<float4> lit_direct_specular_raw;
Texture2D<float4> lit_direct_diffuse_previous;
Texture2D<float4> lit_direct_specular_previous;
// Persistent Rec709 mean and stable variance, not E[L^2]. RG32F avoids both
// FP16 overflow and cancellation when a bright signal has little variance.
Texture2D<float2> lit_direct_diffuse_moments_previous;
Texture2D<float2> lit_direct_specular_moments_previous;
RWTexture2D<float4> lit_direct_diffuse_history_output;
RWTexture2D<float4> lit_direct_specular_history_output;
RWTexture2D<float2> lit_direct_diffuse_moments_output;
RWTexture2D<float2> lit_direct_specular_moments_output;
Texture2D<float4> lit_direct_diffuse_filter_input;
Texture2D<float4> lit_direct_specular_filter_input;
RWTexture2D<float4> lit_direct_diffuse_filter_output;
RWTexture2D<float4> lit_direct_specular_filter_output;
Texture2D<float2> lit_direct_diffuse_moments;
Texture2D<float2> lit_direct_specular_moments;
Texture2D<float4> lit_history_normal;
Texture2D<float> lit_history_depth;
Texture2D<float4> lit_indirect_source;
float lit_indirect_gain;
Texture2D<float> lit_ao;
RWTexture2D<float4> lit_hdr_output;

float3 lit_direct_finite(float3 value) {
    return all(isfinite(value)) ? clamp(value, 0, 65504) : 0;
}
float lit_direct_luminance(float3 value) { return dot(value, float3(.2126, .7152, .0722)); }
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
float lit_direct_pixel_size(uint2 pixel) {
    float2 uv = lit_direct_uv(int2(pixel));
    float depth = lit_depth.Load(int3(pixel, 0));
    float3 position = lit_world(uv, depth);
    return max(1e-6, max(length(lit_world(uv + float2(1.0 / lit_size_frame.x, 0), depth) - position),
                         length(lit_world(uv + float2(0, 1.0 / lit_size_frame.y), depth) - position)));
}
float3 lit_direct_diffuse_factor(float4 material) { return max(material.rgb, 0) * (1 - saturate(material.w)); }
float3 lit_direct_specular_factor(float4 material) {
    // Schlick contains a grazing term even when F0 is zero. A positive
    // demodulator retains that energy and is multiplied back at the receiver.
    // Normalize by the dielectric F0 so demodulation never amplifies the
    // signal 25x before FP16 storage. This constant cancels during remodulation.
    return max(lerp(.04.xxx, max(material.rgb, 0), saturate(material.w)), .04) / .04;
}
struct LitDirectGuide {
    float3 position, normal, geometric_normal;
    float4 material;
    float roughness, pixel_size;
};
uint lit_direct_diffuse_channels(float4 material) {
    bool3 supported = lit_direct_diffuse_factor(material) > 1e-8;
    return uint(supported.x) | (uint(supported.y) << 1) | (uint(supported.z) << 2);
}
// Step 1 reuses a 12x12 full-precision guide tile for its 8x8 receivers.
// The sparser step 2 footprint did not amortize its larger halo in the dungeon.
// Other entry points never reference this cache and require no group barrier.
struct LitDirectCachedGuide {
    float3 position, normal, geometric_normal;
    float roughness, metallic;
    uint valid_channels;
};
groupshared LitDirectCachedGuide lit_direct_guides[144];

void lit_direct_cache_guides(uint2 group, uint lane) {
    int2 origin = int2(lit_screen_dispatch.xy + group * 8) - 2;
    for (uint i = lane; i < 144; i += 64) {
        int2 q = origin + int2(i % 12, i / 12);
        LitDirectCachedGuide guide = (LitDirectCachedGuide)0;
        if (all(q >= 0) && all(q < int2(lit_size_frame.xy)) && lit_inside(uint2(q))) {
            float4 nr = lit_normal_roughness.Load(int3(q, 0));
            float depth = lit_depth.Load(int3(q, 0));
            if (!lit_is_background_depth(depth) && dot(nr.xyz, nr.xyz) > .25) {
                float4 material = lit_base_metallic.Load(int3(q, 0));
                guide.position = lit_world(lit_direct_uv(q), depth);
                guide.normal = lit_safe_normal(nr.xyz);
                guide.geometric_normal = lit_geometry_normal_at(q);
                guide.roughness = nr.w; guide.metallic = material.w;
                guide.valid_channels = 1 | (lit_direct_diffuse_channels(material) << 1);
            }
        }
        lit_direct_guides[i] = guide;
    }
    GroupMemoryBarrierWithGroupSync();
}
LitDirectGuide lit_direct_guide(uint2 p, float3 position, float3 normal, float pixel_size) {
    LitDirectGuide guide;
    guide.position = position; guide.normal = normal; guide.pixel_size = pixel_size;
    guide.geometric_normal = lit_geometry_normal_at(p);
    guide.material = lit_base_metallic.Load(int3(p, 0));
    guide.roughness = lit_normal_roughness.Load(int3(p, 0)).w;
    return guide;
}
// Diffuse/specular share geometric support; material and roughness gates remain
// independent. A paired filter tap loads and reconstructs its guide only once.
bool2 lit_direct_neighbor(LitDirectGuide center, int2 q, uint cache_index, bool cached) {
    LitDirectCachedGuide guide = (LitDirectCachedGuide)0;
    float4 q_nr; float depth = 0;
    if (cached) {
        guide = lit_direct_guides[cache_index];
        if ((guide.valid_channels & 1) == 0) return false;
        q_nr = float4(guide.normal, guide.roughness);
    } else {
        if (any(q < 0) || any(q >= int2(lit_size_frame.xy)) || !lit_inside(uint2(q))) return false;
        q_nr = lit_normal_roughness.Load(int3(q, 0));
        depth = lit_depth.Load(int3(q, 0));
        if (lit_is_background_depth(depth) || !(dot(q_nr.xyz, q_nr.xyz) > .25)) return false;
        q_nr.xyz = lit_safe_normal(q_nr.xyz);
    }
    float3 q_normal = q_nr.xyz;
    if (dot(center.normal, q_normal) < .9995 || abs(center.roughness - q_nr.w) > .15) return false;
    // Strict normals and both tangent planes keep low-poly facets separate.
    // Plane error is in world units, independent of device-depth direction.
    float3 delta = (cached ? guide.position : lit_world(lit_direct_uv(q), depth)) - center.position;
    float tolerance = max(.0005, center.pixel_size * .25);
    if (abs(dot(delta,center.geometric_normal)) > tolerance ||
        abs(dot(delta,cached ? guide.geometric_normal : lit_geometry_normal_at(q))) > tolerance) return false;
    float4 q_material = cached ? float4(0,0,0,guide.metallic) : lit_base_metallic.Load(int3(q, 0));
    // A zero material channel contains no diffuse illumination estimate.
    // It must not darken a neighboring channel whose demodulator is nonzero.
    uint q_channels = cached ? guide.valid_channels >> 1 : lit_direct_diffuse_channels(q_material);
    bool diffuse = lit_direct_diffuse_channels(center.material) == q_channels;
    bool specular = !(abs(center.roughness - q_nr.w) > .05) && abs(center.material.w - q_material.w) <= .15;
    return bool2(diffuse, specular);
}
bool2 lit_direct_neighbor(LitDirectGuide center, int2 q) {
    return lit_direct_neighbor(center, q, 0, false);
}

void lit_direct_merge_moments(float mean, float variance, float weight,
                              inout float sum_weight, inout float result_mean, inout float result_m2) {
    float next_weight = sum_weight + weight;
    float delta = mean - result_mean;
    result_m2 += weight * max(variance, 0) + delta * delta * sum_weight * weight / max(next_weight, 1e-8);
    result_mean += delta * weight / max(next_weight, 1e-8);
    sum_weight = next_weight;
}

void lit_direct_statistics_tap(Texture2D<float4> source, int2 q, float weight,
                               inout float3 average, inout float weight_sum, inout float mean, inout float m2) {
    float4 signal = source.Load(int3(q, 0));
    if (signal.a <= 0 || !all(isfinite(signal))) return;
    average += max(signal.rgb, 0) * weight;
    lit_direct_merge_moments(lit_direct_luminance(max(signal.rgb, 0)), 0, weight,
        weight_sum, mean, m2);
}
void lit_direct_local_statistics(uint2 p, float3 position, float3 normal, float pixel_size,
                                  out float3 diffuse_average, out float2 diffuse_moments,
                                  out float3 specular_average, out float2 specular_moments) {
    diffuse_average = 0; specular_average = 0;
    float diffuse_weight = 0, diffuse_mean = 0, diffuse_m2 = 0;
    float specular_weight = 0, specular_mean = 0, specular_m2 = 0;
    LitDirectGuide guide = lit_direct_guide(p, position, normal, pixel_size);
    // Share only the guide. Each signal keeps its own accepted taps and the
    // original row-major floating-point accumulation order.
    [unroll] for (int y = -1; y <= 1; ++y) [unroll] for (int x = -1; x <= 1; ++x) {
        int2 q = int2(p) + int2(x, y);
        bool2 neighbor = lit_direct_neighbor(guide, q);
        float weight = 1.0 / (1 + x * x + y * y);
        if (neighbor.x) lit_direct_statistics_tap(lit_direct_diffuse_raw, q, weight,
            diffuse_average, diffuse_weight, diffuse_mean, diffuse_m2);
        if (neighbor.y) lit_direct_statistics_tap(lit_direct_specular_raw, q, weight,
            specular_average, specular_weight, specular_mean, specular_m2);
    }
    diffuse_average /= max(diffuse_weight, 1e-8);
    diffuse_moments = float2(diffuse_mean, max(0, diffuse_m2 / max(diffuse_weight, 1e-8)));
    specular_average /= max(specular_weight, 1e-8);
    specular_moments = float2(specular_mean, max(0, specular_m2 / max(specular_weight, 1e-8)));
}

void lit_direct_previous_plane(uint2 p, float3 position, float3 normal, float pixel_size,
                                float expected_depth, out float tolerance, out float2 gradient) {
    gradient = 0; tolerance = abs(expected_depth) * (8 * 1.1920929e-7);
    float3 tangent, bitangent; lit_basis(normal, tangent, bitangent);
    float scale = max(pixel_size, .001);
    float4 a = mul(float4(position, 1), lit_previous_view_projection);
    float4 b = mul(float4(position + tangent * scale, 1), lit_previous_view_projection);
    float4 c = mul(float4(position + bitangent * scale, 1), lit_previous_view_projection);
    float4 extent = mul(float4(position + lit_view_direction(lit_direct_uv(int2(p))) * .02, 1),
        lit_previous_view_projection);
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

void lit_direct_history_tap(Texture2D<float4> source, Texture2D<float2> moment_source, int2 q, float weight,
                            inout float4 history, inout float support, inout float mean, inout float m2) {
    float4 signal = source.Load(int3(q, 0));
    float2 sample_moments = moment_source.Load(int3(q, 0));
    if (signal.a < 1 || !all(isfinite(signal)) || !all(isfinite(sample_moments))) return;
    history += signal * weight;
    lit_direct_merge_moments(sample_moments.x, sample_moments.y, weight, support, mean, m2);
}
bool2 lit_direct_history(uint2 p, float2 previous_uv, float expected_depth, float3 normal, float roughness,
                           float tolerance, float2 gradient,
                           out float4 diffuse_history, out float2 diffuse_moments,
                           out float4 specular_history, out float2 specular_moments, out float2 support) {
    diffuse_history = 0; diffuse_moments = 0;
    specular_history = 0; specular_moments = 0; support = 0;
    if (lit_size_frame.w == 0 || !all(isfinite(previous_uv)) || !isfinite(expected_depth) ||
        any(previous_uv < 0) || any(previous_uv > 1)) return false;
    float2 location = previous_uv * lit_size_frame.xy - .5;
    int2 base = int2(floor(location)); float2 fraction = frac(location);
    float diffuse_mean = 0, diffuse_m2 = 0, specular_mean = 0, specular_m2 = 0;
    [unroll] for (int y = 0; y < 2; ++y) [unroll] for (int x = 0; x < 2; ++x) {
        int2 q = base + int2(x, y);
        if (any(q < 0) || any(q >= int2(lit_size_frame.xy)) || !lit_inside(uint2(q))) continue;
        if (!lit_history_surface_matches(p, uint2(q))) continue;
        float depth = lit_history_depth.Load(int3(q, 0));
        float4 nr = lit_history_normal.Load(int3(q, 0));
        float tap_depth = expected_depth + dot(lit_direct_uv(q) - previous_uv, gradient);
        if (lit_is_background_depth(depth) || !isfinite(depth) || dot(nr.xyz, nr.xyz) < .25 ||
            abs(depth - tap_depth) > tolerance || dot(normal, lit_safe_normal(nr.xyz)) < .9995) continue;
        float weight = (x == 0 ? 1 - fraction.x : fraction.x) * (y == 0 ? 1 - fraction.y : fraction.y);
        if (!(abs(roughness - nr.w) > .15))
            lit_direct_history_tap(lit_direct_diffuse_previous, lit_direct_diffuse_moments_previous, q, weight,
                diffuse_history, support.x, diffuse_mean, diffuse_m2);
        if (!(abs(roughness - nr.w) > .05))
            lit_direct_history_tap(lit_direct_specular_previous, lit_direct_specular_moments_previous, q, weight,
                specular_history, support.y, specular_mean, specular_m2);
    }
    bool2 valid = !(support < .1);
    if (valid.x) {
        diffuse_history /= support.x;
        diffuse_moments = float2(diffuse_mean, max(0, diffuse_m2 / support.x));
    }
    if (valid.y) {
        specular_history /= support.y;
        specular_moments = float2(specular_mean, max(0, specular_m2 / support.y));
    }
    return valid;
}

void lit_direct_resolve_signal(float3 current, bool specular, float roughness, float4 motion,
                                 float3 average, float2 local, bool valid,
                                 float4 history, float2 history_moments, float support,
                                 out float4 result, out float2 result_moments) {
    float current_luma = lit_direct_luminance(current);
    float count = 1;
    result_moments = float2(current_luma, local.y);
    if (valid) {
        float motion_pixels = length(motion.xy * lit_size_frame.xy);
        float maximum_count = specular ? lerp(15, 47, saturate(roughness)) : 47;
        // Known light edits shorten support gently. They never globally turn
        // history off; geometry validity remains an independent hard gate.
        maximum_count = lerp(maximum_count, specular ? 7 : 11, saturate(lit_temporal.z));
        // A moving occluder changes illumination on stationary receivers too.
        // Preserve their history, but let new visibility replace old shadows.
        // Unlike an authored light/color edit, this is not proof that a random
        // zero observation invalidates a distant, unchanged lighting mean.
        maximum_count = min(maximum_count, lerp(47, 4, saturate(lit_temporal.w)));
        float previous_count = min(history.a * support * lerp(1, .7, saturate(motion_pixels / 12)), maximum_count);
        // A known lighting change and a coherent current neighborhood provide
        // evidence of a new signal, independently of the old signal variance.
        // Clip color as well as luminance so a removed colored lamp cannot
        // leave a long chromatic tail. Noisy neighborhoods keep wider bounds.
        if (lit_temporal.z > 0) {
            float response_range = 3 * sqrt(max(local.y, 0)) + .1 * local.x + .001;
            history.rgb = clamp(history.rgb, max(0, average - response_range), average + response_range);
        }
        float sigma = sqrt(max(local.y, history_moments.y));
        float range = max(.005, sigma * 3 + local.x * .25);
        float history_luma = lit_direct_luminance(max(history.rgb, 0));
        float clipped_luma = max(0, clamp(history_luma, local.x - range, local.x + range));
        if (abs(history_luma - local.x) > range * 2) previous_count = min(previous_count, 7);
        if (history_luma > 1e-8) {
            float scale = clipped_luma / history_luma;
            history.rgb *= scale;
            history_moments.y *= scale * scale;
        } else history.rgb = 0; // A valid black history has no chroma to borrow from its neighbors.
        float history_weight = previous_count / (previous_count + 1);
        float clipped_mean = lit_direct_luminance(max(history.rgb, 0));
        result_moments.x = lerp(current_luma, clipped_mean, history_weight);
        // Stable mixture variance retains stochastic fLV variability, while
        // its division by effective count below describes reconstruction noise.
        float delta = current_luma - clipped_mean;
        // The new per-pixel observation has zero internal mixture variance.
        // Local spatial variation only seeds disocclusion; reinjecting it on
        // every frame would mistake a static shadow edge for perpetual noise.
        result_moments.y = max(0, history_weight * history_moments.y +
            history_weight * (1 - history_weight) * delta * delta);
        current = lerp(current, max(history.rgb, 0), history_weight);
        count = previous_count + 1;
    }
    result = float4(lit_direct_finite(current), count);
}

[numthreads(8,8,1)]
void lit_direct_resolve_cs(uint3 id : SV_DispatchThreadID) {
    uint2 p; if (!lit_screen_pixel(id.xy, p)) return;
    lit_direct_diffuse_history_output[p] = 0; lit_direct_specular_history_output[p] = 0;
    lit_direct_diffuse_moments_output[p] = 0; lit_direct_specular_moments_output[p] = 0;
    if (!lit_direct_surface(int2(p))) return;
    float4 diffuse_raw = lit_direct_diffuse_raw.Load(int3(p, 0));
    float4 specular_raw = lit_direct_specular_raw.Load(int3(p, 0));
    float3 diffuse_current = lit_direct_finite(diffuse_raw.rgb);
    float3 specular_current = lit_direct_finite(specular_raw.rgb);
    // The independent reference integrates the same fLV samples without
    // reconstruction bias. No-light frames must never retain a removed lamp.
    if (lit_di_settings.z != 0 || (lit_source_counts.x == 0 && lit_source_counts.y == 0)) {
        lit_direct_diffuse_history_output[p] = float4(diffuse_current, 1);
        lit_direct_specular_history_output[p] = float4(specular_current, 1);
        lit_direct_diffuse_moments_output[p] = float2(lit_direct_luminance(diffuse_current), 0);
        lit_direct_specular_moments_output[p] = float2(lit_direct_luminance(specular_current), 0);
        return;
    }
    if (diffuse_raw.a == 0 && specular_raw.a == 0) {
        // No random estimator contributes here. Moving a hard-shadow caster or
        // editing a delta light changes the current answer immediately, rather
        // than accumulating yesterday's deterministic illumination.
        lit_direct_diffuse_history_output[p] = float4(diffuse_current, 0);
        lit_direct_specular_history_output[p] = float4(specular_current, 0);
        lit_direct_diffuse_moments_output[p] = float2(lit_direct_luminance(diffuse_current), 0);
        lit_direct_specular_moments_output[p] = float2(lit_direct_luminance(specular_current), 0);
        return;
    }
    float4 nr = lit_normal_roughness.Load(int3(p, 0));
    float3 normal = lit_safe_normal(nr.xyz), position = lit_direct_world(int2(p));
    float pixel_size = lit_direct_pixel_size(p);
    float4 motion = lit_motion.Load(int3(p, 0));
    float expected_depth = lit_previous_device_depth(p, motion), tolerance;
    float2 gradient; lit_direct_previous_plane(p, position, lit_geometry_normal_at(p), pixel_size, expected_depth, tolerance, gradient);
    float3 diffuse_average, specular_average; float2 diffuse_local, specular_local;
    lit_direct_local_statistics(p, position, normal, pixel_size,
        diffuse_average, diffuse_local, specular_average, specular_local);
    float4 diffuse_history = 0, specular_history = 0;
    float2 diffuse_history_moments = 0, specular_history_moments = 0, support = 0;
    bool2 valid = false;
    if ((lit_history_invalid & 1) == 0 && motion.w > .5)
        valid = lit_direct_history(p, motion.xy + lit_direct_uv(int2(p)), expected_depth, normal, nr.w, tolerance, gradient,
            diffuse_history, diffuse_history_moments, specular_history, specular_history_moments, support);
    float4 diffuse, specular; float2 diffuse_moments, specular_moments;
    lit_direct_resolve_signal(diffuse_current, false, nr.w, motion, diffuse_average, diffuse_local, valid.x,
        diffuse_history, diffuse_history_moments, support.x, diffuse, diffuse_moments);
    lit_direct_resolve_signal(specular_current, true, nr.w, motion, specular_average, specular_local, valid.y,
        specular_history, specular_history_moments, support.y, specular, specular_moments);
    lit_direct_diffuse_history_output[p] = diffuse; lit_direct_specular_history_output[p] = specular;
    lit_direct_diffuse_moments_output[p] = diffuse_moments; lit_direct_specular_moments_output[p] = specular_moments;
}

struct LitDirectFilter {
    float4 center;
    float3 sum;
    float noise, radius, center_luma, weight_sum;
    bool active;
};
LitDirectFilter lit_direct_filter_begin(Texture2D<float4> source, Texture2D<float2> moment_source,
                                       uint2 p, bool specular, float roughness, uint step) {
    LitDirectFilter filter = (LitDirectFilter)0;
    filter.center = source.Load(int3(p, 0));
    if (lit_di_settings.z != 0 || (lit_source_counts.x == 0 && lit_source_counts.y == 0)) return filter;
    float2 moments = moment_source.Load(int3(p, 0));
    if (!all(isfinite(filter.center)) || !all(isfinite(moments))) {
        filter.center = 0; return filter;
    }
    if (filter.center.a <= 0 || moments.y == 0) return filter;
    // Exact zero variance only skips spatial work. Preserve a positive history
    // count: a stochastic black sample is not proof of deterministic support.
    filter.noise = sqrt(max(moments.y, 0) / max(filter.center.a, 1));
    float relative_noise = saturate(filter.noise / max(moments.x, .005));
    filter.radius = specular ? clamp(roughness * roughness * 8, 1, 4) : 4;
    filter.radius *= lerp(.5, 1, relative_noise);
    if (step > filter.radius || (specular && roughness < .04)) return filter;
    filter.center_luma = lit_direct_luminance(filter.center.rgb);
    filter.active = true;
    return filter;
}
void lit_direct_filter_tap(Texture2D<float4> source, int2 q, float kernel, inout LitDirectFilter filter) {
    float4 sample = source.Load(int3(q, 0));
    if (sample.a <= 0 || !all(isfinite(sample))) return;
    float luma = lit_direct_luminance(sample.rgb);
    // The receiver's uncertainty sets its reconstruction support. A noisy
    // bright neighbor cannot make a converged occluded receiver uncertain
    // and leak through an intervening wall absent from the pixel centers.
    float range = max(.002, filter.noise * 3 + .02 * max(filter.center_luma, luma));
    float weight = kernel * exp(-abs(luma - filter.center_luma) / range);
    filter.sum += max(sample.rgb, 0) * weight; filter.weight_sum += weight;
}
float4 lit_direct_filter_end(LitDirectFilter filter) {
    if (!filter.active) return filter.center;
    return float4(lit_direct_finite(filter.sum / max(filter.weight_sum, 1e-8)), filter.center.a);
}

void lit_direct_filter(uint3 id, uint step) {
    uint2 p; if (!lit_screen_pixel(id.xy, p)) return;
    lit_direct_diffuse_filter_output[p] = 0; lit_direct_specular_filter_output[p] = 0;
    if (!lit_direct_surface(int2(p))) return;
    float4 nr = lit_normal_roughness.Load(int3(p, 0));
    LitDirectFilter diffuse = lit_direct_filter_begin(lit_direct_diffuse_filter_input,
        lit_direct_diffuse_moments, p, false, nr.w, step);
    LitDirectFilter specular = lit_direct_filter_begin(lit_direct_specular_filter_input,
        lit_direct_specular_moments, p, true, nr.w, step);
    if (diffuse.active || specular.active) {
        LitDirectGuide guide = lit_direct_guide(p, lit_direct_world(int2(p)), lit_safe_normal(nr.xyz), lit_direct_pixel_size(p));
        // Three CPU-dispatched a-trous steps (1,2,4), with independent footprints
        // and weights. Each signal retains the original row-major accumulation order;
        // only guide reads and world reconstruction are shared between its taps.
        [unroll] for (int y = -2; y <= 2; ++y) [unroll] for (int x = -2; x <= 2; ++x) {
            float distance = length(float2(x, y)) * step;
            bool center = x == 0 && y == 0;
            bool diffuse_tap = diffuse.active && (center || !(distance > diffuse.radius * 1.5));
            bool specular_tap = specular.active && (center || !(distance > specular.radius * 1.5));
            if (!diffuse_tap && !specular_tap) continue;
            int2 q = int2(p) + int2(x, y) * int(step);
            uint2 tile = (id.xy % 8) + 2;
            uint cache_index = uint(int(tile.y) + y) * 12 + uint(int(tile.x) + x);
            bool2 neighbor = lit_direct_neighbor(guide, q, cache_index, step == 1);
            float kx = x == 0 ? 6 : (abs(x) == 1 ? 4 : 1);
            float ky = y == 0 ? 6 : (abs(y) == 1 ? 4 : 1);
            if (diffuse_tap && neighbor.x) lit_direct_filter_tap(lit_direct_diffuse_filter_input, q, kx * ky, diffuse);
            if (specular_tap && neighbor.y) lit_direct_filter_tap(lit_direct_specular_filter_input, q, kx * ky, specular);
        }
    }
    lit_direct_diffuse_filter_output[p] = lit_direct_filter_end(diffuse);
    lit_direct_specular_filter_output[p] = lit_direct_filter_end(specular);
}

[numthreads(8,8,1)]
void lit_direct_filter1_cs(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint lane : SV_GroupIndex) {
    lit_direct_cache_guides(group.xy, lane);
    lit_direct_filter(id, 1);
}
[numthreads(8,8,1)]
void lit_direct_filter2_cs(uint3 id : SV_DispatchThreadID) { lit_direct_filter(id, 2); }
[numthreads(8,8,1)]
void lit_direct_filter4_cs(uint3 id : SV_DispatchThreadID) { lit_direct_filter(id, 4); }

[numthreads(8,8,1)]
void lit_compose_cs(uint3 tid:SV_DispatchThreadID) {
    uint2 p=tid.xy;if(any(p>=lit_size_frame.xy))return;
    if(!lit_inside(p)||lit_is_background_depth(lit_depth[p])) {
        // Sky Light supplies incident radiance, not a visible sky surface.
        lit_hdr_output[p]=float4(0,0,0,1);return;
    }
    float4 bm=lit_base_metallic[p]; float ao=(lit_effect_flags&4)!=0?lit_ao[p]:1;
    float3 irradiance=(lit_effect_flags&(2|64))!=0?lit_indirect_source[p].rgb:lit_environment.rgb*LIT_PI;
    float3 diffuse=bm.rgb*(1-bm.w)*irradiance/LIT_PI*ao*lit_indirect_gain;
    float3 direct_diffuse=lit_direct_diffuse_filter_input.Load(int3(p,0)).rgb*lit_direct_diffuse_factor(bm);
    float3 direct_specular=lit_direct_specular_filter_input.Load(int3(p,0)).rgb*lit_direct_specular_factor(bm);
    float3 direct=lit_direct_finite(direct_diffuse+direct_specular);
    lit_hdr_output[p]=float4(min(direct+diffuse+lit_emissive[p].rgb,65504.xxx),1);
}
