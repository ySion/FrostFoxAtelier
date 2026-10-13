// Paper-based generalized balance MIS. Each source target is evaluated at the
// same candidate in the fixed (LightID,u,v) domain, including previous targets.
// Visibility is never part of the stored distribution and is retraced at shading.
StructuredBuffer<LitReservoir> lit_di_previous;
StructuredBuffer<LitReservoir> lit_di_input;
RWStructuredBuffer<LitReservoir> lit_di_output;
Texture2D<float4> lit_history_base_metallic;
Texture2D<uint2> lit_history_surface_id;
uint2 lit_di_dispatch_origin; // Shade only: global pixel origin for bounded reference dispatches.

uint lit_di_pixel_index(uint2 p) {return p.x+p.y*lit_size_frame.x;}
uint lit_di_seed(uint2 p,uint salt) {
    // Reference estimates remain independent across frames as well as within a
    // pixel. No lighting option or sample-count value freezes the random stream.
    uint seed=lit_hash(lit_di_pixel_index(p)+lit_size_frame.z*7919u+salt);
    return seed!=0?seed:0xa3c59ac3u;
}
float lit_di_confidence_cap() {
    return float(max(lit_di_settings.x,1u))*float(max(lit_di_settings.y,1u));
}
LitLightingSurface lit_di_surface(uint2 p) {
    LitLightingSurface surface;
    float2 uv=lit_direct_uv(int2(p));
    float4 nr=lit_normal_roughness.Load(int3(p,0)),bm=lit_base_metallic.Load(int3(p,0));
    surface.position=lit_world(uv,lit_depth.Load(int3(p,0)));
    surface.normal=lit_safe_normal(nr.xyz);surface.geometric_normal=lit_geometry_normal_at(p);
    surface.view=lit_view_direction(uv);surface.base=bm.rgb;
    surface.roughness=nr.w;surface.metallic=bm.w;
    return surface;
}
float3 lit_di_previous_world(float2 uv,float depth) {
    float2 local=(uv*lit_size_frame.xy-lit_viewport.xy)/lit_viewport.zw;
    float4 h=mul(float4(local*float2(2,-2)+float2(-1,1),depth,1),lit_previous_inverse_view_projection);
    return h.xyz/h.w;
}
LitLightingSurface lit_di_previous_surface(uint2 p) {
    LitLightingSurface surface;
    float2 uv=lit_direct_uv(int2(p));
    float4 nr=lit_history_normal.Load(int3(p,0)),bm=lit_history_base_metallic.Load(int3(p,0));
    surface.position=lit_di_previous_world(uv,lit_history_depth.Load(int3(p,0)));
    surface.normal=lit_safe_normal(nr.xyz);
    surface.geometric_normal=lit_decode_normal(lit_history_geometry_normal.Load(int3(p,0)));
    surface.view=lit_safe_normal(lit_di_previous_world(uv,lit_depth_near_value())-
        lit_di_previous_world(uv,lerp(lit_depth_near_value(),lit_depth_far_value(),.5)));
    surface.base=bm.rgb;surface.roughness=nr.w;surface.metallic=bm.w;
    return surface;
}
bool lit_di_temporal_surface(uint2 p,LitLightingSurface current,out uint2 previous_pixel,
                             out LitLightingSurface previous) {
    previous_pixel=0;previous=(LitLightingSurface)0;
    if(lit_di_history_valid==0)return false;
    float4 motion=lit_motion.Load(int3(p,0));
    float2 uv=lit_direct_uv(int2(p))+motion.xy;
    if(motion.w<=.5||!all(isfinite(uv))||any(uv<0)||any(uv>=1))return false;
    int2 q=int2(floor(uv*lit_size_frame.xy));
    if(!lit_inside(uint2(q)))return false;
    if(any(lit_surface_id.Load(int3(p,0))!=lit_history_surface_id.Load(int3(q,0))))return false;
    float depth=lit_history_depth.Load(int3(q,0));
    float expected_depth=lit_previous_depth.Load(int3(p,0));
    float4 nr=lit_history_normal.Load(int3(q,0));
    if(lit_is_background_depth(depth)||!isfinite(depth)||!isfinite(expected_depth)||
       lit_is_background_depth(expected_depth)||dot(nr.xyz,nr.xyz)<.25)return false;
    previous=lit_di_previous_surface(uint2(q));
    if(!all(isfinite(previous.position))||dot(current.normal,previous.normal)<.85||
       dot(current.geometric_normal,previous.geometric_normal)<.95||
       abs(current.roughness-previous.roughness)>.2||abs(current.metallic-previous.metallic)>.2)return false;
    // Test the actual previous surface, reconstructed from FP32 expected depth.
    // The current world position is not valid here when the object moved.
    float3 expected=lit_di_previous_world(uv,expected_depth);
    float pixel_size=max(length(lit_di_previous_world(uv+float2(1.0/lit_size_frame.x,0),expected_depth)-expected),
                         length(lit_di_previous_world(uv+float2(0,1.0/lit_size_frame.y),expected_depth)-expected));
    float3 delta=previous.position-expected;
    if(!all(isfinite(expected))||!isfinite(pixel_size)||
       abs(dot(delta,previous.geometric_normal))>max(1e-4,pixel_size*.35)||
       length(delta)>max(.002,pixel_size*2))return false;
    previous_pixel=uint2(q);return true;
}
struct LitReuseInput {LitLightingSurface surface;LitReservoir reservoir;};
float lit_di_input_target(LitReuseInput input,uint source,float2 uv,bool previous) {
    return lit_local_target(input.surface,source,uv,previous,false);
}
// Realized sources are current-local indices or null. Only temporal slot one
// evaluates its target in the previous domain; spatial inputs are all current.
LitReservoir lit_di_reuse(LitReuseInput inputs[3],uint count,bool temporal,inout uint seed) {
    LitReservoir output=lit_empty_reservoir();float sum=0,confidence=0,selected_target=0;
    [loop] for(uint i=0;i<count;++i)confidence+=inputs[i].reservoir.confidence;
    [loop] for(uint i=0;i<count;++i) {
        LitReservoir candidate=inputs[i].reservoir;
        uint source=candidate.source;
        if(source>=lit_source_counts.x||candidate.weight<=0)continue;
        // Input zero is the current surface. Evaluate each source target once
        // for this candidate and reuse it in both MIS numerator and denominator.
        float target=lit_di_input_target(inputs[0],source,candidate.uv,false);
        if(target<=0)continue;
        float denominator=inputs[0].reservoir.confidence*target;
        float source_target=i==0?target:0;
        [loop] for(uint j=1;j<count;++j) {
            // A null realized sample still represents a valid source distribution.
            // Its target/confidence belongs in every other candidate's denominator.
            float input_target=lit_di_input_target(inputs[j],source,candidate.uv,temporal&&j==1);
            denominator+=inputs[j].reservoir.confidence*input_target;
            if(j==i)source_target=input_target;
        }
        float numerator=candidate.confidence*source_target;
        if(denominator<=0||numerator<=0)continue;
        float weight=(numerator/denominator)*target*candidate.weight;
        if(lit_reservoir_stream(output,sum,source,candidate.uv,weight,seed))selected_target=target;
    }
    output.confidence=min(confidence,lit_di_confidence_cap());
    if(output.source!=~0u)output.weight=sum/selected_target;
    return output;
}

