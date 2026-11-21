#pragma once

#include "foundation/detail/byte_hash.hpp"
#include "foundation/string.hpp"

#include <concepts>
#include <cstdint>
#include <functional>
#include <type_traits>

namespace atelier {

namespace detail {
template <class H>
inline constexpr bool hash_is_avalanching_v = [] {
    if constexpr (requires { typename H::is_avalanching; }) {
        using Marker = typename H::is_avalanching;
        if constexpr (std::is_void_v<Marker>)
            return true;
        else
            return static_cast<bool>(Marker::value);
    } else {
        static_assert(!requires { H::is_avalanching; }, "is_avalanching must be a type.");
        return false;
    }
}();

template <class T>
struct HashAvalanche {};

template <class T>
    requires hash_is_avalanching_v<std::hash<T>>
struct HashAvalanche<T> {
    using is_avalanching = void;
};
}

[[nodiscard]] inline std::uint64_t hash_bytes(Span<const std::byte> bytes) noexcept {
    return detail::hash_byte_range(bytes.data(), bytes.size());
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
struct Hash<T> {
    using is_avalanching = void;

    [[nodiscard]] std::size_t operator()(T value) const noexcept {
        if constexpr (std::is_pointer_v<T>)
            return detail::multiply_fold(reinterpret_cast<std::uintptr_t>(value), UINT64_C(0x9e3779b97f4a7c15));
        else
            return detail::multiply_fold(static_cast<std::uint64_t>(value), UINT64_C(0x9e3779b97f4a7c15));
    }
};

// Avalanching hash over every field of a composite identity; one mix per field.
template <std::integral... T>
[[nodiscard]] std::size_t hash_fields(T... fields) noexcept {
    std::uint64_t state = 0;
    ((state = detail::multiply_fold(state + static_cast<std::uint64_t>(fields), UINT64_C(0x9ddfea08eb382d69))), ...);
    return state;
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
        return detail::hash_byte_range(reinterpret_cast<const std::byte*>(value.data()), value.size());
    }

    [[nodiscard]] std::size_t operator()(const char* value) const noexcept { return (*this)(StringView{value}); }
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
concept HashPolicyLifecycle =
    std::is_nothrow_copy_constructible_v<T> && std::is_nothrow_move_constructible_v<T> &&
    std::is_nothrow_move_assignable_v<T> && std::is_nothrow_swappable_v<T> && std::is_nothrow_destructible_v<T>;

template <class T>
concept HashPolicy = HashPolicyLifecycle<T> && std::is_nothrow_default_constructible_v<T>;

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
