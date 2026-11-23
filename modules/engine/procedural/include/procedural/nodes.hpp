#pragma once

#include "procedural/graph.hpp"
#include "runtime/reflection.hpp"

#include "procedural/nodes.gen.hpp"

namespace atelier::procedural {

ACLASS(
    Name = "procedural.MeshImport",
    DESC =
        "把网格资源导入为几何：每个三角形一个图元，法线和 UV 在顶点层，材质槽号在图元层的 material。位置逐位相同的顶点合成同一个点。")
class MeshImport : public Node {
    AGENERATED_BODY(MeshImport)

public:
    AFIELD(Label = "Mesh", DESC = "网格资源键。")
    String mesh;

    AFUNCTION(DESC = "解析网格并导入。")
    [[nodiscard]] Result<void> process(const Context& context, Geometry& output) const noexcept;
};

ACLASS(Name = "procedural.Grid", DESC = "可细分的四边形/三角形平面网格或点阵，自带朝向法线与 [0,1] UV。")
class Grid : public Node {
    AGENERATED_BODY(Grid)

public:
    AFIELD(Label = "Primitive Type", Category = "Grid",
           DESC = "quads 四边形网格，triangles 三角形网格，points 纯点阵。")
    GridPrimitive primitive_type{GridPrimitive::quads};

    AFIELD(Label = "Orientation", Category = "Grid", DESC = "zx 水平地面（+Y），xy 正立面（+Z），yz 侧立面（+X）。")
    OrientationPlane orientation{OrientationPlane::zx};

    AFIELD(Label = "Size", Category = "Dimensions", DESC = "平面两个切向轴的总跨度，单位 m。")
    math::Vector2 size{10.f, 10.f};

    AFIELD(Label = "Center", Category = "Dimensions", DESC = "网格中心位置，单位 m。")
    math::Vector3 center;

    AFIELD(Label = "Rows", Category = "Dimensions", Min = "1", Max = "256", LockMin,
           DESC = "第二切向轴的采样行数（表面模式至少为 2）。")
    std::uint32_t rows{10};

    AFIELD(Label = "Columns", Category = "Dimensions", Min = "1", Max = "256", LockMin,
           DESC = "第一切向轴的采样列数（表面模式至少为 2）。")
    std::uint32_t columns{10};

    AFUNCTION(DESC = "生成平面网格或点阵。")
    [[nodiscard]] Result<void> process(Geometry& output) const noexcept;
};

ACLASS(Name = "procedural.Line", DESC = "从起点到终点等距排列的点，含两端。")
class Line : public Node {
    AGENERATED_BODY(Line)

public:
    AFIELD(Label = "Start", DESC = "第一个点的位置，单位 m。")
    math::Vector3 start;

    AFIELD(Label = "End", DESC = "最后一个点的位置，单位 m。")
    math::Vector3 end{0.f, 1.f, 0.f};

    AFIELD(Label = "Points", Min = "1", Max = "100", DESC = "点数。")
    std::uint32_t count{2};

    AFUNCTION(DESC = "生成线上的点。")
    [[nodiscard]] Result<void> process(Geometry& output) const noexcept;
};

ACLASS(Name = "procedural.Box",
       DESC = "闭合长方体网格，支持三轴细分、底面贴齐地面（Base on Ground），保留硬边顶点法线与逐面 UV。")
class Box : public Node {
    AGENERATED_BODY(Box)

public:
    AFIELD(Label = "Size", Category = "Dimensions", DESC = "X、Y、Z 的总长度，均须有限且为正，单位 m。")
    math::Vector3 size{1.f, 1.f, 1.f};

    AFIELD(Label = "Center", Category = "Transform", DESC = "长方体参考位置，单位 m。")
    math::Vector3 center;

    AFIELD(Label = "Base on Ground", Category = "Transform",
           DESC = "开启时将底面贴齐 Center.y（Y 范围 [Center.y, Center.y + Size.y]），消除半高偏移心算。")
    bool base_on_ground{false};

    AFIELD(Label = "Divisions X", Category = "Subdivisions", Min = "1", Max = "64", LockMin, DESC = "沿 X 轴的分段数。")
    std::uint32_t divisions_x{1};

    AFIELD(Label = "Divisions Y", Category = "Subdivisions", Min = "1", Max = "64", LockMin, DESC = "沿 Y 轴的分段数。")
    std::uint32_t divisions_y{1};

