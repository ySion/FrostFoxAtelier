#include "../helpers.hpp"
#include "editor/session.hpp"
#include "editor/project.hpp"
#include "runtime/native.hpp"
#include "physics/physics.hpp"
#include <chrono>
#include <cstdio>

namespace {
using namespace atelier;

using namespace atelier::acceptance::helpers;

void changed(Result<world::Commit> result) {
    const auto commit = take(std::move(result));
    if (commit.notification_error)
        fatal(commit.notification_error->message);
}

struct Entity : world::Entity {
    std::int32_t health{100};
};

struct Move : world::Component {
    float speed{2};
};

Result<std::shared_ptr<void>> prepare(world::World& world) {
    auto& runtime = world.runtime();
    take(NativeRegistration<Entity>{runtime, "game.Entity", TypeKind::object}
             .base<world::Entity>()
             .field<&Entity::health>("health")
             .publish());
    take(NativeRegistration<Move>{runtime, "game.Move", TypeKind::object}
             .base<world::Component>()
             .field<&Move::speed>("speed")
             .publish());
    return std::shared_ptr<void>{};
}

void verify_roundtrip() {
    const auto directory = std::filesystem::current_path() / "project-scene-evidence" /
                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto project = take(editor::Project::create(directory));
    editor::Session document;
    take(document.initialize(prepare));
    auto& runtime = document.world().runtime();
    const auto entity_type = take(runtime.find_type("game.Entity"));
    const auto move_type = take(runtime.find_type("game.Move"));
    changed(document.add_entity(entity_type));
    const auto entity = document.selection().entity;
    changed(document.add_component(entity, move_type));
    const auto move = document.selection().object;
    take(runtime.write(entity, take(runtime.find_field(entity_type, "health")), Scalar{std::int32_t{75}}));
    take(runtime.write(move, take(runtime.find_field(move_type, "speed")), Scalar{4.f}));
    auto reopened = take(editor::Session::deserialize(take(document.serialize()), project, prepare));
    auto& restored = reopened->world().runtime();
    const auto members = take(reopened->world().members());
    require(members.size() == 1, "Scene lost its Entity.");
    const auto restored_entity = members.front().id();
    const auto components = take(world::components(restored, restored_entity));
    require(components.size() == 1, "Scene lost its component.");
    require(
        std::get<std::int32_t>(take(restored.read(
            restored_entity, take(restored.find_field(take(restored.find_type("game.Entity")), "health"))))) == 75 &&
            std::get<float>(take(restored.read(
                components.front().id(), take(restored.find_field(take(restored.find_type("game.Move")), "speed"))))) ==
                4.f,
        "Scene roundtrip changed authored health=75 or speed=4.");
    changed(reopened->close());
    changed(document.close());
}

void verify_stop_reentry() {
    editor::Session* current{};
    ObjectId query;
    bool fail_tick{};
    unsigned ends{};
    editor::Session document;
    take(document.initialize([&](world::World& world) -> Result<std::shared_ptr<void>> {
        take(world.set_failure_policy(world::FailurePolicy::halt));
        take(world.bind_behavior(
            world.types().entity,
            {.tick = [&](Runtime&, ObjectId, float) -> Result<void> {
                 if (fail_tick)
                     return std::unexpected(Error{ErrorCode::invocation_failed, "reentry tick failure"});
                 return {};
             },
             .end = [&](Runtime& runtime, ObjectId) -> Result<void> {
                 ++ends;
                 (void)current->stop();
                 // Query state belongs to PhysicsScene and must survive until End returns.
                 require(take(runtime.with_native<const physics::SceneQueryComponent>(
                             query, [](const auto& value) -> Result<bool> { return value.query_ready(); })),
                         "Nested stop destroyed physics before End returned.");
                 return {};
             }}));
        return std::shared_ptr<void>{};
    }));
    current = &document;
    changed(document.add_entity());
    take(document.set_transform(document.selection().entity, {}));
    changed(document.add_component(document.selection().entity, document.physics_types().scene_query_component));
    query = document.selection().object;
    changed(document.start());
    take(document.update(1.f / 60));
    fail_tick = true;
    (void)document.update(1.f / 60);
    require(ends == 1 && !document.world().running() &&
                !take(document.world().runtime().with_native<const physics::SceneQueryComponent>(
                    query, [](const auto& value) -> Result<bool> { return value.query_ready(); })),
            "Failed Tick did not release physics after End.");
    changed(document.close());
}
}

int main() {
    atelier::install_fatal_handler();
    verify_roundtrip();
    verify_stop_reentry();
    std::puts("Project Scene acceptance: PASS (authored scalar roundtrip, End/physics shutdown order)");
}
