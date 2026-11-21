#pragma once

// Adapted from unordered_dense v5.3.0, Copyright (c) 2022 Martin Leitner-Ankerl.
// MIT license: third_party/unordered_dense/LICENSE. Its byte hash descends from wyhash.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace atelier::detail {

[[nodiscard]] constexpr std::uint64_t multiply_fold(std::uint64_t a, std::uint64_t b) noexcept {
    const auto product = static_cast<unsigned __int128>(a) * b;
    return static_cast<std::uint64_t>(product) ^ static_cast<std::uint64_t>(product >> 64);
}

template <class Word>
[[nodiscard]] inline std::uint64_t read_hash_word(const std::byte* p) noexcept {
    Word value;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

inline constexpr std::array<std::uint64_t, 19> hash_constants{
    0xa0761d6478bd642fULL, 0xe7037ed1a0b428dbULL, 0x8ebc6af09c88c6e3ULL, 0x589965cc75374cc3ULL, 0x2d358dccaa6c78a5ULL,
    0x8bb84b93962eacc9ULL, 0x4b33a62ed433d4a3ULL, 0xa693c93927d87217ULL, 0x2b63728e53473c2bULL, 0x696cb2a95635a3c5ULL,
    0xa9ccd81ed1b29359ULL, 0x5c2d66ace48db84dULL, 0x69a99c5c53b4ca2dULL, 0x9a9c5a1b27d10f69ULL, 0x2b27f02dc3d4360fULL,
    0x2b39665c8d2d5553ULL, 0x966cd8878bb4b187ULL, 0xc6351e99932b1ee1ULL, 0xd1c5d24d63c959c9ULL};

[[nodiscard]] inline std::uint64_t hash_block(const std::byte* p, std::size_t salt) noexcept {
    return multiply_fold(read_hash_word<std::uint64_t>(p) ^ hash_constants[salt],
                         read_hash_word<std::uint64_t>(p + 8) ^ hash_constants[salt + 1]);
}

// The independent chains consume 96 or 48 bytes at a time. Leave the
// final sixteen bytes for a separate mix, including any overlap.
[[nodiscard]] inline std::uint64_t hash_long_bytes(const std::byte* p, std::size_t size) noexcept {
    const auto* end = p + size;
    std::array<std::uint64_t, 6> lanes;
    lanes.fill(hash_constants[0]);
    const auto step = [&](std::size_t lane) {
        lanes[lane] = multiply_fold(read_hash_word<std::uint64_t>(p) ^ hash_constants[lane + 1],
                                    read_hash_word<std::uint64_t>(p + 8) ^ lanes[lane]);
        p += 16;
    };
    if (size > 192) {
        do {
            for (std::size_t i = 0; i < 6; ++i)
                step(i);
        } while (end - p > 96);
        lanes[0] ^= lanes[3] ^ lanes[4] ^ lanes[5];
    }
    while (end - p > 48)
        for (std::size_t i = 0; i < 3; ++i)
            step(i);
    lanes[0] ^= lanes[1] ^ lanes[2];
    while (end - p > 16)
        step(0);
    return multiply_fold(hash_constants[1] ^ size, lanes[0] ^ hash_block(end - 16, 2));
}

[[nodiscard]] inline std::uint64_t hash_byte_range(const std::byte* p, std::size_t size) noexcept {
    if (size <= 16) {
        std::uint64_t first = 0;
        std::uint64_t last = 0;
        if (size >= 8) {
            first = read_hash_word<std::uint64_t>(p);
            last = read_hash_word<std::uint64_t>(p + size - 8);
        } else if (size >= 4) {
            first = read_hash_word<std::uint32_t>(p);
            last = read_hash_word<std::uint32_t>(p + size - 4);
        } else if (size) {
            first = (std::to_integer<std::uint64_t>(p[0]) << 16) | (std::to_integer<std::uint64_t>(p[size / 2]) << 8) |
                    std::to_integer<std::uint64_t>(p[size - 1]);
        }
        return multiply_fold(hash_constants[1] ^ size,
                             multiply_fold(first ^ hash_constants[1], last ^ hash_constants[0]));
    }
    if (size <= 144) {
        auto result = hash_block(p, 1) ^ hash_block(p + size - 16, 3);
        for (std::size_t offset = 16; offset + 16 < size; offset += 16)
            result ^= hash_block(p + offset, 3 + offset / 8);
        return multiply_fold(hash_constants[1] ^ size, result);
    }
    return hash_long_bytes(p, size);
}
}
