#include "../helpers.hpp"
#include "assets/file.hpp"
#include "editor/project_code.hpp"
#include "editor/workspace.hpp"
#include "application/play_process.hpp"
#include "application/presentation.hpp"
#include "application/timing.hpp"
#include "logging/logging.hpp"
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cstdio>

namespace {
using namespace atelier;

using namespace atelier::acceptance::helpers;

void complete(Result<world::Commit> result) {
    const auto commit = take(std::move(result));
    if (commit.notification_error)
        fatal(commit.notification_error->message);
}

void run(const std::filesystem::path& root) {
    auto project = take(editor::Project::create(root));
    const auto component = root / "Source/Move.source";
    take(editor::write_file(
        root / "Source/Math.source",
        "float travel(float speed,float dt) { return speed * dt * (2.0f * procedural.unit(2147483648)); }"));
    take(editor::write_file(component, "class project.Move : world.Component { float speed=2.0f; void tick(float dt) {"
                                       "self.translate_owner(travel(self.speed,dt),0.0f,0.0f); } }"));
    auto loaded = editor::load_project_scene(project, take(assets::read_file(root / "Content/Scenes/Main.scene")));
    require(loaded.session != nullptr, loaded.error ? loaded.error->message : "Project Source failed to load.");
    auto& document = *loaded.session;
    complete(document.add_entity());
    const auto entity = document.selection().entity;
    take(document.set_transform(entity, {}));
    complete(document.add_component(entity, take(document.world().runtime().find_type("project.Move"))));
    const auto snapshot = root / "Cache/run.scene";
    take(editor::write_file(snapshot, take(document.serialize())));
    complete(document.start());
    take(document.update(.5f));
    // procedural.unit(2^31)=0.5, so speed 2 * dt 0.5 gives one unit of travel.
    require(take(world::read_transform(document.world().runtime(), entity)).position.x == 1.f,
            "Project Source tick did not produce x=1.");
    complete(document.stop());
    const auto wait_exit = [](application::PlayProcess& child) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
        while (child.running() && std::chrono::steady_clock::now() < deadline) {
            child.poll();
            Sleep(10);
        }
        require(!child.running(), "Play child did not exit.");
    };

    // The real command inherits the link and stop events. Read its final authoring state back from disk.
    const auto report = root / "Cache/reported.scene";
    {
        application::PlayProcess child;
        std::unique_ptr<application::LiveLink> pipe;
        take(child.start(root, snapshot, true, 0, report, false, false, &pipe));
        application::PlayLink link;
        link.open(std::move(pipe), root / "Cache/play-changes");
        document.set_live_forwarding(true);
        take(document.rename(entity, L"Renamed in Play"));
        const auto live_ops = take(document.take_live_ops());
        link.forward(live_ops);
        require(link.unacknowledged() == 1, "The editor did not queue the scene rename.");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
        while (link.unacknowledged() && link.divergence().empty() && child.running() &&
               std::chrono::steady_clock::now() < deadline) {
            child.poll();
            link.poll(child.running(), child.stopping());
            Sleep(10);
        }
        require(child.running() && link.unacknowledged() == 0 && link.divergence().empty(),
                "The child did not acknowledge the scene rename.");
        child.stop();
        wait_exit(child);
        require(child.exit_code() == 0u && !child.forced(), "Live Play did not stop cleanly.");
        auto restored = editor::load_project_scene(project, take(assets::read_file(report)));
        const auto entity_id = document.document_identity(entity);
        require(restored.session && entity_id &&
                    restored.session->name(take(restored.session->local_object(*entity_id))) == L"Renamed in Play",
                "The child report lost the acknowledged rename or its identity.");
    }
    complete(document.close());

