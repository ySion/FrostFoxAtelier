#include "platform/utf.hpp"
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

std::expected<std::wstring, UtfError> utf8_to_utf16(std::string_view text, std::size_t maximum_units) noexcept {
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

std::wstring utf8_to_utf16_validated(std::string_view text) noexcept {
    auto result = utf8_to_utf16(text);
    if (!result)
        fatal("UTF-16 conversion requires validated, representable UTF-8 text.");
    return std::move(*result);
}

std::expected<std::string, UtfError> utf16_to_utf8(std::wstring_view text) noexcept {
    if (text.empty())
        return std::string{};
    if (text.size() > INT_MAX)
        return std::unexpected(UtfError{UtfError::Code::input_too_large});
    const auto size = static_cast<int>(text.size());
    const auto count =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), size, nullptr, 0, nullptr, nullptr);
    if (!count)
        return conversion_error();
    std::string result(static_cast<std::size_t>(count), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), size, result.data(), count, nullptr, nullptr))
        return conversion_error();
    return result;
}

}
