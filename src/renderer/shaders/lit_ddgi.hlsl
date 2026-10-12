// Probe updates and receiver irradiance. Shared DDGI source is prepended.
[numthreads(64, 1, 1)]
void DDGICopy(uint3 dispatch_id : SV_DispatchThreadID) {
    uint index = dispatch_id.x;
    if (index < lit_probe_pool.y * 64) {
        uint probe=index/64;
        DDGIVolume volume=lit_probe_current_volumes[ddgi_volume_for_probe(probe)];
        uint2 texel=ddgi_probe_texel(probe)*8+uint2(index%8,index%64/8);
        if (ddgi_probe_fresh(probe,volume)) {
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
    if (index < lit_probe_pool.y) {
        uint2 texel = ddgi_probe_texel(index);
        bool fresh=ddgi_probe_fresh(index,lit_probe_current_volumes[ddgi_volume_for_probe(index)]);
        if (fresh) lit_probe_age_output[texel] = 0xffffffff;
        else {
            uint age = lit_probe_age_output[texel];
            if (age != 0xffffffff) lit_probe_age_output[texel] = min(age, 0xfffffffd) + 1;
        }
        lit_probe_offsets_output[texel] = fresh ? 0 :
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
    // Camera-surface NEE owns direct mesh emission. This field stores the
    // environment and light that has scattered at least once before the probe.
    float3 radiance = 0;
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
        if (dot(surface.normal, direction) > 0 && dot(surface.geometric_normal,direction)>0 && any(incoming > 0)) {
            bool visible = true;
            if ((light.meta.z & 2u) != 0)
                visible = lit_visible(surface.position, surface.geometric_normal, direction, distance);
            if (visible) radiance += incoming * lit_brdf(surface.base, surface.roughness, surface.metallic,
                surface.normal, toward_probe, direction) * (float(lit_counts.x) / samples);
        }
    }
    float3 emitter_diffuse, emitter_specular;
    lit_emitter_lighting(surface.position, surface.normal, surface.geometric_normal, toward_probe,
        surface.base, surface.roughness, surface.metallic, true, true, seed,
        emitter_diffuse, emitter_specular);
    radiance += emitter_diffuse + emitter_specular;
    if (lit_probe_pool.w != 0 && lit_probe_controls.x > 0) {
        // Only diffuse history feeds back. Bounded reflectance and the 0.85
        // transport factor make this iteration a contraction even in a closed
        // white room. Analytic and mesh NEE above inject reflected direct
        // light once at the actual hit, without the feedback attenuation.
        float3 reflectance = saturate(surface.base) * (1 - saturate(surface.metallic));
        if(any(reflectance>0))radiance += ddgi_irradiance(surface.position, surface.normal,
            surface.geometric_normal, lit_probe_controls.y!=0) * reflectance * (lit_probe_controls.x / LIT_PI);
    }
    return ddgi_finite_radiance(radiance);
}

float ddgi_trace_ray(float3 position, float3 direction, DDGIVolume volume, bool shade, inout uint seed, out float3 radiance) {
    RayDesc ray;
    ray.Origin = position;
    ray.Direction = direction;
    ray.TMin = max(1e-4, ddgi_minimum_spacing(volume) * .001);
    // Cache coverage is not a transport horizon. A small following volume
    // must still see the distant walls of the same scene before reporting sky.
    float3 far_corner=max(abs(position-lit_probe_scene_min.xyz),abs(position-lit_probe_scene_max.xyz));
    float far_scale=max(1e-3,max(far_corner.x,max(far_corner.y,far_corner.z)));
    float scene_distance=far_scale*length(far_corner/far_scale);
    ray.TMax = max(scene_distance+max(1e-3,scene_distance*1e-5),ray.TMin*2);
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    // Both fixed geometry and random illumination rays count as probe rays.
    // Secondary shadow rays are counted separately by lit_visible().
    lit_count_rays(1);
    query.TraceRayInline(lit_scene, 0, 255, ray);
    while(query.Proceed()) { if(query.CandidateType()==CANDIDATE_NON_OPAQUE_TRIANGLE &&
        lit_accept_candidate(query.CandidateInstanceID(),query.CandidatePrimitiveIndex(),query.CandidateTriangleBarycentrics()))query.CommitNonOpaqueTriangleHit(); }
    radiance = shade ? ddgi_finite_radiance(lit_environment.rgb) : 0;
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return ray.TMax;
    float distance = query.CommittedRayT();
    LitSurface surface = lit_hit_surface(query.CommittedInstanceID(),
        query.CommittedPrimitiveIndex(), query.CommittedTriangleBarycentrics(),
        position + direction * distance, distance*.01);
    // Only a closed component's geometric backface is evidence of an interior.
    // The local authored orientation uses inverse-transpose, including mirrors.
    if (surface.closed && dot(surface.geometric_normal, direction) > 0) {
        radiance = 0;
        return -max(distance, 1e-5);
    }
    // An open sheet still blocks this ray. Preserve its existing shading side
    // without treating the entire half-space behind it as a solid volume.
    if (dot(surface.geometric_normal, direction) > 0) {
        if(surface.double_sided) {surface.normal=-surface.normal;surface.geometric_normal=-surface.geometric_normal;}
        else {radiance=0;return distance;}
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
    if (group_id.x >= lit_probe_update.y || lit_probe_pool.y == 0) return;
    if (lane == 0) ddgi_current_probe = lit_probe_work[group_id.x/4][group_id.x%4];
    GroupMemoryBarrierWithGroupSync();
    uint probe = ddgi_current_probe;
    DDGIVolume volume=lit_probe_current_volumes[ddgi_volume_for_probe(probe)];
    bool fresh=ddgi_probe_fresh(probe,volume);
    uint2 probe_texel = ddgi_probe_texel(probe);
    float4 old_offset = fresh ? 0 :
        lit_probe_offsets_previous.Load(int3(probe_texel, 0));
    if (!all(isfinite(old_offset))) old_offset = 0;
    float3 position = ddgi_probe_position(probe, old_offset.xyz,volume);
    uint ray_count = clamp(lit_probe_update.z, 1u, DDGI_MAX_RAYS);
    float maximum_distance = max(volume.origin.w, 1e-3);

    // Geometry decisions must not follow the random illumination rotation.
    // Inactive probes retain these rays so moved geometry can reactivate them.
    if (lane < DDGI_FIXED_RAYS) {
        uint seed = 0;
        float3 unused_radiance;
        ddgi_fixed_distances[lane] = ddgi_trace_ray(position,
            ddgi_spherical_direction(lane, DDGI_FIXED_RAYS), volume, false, seed, unused_radiance);
    }
    GroupMemoryBarrierWithGroupSync();

    if (lane == 0) {
        uint backfaces = 0, nearest_backface = 0, nearest_frontface = 0;
        // Fixed rays now cover the shared scene, including hits beyond this
        // volume's moment range. Search over their actual distances.
        float backface_distance = asfloat(0x7f7fffff), frontface_distance = asfloat(0x7f7fffff);
        [loop] for (uint i = 0; i < DDGI_FIXED_RAYS; ++i) {
            float d = ddgi_fixed_distances[i];
            if (d < 0) {
                ++backfaces;
                if (-d < backface_distance) { backface_distance = -d; nearest_backface = i; }
            } else if (d < frontface_distance) { frontface_distance = d; nearest_frontface = i; }
        }
        float clearance = ddgi_minimum_spacing(volume) * .12;
        float3 target_offset = old_offset.xyz;
        ddgi_probe_state = 1;
        bool inside = backfaces > DDGI_FIXED_RAYS / 4;
        if (inside) {
            target_offset += ddgi_spherical_direction(nearest_backface, DDGI_FIXED_RAYS) *
                (backface_distance + clearance);
            ddgi_probe_state = -1;
        } else if (frontface_distance < clearance - ddgi_minimum_spacing(volume) * .01) {
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
            target_offset = old_offset.xyz + move * min(1, ddgi_minimum_spacing(volume) * .04 / max(move_length, 1e-6));
        float3 normalized_offset = target_offset / max(volume.spacing.xyz, 1e-3);
        target_offset *= min(1, .45 / max(length(normalized_offset), 1e-6));
        bool relocated = length(target_offset - old_offset.xyz) > ddgi_minimum_spacing(volume) * .002;
        if (!relocated) target_offset = old_offset.xyz;
        if (relocated) ddgi_probe_state = 0;
        ddgi_discard_history = (fresh || old_offset.w < .5 || relocated || inside) ? 1 : 0;
        lit_probe_offsets_output[probe_texel] = float4(target_offset, ddgi_probe_state);
        lit_probe_age_output[probe_texel] = 0;
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
        ddgi_ray_distances[i] = ddgi_trace_ray(position, direction, volume, true, seed, radiance);
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
        // Transport can reach beyond this cache. Clamp each distance before
        // forming moments; clamping the averaged moments would erase variance.
        float normalized_distance = saturate(max(ddgi_ray_distances[i], 0) / maximum_distance);
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
    // With fewer rays than directional texels, a hemisphere may have no
    // observation this visit. That is missing support, not measured darkness.
    // Preserve both the target and its observation count (or invalid initial
    // support), so a constant sky cannot be driven toward black by empty visits.
    if (irradiance_weight <= 1e-5) {
        lit_probe_target_output[atlas_texel] = previous_irradiance;
        lit_probe_distance_output[atlas_texel] = previous_distance;
        return;
    }
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
    lit_probe_distance_output[atlas_texel] = distance_weight > 1e-8 ?
        lerp(moments, previous_distance, distance_history) : previous_distance;
}

[numthreads(8, 8, 1)]
void DDGIResolve(uint3 dispatch_id : SV_DispatchThreadID) {
    uint2 pixel = dispatch_id.xy;
    if (any(pixel >= lit_size_frame.xy)) return;
    float depth = lit_depth.Load(int3(pixel, 0));
    if (!lit_inside(pixel) || lit_is_background_depth(depth)) { lit_indirect[pixel] = 0; return; }
    if ((lit_counts.w & 64) == 0) {
        float4 material = lit_base_metallic.Load(int3(pixel, 0));
        // Compose cannot consume diffuse irradiance at exactly zero material
        // response. Local GI retains its full input; debug gathers separately.
        if (all(material.rgb * (1 - material.w) == 0)) {
            lit_indirect[pixel] = float4(0, 0, 0, 1);
            return;
        }
    }
    float2 uv = (float2(pixel) + .5) / lit_size_frame.xy;
    float3 normal = lit_safe_normal(lit_normal_roughness.Load(int3(pixel, 0)).xyz);
    float3 position = lit_world(uv, depth);
    lit_indirect[pixel] = float4(ddgi_irradiance(position, normal, lit_geometry_normal_at(pixel), true), 1);
}
