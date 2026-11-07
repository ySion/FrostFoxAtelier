#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace atelier::platform {

struct UtfError {
    enum class Code { invalid_encoding, input_too_large, output_too_large, system_error };

    Code code;

    std::uint32_t system_code{};
};

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

}
