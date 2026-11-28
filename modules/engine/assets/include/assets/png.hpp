#pragma once
#include "runtime/runtime.hpp"
#include <filesystem>
#include <string>

namespace atelier::assets {
// Writes the supplied RGBA8 bytes without color conversion. Caller owns the COM apartment.
Result<void> write_png(std::uint32_t width, std::uint32_t height, Span<const std::uint8_t> rgba,
                       const std::filesystem::path& path);

// Encodes RGBA8 bytes to an in-memory PNG byte stream (manages its own COM apartment).
Result<std::string> encode_png_bytes(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> rgba);
}
