#pragma once

#include "memory/fatal.hpp"

#include <cstddef>
#include <cstdint>
#include <charconv>
#include <concepts>
#include <cstring>
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

    StringView(const String&) noexcept;

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

    [[nodiscard]] constexpr StringView slice(std::size_t first, std::size_t count = npos) const noexcept {
        if (first > size_ || (count != npos && count > size_ - first))
            fatal("StringView slice.");
        return {data_ + first, count == npos ? size_ - first : count};
    }

    // The entire view must already be valid UTF-8. Only the cut is adjusted;
    // this does not validate bytes or detect a violated precondition.
    [[nodiscard]] constexpr StringView utf8_prefix(std::size_t maximum_bytes) const noexcept {
        if (size_ <= maximum_bytes)
            return *this;
        while (maximum_bytes && (static_cast<unsigned char>(data_[maximum_bytes]) & 0xc0) == 0x80)
            --maximum_bytes;
        return {data_, maximum_bytes};
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
        return size_ >= value.size_ && StringView{data_, value.size_}.equals_ascii_case_insensitive(value);
    }

    [[nodiscard]] constexpr bool ends_with_ascii_case_insensitive(StringView value) const noexcept {
        return size_ >= value.size_ &&
               StringView{data_ + size_ - value.size_, value.size_}.equals_ascii_case_insensitive(value);
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

    [[nodiscard]] constexpr bool contains(StringView value) const noexcept { return find(value) != npos; }

    // Byte-set trimming; the default is the six ASCII whitespace bytes, not Unicode whitespace.
    [[nodiscard]] constexpr StringView trim_start(StringView bytes = " \t\r\n\v\f") const noexcept {
        const auto first = std_view().find_first_not_of(bytes.std_view());
        return slice(first == npos ? size_ : first);
    }

    [[nodiscard]] constexpr StringView trim_end(StringView bytes = " \t\r\n\v\f") const noexcept {
        const auto last = std_view().find_last_not_of(bytes.std_view());
        return slice(0, last == npos ? 0 : last + 1);
    }

    [[nodiscard]] constexpr StringView trim(StringView bytes = " \t\r\n\v\f") const noexcept {
        return trim_start(bytes).trim_end(bytes);
    }

    [[nodiscard]] constexpr StringPartition partition(StringView delimiter) const noexcept;

    [[nodiscard]] constexpr StringPartition rpartition(StringView delimiter) const noexcept;

    // Both source and delimiter remain borrowed throughout iteration. Empty pieces are retained;
    // an empty delimiter yields the whole source once, including an empty source.
    [[nodiscard]] constexpr StringSplit split(StringView delimiter) const noexcept;

    // Validates the complete view before exposing any code point. Source bytes must remain unchanged.
    [[nodiscard]] std::expected<CodePointRange, StringError> code_points() const noexcept;

    [[nodiscard]] constexpr std::string_view std_view() const noexcept { return {data_, size_}; }

    [[nodiscard]] constexpr bool is_valid_utf8() const noexcept {
        for (std::size_t i = 0; i < size_;) {
            if (!std::is_constant_evaluated()) {
                while (size_ - i >= sizeof(std::uint64_t)) {
                    std::uint64_t bytes;
                    std::memcpy(&bytes, data_ + i, sizeof(bytes));
                    if (bytes & UINT64_C(0x8080808080808080))
                        break;
                    i += sizeof(bytes);
                }
            }
            if (i == size_)
                return true;
            const auto first = static_cast<unsigned char>(data_[i++]);
            if (first < 0x80)
                continue;
            const unsigned count = first >= 0xc2 && first <= 0xdf   ? 1
                                   : first >= 0xe0 && first <= 0xef ? 2
                                   : first >= 0xf0 && first <= 0xf4 ? 3
                                                                    : 0;
            if (!count || count > size_ - i)
                return false;
            const auto second = static_cast<unsigned char>(data_[i]);
            if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second >= 0xa0) ||
                (first == 0xf0 && second < 0x90) || (first == 0xf4 && second >= 0x90))
                return false;
            for (unsigned j = 0; j != count; ++j)
                if ((static_cast<unsigned char>(data_[i++]) & 0xc0) != 0x80)
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
                      : StringPartition{slice(0, at), slice(at, delimiter.size()), slice(at + delimiter.size())};
}

