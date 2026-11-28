#pragma once
#include "foundation/array.hpp"
#include "foundation/span.hpp"
#include "foundation/string.hpp"
#include "math/vector.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>

namespace atelier::rendering {

enum class ErrorCode { invalid_argument, invalid_storage };

struct Error {
    Error() = default;

    Error(ErrorCode error_code, Utf8Text text = {}) : code(error_code), message(std::move(text).into_string()) {}

    ErrorCode code{};

    String message;
};

template <class T>
using Result = std::expected<T, Error>;

class SkyMap;

using SkyMapRef = std::shared_ptr<const SkyMap>;

// Immutable equirectangular sky radiance, linear RGB relative to sky_color * sky_intensity.
// Map space: row 0 is +Y and rows run to -Y; u = 0.5 faces +Z and u = 0.75 faces +X.
class SkyMap final {
public:
    static constexpr std::uint32_t source_width_limit = 8192;

    static constexpr std::uint32_t width_limit = 4096;

    static constexpr std::uint32_t cdf_width = 256;

    static constexpr std::uint32_t cdf_height = 128;

    struct MipLevel {
        std::uint32_t width{};

        std::uint32_t height{};

        // RGBA16F texels, alpha 1, scaled by 2^-scale_exponent so the peak stays within FP16.
        Array<std::uint16_t> pixels;
    };

    // width * height RGB floats from row 0; width must be 2 * height. Sources wider than
    // width_limit are box-downsampled 2x2 once; mean, SH3, and importance CDF use the full source.
    // Invalid dimensions or radiance return an error without publishing a resource.
    [[nodiscard]] static Result<SkyMapRef> create(std::uint32_t width, std::uint32_t height, Span<const float> rgb);

    SkyMap(const SkyMap&) = delete;

    SkyMap& operator=(const SkyMap&) = delete;

    std::uint32_t width() const noexcept { return width_; }

    std::uint32_t height() const noexcept { return height_; }

    // Mip 0 RGBA16F texels, alpha 1, scaled by 2^-scale_exponent so the peak stays within FP16.
    Span<const std::uint16_t> pixels() const noexcept { return mip_chain_.front().pixels; }

    // Full GGX roughness-prefiltered mip chain (Mip 0 = mirror, Mip M-1 = roughness 1.0).
    Span<const MipLevel> mip_chain() const noexcept { return mip_chain_; }

    int scale_exponent() const noexcept { return scale_exponent_; }

    // Solid-angle mean radiance of the source.
    const math::Vector3& mean() const noexcept { return mean_; }

    // Pre-convolved 3-band (L0..L2, 9 coefficients) Ramamoorthi diffuse irradiance E(n)/pi (unscaled linear RGB):
    // [0]=1, [1]=n.y, [2]=n.z, [3]=n.x, [4]=n.x*n.y, [5]=n.y*n.z, [6]=3*n.z^2-1, [7]=n.x*n.z, [8]=n.x^2-n.y^2.
    const std::array<math::Vector3, 9>& sh3() const noexcept { return sh3_; }

    // Precomputed 2D importance-sampling CDF: [0..cdf_height-1] marginal row CDF + [cdf_height..] 256x128 conditional row CDFs.
    Span<const float> importance_cdf() const noexcept { return importance_cdf_; }

    // SHA-256 hex digest of the canonical precomputed SkyMap payload.
    StringView content_sha256() const noexcept { return content_sha256_; }

private:
    SkyMap(std::uint32_t width, std::uint32_t height, Span<const float> rgb);

    std::uint32_t width_{};

    std::uint32_t height_{};

    Array<MipLevel> mip_chain_;

    int scale_exponent_{};

    math::Vector3 mean_{};

    std::array<math::Vector3, 9> sh3_{};

    Array<float> importance_cdf_;

    String content_sha256_;
};

// Radiance RGBE (.hdr) with flat, old or new run-length scanlines in -Y H +X W order.
[[nodiscard]] Result<SkyMapRef> decode_radiance_hdr(Span<const std::byte> bytes);

} // namespace atelier::rendering
