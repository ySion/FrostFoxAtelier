#pragma once
#include "foundation/string.hpp"
#include "math/math.hpp"
#include "runtime/reflection.hpp"
#include <cstdint>
#include "editor/operator_parameters.gen.hpp"

// Operator parameter records. Only the editor's document Session publishes them;
// see docs/modules/editor/operators.md.
namespace atelier::editor {
AENUM(Name = "editor.TransformTool",
      DESC = "视口手柄：Select 只选择，Transform 合并移动、旋转、缩放手柄，其余各显示一种。")
enum class TransformTool : std::uint32_t { select, transform, move, rotate, scale };

AENUM(Name = "editor.PivotMode", DESC = "变换中心：Median 为所选对象的中心，Cursor 为 3D 光标。")
enum class PivotMode : std::uint32_t { median, cursor };

AENUM(Name = "editor.SelectMode", DESC = "点选或框选与现有选择的组合方式。")
enum class SelectMode : std::uint32_t { set, extend, subtract, invert, intersect };

AENUM(Name = "editor.SelectAll", DESC = "选择全部、清空、反选，或在全选与清空之间切换。")
enum class SelectAll : std::uint32_t { select, deselect, toggle, invert };

AENUM(Name = "editor.TransformChannel", DESC = "要清除的局部变换通道。")
enum class TransformChannel : std::uint32_t { location, rotation, scale };

AENUM(Name = "editor.SnapTarget", DESC = "吸附到网格，或把整体中心移动到 3D 光标。")
enum class SnapTarget : std::uint32_t { grid, cursor };

AENUM(Name = "editor.DeleteTarget", DESC = "删除被检视的对象，或显式选择的 Entity 子树。")
enum class DeleteTarget : std::uint32_t { object, entities };

AENUM(Name = "editor.Orientation", DESC = "移动、旋转手柄和 X/Y/Z 约束使用的坐标轴；缩放总是使用 Local。")
enum class Orientation : std::uint32_t { global, local };

AENUM(Name = "editor.CameraView", DESC = "视口相机：Perspective 透视，或沿某个坐标轴的正交视图。")
enum class CameraView : std::uint32_t { perspective, top, front, right, bottom, back, left };

AENUM(Name = "editor.AddKind",
      DESC = "新建对象的种类：空 Entity、静态网格、程序化生成器、点光、矩形面光、平行光或相机。")
enum class AddKind : std::uint32_t { entity, mesh, procedural_generator, point, rectangle, directional, camera };

AENUM(Name = "editor.RegionKind", DESC = "编辑器工作区区域。")
enum class RegionKind : std::uint32_t {
    none,
    viewport,
    scene_tree,
    structure,
    parameters,
    assets,
    code,
    output,
    world,
    controls,
    network,
    spreadsheet
};

ASTRUCT(Name = "editor.LayoutShow", DESC = "显示并聚焦一个工作区页签。")
struct LayoutShow {
    AGENERATED_BODY(LayoutShow)

    AFIELD(DESC = "目标页签；none 不是可显示的页签。")
    RegionKind tab{RegionKind::viewport};
};

ASTRUCT(Name = "editor.LayoutCycle", DESC = "在指定区域所在窗格中切换到相邻页签。")
struct LayoutCycle {
    AGENERATED_BODY(LayoutCycle)

    AFIELD(HardMin = "-1", HardMax = "1", DESC = "-1 为前一个，1 为后一个；0 不移动。")
    std::int32_t direction{1};
};

AENUM(Name = "editor.ToolSetting", DESC = "可切换的视口工具开关。")
enum class ToolSetting : std::uint32_t { grid_snap, cursor_surface, snap_move, snap_rotate, snap_scale };

ASTRUCT(Name = "editor.ToolToggle", DESC = "反转一个工具开关；非幂等，自动化应使用 tool.settings 写绝对值。")
struct ToolToggle {
    AGENERATED_BODY(ToolToggle)

    AFIELD(DESC = "要反转的开关。")
    ToolSetting setting{ToolSetting::grid_snap};
};

ASTRUCT(Name = "editor.ToolSettings", DESC = "视口变换工具、选择方式与吸附设置。")
struct ToolSettings {
    AGENERATED_BODY(ToolSettings)

    AFIELD(DESC = "视口显示的手柄。")
    TransformTool tool{TransformTool::transform};

