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
constexpr std::array<std::string_view, 3> probe_history{"live","hold_irradiance","hold_distance"};
#define FIELD(id, kind, low, high, label, category, description) \
    ViewOptionInfo{ViewOption::id, #id, K::kind, low, high, {}, label, category, description}
#define ENUM(id, values, label, category, description) \
    ViewOptionInfo{ViewOption::id, #id, K::enumeration, 0, double(values.size()-1), values, label, category, description}
constexpr std::string_view volume_enabled = "启用这个 DDGI Volume，让它参与探针更新与间接光采样。所有启用的 Volume 共用探针数量和每帧更新预算。";
constexpr std::string_view volume_placement = "Fixed 使用固定的世界坐标范围；Follow Focus 把范围作为相对于 lighting focus 的偏移。跟随时按整格移动，保持探针间距。";
constexpr std::string_view volume_minimum = "Volume 范围的最小 XYZ，单位 m。Fixed 使用世界坐标，Follow Focus 使用相对于 lighting focus 的偏移。每个分量必须小于对应的最大值。";
constexpr std::string_view volume_maximum = "Volume 范围的最大 XYZ，单位 m。Fixed 使用世界坐标，Follow Focus 使用相对于 lighting focus 的偏移。每个分量必须大于对应的最小值。";
constexpr std::string_view volume_counts = "三个轴各自的探针数量。范围不变时，增加数量会缩小间距，同时增加存储和更新工作量。\n所有启用的 Volume 合计最多 4096 个探针；Follow Focus 还需要足够的内部格子容纳边界过渡。";
constexpr std::string_view sky_color = "天光的非负线性 RGB，三个分量依次为 R、G、B，逐通道乘以 Intensity。可以大于 1；它不是 sRGB 屏幕颜色。";
constexpr std::string_view volume_blend = "Volume 边缘的过渡宽度，以探针格子为单位。边缘贡献会平滑交给其他覆盖该处的 Volume；它不会补出没有探针覆盖的区域。";
#define VFIELD(slot,id,kind,low,high,label,description) \
    ViewOptionInfo{probe_volume_option(slot,ProbeVolumeField::id),"probe_volume_" #slot "_" #id,K::kind,low,high,{},label,"DDGI Volume",description}
#define VOLUME(slot) VFIELD(slot,enabled,boolean,0,1,"Enabled",volume_enabled), \
    ViewOptionInfo{probe_volume_option(slot,ProbeVolumeField::placement),"probe_volume_" #slot "_placement",K::enumeration,0,1,placement,"Placement","DDGI Volume",volume_placement}, \
    VFIELD(slot,min_x,number,-100000,100000,"World min (m)",volume_minimum),VFIELD(slot,min_y,number,-100000,100000,"World min (m)",volume_minimum),VFIELD(slot,min_z,number,-100000,100000,"World min (m)",volume_minimum), \
    VFIELD(slot,max_x,number,-100000,100000,"World max (m)",volume_maximum),VFIELD(slot,max_y,number,-100000,100000,"World max (m)",volume_maximum),VFIELD(slot,max_z,number,-100000,100000,"World max (m)",volume_maximum), \
    VFIELD(slot,count_x,integer,2,32,"Probe count",volume_counts),VFIELD(slot,count_y,integer,2,32,"Probe count",volume_counts),VFIELD(slot,count_z,integer,2,32,"Probe count",volume_counts),VFIELD(slot,blend_cells,number,0,2,"Blend (cells)",volume_blend)
constexpr std::array options{
    ENUM(shading,shading,"Shading","Display","选择当前视口的着色方式。Smooth、Flat 和 MatCap 用于观察形体；Scene Lit 使用场景的材质、灯光和 World 效果设置。"),
    ENUM(wireframe,wire,"Wireframe","Display","Off 隐藏线框，Overlay 在线框下保留着色表面，Only 只显示线框。它不修改 Mesh 的拓扑或材质。"),
    FIELD(hidden_lines,boolean,0,1,"Hide occluded edges","Display","在线框 Only 模式中隐藏被场景表面遮住的边，便于分辨前后关系。"),
    FIELD(triangle_edges,boolean,0,1,"Triangle edges","Display","显示三角形的全部边。关闭时遵循 Mesh 提供的多边形边标记，避免把三角化内部边画出来。"),
    FIELD(wire_width,number,.25,16,"Wire width (px)","Display","线框宽度，单位为物理像素，不随界面 DPI 再次放大。"),
    FIELD(wire_opacity,number,0,1,"Wire opacity","Display","线框的不透明度。0 为透明，1 为不透明；适当降低可减少密集网格对表面的遮挡。"),
    ENUM(background,background,"Background","Guides","选择 Studio 预览的背景。Scene Lit 使用黑色背景，不使用这个选项。"),
    FIELD(reference_grid,boolean,0,1,"Reference grid","Guides","显示用于观察位置和尺度的参考网格。网格显示与物体是否吸附是独立设置。"),
    FIELD(origin_axes,boolean,0,1,"Origin axes","Guides","显示世界原点的坐标轴，帮助识别当前网格平面和方向。"),
    FIELD(grid_spacing,number,.0001f,10000,"Grid spacing (m)","Guides","参考网格的格子间距，单位 m。改变它不会移动场景中的 Entity。"),
    ENUM(grid_plane,plane,"Grid plane","Guides","选择参考网格所在的坐标平面：XZ、XY 或 YZ。"),
    FIELD(shadows,boolean,0,1,"Shadows","Lighting","控制 Scene Lit 的灯光阴影。灯光自身的 Cast Shadows 设置仍决定该光源是否投影。"),
    FIELD(ddgi,boolean,0,1,"DDGI","Propagation","启用探针场提供的间接漫反射光。覆盖范围、密度和更新预算由下面的 Volume 与 Propagation 参数决定。"),
    FIELD(gtao,boolean,0,1,"GTAO","Effects","用屏幕上的几何信息估计附近遮挡，突出接触与凹角。与 Local GI (SSIL) 互斥，启用其中一个会关闭另一个。"),
    FIELD(sssr,boolean,0,1,"Reflections (SSSR)","Effects","启用屏幕空间反射，沿当前画面中的表面信息寻找反射命中。屏幕外或被遮住的表面信息有限。"),
    FIELD(bloom,boolean,0,1,"Bloom","Effects","让画面中的高亮部分向周围扩散，形成光晕。它改变显示效果，不会把光照写回 DDGI 探针。"),
    FIELD(ssil,boolean,0,1,"Local GI (SSIL)","Effects","使用屏幕上的表面信息补充附近的间接光变化。它与 GTAO 互斥，不替代 DDGI 的空间覆盖。"),
    ENUM(antialiasing,aa,"Antialiasing","Effects","选择抗锯齿方式。SMAA 处理当前画面的边缘；TAA 和 SMAA T2x 还利用跨帧信息；Off 关闭抗锯齿。"),
    FIELD(exposure_ev,number,-16,16,"Exposure (EV)","Lighting","控制最终显示的曝光。每增加 1 EV，曝光倍率翻倍；不改变场景线性 HDR 历史中的光照能量。"),
    FIELD(shadow_bias,number,.000001f,.1f,"Shadow bias (m)","Lighting","阴影射线的起点偏移，单位 m，用于减少表面对自己的误遮挡。过大可能让接触处的阴影脱离表面。"),
    FIELD(direct_candidates,integer,1,32,"Direct candidates","Lighting","每个接收点初始挑选的局部光候选数量。增加候选会增加直接光采样工作量；它不是场景允许存在的灯光数量。"),
    FIELD(ao_radius,number,.001f,100,"AO radius (m)","Effects","GTAO 与 Local GI (SSIL) 观察附近表面的半径，单位 m。应结合场景的实际尺度调整。"),
    FIELD(reflection_distance,number,.001f,10000,"Trace distance (m)","Effects","屏幕空间反射允许追踪的最大距离，单位 m。增加距离不能补出画面外缺失的表面信息。"),
    FIELD(reflection_roughness,number,0,1,"Max roughness","Effects","允许进行屏幕空间反射的最大表面粗糙度。超过此值的表面不执行该反射追踪。"),
    FIELD(probe_update_budget,integer,1,4096,"Updates / frame","Propagation","每帧安排更新的探针数量，所有 Volume 共用，初始化也计入预算。增大可更快更新场中的照明，同时增加单帧工作量。"),
    FIELD(probe_rays,integer,1,256,"Rays / probe","Propagation","每次更新一个探针所使用的照明射线数量。更多射线通常能降低采样噪声，但需要更多 GPU 工作。"),
    FIELD(probe_debug,boolean,0,1,"Receiver coverage","Diagnostics","用颜色显示当前接收面的真实 DDGI 支持情况。它检查实际 gather 的遮挡、权重和历史，不以最近的探针球代替覆盖判断。"),
    FIELD(probe_bounce,number,0,.99f,"Feedback","Propagation","DDGI 将上一轮间接光送回探针场的运输系数。0 关闭这部分反馈；改变它会使旧探针照明失效并重新积累。"),
    FIELD(indirect_gain,number,0,8,"Indirect gain","Propagation","最终合成时对间接漫反射应用的增益。DDGI 与 Local GI 合成后只乘一次，不回写探针，也不补出缺失的空间覆盖。"),
    FIELD(probe_feedback_visibility,boolean,0,1,"Feedback visibility","Diagnostics","控制反馈查询是否使用实际射线可见性。关闭后使用距离缓存遮挡近似，供诊断对照；切换会使旧运输结果失效。"),
    ENUM(probe_visualization,probes,"Probe spheres","Diagnostics","选择探针球显示的内容：Irradiance 为辐照度，State 为分类状态，Update Age 为距上次评估的更新帧数，Distance 为距离信息。Off 隐藏探针球。"),
    FIELD(probe_volume,boolean,0,1,"Volume bounds","Diagnostics","显示 DDGI Volume 的实际世界空间范围，便于检查覆盖位置。"),
    FIELD(probe_relocation,boolean,0,1,"Relocation lines","Diagnostics","显示探针从规则格点到实际修正位置的连线，便于检查几何内部分类与位置修正。"),
    FIELD(probe_xray,boolean,0,1,"X-Ray","Diagnostics","让探针诊断几何透过场景表面显示，便于观察被遮挡的探针和 Volume。"),
    FIELD(probe_radius,number,.01f,.45f,"Sphere size","Diagnostics","探针球半径相对于最小网格间距的比例。只改变诊断球的显示尺寸，不改变光照采样范围。"),
    ENUM(probe_coverage,coverage,"Coverage metric","Diagnostics","选择 Receiver Coverage 的着色依据：状态、主导 Volume、实际贡献邻居数或最大归一化贡献占比。需打开 Receiver Coverage 才显示。"),
    FIELD(probe_debug_volume,integer,-1,3,"Debug volume","Diagnostics","选择显示诊断几何的 Volume 槽位。All volumes（-1）显示所有槽位；不会关闭其他 Volume 的光照贡献。"),
    FIELD(direct_reference,boolean,0,1,"Direct reference","Diagnostics","直接光诊断参考：平均 64 次独立 RIS 估计，不使用跨帧复用或降噪。GPU 开销明显增加，适合对照采样偏差。"),
    FIELD(spatial_light_sampling,boolean,0,1,"Spatial light sampling","Diagnostics","DI 与 DDGI 共同使用附近光源提议，并保留全局光源采样支持。关闭后只使用全局提议，已有采样表仍保留，便于同场景对照。"),
    FIELD(direct_clustered,boolean,0,1,"Clustered direct lighting","Diagnostics","默认开启：按屏幕 tile 与深度分层保守收集局部灯，再精确枚举；紧凑光源确定求值，面光、软阴影半影和发光网格仍为随机估计。列表超过 64 项时回退完整枚举，不丢灯。关闭后回退 ReSTIR 直接光，仅作诊断对照。Direct Reference 优先；此开关不改变 DDGI，也不保存进 Scene。"),
    FIELD(direct_shadow_cache,boolean,0,1,"Cached direct shadows","Diagnostics","临时阴影诊断：仅 Clustered Direct 开启且 Reference 关闭时使用缓存阴影 B；关闭时使用精确 RT 可见性 A。默认关闭，不改变 World、Scene 或撤销历史。"),
    FIELD(direct_cluster_inspect,boolean,0,1,"Inspect receiver lights","Diagnostics","按需统计有效 GBuffer 接收像素落入解析局部灯支持球的数量；不含太阳和发光网格，也不是遮挡后的灯数。仅 Clustered 且非 Reference 路径有效。诊断耗时计入光源列表构建，性能对比时必须关闭。"),
    ENUM(probe_history,probe_history,"Probe history","Diagnostics","Live 正常更新；Hold Irradiance 冻结辐照度历史；Hold Distance 冻结距离历史。用于定位 DDGI 波动来源，不修改 Scene。"),
    FIELD(probe_inspect,boolean,0,1,"Inspect receiver","Diagnostics","读取指定屏幕像素的 DDGI 接收面与探针贡献，供 Control 诊断使用。启用会增加少量诊断工作。"),
    FIELD(probe_inspect_x,integer,0,65535,"Receiver X (px)","Diagnostics","待检查接收面的屏幕 X 坐标，单位为物理像素。配合 Inspect Receiver 使用。"),
    FIELD(probe_inspect_y,integer,0,65535,"Receiver Y (px)","Diagnostics","待检查接收面的屏幕 Y 坐标，单位为物理像素。配合 Inspect Receiver 使用。"),
    FIELD(probe_inspect_source,integer,-1,renderer::probe_resident_limit-1,"Source probe","Diagnostics","指定要检查的源探针索引，-1 自动选择。有效范围为 0–4095；探针尚不存在时不会伪造采样数据。"),
    FIELD(cavity,boolean,0,1,"Cavity","Cavity","在 Studio 预览中增强凸边和凹处的明暗对比，帮助辨认形体。它不修改材质、灯光或 Scene Lit 的结果。"),
    FIELD(cavity_ridge,number,0,2,"Ridge","Cavity","凸边的对比强度。0 关闭凸边增强，不影响 Valley 的凹处增强。"),
    FIELD(cavity_valley,number,0,2,"Valley","Cavity","凹处的对比强度。0 关闭凹处增强，不影响 Ridge 的凸边增强。"),
    FIELD(cavity_radius,number,.5,4,"Radius (px)","Cavity","Cavity 的屏幕空间观察半径，单位为物理像素。较大值强调更宽的边缘区域。"),
    VOLUME(0),VOLUME(1),VOLUME(2),VOLUME(3),
    FIELD(sky_enabled,boolean,0,1,"Sky","Sky","启用均匀天光。关闭时环境照明为零；颜色和强度保留。"),
    FIELD(sky_intensity,number,0,65504,"Intensity (cd/m²)","Sky","均匀天光的辐亮度，单位 cd/m²，再由 Color 逐通道调制。每个通道的 Color × Intensity 不能超过 65504。"),
    FIELD(sky_color_x,number,0,65504,"Color","Sky",sky_color),FIELD(sky_color_y,number,0,65504,"Color","Sky",sky_color),
    FIELD(sky_color_z,number,0,65504,"Color","Sky",sky_color)
};
#undef VOLUME
#undef VFIELD
#undef FIELD
#undef ENUM
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
        option==ViewOption::probe_bounce || option==ViewOption::indirect_gain || volume_option(option) ||
        (option>=ViewOption::sky_enabled && option<=ViewOption::sky_color_z);
}
std::span<const ViewOptionInfo> view_options() noexcept { return options; }
std::span<const ViewOptionAlias> view_option_aliases() noexcept {return aliases;}
const ViewOptionInfo* view_option(ViewOption option) noexcept {
    const auto found=std::ranges::find(options,option,&ViewOptionInfo::option);
    return found==options.end()?nullptr:&*found;
}
const ViewOptionInfo* view_option(std::string_view name) noexcept {
    for(const auto& value:options) if(value.name==name) return &value;
    for(const auto& alias:aliases) if(alias.name==name) return view_option(alias.option);
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
    VALUE(probe_history,lit.probe_history);VALUE(probe_inspect,lit.probe_inspect);
    VALUE(probe_inspect_x,lit.probe_inspect_pixel[0]);VALUE(probe_inspect_y,lit.probe_inspect_pixel[1]);
    VALUE(probe_inspect_source,lit.probe_inspect_source);
    VALUE(direct_reference,lit.direct_reference);VALUE(spatial_light_sampling,lit.spatial_light_sampling);
    VALUE(direct_clustered,lit.direct_clustered);
    VALUE(direct_shadow_cache,lit.direct_shadow_cache);
    VALUE(direct_cluster_inspect,lit.direct_cluster_inspect);
    VALUE(cavity,cavity.enabled);VALUE(cavity_ridge,cavity.ridge);VALUE(cavity_valley,cavity.valley);VALUE(cavity_radius,cavity.radius);
    VALUE(sky_enabled,lit.sky_enabled);VALUE(sky_intensity,lit.sky_intensity);
    VALUE(sky_color_x,lit.sky_color[0]);VALUE(sky_color_y,lit.sky_color[1]);VALUE(sky_color_z,lit.sky_color[2]);
#undef VALUE
    case ViewOption::volume_first:case ViewOption::volume_last:break;
    }
    return 0;
}
Result<bool> set_view_option(renderer::ViewSettings& view, ViewOption option, double value) {
    const auto* found=view_option(option);
    if(!found || !std::isfinite(value) || value<found->minimum || value>found->maximum ||
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
    SET(probe_history,lit.probe_history);SET(probe_inspect,lit.probe_inspect);
    SET(probe_inspect_x,lit.probe_inspect_pixel[0]);SET(probe_inspect_y,lit.probe_inspect_pixel[1]);
    SET(probe_inspect_source,lit.probe_inspect_source);
    SET(direct_reference,lit.direct_reference);SET(spatial_light_sampling,lit.spatial_light_sampling);
    SET(direct_clustered,lit.direct_clustered);
    SET(direct_shadow_cache,lit.direct_shadow_cache);
    SET(direct_cluster_inspect,lit.direct_cluster_inspect);
    SET(cavity,cavity.enabled);SET(cavity_ridge,cavity.ridge);SET(cavity_valley,cavity.valley);SET(cavity_radius,cavity.radius);
    SET(sky_enabled,lit.sky_enabled);SET(sky_intensity,lit.sky_intensity);
    SET(sky_color_x,lit.sky_color[0]);SET(sky_color_y,lit.sky_color[1]);SET(sky_color_z,lit.sky_color[2]);
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
