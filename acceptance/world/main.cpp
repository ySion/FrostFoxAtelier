#include "memory/fatal.hpp"
#include "../helpers.hpp"
#include "presentation/presentation.hpp"
#include "material/resources.hpp"
#include "runtime/native.hpp"
#include "source/program.hpp"
#include "world/detail.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <tuple>

namespace {
using namespace atelier;

namespace w = atelier::world;

namespace p = atelier::presentation;

using namespace atelier::acceptance::helpers;

void committed(Result<w::Commit> result) {
    const auto commit = take(std::move(result));
    if (commit.notification_error)
        throw std::runtime_error(commit.notification_error->message);
}

template <class T>
void reject(const Result<T>& result, const char* message) {
    require(!result, message);
}

void collect(Runtime& runtime) {
    if (runtime.collection_phase() == CollectionPhase::idle)
        take(runtime.begin_collection());
    do {
        take(runtime.collect_step({16}));
    } while (runtime.collection_phase() != CollectionPhase::idle);
}

bool near(float actual, float expected) {
    return std::abs(actual - expected) < 0.0001f;
}

struct Counter : w::Component {
    float value{};

    std::uint32_t steps{};

    void advance(float amount) noexcept {
        value += amount;
        ++steps;
    }
};

struct DerivedCounter : Counter {
    float extra{7.f};
};

struct Holder {
    ReferenceSlot entity;

    ReferenceSlot component;
};

TypeId holder_type(Runtime& runtime, const w::Types& types) {
    return take(NativeRegistration<Holder>{runtime, "worldAcceptance.Holder", TypeKind::object}
                    .field<&Holder::entity>("entity", {types.entity, ValueForm::strong_reference})
                    .field<&Holder::component>("component", {types.component, ValueForm::strong_reference})
                    .publish());
}

// World and presentation types, as hosts register them.
struct AllTypes : w::Types, p::Types {};

AllTypes register_all_types(Runtime& runtime) {
    AllTypes types;
    static_cast<w::Types&>(types) = take(w::register_types(runtime));
    static_cast<p::Types&>(types) = take(p::register_types(runtime));
    return types;
}

struct TestTypes : AllTypes {
    TypeId counter;

