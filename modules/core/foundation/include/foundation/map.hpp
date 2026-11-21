#pragma once

#include "foundation/hash.hpp"
#include "foundation/detail/dense_hash_table.hpp"
#include "memory/fatal.hpp"

#include <iterator>
#include <memory>
#include <utility>

namespace atelier {

// Owning unique-key dictionary. Structural operations invalidate all borrowed
// entries/iterators, including on failure; iteration order is unspecified.
// Hash/Equal and relocation cannot throw. Copies are independent owning values.
template <class K, class V, class H = Hash<K>, class E = Equal<K>>
class Map {
    static_assert(detail::DenseValue<K> && detail::DenseValue<V>,
                  "Map keys/values require non-throwing move construction, move assignment and destruction.");

    static_assert(detail::HashPolicy<H> && detail::HashPolicy<E> && detail::HashQuery<K, K, H, E>,
                  "Map Hash/Equal require non-throwing calls and policy lifecycle operations.");

    using Pair = std::pair<K, V>;
    using Storage = detail::DenseHashTable<K, Pair, H, E>;

    static constexpr bool copyable = std::is_copy_constructible_v<K> && std::is_copy_constructible_v<V>;

public:
    struct InsertResult {
        V* value;

        bool inserted;
    };

    // A small proxy keeps the stored key read-only while permitting value edits.
    // Use `for (auto [key, value] : map)`; both bindings borrow their entries.
    template <bool ReadOnly>
    class EntryIterator {
        using Pointer = std::conditional_t<ReadOnly, const Pair*, Pair*>;

        Pointer current_{};

        explicit EntryIterator(Pointer current) noexcept : current_(current) {}

        friend class Map;

        template <bool>
        friend class EntryIterator;

    public:
        struct Entry {
            const K& key;

            std::conditional_t<ReadOnly, const V&, V&> value;
        };

        struct Arrow {
            Entry entry;

            const Entry* operator->() const noexcept { return &entry; }
        };

        using value_type = Entry;

        using difference_type = std::ptrdiff_t;

        using iterator_concept = std::forward_iterator_tag;

        using iterator_category = std::input_iterator_tag;

        EntryIterator() noexcept = default;

        template <bool Other>
            requires(ReadOnly && !Other)
        EntryIterator(const EntryIterator<Other>& other) noexcept : current_(other.current_) {}

        [[nodiscard]] Entry operator*() const noexcept { return {current_->first, current_->second}; }

        [[nodiscard]] Arrow operator->() const noexcept { return {**this}; }

        EntryIterator& operator++() noexcept {
            ++current_;
            return *this;
        }

        EntryIterator operator++(int) noexcept {
            auto previous = *this;
            ++*this;
            return previous;
        }

        friend bool operator==(const EntryIterator&, const EntryIterator&) = default;
    };

    using Iterator = EntryIterator<false>;

    using ConstIterator = EntryIterator<true>;

    Map() = default;

    Map(const Map&)
        requires copyable
    = default;

    Map& operator=(const Map& other)
        requires copyable
    {
        if (this != &other) {
            Map copy{other};
            swap(copy);
        }
        return *this;
    }

    // Give the source freshly default-constructed policies, not moved-from ones.
    Map(Map&& other) noexcept { swap(other); }

    Map& operator=(Map&& other) noexcept {
        // The source can be owned by an element of this map. Detach it before
        // destroying the old contents (which could destroy the source object).
        if (this != &other) {
            Map incoming{std::move(other)};
            swap(incoming);
        }
        return *this;
    }

    [[nodiscard]] std::size_t size() const noexcept { return storage_.size(); }

    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    void reserve(std::size_t total) noexcept {
        if (total > storage_.max_size())
            fatal("Map capacity.");
        if (total)
            storage_.reserve(total);
    }

    void clear() noexcept { storage_.clear(); }

    void release() noexcept {
        Storage empty;
        storage_.swap(empty);
    }

    void swap(Map& other) noexcept {
        if (this != &other)
            storage_.swap(other.storage_);
    }

    template <class Q>
        requires detail::HashQuery<K, Q, H, E>
    [[nodiscard]] V* find(const Q& key) noexcept [[clang::lifetimebound]] {
        return const_cast<V*>(std::as_const(*this).find(key));
    }

    template <class Q>
        requires detail::HashQuery<K, Q, H, E>
    [[nodiscard]] const V* find(const Q& key) const noexcept [[clang::lifetimebound]] {
        const auto* at = storage_.find(key);
        return at ? std::addressof(at->second) : nullptr;
    }

    template <class Q>
        requires detail::HashQuery<K, Q, H, E>
    [[nodiscard]] bool contains(const Q& key) const noexcept {
        return find(key) != nullptr;
    }

    // Lookup precedes construction: an existing key consumes neither argument.
    template <class Q, class... A>
        requires detail::HashQuery<K, Q, H, E> && std::constructible_from<K, Q&&> && std::constructible_from<V, A&&...>
    InsertResult try_emplace(Q&& key, A&&... arguments) [[clang::lifetimebound]] {
        const auto [at, inserted] = storage_.try_emplace(std::forward<Q>(key), std::forward<A>(arguments)...);
        return {std::addressof(at->second), inserted};
    }

    template <class Q, class U>
        requires detail::HashQuery<K, Q, H, E> && std::constructible_from<K, Q&&> && std::constructible_from<V, U&&>
    InsertResult insert_or_assign(Q&& key, U&& value) [[clang::lifetimebound]] {
        // Construct before replacing the old value; committing its move cannot throw.
        V replacement(std::forward<U>(value));
        auto result = try_emplace(std::forward<Q>(key), std::move(replacement));
        if (!result.inserted)
            *result.value = std::move(replacement);
        return result;
    }

    template <class Q>
        requires detail::HashQuery<K, Q, H, E>
    bool remove(const Q& key) noexcept {
        return storage_.erase(key);
    }

    [[nodiscard]] Iterator begin() noexcept [[clang::lifetimebound]] { return Iterator{storage_.values().data()}; }

    [[nodiscard]] Iterator end() noexcept [[clang::lifetimebound]] {
        auto& values = storage_.values();
        return Iterator{values.data() + values.size()};
    }

    [[nodiscard]] ConstIterator begin() const noexcept [[clang::lifetimebound]] {
        return ConstIterator{storage_.values().data()};
    }

    [[nodiscard]] ConstIterator end() const noexcept [[clang::lifetimebound]] {
        const auto& values = storage_.values();
        return ConstIterator{values.data() + values.size()};
    }

private:
    Storage storage_;
};

}
