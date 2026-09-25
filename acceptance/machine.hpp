#pragma once

#include <cstdint>

namespace atelier::acceptance {

struct alignas(32) NativeState {
    static inline int live{};
    float heat{};
    std::uint32_t cycles{};
    NativeState() { ++live; }
    ~NativeState() noexcept { --live; }
};

struct NativeMachine {
    static inline int created{};
    static inline int destroyed{};
    float progress{};
    NativeState state;
    NativeMachine() { ++created; }
    ~NativeMachine() noexcept { ++destroyed; }
    void advance(float amount) noexcept { progress += amount; }
};

}
