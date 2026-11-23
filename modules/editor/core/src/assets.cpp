#include "editor/assets.hpp"
#include <windows.h>
#include <string>

namespace atelier::editor {
Result<std::filesystem::path> asset_directory() {
    std::wstring executable(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size())
        return std::unexpected(Error{ErrorCode::invalid_storage, "Cannot locate the editor executable directory."});
    executable.resize(length);
    return std::filesystem::path(executable).parent_path();
}
}
