#pragma once

#include "foundation/hash.hpp"
#include "memory/fatal.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <emmintrin.h>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

namespace atelier::detail {

// Dense entries serve iteration; only the index is rebuilt on rehash. Windows
// x64 always has SSE2. A table with up to 16 entries needs no heap index:
// its inline control byte's position is already the dense entry's position.
template <class Key, class Value, class H, class E>
class DenseHashTable {
    using Tags = std::array<std::uint8_t, 16>;
    static constexpr unsigned slots = 15;
    static constexpr unsigned target = 12; // 80% of the usable slots.

    struct Group {
        std::uint32_t displaced;
        std::uint32_t entries[slots];
    };
    static_assert(sizeof(Group) == 64);

    struct Slot {
        std::uint32_t group;
        unsigned lane;
    };

    std::vector<Value> values_;
    std::unique_ptr<Group[]> groups_;
    std::unique_ptr<Tags[]> controls_;
    Tags local_{};
    std::uint32_t mask_{};
    std::uint8_t shift_{};
    H hash_{};
    E equal_{};

    std::size_t capacity() const noexcept { return groups_ ? (std::size_t{mask_} + 1) * target : local_.size(); }

    static const Key& key_of(const Value& value) noexcept {
        if constexpr (std::is_same_v<Key, Value>)
            return value;
        else
            return value.first;
    }

    template <class Q>
    std::uint64_t mixed_hash(const Q& key) const noexcept {
        const auto hash = static_cast<std::size_t>(hash_(key));
        if constexpr (!hash_is_avalanching_v<H>)
            return Hash<std::uint64_t>{}(hash);
        else if constexpr (sizeof(decltype(hash_(key))) < sizeof(std::uint64_t))
            return hash * UINT64_C(0x9ddfea08eb382d69);
        else
            return hash;
    }

    // Indexed groups have 15 key tags. Byte 15 is 0 or 1 according to the
    // exact spill count in the payload, so it cannot match a key fingerprint.
    static std::uint8_t tag(std::uint64_t hash) noexcept {
        const auto byte = static_cast<std::uint8_t>(hash);
        return byte < 2 ? 2 : byte;
    }

    // The borrowed-name index has byte equality. Its small tables can filter
    // using length and a suffix without hashing the whole name. In particular
    // both digits distinguish matrix fields such as m01 and m11.
    static std::uint8_t name_tag(StringView name) noexcept {
        const auto n = name.size();
        if (n == 0)
            return 1;
        const auto last = static_cast<unsigned char>(name.data()[n - 1]);
        const auto previous = n > 1 ? static_cast<unsigned char>(name.data()[n - 2]) : 0u;
        return tag(n * 31 + previous * 13 + last);
    }
    static std::uint8_t name_tag(const char* name) noexcept { return name_tag(StringView{name}); }

    template <class Q>
    std::uint8_t local_tag(const Q& key) const noexcept {
        if constexpr (std::is_same_v<Key, StringView> && std::is_same_v<H, Hash<StringView>> &&
                      std::is_same_v<E, Equal<StringView>>)
            return name_tag(key);
        else
            return tag(mixed_hash(key));
    }

    static unsigned matching(const Tags& tags, std::uint8_t needle) noexcept {
        const auto block = _mm_loadu_si128(reinterpret_cast<const __m128i*>(tags.data()));
        return static_cast<unsigned>(
            _mm_movemask_epi8(_mm_cmpeq_epi8(block, _mm_set1_epi8(static_cast<char>(needle)))));
    }

    std::uint32_t home(std::uint64_t hash) const noexcept { return static_cast<std::uint32_t>(hash >> shift_); }
    std::uint32_t next(std::uint32_t group) const noexcept { return (group + 1) & mask_; }

