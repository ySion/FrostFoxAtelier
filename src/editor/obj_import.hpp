#pragma once
#include "geometry/mesh.hpp"
#include <filesystem>
#include <string>
#include <vector>
#include <unordered_map>

namespace atelier::editor {
struct ObjFile { std::filesystem::path relative; std::string bytes; };
struct ObjImport {
    geometry::MeshRef mesh;
    // One immutable read per dependency, relative to the OBJ's directory.
    // Import publishes these exact bytes; project refresh tracks the same paths.
    std::vector<ObjFile> files;
};
// Project-owned cache: identical source bytes share immutable decoded pixels.
// Weak ownership allows retired meshes and their images to leave independently.
struct ObjImageCache {
    struct Entry { std::string bytes; std::weak_ptr<const geometry::Image> image; };
    std::unordered_map<std::filesystem::path,Entry> entries;
};
// Throws with context on invalid input. All dependencies must remain inside
// the OBJ directory, including after resolving links. No disk mutations.
ObjImport load_obj(const std::filesystem::path& source,ObjImageCache* images = nullptr);
}
