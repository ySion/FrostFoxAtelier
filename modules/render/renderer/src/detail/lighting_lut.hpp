#pragma once

#include "gpu_resources.hpp"

namespace atelier::renderer::detail {

struct LightingLutSet {
    OwnedTexture ltc_matrix; // 64x64 RGBA32_SFLOAT [m00, m20, m02, m22]
    OwnedTexture ltc_amplitude; // 64x64 RG32_SFLOAT [norm, schlick_moment]
    OwnedTexture charlie_lut; // 32x32 R16_SFLOAT directional albedo for OpenPBR Fuzz/Velvet
    OwnedTexture atmos_transmittance; // 256x64 RGBA16_SFLOAT Hillaire 2020 transmittance LUT
    OwnedTexture atmos_multiscatter; // 32x32 RGBA16_SFLOAT Hillaire 2020 multi-scattering LUT
    OwnedTexture smaa_area; // 160x560 RG8_UNORM Jiménez SMAA AreaTex
    OwnedTexture smaa_search; // 64x16 R8_UNORM Jiménez SMAA SearchTex

    bool initialized = false;

    [[nodiscard]] Result<void> ensure(rhi::Device& device);

    [[nodiscard]] std::uint64_t logical_bytes() const noexcept {
        return ltc_matrix.byte_size + ltc_amplitude.byte_size + charlie_lut.byte_size + atmos_transmittance.byte_size +
               atmos_multiscatter.byte_size + smaa_area.byte_size + smaa_search.byte_size;
    }

    void retire(rhi::Device& device) noexcept;
};

} // namespace atelier::renderer::detail
