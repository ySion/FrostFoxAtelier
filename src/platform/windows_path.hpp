#pragma once
#include <filesystem>

namespace atelier::platform {
// Win32 file APIs otherwise apply MAX_PATH to deep project/artifact paths.
// This converts spelling only; callers still own containment/link validation.
inline std::filesystem::path windows_file_path(const std::filesystem::path& path) {
    auto name=std::filesystem::absolute(path).lexically_normal().make_preferred().wstring();
    if(name.starts_with(L"\\\\?\\")) return name;
    if(name.starts_with(L"\\\\")) return L"\\\\?\\UNC\\"+name.substr(2);
    return L"\\\\?\\"+name;
}
}
