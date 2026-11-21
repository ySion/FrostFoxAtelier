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
#include <cstring>
#include <cstdio>
#include <tuple>

void verify_mesh_import();

namespace {
using namespace atelier;

namespace w = atelier::world;

namespace p = atelier::presentation;

using namespace atelier::acceptance::helpers;

void committed(Result<w::Commit> result) {
    const auto commit = take(std::move(result));
    if (commit.notification_error)
        fatal(commit.notification_error->message);
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

void editing_and_execution() {
    Runtime runtime;
    const auto types = register_test_types(runtime);
    const auto derived =
        take(NativeRegistration<DerivedCounter>{runtime, "worldAcceptance.DerivedCounter", TypeKind::object}
                 .base<Counter>()
                 .field<&DerivedCounter::extra>("extra")
                 .publish());
    w::World world;
    take(world.initialize(runtime, types));
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
    require(take(world.members()).front().id() == entity.id() && take(w::read_transform(runtime, entity.id())) == trs,
            "Delete/GC/undo lost identity or data without an offline writer.");
    auto redo = take(world.detach(entity.id()));
    require(take(world.members()).empty(), "Redo left the Entity attached.");
    committed(world.restore(redo));
    committed(world.close());
    committed(w::destroy_entity(runtime, entity.id()));
    require(take(w::inspect_entity(runtime, entity.id())).state == w::EntityState::invalid,
            "Explicit destroy did not invalidate a strongly held Entity.");
    std::puts("[pass] World edit history: tick, detach/GC/restore and redo");
}

void tick_prerequisite_order() {
    Runtime runtime;
    const auto types = take(w::register_types(runtime));
    std::vector<ObjectId> ticks;
    w::World world;
    take(world.initialize(runtime, types));
    auto a = take(w::create_entity(runtime));
    auto b = take(w::create_entity(runtime));
    auto c = take(w::create_entity(runtime));
    for (const auto id : {a.id(), b.id(), c.id()})
        committed(world.attach(id));
    take(world.bind_behavior(types.entity, {.tick = [&](Runtime&, ObjectId id, float) -> Result<void> {
                                 ticks.push_back(id);
                                 return {};
                             }}));
    take(world.add_tick_prerequisite(a.id(), b.id()));
    committed(world.start());
    take(world.update(1.f));
    require(ticks == std::vector<ObjectId>{b.id(), a.id(), c.id()},
            "A newly ready earlier entity did not precede the unrelated ready entity.");
    committed(world.close());
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

    w::World world;
    take(world.initialize(runtime, types));
    take(world.set_failure_policy(w::FailurePolicy::halt));
    auto entity = take(w::create_entity(runtime));
    auto a = take(w::create_component(runtime, types.counter));
    auto b = take(w::create_component(runtime, types.counter));
    auto c = take(w::create_component(runtime, derived));
    committed(world.attach(entity.id()));
    committed(world.attach_components(entity.id(),
                                      std::array{w::Attachment{a.id()}, w::Attachment{b.id()}, w::Attachment{c.id()}}));
    take(world.bind_behavior(types.counter, {.begin = [&](Runtime&, ObjectId id) -> Result<void> {
                                                 events.emplace_back('B', id);
                                                 if (id == c.id())
                                                     return std::unexpected(Error{ErrorCode::invocation_failed,
                                                                                  "Injected Begin failure."});
                                                 return {};
                                             },
                                             .end = [&](Runtime&, ObjectId id) -> Result<void> {
                                                 events.emplace_back('E', id);
                                                 return {};
                                             }}));
    (void)take(world.start());
    committed(world.close());
    require(!world.running() &&
                events == std::vector<Event>{{'B', a.id()}, {'B', b.id()}, {'B', c.id()}, {'E', b.id()}, {'E', a.id()}},
            "Failed Begin did not unwind exactly the successful prefix once, in reverse order.");
}

void spatial_tree_and_mesh() {
    Runtime runtime;
    const auto types = register_all_types(runtime);
    w::World world;
    take(world.initialize(runtime, types));
    const auto scene = take(p::Scene::create(world, types));
    auto entity = take(p::create_static_mesh_entity(
        world, {geometry::cube_mesh(), {{10, 0, 0}, {0, 0, std::sqrt(.5f), std::sqrt(.5f)}, {2, 2, 2}}}));
    auto child = take(w::create_entity(runtime));
    auto leaf = take(w::create_entity(runtime));
    auto mesh = take(w::create_component(runtime, types.static_mesh_component));
    auto triangle = take(geometry::Mesh::create(
        std::vector<geometry::Vertex>{{{0, 0, 0}, {0, 0, -1}}, {{1, 0, 0}, {0, 0, -1}}, {{0, 1, 0}, {0, 0, -1}}}));
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
    committed(world.close());
    committed(w::destroy_entity(runtime, entity.id()));
}

}

int main() {
    atelier::install_fatal_handler();
    editing_and_execution();
    ordered_lifecycle();
    tick_prerequisite_order();
    spatial_tree_and_mesh();
    verify_mesh_import();
    std::puts("World acceptance: PASS (edit history, tick prerequisites, transform and failed Begin cleanup)");
    return 0;
}
