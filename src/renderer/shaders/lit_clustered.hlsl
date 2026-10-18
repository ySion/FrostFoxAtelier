// Conservative lists of analytic local lights over actual GBuffer receivers.
// Four depth slices per 16x16 tile avoid joining foreground and distant walls.
// The 64-entry capacity limits storage only: overflow enumerates every analytic
// light. Emissive triangles have no finite range and use a separate estimator.
static const uint LIT_CLUSTER_CAPACITY=64;
uint2 lit_cluster_tiles;
RWStructuredBuffer<float2> lit_cluster_depth_output;
RWStructuredBuffer<uint> lit_cluster_count_output;
RWStructuredBuffer<uint> lit_cluster_source_output;
StructuredBuffer<float2> lit_cluster_depth;
StructuredBuffer<uint> lit_cluster_count;
StructuredBuffer<uint> lit_cluster_source;

groupshared uint lit_cluster_z_min,lit_cluster_z_max;
groupshared uint lit_cluster_min[12],lit_cluster_max[12];
groupshared uint lit_cluster_receivers[4],lit_cluster_counts[4];
groupshared uint lit_cluster_inspect_sources[4*LIT_CLUSTER_CAPACITY];
groupshared uint lit_cluster_inspect_histogram[66],lit_cluster_inspect_max;

uint lit_cluster_order(float value) {
    uint bits=asuint(value);
    return bits^((bits&0x80000000u)!=0?~0u:0x80000000u);
}
float lit_cluster_float(uint value) {
    return asfloat(value^((value&0x80000000u)!=0?0x80000000u:~0u));
}
uint lit_cluster_slice(float z,float2 bounds) {
    return min(uint(saturate((z-bounds.x)*bounds.y)*4),3u);
}
float lit_cluster_radius(LitLightSource light) {
    // The cutoff is evaluated from the sampled endpoint, not the source center.
    float extent=light.meta.x==0?light.direction_radius.w:
        .5*(length(light.edge1_area.xyz)+length(light.edge2.xyz));
    return lit_local_range(light)+extent;
}
bool lit_cluster_intersects(LitLightSource light,float3 minimum,float3 maximum) {
    float3 delta=max(max(minimum-light.origin_range.xyz,light.origin_range.xyz-maximum),0);
    float radius=lit_cluster_radius(light);
    // Include a floating-point margin; uncertain/overflowing bounds must retain
    // a light. Shader receiver evaluation remains authoritative at the cutoff.
    radius+=max(1e-4,1e-5*max(radius,max(max(abs(minimum.x),abs(maximum.x)),
        max(max(abs(minimum.y),abs(maximum.y)),max(abs(minimum.z),abs(maximum.z))))));
    float distance_squared=dot(delta,delta),radius_squared=radius*radius;
    return !isfinite(distance_squared)||!isfinite(radius_squared)||distance_squared<=radius_squared;
}

