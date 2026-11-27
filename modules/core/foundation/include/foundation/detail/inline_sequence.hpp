#pragma once

#include "foundation/detail/array_sequence.hpp"

#include <initializer_list>

namespace atelier::detail {

struct InlineHeap {
    void* data{};

    std::size_t capacity{};
};

struct NoInlineHeap {};

// Fixed and spillable sequences share all element operations. The fixed policy
// has no pointer or heap capacity; only the live prefix of the inline bytes is T.
template <class T, std::size_t N, bool Spill>
class InlineSequence : public ArraySequence<InlineSequence<T, N, Spill>, T> {
    friend class ArraySequence<InlineSequence<T, N, Spill>, T>;

    friend class ::atelier::Array<T>;

    template <class, std::size_t, bool>
    friend class InlineSequence;

    using Elements = ArrayElements<T>;

    static_assert(std::is_object_v<T> && !std::is_const_v<T> && !std::is_volatile_v<T>);

    static_assert(std::is_nothrow_destructible_v<T>);

    static_assert(std::is_nothrow_move_constructible_v<T> || std::is_copy_constructible_v<T>);

    static_assert(N <= Elements::max_size);

public:
    static constexpr std::size_t max_size = Spill ? Elements::max_size : N;

    InlineSequence() noexcept = default;

    explicit InlineSequence(std::size_t count) noexcept
        requires std::default_initializable<T>
    {
        if (count) {
            reserve(count);
            Elements::default_construct(raw_data(), count);
            size_ = count;
        }
    }

    InlineSequence(std::size_t count, const T& value) noexcept
        requires std::is_copy_constructible_v<T>
    {
        if (count) {
            reserve(count);
            Elements::fill_construct(raw_data(), count, value);
            size_ = count;
        }
    }

    explicit InlineSequence(Span<const T> values) noexcept
        requires std::is_copy_constructible_v<T>
    {
        if (!values.empty()) {
            reserve(values.size());
            Elements::copy_construct(raw_data(), values.data(), values.size());
            size_ = values.size();
        }
    }

    template <std::input_iterator It, std::sentinel_for<It> Sentinel>
        requires std::constructible_from<T, std::iter_reference_t<It>>
    InlineSequence(It first, Sentinel last) noexcept {
        if constexpr (std::contiguous_iterator<It> && std::sized_sentinel_for<Sentinel, It> &&
                      std::same_as<std::remove_cvref_t<std::iter_reference_t<It>>, T> &&
                      std::is_copy_constructible_v<T>) {
            const Span<const T> values{first, last};
            if (!values.empty()) {
                reserve(values.size());
                Elements::copy_construct(raw_data(), values.data(), values.size());
                size_ = values.size();
            }
        } else if constexpr (std::sized_sentinel_for<Sentinel, It>) {
            const auto distance = last - first;
            if (distance < 0)
                fatal("Invalid Array range.");
            const auto count = static_cast<std::size_t>(distance);
            if (count) {
                reserve(count);
                auto* destination = raw_data();
                for (std::size_t index = 0; index != count; ++index, ++first)
                    std::construct_at(destination + index, *first);
                size_ = count;
            }
        } else {
            for (; first != last; ++first)
                this->emplace(*first);
        }
    }

    template <std::size_t M, bool OtherSpill>
        requires(M != N || OtherSpill != Spill)
    explicit InlineSequence(InlineSequence<T, M, OtherSpill>&& other) noexcept {
        if constexpr (Spill && OtherSpill) {
            if (other.heap_.data && other.size_ > N) {
                heap_ = std::exchange(other.heap_, {});
                size_ = std::exchange(other.size_, 0);
                return;
            }
        }
        if (other.size_) {
            reserve(other.size_);
            Elements::relocate(raw_data(), other.data(), other.size_);
            size_ = other.size_;
            other.clear();
        }
    }

