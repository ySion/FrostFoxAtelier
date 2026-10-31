#pragma once
#include <nlohmann/json.hpp>
#include <cmath>
#include <string_view>

namespace atelier::acceptance {
inline bool dungeons_roundtrip_equal(std::string_view before, std::string_view after) {
    if (before == after)
        return true;
    const auto original = nlohmann::json::parse(before), reopened = nlohmann::json::parse(after);
    for (const auto& change : nlohmann::json::diff(original, reopened)) {
        const auto path = change.at("path").get<std::string>();
        // World canonicalizes rotations at load. A float quaternion can move
        // one ULP when normalized again; all other persisted data remains exact.
        const auto rotation = path.rfind("/rotation/");
        if (change.at("op") != "replace" || rotation == std::string::npos || rotation + 11 != path.size() ||
            path.back() < '0' || path.back() > '3')
            return false;
        const auto& value = original.at(nlohmann::json::json_pointer(path));
        if (!value.is_number() || !change.at("value").is_number() ||
            std::abs(value.get<double>() - change.at("value").get<double>()) > 1e-7)
            return false;
    }
    return true;
}
}
