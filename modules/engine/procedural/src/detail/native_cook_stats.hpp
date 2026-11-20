#pragma once

#include <memory>
#include <utility>

namespace atelier::procedural {
struct BooleanStats;

namespace detail {
// The reflected method signature stays identical to a direct SOP call. A nested
// evaluator temporarily owns its own slot and restores the caller's on return.
struct NativeCookStats {
    using Slot = std::shared_ptr<const BooleanStats>;

    static inline thread_local Slot* current{};

    explicit NativeCookStats(Slot& slot) noexcept : previous(std::exchange(current, &slot)) {}

    ~NativeCookStats() { current = previous; }

    NativeCookStats(const NativeCookStats&) = delete;

    NativeCookStats& operator=(const NativeCookStats&) = delete;

private:
    Slot* previous;
};
}
}
