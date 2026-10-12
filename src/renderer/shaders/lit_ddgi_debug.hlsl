// Display-only diagnostics. Never write a lighting, reflection or AA history.
uint4 lit_probe_debug_options; // field available, sphere/coverage mode, geometry kind, volume slot
float4 lit_probe_debug_controls; // radius/spacing, exposure multiplier, expected visit age, reserved

float3 ddgi_debug_status(uint status, uint contributors) {
    if (status == 0) return .18.xxx;              // DDGI disabled / no field
    if (status == 1) return float3(.025,.08,.8);  // outside configured volume
    if (status == 2) return float3(1,.38,.015);   // pending initialization/relocation
    if (status == 3) return float3(.9,.02,.015);  // inside closed geometry
    if (status == 4) return float3(.65,.015,.65); // no usable directional/visible support
    if (status == 5) return float3(.015,.35,.4);  // supported, zero irradiance
    if (status == 7) return float3(.55,.32,.02);  // no directional observations
    if (status == 8) return float3(.85,.02,.5);   // all observed candidates occluded
    return float3(.025,.25+.65*float(contributors)/8,.035);
}
float3 ddgi_debug_volume_color(uint slot) {
    if(slot==0)return float3(.02,.7,.85);
    if(slot==1)return float3(.95,.65,.04);
    if(slot==2)return float3(.55,.2,.9);
    return float3(.15,.8,.3);
}

float4 ddgi_debug_coverage_ps(float4 position : SV_Position) : SV_Target {
    uint2 pixel = uint2(position.xy);
    float depth = lit_depth.Load(int3(pixel, 0));
    if (!lit_inside(pixel) || lit_is_background_depth(depth)) discard;
    DDGIResult sample=(DDGIResult)0;
    if (lit_probe_debug_options.x != 0) {
        float2 uv = (float2(pixel) + .5) / lit_size_frame.xy;
        sample=ddgi_gather(lit_world(uv, depth), lit_safe_normal(lit_normal_roughness[pixel].xyz),
            lit_geometry_normal_at(pixel), true, false);
    }
    if(sample.coverage>0) {
        if(lit_probe_debug_options.y==1)return float4(ddgi_debug_volume_color(sample.volume)*sample.coverage,1);
        if(lit_probe_debug_options.y==2)return float4(lerp(float3(.9,.25,.02),float3(.02,.8,.12),saturate(float(sample.contributors-1)/7)),1);
        if(lit_probe_debug_options.y==3)return float4(lerp(float3(.02,.65,.8),float3(.95,.08,.02),sample.dominance),1);
    }
    return float4(ddgi_debug_status(sample.status, sample.contributors), 1);
}

struct DDGIDebugVertex {
    float4 position : SV_Position;
    float3 normal : TEXCOORD0;
    nointerpolation uint probe : TEXCOORD1;
};

float4 ddgi_debug_line(float3 from, float3 to, uint vertex) {
    float4 a = mul(float4(from,1),lit_view_projection);
    float4 b = mul(float4(to,1),lit_view_projection);
    // Clip before perspective division, including the reversed-Z near plane.
    [unroll] for (uint plane=0;plane<2;++plane) {
        float da = plane==0 ? a.w-1e-5 : a.w-a.z;
        float db = plane==0 ? b.w-1e-5 : b.w-b.z;
        if (da<0 && db<0) return float4(2,2,0,1);
        if (da<0) a=lerp(a,b,da/(da-db));
        else if (db<0) b=lerp(b,a,db/(db-da));
    }
    float2 delta = (b.xy/b.w-a.xy/a.w)*lit_viewport.zw;
    if (dot(delta,delta)<1e-8) return float4(2,2,0,1);
    float2 side = normalize(float2(-delta.y,delta.x))*2.5/lit_viewport.zw;
    static const uint corners[6]={0,1,2,2,1,3};
    uint corner=corners[vertex];
    float4 clip=corner<2?a:b;
    clip.xy+=side*((corner&1)!=0?1:-1)*clip.w;
    return clip;
}

