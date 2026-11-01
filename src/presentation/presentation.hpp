#pragma once
#include "rendering/scene_lighting.hpp"

#include "world/world.hpp"
#include "geometry/mesh.hpp"
#include "material/program_resources.hpp"
#include <cstring>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include "presentation/presentation.gen.hpp"

// Meshes, lights and the Environment: component types consumed by rendering,
// registered on a World as capabilities. Persistent type names keep the world.
// prefix of the scene format.
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

    std::vector<std::string> material_slots_;

    std::vector<bool> material_overrides_;

    // The mesh asset's default material per slot, resolved with the mesh and never saved.
    AFIELD(Name = "default_materials", Hidden, Sequence = "material.Resource")
    ReferenceSequence default_materials_;

    std::vector<std::string> default_slots_;

    std::shared_ptr<const std::vector<material::ProgramSurfaceRef>> surfaces_;

    geometry::MeshRef surface_geometry_;
};

AENUM(
    Name = "world.LightKind",
    DESC =
        "Point 点光、Rectangle 矩形面光或 Directional 平行光。不同种类使用不同的强度单位和形状参数；天光在 Environment 中设置。")
enum class LightKind : std::uint32_t { point, rectangle, directional };

ASTRUCT(Name = "world.LightSettings",
        DESC = "灯光的 Scene 作者参数。Color 与 Intensity 一起决定发光量，位置和朝向来自所属 Entity。")
struct LightSettings {
    AGENERATED_BODY(LightSettings)

public:
    AFIELD(DESC = "灯光种类，决定 Intensity 单位及哪些尺寸参数生效。")
    LightKind kind{LightKind::point};

    AFIELD(HardMin = "0", DESC = "非负线性 RGB，逐通道乘以 Intensity。可以大于 1；它不是 sRGB 屏幕颜色。")
    math::Vector3 color{1.f, 1.f, 1.f};

    // Reference-white intensity: point cd, rectangle cd/m^2, directional lux.
    AFIELD(HardMin = "0",
           DESC = "参考白色的发光强度，再由 Color 调制。Point 使用 cd；Rectangle 使用 cd/m²；Directional 使用 lux。")
    float intensity{1.f};

    AFIELD(
        HardMin = "0",
        DESC = "Point 和 Rectangle 的作用距离，单位为世界 m，必须大于 0。Directional 忽略此值；Entity 缩放不会改变它。")
    float range{10.f};

    // Point: metres. Directional: angular radius in radians, below pi/2.
    // Rectangle emission size uses width/height instead.
    AFIELD(
        HardMin = "0",
        DESC =
            "Point 的发光半径使用世界 m；Directional 的角半径使用 rad，必须小于 π/2。它影响软阴影；Rectangle 忽略此值。")
    float source_radius{};

    AFIELD(HardMin = "0", DESC = "Rectangle 沿局部 X 方向的发光宽度，单位为世界 m，必须大于 0。不乘 Entity 的缩放。")
    float width{1.f};

    AFIELD(HardMin = "0", DESC = "Rectangle 沿局部 Y 方向的发光高度，单位为世界 m，必须大于 0。不乘 Entity 的缩放。")
    float height{1.f};

    AFIELD(DESC = "让 Rectangle 的两面都按同样的 Intensity 发光，不把每面的强度减半。其他灯光种类不使用此项。")
    bool two_sided{};

    AFIELD(DESC = "此直接灯是否参与阴影投射，实际效果还受 World 阴影设置控制。")
    bool casts_shadow{true};

    bool operator==(const LightSettings&) const = default;
};

ACLASS(Name = "world.LightComponent",
       DESC = "让 Entity 提供 Point、Rectangle 或 Directional 光照。所属 Entity 必须有 Transform。")
class LightComponent : public world::SpatialComponent {
    AGENERATED_BODY(LightComponent)

public:
    AFIELD(Category = "Lighting",
           DESC = "此灯的种类、颜色、强度和形状。参数随 Scene 保存；全局曝光与效果在 World Settings 中调整。")
    LightSettings settings;
};

ACLASS(
    Name = "world.CameraComponent",
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
};

AENUM(Name = "world.Antialiasing", DESC = "Off 关闭抗锯齿；SMAA 处理当前画面的边缘；TAA 和 SMAA T2x 还利用跨帧信息。")
enum class Antialiasing : std::uint32_t { off, taa, smaa, smaa_t2x };

AENUM(Name = "world.ProbePlacement",
      DESC = "Fixed 使用固定的世界坐标范围；Follow Focus 把范围作为相对于 lighting focus 的偏移，跟随时按整格移动。")