    AFIELD(DESC = "变换中心。")
    PivotMode pivot{PivotMode::median};

    AFIELD(DESC = "移动、旋转、缩放是否按吸附步长取整；由下面三个 Snap 开关决定作用于哪些操作。")
    bool grid_snap{};

    AFIELD(DESC = "点击视口时，3D 光标是否落到被点中的表面上并对齐其法线。")
    bool cursor_surface{true};

    AFIELD(Min = "0.1", Max = "1", HardMin = "0.0001", HardMax = "10000", DESC = "移动吸附和参考网格的步长，单位 m。")
    float grid_spacing{.2f};

    AFIELD(DESC = "点选或框选与现有选择的组合方式。")
    SelectMode select_mode{SelectMode::set};

    AFIELD(DESC = "移动、旋转手柄与轴约束的坐标轴。")
    Orientation orientation{Orientation::global};

    AFIELD(DESC = "吸附是否作用于移动。")
    bool snap_move{true};

    AFIELD(DESC = "吸附是否作用于旋转。")
    bool snap_rotate{true};

    AFIELD(DESC = "吸附是否作用于缩放。")
    bool snap_scale{};

    AFIELD(Min = "1", Max = "45", HardMin = "0.01", HardMax = "360", DESC = "旋转吸附步长，单位度。")
    float rotation_step{15};

    AFIELD(Min = "0.01", Max = "1", HardMin = "0.0001", HardMax = "100", DESC = "缩放吸附步长，无单位的比例增量。")
    float scale_step{.1f};

    bool operator==(const ToolSettings&) const = default;
};

ASTRUCT(Name = "editor.SceneAdd", DESC = "在 3D 光标处新建一个 Entity，并选中它。")
struct SceneAdd {
    AGENERATED_BODY(SceneAdd)

    AFIELD(DESC = "新建的种类。")
    AddKind kind{AddKind::entity};

    AFIELD(DESC = "网格资源键，只用于 mesh；为空时使用第一个内置网格。")
    String mesh{};

    AFIELD(DESC = "程序化几何图资产键，只用于 procedural_generator；可留空。")
    String graph{};
};

ASTRUCT(Name = "editor.SceneParent", DESC = "按选择顺序改父级；实际提交的移动合为一次历史。")
struct SceneParent {
    AGENERATED_BODY(SceneParent)

    AFIELD(Weak = "world.Entity", DESC = "目标父 Entity；null 表示 World 根。")
    ReferenceSlot parent;

    AFIELD(DESC = "保留世界空间姿态；无法用局部 TRS 表达时拒绝该项。")
    bool keep_world{true};

    AFIELD(HardMin = "-1", DESC = "第一个目标位置；-1 使用默认追加位置，同父级时不重排。")
    std::int32_t index{-1};
};

ASTRUCT(Name = "editor.SceneSpaceReference", DESC = "改变活动 Entity 的空间参考，不改变组合父级。")
struct SceneSpaceReference {
    AGENERATED_BODY(SceneSpaceReference)

    AFIELD(Weak = "world.Entity", DESC = "指定的空间 Entity；null 时默认继承组合祖先。")
    ReferenceSlot origin;

    AFIELD(DESC = "显式使用 World 空间；只能与 null origin 一起使用。")
    bool world_space{};

    AFIELD(DESC = "保留世界姿态；false 保留当前局部变换。")
    bool keep_world{true};
};

ASTRUCT(Name = "editor.SceneSelectAll", DESC = "按 Context 的选择计算新的文档选择，不写场景历史。")
struct SceneSelectAll {
    AGENERATED_BODY(SceneSelectAll)

    AFIELD(DESC = "select 全选、deselect 清空、invert 反选；toggle 保留视口 A 的全选/清空行为。")
    SelectAll action{SelectAll::select};
};

ASTRUCT(Name = "editor.SceneDelete", DESC = "删除 Context 对象或 Entity 子树；保留无关的文档选择。")
struct SceneDelete {
    AGENERATED_BODY(SceneDelete)

    AFIELD(DESC = "object 删除被检视的组件，否则删除所选 Entity；entities 始终删除所选 Entity 子树。")
    DeleteTarget target{DeleteTarget::object};
};

ASTRUCT(Name = "editor.SceneDuplicate", DESC = "复制 Context 的 Entity 子树，选择新对象并合为一次历史。")
struct SceneDuplicate {
    AGENERATED_BODY(SceneDuplicate)

