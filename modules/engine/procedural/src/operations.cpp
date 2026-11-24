#include "detail/node_plan.hpp"
#include "procedural/operations.hpp"
#include "foundation/hash.hpp"
#include "foundation/map.hpp"
#include "foundation/result.hpp"
#include "foundation/tasks.hpp"
#include "procedural/bvh.hpp"
#include "procedural/random.hpp"
#include "detail/triangles.hpp"
#include "detail/topology.hpp"

#include <bit>
#include <cmath>
#include <limits>
#include <numbers>

namespace atelier::procedural {
namespace {

using math::Matrix4;

using math::Quaternion;

using math::Vector2;

using math::Vector3;

using math::Vector3d;

template <class T>
T value(Result<T> result) {
    return std::move(*result);
}

// sign(det) * cofactor rows of the linear part, in double; a zero determinant keeps sign +1.
std::array<double, 9> normal_rows(const Matrix4& m) {
    const double a = m[0];
    const double b = m[1];
    const double c = m[2];
    const double d = m[4];
    const double e = m[5];
    const double f = m[6];
    const double g = m[8];
    const double h = m[9];
    const double i = m[10];
    std::array<double, 9> cofactors{e * i - f * h, f * g - d * i, d * h - e * g, c * h - b * i, a * i - c * g,
                                    b * g - a * h, b * f - c * e, c * d - a * f, a * e - b * d};
    if (a * cofactors[0] + b * cofactors[1] + c * cofactors[2] < 0)
        for (double& value : cofactors)
            value = -value;
    return cofactors;
}

Vector3 transform_normal(Vector3 n, const std::array<double, 9>& rows) {
    const Vector3d result{n.x * rows[0] + n.y * rows[3] + n.z * rows[6], n.x * rows[1] + n.y * rows[4] + n.z * rows[7],
                          n.x * rows[2] + n.y * rows[5] + n.z * rows[8]};
    const double length = math::length(result);
    return length == 0 ? Vector3{}
                       : Vector3(result / length); // Non-finite input stays non-finite for the caller's check.
}

// The shortest rotation taking +Z to the unit vector `z`.
Quaternion dihedral(Vector3 z) {
    const double w = 1.0 + z.z;
    if (w < 1e-7)
        return {1, 0, 0, 0}; // Opposite: half a turn about X.
    const double length = std::sqrt(z.y * double(z.y) + z.x * double(z.x) + w * w);
    return {static_cast<float>(-z.y / length), static_cast<float>(z.x / length), 0, static_cast<float>(w / length)};
}

// Unit quaternion of an orthonormal row basis. Choose the largest component so
// half turns do not divide by a vanishing trace.
Quaternion basis_rotation(const std::array<Vector3, 3>& axes) {
    const double x = axes[0].x;
    const double y = axes[1].y;
    const double z = axes[2].z;
    const std::array<double, 4> squares{1 + x - y - z, 1 - x + y - z, 1 - x - y + z, 1 + x + y + z};
    const auto largest = std::size_t(std::ranges::max_element(squares) - squares.begin());
    std::array<double, 4> q{};
    q[largest] = std::sqrt(squares[largest]) * .5;
    const double scale = .25 / q[largest];
    if (largest == 3) {
        q[0] = (axes[1].z - axes[2].y) * scale;
        q[1] = (axes[2].x - axes[0].z) * scale;
        q[2] = (axes[0].y - axes[1].x) * scale;
    } else {
        const auto i = largest;
        const auto j = (i + 1) % 3;
        const auto k = (i + 2) % 3;
        q[j] = (axes[i][j] + axes[j][i]) * scale;
        q[k] = (axes[i][k] + axes[k][i]) * scale;
        q[3] = (axes[j][k] - axes[k][j]) * scale;
    }
    return *math::try_normalize(Quaternion{static_cast<float>(q[0]), static_cast<float>(q[1]), static_cast<float>(q[2]),
                                           static_cast<float>(q[3])});
}

struct Placement {
    Matrix4 matrix;

    Quaternion rotation;

    bool reverse;
};

// Borrows one immutable point input for either resource instances or real copies.
constexpr char orientation[] = "orient", uniform_scale[] = "pscale", axis_scale[] = "scale", normal_name[] = "N",
               up_name[] = "up";

struct PlacementColumns {
    const Array<Vector3>* positions;

    const Array<Quaternion>* orient;

    const Array<float>* pscale;

    const Array<Vector3>* scale;

    const Array<Vector3>* normal;

    const Array<Vector3>* up;

    const Array<std::uint32_t>* variants;

    static Result<PlacementColumns> read(const Geometry& points, StringView variant) {
        ATELIER_TRY_BIND(orient, points.read<Quaternion>(orientation, Layer::point));
        ATELIER_TRY_BIND(pscale, points.read<float>(uniform_scale, Layer::point));
        ATELIER_TRY_BIND(scale, points.read<Vector3>(axis_scale, Layer::point));
        ATELIER_TRY_BIND(normal, points.read<Vector3>(normal_name, Layer::point));
        ATELIER_TRY_BIND(up, points.read<Vector3>(up_name, Layer::point));
        ATELIER_TRY_BIND(variants, points.read<std::uint32_t>(variant, Layer::point));
        return PlacementColumns{&points.positions(), *orient, *pscale, *scale, *normal, *up, *variants};
    }

