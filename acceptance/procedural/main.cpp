#include "../helpers.hpp"
#include "procedural/bvh.hpp"
#include "procedural/evaluator.hpp"
#include "procedural/instances.hpp"
#include "procedural/nodes.hpp"
#include "procedural/operations.hpp"
#include "procedural/timing.hpp"
#include "runtime/native.hpp"
#include "source/program.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>

namespace {
using namespace atelier;

using namespace atelier::procedural;

using math::Matrix4;

using math::Quaternion;

using math::Vector2;

using math::Vector3;

using namespace atelier::acceptance::helpers;

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

void convex_surface() {
    // The old 3 * primitive addressing reads this quad's last corner and yields +Y, not +Z.
    const auto surface = take(Geometry::polygons(
        Array<Vector3>{
            {10, 0, 0}, {10, 2, 0}, {10, 2, 1}, {10, 0, 1}, {0, 0, 2}, {4, 0, 2}, {5, 2, 2}, {2, 4, 2}, {0, 3, 2}},
        Array<std::uint32_t>{0, 1, 2, 3, 4, 5, 6, 7, 8}, Array<std::uint32_t>{0, 4, 9}));
    const auto points = take(scatter(surface, {.density = 2, .seed = 17}));
    const auto& primitives = column<std::uint32_t>(points, "sourceprim", Layer::point);
    const auto& subtriangles = column<std::uint32_t>(points, "sourcetri", Layer::point);
    const auto& ordinals = column<std::uint32_t>(points, "sourceordinal", Layer::point);
    const auto& uv = column<Vector2>(points, "sourceprimuv", Layer::point);
    constexpr std::array<std::array<std::uint32_t, 3>, 3> corners{{{4, 5, 6}, {4, 6, 7}, {4, 7, 8}}};
    std::array<std::uint32_t, 3> counts{};
    for (std::size_t i = 4; i < points.positions().size(); ++i) {
        const auto expected_triangle = i < 12 ? 0u : i < 28 ? 1u : 2u;
        require(primitives[i] == 1 && subtriangles[i] == expected_triangle,
                "Scatter lost the polygon's original source or fan order.");
        const auto j = subtriangles[i];
        require(ordinals[i] == counts[j]++, "Polygon ordinals are not local to each fan triangle.");
        const auto [a, b, c] = corners[j];
        const auto expected = surface.positions()[a] * (1 - uv[i].x - uv[i].y) + surface.positions()[b] * uv[i].x +
                              surface.positions()[c] * uv[i].y;
        require(near(points.positions()[i], expected), "Polygon UVs do not address the first-corner fan.");
    }
    require(points.count(Layer::point) == 34 && counts == std::array{8u, 16u, 6u},
            "The pentagon fan did not retain its areas 4, 8, 3.");
    // Independently evaluated lowbias32 for (seed,p,j,k)=(17,1,1,3): s=0x6d94d8d1,
    // hashes 0x8e53067a / 0x34951c66. Top 24 bits need no reflection; P=(5u+2v,2u+4v,2).
    require(uv[15] == Vector2{9327366.f * 0x1p-24f, 3446044.f * 0x1p-24f} &&
                points.positions()[15] == Vector3{float(53528918.0 * 0x1p-24), float(32438908.0 * 0x1p-24), 2} &&
                column<std::uint32_t>(points, "id", Layer::point)[15] == 0xa400fab0u,
            "The nonzero fan triangle lost its independent random source.");
    const Vector3 origin{7.f / 3, 2, 5};
    const auto bvh = take(Bvh::build(surface));
    const auto hit = bvh.ray(origin, {0, 0, -1}, 10);
    require(hit && hit->primitive == 1 && hit->triangle == 1 && near(hit->distance, 3) && near(hit->uv.x, 1.f / 3) &&
                near(hit->uv.y, 1.f / 3) && near(hit->position, {7.f / 3, 2, 2}),
            "The ray did not hit the middle fan triangle's centroid.");
    Ray ray;
    ray.direction = {0, 0, -1};
    ray.normal = true;
    Geometry projected;
    ray.evaluate(take(Geometry::points(Array<Vector3>{origin})), surface, projected);
    require(column<std::int32_t>(projected, "hitprim", Layer::point)[0] == 1 &&
                column<std::int32_t>(projected, "hittri", Layer::point)[0] == 1 &&
                near(column<Vector3>(projected, "N", Layer::point)[0], {0, 0, 1}),
            "Ray used triangle-only corner addressing on a polygon.");

    const auto polyline =
        take(Geometry::primitives(Array<Vector3>{{12, 1, 0}, {13, 2, 0}, {14, 1, 0}}, Array<std::uint32_t>{0, 1, 2},
                                  Array<std::uint32_t>{0, 3}, Array<PrimitiveKind>{PrimitiveKind::polyline}));
    auto mixed = take(Geometry::merge(std::array{surface, polyline}));
    {
        auto& tags = *take(mixed.add<std::uint32_t>("tag", Layer::vertex));
        for (std::uint32_t i = 0; i < 12; ++i)
            tags[i] = 100 + i;
    }
    const auto original = mixed;
    take(transform(mixed, {}, {}, {-1, 1, 1}));
    constexpr std::array mirror{3u, 2u, 1u, 0u, 8u, 7u, 6u, 5u, 4u, 9u, 10u, 11u};
    require(mixed.primitive(0).kind == PrimitiveKind::polygon && mixed.primitive(1).kind == PrimitiveKind::polygon &&
                mixed.primitive(2).kind == PrimitiveKind::polyline && mixed.primitive(2).count == 3 &&
                near(mixed.positions()[10], {-13, 2, 0}),
            "Merge or mirror lost the open polyline's kind or position.");
    for (std::size_t i = 0; i < mirror.size(); ++i)
        require(mixed.vertex_points()[i] == mirror[i] &&
                    column<std::uint32_t>(mixed, "tag", Layer::vertex)[i] == 100 + mirror[i] &&
                    original.vertex_points()[i] == i &&
                    column<std::uint32_t>(original, "tag", Layer::vertex)[i] == 100 + i,
                "Mirror reversed a line or lost vertex attributes or COW isolation.");
    mixed.reverse_primitives();
    const auto removed_quad = take(mixed.remove_flagged(Layer::primitive, std::array{true, false, false}));
    constexpr std::array reversed{4u, 5u, 6u, 7u, 8u, 11u, 10u, 9u};
    require(removed_quad.count(Layer::primitive) == 2 && removed_quad.primitive(0).first == 0 &&
                removed_quad.primitive(0).count == 5 && removed_quad.primitive(0).kind == PrimitiveKind::polygon &&
                removed_quad.primitive(1).first == 5 && removed_quad.primitive(1).kind == PrimitiveKind::polyline,
            "Primitive removal lost kinds or offsets.");
    for (std::size_t i = 0; i < reversed.size(); ++i)
        require(removed_quad.vertex_points()[i] == reversed[i] &&
                    column<std::uint32_t>(removed_quad, "tag", Layer::vertex)[i] == 100 + reversed[i],
                "Explicit reversal or removal lost the line direction or its vertex attributes.");
    std::array<bool, 12> removed{};
    removed[10] = true;
    const auto removed_point = take(removed_quad.remove_flagged(Layer::point, removed));
    require(removed_point.count(Layer::point) == 11 && removed_point.count(Layer::primitive) == 1 &&
                removed_point.primitive(0).kind == PrimitiveKind::polygon &&
                std::ranges::equal(removed_point.vertex_points(), std::array{4u, 5u, 6u, 7u, 8u}),
            "Removing a line point did not remove its whole primitive.");
}

// Twice the signed area of (a, b, p) in the XY plane.
double side(Vector3 a, Vector3 b, Vector3 p) {
    return double(b.x - a.x) * (p.y - a.y) - double(b.y - a.y) * (p.x - a.x);
}

bool inside(Vector3 p, Vector3 a, Vector3 b, Vector3 c) {
    const double e = 1e-5;
    return p.z == 0 && side(a, b, p) >= -e && side(b, c, p) >= -e && side(c, a, p) >= -e;
}

void scatter_counts_and_spacing() {
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

    // X ~ Binomial(2236, .25): P(492 <= X <= 626) = .999021330268...
    // This is the smallest n giving 99.9% coverage for |X/n - .25| < .03.
    constexpr std::uint32_t seeds = 2236;
    const auto unit = triangles({a0, a1, a2});
    std::uint64_t total = 0;
    for (std::uint32_t seed = 0; seed < seeds; ++seed) {
        const auto count = take(scatter(unit, {.density = 2.25f, .seed = seed})).count(Layer::point);
        require(count == 2 || count == 3, "A fractional count was not rounded to a neighbour.");
        total += count;
    }
    require(std::abs(double(total) / seeds - 2.25) < 0.03, "Probabilistic rounding is biased.");

    const auto wide = triangles({{0, 0, 0}, {4, 0, 0}, {0, 4, 0}});
    const auto spaced = take(scatter(wide, {.density = 4, .seed = 3, .min_distance = 0.3f}));
    const auto& kept = spaced.positions();
    require(kept.size() >= 2, "Minimum distance left no pair to check.");
    for (std::size_t i = 0; i < kept.size(); ++i)
        for (std::size_t j = 0; j < i; ++j)
            require(math::length(kept[i] - kept[j]) >= 0.3f, "Two kept points are closer than the minimum distance.");
}

void transform_by_usage() {
    auto grid_points = take(grid({4, 3}, 2, 3));
    require(grid_points.count(Layer::point) == 6, "A 3 x 2 grid has six points.");
    const auto tilted = normalized({1, 1, 0});
    for (auto& n : *take(grid_points.write<Vector3>("N", Layer::point)))
        n = tilted;
    take(grid_points.add<Vector3>("Cd", Layer::point, {0.2f, 0.4f, 0.6f}));
    take(grid_points.add<Vector3>("v", Layer::point, {1, 0, 1}));
    const Vector3 t{1, 2, 3};
    const Vector3 s{2, 3, 0.5f};
    const auto q = axis_angle({0, 1, 0}, 90);
    take(transform(grid_points, t, q, s));
    const auto& after = grid_points.positions();
    for (std::size_t i = 0; i < 6; ++i) {
        const float x = -2.f + 2.f * float(i % 3);
        const float z = -1.5f + 3.f * float(i / 3);
        require(near(after[i], {1.f + .5f * z, 2, 3.f - 2.f * x}), "A point did not move by S, then R, then T.");
    }
    const Vector3 inverse_scaled{0, 2.f / std::sqrt(13.f), -3.f / std::sqrt(13.f)};
    for (const auto& n : column<Vector3>(grid_points, "N", Layer::point))
        require(near(n, inverse_scaled), "Normals did not take the normalized inverse transpose.");
    for (const auto& v : column<Vector3>(grid_points, "v", Layer::point))
        require(near(v, {.5f, 0, -2}), "Vectors did not take the linear part only.");
    for (const auto& c : column<Vector3>(grid_points, "Cd", Layer::point))
        require(c == Vector3{0.2f, 0.4f, 0.6f}, "Colors moved.");

    // One input triangle supplies the independent corner order and vertex attributes.
    const auto source = geometry::cube_mesh();
    const auto vertices = source->vertices().first(3);
    const geometry::Mesh mesh{std::vector<geometry::Vertex>{vertices.begin(), vertices.end()}};
    const auto triangle = import_mesh(mesh);
    auto mirrored = triangle;
    take(transform(mirrored, {}, {}, {-1, 1, 1}));
    const auto& old_normals = column<Vector3>(triangle, "N", Layer::vertex);
    const auto& new_normals = column<Vector3>(mirrored, "N", Layer::vertex);
    const auto& old_uvs = column<Vector2>(triangle, "uv", Layer::vertex);
    const auto& new_uvs = column<Vector2>(mirrored, "uv", Layer::vertex);
    for (std::uint32_t i = 0; i < 3; ++i) {
        require(mirrored.vertex_points()[i] == triangle.vertex_points()[2 - i], "A mirrored triangle kept its order.");
        require(new_uvs[i] == old_uvs[2 - i], "Vertex attributes did not follow the reversed order.");
        const auto n = old_normals[2 - i];
        require(near(new_normals[i], {-n.x, n.y, n.z}), "Vertex normals were not reflected.");
    }
}

void removal_merge_and_promotion() {
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

    // Mixed topology must stay addressable across empty inputs and deletion to uniform or empty.
    const auto empty = take(Geometry::polygons({}, {}, Array<std::uint32_t>{0}));
    const auto mixed = take(
        Geometry::polygons(Array<Vector3>{{0, 0, 0}, {2, 0, 0}, {0, 0, 1}, {4, 0, 0}, {6, 0, 0}, {6, 0, 2}, {4, 0, 2}},
                           Array<std::uint32_t>{2, 0, 1, 3, 5, 6, 4}, Array<std::uint32_t>{0, 3, 7}));
    require(empty.count(Layer::point) == 0 && empty.count(Layer::primitive) == 0 && empty.vertex_points().empty() &&
                empty.uniform_primitive_size() == 0,
            "Zero polygons retained topology.");
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
                no_faces.vertex_points().empty(),
            "Deleting all polygons lost their points or retained topology.");
    const auto combined = take(Geometry::merge(std::array{empty, mixed, quad, empty}));
    require(
        combined.count(Layer::point) == 14 && combined.count(Layer::primitive) == 3 &&
            combined.count(Layer::vertex) == 11 && combined.uniform_primitive_size() == 0 &&
            combined.primitive(1).first == 3 && combined.primitive(1).count == 4 && combined.primitive(2).first == 7 &&
            combined.primitive(2).count == 4 &&
            std::ranges::equal(combined.vertex_points(), std::array{2u, 0u, 1u, 3u, 5u, 6u, 4u, 10u, 12u, 13u, 11u}),
        "Merging mixed and empty topology lost ranges or point offsets.");

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
    const auto& weights = column<float>(summed, "weight", Layer::primitive);
    require(weights.size() == 12 && std::ranges::all_of(weights, [](float x) { return x == 4.f; }),
            "Detail did not broadcast its value to primitives.");
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

