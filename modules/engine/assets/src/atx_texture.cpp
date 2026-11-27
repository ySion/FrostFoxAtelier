#include "assets/atx_texture.hpp"
#include "foundation/sha256.hpp"
#include "memory/fatal.hpp"

#include <bc7enc.h>
#include <rgbcx.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>

namespace atelier::assets {
namespace {

constexpr std::uint32_t atx_magic = 0x58455441u; // 'ATEX'

constexpr std::uint32_t atx_version = 4u;

std::atomic<std::uint64_t> g_tmp_sequence{0u};

void ensure_codecs_initialized() noexcept {
    static std::once_flag once;
    std::call_once(once, [] {
        rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal);
        bc7enc_compress_block_init();
    });
}

constexpr std::uint64_t align_up_u64(std::uint64_t val, std::uint64_t align) noexcept {
    return (val + align - 1u) & ~(align - 1u);
}

constexpr std::uint32_t align_up_u32(std::uint32_t val, std::uint32_t align) noexcept {
    return (val + align - 1u) & ~(align - 1u);
}

std::uint32_t expected_mip_count(std::uint32_t w, std::uint32_t h) noexcept {
    std::uint32_t count = 1u;
    for (std::uint32_t d = std::max(w, h); d > 1u; d >>= 1u)
        ++count;
    return count;
}

float srgb_u8_to_linear(std::uint8_t c) noexcept {
    const float s = static_cast<float>(c) * (1.0f / 255.0f);
    return (s <= 0.04045f) ? (s * (1.0f / 12.92f)) : std::pow((s + 0.055f) * (1.0f / 1.055f), 2.4f);
}

std::uint8_t linear_to_srgb_u8(float l) noexcept {
    const float c = std::clamp(l, 0.0f, 1.0f);
    const float s = (c <= 0.0031308f) ? (c * 12.92f) : (1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f);
    return static_cast<std::uint8_t>(std::clamp(std::lrint(s * 255.0f), 0l, 255l));
}

std::uint32_t block_byte_size(AtxFormat fmt) noexcept {
    switch (fmt) {
    case AtxFormat::bc1_rgba_unorm:
    case AtxFormat::bc1_rgba_srgb:
    case AtxFormat::bc4_r_unorm:
        return 8u;
    case AtxFormat::bc5_rg_unorm:
    case AtxFormat::bc6h_uf16:
    case AtxFormat::bc7_rgba_unorm:
    case AtxFormat::bc7_rgba_srgb:
        return 16u;
    case AtxFormat::rgba16_sfloat:
        return 8u;
    case AtxFormat::rgba8_unorm:
    case AtxFormat::rgba8_srgb:
        return 4u;
    }
    return 4u;
}

AtxFormat expected_atx_format(AtxRole role, std::uint32_t width, std::uint32_t height) noexcept {
    const bool use_bcn = (width >= 4u && height >= 4u && (width & 3u) == 0u && (height & 3u) == 0u);
    if (!use_bcn) {
        if (role == AtxRole::hdr_sky_bc6h)
            return AtxFormat::rgba16_sfloat;
        return (role == AtxRole::color_srgb || role == AtxRole::color_opaque_bc1) ? AtxFormat::rgba8_srgb
                                                                                   : AtxFormat::rgba8_unorm;
    }
    switch (role) {
    case AtxRole::color_srgb:
        return AtxFormat::bc7_rgba_srgb;
    case AtxRole::linear_rgba:
        return AtxFormat::bc7_rgba_unorm;
    case AtxRole::scalar_bc4:
        return AtxFormat::bc4_r_unorm;
    case AtxRole::normal_bc5:
        return AtxFormat::bc5_rg_unorm;
    case AtxRole::color_opaque_bc1:
        return AtxFormat::bc1_rgba_srgb;
    case AtxRole::hdr_sky_bc6h:
        return AtxFormat::bc6h_uf16;
    }
    return AtxFormat::bc7_rgba_unorm;
}

// DXGI_FORMAT_BC6H_UF16 Mode 11 (mode bits `00011` = 0x03):
// 1-subset, unpartitioned 10.10.10 + 10.10.10 unsigned FP16 endpoints + 4-bit symmetric interpolation weights.
std::uint32_t bc6h_unquantize_10(std::uint32_t q) noexcept {
    if (q == 0u)
        return 0u;
    if (q >= 1023u)
        return 0xFFFFu;
    return ((q << 15u) + 0x4000u) >> 9u;
}

std::uint32_t bc6h_quantize_10(std::uint16_t h) noexcept {
    const std::uint32_t clamped = std::min<std::uint32_t>(h & 0x7FFFu, 0x7BFFu);
    if (clamped == 0u)
        return 0u;
    if (clamped >= 0x7BFFu)
        return 1023u;
    return std::clamp<std::uint32_t>((clamped + 15u) / 31u, 1u, 1022u);
}

void encode_bc6h_uf16_block(std::uint8_t out_block[16], const std::uint16_t block_rgba16[16 * 4]) noexcept {
    static constexpr std::uint32_t k_weights4[16] = {0u,  4u,  9u,  13u, 17u, 21u, 26u, 30u,
                                                     34u, 38u, 43u, 47u, 51u, 55u, 60u, 64u};
    std::uint16_t lo[3] = {0x7BFFu, 0x7BFFu, 0x7BFFu};
    std::uint16_t hi[3] = {0u, 0u, 0u};
    for (std::size_t i = 0; i < 16u; ++i) {
        for (std::size_t c = 0; c < 3u; ++c) {
            const std::uint16_t v = std::min<std::uint16_t>(block_rgba16[i * 4u + c] & 0x7FFFu, 0x7BFFu);
            lo[c] = std::min(lo[c], v);
            hi[c] = std::max(hi[c], v);
        }
    }

    std::uint32_t q0[3] = {bc6h_quantize_10(lo[0]), bc6h_quantize_10(lo[1]), bc6h_quantize_10(lo[2])};
    std::uint32_t q1[3] = {bc6h_quantize_10(hi[0]), bc6h_quantize_10(hi[1]), bc6h_quantize_10(hi[2])};
    std::uint32_t e0[3] = {bc6h_unquantize_10(q0[0]), bc6h_unquantize_10(q0[1]), bc6h_unquantize_10(q0[2])};
    std::uint32_t e1[3] = {bc6h_unquantize_10(q1[0]), bc6h_unquantize_10(q1[1]), bc6h_unquantize_10(q1[2])};

    std::uint8_t indices[16]{};
    for (std::size_t i = 0; i < 16u; ++i) {
        const std::uint32_t t0 = std::min<std::uint32_t>(block_rgba16[i * 4u + 0u] & 0x7FFFu, 0x7BFFu);
        const std::uint32_t t1 = std::min<std::uint32_t>(block_rgba16[i * 4u + 1u] & 0x7FFFu, 0x7BFFu);
        const std::uint32_t t2 = std::min<std::uint32_t>(block_rgba16[i * 4u + 2u] & 0x7FFFu, 0x7BFFu);
        std::uint64_t best_err = ~0ull;
        std::uint8_t best_idx = 0u;
        for (std::uint32_t k = 0u; k < 16u; ++k) {
            const std::uint32_t w = k_weights4[k];
            const std::uint32_t u0 = ((e0[0] * (64u - w) + e1[0] * w + 32u) >> 6u);
            const std::uint32_t u1 = ((e0[1] * (64u - w) + e1[1] * w + 32u) >> 6u);
            const std::uint32_t u2 = ((e0[2] * (64u - w) + e1[2] * w + 32u) >> 6u);
            const std::int64_t d0 = static_cast<std::int64_t>((u0 * 31u) >> 6u) - static_cast<std::int64_t>(t0);
            const std::int64_t d1 = static_cast<std::int64_t>((u1 * 31u) >> 6u) - static_cast<std::int64_t>(t1);
            const std::int64_t d2 = static_cast<std::int64_t>((u2 * 31u) >> 6u) - static_cast<std::int64_t>(t2);
            const auto err = static_cast<std::uint64_t>(d0 * d0 + d1 * d1 + d2 * d2);
            if (err < best_err) {
                best_err = err;
                best_idx = static_cast<std::uint8_t>(k);
            }
        }
        indices[i] = best_idx;
    }

    // Enforce BC6H 1-subset anchor rule: texel 0 MSB must be 0 (indices[0] < 8).
    if (indices[0] >= 8u) {
        std::swap(q0, q1);
        for (auto& idx : indices)
            idx = static_cast<std::uint8_t>(15u - idx);
    }

    std::uint64_t lo_bits = 0u;
    std::uint64_t hi_bits = 0u;
    std::uint32_t bit_pos = 0u;
    const auto put_bits = [&](std::uint32_t val, std::uint32_t count) noexcept {
        const std::uint64_t masked = static_cast<std::uint64_t>(val) & ((1ull << count) - 1ull);
        if (bit_pos < 64u) {
            lo_bits |= (masked << bit_pos);
            if (bit_pos + count > 64u)
                hi_bits |= (masked >> (64u - bit_pos));
        } else {
            hi_bits |= (masked << (bit_pos - 64u));
        }
        bit_pos += count;
    };

    put_bits(0x03u, 5u); // Mode 11 (`00011`)
    put_bits(q0[0], 10u);
    put_bits(q0[1], 10u);
    put_bits(q0[2], 10u);
    put_bits(q1[0], 10u);
    put_bits(q1[1], 10u);
    put_bits(q1[2], 10u);
    put_bits(indices[0], 3u); // Anchor texel 0 (implicit MSB = 0)
    for (std::size_t i = 1u; i < 16u; ++i)
        put_bits(indices[i], 4u);

    std::memcpy(out_block, &lo_bits, 8u);
    std::memcpy(out_block + 8u, &hi_bits, 8u);
}

template <class T>
void append_pod(std::vector<std::uint8_t>& buf, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* p = reinterpret_cast<const std::uint8_t*>(&value);
    buf.insert(buf.end(), p, p + sizeof(T));
}

template <class T>
bool read_pod(std::span<const std::uint8_t>& cur, T& out) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    if (cur.size() < sizeof(T))
        return false;
    std::memcpy(&out, cur.data(), sizeof(T));
    cur = cur.subspan(sizeof(T));
    return true;
}

