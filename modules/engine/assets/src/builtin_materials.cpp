#include "assets/builtin_materials.hpp"

namespace atelier::assets {
namespace {
constexpr StringView standard_path = "builtin/material/standard-pbr.source";

constexpr StringView grid_path = "builtin/material/default-grid.source";

constexpr StringView standard = R"source(

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
    material.TextureInput normal_texture;
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
    vector4 texel=sample(textures.normal_texture,uv);
    vector3 encoded=vector3(texel.x*2.0-1.0,texel.y*2.0-1.0,texel.z*2.0-1.0);
    float variance=math.clamp(1.0-math.dot(encoded,encoded),0.0,1.0)*p.normal_scale*p.normal_scale;
    result.roughness=math.sqrt(math.clamp(result.roughness*result.roughness+variance*0.25,0.0,1.0));
    vector2 orientation=sample_orientation(textures.normal_texture,uv);
    encoded.x=encoded.x*p.normal_scale*orientation.x;
    encoded.y=encoded.y*p.normal_scale*p.normal_y_sign*orientation.y;
    vector3 mapped=t*encoded.x+b*encoded.y+n*encoded.z;
    if (math.dot(mapped,mapped)>0.0000000001) { result.normal_world=unit_vector(mapped); }
    return result;
}
)source";
MaterialAsset standard_material(Utf8Text key, std::wstring name, material::Policy policy) {
    MaterialSourceAsset source;
    source.source_path = String::from_utf8_validated(standard_path);
    source.policy = policy;
    for (const StringView input :
         {StringView{"base_color"}, StringView{"roughness"}, StringView{"metallic"}, StringView{"normal"},
          StringView{"emissive"}, StringView{"opacity"}}) {
        const bool color = input == "base_color" || input == "emissive";
        source.inputs.add({String::concat(String::validated(input), "_texture"),
                           {},
                           color ? material::TextureColor::srgb : material::TextureColor::linear,
                           input == "normal" ? math::Vector4{.5f, .5f, 1, 1} : math::Vector4{1, 1, 1, 1}});
    }
    return {std::move(key).into_string(), std::move(name), std::move(source)};
}
}

Span<const MaterialAsset> material_assets() {
    static const std::array assets{
        standard_material("builtin/material/standard-opaque-double", L"Standard PBR · Opaque / Two Sided",
                          {material::Coverage::opaque, true, true}),
        standard_material("builtin/material/standard-opaque-single", L"Standard PBR · Opaque / One Sided",
                          {material::Coverage::opaque, false, false}),
        standard_material("builtin/material/standard-mask-double", L"Standard PBR · Masked / Two Sided",
                          {material::Coverage::mask, true, true}),
        standard_material("builtin/material/standard-mask-single", L"Standard PBR · Masked / One Sided",
                          {material::Coverage::mask, false, false}),
        MaterialAsset{"builtin/material/default-grid"_s, L"Default World Grid",
                      MaterialSourceAsset{String::from_utf8_validated(grid_path), material::default_world_policy, {}}}};
    return assets;
}

Optional<source::SourceFile> builtin_material_source(StringView path) {
    if (path == standard_path)
        return source::SourceFile{String::from_utf8_validated(standard_path), source::content_hash(standard),
                                  String::from_utf8_validated(standard)};
    if (path == grid_path) {
        const auto text = material::default_world_source();
        return source::SourceFile{String::from_utf8_validated(grid_path), source::content_hash(text),
                                  String::from_utf8_validated(text)};
    }
    return {};
}
}