    auto oriented = take(Geometry::points(Array<Vector3>{{1, 2, 3}}));
    take(oriented.set(String{"orient"}, Layer::point,
                      SharedArray<Quaternion>{Array<Quaternion>{axis_angle({0, 1, 0}, 90)}}));
    take(oriented.set(String{"pscale"}, Layer::point, SharedArray<float>{Array<float>{2}}));
    take(oriented.set(String{"scale"}, Layer::point, SharedArray<Vector3>{Array<Vector3>{{1, 2, 3}}}));
    take(oriented.set(String{"variant"}, Layer::point, SharedArray<std::uint32_t>{Array<std::uint32_t>{1}}));
    take(oriented.add<String>("label", Layer::detail, String{"kept"}));
    const auto instances = take(copy_to_points(oriented, references));
    require(instances.count(Layer::point) == 0 && instances.count(Layer::instance) == 1,
            "Copy to Points made no instances.");
    require(column<String>(instances, "label", Layer::detail)[0] == String{"kept"}, "Detail attributes were dropped.");
    const Matrix4 expected{{0, 0, -2, 0, 0, 4, 0, 0, 6, 0, 0, 0, 1, 2, 3, 1}};
    require(near(instances.instance_transforms()[0], expected), "An instance transform is not S * O * T.");
    require(instances.instance_references()[0] == 1, "Variant did not pick the reference.");

    auto edited_instances = instances;
    auto changed_matrix = expected;
    changed_matrix[12] = 14;
    edited_instances.write_instance_transforms()[0] = changed_matrix;
    require(edited_instances.instance_transforms()[0] == changed_matrix &&
                near(instances.instance_transforms()[0], expected),
            "Writing instance transforms changed the source.");

