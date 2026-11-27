#pragma once
#include "renderer/view_settings.hpp"
#include "runtime/runtime.hpp"
#include <span>
#include <string_view>

namespace atelier::editor {
enum class ViewOption {
    shading,
    wireframe,
    hidden_lines,
    triangle_edges,
    backface_tint,
    wire_width,
    wire_opacity,
    background,
    reference_grid,
    origin_axes,
    show_locators,
    grid_spacing,
    grid_plane,
    shadows,
    contact_shadow_length,
    direct_candidates,
    indirect_lighting,
    gi_quality,
    indirect_gain,
    ssilvb,
    ao_radius,
    reflections,
    reflection_distance,
    reflection_roughness,
    sky_mode,
    sky_intensity,
    sky_rotation,
    sky_color_x,
    sky_color_y,
    sky_color_z,
    fog_enabled,
    fog_density,
    fog_height_falloff,
    fog_anisotropy,
    exposure_ev,
    bloom,
    tonemapper,
    antialiasing,
    direct_cluster_inspect,
    cavity,
    cavity_ridge,
    cavity_valley,
    cavity_radius,
    last = cavity_radius
};

enum class ViewValueKind { boolean, number, integer, enumeration };

struct ViewOptionInfo {
    ViewOption option;

    std::string_view name;

    ViewValueKind kind;

    double minimum{};

    double maximum{};

    std::span<const std::string_view> choices{};

    // Presentation and Control documentation share this table. These strings
    // describe the option; they are not part of its persisted name or value.
    std::string_view label;

    std::string_view category;

    std::string_view description;
};

std::span<const ViewOptionInfo> view_options() noexcept;

const ViewOptionInfo* view_option(ViewOption) noexcept;

const ViewOptionInfo* view_option(std::string_view) noexcept;

bool world_option(ViewOption) noexcept;

double view_value(const renderer::ViewSettings&, ViewOption) noexcept;

// UI and control callers use the same typed operation. Callers applying batches
// publish a complete candidate.
Result<bool> set_view_option(renderer::ViewSettings&, ViewOption, double);

Result<void> validate_view_options(const renderer::ViewSettings&, math::Vector3 focus = {});
} // namespace atelier::editor