    // Use the same rejected Source build as the authority for the child process diagnostic.
    take(editor::write_file(component,
                            "class project.Move : world.Component { void tick(float dt) { missing_symbol(); } }"));
    const auto invalid = editor::load_project_scene(project, take(assets::read_file(snapshot)));
    require(!invalid.session && invalid.error && invalid.error->source, "Expected a located Source build error.");
    {
        application::PlayProcess child;
        take(child.start(root, snapshot, true, 1));
        wait_exit(child);
        require(child.exit_code() == 1u, "A failed scene build did not reject the Play run.");
        editor::Output output;
        const auto refused = output.add_run(snapshot);
        require(!refused && refused.error().code == logging::ErrorCode::invalid_options &&
                    refused.error().message == "Reader input is not a directory",
                "Attaching a scene file lost the log reader's first error.");
        auto* stream = take(output.add_run(child.log_directory()));
        require(stream == take(output.add_run(child.log_directory())), "The Play run acquired a second stream.");
        output.source_ended(*stream);
        while (!output.source_status(child.log_directory())->finished)
            output.poll();
        const auto failure = std::ranges::find_if(
            output.records(), [](const auto& item) { return item.record.entry.event_code == "runner.failed"; });
        require(failure != output.records().end(), "A rejected scene build produced no runner diagnostic.");
        const auto& entry = failure->record.entry;
        const auto code = std::ranges::find(entry.fields, "error_code", &logging::Field::key);
        const auto& source = *invalid.error->source;
        require(code != entry.fields.end() &&
                    code->value == logging::FieldValue{static_cast<std::uint64_t>(invalid.error->code)} &&
                    entry.message == invalid.error->message && entry.source.file == source.path &&
                    entry.source.version == source.version && entry.source.offset == source.offset &&
                    entry.source.line == source.line && entry.source.column == source.column &&
                    entry.source.disk_path == source.disk_path,
                "The child lost the build error code, message or original Source location.");
    }

    // The independent child uses the saved scene, but compiles a deliberately stuck tick.
    take(editor::write_file(
        component,
        "class project.Move : world.Component { float speed=2.0f; void tick(float dt) { while (true) {} } }"));
    application::PlayProcess process;
    take(process.start(root, snapshot, true));
    auto reader = take(logging::Reader::open(process.log_directory()));
    bool started{};
    const auto startup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
    while (process.running() && std::chrono::steady_clock::now() < startup_deadline && !started) {
        process.poll();
        for (const auto& record : reader->poll().records)
            started |= record.entry.event_code == "runner.started";
        Sleep(10);
    }
    require(started, "Stuck Source child did not start.");
    Sleep(100);
    process.stop();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (process.running() && std::chrono::steady_clock::now() < deadline) {
        process.poll();
        Sleep(10);
    }
    require(!process.running() && process.forced(), "Stuck Source child did not terminate within five seconds.");
}

