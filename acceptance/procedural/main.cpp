#include "memory/fatal.hpp"
#include "../helpers.hpp"
#include "boolean_profile.hpp"
#include "foundation/map.hpp"
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
    Map<String, geometry::MeshRef> meshes;

    Result<geometry::MeshRef> mesh(StringView key) const noexcept override {
        if (const auto* found = meshes.find(key))
            return *found;
        return std::unexpected(Error{ErrorCode::invalid_member, "Unknown mesh."});
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

void grouped_wall_surfaces() {
    auto wall = take(box({2, 2, 2}));
    // Deliberately contradictory shading normals. The selection must follow polygon winding.
    take(wall.remove("N"));
    take(wall.add<Vector3>("N", Layer::point, {0, -1, 0}));
    const auto upper = take(build_group(wall, {.group = "upper", .max_angle = 90.f}));
    const auto top = take(build_group(upper, {.group = "top", .max_angle = 0.f}));
    const auto count = [](const Geometry& g, StringView group, Layer layer = Layer::primitive) {
        return std::ranges::count(*take(g.read<bool>(group, layer)), true);
    };
    require(count(top, "upper") == 5 && count(top, "top") == 1 && !wall.find("upper"),
            "Box winding, inclusive angle boundaries, or input isolation changed.");
    const auto combine = [&](GroupOperation op) {
        return take(build_group(top, {.group = "upper",
                                      .criterion = GroupCriterion::existing,
                                      .source_group = "top",
                                      .operation = op,
                                      .base_group = "upper"}));
    };
    require(count(combine(GroupOperation::subtract), "upper") == 4 &&
                count(combine(GroupOperation::intersect), "upper") == 1 &&
                count(combine(GroupOperation::unite), "upper") == 5 && count(top, "upper") == 5,
            "In-place named group algebra changed its operands or selected the wrong faces.");

    auto painted = top;
    take(group_box(painted, "left", {-2, -2, -2}, {0, 2, 2}));
    const auto points =
        take(build_group(painted, {.group = "keep", .criterion = GroupCriterion::existing, .source_group = "left"}));
    require(count(points, "keep", Layer::point) == 4 && same(std::get<SharedArray<bool>>(points.find("keep")->data),
                                                             std::get<SharedArray<bool>>(painted.find("left")->data)),
            "Copying a GroupBox point group lost its layer or duplicated its mask.");
    const auto mismatch =
        build_group(points, {.group = "upper", .operation = GroupOperation::intersect, .base_group = "keep"});
    const auto missing = build_group(points, {.criterion = GroupCriterion::existing, .source_group = "missing"});
    const auto wrong_type = build_group(points, {.criterion = GroupCriterion::existing, .source_group = "P"});
    const auto degenerate = take(Geometry::polygons({{0, 0, 0}, {1, 0, 0}, {2, 0, 0}}, {0, 1, 2}, {0, 3}));
    const auto bad_face = build_group(degenerate);
    require(!mismatch && mismatch.error().message.find("different layer") != std::string::npos && !missing &&
                missing.error().message.find("missing") != std::string::npos && !wrong_type &&
                wrong_type.error().message.find("bool") != std::string::npos &&
                !build_group(points, {.group = "keep"}) && !build_group(points, {.direction = {}}) &&
                !build_group(points, {.max_angle = 181.f}) && !bad_face &&
                bad_face.error().message == "Group primitive 0: Polygon has zero area." && count(points, "upper") == 5,
            "Group accepted an invalid selection, lost the first diagnostic, or overwrote a prior group.");
    const auto path = take(take(Geometry::points({{0, 0, 0}, {1, 0, 0}})).connect_points());
    require(count(take(build_group(path, {.max_angle = 180.f})), "selected") == 0,
            "An open path acquired a surface normal.");
}

void resampled_wall_and_post_paths() {
    // A five-metre wall turning after three metres: the same path feeds regularly spaced posts
    // and, with preserved turns, the swept wall. Subdividing a straight span must change neither.
    const auto path = take(take(Geometry::points({{0, 0, 0}, {3, 0, 0}, {3, 0, 2}})).connect_points());
    const auto split = take(take(Geometry::points({{0, 0, 0}, {1, 0, 0}, {3, 0, 0}, {3, 0, 2}})).connect_points());
    const ResampleSettings spacing{.spacing = 2.f};
    const auto posts = take(resample(path, spacing));
    require(posts.same_value(take(resample(split, spacing))) && posts.count(Layer::point) == 4 &&
                posts.positions()[1] == Vector3{2, 0, 0} && posts.positions()[2] == Vector3{3, 0, 1} &&
                posts.positions()[3] == Vector3{3, 0, 2} && column<float>(posts, "arclength", Layer::point)[3] == 5.f,
            "Fixed-distance post stations drifted or lost the short tail.");

    const ResampleSettings keep{.spacing = 2.f, .preserve_corners = true};
    require(take(resample(path, keep)).same_value(take(resample(split, keep))),
            "Corner preservation introduced a redundant collinear control point.");
    auto painted = path;
    take(painted.set(String::literal<"pscale">(), Layer::point, SharedArray<float>{Array<float>{1, 4, 6}}));
    take(painted.set(String::literal<"wall">(), Layer::primitive, SharedArray<bool>{Array<bool>{true}}));
    const auto wall = take(resample(painted, keep));
    require(wall.count(Layer::point) == 5 && wall.positions()[2] == Vector3{3, 0, 0} &&
                column<float>(wall, "pscale", Layer::point)[1] == 3.f &&
                column<float>(wall, "pscale", Layer::point)[3] == 5.f &&
                column<bool>(wall, "wall", Layer::primitive)[0] &&
                column<float>(wall, "curveu", Layer::point)[2] == 0.6f &&
                column<Vector3>(wall, "tangent", Layer::point)[2] == Vector3{0, 0, 1} &&
                take(sweep(wall)).count(Layer::point) == 20,
            "Preserved wall corner, outgoing tangent or interpolated Sweep width is incorrect.");

    // Two primitives sharing a control point: each restarts arc length. The closed loop shares
    // its start point, while its two seam corners retain their distinct authored vertex values.
    auto loop = take(Geometry::primitives({{0, 0, 0}, {2, 0, 0}, {2, 0, 2}, {0, 0, 2}}, {0, 1, 2, 3, 0, 0, 0, 1},
                                          {0, 5, 8}, {PrimitiveKind::polyline, PrimitiveKind::polyline}));
    take(loop.set(String::literal<"seam">(), Layer::vertex,
                  SharedArray<float>{Array<float>{0, 2, 4, 6, 8, 10, 10, 12}}));
    const auto ring = take(resample(loop, {.mode = ResampleMode::segments, .segments = 8}));
    const auto& seam = column<float>(ring, "seam", Layer::vertex);
    require(ring.count(Layer::point) == 17 && ring.count(Layer::vertex) == 18 &&
                ring.vertex_points()[8] == ring.vertex_points()[0] && seam[1] == 1.f && seam[8] == 8.f &&
                seam[9] == 10.f && column<float>(ring, "arclength", Layer::point)[8] == 0.f &&
                column<float>(ring, "curveu", Layer::point)[16] == 1.f,
            "Closed seam, corner attributes or independent second primitive were lost.");
    const auto decimal = take(take(Geometry::points({{0, 0, 0}, {0.3f, 0, 0}})).connect_points());
    require(take(resample(decimal, {.spacing = 0.1f})).count(Layer::point) == 4,
            "An unrepresentable decimal tail duplicated the last station.");
    const auto zero = take(take(Geometry::points({{0, 0, 0}, {0, 0, 0}})).connect_points());
    const auto too_many = resample(path, {.spacing = std::numeric_limits<float>::min()});
    require(!resample(zero) && !resample(path, {.spacing = 0.f}) &&
                !resample(loop, {.mode = ResampleMode::segments, .segments = 2}) && !resample(take(box({1, 1, 1}))) &&
                !too_many && too_many.error().message == "Resample exceeds the Geometry vertex limit.",
            "Invalid curve or unrepresentable sample count did not fail before output publication.");
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

    take(set_bypassed(runtime, op.id(), true));
    take(evaluator.evaluate());
    require(evaluator.output() && std::abs(solid_measures(*evaluator.output()).first - 1.) < 1e-6 &&
                evaluator.status(op.id())->boolean_stats == last_cook,
            "Bypassing the Boolean did not forward its first input and keep its last cook measurements.");
    const auto hits_before_restore = evaluator.status(op.id())->cache_hits;
    take(set_bypassed(runtime, op.id(), false));
    take(evaluator.evaluate());
    require(evaluator.output() && std::abs(solid_measures(*evaluator.output()).first - .5) < 1e-6 &&
                evaluator.status(op.id())->cache_hits == hits_before_restore + 1 &&
                evaluator.status(op.id())->boolean_stats == last_cook,
            "Restoring the cached intersection lost the last actual Boolean cook measurements.");

    const auto translate = field(runtime, type<Transform>(runtime), "translate");
    const auto move_and_check = [&](Evaluator& ev, float dx, std::uint32_t hits, double volume) {
        const Vector3 offset{dx, 0, 0};
        take(runtime.write(move.id(), translate, ValueInput{type<Vector3>(runtime), &offset}));
        const auto* previous = ev.status(op.id());
        const auto runs = previous ? previous->evaluations : 0;
        take(ev.evaluate());
        const auto* status = ev.status(op.id());
        require(status && status->evaluations == runs + 1 && status->boolean_stats &&
                    status->boolean_stats->prepared_input_hits == hits && ev.output() &&
                    std::abs(solid_measures(*ev.output()).first - volume) < 1e-6,
                "Boolean preparation reuse, actual cook count or intersected box volume changed.");
    };
    Evaluator prepared{runtime, graph, resources};
    move_and_check(prepared, .125f, 0, .875);
    move_and_check(prepared, .25f, 0, .75);
    move_and_check(prepared, .375f, 1, .625);
    move_and_check(prepared, 4.f, 0, 0); // Same stationary box, larger joint lattice.
    const Vector3 smaller{.5f, 1, 1};
    take(runtime.write(box_node.id(), field(runtime, type<Box>(runtime), "size"),
                       ValueInput{type<Vector3>(runtime), &smaller}));
    move_and_check(prepared, .125f, 0, .375); // Editing the shared source changes both operands.
    for (const std::size_t budget : {0u, 1u}) {
        Evaluator uncached{runtime, graph, resources, nullptr, budget};
        move_and_check(uncached, .125f, 0, .375);
        move_and_check(uncached, .25f, 0, .25);
        move_and_check(uncached, .375f, 0, .125);
    }
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
    resources.meshes.insert_or_assign(String::literal<"fixture/a">(), cube);
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
    take(runtime.write(grid_node, field(runtime, type<Grid>(runtime), "primitive_type"),
                       EnumValue{type<GridPrimitive>(runtime), static_cast<std::uint32_t>(GridPrimitive::points)}));
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
    resources.meshes.insert_or_assign(String::literal<"fixture/cube">(), geometry::cube_mesh());
    const auto graph_root = take(runtime.create(type<Graph>(runtime)));
    const auto graph = graph_root.id();
    take(runtime.write(graph, field(runtime, type<Graph>(runtime), "seed"), Scalar{std::uint32_t{11}}));
    const auto add = [&](TypeId node_type) { return take(add_node(runtime, graph, node_type)); };
    const auto grid_n = add(type<Grid>(runtime));
    const auto bounds_n = add(type<GroupBox>(runtime));
    const auto group_n = add(type<Group>(runtime));
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
        n.primitive_type = GridPrimitive::points;
        n.size = {2.0f, 2.0f};
        n.rows = 2;
        n.columns = 2;
        return {};
    }));
    take(runtime.with_native<Group>(group_n.id(), [](Group& n) -> Result<void> {
        n.criterion = GroupCriterion::existing;
        n.source_group = String::literal<"within_bounds">();
        return {};
    }));
    take(runtime.with_native<GroupBox>(bounds_n.id(), [](GroupBox& n) -> Result<void> {
        n.group = String::literal<"within_bounds">();
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
        n.rotate = {0.0f, 90.0f, 0.0f};
        return {};
    }));
    take(runtime.with_native<CopyToPoints>(copy_n.id(), [](CopyToPoints& n) -> Result<void> {
        n.meshes.add(String::literal<"fixture/cube">());
        return {};
    }));

    take(connect(runtime, graph, grid_n.id(), 0, bounds_n.id(), 0));
    take(connect(runtime, graph, bounds_n.id(), 0, group_n.id(), 0));
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

    // 6. Verify `procedural.Node` retired `Node.fail`, exposes only BorrowSafe builder methods, and multi-chunk parallel Source SOPs report mid-chunk `fail(...)`.
    {
        const auto* base_node_info = take(runtime.describe(type<Node>(runtime)));
        require(std::ranges::none_of(base_node_info->methods, [](const MethodInfo& m) { return m.name == "fail"; }) &&
                    base_node_info->methods.size() == 12 &&
                    std::ranges::all_of(base_node_info->methods, &MethodInfo::preserves_borrows),
                "procedural.Node exposed retired fail or non-BorrowSafe methods.");
        auto chunk_graph = take(runtime.create(type<Graph>(runtime)));
        const auto big_grid = take(add_node(runtime, chunk_graph.id(), type<Grid>(runtime)));
        take(runtime.write(big_grid.id(), field(runtime, type<Grid>(runtime), "primitive_type"),
                           EnumValue{type<GridPrimitive>(runtime), static_cast<std::uint32_t>(GridPrimitive::points)}));
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
    resources.meshes.insert_or_assign(String::literal<"profile/terrain">(), terrain);
    resources.meshes.insert_or_assign(String::literal<"profile/rock">(), geometry::cube_mesh());
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
        node.primitive_type = GridPrimitive::points;
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

void generative_modeling_and_l3_contracts() {
    // 1. C++ Extrude: watertight beveled solid, zero-distance inset panel split, and shared-edge region extrude.
    {
        Array<Vector3> quad_pos{{-2.f, 0.f, -2.f}, {-2.f, 0.f, 2.f}, {2.f, 0.f, 2.f}, {2.f, 0.f, -2.f}};
        Array<std::uint32_t> quad_vp{0u, 1u, 2u, 3u};
        Array<std::uint32_t> quad_off{0u, 4u};
        auto slab = take(Geometry::polygons(std::move(quad_pos), std::move(quad_vp), quad_off));

        auto prism = take(extrude(slab, {.distance = 2.0f,
                                         .inset = 0.25f,
                                         .individual = true,
                                         .output_back = true,
                                         .front_group = StringView{"top_cap"},
                                         .side_group = StringView{"bevel_side"}}));
        require(prism.count(Layer::primitive) == 6 && prism.count(Layer::point) == 8 &&
                    column<bool>(prism, "top_cap", Layer::primitive).size() == 6 &&
                    column<bool>(prism, "bevel_side", Layer::primitive).size() == 6,
                "Beveled Extrude prism produced unexpected face or group counts.");
        // Watertight outward-oriented shell must pass VolumeScatter & Boolean directly.
        const auto prism_vol = take(volume_scatter(prism, {.density = 4.f, .seed = 9, .cell_size = 0.5f}));
        require(prism_vol.count(Layer::point) > 10, "Extruded beveled prism failed closed manifold VolumeScatter.");
        const auto cutter = take(box({1.5f, 4.0f, 1.5f}));
        const auto carved = take(solid_boolean(prism, cutter, BooleanOperation::subtract));
        require(carved.count(Layer::primitive) > 6, "Boolean subtract on extruded prism failed.");

        // Zero-distance inset splits a panel into 4 border frame quads + 1 inner pane.
        const auto framed = take(extrude(slab, {.distance = 0.f,
                                                .inset = 0.4f,
                                                .individual = true,
                                                .output_back = false,
                                                .front_group = StringView{"pane"},
                                                .side_group = StringView{"frame"}}));
        require(framed.count(Layer::primitive) == 5,
                "Zero-distance inset Extrude did not emit 4 frame quads + 1 pane.");
        const auto collapsed = extrude(slab, {.distance = 1.f, .inset = 2.5f});
        require(!collapsed, "Extrude accepted an inset larger than half the polygon width.");

        // Shared-edge region extrude over 2 adjacent quads suppresses the interior wall -> 2 top + 2 back + 6 boundary sides = 10 faces.
        Array<Vector3> two_pos{{0.f, 0.f, 0.f}, {0.f, 0.f, 2.f}, {2.f, 0.f, 2.f},
                               {2.f, 0.f, 0.f}, {4.f, 0.f, 2.f}, {4.f, 0.f, 0.f}};
        Array<std::uint32_t> two_vp{0u, 1u, 2u, 3u, 3u, 2u, 4u, 5u};
        Array<std::uint32_t> two_off{0u, 4u, 8u};
        const auto two_quads = take(Geometry::polygons(std::move(two_pos), std::move(two_vp), two_off));
        const auto region_solid =
            take(extrude(two_quads, {.distance = 1.5f, .individual = false, .output_back = true}));
        require(region_solid.count(Layer::primitive) == 10, "Region extrude failed to suppress shared interior edge.");
        const auto region_vol = take(volume_scatter(region_solid, {.density = 4.f, .seed = 3, .cell_size = 0.5f}));
        require(region_vol.count(Layer::point) > 5, "Region-extruded solid was not a closed outward manifold.");
    }

    // 2. C++ Sweep: mitered L-wall solid & 3D undulating helical tube (warped-quad diagonal split).
    {
        Array<Vector3> l_pts{{0.f, 0.f, 0.f}, {4.f, 0.f, 0.f}, {4.f, 0.f, 4.f}};
        const auto l_curve = take(take(Geometry::points(std::move(l_pts))).connect_points());
        for (const auto& [segments, error] : {std::pair{1'500'000'000u, "Sweep exceeds the Geometry point limit."},
                                              std::pair{800'000'000u, "Sweep exceeds the Geometry face limit."},
                                              std::pair{300'000'000u, "Sweep exceeds the Geometry corner limit."}}) {
            const auto oversized = sweep(l_curve, {.profile = SweepProfile::tube, .segments = segments});
            require(!oversized && oversized.error().message == error,
                    "Oversized Sweep did not reject topology before allocation.");
        }
        const auto l_wall = take(sweep(l_curve, {.profile = SweepProfile::rectangle, .width = 1.2f, .height = 2.0f}));
        require(l_wall.count(Layer::primitive) == 10, "Planar mitered L-wall Sweep should keep 8 side quads + 2 caps.");
        const auto l_vol = take(volume_scatter(l_wall, {.density = 4.f, .seed = 5, .cell_size = 0.5f}));
        require(l_vol.count(Layer::point) > 8, "Mitered L-wall Sweep was not a closed outward manifold.");

        Array<Vector3> helix_pts;
        for (int k = 0; k < 7; ++k) {
            const float ang = float(k) * 0.55f;
            helix_pts.add({3.0f * std::cos(ang), float(k) * 0.65f, 3.0f * std::sin(ang)});
        }
        auto helix_curve = take(take(Geometry::points(std::move(helix_pts))).connect_points());
        auto* pscale = take(helix_curve.add<float>("pscale", Layer::point, 1.f));
        for (std::size_t k = 0; k < pscale->size(); ++k)
            (*pscale)[k] = 0.85f + 0.05f * float(k);
        const auto swept_tube =
            take(sweep(helix_curve, {.profile = SweepProfile::tube, .radius = 0.7f, .segments = 8, .end_caps = true}));
        const auto tube_vol = take(volume_scatter(swept_tube, {.density = 6.f, .seed = 7, .cell_size = 0.45f}));
        require(tube_vol.count(Layer::point) > 5,
                "3D undulating tapered Sweep failed planar polygon validation or closed manifold VolumeScatter.");
    }

    // 3. Source L3 GeometryBuilder + Auxiliary Port Columns + Parallel Spatial BVH Queries.
    {
        Runtime runtime;
        take(register_types(runtime));
        auto program = take(source::Program::create(runtime));
        auto files = take(source::engine_sources(source::default_engine_root()));
        files.push_back({"Source/L3Test.source", 1, R"(
class test.MultiBuilder : procedural.Node {
    float height = 1.5f;

    void process([Port] procedural.Geometry& floor_out,
                 [Port] procedural.Geometry& road_out) {
        Array<vector3> q_pts;
        q_pts.add(vector3(-3.0f, self.height, -3.0f));
        q_pts.add(vector3(-3.0f, self.height, 3.0f));
        q_pts.add(vector3(3.0f, self.height, 3.0f));
        q_pts.add(vector3(3.0f, self.height, -3.0f));
        Array<uint> q_idx;
        q_idx.add(uint(0));
        q_idx.add(uint(1));
        q_idx.add(uint(2));
        q_idx.add(uint(3));
        q_pts.add(vector3(8.0f, self.height, 0.0f));
        q_pts.add(vector3(8.0f, self.height, 0.00005f));
        q_pts.add(vector3(8.00005f, self.height, 0.00005f));
        q_pts.add(vector3(8.00005f, self.height, 0.0f));
        q_idx.add(uint(4));
        q_idx.add(uint(5));
        q_idx.add(uint(6));
        q_idx.add(uint(7));
        self.emit_quads(q_pts, q_idx, uint(0));

        Array<vector3> r_pts;
        r_pts.add(vector3(-2.0f, self.height, 0.0f));
        r_pts.add(vector3(2.0f, self.height, 0.0f));
        Array<uint> r_vp;
        r_vp.add(uint(0));
        r_vp.add(uint(1));
        Array<uint> r_off;
        r_off.add(uint(0));
        r_off.add(uint(2));
        Array<float> r_w;
        r_w.add(2.5f);
        self.emit_polylines(r_pts, r_vp, r_off, uint(1));
        self.set_float("road_w", procedural.Layer::primitive, r_w, uint(1));
    }
}

class test.SpatialProbe : procedural.Node {
    void process([InOut("P")] Array<vector3>& P,
                 [Out("curve_d")] Array<float>& curve_d,
                 [Port] const procedural.Spatial& surface,
                 [Port] const procedural.Spatial& road,
                 [In("road_w", Port = "road", Layer = "primitive")] const Array<float>& road_w) {
        const uint n = P.size();
        float w = 0.0f;
        if (road_w.size() > 0) {
            w = road_w[0];
        }
        const procedural.SurfaceHit tiny = procedural.raycast_surface(
            surface, vector3(8.00001f, 3.5f, 0.00002f), vector3(0.0f, -1.0f, 0.0f), 2.0f);
        const vector3 far = vector3(0.0f, 2e20f, 0.0f);
        const procedural.SurfaceHit far_surface = procedural.closest_surface_point(surface, far, 1e20f);
        const procedural.CurveSample far_curve = procedural.closest_polyline(road, far, 1e20f);
        const procedural.SurfaceHit in_range = procedural.closest_surface_point(surface, far, 3e20f);
        if (!tiny.hit || tiny.distance != 2.0f || far_surface.hit || far_curve.hit ||
            !in_range.hit || in_range.distance != 2e20f) {
            fail("Spatial queries lost a small triangle or compared overflowed squared distances.");
        }
        parallel_for (uint i : n) {
            const procedural.SurfaceHit hit = procedural.raycast_surface(
                surface, P[i] + vector3(0.0f, 10.0f, 0.0f), vector3(0.0f, -1.0f, 0.0f), 20.0f);
            const procedural.CurveSample cs = procedural.closest_polyline(road, P[i], 20.0f);
            if (hit.hit) {
                P[i] = hit.position;
            }
            if (cs.hit) {
                curve_d[i] = cs.distance + w;
            } else {
                curve_d[i] = -1.0f;
            }
        }
    }
}

class test.BadPrimaryEmit : procedural.Node {
    void process([InOut("P")] Array<vector3>& P) {
        Array<vector3> pts;
        pts.add(vector3(0.0f, 0.0f, 0.0f));
        self.emit_points(pts, uint(0));
    }
}

class test.BadWarpedQuad : procedural.Node {
    void process([Port] procedural.Geometry& out) {
        Array<vector3> pts;
        pts.add(vector3(0.0f, 0.0f, 0.0f));
        pts.add(vector3(1.0f, 0.0f, 0.0f));
        pts.add(vector3(1.0f, 1.0f, 1.0f));
        pts.add(vector3(0.0f, 0.0f, 1.0f));
        Array<uint> q;
        q.add(uint(0));
        q.add(uint(1));
        q.add(uint(2));
        q.add(uint(3));
        self.emit_quads(pts, q, uint(0));
    }
}
)"});
        const auto req = take(program->request({"l3_test", 1, std::move(files)}));
        auto cand = take(source::Program::prepare(req));
        take(program->try_publish(cand));

        Meshes resources;
        auto graph = take(runtime.create(type<Graph>(runtime)));
        const auto grid_n = take(add_node(runtime, graph.id(), type<Grid>(runtime)));
        const auto builder_t = take(runtime.find_type("test.MultiBuilder"));
        const auto probe_t = take(runtime.find_type("test.SpatialProbe"));
        const auto probe_ports = take(ports(runtime, probe_t));
        // `SpatialProbe` shares one single-input port `"road"` between `[Port] const Spatial& road` and `[In(..., Port = "road")]`!
        require(probe_ports.inputs.size() == 3 && probe_ports.inputs[0] == "input" &&
                    probe_ports.inputs[1] == "surface" && probe_ports.inputs[2] == "road",
                "SpatialProbe did not merge Spatial and auxiliary column bindings onto single port 'road'.");

        const auto builder_n = take(add_node(runtime, graph.id(), builder_t));
        const auto probe_n = take(add_node(runtime, graph.id(), probe_t));
        take(connect(runtime, graph.id(), grid_n.id(), 0, probe_n.id(), 0));
        take(connect(runtime, graph.id(), builder_n.id(), 0, probe_n.id(), 1));
        take(connect(runtime, graph.id(), builder_n.id(), 1, probe_n.id(), 2));
        take(set_display(runtime, graph.id(), probe_n.id()));

        Evaluator eval{runtime, graph, resources, program.get()};
        take(eval.evaluate());
        const auto* out = eval.output();
        require(out != nullptr && out->count(Layer::point) == 100, "SpatialProbe evaluation failed.");
        bool any_lifted = false;
        for (const auto& p : out->positions())
            any_lifted |= std::abs(p.y - 1.5f) < 1e-4f;
        const auto& cd = column<float>(*out, "curve_d", Layer::point);
        require(any_lifted && cd[0] >= 2.5f,
                "Parallel raycast_surface / closest_polyline + auxiliary port column did not produce expected values.");

        // Reject mutating primary stream output 0 via emit_points.
        const auto bad_emit_n = take(add_node(runtime, graph.id(), take(runtime.find_type("test.BadPrimaryEmit"))));
        take(connect(runtime, graph.id(), grid_n.id(), 0, bad_emit_n.id(), 0));
        take(eval.evaluate_node(bad_emit_n.id()));
        require(eval.status(bad_emit_n.id()) && eval.status(bad_emit_n.id())->error &&
                    eval.status(bad_emit_n.id())->error->message.contains("primary column-stream output 0"),
                "emit_points on primary stream output 0 was not rejected.");

        // Reject warped non-planar quad in emit_quads.
        const auto bad_quad_n = take(add_node(runtime, graph.id(), take(runtime.find_type("test.BadWarpedQuad"))));
        take(eval.evaluate_node(bad_quad_n.id()));
        require(eval.status(bad_quad_n.id()) && eval.status(bad_quad_n.id())->error &&
                    eval.status(bad_quad_n.id())->error->message.contains("planar"),
                "Warped non-planar quad in emit_quads was not rejected by shared polygon validator.");
    }
}

void asset_subgraph_and_cache_pool_contracts() {
    Runtime runtime;
    take(register_types(runtime));
    Meshes resources;
    resources.meshes.insert_or_assign(String::literal<"project/pillar">(), geometry::cube_mesh());

    // 1. Standalone GraphInput fallback vs subgraph external input override, GraphOutput decoupling from Display,
    //    shared CachePool reuse across distinct AssetNodes, and MeshImport dependency bubbling.
    auto make_sub = [&]() {
        auto sub = take(runtime.create(type<Graph>(runtime)));
        const auto gin = take(add_node(runtime, sub.id(), type<GraphInput>(runtime)));
        take(runtime.write(
            gin.id(), take(runtime.find_field(type<GraphInput>(runtime), "preview_fallback")),
            EnumValue{type<PreviewFallback>(runtime), static_cast<std::uint32_t>(PreviewFallback::grid)}));
        const auto scatter_n = take(add_node(runtime, sub.id(), type<Scatter>(runtime)));
        take(runtime.write(scatter_n.id(), take(runtime.find_field(type<Scatter>(runtime), "density")), 2.f));
        const auto copy_n = take(add_node(runtime, sub.id(), type<CopyToPoints>(runtime)));
        Array<String> meshes;
        meshes.add(String::literal<"project/pillar">());
        take(runtime.write(copy_n.id(), take(runtime.find_field(type<CopyToPoints>(runtime), "meshes")),
                           ValueInput{take(runtime.native_type<Array<String>>()), &meshes}));
        const auto gout_pts = take(add_node(runtime, sub.id(), type<GraphOutput>(runtime)));
        const auto gout_inst = take(add_node(runtime, sub.id(), type<GraphOutput>(runtime)));
        const auto debug_sphere = take(add_node(runtime, sub.id(), type<Sphere>(runtime)));

        take(connect(runtime, sub.id(), gin.id(), 0, scatter_n.id(), 0));
        take(connect(runtime, sub.id(), scatter_n.id(), 0, copy_n.id(), 0));
        take(connect(runtime, sub.id(), scatter_n.id(), 0, gout_pts.id(), 0));
        take(connect(runtime, sub.id(), copy_n.id(), 0, gout_inst.id(), 0));
        // Author leaves [D] on debug_sphere! Formal AssetNode outputs must still read gout_pts & gout_inst.
        take(set_display(runtime, sub.id(), debug_sphere.id()));

        struct BuiltSub {
            ObjectRoot graph;

            ObjectId gin;

            ObjectId gout_pts;

            ObjectId gout_inst;

            ObjectId debug_sphere;
        };
        return BuiltSub{std::move(sub), gin.id(), gout_pts.id(), gout_inst.id(), debug_sphere.id()};
    };

    auto standalone = make_sub();
    auto shared_pool = std::make_shared<CachePool>(std::size_t{64} << 20);
    Evaluator standalone_eval{runtime, standalone.graph, resources, nullptr, shared_pool};
    take(standalone_eval.evaluate());
    // Display is debug_sphere (polygon mesh), while standalone GraphInput preview_fallback=Grid feeds gout_pts!
    require(standalone_eval.output() != nullptr && standalone_eval.output()->count(Layer::primitive) > 10,
            "Standalone SOP display did not show debug_sphere.");
    take(standalone_eval.evaluate_node(standalone.gin));
    require(standalone_eval.node_output(standalone.gin) != nullptr &&
                standalone_eval.node_output(standalone.gin)->count(Layer::point) == 100,
            "Standalone GraphInput did not emit 10x10 preview_fallback grid.");

    // Outer graph with two AssetNode instances sharing `shared_pool`.
    auto outer = take(runtime.create(type<Graph>(runtime)));
    const auto box_src = take(add_node(runtime, outer.id(), type<Box>(runtime)));
    const auto asset_a = take(add_node(runtime, outer.id(), type<AssetNode>(runtime)));
    const auto asset_b = take(add_node(runtime, outer.id(), type<AssetNode>(runtime)));
    const auto merge_out = take(add_node(runtime, outer.id(), type<Merge>(runtime)));

    auto sub_a = make_sub();
    auto sub_b = make_sub();
    const Ports sub_ports{{"surface"}, {false}, {"points", "instances"}};
    take(set_asset_subgraph(
        runtime, asset_a.id(),
        {"project/scatter_pillars", sub_ports, sub_a.graph, {sub_a.gin}, {sub_a.gout_pts, sub_a.gout_inst}, {}, 1}));
    take(set_asset_subgraph(
        runtime, asset_b.id(),
        {"project/scatter_pillars", sub_ports, sub_b.graph, {sub_b.gin}, {sub_b.gout_pts, sub_b.gout_inst}, {}, 1}));

    const auto dyn_ports = take(ports(runtime, asset_a.id()));
    require(dyn_ports == sub_ports, "Instance-level ports(runtime, ObjectId) did not return AssetNode dynamic ports.");

    // Connect box_src -> asset_a.surface and asset_b.surface; merge asset_a.instances + asset_b.instances.
    take(connect(runtime, outer.id(), box_src.id(), 0, asset_a.id(), 0));
    take(connect(runtime, outer.id(), box_src.id(), 0, asset_b.id(), 0));
    take(connect(runtime, outer.id(), asset_a.id(), 1, merge_out.id(), 0));
    take(connect(runtime, outer.id(), asset_b.id(), 1, merge_out.id(), 0));
    take(set_display(runtime, outer.id(), merge_out.id()));

    Evaluator outer_eval{runtime, outer, resources, nullptr, shared_pool};
    take(outer_eval.evaluate());
    require(outer_eval.output() != nullptr && outer_eval.output()->count(Layer::instance) >= 8,
            "AssetNode multi-output evaluation failed to produce CopyToPoints instances.");
    // Identical AssetNode asset_b must hit the shared CachePool without re-running!
    require(outer_eval.status(asset_a.id())->evaluations == 1 && outer_eval.status(asset_b.id())->cache_hits >= 1,
            "Second identical AssetNode did not hit shared CachePool.");
    // Mesh dependency from inner CopyToPoints must bubble up to outer display_dependencies()!
    const auto deps = outer_eval.display_dependencies();
    require(deps.size() == 1 && deps[0].key.view() == "project/pillar",
            "Inner AssetNode mesh dependency did not bubble up to outer Evaluator.");

    // Non-zero seed_offset on asset_b produces distinct scatter placements while asset_a stays cached.
    take(runtime.write(asset_b.id(), take(runtime.find_field(type<AssetNode>(runtime), "seed_offset")), 99u));
    take(outer_eval.evaluate());
    const auto* pts_a = outer_eval.node_output(asset_a.id(), 0);
    const auto* pts_b = outer_eval.node_output(asset_b.id(), 0);
    require(pts_a != nullptr && pts_b != nullptr &&
                (pts_a->count(Layer::point) != pts_b->count(Layer::point) ||
                 pts_a->positions()[0] != pts_b->positions()[0]),
            "Non-zero AssetNode seed_offset did not diversify inner scatter seed.");

    // Unconnected AssetNode input must pass empty Geometry, NOT standalone preview_fallback Grid!
    take(disconnect(runtime, box_src.id(), 0, asset_b.id(), 0));
    take(outer_eval.evaluate());
    require(outer_eval.node_output(asset_b.id(), 0) != nullptr &&
                outer_eval.node_output(asset_b.id(), 0)->count(Layer::point) == 0,
            "Unconnected AssetNode input leaked standalone preview_fallback instead of empty Geometry.");

    // 2. Enforce exact max_subgraph_depth = 8 boundary: 8 AssetNode hops succeed, 9 AssetNode hops reject.
    std::vector<ObjectRoot> chain_graphs;
    for (int d = 0; d < 10; ++d) {
        auto g = take(runtime.create(type<Graph>(runtime)));
        const auto n = take(add_node(runtime, g.id(), d == 9 ? type<Box>(runtime) : type<AssetNode>(runtime)));
        take(set_display(runtime, g.id(), n.id()));
        if (d > 0) {
            const auto prev_nodes = take(contents(runtime, chain_graphs.back().id()));
            take(set_asset_subgraph(
                runtime, prev_nodes.nodes().front(),
                {"project/depth_" + std::to_string(d), {{}, {}, {"output"}}, g, {}, {n.id()}, {}, 1}));
        }
        chain_graphs.push_back(std::move(g));
    }
    Evaluator ok_depth_eval{runtime, chain_graphs[1], resources, nullptr, shared_pool};
    take(ok_depth_eval.evaluate());
    require(ok_depth_eval.output() != nullptr && ok_depth_eval.output()->count(Layer::point) == 8 &&
                !ok_depth_eval.display_error().has_value(),
            "Evaluator rejected legal AssetNode chain at exact max_subgraph_depth = 8.");

    Evaluator deep_eval{runtime, chain_graphs.front(), resources, nullptr, shared_pool};
    take(deep_eval.evaluate());
    require(deep_eval.output() == nullptr && deep_eval.display_error().has_value(),
            "Evaluator did not reject AssetNode chain exceeding max_subgraph_depth = 8.");
}

void parametric_primitives_and_alignment_contracts() {
    // 1. Surface Grid (Quads / Triangles / XY / YZ) + Extrude into watertight solid:
    const auto floor_grid = take(grid({.primitive_type = GridPrimitive::quads,
                                       .orientation = OrientationPlane::zx,
                                       .size = {6.f, 4.f},
                                       .center = {1.f, 2.f, -1.f},
                                       .rows = 3,
                                       .columns = 4}));
    require(floor_grid.count(Layer::point) == 12 && floor_grid.count(Layer::primitive) == 6 &&
                floor_grid.count(Layer::vertex) == 24 && std::abs(solid_measures(floor_grid).second - 24.0) < 1e-4,
            "Default Quad Grid did not produce 12 shared points, 6 quads and area 24.");
    const auto floor_slab = take(extrude(floor_grid, {.distance = 2.f, .individual = false, .output_back = true}));
    require(std::abs(solid_measures(floor_slab).first - 48.0) < 1e-4,
            "Extruded Quad Grid did not form a closed slab of volume 48.");

    const auto wall_xy = take(grid({.primitive_type = GridPrimitive::triangles,
                                    .orientation = OrientationPlane::xy,
                                    .size = {4.f, 3.f},
                                    .rows = 2,
                                    .columns = 2}));
    const auto wall_yz = take(grid({.primitive_type = GridPrimitive::quads,
                                    .orientation = OrientationPlane::yz,
                                    .size = {5.f, 2.f},
                                    .rows = 3,
                                    .columns = 3}));
    require(wall_xy.count(Layer::primitive) == 2 && near(column<Vector3>(wall_xy, "N", Layer::vertex)[0], {0, 0, 1}) &&
                wall_yz.count(Layer::primitive) == 4 &&
                near(column<Vector3>(wall_yz, "N", Layer::vertex)[0], {1, 0, 0}),
            "Vertical XY/YZ Grids did not orient normals along +Z and +X.");

    // 2. Subdivided Box + base_on_ground: single closed manifold shell compatible with solid_boolean:
    auto sub_box = take(box_primitive({.size = {6.f, 4.f, 8.f},
                                       .center = {0.f, 1.f, 0.f},
                                       .base_on_ground = true,
                                       .divisions_x = 3,
                                       .divisions_y = 2,
                                       .divisions_z = 4}));
    const auto box_b = bounds(sub_box);
    require(box_b && near(box_b->minimum, {-3.f, 1.f, -4.f}) && near(box_b->maximum, {3.f, 5.f, 4.f}) &&
                sub_box.count(Layer::point) == 54 && sub_box.count(Layer::primitive) == 52,
            "Subdivided Box did not weld seam vertices or honor base_on_ground.");
    take(connectivity(sub_box));
    require(std::ranges::max(column<std::uint32_t>(sub_box, "piece", Layer::point)) == 0,
            "Subdivided Box left unwelded seam components.");
    const auto [sub_vol, sub_area] = solid_measures(take(solid_boolean(sub_box, {}, BooleanOperation::unite)));
    require(std::abs(sub_vol - 192.0) < 1e-4 && std::abs(sub_area - 208.0) < 1e-4,
            "Subdivided Box failed Boolean closed-manifold validation or volume/area check.");

    // 3. Generalized Tube: Cone (radius_top = 0) and 180-deg Hollow Barrel Vault:
    const auto cone = take(tube(
        {.radius_top = 0.f, .radius_bottom = 3.f, .height = 4.f, .base_on_ground = true, .segments = 32, .rows = 2}));
    const double expected_cone_vol = (1.0 / 3.0) * (0.5 * 32.0 * 9.0 * std::sin(2.0 * std::numbers::pi / 32.0)) * 4.0;
    require(
        bounds(cone)->minimum.y == 0.f && std::abs(solid_measures(cone).first - expected_cone_vol) < 1e-3 &&
            take(volume_scatter(cone, {.density = 4.f, .seed = 3, .cell_size = 0.5f})).count(Layer::point) > 10,
        "Cone Tube (radius_top = 0) failed base_on_ground, analytic volume or VolumeScatter closed-manifold check.");

    const auto vault = take(tube({.radius_top = 3.f,
                                  .radius_bottom = 3.f,
                                  .inner_radius = 2.f,
                                  .height = 2.f,
                                  .segments = 24,
                                  .rows = 2,
                                  .arc_degrees = 180.f,
                                  .end_caps = true}));
    const double expected_vault_vol = 2.0 * 24.0 * 0.5 * (9.0 - 4.0) * std::sin(std::numbers::pi / 24.0);
    const auto vault_checked = take(solid_boolean(vault, {}, BooleanOperation::unite));
    require(std::abs(solid_measures(vault_checked).first - expected_vault_vol) < 1e-3,
            "Partial-arc hollow Tube vault was not a watertight manifold of expected volume.");

    // 4. Wedge: symmetric gabled roof (0.5) and right-angle ramp (0.0):
    const auto roof =
        take(wedge({.size = {4.f, 3.f, 6.f}, .peak_ratio = 0.5f, .base_on_ground = true, .divisions_z = 2}));
    const auto ramp =
        take(wedge({.size = {4.f, 3.f, 6.f}, .peak_ratio = 0.0f, .base_on_ground = true, .divisions_z = 1}));
    require(bounds(roof)->minimum.y == 0.f &&
                std::abs(solid_measures(take(solid_boolean(roof, {}, BooleanOperation::unite))).first - 36.0) < 1e-4 &&
                std::abs(solid_measures(take(solid_boolean(ramp, {}, BooleanOperation::unite))).first - 36.0) < 1e-4,
            "Wedge gabled roof or right-angle ramp failed watertight manifold volume check.");

    // 5. Circle + Sweep SOP graph: zero-.source stone arch + 270-degree convex pie sector:
    const auto sector_270 = take(circle({.primitive_type = CirclePrimitive::polygon,
                                         .orientation = OrientationPlane::zx,
                                         .radius = {2.f, 2.f},
                                         .segments = 18,
                                         .arc_degrees = 270.f}));
    require(take(scatter(sector_270, {.density = 4.f, .seed = 7})).count(Layer::point) > 10,
            "Reflex 270-degree Circle polygon sector violated shared convex polygon tolerance.");

    Runtime runtime;
    take(register_types(runtime));
    Meshes resources;
    auto graph = take(runtime.create(type<Graph>(runtime)));
    const auto arch_arc = take(add_node(runtime, graph.id(), type<Circle>(runtime)));
    const auto arch_prof = take(add_node(runtime, graph.id(), type<Circle>(runtime)));
    const auto arch_sweep = take(add_node(runtime, graph.id(), type<Sweep>(runtime)));
    const auto arch_match = take(add_node(runtime, graph.id(), type<MatchSize>(runtime)));
    const auto ref_box = take(add_node(runtime, graph.id(), type<Box>(runtime)));

    take(runtime.with_native<Circle>(arch_arc.id(), [](Circle& n) -> Result<void> {
        n.primitive_type = CirclePrimitive::open_arc;
        n.orientation = OrientationPlane::xy;
        n.radius = {3.f, 3.f};
        n.center = {0.f, 2.f, 0.f};
        n.segments = 16;
        n.arc_degrees = 180.f;
        return {};
    }));
    take(runtime.with_native<Circle>(arch_prof.id(), [](Circle& n) -> Result<void> {
        n.primitive_type = CirclePrimitive::polygon;
        n.orientation = OrientationPlane::xy;
        n.radius = {0.35f, 0.25f};
        n.segments = 6;
        return {};
    }));
    take(runtime.with_native<Sweep>(arch_sweep.id(), [](Sweep& n) -> Result<void> {
        n.profile = SweepProfile::custom;
        n.up = {0.f, 0.f, 1.f};
        n.end_caps = true;
        return {};
    }));
    take(connect(runtime, graph.id(), arch_arc.id(), 0, arch_sweep.id(), 0));
    take(connect(runtime, graph.id(), arch_prof.id(), 0, arch_sweep.id(), 1));
    take(connect(runtime, graph.id(), arch_sweep.id(), 0, arch_match.id(), 0));
    take(set_display(runtime, graph.id(), arch_match.id()));

    Evaluator eval{runtime, graph, resources};
    take(eval.evaluate());
    require(eval.output() != nullptr, "Circle -> Sweep -> MatchSize stone arch graph failed to evaluate.");
    const auto arch_checked = take(solid_boolean(*eval.output(), {}, BooleanOperation::unite));
    const auto arch_b = bounds(arch_checked);
    require(arch_b && std::abs(arch_b->minimum.y) < 1e-4f &&
                std::abs((arch_b->minimum.x + arch_b->maximum.x) * 0.5f) < 1e-4f &&
                solid_measures(arch_checked).first > 1.5,
            "Swept hexagonal stone arch was not watertight or MatchSize did not ground-and-center it.");

    {
        const auto unit = take(box({1.f, 1.f, 1.f}));
        const Geometry empty;
        MatchSizeSettings fit;
        fit.scale_to_fit = true;
        fit.target_size = {5e-7f, 1.f, 1.f};
        const auto fitted = take(match_size(unit, fit, &empty));
        const auto bb = bounds(fitted);
        require(bb && std::abs((bb->maximum.x - bb->minimum.x) - fit.target_size.x) < 1e-12f,
                "MatchSize ignored a small positive target extent.");
        fit.target_size.x = 0.f;
        const auto invalid = match_size(unit, fit, &empty);
        require(!invalid &&
                    invalid.error().message == "MatchSize target_size must be positive when scale_to_fit is enabled.",
                "An empty reference bypassed the independent MatchSize target validation.");
    }

    // MatchSize with connected reference port + uniform scale_to_fit:
    take(runtime.with_native<Box>(ref_box.id(), [](Box& n) -> Result<void> {
        n.size = {10.f, 10.f, 10.f};
        n.center = {20.f, 4.f, -6.f};
        n.base_on_ground = true;
        return {};
    }));
    take(runtime.with_native<MatchSize>(arch_match.id(), [](MatchSize& n) -> Result<void> {
        n.scale_to_fit = true;
        n.uniform_scale = true;
        return {};
    }));
    take(connect(runtime, graph.id(), ref_box.id(), 0, arch_match.id(), 1));
    take(eval.evaluate());
    const auto fit_b = bounds(*eval.output());
    const Vector3 fit_ext = math::size(*fit_b);
    require(fit_b && near(fit_b->minimum.y, 4.f, 1e-4f) &&
                near(std::max(fit_ext.x, std::max(fit_ext.y, fit_ext.z)), 10.f, 1e-4f) &&
                near((fit_b->minimum.x + fit_b->maximum.x) * 0.5f, 20.f, 1e-4f),
            "MatchSize did not uniformly fit and ground-align input to connected reference Box.");

    // 6. Torus (360-deg closed ring & 90-deg capped pipe elbow) + Transform pivot:
    const auto ring = take(torus({.major_radius = 2.f,
                                  .minor_radius = 0.5f,
                                  .base_on_ground = true,
                                  .major_segments = 32,
                                  .minor_segments = 16}));
    const double expected_ring_vol = 0.5 * 32.0 * 16.0 * 2.0 * 0.25 * std::sin(2.0 * std::numbers::pi / 32.0) *
                                     std::sin(2.0 * std::numbers::pi / 16.0);
    require(near(bounds(ring)->minimum.y, 0.f, 1e-5f) &&
                std::abs(solid_measures(take(solid_boolean(ring, {}, BooleanOperation::unite))).first -
                         expected_ring_vol) < 1e-4,
            "Full Torus failed base_on_ground or closed-manifold Boolean volume check.");
    const auto elbow = take(torus({.major_radius = 2.f,
                                   .minor_radius = 0.4f,
                                   .major_segments = 12,
                                   .minor_segments = 12,
                                   .arc_degrees = 90.f,
                                   .end_caps = true}));
    const double expected_elbow_vol =
        12.0 * std::sin(0.5 * std::numbers::pi / 12.0) * 2.0 * (6.0 * 0.16 * std::sin(2.0 * std::numbers::pi / 12.0));
    require(std::abs(solid_measures(take(solid_boolean(elbow, {}, BooleanOperation::unite))).first -
                     expected_elbow_vol) < 1e-4,
            "Partial-arc capped Torus elbow was not a valid closed manifold.");

    auto pivoted = take(box({1.f, 1.f, 1.f}));
    take(transform(pivoted, {.rotate_degrees = {0.f, 90.f, 0.f}, .pivot = {2.f, 0.f, 0.f}}));
    const auto piv_b = bounds(pivoted);
    // Center (0,0,0) relative to pivot (2,0,0) is (-2,0,0); +90 deg Y in row-vector convention maps (-2,0,0) -> (0,0,2), plus pivot = (2,0,2)!
    require(piv_b && near(math::center(*piv_b), {2.f, 0.f, 2.f}, 1e-4f),
            "Transform Euler + Pivot did not rotate box around the specified pivot.");
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
    grouped_wall_surfaces();
    resampled_wall_and_post_paths();
    connectivity_modeling();
    boolean_modeling();
    boolean_contact_sweep();
    graph_evaluation();
    source_and_spatial_contracts();
    generative_modeling_and_l3_contracts();
    asset_subgraph_and_cache_pool_contracts();
    parametric_primitives_and_alignment_contracts();
    std::puts("procedural acceptance passed");
    return 0;
}
