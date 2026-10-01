#pragma once

#include <cstdint>
#include <memory>
#include <vector>

// CPU rendering protocol. No window, layout, text backend, Runtime or GPU types.
namespace atelier::ui {

struct PixelExtent {
    std::uint32_t width{}, height{};
    bool operator==(const PixelExtent&) const = default;
};
struct Rect {
    float x{}, y{}, width{}, height{};
    bool operator==(const Rect&) const = default;
};
// Half-open physical pixel bounds; intersect with the actual render target.
struct Clip {
    std::int32_t left{}, top{}, right{}, bottom{};
    bool operator==(const Clip&) const = default;
};
// Linear, straight RGBA in [0,1]. Coverage multiplies alpha, not RGB.
// Consumers pair shader output with matching blending: straight/SRC_ALPHA or
// premultiplied/ONE, with INV_SRC_ALPHA for the destination in either case.
struct Color {
    float r{}, g{}, b{}, a{1};
    bool operator==(const Color&) const = default;
};
struct CornerRadii {
    float top_left{}, top_right{}, bottom_right{}, bottom_left{};
    bool operator==(const CornerRadii&) const = default;
};
// Immutable color image snapshot. RGB bytes encode sRGB(linear RGB * alpha);
// alpha is linear. Sampling therefore returns linear premultiplied color.
// Identity/revision and publication follow the same rules as Coverage.
struct ColorImage {
    std::uint64_t identity{}, revision{};
    std::uint32_t width{}, height{}, row_pitch{};
    std::vector<std::uint8_t> pixels;
};
// R8 coverage, top-left origin, rows advancing downward. Pixels are UNORM and
// are not sRGB. identity is nonzero and unique across live producer sessions;
// an identity/revision pair must never name different bytes or dimensions.
// Producers relinquish mutable aliases before publishing shared_ptr<const>.
struct Coverage {
    std::uint64_t identity{}, revision{};
    std::uint32_t width{}, height{}, row_pitch{};
    std::vector<std::uint8_t> pixels;
};
struct DrawQuad {
    Rect bounds; // Physical pixels, independent of window logical coordinates.
    Clip clip;
    Color color;
    Rect uv{0, 0, 1, 1}; // Normalized texture coordinates.
    std::shared_ptr<const Coverage> coverage;
    CornerRadii radius{}; // Physical pixels; normalized to fit the box by the renderer.
    float border_width{}; // Uniform inside border, in physical pixels.
    Color border_color{0, 0, 0, 0};
    std::shared_ptr<const ColorImage> image{}; // Exclusive with coverage; color tints the image.
    Color image_background{0, 0, 0, 0}; // Base fill under image, before the common rounded mask.
};
// Draw in vector order; later entries cover earlier ones. All geometry and
// resources are immutable after publication. Hold this frame through render().
// A renderer owns GPU uploads and their in-flight retirement independently.
struct Frame {
    std::uint64_t id{};
    PixelExtent extent; // Actual backbuffer extent used to prepare these draws.
    float dpi_scale{1}; // Already applied to bounds/clip; never apply it again.
    std::uint64_t surface_revision{};
    std::vector<DrawQuad> draws;
};

} // namespace atelier::ui
