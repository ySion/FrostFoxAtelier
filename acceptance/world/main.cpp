#include "world/world.hpp"
#include "runtime/native.hpp"
#include "source/program.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace {
using namespace atelier;
namespace w = atelier::world;

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class T> T take(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(*result);
}
void take(Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }
void committed(Result<w::Commit> result) {
    const auto commit = take(std::move(result));
    if (commit.notification_error) throw std::runtime_error(commit.notification_error->message);
}
template<class T> void reject(const Result<T>& result, const char* message) { require(!result, message); }
void collect(Runtime& runtime) {
    if (runtime.collection_phase() == CollectionPhase::idle) take(runtime.begin_collection());
    do { take(runtime.collect_step({16})); } while (runtime.collection_phase() != CollectionPhase::idle);
}
bool near(float actual, float expected) { return std::abs(actual - expected) < 0.0001f; }
struct DerivedCounter : w::Counter { float extra{7.f}; };
struct Holder { ReferenceSlot actor, component; };

TypeId holder_type(Runtime& runtime, const w::Types& types) {
    return take(NativeRegistration<Holder>{runtime, "worldAcceptance.Holder", TypeKind::object}
        .field<&Holder::actor>("actor", {types.actor, ValueForm::strong_reference})
        .field<&Holder::component>("component", {types.component, ValueForm::strong_reference}).publish());
}
void publish(source::Program& program, std::string_view text) {
    auto candidate = take(source::Program::prepare(take(program.request("world-acceptance.source", text))));
    take(program.try_publish(candidate));
}
std::vector<ObjectRoot> add_shape(Runtime& runtime, const w::Types& types, w::World& world, ObjectId actor) {
    std::vector<ObjectRoot> roots;
    roots.push_back(take(w::create_component(runtime, types.transform)));
    roots.push_back(take(w::create_component(runtime, types.cube)));
    const std::array added{w::Attachment{roots[0].id(), {}}, w::Attachment{roots[1].id(), roots[0].id()}};
    committed(world.attach_components(actor, added));
    return roots;
}

