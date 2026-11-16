#pragma once

#include "memory/fatal.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace atelier {

// An inline value or absence, not an error channel. The payload is at offset zero,
// followed by one byte of engagement; empty storage contains no live T.
template <class T>
class Optional {
    static_assert(std::is_object_v<T> && !std::is_array_v<T> && !std::is_const_v<T> && !std::is_volatile_v<T>);

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
    constexpr Optional() noexcept = default;

    constexpr Optional(std::nullopt_t) noexcept {}

    template <class... Args>
        requires std::is_nothrow_constructible_v<T, Args...>
    explicit constexpr Optional(std::in_place_t, Args&&... arguments) noexcept
        : storage_(std::in_place, std::forward<Args>(arguments)...), engaged_(1) {}

    explicit constexpr Optional(const T& value) noexcept
        requires std::is_nothrow_copy_constructible_v<T>
        : Optional(std::in_place, value) {}

    explicit constexpr Optional(T&& value) noexcept
        requires std::is_nothrow_move_constructible_v<T>
        : Optional(std::in_place, std::move(value)) {}

    constexpr Optional(const Optional&) noexcept
        requires std::is_trivially_copy_constructible_v<T>
    = default;

    constexpr Optional(const Optional& other) noexcept
        requires(std::is_nothrow_copy_constructible_v<T> && !std::is_trivially_copy_constructible_v<T>)
    {
        if (other.engaged_)
            emplace(*other.pointer());
    }

    constexpr Optional(Optional&&) noexcept
        requires std::is_trivially_move_constructible_v<T>
    = default;

    constexpr Optional(Optional&& other) noexcept
        requires(std::is_nothrow_move_constructible_v<T> && !std::is_trivially_move_constructible_v<T>)
    {
        if (other.engaged_)
            emplace(std::move(*other.pointer()));
    }

    constexpr Optional& operator=(const Optional&) noexcept
        requires trivial_copy_assign
    = default;

    constexpr Optional& operator=(const Optional& other) noexcept
        requires(std::is_nothrow_copy_constructible_v<T> && std::is_nothrow_copy_assignable_v<T> &&
                 !trivial_copy_assign)
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
        requires(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T> &&
                 !trivial_move_assign)
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
        requires(std::is_nothrow_copy_constructible_v<T> && std::is_nothrow_constructible_v<T, U>)
    [[nodiscard]] constexpr T value_or(U&& fallback) const& noexcept {
        return engaged_ ? *pointer() : static_cast<T>(std::forward<U>(fallback));
    }

    template <class U = T>
        requires(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_constructible_v<T, U>)
    [[nodiscard]] constexpr T value_or(U&& fallback) && noexcept {
        return engaged_ ? std::move(*pointer()) : static_cast<T>(std::forward<U>(fallback));
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
        requires std::is_nothrow_constructible_v<T, Args...>
    constexpr T& emplace(Args&&... arguments) noexcept [[clang::lifetimebound]] {
        reset();
        auto* value = std::construct_at(std::addressof(storage_.value), std::forward<Args>(arguments)...);
        engaged_ = 1;
        return *value;
    }

    constexpr void swap(Optional& other) noexcept
        requires(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_swappable_v<T>)
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
        requires(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_swappable_v<T>)
    {
        left.swap(right);
    }

    friend constexpr bool operator==(const Optional& left, const Optional& right) noexcept
        requires requires(const T& a, const T& b) {
            { a == b } noexcept -> std::convertible_to<bool>;
        }
    {
        if (left.engaged_ != right.engaged_)
            return false;
        return !left.engaged_ || static_cast<bool>(*left.pointer() == *right.pointer());
    }

    friend constexpr bool operator==(const Optional& left, const T& right) noexcept
        requires requires(const T& a, const T& b) {
            { a == b } noexcept -> std::convertible_to<bool>;
        }
    {
        return left.engaged_ && static_cast<bool>(*left.pointer() == right);
    }

    friend constexpr bool operator==(const Optional& value, std::nullopt_t) noexcept { return !value.engaged_; }

private:
    [[nodiscard]] constexpr T* pointer() noexcept { return std::addressof(storage_.value); }

    [[nodiscard]] constexpr const T* pointer() const noexcept { return std::addressof(storage_.value); }

    Storage storage_{};

    std::uint8_t engaged_{};
};

}
