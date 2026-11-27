#pragma once

#include "foundation/array.hpp"
#include "foundation/optional.hpp"
#include "foundation/shared_array.hpp"
#include "foundation/span.hpp"
#include "foundation/string.hpp"
#include "geometry/mesh.hpp"
#include "math/math.hpp"
#include "runtime/reflection.hpp"

#include <cstdint>
#include <memory>
#include <variant>

#include "procedural/geometry.gen.hpp"

namespace atelier::procedural {

namespace detail {
struct BooleanBoundary;

class TriangleView;
}

// One past the largest count remains available as an invalid element index.
inline constexpr std::uint64_t element_limit = 0xffff'fffeu;

// Detail has exactly one element. Instances are their own layer, not points.
AENUM(Name = "procedural.Layer",
      DESC = "几何的一层：point 点，vertex 图元的角，primitive 图元，detail 整个几何（只有一个元素），instance 实例。")
enum class Layer : std::uint32_t { point, vertex, primitive, detail, instance };

AENUM(Name = "procedural.Reduce", DESC = "把多个元素的属性合成一个值：first 取第一个，sum 求和，average 求平均。")
enum class Reduce : std::uint32_t { first, sum, average };

AENUM(Name = "procedural.RayMode", DESC = "project：沿方向找最近的交点；minimum：取最近的表面点。")
enum class RayMode : std::uint32_t { project, minimum };

AENUM(Name = "procedural.BooleanOperation", DESC = "闭合实体的并集、交集或差集 A 减 B。")
enum class BooleanOperation : std::uint32_t { unite, intersect, subtract };

AENUM(Name = "procedural.GroupCriterion", DESC = "根据面法线、已有 bool 组或 float 属性的闭区间生成选择。")
enum class GroupCriterion : std::uint32_t { normal, existing, range };

AENUM(Name = "procedural.GroupOperation", DESC = "直接使用条件结果，或与指定的同层基组求并、交、差。")
enum class GroupOperation : std::uint32_t { replace, unite, intersect, subtract };

AENUM(Name = "procedural.ResampleMode", DESC = "按固定间距、固定段数或最大间距均分每条折线。")
enum class ResampleMode : std::uint32_t {
    AMETA(Label = "Spacing", DESC = "从起点按间距取样，末端保留余段。") spacing,
    AMETA(Label = "Segments", DESC = "按总弧长平分为指定段数。") segments,
    AMETA(Label = "Fit Spacing", DESC = "全长等分，使用满足最大间距的最少段数。") fit_spacing
};

AENUM(Name = "procedural.SweepProfile",
      DESC = "沿折线扫掠的截面轮廓：rectangle 矩形，tube 圆形多边形管，custom 来自辅助端口的截面。")
enum class SweepProfile : std::uint32_t { rectangle, tube, custom };

AENUM(Name = "procedural.OrientationPlane",
      DESC = "平面图元的构造参考平面：zx 为水平地面（法线 +Y），xy 为正立面（法线 +Z），yz 为侧立面（法线 +X）。")
enum class OrientationPlane : std::uint32_t {
    AMETA(Label = "ZX Plane (+Y)", DESC = "水平地面，法线朝 +Y。") zx = 0,
    AMETA(Label = "XY Plane (+Z)", DESC = "正立面，法线朝 +Z。") xy = 1,
    AMETA(Label = "YZ Plane (+X)", DESC = "侧立面，法线朝 +X。") yz = 2
};

AENUM(Name = "procedural.GridPrimitive",
      DESC = "Grid 输出的几何图元类型：quads 四边形网格，triangles 三角形网格，points 纯点阵。")
enum class GridPrimitive : std::uint32_t {
    AMETA(Label = "Quads", DESC = "共享顶点的平面四边形网格。") quads = 0,
    AMETA(Label = "Triangles", DESC = "共享顶点的平面三角形网格。") triangles = 1,
    AMETA(Label = "Points", DESC = "不生成图元的纯点阵。") points = 2
};

AENUM(Name = "procedural.CirclePrimitive",
      DESC = "Circle 输出的图元形态：polygon 闭合平面多边形，closed_polyline 首尾闭合折线，open_arc 开放圆弧折线。")
enum class CirclePrimitive : std::uint32_t {
    AMETA(Label = "Polygon", DESC = "闭合平面多边形（小于 360° 时生成通过圆心的凸三角扇）。") polygon = 0,
    AMETA(Label = "Closed Polyline", DESC = "首尾闭合的折线轮廓。") closed_polyline = 1,
    AMETA(Label = "Open Arc", DESC = "按张角生成的开放圆弧折线（可直接作为拱门或弧墙的 Sweep 路径）。") open_arc = 2
};

AENUM(Name = "procedural.AxisJustify",
      DESC = "沿单轴的包围盒对齐方式：none 保持不动，min 贴齐下界，center 居中对齐，max 贴齐上界。")
enum class AxisJustify : std::uint32_t {
    AMETA(Label = "None", DESC = "该轴保持原位置不动。") none = 0,
    AMETA(Label = "Min", DESC = "将几何包围盒下界对齐到目标坐标（如底面贴齐 Y=0）。") min = 1,
    AMETA(Label = "Center", DESC = "将几何包围盒中心对齐到目标坐标。") center = 2,
    AMETA(Label = "Max", DESC = "将几何包围盒上界对齐到目标坐标。") max = 3
};

AENUM(Name = "procedural.PrimitiveType",
      DESC = "图元的拓扑形态：polygon 为至少三个角的闭合多边形面，polyline 为至少两个角的开放折线。")
enum class PrimitiveType : std::uint32_t {
    AMETA(Label = "Polygon", DESC = "至少三个角的闭合平面多边形。") polygon = 0,
    AMETA(Label = "Polyline", DESC = "至少两个角、仅连接相邻角的开放折线。") polyline = 1
};

ASTRUCT(Name = "procedural.PrimitiveSpan", DESC = "单个图元在顶点层中的有序区间与拓扑形态。")
struct PrimitiveSpan {
    AGENERATED_BODY(PrimitiveSpan)

