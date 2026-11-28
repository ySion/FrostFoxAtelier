#pragma once

#include "foundation/span.hpp"
#include "foundation/string.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>

namespace atelier::foundation {
// Streaming SHA-256. Call finish exactly once, after the last add. Memory hashing
// has no recoverable input failures; CNG primitive failure and CPU OOM are fatal.
class Sha256 final {
public:
    Sha256() noexcept;

    ~Sha256();

    Sha256(const Sha256&) = delete;

    Sha256& operator=(const Sha256&) = delete;

    void add(atelier::Span<const std::byte>) noexcept;

    String finish() noexcept;

private:
    void* hash_{};
};

String sha256(atelier::Span<const std::byte>) noexcept;

// Path, open and read failures return diagnostics; protocol codes belong to callers.
std::expected<String, String> sha256_file(const std::filesystem::path&) noexcept;
}
