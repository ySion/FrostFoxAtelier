// Operator parameters: editor.* records published only by the editor's document,
// their schema, strict argument decoding and the commands that fix arguments.
#include "editor/operators.hpp"
#include "editor/view_options.hpp"

namespace operator_parameters {
struct Host final : editor::OperatorHost {
    explicit Host(editor::Session& document) : document(document) {}

    editor::Session& document;

    editor::Camera view;

    editor::ToolSettings tools;

    editor::Workspace panels;

    editor::ViewportTool viewport;

    int tool_changes{};

    int scene_changes{};

    Result<std::optional<std::filesystem::path>> choose_path(editor::PathPurpose, const std::filesystem::path&,
                                                             std::wstring_view) override {
        return std::unexpected(Error{ErrorCode::unsupported_operation, "A native file picker requires a window."});
    }

    void asset_created(std::string_view key, bool inspect_material) override {
        panels.material_inspection = key;
        panels.inspecting_asset = inspect_material;
    }

    Result<void> end_interaction() override { return {}; }

    const editor::Camera& camera() const noexcept override { return view; }

    Result<void> set_camera(const editor::Camera& camera) override {
        view = camera;
        return {};
    }

    editor::ViewportTool& viewport_tool() noexcept override { return viewport; }

    Result<void> place_duplicates() override {
        return viewport.place_duplicates(document, editor::ViewportView::make(view, {0, 0, 800, 600}, 1), tools,
                                         {400, 300});
    }

    Result<void> begin_entity_rename(ObjectId, editor::RegionKind) override {
        return std::unexpected(Error{ErrorCode::unsupported_operation, "Inline name editing requires a window."});
    }

    void reset_layout() override { panels.reset_layout(); }

    Result<void> close_tab(editor::RegionKind region, bool) override {
        panels.dock.close(editor::region_tab(region));
        return {};
    }

    void maximize_tab(editor::RegionKind region) override { panels.dock.maximize(editor::region_tab(region)); }

    void show_tab(editor::RegionKind region) override { panels.dock.select(editor::region_tab(region)); }

    void cycle_tab(editor::RegionKind region, int direction) override {
        panels.dock.select(editor::region_tab(region));
        panels.dock.cycle(direction);
    }

    Result<void> set_view_settings(const renderer::ViewSettings& settings) override {
        return panels.set_view_settings(document, settings);
    }

    Result<void> source_history(bool) override { return {}; }

    Result<void> workbench_history(bool) override { return {}; }

    void scene_changed() override { ++scene_changes; }

    void set_tool_settings(const editor::ToolSettings& settings) override {
        tools = settings;
        ++tool_changes;
    }

    void quit() override { throw std::runtime_error("Quit requires the window host."); }

    void about() override { throw std::runtime_error("About requires the window host."); }

    void select_box() override { throw std::runtime_error("Box selection requires the window host."); }

    void show_menu(editor::MenuKind) override { throw std::runtime_error("Menus require the window host."); }

    void toggle_view(editor::ViewToggleOption option) override {
        auto& value = option == editor::ViewToggleOption::fps ? panels.show_fps : panels.viewport_options_collapsed;
        value = !value;
    }

    Result<void> fit_lighting(bool) override { throw std::runtime_error("Lighting fit requires the window host."); }

    Result<void> rename_asset(std::string_view, std::string_view, bool) override {
        throw std::runtime_error("Asset rename requires the application host.");
    }

    Result<void> save_scene(const std::filesystem::path&) override {
        throw std::runtime_error("Save requires the application host.");
    }

    Result<void> save_documents(const editor::Context&, bool) override {
        throw std::runtime_error("Save requires the application host.");
    }

    Result<void> open_source(const std::filesystem::path& path) override { return panels.source_editor.open(path); }

    Result<void> create_source(const std::filesystem::path&, bool) override {
        throw std::runtime_error("Create source requires the application host.");
    }

    Result<void> close_source(bool) override {
        throw std::runtime_error("Close source requires the application host.");
    }

    Result<void> reload_source(bool) override {
        throw std::runtime_error("Reload source requires the application host.");
    }

    Result<void> start_play(bool, bool) override { throw std::runtime_error("Play requires the application host."); }

    void stop_play(bool) override { throw std::runtime_error("Play requires the application host."); }

    Result<std::string> import_asset(editor::ImportKind, const std::filesystem::path&) override {
        throw std::runtime_error("Import requires the application host.");
    }

    Result<void> reimport_asset(std::string_view, const std::filesystem::path&) override {
        throw std::runtime_error("Reimport requires the application host.");
    }
};

bool same(const editor::ToolSettings& a, const editor::ToolSettings& b) {
    return a.tool == b.tool && a.pivot == b.pivot && a.grid_snap == b.grid_snap &&
           a.cursor_surface == b.cursor_surface && a.grid_spacing == b.grid_spacing && a.select_mode == b.select_mode &&
           a.orientation == b.orientation && a.snap_move == b.snap_move && a.snap_rotate == b.snap_rotate &&
           a.snap_scale == b.snap_scale && a.rotation_step == b.rotation_step && a.scale_step == b.scale_step;
}
}

