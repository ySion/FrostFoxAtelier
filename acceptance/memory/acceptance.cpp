#include "acceptance.hpp"
#include "crt_client.hpp"
#include "../helpers.hpp"

#include <mimalloc.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>

namespace atelier::acceptance {
namespace {

using helpers::require;

void require_mimalloc(const void* block) {
    require(block != nullptr && mi_is_in_heap_region(block), "Default allocation did not reach mimalloc.");
}

}

void verify_default_allocator() {
    // Proves load ordering: the DLL made this allocation before main, with plain CRT calls.
    require_mimalloc(atelier_crt_startup_block());

    void* block = std::calloc(32, sizeof(std::uint64_t));
    require_mimalloc(block);
    require(static_cast<std::uint64_t*>(block)[31] == 0, "calloc did not zero memory.");
    static_cast<std::uint64_t*>(block)[0] = 123;
    void* resized = atelier_crt_resize(block, 4096);
    if (!resized) {
        std::free(block);
        fatal("Cross-DLL realloc failed.");
    }
    require_mimalloc(resized);
    require(static_cast<std::uint64_t*>(resized)[0] == 123, "Cross-DLL realloc lost contents.");
    atelier_crt_free(resized);

    std::cout << "[pass] mimalloc: startup and cross-DLL lifetime\n";
}

}
