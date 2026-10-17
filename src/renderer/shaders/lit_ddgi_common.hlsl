// Shared DDGI coordinates, atlas sampling and receiver gather.
// lit_common.hlsl is prepended by the renderer.
// Each probe owns an 8x8 octahedral tile. RG32F distance moments are normalized
// by maxRayDistance; this is a unit convention, not a reduction to FP16.
// Radiance histories are ping-ponged; probe evaluation age is an in-place counter.
struct DDGIVolume {
    float4 origin; // First logical grid point, maximum ray distance.
    float4 spacing; // XYZ spacing, border blend in cells.
    float4 tracking; // Fractional focus cell, following flag.
    uint4 grid; // XYZ counts, first physical pool slot.
    int4 scroll; // Ring mapping, reserved.
    int4 movement; // Current minus previous logical window; w resets this range.
};
DDGIVolume lit_probe_volumes[4]; // Query snapshot: previous during update, current during resolve.
DDGIVolume lit_probe_current_volumes[4];
uint4 lit_probe_pool; // Atlas probe width, total resident, reserved, previous snapshot valid.
uint4 lit_probe_work[1024]; // Bounded host schedule, physical slot IDs.
float4 lit_probe_controls; // Bounce transport, feedback LOS, reserved.
float4 lit_probe_scene_min,lit_probe_scene_max; // Actual shared scene transport domain.
float4 lit_probe_history; // Responding transport, distance, presentation, stationary transport old weights.
uint lit_probe_response_epoch;
uint lit_probe_sequence; // Advances only with this field's updates; resets with its lifetime.
uint4 lit_probe_update; // reserved, bounded count, illumination rays, reset bit
uint lit_probe_history_mode; // 0 live, 1 hold irradiance, 2 hold distance; diagnostics only.
uint2 lit_probe_inspect_pixel;
struct DDGIContribution {
    uint4 identity;
    float4 irradiance;
    float4 weights;
    float4 position;
    float4 transport;
    uint4 response;
};
RWStructuredBuffer<DDGIContribution> lit_probe_inspection;
struct DDGIInspectedRay {
    uint4 identity;
    float4 direct;
    float4 surface;
    float4 sampling;
    float4 endpoint_distance;
    float4 source_parameters;
};
uint lit_probe_inspect_source;
RWStructuredBuffer<DDGIInspectedRay> lit_probe_inspected_rays;

// Transient trace-to-shade record: 64 bytes, indexed by work ordinal and ray.
// Distances remain signed FP32 scene units until the moment convolution.
struct DDGIRayHit {
    float4 base_roughness;
    float4 normal_metallic;
    float3 geometric_normal;
    uint flags;
    float4 radiance_distance;
};
RWStructuredBuffer<DDGIRayHit> lit_probe_ray_hits_output;
StructuredBuffer<DDGIRayHit> lit_probe_ray_hits;

Texture2D<float4> lit_probe_irradiance_previous;
Texture2D<float4> lit_probe_transport; // Inspection only; normal gathers compile it out.
Texture2D<float2> lit_probe_distance_previous;
Texture2D<float4> lit_probe_offsets_previous;
Texture2D<float4> lit_probe_offsets_current;
RWTexture2D<float4> lit_probe_irradiance_output;
// Frame-time presentation cache. Never read by transport feedback. Its w
// mirrors transport support maturity; it is not a display observation count.
RWTexture2D<float4> lit_probe_presentation;
RWTexture2D<float2> lit_probe_distance_output;
RWTexture2D<float4> lit_probe_offsets_output;
RWTexture2D<float4> lit_indirect; // irradiance, before surface albedo / pi
RWTexture2D<uint> lit_probe_age_output; // 0 = evaluated this update, ~0 = never evaluated
RWTexture2D<uint2> lit_probe_sample_cursor; // Active illumination visits and wrap epoch, per physical identity.
RWTexture2D<uint2> lit_probe_response; // Author change epoch and effective observations, per directional texel.
Texture2D<uint> lit_probe_age;

static const uint DDGI_TILE_SIZE = 8;
static const uint DDGI_MAX_RAYS = 256;
static const uint DDGI_FIXED_RAYS = 32;
static const uint DDGI_RAY_SHADEABLE = 1;
static const uint DDGI_RAY_DISCARD_HISTORY = 2;

