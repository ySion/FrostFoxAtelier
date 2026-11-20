#include "memory/fatal.hpp"
#include "../helpers.hpp"
#include "boolean_profile.hpp"
#include "foundation/result.hpp"
#include "procedural/bvh.hpp"
#include "procedural/evaluator.hpp"
#include "procedural/instances.hpp"
#include "procedural/nodes.hpp"
#include "procedural/operations.hpp"
#include "procedural/random.hpp"
#include "procedural/timing.hpp"
#include "runtime/native.hpp"
#include "source/program.hpp"
#include "source/snapshot.hpp"

#include <algorithm>
#include <array>
#include <bit>
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

class BadLengthNode final : public Node {
public:
    bool mutate_positions{};

    Result<void> process(Array<Vector3>& positions, Array<float>& weights) const noexcept {
        if (mutate_positions)
            positions.add({0.0f, 0.0f, 0.0f});
        else
            weights.add(1.0f);
        return {};
    }
};

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

void ray_node() {
    // Ground at y = 0 over [0, 10]^2, and a raised triangle at y = 2 whose winding faces +Y.
    const auto surface = triangles(
        {{0, 0, 0}, {0, 0, 10}, {10, 0, 0}, {10, 0, 0}, {0, 0, 10}, {10, 0, 10}, {0, 2, 0}, {0, 2, 4}, {4, 2, 0}});
    const auto points = take(Geometry::points(Array<Vector3>{{1, 5, 1}, {20, 5, 20}}));
    Ray node;
    node.normal = true;
    Geometry output;
    take(node.process(points, surface, output));
    const auto& p = output.positions();
    const auto& primitive = column<std::int32_t>(output, "hitprim", Layer::point);
    const auto& triangle = column<std::int32_t>(output, "hittri", Layer::point);
    const auto& uv = column<Vector2>(output, "hitprimuv", Layer::point);
    const auto& distance = column<float>(output, "hitdist", Layer::point);
    const auto& n = column<Vector3>(output, "N", Layer::point);
    require(near(p[0], {1, 2, 1}) && primitive[0] == 2 && triangle[0] == 0 && near(distance[0], 3) &&
                near(uv[0].x, 0.25f) && near(uv[0].y, 0.25f) && near(n[0], {0, 1, 0}),
            "A ray did not stop at the nearest, raised triangle.");
}

struct Meshes final : Resources {
    std::map<std::string, geometry::MeshRef, std::less<>> meshes;

    Result<geometry::MeshRef> mesh(StringView key) const noexcept override {
        const auto found = meshes.find(key.std_view());
        if (found == meshes.end())
            return std::unexpected(Error{ErrorCode::invalid_member, "Unknown mesh."});
        return found->second;
    }

    std::uint64_t revision() const noexcept override { return 1; }
};

template <class T>
TypeId type(Runtime& runtime) {
    return take(runtime.native_type<T>());
}

FieldId field(Runtime& runtime, TypeId owner, const char* name) {
    return take(runtime.find_field(owner, name));
}

void connectivity_modeling() {
    // Two coincident triangles use distinct points. A polyline shares a point with
    // only one triangle, and point zero is loose at the same position as both.
    auto model = take(Geometry::primitives(
        Array<Vector3>{{0, 0, 0}, {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 2, 0}},
        Array<std::uint32_t>{6, 4, 5, 7, 3, 1, 2, 3}, Array<std::uint32_t>{0, 3, 5, 8},
        Array<PrimitiveKind>{PrimitiveKind::polygon, PrimitiveKind::polyline, PrimitiveKind::polygon}));
    take(model.add<std::uint32_t>("piece", Layer::point, 42));
    Connectivity classify;
    Geometry tagged;
    take(classify.process(model, tagged));
    require(std::ranges::equal(column<std::uint32_t>(tagged, "piece", Layer::point),
                               std::array{0u, 1u, 1u, 1u, 2u, 2u, 2u, 1u}) &&
                std::ranges::all_of(column<std::uint32_t>(model, "piece", Layer::point),
                                    [](auto id) { return id == 42; }) &&
                tagged.vertex_points().data() == model.vertex_points().data() &&
                tagged.positions().data() == model.positions().data(),
            "Connectivity changed input storage or grouped points by position or primitive traversal order.");

    Promote faces;
    faces.name = String::literal<"piece">();
    faces.to = Layer::primitive;
    faces.reduce = Reduce::first;
    Geometry per_face;
    take(faces.process(tagged, per_face));
    require(std::ranges::equal(column<std::uint32_t>(per_face, "piece", Layer::primitive), std::array{2u, 1u, 1u}),
            "Promote did not carry the piece ID to its polygons and open polyline.");
    require(!connectivity(per_face) && per_face.find("piece")->layer == Layer::primitive,
            "Connectivity overwrote an incompatible existing column.");
}

std::pair<double, double> solid_measures(const Geometry& g) {
    double volume = 0;
    double area = 0;
    for (std::uint32_t i = 0; i < g.count(Layer::primitive); ++i) {
        auto face = g.primitive(i);
        auto vertex = [&](auto n) { return math::Vector3d(g.positions()[g.vertex_points()[face.first + n]]); };
        const auto a = vertex(0);
        for (std::uint32_t j = 1; j + 1 < face.count; ++j) {
            auto cross = math::cross(vertex(j) - a, vertex(j + 1) - a);
            volume += math::dot(a, cross) / 6.;
            area += math::length(cross) / 2.;
        }
    }
    return {volume, area};
}

