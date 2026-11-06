#include "memory/fatal.hpp"
#include "foundation/string.hpp"
#include <mimalloc.h>
#include <mimalloc-stats.h>

#include <cstdio>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

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
    atelier::install_fatal_handler();
    try {
        const std::string ascii(192, 'a');
        std::vector<String> catalog;
        catalog.reserve(1);
        const auto before = live_bytes();
        catalog.emplace_back(StringView{ascii});
        require(catalog[0].view().std_view() == ascii, "String buffer lost its input.");
        catalog.clear();
        require(live_bytes() == before, "String catalog retained live allocations after clear.");
        std::puts("String buffer lifetime: PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "String buffer lifetime: FAIL: %s\n", error.what());
        return 1;
    }
}
