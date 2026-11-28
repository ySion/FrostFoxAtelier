#include "source/snapshot.hpp"
#include "foundation/string.hpp"
#include "language/declarations.hpp"
#include "platform/error.hpp"
#include "platform/utf.hpp"
#include "platform/windows_path.hpp"
#include <algorithm>
#include <fstream>
#include <windows.h>

namespace atelier::source {
namespace {
std::unexpected<Error> fail(Utf8Text message) {
    return std::unexpected(Error{ErrorCode::invalid_definition, std::move(message)});
}
}

std::uint64_t content_hash(StringView text, std::uint64_t seed) noexcept {
    for (const auto c : text) {
        seed ^= static_cast<unsigned char>(c);
        seed *= 1099511628211ULL;
    }
    return seed ? seed : 1;
}

Result<SourceFile> read_source_file(const std::filesystem::path& path, Utf8Text snapshot_path) {
    constexpr std::uint64_t limit = 1024 * 1024;
    auto native = platform::windows_file_path(path);
    if (!native)
        return fail(platform::error_message(native.error()));
    auto name = platform::utf16_to_utf8(path.generic_wstring());
    if (!name)
        return fail("Source file path is not valid UTF-16.");
    std::ifstream file(*native, std::ios::binary | std::ios::ate);
    if (!file)
        return fail(String::concat("Cannot read ", *name));
    const auto size = file.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > limit)
        return fail(String::concat("Source file exceeds the 1 MiB limit: ", *name));
    Array<char> raw;
    raw.resize_uninitialized(static_cast<std::size_t>(size));
    file.seekg(0);
    if (!raw.empty() && !file.read(raw.data(), static_cast<std::streamsize>(raw.size())))
        return fail(String::concat("Could not read the complete file ", *name));
    StringView view{raw.data(), raw.size()};
    if (!view.is_valid_utf8())
        return fail(String::concat("Source file is not valid UTF-8: ", *name));
    const auto version = content_hash(view);
    if (view.starts_with("\xef\xbb\xbf"))
        view.remove_prefix(3);
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    if (error)
        return fail(platform::error_message(error));
    auto location = platform::utf16_to_utf8(absolute.lexically_normal().generic_wstring());
    if (!location)
        return fail("Absolute source file path is not valid UTF-16.");
    return SourceFile{std::move(snapshot_path).into_string(), version, String::from_utf8_validated(view),
                      std::move(*location)};
}

Result<Array<std::filesystem::path>> engine_source_paths(const std::filesystem::path& root) {
    auto native = platform::windows_file_path(root);
    if (!native)
        return fail(platform::error_message(native.error()));
    Array<std::filesystem::path> paths;
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
            paths.add(entry->path().lexically_relative(*native));
        entry.increment(error);
        if (error)
            return fail(platform::error_message(error));
    }
    std::ranges::sort(paths);
    return paths;
}

Result<Array<SourceFile>> engine_sources(const std::filesystem::path& root) {
    ATELIER_TRY_BIND(paths, engine_source_paths(root));
    Array<SourceFile> files;
    files.reserve(paths->size());
    for (const auto& relative : *paths) {
        auto name = platform::utf16_to_utf8(relative.generic_wstring());
        if (!name)
            return fail("Engine source file path is not valid UTF-16.");
        ATELIER_TRY_BIND(file, read_source_file(root / relative, String::concat("engine/", *name)));
        files.add(std::move(*file));
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

Result<String> source_type_name(const Runtime& runtime, TypeId id) {
    ATELIER_TRY_BIND(described, runtime.describe(id));
    const TypeInfo& info = **described;
    const auto unprojectable = [&] {
        return std::unexpected(
            Error{ErrorCode::invalid_type, String::concat("Type has no Source projection: ", info.name)});
    };
    if (language::editor_reserved(info.name.view().std_view()) || info.contains_references)
        return unprojectable();
    switch (info.kind) {
    case TypeKind::scalar:
        if (runtime.native_type<bool>() == id)
            return "bool"_s;
        if (runtime.native_type<std::int32_t>() == id)
            return "int"_s;
        if (runtime.native_type<std::uint32_t>() == id)
            return "uint"_s;
        if (runtime.native_type<float>() == id)
            return "float"_s;
        if (runtime.native_type<double>() == id)
            return "double"_s;
        return unprojectable();
    case TypeKind::enumeration:
        return info.enumeration ? Result<String>{info.name} : unprojectable();
    case TypeKind::string:
        return info.sequence_view ? Result<String>{"String"_s} : unprojectable();
    case TypeKind::record:
        if (info.math)
            return String::from_utf8_validated(StringView{math::short_name(info.math)});
        return info.name;
    case TypeKind::array: {
        if (!info.sequence_view || !info.element)
            return unprojectable();
        ATELIER_TRY_BIND(elem, source_type_name(runtime, *info.element));
        return info.standard_array_layout ? String::concat("Array<", *elem, ">") : info.name;
    }
    case TypeKind::object:
        return unprojectable();
    }
    return unprojectable();
}
}
