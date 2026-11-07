#include "rendering/sky_map.hpp"
#include "rendering/scene_lighting.hpp"
#include "foundation/result.hpp"
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <numbers>

namespace atelier::rendering {
namespace {
// Round to nearest even for finite, non-negative values at most 65504; -0 stores +0.
std::uint16_t half_bits(float value) noexcept {
    std::uint32_t bits = std::bit_cast<std::uint32_t>(value) & 0x7fffffffu;
    if (bits < 0x38800000u) // FP16 subnormal or zero: the 0.5f bias aligns the mantissa.
        return static_cast<std::uint16_t>(std::bit_cast<std::uint32_t>(std::bit_cast<float>(bits) + .5f) - 0x3f000000u);
    bits += 0xc8000fffu + ((bits >> 13) & 1);
    return static_cast<std::uint16_t>(bits >> 13);
}

// D3D face selection, including its Z, Y, X precedence on ties.
std::size_t cube_texel(double x, double y, double z) noexcept {
    const double ax = std::abs(x);
    const double ay = std::abs(y);
    const double az = std::abs(z);
    std::size_t face;
    double sc;
    double tc;
    double major;
    if (ax > ay && ax > az) {
        face = x >= 0 ? 0 : 1;
        major = ax;
        sc = x >= 0 ? -z : z;
        tc = -y;
    } else if (ay > az) {
        face = y >= 0 ? 2 : 3;
        major = ay;
        sc = x;
        tc = y >= 0 ? z : -z;
    } else {
        face = z >= 0 ? 4 : 5;
        major = az;
        sc = z >= 0 ? x : -x;
        tc = -y;
    }
    const auto index = [major](double c) {
        return std::min<std::size_t>(SkyMap::cube_size - 1,
                                     static_cast<std::size_t>((c / major + 1) * .5 * SkyMap::cube_size));
    };
    return (face * SkyMap::cube_size + index(tc)) * SkyMap::cube_size + index(sc);
}
}

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

    std::array<double, 3> sum{};
    double total = 0;
    for (std::uint32_t y = 0; y < height; ++y) {
        const double solid_angle =
            2 * pi / width * (std::sin(pi * (.5 - double(y) / height)) - std::sin(pi * (.5 - double(y + 1) / height)));
        std::array<double, 3> row{};
        for (std::uint32_t x = 0; x < width; ++x) {
            const float* texel = &rgb[(std::size_t(y) * width + x) * 3];
            for (std::size_t c = 0; c < 3; ++c)
                row[c] += texel[c];
        }
        for (std::size_t c = 0; c < 3; ++c)
            sum[c] += row[c] * solid_angle;
        total += solid_angle * width;
    }
    for (std::size_t c = 0; c < 3; ++c)
        mean_[c] = static_cast<float>(sum[c] / total);

    // Splat sub-texels of at most 0.5 degrees so every cube texel (at least ~16 degrees)
    // receives many samples. Normalizing by the sampled area reproduces constant maps
    // exactly; a texel's sampled area differs from its solid angle by <1%.
    const std::uint32_t split = std::max<std::uint32_t>(1, (720 + width - 1) / width);
    const std::uint32_t columns = width * split;
    const std::uint32_t rows = height * split;
    std::vector<double> splat(std::size_t(6) * cube_size * cube_size * 4);
    std::vector<double> column_sin(columns);
    std::vector<double> column_cos(columns);
    for (std::uint32_t x = 0; x < columns; ++x) {
        const double longitude = 2 * pi * ((x + .5) / columns - .5);
        column_sin[x] = std::sin(longitude);
        column_cos[x] = std::cos(longitude);
    }
    for (std::uint32_t y = 0; y < rows; ++y) {
        const double latitude = pi * (.5 - (y + .5) / rows);
        const double up = std::sin(latitude);
        const double across = std::cos(latitude);
        const double solid_angle =
            2 * pi / columns * (std::sin(pi * (.5 - double(y) / rows)) - std::sin(pi * (.5 - double(y + 1) / rows)));
        const float* source = &rgb[std::size_t(y / split) * width * 3];
        for (std::uint32_t x = 0; x < columns; ++x) {
            const float* texel = source + std::size_t(x / split) * 3;
            double* target = &splat[cube_texel(column_sin[x] * across, up, column_cos[x] * across) * 4];
            for (std::size_t c = 0; c < 3; ++c)
                target[c] += texel[c] * solid_angle;
            target[3] += solid_angle;
        }
    }
    cube_.resize(splat.size());
    for (std::size_t i = 0; i < splat.size(); i += 4) {
        for (std::size_t c = 0; c < 3; ++c)
            cube_[i + c] = static_cast<float>(splat[i + c] / splat[i + 3]);
        cube_[i + 3] = 1;
    }

    std::vector<float> halved;
    std::span<const float> stored = rgb;
    if (width > width_limit) {
        width_ = width / 2;
        height_ = height / 2;
        halved.resize(std::size_t(width_) * height_ * 3);
        for (std::uint32_t y = 0; y < height_; ++y)
            for (std::uint32_t x = 0; x < width_; ++x)
                for (std::size_t c = 0; c < 3; ++c) {
                    const auto at = [&](std::uint32_t row, std::uint32_t column) {
                        return rgb[(std::size_t(row) * width + column) * 3 + c];
                    };
                    halved[(std::size_t(y) * width_ + x) * 3 + c] =
                        .25f *
                        ((at(2 * y, 2 * x) + at(2 * y, 2 * x + 1)) + (at(2 * y + 1, 2 * x) + at(2 * y + 1, 2 * x + 1)));
                }
        stored = halved;
    } else {
        width_ = width;
        height_ = height;
    }
    const float stored_peak = *std::max_element(stored.begin(), stored.end());
    while (std::ldexp(stored_peak, -scale_exponent_) > 65504.f)
        ++scale_exponent_;
    pixels_.resize(std::size_t(width_) * height_ * 4);
    for (std::size_t i = 0, texels = std::size_t(width_) * height_; i < texels; ++i) {
        for (std::size_t c = 0; c < 3; ++c)
            pixels_[i * 4 + c] = half_bits(std::ldexp(stored[i * 3 + c], -scale_exponent_));
        pixels_[i * 4 + 3] = 0x3c00;
    }
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
