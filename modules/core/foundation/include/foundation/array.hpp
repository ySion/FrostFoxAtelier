#pragma once

#include "foundation/detail/array_sequence.hpp"

#include <cstring>
#include <initializer_list>
#include <type_traits>
#include <utility>

namespace atelier {

// Unique contiguous storage. Byte paths require a trivially copyable type and
// the exact selected operation to be trivial; resources retain typed lifetimes.
template <class T>
class Array : public detail::ArraySequence<Array<T>, T> {
    friend class detail::ArraySequence<Array<T>, T>;

    using Elements = detail::ArrayElements<T>;

    static_assert(std::is_object_v<T> && !std::is_const_v<T> && !std::is_volatile_v<T>);

    static_assert(std::is_nothrow_destructible_v<T>);

    static_assert(std::is_nothrow_move_constructible_v<T> || std::is_copy_constructible_v<T>,
                  "Array elements require a non-throwing move constructor or a copy constructor.");

public:
    static constexpr std::size_t max_size = Elements::max_size;

    Array() noexcept = default;

    explicit Array(Span<const T> values) noexcept
        requires std::is_copy_constructible_v<T>
    {
        if (!values.empty()) {
            data_ = Elements::allocate(values.size());
            capacity_ = values.size();
            Elements::copy_construct(data_, values.data(), values.size());
            size_ = values.size();
        }
    }

    Array(std::initializer_list<T> values) noexcept
        requires std::is_copy_constructible_v<T>
        : Array(Span<const T>{values.begin(), values.size()}) {}

    Array(const Array& other) noexcept
        requires std::is_copy_constructible_v<T>
        : Array(other.view()) {}

    Array& operator=(const Array& other) noexcept
        requires std::is_copy_constructible_v<T>
    {
        if (this == &other)
            return *this;
        if constexpr (Elements::byte_copy && std::is_trivially_copy_assignable_v<T>) {
            if (other.size_ <= capacity_) {
                // Live prefix assignment and raw tail construction are both
                // trivial and cannot fail. No resource callback is bypassed.
                if (other.size_)
                    std::memcpy(data_, other.data_, other.size_ * sizeof(T));
                size_ = other.size_;
                return *this;
            }
        }
        Array copy{other};
        swap(copy);
        return *this;
    }

    Array(Array&& other) noexcept { swap(other); }

    Array& operator=(Array&& other) noexcept {
        // An element can own the source container. Transfer its storage before
        // releasing our old elements, which may destroy that source object.
        if (this != &other) {
            Array incoming{std::move(other)};
            swap(incoming);
        }
        return *this;
    }

    ~Array() { release(); }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    [[nodiscard]] T* data() noexcept [[clang::lifetimebound]] { return data_; }

    [[nodiscard]] const T* data() const noexcept [[clang::lifetimebound]] { return data_; }

    // Does not construct elements. clear retains allocation; release frees it.
    void reserve(std::size_t capacity) noexcept {
        if (capacity > capacity_)
            reallocate(capacity);
    }

    void squeeze() noexcept {
        if (size_ != capacity_)
            reallocate(size_);
    }

    void release() noexcept {
        this->clear();
        Elements::deallocate(data_);
        data_ = nullptr;
        capacity_ = 0;
    }

    void swap(Array& other) noexcept {
        std::swap(data_, other.data_);
        std::swap(size_, other.size_);
        std::swap(capacity_, other.capacity_);
    }

private:
    [[nodiscard]] std::size_t grown_capacity(std::size_t extra) const noexcept {
        return Elements::grown_capacity(size_, capacity_, extra);
    }

    void reallocate(std::size_t capacity) noexcept {
        if constexpr (Elements::byte_reallocate) {
            data_ = Elements::reallocate(data_, capacity);
            capacity_ = capacity;
            return;
        }
        T* replacement = Elements::allocate(capacity);
        Elements::relocate(replacement, data_, size_);
        Elements::destroy(data_, size_);
        Elements::deallocate(data_);
        data_ = replacement;
        capacity_ = capacity;
    }

    template <class Construct>
    void extend(std::size_t count, Construct construct) noexcept {
        const auto capacity = grown_capacity(count);
        if (!count)
            return;
        const auto total = size_ + count;
        const bool growing = total > capacity_;
        T* destination = Elements::extend(data_, size_, count, capacity, growing, construct);
        if (growing) {
            Elements::destroy(data_, size_);
            Elements::deallocate(data_);
            data_ = destination;
            capacity_ = capacity;
        }
        size_ = total;
    }

    T* data_{};

    std::size_t size_{};

    std::size_t capacity_{};
};

}
