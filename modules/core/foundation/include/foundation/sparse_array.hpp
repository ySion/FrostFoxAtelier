#pragma once

#include "foundation/bit_vector.hpp"

#include <memory>
#include <utility>

namespace atelier {

// Stable live indices, not stable addresses or generational handles. Erased
// indices are reused LIFO. Growth moves live values; holes never contain a T.
template <class T>
class SparseArray {
    static_assert(std::is_object_v<T> && !std::is_const_v<T> && !std::is_volatile_v<T>);

    static_assert(std::is_nothrow_destructible_v<T>);

    static_assert(std::is_nothrow_move_constructible_v<T> || std::is_copy_constructible_v<T>);

    union Slot {
        T value;

        std::size_t next;

        Slot() noexcept : next(SIZE_MAX) {}

        ~Slot()
            requires std::is_trivially_destructible_v<T>
        = default;

        ~Slot()
            requires(!std::is_trivially_destructible_v<T>)
        {}
    };

    using Storage = detail::ArrayElements<Slot>;

public:
    static constexpr std::size_t npos = SIZE_MAX;

    static constexpr std::size_t max_size = Storage::max_size;

    SparseArray() noexcept = default;

    SparseArray(const SparseArray& other) noexcept
        requires std::is_copy_constructible_v<T>
        : slots_(Storage::allocate(other.extent())), capacity_(other.extent()), occupied_(other.occupied_),
          free_head_(other.free_head_), size_(other.size_) {
        const auto limit = capacity_;
        for (std::size_t i = 0; i != limit; ++i) {
            std::construct_at(slots_ + i);
            if (occupied_.test(i))
                std::construct_at(std::addressof(slots_[i].value), std::as_const(other.slots_[i].value));
            else
                slots_[i].next = other.slots_[i].next;
        }
    }

    SparseArray& operator=(const SparseArray& other) noexcept
        requires std::is_copy_constructible_v<T>
    {
        if (this != &other) {
            SparseArray copy{other};
            swap(copy);
        }
        return *this;
    }

    SparseArray(SparseArray&& other) noexcept { swap(other); }

    SparseArray& operator=(SparseArray&& other) noexcept {
        if (this != &other) {
            SparseArray incoming{std::move(other)};
            swap(incoming);
        }
        return *this;
    }

    ~SparseArray() { release(); }

