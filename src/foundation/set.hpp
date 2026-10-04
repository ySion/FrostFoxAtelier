#pragma once

#include "foundation/hash.hpp"

#include <ankerl/unordered_dense.h>
#include <memory>
#include <utility>

namespace atelier {

// Owning unique-value collection with the same hashing, lifetime and structural
// invalidation contract as Map. Stored values are read-only, including iteration.
template<class T, class H = Hash<T>, class E = Equal<T>>
class Set {
    static_assert(detail::DenseValue<T>,
        "Set values require non-throwing move construction, move assignment and destruction.");
    static_assert(detail::HashPolicy<H> && detail::HashPolicy<E> && detail::HashQuery<T, T, H, E>,
        "Set Hash/Equal require non-throwing calls and policy lifecycle operations.");
    using Storage = ankerl::unordered_dense::set<T, H, E>;
public:
    using Iterator = const T*;
    Set() = default;
    Set(const Set&) requires std::is_copy_constructible_v<T> = default;
    Set& operator=(const Set& other) requires std::is_copy_constructible_v<T> {
        if (this != &other) { Set copy{other}; swap(copy); }
        return *this;
    }
    Set(Set&& other) noexcept { swap(other); }
    Set& operator=(Set&& other) noexcept {
        if (this != &other) { Set incoming{std::move(other)}; swap(incoming); }
        return *this;
    }

    [[nodiscard]] std::size_t size() const noexcept { return storage_.size(); }
    [[nodiscard]] bool empty() const noexcept { return storage_.empty(); }
    void reserve(std::size_t total) {
        if (total > storage_.max_size()) throw std::length_error("Set capacity.");
        if (total) storage_.reserve(total);
    }
    void clear() noexcept { storage_.clear(); }
    void release() noexcept { Storage empty; storage_.swap(empty); }
    void swap(Set& other) noexcept { if (this != &other) storage_.swap(other.storage_); }

    template<class Q> requires detail::HashQuery<T, Q, H, E>
    [[nodiscard]] const T* find(const Q& value) const noexcept {
        const auto at = storage_.find(value);
        return at == storage_.end() ? nullptr : std::addressof(*at);
    }
    template<class Q> requires detail::HashQuery<T, Q, H, E>
    [[nodiscard]] bool contains(const Q& value) const noexcept { return find(value) != nullptr; }
    template<class Q> requires detail::HashQuery<T, Q, H, E> && std::constructible_from<T, Q&&>
    bool add(Q&& value) { return storage_.emplace(std::forward<Q>(value)).second; }
    template<class Q> requires detail::HashQuery<T, Q, H, E>
    bool remove(const Q& value) noexcept { return storage_.erase(value) != 0; }

    [[nodiscard]] Iterator begin() const noexcept { return storage_.values().data(); }
    [[nodiscard]] Iterator end() const noexcept { return empty() ? begin() : begin() + size(); }

private:
    Storage storage_;
};

}
