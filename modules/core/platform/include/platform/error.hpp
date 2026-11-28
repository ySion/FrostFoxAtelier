#pragma once

#include "foundation/string.hpp"
#include <system_error>

namespace atelier::platform {

// Formats filesystem/Win32 errors as UTF-8 without the CRT's ANSI conversion.
// System-category codes use FormatMessageW; other categories retain their own
// messages. Unknown system codes retain their numeric identity.
String error_message(const std::error_code& error) noexcept;

}
