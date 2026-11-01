#include "acceptance.hpp"
#include "crt_client.hpp"

#include <mimalloc.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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

struct ExpectedAllocationFailure {
    bool show_errors{mi_option_is_enabled(mi_option_show_errors)};

    ExpectedAllocationFailure() { mi_option_disable(mi_option_show_errors); }

    ~ExpectedAllocationFailure() { mi_option_set_enabled(mi_option_show_errors, show_errors); }
};

}

void verify_default_allocator() {
    require(mi_version() == MI_MALLOC_VERSION, "Loaded mimalloc DLL does not match the compiled headers.");
    require(mi_is_redirected(), "Windows CRT redirection is not active.");
    // Proves load ordering: the DLL made this allocation before main, with plain CRT calls.
    require_mimalloc(atelier_crt_startup_block());

    {
        const auto scalar = std::make_unique<int>(42);
        const auto array = std::make_unique<std::uint64_t[]>(128);
        const auto aligned = std::make_unique<AlignedValue>();
        const auto aligned_array = std::make_unique<AlignedValue[]>(4);
        const auto nothrow = std::unique_ptr<AlignedValue>{new (std::nothrow) AlignedValue};
        std::vector<int> values(1024, 7);
        std::string text(1024, 'x');
        const std::array<const void*, 7> blocks{scalar.get(),  array.get(),   aligned.get(), aligned_array.get(),
                                                nothrow.get(), values.data(), text.data()};
        for (const void* block : blocks) {
            require_mimalloc(block);
        }
        for (const auto* block : {aligned.get(), aligned_array.get(), nothrow.get()}) {
            require(reinterpret_cast<std::uintptr_t>(block) % alignof(AlignedValue) == 0,
                    "Over-aligned allocation lost its alignment.");
        }
        require(*scalar == 42 && aligned->value == 42 && text.back() == 'x', "C++ allocation corrupted contents.");
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

    block = atelier_crt_allocate(512);
    require_mimalloc(block);
    std::free(block);

    void* thread_block{};
    {
        std::jthread worker{[&] { thread_block = std::malloc(512); }};
    }
    require_mimalloc(thread_block);
    std::free(thread_block);

    // Impossible size fails immediately; exercise C++ failure semantics without exhausting memory.
    {
        ExpectedAllocationFailure expected_failure;
        const auto impossible_size = std::numeric_limits<std::size_t>::max();
        block = ::operator new(impossible_size, std::nothrow);
        if (block) {
            ::operator delete(block);
            throw std::runtime_error("Impossible nothrow allocation unexpectedly succeeded.");
        }
        bool threw_bad_alloc = false;
        try {
            block = ::operator new(impossible_size);
            ::operator delete(block);
        } catch (const std::bad_alloc&) {
            threw_bad_alloc = true;
        }
        require(threw_bad_alloc, "Throwing operator new did not preserve std::bad_alloc semantics.");
    }
    std::cout << "[pass] mimalloc: CRT redirect, C++/STL/aligned allocations, startup/cross-DLL/thread lifetime\n";
}

}
