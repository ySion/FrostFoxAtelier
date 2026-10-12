// Shared DDGI coordinates, atlas sampling and receiver gather.
// lit_common.hlsl is prepended by the renderer.
// Each probe owns an 8x8 octahedral tile. RG32F distance moments are normalized
// by maxRayDistance; this is a unit convention, not a reduction to FP16.
// Radiance histories are ping-ponged; probe evaluation age is an in-place counter.
float4 lit_probe_origin; // first grid point xyz, maximum ray distance
float4 lit_probe_spacing; // grid spacing xyz, reserved
float4 lit_probe_history; // sampled irradiance/distance/display old weights xyz; w reserved
uint lit_probe_sequence; // Advances only with this field's updates; resets with its lifetime.
uint4 lit_probe_grid; // XYZ counts, total probe count
uint4 lit_probe_update; // rotating start, bounded count, illumination rays (64/128/256), reset bit

Texture2D<float4> lit_probe_irradiance_previous;
Texture2D<float4> lit_probe_target_previous;
Texture2D<float2> lit_probe_distance_previous;
Texture2D<float4> lit_probe_offsets_previous;
RWTexture2D<float4> lit_probe_irradiance_output;
RWTexture2D<float4> lit_probe_target_output;
RWTexture2D<float2> lit_probe_distance_output;
RWTexture2D<float4> lit_probe_offsets_output;
RWTexture2D<float4> lit_indirect; // irradiance, before surface albedo / pi
RWTexture2D<uint> lit_probe_age_output; // 0 = evaluated this update, ~0 = never evaluated
Texture2D<uint> lit_probe_age;

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

float ddgi_sample_visibility(uint probe, float3 direction, float normalized_distance, float distance_floor) {
    float2 location = ddgi_octahedral_encode(direction) * DDGI_TILE_SIZE - .5;
    int2 lower = int2(floor(location));
    float2 fraction = frac(location);
    int2 tile = int2(ddgi_probe_texel(probe) * DDGI_TILE_SIZE);
    float2 a = lit_probe_distance_previous.Load(int3(tile + ddgi_fold_texel(lower), 0));
    float2 b = lit_probe_distance_previous.Load(int3(tile + ddgi_fold_texel(lower + int2(1, 0)), 0));
    float2 c = lit_probe_distance_previous.Load(int3(tile + ddgi_fold_texel(lower + int2(0, 1)), 0));
    float2 d = lit_probe_distance_previous.Load(int3(tile + ddgi_fold_texel(lower + int2(1, 1)), 0));
    if (!all(isfinite(float4(a, b))) || !all(isfinite(float4(c, d)))) return 0;
    // Evaluate each direction's visibility before spatial filtering. Mixing
    // moments first adds the variation between their means to the variance,
    // which can manufacture visible wedges between otherwise blocked taps.
    float4 means = float4(a.x, b.x, c.x, d.x);
    float4 second_moments = float4(a.y, b.y, c.y, d.y);
    float4 variance = max(second_moments - means * means, distance_floor * distance_floor);
    float4 difference = max(normalized_distance - means, 0);
    float4 visibility = variance / (variance + difference * difference);
    // Cubing each bound retains thin-blocker rejection before interpolation.
    visibility = visibility * visibility * visibility;
    return lerp(lerp(visibility.x, visibility.y, fraction.x),
                lerp(visibility.z, visibility.w, fraction.x), fraction.y);
}

float ddgi_minimum_spacing() {
    return max(1e-3, min(lit_probe_spacing.x, min(lit_probe_spacing.y, lit_probe_spacing.z)));
}

bool ddgi_segment_visible(float3 from, float3 to, bool record_queries) {
    float3 delta = to - from;
    float distance = length(delta);
    if (distance <= 2e-4) return true;
    RayDesc ray;
    ray.Origin = from;
    ray.Direction = delta / distance;
    ray.TMin = 1e-4;
    ray.TMax = distance - 1e-4;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
             RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    if (record_queries) lit_count_rays(lit_visibility_counter);
    query.TraceRayInline(lit_scene, 0, 255, ray);
    while(query.Proceed()) { if(query.CandidateType()==CANDIDATE_NON_OPAQUE_TRIANGLE &&
        lit_accept_candidate(query.CandidateInstanceID(),query.CandidatePrimitiveIndex(),query.CandidateTriangleBarycentrics()))query.CommitNonOpaqueTriangleHit(); }
    return query.CommittedStatus() == COMMITTED_NOTHING;
}