    template <std::ranges::input_range R>
        requires(!std::same_as<std::remove_cvref_t<R>, InlineSequence>) &&
                (!std::same_as<std::remove_cvref_t<R>, Span<const T>>) && (!std::same_as<std::remove_cvref_t<R>, T>) &&
                (!is_inline_sequence_of_v<std::remove_cvref_t<R>, T> || std::is_lvalue_reference_v<R> ||
                 std::is_const_v<std::remove_reference_t<R>>) &&
                std::constructible_from<T, RangeElementSource<R>>
    explicit InlineSequence(R&& values) noexcept {
        if constexpr (Spill && move_elements_from_range_v<R> &&
                      std::same_as<std::remove_cvref_t<R>, ::atelier::Array<T>>) {
            if (values.size_ > N) {
                heap_ = {std::exchange(values.data_, nullptr), std::exchange(values.capacity_, 0)};
                size_ = std::exchange(values.size_, 0);
                return;
            }
        }
        if constexpr (!move_elements_from_range_v<R> && std::ranges::contiguous_range<R> &&
                      std::ranges::sized_range<R> &&
                      std::same_as<std::remove_cvref_t<std::ranges::range_reference_t<R>>, T> &&
                      std::is_copy_constructible_v<T>) {
            const auto count = static_cast<std::size_t>(std::ranges::size(values));
            if (count) {
                reserve(count);
                Elements::copy_construct(raw_data(), std::ranges::data(values), count);
                size_ = count;
            }
        } else if constexpr (std::ranges::sized_range<R>) {
            const auto count = static_cast<std::size_t>(std::ranges::size(values));
            if (count) {
                reserve(count);
                auto* destination = raw_data();
                std::size_t index = 0;
                for (auto&& element : values) {
                    if constexpr (move_elements_from_range_v<R>)
                        std::construct_at(destination + index++, std::move(element));
                    else
                        std::construct_at(destination + index++, std::forward<decltype(element)>(element));
                }
                size_ = count;
            }
        } else {
            for (auto&& element : values) {
                if constexpr (move_elements_from_range_v<R>)
                    this->emplace(std::move(element));
                else
                    this->emplace(std::forward<decltype(element)>(element));
            }
        }
        if constexpr (move_elements_from_range_v<R> && requires { values.clear(); })
            values.clear();
    }

    InlineSequence(std::initializer_list<T> values) noexcept
        requires std::is_copy_constructible_v<T>
        : InlineSequence(Span<const T>{values.begin(), values.size()}) {}

    InlineSequence(const InlineSequence& other) noexcept
        requires std::is_copy_constructible_v<T>
        : InlineSequence(other.view()) {}

    InlineSequence(InlineSequence&& other) noexcept { take_from(other); }

    InlineSequence& operator=(const InlineSequence& other) noexcept
        requires std::is_copy_constructible_v<T>
    {
        if (this == &other)
            return *this;
        if constexpr (Elements::byte_copy && std::is_trivially_copy_assignable_v<T>) {
            if (other.size_ <= capacity()) {
                if (other.size_)
                    std::memcpy(raw_data(), other.data(), other.size_ * sizeof(T));
                size_ = other.size_;
                return *this;
            }
        }
        InlineSequence incoming(other);
        release();
        take_from(incoming);
        return *this;
    }

    InlineSequence& operator=(InlineSequence&& other) noexcept {
        if (this != &other) {
            // Detach before destroying an element that might own `other`.
            InlineSequence incoming(std::move(other));
            release();
            take_from(incoming);
        }
        return *this;
    }

    ~InlineSequence() { release(); }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    [[nodiscard]] std::size_t capacity() const noexcept {
        if constexpr (Spill)
            if (heap_.data)
                return heap_.capacity;
        return N;
    }

    [[nodiscard]] T* data() noexcept [[clang::lifetimebound]] {
        if constexpr (Spill)
            if (heap_.data)
                return static_cast<T*>(heap_.data);
        if constexpr (N > 0 && std::is_implicit_lifetime_v<T> && std::is_trivially_destructible_v<T>)
            return std::launder(reinterpret_cast<T*>(inline_));
        else
            return size_ ? std::launder(reinterpret_cast<T*>(inline_)) : reinterpret_cast<T*>(inline_);
    }

    [[nodiscard]] const T* data() const noexcept [[clang::lifetimebound]] {
        return const_cast<InlineSequence*>(this)->data();
    }

    void reserve(std::size_t count) noexcept {
        if (count > capacity())
            reallocate(count);
    }

    void squeeze() noexcept {
        if constexpr (Spill)
            if (heap_.data && size_ < heap_.capacity)
                reallocate(size_);
    }

