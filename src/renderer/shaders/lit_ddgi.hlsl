// Finite world-space DDGI. lit_common.hlsl is prepended by the renderer.
// Each probe owns an 8x8 octahedral tile. RG32F distance moments are normalized
// by maxRayDistance; this is a unit convention, not a reduction to FP16.
// All histories are ping-ponged; no resource is read while it is a UAV.
float4 lit_probe_origin; // first grid point xyz, maximum ray distance
float4 lit_probe_spacing; // grid spacing xyz, reserved
float4 lit_probe_history; // sampled irradiance/distance/display old weights xyz; w reserved
uint lit_probe_sequence; // Advances only with this field's updates; resets with its lifetime.
uint4 lit_probe_grid; // XYZ counts, total probe count
uint4 lit_probe_update; // rotating start, bounded count, illumination rays (64/128/256), reset/debug bits

Texture2D<float4> lit_probe_irradiance_previous;
Texture2D<float4> lit_probe_target_previous;
Texture2D<float2> lit_probe_distance_previous;
Texture2D<float4> lit_probe_offsets_previous;
RWTexture2D<float4> lit_probe_irradiance_output;
RWTexture2D<float4> lit_probe_target_output;
RWTexture2D<float2> lit_probe_distance_output;
RWTexture2D<float4> lit_probe_offsets_output;
RWTexture2D<float4> lit_indirect; // irradiance, before surface albedo / pi

static const uint DDGI_TILE_SIZE = 8;
static const uint DDGI_MAX_RAYS = 256;
static const uint DDGI_FIXED_RAYS = 32;

float3 ddgi_finite_radiance(float3 radiance) {
    // The irradiance atlas is linear HDR FP16. Clamp only at its representable
    // ceiling; exposure is applied later and is not part of the light field.
    return all(isfinite(radiance)) ? clamp(radiance, 0, 65504) : 0;
}

uint3 ddgi_probe_coordinates(uint probe) {
    uint x = probe % lit_probe_grid.x;
    uint yz = probe / lit_probe_grid.x;
    return uint3(x, yz % lit_probe_grid.y, yz / lit_probe_grid.y);
}

uint2 ddgi_probe_texel(uint probe) {
    return uint2(probe % lit_probe_grid.x, probe / lit_probe_grid.x);
}

uint ddgi_interleaved_probe(uint visit) {
    // A coprime stride is a bijection for every supported grid count. Nearby
    // visits spread through the volume rather than revealing updated slabs.
    // Grid axes are bounded to 32 on the host, so this product fits uint32.
    uint total = lit_probe_grid.w;
    uint stride = max(1u, uint(float(total) * .61803398875));
    [loop] for (;;) {
        uint a = total, b = stride;
        [loop] while (b != 0) { uint remainder = a % b; a = b; b = remainder; }
        if (a == 1) break;
        ++stride;
    }
    return (visit * stride) % total;
}

float3 ddgi_probe_position(uint probe, float3 offset) {
    return lit_probe_origin.xyz + float3(ddgi_probe_coordinates(probe)) *
        lit_probe_spacing.xyz + offset;
}

float2 ddgi_sign_not_zero(float2 value) {
    return float2(value.x >= 0 ? 1 : -1, value.y >= 0 ? 1 : -1);
}

float2 ddgi_octahedral_encode(float3 direction) {
    direction /= max(dot(abs(direction), 1.0.xxx), 1e-8);
    float2 encoded = direction.xy;
    if (direction.z < 0) encoded = (1 - abs(encoded.yx)) * ddgi_sign_not_zero(encoded);
    return encoded * .5 + .5;
}

float3 ddgi_octahedral_decode(float2 uv) {
    float2 encoded = uv * 2 - 1;
    float3 direction = float3(encoded, 1 - abs(encoded.x) - abs(encoded.y));
    if (direction.z < 0) direction.xy =
        (1 - abs(direction.yx)) * ddgi_sign_not_zero(direction.xy);
    return lit_safe_normal(direction);
}

