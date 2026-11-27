#pragma once

#include "foundation/span.hpp"
#include "foundation/string.hpp"
#include "memory/fatal.hpp"

#include <mimalloc.h>
#include <algorithm>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace atelier {

class ScratchScope;

// Linear bump allocator backed by reusable cache-line-aligned blocks from
// mi_malloc_aligned / mi_free. Only trivially destructible payloads may be
// allocated; ScratchScope and reset() rewind in O(blocks) without freeing
// retained block capacity.
class ScratchArena {
public:
    static constexpr std::size_t block_alignment = 64;

    static constexpr std::size_t minimum_block_bytes = 1024;

    explicit ScratchArena(std::size_t default_block_bytes = 64 * 1024) noexcept
        : default_block_bytes_(std::max(default_block_bytes, minimum_block_bytes)) {}

    ScratchArena(const ScratchArena&) = delete;

    ScratchArena& operator=(const ScratchArena&) = delete;

    ScratchArena(ScratchArena&& other) noexcept
        : first_(std::exchange(other.first_, nullptr)), current_(std::exchange(other.current_, nullptr)),
          default_block_bytes_(other.default_block_bytes_), reserved_bytes_(std::exchange(other.reserved_bytes_, 0)) {}

    ScratchArena& operator=(ScratchArena&& other) noexcept {
        if (this != &other) {
            release();
            first_ = std::exchange(other.first_, nullptr);
            current_ = std::exchange(other.current_, nullptr);
            default_block_bytes_ = other.default_block_bytes_;
            reserved_bytes_ = std::exchange(other.reserved_bytes_, 0);
        }
        return *this;
    }

    ~ScratchArena() { release(); }

    [[nodiscard]] void* allocate_bytes(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t)) noexcept {
        if (!std::has_single_bit(alignment))
            fatal("ScratchArena alignment must be a power of two.");
        if (bytes == 0)
            return nullptr;
        if (current_) {
            const auto offset = aligned_offset(payload(current_), current_->used, alignment);
            if (offset <= current_->capacity && bytes <= current_->capacity - offset) {
                current_->used = offset + bytes;
                return payload(current_) + offset;
            }
            for (Block* next = current_->next; next; next = next->next) {
                const auto candidate = aligned_offset(payload(next), 0, alignment);
                if (candidate <= next->capacity && bytes <= next->capacity - candidate) {
                    next->used = candidate + bytes;
                    current_ = next;
                    return payload(next) + candidate;
                }
            }
        }
        return allocate_from_new_block(bytes, alignment);
    }

    template <class T>
        requires(std::is_trivially_destructible_v<T> && std::is_nothrow_default_constructible_v<T> &&
                 !std::is_const_v<T> && !std::is_volatile_v<T>)
    [[nodiscard]] Span<T> allocate(std::size_t count) noexcept {
        if (count == 0)
            return {};
        if (count > SIZE_MAX / sizeof(T))
            fatal("ScratchArena count overflow.");
        auto* raw = static_cast<T*>(allocate_bytes(count * sizeof(T), alignof(T)));
        for (std::size_t i = 0; i < count; ++i)
            std::construct_at(raw + i);
        return {raw, count};
    }

    template <class T>
        requires(std::is_trivially_destructible_v<T> && std::is_implicit_lifetime_v<T> && !std::is_const_v<T> &&
                 !std::is_volatile_v<T>)
    [[nodiscard]] Span<T> allocate_uninitialized(std::size_t count) noexcept {
        if (count == 0)
            return {};
        if (count > SIZE_MAX / sizeof(T))
            fatal("ScratchArena count overflow.");
        const std::size_t bytes = count * sizeof(T);
        void* raw = allocate_bytes(bytes, alignof(T));
        // Reused arena blocks do not re-enter an allocator function, so explicitly
        // start implicit-lifetime array objects per ISO C++23 [intro.object].
#if defined(__cpp_lib_start_lifetime_as) && __cpp_lib_start_lifetime_as >= 202207L
        return {std::start_lifetime_as_array<T>(raw, count), count};
#else
        auto* storage = ::new (raw) unsigned char[bytes];
        return {std::launder(reinterpret_cast<T*>(storage)), count};
#endif
    }

    template <class T, class... Args>
        requires(std::is_trivially_destructible_v<T> && std::is_nothrow_constructible_v<T, Args...> &&
                 !std::is_const_v<T> && !std::is_volatile_v<T>)
    [[nodiscard]] T& emplace(Args&&... args) noexcept {
        auto* raw = static_cast<T*>(allocate_bytes(sizeof(T), alignof(T)));
        return *std::construct_at(raw, std::forward<Args>(args)...);
    }

    template <class T>
        requires(std::is_trivially_destructible_v<T> && std::is_trivially_copyable_v<T> && !std::is_const_v<T> &&
                 !std::is_volatile_v<T>)
    [[nodiscard]] Span<T> copy(Span<const T> values) noexcept {
        if (values.empty())
            return {};
        auto result = allocate_uninitialized<T>(values.size());
        std::memcpy(result.data(), values.data(), values.size_bytes());
        return result;
    }

    [[nodiscard]] StringView copy_string(StringView text) noexcept {
        if (text.size() == SIZE_MAX)
            fatal("ScratchArena string overflow.");
        auto buffer = allocate_uninitialized<char>(text.size() + 1);
        if (!text.empty())
            std::memcpy(buffer.data(), text.data(), text.size());
        buffer[text.size()] = '\0';
        return {buffer.data(), text.size()};
    }

    void reset() noexcept {
        for (Block* block = first_; block; block = block->next)
            block->used = 0;
        current_ = first_;
    }

    void release() noexcept {
        Block* block = first_;
        while (block) {
            Block* next = block->next;
            mi_free(block);
            block = next;
        }
        first_ = nullptr;
        current_ = nullptr;
        reserved_bytes_ = 0;
    }

    [[nodiscard]] std::size_t used_bytes() const noexcept {
        std::size_t total = 0;
        for (const Block* block = first_; block; block = block->next)
            total += block->used;
        return total;
    }

    [[nodiscard]] std::size_t reserved_bytes() const noexcept { return reserved_bytes_; }

