#pragma once

#include "foundation/string.hpp"

#include <concepts>
#include <functional>
#include <type_traits>

namespace atelier {

// Hashes are process-side lookup aids, not persistent identities. Equal keys
// must hash equally; custom policies must not depend on mutable table state.
template<class T>
struct Hash {
    [[nodiscard]] std::size_t operator()(const T& value) const noexcept(noexcept(static_cast<std::size_t>(std::hash<T>{}(value))))
        requires requires { static_cast<std::size_t>(std::hash<T>{}(value)); } {
        return static_cast<std::size_t>(std::hash<T>{}(value));
    }
};

template<class T>
struct Equal {
    [[nodiscard]] bool operator()(const T& left, const T& right) const noexcept(noexcept(static_cast<bool>(left == right))) {
        return static_cast<bool>(left == right);
    }
};

template<>
struct Hash<StringView> {
    using is_transparent = void;
    using is_avalanching = void;
    [[nodiscard]] std::size_t operator()(StringView value) const noexcept;
    [[nodiscard]] std::size_t operator()(const char* value) const noexcept;
};
template<> struct Hash<String> : Hash<StringView> {};

template<>
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
template<> struct Equal<String> : Equal<StringView> {};

namespace detail {
template<class T>
concept DenseValue = std::is_object_v<T> && !std::is_const_v<T> && !std::is_volatile_v<T> &&
    std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T> &&
    std::is_nothrow_destructible_v<T>;

template<class T>
concept HashPolicy = std::is_nothrow_default_constructible_v<T> && std::is_nothrow_copy_constructible_v<T> &&
    std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T> &&
    std::is_nothrow_swappable_v<T> && std::is_nothrow_destructible_v<T>;

// Without transparent policies the backend accepts K, not Q. Requiring the
// actual key type avoids a hidden conversion outside the inspected call path.
template<class K, class Q, class H, class E>
concept HashQuery = (std::same_as<std::remove_cvref_t<Q>, K> ||
    requires { typename H::is_transparent; typename E::is_transparent; }) &&
    std::is_nothrow_invocable_r_v<std::size_t, const H&, const Q&> &&
    std::is_nothrow_invocable_r_v<bool, const E&, const Q&, const K&>;
}

}