    Result<Placement> at(std::uint32_t i) const {
        std::array<Vector3, 3> axes{Vector3{1, 0, 0}, Vector3{0, 1, 0}, Vector3{0, 0, 1}};
        Quaternion rotation;
        const auto from_quaternion = [&](Quaternion q) {
            rotation = q;
            const auto r = math::rotation_matrix(q);
            for (std::size_t row = 0; row < 3; ++row)
                axes[row] = {r(row, 0), r(row, 1), r(row, 2)};
        };
        if (orient) {
            const auto q = math::try_normalize((*orient)[i]);
            if (!q)
                return failure("Point orient must be a finite nonzero quaternion.");
            from_quaternion(*q);
        } else if (normal) {
            const auto n = (*normal)[i];
            if (!math::is_finite(n))
                return failure("Point N must be finite.");
            if (const auto z = math::try_normalize(n)) {
                const auto upward = up ? (*up)[i] : Vector3{};
                if (!math::is_finite(upward))
                    return failure("Point up must be finite.");
                const auto x = math::try_normalize(math::cross(upward, *z));
                if (x) {
                    axes = {*x, math::cross(*z, *x), *z};
                    rotation = basis_rotation(axes);
                } else
                    from_quaternion(dihedral(*z));
            }
        }
        Vector3 factor{1, 1, 1};
        if (scale)
            factor = (*scale)[i];
        if (pscale)
            factor = factor * (*pscale)[i];
        const auto position = (*positions)[i];
        if (!math::is_finite(factor))
            return failure("Point scale and pscale must be finite.");
        Matrix4 matrix = Matrix4::identity();
        for (std::size_t row = 0; row < 3; ++row)
            for (std::size_t column = 0; column < 3; ++column)
                matrix(row, column) = axes[row][column] * factor[row];
        matrix[12] = position.x;
        matrix[13] = position.y;
        matrix[14] = position.z;
        if (!math::is_finite(matrix))
            return failure("A point's instance transform is not finite.");
        return Placement{matrix, rotation, factor.x * double(factor.y) * factor.z < 0};
    }
};

Vector3 transformed(Vector3 v, Usage usage, const Matrix4& matrix, const std::array<double, 9>& normals) {
    return usage == Usage::point    ? math::transform_point(v, matrix)
           : usage == Usage::vector ? math::transform_direction(v, matrix)
           : usage == Usage::normal ? transform_normal(v, normals)
                                    : v;
}

}

Geometry import_mesh(const geometry::Mesh& mesh) {
    struct Key {
        std::uint32_t x;

        std::uint32_t y;

        std::uint32_t z;

        bool operator==(const Key&) const noexcept = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& key) const noexcept { return hash(key.x, hash(key.y, key.z)); }
    };
    const auto vertices = mesh.vertices();
    Map<Key, std::uint32_t, KeyHash> welded;
    welded.reserve(vertices.size());
    Array<Vector3> positions;
    Array<std::uint32_t> vertex_points;
    Array<std::uint32_t> offsets;
    Array<Vector3> normals;
    Array<Vector2> uvs;
    vertex_points.reserve(vertices.size());
    normals.reserve(vertices.size());
    uvs.reserve(vertices.size());
    const auto clean = [](float v) noexcept { return v == 0.0f ? 0.0f : v; };
    for (const auto& vertex : vertices) {
        const Vector3 p{clean(vertex.position[0]), clean(vertex.position[1]), clean(vertex.position[2])};
        const Key key{std::bit_cast<std::uint32_t>(p.x), std::bit_cast<std::uint32_t>(p.y),
                      std::bit_cast<std::uint32_t>(p.z)};
        const auto [value_ptr, added] = welded.try_emplace(key, static_cast<std::uint32_t>(positions.size()));
        if (added)
            positions.add(p);
        vertex_points.add(*value_ptr);
        normals.add({vertex.normal[0], vertex.normal[1], vertex.normal[2]});
        uvs.add({vertex.uv[0], vertex.uv[1]});
    }
    const auto triangles = static_cast<std::uint32_t>(vertices.size() / 3);
    offsets.reserve(triangles + 1);
    for (std::uint32_t t = 0; t <= triangles; ++t)
        offsets.add(t * 3);
    Array<std::uint32_t> material;
    material.resize(triangles);
    for (const auto& part : mesh.parts())
        for (auto t = part.first_vertex / 3; t < (part.first_vertex + part.vertex_count) / 3; ++t)
            material[t] = part.material;
    auto result = value(Geometry::polygons(std::move(positions), std::move(vertex_points), offsets));
    (void)result.set(String::literal<"N">(), Layer::vertex, SharedArray<Vector3>{std::move(normals)});
    (void)result.set(String::literal<"uv">(), Layer::vertex, SharedArray<Vector2>{std::move(uvs)});
    (void)result.set(String::literal<"material">(), Layer::primitive, SharedArray<std::uint32_t>{std::move(material)});
    return result;
}

Result<Geometry> line(Vector3 start, Vector3 end, std::uint32_t count) {
    if (count > element_limit)
        return failure("A line has fewer than 2^32 - 1 points.");
    Array<Vector3> positions;
    positions.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const double t = count < 2 ? 0.0 : double(i) / (count - 1);
        positions.add(Vector3(Vector3d(start) + (Vector3d(end) - Vector3d(start)) * t));
    }
    return Geometry::points(std::move(positions));
}

Result<void> connectivity(Geometry& geometry, StringView attribute) {
    ATELIER_TRY(Geometry::admit_name(attribute));
    ATELIER_TRY(geometry.read<std::uint32_t>(attribute, Layer::point));
    // Replacing the whole column avoids copying its old contents from a shared input.
    return geometry.set(String::from_utf8_validated(attribute), Layer::point,
                        SharedArray<std::uint32_t>{detail::point_pieces(geometry)});
}