void editing_and_execution() {
    Runtime runtime;
    const auto types = take(w::register_types(runtime));
    const auto derived = take(NativeRegistration<DerivedCounter>{runtime, "worldAcceptance.DerivedCounter", TypeKind::object}
        .base<w::Counter>().field<&DerivedCounter::extra>("extra").publish());
    w::World world(runtime, types);
    auto actor = take(w::create_actor(runtime));
    require(take(w::inspect_actor(runtime, actor.id())).state == w::ActorState::detached, "New Actor was not Detached.");
    committed(world.attach(actor.id()));
    require(take(world.members()).size() == 1 && take(w::components(runtime, actor.id())).empty() && take(world.render_items()).empty(),
        "Empty Actor needs a Transform or render component.");
    auto cube = take(w::create_component(runtime, types.cube));
    const std::array invalid{w::Attachment{cube.id(), {}}};
    reject(world.attach_components(actor.id(), invalid), "Cube without Transform was accepted.");
    require(take(w::components(runtime, actor.id())).empty() && take(w::inspect_component(runtime, cube.id())).owner == ObjectId{},
        "Rejected dependency partially published a component.");
    auto transform = take(w::create_component(runtime, types.transform));
    const std::array invalid_last{w::Attachment{transform.id(), {}}, w::Attachment{cube.id(), actor.id()}};
    reject(world.attach_components(actor.id(), invalid_last), "Invalid final binding was accepted.");
    require(take(w::components(runtime, actor.id())).empty() && take(w::inspect_component(runtime, transform.id())).owner == ObjectId{} &&
        take(w::inspect_component(runtime, cube.id())).owner == ObjectId{}, "Late batch failure partially published owners or composition.");
    const std::array duplicate{w::Attachment{transform.id(), {}}, w::Attachment{transform.id(), {}}};
    reject(world.attach_components(actor.id(), duplicate), "Duplicate batch identities were accepted.");
    const std::array added{w::Attachment{transform.id(), {}}, w::Attachment{cube.id(), transform.id()}};
    committed(world.attach_components(actor.id(), added));
    const auto actor_status = take(w::inspect_actor(runtime, actor.id()));
    require(actor_status.state == w::ActorState::attached_inactive && actor_status.can_edit && actor_status.can_present,
        "Editor Actor must be visible/editable without game activation.");
    require(take(world.query(types.component)).size() == 2 && take(world.query(types.component, false)).empty(), "Inherited component query is incorrect.");
    take(world.write(transform.id(), types.position_fields[0], Scalar{4.f}));
    require(std::get<float>(take(world.read(transform.id(), types.position_fields[0]))) == 4.f, "Formal Transform path did not edit the actual object.");
    w::Trs trs{{4.f, 5.f, 6.f}, {0.f, 0.f, std::sqrt(.5f), std::sqrt(.5f)}, {2.f, 3.f, 4.f}};
    take(world.set_transform(transform.id(), trs));
    const auto matrix = take(world.render_items()).front().world;
    require(near(matrix[0], 0.f) && near(matrix[1], 2.f) && near(matrix[4], -3.f) && near(matrix[10], 4.f) && matrix[12] == 4.f,
        "TRS did not produce S*R*T row-vector geometry.");
    const auto original = take(w::read_transform(runtime, transform.id()));
    auto illegal = original; illegal.scale.x = 0.f;
    reject(world.set_transform(transform.id(), illegal), "Zero scale was accepted.");
    illegal.scale.x = -1.f; reject(world.set_transform(transform.id(), illegal), "Negative scale was accepted.");
    illegal = original; illegal.rotation = {0,0,0,0}; reject(world.set_transform(transform.id(), illegal), "Zero quaternion was accepted.");
    require(take(w::read_transform(runtime, transform.id())) == original, "Rejected TRS changed stored values.");
    illegal = original; illegal.scale = {1e-20f, 1e-20f, 1e-20f}; take(world.set_transform(transform.id(), illegal));
    require(take(w::world_matrix(runtime, transform.id()))[10] > 0.f, "Tiny positive scale was rejected or collapsed.");
    illegal = original; illegal.rotation = {0.f, .6f, .8f, 0.f};
    const auto maximum = std::numeric_limits<float>::max();
    illegal.scale = {maximum, maximum, maximum};
    take(world.set_transform(transform.id(), illegal));
    require(std::ranges::all_of(take(w::world_matrix(runtime, transform.id())), [](float value) { return std::isfinite(value); }),
        "Quaternion storage rounding overflowed a finite positive scale.");
    take(world.set_transform(transform.id(), original));
    reject(world.remove_component(transform.id()), "Transform with a dependent Cube was removed.");
    auto removed = take(world.remove_component(cube.id()));
    collect(runtime);
    require(take(world.render_items()).empty() && take(w::inspect_component(runtime, cube.id())).owner == ObjectId{}, "Cube removal retained participation or owner.");
    committed(world.restore(removed));
    require(take(world.render_items()).size() == 1, "Component undo did not restore drawing.");
    auto counter = take(w::create_component(runtime, derived));
    const std::array counter_added{w::Attachment{counter.id(), {}}};
    committed(world.attach_components(actor.id(), counter_added));
    require(take(world.query(types.counter)).size() == 1 && take(world.query(types.counter, false)).empty(), "Derived Counter was not indexed by its actual type.");
    source::Program program(runtime, types.counter);
    publish(program, "void step(float amount) { self.advance(amount); }");
    const auto entry = take(program.bind<void, float>());
    take(world.counter_update([entry](Runtime&, ObjectId receiver, float amount) -> Result<void> {
        const std::array arguments{Scalar{amount}};
        auto result = entry.invoke(receiver, arguments);
        if (!result) return std::unexpected(result.error());
        return {};
    }));
    take(world.update(10.f));
    require(std::get<float>(take(runtime.read(counter.id(), types.counter_value))) == 0.f, "Editor World ran game code.");
    committed(world.start());
    take(world.update(2.f));
    require(take(w::inspect_actor(runtime, actor.id())).state == w::ActorState::active &&
        std::get<float>(take(runtime.read(counter.id(), types.counter_value))) == 2.f, "JIT did not update the same derived Counter.");
    committed(world.set_enabled(counter.id(), false)); take(world.update(7.f));
    require(std::get<float>(take(runtime.read(counter.id(), types.counter_value))) == 2.f && !take(w::inspect_component(runtime, counter.id())).active,
        "Disabled component still ran.");
    require(take(w::inspect_actor(runtime, actor.id())).state == w::ActorState::active && take(world.render_items()).size() == 1,
        "Component activation was confused with Actor activation or presentation.");
    committed(world.set_enabled(counter.id(), true));
    source::Program deferred(runtime, types.counter);
    publish(deferred, "async void later() { yield; self.advance(50.0); }");
    const auto later = take(deferred.bind<void>(FunctionExecution::coroutine));
    take(world.start_task(counter.id(), [later](Runtime&, ObjectId receiver) { return later.start(receiver, {}); }));
    require(world.activity().tasks == 1, "World did not own the real coroutine.");
    take(world.update(1.f)); // task suspends after its yield
    committed(world.set_enabled(actor.id(), false));
    require(world.activity().tasks == 0 && world.activity().update_registrations == 0, "Actor deactivation did not cancel its task/registration.");
    take(world.update(1.f));
    require(std::get<float>(take(runtime.read(counter.id(), types.counter_value))) == 3.f, "Cancelled coroutine resumed or inactive Actor ran.");
    std::size_t phantom_activation{};
    take(world.observe_lifecycle([&](w::World&, w::Lifecycle event, ObjectId target) -> Result<void> {
        if (event == w::Lifecycle::activated && target == counter.id()) ++phantom_activation;
        return {};
    }));
    committed(world.set_enabled(counter.id(), false)); committed(world.set_enabled(counter.id(), true));
    require(phantom_activation == 0 && !take(w::inspect_component(runtime, counter.id())).active,
        "Component emitted activation while its Actor was inactive.");
    take(world.observe_lifecycle({}));
    committed(world.set_enabled(actor.id(), true));
    committed(world.stop());
    require(take(w::inspect_actor(runtime, actor.id())).state == w::ActorState::attached_inactive && take(world.render_items()).size() == 1,
        "World.stop removed editor presentation or membership.");
    auto removal = take(world.detach(actor.id()));
    const auto actor_id = actor.id(), transform_id = transform.id(), cube_id = cube.id(), counter_id = counter.id();
    actor.reset(); transform.reset(); cube.reset(); counter.reset(); removed.root.reset();
    require(runtime.root_count() == 1, "Delete/undo sample still has roots outside its Actor removal record.");
    collect(runtime);
    require(take(w::inspect_actor(runtime, actor_id)).state == w::ActorState::detached && take(world.render_items()).empty(), "Actor detach left participation.");
    reject(world.write(transform_id, types.position_fields[0], Scalar{99.f}), "Detached data was edited through World.");
    committed(world.restore(removal));
    actor = take(runtime.hold(actor_id)); transform = take(runtime.hold(transform_id));
    cube = take(runtime.hold(cube_id)); counter = take(runtime.hold(counter_id));
    require(take(world.members()).front().id() == actor.id() && take(w::read_transform(runtime, transform.id())) == original,
        "Delete/GC/undo lost identity or data without an offline writer.");
    committed(world.start());
    take(world.start_task(counter.id(), [later](Runtime&, ObjectId receiver) { return later.start(receiver, {}); }));
    take(world.update(0.f));
    require(world.activity().tasks == 1, "Close sample did not contain a suspended real task.");
    committed(world.close());
    require(world.activity().tasks == 0 && world.activity().update_registrations == 0 && world.activity().presentation_registrations == 0,
        "Close retained real execution/presentation registrations or task frames.");
    committed(w::destroy_actor(runtime, actor.id()));
    require(take(w::inspect_actor(runtime, actor.id())).state == w::ActorState::invalid, "Explicit destroy did not invalidate a strongly held Actor.");
    reject(world.restore(removal), "Closed World was revived by history.");
    std::puts("[pass] World editor and JIT: empty/derived components, dependencies, TRS, four states, real task cancellation and identity undo");
}

