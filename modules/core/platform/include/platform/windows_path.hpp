#pragma once
#include <expected>
#include <filesystem>

namespace atelier::platform {
// Win32 file APIs otherwise apply MAX_PATH to deep project/artifact paths.
// This converts spelling only; callers still own containment/link validation.
inline std::expected<std::filesystem::path, std::error_code>
windows_file_path(const std::filesystem::path& path) noexcept {
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    if (error)
        return std::unexpected(error);
    auto name = absolute.lexically_normal().make_preferred().wstring();
    if (name.starts_with(L"\\\\?\\"))
        return name;
    if (name.starts_with(L"\\\\"))
        return L"\\\\?\\UNC\\" + name.substr(2);
    return L"\\\\?\\" + name;
}
}
