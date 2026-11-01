#pragma once
#include "runtime/runtime.hpp"
#include <filesystem>

namespace atelier::assets {
Result<std::string> read_file(const std::filesystem::path&, std::size_t maximum = 16 * 1024 * 1024);

std::string utf8(std::wstring_view);

std::wstring wide(std::string_view);

// Shared containment rule for project readers and author writes. Does not create files.
Result<std::filesystem::path> resolve_project_path(const std::filesystem::path& root,
                                                   const std::filesystem::path& relative);
}
