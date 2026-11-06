#pragma once

#include <cstddef>
#include <cstring>
#include <memory>
#include <type_traits>
#include <utility>

namespace atelier::detail {

// Raw-storage operations shared by heap and inline sequences. The caller owns
// the live-prefix count and cleanup if an element constructor fails.
template <class T>
struct ArrayElements {
    static constexpr bool byte_copy = std::is_trivially_copyable_v<T> && std::is_trivially_copy_constructible_v<T>;

    using RelocationSource = decltype(std::move_if_noexcept(std::declval<T&>()));

    static constexpr bool byte_relocate =
        std::is_trivially_copyable_v<T> && std::is_trivially_constructible_v<T, RelocationSource>;

    static void destroy(T* data, std::size_t size) noexcept {
        if constexpr (!std::is_trivially_destructible_v<T>)
            while (size)
                std::destroy_at(data + --size);
    }

    static void copy_construct(T* destination, const T* source, std::size_t count, std::size_t& constructed) {
        if constexpr (byte_copy) {
            if (count)
                std::memcpy(destination, source, count * sizeof(T));
            constructed = count;
        } else {
            for (; constructed != count; ++constructed)
                std::construct_at(destination + constructed, source[constructed]);
        }
    }

    static void relocate(T* destination, T* source, std::size_t count, std::size_t& constructed) {
        if constexpr (byte_relocate) {
            if (count)
                std::memcpy(destination, source, count * sizeof(T));
            constructed = count;
        } else {
            for (; constructed != count; ++constructed)
                std::construct_at(destination + constructed, std::move_if_noexcept(source[constructed]));
        }
    }
};

}
