#include "../helpers.hpp"
#include "assets/file.hpp"
#include "assets/json_format.hpp"
#include "assets/material_format.hpp"
#include "editor/project.hpp"
#include "editor/project_code.hpp"
#include "editor/properties.hpp"
#include "editor/session.hpp"
#include "geometry/mesh.hpp"
#include "physics/physics.hpp"
#include "runtime/native.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>

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
    String label;

    Result<void> validate() const {
        if (health < 0)
            return std::unexpected(Error{ErrorCode::argument_mismatch, "Health must be nonnegative."});
        if (label.view().std_view() == "reserved")
            return std::unexpected(
                Error{ErrorCode::invocation_failed, "Reserved label.", SourceLocation{"entity.source", 7, 12, 3, 4}});
        return {};
    }
};

enum class LayerMask : std::uint32_t { none = 0u, world_static = 1u, dynamic_body = 2u };

struct MotionCache {
    math::Vector3 velocity{};
};

struct SpeedRange {
    float minimum{1};

    float maximum{5};

    static Result<void> check(const SpeedRange& range) noexcept {
        if (range.minimum > range.maximum)
            return std::unexpected(Error{ErrorCode::argument_mismatch, "Speed range is reversed."});
        return {};
    }
};

struct Move : world::Component {
    float speed{2};

    float runtime_meter{1};

    MotionCache cache{};

    SpeedRange range{};

    LayerMask mask{LayerMask::world_static};
};

Result<std::shared_ptr<void>> prepare(world::World& world) {
    auto& runtime = world.runtime();
    take(NativeRegistration<SpeedRange>{runtime, "game.SpeedRange", TypeKind::record}
             .field<&SpeedRange::minimum>("minimum")
             .field<&SpeedRange::maximum>("maximum")
             .validate<&SpeedRange::check>()
             .publish());
    take(NativeRegistration<MotionCache>{runtime, "game.MotionCache", TypeKind::record}
             .field<&MotionCache::velocity>("velocity")
             .transient()
             .publish());
    take(NativeRegistration<LayerMask>{runtime, "game.LayerMask", TypeKind::enumeration}
             .flags()
             .enumerator("none", LayerMask::none, "无")
             .enumerator("world_static", LayerMask::world_static, "静态层", "地形")
             .enumerator("dynamic_body", LayerMask::dynamic_body, "动态层")
             .publish());
    take(NativeRegistration<Entity>{runtime, "game.Entity", TypeKind::object}
             .base<world::Entity>()
             .field<&Entity::health>("health")
             .field<&Entity::label>("label")
             .validate<&Entity::validate>()
             .publish());
    take(NativeRegistration<Move>{runtime, "game.Move", TypeKind::object}
             .base<world::Component>()
             .field<&Move::speed>("speed")
             .field<&Move::runtime_meter>("runtime_meter")
             .transient()
             .read_only()
             .field<&Move::cache>("cache")
             .field<&Move::range>("range")
             .field<&Move::mask>("mask")
             .publish());
    return std::shared_ptr<void>{};
}

