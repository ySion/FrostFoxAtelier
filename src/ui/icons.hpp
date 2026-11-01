#pragma once
#include <cstdint>
#include <span>

namespace atelier::ui {
enum class Icon {
    pointer,
    translate,
    rotate,
    scale,
    transform,
    cursor3d,
    magnet,
    orbit,
    pan,
    fly,
    point_light,
    area_light,
    sun_light,
    sky_light,
    mesh,
    object,
    component,
    world,
    folder,
    search,
    shade_wire,
    shade_solid,
    shade_matcap,
    shade_lit,
    overlays,
    select_new,
    select_extend,
    select_subtract,
    select_invert,
    select_intersect,
    grid,
    axes_origin,
    wire_overlay,
    lights_off,
    fps,
    color_picker,
    rename,
    duplicate,
    trash,
    unparent
};

// Rasterizes the bundled Tabler mask into tightly packed R8 alpha. Requires
// 1..256 pixels and exactly pixels*pixels output bytes; throws on invalid input.
// Call on a cache miss, never per frame. No window or text backend is required.
void raster_icon(Icon, std::uint32_t pixels, std::span<std::uint8_t> alpha);
}