    AFIELD(DESC = "该图元第一个角在 vertex 层的下标。")
    std::uint32_t first_vertex{};

    AFIELD(DESC = "该图元拥有的有序角数量。")
    std::uint32_t vertex_count{};

    AFIELD(DESC = "图元的拓扑形态（polygon 或 polyline）。")
    PrimitiveType kind{PrimitiveType::polygon};
};

struct Spatial;

// How a transform changes a vector or orientation column; see transform().
enum class Usage : std::uint8_t { none, point, vector, normal, color, orientation };

using ColumnData =
    std::variant<SharedArray<bool>, SharedArray<std::int32_t>, SharedArray<std::uint32_t>, SharedArray<float>,
                 SharedArray<math::Vector2>, SharedArray<math::Vector3>, SharedArray<math::Vector4>,
                 SharedArray<math::Quaternion>, SharedArray<math::Matrix4>, SharedArray<String>>;

template <class T>
concept Element = requires(ColumnData data) { data = SharedArray<T>{}; };

struct Attribute {
    String name;

    Layer layer;

    Usage usage;

    ColumnData data;
};

// An instanced resource: `key` is its persistent asset key, `mesh` what the host resolved it to.
struct Reference {
    String key;

    geometry::MeshRef mesh;

    bool operator==(const Reference&) const = default;
};

enum class PrimitiveKind : std::uint8_t { polygon, polyline };

struct PrimitiveRange {
    std::uint32_t first;

    std::uint32_t count;

    PrimitiveKind kind;
};

[[nodiscard]] inline std::unexpected<Error> failure(std::string message) {
    return std::unexpected(Error{ErrorCode::argument_mismatch, std::move(message)});
}

// Points, polygons and open polylines over them, instances and per-layer attribute columns. A value:
// copies share every column, and writing a column first makes it unique. Each writer
// must own its Geometry exclusively. While a column is borrowed, do not move or
// structurally edit the Geometry, or write that column again. A writable borrow also
// forbids copying the Geometry or its column handle; read-only borrows allow copies.
// Point positions are the point attribute P, which always exists. Topology and
// instance columns are reserved: attribute bindings cannot reach them.
ASTRUCT(Name = "procedural.Geometry", DESC = "点、顶点、图元、实例及各层属性构成的几何值。复制共享各列，写入时才复制。")
class Geometry {
    AGENERATED_BODY(Geometry)

public:
    Geometry();