// Fold bilinear taps at the octahedron seams, instead of sampling another
// probe's tile. This avoids a separate border pass and padded atlas storage.
int2 ddgi_fold_texel(int2 texel) {
    const int size = 8;
    if (texel.x < 0) { texel.x = -texel.x - 1; texel.y = size - 1 - texel.y; }
    if (texel.x >= size) { texel.x = 2 * size - texel.x - 1; texel.y = size - 1 - texel.y; }
    if (texel.y < 0) { texel.y = -texel.y - 1; texel.x = size - 1 - texel.x; }
    if (texel.y >= size) { texel.y = 2 * size - texel.y - 1; texel.x = size - 1 - texel.x; }
    return clamp(texel, 0, size - 1);
}

float4 ddgi_sample_tile(Texture2D<float4> atlas, uint probe, float3 direction) {
    float2 location = ddgi_octahedral_encode(direction) * DDGI_TILE_SIZE - .5;
    int2 lower = int2(floor(location));
    float2 fraction = frac(location);
    int2 tile = int2(ddgi_probe_texel(probe) * DDGI_TILE_SIZE);
    float4 a = atlas.Load(int3(tile + ddgi_fold_texel(lower), 0));
    float4 b = atlas.Load(int3(tile + ddgi_fold_texel(lower + int2(1, 0)), 0));
    float4 c = atlas.Load(int3(tile + ddgi_fold_texel(lower + int2(0, 1)), 0));
    float4 d = atlas.Load(int3(tile + ddgi_fold_texel(lower + int2(1, 1)), 0));
    return lerp(lerp(a, b, fraction.x), lerp(c, d, fraction.x), fraction.y);
}

float2 ddgi_sample_distance(uint probe, float3 direction) {
    float2 location = ddgi_octahedral_encode(direction) * DDGI_TILE_SIZE - .5;
    int2 lower = int2(floor(location));
    float2 fraction = frac(location);
    int2 tile = int2(ddgi_probe_texel(probe) * DDGI_TILE_SIZE);
    float2 a = lit_probe_distance_previous.Load(int3(tile + ddgi_fold_texel(lower), 0));
    float2 b = lit_probe_distance_previous.Load(int3(tile + ddgi_fold_texel(lower + int2(1, 0)), 0));
    float2 c = lit_probe_distance_previous.Load(int3(tile + ddgi_fold_texel(lower + int2(0, 1)), 0));
    float2 d = lit_probe_distance_previous.Load(int3(tile + ddgi_fold_texel(lower + int2(1, 1)), 0));
    return lerp(lerp(a, b, fraction.x), lerp(c, d, fraction.x), fraction.y);
}

float ddgi_minimum_spacing() {
    return max(1e-3, min(lit_probe_spacing.x, min(lit_probe_spacing.y, lit_probe_spacing.z)));
}

