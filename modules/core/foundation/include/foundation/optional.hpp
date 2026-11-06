#pragma once

#include "memory/fatal.hpp"

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

public:
    Optional() noexcept = default;

    Optional(std::nullopt_t) noexcept {}

    template <class... Args>
        requires std::is_nothrow_constructible_v<T, Args...>
    explicit Optional(std::in_place_t, Args&&... arguments) noexcept {
        emplace(std::forward<Args>(arguments)...);
    }

    explicit Optional(const T& value) noexcept
        requires std::is_nothrow_copy_constructible_v<T>
        : Optional(std::in_place, value) {}

    explicit Optional(T&& value) noexcept
        requires std::is_nothrow_move_constructible_v<T>
        : Optional(std::in_place, std::move(value)) {}

    Optional(const Optional& other) noexcept
        requires std::is_nothrow_copy_constructible_v<T>
    {
        if (other.engaged_)
            emplace(*other.pointer());
    }

    Optional(Optional&& other) noexcept
        requires std::is_nothrow_move_constructible_v<T>
    {
        if (other.engaged_)
            emplace(std::move(*other.pointer()));
    }

    Optional& operator=(const Optional& other) noexcept
        requires(std::is_nothrow_copy_constructible_v<T> && std::is_nothrow_copy_assignable_v<T>)
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

    Optional& operator=(Optional&& other) noexcept
        requires(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T>)
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

    Optional& operator=(std::nullopt_t) noexcept {
        reset();
        return *this;
    }

    ~Optional() { reset(); }

    [[nodiscard]] bool has_value() const noexcept { return engaged_ != 0; }

    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] T* try_get() & noexcept { return engaged_ ? pointer() : nullptr; }

    [[nodiscard]] const T* try_get() const& noexcept { return engaged_ ? pointer() : nullptr; }

    T* try_get() && = delete;

    const T* try_get() const&& = delete;

    [[nodiscard]] T& operator*() & noexcept { return *operator->(); }

    [[nodiscard]] const T& operator*() const& noexcept { return *operator->(); }

    T& operator*() && = delete;

    const T& operator*() const&& = delete;

    [[nodiscard]] T* operator->() & noexcept {
        if (!engaged_)
            fatal("Empty Optional access.");
        return pointer();
    }

    [[nodiscard]] const T* operator->() const& noexcept {
        if (!engaged_)
            fatal("Empty Optional access.");
        return pointer();
    }

    T* operator->() && = delete;

    const T* operator->() const&& = delete;

    void reset() noexcept {
        if (engaged_) {
            std::destroy_at(pointer());
            engaged_ = 0;
        }
    }

    // Like optional emplacement, this ends the old value before constructing the
    // replacement. Arguments must not borrow that value. No operation may reenter.
    template <class... Args>
        requires std::is_nothrow_constructible_v<T, Args...>
    T& emplace(Args&&... arguments) noexcept {
        reset();
        auto* value = std::construct_at(reinterpret_cast<T*>(storage_), std::forward<Args>(arguments)...);
        engaged_ = 1;
        return *value;
    }

    void swap(Optional& other) noexcept
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

    friend void swap(Optional& left, Optional& right) noexcept
        requires(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_swappable_v<T>)
    {
        left.swap(right);
    }

private:
    [[nodiscard]] T* pointer() noexcept { return std::launder(reinterpret_cast<T*>(storage_)); }

    [[nodiscard]] const T* pointer() const noexcept { return std::launder(reinterpret_cast<const T*>(storage_)); }

    alignas(T) std::byte storage_[sizeof(T)];

    std::uint8_t engaged_{};
};

}
