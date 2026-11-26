#pragma once

#include "foundation/detail/byte_hash.hpp"
#include "foundation/string.hpp"

#include <array>
#include <concepts>
#include <cstdint>
#include <cwchar>
#include <functional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

namespace atelier {

template <class T>
class Optional;

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

namespace detail {
inline constexpr std::uint64_t hash_seed = hash_constants[4];

template <class F>
concept FieldHashable =
    std::is_integral_v<std::remove_cvref_t<F>> || std::is_enum_v<std::remove_cvref_t<F>> ||
    std::is_pointer_v<std::remove_cvref_t<F>> ||
    (std::is_nothrow_default_constructible_v<Hash<std::remove_cvref_t<F>>> &&
     std::is_nothrow_invocable_r_v<std::size_t, const Hash<std::remove_cvref_t<F>>&, const F&>);

template <class Element, class U>
[[nodiscard]] inline std::uint64_t hash_element_step(std::uint64_t state, const U& field) noexcept {
    std::uint64_t bits{};
    if constexpr (std::same_as<Element, std::remove_cvref_t<U>> && std::is_pointer_v<Element>)
        bits = reinterpret_cast<std::uintptr_t>(field);
    else if constexpr (std::same_as<Element, std::remove_cvref_t<U>> &&
                       (std::is_integral_v<Element> || std::is_enum_v<Element>))
        bits = static_cast<std::uint64_t>(field);
    else
        bits = static_cast<std::uint64_t>(Hash<Element>{}(field));
    const auto a = state ^ hash_constants[0];
    const auto b = bits ^ hash_constants[1];
    return multiply_fold(a, b) ^ a ^ b;
}
}

// Avalanching hash over every field of a composite identity; one mix per field.
template <detail::FieldHashable... T>
    requires(sizeof...(T) > 0)
[[nodiscard]] std::size_t hash_fields(const T&... fields) noexcept {
    std::uint64_t state = detail::hash_seed;
    ((state = detail::hash_element_step<std::remove_cvref_t<T>>(state, fields)), ...);
    return state;
}

template <class T>
struct Equal {
    [[nodiscard]] bool operator()(const T& left, const T& right) const
        noexcept(noexcept(static_cast<bool>(left == right))) {
        return static_cast<bool>(left == right);
    }
};

namespace detail {
template <class T>
concept ByteStringLike = std::is_nothrow_convertible_v<const T&, StringView> ||
                         std::is_nothrow_convertible_v<const T&, std::string_view> ||
                         std::is_nothrow_convertible_v<const T&, const char*>;

[[nodiscard]] constexpr std::string_view as_std_string_view(const ByteStringLike auto& value) noexcept {
    using Decayed = std::decay_t<decltype(value)>;
    if constexpr (std::is_pointer_v<Decayed>) {
        const char* ptr = value;
        return ptr ? std::string_view{ptr} : std::string_view{};
    } else if constexpr (std::is_nothrow_convertible_v<const Decayed&, std::string_view>) {
        return std::string_view{value};
    } else {
        return StringView{value}.std_view();
    }
}

template <class T>
concept WideStringLike = std::is_nothrow_convertible_v<const T&, std::wstring_view> ||
                         std::is_nothrow_convertible_v<const T&, const wchar_t*>;

[[nodiscard]] constexpr std::wstring_view as_std_wstring_view(const WideStringLike auto& value) noexcept {
    using Decayed = std::decay_t<decltype(value)>;
    if constexpr (std::is_pointer_v<Decayed>) {
        const wchar_t* ptr = value;
        return ptr ? std::wstring_view{ptr} : std::wstring_view{};
    } else {
        return std::wstring_view{value};
    }
}
}

template <>
struct Hash<StringView> {
    using is_transparent = void;

    using is_avalanching = void;

    template <detail::ByteStringLike S = StringView>
    [[nodiscard]] std::size_t operator()(const S& value) const noexcept {
        const auto view = detail::as_std_string_view(value);
        return detail::hash_byte_range(reinterpret_cast<const std::byte*>(view.data()), view.size());
    }
};

template <>
struct Hash<String> : Hash<StringView> {};

template <>
struct Hash<std::string_view> : Hash<StringView> {};

template <>
struct Hash<std::string> : Hash<StringView> {};

template <>
struct Equal<StringView> {
    using is_transparent = void;

    template <detail::ByteStringLike L = StringView, detail::ByteStringLike R = StringView>
    [[nodiscard]] constexpr bool operator()(const L& left, const R& right) const noexcept {
        return detail::as_std_string_view(left) == detail::as_std_string_view(right);
    }
};

template <>
struct Equal<String> : Equal<StringView> {};

template <>
struct Equal<std::string_view> : Equal<StringView> {};

template <>
struct Equal<std::string> : Equal<StringView> {};

template <>
struct Hash<std::wstring_view> {
    using is_transparent = void;

    using is_avalanching = void;

    template <detail::WideStringLike S = std::wstring_view>
    [[nodiscard]] std::size_t operator()(const S& value) const noexcept {
        const auto view = detail::as_std_wstring_view(value);
        return detail::hash_byte_range(reinterpret_cast<const std::byte*>(view.data()),
                                       view.size() * sizeof(wchar_t));
    }
};

template <>
struct Hash<std::wstring> : Hash<std::wstring_view> {};

template <>
struct Equal<std::wstring_view> {
    using is_transparent = void;

