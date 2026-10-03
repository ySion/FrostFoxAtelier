#pragma once
#include "runtime/reflection.hpp"
#include "foundation/array.hpp"
#include "math/math.hpp"
#include "reflection/fixture.gen.hpp"
#include <stdexcept>

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

AENUM(Name = "reflection.Mode")
enum class Mode : std::int32_t { idle = -1, active = 2, alias = active };
AENUM(Name = "reflection.OtherMode")
enum class OtherMode : std::int32_t { active = 2 };
AENUM(Name = "reflection.UnsignedMode")
enum class UnsignedMode : std::uint32_t { high = 0xffffffffu };

ACLASS(Name = "reflection.CallProbe")
struct CallProbe {
    AGENERATED_BODY(CallProbe)
    AFIELD() int value{};
    AFIELD() Mode mode{Mode::idle};
    AFIELD() Sample sample;
    AFIELD() std::uint32_t bits{};
    AFIELD() double precise_value{};
    AFIELD() math::Vector3d direction;
    AFIELD() Array<double> samples;
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
