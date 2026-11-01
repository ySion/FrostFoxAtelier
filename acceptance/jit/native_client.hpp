#pragma once

#include <cstdint>

namespace atelier::jit_acceptance {

struct alignas(32) ProbeRecord {
    double total{};

    std::uint64_t checksum{};

    float amount{};

    std::uint32_t count{};
};

struct ProbeDiagnostic {
    std::uint32_t cleanups{};

    char message[128]{};
};

struct StackSnapshot {
    void* frames[64]{};

    std::uint64_t storage_address{};

    std::uint32_t count{};
};

inline constexpr std::uint32_t native_success = 0;

inline constexpr std::uint32_t native_failure = 1;

inline constexpr std::uint32_t invalid_storage = 2;

}

#ifdef ATELIER_JIT_NATIVE_CLIENT_BUILD
#define ATELIER_JIT_NATIVE_CLIENT __declspec(dllexport)
#else
#define ATELIER_JIT_NATIVE_CLIENT __declspec(dllimport)
#endif

// Only fixed-width scalars and caller-owned storage cross this DLL/JIT seam.
// No C++ exception or allocation ownership crosses it.
extern "C" {
ATELIER_JIT_NATIVE_CLIENT std::uint32_t
atelier_jit_native_calculate(std::uint32_t fail, std::int32_t value, float fraction, double scale, std::uint64_t offset,
                             const atelier::jit_acceptance::ProbeRecord* input,
                             atelier::jit_acceptance::ProbeRecord* output,
                             atelier::jit_acceptance::ProbeDiagnostic* diagnostic) noexcept;

ATELIER_JIT_NATIVE_CLIENT std::uint32_t
atelier_jit_native_capture(atelier::jit_acceptance::StackSnapshot* snapshot) noexcept;
}

#undef ATELIER_JIT_NATIVE_CLIENT
