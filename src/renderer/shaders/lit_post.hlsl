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
uint lit_depth_source_is_device;
uint lit_depth_level_count;
RWTexture2D<float> lit_ao_output;

Texture2D<float2> lit_brdf_lut;
RWTexture2D<float2> lit_brdf_output;
RWStructuredBuffer<uint> lit_reflection_work;
RWStructuredBuffer<uint> lit_reflection_counter;
RWStructuredBuffer<uint> lit_reflection_arguments;
RWTexture2D<float4> lit_reflection_output;
RWTexture2D<float4> lit_reflection_hit_trace_output;
Texture2D<float4> lit_reflection_trace;
Texture2D<float4> lit_reflection_hit_trace;
Texture2D<float4> lit_reflection_previous;
Texture2D<float4> lit_reflection_hit_previous;
RWTexture2D<float4> lit_reflection_resolved_output;
RWTexture2D<float4> lit_reflection_hit_output;

Texture2D<float4> lit_reflection;
Texture2D<float4> lit_taa_previous;
Texture2D<float4> lit_history_normal;
Texture2D<float> lit_history_depth;
RWTexture2D<float4> lit_taa_output;
RWTexture2D<float4> lit_history_normal_output;
RWTexture2D<float> lit_history_depth_output;
Texture2D<float4> lit_taa;
Texture2D<float4> lit_bloom;
Texture2D<float4> lit_bloom_input;
RWTexture2D<float4> lit_bloom_output;
int2 lit_bloom_direction;
float4 lit_bloom_controls; // exposed threshold, soft knee, intensity, exposed clamp

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
bool lit_post_surface(uint2 p) {
    return lit_inside(p) && lit_depth.Load(int3(p, 0)) < 1 &&
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
[numthreads(8, 8, 1)]
void lit_gtao_cs(uint3 id : SV_DispatchThreadID) {
    uint2 p = id.xy;
    if (any(p >= lit_size_frame.xy)) return;
    if (!lit_post_surface(p) || lit_controls.z <= 0 || (lit_counts.w & 4) == 0) { lit_ao_output[p] = 1; return; }
    float2 uv = lit_post_uv(p);
    float depth = lit_depth.Load(int3(p, 0));
    float3 center = lit_world(uv, depth);
    float3 view_direction = lit_view_direction(uv);
    float3 normal = lit_safe_normal(lit_normal_roughness.Load(int3(p, 0)).xyz);
    float radius = lit_controls.z;
    float pixel_size = max(lit_post_pixel_world_size(uv, depth), 1e-6);
    float pixel_radius = clamp(radius / pixel_size, 1, 64);
    // Bias only the horizon origin; stored low-poly surface normals stay intact.
    center += view_direction * pixel_size * .05;
    uint seed = lit_hash(p.x + p.y * lit_size_frame.x + lit_size_frame.z * 747796405u);
    float rotation = lit_random(seed);
    float step_noise = lit_random(seed);
    float visibility = 0;
    [unroll] for (uint slice = 0; slice < 3; ++slice) {
        float phi = (slice + rotation) * (LIT_PI / 3);
        float2 screen_direction = float2(cos(phi), sin(phi));
        float2 tangent_uv = uv + screen_direction / lit_size_frame.xy;
        float3 tangent = lit_world(tangent_uv, depth) - lit_world(uv, depth);
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
            [unroll] for (uint side = 0; side < 2; ++side) {
                float sign_direction = side == 0 ? -1 : 1;
                float2 sample_uv = uv + sign_direction * screen_direction * offset / lit_size_frame.xy;
                if (!lit_post_inside_uv(sample_uv)) continue;
                int2 sample_pixel = lit_post_pixel(sample_uv);
                float sample_depth = lit_depth.Load(int3(sample_pixel, 0));
                if (sample_depth >= 1) continue;
                float3 delta = lit_world(lit_post_uv(sample_pixel), sample_depth) - center;
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

// Every level is a DIFFERENT RG32F image. Never bind an image simultaneously
// as an SRV and UAV, even when addressing different logical hierarchy levels.
[numthreads(8, 8, 1)]
void lit_depth_pyramid_cs(uint3 id : SV_DispatchThreadID) {
    uint2 p = id.xy;
    if (any(p >= lit_depth_output_size)) return;
    if (lit_depth_source_is_device != 0) {
        float z = lit_inside(p) ? lit_depth.Load(int3(p, 0)) : 1;
        lit_depth_output[p] = float2(z, z);
        return;
    }
    uint width, height;
    lit_depth_input.GetDimensions(width, height);
    float2 range = float2(1, 0);
    [unroll] for (uint y = 0; y < 2; ++y) [unroll] for (uint x = 0; x < 2; ++x) {
        uint2 q = p * 2 + uint2(x, y);
        if (q.x >= width || q.y >= height) continue;
        float2 sample_range = lit_depth_input.Load(int3(q, 0));
        range.x = min(range.x, sample_range.x);
        range.y = max(range.y, sample_range.y);
    }
    lit_depth_output[p] = range;
}

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
[numthreads(1, 1, 1)]
void lit_ssr_reset_cs(uint3 id : SV_DispatchThreadID) { lit_reflection_counter[0] = 0; }
[numthreads(8, 8, 1)]
void lit_ssr_classify_cs(uint3 id : SV_DispatchThreadID) {
    uint2 p = id.xy;
    if (any(p >= lit_size_frame.xy)) return;
    lit_reflection_hit_trace_output[p] = 0;
    if (!lit_post_surface(p)) { lit_reflection_output[p] = 0; return; }
    lit_reflection_output[p] = float4(lit_post_environment(p), 0);
    float4 nr = lit_normal_roughness.Load(int3(p, 0));
    float nv = dot(lit_safe_normal(nr.xyz), lit_view_direction(lit_post_uv(p)));
    // Compact only pixels with a visible, sufficiently smooth specular lobe.
    // Work allocation has a strict one-entry-per-window-pixel capacity contract.
    if ((lit_counts.w & 8) == 0 || nv <= .005 || nr.w > lit_temporal.y || lit_controls.w <= 0) return;
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
    uint2 size;
    lit_depth_pyramid[NonUniformResourceIndex(level)].GetDimensions(size.x, size.y);
    // Ceil-half images retain two-parent-pixels per cell. Their UV boundaries
    // must follow that footprint instead of stretching odd-sized last cells.
    grid_size = float2(lit_size_frame.xy) / float(1u << level);
    int2 cell = clamp(int2(floor(uv * grid_size)), int2(0, 0), int2(size) - 1);
    return lit_depth_pyramid[NonUniformResourceIndex(level)].Load(int3(cell, 0));
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
        bool overlap = range.x < 1 && ray_max >= range.x - depth_thickness &&
                       ray_min <= range.y + depth_thickness;
        if (overlap && level > 0) { --level; continue; }
        if (overlap) {
            float candidate_time = abs(direction.z) > 1e-9 ? (range.x - origin.z) / direction.z : time;
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
                if (z < 1 && along_ray > max(lit_controls.y * 8, .04) &&
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
    uint seed = lit_hash(packed ^ (lit_size_frame.z * 2891336453u));
    float alpha = max(nr.w * nr.w, .0025);
    float3 local_h = lit_post_visible_ggx(local_v, alpha, float2(lit_random(seed), lit_random(seed)));
    float3 h = local_h.x * tangent + local_h.y * bitangent + local_h.z * n;
    float3 ray = lit_safe_normal(reflect(-v, h));
    float nl = saturate(dot(n, ray)), nv = max(saturate(dot(n, v)), 1e-4);
    if (nl <= 0) return;
    float3 origin = lit_post_world(p) + n * max(lit_controls.y * 2, .005);
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
    lit_reflection_output[p] = float4(lit_post_finite(reflection), confidence);
    lit_reflection_hit_trace_output[p] = hit;
}

float lit_post_previous_depth_tolerance(float3 position, float3 view_direction) {
    float4 a = mul(float4(position, 1), lit_previous_view_projection);
    float4 b = mul(float4(position + view_direction * .035, 1), lit_previous_view_projection);
    if (abs(a.w) < 1e-6 || abs(b.w) < 1e-6) return 0;
    return max(2e-5, abs(a.z / a.w - b.z / b.w));
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
    if (abs(determinant) < 1e-12) return 0;
    float2 gradient = float2(db * ac.y - dc * ab.y, dc * ab.x - db * ac.x) / determinant;
    return all(isfinite(gradient)) ? gradient : 0;
}
bool lit_post_valid_history(float2 previous_uv, float expected_depth, float3 current_normal,
                             float depth_tolerance, float2 depth_gradient) {
    if (lit_size_frame.w == 0 || !lit_post_inside_uv(previous_uv)) return false;
    int2 q = lit_post_pixel(previous_uv);
    float previous_depth = lit_history_depth.Load(int3(q, 0));
    float3 previous_normal = lit_safe_normal(lit_history_normal.Load(int3(q, 0)).xyz);
    float tap_depth = expected_depth + dot(lit_post_uv(q) - previous_uv, depth_gradient);
    return previous_depth < 1 && abs(previous_depth - tap_depth) <= depth_tolerance &&
           dot(current_normal, previous_normal) >= .94;
}
bool lit_post_sample_history(Texture2D<float4> source, float2 previous_uv, float expected_depth,
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
        if (!lit_post_valid_history(q_uv, tap_depth, normal, tolerance, depth_gradient)) continue;
        float weight = (x == 0 ? 1 - fraction.x : fraction.x) * (y == 0 ? 1 - fraction.y : fraction.y);
        history += source.Load(int3(q, 0)) * weight; weight_sum += weight;
    }
    if (weight_sum < .1) return false;
    history /= weight_sum;
    return true;
}
float lit_post_response(float2 motion) {
    // Random BRDF/visibility samples are radiance variance, not a flash event.
    // Scene/light edits have an explicit current-frame reactive signal. Color
    // clipping handles local disagreement without canceling Monte Carlo reuse.
    float motion_pixels = length(motion * lit_size_frame.xy);
    return (1 - saturate(lit_temporal.z)) *
           lerp(1, .25, saturate(motion_pixels / 12));
}
[numthreads(8, 8, 1)]
void lit_reflection_resolve_cs(uint3 id : SV_DispatchThreadID) {
    uint2 p = id.xy;
    if (any(p >= lit_size_frame.xy)) return;
    float4 center = lit_reflection_trace.Load(int3(p, 0));
    float4 hit = lit_reflection_hit_trace.Load(int3(p, 0));
    lit_reflection_hit_output[p] = hit;
    if (!lit_post_surface(p)) { lit_reflection_resolved_output[p] = 0; return; }
    float2 uv = lit_post_uv(p);
    float4 nr = lit_normal_roughness.Load(int3(p, 0));
    float3 normal = lit_safe_normal(nr.xyz), position = lit_post_world(p);
    float pixel_world = max(lit_post_pixel_world_size(uv, lit_depth.Load(int3(p, 0))), .001);
    float3 sum = center.rgb; float weight_sum = 1;
    float3 center_encoded = lit_post_ycocg(center.rgb);
    float3 moment1 = center_encoded, moment2 = center_encoded * center_encoded;
    [unroll] for (int y = -2; y <= 2; ++y) [unroll] for (int x = -2; x <= 2; ++x) {
        if (x == 0 && y == 0) continue;
        int2 q = int2(p) + int2(x, y);
        if (any(q < 0) || any(q >= int2(lit_size_frame.xy)) || !lit_post_surface(q)) continue;
        float4 neighbor_normal = lit_normal_roughness.Load(int3(q, 0));
        if (dot(normal, lit_safe_normal(neighbor_normal.xyz)) < .96 ||
            abs(nr.w - neighbor_normal.w) > .08 ||
            abs(dot(lit_post_world(q) - position, normal)) > max(.005, pixel_world * .5)) continue;
        float3 value = lit_reflection_trace.Load(int3(q, 0)).rgb;
        float3 encoded = lit_post_ycocg(value);
        // Mirrors keep their current sharp sample. Rough lobes can pool nearby
        // independent stochastic samples without blending geometric hard edges.
        float weight = saturate(nr.w * 6) / (1 + x * x + y * y);
        sum += value * weight; weight_sum += weight;
        moment1 += encoded * weight; moment2 += encoded * encoded * weight;
    }
    float3 current = sum / weight_sum;
    float3 mean = moment1 / weight_sum;
    float3 spatial_variance = max(0, moment2 / weight_sum - mean * mean);
    float3 spatial_sigma = sqrt(spatial_variance);
    float4 motion = lit_motion.Load(int3(p, 0));
    float2 previous_uv = uv + motion.xy;
    float tolerance = lit_post_previous_depth_tolerance(position, lit_view_direction(uv));
    float2 gradient = lit_post_previous_depth_gradient(position, normal);
    float expected_depth = lit_previous_device_depth(p, motion);
    float4 previous_signal;
    bool valid = (lit_counts.w & 8) != 0 && motion.w > .5 &&
        lit_post_sample_history(lit_reflection_previous, previous_uv, expected_depth, normal,
                                tolerance, gradient, previous_signal);
    if (valid && hit.w > 0) {
        int2 hit_pixel = lit_post_pixel(hit.xy);
        float4 hit_motion = lit_motion.Load(int3(hit_pixel, 0));
        float2 previous_hit_uv = hit.xy + hit_motion.xy;
        float3 hit_normal = lit_safe_normal(lit_normal_roughness.Load(int3(hit_pixel, 0)).xyz);
        float3 hit_position = lit_world(hit.xy, lit_depth.Load(int3(hit_pixel, 0)));
        float hit_tolerance = lit_post_previous_depth_tolerance(hit_position, lit_view_direction(hit.xy));
        float2 hit_gradient = lit_post_previous_depth_gradient(hit_position, hit_normal);
        valid = hit_motion.w > .5 && lit_post_valid_history(previous_hit_uv,
            lit_previous_device_depth(hit_pixel, hit_motion), hit_normal, hit_tolerance, hit_gradient);
        // Independent GGX rays estimate one rough-lobe integral and need not
        // hit the same pixel or even share hit/miss status. Reproject the CURRENT
        // hit to reject real disocclusion. Only near-delta lobes require the
        // previous ray association in addition to surface correspondence.
        if (valid && nr.w < .04) {
            float4 previous_hit = lit_reflection_hit_previous.Load(int3(lit_post_pixel(previous_uv), 0));
            float hit_uv_error = length((previous_hit.xy - previous_hit_uv) * lit_size_frame.xy);
            valid = previous_hit.w > 0 && hit_uv_error < 4 &&
                abs(previous_hit.w - hit.w) < max(.15, hit.w * .04);
        }
    } else if (valid && nr.w < .04) {
        valid = lit_reflection_hit_previous.Load(int3(lit_post_pixel(previous_uv), 0)).w <= 0;
    }
    float3 result = current;
    float result_variance = spatial_variance.x;
    if (valid) {
        float3 previous = previous_signal.rgb;
        float history_weight = min(lit_temporal.x, .95) * lit_post_response(motion.xy);
        // Alpha stores luminance standard deviation, not trace confidence.
        // Persistent variance prevents sparse valid rays followed by random
        // misses from repeatedly clipping the legitimate accumulated mean away.
        float temporal_sigma = max(spatial_sigma.x, previous_signal.a);
        float3 sigma = max(spatial_sigma, float3(temporal_sigma, temporal_sigma * .5, temporal_sigma * .5));
        float3 range = max(sigma * 3, .002.xxx);
        previous = lit_post_rgb(clamp(lit_post_ycocg(previous), mean - range, mean + range));
        result = lerp(current, previous, history_weight);
        float mean_delta = lit_post_luminance(current) - lit_post_luminance(previous_signal.rgb);
        result_variance = lerp(spatial_variance.x, previous_signal.a * previous_signal.a, history_weight) +
                          history_weight * (1 - history_weight) * mean_delta * mean_delta;
    }
    lit_reflection_resolved_output[p] = float4(lit_post_finite(result), min(sqrt(max(result_variance, 0)), 65000));
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
    lit_history_depth_output[p] = lit_inside(p) ? depth : 1;
    lit_history_normal_output[p] = lit_inside(p) ? nr : 0;
    if (!lit_inside(p)) { lit_taa_output[p] = 0; return; }
    float3 current = lit_post_current_hdr(p);
    if (!lit_post_surface(p)) { lit_taa_output[p] = float4(current, 1); return; }
    float2 uv = lit_post_uv(p);
    float3 position = lit_world(uv, depth), normal = lit_safe_normal(nr.xyz);
    float4 motion = lit_motion.Load(int3(p, 0));
    float2 previous_uv = uv + motion.xy;
    float tolerance = lit_post_previous_depth_tolerance(position, lit_view_direction(uv));
    float2 gradient = lit_post_previous_depth_gradient(position, normal);
    float expected_depth = lit_previous_device_depth(p, motion);
    if ((lit_counts.w & 16) == 0 || motion.w <= .5 ||
        !lit_post_valid_history(previous_uv, expected_depth, normal, tolerance, gradient)) {
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
            abs(dot(lit_post_world(q) - position, normal)) > max(.005, pixel_world * .5)) continue;
        float3 encoded = lit_post_ycocg(lit_post_current_hdr(q));
        minimum = min(minimum, encoded); maximum = max(maximum, encoded);
        moment1 += encoded; moment2 += encoded * encoded; ++count;
    }
    float3 mean = moment1 / count;
    float3 sigma = sqrt(max(0, moment2 / count - mean * mean));
    minimum = max(minimum, mean - sigma * 1.25);
    maximum = min(maximum, mean + sigma * 1.25);
    // The shared footprint reconstruction validates each tap against the
    // projected plane, preserving low-poly edges without rejecting plane slope.
    float4 history_signal;
    if (!lit_post_sample_history(lit_taa_previous, previous_uv, expected_depth, normal,
                                tolerance, gradient, history_signal)) {
        lit_taa_output[p] = float4(current, 1); return;
    }
    float3 history = history_signal.rgb;
    float history_weight = min(lit_temporal.x, .95) * lit_post_response(motion.xy);
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
float4 lit_tonemap_ps(float4 position : SV_Position, float2 input_uv : TEXCOORD0) : SV_Target {
    uint2 p = uint2(position.xy);
    if (!lit_inside(p)) return 0;
    float2 uv = lit_post_uv(p);
    float3 bloom = (lit_counts.w & 32) != 0 ? lit_bloom.SampleLevel(lit_linear_sampler, uv, 0).rgb : 0;
    float3 color = lit_post_finite(lit_taa.Load(int3(p, 0)).rgb +
                                  bloom * clamp(lit_bloom_controls.z, 0, 1));
    color = lit_post_finite(color * max(lit_controls.x, 0));
    // Bounded filmic rational curve in scene-linear RGB. Output remains LINEAR
    // SDR; the sRGB RTV performs the one and only display transfer encoding.
    float3 mapped = saturate((color * (2.51 * color + .03)) /
                            (color * (2.43 * color + .59) + .14));
    return float4(mapped, 1);
}
