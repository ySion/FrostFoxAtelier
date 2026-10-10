RWStructuredBuffer<uint> lit_tile_counts_output;
RWStructuredBuffer<uint> lit_tile_indices_output;
StructuredBuffer<uint> lit_tile_counts;
StructuredBuffer<uint> lit_tile_indices;
RWTexture2D<float4> lit_direct_diffuse_raw_output;
RWTexture2D<float4> lit_direct_specular_raw_output;
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
uint lit_direct_filter_step;
Texture2D<float4> lit_history_normal;
Texture2D<float> lit_history_depth;
Texture2D<float4> lit_indirect_source;
Texture2D<float> lit_ao;
RWTexture2D<float4> lit_hdr_output;
uint lit_probe_debug;

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
groupshared float3 tile_minimum[256],tile_maximum[256];
[numthreads(16,16,1)]
void lit_tiles_cs(uint3 dispatch:SV_DispatchThreadID,uint3 group:SV_GroupID,uint lane:SV_GroupIndex) {
    bool surface=lit_direct_surface(int2(dispatch.xy));
    float3 p=surface?lit_direct_world(int2(dispatch.xy)):1e30.xxx;
    tile_minimum[lane]=p; tile_maximum[lane]=surface?p:-1e30.xxx; GroupMemoryBarrierWithGroupSync();
    for(uint stride=128;stride>0;stride>>=1) {
        if(lane<stride) {tile_minimum[lane]=min(tile_minimum[lane],tile_minimum[lane+stride]);
            tile_maximum[lane]=max(tile_maximum[lane],tile_maximum[lane+stride]);}
        GroupMemoryBarrierWithGroupSync();
    }
    if(lane!=0)return; uint tile=group.y*((lit_size_frame.x+15)/16)+group.x,count=0;
    for(uint i=0;i<lit_counts.x;i++) {
        LitLight light=lit_lights[i]; float3 delta=max(tile_minimum[0]-light.position_range.xyz,0)+max(light.position_range.xyz-tile_maximum[0],0);
        // Match the support of lit_sample_light: point samples lie on a sphere,
        // rectangle samples on their actual emitting rectangle.
        float source_extent=light.meta.x==0?light.direction_radius.w:
            .5*length(float2(light.right_width.w,light.up_height.w));
        float radius=light.position_range.w+source_extent;
        if(light.meta.x==2||dot(delta,delta)<=radius*radius) {
            if(count<64)lit_tile_indices_output[tile*64+count]=i; count++;
        }
    }
    lit_tile_counts_output[tile]=count;
}
[numthreads(8,8,1)]
void lit_direct_cs(uint3 tid:SV_DispatchThreadID) {
    uint2 p=tid.xy; if(any(p>=lit_size_frame.xy))return;
    lit_direct_diffuse_raw_output[p]=0; lit_direct_specular_raw_output[p]=0;
    if(!lit_direct_surface(int2(p)))return;
    float2 uv=(p+.5)/lit_size_frame.xy; float3 position=lit_world(uv,lit_depth[p]);
    float4 nr=lit_normal_roughness[p],bm=lit_base_metallic[p];
    float3 n=normalize(nr.xyz),v=lit_view_direction(uv),diffuse_result=0,specular_result=0;
    float3 diffuse_factor=lit_direct_diffuse_factor(bm),specular_factor=lit_direct_specular_factor(bm);
    uint tile=(p.y/16)*((lit_size_frame.x+15)/16)+p.x/16,count=lit_tile_counts[tile];
    uint iterations=count>64?lit_counts.x:count;
    for(uint k=0;k<iterations;k++) {
        uint index=count>64?k:lit_tile_indices[tile*64+k]; LitLight light=lit_lights[index];
        uint seed=lit_light_seed(p,index);
        uint samples=max(1,lit_counts.z); float3 diffuse_sum=0,specular_sum=0;
        for(uint s=0;s<samples;s++) {
            float3 l;float distance;float3 radiance=lit_sample_light(light,position,seed,l,distance);
            if(dot(n,l)<=0||!any(radiance>0))continue;
            float visibility=1;
            if(light.meta.y!=~0u && (lit_counts.w&1)!=0 && (light.meta.z&2u)!=0)
                visibility=lit_visible(position,n,l,distance)?1:0;
            float3 diffuse,specular;
            lit_brdf_components(bm.rgb,nr.w,bm.w,n,v,l,diffuse,specular);
            // Visibility is evaluated at THIS emitter sample, preserving the
            // correlation of GGX, emitter radiance and occlusion for f*L*V.
            diffuse_sum+=diffuse*radiance*visibility;
            specular_sum+=specular*radiance*visibility;
        }
        diffuse_result+=diffuse_sum/samples; specular_result+=specular_sum/samples;
    }
    // SSSR owns camera-path emitter specular until both techniques share MIS.
    // Mesh NEE supplies diffuse here and full BRDF transport at DDGI hits.
    if (lit_emitter_count > 0 && any(diffuse_factor > 0)) {
        uint emitter_seed = lit_light_seed(p, 0x6a09e667u);
        float3 emitter_diffuse, unused_specular;
        lit_emitter_lighting(position, n, v, bm.rgb, nr.w, bm.w,
            (lit_counts.w & 1u) != 0, false, emitter_seed, emitter_diffuse, unused_specular);
        diffuse_result += emitter_diffuse;
    }
    float3 diffuse_divisor=float3(diffuse_factor.r>0?diffuse_factor.r:1,
        diffuse_factor.g>0?diffuse_factor.g:1,diffuse_factor.b>0?diffuse_factor.b:1);
    lit_direct_diffuse_raw_output[p]=float4(lit_direct_finite(diffuse_result/diffuse_divisor),1);
    lit_direct_specular_raw_output[p]=float4(lit_direct_finite(specular_result/specular_factor),1);
}