std::string compute_image_cache_digest(const material::Image& image, AtxRole role) {
    const std::uint32_t header[4] = {atx_version, image.width(), image.height(), static_cast<std::uint32_t>(role)};
    const std::string h_dig = foundation::sha256(std::as_bytes(std::span{header}));
    const std::string combined = h_dig + std::string(image.rgba_sha256());
    return foundation::sha256(std::as_bytes(std::span{combined.data(), combined.size()}));
}

std::string compute_sky_cache_digest(const rendering::SkyMap& sky) {
    const std::uint32_t header[4] = {atx_version, sky.width(), sky.height(),
                                     static_cast<std::uint32_t>(AtxRole::hdr_sky_bc6h)};
    const std::string h_dig = foundation::sha256(std::as_bytes(std::span{header}));
    const std::string combined = h_dig + std::string(sky.content_sha256());
    return foundation::sha256(std::as_bytes(std::span{combined.data(), combined.size()}));
}

void write_cache_file_atomic(const std::filesystem::path& cache_dir, const std::filesystem::path& file,
                             const AtxTexture& cooked) {
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    if (ec)
        return;
    const auto encoded = encode_atx(cooked);
    const auto pid = static_cast<unsigned long>(GetCurrentProcessId());
    const auto seq = ++g_tmp_sequence;
    const std::filesystem::path tmp =
        file.wstring() + L"." + std::to_wstring(pid) + L"." + std::to_wstring(seq) + L".tmp";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    const bool wrote = static_cast<bool>(
        out.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size())));
    out.close();
    if (wrote) {
        std::filesystem::rename(tmp, file, ec);
        if (ec)
            std::filesystem::remove(tmp, ec);
    } else {
        std::filesystem::remove(tmp, ec);
    }
}

