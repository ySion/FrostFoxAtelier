#include "rendering/sky_map.hpp"
#include "foundation/result.hpp"
#include "foundation/sha256.hpp"
#include "rendering/scene_lighting.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <numbers>

namespace atelier::rendering {
namespace {

// Round to nearest even for finite, non-negative values at most 65504; -0 stores +0.
std::uint16_t half_bits(float value) noexcept {
    const float clamped = std::clamp(value, 0.0f, 65504.0f);
    std::uint32_t bits = std::bit_cast<std::uint32_t>(clamped) & 0x7fffffffu;
    if (bits < 0x38800000u) // FP16 subnormal or zero: the 0.5f bias aligns the mantissa.
        return static_cast<std::uint16_t>(std::bit_cast<std::uint32_t>(std::bit_cast<float>(bits) + .5f) - 0x3f000000u);
    bits += 0xc8000fffu + ((bits >> 13) & 1);
    return static_cast<std::uint16_t>(bits >> 13);
}

float radical_inverse_vdc(std::uint32_t bits) noexcept {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

math::Vector3 equirect_uv_to_dir(float u, float v) noexcept {
    constexpr float pi = std::numbers::pi_v<float>;
    const float phi = 2.0f * pi * (u - 0.5f);
    const float theta = pi * (0.5f - v);
    const float cos_t = std::cos(theta);
    return {std::sin(phi) * cos_t, std::sin(theta), std::cos(phi) * cos_t};
}

struct FloatBoxMip {
    std::uint32_t width{};

    std::uint32_t height{};

    std::vector<float> rgb;
};

math::Vector3 sample_equirect_bilinear(const FloatBoxMip& mip, const math::Vector3& dir) noexcept {
    constexpr float inv_two_pi = 0.5f / std::numbers::pi_v<float>;
    constexpr float inv_pi = 1.0f / std::numbers::pi_v<float>;
    float u = std::atan2(dir.x, dir.z) * inv_two_pi + 0.5f;
    u -= std::floor(u);
    const float v = std::clamp(0.5f - std::asin(std::clamp(dir.y, -1.0f, 1.0f)) * inv_pi, 0.0f, 1.0f);

    const float fx = u * static_cast<float>(mip.width) - 0.5f;
    const float fy = v * static_cast<float>(mip.height) - 0.5f;
    const int ix = static_cast<int>(std::floor(fx));
    const int iy = static_cast<int>(std::floor(fy));
    const float wx = fx - static_cast<float>(ix);
    const float wy = fy - static_cast<float>(iy);

    const auto wrap_x = [&](int x) noexcept -> std::uint32_t {
        const int w = static_cast<int>(mip.width);
        return static_cast<std::uint32_t>(((x % w) + w) % w);
    };
    const auto clamp_y = [&](int y) noexcept -> std::uint32_t {
        return static_cast<std::uint32_t>(std::clamp(y, 0, static_cast<int>(mip.height) - 1));
    };
    const auto fetch = [&](std::uint32_t x, std::uint32_t y) noexcept -> math::Vector3 {
        const float* p = &mip.rgb[(std::size_t(y) * mip.width + x) * 3u];
        return {p[0], p[1], p[2]};
    };

    const std::uint32_t x0 = wrap_x(ix);
    const std::uint32_t x1 = wrap_x(ix + 1);
    const std::uint32_t y0 = clamp_y(iy);
    const std::uint32_t y1 = clamp_y(iy + 1);
    const math::Vector3 r0 = fetch(x0, y0) * (1.0f - wx) + fetch(x1, y0) * wx;
    const math::Vector3 r1 = fetch(x0, y1) * (1.0f - wx) + fetch(x1, y1) * wx;
    return r0 * (1.0f - wy) + r1 * wy;
}

math::Vector3 sample_box_trilinear(std::span<const FloatBoxMip> pyramid, const math::Vector3& dir,
                                   float lod) noexcept {
    const float clamped = std::clamp(lod, 0.0f, static_cast<float>(pyramid.size() - 1u));
    const auto l0 = static_cast<std::size_t>(std::floor(clamped));
    const std::size_t l1 = std::min(l0 + 1u, pyramid.size() - 1u);
    const float t = clamped - static_cast<float>(l0);
    if (l0 == l1 || t <= 1e-4f)
        return sample_equirect_bilinear(pyramid[l0], dir);
    return sample_equirect_bilinear(pyramid[l0], dir) * (1.0f - t) + sample_equirect_bilinear(pyramid[l1], dir) * t;
}

} // namespace

Result<SkyMapRef> SkyMap::create(std::uint32_t width, std::uint32_t height, std::span<const float> rgb) {
    if (!height || width > source_width_limit || width != 2ull * height ||
        rgb.size() != std::size_t(width) * height * 3)
        return std::unexpected(Error{ErrorCode::invalid_argument,
                                     "Sky map must be a 2:1 equirectangular RGB image at most 8192 pixels wide."});
    if (width > width_limit && height % 2)
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "Sky maps wider than 4096 pixels need an even height."});
    for (float value : rgb)
        if (!(value >= 0 && value <= analytic_light_radiance_limit))
            return std::unexpected(
                Error{ErrorCode::invalid_argument,
                      "Sky map radiance must be finite, non-negative and at most 2^32 per channel."});
    return SkyMapRef(new SkyMap(width, height, rgb));
}

