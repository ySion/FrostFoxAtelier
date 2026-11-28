#pragma once

#include "foundation/span.hpp"
#include "memory/fatal.hpp"

#include <bit>
#include <charconv>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>
#include <expected>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <ranges>
#include <string_view>
#include <type_traits>

namespace atelier {

class String;

class StringView;

enum class StringError : std::uint8_t { invalid_utf8 };

struct StringPartition;

class StringSplit;

class StringSplitAny;

class StringLines;

class CodePointRange;

class ByteSearchSet;

namespace detail {
template <class T>
concept TextInteger = std::integral<T> && requires(const char* input, char* output, T& value) {
    std::from_chars(input, input, value, 10);
    std::to_chars(output, output, value, 10);
};

constexpr char ascii_lower(char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

constexpr char ascii_upper(char value) noexcept {
    return value >= 'a' && value <= 'z' ? static_cast<char>(value - ('a' - 'A')) : value;
}

// Maps ['A', 'Z'] (0x41..0x5A) onto signed [-128, -103] so a single signed pcmpgtb
// isolates ASCII uppercase bytes without touching '@', '[', or UTF-8 0x80..0xFF.
[[nodiscard]] inline __m128i simd_ascii_lower(__m128i chunk) noexcept {
    const auto shifted = _mm_sub_epi8(chunk, _mm_set1_epi8(static_cast<char>(0xc1)));
    const auto is_upper = _mm_cmpgt_epi8(_mm_set1_epi8(-102), shifted);
    return _mm_or_si128(chunk, _mm_and_si128(is_upper, _mm_set1_epi8(0x20)));
}

[[nodiscard]] inline __m128i simd_ascii_upper(__m128i chunk) noexcept {
    const auto shifted = _mm_sub_epi8(chunk, _mm_set1_epi8(static_cast<char>(0xe1)));
    const auto is_lower = _mm_cmpgt_epi8(_mm_set1_epi8(-102), shifted);
    return _mm_andnot_si128(_mm_and_si128(is_lower, _mm_set1_epi8(0x20)), chunk);
}
}

// Precomputed 256-bit byte lookup table with 1..4 distinct-byte and contiguous-range+extra
// SSE2 fast-path metadata (modeled after .NET SearchValues<byte>).
class ByteSearchSet {
public:
    enum class Mode : std::uint8_t {
        empty = 0,
        small_1 = 1,
        small_2 = 2,
        small_3 = 3,
        small_4 = 4,
        range_extra = 5,
        bitmap = 6
    };

    constexpr ByteSearchSet() noexcept = default;

    constexpr ByteSearchSet(StringView bytes) noexcept;

    template <class P>
        requires(std::same_as<std::decay_t<P>, const char*> || std::same_as<std::decay_t<P>, char*>)
    constexpr ByteSearchSet(P&& bytes) noexcept;

    ByteSearchSet(const String& bytes) noexcept;

    [[nodiscard]] constexpr bool empty() const noexcept { return mode_ == Mode::empty; }

    [[nodiscard]] constexpr bool contains(char value) const noexcept {
        const auto byte = static_cast<unsigned char>(value);
        return ((bits_[byte >> 6] >> (byte & 63u)) & std::uint64_t{1}) != 0;
    }

private:
    friend class StringView;

    struct SimdMatcher {
        __m128i k0;

        __m128i k1;

        __m128i k2;

        __m128i k3;

        Mode mode;

        [[nodiscard]] __m128i match(__m128i v) const noexcept {
            if (mode == Mode::range_extra) {
                const auto in_range = _mm_cmpgt_epi8(k2, _mm_sub_epi8(v, k1));
                return _mm_or_si128(in_range, _mm_cmpeq_epi8(v, k0));
            }
            auto m = _mm_or_si128(_mm_cmpeq_epi8(v, k0), _mm_cmpeq_epi8(v, k1));
            if (mode >= Mode::small_3)
                m = _mm_or_si128(m, _mm_cmpeq_epi8(v, k2));
            if (mode == Mode::small_4)
                m = _mm_or_si128(m, _mm_cmpeq_epi8(v, k3));
            return m;
        }
    };

    [[nodiscard]] SimdMatcher simd_matcher() const noexcept {
        if (mode_ == Mode::range_extra) {
            return {_mm_set1_epi8(small_[0]), _mm_set1_epi8(small_[1]), _mm_set1_epi8(small_[2]), _mm_setzero_si128(),
                    mode_};
        }
        return {_mm_set1_epi8(small_[0]), _mm_set1_epi8(small_[1]),
                _mm_set1_epi8(mode_ >= Mode::small_3 ? small_[2] : small_[1]),
                _mm_set1_epi8(mode_ == Mode::small_4 ? small_[3] : small_[1]), mode_};
    }

    std::uint64_t bits_[4]{};

    char small_[4]{};

    Mode mode_{Mode::empty};
};

static_assert(sizeof(ByteSearchSet) == 40 && alignof(ByteSearchSet) == 8);

// A byte range interpreted as UTF-8 by text consumers. A view may cover external,
// unvalidated bytes or a partial code point; it never promises a trailing zero.
class StringView {
public:
    static constexpr std::size_t npos = std::string_view::npos;

    constexpr StringView() noexcept = default;

    constexpr StringView(const char* data, std::size_t size) noexcept : data_(data ? data : ""), size_(size) {
        if ((!data && size) || size > static_cast<std::size_t>(PTRDIFF_MAX))
            fatal("Invalid StringView storage.");
    }

    constexpr StringView(const char* text) noexcept
        : StringView(text, text ? std::char_traits<char>::length(text) : 0) {}

    explicit constexpr StringView(std::string_view text) noexcept : StringView(text.data(), text.size()) {}

    StringView(const String& text [[clang::lifetimebound]]) noexcept;

    StringView(String&&) = delete;

    StringView(const String&&) = delete;

    [[nodiscard]] constexpr const char* data() const noexcept { return data_; }

    [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }

    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }

    [[nodiscard]] constexpr const char* begin() const noexcept { return data_; }

    [[nodiscard]] constexpr const char* end() const noexcept { return data_ + size_; }

    [[nodiscard]] constexpr char operator[](std::size_t index) const noexcept {
        if (index >= size_)
            fatal("StringView index.");
        return data_[index];
    }

    [[nodiscard]] constexpr char front() const noexcept { return (*this)[0]; }

    [[nodiscard]] constexpr char back() const noexcept { return (*this)[size_ - 1]; }

    [[nodiscard]] constexpr StringView first(std::size_t count) const noexcept {
        if (count > size_)
            fatal("StringView slice.");
        return {Unchecked{}, data_, count};
    }

    [[nodiscard]] constexpr StringView last(std::size_t count) const noexcept {
        if (count > size_)
            fatal("StringView slice.");
        return {Unchecked{}, data_ + (size_ - count), count};
    }

    [[nodiscard]] constexpr StringView slice(std::size_t first, std::size_t count = npos) const noexcept {
        if (first > size_ || (count != npos && count > size_ - first))
            fatal("StringView slice.");
        return {Unchecked{}, data_ + first, count == npos ? size_ - first : count};
    }

    constexpr void remove_prefix(std::size_t count) noexcept {
        if (count > size_)
            fatal("StringView slice.");
        data_ += count;
        size_ -= count;
    }

    constexpr void remove_suffix(std::size_t count) noexcept {
        if (count > size_)
            fatal("StringView slice.");
        size_ -= count;
    }

    constexpr bool strip_prefix(StringView prefix) noexcept {
        if (!starts_with(prefix))
            return false;
        data_ += prefix.size_;
        size_ -= prefix.size_;
        return true;
    }

    constexpr bool strip_prefix(char prefix) noexcept {
        if (!starts_with(prefix))
            return false;
        ++data_;
        --size_;
        return true;
    }

    constexpr bool strip_suffix(StringView suffix) noexcept {
        if (!ends_with(suffix))
            return false;
        size_ -= suffix.size_;
        return true;
    }

    constexpr bool strip_suffix(char suffix) noexcept {
        if (!ends_with(suffix))
            return false;
        --size_;
        return true;
    }

    [[nodiscard]] constexpr StringView without_prefix(StringView prefix) const noexcept {
        return starts_with(prefix) ? StringView{Unchecked{}, data_ + prefix.size_, size_ - prefix.size_} : *this;
    }

    [[nodiscard]] constexpr StringView without_prefix(char prefix) const noexcept {
        return starts_with(prefix) ? StringView{Unchecked{}, data_ + 1, size_ - 1} : *this;
    }

    [[nodiscard]] constexpr StringView without_suffix(StringView suffix) const noexcept {
        return ends_with(suffix) ? StringView{Unchecked{}, data_, size_ - suffix.size_} : *this;
    }

    [[nodiscard]] constexpr StringView without_suffix(char suffix) const noexcept {
        return ends_with(suffix) ? StringView{Unchecked{}, data_, size_ - 1} : *this;
    }

    // The entire view must already be valid UTF-8. Only the cut is adjusted;
    // this does not validate bytes or detect a violated precondition.
    [[nodiscard]] constexpr StringView utf8_prefix(std::size_t maximum_bytes) const noexcept {
        if (size_ <= maximum_bytes)
            return *this;
        while (maximum_bytes && (static_cast<unsigned char>(data_[maximum_bytes]) & 0xc0) == 0x80)
            --maximum_bytes;
        return {Unchecked{}, data_, maximum_bytes};
    }

    // Cuts at a code point boundary when the view exceeds maximum_bytes and is valid UTF-8;
    // returns the view untouched when within limit, or a raw byte prefix when malformed so
    // downstream UTF-8 validators can still detect invalid input.
    [[nodiscard]] constexpr StringView truncate_utf8_or_bytes(std::size_t maximum_bytes) const noexcept {
        if (size_ <= maximum_bytes)
            return *this;
        if (is_valid_utf8())
            return utf8_prefix(maximum_bytes);
        return {Unchecked{}, data_, maximum_bytes};
    }

    [[nodiscard]] constexpr bool starts_with(StringView value) const noexcept {
        return std_view().starts_with(value.std_view());
    }

    [[nodiscard]] constexpr bool ends_with(StringView value) const noexcept {
        return std_view().ends_with(value.std_view());
    }

    [[nodiscard]] constexpr bool starts_with(char value) const noexcept { return std_view().starts_with(value); }

    [[nodiscard]] constexpr bool ends_with(char value) const noexcept { return std_view().ends_with(value); }

    [[nodiscard]] constexpr bool starts_with_any(std::initializer_list<StringView> values) const noexcept {
        for (auto value : values)
            if (starts_with(value))
                return true;
        return false;
    }

    [[nodiscard]] constexpr bool ends_with_any(std::initializer_list<StringView> values) const noexcept {
        for (auto value : values)
            if (ends_with(value))
                return true;
        return false;
    }

