#include "acceptance.hpp"
#include "crt_client.hpp"

#include <mimalloc.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

namespace atelier::acceptance {
namespace {

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void require_mimalloc(const void* block) {
    require(block != nullptr && mi_is_in_heap_region(block), "Default allocation did not reach mimalloc.");
}

struct alignas(256) AlignedValue {
    std::uint64_t value{42};
};

}

void verify_default_allocator() {
    // Proves load ordering: the DLL made this allocation before main, with plain CRT calls.
    require_mimalloc(atelier_crt_startup_block());

    {
        const auto aligned = std::make_unique<AlignedValue>();
        require(reinterpret_cast<std::uintptr_t>(aligned.get()) % alignof(AlignedValue) == 0,
                "Over-aligned allocation lost its alignment.");
        require(aligned->value == 42, "C++ allocation corrupted contents.");
    }

    void* block = std::calloc(32, sizeof(std::uint64_t));
    require_mimalloc(block);
    require(static_cast<std::uint64_t*>(block)[31] == 0, "calloc did not zero memory.");
    static_cast<std::uint64_t*>(block)[0] = 123;
    void* resized = atelier_crt_resize(block, 4096);
    if (!resized) {
        std::free(block);
        throw std::runtime_error("Cross-DLL realloc failed.");
    }
    require_mimalloc(resized);
    require(static_cast<std::uint64_t*>(resized)[0] == 123, "Cross-DLL realloc lost contents.");
    atelier_crt_free(resized);

    void* thread_block{};
    {
        std::jthread worker{[&] { thread_block = std::malloc(512); }};
    }
    require_mimalloc(thread_block);
    std::free(thread_block);

    std::cout << "[pass] mimalloc: aligned ABI, startup/cross-DLL/thread lifetime\n";
}

}
