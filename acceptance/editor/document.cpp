#include "editor/session.hpp"
#include "editor/project.hpp"
#include "editor/examples.hpp"
#include "editor/workspace.hpp"
#include "editor/json_format.hpp"
#include "application/scene_snapshot.hpp"
#include "dungeons_roundtrip.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <cstdio>
#include <stdexcept>
#include <unordered_set>
#include <windows.h>
#include <psapi.h>

namespace {
using namespace atelier;
template<class T> T take(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(*result);
}
void take(Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }
void changed(Result<world::Commit> result) {
    const auto commit = take(std::move(result));
    if (commit.notification_error) throw std::runtime_error(commit.notification_error->message);
}
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void collect(editor::Session& document) { for (unsigned step = 0; step < 96; ++step) take(document.collect_step()); }
void collect(Runtime& runtime) {
    if (runtime.collection_phase()==CollectionPhase::idle) take(runtime.begin_collection());
    while (runtime.collection_phase()!=CollectionPhase::idle) take(runtime.collect_step({1024}));
}
ObjectId component(editor::Session& document, ObjectId actor, TypeId type) {
    for (const auto& root : take(world::components(document.world().runtime(), actor)))
        if (take(document.world().runtime().type_of(root.id())) == type) return root.id();
    throw std::runtime_error("Expected component is absent.");
}
float value(editor::Session& document, ObjectId target, std::size_t axis = 0) {
    return std::get<float>(take(document.world().read(target, document.parameters()[axis].path)));
}
#include "project_workspace.inl"
#include "project_document.inl"
#include "textured_obj.inl"
#include "physics_examples.inl"
#include "examples.inl"
#include "dungeons.inl"
#include "project_profile.inl"
#include "declarative_ui.inl"
#include "object_inspection.inl"
#include "ui_actions.inl"
#include "table_dsl.inl"
#include "composed_ui.inl"
#include "ui_reload_transaction.inl"
#include "ui_source_patch.inl"
#include "ui_reload_profile.inl"
#include "source_editor.inl"
#include "viewport_edit.inl"
#include "scene_lit.inl"
#include "material_resources.inl"
}
int main(int argc, char** argv) {
    try {
        if(argc==2 && std::string_view(argv[1])=="--materials") {verify_material_resources();return 0;}
        if(argc==2 && std::string_view(argv[1])=="--dungeons") {verify_dungeons();return 0;}
        if(argc==2 && std::string_view(argv[1])=="--examples") {verify_official_examples();return 0;}
        if(argc==2 && std::string_view(argv[1])=="--physics-examples") {verify_physics_examples();return 0;}
        if(argc==2 && std::string_view(argv[1])=="--textured-obj") {verify_textured_obj();verify_sponza_project();return 0;}
        if(argc==2 && std::string_view(argv[1])=="--viewport-edit") {verify_viewport_editing();return 0;}
        if((argc==2 || argc==3) && std::string_view(argv[1])=="--scene-lit") {
            verify_scene_lit_application(argc==3?std::filesystem::path{argv[2]}:std::filesystem::path{});return 0;
        }
        if (argc==2 && std::string_view(argv[1])=="--source-editor") {verify_source_editor();return 0;}
        if (argc == 3 && std::string_view(argv[1]) == "--profile-json") {
            profile_project_json(argv[2]);
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--profile-ui-reload") {
            profile_ui_reload(argv[2]);
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--physics-test-project") {
            verify_physics_test_project();
            std::puts("Editor physics project acceptance: PASS");
            return 0;
        }
        require(argc == 1, "Usage: atelier_editor_document_acceptance [--examples | --physics-examples | --physics-test-project | --viewport-edit | --scene-lit [source-project] | --source-editor | --profile-json output-directory | --profile-ui-reload output.csv]");
        verify_physics_test_project();
        verify_project_document();
        verify_textured_obj();
        verify_declarative_parameters();
        verify_object_inspection();
        verify_viewport_editing();
        verify_scene_lit_application();
        verify_ui_actions();
    verify_table_dsl();
    verify_table_views();
        verify_composed_ui();
        verify_ui_reload_transaction();
        verify_ui_source_patch();
        verify_ui_code_reuse();
        verify_source_editor();
        {
            editor::Session single;
            changed(single.create_static_mesh(editor::mesh_assets().front()));
            require(single.undo_count()==1 && take(single.world().runtime().type_of(single.selection().actor))==single.types().static_mesh_actor,
                "Static mesh factory did not enter document history as one typed Actor creation.");
            const auto actor = single.selection().actor;
            auto& rt = single.world().runtime();
            const auto mesh = *take(world::root_component(rt, actor));
            require(take(world::components(rt, actor)).size() == 1 && take(rt.type_of(mesh)) == single.types().static_mesh_component,
                "A mesh Actor acquired a redundant standalone transform component.");
            changed(single.undo()); collect(single);
            require(take(single.world().members()).empty() && take(single.world().render_items()).empty(),
                "Creation undo retained the Actor or its rendering.");
            changed(single.redo());
            require(single.selection().actor==actor && take(world::root_component(rt,actor))==mesh &&
                take(single.world().render_items()).front().geometry==editor::mesh_assets().front().mesh,
                "Creation redo changed Actor/component identity or mesh resource.");
            take(single.begin_edit(mesh, single.types().position_fields[0], 91));
            take(single.preview_edit(91, 4)); take(single.commit_edit(91));
            require(take(single.world().render_items()).front().world[12] == 4, "Inherited root field did not move the mesh.");
            take(single.begin_edit(mesh, single.types().rotation_fields[2], 92));
            take(single.preview_edit(92, .7f)); take(single.commit_edit(92));
            take(single.begin_edit(mesh, single.types().scale_fields[0], 93));
            take(single.preview_edit(93, 2.f)); take(single.commit_edit(93));
            const auto before_reset = take(world::read_transform(rt, mesh));
            const auto reset_history = single.undo_count();
            take(single.reset_position(mesh));
            const auto after_reset = take(world::read_transform(rt, mesh));
            require(after_reset.position == math::Vector3{} && after_reset.rotation == before_reset.rotation &&
                after_reset.scale == before_reset.scale && single.undo_count() == reset_history + 1,
                "Shared tuple reset failed to preserve the root mesh's rotation/scale or one-entry history.");
            take(single.reset_position(mesh));
            require(single.undo_count() == reset_history + 1, "Unchanged tuple reset created history.");
            changed(single.undo());
            require(take(world::read_transform(rt, mesh)) == before_reset && take(world::root_component(rt, actor)) == mesh,
                "Tuple undo replaced root identity or failed to restore the complete SceneComponent TRS.");
            changed(single.redo()); changed(single.undo());
            changed(single.remove_component(mesh));
            require(!take(world::root_component(rt, actor)) && take(single.world().render_items()).empty(), "Root removal retained spatial/presentation state.");
            changed(single.undo());
            require(take(world::root_component(rt, actor)) == mesh && take(single.world().render_items()).front().world[12] == 4,
                "Root mesh undo did not restore explicit root identity and inherited fields.");
            changed(single.close());
        }
        editor::Session document;
        changed(document.add_actor());
        const auto actor = document.selection().actor;
        auto& runtime = document.world().runtime();
        require(take(document.world().members()).size() == 1 && take(world::components(runtime, actor)).empty() &&
            take(document.world().render_items()).empty(), "An empty Actor acquired implicit components or rendering.");
        const auto state = take(world::inspect_actor(runtime, actor));
        require(state.state == world::ActorState::attached_inactive && state.can_edit && !state.can_present,
            "Empty Actor state confused editability with render registrations.");
        const std::wstring original_name(document.name(actor));
        const auto before_rename=document.undo_count();
        take(document.rename(actor,L"主角 Hero"));
        require(document.name(actor)==L"主角 Hero" && document.undo_count()==before_rename+1,"Document name did not create one history entry.");
        take(document.rename(actor,L"主角 Hero"));
        require(document.undo_count()==before_rename+1 && !document.rename(actor,L" ") && document.name(actor)==L"主角 Hero",
            "Unchanged or invalid document names modified history.");
        changed(document.undo());
        require(document.name(actor)==original_name,"Name Undo did not restore the label.");
        changed(document.redo());
        require(document.name(actor)==L"主角 Hero","Name Redo did not restore the label.");
        changed(document.add_component(document.types().scene_component));
        changed(document.add_component(document.types().static_mesh_component));
        const auto transform = component(document, actor, document.types().scene_component);
        const auto cube = component(document, actor, document.types().static_mesh_component);
        take(world::set_geometry(runtime,cube,editor::mesh_assets().front().mesh));
        require(take(document.world().render_items()).size() == 1 && take(world::inspect_actor(runtime, actor)).can_present,
            "AttachedInactive mesh component did not create a value projection.");
        changed(document.remove_component(cube));
        require(take(document.world().members()).size() == 1 && take(document.world().render_items()).empty(),
            "Removing mesh removed its Actor or retained rendering.");
        collect(document);
        changed(document.undo());
        require(component(document, actor, document.types().static_mesh_component) == cube &&
            take(world::inspect_component(runtime, cube)).owner == actor, "Component undo replaced identity or owner.");
        changed(document.redo()); changed(document.undo());
        const auto history = document.undo_count();
        const auto path = document.parameters()[0].path;
        take(document.begin_edit(transform, path, 11));
        for (unsigned step = 0; step < 64; ++step) take(document.preview_edit(11, static_cast<float>(step)));
        require(!document.preview_edit(12, 99.f) && value(document, transform) == 63.f,
            "A different control session modified the active target.");
        take(document.commit_edit(11));
        require(document.undo_count() == history + 1, "Continuous field editing produced more than one history entry.");
        changed(document.undo()); require(value(document, transform) == 0, "Field undo did not restore its before value.");
        changed(document.redo());
        take(document.begin_edit(transform, document.parameters()[1].path, 13));
        take(document.preview_edit(13, 7)); take(document.cancel_edit());
        require(value(document, transform, 1) == 0 && document.undo_count() == history + 1,
            "Field cancellation changed history or the committed value.");
        changed(document.add_actor());
        const auto other = document.selection().actor;
        take(document.begin_edit(transform, path, 14));
        take(document.preview_edit(14, 34)); take(document.commit_edit(14));
        changed(document.undo());
        require(document.selection().actor == actor && value(document, transform) == 63,
            "Property undo failed to select its recorded owner or wrote another component.");
        changed(document.redo());
        const auto original_transform = take(world::read_transform(runtime, transform));
        take(document.begin_edit(transform, document.types().rotation_fields[2], 17));
        take(document.preview_edit(17, .7f)); take(document.commit_edit(17));
        take(document.begin_edit(transform, document.types().scale_fields[0], 18));
        take(document.preview_edit(18, 2.f)); take(document.commit_edit(18));
        require(take(world::read_transform(runtime, transform)) != original_transform,
            "Transform field transactions did not update rotation and scale.");
        changed(document.undo()); changed(document.undo());
        require(take(world::read_transform(runtime, transform)) == original_transform,
            "TRS undo did not restore the complete value after quaternion normalization.");
        take(document.select(actor));
        changed(document.erase_selected());
        require(take(document.world().members()).size() == 1 && take(document.world().render_items()).empty() &&
            !document.begin_edit(transform, path, 15) && !document.rename(actor,L"Detached"), "A detached Actor remained editable or presentable in the World.");
        collect(document);
        changed(document.undo());
        require(document.selection().actor == actor && take(document.world().members()).front().id() == actor &&
            component(document, actor, document.types().scene_component) == transform && component(document, actor, document.types().static_mesh_component) == cube &&
            value(document, transform) == 34 && document.name(actor)==L"主角 Hero", "Actor delete/GC/undo did not restore identities, composition, order, names and values.");
        take(document.begin_edit(transform, path, 16)); take(document.preview_edit(16, 88));
        changed(document.erase_selected()); changed(document.undo());
        require(!document.editing() && value(document, transform) == 34,
            "Structural removal failed to cancel the active field before changing membership.");
        auto retained = take(runtime.hold(actor));
        changed(document.erase_selected());
        for (unsigned step = 0; step < 66; ++step) { changed(document.add_actor()); changed(document.erase_selected()); }
        collect(document);
        require(take(world::inspect_actor(runtime, actor)).state == world::ActorState::detached &&
            take(world::components(runtime, actor)).size() == 2 && take(world::read_transform(runtime, transform)).position.x == 34,
            "History retirement destroyed an Actor still held by an ordinary root or lost its composition.");
        retained.reset(); collect(document);
        require(take(world::inspect_actor(runtime, actor)).state == world::ActorState::invalid && document.name(actor).empty(),
            "Retired document history retained an unreferenced detached Actor.");
        auto survivor = take(runtime.hold(other));
        changed(document.close());
        require(survivor.registered() && take(world::inspect_actor(runtime, other)).state == world::ActorState::detached &&
            document.undo_count() == 0 && document.redo_count() == 0,
            "Document close did not release participation/history or destroyed a retained Actor.");
        std::puts("Editor document acceptance: PASS (Actor/component identity, name/field history, membership, retirement, close)");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Editor document acceptance: FAIL: %s\n", error.what());
        return 1;
    }
}