    [[nodiscard]] constexpr bool is_ascii() const noexcept {
        std::size_t i = 0;
        if !consteval {
            while (size_ - i >= 32) {
                const auto c0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i));
                const auto c1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i + 16));
                if (_mm_movemask_epi8(_mm_or_si128(c0, c1)) != 0)
                    return false;
                i += 32;
            }
            if (size_ - i >= 16) {
                if (_mm_movemask_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i))) != 0)
                    return false;
                i += 16;
            }
            if (size_ - i >= 8) {
                std::uint64_t word{};
                std::memcpy(&word, data_ + i, sizeof(word));
                if ((word & UINT64_C(0x8080808080808080)) != 0)
                    return false;
                i += 8;
            }
            if (size_ - i >= 4) {
                std::uint32_t word{};
                std::memcpy(&word, data_ + i, sizeof(word));
                if ((word & UINT32_C(0x80808080)) != 0)
                    return false;
                i += 4;
            }
        }
        for (; i != size_; ++i)
            if ((static_cast<unsigned char>(data_[i]) & 0x80u) != 0)
                return false;
        return true;
    }

    [[nodiscard]] constexpr bool equals_ascii_case_insensitive(StringView value) const noexcept {
        if (size_ != value.size_)
            return false;
        if (size_ == 0 || data_ == value.data_)
            return true;
        if !consteval {
            if (size_ >= 16) {
                std::size_t i = 0;
                while (size_ - i >= 32) {
                    const auto a0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i));
                    const auto b0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(value.data_ + i));
                    const auto a1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i + 16));
                    const auto b1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(value.data_ + i + 16));
                    const auto exact = _mm_and_si128(_mm_cmpeq_epi8(a0, b0), _mm_cmpeq_epi8(a1, b1));
                    if (_mm_movemask_epi8(exact) != 0xffff) {
                        const auto f0 = _mm_cmpeq_epi8(detail::simd_ascii_lower(a0), detail::simd_ascii_lower(b0));
                        const auto f1 = _mm_cmpeq_epi8(detail::simd_ascii_lower(a1), detail::simd_ascii_lower(b1));
                        if (_mm_movemask_epi8(_mm_and_si128(f0, f1)) != 0xffff)
                            return false;
                    }
                    i += 32;
                }
                if (size_ - i >= 16) {
                    const auto a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i));
                    const auto b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(value.data_ + i));
                    if (_mm_movemask_epi8(_mm_cmpeq_epi8(a, b)) != 0xffff &&
                        _mm_movemask_epi8(_mm_cmpeq_epi8(detail::simd_ascii_lower(a), detail::simd_ascii_lower(b))) !=
                            0xffff)
                        return false;
                    i += 16;
                }
                if (i != size_) {
                    const auto tail = size_ - 16;
                    const auto a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + tail));
                    const auto b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(value.data_ + tail));
                    if (_mm_movemask_epi8(_mm_cmpeq_epi8(a, b)) != 0xffff &&
                        _mm_movemask_epi8(_mm_cmpeq_epi8(detail::simd_ascii_lower(a), detail::simd_ascii_lower(b))) !=
                            0xffff)
                        return false;
                }
                return true;
            }
            if (size_ >= 8) {
                std::uint64_t a0{};
                std::uint64_t a1{};
                std::uint64_t b0{};
                std::uint64_t b1{};
                std::memcpy(&a0, data_, 8);
                std::memcpy(&a1, data_ + size_ - 8, 8);
                std::memcpy(&b0, value.data_, 8);
                std::memcpy(&b1, value.data_ + size_ - 8, 8);
                const auto diff = (a0 ^ b0) | (a1 ^ b1);
                if (diff == 0)
                    return true;
                if ((diff & ~UINT64_C(0x2020202020202020)) != 0)
                    return false;
                const auto va = _mm_set_epi64x(static_cast<long long>(a1), static_cast<long long>(a0));
                const auto vb = _mm_set_epi64x(static_cast<long long>(b1), static_cast<long long>(b0));
                return _mm_movemask_epi8(_mm_cmpeq_epi8(detail::simd_ascii_lower(va), detail::simd_ascii_lower(vb))) ==
                       0xffff;
            }
        }
        for (std::size_t i = 0; i != size_; ++i)
            if (detail::ascii_lower(data_[i]) != detail::ascii_lower(value.data_[i]))
                return false;
        return true;
    }

    [[nodiscard]] constexpr std::strong_ordering compare_ascii_case_insensitive(StringView value) const noexcept {
        const auto limit = size_ < value.size_ ? size_ : value.size_;
        std::size_t i = 0;
        if !consteval {
            if (data_ != value.data_) {
                while (limit - i >= 16) {
                    const auto a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i));
                    const auto b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(value.data_ + i));
                    if (_mm_movemask_epi8(_mm_cmpeq_epi8(a, b)) != 0xffff) {
                        const auto mask = static_cast<unsigned>(_mm_movemask_epi8(
                            _mm_cmpeq_epi8(detail::simd_ascii_lower(a), detail::simd_ascii_lower(b))));
                        if (mask != 0xffffu) {
                            const auto at = i + static_cast<std::size_t>(std::countr_zero(~mask & 0xffffu));
                            return static_cast<unsigned char>(detail::ascii_lower(data_[at])) <=>
                                   static_cast<unsigned char>(detail::ascii_lower(value.data_[at]));
                        }
                    }
                    i += 16;
                }
            } else {
                i = limit;
            }
        }
        for (; i != limit; ++i) {
            const auto left = static_cast<unsigned char>(detail::ascii_lower(data_[i]));
            const auto right = static_cast<unsigned char>(detail::ascii_lower(value.data_[i]));
            if (left != right)
                return left <=> right;
        }
        return size_ <=> value.size_;
    }

    [[nodiscard]] constexpr bool starts_with_ascii_case_insensitive(StringView value) const noexcept {
        return size_ >= value.size_ && StringView{Unchecked{}, data_, value.size_}.equals_ascii_case_insensitive(value);
    }

    [[nodiscard]] constexpr bool ends_with_ascii_case_insensitive(StringView value) const noexcept {
        return size_ >= value.size_ &&
               StringView{Unchecked{}, data_ + size_ - value.size_, value.size_}.equals_ascii_case_insensitive(value);
    }

    // Whole-input parsing, without trimming, locale, leading '+' or base prefixes.
    template <detail::TextInteger T>
    [[nodiscard]] std::expected<T, std::errc> parse_number(int base = 10) const noexcept {
        if (base < 2 || base > 36)
            return std::unexpected(std::errc::invalid_argument);
        T value;
        const auto result = std::from_chars(begin(), end(), value, base);
        if (result.ptr != end())
            return std::unexpected(std::errc::invalid_argument);
        if (result.ec != std::errc{})
            return std::unexpected(result.ec);
        return value;
    }

    template <std::floating_point T>
    [[nodiscard]] std::expected<T, std::errc> parse_number() const noexcept {
        T value;
        const auto result = std::from_chars(begin(), end(), value, std::chars_format::general);
        if (result.ptr != end())
            return std::unexpected(std::errc::invalid_argument);
        if (result.ec != std::errc{})
            return std::unexpected(result.ec);
        return value;
    }

    [[nodiscard]] constexpr std::size_t find(char value, std::size_t from = 0) const noexcept {
        if (from >= size_)
            return npos;
        if consteval {
            return std_view().find(value, from);
        } else {
            const auto rel = detail::simd_find_first(data_ + from, size_ - from, value);
            return rel == npos ? npos : from + rel;
        }
    }

    [[nodiscard]] constexpr std::size_t rfind(char value, std::size_t from = npos) const noexcept {
        if (size_ == 0)
            return npos;
        if consteval {
            return std_view().rfind(value, from);
        } else {
            const auto count = from >= size_ ? size_ : from + 1;
            return detail::simd_find_last(data_, count, value);
        }
    }

    [[nodiscard]] constexpr std::size_t find(StringView value, std::size_t from = 0) const noexcept {
        if (from > size_ || value.size_ > size_ - from)
            return npos;
        if (value.size_ == 0)
            return from;
        if (value.size_ == 1)
            return find(value.data_[0], from);
        if consteval {
            return std_view().find(value.std_view(), from);
        } else {
            const auto last_offset = value.size_ - 1;
            const auto candidates = size_ - last_offset;
            std::size_t i = from;
            if (candidates - i >= 16) {
                const auto first_vec = _mm_set1_epi8(value.data_[0]);
                const auto last_vec = _mm_set1_epi8(value.data_[last_offset]);
                const std::size_t body_bytes = value.size_ - 2;
                while (candidates - i >= 16) {
                    const auto c_first =
                        _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i)), first_vec);
                    const auto c_last = _mm_cmpeq_epi8(
                        _mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i + last_offset)), last_vec);
                    auto mask = static_cast<unsigned>(_mm_movemask_epi8(_mm_and_si128(c_first, c_last)));
                    while (mask != 0) {
                        const auto bit = static_cast<std::size_t>(std::countr_zero(mask));
                        if (body_bytes == 0 || std::memcmp(data_ + i + bit + 1, value.data_ + 1, body_bytes) == 0)
                            return i + bit;
                        mask &= mask - 1;
                    }
                    i += 16;
                }
            }
            const char first_byte = value.data_[0];
            const char last_byte = value.data_[last_offset];
            const std::size_t body_bytes = value.size_ - 2;
            for (; i != candidates; ++i) {
                if (data_[i] == first_byte && data_[i + last_offset] == last_byte &&
                    (body_bytes == 0 || std::memcmp(data_ + i + 1, value.data_ + 1, body_bytes) == 0))
                    return i;
            }
            return npos;
        }
    }

    [[nodiscard]] constexpr std::size_t rfind(StringView value, std::size_t from = npos) const noexcept {
        if (value.size_ > size_)
            return npos;
        const auto max_start = from < size_ - value.size_ ? from : size_ - value.size_;
        if (value.size_ == 0)
            return max_start;
        if (value.size_ == 1)
            return rfind(value.data_[0], max_start);
        if consteval {
            return std_view().rfind(value.std_view(), from);
        } else {
            const auto last_offset = value.size_ - 1;
            const std::size_t body_bytes = value.size_ - 2;
            std::size_t end_candidates = max_start + 1;
            if (end_candidates >= 16) {
                const auto first_vec = _mm_set1_epi8(value.data_[0]);
                const auto last_vec = _mm_set1_epi8(value.data_[last_offset]);
                while (end_candidates >= 16) {
                    const auto base = end_candidates - 16;
                    const auto c_first =
                        _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + base)), first_vec);
                    const auto c_last = _mm_cmpeq_epi8(
                        _mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + base + last_offset)), last_vec);
                    auto mask = static_cast<unsigned>(_mm_movemask_epi8(_mm_and_si128(c_first, c_last)));
                    while (mask != 0) {
                        const auto bit = 31u - static_cast<unsigned>(std::countl_zero(mask));
                        if (body_bytes == 0 || std::memcmp(data_ + base + bit + 1, value.data_ + 1, body_bytes) == 0)
                            return base + bit;
                        mask ^= (1u << bit);
                    }
                    end_candidates = base;
                }
            }
            const char first_byte = value.data_[0];
            const char last_byte = value.data_[last_offset];
            while (end_candidates != 0) {
                --end_candidates;
                if (data_[end_candidates] == first_byte && data_[end_candidates + last_offset] == last_byte &&
                    (body_bytes == 0 || std::memcmp(data_ + end_candidates + 1, value.data_ + 1, body_bytes) == 0))
                    return end_candidates;
            }
            return npos;
        }
    }

    [[nodiscard]] constexpr std::size_t find_ascii_case_insensitive(StringView value,
                                                                    std::size_t from = 0) const noexcept {
        if (from > size_ || value.size_ > size_ - from)
            return npos;
        if (value.size_ == 0)
            return from;
        const char first_lo = detail::ascii_lower(value.data_[0]);
        const auto last_offset = value.size_ - 1;
        const char last_lo = detail::ascii_lower(value.data_[last_offset]);
        const auto candidates = size_ - last_offset;
        std::size_t i = from;
        if !consteval {
            if (candidates - i >= 16) {
                const auto first_vec = _mm_set1_epi8(first_lo);
                const auto last_vec = _mm_set1_epi8(last_lo);
                while (candidates - i >= 16) {
                    const auto a_first =
                        detail::simd_ascii_lower(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i)));
                    if (value.size_ == 1) {
                        const auto mask = static_cast<unsigned>(_mm_movemask_epi8(_mm_cmpeq_epi8(a_first, first_vec)));
                        if (mask != 0)
                            return i + static_cast<std::size_t>(std::countr_zero(mask));
                    } else {
                        const auto a_last = detail::simd_ascii_lower(
                            _mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i + last_offset)));
                        auto mask = static_cast<unsigned>(_mm_movemask_epi8(
                            _mm_and_si128(_mm_cmpeq_epi8(a_first, first_vec), _mm_cmpeq_epi8(a_last, last_vec))));
                        while (mask != 0) {
                            const auto bit = static_cast<std::size_t>(std::countr_zero(mask));
                            if (StringView{Unchecked{}, data_ + i + bit, value.size_}.equals_ascii_case_insensitive(
                                    value))
                                return i + bit;
                            mask &= mask - 1;
                        }
                    }
                    i += 16;
                }
            }
        }
        for (; i != candidates; ++i) {
            if (detail::ascii_lower(data_[i]) == first_lo &&
                detail::ascii_lower(data_[i + last_offset]) == last_lo &&
                StringView{Unchecked{}, data_ + i, value.size_}.equals_ascii_case_insensitive(value))
                return i;
        }
        return npos;
    }

    [[nodiscard]] constexpr std::size_t rfind_ascii_case_insensitive(StringView value,
                                                                     std::size_t from = npos) const noexcept {
        if (value.size_ > size_)
            return npos;
        const auto max_start = from < size_ - value.size_ ? from : size_ - value.size_;
        if (value.size_ == 0)
            return max_start;
        const char first_lo = detail::ascii_lower(value.data_[0]);
        const auto last_offset = value.size_ - 1;
        const char last_lo = detail::ascii_lower(value.data_[last_offset]);
        std::size_t end_candidates = max_start + 1;
        if !consteval {
            if (end_candidates >= 16) {
                const auto first_vec = _mm_set1_epi8(first_lo);
                const auto last_vec = _mm_set1_epi8(last_lo);
                while (end_candidates >= 16) {
                    const auto base = end_candidates - 16;
                    const auto a_first =
                        detail::simd_ascii_lower(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + base)));
                    if (value.size_ == 1) {
                        const auto mask = static_cast<unsigned>(_mm_movemask_epi8(_mm_cmpeq_epi8(a_first, first_vec)));
                        if (mask != 0)
                            return base + (31u - static_cast<unsigned>(std::countl_zero(mask)));
                    } else {
                        const auto a_last = detail::simd_ascii_lower(
                            _mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + base + last_offset)));
                        auto mask = static_cast<unsigned>(_mm_movemask_epi8(
                            _mm_and_si128(_mm_cmpeq_epi8(a_first, first_vec), _mm_cmpeq_epi8(a_last, last_vec))));
                        while (mask != 0) {
                            const auto bit = 31u - static_cast<unsigned>(std::countl_zero(mask));
                            if (StringView{Unchecked{}, data_ + base + bit, value.size_}.equals_ascii_case_insensitive(
                                    value))
                                return base + bit;
                            mask ^= (1u << bit);
                        }
                    }
                    end_candidates = base;
                }
            }
        }
        while (end_candidates != 0) {
            --end_candidates;
            if (detail::ascii_lower(data_[end_candidates]) == first_lo &&
                detail::ascii_lower(data_[end_candidates + last_offset]) == last_lo &&
                StringView{Unchecked{}, data_ + end_candidates, value.size_}.equals_ascii_case_insensitive(value))
                return end_candidates;
        }
        return npos;
    }

    [[nodiscard]] constexpr bool contains_ascii_case_insensitive(StringView value) const noexcept {
        return find_ascii_case_insensitive(value) != npos;
    }

    [[nodiscard]] constexpr std::size_t find_first_of(const ByteSearchSet& set, std::size_t from = 0) const noexcept {
        if (from >= size_ || set.empty())
            return npos;
        if (set.mode_ == ByteSearchSet::Mode::small_1)
            return find(set.small_[0], from);
        std::size_t i = from;
        if !consteval {
            if (set.mode_ != ByteSearchSet::Mode::bitmap && size_ - i >= 16) {
                const auto matcher = set.simd_matcher();
                while (size_ - i >= 32) {
                    const auto m0 = matcher.match(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i)));
                    const auto m1 = matcher.match(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i + 16)));
                    if (_mm_movemask_epi8(_mm_or_si128(m0, m1)) != 0) {
                        const auto b0 = static_cast<unsigned>(_mm_movemask_epi8(m0));
                        if (b0 != 0)
                            return i + static_cast<std::size_t>(std::countr_zero(b0));
                        const auto b1 = static_cast<unsigned>(_mm_movemask_epi8(m1));
                        [[assume(b1 != 0)]];
                        return i + 16 + static_cast<std::size_t>(std::countr_zero(b1));
                    }
                    i += 32;
                }
                if (size_ - i >= 16) {
                    const auto m = matcher.match(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i)));
                    const auto b = static_cast<unsigned>(_mm_movemask_epi8(m));
                    if (b != 0)
                        return i + static_cast<std::size_t>(std::countr_zero(b));
                    i += 16;
                }
            }
        }
        for (; i != size_; ++i)
            if (set.contains(data_[i]))
                return i;
        return npos;
    }

    [[nodiscard]] constexpr std::size_t find_last_of(const ByteSearchSet& set, std::size_t from = npos) const noexcept {
        if (size_ == 0 || set.empty())
            return npos;
        if (set.mode_ == ByteSearchSet::Mode::small_1)
            return rfind(set.small_[0], from);
        std::size_t end_pos = from >= size_ ? size_ : from + 1;
        if !consteval {
            if (set.mode_ != ByteSearchSet::Mode::bitmap && end_pos >= 16) {
                const auto matcher = set.simd_matcher();
                while (end_pos >= 32) {
                    const auto base = end_pos - 32;
                    const auto m0 = matcher.match(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + base)));
                    const auto m1 = matcher.match(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + base + 16)));
                    if (_mm_movemask_epi8(_mm_or_si128(m0, m1)) != 0) {
                        const auto b1 = static_cast<unsigned>(_mm_movemask_epi8(m1));
                        if (b1 != 0)
                            return base + 16 + (31u - static_cast<unsigned>(std::countl_zero(b1)));
                        const auto b0 = static_cast<unsigned>(_mm_movemask_epi8(m0));
                        [[assume(b0 != 0)]];
                        return base + (31u - static_cast<unsigned>(std::countl_zero(b0)));
                    }
                    end_pos = base;
                }
                if (end_pos >= 16) {
                    const auto base = end_pos - 16;
                    const auto m = matcher.match(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + base)));
                    const auto b = static_cast<unsigned>(_mm_movemask_epi8(m));
                    if (b != 0)
                        return base + (31u - static_cast<unsigned>(std::countl_zero(b)));
                    end_pos = base;
                }
            }
        }
        while (end_pos != 0) {
            --end_pos;
            if (set.contains(data_[end_pos]))
                return end_pos;
        }
        return npos;
    }

    template <std::same_as<char> C>
    [[nodiscard]] constexpr std::size_t find_first_not_of(C value, std::size_t from = 0) const noexcept {
        if (from >= size_)
            return npos;
        if (data_[from] != value)
            return from;
        std::size_t i = from + 1;
        if !consteval {
            if (size_ - i >= 16) {
                const auto needle = _mm_set1_epi8(value);
                while (size_ - i >= 32) {
                    const auto c0 = _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i)), needle);
                    const auto c1 =
                        _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i + 16)), needle);
                    if (_mm_movemask_epi8(_mm_and_si128(c0, c1)) != 0xffff) {
                        const auto m0 = (~static_cast<unsigned>(_mm_movemask_epi8(c0))) & 0xffffu;
                        if (m0 != 0)
                            return i + static_cast<std::size_t>(std::countr_zero(m0));
                        const auto m1 = (~static_cast<unsigned>(_mm_movemask_epi8(c1))) & 0xffffu;
                        [[assume(m1 != 0)]];
                        return i + 16 + static_cast<std::size_t>(std::countr_zero(m1));
                    }
                    i += 32;
                }
                if (size_ - i >= 16) {
                    const auto m = (~static_cast<unsigned>(_mm_movemask_epi8(
                                       _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i)), needle)))) &
                                   0xffffu;
                    if (m != 0)
                        return i + static_cast<std::size_t>(std::countr_zero(m));
                    i += 16;
                }
            }
        }
        for (; i != size_; ++i)
            if (data_[i] != value)
                return i;
        return npos;
    }

    template <std::same_as<char> C>
    [[nodiscard]] constexpr std::size_t find_last_not_of(C value, std::size_t from = npos) const noexcept {
        if (size_ == 0)
            return npos;
        std::size_t end_pos = from >= size_ ? size_ : from + 1;
        if (data_[end_pos - 1] != value)
            return end_pos - 1;
        --end_pos;
        if !consteval {
            if (end_pos >= 16) {
                const auto needle = _mm_set1_epi8(value);
                while (end_pos >= 32) {
                    const auto base = end_pos - 32;
                    const auto c0 =
                        _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + base)), needle);
                    const auto c1 =
                        _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + base + 16)), needle);
                    if (_mm_movemask_epi8(_mm_and_si128(c0, c1)) != 0xffff) {
                        const auto m1 = (~static_cast<unsigned>(_mm_movemask_epi8(c1))) & 0xffffu;
                        if (m1 != 0)
                            return base + 16 + (31u - static_cast<unsigned>(std::countl_zero(m1)));
                        const auto m0 = (~static_cast<unsigned>(_mm_movemask_epi8(c0))) & 0xffffu;
                        [[assume(m0 != 0)]];
                        return base + (31u - static_cast<unsigned>(std::countl_zero(m0)));
                    }
                    end_pos = base;
                }
                if (end_pos >= 16) {
                    const auto base = end_pos - 16;
                    const auto m =
                        (~static_cast<unsigned>(_mm_movemask_epi8(
                            _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + base)), needle)))) &
                        0xffffu;
                    if (m != 0)
                        return base + (31u - static_cast<unsigned>(std::countl_zero(m)));
                    end_pos = base;
                }
            }
        }
        while (end_pos != 0) {
            --end_pos;
            if (data_[end_pos] != value)
                return end_pos;
        }
        return npos;
    }

    [[nodiscard]] constexpr std::size_t find_first_not_of(const ByteSearchSet& set,
                                                          std::size_t from = 0) const noexcept {
        if (from >= size_)
            return npos;
        if (set.empty())
            return from;
        if (set.mode_ == ByteSearchSet::Mode::small_1)
            return find_first_not_of(set.small_[0], from);
        if (!set.contains(data_[from]))
            return from;
        std::size_t i = from + 1;
        if !consteval {
            if (set.mode_ != ByteSearchSet::Mode::bitmap && size_ - i >= 16) {
                const auto matcher = set.simd_matcher();
                while (size_ - i >= 32) {
                    const auto m0 = matcher.match(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i)));
                    const auto m1 = matcher.match(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i + 16)));
                    if (_mm_movemask_epi8(_mm_and_si128(m0, m1)) != 0xffff) {
                        const auto out0 = (~static_cast<unsigned>(_mm_movemask_epi8(m0))) & 0xffffu;
                        if (out0 != 0)
                            return i + static_cast<std::size_t>(std::countr_zero(out0));
                        const auto out1 = (~static_cast<unsigned>(_mm_movemask_epi8(m1))) & 0xffffu;
                        [[assume(out1 != 0)]];
                        return i + 16 + static_cast<std::size_t>(std::countr_zero(out1));
                    }
                    i += 32;
                }
                if (size_ - i >= 16) {
                    const auto m = matcher.match(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i)));
                    const auto not_in = (~static_cast<unsigned>(_mm_movemask_epi8(m))) & 0xffffu;
                    if (not_in != 0)
                        return i + static_cast<std::size_t>(std::countr_zero(not_in));
                    i += 16;
                }
            }
        }
        for (; i != size_; ++i)
            if (!set.contains(data_[i]))
                return i;
        return npos;
    }

    [[nodiscard]] constexpr std::size_t find_last_not_of(const ByteSearchSet& set,
                                                         std::size_t from = npos) const noexcept {
        if (size_ == 0)
            return npos;
        std::size_t end_pos = from >= size_ ? size_ : from + 1;
        if (set.empty())
            return end_pos - 1;
        if (set.mode_ == ByteSearchSet::Mode::small_1)
            return find_last_not_of(set.small_[0], from);
        if (!set.contains(data_[end_pos - 1]))
            return end_pos - 1;
        --end_pos;
        if !consteval {
            if (set.mode_ != ByteSearchSet::Mode::bitmap && end_pos >= 16) {
                const auto matcher = set.simd_matcher();
                while (end_pos >= 32) {
                    const auto base = end_pos - 32;
                    const auto m0 = matcher.match(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + base)));
                    const auto m1 = matcher.match(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + base + 16)));
                    if (_mm_movemask_epi8(_mm_and_si128(m0, m1)) != 0xffff) {
                        const auto out1 = (~static_cast<unsigned>(_mm_movemask_epi8(m1))) & 0xffffu;
                        if (out1 != 0)
                            return base + 16 + (31u - static_cast<unsigned>(std::countl_zero(out1)));
                        const auto out0 = (~static_cast<unsigned>(_mm_movemask_epi8(m0))) & 0xffffu;
                        [[assume(out0 != 0)]];
                        return base + (31u - static_cast<unsigned>(std::countl_zero(out0)));
                    }
                    end_pos = base;
                }
                if (end_pos >= 16) {
                    const auto base = end_pos - 16;
                    const auto m = matcher.match(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + base)));
                    const auto not_in = (~static_cast<unsigned>(_mm_movemask_epi8(m))) & 0xffffu;
                    if (not_in != 0)
                        return base + (31u - static_cast<unsigned>(std::countl_zero(not_in)));
                    end_pos = base;
                }
            }
        }
        while (end_pos != 0) {
            --end_pos;
            if (!set.contains(data_[end_pos]))
                return end_pos;
        }
        return npos;
    }

    // Unsigned byte range [low, high] inclusive (modeled after .NET IndexOfAnyInRange).
    [[nodiscard]] constexpr std::size_t find_any_in_range(char low, char high, std::size_t from = 0) const noexcept {
        const auto lo = static_cast<unsigned char>(low);
        const auto hi = static_cast<unsigned char>(high);
        if (from >= size_ || lo > hi)
            return npos;
        if (lo == hi)
            return find(low, from);
        const auto width = static_cast<unsigned>(hi - lo);
        if (width == 255u)
            return from;
        if (width == 254u)
            return find_first_not_of(static_cast<char>(static_cast<unsigned char>(lo - 1u)), from);
        std::size_t i = from;
        if !consteval {
            if (size_ - i >= 16) {
                const auto bias = _mm_set1_epi8(static_cast<char>(static_cast<unsigned char>(lo + 0x80u)));
                const auto bound = _mm_set1_epi8(static_cast<char>(-128 + static_cast<int>(width) + 1));
                while (size_ - i >= 32) {
                    const auto v0 = _mm_sub_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i)), bias);
                    const auto v1 =
                        _mm_sub_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i + 16)), bias);
                    const auto c0 = _mm_cmpgt_epi8(bound, v0);
                    const auto c1 = _mm_cmpgt_epi8(bound, v1);
                    if (_mm_movemask_epi8(_mm_or_si128(c0, c1)) != 0) {
                        const auto m0 = static_cast<unsigned>(_mm_movemask_epi8(c0));
                        if (m0 != 0)
                            return i + static_cast<std::size_t>(std::countr_zero(m0));
                        const auto m1 = static_cast<unsigned>(_mm_movemask_epi8(c1));
                        [[assume(m1 != 0)]];
                        return i + 16 + static_cast<std::size_t>(std::countr_zero(m1));
                    }
                    i += 32;
                }
                if (size_ - i >= 16) {
                    const auto v = _mm_sub_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i)), bias);
                    const auto m = static_cast<unsigned>(_mm_movemask_epi8(_mm_cmpgt_epi8(bound, v)));
                    if (m != 0)
                        return i + static_cast<std::size_t>(std::countr_zero(m));
                    i += 16;
                }
            }
        }
        for (; i != size_; ++i) {
            const auto b = static_cast<unsigned char>(data_[i]);
            if (b >= lo && b <= hi)
                return i;
        }
        return npos;
    }

    [[nodiscard]] constexpr std::size_t find_any_except_range(char low, char high,
                                                              std::size_t from = 0) const noexcept {
        const auto lo = static_cast<unsigned char>(low);
        const auto hi = static_cast<unsigned char>(high);
        if (from >= size_)
            return npos;
        if (lo > hi)
            return from;
        if (lo == hi)
            return find_first_not_of(low, from);
        const auto width = static_cast<unsigned>(hi - lo);
        if (width == 255u)
            return npos;
        if (width == 254u)
            return find(static_cast<char>(static_cast<unsigned char>(lo - 1u)), from);
        std::size_t i = from;
        if !consteval {
            if (size_ - i >= 16) {
                const auto bias = _mm_set1_epi8(static_cast<char>(static_cast<unsigned char>(lo + 0x80u)));
                const auto max_in = _mm_set1_epi8(static_cast<char>(-128 + static_cast<int>(width)));
                while (size_ - i >= 32) {
                    const auto v0 = _mm_sub_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i)), bias);
                    const auto v1 =
                        _mm_sub_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i + 16)), bias);
                    const auto c0 = _mm_cmpgt_epi8(v0, max_in);
                    const auto c1 = _mm_cmpgt_epi8(v1, max_in);
                    if (_mm_movemask_epi8(_mm_or_si128(c0, c1)) != 0) {
                        const auto m0 = static_cast<unsigned>(_mm_movemask_epi8(c0));
                        if (m0 != 0)
                            return i + static_cast<std::size_t>(std::countr_zero(m0));
                        const auto m1 = static_cast<unsigned>(_mm_movemask_epi8(c1));
                        [[assume(m1 != 0)]];
                        return i + 16 + static_cast<std::size_t>(std::countr_zero(m1));
                    }
                    i += 32;
                }
                if (size_ - i >= 16) {
                    const auto v = _mm_sub_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i)), bias);
                    const auto m = static_cast<unsigned>(_mm_movemask_epi8(_mm_cmpgt_epi8(v, max_in)));
                    if (m != 0)
                        return i + static_cast<std::size_t>(std::countr_zero(m));
                    i += 16;
                }
            }
        }
        for (; i != size_; ++i) {
            const auto b = static_cast<unsigned char>(data_[i]);
            if (b < lo || b > hi)
                return i;
        }
        return npos;
    }

    [[nodiscard]] constexpr bool contains(StringView value) const noexcept { return find(value) != npos; }

    [[nodiscard]] constexpr bool contains(char value) const noexcept { return find(value) != npos; }

    [[nodiscard]] constexpr bool contains_any(const ByteSearchSet& set) const noexcept {
        return find_first_of(set) != npos;
    }

    [[nodiscard]] constexpr bool contains_only(const ByteSearchSet& set) const noexcept {
        return find_first_not_of(set) == npos;
    }

    [[nodiscard]] constexpr std::size_t count(char value) const noexcept {
        std::size_t total = 0;
        std::size_t i = 0;
        if !consteval {
            if (size_ >= 16) {
                const auto needle = _mm_set1_epi8(value);
                const auto zero = _mm_setzero_si128();
                while (size_ - i >= 16) {
                    const auto remaining_vecs = (size_ - i) >> 4;
                    const auto batch = remaining_vecs < 255 ? remaining_vecs : std::size_t{255};
                    auto acc = zero;
                    for (std::size_t v = 0; v != batch; ++v, i += 16) {
                        const auto eq =
                            _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i)), needle);
                        acc = _mm_sub_epi8(acc, eq);
                    }
                    const auto sad = _mm_sad_epu8(acc, zero);
                    total += static_cast<std::size_t>(_mm_cvtsi128_si64(sad) +
                                                      _mm_cvtsi128_si64(_mm_srli_si128(sad, 8)));
                }
            }
        }
        for (; i != size_; ++i)
            if (data_[i] == value)
                ++total;
        return total;
    }

    // Non-overlapping substring count; empty target returns 0 (matching .NET MemoryExtensions.Count).
    [[nodiscard]] constexpr std::size_t count(StringView value) const noexcept {
        if (value.empty() || value.size_ > size_)
            return 0;
        if (value.size_ == 1)
            return count(value.data_[0]);
        std::size_t total = 0;
        std::size_t pos = 0;
        while ((pos = find(value, pos)) != npos) {
            ++total;
            pos += value.size_;
        }
        return total;
    }

    [[nodiscard]] constexpr std::size_t common_prefix_length(StringView other) const noexcept {
        const auto limit = size_ < other.size_ ? size_ : other.size_;
        if (data_ == other.data_)
            return limit;
        std::size_t i = 0;
        if !consteval {
            while (limit - i >= 32) {
                const auto c0 = _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i)),
                                               _mm_loadu_si128(reinterpret_cast<const __m128i*>(other.data_ + i)));
                const auto c1 = _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i + 16)),
                                               _mm_loadu_si128(reinterpret_cast<const __m128i*>(other.data_ + i + 16)));
                if (_mm_movemask_epi8(_mm_and_si128(c0, c1)) != 0xffff) {
                    const auto d0 = (~static_cast<unsigned>(_mm_movemask_epi8(c0))) & 0xffffu;
                    if (d0 != 0)
                        return i + static_cast<std::size_t>(std::countr_zero(d0));
                    const auto d1 = (~static_cast<unsigned>(_mm_movemask_epi8(c1))) & 0xffffu;
                    [[assume(d1 != 0)]];
                    return i + 16 + static_cast<std::size_t>(std::countr_zero(d1));
                }
                i += 32;
            }
            if (limit - i >= 16) {
                const auto c = _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i)),
                                              _mm_loadu_si128(reinterpret_cast<const __m128i*>(other.data_ + i)));
                const auto d = (~static_cast<unsigned>(_mm_movemask_epi8(c))) & 0xffffu;
                if (d != 0)
                    return i + static_cast<std::size_t>(std::countr_zero(d));
                i += 16;
            }
        }
        for (; i != limit && data_[i] == other.data_[i]; ++i) {}
        return i;
    }

    [[nodiscard]] constexpr std::size_t common_suffix_length(StringView other) const noexcept {
        const auto limit = size_ < other.size_ ? size_ : other.size_;
        if (limit == 0)
            return 0;
        if (data_ == other.data_ && size_ == other.size_)
            return limit;
        std::size_t matched = 0;
        if !consteval {
            while (limit - matched >= 32) {
                const auto a_ptr = data_ + size_ - matched - 32;
                const auto b_ptr = other.data_ + other.size_ - matched - 32;
                const auto c0 = _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a_ptr)),
                                               _mm_loadu_si128(reinterpret_cast<const __m128i*>(b_ptr)));
                const auto c1 = _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a_ptr + 16)),
                                               _mm_loadu_si128(reinterpret_cast<const __m128i*>(b_ptr + 16)));
                if (_mm_movemask_epi8(_mm_and_si128(c0, c1)) != 0xffff) {
                    const auto d1 = (~static_cast<unsigned>(_mm_movemask_epi8(c1))) & 0xffffu;
                    if (d1 != 0)
                        return matched + static_cast<std::size_t>(std::countl_zero(d1)) - 16u;
                    const auto d0 = (~static_cast<unsigned>(_mm_movemask_epi8(c0))) & 0xffffu;
                    [[assume(d0 != 0)]];
                    return matched + static_cast<std::size_t>(std::countl_zero(d0));
                }
                matched += 32;
            }
            if (limit - matched >= 16) {
                const auto a_ptr = data_ + size_ - matched - 16;
                const auto b_ptr = other.data_ + other.size_ - matched - 16;
                const auto c = _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a_ptr)),
                                              _mm_loadu_si128(reinterpret_cast<const __m128i*>(b_ptr)));
                const auto d = (~static_cast<unsigned>(_mm_movemask_epi8(c))) & 0xffffu;
                if (d != 0)
                    return matched + static_cast<std::size_t>(std::countl_zero(d)) - 16u;
                matched += 16;
            }
        }
        while (matched != limit && data_[size_ - 1 - matched] == other.data_[other.size_ - 1 - matched])
            ++matched;
        return matched;
    }

    // Byte-set trimming; the default is the six ASCII whitespace bytes, not Unicode whitespace.
    [[nodiscard]] constexpr StringView trim_start() const noexcept;

    [[nodiscard]] constexpr StringView trim_start(const ByteSearchSet& bytes) const noexcept {
        const auto first = find_first_not_of(bytes);
        return first == npos ? StringView{Unchecked{}, data_ + size_, 0}
                             : StringView{Unchecked{}, data_ + first, size_ - first};
    }

    template <std::same_as<char> C>
    [[nodiscard]] constexpr StringView trim_start(C byte) const noexcept {
        const auto first = find_first_not_of(byte);
        return first == npos ? StringView{Unchecked{}, data_ + size_, 0}
                             : StringView{Unchecked{}, data_ + first, size_ - first};
    }

    [[nodiscard]] constexpr StringView trim_end() const noexcept;

    [[nodiscard]] constexpr StringView trim_end(const ByteSearchSet& bytes) const noexcept {
        const auto last = find_last_not_of(bytes);
        return {Unchecked{}, data_, last == npos ? 0 : last + 1};
    }

    template <std::same_as<char> C>
    [[nodiscard]] constexpr StringView trim_end(C byte) const noexcept {
        const auto last = find_last_not_of(byte);
        return {Unchecked{}, data_, last == npos ? 0 : last + 1};
    }

    [[nodiscard]] constexpr StringView trim() const noexcept;

    [[nodiscard]] constexpr StringView trim(const ByteSearchSet& bytes) const noexcept {
        return trim_start(bytes).trim_end(bytes);
    }

    template <std::same_as<char> C>
    [[nodiscard]] constexpr StringView trim(C byte) const noexcept {
        return trim_start(byte).trim_end(byte);
    }

    [[nodiscard]] constexpr StringPartition partition(StringView delimiter) const noexcept;

    [[nodiscard]] constexpr StringPartition partition(char delimiter) const noexcept;

    [[nodiscard]] constexpr StringPartition rpartition(StringView delimiter) const noexcept;

    [[nodiscard]] constexpr StringPartition rpartition(char delimiter) const noexcept;

    // Both source and delimiter remain borrowed throughout iteration. Empty pieces are retained;
    // an empty delimiter yields the whole source once, including an empty source.
    [[nodiscard]] constexpr StringSplit split(StringView delimiter) const noexcept;

    [[nodiscard]] constexpr StringSplitAny split_any(const ByteSearchSet& delimiters,
                                                     bool skip_empty = false) const noexcept;

    [[nodiscard]] constexpr StringSplitAny split_ascii_whitespace() const noexcept;

    // Zero-allocation line enumeration stripping '\r\n', '\n', or standalone '\r'.
    // Empty text yields zero lines; a single trailing line ending at EOF does not emit a phantom line.
    [[nodiscard]] constexpr StringLines lines() const noexcept;

    // Validates the complete view before exposing any code point. Source bytes must remain unchanged.
    [[nodiscard]] std::expected<CodePointRange, StringError> code_points() const noexcept;

    [[nodiscard]] constexpr std::string_view std_view() const noexcept { return {data_, size_}; }

    [[nodiscard]] constexpr bool is_valid_utf8() const noexcept {
        for (std::size_t i = 0; i < size_;) {
            if !consteval {
                while (size_ - i >= 32) {
                    const auto c0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i));
                    const auto c1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i + 16));
                    if (_mm_movemask_epi8(_mm_or_si128(c0, c1)) != 0) {
                        const auto m0 = static_cast<unsigned>(_mm_movemask_epi8(c0));
                        if (m0 != 0) {
                            i += static_cast<std::size_t>(std::countr_zero(m0));
                        } else {
                            const auto m1 = static_cast<unsigned>(_mm_movemask_epi8(c1));
                            [[assume(m1 != 0)]];
                            i += 16 + static_cast<std::size_t>(std::countr_zero(m1));
                        }
                        goto decode_non_ascii;
                    }
                    i += 32;
                }
                if (size_ - i >= 16) {
                    const auto chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i));
                    const auto mask = static_cast<unsigned>(_mm_movemask_epi8(chunk));
                    if (mask != 0) {
                        i += static_cast<std::size_t>(std::countr_zero(mask));
                        goto decode_non_ascii;
                    }
                    i += 16;
                }
                if (i == size_)
                    return true;
            }
        decode_non_ascii:
            const auto first = static_cast<unsigned char>(data_[i++]);
            if (first < 0x80)
                continue;
            if (first <= 0xdf) {
                if (first < 0xc2 || i == size_ || (static_cast<unsigned char>(data_[i++]) ^ 0x80u) >= 0x40u)
                    return false;
            } else if (first <= 0xef) {
                if (size_ - i < 2)
                    return false;
                const auto b1 = static_cast<unsigned char>(data_[i]);
                const auto b2 = static_cast<unsigned char>(data_[i + 1]);
                if (((b1 ^ 0x80u) | (b2 ^ 0x80u)) >= 0x40u || (first == 0xe0 && b1 < 0xa0) ||
                    (first == 0xed && b1 >= 0xa0))
                    return false;
                i += 2;
            } else if (first <= 0xf4) {
                if (size_ - i < 3)
                    return false;
                const auto b1 = static_cast<unsigned char>(data_[i]);
                const auto b2 = static_cast<unsigned char>(data_[i + 1]);
                const auto b3 = static_cast<unsigned char>(data_[i + 2]);
                if (((b1 ^ 0x80u) | (b2 ^ 0x80u) | (b3 ^ 0x80u)) >= 0x40u || (first == 0xf0 && b1 < 0x90) ||
                    (first == 0xf4 && b1 >= 0x90))
                    return false;
                i += 3;
            } else
                return false;
        }
        return true;
    }

    friend constexpr bool operator==(StringView left, StringView right) noexcept {
        return left.std_view() == right.std_view();
    }

    friend constexpr auto operator<=>(StringView left, StringView right) noexcept {
        return left.std_view() <=> right.std_view();
    }

