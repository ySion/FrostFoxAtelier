#pragma once

#include "ui/frame.hpp"

#include <array>
#include <cstdint>

namespace atelier::renderer {

// Screen-space viewport annotation (handles, selection marquee, locators),
// drawn after the scene and before UI, clipped to the scene viewport. Physical
// pixels; never depth tested and never part of capture depth.
struct OverlayShape {
    enum class Kind : std::uint32_t { segment, rectangle };

    Kind kind{Kind::segment};

    // segment: anti-aliased round-capped stroke a=(x0,y0) to b=(x1,y1); equal points draw a disc.
    // rectangle: x, y, width, height with an inside border.
    std::array<float, 4> points{};

    float width{1}; // Segment stroke width or rectangle border width.

    ui::Color color{1, 1, 1, 1};

    ui::Color border{0, 0, 0, 0}; // Rectangle only.
};

} // namespace atelier::renderer
