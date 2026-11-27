#pragma once
#include "assets/assets.hpp"
#include "assets/json_format.hpp"
#include "foundation/set.hpp"
#include "foundation/string.hpp"

namespace atelier::assets::material_format {
inline constexpr std::uint32_t version = 2;

namespace detail {
inline json::Result<void> text(std::string_view value, json::Value location, bool allow_empty = true) {
    if ((!allow_empty && value.empty()) || value.size() > 4096 || value.find('\0') != value.npos)
        return std::unexpected(
            location.error(json::Fault::range, "Material keys and paths require no NUL and at most 4096 bytes."));
    return {};
}

inline json::Result<std::string_view> text_view(json::Value value, bool allow_empty = true) {
    ATELIER_TRY_BIND(result, value.string());
    ATELIER_TRY(text(*result, value, allow_empty));
    return *result;
}

inline json::Result<std::string> text(json::Value value, bool allow_empty = true) {
    ATELIER_TRY_BIND(result, text_view(value, allow_empty));
    return std::string{*result};
}

template <std::size_t N>
json::Result<void> vector(json::Builder& builder, json::Value parent, std::string_view key,
                          const math::BasicVector<float, N>& value) {
    ATELIER_TRY_BIND(array, builder.array(parent, key));
    for (std::size_t i = 0; i < N; ++i)
        ATELIER_TRY(builder.append(*array, value[i]));
    return {};
}

template <std::size_t N>
json::Result<math::BasicVector<float, N>> vector(json::Value value) {
    ATELIER_TRY_BIND(values, json_format::vector<N>(value));
    math::BasicVector<float, N> result;
    for (std::size_t i = 0; i < N; ++i)
        result[i] = (*values)[i];
    return result;
}

inline json::Result<void> binding(json::Builder& builder, json::Value target, const material::TextureBinding& value) {
    ATELIER_TRY(text(value.asset_key, target));
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
        return std::unexpected(target.error(json::Fault::range, "Unknown material texture address mode."));
    }
    ATELIER_TRY(builder.set(target, "asset_key", value.asset_key));
    builder.set_validated(target, "address", address);
    return {};
}

inline json::Result<material::TextureBinding> binding(json::Value value) {
    ATELIER_TRY(value.allowed_members({"asset_key", "address"}));
    ATELIER_TRY_BIND(key, value.at("asset_key"));
    ATELIER_TRY_BIND(asset, text(*key));
    ATELIER_TRY_BIND(address, value.string({"address"}));
    material::TextureBinding result;
    result.asset_key = std::move(*asset);
    if (*address == "repeat")
        result.address = material::Address::repeat;
    else if (*address == "clamp")
        result.address = material::Address::clamp;
    else if (*address == "mirror")
        result.address = material::Address::mirror;
    else
        return std::unexpected(value.error(json::Fault::range, "Unknown material texture address mode."));
    return result;
}

inline json::Result<void> policy(json::Builder& builder, json::Value target, const material::Policy& value) {
    if (value.coverage != material::Coverage::opaque && value.coverage != material::Coverage::mask)
        return std::unexpected(target.error(json::Fault::range, "Unknown material coverage."));
    builder.set_validated(target, "coverage", value.coverage == material::Coverage::mask ? "mask" : "opaque");
    ATELIER_TRY(builder.set(target, "double_sided", value.double_sided));
    return builder.set(target, "emission_two_sided", value.emission_two_sided);
}

inline json::Result<material::Policy> policy(json::Value value) {
    ATELIER_TRY(value.allowed_members({"coverage", "double_sided", "emission_two_sided"}));
    ATELIER_TRY_BIND(coverage, value.string({"coverage"}));
    if (*coverage != "opaque" && *coverage != "mask")
        return std::unexpected(value.error(json::Fault::range, "Unknown material coverage."));
    ATELIER_TRY_BIND(double_sided, value.boolean({"double_sided"}));
    ATELIER_TRY_BIND(emission, value.boolean({"emission_two_sided"}));
    return material::Policy{*coverage == "mask" ? material::Coverage::mask : material::Coverage::opaque, *double_sided,
                            *emission};
}

inline json::Result<void> inputs(json::Builder& builder, json::Value target,
                                 Span<const material::TextureDefault> values) {
    Set<std::string_view> paths;
    paths.reserve(values.size());
    for (const auto& value : values) {
        auto item = builder.append_object(target);
        ATELIER_TRY(text(value.path, item, false));
        if (!paths.add(value.path))
            return std::unexpected(item.error(json::Fault::range, "Duplicate material input path."));
        if (value.color != material::TextureColor::linear && value.color != material::TextureColor::srgb)
            return std::unexpected(item.error(json::Fault::range, "Unknown material texture color space."));
        ATELIER_TRY(builder.set(item, "path", value.path));
        ATELIER_TRY_BIND(texture, builder.object(item, "binding"));
        ATELIER_TRY(binding(builder, *texture, value.binding));
        builder.set_validated(item, "color", value.color == material::TextureColor::srgb ? "srgb" : "linear");
        ATELIER_TRY(vector(builder, item, "empty_value", value.empty_value));
    }
    return {};
}

inline json::Result<Array<material::TextureDefault>> inputs(json::Value values) {
    ATELIER_TRY_BIND(items, values.array());
    Array<material::TextureDefault> result;
    result.reserve(items->size());
    Set<std::string_view> paths;
    paths.reserve(items->size());
    for (auto value : *items) {
        ATELIER_TRY(value.allowed_members({"path", "binding", "color", "empty_value"}));
        ATELIER_TRY_BIND(path, value.at("path"));
        ATELIER_TRY_BIND(name, text_view(*path, false));
        if (!paths.add(*name))
            return std::unexpected(path->error(json::Fault::range, "Duplicate material input path."));
        ATELIER_TRY_BIND(texture, value.at("binding"));
        ATELIER_TRY_BIND(bound, binding(*texture));
        ATELIER_TRY_BIND(empty, value.at("empty_value"));
        ATELIER_TRY_BIND(default_value, vector<4>(*empty));
        ATELIER_TRY_BIND(color, value.string({"color"}));
        material::TextureDefault input;
        input.path = std::string{*name};
        input.binding = std::move(*bound);
        input.empty_value = *default_value;
        if (*color == "linear")
            input.color = material::TextureColor::linear;
        else if (*color == "srgb")
            input.color = material::TextureColor::srgb;
        else
            return std::unexpected(value.error(json::Fault::range, "Unknown material texture color space."));
        result.add(std::move(input));
    }
    return result;
}
}

