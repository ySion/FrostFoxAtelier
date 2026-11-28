#pragma once

#include "foundation/string.hpp"
#include "platform/utf.hpp"

#include <expected>
#include <filesystem>
#include <system_error>

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

// Locale-invariant Windows ordinal comparison after lexical normalization,
// without touching the filesystem.
[[nodiscard]] bool paths_lexically_equal(const std::filesystem::path& left,
                                         const std::filesystem::path& right) noexcept;

// True when candidate (lexically normalized) equals base or resides inside base
// without escaping via parent-directory ('..') segments. Purely lexical.
[[nodiscard]] bool is_subpath_of(const std::filesystem::path& candidate, const std::filesystem::path& base) noexcept;

[[nodiscard]] inline std::expected<String, UtfError> path_to_utf8(const std::filesystem::path& path) noexcept {
    return utf16_to_utf8(path.native());
}

[[nodiscard]] std::expected<String, UtfError> generic_path_to_utf8(const std::filesystem::path& path) noexcept;

[[nodiscard]] inline std::expected<String, UtfError> filename_utf8(const std::filesystem::path& path) noexcept {
    return utf16_to_utf8(path.filename().native());
}

} // namespace atelier::platform
