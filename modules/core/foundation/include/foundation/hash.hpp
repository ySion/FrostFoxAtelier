#pragma once

#include "foundation/string.hpp"

#include <ankerl/unordered_dense.h>

#include <concepts>
#include <cstdint>
#include <functional>
#include <tuple>
#include <type_traits>

namespace atelier {

namespace detail {
template <class T>
struct HashAvalanche {};

template <class T>
    requires ankerl::unordered_dense::hash_is_avalanching_v<std::hash<T>>
struct HashAvalanche<T> {
    using is_avalanching = void;
};
}

// Hashes are process-side lookup aids, not persistent identities. Equal keys
// must hash equally; custom policies must not depend on mutable table state.
// is_avalanching lets the table use the result without mixing it again.
template <class T>
struct Hash : detail::HashAvalanche<T> {
    [[nodiscard]] std::size_t operator()(const T& value) const
        noexcept(noexcept(static_cast<std::size_t>(std::hash<T>{}(value))))
        requires requires { static_cast<std::size_t>(std::hash<T>{}(value)); }
    {
        return static_cast<std::size_t>(std::hash<T>{}(value));
    }
};

// The MSVC STL's std::hash folds integers byte by byte (FNV-1a); one 64-bit
// multiply-mix is cheaper and already avalanching.
template <class T>
    requires(std::is_integral_v<T> || std::is_enum_v<T> || std::is_pointer_v<T>) && std::same_as<T, std::remove_cv_t<T>>
struct Hash<T> : ankerl::unordered_dense::hash<T> {};

// Avalanching hash over every field of a composite identity; one mix per field.
template <std::integral... T>
[[nodiscard]] std::size_t hash_fields(T... fields) noexcept {
    return ankerl::unordered_dense::hash<std::tuple<T...>>{}(std::tuple<T...>{fields...});
}

template <class T>
struct Equal {
    [[nodiscard]] bool operator()(const T& left, const T& right) const
        noexcept(noexcept(static_cast<bool>(left == right))) {
        return static_cast<bool>(left == right);
    }
};

template <>
struct Hash<StringView> {
    using is_transparent = void;

    using is_avalanching = void;

    [[nodiscard]] std::size_t operator()(StringView value) const noexcept {
        // Uses the pinned backend's public byte hash. No validation, allocation,
        // case folding or NUL scan: a query may contain arbitrary bytes.
        return ankerl::unordered_dense::hash<std::string_view>{}(value.std_view());
    }

    [[nodiscard]] std::size_t operator()(const char* value) const noexcept {
        return ankerl::unordered_dense::hash<std::string_view>{}(std::string_view{value ? value : ""});
    }
};

template <>
struct Hash<String> : Hash<StringView> {};

template <>
struct Equal<StringView> {
    using is_transparent = void;

    [[nodiscard]] bool operator()(StringView left, StringView right) const noexcept { return left == right; }

    [[nodiscard]] bool operator()(const char* left, StringView right) const noexcept {
        return std::string_view{left ? left : ""} == right.std_view();
    }

    [[nodiscard]] bool operator()(StringView left, const char* right) const noexcept { return (*this)(right, left); }

    [[nodiscard]] bool operator()(const char* left, const char* right) const noexcept {
        return std::string_view{left ? left : ""} == std::string_view{right ? right : ""};
    }
};

template <>
struct Equal<String> : Equal<StringView> {};

namespace detail {
template <class T>
concept DenseValue =
    std::is_object_v<T> && !std::is_const_v<T> && !std::is_volatile_v<T> && std::is_nothrow_move_constructible_v<T> &&
    std::is_nothrow_move_assignable_v<T> && std::is_nothrow_destructible_v<T>;

template <class T>
concept HashPolicy = std::is_nothrow_default_constructible_v<T> && std::is_nothrow_copy_constructible_v<T> &&
                     std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T> &&
                     std::is_nothrow_swappable_v<T> && std::is_nothrow_destructible_v<T>;

// Without transparent policies the backend accepts K, not Q. Requiring the
// actual key type avoids a hidden conversion outside the inspected call path.
template <class K, class Q, class H, class E>
concept HashQuery = (std::same_as<std::remove_cvref_t<Q>, K> ||
                     requires {
                         typename H::is_transparent;
                         typename E::is_transparent;
                     }) &&
                    std::is_nothrow_invocable_r_v<std::size_t, const H&, const Q&> &&
                    std::is_nothrow_invocable_r_v<bool, const E&, const Q&, const K&>;
}

}