void verify_network_control_reuse(const std::filesystem::path& root) {
    auto project = take(editor::Project::create(root));
    const auto graph_key = take(project.create_procedural_graph(
        L"Content/Graphs/ReuseCheck.procgraph",
        R"({"version":4,"settings":{"display":"xform"},"nodes":{)"
        R"("grid":{"type":"procedural.Grid","position":[60,40],"properties":{"columns":4,"rows":4}},)"
        R"("xform":{"type":"procedural.Transform","position":[60,150],"inputs":{"input":"grid.output"}}}})"));
    editor::Session doc;
    take(doc.initialize({}, editor::SessionRole::editor));
    take(doc.open_graph(project, graph_key));

    editor::Workspace workspace;
    take(workspace.initialize());
    workspace.project_open = true;
    workspace.active_graph = graph_key;
    workspace.set_single_graph_selection("xform");
    workspace.dock.select(editor::network_control::tab);
    workspace.dock.select(editor::control::scene_tab);

    ui::Session ui_session;
    const auto take_ui = []<class T>(ui::Result<T> value) -> T {
        if (!value)
            fatal("UI acceptance error: " + std::to_string(static_cast<int>(value.error().code)));
        if constexpr (!std::is_void_v<T>)
            return std::move(*value);
    };
    const auto fonts = take(application::editor_fonts());
    take_ui(ui_session.initialize(fonts));
    take_ui(ui_session.set_style_sheet(take(application::editor_styles())));
    take_ui(ui_session.set_surface({{1400, 900}, 1.f, 1}));

    ui::ControlList list;
    const auto present_once = [&]() {
        take(workspace.describe(list, doc, {1400, 900}, ui_session.presented().get(), &project, &ui_session, nullptr));
        take_ui(ui_session.set_controls(list));
        auto frame = take_ui(ui_session.prepare());
        require(ui_session.accept(frame), "Workspace ControlList frame was rejected.");
        take(workspace.commit_ui_controls());
    };
    // Frame 1 establishes layout bounds; Frame 2 settles canvas_dip from the accepted frame.
    present_once();
    present_once();
    const auto settled_idle_rev = workspace.network.idle_revision();
    require(settled_idle_rev != 0, "Settled NetworkCanvas did not publish a non-zero region revision.");

    // Frame 3: completely settled workspace with visible Network tab must submit zero changed controls.
    const auto stats_before = ui_session.statistics();
    present_once();
    const auto stats_settled = ui_session.statistics();
    require(workspace.network.idle_revision() == settled_idle_rev &&
                stats_settled.control_batches == stats_before.control_batches &&
                stats_settled.controls_updated == stats_before.controls_updated,
            "Settled NetworkPanel + NetworkCanvas still dirtied UI controls on repeat submission.");

    // Frame 4: mutating unrelated SceneViewport state keeps NetworkCanvas's region revision intact.
    workspace.show_fps = true;
    workspace.bump_view_revision();
    present_once();
    require(workspace.network.idle_revision() == settled_idle_rev,
            "Unrelated viewport toggle invalidated settled NetworkCanvas projection.");

    // Frame 5: mutating the procedural graph advances NetworkCanvas's region revision, then settles again.
    take(doc.graph_add_node(graph_key, "procedural.Null", "out_null", {60.f, 260.f}));
    present_once();
    const auto updated_idle_rev = workspace.network.idle_revision();
    require(updated_idle_rev != 0 && updated_idle_rev != settled_idle_rev,
            "Graph topology edit did not advance NetworkCanvas region revision.");
    const auto stats_after_edit = ui_session.statistics();
    present_once();
    const auto stats_resettled = ui_session.statistics();
    require(stats_resettled.control_batches == stats_after_edit.control_batches &&
                stats_resettled.controls_updated == stats_after_edit.controls_updated,
            "NetworkPanel + NetworkCanvas did not re-settle to zero UI control updates after graph edit.");

    // Frame 6: Guides menu checkbox items reflect authoritative show_fps state immediately on projection.
    workspace.menu = ui::ContextMenuRequest{editor::control::scene_guides, 0, 0, 0};
    present_once();
    const auto fps_on =
        std::ranges::find_if(workspace.menu_items, [](const ui::MenuItem& item) { return item.text == L"Show FPS"; });
    require(fps_on != workspace.menu_items.end() && fps_on->selected,
            "Guides menu did not project Show FPS as checked.");
    workspace.show_fps = false;
    workspace.bump_view_revision();
    present_once();
    const auto fps_off =
        std::ranges::find_if(workspace.menu_items, [](const ui::MenuItem& item) { return item.text == L"Show FPS"; });
    require(fps_off != workspace.menu_items.end() && !fps_off->selected,
            "Guides menu retained stale Show FPS state after view_revision update.");
    workspace.menu.reset();

    complete(doc.close());
}

