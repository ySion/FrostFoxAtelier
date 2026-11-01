#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace atelier::rendering {

// Immutable equirectangular sky radiance, linear RGB relative to sky_color * sky_intensity.
// Map space: row 0 is +Y and rows run to -Y; u = 0.5 faces +Z and u = 0.75 faces +X.
class SkyMap final {
public:
    // A 4^2 cube keeps DDGI probes stable under an unextracted sun at 64 rays; 16^2 and 8^2 flickered.
    static constexpr std::uint32_t source_width_limit = 8192;
    static constexpr std::uint32_t width_limit = 4096;
    static constexpr std::uint32_t cube_size = 4;
    // width * height RGB floats from row 0; width must be 2 * height. Sources wider than
    // width_limit are box-downsampled 2x2 once; mean, peak and cube use the full source.
    SkyMap(std::uint32_t width, std::uint32_t height, std::span<const float> rgb);
    SkyMap(const SkyMap&) = delete;
    SkyMap& operator=(const SkyMap&) = delete;
    std::uint32_t width() const noexcept { return width_; }
    std::uint32_t height() const noexcept { return height_; }
    // RGBA16F texels, alpha 1, scaled by 2^-scale_exponent so the peak stays within FP16.
    std::span<const std::uint16_t> pixels() const noexcept { return pixels_; }
    int scale_exponent() const noexcept { return scale_exponent_; }
    // Solid-angle mean radiance and largest channel value of the source.
    const std::array<float, 3>& mean() const noexcept { return mean_; }
    float peak() const noexcept { return peak_; }
    // Map-space unit direction of the most luminous source texel.
    const std::array<float, 3>& brightest_direction() const noexcept { return brightest_direction_; }
    // Solid-angle box-filtered source radiance, unscaled RGBA float, cube_size^2 per face in
    // D3D face order +X -X +Y -Y +Z -Z.
    std::span<const float> cube() const noexcept { return cube_; }

private:
    std::uint32_t width_{};
    std::uint32_t height_{};
    std::vector<std::uint16_t> pixels_;
    int scale_exponent_{};
    std::array<float, 3> mean_{};
    float peak_{};
    std::array<float, 3> brightest_direction_{};
    std::vector<float> cube_;
};
using SkyMapRef = std::shared_ptr<const SkyMap>;

// Radiance RGBE (.hdr) with flat, old or new run-length scanlines in -Y H +X W order.
SkyMapRef decode_radiance_hdr(std::string_view bytes);

} // namespace atelier::rendering