void boolean_modeling() {
    using enum BooleanOperation;
    auto a = take(box({2, 2, 2}));
    auto b = a;
    take(transform(b, {1, 0, 0}, {}, {1, 1, 1}));
    for (auto* mesh : {&a, &b}) {
        auto* material = take(mesh->add<std::uint32_t>("material", Layer::primitive));
        for (std::uint32_t p = 0; p < 6; ++p)
            (*material)[p] = 0xffffff00u + p + (mesh == &b ? 16u : 0u);
        auto* uv = take(mesh->add<Vector2>("uv", Layer::vertex));
        for (std::uint32_t c = 0; c < 24; ++c) {
            const auto v = mesh->positions()[mesh->vertex_points()[c]];
            (*uv)[c] = {v.x + float(c / 4), v.y};
        }
    }
    auto cut = take(solid_boolean(a, b, subtract));
    auto overlap = take(solid_boolean(a, b, intersect));
    auto all = take(solid_boolean(a, b, unite));
    require(std::abs(solid_measures(cut).first - 4) < 1e-6 && std::abs(solid_measures(overlap).first - 4) < 1e-6 &&
                std::abs(solid_measures(all).first - 12) < 1e-6 && std::abs(solid_measures(all).second - 32) < 1e-6,
            "Overlapping boxes produced incorrect volume or retained a coplanar internal face.");
    const auto& operands = column<std::uint32_t>(cut, "boolean_operand", Layer::primitive);
    const auto& primitives = column<std::uint32_t>(cut, "boolean_primitive", Layer::primitive);
    const auto& materials = column<std::uint32_t>(cut, "material", Layer::primitive);
    const auto& uv = column<Vector2>(cut, "uv", Layer::vertex);
    const auto& normals = column<Vector3>(cut, "N", Layer::vertex);
    require(std::ranges::find(operands, 1u) != operands.end(), "Difference did not retain a B wall.");
    for (std::uint32_t p = 0; p < cut.count(Layer::primitive); ++p) {
        require(materials[p] == 0xffffff00u + primitives[p] + 16 * operands[p],
                "Cut wall material lost primitive provenance.");
        const auto range = cut.primitive(p);
        auto pos = [&](auto i) { return cut.positions()[cut.vertex_points()[range.first + i]]; };
        Vector3 area_vector{};
        for (unsigned i = 1; i + 1 < range.count; ++i)
            area_vector += math::cross(pos(i) - pos(0), pos(i + 1) - pos(0));
        auto normal = normalized(area_vector);
        for (unsigned i = 0; i < range.count; ++i) {
            const auto value = uv[range.first + i];
            require(near(value.x, pos(i).x + primitives[p]) && near(value.y, pos(i).y) &&
                        near(normals[range.first + i], normal),
                    "A Boolean seam lost its corner UV or the cut normal points into the solid.");
        }
    }
    auto roundtrip = take(solid_boolean(cut, overlap, unite));
    require(std::abs(solid_measures(roundtrip).first - 8) < 1e-6 &&
                std::abs(solid_measures(roundtrip).second - 24) < 1e-6,
            "Cut and recombine did not restore the box boundary.");
    const auto loose = take(Geometry::points({{1e30f, 0, 0}}));
    const auto with_unused = take(Geometry::merge(std::array{a, loose}));
    require(std::abs(solid_measures(take(solid_boolean(with_unused, b, intersect))).first - 4) < 1e-6,
            "An unreferenced point reduced the solid's working precision.");
    auto diagonal = a;
    take(transform(diagonal, {}, axis_angle({0, 0, 1}, 45), {1, 1, 1}));
    const auto prism = take(solid_boolean(a, diagonal, intersect));
    require(std::abs(solid_measures(prism).first - 16. * (std::sqrt(2.) - 1.)) < 1e-5,
            "Rotated boxes failed the analytic octagonal-prism intersection.");
    auto inner = take(box({1, 1, 1}));
    require(std::abs(solid_measures(take(solid_boolean(a, inner, subtract))).first - 7) < 1e-6,
            "Nested solid cavity disappeared.");
    auto touching = take(box({2, 2, 2}));
    take(transform(touching, {2, 0, 0}, {}, {1, 1, 1}));
    require(take(solid_boolean(a, touching, intersect)).count(Layer::point) == 0 &&
                std::abs(solid_measures(take(solid_boolean(a, touching, unite))).second - 40) < 1e-6 &&
                take(solid_boolean(a, a, subtract)).count(Layer::point) == 0 &&
                std::abs(solid_measures(take(solid_boolean(a, {}, unite))).first - 8) < 1e-6,
            "Boolean failed on face contact, coincident shells, or empty input.");
    take(transform(touching, {0, 2, 0}, {}, {1, 1, 1}));
    auto separate = take(solid_boolean(a, touching, unite));
    separate = take(solid_boolean(separate, {}, unite));
    take(connectivity(separate));
    require(std::ranges::max(column<std::uint32_t>(separate, "piece", Layer::point)) == 1,
            "Edge contact welded two disconnected solid fans.");
    require(!solid_boolean(a, triangles({{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}), intersect),
            "Boolean accepted an open triangle.");
    auto sphere_a = take(sphere(1, 12, 6));
    auto sphere_b = sphere_a;
    take(transform(sphere_b, {.5f, 0, 0}, {}, {1, 1, 1}));
    auto sphere_cut = take(solid_boolean(sphere_a, sphere_b, subtract));
    auto sphere_intersection = take(solid_boolean(sphere_a, sphere_b, intersect));
    auto sphere_volume = solid_measures(sphere_a).first;
    require(std::abs(solid_measures(sphere_cut).first + solid_measures(sphere_intersection).first - sphere_volume) <
                1e-5,
            "Two intersecting sphere meshes lost volume.");

    auto restored = sphere_a;
    const auto expected_area = solid_measures(sphere_a).second;
    for (unsigned round = 0; round < 30; ++round) {
        if (round) {
            sphere_cut = take(solid_boolean(restored, sphere_b, subtract));
            sphere_intersection = take(solid_boolean(restored, sphere_b, intersect));
        }
        restored = take(solid_boolean(sphere_cut, sphere_intersection, unite));
        auto [volume, area] = solid_measures(restored);
        require(std::abs(volume - sphere_volume) < 1e-5 && std::abs(area - expected_area) < 1e-4,
                "Repeated Boolean sphere reconstruction changed volume or boundary area.");
    }

    auto spatial = take(Bvh::build(restored));
    const auto hit = spatial.ray({.04f, .09f, 3.f}, {0.f, 0.f, -1.f}, 10.f);
    const auto ref = take(Bvh::build(sphere_a)).ray({.04f, .09f, 3.f}, {0.f, 0.f, -1.f}, 10.f);
    require(ref && hit && near(hit->position, ref->position), "Ray cannot consume the reconstructed sphere surface.");
    // A point edit on a COW copy invalidates only that copy's exact boundary.
    auto painted = roundtrip;
    auto* labels = take(painted.write<std::uint32_t>("material", Layer::primitive));
    std::ranges::fill(*labels, 0xfffffffeu);
    auto inherited = take(solid_boolean(painted, {}, unite));
    require(std::ranges::all_of(column<std::uint32_t>(inherited, "material", Layer::primitive),
                                [](auto label) { return label == 0xfffffffeu; }),
            "Boolean reused stale attributes after repainting an exact boundary.");
    auto moved = roundtrip;
    take(transform(moved, {3, 0, 0}, {}, {1, 1, 1}));
    require(take(solid_boolean(moved, a, intersect)).count(Layer::point) == 0 &&
                std::abs(solid_measures(take(solid_boolean(roundtrip, a, intersect))).first - 8) < 1e-6,
            "Boolean used stale exact positions or invalidated the original copy.");
    auto independent = take(box({4, 4, 4}));
    take(transform(independent, {3, 0, 0}, {}, {1, 1, 1}));
    auto cached_large = take(solid_boolean(independent, {}, unite));
    auto joined = take(solid_boolean(roundtrip, cached_large, unite));
    require(std::abs(solid_measures(joined).first - 72) < 1e-6,
            "Boolean lost precision combining independent lattice frames.");
    auto opened = roundtrip;
    auto* removed = take(opened.add<bool>("remove_face", Layer::primitive));
    (*removed)[0] = true;
    opened = take(opened.remove_flagged(Layer::primitive, "remove_face"));
    require(!solid_boolean(opened, a, intersect), "Boolean reused exact boundaries after opening a shell.");

    Runtime runtime;
    take(register_types(runtime));
    Meshes resources;
    auto graph = take(runtime.create(type<Graph>(runtime)));
    auto box_node = take(add_node(runtime, graph.id(), type<Box>(runtime)));
    auto move = take(add_node(runtime, graph.id(), type<Transform>(runtime)));
    auto op = take(add_node(runtime, graph.id(), type<Boolean>(runtime)));
    take(runtime.with_native<Transform>(move.id(), [](Transform& n) -> Result<void> {
        n.translate = {.5f, 0, 0};
        return {};
    }));
    take(runtime.with_native<Boolean>(op.id(), [](Boolean& n) -> Result<void> {
        n.operation = BooleanOperation::intersect;
        return {};
    }));
    take(connect(runtime, graph.id(), box_node.id(), 0, move.id(), 0));
    take(connect(runtime, graph.id(), box_node.id(), 0, op.id(), 0));
    take(connect(runtime, graph.id(), move.id(), 0, op.id(), 1));
    auto display = take(add_node(runtime, graph.id(), type<Connectivity>(runtime)));
    take(connect(runtime, graph.id(), op.id(), 0, display.id(), 0));
    take(set_display(runtime, graph.id(), display.id()));
    Evaluator evaluator{runtime, graph, resources};
    take(evaluator.evaluate());
    require(evaluator.output() && std::abs(solid_measures(*evaluator.output()).first - .5) < 1e-6,
            "The reflected two-input Boolean SOP did not cook the selected operation.");
    const auto first_stats = evaluator.status(op.id())->boolean_stats;
    require(first_stats && first_stats->input_faces == 12 && first_stats->candidate_pairs && first_stats->splits &&
                first_stats->peak_face_scratch_bytes && !evaluator.status(display.id())->boolean_stats,
            "Live Boolean diagnostics did not capture the actual SOP cook.");
    const auto preview = evaluator.retain_last_valid_output();
    const auto set_cutter_scale = [&](Vector3 scale) {
        take(runtime.write(move.id(), field(runtime, type<Transform>(runtime), "scale"),
                           ValueInput{type<Vector3>(runtime), &scale}));
        take(evaluator.evaluate());
    };
    set_cutter_scale({1, 0x1p-30f, 1});
    const auto* failed = evaluator.status(op.id());
    const auto* downstream = evaluator.status(display.id());
    require(!evaluator.output() && evaluator.last_valid_output() == preview.get() && failed && failed->error &&
                failed->error->message.contains("Boolean prepare B: Primitive ") &&
                failed->error->message.contains("float coordinate precision") && downstream && downstream->error &&
                downstream->error->message.contains("procedural.Boolean"),
            "A sub-grid cutter lost the source-face precision diagnostic or the previous display preview.");
    require(failed->boolean_stats && failed->boolean_stats != first_stats && failed->boolean_stats->input_faces == 6 &&
                !failed->boolean_stats->candidate_pairs && failed->error->message.contains("prepare B"),
            "A precision rejection retained stale work or lost its failing phase.");
    set_cutter_scale({1, .5f, 1});
    require(evaluator.output() && std::abs(solid_measures(*evaluator.output()).first - .25) < 1e-6 &&
                !evaluator.status(op.id())->error && !evaluator.status(display.id())->error,
            "Editing the cutter back to representable thickness did not recover the downstream display.");
    const auto last_cook = evaluator.status(op.id())->boolean_stats;
    const auto evaluations = evaluator.status(op.id())->evaluations;
    set_cutter_scale({1, 1, 1});
    require(last_cook && evaluator.status(op.id())->boolean_stats == last_cook &&
                evaluator.status(op.id())->evaluations == evaluations && evaluator.status(op.id())->cache_hits,
            "Returning to a cached Boolean result replaced its last actual cook measurements.");
}

void boolean_contact_sweep() {
    // Both the outer shell and its inward cavity enclose the probe AABB.
    // Neither has surface contact with it. Their winding contributions cancel.
    auto cavity = take(solid_boolean(take(box({6, 6, 6})), take(box({2, 2, 2})), BooleanOperation::subtract));
    auto probe = take(box({.5f, .5f, .5f}));
    require(take(solid_boolean(cavity, probe, BooleanOperation::intersect)).count(Layer::primitive) == 0,
            "Shell pruning filled a nested cavity with no surface contact.");
    take(transform(probe, {2, 0, 0}, {}, {1, 1, 1}));
    require(std::abs(solid_measures(take(solid_boolean(cavity, probe, BooleanOperation::intersect))).first - .125) <
                1e-8,
            "Shell pruning discarded an enclosing solid with no surface contact.");
    // One connected boundary encloses a rectangular through-hole. The probe
    // crosses four walls in the center, then joins into one piece as it moves.
    const auto ring = take(solid_boolean(take(box({6, 6, 1})), take(box({2, 2, 2})), BooleanOperation::subtract));
    for (const float y : {0.f, 1.f, 1.25f}) {
        auto bar = take(box({8, .5f, .5f}));
        take(transform(bar, {0, y, 0}, {}, {1, 1, 1}));
        auto clipped = take(solid_boolean(bar, ring, BooleanOperation::intersect));
        const double gap = std::clamp(1.25 - double(y), 0., .5);
        require(std::abs(solid_measures(clipped).first - (3. - 2. * gap) * .5) < 1e-6,
                "A winding query lost the through-hole or crossed a contact boundary.");
        take(connectivity(clipped));
        require(std::ranges::max(column<std::uint32_t>(clipped, "piece", Layer::point)) == (y == 0 ? 1u : 0u),
                "Shared-point classification joined separate sections of a concave solid.");
    }
    const auto empty = take(solid_boolean({}, {}, BooleanOperation::intersect));
    auto fresh = take(box({.25f, .25f, .25f}));
    take(transform(fresh, {1, 1, 1}, {}, {1, 1, 1}));
    const auto after_empty = take(solid_boolean(empty, fresh, BooleanOperation::unite));
    require(std::abs(solid_measures(after_empty).first - 1. / 64.) < 1e-8,
            "An empty upstream Boolean imposed its lattice on a fresh operand.");
    auto curved = take(sphere(1, 24, 12));
    const auto curved_volume = solid_measures(curved).first;
    for (float dx : {.375f, .5f, .625f}) {
        auto cutter = curved;
        take(transform(cutter, {dx, 0, 0}, {}, {1, 1, 1}));
        auto rest = take(solid_boolean(curved, cutter, BooleanOperation::subtract));
        auto inside = take(solid_boolean(curved, cutter, BooleanOperation::intersect));
        require(std::abs(solid_measures(rest).first + solid_measures(inside).first - curved_volume) < 1e-5,
                "Moving a curved cutter changed the combined volume.");
        if (dx == .5f) {
            const auto rebuilt = take(solid_boolean(rest, inside, BooleanOperation::unite));
            const auto [v, area] = solid_measures(rebuilt);
            require(std::abs(v - curved_volume) < 1e-5 && std::abs(area - solid_measures(curved).second) < 1e-4,
                    "A curved cut with interior intersection endpoints could not be sewn back.");
        }
    }
    auto stock = take(box({2, 2, 2}));
    auto tool = stock;
    take(stock.add<std::uint32_t>("material", Layer::primitive, 10));
    take(tool.add<std::uint32_t>("material", Layer::primitive, 20));
    constexpr float step = 1.f / 1024;
    const std::array path{Vector3{2 + step, 2, 2}, Vector3{2, 2, 2},        Vector3{2, 2, 0},
                          Vector3{2, 0, 0},        Vector3{2 - step, 0, 0}, Vector3{1.75f, 0, 0}};
    Geometry cut;
    for (auto center : path) {
        auto cutter = tool;
        take(transform(cutter, center, {}, {1, 1, 1}));
        cut = take(solid_boolean(stock, cutter, BooleanOperation::subtract));
        const double width = std::min(2.f, center.x);
        const auto [volume, area] = solid_measures(cut);
        require(std::abs(volume - 4 * width) < 1e-6 && std::abs(area - (8 + 8 * width)) < 1e-6,
                "Moving cutter changed stock volume or exposed area at contact.");
        const auto overlap = take(solid_boolean(stock, cutter, BooleanOperation::intersect));
        require(std::abs(solid_measures(overlap).first - (8 - volume)) < 1e-6 &&
                    (center.x < 2 || overlap.count(Layer::point) == 0),
                "Moving cutter failed to distinguish boundary contact from a thin solid intersection.");
    }
    // The +Z quad's first corner alone has weight 1. Its authored diagonal
    // crosses x=0 at (0,0,1) with weight 1/2. Removing that seam changes the field.
    auto painted = stock;
    auto* weights = take(painted.add<float>("weight", Layer::vertex));
    (*weights)[painted.primitive(5).first] = 1.f;
    auto halfway = tool;
    take(transform(halfway, {1, 0, 0}, {}, {1, 1, 1}));
    const auto weighted = take(solid_boolean(painted, halfway, BooleanOperation::subtract));
    const auto& values = column<float>(weighted, "weight", Layer::vertex);
    const auto& sources = column<std::uint32_t>(weighted, "boolean_primitive", Layer::primitive);
    const auto& operands = column<std::uint32_t>(weighted, "boolean_operand", Layer::primitive);
    bool found_diagonal = false;
    for (std::uint32_t p = 0; p < weighted.count(Layer::primitive); ++p) {
        if (operands[p] != 0 || sources[p] != 5)
            continue;
        auto face = weighted.primitive(p);
        for (std::uint32_t c = face.first; c < face.first + face.count; ++c) {
            if (weighted.positions()[weighted.vertex_points()[c]] == Vector3{0, 0, 1}) {
                require(values[c] == .5f, "Boolean changed the field across a painted face diagonal.");
                found_diagonal = true;
            }
        }
    }
    require(found_diagonal, "Boolean discarded a semantically significant source diagonal.");
    // A box rotating about its center at x=2 sweeps a triangular wedge into
    // the stock on either side of its face-contact pose. The wedge has height 2.
    for (float degrees : {-2.f, -.25f, 0.f, .25f, 2.f}) {
        auto cutter = tool;
        take(transform(cutter, {2, 0, 0}, axis_angle({0, 0, 1}, degrees), {1, 1, 1}));
        const double angle = std::abs(degrees) * std::numbers::pi / 180.;
        const double c = std::cos(angle);
        const double s = std::sin(angle);
        const auto depth = c + s - 1.;
        const double wedge = s ? depth * depth / (c * s) : 0.;
        auto solve = [&](BooleanOperation op) {
            auto result = solid_boolean(stock, cutter, op);
            if (!result)
                fatal("Rotating cutter at " + std::to_string(degrees) + " degrees: " + result.error().message);
            return std::move(*result);
        };
        const auto clipped = solve(BooleanOperation::intersect);
        const auto rest = solve(BooleanOperation::subtract);
        require(std::abs(solid_measures(clipped).first - wedge) < 1e-6 &&
                    std::abs(solid_measures(rest).first - (8. - wedge)) < 1e-6 &&
                    (degrees != 0 || clipped.count(Layer::point) == 0),
                "Rotating through face contact changed the analytic wedge volume.");
    }
    auto cross_tool = take(box({4, 2, 4}));
    take(cross_tool.add<std::uint32_t>("material", Layer::primitive, 30));
    take(transform(cross_tool, {0, 1, 0}, {}, {1, 1, 1}));
    auto crossed = take(solid_boolean(cut, cross_tool, BooleanOperation::subtract));
    const auto [volume, area] = solid_measures(crossed);
    require(std::abs(volume - 3.5) < 1e-6 && std::abs(area - 14.5) < 1e-6,
            "A second cut across the first tool wall changed volume or exposed area.");
    const auto& material = column<std::uint32_t>(crossed, "material", Layer::primitive);
    const auto& operand = column<std::uint32_t>(crossed, "boolean_operand", Layer::primitive);
    unsigned seen = 0;
    for (std::uint32_t p = 0; p < crossed.count(Layer::primitive); ++p) {
        const auto face = crossed.primitive(p);
        bool first_wall = true;
        bool second_wall = true;
        for (std::uint32_t j = 0; j < face.count; ++j) {
            auto v = crossed.positions()[crossed.vertex_points()[face.first + j]];
            first_wall &= near(v.x, .75f);
            second_wall &= near(v.y, 0.f);
        }
        const unsigned surface = second_wall ? 2u : first_wall ? 1u : 0u;
        require(material[p] == 10 * (surface + 1) && operand[p] == (surface == 2),
                "A consecutive cut lost wall material or assigned the wrong current operand.");
        seen |= 1u << surface;
    }
    require(seen == 7, "A consecutive cut lost the stock surface or one of its two tool walls.");
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
    const auto tagged = add(type<Connectivity>(runtime));
    const auto copied_root = add(type<CopyToPoints>(runtime));
    const auto grid_node = grid_root.id();
    const auto moved = moved_root.id();
    const auto line_node = line_root.id();
    const auto merged = merged_root.id();
    const auto copied = copied_root.id();
    const auto vector2 = type<Vector2>(runtime);
    const auto vector3 = type<Vector3>(runtime);
    const auto translate = field(runtime, type<Transform>(runtime), "translate");
    const auto write_vector = [&](ObjectId node, FieldId at, Vector3 value) {
        take(runtime.write(node, at, ValueInput{vector3, &value}));
    };
    const Vector2 size{3, 2};
    take(runtime.write(grid_node, field(runtime, type<Grid>(runtime), "size"), ValueInput{vector2, &size}));
    take(runtime.write(grid_node, field(runtime, type<Grid>(runtime), "rows"), Scalar{std::uint32_t{3}}));
    take(runtime.write(grid_node, field(runtime, type<Grid>(runtime), "columns"), Scalar{std::uint32_t{4}}));
    write_vector(moved, translate, {1, 0, 0});
    take(runtime.with_native<CopyToPoints>(copied, [](CopyToPoints& node) -> Result<void> {
        node.meshes.add(String::literal<"fixture/a">());
        return {};
    }));
    take(connect(runtime, graph, grid_node, 0, moved, 0));
    take(connect(runtime, graph, moved, 0, merged, 0));
    take(connect(runtime, graph, line_node, 0, merged, 0));
    take(connect(runtime, graph, merged, 0, tagged.id(), 0));
    take(connect(runtime, graph, tagged.id(), 0, copied, 0));
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
    const auto* classified = evaluator.node_output(tagged.id());
    require(classified && evaluator.node_output(merged) && !evaluator.node_output(merged)->find("piece"),
            "Connectivity failed to publish an independent graph result.");
    const auto& pieces = column<std::uint32_t>(*classified, "piece", Layer::point);
    require(pieces.size() == 14, "Connectivity lost a Grid or Line point.");
    for (std::uint32_t i = 0; i < pieces.size(); ++i)
        require(pieces[i] == i, "Grid and Line produce loose points. Each needs its own piece.");
}

void source_and_spatial_contracts() {
    // 1. -0.0f seam welding, Bvh::acquire cache identity, Geometry::set usage preservation, and Scatter min_distance.
    std::vector<geometry::Vertex> seam_vertices{
        {{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},  {{0.0f, 0.0f, 2.0f}, {0.0f, 1.0f, 0.0f}},
        {{2.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},  {{2.0f, -0.0f, -0.0f}, {0.0f, 1.0f, 0.0f}},
        {{-0.0f, 0.0f, 2.0f}, {0.0f, 1.0f, 0.0f}}, {{2.0f, 0.0f, 2.0f}, {0.0f, 1.0f, 0.0f}},
    };
    const auto seam_mesh = take(geometry::Mesh::create(std::move(seam_vertices)));
    auto seam_surface = import_mesh(*seam_mesh);
    require(seam_surface.count(Layer::point) == 4 && seam_surface.count(Layer::primitive) == 2,
            "import_mesh failed to weld -0.0f and +0.0f seam vertices.");
    const auto bvh_first = take(Bvh::acquire(seam_surface));
    auto seam_copy = seam_surface;
    const auto bvh_second = take(Bvh::acquire(seam_copy));
    require(bvh_first != nullptr && bvh_first == bvh_second,
            "Bvh::acquire did not reuse cached BVH across COW copies.");
    (*take(seam_copy.write<Vector3>("P", Layer::point)))[0].y = 1.0f;
    const auto bvh_mutated = take(Bvh::acquire(seam_copy));
    require(bvh_mutated != nullptr && bvh_mutated != bvh_first, "Bvh::acquire served a stale BVH after P mutated.");

    auto orient_geo = take(Geometry::points(Array<Vector3>{{0, 0, 0}, {1, 0, 0}}));
    take(orient_geo.add<Quaternion>("orient", Layer::point, Quaternion{}));
    require(orient_geo.find("orient") && orient_geo.find("orient")->usage == Usage::orientation,
            "Convention-named 'orient' column did not acquire Usage::orientation.");
    SharedArray<Quaternion> replacement_orient{Array<Quaternion>{axis_angle({0, 1, 0}, 90), Quaternion{}}};
    take(orient_geo.set(String::literal<"orient">(), Layer::point, replacement_orient));
    require(orient_geo.find("orient")->usage == Usage::orientation,
            "Geometry::set stripped Usage::orientation from an existing column.");
    require(!orient_geo.set(String::literal<"orient">(), Layer::detail,
                            SharedArray<Quaternion>{Array<Quaternion>{Quaternion{}}}),
            "Geometry::set accepted a cross-layer replacement.");
    require(!orient_geo.set(String::literal<"orient">(), Layer::point, SharedArray<float>{Array<float>{0.0f, 0.0f}}),
            "Geometry::set accepted a cross-type replacement.");

    const auto spaced_a = take(scatter(seam_surface, {.density = 40.0f, .seed = 9, .min_distance = 0.35f}));
    const auto spaced_b = take(scatter(seam_surface, {.density = 40.0f, .seed = 9, .min_distance = 0.35f}));
    const auto& spaced_p = spaced_a.positions();
    require(spaced_p.size() > 8 && std::ranges::equal(spaced_p, spaced_b.positions()),
            "Scatter min_distance lost determinism or candidate retention.");
    for (std::uint32_t i = 0; i < spaced_p.size(); ++i)
        for (std::uint32_t j = i + 1; j < spaced_p.size(); ++j)
            require(math::distance_squared(spaced_p[i], spaced_p[j]) >= 0.35f * 0.35f - 1e-5f,
                    "Scatter min_distance left points closer than the threshold.");

    // 2. Mixed C++ + Engine .source + Custom .source SOP graph.
    Runtime runtime;
    take(register_types(runtime));
    auto files = take(source::engine_sources(source::default_engine_root()));
    files.push_back({"Source/Wave.source", 1, R"(
class game.Wave : procedural.Node {
    float lift = 0.25f;
    void process([InOut] Array<vector3>& P,
                 [Out] Array<float>& custom_h,
                 [Group("selected")] const Array<bool>& mask) {
        const uint n = P.size();
        for (uint i = 0; i < n; i = i + 1) {
            if (mask.size() == n && !mask[i]) { continue; }
            P[i] = P[i] + vector3(0.0f, self.lift, 0.0f);
            custom_h[i] = P[i].y;
        }
    }
}
class game.FrameInputs : procedural.Node {
    vector3 normal;
    vector3 point_up;
    void process([Out] Array<vector3>& N, [Out] Array<vector3>& up) {
        for (uint i = 0; i < N.size(); i = i + 1) {
            N[i] = self.normal;
            up[i] = self.point_up;
        }
    }
}
)"});
    auto program = take(source::Program::create(runtime));
    {
        auto req = take(program->request({"procedural_test", 1, std::move(files)}));
        auto cand = take(source::Program::prepare(req));
        take(program->try_publish(cand));
    }

    const auto jitter_type = take(runtime.find_type("procedural.Jitter"));
    const auto rscale_type = take(runtime.find_type("procedural.RandomScale"));
    const auto align_type = take(runtime.find_type("procedural.NormalAlign"));
    const auto noise_type = take(runtime.find_type("procedural.Noise"));
    const auto remap_type = take(runtime.find_type("procedural.Remap"));
    const auto wave_type = take(runtime.find_type("game.Wave"));
    const auto jitter_ports = take(ports(runtime, jitter_type));
    require(jitter_ports.inputs == std::vector<std::string>{"input"} &&
                jitter_ports.outputs == std::vector<std::string>{"output"},
            "Source Node ports(runtime, type) failed without Program.");

    Meshes resources;
    resources.meshes["fixture/cube"] = geometry::cube_mesh();
    const auto graph_root = take(runtime.create(type<Graph>(runtime)));
    const auto graph = graph_root.id();
    take(runtime.write(graph, field(runtime, type<Graph>(runtime), "seed"), Scalar{std::uint32_t{11}}));
    const auto add = [&](TypeId node_type) { return take(add_node(runtime, graph, node_type)); };
    const auto grid_n = add(type<Grid>(runtime));
    const auto group_n = add(type<GroupBox>(runtime));
    const auto jitter_n = add(jitter_type);
    const auto rscale_n = add(rscale_type);
    const auto align_n = add(align_type);
    const auto wave_n = add(wave_type);
    const auto xform_n = add(type<Transform>(runtime));
    const auto copy_n = add(type<CopyToPoints>(runtime));

    const auto write_str = [&](ObjectId obj, TypeId owner, const char* name, std::string_view text) {
        String s;
        require(s.assign(StringView{text}).has_value(), "Invalid UTF-8 string.");
        take(runtime.write(obj, field(runtime, owner, name), ValueInput{type<String>(runtime), &s}));
    };
    const auto write_v3 = [&](ObjectId obj, TypeId owner, const char* name, Vector3 v) {
        take(runtime.write(obj, field(runtime, owner, name), ValueInput{type<Vector3>(runtime), &v}));
    };
    take(runtime.with_native<Grid>(grid_n.id(), [](Grid& n) -> Result<void> {
        n.size = {2.0f, 2.0f};
        n.rows = 2;
        n.columns = 2;
        return {};
    }));
    take(runtime.with_native<GroupBox>(group_n.id(), [](GroupBox& n) -> Result<void> {
        n.minimum = {0.0f, -1.0f, -2.0f};
        n.maximum = {2.0f, 1.0f, 2.0f};
        return {};
    }));
    write_str(jitter_n.id(), jitter_type, "group", "selected");
    take(runtime.write(jitter_n.id(), field(runtime, jitter_type, "amount"), Scalar{0.5f}));
    write_v3(jitter_n.id(), jitter_type, "axis_scale", {1.0f, 0.0f, 0.0f});
    take(runtime.write(jitter_n.id(), field(runtime, jitter_type, "seed"), Scalar{std::uint32_t{5}}));
    write_str(rscale_n.id(), rscale_type, "group", "selected");
    take(runtime.write(rscale_n.id(), field(runtime, rscale_type, "minimum"), Scalar{2.0f}));
    take(runtime.write(rscale_n.id(), field(runtime, rscale_type, "maximum"), Scalar{2.0f}));
    write_str(align_n.id(), align_type, "group", "selected");
    write_v3(align_n.id(), align_type, "up", {0.0f, 0.0f, 1.0f});
    take(runtime.with_native<Transform>(xform_n.id(), [](Transform& n) -> Result<void> {
        n.rotate = axis_angle({0.0f, 1.0f, 0.0f}, 90.0f);
        return {};
    }));
    take(runtime.with_native<CopyToPoints>(copy_n.id(), [](CopyToPoints& n) -> Result<void> {
        n.meshes.add(String::literal<"fixture/cube">());
        return {};
    }));

    take(connect(runtime, graph, grid_n.id(), 0, group_n.id(), 0));
    take(connect(runtime, graph, group_n.id(), 0, jitter_n.id(), 0));
    take(connect(runtime, graph, jitter_n.id(), 0, rscale_n.id(), 0));
    take(connect(runtime, graph, rscale_n.id(), 0, align_n.id(), 0));
    take(connect(runtime, graph, align_n.id(), 0, wave_n.id(), 0));
    take(connect(runtime, graph, wave_n.id(), 0, xform_n.id(), 0));
    take(connect(runtime, graph, xform_n.id(), 0, copy_n.id(), 0));
    take(set_display(runtime, graph, xform_n.id()));

    Evaluator evaluator{runtime, graph_root, resources, program.get()};
    take(evaluator.evaluate());
    const auto* pts = evaluator.output();
    require(pts != nullptr && pts->count(Layer::point) == 4, "Mixed C++/Source SOP chain failed.");
    const auto& out_p = pts->positions();
    const auto& pscale = column<float>(*pts, "pscale", Layer::point);
    const auto& custom_h = column<float>(*pts, "custom_h", Layer::point);
    const auto& orient = column<Quaternion>(*pts, "orient", Layer::point);
    require(pts->find("orient")->usage == Usage::orientation, "Transform lost Usage::orientation on 'orient'.");
    // Unselected point 0 started at (-1, 0, -1); +90 deg about Y in row-vector convention maps (x, y, z) -> (z, y, -x) = (-1, 0, 1).
    // With RandomScale missing prior `pscale`, unselected points receive unit scale 1.0f rather than 0.0f.
    const std::uint32_t jseed = hash(11u, 5u);
    const std::uint32_t h1 = hash(hash(jseed, 1u));
    const float expected_x1 = 1.0f + (unit(h1) * 2.0f - 1.0f) * 0.5f;
    require(near(out_p[0], {-1.0f, 0.0f, 1.0f}) && near(out_p[1], {-1.0f, 0.25f, -expected_x1}) && pscale[0] == 1.0f &&
                pscale[1] == 2.0f && custom_h[0] == 0.0f && custom_h[1] == 0.25f,
            "Jitter fallback index, GroupBox mask, RandomScale unit default or custom Wave produced wrong values.");
    // Selected point 1 had NormalAlign up=(0,0,1) so +Z -> +Y and +Y -> +Z; unselected point 0 had no prior orient,
    // so NormalAlign preserved CopyToPoints' default shortest-arc (+Z -> +Y, +Y -> -Z). After +90 deg about Y:
    require(near(math::rotate(orient[0], Vector3{0, 0, 1}), {0, 1, 0}, 1e-4f) &&
                near(math::rotate(orient[0], Vector3{0, 1, 0}), {-1, 0, 0}, 1e-4f) &&
                near(math::rotate(orient[1], Vector3{0, 0, 1}), {0, 1, 0}, 1e-4f) &&
                near(math::rotate(orient[1], Vector3{0, 1, 0}), {1, 0, 0}, 1e-4f),
            "Grouped NormalAlign rotated unselected point away from N or Transform lost Usage::orientation.");

    take(set_display(runtime, graph, copy_n.id()));
    take(evaluator.evaluate());
    require(evaluator.output() && evaluator.output()->count(Layer::instance) == 4,
            "CopyToPoints failed after Source chain.");
    const auto& inst_mats = evaluator.output()->instance_transforms();
    require(near(Vector3{inst_mats[0](1, 0), inst_mats[0](1, 1), inst_mats[0](1, 2)}, {-1.0f, 0.0f, 0.0f}, 1e-4f) &&
                near(Vector3{inst_mats[0](2, 0), inst_mats[0](2, 1), inst_mats[0](2, 2)}, {0.0f, 1.0f, 0.0f}, 1e-4f) &&
                near(Vector3{inst_mats[1](1, 0), inst_mats[1](1, 1), inst_mats[1](1, 2)}, {2.0f, 0.0f, 0.0f}, 1e-4f) &&
                near(Vector3{inst_mats[1](2, 0), inst_mats[1](2, 1), inst_mats[1](2, 2)}, {0.0f, 2.0f, 0.0f}, 1e-4f),
            "CopyToPoints instance matrices lost unselected N alignment or selected up/pscale.");
    const auto pub = take(publish(*evaluator.output(), 32.0f));
    std::size_t kept_instances = 0;
    for (const auto& col : pub.collections)
        kept_instances += col.transforms->size();
    require(pub.dropped == 0 && kept_instances == 4, "Grouped RandomScale dropped unselected unit-scale instances.");

    // Chaining a second grouped RandomScale and NormalAlign when `pscale` and `orient` already exist
    // must preserve prior values on unselected points.
    const auto group_z_n = add(type<GroupBox>(runtime));
    const auto rscale2_n = add(rscale_type);
    const auto align2_n = add(align_type);
    take(runtime.with_native<GroupBox>(group_z_n.id(), [](GroupBox& n) -> Result<void> {
        n.group = String::literal<"z_pos">();
        n.minimum = {-2.0f, -1.0f, 0.0f};
        n.maximum = {2.0f, 1.0f, 2.0f};
        return {};
    }));
    write_str(rscale2_n.id(), rscale_type, "group", "z_pos");
    take(runtime.write(rscale2_n.id(), field(runtime, rscale_type, "minimum"), Scalar{3.0f}));
    take(runtime.write(rscale2_n.id(), field(runtime, rscale_type, "maximum"), Scalar{3.0f}));
    write_str(align2_n.id(), align_type, "group", "z_pos");
    write_v3(align2_n.id(), align_type, "up", {1.0f, 0.0f, 0.0f});
    take(connect(runtime, graph, align_n.id(), 0, group_z_n.id(), 0));
    take(connect(runtime, graph, group_z_n.id(), 0, rscale2_n.id(), 0));
    take(connect(runtime, graph, rscale2_n.id(), 0, align2_n.id(), 0));
    take(set_display(runtime, graph, align2_n.id()));
    take(evaluator.evaluate());
    const auto& pscale2 = column<float>(*evaluator.output(), "pscale", Layer::point);
    const auto& orient2 = column<Quaternion>(*evaluator.output(), "orient", Layer::point);
    require(pscale2[0] == 1.0f && pscale2[1] == 2.0f && pscale2[2] == 3.0f && pscale2[3] == 3.0f &&
                near(math::rotate(orient2[1], Vector3{0, 1, 0}), {0, 0, 1}, 1e-4f) &&
                near(math::rotate(orient2[3], Vector3{0, 1, 0}), {1, 0, 0}, 1e-4f),
            "Grouped RandomScale or NormalAlign did not preserve existing values on unselected points.");

    const auto frame_type = take(runtime.find_type("game.FrameInputs"));
    const auto frame_n = add(frame_type);
    write_v3(frame_n.id(), frame_type, "point_up", {std::numeric_limits<float>::quiet_NaN(), 0, 0});
    take(connect(runtime, graph, group_n.id(), 0, frame_n.id(), 0));
    take(connect(runtime, graph, frame_n.id(), 0, copy_n.id(), 0));
    take(set_display(runtime, graph, copy_n.id()));
    take(evaluator.evaluate());
    require(evaluator.output() && evaluator.output()->count(Layer::instance) == 4,
            "CopyToPoints failed to instance a zero normal with unused non-finite up.");
    const auto direct_transforms = evaluator.output()->instance_transforms();
    take(connect(runtime, graph, frame_n.id(), 0, align_n.id(), 0));
    take(connect(runtime, graph, align_n.id(), 0, copy_n.id(), 0));
    take(evaluator.evaluate());
    require(evaluator.output() && std::ranges::equal(direct_transforms, evaluator.output()->instance_transforms()),
            "Grouped NormalAlign changed the zero-normal fallback used by CopyToPoints.");
    write_v3(frame_n.id(), frame_type, "normal", {0, 1, 0});
    take(evaluator.evaluate());
    const auto* align_status = evaluator.status(align_n.id());
    require(!evaluator.output() && align_status && align_status->error &&
                align_status->error->message.contains("NormalAlign point up must be finite"),
            "Grouped NormalAlign accepted a non-finite up used by a nonzero normal.");

    // 3. Dynamic [RunOver], same-name Remap read/write, and Evaluator error/recovery contract.
    const auto box_n = add(type<Box>(runtime));
    const auto noise_n = add(noise_type);
    const auto promote_n = add(type<Promote>(runtime));
    const auto remap_n = add(remap_type);
    const auto tail_n = add(type<Transform>(runtime));
    const auto layer_enum = type<Layer>(runtime);
    take(runtime.with_native<Promote>(promote_n.id(), [](Promote& n) -> Result<void> {
        n.name = String::literal<"noise">();
        n.to = Layer::primitive;
        n.reduce = Reduce::average;
        return {};
    }));
    take(runtime.write(remap_n.id(), field(runtime, remap_type, "layer"),
                       Scalar{EnumValue{layer_enum, static_cast<std::uint32_t>(Layer::primitive)}}));
    write_str(remap_n.id(), remap_type, "source_attribute", "noise");
    write_str(remap_n.id(), remap_type, "attribute", "noise");
    take(runtime.write(remap_n.id(), field(runtime, remap_type, "output_min"), Scalar{10.0f}));
    take(runtime.write(remap_n.id(), field(runtime, remap_type, "output_max"), Scalar{20.0f}));
    take(connect(runtime, graph, box_n.id(), 0, noise_n.id(), 0));
    take(connect(runtime, graph, noise_n.id(), 0, promote_n.id(), 0));
    take(connect(runtime, graph, promote_n.id(), 0, remap_n.id(), 0));
    take(connect(runtime, graph, remap_n.id(), 0, tail_n.id(), 0));

    take(set_display(runtime, graph, promote_n.id()));
    take(evaluator.evaluate());
    const Array<float> before_remap = column<float>(*evaluator.output(), "noise", Layer::primitive);
    take(set_display(runtime, graph, tail_n.id()));
    take(evaluator.evaluate());
    const auto* valid_box = evaluator.output();
    require(valid_box != nullptr && valid_box->count(Layer::primitive) == 6, "Primitive RunOver Remap failed.");
    const auto& after_remap = column<float>(*valid_box, "noise", Layer::primitive);
    for (std::uint32_t p = 0; p < 6; ++p)
        require(near(after_remap[p], 10.0f + before_remap[p] * 10.0f),
                "Same-name primitive Remap did not read old input before writing.");

    // Trigger failure in Remap: output() must become nullptr while last_valid_output() retains prior geometry,
    // and downstream Transform status must name procedural.Remap as root cause.
    take(runtime.write(remap_n.id(), field(runtime, remap_type, "input_max"), Scalar{0.0f}));
    take(evaluator.evaluate());
    const auto remap_status = evaluator.status(remap_n.id());
    const auto tail_status = evaluator.status(tail_n.id());
    require(evaluator.output() == nullptr && evaluator.last_valid_output() != nullptr &&
                evaluator.last_valid_output()->count(Layer::primitive) == 6 && remap_status && remap_status->error &&
                tail_status && tail_status->error &&
                tail_status->error->message.contains("procedural.Remap: Remap endpoints must be finite"),
            "Failed display chain served stale output() or lost upstream root-cause error.");

    // Restore valid input_max = 1.0f: hits cache and restores output().
    take(runtime.write(remap_n.id(), field(runtime, remap_type, "input_max"), Scalar{1.0f}));
    take(evaluator.evaluate());
    require(evaluator.output() != nullptr && evaluator.status(remap_n.id())->cache_hits == 1 &&
                !evaluator.status(tail_n.id())->error.has_value(),
            "Restoring valid parameter did not recover from cache.");

    // 4. Native column-binding length tampering (both attribute column and P) must be rejected before publishing.
    const auto bad_type = take(NativeRegistration<BadLengthNode>{runtime, "test.BadLengthNode", TypeKind::object}
                                   .base<Node>()
                                   .field<&BadLengthNode::mutate_positions>("mutate_positions")
                                   .method<&BadLengthNode::process>("process")
                                   .parameter_names({"P", "weights"})
                                   .parameter_metadata({{{"procedural.role", "inout"}}, {{"procedural.role", "out"}}})
                                   .publish());
    const auto bad_n = add(bad_type);
    take(connect(runtime, graph, grid_n.id(), 0, bad_n.id(), 0));
    take(set_display(runtime, graph, bad_n.id()));
    for (const bool mutate_p : {false, true}) {
        take(runtime.write(bad_n.id(), field(runtime, bad_type, "mutate_positions"), Scalar{mutate_p}));
        take(evaluator.evaluate());
        const auto* bad_status = evaluator.status(bad_n.id());
        require(evaluator.output() == nullptr && evaluator.last_valid_output() != nullptr && bad_status &&
                    bad_status->error && bad_status->error->code == ErrorCode::invocation_failed &&
                    bad_status->error->message.contains(mutate_p ? "Column 'P' changed length"
                                                                 : "Column 'weights' changed length"),
                "Native column-binding node published mismatched column length.");
    }

    // 5. Graph-level failure must also clear output() while preserving last_valid_output().
    std::shared_ptr<const Geometry> preview_geometry;
    {
        auto transient_graph = take(runtime.create(type<Graph>(runtime)));
        const auto transient_grid = take(add_node(runtime, transient_graph.id(), type<Grid>(runtime)));
        take(set_display(runtime, transient_graph.id(), transient_grid.id()));
        Evaluator transient_eval{runtime, transient_graph, resources, program.get()};
        take(transient_eval.evaluate());
        require(transient_eval.output() != nullptr && transient_eval.output()->count(Layer::point) == 100 &&
                    transient_eval.last_valid_output() != nullptr &&
                    transient_eval.last_valid_output()->count(Layer::point) == 100,
                "Transient graph initial evaluation failed.");
        preview_geometry = transient_eval.retain_last_valid_output();
        take(runtime.destroy(transient_graph.id()));
        for (int attempt = 0; attempt < 2; ++attempt) {
            const auto destroyed_res = transient_eval.evaluate();
            require(!destroyed_res && transient_eval.output() == nullptr &&
                        transient_eval.last_valid_output() != nullptr &&
                        transient_eval.last_valid_output()->count(Layer::point) == 100,
                    "Graph-level failure left stale output() non-null or dropped last_valid_output().");
        }
    }

    require(preview_geometry && preview_geometry->count(Layer::point) == 100,
            "An adopted preview lost its geometry when the evaluator closed.");

    // 6. Verify `procedural.Node` retired `Node.fail`, and multi-chunk parallel Source SOPs report mid-chunk `fail(...)`.
    {
        const auto* base_node_info = take(runtime.describe(type<Node>(runtime)));
        require(base_node_info->methods.empty(), "procedural.Node still exposes retired native methods.");
        auto chunk_graph = take(runtime.create(type<Graph>(runtime)));
        const auto big_grid = take(add_node(runtime, chunk_graph.id(), type<Grid>(runtime)));
        take(runtime.write(big_grid.id(), field(runtime, type<Grid>(runtime), "rows"), Scalar{std::uint32_t{64}}));
        take(runtime.write(big_grid.id(), field(runtime, type<Grid>(runtime), "columns"), Scalar{std::uint32_t{64}}));
        const auto align_type = take(runtime.find_type("procedural.NormalAlign"));
        const auto align_n = take(add_node(runtime, chunk_graph.id(), align_type));
        take(connect(runtime, chunk_graph.id(), big_grid.id(), 0, align_n.id(), 0));
        take(set_display(runtime, chunk_graph.id(), align_n.id()));
        Evaluator chunk_eval{runtime, chunk_graph, resources, program.get()};
        take(chunk_eval.evaluate());
        require(chunk_eval.output() != nullptr && chunk_eval.output()->count(Layer::point) == 4096 &&
                    column<Quaternion>(*chunk_eval.output(), "orient", Layer::point).size() == 4096,
                "Multi-chunk parallel NormalAlign evaluation failed.");
        write_v3(align_n.id(), align_type, "up", {0.0f, std::numeric_limits<float>::quiet_NaN(), 0.0f});
        take(chunk_eval.evaluate());
        const auto* align_status = chunk_eval.status(align_n.id());
        require(chunk_eval.output() == nullptr && chunk_eval.last_valid_output() != nullptr &&
                    chunk_eval.last_valid_output()->count(Layer::point) == 4096 && align_status &&
                    align_status->error &&
                    align_status->error->message.contains("NormalAlign up vector must be finite."),
                "Parallel Source SOP fail(...) did not propagate cleanly through Evaluator.");
        const auto chunk_noise_type = take(runtime.find_type("procedural.Noise"));
        const auto chunk_noise = take(add_node(runtime, chunk_graph.id(), chunk_noise_type));
        take(connect(runtime, chunk_graph.id(), big_grid.id(), 0, chunk_noise.id(), 0));
        take(set_display(runtime, chunk_graph.id(), chunk_noise.id()));
        take(runtime.write(chunk_noise.id(), field(runtime, chunk_noise_type, "frequency"), Scalar{1e38f}));
        take(chunk_eval.evaluate());
        const auto* noise_status = chunk_eval.status(chunk_noise.id());
        require(chunk_eval.output() == nullptr && chunk_eval.last_valid_output() != nullptr &&
                    chunk_eval.last_valid_output()->count(Layer::point) == 4096 && noise_status &&
                    noise_status->error && noise_status->error->message.contains("Noise coordinates must be finite"),
                "In-worker parallel_for fail(...) in Noise did not propagate cleanly through Evaluator.");

        // 7. Off-display node inspection cooks only the inspected branch without disturbing display output/fallback.
        const auto inspect_branch = take(add_node(runtime, chunk_graph.id(), type<Box>(runtime)));
        const auto grid_evals_before = chunk_eval.status(big_grid.id())->evaluations;
        take(chunk_eval.evaluate_node(inspect_branch.id()));
        require(chunk_eval.output() == nullptr && chunk_eval.last_valid_output() != nullptr &&
                    chunk_eval.last_valid_output()->count(Layer::point) == 4096,
                "evaluate_node altered display output() or last_valid_output().");
        require(chunk_eval.node_output_count(inspect_branch.id()) == 1 &&
                    chunk_eval.node_output(inspect_branch.id()) != nullptr &&
                    chunk_eval.node_output(inspect_branch.id())->count(Layer::point) == 8 &&
                    chunk_eval.node_output(inspect_branch.id(), 1) == nullptr &&
                    chunk_eval.node_output(chunk_noise.id()) == nullptr &&
                    chunk_eval.status(big_grid.id())->evaluations == grid_evals_before,
                "evaluate_node / node_output returned wrong per-node geometry or re-ran unrelated nodes.");
        const auto missing_node = chunk_eval.evaluate_node(chunk_graph.id());
        require(!missing_node && missing_node.error().code == ErrorCode::argument_mismatch,
                "evaluate_node accepted an ObjectId that is not a node of the graph.");
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
    return take(geometry::Mesh::create(std::move(vertices)));
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
    for (const auto resolution : {32u, 128u}) {
        const std::string prefix = resolution == 32 ? "volume_" : "volume_large_";
        const auto metric = [&](const std::string& key, double value) { number((prefix + key).c_str(), value); };
        const auto volume_surface = take(sphere(5, resolution, resolution));
        std::vector<double> volume_times;
        std::vector<double> build_times;
        std::vector<double> classify_times;
        std::vector<double> remaining_times;
        std::uint32_t retained = 0;
        timing::FrameRecord volume_record;
        for (int i = -2; i < samples; ++i) {
            timing::Recorder recorder;
            timing::Recorder::Frame frame{recorder};
            const auto result = take(volume_scatter(volume_surface, {.density = 10, .seed = 3}));
            const auto& record = frame.finish();
            require(record.structure_errors == 0, "Volume profile timing catalog is incomplete.");
            if (i != -2)
                require(retained == result.count(Layer::point) && volume_record.entries == record.entries,
                        "Volume profile event counts changed across identical inputs.");
            retained = result.count(Layer::point);
            volume_record = record;
            if (i >= 0) {
                volume_times.push_back(std::chrono::duration<double, std::milli>(record.time(phases::volume)).count());
                build_times.push_back(std::chrono::duration<double, std::milli>(record.time(phases::build)).count());
                classify_times.push_back(
                    std::chrono::duration<double, std::milli>(record.time(phases::classify)).count());
                // Subtract within each sample; a difference of medians is not a median remainder.
                remaining_times.push_back(std::chrono::duration<double, std::milli>(record.time(phases::volume) -
                                                                                    record.time(phases::build) -
                                                                                    record.time(phases::classify))
                                              .count());
            }
        }
        std::ranges::sort(volume_times);
        std::ranges::sort(build_times);
        std::ranges::sort(classify_times);
        std::ranges::sort(remaining_times);
        report += "  \"" + prefix + "input\": \"Sphere(radius=5,segments=" + std::to_string(resolution) +
                  ",latitudes=" + std::to_string(resolution) + "); origin=0,h=1,density=10,seed=3\",\n";
        metric("samples", samples);
        metric("warmups", 2);
        metric("cells", 10 * 10 * 10);
        const auto candidates = volume_record.count(phases::candidate);
        require(candidates == 10000, "Volume profile changed the fixed candidate workload.");
        metric("candidates", candidates);
        metric("triangles", 2 * resolution * (resolution - 1));
        metric("retained", retained);
        metric("ms", volume_times[volume_times.size() / 2]);
        metric("build_ms", build_times[build_times.size() / 2]);
        metric("generate_classify_ms", classify_times[classify_times.size() / 2]);
        metric("remaining_ms", remaining_times[remaining_times.size() / 2]);
        metric("classify_ns_per_candidate", classify_times[classify_times.size() / 2] * 1e6 / candidates);
        for (std::size_t i = 0; i < phases::attempts.size(); ++i) {
            metric("attempt" + std::to_string(i), volume_record.count(phases::attempts[i]));
            metric("retry" + std::to_string(i), volume_record.count(phases::retries[i]));
        }
        metric("retry_rate", static_cast<double>(volume_record.count(phases::retries[0])) / candidates);
        metric("fallbacks", volume_record.count(phases::fallback));
        metric("fallback_rate", static_cast<double>(volume_record.count(phases::fallback)) / candidates);
    }

    // One million points: the in-graph scale.
    const auto ground = triangles({{0, 0, 0}, {0, 0, 1000}, {1000, 0, 0}, {1000, 0, 0}, {0, 0, 1000}, {1000, 0, 1000}});
    const auto centered_ground =
        triangles({{-500, 0, -500}, {-500, 0, 500}, {500, 0, -500}, {500, 0, -500}, {-500, 0, 500}, {500, 0, 500}});
    Geometry million;
    number("scatter_1m_ms",
           median_ms(samples, [](int) {}, [&] { million = take(scatter(ground, {.density = 1, .seed = 3})); }));
    number("scatter_1m_points", million.count(Layer::point));
    Geometry spaced;
    number("scatter_spaced_ms",
           median_ms(
               samples, [](int) {},
               [&] { spaced = take(scatter(centered_ground, {.density = 1, .seed = 3, .min_distance = 0.8f})); }));
    number("scatter_spaced_points", spaced.count(Layer::point));
    number("transform_1m_ms", median_ms(
                                  samples, [](int) {},
                                  [&] {
                                      auto moved = million;
                                      take(transform(moved, {1, 2, 3}, axis_angle({0, 1, 0}, 30), {1, 2, 1}));
                                  }));

    {
        Runtime source_rt;
        take(register_types(source_rt));
        auto source_prog = take(source::Program::create(source_rt));
        {
            auto req = take(source_prog->request(
                {"profile_procedural", 1, take(source::engine_sources(source::default_engine_root()))}));
            auto cand = take(source::Program::prepare(req));
            take(source_prog->try_publish(cand));
        }
        Context ctx;
        Array<bool> empty_mask;
        Array<float> empty_float;
        auto jitter_obj = take(source_rt.create(take(source_rt.find_type("procedural.Jitter"))));
        auto rscale_obj = take(source_rt.create(take(source_rt.find_type("procedural.RandomScale"))));
        auto noise_obj = take(source_rt.create(take(source_rt.find_type("procedural.Noise"))));
        auto remap_obj = take(source_rt.create(take(source_rt.find_type("procedural.Remap"))));
        const auto jitter_fn = take(source_prog->entry("procedural.Jitter.process"));
        const auto rscale_fn = take(source_prog->entry("procedural.RandomScale.process"));
        const auto noise_fn = take(source_prog->entry("procedural.Noise.process"));
        const auto remap_fn = take(source_prog->entry("procedural.Remap.process"));
        Array<Vector3> p_buf = million.positions();
        const Array<std::uint32_t>& id_buf = column<std::uint32_t>(million, "id", Layer::point);
        Array<float> float_buf;
        float_buf.resize(p_buf.size());
        number("jitter_1m_ms", median_ms(
                                   samples, [&](int) { p_buf = million.positions(); },
                                   [&] {
                                       const std::array args{Argument::borrow(ctx), Argument::borrow(p_buf),
                                                             Argument::borrow(id_buf),
                                                             Argument::borrow(std::as_const(empty_mask))};
                                       take(jitter_fn.invoke(jitter_obj.id(), args));
                                   }));
        number("random_scale_1m_ms",
               median_ms(
                   samples, [](int) {},
                   [&] {
                       const std::array args{Argument::borrow(ctx), Argument::borrow(std::as_const(empty_float)),
                                             Argument::borrow(float_buf), Argument::borrow(id_buf),
                                             Argument::borrow(std::as_const(empty_mask))};
                       take(rscale_fn.invoke(rscale_obj.id(), args));
                   }));
        number("noise_1m_ms",
               median_ms(
                   samples, [](int) {},
                   [&] {
                       const std::array args{Argument::borrow(ctx), Argument::borrow(std::as_const(p_buf)),
                                             Argument::borrow(float_buf), Argument::borrow(std::as_const(empty_mask))};
                       take(noise_fn.invoke(noise_obj.id(), args));
                   }));
        number("remap_1m_ms", median_ms(
                                  samples, [](int) {},
                                  [&] {
                                      const std::array args{Argument::borrow(std::as_const(float_buf)),
                                                            Argument::borrow(float_buf),
                                                            Argument::borrow(std::as_const(empty_mask))};
                                      take(remap_fn.invoke(remap_obj.id(), args));
                                  }));
    }

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
        node.mesh = String::literal<"profile/terrain">();
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
        node.meshes.add(String::literal<"profile/rock">());
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
                       if (!node.variant.assign(StringView{"v" + std::to_string(i + 2)}))
                           return failure("Variant name requires valid UTF-8.");
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
    atelier::install_fatal_handler();
    if (argc == 3 && std::string_view{argv[1]} == "--profile-procedural") {
        profile(argv[2]);
        return 0;
    }
    if (argc == 3 && std::string_view{argv[1]} == "--profile-boolean") {
        atelier::acceptance::profile_boolean(argv[2]);
        return 0;
    }
    require(argc == 1, "Usage: atelier_procedural_acceptance [--profile-procedural|--profile-boolean new-output.json]");
    ray_node();
    connectivity_modeling();
    boolean_modeling();
    boolean_contact_sweep();
    graph_evaluation();
    source_and_spatial_contracts();
    std::puts("procedural acceptance passed");
    return 0;
}
