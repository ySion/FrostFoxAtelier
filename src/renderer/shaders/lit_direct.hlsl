RWStructuredBuffer<uint> lit_tile_counts_output;
RWStructuredBuffer<uint> lit_tile_indices_output;
StructuredBuffer<uint> lit_tile_counts;
StructuredBuffer<uint> lit_tile_indices;
StructuredBuffer<uint> lit_shadow_light_indices;
Texture2DArray<float> lit_shadow;
RWTexture2D<float4> lit_direct_output;
Texture2D<float4> lit_direct_source;
Texture2D<float4> lit_indirect_source;
Texture2D<float> lit_ao;
RWTexture2D<float4> lit_hdr_output;
uint lit_probe_debug;
uint lit_background;
groupshared float3 tile_minimum[256],tile_maximum[256];
[numthreads(16,16,1)]
void lit_tiles_cs(uint3 dispatch:SV_DispatchThreadID,uint3 group:SV_GroupID,uint lane:SV_GroupIndex) {
    float z=all(dispatch.xy<lit_size_frame.xy)?lit_depth[dispatch.xy]:1;
    float3 p=z<1&&lit_inside(dispatch.xy)?lit_world((dispatch.xy+.5)/lit_size_frame.xy,z):1e30.xxx;
    tile_minimum[lane]=p; tile_maximum[lane]=z<1?p:-1e30.xxx; GroupMemoryBarrierWithGroupSync();
    for(uint stride=128;stride>0;stride>>=1) {
        if(lane<stride) {tile_minimum[lane]=min(tile_minimum[lane],tile_minimum[lane+stride]);
            tile_maximum[lane]=max(tile_maximum[lane],tile_maximum[lane+stride]);}
        GroupMemoryBarrierWithGroupSync();
    }
    if(lane!=0)return; uint tile=group.y*((lit_size_frame.x+15)/16)+group.x,count=0;
    for(uint i=0;i<lit_counts.x;i++) {
        LitLight light=lit_lights[i]; float3 delta=max(tile_minimum[0]-light.position_range.xyz,0)+max(light.position_range.xyz-tile_maximum[0],0);
        float radius=light.position_range.w+max(light.right_width.w,light.up_height.w);
        if(light.meta.x==2||dot(delta,delta)<=radius*radius) {
            if(count<64)lit_tile_indices_output[tile*64+count]=i; count++;
        }
    }
    lit_tile_counts_output[tile]=count;
}
[numthreads(8,8,1)]
void lit_direct_cs(uint3 tid:SV_DispatchThreadID) {
    uint2 p=tid.xy; if(all(p<lit_size_frame.xy))lit_direct_output[p]=0;
    if(!lit_inside(p)||lit_depth[p]>=1)return;
    float2 uv=(p+.5)/lit_size_frame.xy; float3 position=lit_world(uv,lit_depth[p]);
    float4 nr=lit_normal_roughness[p],bm=lit_base_metallic[p];
    float3 n=normalize(nr.xyz),v=lit_view_direction(uv),result=0;
    uint tile=(p.y/16)*((lit_size_frame.x+15)/16)+p.x/16,count=lit_tile_counts[tile];
    uint iterations=count>64?lit_counts.x:count;
    for(uint k=0;k<iterations;k++) {
        uint index=count>64?k:lit_tile_indices[tile*64+k]; LitLight light=lit_lights[index];
        uint seed=lit_light_seed(p,index);
        uint samples=max(1,lit_counts.z); float3 sum=0;
        for(uint s=0;s<samples;s++) {
            float3 l;float distance;float3 radiance=lit_sample_light(light,position,seed,l,distance);
            if(dot(n,l)<=0||!any(radiance>0))continue;
            float visibility=1;
            if(light.meta.y!=~0u && (lit_counts.w&1)!=0) {
                // The diagnostic path integrates visibility with the same emitter
                // samples; normal rendering uses stable, per-light filtered slots.
                visibility=samples>1?(lit_visible(position,n,l,distance)?1:0):lit_shadow.Load(int4(p,light.meta.y,0));
            }
            sum+=lit_brdf(bm.rgb,nr.w,bm.w,n,v,l)*radiance*visibility;
        }
        result+=sum/samples;
    }
    lit_direct_output[p]=float4(min(result,65504.xxx),1);
}
[numthreads(8,8,1)]
void lit_compose_cs(uint3 tid:SV_DispatchThreadID) {
    uint2 p=tid.xy;if(any(p>=lit_size_frame.xy))return;
    if(!lit_inside(p)||lit_depth[p]>=1) {
        float y=saturate((p.y+.5-lit_viewport.y)/lit_viewport.w);
        float3 display_color=lit_background!=0?lerp(float3(.13,.212,.262),float3(.515,.565,.565),y):0;
        // Auxiliary background is a display policy. Invert the final filmic
        // curve so changing scene exposure preserves its established colors.
        float3 a=2.51-2.43*display_color,b=.03-.59*display_color;
        float3 scene_color=(-b+sqrt(b*b+4*a*.14*display_color))/(2*a*max(lit_controls.x,1e-6));
        lit_hdr_output[p]=float4(scene_color,1);return;
    }
    if(lit_probe_debug!=0){lit_hdr_output[p]=float4(lit_indirect_source[p].rgb,1);return;}
    float4 bm=lit_base_metallic[p]; float ao=(lit_counts.w&4)!=0?lit_ao[p]:1;
    float3 irradiance=(lit_counts.w&2)!=0?lit_indirect_source[p].rgb:lit_environment.rgb*LIT_PI;
    float3 diffuse=bm.rgb*(1-bm.w)*irradiance/LIT_PI*ao;
    lit_hdr_output[p]=float4(min(lit_direct_source[p].rgb+diffuse+lit_emissive[p].rgb,65504.xxx),1);
}
