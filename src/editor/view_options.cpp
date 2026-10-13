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
constexpr std::array<std::string_view, 2> placement{"fixed","follow_focus"};
constexpr std::array<std::string_view, 4> coverage{"status","volume","contributors","dominance"};
#define FIELD(id, kind, low, high) ViewOptionInfo{ViewOption::id, #id, K::kind, low, high}
#define VFIELD(slot,id,kind,low,high) ViewOptionInfo{probe_volume_option(slot,ProbeVolumeField::id),"probe_volume_" #slot "_" #id,K::kind,low,high}
#define VOLUME(slot) VFIELD(slot,enabled,boolean,0,1), \
    ViewOptionInfo{probe_volume_option(slot,ProbeVolumeField::placement),"probe_volume_" #slot "_placement",K::enumeration,0,1,placement}, \
    VFIELD(slot,min_x,number,-100000,100000),VFIELD(slot,min_y,number,-100000,100000),VFIELD(slot,min_z,number,-100000,100000), \
    VFIELD(slot,max_x,number,-100000,100000),VFIELD(slot,max_y,number,-100000,100000),VFIELD(slot,max_z,number,-100000,100000), \
    VFIELD(slot,count_x,integer,2,32),VFIELD(slot,count_y,integer,2,32),VFIELD(slot,count_z,integer,2,32),VFIELD(slot,blend_cells,number,0,2)
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
    FIELD(exposure_ev,number,-16,16),FIELD(shadow_bias,number,.000001f,.1f),FIELD(direct_candidates,integer,1,32),
    FIELD(ao_radius,number,.001f,100),FIELD(reflection_distance,number,.001f,10000),
    FIELD(reflection_roughness,number,0,1),
    FIELD(probe_update_budget,integer,1,4096),FIELD(probe_rays,integer,1,256),FIELD(probe_debug,boolean,0,1),
    FIELD(probe_bounce,number,0,.99f),FIELD(indirect_gain,number,0,8),FIELD(probe_feedback_visibility,boolean,0,1),
    ViewOptionInfo{ViewOption::probe_visualization,"probe_visualization",K::enumeration,0,4,probes},
    FIELD(probe_volume,boolean,0,1),FIELD(probe_relocation,boolean,0,1),FIELD(probe_xray,boolean,0,1),FIELD(probe_radius,number,.01f,.45f),
    ViewOptionInfo{ViewOption::probe_coverage,"probe_coverage",K::enumeration,0,3,coverage},FIELD(probe_debug_volume,integer,-1,3),
    FIELD(direct_reference,boolean,0,1),
    FIELD(cavity,boolean,0,1),FIELD(cavity_ridge,number,0,2),FIELD(cavity_valley,number,0,2),FIELD(cavity_radius,number,.5,4),
    VOLUME(0),VOLUME(1),VOLUME(2),VOLUME(3)
};
#undef VOLUME
#undef VFIELD
#undef FIELD
#define ALIAS(id) ViewOptionAlias{"probe_" #id,probe_volume_option(0,ProbeVolumeField::id)}
constexpr std::array aliases{ALIAS(min_x),ALIAS(min_y),ALIAS(min_z),ALIAS(max_x),ALIAS(max_y),ALIAS(max_z),ALIAS(count_x),ALIAS(count_y),ALIAS(count_z)};
#undef ALIAS
constexpr bool volume_option(ViewOption option) {return option>=ViewOption::volume_first && option<=ViewOption::volume_last;}
constexpr unsigned volume_index(ViewOption option) {return (static_cast<unsigned>(option)-static_cast<unsigned>(ViewOption::volume_first))/static_cast<unsigned>(ProbeVolumeField::count);}
constexpr ProbeVolumeField volume_field(ViewOption option) {return static_cast<ProbeVolumeField>((static_cast<unsigned>(option)-static_cast<unsigned>(ViewOption::volume_first))%static_cast<unsigned>(ProbeVolumeField::count));}
auto invalid(std::string message) { return std::unexpected(Error{ErrorCode::invalid_definition,std::move(message)}); }
}
bool world_option(ViewOption option) noexcept {
    return (option>=ViewOption::shadows && option<=ViewOption::probe_rays) ||
        option==ViewOption::probe_bounce || option==ViewOption::indirect_gain || volume_option(option);
}
std::span<const ViewOptionInfo> view_options() noexcept { return options; }
std::span<const ViewOptionAlias> view_option_aliases() noexcept {return aliases;}
const ViewOptionInfo* view_option(std::string_view name) noexcept {
    for(const auto& value:options) if(value.name==name) return &value;
    for(const auto& alias:aliases) if(alias.name==name) {
        const auto found=std::ranges::find(options,alias.option,&ViewOptionInfo::option);
        return found==options.end()?nullptr:&*found;
    }
    return nullptr;
}
double view_value(const renderer::ViewSettings& view, ViewOption option) noexcept {
    if(volume_option(option)) {
        const auto& volume=view.lit.probe_volumes[volume_index(option)];
        switch(volume_field(option)) {
#define VVALUE(id,path) case ProbeVolumeField::id:return static_cast<double>(volume.path)
        VVALUE(enabled,enabled);VVALUE(placement,placement);VVALUE(min_x,minimum[0]);VVALUE(min_y,minimum[1]);VVALUE(min_z,minimum[2]);
        VVALUE(max_x,maximum[0]);VVALUE(max_y,maximum[1]);VVALUE(max_z,maximum[2]);
        VVALUE(count_x,counts[0]);VVALUE(count_y,counts[1]);VVALUE(count_z,counts[2]);VVALUE(blend_cells,blend_cells);
#undef VVALUE
        case ProbeVolumeField::count:break;
        }
        return 0;
    }
    switch(option) {
#define VALUE(id, path) case ViewOption::id:return static_cast<double>(view.path)
    VALUE(shading,shading);VALUE(wireframe,wireframe);VALUE(hidden_lines,hidden_lines);VALUE(triangle_edges,triangle_edges);
    VALUE(wire_width,wire_width);VALUE(wire_opacity,wire_opacity);VALUE(background,background);VALUE(reference_grid,reference_grid);
    VALUE(origin_axes,origin_axes);VALUE(grid_spacing,grid_spacing);VALUE(grid_plane,grid_plane);
    VALUE(shadows,lit.shadows);VALUE(ddgi,lit.ddgi);VALUE(gtao,lit.gtao);VALUE(sssr,lit.sssr);VALUE(bloom,lit.bloom);VALUE(ssil,lit.ssil);
    VALUE(antialiasing,lit.antialiasing);VALUE(exposure_ev,lit.exposure_ev);VALUE(shadow_bias,lit.shadow_bias);
    VALUE(direct_candidates,lit.direct_candidates);VALUE(ao_radius,lit.ao_radius);
    VALUE(reflection_distance,lit.reflection_distance);VALUE(reflection_roughness,lit.reflection_roughness);
    VALUE(probe_update_budget,lit.probe_update_budget);VALUE(probe_rays,lit.probe_rays);VALUE(probe_debug,lit.probe_debug);
    VALUE(probe_bounce,lit.probe_bounce);VALUE(indirect_gain,lit.indirect_gain);VALUE(probe_feedback_visibility,lit.probe_feedback_visibility);
    VALUE(probe_visualization,lit.probe_visualization);VALUE(probe_volume,lit.probe_volume);VALUE(probe_relocation,lit.probe_relocation);
    VALUE(probe_xray,lit.probe_xray);VALUE(probe_radius,lit.probe_radius);
    VALUE(probe_coverage,lit.probe_coverage);VALUE(probe_debug_volume,lit.probe_debug_volume);
    VALUE(direct_reference,lit.direct_reference);
    VALUE(cavity,cavity.enabled);VALUE(cavity_ridge,cavity.ridge);VALUE(cavity_valley,cavity.valley);VALUE(cavity_radius,cavity.radius);
#undef VALUE
    case ViewOption::volume_first:case ViewOption::volume_last:break;
    }
    return 0;
}
Result<bool> set_view_option(renderer::ViewSettings& view, ViewOption option, double value) {
    const auto found=std::ranges::find(options,option,&ViewOptionInfo::option);
    if(found==options.end() || !std::isfinite(value) || value<found->minimum || value>found->maximum ||
        (found->kind!=K::number && value!=std::floor(value))) return invalid("Invalid display option value");
    const auto previous=view_value(view,option);const bool gtao=view.lit.gtao,ssil=view.lit.ssil;
    if(volume_option(option)) {
        auto& volume=view.lit.probe_volumes[volume_index(option)];
        switch(volume_field(option)) {
#define VSET(id,path) case ProbeVolumeField::id:volume.path=static_cast<std::remove_cvref_t<decltype(volume.path)>>(value);break
        VSET(enabled,enabled);VSET(placement,placement);VSET(min_x,minimum[0]);VSET(min_y,minimum[1]);VSET(min_z,minimum[2]);
        VSET(max_x,maximum[0]);VSET(max_y,maximum[1]);VSET(max_z,maximum[2]);
        VSET(count_x,counts[0]);VSET(count_y,counts[1]);VSET(count_z,counts[2]);VSET(blend_cells,blend_cells);
#undef VSET
        case ProbeVolumeField::count:break;
        }
    } else switch(option) {
#define SET(id, path) case ViewOption::id:view.path=static_cast<std::remove_cvref_t<decltype(view.path)>>(value);break
    SET(shading,shading);SET(wireframe,wireframe);SET(hidden_lines,hidden_lines);SET(triangle_edges,triangle_edges);
    SET(wire_width,wire_width);SET(wire_opacity,wire_opacity);SET(background,background);SET(reference_grid,reference_grid);
    SET(origin_axes,origin_axes);SET(grid_spacing,grid_spacing);SET(grid_plane,grid_plane);
    SET(shadows,lit.shadows);SET(ddgi,lit.ddgi);SET(gtao,lit.gtao);SET(sssr,lit.sssr);SET(bloom,lit.bloom);SET(ssil,lit.ssil);
    SET(antialiasing,lit.antialiasing);SET(exposure_ev,lit.exposure_ev);SET(shadow_bias,lit.shadow_bias);
    SET(direct_candidates,lit.direct_candidates);SET(ao_radius,lit.ao_radius);
    SET(reflection_distance,lit.reflection_distance);SET(reflection_roughness,lit.reflection_roughness);
    SET(probe_update_budget,lit.probe_update_budget);SET(probe_rays,lit.probe_rays);SET(probe_debug,lit.probe_debug);
    SET(probe_bounce,lit.probe_bounce);SET(indirect_gain,lit.indirect_gain);SET(probe_feedback_visibility,lit.probe_feedback_visibility);
    SET(probe_visualization,lit.probe_visualization);SET(probe_volume,lit.probe_volume);SET(probe_relocation,lit.probe_relocation);
    SET(probe_xray,lit.probe_xray);SET(probe_radius,lit.probe_radius);
    SET(probe_coverage,lit.probe_coverage);SET(probe_debug_volume,lit.probe_debug_volume);
    SET(direct_reference,lit.direct_reference);
    SET(cavity,cavity.enabled);SET(cavity_ridge,cavity.ridge);SET(cavity_valley,cavity.valley);SET(cavity_radius,cavity.radius);
#undef SET
    case ViewOption::volume_first:case ViewOption::volume_last:break;
    }
    if(value!=0) {if(option==ViewOption::gtao) view.lit.ssil=false;else if(option==ViewOption::ssil) view.lit.gtao=false;}
    return view_value(view,option)!=previous || gtao!=view.lit.gtao || ssil!=view.lit.ssil;
}
Result<void> validate_view_options(const renderer::ViewSettings& view,std::array<float,3> focus) {
    for(const auto& option:options) {
        const auto value=view_value(view,option.option);
        if(!std::isfinite(value) || value<option.minimum || value>option.maximum || (option.kind!=K::number && value!=std::floor(value)))
            return invalid("Invalid display option: "+std::string(option.name));
    }
    if(const auto* error=renderer::probe_focus_error(view.lit,focus)) return invalid(error);
    if(view.lit.gtao && view.lit.ssil) return invalid("GTAO and Local GI are mutually exclusive");
    return {};
}
}
