#include "memory/fatal.hpp"
#include "../helpers.hpp"
#include "editor/session.hpp"
#include "editor/graph_format.hpp"
#include "editor/project.hpp"
#include "procedural/evaluator.hpp"
#include "procedural/nodes.hpp"
#include "source/program.hpp"
#include "source/snapshot.hpp"
#include "editor/assets.hpp"
#include "assets/file.hpp"
#include "platform/utf.hpp"
#include "foundation/frame_timing.hpp"
#include "physics/timing.hpp"
#include "editor/workspace.hpp"
#include "assets/json_format.hpp"
#include "assets/material_format.hpp"
#include "material/resources.hpp"
#include "geometry/mesh.hpp"
#include "runtime/native.hpp"
#include "application/diagnostics.hpp"
#include "application/recovery.hpp"
#include "application/presentation.hpp"
#include "application/procedural_preview.hpp"
#include "application/scene_snapshot.hpp"
#include "application/window.hpp"
#include "json/json.hpp"
#include "logging/logging.hpp"
#include "renderer/scene_visibility.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <numeric>
#include <numbers>
#include <cstdio>
#include "files.hpp"
#include <thread>
#include <unordered_set>
#include <windows.h>

atelier::Result<void> verify_mesh_reimport();

namespace {
using namespace atelier;

using namespace atelier::acceptance::helpers;

Result<void> changed(Result<world::Commit> result) {
    ATELIER_TRY(result);
    if (result->notification_error)
        return std::unexpected(std::move(*result->notification_error));
    return {};
}

void collect(editor::Session& document) {
    for (unsigned step = 0; step < 96; ++step)
        take(document.collect_step());
}

Result<ObjectId> component(editor::Session& document, ObjectId entity, TypeId type) {
    auto& runtime = document.world().runtime();
    ATELIER_TRY_BIND(parts, world::components(runtime, entity));
    for (const auto& root : *parts) {
        ATELIER_TRY_BIND(actual, runtime.type_of(root.id()));
        if (*actual == type)
            return root.id();
    }
    return std::unexpected(Error{ErrorCode::invalid_member, "Expected component is absent."});
}

#include "gltf_import.inl"
#include "project_profile.inl"

void verify_graph_format() {
    Runtime runtime;
    take(procedural::register_types(runtime));
    auto program = take(source::Program::create(runtime));
    {
        auto req =
            take(program->request({"graph_doc", 1, take(source::engine_sources(source::default_engine_root()))}));
        auto cand = take(source::Program::prepare(req));
        take(program->try_publish(cand));
    }
    const std::string_view input_json = R"({
  "nodes": {
    "grid": {
      "inputs": {},
      "position": [10.0, 20.0],
      "properties": {"columns": 3, "rows": 2, "size": {"x": 2.0, "y": 1.0}},
      "type": "procedural.Grid"
    },
    "jitter": {
      "inputs": {"input": "grid.output"},
      "position": [180.0, 20.0],
      "properties": {"amount": 0.25, "group": "", "seed": 4},
      "type": "procedural.Jitter"
    },
    "legacy": {
      "inputs": {"input": "grid.output"},
      "position": [180.0, 140.0],
      "properties": {"foo": 42},
      "type": "game.MissingSop"
    },
    "merged": {
      "inputs": {"inputs": ["jitter.output", "legacy.output"]},
      "position": [360.0, 60.0],
      "properties": {},
      "type": "procedural.Merge"
    },
    "noise": {
      "color": "#eab308",
      "comment": "Per-point noise",
      "inputs": {"input": "merged.output"},
      "position": [540.0, 60.0],
      "properties": {"attribute": "nval", "frequency": 2.0, "layer": "point"},
      "shape": "ribbon",
      "type": "procedural.Noise"
    }
  },
  "boxes": {
    "stage_a": {
      "color": "#3b5b78",
      "position": [0.0, 0.0],
      "size": [420.0, 220.0],
      "title": "Stage A"
    }
  },
  "notes": {
    "memo1": {
      "color": "#d4a72c",
      "position": [540.0, 140.0],
      "size": [200.0, 96.0],
      "text": "Line 1\nLine 2",
      "title": "Memo"
    }
  },
  "parameters": [
    {
      "category": "Sampling",
      "description": "Grid column count.",
      "label": "Columns",
      "name": "grid_cols",
      "targets": ["grid.columns"]
    }
  ],
  "settings": {"display": "noise", "seed": 9, "template": "grid"},
  "version": 4
})";
    const auto old_settlement = editor::load_graph(runtime, R"({"version":3,"settings":{"display":"chapel_place"},
      "nodes":{"chapel_place":{"type":"procedural.Transform","properties":{
        "rotate":{"w":0.98768836,"x":0,"y":-0.15643446,"z":0}}}}})");
    require(!old_settlement && old_settlement.error().message == "Unsupported procedural graph version.",
            "A saved v3 quaternion must fail at the version boundary before decoding Euler angles.");
    const auto doc = take(editor::load_graph(runtime, input_json));
    require(
        doc.nodes.size() == 5 && doc.notes.size() == 1 && doc.boxes.size() == 1 && doc.parameters.size() == 1 &&
            doc.parameters[0].name == "grid_cols" && doc.template_node != ObjectId{} && doc.issues.size() == 2 &&
            doc.issues[0].kind == world::DiagnosticKind::unknown_type &&
            doc.issues[1].kind == world::DiagnosticKind::reference_dropped,
        "load_graph did not tolerantly retain v4 parameters, annotations, template, missing SOP type and dropped connection.");
    const auto saved_once = take(editor::save_graph(runtime, doc));
    const auto reloaded = take(editor::load_graph(runtime, saved_once));
    const auto saved_twice = take(editor::save_graph(runtime, reloaded));
    require(saved_once == saved_twice, "save_graph -> load_graph -> save_graph was not byte-identical.");
    struct NoMeshes final : procedural::Resources {
        Result<geometry::MeshRef> mesh(StringView) const noexcept override {
            return std::unexpected(Error{ErrorCode::invalid_member, "Unused"});
        }

        std::uint64_t revision() const noexcept override { return 1; }
    } resources;
    procedural::Evaluator evaluator{runtime, reloaded.graph, resources, program.get()};
    take(evaluator.evaluate());
    const auto* out = evaluator.output();
    require(out != nullptr && out->count(procedural::Layer::point) == 6 &&
                take(out->read<float>("nval", procedural::Layer::point)) != nullptr,
            "Reloaded C++/Source procedural graph did not evaluate to 6 noisy points.");
    // Persist a wall asset and cook it from its saved graph, including enum and named ports.
    const auto wall = take(editor::load_graph(runtime, R"({"version":4,"settings":{"display":"wall"},"nodes":{
      "line":{"type":"procedural.Line","properties":{"end":{"x":6,"y":0,"z":0},"count":7}},
      "path":{"type":"procedural.Polyline","inputs":{"points":"line.output"}},
      "stations":{"type":"procedural.Resample","properties":{"mode":"segments","segments":4,"preserve_corners":true},
                  "inputs":{"curves":"path.output"}},
      "wall":{"type":"procedural.Sweep","inputs":{"backbone":"stations.output"}}}})"));
    require(wall.issues.empty(), "Wall graph failed to bind Resample parameters or ports.");
    const auto wall_text = take(editor::save_graph(runtime, wall));
    const auto reopened = take(editor::load_graph(runtime, wall_text));
    require(reopened.issues.empty() && take(editor::save_graph(runtime, reopened)) == wall_text,
            "Wall graph was not stable across save/reopen.");
    procedural::Evaluator walls{runtime, reopened.graph, resources, program.get()};
    take(walls.evaluate());
    require(walls.output() && walls.output()->count(procedural::Layer::point) == 20,
            "Line -> Polyline -> Resample -> Sweep should build five rectangular wall stations.");
}

Result<void> verify_scene_recovery(editor::Project& project) {
    editor::Session document;
    take(document.initialize({}, editor::SessionRole::editor));
    ATELIER_TRY(changed(document.add_entity()));
    const auto directory = project.root() / "Cache" / "Recovery";
    const auto original = project.root() / "Content/Scenes/Main.scene";
    const auto before = take(assets::read_file(original));
    const auto expected_scene = take(document.serialize());

    application::SceneRecovery recovery;
    const auto saved = take(recovery.preserve(document, project.root(), original, "Frame read failed", directory));
    require(saved && saved->warning.empty(), "Author scene recovery did not publish a complete record");
    const auto metadata = take(assets::read_file(saved->scene.parent_path() / "recovery.json"));
    const auto parsed = json::Document::parse(metadata, {16});
    require(parsed && parsed->root().string({"reason"}) == "Frame read failed" &&
                take(assets::read_file(saved->scene)) == expected_scene,
            "Recovery lost the failed frame's scene or reason");

    application::SceneRecovery partial;
    const auto surviving = take(partial.preserve(document, project.root(), original, "\xff", directory));
    if (!surviving)
        return std::unexpected(Error{ErrorCode::invalid_storage, "Recovery lost its written scene."});
    ATELIER_TRY_BIND(metadata_exists, acceptance::file_exists(surviving->scene.parent_path() / "recovery.json"));
    require(!surviving->warning.empty() && !*metadata_exists,
            "Rejected metadata hid the saved scene or published a partial sidecar");
    const auto loaded = take(editor::Session::deserialize(take(assets::read_file(surviving->scene)), project, {},
                                                          editor::LoadPolicy::strict));
    require(take(loaded->world().members()).size() == 1, "Surviving recovery scene cannot be reopened");

    application::SceneRecovery blocked;
    const auto failure = blocked.preserve(document, project.root(), original, "Frame read failed", original);
    const auto& again = blocked.preserve(document, project.root(), original, "Another failure", directory);
    require(!failure && !again && !failure.error().message.empty() && again.error().code == failure.error().code &&
                again.error().message == failure.error().message && document.dirty() &&
                take(assets::read_file(original)) == before,
            "A failed recovery replaced its first error or the original scene");
    return {};
}

Result<void> verify_roof_group(editor::Session& doc, editor::Project& project, editor::Workspace& workspace,
                               editor::OperatorHost& host) {
    using procedural::Layer;
    const auto key = take(project.create_procedural_graph(L"Content/Graphs/Roof.procgraph", R"({
      "version":4,"settings":{"display":"working"},"nodes":{
        "roof":{"type":"procedural.Wedge","properties":{"size":{"x":2,"y":1,"z":2}}},
        "upper":{"type":"procedural.Group","properties":{"group":"roof_faces","max_angle":45},"inputs":{"input":"roof.output"}},
        "protected":{"type":"procedural.Group","properties":{"group":"protected_side","direction":{"x":1,"y":1,"z":0},"max_angle":0},
                     "inputs":{"input":"upper.output"}},
        "working":{"type":"procedural.Group","properties":{"group":"work_area","criterion":"existing",
                   "source_group":"protected_side","operation":"subtract","base_group":"roof_faces"},"inputs":{"input":"protected.output"}},
        "raise":{"type":"procedural.Extrude","properties":{"group":"work_area","distance":0.5,"output_back":false,"front_group":"raised"},
                 "inputs":{"surface":"working.output"}}}})"));
    ATELIER_TRY(host.open_graph_context(key));
    workspace.dock.select(editor::control::scene_tab);
    workspace.menu = ui::ContextMenuRequest{editor::control::scene_guides, 0, 0, 0};
    const auto describe = [&]() {
        auto rows = take(workspace.describe(doc, {1400, 900}, nullptr, &project));
        take(workspace.commit_ui_controls());
        return rows;
    };
    describe();
    const auto chooser =
        std::ranges::find(workspace.menu_items, editor::control::display_sop_group, &ui::MenuItem::key);
    require(chooser != workspace.menu_items.end(), "Guides menu did not offer group selection for the roof.");
    const auto choice = std::ranges::find(chooser->children, L"work_area  (primitives)", &ui::MenuItem::text);
    require(choice != chooser->children.end() &&
                take(workspace.apply_view(doc, host, {ui::ActionKind::activate, choice->key, {}})),
            "Could not highlight the cooked work_area through the real viewport menu.");
    workspace.menu.reset();
    const auto rows = describe();
    const auto hud = std::ranges::find(rows, editor::control::viewport_geometry_hud, &ui::Control::key);
    require(hud != rows.end() && hud->text.find(L"work_area  1 / 5 primitives") != std::wstring::npos,
            "Roof HUD did not report the same one-of-five selection as the cook.");

    const auto original = take(doc.evaluate_graph(key, &project)).preview_output;
    require(original && workspace.sop_group.resolve(*original) && workspace.sop_group.samples().size() == 1 &&
                workspace.sop_group.samples().front() == 2,
            "The unprotected left roof slope was not selected.");
    const auto center = [](const procedural::Geometry& g, std::uint32_t index) {
        const auto face = g.primitive(index);
        math::Vector3 sum{};
        for (auto p : g.vertex_points().subspan(face.first, face.count))
            sum += g.positions()[p];
        return sum / static_cast<float>(face.count);
    };
    editor::Camera camera;
    camera.frame({}, 3.f);
    const auto view = editor::ViewportView::make(camera, {0, 0, 800, 600}, 1.f);
    std::vector<renderer::OverlayShape> overlay;
    application::append_procedural_guides(*original, view, {.group = &workspace.sop_group}, overlay);
    const auto screen = view.project(center(*original, 2));
    require(screen && overlay.size() == 5 && std::abs(overlay.front().points[0] - screen->x) < .01f &&
                std::abs(overlay.front().points[1] - screen->y) < .01f,
            "Viewport did not outline the selected quad and mark its projected center.");

    ATELIER_TRY(doc.graph_set_display(key, "raise"));
    const auto extruded = take(doc.evaluate_graph(key, &project));
    require(extruded.display_output && extruded.display_output->count(Layer::primitive) == 9,
            "Extrude did not produce the expected raised slope and four side faces.");
    const auto& g = *extruded.display_output;
    const auto raised = take(g.read<bool>("raised", Layer::primitive));
    const auto protected_side = take(g.read<bool>("protected_side", Layer::primitive));
    require(raised && protected_side && std::ranges::count(*raised, true) == 1 &&
                std::ranges::count(*protected_side, true) == 1,
            "Roof groups did not survive the extrusion.");
    const auto face_index = [](const auto& mask) {
        return static_cast<std::uint32_t>(std::ranges::find(mask, true) - mask.begin());
    };
    const auto delta = center(g, face_index(*raised)) - center(*original, 2);
    require(math::length(delta - math::Vector3{-.35355339f, .35355339f, 0}) < 1e-5f &&
                math::length(center(g, face_index(*protected_side)) - center(*original, 1)) < 1e-5f &&
                workspace.sop_group.resolve(g) && workspace.sop_group.count() == 5,
            "Extrusion disagreed with the highlighted face, moved the protected slope, or left a stale guide.");
    ATELIER_TRY(doc.graph_set_display(key, "roof"));
    const auto bare = take(doc.evaluate_graph(key, &project));
    require(bare.display_output && !workspace.sop_group.resolve(*bare.display_output) &&
                workspace.sop_group.samples().empty(),
            "Switching to a node without the group retained a stale highlight.");
    workspace.sop_group = {};
    ATELIER_TRY(doc.save_graph(project, key));
    host.exit_graph_context();
    return {};
}

