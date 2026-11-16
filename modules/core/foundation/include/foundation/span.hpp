#pragma once

#include "memory/fatal.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

namespace atelier {

// Synchronous borrow. Constness belongs to T, not to the descriptor.
template <class T>
class Span {
    static_assert(std::is_object_v<T> && !std::is_volatile_v<T>);

public:
    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

    constexpr Span() noexcept = default;

    constexpr Span(T* data, std::size_t size) noexcept : data_(data), size_(size) {
        if ((!data && size) || size > static_cast<std::size_t>(PTRDIFF_MAX) / sizeof(T))
            fatal("Invalid Span storage.");
    }

    template <std::size_t N>
    constexpr Span(T (&data [[clang::lifetimebound]])[N]) noexcept : data_(data), size_(N) {}

    explicit constexpr Span(std::span<T> values) noexcept : Span(values.data(), values.size()) {}

    template <class U>
        requires std::is_convertible_v<U (*)[], T (*)[]>
    constexpr Span(Span<U> other) noexcept : data_(other.data()), size_(other.size()) {}

    [[nodiscard]] constexpr T* data() const noexcept { return data_; }

    [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }

    [[nodiscard]] constexpr std::size_t size_bytes() const noexcept { return size_ * sizeof(T); }

    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }

    [[nodiscard]] constexpr T* begin() const noexcept { return data_; }

    [[nodiscard]] constexpr T* end() const noexcept { return data_ + size_; }

    [[nodiscard]] constexpr T& operator[](std::size_t index) const noexcept {
        if (index >= size_)
            fatal("Span index.");
        return data_[index];
    }

    [[nodiscard]] constexpr T& front() const noexcept { return (*this)[0]; }

    [[nodiscard]] constexpr T& back() const noexcept { return (*this)[size_ - 1]; }

    [[nodiscard]] constexpr T* try_at(std::size_t index) const noexcept {
        return index < size_ ? data_ + index : nullptr;
    }

    [[nodiscard]] constexpr Span first(std::size_t count) const noexcept {
        if (count > size_)
            fatal("Span slice.");
        return {Unchecked{}, data_, count};
    }

    [[nodiscard]] constexpr Span last(std::size_t count) const noexcept {
        if (count > size_)
            fatal("Span slice.");
        return {Unchecked{}, data_ + (size_ - count), count};
    }

    [[nodiscard]] constexpr Span slice(std::size_t first, std::size_t count = npos) const noexcept {
        if (first > size_ || (count != npos && count > size_ - first))
            fatal("Span slice.");
        return {Unchecked{}, data_ + first, count == npos ? size_ - first : count};
    }

    [[nodiscard]] constexpr std::span<T> std_span() const noexcept { return {data_, size_}; }

private:
    struct Unchecked {};

    constexpr Span(Unchecked, T* data, std::size_t size) noexcept : data_(data), size_(size) {}

    T* data_{};

    std::size_t size_{};
};

template <class T>
[[nodiscard]] Span<const std::byte> as_bytes(Span<T> values) noexcept {
    return {reinterpret_cast<const std::byte*>(values.data()), values.size_bytes()};
}

template <class T>
    requires(!std::is_const_v<T>)
[[nodiscard]] Span<std::byte> as_writable_bytes(Span<T> values) noexcept {
    return {reinterpret_cast<std::byte*>(values.data()), values.size_bytes()};
}

}