    AFIELD(DESC = "false 直接复制；true 仅由人类调用启动视口放置，取消放置保留原位副本。")
    bool place{};
};

ASTRUCT(Name = "editor.SceneAddComponent", DESC = "向 Context 的活动 Entity 添加一个组件，选择新组件。")
struct SceneAddComponent {
    AGENERATED_BODY(SceneAddComponent)

    AFIELD(DESC = "已登记的完整组件类型名。")
    String type{};
};

ASTRUCT(Name = "editor.SceneRename", DESC = "修改活动 Entity 的名称；交互调用缺少名称时开始内联编辑。")
struct SceneRename {
    AGENERATED_BODY(SceneRename)

    AFIELD(DESC = "完整新名称；非交互调用必须提供非空名称。")
    String name{};
};

ASTRUCT(Name = "editor.SceneTransform", DESC = "写入活动 Entity 的完整局部变换；必要时创建空间能力。")
struct SceneTransform {
    AGENERATED_BODY(SceneTransform)

    AFIELD(DESC = "局部位置，单位 m。")
    math::Vector3 position;

    AFIELD(DESC = "局部旋转四元数；必须能归一化。")
    math::Quaternion rotation;

    AFIELD(DESC = "局部缩放；负值镜像，零值保留。")
    math::Vector3 scale{1, 1, 1};
};

ASTRUCT(Name = "editor.SceneTransformClear", DESC = "清除所选空间根的一种局部变换通道，合为一次历史。")
struct SceneTransformClear {
    AGENERATED_BODY(SceneTransformClear)

    AFIELD(DESC = "位置归零、旋转归零或缩放归一。")
    TransformChannel channel{TransformChannel::location};
};

ASTRUCT(Name = "editor.SceneSnap", DESC = "吸附显式选择，保留空间层级并合为一次历史。")
struct SceneSnap {
    AGENERATED_BODY(SceneSnap)

    AFIELD(DESC = "grid 逐个根吸附；cursor 整组移动并保留相对偏移。")
    SnapTarget to{SnapTarget::grid};
};

ASTRUCT(Name = "editor.ViewAxis", DESC = "把视口相机切换到透视或某个轴向的正交视图，保持观察目标。")
struct ViewAxis {
    AGENERATED_BODY(ViewAxis)

    AFIELD(DESC = "目标视图。")
    CameraView view{CameraView::perspective};
};

ASTRUCT(Name = "editor.CameraState", DESC = "完整视口相机状态，角度使用弧度，视野使用度。")
struct CameraState {
    AGENERATED_BODY(CameraState)

    AFIELD()
    CameraView view{CameraView::perspective};

    AFIELD(HardMin = "-100000000", HardMax = "100000000")
    math::Vector3 target{};

    AFIELD()
    float yaw{};

    AFIELD()
    float pitch{};

    AFIELD()
    float distance{};

    AFIELD()
    float roll{};

    AFIELD()
    float vertical_fov{45};

    AFIELD()
    float near_clip{.05f};

    AFIELD()
    float far_clip{30000};

    bool operator==(const CameraState&) const = default;
};

ASTRUCT(Name = "editor.ViewCameraSet", DESC = "恢复完整视口相机状态。")
struct ViewCameraSet {
    AGENERATED_BODY(ViewCameraSet)

    AFIELD()
    CameraState state;
};

ASTRUCT(Name = "editor.ViewLookAt", DESC = "从 eye 看向 target，单位 m，+Y 向上。")
struct ViewLookAt {
    AGENERATED_BODY(ViewLookAt)

    AFIELD(HardMin = "-100000000", HardMax = "100000000")
    math::Vector3 eye;

    AFIELD(HardMin = "-100000000", HardMax = "100000000")
    math::Vector3 target;
};

ASTRUCT(Name = "editor.OpenPath", DESC = "打开或创建工程、场景及源码的文件路径。")
struct OpenPath {
    AGENERATED_BODY(OpenPath)

    AFIELD(DESC = "非交互调用必须提供路径；交互调用留空时打开原生选择器。")
    String path;
};

ASTRUCT(Name = "editor.SceneSave", DESC = "保存当前场景，或交互选择另存路径。")
struct SceneSave {
    AGENERATED_BODY(SceneSave)

