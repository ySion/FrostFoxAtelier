#pragma once

#include "foundation/span.hpp"
#include "memory/fatal.hpp"

#include <mimalloc.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>
#include <memory>
#include <ranges>
#include <type_traits>
#include <utility>

namespace atelier {
template <class T>
class Array;
}

namespace atelier::detail {

template <class T, std::size_t N, bool Spill>
class InlineSequence;

template <class R, class T>
inline constexpr bool is_inline_sequence_of_v = false;

template <class T, std::size_t N, bool Spill>
inline constexpr bool is_inline_sequence_of_v<InlineSequence<T, N, Spill>, T> = true;

template <class R>
inline constexpr bool move_elements_from_range_v =
    !std::is_lvalue_reference_v<R> && !std::ranges::borrowed_range<std::remove_cvref_t<R>> &&
    !std::ranges::view<std::remove_cvref_t<R>> &&
    std::is_lvalue_reference_v<std::ranges::range_reference_t<R>> &&
    !std::is_const_v<std::remove_reference_t<std::ranges::range_reference_t<R>>>;

template <class R>
using RangeElementSource = std::conditional_t<move_elements_from_range_v<R>,
                                              std::remove_reference_t<std::ranges::range_reference_t<R>>&&,
                                              std::ranges::range_reference_t<R>>;

template <class U>
concept ZeroBitCastAggregate =
    requires { typename std::bool_constant<(std::bit_cast<std::array<std::uint8_t, sizeof(U)>>(U{}) ==
                                            std::array<std::uint8_t, sizeof(U)>{})>; };

// Raw-storage operations shared by heap and inline sequences. Element operations
// are ordinary C++ operations, never a recoverable business-error channel.
template <class T>
struct ArrayElements {
    static constexpr std::size_t max_size = static_cast<std::size_t>(PTRDIFF_MAX) / sizeof(T);

    static constexpr bool byte_copy = std::is_trivially_copyable_v<T> && std::is_trivially_copy_constructible_v<T>;

    using RelocationSource = decltype(std::move_if_noexcept(std::declval<T&>()));

    static constexpr bool byte_relocate =
        std::is_trivially_copyable_v<T> && std::is_trivially_constructible_v<T, RelocationSource>;

    // realloc implicitly creates objects in copied bytes; relocation must also be trivial.
    static constexpr bool byte_reallocate = std::is_implicit_lifetime_v<T> && byte_relocate;

    static constexpr bool no_padding_bytes = span_no_padding_v<T>;

    static constexpr bool zero_default_init = [] {
        if constexpr (!std::is_implicit_lifetime_v<T> || !std::is_trivially_default_constructible_v<T> ||
                      !std::is_trivially_copyable_v<T> || std::is_array_v<T> || std::is_member_pointer_v<T>)
            return false;
        else if constexpr (std::is_scalar_v<T> || std::same_as<T, std::byte>)
            return true;
        else if constexpr (std::has_unique_object_representations_v<T>)
            return ZeroBitCastAggregate<T>;
        else
            return false;
    }();

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

    static void deallocate(T* data) noexcept {
        if (data)
            mi_free(data);
    }

    static void destroy(T* data, std::size_t size) noexcept {
        if constexpr (!std::is_trivially_destructible_v<T>)
            while (size)
                std::destroy_at(data + --size);
    }

    static void default_construct(T* destination, std::size_t count) noexcept {
        if (!count)
            return;
        if constexpr (zero_default_init) {
            std::memset(destination, 0, count * sizeof(T));
        } else {
            for (std::size_t index = 0; index != count; ++index)
                std::construct_at(destination + index);
        }
    }

    static void fill_construct(T* destination, std::size_t count, const T& value) noexcept {
        if (!count)
            return;
        if constexpr (byte_copy && std::is_implicit_lifetime_v<T> && no_padding_bytes &&
                      (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8 || sizeof(T) == 16)) {
            simd_fill_trivial(destination, count, value);
            return;
        }
        for (std::size_t index = 0; index != count; ++index)
            std::construct_at(destination + index, value);
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
