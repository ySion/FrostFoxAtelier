#pragma once

#include "procedural/geometry.hpp"

#include <array>
#include <concepts>

namespace atelier::procedural::detail {

// ids index the source column's layer. Equal barycentric weights resolve to the lowest source ID.
template <class T>
T interpolate_attribute(const Array<T>& values, const std::array<std::uint32_t, 3>& ids,
                        const std::array<double, 3>& weights, Usage usage, bool reverse_normal = false) {
    if constexpr (std::same_as<T, float>) {
        return static_cast<float>(values[ids[0]] * weights[0] + values[ids[1]] * weights[1] +
                                  values[ids[2]] * weights[2]);
    } else if constexpr (std::same_as<T, math::Vector2> || std::same_as<T, math::Vector3> ||
                         std::same_as<T, math::Vector4>) {
        T result{};
        for (std::size_t k = 0; k < T::dimensions; ++k)
            result[k] = static_cast<float>(values[ids[0]][k] * weights[0] + values[ids[1]][k] * weights[1] +
                                           values[ids[2]][k] * weights[2]);
        if constexpr (std::same_as<T, math::Vector3>) {
            if (usage == Usage::normal) {
                result = math::try_normalize(result).value_or(T{});
                if (reverse_normal)
                    result = -result;
            }
        }
        return result;
    } else {
        std::size_t nearest = 0;
        for (std::size_t k = 1; k < ids.size(); ++k)
            if (weights[k] > weights[nearest] || (weights[k] == weights[nearest] && ids[k] < ids[nearest]))
                nearest = k;
        return values[ids[nearest]];
    }
}

}