    AFIELD(Label = "Divisions Z", Category = "Subdivisions", Min = "1", Max = "64", LockMin, DESC = "沿 Z 轴的分段数。")
    std::uint32_t divisions_z{1};

    AFUNCTION(DESC = "生成闭合长方体网格。")
    [[nodiscard]] Result<void> process(Geometry& output) const noexcept;
};

ACLASS(Name = "procedural.Sphere", DESC = "以原点为中心的球面多面体；极区为三角形，中间为四边形，顶点法线径向平滑。")
class Sphere : public Node {
    AGENERATED_BODY(Sphere)

public:
    AFIELD(Label = "Radius", Min = "0", Max = "10", LockMin, DESC = "有限且为正的半径，单位 m。")
    float radius{1.f};

    AFIELD(Label = "Segments", Min = "3", Max = "128", LockMin, DESC = "绕 Y 轴的经向分段数。")
    std::uint32_t segments{32};

    AFIELD(Label = "Latitude Intervals", Min = "2", Max = "64", LockMin, DESC = "北极到南极的纬向区间数。")
    std::uint32_t latitudes{16};

    AFUNCTION(DESC = "生成共享缝与极点的球面。")
    [[nodiscard]] Result<void> process(Geometry& output) const noexcept;
};

ACLASS(
    Name = "procedural.Tube",
    DESC =
        "沿 Y 轴的参数化柱/锥/棱台/中空圆管/扇形柱：上下半径可独立收分（0 为尖锥/棱锥），支持内径中空与扇形切口封盖。")
class Tube : public Node {
    AGENERATED_BODY(Tube)

public:
    AFIELD(Label = "Radius Top", Category = "Dimensions", Min = "0", Max = "20", LockMin,
           DESC = "顶端外半径（设为 0 即圆锥或棱锥屋顶），单位 m。")
    float radius_top{1.f};

    AFIELD(Label = "Radius Bottom", Category = "Dimensions", Min = "0", Max = "20", LockMin,
           DESC = "底端外半径（不可与顶端同时为 0），单位 m。")
    float radius_bottom{1.f};

    AFIELD(Label = "Inner Radius", Category = "Dimensions", Min = "0", Max = "19", LockMin,
           DESC = "中空内半径（0 为实心；大于 0 时生成管壁与凸梯形环盖，须小于两端外半径），单位 m。")
    float inner_radius{0.f};

    AFIELD(Label = "Height", Category = "Dimensions", Min = "0.001", Max = "50", LockMin,
           DESC = "沿 Y 轴的总高度，单位 m。")
    float height{2.f};

    AFIELD(Label = "Center", Category = "Transform", DESC = "柱体参考位置，单位 m。")
    math::Vector3 center;

    AFIELD(Label = "Base on Ground", Category = "Transform", DESC = "开启时将底面贴齐 Center.y。")
    bool base_on_ground{false};

    AFIELD(Label = "Segments", Category = "Resolution & Arc", Min = "3", Max = "256", LockMin,
           DESC = "绕 Y 轴的环向分段数（4 为四棱锥/台，6 为六棱柱）。")
    std::uint32_t segments{32};

    AFIELD(Label = "Height Rows", Category = "Resolution & Arc", Min = "1", Max = "64", LockMin,
           DESC = "沿高度方向的分段层数。")
    std::uint32_t rows{1};

    AFIELD(Label = "Arc Angle", Category = "Resolution & Arc", Min = "1", Max = "360", LockMin, LockMax,
           DESC = "环向张角（度）；小于 360° 且开启封盖时自动封闭两侧径向切面。")
    float arc_degrees{360.f};

    AFIELD(Label = "End Caps", Category = "Resolution & Arc", DESC = "封闭上下端盖及扇形切口以构成水密实体。")
    bool end_caps{true};

    AFUNCTION(DESC = "生成参数化柱/锥/管网格。")
    [[nodiscard]] Result<void> process(Geometry& output) const noexcept;
};

ACLASS(Name = "procedural.Wedge",
       DESC = "闭合楔形体 / 双坡屋顶棱柱：Peak Ratio 在 0（直角斜坡/扶壁）与 0.5（对称人字双坡屋顶）之间连续调节。")
class Wedge : public Node {
    AGENERATED_BODY(Wedge)

public:
    AFIELD(Label = "Size", Category = "Dimensions", DESC = "沿 X（跨度）、Y（脊高）、Z（进深）的总尺寸，单位 m。")
    math::Vector3 size{2.f, 1.f, 2.f};

