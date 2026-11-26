#pragma once

#include "foundation/span.hpp"
#include "memory/fatal.hpp"

#include <bit>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>
#include <expected>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <string_view>
#include <type_traits>

namespace atelier {

class String;

enum class StringError : std::uint8_t { invalid_utf8 };

struct StringPartition;

class StringSplit;

class CodePointRange;

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
}

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

    [[nodiscard]] constexpr bool equals_ascii_case_insensitive(StringView value) const noexcept {
        if (size_ != value.size_)
            return false;
        for (std::size_t i = 0; i != size_; ++i)
            if (detail::ascii_lower(data_[i]) != detail::ascii_lower(value.data_[i]))
                return false;
        return true;
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

    [[nodiscard]] constexpr std::size_t find(StringView value, std::size_t from = 0) const noexcept {
        return std_view().find(value.std_view(), from);
    }

    [[nodiscard]] constexpr std::size_t find(char value, std::size_t from = 0) const noexcept {
        return std_view().find(value, from);
    }

    [[nodiscard]] constexpr std::size_t rfind(StringView value, std::size_t from = npos) const noexcept {
        return std_view().rfind(value.std_view(), from);
    }

    [[nodiscard]] constexpr std::size_t rfind(char value, std::size_t from = npos) const noexcept {
        return std_view().rfind(value, from);
    }

    [[nodiscard]] constexpr std::size_t find_first_of(StringView bytes, std::size_t from = 0) const noexcept {
        return std_view().find_first_of(bytes.std_view(), from);
    }

    [[nodiscard]] constexpr std::size_t find_last_of(StringView bytes, std::size_t from = npos) const noexcept {
        return std_view().find_last_of(bytes.std_view(), from);
    }

    [[nodiscard]] constexpr std::size_t find_first_not_of(StringView bytes, std::size_t from = 0) const noexcept {
        return std_view().find_first_not_of(bytes.std_view(), from);
    }

    [[nodiscard]] constexpr std::size_t find_last_not_of(StringView bytes, std::size_t from = npos) const noexcept {
        return std_view().find_last_not_of(bytes.std_view(), from);
    }

    [[nodiscard]] constexpr bool contains(StringView value) const noexcept { return find(value) != npos; }

    [[nodiscard]] constexpr bool contains(char value) const noexcept { return find(value) != npos; }

    // Byte-set trimming; the default is the six ASCII whitespace bytes, not Unicode whitespace.
    [[nodiscard]] constexpr StringView trim_start(StringView bytes = " \t\r\n\v\f") const noexcept {
        const auto first = find_first_not_of(bytes);
        return first == npos ? StringView{Unchecked{}, data_ + size_, 0}
                             : StringView{Unchecked{}, data_ + first, size_ - first};
    }

    [[nodiscard]] constexpr StringView trim_end(StringView bytes = " \t\r\n\v\f") const noexcept {
        const auto last = find_last_not_of(bytes);
        return {Unchecked{}, data_, last == npos ? 0 : last + 1};
    }

    [[nodiscard]] constexpr StringView trim(StringView bytes = " \t\r\n\v\f") const noexcept {
        return trim_start(bytes).trim_end(bytes);
    }

    [[nodiscard]] constexpr StringPartition partition(StringView delimiter) const noexcept;

    [[nodiscard]] constexpr StringPartition partition(char delimiter) const noexcept;

    [[nodiscard]] constexpr StringPartition rpartition(StringView delimiter) const noexcept;

    [[nodiscard]] constexpr StringPartition rpartition(char delimiter) const noexcept;

    // Both source and delimiter remain borrowed throughout iteration. Empty pieces are retained;
    // an empty delimiter yields the whole source once, including an empty source.
    [[nodiscard]] constexpr StringSplit split(StringView delimiter) const noexcept;

    // Validates the complete view before exposing any code point. Source bytes must remain unchanged.
    [[nodiscard]] std::expected<CodePointRange, StringError> code_points() const noexcept;

    [[nodiscard]] constexpr std::string_view std_view() const noexcept { return {data_, size_}; }

    [[nodiscard]] constexpr bool is_valid_utf8() const noexcept {
        for (std::size_t i = 0; i < size_;) {
            if !consteval {
                while (size_ - i >= 16) {
                    const auto chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data_ + i));
                    const auto mask = static_cast<unsigned>(_mm_movemask_epi8(chunk));
                    if (mask) {
                        i += static_cast<std::size_t>(std::countr_zero(mask));
                        break;
                    }
                    i += 16;
                }
                if (i == size_)
                    return true;
            }
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

        using iterator_concept = std::input_iterator_tag;

        constexpr Iterator() noexcept = default;

        [[nodiscard]] constexpr StringView operator*() const noexcept { return remaining_.slice(0, next_); }

        constexpr Iterator& operator++() noexcept {
            if (next_ == StringView::npos)
                ended_ = true;
            else {
                remaining_ = remaining_.slice(next_ + delimiter_.size());
                locate();
            }
            return *this;
        }

        constexpr void operator++(int) noexcept { ++*this; }

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

    [[nodiscard]] constexpr std::default_sentinel_t end() const noexcept { return {}; }

private:
    friend class StringView;

    constexpr StringSplit(StringView text, StringView delimiter) noexcept : text_(text), delimiter_(delimiter) {}

    StringView text_;

    StringView delimiter_;
};