float3 ddgi_finite_radiance(float3 radiance) {
    // FP32 linear irradiance preserves importance-weighted samples before
    // accumulation. Exposure and display-range clamping belong to composition.
    return all(isfinite(radiance)) ? max(radiance, 0) : 0;
}

uint ddgi_volume_count(DDGIVolume v) { return v.grid.x * v.grid.y * v.grid.z; }
uint ddgi_volume_for_probe(uint probe) {
    [unroll] for (uint slot=0;slot<4;++slot) {
        DDGIVolume v=lit_probe_current_volumes[slot];
        if(probe>=v.grid.w && probe<v.grid.w+ddgi_volume_count(v))return slot;
    }
    return 0;
}
int3 ddgi_wrap(int3 value, uint3 size) { return (value % int3(size) + int3(size)) % int3(size); }
uint3 ddgi_probe_coordinates(uint probe, DDGIVolume v) {
    uint local=probe-v.grid.w;
    int3 physical=int3(local%v.grid.x, local/v.grid.x%v.grid.y, local/(v.grid.x*v.grid.y));
    return uint3(ddgi_wrap(physical-v.scroll.xyz,v.grid.xyz));
}

uint2 ddgi_probe_texel(uint probe) {
    return uint2(probe % lit_probe_pool.x, probe / lit_probe_pool.x);
}

uint ddgi_physical_probe(uint3 logical, DDGIVolume v) {
    uint3 p=uint3(ddgi_wrap(int3(logical)+v.scroll.xyz,v.grid.xyz));
    return v.grid.w+p.x+v.grid.x*(p.y+v.grid.y*p.z);
}

