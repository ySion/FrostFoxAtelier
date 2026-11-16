#pragma once

#include "foundation/span.hpp"

#include <concepts>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <type_traits>

namespace atelier::foundation {
// Streaming SHA-256. Call finish exactly once, after the last add. Memory hashing
// has no recoverable input failures; CNG primitive failure and CPU OOM are fatal.
class Sha256 final {
public:
    Sha256() noexcept;

    ~Sha256();

    Sha256(const Sha256&) = delete;

    Sha256& operator=(const Sha256&) = delete;

    void add(std::span<const std::byte>) noexcept;

    template <class T>
        requires std::same_as<std::remove_const_t<T>, std::byte>
    void add(atelier::Span<T> bytes) noexcept {
        add(bytes.std_span());
    }

    std::string finish() noexcept;

private:
    void* hash_{};
};

std::string sha256(std::span<const std::byte>) noexcept;

template <class T>
    requires std::same_as<std::remove_const_t<T>, std::byte>
inline std::string sha256(atelier::Span<T> bytes) noexcept {
    return sha256(bytes.std_span());
}

// Path, open and read failures return diagnostics; protocol codes belong to callers.
std::expected<std::string, std::string> sha256_file(const std::filesystem::path&) noexcept;
}
