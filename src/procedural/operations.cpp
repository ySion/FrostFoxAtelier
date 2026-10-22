#include "procedural/operations.hpp"
#include "procedural/random.hpp"

#include <bit>
#include <cmath>
#include <unordered_map>

namespace atelier::procedural {
namespace {

using math::Matrix4;
using math::Quaternion;
using math::Vector2;
using math::Vector3;
using math::Vector3d;

constexpr std::uint64_t element_limit = 0xffff'fffeu;

template<class T> T value(Result<T> result) { return std::move(*result); }

Vector3d widen(Vector3 v) { return {v.x, v.y, v.z}; }
Vector3 narrow(Vector3d v) { return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)}; }

// sign(det) * cofactor rows of the linear part, in double; a zero determinant keeps sign +1.
std::array<double, 9> normal_rows(const Matrix4& m) {
    const double a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    std::array<double, 9> cofactors{e * i - f * h, f * g - d * i, d * h - e * g,
                                    c * h - b * i, a * i - c * g, b * g - a * h,
                                    b * f - c * e, c * d - a * f, a * e - b * d};
    if (a * cofactors[0] + b * cofactors[1] + c * cofactors[2] < 0)
        for (double& value : cofactors) value = -value;
    return cofactors;
}
Vector3 transform_normal(Vector3 n, const std::array<double, 9>& rows) {
    const Vector3d result{n.x * rows[0] + n.y * rows[3] + n.z * rows[6],
                          n.x * rows[1] + n.y * rows[4] + n.z * rows[7],
                          n.x * rows[2] + n.y * rows[5] + n.z * rows[8]};
    const double length = math::length(result);
    return length == 0 || !std::isfinite(length) ? Vector3{} : narrow(result / length);
}

// The shortest rotation taking +Z to the unit vector `z`.
Quaternion dihedral(Vector3 z) {
    const double w = 1.0 + z.z;
    if (w < 1e-7) return {1, 0, 0, 0}; // Opposite: half a turn about X.
    const double length = std::sqrt(z.y * double(z.y) + z.x * double(z.x) + w * w);
    return {static_cast<float>(-z.y / length), static_cast<float>(z.x / length), 0, static_cast<float>(w / length)};
}

}

Geometry import_mesh(const geometry::Mesh& mesh) {
    struct Key {
        std::array<std::uint32_t, 3> bits;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& key) const noexcept {
            return hash(key.bits[0], hash(key.bits[1], key.bits[2]));
        }
    };
    const auto vertices = mesh.vertices();
    std::unordered_map<Key, std::uint32_t, KeyHash> welded;
    Array<Vector3> positions;
    Array<std::uint32_t> vertex_points, offsets, materials;
    auto normals = std::make_shared<Array<Vector3>>();
    auto uvs = std::make_shared<Array<Vector2>>();
    vertex_points.reserve(vertices.size()); normals->reserve(vertices.size()); uvs->reserve(vertices.size());
    for (const auto& vertex : vertices) {
        const Key key{{std::bit_cast<std::uint32_t>(vertex.position[0]), std::bit_cast<std::uint32_t>(vertex.position[1]),
                       std::bit_cast<std::uint32_t>(vertex.position[2])}};
        const auto [at, added] = welded.try_emplace(key, static_cast<std::uint32_t>(positions.size()));
        if (added) positions.add({vertex.position[0], vertex.position[1], vertex.position[2]});
        vertex_points.add(at->second);
        normals->add({vertex.normal[0], vertex.normal[1], vertex.normal[2]});
        uvs->add({vertex.uv[0], vertex.uv[1]});
    }
    const auto triangles = static_cast<std::uint32_t>(vertices.size() / 3);
    offsets.reserve(triangles + 1);
    for (std::uint32_t t = 0; t <= triangles; ++t) offsets.add(t * 3);
    auto material = std::make_shared<Array<std::uint32_t>>();
    material->resize(triangles);
    for (const auto& part : mesh.parts())
        for (auto t = part.first_vertex / 3; t < (part.first_vertex + part.vertex_count) / 3; ++t) (*material)[t] = part.material;
    auto result = value(Geometry::polygons(std::move(positions), std::move(vertex_points), offsets));
    (void)result.set(String{"N"}, Layer::vertex, std::move(normals));
    (void)result.set(String{"uv"}, Layer::vertex, std::move(uvs));
    (void)result.set(String{"material"}, Layer::primitive, std::move(material));
    return result;
}

