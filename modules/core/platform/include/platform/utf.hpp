#pragma once

#include "foundation/inline_array.hpp"
#include "foundation/span.hpp"
#include "foundation/string.hpp"

#include <cstddef>
#include <cstdint>
#include <emmintrin.h>
#include <expected>
#include <string>
#include <string_view>

namespace atelier::platform {

struct UtfError {
    enum class Code { invalid_encoding, input_too_large, output_too_large, system_error };

    Code code;

    std::uint32_t system_code{};
};

namespace detail {

[[nodiscard]] std::expected<std::size_t, UtfError> utf8_to_utf16_length(StringView text,
                                                                        std::size_t maximum_units) noexcept;

[[nodiscard]] std::expected<void, UtfError> utf8_to_utf16_write(StringView text, Span<wchar_t> destination) noexcept;

[[nodiscard]] std::expected<void, UtfError> utf16_to_utf8_scratch(std::wstring_view text,
                                                                  InlineArray<char, 128>& out) noexcept;

[[nodiscard]] constexpr bool is_pure_ascii(StringView text) noexcept {
    std::size_t i = 0;
    if !consteval {
        while (text.size() - i >= 16) {
            const auto chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(text.data() + i));
            if (_mm_movemask_epi8(chunk) != 0)
                return false;
            i += 16;
        }
    }
    for (; i < text.size(); ++i)
        if (static_cast<unsigned char>(text.data()[i]) >= 0x80u)
            return false;
    return true;
}

[[nodiscard]] constexpr bool is_pure_ascii(std::wstring_view text) noexcept {
    std::size_t i = 0;
    if !consteval {
        const __m128i mask = _mm_set1_epi16(static_cast<short>(0xff80));
        while (text.size() - i >= 8) {
            const auto chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(text.data() + i));
            if (_mm_movemask_epi8(_mm_cmpeq_epi16(_mm_and_si128(chunk, mask), _mm_setzero_si128())) != 0xffff)
                return false;
            i += 8;
        }
    }
    for (; i < text.size(); ++i)
        if (static_cast<std::uint16_t>(text.data()[i]) >= 0x80u)
            return false;
    return true;
}

} // namespace detail

// Strict conversion: empty input succeeds and embedded NULs are preserved.
// maximum_units counts UTF-16 code units, excluding the terminator, and is
// checked before allocating the output. CPU allocation failure is fatal.
std::expected<std::wstring, UtfError> utf8_to_utf16(std::string_view text,
                                                    std::size_t maximum_units = SIZE_MAX) noexcept;

// For text already validated at its owning boundary. Invalid UTF-8 or a length
// not representable by the Win32 conversion API is a contract violation.
std::wstring utf8_to_utf16_validated(std::string_view text) noexcept;

// Strict conversion; isolated surrogates are rejected rather than replaced.
std::expected<std::string, UtfError> utf16_to_utf8(std::wstring_view text) noexcept;

// Strict conversion into caller-owned inline/spillable UTF-16 storage. Pure ASCII
// widens directly without entering Win32 conversion APIs.
template <std::size_t N>
[[nodiscard]] std::expected<std::wstring_view, UtfError>
utf8_to_utf16_into(StringView text, InlineArray<wchar_t, N>& out, std::size_t maximum_units = SIZE_MAX) noexcept {
    out.clear();
    if (text.empty())
        return std::wstring_view{out.data(), 0};
    if (detail::is_pure_ascii(text)) {
        if (text.size() > maximum_units)
            return std::unexpected(UtfError{UtfError::Code::output_too_large});
        out.resize(text.size());
        wchar_t* dst = out.data();
        const char* src = text.data();
        for (std::size_t i = 0; i < text.size(); ++i)
            dst[i] = static_cast<wchar_t>(static_cast<unsigned char>(src[i]));
        return std::wstring_view{dst, out.size()};
    }
    const auto count = detail::utf8_to_utf16_length(text, maximum_units);
    if (!count)
        return std::unexpected(count.error());
    out.resize(*count);
    if (const auto status = detail::utf8_to_utf16_write(text, out.view()); !status) {
        out.clear();
        return std::unexpected(status.error());
    }
    return std::wstring_view{out.data(), out.size()};
}

// Strict UTF-16 -> validated owning UTF-8 String without an intermediate std::string.
[[nodiscard]] inline std::expected<String, UtfError> utf16_to_utf8_string(std::wstring_view text) noexcept {
    if (text.empty())
        return String{};
    if (text.size() <= String::inline_capacity) {
        char ascii[String::inline_capacity];
        bool pure_ascii = true;
        for (std::size_t i = 0; i < text.size(); ++i) {
            const auto unit = static_cast<std::uint16_t>(text[i]);
            if (unit >= 0x80u) {
                pure_ascii = false;
                break;
            }
            ascii[i] = static_cast<char>(unit);
        }
        if (pure_ascii)
            return String::from_utf8_validated({ascii, text.size()});
    }
    InlineArray<char, 128> scratch;
    if (const auto status = detail::utf16_to_utf8_scratch(text, scratch); !status)
        return std::unexpected(status.error());
    return String::from_utf8_validated({scratch.data(), scratch.size()});
}

// Single-pass zero-allocation offset mapping on validated UTF-8 text.
// Mid-code-point byte offsets snap to the start of the enclosing code point.
[[nodiscard]] constexpr std::size_t utf8_offset_to_utf16(StringView text, std::size_t utf8_offset) noexcept {
    const std::size_t limit = utf8_offset < text.size() ? utf8_offset : text.size();
    std::size_t i = 0;
    std::size_t units = 0;
    if !consteval {
        while (limit - i >= 16) {
            const auto chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(text.data() + i));
            if (_mm_movemask_epi8(chunk) != 0)
                break;
            i += 16;
            units += 16;
        }
    }
    while (i < limit) {
        const auto lead = static_cast<unsigned char>(text.data()[i]);
        if ((lead & 0xc0u) == 0x80u) {
            ++i;
            continue;
        }
        const std::size_t step = lead < 0x80u ? 1 : lead < 0xe0u ? 2 : lead < 0xf0u ? 3 : 4;
        if (i + step > limit)
            break;
        i += step;
        units += step == 4 ? 2 : 1;
    }
    return units;
}

// Maps a UTF-16 code-unit offset (e.g. DirectWrite position) back to a UTF-8 byte offset
// on validated UTF-8 text. Mid-surrogate offsets snap to the start of the surrogate pair.
[[nodiscard]] constexpr std::size_t utf16_offset_to_utf8(StringView text, std::size_t utf16_offset) noexcept {
    std::size_t i = 0;
    std::size_t units = 0;
    if !consteval {
        while (utf16_offset - units >= 16 && text.size() - i >= 16) {
            const auto chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(text.data() + i));
            if (_mm_movemask_epi8(chunk) != 0)
                break;
            i += 16;
            units += 16;
        }
    }
    while (i < text.size() && units < utf16_offset) {
        const auto lead = static_cast<unsigned char>(text.data()[i]);
        if ((lead & 0xc0u) == 0x80u) {
            ++i;
            continue;
        }
        const std::size_t step = lead < 0x80u ? 1 : lead < 0xe0u ? 2 : lead < 0xf0u ? 3 : 4;
        if (i + step > text.size())
            break;
        const std::size_t width = step == 4 ? 2 : 1;
        if (units + width > utf16_offset)
            break;
        i += step;
        units += width;
    }
    return i;
}

} // namespace atelier::platform
