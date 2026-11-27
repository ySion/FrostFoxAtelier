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

constexpr std::array<std::string_view, 3> aa{"off", "smaa", "taa"};

constexpr std::array<std::string_view, 2> gi_quality{"high", "ultra"};

constexpr std::array<std::string_view, 3> sky_mode{"none", "physical_atmosphere", "hdri_sky_map"};

constexpr std::array<std::string_view, 2> tonemapper{"pbr_neutral", "agx"};

constexpr std::string_view sky_color_desc =
    "天光的非负线性 RGB，三个分量依次为 R、G、B，逐通道乘以 Intensity。可以大于 1；它不是 sRGB 屏幕颜色。";

#define FIELD(id, kind, low, high, label, category, description)                                                       \
    ViewOptionInfo {                                                                                                   \
        ViewOption::id, #id, K::kind, low, high, {}, label, category, description                                      \
    }
#define ENUM(id, values, label, category, description)                                                                 \
    ViewOptionInfo {                                                                                                   \
        ViewOption::id, #id, K::enumeration, 0, double(values.size() - 1), values, label, category, description        \
    }

constexpr std::array options{
    ENUM(
        shading, shading, "Shading", "Display",
        "选择当前视口的着色方式。Smooth、Flat 和 MatCap 用于观察形体；Scene Lit 使用场景的材质、灯光和 World 渲染管线设置。"),
    ENUM(wireframe, wire, "Wireframe", "Display",
         "Off 隐藏线框，Overlay 在线框下保留着色表面，Only 只显示线框。它不修改 Mesh 的拓扑或材质。"),
    FIELD(hidden_lines, boolean, 0, 1, "Hide occluded edges", "Display",
          "在线框 Only 模式中隐藏被场景表面遮住的边，便于分辨前后关系。"),
    FIELD(triangle_edges, boolean, 0, 1, "Triangle edges", "Display",
          "显示三角形的全部边。关闭时遵循 Mesh 提供的多边形边标记，避免把三角化内部边画出来。"),
    FIELD(
        backface_tint, boolean, 0, 1, "Backface tint", "Display",
        "在 Studio 视图中用醒目的青蓝色高亮法线背向镜头的反面，并临时显示单面材质背面，便于排查程序化网格的绕序（CCW/CW）反转问题。"),
    FIELD(wire_width, number, .25, 16, "Wire width (px)", "Display",
          "线框宽度，单位为物理像素，不随界面 DPI 再次放大。"),
    FIELD(wire_opacity, number, 0, 1, "Wire opacity", "Display",
          "线框的不透明度。0 为透明，1 为不透明；适当降低可减少密集网格对表面的遮挡。"),
    ENUM(background, background, "Background", "Guides",
         "选择 Studio 预览的背景。Scene Lit 使用物理天空或黑色背景，不使用这个选项。"),
    FIELD(reference_grid, boolean, 0, 1, "Reference grid", "Guides",
          "显示用于观察位置和尺度的参考网格。网格显示与物体是否吸附是独立设置。"),
    FIELD(origin_axes, boolean, 0, 1, "Origin axes", "Guides", "显示世界原点的坐标轴，帮助识别当前网格平面和方向。"),
    FIELD(show_locators, boolean, 0, 1, "Entity locators", "Guides",
          "在视口中显示灯光、相机与空变换实体的定位符号。隐藏时保留纯粹的场景画面预览。"),
    FIELD(grid_spacing, number, .0001f, 10000, "Grid spacing (m)", "Guides",
          "参考网格的格子间距，单位 m。改变它不会移动场景中的 Entity。"),
    ENUM(grid_plane, plane, "Grid plane", "Guides", "选择参考网格所在的坐标平面：XZ、XY 或 YZ。"),
    FIELD(shadows, boolean, 0, 1, "Shadows", "Direct Lighting",
          "控制 Scene Lit 的三级直接光阴影（VSM 虚拟阴影图 + 半影光追 + 接触微阴影）。"),
    FIELD(contact_shadow_length, number, 0, 2, "Contact shadow (m)", "Direct Lighting",
          "主平行光屏幕空间接触阴影最大追踪步长（m）；0 关闭接触阴影补充。"),
    FIELD(direct_candidates, integer, 1, 32, "Direct candidates", "Direct Lighting",
          "次级 GI 命中点每次直接评估的局部光候选上限，超出部分走加权蓄水池无偏采样。"),
    FIELD(indirect_lighting, boolean, 0, 1, "Indirect lighting", "Global Illumination",
          "启用三层混合漫反射全局光照（GT-VBAO + 屏幕空间探针 + SHaRC/GI-1.2 多跳表面哈希辐射缓存）。"),
    ENUM(gi_quality, gi_quality, "GI quality", "Global Illumination",
         "控制屏幕空间辐射探针的每帧追踪质量档位（High / Ultra）。"),
    FIELD(indirect_gain, number, 0, 8, "Indirect gain", "Global Illumination",
          "最终合成时对漫反射间接光应用的增益，只乘一次，不回写多跳辐射缓存。"),
    FIELD(ssilvb, boolean, 0, 1, "GT-VBAO", "Global Illumination",
          "启用有符号 32 扇区地平线比特掩码环境光遮蔽、弯曲法线锥与亚探针微反弹（GT-VBAO）。"),
    FIELD(ao_radius, number, .001f, 100, "AO radius (m)", "Global Illumination",
          "GT-VBAO 观察附近表面遮蔽与微反弹的世界空间半径（m）。"),
    FIELD(reflections, boolean, 0, 1, "Reflections", "Reflections",
          "启用三级清漆感知镜面反射（Hi-Z 屏幕步进 + 有界 VNDF 光追回退 + 粗糙表面弯曲法线锥探针评估）。"),
    FIELD(reflection_distance, number, .001f, 10000, "Trace distance (m)", "Reflections",
          "镜面反射允许追踪的最大世界距离（m）。"),
    FIELD(reflection_roughness, number, 0, 1, "Max roughness", "Reflections",
          "允许执行光线步进与光追反射的最大表面粗糙度；超过此值的表面使用零射线预过滤探针。"),
    ENUM(sky_mode, sky_mode, "Sky mode", "Sky & Fog",
         "选择天空背景与环境辐射来源：None 关闭、Physical Atmosphere 物理大气散射或 HDRI Sky Map。"),
    FIELD(sky_intensity, number, 0, 65504, "Sky intensity (cd/m²)", "Sky & Fog",
          "天光的辐亮度（cd/m²），再由 Sky color 逐通道调制；单通道乘积不超过 65504。"),
    FIELD(sky_rotation, number, -180, 180, "Sky rotation (°)", "Sky & Fog",
          "天空贴图绕世界 +Y 的偏航角（度），与 Transform 绕 +Y 的正旋转同向。"),
    FIELD(sky_color_x, number, 0, 65504, "Sky color", "Sky & Fog", sky_color_desc),
    FIELD(sky_color_y, number, 0, 65504, "Sky color", "Sky & Fog", sky_color_desc),
    FIELD(sky_color_z, number, 0, 65504, "Sky color", "Sky & Fog", sky_color_desc),
    FIELD(fog_enabled, boolean, 0, 1, "Volumetric fog", "Sky & Fog",
          "启用视锥体体素（160×90×64 Froxel）单次散射与时域积分体积雾。"),
    FIELD(fog_density, number, 0, 10, "Fog density", "Sky & Fog", "海平面基准体积雾消光密度（1/m）。"),
    FIELD(fog_height_falloff, number, .001f, 10, "Fog height falloff", "Sky & Fog",
          "体积雾随世界高度 Y 指数衰减的系数。"),
    FIELD(fog_anisotropy, number, -.95f, .95f, "Fog anisotropy", "Sky & Fog",
          "Henyey-Greenstein 相位函数前向/后向散射各向异性参数 g。"),
    FIELD(exposure_ev, number, -16, 16, "Exposure (EV)", "Post & Display",
          "控制最终显示的曝光。每增加 1 EV，曝光倍率翻倍；不改变场景线性 HDR 历史中的光照能量。"),
    FIELD(bloom, boolean, 0, 1, "Bloom", "Post & Display",
          "启用物理软膝盖 Karis 抗萤火虫 6 级金字塔泛光（Bloom）。"),
    ENUM(tonemapper, tonemapper, "Tonemapper", "Post & Display",
         "选择 HDR 到显示空间的色调映射曲线：Khronos PBR Neutral 或 AgX。"),
    ENUM(antialiasing, aa, "Antialiasing", "Post & Display",
         "选择抗锯齿方式：Off 关闭、SMAA 单帧形态学抗锯齿或 TAA 时域抗锯齿。"),
    FIELD(
        direct_cluster_inspect, boolean, 0, 1, "Inspect receiver lights", "Diagnostics",
        "按需统计有效 GBuffer 接收像素落入解析局部灯支持球的数量；不含太阳，也不是遮挡后的灯数。诊断耗时计入光源列表构建，性能对比时必须关闭。"),
    FIELD(cavity, boolean, 0, 1, "Cavity", "Cavity",
          "在 Studio 预览中增强凸边和凹处的明暗对比，帮助辨认形体。它不修改材质、灯光或 Scene Lit 的结果。"),
    FIELD(cavity_ridge, number, 0, 2, "Ridge", "Cavity", "凸边的对比强度。0 关闭凸边增强，不影响 Valley 的凹处增强。"),
    FIELD(cavity_valley, number, 0, 2, "Valley", "Cavity", "凹处的对比强度。0 关闭凹处增强，不影响 Ridge 的凸边增强。"),
    FIELD(cavity_radius, number, .5, 4, "Radius (px)", "Cavity",
          "Cavity 的屏幕空间观察半径，单位为物理像素。较大值强调更宽的边缘区域。")};

