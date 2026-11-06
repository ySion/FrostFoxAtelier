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
    void evaluate(const Context& context, Geometry& output) const;
};

ACLASS(Name = "procedural.Grid", DESC = "XZ 平面上的点阵，以原点为中心，N 朝 +Y。")
class Grid : public Node {
    AGENERATED_BODY(Grid)

public:
    AFIELD(Label = "Size", DESC = "点阵沿 X 和 Z 的总长度，单位 m。")
    math::Vector2 size{10.f, 10.f};

    AFIELD(Label = "Rows", Min = "1", Max = "100", DESC = "沿 Z 的点数。")
    std::uint32_t rows{10};

    AFIELD(Label = "Columns", Min = "1", Max = "100", DESC = "沿 X 的点数。")
    std::uint32_t columns{10};

    AFUNCTION(DESC = "生成点阵。")
    void evaluate(Geometry& output) const;
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
    void evaluate(Geometry& output) const;
};

ACLASS(Name = "procedural.Box", DESC = "以原点为中心的长方体，八个共享点、六个四边形，顶点法线保留硬边。")
class Box : public Node {
    AGENERATED_BODY(Box)

public:
    AFIELD(Label = "Size", DESC = "X、Y、Z 的总长度，均须有限且为正，单位 m。")
    math::Vector3 size{1.f, 1.f, 1.f};

    AFUNCTION(DESC = "生成六面体。")
    void evaluate(Geometry& output) const;
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
    void evaluate(Geometry& output) const;
};

ACLASS(Name = "procedural.Tube", DESC = "以原点为中心、沿 Y 轴的带盖实心圆柱；侧面四边形，两个多边形盖。")
class Tube : public Node {
    AGENERATED_BODY(Tube)

public:
    AFIELD(Label = "Radius", Min = "0", Max = "10", LockMin, DESC = "有限且为正的半径，单位 m。")
    float radius{1.f};

    AFIELD(Label = "Height", Min = "0", Max = "10", LockMin, DESC = "有限且为正的总高度，单位 m。")
    float height{2.f};

    AFIELD(Label = "Segments", Min = "3", Max = "128", LockMin, DESC = "绕 Y 轴的分段数。")
    std::uint32_t segments{32};

    AFUNCTION(DESC = "生成圆柱；侧面法线径向平滑，盖的法线为正负 Y。")
    void evaluate(Geometry& output) const;
};

ACLASS(Name = "procedural.Polyline", DESC = "把纯点输入按点序连成一条开放折线，保留点与 detail 属性，不隐式闭合。")
class Polyline : public Node {
    AGENERATED_BODY(Polyline)

public:
    AFUNCTION(DESC = "连接输入点；0 点输出空几何，1 点、有图元或实例时报错。")
    void evaluate(const Geometry& points, Geometry& output) const;
};

ACLASS(Name = "procedural.GroupBox", DESC = "按闭区间包围盒选点，写入或替换一个 point 层 bool 组。")
class GroupBox : public Node {
    AGENERATED_BODY(GroupBox)

public:
    AFIELD(Label = "Group", DESC = "输出 bool 列名，不能为空；已有列必须是 point bool。")
    String group{"selected"};

    AFIELD(Label = "Minimum", DESC = "包围盒各轴下界，包含边界，单位 m。")
    math::Vector3 minimum{-.5f, -.5f, -.5f};

    AFIELD(Label = "Maximum", DESC = "包围盒各轴上界，包含边界，单位 m。")
    math::Vector3 maximum{.5f, .5f, .5f};

    AFUNCTION(DESC = "在输入几何的副本上建立点组。")
    void evaluate(const Geometry& input, Geometry& output) const;
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
    void evaluate(const Context& context, const Geometry& surface, Geometry& points) const;
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
    void evaluate(const Context& context, const Geometry& surface, Geometry& points) const;
};

ACLASS(
    Name = "procedural.Transform",
    DESC =
        "依次缩放、旋转、平移几何。各列按用途变换：位置完整变换，向量只用线性部分，法线用逆转置，镜像时反转图元的顶点顺序。")
