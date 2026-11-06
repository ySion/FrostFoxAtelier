// Included once per executable or self-owned DLL; covers scalar, array, nothrow, sized and aligned forms.
// All overrides call the same shared mimalloc instance used by the CRT redirector.
#include "memory/fatal.hpp"

#include <mimalloc.h>
#include <new>

namespace {

void* require_memory(void* memory) noexcept {
    if (!memory)
        atelier::fatal("host CPU memory exhausted");
    return memory;
}

}

void* operator new(std::size_t size) {
    return require_memory(mi_malloc(size));
}

void* operator new[](std::size_t size) {
    return require_memory(mi_malloc(size));
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return require_memory(mi_malloc(size));
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return require_memory(mi_malloc(size));
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    return require_memory(mi_malloc_aligned(size, static_cast<std::size_t>(alignment)));
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return require_memory(mi_malloc_aligned(size, static_cast<std::size_t>(alignment)));
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return require_memory(mi_malloc_aligned(size, static_cast<std::size_t>(alignment)));
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return require_memory(mi_malloc_aligned(size, static_cast<std::size_t>(alignment)));
}

void operator delete(void* memory) noexcept {
    mi_free(memory);
}

void operator delete[](void* memory) noexcept {
    mi_free(memory);
}

void operator delete(void* memory, std::size_t size) noexcept {
    mi_free_size(memory, size);
}

void operator delete[](void* memory, std::size_t size) noexcept {
    mi_free_size(memory, size);
}

void operator delete(void* memory, const std::nothrow_t&) noexcept {
    mi_free(memory);
}

void operator delete[](void* memory, const std::nothrow_t&) noexcept {
    mi_free(memory);
}

void operator delete(void* memory, std::align_val_t alignment) noexcept {
    mi_free_aligned(memory, static_cast<std::size_t>(alignment));
}

void operator delete[](void* memory, std::align_val_t alignment) noexcept {
    mi_free_aligned(memory, static_cast<std::size_t>(alignment));
}

void operator delete(void* memory, std::size_t size, std::align_val_t alignment) noexcept {
    mi_free_size_aligned(memory, size, static_cast<std::size_t>(alignment));
}

void operator delete[](void* memory, std::size_t size, std::align_val_t alignment) noexcept {
    mi_free_size_aligned(memory, size, static_cast<std::size_t>(alignment));
}

void operator delete(void* memory, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    mi_free_aligned(memory, static_cast<std::size_t>(alignment));
}

void operator delete[](void* memory, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    mi_free_aligned(memory, static_cast<std::size_t>(alignment));
}
