#include "memory/fatal.hpp"
#include "../helpers.hpp"
#include "editor/session.hpp"
#include "editor/project.hpp"
#include "assets/file.hpp"
#include "assets/material_format.hpp"
#include "editor/properties.hpp"
#include "runtime/foundation.hpp"
#include "runtime/native.hpp"
#include "source/program.hpp"
#include "physics/physics.hpp"
#include "platform/utf.hpp"
#include "assets/json_format.hpp"
#include <bit>
#include <chrono>
#include <fstream>
#include <cstdio>
#include <stdexcept>

namespace {
using namespace atelier;

const char* acceptance_phase = "document initialization";

using namespace atelier::acceptance::helpers;

void changed(Result<world::Commit> value) {
    auto commit = take(std::move(value));
    if (commit.notification_error)
        throw std::runtime_error(commit.notification_error->message);
}

void material_float(editor::Session& session, ObjectId object, std::string_view path, float value,
                    std::uint64_t control) {
    take(session.begin_material_edit(object, control));
    take(session.preview_material_entry(control, {std::string(path), value, true}));
    take(session.commit_material_edit(control));
}

float roughness(editor::Session& session, ObjectId object) {
    return take(take(material::capture_program(session.world().runtime(), object))->evaluate({})).roughness;
}

struct Entity : world::Entity {
    std::int32_t health{100};
};

struct Move : world::Component {
    float speed{2};

    double mass{3.5};

    std::uint32_t count{7};

    bool allowed{true};
};

struct Spatial : world::Component {
    float offset{4};
};

struct QueryProbe : physics::SceneQueryComponent {
    bool observed{};

    std::uint32_t count{};

    float distance{};

    math::Vector3 point{};
};

struct Caption {
    String text;

    float size{1};

    std::uint32_t lines{1};
};

struct Label : world::Component {
    String title;

    Caption caption;
};

struct Tags : world::Component {
    Array<String> names;

    Array<float> weights;

    Array<std::uint32_t> counts;
};

struct DerivedEnvironment : presentation::EnvironmentComponent {};

struct Evidence {
    unsigned prepared{};

    unsigned destroyed{};

    unsigned begins{};

    unsigned ends{};

    bool fail_tick{};

    bool fail_begin{};

    bool fail_end{};

    bool live_runtime_on_release{true};
};

struct Code {
    Runtime& runtime;

    std::unique_ptr<source::Program> program;

    std::shared_ptr<Evidence> evidence;

    Code(Runtime& r, TypeId type, std::shared_ptr<Evidence> e)
        : runtime(r), program(take(source::Program::create(r, type))), evidence(std::move(e)) {}

    ~Code() {
        evidence->live_runtime_on_release &= !runtime.types().empty();
        ++evidence->destroyed;
    }
};

editor::Session::Prepare prepare(std::shared_ptr<Evidence> evidence) {
    return [evidence](world::World& world) -> Result<std::shared_ptr<void>> {
        auto& runtime = world.runtime();
        ++evidence->prepared;
        take(world.set_failure_policy(world::FailurePolicy::halt));
        take(NativeRegistration<Entity>{runtime, "game.Entity", TypeKind::object}
                 .base<world::Entity>()
                 .field<&Entity::health>("health")
                 .publish());
        const auto move = take(NativeRegistration<Move>{runtime, "game.Move", TypeKind::object}
                                   .base<world::Component>()
                                   .field<&Move::speed>("speed")
                                   .field<&Move::mass>("mass")
                                   .field<&Move::count>("count")
                                   .field<&Move::allowed>("allowed")
                                   .publish());
        take(NativeRegistration<Spatial>{runtime, "game.Spatial", TypeKind::object}
                 .base<world::Component>()
                 .field<&Spatial::offset>("offset")
                 .publish());
        take(NativeRegistration<Caption>{runtime, "game.Caption", TypeKind::record}
                 .field<&Caption::text>("text")
                 .field<&Caption::size>("size")
                 .range({.hard_min = Scalar{0.f}, .hard_max = Scalar{10.f}})
                 .field<&Caption::lines>("lines")
                 .publish());
        take(NativeRegistration<Label>{runtime, "game.Label", TypeKind::object}
                 .base<world::Component>()
                 .field<&Label::title>("title")
                 .field<&Label::caption>("caption")
                 .publish());
        take(NativeRegistration<DerivedEnvironment>{runtime, "game.Environment", TypeKind::object}
                 .base<presentation::EnvironmentComponent>()
                 .publish());
        auto code = std::make_shared<Code>(runtime, move, evidence);
        auto candidate = take(source::Program::prepare(take(code->program->request(
            "Move.source", "void tick(float dt) { self.translate_owner(self.speed * dt, 0.0f, 0.0f); }"))));
        take(code->program->try_publish(candidate));
        auto lease = take(code->program->pin());
        take(world.bind_behavior(
            move, {[evidence](Runtime&, ObjectId) -> Result<void> {
                       ++evidence->begins;
                       if (evidence->fail_begin)
                           return std::unexpected(Error{ErrorCode::invocation_failed, "begin failed"});
                       return {};
                   },
                   [lease, evidence](Runtime&, ObjectId object, float dt) -> Result<void> {
                       const std::array<Scalar, 1> arguments{dt};
                       auto called = lease.invoke(object, arguments);
                       if (!called)
                           return std::unexpected(called.error());
                       if (evidence->fail_tick)
                           return std::unexpected(Error{ErrorCode::invocation_failed, "tick failed after enqueue"});
                       return {};
                   },
                   [evidence](Runtime&, ObjectId) -> Result<void> {
                       ++evidence->ends;
                       if (evidence->fail_end)
                           return std::unexpected(Error{ErrorCode::invocation_failed, "end failed"});
                       return {};
                   }}));
        return std::static_pointer_cast<void>(code);
    };
}

// The scene types plus a component of inline Arrays whose weights and counts are bounded per element.
editor::Session::Prepare prepare_tags(std::shared_ptr<Evidence> evidence) {
    return [base = prepare(std::move(evidence))](world::World& world) -> Result<std::shared_ptr<void>> {
        auto code = base(world);
        if (!code)
            return code;
        auto& runtime = world.runtime();
        take(runtime.register_types({array_definition<String>(runtime, "game.Strings"),
                                     array_definition<float>(runtime, "game.Floats"),
                                     array_definition<std::uint32_t>(runtime, "game.Counts")}));
        take(NativeRegistration<Tags>{runtime, "game.Tags", TypeKind::object}
                 .base<world::Component>()
                 .field<&Tags::names>("names")
                 .field<&Tags::weights>("weights")
                 .range({.hard_min = Scalar{0.f}, .hard_max = Scalar{1.f}})
                 .field<&Tags::counts>("counts")
                 .range({.hard_min = Scalar{std::uint32_t{0}}, .hard_max = Scalar{std::uint32_t{3}}})
                 .publish());
        return code;
    };
}

ObjectId member(editor::Session& session, TypeId type) {
    for (const auto& component : take(world::components(session.world().runtime(), session.selection().entity)))
        if (take(session.world().runtime().type_of(component.id())) == type)
            return component.id();
    throw std::runtime_error("Missing component");
}

world::FieldPath field(editor::Session& session, TypeId type, std::string_view name) {
    return {take(session.world().runtime().find_field(type, name))};
}

// A flat Radiance RGBE sky map, 2 * height by height texels of radiance 2^(exponent - 129).
std::string radiance(std::uint32_t height, char exponent) {
    auto bytes = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " + std::to_string(height) + " +X " +
                 std::to_string(2 * height) + "\n";
    for (std::uint32_t i = 0; i < 2 * height * height; ++i)
        bytes += std::string{char(128), char(128), char(128), exponent};
    return bytes;
}

// Writes a sky map into the project's Content and returns its catalog key.
std::string sky_asset(editor::Project& project, const std::filesystem::path& relative, const std::string& bytes) {
    std::filesystem::create_directories((project.root() / relative).parent_path());
    take(editor::write_file(project.root() / relative, bytes));
    take(project.refresh());
    const auto at = std::ranges::find_if(
        project.entries(), [&](const auto& entry) { return entry.path.filename() == relative.filename(); });
    require(at != project.entries().end() && at->kind == assets::AssetKind::sky,
            "A .hdr file was not catalogued as a sky asset.");
    return at->key;
}

void verify_primitive_source(editor::Project& project) {
    acceptance_phase = "Primitive Source reflection and persistence";
    editor::Session document;
    take(document.initialize());
    auto& runtime = document.world().runtime();
    auto program = take(source::Program::create(runtime, document.presentation_types().static_mesh_component));
    auto candidate = take(source::Program::prepare(take(program->request(
        "Primitive.source",
        "void configure() { self.body.enabled = true; self.body.half_extents.x = 2.0f; self.body.material.friction = 0.6f; }"))));
    take(program->try_publish(candidate));
    changed(document.create_static_mesh(take(project.assets().mesh("builtin/cube"))));
    const auto component = member(document, document.presentation_types().static_mesh_component);
    auto lease = take(program->pin());
    take(lease.invoke(component, {}));
    const auto settings = take(physics::read_settings(runtime, component));
    require(settings.enabled && settings.half_extents.x == 2.f && settings.material.friction == .6f,
            "Source did not write the Primitive's shared reflected body configuration.");
    const auto saved = take(document.serialize());
    const auto data = assets::json_format::parse(saved);
    require(bool(data), "Saved Primitive JSON is invalid.");
    const auto components = data->root().at("entities")->at(0)->at("components");
    require(components && components->size() == 1 &&
                components->at(0)->boolean({"properties", "body", "enabled"}) == true &&
                !*components->at(0)->find("body"),
            "Primitive physics acquired a separate component or a second serialization path.");
    auto reopened = take(editor::Session::deserialize(saved, project));
    changed(reopened->start());
    take(reopened->update(1.f / 60.f));
    changed(reopened->stop());
    require(take(reopened->serialize()) == saved, "Jolt startup changed the authored Primitive configuration.");
    changed(document.close());
    changed(reopened->close());
    std::puts("Primitive Source/Scene: PASS (generated reflection, JIT fields, generic properties, Jolt lifecycle)");
}

void verify_query_source(editor::Project& project) {
    acceptance_phase = "Source query loading";
    auto evidence = std::make_shared<Evidence>();
    const editor::Session::Prepare query_code = [&](world::World& world) -> Result<std::shared_ptr<void>> {
        auto& runtime = world.runtime();
        ++evidence->prepared;
        const auto type = take(NativeRegistration<QueryProbe>{runtime, "game.QueryProbe", TypeKind::object}
                                   .base<physics::SceneQueryComponent>()
                                   .field<&QueryProbe::observed>("observed")
                                   .field<&QueryProbe::count>("count")
                                   .field<&QueryProbe::distance>("distance")
                                   .field<&QueryProbe::point>("point")
                                   .publish());
        auto code = std::make_shared<Code>(runtime, type, evidence);
        auto candidate = take(source::Program::prepare(take(code->program->request(
            "QueryProbe.source",
            "void tick(float dt) { self.observed = self.query_ready() && self.has_hit(); self.count = self.hit_count(); self.distance = self.hit_distance(); self.point = self.hit_point(); self.request_query(); }"))));
        take(code->program->try_publish(candidate));
        const auto lease = take(code->program->pin());
        take(world.bind_behavior(type, {.tick = [lease](Runtime&, ObjectId object, float dt) -> Result<void> {
                                     const std::array<Scalar, 1> arguments{dt};
                                     auto called = lease.invoke(object, arguments);
                                     if (!called)
                                         return std::unexpected(called.error());
                                     return {};
                                 }}));
        return std::static_pointer_cast<void>(code);
    };
    const auto parsed = assets::json_format::parse(take(
        assets::read_file(std::filesystem::path{ATELIER_EXAMPLE_PROJECT} / "Content/Physic/Scenes/Raycasts.scene")));
    require(bool(parsed), "Raycast fixture JSON is invalid.");
    json::Builder scene{*parsed};
    unsigned authored_probes{};
    for (auto entity : *scene.root().at("entities")->array()) {
        for (auto component : *entity.at("components")->array())
            if (component.string({"type"}) == "physics.SceneQueryComponent" &&
                component.string({"properties", "settings", "mode"}) == "single") {
                require(scene.set(component, "type", "game.QueryProbe") &&
                            scene.set(*component.at("properties"), "automatic", false),
                        "Cannot construct query fixture.");
                ++authored_probes;
            }
    }
    require(authored_probes == 1,
            "Raycast fixture must contain exactly one single-query capability to become the Source probe.");
    const auto scene_bytes = json::write(scene.root());
    auto document = take(editor::Session::deserialize({scene_bytes.data(), scene_bytes.size()}, project, query_code));
    ObjectId probe{};
    for (const auto& entity : take(document->world().members()))
        for (const auto& component : take(world::components(document->world().runtime(), entity.id())))
            if (take(document->world().runtime().type_of(component.id())) ==
                take(document->world().runtime().native_type<QueryProbe>()))
                probe = component.id();
    require(probe != ObjectId{}, "Source query probe was not loaded.");
    acceptance_phase = "Source query request and result";
    changed(document->start());
    take(document->update(1.f / 60));
    take(document->update(1.f / 60));
    require(take(document->world().runtime().with_native<const QueryProbe>(
                probe,
                [](const auto& value) -> Result<bool> {
                    return value.observed && value.count == 1 && std::abs(value.distance - 6.5f) < .01f &&
                           std::abs(value.point.z - 2.5f) < .01f;
                })),
            "Source could not request a trace and read the completed bool/count/distance/vector result.");
    acceptance_phase = "Source query persistence while active";
    const auto saved = take(document->serialize());
    changed(document->stop());
    changed(document->close());
    document.reset();
    auto reopened = take(editor::Session::deserialize(saved, project, query_code));
    for (const auto& entity : take(reopened->world().members()))
        for (const auto& component : take(world::components(reopened->world().runtime(), entity.id()))) {
            auto ready = reopened->world().runtime().with_native<const physics::SceneQueryComponent>(
                component.id(), [](const auto& q) -> Result<bool> { return q.query_ready(); });
            if (ready)
                require(!*ready, "Scene persistence restored a transient query result.");
        }
    changed(reopened->close());
    reopened.reset();
    require(evidence->prepared == evidence->destroyed, "Query script lease survived its Session.");
    std::puts(
        "Query Source acceptance: PASS (native inherited methods, fixed-step request, scalar/vector result, transient reset)");
}

void edit(editor::Session& session, ObjectId object, const world::FieldPath& path, Scalar value, std::uint64_t key) {
    take(session.begin_edit(object, path, key));
    take(session.preview_edit(key, value));
    take(session.commit_edit(key));
}

void verify_stop_reentry() {
    acceptance_phase = "Session stop reentry";
    editor::Session* current{};
    ObjectId query;
    bool fail_tick{};
    unsigned ends{};
    unsigned live_ends{};
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
                 const auto stopped = current->stop();
                 require(!stopped && stopped.error().code == ErrorCode::busy, "End reentered Session stop.");
                 // The completed query is real PhysicsScene-owned transient state.
                 // Its destructor clears this state, so this detects premature
                 // release without relying on a freed allocation still being readable.
                 const bool alive = take(runtime.with_native<const physics::SceneQueryComponent>(
                     query, [](const auto& value) -> Result<bool> { return value.query_ready(); }));
                 if (alive)
                     ++live_ends;
                 require(alive, "Rejected stop destroyed the physics service before End returned.");
                 return {};
             }}));
        return std::shared_ptr<void>{};
    }));
    current = &document;
    changed(document.add_entity());
    take(document.set_transform(document.selection().entity, {}));
    changed(document.add_component(document.selection().entity, document.physics_types().scene_query_component));
    query = document.selection().object;
    const auto ready = [&] {
        return take(document.world().runtime().with_native<const physics::SceneQueryComponent>(
            query, [](const auto& value) -> Result<bool> { return value.query_ready(); }));
    };
    changed(document.start());
    take(document.update(1.f / 60));
    require(ready(), "Physics query did not complete before End reentry.");
    fail_tick = true;
    const auto failed = document.update(1.f / 60);
    require(!failed && failed.error().message == "reentry tick failure" && ends == 1 && live_ends == 1 &&
                !document.world().running() && !ready(),
            "Failed Session update lost its error or retained physics participation.");
    changed(document.stop());
    changed(document.stop());
    require(ends == 1, "Repeated stop repeated End after failure.");
    fail_tick = false;
    changed(document.start());
    take(document.update(1.f / 60));
    require(ready(), "Physics service did not restart.");
    changed(document.stop());
    require(ends == 2 && live_ends == 2 && !ready(), "Normal stop did not release physics after End.");
    changed(document.close());
    changed(document.close());
    require(ends == 2, "Close repeated an already completed End.");
    std::puts(
        "Session stop reentry: PASS (failed Tick, live physics during End, rejected nested stop, restart, idempotent cleanup)");
}

