#pragma once
#include "assets/assets.hpp"
#include "assets/json_format.hpp"

namespace atelier::assets::material_format {
using json_format::Json;
inline constexpr std::array names{"base_color","roughness","metallic","normal","emissive","opacity"};
inline Json encode(const MaterialAsset& asset) {
    const auto& p=asset.description.parameters;
    Json textures=Json::object();
    for(std::size_t i=0;i<names.size();++i) {
        const auto& t=asset.description.textures[i];
        if(t.image && asset.textures[i].empty()) throw std::runtime_error("Save the texture asset before referencing it from a material.");
        textures[names[i]]={{"source",asset.textures[i]},{"scale",json_format::numbers(t.scale)},{"offset",json_format::numbers(t.offset)},
            {"rotation",json_format::number(t.rotation)},{"address",static_cast<unsigned>(t.address)},{"channel",t.channel}};
    }
    using json_format::number;
    return {{"version",1},{"base_color",json_format::numbers(p.base_color)},{"roughness",number(p.roughness)},{"metallic",number(p.metallic)},
        {"normal_scale",number(p.normal_scale)},{"normal_y_sign",number(p.normal_y_sign)},{"emissive",json_format::numbers(p.emissive)},
        {"coverage",p.coverage==material::Coverage::mask?"mask":"opaque"},{"alpha_cutoff",number(p.alpha_cutoff)},
        {"double_sided",p.double_sided},{"emission_two_sided",p.emission_two_sided},{"textures",std::move(textures)}};
}
inline MaterialAsset decode(const Json& value) {
    json_format::fields(value,{"version","base_color","roughness","metallic","normal_scale","normal_y_sign",
        "emissive","coverage","alpha_cutoff","double_sided","emission_two_sided","textures"});
    if(json_format::identity(value.at("version"))!=1) throw std::runtime_error("Unsupported material version.");
    MaterialAsset asset;auto& p=asset.description.parameters;
    p.base_color=json_format::vector<4>(value.at("base_color"));p.emissive=json_format::vector<3>(value.at("emissive"));
    const auto number=[&](const char* name) {const auto& v=value.at(name);if(!v.is_number()) throw std::runtime_error("Material field requires a number.");return v.get<float>();};
    p.roughness=number("roughness");p.metallic=number("metallic");p.normal_scale=number("normal_scale");p.normal_y_sign=number("normal_y_sign");
    p.alpha_cutoff=number("alpha_cutoff");p.double_sided=value.at("double_sided").get<bool>();p.emission_two_sided=value.at("emission_two_sided").get<bool>();
    const auto coverage=value.at("coverage").get<std::string>();
    if(coverage!="opaque" && coverage!="mask") throw std::runtime_error("Unknown material coverage.");
    p.coverage=coverage=="mask"?material::Coverage::mask:material::Coverage::opaque;
    const auto& textures=value.at("textures");json_format::fields(textures,{"base_color","roughness","metallic","normal","emissive","opacity"});
    for(std::size_t i=0;i<names.size();++i) {
        const auto& item=textures.at(names[i]);json_format::fields(item,{"source","scale","offset","rotation","address","channel"});
        auto& t=asset.description.textures[i];asset.textures[i]=item.at("source").get<std::string>();
        t.scale=json_format::vector<2>(item.at("scale"));t.offset=json_format::vector<2>(item.at("offset"));
        if(!item.at("rotation").is_number()) throw std::runtime_error("Material UV rotation requires a number.");
        t.rotation=item.at("rotation").get<float>();
        const auto address=json_format::identity(item.at("address")),channel=json_format::identity(item.at("channel"));
        if(address>2 || channel>3) throw std::runtime_error("Invalid texture sampler.");
        t.address=static_cast<material::Address>(address);t.channel=static_cast<std::uint32_t>(channel);
    }
    static_cast<void>(material::Surface(asset.description));return asset;
}
}
