#include "assets/builtin_materials.hpp"

namespace atelier::assets {
namespace {
constexpr std::string_view standard_path = "builtin/material/standard-pbr.source";

constexpr std::string_view openpbr_path = "builtin/material/open-pbr.source";

constexpr std::string_view grid_path = "builtin/material/default-grid.source";

constexpr std::string_view standard = R"source(

struct UvTransform { vector2 scale; vector2 offset; float rotation; };
struct Params {
    vector4 base_color;
    float roughness; float metallic; float normal_scale; float normal_y_sign;
    vector3 emissive; float alpha_cutoff;
    UvTransform base_color_uv; UvTransform roughness_uv; UvTransform metallic_uv;
    UvTransform normal_uv; UvTransform emissive_uv; UvTransform opacity_uv;
    float roughness_channel; float metallic_channel; float opacity_channel;
};
struct Textures {
    material.TextureInput base_color_texture;
    material.TextureInput roughness_texture;
    material.TextureInput metallic_texture;
    material.NormalTextureInput normal_texture;
    material.TextureInput emissive_texture;
    material.TextureInput opacity_texture;
};
UvTransform identity_uv() {
    UvTransform result; result.scale=vector2(1.0,1.0); return result;
}
Params defaults() {
    Params p;
    p.base_color=vector4(1.0,1.0,1.0,1.0);
    p.roughness=0.65; p.normal_scale=1.0; p.normal_y_sign=1.0; p.alpha_cutoff=0.5;
    p.base_color_uv=identity_uv(); p.roughness_uv=identity_uv(); p.metallic_uv=identity_uv();
    p.normal_uv=identity_uv(); p.emissive_uv=identity_uv(); p.opacity_uv=identity_uv();
    return p;
}
vector2 transform_uv(vector2 uv,UvTransform t) {
    vector2 scaled=vector2(uv.x*t.scale.x,uv.y*t.scale.y);
    float c=math.cos(t.rotation); float s=math.sin(t.rotation);
    return vector2(c*scaled.x-s*scaled.y,s*scaled.x+c*scaled.y)+t.offset;
}
float channel(vector4 value,float selected) {
    if (selected<0.5) { return value.x; }
    if (selected<1.5) { return value.y; }
    if (selected<2.5) { return value.z; }
    return value.w;
}
vector3 unit_vector(vector3 value) {
    float scale=math.max(math.abs(value.x),math.max(math.abs(value.y),math.abs(value.z)));
    if (scale==0.0) { return vector3(0.0,0.0,0.0); }
    vector3 scaled=value/scale;
    return scaled/math.sqrt(math.dot(scaled,scaled));
}
material.Output shade(material.Context context,Params p,Textures textures) {
    material.Output result;
    vector4 base=sample(textures.base_color_texture,transform_uv(context.uv,p.base_color_uv));
    vector4 rough=sample(textures.roughness_texture,transform_uv(context.uv,p.roughness_uv));
    vector4 metal=sample(textures.metallic_texture,transform_uv(context.uv,p.metallic_uv));
    vector4 emission=sample(textures.emissive_texture,transform_uv(context.uv,p.emissive_uv));
    vector4 alpha=sample(textures.opacity_texture,transform_uv(context.uv,p.opacity_uv));
    result.base_color=vector3(base.x*p.base_color.x,base.y*p.base_color.y,base.z*p.base_color.z);
    result.emissive=vector3(emission.x*p.emissive.x,emission.y*p.emissive.y,emission.z*p.emissive.z);
    result.roughness=math.clamp(p.roughness*channel(rough,p.roughness_channel),0.0,1.0);
    result.metallic=math.clamp(p.metallic*channel(metal,p.metallic_channel),0.0,1.0);
    result.coverage=p.base_color.w*base.w*channel(alpha,p.opacity_channel)-p.alpha_cutoff;
    vector3 n=context.normal_world;
    result.normal_world=n;
    if (p.normal_scale==0.0) { return result; }
    vector3 tangent=vector3(context.tangent_world.x,context.tangent_world.y,context.tangent_world.z);
    vector3 t=unit_vector(tangent-n*math.dot(n,tangent));
    if (math.dot(t,t)==0.0) { return result; }
    float handedness=1.0;
    if (context.tangent_world.w<0.0) { handedness=-1.0; }
    vector3 b=math.cross(n,t)*handedness;
    if (math.abs(p.normal_uv.scale.x)>0.00000001 && math.abs(p.normal_uv.scale.y)>0.00000001) {
        float c=math.cos(p.normal_uv.rotation); float s=math.sin(p.normal_uv.rotation);
        vector3 u=t*(c/p.normal_uv.scale.x)-b*(s/p.normal_uv.scale.y);
        vector3 v=t*(s/p.normal_uv.scale.x)+b*(c/p.normal_uv.scale.y);
        t=unit_vector(u);
        float sign=1.0;
        if (math.dot(math.cross(u,v),n)<0.0) { sign=-1.0; }
        b=unit_vector(math.cross(n,t))*sign;
    }
    vector2 uv=transform_uv(context.uv,p.normal_uv);
    vector3 n_ts=sample_normal(textures.normal_texture,uv);
    float mx=n_ts.x*p.normal_scale;
    float my=n_ts.y*p.normal_scale*p.normal_y_sign;
    vector3 mapped=t*mx+b*my+n*n_ts.z;
    if (math.dot(mapped,mapped)>0.0000000001) { result.normal_world=unit_vector(mapped); }
    return result;
}
)source";

constexpr std::string_view openpbr = R"source(
struct UvTransform { vector2 scale; vector2 offset; float rotation; };
struct Params {
    vector4 base_color;
    float roughness; float metallic; float diffuse_roughness;
    float specular_weight; float specular_ior; vector3 specular_color;
    float micro_ao; float normal_scale; float normal_y_sign; float alpha_cutoff;
    vector3 emissive;
    float coat_weight; float coat_roughness; float coat_darkening;
    float fuzz_weight; float fuzz_roughness; vector3 fuzz_color;
    float transmission_scatter; vector3 transmission_color;
    float thin_film_weight; float thin_film_thickness; float thin_film_ior;
    float subsurface_weight; float subsurface_radius; vector3 subsurface_color;
    UvTransform uv;
};
struct Textures {
    material.TextureInput base_color_texture;
    material.TextureInput roughness_texture;
    material.TextureInput metallic_texture;
    material.NormalTextureInput normal_texture;
    material.TextureInput emissive_texture;
    material.TextureInput occlusion_texture;
    material.TextureInput opacity_texture;
};
Params defaults() {
    Params p;
    p.base_color=vector4(1.0,1.0,1.0,1.0);
    p.roughness=0.5; p.metallic=0.0; p.diffuse_roughness=0.0;
    p.specular_weight=1.0; p.specular_ior=1.5; p.specular_color=vector3(1.0,1.0,1.0);
    p.micro_ao=1.0; p.normal_scale=1.0; p.normal_y_sign=1.0; p.alpha_cutoff=0.5;
    p.coat_roughness=0.1; p.coat_darkening=1.0;
    p.fuzz_roughness=0.5; p.fuzz_color=vector3(1.0,1.0,1.0);
    p.transmission_color=vector3(0.45,0.75,0.25);
    p.thin_film_thickness=0.5; p.thin_film_ior=1.33;
    p.subsurface_color=vector3(1.0,0.35,0.2);
    p.uv.scale=vector2(1.0,1.0);
    return p;
}
vector2 transform_uv(vector2 uv,UvTransform t) {
    vector2 scaled=vector2(uv.x*t.scale.x,uv.y*t.scale.y);
    float c=math.cos(t.rotation); float s=math.sin(t.rotation);
    return vector2(c*scaled.x-s*scaled.y,s*scaled.x+c*scaled.y)+t.offset;
}
vector3 unit_vector(vector3 value) {
    float scale=math.max(math.abs(value.x),math.max(math.abs(value.y),math.abs(value.z)));
    if (scale==0.0) { return vector3(0.0,0.0,0.0); }
    vector3 scaled=value/scale;
    return scaled/math.sqrt(math.dot(scaled,scaled));
}
material.Output shade(material.Context context,Params p,Textures textures) {
    material.Output o;
    vector2 uv=transform_uv(context.uv,p.uv);
    vector4 base=sample(textures.base_color_texture,uv);
    vector4 rough=sample(textures.roughness_texture,uv);
    vector4 metal=sample(textures.metallic_texture,uv);
    vector4 emission=sample(textures.emissive_texture,uv);
    vector4 occ=sample(textures.occlusion_texture,uv);
    vector4 alpha=sample(textures.opacity_texture,uv);
    o.base_color=vector3(base.x*p.base_color.x,base.y*p.base_color.y,base.z*p.base_color.z);
    o.emissive=vector3(emission.x*p.emissive.x,emission.y*p.emissive.y,emission.z*p.emissive.z);
    o.roughness=math.clamp(p.roughness*rough.y,0.0,1.0);
    o.metallic=math.clamp(p.metallic*metal.z,0.0,1.0);
    o.diffuse_roughness=math.clamp(p.diffuse_roughness,0.0,1.0);
    o.specular_weight=math.clamp(p.specular_weight,0.0,1.0);
    o.specular_ior=math.clamp(p.specular_ior,1.0,3.0);
    o.specular_color=math.clamp(p.specular_color,vector3(0.0,0.0,0.0),vector3(1.0,1.0,1.0));
    o.micro_ao=math.clamp(p.micro_ao*occ.x,0.0,1.0);
    o.coverage=p.base_color.w*base.w*alpha.x-p.alpha_cutoff;
    o.coat_weight=math.clamp(p.coat_weight,0.0,1.0);
    o.coat_roughness=math.clamp(p.coat_roughness,0.0,1.0);
    o.coat_darkening=math.clamp(p.coat_darkening,0.0,1.0);
    o.fuzz_weight=math.clamp(p.fuzz_weight,0.0,1.0);
    o.fuzz_roughness=math.clamp(p.fuzz_roughness,0.0,1.0);
    o.fuzz_color=math.clamp(p.fuzz_color,vector3(0.0,0.0,0.0),vector3(1.0,1.0,1.0));
    o.transmission_scatter=math.clamp(p.transmission_scatter,0.0,1.0);
    o.transmission_color=math.clamp(p.transmission_color,vector3(0.0,0.0,0.0),vector3(1.0,1.0,1.0));
    o.thin_film_weight=math.clamp(p.thin_film_weight,0.0,1.0);
    o.thin_film_thickness=math.clamp(p.thin_film_thickness,0.0,1.0);
    o.thin_film_ior=math.clamp(p.thin_film_ior,1.0,2.5);
    o.subsurface_weight=math.clamp(p.subsurface_weight,0.0,1.0);
    o.subsurface_radius=math.clamp(p.subsurface_radius,0.0,0.05);
    o.subsurface_color=math.clamp(p.subsurface_color,vector3(0.0,0.0,0.0),vector3(1.0,1.0,1.0));
    vector3 n=context.normal_world;
    o.normal_world=n;
    if (p.normal_scale==0.0) { return o; }
    vector3 tangent=vector3(context.tangent_world.x,context.tangent_world.y,context.tangent_world.z);
    vector3 t=unit_vector(tangent-n*math.dot(n,tangent));
    if (math.dot(t,t)==0.0) { return o; }
    float handedness=1.0;
    if (context.tangent_world.w<0.0) { handedness=-1.0; }
    vector3 b=math.cross(n,t)*handedness;
    if (math.abs(p.uv.scale.x)>0.00000001 && math.abs(p.uv.scale.y)>0.00000001) {
        float c=math.cos(p.uv.rotation); float s=math.sin(p.uv.rotation);
        vector3 u=t*(c/p.uv.scale.x)-b*(s/p.uv.scale.y);
        vector3 v=t*(s/p.uv.scale.x)+b*(c/p.uv.scale.y);
        t=unit_vector(u);
        float sign=1.0;
        if (math.dot(math.cross(u,v),n)<0.0) { sign=-1.0; }
        b=unit_vector(math.cross(n,t))*sign;
    }
    vector3 n_ts=sample_normal(textures.normal_texture,uv);
    vector3 mapped=t*(n_ts.x*p.normal_scale)+b*(n_ts.y*p.normal_scale*p.normal_y_sign)+n*n_ts.z;
    if (math.dot(mapped,mapped)>0.0000000001) { o.normal_world=unit_vector(mapped); }
    return o;
}
)source";