constexpr StringPartition StringView::rpartition(StringView delimiter) const noexcept {
    const auto at = delimiter.empty() ? npos : std_view().rfind(delimiter.std_view());
    return at == npos ? StringPartition{{}, {}, *this}
                      : StringPartition{slice(0, at), slice(at, delimiter.size()), slice(at + delimiter.size())};
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
            for (std::size_t i = 1; i < count; ++i)
                value = (value << 6) | (static_cast<unsigned char>(text_[offset_ + i]) & 0x3f);
            return {value, offset_, count};
        }

        constexpr Iterator& operator++() noexcept {
            offset_ += (**this).byte_count;
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
}

// Validated UTF-8, owned or borrowed from fixed literal storage, with a trailing zero. Embedded zeros remain part of
// size(). Length and all view positions are bytes, not glyphs or code points.
class String {
public:
    static constexpr std::size_t inline_capacity = 30;

    static constexpr std::size_t max_size = static_cast<std::size_t>(PTRDIFF_MAX) - 1;

    String() noexcept = default;

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
        String text;
        text.write_validated({bytes, static_cast<std::size_t>(result.ptr - bytes)}, 0);
        return text;
    }

    // Shortest general representation that round-trips through from_chars.
    template <std::floating_point T>
    [[nodiscard]] static String from_number(T value) noexcept {
        char bytes[std::numeric_limits<T>::max_digits10 + 16];
        const auto result = std::to_chars(bytes, bytes + sizeof(bytes), value, std::chars_format::general);
        String text;
        text.write_validated({bytes, static_cast<std::size_t>(result.ptr - bytes)}, 0);
        return text;
    }

    // The template parameter object has static storage duration. Its module
    // must remain loaded until every copy dies; JIT text must use from_utf8.
    template <detail::StringLiteral Text>
    [[nodiscard]] static String literal() noexcept {
        String result;
        constexpr auto length = sizeof(Text.bytes) - 1;
        static_assert(length <= max_size && Text.bytes[length] == '\0' &&
                      StringView{Text.bytes, length}.is_valid_utf8());
        result.set_long({Text.bytes, length, length}, literal_tag);
        return result;
    }

    String(const String&) noexcept;

    String& operator=(const String&) noexcept;

    String(String&&) noexcept;

    String& operator=(String&&) noexcept;

    ~String() { release(); }

    [[nodiscard]] std::size_t size() const noexcept { return is_inline() ? tag() : long_value().size; }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return is_inline() ? inline_capacity : long_value().capacity;
    }

    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    [[nodiscard]] const char* c_str() const noexcept { return is_inline() ? bytes_ : long_value().data; }

    [[nodiscard]] StringView view() const& noexcept { return {c_str(), size()}; }

    StringView view() const&& = delete;

    [[nodiscard]] char operator[](std::size_t index) const noexcept { return StringView{c_str(), size()}[index]; }

    void reserve(std::size_t bytes) noexcept;

    void squeeze() noexcept;

    void clear() noexcept;

    void release() noexcept;

    void swap(String& other) noexcept;

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

    [[nodiscard]] bool contains(StringView text) const noexcept { return view().contains(text); }

    friend bool operator==(const String& left, const String& right) noexcept { return left.view() == right.view(); }

    friend auto operator<=>(const String& left, const String& right) noexcept { return left.view() <=> right.view(); }

private:
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
        bytes_[31] = static_cast<char>(mode);
    }

    [[nodiscard]] std::size_t grown_capacity(std::size_t minimum) const noexcept;

    void reallocate(std::size_t capacity) noexcept;

    void write_validated(StringView text, std::size_t prefix) noexcept;

    void change_ascii_case(bool upper) noexcept;

    void move_from(String& other) noexcept;

    alignas(8) char bytes_[32]{};
};

static_assert(sizeof(String) == 32 && alignof(String) == 8);

inline StringView::StringView(const String& text) noexcept : StringView(text.view()) {}

}