    static Result<Geometry> points(Array<math::Vector3> positions);

    // Primitive p owns vertices [offsets[p], offsets[p+1]); vertex v refers to point
    // vertex_points[v]. Offsets start at 0, end at the vertex count, and give each
    // primitive at least three vertices.
    static Result<Geometry> polygons(Array<math::Vector3> positions, Array<std::uint32_t> vertex_points,
                                     const Array<std::uint32_t>& offsets);

    // Open polylines over `positions`, with at least two vertices per primitive.
    static Result<Geometry> polylines(Array<math::Vector3> positions, Array<std::uint32_t> vertex_points,
                                      const Array<std::uint32_t>& offsets);

    // The same topology with one kind per primitive. Polylines need at least two
    // vertices and connect only adjacent vertices, never implicitly last to first.
    static Result<Geometry> primitives(Array<math::Vector3> positions, Array<std::uint32_t> vertex_points,
                                       const Array<std::uint32_t>& offsets, Array<PrimitiveKind> kinds);

    [[nodiscard]] std::uint32_t count(Layer layer) const noexcept;

    [[nodiscard]] Span<const std::uint32_t> vertex_points() const noexcept { return vertex_points_.array(); }

    [[nodiscard]] PrimitiveRange primitive(std::uint32_t index) const noexcept {
        if (offsets_.empty())
            return {index * uniform_, uniform_, kinds_.array()[index]};
        const auto* offsets = offsets_.array().data();
        return {offsets[index], offsets[index + 1] - offsets[index], kinds_.array()[index]};
    }

    // Shared vertex count of every primitive, or 0 when sizes differ or nothing exists.
    [[nodiscard]] std::uint32_t uniform_primitive_size() const noexcept { return offsets_.empty() ? uniform_ : 0; }

    [[nodiscard]] Span<const Attribute> attributes() const noexcept { return attributes_; }

    [[nodiscard]] const Attribute* find(StringView name) const noexcept;

    [[nodiscard]] const Array<math::Vector3>& positions() const noexcept {
        return std::get<SharedArray<math::Vector3>>(attributes_.front().data).array();
    }

    // Null when absent; an attribute of another layer or type is an error.
    template <Element T>
    Result<const Array<T>*> read(StringView name, Layer layer) const {
        const auto* attribute = find(name);
        if (!attribute)
            return nullptr;
        auto checked = check<T>(*attribute, layer);
        if (!checked)
            return std::unexpected(checked.error());
        return &std::get<SharedArray<T>>(attribute->data).array();
    }

    template <Element T>
    Result<Array<T>*> write(StringView name, Layer layer) {
        auto* attribute = mutable_find(name);
        if (!attribute)
            return failure("Geometry has no attribute " + std::string{name.std_view()} + ".");
        if (auto checked = check<T>(*attribute, layer); !checked)
            return std::unexpected(checked.error());
        if (name == "P")
            discard_boolean_boundary();
        return &std::get<SharedArray<T>>(attribute->data).write();
    }

    // Returns the existing column for writing, or adds one filled with `fill`. Usage defaults
    // to the conventional one for the name (P point, N normal, v and up vector, Cd color).
    template <Element T>
    Result<Array<T>*> add(StringView name, Layer layer, T fill = T{}, Optional<Usage> usage = {}) {
        if (find(name))
            return write<T>(name, layer);
        const auto resolved = usage ? *usage : conventional_usage(name);
        if (auto valid = admit<T>(name, layer, resolved); !valid)
            return std::unexpected(valid.error());
        auto owned_name = String::from_utf8(name);
        if (!owned_name)
            return failure("Attribute name requires valid UTF-8.");
        Array<T> values;
        values.resize(count(layer), fill);
        attributes_.add({std::move(*owned_name), layer, resolved, SharedArray<T>{std::move(values)}});
        return &std::get<SharedArray<T>>(attributes_.back().data).write();
    }

    // Takes ownership of a complete column. Replaces an existing attribute only when its
    // layer and column type match, preserving its existing usage when `usage` is omitted.
    Result<void> set(String name, Layer layer, ColumnData data, Optional<Usage> usage = {});

    Result<void> remove(StringView name);

    static Result<void> admit_name(StringView name);