#undef FIELD
#undef ENUM

auto invalid(std::string message) {
    return std::unexpected(Error{ErrorCode::invalid_definition, std::move(message)});
}
} // namespace

bool world_option(ViewOption option) noexcept {
    return option >= ViewOption::shadows && option <= ViewOption::antialiasing;
}

std::span<const ViewOptionInfo> view_options() noexcept {
    return options;
}

const ViewOptionInfo* view_option(ViewOption option) noexcept {
    const auto found = std::ranges::find(options, option, &ViewOptionInfo::option);
    return found == options.end() ? nullptr : &*found;
}

const ViewOptionInfo* view_option(std::string_view name) noexcept {
    for (const auto& value : options)
        if (value.name == name)
            return &value;
    return nullptr;
}

double view_value(const renderer::ViewSettings& view, ViewOption option) noexcept {
    switch (option) {
#define VALUE(id, path)                                                                                                \
    case ViewOption::id:                                                                                               \
        return static_cast<double>(view.path)
        VALUE(shading, shading);
        VALUE(wireframe, wireframe);
        VALUE(hidden_lines, hidden_lines);
        VALUE(triangle_edges, triangle_edges);
        VALUE(backface_tint, backface_tint);
        VALUE(wire_width, wire_width);
        VALUE(wire_opacity, wire_opacity);
        VALUE(background, background);
        VALUE(reference_grid, reference_grid);
        VALUE(origin_axes, origin_axes);
        VALUE(show_locators, show_locators);
        VALUE(grid_spacing, grid_spacing);
        VALUE(grid_plane, grid_plane);
        VALUE(shadows, lit.shadows);
        VALUE(contact_shadow_length, lit.contact_shadow_length);
        VALUE(direct_candidates, lit.direct_candidates);
        VALUE(indirect_lighting, lit.indirect_lighting);
        VALUE(gi_quality, lit.gi_quality);
        VALUE(indirect_gain, lit.indirect_gain);
        VALUE(ssilvb, lit.ssilvb);
        VALUE(ao_radius, lit.ao_radius);
        VALUE(reflections, lit.reflections);
        VALUE(reflection_distance, lit.reflection_distance);
        VALUE(reflection_roughness, lit.reflection_roughness);
        VALUE(sky_mode, lit.sky_mode);
        VALUE(sky_intensity, lit.sky_intensity);
        VALUE(sky_rotation, lit.sky_rotation);
        VALUE(sky_color_x, lit.sky_color[0]);
        VALUE(sky_color_y, lit.sky_color[1]);
        VALUE(sky_color_z, lit.sky_color[2]);
        VALUE(fog_enabled, lit.fog_enabled);
        VALUE(fog_density, lit.fog_density);
        VALUE(fog_height_falloff, lit.fog_height_falloff);
        VALUE(fog_anisotropy, lit.fog_anisotropy);
        VALUE(exposure_ev, lit.exposure_ev);
        VALUE(bloom, lit.bloom);
        VALUE(tonemapper, lit.tonemapper);
        VALUE(antialiasing, lit.antialiasing);
        VALUE(direct_cluster_inspect, lit.direct_cluster_inspect);
        VALUE(cavity, cavity.enabled);
        VALUE(cavity_ridge, cavity.ridge);
        VALUE(cavity_valley, cavity.valley);
        VALUE(cavity_radius, cavity.radius);
#undef VALUE
    }
    return 0;
}

