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
}

std::expected<std::wstring, UtfError> utf8_to_utf16(StringView text, std::size_t maximum_units) noexcept {
    if (text.empty())
        return std::wstring{};
    if (text.size() > INT_MAX)
        return std::unexpected(UtfError{UtfError::Code::input_too_large});
    const auto size = static_cast<int>(text.size());
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, nullptr, 0);
    if (!count)
        return conversion_error();
    if (static_cast<std::size_t>(count) > maximum_units)
        return std::unexpected(UtfError{UtfError::Code::output_too_large});
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, result.data(), count))
        return conversion_error();
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

}
