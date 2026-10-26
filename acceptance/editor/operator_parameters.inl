// Operator parameters: editor.* records published only by the editor's document,
// their schema, strict argument decoding and the commands that fix arguments.
#include "editor/operators.hpp"

namespace operator_parameters {
struct Host final : editor::OperatorHost {
    editor::Camera view;
    editor::ToolSettings tools;
    int tool_changes{}, scene_changes{};
    Result<void> end_interaction() override { return {}; }
    const editor::Camera& camera() const noexcept override { return view; }
    Result<void> set_camera(const editor::Camera& camera) override { view = camera; return {}; }
    void reset_layout() override {}
    Result<void> close_tab(editor::RegionKind, bool) override { return {}; }
    void maximize_tab(editor::RegionKind) override {}
    Result<void> source_history(bool) override { return {}; }
    Result<void> workbench_history(bool) override { return {}; }
    void scene_changed() override { ++scene_changes; }
    void set_tool_settings(const editor::ToolSettings& settings) override { tools = settings; ++tool_changes; }
};
bool same(const editor::ToolSettings& a, const editor::ToolSettings& b) {
    return a.tool == b.tool && a.pivot == b.pivot && a.grid_snap == b.grid_snap && a.cursor_surface == b.cursor_surface &&
        a.grid_spacing == b.grid_spacing && a.select_mode == b.select_mode && a.orientation == b.orientation && a.snap_move == b.snap_move &&
        a.snap_rotate == b.snap_rotate && a.snap_scale == b.snap_scale && a.rotation_step == b.rotation_step && a.scale_step == b.scale_step;
}
}
void verify_operator_parameters() {
    using assets::json_format::Json;
    using namespace operator_parameters;
    const std::array names{"editor.TransformTool", "editor.PivotMode", "editor.SelectMode", "editor.Orientation", "editor.CameraView",
        "editor.AddKind", "editor.ToolSettings", "editor.SceneAdd", "editor.ViewAxis"};
    {
        editor::Session standalone;
        for (const auto* name : names) require(!standalone.world().runtime().find_type(name), "A standalone Session published an editor type.");
    }
    editor::Session document({}, editor::SessionRole::editor);
    auto& runtime = document.world().runtime();
    for (const auto* name : names) require(runtime.find_type(name).has_value(), "The editor document did not publish an editor type.");
    editor::OperatorRegistry registry;editor::register_builtin_operators(registry);

    // The schema follows the declared records: names, enum members, hard and UI bounds, defaults.
    const auto schema = [&](std::string_view id) { return take(editor::parameter_schema(runtime, *registry.find(id))); };
    const auto empty = schema("view.frame_all");
    require(empty.at("properties").empty() && !empty.at("additionalProperties").get<bool>() && !empty.contains("title"),
        "A parameterless operator published parameters.");
    const auto tools = schema("tool.settings");
    require(tools.at("title") == "editor.ToolSettings" && tools.at("seeded").get<bool>() && !tools.at("additionalProperties").get<bool>(),
        "tool.settings is not described as a seeded editor.ToolSettings record.");
    std::vector<std::string> fields;for (const auto& [name, value] : tools.at("properties").items()) fields.push_back(name);
    std::ranges::sort(fields);
    require(fields == std::vector<std::string>{"cursor_surface", "grid_snap", "grid_spacing", "orientation", "pivot", "rotation_step",
        "scale_step", "select_mode", "snap_move", "snap_rotate", "snap_scale", "tool"}, "tool.settings does not list the ToolSettings fields.");
    const auto& spacing = tools.at("properties").at("grid_spacing");
    require(spacing.at("type") == "number" && spacing.at("minimum") == double(.0001f) && spacing.at("maximum") == double(10000.f) &&
        spacing.at("ui_min") == double(.1f) && spacing.at("ui_max") == double(1.f) && !spacing.contains("default"),
        "grid_spacing does not publish its bounds, or a seeded field published a default.");
    require(tools.at("properties").at("tool").at("enum") == Json{"select", "transform", "move", "rotate", "scale"} &&
        tools.at("properties").at("snap_scale").at("type") == "boolean", "ToolSettings field kinds are wrong.");
    const auto add = schema("scene.add");
    require(add.at("title") == "editor.SceneAdd" && !add.contains("seeded") &&
        add.at("properties").at("kind").at("enum") == Json{"entity", "mesh", "point", "rectangle", "directional", "camera"} &&
        add.at("properties").at("kind").at("default") == "entity" && add.at("properties").at("mesh").at("type") == "string" &&
        add.at("properties").at("mesh").at("default") == "", "scene.add does not describe editor.SceneAdd with its defaults.");
    require(schema("view.axis").at("properties").at("view").at("default") == "perspective", "view.axis lost its default view.");

    Host host;
    editor::ToolSettings current{editor::TransformTool::rotate, editor::PivotMode::cursor, true, false, .25f, editor::SelectMode::extend,
        editor::Orientation::local, false, false, true, 5, .25f};
    const math::Vector3 cursor{1, -2, 3};
    const auto context = [&] { return editor::Context{document, nullptr, {}, editor::RegionKind::viewport, {}, {}, {}, true, false, current, cursor}; };
    const auto call = [&](std::string id, Json args) { return editor::call(registry, host, context(), {std::move(id), std::move(args), {}}); };
    const auto rejected = [&](std::string id, Json args, const char* message) {
        const auto undo = document.undo_count();
        const auto changes = host.tool_changes;
        const auto result = call(std::move(id), std::move(args));
        require(!result && result.error().code == ErrorCode::argument_mismatch, message);
        require(document.undo_count() == undo && take(document.world().members()).empty() && host.tool_changes == changes,
            "A rejected operator call changed the document or the tools.");
    };
    rejected("scene.add", {{"colour", 1}}, "scene.add accepted an unknown field.");
    rejected("scene.add", {{"kind", 3}}, "scene.add accepted a number for an enum.");
    rejected("scene.add", {{"kind", "sky"}}, "scene.add accepted an undeclared kind.");
    rejected("tool.settings", {{"grid_spacing", 0}}, "tool.settings accepted a value below the hard minimum.");
    rejected("tool.settings", {{"rotation_step", 361}}, "tool.settings accepted a value above the hard maximum.");
    rejected("tool.settings", {{"grid_snap", 1}}, "tool.settings accepted a number for a boolean.");
    rejected("view.frame_all", {{"view", "top"}}, "A parameterless operator accepted an argument.");

    // A seeded call changes only the named leaf; every other field differs from its default.
    take(call("tool.settings", {{"grid_spacing", .5}}));
    auto expected = current;expected.grid_spacing = .5f;
    require(host.tool_changes == 1 && same(host.tools, expected), "tool.settings changed fields it was not given.");
    require(document.undo_count() == 0, "tool.settings recorded scene history.");

    const auto added = take(call("scene.add", {{"kind", "point"}}));
    const auto light = document.selection().entity;
    require(added.status == editor::OperatorStatus::finished && document.undo_count() == 1 && take(document.world().members()).size() == 1 &&
        light != ObjectId{} && host.scene_changes == 1, "scene.add did not add and select one Entity in one history entry.");
    const auto parts = take(world::components(runtime, light));
    require(parts.size() == 1 && take(presentation::read_light(runtime, parts.front().id())).kind == presentation::LightKind::point,
        "scene.add {kind: point} did not add a point light.");
    const auto at = take(world::world_matrix(runtime, light));
    require(at[12] == cursor.x && at[13] == cursor.y && at[14] == cursor.z, "scene.add did not place the light at the 3D cursor.");
    take(call("scene.add", {{"kind", "mesh"}}));
    require(document.undo_count() == 2 && take(runtime.type_of(document.selection().entity)) == document.presentation_types().static_mesh_entity,
        "scene.add {kind: mesh} did not add the default static mesh.");
    require(!call("scene.add", {{"kind", "mesh"}, {"mesh", "missing"}}) && document.undo_count() == 2, "scene.add accepted an unknown mesh.");

    const auto target = host.view.state().target;
    take(call("view.axis", {{"view", "left"}}));
    require(host.view.view() == editor::CameraView::left && host.view.state().target == target && document.undo_count() == 2 &&
        document.redo_count() == 0, "view.axis did not switch the view around its target without history.");

    // Commands with fixed arguments decode into the values their menu items name.
    using enum editor::Command;
    struct Expect { editor::Command command; std::function<bool()> holds; };
    const auto tool = [&](editor::TransformTool value) { return [&, value] { return host.tools.tool == value; }; };
    const auto mode = [&](editor::SelectMode value) { return [&, value] { return host.tools.select_mode == value; }; };
    const auto axis = [&](editor::CameraView value) { return [&, value] { return host.view.view() == value; }; };
    const std::vector<Expect> commands{
        {tool_select, tool(editor::TransformTool::select)}, {tool_transform, tool(editor::TransformTool::transform)},
        {tool_move, tool(editor::TransformTool::move)}, {tool_rotate, tool(editor::TransformTool::rotate)},
        {tool_scale, tool(editor::TransformTool::scale)},
        {pivot_median, [&] { return host.tools.pivot == editor::PivotMode::median; }},
        {pivot_cursor, [&] { return host.tools.pivot == editor::PivotMode::cursor; }},
        {snap_grid_01, [&] { return host.tools.grid_spacing == .1f; }}, {snap_grid_02, [&] { return host.tools.grid_spacing == .2f; }},
        {snap_grid_05, [&] { return host.tools.grid_spacing == .5f; }}, {snap_grid_1, [&] { return host.tools.grid_spacing == 1.f; }},
        {snap_rotation_1, [&] { return host.tools.rotation_step == 1.f; }}, {snap_rotation_5, [&] { return host.tools.rotation_step == 5.f; }},
        {snap_rotation_15, [&] { return host.tools.rotation_step == 15.f; }}, {snap_rotation_45, [&] { return host.tools.rotation_step == 45.f; }},
        {orientation_global, [&] { return host.tools.orientation == editor::Orientation::global; }},
        {orientation_local, [&] { return host.tools.orientation == editor::Orientation::local; }},
        {select_mode_set, mode(editor::SelectMode::set)}, {select_mode_extend, mode(editor::SelectMode::extend)},
        {select_mode_subtract, mode(editor::SelectMode::subtract)}, {select_mode_invert, mode(editor::SelectMode::invert)},
        {select_mode_intersect, mode(editor::SelectMode::intersect)},
        {view_perspective, axis(editor::CameraView::perspective)}, {view_top, axis(editor::CameraView::top)},
        {view_front, axis(editor::CameraView::front)}, {view_right, axis(editor::CameraView::right)},
        {view_bottom, axis(editor::CameraView::bottom)}, {view_back, axis(editor::CameraView::back)},
        {view_left, axis(editor::CameraView::left)}};
    for (const auto& [command, holds] : commands) {
        // Start each from the other end of its range so a wrong index cannot already hold.
        current = {editor::TransformTool::scale, editor::PivotMode::cursor, false, true, 7, editor::SelectMode::intersect,
            editor::Orientation::local, true, true, false, 90, .5f};
        host.tools = current;host.view.set_view(command == view_left ? editor::CameraView::top : editor::CameraView::left);
        if (command == tool_scale) current.tool = host.tools.tool = editor::TransformTool::select;
        if (command == select_mode_intersect) current.select_mode = host.tools.select_mode = editor::SelectMode::set;
        if (command == pivot_cursor) current.pivot = host.tools.pivot = editor::PivotMode::median;
        if (command == orientation_local) current.orientation = host.tools.orientation = editor::Orientation::global;
        const auto request = editor::operator_for(command);
        require(request && take(editor::call(registry, host, context(), *request)).status == editor::OperatorStatus::finished && holds(),
            "A command's fixed arguments did not decode into the value it names.");
    }
    const auto before = document.undo_count();
    for (const auto command : {create_entity, create_static_mesh, create_point_light, create_rectangle_light, create_directional_light,create_camera})
        take(editor::call(registry, host, context(), *editor::operator_for(command)));
    require(document.undo_count() == before + 6, "A Create command did not add one Entity.");
    for (const auto& definition : editor::command_definitions)
        if (const auto request = editor::operator_for(definition.id)) take(editor::call(registry, host, context(), *request));
    // Scene camera operations share the UI/Control operator path. Viewing copies
    // the full orientation (including roll and vertical views), not just eye/target.
    take(call("scene.add",{{"kind","camera"}}));
    const auto camera_entity=document.selection().entity;
    const auto camera_component=take(world::components(runtime,camera_entity)).front().id();
    const auto camera_call=[&](std::string id) {
        auto selected=context();selected.selection={camera_entity,camera_entity,std::vector{camera_entity}};
        const auto result=take(editor::call(registry,host,selected,{std::move(id),Json::object(),{}}));
        require(result.status==editor::OperatorStatus::finished,"A camera operator was unexpectedly unavailable.");
    };
    take(runtime.write(camera_component,take(runtime.find_field(document.presentation_types().camera_component,"vertical_fov")),73.f));
    take(runtime.write(camera_component,take(runtime.find_field(document.presentation_types().camera_component,"near_clip")),.2f));
    take(runtime.write(camera_component,take(runtime.find_field(document.presentation_types().camera_component,"far_clip")),650.f));
    for(const auto rotation:std::array{math::Quaternion{.2f,.3f,.4f,.8f},math::Quaternion{1,0,0,1},math::Quaternion{1,0,0,-1}}) {
        take(document.set_transform(camera_entity,{{3,-2,7},rotation,{-2,0,5}}));
        const auto source=take(document.presentation().camera(camera_entity));
        const auto expected=take(source.matrices(1.7f));
        const auto history=document.undo_count();const auto saved=take(document.serialize());
        camera_call("view.from_camera");
        const auto actual=host.view.matrices(1.7f);
        for(unsigned i=0;i<16;++i) require(std::abs(actual.view[i]-expected.view[i])<.0002f &&
            std::abs(actual.projection[i]-expected.projection[i])<.0002f,"View from Camera lost pose, roll or projection.");
        require(actual.far_depth==expected.far_depth,"View from Camera lost its independent far boundary.");
        require(document.undo_count()==history && take(document.serialize())==saved,"Viewing a camera modified the author document or its history.");
        editor::Camera restored;require(restored.set_state(host.view.state()) && restored.state()==host.view.state(),"CameraState did not preserve the copied projection and roll.");
    }
    // Normal authoring ranges: perspective depth is reciprocal with an infinite
    // far endpoint; orthographic depth is linear over a finite interval.
    for(const auto clips:std::array{std::array{.2f,650.f},std::array{1.5f,40.f},std::array{.05f,30000.f}}) {
        take(runtime.write(camera_component,take(runtime.find_field(document.presentation_types().camera_component,"near_clip")),clips[0]));
        take(runtime.write(camera_component,take(runtime.find_field(document.presentation_types().camera_component,"far_clip")),clips[1]));
        camera_call("view.from_camera");
        for(const auto view:{editor::CameraView::perspective,editor::CameraView::front}) {
            auto state=host.view.state();state.view=view;
            require(host.view.set_state(state),"A finite, ordered camera clip interval was rejected.");
            const auto matrices=host.view.matrices(1.7f);const auto& projection=matrices.projection;
            require(math::is_finite(matrices.view) && math::is_finite(projection),"A legal camera produced a nonfinite matrix.");
            const auto depth=[&](double z) {return (z*projection[10]+projection[14])/(z*projection[11]+projection[15]);};
            const bool perspective=view==editor::CameraView::perspective;
            const double far_depth=perspective?double(clips[0])/clips[1]:0;
            require(std::abs(matrices.far_depth-far_depth)<.000001,"Camera snapshot lost its derived far boundary.");
            require(std::abs(depth(clips[0])-1)<.00001 && std::abs(depth(clips[1])-far_depth)<.00001,
                "Camera depth did not match infinite perspective or finite orthographic projection.");
            const double mid=(double(clips[0])+clips[1])*.5;
            const double expected=perspective?double(clips[0])/mid:
                (double(clips[1])-mid)/(double(clips[1])-clips[0]);
            require(std::abs(depth(mid)-expected)<.00001,"Camera depth interpolation did not match its projection kind.");
            if(perspective) {
                const auto inverse=math::try_inverse(projection);require(bool(inverse),"Infinite perspective was not invertible.");
                const auto direction=math::Vector4{0,0,0,1}*(*inverse);
                require(direction.w==0 && direction.z>0,"Depth zero was not an infinite forward direction.");
            }
            const auto before=host.view.state();state.far_clip=state.near_clip;
            require(!host.view.set_state(state) && host.view.state()==before,"Equal clip planes were accepted or changed the camera.");
            std::printf("Camera projection %s near=%.9g far=%.9g: PASS (normal range, depth mapping, midpoint, equality rejected)\n",
                view==editor::CameraView::perspective?"perspective":"orthographic",clips[0],clips[1]);
        }
    }
    auto* camera_world=take(world::admit_component_edit(runtime,camera_component));
    changed(camera_world->set_enabled(camera_entity,false));
    camera_call("scene.main_camera_set");
    require(take(document.main_camera())==camera_entity && !document.presentation().main_camera(),"Set could not store a disabled camera, or Play used it.");
    changed(camera_world->set_enabled(camera_entity,true));
    require(take(document.presentation().main_camera()).entity==camera_entity,"Re-enabling the selected camera did not restore it.");
    camera_call("scene.main_camera_clear");require(take(document.main_camera())==ObjectId{},"Clear Main Camera kept the selection.");
    std::puts("Operator parameters: PASS (editor document only, schema, strict rejection, seeded patch, one history entry, command arguments)");
}
