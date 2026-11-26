#pragma once

#include "foundation/hash.hpp"
#include "foundation/optional.hpp"
#include "foundation/span.hpp"
#include "foundation/detail/dense_hash_table.hpp"
#include "memory/fatal.hpp"

#include <concepts>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <ranges>
#include <utility>

namespace atelier {

// Owning unique-value collection with the same hashing, lifetime and structural
// invalidation contract as Map. Stored values are read-only, including iteration.
template <class T, class H = Hash<T>, class E = Equal<T>>
class Set {
    static_assert(detail::DenseValue<T>,
                  "Set values require non-throwing move construction, move assignment and destruction.");

    static_assert(detail::HashPolicyLifecycle<H> && detail::HashPolicyLifecycle<E> && detail::HashQuery<T, T, H, E>,
                  "Set Hash/Equal require non-throwing calls and policy lifecycle operations.");

    using Storage = detail::DenseHashTable<T, T, H, E>;

public:
    using Iterator = const T*;

    Set()
        requires(detail::HashPolicy<H> && detail::HashPolicy<E>)
    = default;

    Set(const H& hash, const E& equal) noexcept : storage_(hash, equal) {}

    Set(std::initializer_list<T> values)
        requires(detail::HashPolicy<H> && detail::HashPolicy<E> && std::is_copy_constructible_v<T>)
        : Set(values.begin(), values.end()) {}

    template <std::input_iterator It, std::sentinel_for<It> Sentinel>
        requires(detail::HashPolicy<H> && detail::HashPolicy<E> &&
                 detail::HashQuery<T, std::iter_reference_t<It>, H, E> &&
                 std::constructible_from<T, std::iter_reference_t<It>>)
    Set(It first, Sentinel last) {
        if constexpr (std::sized_sentinel_for<Sentinel, It>)
            reserve(static_cast<std::size_t>(last - first));
        for (; first != last; ++first)
            add(*first);
    }

    template <std::ranges::input_range R>
        requires(!std::same_as<std::remove_cvref_t<R>, Set> && !std::same_as<std::remove_cvref_t<R>, T> &&
                 detail::HashPolicy<H> && detail::HashPolicy<E> &&
                 detail::HashQuery<T, std::ranges::range_reference_t<R>, H, E> &&
                 std::constructible_from<T, std::ranges::range_reference_t<R>>)
    explicit Set(R&& values) {
        if constexpr (std::ranges::sized_range<R>)
            reserve(static_cast<std::size_t>(std::ranges::size(values)));
        for (auto&& value : values)
            add(std::forward<decltype(value)>(value));
    }

    Set(const Set&)
        requires std::is_copy_constructible_v<T>
    = default;

    Set& operator=(const Set& other)
        requires std::is_copy_constructible_v<T>
    {
        if (this != &other) {
            Set copy{other};
            swap(copy);
        }
        return *this;
    }

    Set(Set&& other) noexcept : storage_(other.empty_after_move()) { swap(other); }

    Set& operator=(Set&& other) noexcept {
        if (this != &other) {
            Set incoming{std::move(other)};
            swap(incoming);
        }
        return *this;
    }

    [[nodiscard]] std::size_t size() const noexcept { return storage_.size(); }

    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    void reserve(std::size_t total) noexcept {
        if (total > storage_.max_size())
            fatal("Set capacity.");
        if (total)
            storage_.reserve(total);
    }

    void clear() noexcept { storage_.clear(); }

    void release() noexcept {
        auto empty = storage_.empty_with_policies();
        storage_.swap(empty);
    }

    void swap(Set& other) noexcept {
        if (this != &other)
            storage_.swap(other.storage_);
    }

    template <class Q = T>
        requires detail::HashQuery<T, Q, H, E>
    [[nodiscard]] const T* find(const Q& value) const noexcept [[clang::lifetimebound]] {
        return storage_.find(value);
    }

    template <class Q = T>
        requires detail::HashQuery<T, Q, H, E>
    [[nodiscard]] bool contains(const Q& value) const noexcept {
        return find(value) != nullptr;
    }

    template <class Q = T>
        requires detail::HashQuery<T, Q, H, E> && std::constructible_from<T, Q&&>
    bool add(Q&& value) {
        return storage_.try_emplace(std::forward<Q>(value)).second;
    }

    void merge(Set& source) { storage_.merge(source.storage_); }

    void merge(Set&& source) { storage_.merge(source.storage_); }

    template <class Q = T>
        requires detail::HashQuery<T, Q, H, E>
    bool remove(const Q& value) noexcept {
        return storage_.erase(value);
    }

    template <class Q = T>
        requires detail::HashQuery<T, Q, H, E>
    [[nodiscard]] Optional<T> take(const Q& value) noexcept {
        return storage_.extract(value);
    }

    template <class Predicate>
        requires std::predicate<Predicate&, const T&>
    std::size_t remove_if(Predicate&& predicate) noexcept {
        return storage_.remove_if([&](const T& item) { return static_cast<bool>(std::invoke(predicate, item)); });
    }

    [[nodiscard]] Span<const T> view() const& noexcept [[clang::lifetimebound]] { return {begin(), size()}; }

    Span<const T> view() const&& = delete;

    [[nodiscard]] Iterator begin() const noexcept [[clang::lifetimebound]] { return storage_.values().data(); }

    [[nodiscard]] Iterator end() const noexcept [[clang::lifetimebound]] { return begin() + size(); }

    friend bool operator==(const Set& left, const Set& right) noexcept {
        if (&left == &right)
            return true;
        if (left.size() != right.size())
            return false;
        for (const auto& item : left)
            if (!right.contains(item))
                return false;
        return true;
    }

private:
    Storage empty_after_move() const noexcept {
        if constexpr (detail::HashPolicy<H> && detail::HashPolicy<E>)
            return {};
        else
            return storage_.empty_with_policies();
    }

    Storage storage_;
};

}
