#pragma once
#include "runtime/reflection.hpp"
#include "reflection/fixture.gen.hpp"

namespace atelier::acceptance::reflection {
ASTRUCT(Name = "reflection.Sample")
struct Sample {
    AGENERATED_BODY(Sample)

    AFIELD(Meta("test.note", "分组\0尾"), Meta("test.binding", "evaluate.values"))
    float value{3};
};

ACLASS(Name = "reflection.Counter")
class Counter {
    AGENERATED_BODY(Counter)

public:
    inline static int live{};

    Counter() { ++live; }

    ~Counter() noexcept { --live; }

private:
    AFIELD(Hidden)
    Sample sample_;
};
}
