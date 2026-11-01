#include "source/program.hpp"
#include "../runtime/inheritance_fixture.hpp"
#include <array>
#include <cstdio>

using namespace atelier;

using namespace atelier::acceptance::inheritance;

void verify_source_inheritance() {
    Runtime runtime;
    const auto types = take(runtime.register_types({
        NativeRegistration<Base>{runtime, "inheritance.Base", TypeKind::object}
            .field<&Base::value>("value")
            .method<&Base::bump>("bump")
            .build(),
        NativeRegistration<Derived>{runtime, "inheritance.Derived", TypeKind::object}.base<Base>().build(),
    }));
    auto object = take(runtime.create(types[1]));
    take(runtime.write(object.id(), take(runtime.find_field(types[0], "value")), 13.f));
    source::Program asynchronous(runtime, types[0]);
    auto candidate = take(source::Program::prepare(take(asynchronous.request("inheritance-async.source", R"(
        async float later(float amount) {
            inheritance.Base held = self;
            weak inheritance.Base watched = observe(held);
            yield;
            inheritance.Base acquired = try_acquire(watched);
            if (acquired != self) { return 0.0; }
            return acquired.bump(amount);
        })"))));
    take(asynchronous.try_publish(candidate));
    const auto id = object.id();
    auto task = take(asynchronous.entry().start(id, std::array{Scalar{2.f}}));
    require(take(task.resume()) == CoroutineState::suspended, "Inherited coroutine failed before yield.");
    object.reset();
    collect(runtime);
    require(runtime.type_of(id).has_value(), "Suspended base view did not retain its adjusted receiver.");
    require(take(task.resume()) == CoroutineState::completed && std::get<float>(*take(task.result())) == 15,
            "Inherited frame lost its adjusted base view.");
    collect(runtime);
    require(!runtime.type_of(id), "Completed inherited frame kept its receiver alive.");

    source::Program factory(runtime, types[0]);
    candidate = take(source::Program::prepare(take(factory.request("inheritance-static-new.source", R"(
        float fresh() {
            inheritance.Base created = new inheritance.Base();
            self.value = created.value;
            return self.value;
        })"))));
    take(factory.try_publish(candidate));
    auto derived = take(runtime.create(types[1]));
    require(std::get<float>(*take(factory.entry().invoke(derived.id(), {}))) == 7 && Derived::live == 1,
            "Source new used the receiver's dynamic type rather than its binding.");
    derived.reset();
    collect(runtime);
    require(runtime.live_objects() == 0 && runtime.root_count() == 0, "Source inheritance left objects or roots.");
    std::puts("[pass] Source inheritance: adjusted coroutine receiver and static new");
}
