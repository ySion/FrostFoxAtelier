#pragma once
#include "geometry/mesh.hpp"
#include "rendering/sky_map.hpp"
#include <string_view>
#include <string>
#include <filesystem>
#include <vector>

namespace atelier::assets {
enum class AssetKind { folder, mesh, scene, source, file, texture, material, sky };
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
    std::vector<SlotMaterial> materials{}; // Slots without an entry show their imported surface.
};
// A key and the sky map it resolved to; an empty key with a null map clears an Environment's sky map.
struct SkyAsset {
    std::string key;
    std::wstring name;
    rendering::SkyMapRef map;
};
struct TextureAsset {std::string key;std::wstring name;material::ImageRef image;};
struct MaterialAsset {
    std::string key;
    std::wstring name;
    material::Description description;
    std::array<std::string,material::input_count> textures;
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
}
