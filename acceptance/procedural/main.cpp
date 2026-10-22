#include "procedural/evaluator.hpp"
#include "procedural/instances.hpp"
#include "procedural/nodes.hpp"
#include "procedural/operations.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <numbers>
#include <stdexcept>
#include <string>

namespace {
using namespace atelier;
using namespace atelier::procedural;
using math::Matrix4;
using math::Quaternion;
using math::Vector2;
using math::Vector3;

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class T> T take(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(*result);
}
void take(Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }
template<class T> void reject(const Result<T>& result, const char* message) { require(!result, message); }
bool near(float a, float b, float tolerance = 1e-5f) { return std::abs(a - b) <= tolerance * std::max(1.f, std::abs(b)); }
bool near(Vector3 a, Vector3 b, float tolerance = 1e-5f) {
    return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance);
}
template<class T> const Array<T>& column(const Geometry& geometry, const char* name, Layer layer) {
    const auto* values = take(geometry.read<T>(name, layer));
    require(values != nullptr, "An expected attribute is missing.");
    return *values;
}
Vector3 normalized(Vector3 v) { return *math::try_normalize(v); }
Quaternion axis_angle(Vector3 axis, float degrees) {
    const float half = degrees * std::numbers::pi_v<float> / 360.f;
    const auto a = normalized(axis) * std::sin(half);
    return {a.x, a.y, a.z, std::cos(half)};
}

Geometry triangles(std::initializer_list<Vector3> corners) {
    Array<Vector3> points{corners};
    Array<std::uint32_t> vertices, offsets{0};
    for (std::uint32_t i = 0; i < points.size(); ++i) vertices.add(i);
    for (std::uint32_t t = 1; t <= points.size() / 3; ++t) offsets.add(t * 3);
    return take(Geometry::polygons(std::move(points), std::move(vertices), offsets));
}
// Twice the signed area of (a, b, p) in the XY plane.
double side(Vector3 a, Vector3 b, Vector3 p) { return double(b.x - a.x) * (p.y - a.y) - double(b.y - a.y) * (p.x - a.x); }
bool inside(Vector3 p, Vector3 a, Vector3 b, Vector3 c) {
    const double e = 1e-5;
    return p.z == 0 && side(a, b, p) >= -e && side(b, c, p) >= -e && side(c, a, p) >= -e;
}

