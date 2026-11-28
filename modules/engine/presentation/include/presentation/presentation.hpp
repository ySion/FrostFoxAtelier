#pragma once
#include "rendering/scene_lighting.hpp"

#include "foundation/bit_vector.hpp"
#include "foundation/map.hpp"
#include "foundation/set.hpp"
#include "world/world.hpp"
#include "geometry/mesh.hpp"
#include "material/program_resources.hpp"
#include "procedural/instances.hpp"
#include <cstring>
#include <type_traits>
#include "presentation/presentation.gen.hpp"

// Meshes, procedural generators, lights, cameras and the Environment: component
// types consumed by rendering, registered on a World as capabilities. Persistent
// type names keep the world. prefix of the scene format.
namespace atelier::presentation {

namespace detail {
struct Access;
}

// A real reflected Entity type. Its complete default composition is built by
// create_static_mesh_entity, while Runtime construction establishes storage only.
ACLASS(
    Name = "world.StaticMeshEntity",
    DESC = "用于静态网格的 Entity 类型。编辑器创建时会同时配置空间能力和 StaticMeshComponent；网格与材质保存在组件中。")
class StaticMeshEntity : public world::Entity {
    AGENERATED_BODY(StaticMeshEntity)
};

ACLASS(Name = "world.StaticMeshComponent",
       DESC = "显示网格资源，并按材质槽绑定 Material。可同时启用 Body 碰撞，空间变换来自所属 Entity。")
class StaticMeshComponent : public world::PrimitiveComponent {
    AGENERATED_BODY(StaticMeshComponent)

private:
    friend struct detail::Access;

    AFIELD(Name = "mesh_source", Hidden)
    String mesh_source_;

    geometry::MeshRef geometry_;

    AFIELD(Name = "materials", Hidden, Sequence = "material.Resource")
    ReferenceSequence materials_;

    Array<String> material_slots_;

    BitVector material_overrides_;

    // The mesh asset's default material per slot, resolved with the mesh and never saved.
    AFIELD(Name = "default_materials", Hidden, Sequence = "material.Resource")
    ReferenceSequence default_materials_;

    Array<String> default_slots_;

    std::shared_ptr<const Array<material::ProgramSurfaceRef>> surfaces_;

    geometry::MeshRef surface_geometry_;
};

struct ProceduralOutput;

ACLASS(Name = "world.ProceduralGeneratorEntity",
       DESC = "用于程序化几何与实例散布的 Entity 类型。编辑器创建时会同时配置空间能力和 ProceduralGeneratorComponent。")
class ProceduralGeneratorEntity : public world::Entity {
    AGENERATED_BODY(ProceduralGeneratorEntity)
};

ACLASS(
    Name = "world.ProceduralGeneratorComponent", Validate = "validate",
    DESC =
        "求值 .procgraph 程序化图资产并发布表面网格与按空间块分组的实例集合。点与实例不创建 Entity；空间变换来自所属 Entity。")
class ProceduralGeneratorComponent : public world::SpatialComponent {
    AGENERATED_BODY(ProceduralGeneratorComponent)

public:
    AFIELD(Label = "Override seed", Category = "Generator",
           DESC = "启用后使用此组件的 Seed 覆盖 .procgraph 图根的默认种子，便于同一图资产在场景中生成不同变体。")
    bool override_seed{};

    AFIELD(Label = "Seed", Category = "Generator", Meta("ui.visible_when", "override_seed=true"),
           DESC = "启用 Override seed 时传给程序化图求值的随机种子。")
    std::int32_t seed{};

    AFIELD(Label = "Chunk size (m)", Category = "Generator", HardMin = "0.1", HardMax = "100000",
           DESC = "实例发布时按局部空间立方网格分块的边长（m）。未变化的块跨重算复用局部变换缓冲。")
    float chunk_size{32.f};

private:
    friend struct detail::Access;

    [[nodiscard]] Result<void> validate() const;

    AFIELD(Name = "graph_source", Hidden)
    String graph_source_;

    std::shared_ptr<const ProceduralOutput> output_;

    AFIELD(Name = "materials", Hidden, Sequence = "material.Resource")
    ReferenceSequence materials_;

    Array<String> material_slots_;

    BitVector material_overrides_;

    std::shared_ptr<const Array<material::ProgramSurfaceRef>> surfaces_;

    geometry::MeshRef surface_geometry_;

    AFIELD(Name = "instance_materials", Hidden, Sequence = "material.Resource")
    ReferenceSequence instance_materials_;

    Array<std::shared_ptr<const Array<material::ProgramSurfaceRef>>> instance_surfaces_;

