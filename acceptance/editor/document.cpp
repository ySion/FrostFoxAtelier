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
#include "application/presentation.hpp"
#include "application/procedural_preview.hpp"
#include "application/scene_snapshot.hpp"
#include "renderer/scene_visibility.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <numeric>
#include <numbers>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <windows.h>

void verify_mesh_reimport();

namespace {
using namespace atelier;

using namespace atelier::acceptance::helpers;

void changed(Result<world::Commit> result) {
    const auto commit = take(std::move(result));
    if (commit.notification_error)
        throw std::runtime_error(commit.notification_error->message);
}

void collect(editor::Session& document) {
    for (unsigned step = 0; step < 96; ++step)
        take(document.collect_step());
}

ObjectId component(editor::Session& document, ObjectId entity, TypeId type) {
    for (const auto& root : take(world::components(document.world().runtime(), entity)))
        if (take(document.world().runtime().type_of(root.id())) == type)
            return root.id();
    throw std::runtime_error("Expected component is absent.");
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
      "inputs": {"input": "merged.output"},
      "position": [540.0, 60.0],
      "properties": {"attribute": "nval", "frequency": 2.0, "layer": "point"},
      "type": "procedural.Noise"
    }
  },
  "settings": {"display": "noise", "seed": 9},
  "version": 1
})";
    const auto doc = take(editor::load_graph(runtime, input_json));
    require(doc.nodes.size() == 5 && doc.issues.size() == 2 &&
                doc.issues[0].kind == world::DiagnosticKind::unknown_type &&
                doc.issues[1].kind == world::DiagnosticKind::reference_dropped,
            "load_graph did not tolerantly retain missing SOP type and dropped multi-port connection.");
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
}

