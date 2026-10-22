// Conservative lists of local lights over actual GBuffer receivers. Four
// depth slices per 16x16 tile avoid joining foreground and distant walls. The
// 64-entry capacity limits storage only: overflow enumerates every local light.
static const uint LIT_CLUSTER_CAPACITY=64;
uint2 lit_cluster_tiles;
// Tile grid origin. Full-window composition widens lit_screen_dispatch, never the grid.
uint2 lit_cluster_origin;
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
    uint2 p=lit_cluster_origin+group.xy*16+local.xy;
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
    [loop] for(uint source=lane;source<lit_source_counts.x;source+=256) {
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
            if(overflow)count=lit_source_counts.x;
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

Texture2D<float4> lit_indirect_source;
float lit_indirect_gain;
Texture2D<float> lit_ao;
RWTexture2D<float4> lit_hdr_output;

// Every shadow-casting light samples the atlas. A casting light the atlas
// could not hold, or with a face not drawn yet, has no descriptor and
// contributes nothing; meta.w fades in a light that waited.
float lit_compose_visibility(LitLightSource light,uint source,LitLightingSurface surface,bool shadows) {
    if(!shadows||(light.meta.y&2u)==0)return 1;
    LitShadowLight cache=lit_shadow_lights[source];
    if(cache.meta.x==0)return 0;
    return asfloat(cache.meta.w)*(cache.meta.x==2?lit_shadow_directional(cache,light,surface):lit_shadow_point(cache,light,surface));
}
// Deterministic radiance: spheres and suns use Karis' representative point;
// rectangles are evaluated at their centre with the area-scaled cosine, and
// their range window starts at the extent the cluster bound already includes.
float3 lit_compose_light(LitLightSource light,uint source,LitLightingSurface surface,bool shadows) {
    LitLightEvaluation value;
    if(light.meta.x==1) {
        LitLightSample prepared=lit_prepare_light_sample(light,.5);
        prepared.cutoff_offset=lit_cluster_radius(light)-lit_local_range(light);
        value=lit_evaluate_light_sample(prepared,surface);
    } else value=lit_evaluate_compact_light(light,surface);
    if(lit_light_target(value)<=0)return 0;
    return (value.diffuse+value.specular)*lit_compose_visibility(light,source,surface,shadows);
}
float3 lit_compose_direct(uint2 p) {
    LitLightingSurface surface=lit_direct_surface_data(p);
    float z=mul(float4(surface.position,1),lit_camera_view).z;
    if(!all(isfinite(surface.position))||!isfinite(z))return 0;
    bool shadows=(lit_effect_flags&1u)!=0;
    uint2 tile_xy=(p-lit_cluster_origin)/16;
    uint tile=tile_xy.x+tile_xy.y*lit_cluster_tiles.x;
    uint cluster=tile*4+lit_cluster_slice(z,lit_cluster_depth[tile]);
    uint count=lit_cluster_count[cluster];
    bool overflow=count>LIT_CLUSTER_CAPACITY;
    if(overflow)count=lit_source_counts.x;
    float3 radiance=0;
    [loop] for(uint i=0;i<count;++i) {
        uint source=overflow?i:lit_cluster_source[cluster*LIT_CLUSTER_CAPACITY+i];
        LitLightSource light=lit_sources[source];
        if(lit_cluster_intersects(light,surface.position,surface.position))
            radiance+=lit_compose_light(light,source,surface,shadows);
    }
    [loop] for(uint j=0;j<lit_source_counts.y;++j) {
        uint source=lit_source_counts.x+j;
        LitLightSource light=lit_sources[source];light.meta.x=2;
        radiance+=lit_compose_light(light,source,surface,shadows);
    }
    return lit_direct_finite(radiance);
}

[numthreads(8,8,1)]
void lit_compose_cs(uint3 tid:SV_DispatchThreadID) {
    uint2 p;if(!lit_screen_pixel(tid.xy,p))return;
    if(!lit_inside(p)||lit_is_background_depth(lit_depth[p])) {
        // Sky Light supplies incident radiance, not a visible sky surface.
        lit_hdr_output[p]=float4(0,0,0,1);return;
    }
    float4 bm=lit_base_metallic[p]; float ao=(lit_effect_flags&4)!=0?lit_ao[p]:1;
    float3 irradiance=(lit_effect_flags&2)!=0?lit_indirect_source[p].rgb:lit_environment.rgb*LIT_PI;
    float3 diffuse=bm.rgb*(1-bm.w)*irradiance/LIT_PI*ao*lit_indirect_gain;
    float3 direct=lit_direct_surface(int2(p))?lit_compose_direct(p):0;
    lit_hdr_output[p]=float4(min(direct+diffuse+lit_emissive[p].rgb,65504.xxx),1);
}
