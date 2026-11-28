#pragma once
#include "foundation/array.hpp"
#include "foundation/span.hpp"
#include "foundation/string.hpp"
#include "runtime/runtime.hpp"
#include <cstddef>
#include <filesystem>

namespace atelier::assets {
Result<Array<std::byte>> read_file(const std::filesystem::path&, std::size_t maximum = 16 * 1024 * 1024);

Result<String> read_utf8_file(const std::filesystem::path&, std::size_t maximum = 16 * 1024 * 1024);

// Shared containment rule for project readers and author writes. Does not create files.
Result<std::filesystem::path> resolve_project_path(const std::filesystem::path& root,
                                                   const std::filesystem::path& relative);
}
