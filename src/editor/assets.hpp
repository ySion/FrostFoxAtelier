#pragma once
#include "geometry/mesh.hpp"
#include <string_view>
#include <string>
#include <filesystem>

namespace atelier::editor {
// Resolved resource data shared by engine and project catalogs. Keys identify
// persistent catalog entries, never Runtime objects or GPU allocations.
struct MeshAsset {
    std::string key;
    std::wstring name;
    geometry::MeshRef mesh;
};
std::span<const MeshAsset> mesh_assets();
const MeshAsset* mesh_asset(std::string_view key);
std::filesystem::path asset_directory();
}