float3 ddgi_irradiance(float3 position, float3 normal) {
    if (any(lit_probe_grid.xyz < 2) || lit_probe_grid.w == 0 ||
        !all(isfinite(position)) || !all(isfinite(normal))) return 0;

    float3 spacing = max(lit_probe_spacing.xyz, 1e-3);
    // Match the query clearance to the probe relocation clearance. The 8x8
    // directional moment filter covers a cone; a smaller bias self-occludes
    // the wall/floor intersection. This remains camera independent.
    float3 biased_position = position + normal * ddgi_minimum_spacing() * .12;
    float3 grid_position = (biased_position - lit_probe_origin.xyz) / spacing;
    if (any(grid_position < 0) || any(grid_position > float3(lit_probe_grid.xyz - 1))) return 0;
    uint3 lower = uint3(clamp(floor(grid_position), 0, float3(lit_probe_grid.xyz - 2)));
    float3 fraction = saturate(grid_position - float3(lower));
    float3 accumulated = 0;
    float accumulated_weight = 0;

    [unroll] for (uint corner = 0; corner < 8; ++corner) {
        uint3 step = uint3(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1);
        uint3 coordinates = lower + step;
        uint probe = coordinates.x + lit_probe_grid.x *
            (coordinates.y + lit_probe_grid.y * coordinates.z);
        float4 offset = lit_probe_offsets_previous.Load(int3(ddgi_probe_texel(probe), 0));
        // w = 0: initializing/relocated, -1: inside geometry, +1: usable.
        if (offset.w < .5 || !all(isfinite(offset))) continue;

        float3 probe_position = ddgi_probe_position(probe, offset.xyz);
        float3 delta = biased_position - probe_position;
        float distance = length(delta);
        float3 direction = delta / max(distance, 1e-5);
        float2 moments = ddgi_sample_distance(probe, direction);
        if (!all(isfinite(moments))) continue;
        float normalized_distance = distance / max(lit_probe_origin.w, 1e-3);
        float distance_floor = ddgi_minimum_spacing() * .002 / max(lit_probe_origin.w, 1e-3);
        float variance = max(moments.y - moments.x * moments.x, distance_floor * distance_floor);
        float difference = max(normalized_distance - moments.x, 0);
        float visibility = difference == 0 ? 1 : variance / (variance + difference * difference);
        // Cubed Chebyshev visibility suppresses light bleeding across thin
        // blockers while retaining the distance field's angular information.
        visibility = visibility * visibility * visibility;
        float3 trilinear = lerp(1 - fraction, fraction, float3(step));
        float facing = max(.02, (dot(normal, -direction) + 1) * .5);
        float weight = facing * facing * visibility;
        if (weight < .2) weight *= weight * weight / .04;
        // Confidence shaping must not sharpen the spatial interpolation into
        // isolated probe cells. Keep the partition of unity linear.
        weight *= trilinear.x * trilinear.y * trilinear.z;
        float4 irradiance = ddgi_sample_tile(lit_probe_irradiance_previous, probe, normal);
        if (irradiance.w <= 0 || !all(isfinite(irradiance))) continue;
        accumulated += max(irradiance.rgb, 0) * weight;
        accumulated_weight += weight;
    }
    // This is a normalized interpolation, not ambient occlusion. A small
    // common confidence scale must not extinguish an otherwise valid field.
    return accumulated_weight > 0 ? ddgi_finite_radiance(accumulated / accumulated_weight) : 0;
}

[numthreads(64, 1, 1)]
void DDGICopy(uint3 dispatch_id : SV_DispatchThreadID) {
    uint width = lit_probe_grid.x * DDGI_TILE_SIZE;
    uint height = lit_probe_grid.y * lit_probe_grid.z * DDGI_TILE_SIZE;
    uint index = dispatch_id.x;
    if (index < width * height) {
        uint2 texel = uint2(index % width, index / width);
        if ((lit_probe_update.w & 1) != 0) {
            lit_probe_irradiance_output[texel] = 0;
            lit_probe_target_output[texel] = 0;
            lit_probe_distance_output[texel] = float2(1, 1);
        } else {
            float4 target = lit_probe_target_previous.Load(int3(texel, 0));
            float4 previous = lit_probe_irradiance_previous.Load(int3(texel, 0));
            lit_probe_target_output[texel] = target;
            // Sampling remains budgeted, while every valid probe approaches
            // its sampled target every frame. This removes the visible stop /
            // jump cadence without extra rays or a screen-space color history.
            float history = target.w > 0 && previous.w > 0 ? lit_probe_history.z : 0;
            float3 encoded = lerp(pow(max(target.rgb, 0), .2), pow(max(previous.rgb, 0), .2), history);
            lit_probe_irradiance_output[texel] = float4(ddgi_finite_radiance(pow(encoded, 5)), target.w);
            lit_probe_distance_output[texel] = lit_probe_distance_previous.Load(int3(texel, 0));
        }
    }
    if (index < lit_probe_grid.w) {
        uint2 texel = ddgi_probe_texel(index);
        lit_probe_offsets_output[texel] = (lit_probe_update.w & 1) != 0 ? 0 :
            lit_probe_offsets_previous.Load(int3(texel, 0));
    }
}