    FieldId counter_value;
};

TestTypes register_test_types(Runtime& runtime) {
    TestTypes types;
    static_cast<AllTypes&>(types) = register_all_types(runtime);
    types.counter = take(NativeRegistration<Counter>{runtime, "worldAcceptance.Counter", TypeKind::object}
                             .base<w::Component>()
                             .field<&Counter::value>("value")
                             .field<&Counter::steps>("steps")
                             .method<&Counter::advance>("advance")
                             .publish());
    types.counter_value = take(runtime.find_field(types.counter, "value"));
    return types;
}

void publish(source::Program& program, std::string_view text) {
    auto candidate = take(source::Program::prepare(take(program.request("world-acceptance.source", text))));
    take(program.try_publish(candidate));
}

material::DefinitionRef mesh_material_definition() {
    static const auto definition = [] {
        Runtime compiler;
        take(material::register_program_types(compiler));
        const auto program = take(source::compile_material(
            compiler, take(compiler.native_type<material::Context>()), take(compiler.native_type<material::Output>()),
            take(compiler.native_type<material::TextureInput>()), "world-material.source", R"(
                struct Params { vector3 tint; float roughness; float metallic; vector3 emission; };
                Params defaults() {
                    Params p; p.tint=vector3(0.2,0.3,0.4); p.roughness=0.25;
                    p.metallic=0.75; p.emission=vector3(4.0,8.0,16.0); return p;
                }
                material.Output shade(material.Context c, Params p) {
                    material.Output o; o.base_color=p.tint; o.roughness=p.roughness;
                    o.metallic=p.metallic; o.emissive=p.emission; o.coverage=1.0; o.normal_world=c.normal_world; return o;
                }
            )"));
        return take(material::make_definition(program, {}, {}));
    }();
    return definition;
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
    const auto derived =
        take(NativeRegistration<DerivedCounter>{runtime, "worldAcceptance.DerivedCounter", TypeKind::object}
                 .base<Counter>()
                 .field<&DerivedCounter::extra>("extra")
                 .publish());
    w::World world(runtime, types);
    const auto scene = take(p::Scene::create(world, types));
    auto entity = take(w::create_entity(runtime));
    committed(world.attach(entity.id()));
    auto cube = take(w::create_component(runtime, types.static_mesh_component));
    auto collision = take(w::create_component(runtime, types.collision_component));
    take(world.set_transform(entity.id(), {}));
    const std::array added{w::Attachment{collision.id()}, w::Attachment{cube.id()}};
    committed(world.attach_components(entity.id(), added));
    take(p::set_geometry(runtime, cube.id(), geometry::cube_mesh()));
    w::Trs trs{{4.f, 5.f, 6.f}, {0.f, 0.f, std::sqrt(.5f), std::sqrt(.5f)}, {2.f, 3.f, 4.f}};
    take(world.set_transform(entity.id(), trs));
    const auto matrix = take(scene.render_items()).front().world;
    require(near(matrix[0], 0.f) && near(matrix[1], 2.f) && near(matrix[4], -3.f) && near(matrix[10], 4.f) &&
                matrix[12] == 4.f,
            "TRS did not produce S*R*T row-vector geometry.");
    const auto original = take(w::read_transform(runtime, entity.id()));
    auto counter = take(w::create_component(runtime, derived));
    const std::array counter_added{w::Attachment{counter.id()}};
    committed(world.attach_components(entity.id(), counter_added));
    auto program = take(source::Program::create(runtime, types.counter));
    publish(*program, "void step(float amount) { self.advance(amount); }");
    const auto entry = take(program->entry().bind<void, float>());
    take(
        world.bind_behavior(types.counter, {.tick = [entry](Runtime&, ObjectId receiver, float amount) -> Result<void> {
                                return entry.invoke(receiver, amount);
                            }}));
    committed(world.start());
    take(world.update(2.f));
    require(take(w::inspect_entity(runtime, entity.id())).state == w::EntityState::active &&
                std::get<float>(take(runtime.read(counter.id(), types.counter_value))) == 2.f,
            "JIT did not update the same derived Counter.");
    auto deferred = take(source::Program::create(runtime, types.counter));
    publish(*deferred, "async void later() { yield; self.advance(50.0); }");
    const auto later = deferred->entry();
    take(world.start_task(counter.id(), [later](Runtime&, ObjectId receiver) { return later.start(receiver, {}); }));
    require(w::detail::Access::state(world).tasks.size() == 1, "World did not own the real coroutine.");
    take(world.update(1.f)); // task suspends after its yield
    committed(world.set_enabled(entity.id(), false));
    require(w::detail::Access::state(world).tasks.size() == 0, "Entity deactivation did not cancel its task.");
    take(world.update(1.f));
    require(std::get<float>(take(runtime.read(counter.id(), types.counter_value))) == 3.f,
            "Cancelled coroutine resumed or inactive Entity ran.");
    committed(world.set_enabled(entity.id(), true));
    committed(world.stop());
    auto removal = take(world.detach(entity.id()));
    const auto entity_id = entity.id();
    const auto collision_id = collision.id();
    const auto cube_id = cube.id();
    const auto counter_id = counter.id();
    entity.reset();
    collision.reset();
    cube.reset();
    counter.reset();
    require(runtime.root_count() == 1, "Delete/undo sample still has roots outside its Entity removal record.");
    collect(runtime);
    require(take(w::inspect_entity(runtime, entity_id)).state == w::EntityState::detached &&
                take(scene.render_items()).empty(),
            "Entity detach left participation.");
    committed(world.restore(removal));
    entity = take(runtime.hold(entity_id));
    collision = take(runtime.hold(collision_id));
    cube = take(runtime.hold(cube_id));
    counter = take(runtime.hold(counter_id));
    require(take(world.members()).front().id() == entity.id() &&
                take(w::read_transform(runtime, entity.id())) == original,
            "Delete/GC/undo lost identity or data without an offline writer.");
    committed(world.start());
    take(world.start_task(counter.id(), [later](Runtime&, ObjectId receiver) { return later.start(receiver, {}); }));
    take(world.update(0.f));
    require(w::detail::Access::state(world).tasks.size() == 1, "Close sample did not contain a suspended real task.");
    committed(world.close());
    require(w::detail::Access::state(world).tasks.size() == 0, "Close retained real task frames.");
    committed(w::destroy_entity(runtime, entity.id()));
    require(take(w::inspect_entity(runtime, entity.id())).state == w::EntityState::invalid,
            "Explicit destroy did not invalidate a strongly held Entity.");
    std::puts("[pass] World TRS and JIT: derived receiver, task cancellation and sole-root GC retention");
}

void reference_lifetime() {
    Runtime runtime;
    const auto types = register_all_types(runtime);
    const auto holder = holder_type(runtime, types);
    auto keeper = take(runtime.create(holder));
    const auto entity_field = take(runtime.find_field(holder, "entity"));
    const auto component_field = take(runtime.find_field(holder, "component"));
    ObjectId entity_id;
    ObjectId cube_id;
    ObjectId sibling_id;
    {
        w::World world(runtime, types);
        auto entity = take(w::create_entity(runtime));
        entity_id = entity.id();
        committed(world.attach(entity_id));
        auto shape = add_shape(runtime, types, world, entity_id);
        sibling_id = shape[0].id();
        cube_id = shape[1].id();
        take(world.write(entity_id, types.position_fields[1], Scalar{23.f}));
        take(runtime.write_reference(keeper.id(), entity_field, entity.reference()));
        auto removed = take(world.detach(entity_id));
        removed.root.reset();
        entity.reset();
        shape.clear();
        collect(runtime);
        require(take(w::components(runtime, entity_id)).size() == 2 && runtime.type_of(cube_id).has_value(),
                "Ordinary Entity reference did not retain the detached composition.");
        auto cube = take(runtime.hold(cube_id, types.component));
        take(runtime.write_reference(keeper.id(), component_field, cube.reference()));
        take(runtime.write_reference(keeper.id(), entity_field, take(runtime.null_reference(types.entity))));
        cube.reset();
        collect(runtime);
        require(runtime.type_of(entity_id).has_value() && runtime.type_of(sibling_id).has_value(),
                "Component strong owner did not retain Entity and siblings.");
        committed(world.attach(entity_id));
        committed(world.close());
    }
    collect(runtime);
    require(take(w::inspect_entity(runtime, entity_id)).state == w::EntityState::detached &&
                take(w::components(runtime, entity_id)).size() == 2,
            "Destroying the old World invalidated retained data.");
    require(take(w::read_transform(runtime, entity_id)).position.y == 23.f,
            "World close lost retained component field values.");
    committed(w::destroy_entity(runtime, entity_id));
    require(!runtime.type_of(entity_id) && !runtime.type_of(cube_id) && !runtime.type_of(sibling_id),
            "Detached destruction did not terminate the complete group.");
    require(take(w::inspect_component(runtime, cube_id)).valid == false, "Destroyed component remained valid.");
    keeper.reset();
    collect(runtime);
    require(runtime.live_objects() == 0 && runtime.root_count() == 0, "Destruction retained hidden roots.");

    // Independent sample: no explicit destroy, including the Entity<->Component cycle.
    {
        w::World world(runtime, types);
        auto entity = take(w::create_entity(runtime));
        entity_id = entity.id();
        committed(world.attach(entity_id));
        auto shape = add_shape(runtime, types, world, entity_id);
        committed(world.close());
        entity.reset();
        shape.clear();
    }
    collect(runtime);
    require(runtime.live_objects() == 0 && runtime.root_count() == 0 &&
                take(w::inspect_entity(runtime, entity_id)).state == w::EntityState::invalid,
            "Unreachable strong composition cycle did not collect.");
    std::puts(
        "[pass] World lifetime: ordinary Entity/Component fields retain the graph, close and old-World destruction, explicit termination, independent GC");
}

void worlds_and_requests() {
    Runtime runtime;
    const auto types = register_all_types(runtime);
    w::World first(runtime, types);
    w::World second(runtime, types);
    const auto second_scene = take(p::Scene::create(second, types));
    auto entity = take(w::create_entity(runtime));
    committed(first.attach(entity.id()));
    auto shape = add_shape(runtime, types, first, entity.id());
    const auto pending = take(first.request({w::RequestKind::destroy_entity, entity.id()}));
    require(runtime.type_of(entity.id()).has_value(), "Queued destroy terminated identity before commit.");
    auto removed = take(first.detach(entity.id()));
    committed(second.attach(entity.id()));
    const auto rejected = take(first.flush());
    require(rejected.size() == 1 && rejected[0].request == pending && !rejected[0].result &&
                runtime.type_of(entity.id()).has_value(),
            "Old World queued destroy followed the Entity into a different World.");
    committed(first.close());
    require(take(second.members()).size() == 1 && take(second_scene.render_items()).size() == 1,
            "Closing one World affected another.");
    const auto destruction = take(second.request({w::RequestKind::destroy_entity, entity.id()}));
    auto destroyed = take(second.flush());
    require(destroyed.size() == 1 && destroyed[0].request == destruction && destroyed[0].result &&
                !runtime.type_of(entity.id()),
            "Queued destruction did not commit through the object protocol.");
    committed(second.close());
    std::puts("[pass] World isolation: queued destruction cannot follow an Entity into another World");
}

void lifecycle_subscription_unsubscribe() {
    Runtime runtime;
    const auto types = register_all_types(runtime);
    w::World world(runtime, types);
    auto entity = take(w::create_entity(runtime));
    auto component = take(w::create_component(runtime, types.component));
    std::uint64_t skipped{};
    std::uint64_t self{};
    std::size_t first_calls{};
    std::size_t skipped_calls{};
    std::size_t self_calls{};
    std::size_t stateful_calls{};
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
    require(first_calls == 2 && skipped_calls == 0 && self_calls == 1, "Lifecycle callbacks ran after unsubscription.");
}

void failures_and_reentry() {
    Runtime runtime;
    const auto types = register_all_types(runtime);
    w::World world(runtime, types);
    take(world.set_failure_policy(w::FailurePolicy::halt));
    auto entity = take(w::create_entity(runtime));
    committed(world.attach(entity.id()));
    std::size_t close_callbacks{};
    take(world.observe_lifecycle([&](w::World&, w::Lifecycle event, ObjectId target) -> Result<void> {
        if (event == w::Lifecycle::detached) {
            ++close_callbacks;
            require(take(w::inspect_entity(runtime, target)).state == w::EntityState::detached,
                    "Close notification did not expose the complete Detached state.");
        }
        return {};
    }));
    committed(world.close());
    require(close_callbacks == 1 && w::detail::Access::state(world).tasks.size() == 0, "Close was not complete.");
    committed(w::destroy_entity(runtime, entity.id()));
    std::puts("[pass] World close: exit callback observes the complete Detached state");
}

void ordered_lifecycle() {
    Runtime runtime;
    const auto types = register_test_types(runtime);
    const auto derived =
        take(NativeRegistration<DerivedCounter>{runtime, "worldAcceptance.OrderedDerived", TypeKind::object}
                 .base<Counter>()
                 .field<&DerivedCounter::extra>("extra")
                 .publish());
    using Event = std::pair<char, ObjectId>;
    std::vector<Event> events;
    ObjectId fail_begin;
    ObjectId fail_end;
    w::World world(runtime, types);
    take(world.set_failure_policy(w::FailurePolicy::halt));
    auto entity = take(w::create_entity(runtime));
    auto second_entity = take(w::create_entity(runtime));
    auto a = take(w::create_component(runtime, types.counter));
    auto b = take(w::create_component(runtime, derived));
    auto c = take(w::create_component(runtime, types.counter));
    auto d = take(w::create_component(runtime, derived));
    auto inserted = take(w::create_component(runtime, types.counter));
    committed(world.attach(entity.id()));
    committed(world.attach(second_entity.id()));
    const std::array group{w::Attachment{a.id()}, w::Attachment{b.id()}, w::Attachment{c.id()}};
    committed(world.attach_components(entity.id(), group));
    const std::array other{w::Attachment{d.id()}};
    committed(world.attach_components(second_entity.id(), other));
    const auto callback = [&](char phase, ObjectId id, ObjectId failure) -> Result<void> {
        events.emplace_back(phase, id);
        if (id == failure)
            return std::unexpected(Error{ErrorCode::invocation_failed, "Intentional lifecycle failure."});
        return {};
    };
    take(world.bind_behavior(types.counter,
                             {.begin = [&](Runtime&, ObjectId id) { return callback('B', id, fail_begin); },
                              .end = [&](Runtime&, ObjectId id) { return callback('E', id, fail_end); }}));
    committed(world.set_enabled(b.id(), false));
    committed(world.start());
    require(events == std::vector<Event>{{'B', a.id()}, {'B', b.id()}, {'B', c.id()}, {'B', d.id()}},
            "Begin order depended on TypeId or skipped disabled components.");
    events.clear();
    const std::array addition{w::Attachment{inserted.id()}};
    committed(world.attach_components(entity.id(), addition, 1));
    events.clear();
    auto removal = take(world.remove_component(b.id()));
    require(events == std::vector<Event>{{'E', b.id()}}, "Dynamic removal did not pair End.");
    events.clear();
    committed(world.restore(removal));
    require(events == std::vector<Event>{{'B', b.id()}}, "Dynamic restore did not start a new participation interval.");
    events.clear();
    committed(world.stop());
    const std::vector<Event> reverse{{'E', d.id()}, {'E', c.id()}, {'E', b.id()}, {'E', inserted.id()}, {'E', a.id()}};
    require(events == reverse, "End did not reverse the current member/composition order.");
    events.clear();
    fail_begin = b.id();
    auto failed = take(world.start());
    require(failed.changed && failed.notification_error && !world.running() &&
                events ==
                    std::vector<Event>{
                        {'B', a.id()}, {'B', inserted.id()}, {'B', b.id()}, {'E', inserted.id()}, {'E', a.id()}},
            "Begin failure did not unwind exactly the successfully begun prefix.");
    fail_begin = {};
    events.clear();
    committed(world.start());
    events.clear();
    fail_end = c.id();
    failed = take(world.stop());
    require(failed.notification_error && events == reverse, "End failure abandoned later cleanup.");
    fail_end = {};
    events.clear();
    committed(world.close());
    std::puts("[pass] Ordered lifecycle: participation pairing, reverse End and failed Begin/End cleanup");
}

void isolated_failures() {
    Runtime runtime;
    const auto types = register_test_types(runtime);
    auto ticking = take(source::Program::create(runtime, types.counter));
    auto waiting = take(source::Program::create(runtime, types.counter));
    auto candidate = take(source::Program::prepare(
        take(ticking->request("world-failure.source", "void tick(float dt) {\n    self.steps = 10 / self.steps;\n}\n",
                              {}, {.diagnostics = true}))));
    take(ticking->try_publish(candidate));
    publish(*waiting, "async void later() { yield; self.steps = 10 / self.steps; }");
    const auto script = take(ticking->pin());
    const auto later = waiting->entry();
    const auto steps = take(runtime.find_field(types.counter, "steps"));
    w::World world(runtime, types);
    auto e = take(w::create_entity(runtime));
    auto f = take(w::create_entity(runtime));
    auto g = take(w::create_entity(runtime));
    auto a = take(w::create_component(runtime, types.counter));
    auto b = take(w::create_component(runtime, types.counter));
    auto c = take(w::create_component(runtime, types.counter));
    auto d = take(w::create_component(runtime, types.counter));
    auto m = take(w::create_component(runtime, types.counter));
    const auto a_id = a.id();
    const auto b_id = b.id();
    const auto c_id = c.id();
    const auto d_id = d.id();
    const auto m_id = m.id();
    take(world.set_transform(g.id(), {}));
    // A registered simulation owns g's pose, as PhysicsScene does for bodies.
    bool throw_relocation{};
    take(world.add_simulation({.step = [](float) -> Result<void> { return {}; },
                               .writes_pose = [g = g.id()](ObjectId entity) { return entity == g; },
                               .relocated = [&](std::span<const ObjectId>) -> Result<void> {
                                   if (throw_relocation)
                                       throw std::runtime_error("Intentional relocation exception.");
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
    take(world.bind_behavior(types.counter,
                             {.begin = [&](Runtime&, ObjectId id) -> Result<void> {
                                  events.emplace_back('B', id);
                                  if (id == fail_begin)
                                      return std::unexpected(
                                          Error{ErrorCode::invocation_failed, "Intentional isolated Begin failure."});
                                  return {};
                              },
                              .tick = [&](Runtime&, ObjectId id, float dt) -> Result<void> {
                                  events.emplace_back('T', id);
                                  if (id == b_id) {
                                      source::Diagnostics capture;
                                      const std::array arguments{Scalar{dt}};
                                      if (auto invoked = script.invoke(id, arguments, &capture); !invoked)
                                          return std::unexpected(invoked.error());
                                  }
                                  return {};
                              },
                              .end = [&](Runtime&, ObjectId id) -> Result<void> {
                                  events.emplace_back('E', id);
                                  return {};
                              }}));
    committed(world.attach(e.id()));
    committed(world.attach(f.id()));
    committed(world.attach(g.id()));

    fail_begin = a_id;
    committed(world.start());
    require(world.running() &&
                events == std::vector<Event>{{'B', a_id}, {'B', b_id}, {'B', c_id}, {'B', d_id}, {'B', m_id}},
            "Isolated Begin failure stopped the World or skipped later objects.");
    const auto starter = [later](Runtime&, ObjectId id) { return later.start(id, {}); };
    take(world.start_task(c_id, starter));
    events.clear();
    take(world.update(1));
    require(events == std::vector<Event>{{'T', b_id}, {'T', c_id}, {'T', d_id}, {'T', m_id}} && world.running(),
            "Tick visited a Begin-faulted object or stopped at the first isolated Tick failure.");
    take(world.update(1));
    events.clear();
    take(world.update(1));
    require(events == std::vector<Event>{{'T', d_id}, {'T', m_id}} && w::detail::Access::state(world).tasks.size() == 0,
            "Tick or task failure did not stop only the failing object's Tick and tasks.");

    // A host clears faults after publishing fixed code: Tick resumes without a
    // second Begin, and the Begin-faulted object begins through the running path.
    take(runtime.write(b_id, steps, Scalar{std::uint32_t{1}}));
    fail_begin = {};
    events.clear();
    committed(world.clear_faults());
    require(events == std::vector<Event>{{'B', a_id}},
            "clear_faults repeated Begin or skipped a Begin-faulted object.");
    events.clear();
    take(world.update(1));
    require(events == std::vector<Event>{{'T', a_id}, {'T', b_id}, {'T', c_id}, {'T', d_id}, {'T', m_id}},
            "Cleared objects did not resume Tick.");
    take(world.set_transform(g.id(), {{0, 5, 0}, {}, {1, 1, 1}}));
    const auto before_exception = take(w::read_transform(runtime, g.id()));
    throw_relocation = true;
    try {
        (void)world.set_transform(g.id(), {{2, 9, -3}, {0, 0, 1, 0}, {2, 3, 4}});
        require(world.running() && take(w::read_transform(runtime, g.id())) == before_exception,
                "A relocation exception changed the TRS or stopped the World.");
        throw_relocation = false;
        const w::Trs after_exception{{-2, 6, 3}, {}, {1, 2, 1}};
        take(world.set_transform(g.id(), after_exception));
        require(take(w::read_transform(runtime, g.id())) == after_exception,
                "An edit after a relocation exception did not publish its transform.");
    } catch (const std::exception& exception) {
        // A broken relocation guard also blocks World destruction. Preserve the
        // first failure and exit nonzero without unwinding through that World.
        std::fprintf(stderr, "World acceptance: FAIL (relocation exception recovery): %s\n", exception.what());
        std::fflush(nullptr);
        std::_Exit(EXIT_FAILURE);
    } catch (...) {
        std::fputs("World acceptance: FAIL (relocation exception recovery): unknown exception\n", stderr);
        std::fflush(nullptr);
        std::_Exit(EXIT_FAILURE);
    }
    events.clear();
    committed(world.stop());
    require(events == std::vector<Event>{{'E', m_id}, {'E', d_id}, {'E', c_id}, {'E', b_id}, {'E', a_id}},
            "Isolation broke End pairing or reverse order.");
    fail_begin = d_id;
    committed(world.start());
    events.clear();
    committed(world.stop());
    require(events == std::vector<Event>{{'E', m_id}, {'E', c_id}, {'E', b_id}, {'E', a_id}},
            "A Begin-faulted object received End.");
    committed(world.close());
    std::puts(
        "[pass] Object isolation: Source task cleanup, fault recovery, relocation exception recovery and End pairing");
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
    auto entity = take(p::create_static_mesh_entity(
        world, {geometry::cube_mesh(), {{10, 0, 0}, {0, 0, std::sqrt(.5f), std::sqrt(.5f)}, {2, 2, 2}}}));
    auto child = take(w::create_entity(runtime));
    auto leaf = take(w::create_entity(runtime));
    auto mesh = take(w::create_component(runtime, types.static_mesh_component));
    auto triangle = std::make_shared<const geometry::Mesh>(
        std::vector<geometry::Vertex>{{{0, 0, 0}, {0, 0, -1}}, {{1, 0, 0}, {0, 0, -1}}, {{0, 1, 0}, {0, 0, -1}}});
    take(p::set_geometry(runtime, mesh.id(), triangle));
    const std::array attachments{w::Attachment{mesh.id()}};
    take(world.set_transform(child.id(), {{1, 0, 0}, {}, {1, 1, 1}}));
    take(world.set_transform(leaf.id(), {{0, 3, 0}, {}, {1, 1, 1}}));
    committed(world.attach_components(leaf.id(), attachments));
    committed(world.reparent(leaf.id(), child.id(), w::ReparentRule::keep_local));
    committed(world.reparent(child.id(), entity.id(), w::ReparentRule::keep_local));
    committed(world.attach(entity.id()));
    auto before = take(w::world_matrix(runtime, leaf.id()));
    require(near(before[12], 4) && near(before[13], 2), "Hierarchy did not compose Entity local transforms.");
    take(world.set_transform(child.id(), {{}, {}, {2, 1, 1}}));
    take(world.set_transform(leaf.id(), {{}, {0, 0, std::sin(.4f), std::cos(.4f)}, {1, 1, 1}}));
    const auto local = take(w::read_transform(runtime, leaf.id()));
    auto removal = take(world.remove_component(mesh.id()));
    require(take(w::read_transform(runtime, leaf.id())) == local && take(w::has_transform(runtime, leaf.id())),
            "Deleting Mesh capability deleted or changed Entity space.");
    committed(world.restore(removal));
    require(take(scene.render_items()).back().geometry == triangle, "Undo lost geometry identity.");
    committed(world.close());
    committed(w::destroy_entity(runtime, entity.id()));
    require(!runtime.type_of(child.id()) && !runtime.type_of(leaf.id()) && !runtime.type_of(mesh.id()),
            "Entity destruction left owned descendants alive.");
    std::puts("[pass] Entity space: hierarchy math, Mesh state restoration and recursive destruction");
}

}

int main() {
    atelier::install_fatal_handler();
    try {
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
        environment_and_group_writes();
        material_dependency_updates();
        hierarchical_lifecycle();
        hierarchical_reference_lifetime();
        hierarchical_space_boundaries();
        forest_publication();
        attachment_space_and_authoring();
        mirrored_and_degenerate_space();
        frame_phases();
        std::puts("World acceptance: PASS (19/19)");
        return 0;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "World acceptance: FAIL: %s\n", exception.what());
        return 1;
    }
}