    // Without orient: N steers +Z and up steers +Y.
    auto with_up = take(Geometry::points(Array<Vector3>{{0, 0, 0}}));
    take(with_up.set(String{"N"}, Layer::point, SharedArray<Vector3>{Array<Vector3>{{3, 0, 0}}}));
    take(with_up.set(String{"up"}, Layer::point, SharedArray<Vector3>{Array<Vector3>{{0, 5, 0}}}));
    const auto rows = [](const Matrix4& m) {
        return std::array<Vector3, 3>{Vector3{m[0], m[1], m[2]}, Vector3{m[4], m[5], m[6]}, Vector3{m[8], m[9], m[10]}};
    };
    const auto up_axes = rows(take(copy_to_points(with_up, references)).instance_transforms()[0]);
    require(near(up_axes[0], {0, 0, -1}) && near(up_axes[1], {0, 1, 0}) && near(up_axes[2], {1, 0, 0}),
            "N and up did not align +Z and +Y.");

    // Publication: independently known floor divisions and affine layout.
    auto scattered = take(Geometry::points(Array<Vector3>{{-0.01f, 0, 0}, {3, 0, 3}, {10, 0, 0}}));
    take(scattered.set(String{"variant"}, Layer::point, SharedArray<std::uint32_t>{Array<std::uint32_t>{0, 1, 0}}));
    take(scattered.set(String{"scale"}, Layer::point,
                       SharedArray<Vector3>{Array<Vector3>{{1, 1, 1}, {2, 2, 2}, {1, 1, 1}}}));
    const auto first = take(publish(take(copy_to_points(scattered, references)), 10));
    std::map<std::pair<std::string, std::array<std::int32_t, 3>>, std::vector<float>> found;
    for (const auto& collection : first.collections) {
        auto& xs = found[{std::string{collection.reference.key.view().std_view()}, collection.chunk}];
        for (const auto& affine : *collection.transforms)
            xs.push_back(affine[9]);
    }
    const std::map<std::pair<std::string, std::array<std::int32_t, 3>>, std::vector<float>> expected_groups{
        {{"fixture/a", {1, 0, 0}}, {10}}, {{"fixture/a", {-1, 0, 0}}, {-0.01f}}, {{"fixture/b", {0, 0, 0}}, {3}}};
    require(found == expected_groups, "Instances were not grouped by reference and chunk.");
    const auto& scaled = first.collections[2].transforms->front(); // fixture/b, the point at 3.
    require(first.collections[2].chunk == std::array<std::int32_t, 3>{0, 0, 0} && scaled[0] == 2 && scaled[4] == 2 &&
                scaled[8] == 2 && scaled[11] == 3,
            "The 3x4 layout is not axes then translation.");
}

void realized_geometry() {
    const Array<Vector3> authored{{0, 0, 0}, {2, 0, 0}, {0, 1, 1}, {1, 0, 2}, {1, 1, 3}, {2, 1, 4}};
    std::array<Geometry, 2> sources;
    sources[0] =
        take(Geometry::primitives(authored, Array<std::uint32_t>{0, 1, 2, 3, 4, 5}, Array<std::uint32_t>{0, 3, 6},
                                  Array<PrimitiveKind>{PrimitiveKind::polygon, PrimitiveKind::polyline}));
    sources[1] = sources[0];
    for (auto& p : *take(sources[1].write<Vector3>("P", Layer::point)))
        p.x += 3;
    for (std::uint32_t s = 0; s < 2; ++s) {
        take(sources[s].add<Vector3>("N", Layer::vertex, {0, -1, 1}));
        take(sources[s].add<Vector3>("v", Layer::point, {1, 2, 3}));
        take(sources[s].add<Quaternion>("orient", Layer::point, {0, 0, 1, 0}));
        take(sources[s].add<float>("weight", Layer::point, float(s + 1)));
        auto& tags = *take(sources[s].add<std::uint32_t>("tag", Layer::vertex));
        for (std::uint32_t i = 0; i < 6; ++i)
            tags[i] = 10 * (s + 1) + i;
    }
    take(
        sources[0].set(String{"material"}, Layer::primitive, SharedArray<std::uint32_t>{Array<std::uint32_t>{31, 32}}));
    take(
        sources[1].set(String{"material"}, Layer::primitive, SharedArray<std::uint32_t>{Array<std::uint32_t>{41, 42}}));
    take(sources[0].add<Vector3>("anchor", Layer::point, {1, 2, 3}, Usage::point));
    take(sources[0].add<String>("label", Layer::detail, String{"port zero"}));
    take(sources[1].add<String>("label", Layer::detail, String{"port one"}));
    take(sources[0].add<Vector3>("pivot", Layer::detail, {100, 101, 102}, Usage::point));

    auto targets = take(Geometry::points(Array<Vector3>{{10, 20, 30}, {-2, 5, 7}, {99, 99, 99}}));
    take(targets.set(String{"chosen"}, Layer::point, SharedArray<bool>{Array<bool>{true, true, false}}));
    take(targets.set(String{"prototype"}, Layer::point, SharedArray<std::uint32_t>{Array<std::uint32_t>{1, 0, 0}}));
    take(
        targets.set(String{"N"}, Layer::point, SharedArray<Vector3>{Array<Vector3>{{3, 0, 0}, {0, 0, -3}, {0, 0, 1}}}));
    take(targets.set(String{"up"}, Layer::point,
                     SharedArray<Vector3>{Array<Vector3>{{0, 4, 0}, {0, -4, 0}, {0, 1, 0}}}));
    take(targets.set(String{"scale"}, Layer::point,
                     SharedArray<Vector3>{Array<Vector3>{{-1, 1.5f, 2}, {1, 2, .5f}, {1, 1, 1}}}));
    take(targets.set(String{"pscale"}, Layer::point, SharedArray<float>{Array<float>{2, 1, 1}}));
    take(targets.set(String{"weight"}, Layer::point, SharedArray<float>{Array<float>{70, 90, 110}}));
    take(targets.add<String>("label", Layer::detail, String{"target loses"}));
    take(targets.add<String>("fallback", Layer::detail, String{"target fills"}));
    const std::array broadcasts{String{"weight"}};
    auto output = take(realize(sources, targets, "chosen", "prototype", broadcasts));
    // Source 1 then 0. The two independent affine formulas are
    // (10+4z,20+3y,30+2x) and (-2+x,5-2y,7-z/2), not calls to transform().
    const std::array expected{Vector3{10, 20, 36},  Vector3{10, 20, 40}, Vector3{14, 23, 36},  Vector3{18, 20, 38},
                              Vector3{22, 23, 38},  Vector3{26, 23, 40}, Vector3{-2, 5, 7},    Vector3{0, 5, 7},
                              Vector3{-2, 3, 6.5f}, Vector3{-1, 5, 6},   Vector3{-1, 3, 5.5f}, Vector3{0, 3, 5}};
    constexpr std::array corners{2u, 1u, 0u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u};
    constexpr std::array tags{22u, 21u, 20u, 23u, 24u, 25u, 10u, 11u, 12u, 13u, 14u, 15u};
    require(output.count(Layer::point) == 12 && output.count(Layer::vertex) == 12 &&
                output.count(Layer::primitive) == 4 && output.count(Layer::instance) == 0 &&
                std::ranges::equal(output.vertex_points(), corners) &&
                std::ranges::equal(column<std::uint32_t>(output, "tag", Layer::vertex), tags) &&
                std::ranges::equal(column<std::uint32_t>(output, "material", Layer::primitive),
                                   std::array{41u, 42u, 31u, 32u}),
            "Realize lost source order, mirrored polygon corners, line direction or layered attributes.");
    for (std::uint32_t p = 0; p < 4; ++p)
        require(output.primitive(p).first == 3 * p && output.primitive(p).count == 3 &&
                    output.primitive(p).kind == (p % 2 ? PrimitiveKind::polyline : PrimitiveKind::polygon),
                "Realize confused equal-size lines and polygons.");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const bool first = i < 6;
        const auto normal = first ? Vector3{.6f, -.8f, 0} : Vector3{0, 1, -4} / std::sqrt(17.f);
        const auto q = column<Quaternion>(output, "orient", Layer::point)[i];
        require(
            near(output.positions()[i], expected[i]) && near(column<Vector3>(output, "N", Layer::vertex)[i], normal) &&
                near(column<Vector3>(output, "v", Layer::point)[i],
                     first ? Vector3{12, 6, 2} : Vector3{1, -4, -1.5f}) &&
                near(column<Vector3>(output, "anchor", Layer::point)[i], first ? Vector3{} : Vector3{-1, 1, 5.5f}) &&
                column<float>(output, "weight", Layer::point)[i] == (first ? 70.f : 90.f) &&
                near(q.x, first ? std::sqrt(.5f) : 0.f) && near(q.y, first ? 0.f : -1.f) &&
                near(q.z, first ? std::sqrt(.5f) : 0.f) && near(q.w, 0.f),
            "Realize changed placement, normal/orient composition, target overrides or missing defaults.");
    }
    require(!output.find("chosen") && !output.find("prototype") && !output.find("scale") &&
                column<String>(output, "label", Layer::detail)[0].view() == "port zero" &&
                column<String>(output, "fallback", Layer::detail)[0].view() == "target fills" &&
                column<Vector3>(output, "pivot", Layer::detail)[0] == Vector3{100, 101, 102},
            "Realize implicitly broadcast placement columns or transformed/reordered detail.");
    (*take(output.write<Vector3>("P", Layer::point)))[0] = {};
    (*take(output.write<String>("label", Layer::detail)))[0] = String{"changed output"};
    require(std::ranges::equal(sources[0].positions(), authored) && sources[1].positions()[0] == Vector3{3, 0, 0} &&
                column<std::uint32_t>(sources[1], "tag", Layer::vertex)[0] == 20 &&
                column<float>(sources[0], "weight", Layer::point)[0] == 1 &&
                column<String>(sources[0], "label", Layer::detail)[0].view() == "port zero" &&
                targets.positions()[0] == Vector3{10, 20, 30},
            "Realize or editing its output mutated an input.");

    auto vector_source = take(Geometry::points(Array<Vector3>{{1, 2, 3}}));
    take(vector_source.set(String{"P"}, Layer::point, SharedArray<Vector3>{Array<Vector3>{{1, 2, 3}}}, Usage::vector));
    auto target = take(Geometry::points(Array<Vector3>{{10, 20, 30}}));
    take(target.add<Vector3>("scale", Layer::point, {2, 3, 4}));
    const std::array vector_sources{vector_source};
    auto vectors = take(realize(vector_sources, target));
    // Explicit P usage is legal: apply the scale, but neither this placement nor
    // a later Transform may translate it. This also catches lost output metadata.
    require(vectors.positions()[0] == Vector3{2, 6, 12} && vectors.find("P")->usage == Usage::vector,
            "Realize ignored or reset the explicit usage of P.");
    take(transform(vectors, {5, 0, 0}, {}, {1, 1, 1}));
    require(vectors.positions()[0] == Vector3{2, 6, 12} && vector_source.positions()[0] == Vector3{1, 2, 3} &&
                vector_source.find("P")->usage == Usage::vector,
            "Realize changed later P semantics or the source column.");
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
    // Eighteen triangles force an internal node (a retained leaf has at most sixteen).
    constexpr std::uint32_t side = 4;
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
    {
        const Vector3 origin{.37f, 4, .61f};
        const auto direction = normalized({.13f, -1, .07f});
        constexpr float limit = 10;
        const auto found = bvh.ray(origin, direction, limit);
        const auto expected =
            brute_ray(references, {origin.x, origin.y, origin.z}, {direction.x, direction.y, direction.z}, limit);
        require(found && expected, "The oblique ray missed the height field.");
        require(std::abs(found->distance - *expected) <= 1e-4 * std::max(1.0, *expected),
                "A BVH ray found another distance.");
        const auto& [a, b, c] = references[found->primitive];
        const auto on = a * (1.0 - found->uv.x - found->uv.y) + b * double(found->uv.x) + c * double(found->uv.y);
        require(near(found->position, Vector3{float(on.x), float(on.y), float(on.z)}, 1e-4f) &&
                    near(found->position, origin + direction * found->distance, 1e-4f),
                "A ray hit is not on its primitive at its distance.");
    }
    {
        const Vector3 point{.63f, 2.4f, .91f};
        constexpr float limit = 10;
        const auto found = bvh.closest(point, limit);
        const double expected = brute_closest(references, {point.x, point.y, point.z});
        require(found && expected <= limit, "The closest query missed the height field.");
        require(std::abs(found->distance - expected) <= 1e-4 * std::max(1.0, expected),
                "A closest query found another distance.");
        const auto& [a, b, c] = references[found->primitive];
        const auto on = a * (1.0 - found->uv.x - found->uv.y) + b * double(found->uv.x) + c * double(found->uv.y);
        require(found->uv.x >= 0 && found->uv.y >= 0 && found->uv.x + found->uv.y <= 1 + 1e-6f &&
                    near(found->position, Vector3{float(on.x), float(on.y), float(on.z)}, 1e-4f),
                "A closest point is not on its primitive.");
    }
}