    std::shared_ptr<const ProceduralOutput> instance_output_;
};

AENUM(
    Name = "world.LightKind",
    DESC =
        "Point 点光、Rectangle 矩形面光、Directional 平行光或 Spot 聚光灯。不同种类使用不同的强度单位和形状参数；天光在 Environment 中设置。")
enum class LightKind : std::uint32_t { point, rectangle, directional, spot };

ASTRUCT(Name = "world.LightSettings", Validate = "validate",
        DESC = "灯光的 Scene 作者参数。Color 与 Intensity 一起决定发光量，位置和朝向来自所属 Entity。")
struct LightSettings {
    AGENERATED_BODY(LightSettings)

public:
    AFIELD(DESC = "灯光种类，决定 Intensity 单位及哪些尺寸参数生效。")
    LightKind kind{LightKind::point};

    AFIELD(HardMin = "0", DESC = "非负线性 RGB，逐通道乘以 Intensity。可以大于 1；它不是 sRGB 屏幕颜色。")
    math::Vector3 color{1.f, 1.f, 1.f};

    // Reference-white intensity: point/spot cd, rectangle cd/m^2, directional lux.
    AFIELD(
        HardMin = "0",
        DESC = "参考白色的发光强度，再由 Color 调制。Point 与 Spot 使用 cd；Rectangle 使用 cd/m²；Directional 使用 lux。")
    float intensity{1.f};

    AFIELD(
        HardMin = "0",
        DESC =
            "Point、Spot 和 Rectangle 的作用距离，单位为世界 m，必须大于 0。Directional 忽略此值；Entity 缩放不会改变它。")
    float range{10.f};

    // Point/Spot: metres. Directional: angular radius in radians, below pi/2.
    // Rectangle emission size uses width/height instead.
    AFIELD(
        HardMin = "0",
        DESC =
            "Point 与 Spot 的发光半径使用世界 m；Directional 的角半径使用 rad，必须小于 π/2。它影响软阴影；Rectangle 忽略此值。")
    float source_radius{};

    AFIELD(HardMin = "0", DESC = "Rectangle 沿局部 X 方向的发光宽度，单位为世界 m，必须大于 0。不乘 Entity 的缩放。")
    float width{1.f};

    AFIELD(HardMin = "0", DESC = "Rectangle 沿局部 Y 方向的发光高度，单位为世界 m，必须大于 0。不乘 Entity 的缩放。")
    float height{1.f};

    AFIELD(Label = "Spot inner (°)", HardMin = "0", HardMax = "89.9", Meta("ui.visible_when", "kind=spot"),
           DESC = "Spot 聚光灯无角度衰减的内锥半角（度），必须小于或等于 Spot outer。")
    float spot_inner_degrees{25.f};

    AFIELD(Label = "Spot outer (°)", HardMin = "0.1", HardMax = "89.9", Meta("ui.visible_when", "kind=spot"),
           DESC = "Spot 聚光灯截止外锥半角（度），范围 [0.1, 89.9]，且不小于 Spot inner。")
    float spot_outer_degrees{40.f};

    AFIELD(DESC = "让 Rectangle 的两面都按同样的 Intensity 发光，不把每面的强度减半。其他灯光种类不使用此项。")
    bool two_sided{};

    AFIELD(DESC = "此直接灯是否参与阴影投射，实际效果还受 World 阴影设置控制。")
    bool casts_shadow{true};

    bool operator==(const LightSettings&) const = default;

private:
    [[nodiscard]] Result<void> validate() const;
};

ACLASS(Name = "world.LightComponent",
       DESC = "让 Entity 提供 Point、Rectangle、Directional 或 Spot 光照。所属 Entity 必须有 Transform。")
class LightComponent : public world::SpatialComponent {
    AGENERATED_BODY(LightComponent)

public:
    AFIELD(Category = "Lighting",
           DESC = "此灯的种类、颜色、强度和形状。参数随 Scene 保存；全局曝光与效果在 World Settings 中调整。")
    LightSettings settings;
};

ACLASS(
    Name = "world.CameraComponent", Validate = "validate",
    DESC =
        "提供透视相机。一个 Entity 至多一个，包括派生和已停用组件。位置来自世界变换，朝向只复合空间父链的旋转，不受缩放或镜像影响。")
class CameraComponent : public world::SpatialComponent {
    AGENERATED_BODY(CameraComponent)

public:
    AFIELD(Label = "Vertical FOV (°)", Category = "Camera", HardMin = "1", HardMax = "179",
           DESC = "垂直视野角度。横向视野由窗口宽高比决定。")
    float vertical_fov{45.f};

    AFIELD(Label = "Near clip (m)", Category = "Camera", HardMin = "0.001", HardMax = "100000",
           DESC = "近裁剪距离，单位 m，必须小于 Far clip。")
    float near_clip{.05f};