// One sparse-author codec for assets, scenes and control payloads. Absence is
// no entry; disabled keeps its typed value; an empty texture key is a clear.
inline json::Result<void> encode_entries(json::Builder& builder, json::Value target,
                                         Span<const material::Override> entries) {
    Set<std::string_view> paths;
    paths.reserve(entries.size());
    for (const auto& entry : entries) {
        auto item = builder.append_object(target);
        ATELIER_TRY(detail::text(entry.path, item, false));
        if (!paths.add(entry.path))
            return std::unexpected(item.error(json::Fault::range, "Duplicate material override path."));
        ATELIER_TRY(builder.set(item, "path", entry.path));
        ATELIER_TRY(builder.set(item, "enabled", entry.enabled));
        const auto encoded = std::visit(
            [&](const auto& value) -> json::Result<void> {
                using T = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::same_as<T, float>) {
                    builder.set_validated(item, "type", "float");
                    return builder.set(item, "value", value);
                } else if constexpr (std::same_as<T, material::TextureBinding>) {
                    builder.set_validated(item, "type", "texture");
                    ATELIER_TRY_BIND(texture, builder.object(item, "value"));
                    return detail::binding(builder, *texture, value);
                } else {
                    builder.set_validated(item, "type",
                                          T::dimensions == 2   ? "vector2"
                                          : T::dimensions == 3 ? "vector3"
                                                               : "vector4");
                    return detail::vector(builder, item, "value", value);
                }
            },
            entry.value);
        ATELIER_TRY(encoded);
    }
    return {};
}

inline json::Result<json::Document> encode_entries(Span<const material::Override> entries) {
    json::Builder builder{json::Kind::array};
    ATELIER_TRY(encode_entries(builder, builder.root(), entries));
    return builder.finish();
}

