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

    template <class, std::size_t, bool>
    friend class detail::InlineSequence;

    using Elements = detail::ArrayElements<T>;

    static_assert(std::is_object_v<T> && !std::is_const_v<T> && !std::is_volatile_v<T>);

public:
    static constexpr std::size_t max_size = Elements::max_size;

    Array() noexcept = default;

    template <class U = T>
        requires std::same_as<U, T> && std::default_initializable<U>
    explicit Array(std::size_t count) noexcept {
        if (count) {
            data_ = Elements::allocate(count);
            capacity_ = count;
            Elements::default_construct(data_, count);
            size_ = count;
        }
    }

    template <class U = T>
        requires std::same_as<U, T> && std::is_copy_constructible_v<U>
    Array(std::size_t count, const T& value) noexcept {
        if (count) {
            data_ = Elements::allocate(count);
            capacity_ = count;
            Elements::fill_construct(data_, count, value);
            size_ = count;
        }
    }

    template <class U = T>
        requires std::same_as<U, T> && std::is_copy_constructible_v<U>
    explicit Array(Span<const T> values) noexcept {
        if (!values.empty()) {
            data_ = Elements::allocate(values.size());
            capacity_ = values.size();
            Elements::copy_construct(data_, values.data(), values.size());
            size_ = values.size();
        }
    }

    template <std::input_iterator It, std::sentinel_for<It> Sentinel>
        requires std::constructible_from<T, std::iter_reference_t<It>>
    Array(It first, Sentinel last) noexcept {
        if constexpr (std::contiguous_iterator<It> && std::sized_sentinel_for<Sentinel, It> &&
                      std::same_as<std::remove_cvref_t<std::iter_reference_t<It>>, T> &&
                      std::is_copy_constructible_v<T>) {
            const Span<const T> values{first, last};
            if (!values.empty()) {
                data_ = Elements::allocate(values.size());
                capacity_ = values.size();
                Elements::copy_construct(data_, values.data(), values.size());
                size_ = values.size();
            }
        } else if constexpr (std::sized_sentinel_for<Sentinel, It>) {
            const auto distance = last - first;
            if (distance < 0)
                fatal("Invalid Array range.");
            const auto count = static_cast<std::size_t>(distance);
            if (count) {
                data_ = Elements::allocate(count);
                capacity_ = count;
                for (std::size_t index = 0; index != count; ++index, ++first)
                    std::construct_at(data_ + index, *first);
                size_ = count;
            }
        } else {
            for (; first != last; ++first)
                this->emplace(*first);
        }
    }

    template <std::size_t N, bool Spill>
    explicit Array(detail::InlineSequence<T, N, Spill>&& other) noexcept {
        if constexpr (Spill) {
            if (other.heap_.data) {
                data_ = static_cast<T*>(other.heap_.data);
                capacity_ = other.heap_.capacity;
                size_ = other.size_;
                other.heap_ = {};
                other.size_ = 0;
                return;
            }
        }
        if (other.size_) {
            data_ = Elements::allocate(other.size_);
            capacity_ = other.size_;
            Elements::relocate(data_, other.data(), other.size_);
            size_ = other.size_;
            other.clear();
        }
    }

    template <std::ranges::input_range R>
        requires(!std::same_as<std::remove_cvref_t<R>, Array>) &&
                (!std::same_as<std::remove_cvref_t<R>, Span<const T>>) && (!std::same_as<std::remove_cvref_t<R>, T>) &&
                (!detail::is_inline_sequence_of_v<std::remove_cvref_t<R>, T> || std::is_lvalue_reference_v<R> ||
                 std::is_const_v<std::remove_reference_t<R>>) &&
                std::constructible_from<T, detail::RangeElementSource<R>>
    explicit Array(R&& values) noexcept {
        if constexpr (!detail::move_elements_from_range_v<R> && std::ranges::contiguous_range<R> &&
                      std::ranges::sized_range<R> &&
                      std::same_as<std::remove_cvref_t<std::ranges::range_reference_t<R>>, T> &&
                      std::is_copy_constructible_v<T>) {
            const auto count = static_cast<std::size_t>(std::ranges::size(values));
            if (count) {
                data_ = Elements::allocate(count);
                capacity_ = count;
                Elements::copy_construct(data_, std::ranges::data(values), count);
                size_ = count;
            }
        } else if constexpr (std::ranges::sized_range<R>) {
            const auto count = static_cast<std::size_t>(std::ranges::size(values));
            if (count) {
                data_ = Elements::allocate(count);
                capacity_ = count;
                std::size_t index = 0;
                for (auto&& element : values) {
                    if constexpr (detail::move_elements_from_range_v<R>)
                        std::construct_at(data_ + index++, std::move(element));
                    else
                        std::construct_at(data_ + index++, std::forward<decltype(element)>(element));
                }
                size_ = count;
            }
        } else {
            for (auto&& element : values) {
                if constexpr (detail::move_elements_from_range_v<R>)
                    this->emplace(std::move(element));
                else
                    this->emplace(std::forward<decltype(element)>(element));
            }
        }
        if constexpr (detail::move_elements_from_range_v<R> && requires { values.clear(); })
            values.clear();
    }

    template <class U = T>
        requires std::same_as<U, T> && std::is_copy_constructible_v<U>
    Array(std::initializer_list<T> values) noexcept : Array(Span<const T>{values.begin(), values.size()}) {}

    Array(const Array& other) noexcept : Array(other.view()) {}

    Array& operator=(const Array& other) noexcept {
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

    ~Array() {
        static_assert(std::is_nothrow_destructible_v<T>);
        static_assert(std::is_nothrow_move_constructible_v<T> || std::is_copy_constructible_v<T>,
                      "Array elements require a non-throwing move constructor or a copy constructor.");
        release();
    }

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

    friend void swap(Array& left, Array& right) noexcept { left.swap(right); }

private:
    [[nodiscard]] T* raw_data() noexcept { return data_; }

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