Result<Geometry> scatter(const Geometry& surface, const ScatterSettings& settings) {
    if (!std::isfinite(settings.density) || settings.density < 0 || !std::isfinite(settings.min_distance) ||
        settings.min_distance < 0)
        return failure("Scatter density and minimum distance must be finite and non-negative.");
    const Array<bool>* selected = nullptr;
    if (!settings.group.empty()) {
        ATELIER_TRY_BIND(mask, surface.read<bool>(settings.group, Layer::primitive));
        if (!*mask)
            return failure("Scatter primitive group " + std::string{settings.group.std_view()} + " was not found.");
        selected = *mask;
    }
    const auto triangles = detail::TriangleView::create(surface);
    if (!triangles)
        return std::unexpected(triangles.error());
    const auto& points = surface.positions();
    const auto vertex_points = surface.vertex_points();
    // Only prefix counts survive preparation, including zero-count triangles.
    // No per-triangle positions, edges, normals or seeds are cached.
    Array<std::uint32_t> point_offsets;
    Array<std::uint32_t> triangle_offsets;
    point_offsets.reserve(static_cast<std::size_t>(triangles->size()) + 1);
    triangle_offsets.reserve(static_cast<std::size_t>(surface.count(Layer::primitive)) + 1);
    point_offsets.add(0);
    for (const auto& [p, j, corners] : *triangles) {
        if (j == 0)
            triangle_offsets.add(static_cast<std::uint32_t>(point_offsets.size() - 1));
        auto end = point_offsets[point_offsets.size() - 1];
        if (selected && !(*selected)[p]) {
            // Keep the original fan index and primitive numbering without generating candidates.
            point_offsets.add(end);
            continue;
        }
        const auto v0 = Vector3d(points[vertex_points[corners[0]]]);
        const auto e1 = Vector3d(points[vertex_points[corners[1]]]) - v0;
        const auto e2 = Vector3d(points[vertex_points[corners[2]]]) - v0;
        const auto normal = math::cross(e1, e2);
        const double area = math::length(normal) / 2;
        const double expected = area * settings.density;
        if (area > 0 && expected > 0) {
            const auto seed = j ? hash(hash(settings.seed, p), j) : hash(settings.seed, p);
            const double whole = std::floor(expected);
            const auto count = whole + (unit(hash(seed, 0u)) < expected - whole ? 1 : 0);
            if (end + count > element_limit)
                return failure("Scatter would create 2^32 - 1 points or more.");
            end += static_cast<std::uint32_t>(count);
        }
        point_offsets.add(end);
    }
    triangle_offsets.add(triangles->size());
    const auto total = point_offsets[point_offsets.size() - 1];

    // Fix all output storage before dispatch. Workers only borrow plain spans.
    Array<Vector3> positions;
    Array<Vector3> normals;
    Array<std::uint32_t> ids;
    Array<std::uint32_t> primitives;
    Array<std::uint32_t> subtriangles;
    Array<std::uint32_t> ordinals;
    Array<Vector2> barycentrics;
    positions.resize(total);
    normals.resize(total);
    ids.resize(total);
    primitives.resize(total);
    if (triangles->has_subtriangles())
        subtriangles.resize(total);
    ordinals.resize(total);
    barycentrics.resize(total);
    const auto filled = tasks::parallel_for(
        {0, total},
        [point_offsets = Span<const std::uint32_t>{point_offsets.view()},
         triangle_offsets = Span<const std::uint32_t>{triangle_offsets.view()}, points = points.view(), vertex_points,
         seed = settings.seed, positions = positions.view(), normals = normals.view(), ids = ids.view(),
         primitives = primitives.view(), subtriangles = subtriangles.view(), ordinals = ordinals.view(),
         barycentrics = barycentrics.view()](tasks::Range range) noexcept -> std::expected<void, tasks::Failure> {
            // Split output ranges so a single large face can still use the pool.
            auto t = static_cast<std::uint32_t>(
                std::upper_bound(point_offsets.begin(), point_offsets.end(), range.begin) - point_offsets.begin() - 1);
            auto p = static_cast<std::uint32_t>(std::upper_bound(triangle_offsets.begin(), triangle_offsets.end(), t) -
                                                triangle_offsets.begin() - 1);
            auto i = range.begin;
            while (i < range.end) {
                if (point_offsets[t + 1] == i) {
                    ++t;
                    continue;
                }
                while (t >= triangle_offsets[p + 1])
                    ++p;
                const auto j = t - triangle_offsets[p];
                // A contiguous n-corner fan contributes n-2 triangles. Thus p's
                // first original corner is its triangle prefix plus 2*p.
                const auto first = triangle_offsets[p] + 2 * p;
                const auto v0 = Vector3d(points[vertex_points[first]]);
                const auto e1 = Vector3d(points[vertex_points[first + j + 1]]) - v0;
                const auto e2 = Vector3d(points[vertex_points[first + j + 2]]) - v0;
                const auto normal = math::cross(e1, e2);
                const double area = math::length(normal) / 2;
                const auto face = Vector3(normal / (area * 2));
                const auto s = j ? hash(hash(seed, p), j) : hash(seed, p);
                const auto source = j ? hash(p, j) : p;
                const auto end = std::min(range.end, static_cast<std::size_t>(point_offsets[t + 1]));
                for (; i < end; ++i) {
                    const auto k = static_cast<std::uint32_t>(i - point_offsets[t]);
                    float r1 = unit(hash(s, 2 * k + 1));
                    float r2 = unit(hash(s, 2 * k + 2));
                    if (r1 + r2 > 1) {
                        r1 = 1 - r1;
                        r2 = 1 - r2;
                    }
                    positions[i] = Vector3(v0 + e1 * double(r1) + e2 * double(r2));
                    normals[i] = face;
                    ids[i] = hash(source, k);
                    primitives[i] = p;
                    if (!subtriangles.empty())
                        subtriangles[i] = j;
                    ordinals[i] = k;
                    barycentrics[i] = {r1, r2};
                }
                ++t;
            }
            return {};
        },
        1024);
    if (filled != tasks::Completion::completed)
        return failure("Scatter was cancelled before all candidates were filled.");
    if (settings.min_distance > 0 && !positions.empty()) {
        // Cells as wide as the distance: a kept point closer than it lies in a neighbouring cell.
        const double cell = settings.min_distance;
        const double limit = cell * cell;
        struct Cell {
            std::array<std::int64_t, 3> coord{};

            std::uint32_t head{UINT32_MAX};
        };
        const std::size_t max_table_size = std::bit_ceil(std::max<std::size_t>(16, positions.size() * 2));
        std::size_t table_size = std::min<std::size_t>(max_table_size, 1024);
        std::size_t mask = table_size - 1;
        Array<std::uint32_t> directory;
        directory.resize(table_size, UINT32_MAX);
        Array<Cell> cells;
        Array<std::uint32_t> next_in_cell;
        next_in_cell.resize(positions.size(), UINT32_MAX);
        std::array<std::int64_t, 3> min_coord{INT64_MAX, INT64_MAX, INT64_MAX};
        std::array<std::int64_t, 3> max_coord{INT64_MIN, INT64_MIN, INT64_MIN};
        const auto cell_hash = [](const std::array<std::int64_t, 3>& c) noexcept -> std::size_t {
            return Hash<StringView>{}({reinterpret_cast<const char*>(c.data()), c.size() * sizeof(c[0])});
        };
        const auto find_head = [&](const std::array<std::int64_t, 3>& c) noexcept -> std::uint32_t {
            for (std::size_t slot = cell_hash(c) & mask;; slot = (slot + 1) & mask) {
                const std::uint32_t cell_index = directory[slot];
                if (cell_index == UINT32_MAX)
                    return UINT32_MAX;
                if (cells[cell_index].coord == c)
                    return cells[cell_index].head;
            }
        };
        const auto insert_point = [&](const std::array<std::int64_t, 3>& c, std::uint32_t index) {
            if (cells.size() * 2 >= table_size) {
                table_size *= 2;
                mask = table_size - 1;
                directory.clear();
                directory.resize(table_size, UINT32_MAX);
                for (std::uint32_t ci = 0; ci < cells.size(); ++ci) {
                    for (std::size_t slot = cell_hash(cells[ci].coord) & mask;; slot = (slot + 1) & mask)
                        if (directory[slot] == UINT32_MAX) {
                            directory[slot] = ci;
                            break;
                        }
                }
            }
            for (std::size_t axis = 0; axis < 3; ++axis) {
                min_coord[axis] = std::min(min_coord[axis], c[axis]);
                max_coord[axis] = std::max(max_coord[axis], c[axis]);
            }
            for (std::size_t slot = cell_hash(c) & mask;; slot = (slot + 1) & mask) {
                const std::uint32_t cell_index = directory[slot];
                if (cell_index == UINT32_MAX) {
                    directory[slot] = static_cast<std::uint32_t>(cells.size());
                    cells.add({c, index});
                    return;
                }
                if (cells[cell_index].coord == c) {
                    next_in_cell[index] = cells[cell_index].head;
                    cells[cell_index].head = index;
                    return;
                }
            }
        };
        Array<std::uint32_t> kept;
        for (std::uint32_t i = 0; i < positions.size(); ++i) {
            const auto p = Vector3d(positions[i]);
            std::array<std::int64_t, 3> c;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                // Leaves room for the neighbour offsets below.
                const double index = std::floor(p[axis] / cell);
                if (!(std::abs(index) < 0x1p62))
                    return failure("Scatter minimum distance is too small for the point coordinates.");
                c[axis] = static_cast<std::int64_t>(index);
            }
            bool near = false;
            if (!cells.empty()) {
                const auto check_cell = [&](const std::array<std::int64_t, 3>& nc) noexcept {
                    for (auto other = find_head(nc); other != UINT32_MAX; other = next_in_cell[other]) {
                        const auto d = Vector3d(positions[other]) - p;
                        if (math::length_squared(d) < limit) {
                            near = true;
                            return;
                        }
                    }
                };
                if (c[0] >= min_coord[0] && c[0] <= max_coord[0] && c[1] >= min_coord[1] && c[1] <= max_coord[1] &&
                    c[2] >= min_coord[2] && c[2] <= max_coord[2])
                    check_cell(c);
                const std::int64_t x0 = std::max(c[0] - 1, min_coord[0]);
                const std::int64_t x1 = std::min(c[0] + 1, max_coord[0]);
                const std::int64_t y0 = std::max(c[1] - 1, min_coord[1]);
                const std::int64_t y1 = std::min(c[1] + 1, max_coord[1]);
                const std::int64_t z0 = std::max(c[2] - 1, min_coord[2]);
                const std::int64_t z1 = std::min(c[2] + 1, max_coord[2]);
                for (std::int64_t x = x0; x <= x1 && !near; ++x)
                    for (std::int64_t y = y0; y <= y1 && !near; ++y)
                        for (std::int64_t z = z0; z <= z1 && !near; ++z) {
                            if (x == c[0] && y == c[1] && z == c[2])
                                continue;
                            check_cell({x, y, z});
                        }
            }
            if (near)
                continue;
            insert_point(c, i);
            kept.add(i);
        }
        const auto keep = [&](auto& column) {
            std::remove_cvref_t<decltype(column)> result;
            result.reserve(kept.size());
            for (const auto i : kept)
                result.add(column[i]);
            column = std::move(result);
        };
        keep(positions);
        keep(normals);
        keep(ids);
        keep(primitives);
        if (triangles->has_subtriangles())
            keep(subtriangles);
        keep(ordinals);
        keep(barycentrics);
    }
    auto result = Geometry::points(std::move(positions));
    if (!result)
        return result;
    for (auto added :
         {result->set(String::literal<"N">(), Layer::point, SharedArray<Vector3>{std::move(normals)}),
          result->set(String::literal<"id">(), Layer::point, SharedArray<std::uint32_t>{std::move(ids)}),
          result->set(String::literal<"sourceprim">(), Layer::point, SharedArray<std::uint32_t>{std::move(primitives)}),
          result->set(String::literal<"sourceordinal">(), Layer::point,
                      SharedArray<std::uint32_t>{std::move(ordinals)}),
          result->set(String::literal<"sourceprimuv">(), Layer::point, SharedArray<Vector2>{std::move(barycentrics)})})
        if (!added)
            return std::unexpected(added.error());
    if (triangles->has_subtriangles())
        ATELIER_TRY(result->set(String::literal<"sourcetri">(), Layer::point,
                                SharedArray<std::uint32_t>{std::move(subtriangles)}));
    return result;
}

