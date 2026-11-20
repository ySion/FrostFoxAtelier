#pragma once

#include "editor/project.hpp"
#include "editor/session.hpp"
#include "editor/viewport_tool.hpp"
#include "procedural/geometry.hpp"
#include "renderer/overlay.hpp"
#include "renderer/renderer.hpp"
#include <memory>
#include <optional>
#include <vector>

namespace atelier::application {

struct SopGuideOptions {
    bool show_points{};

    bool show_normals{};

    bool show_orient{};

    bool show_polylines{true};
};

[[nodiscard]] geometry::MeshRef build_procedural_surface_mesh(const procedural::Geometry& geometry);

void append_procedural_guides(const procedural::Geometry& geometry, const editor::ViewportView& view,
                              const SopGuideOptions& options, std::vector<renderer::OverlayShape>& overlay);

class ProceduralPreview final {
public:
    struct Draw {
        std::vector<renderer::MeshInstance> meshes;

        std::uint64_t identity{1};

        std::shared_ptr<const editor::ViewportScene> picking;

        std::shared_ptr<renderer::MaterialBatch> materials;

        std::shared_ptr<const procedural::Geometry> shown_geometry;
    };

    void reset() noexcept;

    Result<void> refresh(editor::Session& session, editor::Project* project,
                         std::shared_ptr<const procedural::Geometry> shown, std::uint64_t generation);

    renderer::Result<renderer::Preparation> prepare(renderer::Renderer& renderer);

    const Draw* draw() const noexcept { return adopted_ ? &adopted_->draw : nullptr; }

private:
    struct Prepared {
        std::uint64_t request{};

        Draw draw;

        std::optional<renderer::Error> failure;
    };

    std::uint64_t identity_{500000};

    std::uint64_t request_{};

    std::shared_ptr<const procedural::Geometry> cached_shown_;

    std::uint64_t cached_generation_{};

    geometry::MeshRef cached_surface_mesh_;

    std::vector<renderer::MeshInstance> meshes_;

    std::vector<material::ProgramSurfaceRef> surfaces_;

    std::shared_ptr<const editor::ViewportScene> picking_;

    std::optional<Prepared> pending_;

    std::optional<Prepared> adopted_;
};

} // namespace atelier::application
