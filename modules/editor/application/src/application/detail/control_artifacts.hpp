#pragma once
#include "control/control.hpp"
#include "foundation/array.hpp"
#include "foundation/optional.hpp"
#include "foundation/string.hpp"
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
    Array<std::filesystem::path> roots;

    Array<std::filesystem::path> files;

    String scene;

    std::shared_ptr<const language::SourceSnapshot> sources;

    Optional<language::SourceFile> ui_source;

    std::uint64_t publication_version{};

    // Distinct loaded versions, including slot defaults not serialized in the scene.
    Array<assets::MeshAsset> meshes;

    Array<std::pair<String, rendering::SkyMapRef>> skies;

    Array<assets::MaterialAsset> materials;

    // Open procedural graph snapshots so unsaved node/seed edits reproduce faithfully.
    Array<std::pair<String, String>> procedural_graphs;

    // Exact loaded MS text and active input pixels, not file rereads or history.
    // These values have no Runtime roots and can outlive the source document.
    Array<language::SourceFile> material_sources;

    Array<assets::TextureAsset> textures;

    // Typed scene/default/parent closure, including unavailable author references.
    // Dormant or declaration-mismatched texture entries are not dependencies.
    Array<std::pair<assets::AssetKind, String>> references;

    json::Document material_resources;

    Array<assets::EngineDependency> engine_assets;

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

    Array<assets::EngineDependency> engine_assets;

    editor::Project project;

    String scene_text;
};

std::expected<void, control::Failure> validate_candidate_assets(const editor::Session&, const ReproBundle&);

// Inputs are owned snapshots/immutable resources. Cancellation is checked
// between bounded file operations. manifest.json is the completion marker.
std::expected<json::Document, control::Failure> export_repro(const std::filesystem::path&, Repro, WriteState& work);

// Validate the complete dependency set before publishing a replacement document.
std::expected<ReproBundle, control::Failure> validate_repro(const std::filesystem::path&);

std::expected<json::Document, control::Failure> write_samples(const std::filesystem::path&, Array<Sample>,
                                                              json::Document, WriteState& work);

// No GPU readback. The worker receives only owned CPU observations and metadata.
std::expected<json::Document, control::Failure> write_trace(const std::filesystem::path&, rdg::TraceSnapshot,
                                                            json::Document, WriteState& work);
}
