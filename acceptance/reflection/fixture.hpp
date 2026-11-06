#pragma once
#include "runtime/reflection.hpp"
#include "math/math.hpp"
#include "reflection/fixture.gen.hpp"
#include <stdexcept>

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

AENUM(Name = "reflection.Mode")
enum class Mode : std::int32_t { idle = -1, active = 2 };

ASTRUCT(Name = "reflection.PlainRecord")
struct alignas(64) PlainRecord {
    AGENERATED_BODY(PlainRecord)

    AFIELD()
    bool enabled;

    AFIELD()
    Mode mode;

    AFIELD()
    math::Matrix4d transform;

    AFIELD()
    std::uint32_t count;
};

ACLASS(Name = "reflection.CallProbe")
struct CallProbe {
    AGENERATED_BODY(CallProbe)

    AFUNCTION(BorrowSafe)
    Result<PlainRecord> record_alias(PlainRecord copy, PlainRecord& target) const noexcept {
        target.count += 10;
        copy.enabled = !copy.enabled;
        return copy;
    }

    AFIELD()
    int value{};

    AFUNCTION()
    Result<bool> bool_alias(bool copy, const bool& read, bool& write) noexcept {
        write = copy;
        return read;
    }

    AFUNCTION()
    Result<bool> fail_bool(bool& value) noexcept {
        value = true;
        return std::unexpected(Error{ErrorCode::invocation_failed, "generated bool failure"});
    }

    AFUNCTION()
    int aliases(const int& read, int& write) {
        write = 10;
        write += read;
        return read;
    }

    AFUNCTION()
    int mixed(int copy, const int& read, int& write) {
        write += copy + read;
        return write;
    }

    AFUNCTION()
    int mutate() {
        value = 7;
        return 0;
    }
};
}