MaterialAsset standard_material(std::string key, std::wstring name, material::Policy policy) {
    MaterialSourceAsset source;
    source.source_path = standard_path;
    source.policy = policy;
    for (const std::string_view input : {"base_color", "roughness", "metallic", "normal", "emissive", "opacity"}) {
        const bool color = input == "base_color" || input == "emissive";
        source.inputs.push_back({std::string(input) + "_texture",
                                 {},
                                 color ? material::TextureColor::srgb : material::TextureColor::linear,
                                 input == "normal" ? math::Vector4{.5f, .5f, 1, 1} : math::Vector4{1, 1, 1, 1}});
    }
    return {std::move(key), std::move(name), std::move(source)};
}

MaterialAsset openpbr_material(std::string key, std::wstring name, material::Policy policy) {
    MaterialSourceAsset source;
    source.source_path = openpbr_path;
    source.policy = policy;
    for (const std::string_view input :
         {"base_color", "roughness", "metallic", "normal", "emissive", "occlusion", "opacity"}) {
        const bool color = input == "base_color" || input == "emissive";
        source.inputs.push_back({std::string(input) + "_texture",
                                 {},
                                 color ? material::TextureColor::srgb : material::TextureColor::linear,
                                 input == "normal" ? math::Vector4{.5f, .5f, 1, 1} : math::Vector4{1, 1, 1, 1}});
    }
    return {std::move(key), std::move(name), std::move(source)};
}
} // namespace

