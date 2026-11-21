#include "foundation/map.hpp"
#include "foundation/set.hpp"
#include "assets/mesh_import.hpp"
#include "procedural/operations.hpp"
#include "presentation/presentation.hpp"
#include "world/world.hpp"
#include "memory/fatal.hpp"
#include <Windows.h>
#include <intrin.h>
#include <mimalloc.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <numeric>
#include <random>
#include <string>
#include <vector>
using namespace atelier;
using Clock = std::chrono::steady_clock;
void need(bool ok) {
    if (!ok)
        fatal("Invalid profile output");
}
template <class F>
double ns(F&& f) {
    const auto start = Clock::now();
    f();
    return std::chrono::duration<double, std::nano>(Clock::now() - start).count();
}
template <class F>
void metric(const char* key, size_t ops, F&& f) {
    f();
    std::printf("%s,%.5f\n", key, ns(f) / ops);
}
void seed(Runtime& rt) {
    need(world::register_types(rt).has_value());
    need(presentation::register_types(rt).has_value());
}
struct Snapshot {
    size_t blocks{}, bytes{};
};
Snapshot memory() {
    mi_collect(true);
    Snapshot total;
    mi_theap_visit_blocks(
        mi_theap_get_default(), false,
        [](const mi_heap_t*, const mi_heap_area_t* area, void*, size_t, void* ctx) {
            auto& s = *static_cast<Snapshot*>(ctx);
            s.blocks += area->used;
            s.bytes += area->used * area->block_size;
            return true;
        },
        &total);
    return total;
}
void footprint() {
    {
        Runtime warm;
        seed(warm);
    }
    const auto before = memory();
    Runtime live;
    seed(live);
    const auto after = memory();
    need(after.blocks > before.blocks && after.bytes > before.bytes);
    std::printf("registered_bytes,%zu\nregistered_blocks,%zu\n", after.bytes - before.bytes,
                after.blocks - before.blocks);
}
void registration() {
    double total = 0;
    for (unsigned i = 0; i < 24; ++i) {
        const auto start = Clock::now();
        Runtime rt;
        seed(rt);
        const auto stop = Clock::now();
        if (i >= 4)
            total += std::chrono::duration<double, std::micro>(stop - start).count();
        need(rt.type_count() == 46);
    }
    std::printf("register_us,%.5f\n", total / 20);
}
template <class Id>
struct Name {
    TypeId type;
    std::string key;
    Id id;
};
template <class Id>
void runtime_reads(Runtime& rt, std::vector<Name<Id>> names, const char* key, bool miss, bool foreign = false,
                   bool change_first = false) {
    need(!names.empty());
    std::mt19937 gen(1203);
    std::vector<std::string> catalog;
    if (foreign)
        for (auto t : rt.types()) {
            auto info = *rt.describe(t);
            for (auto& m : info->methods)
                catalog.push_back(m.name);
            for (auto& f : info->fields)
                catalog.push_back(f.name);
        }
    std::vector<Name<Id>> trace;
    for (unsigned i = 0; i < 4096; ++i) {
        auto q = names[gen() % names.size()];
        if (foreign) {
            for (;;) {
                const auto& alternate = catalog[gen() % catalog.size()];
                const auto& info = **rt.describe(q.type);
                const bool present = [&] {
                    if constexpr (std::is_same_v<Id, FieldId>)
                        return std::ranges::any_of(info.fields, [&](const auto& f) { return f.name == alternate; });
                    else
                        return std::ranges::any_of(info.methods, [&](const auto& m) { return m.name == alternate; });
                }();
                if (!present) {
                    q.key = alternate;
                    break;
                }
            }
        } else if (miss) {
            (change_first ? q.key.front() : q.key.back()) = '#';
        }
        trace.push_back(std::move(q));
    }
    metric(key, trace.size() * 20, [&] {
        unsigned ok = 0;
        for (unsigned pass = 0; pass < 20; ++pass)
            for (const auto& q : trace) {
                auto result = [&] {
                    if constexpr (std::is_same_v<Id, FieldId>)
                        return rt.find_field(q.type, q.key);
                    else
                        return rt.find_method(q.type, q.key);
                }();
                ok += result ? (!miss && *result == q.id) : (miss && result.error().code == ErrorCode::invalid_member);
            }
        need(ok == trace.size() * 20);
    });
}
void query() {
    Runtime rt;
    seed(rt);
    std::vector<Name<FieldId>> tiny, larger;
    std::vector<Name<MethodId>> methods;
    for (const auto t : rt.types()) {
        auto info = *rt.describe(t);
        for (const auto& f : info->fields)
            (info->fields.size() <= 4 ? tiny : larger).push_back({t, f.name, f.id});
        for (const auto& m : info->methods)
            methods.push_back({t, m.name, m.id});
    }
    runtime_reads(rt, tiny, "field_small_hit_ns", false);
    runtime_reads(rt, tiny, "field_small_miss_ns", true);
    runtime_reads(rt, larger, "field_larger_hit_ns", false);
    runtime_reads(rt, larger, "field_larger_miss_ns", true);
    runtime_reads(rt, methods, "method_hit_ns", false);
    runtime_reads(rt, methods, "method_miss_ns", true);
    runtime_reads(rt, tiny, "field_small_foreign_ns", true, true);
    runtime_reads(rt, larger, "field_larger_foreign_ns", true, true);
    runtime_reads(rt, methods, "method_foreign_ns", true, true);
    runtime_reads(rt, larger, "field_larger_first_byte_miss_ns", true, false, true);
}
unsigned long long stamp() {
    _mm_lfence();
    const auto t = __rdtsc();
    _mm_lfence();
    return t;
}
using M = Map<uint64_t, uint64_t>;
using S = Set<uint64_t>;
template <class T>
bool add(T& t, uint64_t k) {
    if constexpr (std::is_same_v<T, M>)
        return t.try_emplace(k, k + 7).inserted;
    else
        return t.add(k);
}
template <class T>
void latency(const char* key, unsigned before, unsigned reserve, bool duplicate) {
    std::vector<unsigned long long> ticks;
    ticks.reserve(4096);
    for (unsigned j = 0; j < 4352; ++j) {
        T t;
        if (reserve)
            t.reserve(reserve);
        for (uint64_t k = 0; k < before; ++k)
            need(add(t, k));
        const auto lo = stamp();
        const bool inserted = add(t, duplicate ? before - 1 : before);
        const auto hi = stamp();
        if (j >= 256)
            ticks.push_back(hi - lo);
        need(inserted != duplicate && t.size() == before + !duplicate);
    }
    std::sort(ticks.begin(), ticks.end());
    std::printf("%s_p50_tsc,%llu\n%s_p95_tsc,%llu\n", key, ticks[ticks.size() / 2], key,
                ticks[(ticks.size() * 95) / 100]);
}
template <class T>
void large(unsigned n, const char* key, bool spread = false) {
    T t;
    t.reserve(n);
    const uint64_t stride = spread ? UINT64_C(0x9e3779b97f4a7c15) : 1;
    for (unsigned i = 0; i < n; ++i)
        need(add(t, i * stride));
    std::mt19937 gen(42);
    std::vector<uint64_t> qs(16384);
    size_t expected = 0;
    for (auto& q : qs) {
        q = gen() % (2 * n);
        expected += q < n;
        q *= stride;
    }
    metric(key, qs.size() * 48, [&] {
        size_t sum = 0;
        for (unsigned pass = 0; pass < 48; ++pass) {
            std::atomic_signal_fence(std::memory_order_seq_cst);
            for (auto q : qs) {
                const auto* v = t.find(q);
                sum += v && *v == q + (std::is_same_v<T, M> ? 7 : 0);
            }
        }
        need(sum == expected * 48);
    });
}
void growth_and_churn() {
    constexpr size_t live = 4096;
    std::vector<uint64_t> keys(live);
    std::iota(keys.begin(), keys.end(), uint64_t{0});
    std::mt19937 random(42);
    std::shuffle(keys.begin(), keys.end(), random);
    metric("map_churn_ns", live * 16, [&] {
        M table;
        table.reserve(live);
        for (const auto key : keys)
            need(add(table, key));
        for (unsigned pass = 0; pass < 16; ++pass) {
            for (const auto key : keys) {
                const auto old = key + pass * live;
                need(table.remove(old) && !table.contains(old) && add(table, old + live));
            }
            uint64_t sum = 0;
            for (auto [key, value] : table)
                sum += value - key;
            need(sum == 7 * live && table.size() == live);
        }
    });
    std::vector<unsigned long long> worst, at_6145, at_6554;
    for (unsigned round = 0; round < 64; ++round) {
        M table;
        unsigned long long peak = 0;
        for (uint64_t i = 0; i < 8192; ++i) {
            const auto start = stamp();
            const bool inserted = add(table, i);
            const auto ticks = stamp() - start;
            peak = std::max(peak, ticks);
            if (i == 6144)
                at_6145.push_back(ticks);
            if (i == 6553)
                at_6554.push_back(ticks);
            need(inserted);
        }
        worst.push_back(peak);
    }
    std::sort(worst.begin(), worst.end());
    std::sort(at_6145.begin(), at_6145.end());
    std::sort(at_6554.begin(), at_6554.end());
    std::printf("growth_worst_p95_tsc,%llu\n", worst[60]);
    std::printf("growth_6145_p95_tsc,%llu\ngrowth_6554_p95_tsc,%llu\n", at_6145[60], at_6554[60]);
}

