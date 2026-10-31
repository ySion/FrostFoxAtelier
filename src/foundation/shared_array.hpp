#pragma once

#include "foundation/array.hpp"
#include "foundation/string.hpp"

#include <atomic>
#include <concepts>
#include <type_traits>
#include <utility>

namespace atelier {

// Shared contents cross threads outside GC roots and barriers. Runtime reference
// values opt out with a nested `is_runtime_reference` type.
template <class T>
concept SharedElement =
    std::same_as<T, std::remove_cv_t<T>> && std::is_copy_constructible_v<T> &&
    (std::is_trivially_copyable_v<T> || std::same_as<T, String>) && !requires { typename T::is_runtime_reference; };

// A counted handle to one immutable Array in a heap block. Copies share the block;
// write() makes this handle the block's only owner before returning its Array.
template <SharedElement T>
class SharedArray {
public:
    using Value = T;

    SharedArray() noexcept = default;
    explicit SharedArray(Array<T>&& values) : block_(new Block{std::move(values)}) {}
    SharedArray(const SharedArray& other) noexcept : block_(other.block_) {
        if (block_)
            block_->count.fetch_add(1, std::memory_order_relaxed);
    }
    SharedArray(SharedArray&& other) noexcept : block_(std::exchange(other.block_, nullptr)) {}
    SharedArray& operator=(const SharedArray& other) noexcept {
        SharedArray copy{other};
        swap(copy);
        return *this;
    }
    SharedArray& operator=(SharedArray&& other) noexcept {
        SharedArray incoming{std::move(other)};
        swap(incoming);
        return *this;
    }
    ~SharedArray() { release(block_); }

    [[nodiscard]] const Array<T>& array() const& noexcept { return block_ ? block_->values : none_; }
    const Array<T>& array() const&& = delete;
    [[nodiscard]] Span<const T> view() const& noexcept { return array().view(); }
    Span<const T> view() const&& = delete;
    [[nodiscard]] std::size_t size() const noexcept { return array().size(); }
    [[nodiscard]] bool empty() const noexcept { return array().empty(); }

    // Copies the contents first unless this handle is the only owner; a failed copy
    // leaves the handle unchanged. The reference is valid until this handle is next
    // copied, assigned, moved or destroyed: a copy made while it is in use would see
    // later writes.
    Array<T>& write() & {
        // Acquire pairs with the release decrements of owners dropped on other threads.
        if (block_ && block_->count.load(std::memory_order_acquire) == 1)
            return block_->values;
        auto* unique = new Block{array()};
        release(std::exchange(block_, unique));
        return block_->values;
    }
    Array<T>& write() && = delete;

    void swap(SharedArray& other) noexcept { std::swap(block_, other.block_); }
    // Whether both handles share one block, or neither has one.
    friend bool same(const SharedArray& a, const SharedArray& b) noexcept { return a.block_ == b.block_; }

private:
    struct Block {
        explicit Block(Array<T>&& adopted) noexcept : values(std::move(adopted)) {}
        explicit Block(const Array<T>& copied) : values(copied) {}
        std::atomic<std::size_t> count{1};
        Array<T> values;
    };
    static void release(Block* block) noexcept {
        if (block && block->count.fetch_sub(1, std::memory_order_release) == 1) {
            std::atomic_thread_fence(std::memory_order_acquire);
            delete block;
        }
    }

    static constinit inline const Array<T> none_{};
    Block* block_{};
};

}