    AFIELD()
    String path;

    AFIELD()
    bool as_new{};
};

AENUM(Name = "editor.ImportKind", DESC = "导入资源的类型。")
enum class ImportKind : std::uint32_t { mesh, texture };

ASTRUCT(Name = "editor.AssetImport", DESC = "导入网格或纹理，返回新资产键。")
struct AssetImport {
    AGENERATED_BODY(AssetImport)

    AFIELD()
    ImportKind kind{ImportKind::mesh};

    AFIELD()
    String path;
};

AENUM(Name = "editor.ViewToggleOption", DESC = "视口显示开关。")
enum class ViewToggleOption : std::uint32_t { options, fps };

ASTRUCT(Name = "editor.ViewToggle", DESC = "反转视口显示开关。")
struct ViewToggle {
    AGENERATED_BODY(ViewToggle)

    AFIELD()
    ViewToggleOption option{ViewToggleOption::options};
};

AENUM(Name = "editor.MenuKind", DESC = "视口菜单。")
enum class MenuKind : std::uint32_t { tools, delete_selection, shading, add, axis };

ASTRUCT(Name = "editor.Menu", DESC = "在视口指针处打开菜单。")
struct MenuParameters {
    AGENERATED_BODY(MenuParameters)

    AFIELD()
    MenuKind menu{MenuKind::tools};
};

AENUM(Name = "editor.LightingFitMode", DESC = "按场景拟合，或按场景拟合并跟随焦点。")
enum class LightingFitMode : std::uint32_t { scene, scene_focus };

ASTRUCT(Name = "editor.LightingFit", DESC = "拟合场景探针范围。")
struct LightingFit {
    AGENERATED_BODY(LightingFit)

    AFIELD()
    LightingFitMode mode{LightingFitMode::scene};
};

ASTRUCT(Name = "editor.ProjectBuild", DESC = "构建工程；显式重建类型保留作者值但清空场景历史。")
struct ProjectBuildParameters {
    AGENERATED_BODY(ProjectBuildParameters)

    AFIELD(DESC = "true 明确重建类型与场景；false 尝试兼容更新。")
    bool rebuild{};
};

// Only for assets' authoritative material entries JSON; not a general opaque parameter channel.
ASTRUCT(Name = "editor.MaterialEntriesJson")
struct MaterialEntriesJson {
    AGENERATED_BODY(MaterialEntriesJson)

    AFIELD()
    String json = String::literal<"[]">();
};

ASTRUCT(Name = "editor.NewMaterial", DESC = "创建具名材质；实例沿用 Context 中活动材质作为父级。")
struct NewMaterial {
    AGENERATED_BODY(NewMaterial)

    AFIELD(DESC = "工程 Content 下的 .material 路径；非交互调用必须提供。")
    String path;

    AFIELD(DESC = "完整初始 typed entries；格式见 docs/modules/editor/control.md。")
    MaterialEntriesJson entries;
};

ASTRUCT(Name = "editor.NewMaterialSource")
struct NewMaterialSource {
    AGENERATED_BODY(NewMaterialSource)

    AFIELD()
    String path;

    AFIELD()
    String source;
};

ASTRUCT(Name = "editor.AssetResult", DESC = "资产操作发布的持久资源键。")
struct AssetResult {
    AGENERATED_BODY(AssetResult)

    AFIELD(DESC = "新建或导入的资产键。")
    String asset;
};

ASTRUCT(Name = "editor.NewProceduralGraph", DESC = "创建程序化几何图资产（.procgraph）。")
struct NewProceduralGraph {
    AGENERATED_BODY(NewProceduralGraph)

    AFIELD(DESC = "工程 Content 下的 .procgraph 路径；交互调用留空时打开原生选择器。")
    String path;

    AFIELD(DESC = "可选的初始 v3 JSON 文本；留空时生成标准空白图。")
    String source;
};

ASTRUCT(Name = "editor.GraphOpen", DESC = "打开程序化图资产并进入 SOP 视口上下文。")
struct GraphOpen {
    AGENERATED_BODY(GraphOpen)