enum class ProbePlacement : std::uint32_t { fixed, follow_focus };

ASTRUCT(Name = "world.ProbeVolume",
        DESC = "一个 DDGI 探针 Volume 的范围与密度。所有启用的 Volume 共用 4096 个探针和每帧更新预算。")
struct ProbeVolume {
    AGENERATED_BODY(ProbeVolume)

public:
    AFIELD(DESC = "启用这个 DDGI Volume，让它参与探针更新与间接光采样。所有启用的 Volume 共用探针数量和每帧更新预算。")
    bool enabled{};

    AFIELD(
        DESC =
            "Fixed 使用固定的世界坐标范围；Follow Focus 把范围作为相对于 lighting focus 的偏移。跟随时按整格移动，保持探针间距。")
    ProbePlacement placement{ProbePlacement::fixed};

    AFIELD(
        Label = "Min (m)", HardMin = "-100000", HardMax = "100000",
        DESC =
            "Volume 范围的最小 XYZ，单位 m。Fixed 使用世界坐标，Follow Focus 使用相对于 lighting focus 的偏移。每个分量必须小于对应的最大值。")
    math::Vector3 minimum{-6.f, -1.f, -6.f};

    AFIELD(
        Label = "Max (m)", HardMin = "-100000", HardMax = "100000",
        DESC =
            "Volume 范围的最大 XYZ，单位 m。Fixed 使用世界坐标，Follow Focus 使用相对于 lighting focus 的偏移。每个分量必须大于对应的最小值。")
    math::Vector3 maximum{6.f, 5.f, 6.f};

    AFIELD(HardMin = "2", HardMax = "32",
           DESC = "X 轴的探针数量。范围不变时，增加数量会缩小间距，同时增加存储和更新工作量。")
    std::uint32_t count_x{8};

    AFIELD(HardMin = "2", HardMax = "32",
           DESC = "Y 轴的探针数量。范围不变时，增加数量会缩小间距，同时增加存储和更新工作量。")
    std::uint32_t count_y{4};

    AFIELD(HardMin = "2", HardMax = "32",
           DESC = "Z 轴的探针数量。范围不变时，增加数量会缩小间距，同时增加存储和更新工作量。")
    std::uint32_t count_z{8};

    AFIELD(
        Label = "Blend (cells)", HardMin = "0", HardMax = "2",
        DESC =
            "Volume 边缘的过渡宽度，以探针格子为单位。边缘贡献会平滑交给其他覆盖该处的 Volume；它不会补出没有探针覆盖的区域。")
    float blend_cells{};

    bool operator==(const ProbeVolume&) const = default;
};

// Scene Lit settings of the whole World. At most one per World, disabled ones
// included; without an active one the World uses SceneLightingSettings{}.
ACLASS(
    Name = "world.EnvironmentComponent",
    DESC =
        "World 的光照、天光、效果与 DDGI 设置。一个 World 至多一个，包括已停用的；没有生效的 Environment 时使用默认值。它不需要 Entity 的 Transform。")
class EnvironmentComponent : public world::Component {
    AGENERATED_BODY(EnvironmentComponent)

public:
    AFIELD(
        Category = "Camera", Weak = "world.Entity",
        DESC =
            "Play 的主相机 Entity。未指定或目标不可用时场景黑屏，UI 不受影响。停用目标保留选择，不自动改选其他相机。")
    ReferenceSlot main_camera;

    AFIELD(Category = "Lighting",
           DESC = "控制 Scene Lit 的灯光阴影。灯光自身的 Cast Shadows 设置仍决定该光源是否投影。")
    bool shadows{true};

    AFIELD(Label = "DDGI", Category = "Propagation",
           DESC = "启用探针场提供的间接漫反射光。覆盖范围、密度和更新预算由 Volume 与 Propagation 参数决定。")
    bool ddgi{true};

    AFIELD(Label = "GTAO", Category = "Effects", DESC = "用屏幕上的几何信息估计附近遮挡，突出接触与凹角。")
    bool gtao{true};

    AFIELD(Label = "Reflections (SSSR)", Category = "Effects",
           DESC = "启用屏幕空间反射，沿当前画面中的表面信息寻找反射命中。屏幕外或被遮住的表面信息有限。")
    bool sssr{true};

    AFIELD(Category = "Effects",
           DESC = "让画面中的高亮部分向周围扩散，形成光晕。它改变显示效果，不会把光照写回 DDGI 探针。")
    bool bloom{true};