    AFIELD(Label = "Peak Ratio", Category = "Dimensions", Min = "0", Max = "1", LockMin, LockMax,
           DESC = "屋脊在 X 方向的归一化位置：0 为直角斜坡，0.5 为对称双坡屋顶，1 为反向斜坡。")
    float peak_ratio{0.5f};

    AFIELD(Label = "Center", Category = "Transform", DESC = "楔形体参考位置，单位 m。")
    math::Vector3 center;

    AFIELD(Label = "Base on Ground", Category = "Transform", DESC = "开启时将底面贴齐 Center.y。")
    bool base_on_ground{false};

    AFIELD(Label = "Divisions Z", Category = "Subdivisions", Min = "1", Max = "64", LockMin,
           DESC = "沿屋脊 Z 方向的分段数。")
    std::uint32_t divisions_z{1};

    AFUNCTION(DESC = "生成闭合楔形体或双坡屋顶。")
    [[nodiscard]] Result<void> process(Geometry& output) const noexcept;
};

ACLASS(Name = "procedural.Circle",
       DESC = "平面圆、椭圆、正多边形或圆弧曲线：可输出平面多边形、闭合折线截面或供拱门/弧墙 Sweep 使用的开放圆弧。")
class Circle : public Node {
    AGENERATED_BODY(Circle)

public:
    AFIELD(Label = "Primitive Type", Category = "Circle",
           DESC = "polygon 闭合面，closed_polyline 闭合折线截面，open_arc 开放圆弧路径。")
    CirclePrimitive primitive_type{CirclePrimitive::polygon};

    AFIELD(Label = "Orientation", Category = "Circle",
           DESC = "zx 水平面（+Y），xy 正立面（+Z，0..180° 即正立半圆拱），yz 侧立面（+X）。")
    OrientationPlane orientation{OrientationPlane::zx};

    AFIELD(Label = "Radius", Category = "Dimensions", DESC = "两正交切向轴的半轴长（相等为圆，不等为椭圆），单位 m。")
    math::Vector2 radius{1.f, 1.f};

    AFIELD(Label = "Center", Category = "Dimensions", DESC = "圆心位置，单位 m。")
    math::Vector3 center;

    AFIELD(Label = "Segments", Category = "Arc & Resolution", Min = "1", Max = "256", LockMin,
           DESC = "分段数（多边形与闭合环至少为 3；如 6 即正六边形）。")
    std::uint32_t segments{32};

    AFIELD(Label = "Start Angle", Category = "Arc & Resolution", Min = "-360", Max = "360", DESC = "起始角（度）。")
    float start_degrees{0.f};

    AFIELD(Label = "Arc Angle", Category = "Arc & Resolution", Min = "1", Max = "360", LockMin, LockMax,
           DESC = "圆弧张角（度）；180° 配合 open_arc 即半圆拱线。")
    float arc_degrees{360.f};

    AFUNCTION(DESC = "生成平面圆、多边形或圆弧折线。")
    [[nodiscard]] Result<void> process(Geometry& output) const noexcept;
};

ACLASS(Name = "procedural.Torus",
       DESC = "闭合圆环体或带封盖弯管（柱础圆线脚、圆环、弯头），要求管半径严格小于主半径以保持无自交水密流形。")
class Torus : public Node {
    AGENERATED_BODY(Torus)

public:
    AFIELD(Label = "Orientation", Category = "Transform", DESC = "圆环所在主平面。")
    OrientationPlane orientation{OrientationPlane::zx};

    AFIELD(Label = "Major Radius", Category = "Dimensions", Min = "0.01", Max = "50", LockMin,
           DESC = "圆环中心到管心的半径，单位 m。")
    float major_radius{1.f};

    AFIELD(Label = "Minor Radius", Category = "Dimensions", Min = "0.005", Max = "25", LockMin,
           DESC = "截面管半径（须小于 Major Radius），单位 m。")
    float minor_radius{0.25f};

    AFIELD(Label = "Center", Category = "Transform", DESC = "圆环中心位置，单位 m。")
    math::Vector3 center;

    AFIELD(Label = "Base on Ground", Category = "Transform", DESC = "开启时将最低点贴齐 Center.y。")
    bool base_on_ground{false};

    AFIELD(Label = "Ring Segments", Category = "Resolution & Arc", Min = "3", Max = "256", LockMin,
           DESC = "沿主环周的分段数。")
    std::uint32_t major_segments{32};