Result<bool> set_view_option(renderer::ViewSettings& view, ViewOption option, double value) {
    const auto* found = view_option(option);
    if (!found || !std::isfinite(value) || value < found->minimum || value > found->maximum ||
        (found->kind != K::number && value != std::floor(value)))
        return invalid("Invalid display option value");
    const auto previous = view_value(view, option);
    switch (option) {
#define SET(id, path)                                                                                                  \
    case ViewOption::id:                                                                                               \
        view.path = static_cast<std::remove_cvref_t<decltype(view.path)>>(value);                                      \
        break
        SET(shading, shading);
        SET(wireframe, wireframe);
        SET(hidden_lines, hidden_lines);
        SET(triangle_edges, triangle_edges);
        SET(backface_tint, backface_tint);
        SET(wire_width, wire_width);
        SET(wire_opacity, wire_opacity);
        SET(background, background);
        SET(reference_grid, reference_grid);
        SET(origin_axes, origin_axes);
        SET(show_locators, show_locators);
        SET(grid_spacing, grid_spacing);
        SET(grid_plane, grid_plane);
        SET(shadows, lit.shadows);
        SET(contact_shadow_length, lit.contact_shadow_length);
        SET(direct_candidates, lit.direct_candidates);
        SET(indirect_lighting, lit.indirect_lighting);
        SET(gi_quality, lit.gi_quality);
        SET(indirect_gain, lit.indirect_gain);
        SET(ssilvb, lit.ssilvb);
        SET(ao_radius, lit.ao_radius);
        SET(reflections, lit.reflections);
        SET(reflection_distance, lit.reflection_distance);
        SET(reflection_roughness, lit.reflection_roughness);
        SET(sky_mode, lit.sky_mode);
        SET(sky_intensity, lit.sky_intensity);
        SET(sky_rotation, lit.sky_rotation);
        SET(sky_color_x, lit.sky_color[0]);
        SET(sky_color_y, lit.sky_color[1]);
        SET(sky_color_z, lit.sky_color[2]);
        SET(fog_enabled, lit.fog_enabled);
        SET(fog_density, lit.fog_density);
        SET(fog_height_falloff, lit.fog_height_falloff);
        SET(fog_anisotropy, lit.fog_anisotropy);
        SET(exposure_ev, lit.exposure_ev);
        SET(bloom, lit.bloom);
        SET(tonemapper, lit.tonemapper);
        SET(antialiasing, lit.antialiasing);
        SET(direct_cluster_inspect, lit.direct_cluster_inspect);
        SET(cavity, cavity.enabled);
        SET(cavity_ridge, cavity.ridge);
        SET(cavity_valley, cavity.valley);
        SET(cavity_radius, cavity.radius);
#undef SET
    }
    return view_value(view, option) != previous;
}

Result<void> validate_view_options(const renderer::ViewSettings& view, math::Vector3 focus) {
    if (!math::is_finite(focus) || math::max_component(math::cwise_abs(focus)) > 100000.f)
        return invalid("Invalid lighting focus");
    for (const auto& option : options) {
        const auto value = view_value(view, option.option);
        if (!std::isfinite(value) || value < option.minimum || value > option.maximum ||
            (option.kind != K::number && value != std::floor(value)))
            return invalid("Invalid display option: " + std::string(option.name));
    }
    if (const auto* error = rendering::scene_lighting_error(view.lit))
        return invalid(error);
    return {};
}
} // namespace atelier::editor