    AFIELD(DESC = "工程 .procgraph 资产键，或已存在的 scratch 图键。")
    String asset;
};

ASTRUCT(Name = "editor.GraphDiveIn", DESC = "下钻进入指定 AssetNode 所引用的 .procgraph 数字资产并压入面包屑栈。")
struct GraphDiveIn {
    AGENERATED_BODY(GraphDiveIn)

    AFIELD(DESC = "父图资产键；留空时取当前聚焦或活动网络图。")
    String asset;

    AFIELD(DESC = "父图中的 AssetNode 节点名；留空时取当前选中的 AssetNode。")
    String node;
};

ASTRUCT(Name = "editor.GraphSave", DESC = "保存程序化图资产。")
struct GraphSave {
    AGENERATED_BODY(GraphSave)

    AFIELD(DESC = "目标 .procgraph 资产键；留空时保存当前活动图。")
    String asset;
};

ASTRUCT(Name = "editor.GraphAddNode", DESC = "向活动或指定的程序化图添加一个节点并选中它。")
struct GraphAddNode {
    AGENERATED_BODY(GraphAddNode)

    AFIELD(DESC = "目标图键；留空时作用于当前活动图。")
    String asset;

    AFIELD(DESC = "节点完整类型名或短名（例如 procedural.Grid 或 Grid）。")
    String type;

    AFIELD(DESC = "可选节点名；留空时按类型自动生成唯一标识符。")
    String name;

    AFIELD(DESC = "节点在网络画布上的二维坐标。")
    math::Vector2 position{};
};

ASTRUCT(Name = "editor.GraphNodeResult", DESC = "新建程序化图节点的结果。")
struct GraphNodeResult {
    AGENERATED_BODY(GraphNodeResult)

    AFIELD(DESC = "新建节点的唯一名称。")
    String name;
};

ASTRUCT(Name = "editor.GraphDeleteNode", DESC = "从程序化图中删除一个节点。")
struct GraphDeleteNode {
    AGENERATED_BODY(GraphDeleteNode)

    AFIELD(DESC = "目标图键；留空时作用于当前活动图。")
    String asset;

    AFIELD(DESC = "要删除的节点名；留空时删除当前选中的图节点。")
    String name;
};

ASTRUCT(Name = "editor.GraphDuplicateNodes",
        DESC = "复制程序化图中的选中或指定节点，保留属性、外观与连线，合为一步图历史。")
struct GraphDuplicateNodes {
    AGENERATED_BODY(GraphDuplicateNodes)

    AFIELD(DESC = "目标图键；留空时作用于当前活动图。")
    String asset;

    AFIELD(DESC = "可选单个目标节点名；留空时复制当前选中的节点集合。")
    String node;

    AFIELD(DESC = "副本相对原节点的画布坐标偏移。")
    math::Vector2 offset{36.f, 36.f};
};

ASTRUCT(Name = "editor.GraphRenameNode", DESC = "重命名程序化图中的节点。")
struct GraphRenameNode {
    AGENERATED_BODY(GraphRenameNode)

    AFIELD(DESC = "目标图键；留空时作用于当前活动图。")
    String asset;

    AFIELD(DESC = "原节点名；留空时重命名当前选中的图节点。")
    String node;

    AFIELD(DESC = "新的唯一节点标识符。")
    String name;
};

ASTRUCT(Name = "editor.GraphMoveNode", DESC = "更新程序化图节点在画布上的位置。")
struct GraphMoveNode {
    AGENERATED_BODY(GraphMoveNode)

    AFIELD(DESC = "目标图键；留空时作用于当前活动图。")
    String asset;

    AFIELD(DESC = "节点名。")
    String node;

    AFIELD(DESC = "新的二维坐标。")
    math::Vector2 position{};
};

ASTRUCT(Name = "editor.GraphConnect", DESC = "连接两个程序化图节点的端口。")
struct GraphConnect {
    AGENERATED_BODY(GraphConnect)

    AFIELD(DESC = "目标图键；留空时作用于当前活动图。")
    String asset;

    AFIELD(DESC = "上游节点名。")
    String from_node;

    AFIELD(DESC = "上游输出端口名；留空时默认取第 0 个输出端口。")
    String from_port;

    AFIELD(DESC = "下游节点名。")
    String to_node;

    AFIELD(DESC = "下游输入端口名；留空时默认取第 0 个输入端口。")
    String to_port;
};

ASTRUCT(Name = "editor.GraphDisconnect", DESC = "断开程序化图节点的输入连接。")
struct GraphDisconnect {
    AGENERATED_BODY(GraphDisconnect)

