#pragma once
#include "editor/image_decode.hpp"
#include "geometry/mesh.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace atelier::editor {
struct ImportedFile { std::filesystem::path relative; std::string bytes; };
struct MeshImport {
    geometry::MeshRef mesh;
    // One immutable read per dependency, relative to the source file's directory.
    // Import publishes these exact bytes; project refresh tracks the same paths.
    std::vector<ImportedFile> files;
    std::vector<std::string> notes; // Source content the import left out, one line each.
    // glTF only: per material slot and input, the file in `files` that supplied the texture image.
    // Import Mesh generates one material asset per slot from it; OBJ imports leave it empty.
    std::vector<std::array<std::filesystem::path,material::input_count>> textures;
};
// Wavefront OBJ/MTL or glTF 2.0 (.gltf with external buffers and images).
bool is_mesh_path(const std::filesystem::path&);
// Throws with context on invalid input. All dependencies must remain inside
// the source directory, including after resolving links. No disk mutations.
MeshImport load_mesh(const std::filesystem::path& source,ImageCache* images = nullptr);
MeshImport load_obj(const std::filesystem::path& source,ImageCache* images = nullptr);
MeshImport load_gltf(const std::filesystem::path& source,ImageCache* images = nullptr);
}