void verify_rejected_graph(editor::Session& document, ObjectId root, editor::Project& project) {
    const auto snapshot = take(document.serialize());
    const auto subtree = take(assets::json_format::parse(take(document.serialize_subtree(root))));
    json::Builder broken(subtree);
    const auto root_key = take(take(broken.root().at("roots")).at(0));
    const std::string key(take(root_key.string()));
    const auto add_material = [](json::Builder& graph) {
        const auto instance = graph.append_object(take(graph.root().at("materials")));
        graph.set_validated(instance, "id", "900001");
        graph.set_validated(instance, "source", "");
        graph.set_validated(instance, "parent", "builtin/material/standard-opaque-double");
        take(graph.array(instance, "entries"));
        graph.set_validated(graph.root(), "next_id", "900002");
    };
    add_material(broken);
    broken.set_validated(root_key, "999999");
    const auto text = json::write(broken.root());
    const auto objects = document.world().runtime().live_objects();
    const auto history = document.undo_count();
    const auto revision = document.revision();
    document.set_live_forwarding(true);
    const auto refused = document.instantiate_subtree({text.data(), text.size()}, project);
    require(!refused && refused.error().message == "Invalid graph root." &&
                document.world().runtime().live_objects() == objects && document.undo_count() == history &&
                document.revision() == revision && take(document.take_live_ops()).empty() &&
                take(document.serialize()) == snapshot,
            "Failed import left provisional objects, a document edit or an outgoing Play change behind.");
    document.set_live_forwarding(false);

    editor::Session play;
    take(play.initialize(prepare));
    play.enter_play();
    const auto baseline = play.world().runtime().live_objects();
    json::Builder batch;
    const auto ops = take(batch.array(batch.root(), "ops"));
    const auto add = batch.append_object(ops);
    batch.set_validated(add, "op", "add");
    take(batch.set_null(add, "parent"));
    take(batch.set_null(add, "index"));
    batch.set_validated(add, "graph", broken.root());
    const auto apply = [&] {
        const auto bytes = json::write(batch.root());
        return take(play.apply_live({bytes.data(), bytes.size()}, project));
    };
    const auto rejected = apply();
    require(rejected.size() == 1 && rejected.front().message == "Play change 0 #999999: Invalid graph root." &&
                rejected.front().kind == world::DiagnosticKind::command_rejected &&
                rejected.front().severity == world::Severity::error && take(play.world().members()).empty() &&
                play.world().runtime().live_objects() == baseline,
            "A rejected Play add published part of its graph.");

    batch.set_validated(add, "graph", subtree.root());
    const auto change = [&](std::string_view op) {
        const auto item = batch.append_object(ops);
        batch.set_validated(item, "op", op);
        batch.set_validated(item, "id", key);
        return item;
    };
    batch.set_validated(change("set"), "name", "");
    const auto properties = batch.object_validated(change("set"), "properties");
    batch.set_validated(properties, "label", "reserved");
    change("remove");
    take(batch.set(change("set"), "enabled", false));
    const auto issues = apply();
    require(issues.size() == 3 && issues[0].kind == world::DiagnosticKind::command_rejected &&
                issues[0].error_code == ErrorCode::invalid_definition &&
                issues[1].error_code == ErrorCode::argument_mismatch &&
                issues[1].message == "game.Entity: Reserved label." && issues[1].source &&
                issues[1].source->path == "entity.source" && issues[1].source->line == 3 && issues[2].serial == 3 &&
                issues[2].kind == world::DiagnosticKind::reference_dropped &&
                issues[2].severity == world::Severity::warning && take(play.world().members()).empty(),
            "Play failed to reuse rejected identities, preserve the edit error or skip a destroyed target.");
    const auto identity = *document.document_identity(root);
    const auto gone = play.local_object(identity);
    require(!gone &&
                gone.error().message == "Document identity does not belong to a live Entity, Component or material.",
            "A destroyed Play identity resolved as a live object.");
    json::Builder retry;
    retry.append(take(retry.array(retry.root(), "ops")), add);
    const auto retry_text = json::write(retry.root());
    require(take(play.apply_live({retry_text.data(), retry_text.size()}, project)).empty() &&
                play.world().contains(take(play.local_object(identity))),
            "Re-adding a Play graph did not resolve its document identity to its new instance.");
    changed(play.close());

    json::Builder with_reference{take(assets::json_format::parse(snapshot))};
    const auto reference = with_reference.object_validated(with_reference.root(), "future_reference");
    const auto open = [&] {
        const auto bytes = json::write(with_reference.root());
        return editor::Session::deserialize({bytes.data(), bytes.size()}, project, prepare);
    };
    with_reference.set_validated(reference, "$ref", "999999");
    const auto dangling = open();
    require(!dangling && dangling.error().message == "Reference target is outside this graph: 999999",
            "Tolerant load accepted a dangling reference at document scope.");
    with_reference.set_validated(reference, "$ref", key);
    auto retained_play = take(open());
    retained_play->enter_play();
    json::Builder remove;
    const auto command = remove.append_object(take(remove.array(remove.root(), "ops")));
    remove.set_validated(command, "op", "remove");
    remove.set_validated(command, "id", key);
    const auto removal = json::write(remove.root());
    const auto dropped = [&] {
        return std::ranges::count(retained_play->issues(), world::DiagnosticKind::reference_dropped,
                                  &world::Diagnostic::kind);
    };
    require(take(retained_play->apply_live({removal.data(), removal.size()}, project)).empty() && dropped() == 1 &&
                take(retained_play->apply_live({retry_text.data(), retry_text.size()}, project)).empty() &&
                dropped() == 0,
            "Retained reference diagnostics did not follow the current Play instance.");
    changed(retained_play->close());

    const auto scene_root = with_reference.root();
    add_material(with_reference);
    with_reference.set_validated(reference, "$ref", "900001");
    const auto entity_record = take(take(scene_root.at("entities")).at(0));
    with_reference.set_validated(entity_record, "future_material", reference);
    auto material_scene = take(open());
    do {
        take(material_scene->collect_step());
    } while (material_scene->world().runtime().collection_phase() != CollectionPhase::idle);
    const auto verify_material = [&](bool document_ref) {
        const auto saved = take(assets::json_format::parse(take(material_scene->serialize())));
        const auto entity = take(take(saved.root().at("entities")).at(0));
        const auto top = take(saved.root().find("future_reference"));
        const auto definitions = take(take(saved.root().at("materials")).array());
        bool has_definition{};
        for (const auto definition : definitions)
            if (take(definition.string({"id"})) == "900001")
                has_definition = true;
        require(has_definition && material_scene->local_object(900001).has_value() &&
                    take(entity.string({"future_material", "$ref"})) == "900001" &&
                    (document_ref ? top && take(top->string({"$ref"})) == "900001" : !top),
                "A material owned by retained fields did not survive collection and save.");
    };
    verify_material(true);
    take(material_scene->discard_retained({}));
    verify_material(false);
    take(material_scene->undo());
    verify_material(true);
    changed(material_scene->close());
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
    // Native components can supply a value the scene format cannot store.
    const auto entity_identity = document.document_identity(entity);
    const auto component_identity = document.document_identity(move);
    take(runtime.with_native<Move>(move, [](Move& value) -> Result<void> {
        value.speed = std::numeric_limits<float>::quiet_NaN();
        return {};
    }));
    const auto rejected = document.serialize_subtree(entity);
    require(!rejected && rejected.error().code == ErrorCode::invalid_storage &&
                rejected.error().message == "Scene property values must be finite." &&
                document.document_identity(entity) == entity_identity &&
                document.document_identity(move) == component_identity,
            "Rejected subtree capture lost its diagnostic or published provisional identities.");
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
    const auto properties = [](json::Value root) {
        const auto entity = take(take(root.at("entities")).at(0));
        const auto component = take(take(entity.at("components")).at(0));
        return take(component.at("properties"));
    };
    json::Builder old_scene{take(assets::json_format::parse(take(document.serialize())))};
    const auto props = properties(old_scene.root());
    require(!take(props.find("runtime_meter")) && !take(props.find("cache")), "Scene serialized transient fields.");
    take(old_scene.set(props, "runtime_meter", 42.f));
    const auto cache = old_scene.object_validated(props, "cache");
    const auto velocity = old_scene.object_validated(cache, "velocity");
    old_scene.set_validated(velocity, "old_lane", "obsolete");
    const auto load_old = [&](editor::LoadPolicy policy) {
        const auto bytes = json::write(old_scene.root());
        return editor::Session::deserialize({bytes.data(), bytes.size()}, project, prepare, policy);
    };
    auto reopened = take(load_old(editor::LoadPolicy::strict));
    require(reopened->load_issues().empty(), "Strict load reported a transient field.");
    old_scene.set_validated(cache, "future_field", "keep");
    std::vector<std::vector<std::string>> kept_paths;
    take(editor::decode_properties(
        runtime, {move_type, move}, props,
        {[&](std::vector<std::string> at, json::Value, world::DiagnosticKind kind, const std::string&) -> Result<void> {
             require(kind == world::DiagnosticKind::unknown_field, "Unexpected tolerant decode diagnostic.");
             kept_paths.push_back(std::move(at));
             return {};
         },
         [](std::vector<std::string>, json::Value) -> Result<void> { return {}; }}));
    require(kept_paths == std::vector<std::vector<std::string>>{{"cache", "future_field"}},
            "Shared tolerant decode_properties did not skip transient fields.");
    old_scene.set_validated(props, "speed", "legacy speed");
    const auto strict = load_old(editor::LoadPolicy::strict);
    auto tolerant = take(load_old(editor::LoadPolicy::tolerant));
    const auto issues = tolerant->load_issues();
    require(issues.size() == 2 && !strict && strict.error().message == issues.front().message &&
                std::ranges::any_of(issues,
                                    [](const auto& issue) {
                                        return issue.kind == world::DiagnosticKind::unknown_field &&
                                               issue.field ==
                                                   std::vector<std::string>{"properties", "cache", "future_field"};
                                    }) &&
                std::ranges::any_of(issues,
                                    [](const auto& issue) {
                                        return issue.kind == world::DiagnosticKind::mismatch &&
                                               issue.field == std::vector<std::string>{"properties", "speed"};
                                    }),
            "Strict load lost its first error or tolerant load lost a rejected field.");
    const auto owner = take(tolerant->local_object(*document.document_identity(entity)));
    const auto restored_move = take(tolerant->local_object(*document.document_identity(move)));
    const auto kept = take(tolerant->retained(restored_move));
    const auto visible = tolerant->issues();
    require(kept.entries.size() == 2 && visible.size() == 2 &&
                std::ranges::all_of(
                    visible, [&](const auto& issue) { return issue.entity == owner && issue.object == restored_move; }),
            "Retained field diagnostics lost their document or component identity.");
    const auto resaved = take(assets::json_format::parse(take(tolerant->serialize())));
    const auto saved_props = properties(resaved.root());
    const auto saved_cache = take(saved_props.at("cache"));
    require(!take(saved_props.find("runtime_meter")) && !take(saved_cache.find("velocity")) &&
                take(take(saved_cache.at("future_field")).string()) == "keep" &&
                take(saved_props.string({"speed"})) == "legacy speed",
            "Resaving lost the original mismatched value or unknown sibling, or retained transient data.");
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
    old_scene.erase(cache, "future_field");
    take(old_scene.set(props, "speed", 4.f));
    const auto range = take(props.at("range"));
    take(old_scene.set(range, "minimum", 9.f));
    take(old_scene.set(range, "maximum", 7.f));
    const auto bad_component = load_old(editor::LoadPolicy::strict);
    require(!bad_component && bad_component.error().message.contains("Speed range is reversed"),
            "Strict scene load accepted an invalid custom component record.");
    const auto authored_entity = take(take(old_scene.root().at("entities")).at(0));
    take(old_scene.set(take(authored_entity.at("properties")), "health", std::int64_t{-3}));
    const auto bad_entity = load_old(editor::LoadPolicy::strict);
    require(!bad_entity && bad_entity.error().message.contains("Health must be nonnegative"),
            "Strict scene load accepted an invalid custom entity.");
    old_scene.set_validated(range, "discarded_with_range", "obsolete");
    auto recovered = take(load_old(editor::LoadPolicy::tolerant));
    const auto diagnostics = recovered->load_issues();
    require(diagnostics.size() == 3 && diagnostics[0].field == std::vector<std::string>{"properties"} &&
                diagnostics.back().field == std::vector<std::string>{"properties", "range"},
            "Scene recovery must diagnose each invalid record.");
    auto& recovered_runtime = recovered->world().runtime();
    const auto recovered_owner = take(recovered->world().members()).front().id();
    const auto recovered_move = take(world::components(recovered_runtime, recovered_owner)).front().id();
    require(take(recovered_runtime.with_native<const Entity>(
                recovered_owner, [](const Entity& e) -> Result<bool> { return e.health == 100; })) &&
                take(recovered_runtime.with_native<const Move>(recovered_move,
                                                               [](const Move& m) -> Result<bool> {
                                                                   return m.range.minimum == 1.f &&
                                                                          m.range.maximum == 5.f && m.speed == 4.f &&
                                                                          static_cast<std::uint32_t>(m.mask) == 3u;
                                                               })),
            "Tolerant load must restore the constructor defaults for the failed record and preserve valid siblings.");
    const auto repaired = take(recovered->serialize());
    const auto clean_doc = take(assets::json_format::parse(repaired));
    const auto clean_range = take(properties(clean_doc.root()).at("range"));
    require(take(clean_range.number({"minimum"})) == 1.0 && take(clean_range.number({"maximum"})) == 5.0 &&
                !take(clean_range.find("discarded_with_range")),
            "Resaving a repaired record must persist its valid defaults.");
    auto strict_reopen = take(editor::Session::deserialize(repaired, project, prepare, editor::LoadPolicy::strict));
    require(strict_reopen->load_issues().empty(), "A repaired scene reported another issue after resave.");
    changed(strict_reopen->close());
    verify_rejected_graph(document, entity, project);
    changed(recovered->close());
    changed(tolerant->close());
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

void verify_procedural_generator_scene() {
    const auto directory = std::filesystem::current_path() / "project-scene-evidence" /
                           ("procgen-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto project = take(editor::Project::create(directory));
    const std::string_view graph_json = R"({
  "nodes": {
    "sphere": {
      "inputs": {},
      "position": [0.0, 0.0],
      "properties": {"latitudes": 8, "radius": 2.0, "segments": 8},
      "type": "procedural.Sphere"
    },
    "scatter": {
      "inputs": {"surface": "sphere.output"},
      "position": [0.0, 140.0],
      "properties": {"density": 0.4, "min_distance": 0.5, "seed": 3},
      "type": "procedural.Scatter"
    },
    "inst": {
      "inputs": {"points": "scatter.points"},
      "position": [0.0, 280.0],
      "properties": {"meshes": ["builtin/cube"]},
      "type": "procedural.CopyToPoints"
    },
    "out": {
      "inputs": {"inputs": ["sphere.output", "inst.output"]},
      "position": [120.0, 420.0],
      "properties": {},
      "type": "procedural.Merge"
    }
  },
  "settings": {"display": "out", "seed": 42},
  "version": 4
})";
    const auto graph_key = take(project.create_procedural_graph(L"Content/Graphs/Island.procgraph", graph_json));
    const auto mat_key = take(project.create_material(
        L"Content/Materials/Stone.material",
        assets::MaterialAsset{{},
                              L"Stone",
                              assets::MaterialInstanceAsset{
                                  "builtin/material/standard-opaque-double",
                                  {{"base_color", math::Vector4{.62f, .58f, .50f, 1.f}}, {"roughness", .76f}}}}));

    editor::Session doc;
    take(doc.initialize());
    doc.set_live_forwarding(true);
    changed(doc.create_procedural_generator(graph_key, {3.f, 1.f, -2.f}, &project));
    const ObjectId gen_entity = doc.selection().entity;
    ObjectId gen_comp{};
    for (const auto& c : take(world::components(doc.world().runtime(), gen_entity)))
        if (take(doc.world().runtime().type_of(c.id())) == doc.presentation_types().procedural_generator_component)
            gen_comp = c.id();
    require(gen_comp != ObjectId{}, "ProceduralGeneratorEntity did not attach ProceduralGeneratorComponent.");
    take(doc.assign_material(project, gen_comp, "default", mat_key));
    const auto gen_type = doc.presentation_types().procedural_generator_component;
    take(doc.set_properties(
        gen_comp, std::array{editor::PropertyPatch{{take(doc.world().runtime().find_field(gen_type, "override_seed"))},
                                                   Scalar{true}}}));
    take(doc.set_properties(gen_comp,
                            std::array{editor::PropertyPatch{{take(doc.world().runtime().find_field(gen_type, "seed"))},
                                                             Scalar{std::int32_t{99}}}}));
    take(doc.set_properties(
        gen_comp, std::array{editor::PropertyPatch{{take(doc.world().runtime().find_field(gen_type, "chunk_size"))},
                                                   Scalar{12.f}}}));
    take(doc.sync_procedural_generators(&project));
    take(doc.presentation().update());
    const auto initial_slots = doc.presentation().generator_slots();
    require(initial_slots.size() == 1 && initial_slots[0].item && initial_slots[0].item->surface_geometry != nullptr &&
                !initial_slots[0].item->collections.empty(),
            "ProceduralGeneratorComponent did not publish surface geometry and instance collections.");
    const std::size_t initial_col_count = initial_slots[0].item->collections.size();
    const std::uint64_t initial_gen = initial_slots[0].item->output_generation;

    // Recooking an already-renderable generator must not advance World::revision() (no full-scene relayout).
    const std::uint64_t rev_before_seed = doc.world().revision();
    take(doc.set_properties(gen_comp,
                            std::array{editor::PropertyPatch{{take(doc.world().runtime().find_field(gen_type, "seed"))},
                                                             Scalar{std::int32_t{101}}}}));
    take(doc.sync_procedural_generators(&project));
    take(doc.presentation().update());
    require(doc.world().revision() == rev_before_seed &&
                doc.presentation().generator_slots()[0].item->output_generation > initial_gen,
            "Recooking an active ProceduralGeneratorComponent unnecessarily bumped World::revision().");
    take(doc.set_properties(gen_comp,
                            std::array{editor::PropertyPatch{{take(doc.world().runtime().find_field(gen_type, "seed"))},
                                                             Scalar{std::int32_t{99}}}}));
    take(doc.sync_procedural_generators(&project));
    take(doc.presentation().update());
    require(doc.world().revision() == rev_before_seed,
            "Restoring ProceduralGeneratorComponent seed unnecessarily bumped World::revision().");

    // Disabling either ProceduralGeneratorComponent or its owning Entity stops cooking and hides rendered output.
    const std::size_t active_render_count = take(doc.presentation().render_items()).size();
    require(active_render_count > 1, "Expected surface + instance render_items while generator is enabled.");
    auto& mut_world = const_cast<world::World&>(doc.world());
    changed(mut_world.set_enabled(gen_comp, false));
    take(doc.sync_procedural_generators(&project));
    require(take(presentation::read_procedural_source(doc.world().runtime(), gen_comp)).output == nullptr &&
                take(doc.presentation().render_items()).empty(),
            "Disabling ProceduralGeneratorComponent did not clear cooked output and hide render_items.");
    changed(mut_world.set_enabled(gen_comp, true));
    take(doc.sync_procedural_generators(&project));
    require(take(doc.presentation().render_items()).size() == active_render_count,
            "Re-enabling ProceduralGeneratorComponent did not restore render_items.");

    changed(mut_world.set_enabled(gen_entity, false));
    take(doc.sync_procedural_generators(&project));
    require(take(presentation::read_procedural_source(doc.world().runtime(), gen_comp)).output == nullptr &&
                take(doc.presentation().render_items()).empty(),
            "Disabling ProceduralGeneratorEntity did not clear cooked output and hide render_items.");
    changed(mut_world.set_enabled(gen_entity, true));
    take(doc.sync_procedural_generators(&project));
    require(take(doc.presentation().render_items()).size() == active_render_count,
            "Re-enabling ProceduralGeneratorEntity did not restore render_items.");

    // A failed current cook retains the last visible output but cannot commit it as a static hierarchy.
    const auto previous_output = take(presentation::read_procedural_source(doc.world().runtime(), gen_comp)).output;
    const auto member_count = take(doc.world().members()).size();
    take(doc.open_graph(project, graph_key));
    const auto invalid_mesh = take(assets::json_format::parse(R"({"meshes":["project/missing-bake-mesh"]})"));
    take(doc.graph_set_properties(graph_key, "inst", invalid_mesh.root()));
    const auto history = doc.undo_count();
    const auto failed_bake = doc.bake_procedural_generator(&project, gen_comp);
    const auto* cook_error = doc.procedural_generator_error(gen_comp);
    require(
        !failed_bake && cook_error && failed_bake.error().message == cook_error->message &&
            take(presentation::read_procedural_source(doc.world().runtime(), gen_comp)).output == previous_output &&
            take(doc.world().members()).size() == member_count && doc.undo_count() == history,
        "A failed generator cook must retain preview, reject Bake with the cook error, and leave scene history intact.");
    take(doc.graph_undo(graph_key, false));
    take(doc.sync_procedural_generators(&project));
    require(!doc.procedural_generator_error(gen_comp), "Generator cook error survived repair of its source graph.");

    const auto saved = take(doc.serialize());
    auto strict_reloaded = take(editor::Session::deserialize(saved, project, prepare, editor::LoadPolicy::strict));
    require(strict_reloaded->load_issues().empty(), "Strict reload of ProceduralGeneratorEntity reported issues.");
    take(strict_reloaded->presentation().update());
    const auto reloaded_slots = strict_reloaded->presentation().generator_slots();
    require(reloaded_slots.size() == 1 && reloaded_slots[0].item &&
                reloaded_slots[0].item->surface_geometry != nullptr &&
                reloaded_slots[0].item->collections.size() == initial_col_count,
            "Strict reload did not reproduce ProceduralGeneratorComponent render items.");

    // Strict current-schema contract: missing "graph" or "materials" must be rejected without legacy shims.
    {
        const auto parsed = take(assets::json_format::parse(saved));
        json::Builder no_graph(parsed.root());
        const auto comp = take(take(take(take(no_graph.root().at("entities")).at(0)).at("components")).at(0));
        no_graph.erase(comp, "graph");
        const auto text = json::write(no_graph.root());
        require(!editor::Session::deserialize({text.data(), text.size()}, project, {}, editor::LoadPolicy::strict),
                "Strict scene load accepted ProceduralGeneratorComponent missing 'graph'.");
    }
    {
        const auto parsed = take(assets::json_format::parse(saved));
        json::Builder no_mats(parsed.root());
        const auto comp = take(take(take(take(no_mats.root().at("entities")).at(0)).at("components")).at(0));
        no_mats.erase(comp, "materials");
        const auto text = json::write(no_mats.root());
        require(!editor::Session::deserialize({text.data(), text.size()}, project, {}, editor::LoadPolicy::strict),
                "Strict scene load accepted ProceduralGeneratorComponent missing 'materials'.");
    }

    // Tolerant load with an unknown slot in "materials" must drop retained "materials" once assign_material edits it.
    {
        const auto parsed = take(assets::json_format::parse(saved));
        json::Builder extra_slot(parsed.root());
        const auto comp = take(take(take(take(extra_slot.root().at("entities")).at(0)).at("components")).at(0));
        const auto mats = take(comp.at("materials"));
        const auto first_mat_res = std::string{take(take(mats.at(0)).string({"resource"}))};
        const auto stale = extra_slot.append_object(mats);
        extra_slot.set_validated(stale, "slot", "obsolete_slot");
        extra_slot.set_validated(stale, "resource", first_mat_res);
        take(extra_slot.set(stale, "overridden", true));
        const auto text = json::write(extra_slot.root());
        auto tolerant_doc =
            take(editor::Session::deserialize({text.data(), text.size()}, project, {}, editor::LoadPolicy::tolerant));
        const ObjectId tol_ent = take(tolerant_doc->world().members()).front().id();
        const ObjectId tol_comp = take(world::components(tolerant_doc->world().runtime(), tol_ent)).front().id();
        require(tolerant_doc->has_retained(tol_comp), "Expected unknown generator material slot to be retained.");
        take(tolerant_doc->clear_material(tol_comp, "default"));
        take(tolerant_doc->assign_material(project, tol_comp, "default", mat_key));
        require(!tolerant_doc->has_retained(tol_comp),
                "MaterialBindingChange did not release retained 'materials' record on ProceduralGeneratorComponent.");
        const auto repaired_text = take(tolerant_doc->serialize());
        auto strict_after_repair =
            take(editor::Session::deserialize(repaired_text, project, {}, editor::LoadPolicy::strict));
        require(strict_after_repair->load_issues().empty(),
                "Repaired ProceduralGeneratorComponent material bindings failed strict reload.");
        changed(strict_after_repair->close());
        changed(tolerant_doc->close());
    }

    // Corrupt .procgraph on disk must fail LoadPolicy::strict and isolate cleanly under LoadPolicy::tolerant.
    {
        const auto broken_key = take(project.create_procedural_graph(L"Content/Graphs/Broken.procgraph", graph_json));
        const auto broken_path = take(project.resolve(project.entry(broken_key)->path));
        take(editor::write_file(broken_path, "{corrupt json"));
        take(project.refresh());

        const auto parsed = take(assets::json_format::parse(saved));
        json::Builder corrupt_ref(parsed.root());
        const auto comp = take(take(take(take(corrupt_ref.root().at("entities")).at(0)).at("components")).at(0));
        corrupt_ref.set_validated(comp, "graph", broken_key);
        const auto text = json::write(corrupt_ref.root());
        require(!editor::Session::deserialize({text.data(), text.size()}, project, {}, editor::LoadPolicy::strict),
                "Strict scene load accepted a syntactically corrupt .procgraph asset.");
        auto tolerant_corrupt =
            take(editor::Session::deserialize({text.data(), text.size()}, project, {}, editor::LoadPolicy::tolerant));
        require(!tolerant_corrupt->load_issues().empty(),
                "Tolerant scene load did not report unreadable_resource for corrupt .procgraph.");
        take(tolerant_corrupt->sync_procedural_generators(&project));
        changed(tolerant_corrupt->close());
    }

    // Play live-forwarding contract: start from empty Play copy and apply recorded live ops.
    editor::Session empty_base;
    take(empty_base.initialize());
    const auto empty_saved = take(empty_base.serialize());
    changed(empty_base.close());
    auto play_copy = take(editor::Session::deserialize(empty_saved, project, {}, editor::LoadPolicy::strict));
    play_copy->enter_play();
    const auto live_ops = take(doc.take_live_ops());
    require(!live_ops.empty(), "ProceduralGeneratorComponent edits produced no live-forwarding ops.");
    json::Builder batch_builder;
    const auto ops_arr = take(batch_builder.array(batch_builder.root(), "ops"));
    for (const auto& op : live_ops) {
        const auto parsed_op = take(assets::json_format::parse(op.text));
        batch_builder.append(ops_arr, parsed_op.root());
    }
    const auto batch_text = json::write(batch_builder.root());
    const auto live_diags = take(play_copy->apply_live({batch_text.data(), batch_text.size()}, project));
    require(live_diags.empty(), "Play copy rejected ProceduralGeneratorComponent live-forwarding batch.");
    take(play_copy->sync_procedural_generators(&project));
    take(play_copy->presentation().update());
    const auto play_slots = play_copy->presentation().generator_slots();
    require(play_slots.size() == 1 && play_slots[0].item && play_slots[0].item->surface_geometry != nullptr &&
                play_slots[0].item->collections.size() == initial_col_count,
            "Play live-forwarding did not reproduce ProceduralGeneratorComponent render items.");

    changed(play_copy->close());
    changed(strict_reloaded->close());
    changed(doc.close());
}

