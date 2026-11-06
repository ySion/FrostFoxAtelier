#include "foundation/sha256.hpp"
#include "platform/windows_path.hpp"
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>

namespace atelier::foundation {
Sha256::Sha256() {
    if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Cannot open SHA-256 provider");
    if (BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm_, 0);
        algorithm_ = nullptr;
        throw std::runtime_error("Cannot create SHA-256 hash");
    }
}

Sha256::~Sha256() {
    if (hash_)
        BCryptDestroyHash(hash_);
    if (algorithm_)
        BCryptCloseAlgorithmProvider(algorithm_, 0);
}

void Sha256::add(std::span<const std::byte> bytes) {
    while (!bytes.empty()) {
        const auto count = static_cast<ULONG>(std::min<std::size_t>(bytes.size(), 1024 * 1024));
        if (BCryptHashData(hash_, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())), count, 0) < 0)
            throw std::runtime_error("Cannot hash bytes");
        bytes = bytes.subspan(count);
    }
}

std::string Sha256::finish() {
    std::array<unsigned char, 32> digest{};
    if (BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
        throw std::runtime_error("Cannot finish SHA-256 hash");
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (auto value : digest) {
        result += digits[value >> 4];
        result += digits[value & 15];
    }
    return result;
}

std::string sha256(std::span<const std::byte> bytes) {
    Sha256 hash;
    hash.add(bytes);
    return hash.finish();
}

std::string sha256_file(const std::filesystem::path& path) {
    std::ifstream file(platform::windows_file_path(path), std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot read file for hashing");
    Sha256 hash;
    std::array<std::byte, 65536> buffer{};
    while (file) {
        file.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        hash.add({buffer.data(), static_cast<std::size_t>(file.gcount())});
    }
    if (!file.eof())
        throw std::runtime_error("Cannot finish reading file for hashing");
    return hash.finish();
}
}