    void swap(SparseArray& other) noexcept {
        std::swap(slots_, other.slots_);
        std::swap(capacity_, other.capacity_);
        occupied_.swap(other.occupied_);
        std::swap(free_head_, other.free_head_);
        std::swap(size_, other.size_);
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    [[nodiscard]] std::size_t extent() const noexcept { return occupied_.size(); }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    [[nodiscard]] bool contains_index(std::size_t index) const noexcept { return occupied_.contains(index); }

    [[nodiscard]] std::size_t find_index_from(std::size_t first) const noexcept { return occupied_.find_from(first); }

    [[nodiscard]] T* try_at(std::size_t index) & noexcept [[clang::lifetimebound]] {
        return contains_index(index) ? std::addressof(slots_[index].value) : nullptr;
    }

    [[nodiscard]] const T* try_at(std::size_t index) const& noexcept [[clang::lifetimebound]] {
        return contains_index(index) ? std::addressof(slots_[index].value) : nullptr;
    }

    T* try_at(std::size_t) && = delete;

    const T* try_at(std::size_t) const&& = delete;

    [[nodiscard]] T& operator[](std::size_t index) & noexcept [[clang::lifetimebound]] {
        auto* value = try_at(index);
        if (!value)
            fatal("SparseArray index is not occupied.");
        return *value;
    }

    [[nodiscard]] const T& operator[](std::size_t index) const& noexcept [[clang::lifetimebound]] {
        const auto* value = try_at(index);
        if (!value)
            fatal("SparseArray index is not occupied.");
        return *value;
    }

    T& operator[](std::size_t) && = delete;

    const T& operator[](std::size_t) const&& = delete;

    void reserve(std::size_t capacity) noexcept {
        if (capacity <= capacity_)
            return;
        auto* replacement = Storage::allocate(capacity);
        occupied_.reserve(capacity);
        const auto limit = extent();
        for (std::size_t i = 0; i != limit; ++i) {
            std::construct_at(replacement + i);
            if (occupied_.test(i)) {
                std::construct_at(std::addressof(replacement[i].value), std::move_if_noexcept(slots_[i].value));
                std::destroy_at(std::addressof(slots_[i].value));
            } else
                replacement[i].next = slots_[i].next;
        }
        Storage::deallocate(slots_);
        slots_ = replacement;
        capacity_ = capacity;
    }

    template <class... Args>
        requires std::is_constructible_v<T, Args...>
    std::size_t emplace(Args&&... args) noexcept {
        if (free_head_ == npos && extent() == capacity_) {
            // Preserve arguments that borrow an existing value across growth.
            T candidate(std::forward<Args>(args)...);
            reserve(Storage::grown_capacity(extent(), capacity_, 1));
            return emplace(std::move_if_noexcept(candidate));
        }
        const auto index = free_head_ == npos ? extent() : free_head_;
        if (index == extent()) {
            occupied_.resize(index + 1);
            std::construct_at(slots_ + index);
        } else
            free_head_ = slots_[index].next;
        std::construct_at(std::addressof(slots_[index].value), std::forward<Args>(args)...);
        occupied_.set(index);
        ++size_;
        return index;
    }

    bool erase(std::size_t index) noexcept {
        if (!contains_index(index))
            return false;
        std::destroy_at(std::addressof(slots_[index].value));
        slots_[index].next = free_head_;
        free_head_ = index;
        occupied_.reset(index);
        --size_;
        return true;
    }

    void clear() noexcept {
        if constexpr (!std::is_trivially_destructible_v<T>)
            for (auto index : occupied_.set_bits())
                std::destroy_at(std::addressof(slots_[index].value));
        occupied_.clear();
        free_head_ = npos;
        size_ = 0;
    }

    void release() noexcept {
        clear();
        Storage::deallocate(std::exchange(slots_, nullptr));
        capacity_ = 0;
        occupied_.release();
    }

    template <bool Const>
    struct Entry {
        std::size_t index;

        std::conditional_t<Const, const T&, T&> value;
    };

    template <bool Const>
    class Iterator {
        using Owner = std::conditional_t<Const, const SparseArray, SparseArray>;

    public:
        using value_type = Entry<Const>;

        using difference_type = std::ptrdiff_t;

        using iterator_concept = std::forward_iterator_tag;

        Iterator() noexcept = default;

        Iterator(Owner* owner, std::size_t index) noexcept : owner_(owner), index_(index) {}

        value_type operator*() const noexcept { return {index_, owner_->slots_[index_].value}; }

        Iterator& operator++() noexcept {
            index_ = owner_->occupied_.find_next(index_);
            return *this;
        }

        Iterator operator++(int) noexcept {
            auto previous = *this;
            ++*this;
            return previous;
        }

        bool operator==(const Iterator&) const noexcept = default;

    private:
        Owner* owner_{};

        std::size_t index_{npos};
    };

    template <bool Const>
    struct Entries {
        std::conditional_t<Const, const SparseArray*, SparseArray*> owner;

        Iterator<Const> begin() const noexcept { return {owner, owner->occupied_.find_first()}; }

        Iterator<Const> end() const noexcept { return {owner, npos}; }
    };

    [[nodiscard]] Entries<false> entries() & noexcept [[clang::lifetimebound]] { return {this}; }

    [[nodiscard]] Entries<true> entries() const& noexcept [[clang::lifetimebound]] { return {this}; }

    Entries<false> entries() && = delete;

    Entries<true> entries() const&& = delete;

private:
    Slot* slots_{};

    std::size_t capacity_{};

    BitVector occupied_;

    std::size_t free_head_{npos};

    std::size_t size_{};
};

}
