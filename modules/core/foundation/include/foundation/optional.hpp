#pragma once

#include "memory/fatal.hpp"

#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace atelier {

template <class T>
class Optional;

namespace detail {
template <class T>
inline constexpr bool is_atelier_optional_v = false;

template <class T>
inline constexpr bool is_atelier_optional_v<Optional<T>> = true;

template <class T>
inline constexpr bool is_std_optional_v = false;

template <class T>
inline constexpr bool is_std_optional_v<std::optional<T>> = true;

template <class T, class W>
inline constexpr bool converts_from_any_cvref_v =
    std::is_constructible_v<T, W&> || std::is_convertible_v<W&, T> || std::is_constructible_v<T, W> ||
    std::is_convertible_v<W, T> || std::is_constructible_v<T, const W&> || std::is_convertible_v<const W&, T> ||
    std::is_constructible_v<T, const W> || std::is_convertible_v<const W, T>;

template <class T, class W>
inline constexpr bool assigns_from_any_cvref_v =
    std::is_assignable_v<T&, W&> || std::is_assignable_v<T&, W> || std::is_assignable_v<T&, const W&> ||
    std::is_assignable_v<T&, const W>;
}

// An inline value or absence, not an error channel. The payload is at offset zero,
// followed by one byte of engagement; empty storage contains no live T.
template <class T>
class Optional {
    template <class>
    friend class Optional;

    static_assert(std::is_object_v<T> && !std::is_array_v<T> && !std::is_const_v<T> && !std::is_volatile_v<T> &&
                  !std::same_as<T, std::in_place_t> && !std::same_as<T, std::nullopt_t>);

    static_assert(std::is_nothrow_destructible_v<T>);

    static constexpr bool trivial_copy_assign = std::is_trivially_copy_constructible_v<T> &&
                                                std::is_trivially_copy_assignable_v<T> &&
                                                std::is_trivially_destructible_v<T>;

    static constexpr bool trivial_move_assign = std::is_trivially_move_constructible_v<T> &&
                                                std::is_trivially_move_assignable_v<T> &&
                                                std::is_trivially_destructible_v<T>;

    union Storage {
        char empty{};

        T value;

        constexpr Storage() noexcept : empty() {}

        template <class... Args>
        explicit constexpr Storage(std::in_place_t, Args&&... arguments) noexcept
            : value(std::forward<Args>(arguments)...) {}

        constexpr Storage(const Storage&) noexcept
            requires std::is_trivially_copy_constructible_v<T>
        = default;

        constexpr Storage(Storage&&) noexcept
            requires std::is_trivially_move_constructible_v<T>
        = default;

        constexpr Storage& operator=(const Storage&) noexcept
            requires trivial_copy_assign
        = default;

        constexpr Storage& operator=(Storage&&) noexcept
            requires trivial_move_assign
        = default;

        constexpr ~Storage()
            requires std::is_trivially_destructible_v<T>
        = default;

        constexpr ~Storage()
            requires(!std::is_trivially_destructible_v<T>)
        {}
    };

public:
    using Value = T;

    constexpr Optional() noexcept = default;

    constexpr Optional(std::nullopt_t) noexcept {}

    template <class... Args>
        requires std::is_constructible_v<T, Args...>
    explicit constexpr Optional(std::in_place_t, Args&&... arguments) noexcept
        : storage_(std::in_place, std::forward<Args>(arguments)...), engaged_(1) {}

    template <class U, class... Args>
        requires std::is_constructible_v<T, std::initializer_list<U>&, Args...>
    explicit constexpr Optional(std::in_place_t, std::initializer_list<U> items, Args&&... arguments) noexcept
        : storage_(std::in_place, items, std::forward<Args>(arguments)...), engaged_(1) {}

    template <class U = T>
        requires(!std::same_as<std::remove_cvref_t<U>, Optional> &&
                 !std::same_as<std::remove_cvref_t<U>, std::in_place_t> &&
                 !std::same_as<std::remove_cvref_t<U>, std::nullopt_t> &&
                 !(std::same_as<T, bool> && (detail::is_atelier_optional_v<std::remove_cvref_t<U>> ||
                                             detail::is_std_optional_v<std::remove_cvref_t<U>>)) &&
                 std::is_constructible_v<T, U>)
    explicit(!std::is_convertible_v<U, T>) constexpr Optional(U&& value) noexcept
        : Optional(std::in_place, std::forward<U>(value)) {}

    template <class U>
        requires(!std::same_as<T, U>) && std::is_constructible_v<T, const U&> &&
                (std::same_as<T, bool> || !detail::converts_from_any_cvref_v<T, Optional<U>>)
    explicit(!std::is_convertible_v<const U&, T>) constexpr Optional(const Optional<U>& other) noexcept {
        if (other.engaged_) {
            std::construct_at(std::addressof(storage_.value), *other.pointer());
            engaged_ = 1;
        }
    }