void lit_cluster_build(uint3 group,uint3 local,uint lane,bool inspect) {
    uint tile=group.x+group.y*lit_cluster_tiles.x;
    if(lane==0){lit_cluster_z_min=~0u;lit_cluster_z_max=0;}
    if(lane<12){lit_cluster_min[lane]=~0u;lit_cluster_max[lane]=0;}
    if(lane<4){lit_cluster_receivers[lane]=0;lit_cluster_counts[lane]=0;}
    if(inspect){if(lane<66)lit_cluster_inspect_histogram[lane]=0;if(lane==0)lit_cluster_inspect_max=0;}
    GroupMemoryBarrierWithGroupSync();
    uint2 p=lit_screen_dispatch.xy+group.xy*16+local.xy;
    bool valid=all(p<lit_screen_dispatch.zw)&&lit_direct_surface(int2(p));
    float3 position=0;float z=0;
    if(valid){position=lit_direct_world(int2(p));z=mul(float4(position,1),lit_camera_view).z;
        valid=all(isfinite(position))&&isfinite(z);}
    if(valid){InterlockedMin(lit_cluster_z_min,lit_cluster_order(z));InterlockedMax(lit_cluster_z_max,lit_cluster_order(z));}
    GroupMemoryBarrierWithGroupSync();
    float z_min=lit_cluster_float(lit_cluster_z_min),z_max=lit_cluster_float(lit_cluster_z_max);
    float2 bounds=lit_cluster_z_min==~0u?0:float2(z_min,rcp(max(z_max-z_min,1e-5)));
    if(lane==0)lit_cluster_depth_output[tile]=bounds;
    if(valid) {
        uint slice=lit_cluster_slice(z,bounds);InterlockedAdd(lit_cluster_receivers[slice],1);
        [unroll] for(uint axis=0;axis<3;++axis) {
            InterlockedMin(lit_cluster_min[slice*3+axis],lit_cluster_order(position[axis]));
            InterlockedMax(lit_cluster_max[slice*3+axis],lit_cluster_order(position[axis]));
        }
    }
    GroupMemoryBarrierWithGroupSync();
    uint analytic_count=lit_source_counts.x-lit_source_counts.z;
    [loop] for(uint source=lane;source<analytic_count;source+=256) {
        LitLightSource light=lit_sources[source];
        [unroll] for(uint slice=0;slice<4;++slice)if(lit_cluster_receivers[slice]!=0) {
            float3 minimum,maximum;
            [unroll] for(uint axis=0;axis<3;++axis){minimum[axis]=lit_cluster_float(lit_cluster_min[slice*3+axis]);
                maximum[axis]=lit_cluster_float(lit_cluster_max[slice*3+axis]);}
            if(lit_cluster_intersects(light,minimum,maximum)) {
                uint slot;InterlockedAdd(lit_cluster_counts[slice],1,slot);
                if(slot<LIT_CLUSTER_CAPACITY) {
                    lit_cluster_source_output[(tile*4+slice)*LIT_CLUSTER_CAPACITY+slot]=source;
                    if(inspect)lit_cluster_inspect_sources[slice*LIT_CLUSTER_CAPACITY+slot]=source;
                }
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if(inspect) {
        if(valid) {
            uint slice=lit_cluster_slice(z,bounds),count=lit_cluster_counts[slice],supported=0;
            bool overflow=count>LIT_CLUSTER_CAPACITY;
            if(overflow)count=analytic_count;
            [loop] for(uint i=0;i<count;++i) {
                uint source=overflow?i:lit_cluster_inspect_sources[slice*LIT_CLUSTER_CAPACITY+i];
                if(lit_cluster_intersects(lit_sources[source],position,position))++supported;
            }
            InterlockedAdd(lit_cluster_inspect_histogram[min(supported,65u)],1);
            InterlockedMax(lit_cluster_inspect_max,supported);
        }
        GroupMemoryBarrierWithGroupSync();
        if(lane<66 && lit_cluster_inspect_histogram[lane]!=0)
            InterlockedAdd(lit_ray_counters[11+lane],lit_cluster_inspect_histogram[lane]);
        if(lane==0)InterlockedMax(lit_ray_counters[77],lit_cluster_inspect_max);
    }
    if(lane<4) {
        uint count=lit_cluster_counts[lane];lit_cluster_count_output[tile*4+lane]=count;
        if(lit_cluster_receivers[lane]!=0) {
            InterlockedAdd(lit_ray_counters[8],1);
            if(count>LIT_CLUSTER_CAPACITY)InterlockedAdd(lit_ray_counters[9],1);
            InterlockedMax(lit_ray_counters[10],count);
        }
    }
}

[numthreads(16,16,1)]
void lit_cluster_build_cs(uint3 group:SV_GroupID,uint3 local:SV_GroupThreadID,uint lane:SV_GroupIndex) {
    lit_cluster_build(group,local,lane,false);
}
[numthreads(16,16,1)]
void lit_cluster_inspect_cs(uint3 group:SV_GroupID,uint3 local:SV_GroupThreadID,uint lane:SV_GroupIndex) {
    lit_cluster_build(group,local,lane,true);
}

LitReservoir lit_cluster_emitter_initial(LitLightingSurface surface,inout uint seed) {
    LitReservoir result=lit_empty_reservoir();
    uint emitters=lit_source_counts.z;
    if(emitters==0)return result;
    uint candidates=max(lit_di_settings.x,1u),first=lit_source_counts.x-emitters;
    float sum=0,selected_target=0;
    [loop] for(uint i=0;i<candidates;++i) {
        uint source=first+min(uint(lit_random(seed)*float(emitters)),emitters-1);
        float2 uv=lit_light_random_uv(seed);
        float target=lit_local_target(surface,source,uv,false,false);
        if(lit_reservoir_stream(result,sum,source,uv,target*float(emitters),seed))selected_target=target;
    }
    if(result.source!=~0u){result.weight=(sum/float(candidates))/selected_target;result.target=selected_target;}
    return result;
}

// Selection is shared by the RT and cached point entries. Visibility is kept
// outside this helper so the cached kernels cannot inherit a RayQuery branch.
float lit_cluster_analytic_sample(LitLightSource light,LitLightingSurface surface,inout uint seed,
                                  out LitLightEvaluation selected) {
    float3 delta=surface.position-light.origin_range.xyz;
    float radius=light.direction_radius.w,inner_range=max(lit_local_range(light)-radius,0);
    bool extended=light.meta.x==1 || radius*radius>.01*dot(delta,delta) ||
        (radius>0 && dot(delta,delta)>=inner_range*inner_range);
    uint samples=extended?max(lit_di_settings.x,8u):1u;
    selected=(LitLightEvaluation)0;
    float sum=0,selected_target=0;
    [loop] for(uint i=0;i<samples;++i) {
        float2 uv=light.meta.x==0&&light.direction_radius.w==0?0:lit_light_random_uv(seed);
        LitLightEvaluation value=lit_evaluate_light_source(light,surface,uv,true);
        float target=lit_light_target(value);sum+=target;
        if(target>0 && (selected_target==0 || lit_random(seed)*sum<target)){selected=value;selected_target=target;}
    }
    return selected_target>0?(sum/float(samples))/selected_target:0;
}

uint lit_cluster_accumulate;
struct LitClusterReceiver {
    uint2 pixel;
    LitLightingSurface surface;
    uint cluster,count,pixel_seed;
    bool overflow,shadows;
};
bool lit_cluster_receiver(uint2 dispatch,out LitClusterReceiver receiver) {
    receiver=(LitClusterReceiver)0;
    uint2 p;if(!lit_screen_pixel(dispatch,p))return false;
    // CPU clears exactly once, even when a batch has no point or sun work.
    if(lit_cluster_accumulate==0){lit_direct_diffuse_raw_output[p]=0;lit_direct_specular_raw_output[p]=0;}
    if(!lit_direct_surface(int2(p)))return false;
    receiver.pixel=p;receiver.surface=lit_direct_surface_data(p);
    float z=mul(float4(receiver.surface.position,1),lit_camera_view).z;
    if(!all(isfinite(receiver.surface.position))||!isfinite(z))return false;
    uint2 tile_xy=(p-lit_screen_dispatch.xy)/16;
    uint tile=tile_xy.x+tile_xy.y*lit_cluster_tiles.x;
    receiver.cluster=tile*4+lit_cluster_slice(z,lit_cluster_depth[tile]);
    receiver.count=lit_cluster_count[receiver.cluster];receiver.overflow=receiver.count>LIT_CLUSTER_CAPACITY;
    if(receiver.overflow)receiver.count=lit_source_counts.x-lit_source_counts.z;
    receiver.pixel_seed=lit_hash(p.x+p.y*lit_size_frame.x+lit_size_frame.z*7919u+0x68e31da4u);
    receiver.shadows=(lit_effect_flags&1u)!=0;
    return true;
}
uint lit_cluster_source_at(LitClusterReceiver receiver,uint index) {
    return receiver.overflow?index:lit_cluster_source[receiver.cluster*LIT_CLUSTER_CAPACITY+index];
}
uint lit_cluster_source_seed(LitClusterReceiver receiver,uint source) {
    uint seed=lit_hash(receiver.pixel_seed^lit_hash(source+1));return seed!=0?seed:1;
}
bool lit_cluster_point_endpoint(LitLightSource light,LitLightingSurface surface,bool shadows) {
    return shadows&&(light.meta.y&2u)!=0 &&
        length(surface.position-light.origin_range.xyz)<=max(light.direction_radius.w,lit_controls.y*2);
}
void lit_cluster_store(LitClusterReceiver receiver,float3 diffuse,float3 specular,bool stochastic=true) {
    float4 material=float4(receiver.surface.base,receiver.surface.metallic);
    float3 diffuse_factor=lit_direct_diffuse_factor(material),specular_factor=lit_direct_specular_factor(material);
    float3 divisor=float3(diffuse_factor.r>0?diffuse_factor.r:1,diffuse_factor.g>0?diffuse_factor.g:1,
                         diffuse_factor.b>0?diffuse_factor.b:1);
    uint2 p=receiver.pixel;
    float4 previous_diffuse=lit_direct_diffuse_raw_output[p],previous_specular=lit_direct_specular_raw_output[p];
    float support=stochastic || previous_diffuse.a>0 || previous_specular.a>0?1:0;
    lit_direct_diffuse_raw_output[p]=float4(lit_direct_finite(previous_diffuse.rgb+diffuse/divisor),support);
    lit_direct_specular_raw_output[p]=float4(lit_direct_finite(previous_specular.rgb+specular/specular_factor),support);
}

[numthreads(8,8,1)]
void lit_cluster_points_cs(uint3 dispatch:SV_DispatchThreadID) {
    // Receiver rejection precedes every diagnostic event; all other paths
    // reconverge after the source loop before flushing this invocation's counts.
    LitClusterReceiver receiver;if(!lit_cluster_receiver(dispatch.xy,receiver))return;
    float3 diffuse=0,specular=0;
    [loop] for(uint i=0;i<receiver.count;++i) {
        uint source=lit_cluster_source_at(receiver,i);
        if(!lit_shadow_batch_contains(source))continue;
        LitLightSource light=lit_sources[source];if(light.meta.x!=0)continue;
        light.meta.x=0;
        if(!lit_cluster_intersects(light,receiver.surface.position,receiver.surface.position) ||
            lit_cluster_point_endpoint(light,receiver.surface,receiver.shadows))continue;
        uint seed=lit_cluster_source_seed(receiver,source);
        LitLightEvaluation value;float weight=lit_cluster_analytic_sample(light,receiver.surface,seed,value);
        if(weight<=0)continue;
        if(receiver.shadows&&(light.meta.y&2u)!=0) {
            LitShadowLight cache=lit_shadow_lights[lit_shadow_light_index(source)];
            weight*=lit_shadow_point(cache,light,receiver.surface);
        }
        diffuse+=value.diffuse*weight;specular+=value.specular*weight;
    }
    lit_cluster_store(receiver,diffuse,specular);
}

[numthreads(8,8,1)]
void lit_cluster_sun_cs(uint3 dispatch:SV_DispatchThreadID) {
    LitClusterReceiver receiver;if(!lit_cluster_receiver(dispatch.xy,receiver))return;
    float3 diffuse=0,specular=0;
    [loop] for(uint i=0;i<lit_source_counts.y;++i) {
        uint source=lit_source_counts.x+i;if(!lit_shadow_batch_contains(source))continue;
        LitLightSource light=lit_sources[source];light.meta.x=2;
        uint seed=lit_cluster_source_seed(receiver,source);
        float2 uv=light.direction_radius.w>0?lit_light_random_uv(seed):0;
        LitLightEvaluation value=lit_evaluate_light_source(light,receiver.surface,uv,true);
        if(lit_light_target(value)<=0)continue;
        float visibility=1;
        if(receiver.shadows&&(light.meta.y&2u)!=0) {
            LitShadowLight cache=lit_shadow_lights[lit_shadow_light_index(source)];
            visibility=lit_shadow_directional(cache,light,receiver.surface);
        }
        diffuse+=value.diffuse*visibility;specular+=value.specular*visibility;
    }
    lit_cluster_store(receiver,diffuse,specular);
}

// Radiance is deterministic; only a finite source's soft visibility is random,
// and each such ray records its outcome for the resolve classification.
bool lit_cluster_compact(LitClusterReceiver receiver,LitLightSource light,uint source,
                         inout uint visibility,inout bool stochastic,out LitLightEvaluation value) {
    value=lit_evaluate_compact_light(light,receiver.surface);
    if(lit_light_target(value)<=0)return false;
    if(!receiver.shadows||(light.meta.y&2u)==0)return true;
    uint seed=lit_cluster_source_seed(receiver,source);
    bool visible=lit_light_sample_visible(light,receiver.surface,
        lit_light_shadow_endpoint(light,receiver.surface,value,lit_light_random_uv(seed)),true);
    if(light.direction_radius.w>0){stochastic=true;visibility|=lit_direct_visibility_bit(source,visible);}
    return visible;
}
// Both callers specialize this constant at compile time. Neither path reaches
// PCSS; the uncached A entry also has no dependency on shadow metadata. A
// shades points outside four radii and suns as compact sources; nearer or
// rectangle endpoints keep source RIS and force reconstruction.
void lit_cluster_rt(uint2 dispatch,bool residual) {
    uint2 pixel;
    if(!residual&&lit_screen_pixel(dispatch,pixel))lit_direct_visibility_output[pixel]=0;
    LitClusterReceiver receiver;if(!lit_cluster_receiver(dispatch,receiver))return;
    float3 diffuse=0,specular=0;
    // Classify support before sampling or visibility rejection: a random zero
    // observation must never disable the estimator's reconstruction. Cached
    // shadow batches conservatively retain reconstruction in this candidate.
    bool stochastic=residual || lit_source_counts.z!=0;
    uint visibility=stochastic?LIT_DIRECT_FORCED:0;
    [loop] for(uint i=0;i<receiver.count;++i) {
        uint source=lit_cluster_source_at(receiver,i);
        if(residual&&!lit_shadow_batch_contains(source))continue;
        LitLightSource light=lit_sources[source];
        light.meta.x=min(light.meta.x,1u);
        bool endpoint=light.meta.x==0&&lit_cluster_point_endpoint(light,receiver.surface,receiver.shadows);
        if(residual&&light.meta.x==0&&!endpoint)continue;
        if(!lit_cluster_intersects(light,receiver.surface.position,receiver.surface.position))continue;
        float3 offset=receiver.surface.position-light.origin_range.xyz;
        float radius=light.direction_radius.w;
        if(!residual&&light.meta.x==0&&16*radius*radius<=dot(offset,offset)) {
            LitLightEvaluation value;
            if(lit_cluster_compact(receiver,light,source,visibility,stochastic,value)){diffuse+=value.diffuse;specular+=value.specular;}
            continue;
        }
        stochastic=stochastic || light.meta.x==1 || radius>0;
        if(light.meta.x==1||radius>0)visibility|=LIT_DIRECT_FORCED;
        uint seed=lit_cluster_source_seed(receiver,source);
        LitLightEvaluation value;float weight=lit_cluster_analytic_sample(light,receiver.surface,seed,value);
        if(weight<=0)continue;
        if(residual&&endpoint)lit_shadow_count(80);
        if(!lit_light_sample_visible(light,receiver.surface,value,receiver.shadows))continue;
        diffuse+=value.diffuse*weight;specular+=value.specular*weight;
    }
    if(!residual||lit_shadow_first_batch!=0) {
        uint seed=receiver.pixel_seed!=0?receiver.pixel_seed:1;
        LitReservoir emitter=lit_cluster_emitter_initial(receiver.surface,seed);
        float3 local_diffuse,local_specular;
        lit_reservoir_lighting(receiver.surface,emitter,false,receiver.shadows,local_diffuse,local_specular);
        diffuse+=local_diffuse;specular+=local_specular;
    }
    if(!residual) {
        [loop] for(uint i=0;i<lit_source_counts.y;++i) {
            uint source=lit_source_counts.x+i;
            LitLightSource light=lit_sources[source];light.meta.x=2;
            LitLightEvaluation value;
            if(lit_cluster_compact(receiver,light,source,visibility,stochastic,value)){diffuse+=value.diffuse;specular+=value.specular;}
        }
        lit_direct_visibility_output[receiver.pixel]=visibility;
    }
    lit_cluster_store(receiver,diffuse,specular,stochastic);
}
[numthreads(8,8,1)]
void lit_cluster_shade_cs(uint3 dispatch:SV_DispatchThreadID) {lit_cluster_rt(dispatch.xy,false);}
[numthreads(8,8,1)]
void lit_cluster_residual_cs(uint3 dispatch:SV_DispatchThreadID) {lit_cluster_rt(dispatch.xy,true);}
