#pragma once

#include "rendering/scene_lighting.hpp"
#include <cstdint>
#include <array>
#include <algorithm>
#include <cmath>

namespace atelier::renderer {

enum class Shading : std::uint32_t { smooth, flat, matcap, scene_lit };
enum class Wireframe : std::uint32_t { off, overlay, only };
enum class Background : std::uint32_t { none, gradient };
enum class GridPlane : std::uint32_t { xz, xy, yz };
using rendering::Antialiasing;
enum class ProbeVisualization : std::uint32_t { off, irradiance, state, update_age, distance };
using rendering::ProbePlacement;
enum class ProbeCoverage : std::uint32_t { status, volume, contributors, dominance };
enum class ProbeHistory : std::uint32_t { live, hold_irradiance, hold_distance };

using rendering::ProbeVolumeSettings;
using rendering::probe_volume_limit;
using rendering::probe_resident_limit;

struct CavitySettings {
    bool operator==(const CavitySettings&) const = default;
    bool enabled{true};
    float ridge{1.f}, valley{1.f}; // Independent convex/concave contrast, 0..2.
    float radius{1.f}; // Screen-space radius, 0.5..4 physical pixels.
};

struct SceneLitSettings : rendering::SceneLightingSettings {
    bool operator==(const SceneLitSettings&) const = default;
    bool direct_reference{}; // Diagnostic: average 64 independent RIS estimates without reuse or denoising.
    bool direct_clustered{true}; // Conservative analytic light lists with exact enumeration; false is the ReSTIR diagnostic fallback. Reference takes precedence.
    bool direct_shadow_cache{}; // Clustered candidate B only; A keeps exact endpoint RT visibility.
    bool direct_cluster_inspect{}; // Optional receiver-support histogram; excluded from performance sampling.
    bool spatial_light_sampling{true}; // DI/DDGI proposal diagnostic; never persisted in World.
    bool probe_feedback_visibility{true}; // False is a diagnostic moment-only comparison.
    ProbeHistory probe_history{ProbeHistory::live}; // Diagnostic isolation; never persisted in World.
    bool probe_inspect{}; // Bounded asynchronous inspection of one physical client pixel.
    std::array<std::uint32_t,2> probe_inspect_pixel{};
    std::int32_t probe_inspect_source{-1}; // Physical probe slot; -1 selects a receiver contributor automatically.
    bool probe_debug{}; // Actual receiver coverage, independent of the lighting histories.
    ProbeCoverage probe_coverage{ProbeCoverage::status};
    std::int32_t probe_debug_volume{-1}; // Geometry overlay selection; -1 draws all slots.
    ProbeVisualization probe_visualization{ProbeVisualization::off};
    bool probe_volume{}, probe_relocation{}, probe_xray{};
    float probe_radius{.12f}; // Fraction of minimum grid spacing; display geometry only.
    bool probe_overlay() const noexcept {
        return probe_volume || probe_relocation || probe_visualization != ProbeVisualization::off;
    }
};

inline const char* probe_settings_error(const SceneLitSettings& settings) noexcept {
    if(const auto* error=rendering::probe_layout_error(settings)) return error;
    if(settings.probe_coverage>ProbeCoverage::dominance || settings.probe_history>ProbeHistory::hold_distance || settings.probe_debug_volume < -1 ||
       settings.probe_debug_volume>=static_cast<std::int32_t>(probe_volume_limit) || settings.probe_inspect_source < -1 ||
       settings.probe_inspect_source>=static_cast<std::int32_t>(probe_resident_limit)) return "Invalid DDGI diagnostic selection";
    return nullptr;
}
inline const char* probe_focus_error(const SceneLitSettings& settings,const std::array<float,3>& focus) noexcept {
    if(const auto* error=probe_settings_error(settings)) return error;
    return rendering::scene_lighting_focus_error(settings,focus);
}

// Evaluated frame settings: World authoring values plus per-view preferences.
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
