#pragma once

#include "foundation/span.hpp"

#include <algorithm>
#include <concepts>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <type_traits>
#include <utility>

namespace atelier {

// Unique contiguous storage. Byte paths require a trivially copyable type and
// the exact selected operation to be trivial; resources retain typed lifetimes.
template<class T>
class Array {
    static_assert(std::is_object_v<T> && !std::is_const_v<T> && !std::is_volatile_v<T>);
    static_assert(std::is_nothrow_destructible_v<T>);
    static_assert(std::is_nothrow_move_constructible_v<T> || std::is_copy_constructible_v<T>,
        "Array elements require a non-throwing move constructor or a copy constructor.");
public:
    using Value = T;
    static constexpr std::size_t npos = Span<T>::npos;
    static constexpr std::size_t max_size = static_cast<std::size_t>(PTRDIFF_MAX) / sizeof(T);

    Array() noexcept = default;
    explicit Array(Span<const T> values) requires std::is_copy_constructible_v<T> { append(values); }
    Array(std::initializer_list<T> values) requires std::is_copy_constructible_v<T>
        : Array(Span<const T>{values.begin(), values.size()}) {}
    Array(const Array& other) requires std::is_copy_constructible_v<T> : Array(other.view()) {}
    Array& operator=(const Array& other) requires std::is_copy_constructible_v<T> {
        if (this == &other) return *this;
        if constexpr (byte_copy && std::is_trivially_copy_assignable_v<T>) {
            if (other.size_ <= capacity_) {
                // Live prefix assignment and raw tail construction are both
                // trivial and cannot fail. No resource callback is bypassed.
                if (other.size_) std::memcpy(data_, other.data_, other.size_ * sizeof(T));
                size_ = other.size_;
                return *this;
            }
        }
        Array copy{other}; swap(copy);
        return *this;
    }
    Array(Array&& other) noexcept { swap(other); }
    Array& operator=(Array&& other) noexcept {
        // An element can own the source container. Transfer its storage before
        // releasing our old elements, which may destroy that source object.
        if (this != &other) { Array incoming{std::move(other)}; swap(incoming); }
        return *this;
    }
    ~Array() { release(); }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] T* data() noexcept { return data_; }
    [[nodiscard]] const T* data() const noexcept { return data_; }
    [[nodiscard]] T* begin() noexcept { return data_; }
    [[nodiscard]] const T* begin() const noexcept { return data_; }
    [[nodiscard]] T* end() noexcept { return size_ ? data_ + size_ : data_; }
    [[nodiscard]] const T* end() const noexcept { return size_ ? data_ + size_ : data_; }
    [[nodiscard]] Span<T> view() & noexcept { return {data_, size_}; }
    [[nodiscard]] Span<const T> view() const & noexcept { return {data_, size_}; }
    Span<T> view() && = delete;
    Span<const T> view() const && = delete;
    [[nodiscard]] T& operator[](std::size_t index) { return Span<T>{data_, size_}[index]; }
    [[nodiscard]] const T& operator[](std::size_t index) const { return Span<const T>{data_, size_}[index]; }
    [[nodiscard]] T* try_at(std::size_t index) noexcept { return index < size_ ? data_ + index : nullptr; }
    [[nodiscard]] const T* try_at(std::size_t index) const noexcept { return index < size_ ? data_ + index : nullptr; }

    // Does not construct elements. clear retains allocation; release frees it.
    void reserve(std::size_t capacity) { if (capacity > capacity_) reallocate(capacity); }
    void squeeze() { if (size_ != capacity_) reallocate(size_); }
    void clear() noexcept { destroy(data_, size_); size_ = 0; }
    void release() noexcept {
        clear(); deallocate(data_, capacity_); data_ = nullptr; capacity_ = 0;
    }
    void swap(Array& other) noexcept {
        std::swap(data_, other.data_); std::swap(size_, other.size_); std::swap(capacity_, other.capacity_);
    }

