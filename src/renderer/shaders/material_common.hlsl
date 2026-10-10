struct MaterialTexture {
    float4 scale_offset;
    float4 rotation_size;
    uint4 meta;
};
struct MaterialSurface {
    float4 base_roughness;
    float4 emissive_metallic;
    float4 coverage_normal;
    uint4 flags;
    MaterialTexture inputs[6];
};
StructuredBuffer<MaterialSurface> material_surfaces;
StructuredBuffer<uint> material_indices;
StructuredBuffer<uint> material_offsets;
Texture2D<float4> material_textures[] : register(t0, space1);
SamplerState material_repeat_sampler;
SamplerState material_clamp_sampler;
SamplerState material_mirror_sampler;

float2 material_uv_vector(MaterialTexture binding,float2 uv) {
    uv*=binding.scale_offset.xy;
    float2 cs=binding.rotation_size.xy;
    return float2(cs.x*uv.x-cs.y*uv.y,cs.y*uv.x+cs.x*uv.y);
}
float4 material_sample(MaterialTexture binding,float2 uv,float2 dx,float2 dy,float lod,bool gradients) {
    if(binding.meta.w==0)return 1;
    uv=material_uv_vector(binding,uv)+binding.scale_offset.zw;
    dx=material_uv_vector(binding,dx);dy=material_uv_vector(binding,dy);
    uint index=NonUniformResourceIndex(binding.meta.x);
    if(binding.meta.z==1) {
        if(gradients)return material_textures[index].SampleGrad(material_clamp_sampler,uv,dx,dy);
        return material_textures[index].SampleLevel(material_clamp_sampler,uv,lod);
    }
    if(binding.meta.z==2) {
        if(gradients)return material_textures[index].SampleGrad(material_mirror_sampler,uv,dx,dy);
        return material_textures[index].SampleLevel(material_mirror_sampler,uv,lod);
    }
    if(gradients)return material_textures[index].SampleGrad(material_repeat_sampler,uv,dx,dy);
    return material_textures[index].SampleLevel(material_repeat_sampler,uv,lod);
}
float material_lod(MaterialTexture binding,float uv_footprint) {
    float2 size=abs(binding.scale_offset.xy)*binding.rotation_size.zw;
    return max(0,log2(max(uv_footprint*max(size.x,size.y),1)));
}
float4 material_read(MaterialSurface surface,uint input,float2 uv,float2 dx,float2 dy,float footprint,bool gradients) {
    MaterialTexture binding=surface.inputs[input];
    return material_sample(binding,uv,dx,dy,material_lod(binding,footprint),gradients);
}
bool material_covered(MaterialSurface surface,float2 uv,float2 dx,float2 dy,float footprint,bool gradients) {
    if(surface.flags.x==0)return true;
    float alpha=surface.coverage_normal.x*material_read(surface,0,uv,dx,dy,footprint,gradients).a;
    float4 mask=material_read(surface,5,uv,dx,dy,footprint,gradients);
    return alpha*mask[surface.inputs[5].meta.y]>=surface.coverage_normal.y;
}
struct MaterialSample { float3 base,emissive;float roughness,metallic; };
MaterialSample material_evaluate(MaterialSurface surface,float2 uv,float2 dx,float2 dy,float footprint,bool gradients) {
    MaterialSample result;
    result.base=surface.base_roughness.rgb*material_read(surface,0,uv,dx,dy,footprint,gradients).rgb;
    float4 roughness=material_read(surface,1,uv,dx,dy,footprint,gradients);
    float4 metallic=material_read(surface,2,uv,dx,dy,footprint,gradients);
    result.roughness=saturate(surface.base_roughness.w*roughness[surface.inputs[1].meta.y]);
    result.metallic=saturate(surface.emissive_metallic.w*metallic[surface.inputs[2].meta.y]);
    result.emissive=surface.emissive_metallic.rgb*material_read(surface,4,uv,dx,dy,footprint,gradients).rgb;
    return result;
}
float3 material_transform_tangent(float3 tangent,row_major float4x4 world) {
    float3x3 basis=(float3x3)world;
    float3 largest=max(abs(basis[0]),max(abs(basis[1]),abs(basis[2])));
    float scale=max(largest.x,max(largest.y,largest.z));
    return scale>0?mul(tangent,basis/scale):0;
}
float3 material_normal(MaterialSurface surface,float2 uv,float2 dx,float2 dy,float footprint,bool gradients,
                       float3 normal,float4 tangent,inout float roughness) {
    if(surface.inputs[3].meta.w==0||surface.coverage_normal.z==0)return normal;
    float3 t=tangent.xyz-normal*dot(normal,tangent.xyz);
    float magnitude=max(abs(t.x),max(abs(t.y),abs(t.z)));
    if(magnitude==0)return normal;
    t=normalize(t/magnitude);
    float3 b=cross(normal,t)*(tangent.w<0?-1:1);
    MaterialTexture binding=surface.inputs[3];
    // The frame follows the transformed UV chart, including mirrored tiling.
    float2 scale=binding.scale_offset.xy;
    if(all(abs(scale)>1e-8)) {
        float2 cs=binding.rotation_size.xy;
        float3 u=t*(cs.x/scale.x)-b*(cs.y/scale.y);
        float3 v=t*(cs.y/scale.x)+b*(cs.x/scale.y);
        t=normalize(u);b=normalize(cross(normal,t))*(dot(cross(u,v),normal)<0?-1:1);
    }
    float3 encoded=material_read(surface,3,uv,dx,dy,footprint,gradients).xyz*2-1;
    // Mip filtering loses normal-vector length: retain that unresolved variance
    // in specular width instead of producing high-frequency reflection glints.
    float variance=saturate(1-dot(encoded,encoded))*surface.coverage_normal.z*surface.coverage_normal.z;
    roughness=sqrt(saturate(roughness*roughness+variance*.25));
    encoded.xy*=surface.coverage_normal.z;
    encoded.y*=surface.coverage_normal.w;
    if(binding.meta.z==2) {
        float2 tiled=material_uv_vector(binding,uv)+binding.scale_offset.zw;
        encoded.xy*=float2(frac(tiled.x*.5)<.5?1:-1,frac(tiled.y*.5)<.5?1:-1);
    }
    float3 mapped=t*encoded.x+b*encoded.y+normal*encoded.z;
    return dot(mapped,mapped)>1e-10?normalize(mapped):normal;
}
