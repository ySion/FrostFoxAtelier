#pragma once
#include "assets/assets.hpp"
#include "assets/image_decode.hpp"
#include "foundation/array.hpp"
#include "foundation/optional.hpp"
#include "foundation/string.hpp"
#include "geometry/mesh.hpp"
#include "runtime/runtime.hpp"
#include <array>
#include <cstddef>
#include <filesystem>

namespace atelier::assets {
inline constexpr std::array<StringView, 6> imported_material_inputs{"base_color", "roughness", "metallic",
                                                                    "normal",     "emissive",  "opacity"};

struct ImportedFile {
    std::filesystem::path relative;

    Array<std::byte> bytes;
};

struct MeshImport {
    geometry::MeshRef mesh;

    // One immutable read per dependency, relative to the source file's directory.
    // Import publishes these exact bytes; project refresh tracks the same paths.
    Array<ImportedFile> files;

    Array<String> notes; // Source content the import left out, one line each.

    // Both arrays align with mesh->materials(). An absent author material is
    // nullopt, never inferred from white/default parameter values.
    Array<Optional<MaterialInstanceAsset>> materials;

    // StandardPBR input order above. Each nonempty path names an entry in files;
    // its <input>_texture override has an empty key until Project assigns a GUID.
    Array<std::array<std::filesystem::path, imported_material_inputs.size()>> textures;
};

// Wavefront OBJ/MTL or glTF 2.0 (.gltf with external buffers and images).
bool is_mesh_path(const std::filesystem::path&);

// IO/encoding, OBJ/glTF format/schema, image decoding, unsupported extension and
// geometry construction/authored tangent failures return invalid_storage.
// Dependencies stay inside the source directory,
// including after resolving links. No disk mutations or partial publication.
Result<MeshImport> load_mesh(const std::filesystem::path& source, ImageCache* images = nullptr);

Result<MeshImport> load_obj(const std::filesystem::path& source, ImageCache* images = nullptr);

Result<MeshImport> load_gltf(const std::filesystem::path& source, ImageCache* images = nullptr);
}