AtxTextureRef try_read_cache_file(const std::filesystem::path& file, std::uint32_t w, std::uint32_t h, AtxRole role) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in.good())
        return nullptr;
    const auto sz = in.tellg();
    if (sz <= 0)
        return nullptr;
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(sz));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(raw.data()), sz))
        return nullptr;
    if (auto decoded = decode_atx(raw);
        decoded && (*decoded)->width == w && (*decoded)->height == h && (*decoded)->role == role) {
        return std::move(*decoded);
    }
    return nullptr;
}

} // namespace

bool is_block_compressed(AtxFormat format) noexcept {
    return format >= AtxFormat::bc1_rgba_unorm;
}

float compute_atx_toksvig_slope(const AtxTexture& tex) noexcept {
    if (tex.role != AtxRole::normal_bc5 || tex.toksvig_mip_lengths.size() < 2u)
        return 0.0f;
    const std::size_t m_max = std::min<std::size_t>(4u, tex.toksvig_mip_lengths.size() - 1u);
    float contracted = 1.0f;
    for (std::size_t m = 1u; m <= m_max; ++m)
        contracted *= std::clamp(tex.toksvig_mip_lengths[m], 0.0f, 1.0f);
    return std::clamp((1.0f - contracted) * (4.0f / static_cast<float>(m_max)), 0.0f, 1.0f);
}

