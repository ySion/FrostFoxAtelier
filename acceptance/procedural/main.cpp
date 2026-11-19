#include "memory/fatal.hpp"
#include "../helpers.hpp"
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
    const std::uint32_t jseed = hash(11u, 5u);
    const std::uint32_t h1 = hash(hash(jseed, 1u));
    const float expected_x1 = 1.0f + (unit(h1) * 2.0f - 1.0f) * 0.5f;
    require(near(out_p[0], {-1.0f, 0.0f, 1.0f}) && near(out_p[1], {-1.0f, 0.25f, -expected_x1}) && pscale[0] == 0.0f &&
                pscale[1] == 2.0f && custom_h[0] == 0.0f && custom_h[1] == 0.25f,
            "Jitter fallback index, GroupBox mask, RandomScale or custom Wave produced wrong values.");
    // NormalAlign had +Z -> +Y (0,1,0) and +Y -> +Z (0,0,1); after +90 deg about Y, local +Y (0,0,1) rotates to +X (1,0,0)!
    require(near(math::rotate(orient[1], Vector3{0, 0, 1}), {0, 1, 0}, 1e-4f) &&
                near(math::rotate(orient[1], Vector3{0, 1, 0}), {1, 0, 0}, 1e-4f),
            "Transform did not compose rotation onto Usage::orientation.");

    take(set_display(runtime, graph, copy_n.id()));
    take(evaluator.evaluate());
    require(evaluator.output() && evaluator.output()->count(Layer::instance) == 4,
            "CopyToPoints failed after Source chain.");
    const auto pub = take(publish(*evaluator.output(), 32.0f));
    std::size_t kept_instances = 0;
    for (const auto& col : pub.collections)
        kept_instances += col.transforms->size();
    require(pub.dropped == 2 && kept_instances == 2, "publish() did not drop the 2 zero-pscale unselected instances.");

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
        number("random_scale_1m_ms", median_ms(
                                         samples, [](int) {},
                                         [&] {
                                             const std::array args{Argument::borrow(ctx), Argument::borrow(float_buf),
                                                                   Argument::borrow(id_buf),
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
    require(argc == 1, "Usage: atelier_procedural_acceptance [--profile-procedural new-output.json]");
    ray_node();
    graph_evaluation();
    source_and_spatial_contracts();
    std::puts("procedural acceptance passed");
    return 0;
}
