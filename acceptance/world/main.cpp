#include "presentation/presentation.hpp"
#include "runtime/native.hpp"
#include "source/program.hpp"
#include <mimalloc.h>
#include <mimalloc-stats.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <tuple>

namespace {
using namespace atelier;
namespace w = atelier::world; namespace p = atelier::presentation;

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
struct Counter : w::Component {
    float value{};
    std::uint32_t steps{};
    void advance(float amount) noexcept { value += amount; ++steps; }
};
struct DerivedCounter : Counter { float extra{7.f}; };
struct Holder { ReferenceSlot entity, component; };

TypeId holder_type(Runtime& runtime, const w::Types& types) {
    return take(NativeRegistration<Holder>{runtime, "worldAcceptance.Holder", TypeKind::object}
        .field<&Holder::entity>("entity", {types.entity, ValueForm::strong_reference})
        .field<&Holder::component>("component", {types.component, ValueForm::strong_reference}).publish());
}
// World and presentation types, as hosts register them.
struct AllTypes : w::Types, p::Types {};
AllTypes register_all_types(Runtime& runtime) {
    AllTypes types;
    static_cast<w::Types&>(types) = take(w::register_types(runtime));
    static_cast<p::Types&>(types) = take(p::register_types(runtime));
    return types;
}
// Mesh and light registrations of the Scene, or those naming `object` as component or owner.
std::size_t presented(const w::World& world, const p::Scene& scene, ObjectId object = {}) {
    std::size_t count{};
    for (const auto id : {scene.meshes(), scene.lights()})
        for (const auto& member : take(world.members(id)))
            count += object == ObjectId{} || member.component == object || member.entity == object;
    return count;
}
struct TestTypes : AllTypes { TypeId counter; FieldId counter_value; };
TestTypes register_test_types(Runtime& runtime) {
    TestTypes types;
    static_cast<AllTypes&>(types) = register_all_types(runtime);
    types.counter = take(NativeRegistration<Counter>{runtime, "worldAcceptance.Counter", TypeKind::object}
        .base<w::Component>().field<&Counter::value>("value").field<&Counter::steps>("steps")
        .method<&Counter::advance>("advance").publish());
    types.counter_value = take(runtime.find_field(types.counter, "value"));
    return types;
}
void publish(source::Program& program, std::string_view text) {
    auto candidate = take(source::Program::prepare(take(program.request("world-acceptance.source", text))));
    take(program.try_publish(candidate));
}
std::vector<ObjectRoot> add_shape(Runtime& runtime, const AllTypes& types, w::World& world, ObjectId entity) {
    std::vector<ObjectRoot> roots;
    roots.push_back(take(w::create_component(runtime, types.component)));
    roots.push_back(take(w::create_component(runtime, types.static_mesh_component)));
    take(world.set_transform(entity, {}));
    const std::array added{w::Attachment{roots[0].id()}, w::Attachment{roots[1].id()}};
    committed(world.attach_components(entity, added));
    take(p::set_geometry(runtime, roots[1].id(), geometry::cube_mesh()));
    return roots;
}

void editing_and_execution() {
    Runtime runtime;
    const auto types = register_test_types(runtime);
    const auto derived = take(NativeRegistration<DerivedCounter>{runtime, "worldAcceptance.DerivedCounter", TypeKind::object}
        .base<Counter>().field<&DerivedCounter::extra>("extra").publish());
    w::World world(runtime, types);
    const auto scene = take(p::Scene::create(world, types));
    auto entity = take(w::create_entity(runtime));
    require(take(w::inspect_entity(runtime, entity.id())).state == w::EntityState::detached, "New Entity was not Detached.");
    committed(world.attach(entity.id()));
    require(take(world.members()).size() == 1 && take(w::components(runtime, entity.id())).empty() && take(scene.render_items()).empty(),
        "Empty Entity needs a Transform or render component.");
    auto cube = take(w::create_component(runtime, types.static_mesh_component));
    require(!take(w::has_transform(runtime, entity.id())), "Empty Entity silently acquired spatial identity.");
    const std::array invalid_last{w::Attachment{cube.id()}, w::Attachment{entity.id()}};
    reject(world.attach_components(entity.id(), invalid_last), "An Entity was accepted as a capability component.");
    require(take(w::components(runtime, entity.id())).empty() && take(w::inspect_component(runtime, cube.id())).owner == ObjectId{},
        "Late batch failure partially published composition.");
    auto collision = take(w::create_component(runtime, types.collision_component));
    const std::array duplicate{w::Attachment{collision.id()}, w::Attachment{collision.id()}};
    reject(world.attach_components(entity.id(), duplicate), "Duplicate batch identities were accepted.");
    take(world.set_transform(entity.id(), {}));
    const std::array added{w::Attachment{collision.id()}, w::Attachment{cube.id()}};
    committed(world.attach_components(entity.id(), added));
    take(p::set_geometry(runtime, cube.id(), geometry::cube_mesh()));
    const auto entity_status = take(w::inspect_entity(runtime, entity.id()));
    require(entity_status.state == w::EntityState::attached_inactive && entity_status.can_edit && presented(world, scene, entity.id()),
        "Editor Entity must be visible/editable without game activation.");
    require(take(world.query(types.component)).size() == 2 && take(world.query(types.component, false)).empty(), "Inherited component query is incorrect.");
    take(world.write(entity.id(), types.position_fields[0], Scalar{4.f}));
    require(std::get<float>(take(world.read(entity.id(), types.position_fields[0]))) == 4.f, "Formal Transform path did not edit the actual object.");
    // Whole-value writes cannot replace a Transform record around set_transform.
    const math::Vector3 bypass{7.f, 0.f, 0.f};
    reject(world.write(entity.id(), std::span(types.position_fields[0]).first(1), ValueInput{types.vector3, &bypass}),
        "A whole-value write replaced a Transform record.");
    require(take(w::read_transform(runtime, entity.id())).position.x == 4.f, "A rejected whole-value Transform write changed the position.");
    w::Trs trs{{4.f, 5.f, 6.f}, {0.f, 0.f, std::sqrt(.5f), std::sqrt(.5f)}, {2.f, 3.f, 4.f}};
    take(world.set_transform(entity.id(), trs));
    const auto matrix = take(scene.render_items()).front().world;
    require(near(matrix[0], 0.f) && near(matrix[1], 2.f) && near(matrix[4], -3.f) && near(matrix[10], 4.f) && matrix[12] == 4.f,
        "TRS did not produce S*R*T row-vector geometry.");
    const auto original = take(w::read_transform(runtime, entity.id()));
    auto illegal = original; illegal.scale.x = std::numeric_limits<float>::quiet_NaN();
    reject(world.set_transform(entity.id(), illegal), "Non-finite scale was accepted.");
    illegal = original; illegal.rotation = {0,0,0,0}; reject(world.set_transform(entity.id(), illegal), "Zero quaternion was accepted.");
    require(take(w::read_transform(runtime, entity.id())) == original, "Rejected TRS changed stored values.");
    illegal = original; illegal.scale = {1e-20f, 1e-20f, 1e-20f}; take(world.set_transform(entity.id(), illegal));
    require(take(w::world_matrix(runtime, entity.id()))[10] > 0.f, "Tiny positive scale was rejected or collapsed.");
    illegal = original; illegal.rotation = {0.f, .6f, .8f, 0.f};
    const auto maximum = std::numeric_limits<float>::max();
    illegal.scale = {maximum, maximum, maximum};
    take(world.set_transform(entity.id(), illegal));
    require(std::ranges::all_of(take(w::world_matrix(runtime, entity.id())), [](float value) { return std::isfinite(value); }),
        "Quaternion storage rounding overflowed a finite positive scale.");
    take(world.set_transform(entity.id(), original));

    auto removed = take(world.remove_component(cube.id()));
    collect(runtime);
    require(take(scene.render_items()).empty() && take(w::inspect_component(runtime, cube.id())).owner == ObjectId{}, "Cube removal retained participation or owner.");
    require(take(w::has_transform(runtime, entity.id())) && take(w::read_transform(runtime, entity.id())) == original,
        "Removing Mesh capability removed Entity spatial identity.");
    committed(world.restore(removed));
    require(take(scene.render_items()).size() == 1, "Component undo did not restore drawing.");
    auto counter = take(w::create_component(runtime, derived));
    const std::array counter_added{w::Attachment{counter.id()}};
    committed(world.attach_components(entity.id(), counter_added));
    require(take(world.query(types.counter)).size() == 1 && take(world.query(types.counter, false)).empty(), "Derived Counter was not indexed by its actual type.");
    source::Program program(runtime, types.counter);
    publish(program, "void step(float amount) { self.advance(amount); }");
    const auto entry = take(program.bind<void, float>());
    take(world.bind_behavior(types.counter, {.tick = [entry](Runtime&, ObjectId receiver, float amount) -> Result<void> {
        const std::array arguments{Scalar{amount}};
        auto result = entry.invoke(receiver, arguments);
        if (!result) return std::unexpected(result.error());
        return {};
    }}));
    take(world.update(10.f));
    require(std::get<float>(take(runtime.read(counter.id(), types.counter_value))) == 0.f, "Editor World ran game code.");
    committed(world.start());
    take(world.update(2.f));
    require(take(w::inspect_entity(runtime, entity.id())).state == w::EntityState::active &&
        std::get<float>(take(runtime.read(counter.id(), types.counter_value))) == 2.f, "JIT did not update the same derived Counter.");
    committed(world.set_enabled(counter.id(), false)); take(world.update(7.f));
    require(std::get<float>(take(runtime.read(counter.id(), types.counter_value))) == 2.f && !take(w::inspect_component(runtime, counter.id())).active,
        "Disabled component still ran.");
    require(take(w::inspect_entity(runtime, entity.id())).state == w::EntityState::active && take(scene.render_items()).size() == 1,
        "Component activation was confused with Entity activation or presentation.");
    committed(world.set_enabled(counter.id(), true));
    source::Program deferred(runtime, types.counter);
    publish(deferred, "async void later() { yield; self.advance(50.0); }");
    const auto later = take(deferred.bind<void>(FunctionExecution::coroutine));
    take(world.start_task(counter.id(), [later](Runtime&, ObjectId receiver) { return later.start(receiver, {}); }));
    require(world.activity().tasks == 1, "World did not own the real coroutine.");
    take(world.update(1.f)); // task suspends after its yield
    committed(world.set_enabled(entity.id(), false));
    require(world.activity().tasks == 0 && world.activity().update_registrations == 0, "Entity deactivation did not cancel its task/registration.");
    take(world.update(1.f));
    require(std::get<float>(take(runtime.read(counter.id(), types.counter_value))) == 3.f, "Cancelled coroutine resumed or inactive Entity ran.");
    std::size_t phantom_activation{};
    take(world.observe_lifecycle([&](w::World&, w::Lifecycle event, ObjectId target) -> Result<void> {
        if (event == w::Lifecycle::activated && target == counter.id()) ++phantom_activation;
        return {};
    }));
    committed(world.set_enabled(counter.id(), false)); committed(world.set_enabled(counter.id(), true));
    require(phantom_activation == 0 && !take(w::inspect_component(runtime, counter.id())).active,
        "Component emitted activation while its Entity was inactive.");
    take(world.observe_lifecycle({}));
    committed(world.set_enabled(entity.id(), true));
    committed(world.stop());
    require(take(w::inspect_entity(runtime, entity.id())).state == w::EntityState::attached_inactive && take(scene.render_items()).size() == 1,
        "World.stop removed editor presentation or membership.");
    auto removal = take(world.detach(entity.id()));
    const auto entity_id = entity.id(), collision_id = collision.id(), cube_id = cube.id(), counter_id = counter.id();
    entity.reset(); collision.reset(); cube.reset(); counter.reset(); removed.root.reset();
    require(runtime.root_count() == 1, "Delete/undo sample still has roots outside its Entity removal record.");
    collect(runtime);
    require(take(w::inspect_entity(runtime, entity_id)).state == w::EntityState::detached && take(scene.render_items()).empty(), "Entity detach left participation.");
    take(world.write(entity_id, types.position_fields[0], Scalar{99.f}));
    require(std::get<float>(take(world.read(entity_id, types.position_fields[0]))) == 99.f,
        "Detached composition did not support preparation through the same World editor entry.");
    take(world.write(entity_id, types.position_fields[0], Scalar{original.position.x}));
    committed(world.restore(removal));
    entity = take(runtime.hold(entity_id)); collision = take(runtime.hold(collision_id));
    cube = take(runtime.hold(cube_id)); counter = take(runtime.hold(counter_id));
    require(take(world.members()).front().id() == entity.id() && take(w::read_transform(runtime, entity.id())) == original,
        "Delete/GC/undo lost identity or data without an offline writer.");
    committed(world.start());
    take(world.start_task(counter.id(), [later](Runtime&, ObjectId receiver) { return later.start(receiver, {}); }));
    take(world.update(0.f));
    require(world.activity().tasks == 1, "Close sample did not contain a suspended real task.");
    committed(world.close());
    require(world.activity().tasks == 0 && world.activity().update_registrations == 0 && !world.members(scene.meshes()),
        "Close retained real execution/presentation registrations or task frames.");
    committed(w::destroy_entity(runtime, entity.id()));
    require(take(w::inspect_entity(runtime, entity.id())).state == w::EntityState::invalid, "Explicit destroy did not invalidate a strongly held Entity.");
    reject(world.restore(removal), "Closed World was revived by history.");
    std::puts("[pass] World editor and JIT: empty/derived components, dependencies, TRS, four states, real task cancellation and identity undo");
}

void reference_lifetime() {
    Runtime runtime;
    const auto types = register_all_types(runtime);
    const auto holder = holder_type(runtime, types);
    auto keeper = take(runtime.create(holder));
    const auto entity_field = take(runtime.find_field(holder, "entity"));
    const auto component_field = take(runtime.find_field(holder, "component"));
    ObjectId entity_id, cube_id, sibling_id;
    {
        w::World world(runtime, types);
        auto entity = take(w::create_entity(runtime)); entity_id = entity.id();
        committed(world.attach(entity_id));
        auto shape = add_shape(runtime, types, world, entity_id);
        sibling_id = shape[0].id(); cube_id = shape[1].id();
        take(world.write(entity_id, types.position_fields[1], Scalar{23.f}));
        take(runtime.write_reference(keeper.id(), entity_field, entity.reference()));
        auto removed = take(world.detach(entity_id));
        removed.root.reset(); entity.reset(); shape.clear();
        collect(runtime);
        require(take(w::components(runtime, entity_id)).size() == 2 && runtime.type_of(cube_id).has_value(), "Ordinary Entity reference did not retain the detached composition.");
        auto cube = take(runtime.hold(cube_id, types.component));
        take(runtime.write_reference(keeper.id(), component_field, cube.reference()));
        take(runtime.write_reference(keeper.id(), entity_field, take(runtime.null_reference(types.entity))));
        cube.reset(); collect(runtime);
        require(runtime.type_of(entity_id).has_value() && runtime.type_of(sibling_id).has_value(), "Component strong owner did not retain Entity and siblings.");
        committed(world.attach(entity_id));
        committed(world.close());
    }
    collect(runtime);
    require(take(w::inspect_entity(runtime, entity_id)).state == w::EntityState::detached && take(w::components(runtime, entity_id)).size() == 2,
        "Destroying the old World invalidated retained data.");
    require(take(w::read_transform(runtime, entity_id)).position.y == 23.f, "World close lost retained component field values.");
    committed(w::destroy_entity(runtime, entity_id));
    require(!runtime.type_of(entity_id) && !runtime.type_of(cube_id) && !runtime.type_of(sibling_id), "Detached destruction did not terminate the complete group.");
    reject(w::destroy_entity(runtime, entity_id), "Repeated destroy resurrected or accepted a stale identity.");
    require(take(w::inspect_component(runtime, cube_id)).valid == false, "Destroyed component remained valid.");
    keeper.reset(); collect(runtime);
    require(runtime.live_objects() == 0 && runtime.root_count() == 0, "Destruction retained hidden roots.");

    // Independent sample: no explicit destroy, including the Entity<->Component cycle.
    {
        w::World world(runtime, types);
        auto entity = take(w::create_entity(runtime)); entity_id = entity.id();
        committed(world.attach(entity_id));
        auto shape = add_shape(runtime, types, world, entity_id);
        committed(world.close());
        entity.reset(); shape.clear();
    }
    collect(runtime);
    require(runtime.live_objects() == 0 && runtime.root_count() == 0 && take(w::inspect_entity(runtime, entity_id)).state == w::EntityState::invalid,
        "Unreachable strong composition cycle did not collect.");
    std::puts("[pass] World lifetime: ordinary Entity/Component fields retain the graph, close and old-World destruction, explicit termination, independent GC");
}

void worlds_and_requests() {
    Runtime runtime, other;
    const auto types = register_all_types(runtime), foreign_types = register_all_types(other);
    w::World first(runtime, types), second(runtime, types);
    const auto second_scene = take(p::Scene::create(second, types));
    auto entity = take(w::create_entity(runtime));
    auto foreign = take(w::create_entity(other));
    auto scalar_object = take(runtime.create(runtime.object_type()));
    reject(w::inspect_entity(runtime, foreign.id()), "Foreign Runtime identity was classified as local Invalid.");
    reject(w::inspect_entity(runtime, scalar_object.id()), "Non-Entity object was classified as an Entity.");
    require(take(w::inspect_entity(runtime, {})).state == w::EntityState::invalid, "Null identity did not classify as Invalid.");
    committed(first.attach(entity.id()));
    auto shape = add_shape(runtime, types, first, entity.id());
    reject(second.attach(entity.id()), "Direct transfer of an attached Entity succeeded.");
    require(take(w::inspect_entity(runtime, entity.id())).state == w::EntityState::attached_inactive, "Foreign World attempt invalidated the Entity.");
    const auto pending = take(first.request({w::RequestKind::destroy_entity, entity.id()}));
    require(runtime.type_of(entity.id()).has_value(), "Queued destroy terminated identity before commit.");
    auto removed = take(first.detach(entity.id()));
    committed(second.attach(entity.id()));
    const auto rejected = take(first.flush());
    require(rejected.size() == 1 && rejected[0].request == pending && !rejected[0].result && runtime.type_of(entity.id()).has_value(),
        "Old World queued destroy followed the Entity into a different World.");
    reject(first.restore(removed), "History stole the Entity from another World.");
    committed(first.close());
    require(take(second.members()).size() == 1 && take(second_scene.render_items()).size() == 1, "Closing one World affected another.");
    const auto destruction = take(second.request({w::RequestKind::destroy_entity, entity.id()}));
    auto destroyed = take(second.flush());
    require(destroyed.size() == 1 && destroyed[0].request == destruction && destroyed[0].result && !runtime.type_of(entity.id()),
        "Queued destruction did not commit through the object protocol.");
    auto fresh = take(w::create_entity(runtime));
    require(fresh.id() != entity.id(), "Destroyed identity was reused without a new generation.");
    reject(second.attach(entity.id()), "Stale Entity identity was restored.");
    reject(first.request({w::RequestKind::attach_entity, fresh.id()}), "Closed World accepted new work.");
    committed(second.close());
    (void)foreign_types;
    std::puts("[pass] World isolation: context/type errors, detached reattachment, history provenance and queued identity/World revalidation");
}

void lifecycle_subscription_unsubscribe() {
    Runtime runtime;
    const auto types = register_all_types(runtime);
    w::World world(runtime, types);
    auto entity = take(w::create_entity(runtime));
    auto component = take(w::create_component(runtime, types.component));
    std::uint64_t skipped{}, self{};
    std::size_t first_calls{}, skipped_calls{}, self_calls{}, stateful_calls{};
    const auto first = take(world.subscribe_lifecycle([&](w::World& current, w::Lifecycle, ObjectId) -> Result<void> {
        ++first_calls;
        current.unsubscribe_lifecycle(skipped);
        return {};
    }));
    skipped = take(world.subscribe_lifecycle([&](w::World&, w::Lifecycle, ObjectId) -> Result<void> {
        ++skipped_calls;
        return {};
    }));
    self = take(world.subscribe_lifecycle([&](w::World& current, w::Lifecycle, ObjectId) -> Result<void> {
        ++self_calls;
        current.unsubscribe_lifecycle(self);
        return {};
    }));
    const auto stateful = take(world.subscribe_lifecycle(
        [count = std::size_t{}, &stateful_calls](w::World&, w::Lifecycle, ObjectId) mutable -> Result<void> {
            stateful_calls = ++count;
            return {};
        }));
    committed(world.attach(entity.id()));
    const std::array attachments{w::Attachment{component.id()}};
    committed(world.attach_components(entity.id(), attachments));
    require(first_calls == 2 && skipped_calls == 0 && self_calls == 1 && stateful_calls == 2,
        "Lifecycle unsubscription during dispatch left a callback active.");
    world.unsubscribe_lifecycle(first);
    world.unsubscribe_lifecycle(stateful);
    auto removed = take(world.detach(entity.id()));
    require(!removed.commit.notification_error, "Lifecycle detach notification failed.");
    require(first_calls == 2 && skipped_calls == 0 && self_calls == 1,
        "Lifecycle callbacks ran after unsubscription.");
}

void failures_and_reentry() {
    Runtime runtime;
    const auto types = register_all_types(runtime);
    w::World world(runtime, types);
    take(world.set_failure_policy(w::FailurePolicy::halt));
    auto entity = take(w::create_entity(runtime));
    bool callback_called = false;
    take(world.observe_lifecycle([&](w::World& current, w::Lifecycle event, ObjectId target) -> Result<void> {
        if (event != w::Lifecycle::attached) return {};
        callback_called = true;
        reject(current.detach(target), "Lifecycle callback reentered structural commit.");
        reject(w::destroy_entity(runtime, target), "Object destruction reentered a lifecycle callback.");
        take(current.request({w::RequestKind::detach_entity, target}));
        return std::unexpected(Error{ErrorCode::invocation_failed, "Intentional lifecycle consumer failure after commit."});
    }));
    const auto result = take(world.attach(entity.id()));
    require(callback_called && result.changed && result.notification_error && take(world.members()).size() == 1,
        "Post-commit failure was reported as an uncommitted attach.");
    auto completion = take(world.flush());
    require(completion.size() == 1 && completion[0].result && take(w::inspect_entity(runtime, entity.id())).state == w::EntityState::detached,
        "Deferred callback request did not execute at the next boundary.");
    take(world.observe_lifecycle({})); committed(world.attach(entity.id()));
    std::size_t close_callbacks{};
    take(world.observe_lifecycle([&](w::World& current, w::Lifecycle event, ObjectId target) -> Result<void> {
        if (event == w::Lifecycle::detached) {
            ++close_callbacks;
            reject(current.start(), "Closing World restarted from an exit callback.");
            reject(current.request({w::RequestKind::destroy_entity, target}), "Closing World accepted a request.");
            require(take(w::inspect_entity(runtime, target)).state == w::EntityState::detached,
                "Close notification did not expose the complete Detached state.");
        }
        return {};
    }));
    committed(world.close());
    require(close_callbacks == 1 && world.activity().tasks == 0 &&
        !take(world.close()).changed, "Close was not complete/idempotent.");
    committed(w::destroy_entity(runtime, entity.id()));
    std::puts("[pass] World failure boundaries: committed notification failure, deferred reentry, closed admission and no resurrection");
}
void ordered_lifecycle() {
    Runtime runtime;
    const auto types = register_test_types(runtime);
    const auto derived = take(NativeRegistration<DerivedCounter>{runtime, "worldAcceptance.OrderedDerived", TypeKind::object}
        .base<Counter>().field<&DerivedCounter::extra>("extra").publish());
    using Event = std::pair<char, ObjectId>;
    std::vector<Event> events;
    ObjectId fail_begin, fail_tick, fail_end;
    w::World world(runtime, types);
    take(world.set_failure_policy(w::FailurePolicy::halt));
    auto entity = take(w::create_entity(runtime));
    auto second_entity = take(w::create_entity(runtime));
    auto a = take(w::create_component(runtime, types.counter));
    auto b = take(w::create_component(runtime, derived));
    auto c = take(w::create_component(runtime, types.counter));
    auto d = take(w::create_component(runtime, derived));
    auto inserted = take(w::create_component(runtime, types.counter));
    committed(world.attach(entity.id())); committed(world.attach(second_entity.id()));
    const std::array group{w::Attachment{a.id()}, w::Attachment{b.id()}, w::Attachment{c.id()}};
    committed(world.attach_components(entity.id(), group));
    const std::array other{w::Attachment{d.id()}};
    committed(world.attach_components(second_entity.id(), other));
    const auto advance = take(runtime.find_method(types.counter, "advance"));
    const auto callback = [&](char phase, ObjectId id, ObjectId failure) -> Result<void> {
        events.emplace_back(phase, id);
        if (id == failure) return std::unexpected(Error{ErrorCode::invocation_failed, "Intentional lifecycle failure."});
        return {};
    };
    take(world.bind_behavior(types.counter, {
        .begin = [&](Runtime&, ObjectId id) { return callback('B', id, fail_begin); },
        .tick = [&](Runtime& rt, ObjectId id, float dt) -> Result<void> {
            auto result = callback('T', id, fail_tick);
            if (!result) return result;
            const std::array args{Scalar{dt}};
            return rt.call(id, advance, args);
        },
        .end = [&](Runtime&, ObjectId id) { return callback('E', id, fail_end); }
    }));
    committed(world.set_enabled(b.id(), false));
    committed(world.start());
    require(events == std::vector<Event>{{'B',a.id()},{'B',b.id()},{'B',c.id()},{'B',d.id()}},
        "Begin order depended on TypeId or skipped disabled components.");
    events.clear(); take(world.update(1));
    require(events == std::vector<Event>{{'T',a.id()},{'T',c.id()},{'T',d.id()}}, "Disabled component still ticked.");
    events.clear(); committed(world.set_enabled(b.id(), true));
    require(events.empty(), "Enabled toggling repeated Begin or End.");
    const std::array addition{w::Attachment{inserted.id()}};
    committed(world.attach_components(entity.id(), addition, 1));
    require(events == std::vector<Event>{{'B',inserted.id()}}, "Dynamic addition repeated existing Begin.");
    events.clear(); take(world.update(1));
    // Tick order within a phase is unspecified without prerequisites.
    const std::vector<Event> ticked{{'T',a.id()},{'T',inserted.id()},{'T',b.id()},{'T',c.id()},{'T',d.id()}};
    require(std::ranges::is_permutation(events, ticked), "Tick skipped or repeated an actual type or dynamically inserted component.");
    events.clear(); auto removal = take(world.remove_component(b.id()));
    require(events == std::vector<Event>{{'E',b.id()}}, "Dynamic removal did not pair End.");
    events.clear(); committed(world.restore(removal));
    require(events == std::vector<Event>{{'B',b.id()}}, "Dynamic restore did not start a new participation interval.");
    events.clear(); committed(world.stop());
    const std::vector<Event> reverse{{'E',d.id()},{'E',c.id()},{'E',b.id()},{'E',inserted.id()},{'E',a.id()}};
    require(events == reverse, "End did not reverse the current member/composition order.");
    events.clear(); committed(world.stop()); require(events.empty(), "Repeated stop repeated End.");
    fail_begin = b.id();
    auto failed = take(world.start());
    require(failed.changed && failed.notification_error && !world.running() &&
        events == std::vector<Event>{{'B',a.id()},{'B',inserted.id()},{'B',b.id()},{'E',inserted.id()},{'E',a.id()}},
        "Begin failure did not unwind exactly the successfully begun prefix.");
    fail_begin = {}; events.clear(); committed(world.start()); events.clear();
    fail_tick = b.id(); reject(world.update(1), "Tick failure was swallowed.");
    std::vector<Event> expected{{'T',a.id()},{'T',inserted.id()},{'T',b.id()}};
    expected.insert(expected.end(), reverse.begin(), reverse.end());
    require(events == expected && !world.running(), "Tick failure did not stop later ticks and finish reverse End cleanup.");
    fail_tick = {}; events.clear(); committed(world.start()); events.clear();
    fail_end = c.id(); failed = take(world.stop());
    require(failed.notification_error && events == reverse, "End failure abandoned later cleanup.");
    fail_end = {}; events.clear();
    take(world.bind_behavior(derived, {})); // Explicit empty binding replaces the whole inherited behavior.
    committed(world.start()); take(world.update(1)); committed(world.close());
    require(std::ranges::none_of(events, [&](const Event& event) { return event.second == b.id() || event.second == d.id(); }),
        "Derived empty binding incorrectly mixed inherited Begin/End with its own Tick.");
    std::puts("[pass] Ordered lifecycle: inherited native binding, composition order, disabled participation, dynamic insertion/removal, failure cleanup");
}
void isolated_failures() {
    Runtime runtime;
    const auto types = register_test_types(runtime);
    source::Program ticking(runtime, types.counter), waiting(runtime, types.counter);
    auto candidate = take(source::Program::prepare(take(ticking.request("world-failure.source",
        "void tick(float dt) {\n    self.steps = 10 / self.steps;\n}\n", {}, {.diagnostics = true}))));
    take(ticking.try_publish(candidate));
    publish(waiting, "async void later() { yield; self.steps = 10 / self.steps; }");
    const auto script = take(ticking.pin());
    const auto later = take(waiting.bind<void>(FunctionExecution::coroutine));
    const auto steps = take(runtime.find_field(types.counter, "steps"));
    const auto translate = take(runtime.find_method(types.component, "translate_owner"));
    w::World world(runtime, types);
    require(world.failure_policy() == w::FailurePolicy::isolate, "World did not default to object isolation.");
    auto e = take(w::create_entity(runtime)), f = take(w::create_entity(runtime)), g = take(w::create_entity(runtime));
    auto a = take(w::create_component(runtime, types.counter)), b = take(w::create_component(runtime, types.counter));
    auto c = take(w::create_component(runtime, types.counter)), d = take(w::create_component(runtime, types.counter));
    auto m = take(w::create_component(runtime, types.counter));
    const auto a_id = a.id(), b_id = b.id(), c_id = c.id(), d_id = d.id(), m_id = m.id();
    take(world.set_transform(f.id(), {})); take(world.set_transform(g.id(), {}));
    // A registered simulation owns g's pose, as PhysicsScene does for bodies.
    std::vector<ObjectId> relocations;
    bool refuse_relocation{}, relocation_reentry_rejected{};
    take(world.add_simulation({.step = [](float) -> Result<void> { return {}; }, .writes_pose = [g = g.id()](ObjectId entity) { return entity == g; },
        .relocated = [&](std::span<const ObjectId> moved) -> Result<void> {
            relocation_reentry_rejected = !world.set_transform(f.id(), {});
            if (refuse_relocation) return std::unexpected(Error{ErrorCode::unsupported_operation, "Intentional relocation refusal."});
            relocations.assign(moved.begin(), moved.end());
            return {};
        }}));
    const std::array e_group{w::Attachment{a_id}, w::Attachment{b_id}, w::Attachment{c_id}};
    const std::array f_group{w::Attachment{d_id}};
    const std::array g_group{w::Attachment{m_id}};
    committed(world.attach_components(e.id(), e_group));
    committed(world.attach_components(f.id(), f_group));
    committed(world.attach_components(g.id(), g_group));
    using Event = std::pair<char, ObjectId>;
    std::vector<Event> events;
    ObjectId fail_begin;
    bool world_failure{}, fail_observer{true};
    take(world.bind_behavior(types.counter, {
        .begin = [&](Runtime&, ObjectId id) -> Result<void> {
            events.emplace_back('B', id);
            if (id == fail_begin) return std::unexpected(Error{ErrorCode::invocation_failed, "Intentional isolated Begin failure."});
            return {};
        },
        .tick = [&](Runtime& rt, ObjectId id, float dt) -> Result<void> {
            events.emplace_back('T', id);
            if (id == b_id) {
                source::Diagnostics capture;
                const std::array arguments{Scalar{dt}};
                if (auto invoked = script.invoke(id, arguments, &capture); !invoked) return std::unexpected(invoked.error());
            }
            if (id == d_id && world_failure) return std::unexpected(Error{ErrorCode::capacity_exceeded, "Intentional World-level failure."});
            if (id == d_id || id == m_id) {
                const std::array arguments{Scalar{1.f}, Scalar{0.f}, Scalar{0.f}};
                return rt.call(id, translate, arguments);
            }
            return {};
        },
        .end = [&](Runtime&, ObjectId id) -> Result<void> { events.emplace_back('E', id); return {}; }
    }));
    take(world.observe_lifecycle([&](w::World&, w::Lifecycle event, ObjectId id) -> Result<void> {
        if (!fail_observer || event != w::Lifecycle::attached || id != d_id) return {};
        fail_observer = false;
        return std::unexpected(Error{ErrorCode::invocation_failed, "Intentional isolated observer failure."});
    }));
    committed(world.attach(e.id())); committed(world.attach(f.id())); committed(world.attach(g.id()));
    auto report = world.take_diagnostics();
    require(report.size() == 1 && report[0].kind == w::DiagnosticKind::observer_failed && report[0].phase == w::DiagnosticPhase::lifecycle &&
        report[0].object == d_id && report[0].entity == f.id() && report[0].message == "Intentional isolated observer failure.",
        "Observer failure was not isolated to a diagnostic on its object.");

    fail_begin = a_id;
    committed(world.start());
    require(world.running() && events == std::vector<Event>{{'B',a_id},{'B',b_id},{'B',c_id},{'B',d_id},{'B',m_id}},
        "Isolated Begin failure stopped the World or skipped later objects.");
    report = world.take_diagnostics();
    require(report.size() == 1 && report[0].kind == w::DiagnosticKind::behavior_failed && report[0].phase == w::DiagnosticPhase::begin &&
        report[0].severity == w::Severity::error && report[0].object == a_id && report[0].entity == e.id() &&
        report[0].type == "worldAcceptance.Counter" && report[0].serial > 1 && report[0].repeats == 1,
        "Begin failure diagnostic lost object identity, type or phase.");
    reject(world.set_failure_policy(w::FailurePolicy::halt), "Failure policy changed while running.");
    const auto starter = [later](Runtime&, ObjectId id) { return later.start(id, {}); };
    reject(world.start_task(a_id, starter), "A Begin-faulted Component accepted a task.");
    take(world.start_task(c_id, starter));
    events.clear(); take(world.update(1));
    require(events == std::vector<Event>{{'T',b_id},{'T',c_id},{'T',d_id},{'T',m_id}} && world.running(),
        "Tick visited a Begin-faulted object or stopped at the first isolated Tick failure.");
    take(world.update(1));
    events.clear(); take(world.update(1));
    require(events == std::vector<Event>{{'T',d_id},{'T',m_id}} && world.activity().tasks == 0 && world.activity().update_registrations == 2,
        "Tick or task failure did not stop only the failing object's Tick and tasks.");
    require(near(take(w::world_matrix(runtime, f.id()))[12], 3) && near(take(w::world_matrix(runtime, g.id()))[12], 0),
        "Translation rejection was not limited to the simulation-controlled Entity.");
    report = world.take_diagnostics();
    require(report.size() == 3 && report[0].object == b_id && report[0].phase == w::DiagnosticPhase::tick &&
        report[0].kind == w::DiagnosticKind::behavior_failed && report[0].message.find("nonzero divisor") != std::string::npos &&
        report[0].source && report[0].source->path == "world-failure.source" && report[0].source->line == 2,
        "Script Tick failure lost its message or source location.");
    require(report[1].object == m_id && report[1].entity == g.id() && report[1].kind == w::DiagnosticKind::command_rejected &&
        report[1].phase == w::DiagnosticPhase::command && report[1].severity == w::Severity::warning && report[1].repeats == 3 &&
        report[1].message.find("translate_owner") != std::string::npos,
        "Simulation-controlled translation was not rejected per command and coalesced while persisting.");
    require(report[2].object == c_id && report[2].kind == w::DiagnosticKind::task_failed && report[2].phase == w::DiagnosticPhase::task,
        "Task failure was not isolated to its object.");

    // A host clears faults after publishing fixed code: Tick resumes without a
    // second Begin, and the Begin-faulted object begins through the running path.
    take(runtime.write(b_id, steps, Scalar{std::uint32_t{1}}));
    fail_begin = {}; events.clear();
    committed(world.clear_faults());
    require(events == std::vector<Event>{{'B',a_id}}, "clear_faults repeated Begin or skipped a Begin-faulted object.");
    events.clear(); take(world.update(1));
    require(events == std::vector<Event>{{'T',a_id},{'T',b_id},{'T',c_id},{'T',d_id},{'T',m_id}} && world.take_diagnostics().empty(),
        "Cleared objects did not resume Tick or a persisting rejection was reported again.");
    // Running members are edited at the host boundary. A transform edit reaches
    // the simulation claiming the moved Entity, which may not edit the World;
    // its refusal restores the edit.
    take(world.write(b_id, std::array{steps}, Scalar{std::uint32_t{2}}));
    require(take(runtime.read(b_id, steps)) == Scalar{std::uint32_t{2}}, "A running member rejected a host-boundary field edit.");
    take(world.set_transform(f.id(), {{0, 1, 0}, {}, {1, 1, 1}}));
    require(relocations.empty(), "An Entity no simulation claims was relocated.");
    take(world.set_transform(g.id(), {{0, 5, 0}, {}, {1, 1, 1}}));
    require(relocations == std::vector<ObjectId>{g.id()} && relocation_reentry_rejected,
        "A claimed Entity's transform edit did not reach its simulation, or the simulation edited the World.");
    refuse_relocation = true;
    reject(world.set_transform(g.id(), {{0, 9, 0}, {}, {1, 1, 1}}), "A refused relocation kept the edit.");
    require(near(take(w::read_transform(runtime, g.id())).position.y, 5), "A refused relocation did not restore the transform.");
    refuse_relocation = false;
    events.clear(); committed(world.stop());
    require(events == std::vector<Event>{{'E',m_id},{'E',d_id},{'E',c_id},{'E',b_id},{'E',a_id}}, "Isolation broke End pairing or reverse order.");
    fail_begin = d_id; committed(world.start());
    events.clear(); committed(world.stop());
    require(events == std::vector<Event>{{'E',m_id},{'E',c_id},{'E',b_id},{'E',a_id}}, "A Begin-faulted object received End.");
    (void)world.take_diagnostics();

    fail_begin = {}; committed(world.start());
    world_failure = true;
    reject(world.update(1), "A World-level Tick failure was isolated.");
    require(!world.running() && world.take_diagnostics().empty(), "A World-level failure did not stop the World or was reported as an object fault.");
    world_failure = false;
    take(world.set_failure_policy(w::FailurePolicy::halt));
    fail_begin = d_id;
    const auto halted = take(world.start());
    report = world.take_diagnostics();
    require(halted.notification_error && !world.running() && report.size() == 1 && report[0].object == d_id &&
        report[0].phase == w::DiagnosticPhase::begin, "Halt policy did not stop on the first failure and report it.");
    committed(world.close());
    std::puts("[pass] Object isolation: Begin/Tick/task/observer/command faults, script source, coalesced diagnostics, clear_faults, World-level and halt boundaries");
}

#include "lifecycle.inl"
#include "static_mesh_entity.inl"
#include "lighting.inl"
#include "hierarchy.inl"
#include "frame.inl"

void spatial_tree_and_mesh() {
    Runtime runtime;
    const auto types = register_all_types(runtime);
    w::World world(runtime, types);
    const auto scene = take(p::Scene::create(world, types));
    auto entity = take(p::create_static_mesh_entity(world,{geometry::cube_mesh(),
        {{10,0,0},{0,0,std::sqrt(.5f),std::sqrt(.5f)},{2,2,2}}}));
    auto child = take(w::create_entity(runtime));
    auto leaf = take(w::create_entity(runtime));
    auto mesh = take(w::create_component(runtime, types.static_mesh_component));
    auto triangle = std::make_shared<const geometry::Mesh>(std::vector<geometry::Vertex>{
        {{0,0,0},{0,0,-1}}, {{1,0,0},{0,0,-1}}, {{0,1,0},{0,0,-1}}});
    take(p::set_geometry(runtime, mesh.id(), triangle));
    const std::array attachments{w::Attachment{mesh.id()}};
    take(world.set_transform(child.id(), {{1,0,0},{},{1,1,1}}));
    take(world.set_transform(leaf.id(), {{0,3,0},{},{1,1,1}}));
    committed(world.attach_components(leaf.id(), attachments));
    committed(world.reparent(leaf.id(), child.id(), w::ReparentRule::keep_local));
    committed(world.reparent(child.id(), entity.id(), w::ReparentRule::keep_local));
    require(take(world.members()).empty(), "Detached composition registered members.");
    committed(world.attach(entity.id()));
    require(take(world.members()).size()==3, "Subtree attach duplicated or omitted a member.");
    auto before = take(w::world_matrix(runtime, leaf.id()));
    require(near(before[12],4) && near(before[13],2), "Hierarchy did not compose Entity local transforms.");
    const auto snapshots = take(scene.render_items());
    require(snapshots.size() == 2 && snapshots[1].geometry == triangle,
        "Mesh capabilities lost their distinct shared geometry resources.");
    committed(world.reparent(leaf.id(), entity.id(), w::ReparentRule::keep_world));
    auto after = take(w::world_matrix(runtime, leaf.id()));
    for (std::size_t i=0;i<16;++i) require(near(before[i],after[i]), "Keep-world reparent moved the Entity.");
    require(take(w::parent_entity(runtime, leaf.id())) == entity.id(), "Reparent did not publish composition parent.");
    committed(world.reparent(child.id(), leaf.id(), w::ReparentRule::keep_local));
    reject(world.reparent(leaf.id(), child.id(), w::ReparentRule::keep_local), "Composition cycle was accepted.");
    committed(world.reparent(child.id(), entity.id(), w::ReparentRule::keep_local));
    take(world.set_transform(child.id(), {{},{},{2,1,1}}));
    take(world.set_transform(leaf.id(), {{},{0,0,std::sin(.4f),std::cos(.4f)},{1,1,1}}));
    const auto local = take(w::read_transform(runtime, leaf.id()));
    reject(world.reparent(leaf.id(), child.id(), w::ReparentRule::keep_world), "Keep-world silently discarded shear.");
    require(take(w::parent_entity(runtime, leaf.id())) == entity.id() && take(w::read_transform(runtime, leaf.id())) == local,
        "Rejected reparent changed relation or local TRS.");
    auto removal = take(world.remove_component(mesh.id()));
    require(take(w::read_transform(runtime,leaf.id()))==local && take(w::has_transform(runtime,leaf.id())),
        "Deleting Mesh capability deleted or changed Entity space.");
    committed(world.restore(removal));
    require(take(scene.render_items()).back().geometry == triangle, "Undo lost geometry identity.");
    committed(world.start());
    take(p::set_geometry(runtime, mesh.id(), {}));
    require(take(scene.render_items()).size() == 1 && world.activity().update_registrations == 0,
        "Pure mesh resource replacement created Tick or retained presentation.");
    take(p::set_geometry(runtime, mesh.id(), triangle));
    committed(world.close());
    committed(w::destroy_entity(runtime,entity.id()));
    require(!runtime.type_of(child.id()) && !runtime.type_of(leaf.id()) && !runtime.type_of(mesh.id()),
        "Entity destruction left owned descendants alive.");
    std::puts("[pass] Entity space: component-independent TRS, recursive composition, atomic reparent, cycle/shear rejection, Mesh history and destruction");
}

void sparse_presentation() {
    Runtime runtime; const auto types=register_all_types(runtime); w::World world(runtime,types); const auto scene=take(p::Scene::create(world,types));
    const auto allocated_bytes=[] {
        mi_collect(true); // Flush shared allocator counters outside the observed extraction.
        mi_stats_t stats; mi_stats_init(&stats);
        require(mi_stats_get(&stats),"World allocator statistics are unavailable.");
        return stats.malloc_normal.total+stats.malloc_huge.total;
    };
    const auto capture=[&] {
        (void)take(scene.render_items()); // Warm immutable material publication before observing allocations.
        const auto before=allocated_bytes();
        auto items=scene.render_items();
        const auto bytes=allocated_bytes()-before;
        return std::pair{take(std::move(items)),bytes};
    };
    const auto add_logical_members=[&] {
        std::vector<ObjectRoot> held; std::vector<ObjectId> ids;
        held.reserve(256); ids.reserve(256);
        for(unsigned i=0;i<256;++i) {held.push_back(take(w::create_entity(runtime)));ids.push_back(held.back().id());}
        committed(world.attach_batch(ids));
    };
    const auto empty=capture();
    // MSVC checked iterators allocate proxies even for empty return vectors.
    // Use the real empty World as this configuration's fixed return-container baseline.
    require(empty.first.empty() && empty.first.capacity()==0,"Empty World reserved mesh extraction storage.");
    add_logical_members();
    const auto logical=capture();
    require(logical.first.empty() && logical.first.capacity()==0 && logical.second==empty.second,
        "Logical-only World added allocation beyond the empty return-container baseline.");
    auto light_entity=take(w::create_entity(runtime));
    auto light=take(w::create_component(runtime,types.light_component));
    take(world.set_transform(light_entity.id(),{}));
    const std::array light_group{w::Attachment{light.id()}};
    committed(world.attach_components(light_entity.id(),light_group));committed(world.attach(light_entity.id()));
    const auto lights=capture();
    require(lights.first.empty() && lights.first.capacity()==0 && lights.second==empty.second,
        "Light-only presentation added allocation beyond the empty return-container baseline.");

    // The spatial path exceeds the presentation count, so extraction must grow its initial capacity.
    std::vector<ObjectRoot> ancestors;
    for(unsigned i=0;i<8;++i) {
        ancestors.push_back(take(w::create_entity(runtime)));
        take(world.set_transform(ancestors.back().id(),{{1,0,0},{},{1,1,1}}));
        if(i) committed(world.reparent(ancestors[i].id(),ancestors[i-1].id(),w::ReparentRule::keep_local));
    }
    const auto mesh=geometry::cube_mesh();
    auto first=take(p::create_static_mesh_entity(world,{mesh,{{2,0,0},{},{1,1,1}}}));
    auto second=take(p::create_static_mesh_entity(world,{mesh,{{3,0,0},{},{1,1,1}}}));
    committed(world.reparent(first.id(),ancestors.back().id(),w::ReparentRule::keep_local));
    committed(world.reparent(second.id(),ancestors.back().id(),w::ReparentRule::keep_local));
    committed(world.attach(ancestors.front().id()));
    const auto sparse=capture();
    require(sparse.first.size()==2 && sparse.first[0].entity==first.id() && sparse.first[1].entity==second.id() &&
        sparse.first[0].geometry==mesh && sparse.first[1].geometry==mesh &&
        near(sparse.first[0].world[12],10) && near(sparse.first[1].world[12],11),
        "Sparse extraction lost presentation order, geometry or spatial ancestors.");
    require(sparse.second>empty.second,"Shared allocator counters did not observe real render extraction allocations.");
    add_logical_members();
    const auto larger=capture();
    require(larger.first.size()==2 && larger.second==sparse.second,
        "Unrelated logical members increased sparse render extraction allocations.");

    take(runtime.write(ancestors.front().id(),types.position_fields[0],Scalar{5.f}));
    auto moved=take(scene.render_items());
    require(near(moved[0].world[12],14) && near(moved[1].world[12],15),"Extraction hid a direct Runtime transform write.");
    // Isolate (default) skips the dependent meshes and reports the failing Entity
    // once while the failure persists; halt fails the whole extraction.
    const auto break_ancestor=[&] {take(runtime.write(ancestors.front().id(),types.position_fields[0],Scalar{std::numeric_limits<float>::infinity()}));};
    break_ancestor();
    require(take(scene.render_items()).empty() && take(scene.render_items()).empty(),"Isolated extraction kept meshes below a non-finite ancestor.");
    const auto isolated=world.take_diagnostics();
    require(isolated.size()==1 && isolated[0].object==ancestors.front().id() && isolated[0].entity==ancestors.front().id() &&
        isolated[0].kind==w::DiagnosticKind::invalid_value && isolated[0].phase==w::DiagnosticPhase::presentation &&
        isolated[0].severity==w::Severity::error && isolated[0].type=="world.Entity" && isolated[0].repeats==2 && !isolated[0].message.empty(),
        "Isolated extraction did not report the non-finite ancestor once per persisting failure.");
    take(runtime.write(ancestors.front().id(),types.position_fields[0],Scalar{1.f}));
    require(near(take(scene.render_items()).front().world[12],10),"Failed extraction retained invalid spatial state.");
    take(world.set_failure_policy(w::FailurePolicy::halt));
    break_ancestor();
    reject(scene.render_items(),"Halted extraction accepted a non-finite ancestor transform.");
    require(world.take_diagnostics().size()==1,"Halted extraction did not report its resolved-then-repeated failure.");
    take(world.set_failure_policy(w::FailurePolicy::isolate));
    take(runtime.write(ancestors.front().id(),types.position_fields[0],Scalar{1.f}));
    require(near(take(scene.render_items()).front().world[12],10),"Failed extraction retained invalid spatial state.");
    take(p::set_geometry(runtime,sparse.first[0].component,{}));
    take(p::set_geometry(runtime,sparse.first[1].component,{}));
    const auto cleared=capture();
    require(cleared.first.empty() && cleared.first.capacity()==0 && cleared.second==empty.second,
        "Removing all meshes retained allocation beyond the empty return-container baseline.");
    committed(world.close());
    std::printf("[pass] Sparse presentation: empty/logical/light-only/cleared bytes=%lld/%lld/%lld/%lld (capacity=0), sparse bytes=%lld/%lld before/after 256 extra logical Entities; ancestor growth, order, Runtime writes and recovery\n",
        static_cast<long long>(empty.second),static_cast<long long>(logical.second),static_cast<long long>(lights.second),
        static_cast<long long>(cleared.second),static_cast<long long>(sparse.second),static_cast<long long>(larger.second));
}

}

int main() {
    try {
        sparse_presentation();
        editing_and_execution();
        reference_lifetime();
        worlds_and_requests();
        lifecycle_subscription_unsubscribe();
        failures_and_reentry();
        ordered_lifecycle();
        isolated_failures();
        entity_lifecycle_and_detached_graph();
        spatial_tree_and_mesh();
        static_mesh_entity_creation();
        lit_materials_and_lights();
        environment_and_group_writes();
        presentation_cache();
        hierarchical_lifecycle();
        hierarchical_reference_lifetime();
        hierarchical_space_boundaries();
        forest_publication();
        attachment_space_and_authoring();
        mirrored_and_degenerate_space();
        frame_phases();
        std::puts("World acceptance: PASS (21/21)");
        return 0;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "World acceptance: FAIL: %s\n", exception.what());
        return 1;
    }
}