void verify_procedural_editor() {
    const auto temp_root =
        std::filesystem::temp_directory_path() /
        ("atelier-sop-editor-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    struct Cleanup {
        std::filesystem::path dir;

        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    } cleanup{temp_root};

    auto project = take(editor::Project::create(temp_root));
    take(editor::write_file(temp_root / "Source/TerraceWave.source", R"(
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
    require(graph_entry != nullptr && graph_entry->kind == assets::AssetKind::procedural_graph &&
                std::filesystem::exists(temp_root / "Content/Graphs/Terrain.procgraph.asset.json"),
            "Project did not register .procgraph asset with GUID sidecar.");

    auto loaded = editor::load_project_scene(project, take(assets::read_file(temp_root / "Content/Scenes/Main.scene")),
                                             editor::LoadPolicy::strict, false, editor::SessionRole::editor);
    require(loaded.session != nullptr && !loaded.error,
            loaded.error ? loaded.error->message : "Project scene failed to load with custom Source SOP.");
    auto& doc = *loaded.session;
    doc.bind_project_code(loaded.code);

    // Tolerant assets remain editable when an unresolved endpoint shares a multi-input port.
    const auto repair_key = take(project.create_procedural_graph(L"Content/Graphs/Repair.procgraph", R"({
      "version":1,"settings":{},"nodes":{
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

    const auto catalog = take(doc.procedural_node_types());
    require(std::ranges::any_of(catalog, [](const auto& c) { return c.name == "procedural.Grid" && c.native; }) &&
                std::ranges::any_of(catalog, [](const auto& c) { return c.name == "game.TerraceWave" && !c.native; }),
            "Session procedural node catalog did not include both native C++ and project .source SOPs.");

    editor::Workspace workspace;
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

        void reset_layout() override { ws.reset_layout(); }

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

        Result<void> fit_lighting(bool) override { return {}; }

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
    const auto surface_mesh = application::build_procedural_surface_mesh(*eval_terrace.inspected_output);
    require(surface_mesh != nullptr && surface_mesh->vertices().size() == 8064 * 3 && surface_mesh->edge_mask(0) == 7 &&
                surface_mesh->edge_mask(64) == 5 && surface_mesh->edge_mask(65) == 3,
            "build_procedural_surface_mesh did not emit polar triangle (7) and quad boundary edge_masks (5, 3).");
    invoke("view.frame_all");
    const auto view = editor::ViewportView::make(host.camera(), {0, 0, 800, 600}, 1.f);
    std::vector<renderer::OverlayShape> guides;
    application::append_procedural_guides(*eval_terrace.inspected_output, view,
                                          {.show_points = true, .show_normals = true, .show_orient = true}, guides);
    require(!guides.empty(), "append_procedural_guides emitted zero OverlayShapes for framed SOP geometry.");

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
    take_ui(ui_session.initialize(application::editor_fonts()));
    const auto styles = take(application::editor_styles());
    take_ui(ui_session.set_style_sheet(styles));
    take_ui(ui_session.set_surface({{1400, 900}, 1.f, 1}));
    workspace.dock.select(editor::network_control::tab);
    workspace.dock.select(editor::spreadsheet_control::tab);
    const auto controls = take(workspace.describe(doc, {1400, 900}, nullptr, &project, &ui_session));
    take_ui(ui_session.set_controls(controls));
    const auto frame = take_ui(ui_session.prepare());
    require(ui_session.accept(frame), "Workspace UI frame with Network + Geometry Spreadsheet was rejected.");
    workspace.commit_ui_controls();
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
    // Also verify floating tab_box mounts cleanly in UI controls when open:
    send_key(ui::EventKind::key_down, ui::Key::tab);
    const auto tab_controls = take(workspace.describe(doc, {1400, 900}, frame.get(), &project, &ui_session));
    take_ui(ui_session.set_controls(tab_controls));
    const auto tab_frame = take_ui(ui_session.prepare());
    require(ui_session.accept(tab_frame) && tab_frame->find(editor::network_control::tab_search_input) != nullptr,
            "Open Tab quick-create popup was rejected by UI layout or missing search field.");
    workspace.commit_ui_controls();
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
    std::string dense = R"({"version":1,"settings":{"display":"fault"},"nodes":{)";
    std::string inputs;
    for (unsigned i = 0; i < 33; ++i) {
        const auto name = "grid" + std::to_string(i);
        dense += "\"" + name + R"(":{"type":"procedural.Grid","position":[0,0]},)";
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
    workspace.commit_ui_controls();
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
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    const auto help_frame = take_ui(ui_session.prepare());
    require(help_frame->tooltip && help_frame->tooltip->owner == fault_title,
            "Node hover did not survive a canvas refresh.");
    invoke("graph.exit");
    require(workspace.active_graph.empty(), "graph.exit did not return Scene View context to World.");
    changed(doc.close());
}
}

int main(int argc, char** argv) {
    atelier::install_fatal_handler();
    std::string_view stage = argc > 1 ? argv[1] : "document history";
    try {
        if ((argc == 5 || argc == 6) && std::string_view(argv[1]) == "--profile-scene-cpu") {
            profile_scene_cpu(argv[2], argv[3], argv[4],
                              argc == 6 ? std::filesystem::path(argv[5]) : std::filesystem::path{});
            return 0;
        }
        if (argc == 5 && std::string_view(argv[1]) == "--profile-physics-cpu") {
            profile_physics_cpu(argv[2], argv[3], argv[4]);
            return 0;
        }
        require(argc == 1, "Expected no arguments, --profile-scene-cpu or --profile-physics-cpu.");
        editor::Session single;
        take(single.initialize());
        changed(single.create_static_mesh(assets::mesh_assets().front()));
        require(single.undo_count() == 1 && take(single.world().runtime().type_of(single.selection().entity)) ==
                                                single.presentation_types().static_mesh_entity,
                "Static mesh factory did not create one typed Entity history entry.");
        const auto entity = single.selection().entity;
        const auto mesh = component(single, entity, single.presentation_types().static_mesh_component);
        require(take(world::components(single.world().runtime(), entity)).size() == 1,
                "Mesh Entity acquired an extra component.");
        changed(single.undo());
        collect(single);
        require(take(single.world().members()).empty() && take(single.presentation().render_items()).empty(),
                "Creation undo retained the Entity or its rendering.");
        changed(single.redo());
        require(single.selection().entity == entity &&
                    component(single, entity, single.presentation_types().static_mesh_component) == mesh &&
                    take(single.presentation().render_items()).front().geometry == assets::mesh_assets().front().mesh,
                "Creation redo changed identity or mesh resource.");
        changed(single.close());
        stage = "glTF transform";
        verify_gltf_import();
        stage = "mesh reimport";
        verify_mesh_reimport();
        stage = "procedural graph format";
        verify_graph_format();
        stage = "procedural editor";
        verify_procedural_editor();
        std::puts(
            "Editor document acceptance: PASS (creation undo/redo, glTF transform, mesh reimport, graph format, SOP editor + 2D Network canvas)");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Editor document acceptance: FAIL [%.*s]: %s\n", static_cast<int>(stage.size()),
                     stage.data(), error.what());
        return 1;
    }
}
