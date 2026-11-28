#pragma once
#include "language/source.hpp"

namespace atelier::language {
struct MaterialTextureInput {
    std::string key;

    std::uint32_t slot;

    std::size_t offset; // Actual slot-field offset inside the injected resource record.

    bool is_normal_map{};
};

struct MaterialModule {
    Program program;

    std::uint32_t context{};

    std::uint32_t parameters{};

    std::uint32_t output{};

    std::uint32_t texture_input{};

    std::uint32_t normal_texture_input{UINT32_MAX};

    std::uint32_t resources{UINT32_MAX};

    std::vector<MaterialTextureInput> textures;
};

// Uses the same typed Source tree and captured Runtime value layouts. No Runtime
// object, identity resolver or native method is part of the resulting program.
std::expected<MaterialModule, Diagnostic> compile_material(std::string_view name, std::string_view source,
                                                           std::span<const ValueLayout> values, std::uint32_t context,
                                                           std::uint32_t output, std::uint32_t texture_input,
                                                           std::uint32_t normal_texture_input = UINT32_MAX);

enum class MaterialCoverageStatus : std::uint8_t { available, missing_field, node_budget_exceeded };

struct MaterialHlsl {
    std::string shade;
    std::string footprint;
    std::string coverage;
    std::string gi_hit;
    MaterialCoverageStatus coverage_status{MaterialCoverageStatus::missing_field};
    MaterialCoverageStatus gi_hit_status{MaterialCoverageStatus::missing_field};
};

// Generate separate shader bodies after capturing the reachable layouts' initial values.
// A rejected coverage or GI-hit projection leaves full shading variants usable.
MaterialHlsl material_hlsl(const MaterialModule&);
} // namespace atelier::language