    AFIELD(DESC = "目标图键；留空时作用于当前活动图。")
    String asset;

    AFIELD(DESC = "下游节点名。")
    String to_node;

    AFIELD(DESC = "下游输入端口名；留空时匹配所有输入端口。")
    String to_port;

    AFIELD(DESC = "可选上游节点名；留空时断开目标端口上的全部连接。")
    String from_node;

    AFIELD(DESC = "可选上游输出端口名。")
    String from_port;
};

ASTRUCT(Name = "editor.GraphSetDisplay", DESC = "设置或清除程序化图的蓝色 Display 标记节点。")
struct GraphSetDisplay {
    AGENERATED_BODY(GraphSetDisplay)

    AFIELD(DESC = "目标图键；留空时作用于当前活动图。")
    String asset;

    AFIELD(DESC = "要显示输出的节点名；留空时清除 Display 标记。")
    String node;
};

ASTRUCT(Name = "editor.GraphSetTemplate", DESC = "设置或清除程序化图的紫红色 Template 参考线框标记节点。")
struct GraphSetTemplate {
    AGENERATED_BODY(GraphSetTemplate)

    AFIELD(DESC = "目标图键；留空时作用于当前活动图。")
    String asset;

    AFIELD(DESC = "要作为参考线框叠加的节点名；留空时清除 Template 标记。")
    String node;
};

ASTRUCT(Name = "editor.GraphSetBypass", DESC = "设置程序化图节点的黄色 Bypass 直通跳过状态。")
struct GraphSetBypass {
    AGENERATED_BODY(GraphSetBypass)

    AFIELD(DESC = "目标图键；留空时作用于当前活动图。")
    String asset;

    AFIELD(DESC = "目标节点名；留空时作用于当前选中的节点。")
    String node;

    AFIELD(DESC = "为 true 时跳过该节点求值并直通第 0 输入端口。")
    bool bypass{};
};

ASTRUCT(Name = "editor.GraphSetFrozen", DESC = "设置程序化图节点的冰蓝色 Freeze 几何结果冻结状态。")
struct GraphSetFrozen {
    AGENERATED_BODY(GraphSetFrozen)

    AFIELD(DESC = "目标图键；留空时作用于当前活动图。")
    String asset;

    AFIELD(DESC = "目标节点名；留空时作用于当前选中的节点。")
    String node;

    AFIELD(DESC = "为 true 时锁定该节点当前已求值的几何输出快照，直至显式解冻。")
    bool frozen{};
};

ASTRUCT(Name = "editor.GraphSetAppearance", DESC = "更新程序化图节点的形状、颜色或备注。")
struct GraphSetAppearance {
    AGENERATED_BODY(GraphSetAppearance)

    AFIELD(DESC = "目标图键；留空时作用于当前活动图。")
    String asset;

    AFIELD(DESC = "目标节点名；留空时作用于当前选中的节点集合。")
    String node;

    AFIELD(DESC = "可选形状预设名（auto/pill/rect/domed/trapezoid/hexagon/ribbon/diamond/circle）；留空时不修改。")
    String shape;

    AFIELD(DESC = "可选颜色十六进制字符串（#RRGGBB，或 default 恢复默认）；留空时不修改。")
    String color;

    AFIELD(DESC = "可选节点备注；设为单字符 '-' 时清空备注，留空时不修改。")
    String comment;
};

ASTRUCT(Name = "editor.GraphSelect", DESC = "选择程序化图中的节点用于参数与几何表格检视（不改变 Display 标记）。")
struct GraphSelect {
    AGENERATED_BODY(GraphSelect)

    AFIELD(DESC = "目标图键；留空时作用于当前活动图。")
    String asset;

    AFIELD(DESC = "要选中的节点名；留空时检视图根设置（Graph.seed）。")
    String node;
};

ASTRUCT(Name = "editor.GraphLayout", DESC = "按自上而下拓扑层级自动排列程序化图节点，合为一步图历史。")
struct GraphLayout {
    AGENERATED_BODY(GraphLayout)

