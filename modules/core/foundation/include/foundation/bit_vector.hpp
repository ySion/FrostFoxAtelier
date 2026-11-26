#pragma once

#include "foundation/array.hpp"
#include "foundation/hash.hpp"

#include <bit>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <ranges>

namespace atelier {

// Packed bits, not an Array<bool> specialization. Unused tail bits stay zero.
// Structural mutation invalidates set_bits() iterators; no atomic operations.
class BitVector {
public:
    static constexpr std::size_t npos = SIZE_MAX;

    static constexpr std::size_t max_size = ~std::size_t{63};

    BitVector() noexcept = default;

    explicit BitVector(std::size_t size, bool value = false) noexcept { assign(size, value); }

    BitVector(std::initializer_list<bool> values) noexcept { assign(values); }

    BitVector(const BitVector&) noexcept = default;

    BitVector& operator=(const BitVector&) noexcept = default;

    BitVector(BitVector&& other) noexcept { swap(other); }

    BitVector& operator=(BitVector&& other) noexcept {
        if (this != &other) {
            BitVector incoming{std::move(other)};
            swap(incoming);
        }
        return *this;
    }

    void swap(BitVector& other) noexcept {
        words_.swap(other.words_);
        std::swap(size_, other.size_);
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    [[nodiscard]] Span<const std::uint64_t> words() const& noexcept [[clang::lifetimebound]] { return words_.view(); }

    Span<const std::uint64_t> words() const&& = delete;

    void reserve(std::size_t bits) noexcept { words_.reserve(word_count(bits)); }

    void resize(std::size_t bits, bool value = false) noexcept {
        const auto previous = size_;
        const auto next_words = word_count(bits);
        if (bits > previous && value) {
            if (const auto prev_rem = previous % 64)
                words_.data()[previous / 64] |= ~low_mask(prev_rem);
            words_.resize(next_words, UINT64_MAX);
        } else {
            words_.resize(next_words, 0);
        }
        size_ = bits;
        clear_tail();
    }

    void assign(std::size_t bits, bool value = false) noexcept {
        words_.assign(word_count(bits), value ? UINT64_MAX : 0);
        size_ = bits;
        clear_tail();
    }

    void assign(std::initializer_list<bool> values) noexcept {
        const auto bits = values.size();
        words_.assign(word_count(bits), 0);
        size_ = bits;
        const bool* src = values.begin();
        const auto full = bits / 64;
        for (std::size_t w = 0; w != full; ++w) {
            std::uint64_t word = 0;
            for (std::size_t b = 0; b != 64; ++b)
                word |= std::uint64_t{src[w * 64 + b]} << b;
            words_.data()[w] = word;
        }
        if (const auto rem = bits % 64) {
            std::uint64_t word = 0;
            for (std::size_t b = 0; b != rem; ++b)
                word |= std::uint64_t{src[full * 64 + b]} << b;
            words_.data()[full] = word;
        }
    }

    void fill(bool value = true) noexcept {
        if (!words_.empty()) {
            std::memset(words_.data(), value ? 0xff : 0x00, words_.size() * sizeof(std::uint64_t));
            clear_tail();
        }
    }

    void clear() noexcept {
        words_.clear();
        size_ = 0;
    }

    void release() noexcept {
        words_.release();
        size_ = 0;
    }

    void add(bool value) noexcept {
        const auto offset = size_ % 64;
        if (offset == 0) {
            if (size_ >= max_size)
                fatal("BitVector exceeds addressable bits.");
            words_.add(std::uint64_t{value});
        } else {
            words_.data()[size_ / 64] |= std::uint64_t{value} << offset;
        }
        ++size_;
    }

    void pop() noexcept {
        if (size_ == 0)
            fatal("BitVector index.");
        --size_;
        if (size_ % 64 == 0)
            words_.pop();
        else
            words_.data()[size_ / 64] &= ~(std::uint64_t{1} << (size_ % 64));
    }

    [[nodiscard]] bool test(std::size_t index) const noexcept {
        check_index(index);
        return (words_.data()[index / 64] & (std::uint64_t{1} << (index % 64))) != 0;
    }

    [[nodiscard]] bool front() const noexcept { return test(0); }

    [[nodiscard]] bool back() const noexcept { return test(size_ - 1); }

    // Membership query: absent indices are false, unlike checked test().
    [[nodiscard]] bool contains(std::size_t index) const noexcept {
        return index < size_ && (words_.data()[index / 64] & (std::uint64_t{1} << (index % 64))) != 0;
    }

    [[nodiscard]] bool operator[](std::size_t index) const noexcept { return test(index); }

    void set(std::size_t index, bool value = true) noexcept {
        check_index(index);
        const auto mask = std::uint64_t{1} << (index % 64);
        auto& word = words_.data()[index / 64];
        word = value ? word | mask : word & ~mask;
    }

    void reset(std::size_t index) noexcept { set(index, false); }

    void flip(std::size_t index) noexcept {
        check_index(index);
        words_.data()[index / 64] ^= std::uint64_t{1} << (index % 64);
    }

    void flip() noexcept {
        for (auto& word : words_)
            word = ~word;
        clear_tail();
    }

    void set_range(std::size_t first, std::size_t count, bool value = true) noexcept {
        if (first > size_ || count > size_ - first)
            fatal("BitVector range.");
        write_range(first, count, value);
    }

    [[nodiscard]] std::size_t count() const noexcept {
        std::size_t result{};
        for (auto word : words_)
            result += std::popcount(word);
        return result;
    }

    [[nodiscard]] bool all() const noexcept {
        if (size_ == 0)
            return true;
        const auto full = size_ / 64;
        for (std::size_t i = 0; i != full; ++i)
            if (words_.data()[i] != UINT64_MAX)
                return false;
        if (const auto rem = size_ % 64)
            return words_.data()[full] == low_mask(rem);
        return true;
    }

    [[nodiscard]] bool any() const noexcept {
        for (auto word : words_)
            if (word)
                return true;
        return false;
    }

    [[nodiscard]] bool none() const noexcept { return !any(); }

    [[nodiscard]] bool intersects(const BitVector& other) const noexcept {
        if (size_ != other.size_)
            fatal("BitVector operation requires equal sizes.");
        for (std::size_t i = 0; i != words_.size(); ++i)
            if (words_.data()[i] & other.words_.data()[i])
                return true;
        return false;
    }

    [[nodiscard]] bool is_subset_of(const BitVector& other) const noexcept {
        if (size_ != other.size_)
            fatal("BitVector operation requires equal sizes.");
        for (std::size_t i = 0; i != words_.size(); ++i)
            if (words_.data()[i] & ~other.words_.data()[i])
                return false;
        return true;
    }

    [[nodiscard]] std::size_t find_first() const noexcept { return find_from(0); }

    [[nodiscard]] std::size_t find_from(std::size_t first) const noexcept {
        if (first >= size_)
            return npos;
        auto index = first / 64;
        auto word = words_.data()[index] & (UINT64_MAX << (first % 64));
        for (;;) {
            if (word)
                return index * 64 + std::countr_zero(word);
            if (++index == words_.size())
                return npos;
            word = words_.data()[index];
        }
    }

    // Strictly after index. npos or an index beyond the end yields npos.
    [[nodiscard]] std::size_t find_next(std::size_t index) const noexcept {
        return index >= size_ ? npos : find_from(index + 1);
    }

    [[nodiscard]] std::size_t find_last() const noexcept { return size_ == 0 ? npos : find_last_from(size_ - 1); }

    [[nodiscard]] std::size_t find_last_from(std::size_t last) const noexcept {
        if (size_ == 0)
            return npos;
        if (last >= size_)
            last = size_ - 1;
        auto index = last / 64;
        auto word = words_.data()[index] & low_mask((last % 64) + 1);
        for (;;) {
            if (word)
                return index * 64 + static_cast<std::size_t>(63 - std::countl_zero(word));
            if (index == 0)
                return npos;
            word = words_.data()[--index];
        }
    }

    // Strictly before index. Zero, npos or an index beyond size() yields npos; size() yields find_last().
    [[nodiscard]] std::size_t find_prev(std::size_t index) const noexcept {
        return index == 0 || index > size_ ? npos : find_last_from(index - 1);
    }

    BitVector& operator&=(const BitVector& other) noexcept {
        if (this == &other) {
            return *this;
        }
        return combine_disjoint(other, std::bit_and<>{});
    }

    BitVector& operator|=(const BitVector& other) noexcept {
        if (this == &other) {
            return *this;
        }
        return combine_disjoint(other, std::bit_or<>{});
    }

    BitVector& operator^=(const BitVector& other) noexcept {
        if (this == &other) {
            fill(false);
            return *this;
        }
        return combine_disjoint(other, std::bit_xor<>{});
    }

    BitVector& and_not(const BitVector& other) noexcept {
        if (this == &other) {
            fill(false);
            return *this;
        }
        return combine_disjoint(other, [](std::uint64_t a, std::uint64_t b) noexcept { return a & ~b; });
    }

    [[nodiscard]] bool operator==(const BitVector& other) const noexcept {
        return size_ == other.size_ &&
               (words_.empty() ||
                std::memcmp(words_.data(), other.words_.data(), words_.size() * sizeof(std::uint64_t)) == 0);
    }

    class Iterator {
    public:
        using value_type = std::size_t;

        using difference_type = std::ptrdiff_t;

        using iterator_concept = std::bidirectional_iterator_tag;

        using iterator_category = std::bidirectional_iterator_tag;

        Iterator() noexcept = default;

        Iterator(const BitVector* owner, std::size_t index) noexcept : owner_(owner), index_(index) {}

        std::size_t operator*() const noexcept { return index_; }

        Iterator& operator++() noexcept {
            index_ = owner_->find_next(index_);
            return *this;
        }

        Iterator operator++(int) noexcept {
            auto previous = *this;
            ++*this;
            return previous;
        }

        Iterator& operator--() noexcept {
            index_ = index_ == npos ? owner_->find_last() : owner_->find_prev(index_);
            return *this;
        }

        Iterator operator--(int) noexcept {
            auto previous = *this;
            --*this;
            return previous;
        }

        bool operator==(const Iterator&) const noexcept = default;

    private:
        const BitVector* owner_{};

        std::size_t index_{npos};
    };

    struct SetBits {
        const BitVector* owner{};

        [[nodiscard]] Iterator begin() const noexcept { return {owner, owner ? owner->find_first() : npos}; }

        [[nodiscard]] Iterator end() const noexcept { return {owner, npos}; }

        [[nodiscard]] std::reverse_iterator<Iterator> rbegin() const noexcept {
            return std::reverse_iterator<Iterator>{end()};
        }

        [[nodiscard]] std::reverse_iterator<Iterator> rend() const noexcept {
            return std::reverse_iterator<Iterator>{begin()};
        }

        [[nodiscard]] bool empty() const noexcept { return !owner || owner->none(); }
    };

    [[nodiscard]] SetBits set_bits() const& noexcept [[clang::lifetimebound]] { return {this}; }

    SetBits set_bits() const&& = delete;

private:
    static std::size_t word_count(std::size_t bits) noexcept {
        if (bits > max_size)
            fatal("BitVector exceeds addressable bits.");
        return bits / 64 + (bits % 64 != 0);
    }

    static constexpr std::uint64_t low_mask(std::size_t bits) noexcept {
        return bits == 64 ? UINT64_MAX : (std::uint64_t{1} << bits) - 1;
    }

    void clear_tail() noexcept {
        if (const auto rem = size_ % 64)
            words_.data()[size_ / 64] &= low_mask(rem);
    }

    void check_index(std::size_t index) const noexcept {
        if (index >= size_)
            fatal("BitVector index.");
    }

    void write_range(std::size_t first, std::size_t count, bool value) noexcept {
        if (!count)
            return;
        if (const auto head_offset = first % 64) {
            const auto head_length = std::min(count, 64 - head_offset);
            const auto mask = low_mask(head_length) << head_offset;
            auto& word = words_.data()[first / 64];
            word = value ? (word | mask) : (word & ~mask);
            first += head_length;
            count -= head_length;
        }
        if (const auto full_words = count / 64) {
            std::memset(words_.data() + first / 64, value ? 0xff : 0x00, full_words * sizeof(std::uint64_t));
            first += full_words * 64;
            count %= 64;
        }
        if (count) {
            const auto mask = low_mask(count);
            auto& word = words_.data()[first / 64];
            word = value ? (word | mask) : (word & ~mask);
        }
    }

    template <class Operation>
    BitVector& combine_disjoint(const BitVector& other, Operation operation) noexcept {
        if (size_ != other.size_)
            fatal("BitVector operation requires equal sizes.");
        const auto word_total = words_.size();
        std::uint64_t* __restrict dst = words_.data();
        const std::uint64_t* __restrict src = other.words_.data();
        for (std::size_t i = 0; i != word_total; ++i)
            dst[i] = operation(dst[i], src[i]);
        return *this;
    }

    Array<std::uint64_t> words_;

    std::size_t size_{};
};

template <>
struct Hash<BitVector> {
    using is_avalanching = void;

    [[nodiscard]] std::size_t operator()(const BitVector& bits) const noexcept {
        const auto words = bits.words();
        const auto word_hash =
            detail::hash_byte_range(reinterpret_cast<const std::byte*>(words.data()), words.size_bytes());
        return detail::hash_element_step<std::size_t>(word_hash, bits.size());
    }
};

}

template <>
inline constexpr bool std::ranges::enable_borrowed_range<atelier::BitVector::SetBits> = true;

template <>
inline constexpr bool std::ranges::enable_view<atelier::BitVector::SetBits> = true;
