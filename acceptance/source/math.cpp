#include "../helpers.hpp"
#include "math_reference.hpp"
#include "runtime/foundation.hpp"
#include "source/program.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>

namespace {
using namespace atelier;

using source::Program;

using math::Vector3;

using math::Vector3d;

using namespace atelier::acceptance::helpers;

void publish(Program& program, std::string_view text, std::string_view entry = "run") {
    auto candidate = take(Program::prepare(take(program.request("math.source", text, entry))));
    take(program.try_publish(candidate));
}

template <class T, std::size_t N>
T evaluate(Runtime& runtime, const Program::Entry& entry, const std::array<Argument, N>& arguments) {
    T result{};
    take(entry.invoke({}, arguments, ValueOutput{take(runtime.native_type<T>()), &result}));
    return result;
}

struct NativeMath {
    Vector3 position{1.0f, 2.0f, 3.0f};

    Vector3 value(Vector3 input) const { return {input.x + 1.0f, input.y + 2.0f, input.z + 3.0f}; }

    Vector3 constant(const Vector3& input) const { return input; }

    void mutate(Vector3& input) const {
        input.x += 4.0f;
        input.y += 5.0f;
        input.z += 6.0f;
    }

    Vector3 fail(Vector3) const { throw std::runtime_error("Native math failure."); }
};

class GuardedPages {
public:
    GuardedPages() {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        page_size_ = info.dwPageSize;
        data_ =
            static_cast<std::byte*>(VirtualAlloc(nullptr, page_size_ * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!data_)
            throw std::runtime_error("Math guard page allocation failed.");
        DWORD previous{};
        if (!VirtualProtect(data_ + page_size_, page_size_, PAGE_NOACCESS, &previous)) {
            VirtualFree(data_, 0, MEM_RELEASE);
            throw std::runtime_error("Math guard page protection failed.");
        }
    }

    ~GuardedPages() { VirtualFree(data_, 0, MEM_RELEASE); }

    GuardedPages(const GuardedPages&) = delete;

    GuardedPages& operator=(const GuardedPages&) = delete;

    Vector3* construct(Vector3 value) {
        return std::construct_at(reinterpret_cast<Vector3*>(data_ + page_size_ - sizeof(Vector3)), value);
    }

private:
    std::byte* data_{};

    std::size_t page_size_{};
};

void verify_elementwise() {
    struct Case {
        const char* call;

        float x;

        float y;

        float z;

        float expected;
    };
    // One independent truth per operation, not a scalar/vector/precision matrix.
    const std::array cases{
        Case{"math.sqrt(x)", 2.25f, 0, 0, 1.5f},
        Case{"math.floor(x)", -2.25f, 0, 0, -3},
        Case{"math.ceil(x)", -2.25f, 0, 0, -2},
        Case{"math.trunc(x)", -2.25f, 0, 0, -2},
        Case{"math.fract(x)", -1e-30f, 0, 0, std::nextafter(1.0f, 0.0f)},
        Case{"math.abs(x)", -2.25f, 0, 0, 2.25f},
        Case{"math.sign(x)", -3, 0, 0, -1},
        Case{"math.min(x, y)", 1, std::numeric_limits<float>::quiet_NaN(), 0, 1},
        Case{"math.max(x, y)", 1, std::numeric_limits<float>::quiet_NaN(), 0, 1},
        Case{"math.step(x, y)", 0.5f, 0.25f, 0, 0},
        Case{"math.clamp(x, y, z)", 5, 2, 1, 2},
        Case{"math.lerp(x, y, z)", 1, 1e-8f, 1, 1e-8f},
        Case{"math.smoothstep(x, y, z)", 0, 1, 0.25f, 0.15625f},
    };
    Runtime runtime;
    for (const auto& item : cases) {
        Program program(runtime, {});
        publish(program, std::string("float run(float x, float y, float z) { return ") + item.call + "; }");
        const auto result = std::get<float>(
            *take(program.entry().invoke({}, std::array{Scalar{item.x}, Scalar{item.y}, Scalar{item.z}})));
        require(result == item.expected, item.call);
    }
}

void verify_transcendental() {
    // f32 uses an independent double std oracle; f64 uses correctly rounded CORE-MATH.
    const std::array first{0x1.921fb54442d18p+10, 0x1.921fb54442d18p+10, 0.75, 1.25, -0.75, 1.25};
    constexpr std::array second{0.0, 0.0, 0.0, 0.0, 1.25, -0.75};
    const std::array<double (*)(double, double), 6> reference{
        [](double x, double) { return std::sin(x); },        [](double x, double) { return std::cos(x); },
        [](double x, double) { return std::exp(x); },        [](double x, double) { return std::log(x); },
        [](double x, double y) { return std::atan2(x, y); }, [](double x, double y) { return std::pow(x, y); },
    };
    Runtime runtime;
    take(runtime.register_types({array_definition<float>(runtime, "acceptance.TranscendentalFloats"),
                                 array_definition<double>(runtime, "acceptance.TranscendentalDoubles")}));
    for (std::size_t i = 0; i < std::size(math_reference::functions); ++i) {
        const auto& function = math_reference::functions[i];
        const auto expression = "math." + std::string(function.name) + (function.binary ? "(a[i], b[i])" : "(a[i])");
        Program narrow(runtime, {});
        Program wide(runtime, {});
        publish(narrow,
                "void run(Array<float>& a, const Array<float>& b) { uint i = 0; while (i < a.size()) { a[i] = " +
                    expression + "; i = i + 1; } }");
        publish(wide,
                "void run(Array<double>& a, const Array<double>& b) { uint i = 0; while (i < a.size()) { a[i] = " +
                    expression + "; i = i + 1; } }");
        const float x = i < 2 ? std::bit_cast<float>(0x437ce5f1u) : static_cast<float>(first[i]);
        Array<float> f32{x};
        const Array<float> f32_second{static_cast<float>(second[i])};
        take(narrow.entry().invoke({}, std::array{Argument::borrow(f32), Argument::borrow(f32_second)}));
        const double expected = reference[i](x, second[i]);
        const double ulp = std::ldexp(1.0, std::ilogb(static_cast<float>(expected)) - 23);
        require(std::isfinite(f32[0]) && std::abs(double(f32[0]) - expected) <= 4 * ulp,
                "Float transcendental differs from its independent double oracle by more than 4 ULP.");
        Array<double> f64{first[i]};
        const Array<double> f64_second{second[i]};
        take(wide.entry().invoke({}, std::array{Argument::borrow(f64), Argument::borrow(f64_second)}));
        require(math_reference::ulp_distance(f64[0], function.value(first[i], second[i])) <= 3,
                "Double transcendental differs from CORE-MATH by more than 3 ULP.");
    }
}
}

void verify_source_math() {
    Runtime runtime;
    take(runtime.register_types({array_definition<Vector3d>(runtime, "acceptance.MathVectorsd"),
                                 NativeRegistration<NativeMath>{runtime, "acceptance.NativeMath", TypeKind::object}
                                     .field<&NativeMath::position>("position")
                                     .method<&NativeMath::value>("value", true)
                                     .method<&NativeMath::constant>("constant", true)
                                     .method<&NativeMath::mutate>("mutate", true)
                                     .method<&NativeMath::fail>("fail", true)
                                     .build()}));
    Program values(runtime, {});
    publish(values, R"(
        vector3 helper(vector3 value) { return math.cross(value, vector3(0.0, 0.0, 1.0)); }
        math.vector3 run(vector3 value) {
            vector3 result = helper(value) + vector3(1.0, 2.0, 3.0);
            return result * 2.0 / 2.0;
        }
    )");
    struct CompactTail {
        Vector3 value;

        std::uint32_t canary;
    } compact{{2.0f, 3.0f, 4.0f}, 0xfeedbeef};
    const auto result = evaluate<Vector3>(runtime, values.entry(), std::array{Argument::borrow(compact.value)});
    require(result == Vector3{4, 0, 3} && compact.canary == 0xfeedbeef,
            "Source math helper value return or compact storage failed.");
    Program compact_float(runtime, {});
    publish(compact_float, "void run(vector3& value) { value = value * 2.0; }");
    {
        GuardedPages pages;
        auto* guarded = pages.construct({1, 2, 3});
        take(compact_float.entry().invoke({}, std::array{Argument::borrow(*guarded)}));
        require(*guarded == Vector3{2, 4, 6}, "Compact boundary math changed values.");
        std::destroy_at(guarded);
    }
    Program doubles(runtime, {});
    publish(doubles, R"(
        vector3d helper(vector3d value) { return value + vector3d(1.0d, 2.0d, 3.0d); }
        double run(Array<vector3d>& values) {
            uint index = 0;
            while (index < values.size()) { values[index] = helper(values[index]); index = index + 1; }
            return values[0].x + double(1.0);
        }
    )");
    Array<Vector3d> wide;
    wide.add({16777216.0, 0, 0});
    require(std::get<double>(*take(doubles.entry().invoke({}, std::array{Argument::borrow(wide)}))) == 16777218.0 &&
                wide[0].x == 16777217.0,
            "Double source math narrowed through float.");
    Program normalized(runtime, {});
    publish(normalized, "vector3 run(vector3 value) { return math.normalize_or_zero(value); }");
    const float scale = std::numeric_limits<float>::max();
    Vector3 input{scale, scale, scale};
    const auto unit = evaluate<Vector3>(runtime, normalized.entry(), std::array{Argument::borrow(input)});
    for (std::size_t i = 0; i < 3; ++i)
        require(std::abs(unit[i] - 0.577350269f) <= 0.000001f, "Source normalization overflowed finite inputs.");
    Program transforms(runtime, {});
    publish(transforms, R"(
        vector3 run(vector3 value, matrix4 first, matrix4 second) {
            quaternion turn = quaternion(0.0, 0.0, 1.0, 0.0);
            quaternion identity = quaternion();
            vector3 rotated = math.rotate(turn * identity, value);
            return math.transform_point(rotated, first * second * matrix4(1.0));
        }
    )");
    Vector3 point{1, 2, 3};
    auto first = math::Matrix4::identity();
    first(3, 0) = 5;
    first(3, 1) = 7;
    auto second = math::Matrix4::identity();
    second(0, 0) = 2;
    second(1, 1) = 3;
    const auto transformed =
        evaluate<Vector3>(runtime, transforms.entry(),
                          std::array{Argument::borrow(point), Argument::borrow(first), Argument::borrow(second)});
    require(transformed == Vector3{8, 15, 3}, "Quaternion or row-vector matrix composition changed conventions.");
    const auto native_type = take(runtime.native_type<NativeMath>());
    auto host = take(runtime.create(native_type));
    Program native(runtime, native_type);
    publish(native, R"(
        vector3 run(vector3 value) {
            vector3 changed = self.value(value);
            self.mutate(changed);
            return self.constant(changed);
        }
    )");
    Vector3 output{};
    const ValueOutput destination{take(runtime.native_type<Vector3>()), &output};
    take(native.entry().invoke(host.id(), std::array{Argument::borrow(point)}, destination));
    require(output == Vector3{6, 9, 12} && point.x == 1, "Native math value/reference interop failed.");
    Program fields(runtime, native_type);
    publish(fields, R"(
        vector3 run(vector3 step) {
            vector3 previous = self.position;
            self.position = previous + step;
            return self.position;
        }
    )");
    take(fields.entry().invoke(host.id(), std::array{Argument::borrow(point)}, destination));
    require(output == Vector3{2, 4, 6}, "Whole math field read/write lost native state.");
    publish(native, "vector3 run(vector3 value) { return self.fail(value); }");
    output = {91, 92, 93};
    require(!native.entry().invoke(host.id(), std::array{Argument::borrow(point)}, destination) &&
                output == Vector3{91, 92, 93},
            "Native failure changed caller output.");
    Program asynchronous(runtime, {});
    publish(asynchronous, R"(
        async double run(double value) {
            vector3d local = vector3d(value, 2.0d, 3.0d);
            yield;
            local = local + vector3d(1.0d, 4.0d, 5.0d);
            return local.x + local.y - 6.0d;
        }
    )");
    auto coroutine = take(asynchronous.entry().start({}, std::array{Scalar{16777216.0}}));
    require(take(coroutine.resume()) == CoroutineState::suspended &&
                take(coroutine.resume()) == CoroutineState::completed &&
                std::get<double>(*take(coroutine.result())) == 16777217.0,
            "Async f64 parameter or result narrowed precision.");
    verify_elementwise();
    verify_transcendental();
    std::puts("Source math: PASS (independent math truths, compact ABI, native failure and async f64)");
}
