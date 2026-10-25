#include "math/math.hpp"
#include "runtime/foundation.hpp"
#include "runtime/native.hpp"
#include "source/program.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <random>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace atelier;
using source::Program;
using math::Vector3;
using math::Vector3d;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T take(Result<T> value) {
    if (!value) throw std::runtime_error(value.error().message);
    return std::move(*value);
}
void take(Result<void> value) { if (!value) throw std::runtime_error(value.error().message); }
void publish(Program& program, std::string_view text, std::string_view entry = "run") {
    auto candidate = take(Program::prepare(take(program.request("math.source", text, entry))));
    take(program.try_publish(candidate));
}
template<class T, class Callable, std::size_t N>
T evaluate(Runtime& runtime, const Callable& entry, const std::array<Argument, N>& arguments) {
    T result{};
    take(entry.invoke({}, arguments, ValueOutput{take(runtime.native_type<T>()), &result}));
    return result;
}
template<class T> bool approximately_equal(T a, T b, T epsilon) { return std::abs(a - b) <= epsilon; }
struct NativeMath {
    Vector3 position{1.0f, 2.0f, 3.0f};
    Vector3 value(Vector3 input) const { return {input.x + 1.0f, input.y + 2.0f, input.z + 3.0f}; }
    Vector3 constant(const Vector3& input) const { return input; }
    void mutate(Vector3& input) const { input.x += 4.0f; input.y += 5.0f; input.z += 6.0f; }
    Vector3 fail(Vector3) const { throw std::runtime_error("Native math failure."); }
};
class GuardedPages {
public:
    GuardedPages() {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        page_size_ = info.dwPageSize;
        data_ = static_cast<std::byte*>(VirtualAlloc(nullptr, page_size_ * 2,
            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!data_) throw std::runtime_error("Math guard page allocation failed.");
        DWORD previous{};
        if (!VirtualProtect(data_ + page_size_, page_size_, PAGE_NOACCESS, &previous)) {
            VirtualFree(data_, 0, MEM_RELEASE);
            data_ = nullptr;
            throw std::runtime_error("Math guard page protection failed.");
        }
    }
    ~GuardedPages() { if (data_) VirtualFree(data_, 0, MEM_RELEASE); }
    GuardedPages(const GuardedPages&) = delete;
    GuardedPages& operator=(const GuardedPages&) = delete;
    template<class T> T* construct(T value) {
        return std::construct_at(reinterpret_cast<T*>(data_ + page_size_ - sizeof(T)), value);
    }
private:
    std::byte* data_{};
    std::size_t page_size_{};
};

template<class T> using Bits = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
template<class T> bool same(T a, T b) { return (std::isnan(a) && std::isnan(b)) || std::bit_cast<Bits<T>>(a) == std::bit_cast<Bits<T>>(b); }
// The documented element-wise definitions, written independently of the JIT lowering.
template<class T> T glsl_min(T x, T y) { return y < x ? y : x; }
template<class T> T glsl_max(T x, T y) { return x < y ? y : x; }
template<class T> T clamp_reference(T x, T low, T high) { return glsl_max(low, glsl_min(x, high)); }
template<class T> T fract_reference(T x) { return glsl_min(x - std::floor(x), std::nextafter(T(1), T(0))); }
template<class T> T sign_reference(T x) { return x > T(0) ? T(1) : x < T(0) ? T(-1) : x; }
template<class T> T step_reference(T edge, T x) { return x < edge ? T(0) : T(1); }
template<class T> T lerp_reference(T a, T b, T t) { return a * (T(1) - t) + b * t; }
template<class T> T smoothstep_reference(T e0, T e1, T x) {
    const T t = clamp_reference((x - e0) / (e1 - e0), T(0), T(1));
    return t * t * (T(3) - T(2) * t);
}
template<class T> struct ElementwiseCase { const char* call; std::size_t arity; T (*expected)(T, T, T); };
template<class T> std::array<ElementwiseCase<T>, 13> elementwise_cases() {
    return {{
        {"math.sqrt(x)", 1, [](T x, T, T) { return std::sqrt(x); }},
        {"math.floor(x)", 1, [](T x, T, T) { return std::floor(x); }},
        {"math.ceil(x)", 1, [](T x, T, T) { return std::ceil(x); }},
        {"math.trunc(x)", 1, [](T x, T, T) { return std::trunc(x); }},
        {"math.fract(x)", 1, [](T x, T, T) { return fract_reference(x); }},
        {"math.abs(x)", 1, [](T x, T, T) { return std::fabs(x); }},
        {"math.sign(x)", 1, [](T x, T, T) { return sign_reference(x); }},
        {"math.min(x, y)", 2, [](T x, T y, T) { return glsl_min(x, y); }},
        {"math.max(x, y)", 2, [](T x, T y, T) { return glsl_max(x, y); }},
        {"math.step(x, y)", 2, [](T x, T y, T) { return step_reference(x, y); }},
        {"math.clamp(x, y, z)", 3, [](T x, T y, T z) { return clamp_reference(x, y, z); }},
        {"math.lerp(x, y, z)", 3, [](T x, T y, T z) { return lerp_reference(x, y, z); }},
        {"math.smoothstep(x, y, z)", 3, [](T x, T y, T z) { return smoothstep_reference(x, y, z); }},
    }};
}
// One compiled scalar program per call, comparing every argument combination bit for bit.
template<class T> class ScalarCalls {
public:
    ScalarCalls(Runtime& runtime, const char* type) : runtime_(runtime), type_(type) {}
    T operator()(const char* call, T x, T y = T(0), T z = T(0)) {
        auto& program = programs_[call];
        if (!program) {
            program = std::make_unique<Program>(runtime_, TypeId{});
            publish(*program, type_ + " run(" + type_ + " x, " + type_ + " y, " + type_ + " z) { return " + call + "; }");
        }
        return std::get<T>(*take(program->entry().invoke({}, std::array{Scalar{x}, Scalar{y}, Scalar{z}})));
    }
    void table(std::span<const T> values) {
        for (const auto& item : elementwise_cases<T>()) {
            const auto check = [&](T x, T y, T z) {
                const auto result = (*this)(item.call, x, y, z);
                const auto expected = item.expected(x, y, z);
                if (same(result, expected)) return;
                char text[256];
                std::snprintf(text, sizeof(text), "%s (%s) at (%a, %a, %a) returned %a, expected %a.", item.call,
                    type_.c_str(), double(x), double(y), double(z), double(result), double(expected));
                throw std::runtime_error(text);
            };
            for (const T x : values) {
                if (item.arity == 1) { check(x, T(0), T(0)); continue; }
                for (const T y : values) {
                    if (item.arity == 2) { check(x, y, T(0)); continue; }
                    for (const T z : values) check(x, y, z);
                }
            }
        }
    }
private:
    Runtime& runtime_;
    std::string type_;
    std::map<std::string, std::unique_ptr<Program>> programs_;
};

void verify_elementwise() {
    Runtime runtime;
    take(runtime.register_types({array_definition<float>(runtime, "acceptance.MathFloats")}));
    constexpr auto inf = std::numeric_limits<float>::infinity();
    constexpr auto nan = std::numeric_limits<float>::quiet_NaN();
    constexpr auto tiny = std::numeric_limits<float>::denorm_min();
    const std::array floats{0.0f, -0.0f, 0.25f, -0.25f, 0.5f, -0.5f, 1.0f, -1.0f, 2.75f, -2.25f, 7.0f, 1e-8f, -1e-30f,
        tiny, -tiny, 16777216.0f, 3e38f, -3e38f, inf, -inf, nan};
    ScalarCalls<float> f(runtime, "float");
    f.table(floats);
    constexpr auto dinf = std::numeric_limits<double>::infinity();
    constexpr auto dtiny = std::numeric_limits<double>::denorm_min();
    const std::array doubles{0.0, -0.0, 0.25, -0.5, 1.0, -1.0, 2.75, -2.25, 1e-17, -1e-300, dtiny, -dtiny,
        9007199254740992.0, 1.7e308, -1.7e308, dinf, -dinf, std::numeric_limits<double>::quiet_NaN()};
    ScalarCalls<double> d(runtime, "double");
    d.table(doubles);

    // The contract points, stated directly rather than through the reference definitions.
    const auto below_one = std::nextafter(1.0f, 0.0f);
    require(same(f("math.fract(x)", -1e-30f), below_one) && f("math.fract(x)", -2.25f) == 0.75f &&
        std::isnan(f("math.fract(x)", inf)) && same(d("math.fract(x)", -1e-300), std::nextafter(1.0, 0.0)),
        "math.fract left [0, 1) for tiny negatives or lost its rounding and infinity contract.");
    require(f("math.clamp(x, y, z)", nan, 0.0f, 1.0f) == 0.0f && f("math.clamp(x, y, z)", 5.0f, 2.0f, 1.0f) == 2.0f,
        "math.clamp did not return the lower bound for NaN or inverted bounds.");
    require(std::isnan(f("math.min(x, y)", nan, 1.0f)) && f("math.min(x, y)", 1.0f, nan) == 1.0f &&
        std::isnan(f("math.max(x, y)", nan, 1.0f)) && f("math.max(x, y)", 1.0f, nan) == 1.0f,
        "math.min or math.max lost the GLSL operand order for NaN.");
    require(f("math.lerp(x, y, z)", 1.0f, 1e-8f, 1.0f) == 1e-8f && f("math.lerp(x, y, z)", 3.0f, 7.0f, 0.0f) == 3.0f,
        "math.lerp was not exact at its endpoints.");
    require(f("math.step(x, y)", 0.5f, nan) == 1.0f && same(f("math.sign(x)", -0.0f), -0.0f) && f("math.sign(x)", -3.0f) == -1.0f,
        "math.step or math.sign changed its NaN or signed-zero result.");
    require(f("math.smoothstep(x, y, z)", 1.0f, 1.0f, 1.0f) == 0.0f && f("math.smoothstep(x, y, z)", 1.0f, 1.0f, 2.0f) == 1.0f &&
        f("math.smoothstep(x, y, z)", 0.0f, 1.0f, nan) == 0.0f && f("math.smoothstep(x, y, z)", 0.0f, 1.0f, 2.0f) == 1.0f,
        "math.smoothstep lost its clamp or equal-edge step.");

    // Vector forms with scalar arguments applied to every lane; float literals default to float.
    Program vectors(runtime, {});
    publish(vectors, R"(
        vector3 run(vector3 v, vector3 w, float t) {
            return math.lerp(math.clamp(v, 0.0, 1.0), math.smoothstep(0.0, 1.0, w), t) + math.step(0.5, v)
                + math.min(v, w) + math.max(w, 0.25) + math.floor(w) + math.sqrt(math.abs(v));
        }
    )");
    Vector3 v{-0.5f, 0.75f, 2.0f}, w{0.25f, -1.5f, 3.5f};
    const float t = 0.25f;
    const auto lanes = evaluate<Vector3>(runtime, vectors.entry(),
        std::array{Argument::borrow(v), Argument::borrow(w), Argument{Scalar{t}}});
    for (std::size_t i = 0; i < 3; ++i) {
        const float expected = lerp_reference(clamp_reference(v[i], 0.0f, 1.0f), smoothstep_reference(0.0f, 1.0f, w[i]), t)
            + step_reference(0.5f, v[i]) + glsl_min(v[i], w[i]) + glsl_max(w[i], 0.25f) + std::floor(w[i]) + std::sqrt(std::fabs(v[i]));
        require(same(lanes[i], expected), "Float vector element-wise built-ins changed a lane.");
    }
    Program pair(runtime, {}), quad(runtime, {});
    publish(pair, "vector2 run(vector2 v) { return math.fract(v); }");
    publish(quad, "vector4 run(vector4 v) { return math.sign(v) + math.trunc(v) + math.ceil(v); }");
    math::Vector2 small{-1e-30f, 2.75f};
    math::Vector4 four{-2.5f, -0.0f, 0.5f, 7.25f};
    const auto fractions = evaluate<math::Vector2>(runtime, pair.entry(), std::array{Argument::borrow(small)});
    const auto signs = evaluate<math::Vector4>(runtime, quad.entry(), std::array{Argument::borrow(four)});
    require(same(fractions[0], below_one) && fractions[1] == 0.75f, "vector2 math.fract changed a lane.");
    for (std::size_t i = 0; i < 4; ++i)
        require(same(signs[i], sign_reference(four[i]) + std::trunc(four[i]) + std::ceil(four[i])), "vector4 sign/trunc/ceil changed a lane.");
    Program wide(runtime, {});
    publish(wide, R"(
        vector3d run(vector3d v, vector3d w, double t) {
            return math.lerp(math.clamp(v, 0.0d, 1.0d), math.max(w, v), t) + math.sign(v) + math.ceil(w) + math.trunc(v)
                + math.fract(w) + math.step(0.5d, v) + math.smoothstep(0.0d, 2.0d, w);
        }
    )");
    Vector3d dv{-0.5, 0.75, 2.0}, dw{0.25, -1.5, 3.5};
    const double dt = 0.75;
    const auto dlanes = evaluate<Vector3d>(runtime, wide.entry(),
        std::array{Argument::borrow(dv), Argument::borrow(dw), Argument{Scalar{dt}}});
    for (std::size_t i = 0; i < 3; ++i) {
        const double expected = lerp_reference(clamp_reference(dv[i], 0.0, 1.0), glsl_max(dw[i], dv[i]), dt) + sign_reference(dv[i])
            + std::ceil(dw[i]) + std::trunc(dv[i]) + fract_reference(dw[i]) + step_reference(0.5, dv[i]) + smoothstep_reference(0.0, 2.0, dw[i]);
        require(same(dlanes[i], expected), "Double vector element-wise built-ins changed a lane.");
    }

    Program invalid(runtime, {});
    const auto rejected = [&](std::string_view text, std::string_view at, const std::string& message) {
        const auto failed = Program::prepare(take(invalid.request("elementwise.source", text, "run")));
        const auto expected = ":1:" + std::to_string(text.rfind(at) + 1) + ": " + message;
        require(!failed && failed.error().message.find(expected) != std::string::npos,
            ("Element-wise built-in did not reject with \"" + expected + "\": " + (failed ? "accepted" : failed.error().message)).c_str());
    };
    const auto mismatch = [](const char* name) {
        return std::string(name) + " arguments must have the result type or its component type; numeric types and precision do not convert implicitly.";
    };
    rejected("vector3d run(vector3d v) { return math.clamp(v, 0.0, 1.0d); }", "0.0", mismatch("math.clamp"));
    rejected("vector3 run(vector3 a, vector2 b) { return math.max(a, b); }", "b)", mismatch("math.max"));
    rejected("float run(float x) { return math.min(x, 2); }", "2", mismatch("math.min"));
    rejected("quaternion run(quaternion q) { return math.floor(q); }", "math.floor", "math.floor requires float or double scalars or vectors.");
    rejected("matrix4 run(matrix4 m) { return math.abs(m); }", "math.abs", "math.abs requires float or double scalars or vectors.");
    rejected("uint run(uint a) { return math.min(a, 2); }", "math.min", "math.min requires float or double scalars or vectors.");
    rejected("float run(float x) { return math.clamp(x, 0.0); }", "math.clamp", "Math operation argument count mismatch.");
    rejected("double run(double x) { return math.sin(x); }", "math.sin", "math.sin requires float scalars or vectors.");
    rejected("vector3d run(vector3d v) { return math.exp(v); }", "math.exp", "math.exp requires float scalars or vectors.");
    rejected("quaternion run(quaternion q) { return math.cos(q); }", "math.cos", "math.cos requires float scalars or vectors.");
    rejected("uint run(uint a) { return math.log(a); }", "math.log", "math.log requires float scalars or vectors.");
    rejected("float run(float x) { return math.atan2(x); }", "math.atan2", "Math operation argument count mismatch.");
    rejected("float run(float x) { return math.pow(x, 2.0d); }", "2.0d", mismatch("math.pow"));

    // A borrowed-array kernel adds no function call over the same loop without built-ins, and agrees with the definitions.
    const auto calls = [&](const char* name, std::string_view body) {
        Program program(runtime, {});
        auto candidate = take(Program::prepare(take(program.request(name, R"(
            void run(Array<float>& values, float low, float high) {
                uint i = 0;
                while (i < values.size()) { float x = values[i]; values[i] = )" + std::string(body) + R"(; i = i + 1; }
            }
        )", "run", {.artifacts = true}))));
        take(program.try_publish(candidate));
        const auto assembly = take(take(program.pin()).artifacts()).assembly;
        std::size_t count = 0;
        for (auto at = assembly.find("\tcall\t"); at != std::string::npos; at = assembly.find("\tcall\t", at + 1)) ++count;
        return count;
    };
    constexpr auto kernel_body = "math.lerp(math.clamp(math.floor(x), low, high), math.fract(x), math.smoothstep(low, high, x))"
        " + math.min(x, high) + math.sign(x)";
    const auto baseline = calls("baseline-kernel.source", "x * low + high");
    require(calls("elementwise-kernel.source", kernel_body) == baseline, "Element-wise built-ins lowered to a function call.");
    require(calls("transcendental-kernel.source", "math.sin(x) + math.cos(x) + math.exp(x) + math.log(x) + math.atan2(x, high) + math.pow(x, low)")
        == baseline, "Transcendental built-ins lowered to a function call.");
    Program kernel(runtime, {});
    publish(kernel, std::string("void run(Array<float>& values, float low, float high) { uint i = 0; "
        "while (i < values.size()) { float x = values[i]; values[i] = ") + kernel_body + "; i = i + 1; } }");
    Array<float> samples;
    for (const float value : floats) samples.add(value);
    const float low = -0.5f, high = 2.0f;
    take(kernel.entry().invoke({}, std::array{Argument::borrow(samples), Argument{Scalar{low}}, Argument{Scalar{high}}}));
    for (std::size_t i = 0; i < floats.size(); ++i) {
        const float x = floats[i];
        const float expected = lerp_reference(clamp_reference(std::floor(x), low, high), fract_reference(x), smoothstep_reference(low, high, x))
            + glsl_min(x, high) + sign_reference(x);
        require(same(samples[i], expected), "The element-wise array kernel changed a value.");
    }
}

// Error in units of the last place of the double reference, which stands in for the exact value.
double ulp_error(float result, double reference) {
    if (std::isnan(reference) || std::isnan(result)) return std::isnan(reference) && std::isnan(result) ? 0.0 : INFINITY;
    const auto rounded = static_cast<float>(reference);
    if (std::isinf(rounded) || std::isinf(result)) return result == rounded ? 0.0 : std::fabs(double(result) - reference) / 0x1p104;
    const int exponent = std::max(std::ilogb(rounded == 0 ? std::numeric_limits<float>::denorm_min() : rounded), -126);
    return std::fabs(double(result) - reference) / std::ldexp(1.0, exponent - 23);
}
// The documented pow: y = 0 or x = 1 gives 1, NaN and x < 0 give NaN, and -0 counts as +0.
double pow_reference(double x, double y) {
    if (y == 0 || x == 1) return 1;
    if (std::isnan(x) || std::isnan(y) || x < 0) return NAN;
    return std::pow(std::fabs(x), y);
}
struct Transcendental { std::string name; bool binary; double (*reference)(double, double); };

// Float sin/cos/exp/log/atan2/pow: within 4 ULP of the double references, identical on the scalar and the vectorized path.
void verify_transcendental() {
    Runtime runtime;
    take(runtime.register_types({array_definition<float>(runtime, "acceptance.TranscendentalFloats")}));
    constexpr auto inf = std::numeric_limits<float>::infinity();
    constexpr auto nan = std::numeric_limits<float>::quiet_NaN();
    constexpr auto tiny = std::numeric_limits<float>::denorm_min();
    const std::array<Transcendental, 6> functions{{
        {"sin", false, [](double x, double) { return std::sin(x); }},
        {"cos", false, [](double x, double) { return std::cos(x); }},
        {"exp", false, [](double x, double) { return std::exp(x); }},
        {"log", false, [](double x, double) { return std::log(x); }},
        {"atan2", true, [](double y, double x) { return std::atan2(y, x); }},
        {"pow", true, pow_reference},
    }};
    std::mt19937 random(20261023);
    const auto bits = [&](std::uint32_t mask) { return static_cast<std::uint32_t>(random()) & mask; };
    // Both signs of every binade including subnormals: its ends and six random mantissas.
    std::vector<float> binades{nan};
    for (std::uint32_t exponent = 0; exponent < 255; ++exponent)
        for (const auto mantissa : {0u, 0x7fffffu, bits(0x7fffff), bits(0x7fffff), bits(0x7fffff), bits(0x7fffff), bits(0x7fffff), bits(0x7fffff)}) {
            const auto x = std::bit_cast<float>(exponent << 23 | mantissa);
            binades.push_back(x);
            binades.push_back(-x);
        }
    for (const float x : {inf, -inf}) binades.push_back(x);
    ScalarCalls<float> scalar(runtime, "float");
    for (const auto& function : functions) {
        const bool periodic = function.name == "sin" || function.name == "cos";
        std::vector<float> first = binades, second;
        const auto around = [&](float x, int count) {
            for (auto value = x, below = x; count-- > 0; value = std::nextafter(value, inf), below = std::nextafter(below, -inf)) {
                first.push_back(value);
                first.push_back(below);
            }
        };
        if (periodic) {
            // Near multiples of pi/2, where the reduced argument cancels most, and the largest cancellations below 2^20.
            for (int k = 1; k <= 4096; k += k < 64 ? 1 : 61) around(static_cast<float>(k * 1.5707963267948966), 3);
            for (const std::uint32_t hard : {0x437ce5f1u, 0x474d246fu, 0x47cd246fu, 0x4882665eu, 0x4902665eu, 0x49800000u}) around(std::bit_cast<float>(hard), 2);
        } else if (function.name == "exp") {
            for (int k = 0; k <= 4096; ++k) first.push_back(-104.0f + static_cast<float>(k) * (193.0f / 4096));
            for (const float edge : {88.72283f, -87.33654f, -103.97208f}) around(edge, 4);
        } else if (function.name == "log") {
            around(1.0f, 64);
            around(std::bit_cast<float>(0x3f3504f3u), 4);
        } else {
            // Pairs: the special values crossed, random bit patterns, and ratios at the reduction boundaries.
            // 2 and 3 times the smallest subnormal: halving 3 * tiny rounds up, the region test must not.
            std::vector<float> specials{0.0f, -0.0f, tiny, -tiny, 2 * tiny, -2 * tiny, 3 * tiny, -3 * tiny, 0.5f, -0.5f, 1.0f, -1.0f, 2.0f, -3.0f,
                3e38f, -3e38f, inf, -inf, nan};
            first.clear();
            for (const float a : specials) for (const float b : specials) { first.push_back(a); second.push_back(b); }
            for (int i = 0; i < 8192; ++i) { first.push_back(std::bit_cast<float>(bits(~0u))); second.push_back(std::bit_cast<float>(bits(~0u))); }
            for (int i = 0; i < 4096; ++i) {
                const auto x = std::bit_cast<float>(bits(0x7fffffff));
                if (function.name == "atan2") {
                    for (const float ratio : {0.5f, std::nextafter(0.5f, 1.0f), std::nextafter(0.5f, 0.0f), 1.0f, std::nextafter(1.0f, 0.0f), 0.41421357f, 2.0f}) {
                        first.push_back((i & 1 ? -1.0f : 1.0f) * x * ratio);
                        second.push_back(i & 2 ? -x : x);
                    }
                } else {
                    // Exponents y * log2(x) across [-160, 140], covering overflow, subnormal results and underflow.
                    const double log2 = std::log2(double(x));
                    const double target = -160.0 + 300.0 * static_cast<double>(bits(0xffffff)) / 0x1p24;
                    first.push_back(x);
                    second.push_back(log2 == 0 ? 1.0f : static_cast<float>(target / log2));
                    first.push_back(-x);
                    second.push_back(static_cast<float>(i % 7));
                }
            }
        }
        if (second.empty()) second.assign(first.size(), 0.0f);
        Program kernel(runtime, {});
        publish(kernel, "void run(Array<float>& a, const Array<float>& b) { uint i = 0; while (i < a.size()) { a[i] = math." +
            function.name + (function.binary ? "(a[i], b[i])" : "(a[i])") + "; i = i + 1; } }");
        Array<float> lanes, others;
        for (std::size_t i = 0; i < first.size(); ++i) { lanes.add(first[i]); others.add(second[i]); }
        take(kernel.entry().invoke({}, std::array{Argument::borrow(lanes), Argument::borrow(std::as_const(others))}));
        const auto call = "math." + function.name + (function.binary ? "(x, y)" : "(x)");
        for (std::size_t i = 0; i < first.size(); ++i) {
            const float x = first[i], y = second[i], result = lanes[i];
            char text[256];
            const auto fail = [&](const char* reason, double expected) {
                std::snprintf(text, sizeof(text), "%s at (%a, %a) returned %a, expected %a: %s", call.c_str(), double(x), double(y),
                    double(result), expected, reason);
                throw std::runtime_error(text);
            };
            if (!same(scalar(call.c_str(), x, y), result)) fail("the scalar call and the vectorized loop disagree", double(scalar(call.c_str(), x, y)));
            if (periodic && std::fabs(x) > 0x1p20f) {
                if (std::isfinite(x) ? !(std::fabs(result) <= 1.0f) : !std::isnan(result)) fail("outside the accurate range the result must be finite with magnitude at most 1", 0);
                continue;
            }
            const double expected = function.reference(x, y);
            if (ulp_error(result, expected) > 4) fail("more than 4 ULP", expected);
        }
    }

    // Exact results the contract names, including signed zeros and IEEE infinities.
    struct Exact { const char* call; float x, y, expected; };
    constexpr float pi = 0x1.921fb6p+1f, half_pi = 0x1.921fb6p+0f, quarter_pi = 0x1.921fb6p-1f, three_quarters_pi = 0x1.2d97c8p+1f;
    const std::array exact{
        Exact{"math.sin(x)", 0.0f, 0, 0.0f}, Exact{"math.sin(x)", -0.0f, 0, -0.0f}, Exact{"math.cos(x)", -0.0f, 0, 1.0f},
        Exact{"math.sin(x)", inf, 0, nan}, Exact{"math.cos(x)", -inf, 0, nan}, Exact{"math.cos(x)", nan, 0, nan},
        Exact{"math.exp(x)", -inf, 0, 0.0f}, Exact{"math.exp(x)", inf, 0, inf}, Exact{"math.exp(x)", -0.0f, 0, 1.0f},
        Exact{"math.exp(x)", 88.8f, 0, inf}, Exact{"math.exp(x)", -104.0f, 0, 0.0f}, Exact{"math.exp(x)", nan, 0, nan},
        Exact{"math.log(x)", 0.0f, 0, -inf}, Exact{"math.log(x)", -0.0f, 0, -inf}, Exact{"math.log(x)", -1.0f, 0, nan},
        Exact{"math.log(x)", -inf, 0, nan}, Exact{"math.log(x)", inf, 0, inf}, Exact{"math.log(x)", 1.0f, 0, 0.0f}, Exact{"math.log(x)", nan, 0, nan},
        Exact{"math.atan2(x, y)", 0.0f, -0.0f, pi}, Exact{"math.atan2(x, y)", -0.0f, -0.0f, -pi}, Exact{"math.atan2(x, y)", 0.0f, 0.0f, 0.0f},
        Exact{"math.atan2(x, y)", -0.0f, 0.0f, -0.0f}, Exact{"math.atan2(x, y)", -0.0f, -2.0f, -pi}, Exact{"math.atan2(x, y)", 3.0f, 0.0f, half_pi},
        Exact{"math.atan2(x, y)", -3.0f, -0.0f, -half_pi}, Exact{"math.atan2(x, y)", 1.0f, -inf, pi}, Exact{"math.atan2(x, y)", -1.0f, inf, -0.0f},
        Exact{"math.atan2(x, y)", inf, -5.0f, half_pi}, Exact{"math.atan2(x, y)", -inf, -inf, -three_quarters_pi},
        Exact{"math.atan2(x, y)", inf, inf, quarter_pi}, Exact{"math.atan2(x, y)", nan, 1.0f, nan}, Exact{"math.atan2(x, y)", 1.0f, nan, nan},
        Exact{"math.pow(x, y)", nan, 0.0f, 1.0f}, Exact{"math.pow(x, y)", 0.0f, -0.0f, 1.0f}, Exact{"math.pow(x, y)", 1.0f, nan, 1.0f},
        Exact{"math.pow(x, y)", 1.0f, -inf, 1.0f}, Exact{"math.pow(x, y)", -2.0f, 2.0f, nan}, Exact{"math.pow(x, y)", -inf, 3.0f, nan},
        Exact{"math.pow(x, y)", -0.0f, 3.0f, 0.0f}, Exact{"math.pow(x, y)", -0.0f, -1.0f, inf}, Exact{"math.pow(x, y)", 0.0f, -inf, inf},
        Exact{"math.pow(x, y)", 0.5f, inf, 0.0f}, Exact{"math.pow(x, y)", 2.0f, inf, inf}, Exact{"math.pow(x, y)", 0.5f, -inf, inf},
        Exact{"math.pow(x, y)", inf, -1.0f, 0.0f}, Exact{"math.pow(x, y)", inf, 0.5f, inf}, Exact{"math.pow(x, y)", nan, 1.0f, nan},
        Exact{"math.pow(x, y)", 2.0f, nan, nan}, Exact{"math.pow(x, y)", 2.0f, 128.0f, inf}, Exact{"math.pow(x, y)", 2.0f, -150.0f, 0.0f},
        Exact{"math.pow(x, y)", 2.0f, -149.0f, tiny}, Exact{"math.pow(x, y)", 2.0f, 10.0f, 1024.0f},
    };
    for (const auto& item : exact) {
        const auto result = scalar(item.call, item.x, item.y);
        if (same(result, item.expected)) continue;
        char text[256];
        std::snprintf(text, sizeof(text), "%s at (%a, %a) returned %a, expected exactly %a.", item.call, double(item.x), double(item.y),
            double(result), double(item.expected));
        throw std::runtime_error(text);
    }

    // Vector lanes and scalar arguments give the scalar results bit for bit.
    Program vectors(runtime, {}), splats(runtime, {});
    publish(vectors, "vector4 run(vector4 v, vector4 w) { return math.sin(v) + math.cos(w) + math.exp(v) + math.log(w) + math.atan2(v, w) + math.pow(w, v); }");
    publish(splats, "vector2 run(vector2 v, float s) { return math.pow(v, s) + math.atan2(s, v); }");
    math::Vector4 v{0.5f, -2.25f, 10.0f, 1e-3f}, w{3.5f, 0.25f, 1e4f, 7.0f};
    const auto lanes = evaluate<math::Vector4>(runtime, vectors.entry(), std::array{Argument::borrow(v), Argument::borrow(w)});
    for (std::size_t i = 0; i < 4; ++i)
        require(same(lanes[i], scalar("math.sin(x)", v[i]) + scalar("math.cos(x)", w[i]) + scalar("math.exp(x)", v[i])
            + scalar("math.log(x)", w[i]) + scalar("math.atan2(x, y)", v[i], w[i]) + scalar("math.pow(x, y)", w[i], v[i])),
            "vector4 transcendental built-ins changed a lane.");
    math::Vector2 pair{0.75f, 6.5f};
    const float s = 1.25f;
    const auto splatted = evaluate<math::Vector2>(runtime, splats.entry(), std::array{Argument::borrow(pair), Argument{Scalar{s}}});
    for (std::size_t i = 0; i < 2; ++i)
        require(same(splatted[i], scalar("math.pow(x, y)", pair[i], s) + scalar("math.atan2(x, y)", s, pair[i])),
            "Scalar arguments to math.pow or math.atan2 did not apply to every lane.");
}
}

void verify_source_math() {
    static_assert(sizeof(Vector3) == 3 * sizeof(float));
    static_assert(sizeof(Vector3d) == 3 * sizeof(double));
    Runtime runtime;
    take(runtime.register_types({
        array_definition<Vector3>(runtime, "acceptance.MathVectors"),
        array_definition<Vector3d>(runtime, "acceptance.MathVectorsd"),
        NativeRegistration<NativeMath>{runtime, "acceptance.NativeMath", TypeKind::object}
            .field<&NativeMath::position>("position")
            .method<&NativeMath::value>("value", true)
            .method<&NativeMath::constant>("constant", true)
            .method<&NativeMath::mutate>("mutate", true)
            .method<&NativeMath::fail>("fail", true).build()}));

    Program values(runtime, {});
    publish(values, R"(
        vector3 helper(vector3 value) { return math.cross(value, vector3(0.0, 0.0, 1.0)); }
        math.vector3 run(vector3 value) {
            vector3 result = helper(value) + vector3(1.0, 2.0, 3.0);
            return result * 2.0 / 2.0;
        }
    )");
    struct CompactTail { Vector3 value; std::uint32_t canary; } compact{{2.0f, 3.0f, 4.0f}, 0xfeedbeef};
    const std::array arguments{Argument::borrow(compact.value)};
    auto result = evaluate<Vector3>(runtime, values.entry(), arguments);
    require(result.x == 4.0f && result.y == 0.0f && result.z == 3.0f && compact.canary == 0xfeedbeef,
        "Source math construction, helper value return or compact storage failed.");
    const auto old = take(values.pin());
    publish(values, "vector3 run(vector3 value) { return value - vector3(1.0, 1.0, 1.0); }");
    result = evaluate<Vector3>(runtime, values.entry(), arguments);
    const auto previous = evaluate<Vector3>(runtime, old, arguments);
    require(result.x == 1.0f && result.y == 2.0f && result.z == 3.0f && previous.x == 4.0f,
        "Typed math result or old math lease changed across publication.");

    Program compact_float(runtime, {}), compact_double(runtime, {});
    publish(compact_float, "void run(vector3& value) { value = value * 2.0; }");
    publish(compact_double, "void run(vector3d& value) { value = value * 2.0d; }");
    {
        GuardedPages pages;
        auto* guarded = pages.construct(Vector3{1.0f, 2.0f, 3.0f});
        take(compact_float.entry().invoke({}, std::array{Argument::borrow(*guarded)}));
        require(*guarded == Vector3{2.0f, 4.0f, 6.0f}, "Compact float boundary math changed values.");
        std::destroy_at(guarded);
    }
    {
        GuardedPages pages;
        auto* guarded = pages.construct(Vector3d{1.0, 2.0, 3.0});
        take(compact_double.entry().invoke({}, std::array{Argument::borrow(*guarded)}));
        require(*guarded == Vector3d{2.0, 4.0, 6.0}, "Compact double boundary math changed values.");
        std::destroy_at(guarded);
    }

    Program batch(runtime, {});
    publish(batch, R"(
        vector3 helper(vector3 value) { return value + vector3(1.0, 2.0, 3.0); }
        void run(Array<vector3>& values) {
            uint index = 0;
            while (index < values.size()) { values[index] = helper(values[index]); index = index + 1; }
        }
    )");
    Array<Vector3> vectors; vectors.add({1.0f, 2.0f, 3.0f}); vectors.add({4.0f, 5.0f, 6.0f});
    take(batch.entry().invoke({}, std::array{Argument::borrow(vectors)}));
    require(vectors.size() == 2 && vectors[0].x == 2.0f && vectors[1].z == 9.0f,
        "Source batch math did not mutate the host compact vector array.");

    Program doubles(runtime, {});
    publish(doubles, R"(
        vector3d helper(vector3d value) { return value + vector3d(1.0d, 2.0d, 3.0d); }
        double run(Array<vector3d>& values) {
            uint index = 0;
            while (index < values.size()) { values[index] = helper(values[index]); index = index + 1; }
            return values[0].x + double(1.0);
        }
    )");
    Array<Vector3d> wide; wide.add({16777216.0, 0.0, 0.0}); wide.add({1.0, 2.0, 3.0});
    const auto precision = take(doubles.entry().invoke({}, std::array{Argument::borrow(wide)}));
    require(std::get<double>(*precision) == 16777218.0 && wide[0].x == 16777217.0 && wide[1].z == 6.0,
        "Double source math narrowed through float storage or arithmetic.");

    Program families(runtime, {});
    publish(families, R"(
        double run() {
            vector2 small = vector2(3.0, 4.0);
            vector4 four = vector4(1.0, 2.0, 3.0, 4.0);
            vector2d smalld = vector2d(3.0d, 4.0d);
            vector4d fourd = vector4d(1.0d, 2.0d, 3.0d, 4.0d);
            quaterniond rotation = quaterniond(0.0d, 0.0d, 1.0d, 0.0d);
            vector3d direction = math.rotate(math.conjugate(rotation), vector3d(1.0d, 2.0d, 3.0d));
            vector3d transformed = math.transform_direction(direction, math.transpose(matrix4d(1.0d)));
            return double(math.length(small)) + double(math.dot(four, four))
                + math.length(smalld) + math.dot(fourd, fourd) + math.sqrt(4.0d) + transformed.x;
        }
    )");
    const auto family_result = std::get<double>(*take(families.entry().invoke({}, std::array<Scalar, 0>{})));
    if (family_result != 71.0)
        throw std::runtime_error("Math family scalar result expected 71, got " + std::to_string(family_result));

    Program normalized(runtime, {});
    publish(normalized, "vector3 run(vector3 value) { return math.normalize_or_zero(value); }");
    for (const float scale : {std::numeric_limits<float>::max(), std::numeric_limits<float>::min()}) {
        Vector3 input{scale, scale, scale};
        const auto unit = evaluate<Vector3>(runtime, normalized.entry(), std::array{Argument::borrow(input)});
        require(approximately_equal(unit.x, 0.577350269f, 0.000001f) && approximately_equal(unit.y, unit.x, 0.000001f) && approximately_equal(unit.z, unit.x, 0.000001f),
            "Source vector normalization overflowed or underflowed finite inputs.");
    }
    for (const float scale : {0.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        Vector3 input{scale, 0.0f, 0.0f};
        const auto zero = evaluate<Vector3>(runtime, normalized.entry(), std::array{Argument::borrow(input)});
        require(zero.x == 0.0f && zero.y == 0.0f && zero.z == 0.0f,
            "Source safe normalization did not handle zero or nonfinite input.");
    }
    Program normalized_double(runtime, {});
    publish(normalized_double, "vector3d run(vector3d value) { return math.normalize_or_zero(value); }");
    for (const double scale : {std::numeric_limits<double>::max(), std::numeric_limits<double>::min()}) {
        Vector3d input{scale, scale, scale};
        const auto unit = evaluate<Vector3d>(runtime, normalized_double.entry(), std::array{Argument::borrow(input)});
        require(approximately_equal(unit.x, 0.5773502691896257645, 1e-14), "Double normalization lost precision or finite range.");
    }

    Program transforms(runtime, {});
    publish(transforms, R"(
        vector3 run(vector3 value, matrix4 first, matrix4 second) {
            quaternion turn = quaternion(0.0, 0.0, 1.0, 0.0);
            quaternion identity = quaternion();
            vector3 rotated = math.rotate(turn * identity, value);
            return math.transform_point(rotated, first * second * matrix4(1.0));
        }
    )");
    Vector3 point{1.0f, 2.0f, 3.0f};
    auto first = math::Matrix4::identity(); first(3, 0) = 5.0f; first(3, 1) = 7.0f;
    auto second = math::Matrix4::identity(); second(0, 0) = 2.0f; second(1, 1) = 3.0f;
    const auto transformed = evaluate<Vector3>(runtime, transforms.entry(),
        std::array{Argument::borrow(point), Argument::borrow(first), Argument::borrow(second)});
    require(approximately_equal(transformed.x, 8.0f, 0.00001f) && approximately_equal(transformed.y, 15.0f, 0.00001f) && approximately_equal(transformed.z, 3.0f, 0.00001f),
        "Source quaternion or row-vector matrix composition changed conventions.");

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
    Vector3 native_output{};
    take(native.entry().invoke(host.id(), std::array{Argument::borrow(point)},
        ValueOutput{take(runtime.native_type<Vector3>()), &native_output}));
    require(native_output.x == 6.0f && native_output.y == 9.0f && native_output.z == 12.0f && point.x == 1.0f,
        "Native math value, const-reference or mutable-reference interop failed.");
    Program fields(runtime, native_type);
    publish(fields, R"(
        vector3 run(vector3 step) {
            vector3 previous = self.position;
            self.position = previous + step;
            return self.position;
        }
    )");
    take(fields.entry().invoke(host.id(), std::array{Argument::borrow(point)},
        ValueOutput{take(runtime.native_type<Vector3>()), &native_output}));
    require(native_output.x == 2.0f && native_output.y == 4.0f && native_output.z == 6.0f,
        "Source whole math field read/write lost the native receiver state.");
    take(fields.entry().invoke(host.id(), std::array{Argument::borrow(point)},
        ValueOutput{take(runtime.native_type<Vector3>()), &native_output}));
    require(native_output.x == 3.0f && native_output.y == 6.0f && native_output.z == 9.0f,
        "Source whole math field write did not persist across invocations.");
    publish(native, "vector3 run(vector3 value) { return self.fail(value); }");
    Vector3 preserved{91.0f, 92.0f, 93.0f};
    const auto failure = native.entry().invoke(host.id(), std::array{Argument::borrow(point)},
        ValueOutput{take(runtime.native_type<Vector3>()), &preserved});
    require(!failure && failure.error().message == "Native math failure." &&
        preserved.x == 91.0f && preserved.y == 92.0f && preserved.z == 93.0f,
        "Failed typed source call lost its native diagnostic or modified output.");

    Program invalid(runtime, {});
    for (const auto* text : {
        "vector3 run() { return vector3(1.0, 2.0, 3.0) + vector3d(1.0d, 2.0d, 3.0d); }",
        "vector3d run() { return vector3d(1.0d, 2.0d, 3.0d) * 2.0; }"})
        require(!Program::prepare(take(invalid.request("mixed-math.source", text, "run"))),
            "Mixed precision math was silently admitted.");

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
        "Async f64 parameter, suspension or result narrowed precision.");
    verify_elementwise();
    verify_transcendental();
    std::puts("Source math: PASS (compact arrays, float/double, value calls, native interop, stable normalization, transforms, reload, async f64, element-wise and transcendental built-ins)");
}

namespace {
template<class Vector>
void measure_precision(Runtime& runtime, std::string_view scalar, std::string_view vector, const char* precision) {
    using Real = typename Vector::Scalar;
    constexpr std::size_t count = 65536;
    constexpr std::size_t ticks = 32;
    constexpr std::size_t samples = 5;
    const Real dt = Real(0.125);
    Array<Vector> positions, velocities;
    positions.resize(count); velocities.resize(count);
    const auto reset = [&] {
        for (std::size_t i = 0; i < count; ++i) {
            const Real value = Real(i % 1024) * Real(0.25);
            positions[i] = {value, value + Real(1), value - Real(1)};
            velocities[i] = {Real(i % 7) * Real(0.125), Real(-0.25), Real(0.5)};
        }
    };
    reset();
    const std::string signature = "void run(Array<" + std::string(vector) + ">& positions, const Array<" +
        std::string(vector) + ">& velocities, " + std::string(scalar) + " dt) { uint i = 0; while (i < positions.size()) { ";
    Program vector_program(runtime, {}), scalar_program(runtime, {});
    publish(vector_program, signature + "positions[i] = positions[i] + velocities[i] * dt; i = i + 1; } }");
    publish(scalar_program, signature +
        "positions[i].x = positions[i].x + velocities[i].x * dt; "
        "positions[i].y = positions[i].y + velocities[i].y * dt; "
        "positions[i].z = positions[i].z + velocities[i].z * dt; i = i + 1; } }");
    const std::array arguments{Argument::borrow(positions), Argument::borrow(std::as_const(velocities)), Argument{Scalar{dt}}};
    const auto vector_tick = [&] { take(vector_program.entry().invoke({}, arguments)); };
    const auto scalar_tick = [&] { take(scalar_program.entry().invoke({}, arguments)); };
    const auto native_tick = [&] {
        for (std::size_t i = 0; i < count; ++i) positions[i] = positions[i] + velocities[i] * dt;
    };
    // Compile and run each path before sampling. All allocation and resets are
    // outside the measured interval; samples rotate the first path for fairness.
    vector_tick(); scalar_tick(); native_tick();
    Array<Vector> reference;
    reference.resize(count);
    std::array<std::array<double, samples>, 3> nanoseconds{};
    for (std::size_t sample = 0; sample < samples; ++sample) {
        for (std::size_t order = 0; order < 3; ++order) {
            const auto variant = (sample + order) % 3;
            reset();
            const auto begin = std::chrono::steady_clock::now();
            for (std::size_t tick = 0; tick < ticks; ++tick) {
                if (variant == 0) vector_tick();
                else if (variant == 1) scalar_tick();
                else native_tick();
            }
            nanoseconds[variant][sample] = std::chrono::duration<double, std::nano>(
                std::chrono::steady_clock::now() - begin).count();
            if (sample == 0 && order == 0)
                for (std::size_t i = 0; i < count; ++i) reference[i] = positions[i];
            else
                for (std::size_t i = 0; i < count; ++i)
                    require(positions[i] == reference[i], "Math measurement variants produced different positions.");
        }
    }
    constexpr std::array names{"source_vector", "source_scalar", "native_cpp"};
    for (std::size_t variant = 0; variant < 3; ++variant) {
        std::sort(nanoseconds[variant].begin(), nanoseconds[variant].end());
        const double median = nanoseconds[variant][samples / 2];
        std::printf("{\"kind\":\"math_measure\",\"precision\":\"%s\",\"variant\":\"%s\","
            "\"positions\":%zu,\"ticks\":%zu,\"samples\":%zu,\"median_ns\":%.0f,"
            "\"million_positions_per_second\":%.3f,\"verified\":true}\n",
            precision, names[variant], count, ticks, samples, median,
            static_cast<double>(count * ticks) * 1000.0 / median);
    }
}
}

void measure_source_math() {
    Runtime runtime;
    take(runtime.register_types({array_definition<Vector3>(runtime, "measurement.MathVectors"),
        array_definition<Vector3d>(runtime, "measurement.MathVectorsd")}));
    measure_precision<Vector3>(runtime, "float", "vector3", "float");
    measure_precision<Vector3d>(runtime, "double", "vector3d", "double");
}
