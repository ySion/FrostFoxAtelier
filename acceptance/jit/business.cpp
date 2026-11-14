#include "business.hpp"
#include "../helpers.hpp"
#include "runtime/native.hpp"
#include "source/program.hpp"
#include <array>
#include <utility>

namespace atelier::jit_acceptance {
void verify_math_abi();

namespace {
using namespace acceptance::helpers;

void publish(source::Program& program, std::string_view text) {
    auto candidate = take(source::Program::prepare(take(program.request("abi.source", text))));
    take(program.try_publish(candidate));
}

struct Counter {
    float value{13};

    float bump(float amount) { return value += amount; }
};

struct Padding {
    virtual ~Padding() = default;

    std::uint64_t sentinel{0x123456789abcdef0};
};

struct Derived : Padding, Counter {};

void scalar_abi() {
    Runtime runtime;
    auto program = take(source::Program::create(runtime));
    publish(*program, R"(
        double run(int signed_value, float narrow, double wide, uint high, bool enabled, int tail) {
            double value = double(signed_value) * 2.0d + double(narrow) * 3.0d + wide * 5.0d
                         + double(high) + double(tail) * 7.0d;
            if (enabled) { value = value + 11.0d; }
            return value;
        })");
    const auto call =
        take(take(program->pin()).bind<double, std::int32_t, float, double, std::uint32_t, bool, std::int32_t>());
    // Independent arithmetic; the first three values use registers, the rest the stack.
    require(take(call.invoke({}, -17, 1.25f, -2.5, 0xf0000001u, true, 9)) == 4026531872.25,
            "Typed scalar ABI changed register/stack positions or numeric representation.");
}

void boolean_alias() {
    Runtime runtime;
    auto program = take(source::Program::create(runtime));
    publish(
        *program,
        "bool run(bool copy, const bool& read, bool& write) { write = copy; return read == write && write != !copy; }");
    struct Guarded {
        bool value{};

        bool before{true};

        bool after{true};

        std::uint32_t guard{0xdeadbeef};
    } storage;
    const std::array args{Argument{Scalar{true}}, Argument::borrow(std::as_const(storage.value)),
                          Argument::borrow(storage.value)};
    const auto result = take(program->entry().invoke({}, args));
    require(result && std::get<bool>(*result) && storage.value && storage.before && storage.after &&
                storage.guard == 0xdeadbeef,
            "Bool ABI lost its borrowed alias or overwrote adjacent bytes.");
}

void reference_coroutine() {
    Runtime runtime;
    const auto base = take(
        NativeRegistration<Counter>{runtime, "abi.Counter", TypeKind::object}.method<&Counter::bump>("bump").publish());
    const auto derived =
        take(NativeRegistration<Derived>{runtime, "abi.Derived", TypeKind::object}.base<Counter>().publish());
    auto object = take(runtime.create(derived));
    const auto id = object.id();
    auto program = take(source::Program::create(runtime, base));
    publish(*program, R"(
        async float run(float amount) {
            abi.Counter held = self;
            weak abi.Counter watched = observe(held);
            yield;
            abi.Counter acquired = try_acquire(watched);
            if (acquired != self) { return 0.0; }
            return acquired.bump(amount);
        })");
    auto task = take(program->entry().start(id, std::array{Scalar{2.f}}));
    require(take(task.resume()) == CoroutineState::suspended, "Reference coroutine did not suspend.");
    object.reset();
    take(runtime.begin_collection());
    while (runtime.collection_phase() != CollectionPhase::idle)
        take(runtime.collect_step({16}));
    require(take(task.resume()) == CoroutineState::completed && std::get<float>(*take(task.result())) == 15,
            "Suspended strong/weak references or adjusted receiver lost identity or data.");
    take(runtime.begin_collection());
    while (runtime.collection_phase() != CollectionPhase::idle)
        take(runtime.collect_step({16}));
    require(runtime.live_objects() == 0 && runtime.root_count() == 0, "Completed frame retained its receiver.");
}
}

void verify_business_execution() {
    scalar_abi();
    boolean_alias();
    reference_coroutine();
    verify_math_abi();
}
}