void reference_lifetime() {
    Runtime runtime;
    const auto types = take(w::register_types(runtime));
    const auto holder = holder_type(runtime, types);
    auto keeper = take(runtime.create(holder));
    const auto actor_field = take(runtime.find_field(holder, "actor"));
    const auto component_field = take(runtime.find_field(holder, "component"));
    ObjectId actor_id, cube_id, transform_id;
    {
        w::World world(runtime, types);
        auto actor = take(w::create_actor(runtime)); actor_id = actor.id();
        committed(world.attach(actor_id));
        auto shape = add_shape(runtime, types, world, actor_id);
        transform_id = shape[0].id(); cube_id = shape[1].id();
        take(world.write(transform_id, types.position_fields[1], Scalar{23.f}));
        take(runtime.write_reference(keeper.id(), actor_field, actor.reference()));
        auto removed = take(world.detach(actor_id));
        removed.root.reset(); actor.reset(); shape.clear();
        collect(runtime);
        require(take(w::components(runtime, actor_id)).size() == 2 && runtime.type_of(cube_id).has_value(), "Ordinary Actor reference did not retain the detached composition.");
        auto cube = take(runtime.hold(cube_id, types.component));
        take(runtime.write_reference(keeper.id(), component_field, cube.reference()));
        take(runtime.write_reference(keeper.id(), actor_field, take(runtime.null_reference(types.actor))));
        cube.reset(); collect(runtime);
        require(runtime.type_of(actor_id).has_value() && runtime.type_of(transform_id).has_value(), "Component strong owner did not retain Actor and siblings.");
        committed(world.attach(actor_id));
        committed(world.close());
    }
    collect(runtime);
    require(take(w::inspect_actor(runtime, actor_id)).state == w::ActorState::detached && take(w::components(runtime, actor_id)).size() == 2,
        "Destroying the old World invalidated retained data.");
    require(take(w::read_transform(runtime, transform_id)).position.y == 23.f, "World close lost retained component field values.");
    committed(w::destroy_actor(runtime, actor_id));
    require(!runtime.type_of(actor_id) && !runtime.type_of(cube_id) && !runtime.type_of(transform_id), "Detached destruction did not terminate the complete group.");
    reject(w::destroy_actor(runtime, actor_id), "Repeated destroy resurrected or accepted a stale identity.");
    require(take(w::inspect_component(runtime, cube_id)).valid == false, "Destroyed component remained valid.");
    keeper.reset(); collect(runtime);
    require(runtime.live_objects() == 0 && runtime.root_count() == 0, "Destruction retained hidden roots.");

    // Independent sample: no explicit destroy, including the Actor<->Component cycle.
    {
        w::World world(runtime, types);
        auto actor = take(w::create_actor(runtime)); actor_id = actor.id();
        committed(world.attach(actor_id));
        auto shape = add_shape(runtime, types, world, actor_id);
        committed(world.close());
        actor.reset(); shape.clear();
    }
    collect(runtime);
    require(runtime.live_objects() == 0 && runtime.root_count() == 0 && take(w::inspect_actor(runtime, actor_id)).state == w::ActorState::invalid,
        "Unreachable strong composition cycle did not collect.");
    std::puts("[pass] World lifetime: ordinary Actor/Component fields retain the graph, close and old-World destruction, explicit termination, independent GC");
}

