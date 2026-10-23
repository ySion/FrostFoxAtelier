#pragma once
#include "geometry/mesh.hpp"
#include <string_view>
#include <string>
#include <filesystem>
#include <vector>

namespace atelier::editor {
enum class AssetKind { folder, mesh, scene, source, file, texture, material };
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
std::span<const MeshAsset> mesh_assets();
const MeshAsset* mesh_asset(std::string_view key);
std::filesystem::path asset_directory();
}