float3 ddgi_spherical_direction(uint ray_index, uint ray_count) {
    float z = 1 - 2 * (float(ray_index) + .5) / ray_count;
    float phi = 2 * LIT_PI * frac(float(ray_index) * .61803398875);
    float radius = sqrt(max(0, 1 - z * z));
    return float3(radius * cos(phi), radius * sin(phi), z);
}

float3 ddgi_hit_radiance(LitSurface surface, float3 toward_probe, inout uint seed) {
    float3 radiance = max(surface.emissive, 0);
    // Small light sets have no light-index lottery. For larger sets, two
    // uniform distinct indices have inclusion probability 2/N. The N/2
    // estimator remains unbiased and cannot choose the same light twice.
    uint samples = lit_counts.x <= 4 ? lit_counts.x : 2;
    uint first = 0, second = 0;
    if (lit_counts.x > 4) {
        first = min(uint(lit_random(seed) * lit_counts.x), lit_counts.x - 1);
        second = min(uint(lit_random(seed) * (lit_counts.x - 1)), lit_counts.x - 2);
        second += second >= first ? 1 : 0;
    }
    [loop] for (uint sample_index = 0; sample_index < samples; ++sample_index) {
        uint light_index = lit_counts.x <= 4 ? sample_index : (sample_index == 0 ? first : second);
        LitLight light = lit_lights[light_index];
        float3 direction;
        float distance;
        float3 incoming = lit_sample_light(light, surface.position,
            seed, direction, distance);
        if (dot(surface.normal, direction) > 0 && any(incoming > 0)) {
            bool visible = true;
            if ((light.meta.z & 2u) != 0)
                visible = lit_visible(surface.position, surface.normal, direction, distance);
            if (visible) radiance += incoming * lit_brdf(surface.base, surface.roughness, surface.metallic,
                surface.normal, toward_probe, direction) * (float(lit_counts.x) / samples);
        }
    }
    if ((lit_probe_update.w & 1) == 0) {
        // Only diffuse history feeds back. Bounded reflectance and the 0.85
        // transport factor make this iteration a contraction even in a closed
        // white room; direct emission is injected once at the actual hit.
        float3 reflectance = saturate(surface.base) * (1 - saturate(surface.metallic));
        radiance += ddgi_irradiance(surface.position, surface.normal) * reflectance * (.85 / LIT_PI);
    }
    return ddgi_finite_radiance(radiance);
}

float ddgi_trace_ray(float3 position, float3 direction, bool shade, inout uint seed, out float3 radiance) {
    RayDesc ray;
    ray.Origin = position;
    ray.Direction = direction;
    ray.TMin = max(1e-4, ddgi_minimum_spacing() * .001);
    ray.TMax = max(lit_probe_origin.w, ray.TMin * 2);
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    // Both fixed geometry and random illumination rays count as probe rays.
    // Secondary shadow rays are counted separately by lit_visible().
    lit_count_rays(1);
    query.TraceRayInline(lit_scene, 0, 255, ray);
    while (query.Proceed()) {}
    radiance = shade ? ddgi_finite_radiance(lit_environment.rgb) : 0;
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return ray.TMax;
    float distance = query.CommittedRayT();
    LitSurface surface = lit_hit_surface(query.CommittedInstanceID(),
        query.CommittedPrimitiveIndex(), query.CommittedTriangleBarycentrics(),
        position + direction * distance);
    // The mesh normal is the outward contract, including reflected instances.
    if (dot(surface.normal, direction) > 0) {
        radiance = 0;
        return -max(distance, 1e-5);
    }
    if (shade) radiance = ddgi_hit_radiance(surface, -direction, seed);
    return distance;
}

