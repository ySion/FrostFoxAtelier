#pragma once
#include "assets/assets.hpp"
#include "assets/image_decode.hpp"
#include "geometry/mesh.hpp"
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atelier::assets {
inline constexpr std::array<std::string_view, 6> imported_material_inputs{"base_color", "roughness", "metallic",
                                                                          "normal",     "emissive",  "opacity"};
struct ImportedFile {
    std::filesystem::path relative;
    std::string bytes;
};
struct MeshImport {
    geometry::MeshRef mesh;
    // One immutable read per dependency, relative to the source file's directory.
    // Import publishes these exact bytes; project refresh tracks the same paths.
    std::vector<ImportedFile> files;
    std::vector<std::string> notes; // Source content the import left out, one line each.
    // Both arrays align with mesh->materials(). An absent author material is
    // nullopt, never inferred from white/default parameter values.
    std::vector<std::optional<MaterialInstanceAsset>> materials;
    // StandardPBR input order above. Each nonempty path names an entry in files;
    // its <input>_texture override has an empty key until Project assigns a GUID.
    std::vector<std::array<std::filesystem::path, imported_material_inputs.size()>> textures;
};
// Wavefront OBJ/MTL or glTF 2.0 (.gltf with external buffers and images).
bool is_mesh_path(const std::filesystem::path&);
// Throws with context on invalid input. All dependencies must remain inside
// the source directory, including after resolving links. No disk mutations.
MeshImport load_mesh(const std::filesystem::path& source, ImageCache* images = nullptr);
MeshImport load_obj(const std::filesystem::path& source, ImageCache* images = nullptr);
MeshImport load_gltf(const std::filesystem::path& source, ImageCache* images = nullptr);
}
