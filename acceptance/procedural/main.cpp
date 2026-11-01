#include "procedural/bvh.hpp"
#include "procedural/evaluator.hpp"
#include "procedural/instances.hpp"
#include "procedural/nodes.hpp"
#include "procedural/operations.hpp"
#include "procedural/random.hpp"
#include "runtime/native.hpp"
#include "source/program.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using namespace atelier;
using namespace atelier::procedural;
using math::Matrix4;
using math::Quaternion;
using math::Vector2;
using math::Vector3;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <class T>
T take(Result<T> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
    return std::move(*result);
}
void take(Result<void> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
}
template <class T>
void reject(const Result<T>& result, const char* message) {
    require(!result, message);
}
bool near(float a, float b, float tolerance = 1e-5f) {
    return std::abs(a - b) <= tolerance * std::max(1.f, std::abs(b));
}
bool near(Vector3 a, Vector3 b, float tolerance = 1e-5f) {
    return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance);
}
template <class T>
const Array<T>& column(const Geometry& geometry, const char* name, Layer layer) {
    const auto* values = take(geometry.read<T>(name, layer));
    require(values != nullptr, "An expected attribute is missing.");
    return *values;
}
Vector3 normalized(Vector3 v) {
    return *math::try_normalize(v);
}
Quaternion axis_angle(Vector3 axis, float degrees) {
    const float half = degrees * std::numbers::pi_v<float> / 360.f;
    const auto a = normalized(axis) * std::sin(half);
    return {a.x, a.y, a.z, std::cos(half)};
}

Geometry triangles(std::initializer_list<Vector3> corners) {
    Array<Vector3> points{corners};
    Array<std::uint32_t> vertices;
    Array<std::uint32_t> offsets{0};
    for (std::uint32_t i = 0; i < points.size(); ++i)
        vertices.add(i);
    for (std::uint32_t t = 1; t <= points.size() / 3; ++t)
        offsets.add(t * 3);
    return take(Geometry::polygons(std::move(points), std::move(vertices), offsets));
}
// Twice the signed area of (a, b, p) in the XY plane.
double side(Vector3 a, Vector3 b, Vector3 p) {
    return double(b.x - a.x) * (p.y - a.y) - double(b.y - a.y) * (p.x - a.x);
}
bool inside(Vector3 p, Vector3 a, Vector3 b, Vector3 c) {
    const double e = 1e-5;
    return p.z == 0 && side(a, b, p) >= -e && side(b, c, p) >= -e && side(c, a, p) >= -e;
}

void scatter_counts_and_stability() {
    const Vector3 a0{0, 0, 0};
    const Vector3 a1{2, 0, 0};
    const Vector3 a2{0, 1, 0}; // Area 1.
    const Vector3 b0{10, 0, 0};
    const Vector3 b1{13, 0, 0};
    const Vector3 b2{10, 2, 0}; // Area 3.
    const auto surface = triangles({a0, a1, a2, b0, b1, b2});
    const auto points = take(scatter(surface, {.density = 4, .seed = 7}));
    const auto& positions = points.positions();
    const auto& source = column<std::uint32_t>(points, "sourceprim", Layer::point);
    std::uint32_t first = 0;
    std::uint32_t second = 0;
    for (std::size_t i = 0; i < positions.size(); ++i) {
        if (source[i] == 0) {
            ++first;
            require(inside(positions[i], a0, a1, a2), "A point left the first triangle.");
        } else {
            ++second;
            require(source[i] == 1 && inside(positions[i], b0, b1, b2), "A point left the second triangle.");
        }
    }
    require(first == 4 && second == 12, "Area x density points did not land on each triangle exactly.");
    const auto& normals = column<Vector3>(points, "N", Layer::point);
    for (const auto& n : normals)
        require(near(n, {0, 0, 1}), "Scattered normals are not the face normal.");

    const auto again = take(scatter(surface, {.density = 4, .seed = 7}));
    require(std::memcmp(again.positions().data(), positions.data(), positions.size() * sizeof(Vector3)) == 0,
            "The same seed placed points differently.");
    const auto other = take(scatter(surface, {.density = 4, .seed = 8}));
    require(other.positions().size() == positions.size() &&
                std::memcmp(other.positions().data(), positions.data(), positions.size() * sizeof(Vector3)) != 0,
            "Another seed placed the same points.");

    // Moving the second triangle leaves the first one's points bit for bit, and growing
    // the first one (more points before them) leaves the second one's.
    const auto moved =
        take(scatter(triangles({a0, a1, a2, b0 + Vector3{0, 5, 0}, b1 + Vector3{0, 5, 0}, b2 + Vector3{0, 5, 0}}),
                     {.density = 4, .seed = 7}));
    require(std::memcmp(moved.positions().data(), positions.data(), first * sizeof(Vector3)) == 0,
            "Moving one triangle changed another triangle's points.");
    const auto grown = take(scatter(triangles({a0, a1 * 2.f, a2, b0, b1, b2}), {.density = 4, .seed = 7}));
    require(grown.count(Layer::point) == 8 + 12 &&
                std::memcmp(grown.positions().data() + 8, positions.data() + first, second * sizeof(Vector3)) == 0,
            "Growing one triangle changed another triangle's points.");

    // A fractional expectation rounds up with probability equal to its fraction.
    const auto unit = triangles({a0, a1, a2});
    std::uint64_t total = 0;
    for (std::uint32_t seed = 0; seed < 4000; ++seed) {
        const auto count = take(scatter(unit, {.density = 2.25f, .seed = seed})).count(Layer::point);
        require(count == 2 || count == 3, "A fractional count was not rounded to a neighbour.");
        total += count;
    }
    require(std::abs(double(total) / 4000 - 2.25) < 0.03, "Probabilistic rounding is biased.");

    const auto wide = triangles({{0, 0, 0}, {4, 0, 0}, {0, 4, 0}});
    const auto dense = take(scatter(wide, {.density = 60, .seed = 3}));
    const auto spaced = take(scatter(wide, {.density = 60, .seed = 3, .min_distance = 0.3f}));
    const auto& kept = spaced.positions();
    require(kept.size() > 20 && kept.size() < dense.count(Layer::point), "Minimum distance kept no or all points.");
    for (std::size_t i = 0; i < kept.size(); ++i)
        for (std::size_t j = 0; j < i; ++j)
            require(math::length(kept[i] - kept[j]) >= 0.3f, "Two kept points are closer than the minimum distance.");
    require(column<std::uint32_t>(spaced, "id", Layer::point).size() == kept.size(),
            "Elimination left columns unequal.");
    // Cell coordinates beyond the integer range fail instead of overflowing.
    reject(scatter(triangles({{0, 0, 1}, {1, 0, 1}, {0, 1, 1}}), {.density = 2, .min_distance = 1e-20f}),
           "A minimum distance past the cell range scattered.");
}

void transform_by_usage() {
    auto grid_points = take(grid({4, 3}, 4, 5));
    require(grid_points.count(Layer::point) == 20, "A 5 x 4 grid has 20 points.");
    const auto tilted = normalized({1, 1, 0});
    for (auto& n : *take(grid_points.write<Vector3>("N", Layer::point)))
        n = tilted;
    take(grid_points.add<Vector3>("Cd", Layer::point, {0.2f, 0.4f, 0.6f}));
    take(grid_points.add<Vector3>("v", Layer::point, {1, 0, 1}));
    const auto before = grid_points.positions();
    const Vector3 t{1, 2, 3};
    const Vector3 s{2, 3, 0.5f};
    const auto q = axis_angle({0, 1, 0}, 90);
    take(transform(grid_points, t, q, s));
    const auto& after = grid_points.positions();
    const auto mul = [](Vector3 a, Vector3 b) { return Vector3{a.x * b.x, a.y * b.y, a.z * b.z}; };
    for (std::size_t i = 0; i < after.size(); ++i)
        require(near(after[i], math::rotate(q, mul(before[i], s)) + t), "A point did not move by S, then R, then T.");
    const auto inverse_scaled = normalized(math::rotate(q, Vector3{tilted.x / s.x, tilted.y / s.y, tilted.z / s.z}));
    const auto linear = normalized(math::rotate(q, mul(tilted, s)));
    require(!near(inverse_scaled, linear, 1e-2f), "The fixture cannot tell normals from vectors.");
    for (const auto& n : column<Vector3>(grid_points, "N", Layer::point))
        require(near(n, inverse_scaled), "Normals did not take the normalized inverse transpose.");
    for (const auto& v : column<Vector3>(grid_points, "v", Layer::point))
        require(near(v, math::rotate(q, mul({1, 0, 1}, s))), "Vectors did not take the linear part only.");
    for (const auto& c : column<Vector3>(grid_points, "Cd", Layer::point))
        require(c == Vector3{0.2f, 0.4f, 0.6f}, "Colors moved.");

    // A flattening transform still defines plane normals and never makes NaN.
    auto flat = take(line({0, 0, 0}, {1, 0, 0}, 2));
    take(flat.set(String{"N"}, Layer::point, SharedArray<Vector3>{Array<Vector3>{{0.6f, 0, 0.8f}, {1, 0, 0}}}));
    take(transform(flat, {}, {}, {1, 1, 0}));
    const auto& flattened = column<Vector3>(flat, "N", Layer::point);
    require(near(flattened[0], {0, 0, 1}) && flattened[1] == Vector3{}, "Singular transforms broke plane normals.");
    // A non-finite normal or orientation fails even under the identity.
    for (const bool orientation : {false, true}) {
        auto broken = take(line({0, 0, 0}, {1, 0, 0}, 2));
        const float nan = std::numeric_limits<float>::quiet_NaN();
        if (orientation)
            take(broken.add<Quaternion>("orient", Layer::point, {nan, 0, 0, 1}));
        else
            take(broken.set(String{"N"}, Layer::point, SharedArray<Vector3>{Array<Vector3>{{nan, 0, 1}, {0, 0, 1}}}));
        reject(transform(broken, {}, {}, {1, 1, 1}), "A NaN normal or orientation was transformed.");
    }

    // A reflection reverses each primitive with its vertex attributes.
    const auto cube = import_mesh(*geometry::cube_mesh());
    require(cube.count(Layer::point) == 8 && cube.count(Layer::primitive) == 12 && cube.uniform_primitive_size() == 3,
            "The cube did not weld into 8 points and 12 triangles.");
    auto mirrored = cube;
    take(transform(mirrored, {}, {}, {-1, 1, 1}));
    const auto& old_normals = column<Vector3>(cube, "N", Layer::vertex);
    const auto& new_normals = column<Vector3>(mirrored, "N", Layer::vertex);
    const auto& old_uvs = column<Vector2>(cube, "uv", Layer::vertex);
    const auto& new_uvs = column<Vector2>(mirrored, "uv", Layer::vertex);
    for (std::uint32_t p = 0; p < 12; ++p)
        for (std::uint32_t i = 0; i < 3; ++i) {
            require(mirrored.vertex_points()[p * 3 + i] == cube.vertex_points()[p * 3 + 2 - i],
                    "A mirrored triangle kept its order.");
            require(new_uvs[p * 3 + i] == old_uvs[p * 3 + 2 - i],
                    "Vertex attributes did not follow the reversed order.");
            const auto n = old_normals[p * 3 + 2 - i];
            require(near(new_normals[p * 3 + i], {-n.x, n.y, n.z}), "Vertex normals were not reflected.");
        }
    require(column<Vector3>(cube, "N", Layer::vertex).data() != new_normals.data(),
            "Transform wrote the shared source column.");
}

