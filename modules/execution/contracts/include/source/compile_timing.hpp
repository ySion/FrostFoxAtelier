#pragma once
#include <chrono>
#include <cstdint>

namespace atelier::source {
// Single-entry compilation, in nanoseconds. IR includes the optimization pipeline;
// the first lookup includes object emission. Only values cross the backend ABI.
struct CompileTiming {
    std::uint64_t frontend_ns{};
    std::uint64_t native_compile_ns{};
    std::uint64_t ir_ns{};
    std::uint64_t optimize_ns{};
    std::uint64_t first_lookup_ns{};
    std::uint64_t object_ns{};
};

namespace detail {
class CompileTimer {
    using Clock = std::chrono::steady_clock;
    Clock::time_point start_;

public:
    explicit CompileTimer(bool enabled) : start_(enabled ? Clock::now() : Clock::time_point{}) {}

    std::uint64_t elapsed_ns() const {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start_).count());
    }
};
}
}