void verify_codec_graph(editor::Project& project) {
    acceptance_phase = "final spatial graph persistence";
    {
        editor::Session document;
        take(document.initialize());
        const auto saved = take(document.serialize());
        const auto cause_path = platform::utf16_to_utf8((project.root() / "Source/cause.source").generic_wstring());
        require(bool(cause_path), "Invalid UTF-16 source diagnostic fixture path.");
        const Error original{ErrorCode::assignment_failed, "准备失败\n" + std::string(1024, 'x') + "\n完整原因",
                             SourceLocation{"Source/cause.source", 9123, 137, 8, 19, *cause_path}};
        ObjectRoot partial;
        auto rejected = editor::Session::deserialize(
            saved, project,
            [&](world::World& world) -> Result<std::shared_ptr<void>> {
                ATELIER_TRY_BIND(entity, world::create_entity(world.runtime(), world.types().entity));
                partial = std::move(*entity);
                ATELIER_TRY(world.attach(partial.id()));
                return std::unexpected(original);
            },
            editor::LoadPolicy::strict);
        require(!rejected && rejected.error().code == original.code && rejected.error().message == original.message &&
                    rejected.error().source,
                "Session preparation lost the original error code, complete body or source.");
        const auto& source = *rejected.error().source;
        require(source.path == original.source->path && source.version == 9123 && source.offset == 137 &&
                    source.line == 8 && source.column == 19 && source.disk_path == original.source->disk_path,
                "Session preparation changed the original source identity or position.");
        require(!partial.registered(), "Failed Session preparation left its partial object registered.");
        auto fresh = take(editor::Session::deserialize(saved, project));
        require(take(fresh->serialize()) == saved && take(document.serialize()) == saved,
                "Failed Session preparation changed another document or prevented a fresh load.");
    }
    {
        editor::Session document;
        take(document.initialize());
        auto& runtime = document.world().runtime();
        changed(document.add_entity());
        const auto parent = document.selection().entity;
        take(document.set_transform(parent, {{3, 0, 0}, {}, {1, 1, 1}}));
        changed(document.add_entity());
        const auto child = document.selection().entity;
        take(document.set_transform(child, {{7, 0, 0}, {}, {1, 1, 1}}));
        changed(document.reparent(child, parent, world::ReparentRule::keep_local));
        changed(document.set_space_reference(child, ObjectId{}, world::ReparentRule::keep_local));
        changed(document.set_space_reference(parent, child, world::ReparentRule::keep_local));
        const auto saved = take(document.serialize());
        auto reopened = take(editor::Session::deserialize(saved, project));
        require(take(reopened->serialize()) == saved && take(world::world_matrix(runtime, parent))[12] == 10,
                "A valid final spatial DAG was rejected because its restoration passed through a temporary cycle.");
        const auto parsed = assets::json_format::parse(saved);
        require(bool(parsed), "Spatial DAG JSON is invalid.");
        const auto parent_id = std::string{*parsed->root().at("roots")->at(0)->string()};
        const auto restored = take(reopened->local_object(std::stoull(parent_id)));
        require(take(world::world_matrix(reopened->world().runtime(), restored))[12] == 10,
                "Final spatial origin plans were not published with membership.");
    }
    {
        editor::Session document;
        take(document.initialize());
        auto& runtime = document.world().runtime();
        changed(document.add_entity());
        const auto origin = document.selection().entity;
        take(document.set_transform(origin, {{7, 0, 0}, {}, {1, 1, 1}}));
        changed(document.add_entity());
        const auto follower = document.selection().entity;
        take(document.set_transform(follower, {{2, 0, 0}, {}, {1, 1, 1}}));
        changed(document.set_space_reference(follower, origin, world::ReparentRule::keep_local));
        const auto before = take(world::world_matrix(runtime, follower));
        take(document.select(follower));
        changed(document.duplicate_entities(document.selected_entities()));
        const auto copied = document.selection().entity;
        require(copied != follower &&
                    take(world::space_reference(runtime, copied)) == std::optional<ObjectId>{origin} &&
                    take(world::world_matrix(runtime, copied)) == before,
                "Cloning a spatial follower lost its external origin or local/world pose.");
        changed(document.undo());
        require(take(world::inspect_entity(runtime, copied)).state == world::EntityState::detached &&
                    take(world::space_reference(runtime, copied)) == std::optional<ObjectId>{origin},
                "Clone Undo lost its authored external origin.");
        changed(document.redo());
        require(document.selection().entity == copied && take(world::world_matrix(runtime, copied)) == before,
                "Clone Redo changed identity or failed to restore an outgoing external origin.");
    }
    acceptance_phase = "material graph identity persistence";
    {
        editor::Session document;
        take(document.initialize());
        auto& runtime = document.world().runtime();
        std::array<ObjectId, 3> entities;
        std::array<ObjectId, 3> meshes;
        for (std::size_t i = 0; i < entities.size(); ++i) {
            changed(document.create_static_mesh(take(project.assets().mesh("builtin/cube"))));
            entities[i] = document.selection().entity;
            meshes[i] = member(document, document.presentation_types().static_mesh_component);
        }
        assets::MaterialAsset author;
        std::get<assets::MaterialInstanceAsset>(author.author).entries = {{"roughness", .3f, true}};
        const auto shared = take(document.material_resource(project.assets(), author));
        const auto independent = take(document.material_resource(project.assets(), author));
        for (std::size_t i = 0; i < meshes.size(); ++i) {
            const std::array<presentation::MaterialBinding, 1> bindings{
                {{"slot/0", i == 2 ? independent : shared, true}}};
            take(presentation::set_material_bindings(runtime, meshes[i], bindings));
        }
        const auto saved = take(document.serialize());
        const auto data = assets::json_format::parse(saved);
        require(data && data->root().at("materials")->size() == 2,
                "Scene material definitions did not preserve shared and equal-but-independent identities.");
        auto reopened = take(editor::Session::deserialize(saved, project));
        require(take(reopened->serialize()) == saved, "Material document identities changed after save/reload.");
        const auto bindings_of = [&](editor::Session& session, ObjectId entity) {
            take(session.select(entity));
            return take(presentation::read_material_bindings(
                            session.world().runtime(),
                            member(session, session.presentation_types().static_mesh_component)))
                .front()
                .resource;
        };
        const auto members = take(reopened->world().members());
        const auto a = bindings_of(*reopened, members[0].id());
        const auto b = bindings_of(*reopened, members[1].id());
        const auto c = bindings_of(*reopened, members[2].id());
        require(a.id() == b.id() && a.id() != c.id(),
                "Scene reload split a shared inline material or merged equal independent materials.");
        material_float(*reopened, a.id(), "roughness", .6f, 830);
        require(roughness(*reopened, b.id()) == .6f && roughness(*reopened, c.id()) == .3f,
                "Restored shared material edits did not propagate according to object identity.");
        const auto key = take(project.create_material("Content/Materials/Codec-parent.material"));
        const auto parent = take(document.material_resource(project.assets(), key));
        std::get<assets::MaterialInstanceAsset>(author.author) = {key, {}};
        const auto instance = take(document.material_resource(project.assets(), author));
        for (std::size_t i = 0; i < 2; ++i) {
            const std::array<presentation::MaterialBinding, 1> bindings{{{"slot/0", instance, true}}};
            take(presentation::set_material_bindings(runtime, meshes[i], bindings));
        }
        auto instances = take(editor::Session::deserialize(take(document.serialize()), project));
        const auto instance_members = take(instances->world().members());
        const auto left = bindings_of(*instances, instance_members[0].id());
        const auto right = bindings_of(*instances, instance_members[1].id());
        require(left.id() == right.id() &&
                    take(material::read_instance(instances->world().runtime(), left.id())).parent.id() ==
                        take(instances->material_resource(project.assets(), key)).id(),
                "Shared inline instance identity or its shared GUID parent was lost on reload.");
        const std::array selected{entities[0], entities[1]};
        take(document.select_entities(selected, entities[1]));
        changed(document.duplicate_entities(document.selected_entities()));
        const std::vector<ObjectId> copies(document.selected_entities().begin(), document.selected_entities().end());
        const auto copy_a = bindings_of(document, copies[0]);
        const auto copy_b = bindings_of(document, copies[1]);
        require(copy_a.id() == copy_b.id() && copy_a.id() != instance.id() &&
                    take(material::read_instance(runtime, copy_a.id())).parent.id() == parent.id(),
                "Forest clone lost shared instance aliasing or copied the shared material asset.");

        // A scene parent is an identity edge, not a copied set of resolved values.
        const auto child = take(document.create_scene_material(meshes[0], "slot/0"));
        material_float(document, instance.id(), "roughness", .27f, 834);
        const auto parsed = assets::json_format::parse(take(document.serialize()));
        require(bool(parsed), "Material hierarchy JSON is invalid.");
        json::Builder hierarchy{*parsed};
        const auto materials = *parsed->root().at("materials");
        const auto reversed = *hierarchy.array(hierarchy.root(), "materials");
        for (auto i = materials.size(); i > 0; --i)
            hierarchy.append(reversed, *materials.at(i - 1));
        const auto hierarchy_bytes = json::write(hierarchy.root());
        auto ordered = take(editor::Session::deserialize({hierarchy_bytes.data(), hierarchy_bytes.size()}, project, {},
                                                         editor::LoadPolicy::strict));
        const auto ordered_child = take(ordered->local_object(*document.document_identity(child.id())));
        const auto ordered_parent = take(ordered->local_object(*document.document_identity(instance.id())));
        require(take(material::read_instance(ordered->world().runtime(), ordered_child)).parent.id() ==
                        ordered_parent &&
                    roughness(*ordered, ordered_child) == .27f,
                "Unordered scene MI ancestry did not restore the shared parent and inherited value.");
        material_float(*ordered, ordered_parent, "roughness", .42f, 835);
        require(roughness(*ordered, ordered_child) == .42f,
                "A restored scene child stopped following its parent's new value.");
    }
    acceptance_phase = "failed graph resource rollback";
    {
        std::string bmp(70, '\0');
        bmp[0] = 'B';
        bmp[1] = 'M';
        bmp[2] = 70;
        bmp[10] = 54;
        bmp[14] = 40;
        bmp[18] = 2;
        bmp[22] = 2;
        bmp[26] = 1;
        bmp[28] = 32;
        for (std::size_t i = 54; i < bmp.size(); i += 4) {
            bmp[i + 2] = char(255);
            bmp[i + 3] = char(255);
        }
        const auto path = project.root() / "Codec.bmp";
        take(editor::write_file(path, bmp));
        const auto texture = take(project.import_texture(path));
        assets::MaterialAsset asset;
        std::get<assets::MaterialInstanceAsset>(asset.author).entries = {
            {"base_color_texture", material::TextureBinding{texture}, true}};
        const auto incoming = take(project.create_material("Content/Materials/Codec-incoming.material", asset));
        const auto retained = take(project.create_material("Content/Materials/Codec-retained.material"));
        editor::Session source;
        take(source.initialize());
        changed(source.create_static_mesh(take(project.assets().mesh("builtin/cube"))));
        take(source.assign_material(project, member(source, source.presentation_types().static_mesh_component),
                                    "slot/0", incoming));
        changed(source.add_entity());
        const auto parsed = assets::json_format::parse(take(source.serialize()));
        require(bool(parsed), "Source composition JSON is invalid.");
        json::Builder invalid{*parsed};
        require(bool(invalid.set(*invalid.root().at("entities")->at(1)->at("properties"), "unknown_business_field",
                                 json::Document::literal<R"({"$ref":"999999"})">().root())),
                "Cannot damage composition fixture.");
        editor::Session target;
        take(target.initialize());
        changed(target.add_entity());
        const auto original = target.selection().entity;
        const auto resource = take(target.material_resource(project.assets(), retained));
        material_float(target, resource.id(), "roughness", .2f, 832);
        const auto before = take(target.serialize());
        target.mark_saved();
        const auto history = target.undo_count();
        const auto count = take(target.material_assets()).size();
        for (unsigned i = 0; i < 8; ++i)
            take(target.collect_step());
        const auto objects = target.world().runtime().live_objects();
        const auto invalid_bytes = json::write(invalid.root());
        require(!target.instantiate_subtree({invalid_bytes.data(), invalid_bytes.size()}, project),
                "Invalid late business data unexpectedly published a graph.");
        json::Builder cyclic{*parsed};
        const auto first = *cyclic.root().at("entities")->at(0);
        const auto second = *cyclic.root().at("entities")->at(1);
        require(
            cyclic.set(*first.at("space"), "origin", *second.at("id")) &&
                cyclic.set(
                    second, "space",
                    json::Document::literal<R"({"position":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]})">().root()) &&
                cyclic.set(*second.at("space"), "origin", *first.at("id")),
            "Cannot construct cyclic fixture.");
        const auto cyclic_bytes = json::write(cyclic.root());
        require(!target.instantiate_subtree({cyclic_bytes.data(), cyclic_bytes.size()}, project),
                "A candidate spatial cycle was partially published after resource preparation.");
        for (unsigned i = 0; i < 8; ++i)
            take(target.collect_step());
        require(
            take(target.serialize()) == before && target.selection().entity == original &&
                target.undo_count() == history && !target.dirty() && take(target.material_assets()).size() == count &&
                target.loaded_material(incoming) == ObjectId{} && target.loaded_material(retained) == resource.id() &&
                target.material_dirty(retained) && target.world().runtime().live_objects() == objects,
            "Failed composition import changed document IDs/history/selection, leaked new material/texture roots, or discarded a dirty existing resource.");
        changed(target.instantiate_subtree(take(source.serialize()), project));
        const auto created = target.selected_entities().size();
        require(created == 2 && target.loaded_material(incoming) != ObjectId{},
                "Composition import could not retry after resource rollback.");
        changed(target.undo());
        changed(target.redo());
        require(target.selected_entities().size() == created,
                "Imported forest history did not restore the entire graph.");
    }
}

// Every value in expected exists in actual at the same path; actual may add defaults.
bool subset(json::Value actual, json::Value expected) {
    if (expected.kind() == json::Kind::object) {
        if (actual.kind() != json::Kind::object)
            return false;
        for (const auto& [key, value] : *expected.object_items()) {
            const auto field = actual.at(key);
            if (!field || !subset(*field, value))
                return false;
        }
        return true;
    }
    if (expected.kind() == json::Kind::array) {
        if (actual.kind() != json::Kind::array || actual.size() != expected.size())
            return false;
        for (std::size_t i = 0; i < expected.size(); ++i)
            if (!subset(*actual.at(i), *expected.at(i)))
                return false;
        return true;
    }
    return actual == expected;
}

std::size_t count(std::span<const world::Diagnostic> issues, world::DiagnosticKind kind) {
    return static_cast<std::size_t>(std::ranges::count(issues, kind, &world::Diagnostic::kind));
}

std::optional<json::Value> find_entity(json::Value scene, const std::string& id) {
    for (const auto entity : *scene.at("entities")->array())
        if (entity.string({"id"}) == id)
            return entity;
    return {};
}

