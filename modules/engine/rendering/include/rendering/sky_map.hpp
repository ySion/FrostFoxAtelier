#pragma once
#include "foundation/array.hpp"
#include "foundation/span.hpp"
#include "math/vector.hpp"
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace atelier::rendering {

enum class ErrorCode { invalid_argument, invalid_storage };

struct Error {
    ErrorCode code;

    std::string message;
};

template <class T>
using Result = std::expected<T, Error>;

class SkyMap;

using SkyMapRef = std::shared_ptr<const SkyMap>;

// Immutable equirectangular sky radiance, linear RGB relative to sky_color * sky_intensity.
// Map space: row 0 is +Y and rows run to -Y; u = 0.5 faces +Z and u = 0.75 faces +X.
class SkyMap final {
public:
    // A 4^2 cube keeps DDGI probes stable under an unextracted sun at 64 rays; 16^2 and 8^2 flickered.
    static constexpr std::uint32_t source_width_limit = 8192;

    static constexpr std::uint32_t width_limit = 4096;

    static constexpr std::uint32_t cube_size = 4;

    // width * height RGB floats from row 0; width must be 2 * height. Sources wider than
    // width_limit are box-downsampled 2x2 once; mean and cube use the full source.
    // Invalid dimensions or radiance return an error without publishing a resource.
    [[nodiscard]] static Result<SkyMapRef> create(std::uint32_t width, std::uint32_t height, Span<const float> rgb);

    SkyMap(const SkyMap&) = delete;

    SkyMap& operator=(const SkyMap&) = delete;

    std::uint32_t width() const noexcept { return width_; }

    std::uint32_t height() const noexcept { return height_; }

    // RGBA16F texels, alpha 1, scaled by 2^-scale_exponent so the peak stays within FP16.
    Span<const std::uint16_t> pixels() const noexcept { return pixels_; }

    int scale_exponent() const noexcept { return scale_exponent_; }

    // Solid-angle mean radiance of the source.
    const math::Vector3& mean() const noexcept { return mean_; }

    // Solid-angle box-filtered source radiance, unscaled RGBA float, cube_size^2 per face in
    // D3D face order +X -X +Y -Y +Z -Z.
    Span<const float> cube() const noexcept { return cube_; }

private:
    SkyMap(std::uint32_t width, std::uint32_t height, Span<const float> rgb);

    std::uint32_t width_{};

    std::uint32_t height_{};

    Array<std::uint16_t> pixels_;

    int scale_exponent_{};

    math::Vector3 mean_{};

    Array<float> cube_;
};

// Radiance RGBE (.hdr) with flat, old or new run-length scanlines in -Y H +X W order.
[[nodiscard]] Result<SkyMapRef> decode_radiance_hdr(std::string_view bytes);

} // namespace atelier::rendering