std::span<const MaterialAsset> material_assets() {
    static const std::array assets{
        standard_material("builtin/material/standard-opaque-double", L"Standard PBR · Opaque / Two Sided",
                          {material::Coverage::opaque, true, true}),
        standard_material("builtin/material/standard-opaque-single", L"Standard PBR · Opaque / One Sided",
                          {material::Coverage::opaque, false, false}),
        standard_material("builtin/material/standard-mask-double", L"Standard PBR · Masked / Two Sided",
                          {material::Coverage::mask, true, true}),
        standard_material("builtin/material/standard-mask-single", L"Standard PBR · Masked / One Sided",
                          {material::Coverage::mask, false, false}),
        MaterialAsset{"builtin/material/default-grid", L"Default World Grid",
                      MaterialSourceAsset{std::string(grid_path), material::default_world_policy, {}}},
        openpbr_material("builtin/material/open-pbr-opaque-double", L"OpenPBR v1.1 · Opaque / Two Sided",
                         {material::Coverage::opaque, true, true}),
        openpbr_material("builtin/material/open-pbr-opaque-single", L"OpenPBR v1.1 · Opaque / One Sided",
                         {material::Coverage::opaque, false, false}),
        openpbr_material("builtin/material/open-pbr-mask-double", L"OpenPBR v1.1 · Masked / Two Sided",
                         {material::Coverage::mask, true, true}),
        openpbr_material("builtin/material/open-pbr-mask-single", L"OpenPBR v1.1 · Masked / One Sided",
                         {material::Coverage::mask, false, false})};
    return assets;
}

std::optional<source::SourceFile> builtin_material_source(std::string_view path) {
    if (path == standard_path)
        return source::SourceFile{std::string(path), source::content_hash(standard), std::string(standard)};
    if (path == openpbr_path)
        return source::SourceFile{std::string(path), source::content_hash(openpbr), std::string(openpbr)};
    if (path == grid_path) {
        const auto text = material::default_world_source();
        return source::SourceFile{std::string(path), source::content_hash(text), std::string(text)};
    }
    return std::nullopt;
}
} // namespace atelier::assets
