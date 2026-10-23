#include "source/snapshot.hpp"
#include "foundation/string.hpp"
#include "platform/windows_path.hpp"
#include <algorithm>
#include <fstream>
#include <windows.h>

namespace atelier::source {
namespace {
std::string utf8(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}
std::unexpected<Error> fail(std::string message) { return std::unexpected(Error{ErrorCode::invalid_definition, std::move(message)}); }
}

std::uint64_t content_hash(std::string_view text, std::uint64_t seed) noexcept {
    for (const auto c : text) { seed ^= static_cast<unsigned char>(c); seed *= 1099511628211ULL; }
    return seed ? seed : 1;
}

Result<SourceFile> read_source_file(const std::filesystem::path& path, std::string snapshot_path) {
    constexpr std::uint64_t limit = 1024 * 1024;
    std::ifstream file(platform::windows_file_path(path), std::ios::binary | std::ios::ate);
    if (!file) return fail("Cannot read " + utf8(path));
    const auto size = file.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > limit) return fail("Source file exceeds the 1 MiB limit: " + utf8(path));
    std::string text(static_cast<std::size_t>(size), '\0');
    file.seekg(0);
    if (!file.read(text.data(), static_cast<std::streamsize>(text.size()))) return fail("Could not read the complete file " + utf8(path));
    if (!StringView(std::string_view(text)).is_valid_utf8()) return fail("Source file is not valid UTF-8: " + utf8(path));
    const auto version = content_hash(text);
    if (text.starts_with("\xef\xbb\xbf")) text.erase(0, 3);
    return SourceFile{std::move(snapshot_path), version, std::move(text)};
}

Result<std::vector<std::filesystem::path>> engine_source_paths(const std::filesystem::path& root) {
    try {
        const auto native = platform::windows_file_path(root);
        std::vector<std::filesystem::path> paths;
        if (!std::filesystem::exists(native)) return paths;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(native)) {
            if (entry.is_symlink() || !entry.is_regular_file() || entry.path().extension() != L".source") continue;
            paths.push_back(entry.path().lexically_relative(native));
        }
        std::ranges::sort(paths);
        return paths;
    } catch (const std::exception& error) { return fail(error.what()); }
}

Result<std::vector<SourceFile>> engine_sources(const std::filesystem::path& root) {
    auto paths = engine_source_paths(root);
    if (!paths) return std::unexpected(paths.error());
    std::vector<SourceFile> files;
    for (const auto& relative : *paths) {
        auto file = read_source_file(root / relative, "engine/" + utf8(relative));
        if (!file) return std::unexpected(file.error());
        files.push_back(std::move(*file));
    }
    return files;
}

std::filesystem::path default_engine_root() {
    const std::filesystem::path tree{ATELIER_ENGINE_SOURCE_TREE};
    std::error_code error;
    if (std::filesystem::is_directory(tree, error)) return tree;
    std::wstring executable(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    executable.resize(length);
    return std::filesystem::path(executable).parent_path() / L"engine";
}
}