bool ddgi_probe_fresh(uint probe, DDGIVolume v) {
    int3 old=int3(ddgi_probe_coordinates(probe,v))+v.movement.xyz;
    return v.movement.w!=0 || any(old<0) || any(old>=int3(v.grid.xyz));
}
float3 ddgi_probe_position(uint probe, float3 offset, DDGIVolume v) {
    return v.origin.xyz + float3(ddgi_probe_coordinates(probe,v))*v.spacing.xyz + offset;
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

struct DDGISampleStencil {
    int2 a, b, c, d;
    float2 fraction;
};
DDGISampleStencil ddgi_sample_stencil(float3 direction) {
    float2 location = ddgi_octahedral_encode(direction) * DDGI_TILE_SIZE - .5;
    int2 lower = int2(floor(location));
    DDGISampleStencil stencil;
    stencil.fraction = frac(location);
    stencil.a = ddgi_fold_texel(lower);
    stencil.b = ddgi_fold_texel(lower + int2(1, 0));
    stencil.c = ddgi_fold_texel(lower + int2(0, 1));
    stencil.d = ddgi_fold_texel(lower + int2(1, 1));
    return stencil;
}
float4 ddgi_sample_tile(Texture2D<float4> atlas, uint probe, DDGISampleStencil stencil) {
    int2 tile = int2(ddgi_probe_texel(probe) * DDGI_TILE_SIZE);
    float4 a = atlas.Load(int3(tile + stencil.a, 0));
    float4 b = atlas.Load(int3(tile + stencil.b, 0));
    float4 c = atlas.Load(int3(tile + stencil.c, 0));
    float4 d = atlas.Load(int3(tile + stencil.d, 0));
    return lerp(lerp(a, b, stencil.fraction.x), lerp(c, d, stencil.fraction.x), stencil.fraction.y);
}
float4 ddgi_sample_tile(Texture2D<float4> atlas, uint probe, float3 direction) {
    return ddgi_sample_tile(atlas, probe, ddgi_sample_stencil(direction));
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

float ddgi_minimum_spacing(DDGIVolume v) {
    return max(1e-3, min(v.spacing.x, min(v.spacing.y, v.spacing.z)));
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

// Only the screen resolve opts in. Feedback and diagnostics retain independent
// exact queries; constant-disabled callers compile out these resources entirely.
Texture2D<float4> lit_probe_visibility_offsets;
struct DDGIVisibilityCache {
    uint2 previous, current; // x: visible; y: queried. Black light is still valid.
    bool enabled;
};
bool ddgi_cached_segment_visible(uint probe,uint corner,float3 offset,float3 from,float3 to,
                                 bool record_queries,inout DDGIVisibilityCache cache) {
    uint bit=1u<<corner;
    bool visible;
    if(cache.enabled && (cache.previous.y&bit)!=0 &&
       all(offset==lit_probe_visibility_offsets.Load(int3(ddgi_probe_texel(probe),0)).xyz))
        visible=(cache.previous.x&bit)!=0;
    else visible=ddgi_segment_visible(from,to,record_queries);
    if(cache.enabled){cache.current.y|=bit;if(visible)cache.current.x|=bit;}
    return visible;
}

// Status: unavailable/outside/pending/inside/no weight/valid dark/valid light/
// missing directional observations/all candidates occluded. Black is valid data.
struct DDGIResult {
    float3 irradiance;
    float coverage;
    uint status, contributors, volume;
    float dominance, weight_sum;
};
DDGIResult ddgi_gather_volume(float3 position,float3 normal,float3 geometric_normal,
                             DDGIVolume v,DDGISampleStencil irradiance_stencil,bool trace_support,bool record_queries,
                             bool inspect,uint slot,inout DDGIVisibilityCache cache) {
    DDGIResult result=(DDGIResult)0;
    if(ddgi_volume_count(v)==0)return result;
    float minimum_spacing=ddgi_minimum_spacing(v);
    float3 endpoint=position+geometric_normal*max(1e-4,minimum_spacing*.0001);
    // Stencil selection and numerical ray clearance have identical semantics
    // for receivers and feedback. Statistical bias affects moments only.
    float3 support_position=position+geometric_normal*(minimum_spacing*.12);
    float3 grid_position=(endpoint-v.origin.xyz)/v.spacing.xyz;
    if(any(grid_position<0) || any(grid_position>float3(v.grid.xyz-1))) {result.status=1;return result;}
    float3 low=0,high=float3(v.grid.xyz-1);
    float border=v.spacing.w;
    if(v.tracking.w!=0) {
        // The influence box follows continuously one cell inside the snapped
        // storage window. Newly exposed slices cannot suddenly own a pixel.
        low=1+v.tracking.xyz;high-=1-v.tracking.xyz;border=max(border,1);
    }
    float3 edge=min(grid_position-low,high-grid_position);
    float blend=border>0?saturate(min(edge.x,min(edge.y,edge.z))/border):1;
    if(blend<=0){result.status=1;return result;}
    uint3 lower=uint3(clamp(floor(grid_position),0,float3(v.grid.xyz-2)));
    float3 fraction=saturate(grid_position-float3(lower));
    float3 accumulated=0;
    float weight_sum=0,maximum_weight=0,readiness_sum=0;
    bool pending=false,inside=false,usable=false,observed=false,weighted=false;
    [loop] for(uint corner=0;corner<8;++corner) {
        uint3 step=uint3(corner&1,(corner>>1)&1,(corner>>2)&1);
        float3 trilinear=lerp(1-fraction,fraction,float3(step));
        float spatial_weight=trilinear.x*trilinear.y*trilinear.z;
        if(spatial_weight<=0)continue;
        uint probe=ddgi_physical_probe(lower+step,v);
        uint record=slot*8+corner;
        if(inspect) {
            DDGIContribution sample=(DDGIContribution)0;
            sample.identity=uint4(slot,probe,1,lit_probe_age.Load(int3(ddgi_probe_texel(probe),0)));
            sample.weights.x=spatial_weight;
            uint2 tile=ddgi_probe_texel(probe)*DDGI_TILE_SIZE;
            uint least=12,most=0;
            [loop] for(uint t=0;t<64;++t) {
                uint2 progress=lit_probe_response[tile+uint2(t%8,t/8)];
                uint count=progress.x==lit_probe_response_epoch?progress.y:0;
                least=min(least,count);most=max(most,count);
            }
            sample.response=uint4(lit_probe_response_epoch,least,most,lit_probe_sample_cursor[ddgi_probe_texel(probe)].x);
            lit_probe_inspection[record]=sample;
        }
        float4 offset=lit_probe_offsets_previous.Load(int3(ddgi_probe_texel(probe),0));
        if(!all(isfinite(offset)) || offset.w==0){pending=true;continue;}
        if(offset.w<.5){if(inspect)lit_probe_inspection[record].identity.z=2;inside=true;continue;}
        usable=true;
        float4 irradiance=ddgi_sample_tile(lit_probe_irradiance_previous,probe,irradiance_stencil);
        if(inspect){lit_probe_inspection[record].irradiance=irradiance;lit_probe_inspection[record].identity.z=3;
            lit_probe_inspection[record].transport=ddgi_sample_tile(lit_probe_transport,probe,irradiance_stencil);}
        if(irradiance.w<=0 || !all(isfinite(irradiance)))continue;
        observed=true;
        float3 probe_position=ddgi_probe_position(probe,offset.xyz,v);
        float3 delta=endpoint-probe_position;
        float3 direction=delta/max(length(delta),1e-5);
        float3 support_delta=support_position-probe_position;
        float support_distance=length(support_delta);
        float scale=max(v.origin.w,1e-3);
        float visibility=ddgi_sample_visibility(probe,support_delta/max(support_distance,1e-5),
            support_distance/scale,minimum_spacing*.002/scale);
        float facing=max(.02,(dot(normal,-direction)+1)*.5);
        float weight=facing*facing*visibility;
        if(weight<.2)weight*=weight*weight/.04;
        weight*=spatial_weight;
        if(inspect) {
            lit_probe_inspection[record].identity.z=4;
            lit_probe_inspection[record].weights=float4(spatial_weight,facing*facing,visibility,weight);
            lit_probe_inspection[record].position=float4(probe_position,support_distance/scale);
        }
        // Exact zero work can be discarded before the expensive segment query.
        if(weight<=0 || !isfinite(weight))continue;
        weighted=true;
        if(trace_support && !ddgi_cached_segment_visible(probe,record,offset.xyz,probe_position,endpoint,record_queries,cache)){
            if(inspect){lit_probe_inspection[record].identity.z=5;lit_probe_inspection[record].weights.w=0;}
            continue;
        }
        if(inspect)lit_probe_inspection[record].identity.z=6;
        accumulated+=max(irradiance.rgb,0)*weight;
        weight_sum+=weight;maximum_weight=max(maximum_weight,weight);
        readiness_sum+=saturate(irradiance.w/4)*weight;
        ++result.contributors;
    }
    result.weight_sum=weight_sum;
    if(weight_sum>0) {
        result.irradiance=ddgi_finite_radiance(accumulated/weight_sum);
        result.coverage=blend*saturate(readiness_sum/weight_sum);
        result.dominance=maximum_weight/weight_sum;
        result.status=any(result.irradiance>0)?6:5;
    } else result.status=observed?(weighted?8:4):(usable?7:(pending?2:(inside?3:4)));
    return result;
}
DDGIResult ddgi_gather(float3 position,float3 normal,float3 geometric_normal,
                       bool trace_support,bool record_queries,bool inspect,inout DDGIVisibilityCache cache) {
    DDGIResult result=(DDGIResult)0;result.volume=0xffffffff;
    if(lit_probe_pool.y==0 || !all(isfinite(position)) || !all(isfinite(normal)))return result;
    DDGISampleStencil irradiance_stencil=ddgi_sample_stencil(normal);
    result.status=1;
    float largest_share=0;
    [loop] for(uint slot=0;slot<4;++slot) {
        DDGIResult local=ddgi_gather_volume(position,normal,geometric_normal,
            lit_probe_volumes[slot],irradiance_stencil,trace_support,record_queries,inspect,slot,cache);
        if(local.status==5 || local.status==6) {
            float share=(1-result.coverage)*local.coverage;
            result.irradiance+=local.irradiance*share;
            result.coverage+=share;result.contributors+=local.contributors;
            result.weight_sum+=local.weight_sum*share;
            result.dominance=max(result.dominance,local.dominance*share);
            if(share>largest_share){largest_share=share;result.volume=slot;}
            result.status=any(result.irradiance>0)?6:5;
            if(result.coverage>=.99999)break;
        } else if(result.coverage==0 && local.status>1 && result.status<=1)result.status=local.status;
    }
    result.irradiance=ddgi_finite_radiance(result.irradiance);
    if(result.coverage>0)result.dominance/=result.coverage;
    return result;
}
DDGIResult ddgi_gather(float3 position,float3 normal,float3 geometric_normal,
                       bool trace_support,bool record_queries,bool inspect=false) {
    DDGIVisibilityCache cache=(DDGIVisibilityCache)0;
    return ddgi_gather(position,normal,geometric_normal,trace_support,record_queries,inspect,cache);
}
float3 ddgi_irradiance(float3 position,float3 normal,float3 geometric_normal,bool trace_support) {
    return ddgi_gather(position,normal,geometric_normal,trace_support,true).irradiance;
}