private:
    friend class String;

    struct Unchecked {};

    constexpr StringView(Unchecked, const char* data, std::size_t size) noexcept : data_(data), size_(size) {}

    const char* data_{""};

    std::size_t size_{};
};

constexpr ByteSearchSet::ByteSearchSet(StringView bytes) noexcept {
    unsigned unique = 0;
    for (const char raw : bytes) {
        const auto byte = static_cast<unsigned char>(raw);
        const auto mask = std::uint64_t{1} << (byte & 63u);
        auto& word = bits_[byte >> 6];
        if ((word & mask) == 0) {
            word |= mask;
            if (unique < 4)
                small_[unique] = raw;
            if (unique < 5)
                ++unique;
        }
    }
    if (unique <= 4) {
        mode_ = static_cast<Mode>(unique);
        return;
    }
    unsigned r_lo[2]{};
    unsigned r_hi[2]{};
    unsigned runs = 0;
    unsigned b = 0;
    while (b < 256) {
        if (!contains(static_cast<char>(static_cast<unsigned char>(b)))) {
            ++b;
            continue;
        }
        if (runs == 2) {
            runs = 3;
            break;
        }
        r_lo[runs] = b;
        while (b + 1 < 256 && contains(static_cast<char>(static_cast<unsigned char>(b + 1))))
            ++b;
        r_hi[runs] = b;
        ++runs;
        ++b;
    }
    unsigned lo = 0;
    unsigned hi = 0;
    unsigned extra = 0;
    bool is_range_extra = false;
    if (runs == 1) {
        lo = r_lo[0];
        hi = r_hi[0];
        extra = lo;
        is_range_extra = (hi - lo) >= 1 && (hi - lo) <= 253;
    } else if (runs == 2) {
        if (r_lo[0] == r_hi[0]) {
            extra = r_lo[0];
            lo = r_lo[1];
            hi = r_hi[1];
            is_range_extra = (hi - lo) >= 1 && (hi - lo) <= 253;
        } else if (r_lo[1] == r_hi[1]) {
            lo = r_lo[0];
            hi = r_hi[0];
            extra = r_lo[1];
            is_range_extra = (hi - lo) >= 1 && (hi - lo) <= 253;
        }
    }
    if (is_range_extra) {
        const auto width = hi - lo;
        small_[0] = static_cast<char>(static_cast<unsigned char>(extra));
        small_[1] = static_cast<char>(static_cast<unsigned char>(lo + 0x80u));
        small_[2] = static_cast<char>(-128 + static_cast<int>(width) + 1);
        mode_ = Mode::range_extra;
    } else {
        mode_ = Mode::bitmap;
    }
}