Result<Geometry> grid(Vector2 size, std::uint32_t rows, std::uint32_t columns) {
    if (!math::is_finite(size) || std::uint64_t{rows} * columns > element_limit)
        return failure("Grid size must be finite and its point count below 2^32 - 1.");
    Array<Vector3> positions;
    positions.reserve(std::size_t{rows} * columns);
    const auto at = [](float extent, std::uint32_t i, std::uint32_t count) {
        return count < 2 ? 0.f : static_cast<float>(extent * (double(i) / (count - 1) - 0.5));
    };
    for (std::uint32_t r = 0; r < rows; ++r)
        for (std::uint32_t c = 0; c < columns; ++c) positions.add({at(size.x, c, columns), 0, at(size.y, r, rows)});
    auto result = Geometry::points(std::move(positions));
    if (!result) return result;
    if (auto normals = result->add<Vector3>("N", Layer::point, {0, 1, 0}); !normals) return std::unexpected(normals.error());
    return result;
}

Result<Geometry> line(Vector3 start, Vector3 end, std::uint32_t count) {
    if (count > element_limit) return failure("A line has fewer than 2^32 - 1 points.");
    Array<Vector3> positions;
    positions.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const double t = count < 2 ? 0.0 : double(i) / (count - 1);
        positions.add(narrow(widen(start) + (widen(end) - widen(start)) * t));
    }
    return Geometry::points(std::move(positions));
}

Result<Geometry> scatter(const Geometry& surface, const ScatterSettings& settings) {
    if (!std::isfinite(settings.density) || settings.density < 0 || !std::isfinite(settings.min_distance) || settings.min_distance < 0)
        return failure("Scatter density and minimum distance must be finite and non-negative.");
    const auto triangles = surface.count(Layer::primitive);
    if (triangles && surface.uniform_primitive_size() != 3) return failure("Scatter needs triangle primitives.");
    const auto& points = surface.positions();
    const auto vertex_points = surface.vertex_points();
    Array<Vector3> positions, normals;
    Array<std::uint32_t> ids, primitives, ordinals;
    Array<Vector2> barycentrics;
    for (std::uint32_t t = 0; t < triangles; ++t) {
        const auto v0 = widen(points[vertex_points[t * 3]]);
        const auto e1 = widen(points[vertex_points[t * 3 + 1]]) - v0;
        const auto e2 = widen(points[vertex_points[t * 3 + 2]]) - v0;
        const auto normal = math::cross(e1, e2);
        const double area = math::length(normal) / 2;
        const double expected = area * settings.density;
        if (!(area > 0) || !(expected > 0)) continue;
        const auto seed = hash(settings.seed, t);
        const double whole = std::floor(expected);
        const auto count = whole + (unit(hash(seed, 0u)) < expected - whole ? 1 : 0);
        if (positions.size() + count > element_limit) return failure("Scatter would create 2^32 - 1 points or more.");
        const auto face = narrow(normal / (area * 2));
        for (std::uint32_t k = 0; k < static_cast<std::uint32_t>(count); ++k) {
            float r1 = unit(hash(seed, 2 * k + 1)), r2 = unit(hash(seed, 2 * k + 2));
            if (r1 + r2 > 1) { r1 = 1 - r1; r2 = 1 - r2; }
            positions.add(narrow(v0 + e1 * double(r1) + e2 * double(r2)));
            normals.add(face);
            ids.add(hash(t, k));
            primitives.add(t);
            ordinals.add(k);
            barycentrics.add({r1, r2});
        }
    }
    if (settings.min_distance > 0 && !positions.empty()) {
        // Cells as wide as the distance: a kept point closer than it lies in a neighbouring cell.
        const double cell = settings.min_distance, limit = cell * cell;
        struct CellHash {
            std::size_t operator()(const std::array<std::int64_t, 3>& c) const noexcept {
                return hash(static_cast<std::uint32_t>(c[0]), hash(static_cast<std::uint32_t>(c[1]), static_cast<std::uint32_t>(c[2])));
            }
        };
        std::unordered_map<std::array<std::int64_t, 3>, std::vector<std::uint32_t>, CellHash> cells;
        Array<std::uint32_t> kept;
        for (std::uint32_t i = 0; i < positions.size(); ++i) {
            const auto p = widen(positions[i]);
            const std::array<std::int64_t, 3> c{static_cast<std::int64_t>(std::floor(p.x / cell)),
                static_cast<std::int64_t>(std::floor(p.y / cell)), static_cast<std::int64_t>(std::floor(p.z / cell))};
            bool near = false;
            for (std::int64_t x = -1; x <= 1 && !near; ++x)
                for (std::int64_t y = -1; y <= 1 && !near; ++y)
                    for (std::int64_t z = -1; z <= 1 && !near; ++z) {
                        const auto found = cells.find({c[0] + x, c[1] + y, c[2] + z});
                        if (found == cells.end()) continue;
                        for (const auto other : found->second) {
                            const auto d = widen(positions[other]) - p;
                            if (math::dot(d, d) < limit) { near = true; break; }
                        }
                    }
            if (near) continue;
            cells[c].push_back(i);
            kept.add(i);
        }
        const auto keep = [&](auto& column) {
            std::remove_cvref_t<decltype(column)> result;
            result.reserve(kept.size());
            for (const auto i : kept) result.add(column[i]);
            column = std::move(result);
        };
        keep(positions); keep(normals); keep(ids); keep(primitives); keep(ordinals); keep(barycentrics);
    }
    auto result = Geometry::points(std::move(positions));
    if (!result) return result;
    for (auto added : {result->set(String{"N"}, Layer::point, std::make_shared<Array<Vector3>>(std::move(normals))),
                       result->set(String{"id"}, Layer::point, std::make_shared<Array<std::uint32_t>>(std::move(ids))),
                       result->set(String{"sourceprim"}, Layer::point, std::make_shared<Array<std::uint32_t>>(std::move(primitives))),
                       result->set(String{"sourceordinal"}, Layer::point, std::make_shared<Array<std::uint32_t>>(std::move(ordinals))),
                       result->set(String{"sourceprimuv"}, Layer::point, std::make_shared<Array<Vector2>>(std::move(barycentrics)))})
        if (!added) return std::unexpected(added.error());
    return result;
}