Result<void> verify_procedural_editor() {
    ATELIER_TRY_BIND(
        temp, acceptance::file_operation([](auto& error) { return std::filesystem::temp_directory_path(error); }));
    const auto temp_root = *temp / ("atelier-sop-editor-" + std::to_string(GetCurrentProcessId()) + "-" +
                                    std::to_string(GetTickCount64()));
    struct Cleanup {
        std::filesystem::path dir;

        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    } cleanup{temp_root};

    auto project = take(editor::Project::create(temp_root));
    ATELIER_TRY(verify_scene_recovery(project));
    take(editor::write_file(temp_root / "Source/TerraceWave.source", R"(
[DESC = "抬高台地，记录新的点高。"]
class game.TerraceWave : procedural.Node {
    float lift = 0.5f;
    void process([InOut] Array<vector3>& P, [Out] Array<float>& custom_h) {
        const float lift = self.lift;
        const uint n = P.size();
        if (custom_h.size() != n) { return; }
        parallel_for (uint i : n) {
            const vector3 moved = P[i] + vector3(0.0f, lift, 0.0f);
            P[i] = moved;
            custom_h[i] = moved.y;
        }
    }
}
)"));

    take(editor::write_file(temp_root / "Source/DiagnosticSop.source", R"(
class game.DiagnosticSop : procedural.Node {
    void process([InOut] Array<vector3>& P) { fail("A\0B"); }
}
)"));
    const auto graph_key = take(project.create_procedural_graph(L"Content/Graphs/Terrain.procgraph"));
    const auto* graph_entry = project.entry(graph_key);
    ATELIER_TRY_BIND(sidecar_exists,
                     acceptance::file_exists(temp_root / "Content/Graphs/Terrain.procgraph.asset.json"));
    require(graph_entry != nullptr && graph_entry->kind == assets::AssetKind::procedural_graph && *sidecar_exists,
            "Project did not register .procgraph asset with GUID sidecar.");

    auto loaded = editor::load_project_scene(project, take(assets::read_file(temp_root / "Content/Scenes/Main.scene")),
                                             editor::LoadPolicy::strict, false, editor::SessionRole::editor);
    require(loaded.session != nullptr && !loaded.error,
            loaded.error ? loaded.error->message : "Project scene failed to load with custom Source SOP.");
    auto& doc = *loaded.session;
    doc.bind_project_code(loaded.code);

    // Tolerant assets remain editable when an unresolved endpoint shares a multi-input port.
    const auto repair_key = take(project.create_procedural_graph(L"Content/Graphs/Repair.procgraph", R"({
      "version":4,"settings":{},"nodes":{
        "grid":{"type":"procedural.Grid","properties":{"columns":"old"}},
        "legacy":{"type":"game.MissingSop"},
        "merged":{"type":"procedural.Merge","inputs":{"inputs":["grid.output","legacy.output"]}},
        "reader":{"type":"procedural.MeshImport","properties":{"mesh":17}}}})"));
    take(doc.open_graph(project, repair_key));
    const auto reader = doc.graph_node_id(repair_key, "reader");
    auto& rt = doc.world().runtime();
    const std::array mesh_path{take(rt.find_field(take(rt.type_of(reader)), "mesh"))};
    take(doc.set_string_property(reader, mesh_path, "builtin/cube"));
    require(doc.graph_dirty(repair_key), "Repairing a retained property must dirty the document.");
    take(doc.graph_undo(repair_key, false));
    require(!doc.graph_dirty(repair_key), "Undo must restore the retained JSON.");
    take(doc.graph_undo(repair_key, true));
    const auto grid = doc.graph_node_id(repair_key, "grid");
    const std::array cols{take(rt.find_field(take(rt.type_of(grid)), "columns"))};
    take(doc.begin_edit(grid, cols, 71));
    take(doc.commit_edit(71)); // Explicitly accept the fallback value of the malformed field.
    take(doc.graph_rename_node(repair_key, "grid", "renamed"));
    take(doc.graph_undo(repair_key, false));
    take(doc.graph_undo(repair_key, true));
    take(doc.save_graph(project, repair_key));
    const auto disk = take(assets::read_file(temp_root / "Content/Graphs/Repair.procgraph"));
    const auto reopened = take(editor::load_graph(rt, disk));
    require(disk.find("builtin/cube") != disk.npos && disk.find("renamed.output") != disk.npos &&
                disk.find("legacy.output") != disk.npos && disk.find("grid.output") == disk.npos &&
                reopened.issues.size() == 2,
            "Save/reopen must retain repaired properties, renamed live edges and unavailable endpoints.");

    // Cutting wires or deleting nodes on a multi-input port with unresolved siblings must strip removed endpoints,
    // preserve remaining unresolved items and order across save/reopen, and round-trip through Undo/Redo:
    {
        take(doc.graph_add_node(repair_key, "procedural.Box", "extra_box", {40.f, 220.f}));
        take(doc.graph_connect(repair_key, "extra_box", "output", "merged", "inputs"));
        take(doc.save_graph(project, repair_key));
        const auto with_extra = take(assets::read_file(temp_root / "Content/Graphs/Repair.procgraph"));
        const auto pos_ren = with_extra.find("renamed.output");
        const auto pos_leg = with_extra.find("legacy.output");
        const auto pos_ext = with_extra.find("extra_box.output");
        require(pos_ren != with_extra.npos && pos_leg != with_extra.npos && pos_ext != with_extra.npos &&
                    pos_ren < pos_leg && pos_leg < pos_ext,
                "Connecting to a multi-input port with kept endpoints lost unresolved siblings or order.");

        const std::array cut_renamed{editor::GraphConnectionView{"renamed", "output", 0, "merged", "inputs", 0}};
        take(doc.graph_disconnect_edges(repair_key, cut_renamed));
        require(doc.graph_dirty(repair_key), "Cutting a live wire on a kept multi-input port must dirty the graph.");
        take(doc.graph_undo(repair_key, false));
        require(!doc.graph_dirty(repair_key), "Undo of wire cut did not restore exact kept multi-input list.");
        take(doc.graph_undo(repair_key, true));
        take(doc.save_graph(project, repair_key));
        const auto after_cut_disk = take(assets::read_file(temp_root / "Content/Graphs/Repair.procgraph"));
        const auto reopened_after_cut = take(editor::load_graph(rt, after_cut_disk));
        const auto cut_leg = after_cut_disk.find("legacy.output");
        const auto cut_ext = after_cut_disk.find("extra_box.output");
        require(after_cut_disk.find("renamed.output") == after_cut_disk.npos && cut_leg != after_cut_disk.npos &&
                    cut_ext != after_cut_disk.npos && cut_leg < cut_ext && reopened_after_cut.issues.size() == 2,
                "Cut wire resurrected on save/reopen or disturbed remaining kept multi-input order.");

        const std::array remove_extra{std::string{"extra_box"}};
        take(doc.graph_remove_nodes(repair_key, remove_extra));
        require(doc.graph_dirty(repair_key), "Removing an upstream node must dirty the graph.");
        take(doc.graph_undo(repair_key, false));
        require(!doc.graph_dirty(repair_key), "Undo of node removal did not restore downstream kept multi-input list.");
        take(doc.graph_undo(repair_key, true));
        take(doc.save_graph(project, repair_key));
        const auto after_del_disk = take(assets::read_file(temp_root / "Content/Graphs/Repair.procgraph"));
        require(after_del_disk.find("extra_box") == after_del_disk.npos &&
                    after_del_disk.find("renamed.output") == after_del_disk.npos &&
                    after_del_disk.find("legacy.output") != after_del_disk.npos,
                "Removed node endpoint resurrected on save or lost unresolved sibling.");
    }

    const auto catalog = take(doc.procedural_node_types());
    require(std::ranges::any_of(catalog, [](const auto& c) { return c.name == "procedural.Resample" && c.native; }),
            "Resample was missing from the editor SOP catalog.");
    require(std::ranges::any_of(catalog, [](const auto& c) { return c.name == "procedural.Grid" && c.native; }) &&
                std::ranges::any_of(catalog, [](const auto& c) { return c.name == "game.TerraceWave" && !c.native; }),
            "Session procedural node catalog did not include both native C++ and project .source SOPs.");

    editor::Workspace workspace;
    take(workspace.initialize());
    workspace.project_open = true;

    struct Host final : editor::OperatorHost {
        editor::Session& doc;

        editor::Project& proj;

        editor::Workspace& ws;

        editor::Camera cam{};

        editor::ViewportTool vtool{};

        Host(editor::Session& d, editor::Project& p, editor::Workspace& w) : doc(d), proj(p), ws(w) {}

        Result<std::optional<std::filesystem::path>> choose_path(editor::PathPurpose, const std::filesystem::path&,
                                                                 std::wstring_view) override {
            return std::optional<std::filesystem::path>{};
        }

        void asset_created(std::string_view, bool) override {}

        Result<void> end_interaction() override { return {}; }

        const editor::Camera& camera() const noexcept override { return cam; }

        Result<void> set_camera(const editor::Camera& next) override {
            cam = next;
            return {};
        }

        editor::ViewportTool& viewport_tool() noexcept override { return vtool; }

        Result<void> place_duplicates() override { return {}; }

        Result<void> begin_entity_rename(ObjectId, editor::RegionKind) override { return {}; }

        Result<void> reset_layout() override { return ws.reset_layout(); }

        Result<void> close_tab(editor::RegionKind, bool) override { return {}; }

        void maximize_tab(editor::RegionKind) override {}

        void show_tab(editor::RegionKind r) override { ws.dock.select(editor::region_tab(r)); }

        void cycle_tab(editor::RegionKind, int) override {}

        Result<void> source_history(bool) override { return {}; }

        Result<void> workbench_history(bool) override { return {}; }

        void scene_changed() override {}

        void set_tool_settings(const editor::ToolSettings& s) override { ws.viewport_settings = s; }

        Result<void> set_view_settings(const renderer::ViewSettings& v) override {
            return ws.set_view_settings(doc, v);
        }

        void quit() override {}

        void about() override {}

        void select_box() override {}

        void show_menu(editor::MenuKind) override {}

        void toggle_view(editor::ViewToggleOption) override {}

        Result<void> rename_asset(std::string_view, std::string_view) override { return {}; }

        Result<void> save_scene(const std::filesystem::path&) override { return {}; }

        Result<void> save_documents(const editor::Context& c, bool) override {
            if (!c.active_graph.empty()) {
                ATELIER_TRY(doc.save_graph(proj, c.active_graph));
                return proj.refresh();
            }
            return {};
        }

        Result<void> open_source(const std::filesystem::path&) override { return {}; }

        Result<void> create_source(const std::filesystem::path&, bool) override { return {}; }

        Result<void> close_source(bool) override { return {}; }

        Result<void> reload_source(bool) override { return {}; }

        Result<void> start_play(bool, bool) override { return {}; }

        void stop_play(bool) override {}

        Result<std::string> import_asset(editor::ImportKind, const std::filesystem::path&) override {
            return std::string{};
        }

        Result<void> reimport_asset(std::string_view, const std::filesystem::path&) override { return {}; }

        Result<void> open_graph_context(std::string_view key) override {
            if (!doc.has_graph(key))
                ATELIER_TRY(doc.open_graph(proj, key));
            ws.active_graph = std::string(key);
            ws.set_single_graph_selection({});
            ws.dock.select(editor::network_control::tab);
            ws.dock.select(editor::control::parameters_tab);
            ws.bump_view_revision();
            return {};
        }

        void exit_graph_context() override {
            ws.active_graph.clear();
            ws.set_single_graph_selection({});
            ws.bump_view_revision();
        }

        void select_graph_node(std::string_view key, std::string_view node_name) override {
            if (ws.active_graph == key) {
                ws.set_single_graph_selection(node_name);
                ws.bump_view_revision();
            }
        }
    } host{doc, project, workspace};
    ATELIER_TRY(verify_roof_group(doc, project, workspace, host));

    const auto parse_doc = [](std::string_view json_text) { return take(json::Document::parse(json_text, {65536})); };
    const auto take_ui = [](auto result) {
        if (!result)
            fatal(application::ui_error(result.error()).message);
        if constexpr (!std::is_void_v<typename decltype(result)::value_type>)
            return std::move(*result);
    };
    const auto invoke = [&](std::string id, std::string_view json_args = "{}") {
        auto parsed = parse_doc(json_args);
        const auto label = id;
        const auto report =
            take(editor::call(workspace.operators, host, workspace.operator_context(doc, &project, false),
                              {std::move(id), std::move(parsed), {}}));
        require(report.status == editor::OperatorStatus::finished,
                report.message.empty() ? ("Operator did not finish: " + label) : report.message);
        return report;
    };

    invoke("graph.open", "{\"asset\":\"" + graph_key + "\"}");
    require(workspace.active_graph == graph_key && !doc.graph_dirty(graph_key) && !doc.dirty(),
            "Opening an untouched .procgraph marked graph or scene dirty.");

    const auto set_node_props = [&](std::string_view node, std::string_view json_props) {
        const auto parsed = parse_doc(json_props);
        take(doc.graph_set_properties(graph_key, node, parsed.root()));
    };

    // Build 4034-point Sphere -> Noise -> TerraceWave (parallel_for) -> Scatter -> CopyToPoints(builtin/cube).
    invoke("graph.add_node", R"({"type":"procedural.Sphere","name":"sphere"})");
    set_node_props("sphere", R"({"radius":4.0,"segments":64,"latitudes":64})");
    invoke("graph.add_node", R"({"type":"procedural.Noise","name":"noise"})");
    invoke("graph.connect", R"({"from_node":"sphere","from_port":"output","to_node":"noise","to_port":"input"})");
    invoke("graph.add_node", R"({"type":"game.TerraceWave","name":"terrace"})");
    invoke("graph.connect", R"({"from_node":"noise","from_port":"output","to_node":"terrace","to_port":"input"})");
    invoke("graph.add_node", R"({"type":"procedural.Scatter","name":"scatter"})");
    set_node_props("scatter", R"({"density":0.25})");
    invoke("graph.connect", R"({"from_node":"terrace","from_port":"output","to_node":"scatter","to_port":"surface"})");
    invoke("graph.add_node", R"({"type":"procedural.CopyToPoints","name":"instancer"})");
    set_node_props("instancer", R"({"meshes":["builtin/cube"]})");
    invoke("graph.connect", R"({"from_node":"scatter","from_port":"points","to_node":"instancer","to_port":"points"})");

    // Decoupled Display Flag ("instancer") vs Selected Node ("terrace"):
    invoke("graph.set_display", R"({"node":"instancer"})");
    invoke("graph.select", R"({"node":"terrace"})");
    require(workspace.selected_graph_node == "terrace" && doc.graph_dirty(graph_key) && !doc.dirty(),
            "Graph edits polluted Scene dirty state or lost node selection.");

    const ObjectId terrace_id = doc.graph_node_id(graph_key, "terrace");
    const ObjectId noise_id = doc.graph_node_id(graph_key, "noise");
    require(terrace_id != ObjectId{} && noise_id != ObjectId{}, "Added SOP nodes missing ObjectId.");

    const auto inspected_view = take(doc.inspect_graph(graph_key, &project, terrace_id, true));
    const auto inst_count =
        inspected_view.display_summary.counts[static_cast<std::size_t>(procedural::Layer::instance)];
    require(inspected_view.display == "instancer" && !inspected_view.display_error && !inspected_view.inspected_error &&
                inst_count > 0 && inspected_view.inspected_summary.has_value() &&
                inspected_view.inspected_summary->counts[static_cast<std::size_t>(procedural::Layer::point)] == 4034 &&
                inspected_view.inspected_summary->counts[static_cast<std::size_t>(procedural::Layer::primitive)] ==
                    4096,
            "Decoupled Display Flag (cube instances) vs Selected Node (4034-point surface) inspection failed.");

    // Edit String parameter on Noise ("attribute": "elev") and Scalar parameter on TerraceWave ("lift": 2.0), then Undo/Redo.
    const auto noise_type = take(doc.world().runtime().type_of(noise_id));
    const std::array attr_path{take(doc.world().runtime().find_field(noise_type, "attribute"))};
    take(doc.set_string_property(noise_id, attr_path, "elev"));
    invoke("scene.set_properties", R"({"lift":2.0})");
    const auto after_edit = take(doc.inspect_graph(graph_key, &project, terrace_id));
    const auto& pt_attrs_after =
        after_edit.inspected_summary->attributes[static_cast<std::size_t>(procedural::Layer::point)];
    require(std::ranges::any_of(pt_attrs_after, [](const auto& a) { return a.name == "elev"; }) &&
                std::abs(after_edit.inspected_summary->bounds_max.y - 6.0f) < 1e-4f,
            "String and scalar SOP edits did not update cooked geometry.");

    const auto terrace_node_after = std::ranges::find(after_edit.nodes, "terrace", &editor::GraphNodeView::name);
    const auto hits_before_undo = terrace_node_after->status->cache_hits;

    invoke("edit.undo");
    invoke("edit.undo");
    const auto after_undo = take(doc.inspect_graph(graph_key, &project, terrace_id));
    const auto terrace_node_undone = std::ranges::find(after_undo.nodes, "terrace", &editor::GraphNodeView::name);
    require(std::abs(after_undo.inspected_summary->bounds_max.y - 4.5f) < 1e-4f &&
                terrace_node_undone->status->cache_hits > hits_before_undo,
            "SOP edit.undo did not restore original geometry or missed Evaluator cache.");

    invoke("edit.redo");
    invoke("edit.redo");
    const auto after_redo = take(doc.inspect_graph(graph_key, &project, terrace_id));
    require(std::abs(after_redo.inspected_summary->bounds_max.y - 6.0f) < 1e-4f,
            "SOP edit.redo did not re-apply graph changes.");

    // Save via file.save in SOP context, verify dirty clears and file round-trips.
    invoke("file.save");
    require(!doc.graph_dirty(graph_key) && doc.dirty_graphs().empty(),
            "file.save in SOP context did not save and mark .procgraph clean.");

    // GeometrySpreadsheet 512-row zero-copy paging + filtering + Instance layer:
    const auto eval_terrace = take(doc.evaluate_graph(graph_key, &project, terrace_id));
    require(eval_terrace.inspected_output != nullptr, "Missing terrace output.");
    const auto page0 = workspace.spreadsheet.build_table(eval_terrace.inspected_output, eval_terrace.generation);
    require(workspace.spreadsheet.total_rows() == 4034 && page0->rows.size() == editor::GeometrySpreadsheet::page_size,
            "GeometrySpreadsheet did not slice 4034 points into a 512-row page.");
    require(workspace.spreadsheet.apply({ui::ActionKind::activate, editor::spreadsheet_control::page_next, {}}),
            "GeometrySpreadsheet page_next action was not handled.");
    const auto page1 = workspace.spreadsheet.build_table(eval_terrace.inspected_output, eval_terrace.generation);
    require(workspace.spreadsheet.page_offset() == 512 && page1->rows.front().cells.front().text == L"512",
            "GeometrySpreadsheet second page did not start at point 512.");
    workspace.spreadsheet.set_filter(L"custom_h");
    const auto filtered_table =
        workspace.spreadsheet.build_table(eval_terrace.inspected_output, eval_terrace.generation);
    require(filtered_table->columns.size() == 2 && filtered_table->columns[1].title.starts_with(L"custom_h"),
            "GeometrySpreadsheet column filter did not isolate custom_h.");
    workspace.spreadsheet.set_filter(L"");
    workspace.spreadsheet.set_layer(procedural::Layer::instance);
    const auto inst_table = workspace.spreadsheet.build_table(eval_terrace.display_output, eval_terrace.generation);
    require(workspace.spreadsheet.total_rows() == inst_count && inst_table->rows.size() == inst_count,
            "GeometrySpreadsheet Instance layer did not show stamped instances.");
    workspace.spreadsheet.set_layer(procedural::Layer::point);

    // ProceduralPreview surface mesh triangle + quad wireframe edge_masks & viewport SOP guides:
    const auto surface_mesh = presentation::build_procedural_surface_mesh(*eval_terrace.inspected_output);
    require(surface_mesh != nullptr && surface_mesh->vertices().size() == 8064 * 3 && surface_mesh->edge_mask(0) == 7 &&
                surface_mesh->edge_mask(64) == 5 && surface_mesh->edge_mask(65) == 3,
            "build_procedural_surface_mesh did not emit polar triangle (7) and quad boundary edge_masks (5, 3).");
    invoke("view.frame_all");
    const auto view = editor::ViewportView::make(host.camera(), {0, 0, 800, 600}, 1.f);
    std::vector<renderer::OverlayShape> guides;
    application::append_procedural_guides(*eval_terrace.inspected_output, view,
                                          {.show_points = true,
                                           .show_point_normals = true,
                                           .show_face_normals = true,
                                           .show_orient = true,
                                           .show_bounds = true,
                                           .chunk_size = 4.f},
                                          guides);
    require(!guides.empty(), "append_procedural_guides emitted zero OverlayShapes for framed SOP geometry.");
    std::vector<renderer::OverlayShape> gnomon_shapes;
    application::append_orientation_gnomon(view, gnomon_shapes);
    require(gnomon_shapes.size() > 6, "append_orientation_gnomon did not emit 3D XYZ axis lines and tip glyphs.");

    // Phase A: pure point-cloud auto-points vs surface/instance default, Backface Tint option, HUD & bilingual tooltips:
    take(doc.graph_set_display(graph_key, "scatter"));
    const auto eval_scatter_only = take(doc.evaluate_graph(graph_key, &project));
    require(!workspace.sop_show_points.has_value() && eval_scatter_only.preview_output &&
                application::is_pure_point_cloud(*eval_scatter_only.preview_output) &&
                workspace.effective_show_points(eval_scatter_only.preview_output.get()),
            "Pure point-cloud SOP node (scatter) did not auto-enable viewport point markers.");
    take(doc.graph_set_display(graph_key, "instancer"));
    const auto eval_instancer = take(doc.evaluate_graph(graph_key, &project));
    require(eval_instancer.preview_output && !application::is_pure_point_cloud(*eval_instancer.preview_output) &&
                !workspace.effective_show_points(eval_instancer.preview_output.get()),
            "Surface/instance SOP node unexpectedly auto-enabled point markers without explicit user toggle.");
    invoke("view.set", R"({"backface_tint":true})");
    require(workspace.view.backface_tint, "view.set did not enable backface_tint on ViewSettings.");
    invoke("view.set", R"({"backface_tint":false})");
    require(!workspace.view.backface_tint, "view.set did not clear backface_tint on ViewSettings.");

    const auto original_grid = workspace.view.reference_grid;
    require(take(workspace.apply_view(doc, host,
                                      {ui::ActionKind::activate, editor::control::display_grid, !original_grid})) &&
                workspace.view.reference_grid != original_grid &&
                take(workspace.apply_view(doc, host,
                                          {ui::ActionKind::activate, editor::control::guide_grid, original_grid})) &&
                workspace.view.reference_grid == original_grid,
            "Toolbar and Guides did not update the same viewport option");

    // Slice 1B: graph.layout topological rank assignment + 1-step Undo/Redo:
    invoke("graph.layout");
    const auto laid_out = take(doc.inspect_graph(graph_key, &project));
    const auto node_y = [](const editor::GraphInspectView& gv, std::string_view name) {
        const auto it = std::ranges::find(gv.nodes, name, &editor::GraphNodeView::name);
        require(it != gv.nodes.end(), "Missing expected SOP node in layout check.");
        return it->position[1];
    };
    require(node_y(laid_out, "sphere") < node_y(laid_out, "noise") &&
                node_y(laid_out, "noise") < node_y(laid_out, "terrace") &&
                node_y(laid_out, "terrace") < node_y(laid_out, "scatter") &&
                node_y(laid_out, "scatter") < node_y(laid_out, "instancer"),
            "graph.layout did not arrange top-to-bottom SOP chain in ascending topological y order.");
    invoke("edit.undo");
    const auto layout_undone = take(doc.inspect_graph(graph_key, &project));
    require(node_y(layout_undone, "instancer") != node_y(laid_out, "instancer"),
            "edit.undo did not revert graph.layout in a single step.");
    invoke("edit.redo");
    invoke("file.save");

    // Full Workspace UI composition across 2D Network Canvas, Geometry Spreadsheet, Parameters and Scene View:
    ui::Session ui_session;
    take_ui(ui_session.initialize(take(application::editor_fonts())));
    const auto styles = take(application::editor_styles());
    take_ui(ui_session.set_style_sheet(styles));
    take_ui(ui_session.set_surface({{1400, 900}, 1.f, 1}));
    workspace.dock.select(editor::network_control::tab);
    workspace.dock.select(editor::spreadsheet_control::tab);
    const auto controls = take(workspace.describe(doc, {1400, 900}, nullptr, &project, &ui_session));
    take_ui(ui_session.set_controls(controls));
    const auto frame = take_ui(ui_session.prepare());
    require(ui_session.accept(frame), "Workspace UI frame with Network + Geometry Spreadsheet was rejected.");
    take(workspace.commit_ui_controls());
    const auto source_path = take(workspace.ui_source());
    const auto late_source = workspace.set_ui_source(temp_root / "Other.ui.source");
    require(!late_source && late_source.error().code == ErrorCode::unsupported_operation &&
                take(workspace.ui_source()) == source_path,
            "A late source change replaced the live workspace UI");
    const auto has_cjk = [](std::wstring_view text) {
        return std::ranges::any_of(text, [](wchar_t ch) { return ch >= 0x4E00 && ch <= 0x9FFF; });
    };
    const auto hud_ctrl = std::ranges::find(controls, editor::control::viewport_geometry_hud, &ui::Control::key);
    require(hud_ctrl != controls.end() && hud_ctrl->text.find(L"Pts:") != std::wstring::npos &&
                hud_ctrl->text.find(L"Inst:") != std::wstring::npos,
            "SceneViewport did not mount viewport_geometry_hud with element counts in SOP context.");
    const auto backface_btn = std::ranges::find(controls, editor::control::display_toggle_first + 5, &ui::Control::key);
    const auto sop_pts_btn = std::ranges::find(controls, editor::control::display_sop_toggle_first, &ui::Control::key);
    require(backface_btn != controls.end() && backface_btn->tooltip &&
                backface_btn->tooltip->title == L"Backface Winding Tint" &&
                has_cjk(backface_btn->tooltip->description) && sop_pts_btn != controls.end() && sop_pts_btn->tooltip &&
                sop_pts_btn->tooltip->title == L"Display Points" && has_cjk(sop_pts_btn->tooltip->description),
            "Scene View right-rail buttons are missing English-title + Chinese-explanation tooltips.");
    const auto* canvas_box = frame->find(editor::network_control::canvas);
    require(frame->find(editor::spreadsheet_control::table) != nullptr &&
                frame->find(editor::network_control::add_node) != nullptr &&
                frame->find(editor::control::display_sop_toggle_first) != nullptr && canvas_box != nullptr &&
                canvas_box->bounds.width > 64.f && canvas_box->bounds.height > 64.f,
            "Presented Workspace frame is missing Spreadsheet table, Network canvas/toolbar or SOP display toggle.");
    const auto canvas_layer = [&](ui::ControlKey key) -> const ui::CanvasData& {
        const auto at = std::ranges::find(controls, key, &ui::Control::key);
        require(at != controls.end() && at->canvas, "Missing Network canvas layer");
        return *at->canvas;
    };
    require(!canvas_layer(editor::network_control::canvas_backdrop).strokes.empty() &&
                !canvas_layer(editor::network_control::canvas_nodes).boxes.empty() &&
                !canvas_layer(editor::network_control::canvas_overlay).boxes.empty() &&
                std::ranges::any_of(frame->drawing->draws, [](const ui::DrawQuad& q) { return q.capsule_width > 0.f; }),
            "2D Network Canvas did not emit Bezier wire capsule strokes, SOP node boxes, and port connectors.");

    // 2D Network Canvas interactive gestures: wheel zoom, marquee + multi-drag, Y wire-cut, Tab search + auto-wire:
    const auto cpx = canvas_box->bounds;
    workspace.network.frame_nodes(laid_out.nodes, {cpx.width, cpx.height});
    const float zoom_before = workspace.network.zoom();
    const auto anchor_world = workspace.network.to_world(160.f, 100.f);
    {
        ui::Event wheel_ev;
        wheel_ev.kind = ui::EventKind::wheel;
        wheel_ev.x = cpx.x + 160.f;
        wheel_ev.y = cpx.y + 100.f;
        wheel_ev.wheel = 1.f;
        take(workspace.network.pointer_event(doc, graph_key, workspace.selected_graph_node,
                                             workspace.selected_graph_nodes, cpx, 1.f, wheel_ev));
    }
    const auto anchor_after = workspace.network.to_world(160.f, 100.f);
    require(workspace.network.zoom() > zoom_before && std::abs(anchor_after[0] - anchor_world[0]) < 1e-2f &&
                std::abs(anchor_after[1] - anchor_world[1]) < 1e-2f,
            "Cursor-anchored Network wheel zoom drifted off anchor world coordinate.");
    workspace.network.frame_nodes(laid_out.nodes, {cpx.width, cpx.height});

    const auto send_ptr = [&](ui::EventKind kind, float lx, float ly,
                              ui::PointerButton btn = ui::PointerButton::primary) {
        ui::Event ev;
        ev.kind = kind;
        ev.x = cpx.x + lx;
        ev.y = cpx.y + ly;
        ev.button = btn;
        return take(workspace.network.pointer_event(doc, graph_key, workspace.selected_graph_node,
                                                    workspace.selected_graph_nodes, cpx, 1.f, ev));
    };
    const auto send_key = [&](ui::EventKind kind, ui::Key k, std::uint32_t vk = 0, bool shift = false) {
        ui::Event ev;
        ev.kind = kind;
        ev.key = k;
        ev.shift = shift;
        return take(workspace.network.key_event(doc, graph_key, workspace.selected_graph_node,
                                                workspace.selected_graph_nodes, cpx, 1.f, ev, vk));
    };

    // Marquee select all 5 framed nodes, then drag them together in 1 Undo step:
    send_ptr(ui::EventKind::pointer_down, 4.f, 4.f);
    send_ptr(ui::EventKind::pointer_move, cpx.width - 4.f, cpx.height - 4.f);
    send_ptr(ui::EventKind::pointer_up, cpx.width - 4.f, cpx.height - 4.f);
    require(workspace.selected_graph_nodes.size() == 5, "Marquee box drag did not select all 5 framed SOP nodes.");

    const auto terrace_it = std::ranges::find(laid_out.nodes, "terrace", &editor::GraphNodeView::name);
    const auto terrace_dip =
        workspace.network.to_dip(terrace_it->position[0] + editor::NetworkCanvas::node_width * .35f,
                                 terrace_it->position[1] + editor::NetworkCanvas::node_height * .5f);
    send_ptr(ui::EventKind::pointer_down, terrace_dip[0], terrace_dip[1]);
    send_ptr(ui::EventKind::pointer_move, terrace_dip[0] + 48.f, terrace_dip[1] + 24.f);
    send_ptr(ui::EventKind::pointer_up, terrace_dip[0] + 48.f, terrace_dip[1] + 24.f);
    const auto after_drag = take(doc.inspect_graph(graph_key, &project));
    require(node_y(after_drag, "sphere") > node_y(laid_out, "sphere") &&
                node_y(after_drag, "instancer") > node_y(laid_out, "instancer"),
            "Multi-node drag did not move all selected SOP nodes together.");
    invoke("edit.undo");
    const auto drag_undone = take(doc.inspect_graph(graph_key, &project));
    require(std::abs(node_y(drag_undone, "sphere") - node_y(laid_out, "sphere")) < 1e-3f &&
                std::abs(node_y(drag_undone, "instancer") - node_y(laid_out, "instancer")) < 1e-3f,
            "edit.undo did not revert multi-node SOP drag in a single Undo step.");

    // Houdini LMB drag vs click: dragging an unselected node moves ONLY that node without selecting/activating it,
    // whereas clicking without dragging selects it; detached pins highlight magnetically at generous proximity:
    {
        workspace.set_single_graph_selection("terrace");
        const auto sphere_it = std::ranges::find(laid_out.nodes, "sphere", &editor::GraphNodeView::name);
        const float sphere_x0 = sphere_it->position[0];
        const float terrace_x0 = terrace_it->position[0];
        const auto sphere_body =
            workspace.network.to_dip(sphere_it->position[0] + editor::NetworkCanvas::node_width * .45f,
                                     sphere_it->position[1] + editor::NetworkCanvas::node_height * .5f);
        send_ptr(ui::EventKind::pointer_down, sphere_body[0], sphere_body[1]);
        send_ptr(ui::EventKind::pointer_move, sphere_body[0] + 40.f, sphere_body[1]);
        send_ptr(ui::EventKind::pointer_up, sphere_body[0] + 40.f, sphere_body[1]);
        const auto after_unsel_drag = take(doc.graph_topology(graph_key));
        require(after_unsel_drag->find_node("sphere")->position[0] > sphere_x0 + 10.f &&
                    std::abs(after_unsel_drag->find_node("terrace")->position[0] - terrace_x0) < 1e-3f &&
                    workspace.selected_graph_node == "terrace" && workspace.selected_graph_nodes.size() == 1,
                "Dragging an unselected SOP node changed graph selection or moved unrelated selected nodes.");
        invoke("edit.undo");
        require(std::abs(take(doc.graph_topology(graph_key))->find_node("sphere")->position[0] - sphere_x0) < 1e-3f,
                "edit.undo did not revert unselected SOP node drag.");

        send_ptr(ui::EventKind::pointer_down, sphere_body[0], sphere_body[1]);
        send_ptr(ui::EventKind::pointer_up, sphere_body[0], sphere_body[1]);
        require(workspace.selected_graph_node == "sphere" &&
                    workspace.selected_graph_nodes == std::vector<std::string>{"sphere"},
                "Clicking an unselected SOP node without dragging did not select it.");

        const auto out_pin = workspace.network.port_dip(*sphere_it, true, 0);
        const auto sphere_bottom = workspace.network.to_dip(
            sphere_it->position[0], sphere_it->position[1] + editor::NetworkCanvas::node_height)[1];
        require(std::abs((out_pin[1] - sphere_bottom) - editor::NetworkCanvas::pin_offset * workspace.network.zoom()) <
                    1e-2f,
                "Output pin dot is not detached outside the node bottom edge.");
        const auto prox_move = send_ptr(ui::EventKind::pointer_move, out_pin[0] + 11.f, out_pin[1] + 5.f);
        require(prox_move.cursor == ui::Cursor::crosshair,
                "Detached pin port did not magnetically highlight within generous cursor proximity.");
    }

    // Y + LMB scissor wire-cut across scatter -> instancer Bezier wire, then 1-step Undo:
    const auto scatter_it = std::ranges::find(laid_out.nodes, "scatter", &editor::GraphNodeView::name);
    const auto inst_it = std::ranges::find(laid_out.nodes, "instancer", &editor::GraphNodeView::name);
    const auto p_from = workspace.network.port_dip(*scatter_it, true, 0);
    const auto p_to = workspace.network.port_dip(*inst_it, false, 0);
    const float mid_x = (p_from[0] + p_to[0]) * .5f;
    const float mid_y = (p_from[1] + p_to[1]) * .5f;
    send_key(ui::EventKind::key_down, ui::Key::y, 'Y');
    send_ptr(ui::EventKind::pointer_down, mid_x - 45.f, mid_y);
    send_ptr(ui::EventKind::pointer_move, mid_x + 45.f, mid_y);
    send_ptr(ui::EventKind::pointer_up, mid_x + 45.f, mid_y);
    send_key(ui::EventKind::key_up, ui::Key::y, 'Y');
    require(take(doc.inspect_graph(graph_key, &project)).connections.size() == 3,
            "Y + drag scissor wire-cut did not disconnect the scatter -> instancer wire.");
    invoke("edit.undo");
    require(take(doc.inspect_graph(graph_key, &project)).connections.size() == 4,
            "edit.undo did not restore Y-cut SOP wire in a single Undo step.");

    // Floating Tab quick-create search + Shift+Enter auto-wire + Display Flag in 1 Undo step:
    workspace.set_single_graph_selection("terrace");
    const auto tab_opened = send_key(ui::EventKind::key_down, ui::Key::tab);
    require(workspace.network.tab_search().open && tab_opened.focus_tab_search,
            "Tab hotkey did not open the floating SOP quick-create popup.");
    workspace.network.set_tab_query(L"Transform");
    send_key(ui::EventKind::key_down, ui::Key::enter, 0, true);
    const auto after_tab = take(doc.inspect_graph(graph_key, &project));
    require(!workspace.network.tab_search().open && workspace.selected_graph_node == "transform1" &&
                after_tab.display == "transform1" && after_tab.connections.size() == 5 &&
                std::ranges::any_of(after_tab.connections,
                                    [](const editor::GraphConnectionView& c) {
                                        return c.from_node == "terrace" && c.to_node == "transform1";
                                    }),
            "Tab quick-create Shift+Enter did not place, auto-wire, and set Display Flag on transform1.");
    // Search by purpose and inspect the tooltip actually delivered to the Tab popup.
    send_key(ui::EventKind::key_down, ui::Key::tab);
    workspace.network.set_tab_query(L"抬高台地");
    const auto purpose_matches = workspace.network.filter_catalog(catalog);
    require(purpose_matches.size() == 1 && purpose_matches.front().name == "game.TerraceWave",
            "Tab search did not find the Source node by its reflected purpose.");
    const auto tab_controls = take(workspace.describe(doc, {1400, 900}, frame.get(), &project, &ui_session));
    const auto& purpose = purpose_matches.front().description;
    require(std::ranges::any_of(tab_controls,
                                [&](const auto& ctrl) {
                                    return ctrl.text == L"TerraceWave" && ctrl.tooltip &&
                                           ctrl.tooltip->description == platform::utf8_to_utf16_validated(purpose);
                                }),
            "The Tab result tooltip did not show the Source catalog description.");
    take_ui(ui_session.set_controls(tab_controls));
    const auto tab_frame = take_ui(ui_session.prepare());
    require(ui_session.accept(tab_frame) && tab_frame->find(editor::network_control::tab_search_input) != nullptr,
            "Open Tab quick-create popup was rejected by UI layout or missing search field.");
    take(workspace.commit_ui_controls());
    workspace.network.close_tab_search();

    invoke("edit.undo");
    const auto tab_undone = take(doc.inspect_graph(graph_key, &project));
    require(doc.graph_node_id(graph_key, "transform1") == ObjectId{} && tab_undone.connections.size() == 4 &&
                tab_undone.display == "instancer",
            "edit.undo did not revert Tab node placement + auto-wire + Display Flag in a single Undo step.");

    invoke("graph.open", "{\"asset\":\"" + repair_key + "\"}");
    invoke("graph.select", R"({"node":"legacy"})");
    auto repair_controls = take(workspace.describe(doc, {1400, 900}, frame.get(), &project, &ui_session));
    take_ui(ui_session.set_controls(repair_controls));
    require(workspace.selected_graph_node == "legacy", "Workspace discarded a selected unresolved node.");
    invoke("graph.delete_node");
    require(!doc.graph_node(repair_key, "legacy"), "Could not delete an unresolved node.");
    invoke("edit.undo");
    require(doc.graph_node(repair_key, "legacy"), "Undo did not restore the placeholder.");

    // 33 x 32 connections fit on screen and exceed the capacity of a single stroke batch.
    std::string dense = R"({"version":4,"settings":{"display":"fault"},"nodes":{)";
    std::string inputs;
    for (unsigned i = 0; i < 33; ++i) {
        const auto name = "grid" + std::to_string(i);
        dense += "\"" + name + R"(":{"type":"procedural.Grid","position":[)" + std::to_string(i * 8 + 12) + R"(,0]},)";
        if (i)
            inputs += ',';
        inputs += "\"" + name + ".output\"";
    }
    for (unsigned i = 0; i < 32; ++i)
        dense += "\"merge" + std::to_string(i) +
                 R"(":{"type":"procedural.Merge","position":[0,160],"inputs":{"inputs":[)" + inputs + "]}},";
    dense += R"("fault":{"type":"game.DiagnosticSop","position":[210,80],"inputs":{"input":"grid0.output"}}}})";
    const auto dense_key = take(project.create_procedural_graph(L"Content/Graphs/Dense.procgraph", dense));
    invoke("graph.open", "{\"asset\":\"" + dense_key + "\"}");
    const auto error_cook = take(doc.evaluate_graph(dense_key, &project));
    require(error_cook.display_error &&
                error_cook.display_error->message.find(std::string("A\0B", 3)) != std::string::npos,
            "The original Source diagnostic was not retained.");
    const auto dense_controls = take(workspace.describe(doc, {1400, 900}, frame.get(), &project, &ui_session));
    std::size_t wire_segments = 0;
    ui::ControlKey fault_title{};
    for (const auto& ctrl : dense_controls) {
        if (ctrl.canvas)
            wire_segments += ctrl.canvas->strokes.size();
        if (ctrl.tooltip && ctrl.text == L"fault" && ctrl.tooltip->description.find(L"A\u2400B") != std::wstring::npos)
            fault_title = ctrl.key;
    }
    take_ui(ui_session.set_controls(dense_controls));
    auto dense_frame = take_ui(ui_session.prepare());
    require(ui_session.accept(dense_frame) && wire_segments > ui::CanvasData::max_strokes && fault_title,
            "Dense wire batches or the Source error tooltip did not reach the accepted frame.");
    take(workspace.commit_ui_controls());
    const auto* box = dense_frame->find(fault_title);
    require(box != nullptr, "Missing fault node hit region.");
    ui::Event hover;
    hover.kind = ui::EventKind::pointer_move;
    hover.x = box->bounds.x + box->bounds.width * .5f;
    hover.y = box->bounds.y + box->bounds.height * .5f;
    ui_session.input(hover);
    const auto next = take(workspace.describe(doc, {1400, 900}, dense_frame.get(), &project, &ui_session));
    take_ui(ui_session.set_controls(next));
    require(ui_session.accept(take_ui(ui_session.prepare())), "Dense frame refresh failed.");
    std::this_thread::sleep_for(std::chrono::milliseconds(210));
    const auto help_frame = take_ui(ui_session.prepare());
    require(help_frame->tooltip && help_frame->tooltip->owner == fault_title,
            "Node hover tooltip did not open promptly (~180ms) after canvas refresh.");

    // Phase B: Bypass pass-through on failing Source SOP ('fault'), 1-step Undo, and multi-input port-1 skip:
    invoke("graph.set_bypass", R"({"node":"fault","bypass":true})");
    const auto bypassed_cook = take(doc.evaluate_graph(dense_key, &project));
    require(!bypassed_cook.display_error && bypassed_cook.display_output != nullptr &&
                bypassed_cook.display_output->count(procedural::Layer::point) > 0,
            "Bypassing failing SOP 'fault' did not skip execution and pass grid0.output straight through.");
    invoke("edit.undo");
    require(take(doc.evaluate_graph(dense_key, &project)).display_error.has_value(),
            "edit.undo did not restore Bypass state on 'fault'.");
    {
        const auto ray_bypass = take(doc.graph_add_node(dense_key, "procedural.Ray", "ray_byp", {200.f, 240.f}));
        take(doc.graph_connect(dense_key, "grid0", "output", ray_bypass, "points"));
        take(doc.graph_connect(dense_key, "fault", "output", ray_bypass, "surface"));
        take(doc.graph_set_display(dense_key, ray_bypass));
        require(take(doc.evaluate_graph(dense_key, &project)).display_error.has_value(),
                "Unbypassed Ray with failing port-1 surface should fail.");
        take(doc.graph_set_bypass(dense_key, ray_bypass, true));
        const auto ray_byp_eval = take(doc.evaluate_graph(dense_key, &project));
        require(!ray_byp_eval.display_error && ray_byp_eval.display_output != nullptr &&
                    ray_byp_eval.display_output->count(procedural::Layer::point) > 0,
                "Bypassed multi-input Ray still cooked failing port-1 surface instead of passing port-0 through.");
    }

    // Phase B: Null milestone node, Drag-onto-wire Auto-Splice, Template Flag wireframe, Shapes/Colors/Notes/Boxes:
    invoke("graph.open", "{\"asset\":\"" + graph_key + "\"}");
    workspace.network.frame_nodes(take(doc.graph_topology(graph_key))->nodes, {cpx.width, cpx.height});

    // Acronym Tab search ranking ('ctp' -> CopyToPoints, 'vs' -> VolumeScatter):
    workspace.network.open_tab_search(80.f, 80.f, {cpx.width, cpx.height});
    workspace.network.set_tab_query(L"ctp");
    const auto ctp_matches = workspace.network.filter_catalog(catalog);
    require(!ctp_matches.empty() && ctp_matches.front().name == "procedural.CopyToPoints",
            "Acronym query 'ctp' did not rank procedural.CopyToPoints first.");
    workspace.network.set_tab_query(L"vs");
    const auto vs_matches = workspace.network.filter_catalog(catalog);
    require(!vs_matches.empty() && vs_matches.front().name == "procedural.VolumeScatter",
            "Acronym query 'vs' did not rank procedural.VolumeScatter first.");
    workspace.network.close_tab_search();

    // Add procedural.Null unconnected, then drag it directly onto scatter.points -> instancer.points wire to auto-splice:
    const auto null_name = take(doc.graph_add_node(graph_key, "procedural.Null", "OUT_pts", {-240.f, 180.f}));
    const auto before_splice = take(doc.graph_topology(graph_key));
    const auto* sc_v = before_splice->find_node("scatter");
    const auto* in_v = before_splice->find_node("instancer");
    const auto* nl_v = before_splice->find_node(null_name);
    require(sc_v && in_v && nl_v, "Missing scatter, instancer, or OUT_pts node.");
    const auto wire_mid_dip = [&]() {
        const auto p0 = workspace.network.port_dip(*sc_v, true, 0);
        const auto p1 = workspace.network.port_dip(*in_v, false, 0);
        return std::array{(p0[0] + p1[0]) * .5f, (p0[1] + p1[1]) * .5f};
    }();
    const auto nl_center_dip = workspace.network.to_dip(nl_v->position[0] + editor::NetworkCanvas::node_width * .45f,
                                                        nl_v->position[1] + editor::NetworkCanvas::node_height * .5f);
    send_ptr(ui::EventKind::pointer_down, nl_center_dip[0], nl_center_dip[1]);
    send_ptr(ui::EventKind::pointer_move, wire_mid_dip[0], wire_mid_dip[1]);
    send_ptr(ui::EventKind::pointer_up, wire_mid_dip[0], wire_mid_dip[1]);
    const auto after_splice = take(doc.inspect_graph(graph_key, &project));
    require(after_splice.connections.size() == 5 && !after_splice.display_error &&
                after_splice.display_summary.counts[static_cast<std::size_t>(procedural::Layer::instance)] ==
                    inst_count &&
                std::ranges::any_of(after_splice.connections,
                                    [&](const editor::GraphConnectionView& c) {
                                        return c.from_node == "scatter" && c.to_node == null_name;
                                    }) &&
                std::ranges::any_of(after_splice.connections,
                                    [&](const editor::GraphConnectionView& c) {
                                        return c.from_node == null_name && c.to_node == "instancer";
                                    }),
            "Dragging unconnected Null node onto wire did not auto-splice it cleanly.");
    invoke("edit.undo");
    const auto splice_undone = take(doc.graph_topology(graph_key));
    require(splice_undone->connections.size() == 4 &&
                std::abs(splice_undone->find_node(null_name)->position[0] - (-240.f)) < 1e-3f,
            "1-step edit.undo did not revert both node position and spliced wire connections.");
    invoke("edit.undo"); // Remove OUT_pts

    // Template Flag evaluation + ghosted wireframe overlay:
    invoke("graph.set_template", R"({"node":"sphere"})");
    const auto tmpl_eval = take(doc.evaluate_graph(graph_key, &project));
    require(tmpl_eval.template_output != nullptr && tmpl_eval.template_name == "sphere",
            "Template Flag did not evaluate sphere reference geometry alongside Display output.");
    {
        editor::Camera vcam;
        auto vview = editor::ViewportView::make(vcam, {0.f, 0.f, 800.f, 600.f}, 1.f);
        std::vector<renderer::OverlayShape> tmpl_overlay;
        application::append_template_wireframe(*tmpl_eval.template_output, vview, tmpl_overlay);
        require(!tmpl_overlay.empty(), "append_template_wireframe emitted no ghosted wireframe segments.");

        require(vcam.look_at({0, 0, 0}, {0, 0, -1}), "Failed to align the template inspection camera.");
        vview = editor::ViewportView::make(vcam, {30, 20, 800, 600}, 1.f);
        const auto center = vview.eye + vview.forward;
        const auto span = vview.right * 100000.f;
        const auto endpoints = take(procedural::Geometry::points({center - span, center + span}));
        tmpl_overlay.clear();
        application::append_template_wireframe(endpoints, vview, tmpl_overlay);
        require(tmpl_overlay.empty(), "Off-screen template points reached the renderer.");
        application::append_template_wireframe(take(endpoints.connect_points()), vview, tmpl_overlay);
        require(tmpl_overlay.size() == 1, "Lost a template line spanning the entire viewport.");
        const auto horizontal = tmpl_overlay.front().points;
        require(horizontal[0] >= 27.f && horizontal[0] <= 30.01f && horizontal[2] >= 829.99f &&
                    horizontal[2] <= 833.f && std::abs(horizontal[1] - 320.f) < .01f &&
                    std::abs(horizontal[3] - 320.f) < .01f,
                "Template line endpoints must lie at the viewport edges within the stroke margin.");

        const auto behind = vview.eye - vview.forward - vview.right * .02f;
        const auto depth_crossing = take(procedural::Geometry::points({behind, center}));
        tmpl_overlay.clear();
        application::append_template_wireframe(take(depth_crossing.connect_points()), vview, tmpl_overlay);
        require(tmpl_overlay.size() == 1, "Lost the visible part of a template line crossing the near plane.");
        const auto depth_line = tmpl_overlay.front().points;
        require(depth_line[0] > 30.f && depth_line[0] < 420.f && std::abs(depth_line[2] - 430.f) < .01f &&
                    std::abs(depth_line[1] - 320.f) < .01f && std::abs(depth_line[3] - 320.f) < .01f,
                "Near-plane clipping must terminate inside the view and preserve the visible endpoint.");
    }

    // Node Appearance (Shape + Color + Comment) + Sticky Note + Network Box group drag in 1 Undo step:
    invoke("graph.set_appearance", R"({"node":"terrace","shape":"ribbon","color":"#eab308","comment":"Plateaus"})");
    const auto note_id =
        take(doc.graph_add_note(graph_key, {220.f, 20.f}, {200.f, 100.f}, "Guide", "Line A\nLine B", 0xd4a72c));
    const auto top_now = take(doc.graph_topology(graph_key));
    const auto* sp_before = top_now->find_node("sphere");
    require(sp_before != nullptr && top_now->find_node("terrace")->shape == editor::GraphNodeShape::ribbon &&
                top_now->find_node("terrace")->comment == "Plateaus",
            "graph.set_appearance did not persist shape and comment.");
    const auto box_id = take(doc.graph_add_box(
        graph_key, {sp_before->position[0] - 30.f, sp_before->position[1] - 36.f}, {200.f, 110.f}, "Source Box"));
    const float sp_y0 = sp_before->position[1];
    const auto with_box = take(doc.graph_topology(graph_key));
    workspace.network.frame_nodes(with_box->nodes, {cpx.width, cpx.height}, {}, with_box->boxes, with_box->notes);
    const auto* box_view = with_box->find_box(box_id);
    const auto box_hdr_dip = workspace.network.to_dip(box_view->position[0] + 40.f, box_view->position[1] + 8.f);
    send_ptr(ui::EventKind::pointer_down, box_hdr_dip[0], box_hdr_dip[1]);
    send_ptr(ui::EventKind::pointer_move, box_hdr_dip[0] + 36.f, box_hdr_dip[1] + 28.f);
    send_ptr(ui::EventKind::pointer_up, box_hdr_dip[0] + 36.f, box_hdr_dip[1] + 28.f);
    const auto after_box_drag = take(doc.graph_topology(graph_key));
    require(after_box_drag->find_node("sphere")->position[1] > sp_y0 + 5.f,
            "Dragging Network Box header did not move enclosed 'sphere' node together.");
    invoke("edit.undo");
    require(std::abs(take(doc.graph_topology(graph_key))->find_node("sphere")->position[1] - sp_y0) < 1e-3f,
            "1-step edit.undo did not revert Network Box group drag.");

    // Hover node on NetworkCanvas -> verify ProceduralQuad::node_shape + ProceduralQuad::arc_sector (Air-Ring + Halos),
    // and verify Z / C / I floating popups mount cleanly in UI layout:
    {
        const auto cur_top = take(doc.graph_topology(graph_key));
        const std::array focus_sphere{std::string{"sphere"}};
        workspace.network.frame_nodes(cur_top->nodes, {cpx.width, cpx.height}, focus_sphere);
        const auto* sph = cur_top->find_node("sphere");
        const auto sph_dip = workspace.network.to_dip(sph->position[0] + editor::NetworkCanvas::node_width * .45f,
                                                      sph->position[1] + editor::NetworkCanvas::node_height * .5f);
        send_ptr(ui::EventKind::pointer_move, sph_dip[0], sph_dip[1]);
        const auto b_controls = take(workspace.describe(doc, {1400, 900}, frame.get(), &project, &ui_session));
        take_ui(ui_session.set_controls(b_controls));
        const auto b_frame = take_ui(ui_session.prepare());
        require(ui_session.accept(b_frame) &&
                    std::ranges::any_of(
                        b_frame->drawing->draws,
                        [](const ui::DrawQuad& q) { return q.procedural == ui::ProceduralQuad::node_shape; }) &&
                    std::ranges::any_of(
                        b_frame->drawing->draws,
                        [](const ui::DrawQuad& q) { return q.procedural == ui::ProceduralQuad::arc_sector; }),
                "NetworkCanvas did not emit analytic SDF node_shape and arc_sector DrawQuads.");
        take(workspace.commit_ui_controls());

        for (const std::uint32_t vk : {'Z', 'C', 'I'}) {
            send_key(ui::EventKind::key_down, ui::Key::none, vk);
            require(workspace.network.popup().open(), "Shortcut Z/C/I did not open floating Network popup.");
            const auto pop_controls = take(workspace.describe(doc, {1400, 900}, b_frame.get(), &project, &ui_session));
            take_ui(ui_session.set_controls(pop_controls));
            const auto pop_frame = take_ui(ui_session.prepare());
            require(ui_session.accept(pop_frame) && pop_frame->find(editor::network_control::popup_box) != nullptr,
                    "Floating Network popup (Shape/Color/Info) failed UI validation or layout.");
            take(workspace.commit_ui_controls());
            workspace.network.close_popup();
        }

        // Air-Ring bottom-left Freeze (Snowflake) locks cooked geometry across upstream edits + Undo/Redo:
        {
            const auto pre_freeze_eval = take(doc.evaluate_graph(graph_key, &project));
            require(pre_freeze_eval.display_output != nullptr, "Missing display output before Freeze test.");
            const std::size_t frozen_inst_count = pre_freeze_eval.display_output->count(procedural::Layer::instance);
            const auto cur_t2 = take(doc.graph_topology(graph_key));
            const std::array focus_terrace{std::string{"terrace"}};
            workspace.network.frame_nodes(cur_t2->nodes, {cpx.width, cpx.height}, focus_terrace);
            const auto* ter = cur_t2->find_node("terrace");
            const auto ter_center =
                workspace.network.to_dip(ter->position[0] + editor::NetworkCanvas::node_width * .5f,
                                         ter->position[1] + editor::NetworkCanvas::node_height * .5f);
            send_ptr(ui::EventKind::pointer_move, ter_center[0], ter_center[1]);
            const float z_air = workspace.network.zoom();
            const float r_mid =
                (editor::NetworkCanvas::air_inner_radius + editor::NetworkCanvas::air_outer_radius) * .5f * z_air;
            const float rad_freeze = 140.f * (std::numbers::pi_v<float> / 180.f);
            const float fx = ter_center[0] + std::cos(rad_freeze) * r_mid;
            const float fy = ter_center[1] + std::sin(rad_freeze) * r_mid;
            send_ptr(ui::EventKind::pointer_move, fx, fy);
            send_ptr(ui::EventKind::pointer_down, fx, fy);
            send_ptr(ui::EventKind::pointer_up, fx, fy);
            require(take(doc.graph_topology(graph_key))->find_node("terrace")->is_frozen,
                    "Clicking Air-Ring bottom-left Snowflake sector did not freeze 'terrace'.");

            {
                json::Builder patch;
                static_cast<void>(patch.set(patch.root(), "radius", 0.05));
                take(doc.graph_set_properties(graph_key, "sphere", patch.root()));
            }
            const auto while_frozen = take(doc.evaluate_graph(graph_key, &project));
            require(!while_frozen.display_error && while_frozen.display_output != nullptr &&
                        while_frozen.display_output->count(procedural::Layer::instance) == frozen_inst_count,
                    "Frozen SOP node 'terrace' changed output when upstream 'sphere' radius shrunk to 0.05.");

            invoke("graph.set_frozen", R"({"node":"terrace","frozen":false})");
            const auto after_unfreeze = take(doc.evaluate_graph(graph_key, &project));
            require(after_unfreeze.display_output != nullptr &&
                        after_unfreeze.display_output->count(procedural::Layer::instance) < frozen_inst_count,
                    "Unfreezing 'terrace' did not resume live evaluation against shrunk upstream 'sphere'.");
            invoke("edit.undo"); // Undoes unfreeze -> restores Freeze
            const auto restored = take(doc.evaluate_graph(graph_key, &project));
            require(take(doc.graph_topology(graph_key))->find_node("terrace")->is_frozen && restored.display_output &&
                        restored.display_output->count(procedural::Layer::instance) == frozen_inst_count,
                    "edit.undo did not restore the frozen geometry on 'terrace'.");
            invoke("edit.undo"); // Undoes sphere radius = 0.05
            invoke("edit.undo"); // Undoes initial Freeze
            require(
                !take(doc.graph_topology(graph_key))->find_node("terrace")->is_frozen &&
                    take(doc.evaluate_graph(graph_key, &project)).display_output->count(procedural::Layer::instance) ==
                        frozen_inst_count,
                "Undo chain did not cleanly revert Freeze + upstream edit.");
        }

        // Strict #RRGGBB format validation + unresolved placeholder display/template cleanup + popup auto-close:
        require(!editor::parse_graph_hex_color("eab308").has_value() &&
                    editor::parse_graph_hex_color("#eab308") == 0xeab308u,
                "parse_graph_hex_color must require a leading '#' and 6 hex digits.");
        {
            send_ptr(ui::EventKind::pointer_move, 2.f, 2.f);
            const auto temp_node = take(doc.graph_add_node(graph_key, "procedural.Null", "tmp_pop", {320.f, 0.f}));
            workspace.selected_graph_node = temp_node;
            workspace.selected_graph_nodes = {temp_node};
            send_key(ui::EventKind::key_down, ui::Key::none, 'C');
            require(workspace.network.popup().open(), "Color picker popup did not open for tmp_pop.");
            invoke("edit.undo"); // Deletes tmp_pop while popup is open
            const auto after_pop_undo =
                take(workspace.describe(doc, {1400, 900}, b_frame.get(), &project, &ui_session));
            take_ui(ui_session.set_controls(after_pop_undo));
            require(ui_session.accept(take_ui(ui_session.prepare())) && !workspace.network.popup().open(),
                    "Deleting popup target via Undo did not auto-close the stale floating popup.");
            take(workspace.commit_ui_controls());
        }
        {
            const auto ghost_key = take(project.create_procedural_graph(L"Content/Graphs/GhostTest.procgraph", R"({
  "version": 4,
  "settings": { "display": "ghost", "template": "ghost" },
  "nodes": { "ghost": { "type": "missing.CustomSop", "position": [0, 0] } }
})"));
            take(doc.open_graph(project, ghost_key));
            require(take(doc.graph_topology(ghost_key))->display == "ghost" &&
                        take(doc.graph_topology(ghost_key))->template_node == "ghost",
                    "graph_topology did not report kept placeholder display/template.");
            const std::array rm_ghost{std::string{"ghost"}};
            take(doc.graph_remove_nodes(ghost_key, rm_ghost));
            const auto cleaned_snap = take(doc.procedural_graph_snapshot(ghost_key)).value_or("");
            require(!cleaned_snap.empty() && cleaned_snap.find("ghost") == std::string::npos,
                    "Removing placeholder node left dangling display/template entry in kept settings.");
            take(doc.graph_undo(ghost_key, false));
            require(!doc.graph_dirty(ghost_key) && take(doc.graph_topology(ghost_key))->display == "ghost" &&
                        take(doc.graph_topology(ghost_key))->template_node == "ghost",
                    "graph_undo did not restore kept display/template after placeholder deletion.");
        }

        // Shift+D interactive node duplicate + grab (1-step Undo/Redo), Ctrl+I invert, and X delete:
        {
            const auto disp_before_dup = take(doc.graph_topology(graph_key))->display;
            workspace.set_single_graph_selection("scatter");
            send_ptr(ui::EventKind::pointer_move, 140.f, 120.f);
            send_key(ui::EventKind::key_down, ui::Key::none, 'D', true);
            require(workspace.network.active_gesture() && workspace.selected_graph_nodes.size() == 1 &&
                        workspace.selected_graph_node == "scatter1" &&
                        take(doc.graph_topology(graph_key))->display == disp_before_dup,
                    "Shift+D did not duplicate scatter into interactive grab without altering Display Flag.");
            send_ptr(ui::EventKind::pointer_move, 220.f, 170.f);
            send_ptr(ui::EventKind::pointer_down, 220.f, 170.f);
            require(!workspace.network.active_gesture(), "Primary click did not commit Shift+D node placement.");
            const auto after_dup_topo = take(doc.graph_topology(graph_key));
            const auto* dup_scatter = after_dup_topo->find_node("scatter1");
            const bool kept_upstream = std::ranges::any_of(after_dup_topo->connections, [](const auto& c) {
                return c.to_node == "scatter1" && c.from_node == "terrace";
            });
            require(dup_scatter != nullptr && kept_upstream,
                    "Duplicated scatter1 lost its upstream terrace connection.");
            const auto placed_pos = dup_scatter->position;
            take(doc.graph_undo(graph_key, false));
            require(take(doc.graph_topology(graph_key))->find_node("scatter1") == nullptr,
                    "Shift+D + LMB placement was not undone as a single graph history step.");
            take(doc.graph_undo(graph_key, true));
            const auto* redone_scatter = take(doc.graph_topology(graph_key))->find_node("scatter1");
            require(redone_scatter != nullptr && redone_scatter->position == placed_pos,
                    "Redo of Shift+D did not restore scatter1 at its amended placement coordinates.");

            // Delete scatter1 via X shortcut and verify graph returns to pre-duplicate state:
            workspace.set_single_graph_selection("scatter1");
            send_key(ui::EventKind::key_down, ui::Key::x, 'X');
            require(take(doc.graph_topology(graph_key))->find_node("scatter1") == nullptr,
                    "X shortcut did not delete selected SOP node.");
        }
    }

    // Save v2 .procgraph with notes, boxes, template, shape, color & comment and verify clean state:
    invoke("file.save");
    require(!doc.graph_dirty(graph_key) && !note_id.empty(),
            "Saving .procgraph v2 with annotations, template and styles left graph dirty.");

    invoke("graph.exit");
    require(workspace.active_graph.empty(), "graph.exit did not return Scene View context to World.");

    // Scratch graphs cannot be bound to persistent Scene ProceduralGeneratorComponents:
    take(doc.create_scratch_graph("scratch/procedural"));
    require(!doc.create_procedural_generator("scratch/procedural", {}, &project),
            "Session allowed binding a scratch procedural graph to a Scene Entity.");

    // S5-1: Scene Procedural Generator Entity creation, Inspector UI, live .procgraph sync, assign_graph, and Bake:
    invoke("scene.add", "{\"kind\":\"procedural_generator\",\"graph\":\"" + graph_key + "\"}");
    const ObjectId gen_ent = doc.selection().entity;
    require(gen_ent != ObjectId{} &&
                take(doc.world().runtime().type_of(gen_ent)) == doc.presentation_types().procedural_generator_entity,
            "scene.add procedural_generator did not create a ProceduralGeneratorEntity.");
    // Long name ending in a supplementary-plane surrogate pair to verify UTF-16 safe truncation on Bake:
    std::wstring long_surrogate_name(251, L'A');
    long_surrogate_name += L"\U0001F3DD"; // 2 UTF-16 code units -> + " Baked" exceeds 256 at surrogate boundary.
    take(doc.rename(gen_ent, long_surrogate_name));
    take(doc.sync_procedural_generators(&project));
    const auto world_items_initial = take(doc.presentation().render_items());
    require(world_items_initial.size() == inst_count,
            "ProceduralGeneratorEntity did not publish expected CopyToPoints render_items in World.");

    // Verify Parameters pane mounts ProceduralGeneratorComponent controls cleanly:
    workspace.dock.select(editor::control::parameters_tab);
    const auto gen_controls = take(workspace.describe(doc, {1400, 900}, nullptr, &project, &ui_session));
    take_ui(ui_session.set_controls(gen_controls));
    const auto gen_frame = take_ui(ui_session.prepare());
    require(ui_session.accept(gen_frame) &&
                std::ranges::any_of(gen_controls, [](const ui::Control& c) { return c.text == L"Procedural Graph"; }) &&
                std::ranges::any_of(gen_controls, [](const ui::Control& c) { return c.text == L"Material · default"; }),
            "Parameters pane did not mount ProceduralGeneratorComponent graph and material selectors.");
    take(workspace.commit_ui_controls());

    // Verify World-context ProceduralGeneratorEntity populates GeometrySpreadsheet and SceneViewport HUD/guides:
    workspace.dock.select(editor::spreadsheet_control::tab);
    workspace.dock.select(editor::control::scene_tab);
    workspace.spreadsheet.set_layer(procedural::Layer::instance);
    const auto world_gen_ui = take(workspace.describe(doc, {1400, 900}, gen_frame.get(), &project, &ui_session));
    take_ui(ui_session.set_controls(world_gen_ui));
    const auto world_gen_frame = take_ui(ui_session.prepare());
    require(
        ui_session.accept(world_gen_frame) && workspace.active_graph.empty() &&
            workspace.spreadsheet.total_rows() == inst_count &&
            std::ranges::any_of(world_gen_ui,
                                [](const ui::Control& c) {
                                    return c.key == editor::control::viewport_geometry_hud &&
                                           c.text.find(L"[Generator]") != std::wstring::npos &&
                                           c.text.find(L"Chunks:") != std::wstring::npos;
                                }) &&
            std::ranges::any_of(
                world_gen_ui, [](const ui::Control& c) { return c.key == editor::control::display_sop_toggle_first; }),
        "Selecting a ProceduralGeneratorEntity in World context did not expose GeometrySpreadsheet rows, HUD, and SOP guide toggles.");
    workspace.spreadsheet.set_layer(procedural::Layer::point);
    take(workspace.commit_ui_controls());

    // Verify clicking ProceduralGeneratorEntity in World immediately populates Network pane & allows live node inspection:
    {
        workspace.dock.select(editor::network_control::tab);
        const auto world_net_ui =
            take(workspace.describe(doc, {1400, 900}, world_gen_frame.get(), &project, &ui_session));
        take_ui(ui_session.set_controls(world_net_ui));
        const auto world_net_frame = take_ui(ui_session.prepare());
        const auto path_it = std::ranges::find(world_net_ui, editor::network_control::path_combo, &ui::Control::key);
        const auto menu_add_it = std::ranges::find(world_net_ui, editor::network_control::menu_add, &ui::Control::key);
        const auto menu_edit_it =
            std::ranges::find(world_net_ui, editor::network_control::menu_edit, &ui::Control::key);
        const auto add_it = std::ranges::find(world_net_ui, editor::network_control::add_node, &ui::Control::key);
        const auto nodes_it = std::ranges::find(world_net_ui, editor::network_control::canvas_nodes, &ui::Control::key);
        require(
            ui_session.accept(world_net_frame) && workspace.active_graph.empty() && path_it != world_net_ui.end() &&
                path_it->text.starts_with(L"World  ▸") && menu_add_it != world_net_ui.end() &&
                std::ranges::find(menu_add_it->classes, "viewport-header-menu") != menu_add_it->classes.end() &&
                menu_edit_it != world_net_ui.end() &&
                std::ranges::find(menu_edit_it->classes, "viewport-header-menu") != menu_edit_it->classes.end() &&
                add_it != world_net_ui.end() && add_it->icon == ui::ControlIcon::plus && add_it->text.empty() &&
                std::ranges::find(add_it->classes, "ghost-tool") != add_it->classes.end() &&
                nodes_it != world_net_ui.end() && nodes_it->canvas && !nodes_it->canvas->shapes.empty(),
            "Selecting a ProceduralGeneratorEntity in World did not display its Network canvas, air-state menus, and ghost icon toolbar.");
        take(workspace.commit_ui_controls());

        // Verify Blender-style hover/enter DockSpace::focus_point switches focused_tab across visible panes without clicking:
        const auto* vp_pane = world_net_frame->find(workspace.dock.pane(editor::control::scene_tab));
        const auto* net_pane = world_net_frame->find(workspace.dock.pane(editor::network_control::tab));
        require(vp_pane != nullptr && net_pane != nullptr, "Expected both Scene View and Network panes to be visible.");
        require(workspace.dock.focus_point(vp_pane->bounds.x + vp_pane->bounds.width * .5f,
                                           vp_pane->bounds.y + vp_pane->bounds.height * .5f, *world_net_frame) &&
                    workspace.dock.focused_tab() == editor::control::scene_tab,
                "DockSpace::focus_point did not focus Scene View on hover.");
        require(workspace.dock.focus_point(net_pane->bounds.x + net_pane->bounds.width * .5f,
                                           net_pane->bounds.y + net_pane->bounds.height * .5f, *world_net_frame) &&
                    workspace.dock.focused_tab() == editor::network_control::tab,
                "DockSpace::focus_point did not focus Network pane on hover.");

        // Verify SceneSnapshot suppresses interior selection wireframe on ProceduralGeneratorEntity while Network is visible:
        {
            application::SceneSnapshot snap_check;
            take(snap_check.refresh(doc, true, false));
            require(!snap_check.meshes.empty() &&
                        std::ranges::none_of(snap_check.meshes, [](const auto& m) { return m.selected; }),
                    "SceneSnapshot highlighted ProceduralGeneratorEntity meshes while highlight_generators was false.");
            take(snap_check.refresh(doc, true, true));
            require(
                std::ranges::all_of(snap_check.meshes, [](const auto& m) { return m.selected; }),
                "SceneSnapshot did not restore generator selection highlight when highlight_generators became true.");
        }

        workspace.set_single_graph_selection("scatter");
        workspace.inspecting_network = true;
        const auto inspect_node_ui =
            take(workspace.describe(doc, {1400, 900}, world_net_frame.get(), &project, &ui_session));
        take_ui(ui_session.set_controls(inspect_node_ui));
        const auto inspect_node_frame = take_ui(ui_session.prepare());
        require(ui_session.accept(inspect_node_frame) && workspace.active_graph.empty() &&
                    std::ranges::any_of(inspect_node_ui, [](const ui::Control& c) { return c.text == L"Density"; }),
                "Selecting a SOP node in World Network mode did not inspect its parameters in Parameters pane.");
        take(workspace.commit_ui_controls());

        workspace.inspecting_network = false;
        workspace.set_single_graph_selection({});
        workspace.dock.select(editor::control::entities_tab);
        workspace.dock.select(editor::control::scene_tab);
    }

    // procedural.assign_graph + Undo/Redo:
    invoke("procedural.assign_graph", R"({"asset":""})");
    take(doc.sync_procedural_generators(&project));
    require(take(doc.presentation().render_items()).empty(),
            "Clearing ProceduralGeneratorComponent graph did not clear World render_items.");
    invoke("edit.undo");
    take(doc.sync_procedural_generators(&project));
    require(take(doc.presentation().render_items()).size() == inst_count,
            "edit.undo did not restore ProceduralGeneratorComponent graph binding.");

    // Live .procgraph edit in Network updates World generator on sync:
    invoke("graph.open", "{\"asset\":\"" + graph_key + "\"}");
    set_node_props("scatter", R"({"density":0.55})");
    invoke("graph.exit");
    take(doc.sync_procedural_generators(&project));
    const auto world_items_dense = take(doc.presentation().render_items());
    require(world_items_dense.size() > inst_count,
            "Editing .procgraph in Network did not update World ProceduralGeneratorComponent output.");
    const std::size_t baked_expected = world_items_dense.size();

    // procedural.bake converts CopyToPoints instances into StaticMeshEntity hierarchy in 1 Undo step:
    const std::size_t members_before_bake = take(doc.world().members()).size();
    invoke("procedural.bake");
    const auto members_after_bake = take(doc.world().members());
    require(members_after_bake.size() == members_before_bake + 1 + baked_expected,
            "procedural.bake did not attach Baked group Entity and all child StaticMeshEntities.");
    const ObjectId baked_group = doc.selection().entity;
    const auto baked_children = take(world::children(doc.world().runtime(), baked_group));
    require(baked_children.size() == baked_expected &&
                take(doc.presentation().render_items()).size() == baked_expected * 2,
            "procedural.bake did not instantiate all CopyToPoints instances as child StaticMeshEntities.");
    invoke("edit.undo");
    require(take(doc.world().members()).size() == members_before_bake &&
                take(doc.presentation().render_items()).size() == baked_expected,
            "edit.undo did not remove the baked StaticMeshEntity hierarchy in a single step.");
    invoke("edit.redo");
    require(take(doc.world().members()).size() == members_before_bake + 1 + baked_expected &&
                take(doc.presentation().render_items()).size() == baked_expected * 2,
            "edit.redo did not restore the baked StaticMeshEntity hierarchy.");
    // Verify UTF-16 surrogate-safe truncation kept the document strictly serializable:
    const auto post_bake_serialized = take(doc.serialize());
    require(!post_bake_serialized.empty(), "Serialization after surrogate-truncated Bake failed.");

    // Sheared instance transforms must be explicitly rejected by procedural.bake rather than silently distorted:
    take(doc.open_graph(project, graph_key));
    const auto rot_node = take(doc.graph_add_node(graph_key, "procedural.Transform", "rot_inst", {320.f, 220.f}));
    take(doc.graph_connect(graph_key, "instancer", "output", rot_node, "input"));
    set_node_props(rot_node, R"({"rotate":{"x":0.0,"y":0.0,"z":45.0}})");
    const auto shear_node = take(doc.graph_add_node(graph_key, "procedural.Transform", "shear_inst", {500.f, 220.f}));
    take(doc.graph_connect(graph_key, rot_node, "output", shear_node, "input"));
    set_node_props(shear_node, R"({"scale":{"x":2.5,"y":1.0,"z":1.0}})");
    take(doc.graph_set_display(graph_key, shear_node));
    const auto sheared_bake = doc.bake_procedural_generator(&project, gen_ent);
    require(!sheared_bake && sheared_bake.error().message.find("shear") != std::string::npos,
            "procedural.bake did not reject sheared instance transforms.");

    // Reimporting a mesh, then deleting it from disk and refreshing assets must invalidate SOP snapshots:
    // current cook must fail (while keeping last valid preview), and Bake must reject creating dangling entities:
    {
        const auto obj_src = temp_root / "reimport_prop.obj";
        take(editor::write_file(obj_src, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"));
        const auto prop_key = take(project.import_mesh(obj_src));
        const auto prop_graph_key = take(project.create_procedural_graph(
            L"Content/Graphs/ReimportProp.procgraph",
            "{\"version\":4,\"settings\":{\"display\":\"copy\"},\"nodes\":{"
            "\"pts\":{\"type\":\"procedural.Grid\",\"properties\":{\"primitive_type\":\"points\",\"columns\":2,\"rows\":2}},"
            "\"copy\":{\"type\":\"procedural.CopyToPoints\",\"inputs\":{\"points\":\"pts.output\"},"
            "\"properties\":{\"meshes\":[\"" +
                prop_key + "\"]}}}}"));
        take(doc.open_graph(project, prop_graph_key));
        ATELIER_TRY(changed(doc.create_procedural_generator(prop_graph_key, {10.f, 0.f, 0.f}, &project)));
        const ObjectId prop_gen = doc.selection().entity;
        ATELIER_TRY_BIND(prop_component,
                         component(doc, prop_gen, doc.presentation_types().procedural_generator_component));
        const auto prop_comp = *prop_component;
        const auto eval_initial = take(doc.evaluate_graph(prop_graph_key, &project));
        require(eval_initial.display_output != nullptr && !eval_initial.display_error,
                "Initial imported mesh SOP evaluation failed.");

        take(editor::write_file(obj_src, "v 0 0 0\nv 3 0 0\nv 0 3 0\nf 1 2 3\n"));
        take(doc.reimport_mesh(project, prop_key, obj_src));
        const auto eval_reimp = take(doc.inspect_graph(prop_graph_key, &project));
        require(!eval_reimp.display_error && eval_reimp.display_summary.bounds_max.x > 2.5f,
                "SOP evaluation did not reflect reimported mesh geometry.");
        take(doc.sync_procedural_generators(&project));
        const auto retained_gen_preview =
            take(presentation::read_procedural_source(doc.world().runtime(), prop_comp)).output;
        require(retained_gen_preview != nullptr, "Generator did not publish reimported mesh preview.");

        const auto* prop_entry = project.entry(prop_key);
        require(prop_entry != nullptr, "Missing project entry for imported mesh.");
        const auto imported_path = temp_root / prop_entry->path;
        auto sidecar_path = imported_path;
        sidecar_path += L".asset.json";

        ATELIER_TRY(acceptance::remove_file(imported_path));
        ATELIER_TRY(acceptance::remove_file(sidecar_path));
        take(project.refresh());
        require(project.entry(prop_key) == nullptr, "project.refresh() did not evict deleted mesh entry.");

        // 1) Direct unfrozen case (reimport -> delete -> refresh): current cook fails, preview stays, Bake rejects:
        const std::size_t members_pre_del = take(doc.world().members()).size();
        const std::size_t undo_pre_del = doc.undo_count();
        const auto eval_deleted = take(doc.evaluate_graph(prop_graph_key, &project));
        require(eval_deleted.display_output == nullptr && eval_deleted.display_error.has_value() &&
                    eval_deleted.preview_output != nullptr,
                "Deleting a reimported mesh from disk did not fail current SOP cook or lost fallback preview.");

        const auto unfrozen_deleted_bake = doc.bake_procedural_generator(&project, prop_gen);
        const auto* gen_err = doc.procedural_generator_error(prop_comp);
        require(!unfrozen_deleted_bake && gen_err != nullptr &&
                    unfrozen_deleted_bake.error().message == gen_err->message &&
                    take(presentation::read_procedural_source(doc.world().runtime(), prop_comp)).output ==
                        retained_gen_preview &&
                    take(doc.world().members()).size() == members_pre_del && doc.undo_count() == undo_pre_del,
                "Bake succeeded or dropped retained preview after reimported mesh was deleted from disk.");

        // 2) Frozen node defense-in-depth: even if CopyToPoints was frozen before mesh deletion, Bake rejects it:
        const auto frozen_obj_src = temp_root / "frozen_prop.obj";
        take(editor::write_file(frozen_obj_src, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"));
        const auto frozen_key = take(project.import_mesh(frozen_obj_src));
        const auto patch_frozen_mesh = parse_doc("{\"meshes\":[\"" + frozen_key + "\"]}");
        take(doc.graph_set_properties(prop_graph_key, "copy", patch_frozen_mesh.root()));
        take(doc.sync_procedural_generators(&project));
        take(doc.graph_set_frozen(prop_graph_key, "copy", true, &project));
        take(doc.sync_procedural_generators(&project));
        take(editor::write_file(frozen_obj_src, "v 0 0 0\nv 2 0 0\nv 0 2 0\nf 1 2 3\n"));
        take(doc.reimport_mesh(project, frozen_key, frozen_obj_src));
        const auto changed_mesh_bake = doc.bake_procedural_generator(&project, prop_gen);
        require(!changed_mesh_bake && take(doc.world().members()).size() == members_pre_del &&
                    doc.undo_count() == undo_pre_del,
                "Bake used a reimported mesh instead of the frozen instance geometry.");
        const auto* frozen_entry = project.entry(frozen_key);
        require(frozen_entry != nullptr, "Missing project entry for frozen mesh.");
        const auto frozen_imported_path = temp_root / frozen_entry->path;
        auto frozen_sidecar_path = frozen_imported_path;
        frozen_sidecar_path += L".asset.json";
        ATELIER_TRY(acceptance::remove_file(frozen_imported_path));
        ATELIER_TRY(acceptance::remove_file(frozen_sidecar_path));
        take(project.refresh());
        const auto frozen_deleted_bake = doc.bake_procedural_generator(&project, prop_gen);
        require(!frozen_deleted_bake && take(doc.world().members()).size() == members_pre_del &&
                    doc.undo_count() == undo_pre_del,
                "Bake created StaticMeshEntities referencing a deleted mesh from a frozen SOP snapshot.");
    }

    // Spatial health diagnostics in inspect_graph (topology manifold check, spacing, AABB overlaps, surface clearance):
    {
        const auto unit_cube_src = temp_root / "unit_cube.obj";
        take(editor::write_file(unit_cube_src, "v -0.5 -0.5 -0.5\nv 0.5 -0.5 -0.5\nv 0.5 0.5 -0.5\nv -0.5 0.5 -0.5\n"
                                               "v -0.5 -0.5 0.5\nv 0.5 -0.5 0.5\nv 0.5 0.5 0.5\nv -0.5 0.5 0.5\n"
                                               "f 1 3 2\nf 1 4 3\nf 5 6 7\nf 5 7 8\n"
                                               "f 1 2 6\nf 1 6 5\nf 2 3 7\nf 2 7 6\n"
                                               "f 3 4 8\nf 3 8 7\nf 4 1 5\nf 4 5 8\n"));
        const auto cube_key = take(project.import_mesh(unit_cube_src));
        const auto floor_quad_src = temp_root / "floor_quad.obj";
        take(editor::write_file(floor_quad_src, "v -5.0 0.0 -5.0\nv -5.0 0.0 5.0\nv 5.0 0.0 5.0\nv 5.0 0.0 -5.0\n"
                                                "f 1 2 3\nf 1 3 4\n"));
        const auto floor_mesh_key = take(project.import_mesh(floor_quad_src));
        const std::string sp_key = "scratch/spatial_check";
        take(doc.create_scratch_graph(sp_key));
        take(doc.graph_add_node(sp_key, "procedural.MeshImport", "floor", {0.f, 0.f}));
        const auto floor_patch = parse_doc("{\"mesh\":\"" + floor_mesh_key + "\"}");
        take(doc.graph_set_properties(sp_key, "floor", floor_patch.root()));
        take(doc.graph_add_node(sp_key, "procedural.Box", "closed_box", {160.f, 0.f}));
        take(doc.graph_add_node(sp_key, "procedural.Line", "pts", {0.f, 120.f}));
        const auto pts_patch_1 =
            parse_doc(R"({"start":{"x":0.0,"y":0.5,"z":0.0},"end":{"x":0.4,"y":0.5,"z":0.0},"count":2})");
        take(doc.graph_set_properties(sp_key, "pts", pts_patch_1.root()));
        take(doc.graph_add_node(sp_key, "procedural.CopyToPoints", "copy", {0.f, 220.f}));
        take(doc.graph_connect(sp_key, "pts", "output", "copy", "points"));
        const auto copy_patch = parse_doc("{\"meshes\":[\"" + cube_key + "\"]}");
        take(doc.graph_set_properties(sp_key, "copy", copy_patch.root()));
        take(doc.graph_set_display(sp_key, "copy"));

        const ObjectId floor_id = doc.graph_node_id(sp_key, "floor");
        const ObjectId box_id_sp = doc.graph_node_id(sp_key, "closed_box");
        const auto floor_insp = take(doc.inspect_graph(sp_key, &project, floor_id, false, 4, true));
        require(floor_insp.inspected_summary && floor_insp.inspected_summary->spatial &&
                    floor_insp.inspected_summary->spatial->topology &&
                    floor_insp.inspected_summary->spatial->topology->polygon_count == 2 &&
                    floor_insp.inspected_summary->spatial->topology->open_boundary_edges == 4 &&
                    floor_insp.inspected_summary->spatial->topology->non_manifold_edges == 0,
                "Open floor quad topology diagnostics did not report 2 triangles and 4 boundary edges.");

        const auto box_insp = take(doc.inspect_graph(sp_key, &project, box_id_sp, false, 4, true));
        require(box_insp.inspected_summary && box_insp.inspected_summary->spatial &&
                    box_insp.inspected_summary->spatial->topology &&
                    box_insp.inspected_summary->spatial->topology->polygon_count == 6 &&
                    box_insp.inspected_summary->spatial->topology->open_boundary_edges == 0 &&
                    box_insp.inspected_summary->spatial->topology->non_manifold_edges == 0 &&
                    box_insp.inspected_summary->spatial->topology->degenerate_primitives == 0,
                "Closed Box topology diagnostics did not report a watertight manifold.");

        const auto overlap_insp = take(doc.inspect_graph(sp_key, &project, {}, false, 4, true, floor_id, "floor"));
        require(overlap_insp.display_summary.spatial.has_value(), "Display summary omitted spatial diagnostics.");
        const auto& sp1 = *overlap_insp.display_summary.spatial;
        require(sp1.spacing && sp1.spacing->domain == "instance" && sp1.spacing->element_count == 2 &&
                    std::abs(sp1.spacing->minimum - 0.4) < 1e-4,
                "Instance spacing diagnostics did not report 0.4m nearest-neighbor distance.");
        require(sp1.instance_overlaps && sp1.instance_overlaps->tested_instances == 2 &&
                    sp1.instance_overlaps->overlapping_pairs == 1 &&
                    sp1.instance_overlaps->overlapping_instances == 2 &&
                    std::abs(sp1.instance_overlaps->max_penetration_depth - 0.6) < 1e-3,
                "Instance overlap diagnostics did not detect 0.6m AABB penetration.");
        require(sp1.surface_clearance && sp1.surface_clearance->reference_node == "floor" &&
                    sp1.surface_clearance->grounded_count == 2 && sp1.surface_clearance->floating_count == 0 &&
                    sp1.surface_clearance->buried_count == 0 && sp1.surface_clearance->off_surface_count == 0 &&
                    sp1.surface_clearance->has_gaps && std::abs(sp1.surface_clearance->gap_max) < 1e-4,
                "Surface clearance diagnostics did not report 2 grounded instances on floor.");

        // Move second instance off the 10x10 floor and lift both instances to bottom y = +0.7m (floating):
        const auto pts_patch_2 =
            parse_doc(R"({"start":{"x":0.0,"y":1.2,"z":0.0},"end":{"x":20.0,"y":1.2,"z":0.0},"count":2})");
        take(doc.graph_set_properties(sp_key, "pts", pts_patch_2.root()));
        const auto float_insp = take(doc.inspect_graph(sp_key, &project, {}, false, 4, true, floor_id, "floor"));
        const auto& sp2 = *float_insp.display_summary.spatial;
        require(sp2.instance_overlaps && sp2.instance_overlaps->overlapping_pairs == 0 &&
                    sp2.instance_overlaps->overlapping_instances == 0,
                "Separated instances still reported AABB overlap.");
        require(sp2.surface_clearance && sp2.surface_clearance->grounded_count == 0 &&
                    sp2.surface_clearance->floating_count == 1 && sp2.surface_clearance->off_surface_count == 1 &&
                    sp2.surface_clearance->has_gaps && std::abs(sp2.surface_clearance->gap_min - 0.7) < 1e-3,
                "Surface clearance diagnostics did not distinguish floating (+0.7m) and off-surface instances.");

        const auto disabled_sp = take(doc.inspect_graph(sp_key, &project, {}, false, 4, false));
        require(!disabled_sp.display_summary.spatial.has_value(),
                "inspect_graph with include_spatial=false still populated spatial diagnostics.");
    }

    // S4 Digital Assets (.procgraph v4), multi-target promoted parameters, port realignment & graph_extract_asset:
    {
        const std::string pillar_key = take(project.create_procedural_graph("Content/Graphs/PillarPair.procgraph"));
        take(doc.open_graph(project, pillar_key));
        take(doc.graph_add_node(pillar_key, "procedural.GraphInput", "in_pts", {0.f, -120.f}));
        {
            const auto in_patch = parse_doc(R"({"port_name":"points","order":0,"preview_fallback":"line"})");
            take(doc.graph_set_properties(pillar_key, "in_pts", in_patch.root()));
        }
        take(doc.graph_add_node(pillar_key, "procedural.Box", "box_a", {-96.f, 0.f}));
        take(doc.graph_add_node(pillar_key, "procedural.Box", "box_b", {96.f, 0.f}));
        {
            const auto b_patch = parse_doc(R"({"size":{"x":1.0,"y":9.0,"z":1.0}})");
            take(doc.graph_set_properties(pillar_key, "box_b", b_patch.root()));
        }
        take(doc.graph_add_node(pillar_key, "procedural.Merge", "pair_merge", {0.f, 120.f}));
        take(doc.graph_connect(pillar_key, "in_pts", "output", "pair_merge", "inputs"));
        take(doc.graph_connect(pillar_key, "box_a", "output", "pair_merge", "inputs"));
        take(doc.graph_connect(pillar_key, "box_b", "output", "pair_merge", "inputs"));
        take(doc.graph_add_node(pillar_key, "procedural.GraphOutput", "out_geo", {0.f, 240.f}));
        {
            const auto out_patch = parse_doc(R"({"port_name":"geometry","order":0})");
            take(doc.graph_set_properties(pillar_key, "out_geo", out_patch.root()));
        }
        take(doc.graph_connect(pillar_key, "pair_merge", "output", "out_geo", "input"));
        // Decoupled author debug node: set Display [D] on an unconnected Sphere; AssetNode must still cook GraphOutput!
        take(doc.graph_add_node(pillar_key, "procedural.Sphere", "dbg_sphere", {280.f, 0.f}));
        take(doc.graph_set_display(pillar_key, "dbg_sphere"));

        // Expose box_a.size.y -> "height", then multi-target bind box_b.size.y (syncs box_b.size.y to box_a.size.y = 1.0):
        const auto exposed_name =
            take(doc.graph_expose_parameter(pillar_key, "box_a", "size.y", "height", "Pillar Height", "Dimensions"));
        require(exposed_name == "height", "Expected promoted parameter identifier 'height'.");
        take(doc.graph_bind_parameter_target(pillar_key, "height", "box_b", "size.y"));

        // Controlled field direct-write rejection:
        {
            const auto illegal_patch = parse_doc(R"({"size":{"x":1.0,"y":5.0,"z":1.0}})");
            require(!doc.graph_set_properties(pillar_key, "box_a", illegal_patch.root()),
                    "Direct graph_set_properties on a controlled promoted target must be rejected.");
        }

        // Update promoted parameter -> drives both targets simultaneously:
        {
            const auto val_35 = parse_doc("3.5");
            take(doc.graph_set_parameter_json(pillar_key, "height", val_35.root()));
            const ObjectId box_a_id = doc.graph_node_id(pillar_key, "box_a");
            const ObjectId box_b_id = doc.graph_node_id(pillar_key, "box_b");
            const auto props_a = take(doc.properties(box_a_id));
            const auto props_b = take(doc.properties(box_b_id));
            const auto ay = std::ranges::find(props_a, std::string_view{"size.y"}, &editor::Property::name);
            const auto by = std::ranges::find(props_b, std::string_view{"size.y"}, &editor::Property::name);
            require(ay != props_a.end() && by != props_b.end() && std::abs(std::get<float>(ay->value) - 3.5f) < 1e-5f &&
                        std::abs(std::get<float>(by->value) - 3.5f) < 1e-5f,
                    "Multi-target promoted parameter did not synchronize both bound node fields.");
        }

        // Node rename automatically syncs promoted parameter targets + Undo/Redo:
        take(doc.graph_rename_node(pillar_key, "box_b", "box_right"));
        {
            const auto pviews = take(doc.graph_promoted_parameters(pillar_key));
            require(pviews.size() == 1 && pviews[0].targets.size() == 2 && pviews[0].targets[1] == "box_right.size.y",
                    "Renaming a bound node did not update promoted parameter targets.");
        }
        take(doc.graph_undo(pillar_key, false));
        {
            const auto pviews = take(doc.graph_promoted_parameters(pillar_key));
            require(pviews.size() == 1 && pviews[0].targets[1] == "box_b.size.y",
                    "Undo of node rename did not restore promoted parameter targets.");
        }
        take(doc.graph_undo(pillar_key, true));
        take(doc.save_graph(project, pillar_key));

        // Instantiate Digital Asset in outer graph via "asset:<key>":
        const std::string court_key = take(project.create_procedural_graph("Content/Graphs/OuterCourt.procgraph"));
        take(doc.open_graph(project, court_key));
        take(doc.graph_add_node(court_key, "procedural.Line", "line_pts", {0.f, -120.f}, &project));
        {
            const auto lpatch = parse_doc(R"({"count":4})");
            take(doc.graph_set_properties(court_key, "line_pts", lpatch.root()));
        }
        const auto inst_name =
            take(doc.graph_add_node(court_key, "asset:" + pillar_key, "pillars", {0.f, 0.f}, &project));
        require(inst_name == "pillars", "Expected AssetNode name 'pillars'.");
        take(doc.graph_connect(court_key, "line_pts", "output", "pillars", "points"));
        take(doc.graph_add_node(court_key, "procedural.GraphOutput", "court_out", {0.f, 120.f}));
        take(doc.graph_connect(court_key, "pillars", "geometry", "court_out", "input"));
        take(doc.graph_set_display(court_key, "court_out"));

        // Verify GraphOutput (4 Line points + 2 boxes = 20 points, 12 primitives) is evaluated instead of dbg_sphere:
        const auto court_eval_1 = take(doc.inspect_graph(court_key, &project, {}, false, 4, false));
        require(!court_eval_1.display_error && court_eval_1.display_summary.counts[2] == 12 &&
                    court_eval_1.display_summary.counts[0] == 20,
                "AssetNode did not evaluate GraphOutput with external input (expected 20 points, 12 primitives).");

        // Override "height" = 6.0 on AssetNode without mutating PillarPair's authored 3.5, plus Undo/Redo:
        {
            const auto ov_6 = parse_doc("6.0");
            take(doc.graph_set_asset_override_json(court_key, "pillars", "height", ov_6.root(), &project));
            take(doc.graph_undo(court_key, false));
            const auto court_eval_undo = take(doc.evaluate_graph(court_key, &project));
            float undo_max_y = 0.f;
            for (const auto& p : court_eval_undo.preview_output->positions())
                undo_max_y = std::max(undo_max_y, p.y);
            require(std::abs(undo_max_y - 1.75f) < 1e-4f,
                    "Undo of AssetNode override did not restore default half-extent y=1.75m.");
            take(doc.graph_undo(court_key, true));

            const auto court_eval_ov = take(doc.evaluate_graph(court_key, &project));
            require(court_eval_ov.preview_output != nullptr, "Missing outer court preview output.");
            float max_y = 0.f;
            for (const auto& p : court_eval_ov.preview_output->positions())
                max_y = std::max(max_y, p.y);
            require(std::abs(max_y - 3.0f) < 1e-4f,
                    "AssetNode height=6.0 override did not produce half-extent y=3.0m.");
            const auto props_a = take(doc.properties(doc.graph_node_id(pillar_key, "box_a")));
            const auto ay = std::ranges::find(props_a, std::string_view{"size.y"}, &editor::Property::name);
            require(std::abs(std::get<float>(ay->value) - 3.5f) < 1e-5f,
                    "AssetNode override mutated the shared Digital Asset definition.");
        }

        // Live propagation: edit uncontrolled sibling lane box_a.size.x via graph_set_properties WITHOUT saving:
        {
            const auto xy_patch = parse_doc(R"({"size":{"x":4.0,"y":3.5}})");
            require(!doc.graph_set_properties(pillar_key, "box_a", xy_patch.root()),
                    "graph_set_properties accepted a patch touching promoted lane size.y.");
            const auto x_patch = parse_doc(R"({"size":{"x":4.0}})");
            take(doc.graph_set_properties(pillar_key, "box_a", x_patch.root()));
            const auto court_eval_live = take(doc.evaluate_graph(court_key, &project));
            float max_x = 0.f;
            float max_y = 0.f;
            for (const auto& p : court_eval_live.preview_output->positions()) {
                max_x = std::max(max_x, p.x);
                max_y = std::max(max_y, p.y);
            }
            require(std::abs(max_x - 2.0f) < 1e-4f && std::abs(max_y - 3.0f) < 1e-4f,
                    "Unlocked edit in open Digital Asset did not live-propagate while preserving instance overrides.");
        }

        // Removing a bound target node shrinks/restores promoted targets across Undo/Redo:
        {
            const std::array<std::string, 1> rm_b{"box_right"};
            take(doc.graph_remove_nodes(pillar_key, rm_b));
            require(take(doc.graph_promoted_parameters(pillar_key))[0].targets.size() == 1,
                    "Removing box_right did not prune promoted parameter target list.");
            take(doc.graph_undo(pillar_key, false));
            require(take(doc.graph_promoted_parameters(pillar_key))[0].targets.size() == 2,
                    "Undo of box_right removal did not restore promoted parameter target list.");
        }

        // Upstream parameter removal surfaces undeclared override on AssetNode and preserves byte-identical roundtrip:
        {
            (void)take(doc.graph_expose_parameter(pillar_key, "box_a", "size.z", "obsolete_depth"));
            const auto ov_z = parse_doc("2.5");
            take(doc.graph_set_asset_override_json(court_key, "pillars", "obsolete_depth", ov_z.root(), &project));
            take(doc.graph_remove_parameter(pillar_key, "obsolete_depth"));
            take(doc.save_graph(project, pillar_key));
            take(doc.save_graph(project, court_key));
            const auto court_disk_1 = take(project.assets().procedural_graph(court_key)).text;
            const auto snapshots = take(doc.snapshot_open_graphs());
            const auto court_snap = std::ranges::find(snapshots, court_key, &editor::OpenGraphSnapshot::key);
            require(court_snap != snapshots.end() && court_snap->text == court_disk_1,
                    "OuterCourt.procgraph save_graph -> snapshot roundtrip was not byte-identical.");
            const auto inst_pviews = take(doc.graph_asset_node_parameters(court_key, "pillars", &project));
            const auto stale_ov = std::ranges::find_if(inst_pviews, [](const editor::GraphPromotedParameterView& p) {
                return p.name == "obsolete_depth" && p.undeclared && p.overridden;
            });
            require(stale_ov != inst_pviews.end(),
                    "AssetNode did not tolerantly surface unmatched stale override as undeclared.");
            const auto nested_key = take(project.create_procedural_graph("Content/Graphs/NestedCourt.procgraph"));
            take(doc.open_graph(project, nested_key));
            take(doc.graph_add_node(nested_key, "asset:" + court_key, "court", {0.f, 0.f}, &project));
            take(doc.graph_set_display(nested_key, "court"));
            take(doc.save_graph(project, nested_key));
            const std::vector<std::string> nested_field{"nodes", "court",   "asset",     court_key,
                                                        "nodes", "pillars", "overrides", "obsolete_depth"};
            for (int cook = 0; cook < 2; ++cook) {
                (void)take(doc.evaluate_graph(nested_key, &project));
                const auto nested_view = take(doc.inspect_graph(nested_key, &project));
                require(std::ranges::count(nested_view.issues, nested_field, &world::Diagnostic::field) == 1,
                        "Nested asset override diagnostic lost its instance path or was duplicated on a cached cook.");
            }
            (void)doc.take_diagnostics();
            ATELIER_TRY(changed(doc.create_procedural_generator(nested_key, {}, &project)));
            const auto generator =
                take(component(doc, doc.selection().entity, doc.presentation_types().procedural_generator_component));
            take(doc.sync_procedural_generators(&project));
            auto generator_field = nested_field;
            generator_field.insert(generator_field.begin(), {"graph", nested_key});
            const auto generator_issues = doc.take_diagnostics();
            require(std::ranges::count_if(generator_issues,
                                          [&](const world::Diagnostic& issue) {
                                              return issue.field == generator_field && issue.object == generator &&
                                                     issue.severity == world::Severity::warning;
                                          }) == 1,
                    "Generator did not report the stale nested override on its owning component.");

            take(doc.clear_promoted_parameter_override(
                editor::PromotedParameterTarget{std::string(court_key), "pillars", ObjectId{}}, "obsolete_depth",
                &project));
        }

        // Indirect cycle A -> B -> A is rejected without infinite recursion, and graph_set_node_asset Undo/Redo works:
        {
            const auto cyc_node =
                take(doc.graph_add_node(pillar_key, "asset:" + court_key, "back_ref", {160.f, 0.f}, &project));
            require(cyc_node == "back_ref", "Expected AssetNode back_ref.");
            take(doc.graph_connect(pillar_key, "back_ref", "output", "pair_merge", "inputs"));
            const auto cyc_inspect = take(doc.inspect_graph(court_key, &project, {}, false, 4, false));
            require(cyc_inspect.display_error.has_value(),
                    "Indirect AssetNode cycle (OuterCourt -> PillarPair -> OuterCourt) was not rejected.");
            take(doc.graph_undo(pillar_key, false)); // undo connect
            take(doc.graph_undo(pillar_key, false)); // undo add_node back_ref
            take(doc.save_graph(project, pillar_key));

            const auto bad_key = take(project.create_procedural_graph("Content/Graphs/BadPorts.procgraph", R"({
                "version":4,"settings":{},"nodes":{
                "one":{"type":"procedural.GraphOutput","properties":{"port_name":"same"}},
                "two":{"type":"procedural.GraphOutput","properties":{"port_name":"same"}}}})"));
            const std::string missing_key = "nonexistent-asset";
            const auto missing = project.assets().procedural_graph(missing_key);
            require(!missing, "Missing-asset fixture unexpectedly exists.");
            for (const auto& [key, message] :
                 std::array{std::pair{missing_key, missing.error().message},
                            std::pair{bad_key, std::string("Duplicate GraphOutput port_name 'same'.")}}) {
                take(doc.graph_set_node_asset(court_key, "pillars", key, &project));
                const auto* node = doc.graph_node(court_key, "pillars");
                const auto binding = take(procedural::asset_subgraph(rt, node->node.id()));
                require(node->asset_key == key && binding.bind_error && binding.bind_error->message == message &&
                            binding.ports.inputs.empty() && binding.ports.outputs.empty() &&
                            node->kept.root().path({"inputs", "points"}).has_value(),
                        "Rebinding an unavailable asset exposed stale ports or lost the original error or wire.");
                take(doc.graph_undo(court_key, false));
                require(!doc.graph_node(court_key, "pillars")->kept.root().path({"inputs", "points"}).has_value(),
                        "Undo of failed binding did not restore the original connection.");
            }

            const std::string alt_key = take(project.create_procedural_graph("Content/Graphs/AltAsset.procgraph"));
            take(doc.open_graph(project, alt_key));
            take(doc.graph_add_node(alt_key, "procedural.Sphere", "sph", {0.f, 0.f}));
            take(doc.graph_set_display(alt_key, "sph"));
            take(doc.save_graph(project, alt_key));

            take(doc.graph_set_node_asset(court_key, "pillars", alt_key, &project));
            require(doc.graph_node(court_key, "pillars")->asset_key == alt_key &&
                        doc.graph_node(court_key, "pillars")->kept.root().path({"inputs", "points"}).has_value(),
                    "graph_set_node_asset did not switch asset and preserve unmatched input in kept['inputs'].");
            take(doc.graph_undo(court_key, false));
            require(doc.graph_node(court_key, "pillars")->asset_key == pillar_key &&
                        !doc.graph_node(court_key, "pillars")->kept.root().path({"inputs", "points"}).has_value(),
                    "Undo of graph_set_node_asset did not restore original asset and reconnect 'points'.");
            take(doc.graph_undo(court_key, true));
            take(doc.graph_undo(court_key, false));
        }

        // Upstream port rename realigns AssetNode ports and retains broken connection in kept["inputs"]:
        {
            const auto rename_port = parse_doc(R"({"port_name":"anchors","order":0,"preview_fallback":"line"})");
            take(doc.graph_set_properties(pillar_key, "in_pts", rename_port.root()));
            (void)take(doc.evaluate_graph(court_key, &project));
            const auto* pillars_node = doc.graph_node(court_key, "pillars");
            require(pillars_node != nullptr && pillars_node->kept.root().path({"inputs", "points"}).has_value(),
                    "Upstream GraphInput port rename did not preserve broken outer connection in kept['inputs'].");
            const auto restore_port = parse_doc(R"({"port_name":"points","order":0,"preview_fallback":"line"})");
            take(doc.graph_set_properties(pillar_key, "in_pts", restore_port.root()));
            (void)take(doc.evaluate_graph(court_key, &project));
            require(!doc.graph_node(court_key, "pillars")->kept.root().path({"inputs", "points"}).has_value(),
                    "Restoring upstream GraphInput port name did not automatically reconnect kept['inputs'].");
        }

        {
            const auto host = take(project.create_procedural_graph("Content/Graphs/RetainedHost.procgraph", R"({
              "version":4,"settings":{"display":"outside"},"nodes":{
              "a":{"type":"procedural.Grid"},"left":{"type":"procedural.Box"},
              "b":{"type":"procedural.Merge","inputs":{"inputs":["a.output","a.legacy","left.output"]}},
              "outside":{"type":"procedural.Merge","inputs":{"inputs":["a.output","Retained.output","b.output"]}}}})"));
            take(doc.open_graph(project, host));
            const auto revision = project.revision();
            const std::array<std::string, 1> only_b{"b"};
            const auto failed = doc.graph_extract_asset(project, host, only_b, "Content/Graphs/Rejected.procgraph");
            require(
                !failed &&
                    failed.error().message ==
                        "Cannot extract saved connection 'a.legacy' at b.inputs: the boundary source port is unresolved." &&
                    revision == project.revision() && !doc.graph_dirty(host) &&
                    !std::filesystem::exists(project.root() / "Content/Graphs/Rejected.procgraph") &&
                    !std::filesystem::exists(project.root() / "Content/Graphs/Rejected.procgraph.asset.json"),
                "Unrepresentable extraction must return the first boundary error before publishing anything.");

            const std::array<std::string, 2> selected{"a", "b"};
            const auto shadow =
                doc.graph_extract_asset(project, host, selected, "Content/Graphs/Shadow.procgraph", "Retained");
            require(!shadow && shadow.error().message.find("Retained") != std::string::npos &&
                        project.revision() == revision &&
                        !std::filesystem::exists(project.root() / "Content/Graphs/Shadow.procgraph"),
                    "An explicit AssetNode name captured an unrelated unresolved outer wire.");
            const auto stage =
                take(doc.graph_extract_asset(project, host, selected, "Content/Graphs/Retained.procgraph"));
            take(doc.open_graph(project, stage.first));
            const auto check_inputs = [&](const std::string& graph, std::string_view node, std::string_view first,
                                          std::string_view second, std::string_view third) {
                const auto* found = doc.graph_node(graph, node);
                require(found != nullptr, "Expected retained node after extraction.");
                const auto port = take(found->kept.root().path({"inputs", "inputs"}));
                require(port.size() == 3 && port.at(0)->string() == first && port.at(1)->string() == second &&
                            port.at(2)->string() == third,
                        "Extraction changed retained multi-input slot order.");
            };
            check_inputs(stage.first, "b", "a.output", "a.legacy", "input_inputs.output");
            check_inputs(host, "outside", "Retained_2.output", "Retained.output", "Retained_2.output_2");
            take(doc.graph_undo(host, false));
            check_inputs(host, "b", "a.output", "a.legacy", "left.output");
            check_inputs(host, "outside", "a.output", "Retained.output", "b.output");
            take(doc.graph_undo(host, true));
            check_inputs(host, "outside", "Retained_2.output", "Retained.output", "Retained_2.output_2");
        }

        // graph_extract_asset: reject non-convex selection, extract convex stage, and 1-step Undo/Redo:
        {
            const std::string ext_test_key =
                take(project.create_procedural_graph("Content/Graphs/ExtractHost.procgraph"));
            take(doc.open_graph(project, ext_test_key));
            take(doc.graph_add_node(ext_test_key, "procedural.Box", "src_box", {0.f, 0.f}));
            take(doc.graph_add_node(ext_test_key, "procedural.Transform", "mid_xform", {0.f, 120.f}));
            {
                const auto xpatch = parse_doc(R"({"translate":{"x":2.0,"y":3.0,"z":0.0}})");
                take(doc.graph_set_properties(ext_test_key, "mid_xform", xpatch.root()));
            }
            take(doc.graph_add_node(ext_test_key, "procedural.Transform", "tail_xform", {0.f, 240.f}));
            take(doc.graph_connect(ext_test_key, "src_box", "output", "mid_xform", "input"));
            take(doc.graph_connect(ext_test_key, "mid_xform", "output", "tail_xform", "input"));
            take(doc.graph_set_display(ext_test_key, "tail_xform"));
            (void)take(doc.graph_expose_parameter(ext_test_key, "mid_xform", "translate.y", "lift_y"));

            // Non-convex selection {src_box, tail_xform} leaves mid_xform both downstream and upstream -> must reject!
            const std::array<std::string, 2> non_convex{"src_box", "tail_xform"};
            require(!doc.graph_extract_asset(project, ext_test_key, non_convex,
                                             "Content/Graphs/IllegalNonConvex.procgraph"),
                    "graph_extract_asset accepted a non-convex selection that would introduce a cycle.");

            const auto pre_eval = take(doc.evaluate_graph(ext_test_key, &project));
            const std::array<std::string, 1> convex_sel{"mid_xform"};
            const auto extracted = take(doc.graph_extract_asset(
                project, ext_test_key, convex_sel, "Content/Graphs/ExtractedStage.procgraph", "stage_asset"));
            require(!extracted.first.empty() && extracted.second == "stage_asset",
                    "graph_extract_asset did not create asset key and expected AssetNode name.");
            require(doc.graph_node(ext_test_key, "mid_xform") == nullptr &&
                        doc.graph_node(ext_test_key, "stage_asset") != nullptr,
                    "graph_extract_asset did not replace selected nodes with stage_asset.");

            const auto post_eval = take(doc.evaluate_graph(ext_test_key, &project));
            require(pre_eval.preview_output && post_eval.preview_output &&
                        std::ranges::equal(pre_eval.preview_output->positions(), post_eval.preview_output->positions()),
                    "Extracted AssetNode did not preserve exact geometry output.");

            // Extracted asset preserved internal promoted parameter 'lift_y':
            const auto stage_params = take(doc.graph_asset_node_parameters(ext_test_key, "stage_asset", &project));
            require(stage_params.size() == 1 && stage_params[0].name == "lift_y",
                    "Extracted .procgraph asset did not preserve internal promoted parameter 'lift_y'.");

            // 1-step Undo restores mid_xform, wires, and outer 'lift_y' parameter; Redo restores stage_asset:
            take(doc.graph_undo(ext_test_key, false));
            require(doc.graph_node(ext_test_key, "mid_xform") != nullptr &&
                        doc.graph_node(ext_test_key, "stage_asset") == nullptr &&
                        take(doc.graph_promoted_parameters(ext_test_key)).size() == 1,
                    "Single-step Undo of graph_extract_asset did not restore original nodes and parameters.");
            take(doc.graph_undo(ext_test_key, true));
            const auto redo_eval = take(doc.evaluate_graph(ext_test_key, &project));
            require(doc.graph_node(ext_test_key, "stage_asset") != nullptr && redo_eval.preview_output &&
                        std::ranges::equal(redo_eval.preview_output->positions(), pre_eval.preview_output->positions()),
                    "Redo of graph_extract_asset did not restore wired stage_asset and geometry.");
        }
    }

    return changed(doc.close());
}

Result<void> verify_scene_recovery() {
    ATELIER_TRY_BIND(
        temp, acceptance::file_operation([](auto& error) { return std::filesystem::temp_directory_path(error); }));
    const auto temp_root = *temp / ("atelier-scene-recovery-" + std::to_string(GetCurrentProcessId()) + "-" +
                                    std::to_string(GetTickCount64()));
    struct Cleanup {
        std::filesystem::path dir;

        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    } cleanup{temp_root};

    auto project = take(editor::Project::create(temp_root / "project"));
    auto loaded =
        editor::load_project_scene(project, take(assets::read_file(project.root() / "Content/Scenes/Main.scene")),
                                   editor::LoadPolicy::strict, false, editor::SessionRole::editor);
    require(loaded.session != nullptr && !loaded.error, "Recovery test scene failed to load.");
    auto& doc = *loaded.session;
    ATELIER_TRY(changed(doc.create_static_mesh(assets::mesh_assets().front())));
    const ObjectId entity = doc.selection().entity;
    const std::array entities{entity};
    const std::array committed_trs{world::Trs{{3.f, 0.f, 0.f}, {}, {1.f, 1.f, 1.f}}};
    take(doc.begin_transform(entities, 901));
    take(doc.preview_transform(901, committed_trs));
    take(doc.commit_edit(901));

    const std::array unfinished_trs{world::Trs{{99.f, 0.f, 0.f}, {}, {1.f, 1.f, 1.f}}};
    take(doc.begin_transform(entities, 902));
    take(doc.preview_transform(902, unfinished_trs));
    require(doc.editing() && doc.dirty(), "Expected dirty document with an active unfinished transform gesture.");

    const auto log_dir = temp_root / "logs";
    auto run = take(logging::start_run({.directory = log_dir, .process_name = "recovery_acceptance"}));
    application::SceneRecovery recovery;
    const std::string oversized_reason(5000, 'r');
    const auto& outcome =
        recovery.preserve(doc, project.root(), "Content/Scenes/Main.scene", oversized_reason, temp_root / "recovery");
    require(recovery.attempted() && recovery.logged() && outcome && outcome->has_value() && doc.dirty() &&
                !doc.editing(),
            "SceneRecovery did not cancel unfinished edit, keep dirty state, and log oversized failure reason.");
    const auto saved_scene = (*outcome)->scene;
    ATELIER_TRY_BIND(saved_exists, acceptance::file_exists(saved_scene));
    require((*outcome)->warning.empty() && *saved_exists, "Recovery scene file was not written cleanly.");
    const auto diag_field = recovery.diagnostic_field();
    const auto expected_utf8 = platform::utf16_to_utf8(saved_scene.generic_wstring());
    require(expected_utf8.has_value() && diag_field && diag_field->key == "recovery_scene" &&
                std::get<std::string>(diag_field->value) == *expected_utf8,
            "SceneRecovery::diagnostic_field did not retain the saved scene path.");

    const auto finished = run->finish();
    require(finished.backend_acknowledged && finished.sink_flush_succeeded &&
                finished.health.rejected_invalid_record == 0 && finished.health.rejected_record_too_large == 0,
            "Logging rejected or failed to flush the scene recovery record.");
    auto reader = take(logging::Reader::open(log_dir));
    std::vector<logging::Record> records;
    bool reader_done = false;
    for (unsigned polls = 0; polls < 200 && !reader_done; ++polls) {
        auto batch = reader->poll({1024 * 1024, 4096, std::chrono::milliseconds{50}});
        for (auto& r : batch.records)
            records.push_back(std::move(r));
        reader_done = batch.status.finished;
    }
    require(reader_done, "Logging Reader did not finish reading recovery run.");
    const auto logged_it =
        std::ranges::find_if(records, [](const logging::Record& r) { return r.entry.event_code == "recovery.saved"; });
    require(logged_it != records.end() && logged_it->entry.message == "Scene recovery saved: " + *expected_utf8,
            "events.jsonl did not persist recovery.saved with the full recovery scene path.");

    const auto sidecar =
        take(json::Document::parse(take(assets::read_file(saved_scene.parent_path() / "recovery.json")), {16}));
    require(take(sidecar.root().string({"kind"})) == "author_scene_recovery" &&
                take(sidecar.root().boolean({"unfinished_edit_cancelled"})) &&
                take(sidecar.root().string({"reason"})) == oversized_reason,
            "recovery.json did not record cancelled unfinished gesture and full failure reason.");

    auto recovered = editor::load_project_scene(project, take(assets::read_file(saved_scene)),
                                                editor::LoadPolicy::strict, false, editor::SessionRole::editor);
    require(recovered.session != nullptr && !recovered.error, "Recovered scene failed strict reload.");
    const auto members = take(recovered.session->world().members());
    ObjectId recovered_mesh_entity{};
    for (const auto& m : members)
        if (take(recovered.session->world().runtime().type_of(m.id())) ==
            recovered.session->presentation_types().static_mesh_entity)
            recovered_mesh_entity = m.id();
    require(recovered_mesh_entity != ObjectId{}, "Recovered scene lost the StaticMeshEntity.");
    const auto mtx = take(world::world_matrix(recovered.session->world().runtime(), recovered_mesh_entity));
    require(mtx == world::transform_matrix(committed_trs[0]),
            "Recovered scene did not restore the last committed transform instead of the unfinished gesture.");

    ATELIER_TRY(changed(recovered.session->close()));
    return changed(doc.close());
}
}

Result<void> run_document(int argc, char** argv, std::string_view& stage) {
    if ((argc == 5 || argc == 6) && stage == "--profile-scene-cpu")
        return profile_scene_cpu(argv[2], argv[3], argv[4],
                                 argc == 6 ? std::filesystem::path(argv[5]) : std::filesystem::path{});
    if (argc == 5 && stage == "--profile-physics-cpu")
        return profile_physics_cpu(argv[2], argv[3], argv[4]);
    if (argc != 1)
        return std::unexpected(Error{ErrorCode::argument_mismatch,
                                     "Expected no arguments, --profile-scene-cpu or --profile-physics-cpu."});
    editor::Session single;
    take(single.initialize());
    ATELIER_TRY(changed(single.create_static_mesh(assets::mesh_assets().front())));
    require(single.undo_count() == 1 && take(single.world().runtime().type_of(single.selection().entity)) ==
                                            single.presentation_types().static_mesh_entity,
            "Static mesh factory did not create one typed Entity history entry.");
    const auto entity = single.selection().entity;
    ATELIER_TRY_BIND(mesh, component(single, entity, single.presentation_types().static_mesh_component));
    require(take(world::components(single.world().runtime(), entity)).size() == 1,
            "Mesh Entity acquired an extra component.");
    ATELIER_TRY(changed(single.undo()));
    collect(single);
    require(take(single.world().members()).empty() && take(single.presentation().render_items()).empty(),
            "Creation undo retained the Entity or its rendering.");
    ATELIER_TRY(changed(single.redo()));
    ATELIER_TRY_BIND(restored, component(single, entity, single.presentation_types().static_mesh_component));
    require(single.selection().entity == entity && *restored == *mesh &&
                take(single.presentation().render_items()).front().geometry == assets::mesh_assets().front().mesh,
            "Creation redo changed identity or mesh resource.");
    ATELIER_TRY(changed(single.close()));
    stage = "glTF transform";
    verify_gltf_import();
    stage = "mesh reimport";
    ATELIER_TRY(verify_mesh_reimport());
    stage = "procedural graph format";
    verify_graph_format();
    stage = "procedural editor";
    ATELIER_TRY(verify_procedural_editor());
    stage = "scene recovery";
    ATELIER_TRY(verify_scene_recovery());
    stage = "window quit sources";
    {
        application::Window win;
        ATELIER_TRY(win.initialize());
        require(!win.closing() && application::Window::quit_source_name(win.quit_source()) == "none",
                "Fresh Window reported non-empty quit_source.");
        SendMessageW(win.handle(), WM_SYSCOMMAND, SC_CLOSE, 0);
        require(win.closing() && application::Window::quit_source_name(win.quit_source()) == "wm_syscommand_close",
                "SC_CLOSE did not record wm_syscommand_close.");
        win.cancel_close();
        require(!win.closing() && application::Window::quit_source_name(win.quit_source()) == "none",
                "cancel_close() did not reset quit_source.");
        win.request_operator_close();
        win.poll([](application::NativeEvent&&) {});
        require(win.closing() && application::Window::quit_source_name(win.quit_source()) == "operator:app.quit",
                "request_operator_close() did not record operator:app.quit.");
        win.cancel_close();
        SendMessageW(win.handle(), WM_CLOSE, 0, 0);
        require(win.closing() && application::Window::quit_source_name(win.quit_source()) == "wm_close",
                "External WM_CLOSE did not record wm_close.");
        win.cancel_close();
        PostQuitMessage(0);
        win.poll([](application::NativeEvent&&) {});
        require(win.closing() && application::Window::quit_source_name(win.quit_source()) == "wm_quit",
                "WM_QUIT did not record wm_quit.");
    }
    std::puts(
        "Editor document acceptance: PASS (creation undo/redo, glTF transform, mesh reimport, graph format, SOP editor + 2D Network canvas)");
    return {};
}

int main(int argc, char** argv) {
    atelier::install_fatal_handler();
    std::string_view stage = argc > 1 ? argv[1] : "document history";
    auto run = run_document(argc, argv, stage);
    if (!run) {
        std::fprintf(stderr, "Editor document acceptance: FAIL [%.*s]: %s\n", static_cast<int>(stage.size()),
                     stage.data(), run.error().message.c_str());
        return 1;
    }
    return 0;
}
