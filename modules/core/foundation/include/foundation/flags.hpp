#pragma once

#include <type_traits>
#include <utility>

namespace atelier {

template <class E>
inline constexpr bool enable_enum_flags = false;

template <class E>
concept FlagEnum = std::is_enum_v<E> && enable_enum_flags<E>;

// Opt-in preserves enum identity and unknown bits; there is no implicit mask
// or global operator overload that changes unrelated enums' arithmetic.
template <FlagEnum E>
[[nodiscard]] constexpr E flags_or(E left, E right) noexcept {
    return static_cast<E>(std::to_underlying(left) | std::to_underlying(right));
}

template <FlagEnum E>
[[nodiscard]] constexpr E flags_and(E left, E right) noexcept {
    return static_cast<E>(std::to_underlying(left) & std::to_underlying(right));
}

template <FlagEnum E>
[[nodiscard]] constexpr E flags_xor(E left, E right) noexcept {
    return static_cast<E>(std::to_underlying(left) ^ std::to_underlying(right));
}

template <FlagEnum E>
[[nodiscard]] constexpr E flags_without(E value, E removed) noexcept {
    return static_cast<E>(std::to_underlying(value) & ~std::to_underlying(removed));
}

template <FlagEnum E>
[[nodiscard]] constexpr bool has_any(E value, E mask) noexcept {
    return std::to_underlying(flags_and(value, mask)) != 0;
}

template <FlagEnum E>
[[nodiscard]] constexpr bool has_all(E value, E mask) noexcept {
    return flags_and(value, mask) == mask;
}

}