AtxTextureRef cook_atx_image(const material::Image& image, AtxRole role) {
    ensure_codecs_initialized();

    auto out = std::make_shared<AtxTexture>();
    out->role = role;
    out->width = image.width();
    out->height = image.height();
    out->format = expected_atx_format(role, out->width, out->height);

    const std::uint32_t mip_count = expected_mip_count(out->width, out->height);
    const auto pinned_mip0 = image.acquire_pixels();
    if (!pinned_mip0 || pinned_mip0->size() != image.byte_size())
        return nullptr;
    const std::span<const std::uint8_t> mip0_pixels{*pinned_mip0};

    std::vector<std::vector<std::uint8_t>> upper_mips(mip_count > 1u ? (mip_count - 1u) : 0u);
    out->toksvig_mip_lengths.assign(mip_count, 1.0f);

    std::vector<math::Vector3> prev_normals;
    if (role == AtxRole::normal_bc5 && mip_count > 1u) {
        prev_normals.resize(std::size_t(out->width) * out->height);
        for (std::size_t i = 0; i < prev_normals.size(); ++i) {
            const float nx = static_cast<float>(mip0_pixels[i * 4u + 0u]) * (2.0f / 255.0f) - 1.0f;
            const float ny = static_cast<float>(mip0_pixels[i * 4u + 1u]) * (2.0f / 255.0f) - 1.0f;
            const float nz = static_cast<float>(mip0_pixels[i * 4u + 2u]) * (2.0f / 255.0f) - 1.0f;
            const float len = std::sqrt(std::max(nx * nx + ny * ny + nz * nz, 1e-12f));
            prev_normals[i] = {nx / len, ny / len, nz / len};
        }
    }

    std::uint32_t prev_w = out->width;
    std::uint32_t prev_h = out->height;
    for (std::uint32_t m = 1u; m < mip_count; ++m) {
        const std::uint32_t mw = std::max(1u, prev_w >> 1u);
        const std::uint32_t mh = std::max(1u, prev_h >> 1u);
        auto& dst = upper_mips[m - 1u];
        dst.resize(std::size_t(mw) * mh * 4u);

        if (role == AtxRole::normal_bc5) {
            std::vector<math::Vector3> cur_normals(std::size_t(mw) * mh);
            double sum_len = 0.0;
            for (std::uint32_t y = 0u; y < mh; ++y) {
                for (std::uint32_t x = 0u; x < mw; ++x) {
                    const std::uint32_t x0 = std::min(x * 2u, prev_w - 1u);
                    const std::uint32_t x1 = std::min(x * 2u + 1u, prev_w - 1u);
                    const std::uint32_t y0 = std::min(y * 2u, prev_h - 1u);
                    const std::uint32_t y1 = std::min(y * 2u + 1u, prev_h - 1u);
                    const math::Vector3 avg =
                        (prev_normals[std::size_t(y0) * prev_w + x0] + prev_normals[std::size_t(y0) * prev_w + x1] +
                         prev_normals[std::size_t(y1) * prev_w + x0] + prev_normals[std::size_t(y1) * prev_w + x1]) *
                        0.25f;
                    const float len = std::sqrt(std::max(math::dot(avg, avg), 1e-12f));
                    sum_len += std::clamp(len, 0.0f, 1.0f);
                    const math::Vector3 unit = avg * (1.0f / len);
                    cur_normals[std::size_t(y) * mw + x] = unit;
                    const std::size_t idx = (std::size_t(y) * mw + x) * 4u;
                    dst[idx + 0u] =
                        static_cast<std::uint8_t>(std::clamp(std::lrint((unit.x * 0.5f + 0.5f) * 255.0f), 0l, 255l));
                    dst[idx + 1u] =
                        static_cast<std::uint8_t>(std::clamp(std::lrint((unit.y * 0.5f + 0.5f) * 255.0f), 0l, 255l));
                    dst[idx + 2u] =
                        static_cast<std::uint8_t>(std::clamp(std::lrint((unit.z * 0.5f + 0.5f) * 255.0f), 0l, 255l));
                    dst[idx + 3u] = 255u;
                }
            }
            out->toksvig_mip_lengths[m] = static_cast<float>(sum_len / double(std::size_t(mw) * mh));
            prev_normals = std::move(cur_normals);
        } else {
            const std::span<const std::uint8_t> prev =
                (m == 1u) ? mip0_pixels : std::span<const std::uint8_t>{upper_mips[m - 2u]};
            const bool is_srgb = (role == AtxRole::color_srgb || role == AtxRole::color_opaque_bc1);
            const bool alpha_weight_rgb = is_srgb && image.has_alpha();
            for (std::uint32_t y = 0u; y < mh; ++y) {
                for (std::uint32_t x = 0u; x < mw; ++x) {
                    const std::uint32_t x0 = std::min(x * 2u, prev_w - 1u);
                    const std::uint32_t x1 = std::min(x * 2u + 1u, prev_w - 1u);
                    const std::uint32_t y0 = std::min(y * 2u, prev_h - 1u);
                    const std::uint32_t y1 = std::min(y * 2u + 1u, prev_h - 1u);
                    const std::size_t o00 = (std::size_t(y0) * prev_w + x0) * 4u;
                    const std::size_t o10 = (std::size_t(y0) * prev_w + x1) * 4u;
                    const std::size_t o01 = (std::size_t(y1) * prev_w + x0) * 4u;
                    const std::size_t o11 = (std::size_t(y1) * prev_w + x1) * 4u;
                    const std::size_t dst_o = (std::size_t(y) * mw + x) * 4u;

                    const float w00 = alpha_weight_rgb ? (float(prev[o00 + 3u]) * (1.0f / 255.0f)) : 1.0f;
                    const float w10 = alpha_weight_rgb ? (float(prev[o10 + 3u]) * (1.0f / 255.0f)) : 1.0f;
                    const float w01 = alpha_weight_rgb ? (float(prev[o01 + 3u]) * (1.0f / 255.0f)) : 1.0f;
                    const float w11 = alpha_weight_rgb ? (float(prev[o11 + 3u]) * (1.0f / 255.0f)) : 1.0f;
                    const float w_sum = w00 + w10 + w01 + w11;

                    for (std::uint32_t c = 0u; c < 4u; ++c) {
                        const std::uint8_t p00 = prev[o00 + c];
                        const std::uint8_t p10 = prev[o10 + c];
                        const std::uint8_t p01 = prev[o01 + c];
                        const std::uint8_t p11 = prev[o11 + c];
                        if (is_srgb && c < 3u) {
                            const float l00 = srgb_u8_to_linear(p00);
                            const float l10 = srgb_u8_to_linear(p10);
                            const float l01 = srgb_u8_to_linear(p01);
                            const float l11 = srgb_u8_to_linear(p11);
                            const float avg = (w_sum > 1e-5f)
                                                  ? ((l00 * w00 + l10 * w10 + l01 * w01 + l11 * w11) / w_sum)
                                                  : (0.25f * (l00 + l10 + l01 + l11));
                            dst[dst_o + c] = linear_to_srgb_u8(avg);
                        } else {
                            dst[dst_o + c] = static_cast<std::uint8_t>((unsigned(p00) + p10 + p01 + p11 + 2u) >> 2u);
                        }
                    }
                }
            }
        }
        prev_w = mw;
        prev_h = mh;
    }

    out->mips.resize(mip_count);
    std::uint64_t total_bytes = 0u;
    const bool bcn = is_block_compressed(out->format);
    const std::uint32_t blk_bytes = block_byte_size(out->format);

    for (std::uint32_t m = 0u; m < mip_count; ++m) {
        const std::uint32_t mw = std::max(1u, out->width >> m);
        const std::uint32_t mh = std::max(1u, out->height >> m);
        const std::uint32_t blocks_x = bcn ? ((mw + 3u) >> 2u) : mw;
        const std::uint32_t blocks_y = bcn ? ((mh + 3u) >> 2u) : mh;
        const std::uint32_t pitch = align_up_u32(blocks_x * blk_bytes, 256u);
        const std::uint32_t slice = pitch * blocks_y;
        total_bytes = align_up_u64(total_bytes, 512u);
        out->mips[m] = {total_bytes, pitch, slice, mw, mh};
        total_bytes += slice;
    }

    out->payload.assign(static_cast<std::size_t>(total_bytes), 0u);

    bc7enc_compress_block_params bc7_params{};
    bc7enc_compress_block_params_init(&bc7_params);
    bc7enc_compress_block_params_init_linear_weights(&bc7_params);
    bc7_params.m_max_partitions = 16u;
    bc7_params.m_uber_level = 1u;

    for (std::uint32_t m = 0u; m < mip_count; ++m) {
        const AtxMipSlice& mip = out->mips[m];
        const std::span<const std::uint8_t> src =
            (m == 0u) ? mip0_pixels : std::span<const std::uint8_t>{upper_mips[m - 1u]};
        std::uint8_t* mip_dst = out->payload.data() + mip.byte_offset;

        if (!bcn) {
            for (std::uint32_t y = 0u; y < mip.height; ++y) {
                std::memcpy(mip_dst + std::size_t(y) * mip.row_pitch, src.data() + std::size_t(y) * mip.width * 4u,
                            std::size_t(mip.width) * 4u);
            }
            continue;
        }

        const std::uint32_t bx_count = (mip.width + 3u) >> 2u;
        const std::uint32_t by_count = (mip.height + 3u) >> 2u;
        for (std::uint32_t by = 0u; by < by_count; ++by) {
            std::uint8_t* row_dst = mip_dst + std::size_t(by) * mip.row_pitch;
            for (std::uint32_t bx = 0u; bx < bx_count; ++bx) {
                std::uint8_t block_rgba[16 * 4]{};
                for (std::uint32_t py = 0u; py < 4u; ++py) {
                    const std::uint32_t sy = std::min(by * 4u + py, mip.height - 1u);
                    for (std::uint32_t px = 0u; px < 4u; ++px) {
                        const std::uint32_t sx = std::min(bx * 4u + px, mip.width - 1u);
                        std::memcpy(block_rgba + (py * 4u + px) * 4u,
                                    src.data() + (std::size_t(sy) * mip.width + sx) * 4u, 4u);
                    }
                }
                std::uint8_t* blk_out = row_dst + std::size_t(bx) * blk_bytes;
                switch (out->format) {
                case AtxFormat::bc1_rgba_unorm:
                case AtxFormat::bc1_rgba_srgb:
                    rgbcx::encode_bc1(10u, blk_out, block_rgba, false, false);
                    break;
                case AtxFormat::bc4_r_unorm:
                    rgbcx::encode_bc4(blk_out, block_rgba, 4);
                    break;
                case AtxFormat::bc5_rg_unorm:
                    rgbcx::encode_bc5(blk_out, block_rgba, 0, 1, 4);
                    break;
                case AtxFormat::bc7_rgba_unorm:
                case AtxFormat::bc7_rgba_srgb:
                    bc7enc_compress_block(blk_out, block_rgba, &bc7_params);
                    break;
                case AtxFormat::rgba8_unorm:
                case AtxFormat::rgba8_srgb:
                case AtxFormat::rgba16_sfloat:
                case AtxFormat::bc6h_uf16:
                    atelier::fatal("Unexpected format inside LDR BCn block loop");
                }
            }
        }
    }

    return out;
}

