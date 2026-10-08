StructuredBuffer<uint> lit_shadow_light_indices;
RWTexture2DArray<float> lit_shadow_output;
Texture2DArray<float> lit_shadow_raw;
Texture2DArray<float> lit_shadow_previous;
Texture2D<float> lit_history_depth;
Texture2D<float4> lit_history_normal;
[numthreads(8,8,1)]
void lit_shadow_trace_cs(uint3 tid:SV_DispatchThreadID) {
    if(any(tid.xy>=lit_size_frame.xy)||tid.z>=lit_counts.y)return;
    lit_shadow_output[tid]=1;
    if(!lit_inside(tid.xy)||lit_depth[tid.xy]>=1)return;
    float2 uv=(tid.xy+.5)/lit_size_frame.xy;float3 p=lit_world(uv,lit_depth[tid.xy]),n=lit_normal_roughness[tid.xy].xyz;
    uint index=lit_shadow_light_indices[tid.z],seed=lit_light_seed(tid.xy,index);
    float sum=0;for(uint sample=0;sample<lit_counts.z;sample++) {
        float3 l;float distance;float3 radiance=lit_sample_light(lit_lights[index],p,seed,l,distance);
        sum+=dot(n,l)>0&&any(radiance>0)?(lit_visible(p,n,l,distance)?1:0):1;
    }
    lit_shadow_output[tid]=sum/max(1,lit_counts.z);
}
[numthreads(8,8,1)]
void lit_shadow_filter_cs(uint3 tid:SV_DispatchThreadID) {
    if(any(tid.xy>=lit_size_frame.xy)||tid.z>=lit_counts.y)return;
    float z=lit_depth[tid.xy];float3 n=lit_normal_roughness[tid.xy].xyz;float sum=0,weight=0;
    for(int y=-1;y<=1;y++)for(int x=-1;x<=1;x++) {
        int2 q=int2(tid.xy)+int2(x,y);if(any(q<0)||any(q>=int2(lit_size_frame.xy)))continue;
        float w=exp(-abs(lit_depth[q]-z)*500)*pow(saturate(dot(n,lit_normal_roughness[q].xyz)),32);
        sum+=lit_shadow_raw.Load(int4(q,tid.z,0))*w;weight+=w;
    }
    float current=weight>0?sum/weight:lit_shadow_raw.Load(int4(tid.xy,tid.z,0));
    float4 motion=lit_motion[tid.xy];float2 previous_uv=(tid.xy+.5)/lit_size_frame.xy+motion.xy;
    if(lit_size_frame.w!=0&&motion.w>.5&&all(previous_uv>0)&&all(previous_uv<1)) {
        float old_z=lit_history_depth.SampleLevel(lit_linear_sampler,previous_uv,0);
        float3 old_n=lit_history_normal.SampleLevel(lit_linear_sampler,previous_uv,0).xyz;
        if(abs(old_z-lit_previous_device_depth(tid.xy,motion))<.002&&dot(old_n,n)>.95) {
            float history=lit_shadow_previous.SampleLevel(lit_linear_sampler,float3(previous_uv,tid.z),0);
            current=lerp(current,clamp(history,current-.25,current+.25),lit_temporal.x*(1-lit_temporal.z));
        }
    }
    lit_shadow_output[tid]=current;
}