    AFIELD(Label = "Far clip (m)", Category = "Camera", HardMin = "0.002", HardMax = "1000000",
           DESC = "远裁剪距离，单位 m，必须大于 Near clip。")
    float far_clip{30000.f};

private:
    [[nodiscard]] Result<void> validate() const;
};

AENUM(Name = "world.Antialiasing", DESC = "Off 关闭抗锯齿；SMAA 处理当前画面的边缘；TAA 利用跨帧时域超采样与抗锯齿。")
enum class Antialiasing : std::uint32_t { off, smaa, taa };

AENUM(Name = "world.GiQuality", DESC = "High 使用平衡的屏幕空间探针射线预算；Ultra 提升每探针追踪预算以降低深遮挡噪声。")
enum class GiQuality : std::uint32_t { high, ultra };

AENUM(Name = "world.SkyMode",
      DESC = "None 关闭天光；Physical Atmosphere 使用物理瑞利/米氏散射大气；HDRI Sky Map 使用等距柱状投影贴图或均匀天光。")
enum class SkyMode : std::uint32_t { none, physical_atmosphere, hdri_sky_map };

AENUM(Name = "world.Tonemapper", DESC = "Khronos PBR Neutral 保留中灰与色相保真度；AgX 提供高光胶片式平滑退饱和。")
enum class Tonemapper : std::uint32_t { pbr_neutral, agx };

// Scene Lit settings of the whole World. At most one per World, disabled ones
// included; without an active one the World uses SceneLightingSettings{}.
ACLASS(
    Name = "world.EnvironmentComponent", Validate = "validate",
    DESC =
        "World 的直接光阴影、三层全局光照、反射、物理大气/体积雾与后处理设置。一个 World 至多一个，包括已停用的；没有生效的 Environment 时使用默认值。")
class EnvironmentComponent : public world::Component {
    AGENERATED_BODY(EnvironmentComponent)

public:
    AFIELD(
        Category = "Camera", Weak = "world.Entity",
        DESC =
            "Play 的主相机 Entity。未指定或目标不可用时场景黑屏，UI 不受影响。停用目标保留选择，不自动改选其他相机。")
    ReferenceSlot main_camera;

    AFIELD(Category = "Direct Lighting",
           DESC = "控制 Scene Lit 的三级直接光阴影（VSM 虚拟阴影图 + 半影硬光追 + 接触微阴影）。")
    bool shadows{true};

    AFIELD(Label = "Contact shadow (m)", Category = "Direct Lighting", HardMin = "0", HardMax = "2",
           DESC = "主平行光屏幕空间接触阴影最大追踪步长（m）；0 关闭屏幕接触阴影补充。")
    float contact_shadow_length{.15f};

    AFIELD(Category = "Direct Lighting", HardMin = "1", HardMax = "32",
           DESC = "次级 GI 命中点直接评估的局部光最大候选预算，超出部分进入加权蓄水池无偏采样。")
    std::uint32_t direct_candidates{8};

    AFIELD(Label = "Indirect lighting", Category = "Global Illumination",
           DESC = "启用三层混合漫反射全局光照（GT-VBAO + 屏幕空间探针 + SHaRC/GI-1.2 多跳表面哈希辐射缓存）。")
    bool indirect_lighting{true};

    AFIELD(Label = "GI quality", Category = "Global Illumination",
           DESC = "控制 8×8 屏幕空间辐射探针的追踪采样档位（High / Ultra）。")
    GiQuality gi_quality{GiQuality::high};

    AFIELD(Label = "Indirect gain", Category = "Global Illumination", HardMin = "0", HardMax = "8",
           DESC = "最终合成时对漫反射间接光应用的增益，只乘一次，不写回多跳辐射缓存。")
    float indirect_gain{1.f};

    AFIELD(Label = "GT-VBAO", Category = "Global Illumination",
           DESC = "启用有符号 32 扇区地平线比特掩码环境光遮蔽、弯曲法线锥与亚探针微反弹（GT-VBAO）。")
    bool ssilvb{true};

    AFIELD(Label = "AO radius (m)", Category = "Global Illumination", HardMin = "0.001", HardMax = "100",
           DESC = "GT-VBAO 观察附近表面遮蔽与微反弹的世界空间半径（m）。")
    float ao_radius{.35f};

    AFIELD(Label = "Reflections", Category = "Reflections",
           DESC = "启用三级清漆感知镜面反射（Hi-Z 屏幕步进 + 有界 VNDF 光追回退 + 粗糙表面弯曲法线锥探针评估）。")
    bool reflections{true};

    AFIELD(Label = "Trace distance (m)", Category = "Reflections", HardMin = "0.001", HardMax = "10000",
           DESC = "镜面反射允许追踪的最大世界距离（m）。")
    float reflection_distance{30.f};

    AFIELD(Label = "Max roughness", Category = "Reflections", HardMin = "0", HardMax = "1",
           DESC = "允许执行光线步进与光追反射的最大表面粗糙度；更粗糙表面走零射线预过滤探针路径。")
    float reflection_roughness{.6f};

