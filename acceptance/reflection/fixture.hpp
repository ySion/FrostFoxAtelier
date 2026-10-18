#pragma once
#include "runtime/reflection.hpp"
#include "foundation/array.hpp"
#include "math/math.hpp"
#include "reflection/fixture.gen.hpp"
#include <stdexcept>

namespace atelier::acceptance::reflection {
ASTRUCT(Name = "reflection.Sample", DESC = "Sample 记录。\r\n包含测量值和次数。\r普通 UTF-8。")
struct Sample {
    AGENERATED_BODY(Sample)
    AFIELD(Label = "数值 \"value\" \\ preview", Category = "Transform", DESC = "Value 测量值。\n允许 \"引号\"、\\ 路径和\t缩进。") float value{3};
    AFIELD(DESC = "Count 累计次数。") std::uint32_t count{};
};

ACLASS(Name = "reflection.Counter", DESC = "Counter 累计测量次数。")
class Counter {
    AGENERATED_BODY(Counter)
public:
    inline static int live{};
    Counter() { ++live; }
    ~Counter() noexcept { --live; }
    AFUNCTION(DESC = "读取当前 Value。\r\n不会改变计数。") float read() const noexcept { return sample_.value; }
    AFUNCTION(Name = "count") std::uint32_t advance(const std::uint32_t amount) noexcept { return sample_.count += amount; }
    AFUNCTION(Name = "scale", DESC = "") float advance(float scale) { return sample_.value *= scale; }
#if defined(_DEBUG)
    AFIELD(Label = "Debug value", DESC = "Debug 字段。") float debug_only{};
#else
    AFIELD(Label = "Release value", DESC = "Release 字段。") std::uint32_t release_only{};
#endif
private:
    AFIELD(Hidden, Label = "Internal sample", Category = "Internal", DESC = "仅供内部使用。") Sample sample_;
};

ACLASS(Name = "reflection.CounterChild")
class CounterChild : public Counter {
    AGENERATED_BODY(CounterChild)
};

ACLASS(Name = "reflection.RangeProbe")
struct RangeProbe {
    AGENERATED_BODY(RangeProbe)
    AFIELD(Min = "0", Max = "10", LockMin, HardMax = "65504") float emissive{1.0f};
    AFIELD(Min = "0.1", Max = "1", LockMax) math::Vector3d tint{0.5, 0.5, 0.5};
    AFIELD(HardMin = "-5", HardMax = "5") int offset{};
    AFIELD(Min = "1", Max = "64") std::uint32_t samples{8};
    AFIELD(Max = "0.5", HardMin = "0", HardMax = "1") Array<double> weights;
};

// Targets outside the generated group resolve by name when the Runtime publishes the batch.
ASTRUCT(Name = "reflection.ParentArgs")
struct ParentArgs {
    AGENERATED_BODY(ParentArgs)
    AFIELD(Weak = "reflection.External") ReferenceSlot parent;
};

AENUM(Name = "reflection.Mode", DESC = "Mode 工作状态。\nidle 表示空闲。")
enum class Mode : std::int32_t { idle = -1, active = 2, alias = active };
AENUM(Name = "reflection.OtherMode")
enum class OtherMode : std::int32_t { active = 2 };
AENUM(Name = "reflection.UnsignedMode")
enum class UnsignedMode : std::uint32_t { high = 0xffffffffu };

ASTRUCT(Name = "reflection.PlainRecord")
struct alignas(64) PlainRecord {
    AGENERATED_BODY(PlainRecord)
    AFIELD() bool enabled;
    AFIELD() Mode mode;
    AFIELD() math::Matrix4d transform;
    AFIELD() std::uint32_t count;
};
ACLASS(Name = "reflection.CallProbe")
struct CallProbe {
    AGENERATED_BODY(CallProbe)
    AFUNCTION(BorrowSafe, DESC = "更新 Record 的次数，并返回副本。") PlainRecord record_alias(PlainRecord copy, PlainRecord& target) const {
        target.count += 10; copy.enabled = !copy.enabled; return copy;
    }
    AFIELD() int value{};
    AFIELD() Mode mode{Mode::idle};
    AFIELD() Sample sample;
    AFIELD() std::uint32_t bits{};
    AFIELD() double precise_value{};
    AFIELD(Label = "Position", Category = "Transform") math::Vector3d direction;
    AFIELD() Array<double> samples;
    AFIELD(Category = "Flags") bool enabled{};
    AFIELD(Label = "", Category = "", DESC = "") bool before{true};
    AFIELD() bool after{true};
    AFIELD() Array<bool> flags;
    AFUNCTION() bool bool_alias(bool copy, const bool& read, bool& write) { write = copy; return read; }
    AFUNCTION() bool append_flag(Array<bool>& values, bool value) { values.add(value); return values[values.size() - 1]; }
    AFUNCTION() bool fail_bool(bool& value) { value = true; throw std::runtime_error("generated bool failure"); }
    AFUNCTION() int aliases(const int& read, int& write) { write = 10; write += read; return read; }
    AFUNCTION() int mixed(int copy, const int& read, int& write) { write += copy + read; return write; }
    AFUNCTION() int read_only(const int& v) const noexcept { return v; }
    AFUNCTION() int mutate() { value = 7; return 0; }
    AFUNCTION() int observe_after(const int& read, int) { return read; }
    AFUNCTION() int self_alias(int& v) { v = -19; return value; }
    AFUNCTION() void float_alias(const float& read, float& write) { write = 3.f; write += read; }
    AFUNCTION() void uint_alias(const std::uint32_t& read, std::uint32_t& write) { write = 5; write += read; }
    AFUNCTION() Mode enum_alias(const Mode& read, Mode& write) { write = Mode::active; return read; }
    AFUNCTION() Mode echo(Mode v) const { return v; }
    AFUNCTION() UnsignedMode unsigned_echo(UnsignedMode v) const { return v; }
    AFUNCTION() int fail(int& v) { v = -7; throw std::runtime_error("borrow failure"); }
    AFUNCTION() int wide(int& a, const int& b, int& c, const int& d, int& e, const int& f, int& g, const int& h, int& i) {
        a = 1; c += b; e += d; g += f; i += h; return i;
    }
    AFUNCTION() double precise_echo(double input) const noexcept { return input; }
    AFUNCTION() double precise(double copy, const double& read, double& write) {
        write += copy + read;
        return write;
    }
    AFUNCTION() math::Vector3d translate(math::Vector3d copy, const math::Vector3d& read, math::Vector3d& write) {
        write += copy + read;
        return write;
    }
    AFUNCTION() double append_sample(Array<double>& values, const double& input) {
        values.add(input);
        return values[values.size() - 1];
    }
};
}