void verify_retained_data(editor::Project& project, const json::Document& baseline,
                          const std::shared_ptr<Evidence>& evidence) {
    acceptance_phase = "retained scene data";
    using enum world::DiagnosticKind;
    json::Builder scene{baseline};
    const auto root = scene.root();
    auto next = std::stoull(std::string{*root.string({"next_id"})});
    const auto fresh = [&] { return std::to_string(next++); };
    const auto entity = *root.at("entities")->at(0);
    const auto move = *entity.at("components")->at(1);
    const std::string entity_id{*entity.string({"id"})};
    const std::string move_id{*move.string({"id"})};
    const auto properties = *move.at("properties");
    require(scene.set(properties, "legacy_speed", 9.5) && scene.set(properties, "count", std::int64_t{-1}) &&
                scene.set(properties, "allowed", "yes") && scene.set(*entity.at("properties"), "health", 75.0) &&
                scene.set(entity, "tags", json::Document::literal<R"(["a","b"])">().root()) &&
                scene.set(*entity.at("space"), "pivot", json::Document::literal<"[0,0,0]">().root()),
            "Cannot construct retained fields.");
    const auto missing_id = fresh();
    const auto mesh_id = fresh();
    const auto constraint_id = fresh();
    const auto gone_id = fresh();
    json::Builder missing{json::Document::literal<
        R"({"type":"game.Missing","name":"","enabled":true,"properties":{"power":3,"owner_ref":{}}})">()};
    require(missing.set(missing.root(), "id", missing_id) &&
                missing.set(*missing.root().path({"properties", "owner_ref"}), "$ref", entity_id),
            "Cannot construct missing component.");
    scene.append(*entity.at("components"), missing.root());
    json::Builder mesh{json::Document::literal<
        R"({"type":"game.MissingMesh","name":"","enabled":true,"properties":{"glow":1.5},"mesh":"builtin/cube","materials":[]})">()};
    require(bool(mesh.set(mesh.root(), "id", mesh_id)), "Cannot construct missing mesh.");
    scene.append(*entity.at("components"), mesh.root());
    json::Builder constraint_data{json::Document::literal<
        R"({"type":"physics.DistanceConstraintComponent","name":"","enabled":true,"properties":{"target":{}}})">()};
    require(constraint_data.set(constraint_data.root(), "id", constraint_id) &&
                constraint_data.set(*constraint_data.root().path({"properties", "target"}), "$ref", move_id),
            "Cannot construct constraint.");
    scene.append(*entity.at("components"), constraint_data.root());
    json::Builder gone{json::Document::literal<
        R"({"type":"game.Gone","name":"Gone","enabled":true,"space":null,"children":[],"components":[],"properties":{"peer":{}}})">()};
    require(gone.set(gone.root(), "id", gone_id) &&
                gone.set(*gone.root().path({"properties", "peer"}), "$ref", entity_id),
            "Cannot construct missing entity.");
    scene.append(*root.at("entities"), gone.root());
    require(scene.append(*root.at("roots"), gone_id) && scene.set(root, "next_id", std::to_string(next)) &&
                scene.set(root, "editor_meta", json::Document::literal<R"({"layout":1})">().root()) &&
                scene.set(root, "world", json::Document::literal<R"({"future_option":3})">().root()),
            "Cannot construct retained scene.");
    const auto bytes = json::write(root);
    const std::string_view text{bytes.data(), bytes.size()};

    auto document = take(editor::Session::deserialize(text, project, prepare(evidence)));
    auto& runtime = document->world().runtime();
    require(!document->dirty(), "Tolerant load marked the document dirty.");
    const auto move_type = take(runtime.find_type("game.Move"));
    const auto entity_type = take(runtime.find_type("game.Entity"));
    const auto entity_object = take(document->local_object(std::stoull(entity_id)));
    const auto move_object = take(document->local_object(std::stoull(move_id)));
    const auto mesh_object = take(document->local_object(std::stoull(mesh_id)));
    const auto constraint = take(document->local_object(std::stoull(constraint_id)));
    require(std::get<std::uint32_t>(take(runtime.read(move_object, field(*document, move_type, "count")))) == 7 &&
                std::get<bool>(take(runtime.read(move_object, field(*document, move_type, "allowed")))) &&
                std::get<float>(take(runtime.read(move_object, field(*document, move_type, "speed")))) == 4 &&
                std::get<std::int32_t>(take(runtime.read(entity_object, field(*document, entity_type, "health")))) ==
                    75,
            "Mismatched values did not fall back to defaults, or applicable values were lost.");
    require(document->placeholder_count() == 3 &&
                take(presentation::read_static_mesh(runtime, mesh_object)).mesh.source == "builtin/cube" &&
                take(physics::constraint_target(runtime, constraint)) == ObjectId{},
            "Placeholders did not keep their native base, or a mistyped reference was applied.");
    const auto view = take(document->retained(move_object));
    require(view.type.empty() && view.entries.size() == 3 &&
                std::ranges::count(view.entries, unknown_field, &editor::RetainedEntry::kind) == 1 &&
                std::ranges::count(view.entries, mismatch, &editor::RetainedEntry::kind) == 2,
            "Retained view misclassified saved data.");

    json::Builder expected{root};
    require(bool(expected.set(*expected.root().at("entities")->at(0)->at("properties"), "health", 75u)),
            "Cannot construct expected health.");
    const auto first_text = take(document->serialize());
    const auto first = assets::json_format::parse(first_text);
    require(first && subset(first->root(), expected.root()), "Saving dropped or altered retained scene data.");
    auto reopened = take(editor::Session::deserialize(first_text, project, prepare(evidence)));
    const auto reopened_json = assets::json_format::parse(take(reopened->serialize()));
    require(reopened->load_issues().size() == 15 && reopened_json && *reopened_json == *first,
            "Retained data did not round-trip stably.");

    acceptance_phase = "retained data released by edits";
    take(document->select(entity_object));
    edit(*document, move_object, field(*document, move_type, "count"), Scalar{std::uint32_t{9}}, 901);
    const auto count_of = [&] {
        const auto parsed = assets::json_format::parse(take(document->serialize()));
        require(bool(parsed), "Edited scene JSON is invalid.");
        return parsed->root().at("entities")->at(0)->at("components")->at(1)->i64({"properties", "count"});
    };
    require(count_of() == 9, "An edit did not replace the retained value.");
    changed(document->undo());
    require(count_of() == -1, "Undo did not restore the retained value.");
    changed(document->redo());
    require(count_of() == 9, "Redo did not release the retained value again.");
    take(document->discard_retained(move_object));
    auto discarded = assets::json_format::parse(take(document->serialize()));
    require(bool(discarded), "Discarded scene JSON is invalid.");
    auto discarded_properties = *discarded->root().at("entities")->at(0)->at("components")->at(1)->at("properties");
    require(!*discarded_properties.find("legacy_speed") && discarded_properties.boolean({"allowed"}) == true &&
                discarded_properties.i64({"count"}) == 9,
            "Discard did not drop the object's retained data.");
    changed(document->undo());
    discarded = assets::json_format::parse(take(document->serialize()));
    require(bool(discarded), "Undo scene JSON is invalid.");
    discarded_properties = *discarded->root().at("entities")->at(0)->at("components")->at(1)->at("properties");
    require(discarded_properties.number({"legacy_speed"}) == 9.5 && discarded_properties.string({"allowed"}) == "yes",
            "Undo did not restore discarded data.");

    acceptance_phase = "retained data in duplicates";
    {
        auto copy = take(editor::Session::deserialize(text, project, prepare(evidence)));
        const auto source = take(copy->local_object(std::stoull(entity_id)));
        take(copy->select(source));
        changed(copy->duplicate_entities(copy->selected_entities()));
        const auto duplicate = copy->selected_entities().back();
        const auto duplicate_id = std::to_string(*copy->document_identity(duplicate));
        const auto saved = assets::json_format::parse(take(copy->serialize()));
        require(bool(saved), "Duplicate scene JSON is invalid.");
        const auto record = find_entity(saved->root(), duplicate_id);
        require(record && record->at("components")->size() == 5 &&
                    record->at("components")->at(2)->string({"type"}) == "game.Missing" &&
                    record->at("components")->at(2)->string({"properties", "owner_ref", "$ref"}) == duplicate_id &&
                    record->at("components")->at(3)->string({"type"}) == "game.MissingMesh" &&
                    *record->at("tags") == json::Document::literal<R"(["a","b"])">().root(),
                "Duplicate lost retained data or did not remap a reference inside the copy.");
        require(copy->load_issues().size() == 16, "Duplicating reported its retained data as a new load.");
    }

    acceptance_phase = "retained references to deleted objects";
    {
        auto deleted = take(editor::Session::deserialize(text, project, prepare(evidence)));
        const auto target = take(deleted->local_object(std::stoull(entity_id)));
        const std::array targets{target};
        take(deleted->select_entities(targets, target));
        changed(deleted->erase_entities(deleted->selected_entities()));
        const auto saved = take(deleted->serialize());
        const auto parsed = assets::json_format::parse(saved);
        require(bool(parsed), "Deleted scene JSON is invalid.");
        require(find_entity(parsed->root(), gone_id)->path({"properties", "peer"})->is_null() &&
                    count(deleted->issues(), reference_dropped) == 1,
                "A retained reference to a deleted object was not written as null and reported.");
        require(editor::Session::deserialize(saved, project, prepare(evidence)).has_value(),
                "Saving after a deletion produced an unloadable scene.");
        changed(deleted->undo());
        const auto undone = assets::json_format::parse(take(deleted->serialize()));
        require(bool(undone), "Undo deletion JSON is invalid.");
        require(find_entity(undone->root(), gone_id)->string({"properties", "peer", "$ref"}) == entity_id,
                "Undoing the deletion did not restore the retained reference.");
    }
}

// String leaves use the scene codec's shared leaf decoder, which also decodes operator parameters strictly.
void verify_string_properties(editor::Project& project, const std::shared_ptr<Evidence>& evidence) {
    acceptance_phase = "String properties";
    using enum world::DiagnosticKind;
    editor::Session document;
    take(document.initialize(prepare(evidence)));
    auto& runtime = document.world().runtime();
    const auto label_type = take(runtime.find_type("game.Label"));
    const auto caption_type = take(runtime.find_type("game.Caption"));
    const auto string_type = take(runtime.native_type<String>());
    changed(document.add_entity());
    const auto entity = document.selection().entity;
    changed(document.add_component(entity, label_type));
    const auto label = member(document, label_type);
    const auto caption = take(runtime.find_field(label_type, "caption"));
    const world::FieldPath title{take(runtime.find_field(label_type, "title"))};
    const world::FieldPath text{caption, take(runtime.find_field(caption_type, "text"))};
    const world::FieldPath size{caption, take(runtime.find_field(caption_type, "size"))};
    const world::FieldPath lines{caption, take(runtime.find_field(caption_type, "lines"))};
    const auto set = [&](ObjectId object, const world::FieldPath& path, std::string_view value) {
        String stored;
        require(stored.assign(StringView{value}).has_value(), "Expected valid fixture UTF-8.");
        take(runtime.write(object, path, ValueInput{string_type, &stored}));
    };
    // Each session owns a Runtime with its own field identities, so paths resolve by name per session.
    const auto path = [](editor::Session& session, std::string_view leaf) -> world::FieldPath {
        auto& owner = session.world().runtime();
        const auto type = take(owner.find_type("game.Label"));
        if (leaf == "title")
            return {take(owner.find_field(type, leaf))};
        return {take(owner.find_field(type, "caption")),
                take(owner.find_field(take(owner.find_type("game.Caption")), leaf))};
    };
    const auto get = [&](editor::Session& session, ObjectId object, std::string_view leaf) {
        auto& owner = session.world().runtime();
        String value;
        take(owner.read(object, path(session, leaf), ValueOutput{take(owner.native_type<String>()), &value}));
        return std::string{value.c_str(), value.size()};
    };
    const auto label_json = [](json::Value scene) {
        for (auto part : *scene.at("entities")->at(0)->at("components")->array())
            if (part.string({"type"}) == "game.Label")
                return part;
        throw std::runtime_error("The saved scene lost its Label component.");
    };
    const std::string gate = "Gate \"north\" \xE5\x8C\x97\xE9\x97\xA8";
    set(label, title, gate);
    set(label, text, "line\nbreak");
    const auto saved_text = take(document.serialize());
    const auto saved = assets::json_format::parse(saved_text);
    require(bool(saved), "Label scene JSON is invalid.");
    const auto label_id = std::stoull(std::string{*label_json(saved->root()).string({"id"})});
    require(subset(*label_json(saved->root()).at("properties"),
                   json::Document::literal<
                       R"({"title":"Gate \"north\" 北门","caption":{"text":"line\nbreak","size":1.0,"lines":1}})">()
                       .root()),
            "String fields were not saved as JSON strings beside their record siblings.");
    {
        auto reopened = take(editor::Session::deserialize(saved_text, project, prepare(evidence)));
        const auto copy = take(reopened->local_object(label_id));
        require(reopened->load_issues().empty() && get(*reopened, copy, "title") == gate &&
                    get(*reopened, copy, "text") == "line\nbreak",
                "String fields did not round-trip through a scene load.");
    }
    {
        json::Builder damaged{*saved};
        const auto properties = *label_json(damaged.root()).at("properties");
        require(damaged.set(properties, "title", 5u) && damaged.set(*properties.at("caption"), "size", 50u) &&
                    damaged.set(*properties.at("caption"), "lines", 2.0),
                "Cannot damage Label fixture.");
        const auto bytes = json::write(damaged.root());
        auto tolerant = take(editor::Session::deserialize({bytes.data(), bytes.size()}, project, prepare(evidence)));
        const auto copy = take(tolerant->local_object(label_id));
        const auto issues = tolerant->load_issues();
        auto& owner = tolerant->world().runtime();
        require(issues.size() == 3 && count(issues, mismatch) == 2 && count(issues, converted) == 1 &&
                    get(*tolerant, copy, "title").empty() &&
                    std::get<float>(take(owner.read(copy, path(*tolerant, "size")))) == 10 &&
                    std::get<std::uint32_t>(take(owner.read(copy, path(*tolerant, "lines")))) == 2,
                "A mistyped String did not keep its default, or its record siblings were not decoded.");
        const auto resaved_scene = assets::json_format::parse(take(tolerant->serialize()));
        require(bool(resaved_scene), "Resaved Label JSON is invalid.");
        const auto resaved = *label_json(resaved_scene->root()).at("properties");
        require(resaved.i64({"title"}) == 5 && resaved.i64({"caption", "size"}) == 50 &&
                    resaved.string({"caption", "text"}) == "line\nbreak",
                "The saved values of mismatched leaves were not written back.");
    }

    acceptance_phase = "strict properties decoding";
    alignas(Caption) std::array<std::byte, sizeof(Caption)> storage{};
    take(runtime.initialize_value(caption_type, storage));
    const editor::PropertyTarget record{caption_type, {}, storage};
    const auto value_of = [&](const world::FieldPath& path) {
        return take(runtime.read(caption_type, storage, std::span(path).subspan(1)));
    };
    take(editor::decode_properties(runtime, record,
                                   json::Document::literal<R"({"text":"hi","size":3,"lines":2.0})">().root()));
    String decoded;
    take(runtime.read(caption_type, storage, std::span(text).subspan(1), ValueOutput{string_type, &decoded}));
    require(std::string{decoded.c_str(), decoded.size()} == "hi" && value_of(size) == Scalar{3.f} &&
                value_of(lines) == Scalar{std::uint32_t{2}},
            "Strict decoding did not write a caller-owned record, or rejected an exact integral conversion.");
    require(!editor::decode_properties(runtime, record, json::Document::literal<R"({"size":50})">().root()) &&
                value_of(size) == Scalar{3.f},
            "A rejected strict decode clamped and wrote its value.");
    const auto invalid_text = assets::json_format::parse("{\"text\":\"\xc0\xaf\"}");
    take(runtime.read(caption_type, storage, std::span(text).subspan(1), ValueOutput{string_type, &decoded}));
    require(!invalid_text && invalid_text.error().fault == json::Fault::encoding && invalid_text.error().offset == 9 &&
                decoded.view() == "hi" && value_of(size) == Scalar{3.f} && value_of(lines) == Scalar{std::uint32_t{2}},
            "Invalid UTF-8 reached property decoding or changed the record instead of failing at its byte offset.");
    take(runtime.destroy_value(caption_type, storage));

    // A write hook turns an object decode into a patch without touching the object.
    std::vector<std::pair<world::FieldPath, editor::PropertyLeaf>> patch;
    take(editor::decode_properties(
        runtime, {label_type, label}, json::Document::literal<R"({"title":"patched","caption":{"size":4}})">().root(),
        {}, [&](std::span<const FieldId> path, const editor::PropertyLeaf& leaf) -> Result<void> {
            patch.emplace_back(world::FieldPath(path.begin(), path.end()), leaf);
            return {};
        }));
    const auto leaf = [&](const world::FieldPath& path) -> const editor::PropertyLeaf* {
        const auto at = std::ranges::find(patch, path, &std::pair<world::FieldPath, editor::PropertyLeaf>::first);
        return at == patch.end() ? nullptr : &at->second;
    };
    require(patch.size() == 2 && leaf(title) && std::get<String>(*leaf(title)) == String::literal<"patched">() &&
                leaf(size) && std::get<Scalar>(*leaf(size)) == Scalar{4.f} && get(document, label, "title") == gate,
            "A collecting write hook did not receive the decoded String and scalar leaves, or the object changed.");

    acceptance_phase = "String properties in duplicates";
    take(document.select(entity));
    changed(document.duplicate_entities(document.selected_entities()));
    const auto duplicate = document.selection().entity;
    ObjectId copied{};
    for (const auto& part : take(world::components(runtime, duplicate)))
        if (take(runtime.type_of(part.id())) == label_type)
            copied = part.id();
    require(duplicate != entity && copied != ObjectId{} && get(document, copied, "title") == gate &&
                get(document, copied, "text") == "line\nbreak",
            "A duplicate did not copy its String fields.");
}