    AFIELD(Label = "Sky mode", Category = "Sky & Fog",
           DESC = "选择天空背景与远场环境辐射来源：关闭、物理大气散射或 HDRI 天空贴图（无贴图时使用均匀天光）。")
    SkyMode sky_mode{SkyMode::none};

    AFIELD(
        Label = "Sky color", Category = "Sky & Fog", HardMin = "0",
        DESC = "非负线性 RGB，逐通道乘以 Sky intensity；设了天空贴图时再乘贴图的值。可以大于 1；它不是 sRGB 屏幕颜色。")
    math::Vector3 sky_color{1.f, 1.f, 1.f};

    AFIELD(
        Label = "Sky intensity", Category = "Sky & Fog", HardMin = "0",
        DESC =
            "天光的辐亮度（cd/m²）。无贴图时为均匀天空辐亮度；有贴图时为贴图值为 1 处的基准辐亮度，与 Sky color 逐通道乘积不超过 65504。")
    float sky_intensity{.03f};

    AFIELD(
        Label = "Sky rotation (°)", Category = "Sky & Fog", HardMin = "-180", HardMax = "180",
        DESC = "天空贴图绕世界 +Y 的偏航角（度），与 Transform 绕 +Y 的正旋转同向。")
    float sky_rotation{};

    AFIELD(Label = "Volumetric fog", Category = "Sky & Fog",
           DESC = "启用视锥体体素（160×90×64 Froxel）单次散射与时域积分体积雾。")
    bool fog_enabled{};

    AFIELD(Label = "Fog density", Category = "Sky & Fog", HardMin = "0", HardMax = "10",
           DESC = "海平面基准体积雾消光密度（1/m）。")
    float fog_density{.01f};

    AFIELD(Label = "Fog height falloff", Category = "Sky & Fog", HardMin = "0.001", HardMax = "10",
           DESC = "体积雾随世界高度 Y 指数衰减的系数。")
    float fog_height_falloff{.2f};

    AFIELD(Label = "Fog anisotropy", Category = "Sky & Fog", HardMin = "-0.95", HardMax = "0.95",
           DESC = "Henyey-Greenstein 相位函数前向/后向散射各向异性参数 g。")
    float fog_anisotropy{.3f};

    AFIELD(Label = "Exposure (EV)", Category = "Post & Display", HardMin = "-16", HardMax = "16",
           DESC = "控制最终显示的曝光。每增加 1 EV，曝光倍率翻倍；不改变场景线性 HDR 历史中的光照能量。")
    float exposure_ev{};

    AFIELD(Category = "Post & Display",
           DESC = "启用物理软膝盖 Karis 抗萤火虫 6 级金字塔泛光（Bloom）。")
    bool bloom{true};

    AFIELD(Category = "Post & Display",
           DESC = "选择 HDR 到显示空间的色调映射曲线：Khronos PBR Neutral 或 AgX。")
    Tonemapper tonemapper{Tonemapper::agx};

    AFIELD(Category = "Post & Display",
           DESC = "选择抗锯齿方式：Off 关闭、SMAA 单帧形态学抗锯齿或 TAA 时域抗锯齿。")
    Antialiasing antialiasing{Antialiasing::smaa};

private:
    friend struct detail::Access;

    [[nodiscard]] Result<void> validate() const;

    AFIELD(Name = "sky_map", Hidden)
    String sky_map_;

    rendering::SkyMapRef sky_map_resolved_;
};

[[nodiscard]] Result<void> validate_light(Runtime&, const LightSettings&);

// Reads authored settings, including detached/disabled objects. Does not root them.
[[nodiscard]] Result<LightSettings> read_light(Runtime&, ObjectId light_component);

// Typed whole-value authoring: a value that fails its type rules is rejected, not clamped;
// World::write then applies it with the World's admissions and exact restore.
[[nodiscard]] Result<void> set_light(world::World&, ObjectId light_component, LightSettings);

struct Types {
    TypeId static_mesh_component;

    TypeId static_mesh_entity;

    TypeId light_kind;

    TypeId light_settings;

    TypeId light_component;

    TypeId environment_component;

    TypeId camera_component;

    TypeId procedural_generator_component;

    TypeId procedural_generator_entity;
};

// Call once per Runtime, after world::register_types.
[[nodiscard]] Result<Types> register_types(Runtime&);

struct MaterialBinding {
    String slot;

    ObjectRoot resource;

    bool overridden{true}; // Explicit instance assignment; false is a mesh default.

    MaterialBinding() = default;

    MaterialBinding(Utf8Text slot_name, ObjectRoot bound_resource = {}, bool is_overridden = true)
        : slot(std::move(slot_name).into_string()), resource(std::move(bound_resource)), overridden(is_overridden) {}
};

struct StaticMeshEntityDesc {
    geometry::MeshRef mesh;

