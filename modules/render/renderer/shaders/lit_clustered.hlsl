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
    // Receiver view-Z range for cascade fitting; [82] holds the complement of the minimum.
    if(lane==0&&lit_cluster_z_min!=~0u){InterlockedMax(lit_ray_counters[82],~lit_cluster_z_min);InterlockedMax(lit_ray_counters[83],lit_cluster_z_max);}
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
RWTexture2D<float4> lit_motion_output; // The GBuffer motion target; compose fills its sky-map background pixels.
RWStructuredBuffer<uint> lit_sun_trace_list;

// Every shadow-casting light samples the atlas. A casting light the atlas
// could not hold, or with a face not drawn yet, has no descriptor and
// contributes nothing; meta.w fades in a light that waited. Negative: a sun
// receiver beyond every cascade, left to lit_sun_trace_cs.
float lit_compose_visibility(LitLightSource light,uint source,LitLightingSurface surface,bool shadows) {
    if(!shadows||(light.meta.y&2u)==0)return 1;
    LitShadowLight cache=lit_shadow_lights[source];
    if(cache.meta.x==0)return 0;
    float visibility=cache.meta.x==2?lit_shadow_directional(cache,surface):lit_shadow_point(cache,light,surface);
    return visibility<0?-1:asfloat(cache.meta.w)*visibility;
}
// Linearly transformed cosines (Heitz et al. 2016) fitted to
// lit_brdf_components by tools/ltc/fit.py. Cell (theta row, roughness column)
// holds the inverse transform m00, m20, m02, m22 with m11 = 1, then the
// specular albedo with F = 1 and its (1 - v.h)^5 moment.
struct LitLtcCell { float4 inverse; float2 amplitude; };
StructuredBuffer<LitLtcCell> lit_ltc;
static const uint LIT_LTC_SIZE=64;
void lit_ltc_lookup(float roughness,float nv,out float3x3 inverse,out float2 amplitude) {
    float2 coordinate=float2(saturate(roughness),sqrt(saturate(1-nv)))*(LIT_LTC_SIZE-1);
    uint2 base=min(uint2(coordinate),LIT_LTC_SIZE-2);
    float2 f=coordinate-base;
    LitLtcCell c00=lit_ltc[base.y*LIT_LTC_SIZE+base.x],c10=lit_ltc[base.y*LIT_LTC_SIZE+base.x+1];
    LitLtcCell c01=lit_ltc[(base.y+1)*LIT_LTC_SIZE+base.x],c11=lit_ltc[(base.y+1)*LIT_LTC_SIZE+base.x+1];
    float4 m=lerp(lerp(c00.inverse,c10.inverse,f.x),lerp(c01.inverse,c11.inverse,f.x),f.y);
    amplitude=lerp(lerp(c00.amplitude,c10.amplitude,f.x),lerp(c01.amplitude,c11.amplitude,f.x),f.y);
    inverse=float3x3(m.x,0,m.z, 0,1,0, m.y,0,m.w);
}
// Edge term of the polygon's vector form factor for corners c+a and c+b.
// Expanding the cross product around the shared centre c avoids cancelling
// nearly parallel corner vectors, which the near-cancelling edge sum of a
// small, distant rectangle would amplify again. Scale-free; atan2 keeps the
// small angles accurate.
float3 lit_ltc_edge(float3 c,float3 a,float3 b) {
    float3 n=cross(c,b-a)+cross(a,b);float s=length(n);
    return s>0?n*(atan2(s,dot(c+a,c+b))/s):0;
}
// Vector form factor (times 2 pi) of the quad c+q[i] clipped to z > 0. A convex
// quad leaves the horizon at most once, so one horizon edge closes the clip.
float3 lit_ltc_quad(float3 c,float3 q[4]) {
    float3 sum=0,leaving=0,entering=0;bool clipped=false;
    [unroll] for(uint i=0;i<4;++i) {
        float3 a=q[i],b=q[(i+1)&3];
        float za=c.z+a.z,zb=c.z+b.z;
        bool above_a=za>0,above_b=zb>0;
        if(above_a&&above_b)sum+=lit_ltc_edge(c,a,b);
        else if(above_a||above_b) {
            float3 crossing=lerp(a,b,za/(za-zb));crossing.z=-c.z;clipped=true;
            if(above_a){sum+=lit_ltc_edge(c,a,crossing);leaving=crossing;}
            else {sum+=lit_ltc_edge(c,crossing,b);entering=crossing;}
        }
    }
    if(clipped)sum+=lit_ltc_edge(c,leaving,entering);
    return sum;
}
// Rectangle radiance integrated over its solid angle: the clamped-cosine
// polygon integral gives the diffuse term, whose (1 - F) uses the half vector
// of the polygon's mean incident direction; the fitted LTC gives the GGX term.
// Both are exact for the point limit's direction. The range window starts at
// the extent the cluster bound already includes. A one-sided rectangle lights
// only the side its direction faces; a receiver whose geometric plane has every
// corner behind it receives nothing. Visibility is separate.
LitLightEvaluation lit_evaluate_rectangle_light(LitLightSource light,LitLightingSurface surface) {
    LitLightEvaluation result=(LitLightEvaluation)0;
    float3 delta=light.origin_range.xyz-surface.position;
    float distance=length(delta);
    float3 facing=lit_safe_normal(light.direction_radius.xyz);
    float side=dot(facing,-delta);
    // A receiver in the emitter's plane sees it edge-on, also when it is two-sided.
    if(!(distance>0)||((light.meta.y&1u)!=0?abs(side):side)<=0)return result;
    result.target=light.origin_range.xyz;result.distance=distance;result.direction=delta/distance;
    float window=saturate(1-pow(max(distance-(lit_cluster_radius(light)-lit_local_range(light)),0)/lit_local_range(light),4));
    window*=window;
    float3 radiance=max(light.radiance.rgb,0)*window;
    float3 half_width=.5*light.edge1_area.xyz,half_height=.5*light.edge2.xyz;
    float3 offsets[4]={-half_width-half_height,half_width-half_height,half_width+half_height,half_height-half_width};
    bool front=false;
    [unroll] for(uint c=0;c<4;++c)front=front||dot(surface.geometric_normal,delta+offsets[c])>0;
    if(!front||!any(radiance>0))return result;
    float3 n=surface.normal,v=surface.view,t1=v-n*dot(n,v),t2;
    if(dot(t1,t1)>1e-12){t1=normalize(t1);t2=cross(n,t1);}
    else lit_basis(n,t1,t2);
    float3 local[4],transformed[4];
    float3x3 inverse;float2 amplitude;
    float nv=max(saturate(dot(n,v)),1e-4);
    lit_ltc_lookup(surface.roughness,nv,inverse,amplitude);
    float3 centre=float3(dot(delta,t1),dot(delta,t2),dot(delta,n));
    [unroll] for(uint i=0;i<4;++i) {
        local[i]=float3(dot(offsets[i],t1),dot(offsets[i],t2),dot(offsets[i],n));
        transformed[i]=mul(inverse,local[i]);
    }
    float3 form=lit_ltc_quad(centre,local);
    float diffuse_integral=abs(form.z)/(2*LIT_PI);
    float specular_integral=abs(lit_ltc_quad(mul(inverse,centre),transformed).z)/(2*LIT_PI);
    float3 f0=lerp(.04.xxx,surface.base,surface.metallic);
    if(diffuse_integral>0) {
        float3 mean=lit_safe_normal(form*(form.z<0?-1:1));
        float3 l=t1*mean.x+t2*mean.y+n*mean.z;
        float schlick=pow(1-saturate(dot(v,lit_safe_normal(v+l))),5);
        result.diffuse=(1-f0)*(1-schlick)*surface.base*(1-surface.metallic)*diffuse_integral*radiance;
    }
    result.specular=(f0*amplitude.x+(1-f0)*amplitude.y)*specular_integral*radiance;
    return result;
}
// Deterministic radiance: spheres and suns use Karis' representative point;
// rectangles integrate their solid angle with linearly transformed cosines.
float3 lit_compose_light(LitLightSource light,uint source,LitLightingSurface surface,bool shadows,inout bool traced) {
    LitLightEvaluation value;
    if(light.meta.x==1)value=lit_evaluate_rectangle_light(light,surface);
    else value=lit_evaluate_compact_light(light,surface);
    if(lit_light_target(value)<=0)return 0;
    float visibility=lit_compose_visibility(light,source,surface,shadows);
    if(visibility<0){traced=true;return 0;}
    return (value.diffuse+value.specular)*visibility;
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
    float3 radiance=0;bool traced=false;
    [loop] for(uint i=0;i<count;++i) {
        uint source=overflow?i:lit_cluster_source[cluster*LIT_CLUSTER_CAPACITY+i];
        LitLightSource light=lit_sources[source];
        if(lit_cluster_intersects(light,surface.position,surface.position))
            radiance+=lit_compose_light(light,source,surface,shadows,traced);
    }
    [loop] for(uint j=0;j<lit_source_counts.y;++j) {
        uint source=lit_source_counts.x+j;
        LitLightSource light=lit_sources[source];light.meta.x=2;
        radiance+=lit_compose_light(light,source,surface,shadows,traced);
    }
    // At most one entry per pixel; lit_sun_trace_list holds a whole target.
    if(traced){uint slot;InterlockedAdd(lit_ray_counters[84],1,slot);lit_sun_trace_list[slot]=p.x|(p.y<<16);}
    return lit_direct_finite(radiance);
}

