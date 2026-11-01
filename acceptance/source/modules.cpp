#include "../helpers.hpp"
#include "source/native_values.hpp"
#include "source/program.hpp"

#include <array>
#include <cstdio>
#include <stdexcept>

namespace {
using namespace atelier;

using source::Program;

using acceptance::borrow::Particle;

using acceptance::borrow::Position;

using acceptance::borrow::Algorithms;

using namespace atelier::acceptance::helpers;

void publish(Program& program, std::string_view text, std::string_view entry = {}) {
    auto candidate = take(Program::prepare(take(program.request("module.source", text, entry))));
    take(program.try_publish(candidate));
}

std::uint32_t number(const std::optional<Scalar>& value) {
    return std::get<std::uint32_t>(*value);
}

void collect(Runtime& runtime) {
    while (runtime.collection_phase() != CollectionPhase::idle)
        take(runtime.collect_step({1}));
    take(runtime.begin_collection());
    while (take(runtime.collect_step({1})).phase != CollectionPhase::idle) {
    }
}

struct Object {
    static inline int live{};

    std::uint32_t value{};

    Object() { ++live; }

    ~Object() { --live; }

    void fail() { throw std::runtime_error("Module native failure."); }
};
}

void verify_source_modules() {
    Runtime runtime;
    auto particle = reflection::Definition<Particle>::build(runtime);
    particle.native.value_operations_preserve_borrows = true;
    take(runtime.register_types({std::move(particle), reflection::Definition<Position>::build(runtime),
                                 reflection::Definition<Algorithms>::build(runtime), string_definition(runtime),
                                 array_definition<Particle>(runtime, "module.Particles"),
                                 array_definition<float>(runtime, "module.Floats"),
                                 array_definition<std::int32_t>(runtime, "module.Ints"),
                                 NativeRegistration<Object>{runtime, "module.Object", TypeKind::object}
                                     .field<&Object::value>("value")
                                     .method<&Object::fail>("fail")
                                     .build()}));
    const auto object_type = take(runtime.native_type<Object>());
    const auto algorithm_type = take(runtime.native_type<Algorithms>());

    Program arithmetic(runtime, {});
    publish(arithmetic, R"(
        uint run() { return add(add(1, 2), add(4, 8)); }
        uint add(uint first, uint second) { return first + second; }
    )",
            "run");
    require(number(take(arithmetic.entry().invoke({}, std::array<Scalar, 0>{}))) == 15,
            "Forward/nested source calls overwrote earlier operands.");
    const auto old = take(arithmetic.pin());
    publish(arithmetic, R"(
        uint helper(const uint& first, const uint& second, uint third) { return first + second + third; }
        uint run() { return helper(7, helper(1, 2, 3), 9); }
    )");
    require(number(take(arithmetic.entry().invoke({}, std::array<Scalar, 0>{}))) == 22 &&
                number(take(old.invoke({}, std::array<Scalar, 0>{}))) == 15,
            "Module replacement did not retain private helper signatures and code per version.");

    Program arrays(runtime, {});
    publish(arrays, R"(
        float add(float left, float right) { return left + right; }
        float sum(const Array<float>& values) {
            float result = 0.0; uint index = 0;
            while (index < values.size()) { result = add(result, values[index]); index = index + 1; }
            return result;
        }
        float process(const Array<float>& input) {
            Array<float> working = input; working.add(1.0);
            return sum(working);
        }
    )",
            "process");
    Array<float> input;
    input.add(2.0);
    input.add(3.0);
    const std::array input_args{Argument::borrow(std::as_const(input))};
    require(std::get<float>(*take(arrays.entry().invoke({}, input_args))) == 6.0f && input.size() == 2,
            "Owned Array did not survive a source const-reference helper call.");

    Program references(runtime, algorithm_type);
    publish(references, R"(
        int bump(int& value, const int& alias) { value = value + 2; return alias; }
        uint text(const String& value) { return self.text_size(value); }
        uint byte(const uint& value) { return value; }
        int run(int& value) {
            int snapshot = bump(value, value);
            Array<int> local; local.add(snapshot);
            int result = bump(local[0], local[0]);
            if (text("literal/argument") != 16) { return -1; }
            if (byte("x"[0]) != 120) { return -2; }
            return result;
        }
    )",
            "run");
    auto host = take(runtime.create(algorithm_type));
    std::int32_t integer = 1;
    const std::array refs{Argument::borrow(integer)};
    require(std::get<std::int32_t>(*take(references.entry().invoke(host.id(), refs))) == 5 && integer == 3,
            "Source references lost aliases, scalar temporaries or a transitive receiver.");

    Program owned(runtime, algorithm_type);
    publish(owned, R"(
        uint leaf(Array<borrow.Particle>& output, const Array<borrow.Particle>& input, uint mode) {
            Array<borrow.Particle> local = input;
            String label = "owned/火花/long-text-for-heap";
            uint count = self.process(output, local, label);
            if (mode == 1) { self.fail(local); }
            if (mode == 2) { return local[local.size()].name.size(); }
            return count;
        }
        uint middle(Array<borrow.Particle>& output, const Array<borrow.Particle>& input, uint mode) {
            Array<borrow.Particle> local = input;
            uint result = leaf(output, local, mode);
            return result;
        }
        uint run(const Array<borrow.Particle>& input, uint mode) {
            Array<borrow.Particle> local = input;
            return middle(local, input, mode);
        }
    )",
            "run");
    Array<Particle> particles;
    particles.resize(2);
    const auto baseline = Particle::live;
    for (std::uint32_t mode = 0; mode < 3; ++mode) {
        const std::array args{Argument::borrow(std::as_const(particles)), Argument{Scalar{mode}}};
        const auto result = owned.entry().invoke(host.id(), args);
        require(mode == 0 ? result && number(*result) == 2 : !result && !result.error().message.empty(),
                "A deep source call lost its result or native/bounds failure.");
        if (mode != 0) {
            const auto cause =
                mode == 1 ? "Native owned-value failure." : "Borrowed Array/String index is out of range.";
            require(result.error().code == ErrorCode::invocation_failed && result.error().message == cause,
                    "A deep source call replaced the original failure code or diagnostic.");
        }
        require(Particle::live == baseline && particles[0].count == 0,
                "Nested source failure or early return leaked owned values or modified an independent input.");
    }
    Particle::copy_budget = 3; // caller copy succeeds; middle copy fails partway
    const std::array copy_args{Argument::borrow(std::as_const(particles)), Argument{Scalar{std::uint32_t{0}}}};
    auto failed_copy = owned.entry().invoke(host.id(), copy_args);
    Particle::copy_budget = -1;
    require(!failed_copy && failed_copy.error().code == ErrorCode::construction_failed &&
                failed_copy.error().message == "Injected native value copy failure." && Particle::live == baseline,
            "Partial helper construction lost its failure cause or leaked its caller's values.");
    particles.clear();

    Program objects(runtime, object_type);
    publish(objects, R"(
        uint leaf(uint value) { module.Object inner = new module.Object(); inner.value = value + 1; return inner.value; }
        uint middle(uint value) { module.Object inner = new module.Object(); inner.value = leaf(value); return inner.value; }
        uint run(uint value) {
            module.Object outer = new module.Object(); outer.value = value;
            uint result = middle(value);
            uint again = middle(value + 1);
            return outer.value + result + again;
        }
    )",
            "run");
    take(runtime.begin_collection());
    const std::array object_args{Scalar{std::uint32_t{10}}};
    require(number(take(objects.entry().invoke({}, object_args))) == 33,
            "A callee cleared caller root slots or lost objects across nested safe points.");
    collect(runtime);
    require(Object::live == 0, "Module return retained object roots.");
    publish(objects, R"(
        uint leaf(uint value) { module.Object inner = new module.Object(); inner.fail(); return value; }
        uint run(uint value) { module.Object outer = new module.Object(); return leaf(value); }
    )");
    const auto object_failure = objects.entry().invoke({}, object_args);
    require(!object_failure && object_failure.error().code == ErrorCode::invocation_failed &&
                object_failure.error().message == "Module native failure.",
            "Nested object helper lost its native failure cause.");
    collect(runtime);
    require(Object::live == 0, "Nested failure retained a caller/callee object root.");

    Program asynchronous(runtime, object_type);
    publish(asynchronous, R"(
        uint helper(uint value) { module.Object temp = new module.Object(); temp.value = value + 1; return temp.value; }
        async uint run(uint value) {
            module.Object retained = new module.Object(); retained.value = helper(value);
            yield;
            return retained.value + helper(value);
        }
    )",
            "run");
    const auto old_async = take(asynchronous.pin());
    auto task = take(asynchronous.entry().start({}, object_args));
    require(take(task.resume()) == CoroutineState::suspended,
            "Module coroutine did not yield after its synchronous helper.");
    collect(runtime);
    require(Object::live == 1, "A completed helper root survived suspension or cleared its caller.");
    publish(asynchronous, R"(
        async uint run(uint value) { yield; return helper(value); }
        uint helper(uint value) { return value + 100; }
    )");
    require(take(task.resume()) == CoroutineState::completed && number(take(task.result())) == 22,
            "Suspended entry called a helper from a newer module version.");
    auto fresh = take(asynchronous.entry().start({}, object_args));
    take(fresh.resume());
    require(take(fresh.resume()) == CoroutineState::completed && number(take(fresh.result())) == 110,
            "New coroutine did not select the new complete module.");
    auto cancelled = take(old_async.start({}, object_args));
    take(cancelled.resume());
    collect(runtime);
    require(Object::live == 1, "Ready helper cleanup did not isolate the suspended caller.");
    cancelled.cancel();
    collect(runtime);
    require(Object::live == 0 && Particle::live == 0, "Module execution left native resources alive.");

    Program unused(runtime, algorithm_type);
    publish(unused, "uint run() { return 9; } uint spare(const String& text) { return self.text_size(text); }", "run");
    require(number(take(unused.entry().invoke({}, std::array<Scalar, 0>{}))) == 9,
            "An unreachable helper changed the selected entry's receiver/effect requirements.");
    auto switched = Program::prepare(take(
        arithmetic.request("switched-entry.source", "uint run() { return 1; } uint other() { return 2; }", "other")));
    require(!switched && number(take(arithmetic.entry().invoke({}, std::array<Scalar, 0>{}))) == 22,
            "An explicit entry switch changed a frozen public declaration.");

    Program invalid(runtime, algorithm_type);
    constexpr std::array rejected{
        "uint run() { return missing(); }",
        "uint run() { return run(); }",
        "uint run() { return other(); } uint other() { return run(); }",
        "uint run() { return 1; } uint run(uint value) { return value; }",
        "uint run() { return other(); } uint other(uint value) { return value; }",
        "uint run() { return other(1); } uint other(uint& value) { return value; }",
        "void run(const Array<float>& values) { other(values); } void other(Array<float>& values) {}",
        "void run(Array<float>& values) { other(values); } void other(Array<float>& values) { values.clear(); }",
        "uint run() { return other(); } async uint other() { yield; return 1; }",
        "void run(const Array<float>& values) { other(); } void other() { self.unsafe(); }",
        "void run(const Array<float>& values) { other(); } void other() { borrow.Algorithms object = new borrow.Algorithms(); }",
        "async void run() { other(); yield; } void other() { String owned; }"};
    for (const auto* text : rejected)
        require(!Program::prepare(take(invalid.request("invalid-module.source", text, "run"))),
                "Invalid module call, signature or transitive effect was admitted.");
    require(!Program::prepare(
                take(invalid.request("ambiguous-module.source", "uint a() { return 1; } uint b() { return 2; }"))),
            "Multiple functions silently chose a source-order entry.");
    require(!Program::prepare(take(invalid.request("missing-entry.source", "uint a() { return 1; }", "run"))),
            "Unknown selected entry was admitted.");
    require(
        !Program::prepare(take(objects.request(
            "field-borrow.source",
            "uint helper(const uint& value) { return value; } uint run(uint value) { return helper(self.value); }"))),
        "A Runtime object field escaped protected native-call borrowing into source execution.");
    std::puts(
        "Source modules: PASS (forward/nested calls, refs, transitive effects, scoped cleanup, GC, async helpers, whole-version publication)");
}