    template <class U>
        requires(!std::same_as<T, U>) && std::is_constructible_v<T, U> &&
                (std::same_as<T, bool> || !detail::converts_from_any_cvref_v<T, Optional<U>>)
    explicit(!std::is_convertible_v<U, T>) constexpr Optional(Optional<U>&& other) noexcept {
        if (other.engaged_) {
            std::construct_at(std::addressof(storage_.value), std::move(*other.pointer()));
            engaged_ = 1;
        }
    }

    template <class U>
        requires(!std::same_as<T, U>) && std::is_constructible_v<T, const U&> && (!std::is_constructible_v<T, const U>)
    Optional(const Optional<U>&&) = delete;

    constexpr Optional(const Optional&) noexcept
        requires std::is_trivially_copy_constructible_v<T>
    = default;

    constexpr Optional(const Optional& other) noexcept
        requires(std::is_copy_constructible_v<T> && !std::is_trivially_copy_constructible_v<T>)
    {
        if (other.engaged_) {
            std::construct_at(std::addressof(storage_.value), *other.pointer());
            engaged_ = 1;
        }
    }

    constexpr Optional(Optional&&) noexcept
        requires std::is_trivially_move_constructible_v<T>
    = default;

    constexpr Optional(Optional&& other) noexcept
        requires(std::is_move_constructible_v<T> && !std::is_trivially_move_constructible_v<T>)
    {
        if (other.engaged_) {
            std::construct_at(std::addressof(storage_.value), std::move(*other.pointer()));
            engaged_ = 1;
        }
    }

    constexpr Optional& operator=(const Optional&) noexcept
        requires trivial_copy_assign
    = default;

    constexpr Optional& operator=(const Optional& other) noexcept
        requires(std::is_copy_constructible_v<T> && std::is_copy_assignable_v<T> && !trivial_copy_assign)
    {
        if (this != &other) {
            if (!other.engaged_)
                reset();
            else if (engaged_)
                *pointer() = *other.pointer();
            else
                emplace(*other.pointer());
        }
        return *this;
    }

    constexpr Optional& operator=(Optional&&) noexcept
        requires trivial_move_assign
    = default;

    constexpr Optional& operator=(Optional&& other) noexcept
        requires(std::is_move_constructible_v<T> && std::is_move_assignable_v<T> && !trivial_move_assign)
    {
        if (this != &other) {
            if (!other.engaged_)
                reset();
            else if (engaged_)
                *pointer() = std::move(*other.pointer());
            else
                emplace(std::move(*other.pointer()));
        }
        return *this;
    }

    template <class U = T>
        requires(!std::same_as<std::remove_cvref_t<U>, Optional>) &&
                (!detail::is_atelier_optional_v<std::remove_cvref_t<U>>) &&
                (!std::same_as<std::remove_cvref_t<U>, std::nullopt_t>) &&
                (!std::same_as<std::remove_cvref_t<U>, std::in_place_t>) &&
                (!(std::is_scalar_v<T> && std::same_as<T, std::decay_t<U>>)) &&
                (!(std::same_as<T, bool> && detail::is_std_optional_v<std::remove_cvref_t<U>>)) &&
                std::is_constructible_v<T, U> && std::is_assignable_v<T&, U>
    constexpr Optional& operator=(U&& value) noexcept {
        if (engaged_) {
            *pointer() = std::forward<U>(value);
        } else {
            std::construct_at(std::addressof(storage_.value), std::forward<U>(value));
            engaged_ = 1;
        }
        return *this;
    }

    template <class U>
        requires(!std::same_as<T, U>) && std::is_constructible_v<T, const U&> && std::is_assignable_v<T&, const U&> &&
                (!detail::converts_from_any_cvref_v<T, Optional<U>>) &&
                (!detail::assigns_from_any_cvref_v<T, Optional<U>>)
    constexpr Optional& operator=(const Optional<U>& other) noexcept {
        if (!other.engaged_)
            reset();
        else if (engaged_)
            *pointer() = *other.pointer();
        else {
            std::construct_at(std::addressof(storage_.value), *other.pointer());
            engaged_ = 1;
        }
        return *this;
    }

