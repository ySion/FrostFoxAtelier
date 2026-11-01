#pragma once

#include "runtime/runtime.hpp"
#include "language/snapshot.hpp"
#include <filesystem>

namespace atelier::source {
using SourceFile = language::SourceFile;
// One immutable compilation input, owned by its Request. Paths are distinct,
// project-relative UTF-8 names; versions identify the exact saved text to editors.
using SourceSnapshot = language::SourceSnapshot;
struct ClassInfo {
    std::string name;
    TypeId type;
    TypeId base;
};
// parameter_names has one name per signature parameter; neither it nor code is part of entry compatibility.
// code is process-unique and kept only while the entry's compile unit input is unchanged.
struct EntryInfo {
    std::string name;
    FunctionSignature signature;
    std::vector<std::string> parameter_names;
    std::uint64_t code{};
};

// FNV-1a 64; never 0.
[[nodiscard]] std::uint64_t content_hash(std::string_view, std::uint64_t seed = 14695981039346656037ULL) noexcept;
// A saved source file of at most 1 MiB UTF-8. The version hashes the saved bytes; the text drops a UTF-8 BOM.
[[nodiscard]] Result<SourceFile> read_source_file(const std::filesystem::path&, std::string snapshot_path);
// Engine Source is <root>/<module>/**.source. Paths are relative to root and sorted; a missing root has none.
[[nodiscard]] Result<std::vector<std::filesystem::path>> engine_source_paths(const std::filesystem::path& root);
// Snapshot files named engine/<relative path>.
[[nodiscard]] Result<std::vector<SourceFile>> engine_sources(const std::filesystem::path& root);
// The source tree's assets/engine while it exists, otherwise engine/ beside the executable.
[[nodiscard]] std::filesystem::path default_engine_root();
}
