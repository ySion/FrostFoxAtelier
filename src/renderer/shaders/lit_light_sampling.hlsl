// Native RIS over (stable light identity, uniform unit-square coordinates).
// Area Jacobians belong to the integrand; the proposal is a discrete light PMF.
// The reservoir's contribution weight excludes visibility at every stage.
struct LitLightSource {
    float4 origin_range;
    float4 direction_radius;
    float4 radiance;
    float4 edge1_area;
    float4 edge2;
    float4 uv01;
    float4 uv2;
    uint4 meta;
};
struct LitLightProposal { float probability,accept; uint alias,reserved; };
StructuredBuffer<LitLightSource> lit_sources;
StructuredBuffer<LitLightSource> lit_previous_sources;
StructuredBuffer<LitLightProposal> lit_light_proposals;
StructuredBuffer<uint> lit_current_to_previous;
StructuredBuffer<uint> lit_previous_to_current;
uint4 lit_source_counts; // local,directional,emissive,previous total
uint4 lit_di_settings; // initial proposals,history multiplier,reference enabled,reference estimates
uint lit_di_history_valid;

struct LitLightingSurface {
    float3 position,normal,geometric_normal,view,base;
    float roughness,metallic;
};
struct LitLightEvaluation {
    float3 diffuse,specular,target,direction;
    float distance;
};
// Structured-buffer stride is 24 bytes. Confidence is not an independent
// sample count after spatial reuse; its cap never changes the normalized weight.
struct LitReservoir {
    uint source;
    float2 uv;
    float weight,confidence;
    uint reserved;
};

LitReservoir lit_empty_reservoir() {
    LitReservoir r=(LitReservoir)0;r.source=~0u;return r;
}
float lit_light_target(LitLightEvaluation value) {
    return dot(value.diffuse+value.specular,float3(.2126,.7152,.0722));
}
float2 lit_light_random_uv(inout uint seed) {
    if(seed==0)seed=0xa3c59ac3u;
    float u=lit_random(seed),v=lit_random(seed);return float2(u,v);
}
LitLightEvaluation lit_evaluate_light_source(LitLightSource light, LitLightingSurface surface,
                                            float2 sample_uv, bool include_emitter_specular) {
    LitLightEvaluation result=(LitLightEvaluation)0;
    float3 incoming=max(light.radiance.rgb,0);
    uint kind=light.meta.x;
    if(kind==2) {
        result.direction=-lit_safe_normal(light.direction_radius.xyz);
        if(light.direction_radius.w>0) {
            float3 tangent,bitangent;lit_basis(result.direction,tangent,bitangent);
            float radius=sqrt(sample_uv.x)*tan(light.direction_radius.w);
            float phi=2*LIT_PI*sample_uv.y;
            result.direction=normalize(result.direction+radius*(cos(phi)*tangent+sin(phi)*bitangent));
        }
        result.distance=1e5;
        result.target=surface.position+result.direction*result.distance;
    } else {
        result.target=light.origin_range.xyz;
        float2 bary=0;
        if(kind==1) {
            result.target+=(sample_uv.x-.5)*light.edge1_area.xyz+(sample_uv.y-.5)*light.edge2.xyz;
        } else if(kind==3) {
            float root=sqrt(sample_uv.x);
            bary=float2(root*(1-sample_uv.y),root*sample_uv.y);
            result.target+=bary.x*light.edge1_area.xyz+bary.y*light.edge2.xyz;
        } else if(light.direction_radius.w>0) {
            // Preserve the engine's finite point-source convention: the unit
            // square averages positions on a sphere, without an area multiplier.
            float z=2*sample_uv.x-1,phi=2*LIT_PI*sample_uv.y;
            float radial=sqrt(max(0,1-z*z));
            result.target+=light.direction_radius.w*float3(radial*cos(phi),z,radial*sin(phi));
        }
        float3 delta=result.target-surface.position;
        float distance_squared=dot(delta,delta);
        if(distance_squared<=0||!isfinite(distance_squared))return result;
        result.distance=sqrt(distance_squared);
        result.direction=delta/result.distance;
        float attenuation;
        if(kind==3) {
            if(light.meta.z==~0u||light.edge1_area.w<=0)return result;
            MaterialSurface material=material_surfaces[light.meta.z];
            float2 material_uv=light.uv01.xy*(1-bary.x-bary.y)+light.uv01.zw*bary.x+light.uv2.xy*bary.y;
            if(!material_covered(material,material_uv,0,0,0,false))return result;
            incoming=max(material.emissive_metallic.rgb*material_read(material,4,material_uv,0,0,0,false).rgb,0);
            // CPU supplies the inverse-transpose author front face. A mirrored
            // instance reverses the edge cross product, not this sidedness contract.
            float3 normal=lit_safe_normal(light.direction_radius.xyz);
            float cosine=dot(normal,-result.direction);
            cosine=(light.meta.y&1u)!=0?abs(cosine):saturate(cosine);
            attenuation=light.edge1_area.w*cosine/distance_squared;
        } else {
            float cutoff=saturate(1-pow(result.distance/max(light.origin_range.w,1e-3),4));
            cutoff*=cutoff;
            attenuation=cutoff/max(distance_squared,max(.0025,light.direction_radius.w*light.direction_radius.w));
            if(kind==1) {
                float cosine=dot(lit_safe_normal(light.direction_radius.xyz),-result.direction);
                cosine=(light.meta.y&1u)!=0?abs(cosine):saturate(cosine);
                // The shared input contract bounds radiance * area, not
                // attenuation * area. Multiply the validated pair first.
                precise float3 area_radiance=incoming*light.edge1_area.w;
                incoming=area_radiance;
                attenuation*=cosine;
            }
        }
        incoming*=attenuation;
    }
    if(dot(surface.normal,result.direction)<=0||dot(surface.geometric_normal,result.direction)<=0||!any(incoming>0))return result;
    lit_brdf_components(surface.base,surface.roughness,surface.metallic,surface.normal,surface.view,
        result.direction,result.diffuse,result.specular);
    result.diffuse*=incoming;
    result.specular=(kind!=3||include_emitter_specular)?result.specular*incoming:0;
    return result;
}

