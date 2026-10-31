#pragma once
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>

namespace atelier::foundation {
// Streaming SHA-256. Call finish exactly once, after the last add. Provider and
// file failures throw std::runtime_error; protocol error codes belong to callers.
class Sha256 final {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void add(std::span<const std::byte>);
    std::string finish();

private:
    void* algorithm_{};
    void* hash_{};
};
std::string sha256(std::span<const std::byte>);
std::string sha256_file(const std::filesystem::path&);
}