// Inline Arrays save as JSON arrays and decode into a value of their own, stored as one whole leaf.
void verify_array_properties(editor::Project& project, const std::shared_ptr<Evidence>& evidence) {
    acceptance_phase = "Array properties";
    using enum world::DiagnosticKind;
    // Each session owns a Runtime with its own identities, so types and fields resolve by name per session.
    struct Fields {
        TypeId tags;

        TypeId strings;

        TypeId floats;

        world::FieldPath names;

        world::FieldPath weights;
    };
    const auto fields = [](const Runtime& runtime) {
        const auto tags = take(runtime.find_type("game.Tags"));
        return Fields{tags,
                      take(runtime.find_type("game.Strings")),
                      take(runtime.find_type("game.Floats")),
                      {take(runtime.find_field(tags, "names"))},
                      {take(runtime.find_field(tags, "weights"))}};
    };
    const auto names_of = [&](const Runtime& runtime, ObjectId object) {
        const auto f = fields(runtime);
        Array<String> value;
        take(runtime.read(object, f.names, ValueOutput{f.strings, &value}));
        std::vector<std::string> result;
        for (const auto& text : value)
            result.emplace_back(text.c_str(), text.size());
        return result;
    };
    const auto weights_of = [&](const Runtime& runtime, ObjectId object) {
        const auto f = fields(runtime);
        Array<float> value;
        take(runtime.read(object, f.weights, ValueOutput{f.floats, &value}));
        return std::vector<float>(value.begin(), value.end());
    };
    const auto counts_of = [](const Runtime& runtime, ObjectId object) {
        const auto tags = take(runtime.find_type("game.Tags"));
        const world::FieldPath counts{take(runtime.find_field(tags, "counts"))};
        Array<std::uint32_t> value;
        take(runtime.read(object, counts, ValueOutput{take(runtime.find_type("game.Counts")), &value}));
        return std::vector<std::uint32_t>(value.begin(), value.end());
    };
    const auto tags_json = [](json::Value scene) {
        for (auto part : *scene.at("entities")->at(0)->at("components")->array())
            if (part.string({"type"}) == "game.Tags")
                return part;
        throw std::runtime_error("The saved scene lost its Tags component.");
    };
    editor::Session document;
    take(document.initialize(prepare_tags(evidence)));
    auto& runtime = document.world().runtime();
    const auto f = fields(runtime);
    changed(document.add_entity());
    changed(document.add_component(document.selection().entity, f.tags));
    const auto tags = member(document, f.tags);
    const std::vector<std::string> names{"north", "\xE5\x8C\x97"};
    const std::vector<float> weights{.25f, 1.f, .5f};
    {
        Array<String> stored;
        for (const auto& name : names)
            require(stored.emplace().assign(StringView{name}).has_value(), "Expected valid fixture UTF-8.");
        const Array<float> bounded{.25f, 1.f, .5f};
        take(runtime.write(tags, f.names, ValueInput{f.strings, &stored}));
        take(runtime.write(tags, f.weights, ValueInput{f.floats, &bounded}));
    }
    const auto saved_text = take(document.serialize());
    const auto saved = assets::json_format::parse(saved_text);
    require(bool(saved), "Tags scene JSON is invalid.");
    const auto tags_id = std::stoull(std::string{*tags_json(saved->root()).string({"id"})});
    require(subset(*tags_json(saved->root()).at("properties"),
                   json::Document::literal<R"({"names":["north","北"],"weights":[0.25,1.0,0.5]})">().root()) &&
                tags_json(saved->root()).path({"properties", "weights"})->at(1)->kind() == json::Kind::real,
            "Inline Arrays were not saved as JSON arrays of their elements.");
    {
        auto reopened = take(editor::Session::deserialize(saved_text, project, prepare_tags(evidence)));
        const auto copy = take(reopened->local_object(tags_id));
        auto& owner = reopened->world().runtime();
        require(reopened->load_issues().empty() && names_of(owner, copy) == names && weights_of(owner, copy) == weights,
                "Inline Arrays did not round-trip through a scene load.");
    }
    {
        json::Builder damaged{*saved};
        const auto properties = *tags_json(damaged.root()).at("properties");
        require(damaged.set(properties, "names", json::Document::literal<R"(["kept",5])">().root()) &&
                    damaged.set(properties, "weights", json::Document::literal<"[0.5,2.0,-1.0]">().root()) &&
                    damaged.set(properties, "counts", json::Document::literal<"[2.0,5.0]">().root()),
                "Cannot damage Tags fixture.");
        const auto bytes = json::write(damaged.root());
        auto tolerant =
            take(editor::Session::deserialize({bytes.data(), bytes.size()}, project, prepare_tags(evidence)));
        const auto copy = take(tolerant->local_object(tags_id));
        const auto issues = tolerant->load_issues();
        auto& owner = tolerant->world().runtime();
        // A clamped Array is reported once, as a mismatch, even when its elements also converted.
        require(
            issues.size() == 3 && count(issues, mismatch) == 3 && names_of(owner, copy).empty() &&
                weights_of(owner, copy) == std::vector<float>{.5f, 1.f, 0.f} &&
                counts_of(owner, copy) == std::vector<std::uint32_t>{2, 3},
            "A mistyped element did not leave its Array at the default, or elements outside the hard bounds were not clamped once.");
        const auto resaved = assets::json_format::parse(take(tolerant->serialize()));
        require(resaved && resaved->root() == damaged.root(),
                "The saved values of mismatched Arrays were not written back unchanged.");
    }

    acceptance_phase = "Array properties decoding";
    const auto unchanged = [&] { return names_of(runtime, tags) == names && weights_of(runtime, tags) == weights; };
    require(!editor::decode_properties(runtime, {f.tags, tags},
                                       json::Document::literal<R"({"weights":[0.5,2.0]})">().root()) &&
                unchanged(),
            "Strict decoding partly wrote an invalid Array.");
    const auto invalid_array = assets::json_format::parse("{\"names\":[\"valid prefix\",\"\xed\xa0\x80\"]}");
    require(!invalid_array && invalid_array.error().fault == json::Fault::encoding &&
                invalid_array.error().offset == 26 && unchanged(),
            "Invalid UTF-8 reached Array decoding or changed its valid prefix instead of failing at its byte offset.");
    // A write hook receives each Array as one whole leaf and leaves the object as it was.
    std::vector<std::pair<world::FieldPath, editor::PropertyLeaf>> patch;
    take(editor::decode_properties(
        runtime, {f.tags, tags}, json::Document::literal<R"({"names":["patched"],"weights":[0.75]})">().root(), {},
        [&](std::span<const FieldId> path, const editor::PropertyLeaf& leaf) -> Result<void> {
            patch.emplace_back(world::FieldPath(path.begin(), path.end()), leaf);
            return {};
        }));
    const auto leaf = [&](const world::FieldPath& path) -> const editor::ArrayLeaf* {
        const auto at = std::ranges::find(patch, path, &std::pair<world::FieldPath, editor::PropertyLeaf>::first);
        return at == patch.end() ? nullptr : std::get_if<editor::ArrayLeaf>(&at->second);
    };
    const auto* patched_names = leaf(f.names);
    const auto* patched_weights = leaf(f.weights);
    require(patch.size() == 2 && patched_names && patched_weights && patched_names->type() == f.strings &&
                patched_weights->type() == f.floats &&
                std::ranges::equal(*reinterpret_cast<const Array<String>*>(patched_names->storage().data()),
                                   std::array{String::literal<"patched">()}) &&
                std::ranges::equal(*reinterpret_cast<const Array<float>*>(patched_weights->storage().data()),
                                   std::array{.75f}) &&
                unchanged(),
            "A collecting write hook did not receive whole Array leaves, or the object changed.");
    // A tolerant decode reports from the record, applies the clamped Array and reports an exact conversion.
    std::vector<std::vector<std::string>> kept;
    std::vector<std::vector<std::string>> conversions;
    const editor::PropertyReport report{
        [&](std::vector<std::string> at, json::Value, world::DiagnosticKind kind, const std::string&) -> Result<void> {
            if (kind == mismatch)
                kept.push_back(std::move(at));
            return {};
        },
        [&](std::vector<std::string> at, json::Value) -> Result<void> {
            conversions.push_back(std::move(at));
            return {};
        }};
    take(editor::decode_properties(
        runtime, {f.tags, tags}, take(editor::detail::author_fields(runtime, f.tags)),
        json::Document::literal<R"({"names":["a",3],"weights":[0.75,2.0],"counts":[1.0]})">().root(), report));
    require(
        kept == std::vector<std::vector<std::string>>{{"names"}, {"weights"}} &&
            conversions == std::vector<std::vector<std::string>>{{"counts"}} && names_of(runtime, tags) == names &&
            weights_of(runtime, tags) == std::vector<float>{.75f, 1.f} &&
            counts_of(runtime, tags) == std::vector<std::uint32_t>{1},
        "A tolerant decode did not report its leaves from the record, or did not apply the clamped and converted Arrays.");

    const Error report_rejected{ErrorCode::busy, "Property report refused.",
                                SourceLocation{"report", 7, 3, 2, 4, "report.src"}};
    const editor::PropertyReport rejecting{
        [&](std::vector<std::string>, json::Value, world::DiagnosticKind, const std::string&) -> Result<void> {
            return std::unexpected(report_rejected);
        },
        [&](std::vector<std::string>, json::Value) -> Result<void> { return std::unexpected(report_rejected); }};
    const auto schema = editor::detail::author_fields(runtime, f.tags);
    require(bool(schema), "Cannot inspect report refusal fixture.");
    const auto refused = [&](const Result<void>& result) {
        return !result && result.error().code == report_rejected.code &&
               result.error().message == report_rejected.message && result.error().source &&
               result.error().source->path == "report" && result.error().source->version == 7 &&
               result.error().source->offset == 3 && result.error().source->line == 2 &&
               result.error().source->column == 4 && result.error().source->disk_path == "report.src";
    };
    const auto clamp_refused = editor::decode_properties(
        runtime, {f.tags, tags}, *schema, json::Document::literal<R"({"weights":[0.25,2.0]})">().root(), rejecting);
    require(refused(clamp_refused) && weights_of(runtime, tags) == std::vector<float>{.75f, 1.f},
            "An Array clamp wrote before its report accepted, or lost the report Error.");
    const auto conversion_refused =
        editor::decode_properties(runtime, {f.tags, tags}, *schema,
                                  json::Document::literal<R"({"counts":[2.0],"weights":[0.125]})">().root(), rejecting);
    require(refused(conversion_refused) && counts_of(runtime, tags) == std::vector<std::uint32_t>{2} &&
                weights_of(runtime, tags) == std::vector<float>{.75f, 1.f},
            "A conversion refusal rolled back its written leaf or continued to the next leaf.");

    acceptance_phase = "Array properties in Play";
    auto play_project = take(editor::Project::open(project.root()));
    auto b = take(editor::Session::deserialize(saved_text, play_project, prepare_tags(evidence)));
    b->enter_play();
    auto& rb = b->world().runtime();
    const auto b_tags = take(b->local_object(tags_id));
    const auto apply = [&](const json::Document& properties) {
        json::Builder batch{json::Document::literal<R"({"ops":[{"op":"set"}]})">()};
        const auto op = *batch.root().at("ops")->at(0);
        require(batch.set(op, "id", std::to_string(tags_id)) && batch.set(op, "properties", properties.root()),
                "Cannot construct Tags live patch.");
        const auto bytes = json::write(batch.root());
        return take(b->apply_live({bytes.data(), bytes.size()}, play_project));
    };
    require(apply(json::Document::literal<R"({"weights":[0.5]})">()).empty() &&
                weights_of(rb, b_tags) == std::vector<float>{.5f},
            "A Play set did not shorten an Array to its one element.");
    take(const_cast<world::World&>(b->world())
             .add_admission(fields(rb).tags,
                            [&](const world::World& world,
                                std::span<const world::World::AdmissionCandidate> candidates) -> Result<void> {
                                for (const auto& candidate : candidates)
                                    if (std::ranges::contains(names_of(world.runtime(), candidate.component),
                                                              std::string{"Closed"}))
                                        return std::unexpected(
                                            Error{ErrorCode::invalid_member, "Tags cannot name Closed."});
                                return {};
                            }));
    const auto rejected = apply(json::Document::literal<R"({"names":["open","Closed"]})">());
    require(rejected.size() == 1 && rejected[0].severity == world::Severity::error &&
                rejected[0].kind == command_rejected && names_of(rb, b_tags) == names,
            "A Play Array change the type's rules reject did not keep the whole previous Array.");
    const auto clamped = apply(json::Document::literal<R"({"weights":[0.25,2.0]})">());
    require(clamped.size() == 1 && clamped[0].kind == command_rejected &&
                weights_of(rb, b_tags) == std::vector<float>{.5f},
            "A Play Array change outside the hard bounds was clamped and applied.");
    std::puts(
        "Array properties: PASS (save/reopen, strict and tolerant elements, clamp, write hook, Play shorten, admission and clamp rollback)");
}

