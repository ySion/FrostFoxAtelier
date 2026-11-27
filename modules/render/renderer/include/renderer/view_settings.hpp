#pragma once

#include "rendering/scene_lighting.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace atelier::renderer {

enum class Shading : std::uint32_t { smooth, flat, matcap, scene_lit };

enum class Wireframe : std::uint32_t { off, overlay, only };

enum class Background : std::uint32_t { none, gradient };

enum class GridPlane : std::uint32_t { xz, xy, yz };

using rendering::Antialiasing;

using rendering::GiQuality;

using rendering::SkyMode;

using rendering::Tonemapper;

struct CavitySettings {
    bool operator==(const CavitySettings&) const = default;

    bool enabled{true};

    float ridge{1.f};

    float valley{1.f}; // Independent convex/concave contrast, 0..2.

    float radius{1.f}; // Screen-space radius, 0.5..4 physical pixels.
};

struct SceneLitSettings : rendering::SceneLightingSettings {
    bool operator==(const SceneLitSettings&) const = default;

    bool direct_cluster_inspect{}; // Optional receiver-support histogram; excluded from performance sampling.
};

// Evaluated frame settings: World authoring values plus per-view preferences.
// Per-view display policy, independent of World materials and simulation state.
// Width is in physical pixels; UI DPI must not be applied a second time.
struct ViewSettings {
    bool operator==(const ViewSettings&) const = default;

    Shading shading{Shading::smooth};

    Wireframe wireframe{Wireframe::off};

    bool hidden_lines{true}; // In wire-only mode, hide edges behind scene surfaces.

    bool triangle_edges{}; // Otherwise respect the producer's polygon edge mask.

    bool backface_tint{}; // Studio geometry only: highlight back-facing triangles to inspect winding.

    float wire_width{1.25f};

    float wire_opacity{0.7f};

    // Studio background preference. Scene Lit resolves sky/black background.
    // Reference grid, axes and locators remain independent of shading.
    Background background{Background::none};

    bool reference_grid{};

    bool origin_axes{};

    bool show_locators{true};

    float grid_spacing{.2f};

    GridPlane grid_plane{GridPlane::xz};

    CavitySettings cavity{}; // Studio geometry only; independent of lighting and materials.

    SceneLitSettings lit{};
};

} // namespace atelier::renderer
