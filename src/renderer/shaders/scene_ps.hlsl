Texture2D<float4> matcap;
SamplerState matcap_sampler;

float edge_distance(float value, float dx, float dy) {
    // Normalize before squaring: homogeneous edge gradients scale with 1/w.
    // A constant edge function describes no finite screen-space boundary.
    float scale = max(abs(dx), abs(dy));
    if (scale == 0) return 1e8;
    float2 gradient = float2(dx, dy) / scale;
    return abs(value / scale) * rsqrt(dot(gradient, gradient));
}

float4 scene_color(SceneOutput input) {
    if(display.w==1 && display.x==3 && display.y!=1 && input.selected==0)discard;
    if(display.w==3 && display.y!=1 && input.selected==0)discard;
    MaterialSurface material=material_surfaces[input.material_index];
    float3 eye_direction=projection[2][3]!=0?-input.eye_position:float3(0,0,-1);
    if(material.flags.y==0&&dot(input.eye_geometric_normal,eye_direction)<=0)discard;
    float2 uv_dx=ddx(input.uv),uv_dy=ddy(input.uv);
    if(!material_covered(material,input.uv,uv_dx,uv_dy,0,true))discard;
    if(display.w==2)return 0;
    float4 diffuse=float4(material.base_roughness.rgb*material_read(material,0,input.uv,uv_dx,uv_dy,0,true).rgb,1);
    float3 color = 0;
    if (display.w == 0) {
        float3 normal = unit_normal(input.normal);
        if(display.x!=2) {
            float roughness=material.base_roughness.w;
            normal=material_normal(material,input.uv,uv_dx,uv_dy,0,true,normal,input.tangent,roughness);
        }
        if(material.flags.y!=0&&dot(input.eye_geometric_normal,eye_direction)<0)normal=-normal;
        if (display.x == 2) {
            float3 eye_normal = unit_normal(mul(normal, (float3x3)view));
            color = matcap.SampleLevel(matcap_sampler, eye_normal.xy * float2(.5, -.5) + .5, 0).rgb;
        } else {
            float light = .3 + .7 * saturate(dot(normal, normalize(float3(.4, .8, -.6))));
            color = diffuse.rgb * light;
        }
#ifdef CAVITY_SURFACE
        return float4(color,1); // Editing ink is composed after the cavity pass.
#else
        if (display.y != 1 && input.selected == 0) return float4(color, 1);
#endif
    }
    // Dividing perspective barycentrics by the interpolated clip w recovers
    // affine edge functions, including vertices on/behind w=0. Raw screen
    // barycentrics can be negative there and turn an entire face into a line.
    float3 edge_function = input.edge_coordinates.xyz / input.edge_coordinates.w;
    float3 dx = ddx(edge_function), dy = ddy(edge_function);
    float3 distance = float3(edge_distance(edge_function.x, dx.x, dy.x),
                             edge_distance(edge_function.y, dx.y, dy.y),
                             edge_distance(edge_function.z, dx.z, dy.z));
    distance = float3((input.edge_mask & 1) != 0 ? distance.x : 1e8,
                      (input.edge_mask & 2) != 0 ? distance.y : 1e8,
                      (input.edge_mask & 4) != 0 ? distance.z : 1e8);
    float width = input.selected != 0 ? max(wire_style.x, 2.0) : wire_style.x;
    float edge = 1 - smoothstep(max(0, width * .5 - .5), width * .5 + .5,
                               min(distance.x, min(distance.y, distance.z)));
    float3 ink = input.selected != 0 ? float3(1, .69, 0) :
                  display.w == 1 ? float3(.48, .55, .62) : float3(.025, .032, .04);
    float alpha = edge * (input.selected != 0 || display.w == 1 ? 1 : wire_style.y);
    if (display.w == 1 || display.w == 3) {
        if (alpha <= 0) discard;
        return float4(ink * alpha, alpha);
    }
    if (display.y == 1 || input.selected != 0) color = lerp(color, ink, alpha);
    return float4(color, 1);
}

#ifdef CAVITY_SURFACE
struct StudioSurface { float4 color:SV_Target0; float4 normal:SV_Target1; };
StudioSurface main(SceneOutput input) {
    StudioSurface result;result.color=scene_color(input);
    float3 normal=unit_normal(mul(input.normal,(float3x3)view));
    float3 eye_direction=projection[2][3]!=0?-input.eye_position:float3(0,0,-1);
    if(dot(input.eye_geometric_normal,eye_direction)<0)normal=-normal;
    result.normal=float4(normal,1);return result;
}
#else
float4 main(SceneOutput input):SV_Target {return scene_color(input);}
#endif