    void release() noexcept {
        this->clear();
        if constexpr (Spill) {
            Elements::deallocate(static_cast<T*>(heap_.data));
            heap_ = {};
        }
    }

    void swap(InlineSequence& other) noexcept {
        if (this == &other)
            return;
        if constexpr (Spill) {
            if (heap_.data && other.heap_.data) {
                std::swap(heap_, other.heap_);
                std::swap(size_, other.size_);
                return;
            }
            if (heap_.data || other.heap_.data) {
                auto& heap_seq = heap_.data ? *this : other;
                auto& inline_seq = heap_.data ? other : *this;
                const auto saved_heap = std::exchange(heap_seq.heap_, {});
                const auto saved_heap_size = std::exchange(heap_seq.size_, 0);
                const auto inline_count = inline_seq.size_;
                Elements::relocate(heap_seq.raw_inline(), inline_seq.data(), inline_count);
                heap_seq.size_ = inline_count;
                inline_seq.clear();
                inline_seq.heap_ = saved_heap;
                inline_seq.size_ = saved_heap_size;
                return;
            }
        }
        if constexpr (std::is_nothrow_swappable_v<T>) {
            const auto common = std::min(size_, other.size_);
            for (std::size_t index = 0; index != common; ++index)
                std::ranges::swap(data()[index], other.data()[index]);
            if (size_ > other.size_) {
                const auto tail = size_ - common;
                Elements::relocate(other.raw_inline() + common, data() + common, tail);
                Elements::destroy(data() + common, tail);
            } else if (other.size_ > size_) {
                const auto tail = other.size_ - common;
                Elements::relocate(raw_inline() + common, other.data() + common, tail);
                Elements::destroy(other.data() + common, tail);
            }
            std::swap(size_, other.size_);
        } else {
            InlineSequence temporary(std::move(other));
            other = std::move(*this);
            *this = std::move(temporary);
        }
    }

    friend void swap(InlineSequence& left, InlineSequence& right) noexcept { left.swap(right); }

private:
    [[nodiscard]] T* raw_inline() noexcept { return reinterpret_cast<T*>(inline_); }

    [[nodiscard]] T* raw_data() noexcept {
        if constexpr (Spill)
            if (heap_.data)
                return static_cast<T*>(heap_.data);
        return raw_inline();
    }

    [[nodiscard]] std::size_t grown_capacity(std::size_t extra) const noexcept {
        return Elements::grown_capacity(size_, capacity(), extra, max_size);
    }

    void take_from(InlineSequence& other) noexcept {
        if constexpr (Spill) {
            if (other.heap_.data) {
                heap_ = std::exchange(other.heap_, {});
                size_ = std::exchange(other.size_, 0);
                return;
            }
        }
        Elements::relocate(raw_inline(), other.data(), other.size_);
        size_ = other.size_;
        other.clear();
    }

    void reallocate(std::size_t count) noexcept {
        if constexpr (Spill) {
            if constexpr (Elements::byte_reallocate) {
                if (heap_.data && count > N) {
                    heap_.data = Elements::reallocate(static_cast<T*>(heap_.data), count);
                    heap_.capacity = count;
                    return;
                }
            }
            auto* replacement = count <= N ? raw_inline() : Elements::allocate(count);
            Elements::relocate(replacement, data(), size_);
            Elements::destroy(data(), size_);
            Elements::deallocate(static_cast<T*>(heap_.data));
            heap_ = count <= N ? InlineHeap{} : InlineHeap{replacement, count};
        } else
            fatal("Fixed array cannot allocate.");
    }

    template <class Construct>
    void extend(std::size_t count, Construct construct) noexcept {
        const auto capacity = grown_capacity(count);
        if (!count)
            return;
        const bool growing = size_ + count > this->capacity();
        T* destination = Elements::extend(raw_data(), size_, count, capacity, growing, construct);
        if (growing) {
            Elements::destroy(data(), size_);
            if constexpr (Spill) {
                Elements::deallocate(static_cast<T*>(heap_.data));
                heap_ = {destination, capacity};
            }
        }
        size_ += count;
    }

    std::size_t size_{};

    [[msvc::no_unique_address]] std::conditional_t<Spill, InlineHeap, NoInlineHeap> heap_{};

    alignas(T) std::byte inline_[N ? sizeof(T) * N : 1];
};

}
