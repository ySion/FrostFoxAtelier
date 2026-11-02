#pragma once
#include "runtime/foundation.hpp"
#include "runtime/reflection.hpp"
#include "source/native_values.gen.hpp"

namespace atelier::acceptance::borrow {
ASTRUCT(Name = "borrow.Position")
struct Position {
    AGENERATED_BODY(Position)

    AFIELD()
    float x{};

    AFIELD()
    float y{};

    AFIELD()
    float z{};
};

ASTRUCT(Name = "borrow.Particle")
struct alignas(64) Particle {
    AGENERATED_BODY(Particle)

    static inline int live{};

    static inline int copy_budget{-1};

    AFIELD()
    Position position;

    AFIELD()
    std::int32_t count{};

    AFIELD()
    String name;

    Particle() { ++live; }

    Particle(const Particle& value) : position(value.position), count(value.count), name(value.name) {
        if (copy_budget == 0)
            throw std::runtime_error("Injected native value copy failure.");
        if (copy_budget > 0)
            --copy_budget;
        ++live;
    }

    Particle(Particle&& value) noexcept : position(value.position), count(value.count), name(std::move(value.name)) {
        ++live;
    }

    Particle& operator=(const Particle&) = default;

    ~Particle() { --live; }
};

ACLASS(Name = "borrow.Algorithms")
struct Algorithms {
    AGENERATED_BODY(Algorithms)

    static inline std::uint32_t failures{};

    AFUNCTION(BorrowSafe)
    void fail(Array<Particle>& values) {
        if (!values.empty())
            values[0].count = 999;
        ++failures;
        throw std::runtime_error("Native owned-value failure.");
    }
};

}