// Editor document A authors; its Play copy B follows A's changes while it runs.
void verify_play_changes(editor::Project& project, const std::shared_ptr<Evidence>& evidence) {
    acceptance_phase = "Play change batches";
    using enum world::DiagnosticKind;
    editor::Session a;
    take(a.initialize(prepare(evidence)));
    auto& ra = a.world().runtime();
    const auto entity_type = take(ra.find_type("game.Entity"));
    const auto move_type = take(ra.find_type("game.Move"));
    const auto spatial_type = take(ra.find_type("game.Spatial"));
    const auto added = [&](math::Vector3 position) {
        changed(a.add_entity(entity_type));
        const auto entity = a.selection().entity;
        take(a.set_transform(entity, {position, {}, {1, 1, 1}}));
        return entity;
    };
    const auto mover = added({});
    changed(a.add_component(mover, move_type));
    const auto move = member(a, move_type);
    const auto label_type = take(ra.find_type("game.Label"));
    changed(a.add_component(mover, label_type));
    const auto label = member(a, label_type);
    const auto doomed = added({0, 0, 4});
    const auto falling = added({0, 5, 0});
    const auto body_type = take(ra.find_type("world.CollisionComponent"));
    changed(a.add_component(falling, body_type));
    const auto body = member(a, body_type);
    physics::BodySettings settings;
    settings.shape = physics::ShapeKind::sphere;
    settings.motion = physics::MotionKind::dynamic;
    settings.radius = .5f;
    take(a.configure_body(body, settings));
    auto lighting = take(a.presentation().environment());
    lighting.exposure_ev = 1.5f;
    take(a.set_environment(lighting));
    const auto environment = a.environment();
    const auto saved_text = take(a.serialize());
    const auto saved = assets::json_format::parse(saved_text);
    require(bool(saved), "Play source JSON is invalid.");
    const auto id = [&](ObjectId object) { return std::to_string(*a.document_identity(object)); };
    require(
        find_entity(saved->root(), id(falling))->at("components")->at(0)->string({"properties", "body", "motion"}) ==
            "dynamic",
        "Body motion is not saved by member name.");

    auto play_project = take(editor::Project::open(project.root()));
    auto b = take(editor::Session::deserialize(saved_text, play_project, prepare(evidence)));
    b->enter_play();
    auto& rb = b->world().runtime();
    const auto in_b = [&](ObjectId object) { return take(b->local_object(*a.document_identity(object))); };
    const auto apply = [&](json::Value ops) {
        json::Builder batch;
        require(bool(batch.set(batch.root(), "ops", ops)), "Cannot construct Play batch.");
        const auto bytes = json::write(batch.root());
        return take(b->apply_live({bytes.data(), bytes.size()}, play_project));
    };
    const auto applied = [&](json::Value ops) {
        const auto issues = apply(ops);
        if (!issues.empty())
            throw std::runtime_error(issues.front().message);
    };
    const auto b_move_type = take(rb.find_type("game.Move"));
    const world::FieldPath speed{take(rb.find_field(b_move_type, "speed"))};
    const world::FieldPath count{take(rb.find_field(b_move_type, "count"))};
    const auto position = [&](ObjectId object) { return take(world::read_transform(rb, object)).position; };
    const auto step = [&] { take(b->update(1.f / 60)); };
    changed(b->start());
    for (int frame = 0; frame < 10; ++frame)
        step();

    // A field edit replaces only its own leaf; the game's write to a sibling stays.
    const auto b_move = in_b(move);
    const auto b_mover = in_b(mover);
    take(rb.write(b_move, count, Scalar{std::uint32_t{42}}));
    json::Builder ops{json::Document::literal<
        R"([{"op":"set","properties":{"speed":5.0},"enabled":false},{"op":"set","name":"Runner"}])">()};
    require(ops.set(*ops.root().at(0), "id", id(move)) && ops.set(*ops.root().at(1), "id", id(mover)),
            "Cannot construct field batch.");
    applied(ops.root());
    require(std::get<float>(take(rb.read(b_move, speed))) == 5 &&
                std::get<std::uint32_t>(take(rb.read(b_move, count))) == 42 && b->name(b_mover) == L"Runner",
            "A Play field or name change was lost, or overwrote a runtime value.");
    const auto paused = position(b_mover).x;
    step();
    require(position(b_mover).x == paused, "A disabled component kept ticking.");
    ops = json::Builder{json::Document::literal<R"([{"op":"set","enabled":true}])">()};
    require(bool(ops.set(*ops.root().at(0), "id", id(move))), "Cannot construct enable batch.");
    applied(ops.root());
    step();
    require(std::abs(position(b_mover).x - paused - 5.f / 60) < 1e-5f,
            "A re-enabled component did not tick with its new speed.");
    // String leaves apply as whole values; an admission that rejects one restores the whole previous String.
    const auto b_label_type = take(rb.find_type("game.Label"));
    const auto b_string = take(rb.native_type<String>());
    const world::FieldPath title{take(rb.find_field(b_label_type, "title"))};
    const auto title_of = [&](const world::World& world, ObjectId object) {
        String value;
        take(world.runtime().read(object, title, ValueOutput{b_string, &value}));
        return std::string{value.c_str(), value.size()};
    };
    const auto b_label = in_b(label);
    ops = json::Builder{json::Document::literal<R"([{"op":"set","properties":{"title":"North gate"}}])">()};
    require(bool(ops.set(*ops.root().at(0), "id", id(label))), "Cannot construct title batch.");
    applied(ops.root());
    require(title_of(b->world(), b_label) == "North gate", "A Play String field change was not applied.");
    take(const_cast<world::World&>(b->world())
             .add_admission(b_label_type,
                            [&](const world::World& world,
                                std::span<const world::World::AdmissionCandidate> candidates) -> Result<void> {
                                for (const auto& candidate : candidates)
                                    if (title_of(world, candidate.component) == "Closed")
                                        return std::unexpected(
                                            Error{ErrorCode::invalid_member, "A Label cannot be titled Closed."});
                                return {};
                            }));
    require(bool(ops.set(*ops.root().at(0)->at("properties"), "title", "Closed")), "Cannot change title fixture.");
    const auto closed = apply(ops.root());
    require(closed.size() == 1 && closed[0].severity == world::Severity::error && closed[0].kind == command_rejected &&
                title_of(b->world(), b_label) == "North gate",
            "A rejected Play String change did not restore the whole previous value.");

    // An authored pose replaces the simulated one, and body settings reach physics.
    const auto b_falling = in_b(falling);
    require(position(b_falling).y < 5, "The dynamic body did not fall.");
    ops = json::Builder{json::Document::literal<R"([{"op":"set","space":{"position":[3,10,0]}}])">()};
    require(bool(ops.set(*ops.root().at(0), "id", id(falling))), "Cannot construct pose batch.");
    applied(ops.root());
    step();
    const auto teleported = position(b_falling);
    require(std::abs(teleported.x - 3) < 1e-4f && teleported.y > 9.5f && teleported.y < 10,
            "A moved dynamic body snapped back to its simulated pose.");
    ops = json::Builder{json::Document::literal<R"([{"op":"set","properties":{"body":{"motion":"stationary"}}}])">()};
    require(bool(ops.set(*ops.root().at(0), "id", id(body))), "Cannot construct body batch.");
    applied(ops.root());
    step();
    const auto resting = position(b_falling);
    step();
    step();
    require(position(b_falling) == resting, "A Play body setting did not reach physics.");

    // Scalar leaves of one set are one group: moving a probe volume past its old bounds fails leaf by leaf in either order.
    ops = json::Builder{json::Document::literal<
        R"([{"op":"set","properties":{"volume0":{"minimum":{"x":-20,"y":10},"maximum":{"x":-10,"y":20}}}}])">()};
    require(bool(ops.set(*ops.root().at(0), "id", id(environment))), "Cannot construct volume batch.");
    applied(ops.root());
    const auto moved_volume = take(b->presentation().environment()).probe_volumes[0];
    require(moved_volume.minimum[0] == -20 && moved_volume.maximum[0] == -10 && moved_volume.minimum[1] == 10 &&
                moved_volume.maximum[1] == 20 && take(b->presentation().environment()).exposure_ev == 1.5f,
            "A Play set did not write a moved probe volume as one group.");
    require(bool(ops.set(*ops.root().at(0), "properties",
                         json::Document::literal<R"({"volume0":{"maximum":{"x":-30}}})">().root())),
            "Cannot damage volume batch.");
    require(!apply(ops.root()).empty() && take(b->presentation().environment()).probe_volumes[0].maximum[0] == -10,
            "A Play set that breaks the Environment's rules was applied.");
    const auto key = take(project.create_material("Content/Materials/Live.material"));
    require(b->loaded_material(key) == ObjectId{}, "Play loaded an unreferenced material.");
    assets::MaterialAsset asset;
    std::get<assets::MaterialInstanceAsset>(asset.author).entries = {{"roughness", .25f, true}};
    ops = json::Builder{json::Document::literal<R"([{"op":"resource"}])">()};
    auto material_json = assets::material_format::encode(asset);
    require(material_json && ops.set(*ops.root().at(0), "key", key) &&
                ops.set(*ops.root().at(0), "material", material_json->root()),
            "Cannot construct material batch.");
    applied(ops.root());
    const auto loaded = b->loaded_material(key);
    require(loaded != ObjectId{} && roughness(*b, loaded) == .25f, "A new material asset did not reach Play.");
    std::get<assets::MaterialInstanceAsset>(asset.author).entries = {{"roughness", .75f, true}};
    material_json = assets::material_format::encode(asset);
    require(material_json && ops.set(*ops.root().at(0), "material", material_json->root()),
            "Cannot update material batch.");
    applied(ops.root());
    require(b->loaded_material(key) == loaded && roughness(*b, loaded) == .75f,
            "A material asset edit did not update the loaded material in place.");

    // Added objects Begin once with their editor identity; removal Ends them and frees the identity.
    const auto runner = added({0, 0, 8});
    changed(a.add_component(runner, move_type));
    json::Builder add{json::Document::literal<R"([{"op":"add","parent":null,"index":null}])">()};
    const auto runner_graph = assets::json_format::parse(take(a.serialize_live_subtree(runner)));
    require(runner_graph && add.set(*add.root().at(0), "graph", runner_graph->root()), "Cannot construct add batch.");
    auto begins = evidence->begins;
    applied(add.root());
    require(evidence->begins == begins + 1, "An added Entity did not Begin exactly once.");
    step();
    require(position(in_b(runner)).x > 0, "An added Entity did not tick.");
    const auto ends = evidence->ends;
    ops = json::Builder{json::Document::literal<R"([{"op":"remove"}])">()};
    require(bool(ops.set(*ops.root().at(0), "id", id(runner))), "Cannot construct remove batch.");
    applied(ops.root());
    require(evidence->ends == ends + 1 && !b->local_object(*a.document_identity(runner)),
            "A removed Entity did not End or stayed alive.");
    begins = evidence->begins;
    applied(add.root());
    require(evidence->begins == begins + 1 && b->local_object(*a.document_identity(runner)),
            "Restoring a removed identity failed.");
    const auto members = take(b->world().members()).size();
    const auto rejected = apply(add.root());
    require(rejected.size() == 1 && rejected[0].severity == world::Severity::error &&
                rejected[0].kind == command_rejected && take(b->world().members()).size() == members,
            "An add of an identity Play already has created a second object.");

    take(a.select(mover));
    changed(a.add_component(mover, spatial_type));
    const auto spatial = member(a, spatial_type);
    edit(a, spatial, field(a, spatial_type, "offset"), Scalar{9.f}, 31);
    ops = json::Builder{json::Document::literal<R"([{"op":"attach","index":null}])">()};
    const auto spatial_graph =
        assets::json_format::parse(take(a.serialize_live_components(mover, std::array{spatial})));
    require(spatial_graph && ops.set(*ops.root().at(0), "entity", id(mover)) &&
                ops.set(*ops.root().at(0), "graph", spatial_graph->root()),
            "Cannot construct attach batch.");
    applied(ops.root());
    const auto b_spatial = in_b(spatial);
    require(take(world::inspect_component(rb, b_spatial)).owner == b_mover &&
                std::get<float>(
                    take(rb.read(b_spatial, {take(rb.find_field(take(rb.find_type("game.Spatial")), "offset"))}))) == 9,
            "An attached component lost its owner or values.");
    changed(a.reparent(runner, mover));
    ops = json::Builder{json::Document::literal<R"([{"op":"move","index":null}])">()};
    const auto moved_graph = assets::json_format::parse(take(a.serialize_live_subtree(runner)));
    require(moved_graph && ops.set(*ops.root().at(0), "id", id(runner)) &&
                ops.set(*ops.root().at(0), "parent", id(mover)) &&
                ops.set(*ops.root().at(0), "space", *moved_graph->root().at("entities")->at(0)->at("space")),
            "Cannot construct move batch.");
    applied(ops.root());
    require(take(world::parent_entity(rb, in_b(runner))) == b_mover &&
                take(world::read_transform(rb, in_b(runner))) == take(world::read_transform(ra, runner)),
            "A Play reparent did not keep the editor's parent and local transform.");

    // An index is the final position in the destination list (roots for a null parent); Play clamps one past its own list.
    const auto roots = [&] {
        std::vector<ObjectId> result;
        for (const auto& item : take(b->world().members()))
            if (take(world::parent_entity(rb, item.id())) == ObjectId{})
                result.push_back(item.id());
        return result;
    };
    const auto children_of = [&](ObjectId parent) {
        std::vector<ObjectId> result;
        for (const auto& child : take(world::children(rb, parent)))
            result.push_back(child.id());
        return result;
    };
    const auto placed = [&](std::optional<std::string> parent, std::uint32_t index) {
        const auto entity = added({});
        json::Builder batch{json::Document::literal<R"([{"op":"add","parent":null}])">()};
        const auto graph = assets::json_format::parse(take(a.serialize_live_subtree(entity)));
        const auto op = *batch.root().at(0);
        require(graph && batch.set(op, "index", index) && batch.set(op, "graph", graph->root()) &&
                    (!parent || batch.set(op, "parent", *parent)),
                "Cannot construct indexed add.");
        applied(batch.root());
        return entity;
    };
    const auto b_doomed = in_b(doomed);
    const auto b_runner = in_b(runner);
    const auto b_environment = take(world::inspect_component(rb, in_b(environment))).owner;
    // Member position 1 is the mover's child, so a root ordinal read as a member position fails here.
    const auto second = placed({}, 1);
    const auto last = placed({}, 99);
    const auto first_child = placed(id(mover), 0);
    const auto last_child = placed(id(mover), 99);
    require(roots() == std::vector{b_mover, in_b(second), b_doomed, b_falling, b_environment, in_b(last)} &&
                children_of(b_mover) == std::vector{in_b(first_child), b_runner, in_b(last_child)},
            "An add index is not the final position in its destination list, or did not clamp.");
    ops = json::Builder{json::Document::literal<
        R"([{"op":"move","parent":null,"index":0,"space":null},{"op":"move","parent":null,"index":99,"space":null},{"op":"move","index":99,"space":null}])">()};
    require(ops.set(*ops.root().at(0), "id", id(last)) && ops.set(*ops.root().at(1), "id", id(second)) &&
                ops.set(*ops.root().at(2), "id", id(first_child)) && ops.set(*ops.root().at(2), "parent", id(mover)),
            "Cannot construct indexed moves.");
    applied(ops.root());
    require(roots() == std::vector{in_b(last), b_mover, b_doomed, b_falling, b_environment, in_b(second)} &&
                children_of(b_mover) == std::vector{b_runner, in_b(last_child), in_b(first_child)},
            "A move index is not the final position in its destination list, or did not clamp.");

    // A mesh imported after Play opened its own Project still resolves.
    const auto obj = project.root().parent_path() / "Imported.obj";
    {
        std::ofstream file(obj);
        file << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    }
    changed(a.create_static_mesh(take(project.assets().mesh(take(project.import_mesh(obj))))));
    const auto imported = a.selection().entity;
    physics::BodySettings triangle;
    triangle.shape = physics::ShapeKind::triangle_mesh;
    take(a.configure_body(member(a, take(ra.find_type("world.StaticMeshComponent"))), triangle));
    ops = json::Builder{json::Document::literal<R"([{"op":"add","parent":null,"index":null}])">()};
    const auto imported_graph = assets::json_format::parse(take(a.serialize_live_subtree(imported)));
    require(imported_graph && ops.set(*ops.root().at(0), "graph", imported_graph->root()),
            "Cannot construct imported add.");
    applied(ops.root());
    require(std::ranges::any_of(take(b->presentation().render_items()),
                                [&](const auto& item) { return item.entity == in_b(imported); }),
            "A mesh imported during Play did not resolve in the Play copy.");
    // A rescale outgrows the shape cooked at load; physics cooks the new scale once instead of dropping the body.
    auto& simulation = *b->physics_scene();
    ops = json::Builder{json::Document::literal<
        R"([{"op":"set","space":{"position":[20,0,0],"rotation":[0,0,0,1],"scale":[2,2,2]}}])">()};
    require(bool(ops.set(*ops.root().at(0), "id", id(imported))), "Cannot construct rescale.");
    applied(ops.root());
    step();
    const auto cooks = simulation.last_step().runtime_cooks;
    step();
    const auto hit = take(simulation.raycast({21.5f, .2f, 5}, {21.5f, .2f, -5}));
    require(cooks == 1 && simulation.last_step().runtime_cooks == 0 && !hit.hits.empty() &&
                hit.hits.front().body.entity == in_b(imported),
            "A rescaled triangle body was not rebuilt at its new scale, or was cooked more than once.");

    // The game's own destruction skips that item only.
    changed(world::destroy_entity(rb, in_b(doomed)));
    ops = json::Builder{json::Document::literal<R"([{"op":"set","name":"Gone"},{"op":"set","name":"Still here"}])">()};
    require(ops.set(*ops.root().at(0), "id", id(doomed)) && ops.set(*ops.root().at(1), "id", id(mover)),
            "Cannot construct skipped batch.");
    const auto skipped = apply(ops.root());
    require(skipped.size() == 1 && skipped[0].severity == world::Severity::warning &&
                skipped[0].kind == reference_dropped && b->name(b_mover) == L"Still here",
            "A change to an object the game destroyed was not skipped and reported alone.");
    // A reference whose target the game destroyed is null; the rest of the object arrives.
    const auto constraint_type = take(ra.find_type("physics.DistanceConstraintComponent"));
    const auto tethered = added({0, 0, 12});
    changed(a.add_component(tethered, constraint_type));
    const auto constraint = member(a, constraint_type);
    const auto tether_json = assets::json_format::parse(take(a.serialize_live_subtree(tethered)));
    require(bool(tether_json), "Tether JSON is invalid.");
    json::Builder tether{*tether_json};
    const auto tether_properties = *tether.root().at("entities")->at(0)->at("components")->at(0)->at("properties");
    require(tether.set(*tether.object(tether_properties, "target"), "$ref", id(doomed)) &&
                tether.set(tether_properties, "minimum", 2.f) && tether.set(tether_properties, "maximum", 3.f),
            "Cannot construct dropped reference.");
    ops = json::Builder{json::Document::literal<R"([{"op":"add","parent":null,"index":null}])">()};
    require(bool(ops.set(*ops.root().at(0), "graph", tether.root())), "Cannot construct tether add.");
    const auto dropped = apply(ops.root());
    const auto b_constraint = in_b(constraint);
    const auto b_constraint_type = take(rb.find_type("physics.DistanceConstraintComponent"));
    require(dropped.size() == 1 && dropped[0].severity == world::Severity::warning &&
                dropped[0].kind == reference_dropped &&
                take(rb.read_reference(b_constraint, {take(rb.find_field(b_constraint_type, "target"))})).id() ==
                    ObjectId{} &&
                std::get<float>(take(rb.read(b_constraint, {take(rb.find_field(b_constraint_type, "maximum"))}))) == 3,
            "A reference to an object the game destroyed was not nulled and reported, or stopped the rest of the add.");

    // A Play copy never assigns document identities, so they cannot collide with the editor's.
    changed(b->stop());
    take(b->select(b_mover));
    require(!b->duplicate_entities(b->selected_entities()), "A Play copy duplicated with new document identities.");
    require(!b->instantiate_subtree(take(a.serialize_subtree(mover)), play_project),
            "A Play copy imported with new document identities.");
    std::unordered_set<std::string> ids;
    const auto collect = [&](json::Value scene, auto&& visit) {
        for (const auto entity : *scene.at("entities")->array()) {
            visit(*entity.string({"id"}));
            for (const auto part : *entity.at("components")->array())
                visit(*part.string({"id"}));
        }
    };
    const auto author_json = assets::json_format::parse(take(a.serialize()));
    const auto play_json = assets::json_format::parse(take(b->serialize()));
    require(author_json && play_json, "Identity comparison JSON is invalid.");
    collect(author_json->root(), [&](std::string_view value) { ids.emplace(value); });
    collect(play_json->root(), [&](std::string_view value) {
        if (!ids.contains(std::string{value}))
            throw std::runtime_error("A Play copy wrote an identity the editor never assigned.");
    });
    changed(b->close());
    changed(a.close());

    // A Ragdoll follows its controller, so Play cannot follow an authored move of it.
    const auto ragdoll_scene =
        take(assets::read_file(std::filesystem::path{ATELIER_EXAMPLE_PROJECT} / "Content/Physic/Scenes/Ragdoll.scene"));
    std::string ragdoll;
    const auto ragdoll_json = assets::json_format::parse(ragdoll_scene);
    require(bool(ragdoll_json), "Ragdoll JSON is invalid.");
    for (const auto entity : *ragdoll_json->root().at("entities")->array())
        for (const auto part : *entity.at("components")->array())
            if (part.string({"type"}) == "physics.RagdollComponent")
                ragdoll = *entity.string({"id"});
    auto ragdolls = take(editor::Session::deserialize(ragdoll_scene, play_project));
    ragdolls->enter_play();
    const auto ragdoll_root = take(ragdolls->local_object(std::stoull(ragdoll)));
    changed(ragdolls->start());
    for (int frame = 0; frame < 3; ++frame)
        take(ragdolls->update(1.f / 60));
    const auto posed = take(world::read_transform(ragdolls->world().runtime(), ragdoll_root));
    json::Builder ragdoll_batch{json::Document::literal<R"({"ops":[{"op":"set","space":{"position":[1,2,3]}}]})">()};
    require(bool(ragdoll_batch.set(*ragdoll_batch.root().at("ops")->at(0), "id", ragdoll)),
            "Cannot construct ragdoll move.");
    const auto ragdoll_bytes = json::write(ragdoll_batch.root());
    const auto refused = take(ragdolls->apply_live({ragdoll_bytes.data(), ragdoll_bytes.size()}, play_project));
    require(refused.size() == 1 && refused[0].severity == world::Severity::warning &&
                refused[0].kind == command_rejected &&
                take(world::read_transform(ragdolls->world().runtime(), ragdoll_root)) == posed,
            "An authored move Play cannot follow was applied or reported as divergence.");
    changed(ragdolls->close());
}

