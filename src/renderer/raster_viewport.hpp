#pragma once
#include "ui/frame.hpp"

#include <array>
#include <cmath>

namespace atelier::renderer::detail {
struct RasterViewport {
    ui::Rect hardware;
    ui::Clip scissor;
    std::array<float,4> clip_transform{1,1,0,0}; // XY scale, XY clip-w offset.
};

// D3D's implicit viewport scissor rounds fractional edges down. Enclose the
// logical rectangle with integer hardware edges, then preserve its pixel-center
// coverage and projection through an explicit scissor and final clip transform.
inline RasterViewport raster_viewport(const ui::Rect& logical) {
    const float left=std::floor(logical.x),top=std::floor(logical.y);
    const float right=std::ceil(logical.x+logical.width),bottom=std::ceil(logical.y+logical.height);
    RasterViewport result{{left,top,right-left,bottom-top},
        {static_cast<int>(std::ceil(logical.x-.5f)),static_cast<int>(std::ceil(logical.y-.5f)),
         static_cast<int>(std::ceil(logical.x+logical.width-.5f)),
         static_cast<int>(std::ceil(logical.y+logical.height-.5f))}};
    const double width=result.hardware.width,height=result.hardware.height;
    // A subpixel rectangle can have no pixel centers and a rounded zero extent.
    // Its empty scissor already suppresses output; avoid a nonfinite transform.
    if(width>0&&height>0)result.clip_transform={
        static_cast<float>(logical.width/width),static_cast<float>(logical.height/height),
        static_cast<float>((2*(double(logical.x)-left)+logical.width-width)/width),
        static_cast<float>((height-logical.height-2*(double(logical.y)-top))/height)};
    return result;
}
}