[numthreads(8,8,1)]
void lit_compose_cs(uint3 tid:SV_DispatchThreadID) {
    uint2 p;if(!lit_screen_pixel(tid.xy,p))return;
    if(!lit_inside(p)||lit_is_background_depth(lit_depth[p])) {
        // The Sky Light is incident radiance; only a sky map is also seen behind the scene.
        float3 sky=0;
        if(lit_inside(p)&&lit_sky.w!=0) {
            sky=min(lit_sky_background_at(p),65504.xxx);
            lit_motion_output[p]=lit_background_motion(p);
        }
        lit_hdr_output[p]=float4(sky,1);return;
    }
    float4 bm=lit_base_metallic[p]; float ao=(lit_effect_flags&4)!=0?lit_ao[p]:1;
    float3 irradiance=(lit_effect_flags&2)!=0?lit_indirect_source[p].rgb:lit_environment.rgb*LIT_PI;
    float3 diffuse=bm.rgb*(1-bm.w)*irradiance/LIT_PI*ao*lit_indirect_gain;
    float3 direct=lit_direct_surface(int2(p))?lit_compose_direct(p):0;
    lit_hdr_output[p]=float4(min(direct+diffuse+lit_emissive[p].rgb,65504.xxx),1);
}
RWStructuredBuffer<uint> lit_sun_trace_arguments;
// Rows of at most 65535 groups, the D3D12 limit per dimension; a 4K list needs two.
static const uint lit_sun_trace_row=65535;
[numthreads(1,1,1)]
void lit_sun_trace_prepare_cs() {
    uint groups=(lit_ray_counters[84]+63)/64;
    lit_sun_trace_arguments[0]=min(groups,lit_sun_trace_row);lit_sun_trace_arguments[1]=max((groups+lit_sun_trace_row-1)/lit_sun_trace_row,1);
    lit_sun_trace_arguments[2]=1;
}
// Adds the suns composition left out: for each listed pixel, every casting sun
// no cascade holds is traced as far as cascade depth reaches, every caster.
[numthreads(64,1,1)]
void lit_sun_trace_cs(uint3 tid:SV_DispatchThreadID) {
    uint entry=tid.y*lit_sun_trace_row*64+tid.x;
    if(entry>=lit_ray_counters[84])return;
    uint packed=lit_sun_trace_list[entry];uint2 p=uint2(packed&0xffffu,packed>>16);
    LitLightingSurface surface=lit_direct_surface_data(p);
    float3 radiance=0;
    [loop] for(uint j=0;j<lit_source_counts.y;++j) {
        uint source=lit_source_counts.x+j;
        LitLightSource light=lit_sources[source];light.meta.x=2;
        LitShadowLight cache=lit_shadow_lights[source];
        if((light.meta.y&2u)==0||cache.meta.x!=2||lit_shadow_directional_covered(cache,surface))continue;
        LitLightEvaluation value=lit_evaluate_compact_light(light,surface);
        if(lit_light_target(value)<=0)continue;
        if(lit_visible(surface.position,surface.geometric_normal,-lit_safe_normal(light.direction_radius.xyz),3.402823466e+38f))
            radiance+=(value.diffuse+value.specular)*asfloat(cache.meta.w);
    }
    lit_hdr_output[p]=float4(min(lit_hdr_output[p].rgb+lit_direct_finite(radiance),65504.xxx),1);
}
