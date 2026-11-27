#include "material/image.hpp"
#include "foundation/sha256.hpp"
#include "memory/fatal.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <span>

namespace atelier::material {
namespace {

constexpr std::uint8_t cold_mode_raw = 0u;

constexpr std::uint8_t cold_mode_delta_lz4 = 1u;

std::uint32_t load_u32(const std::uint8_t* p) noexcept {
    std::uint32_t v = 0u;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

std::size_t hash_u32(std::uint32_t v) noexcept {
    return static_cast<std::size_t>((v * 2654435761u) >> 18u); // 14-bit table (16384 entries)
}

void append_len(std::vector<std::uint8_t>& out, std::size_t rem) {
    while (rem >= 255u) {
        out.push_back(255u);
        rem -= 255u;
    }
    out.push_back(static_cast<std::uint8_t>(rem));
}

std::vector<std::uint8_t> compress_rgba_cold(std::span<const std::uint8_t> raw) {
    const std::size_t n = raw.size();
    std::vector<std::uint8_t> filtered(n);
    for (std::size_t i = 0u; i < std::min<std::size_t>(4u, n); ++i)
        filtered[i] = raw[i];
    for (std::size_t i = 4u; i < n; ++i)
        filtered[i] = static_cast<std::uint8_t>(raw[i] - raw[i - 4u]);

    std::vector<std::uint8_t> out;
    if (n < 16u) {
        out.reserve(1u + n);
        out.push_back(cold_mode_raw);
        out.insert(out.end(), raw.begin(), raw.end());
        return out;
    }

    out.reserve(std::min<std::size_t>(n + 1u, (n >> 1u) + 64u));
    out.push_back(cold_mode_delta_lz4);

    std::array<std::uint32_t, 16384> table{};
    table.fill(0xFFFFFFFFu);

    const std::uint8_t* const src = filtered.data();
    const std::size_t match_limit = n - 5u;
    std::size_t anchor = 0u;
    std::size_t ip = 0u;

    while (ip <= match_limit) {
        const std::uint32_t seq = load_u32(src + ip);
        const std::size_t h = hash_u32(seq);
        const std::uint32_t ref = table[h];
        table[h] = static_cast<std::uint32_t>(ip);

        if (ref != 0xFFFFFFFFu && ip > ref && (ip - ref) <= 65535u && load_u32(src + ref) == seq) {
            const std::size_t lit_len = ip - anchor;
            std::size_t match_len = 4u;
            while (ip + match_len < n && src[ref + match_len] == src[ip + match_len])
                ++match_len;

            const std::uint8_t lit_tag = static_cast<std::uint8_t>(std::min<std::size_t>(lit_len, 15u));
            const std::uint8_t match_tag = static_cast<std::uint8_t>(std::min<std::size_t>(match_len - 4u, 15u));
            out.push_back(static_cast<std::uint8_t>((lit_tag << 4u) | match_tag));
            if (lit_len >= 15u)
                append_len(out, lit_len - 15u);
            out.insert(out.end(), src + anchor, src + ip);

            const std::uint16_t offset = static_cast<std::uint16_t>(ip - ref);
            out.push_back(static_cast<std::uint8_t>(offset & 0xFFu));
            out.push_back(static_cast<std::uint8_t>((offset >> 8u) & 0xFFu));
            if (match_len - 4u >= 15u)
                append_len(out, (match_len - 4u) - 15u);

            ip += match_len;
            anchor = ip;
            if (ip <= match_limit)
                table[hash_u32(load_u32(src + ip - 2u))] = static_cast<std::uint32_t>(ip - 2u);
            if (out.size() >= n) {
                out.clear();
                out.reserve(1u + n);
                out.push_back(cold_mode_raw);
                out.insert(out.end(), raw.begin(), raw.end());
                out.shrink_to_fit();
                return out;
            }
            continue;
        }
        ip += 1u + ((ip - anchor) >> 6u);
    }

    const std::size_t tail_lit = n - anchor;
    const std::uint8_t lit_tag = static_cast<std::uint8_t>(std::min<std::size_t>(tail_lit, 15u));
    out.push_back(static_cast<std::uint8_t>(lit_tag << 4u));
    if (tail_lit >= 15u)
        append_len(out, tail_lit - 15u);
    out.insert(out.end(), src + anchor, src + n);

    if (out.size() >= n + 1u) {
        out.clear();
        out.reserve(1u + n);
        out.push_back(cold_mode_raw);
        out.insert(out.end(), raw.begin(), raw.end());
    }
    out.shrink_to_fit();
    return out;
}

std::shared_ptr<const std::vector<std::uint8_t>> decompress_rgba_cold(std::span<const std::uint8_t> comp,
                                                                      std::size_t expected_bytes) {
    if (comp.empty())
        return nullptr;
    auto dst_vec = std::make_shared<std::vector<std::uint8_t>>(expected_bytes);
    auto& dst = *dst_vec;
    if (comp[0] == cold_mode_raw) {
        if (comp.size() != 1u + expected_bytes)
            fatal("Corrupt raw cold material image buffer.");
        std::memcpy(dst.data(), comp.data() + 1u, expected_bytes);
        return dst_vec;
    }
    if (comp[0] != cold_mode_delta_lz4)
        fatal("Unknown cold material image encoding.");

    std::size_t ip = 1u;
    std::size_t op = 0u;
    const std::size_t in_end = comp.size();
    while (ip < in_end) {
        const std::uint8_t tok = comp[ip++];
        std::size_t lit_len = tok >> 4u;
        if (lit_len == 15u) {
            std::uint8_t s = 0u;
            do {
                if (ip >= in_end)
                    fatal("Truncated literal length in cold material image.");
                s = comp[ip++];
                lit_len += s;
            } while (s == 255u);
        }
        if (lit_len > in_end - ip || lit_len > expected_bytes - op)
            fatal("Literal bounds overflow in cold material image.");
        std::memcpy(dst.data() + op, comp.data() + ip, lit_len);
        ip += lit_len;
        op += lit_len;
        if (ip >= in_end)
            break;

        if (in_end - ip < 2u)
            fatal("Truncated match offset in cold material image.");
        const std::size_t offset = std::size_t(comp[ip]) | (std::size_t(comp[ip + 1u]) << 8u);
        ip += 2u;
        if (offset == 0u || offset > op)
            fatal("Invalid match offset in cold material image.");

        std::size_t match_len = 4u + std::size_t(tok & 0x0Fu);
        if ((tok & 0x0Fu) == 15u) {
            std::uint8_t s = 0u;
            do {
                if (ip >= in_end)
                    fatal("Truncated match length in cold material image.");
                s = comp[ip++];
                match_len += s;
            } while (s == 255u);
        }
        if (match_len > expected_bytes - op)
            fatal("Match bounds overflow in cold material image.");
        for (std::size_t k = 0u; k < match_len; ++k) {
            dst[op] = dst[op - offset];
            ++op;
        }
    }
    if (op != expected_bytes)
        fatal("Decompressed cold material image size mismatch.");
    for (std::size_t i = 4u; i < expected_bytes; ++i)
        dst[i] = static_cast<std::uint8_t>(dst[i] + dst[i - 4u]);
    return dst_vec;
}

} // namespace

Result<ImageRef> Image::create(std::uint32_t width, std::uint32_t height, std::vector<std::uint8_t> pixels) {
    if (!width || !height || width > 8192 || height > 8192 || std::uint64_t(width) * height * 4 != pixels.size())
        return std::unexpected(Error{ErrorCode::invalid_storage, "Invalid material image dimensions or RGBA storage."});
    return ImageRef(new Image(width, height, std::move(pixels)));
}

Image::Image(std::uint32_t width, std::uint32_t height, std::vector<std::uint8_t> pixels)
    : width_(width), height_(height),
      rgba_sha256_(foundation::sha256(std::as_bytes(std::span<const std::uint8_t>{pixels}))),
      pixels_(std::make_shared<const std::vector<std::uint8_t>>(std::move(pixels))) {
    const auto& raw = *pixels_;
    bool gray = true;
    for (std::size_t i = 0; i < raw.size(); i += 4) {
        has_alpha_ |= raw[i + 3] != 255;
        gray &= (raw[i] == raw[i + 1] && raw[i] == raw[i + 2]);
    }
    is_grayscale_ = gray && !has_alpha_;
}

std::shared_ptr<const std::vector<std::uint8_t>> Image::acquire_pixels() const {
    std::scoped_lock lock(mutex_);
    if (pixels_)
        return pixels_;
    if (auto existing = cold_inflated_.lock())
        return existing;
    auto inflated = decompress_rgba_cold(compressed_, byte_size());
    cold_inflated_ = inflated;
    return inflated;
}

void Image::evict_cold_pixels() const noexcept {
    std::scoped_lock lock(mutex_);
    if (!pixels_)
        return;
    if (compressed_.empty())
        compressed_ = compress_rgba_cold(*pixels_);
    cold_inflated_ = pixels_;
    pixels_.reset();
}

bool Image::has_resident_pixels() const noexcept {
    std::scoped_lock lock(mutex_);
    return pixels_ != nullptr;
}

} // namespace atelier::material
