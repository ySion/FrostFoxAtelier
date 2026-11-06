#pragma once

#include "memory/fatal.hpp"

#include <mimalloc.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>
#include <utility>

namespace atelier::detail {

// The pinned toolchain lacks is_implicit_lifetime_v. These sufficient conditions
// permit implicit object creation under [c.malloc] for realloc's copied bytes.
// Replace this conservative condition with that trait when the toolchain supplies it.
template <class T>
concept ReallocatableArrayElement =
    std::is_trivially_copyable_v<T> &&
    (std::is_aggregate_v<T> || std::is_trivially_default_constructible_v<T> ||
     std::is_trivially_copy_constructible_v<T> || std::is_trivially_move_constructible_v<T>);

// Raw-storage operations shared by heap and inline sequences. Element operations
// are ordinary C++ operations, never a recoverable business-error channel.
template <class T>
struct ArrayElements {
    static constexpr std::size_t max_size = static_cast<std::size_t>(PTRDIFF_MAX) / sizeof(T);

    static constexpr bool byte_copy = std::is_trivially_copyable_v<T> && std::is_trivially_copy_constructible_v<T>;

    using RelocationSource = decltype(std::move_if_noexcept(std::declval<T&>()));

    static constexpr bool byte_relocate =
        std::is_trivially_copyable_v<T> && std::is_trivially_constructible_v<T, RelocationSource>;

    // Implicit lifetime alone does not permit skipping a nontrivial selected constructor.
    static constexpr bool byte_reallocate = ReallocatableArrayElement<T> && byte_relocate;

    static std::size_t grown_capacity(std::size_t size, std::size_t capacity, std::size_t extra,
                                      std::size_t limit = max_size) noexcept {
        if (extra > limit - size)
            fatal("Array exceeds addressable storage.");
        const auto total = size + extra;
        return total > capacity
                   ? std::max(total, capacity + std::min(limit - capacity, std::max(capacity / 2, std::size_t{8})))
                   : capacity;
    }

    // Preserve a self-view across realloc; external views retain their pointer.
    static std::size_t alias_offset(const T* data, std::size_t size, const T* source, std::size_t count) noexcept {
        const auto address = reinterpret_cast<std::uintptr_t>(source);
        const auto begin = reinterpret_cast<std::uintptr_t>(data);
        if (address < begin || address - begin >= size * sizeof(T))
            return SIZE_MAX;
        const auto offset = (address - begin) / sizeof(T);
        if (count > size - offset)
            fatal("Array self-append range.");
        return offset;
    }

    // All buffers eligible for realloc originate here, with the same alignment
    // and zero offset. Zero capacity is a separate free, never realloc(p, 0).
    static T* allocate(std::size_t count) noexcept {
        if (count > max_size)
            fatal("Array exceeds addressable storage.");
        if (!count)
            return nullptr;
        auto* result = static_cast<T*>(mi_malloc_aligned(count * sizeof(T), alignof(T)));
        if (!result)
            fatal("Array allocation failed.");
        return result;
    }

    static T* reallocate(T* data, std::size_t count) noexcept
        requires byte_reallocate
    {
        if (count > max_size)
            fatal("Array exceeds addressable storage.");
        if (!count) {
            deallocate(data);
            return nullptr;
        }
        auto* result = static_cast<T*>(mi_realloc_aligned(data, count * sizeof(T), alignof(T)));
        if (!result)
            fatal("Array allocation failed.");
        return result;
    }

    static void deallocate(T* data) noexcept { mi_free(data); }

    static void destroy(T* data, std::size_t size) noexcept {
        if constexpr (!std::is_trivially_destructible_v<T>)
            while (size)
                std::destroy_at(data + --size);
    }

    static void copy_construct(T* destination, const T* source, std::size_t count) noexcept {
        if constexpr (byte_copy) {
            if (count)
                std::memcpy(destination, source, count * sizeof(T));
        } else {
            for (std::size_t index = 0; index != count; ++index)
                std::construct_at(destination + index, source[index]);
        }
    }

    static void relocate(T* destination, T* source, std::size_t count) noexcept {
        if constexpr (byte_relocate) {
            if (count)
                std::memcpy(destination, source, count * sizeof(T));
        } else {
            for (std::size_t index = 0; index != count; ++index)
                std::construct_at(destination + index, std::move_if_noexcept(source[index]));
        }
    }

    // Construct the suffix while aliased arguments still name the old storage.
    template <class Construct>
    static T* extend(T* source, std::size_t size, std::size_t count, std::size_t capacity, bool growing,
                     Construct construct) noexcept {
        T* destination = growing ? allocate(capacity) : source;
        construct(destination + size, count);
        if (growing)
            relocate(destination, source, size);
        return destination;
    }
};

}