    template <detail::WideStringLike L = std::wstring_view, detail::WideStringLike R = std::wstring_view>
    [[nodiscard]] constexpr bool operator()(const L& left, const R& right) const noexcept {
        return detail::as_std_wstring_view(left) == detail::as_std_wstring_view(right);
    }
};

template <>
struct Equal<std::wstring> : Equal<std::wstring_view> {};

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

template <class H, class E>
concept TransparentPolicyPair = requires {
    typename H::is_transparent;
    typename E::is_transparent;
};

// Without transparent policies the backend accepts K, not Q. Requiring the
// actual key type avoids a hidden conversion outside the inspected call path.
template <class K, class Q, class H, class E>
concept HashQuery = (std::same_as<std::remove_cvref_t<Q>, K> || TransparentPolicyPair<H, E>) &&
                    std::is_nothrow_invocable_r_v<std::size_t, const H&, const Q&> &&
                    std::is_nothrow_invocable_r_v<bool, const E&, const Q&, const K&>;

template <class T>
concept HashableElement =
    std::same_as<T, std::remove_cv_t<T>> && std::is_nothrow_default_constructible_v<Hash<T>> &&
    std::is_nothrow_invocable_r_v<std::size_t, const Hash<T>&, const T&>;

template <class T>
concept EquatableElement =
    std::same_as<T, std::remove_cv_t<T>> && std::is_nothrow_default_constructible_v<Equal<T>> &&
    std::is_nothrow_invocable_r_v<bool, const Equal<T>&, const T&, const T&>;

template <class A, class B>
struct PairTransparency {};

template <class A, class B>
    requires(TransparentPolicyPair<Hash<A>, Equal<A>> || TransparentPolicyPair<Hash<B>, Equal<B>>)
struct PairTransparency<A, B> {
    using is_transparent = void;
};
}

template <detail::HashableElement A, detail::HashableElement B>
struct Hash<std::pair<A, B>> : detail::PairTransparency<A, B> {
    using is_avalanching = void;

    template <class U1 = A, class U2 = B>
        requires detail::HashQuery<A, U1, Hash<A>, Equal<A>> && detail::HashQuery<B, U2, Hash<B>, Equal<B>>
    [[nodiscard]] std::size_t operator()(const std::pair<U1, U2>& value) const noexcept {
        const auto first = detail::hash_element_step<A>(detail::hash_seed, value.first);
        return detail::hash_element_step<B>(first, value.second);
    }
};

template <detail::EquatableElement A, detail::EquatableElement B>
struct Equal<std::pair<A, B>> : detail::PairTransparency<A, B> {
    template <class U1 = A, class U2 = B, class V1 = A, class V2 = B>
        requires std::is_nothrow_invocable_r_v<bool, const Equal<A>&, const U1&, const V1&> &&
                 std::is_nothrow_invocable_r_v<bool, const Equal<B>&, const U2&, const V2&>
    [[nodiscard]] constexpr bool operator()(const std::pair<U1, U2>& left,
                                            const std::pair<V1, V2>& right) const noexcept {
        return Equal<A>{}(left.first, right.first) && Equal<B>{}(left.second, right.second);
    }
};

template <detail::HashableElement... Ts>
struct Hash<std::tuple<Ts...>> {
    using is_avalanching = void;

    [[nodiscard]] std::size_t operator()(const std::tuple<Ts...>& value) const noexcept {
        if constexpr (sizeof...(Ts) == 0) {
            static_cast<void>(value);
            return detail::hash_seed;
        } else {
            std::uint64_t state = detail::hash_seed;
            std::apply(
                [&state](const Ts&... elements) noexcept {
                    ((state = detail::hash_element_step<Ts>(state, elements)), ...);
                },
                value);
            return state;
        }
    }
};

template <detail::EquatableElement... Ts>
struct Equal<std::tuple<Ts...>> {
    [[nodiscard]] constexpr bool operator()(const std::tuple<Ts...>& left,
                                            const std::tuple<Ts...>& right) const noexcept {
        return [&]<std::size_t... I>(std::index_sequence<I...>) noexcept {
            return (Equal<Ts>{}(std::get<I>(left), std::get<I>(right)) && ...);
        }(std::index_sequence_for<Ts...>{});
    }
};

template <detail::HashableElement T, std::size_t N>
struct Hash<std::array<T, N>> {
    using is_avalanching = void;

    [[nodiscard]] std::size_t operator()(const std::array<T, N>& values) const noexcept {
        std::uint64_t state = detail::hash_seed;
        for (const auto& element : values)
            state = detail::hash_element_step<T>(state, element);
        return state;
    }
};

template <detail::EquatableElement T, std::size_t N>
struct Equal<std::array<T, N>> {
    [[nodiscard]] constexpr bool operator()(const std::array<T, N>& left,
                                            const std::array<T, N>& right) const noexcept {
        for (std::size_t i = 0; i < N; ++i)
            if (!Equal<T>{}(left[i], right[i]))
                return false;
        return true;
    }
};

template <detail::HashableElement T>
struct Hash<Optional<T>> {
    using is_avalanching = void;

    [[nodiscard]] std::size_t operator()(const Optional<T>& value) const noexcept {
        if (const auto* present = value.try_get())
            return detail::hash_element_step<T>(detail::hash_seed, *present);
        return UINT64_C(0x9e3779b97f4a7c15);
    }
};

template <detail::EquatableElement T>
struct Equal<Optional<T>> {
    [[nodiscard]] constexpr bool operator()(const Optional<T>& left, const Optional<T>& right) const noexcept {
        const auto* a = left.try_get();
        const auto* b = right.try_get();
        if (!a || !b)
            return a == b;
        return Equal<T>{}(*a, *b);
    }
};

}
