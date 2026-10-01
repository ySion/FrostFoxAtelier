#pragma once

#include "jit/source_backend.hpp"

#ifdef ATELIER_SOURCE_BACKEND_BUILD
#define ATELIER_SOURCE_VERIFICATION __declspec(dllexport)
#else
#define ATELIER_SOURCE_VERIFICATION __declspec(dllimport)
#endif

// Available only when ATELIER_BUILD_JIT_ACCEPTANCE is enabled. These entry
// points run inside the same DLL that owns all source-backend code handles.
// They exercise the real source compiler/linker with business_poll omitted;
// failed compilation leaves *output, including an existing handle, untouched.
extern "C" ATELIER_SOURCE_VERIFICATION std::uint32_t atelier_compile_business_without_poll_for_verification(
    const atelier::source::detail::BusinessServices* services,
    const char* source_name, const char* source, std::uint32_t source_size,
    const atelier::source::detail::BusinessEnvironment* environment,
    atelier::source::detail::BusinessCode** output,
    char* error, std::uint32_t error_capacity, const char* entry_name = nullptr) noexcept;

extern "C" ATELIER_SOURCE_VERIFICATION std::uint32_t atelier_verify_business_link_failure(
    const atelier::source::detail::BusinessServices* services,
    char* error, std::uint32_t error_capacity) noexcept;

#undef ATELIER_SOURCE_VERIFICATION