inline json::Result<Array<material::Override>> decode_entries(json::Value entries) {
    ATELIER_TRY_BIND(items, entries.array());
    Array<material::Override> result;
    result.reserve(items->size());
    Set<std::string_view> paths;
    paths.reserve(items->size());
    for (auto item : *items) {
        ATELIER_TRY(item.allowed_members({"path", "type", "enabled", "value"}));
        ATELIER_TRY_BIND(path, item.at("path"));
        ATELIER_TRY_BIND(name, detail::text_view(*path, false));
        if (!paths.add(*name))
            return std::unexpected(path->error(json::Fault::range, "Duplicate material override path."));
        ATELIER_TRY_BIND(enabled, item.boolean({"enabled"}));
        ATELIER_TRY_BIND(type, item.string({"type"}));
        ATELIER_TRY_BIND(value, item.at("value"));
        material::Override entry;
        entry.path = std::string{*name};
        entry.enabled = *enabled;
        if (*type == "float") {
            ATELIER_TRY_BIND(number, value->f32());
            entry.value = *number;
        } else if (*type == "vector2") {
            ATELIER_TRY_BIND(vector, detail::vector<2>(*value));
            entry.value = *vector;
        } else if (*type == "vector3") {
            ATELIER_TRY_BIND(vector, detail::vector<3>(*value));
            entry.value = *vector;
        } else if (*type == "vector4") {
            ATELIER_TRY_BIND(vector, detail::vector<4>(*value));
            entry.value = *vector;
        } else if (*type == "texture") {
            ATELIER_TRY_BIND(binding, detail::binding(*value));
            entry.value = std::move(*binding);
        } else
            return std::unexpected(item.error(json::Fault::range, "Unknown material override type."));
        result.add(std::move(entry));
    }
    return result;
}

inline json::Result<void> encode(json::Builder& builder, json::Value target, const MaterialAsset& asset) {
    ATELIER_TRY(detail::text(asset.key, target));
    // The catalog key is checked but is not emitted into this author document.
    if (!StringView{asset.key}.is_valid_utf8())
        return std::unexpected(target.error(json::Fault::encoding, "Invalid UTF-8 material key."));
    ATELIER_TRY(builder.set(target, "version", version));
    if (const auto* author = std::get_if<MaterialSourceAsset>(&asset.author)) {
        ATELIER_TRY(detail::text(author->source_path, target));
        builder.set_validated(target, "role", "source");
        ATELIER_TRY(builder.set(target, "source_path", author->source_path));
        ATELIER_TRY_BIND(policy, builder.object(target, "policy"));
        ATELIER_TRY(detail::policy(builder, *policy, author->policy));
        ATELIER_TRY_BIND(inputs, builder.array(target, "inputs"));
        return detail::inputs(builder, *inputs, author->inputs);
    }
    const auto& author = std::get<MaterialInstanceAsset>(asset.author);
    ATELIER_TRY(detail::text(author.parent_key, target));
    builder.set_validated(target, "role", "instance");
    ATELIER_TRY(builder.set(target, "parent_key", author.parent_key));
    ATELIER_TRY_BIND(entries, builder.array(target, "entries"));
    return encode_entries(builder, *entries, author.entries);
}

inline json::Result<json::Document> encode(const MaterialAsset& asset) {
    json::Builder builder;
    ATELIER_TRY(encode(builder, builder.root(), asset));
    return builder.finish();
}

inline json::Result<MaterialAsset> decode(json::Value value) {
    ATELIER_TRY_BIND(format, value.u64({"version"}));
    if (*format != version)
        return std::unexpected(value.error(json::Fault::range, "Unsupported material version; expected v2."));
    ATELIER_TRY_BIND(role, value.string({"role"}));
    MaterialAsset asset;
    if (*role == "source") {
        ATELIER_TRY(value.allowed_members({"version", "role", "source_path", "policy", "inputs"}));
        ATELIER_TRY_BIND(path, value.at("source_path"));
        ATELIER_TRY_BIND(source, detail::text(*path));
        ATELIER_TRY_BIND(policy, value.at("policy"));
        ATELIER_TRY_BIND(decoded_policy, detail::policy(*policy));
        ATELIER_TRY_BIND(inputs, value.at("inputs"));
        ATELIER_TRY_BIND(decoded_inputs, detail::inputs(*inputs));
        asset.author = MaterialSourceAsset{std::move(*source), *decoded_policy, std::move(*decoded_inputs)};
    } else if (*role == "instance") {
        ATELIER_TRY(value.allowed_members({"version", "role", "parent_key", "entries"}));
        ATELIER_TRY_BIND(parent, value.at("parent_key"));
        ATELIER_TRY_BIND(key, detail::text(*parent));
        ATELIER_TRY_BIND(entries, value.at("entries"));
        ATELIER_TRY_BIND(decoded, decode_entries(*entries));
        asset.author = MaterialInstanceAsset{std::move(*key), std::move(*decoded)};
    } else
        return std::unexpected(value.error(json::Fault::range, "Unknown material role."));
    return asset;
}
}