template <class P>
    requires(std::same_as<std::decay_t<P>, const char*> || std::same_as<std::decay_t<P>, char*>)
constexpr ByteSearchSet::ByteSearchSet(P&& bytes) noexcept : ByteSearchSet(StringView{bytes}) {}

inline constexpr ByteSearchSet ascii_whitespace_set{" \t\r\n\v\f"};

inline constexpr ByteSearchSet line_break_set{"\r\n"};

constexpr StringView StringView::trim_start() const noexcept {
    return trim_start(ascii_whitespace_set);
}

constexpr StringView StringView::trim_end() const noexcept {
    return trim_end(ascii_whitespace_set);
}

constexpr StringView StringView::trim() const noexcept {
    return trim(ascii_whitespace_set);
}

struct StringPartition {
    StringView before;

    StringView separator;

    StringView after;
};

constexpr StringPartition StringView::partition(StringView delimiter) const noexcept {
    const auto at = delimiter.empty() ? npos : find(delimiter);
    return at == npos ? StringPartition{*this, {}, {}}
                      : StringPartition{{Unchecked{}, data_, at},
                                        {Unchecked{}, data_ + at, delimiter.size_},
                                        {Unchecked{}, data_ + at + delimiter.size_, size_ - at - delimiter.size_}};
}