    AFIELD(Label = "Tube Segments", Category = "Resolution & Arc", Min = "3", Max = "128", LockMin,
           DESC = "沿管截面的分段数。")
    std::uint32_t minor_segments{16};

    AFIELD(Label = "Arc Angle", Category = "Resolution & Arc", Min = "1", Max = "360", LockMin, LockMax,
           DESC = "主环张角（度）；小于 360° 时为弯管段。")
    float arc_degrees{360.f};

    AFIELD(Label = "End Caps", Category = "Resolution & Arc", Meta("ui.visible_when", "arc_degrees!=360"),
           DESC = "张角小于 360° 时封闭弯管两端截面。")
    bool end_caps{true};

    AFUNCTION(DESC = "生成圆环或弯管网格。")
    [[nodiscard]] Result<void> process(Geometry& output) const noexcept;
};

ACLASS(Name = "procedural.Polyline", DESC = "把纯点输入按点序连成一条开放折线，保留点与 detail 属性，不隐式闭合。")
class Polyline : public Node {
    AGENERATED_BODY(Polyline)

public:
    AFUNCTION(DESC = "连接输入点；0 点输出空几何，1 点、有图元或实例时报错。")
    [[nodiscard]] Result<void> process(const Geometry& points, Geometry& output) const noexcept;
};

ACLASS(Name = "procedural.GroupBox", DESC = "按闭区间包围盒选点，写入或替换一个 point 层 bool 组。")
class GroupBox : public Node {
    AGENERATED_BODY(GroupBox)

public:
    AFIELD(Label = "Group", DESC = "输出 bool 列名，不能为空；已有列必须是 point bool。")
    String group = String::literal<"selected">();

    AFIELD(Label = "Minimum", DESC = "包围盒各轴下界，包含边界，单位 m。")
    math::Vector3 minimum{-.5f, -.5f, -.5f};

    AFIELD(Label = "Maximum", DESC = "包围盒各轴上界，包含边界，单位 m。")
    math::Vector3 maximum{.5f, .5f, .5f};

    AFUNCTION(DESC = "在输入几何的副本上建立点组。")
    [[nodiscard]] Result<void> process(const Geometry& input, Geometry& output) const noexcept;
};

ACLASS(Name = "procedural.Boolean", DESC = "对两个闭合有向实体做并集、交集或差集，保留来源面的属性。")
class Boolean : public Node {
    AGENERATED_BODY(Boolean)

public:
    AFIELD(Label = "Operation", DESC = "unite 并集，intersect 交集，subtract 从 A 切除 B。")
    BooleanOperation operation{BooleanOperation::unite};

    AFUNCTION(DESC = "求两个实体的布尔结果。")
    [[nodiscard]] Result<void> process(const Geometry& a, const Geometry& b, Geometry& output) const noexcept;
};

ACLASS(Name = "procedural.Connectivity", DESC = "按共享点的拓扑给连通块编号；独立的散点各成一块。")
class Connectivity : public Node {
    AGENERATED_BODY(Connectivity)

public:
    AFIELD(Label = "Piece Attribute", DESC = "写入 point 层的 uint 属性，按每个连通块的最小点索引从 0 编号。")
    String attribute = String::literal<"piece">();

    AFUNCTION(DESC = "保留输入几何，在点上标记各个连通块。")
    [[nodiscard]] Result<void> process(const Geometry& input, Geometry& output) const noexcept;
};

ACLASS(Name = "procedural.Scatter", DESC = "在三角形和凸平面多边形上按面积撒点。")
class Scatter : public Node {
    AGENERATED_BODY(Scatter)

public:
    AFIELD(Label = "Density", Min = "0", Max = "100", LockMin, DESC = "每平方米的点数，淘汰过近的点之前。")
    float density{1.f};

    AFIELD(Label = "Seed", DESC = "与图的种子混合后得到本节点的随机种子。")
    std::uint32_t seed{};

    AFIELD(Label = "Minimum Distance", Min = "0", Max = "10", LockMin,
           DESC = "两点之间的最小距离，单位 m。0 表示不淘汰。")
    float min_distance{};

    AFUNCTION(DESC = "在三角形和凸平面多边形上按面积撒点。")
    [[nodiscard]] Result<void> process(const Context& context, const Geometry& surface,
                                       Geometry& points) const noexcept;
};

ACLASS(Name = "procedural.VolumeScatter", DESC = "在闭合有向流形壳内按密度和固定格撒点，排除表面，不修补网格。")
class VolumeScatter : public Node {
    AGENERATED_BODY(VolumeScatter)

public:
    AFIELD(Label = "Density", Min = "0", Max = "100", LockMin, DESC = "筛选前每立方米的期望点数。")
    float density{1.f};

