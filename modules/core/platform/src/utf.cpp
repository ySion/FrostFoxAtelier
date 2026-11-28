#include "platform/utf.hpp"
#include "foundation/array.hpp"
#include "memory/fatal.hpp"

#include <Windows.h>
#include <climits>
#include <utility>

namespace atelier::platform {
namespace {

std::unexpected<UtfError> conversion_error() noexcept {
    const auto code = GetLastError();
    return std::unexpected(UtfError{
        code == ERROR_NO_UNICODE_TRANSLATION ? UtfError::Code::invalid_encoding : UtfError::Code::system_error, code});
}

} // namespace

namespace detail {

std::expected<std::size_t, UtfError> utf8_to_utf16_length(StringView text, std::size_t maximum_units) noexcept {
    if (text.empty())
        return std::size_t{0};
    if (text.size() > INT_MAX)
        return std::unexpected(UtfError{UtfError::Code::input_too_large});
    const auto size = static_cast<int>(text.size());
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, nullptr, 0);
    if (!count)
        return conversion_error();
    if (static_cast<std::size_t>(count) > maximum_units)
        return std::unexpected(UtfError{UtfError::Code::output_too_large});
    return static_cast<std::size_t>(count);
}

std::expected<void, UtfError> utf8_to_utf16_write(StringView text, Span<wchar_t> destination) noexcept {
    if (text.empty())
        return {};
    if (text.size() > INT_MAX || destination.size() > INT_MAX)
        return std::unexpected(UtfError{UtfError::Code::input_too_large});
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                             destination.data(), static_cast<int>(destination.size())))
        return conversion_error();
    return {};
}

std::expected<void, UtfError> utf16_to_utf8_scratch(std::wstring_view text, InlineArray<char, 128>& out) noexcept {
    out.clear();
    if (text.empty())
        return {};
    if (is_pure_ascii(text)) {
        out.resize(text.size());
        char* dst = out.data();
        for (std::size_t i = 0; i < text.size(); ++i)
            dst[i] = static_cast<char>(static_cast<std::uint16_t>(text[i]));
        return {};
    }
    if (text.size() > INT_MAX)
        return std::unexpected(UtfError{UtfError::Code::input_too_large});
    const auto size = static_cast<int>(text.size());
    const auto count =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), size, nullptr, 0, nullptr, nullptr);
    if (!count)
        return conversion_error();
    out.resize(static_cast<std::size_t>(count));
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), size, out.data(), count, nullptr, nullptr)) {
        out.clear();
        return conversion_error();
    }
    return {};
}

} // namespace detail

std::expected<std::wstring, UtfError> utf8_to_utf16(StringView text, std::size_t maximum_units) noexcept {
    if (text.empty())
        return std::wstring{};
    if (detail::is_pure_ascii(text)) {
        if (text.size() > maximum_units)
            return std::unexpected(UtfError{UtfError::Code::output_too_large});
        std::wstring result(text.size(), L'\0');
        for (std::size_t i = 0; i < text.size(); ++i)
            result[i] = static_cast<wchar_t>(static_cast<unsigned char>(text[i]));
        return result;
    }
    const auto count = detail::utf8_to_utf16_length(text, maximum_units);
    if (!count)
        return std::unexpected(count.error());
    std::wstring result(*count, L'\0');
    if (const auto status = detail::utf8_to_utf16_write(text, {result.data(), result.size()}); !status)
        return std::unexpected(status.error());
    return result;
}

std::wstring utf8_to_utf16_validated(StringView text) noexcept {
    auto result = utf8_to_utf16(text);
    if (!result)
        fatal("UTF-16 conversion requires validated, representable UTF-8 text.");
    return std::move(*result);
}

std::expected<String, UtfError> utf16_to_utf8(std::wstring_view text) noexcept {
    if (text.empty())
        return String{};
    if (detail::is_pure_ascii(text)) {
        if (text.size() <= 512) {
            char stack_bytes[512];
            for (std::size_t i = 0; i < text.size(); ++i)
                stack_bytes[i] = static_cast<char>(static_cast<std::uint16_t>(text[i]));
            return String::from_utf8_validated({stack_bytes, text.size()});
        }
        Array<char> heap_bytes;
        heap_bytes.resize_uninitialized(text.size());
        char* dst = heap_bytes.data();
        for (std::size_t i = 0; i < text.size(); ++i)
            dst[i] = static_cast<char>(static_cast<std::uint16_t>(text[i]));
        return String::from_utf8_validated({dst, text.size()});
    }
    if (text.size() > INT_MAX)
        return std::unexpected(UtfError{UtfError::Code::input_too_large});
    const auto size = static_cast<int>(text.size());
    const auto count =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), size, nullptr, 0, nullptr, nullptr);
    if (!count)
        return conversion_error();
    const auto byte_count = static_cast<std::size_t>(count);
    if (byte_count <= 512) {
        char stack_bytes[512];
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), size, stack_bytes, count, nullptr,
                                 nullptr))
            return conversion_error();
        return String::from_utf8_validated({stack_bytes, byte_count});
    }
    Array<char> heap_bytes;
    heap_bytes.resize_uninitialized(byte_count);
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), size, heap_bytes.data(), count, nullptr,
                             nullptr))
        return conversion_error();
    return String::from_utf8_validated({heap_bytes.data(), byte_count});
}

} // namespace atelier::platform