void removal_merge_and_promotion() {
    const auto points = take(grid({4, 3}, 4, 5)); // Columns at x = -2, -1, 0, 1, 2.
    auto flagged = points;
    auto& flags = *take(flagged.add<bool>("cut", Layer::point));
    for (std::size_t i = 0; i < flags.size(); ++i)
        flags[i] = flagged.positions()[i].x > 0;
    const auto remaining = take(flagged.remove_flagged(Layer::point, "cut"));
    require(remaining.count(Layer::point) == 12, "Removing x > 0 should leave 3 of 5 columns.");
    for (const auto& p : remaining.positions())
        require(p.x <= 0, "A flagged point survived.");
    require(column<Vector3>(remaining, "N", Layer::point).size() == 12, "Attributes did not follow removal.");

    const auto cube = import_mesh(*geometry::cube_mesh());
    const auto corner = cube.vertex_points()[0];
    std::uint32_t using_corner = 0;
    const auto vertices = geometry::cube_mesh()->vertices(); // Triangles touching the corner, from the source mesh.
    const auto& corner_position = cube.positions()[corner];
    for (std::size_t t = 0; t < vertices.size() / 3; ++t)
        for (std::size_t i = 0; i < 3; ++i)
            if (Vector3{vertices[t * 3 + i].position[0], vertices[t * 3 + i].position[1],
                        vertices[t * 3 + i].position[2]} == corner_position) {
                ++using_corner;
                break;
            }
    auto marked = cube;
    auto& corners = *take(marked.add<bool>("cut", Layer::point));
    corners[corner] = true;
    const auto opened = take(marked.remove_flagged(Layer::point, "cut"));
    require(opened.count(Layer::point) == 7 && opened.count(Layer::primitive) == 12 - using_corner &&
                opened.count(Layer::vertex) == opened.count(Layer::primitive) * 3,
            "Removing a corner kept a triangle that used it.");
    for (const auto point : opened.vertex_points())
        require(point < 7, "A vertex refers past the remaining points.");
    auto faces = cube;
    auto& face_flags = *take(faces.add<bool>("cut", Layer::primitive));
    face_flags[0] = face_flags[5] = true;
    const auto fewer = take(faces.remove_flagged(Layer::primitive, "cut"));
    require(fewer.count(Layer::primitive) == 10 && fewer.count(Layer::point) == 8,
            "Removing primitives removed points.");
    // An offset whose step would wrap past 2^32 is not a primitive.
    reject(Geometry::polygons({}, {}, Array<std::uint32_t>{0, 0xffff'fffdu, 0}),
           "Wrapping primitive offsets were accepted.");

    // Mixed topology must stay addressable across empty inputs and deletion to uniform or empty.
    const auto empty = take(Geometry::polygons({}, {}, Array<std::uint32_t>{0}));
    const auto mixed = take(
        Geometry::polygons(Array<Vector3>{{0, 0, 0}, {2, 0, 0}, {0, 0, 1}, {4, 0, 0}, {6, 0, 0}, {6, 0, 2}, {4, 0, 2}},
                           Array<std::uint32_t>{2, 0, 1, 3, 5, 6, 4}, Array<std::uint32_t>{0, 3, 7}));
    require(empty.count(Layer::point) == 0 && empty.count(Layer::primitive) == 0 && empty.vertex_points().empty() &&
                empty.uniform_primitive_size() == 0 && empty.bytes() == 0,
            "Zero polygons retained topology or column storage.");
    require(mixed.count(Layer::point) == 7 && mixed.count(Layer::vertex) == 7 && mixed.count(Layer::primitive) == 2 &&
                mixed.uniform_primitive_size() == 0 && mixed.primitive(0).first == 0 && mixed.primitive(0).count == 3 &&
                mixed.primitive(1).first == 3 && mixed.primitive(1).count == 4,
            "Mixed polygon ranges are wrong.");
    const bool remove_triangle[]{true, false};
    const bool remove_all[]{true, true};
    const auto quad = take(mixed.remove_flagged(Layer::primitive, remove_triangle));
    require(quad.count(Layer::point) == 7 && quad.count(Layer::primitive) == 1 && quad.uniform_primitive_size() == 4 &&
                quad.primitive(0).first == 0 && quad.primitive(0).count == 4 &&
                std::ranges::equal(quad.vertex_points(), std::array{3u, 5u, 6u, 4u}),
            "Deleting a triangle damaged the surviving quad.");
    const auto no_faces = take(mixed.remove_flagged(Layer::primitive, remove_all));
    require(no_faces.count(Layer::point) == 7 && no_faces.count(Layer::primitive) == 0 &&
                no_faces.count(Layer::vertex) == 0 && no_faces.uniform_primitive_size() == 0 &&
                no_faces.vertex_points().empty() && no_faces.bytes() > 0,
            "Deleting all polygons lost their points or retained topology.");
    const auto combined = take(Geometry::merge(std::array{empty, mixed, quad, empty}));
    require(
        combined.count(Layer::point) == 14 && combined.count(Layer::primitive) == 3 &&
            combined.count(Layer::vertex) == 11 && combined.uniform_primitive_size() == 0 &&
            combined.primitive(1).first == 3 && combined.primitive(1).count == 4 && combined.primitive(2).first == 7 &&
            combined.primitive(2).count == 4 &&
            std::ranges::equal(combined.vertex_points(), std::array{2u, 0u, 1u, 3u, 5u, 6u, 4u, 10u, 12u, 13u, 11u}),
        "Merging mixed and empty topology lost ranges or point offsets.");
    const auto empty_pair = take(Geometry::merge(std::array{empty, empty}));
    require(empty_pair.count(Layer::point) == 0 && empty_pair.count(Layer::primitive) == 0 &&
                empty_pair.count(Layer::vertex) == 0 && empty_pair.uniform_primitive_size() == 0 &&
                empty_pair.bytes() == 0,
            "Merging empty topology allocated element storage.");

    reject(Geometry::merge(std::vector{points, cube}), "Point N and vertex N merged.");
    const auto pair = take(Geometry::merge(std::vector{cube, cube}));
    require(pair.count(Layer::point) == 16 && pair.count(Layer::primitive) == 24 && pair.uniform_primitive_size() == 3,
            "Merged cubes lost elements.");
    for (std::uint32_t v = 0; v < 36; ++v)
        require(pair.vertex_points()[36 + v] == cube.vertex_points()[v] + 8,
                "The second cube's vertices were not offset.");

    // Vertex normals averaged per point, against sums taken straight from the mesh vertices.
    auto smooth = cube;
    take(promote(smooth, "N", Layer::point, Reduce::average));
    const auto& point_normals = column<Vector3>(smooth, "N", Layer::point);
    for (std::uint32_t point = 0; point < 8; ++point) {
        Vector3 sum{};
        float count = 0;
        for (const auto& vertex : vertices)
            if (Vector3{vertex.position[0], vertex.position[1], vertex.position[2]} == smooth.positions()[point]) {
                sum += Vector3{vertex.normal[0], vertex.normal[1], vertex.normal[2]};
                ++count;
            }
        require(near(point_normals[point], sum / count), "A point normal is not the mean of its vertex normals.");
    }
    auto summed = smooth;
    take(summed.add<float>("weight", Layer::point, 0.5f));
    take(promote(summed, "weight", Layer::detail, Reduce::sum));
    require(column<float>(summed, "weight", Layer::detail)[0] == 4.f, "Detail sum of 8 x 0.5 is not 4.");
    take(promote(summed, "weight", Layer::primitive, Reduce::first));
    require(column<float>(summed, "weight", Layer::primitive).size() == 12, "Detail did not broadcast to primitives.");
    reject(promote(summed, "P", Layer::vertex, Reduce::first), "P left the points.");
    reject(summed.add<String>("label", Layer::point), "A string attribute was put on points.");
    reject(summed.add<float>("2bad", Layer::point), "A non-identifier attribute name was accepted.");

    auto copy = cube;
    for (auto& uv : *take(copy.write<Vector2>("uv", Layer::vertex)))
        uv = {9, 9};
    require(column<Vector2>(cube, "uv", Layer::vertex)[0] != Vector2{9, 9}, "Writing a copy changed the source.");
    require(column<Vector3>(copy, "N", Layer::vertex).data() == column<Vector3>(cube, "N", Layer::vertex).data(),
            "An unwritten column was copied.");

    // Charge actual retained capacity, not logical length; sharing does not discount a value's budget.
    for (const std::uint32_t count : {0u, 3u}) {
        Array<Vector3> positions;
        positions.resize(count, {1, 2, 3});
        auto small = take(Geometry::points(std::move(positions)));
        const auto before = small.bytes();
        take(small.add<float>("weight", Layer::point, 7.25f));
        const auto& weights = column<float>(small, "weight", Layer::point);
        require(weights.size() == count && std::ranges::all_of(weights, [](float x) { return x == 7.25f; }),
                "A new column lost its fill.");
        require(small.bytes() - before == weights.capacity() * sizeof(float),
                "Column bytes do not charge retained capacity.");
        take(small.add<String>("label", Layer::detail, String{"nonzero fill"}));
        require(column<String>(small, "label", Layer::detail)[0] == String{"nonzero fill"},
                "Detail String fill was lost.");
        const auto shared = small;
        require(shared.bytes() == small.bytes(), "Sharing discounted a Geometry's byte budget.");
        take(small.add<float>("weight", Layer::point, -2.f));
        require(std::ranges::all_of(column<float>(small, "weight", Layer::point), [](float x) { return x == 7.25f; }),
                "Adding an existing column replaced its values with the fill.");
    }
}

Matrix4 expected_instance(Vector3 position, Quaternion orient, Vector3 scale) {
    Matrix4 m = Matrix4::identity();
    const Vector3 axes[3]{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (std::size_t row = 0; row < 3; ++row) {
        const auto axis = math::rotate(orient, axes[row]) * scale[row];
        m(row, 0) = axis.x;
        m(row, 1) = axis.y;
        m(row, 2) = axis.z;
    }
    m[12] = position.x;
    m[13] = position.y;
    m[14] = position.z;
    return m;
}
bool near(const Matrix4& a, const Matrix4& b) {
    for (std::size_t i = 0; i < 16; ++i)
        if (!near(a[i], b[i]))
            return false;
    return true;
}

void copy_to_points_and_publication() {
    const auto mesh_a = geometry::cube_mesh();
    const auto mesh_b = std::make_shared<const geometry::Mesh>(
        std::vector<geometry::Vertex>(mesh_a->vertices().begin(), mesh_a->vertices().end()));
    const std::vector<Reference> references{{String{"fixture/a"}, mesh_a}, {String{"fixture/b"}, mesh_b}};

    auto oriented = take(Geometry::points(Array<Vector3>{{1, 2, 3}, {-4, 0, 1}, {0, 0, 0}}));
    const Quaternion orients[3]{axis_angle({1, 2, 3}, 40), axis_angle({0, 0, 1}, -75), {}};
    take(oriented.set(String{"orient"}, Layer::point,
                      SharedArray<Quaternion>{Array<Quaternion>{orients[0], orients[1], orients[2]}}));
    take(oriented.set(String{"pscale"}, Layer::point, SharedArray<float>{Array<float>{2, 0.5f, 1}}));
    take(oriented.set(String{"scale"}, Layer::point,
                      SharedArray<Vector3>{Array<Vector3>{{1, 2, 3}, {3, 1, 1}, {1, 1, 1}}}));
    take(oriented.set(String{"variant"}, Layer::point, SharedArray<std::uint32_t>{Array<std::uint32_t>{0, 1, 1}}));
    take(oriented.add<String>("label", Layer::detail, String{"kept"}));
    const auto instances = take(copy_to_points(oriented, references));
    require(instances.count(Layer::point) == 0 && instances.count(Layer::instance) == 3,
            "Copy to Points made no instances.");
    require(column<String>(instances, "label", Layer::detail)[0] == String{"kept"}, "Detail attributes were dropped.");
    const Vector3 scales[3]{{2, 4, 6}, {1.5f, 0.5f, 0.5f}, {1, 1, 1}};
    for (std::uint32_t i = 0; i < 3; ++i) {
        require(
            near(instances.instance_transforms()[i], expected_instance(oriented.positions()[i], orients[i], scales[i])),
            "An instance transform is not S * O * T.");
        require(instances.instance_references()[i] == (i ? 1u : 0u), "Variant did not pick the reference.");
    }

    auto edited_instances = instances;
    auto changed_matrix = instances.instance_transforms()[1];
    changed_matrix[12] += 13.f;
    edited_instances.write_instance_transforms()[1] = changed_matrix;
    require(edited_instances.instance_transforms()[1] == changed_matrix &&
                near(instances.instance_transforms()[1],
                     expected_instance(oriented.positions()[1], orients[1], scales[1])) &&
                edited_instances.instance_transforms()[0] == instances.instance_transforms()[0] &&
                edited_instances.instance_transforms()[2] == instances.instance_transforms()[2],
            "Writing instance transforms changed the source or another instance.");
    require(edited_instances.instance_references().data() == instances.instance_references().data(),
            "Writing transforms copied the reference column.");

    // Without orient: N is +Z and up steers +Y; N alone takes the shortest turn.
    auto aligned = take(Geometry::points(Array<Vector3>{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}));
    take(
        aligned.set(String{"N"}, Layer::point, SharedArray<Vector3>{Array<Vector3>{{3, 0, 0}, {0, 0, -1}, {0, 0, 1}}}));
    auto with_up = aligned;
    take(
        with_up.set(String{"up"}, Layer::point, SharedArray<Vector3>{Array<Vector3>{{0, 5, 0}, {0, 0, 0}, {1, 0, 0}}}));
    const auto rows = [](const Matrix4& m) {
        return std::array<Vector3, 3>{Vector3{m[0], m[1], m[2]}, Vector3{m[4], m[5], m[6]}, Vector3{m[8], m[9], m[10]}};
    };
    const auto up_axes = rows(take(copy_to_points(with_up, references)).instance_transforms()[0]);
    require(near(up_axes[0], {0, 0, -1}) && near(up_axes[1], {0, 1, 0}) && near(up_axes[2], {1, 0, 0}),
            "N and up did not align +Z and +Y.");
    const auto turned = take(copy_to_points(aligned, references));
    const auto opposite = rows(turned.instance_transforms()[1]);
    require(near(opposite[0], {1, 0, 0}) && near(opposite[1], {0, -1, 0}) && near(opposite[2], {0, 0, -1}),
            "N = -Z did not turn half way.");
    const auto same = rows(turned.instance_transforms()[2]);
    require(near(same[0], {1, 0, 0}) && near(same[2], {0, 0, 1}), "N = +Z rotated the instance.");
    const auto rolled = rows(take(copy_to_points(with_up, references)).instance_transforms()[2]);
    require(near(rolled[2], {0, 0, 1}) && near(rolled[1], {1, 0, 0}), "Up did not roll an instance about N.");

    auto broken = oriented;
    (*take(broken.write<float>("pscale", Layer::point)))[1] = std::nanf("");
    reject(copy_to_points(broken, references), "A NaN pscale made an instance.");
    auto missing = oriented;
    (*take(missing.write<std::uint32_t>("variant", Layer::point)))[2] = 2;
    reject(copy_to_points(missing, references), "A variant past the references made an instance.");

    // Publication: chunks of 10 split at 10 and 0; zero and flattened scales drop.
    auto scattered = take(Geometry::points(
        Array<Vector3>{{9.99f, 0, 0}, {10, 0, 0}, {-0.01f, 0, 0}, {0, 0, 0}, {5, 0, 5}, {6, 0, 6}, {3, 0, 3}}));
    take(scattered.set(String{"variant"}, Layer::point,
                       SharedArray<std::uint32_t>{Array<std::uint32_t>{0, 0, 0, 1, 1, 0, 0}}));
    take(scattered.set(String{"pscale"}, Layer::point, SharedArray<float>{Array<float>{1, 1, 1, 1, 0, 1, 1}}));
    take(scattered.set(String{"scale"}, Layer::point,
                       SharedArray<Vector3>{Array<Vector3>{
                           {1, 1, 1}, {1, 1, 1}, {1, 1, 1}, {1, 1, 1}, {1, 1, 1}, {1, 0, 1}, {2, 2, 2}}}));
    const auto first = take(publish(take(copy_to_points(scattered, references)), 10));
    require(first.dropped == 2, "Zero and flattened scales were published.");
    std::map<std::pair<std::string, std::array<std::int32_t, 3>>, std::vector<float>> found;
    for (const auto& collection : first.collections) {
        require(!collection.transforms->empty(), "An empty collection was published.");
        auto& xs = found[{std::string{collection.reference.key.view().std_view()}, collection.chunk}];
        for (const auto& affine : *collection.transforms)
            xs.push_back(affine[9]);
    }
    const std::map<std::pair<std::string, std::array<std::int32_t, 3>>, std::vector<float>> expected{
        {{"fixture/a", {0, 0, 0}}, {9.99f, 3}},
        {{"fixture/a", {1, 0, 0}}, {10}},
        {{"fixture/a", {-1, 0, 0}}, {-0.01f}},
        {{"fixture/b", {0, 0, 0}}, {0}}};
    require(found == expected, "Instances were not grouped by reference and chunk in order.");
    const auto& scaled = first.collections[1].transforms->back(); // fixture/a chunk 0, the point at 3.
    require(first.collections[1].chunk == std::array<std::int32_t, 3>{0, 0, 0} && scaled[0] == 2 && scaled[4] == 2 &&
                scaled[8] == 2 && scaled[11] == 3,
            "The 3x4 layout is not axes then translation.");

    // Republishing after moving one instance within its chunk replaces only that collection.
    auto shifted = scattered;
    (*take(shifted.write<Vector3>("P", Layer::point)))[1] = {11, 0, 0};
    const auto second = take(publish(take(copy_to_points(shifted, references)), 10, &first));
    require(second.collections.size() == first.collections.size(), "Moving within a chunk changed the collections.");
    std::size_t reused = 0;
    for (std::size_t i = 0; i < second.collections.size(); ++i) {
        const bool changed = second.collections[i].chunk == std::array<std::int32_t, 3>{1, 0, 0};
        require((second.collections[i].transforms == first.collections[i].transforms) != changed,
                "Only the changed chunk should get a new transforms pointer.");
        reused += !changed;
    }
    require(reused == 3, "Unchanged chunks were not reused.");
    reject(publish(instances, 0), "A zero chunk size was accepted.");
}

// References in double precision over every triangle, by other formulas than the BVH uses.
using Corners = std::array<math::Vector3d, 3>;
std::vector<Corners> corners(const Geometry& surface) {
    std::vector<Corners> result;
    const auto& p = surface.positions();
    const auto v = surface.vertex_points();
    const auto d = [](Vector3 x) { return math::Vector3d{x.x, x.y, x.z}; };
    for (std::uint32_t t = 0; t < surface.count(Layer::primitive); ++t)
        result.push_back({d(p[v[3 * t]]), d(p[v[3 * t + 1]]), d(p[v[3 * t + 2]])});
    return result;
}
// Distance along a unit ray to the plane, then inside-ness by the signs of three edge volumes.
std::optional<double> brute_ray(const std::vector<Corners>& triangles, math::Vector3d o, math::Vector3d d,
                                double limit) {
    std::optional<double> best;
    for (const auto& [a, b, c] : triangles) {
        const auto n = math::cross(b - a, c - a);
        const double facing = math::dot(n, d);
        if (facing == 0)
            continue;
        const double t = math::dot(a - o, n) / facing;
        if (t < 0 || t > limit || (best && t >= *best))
            continue;
        const auto q = o + d * t;
        const double s0 = math::dot(math::cross(b - a, q - a), n);
        const double s1 = math::dot(math::cross(c - b, q - b), n);
        const double s2 = math::dot(math::cross(a - c, q - c), n);
        if ((s0 >= 0 && s1 >= 0 && s2 >= 0) || (s0 <= 0 && s1 <= 0 && s2 <= 0))
            best = t;
    }
    return best;
}
double segment_distance(math::Vector3d p, math::Vector3d a, math::Vector3d b) {
    const auto ab = b - a;
    const double t = std::clamp(math::dot(p - a, ab) / math::dot(ab, ab), 0.0, 1.0);
    const auto q = a + ab * t - p;
    return std::sqrt(math::dot(q, q));
}
// The plane projection when it falls inside the triangle, else the nearest edge point.
double brute_closest(const std::vector<Corners>& triangles, math::Vector3d p) {
    double best = std::numeric_limits<double>::infinity();
    for (const auto& [a, b, c] : triangles) {
        const auto n = math::cross(b - a, c - a);
        const auto q = p - n * (math::dot(p - a, n) / math::dot(n, n));
        const double s0 = math::dot(math::cross(b - a, q - a), n);
        const double s1 = math::dot(math::cross(c - b, q - b), n);
        const double s2 = math::dot(math::cross(a - c, q - c), n);
        const auto offset = q - p;
        const double distance =
            s0 >= 0 && s1 >= 0 && s2 >= 0
                ? std::sqrt(math::dot(offset, offset))
                : std::min({segment_distance(p, a, b), segment_distance(p, b, c), segment_distance(p, c, a)});
        best = std::min(best, distance);
    }
    return best;
}

void bvh_queries() {
    // A bumpy, tilted 40 x 40 height field with alternating diagonals: 3200 triangles.
    constexpr std::uint32_t side = 41;
    Array<Vector3> positions;
    for (std::uint32_t r = 0; r < side; ++r)
        for (std::uint32_t c = 0; c < side; ++c) {
            const float x = float(c) * 0.5f;
            const float z = float(r) * 0.5f;
            positions.add({x, std::sin(x * 0.7f) * std::cos(z * 0.4f) * 1.5f + 0.1f * x, z});
        }
    Array<std::uint32_t> vertices;
    Array<std::uint32_t> offsets{0};
    for (std::uint32_t r = 0; r + 1 < side; ++r)
        for (std::uint32_t c = 0; c + 1 < side; ++c) {
            const std::uint32_t p00 = r * side + c;
            const std::uint32_t p01 = p00 + 1;
            const std::uint32_t p10 = p00 + side;
            const std::uint32_t p11 = p10 + 1;
            const bool flip = (r + c) % 2 != 0;
            for (const auto point :
                 flip ? std::array{p00, p10, p11, p00, p11, p01} : std::array{p00, p10, p01, p01, p10, p11})
                vertices.add(point);
            offsets.add(static_cast<std::uint32_t>(vertices.size() - 3));
            offsets.add(static_cast<std::uint32_t>(vertices.size()));
        }
    const auto surface = take(Geometry::polygons(std::move(positions), std::move(vertices), offsets));
    const auto bvh = take(Bvh::build(surface));
    const auto references = corners(surface);
    std::uint32_t random = 12345;
    std::uint32_t hits = 0;
    std::uint32_t misses = 0;
    const auto next = [&] {
        random = hash(random);
        return unit(random);
    };
    for (int i = 0; i < 400; ++i) {
        // Down, up from below, and nearly horizontal rays through the bumps, some starting outside.
        const int kind = i % 3;
        const Vector3 origin{next() * 26 - 3, kind == 0 ? 10.f : kind == 1 ? -10.f : next() * 3 - 1, next() * 26 - 3};
        const auto direction =
            normalized(kind == 2 ? Vector3{next() - 0.5f, (next() - 0.5f) * 0.2f, next() - 0.5f}
                                 : Vector3{(next() - 0.5f) * 0.6f, kind == 0 ? -1.f : 1.f, (next() - 0.5f) * 0.6f});
        const float limit = i % 7 == 0 ? 4.f : 40.f;
        const auto found = bvh.ray(origin, direction, limit);
        const auto expected =
            brute_ray(references, {origin.x, origin.y, origin.z}, {direction.x, direction.y, direction.z}, limit);
        require(found.has_value() == expected.has_value(), "A BVH ray disagrees with the brute force on hitting.");
        if (!found) {
            ++misses;
            continue;
        }
        ++hits;
        require(std::abs(found->distance - *expected) <= 1e-4 * std::max(1.0, *expected),
                "A BVH ray found another distance.");
        const auto& [a, b, c] = references[found->primitive];
        const auto on = a * (1.0 - found->uv.x - found->uv.y) + b * double(found->uv.x) + c * double(found->uv.y);
        require(near(found->position, Vector3{float(on.x), float(on.y), float(on.z)}, 1e-4f) &&
                    near(found->position, origin + direction * found->distance, 1e-4f),
                "A ray hit is not on its primitive at its distance.");
    }
    require(hits > 100 && misses > 30, "The rays did not cover hits and misses.");
    for (int i = 0; i < 300; ++i) {
        const Vector3 point{next() * 26 - 3, next() * 8 - 4, next() * 26 - 3};
        const float limit = i % 5 == 0 ? 0.5f : 100.f;
        const auto found = bvh.closest(point, limit);
        const double expected = brute_closest(references, {point.x, point.y, point.z});
        require(found.has_value() == (expected <= limit), "A closest query disagrees with the brute force on range.");
        if (!found)
            continue;
        require(std::abs(found->distance - expected) <= 1e-4 * std::max(1.0, expected),
                "A closest query found another distance.");
        const auto& [a, b, c] = references[found->primitive];
        const auto on = a * (1.0 - found->uv.x - found->uv.y) + b * double(found->uv.x) + c * double(found->uv.y);
        require(found->uv.x >= 0 && found->uv.y >= 0 && found->uv.x + found->uv.y <= 1 + 1e-6f &&
                    near(found->position, Vector3{float(on.x), float(on.y), float(on.z)}, 1e-4f),
                "A closest point is not on its primitive.");
    }
    const auto quad = take(Geometry::polygons(Array<Vector3>{{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}},
                                              Array<std::uint32_t>{0, 1, 2, 3}, Array<std::uint32_t>{0, 4}));
    reject(Bvh::build(quad), "A quad built a BVH.");
    require(!take(Bvh::build(take(grid({1, 1}, 2, 2)))).ray({0, 1, 0}, {0, -1, 0}, 10),
            "Points without primitives were hit.");
    // Two stacked triangles share one leaf; the nearer one is found from either side.
    const auto stacked =
        take(Bvh::build(triangles({{0, 1, 0}, {0, 1, 4}, {4, 1, 0}, {0, 0, 0}, {0, 0, 4}, {4, 0, 0}})));
    const auto above = stacked.ray({1, 5, 1}, {0, -1, 0}, 10);
    const auto below = stacked.ray({1, -5, 1}, {0, 1, 0}, 10);
    require(above && above->primitive == 0 && near(above->distance, 4) && below && below->primitive == 1 &&
                near(below->distance, 5),
            "A ray did not keep the nearest of several hits.");
    // A negative-zero direction component hits the box boundary like a positive one.
    const auto edge = take(Bvh::build(triangles({{-1, 0, 0}, {0, -1, 0}, {0, 1, 0}})));
    for (const auto direction : {Vector3{0, 0, -1}, -Vector3{0, 0, 1}}) {
        const auto hit = edge.ray({0, 0, 1}, direction, 2);
        require(hit && near(hit->distance, 1), "The sign of a zero direction component changed a hit.");
    }
    // Finite coordinates whose float products overflow give finite hits within the limit.
    const auto huge = take(Bvh::build(triangles({{0, 0, 0}, {1e20f, 0, 0}, {0, 1e20f, 0}})));
    const auto far_hit = huge.ray({2.5e19f, 2.5e19f, 1}, {0, 0, -1}, 2);
    require(far_hit && far_hit->distance == 1 && math::is_finite(far_hit->uv) && near(far_hit->uv.x, 0.25f) &&
                far_hit->position == Vector3{2.5e19f, 2.5e19f, 0},
            "Overflowing products broke a ray hit.");
    const auto distant = take(Bvh::build(triangles({{2e20f, 0, 0}, {2e20f, 1, 0}, {2e20f, 0, 1}})));
    require(!distant.closest({0, 0, 0}, 1e20f) && distant.closest({0, 0, 0}, 3e20f),
            "Overflowing squares broke the closest range.");
}

void ray_node() {
    // Ground at y = 0 over [0, 10]^2, and a raised triangle at y = 2 whose winding faces +Y.
    const auto surface = triangles(
        {{0, 0, 0}, {0, 0, 10}, {10, 0, 0}, {10, 0, 0}, {0, 0, 10}, {10, 0, 10}, {0, 2, 0}, {0, 2, 4}, {4, 2, 0}});
    const auto points = take(Geometry::points(Array<Vector3>{{1, 5, 1}, {8, 5, 8}, {20, 5, 20}, {1, -1, 1}}));
    Ray node;
    node.normal = true;
    Geometry output;
    node.evaluate(points, surface, output);
    const auto& p = output.positions();
    const auto& primitive = column<std::int32_t>(output, "hitprim", Layer::point);
    const auto& uv = column<Vector2>(output, "hitprimuv", Layer::point);
    const auto& distance = column<float>(output, "hitdist", Layer::point);
    const auto& n = column<Vector3>(output, "N", Layer::point);
    require(near(p[0], {1, 2, 1}) && primitive[0] == 2 && near(distance[0], 3) && near(uv[0].x, 0.25f) &&
                near(uv[0].y, 0.25f) && near(n[0], {0, 1, 0}),
            "A ray did not stop at the nearest, raised triangle.");
    require(near(p[1], {8, 0, 8}) && (primitive[1] == 0 || primitive[1] == 1) && near(distance[1], 5),
            "A ray did not reach the ground.");
    require(near(p[2], {20, 5, 20}) && primitive[2] == -1 && distance[2] == -1 && near(n[2], {0, 0, 0}),
            "A missing ray moved its point.");
    require(near(p[3], {1, -1, 1}) && primitive[3] == -1, "A ray hit a surface behind its origin.");
    Geometry projected_again;
    node.evaluate(output, surface, projected_again);
    require(near(column<float>(projected_again, "hitdist", Layer::point)[0], 0) &&
                near(column<float>(projected_again, "hitdist", Layer::point)[1], 0) &&
                column<std::int32_t>(projected_again, "hitprim", Layer::point)[0] == 2 &&
                column<std::int32_t>(projected_again, "hitprim", Layer::point)[2] == -1 &&
                near(projected_again.positions()[0], {1, 2, 1}),
            "Ray did not overwrite existing result columns.");
    require(near(column<float>(output, "hitdist", Layer::point)[0], 3) &&
                near(column<float>(output, "hitdist", Layer::point)[1], 5) && points.positions()[0] == Vector3{1, 5, 1},
            "Ray wrote through a shared input column.");
    node.remove_misses = true;
    node.evaluate(points, surface, output);
    require(output.count(Layer::point) == 2 && near(output.positions()[1], {8, 0, 8}),
            "Remove Misses did not keep exactly the hits.");
    // Closest point: straight down is the ground at 2.5, but the raised triangle's corner is nearer.
    node.mode = RayMode::minimum;
    node.direction = {};
    node.evaluate(take(Geometry::points(Array<Vector3>{{6, 2.5f, 1}})), surface, output);
    require(near(output.positions()[0], {4, 2, 0}) &&
                near(column<float>(output, "hitdist", Layer::point)[0], std::sqrt(5.25f)),
            "Minimum mode did not find the nearest surface point.");
    node.mode = RayMode::project;
    bool thrown = false;
    try {
        node.evaluate(points, surface, output);
    } catch (const std::runtime_error&) {
        thrown = true;
    }
    require(thrown, "A zero direction was projected.");
}

struct Meshes final : Resources {
    std::map<std::string, geometry::MeshRef, std::less<>> meshes;
    std::uint64_t version{1};
    Result<geometry::MeshRef> mesh(StringView key) const override {
        const auto found = meshes.find(key.std_view());
        if (found == meshes.end())
            return std::unexpected(Error{ErrorCode::invalid_member, "No mesh " + std::string{key.std_view()} + "."});
        return found->second;
    }
    std::uint64_t revision() const noexcept override { return version; }
};

template <class T>
TypeId type(Runtime& runtime) {
    return take(runtime.native_type<T>());
}
FieldId field(Runtime& runtime, TypeId owner, const char* name) {
    return take(runtime.find_field(owner, name));
}

// A node registered by hand, without parameter names.
class Unnamed : public Node {
public:
    void evaluate(Geometry&) const {}
};

struct PaddedParameters {
    bool enabled{true};
    std::uint32_t count{3};
};

class RecordNode : public Node {
public:
    PaddedParameters parameters;
    void evaluate(Geometry& output) const {
        output = parameters.enabled ? take(line({0, 0, 0}, {1, 0, 0}, parameters.count)) : Geometry{};
    }
};

struct Counts {
    std::uint64_t evaluations;
    std::uint64_t hits;
    bool operator==(const Counts&) const = default;
};
void expect(const Evaluator& evaluator, std::initializer_list<std::pair<ObjectId, Counts>> nodes, const char* message) {
    for (const auto& [node, counts] : nodes) {
        const auto* status = evaluator.status(node);
        require(status && Counts{status->evaluations, status->cache_hits} == counts, message);
    }
}
bool failed(const Evaluator& evaluator, ObjectId node) {
    return evaluator.status(node)->error.has_value();
}

void record_parameter_keys() {
    Runtime runtime;
    take(register_types(runtime));
    const auto parameters_type =
        take(NativeRegistration<PaddedParameters>{runtime, "proceduralAcceptance.Parameters", TypeKind::record}
                 .field<&PaddedParameters::enabled>("enabled")
                 .field<&PaddedParameters::count>("count")
                 .publish());
    const auto node_type =
        take(NativeRegistration<RecordNode>{runtime, "proceduralAcceptance.RecordNode", TypeKind::object}
                 .base<Node>()
                 .field<&RecordNode::parameters>("parameters")
                 .method<&RecordNode::evaluate>("evaluate")
                 .parameter_names({"output"})
                 .publish());
    const auto parameters_field = field(runtime, node_type, "parameters");
    const auto graph = take(runtime.create(type<Graph>(runtime)));
    const auto first = take(add_node(runtime, graph.id(), node_type));
    const auto second = take(add_node(runtime, graph.id(), node_type));
    PaddedParameters parameters;
    static_assert(offsetof(PaddedParameters, count) > sizeof(bool));
    const auto bytes = std::as_writable_bytes(std::span{&parameters, 1});
    std::fill(bytes.begin() + sizeof(bool), bytes.begin() + offsetof(PaddedParameters, count), std::byte{});
    take(runtime.write(first.id(), parameters_field, ValueInput{parameters_type, &parameters}));
    take(set_display(runtime, graph.id(), first.id()));
    Meshes resources;
    Evaluator evaluator{runtime, graph, resources};
    take(evaluator.evaluate(true));
    require(!failed(evaluator, first.id()) && evaluator.output()->count(Layer::point) == 3,
            "A record parameter did not reach evaluate.");
    require(evaluator.status(first.id())->time.has_value(), "An explicitly timed node did not record its run.");

    std::fill(bytes.begin() + sizeof(bool), bytes.begin() + offsetof(PaddedParameters, count), std::byte{0x7f});
    take(runtime.write(first.id(), parameters_field, ValueInput{parameters_type, &parameters}));
    take(evaluator.evaluate());
    expect(evaluator, {{first.id(), {1, 0}}}, "Changing only record padding reran a node.");
    take(runtime.write(second.id(), parameters_field, ValueInput{parameters_type, &parameters}));
    take(set_display(runtime, graph.id(), second.id()));
    take(evaluator.evaluate());
    expect(evaluator, {{second.id(), {0, 1}}}, "Equal record fields with different padding did not share a result.");
    require(!evaluator.status(second.id())->time, "A cache hit inherited another node's timing.");
    require(evaluator.output()->count(Layer::point) == 3, "A shared record result changed its output.");

    parameters.count = 4;
    take(runtime.write(second.id(), parameters_field, ValueInput{parameters_type, &parameters}));
    take(evaluator.evaluate());
    expect(evaluator, {{second.id(), {1, 1}}}, "A record field change reused an obsolete result.");
    require(evaluator.output()->count(Layer::point) == 4, "The changed record count did not reach evaluate.");
    parameters.enabled = false;
    take(runtime.write(second.id(), parameters_field, ValueInput{parameters_type, &parameters}));
    take(evaluator.evaluate());
    require(evaluator.output()->count(Layer::point) == 0, "The record bool was omitted from its key.");
}

bool same_instances(const Geometry* actual, const Geometry& expected) {
    require(actual != nullptr, "The graph has no output.");
    const auto& a = actual->instance_transforms();
    const auto& b = expected.instance_transforms();
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(Matrix4)) == 0 &&
           std::ranges::equal(actual->references(), expected.references());
}

// Grid -> Transform -> Merge <- Line, Merge -> Copy to Points, by direct calls.
Geometry expected_output(Vector3 translate, Vector3 scale, std::uint32_t line_points, const geometry::MeshRef& mesh) {
    auto moved = take(grid({3, 2}, 3, 4));
    take(transform(moved, translate, {}, scale));
    const std::array inputs{moved, take(line({0, 0, 0}, {0, 1, 0}, line_points))};
    const std::vector<Reference> references{{String{"fixture/a"}, mesh}};
    return take(copy_to_points(take(Geometry::merge(inputs)), references));
}

void graph_evaluation() {
    Runtime runtime;
    take(register_types(runtime));
    const auto published = runtime.type_count();
    take(register_types(runtime));
    require(runtime.type_count() == published, "Registering twice published types again.");
    const auto merge_ports = take(ports(runtime, type<Merge>(runtime)));
    require(merge_ports.multiple == std::vector<bool>{true} &&
                merge_ports.inputs == std::vector<std::string>{"inputs"} &&
                merge_ports.outputs == std::vector<std::string>{"output"},
            "Merge ports were not read from evaluate.");
    // Port names skip the context parameter but keep evaluate's names.
    const auto scatter_ports = take(ports(runtime, type<Scatter>(runtime)));
    require(scatter_ports.parameters == std::vector<Slot>{Slot::context, Slot::input, Slot::output} &&
                scatter_ports.inputs == std::vector<std::string>{"surface"} &&
                scatter_ports.outputs == std::vector<std::string>{"points"},
            "Scatter ports were not read in order.");
    require(take(ports(runtime, type<Ray>(runtime))).inputs == std::vector<std::string>{"points", "surface"},
            "Ray input names are wrong.");
    reject(ports(runtime, type<Graph>(runtime)), "A graph was accepted as a node type.");
    const auto unnamed = take(NativeRegistration<Unnamed>{runtime, "proceduralAcceptance.Unnamed", TypeKind::object}
                                  .base<Node>()
                                  .method<&Unnamed::evaluate>("evaluate")
                                  .publish());
    reject(ports(runtime, unnamed), "An unnamed output was accepted as a port.");

    Meshes resources;
    auto cube = geometry::cube_mesh();
    resources.meshes["fixture/a"] = cube;
    const auto graph_root = take(runtime.create(type<Graph>(runtime)));
    const auto graph = graph_root.id();
    reject(add_node(runtime, graph, type<Graph>(runtime)), "A graph was added as a node.");
    const auto add = [&](TypeId node_type) { return take(add_node(runtime, graph, node_type)); };
    const auto grid_root = add(type<Grid>(runtime));
    const auto moved_root = add(type<Transform>(runtime));
    const auto line_root = add(type<Line>(runtime));
    const auto merged_root = add(type<Merge>(runtime));
    const auto copied_root = add(type<CopyToPoints>(runtime));
    const auto grid_node = grid_root.id();
    const auto moved = moved_root.id();
    const auto line_node = line_root.id();
    const auto merged = merged_root.id();
    const auto copied = copied_root.id();
    const auto vector2 = type<Vector2>(runtime);
    const auto vector3 = type<Vector3>(runtime);
    const auto translate = field(runtime, type<Transform>(runtime), "translate");
    const auto scale = field(runtime, type<Transform>(runtime), "scale");
    const auto write_vector = [&](ObjectId node, FieldId at, Vector3 value) {
        take(runtime.write(node, at, ValueInput{vector3, &value}));
    };
    const Vector2 size{3, 2};
    take(runtime.write(grid_node, field(runtime, type<Grid>(runtime), "size"), ValueInput{vector2, &size}));
    take(runtime.write(grid_node, field(runtime, type<Grid>(runtime), "rows"), Scalar{std::uint32_t{3}}));
    take(runtime.write(grid_node, field(runtime, type<Grid>(runtime), "columns"), Scalar{std::uint32_t{4}}));
    write_vector(moved, translate, {1, 0, 0});
    take(runtime.with_native<CopyToPoints>(copied, [](CopyToPoints& node) -> Result<void> {
        node.meshes.add(String{"fixture/a"});
        return {};
    }));
    take(connect(runtime, graph, grid_node, 0, moved, 0));
    take(connect(runtime, graph, moved, 0, merged, 0));
    take(connect(runtime, graph, line_node, 0, merged, 0));
    take(connect(runtime, graph, merged, 0, copied, 0));
    take(set_display(runtime, graph, copied));
    const auto merged_inputs = take(inputs(runtime, merged));
    require(merged_inputs == std::vector<Input>{{moved, 0, 0}, {line_node, 0, 0}},
            "A multiple input did not keep connection order.");

    Evaluator evaluator{runtime, graph_root, resources};
    auto cursor = take(runtime.change_cursor());
    take(evaluator.evaluate());
    std::vector<ObjectId> written;
    runtime.changed_objects(cursor, written);
    require(written.empty(), "Evaluation wrote the graph or its nodes.");
    expect(evaluator, {{grid_node, {1, 0}}, {moved, {1, 0}}, {line_node, {1, 0}}, {merged, {1, 0}}, {copied, {1, 0}}},
           "The first evaluation did not run each node once.");
    require(same_instances(evaluator.output(), expected_output({1, 0, 0}, {1, 1, 1}, 2, cube)),
            "The graph output differs from direct calls.");
    require(!evaluator.status(moved)->time, "Node timing was enabled by default.");
    take(evaluator.evaluate(true));
    expect(evaluator, {{grid_node, {1, 0}}, {moved, {1, 0}}, {line_node, {1, 0}}, {merged, {1, 0}}, {copied, {1, 0}}},
           "An unchanged graph was evaluated again.");
    require(!evaluator.status(moved)->time, "Enabling timing measured an idle node.");

    // A parameter write reruns the node and what reads it; undoing it finds the cached results.
    write_vector(moved, translate, {2, 0, 0});
    take(evaluator.evaluate(true));
    expect(evaluator, {{grid_node, {1, 0}}, {line_node, {1, 0}}, {moved, {2, 0}}, {merged, {2, 0}}, {copied, {2, 0}}},
           "A Transform write did not rerun exactly Transform and its downstream nodes.");
    const auto moved_time = evaluator.status(moved)->time;
    require(moved_time && !evaluator.status(grid_node)->time, "Timing did not follow actual node executions.");
    require(same_instances(evaluator.output(), expected_output({2, 0, 0}, {1, 1, 1}, 2, cube)),
            "The rerun output is wrong.");
    write_vector(moved, translate, {1, 0, 0});
    take(evaluator.evaluate());
    expect(evaluator, {{grid_node, {1, 0}}, {line_node, {1, 0}}, {moved, {2, 1}}, {merged, {2, 1}}, {copied, {2, 1}}},
           "Undoing a write did not reuse the cached results.");
    require(evaluator.status(moved)->time == moved_time, "A cache hit replaced the last actual run's timing.");
    require(same_instances(evaluator.output(), expected_output({1, 0, 0}, {1, 1, 1}, 2, cube)),
            "The undone output is wrong.");

    // A writable borrow that changes nothing stamps the node, but its key filters it out.
    cursor = take(runtime.change_cursor());
    take(runtime.with_native<Transform>(moved, [](Transform&) -> Result<void> { return {}; }));
    require(runtime.changed_since(moved, cursor), "A writable borrow did not stamp the node.");
    take(evaluator.evaluate());
    expect(evaluator, {{moved, {2, 1}}, {merged, {2, 1}}, {copied, {2, 1}}},
           "An unchanged stamped node was evaluated.");
    take(runtime.with_native<Transform>(moved, [](Transform& node) -> Result<void> {
        node.scale = {1, 2, 1};
        return {};
    }));
    take(evaluator.evaluate());
    expect(evaluator, {{grid_node, {1, 0}}, {moved, {3, 1}}, {merged, {3, 1}}, {copied, {3, 1}}},
           "A native write was not evaluated.");
    require(!evaluator.status(moved)->time, "An unmeasured new run retained obsolete timing.");
    take(runtime.write(line_node, field(runtime, type<Line>(runtime), "count"), Scalar{std::uint32_t{3}}));
    take(evaluator.evaluate());
    expect(evaluator, {{grid_node, {1, 0}}, {moved, {3, 1}}, {line_node, {2, 0}}, {merged, {4, 1}}, {copied, {4, 1}}},
           "A write on one branch reran the other branch.");
    require(same_instances(evaluator.output(), expected_output({1, 0, 0}, {1, 2, 1}, 3, cube)),
            "The two-branch output is wrong.");
    // The graph seed reaches only nodes that take the context.
    take(runtime.write(graph, field(runtime, type<Graph>(runtime), "seed"), Scalar{std::uint32_t{9}}));
    take(evaluator.evaluate(true));
    expect(evaluator, {{grid_node, {1, 0}}, {moved, {3, 1}}, {line_node, {2, 0}}, {merged, {4, 1}}, {copied, {5, 1}}},
           "The graph seed did not rerun exactly the nodes taking the context.");
    const auto copied_time = evaluator.status(copied)->time;
    require(copied_time.has_value(), "A timed downstream node has no timing.");

    // A failing node blocks its downstream nodes and keeps the published output; it is not
    // retried until its key changes, and restoring it finds the cache again.
    const auto* shown = evaluator.output();
    write_vector(moved, scale, {std::nanf(""), 1, 1});
    take(evaluator.evaluate(true));
    require(failed(evaluator, moved) && failed(evaluator, merged) && failed(evaluator, copied) &&
                !failed(evaluator, line_node),
            "A failure did not block exactly the downstream nodes.");
    require(evaluator.output() == shown, "A failure replaced the published output.");
    expect(evaluator, {{moved, {4, 1}}, {merged, {4, 1}}, {copied, {5, 1}}}, "Blocked nodes were evaluated.");
    const auto failed_time = evaluator.status(moved)->time;
    require(failed_time && evaluator.status(copied)->time == copied_time,
            "Failure timing changed a blocked node's timing.");
    take(runtime.write(line_node, field(runtime, type<Line>(runtime), "count"), Scalar{std::uint32_t{4}}));
    take(evaluator.evaluate());
    expect(evaluator, {{moved, {4, 1}}, {line_node, {3, 0}}, {merged, {4, 1}}},
           "A failed node was retried with the same key.");
    require(evaluator.status(moved)->time == failed_time, "Suppressing a repeated failure replaced its timing.");
    write_vector(moved, scale, {1, 2, 1});
    take(evaluator.evaluate());
    require(!failed(evaluator, moved) && !failed(evaluator, merged) && !failed(evaluator, copied),
            "Restoring the parameter left a failure.");
    expect(evaluator, {{moved, {4, 2}}, {merged, {5, 1}}, {copied, {6, 1}}},
           "The restored node did not hit the cache.");
    require(same_instances(evaluator.output(), expected_output({1, 0, 0}, {1, 2, 1}, 4, cube)),
            "The recovered output is wrong.");

    // Resources: a new revision that resolves the same mesh reruns nothing; another mesh reruns
    // its readers; a missing mesh fails until it appears.
    ++resources.version;
    take(evaluator.evaluate());
    expect(evaluator, {{copied, {6, 1}}}, "An unchanged resource reran its reader.");
    const auto replacement = std::make_shared<const geometry::Mesh>(
        std::vector<geometry::Vertex>(cube->vertices().begin(), cube->vertices().end()));
    resources.meshes["fixture/a"] = replacement;
    ++resources.version;
    take(evaluator.evaluate());
    expect(evaluator, {{merged, {5, 1}}, {copied, {7, 1}}}, "A changed resource did not rerun exactly its reader.");
    require(same_instances(evaluator.output(), expected_output({1, 0, 0}, {1, 2, 1}, 4, replacement)),
            "The new mesh was not used.");
    take(runtime.with_native<CopyToPoints>(copied, [](CopyToPoints& node) -> Result<void> {
        node.meshes[0] = String{"fixture/later"};
        return {};
    }));
    shown = evaluator.output();
    take(evaluator.evaluate());
    require(failed(evaluator, copied) && evaluator.output() == shown,
            "A missing mesh did not fail and keep the output.");
    resources.meshes["fixture/later"] = cube;
    ++resources.version;
    take(evaluator.evaluate());
    require(!failed(evaluator, copied) && evaluator.output() != shown, "A mesh that appeared was not picked up.");
    expect(evaluator, {{copied, {9, 1}}}, "The failed reader was not retried once.");

    // Results are cached by content, not node: an identical Grid finds the first one's result.
    const auto twin_root = add(type<Grid>(runtime));
    const auto twin = twin_root.id();
    take(runtime.write(twin, field(runtime, type<Grid>(runtime), "size"), ValueInput{vector2, &size}));
    take(runtime.write(twin, field(runtime, type<Grid>(runtime), "rows"), Scalar{std::uint32_t{3}}));
    take(runtime.write(twin, field(runtime, type<Grid>(runtime), "columns"), Scalar{std::uint32_t{4}}));
    take(set_display(runtime, graph, twin));
    take(evaluator.evaluate());
    expect(evaluator, {{twin, {0, 1}}}, "An identical node did not share the cached result.");
    require(evaluator.output()->count(Layer::point) == 12, "The display node was not published.");
    take(set_display(runtime, graph, copied));

    // Edits: cycles, missing ports and foreign nodes fail; a single input replaces its connection.
    reject(connect(runtime, graph, merged, 0, moved, 0), "A cycle was connected.");
    reject(connect(runtime, graph, moved, 0, moved, 0), "A node was connected to itself.");
    reject(connect(runtime, graph, grid_node, 1, moved, 0), "A missing output was connected.");
    reject(connect(runtime, graph, grid_node, 0, moved, 1), "A missing input was connected.");
    const auto other_root = take(runtime.create(type<Graph>(runtime)));
    const auto stranger = take(add_node(runtime, other_root.id(), type<Grid>(runtime)));
    reject(connect(runtime, graph, stranger.id(), 0, moved, 0), "A node of another graph was connected.");
    reject(set_display(runtime, graph, stranger.id()), "A node of another graph was displayed.");
    take(connect(runtime, graph, line_node, 0, moved, 0));
    require(take(inputs(runtime, moved)) == std::vector<Input>{{line_node, 0, 0}},
            "A single input kept its old connection.");
    take(connect(runtime, graph, grid_node, 0, moved, 0));
    take(disconnect(runtime, line_node, 0, merged, 0));
    require(take(inputs(runtime, merged)) == std::vector<Input>{{moved, 0, 0}},
            "Disconnecting removed the wrong connection.");
    reject(disconnect(runtime, line_node, 0, merged, 0), "A missing connection was disconnected.");
    take(evaluator.evaluate());
    auto alone = take(grid({3, 2}, 3, 4));
    take(transform(alone, {1, 0, 0}, {}, {1, 2, 1}));
    const std::vector<Reference> later{{String{"fixture/later"}, cube}};
    require(same_instances(evaluator.output(), take(copy_to_points(take(Geometry::merge({&alone, 1})), later))),
            "The output after disconnecting is wrong.");

    // Removing a node disconnects its outputs and clears the display.
    take(remove_node(runtime, graph, grid_node));
    require(take(inputs(runtime, moved)).empty(), "Removing a node left a connection to it.");
    take(remove_node(runtime, graph, copied));
    require(take(contents(runtime, graph)).display == ObjectId{}, "Removing the display node left it displayed.");
    take(evaluator.evaluate());
    require(evaluator.output() == nullptr && evaluator.status(copied) == nullptr,
            "A removed display node is still evaluated.");
    take(set_display(runtime, graph, moved));
    take(evaluator.evaluate());
    require(evaluator.output() && evaluator.output()->count(Layer::point) == 0,
            "An unconnected input was not empty geometry.");

    // A zero budget caches nothing, so undoing a write evaluates again.
    Evaluator uncached{runtime, graph_root, resources, nullptr, 0};
    take(uncached.evaluate());
    write_vector(moved, translate, {5, 0, 0});
    take(uncached.evaluate());
    write_vector(moved, translate, {1, 0, 0});
    take(uncached.evaluate());
    expect(uncached, {{moved, {3, 0}}}, "A zero cache budget kept results.");
}

std::string replaced(std::string text, std::string_view from, std::string_view to) {
    const auto at = text.find(from);
    require(at != std::string::npos, "A fixture edit did not find its text.");
    return text.replace(at, from.size(), to);
}

// The engine's Source files, as every project gets them, plus fixtures; files are edited by
// snapshot path and versioned by content, like project files.
struct Project {
    std::map<std::string, std::string> files;
    std::uint64_t version{};
    Project() {
        for (auto& file : take(source::engine_sources(source::default_engine_root())))
            files[file.path] = std::move(file.source);
    }
    const std::string& text(const std::string& path) const {
        const auto found = files.find(path);
        require(found != files.end(), "An engine Source file is missing.");
        return found->second;
    }
    void publish(source::Program& program) {
        source::SourceSnapshot snapshot{"procedural", ++version, {}};
        for (const auto& [path, text] : files)
            snapshot.files.push_back({path, source::content_hash(text), text});
        auto candidate = take(source::Program::prepare(take(program.request(std::move(snapshot)))));
        take(program.try_publish(candidate));
    }
};

// Beside the engine's nodes: internal random entry wrappers, parameter/context consumers,
// multi-column writes and failures in binding, preparation and execution.
constexpr std::string_view fixture_source = R"(
uint test_hash(uint x) { return procedural.hash(x); }
uint test_hash2(uint a, uint b) { return procedural.hash2(a, b); }
float test_unit(uint x) { return procedural.unit(x); }
class fixture.Lift : procedural.PointNode {
    float height = 1.0f;
    void set_height(float value) { self.height = value; }
    void evaluate(Array<vector3>& P) {
        const float lift = self.height;
        for (uint i = 0; i < P.size(); i = i + 1) { P[i] = P[i] + vector3(0.0f, lift, 0.0f); }
    }
}
class fixture.Shift : procedural.PointNode {
    void evaluate(const procedural.Context& context, Array<vector3>& P) {
        const float shift = float(context.seed);
        for (uint i = 0; i < P.size(); i = i + 1) { P[i] = P[i] + vector3(shift, 0.0f, 0.0f); }
    }
}
class fixture.Counter : procedural.PointNode {
    uint count = 0;
    void evaluate() { self.count = self.count + 1; }
}
class fixture.ZeroEven : procedural.PointNode {
    void evaluate(Array<vector3>& N) {
        for (uint i = 0; i < N.size(); i = i + 2) { N[i] = vector3(); }
    }
}
class fixture.ByValue : procedural.PointNode {
    void evaluate(float x) { }
}
class fixture.WrongColumn : procedural.PointNode {
    void evaluate(Array<float>& fresh, Array<float>& P) { }
}
class fixture.MissingAfterWrite : procedural.PointNode {
    void evaluate(Array<float>& fresh, const Array<float>& missing) { }
}
class fixture.ColumnBatch : procedural.PointNode {
    float offset = 2.0f;
    bool fail = false;
    void evaluate(const Array<vector3>& P, Array<vector3>& N,
        Array<float>& a, Array<float>& b, Array<float>& c, Array<float>& d,
        Array<float>& e, Array<float>& f, Array<float>& g, Array<float>& h) {
        const float offset = self.offset;
        for (uint i = 0; i < P.size(); i = i + 1) {
            N[i] = vector3(offset, P[i].z, -P[i].x);
            a[i] = P[i].x + offset;
            b[i] = a[i] + 1.0f;
            c[i] = b[i] + 1.0f;
            d[i] = c[i] + 1.0f;
            e[i] = d[i] + 1.0f;
            f[i] = e[i] + 1.0f;
            g[i] = f[i] + 1.0f;
            h[i] = g[i] + 1.0f;
        }
        if (self.fail) { a[P.size()] = 1.0f; }
    }
}
class fixture.Unbound : procedural.PointNode {
    float value = 0.0f;
}
)";

Vector3 random_vector(std::uint32_t x) {
    const auto a = hash(x);
    const auto b = hash(a);
    const auto c = hash(b);
    return Vector3{unit(a), unit(b), unit(c)} * 2.f - Vector3{1, 1, 1};
}

// Positions and pscale agree within float rounding, so a differently contracted product passes
// and a wrong hash, seed or parameter does not.
bool same_points(const Geometry* actual, const Geometry& expected) {
    require(actual != nullptr, "The graph has no output.");
    const auto count = expected.count(Layer::point);
    if (actual->count(Layer::point) != count)
        return false;
    const auto& scales = column<float>(*actual, "pscale", Layer::point);
    const auto& wanted = column<float>(expected, "pscale", Layer::point);
    for (std::uint32_t i = 0; i < count; ++i)
        if (!near(actual->positions()[i], expected.positions()[i], 1e-6f) || !near(scales[i], wanted[i], 1e-6f))
            return false;
    return true;
}

// Source PointNode types from the engine's files and a fixture snapshot: results match the C++
// formulas, a parameter written by Runtime::write or by Source reruns only that node and its
// downstream nodes, a compatible publication reruns only nodes of the types whose code changed,
// and unbindable types fail without blocking anything else.
void source_nodes() {
    Runtime runtime;
    take(register_types(runtime));
    Project project;
    const std::string jitter_path = "engine/procedural/jitter.source";
    const std::string random_path = "engine/procedural/random.source";
    const std::string scale_path = "engine/procedural/random_scale.source";
    const auto jitter_text = project.text(jitter_path);
    const auto random_text = project.text(random_path);
    const auto scale_text = project.text(scale_path);
    project.files["fixture.source"] = std::string{fixture_source};
    source::Program program{runtime};
    project.publish(program);
    const auto source_type = [&](std::string_view name) {
        const auto classes = program.classes();
        const auto found = std::ranges::find(classes, name, &source::ClassInfo::name);
        require(found != classes.end(), "A Source node class was not published.");
        return found->type;
    };
    const auto jitter_type = source_type("procedural.Jitter");
    const auto scale_type = source_type("procedural.RandomScale");
    const auto lift_type = source_type("fixture.Lift");
    const auto shift_type = source_type("fixture.Shift");
    const auto point_ports = take(ports(runtime, jitter_type));
    require(point_ports.evaluate == MethodId{} && point_ports.parameters.empty() &&
                point_ports.inputs == std::vector<std::string>{"input"} &&
                point_ports.multiple == std::vector<bool>{false} &&
                point_ports.outputs == std::vector<std::string>{"output"},
            "A PointNode type did not get the fixed ports.");
    reject(ports(runtime, type<PointNode>(runtime)), "The PointNode base was accepted as a node.");
    // Source draws exactly the C++ values: the same integer hashes, and units below 1 even for
    // the largest top 24 bits.
    const auto source_hash = take(program.entry("test_hash"));
    const auto source_hash2 = take(program.entry("test_hash2"));
    const auto source_unit = take(program.entry("test_unit"));
    const auto call = [](const source::Program::Entry& entry, auto... values) {
        return *take(entry.invoke({}, std::array{Scalar{values}...}));
    };
    for (const std::uint32_t x : {0u, 1u, 0x9e3779b9u, 0xffffffffu})
        require(std::get<std::uint32_t>(call(source_hash, x)) == hash(x) &&
                    std::get<std::uint32_t>(call(source_hash2, x, 7u)) == hash(x, 7u),
                "The Source hash differs from random.hpp.");
    for (const std::uint32_t x : {0u, 0x100u, 0x80000000u})
        require(std::get<float>(call(source_unit, x)) == static_cast<float>(x >> 8) * 0x1p-24f,
                "The Source unit differs from random.hpp.");
    require(std::get<float>(call(source_unit, 0xffffffffu)) == 0x1.fffffep-1f, "The Source unit reached 1.");

    Meshes resources;
    resources.meshes["fixture/a"] = geometry::cube_mesh();
    const auto graph_root = take(runtime.create(type<Graph>(runtime)));
    const auto graph = graph_root.id();
    std::vector<ObjectRoot> roots;
    const auto add = [&](ObjectId owner, TypeId node_type) {
        return roots.emplace_back(take(add_node(runtime, owner, node_type))).id();
    };
    const auto imported = add(graph, type<MeshImport>(runtime));
    const auto scattered = add(graph, type<Scatter>(runtime));
    const auto grid_node = add(graph, type<Grid>(runtime));
    const auto jittered = add(graph, jitter_type);
    const auto twin = add(graph, jitter_type);
    const auto scaled = add(graph, scale_type);
    const auto lifted = add(graph, lift_type);
    const auto shifted = add(graph, shift_type);
    const auto merged = add(graph, type<Merge>(runtime));
    take(runtime.with_native<MeshImport>(imported, [](MeshImport& node) -> Result<void> {
        node.mesh = String{"fixture/a"};
        return {};
    }));
    take(runtime.write(scattered, field(runtime, type<Scatter>(runtime), "density"), Scalar{4.f}));
    take(runtime.write(scattered, field(runtime, type<Scatter>(runtime), "seed"), Scalar{std::uint32_t{3}}));
    const auto amount = field(runtime, jitter_type, "amount");
    take(runtime.write(jittered, amount, Scalar{0.25f}));
    take(runtime.write(twin, amount, Scalar{0.5f}));
    // Unequal axes, one of them zero, show Jitter scales each axis by its own component.
    const Vector3 axes{1, 0, 2};
    take(runtime.write(jittered, field(runtime, jitter_type, "axis_scale"), ValueInput{type<Vector3>(runtime), &axes}));
    take(connect(runtime, graph, imported, 0, scattered, 0));
    for (const auto node : {jittered, twin, scaled})
        take(connect(runtime, graph, scattered, 0, node, 0));
    for (const auto node : {lifted, shifted})
        take(connect(runtime, graph, grid_node, 0, node, 0));
    for (const auto node : {jittered, twin, scaled, lifted, shifted})
        take(connect(runtime, graph, node, 0, merged, 0));
    take(set_display(runtime, graph, merged));

    // The same graph by direct calls and the documented hash; `factor` scales Jitter's offsets.
    struct Settings {
        std::uint32_t seed{};
        float amount{0.25f};
        float height{1.f};
        float factor{1.f};
    };
    const auto expected = [&](Settings settings) {
        const auto points =
            take(scatter(import_mesh(*geometry::cube_mesh()), {.density = 4, .seed = hash(settings.seed, 3u)}));
        const auto& ids = column<std::uint32_t>(points, "id", Layer::point);
        const auto node_seed = hash(settings.seed, 0u);
        const auto jitter = [&](float scale, Vector3 axis) {
            auto moved = points;
            auto& positions = *take(moved.write<Vector3>("P", Layer::point));
            const auto reach = axis * (scale * settings.factor);
            for (std::uint32_t i = 0; i < positions.size(); ++i) {
                const auto r = random_vector(hash(node_seed, ids[i]));
                positions[i] = positions[i] + Vector3{r.x * reach.x, r.y * reach.y, r.z * reach.z};
            }
            return moved;
        };
        auto sized = points;
        auto& scales = *take(sized.add<float>("pscale", Layer::point));
        for (std::uint32_t i = 0; i < scales.size(); ++i) {
            const float t = unit(hash(node_seed, ids[i]));
            scales[i] = 0.5f * (1.f - t) + 1.5f * t;
        }
        auto raised = take(grid({10, 10}, 10, 10));
        take(transform(raised, {0, settings.height, 0}, {}, {1, 1, 1}));
        auto moved = take(grid({10, 10}, 10, 10));
        take(transform(moved, {static_cast<float>(settings.seed), 0, 0}, {}, {1, 1, 1}));
        const std::array parts{jitter(settings.amount, axes), jitter(0.5f, {1, 1, 1}), sized, raised, moved};
        return take(Geometry::merge(parts));
    };

    Evaluator evaluator{runtime, graph_root, resources, &program};
    auto cursor = take(runtime.change_cursor());
    take(evaluator.evaluate());
    std::vector<ObjectId> written;
    runtime.changed_objects(cursor, written);
    require(written.empty(), "Evaluating Source nodes wrote the graph or its nodes.");
    expect(evaluator,
           {{imported, {1, 0}},
            {scattered, {1, 0}},
            {grid_node, {1, 0}},
            {jittered, {1, 0}},
            {twin, {1, 0}},
            {scaled, {1, 0}},
            {lifted, {1, 0}},
            {shifted, {1, 0}},
            {merged, {1, 0}}},
           "The first evaluation did not run each node once.");
    // RandomScale reads the P that Jitter writes in a sibling; equal positions show Jitter wrote a copy.
    require(same_points(evaluator.output(), expected({})), "Source nodes differ from the documented formulas.");

    // A Runtime write, and a Source method writing self, rerun the node and its downstream nodes;
    // undoing them finds the cache.
    take(runtime.write(jittered, amount, Scalar{0.75f}));
    take(evaluator.evaluate());
    expect(evaluator,
           {{scattered, {1, 0}},
            {twin, {1, 0}},
            {scaled, {1, 0}},
            {lifted, {1, 0}},
            {shifted, {1, 0}},
            {jittered, {2, 0}},
            {merged, {2, 0}}},
           "A Runtime write on a Source node did not rerun exactly it and its downstream nodes.");
    require(same_points(evaluator.output(), expected({.amount = 0.75f})), "The rewritten Jitter output is wrong.");
    take(runtime.write(jittered, amount, Scalar{0.25f}));
    take(evaluator.evaluate());
    expect(evaluator, {{jittered, {2, 1}}, {merged, {2, 1}}},
           "Undoing a Source node parameter did not reuse the cache.");
    const auto set_height = take(program.entry("fixture.Lift.set_height"));
    cursor = take(runtime.change_cursor());
    (void)take(set_height.invoke(lifted, std::array{Scalar{2.f}}));
    require(runtime.changed_since(lifted, cursor), "A Source write did not stamp the node.");
    take(evaluator.evaluate());
    expect(evaluator, {{grid_node, {1, 0}}, {jittered, {2, 1}}, {shifted, {1, 0}}, {lifted, {2, 0}}, {merged, {3, 1}}},
           "A Source write did not rerun exactly the node and its downstream nodes.");
    require(same_points(evaluator.output(), expected({.height = 2.f})), "The Source-written Lift output is wrong.");
    (void)take(set_height.invoke(lifted, std::array{Scalar{1.f}}));
    take(evaluator.evaluate());
    expect(evaluator, {{lifted, {2, 1}}, {merged, {3, 2}}}, "Undoing a Source write did not reuse the cache.");

    // The graph seed reaches Source nodes through the context; Lift takes none. Shift's input does not
    // depend on the seed, so only its own key can rerun it.
    take(runtime.write(graph, field(runtime, type<Graph>(runtime), "seed"), Scalar{std::uint32_t{9}}));
    take(evaluator.evaluate());
    expect(evaluator,
           {{grid_node, {1, 0}},
            {lifted, {2, 1}},
            {scattered, {2, 0}},
            {jittered, {3, 1}},
            {scaled, {2, 0}},
            {shifted, {2, 0}},
            {merged, {4, 2}}},
           "The graph seed did not rerun exactly the nodes reading it.");
    require(same_points(evaluator.output(), expected({.seed = 9})), "Source nodes did not read the graph seed.");

    // Publications: Jitter's new body reruns both Jitter nodes only; unchanged code reruns nothing;
    // RandomScale's whitespace edit reruns it only; a helper edit reruns only the nodes whose code
    // reaches it, directly or through other helpers: random_vector only Jitter, hash also
    // RandomScale, neither Lift nor Shift.
    project.files[jitter_path] =
        replaced(jitter_text, "self.axis_scale * self.amount;", "self.axis_scale * (self.amount * 2.0f);");
    project.publish(program);
    take(evaluator.evaluate());
    expect(evaluator,
           {{scattered, {2, 0}},
            {scaled, {2, 0}},
            {lifted, {2, 1}},
            {shifted, {2, 0}},
            {jittered, {4, 1}},
            {twin, {3, 0}},
            {merged, {5, 2}}},
           "A Jitter publication did not rerun exactly the Jitter nodes and their downstream nodes.");
    require(same_points(evaluator.output(), expected({.seed = 9, .factor = 2.f})),
            "The published Jitter body was not used.");
    project.publish(program);
    take(evaluator.evaluate());
    expect(
        evaluator,
        {{jittered, {4, 1}}, {twin, {3, 0}}, {scaled, {2, 0}}, {lifted, {2, 1}}, {shifted, {2, 0}}, {merged, {5, 2}}},
        "A publication without code changes reran a node.");
    project.files[scale_path] = replaced(scale_text, "const float low", "const  float low");
    project.publish(program);
    take(evaluator.evaluate());
    expect(
        evaluator,
        {{jittered, {4, 1}}, {twin, {3, 0}}, {lifted, {2, 1}}, {shifted, {2, 0}}, {scaled, {3, 0}}, {merged, {6, 2}}},
        "A RandomScale publication did not rerun exactly RandomScale and its downstream nodes.");
    project.files[random_path] =
        replaced(random_text, "const uint a = procedural.hash(x);", "const uint a = procedural.hash(x + 0);");
    project.publish(program);
    take(evaluator.evaluate());
    expect(
        evaluator,
        {{jittered, {5, 1}}, {twin, {4, 0}}, {scaled, {3, 0}}, {lifted, {2, 1}}, {shifted, {2, 0}}, {merged, {7, 2}}},
        "A random_vector publication did not rerun exactly the Jitter nodes, which call it, and their downstream nodes.");
    project.files[random_path] = replaced(project.files[random_path], "uint x = value;", "uint x = value + 0;");
    project.publish(program);
    take(evaluator.evaluate());
    expect(evaluator,
           {{scattered, {2, 0}},
            {grid_node, {1, 0}},
            {jittered, {6, 1}},
            {twin, {5, 0}},
            {scaled, {4, 0}},
            {lifted, {2, 1}},
            {shifted, {2, 0}},
            {merged, {8, 2}}},
           "A hash publication did not rerun exactly the nodes reaching hash and their downstream nodes.");
    require(same_points(evaluator.output(), expected({.seed = 9, .factor = 2.f})),
            "An equivalent helper changed the output.");

    // NormalAlign writes the orient Copy to Points would derive from N and an up column. On a cube
    // every face normal is axis-aligned, so the up vectors reach the frame's four quaternion
    // branches, the parallel fallback and the half turn; ZeroEven adds zero normals.
    const auto aligned_root = take(runtime.create(type<Graph>(runtime)));
    const auto aligned = aligned_root.id();
    const auto surface = add(aligned, type<MeshImport>(runtime));
    const auto spread = add(aligned, type<Scatter>(runtime));
    const auto zeroed = add(aligned, source_type("fixture.ZeroEven"));
    const auto oriented = add(aligned, source_type("procedural.NormalAlign"));
    const auto copies = add(aligned, type<CopyToPoints>(runtime));
    take(runtime.with_native<MeshImport>(surface, [](MeshImport& node) -> Result<void> {
        node.mesh = String{"fixture/a"};
        return {};
    }));
    take(runtime.with_native<CopyToPoints>(copies, [](CopyToPoints& node) -> Result<void> {
        node.meshes.add(String{"fixture/a"});
        return {};
    }));
    take(runtime.write(spread, field(runtime, type<Scatter>(runtime), "density"), Scalar{4.f}));
    const std::array chain{surface, spread, zeroed, oriented, copies};
    for (std::size_t i = 1; i < chain.size(); ++i)
        take(connect(runtime, aligned, chain[i - 1], 0, chain[i], 0));
    take(set_display(runtime, aligned, copies));
    Evaluator aligning{runtime, aligned_root, resources, &program};
    const auto up = field(runtime, source_type("procedural.NormalAlign"), "up");
    for (const Vector3 upward : {Vector3{0, 1, 0}, Vector3{0, 0, 1}, Vector3{0, -1, 0}, Vector3{}}) {
        take(runtime.write(oriented, up, ValueInput{type<Vector3>(runtime), &upward}));
        take(aligning.evaluate());
        auto points = take(scatter(import_mesh(*geometry::cube_mesh()), {.density = 4, .seed = hash(0u, 0u)}));
        auto& normals = *take(points.write<Vector3>("N", Layer::point));
        for (std::uint32_t i = 0; i < normals.size(); i += 2)
            normals[i] = {};
        take(points.add<Vector3>("up", Layer::point, upward));
        const std::vector<Reference> references{{String{"fixture/a"}, geometry::cube_mesh()}};
        const auto wanted = take(copy_to_points(points, references));
        const auto* actual = aligning.output();
        require(actual && actual->instance_transforms().size() == 24 &&
                    actual->instance_transforms().size() == wanted.instance_transforms().size(),
                "NormalAlign changed the instance count.");
        for (std::size_t i = 0; i < wanted.instance_transforms().size(); ++i)
            for (std::size_t e = 0; e < 16; ++e)
                require(near(actual->instance_transforms()[i][e], wanted.instance_transforms()[i][e], 1e-5f),
                        "NormalAlign differs from the orientation Copy to Points derives.");
    }
    expect(aligning, {{spread, {1, 0}}, {zeroed, {1, 0}}, {oriented, {4, 0}}, {copies, {4, 0}}},
           "An up write did not rerun exactly NormalAlign and its downstream node.");

    // Types that cannot bind, or input without the columns a node reads, fail only themselves.
    const auto faults_root = take(runtime.create(type<Graph>(runtime)));
    const auto faults = faults_root.id();
    const auto points = add(faults, type<Grid>(runtime));
    const auto unread = add(faults, jitter_type);
    const auto by_value = add(faults, source_type("fixture.ByValue"));
    const auto retyped = add(faults, source_type("fixture.WrongColumn"));
    const auto unbound = add(faults, source_type("fixture.Unbound"));
    const auto counter = add(faults, source_type("fixture.Counter"));
    const auto missing_after_write = add(faults, source_type("fixture.MissingAfterWrite"));
    const auto gathered = add(faults, type<Merge>(runtime));
    for (const auto node : {unread, by_value, retyped, unbound, counter, missing_after_write}) {
        take(connect(runtime, faults, points, 0, node, 0));
        take(connect(runtime, faults, node, 0, gathered, 0));
    }
    take(set_display(runtime, faults, gathered));
    Evaluator checking{runtime, faults_root, resources, &program};
    take(checking.evaluate());
    const auto failure_of = [&](const Evaluator& owner, ObjectId node, ErrorCode code, std::string_view text) {
        const auto& error = owner.status(node)->error;
        return error && error->code == code && error->message.find(text) != std::string::npos;
    };
    require(failure_of(checking, unread, ErrorCode::argument_mismatch, "no point attribute id"),
            "A missing read column did not fail.");
    require(failure_of(checking, by_value, ErrorCode::invalid_definition, "parameter x"),
            "A by-value parameter was bound.");
    require(failure_of(checking, retyped, ErrorCode::argument_mismatch, "another layer or type"),
            "A column of another type was bound.");
    require(failure_of(checking, missing_after_write, ErrorCode::argument_mismatch, "no point attribute missing"),
            "A missing column after output preparation did not fail.");
    require(failure_of(checking, unbound, ErrorCode::invalid_definition, "no evaluate"),
            "A type without evaluate was bound.");
    // Without a borrow evaluate is no value region, so it could write self.
    require(failure_of(checking, counter, ErrorCode::invalid_definition, "must borrow"),
            "An evaluate without borrows was bound.");
    require(!failed(checking, points) && failed(checking, gathered) && checking.output() == nullptr,
            "Unbindable nodes did not fail alone and block their downstream node.");
    Evaluator unprogrammed{runtime, graph_root, resources};
    take(unprogrammed.evaluate());
    require(failure_of(unprogrammed, jittered, ErrorCode::unsupported_operation, "needs a program") &&
                !failed(unprogrammed, scattered),
            "A Source node ran without a program.");

    // Eight new columns grow the two-attribute input while N already needs a writable borrow.
    // A bounds failure occurs inside invoke, after writing every column, not during preparation.
    const auto batch_root = take(runtime.create(type<Graph>(runtime)));
    const auto batch_graph = batch_root.id();
    const auto batch_input = add(batch_graph, type<Grid>(runtime));
    const auto batch = add(batch_graph, source_type("fixture.ColumnBatch"));
    const auto sibling = add(batch_graph, lift_type);
    take(runtime.with_native<Grid>(batch_input, [](Grid& node) -> Result<void> {
        node.rows = 2;
        node.columns = 3;
        node.size = {4, 2};
        return {};
    }));
    take(connect(runtime, batch_graph, batch_input, 0, batch, 0));
    take(connect(runtime, batch_graph, batch_input, 0, sibling, 0));
    take(set_display(runtime, batch_graph, batch_input));
    Evaluator batching{runtime, batch_root, resources, &program};
    take(batching.evaluate());
    require(batching.output() != nullptr, "The batch input failed.");
    const Geometry original = *batching.output();
    const auto verify_batch = [](const Geometry* result, float offset) {
        require(result && result->count(Layer::point) == 6 && result->attributes().size() == 10,
                "Batch output lost columns or points.");
        for (std::uint32_t i = 0; i < 6; ++i) {
            const Vector3 p{-2.f + 2.f * float(i % 3), 0, -1.f + 2.f * float(i / 3)};
            require(result->positions()[i] == p &&
                        column<Vector3>(*result, "N", Layer::point)[i] == Vector3{offset, p.z, -p.x},
                    "A batch read or existing writable column is wrong.");
            std::uint32_t index = 0;
            for (const auto* name : {"a", "b", "c", "d", "e", "f", "g", "h"})
                require(column<float>(*result, name, Layer::point)[i] == p.x + offset + float(index++),
                        "A new batch column is wrong.");
        }
    };
    take(set_display(runtime, batch_graph, batch));
    take(batching.evaluate());
    verify_batch(batching.output(), 2);
    take(set_display(runtime, batch_graph, sibling));
    take(batching.evaluate());
    require(batching.output() && batching.output()->attributes().size() == 2, "New columns leaked into a sibling.");
    const Geometry sibling_result = *batching.output();
    take(set_display(runtime, batch_graph, batch));
    take(batching.evaluate());
    const auto* previous = batching.output();
    const auto batch_type = source_type("fixture.ColumnBatch");
    const auto fail = field(runtime, batch_type, "fail");
    const auto offset = field(runtime, batch_type, "offset");
    take(runtime.write(batch, offset, Scalar{9.f}));
    take(runtime.write(batch, fail, Scalar{true}));
    take(batching.evaluate());
    require(failure_of(batching, batch, ErrorCode::invocation_failed, "out of range") && batching.output() == previous,
            "An invoke failure published a partially written Geometry.");
    verify_batch(batching.output(), 2);
    take(batching.evaluate());
    require(batching.status(batch)->evaluations == 2, "An unchanged invoke failure retried.");
    take(runtime.write(batch, fail, Scalar{false}));
    take(batching.evaluate());
    require(!failed(batching, batch) && batching.status(batch)->evaluations == 3,
            "A corrected invoke failure did not recover.");
    verify_batch(batching.output(), 9);
    for (std::uint32_t i = 0; i < 6; ++i) {
        const Vector3 p{-2.f + 2.f * float(i % 3), 0, -1.f + 2.f * float(i / 3)};
        require(original.positions()[i] == p && original.attributes().size() == 2 &&
                    column<Vector3>(original, "N", Layer::point)[i] == Vector3{0, 1, 0} &&
                    sibling_result.positions()[i] == p + Vector3{0, 1, 0} &&
                    column<Vector3>(sibling_result, "N", Layer::point)[i] == Vector3{0, 1, 0},
                "A multi-column write or failed invoke changed the input or sibling.");
    }
}

// A height field mesh over [0, size]^2 with `quads` quads per side, two triangles each.
geometry::MeshRef terrain_mesh(std::uint32_t quads, float size) {
    const auto height = [&](std::uint32_t r, std::uint32_t c) {
        const float x = float(c) * size / float(quads);
        const float z = float(r) * size / float(quads);
        return Vector3{x, std::sin(x * 0.21f) * std::cos(z * 0.13f) * 4.f + 0.05f * x, z};
    };
    std::vector<geometry::Vertex> vertices;
    vertices.reserve(std::size_t{quads} * quads * 6);
    for (std::uint32_t r = 0; r < quads; ++r)
        for (std::uint32_t c = 0; c < quads; ++c)
            for (const auto& corners : {std::array{height(r, c), height(r + 1, c), height(r, c + 1)},
                                        std::array{height(r, c + 1), height(r + 1, c), height(r + 1, c + 1)}}) {
                const auto n = normalized(math::cross(corners[1] - corners[0], corners[2] - corners[0]));
                for (const auto& p : corners)
                    vertices.push_back({{p.x, p.y, p.z}, {n.x, n.y, n.z}});
            }
    return std::make_shared<const geometry::Mesh>(std::move(vertices));
}

// Median milliseconds of `samples` runs after two warmups; `prepare` runs untimed before each.
template <class Prepare, class Run>
double median_ms(int samples, Prepare prepare, Run run) {
    std::vector<double> times;
    for (int i = -2; i < samples; ++i) {
        prepare(i);
        const auto start = std::chrono::steady_clock::now();
        run();
        const auto elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (i >= 0)
            times.push_back(elapsed);
    }
    std::ranges::sort(times);
    return times[times.size() / 2];
}

// Numbers only; not an acceptance check. A typical generator graph over a 32768-triangle
// terrain: Scatter on it and a Grid projected onto it by Ray, merged and copied to ~100k
// instances, then published in 32 m chunks.
void profile(const char* path) {
    const std::filesystem::path output{path};
    require(output.is_absolute() && !std::filesystem::exists(output), "Choose a new absolute profile file.");
    constexpr int samples = 9;
    std::string report = "{\n  \"schema\": \"atelier.procedural.profile.v1\",\n";
#ifdef NDEBUG
    report += "  \"configuration\": \"release\",\n";
#else
    report += "  \"configuration\": \"dev\",\n";
#endif
    const auto number = [&](const char* name, double value) {
        report += "  \"" + std::string{name} + "\": " + std::to_string(value) + ",\n";
    };

    // One million points: the in-graph scale.
    const auto ground = triangles({{0, 0, 0}, {0, 0, 1000}, {1000, 0, 0}, {1000, 0, 0}, {0, 0, 1000}, {1000, 0, 1000}});
    Geometry million;
    number("scatter_1m_ms",
           median_ms(samples, [](int) {}, [&] { million = take(scatter(ground, {.density = 1, .seed = 3})); }));
    number("scatter_1m_points", million.count(Layer::point));
    number("transform_1m_ms", median_ms(
                                  samples, [](int) {},
                                  [&] {
                                      auto moved = million;
                                      take(transform(moved, {1, 2, 3}, axis_angle({0, 1, 0}, 30), {1, 2, 1}));
                                  }));

    const auto terrain = terrain_mesh(128, 64);
    const auto terrain_surface = import_mesh(*terrain);
    std::optional<Bvh> bvh;
    number("bvh_build_ms", median_ms(samples, [](int) {}, [&] { bvh.emplace(take(Bvh::build(terrain_surface))); }));
    number("bvh_triangles", terrain_surface.count(Layer::primitive));
    number("bvh_rays_ms",
           median_ms(
               samples, [](int) {},
               [&] {
                   std::uint32_t hits = 0;
                   for (std::uint32_t r = 0; r < 224; ++r)
                       for (std::uint32_t c = 0; c < 224; ++c)
                           hits += bvh->ray({float(c) * 63.f / 223.f + 0.5f, 50.f, float(r) * 63.f / 223.f + 0.5f},
                                            {0, -1, 0}, 1000.f)
                                       .has_value();
                   require(hits == 224 * 224, "Every profile ray hits the terrain.");
               }));

    Runtime runtime;
    take(register_types(runtime));
    Meshes resources;
    resources.meshes["profile/terrain"] = terrain;
    resources.meshes["profile/rock"] = geometry::cube_mesh();
    const auto graph_root = take(runtime.create(type<Graph>(runtime)));
    const auto graph = graph_root.id();
    const auto add = [&](TypeId node_type) { return take(add_node(runtime, graph, node_type)); };
    const auto imported = add(type<MeshImport>(runtime));
    const auto scattered = add(type<Scatter>(runtime));
    const auto gridded = add(type<Grid>(runtime));
    const auto lifted = add(type<Transform>(runtime));
    const auto cast = add(type<Ray>(runtime));
    const auto merged = add(type<Merge>(runtime));
    const auto copied = add(type<CopyToPoints>(runtime));
    take(runtime.with_native<MeshImport>(imported.id(), [](MeshImport& node) -> Result<void> {
        node.mesh = String{"profile/terrain"};
        return {};
    }));
    take(runtime.with_native<Scatter>(scattered.id(), [](Scatter& node) -> Result<void> {
        node.density = 12.2f;
        return {};
    }));
    take(runtime.with_native<Grid>(gridded.id(), [](Grid& node) -> Result<void> {
        node.size = {63, 63};
        node.rows = node.columns = 224;
        return {};
    }));
    take(runtime.with_native<Transform>(lifted.id(), [](Transform& node) -> Result<void> {
        node.translate = {32, 50, 32};
        return {};
    }));
    take(runtime.with_native<CopyToPoints>(copied.id(), [](CopyToPoints& node) -> Result<void> {
        node.meshes.add(String{"profile/rock"});
        return {};
    }));
    take(connect(runtime, graph, imported.id(), 0, scattered.id(), 0));
    take(connect(runtime, graph, gridded.id(), 0, lifted.id(), 0));
    take(connect(runtime, graph, lifted.id(), 0, cast.id(), 0));
    take(connect(runtime, graph, imported.id(), 0, cast.id(), 1));
    take(connect(runtime, graph, scattered.id(), 0, merged.id(), 0));
    take(connect(runtime, graph, cast.id(), 0, merged.id(), 0));
    take(connect(runtime, graph, merged.id(), 0, copied.id(), 0));
    take(set_display(runtime, graph, copied.id()));

    std::optional<Evaluator> evaluator;
    number("cold_ms", median_ms(
                          samples, [&](int) { evaluator.emplace(runtime, graph_root, resources); },
                          [&] { take(evaluator->evaluate(true)); }));
    const std::pair<const char*, ObjectId> nodes[]{
        {"mesh_import", imported.id()}, {"scatter", scattered.id()}, {"grid", gridded.id()},
        {"transform", lifted.id()},     {"ray", cast.id()},          {"merge", merged.id()},
        {"copy_to_points", copied.id()}};
    for (const auto& [name, node] : nodes) {
        const auto elapsed = evaluator->status(node)->time;
        require(elapsed.has_value(), "The profile node was not timed.");
        number((std::string{"node_"} + name + "_ms").c_str(),
               std::chrono::duration<double, std::milli>(*elapsed).count());
    }
    const auto* result = evaluator->output();
    require(result != nullptr, "The profile graph failed.");
    number("idle_ms", median_ms(samples, [](int) {}, [&] { take(evaluator->evaluate()); }));
    const auto write_seed = [&](std::uint32_t seed) {
        take(runtime.write(scattered.id(), field(runtime, type<Scatter>(runtime), "seed"), Scalar{seed}));
    };
    number("scatter_seed_ms",
           median_ms(
               samples, [&](int i) { write_seed(100 + std::uint32_t(i + 2)); }, [&] { take(evaluator->evaluate()); }));
    number("undo_seed_ms", median_ms(
                               samples, [&](int i) { write_seed(100 + std::uint32_t((i + 2) % samples)); },
                               [&] { take(evaluator->evaluate()); }));
    const auto distance = field(runtime, type<Ray>(runtime), "max_distance");
    number("ray_distance_ms",
           median_ms(
               samples, [&](int i) { take(runtime.write(cast.id(), distance, Scalar{200.f + float(i)})); },
               [&] { take(evaluator->evaluate()); }));
    number("copy_variant_ms",
           median_ms(
               samples,
               [&](int i) {
                   take(runtime.with_native<CopyToPoints>(copied.id(), [&](CopyToPoints& node) -> Result<void> {
                       node.variant = String{StringView{"v" + std::to_string(i + 2)}};
                       return {};
                   }));
               },
               [&] { take(evaluator->evaluate()); }));
    const auto* published_from = evaluator->output();
    Publication first;
    number("publish_ms", median_ms(samples, [](int) {}, [&] { first = take(publish(*published_from, 32)); }));
    number("republish_ms", median_ms(samples, [](int) {}, [&] { (void)take(publish(*published_from, 32, &first)); }));
    number("instances", published_from->count(Layer::instance));
    number("collections", double(first.collections.size()));
    report.erase(report.size() - 2);
    report += "\n}\n";
    std::ofstream file{output, std::ios::binary};
    file << report;
    require(static_cast<bool>(file), "The profile file cannot be written.");
    std::fputs(report.c_str(), stdout);
}

}

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view{argv[1]} == "--profile-procedural") {
            profile(argv[2]);
            return 0;
        }
        require(argc == 1, "Usage: atelier_procedural_acceptance [--profile-procedural new-output.json]");
        scatter_counts_and_stability();
        transform_by_usage();
        removal_merge_and_promotion();
        copy_to_points_and_publication();
        bvh_queries();
        ray_node();
        record_parameter_keys();
        graph_evaluation();
        source_nodes();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "procedural acceptance failed: %s\n", error.what());
        return 1;
    }
    std::puts("procedural acceptance passed");
    return 0;
}