// Editor document A forwards its own changes; applying each batch to its Play copy B keeps B equal to A.
void verify_live_forwarding(editor::Project& project, const std::shared_ptr<Evidence>& evidence) {
    acceptance_phase = "Play change forwarding";
    const auto material_key = take(project.create_material("Content/Materials/Forwarded.material"));
    editor::Session a;
    take(a.initialize(prepare(evidence)));
    auto& ra = a.world().runtime();
    const auto entity_type = take(ra.find_type("game.Entity"));
    const auto move_type = take(ra.find_type("game.Move"));
    const auto spatial_type = take(ra.find_type("game.Spatial"));
    const auto added = [&](math::Vector3 position) {
        changed(a.add_entity(entity_type));
        const auto entity = a.selection().entity;
        take(a.set_transform(entity, {position, {}, {1, 1, 1}}));
        return entity;
    };
    const auto first = added({1, 2, 3});
    changed(a.add_component(first, move_type));
    const auto move = member(a, move_type);
    const auto second = added({0, 0, 4});
    changed(a.create_static_mesh(take(project.assets().mesh("builtin/cube")), {0, 1, 0}));
    const auto mesh = member(a, a.presentation_types().static_mesh_component);
    take(a.assign_material(project, mesh, "slot/0", material_key));
    const auto shared = take(presentation::read_material_bindings(ra, mesh)).front().resource.id();
    const auto speed = field(a, move_type, "speed");
    const auto mass = field(a, move_type, "mass");

    auto play_project = take(editor::Project::open(project.root()));
    auto b = take(editor::Session::deserialize(take(a.serialize()), play_project, prepare(evidence)));
    b->enter_play();
    auto& rb = b->world().runtime();
    const auto in_b = [&](ObjectId object) { return take(b->local_object(*a.document_identity(object))); };
    require(take(a.take_live_ops()).empty(), "A document recorded Play changes before forwarding started.");
    a.set_live_forwarding(true);
    const auto scene = [](const editor::Session& session) {
        const auto parsed = assets::json_format::parse(take(session.serialize()));
        require(bool(parsed), "Forwarded scene JSON is invalid.");
        json::Builder value{*parsed};
        value.erase(value.root(), "next_id");
        return value.finish();
    };
    const auto sync = [&](const std::string& step, bool compare = true) {
        json::Builder batch;
        const auto ops = *batch.array(batch.root(), "ops");
        for (const auto& op : take(a.take_live_ops())) {
            const auto parsed = assets::json_format::parse(op.text);
            require(bool(parsed), "Forwarded operation JSON is invalid.");
            batch.append(ops, parsed->root());
        }
        const auto bytes = json::write(batch.root());
        const auto issues = take(b->apply_live({bytes.data(), bytes.size()}, play_project));
        if (!issues.empty())
            throw std::runtime_error(step + ": " + issues.front().message);
        if (compare && scene(a) != scene(*b))
            throw std::runtime_error(step + ": the Play copy differs from the document.");
        return ops.size();
    };
    const auto each = [&](const std::string& step, auto&& change) {
        change();
        if (!sync(step))
            throw std::runtime_error(step + ": the change sent nothing.");
        changed(a.undo());
        sync(step + " undo");
        changed(a.redo());
        sync(step + " redo");
    };
    // The first World Settings edit reaches Play as one add carrying its values; undoing it removes the Environment.
    const rendering::SceneLightingSettings defaults{};
    auto lighting = defaults;
    lighting.exposure_ev = 1.5f;
    lighting.sky_enabled = true;
    take(a.set_environment(lighting));
    sync("World settings creation");
    require(b->environment() != ObjectId{} && take(b->presentation().environment()) == lighting,
            "The first World Settings edit did not reach Play with its values.");
    changed(a.undo());
    sync("World settings creation undo");
    require(b->environment() == ObjectId{} && take(b->presentation().environment()) == defaults,
            "Undoing the first World Settings edit left Play with an Environment.");
    changed(a.redo());
    sync("World settings creation redo");
    require(take(b->presentation().environment()) == lighting,
            "Redoing the first World Settings edit did not restore Play's Environment.");
    lighting.exposure_ev = 2.5f;
    lighting.probe_volumes[0].minimum[0] = 7;
    lighting.probe_volumes[0].maximum[0] = 9;
    each("World settings", [&] { take(a.set_environment(lighting)); });
    require(take(b->presentation().environment()) == lighting, "A later World Settings edit did not reach Play.");
    // A sky map reaches Play as its key, which Play resolves in its own Project, refreshed for a file added after Play opened it.
    const auto sky_key = sky_asset(project, "Content/Sky/Forwarded.hdr", radiance(1, char(130)));
    const auto play_sky = [&] { return take(b->presentation().environment()).sky_map; };
    each("Sky map", [&] { take(a.set_sky_map(take(project.assets().sky_map(sky_key)))); });
    require(play_sky() && play_sky() == take(play_project.assets().sky_map(sky_key)).map,
            "A sky map did not reach Play through its own Project.");
    const auto environment_entity = take(world::inspect_component(ra, a.environment())).owner;
    take(a.select(environment_entity));
    changed(a.erase_entities(a.selected_entities()));
    sync("Environment removal");
    changed(a.undo());
    sync("Environment restoration");
    require(play_sky() == take(play_project.assets().sky_map(sky_key)).map,
            "An added Environment did not carry its sky map key to Play.");
    // A key Play cannot resolve keeps its authored identity until a valid map arrives.
    const auto environment_id = std::to_string(*a.document_identity(a.environment()));
    const auto set_play_sky = [&](std::string_view value) {
        json::Builder batch{json::Document::literal<R"({"ops":[{"op":"set"}]})">()};
        const auto op = *batch.root().at("ops")->at(0);
        require(batch.set(op, "id", environment_id) && batch.set(op, "sky_map", value), "Cannot construct sky patch.");
        const auto bytes = json::write(batch.root());
        return take(b->apply_live({bytes.data(), bytes.size()}, play_project));
    };
    const auto missing_sky = set_play_sky("project/{00000000-0000-0000-0000-000000000000}");
    require(missing_sky.size() == 1 && missing_sky[0].severity == world::Severity::warning &&
                missing_sky[0].kind == world::DiagnosticKind::missing_resource && !play_sky() &&
                take(presentation::read_sky_map(rb, b->environment())).source ==
                    "project/{00000000-0000-0000-0000-000000000000}",
            "An unresolvable Play sky map was not one warning that keeps its key.");
    require(set_play_sky(sky_key).empty() && scene(a) == scene(*b) && play_sky(),
            "Play did not return to the document's sky map.");
    // An add reads a key Play failed to read before again: the editor may send it once the file is restored.
    const auto late_key = sky_asset(project, "Content/Sky/Late.hdr", "broken\n");
    require(set_play_sky(late_key).size() == 1 && b->unavailable(assets::AssetKind::sky, late_key),
            "Play resolved a corrupt sky map.");
    take(a.select(environment_entity));
    changed(a.erase_entities(a.selected_entities()));
    sync("Environment removal before a restored sky map");
    take(editor::write_file(project.root() / "Content/Sky/Late.hdr", radiance(1, char(131))));
    take(project.refresh());
    take(a.set_sky_map(take(project.assets().sky_map(late_key))));
    sync("Environment creation with a restored sky map");
    require(play_sky() && play_sky() == take(play_project.assets().sky_map(late_key)).map,
            "Play kept an earlier failure for a restored sky map.");
    changed(a.undo());
    sync("Restored sky map undo");
    changed(a.undo());
    sync("Environment removal undo");
    require(play_sky() == take(play_project.assets().sky_map(sky_key)).map,
            "Undoing back to the original Environment lost its sky map in Play.");
    each("Name", [&] { take(a.rename(first, L"Runner")); });
    each("Field", [&] { edit(a, move, speed, Scalar{5.f}, 201); });
    each("Field group", [&] {
        take(a.begin_group_edit(move, std::array{speed, mass}, 202));
        take(a.preview_group_edit(202, std::array{Scalar{6.f}, Scalar{4.5}}));
        take(a.commit_edit(202));
    });
    each("Transform field", [&] {
        const auto& x = a.types().position_fields[0];
        edit(a, first, world::FieldPath{x.begin(), x.end()}, Scalar{7.f}, 203);
    });
    each("Transform", [&] {
        take(a.begin_transform(std::array{first, second}, 204));
        take(a.preview_transform(
            204, std::array{world::Trs{{1, 1, 1}, {}, {2, 2, 2}}, world::Trs{{0, 0, 5}, {}, {1, 1, 1}}}));
        take(a.commit_edit(204));
    });
    each("Transform removal", [&] { take(a.remove_transform(second)); });
    each("Transform addition", [&] { take(a.set_transform(second, {{0, 3, 0}, {}, {1, 1, 1}})); });
    each("Space reference", [&] { changed(a.set_space_reference(second, ObjectId{})); });
    each("Reparent", [&] { changed(a.reparent(second, first)); });
    ObjectId plain;
    each("Entity", [&] {
        changed(a.add_entity(entity_type));
        plain = a.selection().entity;
    });
    each("Duplicate", [&] {
        take(a.select(first));
        changed(a.duplicate_entities(a.selected_entities()));
    });
    each("Deletion", [&] {
        take(a.select(second));
        changed(a.erase_entities(a.selected_entities()));
    });
    each("Component", [&] {
        take(a.select(plain));
        changed(a.add_component(plain, spatial_type));
    });
    const auto body_type = take(ra.find_type("world.CollisionComponent"));
    ObjectId body;
    each("Component with a Transform", [&] {
        take(a.select(plain));
        changed(a.add_component(plain, body_type));
        body = member(a, body_type);
    });
    physics::BodySettings settings;
    settings.shape = physics::ShapeKind::sphere;
    settings.radius = .75f;
    each("Body", [&] { take(a.configure_body(body, settings)); });
    each("Scene instance creation", [&] { take(a.create_scene_material(mesh, "slot/0")); });
    const auto instance = take(presentation::read_material_bindings(ra, mesh)).front().resource.id();
    require(instance != shared, "The override did not create a material instance.");
    each("Material override", [&] { take(a.set_material_override(instance, "roughness", true)); });
    each("Material instance", [&] { material_float(a, instance, "roughness", .8f, 206); });
    const auto shared_roughness = [&](editor::Session& session) {
        return roughness(session, session.loaded_material(material_key));
    };
    material_float(a, shared, "roughness", .6f, 207);
    sync("Material asset");
    require(shared_roughness(*b) == .6f, "A material asset edit did not reach Play.");
    take(a.material_undo(material_key));
    sync("Material asset undo");
    require(shared_roughness(*b) == shared_roughness(a) && shared_roughness(a) != .6f,
            "A material asset undo did not reach Play.");

    acceptance_phase = "Play preview forwarding";
    const auto pose = [&](const editor::Session& session, ObjectId entity) {
        return take(world::read_transform(session.world().runtime(), entity));
    };
    // A leaf a preview returns to its start is still sent: Play saw the earlier preview.
    take(a.begin_transform(std::array{first}, 208));
    const auto start = pose(a, first);
    auto stretched = start;
    stretched.scale = {5, 5, 5};
    take(a.preview_transform(208, std::array{stretched}));
    sync("Scale preview", false);
    auto moved = start;
    moved.position.x += 3;
    take(a.preview_transform(208, std::array{moved}));
    moved.position.x += 1;
    take(a.preview_transform(208, std::array{moved}));
    sync("Move preview", false);
    require(pose(*b, in_b(first)) == pose(a, first),
            "A preview that returned a leaf to its start left Play at the earlier preview.");
    take(a.commit_edit(208));
    sync("Preview commit");
    take(a.begin_edit(move, speed, 209));
    take(a.preview_edit(209, Scalar{9.f}));
    sync("Field preview", false);
    require(std::get<float>(take(rb.read(in_b(move), field(*b, take(rb.find_type("game.Move")), "speed")))) == 9,
            "A field preview did not reach Play.");
    take(a.cancel_edit());
    sync("Field cancel");
    each("Component removal", [&] {
        take(a.select(first));
        changed(a.remove_component(move));
    });

    acceptance_phase = "Play forwarding failure";
    auto procedural = take(project.assets().mesh("builtin/cube"));
    procedural.key.clear();
    changed(a.create_static_mesh(procedural));
    require(!a.take_live_ops() && !a.take_live_ops(), "A change Play cannot follow did not stop forwarding.");
    a.set_live_forwarding(false);
    require(take(a.take_live_ops()).empty(), "Stopping forwarding kept its failure.");
    std::puts(
        "Play change forwarding: PASS (every change kind, undo and redo, previews, cancel, material assets, failure)");
}