Result<void> transform(Geometry& geometry, Vector3 translate, Quaternion rotation, Vector3 scale) {
    const auto unit_rotation = math::try_normalize(rotation);
    if (!math::is_finite(translate) || !math::is_finite(scale) || !unit_rotation)
        return failure("Transform needs a finite translation and scale and a nonzero rotation.");
    const auto matrix = math::trs_matrix(translate, *unit_rotation, scale);
    const auto normals = normal_rows(matrix);
    std::vector<std::pair<std::string, Layer>> targets;
    for (const auto& attribute : geometry.attributes())
        targets.emplace_back(std::string{attribute.name.view().std_view()}, attribute.layer);
    for (const auto& [name, layer] : targets) {
        const auto* attribute = geometry.find(StringView{name});
        const auto view = StringView{name};
        if (std::holds_alternative<Column<Quaternion>>(attribute->data) && name == "orient") {
            auto& values = *value(geometry.write<Quaternion>(view, layer));
            for (auto& q : values) q = *unit_rotation * q;
            continue;
        }
        if (!std::holds_alternative<Column<Vector3>>(attribute->data)) continue;
        const auto usage = attribute->usage;
        if (usage != Usage::point && usage != Usage::vector && usage != Usage::normal) continue;
        auto& values = *value(geometry.write<Vector3>(view, layer));
        for (auto& v : values) {
            v = usage == Usage::point ? math::transform_point(v, matrix)
              : usage == Usage::vector ? math::transform_direction(v, matrix) : transform_normal(v, normals);
            if (!math::is_finite(v)) return failure("Transformed " + name + " is not finite.");
        }
    }
    if (geometry.count(Layer::instance))
        for (auto& instance : geometry.write_instance_transforms()) {
            instance = instance * matrix;
            if (!math::is_finite(instance)) return failure("Transformed instances are not finite.");
        }
    if (scale.x * double(scale.y) * scale.z < 0) geometry.reverse_primitives();
    return {};
}