    template <class Q>
    const Value* find_local(const Q& key, std::uint8_t needle) const noexcept {
        for (auto hits = matching(local_, needle); hits; hits &= hits - 1) {
            const auto& value = values_.data()[std::countr_zero(hits)];
            if (equal_(key, key_of(value)))
                return std::addressof(value);
        }
        return nullptr;
    }

    template <class Q>
    Slot probe(const Q& key, std::uint64_t hash) const noexcept {
        const auto first = home(hash);
        auto group = first;
        const auto needle = tag(hash);
        for (;;) {
            const auto& block = groups_[group];
            for (auto hits = matching(controls_[group], needle); hits; hits &= hits - 1) {
                const auto lane = static_cast<unsigned>(std::countr_zero(hits));
                if (equal_(key, key_of(values_.data()[block.entries[lane]])))
                    return {group, lane};
            }
            if (!controls_[group][slots])
                return {group, slots};
            group = next(group);
            if (group == first)
                return {group, slots};
        }
    }

    // Each crossed group counts the live entries that had to pass it. Erase
    // subtracts exactly the same path, so churn needs neither tombstones nor
    // a reverse slot pointer on every dense entry.
    void place(std::uint64_t hash, std::uint32_t index) noexcept {
        auto group = home(hash);
        for (;;) {
            auto& block = groups_[group];
            const auto free = matching(controls_[group], 0) & ((1u << slots) - 1);
            if (free) {
                const auto lane = std::countr_zero(free);
                block.entries[lane] = index;
                controls_[group][lane] = tag(hash);
                return;
            }
            controls_[group][slots] = 1;
            ++block.displaced;
            group = next(group);
        }
    }

    void rebuild(std::size_t requested) {
        const auto count = std::bit_ceil((requested + target - 1) / target);
        // Dense values remain authoritative during growth. No old index
        // needs to coexist with the new allocation.
        groups_.reset();
        controls_.reset();
        auto tags = std::make_unique<Tags[]>(count);
        auto fresh = std::make_unique<Group[]>(count);
        auto* const blocks = fresh.get();
        const auto mask = static_cast<std::uint32_t>(count - 1);
        const auto shift = static_cast<std::uint8_t>(64 - std::countr_zero(count));
        const auto entries = values_.data();
        const auto length = size();
        // A byte per group keeps the insertion cursors together during a bulk
        // rebuild. A fresh group has no holes.
        std::array<std::uint8_t, 64> small_cursors{};
        auto overflow_cursors =
            length && count > small_cursors.size() ? std::make_unique<std::uint8_t[]>(count) : nullptr;
        auto* const cursors = overflow_cursors ? overflow_cursors.get() : small_cursors.data();
        std::array<std::uint64_t, 32> hashes;
        for (std::size_t start = 0; start < length; start += hashes.size()) {
            const auto n = std::min(hashes.size(), length - start);
            for (std::size_t j = 0; j < n; ++j) {
                hashes[j] = mixed_hash(key_of(entries[start + j]));
                _mm_prefetch(reinterpret_cast<const char*>(blocks + (hashes[j] >> shift)), _MM_HINT_T0);
            }
            for (std::size_t j = 0; j < n; ++j) {
                const auto hash = hashes[j];
                auto group = static_cast<std::uint32_t>(hash >> shift);
                while (cursors[group] == slots) {
                    tags[group][slots] = 1;
                    ++blocks[group].displaced;
                    group = (group + 1) & mask;
                }
                auto& block = blocks[group];
                const auto lane = cursors[group]++;
                block.entries[lane] = static_cast<std::uint32_t>(start + j);
                tags[group][lane] = tag(hash);
            }
        }
        groups_ = std::move(fresh);
        mask_ = mask;
        shift_ = shift;
        controls_ = std::move(tags);
    }

public:
    DenseHashTable() = default;

    DenseHashTable(const H& hash, const E& equal) noexcept : hash_(hash), equal_(equal) {}

    DenseHashTable empty_with_policies() const noexcept { return {hash_, equal_}; }

