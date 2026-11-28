#pragma once
#include "json/json.hpp"
#include <array>

// File policy and sparse-author semantics shared by Project, Scene and graphs.
namespace atelier::assets::json_format {
inline constexpr std::size_t maximum_bytes = 16 * 1024 * 1024;

// Persistent JSON-byte digests and bundle manifests use this same declaration.
inline constexpr std::uint32_t json_bytes_version = 2;

inline json::Result<json::Document> parse(StringView text) {
    if (text.size() > maximum_bytes)
        return std::unexpected(
            json::Error{json::Fault::range, String::literal<"JSON exceeds the 16 MiB file limit.">(), {}, {}});
    return json::Document::parse(text, {32, json::DepthPolicy::containers, true});
}

inline json::Result<json::Document> parse(const String& text) {
    return parse(text.view());
}

inline json::Result<json::Document> parse(Span<const std::byte> bytes) {
    return parse(as_chars(bytes));
}

template <std::size_t Count>
json::Result<std::array<float, Count>> vector(json::Value value) {
    ATELIER_TRY_BIND(items, value.array());
    if (items->size() != Count)
        return std::unexpected(value.error(json::Fault::range, "Invalid vector length."));
    std::array<float, Count> result;
    std::size_t index = 0;
    for (auto item : *items) {
        ATELIER_TRY_BIND(number, item.f32());
        result[index++] = *number;
    }
    return result;
}

// Kept records recursively merge; any other kept leaf replaces the emitted one.
inline void overlay(json::Builder& builder, json::Value target, json::Value kept) {
    if (target.kind() != json::Kind::object || kept.kind() != json::Kind::object) {
        builder.replace(target, kept);
        return;
    }
    const auto members = kept.object_items();
    for (auto [key, value] : *members) {
        const auto at = target.find(key);
        if (*at)
            overlay(builder, **at, value);
        else
            builder.set_validated(target, key, value);
    }
}

// A saved value shortened for diagnostics, cut at a UTF-8 boundary.
inline String preview(json::Value value) {
    const auto bytes = json::write(value);
    const StringView full{bytes.data(), bytes.size()};
    const auto prefix = bytes.size() > 96 ? full.utf8_prefix(93) : full;
    return prefix.size() != bytes.size() ? String::concat(String::from_utf8_validated(prefix), "...")
                                         : String::from_utf8_validated(prefix);
}
}