// The native placement contract covers named bindings. Bulk propagation of source geometry
// and Realize's explicit target_attributes list are separate from these conventional names.
void detail::append_native_rules(std::vector<AttributeRule>& rules, std::string_view type) {
    using enum AttributeAccess;
    if (type == "procedural.Scatter") {
        rules = {{"surface", "P", {}, "point", {}, "vector3", read, "error", "Surface positions"},
                 {"surface",
                  {},
                  "group",
                  "primitive",
                  {},
                  "bool",
                  group,
                  "empty selector selects all; named group required",
                  "Eligible faces"},
                 {"points", "P", {}, "point", {}, "vector3", write, "create", "Sample position"},
                 {"points", normal_name, {}, "point", {}, "vector3", write, "create", "Geometric face normal"},
                 {"points", "id", {}, "point", {}, "uint", write, "create", "Deterministic seed, may repeat"},
                 {"points", "sourceprim", {}, "point", {}, "uint", write, "create", "Source face index"},
                 {"points",
                  "sourceordinal",
                  {},
                  "point",
                  {},
                  "uint",
                  write,
                  "create",
                  "Candidate ordinal within the sampling triangle"},
                 {"points",
                  "sourceprimuv",
                  {},
                  "point",
                  {},
                  "vector2",
                  write,
                  "create",
                  "Barycentric coordinates in the sampling triangle"},
                 {"points",
                  "sourcetri",
                  {},
                  "point",
                  {},
                  "uint",
                  write,
                  "conditional",
                  "Fan triangle index when the input has a polygon with more than three corners"}};
    } else if (type == "procedural.Realize" || type == "procedural.CopyToPoints") {
        rules = {
            {"points", "P", {}, "point", {}, "vector3", read, "error", "Translation"},
            {"points", orientation, {}, "point", {}, "quaternion", read, "use N, then identity", "Rotation"},
            {"points",
             normal_name,
             {},
             "point",
             {},
             "vector3",
             read,
             "identity",
             "Align local +Z; zero normal keeps identity",
             orientation},
            {"points",
             up_name,
             {},
             "point",
             {},
             "vector3",
             read,
             "shortest rotation from +Z",
             "Roll reference; parallel or zero up uses shortest rotation",
             orientation,
             normal_name},
            {"points", uniform_scale, {}, "point", {}, "float", read, "1", "Uniform factor multiplied by scale"},
            {"points", axis_scale, {}, "point", {}, "vector3", read, "(1,1,1)", "Axis factors multiplied by pscale"},
            {"points", {}, "variant", "point", {}, "uint", read, "source index 0", "Select ordered source"}};
        if (type == "procedural.Realize")
            rules.push_back({"points",
                             {},
                             "group",
                             "point",
                             {},
                             "bool",
                             group,
                             "empty selector selects all; named group required",
                             "Eligible target points"});
    }
}

