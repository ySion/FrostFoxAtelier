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

enum class LayerMask : std::uint32_t { none = 0u, world_static = 1u, dynamic_body = 2u };

struct Move : world::Component {
    float speed{2};

    float runtime_meter{1};

    LayerMask mask{LayerMask::world_static};
};

Result<std::shared_ptr<void>> prepare(world::World& world) {
    auto& runtime = world.runtime();
    take(NativeRegistration<LayerMask>{runtime, "game.LayerMask", TypeKind::enumeration}
             .flags()
             .enumerator("none", LayerMask::none, "无")
             .enumerator("world_static", LayerMask::world_static, "静态层", "地形")
             .enumerator("dynamic_body", LayerMask::dynamic_body, "动态层")
             .publish());
    take(NativeRegistration<Entity>{runtime, "game.Entity", TypeKind::object}
             .base<world::Entity>()
             .field<&Entity::health>("health")
             .publish());
    take(NativeRegistration<Move>{runtime, "game.Move", TypeKind::object}
             .base<world::Component>()
             .field<&Move::speed>("speed")
             .field<&Move::runtime_meter>("runtime_meter")
             .transient()
             .read_only()
             .field<&Move::mask>("mask")
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
    const FieldId meter_field = take(runtime.find_field(move_type, "runtime_meter"));
    take(runtime.write(move, std::span{&meter_field, 1}, Scalar{99.f}));
    const auto inspected = take(document.properties(move));
    const auto meter_prop = std::ranges::find(inspected, "runtime_meter", &editor::Property::name);
    require(meter_prop != inspected.end() && meter_prop->transient && meter_prop->read_only &&
                std::get<float>(meter_prop->value) == 99.f,
            "Inspector properties did not expose transient read-only field.");
    const std::array patch{editor::PropertyPatch{{meter_field}, Scalar{5.f}}};
    require(!document.set_properties(move, patch) && !document.begin_edit(move, std::span{&meter_field, 1}, 1),
            "Inspector allowed editing a ReadOnly field.");
    const auto mask_type = take(runtime.find_type("game.LayerMask"));
    const FieldId mask_field = take(runtime.find_field(move_type, "mask"));
    const std::array invalid_mask_patch{editor::PropertyPatch{{mask_field}, Scalar{EnumValue{mask_type, 8u}}}};
    require(!document.set_properties(move, invalid_mask_patch), "Inspector accepted undeclared flag bit.");
    const std::array group_paths{world::FieldPath{mask_field}};
    const std::array invalid_group_vals{Scalar{EnumValue{mask_type, 8u}}};
    take(document.begin_group_edit(move, group_paths, 7));
    require(!document.preview_group_edit(7, invalid_group_vals), "Group edit accepted undeclared flag bit.");
    take(document.cancel_edit());
    const std::array valid_mask_patch{editor::PropertyPatch{{mask_field}, Scalar{EnumValue{mask_type, 3u}}}};
    take(document.set_properties(move, valid_mask_patch));
    auto reopened = take(editor::Session::deserialize(take(document.serialize()), project, prepare));
    auto& restored = reopened->world().runtime();
    const auto members = take(reopened->world().members());
    require(members.size() == 1, "Scene lost its Entity.");
    const auto restored_entity = members.front().id();
    const auto components = take(world::components(restored, restored_entity));
    require(components.size() == 1, "Scene lost its component.");
    const auto restored_move_type = take(restored.find_type("game.Move"));
    const FieldId restored_meter = take(restored.find_field(restored_move_type, "runtime_meter"));
    const FieldId restored_mask = take(restored.find_field(restored_move_type, "mask"));
    require(
        std::get<std::int32_t>(take(restored.read(
            restored_entity, take(restored.find_field(take(restored.find_type("game.Entity")), "health"))))) == 75 &&
            std::get<float>(take(restored.read(components.front().id(),
                                               take(restored.find_field(restored_move_type, "speed"))))) == 4.f &&
            std::get<float>(take(restored.read(components.front().id(), std::span{&restored_meter, 1}))) == 1.f &&
            std::get<EnumValue>(take(restored.read(components.front().id(), std::span{&restored_mask, 1}))).bits == 3u,
        "Scene roundtrip changed authored fields, Flags enum mask or serialized a Transient field.");
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