void mesh_weld(const char* path) {
    auto mesh = assets::load_obj(path);
    need(mesh.has_value());
    const auto reference = procedural::import_mesh(*mesh->mesh);
    need(reference.positions().size() == 1089 && reference.vertex_points().size() == 6144);
    uint32_t digest = 2166136261u;
    for (const auto index : reference.vertex_points())
        digest = (digest ^ index) * 16777619u;
    for (const auto& point : reference.positions())
        for (const float coordinate : {point.x, point.y, point.z})
            digest = (digest ^ std::bit_cast<uint32_t>(coordinate)) * 16777619u;
    std::printf("mesh_digest,%u\n", digest);
    double total = 0;
    for (unsigned i = 0; i < 64; ++i) {
        const auto start = Clock::now();
        auto result = procedural::import_mesh(*mesh->mesh);
        total += std::chrono::duration<double, std::micro>(Clock::now() - start).count();
        need(std::ranges::equal(reference.vertex_points(), result.vertex_points()) &&
             std::ranges::equal(reference.positions(), result.positions()));
    }
    std::printf("weld_us,%.5f\n", total / 64);
}

int main(int argc, char** argv) {
    install_fatal_handler();
    need(argc == 1 || argc == 2);
    DWORD_PTR mask{}, machine{};
    need(GetProcessAffinityMask(GetCurrentProcess(), &mask, &machine));
    const DWORD_PTR pin = mask & (~mask + 1);
    need(SetThreadAffinityMask(GetCurrentThread(), pin) != 0);
    registration();
    query();
    footprint();
    if (argc == 2) {
        mesh_weld(argv[1]);
        growth_and_churn();
        latency<M>("map_seventeenth", 16, 0, false);
        large<M>(65536, "spread_map_64k_ns", true);
        large<S>(65536, "spread_set_64k_ns", true);
    }
    latency<M>("map_first", 0, 0, false);
    latency<M>("map_third", 2, 0, false);
    latency<M>("map_third_prereserved", 2, 3, false);
    latency<M>("map_fourth", 3, 0, false);
    latency<M>("map_fifth", 4, 0, false);
    latency<M>("map_fifth_prereserved", 4, 5, false);
    latency<M>("map_sixth", 5, 0, false);
    latency<M>("map_duplicate4", 4, 0, true);
    latency<S>("set_third", 2, 0, false);
    latency<S>("set_fifth", 4, 0, false);
    latency<S>("set_fifth_prereserved", 4, 5, false);
    large<M>(1024, "map_1k_ns");
    large<M>(65536, "map_64k_ns");
    large<S>(1024, "set_1k_ns");
    large<S>(65536, "set_64k_ns");
    std::vector<unsigned long long> clock_cost;
    for (unsigned k = 0; k < 4096; ++k) {
        const auto lo = stamp();
        const auto hi = stamp();
        clock_cost.push_back(hi - lo);
    }
    std::sort(clock_cost.begin(), clock_cost.end());
    std::printf("clock_p50_tsc,%llu\n", clock_cost[2048]);
    const auto t0 = Clock::now();
    const auto lo = stamp();
    Sleep(50);
    const auto hi = stamp();
    const auto t1 = Clock::now();
    std::printf("tsc_per_ns,%.4f\n", (hi - lo) / std::chrono::duration<double, std::nano>(t1 - t0).count());
    std::printf("map_size,%zu\nset_size,%zu\n", sizeof(M), sizeof(S));
}
