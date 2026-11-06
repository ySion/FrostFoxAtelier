#include "assets/file.hpp"
#include "platform/windows_path.hpp"
#include <Windows.h>
#include <algorithm>
#include <fstream>

namespace atelier::assets {
namespace {
auto failure(std::string text) {
    return std::unexpected(Error{ErrorCode::invalid_storage, std::move(text)});
}
}

std::string utf8(std::wstring_view value) {
    if (value.empty())
        return {};
    const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (!count)
        throw std::runtime_error("Invalid UTF-16 text.");
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(),
                        count, nullptr, nullptr);
    return result;
}

std::wstring wide(std::string_view value) {
    if (value.empty())
        return {};
    const auto count =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!count)
        throw std::runtime_error("Invalid UTF-8 text.");
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(),
                        count);
    return result;
}

Result<std::string> read_file(const std::filesystem::path& path, std::size_t maximum) {
    std::ifstream file(platform::windows_file_path(path), std::ios::binary | std::ios::ate);
    if (!file)
        return failure("Cannot read " + utf8(path.wstring()));
    const auto size = file.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > maximum)
        return failure("File exceeds its size limit.");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    file.seekg(0);
    if (!file.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
        return failure("Could not read the complete file.");
    return bytes;
}

Result<std::filesystem::path> resolve_project_path(const std::filesystem::path& root,
                                                   const std::filesystem::path& relative) {
    try {
        if (relative.empty() || relative.has_root_path())
            return failure("Expected a project-relative path.");
        for (const auto& part : relative)
            if (part == L".." || part.wstring().find(L':') != std::wstring::npos ||
                std::ranges::any_of(part.wstring(), [](wchar_t c) { return c < 32; }))
                return failure("Invalid project-relative path.");
        auto path = std::filesystem::weakly_canonical(platform::windows_file_path(root / relative));
        const auto within = path.lexically_relative(root);
        if (within.empty() || within.is_absolute() || *within.begin() == L"..")
            return failure("Project path escapes its root through a link.");
        return path;
    } catch (const std::exception& error) {
        return failure(error.what());
    }
}
}