    AFIELD(Label = "Seed", DESC = "与图种子混合，不依赖格的遍历下标。")
    std::uint32_t seed{};

    AFIELD(Label = "Origin", DESC = "固定格原点，几何局部坐标，单位 m。")
    math::Vector3 origin;

    AFIELD(Label = "Cell Size", Min = "0", Max = "10", LockMin, DESC = "有限且为正的格边长，单位 m，不按包围盒推导。")
    float cell_size{1.f};

    AFUNCTION(DESC = "在输入壳集合的严格内部生成点。")
    [[nodiscard]] Result<void> process(const Context& context, const Geometry& surface,
                                       Geometry& points) const noexcept;
};

ACLASS(
    Name = "procedural.Transform",
    DESC =
        "围绕 Pivot 依次缩放、按欧拉角（X → Y → Z 度）旋转并平移几何。各列按用途变换：位置完整变换，向量只用线性部分，法线用逆转置，朝向四元数复合旋转，镜像时反转多边形顶点顺序。")
class Transform : public Node {
    AGENERATED_BODY(Transform)

public:
    AFIELD(Label = "Translate", DESC = "平移偏移，单位 m。")
    math::Vector3 translate;

    AFIELD(Label = "Rotate", DESC = "按 X → Y → Z 顺序应用的欧拉旋转角，单位度（°）。")
    math::Vector3 rotate;

    AFIELD(Label = "Scale", DESC = "沿 X、Y、Z 的缩放比例。")
    math::Vector3 scale{1.f, 1.f, 1.f};

    AFIELD(Label = "Pivot", DESC = "缩放与旋转围绕的局部枢轴点，单位 m。")
    math::Vector3 pivot;

    AFUNCTION(DESC = "变换输入几何。")
    [[nodiscard]] Result<void> process(const Geometry& input, Geometry& output) const noexcept;
};

ACLASS(Name = "procedural.MatchSize",
       DESC = "按包围盒将几何对齐到目标坐标或参考几何（默认 XZ 居中、底面贴齐 Y=0），可选等比或非等比归一化尺寸。")
class MatchSize : public Node {
    AGENERATED_BODY(MatchSize)

public:
    AFIELD(Label = "Justify X", Category = "Alignment", DESC = "X 轴对齐锚点。")
    AxisJustify justify_x{AxisJustify::center};

    AFIELD(Label = "Justify Y", Category = "Alignment", DESC = "Y 轴对齐锚点（默认 Min 即底面接地）。")
    AxisJustify justify_y{AxisJustify::min};

    AFIELD(Label = "Justify Z", Category = "Alignment", DESC = "Z 轴对齐锚点。")
    AxisJustify justify_z{AxisJustify::center};

    AFIELD(Label = "Target Position", Category = "Alignment",
           DESC = "未连接 reference 端口时使用的对齐目标坐标，单位 m。")
    math::Vector3 target_position;

    AFIELD(Label = "Scale to Fit", Category = "Target Size", DESC = "开启时将输入缩放到目标包围盒尺寸。")
    bool scale_to_fit{false};

    AFIELD(Label = "Uniform Scale", Category = "Target Size", Meta("ui.visible_when", "scale_to_fit=true"),
           DESC = "保持原始高宽比等比缩放，内接于目标尺寸。")
    bool uniform_scale{true};

    AFIELD(Label = "Target Size", Category = "Target Size", Meta("ui.visible_when", "scale_to_fit=true"),
           DESC = "未连接 reference 端口时使用的目标包围盒尺寸，单位 m。")
    math::Vector3 target_size{1.f, 1.f, 1.f};

    AFUNCTION(DESC = "对齐并归一化输入几何；reference 端口为空时使用面板目标参数。")
    [[nodiscard]] Result<void> process(const Geometry& input, const Geometry& reference,
                                       Geometry& output) const noexcept;
};

ACLASS(Name = "procedural.Promote", DESC = "把属性移到另一层，按 Reduce 合成该层每个元素接触到的值。")
class Promote : public Node {
    AGENERATED_BODY(Promote)

public:
    AFIELD(Label = "Attribute", DESC = "要移动的属性名。")
    String name;

