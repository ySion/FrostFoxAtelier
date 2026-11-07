#include "assets/file.hpp"
#include "platform/error.hpp"
#include "platform/utf.hpp"
#include "platform/windows_path.hpp"
#include <algorithm>
#include <fstream>

namespace atelier::assets {
namespace {
auto failure(std::string text) {
    return std::unexpected(Error{ErrorCode::invalid_storage, std::move(text)});
}
}

Result<std::string> read_file(const std::filesystem::path& path, std::size_t maximum) {
    auto native = platform::windows_file_path(path);
    if (!native)
        return failure(platform::error_message(native.error()));
    std::ifstream file(*native, std::ios::binary | std::ios::ate);
    if (!file) {
        auto name = platform::utf16_to_utf8(path.native());
        if (!name)
            return failure("Invalid UTF-16 text.");
        return failure("Cannot read " + *name);
    }
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
    if (relative.empty() || relative.has_root_path())
        return failure("Expected a project-relative path.");
    for (const auto& part : relative)
        if (part == L".." || part.wstring().find(L':') != std::wstring::npos ||
            std::ranges::any_of(part.wstring(), [](wchar_t c) { return c < 32; }))
            return failure("Invalid project-relative path.");
    auto native = platform::windows_file_path(root / relative);
    if (!native)
        return failure(platform::error_message(native.error()));
    std::error_code error;
    auto path = std::filesystem::weakly_canonical(*native, error);
    if (error)
        return failure(platform::error_message(error));
    const auto within = path.lexically_relative(root);
    if (within.empty() || within.is_absolute() || *within.begin() == L"..")
        return failure("Project path escapes its root through a link.");
    return path;
}
}
