#pragma once
#include "geometry/mesh.hpp"
#include "material/program_resources.hpp"
#include "rendering/sky_map.hpp"
#include <string_view>
#include <string>
#include <filesystem>
#include <variant>
#include <vector>

namespace atelier::assets {
enum class AssetKind { folder, mesh, scene, source, file, texture, material, sky, procedural_graph };

// An installed producer's compatibility declaration and actual immutable content.
// Neither the digest nor the semantic version authenticates executable code.
struct EngineDependency {
    std::string key;

    AssetKind kind;

    std::uint32_t semantic_version;

    std::uint32_t digest_version;

    std::string content_sha256;

    bool operator==(const EngineDependency&) const = default;
};

inline constexpr std::uint32_t mesh_digest_version = 2;

inline constexpr std::uint32_t material_digest_version = 2;

// A mesh slot's default material: the material asset key its sidecar names for the slot.
struct SlotMaterial {
    std::string slot;

    std::string key;

    bool operator==(const SlotMaterial&) const = default;
};

// Resolved resource data shared by engine and project catalogs. Keys identify
// persistent catalog entries, never Runtime objects or GPU allocations.
struct MeshAsset {
    std::string key;

    std::wstring name;

    geometry::MeshRef mesh;

    std::vector<SlotMaterial> materials{}; // Slots without an entry show DefaultWorldGrid.
};

// A key and the sky map it resolved to; an empty key with a null map clears an Environment's sky map.
struct SkyAsset {
    std::string key;

    std::wstring name;

    rendering::SkyMapRef map;
};

struct TextureAsset {
    std::string key;

    std::wstring name;

    material::ImageRef image;
};

struct MaterialSourceAsset {
    std::string source_path; // Explicit project-relative Source path, or a reserved builtin/ Source path.

    material::Policy policy;

    std::vector<material::TextureDefault> inputs;
};

struct MaterialInstanceAsset {
    std::string parent_key{"builtin/material/standard-opaque-double"};

    std::vector<material::Override> entries;
};

// Author values only: no compiled program, resolved texture or Runtime owner.
struct MaterialAsset {
    std::string key;

    std::wstring name;

    std::variant<MaterialSourceAsset, MaterialInstanceAsset> author{MaterialInstanceAsset{}};
};

struct ProceduralGraphAsset {
    std::string key;

    std::wstring name;

    std::string text;
};

struct ProjectEntry {
    std::filesystem::path path; // Relative to the project root; never a Runtime ID.

    std::string key;

    AssetKind kind{};

    std::uintmax_t bytes{};

    std::filesystem::file_time_type modified{};

    std::vector<SlotMaterial> materials{};

    bool operator==(const ProjectEntry&) const = default;
};

// Immutable engine catalogue, in its presentation order. Typed loads use Reader.
std::span<const MeshAsset> mesh_assets();

// Shared by directory classification and import dependency validation.
bool is_texture_path(const std::filesystem::path&);

bool is_sky_path(const std::filesystem::path&);

// Both roles remain AssetKind::material; .ms.material is MS, other .material is MI.
bool is_material_source_path(const std::filesystem::path&);

bool is_procedural_graph_path(const std::filesystem::path&);
}