    AFIELD(Label = "To", DESC = "目标层。")
    Layer to{Layer::point};

    AFIELD(Label = "Reduce", DESC = "多个来源值如何合成一个。")
    Reduce reduce{Reduce::average};

    AFUNCTION(DESC = "移动属性。")
    [[nodiscard]] Result<void> process(const Geometry& input, Geometry& output) const noexcept;
};

ACLASS(Name = "procedural.Remove", DESC = "删除一层中 bool 属性为 true 的元素。删除点时一并删除用到它们的图元。")
class Remove : public Node {
    AGENERATED_BODY(Remove)

public:
    AFIELD(Label = "Flag", DESC = "bool 属性名，为 true 的元素被删除。")
    String flag;

    AFIELD(Label = "Layer", DESC = "要删除元素的层。")
    Layer layer{Layer::point};

    AFUNCTION(DESC = "删除标记的元素。")
    [[nodiscard]] Result<void> process(const Geometry& input, Geometry& output) const noexcept;
};

ACLASS(Name = "procedural.Merge", DESC = "按连接顺序拼接各输入的每一层，属性按名字合并。")
class Merge : public Node {
    AGENERATED_BODY(Merge)

public:
    AFUNCTION(DESC = "合并输入。")
    [[nodiscard]] Result<void> process(const Geometries& inputs, Geometry& output) const noexcept;
};

ACLASS(
    Name = "procedural.Ray",
    DESC =
        "把点移到凸多边形表面上：沿方向投射或者取最近点。记录原图元 hitprim、首角扇形子三角 hittri、子三角内 hitprimuv 和 hitdist；未命中时两个编号与距离为 -1。")
class Ray : public Node {
    AGENERATED_BODY(Ray)

public:
    AFIELD(Label = "Mode", DESC = "project 沿 Direction 投射；minimum 取最近的表面点。")
    RayMode mode{RayMode::project};

    AFIELD(Label = "Direction", Meta("ui.visible_when", "mode=project"), DESC = "project 模式的投射方向，内部归一化。")
    math::Vector3 direction{0.f, -1.f, 0.f};

    AFIELD(Label = "Max Distance", Min = "0", Max = "1000", LockMin, DESC = "最远查找距离，单位 m。")
    float max_distance{1000.f};

    AFIELD(Label = "Hit Normal", DESC = "命中时把点的 N 设为命中三角形的法线。")
    bool normal{};

    AFIELD(Label = "Remove Misses", DESC = "删除没有命中的点。")
    bool remove_misses{};

    AFUNCTION(DESC = "把点移到表面上。")
    [[nodiscard]] Result<void> process(const Geometry& points, const Geometry& surface,
                                       Geometry& output) const noexcept;
};

ACLASS(Name = "procedural.CopyToPoints",
       DESC = "在每个点上放一个网格实例：变换为 S·O·T，S 来自 scale 与 pscale，O 来自 orient 或 N 与 up，T 来自 P。")
class CopyToPoints : public Node {
    AGENERATED_BODY(CopyToPoints)

public:
    AFIELD(Label = "Meshes", DESC = "网格资源键列表，点的变体属性从中选取。")
    Array<String> meshes;

    AFIELD(Label = "Variant Attribute", DESC = "点上的 uint 属性，作为 Meshes 的下标；缺失时都用第一个网格。")
    String variant = String::literal<"variant">();

    AFUNCTION(DESC = "把点转成实例。")
    [[nodiscard]] Result<void> process(const Context& context, const Geometry& points, Geometry& output) const noexcept;
};

ACLASS(Name = "procedural.Realize", DESC = "在目标点上复制有序几何源，生成真实点和图元，不生成资源实例。")
class Realize : public Node {
    AGENERATED_BODY(Realize)

public:
    AFIELD(Label = "Group", DESC = "仅选择目标点的 bool 组；空为全选，显式缺列报错。")
    String group;

    AFIELD(Label = "Variant Attribute", DESC = "目标点 uint 列，选择 Sources 的连接序号；缺失为 0。")
    String variant = String::literal<"variant">();

    AFIELD(Label = "Target Attributes", DESC = "显式广播到副本 point 层的目标点属性；默认不广播，禁止 P。")
    Array<String> target_attributes;