constexpr StringPartition StringView::partition(char delimiter) const noexcept {
    const auto at = find(delimiter);
    return at == npos ? StringPartition{*this, {}, {}}
                      : StringPartition{{Unchecked{}, data_, at},
                                        {Unchecked{}, data_ + at, 1},
                                        {Unchecked{}, data_ + at + 1, size_ - at - 1}};
}

constexpr StringPartition StringView::rpartition(StringView delimiter) const noexcept {
    const auto at = delimiter.empty() ? npos : rfind(delimiter);
    return at == npos ? StringPartition{{}, {}, *this}
                      : StringPartition{{Unchecked{}, data_, at},
                                        {Unchecked{}, data_ + at, delimiter.size_},
                                        {Unchecked{}, data_ + at + delimiter.size_, size_ - at - delimiter.size_}};
}

constexpr StringPartition StringView::rpartition(char delimiter) const noexcept {
    const auto at = rfind(delimiter);
    return at == npos ? StringPartition{{}, {}, *this}
                      : StringPartition{{Unchecked{}, data_, at},
                                        {Unchecked{}, data_ + at, 1},
                                        {Unchecked{}, data_ + at + 1, size_ - at - 1}};
}

class StringSplit {
public:
    class Iterator {
    public:
        using value_type = StringView;

        using difference_type = std::ptrdiff_t;

        using iterator_concept = std::forward_iterator_tag;

        using iterator_category = std::input_iterator_tag;

        constexpr Iterator() noexcept = default;

        [[nodiscard]] constexpr StringView operator*() const noexcept { return remaining_.slice(0, next_); }

        constexpr Iterator& operator++() noexcept {
            if (next_ == StringView::npos) {
                remaining_ = {};
                delimiter_ = {};
                ended_ = true;
            } else {
                remaining_ = remaining_.slice(next_ + delimiter_.size());
                locate();
            }
            return *this;
        }

        constexpr Iterator operator++(int) noexcept {
            const auto copy = *this;
            ++*this;
            return copy;
        }

        friend constexpr bool operator==(const Iterator& left, const Iterator& right) noexcept {
            if (left.ended_ || right.ended_)
                return left.ended_ == right.ended_;
            return left.remaining_.data() == right.remaining_.data() &&
                   left.remaining_.size() == right.remaining_.size() && left.next_ == right.next_;
        }

        friend constexpr bool operator==(const Iterator& value, std::default_sentinel_t) noexcept {
            return value.ended_;
        }

    private:
        friend class StringSplit;

        constexpr Iterator(StringView text, StringView delimiter) noexcept
            : remaining_(text), delimiter_(delimiter), ended_(false) {
            locate();
        }

        constexpr void locate() noexcept {
            next_ = delimiter_.empty() ? StringView::npos : remaining_.find(delimiter_);
        }

        StringView remaining_;

        StringView delimiter_;

        std::size_t next_{StringView::npos};

        bool ended_{true};
    };

    [[nodiscard]] constexpr Iterator begin() const noexcept { return Iterator{text_, delimiter_}; }

    [[nodiscard]] constexpr Iterator end() const noexcept { return {}; }

private:
    friend class StringView;

    constexpr StringSplit(StringView text, StringView delimiter) noexcept : text_(text), delimiter_(delimiter) {}

    StringView text_;

    StringView delimiter_;
};

constexpr StringSplit StringView::split(StringView delimiter) const noexcept {
    return {*this, delimiter};
}

class StringSplitAny {
public:
    class Iterator {
    public:
        using value_type = StringView;

        using difference_type = std::ptrdiff_t;

        using iterator_concept = std::forward_iterator_tag;

        using iterator_category = std::input_iterator_tag;

        constexpr Iterator() noexcept = default;

        [[nodiscard]] constexpr StringView operator*() const noexcept { return remaining_.slice(0, next_); }

        constexpr Iterator& operator++() noexcept {
            if (next_ == StringView::npos) {
                remaining_ = {};
                ended_ = true;
            } else {
                remaining_ = remaining_.slice(next_ + 1);
                advance();
            }
            return *this;
        }

        constexpr Iterator operator++(int) noexcept {
            const auto copy = *this;
            ++*this;
            return copy;
        }

        friend constexpr bool operator==(const Iterator& left, const Iterator& right) noexcept {
            if (left.ended_ || right.ended_)
                return left.ended_ == right.ended_;
            return left.remaining_.data() == right.remaining_.data() &&
                   left.remaining_.size() == right.remaining_.size() && left.next_ == right.next_;
        }

        friend constexpr bool operator==(const Iterator& value, std::default_sentinel_t) noexcept {
            return value.ended_;
        }

    private:
        friend class StringSplitAny;

        constexpr Iterator(StringView text, const ByteSearchSet& delimiters, bool skip_empty) noexcept
            : remaining_(text), delimiters_(delimiters), skip_empty_(skip_empty), ended_(false) {
            advance();
        }

        constexpr void advance() noexcept {
            if (skip_empty_) {
                const auto first = remaining_.find_first_not_of(delimiters_);
                if (first == StringView::npos) {
                    remaining_ = {};
                    next_ = StringView::npos;
                    ended_ = true;
                    return;
                }
                remaining_.remove_prefix(first);
            }
            next_ = remaining_.find_first_of(delimiters_);
        }

        StringView remaining_;

        ByteSearchSet delimiters_;

        std::size_t next_{StringView::npos};

        bool skip_empty_{};

        bool ended_{true};
    };

    [[nodiscard]] constexpr Iterator begin() const noexcept { return Iterator{text_, delimiters_, skip_empty_}; }

    [[nodiscard]] constexpr Iterator end() const noexcept { return {}; }

private:
    friend class StringView;

    constexpr StringSplitAny(StringView text, const ByteSearchSet& delimiters, bool skip_empty) noexcept
        : text_(text), delimiters_(delimiters), skip_empty_(skip_empty) {}

    StringView text_;

    ByteSearchSet delimiters_;

    bool skip_empty_{};
};

constexpr StringSplitAny StringView::split_any(const ByteSearchSet& delimiters, bool skip_empty) const noexcept {
    return {*this, delimiters, skip_empty};
}

constexpr StringSplitAny StringView::split_ascii_whitespace() const noexcept {
    return {*this, ascii_whitespace_set, true};
}

class StringLines {
public:
    class Iterator {
    public:
        using value_type = StringView;

        using difference_type = std::ptrdiff_t;

        using iterator_concept = std::forward_iterator_tag;

        using iterator_category = std::input_iterator_tag;

        constexpr Iterator() noexcept = default;

        [[nodiscard]] constexpr StringView operator*() const noexcept { return remaining_.slice(0, line_length_); }

        constexpr Iterator& operator++() noexcept {
            if (step_ >= remaining_.size()) {
                remaining_ = {};
                line_length_ = 0;
                step_ = 0;
                ended_ = true;
            } else {
                remaining_.remove_prefix(step_);
                locate();
            }
            return *this;
        }

        constexpr Iterator operator++(int) noexcept {
            const auto copy = *this;
            ++*this;
            return copy;
        }

        friend constexpr bool operator==(const Iterator& left, const Iterator& right) noexcept {
            if (left.ended_ || right.ended_)
                return left.ended_ == right.ended_;
            return left.remaining_.data() == right.remaining_.data() &&
                   left.remaining_.size() == right.remaining_.size() && left.line_length_ == right.line_length_;
        }

        friend constexpr bool operator==(const Iterator& value, std::default_sentinel_t) noexcept {
            return value.ended_;
        }

    private:
        friend class StringLines;

        explicit constexpr Iterator(StringView text) noexcept : remaining_(text), ended_(text.empty()) {
            if (!ended_)
                locate();
        }

        constexpr void locate() noexcept {
            const auto at = remaining_.find_first_of(line_break_set);
            if (at == StringView::npos) {
                line_length_ = remaining_.size();
                step_ = remaining_.size();
            } else {
                line_length_ = at;
                step_ = (remaining_[at] == '\r' && at + 1 < remaining_.size() && remaining_[at + 1] == '\n') ? at + 2
                                                                                                             : at + 1;
            }
        }

