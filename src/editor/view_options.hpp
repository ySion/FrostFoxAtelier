#pragma once
#include "renderer/view_settings.hpp"
#include "runtime/runtime.hpp"
#include <span>
#include <string_view>

namespace atelier::editor {
enum class ViewOption {
    shading, wireframe, hidden_lines, triangle_edges, wire_width, wire_opacity,
    background, reference_grid, origin_axes, grid_spacing, grid_plane,
    shadows, ddgi, gtao, sssr, bloom, ssil, antialiasing, exposure_ev,
    shadow_bias, shadow_light_budget, shadow_samples, ao_radius, reflection_distance, reflection_roughness,
    probe_min_x, probe_min_y, probe_min_z, probe_max_x, probe_max_y, probe_max_z,
    probe_count_x, probe_count_y, probe_count_z, probe_update_budget, probe_rays, probe_debug,
    cavity, cavity_ridge, cavity_valley, cavity_radius
};
enum class ViewValueKind { boolean, number, integer, enumeration };
struct ViewOptionInfo {
    ViewOption option;
    std::string_view name;
    ViewValueKind kind;
    double minimum{}, maximum{};
    std::span<const std::string_view> choices{};
};
std::span<const ViewOptionInfo> view_options() noexcept;
const ViewOptionInfo* view_option(std::string_view) noexcept;
double view_value(const renderer::ViewSettings&, ViewOption) noexcept;
// UI and control callers use the same typed operation. Enabling one near-field
// policy disables the other. Callers applying batches publish a complete candidate.
Result<bool> set_view_option(renderer::ViewSettings&, ViewOption, double);
Result<void> validate_view_options(const renderer::ViewSettings&);
}
