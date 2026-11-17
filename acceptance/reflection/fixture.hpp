#pragma once
#include "reflection/upstream.hpp"
#include "runtime/reflection.hpp"
#include "reflection/fixture.gen.hpp"

namespace atelier::acceptance::reflection {
AENUM(Name = "reflection.LayerMask", Flags)
enum class LayerMask : std::uint32_t {
    AMETA(Label = "无层", DESC = "不命中任何层") none = 0u,

    AMETA(Label = "静态层", DESC = "地形与建筑") static_layer = 1u << 0,

    AMETA(Label = "动态层") dynamic_layer = 1u << 1,
};

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

ACLASS(Name = "reflection.DerivedCounter", Validate = "validate")
class DerivedCounter : public Counter {
    AGENERATED_BODY(DerivedCounter)

public:
    AFIELD()
    float step{1.0f};

    AFIELD()
    upstream::Mode mode{upstream::Mode::ready};

    AFIELD()
    upstream::Payload payload{};

    AFIELD(Transient, ReadOnly)
    float telemetry{7.0f};

    AFUNCTION(BorrowSafe)
    static Result<float> advance(Runtime& runtime, ObjectId self, DerivedCounter& counter, upstream::Mode next,
                                 const upstream::Payload& input, float multiplier = 1.0f,
                                 LayerMask mask = LayerMask::static_layer) {
        if (!runtime.type_of(self))
            return std::unexpected(Error{ErrorCode::invalid_object, "Missing self."});
        counter.mode = next;
        if (static_cast<std::uint32_t>(mask) != 0u)
            counter.telemetry += counter.step * input.scale * multiplier;
        return counter.telemetry;
    }

private:
    [[nodiscard]] Result<void> validate() const {
        if (step <= 0.0f)
            return std::unexpected(Error{ErrorCode::argument_mismatch, "step must be positive."});
        return {};
    }
};
}
