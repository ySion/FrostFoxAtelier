#include "boolean_profile.hpp"
#include "../helpers.hpp"
#include "procedural/operations.hpp"

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
#include <sstream>

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

bool scenario(std::ostream& report, const Geometry& subject, const Geometry& cutter, int boxes, bool coplanar,
              int segments = 0) {
    const bool curved = segments != 0;
    const int preheats = curved ? 3 : warmups;
    std::array<double, samples> times{};
    const std::string name = curved ? "sphere_" + std::to_string(segments) + "x" + std::to_string(segments / 2)
                                    : (coplanar ? "coplanar_heavy" : "sparse_contact");
    report << "{\"name\":" << std::quoted(name);
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
            if (!curved) {
                const double overlap = (2. - offset) * (coplanar ? boxes * 4. : (2. - .375) * (2. - .25));
                require(std::abs(m.volume - (8. * boxes - overlap)) < 1e-4,
                        "Moving Boolean changed the expected volume.");
            }
            report << ",\"output_primitives\":" << result->count(Layer::primitive) << ",\"output_volume\":" << m.volume
                   << ",\"output_area\":" << m.area << ",\"signature\":\"" << std::hex << m.signature << std::dec
                   << "\"";
        } else {
            report << ",\"error\":" << std::quoted(result.error().message);
        }
        report << ",\"input_faces\":" << stats.input_faces << ",\"candidate_pairs\":" << stats.candidate_pairs
               << ",\"ray_candidates\":" << stats.ray_candidates << ",\"splits\":" << stats.splits
               << ",\"peak_face_scratch_bytes\":" << stats.peak_face_scratch_bytes
               << ",\"prepare_ns\":" << stats.prepare.count() << ",\"arrange_ns\":" << stats.arrange.count()
               << ",\"sew_ns\":" << stats.sew.count() << ",\"publish_ns\":" << stats.publish.count() << "}";
    }
    std::ranges::sort(times);
    report << "\n],\"median_ms\":" << times[samples / 2] << ",\"p95_ms\":" << times[(95 * samples + 99) / 100 - 1]
           << ",\"successful_cooks\":" << successes << "}";
    return required_poses_passed;
}
}

void profile_boolean(const char* path) {
    const std::filesystem::path output{path};
    require(output.is_absolute() && !std::filesystem::exists(output), "Choose a new absolute profile file.");
    const std::array source{take(box({2, 2, 2}))};
    bool valid = true;
    std::ostringstream report;
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
    for (const int segments : {24, 48, 96}) {
        const auto shell = take(sphere(1, segments, segments / 2));
        report << ",\n";
        valid &= scenario(report, shell, shell, 0, false, segments);
    }
    report << "\n]}\n";
    const auto json = std::move(report).str();
    std::ofstream file{output, std::ios::binary | std::ios::noreplace};
    file << json;
    file.close();
    require(static_cast<bool>(file), "The Boolean profile file cannot be written.");
    std::fputs(json.c_str(), stdout);
    require(valid, "A required Boolean profile pose was rejected; see the JSON report.");
}
}