AtxTextureRef cook_atx_sky_map(const rendering::SkyMap& sky) {
    auto out = std::make_shared<AtxTexture>();
    out->role = AtxRole::hdr_sky_bc6h;
    out->width = sky.width();
    out->height = sky.height();
    out->format = expected_atx_format(AtxRole::hdr_sky_bc6h, out->width, out->height);

    const auto chain = sky.mip_chain();
    const auto mip_count = static_cast<std::uint32_t>(chain.size());
    out->mips.resize(mip_count);
    out->toksvig_mip_lengths.assign(mip_count, 1.0f);

    const bool bcn = is_block_compressed(out->format);
    const std::uint32_t blk_bytes = block_byte_size(out->format);
    std::uint64_t total_bytes = 0u;
    for (std::uint32_t m = 0u; m < mip_count; ++m) {
        const std::uint32_t mw = chain[m].width;
        const std::uint32_t mh = chain[m].height;
        const std::uint32_t blocks_x = bcn ? ((mw + 3u) >> 2u) : mw;
        const std::uint32_t blocks_y = bcn ? ((mh + 3u) >> 2u) : mh;
        const std::uint32_t pitch = align_up_u32(blocks_x * blk_bytes, 256u);
        const std::uint32_t slice = pitch * blocks_y;
        total_bytes = align_up_u64(total_bytes, 512u);
        out->mips[m] = {total_bytes, pitch, slice, mw, mh};
        total_bytes += slice;
    }

    out->payload.assign(static_cast<std::size_t>(total_bytes), 0u);
    for (std::uint32_t m = 0u; m < mip_count; ++m) {
        const AtxMipSlice& mip = out->mips[m];
        const auto& src_fp16 = chain[m].pixels;
        std::uint8_t* mip_dst = out->payload.data() + mip.byte_offset;

        if (!bcn) {
            for (std::uint32_t y = 0u; y < mip.height; ++y) {
                std::memcpy(mip_dst + std::size_t(y) * mip.row_pitch,
                            src_fp16.data() + std::size_t(y) * mip.width * 4u, std::size_t(mip.width) * 8u);
            }
            continue;
        }

        const std::uint32_t bx_count = (mip.width + 3u) >> 2u;
        const std::uint32_t by_count = (mip.height + 3u) >> 2u;
        for (std::uint32_t by = 0u; by < by_count; ++by) {
            std::uint8_t* row_dst = mip_dst + std::size_t(by) * mip.row_pitch;
            for (std::uint32_t bx = 0u; bx < bx_count; ++bx) {
                std::uint16_t block_rgba16[16 * 4]{};
                for (std::uint32_t py = 0u; py < 4u; ++py) {
                    const std::uint32_t sy = std::min(by * 4u + py, mip.height - 1u);
                    for (std::uint32_t px = 0u; px < 4u; ++px) {
                        const std::uint32_t sx = std::min(bx * 4u + px, mip.width - 1u);
                        std::memcpy(block_rgba16 + (py * 4u + px) * 4u,
                                    src_fp16.data() + (std::size_t(sy) * mip.width + sx) * 4u, 8u);
                    }
                }
                encode_bc6h_uf16_block(row_dst + std::size_t(bx) * blk_bytes, block_rgba16);
            }
        }
    }
    return out;
}

