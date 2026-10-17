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
struct LitLightCell { uint offset,count; float mass; uint reserved; };
struct LitSpatialLightProposal { uint source; float accept; uint alias; float probability; };
StructuredBuffer<LitLightSource> lit_sources;
StructuredBuffer<LitLightSource> lit_previous_sources;
StructuredBuffer<LitLightProposal> lit_light_proposals;
StructuredBuffer<LitLightCell> lit_light_cells;
StructuredBuffer<LitSpatialLightProposal> lit_spatial_light_proposals;
StructuredBuffer<uint> lit_spatial_source_slots;
float4 lit_light_grid_origin; // Minimum XYZ, spacing.
uint4 lit_light_grid_counts; // XYZ, enabled for this view.
StructuredBuffer<uint> lit_current_to_previous;
StructuredBuffer<uint> lit_previous_to_current;
uint4 lit_source_counts; // local,directional,emissive,previous total
uint4 lit_di_settings; // initial proposals,history multiplier,reference enabled,reference estimates
uint lit_di_history_valid;

struct LitLightingSurface {
    float3 position,normal,geometric_normal,view,base;
    float roughness,metallic;
};
float lit_local_range(LitLightSource light) { return max(light.origin_range.w,1e-3); }
struct LitLightEvaluation {
    float3 diffuse,specular,target,direction;
    float distance;
};
// A sampled endpoint and emission are independent of the receiver. Reuse keeps
// this smaller value live across its MIS targets instead of rebuilding the same
// endpoint (and reading textured emission) for each input surface.
struct LitLightSample {
    float3 position, radiance, normal;
    float range, minimum_distance_squared, area;
    uint kind, flags;
};
// Structured-buffer stride is 24 bytes. Confidence is not an independent
// sample count after spatial reuse; its cap never changes the normalized weight.
struct LitReservoir {
    uint source;
    float2 uv;
    // Unnormalized, visibility-free target at this reservoir's receiver.
    // This replaces the spare DWORD without changing the 24-byte stride.
    float weight,confidence,target;
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
LitLightSample lit_prepare_light_sample(LitLightSource light, float2 sample_uv) {
    LitLightSample prepared=(LitLightSample)0;
    prepared.kind=light.meta.x;prepared.flags=light.meta.y;
    prepared.radiance=max(light.radiance.rgb,0);
    prepared.range=lit_local_range(light);
    prepared.minimum_distance_squared=max(.0025,light.direction_radius.w*light.direction_radius.w);
    prepared.area=light.edge1_area.w;
    if(prepared.kind==2) {
        prepared.position=-lit_safe_normal(light.direction_radius.xyz);
        if(light.direction_radius.w>0) {
            float3 tangent,bitangent;lit_basis(prepared.position,tangent,bitangent);
            float radius=sqrt(sample_uv.x)*tan(light.direction_radius.w);
            float phi=2*LIT_PI*sample_uv.y;
            prepared.position=normalize(prepared.position+radius*(cos(phi)*tangent+sin(phi)*bitangent));
        }
        return prepared;
    }
    prepared.position=light.origin_range.xyz;
    if(prepared.kind==1) {
        prepared.position+=(sample_uv.x-.5)*light.edge1_area.xyz+(sample_uv.y-.5)*light.edge2.xyz;
        // Preserve the validated radiance * area multiplication order.
        precise float3 area_radiance=prepared.radiance*prepared.area;
        prepared.radiance=area_radiance;
        prepared.normal=lit_safe_normal(light.direction_radius.xyz);
    } else if(prepared.kind==3) {
        if(light.meta.z==~0u||prepared.area<=0){prepared.kind=~0u;return prepared;}
        float root=sqrt(sample_uv.x);
        float2 bary=float2(root*(1-sample_uv.y),root*sample_uv.y);
        prepared.position+=bary.x*light.edge1_area.xyz+bary.y*light.edge2.xyz;
        MaterialSurface material=material_surfaces[light.meta.z];
        float2 material_uv=light.uv01.xy*(1-bary.x-bary.y)+light.uv01.zw*bary.x+light.uv2.xy*bary.y;
        if(!material_covered(material,material_uv,0,0,0,false)){prepared.kind=~0u;return prepared;}
        prepared.radiance=max(material.emissive_metallic.rgb*material_read(material,4,material_uv,0,0,0,false).rgb,0);
        // Mirrored edge winding does not change the inverse-transpose author front.
        prepared.normal=lit_safe_normal(light.direction_radius.xyz);
    } else if(light.direction_radius.w>0) {
        // Finite point sources average sphere positions without an area factor.
        float z=2*sample_uv.x-1,phi=2*LIT_PI*sample_uv.y;
        float radial=sqrt(max(0,1-z*z));
        prepared.position+=light.direction_radius.w*float3(radial*cos(phi),z,radial*sin(phi));
    }
    return prepared;
}
LitLightEvaluation lit_evaluate_light_sample(LitLightSample prepared, LitLightingSurface surface,
                                            bool include_emitter_specular) {
    LitLightEvaluation result=(LitLightEvaluation)0;
    if(prepared.kind==~0u)return result;
    float3 incoming=prepared.radiance;
    if(prepared.kind==2) {
        result.direction=prepared.position;
        result.distance=1e5;
        result.target=surface.position+result.direction*result.distance;
    } else {
        result.target=prepared.position;
        float3 delta=result.target-surface.position;
        float distance_squared=dot(delta,delta);
        if(distance_squared<=0||!isfinite(distance_squared))return result;
        result.distance=sqrt(distance_squared);
        result.direction=delta/result.distance;
        float attenuation;
        if(prepared.kind==3) {
            float cosine=dot(prepared.normal,-result.direction);
            cosine=(prepared.flags&1u)!=0?abs(cosine):saturate(cosine);
            attenuation=prepared.area*cosine/distance_squared;
        } else {
            float cutoff=saturate(1-pow(result.distance/prepared.range,4));
            cutoff*=cutoff;
            attenuation=cutoff/max(distance_squared,prepared.minimum_distance_squared);
            if(prepared.kind==1) {
                float cosine=dot(prepared.normal,-result.direction);
                cosine=(prepared.flags&1u)!=0?abs(cosine):saturate(cosine);
                attenuation*=cosine;
            }
        }
        incoming*=attenuation;
    }
    if(dot(surface.normal,result.direction)<=0||dot(surface.geometric_normal,result.direction)<=0||!any(incoming>0))return result;
    lit_brdf_components(surface.base,surface.roughness,surface.metallic,surface.normal,surface.view,
        result.direction,result.diffuse,result.specular);
    result.diffuse*=incoming;
    result.specular=(prepared.kind!=3||include_emitter_specular)?result.specular*incoming:0;
    return result;
}
LitLightEvaluation lit_evaluate_light_source(LitLightSource light, LitLightingSurface surface,
                                            float2 sample_uv, bool include_emitter_specular) {
    return lit_evaluate_light_sample(lit_prepare_light_sample(light,sample_uv),surface,include_emitter_specular);
}

// Input sample identities here are always in the current frame's local range.
// A missing reverse map denotes zero support in that previous distribution.
LitLightSample lit_local_sample(uint source,float2 uv,bool previous) {
    LitLightSample invalid=(LitLightSample)0;invalid.kind=~0u;
    if(source>=lit_source_counts.x)return invalid;
    LitLightSource light;
    if(previous) {
        uint index=lit_current_to_previous[source];
        if(index>=lit_source_counts.w)return invalid;
        light=lit_previous_sources[index];
    } else light=lit_sources[source];
    return lit_prepare_light_sample(light,uv);
}
float lit_local_target(LitLightingSurface surface,uint source,float2 uv,bool previous,
                       bool include_emitter_specular) {
    return lit_light_target(lit_evaluate_light_sample(lit_local_sample(source,uv,previous),surface,include_emitter_specular));
}
bool lit_reservoir_stream(inout LitReservoir output,inout float sum,uint source,float2 uv,
                          float weight,inout uint seed) {
    if(weight<=0)return false;
    sum+=weight;
    if(output.source==~0u||lit_random(seed)*sum<weight) {output.source=source;output.uv=uv;return true;}
    return false;
}
// This grid proposes nearby sources; it never decides visibility or discards
// distant/overflow sources. Every receiver retains the full global support.
struct LitLightDistribution { int3 base; float4 cdf_low,cdf_high; float local_weight; };
float lit_light_cell_cdf(LitLightDistribution distribution,uint index) {
    return index<4?distribution.cdf_low[index]:distribution.cdf_high[index-4];
}
float lit_light_cell_probability(LitLightDistribution distribution,uint index) {
    return lit_light_cell_cdf(distribution,index)-(index>0?lit_light_cell_cdf(distribution,index-1):0);
}
uint lit_light_cell_index(int3 cell) {
    return uint(cell.x)+lit_light_grid_counts.x*(uint(cell.y)+lit_light_grid_counts.y*uint(cell.z));
}
LitLightDistribution lit_light_distribution(float3 position,bool allow_spatial,uint local_candidates,uint candidates) {
    LitLightDistribution distribution=(LitLightDistribution)0;
    if(!allow_spatial||local_candidates==0||lit_light_grid_counts.w==0)return distribution;
    float3 grid=(position-lit_light_grid_origin.xyz)/lit_light_grid_origin.w-.5;
    // Bound before the float-to-int conversion, including extreme valid Frames.
    if(!all(isfinite(grid))||any(grid < -1)||any(grid >= float3(lit_light_grid_counts.xyz)))return distribution;
    distribution.base=int3(floor(grid));
    float3 fraction=frac(grid);
    float weights[8];float total=0;uint last=0;
    [loop] for(uint i=0;i<8;++i) {
        uint3 offset=uint3(i&1,(i>>1)&1,(i>>2)&1);
        int3 cell=distribution.base+int3(offset);
        float weight=0;
        if(all(cell>=0)&&all(cell<int3(lit_light_grid_counts.xyz))) {
            LitLightCell header=lit_light_cells[lit_light_cell_index(cell)];
            float3 axis=lerp(1-fraction,fraction,float3(offset));
            if(header.count>0)weight=axis.x*axis.y*axis.z*header.mass;
        }
        weights[i]=weight;total+=weight;if(weight>0)last=i;
    }
    if(total<=0)return distribution;
    float cumulative=0;
    [unroll] for(uint j=0;j<8;++j) {
        cumulative+=weights[j]/total;
        // Selection and evaluation use the same rounded CDF interval widths.
        float cdf=j>=last?1:saturate(cumulative);
        if(j<4)distribution.cdf_low[j]=cdf;else distribution.cdf_high[j-4]=cdf;
    }
    distribution.local_weight=float(local_candidates)/float(candidates);
    return distribution;
}
uint lit_propose_light(LitLightDistribution distribution,bool use_local,inout uint seed,out float probability) {
    float random=lit_random(seed),local_probability=0;
    uint source;
    if(use_local) {
        float cell_random=random;
        uint cell_slot=0;
        [unroll] for(uint i=0;i<7;++i)if(cell_random>=lit_light_cell_cdf(distribution,i))cell_slot=i+1;
        int3 cell=distribution.base+int3(cell_slot&1,(cell_slot>>1)&1,(cell_slot>>2)&1);
        LitLightCell header=lit_light_cells[lit_light_cell_index(cell)];
        uint bucket=min(uint(lit_random(seed)*header.count),header.count-1);
        LitSpatialLightProposal proposal=lit_spatial_light_proposals[header.offset+bucket];
        uint selected=lit_random(seed)<proposal.accept?bucket:proposal.alias;
        LitSpatialLightProposal entry=lit_spatial_light_proposals[header.offset+selected];
        source=entry.source;
        local_probability=lit_light_cell_probability(distribution,cell_slot)*entry.probability;
    } else {
        uint bucket=min(uint(random*lit_source_counts.x),lit_source_counts.x-1);
        LitLightProposal proposal=lit_light_proposals[bucket];
        source=lit_random(seed)<proposal.accept?bucket:proposal.alias;
        if(distribution.local_weight>0) {
            uint packed=lit_spatial_source_slots[source];
            if(packed!=~0u) {
                uint index=packed>>5;
                // LightFrame's fixed 16x8x16 layout; no integer division in the candidate loop.
                int3 cell=int3(index&15,(index>>4)&7,index>>7);
                int3 offset=cell-distribution.base;
                if(all(offset>=0)&&all(offset<=1)) {
                    uint slot=uint(offset.x+2*offset.y+4*offset.z);
                    LitLightCell header=lit_light_cells[index];
                    local_probability=lit_light_cell_probability(distribution,slot)*
                        lit_spatial_light_proposals[header.offset+(packed&31)].probability;
                }
            }
        }
    }
    probability=(1-distribution.local_weight)*lit_light_proposals[source].probability+
        distribution.local_weight*local_probability;
    return source;
}
LitReservoir lit_local_initial(LitLightingSurface surface,bool include_emitter_specular,
                               uint candidates,bool allow_spatial,inout uint seed,out float selected_probability) {
    LitReservoir result=lit_empty_reservoir();
    selected_probability=0;
    if(lit_source_counts.x==0||candidates==0)return result;
    if(seed==0)seed=0xa3c59ac3u;
    result.confidence=float(candidates);
    float sum=0,selected_target=0;
    // Deterministic-mixture RIS reserves global candidates, even when all nearby
    // sources contribute zero. Every technique uses the same balance-heuristic
    // q, with its actual integer allocation, and the total count remains M.
    uint local_candidates=(3*candidates)/4;
    LitLightDistribution distribution=lit_light_distribution(surface.position,allow_spatial,local_candidates,candidates);
    if(distribution.local_weight==0)local_candidates=0;
    [loop] for(uint i=0;i<candidates;++i) {
        float probability;uint source=lit_propose_light(distribution,i<local_candidates,seed,probability);
        float2 uv=lit_light_random_uv(seed);
        if(probability<=0)continue;
        float target=lit_local_target(surface,source,uv,false,include_emitter_specular);
        if(lit_reservoir_stream(result,sum,source,uv,target/probability,seed)) {
            selected_target=target;selected_probability=probability;
        }
    }
    if(result.source!=~0u) {
        result.weight=(sum/float(candidates))/selected_target;
        result.target=selected_target;
    }
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
