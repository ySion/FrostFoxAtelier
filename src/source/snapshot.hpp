#pragma once

#include "runtime/runtime.hpp"
#include "language/snapshot.hpp"

namespace atelier::source {
using SourceFile = language::SourceFile;
// One immutable compilation input, owned by its Request. Paths are distinct,
// project-relative UTF-8 names; versions identify the exact saved text to editors.
using SourceSnapshot = language::SourceSnapshot;
struct ClassInfo { std::string name; TypeId type, base; };
struct EntryInfo { std::string name; FunctionSignature signature; };
}