SkyMap::SkyMap(std::uint32_t width, std::uint32_t height, std::span<const float> rgb) {
    constexpr double pi = std::numbers::pi;

    // 1. Exact solid-angle mean, 3-band Ramamoorthi SH irradiance (E(n)/pi), and 256x128 importance CDF.
    std::vector<double> col_sin(width);
    std::vector<double> col_cos(width);
    for (std::uint32_t x = 0; x < width; ++x) {
        const double phi = 2.0 * pi * ((double(x) + 0.5) / double(width) - 0.5);
        col_sin[x] = std::sin(phi);
        col_cos[x] = std::cos(phi);
    }

    std::array<math::Vector3d, 9> sh_accum{};
    double total_solid_angle = 0.0;
    std::vector<double> cdf_grid(std::size_t(cdf_width) * cdf_height, 0.0);

    for (std::uint32_t y = 0; y < height; ++y) {
        const double lat_center = pi * (0.5 - (double(y) + 0.5) / double(height));
        const double wy = std::sin(lat_center);
        const double across = std::cos(lat_center);
        const double dw =
            (2.0 * pi / double(width)) *
            (std::sin(pi * (0.5 - double(y) / double(height))) - std::sin(pi * (0.5 - double(y + 1u) / double(height))));
        const std::uint32_t cy = std::min<std::uint32_t>(cdf_height - 1u, (y * cdf_height) / height);

        for (std::uint32_t x = 0; x < width; ++x) {
            const float* texel = &rgb[(std::size_t(y) * width + x) * 3u];
            const math::Vector3d L{texel[0], texel[1], texel[2]};
            const double wx = col_sin[x] * across;
            const double wz = col_cos[x] * across;
            const math::Vector3d L_dw = L * dw;

            sh_accum[0] += L_dw;
            sh_accum[1] += L_dw * wy;
            sh_accum[2] += L_dw * wz;
            sh_accum[3] += L_dw * wx;
            sh_accum[4] += L_dw * (wx * wy);
            sh_accum[5] += L_dw * (wy * wz);
            sh_accum[6] += L_dw * (3.0 * wz * wz - 1.0);
            sh_accum[7] += L_dw * (wx * wz);
            sh_accum[8] += L_dw * (wx * wx - wy * wy);
            total_solid_angle += dw;

            const double lum = (0.2126 * L.x + 0.7152 * L.y + 0.0722 * L.z) * dw;
            const std::uint32_t cx = std::min<std::uint32_t>(cdf_width - 1u, (x * cdf_width) / width);
            cdf_grid[std::size_t(cy) * cdf_width + cx] += lum;
        }
    }

    const double norm_sphere = (total_solid_angle > 0.0) ? (4.0 * pi / total_solid_angle) : 1.0;
    mean_ = math::Vector3(sh_accum[0] / std::max(total_solid_angle, 1e-30));

    // Pre-scaled Ramamoorthi & Hanrahan (2001) cosine-lobe coefficients (divided by pi -> exact E(n)/pi):
    constexpr double c0 = 1.0 / (4.0 * pi);
    constexpr double c1 = 1.0 / (2.0 * pi);
    constexpr double c2_off = 15.0 / (16.0 * pi);
    constexpr double c2_z = 5.0 / (64.0 * pi);
    constexpr double c2_xy = 15.0 / (64.0 * pi);
    const std::array<double, 9> sh_scales{c0, c1, c1, c1, c2_off, c2_off, c2_z, c2_off, c2_xy};
    for (std::size_t k = 0; k < 9u; ++k)
        sh3_[k] = math::Vector3(sh_accum[k] * (norm_sphere * sh_scales[k]));

    // Build [0..cdf_height-1] marginal CDF + [cdf_height..] 256x128 conditional row CDFs.
    importance_cdf_.assign(cdf_height + std::size_t(cdf_width) * cdf_height, 0.0f);
    double marginal_sum = 0.0;
    std::array<double, cdf_height> row_sums{};
    for (std::uint32_t ry = 0; ry < cdf_height; ++ry) {
        double r_sum = 0.0;
        for (std::uint32_t rx = 0; rx < cdf_width; ++rx)
            r_sum += cdf_grid[std::size_t(ry) * cdf_width + rx];
        row_sums[ry] = r_sum;
        marginal_sum += r_sum;

        float* cond_dst = importance_cdf_.data() + cdf_height + std::size_t(ry) * cdf_width;
        if (r_sum > 1e-30) {
            double c_acc = 0.0;
            for (std::uint32_t rx = 0; rx < cdf_width; ++rx) {
                c_acc += cdf_grid[std::size_t(ry) * cdf_width + rx];
                cond_dst[rx] = static_cast<float>(std::clamp(c_acc / r_sum, 0.0, 1.0));
            }
            cond_dst[cdf_width - 1u] = 1.0f;
        } else {
            for (std::uint32_t rx = 0; rx < cdf_width; ++rx)
                cond_dst[rx] = static_cast<float>(rx + 1u) / static_cast<float>(cdf_width);
        }
    }
    if (marginal_sum > 1e-30) {
        double m_acc = 0.0;
        for (std::uint32_t ry = 0; ry < cdf_height; ++ry) {
            m_acc += row_sums[ry];
            importance_cdf_[ry] = static_cast<float>(std::clamp(m_acc / marginal_sum, 0.0, 1.0));
        }
        importance_cdf_[cdf_height - 1u] = 1.0f;
    } else {
        for (std::uint32_t ry = 0; ry < cdf_height; ++ry)
            importance_cdf_[ry] = static_cast<float>(ry + 1u) / static_cast<float>(cdf_height);
    }

    // 2. Cap base mip at width_limit (4096) and determine global FP16 scale_exponent_.
    std::vector<float> halved;
    std::span<const float> stored = rgb;
    if (width > width_limit) {
        width_ = width / 2u;
        height_ = height / 2u;
        halved.resize(std::size_t(width_) * height_ * 3u);
        for (std::uint32_t y = 0; y < height_; ++y) {
            for (std::uint32_t x = 0; x < width_; ++x) {
                for (std::size_t c = 0; c < 3u; ++c) {
                    const auto at = [&](std::uint32_t row, std::uint32_t column) {
                        return rgb[(std::size_t(row) * width + column) * 3u + c];
                    };
                    halved[(std::size_t(y) * width_ + x) * 3u + c] =
                        0.25f *
                        ((at(2u * y, 2u * x) + at(2u * y, 2u * x + 1u)) +
                         (at(2u * y + 1u, 2u * x) + at(2u * y + 1u, 2u * x + 1u)));
                }
            }
        }
        stored = halved;
    } else {
        width_ = width;
        height_ = height;
    }

    const float stored_peak = *std::max_element(stored.begin(), stored.end());
    while (std::ldexp(stored_peak, -scale_exponent_) > 65504.0f)
        ++scale_exponent_;

    // 3. Build auxiliary float box pyramid for Filtered Importance Sampling (Krivanek & Colbert).
    std::uint32_t mip_count = 1u;
    for (std::uint32_t d = std::max(width_, height_); d > 1u; d >>= 1u)
        ++mip_count;

    std::vector<FloatBoxMip> box_pyramid(mip_count);
    box_pyramid[0].width = width_;
    box_pyramid[0].height = height_;
    box_pyramid[0].rgb.resize(stored.size());
    for (std::size_t i = 0; i < stored.size(); ++i)
        box_pyramid[0].rgb[i] = std::ldexp(stored[i], -scale_exponent_);

    for (std::uint32_t m = 1u; m < mip_count; ++m) {
        const auto& prev = box_pyramid[m - 1u];
        auto& cur = box_pyramid[m];
        cur.width = std::max(1u, prev.width >> 1u);
        cur.height = std::max(1u, prev.height >> 1u);
        cur.rgb.resize(std::size_t(cur.width) * cur.height * 3u);
        for (std::uint32_t y = 0; y < cur.height; ++y) {
            const std::uint32_t y0 = std::min(y * 2u, prev.height - 1u);
            const std::uint32_t y1 = std::min(y * 2u + 1u, prev.height - 1u);
            for (std::uint32_t x = 0; x < cur.width; ++x) {
                const std::uint32_t x0 = std::min(x * 2u, prev.width - 1u);
                const std::uint32_t x1 = std::min(x * 2u + 1u, prev.width - 1u);
                const float* p00 = &prev.rgb[(std::size_t(y0) * prev.width + x0) * 3u];
                const float* p10 = &prev.rgb[(std::size_t(y0) * prev.width + x1) * 3u];
                const float* p01 = &prev.rgb[(std::size_t(y1) * prev.width + x0) * 3u];
                const float* p11 = &prev.rgb[(std::size_t(y1) * prev.width + x1) * 3u];
                float* dst = &cur.rgb[(std::size_t(y) * cur.width + x) * 3u];
                for (std::size_t c = 0; c < 3u; ++c)
                    dst[c] = 0.25f * (p00[c] + p10[c] + p01[c] + p11[c]);
            }
        }
    }

    // 4. Populate GGX roughness-prefiltered equirectangular mip chain (Mip 0 = exact mirror, Mip m = roughness m/(M-1)).
    mip_chain_.resize(mip_count);
    mip_chain_[0].width = width_;
    mip_chain_[0].height = height_;
    mip_chain_[0].pixels.resize(std::size_t(width_) * height_ * 4u);
    for (std::size_t i = 0, texels = std::size_t(width_) * height_; i < texels; ++i) {
        mip_chain_[0].pixels[i * 4u + 0u] = half_bits(box_pyramid[0].rgb[i * 3u + 0u]);
        mip_chain_[0].pixels[i * 4u + 1u] = half_bits(box_pyramid[0].rgb[i * 3u + 1u]);
        mip_chain_[0].pixels[i * 4u + 2u] = half_bits(box_pyramid[0].rgb[i * 3u + 2u]);
        mip_chain_[0].pixels[i * 4u + 3u] = 0x3c00u;
    }

    const float omega_texel = (4.0f * std::numbers::pi_v<float>) / static_cast<float>(std::size_t(width_) * height_);
    for (std::uint32_t m = 1u; m < mip_count; ++m) {
        auto& dst_mip = mip_chain_[m];
        dst_mip.width = box_pyramid[m].width;
        dst_mip.height = box_pyramid[m].height;
        dst_mip.pixels.resize(std::size_t(dst_mip.width) * dst_mip.height * 4u);

        const float roughness = static_cast<float>(m) / static_cast<float>(std::max(1u, mip_count - 1u));
        const float a = roughness * roughness;
        const float a2 = a * a;
        const std::uint32_t sample_count = (dst_mip.width >= 1024u) ? 12u : ((dst_mip.width >= 256u) ? 16u : 24u);

        for (std::uint32_t y = 0; y < dst_mip.height; ++y) {
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(dst_mip.height);
            for (std::uint32_t x = 0; x < dst_mip.width; ++x) {
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(dst_mip.width);
                const math::Vector3 N = equirect_uv_to_dir(u, v);
                const math::Vector3 up = (std::abs(N.y) < 0.999f) ? math::Vector3{0.0f, 1.0f, 0.0f}
                                                                  : math::Vector3{1.0f, 0.0f, 0.0f};
                const math::Vector3 T =
                    math::try_normalize(math::cross(up, N)).value_or(math::Vector3{1.0f, 0.0f, 0.0f});
                const math::Vector3 B = math::cross(N, T);

                math::Vector3 prefiltered{};
                float weight_sum = 0.0f;
                for (std::uint32_t s = 0u; s < sample_count; ++s) {
                    const float xi1 = (static_cast<float>(s) + 0.5f) / static_cast<float>(sample_count);
                    const float xi2 = radical_inverse_vdc(s);
                    const float phi = 2.0f * std::numbers::pi_v<float> * xi1;
                    const float cos_theta = std::sqrt((1.0f - xi2) / std::max(1e-6f, 1.0f + (a2 - 1.0f) * xi2));
                    const float sin_theta = std::sqrt(std::max(0.0f, 1.0f - cos_theta * cos_theta));

                    const math::Vector3 H =
                        T * (std::cos(phi) * sin_theta) + B * (std::sin(phi) * sin_theta) + N * cos_theta;
                    const float n_dot_h = std::max(0.0f, math::dot(N, H));
                    const math::Vector3 L = H * (2.0f * n_dot_h) - N;
                    const float n_dot_l = math::dot(N, L);
                    if (n_dot_l > 0.0f) {
                        const float denom = n_dot_h * n_dot_h * (a2 - 1.0f) + 1.0f;
                        const float D = a2 / std::max(1e-6f, std::numbers::pi_v<float> * denom * denom);
                        const float pdf = std::max(1e-6f, D * 0.25f);
                        const float omega_s = 1.0f / (static_cast<float>(sample_count) * pdf);
                        const float lod = std::max(0.0f, 0.5f * std::log2(omega_s / std::max(omega_texel, 1e-12f)));
                        prefiltered += sample_box_trilinear(box_pyramid, L, lod) * n_dot_l;
                        weight_sum += n_dot_l;
                    }
                }
                const math::Vector3 resolved =
                    (weight_sum > 1e-6f) ? (prefiltered * (1.0f / weight_sum))
                                         : sample_equirect_bilinear(box_pyramid[m], N);
                const std::size_t o = (std::size_t(y) * dst_mip.width + x) * 4u;
                dst_mip.pixels[o + 0u] = half_bits(resolved.x);
                dst_mip.pixels[o + 1u] = half_bits(resolved.y);
                dst_mip.pixels[o + 2u] = half_bits(resolved.z);
                dst_mip.pixels[o + 3u] = 0x3c00u;
            }
        }
    }

    // 5. Compute deterministic content_sha256_ over dimensions, exponent, SH3, and Mip 0..M-1 pixels.
    std::string digest_chain = foundation::sha256(std::as_bytes(std::span{sh3_}));
    for (const auto& ml : mip_chain_)
        digest_chain += foundation::sha256(std::as_bytes(std::span{ml.pixels}));
    content_sha256_ = foundation::sha256(std::as_bytes(std::span{digest_chain.data(), digest_chain.size()}));
}