void ray_node() {
    // Ground at y = 0 over [0, 10]^2, and a raised triangle at y = 2 whose winding faces +Y.
    const auto surface = triangles(
        {{0, 0, 0}, {0, 0, 10}, {10, 0, 0}, {10, 0, 0}, {0, 0, 10}, {10, 0, 10}, {0, 2, 0}, {0, 2, 4}, {4, 2, 0}});
    const auto points = take(Geometry::points(Array<Vector3>{{1, 5, 1}, {20, 5, 20}}));
    Ray node;
    node.normal = true;
    Geometry output;
    node.evaluate(points, surface, output);
    const auto& p = output.positions();
    const auto& primitive = column<std::int32_t>(output, "hitprim", Layer::point);
    const auto& triangle = column<std::int32_t>(output, "hittri", Layer::point);
    const auto& uv = column<Vector2>(output, "hitprimuv", Layer::point);
    const auto& distance = column<float>(output, "hitdist", Layer::point);
    const auto& n = column<Vector3>(output, "N", Layer::point);
    require(near(p[0], {1, 2, 1}) && primitive[0] == 2 && triangle[0] == 0 && near(distance[0], 3) &&
                near(uv[0].x, 0.25f) && near(uv[0].y, 0.25f) && near(n[0], {0, 1, 0}),
            "A ray did not stop at the nearest, raised triangle.");
    require(near(p[1], {20, 5, 20}) && primitive[1] == -1 && triangle[1] == -1 && distance[1] == -1 &&
                near(n[1], {0, 0, 0}),
            "A missing ray moved its point.");
    Geometry projected_again;
    node.evaluate(output, surface, projected_again);
    require(near(column<float>(projected_again, "hitdist", Layer::point)[0], 0) &&
                column<std::int32_t>(projected_again, "hitprim", Layer::point)[0] == 2 &&
                column<std::int32_t>(projected_again, "hitprim", Layer::point)[1] == -1 &&
                near(projected_again.positions()[0], {1, 2, 1}),
            "Ray did not overwrite existing result columns.");
    require(near(column<float>(output, "hitdist", Layer::point)[0], 3) && points.positions()[0] == Vector3{1, 5, 1},
            "Ray wrote through a shared input column.");
    node.remove_misses = true;
    node.evaluate(points, surface, output);
    require(output.count(Layer::point) == 1 && near(output.positions()[0], {1, 2, 1}),
            "Remove Misses did not keep exactly the hits.");
    // Closest point: straight down is the ground at 2.5, but the raised triangle's corner is nearer.
    node.mode = RayMode::minimum;
    node.direction = {};
    node.evaluate(take(Geometry::points(Array<Vector3>{{6, 2.5f, 1}})), surface, output);
    require(near(output.positions()[0], {4, 2, 0}) &&
                near(column<float>(output, "hitdist", Layer::point)[0], std::sqrt(5.25f)),
            "Minimum mode did not find the nearest surface point.");
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

// A mixed-field plain record crosses the Runtime/native boundary as one value.
struct PaddedParameters {
    bool enabled{};

    std::uint32_t count{};
};

class RecordNode : public Node {
public:
    PaddedParameters parameters;

    void evaluate(Geometry& output) const {
        output = parameters.enabled ? take(line({0, 0, 0}, {1, 0, 0}, parameters.count)) : Geometry{};
    }
};

bool failed(const Evaluator& evaluator, ObjectId node) {
    return evaluator.status(node)->error.has_value();
}

void native_constructors() {
    // Divergence measures the authored surface, never another constructor.
    const auto volume = [](const Geometry& geometry) {
        double sum = 0;
        for (std::uint32_t p = 0; p < geometry.count(Layer::primitive); ++p) {
            const auto range = geometry.primitive(p);
            require(range.kind == PrimitiveKind::polygon, "A solid constructor emitted a polyline.");
            const auto a = geometry.positions()[geometry.vertex_points()[range.first]];
            for (std::uint32_t j = 1; j + 1 < range.count; ++j) {
                const auto b = geometry.positions()[geometry.vertex_points()[range.first + j]];
                const auto c = geometry.positions()[geometry.vertex_points()[range.first + j + 1]];
                const double tetrahedron = math::dot(a, math::cross(b, c)) / 6.0;
                require(tetrahedron > 0, "A solid constructor has inward winding.");
                sum += tetrahedron;
            }
        }
        return sum;
    };
    Runtime runtime;
    take(register_types(runtime));
    Meshes resources;
    const auto graph = take(runtime.create(type<Graph>(runtime)));
    const auto sphere_node = take(add_node(runtime, graph.id(), type<Sphere>(runtime)));
    take(runtime.with_native<Sphere>(sphere_node.id(), [](Sphere& node) -> Result<void> {
        node.radius = 2;
        node.segments = 4;
        node.latitudes = 3;
        return {};
    }));
    take(set_display(runtime, graph.id(), sphere_node.id()));
    Evaluator evaluator{runtime, graph, resources};
    take(evaluator.evaluate());
    require(evaluator.output() != nullptr, "Sphere did not publish geometry.");
    const auto& ball = *evaluator.output();
    const float root3 = std::sqrt(3.f);
    const std::array<Vector3, 10> positions{{{0, 2, 0},
                                             {root3, 1, 0},
                                             {0, 1, root3},
                                             {-root3, 1, 0},
                                             {0, 1, -root3},
                                             {root3, -1, 0},
                                             {0, -1, root3},
                                             {-root3, -1, 0},
                                             {0, -1, -root3},
                                             {0, -2, 0}}};
    const std::array<std::uint32_t, 40> corners{0, 2, 1, 0, 3, 2, 0, 4, 3, 0, 1, 4, 1, 2, 6, 5, 2, 3, 7, 6,
                                                3, 4, 8, 7, 4, 1, 5, 8, 9, 5, 6, 9, 6, 7, 9, 7, 8, 9, 8, 5};
    require(ball.count(Layer::point) == 10 && ball.count(Layer::primitive) == 12 &&
                std::ranges::equal(ball.vertex_points(), corners),
            "Sphere topology changed from its authored polar/ring order.");
    for (std::size_t i = 0; i < positions.size(); ++i)
        require(near(ball.positions()[i], positions[i]), "Sphere ring positions differ from the analytic fixture.");
    const auto& normals = column<Vector3>(ball, "N", Layer::vertex);
    for (std::size_t i = 0; i < corners.size(); ++i)
        require(near(normals[i], positions[corners[i]] / 2.f), "Sphere normals are not radial vertex values.");
    // The divergence formula measures the constructed surface; the independent
    // expected volume is the middle prism (12) plus two square pyramids (2 each).
    for (std::uint32_t p = 0; p < ball.count(Layer::primitive); ++p) {
        const auto range = ball.primitive(p);
        require(range.kind == PrimitiveKind::polygon && range.count == (p < 4 || p >= 8 ? 3u : 4u),
                "Sphere polar triangles or middle quads changed.");
    }
    require(std::abs(volume(ball) - 16) < 1e-5, "Sphere volume is not the analytic 16.");
    // Use an existing surface consumer to exercise float polygon validation.
    const auto surface = take(Bvh::build(ball));
    // A pole is a shared-edge degeneracy for Ray; closest has no ray-edge ambiguity.
    const auto closest = surface.closest({0, 3, 0}, 4);
    require(closest && near(closest->position, {0, 2, 0}) && near(closest->distance, 1),
            "The constructed Sphere is not consumable as a polygon surface.");

    Box box_node;
    box_node.size = {2, 4, 6};
    Geometry cube;
    box_node.evaluate(cube);
    const std::array<Vector3, 8> box_points{
        {{-1, -2, -3}, {1, -2, -3}, {-1, 2, -3}, {1, 2, -3}, {-1, -2, 3}, {1, -2, 3}, {-1, 2, 3}, {1, 2, 3}}};
    const std::array<std::uint32_t, 24> box_corners{0, 4, 6, 2, 1, 3, 7, 5, 0, 1, 5, 4,
                                                    2, 6, 7, 3, 0, 2, 3, 1, 4, 5, 7, 6};
    const std::array<Vector3, 6> box_normals{{{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}}};
    require(std::ranges::equal(cube.positions(), box_points) && cube.count(Layer::primitive) == 6 &&
                cube.uniform_primitive_size() == 4 && std::ranges::equal(cube.vertex_points(), box_corners) &&
                std::abs(volume(cube) - 48) < 1e-5,
            "Box did not retain its eight bit-ordered corners and six outward quads.");
    const auto& box_n = column<Vector3>(cube, "N", Layer::vertex);
    for (std::size_t i = 0; i < box_corners.size(); ++i)
        require(box_n[i] == box_normals[i / 4], "Box lost its hard vertex normals.");

    Tube tube_node;
    tube_node.radius = 1;
    tube_node.height = 2;
    tube_node.segments = 5;
    Geometry cylinder;
    tube_node.evaluate(cylinder);
    // Offline sin/cos of the five regular-pentagon vertices, not constructor output.
    const std::array<Vector3, 5> ring{{{1, 0, 0},
                                       {.309016994f, 0, .951056516f},
                                       {-.809016994f, 0, .587785252f},
                                       {-.809016994f, 0, -.587785252f},
                                       {.309016994f, 0, -.951056516f}}};
    const std::array<std::uint32_t, 30> tube_corners{0, 5, 6, 1, 1, 6, 7, 2, 2, 7, 8, 3, 3, 8, 9,
                                                     4, 4, 9, 5, 0, 0, 1, 2, 3, 4, 5, 9, 8, 7, 6};
    require(cylinder.count(Layer::point) == 10 && cylinder.count(Layer::primitive) == 7 &&
                std::ranges::equal(cylinder.vertex_points(), tube_corners) &&
                std::abs(volume(cylinder) - 4.755282581475768) < 1e-5,
            "Tube differs from the pentagonal prism of volume 5 sin(72 degrees).");
    for (std::size_t i = 0; i < 10; ++i)
        require(near(cylinder.positions()[i], ring[i % 5] + Vector3{0, i < 5 ? -1.f : 1.f, 0}),
                "Tube ring order or positions changed.");
    const auto& tube_n = column<Vector3>(cylinder, "N", Layer::vertex);
    for (std::size_t i = 0; i < tube_corners.size(); ++i) {
        const auto expected = i < 20 ? ring[tube_corners[i] % 5] : Vector3{0, i < 25 ? -1.f : 1.f, 0};
        require(near(tube_n[i], expected), "Tube side/cap normals are not distinct vertex values.");
    }
    const auto cap = take(Bvh::build(cylinder)).closest({0, 3, 0}, 4);
    require(cap && near(cap->position, {0, 1, 0}) && near(cap->distance, 2),
            "Tube's float cap is not consumable as a planar polygon.");

    Line line_node;
    line_node.start = {0, 0, 0};
    line_node.end = {4, 2, 6};
    line_node.count = 3;
    Geometry cloud;
    line_node.evaluate(cloud);
    take(cloud.set(String{"weight"}, Layer::point, SharedArray<float>{Array<float>{3, 7, 11}}));
    (void)take(cloud.add<String>("label", Layer::detail, String{"path"}));
    Polyline polyline_node;
    Geometry path;
    polyline_node.evaluate(cloud, path);
    require(path.count(Layer::point) == 3 && path.count(Layer::primitive) == 1 &&
                path.primitive(0).kind == PrimitiveKind::polyline && path.primitive(0).count == 3 &&
                std::ranges::equal(path.vertex_points(), std::array<std::uint32_t, 3>{0, 1, 2}) &&
                path.positions()[0] == Vector3{0, 0, 0} && path.positions()[1] == Vector3{2, 1, 3} &&
                path.positions()[2] == Vector3{4, 2, 6} &&
                std::ranges::equal(column<float>(path, "weight", Layer::point), std::array{3.f, 7.f, 11.f}) &&
                column<String>(path, "label", Layer::detail)[0].view() == "path" && cloud.count(Layer::primitive) == 0,
            "Line to Polyline lost its open AB/BC path, attributes or input isolation.");

    GroupBox group_node;
    group_node.group = String{"chosen"};
    group_node.minimum = {0, -1, 2};
    group_node.maximum = {2, 1, 4};
    auto group_input = take(Geometry::points({{1, 0, 3}, {0, -1, 2}, {2, 1, 4}, {1, 2, 3}, {1, 0, 5}}));
    (void)take(group_input.add<bool>("chosen", Layer::point, true));
    Geometry selected;
    group_node.evaluate(group_input, selected);
    require(std::ranges::equal(column<bool>(selected, "chosen", Layer::point),
                               std::array<bool, 5>{true, true, true, false, false}),
            "GroupBox must replace its bool column using closed bounds on every axis.");
}

void record_parameter_abi() {
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
    const PaddedParameters parameters{true, 3};
    take(runtime.write(first.id(), parameters_field, ValueInput{parameters_type, &parameters}));
    take(set_display(runtime, graph.id(), first.id()));
    Meshes resources;
    Evaluator evaluator{runtime, graph, resources};
    take(evaluator.evaluate());
    require(!failed(evaluator, first.id()) && evaluator.output()->count(Layer::point) == 3,
            "A record parameter did not reach evaluate.");
}

void graph_evaluation() {
    Runtime runtime;
    take(register_types(runtime));

    Meshes resources;
    auto cube = geometry::cube_mesh();
    resources.meshes["fixture/a"] = cube;
    const auto graph_root = take(runtime.create(type<Graph>(runtime)));
    const auto graph = graph_root.id();
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

    Evaluator evaluator{runtime, graph_root, resources};
    take(evaluator.evaluate());
    const auto verify_output = [&] {
        const auto* output = evaluator.output();
        require(output && output->count(Layer::instance) == 14, "The graph lost its 12 grid and two line instances.");
        const auto& first = output->instance_transforms()[0];
        require(near(Vector3{first[12], first[13], first[14]}, {-.5f, 0, -1}), "The published grid origin is wrong.");
    };
    verify_output();

    // A native invoke failure must leave the previous publication alive and readable.
    const auto* shown = evaluator.output();
    write_vector(moved, scale, {std::nanf(""), 1, 1});
    take(evaluator.evaluate());
    require(failed(evaluator, moved) && failed(evaluator, merged) && failed(evaluator, copied) &&
                !failed(evaluator, line_node),
            "A failure did not block exactly the downstream nodes.");
    require(evaluator.output() == shown, "A failure replaced the published output.");
    verify_output();
    write_vector(moved, scale, {1, 1, 1});
    take(evaluator.evaluate());
    require(!failed(evaluator, moved) && !failed(evaluator, merged) && !failed(evaluator, copied),
            "Restoring the parameter left a failure.");
    verify_output();

    // The sole guard against silently retaining a deleted display's output and status.
    take(remove_node(runtime, graph, copied));
    require(take(contents(runtime, graph)).display == ObjectId{}, "Removing the display node left it displayed.");
    take(evaluator.evaluate());
    require(evaluator.output() == nullptr && evaluator.status(copied) == nullptr,
            "A removed display node is still evaluated.");
}

// The engine's Source files, as every project gets them, plus the retained ABI/lifetime fixtures.
struct Project {
    std::map<std::string, std::string> files;

    std::uint64_t version{};

    Project() {
        for (auto& file : take(source::engine_sources(source::default_engine_root())))
            files[file.path] = std::move(file.source);
    }

    void publish(source::Program& program) {
        source::SourceSnapshot snapshot{"procedural", ++version, {}};
        for (const auto& [path, text] : files)
            snapshot.files.push_back({path, source::content_hash(text), text});
        auto candidate = take(source::Program::prepare(take(program.request(std::move(snapshot)))));
        take(program.try_publish(candidate));
    }
};

// Scalar ABI entries, a sibling writer, and a failure after multi-column writes inside invoke.
constexpr std::string_view fixture_source = R"(
uint test_hash(uint x) { return procedural.hash(x); }
uint test_hash2(uint a, uint b) { return procedural.hash2(a, b); }
float test_unit(uint x) { return procedural.unit(x); }
class fixture.Lift : procedural.PointNode {
    float height = 1.0f;
    void evaluate(Array<vector3>& P) {
        const float lift = self.height;
        for (uint i = 0; i < P.size(); i = i + 1) { P[i] = P[i] + vector3(0.0f, lift, 0.0f); }
    }
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
)";

// Independent scalar results and column borrows surviving growth, invoke failure and recovery.
void source_nodes() {
    Runtime runtime;
    take(register_types(runtime));
    Project project;
    project.files["fixture.source"] = std::string{fixture_source};
    source::Program program{runtime};
    project.publish(program);
    const auto source_type = [&](std::string_view name) {
        const auto classes = program.classes();
        const auto found = std::ranges::find(classes, name, &source::ClassInfo::name);
        require(found != classes.end(), "A Source node class was not published.");
        return found->type;
    };
    const auto lift_type = source_type("fixture.Lift");
    // Fixed lowbias32 vectors computed offline with modular integer arithmetic, not random.hpp.
    const auto source_hash = take(program.entry("test_hash"));
    const auto source_hash2 = take(program.entry("test_hash2"));
    const auto source_unit = take(program.entry("test_unit"));
    const auto call = [](const source::Program::Entry& entry, auto... values) {
        return *take(entry.invoke({}, std::array{Scalar{values}...}));
    };
    require(std::get<std::uint32_t>(call(source_hash, 0x9e3779b9u)) == 0x01fce552u &&
                std::get<std::uint32_t>(call(source_hash2, 0x9e3779b9u, 7u)) == 0x9f0407bfu,
            "The Source hash differs from the fixed lowbias32 vectors.");
    require(std::get<float>(call(source_unit, 0x80000000u)) == .5f, "The Source unit is not one half.");

    Meshes resources;
    std::vector<ObjectRoot> roots;
    const auto add = [&](ObjectId owner, TypeId node_type) {
        return roots.emplace_back(take(add_node(runtime, owner, node_type))).id();
    };

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
    const auto& error = batching.status(batch)->error;
    require(error && error->code == ErrorCode::invocation_failed && batching.output() == previous,
            "An invoke failure published a partially written Geometry.");
    verify_batch(batching.output(), 2);
    take(runtime.write(batch, fail, Scalar{false}));
    take(batching.evaluate());
    require(!failed(batching, batch), "A corrected invoke failure did not recover.");
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

void volume_boundary() {
    const auto prism = [](bool rotate_top) {
        Array<Vector3> points{{0, 0, 0},  {1, 0, 0},  {2, 0x1p-14f, 0},  {2, 2, 0},  {0, 2, 0},
                              {0, 0, -1}, {1, 0, -1}, {2, 0x1p-14f, -1}, {2, 2, -1}, {0, 2, -1}};
        Array<std::uint32_t> corners =
            rotate_top ? Array<std::uint32_t>{3, 4, 0, 1, 2} : Array<std::uint32_t>{0, 1, 2, 3, 4};
        const std::array<std::uint32_t, 5> bottom{5, 9, 8, 7, 6};
        corners.append({bottom.data(), bottom.size()});
        Array<std::uint32_t> offsets{0, 5, 10};
        for (std::uint32_t i = 0; i < 5; ++i) {
            const auto next = (i + 1) % 5;
            corners.add(i);
            corners.add(i + 5);
            corners.add(next + 5);
            corners.add(next);
            offsets.add(static_cast<std::uint32_t>(corners.size()));
        }
        return take(Geometry::polygons(std::move(points), std::move(corners), offsets));
    };
    constexpr Vector3 q{-0x1p-5f, 0, 0};
    // Independently evaluated integer hash channels: cell (0,0,0), seed=0, k=0.
    // These dyadic offsets make the final float candidate exactly q, with w=1.
    const VolumeScatterSettings settings{.density = 1,
                                         .seed = 0,
                                         .origin = {q.x - 5829961 * 0x1p-24f, -3561215 * 0x1p-24f, -6784506 * 0x1p-24f},
                                         .cell_size = 1};
    auto cube = take(box({0x1p-6f, 0x1p-6f, 0x1p-6f}));
    for (auto& p : *take(cube.write<Vector3>("P", Layer::point)))
        p += q;
    const Geometry skinny[]{prism(false), cube};
    const Geometry cyclic[]{prism(true), cube};
    const Geometry cases[]{cube, take(Geometry::merge(skinny)), take(Geometry::merge(cyclic))};
    for (const auto& geometry : cases) {
        const auto result = take(volume_scatter(geometry, settings));
        const auto& x = column<std::int32_t>(result, "sourcecellx", Layer::point);
        const auto& y = column<std::int32_t>(result, "sourcecelly", Layer::point);
        const auto& z = column<std::int32_t>(result, "sourcecellz", Layer::point);
        const auto& ordinals = column<std::uint32_t>(result, "sourceordinal", Layer::point);
        bool retained = false;
        for (std::size_t i = 0; i < result.positions().size(); ++i)
            if (x[i] == 0 && y[i] == 0 && z[i] == 0 && ordinals[i] == 0) {
                require(result.positions()[i] == q, "The independent boundary candidate changed.");
                retained = true;
            }
        // q is 1/32 from the prism and 1/128 from its own cube's boundary;
        // a skinny fan's extended tip must not turn it into a surface point.
        require(retained, "A distant narrow fan or changed first corner erased a strict interior point.");
    }
}

void volume_sampling() {
    // Offline integer evaluation, not calls to procedural::hash: graph=3,node=19
    // gives effective seed 0xc98fa26b. lambda=7.5 rounds to 8/7 in cells (-1,0,0)/(0,0,0).
    // Rows contain id and the top 24 random bits of channels X/Y/Z, in cell/ordinal order.
    constexpr std::array<std::array<std::uint32_t, 4>, 15> random{{{0x3ca21855, 8526772, 5547691, 4132262},
                                                                   {0x380d69f4, 7508050, 1922482, 2003365},
                                                                   {0x1880195d, 12870893, 13685249, 4379748},
                                                                   {0x91e08aff, 8075927, 13541315, 8533724},
                                                                   {0x897072c3, 11841542, 16309124, 4024693},
                                                                   {0x216d6968, 477684, 3483467, 16277160},
                                                                   {0xf0476850, 8314132, 14828083, 7378086},
                                                                   {0x7438e251, 15003252, 8474602, 6076835},
                                                                   {0xc6936d90, 9986235, 14698489, 9393789},
                                                                   {0xef84998c, 7524404, 16576179, 3007072},
                                                                   {0x986f1756, 1278147, 12490762, 4397497},
                                                                   {0xd5b289b4, 336057, 4732733, 9624711},
                                                                   {0x7d5740d3, 6989278, 7673449, 648773},
                                                                   {0x9befa51f, 14649682, 2449564, 11813155},
                                                                   {0xeaa27393, 321380, 15649314, 5206315}}};
    const VolumeScatterSettings settings{
        .density = .1171875f, .seed = 0xc98fa26b, .origin = {0, -2, -2}, .cell_size = 4};
    const auto candidate = [&](std::size_t i) {
        return Vector3{(i < 8 ? -4.f : 0.f) + float(random[i][1]) * 0x1p-22f, -2.f + float(random[i][2]) * 0x1p-22f,
                       -2.f + float(random[i][3]) * 0x1p-22f};
    };
    const auto check = [&](const Geometry& result, auto expected) {
        const auto& ids = column<std::uint32_t>(result, "id", Layer::point);
        const auto& x = column<std::int32_t>(result, "sourcecellx", Layer::point);
        const auto& y = column<std::int32_t>(result, "sourcecelly", Layer::point);
        const auto& z = column<std::int32_t>(result, "sourcecellz", Layer::point);
        const auto& ordinals = column<std::uint32_t>(result, "sourceordinal", Layer::point);
        std::size_t output = 0;
        for (std::size_t i = 0; i < random.size(); ++i) {
            const auto p = candidate(i);
            if (!expected(p))
                continue;
            require(output < result.positions().size(), "Volume omitted an independently classified candidate.");
            require(result.positions()[output] == p && ids[output] == random[i][0] && x[output] == (i < 8 ? -1 : 0) &&
                        y[output] == 0 && z[output] == 0 && ordinals[output] == (i < 8 ? i : i - 8),
                    "Volume changed fixed-grid randomness, classification, provenance or stable compaction.");
            ++output;
        }
        require(output == result.positions().size(), "Volume retained an outside or boundary candidate.");
    };
    const auto bounded_box = [](Vector3 lo, Vector3 hi) {
        auto result = take(box({1, 1, 1}));
        auto& positions = *take(result.write<Vector3>("P", Layer::point));
        for (std::uint32_t i = 0; i < 8; ++i)
            for (std::uint32_t axis = 0; axis < 3; ++axis)
                positions[i][axis] = i & (1u << axis) ? hi[axis] : lo[axis];
        return result;
    };
    const Geometry overlapping[]{bounded_box({-4, -2, -2}, {1, 2, 2}), bounded_box({-1, -2, -2}, {4, 2, 2})};
    check(take(volume_scatter(take(Geometry::merge(overlapping)), settings)), [](Vector3) { return true; });
    auto inner = bounded_box({-2.5f, -1.75f, -1.75f}, {-1.5f, -.25f, -.25f});
    inner.reverse_primitives();
    const Geometry hollow[]{bounded_box({-4, -2, -2}, {4, 2, 2}), inner};
    check(take(volume_scatter(take(Geometry::merge(hollow)), settings)), [](Vector3 p) {
        return !(p.x > -2.5f && p.x < -1.5f && p.y > -1.75f && p.y < -.25f && p.z > -1.75f && p.z < -.25f);
    });
    const float boundary = candidate(0).x;
    check(take(volume_scatter(bounded_box({-4, -2, -2}, {boundary, 2, 2}), settings)),
          [&](Vector3 p) { return p.x < boundary; });

    const Vector3 translate{.125f, -.25f, .125f};
    const float sine = std::sin(.37f) / std::sqrt(14.f);
    const Quaternion rotation{sine, 2 * sine, 3 * sine, std::cos(.37f)};
    // Independent inverse Rodrigues rotation; not math::transform or the generated faces.
    const auto local = [&](Vector3 p) {
        const math::Vector3d q{double{p.x} - .125, double{p.y} + .25, double{p.z} - .125};
        const math::Vector3d axis{1 / std::sqrt(14.), 2 / std::sqrt(14.), 3 / std::sqrt(14.)};
        return q * std::cos(.74) - math::cross(axis, q) * std::sin(.74) +
               axis * (math::dot(axis, q) * (1 - std::cos(.74)));
    };
    const auto in_ball = [&](Vector3 p) {
        const auto q = local(p);
        return std::abs(q.y) < 2 && std::abs(q.x) + std::abs(q.z) < std::sqrt(3.) * std::min(1., 2 - std::abs(q.y));
    };
    auto cylinder = take(tube(1, 2, 5));
    take(transform(cylinder, translate, rotation, {1, 1, 1}));
    check(take(volume_scatter(cylinder, settings)), [&](Vector3 p) {
        const auto q = local(p);
        if (std::abs(q.y) >= 1)
            return false;
        for (int j = 0; j < 5; ++j) {
            const double angle = (2 * j + 1) * std::numbers::pi / 5;
            if (q.x * std::cos(angle) + q.z * std::sin(angle) >= std::cos(std::numbers::pi / 5))
                return false;
        }
        return true;
    });
    // The Sphere representative also exercises the native reflected node/port/seed path.
    Runtime runtime;
    take(register_types(runtime));
    Meshes resources;
    const auto graph = take(runtime.create(type<Graph>(runtime)));
    const auto ball = take(add_node(runtime, graph.id(), type<Sphere>(runtime)));
    const auto moved = take(add_node(runtime, graph.id(), type<Transform>(runtime)));
    const auto volume = take(add_node(runtime, graph.id(), type<VolumeScatter>(runtime)));
    take(runtime.write(graph.id(), field(runtime, type<Graph>(runtime), "seed"), Scalar{3u}));
    take(runtime.with_native<Sphere>(ball.id(), [](Sphere& node) -> Result<void> {
        node.radius = 2;
        node.segments = 4;
        node.latitudes = 3;
        return {};
    }));
    take(runtime.with_native<Transform>(moved.id(), [&](Transform& node) -> Result<void> {
        node.translate = translate;
        node.rotate = rotation;
        return {};
    }));
    take(runtime.with_native<VolumeScatter>(volume.id(), [&](VolumeScatter& node) -> Result<void> {
        node.density = settings.density;
        node.seed = 19;
        node.origin = settings.origin;
        node.cell_size = 4;
        return {};
    }));
    take(connect(runtime, graph.id(), ball.id(), 0, moved.id(), 0));
    take(connect(runtime, graph.id(), moved.id(), 0, volume.id(), 0));
    take(set_display(runtime, graph.id(), volume.id()));
    Evaluator evaluator{runtime, graph, resources};
    take(evaluator.evaluate());
    require(evaluator.output() != nullptr, "VolumeScatter node failed to publish.");
    check(*evaluator.output(), in_ball);
}

void realization_chain() {
    Runtime runtime;
    take(register_types(runtime));
    Meshes resources;
    const auto graph = take(runtime.create(type<Graph>(runtime)));
    const auto add = [&](TypeId node_type) { return take(add_node(runtime, graph.id(), node_type)); };
    const auto cube = add(type<Box>(runtime));
    const auto volume = add(type<VolumeScatter>(runtime));
    const auto selected = add(type<GroupBox>(runtime));
    const auto ball = add(type<Sphere>(runtime));
    const auto cylinder = add(type<Tube>(runtime));
    const auto copied = add(type<Realize>(runtime));
    const auto moved = add(type<Transform>(runtime));
    const auto line_node = add(type<Line>(runtime));
    const auto path = add(type<Polyline>(runtime));
    const auto merged = add(type<Merge>(runtime));
    take(runtime.write(graph.id(), field(runtime, type<Graph>(runtime), "seed"), Scalar{3u}));
    take(runtime.with_native<Box>(cube.id(), [](Box& node) -> Result<void> {
        node.size = {2, 2, 2};
        return {};
    }));
    take(runtime.with_native<VolumeScatter>(volume.id(), [](VolumeScatter& node) -> Result<void> {
        node.seed = 19;
        node.density = 2;
        return {};
    }));
    take(runtime.with_native<GroupBox>(selected.id(), [](GroupBox& node) -> Result<void> {
        node.minimum = {0, -1, -1};
        node.maximum = {1, 1, 1};
        return {};
    }));
    take(runtime.with_native<Sphere>(ball.id(), [](Sphere& node) -> Result<void> {
        node.radius = .1f;
        node.segments = 4;
        node.latitudes = 3;
        return {};
    }));
    take(runtime.with_native<Tube>(cylinder.id(), [](Tube& node) -> Result<void> {
        node.radius = .1f;
        node.height = .2f;
        node.segments = 5;
        return {};
    }));
    take(runtime.with_native<Realize>(copied.id(), [](Realize& node) -> Result<void> {
        node.group = String{"selected"};
        node.variant = String{"sourceordinal"};
        node.target_attributes = Array<String>{String{"id"}};
        return {};
    }));
    take(runtime.with_native<Transform>(moved.id(), [](Transform& node) -> Result<void> {
        node.translate = {2, 0, 0};
        return {};
    }));
    take(runtime.with_native<Line>(line_node.id(), [](Line& node) -> Result<void> {
        node.start = {-4, 0, 0};
        node.end = {-2, 2, 0};
        node.count = 3;
        return {};
    }));
    for (const auto& [from, to, input] : {std::tuple{cube.id(), volume.id(), 0u},
                                          {volume.id(), selected.id(), 0u},
                                          {ball.id(), copied.id(), 0u},
                                          {cylinder.id(), copied.id(), 0u},
                                          {selected.id(), copied.id(), 1u},
                                          {copied.id(), moved.id(), 0u},
                                          {line_node.id(), path.id(), 0u},
                                          {moved.id(), merged.id(), 0u},
                                          {path.id(), merged.id(), 0u}})
        take(connect(runtime, graph.id(), from, 0, to, input));
    take(set_display(runtime, graph.id(), merged.id()));
    Evaluator evaluator{runtime, graph, resources};
    take(evaluator.evaluate());
    const auto* output = evaluator.output();
    // Four complete positive-X cells, each with ordinal 0 (Sphere) and 1 (Tube).
    // 4*(10+10) points, 4*(12+7) faces, 4*(40+30) corners, then one three-point line.
    require(output && output->count(Layer::point) == 83 && output->count(Layer::primitive) == 77 &&
                output->count(Layer::vertex) == 283 && output->count(Layer::instance) == 0 &&
                output->primitive(76).kind == PrimitiveKind::polyline && output->positions()[80] == Vector3{-4, 0, 0} &&
                output->positions()[82] == Vector3{-2, 2, 0},
            "The native Volume/GroupBox/Realize/Transform/Merge chain lost its selected copies or line.");
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

    // Closed workload, not the open terrain below. Kept out of ordinary acceptance.
    timing::install({phases::all});
    const auto volume_surface = take(sphere(8, 128, 128));
    std::vector<double> volume_times;
    std::vector<double> classify_times;
    std::uint32_t retained = 0;
    for (int i = -2; i < samples; ++i) {
        timing::Recorder recorder;
        timing::Recorder::Frame frame{recorder};
        const auto result = take(volume_scatter(volume_surface, {.density = 25, .seed = 3}));
        retained = result.count(Layer::point);
        const auto& record = frame.finish();
        require(record.structure_errors == 0, "Volume profile timing catalog is incomplete.");
        if (i >= 0) {
            volume_times.push_back(std::chrono::duration<double, std::milli>(record.time(phases::volume)).count());
            classify_times.push_back(std::chrono::duration<double, std::milli>(record.time(phases::classify)).count());
        }
    }
    std::ranges::sort(volume_times);
    std::ranges::sort(classify_times);
    report += "  \"volume_input\": \"Sphere(radius=8,segments=128,latitudes=128); origin=0,h=1,density=25,seed=3\",\n";
    number("volume_samples", samples);
    number("volume_warmups", 2);
    number("volume_cells", 16 * 16 * 16);
    number("volume_candidates", 16 * 16 * 16 * 25);
    number("volume_triangles", 2 * 128 * (128 - 1));
    number("volume_retained", retained);
    number("volume_ms", volume_times[volume_times.size() / 2]);
    number("volume_generate_classify_ms", classify_times[classify_times.size() / 2]);

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
        convex_surface();
        scatter_counts_and_spacing();
        transform_by_usage();
        removal_merge_and_promotion();
        copy_to_points_and_publication();
        realized_geometry();
        bvh_queries();
        ray_node();
        native_constructors();
        volume_boundary();
        volume_sampling();
        realization_chain();
        record_parameter_abi();
        graph_evaluation();
        source_nodes();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "procedural acceptance failed: %s\n", error.what());
        return 1;
    }
    std::puts("procedural acceptance passed");
    return 0;
}
