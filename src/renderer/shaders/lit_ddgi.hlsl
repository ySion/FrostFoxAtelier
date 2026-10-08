// Finite world-space DDGI. lit_common.hlsl is prepended by the renderer.
// Each probe owns an 8x8 octahedral tile. Distance moments are normalized by
// maxRayDistance, keeping their second moment representable in FP16.
// All three histories are ping-ponged; no resource is read while it is a UAV.
float4 lit_probe_origin; // first grid point xyz, maximum ray distance
float4 lit_probe_spacing; // grid spacing xyz, previous-data hysteresis
uint4 lit_probe_grid; // XYZ counts, total probe count
uint4 lit_probe_update; // rotating start, bounded count, rays (1..64), reset/debug bits

Texture2D<float4> lit_probe_irradiance_previous;
Texture2D<float4> lit_probe_distance_previous;
Texture2D<float4> lit_probe_offsets_previous;
RWTexture2D<float4> lit_probe_irradiance_output;
RWTexture2D<float4> lit_probe_distance_output;
RWTexture2D<float4> lit_probe_offsets_output;
RWTexture2D<float4> lit_indirect; // irradiance, before surface albedo / pi

static const uint DDGI_TILE_SIZE = 8;
static const uint DDGI_RAY_COUNT = 64;

float3 ddgi_finite_radiance(float3 radiance) {
    return all(isfinite(radiance)) ? clamp(radiance, 0, 32000) : 0;
}

uint3 ddgi_probe_coordinates(uint probe) {
    uint x = probe % lit_probe_grid.x;
    uint yz = probe / lit_probe_grid.x;
    return uint3(x, yz % lit_probe_grid.y, yz / lit_probe_grid.y);
}

uint2 ddgi_probe_texel(uint probe) {
    return uint2(probe % lit_probe_grid.x, probe / lit_probe_grid.x);
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

float ddgi_minimum_spacing() {
    return max(1e-3, min(lit_probe_spacing.x, min(lit_probe_spacing.y, lit_probe_spacing.z)));
}

float3 ddgi_irradiance(float3 position, float3 normal) {
    if (any(lit_probe_grid.xyz < 2) || lit_probe_grid.w == 0 ||
        !all(isfinite(position)) || !all(isfinite(normal))) return 0;

    float3 spacing = max(lit_probe_spacing.xyz, 1e-3);
    // Geometric-normal bias is independent of the camera, including an ortho
    // camera. It offsets moment queries away from the receiving surface.
    float3 biased_position = position + normal * ddgi_minimum_spacing() * .04;
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
        float2 moments = ddgi_sample_tile(lit_probe_distance_previous, probe, direction).xy;
        if (!all(isfinite(moments))) continue;
        float normalized_distance = distance / max(lit_probe_origin.w, 1e-3);
        float variance = max(moments.y - moments.x * moments.x, 1e-5);
        float difference = max(normalized_distance - moments.x, 0);
        float visibility = difference == 0 ? 1 : variance / (variance + difference * difference);
        // Cubed Chebyshev visibility suppresses light bleeding across thin
        // blockers while retaining the distance field's angular information.
        visibility = visibility * visibility * visibility;
        float3 trilinear = lerp(1 - fraction, fraction, float3(step));
        float facing = max(.02, (dot(normal, -direction) + 1) * .5);
        float weight = trilinear.x * trilinear.y * trilinear.z * facing * facing * visibility;
        if (weight < .2) weight *= weight * weight / .04;
        float4 irradiance = ddgi_sample_tile(lit_probe_irradiance_previous, probe, normal);
        if (irradiance.w <= 0 || !all(isfinite(irradiance))) continue;
        accumulated += max(irradiance.rgb, 0) * weight;
        accumulated_weight += weight;
    }
    return ddgi_finite_radiance(accumulated / max(accumulated_weight, 1e-8));
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
            lit_probe_distance_output[texel] = float4(1, 1, 0, 0);
        } else {
            lit_probe_irradiance_output[texel] = lit_probe_irradiance_previous.Load(int3(texel, 0));
            lit_probe_distance_output[texel] = lit_probe_distance_previous.Load(int3(texel, 0));
        }
    }
    if (index < lit_probe_grid.w) {
        uint2 texel = ddgi_probe_texel(index);
        lit_probe_offsets_output[texel] = (lit_probe_update.w & 1) != 0 ? 0 :
            lit_probe_offsets_previous.Load(int3(texel, 0));
    }
}