        StringView remaining_;

        std::size_t line_length_{};

        std::size_t step_{};

        bool ended_{true};
    };

    [[nodiscard]] constexpr Iterator begin() const noexcept { return Iterator{text_}; }

    [[nodiscard]] constexpr Iterator end() const noexcept { return {}; }

private:
    friend class StringView;

    explicit constexpr StringLines(StringView text) noexcept : text_(text) {}

    StringView text_;
};

constexpr StringLines StringView::lines() const noexcept {
    return StringLines{*this};
}

struct CodePoint {
    char32_t value;

    std::size_t byte_offset;

    std::size_t byte_count;
};

class CodePointRange {
public:
    class Iterator {
    public:
        using value_type = CodePoint;

        using difference_type = std::ptrdiff_t;

        using iterator_concept = std::forward_iterator_tag;

        using iterator_category = std::input_iterator_tag;

        constexpr Iterator() noexcept = default;

        [[nodiscard]] constexpr CodePoint operator*() const noexcept {
            const auto first = static_cast<unsigned char>(text_[offset_]);
            const std::size_t count = first < 0x80 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
            char32_t value = first & (count == 1 ? 0x7f : count == 2 ? 0x1f : count == 3 ? 0x0f : 0x07);
            const char* bytes = text_.data() + offset_;
            for (std::size_t i = 1; i < count; ++i)
                value = (value << 6) | (static_cast<unsigned char>(bytes[i]) & 0x3f);
            return {value, offset_, count};
        }

        constexpr Iterator& operator++() noexcept {
            const auto first = static_cast<unsigned char>(text_[offset_]);
            offset_ += first < 0x80 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
            return *this;
        }

        constexpr Iterator operator++(int) noexcept {
            const auto copy = *this;
            ++*this;
            return copy;
        }

        friend constexpr bool operator==(const Iterator& left, const Iterator& right) noexcept {
            const bool left_end = left.offset_ == left.text_.size();
            const bool right_end = right.offset_ == right.text_.size();
            if (left_end || right_end)
                return left_end == right_end;
            return left.text_.data() + left.offset_ == right.text_.data() + right.offset_ &&
                   left.text_.end() == right.text_.end();
        }

        friend constexpr bool operator==(const Iterator& value, std::default_sentinel_t) noexcept {
            return value.offset_ == value.text_.size();
        }

    private:
        friend class CodePointRange;

        explicit constexpr Iterator(StringView text) noexcept : text_(text) {}

        StringView text_;

        std::size_t offset_{};
    };

    [[nodiscard]] constexpr Iterator begin() const noexcept { return Iterator{text_}; }

    [[nodiscard]] constexpr Iterator end() const noexcept { return {}; }

private:
    friend class StringView;

    explicit constexpr CodePointRange(StringView text) noexcept : text_(text) {}

    StringView text_;
};

inline std::expected<CodePointRange, StringError> StringView::code_points() const noexcept {
    if (!is_valid_utf8())
        return std::unexpected(StringError::invalid_utf8);
    return CodePointRange{*this};
}

namespace detail {
template <std::size_t N>
struct StringLiteral {
    char bytes[N];

    consteval StringLiteral(const char (&text)[N]) {
        for (std::size_t i = 0; i != N; ++i)
            bytes[i] = text[i];
        if (bytes[N - 1] != '\0' || !StringView{bytes, N - 1}.is_valid_utf8())
            fatal("String literal requires valid UTF-8 and a trailing zero.");
    }
};

template <class R>
concept StringRange = requires(const R& range) {
    std::begin(range);
    std::end(range);
    requires std::same_as<std::remove_cvref_t<decltype(*std::begin(range))>, String>;
};

template <class R>
concept StringViewRange = requires(const R& range) {
    std::begin(range);
    std::end(range);
    requires std::convertible_to<decltype(*std::begin(range)), StringView>;
};

template <class R, class Element>
concept ContiguousRangeOf = requires(const R& range) {
    { std::data(range) } -> std::convertible_to<const Element*>;
    { std::size(range) } -> std::convertible_to<std::size_t>;
    requires std::same_as<std::remove_cvref_t<decltype(*std::data(range))>, Element>;
};

template <class T>
concept NonBoolUnsigned = std::unsigned_integral<T> && !std::same_as<std::remove_cv_t<T>, bool>;

template <class T>
concept ConcatInteger =
    TextInteger<std::remove_cvref_t<T>> && !std::same_as<std::remove_cvref_t<T>, char> &&
    !std::same_as<std::remove_cvref_t<T>, wchar_t> && !std::same_as<std::remove_cvref_t<T>, char8_t> &&
    !std::same_as<std::remove_cvref_t<T>, char16_t> && !std::same_as<std::remove_cvref_t<T>, char32_t>;
}

// Validated UTF-8, owned or borrowed from fixed literal storage, with a trailing zero. Embedded zeros remain part of
// size(). Length and all view positions are bytes, not glyphs or code points.
class String {
public:
    static constexpr std::size_t npos = StringView::npos;

    static constexpr std::size_t inline_capacity = 30;

    static constexpr std::size_t max_size = static_cast<std::size_t>(PTRDIFF_MAX) - 1;

    struct Validated {
        StringView text;
    };

    class ConcatPiece {
    public:
        ConcatPiece(const String& text [[clang::lifetimebound]]) noexcept;

        constexpr ConcatPiece(Validated validated) noexcept
            : dummy_{}, data_(validated.text.data()), size_(validated.text.size()) {}

        template <std::size_t N>
        consteval ConcatPiece(const char (&text)[N]) noexcept : dummy_{}, data_(text), size_(N - 1) {
            if (text[N - 1] != '\0' || !StringView{text, N - 1}.is_valid_utf8())
                fatal("String::concat literal requires valid UTF-8 and a trailing zero.");
        }

        template <std::size_t N>
        ConcatPiece(char (&)[N]) = delete;

        template <std::same_as<bool> B>
        constexpr ConcatPiece(B value) noexcept : dummy_{}, data_(value ? "true" : "false"), size_(value ? 4 : 5) {}

        template <detail::ConcatInteger T>
        ConcatPiece(T value) noexcept : buffer_{}, data_(nullptr) {
            const auto result = std::to_chars(buffer_, buffer_ + sizeof(buffer_), value, 10);
            size_ = static_cast<std::size_t>(result.ptr - buffer_);
        }

        template <std::floating_point T>
        ConcatPiece(T value) noexcept : buffer_{}, data_(nullptr) {
            const auto result = std::to_chars(buffer_, buffer_ + sizeof(buffer_), value, std::chars_format::general);
            size_ = static_cast<std::size_t>(result.ptr - buffer_);
        }

        [[nodiscard]] constexpr StringView view() const& noexcept [[clang::lifetimebound]] {
            return {StringView::Unchecked{}, data_ ? data_ : buffer_, size_};
        }

        StringView view() const&& = delete;

    private:
        union {
            char dummy_{};

            char buffer_[40];
        };

        const char* data_{""};

        std::size_t size_{};
    };

    String() noexcept : bytes_{} {}

    [[nodiscard]] static constexpr Validated validated(StringView text) noexcept { return {text}; }

    [[nodiscard]] static std::expected<String, StringError> from_utf8(StringView text) noexcept;

    // Caller guarantees complete, valid UTF-8. Copies without scanning in either
    // configuration; invalid encoding violates the contract and is not detected.
    [[nodiscard]] static String from_utf8_validated(StringView text) noexcept;

    template <detail::TextInteger T>
    [[nodiscard]] static String from_number(T value) noexcept {
        char bytes[std::numeric_limits<T>::digits + 3];
        const auto result = std::to_chars(bytes, bytes + sizeof(bytes), value, 10);
        return from_utf8_validated({StringView::Unchecked{}, bytes, static_cast<std::size_t>(result.ptr - bytes)});
    }

    template <detail::TextInteger T>
    [[nodiscard]] static std::expected<String, std::errc> from_number(T value, int base) noexcept {
        if (base < 2 || base > 36)
            return std::unexpected(std::errc::invalid_argument);
        char bytes[std::numeric_limits<T>::digits + 3];
        const auto result = std::to_chars(bytes, bytes + sizeof(bytes), value, base);
        return from_utf8_validated({StringView::Unchecked{}, bytes, static_cast<std::size_t>(result.ptr - bytes)});
    }

    // Shortest general representation that round-trips through from_chars.
    template <std::floating_point T>
    [[nodiscard]] static String from_number(T value) noexcept {
        char bytes[std::numeric_limits<T>::max_digits10 + 16];
        const auto result = std::to_chars(bytes, bytes + sizeof(bytes), value, std::chars_format::general);
        return from_utf8_validated({StringView::Unchecked{}, bytes, static_cast<std::size_t>(result.ptr - bytes)});
    }

    template <detail::NonBoolUnsigned T>
    [[nodiscard]] static String from_hex(T value, std::size_t min_digits = 0, bool uppercase = false) noexcept {
        return format_hex_value(static_cast<std::uint64_t>(value), min_digits, uppercase);
    }

    [[nodiscard]] static String from_float_fixed(double value, int precision) noexcept;

    [[nodiscard]] static String from_float_general(double value, int precision) noexcept;

    [[nodiscard]] static String repeat(const String& unit, std::size_t count) noexcept;

    [[nodiscard]] static std::expected<String, StringError> repeat(StringView unit, std::size_t count) noexcept;

    [[nodiscard]] static String concat() noexcept { return {}; }

    [[nodiscard]] static String concat(const String& text) noexcept { return text; }

    [[nodiscard]] static String concat(String&& text) noexcept { return std::move(text); }

    [[nodiscard]] static String concat(const ConcatPiece& p0) noexcept {
        const StringView views[]{p0.view()};
        return concat_views({views, 1});
    }

    [[nodiscard]] static String concat(const ConcatPiece& p0, const ConcatPiece& p1) noexcept {
        const StringView views[]{p0.view(), p1.view()};
        return concat_views({views, 2});
    }

    [[nodiscard]] static String concat(const ConcatPiece& p0, const ConcatPiece& p1, const ConcatPiece& p2) noexcept {
        const StringView views[]{p0.view(), p1.view(), p2.view()};
        return concat_views({views, 3});
    }

    [[nodiscard]] static String concat(const ConcatPiece& p0, const ConcatPiece& p1, const ConcatPiece& p2,
                                       const ConcatPiece& p3) noexcept {
        const StringView views[]{p0.view(), p1.view(), p2.view(), p3.view()};
        return concat_views({views, 4});
    }

    [[nodiscard]] static String concat(const ConcatPiece& p0, const ConcatPiece& p1, const ConcatPiece& p2,
                                       const ConcatPiece& p3, const ConcatPiece& p4) noexcept {
        const StringView views[]{p0.view(), p1.view(), p2.view(), p3.view(), p4.view()};
        return concat_views({views, 5});
    }

    [[nodiscard]] static String concat(const ConcatPiece& p0, const ConcatPiece& p1, const ConcatPiece& p2,
                                       const ConcatPiece& p3, const ConcatPiece& p4, const ConcatPiece& p5) noexcept {
        const StringView views[]{p0.view(), p1.view(), p2.view(), p3.view(), p4.view(), p5.view()};
        return concat_views({views, 6});
    }

    [[nodiscard]] static String concat(const ConcatPiece& p0, const ConcatPiece& p1, const ConcatPiece& p2,
                                       const ConcatPiece& p3, const ConcatPiece& p4, const ConcatPiece& p5,
                                       const ConcatPiece& p6) noexcept {
        const StringView views[]{p0.view(), p1.view(), p2.view(), p3.view(), p4.view(), p5.view(), p6.view()};
        return concat_views({views, 7});
    }

    [[nodiscard]] static String concat(const ConcatPiece& p0, const ConcatPiece& p1, const ConcatPiece& p2,
                                       const ConcatPiece& p3, const ConcatPiece& p4, const ConcatPiece& p5,
                                       const ConcatPiece& p6, const ConcatPiece& p7) noexcept {
        const StringView views[]{p0.view(), p1.view(), p2.view(), p3.view(),
                                 p4.view(), p5.view(), p6.view(), p7.view()};
        return concat_views({views, 8});
    }

    [[nodiscard]] static String concat(const ConcatPiece& p0, const ConcatPiece& p1, const ConcatPiece& p2,
                                       const ConcatPiece& p3, const ConcatPiece& p4, const ConcatPiece& p5,
                                       const ConcatPiece& p6, const ConcatPiece& p7, const ConcatPiece& p8) noexcept {
        const StringView views[]{p0.view(), p1.view(), p2.view(), p3.view(), p4.view(),
                                 p5.view(), p6.view(), p7.view(), p8.view()};
        return concat_views({views, 9});
    }