    world::Trs transform{};

    StringView mesh_source{};

    Array<MaterialBinding> default_materials{}; // See MeshBinding::defaults.
};

// Returns a complete Detached spatial Entity with one StaticMeshComponent.
// A null mesh is valid and produces no presentation. No World registration,
// lifecycle notification or Begin occurs until the caller invokes World::attach.
// Requires an open World at a host boundary; a running World is supported.
[[nodiscard]] Result<ObjectRoot> create_static_mesh_entity(world::World&, const StaticMeshEntityDesc&);

[[nodiscard]] geometry::MeshRef build_procedural_surface_mesh(const procedural::Geometry&);

inline math::Matrix4 procedural_affine_matrix(const procedural::Affine& a) noexcept {
    return math::Matrix4{
        {a[0], a[1], a[2], 0.f, a[3], a[4], a[5], 0.f, a[6], a[7], a[8], 0.f, a[9], a[10], a[11], 1.f}};
}

// Row-vector local * world composition in double intermediates with finite and
// normal-transform admission matching the renderer's instance validation.
[[nodiscard]] inline Optional<math::Matrix4> compose_instance_world(const procedural::Affine& a,
                                                                    const math::Matrix4& gen_world) noexcept {
    for (const float v : a)
        if (!std::isfinite(v))
            return {};
    if (!math::is_finite(gen_world))
        return {};
    const math::Matrix4d local{{double(a[0]), double(a[1]), double(a[2]), 0., double(a[3]), double(a[4]), double(a[5]),
                                0., double(a[6]), double(a[7]), double(a[8]), 0., double(a[9]), double(a[10]),
                                double(a[11]), 1.}};
    const math::Matrix4d product = local * static_cast<math::Matrix4d>(gen_world);
    auto out = static_cast<math::Matrix4>(product);
    out[3] = 0.f;
    out[7] = 0.f;
    out[11] = 0.f;
    out[15] = 1.f;
    if (!math::is_finite(out))
        return {};
    if (!math::normal_transform({out[0], out[1], out[2], out[4], out[5], out[6], out[8], out[9], out[10]}))
        return {};
    return out;
}

struct ProceduralAssetSlotRange {
    String key;

    geometry::MeshRef mesh;

    std::size_t default_offset{};

    std::size_t default_count{};

    Array<String> default_slots{};

    bool operator==(const ProceduralAssetSlotRange&) const = default;
};

struct ProceduralMeshDependency {
    String key;

    geometry::MeshRef mesh;

    Array<std::pair<String, String>> slot_materials{};

    bool operator==(const ProceduralMeshDependency&) const = default;
};

struct ProceduralOutput {
    std::uint64_t generation{};

    std::shared_ptr<const procedural::Geometry> geometry;

    geometry::MeshRef surface_mesh;

    procedural::Publication publication;

    Array<ProceduralAssetSlotRange> assets;

    Array<ProceduralMeshDependency> mesh_dependencies{};
};

struct ProceduralInstanceAsset {
    String key;

    geometry::MeshRef mesh;

    Array<MaterialBinding> defaults{};
};

struct ProceduralSource {
    String source;

    std::shared_ptr<const ProceduralOutput> output;
};

struct ProceduralBinding {
    String source;

    std::shared_ptr<const ProceduralOutput> output;

    Array<ProceduralInstanceAsset> resolved_assets{};

    ProceduralBinding() = default;

    ProceduralBinding(Utf8Text graph_source, std::shared_ptr<const ProceduralOutput> graph_output = {},
                      Array<ProceduralInstanceAsset> assets = {})
        : source(std::move(graph_source).into_string()), output(std::move(graph_output)),
          resolved_assets(std::move(assets)) {}
};

struct ProceduralGeneratorEntityDesc {
    world::Trs transform{};

    StringView graph_source{};

    std::shared_ptr<const ProceduralOutput> output{};

    Array<ProceduralInstanceAsset> resolved_assets{};
};

[[nodiscard]] Result<ObjectRoot> create_procedural_generator_entity(world::World&,
                                                                    const ProceduralGeneratorEntityDesc&);

[[nodiscard]] Result<void> set_procedural_binding(Runtime&, ObjectId generator_component, ProceduralBinding);

[[nodiscard]] Result<std::shared_ptr<const ProceduralOutput>>
publish_procedural_binding(Runtime&, ObjectId generator_component, StringView source, ProceduralOutput output,
                           Span<const ProceduralInstanceAsset> resolved_assets);

// Reads the graph key and published output handle without reading or rooting instance materials.
[[nodiscard]] Result<ProceduralSource> read_procedural_source(Runtime&, ObjectId generator_component);

[[nodiscard]] Result<ProceduralBinding> read_procedural_binding(Runtime&, ObjectId generator_component);

struct RenderItem {
    ObjectId entity;