// A document's World lighting is its Environment. Explicit authoring creates it
// through ordinary history; opening a scene never fits or rewrites these settings.
void verify_environment_document(editor::Project& project) {
    acceptance_phase = "Environment document";
    using enum world::DiagnosticKind;
    const auto settings = [](const editor::Session& session) { return take(session.presentation().environment()); };
    const auto& empty_scene =
        json::Document::literal<R"({"version":10,"next_id":"1","roots":[],"entities":[],"materials":[]})">();
    const auto load = [&](json::Value data) {
        const auto bytes = json::write(data);
        return take(editor::Session::deserialize({bytes.data(), bytes.size()}, project));
    };
    auto opened = load(empty_scene.root());
    require(opened->environment() == ObjectId{} && opened->load_issues().empty() &&
                settings(*opened) == rendering::SceneLightingSettings{},
            "A document without an Environment did not use the default World settings.");
    auto fitted = settings(*opened);
    fitted.probe_volumes[0].minimum = {-3, -1, -4};
    fitted.probe_volumes[0].maximum = {3, 4, 4};
    fitted.probe_volumes[0].counts = {6, 5, 8};
    fitted.probe_update_budget = 60;
    take(opened->set_environment(fitted));
    const auto owner = take(world::inspect_component(opened->world().runtime(), opened->environment())).owner;
    require(settings(*opened) == fitted && opened->dirty() && opened->undo_count() == 1 && opened->redo_count() == 0 &&
                take(opened->world().members()).size() == 1 && opened->name(owner) == L"Environment" &&
                !take(world::has_transform(opened->world().runtime(), owner)),
            "Explicit World settings did not create one undoable spaceless Environment root.");
    changed(opened->undo());
    require(opened->environment() == ObjectId{}, "Undo did not remove the authored Environment.");
    changed(opened->redo());
    require(settings(*opened) == fitted, "Redo did not restore the authored Environment settings.");
    const auto saved = assets::json_format::parse(take(opened->serialize()));
    require(bool(saved), "Environment scene JSON is invalid.");
    auto reopened = load(saved->root());
    require(saved->root().u32({"version"}) == 10 && settings(*reopened) == fitted && reopened->load_issues().empty(),
            "A saved Environment did not reopen with the same settings.");
    {
        acceptance_phase = "Environment sky map";
        const auto environment_record = [](json::Value data, const std::string& type = "world.EnvironmentComponent") {
            for (auto node : *data.at("entities")->array())
                for (auto part : *node.at("components")->array())
                    if (part.string({"type"}) == type)
                        return part;
            throw std::runtime_error("The scene has no Environment record.");
        };
        const auto current = saved->root();
        require(environment_record(current).string({"sky_map"}) == "",
                "A saved Environment did not write an empty sky map key.");
        json::Builder derived{current};
        require(bool(derived.set(environment_record(derived.root()), "type", "game.Environment")),
                "Cannot construct derived Environment.");
        const auto derived_bytes = json::write(derived.root());
        auto derived_document =
            take(editor::Session::deserialize({derived_bytes.data(), derived_bytes.size()}, project,
                                              prepare(std::make_shared<Evidence>()), editor::LoadPolicy::strict));
        const auto derived_saved = assets::json_format::parse(take(derived_document->serialize()));
        require(derived_document->environment() != ObjectId{} && derived_saved &&
                    derived_saved->root() == derived.root(),
                "An Environment of a derived type did not load as the Environment or did not save unchanged.");
        // A key resolves through the Project's cache; a missing one renders without its map and is saved unchanged.
        const auto key = sky_asset(project, "Content/Sky/Scene.hdr", radiance(1, char(129)));
        const auto map = take(project.assets().sky_map(key)).map;
        json::Builder mapped{current};
        require(bool(mapped.set(environment_record(mapped.root()), "sky_map", key)),
                "Cannot construct mapped Environment.");
        auto bound = load(mapped.root());
        const auto bound_saved = assets::json_format::parse(take(bound->serialize()));
        require(map && settings(*bound).sky_map == map && bound->load_issues().empty() && bound_saved &&
                    bound_saved->root() == mapped.root(),
                "A sky map key did not resolve to the Project's map or did not save unchanged.");
        json::Builder missing{current};
        require(bool(missing.set(environment_record(missing.root()), "sky_map",
                                 "project/{00000000-0000-0000-0000-000000000000}")),
                "Cannot construct missing sky.");
        auto unresolved = load(missing.root());
        const auto unresolved_saved = assets::json_format::parse(take(unresolved->serialize()));
        const auto& issues = unresolved->load_issues();
        require(issues.size() == 1 && issues[0].kind == missing_resource &&
                    issues[0].severity == world::Severity::warning &&
                    issues[0].field == std::vector<std::string>{"sky_map"} && !settings(*unresolved).sky_map &&
                    unresolved_saved && unresolved_saved->root() == missing.root(),
                "A missing sky map was not one warning, or its key did not save unchanged.");
        json::Builder malformed{current};
        require(bool(malformed.set(environment_record(malformed.root()), "sky_map", 5u)),
                "Cannot construct malformed sky.");
        auto kept = load(malformed.root());
        auto kept_saved = assets::json_format::parse(take(kept->serialize()));
        require(kept->load_issues().size() == 1 && kept->load_issues()[0].kind == mismatch && kept_saved &&
                    kept_saved->root() == malformed.root() &&
                    take(kept->retained(kept->environment())).entries.at(0).kind == mismatch,
                "A malformed sky map key was not reported and kept as a mismatch.");
        // set_sky_map is one history item that releases the kept value; undo and redo restore the stored map.
        const auto asset = take(project.assets().sky_map(key));
        take(kept->set_sky_map(asset));
        kept_saved = assets::json_format::parse(take(kept->serialize()));
        require(settings(*kept).sky_map == map && kept->undo_count() == 1 && kept_saved &&
                    kept_saved->root() == mapped.root(),
                "Setting a sky map did not bind it as one history item or kept the malformed value.");
        changed(kept->undo());
        kept_saved = assets::json_format::parse(take(kept->serialize()));
        require(!settings(*kept).sky_map && kept_saved && kept_saved->root() == malformed.root(),
                "Undoing a sky map did not restore the kept value.");
        changed(kept->redo());
        require(settings(*kept).sky_map == map, "Redoing a sky map did not restore the same map.");
        take(kept->set_sky_map({}));
        const auto cleared = assets::json_format::parse(take(kept->serialize()));
        require(!settings(*kept).sky_map && cleared && environment_record(cleared->root()).string({"sky_map"}) == "" &&
                    kept->undo_count() == 2,
                "An empty sky asset did not clear the sky map.");
        changed(kept->undo());
        require(settings(*kept).sky_map == map, "Undoing a cleared sky map did not restore the same map.");
        take(kept->set_sky_map(asset));
        require(kept->undo_count() == 1 && kept->redo_count() == 1, "An unchanged sky map created history.");
        // Without an Environment the first sky map creates one in the same history item.
        auto empty = load(empty_scene.root());
        take(empty->set_sky_map({}));
        require(empty->environment() == ObjectId{} && empty->undo_count() == 0,
                "Clearing an absent sky map created an Environment.");
        take(empty->set_sky_map(asset));
        require(empty->environment() != ObjectId{} && settings(*empty).sky_map == map && empty->undo_count() == 1,
                "The first sky map did not create its Environment.");
        changed(empty->undo());
        require(empty->environment() == ObjectId{} && take(empty->world().members()).empty(),
                "Undoing the first sky map left an Environment.");
    }
    std::puts(
        "Environment document: PASS (load initialization only without an Environment, derived Environment type, sky map keys and history)");
}

void verify_camera_loading(editor::Project& project) {
    acceptance_phase = "Camera combined load validation";
    struct DerivedCamera : presentation::CameraComponent {
        ReferenceSlot target;

        ReferenceSlot held_target;

        ReferenceSequence targets;

        Array<float> weights{.25f, .75f};

        DerivedCamera() {
            near_clip = .25f;
            far_clip = 60;
            vertical_fov = 80;
        }
    };
    const editor::Session::Prepare camera_type = [](world::World& world) -> Result<std::shared_ptr<void>> {
        take(world.runtime().register_type(array_definition<float>(world.runtime(), "game.CameraWeights")));
        take(NativeRegistration<DerivedCamera>{world.runtime(), "game.Camera", TypeKind::object}
                 .base<presentation::CameraComponent>()
                 .field<&DerivedCamera::target>("target",
                                                {TypeReference::native<world::Entity>(), ValueForm::weak_reference})
                 .field<&DerivedCamera::held_target>(
                     "held_target", {TypeReference::native<world::Entity>(), ValueForm::strong_reference})
                 .field<&DerivedCamera::targets>("targets",
                                                 {TypeReference::native<world::Entity>(), ValueForm::strong_sequence})
                 .field<&DerivedCamera::weights>("weights")
                 .publish());
        return {};
    };
    editor::Session source;
    take(source.initialize());
    changed(source.create_camera({2, 3, 4}));
    const auto baseline = assets::json_format::parse(take(source.serialize()));
    require(bool(baseline), "Camera baseline JSON is invalid.");
    const auto entity_id = *source.document_identity(source.selection().entity);
    bool passed = true;
    for (const float far : {2.f, 9.f}) {
        json::Builder input{*baseline};
        const auto part = *input.root().at("entities")->at(0)->at("components")->at(0);
        const auto properties = *part.at("properties");
        json::Builder target;
        require(target.set(target.root(), "$ref", std::to_string(entity_id)) &&
                    input.set(part, "type", "game.Camera") && input.set(properties, "near_clip", 5u) &&
                    input.set(properties, "far_clip", far) && input.set(properties, "vertical_fov", 70u) &&
                    input.set(properties, "target", target.root()) &&
                    input.set(properties, "held_target", target.root()) &&
                    input.set(properties, "weights", json::Document::literal<"[2.0,3.0,5.0]">().root()),
                "Cannot construct camera fixture.");
        const auto targets = *input.array(properties, "targets");
        input.append(targets, target.root());
        input.append_null(targets);
        input.append(targets, target.root());
        const auto component_id = std::stoull(std::string{*part.string({"id"})});
        const bool valid = far > 5;
        const auto input_bytes = json::write(input.root());
        auto document =
            take(editor::Session::deserialize({input_bytes.data(), input_bytes.size()}, project, camera_type));
        bool tolerant_ok = true;
        bool references_ok = true;
        bool sequence_ok = true;
        bool array_ok = true;
        bool roundtrip_ok = true;
        // Reopen the saved retained record as well as checking its first load.
        for (int pass = 0; pass < 2; ++pass) {
            const auto component = take(document->local_object(component_id));
            const auto& issues = document->load_issues();
            tolerant_ok &= valid ? issues.empty()
                                 : issues.size() == 1 && issues[0].kind == world::DiagnosticKind::mismatch &&
                                       issues[0].object == component &&
                                       issues[0].field == std::vector<std::string>{"properties"} &&
                                       document->has_retained(component);
            const auto camera = document->presentation().camera(take(document->local_object(entity_id)));
            tolerant_ok &= camera && camera->near_clip == (valid ? 5.f : .25f) &&
                           camera->far_clip == (valid ? 9.f : 60.f) && camera->vertical_fov == (valid ? 70.f : 80.f);
            auto& runtime = document->world().runtime();
            const auto type = take(runtime.type_of(component));
            const auto expected = valid ? take(document->local_object(entity_id)) : ObjectId{};
            for (const auto name : {"target", "held_target"})
                references_ok &=
                    take(runtime.read_reference(component, take(runtime.find_field(type, name)))).id() == expected;
            const auto targets = take(runtime.reference_field(component, take(runtime.find_field(type, "targets"))));
            const auto count = take(runtime.sequence_size(targets));
            sequence_ok &= count == (valid ? 3u : 0u);
            if (valid && count == 3)
                for (std::size_t i = 0; i < count; ++i)
                    sequence_ok &= take(runtime.sequence_read(targets, i)).id() == (i == 1 ? ObjectId{} : expected);
            Array<float> weights;
            take(runtime.read(component, take(runtime.find_field(type, "weights")),
                              ValueOutput{take(runtime.native_type<Array<float>>()), &weights}));
            array_ok &= valid ? weights.size() == 3 && weights[0] == 2.f && weights[1] == 3.f && weights[2] == 5.f
                              : weights.size() == 2 && weights[0] == .25f && weights[1] == .75f;
            const auto saved = take(document->serialize());
            const auto parsed = assets::json_format::parse(saved);
            roundtrip_ok &= parsed && parsed->root() == input.root();
            if (pass == 0)
                document = take(editor::Session::deserialize(saved, project, camera_type));
        }
        passed &= tolerant_ok && references_ok && sequence_ok && array_ok && roundtrip_ok;
    }
    require(passed, "Camera loading did not enforce its combined projection rule and retained-data contract.");
}

void verify_camera_import(editor::Project& project) {
    acceptance_phase = "Camera forest import forwarding";
    editor::Session source;
    take(source.initialize());
    changed(source.create_camera({2, 3, 4}));
    take(source.set_main_camera(source.selection().entity));
    const auto parsed = assets::json_format::parse(take(source.serialize()));
    require(bool(parsed), "Camera forest JSON is invalid.");
    json::Builder forest{*parsed};
    const auto environment = take(world::inspect_component(source.world().runtime(), source.environment())).owner;
    const auto roots = *forest.array(forest.root(), "roots");
    require(forest.append(roots, std::to_string(*source.document_identity(environment))) &&
                forest.append(roots, std::to_string(*source.document_identity(source.selection().entity))),
            "Cannot order camera roots.");
    editor::Session author;
    take(author.initialize());
    changed(author.add_entity());
    auto play = take(editor::Session::deserialize(take(author.serialize()), project));
    play->enter_play();
    author.set_live_forwarding(true);
    const auto forest_bytes = json::write(forest.root());
    changed(author.instantiate_subtree({forest_bytes.data(), forest_bytes.size()}, project));
    const auto target = take(author.main_camera());
    const auto sync = [&](bool present) {
        json::Builder batch;
        const auto ops = *batch.array(batch.root(), "ops");
        for (const auto& op : take(author.take_live_ops())) {
            const auto value = assets::json_format::parse(op.text);
            require(bool(value), "Camera import operation JSON is invalid.");
            batch.append(ops, value->root());
        }
        const auto bytes = json::write(batch.root());
        const auto issues = take(play->apply_live({bytes.data(), bytes.size()}, project));
        const auto selected = take(play->main_camera());
        bool correct = issues.empty();
        if (present) {
            const auto camera = play->presentation().main_camera();
            correct &= selected != ObjectId{} && selected != target &&
                       play->document_identity(selected) == author.document_identity(target) && camera &&
                       camera->position == math::Vector3{2, 3, 4};
        } else
            correct &= selected == ObjectId{} && play->environment() == ObjectId{} &&
                       take(play->world().members()).size() == 1;
        require(correct, "A multi-root camera import or its history diverged between author and Play.");
    };
    sync(true);
    changed(author.undo());
    sync(false);
    changed(author.redo());
    sync(true);
}

