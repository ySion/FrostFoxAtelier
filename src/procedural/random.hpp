#pragma once

#include <cstdint>

namespace atelier::procedural {

// Fixed integer hashes for per-element randomness. Results are part of the
// generated content, so the algorithms never change silently: lowbias32 by
// Chris Wellons (public domain), combined as hash(a ^ hash(b)).
[[nodiscard]] constexpr std::uint32_t hash(std::uint32_t x) noexcept {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

[[nodiscard]] constexpr std::uint32_t hash(std::uint32_t a, std::uint32_t b) noexcept {
    return hash(a ^ hash(b));
}

// Uniform in [0, 1) from the top 24 bits, exact in float.
[[nodiscard]] constexpr float unit(std::uint32_t x) noexcept {
    return static_cast<float>(x >> 8) * 0x1p-24f;
}

}