groupshared float ddgi_fixed_distances[32];
groupshared float3 ddgi_ray_directions[256];
groupshared float3 ddgi_ray_radiance[256];
groupshared float ddgi_ray_distances[256]; // negative means back-facing hit
groupshared float4 ddgi_ray_rotation;
groupshared uint ddgi_current_probe;
groupshared float ddgi_probe_state;
groupshared uint ddgi_discard_history;

[numthreads(64, 1, 1)]
void DDGIUpdate(uint3 group_id : SV_GroupID, uint lane : SV_GroupIndex) {
    if (group_id.x >= min(lit_probe_update.y, lit_probe_grid.w) || lit_probe_grid.w == 0) return;
    if (lane == 0) ddgi_current_probe = ddgi_interleaved_probe(
        (lit_probe_update.x + group_id.x) % lit_probe_grid.w);
    GroupMemoryBarrierWithGroupSync();
    uint probe = ddgi_current_probe;
    uint2 probe_texel = ddgi_probe_texel(probe);
    float4 old_offset = (lit_probe_update.w & 1) != 0 ? 0 :
        lit_probe_offsets_previous.Load(int3(probe_texel, 0));
    if (!all(isfinite(old_offset))) old_offset = 0;
    float3 position = ddgi_probe_position(probe, old_offset.xyz);
    uint ray_count = clamp(lit_probe_update.z, 64u, DDGI_MAX_RAYS);
    float maximum_distance = max(lit_probe_origin.w, 1e-3);

    // Geometry decisions must not follow the random illumination rotation.
    // Inactive probes retain these rays so moved geometry can reactivate them.
    if (lane < DDGI_FIXED_RAYS) {
        uint seed = 0;
        float3 unused_radiance;
        ddgi_fixed_distances[lane] = ddgi_trace_ray(position,
            ddgi_spherical_direction(lane, DDGI_FIXED_RAYS), false, seed, unused_radiance);
    }
    GroupMemoryBarrierWithGroupSync();

    if (lane == 0) {
        uint backfaces = 0, nearest_backface = 0, nearest_frontface = 0;
        float backface_distance = maximum_distance, frontface_distance = maximum_distance;
        [loop] for (uint i = 0; i < DDGI_FIXED_RAYS; ++i) {
            float d = ddgi_fixed_distances[i];
            if (d < 0) {
                ++backfaces;
                if (-d < backface_distance) { backface_distance = -d; nearest_backface = i; }
            } else if (d < frontface_distance) { frontface_distance = d; nearest_frontface = i; }
        }
        float clearance = ddgi_minimum_spacing() * .12;
        float3 target_offset = old_offset.xyz;
        ddgi_probe_state = 1;
        bool inside = backfaces > DDGI_FIXED_RAYS / 4;
        if (inside) {
            target_offset += ddgi_spherical_direction(nearest_backface, DDGI_FIXED_RAYS) *
                (backface_distance + clearance);
            ddgi_probe_state = -1;
        } else if (frontface_distance < clearance - ddgi_minimum_spacing() * .01) {
            target_offset -= ddgi_spherical_direction(nearest_frontface, DDGI_FIXED_RAYS) *
                (clearance - frontface_distance);
        }
        // A useful probe with valid history moves gently. Initialization,
        // pending relocation and fixed-ray inside classification have no
        // usable light field to preserve: reach the geometric target in one
        // visit instead of spending many budgeted visits on small steps.
        // Every target still stays in the cell ellipsoid, with the same
        // clearance deadband and no random-ray or return-to-center force.
        float3 move = target_offset - old_offset.xyz;
        float move_length = length(move);
        if (old_offset.w >= .5 && !inside)
            target_offset = old_offset.xyz + move * min(1, ddgi_minimum_spacing() * .04 / max(move_length, 1e-6));
        float3 normalized_offset = target_offset / max(lit_probe_spacing.xyz, 1e-3);
        target_offset *= min(1, .45 / max(length(normalized_offset), 1e-6));
        bool relocated = length(target_offset - old_offset.xyz) > ddgi_minimum_spacing() * .002;
        if (!relocated) target_offset = old_offset.xyz;
        if (relocated) ddgi_probe_state = 0;
        ddgi_discard_history = ((lit_probe_update.w & 1) != 0 || old_offset.w < .5 || relocated || inside) ? 1 : 0;
        lit_probe_offsets_output[probe_texel] = float4(target_offset, ddgi_probe_state);
        uint seed = lit_hash(probe ^ lit_hash(lit_probe_sequence + 0x9e3779b9));
        float u = lit_random(seed), a = 2 * LIT_PI * lit_random(seed), b = 2 * LIT_PI * lit_random(seed);
        ddgi_ray_rotation = float4(sqrt(1 - u) * sin(a), sqrt(1 - u) * cos(a),
                                  sqrt(u) * sin(b), sqrt(u) * cos(b));
    }
    GroupMemoryBarrierWithGroupSync();

    uint2 local_texel = uint2(lane % DDGI_TILE_SIZE, lane / DDGI_TILE_SIZE);
    uint2 atlas_texel = probe_texel * DDGI_TILE_SIZE + local_texel;
    if (ddgi_probe_state < .5) {
        // Rays were cast at the old position; never associate their radiance
        // or moments with a relocated origin. The next bounded visit retraces.
        lit_probe_irradiance_output[atlas_texel] = 0;
        lit_probe_target_output[atlas_texel] = 0;
        lit_probe_distance_output[atlas_texel] = float2(1, 1);
        return;
    }
    // The 64 lanes own texels, not a fixed ray budget. Each lane traces up to
    // four illumination rays; all are available to the convolution below.
    [loop] for (uint i = lane; i < ray_count; i += 64) {
        float3 direction = ddgi_spherical_direction(i, ray_count);
        direction += 2 * cross(ddgi_ray_rotation.xyz,
            cross(ddgi_ray_rotation.xyz, direction) + ddgi_ray_rotation.w * direction);
        uint seed = lit_hash(probe ^ lit_hash(i + DDGI_MAX_RAYS * lit_probe_sequence));
        float3 radiance;
        ddgi_ray_distances[i] = ddgi_trace_ray(position, direction, true, seed, radiance);
        ddgi_ray_directions[i] = direction;
        ddgi_ray_radiance[i] = radiance;
    }
    GroupMemoryBarrierWithGroupSync();
    float3 texel_direction = ddgi_octahedral_decode((float2(local_texel) + .5) / DDGI_TILE_SIZE);
    float3 irradiance = 0;
    float irradiance_weight = 0;
    float2 moments = 0;
    float distance_weight = 0;
    [loop] for (uint i = 0; i < ray_count; ++i) {
        float cosine = max(0, dot(texel_direction, ddgi_ray_directions[i]));
        irradiance += ddgi_ray_radiance[i] * cosine;
        irradiance_weight += cosine;
        float weight = pow(cosine, 32);
        float normalized_distance = max(ddgi_ray_distances[i], 0) / maximum_distance;
        moments += float2(normalized_distance, normalized_distance * normalized_distance) * weight;
        distance_weight += weight;
    }
    // Cosine normalization integrates a constant incident field to pi*L.
    irradiance = ddgi_finite_radiance(irradiance * LIT_PI / max(irradiance_weight, 1e-5));
    moments = distance_weight > 1e-8 ? moments / distance_weight : float2(1, 1);
    if (!all(isfinite(moments))) moments = float2(1, 1);
    moments = saturate(moments);

    float4 previous_irradiance = 0;
    float2 previous_distance = float2(1, 1);
    if (ddgi_discard_history == 0) {
        previous_irradiance = lit_probe_target_previous.Load(int3(atlas_texel, 0));
        previous_distance = lit_probe_distance_previous.Load(int3(atlas_texel, 0));
    }
    if (!all(isfinite(previous_irradiance))) previous_irradiance = 0;
    if (!all(isfinite(previous_distance))) previous_distance = float2(1, 1);
    float irradiance_history = previous_irradiance.w > 0 ? clamp(lit_probe_history.x, 0, .98) : 0;
    float distance_history = previous_irradiance.w > 0 ? clamp(lit_probe_history.y, 0, .98) : 0;
    // Running-average warmup uses the actual number of visits, not render
    // frames. The application selects response weights from scene changes;
    // a light change never lowers distance history via a luminance hint.
    float warmup_history = previous_irradiance.w / (previous_irradiance.w + 1);
    if (previous_irradiance.w < 8) {
        irradiance_history = min(irradiance_history, warmup_history);
        distance_history = min(distance_history, warmup_history);
    }
    // Blend in perceptual space but store linear HDR. This is an explicitly
    // smooth response, not a sample-delta history reset or a tone-map clamp.
    float3 encoded = pow(max(irradiance, 0), .2);
    float3 old_encoded = pow(max(previous_irradiance.rgb, 0), .2);
    if (previous_irradiance.w > 0) {
        // Bound a single random impulse symmetrically. Authoritative response
        // still comes from irradiance_history, including known light edits.
        float3 limit = max(old_encoded, .5) * 2;
        encoded = old_encoded + clamp(encoded - old_encoded, -limit, limit);
    }
    float updates = min(previous_irradiance.w + 1, 32);
    lit_probe_target_output[atlas_texel] = float4(
        ddgi_finite_radiance(pow(max(lerp(encoded, old_encoded, irradiance_history), 0), 5)), updates);
    lit_probe_distance_output[atlas_texel] = lerp(moments, previous_distance, distance_history);
}