    AFIELD(Category = "Effects",
           DESC = "选择抗锯齿方式。SMAA 处理当前画面的边缘；TAA 和 SMAA T2x 还利用跨帧信息；Off 关闭抗锯齿。")
    Antialiasing antialiasing{Antialiasing::smaa};

    AFIELD(Label = "Exposure (EV)", Category = "Lighting", HardMin = "-16", HardMax = "16",
           DESC = "控制最终显示的曝光。每增加 1 EV，曝光倍率翻倍；不改变场景线性 HDR 历史中的光照能量。")
    float exposure_ev{};

    AFIELD(Category = "Lighting", HardMin = "1", HardMax = "32",
           DESC = "每个接收点初始挑选的局部光候选数量。增加候选会增加直接光采样工作量；它不是场景允许存在的灯光数量。")
    std::uint32_t direct_candidates{8};

    AFIELD(Label = "AO radius (m)", Category = "Effects", HardMin = "0.001", HardMax = "100",
           DESC = "GTAO 观察附近表面的半径，单位 m。应结合场景的实际尺度调整。")
    float ao_radius{.35f};

    AFIELD(Label = "Trace distance (m)", Category = "Effects", HardMin = "0.001", HardMax = "10000",
           DESC = "屏幕空间反射允许追踪的最大距离，单位 m。增加距离不能补出画面外缺失的表面信息。")
    float reflection_distance{30.f};

    AFIELD(Label = "Max roughness", Category = "Effects", HardMin = "0", HardMax = "1",
           DESC = "允许进行屏幕空间反射的最大表面粗糙度。超过此值的表面不执行该反射追踪。")
    float reflection_roughness{.6f};

    AFIELD(
        Label = "Updates / frame", Category = "Propagation", HardMin = "1", HardMax = "4096",
        DESC =
            "每帧安排更新的探针数量，所有 Volume 共用，初始化也计入预算。增大可更快更新场中的照明，同时增加单帧工作量。")
    std::uint32_t probe_update_budget{64};

    AFIELD(Label = "Rays / probe", Category = "Propagation", HardMin = "1", HardMax = "256",
           DESC = "每次更新一个探针所使用的照明射线数量。更多射线通常能降低采样噪声，但需要更多 GPU 工作。")
    std::uint32_t probe_rays{128};

    AFIELD(Label = "Feedback", Category = "Propagation", HardMin = "0", HardMax = "0.99",
           DESC = "DDGI 将上一轮间接光送回探针场的运输系数。0 关闭这部分反馈；改变它会使旧探针照明失效并重新积累。")
    float probe_bounce{.85f};

    AFIELD(Category = "Propagation", HardMin = "0", HardMax = "8",
           DESC = "最终合成时对 DDGI 间接漫反射应用的增益，只乘一次，不回写探针，也不补出缺失的空间覆盖。")
    float indirect_gain{1.f};

    AFIELD(Label = "Sky", Category = "Sky", DESC = "启用天光。关闭时环境照明为零；颜色、强度、天空贴图和旋转都保留。")
    bool sky_enabled{};

    AFIELD(
        Label = "Sky color", Category = "Sky", HardMin = "0",
        DESC = "非负线性 RGB，逐通道乘以 Sky intensity；设了天空贴图时再乘贴图的值。可以大于 1；它不是 sRGB 屏幕颜色。")
    math::Vector3 sky_color{1.f, 1.f, 1.f};

    AFIELD(
        Label = "Sky intensity", Category = "Sky", HardMin = "0",
        DESC =
            "天光的辐亮度，单位 cd/m²。没有天空贴图时是均匀天空的辐亮度；有贴图时是贴图值为 1 处的辐亮度，实际辐亮度再乘贴图的值和 Sky color。它与 Sky color 每个通道的乘积不能超过 65504；贴图里比这更亮的值（比如太阳）由渲染器另外处理。")
    float sky_intensity{.03f};

    AFIELD(
        Label = "Sky rotation (°)", Category = "Sky", HardMin = "-180", HardMax = "180",
        DESC =
            "天空贴图绕世界 +Y 的偏航角，单位度，与 Transform 绕 +Y 的正旋转同向，可用来把贴图里的太阳对到 Directional 光的方向。没有贴图时不起作用。±180 是同一方向。")
    float sky_rotation{};

    AFIELD(Label = "Volume 0", Category = "DDGI Volume",
           DESC = "DDGI Volume 槽位 0，优先级最高，适合最精细的局部覆盖。")
    ProbeVolume volume0{.enabled = true};

