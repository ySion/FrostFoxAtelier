#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace atelier::language {
struct SourceFile {
    std::string path;

    std::uint64_t version{};

    std::string source;

    // Diagnostic provenance only; empty for virtual text. Never used to resolve imports.
    std::string disk_path{};
};

struct SourceSnapshot {
    std::string name;

    std::uint64_t version{};

    std::vector<SourceFile> files;
};
}
