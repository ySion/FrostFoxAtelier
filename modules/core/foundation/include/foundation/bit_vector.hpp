#pragma once

#include "foundation/array.hpp"

#include <bit>
#include <functional>
#include <iterator>

namespace atelier {

// Packed bits, not an Array<bool> specialization. Unused tail bits stay zero.
// Structural mutation invalidates set_bits() iterators; no atomic operations.
class BitVector {
public:
    static constexpr std::size_t npos = SIZE_MAX;

    BitVector() noexcept = default;

    explicit BitVector(std::size_t size, bool value = false) noexcept { resize(size, value); }

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
        words_.resize(word_count(bits), 0);
        size_ = bits;
        if (bits > previous && value)
            write_range(previous, bits - previous, true);
        if (bits % 64)
            words_.data()[bits / 64] &= low_mask(bits % 64);
    }

    void clear() noexcept {
        words_.clear();
        size_ = 0;
    }

    void release() noexcept {
        words_.release();
        size_ = 0;
    }

    [[nodiscard]] bool test(std::size_t index) const noexcept {
        check_index(index);
        return (words_.data()[index / 64] & (std::uint64_t{1} << (index % 64))) != 0;
    }

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

    [[nodiscard]] bool any() const noexcept {
        for (auto word : words_)
            if (word)
                return true;
        return false;
    }

    [[nodiscard]] bool none() const noexcept { return !any(); }

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

    BitVector& operator&=(const BitVector& other) noexcept { return combine(other, std::bit_and<>{}); }

    BitVector& operator|=(const BitVector& other) noexcept { return combine(other, std::bit_or<>{}); }

    BitVector& operator^=(const BitVector& other) noexcept { return combine(other, std::bit_xor<>{}); }

    class Iterator {
    public:
        using value_type = std::size_t;

        using difference_type = std::ptrdiff_t;

        using iterator_concept = std::forward_iterator_tag;

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

        bool operator==(const Iterator&) const noexcept = default;

    private:
        const BitVector* owner_{};

        std::size_t index_{npos};
    };

    struct SetBits {
        const BitVector* owner;

        Iterator begin() const noexcept { return {owner, owner->find_first()}; }

        Iterator end() const noexcept { return {owner, npos}; }
    };

    [[nodiscard]] SetBits set_bits() const& noexcept [[clang::lifetimebound]] { return {this}; }

    SetBits set_bits() const&& = delete;

private:
    static constexpr std::size_t word_count(std::size_t bits) noexcept { return bits / 64 + (bits % 64 != 0); }

    static constexpr std::uint64_t low_mask(std::size_t bits) noexcept {
        return bits == 64 ? UINT64_MAX : (std::uint64_t{1} << bits) - 1;
    }

    void check_index(std::size_t index) const noexcept {
        if (index >= size_)
            fatal("BitVector index.");
    }

    void write_range(std::size_t first, std::size_t count, bool value) noexcept {
        while (count) {
            const auto offset = first % 64;
            const auto length = std::min(count, 64 - offset);
            const auto mask = low_mask(length) << offset;
            auto& word = words_.data()[first / 64];
            word = value ? word | mask : word & ~mask;
            first += length;
            count -= length;
        }
    }

    template <class Operation>
    BitVector& combine(const BitVector& other, Operation operation) noexcept {
        if (size_ != other.size_)
            fatal("BitVector operation requires equal sizes.");
        for (std::size_t i = 0; i != words_.size(); ++i)
            words_.data()[i] = operation(words_.data()[i], other.words_.data()[i]);
        return *this;
    }

    Array<std::uint64_t> words_;

    std::size_t size_{};
};

}
