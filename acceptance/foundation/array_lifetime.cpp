#include "foundation/array.hpp"
#include <mimalloc.h>
#include <mimalloc-stats.h>

#include <cstdio>
#include <cstdint>
#include <stdexcept>

namespace {
using namespace atelier;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::int64_t live_bytes() {
    mi_collect(true);
    mi_stats_t stats;
    mi_stats_init(&stats);
    require(mi_stats_get(&stats), "mimalloc statistics ABI mismatch.");
    return stats.malloc_normal.current + stats.malloc_huge.current;
}
}

int main() {
    try {
        const auto before = live_bytes();
        {
            const Array<std::uint32_t> source{7, 9};
            const Array<std::uint32_t> copy{source};
            require(copy[0] == 7 && copy[1] == 9, "Array buffer copy lost its input.");
        }
        require(live_bytes() == before, "Array retained live buffers after owner destruction.");
        std::puts("Array buffer lifetime: PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Array buffer lifetime: FAIL: %s\n", error.what());
        return 1;
    }
}