    ObjectId component;

    math::Matrix4 world;

    geometry::MeshRef geometry;

    // Exact immutable candidates. The host's MaterialBatch chooses the adopted
    // group; presentation neither owns GPU resources nor publishes that group.
    std::shared_ptr<const Array<material::ProgramSurfaceRef>> surfaces{};
};

struct LightRenderItem {
    ObjectId entity;

    ObjectId component;

    math::Vector3 position;

    // Orthonormal world axes from the rotation chain, excluding scale/shear.
    // direction is propagation along local +Z; rectangle lies in local XY.
    math::Vector3 direction;

    math::Vector3 right;

    math::Vector3 up;

    // Range and physical dimensions are world metres, not scaled. Directional
    // source_radius is angular (radians); see LightSettings.
    LightSettings settings;
};

struct CameraMatrices {
    math::Matrix4 view;

    math::Matrix4 projection;

    float far_depth{}; // Derived near/far clipping depth, not background zero.
};

// Values, not borrowed payloads; extracting a camera holds no roots afterwards.
struct CameraSnapshot {
    ObjectId entity;

    ObjectId component;

    math::Vector3 position;

    math::Quaternion rotation;

    float vertical_fov{};

    float near_clip{};

    float far_clip{};

    // Left-handed +Z forward, row vectors; infinite reversed Z (depth=near/view Z),
    // with a separate finite +Z far clipping boundary in the returned snapshot.
    [[nodiscard]] Result<CameraMatrices> matrices(float aspect) const;
};

// Configure detached objects before attachment, or replace a member's immutable
// mesh resource at a host boundary. No callback/dispatch reentry is permitted.
[[nodiscard]] Result<void> set_geometry(Runtime&, ObjectId static_mesh_component, geometry::MeshRef);

// `defaults` are the mesh asset's slot materials (overridden false). They are not saved: hosts
// resolve them from the mesh source wherever they resolve the mesh. An explicit binding wins.
struct MeshBinding {
    String source;

    geometry::MeshRef resolved;

    Array<MaterialBinding> defaults{};

    MeshBinding() = default;

    MeshBinding(Utf8Text mesh_source, geometry::MeshRef mesh_resolved = {}, Array<MaterialBinding> mesh_defaults = {})
        : source(std::move(mesh_source).into_string()), resolved(std::move(mesh_resolved)),
          defaults(std::move(mesh_defaults)) {}
};

struct StaticMeshState {
    MeshBinding mesh;
};

// Source and resolved immutable content change together. Bare set_geometry
// clears provenance. These snapshots also support detached and null-mesh data.
[[nodiscard]] Result<void> set_mesh_binding(Runtime&, ObjectId, MeshBinding);

// Geometry consumers capture the exact component's key/content without reading or rooting its materials.
[[nodiscard]] Result<geometry::MeshSource> read_mesh_source(Runtime&, ObjectId);

[[nodiscard]] Result<StaticMeshState> read_static_mesh(Runtime&, ObjectId);

// An Environment's sky map: the persistent asset key and the map it resolved to, null when
// the key is empty or did not resolve (the sky then renders as map = 1). Only this pair
// changes the key; it is hidden from field paths.
struct SkyMapBinding {
    String source;

    rendering::SkyMapRef resolved;

    SkyMapBinding() = default;

    SkyMapBinding(Utf8Text sky_source, rendering::SkyMapRef sky_resolved = {})
        : source(std::move(sky_source).into_string()), resolved(std::move(sky_resolved)) {}
};

[[nodiscard]] Result<void> set_sky_map_binding(Runtime&, ObjectId environment_component, SkyMapBinding);

[[nodiscard]] Result<SkyMapBinding> read_sky_map(Runtime&, ObjectId environment_component);

using geometry::material_slot_name;

[[nodiscard]] Result<Array<MaterialBinding>> read_material_bindings(Runtime&, ObjectId);

[[nodiscard]] Result<void> set_material_bindings(Runtime&, ObjectId, Span<const MaterialBinding>);

// Shares unchanged captures; normally unbound slots use the engine world-grid MS.
[[nodiscard]] Result<std::shared_ptr<const Array<material::ProgramSurfaceRef>>> material_surfaces(Runtime&, ObjectId);

namespace detail {
// Values that passed validation, by bucket position. Validators are pure checks
// of a value's bytes and published types never change, so the same bytes pass
// again; any other value at that position is validated again.
template <class T>
class PassedValues {
    static_assert(std::is_trivially_copyable_v<T>);

    Array<Optional<T>> values_;

public:
    void resize(std::size_t size) { values_.resize(size); }