Quaternion euler_to_quaternion(Vector3 euler_degrees) noexcept {
    constexpr double radians = std::numbers::pi / 180.0;
    const auto q = math::from_axis_angle(Vector3d{0.0, 0.0, 1.0}, double{euler_degrees.z} * radians) *
                   math::from_axis_angle(Vector3d{0.0, 1.0, 0.0}, double{euler_degrees.y} * radians) *
                   math::from_axis_angle(Vector3d{1.0, 0.0, 0.0}, double{euler_degrees.x} * radians);
    return Quaternion(q);
}

Result<void> transform(Geometry& geometry, const TransformSettings& settings) {
    if (!math::is_finite(settings.translate) || !math::is_finite(settings.rotate_degrees) ||
        !math::is_finite(settings.scale) || !math::is_finite(settings.pivot))
        return failure("Transform translate, rotate, scale and pivot must be finite.");
    const Quaternion q = euler_to_quaternion(settings.rotate_degrees);
    Vector3 effective_translate = settings.translate;
    if (settings.pivot != Vector3{}) {
        const auto unit_q = math::try_normalize(q);
        if (!unit_q)
            return failure("Transform rotation is degenerate.");
        const Matrix4 linear = math::trs_matrix({}, *unit_q, settings.scale);
        const Vector3 pivoted = math::transform_direction(settings.pivot, linear);
        const Vector3d delta = Vector3d(settings.translate) + Vector3d(settings.pivot) - Vector3d(pivoted);
        effective_translate = Vector3(delta);
        if (!math::is_finite(effective_translate))
            return failure("Transform pivot offset overflowed float range.");
    }
    return transform(geometry, effective_translate, q, settings.scale);
}

Result<void> transform(Geometry& geometry, Vector3 translate, Quaternion rotation, Vector3 scale) {
    const auto unit_rotation = math::try_normalize(rotation);
    if (!math::is_finite(translate) || !math::is_finite(scale) || !unit_rotation)
        return failure("Transform needs a finite translation and scale and a nonzero rotation.");
    const auto matrix = math::trs_matrix(translate, *unit_rotation, scale);
    const auto normals = normal_rows(matrix);
    for (const auto& attribute : geometry.attributes()) {
        const auto name = attribute.name.view();
        if (std::holds_alternative<SharedArray<Quaternion>>(attribute.data) && attribute.usage == Usage::orientation) {
            auto& values = *value(geometry.write<Quaternion>(name, attribute.layer));
            for (auto& q : values) {
                q = *unit_rotation * q;
                if (!math::is_finite(q))
                    return failure("Transformed " + std::string{name.std_view()} + " is not finite.");
            }
            continue;
        }
        if (!std::holds_alternative<SharedArray<Vector3>>(attribute.data))
            continue;
        const auto usage = attribute.usage;
        if (usage != Usage::point && usage != Usage::vector && usage != Usage::normal)
            continue;
        auto& values = *value(geometry.write<Vector3>(name, attribute.layer));
        for (auto& v : values) {
            v = transformed(v, usage, matrix, normals);
            if (!math::is_finite(v))
                return failure("Transformed " + std::string{name.std_view()} + " is not finite.");
        }
    }
    if (geometry.count(Layer::instance))
        for (auto& instance : geometry.write_instance_transforms()) {
            instance = instance * matrix;
            if (!math::is_finite(instance))
                return failure("Transformed instances are not finite.");
        }
    if (scale.x * double(scale.y) * scale.z < 0)
        geometry.reverse_primitives(true);
    return {};
}

Result<Geometry> match_size(const Geometry& geometry, const MatchSizeSettings& settings, const Geometry* reference) {
    if (!math::is_finite(settings.target_position) || !math::is_finite(settings.target_size))
        return failure("MatchSize target_position and target_size must be finite.");
    const auto ref_bounds = reference ? bounds(*reference) : std::nullopt;
    if (settings.scale_to_fit && !ref_bounds &&
        (settings.target_size.x <= 0.f || settings.target_size.y <= 0.f || settings.target_size.z <= 0.f))
        return failure("MatchSize target_size must be positive when scale_to_fit is enabled.");

    const auto src_bounds = bounds(geometry);
    if (!src_bounds)
        return geometry;

    const Vector3d src_min(src_bounds->minimum);
    const Vector3d src_max(src_bounds->maximum);
    const Vector3d src_center = (src_min + src_max) * 0.5;
    const Vector3d src_ext = src_max - src_min;

    const Vector3d goal_size =
        ref_bounds ? (Vector3d(ref_bounds->maximum) - Vector3d(ref_bounds->minimum)) : Vector3d(settings.target_size);

    Vector3d scale_vec{1.0, 1.0, 1.0};
    if (settings.scale_to_fit) {
        if (settings.uniform_scale) {
            double best = 0.0;
            bool any_axis = false;
            for (std::size_t a = 0; a < 3; ++a) {
                if (src_ext[a] > 0.0 && goal_size[a] > 0.0) {
                    const double ratio = goal_size[a] / src_ext[a];
                    best = any_axis ? std::min(best, ratio) : ratio;
                    any_axis = true;
                }
            }
            if (any_axis)
                scale_vec = {best, best, best};
        } else {
            for (std::size_t a = 0; a < 3; ++a) {
                if (src_ext[a] > 0.0 && goal_size[a] > 0.0)
                    scale_vec[a] = goal_size[a] / src_ext[a];
            }
        }
    }

    const Vector3d anchor_min = ref_bounds ? Vector3d(ref_bounds->minimum) : Vector3d(settings.target_position);
    const Vector3d anchor_center = ref_bounds ? ((Vector3d(ref_bounds->minimum) + Vector3d(ref_bounds->maximum)) * 0.5)
                                              : Vector3d(settings.target_position);
    const Vector3d anchor_max = ref_bounds ? Vector3d(ref_bounds->maximum) : Vector3d(settings.target_position);
    const std::array<AxisJustify, 3> justifies{settings.justify_x, settings.justify_y, settings.justify_z};

    Vector3d eff_translate{};
    for (std::size_t a = 0; a < 3; ++a) {
        const double scaled_min = src_center[a] + (src_min[a] - src_center[a]) * scale_vec[a];
        const double scaled_max = src_center[a] + (src_max[a] - src_center[a]) * scale_vec[a];
        double shift = 0.0;
        switch (justifies[a]) {
        case AxisJustify::none:
            shift = 0.0;
            break;
        case AxisJustify::min:
            shift = anchor_min[a] - scaled_min;
            break;
        case AxisJustify::center:
            shift = anchor_center[a] - src_center[a];
            break;
        case AxisJustify::max:
            shift = anchor_max[a] - scaled_max;
            break;
        default:
            return failure("Invalid AxisJustify value.");
        }
        eff_translate[a] = src_center[a] * (1.0 - scale_vec[a]) + shift;
    }

    Geometry out = geometry;
    ATELIER_TRY(transform(out, Vector3(eff_translate), Quaternion{}, Vector3(scale_vec)));
    return out;
}

