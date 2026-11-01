#include "editor/session.hpp"
#include "editor/project.hpp"
#include "editor/assets.hpp"
#include "assets/file.hpp"
#include "foundation/frame_timing.hpp"
#include "physics/timing.hpp"
#include "editor/workspace.hpp"
#include "assets/json_format.hpp"
#include "assets/material_format.hpp"
#include "material/resources.hpp"
#include "editor/graph_format.hpp"
#include "procedural/evaluator.hpp"
#include "procedural/nodes.hpp"
#include "geometry/mesh.hpp"
#include "runtime/native.hpp"
#include "application/diagnostics.hpp"
#include "application/scene_snapshot.hpp"
#include "renderer/scene_visibility.hpp"
#include "application/recovery.hpp"
#include "ui/text.hpp"
#include "dungeons_roundtrip.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <numeric>
#include <numbers>
#include <cstdio>
#include <stdexcept>
#include <unordered_set>
#include <windows.h>
#include <psapi.h>

namespace {
using namespace atelier;
template <class T>
T take(Result<T> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
    return std::move(*result);
}
void take(Result<void> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
}
void changed(Result<world::Commit> result) {
    const auto commit = take(std::move(result));
    if (commit.notification_error)
        throw std::runtime_error(commit.notification_error->message);
}
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
material::ProgramSurfaceRef snapshot_surface(const application::SceneSnapshot& snapshot, std::size_t mesh = 0,
                                             std::size_t slot = 0) {
    return snapshot.picking()->materials.at(snapshot.meshes.at(mesh).material_slots->at(slot));
}
void collect(editor::Session& document) {
    for (unsigned step = 0; step < 96; ++step)
        take(document.collect_step());
}
void collect(Runtime& runtime) {
    if (runtime.collection_phase() == CollectionPhase::idle)
        take(runtime.begin_collection());
    while (runtime.collection_phase() != CollectionPhase::idle)
        take(runtime.collect_step({1024}));
}
ObjectId component(editor::Session& document, ObjectId entity, TypeId type) {
    for (const auto& root : take(world::components(document.world().runtime(), entity)))
        if (take(document.world().runtime().type_of(root.id())) == type)
            return root.id();
    throw std::runtime_error("Expected component is absent.");
}
float value(editor::Session& document, ObjectId target, std::size_t axis = 0) {
    return std::get<float>(take(document.world().read(target, document.parameters()[axis].path)));
}
#include "project_workspace.inl"
#include "project_document.inl"
#include "textured_obj.inl"
#include "gltf_import.inl"
#include "physics_examples.inl"
#include "examples.inl"
#include "dungeons.inl"
#include "project_profile.inl"
#include "declarative_ui.inl"
#include "tooltip_dsl.inl"
#include "property_tooltips.inl"
#include "object_inspection.inl"
#include "ui_actions.inl"
#include "table_dsl.inl"
#include "composed_ui.inl"
#include "ui_reload_transaction.inl"
#include "ui_source_patch.inl"
#include "ui_reload_profile.inl"
#include "source_editor.inl"
#include "output.inl"
#include "viewport_edit.inl"
#include "entity_composition.inl"
#include "scene_lit.inl"
#include "material_resources.inl"
#include "color_edits.inl"
#include "recovery.inl"
#include "operator_parameters.inl"
#include "procedural_graph.inl"
}
int main(int argc, char** argv) {
    std::string_view stage = argc > 1 ? argv[1] : "document history";
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--recovery") {
            verify_scene_recovery();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--materials") {
            verify_material_resources();
            verify_missing_resources();
            verify_color_edits();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--dungeons") {
            verify_dungeons();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--procedural-graph") {
            verify_procedural_graph();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--examples") {
            verify_examples();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--physics-examples") {
            verify_physics_examples();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--textured-obj") {
            verify_textured_obj();
            verify_sponza_project();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--gltf") {
            verify_gltf_import();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--viewport-edit") {
            verify_viewport_editing();
            verify_entity_composition();
            return 0;
        }
        if ((argc == 2 || argc == 3) && std::string_view(argv[1]) == "--scene-lit") {
            verify_scene_lit_application(argc == 3 ? std::filesystem::path{argv[2]} : std::filesystem::path{});
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--profile-json") {
            profile_project_json(argv[2]);
            return 0;
        }
        if ((argc == 5 || argc == 6) && std::string_view(argv[1]) == "--profile-scene-cpu") {
            profile_scene_cpu(argv[2], argv[3], argv[4],
                              argc == 6 ? std::filesystem::path(argv[5]) : std::filesystem::path{});
            return 0;
        }
        if (argc == 5 && std::string_view(argv[1]) == "--profile-physics-cpu") {
            profile_physics_cpu(argv[2], argv[3], argv[4]);
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--profile-ui-reload") {
            profile_ui_reload(argv[2]);
            return 0;
        }
        // Each check runs once in the acceptance gate: a mode run by its own target is not repeated here.
        require(
            argc == 1,
            "Usage: atelier_editor_document_acceptance [--recovery | --materials | --dungeons | --procedural-graph | --examples | --physics-examples | --textured-obj | --gltf | --viewport-edit | --scene-lit [source-project] | --profile-json output-directory | --profile-scene-cpu project scene output.json [camera-report.json] | --profile-physics-cpu project scene output.json | --profile-ui-reload output.csv]");
        stage = "project document";
        verify_project_document();
        stage = "declarative parameters";
        verify_declarative_parameters();
        stage = "tooltip DSL";
        verify_tooltip_dsl();
        stage = "property tooltips";
        verify_property_tooltips();
        stage = "object inspection";
        verify_object_inspection();
        stage = "UI actions";
        verify_ui_actions();
        stage = "table DSL";
        verify_table_dsl();
        stage = "table views";
        verify_table_views();
        stage = "composed UI";
        verify_composed_ui();
        stage = "UI reload transaction";
        verify_ui_reload_transaction();
        stage = "UI source patch";
        verify_ui_source_patch();
        stage = "UI code reuse";
        verify_ui_code_reuse();
        stage = "source editor";
        verify_source_editor();
        stage = "output projection";
        verify_output_projection();
        stage = "operator parameters";
        verify_operator_parameters();
        stage = "document history";
        {
            editor::Session single;
            changed(single.create_static_mesh(assets::mesh_assets().front()));
            require(single.undo_count() == 1 && take(single.world().runtime().type_of(single.selection().entity)) ==
                                                    single.presentation_types().static_mesh_entity,
                    "Static mesh factory did not enter document history as one typed Entity creation.");
            const auto entity = single.selection().entity;
            auto& rt = single.world().runtime();
            const auto mesh = component(single, entity, single.presentation_types().static_mesh_component);
            require(take(world::components(rt, entity)).size() == 1 &&
                        take(rt.type_of(mesh)) == single.presentation_types().static_mesh_component,
                    "A mesh Entity acquired a redundant Transform component.");
            changed(single.undo());
            collect(single);
            require(take(single.world().members()).empty() && take(single.presentation().render_items()).empty(),
                    "Creation undo retained the Entity or its rendering.");
            changed(single.redo());
            require(single.selection().entity == entity &&
                        component(single, entity, single.presentation_types().static_mesh_component) == mesh &&
                        take(single.presentation().render_items()).front().geometry ==
                            assets::mesh_assets().front().mesh,
                    "Creation redo changed Entity/component identity or mesh resource.");
            take(single.begin_edit(entity, single.types().position_fields[0], 91));
            take(single.preview_edit(91, 4));
            take(single.commit_edit(91));
            require(take(single.presentation().render_items()).front().world[12] == 4,
                    "Inherited root field did not move the mesh.");
            take(single.begin_edit(entity, single.types().rotation_fields[2], 92));
            take(single.preview_edit(92, .7f));
            take(single.commit_edit(92));
            take(single.begin_edit(entity, single.types().scale_fields[0], 93));
            take(single.preview_edit(93, 2.f));
            take(single.commit_edit(93));
            const auto before_reset = take(world::read_transform(rt, entity));
            const auto reset_history = single.undo_count();
            take(single.reset_position(entity));
            const auto after_reset = take(world::read_transform(rt, entity));
            require(after_reset.position == math::Vector3{} && after_reset.rotation == before_reset.rotation &&
                        after_reset.scale == before_reset.scale && single.undo_count() == reset_history + 1,
                    "Shared tuple reset failed to preserve the root mesh's rotation/scale or one-entry history.");
            take(single.reset_position(entity));
            require(single.undo_count() == reset_history + 1, "Unchanged tuple reset created history.");
            changed(single.undo());
            require(take(world::read_transform(rt, entity)) == before_reset &&
                        component(single, entity, single.presentation_types().static_mesh_component) == mesh,
                    "Tuple undo replaced root identity or failed to restore the complete Entity TRS.");
            changed(single.redo());
            changed(single.undo());
            changed(single.remove_component(mesh));
            require(take(world::has_transform(rt, entity)) && take(world::read_transform(rt, entity)) == before_reset &&
                        take(single.presentation().render_items()).empty(),
                    "Removing the mesh lost Entity space or retained presentation state.");
            changed(single.undo());
            require(component(single, entity, single.presentation_types().static_mesh_component) == mesh &&
                        take(single.presentation().render_items()).front().world[12] == 4,
                    "Root mesh undo did not restore explicit root identity and inherited fields.");
            changed(single.close());
        }
        editor::Session document;
        changed(document.add_entity());
        const auto entity = document.selection().entity;
        auto& runtime = document.world().runtime();
        require(take(document.world().members()).size() == 1 && take(world::components(runtime, entity)).empty() &&
                    take(document.presentation().render_items()).empty(),
                "An empty Entity acquired implicit components or rendering.");
        const auto state = take(world::inspect_entity(runtime, entity));
        require(state.state == world::EntityState::attached_inactive && state.can_edit &&
                    take(document.world().members(document.presentation().meshes())).empty() &&
                    take(document.world().members(document.presentation().lights())).empty(),
                "Empty Entity state confused editability with render registrations.");
        const std::wstring original_name(document.name(entity));
        const auto before_rename = document.undo_count();
        take(document.rename(entity, L"主角 Hero"));
        require(document.name(entity) == L"主角 Hero" && document.undo_count() == before_rename + 1,
                "Document name did not create one history entry.");
        take(document.rename(entity, L"主角 Hero"));
        require(document.undo_count() == before_rename + 1 && !document.rename(entity, L" ") &&
                    document.name(entity) == L"主角 Hero",
                "Unchanged or invalid document names modified history.");
        changed(document.undo());
        require(document.name(entity) == original_name, "Name Undo did not restore the label.");
        changed(document.redo());
        require(document.name(entity) == L"主角 Hero", "Name Redo did not restore the label.");
        changed(document.add_component(document.presentation_types().static_mesh_component));
        const auto transform = entity;
        const auto cube = component(document, entity, document.presentation_types().static_mesh_component);
        take(presentation::set_geometry(runtime, cube, assets::mesh_assets().front().mesh));
        require(take(document.presentation().render_items()).size() == 1 &&
                    take(document.world().members(document.presentation().meshes())).size() == 1,
                "AttachedInactive mesh component did not create a value projection.");
        changed(document.remove_component(cube));
        require(take(document.world().members()).size() == 1 && take(document.presentation().render_items()).empty(),
                "Removing mesh removed its Entity or retained rendering.");
        collect(document);
        changed(document.undo());
        require(component(document, entity, document.presentation_types().static_mesh_component) == cube &&
                    take(world::inspect_component(runtime, cube)).owner == entity,
                "Component undo replaced identity or owner.");
        changed(document.redo());
        changed(document.undo());
        const auto history = document.undo_count();
        const auto path = document.parameters()[0].path;
        take(document.begin_edit(transform, path, 11));
        for (unsigned step = 0; step < 64; ++step)
            take(document.preview_edit(11, static_cast<float>(step)));
        require(!document.preview_edit(12, 99.f) && value(document, transform) == 63.f,
                "A different control session modified the active target.");
        take(document.commit_edit(11));
        require(document.undo_count() == history + 1, "Continuous field editing produced more than one history entry.");
        changed(document.undo());
        require(value(document, transform) == 0, "Field undo did not restore its before value.");
        changed(document.redo());
        take(document.begin_edit(transform, document.parameters()[1].path, 13));
        take(document.preview_edit(13, 7));
        take(document.cancel_edit());
        require(value(document, transform, 1) == 0 && document.undo_count() == history + 1,
                "Field cancellation changed history or the committed value.");
        changed(document.add_entity());
        const auto other = document.selection().entity;
        take(document.begin_edit(transform, path, 14));
        take(document.preview_edit(14, 34));
        take(document.commit_edit(14));
        changed(document.undo());
        require(document.selection().entity == entity && value(document, transform) == 63,
                "Property undo failed to select its recorded owner or wrote another component.");
        changed(document.redo());
        const auto original_transform = take(world::read_transform(runtime, transform));
        take(document.begin_edit(transform, document.types().rotation_fields[2], 17));
        take(document.preview_edit(17, .7f));
        take(document.commit_edit(17));
        take(document.begin_edit(transform, document.types().scale_fields[0], 18));
        take(document.preview_edit(18, 2.f));
        take(document.commit_edit(18));
        require(take(world::read_transform(runtime, transform)) != original_transform,
                "Transform field transactions did not update rotation and scale.");
        changed(document.undo());
        changed(document.undo());
        require(take(world::read_transform(runtime, transform)) == original_transform,
                "TRS undo did not restore the complete value after quaternion normalization.");
        take(document.select(entity));
        changed(document.erase_selected());
        require(take(document.world().members()).size() == 1 && take(document.presentation().render_items()).empty() &&
                    !document.begin_edit(transform, path, 15) && !document.rename(entity, L"Detached"),
                "A detached Entity remained editable or presentable in the World.");
        collect(document);
        changed(document.undo());
        require(document.selection().entity == entity && take(document.world().members()).front().id() == entity &&
                    take(world::has_transform(runtime, entity)) &&
                    component(document, entity, document.presentation_types().static_mesh_component) == cube &&
                    value(document, transform) == 34 && document.name(entity) == L"主角 Hero",
                "Entity delete/GC/undo did not restore identities, composition, order, names and values.");
        take(document.begin_edit(transform, path, 16));
        take(document.preview_edit(16, 88));
        changed(document.erase_selected());
        changed(document.undo());
        require(!document.editing() && value(document, transform) == 34,
                "Structural removal failed to cancel the active field before changing membership.");
        auto retained = take(runtime.hold(entity));
        changed(document.erase_selected());
        for (unsigned step = 0; step < 66; ++step) {
            changed(document.add_entity());
            changed(document.erase_selected());
        }
        collect(document);
        require(take(world::inspect_entity(runtime, entity)).state == world::EntityState::detached &&
                    take(world::components(runtime, entity)).size() == 1 &&
                    take(world::read_transform(runtime, transform)).position.x == 34,
                "History retirement destroyed an Entity still held by an ordinary root or lost its composition.");
        retained.reset();
        collect(document);
        require(take(world::inspect_entity(runtime, entity)).state == world::EntityState::invalid &&
                    document.name(entity).empty(),
                "Retired document history retained an unreferenced detached Entity.");
        auto survivor = take(runtime.hold(other));
        changed(document.close());
        require(survivor.registered() &&
                    take(world::inspect_entity(runtime, other)).state == world::EntityState::detached &&
                    document.undo_count() == 0 && document.redo_count() == 0,
                "Document close did not release participation/history or destroyed a retained Entity.");
        std::puts(
            "Editor document acceptance: PASS (Entity/component identity, name/field history, membership, retirement, close)");
        return 0;
    } catch (const ui::detail::TextFailure& error) {
        std::fprintf(stderr, "Editor document acceptance: FAIL [%.*s]: %s (HRESULT 0x%08lx)\n",
                     static_cast<int>(stage.size()), stage.data(), error.what(),
                     static_cast<unsigned long>(error.code));
        return 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Editor document acceptance: FAIL [%.*s]: %s\n", static_cast<int>(stage.size()),
                     stage.data(), error.what());
        return 1;
    }
}