    [[nodiscard]] bool contains(std::size_t at, const T& value) const noexcept {
        return values_[at] && std::memcmp(&*values_[at], &value, sizeof(T)) == 0;
    }

    void insert(std::size_t at, const T& value) noexcept { std::memcpy(&values_[at].emplace(), &value, sizeof(T)); }
};

class Presented;

struct BucketTracker {
    BitVector materials_dirty; // Component/binding or resource changed, not just space.

    Array<Set<ObjectId>> material_dependencies;

    Map<ObjectId, Set<std::uint32_t>> material_dependents;

    // Each Entity on a slot's spatial chain leads to that slot; `components` maps its component to the slot.
    Map<ObjectId, Array<std::uint32_t>> dependents;

    Map<ObjectId, std::uint32_t> components;

    // Slot s's chain, its own Entity first, is chains[chain_starts[s], chain_starts[s + 1]).
    Array<ObjectId> chains;

    Array<std::uint32_t> chain_starts;

    Array<std::uint32_t> dirty;

    Array<std::uint32_t> volatile_slots;

    Array<std::uint32_t> presented_list;

    BitVector volatile_flags;

    Array<std::uint64_t> marks;

    Array<std::uint64_t> presented_marks; // Serial of the update that last listed each slot.

    void clear_layout();

    void reset(std::size_t count);

    void mark(std::uint32_t slot, std::uint64_t serial, bool material = false);

    void index_materials(std::size_t slot, Set<ObjectId> dependencies);
};
}

// One position of the mesh bucket in the cached extraction.
struct MeshSlot {
    Optional<RenderItem> item; // Absent while isolated or degenerate.

    // Where `item` is drawn at the latest update when a presented pose moves it off item->world.
    Optional<math::Matrix4> presented;

    std::uint64_t changed{}; // Scene::update serial that last changed `item` or `presented`.
};

struct GeneratorRenderCollection {
    geometry::MeshRef geometry;

    std::array<std::int32_t, 3> chunk{};

    std::shared_ptr<const Array<procedural::Affine>> transforms;

    std::shared_ptr<const Array<material::ProgramSurfaceRef>> surfaces;
};

struct GeneratorRenderItem {
    ObjectId entity;

    ObjectId component;

    math::Matrix4 world;

    std::uint64_t output_generation{};

    geometry::MeshRef surface_geometry;

    std::shared_ptr<const Array<material::ProgramSurfaceRef>> surface_materials;

    Array<GeneratorRenderCollection> collections;
};

struct GeneratorSlot {
    Optional<GeneratorRenderItem> item;

    Optional<math::Matrix4> presented;

    std::uint64_t changed{};
};

// The presentation of one World; a World without a Scene created by its host is
// not presented. Creating it registers the mesh (a mesh needs geometry),
// procedural generator (a generator needs renderable output), light, camera and
// Environment capabilities and the unique Environment/camera admissions on that
// World; extraction reads only those buckets and reports object failures to the
// World's log under its failure policy. One per World, created at a host
// boundary and destroyed before the World.
class Scene final {
public:
    [[nodiscard]] static Result<Scene> create(world::World&, const Types&);

    [[nodiscard]] world::CapabilityId meshes() const noexcept { return meshes_; }

    [[nodiscard]] world::CapabilityId generators() const noexcept { return generators_; }

    [[nodiscard]] world::CapabilityId lights() const noexcept { return lights_; }

    [[nodiscard]] world::CapabilityId cameras() const noexcept { return cameras_; }

    // Unique in the process: no later Scene takes it, so consumers of the cache
    // can tell a replaced document from the one they synchronized with.
    [[nodiscard]] std::uint64_t identity() const noexcept { return identity_; }

    // Brings the cached extraction up to date in one presentation pass and one
    // lighting pass, and returns the presentation version: the serial of the
    // latest update that changed a mesh slot, the mesh layout, the lights or the
    // Environment settings. Runtime write stamps find every value written since
    // the previous update, including past World (Runtime::write, native code, JIT):
    // a mesh slot is read again when its component or an Entity on its spatial
    // chain was written, the lights when any light or Entity on a light's chain
    // was, and everything after World::revision advances; only changed meshes are
    // rebuilt. Material captures index live parent/texture dependencies; their
    // writes or deaths refresh affected slots, while spatial-only edits reuse
    // surfaces. Slots not in the cache (isolated, degenerate) are read on every update, as
    // are the lights while one is not drawn, so failures are reported as by
    // extraction on every update and persistent ones keep counting repeats.
    // Must be called at a host safe point (Runtime::change_cursor is busy otherwise).
    // Each mesh and light is drawn relative to the nearest Entity of `presented`
    // on its spatial chain, itself included: moved rigidly from that Entity's
    // world pose to its presented pose, or left at its World matrix when the two
    // are equal. Hosts list every simulated Entity below a moved one. Authoritative
    // items and render_items() never include presented poses.
    [[nodiscard]] Result<std::uint64_t> update(Span<const world::PresentedPose> presented = {}) const;

