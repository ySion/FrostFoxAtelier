#pragma once

#include <cstdint>
#include <array>

namespace atelier::renderer {

enum class Shading : std::uint32_t { smooth, flat, matcap, scene_lit };
enum class Wireframe : std::uint32_t { off, overlay, only };
enum class Background : std::uint32_t { none, gradient };
enum class GridPlane : std::uint32_t { xz, xy, yz };
enum class Antialiasing : std::uint32_t { none, taa, smaa, smaa_t2x };
enum class ProbeVisualization : std::uint32_t { off, irradiance, state, update_age, distance };

struct CavitySettings {
    bool operator==(const CavitySettings&) const = default;
    bool enabled{true};
    float ridge{1.f}, valley{1.f}; // Independent convex/concave contrast, 0..2.
    float radius{1.f}; // Screen-space radius, 0.5..4 physical pixels.
};

struct SceneLitSettings {
    bool operator==(const SceneLitSettings&) const = default;
    bool shadows{true};
    bool ddgi{true};
    bool gtao{true};
    bool sssr{true};
    bool bloom{true};
    bool ssil{false}; // Bounded screen-space near-field correction; takes precedence over GTAO.
    Antialiasing antialiasing{Antialiasing::smaa};
    float exposure_ev{}; // Display exposure; HDR histories remain scene linear.
    float shadow_bias{0.002f}; // World meters.
    std::uint32_t shadow_light_budget{4}; // Explicit maximum simultaneously traced lights.
    std::uint32_t shadow_samples{1}; // 1..64; deterministic reference uses 64.
    float ao_radius{0.35f};
    float reflection_distance{30.f};
    float reflection_roughness{0.6f};
    std::array<float, 3> probe_min{-6.f, -1.f, -6.f};
    std::array<float, 3> probe_max{6.f, 5.f, 6.f};
    std::array<std::uint32_t, 3> probe_counts{8, 4, 8};
    std::uint32_t probe_update_budget{64}; // Probes per frame, including initialization.
    std::uint32_t probe_rays{128}; // At most 256 rays per probe update.
    bool probe_debug{}; // Actual receiver coverage, independent of the lighting histories.
    ProbeVisualization probe_visualization{ProbeVisualization::off};
    bool probe_volume{}, probe_relocation{}, probe_xray{};
    float probe_radius{.12f}; // Fraction of minimum grid spacing; display geometry only.
    bool probe_overlay() const noexcept {
        return probe_volume || probe_relocation || probe_visualization != ProbeVisualization::off;
    }
};

// Per-view display policy, independent of World materials and simulation state.
// Width is in physical pixels; UI DPI must not be applied a second time.
struct ViewSettings {
    bool operator==(const ViewSettings&) const = default;
    Shading shading{Shading::smooth};
    Wireframe wireframe{Wireframe::off};
    bool hidden_lines{true}; // In wire-only mode, hide edges behind scene surfaces.
    bool triangle_edges{};   // Otherwise respect the producer's polygon edge mask.
    float wire_width{1.25f};
    float wire_opacity{0.7f};
    // Studio background preference. Scene Lit always resolves a black background.
    // Reference grid and axes remain independent of shading.
    Background background{Background::none};
    bool reference_grid{};
    bool origin_axes{};
    float grid_spacing{.2f};
    GridPlane grid_plane{GridPlane::xz};
    CavitySettings cavity{}; // Studio geometry only; independent of lighting and materials.
    SceneLitSettings lit{};
};

} // namespace atelier::renderer