    template<class... A> requires std::constructible_from<T, A...>
    T& emplace(A&&... arguments) {
        if (size_ < capacity_) {
            auto* destination = data_ + size_;
            std::construct_at(destination, std::forward<A>(arguments)...);
            ++size_;
            return *destination;
        }
        extend(1, [&](T* destination, std::size_t, std::size_t& constructed) {
            std::construct_at(destination, std::forward<A>(arguments)...);
            constructed = 1;
        });
        return data_[size_ - 1];
    }
    T& add(const T& value) requires std::is_copy_constructible_v<T> { return emplace(value); }
    T& add(T&& value) requires std::is_move_constructible_v<T> { return emplace(std::move(value)); }
    void append(Span<const T> values) requires std::is_copy_constructible_v<T> {
        // Construct the suffix before relocating old elements, including a self-view.
        extend(values.size(), [&](T* destination, std::size_t count, std::size_t& constructed) {
            copy_construct(destination, values.data(), count, constructed);
        });
    }
    void resize(std::size_t size) requires std::default_initializable<T> {
        if (size < size_) { destroy(data_ + size, size_ - size); size_ = size; }
        else extend(size - size_, [](T* destination, std::size_t count, std::size_t& constructed) {
            for (; constructed != count; ++constructed) std::construct_at(destination + constructed);
        });
    }
    void remove_at(std::size_t first, std::size_t count = 1) requires std::is_move_assignable_v<T> {
        check_range(first, count);
        if (!count) return;
        if constexpr (std::is_trivially_copyable_v<T> && std::is_trivially_move_assignable_v<T>) {
            const auto remaining = size_ - first - count;
            if (remaining) std::memmove(data_ + first, data_ + first + count, remaining * sizeof(T));
        } else {
            for (std::size_t i = first; i != size_ - count; ++i) data_[i] = std::move(data_[i + count]);
        }
        destroy(data_ + size_ - count, count); size_ -= count;
    }
    // Constant-time erase when order is irrelevant. Never shrinks capacity.
    void remove_at_swap(std::size_t index) requires std::is_move_assignable_v<T> {
        check_range(index, 1);
        if (index != size_ - 1) data_[index] = std::move(data_[size_ - 1]);
        std::destroy_at(data_ + --size_);
    }
    template<class Predicate> requires std::predicate<Predicate&, const T&> && std::is_move_assignable_v<T>
    std::size_t remove_if(Predicate predicate) {
        std::size_t write = 0;
        for (std::size_t read = 0; read != size_; ++read) {
            if (predicate(std::as_const(data_[read]))) continue;
            if (write != read) data_[write] = std::move(data_[read]);
            ++write;
        }
        const auto removed = size_ - write;
        if (removed) destroy(data_ + write, removed);
        size_ = write; return removed;
    }
    [[nodiscard]] std::size_t find(const T& value) const requires std::equality_comparable<T> {
        for (std::size_t i = 0; i != size_; ++i) if (data_[i] == value) return i;
        return npos;
    }
    [[nodiscard]] bool contains(const T& value) const requires std::equality_comparable<T> { return find(value) != npos; }

private:
    static constexpr bool byte_copy = std::is_trivially_copyable_v<T> && std::is_trivially_copy_constructible_v<T>;
    using RelocationSource = decltype(std::move_if_noexcept(std::declval<T&>()));
    static constexpr bool byte_relocate = std::is_trivially_copyable_v<T> && std::is_trivially_constructible_v<T, RelocationSource>;

    static void destroy(T* data, std::size_t size) noexcept {
        if constexpr (!std::is_trivially_destructible_v<T>) while (size) std::destroy_at(data + --size);
    }
    static void copy_construct(T* destination, const T* source, std::size_t count, std::size_t& constructed) {
        if constexpr (byte_copy) {
            if (count) std::memcpy(destination, source, count * sizeof(T));
            constructed = count;
        } else {
            for (; constructed != count; ++constructed) std::construct_at(destination + constructed, source[constructed]);
        }
    }
    static void relocate(T* destination, T* source, std::size_t count, std::size_t& constructed) {
        if constexpr (byte_relocate) {
            if (count) std::memcpy(destination, source, count * sizeof(T));
            constructed = count;
        } else {
            for (; constructed != count; ++constructed)
                std::construct_at(destination + constructed, std::move_if_noexcept(source[constructed]));
        }
    }
    static T* allocate(std::size_t count) {
        if (count > max_size) throw std::length_error("Array exceeds addressable storage.");
        return count ? std::allocator<T>{}.allocate(count) : nullptr;
    }
    static void deallocate(T* data, std::size_t count) noexcept { if (data) std::allocator<T>{}.deallocate(data, count); }
    void check_range(std::size_t first, std::size_t count) const {
        if (first > size_ || count > size_ - first) throw std::out_of_range("Array erase range.");
    }
    void reallocate(std::size_t capacity) {
        T* replacement = allocate(capacity);
        std::size_t constructed = 0;
        try {
            relocate(replacement, data_, size_, constructed);
        } catch (...) { destroy(replacement, constructed); deallocate(replacement, capacity); throw; }
        destroy(data_, size_); deallocate(data_, capacity_);
        data_ = replacement; capacity_ = capacity;
    }
    template<class Construct>
    void extend(std::size_t count, Construct construct) {
        if (count > max_size - size_) throw std::length_error("Array exceeds addressable storage.");
        if (!count) return;
        const auto total = size_ + count;
        const bool growing = total > capacity_;
        const auto capacity = growing ? std::max(total, capacity_ + std::min(max_size - capacity_, std::max(capacity_ / 2, std::size_t{8}))) : capacity_;
        T* destination = growing ? allocate(capacity) : data_;
        std::size_t suffix = 0, prefix = 0;
        try {
            construct(destination + size_, count, suffix);
            if (growing) relocate(destination, data_, size_, prefix);
        } catch (...) {
            destroy(destination + size_, suffix);
            if (growing) { destroy(destination, prefix); deallocate(destination, capacity); }
            throw;
        }
        if (growing) { destroy(data_, size_); deallocate(data_, capacity_); data_ = destination; capacity_ = capacity; }
        size_ = total;
    }

    T* data_{};
    std::size_t size_{};
    std::size_t capacity_{};
};

}
