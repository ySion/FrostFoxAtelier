#pragma once
#include "assets/assets.hpp"
#include "assets/json_format.hpp"
#include "foundation/string.hpp"

namespace atelier::assets::material_format {
using json_format::Json;

inline constexpr std::uint32_t version = 2;

namespace detail {
inline void text(std::string_view value, bool allow_empty = true) {
    if ((!allow_empty && value.empty()) || value.size() > 4096 || value.find('\0') != value.npos ||
        !StringView(value).is_valid_utf8())
        throw std::runtime_error("Material keys and paths require valid UTF-8, no NUL, and at most 4096 bytes.");
}

inline std::string text(const Json& value, bool allow_empty = true) {
    auto result = value.get<std::string>();
    text(std::string_view(result), allow_empty);
    return result;
}

inline Json number(float value) {
    if (!std::isfinite(value))
        throw std::runtime_error("Material values must be finite.");
    return json_format::number(value);
}

inline float number(const Json& value) {
    if (!value.is_number())
        throw std::runtime_error("Material value requires a number.");
    const auto result = value.get<float>();
    if (!std::isfinite(result))
        throw std::runtime_error("Material values must be finite.");
    return result;
}

template <std::size_t N>
Json vector(const math::BasicVector<float, N>& value) {
    Json result = Json::array();
    for (std::size_t i = 0; i < N; ++i)
        result.push_back(number(value[i]));
    return result;
}

template <std::size_t N>
math::BasicVector<float, N> vector(const Json& value) {
    const auto values = json_format::vector<N>(value);
    math::BasicVector<float, N> result;
    for (std::size_t i = 0; i < N; ++i)
        result[i] = values[i];
    return result;
}

inline Json binding(const material::TextureBinding& value) {
    text(std::string_view(value.asset_key));
    const char* address{};
    switch (value.address) {
    case material::Address::repeat:
        address = "repeat";
        break;
    case material::Address::clamp:
        address = "clamp";
        break;
    case material::Address::mirror:
        address = "mirror";
        break;
    default:
        throw std::runtime_error("Unknown material texture address mode.");
    }
    return {{"asset_key", value.asset_key}, {"address", address}};
}

inline material::TextureBinding binding(const Json& value) {
    json_format::fields(value, {"asset_key", "address"});
    material::TextureBinding result;
    result.asset_key = text(value.at("asset_key"));
    const auto address = value.at("address").get<std::string>();
    if (address == "repeat")
        result.address = material::Address::repeat;
    else if (address == "clamp")
        result.address = material::Address::clamp;
    else if (address == "mirror")
        result.address = material::Address::mirror;
    else
        throw std::runtime_error("Unknown material texture address mode.");
    return result;
}

inline Json policy(const material::Policy& value) {
    if (value.coverage != material::Coverage::opaque && value.coverage != material::Coverage::mask)
        throw std::runtime_error("Unknown material coverage.");
    return {{"coverage", value.coverage == material::Coverage::mask ? "mask" : "opaque"},
            {"double_sided", value.double_sided},
            {"emission_two_sided", value.emission_two_sided}};
}

inline material::Policy policy(const Json& value) {
    json_format::fields(value, {"coverage", "double_sided", "emission_two_sided"});
    const auto coverage = value.at("coverage").get<std::string>();
    if (coverage != "opaque" && coverage != "mask")
        throw std::runtime_error("Unknown material coverage.");
    return {coverage == "mask" ? material::Coverage::mask : material::Coverage::opaque,
            value.at("double_sided").get<bool>(), value.at("emission_two_sided").get<bool>()};
}

inline Json inputs(std::span<const material::TextureDefault> values) {
    Json result = Json::array();
    std::unordered_set<std::string_view> paths;
    for (const auto& value : values) {
        text(std::string_view(value.path), false);
        if (!paths.insert(value.path).second)
            throw std::runtime_error("Duplicate material input path.");
        if (value.color != material::TextureColor::linear && value.color != material::TextureColor::srgb)
            throw std::runtime_error("Unknown material texture color space.");
        result.push_back({{"path", value.path},
                          {"binding", binding(value.binding)},
                          {"color", value.color == material::TextureColor::srgb ? "srgb" : "linear"},
                          {"empty_value", vector(value.empty_value)}});
    }
    return result;
}

inline std::vector<material::TextureDefault> inputs(const Json& values) {
    if (!values.is_array())
        throw std::runtime_error("Material inputs require an array.");
    std::vector<material::TextureDefault> result;
    result.reserve(values.size());
    std::unordered_set<std::string> paths;
    for (const auto& value : values) {
        json_format::fields(value, {"path", "binding", "color", "empty_value"});
        material::TextureDefault input;
        input.path = text(value.at("path"), false);
        if (!paths.insert(input.path).second)
            throw std::runtime_error("Duplicate material input path.");
        input.binding = binding(value.at("binding"));
        input.empty_value = vector<4>(value.at("empty_value"));
        const auto color = value.at("color").get<std::string>();
        if (color == "linear")
            input.color = material::TextureColor::linear;
        else if (color == "srgb")
            input.color = material::TextureColor::srgb;
        else
            throw std::runtime_error("Unknown material texture color space.");
        result.push_back(std::move(input));
    }
    return result;
}
}

// The one sparse-author codec for assets, scenes and live/control payloads.
// Absence is no entry, disabled keeps its typed value, and an empty texture key
// is an explicit clear. No declaration lookup or dormant-value pruning occurs.
inline Json encode_entries(std::span<const material::Override> entries) {
    Json result = Json::array();
    std::unordered_set<std::string_view> paths;
    for (const auto& entry : entries) {
        detail::text(std::string_view(entry.path), false);
        if (!paths.insert(entry.path).second)
            throw std::runtime_error("Duplicate material override path.");
        Json item = {{"path", entry.path}, {"enabled", entry.enabled}};
        std::visit(
            [&](const auto& value) {
                using T = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::same_as<T, float>) {
                    item["type"] = "float";
                    item["value"] = detail::number(value);
                } else if constexpr (std::same_as<T, material::TextureBinding>) {
                    item["type"] = "texture";
                    item["value"] = detail::binding(value);
                } else {
                    if constexpr (T::dimensions == 2)
                        item["type"] = "vector2";
                    else if constexpr (T::dimensions == 3)
                        item["type"] = "vector3";
                    else
                        item["type"] = "vector4";
                    item["value"] = detail::vector(value);
                }
            },
            entry.value);
        result.push_back(std::move(item));
    }
    return result;
}

