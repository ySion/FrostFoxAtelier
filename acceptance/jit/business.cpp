#include "business.hpp"
#include "../helpers.hpp"
#include "runtime/native.hpp"
#include "source/detail/bridge.hpp"
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

enum class Mode : std::int32_t { idle = -3, active = 7 };

void scalar_abi() {
    Runtime runtime;
    take(NativeRegistration<Mode>{runtime, "abi.Mode", TypeKind::enumeration}
             .enumerator("idle", Mode::idle)
             .enumerator("active", Mode::active)
             .publish());
    auto program = take(source::Program::create(runtime));
    publish(*program, R"(
        double run(int signed_value, float narrow, double wide, uint high, bool enabled, int tail, abi.Mode mode) {
            double value = double(signed_value) * 2.0d + double(narrow) * 3.0d + wide * 5.0d
                         + double(high) + double(tail) * 7.0d;
            if (enabled && mode == abi.Mode::active) { value = value + 11.0d; }
            return value;
        })");
    const auto call =
        take(take(program->pin()).bind<double, std::int32_t, float, double, std::uint32_t, bool, std::int32_t, Mode>());
    // Independent arithmetic; the first three values use registers, the rest the stack.
    require(take(call.invoke({}, -17, 1.25f, -2.5, 0xf0000001u, true, 9, Mode::active)) == 4026531872.25,
            "Typed scalar ABI changed register/stack positions or numeric representation.");
}

void boolean_alias() {
    Runtime runtime;
    auto program = take(source::Program::create(runtime));
    publish(*program, "bool run(bool copy, const bool& read, bool& write) { write = copy; return read == write && "
                      "write != !copy; }");
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
    const auto typed = take(program->entry().bind<bool, bool, const bool&, bool&>());
    require(take(typed.invoke({}, false, std::as_const(storage.value), storage.value)) && !storage.value &&
                storage.before && storage.after && storage.guard == 0xdeadbeef,
            "Typed reference ABI lost its borrowed alias or overwrote adjacent bytes.");
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
    const std::array start_args{Scalar{2.f}};
    auto task = take(program->entry().start(id, start_args));
    require(take(task.resume()) == CoroutineState::suspended, "Reference coroutine did not suspend.");
    object.reset();
    take(runtime.begin_collection());
    while (runtime.collection_phase() != CollectionPhase::idle)
        take(runtime.collect_step({16}));
    require(take(task.resume()) == CoroutineState::completed,
            "Suspended strong/weak references or adjusted receiver did not complete.");
    const auto task_result = take(task.result());
    require(task_result && std::get<float>(*task_result) == 15,
            "Suspended strong/weak references or adjusted receiver lost identity or data.");
    take(runtime.begin_collection());
    while (runtime.collection_phase() != CollectionPhase::idle)
        take(runtime.collect_step({16}));
    require(runtime.live_objects() == 0 && runtime.root_count() == 0, "Completed frame retained its receiver.");
}

void type_bound_stateless() {
    struct Unrelated {};
    Runtime runtime;
    const auto base = take(NativeRegistration<Counter>{runtime, "abi.Counter", TypeKind::object}.publish());
    const auto derived =
        take(NativeRegistration<Derived>{runtime, "abi.Derived", TypeKind::object}.base<Counter>().publish());
    const auto other_type = take(NativeRegistration<Unrelated>{runtime, "abi.Unrelated", TypeKind::object}.publish());
    auto object = take(runtime.create(derived));
    auto wrong = take(runtime.create(other_type));
    const auto object_id = object.id();
    auto program = take(source::Program::create(runtime, base));
    publish(*program, "int run(int x) { return x * 3; }");
    require(source::detail::ProgramAccess::stateless_direct(take(program->pin())),
            "A bound scalar entry without self must select stateless Typed dispatch.");
    const std::array invoke_args{Scalar{7}};
    const auto invoke_result = take(program->entry().invoke(object_id, invoke_args));
    require(invoke_result && std::get<std::int32_t>(*invoke_result) == 21,
            "Generic dispatch failed to finish with an unused optional receiver.");
    const auto typed = take(program->entry().bind<std::int32_t, std::int32_t>());
    require(take(typed.invoke(object_id, 7)) == 21 && take(typed.invoke({}, 7)) == 21,
            "Stateless Typed::invoke on a type-bound Program rejected a valid derived or empty receiver.");
    const auto mismatch = typed.invoke(wrong.id(), 7);
    require(!mismatch && mismatch.error().code == ErrorCode::type_mismatch,
            "Stateless Typed::invoke accepted an unrelated receiver type.");
    take(runtime.destroy(object_id));
    const auto destroyed = typed.invoke(object_id, 7);
    require(!destroyed && destroyed.error().code == ErrorCode::invalid_object,
            "Stateless Typed::invoke accepted a destroyed receiver.");
}

void weak_acquisition_during_mark() {
    Runtime runtime;
    const auto type = take(runtime.register_type({"abi.Observed", TypeKind::object, {}, {}, {}}));
    auto program = take(source::Program::create(runtime, type));
    publish(*program, "bool run() { weak abi.Observed w = observe(self); return try_acquire(w) == self; }");
    const auto typed = take(program->entry().bind<bool>());
    for (const bool direct : {false, true}) {
        auto owner = take(runtime.create(type));
        const auto id = owner.id();
        const auto weak = take(runtime.observe(owner.reference()));
        owner.reset();
        take(runtime.begin_collection());
        while (runtime.collection_phase() == CollectionPhase::roots)
            take(runtime.collect_step({1}));
        bool acquired = false;
        if (direct)
            acquired = take(typed.invoke(id));
        else {
            const auto generic_res = take(program->entry().invoke(id, Span<const Scalar>{}));
            acquired = generic_res && std::get<bool>(*generic_res);
        }
        while (runtime.collection_phase() != CollectionPhase::idle)
            take(runtime.collect_step({1}));
        require(acquired && take(runtime.try_acquire(weak)).has_value(),
                "A successful synchronous weak acquisition must mark the object for the current cycle.");
        take(runtime.begin_collection());
        while (runtime.collection_phase() != CollectionPhase::idle)
            take(runtime.collect_step({1}));
        require(!take(runtime.try_acquire(weak)), "Synchronous weak acquisition retained a root after return.");
    }
}
}

void verify_business_execution() {
    scalar_abi();
    boolean_alias();
    reference_coroutine();
    type_bound_stateless();
    weak_acquisition_during_mark();
    verify_math_abi();
}
}
