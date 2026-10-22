#pragma once

#include "foundation/array.hpp"
#include "foundation/string.hpp"
#include "geometry/mesh.hpp"
#include "math/math.hpp"
#include "runtime/reflection.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "procedural/geometry.gen.hpp"

namespace atelier::procedural {

// Detail has exactly one element. Instances are their own layer, not points.
AENUM(Name="procedural.Layer", DESC="几何的一层：point 点，vertex 图元的角，primitive 图元，detail 整个几何（只有一个元素），instance 实例。")
enum class Layer : std::uint32_t { point, vertex, primitive, detail, instance };
AENUM(Name="procedural.Reduce", DESC="把多个元素的属性合成一个值：first 取第一个，sum 求和，average 求平均。")
enum class Reduce : std::uint32_t { first, sum, average };
// How a transform changes a float vector column; see transform().
enum class Usage : std::uint8_t { none, point, vector, normal, color };

template<class T> using Column = std::shared_ptr<Array<T>>;
using ColumnData = std::variant<Column<bool>, Column<std::int32_t>, Column<std::uint32_t>, Column<float>,
    Column<math::Vector2>, Column<math::Vector3>, Column<math::Vector4>, Column<math::Quaternion>,
    Column<math::Matrix4>, Column<String>>;
template<class T> concept Element = requires(ColumnData data) { data = Column<T>{}; };

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

struct PrimitiveRange { std::uint32_t first, count; };

[[nodiscard]] inline std::unexpected<Error> failure(std::string message) {
    return std::unexpected(Error{ErrorCode::argument_mismatch, std::move(message)});
}

// Points, polygons over them, instances and per-layer attribute columns. A value:
// copies share every column, and writing a column first makes it unique. The sharing
// test reads owner counts, so make columns unique on one thread before parallel writers
// use them. Point positions are the point attribute P, which always exists. Topology and
// instance columns are reserved: attribute bindings cannot reach them.
ASTRUCT(Name="procedural.Geometry", DESC="点、顶点、图元、实例及各层属性构成的几何值。复制共享各列，写入时才复制。")
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

    [[nodiscard]] std::uint32_t count(Layer layer) const noexcept;
    [[nodiscard]] std::span<const std::uint32_t> vertex_points() const noexcept { return *vertex_points_; }
    [[nodiscard]] PrimitiveRange primitive(std::uint32_t index) const noexcept {
        if (!offsets_) return {index * uniform_, uniform_};
        const auto* offsets = offsets_->data();
        return {offsets[index], offsets[index + 1] - offsets[index]};
    }
    // Shared vertex count of every primitive, or 0 when sizes differ or nothing exists.
    [[nodiscard]] std::uint32_t uniform_primitive_size() const noexcept { return offsets_ ? 0 : uniform_; }

    [[nodiscard]] std::span<const Attribute> attributes() const noexcept { return attributes_; }
    [[nodiscard]] const Attribute* find(StringView name) const noexcept;
    [[nodiscard]] const Array<math::Vector3>& positions() const noexcept { return *std::get<Column<math::Vector3>>(attributes_.front().data); }
    // Null when absent; an attribute of another layer or type is an error.
    template<Element T> Result<const Array<T>*> read(StringView name, Layer layer) const {
        const auto* attribute = find(name);
        if (!attribute) return nullptr;
        auto checked = check<T>(*attribute, layer);
        if (!checked) return std::unexpected(checked.error());
        return std::get<Column<T>>(attribute->data).get();
    }
    template<Element T> Result<Array<T>*> write(StringView name, Layer layer) {
        auto* attribute = mutable_find(name);
        if (!attribute) return failure("Geometry has no attribute " + std::string{name.std_view()} + ".");
        if (auto checked = check<T>(*attribute, layer); !checked) return std::unexpected(checked.error());
        auto& column = std::get<Column<T>>(attribute->data);
        if (column.use_count() != 1) column = std::make_shared<Array<T>>(*column);
        return column.get();
    }
    // Returns the existing column for writing, or adds one filled with `fill`. Usage defaults
    // to the conventional one for the name (P point, N normal, v and up vector, Cd color).
    template<Element T> Result<Array<T>*> add(StringView name, Layer layer, T fill = T{}, std::optional<Usage> usage = {}) {
        if (find(name)) return write<T>(name, layer);
        const auto resolved = usage ? *usage : conventional_usage(name);
        if (auto valid = admit<T>(name, layer, resolved); !valid) return std::unexpected(valid.error());
        Array<T> values;
        values.reserve(count(layer));
        for (std::uint32_t i = 0; i < count(layer); ++i) values.add(fill);
        auto column = std::make_shared<Array<T>>(std::move(values));
        auto* result = column.get();
        attributes_.push_back({String{name}, layer, resolved, std::move(column)});
        return result;
    }
    // Takes ownership of a complete column. Replaces an attribute of the same name.
    Result<void> set(String name, Layer layer, ColumnData data, std::optional<Usage> usage = {});
    Result<void> remove(StringView name);