    template <class U>
        requires(!std::same_as<T, U>) && std::is_constructible_v<T, U> && std::is_assignable_v<T&, U> &&
                (!detail::converts_from_any_cvref_v<T, Optional<U>>) &&
                (!detail::assigns_from_any_cvref_v<T, Optional<U>>)
    constexpr Optional& operator=(Optional<U>&& other) noexcept {
        if (!other.engaged_)
            reset();
        else if (engaged_)
            *pointer() = std::move(*other.pointer());
        else {
            std::construct_at(std::addressof(storage_.value), std::move(*other.pointer()));
            engaged_ = 1;
        }
        return *this;
    }

    template <class U>
        requires(!std::same_as<T, U>) && std::is_constructible_v<T, const U&> && std::is_assignable_v<T&, const U&> &&
                (!std::is_constructible_v<T, const U> || !std::is_assignable_v<T&, const U>)
    Optional& operator=(const Optional<U>&&) = delete;

    constexpr Optional& operator=(std::nullopt_t) noexcept {
        reset();
        return *this;
    }

    constexpr ~Optional()
        requires std::is_trivially_destructible_v<T>
    = default;

    constexpr ~Optional()
        requires(!std::is_trivially_destructible_v<T>)
    {
        reset();
    }

    [[nodiscard]] constexpr bool has_value() const noexcept { return engaged_ != 0; }

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] constexpr T* try_get() & noexcept [[clang::lifetimebound]] { return engaged_ ? pointer() : nullptr; }

    [[nodiscard]] constexpr const T* try_get() const& noexcept [[clang::lifetimebound]] {
        return engaged_ ? pointer() : nullptr;
    }

    T* try_get() && = delete;

    const T* try_get() const&& = delete;

    [[nodiscard]] constexpr T& operator*() & noexcept [[clang::lifetimebound]] { return *operator->(); }

    [[nodiscard]] constexpr const T& operator*() const& noexcept [[clang::lifetimebound]] { return *operator->(); }

    T& operator*() && = delete;

    const T& operator*() const&& = delete;

    [[nodiscard]] constexpr T* operator->() & noexcept [[clang::lifetimebound]] {
        if (!engaged_)
            fatal("Empty Optional access.");
        return pointer();
    }

    [[nodiscard]] constexpr const T* operator->() const& noexcept [[clang::lifetimebound]] {
        if (!engaged_)
            fatal("Empty Optional access.");
        return pointer();
    }

    T* operator->() && = delete;

    const T* operator->() const&& = delete;

    template <class U = T>
        requires(std::is_copy_constructible_v<T> && std::is_constructible_v<T, U>)
    [[nodiscard]] constexpr T value_or(U&& fallback) const& noexcept {
        return engaged_ ? *pointer() : static_cast<T>(std::forward<U>(fallback));
    }

    template <class U = T>
        requires(std::is_move_constructible_v<T> && std::is_constructible_v<T, U>)
    [[nodiscard]] constexpr T value_or(U&& fallback) && noexcept {
        return engaged_ ? std::move(*pointer()) : static_cast<T>(std::forward<U>(fallback));
    }

    [[nodiscard]] constexpr Optional take() & noexcept
        requires std::is_move_constructible_v<T>
    {
        Optional extracted;
        if (engaged_) {
            std::construct_at(std::addressof(extracted.storage_.value), std::move(*pointer()));
            extracted.engaged_ = 1;
            reset();
        }
        return extracted;
    }

    Optional take() && = delete;

    template <class Self, class F>
        requires std::invocable<F, decltype(std::forward_like<Self>(std::declval<T&>()))>
    [[nodiscard]] constexpr auto transform(this Self&& self, F&& fn) noexcept {
        using Arg = decltype(std::forward_like<Self>(*self.pointer()));
        using Result = std::remove_cv_t<std::invoke_result_t<F, Arg>>;
        static_assert(std::is_object_v<Result> && !std::is_array_v<Result> &&
                      !std::same_as<Result, std::in_place_t> && !std::same_as<Result, std::nullopt_t>);
        Optional<Result> result;
        if (self.engaged_) {
            std::construct_at(std::addressof(result.storage_.value),
                              std::invoke(std::forward<F>(fn), std::forward_like<Self>(*self.pointer())));
            result.engaged_ = 1;
        }
        return result;
    }

    constexpr void reset() noexcept {
        if (engaged_) {
            std::destroy_at(pointer());
            if consteval {
                std::construct_at(std::addressof(storage_.empty));
            }
            engaged_ = 0;
        }
    }

    // Like optional emplacement, this ends the old value before constructing the
    // replacement. Arguments must not borrow that value. No operation may reenter.
    template <class... Args>
        requires std::is_constructible_v<T, Args...>
    constexpr T& emplace(Args&&... arguments) noexcept [[clang::lifetimebound]] {
        reset();
        auto* value = std::construct_at(std::addressof(storage_.value), std::forward<Args>(arguments)...);
        engaged_ = 1;
        return *value;
    }

    template <class U, class... Args>
        requires std::is_constructible_v<T, std::initializer_list<U>&, Args...>
    constexpr T& emplace(std::initializer_list<U> items, Args&&... arguments) noexcept [[clang::lifetimebound]] {
        reset();
        auto* value = std::construct_at(std::addressof(storage_.value), items, std::forward<Args>(arguments)...);
        engaged_ = 1;
        return *value;
    }

    constexpr void swap(Optional& other) noexcept
        requires(std::is_move_constructible_v<T> && std::is_swappable_v<T>)
    {
        if (this == &other)
            return;
        if (engaged_ && other.engaged_) {
            using std::swap;
            swap(*pointer(), *other.pointer());
        } else if (engaged_) {
            other.emplace(std::move(*pointer()));
            reset();
        } else if (other.engaged_) {
            emplace(std::move(*other.pointer()));
            other.reset();
        }
    }

    friend constexpr void swap(Optional& left, Optional& right) noexcept
        requires(std::is_move_constructible_v<T> && std::is_swappable_v<T>)
    {
        left.swap(right);
    }

    friend constexpr bool operator==(const Optional& left, const Optional& right) noexcept
        requires requires(const T& a, const T& b) {
            { a == b } -> std::convertible_to<bool>;
        }
    {
        if (left.engaged_ != right.engaged_)
            return false;
        return !left.engaged_ || static_cast<bool>(*left.pointer() == *right.pointer());
    }

    template <class U = T>
        requires(!detail::is_atelier_optional_v<std::remove_cvref_t<U>>) &&
                (!std::same_as<std::remove_cvref_t<U>, std::nullopt_t>) &&
                requires(const T& a, const U& b) {
                    { a == b } -> std::convertible_to<bool>;
                }
    friend constexpr bool operator==(const Optional& left, const U& right) noexcept {
        return left.engaged_ && static_cast<bool>(*left.pointer() == right);
    }

    friend constexpr bool operator==(const Optional& value, std::nullopt_t) noexcept { return !value.engaged_; }

    friend constexpr auto operator<=>(const Optional& left, const Optional& right) noexcept
        requires std::three_way_comparable<T>
    {
        using Result = std::compare_three_way_result_t<T>;
        if (left.engaged_ && right.engaged_)
            return static_cast<Result>(*left.pointer() <=> *right.pointer());
        return static_cast<Result>(left.engaged_ <=> right.engaged_);
    }

    friend constexpr std::strong_ordering operator<=>(const Optional& value, std::nullopt_t) noexcept {
        return value.engaged_ <=> std::uint8_t{0};
    }

    template <class U = T>
        requires(!detail::is_atelier_optional_v<std::remove_cvref_t<U>>) &&
                (!std::same_as<std::remove_cvref_t<U>, std::nullopt_t>) &&
                requires(const T& a, const U& b) {
                    { a <=> b } -> std::convertible_to<std::partial_ordering>;
                }
    friend constexpr std::compare_three_way_result_t<T, U> operator<=>(const Optional& left, const U& right) noexcept {
        using Result = std::compare_three_way_result_t<T, U>;
        return left.engaged_ ? static_cast<Result>(*left.pointer() <=> right)
                             : static_cast<Result>(std::strong_ordering::less);
    }