constexpr StringSplit StringView::split(StringView delimiter) const noexcept {
    return {*this, delimiter};
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

        using iterator_concept = std::input_iterator_tag;

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

        constexpr void operator++(int) noexcept { ++*this; }

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

    [[nodiscard]] constexpr std::default_sentinel_t end() const noexcept { return {}; }

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
}

// Validated UTF-8, owned or borrowed from fixed literal storage, with a trailing zero. Embedded zeros remain part of
// size(). Length and all view positions are bytes, not glyphs or code points.
class String {
public:
    static constexpr std::size_t inline_capacity = 30;

    static constexpr std::size_t max_size = static_cast<std::size_t>(PTRDIFF_MAX) - 1;

    String() noexcept : bytes_{} {}

    [[nodiscard]] static std::expected<String, StringError> from_utf8(StringView text) noexcept;

    // Caller guarantees complete, valid UTF-8. Copies without scanning in either
    // configuration; invalid encoding violates the contract and is not detected.
    [[nodiscard]] static String from_utf8_validated(StringView text) noexcept;

    template <detail::TextInteger T>
    [[nodiscard]] static std::expected<String, std::errc> from_number(T value, int base = 10) noexcept {
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

    [[nodiscard]] std::expected<void, StringError> assign(StringView text) noexcept;

    void assign(const String& text) noexcept { *this = text; }

    [[nodiscard]] std::expected<void, StringError> append(StringView text) noexcept;

    // A complete owning String already satisfies the UTF-8 invariant. Byte
    // views, including slices of this String, still use the checked overload.
    void append(const String& text) noexcept { write_validated(text.view(), size()); }

    // Byte range must be in bounds. Splitting a code point or malformed replacement
    // returns invalid_utf8 without changing this value. Aliased inputs are supported.
    [[nodiscard]] std::expected<void, StringError> replace(std::size_t first, std::size_t count,
                                                           StringView replacement) noexcept;

    // Left-to-right, nonoverlapping matches. Empty search is a no-op, not insertion
    // between bytes. Both views must contain valid UTF-8, even when no match exists.
    [[nodiscard]] std::expected<std::size_t, StringError> replace_all(StringView search,
                                                                      StringView replacement) noexcept;

    void to_ascii_lower() noexcept;

    void to_ascii_upper() noexcept;

    [[nodiscard]] bool starts_with(StringView text) const noexcept { return view().starts_with(text); }

    [[nodiscard]] bool starts_with(char value) const noexcept { return view().starts_with(value); }

    [[nodiscard]] bool ends_with(StringView text) const noexcept { return view().ends_with(text); }

    [[nodiscard]] bool ends_with(char value) const noexcept { return view().ends_with(value); }

    [[nodiscard]] bool equals_ascii_case_insensitive(StringView text) const noexcept {
        return view().equals_ascii_case_insensitive(text);
    }

    [[nodiscard]] std::size_t find(StringView text, std::size_t from = 0) const noexcept {
        return view().find(text, from);
    }

    [[nodiscard]] std::size_t find(char value, std::size_t from = 0) const noexcept { return view().find(value, from); }

    [[nodiscard]] std::size_t rfind(StringView text, std::size_t from = StringView::npos) const noexcept {
        return view().rfind(text, from);
    }

    [[nodiscard]] std::size_t rfind(char value, std::size_t from = StringView::npos) const noexcept {
        return view().rfind(value, from);
    }

    [[nodiscard]] bool contains(StringView text) const noexcept { return view().contains(text); }

    [[nodiscard]] bool contains(char value) const noexcept { return view().contains(value); }

    friend bool operator==(const String& left, const String& right) noexcept { return left.view() == right.view(); }

    friend bool operator==(const String& left, StringView right) noexcept { return left.view() == right; }

    friend auto operator<=>(const String& left, const String& right) noexcept { return left.view() <=> right.view(); }

    friend auto operator<=>(const String& left, StringView right) noexcept { return left.view() <=> right; }

private:
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

inline StringView::StringView(const String& text) noexcept : StringView(text.view()) {}

[[nodiscard]] inline Span<const std::byte> as_bytes(StringView text) noexcept {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

}

template <>
inline constexpr bool std::ranges::enable_borrowed_range<atelier::StringView> = true;

template <>
inline constexpr bool std::ranges::enable_view<atelier::StringView> = true;
