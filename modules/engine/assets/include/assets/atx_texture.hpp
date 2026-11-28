#pragma once

#include "foundation/array.hpp"
#include "foundation/span.hpp"
#include "material/image.hpp"
#include "math/vector.hpp"
#include "rendering/sky_map.hpp"
#include "runtime/runtime.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>

namespace atelier::assets {

enum class AtxFormat : std::uint32_t {
    rgba8_unorm = 0,
    rgba8_srgb = 1,
    rgba16_sfloat = 2,
    bc1_rgba_unorm = 3,
    bc1_rgba_srgb = 4,
    bc4_r_unorm = 5,
    bc5_rg_unorm = 6,
    bc6h_uf16 = 7,
    bc7_rgba_unorm = 8,
    bc7_rgba_srgb = 9,
};

enum class AtxRole : std::uint8_t {
    color_srgb = 0, // BC7_RGBA_SRGB (alpha-weighted linear mip filtering); <4x4 or non-multiple-of-4 stays RGBA8_SRGB
    linear_rgba =
        1, // BC7_RGBA_UNORM (packed multi-channel ORM / linear RGBA, zero channel crosstalk); <4x4 stays RGBA8_UNORM
    scalar_bc4 = 2, // BC4_R_UNORM single-channel grayscale scalar (roughness / metallic / opacity / micro_ao)
    normal_bc5 = 3, // BC5_RG_UNORM 2-channel tangent normal + Toksvig mip variance table
    color_opaque_bc1 = 4, // BC1_RGBA_SRGB 4bpp opaque sRGB albedo; <4x4 stays RGBA8_SRGB
    hdr_sky_bc6h = 5, // BC6H_UF16 8bpp GGX-prefiltered HDR sky map; <4x4 or non-multiple-of-4 stays RGBA16_SFLOAT
};

struct AtxMipSlice {
    std::uint64_t byte_offset{}; // 512B-aligned offset inside AtxTexture::payload

    std::uint32_t row_pitch{}; // 256B-aligned D3D12 copyable row pitch

    std::uint32_t slice_pitch{}; // row_pitch * num_block_rows

    std::uint32_t width{};

    std::uint32_t height{};
};

struct AtxTexture {
    AtxFormat format{AtxFormat::rgba8_unorm};

    AtxRole role{AtxRole::linear_rgba};

    std::uint32_t width{};

    std::uint32_t height{};

    Array<AtxMipSlice> mips;

    // Average normal length ||N_avg|| per mip for Toksvig roughness compensation (1.0 when non-normal).
    Array<float> toksvig_mip_lengths;

    Array<std::uint8_t> payload; // All mips tightly packed at 512B-aligned offsets with 256B-aligned row pitches.
};

using AtxTextureRef = std::shared_ptr<const AtxTexture>;

[[nodiscard]] bool is_block_compressed(AtxFormat format) noexcept;

[[nodiscard]] float compute_atx_toksvig_slope(const AtxTexture& tex) noexcept;

// Cooks an immutable material::Image into a mipmapped BCn (or <4px / unaligned RGBA8 fallback) AtxTexture.
[[nodiscard]] AtxTextureRef cook_atx_image(const material::Image& image, AtxRole role);

// Cooks a prefiltered rendering::SkyMap into a mipmapped BC6H_UF16 (or <4px / unaligned RGBA16_SFLOAT fallback) AtxTexture.
[[nodiscard]] AtxTextureRef cook_atx_sky_map(const rendering::SkyMap& sky);

// Serializes / deserializes the self-contained binary .atx container ("ATEX" v4).
[[nodiscard]] Array<std::uint8_t> encode_atx(const AtxTexture& tex);

[[nodiscard]] Result<AtxTextureRef> decode_atx(Span<const std::uint8_t> bytes);

// Disk cache helpers: load Cache/Textures/<digest>.atx if valid, otherwise cook and atomically write it.
[[nodiscard]] AtxTextureRef load_or_cook_atx_image(const material::Image& image, AtxRole role,
                                                   const std::filesystem::path& cache_dir = {});

[[nodiscard]] AtxTextureRef load_or_cook_atx_sky_map(const rendering::SkyMap& sky,
                                                     const std::filesystem::path& cache_dir = {});

} // namespace atelier::assets