private:
    [[nodiscard]] constexpr T* pointer() noexcept { return std::addressof(storage_.value); }

    [[nodiscard]] constexpr const T* pointer() const noexcept { return std::addressof(storage_.value); }

    Storage storage_{};

    std::uint8_t engaged_{};
};

template <class T>
Optional(T) -> Optional<T>;

template <class T, class U>
    requires(!std::same_as<T, U>) && requires(const T& a, const U& b) {
        { a == b } -> std::convertible_to<bool>;
    }
[[nodiscard]] constexpr bool operator==(const Optional<T>& left, const Optional<U>& right) noexcept {
    const auto* a = left.try_get();
    const auto* b = right.try_get();
    if (!a || !b)
        return !a && !b;
    return static_cast<bool>(*a == *b);
}

template <class T, class U>
    requires(!std::same_as<T, U>) && requires(const T& a, const U& b) {
        { a <=> b } -> std::convertible_to<std::partial_ordering>;
    }
[[nodiscard]] constexpr std::compare_three_way_result_t<T, U> operator<=>(const Optional<T>& left,
                                                                          const Optional<U>& right) noexcept {
    using Result = std::compare_three_way_result_t<T, U>;
    const auto* a = left.try_get();
    const auto* b = right.try_get();
    if (a && b)
        return static_cast<Result>(*a <=> *b);
    return static_cast<Result>(left.has_value() <=> right.has_value());
}

}
