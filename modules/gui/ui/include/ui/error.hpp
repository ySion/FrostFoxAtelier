#pragma once
#include <cstdint>
#include <expected>
#include <string_view>

namespace atelier::ui {
using ControlKey = std::uint64_t; // Application key; zero denotes the layout root.

enum class ErrorCode {
    invalid_surface,
    unavailable_surface,
    invalid_control,
    layout_failed,
    text_failed,
    capacity_exceeded,
    invalid_style,
    invalid_text,
    platform_failed
};

struct Error {
    ErrorCode code{};

    ControlKey key{};

    std::uint32_t native_code{};

    // Non-owning; nonempty messages must refer to static-storage text (string literals).
    std::string_view message{};
};

template <class T>
using Result = std::expected<T, Error>;
}