    [[nodiscard]] Span<const Reference> references() const noexcept { return references_; }

    [[nodiscard]] const Array<std::uint32_t>& instance_references() const noexcept {
        return instance_references_.array();
    }

    [[nodiscard]] const Array<math::Matrix4>& instance_transforms() const noexcept {
        return instance_transforms_.array();
    }

    // Replaces the instance layer, dropping its attributes. References must be distinct
    // and each index must name one; transforms must be finite and affine.
    Result<void> set_instances(Array<Reference> references, Array<std::uint32_t> indices,
                               Array<math::Matrix4> transforms);

    Array<math::Matrix4>& write_instance_transforms();

    // Pure points to one open polyline in point order, sharing point/detail columns.
    // No primitives or instances are accepted. Zero points pass through; one fails.
    // Empty vertex/primitive columns expand with type defaults; instance columns stay empty.
    Result<Geometry> connect_points() const;

    // Reverses vertex order and vertex attributes. A reflection passes polygons_only
    // to correct face winding without changing a polyline's direction.
    void reverse_primitives(bool polygons_only = false);

    // Elements whose `flag` is true are removed. Removing points removes the primitives that
    // use them; removing primitives keeps their points.
    Result<Geometry> remove_flagged(Layer layer, StringView flag) const;

    // The same with one flag per element of `layer`.
    Result<Geometry> remove_flagged(Layer layer, Span<const bool> removed) const;

    // Concatenates layers in input order. Attributes join by name; an element without a
    // value takes the type's default. Detail values come from the first input that has
    // them. A name with different layers, types or usages is an error.
    static Result<Geometry> merge(Span<const Geometry> inputs);

    [[nodiscard]] static Usage conventional_usage(StringView name) noexcept;

    // Exact value comparison for replay, including float bits, resolved mesh identities and Boolean constructions.
    [[nodiscard]] bool same_value(const Geometry& other) const noexcept;

    // Bytes of the columns and topology this value holds, counting shared columns in full.
    [[nodiscard]] std::size_t bytes() const noexcept;

private:
    friend class Bvh;

    friend struct Spatial;

    friend struct detail::BooleanBoundary;

    std::shared_ptr<const detail::BooleanBoundary> boolean_boundary_;

    friend class detail::TriangleView;

    [[nodiscard]] std::size_t boolean_boundary_bytes() const noexcept;

    bool same_boolean_boundary(const Geometry& other) const noexcept;

    void discard_boolean_boundary() noexcept { boolean_boundary_.reset(); }

    Attribute* mutable_find(StringView name) noexcept;

    template <Element T>
    Result<void> check(const Attribute& attribute, Layer layer) const {
        if (attribute.layer != layer || !std::holds_alternative<SharedArray<T>>(attribute.data))
            return failure("Attribute " + std::string{attribute.name.view().std_view()} +
                           " has another layer or type.");
        return {};
    }

    template <Element T>
    Result<void> admit(StringView name, Layer layer, Usage usage) const {
        if (auto valid = admit_name(name); !valid)
            return valid;
        if constexpr (std::same_as<T, String>)
            if (layer != Layer::detail)
                return failure("String attributes live on the detail layer.");
        if (usage == Usage::none)
            return {};
        if (usage == Usage::orientation) {
            if constexpr (std::same_as<T, math::Quaternion>)
                return {};
            return failure("Orientation usage needs a quaternion column.");
        }
        if (std::same_as<T, math::Vector3> || (std::same_as<T, math::Vector4> && usage == Usage::color))
            return {};
        return failure("Usage needs a vector3 column, a vector4 color, or a quaternion orientation.");
    }

    Result<void> fits(Layer layer, const ColumnData& data) const;

    std::uint32_t vertices_{};

    std::uint32_t primitives_{};

    std::uint32_t uniform_{};

    SharedArray<std::uint32_t> vertex_points_;

    SharedArray<std::uint32_t> offsets_; // offsets_ is empty when every primitive has uniform_ vertices.

    SharedArray<PrimitiveKind> kinds_;

    Array<Attribute> attributes_; // P first.

    Array<Reference> references_;

    SharedArray<std::uint32_t> instance_references_;

    SharedArray<math::Matrix4> instance_transforms_;
};

using Geometries = Array<Geometry>;

}