Result<void> promote(Geometry& geometry, StringView name, Layer to, Reduce reduce) {
    const auto* attribute = geometry.find(name);
    if (!attribute)
        return failure("Geometry has no attribute " + std::string{name.std_view()} + ".");
    const auto from = attribute->layer;
    if (from == to)
        return {};
    if (name == "P")
        return failure("P stays on points.");
    if ((from == Layer::instance && to != Layer::detail) || (to == Layer::instance && from != Layer::detail))
        return failure("Instance attributes only exchange with detail.");
    const auto vertex_points = geometry.vertex_points();
    const auto primitives = geometry.count(Layer::primitive);
    const auto targets = geometry.count(to);
    Array<std::uint32_t> starts;
    Array<std::uint32_t> sources;
    if (from != Layer::detail && to != Layer::detail) {
        starts.add(0);
        const auto close = [&] { starts.add(static_cast<std::uint32_t>(sources.size())); };
        if (to == Layer::primitive) {
            for (std::uint32_t p = 0; p < primitives; ++p) {
                const auto range = geometry.primitive(p);
                for (auto v = range.first; v < range.first + range.count; ++v)
                    sources.add(from == Layer::vertex ? v : vertex_points[v]);
                close();
            }
        } else if (to == Layer::vertex) {
            for (std::uint32_t p = 0; p < primitives; ++p) {
                const auto range = geometry.primitive(p);
                for (auto v = range.first; v < range.first + range.count; ++v) {
                    sources.add(from == Layer::point ? vertex_points[v] : p);
                    close();
                }
            }
        } else { // To points from vertices or primitives: bucket the vertices by point.
            Array<std::uint32_t> counts;
            Array<std::uint32_t> vertex_primitive;
            counts.resize(targets + 1);
            vertex_primitive.resize(vertex_points.size());
            for (std::uint32_t p = 0; p < primitives; ++p) {
                const auto range = geometry.primitive(p);
                for (auto v = range.first; v < range.first + range.count; ++v)
                    vertex_primitive[v] = p;
            }
            for (const auto point : vertex_points)
                ++counts[point + 1];
            for (std::uint32_t i = 0; i < targets; ++i)
                counts[i + 1] += counts[i];
            starts = counts;
            sources.resize(vertex_points.size());
            for (std::uint32_t v = 0; v < vertex_points.size(); ++v)
                sources[counts[vertex_points[v]]++] = from == Layer::vertex ? v : vertex_primitive[v];
        }
    }
    auto promoted = std::visit(
        [&](const auto& column) -> Result<ColumnData> {
            using T = typename std::remove_cvref_t<decltype(column)>::Value;
            constexpr bool numeric = std::same_as<T, float> || std::same_as<T, Vector2> || std::same_as<T, Vector3> ||
                                     std::same_as<T, math::Vector4>;
            if constexpr (std::same_as<T, String>)
                return failure("String attributes stay on detail.");
            else {
                if (reduce != Reduce::first && !numeric)
                    return failure("Sum and average need float or float vector attributes.");
                Array<T> result;
                if (from == Layer::detail) {
                    result.resize(targets, column.array()[0]);
                    return ColumnData{SharedArray<T>{std::move(result)}};
                }
                if (to == Layer::detail) {
                    const auto& items = column.array();
                    if (items.empty()) {
                        result.emplace();
                    } else if constexpr (numeric) {
                        if (reduce != Reduce::first) {
                            T total{};
                            for (const auto& item : items)
                                total += item;
                            result.add(reduce == Reduce::sum ? total : total / static_cast<float>(items.size()));
                        } else {
                            result.add(items[0]);
                        }
                    } else {
                        result.add(items[0]);
                    }
                    return ColumnData{SharedArray<T>{std::move(result)}};
                }
                result.reserve(targets);
                for (std::uint32_t i = 0; i < targets; ++i) {
                    const auto first = starts[i];
                    const auto end = starts[i + 1];
                    if (first == end) {
                        result.emplace();
                        continue;
                    }
                    if constexpr (numeric) {
                        if (reduce != Reduce::first) {
                            T total{};
                            for (auto s = first; s < end; ++s)
                                total += column.array()[sources[s]];
                            result.add(reduce == Reduce::sum ? total : total / static_cast<float>(end - first));
                            continue;
                        }
                    }
                    result.add(column.array()[sources[first]]);
                }
                return ColumnData{SharedArray<T>{std::move(result)}};
            }
        },
        attribute->data);
    if (!promoted)
        return std::unexpected(promoted.error());
    const auto usage = attribute->usage;
    String moved{attribute->name};
    (void)geometry.remove(name);
    return geometry.set(std::move(moved), to, std::move(*promoted), usage);
}

