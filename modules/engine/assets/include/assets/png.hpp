#pragma once
#include "runtime/runtime.hpp"
#include <filesystem>

namespace atelier::assets {
// Writes the supplied RGBA8 bytes without color conversion. Caller owns the COM apartment.
Result<void> write_png(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> rgba,
                       const std::filesystem::path& path);
}