    [[nodiscard]] static String concat(const ConcatPiece& p0, const ConcatPiece& p1, const ConcatPiece& p2,
                                       const ConcatPiece& p3, const ConcatPiece& p4, const ConcatPiece& p5,
                                       const ConcatPiece& p6, const ConcatPiece& p7, const ConcatPiece& p8,
                                       const ConcatPiece& p9) noexcept {
        const StringView views[]{p0.view(), p1.view(), p2.view(), p3.view(), p4.view(),
                                 p5.view(), p6.view(), p7.view(), p8.view(), p9.view()};
        return concat_views({views, 10});
    }

    [[nodiscard]] static String concat(const ConcatPiece& p0, const ConcatPiece& p1, const ConcatPiece& p2,
                                       const ConcatPiece& p3, const ConcatPiece& p4, const ConcatPiece& p5,
                                       const ConcatPiece& p6, const ConcatPiece& p7, const ConcatPiece& p8,
                                       const ConcatPiece& p9, const ConcatPiece& p10) noexcept {
        const StringView views[]{p0.view(), p1.view(), p2.view(), p3.view(), p4.view(), p5.view(),
                                 p6.view(), p7.view(), p8.view(), p9.view(), p10.view()};
        return concat_views({views, 11});
    }

    [[nodiscard]] static String concat(const ConcatPiece& p0, const ConcatPiece& p1, const ConcatPiece& p2,
                                       const ConcatPiece& p3, const ConcatPiece& p4, const ConcatPiece& p5,
                                       const ConcatPiece& p6, const ConcatPiece& p7, const ConcatPiece& p8,
                                       const ConcatPiece& p9, const ConcatPiece& p10, const ConcatPiece& p11) noexcept {
        const StringView views[]{p0.view(), p1.view(), p2.view(), p3.view(), p4.view(),  p5.view(),
                                 p6.view(), p7.view(), p8.view(), p9.view(), p10.view(), p11.view()};
        return concat_views({views, 12});
    }

    // The template parameter object has static storage duration. Its module
    // must remain loaded until every copy dies; JIT text must use from_utf8.
    template <detail::StringLiteral Text>
    [[nodiscard]] static String literal() noexcept {
        String result{Uninitialized{}};
        constexpr auto length = sizeof(Text.bytes) - 1;
        static_assert(length <= max_size && Text.bytes[length] == '\0' &&
                      StringView{Text.bytes, length}.is_valid_utf8());
        result.set_long({Text.bytes, length, length}, literal_tag);
        return result;
    }

    // Separator is validated even when fewer than two parts are joined.
    [[nodiscard]] static std::expected<String, StringError> join(std::initializer_list<StringView> parts,
                                                                 StringView separator) noexcept {
        return join_views({parts.begin(), parts.size()}, separator);
    }

    template <detail::StringRange R>
    [[nodiscard]] static String join(const R& parts, const String& separator) noexcept {
        if constexpr (detail::ContiguousRangeOf<R, String>)
            return join_strings({std::data(parts), std::size(parts)}, separator);
        else {
            String result;
            bool first = true;
            for (const String& part : parts) {
                if (!first)
                    result.append(separator);
                first = false;
                result.append(part);
            }
            return result;
        }
    }

    template <detail::StringViewRange R>
    [[nodiscard]] static std::expected<String, StringError> join(const R& parts, StringView separator) noexcept {
        if constexpr (detail::ContiguousRangeOf<R, String>)
            return join_strings_with_view({std::data(parts), std::size(parts)}, separator);
        else if constexpr (detail::ContiguousRangeOf<R, StringView>)
            return join_views({std::data(parts), std::size(parts)}, separator);
        else {
            if (!separator.is_valid_utf8())
                return std::unexpected(StringError::invalid_utf8);
            String result;
            bool first = true;
            for (decltype(auto) part : parts) {
                if (!first)
                    result.write_validated(separator, result.size());
                first = false;
                if (const auto status = result.append(StringView{part}); !status)
                    return std::unexpected(status.error());
            }
            return result;
        }
    }

    String(const String& other) noexcept {
        if (other.tag() != heap_tag)
            std::memcpy(bytes_, other.bytes_, sizeof(bytes_));
        else
            copy_heap(other);
    }

    String& operator=(const String& other) noexcept {
        if (this != &other) {
            if (tag() != heap_tag && other.tag() != heap_tag)
                std::memcpy(bytes_, other.bytes_, sizeof(bytes_));
            else
                assign_from(other);
        }
        return *this;
    }

    String(String&& other) noexcept { move_from(other); }

    String& operator=(String&& other) noexcept {
        if (this != &other) {
            if (tag() == heap_tag)
                free_heap();
            move_from(other);
        }
        return *this;
    }

    ~String() {
        if (tag() == heap_tag)
            free_heap();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        if (is_inline())
            return tag();
        std::size_t length;
        std::memcpy(&length, bytes_ + sizeof(const char*), sizeof(length));
        return length;
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return is_inline() ? inline_capacity : long_value().capacity;
    }

    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    [[nodiscard]] const char* c_str() const noexcept [[clang::lifetimebound]] {
        if (is_inline())
            return bytes_;
        const char* pointer;
        std::memcpy(&pointer, bytes_, sizeof(pointer));
        return pointer;
    }

    [[nodiscard]] StringView view() const& noexcept [[clang::lifetimebound]] {
        if (is_inline())
            return {StringView::Unchecked{}, bytes_, tag()};
        const auto value = long_value();
        return {StringView::Unchecked{}, value.data, value.size};
    }

    StringView view() const&& = delete;

    [[nodiscard]] StringView slice(std::size_t first, std::size_t count = StringView::npos) const& noexcept
        [[clang::lifetimebound]] {
        return view().slice(first, count);
    }

    StringView slice(std::size_t first, std::size_t count = StringView::npos) const&& = delete;

    [[nodiscard]] char operator[](std::size_t index) const noexcept { return view()[index]; }

    [[nodiscard]] char front() const noexcept { return view().front(); }

    [[nodiscard]] char back() const noexcept { return view().back(); }

    void reserve(std::size_t bytes) noexcept;

    void squeeze() noexcept;

    void clear() noexcept;

    void release() noexcept {
        if (tag() == heap_tag)
            free_heap();
        bytes_[0] = '\0';
        bytes_[31] = 0;
    }

    void swap(String& other) noexcept {
        const auto a0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(bytes_));
        const auto a1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(bytes_ + 16));
        const auto b0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(other.bytes_));
        const auto b1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(other.bytes_ + 16));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(bytes_), b0);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(bytes_ + 16), b1);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(other.bytes_), a0);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(other.bytes_ + 16), a1);
    }

    friend void swap(String& left, String& right) noexcept { left.swap(right); }

    [[nodiscard]] bool is_code_point_boundary(std::size_t byte_offset) const noexcept {
        const auto text = view();
        if (byte_offset > text.size())
            return false;
        return byte_offset == text.size() || (static_cast<unsigned char>(text.data()[byte_offset]) & 0xc0u) != 0x80u;
    }

    [[nodiscard]] std::expected<void, StringError> assign(StringView text) noexcept;

    void assign(const String& text) noexcept { *this = text; }

    void assign_validated(StringView text) noexcept {
        if (text.size() > max_size)
            fatal("String exceeds addressable storage.");
        write_validated(text, 0);
    }

    [[nodiscard]] std::expected<void, StringError> append(StringView text) noexcept;

    [[nodiscard]] std::expected<void, StringError> append(char byte) noexcept {
        if ((static_cast<unsigned char>(byte) & 0x80u) != 0)
            return std::unexpected(StringError::invalid_utf8);
        write_validated({StringView::Unchecked{}, &byte, 1}, size());
        return {};
    }

    // A complete owning String already satisfies the UTF-8 invariant. Byte
    // views, including slices of this String, still use the checked overload.
    void append(const String& text) noexcept { write_validated(text.view(), size()); }

    String& operator+=(const String& text) noexcept {
        append(text);
        return *this;
    }

    void append_validated(StringView text) noexcept { write_validated(text, size()); }

    template <detail::StringLiteral Text>
    void append_literal() noexcept {
        constexpr auto length = sizeof(Text.bytes) - 1;
        static_assert(length <= max_size && Text.bytes[length] == '\0' &&
                      StringView{Text.bytes, length}.is_valid_utf8());
        write_validated({StringView::Unchecked{}, Text.bytes, length}, size());
    }

    void append_ascii(char ascii, std::size_t count = 1) noexcept;

    template <detail::TextInteger T>
    void append_number(T value) noexcept {
        char bytes[std::numeric_limits<T>::digits + 3];
        const auto result = std::to_chars(bytes, bytes + sizeof(bytes), value, 10);
        write_validated({StringView::Unchecked{}, bytes, static_cast<std::size_t>(result.ptr - bytes)}, size());
    }

    template <std::floating_point T>
    void append_number(T value) noexcept {
        char bytes[std::numeric_limits<T>::max_digits10 + 16];
        const auto result = std::to_chars(bytes, bytes + sizeof(bytes), value, std::chars_format::general);
        write_validated({StringView::Unchecked{}, bytes, static_cast<std::size_t>(result.ptr - bytes)}, size());
    }

    template <detail::NonBoolUnsigned T>
    void append_hex(T value, std::size_t min_digits = 0, bool uppercase = false) noexcept {
        append_hex_value(static_cast<std::uint64_t>(value), min_digits, uppercase);
    }

    void append_float_fixed(double value, int precision) noexcept;

    void append_float_general(double value, int precision) noexcept;

    void trim_start_in_place() noexcept;

    void trim_end_in_place() noexcept;

    void trim_in_place() noexcept;

    bool strip_prefix(StringView prefix) noexcept;

    bool strip_prefix(char prefix) noexcept;

    bool strip_suffix(StringView suffix) noexcept;

    bool strip_suffix(char suffix) noexcept;

    [[nodiscard]] std::expected<void, StringError> slice_in_place(std::size_t first,
                                                                  std::size_t count = StringView::npos) noexcept;

    [[nodiscard]] std::expected<void, StringError> erase(std::size_t first,
                                                         std::size_t count = StringView::npos) noexcept;

    [[nodiscard]] std::expected<void, StringError> insert(std::size_t position, StringView text) noexcept;

    [[nodiscard]] std::expected<void, StringError> insert(std::size_t position, const String& text) noexcept;

    // Byte range must be in bounds. Splitting a code point or malformed replacement
    // returns invalid_utf8 without changing this value. Aliased inputs are supported.
    [[nodiscard]] std::expected<void, StringError> replace(std::size_t first, std::size_t count,
                                                           StringView replacement) noexcept;

    // Left-to-right, nonoverlapping matches. Empty search is a no-op, not insertion
    // between bytes. Both views must contain valid UTF-8, even when no match exists.
    [[nodiscard]] std::expected<std::size_t, StringError> replace_all(StringView search,
                                                                      StringView replacement) noexcept;

    // Normalizes '\r\n', '\n', and standalone '\r' to newline (default "\n").
    [[nodiscard]] std::expected<void, StringError> replace_line_endings(StringView newline = "\n") noexcept;

    void pad_left(std::size_t total_bytes, char fill_ascii = ' ') noexcept;

    void pad_right(std::size_t total_bytes, char fill_ascii = ' ') noexcept;

    void to_ascii_lower() noexcept;

    void to_ascii_upper() noexcept;

    [[nodiscard]] bool is_ascii() const noexcept { return view().is_ascii(); }

    [[nodiscard]] bool starts_with(StringView text) const noexcept { return view().starts_with(text); }

    [[nodiscard]] bool starts_with(const String& text) const noexcept { return view().starts_with(text.view()); }

    [[nodiscard]] bool starts_with(char value) const noexcept { return view().starts_with(value); }

    [[nodiscard]] bool ends_with(StringView text) const noexcept { return view().ends_with(text); }

    [[nodiscard]] bool ends_with(const String& text) const noexcept { return view().ends_with(text.view()); }

    [[nodiscard]] bool ends_with(char value) const noexcept { return view().ends_with(value); }

    [[nodiscard]] bool equals_ascii_case_insensitive(StringView text) const noexcept {
        return view().equals_ascii_case_insensitive(text);
    }

    [[nodiscard]] bool equals_ascii_case_insensitive(const String& text) const noexcept {
        return view().equals_ascii_case_insensitive(text.view());
    }

    [[nodiscard]] std::strong_ordering compare_ascii_case_insensitive(StringView text) const noexcept {
        return view().compare_ascii_case_insensitive(text);
    }

    [[nodiscard]] std::strong_ordering compare_ascii_case_insensitive(const String& text) const noexcept {
        return view().compare_ascii_case_insensitive(text.view());
    }

    [[nodiscard]] bool starts_with_ascii_case_insensitive(StringView text) const noexcept {
        return view().starts_with_ascii_case_insensitive(text);
    }

    [[nodiscard]] bool starts_with_ascii_case_insensitive(const String& text) const noexcept {
        return view().starts_with_ascii_case_insensitive(text.view());
    }

    [[nodiscard]] bool ends_with_ascii_case_insensitive(StringView text) const noexcept {
        return view().ends_with_ascii_case_insensitive(text);
    }

    [[nodiscard]] bool ends_with_ascii_case_insensitive(const String& text) const noexcept {
        return view().ends_with_ascii_case_insensitive(text.view());
    }

    [[nodiscard]] std::size_t find(StringView text, std::size_t from = 0) const noexcept {
        return view().find(text, from);
    }

    [[nodiscard]] std::size_t find(const String& text, std::size_t from = 0) const noexcept {
        return view().find(text.view(), from);
    }

    [[nodiscard]] std::size_t find(char value, std::size_t from = 0) const noexcept { return view().find(value, from); }

    [[nodiscard]] std::size_t rfind(StringView text, std::size_t from = StringView::npos) const noexcept {
        return view().rfind(text, from);
    }

    [[nodiscard]] std::size_t rfind(const String& text, std::size_t from = StringView::npos) const noexcept {
        return view().rfind(text.view(), from);
    }

    [[nodiscard]] std::size_t rfind(char value, std::size_t from = StringView::npos) const noexcept {
        return view().rfind(value, from);
    }

    [[nodiscard]] std::size_t find_ascii_case_insensitive(StringView text, std::size_t from = 0) const noexcept {
        return view().find_ascii_case_insensitive(text, from);
    }

    [[nodiscard]] std::size_t find_ascii_case_insensitive(const String& text, std::size_t from = 0) const noexcept {
        return view().find_ascii_case_insensitive(text.view(), from);
    }

    [[nodiscard]] std::size_t rfind_ascii_case_insensitive(StringView text,
                                                           std::size_t from = StringView::npos) const noexcept {
        return view().rfind_ascii_case_insensitive(text, from);
    }

    [[nodiscard]] std::size_t rfind_ascii_case_insensitive(const String& text,
                                                           std::size_t from = StringView::npos) const noexcept {
        return view().rfind_ascii_case_insensitive(text.view(), from);
    }

    [[nodiscard]] bool contains_ascii_case_insensitive(StringView text) const noexcept {
        return view().contains_ascii_case_insensitive(text);
    }

    [[nodiscard]] bool contains_ascii_case_insensitive(const String& text) const noexcept {
        return view().contains_ascii_case_insensitive(text.view());
    }

    [[nodiscard]] std::size_t find_first_of(const ByteSearchSet& set, std::size_t from = 0) const noexcept {
        return view().find_first_of(set, from);
    }

    [[nodiscard]] std::size_t find_last_of(const ByteSearchSet& set,
                                           std::size_t from = StringView::npos) const noexcept {
        return view().find_last_of(set, from);
    }

    [[nodiscard]] std::size_t find_first_not_of(const ByteSearchSet& set, std::size_t from = 0) const noexcept {
        return view().find_first_not_of(set, from);
    }

    template <std::same_as<char> C>
    [[nodiscard]] std::size_t find_first_not_of(C value, std::size_t from = 0) const noexcept {
        return view().find_first_not_of(value, from);
    }

    [[nodiscard]] std::size_t find_last_not_of(const ByteSearchSet& set,
                                               std::size_t from = StringView::npos) const noexcept {
        return view().find_last_not_of(set, from);
    }

    template <std::same_as<char> C>
    [[nodiscard]] std::size_t find_last_not_of(C value, std::size_t from = StringView::npos) const noexcept {
        return view().find_last_not_of(value, from);
    }

    [[nodiscard]] bool contains(StringView text) const noexcept { return view().contains(text); }

    [[nodiscard]] bool contains(const String& text) const noexcept { return view().contains(text.view()); }

    [[nodiscard]] bool contains(char value) const noexcept { return view().contains(value); }

    [[nodiscard]] bool contains_any(const ByteSearchSet& set) const noexcept { return view().contains_any(set); }

    [[nodiscard]] bool contains_only(const ByteSearchSet& set) const noexcept { return view().contains_only(set); }

    [[nodiscard]] std::size_t count(StringView text) const noexcept { return view().count(text); }

    [[nodiscard]] std::size_t count(const String& text) const noexcept { return view().count(text.view()); }

    [[nodiscard]] std::size_t count(char value) const noexcept { return view().count(value); }

    [[nodiscard]] std::size_t common_prefix_length(StringView other) const noexcept {
        return view().common_prefix_length(other);
    }

    [[nodiscard]] std::size_t common_prefix_length(const String& other) const noexcept {
        return view().common_prefix_length(other.view());
    }

    [[nodiscard]] std::size_t common_suffix_length(StringView other) const noexcept {
        return view().common_suffix_length(other);
    }

    [[nodiscard]] std::size_t common_suffix_length(const String& other) const noexcept {
        return view().common_suffix_length(other.view());
    }

    friend bool operator==(const String& left, const String& right) noexcept { return left.view() == right.view(); }

    friend bool operator==(const String& left, StringView right) noexcept { return left.view() == right; }

    friend auto operator<=>(const String& left, const String& right) noexcept { return left.view() <=> right.view(); }

    friend auto operator<=>(const String& left, StringView right) noexcept { return left.view() <=> right; }

