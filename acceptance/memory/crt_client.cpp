#include "crt_client.hpp"

#include <cstdlib>

// This DLL deliberately neither links mimalloc nor includes its headers.
namespace {
struct StartupAllocation {
    void* block{std::malloc(256)};
    ~StartupAllocation() { std::free(block); }
};
StartupAllocation startup;
}

void* atelier_crt_allocate(std::size_t size) noexcept { return std::malloc(size); }
void* atelier_crt_resize(void* block, std::size_t size) noexcept { return std::realloc(block, size); }
void atelier_crt_free(void* block) noexcept { std::free(block); }
const void* atelier_crt_startup_block() noexcept { return startup.block; }