    AFUNCTION(DESC = "按目标点序批量复制几何，源不得含 instance。")
    [[nodiscard]] Result<void> process(const Geometries& sources, const Geometry& points,
                                       Geometry& output) const noexcept;
};

ACLASS(
    Name = "procedural.Extrude",
    DESC = "将选中的凸平面多边形沿法向挤出并做平面角平分内缩，保留硬边法线与来源图元属性；可选封闭背面构成水密体块。")
class Extrude : public Node {
    AGENERATED_BODY(Extrude)

public:
    AFIELD(Label = "Distance", Category = "Extrusion", Min = "-10", Max = "10", DESC = "沿面法向的挤出距离，单位 m。")
    float distance{0.5f};

    AFIELD(Label = "Inset", Category = "Extrusion", Min = "-2", Max = "2",
           DESC = "在原多边形平面内沿角平分线内缩的距离（负值为外扩），单位 m。")
    float inset{0.f};

    AFIELD(Label = "Individual Faces", Category = "Extrusion",
           DESC = "为 true 时逐面独立挤出；为 false 时合并选中区域的共享内边。")
    bool individual{true};

    AFIELD(Label = "Output Back", Category = "Extrusion",
           DESC = "生成反向底盖以构成闭合流形体块；在已有闭合壳局部挤出时设为 false。")
    bool output_back{true};

    AFIELD(Label = "Group", Category = "Selection & Overrides",
           DESC = "仅挤出选中的 primitive 或 point bool 组；空表示全部多边形。")
    String group;

    AFIELD(Label = "Distance Attribute", Category = "Selection & Overrides",
           DESC = "可选的 primitive float 属性名，逐图元覆盖 Distance。")
    String distance_attribute;

    AFIELD(Label = "Inset Attribute", Category = "Selection & Overrides",
           DESC = "可选的 primitive float 属性名，逐图元覆盖 Inset。")
    String inset_attribute;

    AFIELD(Label = "Front Group", Category = "Output Groups", DESC = "可选的 primitive bool 输出组名，标记挤出顶面。")
    String front_group;

    AFIELD(Label = "Side Group", Category = "Output Groups", DESC = "可选的 primitive bool 输出组名，标记挤出侧面。")
    String side_group;

    AFUNCTION(DESC = "挤出输入多边形表面。")
    [[nodiscard]] Result<void> process(const Geometry& surface, Geometry& output) const noexcept;
};

ACLASS(Name = "procedural.Sweep",
       DESC = "沿输入开放折线扫掠矩形、圆管或自定义截面，使用 up 稳定斜接标架生成带封盖、法线与弧长 UV 的流形网格。")
class Sweep : public Node {
    AGENERATED_BODY(Sweep)

public:
    AFIELD(Label = "Profile", Category = "Profile",
           DESC = "rectangle 矩形截面，tube 圆管截面，custom 使用 cross_section 端口截面。")
    SweepProfile profile{SweepProfile::rectangle};

    AFIELD(Label = "Width", Category = "Profile", Min = "0.01", Max = "20", LockMin,
           Meta("ui.visible_when", "profile=rectangle"), DESC = "rectangle 截面的水平宽度，单位 m。")
    float width{1.f};

    AFIELD(Label = "Height", Category = "Profile", Min = "0.01", Max = "20", LockMin,
           Meta("ui.visible_when", "profile=rectangle"), DESC = "rectangle 截面的竖直高度，单位 m。")
    float height{1.f};

    AFIELD(Label = "Radius", Category = "Profile", Min = "0.01", Max = "10", LockMin,
           Meta("ui.visible_when", "profile=tube"), DESC = "tube 截面的半径，单位 m。")
    float radius{0.5f};

    AFIELD(Label = "Segments", Category = "Profile", Min = "3", Max = "64", LockMin,
           Meta("ui.visible_when", "profile=tube"), DESC = "tube 截面圆周分段数。")
    std::uint32_t segments{8};

    AFIELD(Label = "Up Vector", Category = "Frame & Caps", DESC = "稳定扫掠截面竖直方向的参考上向量。")
    math::Vector3 up{0.f, 1.f, 0.f};

    AFIELD(Label = "End Caps", Category = "Frame & Caps", DESC = "闭合截面时在折线首尾两端生成封盖。")
    bool end_caps{true};

    AFIELD(Label = "Scale Attribute", Category = "Frame & Caps",
           DESC = "可选的折线 point float 缩放属性名；缺失时按 1 处理。")
    String scale_attribute = String::literal<"pscale">();

