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

    using Elements = ArrayElements<T>;

    static_assert(std::is_object_v<T> && !std::is_const_v<T> && !std::is_volatile_v<T>);

    static_assert(std::is_nothrow_destructible_v<T>);

    static_assert(std::is_nothrow_move_constructible_v<T> || std::is_copy_constructible_v<T>);

    static_assert(N <= Elements::max_size);

public:
    static constexpr std::size_t max_size = Spill ? Elements::max_size : N;

    InlineSequence() noexcept = default;

    explicit InlineSequence(Span<const T> values) noexcept
        requires std::is_copy_constructible_v<T>
    {
        this->append(values);
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
                    std::memcpy(data(), other.data(), other.size_ * sizeof(T));
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
        return reinterpret_cast<T*>(inline_);
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
        }
        InlineSequence temporary(std::move(other));
        other = std::move(*this);
        *this = std::move(temporary);
    }

private:
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
        Elements::relocate(data(), other.data(), other.size_);
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
            auto* replacement = count <= N ? reinterpret_cast<T*>(inline_) : Elements::allocate(count);
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
        T* destination = Elements::extend(data(), size_, count, capacity, growing, construct);
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