DDGIDebugVertex ddgi_debug_vs(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
    DDGIDebugVertex result;
    DDGIVolume volume=lit_probe_volumes[lit_probe_debug_options.w];
    result.probe=volume.grid.w+instance; result.normal=float3(0,0,1);
    if (lit_probe_debug_options.z==1) {
        uint axis=instance/4, side=instance%4;
        float3 a=0, b=0;
        a[(axis+1)%3]=side&1; a[(axis+2)%3]=(side>>1)&1;
        b=a; b[axis]=1;
        float3 size=volume.spacing.xyz*float3(volume.grid.xyz-1);
        result.position=ddgi_debug_line(volume.origin.xyz+a*size,volume.origin.xyz+b*size,vertex);
    } else {
        float4 offset=0;
        if (lit_probe_debug_options.x!=0) offset=lit_probe_offsets_previous.Load(int3(ddgi_probe_texel(result.probe),0));
        if (!all(isfinite(offset))) offset=0;
        float3 base=ddgi_probe_position(result.probe,0,volume);
        if (lit_probe_debug_options.z==2) {
            result.position=ddgi_debug_line(base,base+offset.xyz,vertex);
        } else {
            // Small procedural spheres; native clipping also handles a camera inside the volume.
            static const uint2 corners[6]={uint2(0,0),uint2(1,0),uint2(0,1),uint2(0,1),uint2(1,0),uint2(1,1)};
            uint cell=vertex/6; uint2 corner=corners[vertex%6];
            float azimuth=float(cell%12+corner.x)*(2*LIT_PI/12);
            float polar=float(cell/12+corner.y)*(LIT_PI/8);
            result.normal=float3(sin(polar)*cos(azimuth),cos(polar),sin(polar)*sin(azimuth));
            float radius=ddgi_minimum_spacing(volume)*lit_probe_debug_controls.x;
            result.position=mul(float4(base+offset.xyz+result.normal*radius,1),lit_view_projection);
        }
    }
    return result;
}

float4 ddgi_debug_ps(DDGIDebugVertex input) : SV_Target {
    DDGIVolume volume=lit_probe_volumes[lit_probe_debug_options.w];
    if (lit_probe_debug_options.z!=0)
        return float4(lit_probe_debug_options.x==0?.18.xxx:
            (lit_probe_debug_options.z==1?ddgi_debug_volume_color(lit_probe_debug_options.w):float3(.65,.08,.85)),1);
    if (lit_probe_debug_options.x==0) return float4(.18,.18,.18,1);
    uint2 texel=ddgi_probe_texel(input.probe);
    float4 offset=lit_probe_offsets_previous.Load(int3(texel,0));
    uint mode=lit_probe_debug_options.y;
    if (mode==3) {
        uint age=lit_probe_age.Load(int3(texel,0));
        if (age==0xffffffff) return float4(.18,.18,.18,1);
        if (age==0) return float4(.02,.85,.8,1);
        float t=saturate(float(age)/max(lit_probe_debug_controls.z,1));
        return float4(lerp(float3(.1,.65,.03),float3(.9,.05,.25),t),1);
    }
    uint status=!all(isfinite(offset))?2:(offset.w<-.5?3:(offset.w<.5?2:6));
    if (mode==2 || status!=6) return float4(ddgi_debug_status(status,8),1);
    float3 direction=normalize(input.normal);
    float4 irradiance=ddgi_sample_tile(lit_probe_irradiance_previous,input.probe,direction);
    if (irradiance.w<=0 || !all(isfinite(irradiance))) return float4(ddgi_debug_status(2,0),1);
    if (mode==4) {
        // Show the cached directional mean in grid-spacing units (black 0, white >= 4).
        float2 location=ddgi_octahedral_encode(direction)*DDGI_TILE_SIZE-.5;
        int2 lower=int2(floor(location)), tile=int2(texel*DDGI_TILE_SIZE);
        float2 fraction=frac(location);
        float a=lit_probe_distance_previous.Load(int3(tile+ddgi_fold_texel(lower),0)).x;
        float b=lit_probe_distance_previous.Load(int3(tile+ddgi_fold_texel(lower+int2(1,0)),0)).x;
        float c=lit_probe_distance_previous.Load(int3(tile+ddgi_fold_texel(lower+int2(0,1)),0)).x;
        float d=lit_probe_distance_previous.Load(int3(tile+ddgi_fold_texel(lower+int2(1,1)),0)).x;
        float distance=lerp(lerp(a,b,fraction.x),lerp(c,d,fraction.x),fraction.y)*volume.origin.w;
        float value=saturate(distance/(4*ddgi_minimum_spacing(volume)));
        return float4(value.xxx,1);
    }
    float3 color=max(irradiance.rgb,0)*(lit_probe_debug_controls.y/LIT_PI);
    return float4(color/(1+color),1);
}
