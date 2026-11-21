#include "../helpers.hpp"
#include "assets/file.hpp"
#include "editor/project_code.hpp"
#include "editor/workspace.hpp"
#include "application/play_process.hpp"
#include "application/presentation.hpp"
#include "application/timing.hpp"
#include "logging/logging.hpp"
#include <Windows.h>
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
    complete(document.close());

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
        R"({"version":2,"settings":{"display":"xform"},"nodes":{)"
        R"("grid":{"type":"procedural.Grid","position":[60,40],"properties":{"columns":4,"rows":4}},)"
        R"("xform":{"type":"procedural.Transform","position":[60,150],"inputs":{"input":"grid.output"}}}})"));
    editor::Session doc;
    take(doc.initialize({}, editor::SessionRole::editor));
    take(doc.open_graph(project, graph_key));

    editor::Workspace workspace;
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
    take_ui(ui_session.initialize(application::editor_fonts()));
    take_ui(ui_session.set_style_sheet(take(application::editor_styles())));
    take_ui(ui_session.set_surface({{1400, 900}, 1.f, 1}));

    ui::ControlList list;
    const auto present_once = [&]() {
        take(workspace.describe(list, doc, {1400, 900}, ui_session.presented().get(), &project, &ui_session, nullptr));
        take_ui(ui_session.set_controls(list));
        auto frame = take_ui(ui_session.prepare());
        require(ui_session.accept(frame), "Workspace ControlList frame was rejected.");
        workspace.commit_ui_controls();
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
}

int main() {
    atelier::install_fatal_handler();
    application::install_frame_timing();
    const auto base = std::filesystem::current_path() / "project-integration-evidence" /
                      (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    run(base / "source-run");
    verify_network_control_reuse(base / "network-reuse");
    std::puts("Project integration: PASS (Source tick, stuck child shutdown, Network ControlList reuse)");
    return 0;
}