void scatter_counts_and_stability() {
    const Vector3 a0{0, 0, 0}, a1{2, 0, 0}, a2{0, 1, 0};      // Area 1.
    const Vector3 b0{10, 0, 0}, b1{13, 0, 0}, b2{10, 2, 0};   // Area 3.
    const auto surface = triangles({a0, a1, a2, b0, b1, b2});
    const auto points = take(scatter(surface, {.density = 4, .seed = 7}));
    const auto& positions = points.positions();
    const auto& source = column<std::uint32_t>(points, "sourceprim", Layer::point);
    std::uint32_t first = 0, second = 0;
    for (std::size_t i = 0; i < positions.size(); ++i) {
        if (source[i] == 0) { ++first; require(inside(positions[i], a0, a1, a2), "A point left the first triangle."); }
        else { ++second; require(source[i] == 1 && inside(positions[i], b0, b1, b2), "A point left the second triangle."); }
    }
    require(first == 4 && second == 12, "Area x density points did not land on each triangle exactly.");
    const auto& normals = column<Vector3>(points, "N", Layer::point);
    for (const auto& n : normals) require(near(n, {0, 0, 1}), "Scattered normals are not the face normal.");

    const auto again = take(scatter(surface, {.density = 4, .seed = 7}));
    require(std::memcmp(again.positions().data(), positions.data(), positions.size() * sizeof(Vector3)) == 0,
        "The same seed placed points differently.");
    const auto other = take(scatter(surface, {.density = 4, .seed = 8}));
    require(other.positions().size() == positions.size() &&
        std::memcmp(other.positions().data(), positions.data(), positions.size() * sizeof(Vector3)) != 0,
        "Another seed placed the same points.");

    // Moving the second triangle leaves the first one's points bit for bit, and growing
    // the first one (more points before them) leaves the second one's.
    const auto moved = take(scatter(triangles({a0, a1, a2, b0 + Vector3{0, 5, 0}, b1 + Vector3{0, 5, 0}, b2 + Vector3{0, 5, 0}}),
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
    require(column<std::uint32_t>(spaced, "id", Layer::point).size() == kept.size(), "Elimination left columns unequal.");
}

void transform_by_usage() {
    auto grid_points = take(grid({4, 3}, 4, 5));
    require(grid_points.count(Layer::point) == 20, "A 5 x 4 grid has 20 points.");
    const auto tilted = normalized({1, 1, 0});
    for (auto& n : *take(grid_points.write<Vector3>("N", Layer::point))) n = tilted;
    take(grid_points.add<Vector3>("Cd", Layer::point, {0.2f, 0.4f, 0.6f}));
    take(grid_points.add<Vector3>("v", Layer::point, {1, 0, 1}));
    const auto before = grid_points.positions();
    const Vector3 t{1, 2, 3}, s{2, 3, 0.5f};
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
    for (const auto& c : column<Vector3>(grid_points, "Cd", Layer::point)) require(c == Vector3{0.2f, 0.4f, 0.6f}, "Colors moved.");

    // A flattening transform still defines plane normals and never makes NaN.
    auto flat = take(line({0, 0, 0}, {1, 0, 0}, 2));
    take(flat.set(String{"N"}, Layer::point, std::make_shared<Array<Vector3>>(Array<Vector3>{{0.6f, 0, 0.8f}, {1, 0, 0}})));
    take(transform(flat, {}, {}, {1, 1, 0}));
    const auto& flattened = column<Vector3>(flat, "N", Layer::point);
    require(near(flattened[0], {0, 0, 1}) && flattened[1] == Vector3{}, "Singular transforms broke plane normals.");

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
            require(mirrored.vertex_points()[p * 3 + i] == cube.vertex_points()[p * 3 + 2 - i], "A mirrored triangle kept its order.");
            require(new_uvs[p * 3 + i] == old_uvs[p * 3 + 2 - i], "Vertex attributes did not follow the reversed order.");
            const auto n = old_normals[p * 3 + 2 - i];
            require(near(new_normals[p * 3 + i], {-n.x, n.y, n.z}), "Vertex normals were not reflected.");
        }
    require(column<Vector3>(cube, "N", Layer::vertex).data() != new_normals.data(), "Transform wrote the shared source column.");
}

void removal_merge_and_promotion() {
    const auto points = take(grid({4, 3}, 4, 5)); // Columns at x = -2, -1, 0, 1, 2.
    auto flagged = points;
    auto& flags = *take(flagged.add<bool>("cut", Layer::point));
    for (std::size_t i = 0; i < flags.size(); ++i) flags[i] = flagged.positions()[i].x > 0;
    const auto remaining = take(flagged.remove_flagged(Layer::point, "cut"));
    require(remaining.count(Layer::point) == 12, "Removing x > 0 should leave 3 of 5 columns.");
    for (const auto& p : remaining.positions()) require(p.x <= 0, "A flagged point survived.");
    require(column<Vector3>(remaining, "N", Layer::point).size() == 12, "Attributes did not follow removal.");

    const auto cube = import_mesh(*geometry::cube_mesh());
    const auto corner = cube.vertex_points()[0];
    std::uint32_t using_corner = 0;
    const auto vertices = geometry::cube_mesh()->vertices(); // Triangles touching the corner, from the source mesh.
    const auto& corner_position = cube.positions()[corner];
    for (std::size_t t = 0; t < vertices.size() / 3; ++t)
        for (std::size_t i = 0; i < 3; ++i)
            if (Vector3{vertices[t * 3 + i].position[0], vertices[t * 3 + i].position[1], vertices[t * 3 + i].position[2]} == corner_position) {
                ++using_corner; break;
            }
    auto marked = cube;
    auto& corners = *take(marked.add<bool>("cut", Layer::point));
    corners[corner] = true;
    const auto opened = take(marked.remove_flagged(Layer::point, "cut"));
    require(opened.count(Layer::point) == 7 && opened.count(Layer::primitive) == 12 - using_corner &&
        opened.count(Layer::vertex) == opened.count(Layer::primitive) * 3, "Removing a corner kept a triangle that used it.");
    for (const auto point : opened.vertex_points()) require(point < 7, "A vertex refers past the remaining points.");
    auto faces = cube;
    auto& face_flags = *take(faces.add<bool>("cut", Layer::primitive));
    face_flags[0] = face_flags[5] = true;
    const auto fewer = take(faces.remove_flagged(Layer::primitive, "cut"));
    require(fewer.count(Layer::primitive) == 10 && fewer.count(Layer::point) == 8, "Removing primitives removed points.");

    reject(Geometry::merge(std::vector{points, cube}), "Point N and vertex N merged.");
    const auto pair = take(Geometry::merge(std::vector{cube, cube}));
    require(pair.count(Layer::point) == 16 && pair.count(Layer::primitive) == 24 && pair.uniform_primitive_size() == 3,
        "Merged cubes lost elements.");
    for (std::uint32_t v = 0; v < 36; ++v)
        require(pair.vertex_points()[36 + v] == cube.vertex_points()[v] + 8, "The second cube's vertices were not offset.");

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
    for (auto& uv : *take(copy.write<Vector2>("uv", Layer::vertex))) uv = {9, 9};
    require(column<Vector2>(cube, "uv", Layer::vertex)[0] != Vector2{9, 9}, "Writing a copy changed the source.");
    require(column<Vector3>(copy, "N", Layer::vertex).data() == column<Vector3>(cube, "N", Layer::vertex).data(),
        "An unwritten column was copied.");
}

Matrix4 expected_instance(Vector3 position, Quaternion orient, Vector3 scale) {
    Matrix4 m = Matrix4::identity();
    const Vector3 axes[3]{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (std::size_t row = 0; row < 3; ++row) {
        const auto axis = math::rotate(orient, axes[row]) * scale[row];
        m(row, 0) = axis.x; m(row, 1) = axis.y; m(row, 2) = axis.z;
    }
    m[12] = position.x; m[13] = position.y; m[14] = position.z;
    return m;
}
bool near(const Matrix4& a, const Matrix4& b) {
    for (std::size_t i = 0; i < 16; ++i) if (!near(a[i], b[i])) return false;
    return true;
}

void copy_to_points_and_publication() {
    const auto mesh_a = geometry::cube_mesh();
    const auto mesh_b = std::make_shared<const geometry::Mesh>(std::vector<geometry::Vertex>(mesh_a->vertices().begin(), mesh_a->vertices().end()));
    const std::vector<Reference> references{{String{"fixture/a"}, mesh_a}, {String{"fixture/b"}, mesh_b}};

    auto oriented = take(Geometry::points(Array<Vector3>{{1, 2, 3}, {-4, 0, 1}, {0, 0, 0}}));
    const Quaternion orients[3]{axis_angle({1, 2, 3}, 40), axis_angle({0, 0, 1}, -75), {}};
    take(oriented.set(String{"orient"}, Layer::point, std::make_shared<Array<Quaternion>>(Array<Quaternion>{orients[0], orients[1], orients[2]})));
    take(oriented.set(String{"pscale"}, Layer::point, std::make_shared<Array<float>>(Array<float>{2, 0.5f, 1})));
    take(oriented.set(String{"scale"}, Layer::point, std::make_shared<Array<Vector3>>(Array<Vector3>{{1, 2, 3}, {3, 1, 1}, {1, 1, 1}})));
    take(oriented.set(String{"variant"}, Layer::point, std::make_shared<Array<std::uint32_t>>(Array<std::uint32_t>{0, 1, 1})));
    take(oriented.add<String>("label", Layer::detail, String{"kept"}));
    const auto instances = take(copy_to_points(oriented, references));
    require(instances.count(Layer::point) == 0 && instances.count(Layer::instance) == 3, "Copy to Points made no instances.");
    require(column<String>(instances, "label", Layer::detail)[0] == String{"kept"}, "Detail attributes were dropped.");
    const Vector3 scales[3]{{2, 4, 6}, {1.5f, 0.5f, 0.5f}, {1, 1, 1}};
    for (std::uint32_t i = 0; i < 3; ++i) {
        require(near(instances.instance_transforms()[i], expected_instance(oriented.positions()[i], orients[i], scales[i])),
            "An instance transform is not S * O * T.");
        require(instances.instance_references()[i] == (i ? 1u : 0u), "Variant did not pick the reference.");
    }

    // Without orient: N is +Z and up steers +Y; N alone takes the shortest turn.
    auto aligned = take(Geometry::points(Array<Vector3>{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}));
    take(aligned.set(String{"N"}, Layer::point, std::make_shared<Array<Vector3>>(Array<Vector3>{{3, 0, 0}, {0, 0, -1}, {0, 0, 1}})));
    auto with_up = aligned;
    take(with_up.set(String{"up"}, Layer::point, std::make_shared<Array<Vector3>>(Array<Vector3>{{0, 5, 0}, {0, 0, 0}, {1, 0, 0}})));
    const auto rows = [](const Matrix4& m) { return std::array<Vector3, 3>{Vector3{m[0], m[1], m[2]}, Vector3{m[4], m[5], m[6]}, Vector3{m[8], m[9], m[10]}}; };
    const auto up_axes = rows(take(copy_to_points(with_up, references)).instance_transforms()[0]);
    require(near(up_axes[0], {0, 0, -1}) && near(up_axes[1], {0, 1, 0}) && near(up_axes[2], {1, 0, 0}), "N and up did not align +Z and +Y.");
    const auto turned = take(copy_to_points(aligned, references));
    const auto opposite = rows(turned.instance_transforms()[1]);
    require(near(opposite[0], {1, 0, 0}) && near(opposite[1], {0, -1, 0}) && near(opposite[2], {0, 0, -1}), "N = -Z did not turn half way.");
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
    auto scattered = take(Geometry::points(Array<Vector3>{{9.99f, 0, 0}, {10, 0, 0}, {-0.01f, 0, 0}, {0, 0, 0}, {5, 0, 5}, {6, 0, 6}, {3, 0, 3}}));
    take(scattered.set(String{"variant"}, Layer::point, std::make_shared<Array<std::uint32_t>>(Array<std::uint32_t>{0, 0, 0, 1, 1, 0, 0})));
    take(scattered.set(String{"pscale"}, Layer::point, std::make_shared<Array<float>>(Array<float>{1, 1, 1, 1, 0, 1, 1})));
    take(scattered.set(String{"scale"}, Layer::point, std::make_shared<Array<Vector3>>(Array<Vector3>{{1, 1, 1}, {1, 1, 1}, {1, 1, 1}, {1, 1, 1}, {1, 1, 1}, {1, 0, 1}, {2, 2, 2}})));
    const auto first = take(publish(take(copy_to_points(scattered, references)), 10));
    require(first.dropped == 2, "Zero and flattened scales were published.");
    std::map<std::pair<std::string, std::array<std::int32_t, 3>>, std::vector<float>> found;
    for (const auto& collection : first.collections) {
        require(!collection.transforms->empty(), "An empty collection was published.");
        auto& xs = found[{std::string{collection.reference.key.view().std_view()}, collection.chunk}];
        for (const auto& affine : *collection.transforms) xs.push_back(affine[9]);
    }
    const std::map<std::pair<std::string, std::array<std::int32_t, 3>>, std::vector<float>> expected{
        {{"fixture/a", {0, 0, 0}}, {9.99f, 3}}, {{"fixture/a", {1, 0, 0}}, {10}}, {{"fixture/a", {-1, 0, 0}}, {-0.01f}},
        {{"fixture/b", {0, 0, 0}}, {0}}};
    require(found == expected, "Instances were not grouped by reference and chunk in order.");
    const auto& scaled = first.collections[1].transforms->back(); // fixture/a chunk 0, the point at 3.
    require(first.collections[1].chunk == std::array<std::int32_t, 3>{0, 0, 0} && scaled[0] == 2 && scaled[4] == 2 && scaled[8] == 2 && scaled[11] == 3,
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

struct Meshes final : Resources {
    std::map<std::string, geometry::MeshRef, std::less<>> meshes;
    std::uint64_t version{1};
    Result<geometry::MeshRef> mesh(StringView key) const override {
        const auto found = meshes.find(key.std_view());
        if (found == meshes.end()) return std::unexpected(Error{ErrorCode::invalid_member, "No mesh " + std::string{key.std_view()} + "."});
        return found->second;
    }
    std::uint64_t revision() const noexcept override { return version; }
};

template<class T> TypeId type(Runtime& runtime) { return take(runtime.native_type<T>()); }
FieldId field(Runtime& runtime, TypeId owner, const char* name) { return take(runtime.find_field(owner, name)); }

struct Counts {
    std::uint64_t evaluations, hits;
    bool operator==(const Counts&) const = default;
};
void expect(const Evaluator& evaluator, std::initializer_list<std::pair<ObjectId, Counts>> nodes, const char* message) {
    for (const auto& [node, counts] : nodes) {
        const auto* status = evaluator.status(node);
        require(status && Counts{status->evaluations, status->cache_hits} == counts, message);
    }
}
bool failed(const Evaluator& evaluator, ObjectId node) { return evaluator.status(node)->error.has_value(); }

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
    require(merge_ports.multiple == std::vector<bool>{true} && merge_ports.outputs == 1, "Merge ports were not read from evaluate.");
    const auto scatter_ports = take(ports(runtime, type<Scatter>(runtime)));
    require(scatter_ports.parameters == std::vector<Slot>{Slot::context, Slot::input, Slot::output}, "Scatter ports were not read in order.");
    reject(ports(runtime, type<Graph>(runtime)), "A graph was accepted as a node type.");

    Meshes resources;
    auto cube = geometry::cube_mesh();
    resources.meshes["fixture/a"] = cube;
    const auto graph_root = take(runtime.create(type<Graph>(runtime)));
    const auto graph = graph_root.id();
    reject(add_node(runtime, graph, type<Graph>(runtime)), "A graph was added as a node.");
    const auto add = [&](TypeId node_type) { return take(add_node(runtime, graph, node_type)); };
    const auto grid_root = add(type<Grid>(runtime)), moved_root = add(type<Transform>(runtime)), line_root = add(type<Line>(runtime)),
        merged_root = add(type<Merge>(runtime)), copied_root = add(type<CopyToPoints>(runtime));
    const auto grid_node = grid_root.id(), moved = moved_root.id(), line_node = line_root.id(), merged = merged_root.id(), copied = copied_root.id();
    const auto vector2 = type<Vector2>(runtime), vector3 = type<Vector3>(runtime);
    const auto translate = field(runtime, type<Transform>(runtime), "translate"), scale = field(runtime, type<Transform>(runtime), "scale");
    const auto write_vector = [&](ObjectId node, FieldId at, Vector3 value) { take(runtime.write(node, at, ValueInput{vector3, &value})); };
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
    require(merged_inputs == std::vector<Input>{{moved, 0, 0}, {line_node, 0, 0}}, "A multiple input did not keep connection order.");

    Evaluator evaluator{runtime, graph_root, resources};
    auto cursor = take(runtime.change_cursor());
    take(evaluator.evaluate());
    std::vector<ObjectId> written;
    runtime.changed_objects(cursor, written);
    require(written.empty(), "Evaluation wrote the graph or its nodes.");
    expect(evaluator, {{grid_node, {1, 0}}, {moved, {1, 0}}, {line_node, {1, 0}}, {merged, {1, 0}}, {copied, {1, 0}}},
        "The first evaluation did not run each node once.");
    require(same_instances(evaluator.output(), expected_output({1, 0, 0}, {1, 1, 1}, 2, cube)), "The graph output differs from direct calls.");
    take(evaluator.evaluate());
    expect(evaluator, {{grid_node, {1, 0}}, {moved, {1, 0}}, {line_node, {1, 0}}, {merged, {1, 0}}, {copied, {1, 0}}},
        "An unchanged graph was evaluated again.");

    // A parameter write reruns the node and what reads it; undoing it finds the cached results.
    write_vector(moved, translate, {2, 0, 0});
    take(evaluator.evaluate());
    expect(evaluator, {{grid_node, {1, 0}}, {line_node, {1, 0}}, {moved, {2, 0}}, {merged, {2, 0}}, {copied, {2, 0}}},
        "A Transform write did not rerun exactly Transform and its downstream nodes.");
    require(same_instances(evaluator.output(), expected_output({2, 0, 0}, {1, 1, 1}, 2, cube)), "The rerun output is wrong.");
    write_vector(moved, translate, {1, 0, 0});
    take(evaluator.evaluate());
    expect(evaluator, {{grid_node, {1, 0}}, {line_node, {1, 0}}, {moved, {2, 1}}, {merged, {2, 1}}, {copied, {2, 1}}},
        "Undoing a write did not reuse the cached results.");
    require(same_instances(evaluator.output(), expected_output({1, 0, 0}, {1, 1, 1}, 2, cube)), "The undone output is wrong.");

    // A writable borrow that changes nothing stamps the node, but its key filters it out.
    cursor = take(runtime.change_cursor());
    take(runtime.with_native<Transform>(moved, [](Transform&) -> Result<void> { return {}; }));
    require(runtime.changed_since(moved, cursor), "A writable borrow did not stamp the node.");
    take(evaluator.evaluate());
    expect(evaluator, {{moved, {2, 1}}, {merged, {2, 1}}, {copied, {2, 1}}}, "An unchanged stamped node was evaluated.");
    take(runtime.with_native<Transform>(moved, [](Transform& node) -> Result<void> { node.scale = {1, 2, 1}; return {}; }));
    take(evaluator.evaluate());
    expect(evaluator, {{grid_node, {1, 0}}, {moved, {3, 1}}, {merged, {3, 1}}, {copied, {3, 1}}}, "A native write was not evaluated.");
    take(runtime.write(line_node, field(runtime, type<Line>(runtime), "count"), Scalar{std::uint32_t{3}}));
    take(evaluator.evaluate());
    expect(evaluator, {{grid_node, {1, 0}}, {moved, {3, 1}}, {line_node, {2, 0}}, {merged, {4, 1}}, {copied, {4, 1}}},
        "A write on one branch reran the other branch.");
    require(same_instances(evaluator.output(), expected_output({1, 0, 0}, {1, 2, 1}, 3, cube)), "The two-branch output is wrong.");
    // The graph seed reaches only nodes that take the context.
    take(runtime.write(graph, field(runtime, type<Graph>(runtime), "seed"), Scalar{std::uint32_t{9}}));
    take(evaluator.evaluate());
    expect(evaluator, {{grid_node, {1, 0}}, {moved, {3, 1}}, {line_node, {2, 0}}, {merged, {4, 1}}, {copied, {5, 1}}},
        "The graph seed did not rerun exactly the nodes taking the context.");

    // A failing node blocks its downstream nodes and keeps the published output; it is not
    // retried until its key changes, and restoring it finds the cache again.
    const auto* shown = evaluator.output();
    write_vector(moved, scale, {std::nanf(""), 1, 1});
    take(evaluator.evaluate());
    require(failed(evaluator, moved) && failed(evaluator, merged) && failed(evaluator, copied) && !failed(evaluator, line_node),
        "A failure did not block exactly the downstream nodes.");
    require(evaluator.output() == shown, "A failure replaced the published output.");
    expect(evaluator, {{moved, {4, 1}}, {merged, {4, 1}}, {copied, {5, 1}}}, "Blocked nodes were evaluated.");
    take(runtime.write(line_node, field(runtime, type<Line>(runtime), "count"), Scalar{std::uint32_t{4}}));
    take(evaluator.evaluate());
    expect(evaluator, {{moved, {4, 1}}, {line_node, {3, 0}}, {merged, {4, 1}}}, "A failed node was retried with the same key.");
    write_vector(moved, scale, {1, 2, 1});
    take(evaluator.evaluate());
    require(!failed(evaluator, moved) && !failed(evaluator, merged) && !failed(evaluator, copied), "Restoring the parameter left a failure.");
    expect(evaluator, {{moved, {4, 2}}, {merged, {5, 1}}, {copied, {6, 1}}}, "The restored node did not hit the cache.");
    require(same_instances(evaluator.output(), expected_output({1, 0, 0}, {1, 2, 1}, 4, cube)), "The recovered output is wrong.");

    // Resources: a new revision that resolves the same mesh reruns nothing; another mesh reruns
    // its readers; a missing mesh fails until it appears.
    ++resources.version;
    take(evaluator.evaluate());
    expect(evaluator, {{copied, {6, 1}}}, "An unchanged resource reran its reader.");
    const auto replacement = std::make_shared<const geometry::Mesh>(std::vector<geometry::Vertex>(cube->vertices().begin(), cube->vertices().end()));
    resources.meshes["fixture/a"] = replacement;
    ++resources.version;
    take(evaluator.evaluate());
    expect(evaluator, {{merged, {5, 1}}, {copied, {7, 1}}}, "A changed resource did not rerun exactly its reader.");
    require(same_instances(evaluator.output(), expected_output({1, 0, 0}, {1, 2, 1}, 4, replacement)), "The new mesh was not used.");
    take(runtime.with_native<CopyToPoints>(copied, [](CopyToPoints& node) -> Result<void> {
        node.meshes[0] = String{"fixture/later"};
        return {};
    }));
    shown = evaluator.output();
    take(evaluator.evaluate());
    require(failed(evaluator, copied) && evaluator.output() == shown, "A missing mesh did not fail and keep the output.");
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
    require(take(inputs(runtime, moved)) == std::vector<Input>{{line_node, 0, 0}}, "A single input kept its old connection.");
    take(connect(runtime, graph, grid_node, 0, moved, 0));
    take(disconnect(runtime, line_node, 0, merged, 0));
    require(take(inputs(runtime, merged)) == std::vector<Input>{{moved, 0, 0}}, "Disconnecting removed the wrong connection.");
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
    require(evaluator.output() == nullptr && evaluator.status(copied) == nullptr, "A removed display node is still evaluated.");
    take(set_display(runtime, graph, moved));
    take(evaluator.evaluate());
    require(evaluator.output() && evaluator.output()->count(Layer::point) == 0, "An unconnected input was not empty geometry.");

    // A zero budget caches nothing, so undoing a write evaluates again.
    Evaluator uncached{runtime, graph_root, resources, 0};
    take(uncached.evaluate());
    write_vector(moved, translate, {5, 0, 0});
    take(uncached.evaluate());
    write_vector(moved, translate, {1, 0, 0});
    take(uncached.evaluate());
    expect(uncached, {{moved, {3, 0}}}, "A zero cache budget kept results.");
}

}

int main() {
    try {
        scatter_counts_and_stability();
        transform_by_usage();
        removal_merge_and_promotion();
        copy_to_points_and_publication();
        graph_evaluation();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "procedural acceptance failed: %s\n", error.what());
        return 1;
    }
    std::puts("procedural acceptance passed");
    return 0;
}