void verify_examples_procedural_showcases() {
    const std::filesystem::path examples_root{ATELIER_EXAMPLE_PROJECT};
    const auto temp_dir =
        std::filesystem::current_path() / "project-scene-evidence" /
        ("examples-proc-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(temp_dir / "Content");
    const auto copy_opts = std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing;
    std::filesystem::copy(examples_root / "Config", temp_dir / "Config", copy_opts);
    std::filesystem::copy(examples_root / "Source", temp_dir / "Source", copy_opts);
    std::filesystem::copy(examples_root / "Content/Procedural", temp_dir / "Content/Procedural", copy_opts);

    auto project = take(editor::Project::open(temp_dir));

    // 1. ProceduralShowcase.scene
    {
        const auto scene_file = take(project.resolve("Content/Procedural/Scenes/ProceduralShowcase.scene"));
        const auto scene_text = take(assets::read_file(scene_file, 16 * 1024 * 1024));
        auto loaded = editor::load_project_scene(project, scene_text, editor::LoadPolicy::strict);
        if (loaded.error)
            fatal(loaded.error->message);
        require(loaded.session != nullptr && loaded.code != nullptr && loaded.session->load_issues().empty() &&
                    loaded.session->issues().empty(),
                "ProceduralShowcase.scene reported strict load or runtime issues.");

        take(loaded.session->presentation().update());
        const auto gen_slots = loaded.session->presentation().generator_slots();
        require(gen_slots.size() == 3, "ProceduralShowcase.scene expected 3 ProceduralGeneratorEntity islands.");
        for (const auto& slot : gen_slots) {
            require(slot.item && slot.item->surface_geometry != nullptr && !slot.item->collections.empty() &&
                        slot.item->surface_materials != nullptr && !slot.item->surface_materials->empty(),
                    "ProceduralShowcase island missing surface mesh, surface material, or CopyToPoints instances.");
        }
        require(*gen_slots[0].item->collections.front().transforms !=
                    *gen_slots[1].item->collections.front().transforms,
                "override_seed on West Outpost Island did not produce distinct instance placements.");
        const auto showcase_source =
            take(presentation::read_procedural_source(loaded.session->world().runtime(), gen_slots[0].item->component));
        require(
            showcase_source.output && showcase_source.output->mesh_dependencies.size() == 3,
            "ProceduralShowcase generator did not record all 3 display mesh dependencies (including MeshImport TerrainGrid).");
        require(take(loaded.session->main_camera()) != ObjectId{} &&
                    !take(loaded.session->presentation().light_items()).empty(),
                "ProceduralShowcase.scene missing Main Camera or lights.");
        changed(loaded.session->close());
    }

    // 2. GenerativeModelingShowcase.scene
    {
        const auto scene_file = take(project.resolve("Content/Procedural/Scenes/GenerativeModelingShowcase.scene"));
        const auto scene_text = take(assets::read_file(scene_file, 16 * 1024 * 1024));
        auto loaded = editor::load_project_scene(project, scene_text, editor::LoadPolicy::strict);
        if (loaded.error)
            fatal(loaded.error->message);
        require(loaded.session != nullptr && loaded.code != nullptr && loaded.session->load_issues().empty() &&
                    loaded.session->issues().empty(),
                "GenerativeModelingShowcase.scene reported strict load or runtime issues.");

        take(loaded.session->presentation().update());
        const auto gen_slots = loaded.session->presentation().generator_slots();
        require(gen_slots.size() == 3,
                "GenerativeModelingShowcase.scene expected 3 ProceduralGeneratorEntity entries.");
        for (const auto& slot : gen_slots) {
            require(slot.item && slot.item->surface_geometry != nullptr && !slot.item->collections.empty() &&
                        slot.item->surface_materials != nullptr && !slot.item->surface_materials->empty(),
                    "GenerativeModelingShowcase entity missing surface mesh, material, or CopyToPoints instances.");
        }
        require(*gen_slots[1].item->collections.front().transforms !=
                    *gen_slots[2].item->collections.front().transforms,
                "override_seed on North Sanctuary Ruin did not produce distinct rubble placements.");
        require(gen_slots[1].item->surface_geometry->vertices().size() >
                    gen_slots[2].item->surface_geometry->vertices().size(),
                "North Sanctuary Ruin inline overrides (2 floors vs 3 floors) did not isolate runtime graph geometry.");
        {
            const auto hall_params =
                take(loaded.session->generator_promoted_parameters(gen_slots[1].item->component, &project));
            const auto ruin_params =
                take(loaded.session->generator_promoted_parameters(gen_slots[2].item->component, &project));
            const auto hall_floors =
                std::ranges::find(hall_params, std::string_view{"floors"}, &editor::GraphPromotedParameterView::name);
            const auto ruin_floors =
                std::ranges::find(ruin_params, std::string_view{"floors"}, &editor::GraphPromotedParameterView::name);
            require(hall_floors != hall_params.end() && !hall_floors->overridden &&
                        std::get<std::int32_t>(hall_floors->properties.front().value) == 3 &&
                        ruin_floors != ruin_params.end() && ruin_floors->overridden &&
                        std::get<std::int32_t>(ruin_floors->properties.front().value) == 2,
                    "Generator promoted parameter views did not reflect default vs overridden floors.");
        }

        const auto assert_manifold_stages = [&](std::string_view graph_key,
                                                std::initializer_list<std::string_view> stages) {
            (void)take(loaded.session->open_graph(project, graph_key));
            for (const std::string_view stage : stages) {
                const ObjectId node_id = loaded.session->graph_node_id(graph_key, stage);
                require(node_id != ObjectId{}, "Missing expected solid stage in showcase graph.");
                const auto view = take(loaded.session->inspect_graph(graph_key, &project, node_id, false, 4, true));
                require(!view.inspected_error && view.inspected_summary && view.inspected_summary->spatial &&
                            view.inspected_summary->spatial->topology &&
                            view.inspected_summary->spatial->topology->polygon_count > 0 &&
                            view.inspected_summary->spatial->topology->degenerate_primitives == 0 &&
                            view.inspected_summary->spatial->topology->open_boundary_edges == 0 &&
                            view.inspected_summary->spatial->topology->non_manifold_edges == 0,
                        "Showcase solid stage was not a watertight closed manifold.");
            }
        };

        const std::string_view outpost_key = "project/{E4A19C72-8F3B-4D12-9A65-7B2C41F8E301}";
        assert_manifold_stages(outpost_key, {"road_sweep", "wall_sweep", "merlon_sweep"});
        const ObjectId island_ref = loaded.session->graph_node_id(outpost_key, "island_terrace");
        const auto outpost_view = take(
            loaded.session->inspect_graph(outpost_key, &project, {}, false, 4, true, island_ref, "island_terrace"));
        require(!outpost_view.display_error && outpost_view.display_summary.valid &&
                    outpost_view.display_summary.counts[4] >= 6 && outpost_view.display_summary.spatial &&
                    outpost_view.display_summary.spatial->instance_overlaps &&
                    outpost_view.display_summary.spatial->instance_overlaps->overlapping_pairs == 0 &&
                    outpost_view.display_summary.spatial->surface_clearance &&
                    outpost_view.display_summary.spatial->surface_clearance->floating_count == 0,
                "FortifiedOutpost display reported instance overlaps or floating props.");

        const std::string_view ruin_key = "project/{A91C4E85-3D72-49B0-8E24-6C1F5B70D402}";
        assert_manifold_stages(ruin_key, {"facade_extrude", "slab_extrude", "roof_sweep", "structure_shell"});
        const ObjectId slab_ref = loaded.session->graph_node_id(ruin_key, "slab_extrude");
        const auto ruin_view =
            take(loaded.session->inspect_graph(ruin_key, &project, {}, false, 4, true, slab_ref, "slab_extrude"));
        require(!ruin_view.display_error && ruin_view.display_summary.valid &&
                    ruin_view.display_summary.counts[2] > 400 && ruin_view.display_summary.counts[4] >= 2 &&
                    ruin_view.display_summary.spatial && ruin_view.display_summary.spatial->instance_overlaps &&
                    ruin_view.display_summary.spatial->instance_overlaps->overlapping_pairs == 0 &&
                    ruin_view.display_summary.spatial->surface_clearance &&
                    ruin_view.display_summary.spatial->surface_clearance->floating_count == 0,
                "ModularRuinBuilding display reported instance overlaps or floating rubble.");

        auto intact_doc = take(json::Document::parse(R"({"ruin_collapse": 0.0})", json::ParseOptions{64}));
        require(!loaded.session->graph_set_properties(ruin_key, "building_layout", intact_doc.root()),
                "Direct write to promoted target building_layout.ruin_collapse must be rejected.");
        auto zero_val = take(json::Document::parse("0.0", json::ParseOptions{8}));
        take(loaded.session->graph_set_parameter_json(ruin_key, "ruin_collapse", zero_val.root()));
        const auto intact_view = take(loaded.session->inspect_graph(ruin_key, &project, {}, false, 4, false));
        auto heavy_val = take(json::Document::parse("0.78", json::ParseOptions{8}));
        take(loaded.session->graph_set_parameter_json(ruin_key, "ruin_collapse", heavy_val.root()));
        const auto heavy_ruin_view =
            take(loaded.session->inspect_graph(ruin_key, &project, {}, false, 4, true, slab_ref, "slab_extrude"));
        require(!intact_view.display_error && !heavy_ruin_view.display_error &&
                    intact_view.display_summary.counts[2] > heavy_ruin_view.display_summary.counts[2] &&
                    heavy_ruin_view.display_summary.spatial->instance_overlaps->overlapping_pairs == 0,
                "ModularRuinBuilding parametric ruin_collapse slider did not remodel structure cleanly.");
        take(loaded.session->graph_undo(ruin_key, false));
        take(loaded.session->graph_undo(ruin_key, false));
        take(loaded.session->sync_procedural_generators(&project));
        take(loaded.session->presentation().update());

        // 3. RuinedSettlement.procgraph (3-instance AssetNode composition + spatial overlap clearance):
        const std::string_view settlement_key = "project/{D37A5B14-6C8E-4F29-B510-9E4D2A83C604}";
        (void)take(loaded.session->open_graph(project, settlement_key));
        const auto settlement_view = take(loaded.session->inspect_graph(settlement_key, &project, {}, false, 4, true));
        require(!settlement_view.display_error && settlement_view.display_summary.valid &&
                    settlement_view.display_summary.counts[2] > ruin_view.display_summary.counts[2] * 2 &&
                    settlement_view.display_summary.counts[4] >= 6 && settlement_view.display_summary.spatial &&
                    settlement_view.display_summary.spatial->instance_overlaps &&
                    settlement_view.display_summary.spatial->instance_overlaps->overlapping_pairs == 0,
                "RuinedSettlement.procgraph 3-instance AssetNode composition reported missing geometry or overlaps.");

        // 4. Scene ProceduralGeneratorComponent overrides: cancel_edit rollback, roundtrip, duplicate, Undo/Redo & Play:
        {
            const auto cur_slots = loaded.session->presentation().generator_slots();
            const ObjectId hall_comp = cur_slots[1].item->component;
            const ObjectId hall_ent = cur_slots[1].item->entity;
            const std::size_t base_verts = cur_slots[1].item->surface_geometry->vertices().size();
            const std::size_t undo_before = loaded.session->undo_count();
            const bool dirty_before = loaded.session->dirty();
            const Scalar five_floors{std::int32_t{5}};

            // cancel_edit during live slider preview rolls back cooked output without advancing history or dirty state:
            take(loaded.session->begin_graph_parameter_edit({}, {}, "floors", std::nullopt, 887, &project, hall_comp));
            take(loaded.session->preview_graph_parameter_edit(887, {&five_floors, 1}));
            take(loaded.session->presentation().update());
            require(loaded.session->presentation().generator_slots()[1].item->surface_geometry->vertices().size() >
                        base_verts,
                    "Live preview of Scene ProceduralGeneratorComponent 'floors' override did not update geometry.");
            take(loaded.session->cancel_edit());
            take(loaded.session->presentation().update());
            require(loaded.session->presentation().generator_slots()[1].item->surface_geometry->vertices().size() ==
                            base_verts &&
                        loaded.session->undo_count() == undo_before && loaded.session->dirty() == dirty_before,
                    "Cancelling Scene ProceduralGeneratorComponent parameter preview did not roll back cleanly.");

            loaded.session->set_live_forwarding(true);

            auto play_base =
                editor::load_project_scene(project, take(loaded.session->serialize()), editor::LoadPolicy::strict);
            if (play_base.error)
                fatal(play_base.error->message);
            play_base.session->enter_play();

            take(loaded.session->begin_graph_parameter_edit({}, {}, "floors", std::nullopt, 888, &project, hall_comp));
            take(loaded.session->preview_graph_parameter_edit(888, {&five_floors, 1}));
            take(loaded.session->commit_edit(888));
            take(loaded.session->presentation().update());
            const std::size_t five_verts =
                loaded.session->presentation().generator_slots()[1].item->surface_geometry->vertices().size();
            require(
                five_verts > base_verts,
                "Dragging Scene ProceduralGeneratorComponent 'floors' override to 5 did not increase vertex count.");

            // Forward live ops to Play copy:
            const auto ops = take(loaded.session->take_live_ops());
            require(!ops.empty(), "Generator override edit emitted no Play live ops.");
            json::Builder b;
            const auto arr = take(b.array(b.root(), "ops"));
            for (const auto& op : ops) {
                const auto parsed_op = take(assets::json_format::parse(op.text));
                b.append(arr, parsed_op.root());
            }
            const auto batch = json::write(b.root());
            const auto ldiags = take(play_base.session->apply_live({batch.data(), batch.size()}, project));
            require(ldiags.empty(), "Play copy rejected generator override live batch.");
            take(play_base.session->presentation().update());
            require(play_base.session->presentation().generator_slots()[1].item->surface_geometry->vertices().size() ==
                        five_verts,
                    "Play copy did not reproduce 5-story overridden generator geometry.");
            changed(play_base.session->close());

            // Duplicate entity copies component overrides:
            changed(loaded.session->duplicate_entities({&hall_ent, 1}));
            const ObjectId dup_ent = loaded.session->selection().entity;
            const auto dup_parts = take(world::components(loaded.session->world().runtime(), dup_ent));
            const auto dup_ov =
                dup_parts.empty() ? json::Document{} : loaded.session->generator_overrides(dup_parts.front().id());
            require(dup_ov.root().size() > 0,
                    "duplicate_entities did not copy ProceduralGeneratorComponent overrides.");
            changed(loaded.session->undo(&project)); // undo duplicate

            // Undo / Redo of generator parameter override:
            changed(loaded.session->undo(&project));
            take(loaded.session->presentation().update());
            require(loaded.session->presentation().generator_slots()[1].item->surface_geometry->vertices().size() ==
                        base_verts,
                    "Undo of generator parameter override did not restore 3-story geometry.");
            changed(loaded.session->redo(&project));
            take(loaded.session->presentation().update());
            require(loaded.session->presentation().generator_slots()[1].item->surface_geometry->vertices().size() ==
                        five_verts,
                    "Redo of generator parameter override did not restore 5-story geometry.");

            // Scene roundtrip preserves sparse overrides + tolerantly retains unmatched stale overrides:
            auto saved_with_ov = take(loaded.session->serialize());
            const std::string_view needle = "\"floors\": 5";
            const auto at_floors = saved_with_ov.find(needle);
            require(at_floors != std::string::npos, "Serialized scene did not write '\"floors\": 5' override.");
            saved_with_ov.replace(at_floors, needle.size(), "\"floors\": 5, \"legacy_obsolete\": 42");
            auto reloaded_ov = editor::load_project_scene(project, saved_with_ov, editor::LoadPolicy::tolerant);
            if (reloaded_ov.error)
                fatal(reloaded_ov.error->message);
            take(reloaded_ov.session->presentation().update());
            const ObjectId reloaded_comp = reloaded_ov.session->presentation().generator_slots()[1].item->component;
            const auto reloaded_pviews =
                take(reloaded_ov.session->generator_promoted_parameters(reloaded_comp, &project));
            const auto stale_it =
                std::ranges::find_if(reloaded_pviews, [](const editor::GraphPromotedParameterView& p) {
                    return p.name == "legacy_obsolete" && p.undeclared && p.overridden;
                });
            require(
                reloaded_ov.session->presentation().generator_slots()[1].item->surface_geometry->vertices().size() ==
                        five_verts &&
                    stale_it != reloaded_pviews.end() &&
                    take(reloaded_ov.session->serialize()).contains("\"legacy_obsolete\": 42"),
                "Scene roundtrip did not preserve overrides and tolerantly retain unmatched stale override.");
            changed(reloaded_ov.session->close());
        }

        std::printf(
            "  [Showcase Metrics] FortifiedOutpost: %zu pts, %zu prims, %zu instances | ModularRuinBuilding: %zu prims (intact) -> %zu prims (default) -> %zu prims (0.78 collapse), %zu instances\n",
            outpost_view.display_summary.counts[0], outpost_view.display_summary.counts[2],
            outpost_view.display_summary.counts[4], intact_view.display_summary.counts[2],
            ruin_view.display_summary.counts[2], heavy_ruin_view.display_summary.counts[2],
            ruin_view.display_summary.counts[4]);

        changed(loaded.session->close());
    }

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
}
}

int main() {
    atelier::install_fatal_handler();
    verify_roundtrip();
    verify_stop_reentry();
    verify_procedural_generator_scene();
    verify_examples_procedural_showcases();
    std::puts(
        "Project Scene acceptance: PASS (authored scalar roundtrip, End/physics shutdown order, ProceduralGenerator strict schema + Play live, Examples ProceduralShowcase + GenerativeModelingShowcase)");
    return 0;
}
