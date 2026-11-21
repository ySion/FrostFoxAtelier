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
  "settings": {"display": "noise", "seed": 9, "template": "grid"},
  "version": 2
})";
    require(!editor::load_graph(runtime, R"({"version":1,"settings":{},"nodes":{}})"),
            "load_graph accepted legacy version 1 procedural graph.");
    const auto doc = take(editor::load_graph(runtime, input_json));
    require(doc.nodes.size() == 5 && doc.notes.size() == 1 && doc.boxes.size() == 1 &&
                doc.template_node != ObjectId{} && doc.issues.size() == 2 &&
                doc.issues[0].kind == world::DiagnosticKind::unknown_type &&
                doc.issues[1].kind == world::DiagnosticKind::reference_dropped,
            "load_graph did not tolerantly retain v2 annotations, template, missing SOP type and dropped connection.");
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
      "version":2,"settings":{},"nodes":{
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
    std::string dense = R"({"version":2,"settings":{"display":"fault"},"nodes":{)";
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
        workspace.commit_ui_controls();

        for (const std::uint32_t vk : {'Z', 'C', 'I'}) {
            send_key(ui::EventKind::key_down, ui::Key::none, vk);
            require(workspace.network.popup().open(), "Shortcut Z/C/I did not open floating Network popup.");
            const auto pop_controls = take(workspace.describe(doc, {1400, 900}, b_frame.get(), &project, &ui_session));
            take_ui(ui_session.set_controls(pop_controls));
            const auto pop_frame = take_ui(ui_session.prepare());
            require(ui_session.accept(pop_frame) && pop_frame->find(editor::network_control::popup_box) != nullptr,
                    "Floating Network popup (Shape/Color/Info) failed UI validation or layout.");
            workspace.commit_ui_controls();
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
            workspace.commit_ui_controls();
        }
        {
            const auto ghost_key = take(project.create_procedural_graph(L"Content/Graphs/GhostTest.procgraph", R"({
  "version": 2,
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
    workspace.commit_ui_controls();

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
    workspace.commit_ui_controls();

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
        workspace.commit_ui_controls();

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
        workspace.commit_ui_controls();

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
    set_node_props(rot_node, R"({"rotate":{"x":0.0,"y":0.0,"z":0.38268343,"w":0.92387953}})");
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
            "{\"version\":2,\"settings\":{\"display\":\"copy\"},\"nodes\":{"
            "\"pts\":{\"type\":\"procedural.Grid\",\"properties\":{\"columns\":2,\"rows\":2}},"
            "\"copy\":{\"type\":\"procedural.CopyToPoints\",\"inputs\":{\"points\":\"pts.output\"},"
            "\"properties\":{\"meshes\":[\"" +
                prop_key + "\"]}}}}"));
        take(doc.open_graph(project, prop_graph_key));
        changed(doc.create_procedural_generator(prop_graph_key, {10.f, 0.f, 0.f}, &project));
        const ObjectId prop_gen = doc.selection().entity;
        const ObjectId prop_comp = component(doc, prop_gen, doc.presentation_types().procedural_generator_component);
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

        std::filesystem::remove(imported_path);
        std::filesystem::remove(sidecar_path);
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
        std::filesystem::remove(frozen_imported_path);
        std::filesystem::remove(frozen_sidecar_path);
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