    AFIELD(Label = "Volume 1", Category = "DDGI Volume", DESC = "DDGI Volume 槽位 1。")
    ProbeVolume volume1;

    AFIELD(Label = "Volume 2", Category = "DDGI Volume", DESC = "DDGI Volume 槽位 2。")
    ProbeVolume volume2;

    AFIELD(Label = "Volume 3", Category = "DDGI Volume", DESC = "DDGI Volume 槽位 3，优先级最低，适合最粗的整体覆盖。")
    ProbeVolume volume3;

private:
    friend struct detail::Access;

    AFIELD(Name = "sky_map", Hidden)
    String sky_map_;

    rendering::SkyMapRef sky_map_resolved_;
};

// The renderer's view of an Environment's authored values.
[[nodiscard]] rendering::SceneLightingSettings environment_settings(const EnvironmentComponent&) noexcept;

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
};

// Call once per Runtime, after world::register_types.
[[nodiscard]] Result<Types> register_types(Runtime&);

struct MaterialBinding {
    std::string slot;

    ObjectRoot resource;

    bool overridden{true}; // Explicit instance assignment; false is a mesh default.
};

struct StaticMeshEntityDesc {
    geometry::MeshRef mesh;

    world::Trs transform{};

    std::string_view mesh_source{};

    std::vector<MaterialBinding> default_materials{}; // See MeshBinding::defaults.
};

// Returns a complete Detached spatial Entity with one StaticMeshComponent.
// A null mesh is valid and produces no presentation. No World registration,
// lifecycle notification or Begin occurs until the caller invokes World::attach.
// Requires an open World at a host boundary; a running World is supported.
[[nodiscard]] Result<ObjectRoot> create_static_mesh_entity(world::World&, const StaticMeshEntityDesc&);

struct RenderItem {
    ObjectId entity;

    ObjectId component;

    math::Matrix4 world;

    geometry::MeshRef geometry;

    // Exact immutable candidates. The host's MaterialBatch chooses the adopted
    // group; presentation neither owns GPU resources nor publishes that group.
    std::shared_ptr<const std::vector<material::ProgramSurfaceRef>> surfaces{};
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
    std::string source;

    geometry::MeshRef resolved;

    std::vector<MaterialBinding> defaults{};
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
    std::string source;

    rendering::SkyMapRef resolved;
};

[[nodiscard]] Result<void> set_sky_map_binding(Runtime&, ObjectId environment_component, SkyMapBinding);

[[nodiscard]] Result<SkyMapBinding> read_sky_map(Runtime&, ObjectId environment_component);

using geometry::material_slot_name;

[[nodiscard]] Result<std::vector<MaterialBinding>> read_material_bindings(Runtime&, ObjectId);

[[nodiscard]] Result<void> set_material_bindings(Runtime&, ObjectId, std::span<const MaterialBinding>);

// Shares unchanged captures; normally unbound slots use the engine world-grid MS.
[[nodiscard]] Result<std::shared_ptr<const std::vector<material::ProgramSurfaceRef>>> material_surfaces(Runtime&,
                                                                                                        ObjectId);

namespace detail {
// Values that passed validation, by bucket position. Validators are pure checks
// of a value's bytes and published types never change, so the same bytes pass
// again; any other value at that position is validated again.
template <class T>
class PassedValues {
    static_assert(std::is_trivially_copyable_v<T>);

    std::vector<std::optional<T>> values_;

public:
    void resize(std::size_t size) { values_.resize(size); }

    [[nodiscard]] bool contains(std::size_t at, const T& value) const noexcept {
        return values_[at] && std::memcmp(&*values_[at], &value, sizeof(T)) == 0;
    }

    void insert(std::size_t at, const T& value) noexcept { std::memcpy(&values_[at].emplace(), &value, sizeof(T)); }
};

class Presented;
}

// One position of the mesh bucket in the cached extraction.
struct MeshSlot {
    std::optional<RenderItem> item; // Absent while isolated or degenerate.

    // Where `item` is drawn at the latest update when a presented pose moves it off item->world.
    std::optional<math::Matrix4> presented;

    std::uint64_t changed{}; // Scene::update serial that last changed `item` or `presented`.
};

// The presentation of one World; a World without a Scene created by its host is
// not presented. Creating it registers the mesh (a mesh needs
// geometry), light and Environment capabilities and the one-Environment
// admission on that World; extraction reads only those buckets and reports
// object failures to the World's log under its failure policy. One per World,
// created at a host boundary and destroyed before the World.
class Scene final {
public:
    [[nodiscard]] static Result<Scene> create(world::World&, const Types&);

