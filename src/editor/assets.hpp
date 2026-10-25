#pragma once
#include "assets/assets.hpp"

namespace atelier::editor {
using assets::AssetKind;
using assets::SlotMaterial;
using assets::MeshAsset;
using assets::SkyAsset;
using assets::TextureAsset;
using assets::MaterialAsset;
using assets::ProjectEntry;
using assets::mesh_assets;
std::filesystem::path asset_directory();
}