[numthreads(8,8,1)]
void lit_restir_initial_cs(uint3 dispatch:SV_DispatchThreadID) {
    uint2 p=dispatch.xy;if(any(p>=lit_size_frame.xy))return;
    uint index=lit_di_pixel_index(p);
    lit_di_output[index]=lit_empty_reservoir();
    if(lit_source_counts.x==0)return;
    if(lit_di_settings.z!=0||!lit_direct_surface(int2(p)))return;
    LitLightingSurface surface=lit_di_surface(p);
    uint seed=lit_di_seed(p,0x63d83595u);
    LitReuseInput inputs[3];
    inputs[0].surface=surface;
    inputs[0].reservoir=lit_local_initial(surface,false,max(lit_di_settings.x,1u),seed);
    uint count=1;uint2 previous_pixel;LitLightingSurface previous;
    if(lit_di_temporal_surface(p,surface,previous_pixel,previous)) {
        inputs[1].surface=previous;
        inputs[1].reservoir=lit_di_previous[lit_di_pixel_index(previous_pixel)];
        inputs[1].reservoir.confidence=min(inputs[1].reservoir.confidence,lit_di_confidence_cap());
        if(inputs[1].reservoir.confidence>0) {
            uint source=inputs[1].reservoir.source;
            inputs[1].reservoir.source=~0u;
            if(source<lit_source_counts.w) {
                uint mapped=lit_previous_to_current[source];
                if(mapped<lit_source_counts.x)inputs[1].reservoir.source=mapped;
            }
            // A deleted source leaves a null realization, but its distribution
            // still owns confidence and must participate in MIS normalization.
            count=2;
        }
    }
    if(count==1)lit_di_output[index]=inputs[0].reservoir;
    else lit_di_output[index]=lit_di_reuse(inputs,count,true,seed);
}
bool lit_di_spatial_surface(uint2 p,int2 q,LitLightingSurface current,float pixel_size,
                            out LitLightingSurface neighbor) {
    neighbor=(LitLightingSurface)0;
    if(!lit_direct_surface(q))return false;
    neighbor=lit_di_surface(uint2(q));
    if(dot(current.normal,neighbor.normal)<.85||dot(current.geometric_normal,neighbor.geometric_normal)<.95||
       abs(current.roughness-neighbor.roughness)>.2||abs(current.metallic-neighbor.metallic)>.2)return false;
    float3 delta=neighbor.position-current.position;
    float tolerance=max(.0005,pixel_size*.5);
    return abs(dot(delta,current.geometric_normal))<=tolerance&&abs(dot(delta,neighbor.geometric_normal))<=tolerance;
}
[numthreads(8,8,1)]
void lit_restir_spatial_cs(uint3 dispatch:SV_DispatchThreadID) {
    uint2 p=dispatch.xy;if(any(p>=lit_size_frame.xy))return;
    uint index=lit_di_pixel_index(p);
    lit_di_output[index]=lit_empty_reservoir();
    if(lit_source_counts.x==0)return;
    if(lit_di_settings.z!=0||!lit_direct_surface(int2(p)))return;
    LitLightingSurface surface=lit_di_surface(p);
    LitReuseInput inputs[3];
    inputs[0].surface=surface;inputs[0].reservoir=lit_di_input[index];
    uint count=1,seed=lit_di_seed(p,0xb5297a4du);
    static const int2 offsets[8]={int2(1,0),int2(1,1),int2(0,1),int2(-1,1),
                                  int2(-1,0),int2(-1,-1),int2(0,-1),int2(1,-1)};
    uint first=min(uint(lit_random(seed)*8),7u);
    uint second=(first+3u+min(uint(lit_random(seed)*3),2u))&7u;
    float pixel_size=lit_direct_pixel_size(p);
    [unroll] for(uint i=0;i<2;++i) {
        int radius=1+int(lit_random(seed)*5);
        int2 q=int2(p)+offsets[i==0?first:second]*radius;
        LitLightingSurface neighbor;
        if(!lit_di_spatial_surface(p,q,surface,pixel_size,neighbor))continue;
        LitReservoir candidate=lit_di_input[lit_di_pixel_index(uint2(q))];
        // Selection only uses surfaces and history confidence, never a sampled
        // light's brightness, visibility, or whether its realization is null.
        if(candidate.confidence<=0)continue;
        inputs[count].surface=neighbor;inputs[count].reservoir=candidate;
        ++count;
    }
    if(count==1)lit_di_output[index]=inputs[0].reservoir;
    else lit_di_output[index]=lit_di_reuse(inputs,count,false,seed);
}
[numthreads(8,8,1)]
void lit_restir_shade_cs(uint3 dispatch:SV_DispatchThreadID) {
    uint2 p=dispatch.xy+lit_di_dispatch_origin;if(any(p>=lit_size_frame.xy))return;
    lit_direct_diffuse_raw_output[p]=0;lit_direct_specular_raw_output[p]=0;
    if(!lit_direct_surface(int2(p)))return;
    LitLightingSurface surface=lit_di_surface(p);
    uint seed=lit_di_seed(p,0x68e31da4u);
    bool shadows=(lit_effect_flags&1u)!=0;
    float3 diffuse=0,specular=0;
    if(lit_di_settings.z!=0) {
        uint estimates=max(lit_di_settings.w,1u);
        [loop] for(uint i=0;i<estimates;++i) {
            LitReservoir estimate=lit_local_initial(surface,false,max(lit_di_settings.x,1u),seed);
            float3 sample_diffuse,sample_specular;
            lit_reservoir_lighting(surface,estimate,false,shadows,sample_diffuse,sample_specular);
            diffuse+=sample_diffuse;specular+=sample_specular;
        }
        diffuse/=float(estimates);specular/=float(estimates);
    } else {
        LitReservoir estimate=lit_di_input[lit_di_pixel_index(p)];
        lit_reservoir_lighting(surface,estimate,false,shadows,diffuse,specular);
    }
    float3 sun_diffuse,sun_specular;
    uint sun_samples=lit_di_settings.z!=0?max(lit_di_settings.w,1u):1u;
    lit_directional_lighting(surface,sun_samples,shadows,seed,sun_diffuse,sun_specular);
    diffuse+=sun_diffuse;specular+=sun_specular;
    float4 material=float4(surface.base,surface.metallic);
    float3 diffuse_factor=lit_direct_diffuse_factor(material),specular_factor=lit_direct_specular_factor(material);
    float3 divisor=float3(diffuse_factor.r>0?diffuse_factor.r:1,diffuse_factor.g>0?diffuse_factor.g:1,
                         diffuse_factor.b>0?diffuse_factor.b:1);
    lit_direct_diffuse_raw_output[p]=float4(lit_direct_finite(diffuse/divisor),1);
    lit_direct_specular_raw_output[p]=float4(lit_direct_finite(specular/specular_factor),1);
}