    // The cache as of the latest update, by mesh bucket position.
    [[nodiscard]] Span<const MeshSlot> mesh_slots() const noexcept { return mesh_slots_; }

    [[nodiscard]] Span<const GeneratorSlot> generator_slots() const noexcept { return generator_slots_; }

    // Serial of the latest update that rebuilt every slot after the mesh bucket
    // changed (World::revision advanced); slot positions keep their component until then.
    [[nodiscard]] std::uint64_t mesh_layout() const noexcept { return mesh_layout_; }

    // Lights as drawn at the latest update, presented poses applied.
    [[nodiscard]] Span<const LightRenderItem> light_snapshot() const noexcept { return lights_cache_; }

    [[nodiscard]] std::uint64_t lights_changed() const noexcept { return lights_changed_; }

    // update(), then the drawn items in Entity/composition order.
    [[nodiscard]] Result<Array<RenderItem>> render_items() const;

    // Enabled members, in Entity/composition order. Stopped Worlds also present.
    // Extracted afresh on every call.
    [[nodiscard]] Result<Array<LightRenderItem>> light_items() const;

    // The active EnvironmentComponent's settings, or SceneLightingSettings{}
    // without one. An invalid Environment is reported as invalid_value in the
    // lighting phase's current pass and yields the defaults; see contain.
    [[nodiscard]] Result<rendering::SceneLightingSettings> environment() const;

    // Author preview of one member Entity's unique camera, including disabled ones.
    [[nodiscard]] Result<CameraSnapshot> camera(ObjectId entity) const;

    // Resolve afresh after every update (including late flush), not by revision.
    // Missing/stale/foreign/disabled selections fail without changing the reference.
    // Hosts report the error and draw black with UI; they validate focus before Lit submission.
    [[nodiscard]] Result<CameraSnapshot> main_camera() const;

private:
    Scene(world::World& world, const Types& types) noexcept : world_(&world), types_(types) {}

    // Re-reads the mesh slots whose sources were written since the latest update (all
    // of them after a layout change) in one presentation pass; changed slots take `serial`.
    // Without `presented` the slots keep their presented matrices.
    Result<void> update_meshes(std::uint64_t serial, const detail::Presented* presented) const;

    // Re-reads one mesh slot; true when its item changed.
    Result<bool> update_mesh(world::World::SpatialPass&, std::size_t slot, const world::CapabilityMember&) const;

    Result<bool> update_generator(world::World::SpatialPass&, std::size_t slot, const world::CapabilityMember&) const;

    // Rebuilds the spatial chains of the mesh, generator and light sources for a new layout.
    Result<void> index_layout(world::World::SpatialPass&, Span<const world::CapabilityMember> meshes,
                              Span<const world::CapabilityMember> generators) const;

    // `failed`: an active light was not drawn. `moved`: a presented pose moved a light.
    Result<Array<LightRenderItem>> extract_lights(const detail::Presented*, bool* failed = nullptr,
                                                  bool* moved = nullptr) const;

    world::World* world_;

    Types types_;

    world::CapabilityId meshes_;

    world::CapabilityId generators_;

    world::CapabilityId lights_;

    world::CapabilityId environments_;

    world::CapabilityId cameras_;

    // Aligned with the meshes and lights buckets at each extraction call.
    mutable detail::PassedValues<LightSettings> valid_lights_;

    mutable Array<MeshSlot> mesh_slots_;

    mutable detail::BucketTracker mesh_tracker_;

    mutable Array<GeneratorSlot> generator_slots_;

    mutable detail::BucketTracker generator_tracker_;

    mutable Map<std::size_t, std::shared_ptr<const Array<material::ProgramSurfaceRef>>> fallback_surfaces_;

    mutable Array<LightRenderItem> lights_cache_;

    mutable rendering::SceneLightingSettings environment_cache_;

    // Writes stamped after `cursor_` are not yet read; valid while `tracked_`.
    mutable std::uint64_t cursor_{};

    mutable bool tracked_{};

    mutable Array<ObjectId> written_;

    // Light components and the Entities on their chains.
    mutable Set<ObjectId> light_sources_;

    mutable bool lights_dirty_{true};

    mutable bool lights_failed_{};

    mutable bool lights_moved_{};

    std::uint64_t identity_{};

    mutable std::uint64_t serial_{};

    mutable std::uint64_t version_{};

    mutable std::uint64_t mesh_layout_{};

    mutable std::uint64_t lights_changed_{};

    mutable std::uint64_t layout_revision_{};

    mutable Array<world::CapabilityMember> mesh_members_;

    mutable Array<world::CapabilityMember> generator_members_;
};

}