Result<Geometry> ray(const Geometry& points, const Geometry& surface, const RaySettings& settings) {
    if (!std::isfinite(settings.max_distance) || settings.max_distance < 0)
        return failure("Ray max distance must be finite and non-negative.");
    const auto unit = math::try_normalize(settings.direction);
    if (settings.mode == RayMode::project && !unit)
        return failure("Ray direction must be finite and nonzero.");
    if (surface.count(Layer::primitive) > std::uint32_t(std::numeric_limits<std::int32_t>::max()))
        return failure("Ray surfaces have fewer than 2^31 primitives.");
    if (surface.count(Layer::vertex) > std::uint32_t(std::numeric_limits<std::int32_t>::max()) + 2u)
        for (std::uint32_t p = 0; p < surface.count(Layer::primitive); ++p)
            if (surface.primitive(p).count - 2 > std::uint32_t(std::numeric_limits<std::int32_t>::max()))
                return failure("Ray polygons have fewer than 2^31 fan triangles.");
    ATELIER_TRY_BIND(bvh, Bvh::acquire(surface));
    Geometry output = points;
    const auto count = output.count(Layer::point);
    // Finish adding columns before borrowing them: growing attributes moves their handles.
    ATELIER_TRY(output.add<std::int32_t>("hitprim", Layer::point));
    ATELIER_TRY(output.add<std::int32_t>("hittri", Layer::point));
    ATELIER_TRY(output.add<Vector2>("hitprimuv", Layer::point));
    ATELIER_TRY(output.add<float>("hitdist", Layer::point));
    if (settings.normal)
        ATELIER_TRY(output.add<Vector3>("N", Layer::point));
    ATELIER_TRY_BIND(positions, output.write<Vector3>("P", Layer::point));
    ATELIER_TRY_BIND(primitives, output.write<std::int32_t>("hitprim", Layer::point));
    ATELIER_TRY_BIND(subtriangles, output.write<std::int32_t>("hittri", Layer::point));
    ATELIER_TRY_BIND(uvs, output.write<Vector2>("hitprimuv", Layer::point));
    ATELIER_TRY_BIND(distances, output.write<float>("hitdist", Layer::point));
    Array<Vector3>* normals{};
    if (settings.normal) {
        ATELIER_TRY_BIND(writable, output.write<Vector3>("N", Layer::point));
        normals = *writable;
    }
    Array<bool> missed;
    missed.resize(settings.remove_misses ? count : 0);
    const auto& corners = surface.positions();
    const auto vertices = surface.vertex_points();
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto hit = settings.mode == RayMode::project ? (*bvh)->ray((**positions)[i], *unit, settings.max_distance)
                                                           : (*bvh)->closest((**positions)[i], settings.max_distance);
        if (!hit) {
            (**primitives)[i] = -1;
            (**subtriangles)[i] = -1;
            (**uvs)[i] = {};
            (**distances)[i] = -1;
            if (settings.remove_misses)
                missed[i] = true;
            continue;
        }
        (**positions)[i] = hit->position;
        (**primitives)[i] = static_cast<std::int32_t>(hit->primitive);
        (**subtriangles)[i] = static_cast<std::int32_t>(hit->triangle);
        (**uvs)[i] = hit->uv;
        (**distances)[i] = hit->distance;
        if (normals) {
            const auto indices = detail::fan_corners(surface.primitive(hit->primitive), hit->triangle);
            const auto a = corners[vertices[indices[0]]];
            const auto b = corners[vertices[indices[1]]];
            const auto c = corners[vertices[indices[2]]];
            if (const auto n = math::try_normalize(math::cross(b - a, c - a)))
                (*normals)[i] = *n;
        }
    }
    if (settings.remove_misses)
        return output.remove_flagged(Layer::point, std::span<const bool>{missed.data(), missed.size()});
    return output;
}

Result<Geometry> copy_to_points(const Geometry& points, std::span<const Reference> references, StringView variant) {
    const auto count = points.count(Layer::point);
    if (count && references.empty())
        return failure("Copy to Points needs at least one reference.");
    ATELIER_TRY_BIND(columns, PlacementColumns::read(points, variant));
    Array<std::uint32_t> indices;
    Array<Matrix4> transforms;
    indices.reserve(count);
    transforms.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        ATELIER_TRY_BIND(placement, columns->at(i));
        const auto picked = columns->variants ? (*columns->variants)[i] : 0u;
        if (picked >= references.size())
            return failure("A point's variant names a missing reference.");
        indices.add(picked);
        transforms.add(placement->matrix);
    }
    Geometry result;
    for (const auto& attribute : points.attributes())
        if (attribute.layer == Layer::detail)
            if (auto copied = result.set(attribute.name, Layer::detail, attribute.data, attribute.usage); !copied)
                return std::unexpected(copied.error());
    if (auto set =
            result.set_instances({references.begin(), references.end()}, std::move(indices), std::move(transforms));
        !set)
        return std::unexpected(set.error());
    return result;
}