    AFIELD(DESC = "目标图键；留空时作用于当前活动图。")
    String asset;
};

ASTRUCT(Name = "editor.GraphExposeParameter", DESC = "将程序化图内部节点字段提升为图级公开参数。")
struct GraphExposeParameter {
    AGENERATED_BODY(GraphExposeParameter)

    AFIELD(DESC = "目标图键；留空时作用于当前活动图。")
    String asset;

    AFIELD(DESC = "目标节点名；留空时作用于当前选中节点。")
    String node;

    AFIELD(DESC = "节点上的字段路径（例如 width 或 size.x）。")
    String path;

    AFIELD(DESC = "可选图级参数标识符；留空时按字段名自动生成唯一名称。")
    String name;

    AFIELD(DESC = "可选 UI 显示标题。")
    String label;

    AFIELD(DESC = "可选分组名称。")
    String category;

    AFIELD(DESC = "可选提示说明。")
    String description;
};

ASTRUCT(Name = "editor.GraphBindParameter", DESC = "将节点字段追加绑定到已有图级公开参数（多目标驱动）。")
struct GraphBindParameter {
    AGENERATED_BODY(GraphBindParameter)

    AFIELD(DESC = "目标图键；留空时作用于当前活动图。")
    String asset;

    AFIELD(DESC = "已有图级公开参数名称。")
    String parameter;

    AFIELD(DESC = "目标节点名；留空时作用于当前选中节点。")
    String node;

    AFIELD(DESC = "节点上的字段路径。")
    String path;
};

ASTRUCT(Name = "editor.GraphUnbindParameter", DESC = "解除节点字段与图级公开参数的绑定。")
struct GraphUnbindParameter {
    AGENERATED_BODY(GraphUnbindParameter)

    AFIELD(DESC = "目标图键；留空时作用于当前活动图。")
    String asset;

    AFIELD(DESC = "目标节点名；留空时作用于当前选中节点。")
    String node;

    AFIELD(DESC = "节点上的字段路径。")
    String path;
};

ASTRUCT(Name = "editor.GraphUpdateParameter", DESC = "修改图级公开参数的名称、元数据或排列顺序。")
struct GraphUpdateParameter {
    AGENERATED_BODY(GraphUpdateParameter)

    AFIELD(DESC = "目标图键；留空时作用于当前活动图。")
    String asset;

    AFIELD(DESC = "图级公开参数当前名称。")
    String parameter;

    AFIELD(DESC = "可选新标识符；留空时不重命名。")
    String name;

    AFIELD(DESC = "可选新标题；设为 '-' 时清空。")
    String label;

    AFIELD(DESC = "可选新分组；设为 '-' 时清空。")
    String category;

    AFIELD(DESC = "可选新说明；设为 '-' 时清空。")
    String description;

    AFIELD(DESC = "顺序移动偏移（-1 上移，+1 下移，0 不变）。")
    std::int32_t move_delta{};
};

ASTRUCT(Name = "editor.GraphRemoveParameter", DESC = "删除图级公开参数并释放其控制的全部内部节点字段。")
struct GraphRemoveParameter {
    AGENERATED_BODY(GraphRemoveParameter)

    AFIELD(DESC = "目标图键；留空时作用于当前活动图。")
    String asset;

    AFIELD(DESC = "要移除的图级公开参数名称。")
    String parameter;
};

ASTRUCT(Name = "editor.GraphSetParameter", DESC = "更新程序化图根级公开参数的值并同步写入其绑定的全部内部目标。")
struct GraphSetParameter {
    AGENERATED_BODY(GraphSetParameter)

    AFIELD(DESC = "目标图键；留空时作用于当前活动图。")
    String asset;

    AFIELD(DESC = "图级公开参数名称。")
    String parameter;

    AFIELD(DESC = "JSON 编码的参数值（数值、布尔、枚举名、字符串或向量数组）。")
    String value_json;
};

ASTRUCT(Name = "editor.GraphSetOverride", DESC = "设置或清除程序化图中 AssetNode 实例上的公开参数覆写。")
struct GraphSetOverride {
    AGENERATED_BODY(GraphSetOverride)

    AFIELD(DESC = "外层目标图键；留空时作用于当前活动图。")
    String asset;

    AFIELD(DESC = "目标 AssetNode 节点名；留空时作用于当前选中节点。")
    String node;

    AFIELD(DESC = "被引用的数字资产公开参数名。")
    String parameter;

