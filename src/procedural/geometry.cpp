#include "procedural/geometry.hpp"

#include <algorithm>
#include <numeric>

namespace atelier::procedural {
namespace {

std::string text(StringView value) { return std::string{value.std_view()}; }

std::size_t column_size(const ColumnData& data) noexcept {
    return std::visit([](const auto& column) { return column->size(); }, data);
}
// Values at `indices`, or the type's default where an index is `absent`.
constexpr std::uint32_t absent = ~0u;
ColumnData gather(const ColumnData& data, std::span<const std::uint32_t> indices) {
    return std::visit([&](const auto& column) -> ColumnData {
        using T = typename std::remove_cvref_t<decltype(*column)>::Value;
        auto result = std::make_shared<Array<T>>();
        result->reserve(indices.size());
        for (const auto index : indices) {
            if (index == absent) result->emplace();
            else result->add((*column)[index]);
        }
        return result;
    }, data);
}
ColumnData empty_like(const ColumnData& data) {
    return std::visit([](const auto& column) -> ColumnData {
        using T = typename std::remove_cvref_t<decltype(*column)>::Value;
        return std::make_shared<Array<T>>();
    }, data);
}
// Appends `source`, or `count` default values when it is null.
void append(ColumnData& target, const ColumnData* source, std::uint32_t count) {
    std::visit([&](auto& column) {
        using T = typename std::remove_cvref_t<decltype(*column)>::Value;
        if (source) column->append(std::get<Column<T>>(*source)->view());
        else for (std::uint32_t i = 0; i < count; ++i) column->emplace();
    }, target);
}

}

Geometry::Geometry()
    : vertex_points_(std::make_shared<Array<std::uint32_t>>()),
      instance_references_(std::make_shared<Array<std::uint32_t>>()),
      instance_transforms_(std::make_shared<Array<math::Matrix4>>()) {
    attributes_.push_back({String{"P"}, Layer::point, Usage::point, std::make_shared<Array<math::Vector3>>()});
}

Result<Geometry> Geometry::points(Array<math::Vector3> positions) {
    if (positions.size() > absent - 1) return failure("Geometry supports fewer than 2^32 - 1 points.");
    for (const auto& position : positions)
        if (!math::is_finite(position)) return failure("Point positions must be finite.");
    Geometry result;
    result.attributes_.front().data = std::make_shared<Array<math::Vector3>>(std::move(positions));
    return result;
}

Result<Geometry> Geometry::polygons(Array<math::Vector3> positions, Array<std::uint32_t> vertex_points,
    const Array<std::uint32_t>& offsets) {
    auto result = points(std::move(positions));
    if (!result) return result;
    if (vertex_points.size() > absent - 1 || offsets.empty() || offsets[0] != 0 || offsets[offsets.size() - 1] != vertex_points.size())
        return failure("Primitive offsets must start at 0 and end at the vertex count.");
    const auto point_count = result->count(Layer::point);
    for (const auto point : vertex_points)
        if (point >= point_count) return failure("A vertex refers to a missing point.");
    std::uint32_t uniform = offsets.size() > 1 ? offsets[1] : 0;
    for (std::size_t p = 1; p < offsets.size(); ++p) {
        if (offsets[p] < offsets[p - 1] || offsets[p] - offsets[p - 1] < 3) return failure("Each primitive needs at least three vertices.");
        if (offsets[p] - offsets[p - 1] != uniform) uniform = 0;
    }
    result->vertices_ = static_cast<std::uint32_t>(vertex_points.size());
    result->primitives_ = static_cast<std::uint32_t>(offsets.size() - 1);
    result->vertex_points_ = std::make_shared<Array<std::uint32_t>>(std::move(vertex_points));
    if (uniform || !result->primitives_) result->uniform_ = uniform;
    else result->offsets_ = std::make_shared<Array<std::uint32_t>>(offsets);
    return result;
}

std::uint32_t Geometry::count(Layer layer) const noexcept {
    switch (layer) {
    case Layer::point: return static_cast<std::uint32_t>(positions().size());
    case Layer::vertex: return vertices_;
    case Layer::primitive: return primitives_;
    case Layer::detail: return 1;
    case Layer::instance: return static_cast<std::uint32_t>(instance_references_->size());
    }
    return 0;
}

const Attribute* Geometry::find(StringView name) const noexcept {
    for (const auto& attribute : attributes_)
        if (attribute.name.view() == name) return &attribute;
    return nullptr;
}
Attribute* Geometry::mutable_find(StringView name) noexcept {
    return const_cast<Attribute*>(std::as_const(*this).find(name));
}

Usage Geometry::conventional_usage(StringView name) noexcept {
    if (name == "P") return Usage::point;
    if (name == "N") return Usage::normal;
    if (name == "v" || name == "up") return Usage::vector;
    if (name == "Cd") return Usage::color;
    return Usage::none;
}

std::size_t Geometry::bytes() const noexcept {
    std::size_t total = (vertex_points_->capacity() + (offsets_ ? offsets_->capacity() : 0) + instance_references_->capacity()) *
        sizeof(std::uint32_t) + instance_transforms_->capacity() * sizeof(math::Matrix4) + references_.capacity() * sizeof(Reference);
    for (const auto& attribute : attributes_)
        total += std::visit([](const auto& column) {
            using T = typename std::remove_cvref_t<decltype(*column)>::Value;
            std::size_t size = column->capacity() * sizeof(T);
            if constexpr (std::same_as<T, String>) for (const auto& text : *column) size += text.size();
            return size;
        }, attribute.data);
    return total;
}

Result<void> Geometry::admit_name(StringView name) {
    const auto value = name.std_view();
    const auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    if (value.empty() || !letter(value[0]) ||
        !std::ranges::all_of(value, [&](char c) { return letter(c) || (c >= '0' && c <= '9'); }))
        return failure("Attribute name " + text(name) + " must be an identifier.");
    return {};
}

Result<void> Geometry::fits(Layer layer, const ColumnData& data) const {
    if (column_size(data) != count(layer)) return failure("Column size differs from its layer's element count.");
    return {};
}

Result<void> Geometry::set(String name, Layer layer, ColumnData data, std::optional<Usage> usage) {
    const auto resolved = usage ? *usage : conventional_usage(name);
    auto valid = std::visit([&](const auto& column) {
        using T = typename std::remove_cvref_t<decltype(*column)>::Value;
        return admit<T>(name, layer, resolved);
    }, data);
    if (!valid) return valid;
    if (auto sized = fits(layer, data); !sized) return sized;
    if (name.view() == "P") {
        if (layer != Layer::point || !std::holds_alternative<Column<math::Vector3>>(data))
            return failure("P is the point vector3 position.");
        for (const auto& position : *std::get<Column<math::Vector3>>(data))
            if (!math::is_finite(position)) return failure("Point positions must be finite.");
    }
    if (auto* attribute = mutable_find(name)) {
        attribute->layer = layer; attribute->usage = resolved; attribute->data = std::move(data);
    } else attributes_.push_back({std::move(name), layer, resolved, std::move(data)});
    return {};
}

Result<void> Geometry::remove(StringView name) {
    if (name == "P") return failure("P cannot be removed.");
    const auto at = std::ranges::find_if(attributes_, [&](const Attribute& a) { return a.name.view() == name; });
    if (at != attributes_.end()) attributes_.erase(at);
    return {};
}

Result<void> Geometry::set_instances(std::vector<Reference> references, Array<std::uint32_t> indices,
    Array<math::Matrix4> transforms) {
    if (indices.size() != transforms.size() || indices.size() > absent - 1)
        return failure("Each instance needs one reference index and one transform.");
    for (std::size_t i = 0; i < references.size(); ++i)
        for (std::size_t j = 0; j < i; ++j)
            if (references[i] == references[j]) return failure("Instance references must be distinct.");
    for (const auto index : indices)
        if (index >= references.size()) return failure("An instance names a missing reference.");
    for (const auto& matrix : transforms)
        if (!math::is_finite(matrix) || matrix[3] != 0 || matrix[7] != 0 || matrix[11] != 0 || matrix[15] != 1)
            return failure("Instance transforms must be finite and affine.");
    std::erase_if(attributes_, [](const Attribute& a) { return a.layer == Layer::instance; });
    references_ = std::move(references);
    instance_references_ = std::make_shared<Array<std::uint32_t>>(std::move(indices));
    instance_transforms_ = std::make_shared<Array<math::Matrix4>>(std::move(transforms));
    return {};
}

Array<math::Matrix4>& Geometry::write_instance_transforms() {
    if (instance_transforms_.use_count() != 1) instance_transforms_ = std::make_shared<Array<math::Matrix4>>(*instance_transforms_);
    return *instance_transforms_;
}

void Geometry::reverse_primitives() {
    if (!primitives_) return;
    Array<std::uint32_t> order;
    order.resize(vertices_);
    for (std::uint32_t p = 0; p < primitives_; ++p) {
        const auto range = primitive(p);
        for (std::uint32_t i = 0; i < range.count; ++i) order[range.first + i] = range.first + range.count - 1 - i;
    }
    vertex_points_ = std::get<Column<std::uint32_t>>(gather(vertex_points_, order.view()));
    for (auto& attribute : attributes_)
        if (attribute.layer == Layer::vertex) attribute.data = gather(attribute.data, order.view());
}

Result<Geometry> Geometry::remove_flagged(Layer layer, StringView flag) const {
    auto flags = read<bool>(flag, layer);
    if (!flags) return std::unexpected(flags.error());
    if (!*flags) return failure("Geometry has no attribute " + text(flag) + ".");
    return remove_flagged(layer, std::span<const bool>{(*flags)->data(), (*flags)->size()});
}

Result<Geometry> Geometry::remove_flagged(Layer layer, std::span<const bool> removed) const {
    if (layer != Layer::point && layer != Layer::primitive && layer != Layer::instance)
        return failure("Only points, primitives and instances can be removed.");
    if (removed.size() != count(layer)) return failure("Removal needs one flag per element.");
    const auto kept = [&](std::uint32_t count, auto keep) {
        Array<std::uint32_t> indices;
        for (std::uint32_t i = 0; i < count; ++i) if (keep(i)) indices.add(i);
        return indices;
    };
    Geometry result = *this;
    const auto retain = [&](Layer target, std::span<const std::uint32_t> indices) {
        for (auto& attribute : result.attributes_)
            if (attribute.layer == target) attribute.data = gather(attribute.data, indices);
    };
    if (layer == Layer::instance) {
        const auto instances = kept(count(Layer::instance), [&](std::uint32_t i) { return !removed[i]; });
        result.instance_references_ = std::get<Column<std::uint32_t>>(gather(instance_references_, instances.view()));
        result.instance_transforms_ = std::get<Column<math::Matrix4>>(gather(instance_transforms_, instances.view()));
        retain(Layer::instance, instances.view());
        return result;
    }
    const auto& vertex_points = *vertex_points_;
    const auto primitives = kept(primitives_, [&](std::uint32_t p) {
        if (layer == Layer::primitive) return !removed[p];
        const auto range = primitive(p);
        for (std::uint32_t v = range.first; v < range.first + range.count; ++v)
            if (removed[vertex_points[v]]) return false;
        return true;
    });
    Array<std::uint32_t> point_map; // Old point index to new, or absent.
    Array<std::uint32_t> points;
    point_map.resize(count(Layer::point));
    for (std::uint32_t i = 0; i < point_map.size(); ++i) {
        point_map[i] = layer == Layer::point && removed[i] ? absent : static_cast<std::uint32_t>(points.size());
        if (point_map[i] != absent) points.add(i);
    }
    Array<std::uint32_t> vertices, new_vertex_points, offsets{0};
    for (const auto p : primitives) {
        const auto range = primitive(p);
        for (std::uint32_t v = range.first; v < range.first + range.count; ++v) {
            vertices.add(v);
            new_vertex_points.add(point_map[vertex_points[v]]);
        }
        offsets.add(static_cast<std::uint32_t>(vertices.size()));
    }
    retain(Layer::point, points.view());
    retain(Layer::vertex, vertices.view());
    retain(Layer::primitive, primitives.view());
    result.vertices_ = static_cast<std::uint32_t>(vertices.size());
    result.primitives_ = static_cast<std::uint32_t>(primitives.size());
    result.vertex_points_ = std::make_shared<Array<std::uint32_t>>(std::move(new_vertex_points));
    result.offsets_.reset();
    result.uniform_ = 0;
    if (offsets_ && result.primitives_) result.offsets_ = std::make_shared<Array<std::uint32_t>>(std::move(offsets));
    else if (result.primitives_) result.uniform_ = uniform_;
    if (result.offsets_) { // Removal may leave only primitives of one size.
        const auto first = (*result.offsets_)[1];
        bool uniform = true;
        for (std::size_t p = 1; p < result.offsets_->size(); ++p)
            uniform = uniform && (*result.offsets_)[p] - (*result.offsets_)[p - 1] == first;
        if (uniform) { result.uniform_ = first; result.offsets_.reset(); }
    }
    return result;
}

Result<Geometry> Geometry::merge(std::span<const Geometry> inputs) {
    if (inputs.empty()) return Geometry{};
    if (inputs.size() == 1) return inputs.front();
    Geometry result;
    result.attributes_.clear();
    // Attribute order: first appearance across the inputs, P first.
    for (const auto& input : inputs)
        for (const auto& attribute : input.attributes_) {
            const auto* existing = result.find(attribute.name);
            if (!existing) {
                result.attributes_.push_back({attribute.name, attribute.layer, attribute.usage, empty_like(attribute.data)});
                continue;
            }
            if (existing->layer != attribute.layer || existing->data.index() != attribute.data.index() || existing->usage != attribute.usage)
                return failure("Merged attribute " + text(attribute.name) + " differs in layer, type or usage.");
        }
    std::uint64_t vertices{}, primitives{}, instances{};
    std::uint32_t uniform = 0;
    bool same = true;
    for (const auto& input : inputs) {
        vertices += input.vertices_; primitives += input.primitives_; instances += input.count(Layer::instance);
        if (!input.primitives_) continue;
        const auto size = input.uniform_primitive_size();
        if (!uniform && same) uniform = size;
        same = same && size && size == uniform;
    }
    std::uint64_t points{};
    for (const auto& input : inputs) points += input.count(Layer::point);
    if (points >= absent || vertices >= absent || primitives >= absent || instances >= absent)
        return failure("Merged geometry needs fewer than 2^32 - 1 elements per layer.");
    auto vertex_points = std::make_shared<Array<std::uint32_t>>();
    auto offsets = std::make_shared<Array<std::uint32_t>>(Array<std::uint32_t>{0});
    auto indices = std::make_shared<Array<std::uint32_t>>();
    auto transforms = std::make_shared<Array<math::Matrix4>>();
    std::uint32_t point_base = 0;
    for (const auto& input : inputs) {
        for (const auto point : *input.vertex_points_) vertex_points->add(point_base + point);
        const auto vertex_base = offsets->data()[offsets->size() - 1];
        for (std::uint32_t p = 0; p < input.primitives_; ++p) {
            const auto range = input.primitive(p);
            offsets->add(vertex_base + range.first + range.count);
        }
        for (std::size_t i = 0; i < input.instance_references_->size(); ++i) {
            const auto& reference = input.references_[(*input.instance_references_)[i]];
            auto at = std::ranges::find(result.references_, reference);
            if (at == result.references_.end()) at = result.references_.insert(at, reference);
            indices->add(static_cast<std::uint32_t>(at - result.references_.begin()));
        }
        transforms->append(input.instance_transforms_->view());
        for (auto& attribute : result.attributes_) {
            const auto* source = input.find(attribute.name);
            if (attribute.layer == Layer::detail) {
                if (column_size(attribute.data) == 0 && source) append(attribute.data, &source->data, 1);
                continue;
            }
            append(attribute.data, source ? &source->data : nullptr, input.count(attribute.layer));
        }
        point_base += input.count(Layer::point);
    }
    for (auto& attribute : result.attributes_)
        if (attribute.layer == Layer::detail && column_size(attribute.data) == 0) append(attribute.data, nullptr, 1);
    result.vertices_ = static_cast<std::uint32_t>(vertices);
    result.primitives_ = static_cast<std::uint32_t>(primitives);
    result.vertex_points_ = std::move(vertex_points);
    if (result.primitives_ && !same) result.offsets_ = std::move(offsets);
    else result.uniform_ = result.primitives_ ? uniform : 0;
    result.instance_references_ = std::move(indices);
    result.instance_transforms_ = std::move(transforms);
    return result;
}

}
