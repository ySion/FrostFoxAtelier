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

    Particle(const Particle& value) noexcept : position(value.position), count(value.count), name(value.name) {
        ++live;
    }

    static Result<void> accept_copy() noexcept {
        if (copy_budget == 0)
            return std::unexpected(Error{ErrorCode::construction_failed, "Injected native value copy failure."});
        if (copy_budget > 0)
            --copy_budget;
        return {};
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
    Result<void> fail(Array<Particle>& values) noexcept {
        if (!values.empty())
            values[0].count = 999;
        ++failures;
        return std::unexpected(Error{ErrorCode::invocation_failed, "Native owned-value failure."});
    }
};

inline TypeDefinition particle_definition(Runtime& runtime) {
    auto definition = reflection::Definition<Particle>::build(runtime);
    definition.native.value_operations_preserve_borrows = true;
    definition.native.copy_construct = [](void* destination, const void* source) noexcept -> Result<void> {
        ATELIER_TRY(Particle::accept_copy());
        std::construct_at(static_cast<Particle*>(destination), *static_cast<const Particle*>(source));
        return {};
    };
    return definition;
}

inline Result<Array<Particle>> copy_particles(const Array<Particle>& source) noexcept {
    Array<Particle> prepared;
    prepared.reserve(source.size());
    for (const auto& element : source) {
        ATELIER_TRY(Particle::accept_copy());
        prepared.add(element);
    }
    return prepared;
}

inline TypeDefinition particle_array_definition(Runtime& runtime, std::string name) {
    auto definition = array_definition<Particle>(runtime, std::move(name));
    definition.native.copy_construct = [](void* destination, const void* source) noexcept -> Result<void> {
        ATELIER_TRY_BIND(prepared, copy_particles(*static_cast<const Array<Particle>*>(source)));
        std::construct_at(static_cast<Array<Particle>*>(destination), std::move(*prepared));
        return {};
    };
    definition.native.copy_assign = [](void* destination, const void* source) noexcept -> Result<void> {
        ATELIER_TRY_BIND(prepared, copy_particles(*static_cast<const Array<Particle>*>(source)));
        static_cast<Array<Particle>*>(destination)->swap(*prepared);
        return {};
    };
    return definition;
}

}
