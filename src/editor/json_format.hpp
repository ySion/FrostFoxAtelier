#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <string>
#include <unordered_set>

// On-disk rules shared by Project, Scene and graph files; the DOM stays out of their interfaces.
namespace atelier::editor::json_format {
using Json = nlohmann::json;
inline constexpr std::size_t maximum_bytes = 16 * 1024 * 1024;
inline Json parse(std::string_view text) {
    if (text.size() > maximum_bytes) throw std::runtime_error("JSON exceeds the 16 MiB file limit.");
    std::vector<std::unordered_set<std::string>> objects;
    return Json::parse(text, [&](int depth, Json::parse_event_t event, Json& parsed) {
        using enum Json::parse_event_t;
        if ((event == object_start || event == array_start) && depth >= 32)
            throw std::runtime_error("JSON nesting exceeds the format limit.");
        if (event == object_start) objects.emplace_back();
        else if (event == object_end) objects.pop_back();
        else if (event == key && !objects.back().insert(parsed.get_ref<const std::string&>()).second)
            throw std::runtime_error("Duplicate JSON field: " + parsed.get<std::string>());
        return true;
    });
}
inline void fields(const Json& value, std::initializer_list<std::string_view> allowed) {
    if (!value.is_object()) throw std::runtime_error("Expected a JSON record.");
    for (const auto& [name, item] : value.items()) {
        (void)item;
        if (std::ranges::find(allowed, name) == allowed.end()) throw std::runtime_error("Unsupported field: " + name);
    }
}
inline std::size_t identity(const Json& value) {
    if (value.is_number_unsigned()) return value.get<std::size_t>();
    if (value.is_number_integer() && value.get<std::int64_t>() >= 0) return value.get<std::size_t>();
    throw std::runtime_error("Expected a nonnegative integer identity or version.");
}
// A float field is written as a decimal that reads back as the same float, normally the
// shortest (0.2f as 0.2), not as the exact double the float widens to (0.20000000298023224).
inline Json number(float value) {
    std::array<char, 32> text;
    const auto end = std::to_chars(text.data(), text.data() + text.size(), value).ptr;
    double shortest{};
    std::from_chars(text.data(), end, shortest);
    // Reading parses a double and rounds it to float. Only for ±7.038531e-26 does that double
    // rounding land on the neighbouring float; those two keep the exact widened value.
    return std::bit_cast<std::uint32_t>(static_cast<float>(shortest)) == std::bit_cast<std::uint32_t>(value) ? shortest : double(value);
}
template<std::size_t Count> Json numbers(const std::array<float, Count>& values) {
    Json result = Json::array();
    for (const float value : values) result.push_back(number(value));
    return result;
}
template<std::size_t Count> std::array<float, Count> vector(const Json& value) {
    if (!value.is_array() || value.size() != Count) throw std::runtime_error("Invalid vector length.");
    std::array<float, Count> result;
    for (std::size_t i = 0; i < Count; ++i) {
        if (!value[i].is_number()) throw std::runtime_error("Expected a numeric vector component.");
        result[i] = value[i].get<float>();
        if (!std::isfinite(result[i])) throw std::runtime_error("Vector components must be finite.");
    }
    return result;
}
// Kept saved JSON is sparse: on save a kept leaf replaces the emitted value at its path, and kept
// records merge into emitted records.
inline void overlay(Json& target, const Json& kept) {
    if (!target.is_object() || !kept.is_object()) { target = kept; return; }
    for (const auto& [key, value] : kept.items()) {
        if (auto at = target.find(key); at != target.end()) overlay(*at, value);
        else target[key] = value;
    }
}
// A saved value shortened for diagnostics, cut at a UTF-8 boundary.
inline std::string preview(const Json& value) {
    auto text = value.dump(-1, ' ', false, Json::error_handler_t::replace);
    if (text.size() > 96) {
        std::size_t cut = 93;
        while (cut && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
        text.resize(cut);
        text += "...";
    }
    return text;
}
}