bool lit_direct_neighbor(uint2 p, int2 q, bool specular, float3 position, float3 normal, float pixel_size) {
    if (!lit_direct_surface(q)) return false;
    float4 nr = lit_normal_roughness.Load(int3(p, 0));
    float4 q_nr = lit_normal_roughness.Load(int3(q, 0));
    float3 q_normal = lit_safe_normal(q_nr.xyz);
    // Strict normals and both tangent planes keep low-poly facets separate.
    // Plane error is in world units, independent of device-depth direction.
    float3 delta = lit_direct_world(q) - position;
    float tolerance = max(.0005, pixel_size * .25);
    if (dot(normal, q_normal) < .9995 || abs(dot(delta, normal)) > tolerance ||
        abs(dot(delta, q_normal)) > tolerance ||
        abs(nr.w - q_nr.w) > (specular ? .05 : .15)) return false;
    float4 material = lit_base_metallic.Load(int3(p, 0));
    float4 q_material = lit_base_metallic.Load(int3(q, 0));
    if (specular) return abs(material.w - q_material.w) <= .15;
    // A zero material channel contains no diffuse illumination estimate.
    // It must not darken a neighboring channel whose demodulator is nonzero.
    return all((lit_direct_diffuse_factor(material) > 1e-8) ==
               (lit_direct_diffuse_factor(q_material) > 1e-8));
}

void lit_direct_merge_moments(float mean, float variance, float weight,
                              inout float sum_weight, inout float result_mean, inout float result_m2) {
    float next_weight = sum_weight + weight;
    float delta = mean - result_mean;
    result_m2 += weight * max(variance, 0) + delta * delta * sum_weight * weight / max(next_weight, 1e-8);
    result_mean += delta * weight / max(next_weight, 1e-8);
    sum_weight = next_weight;
}

