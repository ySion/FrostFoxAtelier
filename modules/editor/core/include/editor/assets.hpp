#pragma once
#include "runtime/runtime.hpp"
#include <filesystem>

namespace atelier::editor {
Result<std::filesystem::path> asset_directory();
}
