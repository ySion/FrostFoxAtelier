#pragma once

#include "foundation/array.hpp"

#include <algorithm>
#include <functional>

namespace atelier {

// Compare(a,b) places a below b in priority. Only the root is observable.
template <class T, class Compare = std::less<>>
class PriorityQueue {
    static_assert(std::strict_weak_order<Compare&, const T&, const T&>);

    static_assert(std::is_move_constructible_v<T> && std::is_move_assignable_v<T>);

public:
    explicit PriorityQueue(Compare compare = {}) noexcept : compare_(std::move(compare)) {}

    explicit PriorityQueue(Array<T>&& values, Compare compare = {}) noexcept
        : compare_(std::move(compare)), values_(std::move(values)) {
        std::make_heap(values_.begin(), values_.end(), std::ref(compare_));
    }

    PriorityQueue(const PriorityQueue&) noexcept = default;

    PriorityQueue(PriorityQueue&&) noexcept = default;

    PriorityQueue& operator=(PriorityQueue incoming) noexcept
        requires std::is_swappable_v<Compare>
    {
        swap(incoming);
        return *this;
    }

    [[nodiscard]] bool empty() const noexcept { return values_.empty(); }

    [[nodiscard]] std::size_t size() const noexcept { return values_.size(); }

    [[nodiscard]] std::size_t capacity() const noexcept { return values_.capacity(); }

    [[nodiscard]] const T& top() const& noexcept [[clang::lifetimebound]] { return values_.front(); }

    const T& top() const&& = delete;

    template <class... Args>
        requires std::constructible_from<T, Args...>
    void emplace(Args&&... args) noexcept {
        values_.emplace(std::forward<Args>(args)...);
        std::push_heap(values_.begin(), values_.end(), std::ref(compare_));
    }

    T pop() noexcept {
        if (empty())
            fatal("Cannot pop an empty PriorityQueue.");
        std::pop_heap(values_.begin(), values_.end(), std::ref(compare_));
        T result(std::move(values_.back()));
        values_.pop();
        return result;
    }

    void reserve(std::size_t capacity) noexcept { values_.reserve(capacity); }

    void clear() noexcept { values_.clear(); }

    void release() noexcept { values_.release(); }

    void swap(PriorityQueue& other) noexcept
        requires std::is_swappable_v<Compare>
    {
        using std::swap;
        swap(compare_, other.compare_);
        values_.swap(other.values_);
    }

private:
    [[msvc::no_unique_address]] Compare compare_;

    Array<T> values_;
};

}
