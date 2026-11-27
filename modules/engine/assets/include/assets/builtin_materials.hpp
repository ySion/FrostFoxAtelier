#pragma once

#include "assets/assets.hpp"
#include "source/snapshot.hpp"

namespace atelier::assets {
// Ordinary immutable MS author data; the compiler and renderer do not special-case
// these keys. Four StandardPBR policies share one source and parameter interface.
Span<const MaterialAsset> material_assets();

Optional<source::SourceFile> builtin_material_source(std::string_view path);
}
