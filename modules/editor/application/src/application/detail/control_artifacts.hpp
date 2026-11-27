#pragma once
#include "control/control.hpp"
#include "geometry/mesh.hpp"
#include "renderer/renderer.hpp"
#include "language/snapshot.hpp"
#include "assets/assets.hpp"
#include "editor/project.hpp"

namespace atelier::editor {
class Session;
}

namespace atelier::application::control_artifacts {
struct Repro {
    std::filesystem::path project;

    // The project's folders, and the project-relative files of the assets the scene uses.
    std::vector<std::filesystem::path> roots;

    std::vector<std::filesystem::path> files;

    std::string scene;

    std::shared_ptr<const language::SourceSnapshot> sources;

    std::optional<language::SourceFile> ui_source;

    std::uint64_t publication_version{};

    // Distinct loaded versions, including slot defaults not serialized in the scene.
    std::vector<assets::MeshAsset> meshes;

    std::vector<std::pair<std::string, rendering::SkyMapRef>> skies;

    std::vector<assets::MaterialAsset> materials;

    // Open procedural graph snapshots so unsaved node/seed edits reproduce faithfully.
    std::vector<std::pair<std::string, std::string>> procedural_graphs;

    // Exact loaded MS text and active input pixels, not file rereads or history.
    // These values have no Runtime roots and can outlive the source document.
    std::vector<language::SourceFile> material_sources;

    std::vector<assets::TextureAsset> textures;

    // Typed scene/default/parent closure, including unavailable author references.
    // Dormant or declaration-mismatched texture entries are not dependencies.
    std::vector<std::pair<assets::AssetKind, std::string>> references;

    json::Document material_resources;

    std::vector<assets::EngineDependency> engine_assets;

    json::Document manifest;

    json::Document drafts;
};

// Cancellation and publication compete for the same job. Once the worker owns
// the outcome, the owner must report its Result, including a failed manifest write.
class WriteState {
public:
    bool cancel() { return claim(State::cancelled); }

    bool claim_outcome() { return claim(State::finishing); }

    bool cancelled() const { return state_.load() == State::cancelled; }

private:
    enum class State { running, cancelled, finishing };

    std::atomic<State> state_{State::running};

    bool claim(State next) {
        auto expected = State::running;
        return state_.compare_exchange_strong(expected, next);
    }
};

struct Sample {
    renderer::Capture pixels;

    json::Document metadata;
};

// Owner-thread snapshots of actual Scene roots, never all cached material drafts.
// The same collection admits a repro candidate before replacing the document.
// These entry points adapt provider failures to control::Failure(storage_error).
std::expected<Repro, control::Failure> snapshot_resources(const editor::Session&);

struct ReproBundle {
    json::Document manifest;

    std::vector<assets::EngineDependency> engine_assets;

    editor::Project project;

    std::string scene_text;
};

std::expected<void, control::Failure> validate_candidate_assets(const editor::Session&, const ReproBundle&);

// Inputs are owned snapshots/immutable resources. Cancellation is checked
// between bounded file operations. manifest.json is the completion marker.
std::expected<json::Document, control::Failure> export_repro(const std::filesystem::path&, Repro, WriteState& work);

// Validate the complete dependency set before publishing a replacement document.
std::expected<ReproBundle, control::Failure> validate_repro(const std::filesystem::path&);

std::expected<json::Document, control::Failure> write_samples(const std::filesystem::path&, std::vector<Sample>,
                                                              json::Document, WriteState& work);

// No GPU readback. The worker receives only owned CPU observations and metadata.
std::expected<json::Document, control::Failure> write_trace(const std::filesystem::path&, rdg::TraceSnapshot,
                                                            json::Document, WriteState& work);
}