void verify_stateful_rebuild(const std::filesystem::path& root) {
    auto project = take(editor::Project::create(root));
    const auto sop_file = root / "Source/HeightStep.source";
    take(editor::write_file(sop_file, "class game.HeightStep : procedural.Node {\n"
                                      "    float offset = 1.0f;\n"
                                      "    void process([InOut(\"P\")] Array<vector3>& P) {\n"
                                      "        const float dy = self.offset;\n"
                                      "        const uint n = P.size();\n"
                                      "        parallel_for (uint i : n) { P[i] = P[i] + vector3(0.0f, dy, 0.0f); }\n"
                                      "    }\n"
                                      "}\n"));
    const auto disk_graph_json =
        R"({"version":4,"settings":{"display":"step"},"parameters":[{"name":"step_offset","targets":["step.offset"]}],"nodes":{)"
        R"("box":{"type":"procedural.Box","position":[40,20],"properties":{"size":{"x":1.0,"y":1.0,"z":1.0}}},)"
        R"("step":{"type":"game.HeightStep","position":[40,120],"inputs":{"input":"box.output"},"properties":{"offset":1.5}})"
        R"(}})";
    const auto graph_key =
        take(project.create_procedural_graph(L"Content/Graphs/RebuildCheck.procgraph", disk_graph_json));

    auto loaded = editor::load_project_scene(project, take(assets::read_file(root / "Content/Scenes/Main.scene")),
                                             editor::LoadPolicy::tolerant, false, editor::SessionRole::editor);
    require(loaded.session != nullptr && loaded.code != nullptr && !loaded.error,
            loaded.error ? loaded.error->message : "Initial HeightStep project failed to build.");
    auto doc = std::move(loaded.session);

    complete(doc->create_procedural_generator(graph_key, {2.f, 0.f, -1.f}, &project));
    const ObjectId gen_entity_before = doc->selection().entity;
    {
        const auto gen_parts = take(world::components(doc->world().runtime(), gen_entity_before));
        const Scalar ov_two{2.0f};
        take(doc->set_promoted_parameter_scalars(editor::PromotedParameterTarget{{}, {}, gen_parts.front().id()},
                                                 "step_offset", {&ov_two, 1}, &project));
    }

    // Open the project graph and make an UNSAVED in-memory edit, plus create an in-memory scratch graph with AssetNode:
    take(doc->open_graph(project, graph_key));
    take(doc->graph_add_node(graph_key, "procedural.Null", "unsaved_out", {40.f, 220.f}));
    take(doc->graph_connect(graph_key, "step", "output", "unsaved_out", "input"));
    take(doc->graph_set_display(graph_key, "unsaved_out"));
    require(doc->graph_dirty(graph_key), "Unsaved node addition did not mark procedural graph dirty.");

    take(doc->create_scratch_graph("scratch/procedural"));
    take(doc->graph_add_node("scratch/procedural", "asset:" + graph_key, "temp_asset", {20.f, 30.f}, &project));
    take(doc->graph_set_display("scratch/procedural", "temp_asset"));

    editor::Workspace workspace;
    workspace.project_open = true;
    workspace.active_graph = graph_key;
    workspace.set_single_graph_selection("step");
    workspace.network.step_zoom(2, 320.f, 180.f);
    const float saved_zoom = workspace.network.zoom();
    const auto saved_pan = workspace.network.pan();

    // 1) Compatible method-body edit hot-reloads in place via poll() without losing graph Undo history:
    take(editor::write_file(sop_file, "class game.HeightStep : procedural.Node {\n"
                                      "    float offset = 1.0f;\n"
                                      "    void process([InOut(\"P\")] Array<vector3>& P) {\n"
                                      "        const float dy = self.offset * 2.0f;\n"
                                      "        const uint n = P.size();\n"
                                      "        parallel_for (uint i : n) { P[i] = P[i] + vector3(0.0f, dy, 0.0f); }\n"
                                      "    }\n"
                                      "}\n"));
    take(loaded.code->poll(project, true));
    require(doc->graph_can_undo(graph_key, false), "Compatible Source hot-reload cleared graph Undo history.");
    const auto hot_eval = take(doc->evaluate_graph(graph_key, &project));
    require(hot_eval.display_output && std::abs(hot_eval.display_output->positions()[0].y - 2.5f) < 1e-4f,
            "Compatible Source hot-reload did not update cooked HeightStep output (-0.5 + 1.5 * 2 = 2.5).");

    // 2) Adding a new public field triggers rebuild_required on poll(), then stateful rebuild preserves everything:
    take(editor::write_file(sop_file, "class game.HeightStep : procedural.Node {\n"
                                      "    float offset = 1.0f;\n"
                                      "    float gain = 4.0f;\n"
                                      "    void process([InOut(\"P\")] Array<vector3>& P) {\n"
                                      "        const float dy = self.offset * self.gain;\n"
                                      "        const uint n = P.size();\n"
                                      "        parallel_for (uint i : n) { P[i] = P[i] + vector3(0.0f, dy, 0.0f); }\n"
                                      "    }\n"
                                      "}\n"));
    const auto schema_poll = loaded.code->poll(project, true);
    require(!schema_poll && schema_poll.error().code == ErrorCode::rebuild_required,
            "Adding a field to a published procedural.Node did not report rebuild_required.");

    const auto open_snapshots = take(doc->snapshot_open_graphs());
    const auto scene_text = take(doc->serialize());
    const auto gen_lid = doc->document_identity(gen_entity_before);
    require(gen_lid.has_value(), "Serialized ProceduralGeneratorEntity lacked a persistent document LocalId.");
    const bool scene_dirty = doc->dirty();

    auto rebuilt = editor::load_project_scene(project, scene_text, editor::LoadPolicy::tolerant, false,
                                              editor::SessionRole::editor);
    require(rebuilt.session != nullptr && rebuilt.code != nullptr && !rebuilt.error,
            rebuilt.error ? rebuilt.error->message : "Stateful rebuild failed to load scene.");
    if (scene_dirty)
        rebuilt.session->mark_modified();
    take(rebuilt.session->restore_open_graphs(&project, open_snapshots));
    take(rebuilt.session->sync_procedural_generators(&project));

    workspace.reset_document(true);
    complete(doc->close());
    doc = std::move(rebuilt.session);
    const ObjectId gen_entity_after = take(doc->local_object(*gen_lid));
    take(doc->select(gen_entity_after));
    workspace.reconcile_rebuild(*doc, &project);

    // Verify unsaved .procgraph draft, dirty flag, non-overwritten disk file, scratch graph, new field & Workspace state:
    const auto disk_after = take(project.assets().procedural_graph(graph_key));
    require(disk_after.text.find("unsaved_out") == std::string::npos,
            "Stateful rebuild silently overwrote the unsaved .procgraph file on disk.");
    require(doc->graph_dirty(graph_key) && doc->graph_node(graph_key, "unsaved_out") != nullptr,
            "Stateful rebuild lost unsaved .procgraph edits or its dirty marker.");
    require(doc->has_graph("scratch/procedural") && doc->graph_node("scratch/procedural", "temp_asset") != nullptr,
            "Stateful rebuild lost the in-memory scratch procedural graph or its AssetNode.");
    require(workspace.active_graph == graph_key && workspace.selected_graph_node == "step" &&
                std::abs(workspace.network.zoom() - saved_zoom) < 1e-5f && workspace.network.pan() == saved_pan,
            "Stateful rebuild reset active SOP graph, selected node or NetworkCanvas pan/zoom.");

    const auto rebuilt_eval = take(doc->evaluate_graph(graph_key, &project));
    require(rebuilt_eval.display_output && std::abs(rebuilt_eval.display_output->positions()[0].y - 5.5f) < 1e-4f,
            "Rebuilt HeightStep did not preserve offset=1.5 while applying new default gain=4.0 (-0.5 + 6.0 = 5.5).");
    const auto scratch_eval = take(doc->evaluate_graph("scratch/procedural", &project));
    require(scratch_eval.display_output && std::abs(scratch_eval.display_output->positions()[0].y - 5.5f) < 1e-4f,
            "Scratch AssetNode did not rebind and cook against the rebuilt open Digital Asset.");
    const auto items = take(doc->presentation().render_items());
    require(
        !items.empty() && items.front().entity == gen_entity_after && items.front().geometry &&
            std::abs(items.front().geometry->bounds().maximum.y - 8.5f) < 1e-4f,
        "World ProceduralGeneratorEntity did not preserve step_offset=2.0 override across stateful rebuild (0.5 + 2.0 * 4.0 = 8.5).");

    complete(doc->close());
}
}

int main() {
    atelier::install_fatal_handler();
    application::install_frame_timing();
    const auto base = std::filesystem::current_path() / "project-integration-evidence" /
                      (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    run(base / "source-run");
    verify_network_control_reuse(base / "network-reuse");
    verify_stateful_rebuild(base / "stateful-rebuild");
    std::puts(
        "Project integration: PASS (Source tick, live rename and report, located build rejection, stuck child shutdown, Network ControlList reuse, stateful rebuild)");
    return 0;
}