void worlds_and_requests() {
    Runtime runtime, other;
    const auto types = take(w::register_types(runtime)), foreign_types = take(w::register_types(other));
    w::World first(runtime, types), second(runtime, types);
    auto actor = take(w::create_actor(runtime));
    auto foreign = take(w::create_actor(other));
    auto scalar_object = take(runtime.create(runtime.object_type()));
    reject(w::inspect_actor(runtime, foreign.id()), "Foreign Runtime identity was classified as local Invalid.");
    reject(w::inspect_actor(runtime, scalar_object.id()), "Non-Actor object was classified as an Actor.");
    require(take(w::inspect_actor(runtime, {})).state == w::ActorState::invalid, "Null identity did not classify as Invalid.");
    committed(first.attach(actor.id()));
    auto shape = add_shape(runtime, types, first, actor.id());
    reject(second.attach(actor.id()), "Direct transfer of an attached Actor succeeded.");
    require(take(w::inspect_actor(runtime, actor.id())).state == w::ActorState::attached_inactive, "Foreign World attempt invalidated the Actor.");
    const auto pending = take(first.request({w::RequestKind::destroy_actor, actor.id()}));
    require(runtime.type_of(actor.id()).has_value(), "Queued destroy terminated identity before commit.");
    auto removed = take(first.detach(actor.id()));
    committed(second.attach(actor.id()));
    const auto rejected = take(first.flush());
    require(rejected.size() == 1 && rejected[0].request == pending && !rejected[0].result && runtime.type_of(actor.id()).has_value(),
        "Old World queued destroy followed the Actor into a different World.");
    reject(first.restore(removed), "History stole the Actor from another World.");
    committed(first.close());
    require(take(second.members()).size() == 1 && take(second.render_items()).size() == 1, "Closing one World affected another.");
    const auto destruction = take(second.request({w::RequestKind::destroy_actor, actor.id()}));
    auto destroyed = take(second.flush());
    require(destroyed.size() == 1 && destroyed[0].request == destruction && destroyed[0].result && !runtime.type_of(actor.id()),
        "Queued destruction did not commit through the object protocol.");
    auto fresh = take(w::create_actor(runtime));
    require(fresh.id() != actor.id(), "Destroyed identity was reused without a new generation.");
    reject(second.attach(actor.id()), "Stale Actor identity was restored.");
    reject(first.request({w::RequestKind::attach_actor, fresh.id()}), "Closed World accepted new work.");
    committed(second.close());
    (void)foreign_types;
    std::puts("[pass] World isolation: context/type errors, detached reattachment, history provenance and queued identity/World revalidation");
}

