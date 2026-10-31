#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace atelier {

// Synchronous borrow. Constness belongs to T, not to the descriptor.
template <class T>
class Span {
    static_assert(std::is_object_v<T> && !std::is_volatile_v<T>);

public:
    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

    constexpr Span() noexcept = default;
    constexpr Span(T* data, std::size_t size) : data_(data), size_(size) {
        if ((!data && size) || size > static_cast<std::size_t>(PTRDIFF_MAX) / sizeof(T))
            throw std::invalid_argument("Invalid Span storage.");
    }
    template <std::size_t N>
    constexpr Span(T (&data)[N]) noexcept : data_(data), size_(N) {}
    template <class U>
        requires std::is_convertible_v<U (*)[], T (*)[]>
    constexpr Span(Span<U> other) noexcept : data_(other.data()), size_(other.size()) {}

    [[nodiscard]] constexpr T* data() const noexcept { return data_; }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] constexpr T* begin() const noexcept { return data_; }
    [[nodiscard]] constexpr T* end() const noexcept { return size_ ? data_ + size_ : data_; }
    [[nodiscard]] constexpr T& operator[](std::size_t index) const {
        if (index >= size_)
            throw std::out_of_range("Span index.");
        return data_[index];
    }
    [[nodiscard]] constexpr Span slice(std::size_t first, std::size_t count = npos) const {
        if (first > size_ || (count != npos && count > size_ - first))
            throw std::out_of_range("Span slice.");
        return {first ? data_ + first : data_, count == npos ? size_ - first : count};
    }

private:
    T* data_{};
    std::size_t size_{};
};

}
