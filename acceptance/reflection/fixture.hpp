#pragma once
#include "runtime/reflection.hpp"
#include "reflection/fixture.gen.hpp"

namespace atelier::acceptance::reflection {
ASTRUCT(Name = "reflection.Sample")
struct Sample {
    AGENERATED_BODY(Sample)
    AFIELD() float value{3};
    AFIELD() std::uint32_t count{};
};

ACLASS(Name = "reflection.Counter")
class Counter {
    AGENERATED_BODY(Counter)
public:
    inline static int live{};
    Counter() { ++live; }
    ~Counter() noexcept { --live; }
    AFUNCTION() float read() const noexcept { return sample_.value; }
    AFUNCTION(Name = "count") std::uint32_t advance(const std::uint32_t amount) noexcept { return sample_.count += amount; }
    AFUNCTION(Name = "scale") float advance(float scale) { return sample_.value *= scale; }
#if defined(_DEBUG)
    AFIELD() float debug_only{};
#else
    AFIELD() std::uint32_t release_only{};
#endif
private:
    AFIELD(Hidden) Sample sample_;
};
}
