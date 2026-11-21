#include "assets/mesh_import.hpp"
#include "detail/scene_geometry.hpp"
#include "foundation/hash.hpp"
#include "foundation/sha256.hpp"
#include <ankerl/unordered_dense.h>
#include <Windows.h>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <random>
#include <string>

#define XXH_INLINE_ALL
#include <xxhash.h>
#include <wyhash.h>

using namespace atelier;
using Clock = std::chrono::steady_clock;

void check(bool ok) {
    if (!ok) fatal("Hash or vertex replay mismatch.");
}

struct Ankerl {
    uint64_t operator()(std::string_view s) const { return ankerl::unordered_dense::hash<std::string_view>{}(s); }
};
struct Wy {
    uint64_t operator()(std::string_view s) const { return wyhash(s.data(), s.size(), 0, _wyp); }
};
struct Xxh {
    uint64_t operator()(std::string_view s) const { return XXH3_64bits(s.data(), s.size()); }
};
struct Project {
    uint64_t operator()(std::string_view s) const { return Hash<StringView>{}(StringView{s}); }
};

template <class F>
void metric(const char* name, size_t ops, F f) {
    const auto expected = f();
    const auto start = Clock::now();
    const auto actual = f();
    const auto ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    check(actual == expected);
    std::printf("%s,%.5f\n", name, ns / ops);
}

template <class H>
void hash_keys(const std::vector<std::string_view>& names, const geometry::Mesh& mesh) {
    metric("name_ns", names.size() * 256, [&] {
        uint64_t sum = 0;
        for (int n = 0; n < 256; ++n) {
            std::atomic_signal_fence(std::memory_order_seq_cst);
            for (auto s : names) sum += H{}(s);
        }
        return sum;
    });
    const auto vertices = mesh.vertices();
    metric("vertex_ns", vertices.size() * 128, [&] {
        uint64_t sum = 0;
        for (int n = 0; n < 128; ++n) {
            std::atomic_signal_fence(std::memory_order_seq_cst);
            for (const auto& v : vertices)
                sum += H{}({reinterpret_cast<const char*>(&v), sizeof(v)});
        }
        return sum;
    });
}

void verify_weld(const geometry::Mesh& mesh) {
    const auto result = renderer::detail::weld(mesh, nullptr);
    check(result.has_value() && result->indices.size() == mesh.vertices().size());
    std::map<std::array<std::byte, sizeof(geometry::Vertex)>, uint32_t> expected;
    size_t i = 0;
    for (const auto& v : mesh.vertices()) {
        const auto bits = std::bit_cast<std::array<std::byte, sizeof(v)>>(v);
        const auto [at, inserted] = expected.try_emplace(bits, static_cast<uint32_t>(expected.size()));
        (void)inserted;
        check(result->indices[i++] == at->second && at->second < result->vertices.size() &&
              std::memcmp(&result->vertices[at->second], &v, sizeof(v)) == 0);
    }
    check(result->vertices.size() == expected.size());
    foundation::Sha256 sha;
    sha.add(std::as_bytes(std::span{result->vertices}));
    sha.add(std::as_bytes(std::span{result->indices}));
    sha.add(std::as_bytes(std::span{result->faces}));
    std::printf("weld_digest,%s\n", sha.finish().c_str());
}

void verify_contract() {
    std::array<char, 4096 + 16> buffer;
    std::mt19937 rng(191);
    for (auto& c : buffer) c = static_cast<char>(rng());
    for (size_t offset = 0; offset < 16; ++offset) {
        for (size_t n = 0; n <= 4096; ++n) {
            const StringView view{buffer.data() + offset, n};
            const auto expected = Ankerl{}(view.std_view());
            check(Hash<StringView>{}(view) == expected);
#ifndef ATELIER_HASH_REFERENCE_BUILD
            check(hash_bytes(as_bytes(view)) == expected);
#endif
        }
    }
    check(Hash<StringView>{}(nullptr) == Ankerl{}({}));
#ifndef ATELIER_HASH_REFERENCE_BUILD
    check(hash_bytes({}) == Ankerl{}({}));
#endif
    enum class SignedKey : signed char { negative = -1 };
    check(Hash<SignedKey>{}(SignedKey::negative) == ankerl::unordered_dense::hash<SignedKey>{}(SignedKey::negative));
    check(Hash<const char*>{}(buffer.data()) == ankerl::unordered_dense::hash<const char*>{}(buffer.data()));
    check(Hash<const char*>{}(nullptr) == ankerl::unordered_dense::hash<const char*>{}(nullptr));
    for (int64_t x = -1024; x < 65536; ++x) {
        check(Hash<int64_t>{}(x) == ankerl::unordered_dense::hash<int64_t>{}(x));
        const auto y = static_cast<uint32_t>(rng());
        const std::tuple fields{x, y, ~y};
        check(hash_fields(x, y, ~y) == ankerl::unordered_dense::hash<std::remove_cv_t<decltype(fields)>>{}(fields));
    }
    geometry::Vertex vertex{};
    vertex.normal[2] = 1;
    vertex.tangent[0] = vertex.tangent[3] = 1;
    auto signed_zero = vertex;
    signed_zero.uv[1] = -0.0f;
    auto mirrored = vertex;
    mirrored.tangent[3] = -1;
    auto mesh = geometry::Mesh::restore_published({vertex, signed_zero, mirrored, signed_zero, mirrored, vertex},
                                                   {}, {{"test"}}, {{0, 6, 0}});
    check(mesh.has_value());
    verify_weld(**mesh);
    const auto welded = renderer::detail::weld(**mesh, nullptr);
    check(welded && welded->indices == std::vector<uint32_t>{0, 1, 2, 1, 2, 0});
    const std::function<bool()> stop = [] { return true; };
    const auto cancelled = renderer::detail::weld(**mesh, &stop);
    check(!cancelled && std::holds_alternative<renderer::detail::FrameCancelled>(cancelled.error()));
}

int main(int argc, char** argv) {
    check(argc == 4 && SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << 18));
    const auto imported = assets::load_obj(argv[1]);
    check(imported.has_value());
    const auto& mesh = *imported->mesh;
    std::ifstream in(argv[2]);
    std::vector<std::string> catalog;
    for (std::string line; std::getline(in, line);) catalog.push_back(line.substr(line.rfind('\t') + 1));
    check(!catalog.empty());
    std::mt19937 rng(1203);
    std::vector<std::string_view> names(4096);
    for (auto& name : names) name = catalog[rng() % catalog.size()];
    const std::string_view mode{argv[3]};
    if (mode == "verify") verify_contract();
    else if (mode == "ankerl") hash_keys<Ankerl>(names, mesh);
    else if (mode == "wy") hash_keys<Wy>(names, mesh);
    else if (mode == "xxh3") hash_keys<Xxh>(names, mesh);
    else if (mode == "project") hash_keys<Project>(names, mesh);
    else if (mode == "weld") {
        verify_weld(mesh);
        metric("weld_ns", 64, [&] {
            size_t total = 0;
            for (int n = 0; n < 64; ++n) {
                const auto result = renderer::detail::weld(mesh, nullptr);
                check(result.has_value());
                total += result->vertices.size();
            }
            return total;
        });
    } else return 1;
}
