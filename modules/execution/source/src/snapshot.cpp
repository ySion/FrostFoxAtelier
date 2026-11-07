#include "source/snapshot.hpp"
#include "foundation/string.hpp"
#include "platform/error.hpp"
#include "platform/utf.hpp"
#include "platform/windows_path.hpp"
#include <algorithm>
#include <fstream>
#include <windows.h>

namespace atelier::source {
namespace {
std::unexpected<Error> fail(std::string message) {
    return std::unexpected(Error{ErrorCode::invalid_definition, std::move(message)});
}
}

std::uint64_t content_hash(std::string_view text, std::uint64_t seed) noexcept {
    for (const auto c : text) {
        seed ^= static_cast<unsigned char>(c);
        seed *= 1099511628211ULL;
    }
    return seed ? seed : 1;
}

Result<SourceFile> read_source_file(const std::filesystem::path& path, std::string snapshot_path) {
    constexpr std::uint64_t limit = 1024 * 1024;
    auto native = platform::windows_file_path(path);
    if (!native)
        return fail(platform::error_message(native.error()));
    auto name = platform::utf16_to_utf8(path.generic_wstring());
    if (!name)
        return fail("Source file path is not valid UTF-16.");
    std::ifstream file(*native, std::ios::binary | std::ios::ate);
    if (!file)
        return fail("Cannot read " + *name);
    const auto size = file.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > limit)
        return fail("Source file exceeds the 1 MiB limit: " + *name);
    std::string text(static_cast<std::size_t>(size), '\0');
    file.seekg(0);
    if (!file.read(text.data(), static_cast<std::streamsize>(text.size())))
        return fail("Could not read the complete file " + *name);
    if (!StringView(std::string_view(text)).is_valid_utf8())
        return fail("Source file is not valid UTF-8: " + *name);
    const auto version = content_hash(text);
    if (text.starts_with("\xef\xbb\xbf"))
        text.erase(0, 3);
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    if (error)
        return fail(platform::error_message(error));
    auto location = platform::utf16_to_utf8(absolute.lexically_normal().generic_wstring());
    if (!location)
        return fail("Absolute source file path is not valid UTF-16.");
    return SourceFile{std::move(snapshot_path), version, std::move(text), std::move(*location)};
}

Result<std::vector<std::filesystem::path>> engine_source_paths(const std::filesystem::path& root) {
    auto native = platform::windows_file_path(root);
    if (!native)
        return fail(platform::error_message(native.error()));
    std::vector<std::filesystem::path> paths;
    std::error_code error;
    const auto exists = std::filesystem::exists(*native, error);
    if (error)
        return fail(platform::error_message(error));
    if (!exists)
        return paths;
    std::filesystem::recursive_directory_iterator entry(*native, error);
    std::filesystem::recursive_directory_iterator end;
    if (error)
        return fail(platform::error_message(error));
    while (entry != end) {
        const auto status = entry->symlink_status(error);
        if (error && !std::filesystem::status_known(status))
            return fail(platform::error_message(error));
        if (std::filesystem::is_regular_file(status) && entry->path().extension() == L".source")
            paths.push_back(entry->path().lexically_relative(*native));
        entry.increment(error);
        if (error)
            return fail(platform::error_message(error));
    }
    std::ranges::sort(paths);
    return paths;
}

Result<std::vector<SourceFile>> engine_sources(const std::filesystem::path& root) {
    ATELIER_TRY_BIND(paths, engine_source_paths(root));
    std::vector<SourceFile> files;
    for (const auto& relative : *paths) {
        auto name = platform::utf16_to_utf8(relative.generic_wstring());
        if (!name)
            return fail("Engine source file path is not valid UTF-16.");
        ATELIER_TRY_BIND(file, read_source_file(root / relative, "engine/" + *name));
        files.push_back(std::move(*file));
    }
    return files;
}

std::filesystem::path default_engine_root() {
    const std::filesystem::path tree{ATELIER_ENGINE_SOURCE_TREE};
    std::error_code error;
    if (std::filesystem::is_directory(tree, error))
        return tree;
    std::wstring executable(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    executable.resize(length);
    return std::filesystem::path(executable).parent_path() / L"engine";
}
}