void lit_direct_local_statistics(Texture2D<float4> source, uint2 p, bool specular,
                                  float3 position, float3 normal, float pixel_size,
                                  out float3 average, out float2 moments) {
    average = 0;
    float weight_sum = 0, mean = 0, m2 = 0;
    [unroll] for (int y = -1; y <= 1; ++y) [unroll] for (int x = -1; x <= 1; ++x) {
        int2 q = int2(p) + int2(x, y);
        if (!lit_direct_neighbor(p, q, specular, position, normal, pixel_size)) continue;
        float4 signal = source.Load(int3(q, 0));
        if (signal.a <= 0 || !all(isfinite(signal))) continue;
        float weight = 1.0 / (1 + x * x + y * y);
        average += max(signal.rgb, 0) * weight;
        lit_direct_merge_moments(lit_direct_luminance(max(signal.rgb, 0)), 0, weight,
            weight_sum, mean, m2);
    }
    average /= max(weight_sum, 1e-8);
    moments = float2(mean, max(0, m2 / max(weight_sum, 1e-8)));
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

bool lit_direct_history(Texture2D<float4> source, Texture2D<float2> moment_source,
                          float2 previous_uv, float expected_depth, float3 normal, float roughness,
                          bool specular, float tolerance, float2 gradient,
                          out float4 history, out float2 moments, out float support) {
    history = 0; moments = 0; support = 0;
    if (lit_size_frame.w == 0 || !all(isfinite(previous_uv)) || !isfinite(expected_depth) ||
        any(previous_uv < 0) || any(previous_uv > 1)) return false;
    float2 location = previous_uv * lit_size_frame.xy - .5;
    int2 base = int2(floor(location)); float2 fraction = frac(location);
    float mean = 0, m2 = 0;
    [unroll] for (int y = 0; y < 2; ++y) [unroll] for (int x = 0; x < 2; ++x) {
        int2 q = base + int2(x, y);
        if (any(q < 0) || any(q >= int2(lit_size_frame.xy)) || !lit_inside(uint2(q))) continue;
        float depth = lit_history_depth.Load(int3(q, 0));
        float4 nr = lit_history_normal.Load(int3(q, 0));
        float tap_depth = expected_depth + dot(lit_direct_uv(q) - previous_uv, gradient);
        if (lit_is_background_depth(depth) || !isfinite(depth) || dot(nr.xyz, nr.xyz) < .25 ||
            abs(depth - tap_depth) > tolerance || dot(normal, lit_safe_normal(nr.xyz)) < .9995 ||
            abs(roughness - nr.w) > (specular ? .05 : .15)) continue;
        float4 signal = source.Load(int3(q, 0));
        float2 sample_moments = moment_source.Load(int3(q, 0));
        if (signal.a < 1 || !all(isfinite(signal)) || !all(isfinite(sample_moments))) continue;
        float weight = (x == 0 ? 1 - fraction.x : fraction.x) * (y == 0 ? 1 - fraction.y : fraction.y);
        history += signal * weight;
        lit_direct_merge_moments(sample_moments.x, sample_moments.y, weight, support, mean, m2);
    }
    if (support < .1) return false;
    history /= support;
    moments = float2(mean, max(0, m2 / support));
    return true;
}

void lit_direct_resolve_signal(Texture2D<float4> raw, Texture2D<float4> previous,
                                 Texture2D<float2> previous_moments, uint2 p, bool specular,
                                 float3 position, float3 normal, float roughness, float pixel_size,
                                 float4 motion, float expected_depth, float tolerance, float2 gradient,
                                 out float4 result, out float2 result_moments) {
    float3 current = lit_direct_finite(raw.Load(int3(p, 0)).rgb);
    float current_luma = lit_direct_luminance(current);
    // The deterministic reference integrates the same fLV samples without
    // reconstruction bias. No-light frames must never retain a removed lamp.
    if (lit_counts.z == 64 || (lit_counts.x == 0 && lit_emitter_count == 0)) {
        result = float4(current, 1);
        result_moments = float2(current_luma, 0);
        return;
    }
    float3 average; float2 local;
    lit_direct_local_statistics(raw, p, specular, position, normal, pixel_size, average, local);
    float4 history; float2 history_moments; float support;
    bool valid = (lit_history_invalid & 1) == 0 && motion.w > .5 && lit_direct_history(previous, previous_moments, motion.xy + lit_direct_uv(int2(p)),
        expected_depth, normal, roughness, specular, tolerance, gradient, history, history_moments, support);
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
    uint2 p = id.xy; if (any(p >= lit_size_frame.xy)) return;
    lit_direct_diffuse_history_output[p] = 0; lit_direct_specular_history_output[p] = 0;
    lit_direct_diffuse_moments_output[p] = 0; lit_direct_specular_moments_output[p] = 0;
    if (!lit_direct_surface(int2(p))) return;
    float4 nr = lit_normal_roughness.Load(int3(p, 0));
    float3 normal = lit_safe_normal(nr.xyz), position = lit_direct_world(int2(p));
    float pixel_size = lit_direct_pixel_size(p);
    float4 motion = lit_motion.Load(int3(p, 0));
    float expected_depth = lit_previous_device_depth(p, motion), tolerance;
    float2 gradient; lit_direct_previous_plane(p, position, normal, pixel_size, expected_depth, tolerance, gradient);
    float4 diffuse, specular; float2 diffuse_moments, specular_moments;
    lit_direct_resolve_signal(lit_direct_diffuse_raw, lit_direct_diffuse_previous,
        lit_direct_diffuse_moments_previous, p, false, position, normal, nr.w, pixel_size,
        motion, expected_depth, tolerance, gradient, diffuse, diffuse_moments);
    lit_direct_resolve_signal(lit_direct_specular_raw, lit_direct_specular_previous,
        lit_direct_specular_moments_previous, p, true, position, normal, nr.w, pixel_size,
        motion, expected_depth, tolerance, gradient, specular, specular_moments);
    lit_direct_diffuse_history_output[p] = diffuse; lit_direct_specular_history_output[p] = specular;
    lit_direct_diffuse_moments_output[p] = diffuse_moments; lit_direct_specular_moments_output[p] = specular_moments;
}

float4 lit_direct_filter_signal(Texture2D<float4> source, Texture2D<float2> moment_source,
                                  uint2 p, bool specular, float3 position, float3 normal,
                                  float roughness, float pixel_size) {
    float4 center = source.Load(int3(p, 0));
    if (lit_counts.z == 64 || (lit_counts.x == 0 && lit_emitter_count == 0)) return center;
    float2 moments = moment_source.Load(int3(p, 0));
    if (center.a <= 0 || !all(isfinite(center)) || !all(isfinite(moments))) return 0;
    float noise = sqrt(max(moments.y, 0) / max(center.a, 1));
    float relative_noise = saturate(noise / max(moments.x, .005));
    float radius = specular ? clamp(roughness * roughness * 8, 1, 4) : 4;
    radius *= lerp(.5, 1, relative_noise);
    uint step = max(1u, lit_direct_filter_step);
    if (step > radius || (specular && roughness < .04)) return center;
    float3 sum = 0; float weight_sum = 0;
    float center_luma = lit_direct_luminance(center.rgb);
    // Three CPU-dispatched a-trous steps (1,2,4), with independent footprints
    // for diffuse transport and the actual GGX roughness of the specular lobe.
    [unroll] for (int y = -2; y <= 2; ++y) [unroll] for (int x = -2; x <= 2; ++x) {
        if (length(float2(x, y)) * step > radius * 1.5 && (x != 0 || y != 0)) continue;
        int2 q = int2(p) + int2(x, y) * int(step);
        if (!lit_direct_neighbor(p, q, specular, position, normal, pixel_size)) continue;
        float4 sample = source.Load(int3(q, 0));
        if (sample.a <= 0 || !all(isfinite(sample))) continue;
        float luma = lit_direct_luminance(sample.rgb);
        // The receiver's uncertainty sets its reconstruction support. A noisy
        // bright neighbor cannot make a converged occluded receiver uncertain
        // and leak through an intervening wall absent from the pixel centers.
        float range = max(.002, noise * 3 + .02 * max(center_luma, luma));
        float kx = x == 0 ? 6 : (abs(x) == 1 ? 4 : 1);
        float ky = y == 0 ? 6 : (abs(y) == 1 ? 4 : 1);
        float weight = kx * ky * exp(-abs(luma - center_luma) / range);
        sum += max(sample.rgb, 0) * weight; weight_sum += weight;
    }
    return float4(lit_direct_finite(sum / max(weight_sum, 1e-8)), center.a);
}

[numthreads(8,8,1)]
void lit_direct_filter_cs(uint3 id : SV_DispatchThreadID) {
    uint2 p = id.xy; if (any(p >= lit_size_frame.xy)) return;
    lit_direct_diffuse_filter_output[p] = 0; lit_direct_specular_filter_output[p] = 0;
    if (!lit_direct_surface(int2(p))) return;
    float4 nr = lit_normal_roughness.Load(int3(p, 0));
    float3 position = lit_direct_world(int2(p)), normal = lit_safe_normal(nr.xyz);
    float pixel_size = lit_direct_pixel_size(p);
    lit_direct_diffuse_filter_output[p] = lit_direct_filter_signal(lit_direct_diffuse_filter_input,
        lit_direct_diffuse_moments, p, false, position, normal, nr.w, pixel_size);
    lit_direct_specular_filter_output[p] = lit_direct_filter_signal(lit_direct_specular_filter_input,
        lit_direct_specular_moments, p, true, position, normal, nr.w, pixel_size);
}

[numthreads(8,8,1)]
void lit_compose_cs(uint3 tid:SV_DispatchThreadID) {
    uint2 p=tid.xy;if(any(p>=lit_size_frame.xy))return;
    if(!lit_inside(p)||lit_is_background_depth(lit_depth[p])) {
        // Sky Light supplies incident radiance, not a visible sky surface.
        lit_hdr_output[p]=float4(0,0,0,1);return;
    }
    if(lit_probe_debug!=0){lit_hdr_output[p]=float4(lit_indirect_source[p].rgb,1);return;}
    float4 bm=lit_base_metallic[p]; float ao=(lit_counts.w&4)!=0?lit_ao[p]:1;
    float3 irradiance=(lit_counts.w&(2|64))!=0?lit_indirect_source[p].rgb:lit_environment.rgb*LIT_PI;
    float3 diffuse=bm.rgb*(1-bm.w)*irradiance/LIT_PI*ao;
    float3 direct_diffuse=lit_direct_diffuse_filter_input.Load(int3(p,0)).rgb*lit_direct_diffuse_factor(bm);
    float3 direct_specular=lit_direct_specular_filter_input.Load(int3(p,0)).rgb*lit_direct_specular_factor(bm);
    // Preserve the removed RGBA16F material-resolve store's rounding. Depth,
    // normals and all geometric correspondence keep their existing precision.
    float3 direct=f16tof32(f32tof16(lit_direct_finite(direct_diffuse+direct_specular)));
    lit_hdr_output[p]=float4(min(direct+diffuse+lit_emissive[p].rgb,65504.xxx),1);
}
