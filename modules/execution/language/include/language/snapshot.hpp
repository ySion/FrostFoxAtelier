#pragma once
#include "foundation/array.hpp"
#include "foundation/string.hpp"
#include <cstdint>
#include <utility>

namespace atelier::language {
struct SourceFile {
    String path;

    std::uint64_t version{};

    String source;

    // Diagnostic provenance only; empty for virtual text. Never used to resolve imports.
    String disk_path{};

    SourceFile() noexcept = default;

    SourceFile(Utf8Text file_path, std::uint64_t file_version, Utf8Text source_text,
               Utf8Text original_disk_path = {}) noexcept
        : path(std::move(file_path).into_string()), version(file_version),
          source(std::move(source_text).into_string()), disk_path(std::move(original_disk_path).into_string()) {}
};

struct SourceSnapshot {
    String name;

    std::uint64_t version{};

    Array<SourceFile> files;

    SourceSnapshot() noexcept = default;

    SourceSnapshot(Utf8Text snapshot_name, std::uint64_t snapshot_version, Array<SourceFile> snapshot_files = {}) noexcept
        : name(std::move(snapshot_name).into_string()), version(snapshot_version),
          files(std::move(snapshot_files)) {}
};
}
