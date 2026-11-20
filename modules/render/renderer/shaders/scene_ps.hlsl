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

// Triangle data the pixel shader reads from the shared per-triangle record.
struct SceneFace {
    uint material_index;
    uint edge_mask;
    float3 normal; // Shading normal: the face normal in Flat shading.
    float3 geometric_normal;
    float3 eye_geometric_normal;
    float3 edge_function;
    MaterialContext center,dx,dy;
};
SceneFace scene_face(SceneOutput input, uint primitive, float3 barycentrics) {
    // Unconditional derivatives of the actual perspective triangle weights,
    // before policy/author dispatch and any discard (including editing passes).
    float2 bx=ddx(barycentrics.yz),by=ddy(barycentrics.yz);
    Face record = faces[first_triangle + primitive];
    Instance instance = instances[input.instance];
    float3 gn = record.normal.x * instance.normal_x + record.normal.y * instance.normal_y + record.normal.z * instance.normal_z;
    SceneFace face;
    MaterialTriangle tri;tri.geometric_normal=gn;
    [unroll] for(uint corner=0;corner<3;++corner) {
        Vertex v=vertices[triangle_indices[(first_triangle+primitive)*3+corner]];
        tri.position[corner]=mul(float4(v.position,1),instance.world).xyz;
        tri.normal[corner]=v.normal.x*instance.normal_x+v.normal.y*instance.normal_y+v.normal.z*instance.normal_z;
        tri.uv[corner]=v.uv;
        tri.tangent[corner]=float4(material_transform_tangent(v.tangent.xyz,instance.world),v.tangent.w*instance.tangent_sign);
    }
    face.center=material_triangle_context(tri,barycentrics.yz);
    face.dx=material_triangle_delta(tri,barycentrics.yz,bx);
    face.dy=material_triangle_delta(tri,barycentrics.yz,by);
    face.material_index = material_indices[material_offsets[input.instance] + (record.slot_edges & 0x1fffffffu)];
    face.edge_mask = (display.z & 1u) != 0 ? 7 : record.slot_edges >> 29;
    face.normal = display.x == 1 ? gn : input.normal;
    face.geometric_normal = gn;
    face.eye_geometric_normal = mul(float4(gn, 0), view).xyz;
    face.edge_function = barycentrics / input.clip_w;
    return face;
}

float4 scene_color(SceneOutput input, SceneFace face) {
    if(display.w==1 && display.x==3 && display.y!=1 && input.selected==0)discard;
    if(display.w==3 && display.y!=1 && input.selected==0)discard;
    MaterialSurface material=material_surfaces[face.material_index];
    float3 eye_direction=projection[2][3]!=0?-input.eye_position:float3(0,0,-1);
    bool backfacing = dot(face.eye_geometric_normal, eye_direction) <= 0;
    bool tint_backfaces = (display.z & 2u) != 0;
    if(material.flags.y==0 && !tint_backfaces && backfacing)discard;
    MaterialOutput result=material_shade_footprint(material,face.center,face.dx,face.dy);
    if(material.flags.x!=0&&!(result.ms_coverage>=0))discard;
    if(display.w==2)return 0;
    float4 diffuse=float4(result.ms_base_color,1);
    float3 color = 0;
    if (display.w == 0) {
        float3 normal = unit_normal(face.normal);
        if(display.x!=1&&display.x!=2)normal=unit_normal(result.ms_normal_world);
        if((material.flags.y!=0 || tint_backfaces) && backfacing)normal=-normal;
        if (display.x == 2) {
            float3 eye_normal = unit_normal(mul(normal, (float3x3)view));
            color = matcap.SampleLevel(matcap_sampler, eye_normal.xy * float2(.5, -.5) + .5, 0).rgb;
        } else {
            float light = .3 + .7 * saturate(dot(normal, normalize(float3(.4, .8, -.6))));
            color = diffuse.rgb * light;
        }
        if (tint_backfaces && backfacing) {
            float bf_light = .34 + .66 * saturate(dot(normal, normalize(float3(.4, .8, -.6))));
            color = lerp(color, float3(.06, .74, .90) * bf_light, .84);
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
    float3 edge_function = face.edge_function;
    float3 dx = ddx(edge_function), dy = ddy(edge_function);
    float3 distance = float3(edge_distance(edge_function.x, dx.x, dy.x),
                             edge_distance(edge_function.y, dx.y, dy.y),
                             edge_distance(edge_function.z, dx.z, dy.z));
    distance = float3((face.edge_mask & 1) != 0 ? distance.x : 1e8,
                      (face.edge_mask & 2) != 0 ? distance.y : 1e8,
                      (face.edge_mask & 4) != 0 ? distance.z : 1e8);
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
StudioSurface main(SceneOutput input, uint primitive : SV_PrimitiveID, float3 barycentrics : SV_Barycentrics) {
    SceneFace face = scene_face(input, primitive, barycentrics);
    StudioSurface result;result.color=scene_color(input,face);
    float3 normal=unit_normal(mul(face.normal,(float3x3)view));
    float3 eye_direction=projection[2][3]!=0?-input.eye_position:float3(0,0,-1);
    if(dot(face.eye_geometric_normal,eye_direction)<0)normal=-normal;
    result.normal=float4(normal,1);return result;
}
#else
float4 main(SceneOutput input, uint primitive : SV_PrimitiveID, float3 barycentrics : SV_Barycentrics) : SV_Target {
    return scene_color(input, scene_face(input, primitive, barycentrics));
}
#endif