std::vector<std::uint8_t> encode_atx(const AtxTexture& tex) {
    std::vector<std::uint8_t> out;
    out.reserve(512u + tex.payload.size());
    append_pod(out, atx_magic);
    append_pod(out, atx_version);
    append_pod(out, static_cast<std::uint32_t>(tex.format));
    append_pod(out, static_cast<std::uint32_t>(tex.role));
    append_pod(out, tex.width);
    append_pod(out, tex.height);
    const auto mip_count = static_cast<std::uint32_t>(tex.mips.size());
    append_pod(out, mip_count);
    const std::uint32_t reserved = 0u;
    append_pod(out, reserved);
    const auto payload_size = static_cast<std::uint64_t>(tex.payload.size());
    append_pod(out, payload_size);

    for (const auto& m : tex.mips)
        append_pod(out, m);
    for (std::uint32_t i = 0u; i < mip_count; ++i) {
        const float v = (i < tex.toksvig_mip_lengths.size()) ? tex.toksvig_mip_lengths[i] : 1.0f;
        append_pod(out, v);
    }
    while ((out.size() & 511u) != 0u)
        out.push_back(0u);
    out.insert(out.end(), tex.payload.begin(), tex.payload.end());
    return out;
}

Result<AtxTextureRef> decode_atx(std::span<const std::uint8_t> bytes) {
    std::span<const std::uint8_t> cur = bytes;
    std::uint32_t magic{};
    std::uint32_t ver{};
    std::uint32_t fmt_u{};
    std::uint32_t role_u{};
    std::uint32_t w{};
    std::uint32_t h{};
    std::uint32_t mip_count{};
    std::uint32_t reserved{};
    std::uint64_t payload_size{};
    if (!read_pod(cur, magic) || !read_pod(cur, ver) || !read_pod(cur, fmt_u) || !read_pod(cur, role_u) ||
        !read_pod(cur, w) || !read_pod(cur, h) || !read_pod(cur, mip_count) || !read_pod(cur, reserved) ||
        !read_pod(cur, payload_size)) {
        return std::unexpected(Error{ErrorCode::invalid_storage, "Truncated .atx header."});
    }
    if (magic != atx_magic || ver != atx_version || reserved != 0u || w == 0u || h == 0u || w > 16384u || h > 16384u ||
        mip_count != expected_mip_count(w, h) || fmt_u > static_cast<std::uint32_t>(AtxFormat::bc7_rgba_srgb) ||
        role_u > static_cast<std::uint32_t>(AtxRole::hdr_sky_bc6h)) {
        return std::unexpected(Error{ErrorCode::invalid_storage, "Unsupported or corrupt .atx container."});
    }

    const auto fmt = static_cast<AtxFormat>(fmt_u);
    const auto role = static_cast<AtxRole>(role_u);
    if (fmt != expected_atx_format(role, w, h))
        return std::unexpected(Error{ErrorCode::invalid_storage, "Mismatched role/format in .atx container."});
    const bool bcn = is_block_compressed(fmt);

    auto tex = std::make_shared<AtxTexture>();
    tex->format = fmt;
    tex->role = role;
    tex->width = w;
    tex->height = h;
    tex->mips.resize(mip_count);
    tex->toksvig_mip_lengths.resize(mip_count, 1.0f);
    for (std::uint32_t i = 0u; i < mip_count; ++i) {
        if (!read_pod(cur, tex->mips[i]))
            return std::unexpected(Error{ErrorCode::invalid_storage, "Truncated .atx mip table."});
    }
    for (std::uint32_t i = 0u; i < mip_count; ++i) {
        if (!read_pod(cur, tex->toksvig_mip_lengths[i]) || !std::isfinite(tex->toksvig_mip_lengths[i]) ||
            tex->toksvig_mip_lengths[i] < 0.0f || tex->toksvig_mip_lengths[i] > 1.0f) {
            return std::unexpected(Error{ErrorCode::invalid_storage, "Invalid .atx Toksvig table."});
        }
    }

    const std::size_t consumed = bytes.size() - cur.size();
    const std::size_t aligned_off = static_cast<std::size_t>(align_up_u64(consumed, 512u));
    if (aligned_off > bytes.size() || bytes.size() - aligned_off < payload_size)
        return std::unexpected(Error{ErrorCode::invalid_storage, "Truncated .atx mip payload."});

    const std::uint32_t blk_bytes = block_byte_size(fmt);
    for (std::uint32_t i = 0u; i < mip_count; ++i) {
        const AtxMipSlice& s = tex->mips[i];
        const std::uint32_t mw = std::max(1u, w >> i);
        const std::uint32_t mh = std::max(1u, h >> i);
        const std::uint32_t bx = bcn ? ((mw + 3u) >> 2u) : mw;
        const std::uint32_t by = bcn ? ((mh + 3u) >> 2u) : mh;
        const std::uint32_t row_bytes = bx * blk_bytes;
        const std::uint64_t expected_slice = std::uint64_t(s.row_pitch) * by;
        if (s.width != mw || s.height != mh || (s.byte_offset & 511ull) != 0ull || (s.row_pitch & 255u) != 0u ||
            s.row_pitch < row_bytes || expected_slice > 0xFFFFFFFFull ||
            s.slice_pitch != static_cast<std::uint32_t>(expected_slice) || s.byte_offset > payload_size ||
            expected_slice > payload_size - s.byte_offset) {
            return std::unexpected(Error{ErrorCode::invalid_storage, "Corrupt .atx mip slice layout."});
        }
    }

    tex->payload.assign(bytes.data() + aligned_off, bytes.data() + aligned_off + payload_size);
    return tex;
}