Result<void> promote(Geometry& geometry, StringView name, Layer to, Reduce reduce) {
    const auto* attribute = geometry.find(name);
    if (!attribute) return failure("Geometry has no attribute " + std::string{name.std_view()} + ".");
    const auto from = attribute->layer;
    if (from == to) return {};
    if (name == "P") return failure("P stays on points.");
    if ((from == Layer::instance && to != Layer::detail) || (to == Layer::instance && from != Layer::detail))
        return failure("Instance attributes only exchange with detail.");
    // Sources of each target element as offsets into `sources`.
    Array<std::uint32_t> starts{0}, sources;
    const auto vertex_points = geometry.vertex_points();
    const auto primitives = geometry.count(Layer::primitive);
    const auto targets = geometry.count(to);
    const auto close = [&] { starts.add(static_cast<std::uint32_t>(sources.size())); };
    if (from == Layer::detail) {
        for (std::uint32_t i = 0; i < targets; ++i) { sources.add(0); close(); }
    } else if (to == Layer::detail) {
        for (std::uint32_t i = 0; i < geometry.count(from); ++i) sources.add(i);
        close();
    } else if (to == Layer::primitive) {
        for (std::uint32_t p = 0; p < primitives; ++p) {
            const auto range = geometry.primitive(p);
            for (auto v = range.first; v < range.first + range.count; ++v) sources.add(from == Layer::vertex ? v : vertex_points[v]);
            close();
        }
    } else if (to == Layer::vertex) {
        for (std::uint32_t p = 0; p < primitives; ++p) {
            const auto range = geometry.primitive(p);
            for (auto v = range.first; v < range.first + range.count; ++v) { sources.add(from == Layer::point ? vertex_points[v] : p); close(); }
        }
    } else { // To points from vertices or primitives: bucket the vertices by point.
        Array<std::uint32_t> counts, vertex_primitive;
        counts.resize(targets + 1);
        vertex_primitive.resize(vertex_points.size());
        for (std::uint32_t p = 0; p < primitives; ++p) {
            const auto range = geometry.primitive(p);
            for (auto v = range.first; v < range.first + range.count; ++v) vertex_primitive[v] = p;
        }
        for (const auto point : vertex_points) ++counts[point + 1];
        for (std::uint32_t i = 0; i < targets; ++i) counts[i + 1] += counts[i];
        starts = counts;
        sources.resize(vertex_points.size());
        for (std::uint32_t v = 0; v < vertex_points.size(); ++v)
            sources[counts[vertex_points[v]]++] = from == Layer::vertex ? v : vertex_primitive[v];
    }
    auto promoted = std::visit([&](const auto& column) -> Result<ColumnData> {
        using T = typename std::remove_cvref_t<decltype(*column)>::Value;
        constexpr bool numeric = std::same_as<T, float> || std::same_as<T, Vector2> || std::same_as<T, Vector3> || std::same_as<T, math::Vector4>;
        if constexpr (std::same_as<T, String>) return failure("String attributes stay on detail.");
        else {
            if (reduce != Reduce::first && !numeric) return failure("Sum and average need float or float vector attributes.");
            auto result = std::make_shared<Array<T>>();
            result->reserve(targets);
            for (std::uint32_t i = 0; i < targets; ++i) {
                const auto first = starts[i], end = starts[i + 1];
                if (first == end) { result->emplace(); continue; }
                if constexpr (numeric) {
                    if (reduce != Reduce::first) {
                        T total{};
                        for (auto s = first; s < end; ++s) total += (*column)[sources[s]];
                        result->add(reduce == Reduce::sum ? total : total / static_cast<float>(end - first));
                        continue;
                    }
                }
                result->add((*column)[sources[first]]);
            }
            return ColumnData{std::move(result)};
        }
    }, attribute->data);
    if (!promoted) return std::unexpected(promoted.error());
    const auto usage = attribute->usage;
    String moved{name};
    (void)geometry.remove(name);
    return geometry.set(std::move(moved), to, std::move(*promoted), usage);
}

