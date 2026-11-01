#include "../helpers.hpp"
#include "source/native_values.hpp"
#include "source/program.hpp"

#include <array>
#include <cstdio>
#include <stdexcept>

namespace {
using namespace atelier;

using namespace atelier::acceptance::borrow;

using namespace atelier::acceptance::helpers;

using source::Program;

void publish(Program& program, std::string_view text) {
    auto candidate = take(Program::prepare(take(program.request("module.source", text, "run"))));
    take(program.try_publish(candidate));
}

struct Object {
    static inline int live{};

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
                                 NativeRegistration<Object>{runtime, "module.Object", TypeKind::object}
                                     .method<&Object::fail>("fail")
                                     .build()}));
    const auto algorithm_type = take(runtime.native_type<Algorithms>());
    const auto host = take(runtime.create(algorithm_type));
    Program owned(runtime, algorithm_type);
    publish(owned, R"(
        void leaf(const Array<borrow.Particle>& input) {
            Array<borrow.Particle> local = input;
            self.fail(local);
        }
        void middle(const Array<borrow.Particle>& input) {
            Array<borrow.Particle> local = input;
            leaf(local);
        }
        void run(const Array<borrow.Particle>& input) {
            Array<borrow.Particle> local = input;
            middle(local);
        }
    )");
    Array<Particle> particles;
    particles.resize(2);
    const std::array args{Argument::borrow(std::as_const(particles))};
    const auto failures = Algorithms::failures;
    require(!owned.entry().invoke(host.id(), args) && Algorithms::failures == failures + 1 && Particle::live == 2 &&
                particles[0].count == 0,
            "Nested native failure leaked owned values or modified the independent input.");
    Particle::copy_budget = 3; // Caller copy succeeds; middle copy fails after one element.
    const auto failed_copy = owned.entry().invoke(host.id(), args);
    Particle::copy_budget = -1;
    require(!failed_copy && Algorithms::failures == failures + 1 && Particle::live == 2 && particles[0].count == 0,
            "Partial helper construction leaked its caller's values.");
    particles.clear();
    require(Particle::live == 0, "Module execution left native values alive.");

    Program objects(runtime, take(runtime.native_type<Object>()));
    publish(objects, R"(
        uint leaf(uint value) { module.Object inner = new module.Object(); inner.fail(); return value; }
        uint run(uint value) { module.Object outer = new module.Object(); return leaf(value); }
    )");
    require(!objects.entry().invoke({}, std::array{Scalar{std::uint32_t{10}}}),
            "Nested object helper did not reach its native failure.");
    take(runtime.begin_collection());
    while (take(runtime.collect_step({1})).phase != CollectionPhase::idle) {
    }
    require(Object::live == 0, "Nested failure retained a caller/callee object root.");
    std::puts("Source modules: PASS (nested native/copy failure and object-root cleanup)");
}
