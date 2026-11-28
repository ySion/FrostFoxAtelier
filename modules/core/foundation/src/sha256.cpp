#include "foundation/sha256.hpp"
#include "memory/fatal.hpp"
#include "platform/error.hpp"
#include "platform/windows_path.hpp"
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <fstream>

namespace atelier::foundation {
Sha256::Sha256() noexcept {
    if (BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash_, nullptr, 0, nullptr, 0, 0) < 0)
        fatal("Cannot create SHA-256 hash");
}

Sha256::~Sha256() {
    BCryptDestroyHash(hash_);
}

void Sha256::add(atelier::Span<const std::byte> bytes) noexcept {
    while (!bytes.empty()) {
        const auto count = static_cast<ULONG>(std::min<std::size_t>(bytes.size(), 1024 * 1024));
        if (BCryptHashData(hash_, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())), count, 0) < 0)
            fatal("Cannot hash bytes");
        bytes.remove_prefix(count);
    }
}

String Sha256::finish() noexcept {
    std::array<unsigned char, 32> digest{};
    if (BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
        fatal("Cannot finish SHA-256 hash");
    constexpr char digits[] = "0123456789abcdef";
    char hex[64];
    for (std::size_t i = 0; i < digest.size(); ++i) {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 15];
    }
    return String::from_utf8_validated({hex, sizeof(hex)});
}

String sha256(atelier::Span<const std::byte> bytes) noexcept {
    Sha256 hash;
    hash.add(bytes);
    return hash.finish();
}

std::expected<String, String> sha256_file(const std::filesystem::path& path) noexcept {
    auto native = platform::windows_file_path(path);
    if (!native)
        return std::unexpected(platform::error_message(native.error()));
    std::ifstream file(*native, std::ios::binary);
    if (!file)
        return std::unexpected("Cannot read file for hashing"_s);
    Sha256 hash;
    std::array<std::byte, 65536> buffer{};
    while (file) {
        file.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        hash.add({buffer.data(), static_cast<std::size_t>(file.gcount())});
    }
    if (!file.eof())
        return std::unexpected("Cannot finish reading file for hashing"_s);
    return hash.finish();
}
}
