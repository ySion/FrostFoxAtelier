#pragma once
#include "renderer/view_settings.hpp"
#include "runtime/runtime.hpp"
#include <span>
#include <string_view>

namespace atelier::editor {
enum class ProbeVolumeField {
    enabled,
    placement,
    min_x,
    min_y,
    min_z,
    max_x,
    max_y,
    max_z,
    count_x,
    count_y,
    count_z,
    blend_cells,
    count
};
enum class ViewOption {
    shading,
    wireframe,
    hidden_lines,
    triangle_edges,
    wire_width,
    wire_opacity,
    background,
    reference_grid,
    origin_axes,
    grid_spacing,
    grid_plane,
    shadows,
    ddgi,
    gtao,
    sssr,
    bloom,
    antialiasing,
    exposure_ev,
    direct_candidates,
    ao_radius,
    reflection_distance,
    reflection_roughness,
    probe_update_budget,
    probe_rays,
    probe_debug,
    probe_bounce,
    indirect_gain,
    probe_feedback_visibility,
    probe_visualization,
    probe_volume,
    probe_relocation,
    probe_xray,
    probe_radius,
    probe_coverage,
    probe_debug_volume,
    spatial_light_sampling,
    direct_cluster_inspect,
    probe_history,
    probe_inspect,
    probe_inspect_x,
    probe_inspect_y,
    probe_inspect_source,
    cavity,
    cavity_ridge,
    cavity_valley,
    cavity_radius,
    volume_first,
    volume_last = volume_first + 4 * static_cast<int>(ProbeVolumeField::count) - 1,
    sky_enabled,
    sky_intensity,
    sky_color_x,
    sky_color_y,
    sky_color_z
};
constexpr ViewOption probe_volume_option(unsigned slot, ProbeVolumeField field) noexcept {
    return static_cast<ViewOption>(static_cast<unsigned>(ViewOption::volume_first) +
                                   slot * static_cast<unsigned>(ProbeVolumeField::count) +
                                   static_cast<unsigned>(field));
}
enum class ViewValueKind { boolean, number, integer, enumeration };
struct ViewOptionInfo {
    ViewOption option;
    std::string_view name;
    ViewValueKind kind;
    double minimum{}, maximum{};
    std::span<const std::string_view> choices{};
    // Presentation and Control documentation share this table. These strings
    // describe the option; they are not part of its persisted name or value.
    std::string_view label, category, description;
};
std::span<const ViewOptionInfo> view_options() noexcept;
const ViewOptionInfo* view_option(ViewOption) noexcept;
const ViewOptionInfo* view_option(std::string_view) noexcept;
struct ViewOptionAlias {
    std::string_view name;
    ViewOption option;
};
std::span<const ViewOptionAlias> view_option_aliases() noexcept;
bool world_option(ViewOption) noexcept;
double view_value(const renderer::ViewSettings&, ViewOption) noexcept;
// UI and control callers use the same typed operation. Callers applying batches
// publish a complete candidate.
Result<bool> set_view_option(renderer::ViewSettings&, ViewOption, double);
Result<void> validate_view_options(const renderer::ViewSettings&, std::array<float, 3> focus = {});
}