Result<Geometry> realize(std::span<const Geometry> sources, const Geometry& points, StringView group,
                         StringView variant, std::span<const String> target_attributes) {
    for (const auto& source : sources)
        if (source.count(Layer::instance))
            return failure("Realize sources cannot contain instances.");
    const Array<bool>* selected = nullptr;
    if (!group.empty()) {
        ATELIER_TRY_BIND(flags, points.read<bool>(group, Layer::point));
        if (!*flags)
            return failure("Realize target group is missing.");
        selected = *flags;
    }
    ATELIER_TRY_BIND(columns, PlacementColumns::read(points, variant));
    struct Copy {
        std::uint32_t source;

        std::uint32_t target;

        Placement placement;

        std::array<double, 9> normals;
    };
    std::vector<Copy> copies;
    copies.reserve(selected ? static_cast<std::size_t>(std::ranges::count(*selected, true))
                            : points.count(Layer::point));
    std::vector<bool> used(sources.size());
    constexpr std::array layers{Layer::point, Layer::vertex, Layer::primitive};
    std::array<std::uint64_t, 3> totals{};
    for (std::uint32_t i = 0; i < points.count(Layer::point); ++i) {
        if (selected && !(*selected)[i])
            continue;
        const auto picked = columns->variants ? (*columns->variants)[i] : 0u;
        if (picked >= sources.size())
            return failure("A Realize target's variant names a missing source.");
        const auto& source = sources[picked];
        for (std::size_t layer = 0; layer < layers.size(); ++layer) {
            totals[layer] += source.count(layers[layer]);
            if (totals[layer] > element_limit)
                return failure("Realize needs fewer than 2^32 - 1 elements per layer.");
        }
        ATELIER_TRY_BIND(placement, columns->at(i));
        copies.push_back({picked, i, *placement, normal_rows(placement->matrix)});
        used[picked] = true;
    }

    // Schema and first detail values follow source-port order, not target encounter
    // order. Only the inputs are borrowed; output growth cannot invalidate these.
    struct Column {
        const Attribute* first;

        const Attribute* broadcast{};
    };
    std::vector<Column> schema;
    const auto admit = [&](const Attribute& attribute, bool broadcast) -> Result<void> {
        auto found = std::ranges::find_if(schema, [&](const Column& c) { return c.first->name == attribute.name; });
        if (found == schema.end()) {
            schema.push_back({&attribute, broadcast ? &attribute : nullptr});
            return {};
        }
        const auto& previous = *found->first;
        if (previous.layer != attribute.layer || previous.usage != attribute.usage ||
            previous.data.index() != attribute.data.index())
            return failure("Realize attribute " + std::string{attribute.name.view().std_view()} +
                           " differs in layer, type or usage.");
        if (broadcast)
            found->broadcast = &attribute;
        return {};
    };
    for (std::size_t s = 0; s < sources.size(); ++s)
        if (used[s])
            for (const auto& attribute : sources[s].attributes())
                ATELIER_TRY(admit(attribute, false));
    for (const auto& name : target_attributes) {
        const auto* attribute = points.find(name);
        if (name == "P" || !attribute || attribute->layer != Layer::point)
            return failure("Realize broadcasts must name existing target point columns other than P.");
        ATELIER_TRY(admit(*attribute, true));
    }
    for (const auto& attribute : points.attributes())
        if (attribute.layer == Layer::detail)
            ATELIER_TRY(admit(attribute, false));

    Array<Vector3> positions;
    Array<std::uint32_t> vertex_points;
    Array<std::uint32_t> offsets;
    Array<PrimitiveKind> kinds;
    positions.reserve(totals[0]);
    vertex_points.reserve(totals[1]);
    offsets.reserve(totals[2] + 1);
    kinds.reserve(totals[2]);
    offsets.add(0);
    for (const auto& copy : copies) {
        const auto& source = sources[copy.source];
        const auto base = static_cast<std::uint32_t>(positions.size());
        const auto usage = source.attributes().front().usage; // Geometry always stores P first.
        for (const auto p : source.positions())
            positions.add(transformed(p, usage, copy.placement.matrix, copy.normals));
        const auto corners = source.vertex_points();
        for (std::uint32_t p = 0; p < source.count(Layer::primitive); ++p) {
            const auto range = source.primitive(p);
            const bool reverse = copy.placement.reverse && range.kind == PrimitiveKind::polygon;
            for (std::uint32_t c = 0; c < range.count; ++c)
                vertex_points.add(base + corners[range.first + (reverse ? range.count - 1 - c : c)]);
            offsets.add(static_cast<std::uint32_t>(vertex_points.size()));
            kinds.add(range.kind);
        }
    }
    ATELIER_TRY_BIND(result,
                     Geometry::primitives(std::move(positions), std::move(vertex_points), offsets, std::move(kinds)));
    std::vector<const Attribute*> inputs;
    for (const auto& column : schema) {
        const auto& attribute = *column.first;
        if (attribute.name == "P") {
            if (attribute.usage != Usage::point)
                ATELIER_TRY(result->set(attribute.name, attribute.layer, result->find("P")->data, attribute.usage));
            continue;
        }
        if (attribute.layer == Layer::detail) {
            ATELIER_TRY(result->set(attribute.name, attribute.layer, attribute.data, attribute.usage));
            continue;
        }
        // Inputs stay immutable. Resolve each used source once per column, not
        // once per copy, and reuse the lookup storage across output columns.
        if (!column.broadcast) {
            inputs.resize(sources.size());
            for (std::size_t s = 0; s < sources.size(); ++s)
                if (used[s])
                    inputs[s] = sources[s].find(attribute.name);
        }
        auto data = std::visit(
            [&](const auto& prototype) -> Result<ColumnData> {
                using T = typename std::remove_cvref_t<decltype(prototype)>::Value;
                Array<T> values;
                values.reserve(result->count(attribute.layer));
                for (const auto& copy : copies) {
                    const auto& source = sources[copy.source];
                    const auto count = source.count(attribute.layer);
                    if (column.broadcast) {
                        const auto& target = std::get<SharedArray<T>>(column.broadcast->data).array();
                        values.resize(values.size() + count, target[copy.target]);
                        continue; // Target values already live in the destination space.
                    }
                    const auto* input = inputs[copy.source];
                    if (!input) {
                        values.resize(values.size() + count);
                        continue; // Missing values are defaults, not transformed defaults.
                    }
                    const auto& source_values = std::get<SharedArray<T>>(input->data).array();
                    const auto append = [&](std::uint32_t index) -> Result<void> {
                        T v = source_values[index];
                        if constexpr (std::same_as<T, Vector3>) {
                            if (attribute.usage == Usage::point || attribute.usage == Usage::vector ||
                                attribute.usage == Usage::normal) {
                                v = transformed(v, attribute.usage, copy.placement.matrix, copy.normals);
                                if (!math::is_finite(v))
                                    return failure("A realized vector is not finite.");
                            }
                        } else if constexpr (std::same_as<T, Quaternion>) {
                            if (attribute.usage == Usage::orientation) {
                                v = copy.placement.rotation * v;
                                if (!math::is_finite(v))
                                    return failure("A realized " + std::string{attribute.name.view().std_view()} +
                                                   " is not finite.");
                            }
                        }
                        values.add(std::move(v));
                        return {};
                    };
                    if (attribute.layer == Layer::vertex) {
                        for (std::uint32_t p = 0; p < source.count(Layer::primitive); ++p) {
                            const auto range = source.primitive(p);
                            const bool reverse = copy.placement.reverse && range.kind == PrimitiveKind::polygon;
                            for (std::uint32_t c = 0; c < range.count; ++c)
                                ATELIER_TRY(append(range.first + (reverse ? range.count - 1 - c : c)));
                        }
                    } else
                        for (std::uint32_t i = 0; i < count; ++i)
                            ATELIER_TRY(append(i));
                }
                return SharedArray<T>{std::move(values)};
            },
            attribute.data);
        if (!data)
            return std::unexpected(data.error());
        ATELIER_TRY(result->set(attribute.name, attribute.layer, std::move(*data), attribute.usage));
    }
    return result;
}

std::optional<math::Bounds> bounds(const Geometry& geometry) noexcept {
    const auto& positions = geometry.positions();
    if (positions.empty())
        return std::nullopt;
    math::Bounds result{positions[0], positions[0]};
    for (const auto& p : positions)
        result = math::encapsulate(result, p);
    return result;
}

}
