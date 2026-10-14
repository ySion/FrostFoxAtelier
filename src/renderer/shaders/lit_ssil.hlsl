// Optional, bounded near-field diffuse correction; lit_common is prepended.
// The DDGI fallback E/pi is isotropic. Screen visibility, distance fading and
// residual bounds make this a controlled approximation, NOT an unbiased GI
// estimator. No specular, final HDR, display image or SSIL feedback is read.
Texture2D<float4> lit_direct_diffuse; // Stable diffuse demodulation, before base.
Texture2D<float4> lit_indirect_source; // DDGI irradiance, before material/AO.
RWTexture2D<float4> lit_ssil_raw_output;
Texture2D<float4> lit_ssil_raw;
Texture2D<float4> lit_ssil_previous; // Bounded residual, effective history count.
Texture2D<float> lit_history_depth;
Texture2D<float4> lit_history_normal;
RWTexture2D<float4> lit_ssil_history_output;
RWTexture2D<float4> lit_ssil_irradiance_output;
float4 lit_ssil_controls; // World radius (<=1.2m), strength (normally .3), unused.

float3 lit_ssil_finite(float3 value) {
    return all(isfinite(value)) ? clamp(value, -65000, 65000) : 0;
}
float2 lit_ssil_uv(int2 p) { return (float2(p) + .5) / lit_size_frame.xy; }
bool lit_ssil_surface(int2 p) {
    if (any(p < 0) || any(p >= int2(lit_size_frame.xy)) || !lit_inside(uint2(p))) return false;
    float depth = lit_depth.Load(int3(p, 0));
    float3 normal = lit_normal_roughness.Load(int3(p, 0)).xyz;
    return isfinite(depth) && !lit_is_background_depth(depth) && dot(normal, normal) > .25;
}
float3 lit_ssil_world(int2 p) { return lit_world(lit_ssil_uv(p), lit_depth.Load(int3(p, 0))); }
float lit_ssil_pixel_size(int2 p) {
    float2 uv = lit_ssil_uv(p); float z = lit_depth.Load(int3(p, 0));
    float3 position = lit_world(uv, z);
    return max(.0001, max(length(lit_world(uv + float2(1.0 / lit_size_frame.x, 0), z) - position),
                          length(lit_world(uv + float2(0, 1.0 / lit_size_frame.y), z) - position)));
}
float3 lit_ssil_baseline(int2 p) {
    return max(0, lit_ssil_finite((lit_effect_flags & 2) != 0 ?
        lit_indirect_source.Load(int3(p, 0)).rgb : lit_environment.rgb * LIT_PI));
}
float3 lit_ssil_bound(float3 residual, float3 baseline) {
    float3 limit = max(baseline, .25.xxx);
    return clamp(lit_ssil_finite(residual), -limit, limit);
}
bool lit_ssil_project(float3 position, out float2 uv, out float device_z) {
    float4 clip = mul(float4(position, 1), lit_view_projection);
    uv = 0; device_z = 0;
    if (clip.w <= 1e-6 || !all(isfinite(clip))) return false;
    uv = lit_window_uv(clip); device_z = clip.z / clip.w;
    float2 pixel = uv * lit_size_frame.xy;
    return device_z >= 0 && device_z <= 1 &&
        all(pixel >= lit_viewport.xy + .5) && all(pixel <= lit_viewport.xy + lit_viewport.zw - .5);
}
bool lit_ssil_intersect(float3 origin, float3 ray, float radius, out int2 hit_pixel, out float confidence) {
    hit_pixel = 0; confidence = 0;
    float previous_t = 0; bool was_front = true;
    [loop] for (uint step = 1; step <= 16; ++step) {
        float t = radius * (step / 16.0);
        float2 uv; float device_z;
        if (!lit_ssil_project(origin + ray * t, uv, device_z)) return false;
        int2 q = int2(uv * lit_size_frame.xy);
        if (!lit_ssil_surface(q)) { previous_t = t; was_front = true; continue; }
        // Reversed Z: a positive gap is in front of the primary surface.
        float gap = device_z - lit_depth.Load(int3(q, 0));
        if (gap <= 0 && was_front) {
            float lo = previous_t, hi = t;
            [unroll] for (uint refine = 0; refine < 5; ++refine) {
                float mid = (lo + hi) * .5;
                float2 mid_uv; float mid_z;
                if (!lit_ssil_project(origin + ray * mid, mid_uv, mid_z)) return false;
                int2 mid_pixel = int2(mid_uv * lit_size_frame.xy);
                if (!lit_ssil_surface(mid_pixel) || mid_z > lit_depth.Load(int3(mid_pixel, 0))) lo = mid;
                else hi = mid;
            }
            if (!lit_ssil_project(origin + ray * hi, uv, device_z)) return false;
            q = int2(uv * lit_size_frame.xy);
            if (lit_ssil_surface(q)) {
                float3 surface = lit_ssil_world(q);
                float along = dot(surface - origin, ray);
                float error = length(surface - origin - ray * along);
                float thickness = clamp(lit_ssil_pixel_size(q) * 1.5, .008, .04) + max(along, 0) * .008;
                float facing = dot(lit_safe_normal(lit_normal_roughness.Load(int3(q, 0)).xyz), -ray);
                if (along > .02 && along < radius && error < thickness && facing > .08) {
                    float2 local = (uv * lit_size_frame.xy - lit_viewport.xy) / lit_viewport.zw;
                    float edge = min(min(local.x, local.y), min(1 - local.x, 1 - local.y));
                    confidence = saturate(edge * 20) * saturate((facing - .08) / .3) *
                        (1 - smoothstep(.3, 1, error / thickness)) *
                        (1 - smoothstep(radius * .35, radius, along));
                    hit_pixel = q; return confidence > 0;
                }
            }
        }
        previous_t = t; was_front = gap > 0;
    }
    return false;
}
[numthreads(8, 8, 1)]
void lit_ssil_trace_cs(uint3 id : SV_DispatchThreadID) {
    uint2 p; if (!lit_screen_pixel(id.xy, p)) return;
    lit_ssil_raw_output[p] = 0;
    if (!lit_ssil_surface(int2(p)) || lit_ssil_controls.y <= 0 ||
        lit_base_metallic.Load(int3(p, 0)).w >= .999) return;
    float3 normal = lit_safe_normal(lit_normal_roughness.Load(int3(p, 0)).xyz);
    float3 tangent, bitangent; lit_basis(normal, tangent, bitangent);
    uint seed = lit_hash(p.x + p.y * lit_size_frame.x);
    float2 sample = frac(float2(lit_random(seed), lit_random(seed)) +
        (lit_size_frame.z % 1024) * float2(.754877666, .569840296));
    float phi = sample.y * (2 * LIT_PI), disk = sqrt(sample.x);
    float3 ray = lit_safe_normal(tangent * (disk * cos(phi)) + bitangent * (disk * sin(phi)) +
                                normal * sqrt(max(0, 1 - sample.x)));
    if(dot(lit_geometry_normal_at(p),ray)<=0)return;
    float3 origin = lit_ssil_world(int2(p)) + lit_geometry_normal_at(p) * max(.005, lit_controls.y * 2);
    int2 q; float confidence;
    if (!lit_ssil_intersect(origin, ray, clamp(lit_ssil_controls.x, .05, 1.2), q, confidence)) return;
    float4 material = lit_base_metallic.Load(int3(q, 0));
    float3 factor = max(material.rgb, 0) * (1 - saturate(material.w));
    // Match the DDGI scattered-light boundary. Mesh NEE already owns direct
    // emitter illumination at the receiver; only reflected light is corrected.
    float3 incoming = max(0, lit_ssil_finite(lit_direct_diffuse.Load(int3(q, 0)).rgb * factor +
        lit_ssil_baseline(q) * factor / LIT_PI));
    float3 baseline = lit_ssil_baseline(int2(p));
    // cosine/pdf = pi. A miss adds zero residual, retaining DDGI/environment.
    float3 residual = lit_ssil_bound(incoming * LIT_PI - baseline, baseline) * confidence;
    lit_ssil_raw_output[p] = float4(residual, confidence);
}
void lit_ssil_previous_plane(float3 position, float3 normal, float pixel_size, float expected_depth,
                             out float tolerance, out float2 gradient) {
    tolerance = abs(expected_depth) * (8 * 1.1920929e-7); gradient = 0;
    float3 tangent, bitangent; lit_basis(normal, tangent, bitangent);
    float scale = max(pixel_size, .001);
    float4 a = mul(float4(position, 1), lit_previous_view_projection);
    float4 b = mul(float4(position + tangent * scale, 1), lit_previous_view_projection);
    float4 c = mul(float4(position + bitangent * scale, 1), lit_previous_view_projection);
    if (a.w <= 1e-8 || b.w <= 1e-8 || c.w <= 1e-8) return;
    float2 ab = lit_window_uv(b) - lit_window_uv(a), ac = lit_window_uv(c) - lit_window_uv(a);
    float determinant = ab.x * ac.y - ab.y * ac.x;
    if (abs(determinant) > 1e-16) {
        float db = b.z / b.w - a.z / a.w, dc = c.z / c.w - a.z / a.w;
        gradient = float2(db * ac.y - dc * ab.y, dc * ab.x - db * ac.x) / determinant;
        if (!all(isfinite(gradient))) gradient = 0;
    }
    // Finite reversed-depth precision follows the actual projection, not a
    // fixed device-Z epsilon that would accept unrelated distant surfaces.
    float4 extent = mul(float4(position + normal * .015, 1), lit_previous_view_projection);
    if (extent.w > 1e-8) tolerance += abs(extent.z / extent.w - a.z / a.w);
}
[numthreads(8, 8, 1)]
void lit_ssil_resolve_cs(uint3 id : SV_DispatchThreadID) {
    uint2 p; if (!lit_screen_pixel(id.xy, p)) return;
    lit_ssil_history_output[p] = 0; lit_ssil_irradiance_output[p] = 0;
    if (!lit_ssil_surface(int2(p))) return;
    float3 baseline = lit_ssil_baseline(int2(p));
    if (lit_base_metallic.Load(int3(p, 0)).w >= .999) {
        lit_ssil_irradiance_output[p] = float4(baseline, 1); return;
    }
    float3 position = lit_ssil_world(int2(p));
    float3 normal = lit_safe_normal(lit_normal_roughness.Load(int3(p, 0)).xyz);
    float pixel_size = lit_ssil_pixel_size(int2(p));
    float3 average = 0; float weight_sum = 0, mean = 0, m2 = 0;
    [unroll] for (int y = -2; y <= 2; ++y) [unroll] for (int x = -2; x <= 2; ++x) {
        int2 q = int2(p) + int2(x, y);
        if (!lit_ssil_surface(q) || lit_base_metallic.Load(int3(q, 0)).w >= .999) continue;
        float3 q_normal = lit_safe_normal(lit_normal_roughness.Load(int3(q, 0)).xyz);
        float3 delta = lit_ssil_world(q) - position;
        float plane_tolerance = max(.0005, pixel_size * .25);
        if (dot(normal, q_normal) < .9995 || abs(dot(delta,lit_geometry_normal_at(p))) > plane_tolerance ||
            abs(dot(delta,lit_geometry_normal_at(q))) > plane_tolerance) continue;
        // Include zero-confidence misses: conditioning only on hits would
        // overestimate bounce energy and erase the isotropic fallback contract.
        float3 value = lit_ssil_finite(lit_ssil_raw.Load(int3(q, 0)).rgb);
        float weight = exp(-.3 * (x * x + y * y));
        float luma = dot(value, float3(.2126, .7152, .0722));
        float next_weight = weight_sum + weight, difference = luma - mean;
        m2 += difference * difference * weight_sum * weight / max(next_weight, 1e-8);
        mean += difference * weight / max(next_weight, 1e-8); weight_sum = next_weight;
        average += value * weight;
    }
    average /= max(weight_sum, 1e-8);
    float sigma = sqrt(max(0, m2 / max(weight_sum, 1e-8)));
    float4 motion = lit_motion.Load(int3(p, 0));
    float2 previous_uv = lit_ssil_uv(int2(p)) + motion.xy;
    float expected_depth = lit_previous_device_depth(p, motion), tolerance; float2 gradient;
    lit_ssil_previous_plane(position,lit_geometry_normal_at(p), pixel_size, expected_depth, tolerance, gradient);
    float4 history = 0; float support = 0;
    if ((lit_history_invalid & 16) == 0 && lit_size_frame.w != 0 && motion.w > .5 &&
        all(isfinite(previous_uv)) && isfinite(expected_depth) && all(previous_uv >= 0) && all(previous_uv <= 1)) {
        float2 location = previous_uv * lit_size_frame.xy - .5;
        int2 base = int2(floor(location)); float2 fraction = frac(location);
        [unroll] for (int y = 0; y < 2; ++y) [unroll] for (int x = 0; x < 2; ++x) {
            int2 q = base + int2(x, y);
            if (any(q < 0) || any(q >= int2(lit_size_frame.xy)) || !lit_inside(uint2(q))) continue;
            if (!lit_history_surface_matches(p, uint2(q))) continue;
            float depth = lit_history_depth.Load(int3(q, 0));
            float3 q_normal = lit_history_normal.Load(int3(q, 0)).xyz;
            float tap_depth = expected_depth + dot(lit_ssil_uv(q) - previous_uv, gradient);
            if (!isfinite(depth) || lit_is_background_depth(depth) || dot(q_normal, q_normal) < .25 ||
                abs(depth - tap_depth) > tolerance || dot(normal, lit_safe_normal(q_normal)) < .9995) continue;
            float4 value = lit_ssil_previous.Load(int3(q, 0));
            if (value.a < 1 || !all(isfinite(value))) continue;
            float weight = (x == 0 ? 1 - fraction.x : fraction.x) * (y == 0 ? 1 - fraction.y : fraction.y);
            history += value * weight; support += weight;
        }
    }
    float count = 1;
    if (support >= .1) {
        history /= support;
        float motion_pixels = length(motion.xy * lit_size_frame.xy);
        float maximum_count = min(lerp(47, 7, saturate(lit_temporal.z)),
                                  lerp(47, 4, saturate(lit_temporal.w)));
        float previous_count = min(history.a * support * lerp(1, .7, saturate(motion_pixels / 12)), maximum_count);
        float3 range = max(sigma * 4, baseline * .1 + .015);
        if (any(abs(history.rgb - average) > range * 2)) previous_count = min(previous_count, 7);
        float3 clipped = clamp(history.rgb, average - range, average + range);
        average = lerp(average, clipped, previous_count / (previous_count + 1)); count = previous_count + 1;
    }
    average = lit_ssil_bound(average, baseline);
    float3 corrected = max(0, lit_ssil_finite(baseline + clamp(lit_ssil_controls.y, 0, .5) * average));
    lit_ssil_history_output[p] = float4(average, count);
    lit_ssil_irradiance_output[p] = float4(corrected, 1);
}
