#include "foundation/map.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <intrin.h>
#include <mimalloc.h>

namespace {
struct Usage {
    bool active{};
    size_t live{}, peak{};
} usage;

void* allocated(void* p) noexcept {
    if (usage.active && p) {
        usage.live += mi_usable_size(p);
        usage.peak = std::max(usage.live, usage.peak);
    }
    return p;
}

void* releasing(void* p) noexcept {
    if (usage.active && p)
        usage.live -= mi_usable_size(p);
    return p;
}
}

// Instrument only this executable. Keep all of the project's scalar/array
// and aligned allocation forms, including the same fatal OOM handling.
#define mi_malloc(n) allocated((mi_malloc)(n))
#define mi_malloc_aligned(n, a) allocated((mi_malloc_aligned)(n, a))
#define mi_free(p) (mi_free)(releasing(p))
#define mi_free_size(p, n) (mi_free_size)(releasing(p), n)
#define mi_free_aligned(p, a) (mi_free_aligned)(releasing(p), a)
#define mi_free_size_aligned(p, n, a) (mi_free_size_aligned)(releasing(p), n, a)
#include "../../../modules/core/memory/src/global_new_delete.cpp"

unsigned long long ticks() {
    _mm_lfence();
    const auto now = __rdtsc();
    _mm_lfence();
    return now;
}

void growth(const char* name, uint64_t stride) {
    std::array<unsigned long long, 64> worst{}, at_40961{}, at_49153{}, at_52429{};
    for (size_t round = 0; round < worst.size(); ++round) {
        atelier::Map<uint64_t, uint64_t> map;
        for (uint64_t i = 0; i < 65536; ++i) {
            const auto key = i * stride;
            const auto start = ticks();
            const bool added = map.try_emplace(key, i).inserted;
            const auto elapsed = ticks() - start;
            worst[round] = std::max(worst[round], elapsed);
            if (i == 40960)
                at_40961[round] = elapsed;
            if (i == 49152)
                at_49153[round] = elapsed;
            if (i == 52428)
                at_52429[round] = elapsed;
            if (!added)
                atelier::fatal("Incomplete growth replay.");
        }
        if (*map.find(65535 * stride) != 65535)
            atelier::fatal("Incorrect last entry.");
    }
    std::sort(worst.begin(), worst.end());
    std::sort(at_40961.begin(), at_40961.end());
    std::sort(at_49153.begin(), at_49153.end());
    std::sort(at_52429.begin(), at_52429.end());
    std::printf("%s_worst_p50_tsc,%llu\n%s_worst_p95_tsc,%llu\n", name, worst[32], name, worst[60]);
    std::printf("%s_at_40961_p95_tsc,%llu\n", name, at_40961[60]);
    std::printf("%s_at_49153_p95_tsc,%llu\n%s_at_52429_p95_tsc,%llu\n", name, at_49153[60], name, at_52429[60]);
}

int main() {
    for (bool reserve : {false, true}) {
        size_t retained;
        usage = {true};
        {
            atelier::Map<uint64_t, uint64_t> map;
            if (reserve)
                map.reserve(65536);
            for (uint64_t key = 0; key < 65536; ++key)
                map.try_emplace(key, key + 7);
            if (*map.find(uint64_t{65535}) != 65542)
                return 1;
            retained = usage.live;
        }
        usage.active = false;
        if (usage.live || !retained || usage.peak < retained)
            return 1;
        const auto label = reserve ? "reserved" : "natural";
        std::printf("%s_live_bytes,%zu\n%s_peak_bytes,%zu\n", label, retained, label, usage.peak);
    }
    // At this capacity a 10-entry group target has just doubled its index.
    usage = {true};
    {
        atelier::Map<uint64_t, uint64_t> map;
        map.reserve(81921);
        std::printf("reserved_81921_bytes,%zu\n", usage.live);
    }
    usage.active = false;
    if (usage.live)
        return 1;
    growth("dense_64k", 1);
    growth("spread_64k", UINT64_C(0x9e3779b97f4a7c15));
}