[numthreads(8, 8, 1)]
void DDGIResolve(uint3 dispatch_id : SV_DispatchThreadID) {
    uint2 pixel = dispatch_id.xy;
    if (any(pixel >= lit_size_frame.xy)) return;
    float depth = lit_depth.Load(int3(pixel, 0));
    if (!lit_inside(pixel) || lit_is_background_depth(depth)) { lit_indirect[pixel] = 0; return; }
    float2 uv = (float2(pixel) + .5) / lit_size_frame.xy;
    float3 normal = lit_safe_normal(lit_normal_roughness.Load(int3(pixel, 0)).xyz);
    float3 position = lit_world(uv, depth);
    if ((lit_probe_update.w & 2) != 0) {
        float3 grid_position = (position - lit_probe_origin.xyz) / max(lit_probe_spacing.xyz, 1e-3);
        if (any(grid_position < 0) || any(grid_position > float3(lit_probe_grid.xyz - 1))) {
            lit_indirect[pixel] = float4(.03, .05, .2, 1);
            return;
        }
        uint3 coordinates = uint3(clamp(round(grid_position), 0, float3(lit_probe_grid.xyz - 1)));
        uint probe = coordinates.x + lit_probe_grid.x *
            (coordinates.y + lit_probe_grid.y * coordinates.z);
        float4 offset = lit_probe_offsets_previous.Load(int3(ddgi_probe_texel(probe), 0));
        float3 color = offset.w > .5 ? float3(.05, .8, .15) :
            (offset.w < -.5 ? float3(1, .035, .02) : float3(1, .55, .025));
        float proximity = saturate(1 - length(position - ddgi_probe_position(probe, offset.xyz)) /
            length(lit_probe_spacing.xyz));
        // Surface cells expose the finite grid and each nearest probe's
        // classification: green usable, red inside, amber pending relocation.
        lit_indirect[pixel] = float4(color * (.25 + .75 * proximity), 1);
        return;
    }
    lit_indirect[pixel] = float4(ddgi_irradiance(position, normal), 1);
}