Result<Geometry> copy_to_points(const Geometry& points, std::span<const Reference> references, StringView variant) {
    const auto count = points.count(Layer::point);
    if (count && references.empty()) return failure("Copy to Points needs at least one reference.");
    const auto optional = [&]<class T>(StringView name, std::type_identity<T>) { return points.read<T>(name, Layer::point); };
    auto orient = optional("orient", std::type_identity<Quaternion>{});
    auto pscale = optional("pscale", std::type_identity<float>{});
    auto scale = optional("scale", std::type_identity<Vector3>{});
    auto normal = optional("N", std::type_identity<Vector3>{});
    auto up = optional("up", std::type_identity<Vector3>{});
    auto variants = optional(variant, std::type_identity<std::uint32_t>{});
    for (const auto* error : {orient ? nullptr : &orient.error(), pscale ? nullptr : &pscale.error(), scale ? nullptr : &scale.error(),
                              normal ? nullptr : &normal.error(), up ? nullptr : &up.error(), variants ? nullptr : &variants.error()})
        if (error) return std::unexpected(*error);
    const auto& positions = points.positions();
    Array<std::uint32_t> indices;
    Array<Matrix4> transforms;
    indices.reserve(count); transforms.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::array<Vector3, 3> axes{Vector3{1, 0, 0}, Vector3{0, 1, 0}, Vector3{0, 0, 1}};
        const auto from_quaternion = [&](Quaternion q) {
            const auto r = math::rotation_matrix(q);
            for (std::size_t row = 0; row < 3; ++row) axes[row] = {r(row, 0), r(row, 1), r(row, 2)};
        };
        if (*orient) {
            const auto q = math::try_normalize((**orient)[i]);
            if (!q) return failure("Point orient must be a finite nonzero quaternion.");
            from_quaternion(*q);
        } else if (*normal) {
            const auto n = (**normal)[i];
            if (!math::is_finite(n)) return failure("Point N must be finite.");
            if (const auto z = math::try_normalize(n)) {
                const auto upward = *up ? (**up)[i] : Vector3{};
                if (!math::is_finite(upward)) return failure("Point up must be finite.");
                const auto x = math::try_normalize(math::cross(upward, *z));
                if (x) axes = {*x, math::cross(*z, *x), *z};
                else from_quaternion(dihedral(*z));
            }
        }
        Vector3 factor{1, 1, 1};
        if (*scale) factor = (**scale)[i];
        if (*pscale) factor = factor * (**pscale)[i];
        const auto position = positions[i];
        if (!math::is_finite(factor)) return failure("Point scale and pscale must be finite.");
        Matrix4 matrix = Matrix4::identity();
        for (std::size_t row = 0; row < 3; ++row)
            for (std::size_t column = 0; column < 3; ++column) matrix(row, column) = axes[row][column] * factor[row];
        matrix[12] = position.x; matrix[13] = position.y; matrix[14] = position.z;
        if (!math::is_finite(matrix)) return failure("A point's instance transform is not finite.");
        const auto picked = *variants ? (**variants)[i] : 0u;
        if (picked >= references.size()) return failure("A point's variant names a missing reference.");
        indices.add(picked);
        transforms.add(matrix);
    }
    Geometry result;
    for (const auto& attribute : points.attributes())
        if (attribute.layer == Layer::detail)
            if (auto copied = result.set(attribute.name, Layer::detail, attribute.data, attribute.usage); !copied)
                return std::unexpected(copied.error());
    if (auto set = result.set_instances({references.begin(), references.end()}, std::move(indices), std::move(transforms)); !set)
        return std::unexpected(set.error());
    return result;
}

std::optional<Bounds> bounds(const Geometry& geometry) noexcept {
    const auto& positions = geometry.positions();
    if (positions.empty()) return std::nullopt;
    Bounds result{positions[0], positions[0]};
    for (const auto& p : positions)
        for (std::size_t axis = 0; axis < 3; ++axis) {
            result.minimum[axis] = std::min(result.minimum[axis], p[axis]);
            result.maximum[axis] = std::max(result.maximum[axis], p[axis]);
        }
    return result;
}

}
