#pragma once

#include <cstdint>

namespace atelier::renderer {

enum class Shading : std::uint32_t { smooth, flat, matcap };
enum class Wireframe : std::uint32_t { off, overlay, only };
enum class Background : std::uint32_t { none, gradient };

// Per-view display policy, independent of World materials and simulation state.
// Width is in physical pixels; UI DPI must not be applied a second time.
struct ViewSettings {
    Shading shading{Shading::smooth};
    Wireframe wireframe{Wireframe::off};
    bool hidden_lines{true}; // In wire-only mode, hide edges behind scene surfaces.
    bool triangle_edges{};   // Otherwise respect the producer's polygon edge mask.
    float wire_width{1.25f};
    float wire_opacity{0.7f};
    // Auxiliary display is independent of shading, including future scene lighting.
    Background background{Background::none};
    bool reference_grid{};
    bool origin_axes{};
};

} // namespace atelier::renderer
