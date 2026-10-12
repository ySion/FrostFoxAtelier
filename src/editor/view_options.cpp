#include "editor/view_options.hpp"
#include <array>
#include <cmath>
#include <type_traits>

namespace atelier::editor {
namespace {
using K = ViewValueKind;
constexpr std::array<std::string_view, 4> shading{"smooth", "flat", "matcap", "scene_lit"};
constexpr std::array<std::string_view, 3> wire{"off", "overlay", "only"};
constexpr std::array<std::string_view, 2> background{"none", "gradient"};
constexpr std::array<std::string_view, 3> plane{"xz", "xy", "yz"};
constexpr std::array<std::string_view, 4> aa{"off", "taa", "smaa", "smaa_t2x"};
constexpr std::array<std::string_view, 5> probes{"off", "irradiance", "state", "update_age", "distance"};
#define FIELD(id, kind, low, high) ViewOptionInfo{ViewOption::id, #id, K::kind, low, high}
constexpr std::array options{
    ViewOptionInfo{ViewOption::shading,"shading",K::enumeration,0,3,shading},
    ViewOptionInfo{ViewOption::wireframe,"wireframe",K::enumeration,0,2,wire},
    FIELD(hidden_lines,boolean,0,1),FIELD(triangle_edges,boolean,0,1),FIELD(wire_width,number,.25,16),FIELD(wire_opacity,number,0,1),
    ViewOptionInfo{ViewOption::background,"background",K::enumeration,0,1,background},
    FIELD(reference_grid,boolean,0,1),FIELD(origin_axes,boolean,0,1),FIELD(grid_spacing,number,.0001f,10000),
    ViewOptionInfo{ViewOption::grid_plane,"grid_plane",K::enumeration,0,2,plane},
    FIELD(shadows,boolean,0,1),FIELD(ddgi,boolean,0,1),FIELD(gtao,boolean,0,1),FIELD(sssr,boolean,0,1),
    FIELD(bloom,boolean,0,1),FIELD(ssil,boolean,0,1),
    ViewOptionInfo{ViewOption::antialiasing,"antialiasing",K::enumeration,0,3,aa},
    FIELD(exposure_ev,number,-16,16),FIELD(shadow_bias,number,.000001f,.1f),FIELD(shadow_light_budget,integer,0,16),
    FIELD(shadow_samples,integer,1,64),FIELD(ao_radius,number,.001f,100),FIELD(reflection_distance,number,.001f,10000),
    FIELD(reflection_roughness,number,0,1),
    FIELD(probe_min_x,number,-100000,100000),FIELD(probe_min_y,number,-100000,100000),FIELD(probe_min_z,number,-100000,100000),
    FIELD(probe_max_x,number,-100000,100000),FIELD(probe_max_y,number,-100000,100000),FIELD(probe_max_z,number,-100000,100000),
    FIELD(probe_count_x,integer,2,32),FIELD(probe_count_y,integer,2,32),FIELD(probe_count_z,integer,2,32),
    FIELD(probe_update_budget,integer,1,4096),FIELD(probe_rays,integer,1,256),FIELD(probe_debug,boolean,0,1),
    ViewOptionInfo{ViewOption::probe_visualization,"probe_visualization",K::enumeration,0,4,probes},
    FIELD(probe_volume,boolean,0,1),FIELD(probe_relocation,boolean,0,1),FIELD(probe_xray,boolean,0,1),FIELD(probe_radius,number,.01f,.45f),
    FIELD(cavity,boolean,0,1),FIELD(cavity_ridge,number,0,2),FIELD(cavity_valley,number,0,2),FIELD(cavity_radius,number,.5,4)
};
#undef FIELD
auto invalid(std::string message) { return std::unexpected(Error{ErrorCode::invalid_definition,std::move(message)}); }
}
std::span<const ViewOptionInfo> view_options() noexcept { return options; }
const ViewOptionInfo* view_option(std::string_view name) noexcept { for (const auto& value:options) if(value.name==name) return &value; return nullptr; }
double view_value(const renderer::ViewSettings& view, ViewOption option) noexcept {
    switch(option) {
#define VALUE(id, path) case ViewOption::id:return static_cast<double>(view.path)
    VALUE(shading,shading);VALUE(wireframe,wireframe);VALUE(hidden_lines,hidden_lines);VALUE(triangle_edges,triangle_edges);
    VALUE(wire_width,wire_width);VALUE(wire_opacity,wire_opacity);VALUE(background,background);VALUE(reference_grid,reference_grid);
    VALUE(origin_axes,origin_axes);VALUE(grid_spacing,grid_spacing);VALUE(grid_plane,grid_plane);
    VALUE(shadows,lit.shadows);VALUE(ddgi,lit.ddgi);VALUE(gtao,lit.gtao);VALUE(sssr,lit.sssr);VALUE(bloom,lit.bloom);VALUE(ssil,lit.ssil);
    VALUE(antialiasing,lit.antialiasing);VALUE(exposure_ev,lit.exposure_ev);VALUE(shadow_bias,lit.shadow_bias);
    VALUE(shadow_light_budget,lit.shadow_light_budget);VALUE(shadow_samples,lit.shadow_samples);VALUE(ao_radius,lit.ao_radius);
    VALUE(reflection_distance,lit.reflection_distance);VALUE(reflection_roughness,lit.reflection_roughness);
    VALUE(probe_min_x,lit.probe_min[0]);VALUE(probe_min_y,lit.probe_min[1]);VALUE(probe_min_z,lit.probe_min[2]);
    VALUE(probe_max_x,lit.probe_max[0]);VALUE(probe_max_y,lit.probe_max[1]);VALUE(probe_max_z,lit.probe_max[2]);
    VALUE(probe_count_x,lit.probe_counts[0]);VALUE(probe_count_y,lit.probe_counts[1]);VALUE(probe_count_z,lit.probe_counts[2]);
    VALUE(probe_update_budget,lit.probe_update_budget);VALUE(probe_rays,lit.probe_rays);VALUE(probe_debug,lit.probe_debug);
    VALUE(probe_visualization,lit.probe_visualization);VALUE(probe_volume,lit.probe_volume);VALUE(probe_relocation,lit.probe_relocation);
    VALUE(probe_xray,lit.probe_xray);VALUE(probe_radius,lit.probe_radius);
    VALUE(cavity,cavity.enabled);VALUE(cavity_ridge,cavity.ridge);VALUE(cavity_valley,cavity.valley);VALUE(cavity_radius,cavity.radius);
#undef VALUE
    }
    return 0;
}
Result<bool> set_view_option(renderer::ViewSettings& view, ViewOption option, double value) {
    const auto found=std::ranges::find(options,option,&ViewOptionInfo::option);
    if(found==options.end() || !std::isfinite(value) || value<found->minimum || value>found->maximum ||
        (found->kind!=K::number && value!=std::floor(value))) return invalid("Invalid display option value");
    const bool changed=view_value(view,option)!=value ||
        (value!=0 && ((option==ViewOption::gtao && view.lit.ssil) || (option==ViewOption::ssil && view.lit.gtao)));
    switch(option) {
#define SET(id, path) case ViewOption::id:view.path=static_cast<std::remove_cvref_t<decltype(view.path)>>(value);break
    SET(shading,shading);SET(wireframe,wireframe);SET(hidden_lines,hidden_lines);SET(triangle_edges,triangle_edges);
    SET(wire_width,wire_width);SET(wire_opacity,wire_opacity);SET(background,background);SET(reference_grid,reference_grid);
    SET(origin_axes,origin_axes);SET(grid_spacing,grid_spacing);SET(grid_plane,grid_plane);
    SET(shadows,lit.shadows);SET(ddgi,lit.ddgi);SET(gtao,lit.gtao);SET(sssr,lit.sssr);SET(bloom,lit.bloom);SET(ssil,lit.ssil);
    SET(antialiasing,lit.antialiasing);SET(exposure_ev,lit.exposure_ev);SET(shadow_bias,lit.shadow_bias);
    SET(shadow_light_budget,lit.shadow_light_budget);SET(shadow_samples,lit.shadow_samples);SET(ao_radius,lit.ao_radius);
    SET(reflection_distance,lit.reflection_distance);SET(reflection_roughness,lit.reflection_roughness);
    SET(probe_min_x,lit.probe_min[0]);SET(probe_min_y,lit.probe_min[1]);SET(probe_min_z,lit.probe_min[2]);
    SET(probe_max_x,lit.probe_max[0]);SET(probe_max_y,lit.probe_max[1]);SET(probe_max_z,lit.probe_max[2]);
    SET(probe_count_x,lit.probe_counts[0]);SET(probe_count_y,lit.probe_counts[1]);SET(probe_count_z,lit.probe_counts[2]);
    SET(probe_update_budget,lit.probe_update_budget);SET(probe_rays,lit.probe_rays);SET(probe_debug,lit.probe_debug);
    SET(probe_visualization,lit.probe_visualization);SET(probe_volume,lit.probe_volume);SET(probe_relocation,lit.probe_relocation);
    SET(probe_xray,lit.probe_xray);SET(probe_radius,lit.probe_radius);
    SET(cavity,cavity.enabled);SET(cavity_ridge,cavity.ridge);SET(cavity_valley,cavity.valley);SET(cavity_radius,cavity.radius);
#undef SET
    }
    if(value!=0) {if(option==ViewOption::gtao) view.lit.ssil=false;else if(option==ViewOption::ssil) view.lit.gtao=false;}
    return changed;
}
Result<void> validate_view_options(const renderer::ViewSettings& view) {
    for(const auto& option:options) {
        const auto value=view_value(view,option.option);
        if(!std::isfinite(value) || value<option.minimum || value>option.maximum || (option.kind!=K::number && value!=std::floor(value)))
            return invalid("Invalid display option: "+std::string(option.name));
    }
    for(std::size_t axis=0;axis<3;++axis) if(view.lit.probe_min[axis]>=view.lit.probe_max[axis]) return invalid("Probe bounds must have positive extent");
    const auto count=std::uint64_t(view.lit.probe_counts[0])*view.lit.probe_counts[1]*view.lit.probe_counts[2];
    if(count>4096) return invalid("Probe grid exceeds 4096 probes");
    if(view.lit.gtao && view.lit.ssil) return invalid("GTAO and Local GI are mutually exclusive");
    return {};
}
}
