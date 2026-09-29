#include "source/program.hpp"
#include "../runtime/inheritance_fixture.hpp"
#include <array>
#include <cstdio>

using namespace atelier;
using namespace atelier::acceptance::inheritance;

namespace {
void publish(source::Program& program, std::string_view name, std::string_view body) {
    auto candidate = take(source::Program::prepare(take(program.request(name, body))));
    take(program.try_publish(candidate));
}
float scalar(std::optional<Scalar> result) {
    require(result && std::holds_alternative<float>(*result), "Inherited source result was not a float.");
    return std::get<float>(*result);
}
}

void verify_source_inheritance() {
    Runtime runtime;
    const auto types = register_types(runtime);
    const auto value = take(runtime.find_field(types.base, "value"));
    source::Program base(runtime, types.base);
    publish(base, "inheritance-sync.source", R"(
        float step(float amount) {
            inheritance.Base held = self;
            weak inheritance.Base watched = observe(held);
            inheritance.Base acquired = try_acquire(watched);
            if (acquired != self) { return 0.0; }
            return acquired.bump(amount);
        })");
    const auto bound = take(base.bind<float, float>());
    auto previous = take(base.pin());
    publish(base, "inheritance-sync-replacement.source", "float step(float amount) { return self.bump(amount + 1.0); }");
    auto unrelated = take(runtime.create(types.leaf));
    const auto roots = runtime.root_count();
    const std::array args{Scalar{2.f}};
    reject(bound.invoke(unrelated.id(), args), ErrorCode::type_mismatch);
    require(runtime.root_count() == roots, "Rejected inherited source receiver leaked a root.");
    for (const auto type : {types.derived, types.described_native, types.grand}) {
        auto object = take(runtime.create(type));
        require(scalar(take(previous.invoke(object.id(), args))) == 9 &&
            scalar(take(bound.invoke(object.id(), args))) == 12,
            "Base entry/lease failed on a derived native or described/native receiver.");
        const auto* info = take(runtime.describe(type));
        source::Program derived(runtime, type);
        publish(derived, "inheritance-derived.source", "float read() { return self.bump(1.0) + self.extra; }");
        take(runtime.write(object.id(), take(runtime.find_field(type, "extra")), 40.f));
        require(scalar(take(derived.entry().invoke(object.id(), {}))) == 53,
            "Derived source did not bind inherited declarations and own fields.");
        require(info->base == (type == types.grand ? types.derived : types.base), "Source binding mutated type ancestry.");

        source::Program asynchronous(runtime, types.base);
        publish(asynchronous, "inheritance-async.source", R"(
            async float later(float amount) {
                inheritance.Base held = self;
                weak inheritance.Base watched = observe(held);
                yield;
                inheritance.Base acquired = try_acquire(watched);
                if (acquired != self) { return 0.0; }
                return acquired.bump(amount);
            })");
        auto entry = take(asynchronous.bind<float, float>(FunctionExecution::coroutine));
        reject(entry.start(unrelated.id(), args), ErrorCode::type_mismatch);
        const auto id = object.id();
        auto task = take(entry.start(id, args));
        require(take(task.resume()) == CoroutineState::suspended, "Inherited coroutine failed before yield.");
        const auto old = take(asynchronous.pin());
        publish(asynchronous, "inheritance-async-update.source", "async float later(float amount) { yield; return self.bump(amount + 10.0); }");
        object.reset(); collect(runtime);
        require(runtime.type_of(id).has_value(), "Suspended base view did not retain the derived receiver.");
        require(take(task.resume()) == CoroutineState::completed && scalar(take(task.result())) == 15,
            "Suspended inherited frame lost its view or old code version.");
        auto old_task = take(old.start(id, args));
        auto new_task = take(entry.start(id, args));
        take(old_task.resume()); take(new_task.resume()); collect(runtime);
        require(take(old_task.resume()) == CoroutineState::completed && scalar(take(old_task.result())) == 17,
            "Pinned inherited coroutine used replacement code.");
        require(take(new_task.resume()) == CoroutineState::completed && scalar(take(new_task.result())) == 29,
            "Updated inherited coroutine used stale code.");
        require(std::get<float>(take(runtime.read(id, value))) == 29, "Source changed a copied object.");
        collect(runtime);
        require(!runtime.type_of(id), "Completed inherited frames kept their receiver alive.");
    }
    // Binding controls new's type even when the receiver is derived.
    source::Program factory(runtime, types.base);
    publish(factory, "inheritance-static-new.source", R"(
        float fresh() {
            inheritance.Base created = new inheritance.Base();
            self.value = created.value;
            return self.value;
        })");
    auto derived = take(runtime.create(types.derived));
    const auto derived_constructions = Derived::live;
    require(scalar(take(factory.entry().invoke(derived.id(), {}))) == 7 && Derived::live == derived_constructions,
        "Source new used the receiver's dynamic type rather than its binding.");
    // The source declaration view cannot see derived-only members.
    source::Program static_view(runtime, types.base);
    auto rejected = source::Program::prepare(take(static_view.request("inheritance-hidden.source", "float read() { return self.extra; }")));
    require(!rejected, "Base source directory exposed a derived-only field.");
    derived.reset(); unrelated.reset(); collect(runtime);
    require(runtime.live_objects() == 0 && runtime.root_count() == 0, "Source inheritance left objects or roots.");
    std::puts("[pass] Source inheritance: shared sync/async receiver views, weak promotion, inherited methods, old code and static new");
}