    DenseHashTable(const DenseHashTable& other)
        : values_(other.values_), local_(other.local_), hash_(other.hash_), equal_(other.equal_) {
        if (other.groups_)
            rebuild(std::max(size(), local_.size() + 1));
    }

    static constexpr std::size_t max_size() noexcept { return std::size_t{1} << 32; }
    const auto& values() const noexcept { return values_; }
    auto& values() noexcept { return values_; }
    std::size_t size() const noexcept { return values_.size(); }

    void reserve(std::size_t total) noexcept {
        values_.reserve(total);
        if (total > capacity())
            rebuild(total);
    }

    template <class Q>
    const Value* find(const Q& key) const noexcept {
        if (!groups_)
            return find_local(key, local_tag(key));
        const auto at = probe(key, mixed_hash(key));
        return at.lane == slots ? nullptr : values_.data() + groups_[at.group].entries[at.lane];
    }

    template <class Q, class... Args>
    std::pair<Value*, bool> try_emplace(Q&& key, Args&&... args) {
        std::uint64_t hash{};
        std::uint8_t needle{};
        if (groups_) {
            hash = mixed_hash(key);
            const auto at = probe(key, hash);
            if (at.lane != slots)
                return {values_.data() + groups_[at.group].entries[at.lane], false};
        } else {
            needle = local_tag(key);
            if (const auto* found = find_local(key, needle))
                return {const_cast<Value*>(found), false};
        }

        const auto index = size();
        if (index == max_size())
            fatal("Dense hash table capacity.");
        // Construct before publishing a tag. The arguments can borrow the
        // old values buffer even when emplace_back has to relocate it.
        if constexpr (std::is_same_v<Key, Value>)
            values_.emplace_back(std::forward<Q>(key));
        else
            values_.emplace_back(std::piecewise_construct, std::forward_as_tuple(std::forward<Q>(key)),
                                 std::forward_as_tuple(std::forward<Args>(args)...));
        if (size() > capacity())
            rebuild(size());
        else if (groups_)
            place(hash, static_cast<std::uint32_t>(index));
        else
            local_[index] = needle;
        return {std::addressof(values_.back()), true};
    }

    template <class Q>
    bool erase(const Q& key) noexcept {
        std::size_t index;
        if (!groups_) {
            const auto* found = find_local(key, local_tag(key));
            if (!found)
                return false;
            index = static_cast<std::size_t>(found - values_.data());
            local_[index] = local_[size() - 1];
            local_[size() - 1] = 0;
        } else {
            const auto hash = mixed_hash(key);
            const auto at = probe(key, hash);
            if (at.lane == slots)
                return false;
            auto& block = groups_[at.group];
            index = block.entries[at.lane];
            controls_[at.group][at.lane] = 0;
            for (auto g = home(hash); g != at.group; g = next(g))
                if (--groups_[g].displaced == 0)
                    controls_[g][slots] = 0;
            if (index != size() - 1) {
                const auto& last = key_of(values_.back());
                const auto tail = probe(last, mixed_hash(last));
                groups_[tail.group].entries[tail.lane] = static_cast<std::uint32_t>(index);
            }
        }
        if (index != size() - 1)
            values_[index] = std::move(values_.back());
        values_.pop_back();
        return true;
    }

    void clear() noexcept {
        if (values_.empty())
            return;
        values_.clear();
        local_ = {};
        if (groups_)
            for (std::uint32_t g = 0; g <= mask_; ++g) {
                controls_[g] = {};
                groups_[g].displaced = 0;
            }
    }

    void swap(DenseHashTable& other) noexcept {
        using std::swap;
        values_.swap(other.values_);
        groups_.swap(other.groups_);
        controls_.swap(other.controls_);
        swap(local_, other.local_);
        swap(mask_, other.mask_);
        swap(shift_, other.shift_);
        swap(hash_, other.hash_);
        swap(equal_, other.equal_);
    }
};

}