inline std::vector<material::Override> decode_entries(const Json& entries) {
    if (!entries.is_array())
        throw std::runtime_error("Material overrides require an array.");
    std::vector<material::Override> result;
    result.reserve(entries.size());
    std::unordered_set<std::string> paths;
    for (const auto& item : entries) {
        json_format::fields(item, {"path", "type", "enabled", "value"});
        material::Override entry;
        entry.path = detail::text(item.at("path"), false);
        entry.enabled = item.at("enabled").get<bool>();
        if (!paths.insert(entry.path).second)
            throw std::runtime_error("Duplicate material override path.");
        const auto type = item.at("type").get<std::string>();
        const auto& value = item.at("value");
        if (type == "float")
            entry.value = detail::number(value);
        else if (type == "vector2")
            entry.value = detail::vector<2>(value);
        else if (type == "vector3")
            entry.value = detail::vector<3>(value);
        else if (type == "vector4")
            entry.value = detail::vector<4>(value);
        else if (type == "texture")
            entry.value = detail::binding(value);
        else
            throw std::runtime_error("Unknown material override type.");
        result.push_back(std::move(entry));
    }
    return result;
}

inline Json encode(const MaterialAsset& asset) {
    detail::text(std::string_view(asset.key));
    if (const auto* author = std::get_if<MaterialSourceAsset>(&asset.author)) {
        detail::text(std::string_view(author->source_path));
        return {{"version", version},
                {"role", "source"},
                {"source_path", author->source_path},
                {"policy", detail::policy(author->policy)},
                {"inputs", detail::inputs(std::span<const material::TextureDefault>(author->inputs))}};
    }
    const auto& author = std::get<MaterialInstanceAsset>(asset.author);
    detail::text(std::string_view(author.parent_key));
    return {{"version", version},
            {"role", "instance"},
            {"parent_key", author.parent_key},
            {"entries", encode_entries(author.entries)}};
}

inline MaterialAsset decode(const Json& value) {
    if (json_format::identity(value.at("version")) != version)
        throw std::runtime_error("Unsupported material version; expected v2.");
    const auto role = value.at("role").get<std::string>();
    MaterialAsset asset;
    if (role == "source") {
        json_format::fields(value, {"version", "role", "source_path", "policy", "inputs"});
        asset.author = MaterialSourceAsset{detail::text(value.at("source_path")), detail::policy(value.at("policy")),
                                           detail::inputs(value.at("inputs"))};
    } else if (role == "instance") {
        json_format::fields(value, {"version", "role", "parent_key", "entries"});
        asset.author = MaterialInstanceAsset{detail::text(value.at("parent_key")), decode_entries(value.at("entries"))};
    } else
        throw std::runtime_error("Unknown material role.");
    return asset;
}
}
