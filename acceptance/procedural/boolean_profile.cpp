#include "boolean_profile.hpp"
#include "../helpers.hpp"
#include "procedural/operations.hpp"
#include "procedural/evaluator.hpp"
#include "procedural/nodes.hpp"
#include "runtime/native.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>

namespace atelier::acceptance {
namespace {
using namespace procedural;

using helpers::require;

using helpers::take;

constexpr int samples = 21;

constexpr int warmups = 2;

struct Measures {
    double volume{};

    double area{};

    std::uint64_t signature{14695981039346656037ull};

    void hash(std::uint32_t word) {
        // FNV-1a over little-endian scalar components, independent of struct layout.
        for (int i = 0; i < 4; ++i) {
            signature ^= word & 255u;
            signature *= 1099511628211ull;
            word >>= 8;
        }
    }

    void hash(math::Vector3 v) {
        hash(std::bit_cast<std::uint32_t>(v.x));
        hash(std::bit_cast<std::uint32_t>(v.y));
        hash(std::bit_cast<std::uint32_t>(v.z));
    }
};

Measures measure(const Geometry& mesh) {
    Measures m;
    const auto* operands = take(mesh.read<std::uint32_t>("boolean_operand", Layer::primitive));
    const auto* origins = take(mesh.read<std::uint32_t>("boolean_primitive", Layer::primitive));
    const auto* normals = take(mesh.read<math::Vector3>("N", Layer::vertex));
    require(operands && origins && normals, "Boolean profile result is missing provenance or corner normals.");
    m.hash(mesh.count(Layer::point));
    for (const auto& p : mesh.positions())
        m.hash(p);
    m.hash(mesh.count(Layer::primitive));
    for (std::uint32_t i = 0; i < mesh.count(Layer::primitive); ++i) {
        const auto face = mesh.primitive(i);
        m.hash(static_cast<std::uint32_t>(face.kind));
        m.hash(face.count);
        m.hash((*operands)[i]);
        m.hash((*origins)[i]);
        for (std::uint32_t j = 0; j < face.count; ++j) {
            m.hash(mesh.vertex_points()[face.first + j]);
            m.hash((*normals)[face.first + j]);
        }
        const auto vertex = [&](std::uint32_t corner) {
            return math::Vector3d(mesh.positions()[mesh.vertex_points()[face.first + corner]]);
        };
        const auto a = vertex(0);
        for (std::uint32_t j = 1; j + 1 < face.count; ++j) {
            const auto cross = math::cross(vertex(j) - a, vertex(j + 1) - a);
            m.volume += math::dot(a, cross);
            m.area += math::length(cross);
        }
    }
    m.volume /= 6.;
    m.area /= 2.;
    return m;
}

void write_stats(std::ostream& report, const BooleanStats& stats) {
    report << ",\"input_faces\":" << stats.input_faces << ",\"candidate_pairs\":" << stats.candidate_pairs
           << ",\"ray_candidates\":" << stats.ray_candidates << ",\"splits\":" << stats.splits
           << ",\"peak_face_scratch_bytes\":" << stats.peak_face_scratch_bytes
           << ",\"prepare_ns\":" << stats.prepare.count() << ",\"arrange_ns\":" << stats.arrange.count()
           << ",\"sew_ns\":" << stats.sew.count() << ",\"publish_ns\":" << stats.publish.count() << "}";
}

bool scenario(std::ostream& report, const Geometry& subject, const Geometry& cutter, int boxes, bool coplanar,
              int segments = 0, std::array<std::uint64_t, 3>* signatures = nullptr) {
    const bool curved = segments != 0;
    const int preheats = curved ? 3 : warmups;
    std::array<double, samples> times{};
    const String name = curved ? String::concat("sphere_", segments, "x", segments / 2)
                               : (coplanar ? "coplanar_heavy"_s : "sparse_contact"_s);
    report << "{\"name\":" << std::quoted(name.c_str());
    if (!curved)
        report << ",\"boxes_per_operand\":" << boxes;
    report << ",\"triangles_per_operand\":" << (curved ? 2 * segments * (segments / 2 - 1) : boxes * 12)
           << ",\"warmups\":" << preheats << ",\"cooks\":[\n";
    int successes = 0;
    bool required_poses_passed = true;
    for (int i = -preheats; i < samples; ++i) {
        const float offset = curved ? .375f + float((i + preheats) % 3) * .125f : 0.25f + float(std::max(i, 0)) / 32.f;
        auto moving = cutter;
        const bool transverse = !curved && !coplanar;
        take(transform(moving, {offset, transverse ? .375f : 0.f, transverse ? .25f : 0.f}, {}, {1, 1, 1}));
        BooleanStats stats{};
        const auto start = std::chrono::steady_clock::now();
        const auto result = solid_boolean(subject, moving, BooleanOperation::subtract, &stats);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (!result) {
            std::fprintf(stderr, "Boolean profile %s cook %d, x offset %f: %s\n", name.c_str(), i,
                         static_cast<double>(offset), result.error().message.c_str());
            required_poses_passed = false;
        }
        if (i < 0)
            continue;
        times[i] = ms;
        if (i)
            report << ",\n";
        report << "{\"x_offset\":" << offset << ",\"cook_ms\":" << ms;
        if (result) {
            ++successes;
            const auto m = measure(*result);
            if (signatures)
                (*signatures)[i % 3] = m.signature;
            if (!curved) {
                const double overlap = (2. - offset) * (coplanar ? boxes * 4. : (2. - .375) * (2. - .25));
                require(std::abs(m.volume - (8. * boxes - overlap)) < 1e-4,
                        "Moving Boolean changed the expected volume.");
            }
            report << ",\"output_primitives\":" << result->count(Layer::primitive) << ",\"output_volume\":" << m.volume
                   << ",\"output_area\":" << m.area << ",\"signature\":\"" << std::hex << m.signature << std::dec
                   << "\"";
        } else {
            report << ",\"error\":" << std::quoted(result.error().message.c_str());
        }
        write_stats(report, stats);
    }
    std::ranges::sort(times);
    report << "\n],\"median_ms\":" << times[samples / 2] << ",\"p95_ms\":" << times[(95 * samples + 99) / 100 - 1]
           << ",\"successful_cooks\":" << successes << "}";
    return required_poses_passed;
}

void evaluator_scenario(std::ostream& report, const std::array<std::uint64_t, 3>& signatures) {
    struct ResourcesWithoutMeshes final : Resources {
        Result<geometry::MeshRef> mesh(StringView) const noexcept override {
            return failure("Unexpected mesh lookup.");
        }

        std::uint64_t revision() const noexcept override { return 0; }
    } resources;
    Runtime runtime;
    take(register_types(runtime));
    auto graph = take(runtime.create(take(runtime.native_type<Graph>())));
    const auto sphere_node = take(add_node(runtime, graph.id(), take(runtime.native_type<Sphere>())));
    const auto transform_type = take(runtime.native_type<Transform>());
    const auto move = take(add_node(runtime, graph.id(), transform_type));
    const auto op = take(add_node(runtime, graph.id(), take(runtime.native_type<Boolean>())));
    take(runtime.with_native<Sphere>(sphere_node.id(), [](Sphere& node) -> Result<void> {
        node.segments = 96;
        node.latitudes = 48;
        return {};
    }));
    take(connect(runtime, graph.id(), sphere_node.id(), 0, move.id(), 0));
    take(connect(runtime, graph.id(), sphere_node.id(), 0, op.id(), 0));
    take(connect(runtime, graph.id(), move.id(), 0, op.id(), 1));
    take(set_display(runtime, graph.id(), op.id()));
    const auto translate = take(runtime.find_field(transform_type, "translate"));
    const auto vector_type = take(runtime.native_type<math::Vector3>());
    for (const bool zero_budget : {true, false}) {
        if (!zero_budget)
            report << ",\n";
        report << "{\"name\":\"evaluator_sphere_96x48_" << (zero_budget ? "zero_budget" : "default_budget")
               << "\",\"triangles_per_operand\":9024,\"warmups\":3,\"cooks\":[\n";
        std::array<double, samples> times{};
        static_assert(samples % 3 == 0);
        for (int traversal = -1; traversal < samples / 3; ++traversal) {
            auto evaluator =
                zero_budget ? Evaluator{runtime, graph, resources, nullptr, 0} : Evaluator{runtime, graph, resources};
            // Observe the stationary geometry once before admission. Changing operation
            // makes the first measured pose an actual cook even with the same cutter.
            for (int pose = -1; pose < 3; ++pose) {
                if (pose <= 0)
                    take(runtime.with_native<Boolean>(op.id(), [&](Boolean& node) -> Result<void> {
                        node.operation = pose < 0 ? BooleanOperation::intersect : BooleanOperation::subtract;
                        return {};
                    }));
                const math::Vector3 offset{.375f + std::max(0, pose) * .125f, 0, 0};
                take(runtime.write(move.id(), translate, ValueInput{vector_type, &offset}));
                const auto begin = std::chrono::steady_clock::now();
                take(evaluator.evaluate(true));
                const double ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
                const auto* status = evaluator.status(op.id());
                require(status && status->evaluations == static_cast<unsigned>(pose + 2) && status->boolean_stats,
                        "The moving sphere Boolean did not actually cook.");
                if (status->error)
                    fatal(status->error->message.c_str());
                require(evaluator.output() && status->time, "Missing Boolean SOP result or timing.");
                if (pose < 0)
                    continue;
                const auto& stats = *status->boolean_stats;
                require(stats.prepared_input_hits == (!zero_budget && pose > 0 ? 1u : 0u),
                        "Unexpected moving sphere preparation hits.");
                const auto m = measure(*evaluator.output());
                require(m.signature == signatures[pose], "Prepared Boolean output differs from the direct cold solve.");
                if (traversal < 0)
                    continue;
                const int sample = traversal * 3 + pose;
                times[sample] = ms;
                if (sample)
                    report << ",\n";
                report << "{\"x_offset\":" << offset.x << ",\"cook_ms\":" << ms
                       << ",\"boolean_node_ns\":" << status->time->count()
                       << ",\"prepared_input_hits\":" << stats.prepared_input_hits << ",\"output_volume\":" << m.volume
                       << ",\"output_area\":" << m.area << ",\"signature\":\"" << std::hex << m.signature << std::dec
                       << "\"";
                write_stats(report, stats);
            }
        }
        std::ranges::sort(times);
        report << "\n],\"median_ms\":" << times[samples / 2] << ",\"p95_ms\":" << times[(95 * samples + 99) / 100 - 1]
               << ",\"successful_cooks\":" << samples << "}";
    }
}

}

void profile_boolean(const char* path) {
    const std::filesystem::path output{path};
    require(output.is_absolute() && !std::filesystem::exists(output), "Choose a new absolute profile file.");
    const std::array source{take(box({2, 2, 2}))};
    bool valid = true;
    std::ofstream report{output, std::ios::binary | std::ios::noreplace};
    require(static_cast<bool>(report), "The Boolean profile file cannot be opened.");
    report << std::setprecision(std::numeric_limits<double>::max_digits10);
    report << "{\n\"schema\":\"atelier.boolean.profile.v1\",\n\"configuration\":\""
#ifdef NDEBUG
           << "release"
#else
           << "dev"
#endif
           << "\",\n\"operation\":\"subtract\",\n\"warmups\":" << warmups << ",\"samples\":" << samples
           << ",\n\"scenarios\":[\n";
    for (const int boxes : {12, 96}) {
        const float extent = (boxes - 1) * 4.f;
        const auto subject = take(realize(source, take(line({}, {extent, 0, 0}, boxes))));
        const auto transverse = take(realize(source, take(line({}, {0, extent, 0}, boxes))));
        if (boxes != 12)
            report << ",\n";
        valid &= scenario(report, subject, transverse, boxes, false);
        report << ",\n";
        valid &= scenario(report, subject, subject, boxes, true);
    }
    std::array<std::uint64_t, 3> signatures{};
    for (const int segments : {24, 48, 96}) {
        const auto shell = take(sphere(1, segments, segments / 2));
        report << ",\n";
        valid &= scenario(report, shell, shell, 0, false, segments, segments == 96 ? &signatures : nullptr);
    }
    report << ",\n";
    evaluator_scenario(report, signatures);
    report << "\n]}\n";
    report.close();
    require(static_cast<bool>(report), "The Boolean profile file cannot be written.");
    std::ifstream in{output, std::ios::binary | std::ios::ate};
    const auto json_size = in.tellg();
    require(json_size >= 0, "The Boolean profile file size cannot be read.");
    Array<char> json;
    json.resize_uninitialized(static_cast<std::size_t>(json_size));
    in.seekg(0);
    if (!json.empty()) {
        require(static_cast<bool>(in.read(json.data(), static_cast<std::streamsize>(json.size()))),
                "The Boolean profile file cannot be read.");
        (void)std::fwrite(json.data(), 1, json.size(), stdout);
    }
    require(valid, "A required Boolean profile pose was rejected; see the JSON report.");
}
}
