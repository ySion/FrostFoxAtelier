#pragma once

#include <cstddef>

#ifdef ATELIER_ALLOCATOR_CLIENT_BUILD
#define ATELIER_CRT_CLIENT __declspec(dllexport)
#else
#define ATELIER_CRT_CLIENT __declspec(dllimport)
#endif

extern "C" {
ATELIER_CRT_CLIENT void* atelier_crt_allocate(std::size_t size) noexcept;

ATELIER_CRT_CLIENT void* atelier_crt_resize(void* block, std::size_t size) noexcept;

ATELIER_CRT_CLIENT void atelier_crt_free(void* block) noexcept;

ATELIER_CRT_CLIENT const void* atelier_crt_startup_block() noexcept;
}

#undef ATELIER_CRT_CLIENT
