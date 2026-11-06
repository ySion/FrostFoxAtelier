#pragma once

#include "../helpers.hpp"
#include "runtime/native.hpp"
#include <stdexcept>

namespace atelier::acceptance::inheritance {
using namespace atelier::acceptance::helpers;

inline void collect(Runtime& runtime) {
    if (runtime.collection_phase() == CollectionPhase::idle)
        take(runtime.begin_collection());
    while (runtime.collection_phase() != CollectionPhase::idle)
        take(runtime.collect_step({1}));
    take(runtime.begin_collection());
    while (runtime.collection_phase() != CollectionPhase::idle)
        take(runtime.collect_step({1}));
}

struct Base {
    inline static int live{};

    inline static int destroyed{};

    float value{7};

    Base() { ++live; }

    ~Base() noexcept {
        --live;
        ++destroyed;
    }

    float bump(float amount) {
        value += amount;
        return value;
    }
};

// Unregistered C++ storage forces an adjusted address for the one Runtime base.
struct Prefix {
    virtual ~Prefix() = default;

    std::uint64_t sentinel{0x12345678};
};

struct Derived : Prefix, Base {
    inline static int live{};

    Derived() { ++live; }

    ~Derived() noexcept override { --live; }
};

struct Resource {
    inline static int live{};

    inline static int destroyed{};

    Resource() { ++live; }

    ~Resource() noexcept {
        --live;
        ++destroyed;
    }
};

struct Failing {
    inline static bool fail{};
};
}
