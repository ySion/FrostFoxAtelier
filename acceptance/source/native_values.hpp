#pragma once
#include "runtime/foundation.hpp"
#include "runtime/reflection.hpp"
#include "source/native_values.gen.hpp"

namespace atelier::acceptance::borrow {
#pragma pack(push, 1)
ASTRUCT(Name = "borrow.Packed")
struct Packed {
    AGENERATED_BODY(Packed)
    AFIELD()
    std::int32_t value{};
};
#pragma pack(pop)

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
    static inline int live{}, copies{}, moves{}, destroyed{}, copy_budget{-1};
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
        ++copies;
    }
    Particle(Particle&& value) noexcept : position(value.position), count(value.count), name(std::move(value.name)) {
        ++live;
        ++moves;
    }
    Particle& operator=(const Particle&) = default;
    ~Particle() {
        --live;
        ++destroyed;
    }
};
ACLASS(Name = "borrow.Algorithms")
struct Algorithms {
    AGENERATED_BODY(Algorithms)
    static inline std::uint32_t calls{}, failures{};
    AFUNCTION(BorrowSafe)
    std::uint32_t process(Array<Particle>& values, const Array<Particle>& input, const String& text) const {
        if (values.data() == input.data() || text.view() != "owned/火花/long-text-for-heap")
            throw std::runtime_error("Owned source values did not reach native code independently.");
        for (auto& value : values)
            value.count += 10;
        ++calls;
        return static_cast<std::uint32_t>(values.size());
    }
    AFUNCTION(BorrowSafe)
    std::uint32_t live_count() const noexcept { return static_cast<std::uint32_t>(Particle::live); }
    AFUNCTION(BorrowSafe)
    std::uint32_t text_size(const String& value) const noexcept { return static_cast<std::uint32_t>(value.size()); }
    AFUNCTION(BorrowSafe)
    void fail(Array<Particle>& values) {
        if (!values.empty())
            values[0].count = 999;
        ++failures;
        throw std::runtime_error("Native owned-value failure.");
    }
    AFUNCTION(BorrowSafe)
    void touch(Particle& value) { value.count += 4; }
    AFUNCTION(BorrowSafe)
    void bump(std::int32_t& value, const std::int32_t& alias) {
        value += 2;
        if (alias != value)
            throw std::runtime_error("Native leaf references lost their alias.");
    }
    AFUNCTION(BorrowSafe)
    std::uint32_t read_byte(const std::uint32_t& value) const noexcept { return value; }
    AFUNCTION()
    void unsafe() { ++calls; }
};

}