    AFUNCTION(DESC = "沿 backbone 折线扫掠截面。")
    [[nodiscard]] Result<void> process(const Geometry& backbone, const Geometry& cross_section,
                                       Geometry& output) const noexcept;
};

ACLASS(Name = "procedural.Null",
       DESC = "无修改直通节点，常用作程序化图中的命名里程碑或稳定输出端点（如 OUT_terrain）。")
class Null : public Node {
    AGENERATED_BODY(Null)

public:
    AFUNCTION(DESC = "原样传递输入几何，零拷贝共享底层属性与拓扑列。")
    [[nodiscard]] Result<void> process(const Geometry& input, Geometry& output) const noexcept;
};

AENUM(Name = "procedural.PreviewFallback",
      DESC = "独立检视含 GraphInput 的 .procgraph 且无外层输入连线时提供的预览回退几何。")
enum class PreviewFallback : std::uint32_t {
    AMETA(Label = "Empty", DESC = "输出空几何。") empty = 0,
    AMETA(Label = "Grid (10m)", DESC = "10m × 10m XZ 平面点阵。") grid = 1,
    AMETA(Label = "Box (1m)", DESC = "1m 闭合六面体。") box = 2,
    AMETA(Label = "Sphere (r=1m)", DESC = "半径 1m 闭合球体。") sphere = 3,
    AMETA(Label = "Line (2m)", DESC = "沿 +Y 方向 2m 采样点列。") line = 4
};

ACLASS(Name = "procedural.GraphInput",
       DESC = "数字资产子图（.procgraph）的外部单几何输入边界节点；独立检视时可输出预览回退几何。")
class GraphInput : public Node {
    AGENERATED_BODY(GraphInput)

public:
    AFIELD(Label = "Port Name", Category = "Boundary", DESC = "暴露给外层 AssetNode 的输入端口标识符。")
    String port_name = String::literal<"input">();

    AFIELD(Label = "Port Order", Category = "Boundary", Min = "0", Max = "16",
           DESC = "多个 GraphInput 节点按此序号升序排列。")
    std::int32_t order{0};

    AFIELD(Label = "Standalone Fallback", Category = "Preview",
           DESC = "独立检视该 .procgraph 时使用的预览几何；作为 AssetNode 子图求值时忽略。")
    PreviewFallback preview_fallback{PreviewFallback::empty};

    AFUNCTION(DESC = "独立检视时生成回退预览几何；子图求值时由宿主注入外层输入。")
    [[nodiscard]] Result<void> process(Geometry& output) const noexcept;
};

ACLASS(Name = "procedural.GraphOutput",
       DESC = "数字资产子图（.procgraph）的正式几何输出边界节点，与作者调试 Display [D] 独立。")
class GraphOutput : public Node {
    AGENERATED_BODY(GraphOutput)

public:
    AFIELD(Label = "Port Name", Category = "Boundary", DESC = "暴露给外层 AssetNode 的输出端口标识符。")
    String port_name = String::literal<"output">();

    AFIELD(Label = "Port Order", Category = "Boundary", Min = "0", Max = "16",
           DESC = "多个 GraphOutput 节点按此序号升序排列；首项作为场景生成器的主交付输出。")
    std::int32_t order{0};

    AFUNCTION(DESC = "零拷贝透传输入几何作为图资产的正式输出。")
    [[nodiscard]] Result<void> process(const Geometry& input, Geometry& output) const noexcept;
};

ACLASS(Name = "procedural.AssetNode",
       DESC = "实例化调用另一个 .procgraph 数字资产的通用复合节点，持有私有克隆图根并共享结果缓存池。")
class AssetNode : public Node {
    AGENERATED_BODY(AssetNode)

public:
    struct SubgraphMeta {
        std::string asset_key;

        Ports ports; // Resolved schema.

        std::vector<ObjectId> input_nodes;

        std::vector<ObjectId> output_nodes;

        std::optional<Error> bind_error;

        std::uint64_t revision{};
    };

    AFIELD(Label = "Seed Offset", Category = "Asset",
           DESC = "0 原样透传当前求值种子；非零时经确定性哈希与外层种子混合。")
    std::uint32_t seed_offset{0};

    AFUNCTION(DESC = "求值所绑定的 .procgraph 数字资产子图。")
    [[nodiscard]] Result<void> process(const Context& context, Geometry& output) const noexcept;

private:
    friend struct detail::Access;

    AFIELD(Name = "subgraph_graph", Hidden, Strong = "procedural.Graph")
    ReferenceSlot subgraph_graph_;

    SubgraphMeta subgraph_{};
};

}
