#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_set>

// On-disk rules shared by Project and Scene; the DOM stays out of their interfaces.
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
}
