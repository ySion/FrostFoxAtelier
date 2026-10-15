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
            lit_probe_presentation[texel] = 0;
            lit_probe_distance_output[texel] = float2(1, 1);
            lit_probe_response[texel]=uint2(lit_probe_response_epoch,0);
        } else {
            // Unscheduled probes retain the authoritative transport estimate.
            // All scheduled feedback reads this immutable previous snapshot.
            lit_probe_irradiance_output[texel] = lit_probe_irradiance_previous.Load(int3(texel, 0));
            lit_probe_distance_output[texel] = lit_probe_distance_previous.Load(int3(texel, 0));
        }
    }
    if (index < lit_probe_pool.y) {
        uint2 texel = ddgi_probe_texel(index);
        bool fresh=ddgi_probe_fresh(index,lit_probe_current_volumes[ddgi_volume_for_probe(index)]);
        if (fresh) {lit_probe_age_output[texel] = 0xffffffff;lit_probe_sample_cursor[texel]=0;}
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

float4 ddgi_illumination_rotation(uint2 visit,uint probe) {
    // A per-probe additive R3 sequence replaces independent whole-set rotations.
    // Integer Weyl steps retain phase precision during long-running sessions.
    uint seed=lit_hash(probe^lit_hash(visit.y));
    uint3 shift=uint3(lit_hash(seed^0x68bc21ebu),lit_hash(seed^0x02e5be93u),lit_hash(seed^0x967a889bu));
    float3 u=float3((visit.x*uint3(0xd1b54a33u,0xabc98389u,0x8cb92ba7u)+shift)>>8)*(1.0/16777216.0);
    precise float a=2*LIT_PI*u.y,b=2*LIT_PI*u.z;
    precise float4 rotation=float4(sqrt(1-u.x)*sin(a),sqrt(1-u.x)*cos(a),sqrt(u.x)*sin(b),sqrt(u.x)*cos(b));
    return rotation;
}
float3 ddgi_illumination_direction(uint ray_index,uint ray_count,float4 rotation) {
    precise float3 direction=ddgi_spherical_direction(ray_index,ray_count);
    direction+=2*cross(rotation.xyz,cross(rotation.xyz,direction)+rotation.w*direction);
    return direction;
}

float3 ddgi_hit_radiance(LitSurface surface,float3 toward_probe,inout uint seed,bool inspect,uint ray_index,float hit_distance) {
    // Camera-surface NEE owns direct mesh emission. This field stores the
    // environment and light that has scattered at least once before the probe.
    LitLightingSurface receiver;
    receiver.position=surface.position;receiver.normal=surface.normal;receiver.geometric_normal=surface.geometric_normal;
    receiver.view=toward_probe;receiver.base=surface.base;receiver.roughness=surface.roughness;receiver.metallic=surface.metallic;
    // The same unit-square light domain and PDFs serve secondary hits, but a
    // probe owns fresh RIS samples, never screen-space temporal reservoirs.
    // Explicit directionals cannot disappear in a large local-light lottery.
    float3 diffuse,specular,sun_diffuse,sun_specular;
    uint sun_seed=lit_hash(seed^0xb5297a4du);
    float proposal_probability;
    LitReservoir local=lit_local_initial(receiver,true,lit_di_settings.x,true,seed,proposal_probability);
    lit_reservoir_lighting(receiver,local,true,true,diffuse,specular);
    // Independent random domains keep sun samples unchanged when the local
    // candidate count or proposal changes.
    lit_directional_lighting(receiver,1,true,sun_seed,sun_diffuse,sun_specular);
    float3 radiance=diffuse+specular+sun_diffuse+sun_specular;
    float3 feedback=0;
    if (lit_probe_pool.w != 0 && lit_probe_controls.x > 0) {
        // Only diffuse history feeds back. Bounded reflectance and the authored
        // transport factor make this iteration a contraction even in a closed
        // white room. Analytic and mesh NEE above inject reflected direct
        // light once at the actual hit, without the feedback attenuation.
        float3 reflectance = saturate(surface.base) * (1 - saturate(surface.metallic));
        if(any(reflectance>0))feedback = ddgi_irradiance(surface.position, surface.normal,
            surface.geometric_normal, lit_probe_controls.y!=0) * reflectance * (lit_probe_controls.x / LIT_PI);
    }
    if(inspect) {
        const float3 y=float3(.2126,.7152,.0722);
        DDGIInspectedRay record=(DDGIInspectedRay)0;
        bool selected=local.source<lit_source_counts.x;
        record.identity=uint4(local.source,selected?lit_sources[local.source].meta.x:~0u,ray_index,1);
        record.direct=float4(dot(diffuse,y),dot(specular,y),dot(sun_diffuse,y),dot(sun_specular,y));
        record.surface=float4(surface.position,surface.roughness);
        record.sampling=float4(local.weight,proposal_probability,dot(feedback,y),hit_distance);
        record.endpoint_distance.w=-1;
        if(selected) {
            // Inspection alone reconstructs the selected sample; no new random
            // draw or visibility query may change the production estimator.
            LitLightSource source=lit_sources[local.source];
            LitLightSample prepared=lit_prepare_light_sample(source,local.uv);
            if(prepared.kind!=~0u) {
                record.endpoint_distance=float4(prepared.position,length(prepared.position-surface.position));
                record.source_parameters=float4(source.direction_radius.w,source.origin_range.w,prepared.area,
                    prepared.kind==3?0:prepared.minimum_distance_squared);
            }
        }
        lit_probe_inspected_rays[ray_index]=record;
    }
    return ddgi_finite_radiance(radiance+feedback);
}

DDGIRayHit ddgi_trace_ray(float3 position, float3 direction, DDGIVolume volume, bool record_surface) {
    DDGIRayHit hit = (DDGIRayHit)0;
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
    hit.radiance_distance = float4(record_surface ? ddgi_finite_radiance(lit_environment.rgb) : 0, ray.TMax);
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return hit;
    float distance = query.CommittedRayT();
    hit.radiance_distance = float4(0, 0, 0, distance);
    LitSurface surface = lit_hit_surface(query.CommittedInstanceID(),
        query.CommittedPrimitiveIndex(), query.CommittedTriangleBarycentrics(),
        position + direction * distance, distance*.01);
    // Only a closed component's geometric backface is evidence of an interior.
    // The local authored orientation uses inverse-transpose, including mirrors.
    if (surface.closed && dot(surface.geometric_normal, direction) > 0) {
        hit.radiance_distance.w = -max(distance, 1e-5);
        return hit;
    }
    // An open sheet still blocks this ray. Preserve its existing shading side
    // without treating the entire half-space behind it as a solid volume.
    if (dot(surface.geometric_normal, direction) > 0) {
        if(surface.double_sided) {surface.normal=-surface.normal;surface.geometric_normal=-surface.geometric_normal;}
        else return hit;
    }
    if (record_surface) {
        hit.base_roughness = float4(surface.base, surface.roughness);
        hit.normal_metallic = float4(surface.normal, surface.metallic);
        hit.geometric_normal = surface.geometric_normal;
        hit.flags = DDGI_RAY_SHADEABLE;
    }
    return hit;
}

groupshared float ddgi_fixed_distances[32];
groupshared float3 ddgi_ray_directions[256];
groupshared float3 ddgi_ray_radiance[256];
groupshared float ddgi_ray_distances[256]; // negative means back-facing hit
groupshared uint2 ddgi_sample_start;
groupshared float4 ddgi_ray_rotation;
groupshared uint ddgi_current_probe;
groupshared float ddgi_probe_state;
groupshared uint ddgi_discard_history;

[numthreads(64, 1, 1)]
void DDGITrace(uint3 group_id : SV_GroupID, uint lane : SV_GroupIndex) {
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

    // Geometry decisions must not follow the random illumination rotation.
    // Inactive probes retain these rays so moved geometry can reactivate them.
    if (lane < DDGI_FIXED_RAYS) {
        DDGIRayHit hit = ddgi_trace_ray(position,
            ddgi_spherical_direction(lane, DDGI_FIXED_RAYS), volume, false);
        ddgi_fixed_distances[lane] = hit.radiance_distance.w;
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
        ddgi_sample_start=lit_probe_sample_cursor[probe_texel];
        ddgi_ray_rotation=ddgi_illumination_rotation(ddgi_sample_start,probe);
    }
    GroupMemoryBarrierWithGroupSync();

    // Relocated and inactive probes have no records to consume this visit.
    // DDGIUpdate clears their atlas before attempting any scratch reads.
    if (ddgi_probe_state < .5) return;
    [loop] for (uint i = lane; i < ray_count; i += 64) {
        float3 direction = ddgi_illumination_direction(i,ray_count,ddgi_ray_rotation);
        DDGIRayHit hit = ddgi_trace_ray(position, direction, volume, true);
        if (ddgi_discard_history != 0) hit.flags |= DDGI_RAY_DISCARD_HISTORY;
        lit_probe_ray_hits_output[group_id.x * ray_count + i] = hit;
    }
}

void ddgi_update(uint3 group_id,uint lane,bool inspect) {
    if (group_id.x >= lit_probe_update.y || lit_probe_pool.y == 0) return;
    if (lane == 0) {
        ddgi_current_probe = lit_probe_work[group_id.x/4][group_id.x%4];
        ddgi_sample_start=lit_probe_sample_cursor[ddgi_probe_texel(ddgi_current_probe)];
        ddgi_ray_rotation=ddgi_illumination_rotation(ddgi_sample_start,ddgi_current_probe);
    }
    GroupMemoryBarrierWithGroupSync();
    uint probe = ddgi_current_probe;
    DDGIVolume volume = lit_probe_current_volumes[ddgi_volume_for_probe(probe)];
    uint2 probe_texel = ddgi_probe_texel(probe);
    float4 offset = lit_probe_offsets_current.Load(int3(probe_texel, 0));
    uint2 local_texel = uint2(lane % DDGI_TILE_SIZE, lane / DDGI_TILE_SIZE);
    uint2 atlas_texel = probe_texel * DDGI_TILE_SIZE + local_texel;
    if (offset.w < .5) {
        // Rays were cast at the old position; never associate their radiance
        // or moments with a relocated origin. The next bounded visit retraces.
        lit_probe_irradiance_output[atlas_texel] = 0;
        lit_probe_presentation[atlas_texel] = 0;
        lit_probe_distance_output[atlas_texel] = float2(1, 1);
        lit_probe_response[atlas_texel]=uint2(lit_probe_response_epoch,0);
        if(lane==0)lit_probe_sample_cursor[probe_texel]=0;
        return;
    }
    // Active origins are unchanged by the trace pass. Scratch only holds this
    // update's work; feedback continues to sample the previous field snapshot.
    float3 position = ddgi_probe_position(probe, offset.xyz, volume);
    uint ray_count = clamp(lit_probe_update.z, 1u, DDGI_MAX_RAYS);
    float maximum_distance = max(volume.origin.w, 1e-3);
    // The 64 lanes own texels, not a fixed ray budget. Each lane shades up to
    // four recorded rays; all are available to the convolution below.
    [loop] for (uint i = lane; i < ray_count; i += 64) {
        DDGIRayHit hit = lit_probe_ray_hits[group_id.x * ray_count + i];
        if (i == 0) ddgi_discard_history = (hit.flags & DDGI_RAY_DISCARD_HISTORY) != 0;
        float3 direction = ddgi_illumination_direction(i,ray_count,ddgi_ray_rotation);
        float3 radiance = hit.radiance_distance.rgb;
        if ((hit.flags & DDGI_RAY_SHADEABLE) != 0) {
            LitSurface surface = (LitSurface)0;
            surface.position = position + direction * hit.radiance_distance.w;
            surface.base = hit.base_roughness.rgb;
            surface.roughness = hit.base_roughness.w;
            surface.normal = hit.normal_metallic.xyz;
            surface.metallic = hit.normal_metallic.w;
            surface.geometric_normal = hit.geometric_normal;
            uint seed = lit_hash(probe ^ lit_hash(i + DDGI_MAX_RAYS * lit_probe_sequence));
            radiance = ddgi_hit_radiance(surface,-direction,seed,inspect && probe==lit_probe_inspect_source,i,hit.radiance_distance.w);
        }
        ddgi_ray_distances[i] = hit.radiance_distance.w;
        ddgi_ray_directions[i] = direction;
        ddgi_ray_radiance[i] = radiance;
    }
    GroupMemoryBarrierWithGroupSync();
    // Trace and every Update lane used the same start. Advance only after an
    // active batch; budget changes cannot skip or repeat its rotation.
    if(lane==0) {
        uint next=ddgi_sample_start.x+1;
        lit_probe_sample_cursor[probe_texel]=uint2(next,ddgi_sample_start.y+uint(next<ddgi_sample_start.x));
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
        previous_irradiance = lit_probe_irradiance_previous.Load(int3(atlas_texel, 0));
        previous_distance = lit_probe_distance_previous.Load(int3(atlas_texel, 0));
    }
    if (!all(isfinite(previous_irradiance))) previous_irradiance = 0;
    // Any new origin/identity must discard the display prior in this same
    // frame, even if its first valid transport observation is already ready.
    if (ddgi_discard_history != 0) lit_probe_presentation[atlas_texel] = 0;
    if (!all(isfinite(previous_distance))) previous_distance = float2(1, 1);
    // With fewer rays than directional texels, a hemisphere may have no
    // observation this visit. That is missing support, not measured darkness.
    // Preserve both the target and its observation count (or invalid initial
    // support), so a constant sky cannot be driven toward black by empty visits.
    if (irradiance_weight <= 1e-5) {
        lit_probe_irradiance_output[atlas_texel] = previous_irradiance;
        lit_probe_distance_output[atlas_texel] = previous_distance;
        return;
    }
    uint2 response=lit_probe_response[atlas_texel];
    if(response.x!=lit_probe_response_epoch || ddgi_discard_history!=0 || previous_irradiance.w<=0)
        response=uint2(lit_probe_response_epoch,0);
    // Each direction consumes its own valid observations. A small frame budget,
    // an empty hemisphere or a diagnostic hold cannot exhaust another probe's
    // response window. Authored changes retain light, coverage and geometry.
    float irradiance_history = previous_irradiance.w > 0 ?
        clamp(response.y<4?lit_probe_history.x:(response.y<12?.8:lit_probe_history.w),0,.98) : 0;
    float distance_history = previous_irradiance.w > 0 ? clamp(lit_probe_history.y, 0, .98) : 0;
    // Running-average warmup uses the actual number of visits, not render
    // frames. The application selects response weights from scene changes;
    // a light change never lowers distance history via a luminance hint.
    float warmup_history = previous_irradiance.w / (previous_irradiance.w + 1);
    if (previous_irradiance.w < 8) {
        irradiance_history = min(irradiance_history, warmup_history);
        distance_history = min(distance_history, warmup_history);
    }
    // Importance samples carry inverse-probability compensation. Nonlinear
    // blending or clipping their rare positive contributions loses energy;
    // keep the authoritative irradiance history scene linear.
    float updates = min(previous_irradiance.w + 1, 32);
    if(lit_probe_history_mode!=1 || ddgi_discard_history!=0 || previous_irradiance.w==0) {
        lit_probe_irradiance_output[atlas_texel] = float4(
            ddgi_finite_radiance(lerp(irradiance, max(previous_irradiance.rgb, 0), irradiance_history)), updates);
        lit_probe_response[atlas_texel]=uint2(response.x,min(response.y+1,12u));
    }
    if(lit_probe_history_mode!=2 || ddgi_discard_history!=0 || previous_irradiance.w==0)
        lit_probe_distance_output[atlas_texel] = distance_weight > 1e-8 ?
            lerp(moments, previous_distance, distance_history) : previous_distance;
}

[numthreads(64,1,1)]
void DDGIUpdate(uint3 group_id:SV_GroupID,uint lane:SV_GroupIndex) {ddgi_update(group_id,lane,false);}
[numthreads(64,1,1)]
void DDGIUpdateInspect(uint3 group_id:SV_GroupID,uint lane:SV_GroupIndex) {ddgi_update(group_id,lane,true);}

// Every frame presents the completed next transport snapshot. This low-pass
// never feeds another bounce: ray budget changes sampling, not this time scale.
[numthreads(64,1,1)]
void DDGIPresent(uint3 dispatch_id : SV_DispatchThreadID) {
    uint index=dispatch_id.x;
    if(index>=lit_probe_pool.y*64)return;
    uint2 texel=ddgi_probe_texel(index/64)*8+uint2(index%8,index%64/8);
    float4 transport=lit_probe_irradiance_previous.Load(int3(texel,0));
    float4 previous=lit_probe_presentation[texel];
    if(transport.w<=0) {lit_probe_presentation[texel]=0;return;}
    float history=previous.w>0?lit_probe_history.z:0;
    if(lit_probe_history_mode==1 && previous.w>0)history=1;
    // Explicit endpoint preserves a paused cache bit-for-bit; lerp(a,b,1)
    // can still round differently as the live transport value a changes.
    float3 displayed=history>=1?previous.rgb:lerp(max(transport.rgb,0),max(previous.rgb,0),history);
    lit_probe_presentation[texel]=float4(ddgi_finite_radiance(displayed),transport.w);
}

// One independent gather records the same inputs as receiver shading. Normal
// entries pass inspect=false, so the record writes compile out of their loops.
[numthreads(1,1,1)]
void DDGIInspect() {
    [unroll] for(uint i=0;i<33;++i)lit_probe_inspection[i]=(DDGIContribution)0;
    uint2 pixel=lit_probe_inspect_pixel;
    if(any(pixel>=lit_size_frame.xy) || !lit_inside(pixel))return;
    float depth=lit_depth.Load(int3(pixel,0));
    if(lit_is_background_depth(depth))return;
    float3 position=lit_world((float2(pixel)+.5)/lit_size_frame.xy,depth);
    float3 normal=lit_safe_normal(lit_normal_roughness.Load(int3(pixel,0)).xyz);
    DDGIResult result=ddgi_gather(position,normal,lit_geometry_normal_at(pixel),true,false,true);
    DDGIContribution receiver=(DDGIContribution)0;
    receiver.identity=uint4(result.status,result.volume,result.contributors,0);
    receiver.irradiance=float4(result.irradiance,result.coverage);
    receiver.weights=float4(result.weight_sum,result.dominance,0,0);
    receiver.position=float4(position,1);
    lit_probe_inspection[32]=receiver;
}

[numthreads(8, 8, 1)]
void DDGIResolve(uint3 dispatch_id : SV_DispatchThreadID) {
    uint2 pixel;
    if (!lit_screen_pixel(dispatch_id.xy, pixel)) return;
    float depth = lit_depth.Load(int3(pixel, 0));
    if (!lit_inside(pixel) || lit_is_background_depth(depth)) { lit_indirect[pixel] = 0; return; }
    if ((lit_effect_flags & 64) == 0) {
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
