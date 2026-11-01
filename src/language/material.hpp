#pragma once
#include "language/source.hpp"

namespace atelier::language {
struct MaterialTextureInput {
    std::string key;
    std::uint32_t slot;
    std::size_t offset; // Actual slot-field offset inside the injected resource record.
};
struct MaterialModule {
    Program program;
    std::uint32_t context{};
    std::uint32_t parameters{};
    std::uint32_t output{};
    std::uint32_t texture_input{};
    std::uint32_t resources{UINT32_MAX};
    std::vector<MaterialTextureInput> textures;
};
// Uses the same typed Source tree and captured Runtime value layouts. No Runtime
// object, identity resolver or native method is part of the resulting program.
std::expected<MaterialModule, Diagnostic> compile_material(std::string_view name, std::string_view source,
                                                           std::span<const ValueLayout> values, std::uint32_t context,
                                                           std::uint32_t output, std::uint32_t texture_input);
// Called after capturing initial_value for the reachable value layouts. The
// owned text selects coverage-only with ATELIER_MATERIAL_COVERAGE, otherwise
// ATELIER_MATERIAL_FOOTPRINT selects point (0) or two local changes (1).
// Coverage requires a top-level float coverage field; other Outputs retain the
// existing point/footprint support. Source's public wrapper selects the variant
// and declares its sampling bridge; the CPU/JIT invocation ABI is unchanged.
std::string material_hlsl(const MaterialModule&);
}
