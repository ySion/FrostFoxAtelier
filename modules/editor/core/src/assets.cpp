#include "editor/assets.hpp"
#include <windows.h>
#include <stdexcept>
#include <string>

namespace atelier::editor {
std::filesystem::path asset_directory() {
    std::wstring executable(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size())
        throw std::runtime_error("Cannot locate editor assets.");
    executable.resize(length);
    return std::filesystem::path(executable).parent_path();
}
}