// Status: 0 unavailable, 1 outside, 2 pending, 3 inside geometry,
// 4 no directional/visible support, 5 valid zero irradiance, 6 valid light.
// Diagnostics use this exact gather; unused status arithmetic is stripped from lighting kernels.
float3 ddgi_gather(float3 position, float3 normal, float3 geometric_normal,
                  bool trace_support, bool record_queries, out uint status, out uint contributors) {
    status = 0; contributors = 0;
    if (any(lit_probe_grid.xyz < 2) || lit_probe_grid.w == 0 ||
        !all(isfinite(position)) || !all(isfinite(normal))) return 0;

    float3 spacing = max(lit_probe_spacing.xyz, 1e-3);
    // Cached angular support needs its established filter bias. Actual
    // surface visibility uses only numerical clearance, so this larger bias
    // cannot move the receiver through a thin wall during the ray query.
    float3 support_position = position + geometric_normal * (ddgi_minimum_spacing() * .12);
    float3 biased_position = trace_support ?
        position + geometric_normal * max(1e-4, ddgi_minimum_spacing() * .0001) : support_position;
    float3 grid_position = (biased_position - lit_probe_origin.xyz) / spacing;
    if (any(grid_position < 0) || any(grid_position > float3(lit_probe_grid.xyz - 1))) { status = 1; return 0; }
    uint3 lower = uint3(clamp(floor(grid_position), 0, float3(lit_probe_grid.xyz - 2)));
    float3 fraction = saturate(grid_position - float3(lower));
    float3 accumulated = 0;
    float accumulated_weight = 0;
    bool pending = false, inside = false, usable = false;

    [unroll] for (uint corner = 0; corner < 8; ++corner) {
        uint3 step = uint3(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1);
        uint3 coordinates = lower + step;
        uint probe = coordinates.x + lit_probe_grid.x *
            (coordinates.y + lit_probe_grid.y * coordinates.z);
        float4 offset = lit_probe_offsets_previous.Load(int3(ddgi_probe_texel(probe), 0));
        float3 trilinear = lerp(1 - fraction, fraction, float3(step));
        float spatial_weight = trilinear.x * trilinear.y * trilinear.z;
        if (spatial_weight <= 0) continue;
        // w = 0: initializing/relocated, -1: inside geometry, +1: usable.
        if (!all(isfinite(offset)) || offset.w == 0) { pending = true; continue; }
        if (offset.w < .5) { inside = true; continue; }
        usable = true;
        float4 irradiance = ddgi_sample_tile(lit_probe_irradiance_previous, probe, normal);
        if (irradiance.w <= 0 || !all(isfinite(irradiance))) continue;

        float3 probe_position = ddgi_probe_position(probe, offset.xyz);
        float3 delta = biased_position - probe_position;
        float distance = length(delta);
        float3 direction = delta / max(distance, 1e-5);
        if (trace_support) {
            if (!ddgi_segment_visible(probe_position, biased_position, record_queries)) continue;
        }
        // Keep the continuous directional support: binary visibility alone
        // projects hard probe-membership contours onto the receiver. The ray
        // above rejects blocked contributions that cached moments can admit.
        float3 support_delta = support_position - probe_position;
        float support_distance = length(support_delta);
        float distance_scale = max(lit_probe_origin.w, 1e-3);
        float visibility = ddgi_sample_visibility(probe, support_delta / max(support_distance, 1e-5),
            support_distance / distance_scale, ddgi_minimum_spacing() * .002 / distance_scale);
        float facing = max(.02, (dot(normal, -direction) + 1) * .5);
        float weight = facing * facing * visibility;
        if (weight < .2) weight *= weight * weight / .04;
        // Confidence shaping must not sharpen the spatial interpolation into
        // isolated probe cells. Keep the partition of unity linear.
        weight *= spatial_weight;
        accumulated += max(irradiance.rgb, 0) * weight;
        accumulated_weight += weight;
        contributors += weight > 0 ? 1 : 0;
    }
    // This is a normalized interpolation, not ambient occlusion. A small
    // common confidence scale must not extinguish an otherwise valid field.
    float3 result = accumulated_weight > 0 ? ddgi_finite_radiance(accumulated / accumulated_weight) : 0;
    status = accumulated_weight > 0 ? (any(result > 0) ? 6 : 5) : (usable ? 4 : (pending ? 2 : (inside ? 3 : 4)));
    return result;
}

float3 ddgi_irradiance(float3 position, float3 normal, float3 geometric_normal, bool trace_support) {
    uint status, contributors;
    return ddgi_gather(position, normal, geometric_normal, trace_support, true, status, contributors);
}