    [[nodiscard]] std::span<const Reference> references() const noexcept { return references_; }
    [[nodiscard]] const Array<std::uint32_t>& instance_references() const noexcept { return *instance_references_; }
    [[nodiscard]] const Array<math::Matrix4>& instance_transforms() const noexcept { return *instance_transforms_; }
    // Replaces the instance layer, dropping its attributes. References must be distinct
    // and each index must name one; transforms must be finite and affine.
    Result<void> set_instances(std::vector<Reference> references, Array<std::uint32_t> indices,
        Array<math::Matrix4> transforms);
    Array<math::Matrix4>& write_instance_transforms();

    // Reverses each primitive's vertex order, with vertex attributes, keeping faces
    // oriented after a reflection.
    void reverse_primitives();
    // Elements whose `flag` is true are removed. Removing points removes the primitives that
    // use them; removing primitives keeps their points.
    Result<Geometry> remove_flagged(Layer layer, StringView flag) const;
    // Concatenates layers in input order. Attributes join by name; an element without a
    // value takes the type's default. Detail values come from the first input that has
    // them. A name with different layers, types or usages is an error.
    static Result<Geometry> merge(std::span<const Geometry> inputs);

    [[nodiscard]] static Usage conventional_usage(StringView name) noexcept;
    // Bytes of the columns and topology this value holds, counting shared columns in full.
    [[nodiscard]] std::size_t bytes() const noexcept;
private:
    Attribute* mutable_find(StringView name) noexcept;
    template<Element T> Result<void> check(const Attribute& attribute, Layer layer) const {
        if (attribute.layer != layer || !std::holds_alternative<Column<T>>(attribute.data))
            return failure("Attribute " + std::string{attribute.name.view().std_view()} + " has another layer or type.");
        return {};
    }
    template<Element T> Result<void> admit(StringView name, Layer layer, Usage usage) const {
        if (auto valid = admit_name(name); !valid) return valid;
        if constexpr (std::same_as<T, String>)
            if (layer != Layer::detail) return failure("String attributes live on the detail layer.");
        if (usage == Usage::none) return {};
        if (std::same_as<T, math::Vector3> || (std::same_as<T, math::Vector4> && usage == Usage::color)) return {};
        return failure("Usage needs a vector3 column, or a vector4 color.");
    }
    static Result<void> admit_name(StringView name);
    Result<void> fits(Layer layer, const ColumnData& data) const;

    std::uint32_t vertices_{}, primitives_{}, uniform_{};
    Column<std::uint32_t> vertex_points_, offsets_; // offsets_ is null when every primitive has uniform_ vertices.
    std::vector<Attribute> attributes_;             // P first.
    std::vector<Reference> references_;
    Column<std::uint32_t> instance_references_;
    Column<math::Matrix4> instance_transforms_;
};
using Geometries = Array<Geometry>;

}
