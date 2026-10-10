#pragma once
#include "control/control.hpp"
#include "geometry/mesh.hpp"
#include "renderer/renderer.hpp"
#include "language/snapshot.hpp"

namespace atelier::application::control_artifacts {
struct Repro {
    std::filesystem::path project;
    std::string scene;
    std::shared_ptr<const language::SourceSnapshot> sources;
    std::optional<language::SourceFile> ui_source;
    std::uint64_t publication_version{};
    std::vector<std::pair<std::string,geometry::MeshRef>> meshes;
    control::Json manifest, drafts;
};
struct Sample {renderer::Capture pixels;control::Json metadata;};
// Inputs are owned snapshots/immutable resources. Cancellation is checked
// between bounded file operations. manifest.json is the completion marker.
control::Json export_repro(const std::filesystem::path&,Repro,const std::atomic_bool& cancel);
// Validate the complete dependency set before publishing a replacement document.
control::Json validate_repro(const std::filesystem::path&);
control::Json write_samples(const std::filesystem::path&,std::vector<Sample>,control::Json,const std::atomic_bool& cancel);
}
