#pragma once

#include "native_client.hpp"

namespace atelier::jit_acceptance {

struct ProbeCallbacks {
    std::uint64_t (*host_bias)(std::uint64_t) noexcept;

    decltype(&atelier_jit_native_calculate) native_calculate;

    decltype(&atelier_jit_native_capture) native_capture;

    void (*log)(const char*) noexcept;
};

}

#ifdef ATELIER_JIT_PROBE_BUILD
#define ATELIER_JIT_PROBE __declspec(dllexport)
#else
#define ATELIER_JIT_PROBE __declspec(dllimport)
#endif

// The SDK's C++ objects and allocator ownership stay inside this DLL.
// Callbacks remain valid until this call returns; acceptance failure is fatal.
extern "C" ATELIER_JIT_PROBE void
atelier_run_jit_acceptance(const atelier::jit_acceptance::ProbeCallbacks* callbacks) noexcept;

#undef ATELIER_JIT_PROBE