AtxTextureRef load_or_cook_atx_image(const material::Image& image, AtxRole role,
                                     const std::filesystem::path& cache_dir) {
    if (!cache_dir.empty()) {
        const std::string digest = compute_image_cache_digest(image, role);
        const std::filesystem::path file =
            cache_dir / (digest + "-v" + std::to_string(atx_version) + "-" + std::to_string(image.width()) + "x" +
                         std::to_string(image.height()) + "-r" + std::to_string(static_cast<unsigned>(role)) + ".atx");
        if (auto cached = try_read_cache_file(file, image.width(), image.height(), role))
            return cached;
        auto cooked = cook_atx_image(image, role);
        write_cache_file_atomic(cache_dir, file, *cooked);
        return cooked;
    }
    return cook_atx_image(image, role);
}

AtxTextureRef load_or_cook_atx_sky_map(const rendering::SkyMap& sky, const std::filesystem::path& cache_dir) {
    if (!cache_dir.empty()) {
        const std::string digest = compute_sky_cache_digest(sky);
        const std::filesystem::path file =
            cache_dir / (digest + "-v" + std::to_string(atx_version) + "-" + std::to_string(sky.width()) + "x" +
                         std::to_string(sky.height()) + "-sky.atx");
        if (auto cached = try_read_cache_file(file, sky.width(), sky.height(), AtxRole::hdr_sky_bc6h))
            return cached;
        auto cooked = cook_atx_sky_map(sky);
        write_cache_file_atomic(cache_dir, file, *cooked);
        return cooked;
    }
    return cook_atx_sky_map(sky);
}

} // namespace atelier::assets