float3 ddgi_ray_direction(uint ray_index, uint probe, uint ray_count) {
    float z = 1 - 2 * (float(ray_index) + .5) / ray_count;
    float phi = 2 * LIT_PI * frac(float(ray_index) * .61803398875);
    float radius = sqrt(max(0, 1 - z * z));
    float3 direction = float3(radius * cos(phi), radius * sin(phi), z);
    // A deterministic per-frame/per-probe uniform rotation changes the ray
    // coverage without making the field depend on view visibility.
    uint seed = lit_hash(probe ^ lit_hash(lit_size_frame.z + 0x9e3779b9));
    float u = lit_random(seed), a = 2 * LIT_PI * lit_random(seed), b = 2 * LIT_PI * lit_random(seed);
    float4 rotation = float4(sqrt(1 - u) * sin(a), sqrt(1 - u) * cos(a),
                            sqrt(u) * sin(b), sqrt(u) * cos(b));
    return direction + 2 * cross(rotation.xyz, cross(rotation.xyz, direction) + rotation.w * direction);
}

float3 ddgi_hit_radiance(LitSurface surface, float3 toward_probe, inout uint seed) {
    float3 radiance = max(surface.emissive, 0);
    // Uniform selection with replacement has a known 1/N probability. Keep
    // hit shading bounded: at most two additional RT shadow queries per ray,
    // including for offscreen geometry. Main-view shadow masks are not used.
    uint samples = min(2u, lit_counts.x);
    [loop] for (uint sample_index = 0; sample_index < samples; ++sample_index) {
        uint light_index = min(uint(lit_random(seed) * lit_counts.x), lit_counts.x - 1);
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

groupshared float3 ddgi_ray_directions[64];
groupshared float3 ddgi_ray_radiance[64];
groupshared float ddgi_ray_distances[64]; // negative means back-facing hit
groupshared float3 ddgi_new_offset;
groupshared float ddgi_probe_state;
groupshared uint ddgi_discard_history;

[numthreads(64, 1, 1)]
void DDGIUpdate(uint3 group_id : SV_GroupID, uint lane : SV_GroupIndex) {
    if (group_id.x >= min(lit_probe_update.y, lit_probe_grid.w) || lit_probe_grid.w == 0) return;
    uint probe = (lit_probe_update.x + group_id.x) % lit_probe_grid.w;
    uint2 probe_texel = ddgi_probe_texel(probe);
    float4 old_offset = (lit_probe_update.w & 1) != 0 ? 0 :
        lit_probe_offsets_previous.Load(int3(probe_texel, 0));
    if (!all(isfinite(old_offset))) old_offset = 0;
    float3 position = ddgi_probe_position(probe, old_offset.xyz);
    uint ray_count = clamp(lit_probe_update.z, 1u, DDGI_RAY_COUNT);
    float3 direction = lane < ray_count ? ddgi_ray_direction(lane, probe, ray_count) : 0;
    float maximum_distance = max(lit_probe_origin.w, 1e-3);
    float distance = maximum_distance;
    float3 radiance = ddgi_finite_radiance(lit_environment.rgb);

    if (lane < ray_count) {
        RayDesc ray;
        ray.Origin = position;
        ray.Direction = direction;
        ray.TMin = max(1e-4, ddgi_minimum_spacing() * .001);
        ray.TMax = maximum_distance;
        // Closest-hit distance and surface orientation are required for both
        // directional moments and relocation: first-hit shadow flags do not fit.
        RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
        lit_count_rays(1);
        query.TraceRayInline(lit_scene, 0, 255, ray);
        while (query.Proceed()) {}
        if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
            distance = query.CommittedRayT();
            LitSurface surface = lit_hit_surface(query.CommittedInstanceID(),
                query.CommittedPrimitiveIndex(), query.CommittedTriangleBarycentrics(),
                position + direction * distance);
            // Mesh normals define outward orientation, including reflected
            // instances; triangle winding in the AS is not that contract.
            if (dot(surface.normal, direction) > 0) {
                distance = -max(distance, 1e-5);
                radiance = 0;
            } else {
                uint seed = lit_hash(probe ^ lit_hash(lane + 64 * lit_size_frame.z));
                radiance = ddgi_hit_radiance(surface, -direction, seed);
            }
        }
    } else {
        radiance = 0;
    }
    ddgi_ray_directions[lane] = direction;
    ddgi_ray_radiance[lane] = radiance;
    ddgi_ray_distances[lane] = distance;
    GroupMemoryBarrierWithGroupSync();

    if (lane == 0) {
        uint backfaces = 0, nearest_backface = 0, nearest_frontface = 0;
        float backface_distance = maximum_distance, frontface_distance = maximum_distance;
        [loop] for (uint i = 0; i < ray_count; ++i) {
            float d = ddgi_ray_distances[i];
            if (d < 0) {
                ++backfaces;
                if (-d < backface_distance) { backface_distance = -d; nearest_backface = i; }
            } else if (d < frontface_distance) { frontface_distance = d; nearest_frontface = i; }
        }
        float clearance = ddgi_minimum_spacing() * .12;
        float3 target_offset = old_offset.xyz;
        ddgi_probe_state = 1;
        bool inside = backfaces > ray_count / 4;
        if (inside) {
            target_offset += ddgi_ray_directions[nearest_backface] * (backface_distance + clearance);
            ddgi_probe_state = -1;
        } else if (frontface_distance < clearance) {
            target_offset -= ddgi_ray_directions[nearest_frontface] * (clearance - frontface_distance);
        }
        // Relocation remains within this probe's cell; its grid identity and
        // all interpolation addresses remain stable without voxel resources.
        target_offset = clamp(target_offset, -.45 * lit_probe_spacing.xyz, .45 * lit_probe_spacing.xyz);
        bool relocated = length(target_offset - old_offset.xyz) > ddgi_minimum_spacing() * .002;
        ddgi_new_offset = target_offset;
        if (relocated) ddgi_probe_state = 0;
        ddgi_discard_history = ((lit_probe_update.w & 1) != 0 || old_offset.w < .5 || relocated || inside) ? 1 : 0;
        lit_probe_offsets_output[probe_texel] = float4(ddgi_new_offset, ddgi_probe_state);
    }
    GroupMemoryBarrierWithGroupSync();

    uint2 local_texel = uint2(lane % DDGI_TILE_SIZE, lane / DDGI_TILE_SIZE);
    uint2 atlas_texel = probe_texel * DDGI_TILE_SIZE + local_texel;
    if (ddgi_probe_state < .5) {
        // Rays were cast at the old position; never associate their radiance
        // or moments with a relocated origin. The next bounded visit retraces.
        lit_probe_irradiance_output[atlas_texel] = 0;
        lit_probe_distance_output[atlas_texel] = float4(1, 1, 0, 0);
        return;
    }
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
    float4 previous_distance = float4(1, 1, 0, 0);
    if (ddgi_discard_history == 0) {
        previous_irradiance = lit_probe_irradiance_previous.Load(int3(atlas_texel, 0));
        previous_distance = lit_probe_distance_previous.Load(int3(atlas_texel, 0));
    }
    if (!all(isfinite(previous_irradiance))) previous_irradiance = 0;
    if (!all(isfinite(previous_distance))) previous_distance = float4(1, 1, 0, 0);
    float history_weight = previous_irradiance.w > 0 ? clamp(lit_probe_spacing.w, 0, .98) : 0;
    if (previous_irradiance.w < 4) history_weight = min(history_weight, .5);
    float3 change = abs(irradiance - previous_irradiance.rgb) /
        max(max(irradiance, previous_irradiance.rgb), .05);
    float relative_change = max(change.x, max(change.y, change.z));
    if (relative_change > .25) history_weight = min(history_weight, .35);
    if (relative_change > .75) history_weight = min(history_weight, .1);
    history_weight *= 1 - .85 * saturate(lit_temporal.z);
    float distance_history_weight = history_weight;
    if (abs(moments.x - previous_distance.x) > .025) distance_history_weight = min(distance_history_weight, .2);
    float updates = min(previous_irradiance.w + 1, 32);
    lit_probe_irradiance_output[atlas_texel] = float4(
        ddgi_finite_radiance(lerp(irradiance, previous_irradiance.rgb, history_weight)), updates);
    lit_probe_distance_output[atlas_texel] = float4(
        lerp(moments, previous_distance.xy, distance_history_weight), 0, 1);
}

[numthreads(8, 8, 1)]
void DDGIResolve(uint3 dispatch_id : SV_DispatchThreadID) {
    uint2 pixel = dispatch_id.xy;
    if (any(pixel >= lit_size_frame.xy)) return;
    float depth = lit_depth.Load(int3(pixel, 0));
    if (!lit_inside(pixel) || depth >= 1) { lit_indirect[pixel] = 0; return; }
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
