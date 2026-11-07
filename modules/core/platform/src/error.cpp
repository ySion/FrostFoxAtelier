#include "platform/error.hpp"
#include "platform/utf.hpp"
#include <Windows.h>

namespace atelier::platform {

std::string error_message(const std::error_code& error) noexcept {
    if (error.category() != std::system_category())
        return error.message();
    wchar_t* buffer{};
    const auto count =
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, static_cast<DWORD>(error.value()), 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    if (!count)
        return "Windows error " + std::to_string(error.value());
    auto text = std::wstring_view(buffer, count);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n'))
        text.remove_suffix(1);
    auto result = utf16_to_utf8(text);
    LocalFree(buffer);
    return result ? std::move(*result) : "Windows error " + std::to_string(error.value());
}

}
