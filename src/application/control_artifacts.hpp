#pragma once
#include "control/control.hpp"
#include "geometry/mesh.hpp"
#include "renderer/renderer.hpp"
#include "language/snapshot.hpp"
#include "assets/assets.hpp"

namespace atelier::editor {class Session;}
namespace atelier::application::control_artifacts {
struct Repro {
    std::filesystem::path project;
    // The project's folders, and the project-relative files of the assets the scene uses.
    std::vector<std::filesystem::path> roots, files;
    std::string scene;
    std::shared_ptr<const language::SourceSnapshot> sources;
    std::optional<language::SourceFile> ui_source;
    std::uint64_t publication_version{};
    // Distinct loaded versions, including slot defaults not serialized in the scene.
    std::vector<assets::MeshAsset> meshes;
    std::vector<std::pair<std::string,rendering::SkyMapRef>> skies;
    std::vector<assets::MaterialAsset> materials;
    // Exact loaded MS text and active input pixels, not file rereads or history.
    // These values have no Runtime roots and can outlive the source document.
    std::vector<language::SourceFile> material_sources;
    std::vector<assets::TextureAsset> textures;
    // Typed scene/default/parent closure, including unavailable author references.
    // Dormant or declaration-mismatched texture entries are not dependencies.
    std::vector<std::pair<assets::AssetKind,std::string>> references;
    control::Json material_resources;
    std::vector<assets::EngineDependency> engine_assets;
    control::Json manifest, drafts;
};
struct Sample {renderer::Capture pixels;control::Json metadata;};
// Owner-thread snapshots of actual Scene roots, never all cached material drafts.
// The same collection admits a repro candidate before replacing the document.
// These entry points adapt provider failures to control::Failure(storage_error).
Repro snapshot_resources(const editor::Session&);
void validate_candidate_assets(const editor::Session&,const control::Json& manifest);
// Inputs are owned snapshots/immutable resources. Cancellation is checked
// between bounded file operations. manifest.json is the completion marker.
control::Json export_repro(const std::filesystem::path&,Repro,const std::atomic_bool& cancel);
// Validate the complete dependency set before publishing a replacement document.
control::Json validate_repro(const std::filesystem::path&);
control::Json write_samples(const std::filesystem::path&,std::vector<Sample>,control::Json,const std::atomic_bool& cancel);
}
