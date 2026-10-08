#pragma once
#include "geometry/mesh.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace atelier::editor {
struct ObjFile { std::filesystem::path relative; std::string bytes; };
struct ObjImport {
    geometry::MeshRef mesh;
    // One immutable read per dependency, relative to the OBJ's directory.
    // Import publishes these exact bytes; project refresh tracks the same paths.
    std::vector<ObjFile> files;
};
// Throws with context on invalid input. All dependencies must remain inside
// the OBJ directory, including after resolving links. No disk mutations.
ObjImport load_obj(const std::filesystem::path& source);
}
