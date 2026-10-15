#include "ui/icons.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace atelier::ui {
namespace {
struct Run { std::uint16_t count; std::uint8_t alpha; };
#include "ui/icon_data.inl"
std::span<const Run> data(Icon icon) {
    switch (icon) {
    case Icon::pointer:return pointer_runs;
    case Icon::translate:return translate_runs;
    case Icon::rotate:return rotate_runs;
    case Icon::scale:return scale_runs;
    case Icon::transform:return transform_runs;
    case Icon::cursor3d:return cursor3d_runs;
    case Icon::magnet:return magnet_runs;
    case Icon::orbit:return orbit_runs;
    case Icon::pan:return pan_runs;
    case Icon::fly:return fly_runs;
    case Icon::point_light:return point_light_runs;
    case Icon::area_light:return area_light_runs;
    case Icon::sun_light:return sun_light_runs;
    case Icon::sky_light:return sky_light_runs;
    case Icon::mesh:return mesh_runs;
    case Icon::object:return object_runs;
    case Icon::component:return component_runs;
    case Icon::world:return world_runs;
    case Icon::folder:return folder_runs;
    case Icon::search:return search_runs;
    case Icon::shade_wire:return shade_wire_runs;
    case Icon::shade_solid:return shade_solid_runs;
    case Icon::shade_matcap:return shade_matcap_runs;
    case Icon::shade_lit:return shade_lit_runs;
    case Icon::overlays:return overlays_runs;
    case Icon::select_new:return select_new_runs;
    case Icon::select_extend:return select_extend_runs;
    case Icon::select_subtract:return select_subtract_runs;
    case Icon::select_invert:return select_invert_runs;
    case Icon::select_intersect:return select_intersect_runs;
    case Icon::grid:return grid_runs;
    case Icon::axes_origin:return axes_origin_runs;
    case Icon::wire_overlay:return wire_overlay_runs;
    case Icon::lights_off:return lights_off_runs;
    case Icon::fps:return fps_runs;
    case Icon::color_picker:return color_picker_runs;
    case Icon::rename:return rename_runs;
    case Icon::duplicate:return duplicate_runs;
    case Icon::trash:return trash_runs;
    case Icon::unparent:return unparent_runs;
    }
    throw std::invalid_argument("Unknown UI icon");
}
}
void raster_icon(Icon icon, std::uint32_t pixels, std::span<std::uint8_t> alpha) {
    if (!pixels || pixels>256 || alpha.size()!=std::size_t(pixels)*pixels)
        throw std::invalid_argument("Invalid UI icon raster extent");
    constexpr unsigned source_size=icon_source_size;
    std::array<std::uint8_t,source_size*source_size> source;
    auto destination=source.begin();
    for (const auto run:data(icon)) destination=std::fill_n(destination,run.count,run.alpha);
    const double ratio=double(source_size)/pixels;
    // Area sampling preserves thin strokes when shrinking the source mask.
    for (unsigned y=0;y<pixels;++y) for (unsigned x=0;x<pixels;++x) {
        const double left=x*ratio,right=(x+1)*ratio,top=y*ratio,bottom=(y+1)*ratio;
        double sum=0;
        for (unsigned sy=unsigned(top);sy<std::min(source_size,unsigned(std::ceil(bottom)));++sy)
            for (unsigned sx=unsigned(left);sx<std::min(source_size,unsigned(std::ceil(right)));++sx)
                sum+=source[sy*source_size+sx]*(std::min(right,double(sx+1))-std::max(left,double(sx)))*
                    (std::min(bottom,double(sy+1))-std::max(top,double(sy)));
        alpha[std::size_t(y)*pixels+x]=static_cast<std::uint8_t>(std::clamp(std::lround(sum/(ratio*ratio)),0l,255l));
    }
}
}