void verify_camera_document(editor::Project& project) {
    acceptance_phase = "Camera authoring, reference history and sparse Play changes";
    editor::Session a;
    take(a.initialize());
    take(a.set_main_camera({}));
    require(a.environment() == ObjectId{} && a.undo_count() == 0, "Clear created an absent Environment.");
    changed(a.create_camera({2, 3, -4}));
    const auto first = a.selection().entity;
    changed(a.create_camera({-5, 1, 7}));
    const auto second = a.selection().entity;
    require(take(a.main_camera()) == ObjectId{}, "Create Camera selected itself as main.");
    auto b = take(editor::Session::deserialize(take(a.serialize()), project));
    b->enter_play();
    a.set_live_forwarding(true);
    const auto in_b = [&](ObjectId object) { return take(b->local_object(*a.document_identity(object))); };
    const auto sync = [&] {
        json::Builder batch;
        const auto ops = *batch.array(batch.root(), "ops");
        for (const auto& op : take(a.take_live_ops())) {
            const auto parsed = assets::json_format::parse(op.text);
            require(bool(parsed), "Camera operation JSON is invalid.");
            batch.append(ops, parsed->root());
        }
        const auto bytes = json::write(batch.root());
        const auto issues = take(b->apply_live({bytes.data(), bytes.size()}, project));
        if (!issues.empty())
            throw std::runtime_error(issues.front().message);
        return json::Document{ops};
    };
    const auto before = a.undo_count();
    take(a.set_main_camera(first));
    const auto initial = sync();
    require(initial.root().size() == 1 && a.undo_count() == before + 1 &&
                take(b->presentation().main_camera()).entity == in_b(first),
            "First Set did not create and forward the Environment with its selection in one history item.");
    changed(a.undo());
    sync();
    require(a.environment() == ObjectId{} && b->environment() == ObjectId{}, "First Set undo left an Environment.");
    changed(a.redo());
    sync();
    require(take(b->presentation().main_camera()).entity == in_b(first), "First Set redo lost the reference.");
    take(a.set_main_camera(second));
    const auto set = sync();
    require(set.root().size() == 1 && set.root().at(0)->at("properties")->size() == 1 &&
                set.root().at(0)->string({"properties", "main_camera", "$ref"}) ==
                    std::to_string(*a.document_identity(second)) &&
                take(b->presentation().main_camera()).entity == in_b(second),
            "Set did not forward only the changed reference by document identity.");
    const auto stable = a.undo_count();
    take(a.set_main_camera(second));
    const auto unchanged = sync();
    require(a.undo_count() == stable && unchanged.root().size() == 0,
            "Unchanged main selection created history or IPC.");
    take(a.set_main_camera({}));
    sync();
    require(!b->presentation().main_camera(), "Clear chose a Play fallback.");
    changed(a.undo());
    sync();
    require(take(b->presentation().main_camera()).entity == in_b(second), "Reference undo did not reach Play.");
    changed(a.redo());
    sync();
    require(take(b->main_camera()) == ObjectId{}, "Reference redo did not clear Play.");
    changed(a.undo());
    sync();
    // Native game writes do not travel back to the document; an unrelated scalar
    // edit must not send the entire Environment and overwrite the game's choice.
    auto& rb = b->world().runtime();
    const auto selected = take(rb.find_field(b->presentation_types().environment_component, "main_camera"));
    auto game_target = take(rb.hold(in_b(first), b->world().types().entity));
    take(rb.write_reference(b->environment(), selected, game_target.reference()));
    auto settings = take(a.presentation().environment());
    settings.exposure_ev = 2;
    take(a.set_environment(settings));
    sync();
    require(take(a.main_camera()) == second && take(b->presentation().main_camera()).entity == in_b(first),
            "A scalar edit overwrote a Play-only reference write.");
    take(rb.write(b->environment(), field(*b, b->presentation_types().environment_component, "exposure_ev"),
                  Scalar{6.f}));
    const auto game_choice = [&] {
        sync();
        require(take(b->presentation().main_camera()).entity == in_b(first) &&
                    take(b->presentation().environment()).exposure_ev == 6,
                "An unrelated add/history resent the existing Environment and overwrote game state.");
    };
    changed(a.create_camera({8, 2, 1}));
    game_choice();
    changed(a.undo());
    game_choice();
    changed(a.redo());
    game_choice();
    take(a.set_main_camera(first));
    sync();
    take(a.set_main_camera(second));
    sync();
    changed(a.add_entity());
    const auto parent = a.selection().entity;
    sync();
    changed(a.reparent(second, parent, world::ReparentRule::keep_local));
    sync();
    take(a.select(parent));
    changed(a.erase_entities(a.selected_entities()));
    sync();
    require(take(a.main_camera()) == ObjectId{} && take(b->main_camera()) == ObjectId{},
            "Subtree deletion did not synchronously clear both selections.");
    const auto deleted = take(a.serialize());
    a.mark_saved();
    require(take(editor::Session::deserialize(deleted, project))->load_issues().empty(),
            "Deleted camera could not be saved and reopened cleanly.");
    changed(a.undo());
    sync();
    require(take(b->presentation().main_camera()).entity == in_b(second),
            "Delete-save-undo did not restore graph and reference.");
    changed(a.redo());
    sync();
    require(take(b->main_camera()) == ObjectId{}, "Deletion redo did not clear the Play reference.");
    changed(a.undo());
    sync();
    const auto saved = take(a.serialize());
    auto reopened = take(editor::Session::deserialize(saved, project, {}, editor::LoadPolicy::strict));
    require(take(reopened->serialize()) == saved && reopened->name(take(reopened->main_camera())) == a.name(second),
            "Camera fields/reference failed strict Scene v10 roundtrip.");
    // The game can destroy a target independently; editor Set reports the dropped
    // reference instead of recreating that target or reusing an ObjectId.
    changed(world::destroy_entity(rb, in_b(first)));
    take(a.set_main_camera(first));
    json::Builder missing;
    const auto missing_ops = *missing.array(missing.root(), "ops");
    for (const auto& op : take(a.take_live_ops())) {
        const auto parsed = assets::json_format::parse(op.text);
        require(bool(parsed), "Missing camera operation JSON is invalid.");
        missing.append(missing_ops, parsed->root());
    }
    const auto missing_bytes = json::write(missing.root());
    const auto dropped = take(b->apply_live({missing_bytes.data(), missing_bytes.size()}, project));
    require(dropped.size() == 1 && dropped[0].kind == world::DiagnosticKind::reference_dropped &&
                take(b->main_camera()) == ObjectId{},
            "Setting a game-destroyed camera resurrected it or omitted its diagnostic.");
    changed(a.add_entity());
    sync();
    changed(a.undo());
    sync();
    changed(a.redo());
    sync();
    require(!b->local_object(*a.document_identity(first)) && take(b->main_camera()) == ObjectId{},
            "An unrelated add/history resurrected a game-deleted camera outside its batch.");

    const auto parsed = assets::json_format::parse(saved);
    require(bool(parsed), "Saved camera scene JSON is invalid.");
    json::Builder malformed{*parsed};
    for (auto entity : *malformed.root().at("entities")->array())
        for (auto part : *entity.at("components")->array())
            if (part.string({"type"}) == "world.EnvironmentComponent")
                require(bool(malformed.set(*part.at("properties"), "main_camera", "not a reference")),
                        "Cannot damage camera reference.");
    const auto malformed_bytes = json::write(malformed.root());
    auto retained = take(editor::Session::deserialize({malformed_bytes.data(), malformed_bytes.size()}, project));
    const auto env = retained->environment();
    require(retained->has_retained(env), "Malformed main camera was not retained.");
    take(retained->set_main_camera({}));
    require(!retained->has_retained(env), "Clear did not release retained reference data.");
    changed(retained->undo());
    const auto undone = assets::json_format::parse(take(retained->serialize()));
    require(undone && undone->root() == malformed.root(), "Reference undo lost retained data.");
    const auto target = take(retained->local_object(*a.document_identity(second)));
    auto& rr = retained->world().runtime();
    auto held = take(rr.hold(target, retained->world().types().entity));
    take(rr.write_reference(env,
                            take(rr.find_field(retained->presentation_types().environment_component, "main_camera")),
                            held.reference()));
    take(retained->select(target));
    changed(retained->erase_entities(retained->selected_entities()));
    require(!retained->has_retained(env) && take(retained->main_camera()) == ObjectId{} && retained->serialize(),
            "Delete left retained data overriding the cleared selection.");
    changed(retained->undo());
    require(retained->has_retained(env) && take(retained->main_camera()) == target,
            "Delete undo did not restore the graph, selection and retained value together.");
    changed(retained->redo());
    require(take(retained->main_camera()) == ObjectId{} && !retained->has_retained(env),
            "Delete redo did not clear retained reference data.");
    std::puts(
        "Camera document: PASS (Scene v10 references, first-Set creation, history, sparse Play forwarding, game deletion, retained data, delete-save-undo)");
}

void run() {
    verify_stop_reentry();
    acceptance_phase = "Source document fields and history";
    const auto directory = std::filesystem::current_path() / "project-scene-evidence" /
                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto project = take(editor::Project::create(directory));
    verify_codec_graph(project);
    acceptance_phase = "Source document fields and history";
    auto evidence = std::make_shared<Evidence>();
    editor::Session document;
    take(document.initialize(prepare(evidence)));
    auto& runtime = document.world().runtime();
    const auto entity_type = take(runtime.find_type("game.Entity"));
    const auto move_type = take(runtime.find_type("game.Move"));
    const auto spatial_type = take(runtime.find_type("game.Spatial"));
    require(take(document.component_types()).size() >= 5, "Derived component discovery failed.");
    changed(document.add_entity(entity_type));
    const auto entity = document.selection().entity;
    take(document.set_transform(entity, {}));
    changed(document.add_component(entity, spatial_type));
    changed(document.add_component(entity, move_type));
    const auto move = member(document, move_type);
    require(take(world::has_transform(runtime, entity)) && take(world::parent_entity(runtime, entity)) == ObjectId{},
            "Non-spatial business component altered the root.");
    const auto speed = field(document, move_type, "speed");
    const auto health = field(document, entity_type, "health");
    require(std::get<float>(take(document.read_property(move, speed))) == 2 &&
                take(document.properties(move)).size() == 4,
            "Default values or scalar discovery failed.");
    edit(document, move, speed, Scalar{4.f}, 11);
    edit(document, entity, health, Scalar{std::int32_t{75}}, 12);
    changed(document.undo());
    require(std::get<std::int32_t>(take(document.read_property(entity, health))) == 100, "Entity scalar undo failed.");
    changed(document.redo());
    take(document.begin_edit(move, speed, 13));
    take(document.preview_edit(13, 6.f));
    take(document.cancel_edit());
    take(document.cancel_edit());
    require(std::get<float>(take(document.read_property(move, speed))) == 4, "Cancel failed to restore scalar value.");
    const auto saved = take(document.serialize());
    const auto saved_json = assets::json_format::parse(saved);
    require(saved_json && saved_json->root().u32({"version"}) == 10 &&
                saved_json->root().at("entities")->at(0)->i64({"properties", "health"}) == 75,
            "Scene did not save real Entity properties.");
    auto reopened = take(editor::Session::deserialize(saved, project, prepare(evidence)));
    const auto reopened_json = assets::json_format::parse(take(reopened->serialize()));
    require(reopened_json && *reopened_json == *saved_json,
            "Derived scene roundtrip changed type, fields or composition.");
    {
        // A float field is written as its shortest decimal and reads back bit for bit; a double keeps its full value.
        const auto mass = field(document, move_type, "mass");
        const float value = .1f;
        take(runtime.write(move, speed, Scalar{value}));
        take(runtime.write(move, mass, Scalar{double(.1f)}));
        const auto text = take(document.serialize());
        const auto parsed = assets::json_format::parse(text);
        require(bool(parsed), "Written numeric scene JSON is invalid.");
        const auto written = *parsed->root().at("entities")->at(0)->at("components")->at(1)->at("properties");
        const auto speed_bytes = json::write(*written.at("speed"));
        const auto mass_bytes = json::write(*written.at("mass"));
        auto loaded = take(editor::Session::deserialize(text, project, prepare(evidence)));
        take(loaded->select(take(loaded->world().members()).front().id()));
        const auto loaded_type = take(loaded->world().runtime().find_type("game.Move"));
        const auto loaded_move = member(*loaded, loaded_type);
        const auto back =
            std::get<float>(take(loaded->read_property(loaded_move, field(*loaded, loaded_type, "speed"))));
        require(
            std::bit_cast<std::uint32_t>(back) == std::bit_cast<std::uint32_t>(value) &&
                std::get<double>(take(loaded->read_property(loaded_move, field(*loaded, loaded_type, "mass")))) ==
                    double(.1f) &&
                std::string_view{speed_bytes.data(), speed_bytes.size()} == "0.1" &&
                std::string_view{mass_bytes.data(), mass_bytes.size()} == "0.10000000149011612",
            "A float field was not saved as its shortest decimal, or a float or double field changed across save and reload.");
        take(runtime.write(move, speed, Scalar{4.f}));
        take(runtime.write(move, mass, Scalar{3.5}));
    }
    json::Builder defaults{*saved_json};
    defaults.erase(*defaults.root().at("entities")->at(0)->at("components")->at(1)->at("properties"), "speed");
    const auto default_bytes = json::write(defaults.root());
    auto defaulted =
        take(editor::Session::deserialize({default_bytes.data(), default_bytes.size()}, project, prepare(evidence)));
    const auto default_entity = take(defaulted->world().members()).front().id();
    take(defaulted->select(default_entity));
    const auto default_type = take(defaulted->world().runtime().find_type("game.Move"));
    require(std::get<float>(take(defaulted->read_property(member(*defaulted, default_type),
                                                          field(*defaulted, default_type, "speed")))) == 2,
            "Absent field did not preserve the constructor default.");
    json::Builder bad{*saved_json};
    require(bool(bad.set(*bad.root().at("entities")->at(0)->at("components")->at(1)->at("properties"), "deleted_field",
                         1u)),
            "Cannot construct unknown-property fixture.");
    const auto bad_bytes = json::write(bad.root());
    const auto releases = evidence->destroyed;
    require(!editor::Session::deserialize({bad_bytes.data(), bad_bytes.size()}, project, prepare(evidence),
                                          editor::LoadPolicy::strict),
            "Strict load accepted an unknown saved property.");
    require(evidence->destroyed == releases + 1 && evidence->live_runtime_on_release,
            "Failed load released Runtime before its prepared code.");
    require(take(document.serialize()) == saved, "Failed candidate load changed the edit document.");
    verify_retained_data(project, *saved_json, evidence);
    verify_play_changes(project, evidence);
    verify_string_properties(project, evidence);
    verify_array_properties(project, evidence);
    verify_live_forwarding(project, evidence);
    verify_environment_document(project);
    verify_camera_loading(project);
    verify_camera_document(project);
    verify_camera_import(project);
    acceptance_phase = "Source play and stop lifecycle";
    const auto history = document.undo_count();
    {
        auto running = take(editor::Session::deserialize(saved, project, prepare(evidence)));
        const auto run_entity = take(running->world().members()).front().id();
        const auto run_root = run_entity;
        require(run_entity != entity, "Play reused editing Runtime identity.");
        changed(running->start());
        changed(running->start());
        take(running->update(.5f));
        require(take(world::read_transform(running->world().runtime(), run_root)).position.x == 2,
                "Source native action did not run after JIT returned.");
        changed(running->stop());
        changed(running->stop());
        changed(running->close());
        changed(running->close());
    }
    require(take(document.serialize()) == saved && document.undo_count() == history,
            "Play/Stop changed editing state/history.");
    auto failure = take(editor::Session::deserialize(saved, project, prepare(evidence)));
    const auto failure_root = take(failure->world().members()).front().id();
    const auto ends = evidence->ends;
    changed(failure->start());
    evidence->fail_tick = true;
    require(!failure->update(.5f), "Tick failure was lost.");
    evidence->fail_tick = false;
    require(!failure->world().running() && evidence->ends == ends + 1 &&
                take(world::read_transform(failure->world().runtime(), failure_root)).position.x == 0,
            "Failed callback applied queued movement or skipped End.");
    changed(failure->stop());
    changed(failure->stop());
    require(evidence->ends == ends + 1, "Repeated stop duplicated End.");
    changed(failure->start());
    take(failure->update(.5f));
    require(take(world::read_transform(failure->world().runtime(), failure_root)).position.x == 2,
            "Restart replayed a failed translation batch.");
    evidence->fail_end = true;
    auto stopped = take(failure->stop());
    evidence->fail_end = false;
    require(stopped.notification_error.has_value() && !failure->world().running(),
            "End error lost the committed stop state.");
    changed(failure->stop());
    evidence->fail_begin = true;
    auto started = take(failure->start());
    evidence->fail_begin = false;
    require(started.notification_error.has_value() && !failure->world().running(), "Begin error left World running.");
    changed(failure->close());
    acceptance_phase = "Source and physics pose ownership";
    auto physical = take(editor::Session::deserialize(saved, project, prepare(evidence)));
    const auto physical_entity = take(physical->world().members()).front().id();
    take(physical->select(physical_entity));
    auto& physical_runtime = physical->world().runtime();
    const auto body_type = take(physical_runtime.find_type("world.CollisionComponent"));
    changed(physical->add_component(physical_entity, body_type));
    const auto body = member(*physical, body_type);
    physics::BodySettings settings;
    settings.shape = physics::ShapeKind::sphere;
    settings.motion = physics::MotionKind::dynamic;
    settings.half_extents = {1, 2, 3};
    settings.radius = .75f;
    take(physical->configure_body(body, settings));
    auto physical_reopened =
        take(editor::Session::deserialize(take(physical->serialize()), project, prepare(evidence)));
    take(physical_reopened->select(take(physical_reopened->world().members()).front().id()));
    const auto reopened_body_type = take(physical_reopened->world().runtime().find_type("world.CollisionComponent"));
    require(take(physics::read_settings(physical_reopened->world().runtime(),
                                        member(*physical_reopened, reopened_body_type))) ==
                take(physics::read_settings(physical_runtime, body)),
            "Scene lost reflected Primitive body settings.");
    changed(physical_reopened->close());
    changed(physical->start());
    require(!physical->update(.01f) && !physical->world().running(), "translate_owner overrode physics ownership.");
    changed(physical->close());
    changed(document.erase_entities(document.selected_entities()));
    require(!document.properties(move), "Detached history object remained editable.");
    changed(document.undo());
    require(document.selection().entity == entity && std::get<float>(take(document.read_property(move, speed))) == 4,
            "Deletion undo lost business identity/fields.");
    changed(document.close());
    changed(reopened->close());
    changed(defaulted->close());
    verify_query_source(project);
    verify_primitive_source(project);
    require(evidence->prepared == evidence->destroyed && evidence->live_runtime_on_release,
            "Prepared code leaked or outlived its Runtime.");
    std::puts(
        "Project Scene acceptance: PASS (real derived types, typed history, defaults, retained data, load cleanup, Play change batches, String properties and strict decoding, Source actions, play/stop lifecycle, Jolt ownership)");
}
}

int main() {
    atelier::install_fatal_handler();
    try {
        run();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Project Scene acceptance: FAIL [%s]: %s\n", acceptance_phase, error.what());
        return 1;
    }
}