    [[nodiscard]] world::World& world() const noexcept { return *world_; }

    [[nodiscard]] const Types& types() const noexcept { return types_; }

    [[nodiscard]] world::CapabilityId meshes() const noexcept { return meshes_; }

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
    [[nodiscard]] Result<std::uint64_t> update(std::span<const world::PresentedPose> presented = {}) const;

    // The cache as of the latest update, by mesh bucket position.
    [[nodiscard]] std::span<const MeshSlot> mesh_slots() const noexcept { return mesh_slots_; }

    // Serial of the latest update that rebuilt every slot after the mesh bucket
    // changed (World::revision advanced); slot positions keep their component until then.
    [[nodiscard]] std::uint64_t mesh_layout() const noexcept { return mesh_layout_; }

    // Lights as drawn at the latest update, presented poses applied.
    [[nodiscard]] std::span<const LightRenderItem> light_snapshot() const noexcept { return lights_cache_; }

    [[nodiscard]] std::uint64_t lights_changed() const noexcept { return lights_changed_; }

    // update(), then the drawn items in Entity/composition order.
    [[nodiscard]] Result<std::vector<RenderItem>> render_items() const;

    // Enabled members, in Entity/composition order. Stopped Worlds also present.
    // Extracted afresh on every call.
    [[nodiscard]] Result<std::vector<LightRenderItem>> light_items() const;

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

    // Replace only after a successful capture/storage, even when the Surface owner is unchanged.
    void index_materials(std::size_t slot, std::unordered_set<ObjectId> dependencies) const;

    // Rebuilds the spatial chains of the mesh slots and the light sources for a new layout.
    Result<void> index_layout(world::World::SpatialPass&, std::span<const world::CapabilityMember> meshes) const;

    // `failed`: an active light was not drawn. `moved`: a presented pose moved a light.
    Result<std::vector<LightRenderItem>> extract_lights(const detail::Presented*, bool* failed = nullptr,
                                                        bool* moved = nullptr) const;

    world::World* world_;

    Types types_;

    world::CapabilityId meshes_;

    world::CapabilityId lights_;

    world::CapabilityId environments_;

    world::CapabilityId cameras_;

    // Aligned with the meshes and lights buckets at each extraction call.
    mutable detail::PassedValues<LightSettings> valid_lights_;

    mutable std::vector<MeshSlot> mesh_slots_;

    mutable std::vector<bool> materials_dirty_; // Component/binding or resource changed, not just space.

    mutable std::vector<std::unordered_set<ObjectId>> material_dependencies_;

    mutable std::unordered_map<ObjectId, std::unordered_set<std::uint32_t>> material_dependents_;

    mutable std::vector<LightRenderItem> lights_cache_;

    mutable rendering::SceneLightingSettings environment_cache_;

    // Writes stamped after `cursor_` are not yet read; valid while `tracked_`.
    mutable std::uint64_t cursor_{};

    mutable bool tracked_{};

    mutable std::vector<ObjectId> written_;

    // Each Entity on a mesh's spatial chain, and its component, lead to that slot.
    mutable std::unordered_map<ObjectId, std::vector<std::uint32_t>> mesh_dependents_;

    mutable std::unordered_map<ObjectId, std::uint32_t> mesh_components_;

    // Slot s's chain, its own Entity first, is chains_[chain_starts_[s], chain_starts_[s + 1]).
    mutable std::vector<ObjectId> chains_;

    mutable std::vector<std::uint32_t> chain_starts_;

    // Light components and the Entities on their chains.
    mutable std::unordered_set<ObjectId> light_sources_;

    mutable std::vector<std::uint32_t> dirty_;

    mutable std::vector<std::uint32_t> volatile_;

    mutable std::vector<std::uint32_t> presented_list_;

    mutable std::vector<bool> volatile_flags_;

    mutable std::vector<std::uint64_t> marks_;

    mutable std::vector<std::uint64_t> presented_marks_; // Serial of the update that last listed each slot.

    mutable bool lights_dirty_{true};

    mutable bool lights_failed_{};

    mutable bool lights_moved_{};

    std::uint64_t identity_{};

    mutable std::uint64_t serial_{};

    mutable std::uint64_t version_{};

    mutable std::uint64_t mesh_layout_{};

    mutable std::uint64_t lights_changed_{};

    mutable std::uint64_t layout_revision_{};
};

}