    AFIELD(DESC = "为 true 时移除覆写并恢复资产默认值。")
    bool clear{};

    AFIELD(DESC = "clear 为 false 时应用的 JSON 编码覆写值。")
    String value_json;
};

ASTRUCT(Name = "editor.GraphSetNodeAsset", DESC = "绑定或切换程序化图中 AssetNode 实例引用的 .procgraph 数字资产。")
struct GraphSetNodeAsset {
    AGENERATED_BODY(GraphSetNodeAsset)

    AFIELD(DESC = "外层目标图键；留空时作用于当前活动图。")
    String asset;

    AFIELD(DESC = "目标 AssetNode 节点名；留空时作用于当前选中节点。")
    String node;

    AFIELD(DESC = "引用的目标 .procgraph 资产键；留空时清除绑定。")
    String target_asset;
};

ASTRUCT(Name = "editor.GraphExtractAsset",
        DESC = "将选中或指定的连通凸子图提取为独立 .procgraph 数字资产，并在原位置替换为连线对齐的 AssetNode。")
struct GraphExtractAsset {
    AGENERATED_BODY(GraphExtractAsset)

    AFIELD(DESC = "外层目标图键；留空时作用于当前活动图。")
    String asset;

    AFIELD(DESC = "可选逗号分隔的节点名列表；留空时提取当前选中的节点集合。")
    String nodes;

    AFIELD(DESC = "新 .procgraph 数字资产的保存路径；交互调用留空时打开原生保存选择器。")
    String path;

    AFIELD(DESC = "替换生成的 AssetNode 可选期望名称；留空时按资产文件名派生。")
    String name;
};

ASTRUCT(Name = "editor.GraphExtractAssetResult", DESC = "提取数字资产后的资产键与替换生成的 AssetNode 节点名。")
struct GraphExtractAssetResult {
    AGENERATED_BODY(GraphExtractAssetResult)

    AFIELD(DESC = "新创建的 .procgraph 资产键。")
    String asset;

    AFIELD(DESC = "外层图中替换生成的 AssetNode 节点名。")
    String node;
};

ASTRUCT(Name = "editor.ProceduralSetOverride",
        DESC = "设置或清除场景 ProceduralGeneratorComponent 实例上的公开参数差量覆写。")
struct ProceduralSetOverride {
    AGENERATED_BODY(ProceduralSetOverride)

    AFIELD(Weak = "world.Entity", DESC = "目标生成器所属 Entity；与 component 同为 null 时使用当前选择。")
    ReferenceSlot entity;

    AFIELD(Weak = "world.ProceduralGeneratorComponent", DESC = "目标生成器组件；与 entity 同为 null 时使用当前选择。")
    ReferenceSlot component;

    AFIELD(DESC = "被引用的 .procgraph 公开参数名称。")
    String parameter;

    AFIELD(DESC = "为 true 时移除覆写并恢复资产默认值。")
    bool clear{};

    AFIELD(DESC = "clear 为 false 时应用的 JSON 编码覆写值。")
    String value_json;
};

ASTRUCT(Name = "editor.AssignProceduralGraph",
        DESC = "绑定或清除 ProceduralGeneratorComponent 引用的 .procgraph 资产。")
struct AssignProceduralGraph {
    AGENERATED_BODY(AssignProceduralGraph)

    AFIELD(Weak = "world.ProceduralGeneratorComponent",
           DESC = "目标生成器组件；null 时使用当前 Context 中的生成器组件。")
    ReferenceSlot component;

    AFIELD(DESC = "目标 .procgraph 资产键；留空时清除绑定。")
    String asset;
};

ASTRUCT(Name = "editor.ProceduralBake", DESC = "将程序化生成器的求值实例可撤销地烘焙为 StaticMeshEntity 层级。")
struct ProceduralBake {
    AGENERATED_BODY(ProceduralBake)

    AFIELD(Weak = "world.Entity", DESC = "目标生成器所属 Entity；与 component 同为 null 时使用当前选择。")
    ReferenceSlot entity;

    AFIELD(Weak = "world.ProceduralGeneratorComponent", DESC = "目标生成器组件；与 entity 同为 null 时使用当前选择。")
    ReferenceSlot component;
};

// Registers the records above; called once by a document Session.
[[nodiscard]] Result<void> register_editor_types(Runtime&);
}