void failures_and_reentry() {
    Runtime runtime;
    const auto types = take(w::register_types(runtime));
    w::World world(runtime, types);
    auto actor = take(w::create_actor(runtime));
    bool callback_called = false;
    take(world.observe_lifecycle([&](w::World& current, w::Lifecycle event, ObjectId target) -> Result<void> {
        if (event != w::Lifecycle::attached) return {};
        callback_called = true;
        reject(current.detach(target), "Lifecycle callback reentered structural commit.");
        reject(w::destroy_actor(runtime, target), "Object destruction reentered a lifecycle callback.");
        take(current.request({w::RequestKind::detach_actor, target}));
        return std::unexpected(Error{ErrorCode::invocation_failed, "Intentional lifecycle consumer failure after commit."});
    }));
    const auto result = take(world.attach(actor.id()));
    require(callback_called && result.changed && result.notification_error && take(world.members()).size() == 1,
        "Post-commit failure was reported as an uncommitted attach.");
    auto completion = take(world.flush());
    require(completion.size() == 1 && completion[0].result && take(w::inspect_actor(runtime, actor.id())).state == w::ActorState::detached,
        "Deferred callback request did not execute at the next boundary.");
    take(world.observe_lifecycle({})); committed(world.attach(actor.id()));
    std::size_t close_callbacks{};
    take(world.observe_lifecycle([&](w::World& current, w::Lifecycle event, ObjectId target) -> Result<void> {
        if (event == w::Lifecycle::detached) {
            ++close_callbacks;
            reject(current.start(), "Closing World restarted from an exit callback.");
            reject(current.request({w::RequestKind::destroy_actor, target}), "Closing World accepted a request.");
            reject(w::inspect_actor(runtime, target), "Close exposed a partially transitioned Actor.");
        }
        return {};
    }));
    committed(world.close());
    require(close_callbacks == 1 && world.activity().tasks == 0 && world.activity().presentation_registrations == 0 &&
        !take(world.close()).changed, "Close was not complete/idempotent.");
    committed(w::destroy_actor(runtime, actor.id()));
    std::puts("[pass] World failure boundaries: committed notification failure, deferred reentry, closed admission and no resurrection");
}
}

int main() {
    try {
        editing_and_execution();
        reference_lifetime();
        worlds_and_requests();
        failures_and_reentry();
        std::puts("World acceptance: PASS (4/4)");
        return 0;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "World acceptance: FAIL: %s\n", exception.what());
        return 1;
    }
}
