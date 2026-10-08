#pragma once

#include <cstdint>
#include <array>

namespace atelier::renderer {

enum class Shading : std::uint32_t { smooth, flat, matcap, scene_lit };
enum class Wireframe : std::uint32_t { off, overlay, only };
enum class Background : std::uint32_t { none, gradient };
enum class GridPlane : std::uint32_t { xz, xy, yz };

struct SceneLitSettings {
    bool shadows{true};
    bool ddgi{true};
    bool gtao{true};
    bool sssr{true};
    bool taa{true};
    bool bloom{true};
    float exposure_ev{}; // Display exposure; HDR histories remain scene linear.
    float shadow_bias{0.002f}; // World meters.
    std::uint32_t shadow_light_budget{4}; // Explicit maximum simultaneously traced lights.
    std::uint32_t shadow_samples{1}; // 1..64; deterministic reference uses 64.
    float ao_radius{0.35f};
    float reflection_distance{30.f};
    float reflection_roughness{0.6f};
    std::array<float, 3> environment{0.025f, 0.035f, 0.055f}; // Linear HDR radiance.
    std::array<float, 3> probe_min{-6.f, -1.f, -6.f};
    std::array<float, 3> probe_max{6.f, 5.f, 6.f};
    std::array<std::uint32_t, 3> probe_counts{8, 4, 8};
    std::uint32_t probe_update_budget{32}; // Probes per frame, including initialization.
    std::uint32_t probe_rays{64};
    bool probe_debug{};
};

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
    float grid_spacing{.2f};
    GridPlane grid_plane{GridPlane::xz};
    SceneLitSettings lit{};
};

} // namespace atelier::renderer
