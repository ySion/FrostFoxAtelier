#pragma once
#include <cstdint>
#include <span>

namespace atelier::ui {
enum class Icon { pointer, translate, rotate, scale, transform, cursor3d, magnet, orbit, pan, fly };
// Rasterizes the bundled Tabler mask into tightly packed R8 alpha. Requires
// 1..256 pixels and exactly pixels*pixels output bytes; throws on invalid input.
// Call on a cache miss, never per frame. No window or text backend is required.
void raster_icon(Icon, std::uint32_t pixels, std::span<std::uint8_t> alpha);
}
