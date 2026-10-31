#pragma once
#include "foundation/string.hpp"
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
AENUM(Name = "editor.Orientation", DESC = "移动、旋转手柄和 X/Y/Z 约束使用的坐标轴；缩放总是使用 Local。")
enum class Orientation : std::uint32_t { global, local };
AENUM(Name = "editor.CameraView", DESC = "视口相机：Perspective 透视，或沿某个坐标轴的正交视图。")
enum class CameraView : std::uint32_t { perspective, top, front, right, bottom, back, left };
AENUM(Name = "editor.AddKind", DESC = "新建对象的种类：空 Entity、静态网格、点光、矩形面光、平行光或相机。")
enum class AddKind : std::uint32_t { entity, mesh, point, rectangle, directional, camera };

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
};

ASTRUCT(Name = "editor.SceneAdd", DESC = "在 3D 光标处新建一个 Entity，并选中它。")
struct SceneAdd {
    AGENERATED_BODY(SceneAdd)
    AFIELD(DESC = "新建的种类。")
    AddKind kind{AddKind::entity};
    AFIELD(DESC = "网格资源键，只用于 mesh；为空时使用第一个内置网格。")
    String mesh{};
};

ASTRUCT(Name = "editor.ViewAxis", DESC = "把视口相机切换到透视或某个轴向的正交视图，保持观察目标。")
struct ViewAxis {
    AGENERATED_BODY(ViewAxis)
    AFIELD(DESC = "目标视图。")
    CameraView view{CameraView::perspective};
};

// Registers the records above; called once by a document Session.
[[nodiscard]] Result<void> register_editor_types(Runtime&);
}