private:
    friend class ScratchScope;

    struct alignas(block_alignment) Block {
        Block* next{};

        std::size_t capacity{};

        std::size_t used{};
    };

    static_assert(sizeof(Block) == block_alignment);

    static std::byte* payload(Block* block) noexcept { return reinterpret_cast<std::byte*>(block + 1); }

    static std::size_t aligned_offset(const std::byte* base, std::size_t used, std::size_t alignment) noexcept {
        const auto address = reinterpret_cast<std::uintptr_t>(base + used);
        const auto mask = alignment - 1;
        const auto aligned = (address + mask) & ~mask;
        const auto padding = static_cast<std::size_t>(aligned - address);
        if (padding > SIZE_MAX - used)
            fatal("ScratchArena offset overflow.");
        return used + padding;
    }

    void* allocate_from_new_block(std::size_t bytes, std::size_t alignment) noexcept {
        const std::size_t slack = alignment > block_alignment ? alignment - block_alignment : 0;
        if (bytes > SIZE_MAX - slack - sizeof(Block))
            fatal("ScratchArena block size overflow.");
        const std::size_t capacity = std::max(default_block_bytes_, bytes + slack);
        const std::size_t total = sizeof(Block) + capacity;
        void* raw = mi_malloc_aligned(total, block_alignment);
        if (!raw)
            fatal("ScratchArena allocation failed.");
        auto* block = std::construct_at(static_cast<Block*>(raw));
        block->capacity = capacity;
        reserved_bytes_ += capacity;
        if (!current_) {
            block->next = first_;
            first_ = block;
        } else {
            block->next = current_->next;
            current_->next = block;
        }
        current_ = block;
        const auto offset = aligned_offset(payload(block), 0, alignment);
        block->used = offset + bytes;
        return payload(block) + offset;
    }

    Block* first_{};

    Block* current_{};

    std::size_t default_block_bytes_{};

    std::size_t reserved_bytes_{};
};

// Rewinds a ScratchArena to the exact watermark recorded on entry while keeping
// any newly grown blocks linked for zero-allocation reuse in sibling scopes.
class ScratchScope {
public:
    explicit ScratchScope(ScratchArena& arena) noexcept
        : arena_(arena), block_(arena.current_), used_(arena.current_ ? arena.current_->used : 0) {}

    ScratchScope(const ScratchScope&) = delete;

    ScratchScope& operator=(const ScratchScope&) = delete;

    ~ScratchScope() {
        ScratchArena::Block* tail = block_ ? block_->next : arena_.first_;
        for (; tail; tail = tail->next)
            tail->used = 0;
        if (block_) {
            block_->used = used_;
            arena_.current_ = block_;
        } else
            arena_.current_ = arena_.first_;
    }

private:
    ScratchArena& arena_;

    ScratchArena::Block* block_;

    std::size_t used_;
};

} // namespace atelier