// Input sample identities here are always in the current frame's local range.
// A missing reverse map denotes zero support in that previous distribution.
float lit_local_target(LitLightingSurface surface,uint source,float2 uv,bool previous,
                       bool include_emitter_specular) {
    if(source>=lit_source_counts.x)return 0;
    LitLightSource light;
    if(previous) {
        uint index=lit_current_to_previous[source];
        if(index>=lit_source_counts.w)return 0;
        light=lit_previous_sources[index];
    } else light=lit_sources[source];
    return lit_light_target(lit_evaluate_light_source(light,surface,uv,include_emitter_specular));
}
bool lit_reservoir_stream(inout LitReservoir output,inout float sum,uint source,float2 uv,
                          float weight,inout uint seed) {
    if(weight<=0)return false;
    sum+=weight;
    if(output.source==~0u||lit_random(seed)*sum<weight) {output.source=source;output.uv=uv;return true;}
    return false;
}
LitReservoir lit_local_initial(LitLightingSurface surface,bool include_emitter_specular,
                               uint candidates,inout uint seed) {
    LitReservoir result=lit_empty_reservoir();
    if(lit_source_counts.x==0||candidates==0)return result;
    if(seed==0)seed=0xa3c59ac3u;
    result.confidence=float(candidates);
    float sum=0,selected_target=0;
    [loop] for(uint i=0;i<candidates;++i) {
        uint bucket=min(uint(lit_random(seed)*lit_source_counts.x),lit_source_counts.x-1);
        LitLightProposal proposal=lit_light_proposals[bucket];
        uint source=lit_random(seed)<proposal.accept?bucket:proposal.alias;
        if(source>=lit_source_counts.x)continue;
        float probability=lit_light_proposals[source].probability;
        float2 uv=lit_light_random_uv(seed);
        if(probability<=0)continue;
        float target=lit_local_target(surface,source,uv,false,include_emitter_specular);
        if(lit_reservoir_stream(result,sum,source,uv,target/probability,seed))selected_target=target;
    }
    if(result.source!=~0u)result.weight=(sum/float(candidates))/selected_target;
    return result;
}
bool lit_light_sample_visible(LitLightSource light,LitLightingSurface surface, LitLightEvaluation value,bool shadows) {
    if(!shadows||(light.meta.y&2u)==0)return true;
    if(light.meta.x==2)return lit_visible(surface.position,surface.geometric_normal,value.direction,value.distance);
    return lit_visible_target(surface.position,surface.geometric_normal,value.target);
}
void lit_reservoir_lighting(LitLightingSurface surface,LitReservoir reservoir,bool include_emitter_specular,
                            bool shadows,out float3 diffuse,out float3 specular) {
    diffuse=0;specular=0;
    if(reservoir.source>=lit_source_counts.x||reservoir.weight<=0)return;
    LitLightSource light=lit_sources[reservoir.source];
    LitLightEvaluation value=lit_evaluate_light_source(light,surface,reservoir.uv,include_emitter_specular);
    if(lit_light_target(value)<=0||!lit_light_sample_visible(light,surface,value,shadows))return;
    diffuse=value.diffuse*reservoir.weight;
    specular=value.specular*reservoir.weight;
}
void lit_directional_lighting(LitLightingSurface surface,uint samples,bool shadows,inout uint seed,
                              out float3 diffuse,out float3 specular) {
    diffuse=0;specular=0;
    [loop] for(uint i=0;i<lit_source_counts.y;++i) {
        LitLightSource light=lit_sources[lit_source_counts.x+i];
        // LightFrame's tail is exclusively directional. Expose that invariant
        // so the shared evaluator drops local/emissive material and ray paths.
        light.meta.x=2;
        uint count=light.direction_radius.w>0?max(samples,1u):1u;
        float3 light_diffuse=0,light_specular=0;
        [loop] for(uint s=0;s<count;++s) {
            float2 uv=light.direction_radius.w>0?lit_light_random_uv(seed):0;
            LitLightEvaluation value=lit_evaluate_light_source(light,surface,uv,true);
            if(lit_light_target(value)<=0||!lit_light_sample_visible(light,surface,value,shadows))continue;
            light_diffuse+=value.diffuse;light_specular+=value.specular;
        }
        diffuse+=light_diffuse/float(count);specular+=light_specular/float(count);
    }
}