Result<SkyMapRef> decode_radiance_hdr(std::string_view bytes) {
    if (bytes.size() > std::size_t(256) << 20)
        return std::unexpected(Error{ErrorCode::invalid_storage, "Radiance HDR source exceeds 256 MiB."});
    std::size_t at = 0;
    const auto line = [&]() -> Result<std::string_view> {
        const auto end = bytes.find('\n', at);
        if (end == std::string_view::npos)
            return std::unexpected(Error{ErrorCode::invalid_storage, "Truncated Radiance HDR header."});
        auto text = bytes.substr(at, end - at);
        at = end + 1;
        if (!text.empty() && text.back() == '\r')
            text.remove_suffix(1);
        return text;
    };
    ATELIER_TRY_BIND(magic, line());
    if (*magic != "#?RADIANCE" && *magic != "#?RGBE")
        return std::unexpected(Error{ErrorCode::invalid_storage, "Not a Radiance HDR file."});
    for (;;) {
        ATELIER_TRY_BIND(entry, line());
        if (entry->empty())
            break;
        if (entry->starts_with("FORMAT=") && *entry != "FORMAT=32-bit_rle_rgbe")
            return std::unexpected(
                Error{ErrorCode::invalid_storage, "Radiance HDR must use the 32-bit_rle_rgbe format."});
    }
    ATELIER_TRY_BIND(resolution, line());
    std::uint32_t width{};
    std::uint32_t height{};
    {
        const char* cursor = resolution->data();
        const char* const end = cursor + resolution->size();
        const auto token = [&](std::string_view expected) -> Result<void> {
            if (!std::string_view(cursor, end).starts_with(expected))
                return std::unexpected(
                    Error{ErrorCode::invalid_storage, "Radiance HDR orientation must be -Y H +X W."});
            cursor += expected.size();
            return {};
        };
        const auto number = [&](std::uint32_t& value) -> Result<void> {
            const auto [next, error] = std::from_chars(cursor, end, value);
            if (error != std::errc{} || next == cursor)
                return std::unexpected(Error{ErrorCode::invalid_storage, "Invalid Radiance HDR resolution."});
            cursor = next;
            return {};
        };
        ATELIER_TRY(token("-Y "));
        ATELIER_TRY(number(height));
        ATELIER_TRY(token(" +X "));
        ATELIER_TRY(number(width));
        if (cursor != end)
            return std::unexpected(Error{ErrorCode::invalid_storage, "Invalid Radiance HDR resolution."});
    }
    if (!width || !height || width > SkyMap::source_width_limit || height > SkyMap::source_width_limit / 2)
        return std::unexpected(Error{ErrorCode::invalid_storage, "Radiance HDR exceeds 8192x4096 pixels."});

    const auto byte = [&]() -> Result<std::uint8_t> {
        if (at >= bytes.size())
            return std::unexpected(Error{ErrorCode::invalid_storage, "Truncated Radiance HDR scanline data."});
        return static_cast<std::uint8_t>(bytes[at++]);
    };
    std::vector<float> rgb(std::size_t(width) * height * 3);
    std::vector<std::uint8_t> scanline(std::size_t(width) * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        // New-style scanlines start with 2, 2 and the width; anything else is flat or old RLE.
        if (width >= 8 && width < 32768 && bytes.size() - at >= 4 && bytes[at] == 2 && bytes[at + 1] == 2 &&
            !(bytes[at + 2] & 0x80)) {
            at += 2;
            ATELIER_TRY_BIND(high, byte());
            ATELIER_TRY_BIND(low, byte());
            if (std::uint32_t(*high << 8 | *low) != width)
                return std::unexpected(
                    Error{ErrorCode::invalid_storage, "Radiance HDR scanline width does not match the header."});
            for (std::size_t channel = 0; channel < 4; ++channel)
                for (std::size_t x = 0; x < width;) {
                    ATELIER_TRY_BIND(count, byte());
                    if (*count > 128) {
                        if (std::size_t(*count - 128) > width - x)
                            return std::unexpected(
                                Error{ErrorCode::invalid_storage, "Radiance HDR run exceeds its scanline."});
                        ATELIER_TRY_BIND(value, byte());
                        for (std::uint32_t i = 128; i < *count; ++i)
                            scanline[x++ * 4 + channel] = *value;
                    } else {
                        if (!*count || *count > width - x)
                            return std::unexpected(
                                Error{ErrorCode::invalid_storage, "Radiance HDR run exceeds its scanline."});
                        for (std::uint32_t i = 0; i < *count; ++i) {
                            ATELIER_TRY_BIND(value, byte());
                            scanline[x++ * 4 + channel] = *value;
                        }
                    }
                }
        } else {
            unsigned shift = 0;
            for (std::size_t x = 0; x < width;) {
                std::array<std::uint8_t, 4> pixel{};
                for (auto& component : pixel) {
                    ATELIER_TRY_BIND(value, byte());
                    component = *value;
                }
                if (pixel[0] == 1 && pixel[1] == 1 && pixel[2] == 1) {
                    if (!x || shift >= 32)
                        return std::unexpected(Error{ErrorCode::invalid_storage, "Invalid Radiance HDR repeat run."});
                    const std::uint64_t run = std::uint64_t(pixel[3]) << shift;
                    if (run > width - x)
                        return std::unexpected(
                            Error{ErrorCode::invalid_storage, "Radiance HDR run exceeds its scanline."});
                    for (std::uint64_t i = 0; i < run; ++i, ++x)
                        std::copy_n(&scanline[(x - 1) * 4], 4, &scanline[x * 4]);
                    shift += 8;
                } else {
                    std::copy(pixel.begin(), pixel.end(), &scanline[x * 4]);
                    ++x;
                    shift = 0;
                }
            }
        }
        for (std::size_t x = 0; x < width; ++x) {
            const std::uint8_t exponent = scanline[x * 4 + 3];
            const float scale = exponent ? std::ldexp(1.f, int(exponent) - 136) : 0.f;
            for (std::size_t c = 0; c < 3; ++c)
                rgb[(std::size_t(y) * width + x) * 3 + c] = scanline[x * 4 + c] * scale;
        }
    }
    auto map = SkyMap::create(width, height, rgb);
    if (!map)
        return std::unexpected(Error{ErrorCode::invalid_storage, map.error().message});
    return std::move(*map);
}

} // namespace atelier::rendering