private:
    friend class Utf8Text;

    [[nodiscard]] static String from_static_literal(const char* data, std::size_t size) noexcept {
        if (size > max_size)
            fatal("String exceeds addressable storage.");
        String result{Uninitialized{}};
        result.set_long({data, size, size}, literal_tag);
        return result;
    }

    [[nodiscard]] static String format_hex_value(std::uint64_t value, std::size_t min_digits, bool uppercase) noexcept;

    [[nodiscard]] static String concat_views(Span<const StringView> pieces) noexcept;

    [[nodiscard]] static char* allocate_heap(std::size_t capacity) noexcept;

    [[nodiscard]] static char* reallocate_heap(char* data, std::size_t capacity) noexcept;

    void append_hex_value(std::uint64_t value, std::size_t min_digits, bool uppercase) noexcept;

    void insert_validated(std::size_t position, StringView text) noexcept;

    [[nodiscard]] static std::expected<String, StringError> join_views(Span<const StringView> parts,
                                                                       StringView separator) noexcept;

    [[nodiscard]] static String join_strings(Span<const String> parts, const String& separator) noexcept;

    [[nodiscard]] static std::expected<String, StringError> join_strings_with_view(Span<const String> parts,
                                                                                   StringView separator) noexcept;

    template <class Part, class ViewOf>
    [[nodiscard]] static String build_joined(Span<const Part> parts, StringView separator, ViewOf view_of) noexcept;

    struct Uninitialized {};

    explicit String(Uninitialized) noexcept {}

    struct LongValue {
        const char* data;

        std::size_t size;

        std::size_t capacity;
    };

    static constexpr unsigned char heap_tag = 0xfe;

    static constexpr unsigned char literal_tag = 0xff;

    [[nodiscard]] unsigned char tag() const noexcept { return static_cast<unsigned char>(bytes_[31]); }

    [[nodiscard]] bool is_inline() const noexcept { return tag() <= inline_capacity; }

    [[nodiscard]] LongValue long_value() const noexcept {
        LongValue value;
        std::memcpy(&value, bytes_, sizeof(value));
        return value;
    }

    void set_long(LongValue value, unsigned char mode) noexcept {
        std::memcpy(bytes_, &value, sizeof(value));
        const std::uint64_t tail = static_cast<std::uint64_t>(mode) << 56;
        std::memcpy(bytes_ + sizeof(value), &tail, sizeof(tail));
    }

    void move_from(String& other) noexcept {
        std::memcpy(bytes_, other.bytes_, sizeof(bytes_));
        other.bytes_[0] = '\0';
        other.bytes_[31] = 0;
    }

    void free_heap() noexcept;

    void copy_heap(const String& other) noexcept;

    void assign_from(const String& other) noexcept;

    [[nodiscard]] std::size_t grown_capacity(std::size_t minimum) const noexcept;

    void reallocate(std::size_t capacity) noexcept;

    void write_validated(StringView text, std::size_t prefix) noexcept;

    void change_ascii_case(bool upper) noexcept;

    alignas(8) char bytes_[32];
};

static_assert(sizeof(String) == 32 && alignof(String) == 8);

// Zero-overhead parameter adapter accepting either an owned String or a compile-time
// validated UTF-8 string literal (materialized with literal_tag, zero heap allocation).
class Utf8Text {
public:
    constexpr Utf8Text() noexcept : literal_{"", 0}, is_literal_(true) {}

    Utf8Text(String value) noexcept : value_(std::move(value)), is_literal_(false) {}

    template <std::size_t N>
    consteval Utf8Text(const char (&text)[N]) noexcept : literal_{text, N - 1}, is_literal_(true) {
        if (text[N - 1] != '\0' || !StringView{text, N - 1}.is_valid_utf8())
            fatal("UTF-8 string literal requires valid UTF-8 and a trailing zero.");
    }

    template <std::size_t N>
    Utf8Text(char (&)[N]) = delete;

    Utf8Text(const Utf8Text& other) noexcept : is_literal_(other.is_literal_) {
        if (is_literal_)
            literal_ = other.literal_;
        else
            std::construct_at(&value_, other.value_);
    }

    Utf8Text(Utf8Text&& other) noexcept : is_literal_(other.is_literal_) {
        if (is_literal_)
            literal_ = other.literal_;
        else
            std::construct_at(&value_, std::move(other.value_));
    }

    Utf8Text& operator=(const Utf8Text&) = delete;

    Utf8Text& operator=(Utf8Text&&) = delete;

    constexpr ~Utf8Text() {
        if (!is_literal_)
            std::destroy_at(&value_);
    }

    [[nodiscard]] String into_string() && noexcept {
        if (is_literal_)
            return String::from_static_literal(literal_.data, literal_.size);
        return std::move(value_);
    }

    [[nodiscard]] bool empty() const noexcept { return is_literal_ ? literal_.size == 0 : value_.empty(); }

private:
    struct Literal {
        const char* data;

        std::size_t size;
    };

    union {
        Literal literal_;

        String value_;
    };

    bool is_literal_{true};
};

inline String::ConcatPiece::ConcatPiece(const String& text) noexcept : dummy_{} {
    const auto slice = text.view();
    data_ = slice.data();
    size_ = slice.size();
}

inline StringView::StringView(const String& text) noexcept : StringView(text.view()) {}

inline ByteSearchSet::ByteSearchSet(const String& bytes) noexcept : ByteSearchSet(bytes.view()) {}

template <class T>
    requires(std::same_as<std::remove_cvref_t<T>, StringView> || std::same_as<std::remove_cvref_t<T>, String>)
[[noreturn]] inline void fatal(const T& reason) noexcept {
    if constexpr (std::same_as<std::remove_cvref_t<T>, String>)
        fatal(reason.view().std_view());
    else
        fatal(reason.std_view());
}

[[nodiscard]] inline Span<const std::byte> as_bytes(StringView text) noexcept {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

[[nodiscard]] inline StringView as_chars(Span<const std::byte> bytes) noexcept {
    return bytes.empty() ? StringView{} : StringView{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

inline namespace literals {
template <detail::StringLiteral Text>
[[nodiscard]] String operator""_s() noexcept {
    return String::literal<Text>();
}
}

}

template <>
inline constexpr bool std::ranges::enable_borrowed_range<atelier::StringView> = true;

template <>
inline constexpr bool std::ranges::enable_view<atelier::StringView> = true;

template <>
inline constexpr bool std::ranges::enable_borrowed_range<atelier::StringSplit> = true;

template <>
inline constexpr bool std::ranges::enable_view<atelier::StringSplit> = true;

template <>
inline constexpr bool std::ranges::enable_borrowed_range<atelier::StringSplitAny> = true;

template <>
inline constexpr bool std::ranges::enable_view<atelier::StringSplitAny> = true;

template <>
inline constexpr bool std::ranges::enable_borrowed_range<atelier::StringLines> = true;

template <>
inline constexpr bool std::ranges::enable_view<atelier::StringLines> = true;

template <>
inline constexpr bool std::ranges::enable_borrowed_range<atelier::CodePointRange> = true;

template <>
inline constexpr bool std::ranges::enable_view<atelier::CodePointRange> = true;

template <>
struct std::common_type<atelier::String, atelier::StringView> {
    using type = atelier::StringView;
};

template <>
struct std::common_type<atelier::StringView, atelier::String> {
    using type = atelier::StringView;
};

template <>
struct std::common_type<atelier::String, const char*> {
    using type = atelier::StringView;
};

template <>
struct std::common_type<const char*, atelier::String> {
    using type = atelier::StringView;
};

template <>
struct std::common_type<atelier::String, char*> {
    using type = atelier::StringView;
};

template <>
struct std::common_type<char*, atelier::String> {
    using type = atelier::StringView;
};

template <>
struct std::common_type<atelier::StringView, const char*> {
    using type = atelier::StringView;
};

template <>
struct std::common_type<const char*, atelier::StringView> {
    using type = atelier::StringView;
};

template <>
struct std::common_type<atelier::StringView, char*> {
    using type = atelier::StringView;
};

template <>
struct std::common_type<char*, atelier::StringView> {
    using type = atelier::StringView;
};

static_assert(std::equality_comparable_with<const atelier::String&, const char (&)[4]> &&
              std::equality_comparable_with<const atelier::String&, const char*> &&
              std::equality_comparable_with<const atelier::String&, atelier::StringView>);