class Transform : public Node {
    AGENERATED_BODY(Transform)

public:
    AFIELD(Label = "Translate", DESC = "平移，单位 m。")
    math::Vector3 translate;

    AFIELD(Label = "Rotate", DESC = "旋转，单位四元数。")
    math::Quaternion rotate;

    AFIELD(Label = "Scale", DESC = "沿 X、Y、Z 的缩放比例。")
    math::Vector3 scale{1.f, 1.f, 1.f};

    AFUNCTION(DESC = "变换输入几何。")
    void evaluate(const Geometry& input, Geometry& output) const;
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
    void evaluate(const Geometry& input, Geometry& output) const;
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
    void evaluate(const Geometry& input, Geometry& output) const;
};

ACLASS(Name = "procedural.Merge", DESC = "按连接顺序拼接各输入的每一层，属性按名字合并。")
class Merge : public Node {
    AGENERATED_BODY(Merge)

public:
    AFUNCTION(DESC = "合并输入。")
    void evaluate(const Geometries& inputs, Geometry& output) const;
};

AENUM(Name = "procedural.RayMode", DESC = "project：沿方向找最近的交点；minimum：取最近的表面点。")
enum class RayMode : std::uint32_t { project, minimum };

ACLASS(
    Name = "procedural.Ray",
    DESC =
        "把点移到凸多边形表面上：沿方向投射或者取最近点。记录原图元 hitprim、首角扇形子三角 hittri、子三角内 hitprimuv 和 hitdist；未命中时两个编号与距离为 -1。")
class Ray : public Node {
    AGENERATED_BODY(Ray)

public:
    AFIELD(Label = "Mode", DESC = "project 沿 Direction 投射；minimum 取最近的表面点。")
    RayMode mode{RayMode::project};

    AFIELD(Label = "Direction", DESC = "project 模式的投射方向，内部归一化。")
    math::Vector3 direction{0.f, -1.f, 0.f};

    AFIELD(Label = "Max Distance", Min = "0", Max = "1000", LockMin, DESC = "最远查找距离，单位 m。")
    float max_distance{1000.f};

    AFIELD(Label = "Hit Normal", DESC = "命中时把点的 N 设为命中三角形的法线。")
    bool normal{};

    AFIELD(Label = "Remove Misses", DESC = "删除没有命中的点。")
    bool remove_misses{};

    AFUNCTION(DESC = "把点移到表面上。")
    void evaluate(const Geometry& points, const Geometry& surface, Geometry& output) const;
};

ACLASS(Name = "procedural.CopyToPoints",
       DESC = "在每个点上放一个网格实例：变换为 S·O·T，S 来自 scale 与 pscale，O 来自 orient 或 N 与 up，T 来自 P。")
class CopyToPoints : public Node {
    AGENERATED_BODY(CopyToPoints)

public:
    AFIELD(Label = "Meshes", DESC = "网格资源键列表，点的变体属性从中选取。")
    Array<String> meshes;

    AFIELD(Label = "Variant Attribute", DESC = "点上的 uint 属性，作为 Meshes 的下标；缺失时都用第一个网格。")
    String variant{"variant"};

    AFUNCTION(DESC = "把点转成实例。")
    void evaluate(const Context& context, const Geometry& points, Geometry& output) const;
};

ACLASS(Name = "procedural.Realize", DESC = "在目标点上复制有序几何源，生成真实点和图元，不生成资源实例。")
class Realize : public Node {
    AGENERATED_BODY(Realize)

public:
    AFIELD(Label = "Group", DESC = "仅选择目标点的 bool 组；空为全选，显式缺列报错。")
    String group;

    AFIELD(Label = "Variant Attribute", DESC = "目标点 uint 列，选择 Sources 的连接序号；缺失为 0。")
    String variant{"variant"};

    AFIELD(Label = "Target Attributes", DESC = "显式广播到副本 point 层的目标点属性；默认不广播，禁止 P。")
    Array<String> target_attributes;

    AFUNCTION(DESC = "按目标点序批量复制几何，源不得含 instance。")
    void evaluate(const Geometries& sources, const Geometry& points, Geometry& output) const;
};

}
