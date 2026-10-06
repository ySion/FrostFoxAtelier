#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace atelier::language {
struct SourceFile { std::string path; std::uint64_t version{}; std::string source; };
struct SourceSnapshot { std::string name; std::uint64_t version{}; std::vector<SourceFile> files; };
}