void verify_operator_parameters() {
    using assets::json_format::Json;
    using namespace operator_parameters;
    editor::Session document({}, editor::SessionRole::editor);
    auto& runtime = document.world().runtime();
    editor::OperatorRegistry registry;
    editor::register_builtin_operators(registry);

    // A filesystem operation uses the same public call boundary as menus and Control.
    require(registry.find("asset.new_material"), "Material creation has not moved to the operator registry.");
    {
        const auto folder = std::filesystem::current_path() / "out/operator-result-evidence" /
                            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        auto project = take(editor::Project::create(folder));
        editor::Session resources({}, editor::SessionRole::editor);
        Host host(resources);
        auto context = host.panels.operator_context(resources, &project, false);
        const auto invoke = [&](Json args, std::optional<std::string> parent = {}) {
            editor::ContextOverride overrides;
            overrides.asset = std::move(parent);
            return editor::call(registry, host, context, {"asset.new_material", std::move(args), std::move(overrides)});
        };
        const auto before = project.entries().size();
        (void)invoke(Json::object());
        require(project.entries().size() == before,
                "Noninteractive material creation prompted or changed files without a path.");
        const auto source =
            take(editor::call(registry, host, context,
                              {"asset.new_material_source",
                               {{"path", "Content/Materials/Parent.ms.material"}, {"source", "struct Params {"}}}));
        const auto parent = source.result.at("asset").get<std::string>();
        require(source.status == editor::OperatorStatus::finished && project.entry(parent) &&
                    project.entry(parent)->path == L"Content/Materials/Parent.ms.material" &&
                    take(assets::read_file(folder / "Content/Materials/Parent.ms.source")) == "struct Params {" &&
                    std::holds_alternative<assets::MaterialSourceAsset>(take(project.assets().material(parent)).author),
                "Material source creation did not return the published asset's exact identity.");
        context.asset = "builtin/material/standard-opaque-double";
        Json entries =
            Json::array({{{"path", "gain"}, {"type", "float"}, {"enabled", true}, {"value", .375}},
                         {{"path", "tint"}, {"type", "vector3"}, {"enabled", false}, {"value", {.25, .5, .75}}},
                         {{"path", "removed"},
                          {"type", "texture"},
                          {"enabled", true},
                          {"value", {{"asset_key", ""}, {"address", "mirror"}}}}});
        const auto before_child = project.entries().size();
        const auto child = take(invoke({{"path", "Content/Materials/子材质.material"}, {"entries", entries}}, parent));
        const auto key = child.result.at("asset").get<std::string>();
        const auto author = take(project.assets().material(key));
        require(child.status == editor::OperatorStatus::finished && key != parent &&
                    project.entry(key)->path == L"Content/Materials/子材质.material" &&
                    std::get<assets::MaterialInstanceAsset>(author.author).parent_key == parent &&
                    resources.loaded_material(key) == ObjectId{} && host.panels.material_inspection.empty() &&
                    context.asset == "builtin/material/standard-opaque-double" && !resources.dirty() &&
                    resources.undo_count() == 0 && resources.redo_count() == 0 && resources.recorded() == 0,
                "Material creation lost UTF-8, ignored its context parent, or entered scene history.");
        const auto unchanged = project.entries().size();
        const auto saved = assets::json_format::Json::parse(
            take(assets::read_file(take(project.resolve(project.entry(key)->path)), 1024 * 1024)));
        require(unchanged == before_child + 1 && saved.at("entries") == entries &&
                    assets::material_format::encode_entries(
                        std::get<assets::MaterialInstanceAsset>(author.author).entries) == entries,
                "Initial entries were not published intact in the single created material.");
        entries.back()["value"]["address"] = "invalid";
        (void)invoke({{"path", "Content/Materials/BadEntries.material"}, {"entries", entries}}, parent);
        require(project.entries().size() == unchanged &&
                    !std::filesystem::exists(folder / "Content/Materials/BadEntries.material") &&
                    !std::filesystem::exists(folder / "Content/Materials/BadEntries.material.asset.json") &&
                    resources.recorded() == 0,
                "An invalid final entry published a partial material or history.");
        (void)invoke({{"path", "Content/Materials/Rejected.material"}}, "missing-parent");
        require(project.entries().size() == unchanged, "An invalid context asset reached filesystem publication.");
        (void)editor::call(registry, host, context, {"scene.open", Json::object(), {}});
        require(!resources.dirty() && resources.recorded() == 0 && project.entries().size() == unchanged,
                "Noninteractive document opening changed accepted state without a path.");
    }

    Host host(document);
    editor::ToolSettings current{editor::TransformTool::rotate,
                                 editor::PivotMode::cursor,
                                 true,
                                 false,
                                 .25f,
                                 editor::SelectMode::extend,
                                 editor::Orientation::local,
                                 false,
                                 false,
                                 true,
                                 5,
                                 .25f};
    const math::Vector3 cursor{1, -2, 3};
    const auto context = [&] {
        return editor::Context{document, nullptr, {},    editor::RegionKind::viewport, {}, {}, {}, true,
                               false,    current, cursor};
    };
    const auto call = [&](std::string id, Json args) {
        return editor::call(registry, host, context(), {std::move(id), std::move(args), {}});
    };
    host.panels.dock.reset();
    take(call("layout.show", {{"tab", "code"}}));
    require(host.panels.dock.focused_tab() == editor::source_control::tab,
            "layout.show did not select the requested page.");
    host.panels.dock.select(editor::control::parameters_tab);
    auto region_context = context();
    region_context.region = editor::RegionKind::code;
    take(editor::call(registry, host, region_context, {"layout.cycle_tab", {{"direction", -1}}, {}}));
    require(host.panels.dock.focused_tab() == editor::control::assets_tab,
            "layout.cycle_tab used the focused page instead of its explicit region.");
    region_context.region = editor::RegionKind::assets;
    take(editor::call(registry, host, region_context, {"layout.cycle_tab", {{"direction", 1}}, {}}));
    require(host.panels.dock.focused_tab() == editor::source_control::tab && document.undo_count() == 0 &&
                document.redo_count() == 0,
            "Layout navigation did not move forward without scene history.");
    const auto undo = document.undo_count();
    const auto changes = host.tool_changes;
    (void)call("tool.settings", {{"grid_spacing", 0}});
    require(document.undo_count() == undo && take(document.world().members()).empty() && host.tool_changes == changes,
            "A rejected operator call changed the document or the tools.");

    // A seeded call changes only the named leaf; every other field differs from its default.
    take(call("tool.settings", {{"grid_spacing", .5}}));
    auto expected = current;
    expected.grid_spacing = .5f;
    require(host.tool_changes == 1 && same(host.tools, expected), "tool.settings changed fields it was not given.");
    require(document.undo_count() == 0, "tool.settings recorded scene history.");
    const auto added = take(call("scene.add", {{"kind", "point"}}));
    const auto light = document.selection().entity;
    require(added.status == editor::OperatorStatus::finished && document.undo_count() == 1 &&
                take(document.world().members()).size() == 1 && light != ObjectId{} && host.scene_changes == 1,
            "scene.add did not add and select one Entity in one history entry.");
    const auto parts = take(world::components(runtime, light));
    require(parts.size() == 1 &&
                take(presentation::read_light(runtime, parts.front().id())).kind == presentation::LightKind::point,
            "scene.add {kind: point} did not add a point light.");
    const auto at = take(world::world_matrix(runtime, light));
    require(at[12] == cursor.x && at[13] == cursor.y && at[14] == cursor.z,
            "scene.add did not place the light at the 3D cursor.");
    take(call("scene.add", {{"kind", "mesh"}}));
    require(document.undo_count() == 2 &&
                take(runtime.type_of(document.selection().entity)) == document.presentation_types().static_mesh_entity,
            "scene.add {kind: mesh} did not add the default static mesh.");
    static_cast<void>(call("scene.add", {{"kind", "mesh"}, {"mesh", "missing"}}));
    require(document.undo_count() == 2, "Rejected scene.add changed history.");

    const auto target = host.view.state().target;
    take(call("view.axis", {{"view", "left"}}));
    require(host.view.view() == editor::CameraView::left && host.view.state().target == target &&
                document.undo_count() == 2 && document.redo_count() == 0,
            "view.axis did not switch the view around its target without history.");

    // Scene camera operations share the UI/Control operator path. Viewing copies
    // the full orientation including roll, not just eye/target.
    take(call("scene.add", {{"kind", "camera"}}));
    const auto camera_entity = document.selection().entity;
    const auto camera_component = take(world::components(runtime, camera_entity)).front().id();
    const auto camera_call = [&](std::string id) {
        auto selected = context();
        selected.selection = {camera_entity, camera_entity, std::vector{camera_entity}};
        const auto result = take(editor::call(registry, host, selected, {std::move(id), Json::object(), {}}));
        require(result.status == editor::OperatorStatus::finished, "A camera operator was unexpectedly unavailable.");
    };
    take(runtime.write(camera_component,
                       take(runtime.find_field(document.presentation_types().camera_component, "vertical_fov")), 73.f));
    take(runtime.write(camera_component,
                       take(runtime.find_field(document.presentation_types().camera_component, "near_clip")), .2f));
    take(runtime.write(camera_component,
                       take(runtime.find_field(document.presentation_types().camera_component, "far_clip")), 650.f));
    {
        const math::Quaternion rotation{.2f, .3f, .4f, .8f};
        take(document.set_transform(camera_entity, {{3, -2, 7}, rotation, {-2, 0, 5}}));
        const auto history = document.undo_count();
        const auto saved = take(document.serialize());
        camera_call("view.from_camera");
        require(document.undo_count() == history && take(document.serialize()) == saved,
                "Viewing a camera modified the author document or its history.");
        auto state = host.view.state();
        const auto type = take(runtime.native_type<editor::CameraState>());
        const auto encoded =
            take(editor::encode_properties(runtime, {type, {}, std::as_writable_bytes(std::span{&state, 1})},
                                           take(editor::detail::author_fields(runtime, type)), {}));
        take(call("view.look_at",
                  {{"eye", {{"x", 9}, {"y", 4}, {"z", -2}}}, {"target", {{"x", 1}, {"y", -2}, {"z", 3}}}}));
        require(host.view.target() == math::Vector3{1, -2, 3} &&
                    math::length(host.view.eye() - math::Vector3{9, 4, -2}) < .0001f,
                "Look At lost the explicit eye or target.");
        take(call("view.camera_set", {{"state", encoded}}));
        require(host.view.state() == state && document.undo_count() == history && take(document.serialize()) == saved,
                "Camera state roundtrip lost projection/roll or changed author history.");
    }
    auto* camera_world = take(world::admit_component_edit(runtime, camera_component));
    changed(camera_world->set_enabled(camera_entity, false));
    camera_call("scene.main_camera_set");
    require(take(document.main_camera()) == camera_entity && !document.presentation().main_camera(),
            "Set could not store a disabled camera, or Play used it.");
    changed(camera_world->set_enabled(camera_entity, true));
    require(take(document.presentation().main_camera()).entity == camera_entity,
            "Re-enabling the selected camera did not restore it.");
    camera_call("scene.main_camera_clear");
    require(take(document.main_camera()) == ObjectId{}, "Clear Main Camera kept the selection.");
    {
        editor::Session edited({}, editor::SessionRole::editor);
        Host controls(edited);
        auto& rt = edited.world().runtime();
        changed(edited.add_entity());
        const auto first = edited.selection().entity;
        const world::Trs first_pose{{1.25f, 2.75f, -.6f}, editor::from_euler_degrees({10, 0, 20}), {2, 3, 4}};
        take(edited.set_transform(first, first_pose));
        changed(edited.add_entity());
        const auto second = edited.selection().entity;
        const world::Trs second_pose{{-3.75f, 8.25f, 2.6f}, editor::from_euler_degrees({0, 35, 0}), {5, 6, 7}};
        take(edited.set_transform(second, second_pose));
        changed(edited.add_entity());
        const auto unrelated = edited.selection().entity;
        const world::Trs unrelated_pose{{40, 5, 8}, {}, {1, 1, 1}};
        take(edited.set_transform(unrelated, unrelated_pose));
        const auto history = edited.undo_count();
        auto context = controls.panels.operator_context(edited, nullptr, false);
        context.cursor = {10, 3, -2};
        context.tools.grid_spacing = .5f;
        editor::ContextOverride override;
        override.selection = editor::SelectionContext{first, first, {first, second}};
        const auto invoke = [&](std::string id, Json args = Json::object()) {
            // Undo intentionally selects its edited objects; re-establish a different UI selection for each call.
            take(edited.select(unrelated));
            auto result = take(editor::call(registry, controls, context, {std::move(id), std::move(args), override}));
            require(result.status == editor::OperatorStatus::finished, "Explicit-target operator was unavailable.");
        };
        const auto pose = [&](ObjectId entity) { return take(world::read_transform(rt, entity)); };
        invoke("scene.snap_selection", {{"to", "grid"}});
        require(pose(first).position == math::Vector3{1.5f, 3, -.5f} &&
                    pose(second).position == math::Vector3{-4, 8.5f, 2.5f} && pose(unrelated) == unrelated_pose &&
                    edited.selection().entity == unrelated && edited.undo_count() == history + 1,
                "Grid snap used document selection, changed selection, or recorded multiple entries.");
        changed(edited.undo());
        invoke("scene.snap_selection", {{"to", "cursor"}});
        require(math::length(pose(first).position - math::Vector3{12.5f, .25f, -3.6f}) < .0001f &&
                    math::length(pose(second).position - math::Vector3{7.5f, 5.75f, -.4f}) < .0001f &&
                    pose(unrelated) == unrelated_pose && edited.undo_count() == history + 1,
                "Cursor snap ignored the context cursor or failed to preserve group offsets.");
        changed(edited.undo());
        invoke("scene.transform_clear", {{"channel", "scale"}});
        auto expected_first = first_pose;
        auto expected_second = second_pose;
        expected_first.scale = expected_second.scale = {1, 1, 1};
        require(pose(first) == expected_first && pose(second) == expected_second && pose(unrelated) == unrelated_pose &&
                    edited.selection().entity == unrelated && edited.undo_count() == history + 1,
                "Clear changed the wrong channel, target, selection or history.");
        changed(edited.undo());
        invoke("cursor.to_selection");
        require(math::length(controls.viewport.cursor - math::Vector3{-1.25f, 5.5f, 1}) < .000001f,
                "Cursor to Selection used the document selection instead of Context.");
        context.cursor = {1.25f, -2.75f, .8f};
        invoke("cursor.to_grid");
        require(controls.viewport.cursor == math::Vector3{1.5f, -3, 1},
                "Cursor to Grid used a stale cursor or the wrong grid spacing.");
        controls.viewport.cursor_rotation = editor::from_euler_degrees({30, 0, 45});
        invoke("cursor.reset");
        require(controls.viewport.cursor == math::Vector3{} && controls.viewport.cursor_rotation == math::Quaternion{},
                "Reset Cursor kept position or orientation.");
        invoke("scene.select_all", {{"action", "invert"}});
        require(edited.selected_entities().size() == 1 && edited.selection().entity == unrelated &&
                    edited.undo_count() == history && edited.redo_count() == 1,
                "Invert used document selection rather than the explicit context or moved author history.");
    }
    {
        editor::Session erased({}, editor::SessionRole::editor);
        Host controls(erased);
        changed(erased.add_entity());
        const auto parent = erased.selection().entity;
        changed(erased.add_component(parent, erased.presentation_types().camera_component));
        const auto camera = erased.selection().object;
        changed(erased.add_entity());
        const auto child = erased.selection().entity;
        changed(erased.reparent(child, parent, world::ReparentRule::keep_local));
        changed(erased.add_entity());
        const auto kept = erased.selection().entity;
        const auto before = take(erased.serialize());
        const auto history = erased.undo_count();
        auto context = controls.panels.operator_context(erased, nullptr, false);
        editor::ContextOverride override;
        override.selection = editor::SelectionContext{child, child, {parent, child}};
        const auto deleted =
            take(editor::call(registry, controls, context, {"scene.delete", {{"target", "entities"}}, override}));
        const auto remaining = take(erased.world().members());
        require(deleted.status == editor::OperatorStatus::finished && remaining.size() == 1 &&
                    remaining.front().id() == kept && erased.selection().entity == kept &&
                    erased.selected_entities().size() == 1 && erased.undo_count() == history + 1,
                "Explicit delete lost the unrelated document selection, missed a subtree, or split history.");
        changed(erased.undo());
        require(take(erased.serialize()) == before,
                "Explicit subtree deletion Undo did not restore the complete scene.");
        take(erased.select(kept));
        const auto component_context = controls.panels.operator_context(erased, nullptr, false);
        override.selection = editor::SelectionContext{parent, camera, {parent}};
        take(editor::call(registry, controls, component_context, {"scene.delete", Json::object(), override}));
        require(take(erased.world().members()).size() == 3 &&
                    take(world::components(erased.world().runtime(), parent)).empty() &&
                    erased.selection().entity == kept && erased.undo_count() == history + 1,
                "Object deletion ignored the inspected component or replaced unrelated document selection.");
        changed(erased.undo());
        require(take(erased.serialize()) == before,
                "Component deletion Undo did not restore its identity and author data.");
    }
    {
        editor::Session copied({}, editor::SessionRole::editor);
        Host controls(copied);
        changed(copied.add_entity());
        const auto source = copied.selection().entity;
        take(copied.set_transform(source, {{3, -2, 5}, {}, {2, 1, 3}}));
        changed(copied.add_entity());
        const auto unrelated = copied.selection().entity;
        take(copied.set_transform(unrelated, {{-6, 1, 0}, {}, {1, 1, 1}}));
        const auto history = copied.undo_count();
        auto context = controls.panels.operator_context(copied, nullptr, false);
        editor::ContextOverride override;
        override.selection = editor::SelectionContext{source, source, {source}};
        static_cast<void>(editor::call(registry, controls, context, {"scene.duplicate", {{"place", true}}, override}));
        require(copied.undo_count() == history && take(copied.world().members()).size() == 2,
                "Duplicate placement without a viewport changed the document before refusing.");
        take(editor::call(registry, controls, context, {"scene.duplicate", Json::object(), override}));
        const auto clone = copied.selection().entity;
        require(clone != source && clone != unrelated && take(copied.world().members()).size() == 3 &&
                    take(world::read_transform(copied.world().runtime(), clone)) ==
                        world::Trs{{3, -2, 5}, {}, {2, 1, 3}} &&
                    copied.undo_count() == history + 1,
                "Duplicate used document selection instead of Context or split its history.");
        changed(copied.undo());
        require(take(copied.world().members()).size() == 2,
                "Explicit duplicate Undo did not remove exactly the new object.");
        changed(copied.redo());
        require(copied.selection().entity == clone && take(copied.world().members()).size() == 3,
                "Explicit duplicate Redo changed the new identity.");
        override.region = editor::RegionKind::viewport;
        take(editor::call(registry, controls, context, {"scene.duplicate", {{"place", true}}, override}));
        const auto placed = copied.selection().entity;
        require(controls.viewport.active() && copied.undo_count() == history + 2,
                "Viewport duplicate did not enter the existing placement gesture in one history item.");
        take(controls.viewport.cancel(copied));
        require(take(copied.world().members()).size() == 4 && copied.undo_count() == history + 2 &&
                    take(world::read_transform(copied.world().runtime(), placed)) ==
                        world::Trs{{3, -2, 5}, {}, {2, 1, 3}},
                "Cancelling placement deleted the copy or changed its original transform/history.");
    }
    {
        editor::Session composed({}, editor::SessionRole::editor);
        Host controls(composed);
        changed(composed.add_entity());
        const auto target = composed.selection().entity;
        changed(composed.add_entity());
        const auto unrelated = composed.selection().entity;
        const auto history = composed.undo_count();
        const auto context = controls.panels.operator_context(composed, nullptr, false);
        editor::ContextOverride override;
        override.selection = editor::SelectionContext{target, target, {target}};
        static_cast<void>(
            editor::call(registry, controls, context, {"scene.add_component", {{"type", "world.Entity"}}, override}));
        require(composed.undo_count() == history, "Add Component changed history on refusal.");
        take(editor::call(registry, controls, context,
                          {"scene.add_component", {{"type", "world.CameraComponent"}}, override}));
        const auto camera = composed.selection().object;
        auto& rt = composed.world().runtime();
        require(composed.selection().entity == target && camera != target &&
                    take(world::inspect_component(rt, camera)).owner == target &&
                    take(world::components(rt, unrelated)).empty() && take(world::has_transform(rt, target)) &&
                    composed.undo_count() == history + 1,
                "Add Component used document selection or split the component's required Transform history.");
        changed(composed.undo());
        require(take(world::components(rt, target)).empty() && !take(world::has_transform(rt, target)),
                "Add Component Undo kept the new component or its automatically created Transform.");
        changed(composed.redo());
        require(composed.selection().object == camera && take(world::has_transform(rt, target)),
                "Add Component Redo lost component identity or required Transform.");
    }
    {
        editor::Session transformed({}, editor::SessionRole::editor);
        Host controls(transformed);
        changed(transformed.add_entity());
        const auto target = transformed.selection().entity;
        changed(transformed.add_entity());
        const auto unrelated = transformed.selection().entity;
        const world::Trs other{{-9, 4, 2}, {}, {3, 2, 1}};
        take(transformed.set_transform(unrelated, other));
        const auto history = transformed.undo_count();
        const auto context = controls.panels.operator_context(transformed, nullptr, false);
        editor::ContextOverride override;
        override.selection = editor::SelectionContext{target, target, {target}};
        const auto invoke = [&](Json args) {
            return editor::call(registry, controls, context, {"scene.transform_set", std::move(args), override});
        };
        auto& rt = transformed.world().runtime();
        static_cast<void>(invoke({{"position", {{"x", 7}}}, {"rotation", {{"w", 0}}}}));
        require(!take(world::has_transform(rt, target)) && transformed.undo_count() == history,
                "Rejected transform created spatial state or scene history.");
        const Json values{{"position", {{"x", 3}, {"y", -2}, {"z", 5}}},
                          {"rotation", {{"x", 0}, {"y", 0}, {"z", 1}, {"w", 0}}},
                          {"scale", {{"x", -2}, {"y", 0}, {"z", 4}}}};
        const world::Trs expected{{3, -2, 5}, {0, 0, 1, 0}, {-2, 0, 4}};
        take(invoke(values));
        require(take(world::read_transform(rt, target)) == expected &&
                    take(world::read_transform(rt, unrelated)) == other &&
                    transformed.selection().entity == unrelated && transformed.undo_count() == history + 1,
                "Transform operator ignored Context, changed selection, or split its history.");
        take(invoke(values));
        require(transformed.undo_count() == history + 1, "Repeated transform added a redundant history entry.");
        changed(transformed.undo());
        require(!take(world::has_transform(rt, target)) && take(world::read_transform(rt, unrelated)) == other,
                "Transform Undo did not restore the absent Transform without changing another Entity.");
        changed(transformed.redo());
        require(take(world::read_transform(rt, target)) == expected,
                "Transform Redo lost the explicit rotation, mirrored axis or zero scale.");
    }
    {
        editor::Session spaces({}, editor::SessionRole::editor);
        Host controls(spaces);
        changed(spaces.add_entity());
        const auto parent = spaces.selection().entity;
        take(spaces.set_transform(parent, {{10, 0, 0}, {}, {1, 1, 1}}));
        changed(spaces.add_entity());
        const auto target = spaces.selection().entity;
        take(spaces.set_transform(target, {{2, 1, 0}, {}, {1, 1, 1}}));
        changed(spaces.reparent(target, parent, world::ReparentRule::keep_local));
        changed(spaces.add_entity());
        const auto origin = spaces.selection().entity;
        take(spaces.set_transform(origin, {{-4, 3, 0}, {}, {1, 1, 1}}));
        auto& rt = spaces.world().runtime();
        const auto history = spaces.undo_count();
        const auto context = controls.panels.operator_context(spaces, nullptr, false);
        editor::ContextOverride override;
        override.selection = editor::SelectionContext{target, target, {target}};
        const editor::ResolveReference resolve = [&](const Json& value, TypeId,
                                                     ValueForm) -> std::optional<ObjectReference> {
            return value == "origin" ? std::optional{take(rt.observe(take(rt.hold(origin)).reference()))}
                                     : std::nullopt;
        };
        const auto invoke = [&](Json args) {
            return editor::call(registry, controls, context, {"scene.space_reference_set", std::move(args), override},
                                resolve);
        };
        static_cast<void>(invoke({{"origin", "origin"}, {"world_space", true}}));
        require(!take(world::space_reference(rt, target)) && spaces.undo_count() == history,
                "Conflicting spatial origins changed the document or history.");
        take(invoke({{"world_space", true}}));
        require(take(world::space_reference(rt, target)) == std::optional{ObjectId{}} &&
                    take(world::read_transform(rt, target)).position == math::Vector3{12, 1, 0} &&
                    take(world::parent_entity(rt, target)) == parent && spaces.undo_count() == history + 1,
                "Explicit World space became inherited space, changed composition, or lost world position.");
        take(invoke({{"origin", "origin"}}));
        require(take(world::space_reference(rt, target)) == std::optional{origin} &&
                    take(world::read_transform(rt, target)).position == math::Vector3{16, -2, 0} &&
                    take(world::read_transform(rt, origin)).position == math::Vector3{-4, 3, 0} &&
                    spaces.undo_count() == history + 2,
                "Named space reference did not keep the world pose on the explicit Context Entity.");
        changed(spaces.undo());
        require(take(world::space_reference(rt, target)) == std::optional{ObjectId{}} &&
                    take(world::read_transform(rt, target)).position == math::Vector3{12, 1, 0},
                "Space-reference Undo did not restore explicit World space and its local pose.");
        changed(spaces.redo());
        take(invoke({{"keep_world", false}}));
        require(!take(world::space_reference(rt, target)) &&
                    take(world::read_transform(rt, target)).position == math::Vector3{16, -2, 0} &&
                    take(world::world_matrix(rt, target))[12] == 26 && spaces.undo_count() == history + 3,
                "Null origin did not restore inheritance while preserving the requested local pose.");
    }
    {
        editor::Session named({}, editor::SessionRole::editor);
        Host controls(named);
        changed(named.add_entity());
        const auto target = named.selection().entity;
        const std::wstring before(named.name(target));
        changed(named.add_entity());
        const auto unrelated = named.selection().entity;
        const std::wstring other(named.name(unrelated));
        const auto history = named.undo_count();
        const auto context = controls.panels.operator_context(named, nullptr, false);
        editor::ContextOverride override;
        override.selection = editor::SelectionContext{target, target, {target}};
        const auto invoke = [&](Json args) {
            return editor::call(registry, controls, context, {"scene.rename", std::move(args), override});
        };
        static_cast<void>(invoke(Json::object()));
        require(named.undo_count() == history && named.name(target) == before,
                "Noninteractive Rename without a name did not fail without editing.");
        take(invoke({{"name", "房间 · North"}}));
        require(named.name(target) == L"房间 · North" && named.name(unrelated) == other &&
                    named.selection().entity == unrelated && named.undo_count() == history + 1,
                "Rename used document selection, changed selection, or lost Unicode/history.");
        take(invoke({{"name", "房间 · North"}}));
        require(named.undo_count() == history + 1, "Repeated Rename added history.");
        changed(named.undo());
        require(named.name(target) == before, "Rename Undo did not restore the original label.");
        changed(named.redo());
        require(named.name(target) == L"房间 · North", "Rename Redo did not restore the Unicode label.");
    }
    {
        const auto folder = std::filesystem::current_path() / "out/operator-patch-evidence" /
                            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        auto project = take(editor::Project::create(folder));
        take(project.create_source("Source/Patch.source", R"(
class patch.Actor : world.Entity { int a = 3; int z = 11; }
class patch.Fields : world.Component { float position = 4.0f; float rotation = 5.0f; float scale = 6.0f; }
)"));
        editor::Session patched(
            [&](world::World& world) -> Result<std::shared_ptr<void>> {
                auto code = std::make_shared<editor::ProjectCode>(world);
                if (auto built = code->build(project); !built)
                    return std::unexpected(built.error());
                return std::static_pointer_cast<void>(code);
            },
            editor::SessionRole::editor);
        Host controls(patched);
        auto& rt = patched.world().runtime();
        const auto actor_type = take(rt.find_type("patch.Actor"));
        changed(patched.add_entity(actor_type));
        const auto entity = patched.selection().entity;
        const world::Trs pose{{3, -2, 5}, {}, {2, 1, 4}};
        take(patched.set_transform(entity, pose));
        const std::array a{take(rt.find_field(actor_type, "a"))};
        const std::array z{take(rt.find_field(actor_type, "z"))};
        take(patched.set_property(entity, a, std::int32_t{7}, 101));
        changed(patched.undo());
        patched.mark_saved();
        const auto history = patched.undo_count();
        const auto redo = patched.redo_count();
        const auto recorded = patched.recorded();
        const auto invoke = [&](ObjectId object, Json args) {
            auto context = controls.panels.operator_context(patched, &project, false);
            editor::ContextOverride override;
            override.selection = editor::SelectionContext{entity, object, {entity}};
            return editor::call(registry, controls, context, {"scene.set_properties", std::move(args), override});
        };
        take(patched.begin_edit(entity, a, 102));
        take(patched.preview_edit(102, Scalar{std::int32_t{5}}));
        // JSON visits a before the forbidden position: rejecting the tail must preserve the active edit.
        static_cast<void>(invoke(entity, {{"a", 8}, {"position", {{"x", 90}}}}));
        require(take(world::read_transform(rt, entity)) == pose &&
                    take(rt.read(entity, a)) == Scalar{std::int32_t{5}} &&
                    take(rt.read(entity, z)) == Scalar{std::int32_t{11}} && patched.owns_edit(102) && patched.dirty() &&
                    patched.undo_count() == history && patched.redo_count() == redo && patched.recorded() == recorded,
                "A Transform patch changed values, cancelled the existing edit, or changed history.");
        take(patched.cancel_edit());
        require(!patched.dirty(), "Transform refusal invalidated the saved cursor.");
        static_cast<void>(invoke(entity, {{"position", {{"x", 90}}}}));
        require(!patched.dirty() && patched.undo_count() == history && patched.redo_count() == redo &&
                    patched.recorded() == recorded,
                "Pure Transform patch dirtied a clean document or changed history.");
        take(invoke(entity, {{"a", 8}, {"z", 23}}));
        require(take(rt.read(entity, a)) == Scalar{std::int32_t{8}} &&
                    take(rt.read(entity, z)) == Scalar{std::int32_t{23}} &&
                    take(world::read_transform(rt, entity)) == pose && patched.undo_count() == history + 1 &&
                    patched.redo_count() == 0 && patched.recorded() == recorded + 1,
                "Source Entity business patch did not remain one history item.");
        changed(patched.undo());
        require(take(rt.read(entity, a)) == Scalar{std::int32_t{3}} &&
                    take(rt.read(entity, z)) == Scalar{std::int32_t{11}} && !patched.dirty(),
                "Business patch Undo failed to restore both leaves and the saved cursor.");
        changed(patched.redo());
        require(take(rt.read(entity, a)) == Scalar{std::int32_t{8}} &&
                    take(rt.read(entity, z)) == Scalar{std::int32_t{23}},
                "Business patch Redo did not restore both leaves.");
        const auto fields_type = take(rt.find_type("patch.Fields"));
        changed(patched.add_component(entity, fields_type));
        const auto fields = patched.selection().object;
        take(invoke(fields, {{"position", 7}, {"rotation", 8}, {"scale", 9}}));
        for (const auto& [name, expected] :
             std::array{std::pair{"position", 7.f}, std::pair{"rotation", 8.f}, std::pair{"scale", 9.f}})
            require(take(rt.read(fields, std::array{take(rt.find_field(fields_type, name))})) == Scalar{expected},
                    "A non-Transform business field was rejected by name.");
        changed(patched.add_component(entity, patched.types().collision_component));
        const auto collision = patched.selection().object;
        physics::BodySettings settings;
        settings.ignore_categories = 1;
        settings.overlap_categories = 2;
        take(patched.configure_body(collision, settings));
        const auto body_history = patched.undo_count();
        take(invoke(collision, {{"body", {{"ignore_categories", 2}, {"overlap_categories", 1}}}}));
        auto swapped = settings;
        swapped.ignore_categories = 2;
        swapped.overlap_categories = 1;
        require(take(physics::read_settings(rt, collision)) == swapped && patched.undo_count() == body_history + 1,
                "Body patch validated an intermediate leaf or split its history.");
        // The World reports validation on the whole Component, including the body's field path.
        // A detached object supplies that cause without changing the document or its history.
        auto invalid_component = take(world::create_component(rt, patched.types().collision_component));
        const auto body = take(rt.find_field(patched.types().collision_component, "body"));
        const auto body_type = take(rt.native_type<world::BodySettings>());
        for (const auto* mask : {"ignore_categories", "overlap_categories"})
            take(rt.write(invalid_component.id(), std::array{body, take(rt.find_field(body_type, mask))}, Scalar{2u}));
        const auto cause = take(rt.validate(invalid_component.id()));
        const auto refused = invoke(collision, {{"body", {{"ignore_categories", 2}, {"overlap_categories", 2}}}});
        require(cause && !refused && refused.error().code == cause->error.code &&
                    refused.error().message == cause->error.message && !refused.error().source &&
                    !cause->error.source && take(physics::read_settings(rt, collision)) == swapped &&
                    patched.undo_count() == body_history + 1,
                "Invalid body patch lost the original cause, changed values, or added history.");
        changed(patched.undo());
        require(take(physics::read_settings(rt, collision)) == settings,
                "Body patch Undo did not restore both masks together.");
        changed(patched.redo());
        require(take(physics::read_settings(rt, collision)) == swapped,
                "Body patch Redo did not replay both masks together.");
    }
    {
        editor::Session display({}, editor::SessionRole::editor);
        Host controls(display);
        auto before = controls.panels.view;
        before.wire_width = 2.25f;
        before.reference_grid = false;
        before.cavity.ridge = .35f;
        take(controls.panels.set_view_settings(display, before));
        const auto history = display.undo_count();
        const auto invoke = [&](Json args) {
            return editor::call(registry, controls, controls.panels.operator_context(display, nullptr, false),
                                {"view.set", std::move(args), {}});
        };
        take(invoke({{"shading", "flat"}, {"origin_axes", false}}));
        auto expected_view = before;
        expected_view.shading = renderer::Shading::flat;
        expected_view.origin_axes = false;
        require(controls.panels.view == expected_view && display.undo_count() == history,
                "view.set overwrote an unnamed display field or recorded display-only history.");
        take(invoke({{"exposure_ev", 2.5}, {"sky_intensity", 3.25}}));
        const auto environment = take(display.presentation().environment());
        require(environment.exposure_ev == 2.5f && environment.sky_intensity == 3.25f &&
                    display.undo_count() == history + 1,
                "view.set did not group World author fields into one history entry.");
        const auto published = controls.panels.view;
        const auto saved = take(display.serialize());
        static_cast<void>(invoke({{"probe_volume_0_min_x", 10000}, {"wire_width", .75}}));
        require(controls.panels.view == published && take(display.serialize()) == saved &&
                    display.undo_count() == history + 1,
                "A rejected view combination partially changed display or World author state.");
        changed(display.undo());
        controls.panels.sync_world(display);
        require(controls.panels.view == expected_view,
                "Undo did not restore World fields while preserving non-history display fields.");
    }
    {
        editor::Session parents({}, editor::SessionRole::editor);
        auto& rt = parents.world().runtime();
        changed(parents.add_entity());
        const auto parent = parents.selection().entity;
        changed(parents.add_entity());
        const auto first = parents.selection().entity;
        changed(parents.add_entity());
        const auto second = parents.selection().entity;
        changed(parents.reparent(first, parent, world::ReparentRule::keep_local));
        const editor::Context target{parents,
                                     nullptr,
                                     {second, second, {first, second}},
                                     editor::RegionKind::scene_tree,
                                     {},
                                     {},
                                     {},
                                     true,
                                     false,
                                     {},
                                     {}};
        const auto history = parents.undo_count();
        const auto* op = registry.find("scene.parent_set");
        require(op != nullptr, "scene.parent_set is not registered.");
        const auto moved = editor::call(registry, host, target, {"scene.parent_set", {{"parent", nullptr}}, {}});
        require(moved && moved->status == editor::OperatorStatus::finished && parents.undo_count() == history + 1 &&
                    take(world::parent_entity(rt, first)) == ObjectId{} &&
                    take(world::parent_entity(rt, second)) == ObjectId{},
                "scene.parent_set did not unparent its explicit selection in one history entry.");
        const editor::ResolveReference resolve = [&](const Json& value, TypeId type,
                                                     ValueForm form) -> std::optional<ObjectReference> {
            require(type == parents.types().entity && form == ValueForm::weak_reference,
                    "The parent token was not resolved as a weak Entity reference.");
            if (value != "parent")
                return {};
            return take(rt.observe(take(rt.hold(parent)).reference()));
        };
        const auto before = take(parents.serialize());
        static_cast<void>(
            editor::call(registry, host, target, {"scene.parent_set", {{"parent", "unknown"}}, {}}, resolve));
        require(parents.undo_count() == history + 1 && take(parents.serialize()) == before,
                "An unresolved parent token changed the scene or its history.");
        const auto attached = editor::call(
            registry, host, target, {"scene.parent_set", {{"parent", "parent"}, {"keep_world", false}}, {}}, resolve);
        require(attached && attached->status == editor::OperatorStatus::finished &&
                    parents.undo_count() == history + 2 && take(world::parent_entity(rt, first)) == parent &&
                    take(world::parent_entity(rt, second)) == parent,
                "scene.parent_set did not resolve a non-null parent and group both moves.");
        changed(parents.undo());
        require(take(parents.serialize()) == before, "Undo did not restore the operator's grouped parent edits.");
    }
    {
        // Real menu projection: fixed parameters may persist, but checked state
        // and seeded unnamed fields must follow the current document/context.
        auto menus = std::make_unique<editor::Session>(editor::Session::Prepare{}, editor::SessionRole::editor);
        editor::Workspace workspace;
        const std::array fonts{(editor::asset_directory() / "fonts/SarasaUiSC-Regular.ttf").wstring()};
        ui::Session surface(fonts);
        const auto theme = ui::StyleSheet::parse(take(assets::read_file(editor::asset_directory() / "editor.ss")));
        require(theme && surface.set_style_sheet(*theme), "Menu stylesheet failed.");
        require(surface.set_surface({{1600, 1000}, 1, 1}).has_value(), "Menu surface failed.");
        const auto present = [&] {
            const auto controls = workspace.describe(*menus, {1600, 1000}, surface.presented().get());
            require(surface.set_controls(controls).has_value(), "Menu controls were rejected.");
            workspace.commit_ui_controls();
            const auto frame = surface.prepare();
            if (!frame)
                throw std::runtime_error("Menu layout failed at control " + std::to_string(frame.error().key) +
                                         " code=" + std::to_string(static_cast<unsigned>(frame.error().code)));
            require(surface.accept(*frame), "Menu host frame was rejected.");
        };
        const auto item = [&](ui::ControlKey group, const editor::OperatorCall& request) -> const ui::MenuItem& {
            const auto branch = std::ranges::find(workspace.menu_items, group, &ui::MenuItem::key);
            require(branch != workspace.menu_items.end(), "Expected menu branch is absent.");
            const auto found = std::ranges::find_if(branch->children, [&](const auto& entry) {
                const auto* binding = workspace.binding(entry.key);
                return binding && binding->call && binding->call->request.id == request.id &&
                       binding->call->request.args == request.args;
            });
            require(found != branch->children.end(), "Expected menu call is absent.");
            return *found;
        };
        const editor::OperatorCall top{"view.axis", {{"view", "top"}}};
        const editor::OperatorCall snap{"tool.toggle", {{"setting", "grid_snap"}}};
        const editor::OperatorCall spacing{"tool.settings", {{"grid_spacing", .5}}};
        workspace.camera_view = editor::CameraView::front;
        workspace.viewport_settings.grid_snap = false;
        workspace.viewport_settings.grid_spacing = .5f;
        workspace.menu = ui::ContextMenuRequest{editor::control::view_menu};
        present();
        const auto retired = item(editor::control::viewport_camera, top).key;
        require(!item(editor::control::viewport_camera, top).selected &&
                    !item(editor::control::viewport_snap, snap).selected &&
                    item(editor::control::viewport_snap, spacing).selected,
                "Initial menu state disagrees with the current camera/tools.");
        workspace.camera_view = editor::CameraView::top;
        workspace.viewport_settings.grid_snap = true;
        workspace.viewport_settings.rotation_step = 45;
        present();
        require(item(editor::control::viewport_camera, top).selected &&
                    item(editor::control::viewport_snap, snap).selected &&
                    item(editor::control::viewport_snap, spacing).selected,
                "Menu state or a seeded unnamed field was frozen with its first parameter record.");
        workspace.menu.reset();
        present();
        require(!workspace.binding(retired), "A closed menu retained its executable call.");
        workspace.menu = ui::ContextMenuRequest{editor::control::view_menu};
        present();
        workspace.reset_document();
        changed(menus->close());
        menus = std::make_unique<editor::Session>(editor::Session::Prepare{}, editor::SessionRole::editor);
        workspace.camera_view = editor::CameraView::left;
        workspace.menu = ui::ContextMenuRequest{editor::control::view_menu};
        present();
        require(!item(editor::control::viewport_camera, top).selected && !workspace.binding(retired),
                "Document replacement reused retired menu state or call identity.");
        workspace.reset_document();
        changed(menus->close());
    }
    std::puts("Operator parameters: PASS (seeded patch, explicit Context, atomic history, author values, first cause)");
}
