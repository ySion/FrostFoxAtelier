#include "editor/keymap.hpp"

void verify_rotated_viewport_transforms() {
    using namespace editor;
    Session document;
    take(document.initialize());
    changed(document.create_light(presentation::LightKind::directional));
    const auto entity = document.selection().entity;
    const world::Trs authored{{-5.8103158e-9f, 2.9999993f, -1.532578e-7f},
                              {.761490941f, .141828924f, -.176239014f, .607417405f},
                              {1.f, .999999762f, .999999881f}};
    take(document.set_transform(entity, authored));
    const auto original = take(world::read_transform(document.world().runtime(), entity));
    application::SceneSnapshot snapshot;
    take(snapshot.refresh(document));
    Camera camera;
    auto view = ViewportView::make(camera, {0, 0, 1000, 700}, 1.5f);
    view.scene = snapshot.picking();
    ViewportTool tool;
    ToolSettings settings{TransformTool::transform, PivotMode::median};
    const auto key = [&](std::uint32_t code, ui::Key key = ui::Key::none) {
        return tool.key(document, view, {.kind = ui::EventKind::key_down, .key = key}, code, settings, {500, 350});
    };
    const auto text = [&](std::wstring value) {
        ui::Event event;
        event.kind = ui::EventKind::text;
        event.data = std::move(value);
        return tool.key(document, view, event, 0, settings, {500, 350});
    };
    {
        const auto history = document.undo_count();
        take(key('G'));
        take(key('X'));
        take(text(L"1"));
        const auto preview = take(world::read_transform(document.world().runtime(), entity));
        require(std::abs(preview.position.x - authored.position.x - 1.f) < .0001f && document.undo_count() == history,
                "Rotated Entity world-axis move did not reach its authored position.");
        take(key(27, ui::Key::escape));
        require(!tool.active() && !document.editing() &&
                    take(world::read_transform(document.world().runtime(), entity)) == original &&
                    document.undo_count() == history,
                "Cancelling a rotated Entity transform changed authoring or retained its gesture.");
        take(key('G'));
        take(key('X'));
        take(text(L"1"));
        take(key(13, ui::Key::enter));
        require(!tool.active() && document.undo_count() == history + 1,
                "Rotated Entity transform did not commit once.");
        changed(document.undo());
        require(take(world::read_transform(document.world().runtime(), entity)) == original,
                "Rotated Entity transform undo lost its original TRS.");
    }
    std::puts("Rotated viewport movement: PASS (imported pose, preview, cancel, commit and undo)");
}

void verify_mirrored_viewport_transforms() {
    using namespace editor;
    Session document;
    take(document.initialize());
    changed(document.create_static_mesh(assets::mesh_assets().front()));
    const auto entity = document.selection().entity;
    const auto pose = [&] { return take(world::read_transform(document.world().runtime(), entity)); };
    // One mirrored axis: an unbiased decomposition reports (-1,1,1) plus a half turn instead.
    const world::Trs mirrored{{1, 2, 3}, from_euler_degrees({0, 30, 0}), {1, -1, 1}};
    take(document.set_transform(entity, mirrored));
    application::SceneSnapshot snapshot;
    take(snapshot.refresh(document));
    Camera camera;
    auto view = ViewportView::make(camera, {0, 0, 1000, 700}, 1.5f);
    view.scene = snapshot.picking();
    ViewportTool tool;
    ToolSettings settings{TransformTool::transform, PivotMode::median};
    const auto key = [&](std::uint32_t code, ui::Key key = ui::Key::none) {
        return tool.key(document, view, {.kind = ui::EventKind::key_down, .key = key}, code, settings, {500, 350});
    };
    const auto text = [&](std::wstring value) {
        ui::Event event;
        event.kind = ui::EventKind::text;
        event.data = std::move(value);
        return tool.key(document, view, event, 0, settings, {500, 350});
    };
    const auto nearly = [](math::Vector3 a, math::Vector3 b) { return math::length(a - b) < 1e-4f; };
    const auto same_turn = [](math::Quaternion a, math::Quaternion b) {
        return std::abs(std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) - 1) < 1e-5f;
    };
    const auto gesture = [&](std::uint32_t mode, std::uint32_t axis, const wchar_t* amount) {
        take(key(mode));
        if (axis)
            take(key(axis));
        take(text(amount));
        take(key(13, ui::Key::enter));
    };
    const auto history = document.undo_count();
    gesture('G', 'X', L"2");
    require(nearly(pose().position, {3, 2, 3}) && nearly(pose().scale, {1, -1, 1}) &&
                same_turn(pose().rotation, mirrored.rotation),
            "Moving a mirrored Entity changed its mirror axis or rotation.");
    gesture('R', 'Z', L"90");
    const auto turned = pose();
    require(nearly(turned.scale, {1, -1, 1}) && !same_turn(turned.rotation, mirrored.rotation),
            "Rotating a mirrored Entity moved its mirror to another axis.");
    gesture('S', 0, L"2");
    require(nearly(pose().scale, {2, -2, 2}) && same_turn(pose().rotation, turned.rotation),
            "Scaling a mirrored Entity lost its own mirror axis.");
    gesture('S', 0, L"-0.5");
    require(nearly(pose().scale, {-1, 1, -1}) && same_turn(pose().rotation, turned.rotation),
            "A scale gesture did not cross zero into a mirror.");
    take(key('S'));
    take(text(L"0"));
    require(document.editing() && pose().scale == math::Vector3{}, "A zero scale preview was refused.");
    take(key(27, ui::Key::escape));
    require(nearly(pose().scale, {-1, 1, -1}) && document.undo_count() == history + 4,
            "Cancelling a zero scale preview did not restore the mirror.");
    const auto field = document.types().scale_fields[0];
    take(document.begin_edit(entity, field, 77));
    take(document.preview_edit(77, 0.f));
    take(document.preview_edit(77, -3.f));
    take(document.commit_edit(77));
    require(pose().scale.x == -3 && document.undo_count() == history + 5,
            "Scale field did not commit a negative value.");
    // A zero axis leaves the Entity's frame singular; G, R and S still apply, and the axis stays zero.
    take(document.set_transform(entity, {{1, 2, 3}, {}, {0, -1, 1}}));
    gesture('G', 'Y', L"2");
    require(nearly(pose().position, {1, 4, 3}) && nearly(pose().scale, {0, -1, 1}) && pose().scale.x == 0,
            "Moving a zero scale Entity failed or changed its scale.");
    gesture('R', 'Z', L"30");
    const auto spun = pose();
    require(nearly(spun.position, {1, 4, 3}) && nearly(spun.scale, {0, -1, 1}) && spun.scale.x == 0 &&
                std::abs(spun.rotation.x) < 1e-5f && std::abs(spun.rotation.y) < 1e-5f &&
                std::abs(std::abs(spun.rotation.z) - std::sin(std::numbers::pi_v<float> / 12)) < 1e-5f,
            "Rotating a zero scale Entity failed, or changed its scale or turn axis.");
    gesture('S', 'Y', L"2");
    gesture('S', 'X', L"2");
    require(nearly(pose().scale, {0, -2, 1}) && pose().scale.x == 0 && nearly(pose().position, {1, 4, 3}),
            "Scaling a zero scale Entity failed or left its zero axis.");
    std::puts(
        "Mirrored viewport G/R/S: PASS (single-axis mirror kept, scale crosses zero, zero preview, signed scale field, zero-axis G/R/S)");
}

void verify_viewport_local_constraints() {
    using namespace editor;
    Session document;
    take(document.initialize());
    changed(document.create_static_mesh(assets::mesh_assets().front()));
    const auto entity = document.selection().entity;
    take(document.set_transform(entity, {{}, from_euler_degrees({0, 0, 90}), {1, 1, 1}}));
    const auto before = take(world::read_transform(document.world().runtime(), entity));
    application::SceneSnapshot snapshot;
    take(snapshot.refresh(document));
    Camera camera;
    camera.set_view(CameraView::front);
    auto view = ViewportView::make(camera, {0, 0, 1000, 700}, 1);
    view.scene = snapshot.picking();
    const auto center = *view.project({});
    const auto travel = center + math::Vector2{60, 0};
    ViewportTool tool;
    ToolSettings settings;
    settings.tool = TransformTool::select;
    settings.orientation = Orientation::local;
    const auto key = [&](std::uint32_t code, ui::Key key = ui::Key::none) {
        return tool.key(document, view, {.kind = ui::EventKind::key_down, .key = key}, code, settings, center);
    };
    for (const auto mode : std::array<std::uint32_t, 2>{'G', 'S'}) {
        settings.orientation = mode == 'G' ? Orientation::local : Orientation::global;
        take(key(mode));
        take(tool.pointer(document, view, {.kind = ui::EventKind::pointer_move, .x = travel.x, .y = travel.y},
                          settings));
        take(tool.pointer(
            document, view,
            {.kind = ui::EventKind::pointer_down, .x = travel.x, .y = travel.y, .button = ui::PointerButton::middle},
            settings));
        require(tool.readout().find(L"along local Y") != std::wstring_view::npos,
                "Middle-click constraint did not use the operation's Local projected axes.");
        const auto preview = take(world::read_transform(document.world().runtime(), entity));
        if (mode == 'G')
            require(std::abs(preview.position.x) > .1f && std::abs(preview.position.y) < .0001f,
                    "Local middle-click movement changed direction after selecting its constraint.");
        else
            require(preview.scale.x == 1 && std::abs(preview.scale.y - 1) > .1f && preview.scale.z == 1,
                    "Middle-click Scale constrained the world-axis index instead of the Local axis.");
        take(snapshot.refresh(document));
        view.scene = snapshot.picking();
        std::vector<renderer::OverlayShape> shapes;
        tool.overlay(document, view, settings, shapes);
        require(std::ranges::any_of(shapes,
                                    [](const auto& shape) {
                                        return shape.kind == renderer::OverlayShape::Kind::segment &&
                                               std::abs(shape.points[2] - shape.points[0]) > 700 &&
                                               std::abs(shape.points[3] - shape.points[1]) < .01f &&
                                               shape.color.g > shape.color.r;
                                    }),
                "The modal constraint line did not follow the same Local axis as the transform.");
        take(key(27, ui::Key::escape));
        require(take(world::read_transform(document.world().runtime(), entity)) == before,
                "Cancelling a Local constraint did not restore the authored pose.");
    }
}

void verify_viewport_gesture_input() {
    using namespace editor;
    Session document;
    take(document.initialize({}, SessionRole::editor));
    changed(document.create_static_mesh(assets::mesh_assets().front()));
    const auto first = document.selection().entity;
    changed(document.create_static_mesh(assets::mesh_assets().front(), {2, 0, 0}));
    take(document.select(first));
    application::SceneSnapshot snapshot;
    take(snapshot.refresh(document));
    Camera camera;
    camera.set_view(CameraView::front);
    auto view = ViewportView::make(camera, {0, 0, 1000, 700}, 1);
    view.scene = snapshot.picking();
    const auto center = *view.project({});
    auto pointer = center;
    ViewportTool tool;
    ToolSettings settings;
    settings.tool = TransformTool::select;
    settings.grid_spacing = .25f;
    const auto key = [&](std::uint32_t code, ui::Event event = ui::Event{.kind = ui::EventKind::key_down}) {
        return tool.key(document, view, event, code, settings, pointer);
    };
    const auto text = [&](std::wstring value, bool shift = false) {
        return key(0, {.kind = ui::EventKind::text, .data = std::move(value), .shift = shift});
    };
    const auto escape = [&] { take(key(27, {.kind = ui::EventKind::key_down, .key = ui::Key::escape})); };
    const auto pose = [&] { return take(world::read_transform(document.world().runtime(), first)); };
    const auto history = document.undo_count();
    OperatorRegistry registry;
    register_builtin_operators(registry);
    operator_parameters::Host host(document);
    const auto* binding =
        key_binding(RegionKind::viewport, KeyPhase::before_controls, {.kind = ui::EventKind::key_down}, 'A', false);
    require(binding, "The viewport selection key has no operator binding.");
    for (const auto count : {2u, 0u}) {
        auto context = host.panels.operator_context(document, nullptr, false);
        const auto result = take(call(registry, host, context, binding->call));
        require(result.status == OperatorStatus::finished && document.selected_entities().size() == count &&
                    document.undo_count() == history,
                "The viewport keymap did not toggle selection through call without author history.");
    }
    take(document.select(first));
    take(key('B'));
    for (const auto code : std::array<std::uint32_t, 4>{'G', 'R', 'S', 'A'}) {
        take(key(code));
        require(tool.active() && !tool.modal() && !document.editing() && document.selected_entities().size() == 1,
                "A box-selection owner admitted another gesture or a selection shortcut.");
    }
    escape();
    require(!tool.active(), "Escape did not release the box-selection owner.");
    for (const auto mode : std::array<std::uint32_t, 3>{'G', 'R', 'S'}) {
        take(key(mode));
        take(key('X'));
        take(text(mode == 'R' ? L"15" : L"2"));
        const auto preview = pose();
        const std::wstring readout(tool.readout());
        take(key('X', {.kind = ui::EventKind::key_down, .repeat = true}));
        require(pose() == preview && tool.readout() == readout,
                "An auto-repeated axis key toggled the active transform constraint.");
        take(key('X'));
        require(tool.readout().find(L"along ") == std::wstring_view::npos,
                "A fresh axis key did not release the previous constraint.");
        escape();
    }
    take(key('G'));
    take(key('X'));
    pointer = *view.project({1, 0, 0});
    take(tool.pointer(document, view,
                      {.kind = ui::EventKind::pointer_move, .x = pointer.x, .y = pointer.y, .shift = true}, settings));
    require(std::abs(pose().position.x - .1f) < .0001f, "Shift pointer movement did not enter fine translation.");
    take(key('Q', {.kind = ui::EventKind::key_down, .shift = true}));
    require(std::abs(pose().position.x - .1f) < .0001f, "A key event dropped the held fine-movement modifier.");
    take(text(L"Q", true));
    require(std::abs(pose().position.x - .1f) < .0001f, "A text event dropped the held fine-movement modifier.");
    take(text(L"2", true));
    require(std::abs(pose().position.x - 2) < .0001f, "Fine movement scaled an explicitly typed value.");
    take(key(8, {.kind = ui::EventKind::key_down, .key = ui::Key::backspace, .shift = true}));
    require(std::abs(pose().position.x - .1f) < .0001f,
            "Clearing typed input did not resume fine movement with the current Shift state.");
    take(key(0x10, {.kind = ui::EventKind::key_up}));
    require(std::abs(pose().position.x - 1) < .0001f,
            "Releasing Shift without moving the pointer left a stale fine preview.");
    take(key(0x10, {.kind = ui::EventKind::key_down, .shift = true}));
    require(std::abs(pose().position.x - .1f) < .0001f,
            "Pressing Shift without moving the pointer failed to refresh the preview.");
    escape();
    {
        settings.grid_snap = false;
        pointer = center;
        take(key('G'));
        take(key('X'));
        pointer = *view.project({.31f, 0, 0});
        take(tool.pointer(document, view, {.kind = ui::EventKind::pointer_move, .x = pointer.x, .y = pointer.y},
                          settings));
        require(std::abs(pose().position.x - .31f) < .0001f, "The initial movement preview ignored its snap setting.");
        take(key(0x11, {.kind = ui::EventKind::key_down, .control = true}));
        require(std::abs(pose().position.x - .25f) < .0001f,
                "Pressing Ctrl did not invert snapping at a stationary pointer.");
        take(key(0x11, {.kind = ui::EventKind::key_up}));
        require(std::abs(pose().position.x - .31f) < .0001f, "Releasing Ctrl left the previous snap preview active.");
        escape();
    }
    require(document.undo_count() == history && pose().position == math::Vector3{},
            "Cancelled input gestures changed author history or position.");
    settings.grid_snap = false;
    pointer = center;
    take(key('G'));
    take(key('X'));
    take(text(L"1"));
    tool.arm_box_select({100, 100}, false);
    take(key(13, {.kind = ui::EventKind::key_down, .key = ui::Key::enter}));
    require(!tool.active() && !document.editing() && document.undo_count() == history + 1,
            "Arming a box select during a transform left a second gesture after confirmation.");
    changed(document.undo());
    take(snapshot.refresh(document));
    view.scene = snapshot.picking();
    pointer = {100, 100};
    take(key('B'));
    take(tool.pointer(document, view, {.kind = ui::EventKind::pointer_down, .x = pointer.x, .y = pointer.y}, settings));
    pointer = {900, 600};
    take(tool.pointer(document, view, {.kind = ui::EventKind::pointer_move, .x = pointer.x, .y = pointer.y}, settings));
    take(key('B', {.kind = ui::EventKind::key_down, .repeat = true}));
    take(tool.pointer(document, view, {.kind = ui::EventKind::pointer_up, .x = pointer.x, .y = pointer.y}, settings));
    require(!tool.active() && document.selected_entities().size() == 2 && document.undo_count() == history,
            "Auto-repeated B rearmed an active box selection or lost its original drag.");
}

void verify_viewport_duplication() {
    editor::Session document;
    take(document.initialize());
    changed(document.create_static_mesh(assets::mesh_assets().front(), {1, 2, 3}));
    const auto first = document.selection().entity;
    changed(document.add_component(first, document.types().component));
    changed(document.add_component(first, document.types().collision_component));
    auto body = physics::BodySettings{};
    body.sensor = true;
    body.radius = 2.f;
    take(document.configure_body(document.selection().object, body));
    changed(document.create_light(presentation::LightKind::point, {4, 5, 6}));
    const auto second = document.selection().entity;
    const std::array selected{first, second};
    take(document.select_entities(selected, second));
    const auto history = document.undo_count();
    const auto before = take(assets::json_format::parse(take(document.serialize())));
    changed(document.duplicate_entities(document.selected_entities()));
    const std::vector copies(document.selected_entities().begin(), document.selected_entities().end());
    require(copies.size() == 2 && copies[0] != first && copies[1] != second && document.undo_count() == history + 1,
            "Duplicate did not create independent identities in one history entry.");
    json::Builder after{take(assets::json_format::parse(take(document.serialize())))};
    for (std::size_t i = 0; i < 2; ++i) {
        const auto clone = take(take(after.root().at("entities")).at(i + 2));
        const auto original = take(take(before.root().at("entities")).at(i));
        take(after.set(clone, "name", take(original.at("name"))));
        require(take(clone.at("id")) != take(original.at("id")), "Duplicate reused its source Entity local ID.");
        take(after.set(clone, "id", take(original.at("id"))));
        const auto components = take(clone.at("components"));
        const auto sources = take(original.at("components"));
        for (std::size_t j = 0; j < components.size(); ++j) {
            const auto component = take(components.at(j));
            const auto source_id = take(take(sources.at(j)).at("id"));
            require(take(component.at("id")) != source_id, "Duplicate reused a source Component local ID.");
            take(after.set(component, "id", source_id));
        }
        require(clone == original, "Duplicate lost component state, links, physics settings or reflected properties.");
    }
    const auto root = [&](ObjectId entity) { return entity; };
    const auto original_mesh = take(presentation::read_static_mesh(
        document.world().runtime(), component(document, first, document.presentation_types().static_mesh_component)));
    require(take(presentation::read_static_mesh(
                     document.world().runtime(),
                     component(document, copies[0], document.presentation_types().static_mesh_component)))
                    .mesh.resolved == original_mesh.mesh.resolved,
            "Duplicating an Entity unnecessarily copied its immutable mesh resource.");
    application::SceneSnapshot snapshot;
    take(snapshot.refresh(document));
    editor::ViewportTool tool;
    editor::Camera camera;
    auto view = editor::ViewportView::make(camera, {0, 0, 1000, 700}, 1.f);
    view.scene = snapshot.picking();
    take(tool.place_duplicates(document, view, {}, {500, 350}));
    const auto event = [&](ui::Event e, std::uint32_t key = 0) {
        take(tool.key(document, view, e, key, {}, {500, 350}));
    };
    event({.kind = ui::EventKind::key_down}, 'X');
    event({.kind = ui::EventKind::text, .data = std::wstring{L"2"}});
    event({.kind = ui::EventKind::key_down, .key = ui::Key::enter});
    require(document.undo_count() == history + 1 &&
                take(world::read_transform(document.world().runtime(), root(copies[0]))).position.x == 3,
            "Duplicate placement was split into another undo step.");
    changed(document.undo());
    require(take(document.world().members()).size() == 2, "Undo did not remove the complete duplicate graph.");
    changed(document.redo());
    require(document.selected_entities().size() == 2 && document.selected_entities()[0] == copies[0] &&
                take(world::read_transform(document.world().runtime(), root(copies[0]))).position.x == 3,
            "Redo did not retain duplicate identity and final placement.");
    changed(document.duplicate_entities(document.selected_entities()));
    const auto stationary = document.selection().entity;
    const auto transform = take(world::read_transform(document.world().runtime(), root(stationary)));
    take(tool.place_duplicates(document, view, {}, {500, 350}));
    event({.kind = ui::EventKind::key_down}, 'X');
    event({.kind = ui::EventKind::text, .data = std::wstring{L"7"}});
    event({.kind = ui::EventKind::key_down, .key = ui::Key::escape});
    require(take(document.world().members()).size() == 6 &&
                take(world::read_transform(document.world().runtime(), root(stationary))) == transform,
            "Cancelling duplicate movement removed copies or kept the preview displacement.");
    take(document.select(root(stationary)));
    changed(document.erase_entities(document.selected_entities()));
    require(take(document.world().members()).size() == 5, "Viewport deletion removed only the inspected Component.");
    changed(document.undo());
    take(document.select({}));
    const auto count = document.undo_count();
    changed(document.duplicate_entities(document.selected_entities()));
    require(document.undo_count() == count, "Empty duplicate changed history.");
    editor::Session restricted;
    take(restricted.initialize());
    changed(restricted.create_static_mesh(assets::mesh_assets().front()));
    const auto mesh_entity = restricted.selection().entity;
    // A World keeps one Environment, so copying it with a mesh fails as one group.
    auto restricted_lighting = take(restricted.presentation().environment());
    restricted_lighting.exposure_ev = 1;
    take(restricted.set_environment(restricted_lighting));
    const auto sky_entity =
        take(world::inspect_component(restricted.world().runtime(), restricted.environment())).owner;
    const std::array mixed{mesh_entity, sky_entity};
    take(restricted.select_entities(mixed, sky_entity));
    const auto checkpoint = take(restricted.serialize());
    const auto checkpoint_history = restricted.undo_count();
    static_cast<void>(restricted.duplicate_entities(restricted.selected_entities()));
    require(restricted.undo_count() == checkpoint_history && take(restricted.serialize()) == checkpoint &&
                restricted.selection().entity == sky_entity,
            "Failed group duplication left a partial Entity, changed selection or changed history.");
}

void verify_viewport_editing() {
    verify_rotated_viewport_transforms();
    verify_mirrored_viewport_transforms();
    verify_viewport_local_constraints();
    verify_viewport_gesture_input();
    verify_viewport_duplication();
    using namespace editor;
    Session document;
    take(document.initialize({}, SessionRole::editor));
    const auto create = [&](math::Vector3 at) {
        changed(document.create_static_mesh(assets::mesh_assets().front(), at));
        return document.selection().entity;
    };
    const auto first = create({-1.5f, 0, 0});
    const auto second = create({1.5f, 0, 0});
    const auto root = [&](ObjectId entity) { return entity; };
    const auto transform = [&](ObjectId entity) {
        return take(world::read_transform(document.world().runtime(), root(entity)));
    };
    const auto first_root = root(first);
    const auto second_root = root(second);
    // CPU acceptance explicitly publishes a frozen scene at presentation boundaries.
    application::SceneSnapshot overlay_scene;
    take(overlay_scene.refresh(document));
    Camera camera;
    auto view = ViewportView::make(camera, {30, 40, 1000, 700}, 1.5f);
    view.scene = overlay_scene.picking();
    ViewportTool tool;
    ToolSettings settings{TransformTool::select, PivotMode::median};
    const auto point = [&](ObjectId entity) { return *view.project(transform(entity).position); };
    const auto pointer = [&](ui::EventKind kind, math::Vector2 p, bool shift = false, bool control = false,
                             ui::PointerButton button = ui::PointerButton::secondary) {
        ui::Event event;
        event.kind = kind;
        event.x = p.x;
        event.y = p.y;
        event.shift = shift;
        event.control = control;
        event.button = button;
        return tool.pointer(document, view, event, settings);
    };
    const auto key = [&](std::uint32_t code, ui::Key key = ui::Key::none) {
        ui::Event event;
        event.kind = ui::EventKind::key_down;
        event.key = key;
        return tool.key(document, view, event, code, settings, point(second));
    };
    const auto text = [&](std::wstring value) {
        ui::Event event;
        event.kind = ui::EventKind::text;
        event.data = std::move(value);
        return tool.key(document, view, event, 0, settings, point(second));
    };
    require(tool.pick(document, view, point(first).x, point(first).y) == first,
            "Viewport ray did not hit the actual first mesh.");
    require(tool.pick(document, view, point(second).x, point(second).y) == second,
            "Viewport ray did not hit the actual second mesh.");
    const auto before_cursor_history = document.undo_count();
    take(pointer(ui::EventKind::pointer_down, point(first), false, false, ui::PointerButton::primary));
    require(document.selection().entity == second && document.undo_count() == before_cursor_history &&
                tool.cursor != math::Vector3{},
            "Left click selected an Entity or failed to place the cursor on actual geometry.");
    take(pointer(ui::EventKind::pointer_down, point(first)));
    take(pointer(ui::EventKind::pointer_up, point(first)));
    take(pointer(ui::EventKind::pointer_down, point(second), true));
    take(pointer(ui::EventKind::pointer_up, point(second), true));
    require(document.selected_entities().size() == 2 && document.selection().entity == second,
            "Shift click did not preserve and extend the Entity selection.");
    const auto history = document.undo_count();
    const auto before_first = transform(first);
    const auto before_second = transform(second);
    take(key('G'));
    take(key('X'));
    take(text(L"2"));
    require(std::abs(transform(first).position.x - before_first.position.x - 2) < .0001f &&
                std::abs(transform(second).position.x - before_second.position.x - 2) < .0001f &&
                document.undo_count() == history,
            "Modal move did not preview the whole selection without history.");
    take(key(13, ui::Key::enter));
    require(document.undo_count() == history + 1 && !tool.active(),
            "One move gesture did not produce one history entry.");
    changed(document.undo());
    require(transform(first) == before_first && transform(second) == before_second &&
                document.selected_entities().size() == 2,
            "Transform undo lost a member, selection or original TRS.");
    changed(document.redo());
    changed(document.undo());
    settings.grid_spacing = 1;
    settings.grid_snap = true;
    take(key('G'));
    take(key('X'));
    const auto target = *view.project(before_second.position + math::Vector3{.31f, 0, 0});
    take(pointer(ui::EventKind::pointer_move, target));
    require(std::abs(transform(second).position.x - 2) < .0001f && std::abs(transform(first).position.x + 1) < .0001f,
            "Grid movement did not align the active origin while preserving group offsets.");
    take(pointer(ui::EventKind::pointer_move, target, false, true));
    require(std::abs(transform(second).position.x - 1.81f) < .001f, "Ctrl did not temporarily invert grid snapping.");
    take(key(27, ui::Key::escape));
    take(tool.snap_selection(document, document.selected_entities(), settings));
    require(transform(first).position.x == -2 && transform(second).position.x == 2,
            "Selection to Grid did not align each root.");
    changed(document.undo());
    {
        // Snapping panel: rotation increment and Affect gate R/S like G (Ctrl still inverts).
        settings.rotation_step = 45;
        settings.snap_rotate = true;
        take(key('R'));
        take(key('Z'));
        const auto centre = *view.project(transform(second).position);
        take(pointer(ui::EventKind::pointer_move, {centre.x + 80, centre.y - 35}));
        const auto snapped = euler_degrees(transform(second).rotation).z;
        require(std::abs(std::remainder(snapped, 45.f)) < .01f, "Rotation did not snap to the panel's 45° increment.");
        take(key(27, ui::Key::escape));
        settings.snap_move = false;
        take(key('G'));
        take(key('X'));
        take(pointer(ui::EventKind::pointer_move, *view.project(before_second.position + math::Vector3{.31f, 0, 0})));
        require(std::abs(transform(second).position.x - 1.81f) < .001f,
                "Disabling Affect Move still snapped movement.");
        take(key(27, ui::Key::escape));
        settings.snap_move = true;
        settings.rotation_step = 15;
        // Local orientation: X constrains along the active Entity's own X axis.
        const auto snapping = settings.grid_snap;
        settings.grid_snap = false;
        settings.orientation = Orientation::local;
        auto turned = transform(second);
        turned.rotation = from_euler_degrees({0, 90, 0});
        take(document.set_transform(second, turned));
        const auto start = transform(second).position;
        take(key('G'));
        take(key('X'));
        take(text(L"1"));
        take(key(13, ui::Key::enter));
        const auto moved = transform(second).position - start;
        require(math::length(moved - math::Vector3{0, 0, -1}) < .001f,
                "Local orientation did not constrain X to the Entity's own axis.");
        changed(document.undo());
        changed(document.undo());
        settings.orientation = Orientation::global;
        settings.grid_snap = snapping;
        take(document.select_entities(std::array{first, second}, second));
    }
    settings.grid_snap = false;
    settings.grid_spacing = .2f;
    take(key('G'));
    take(key('X'));
    take(text(L"-"));
    static_cast<void>(key(13, ui::Key::enter));
    require(tool.active(), "Incomplete typed transform ended the active gesture.");
    take(key(27, ui::Key::escape));
    take(key('G'));
    take(key('X'));
    static_cast<void>(text(L"10001"));
    static_cast<void>(key(13, ui::Key::enter));
    require(tool.active() && transform(first) == before_first && transform(second) == before_second,
            "Out-of-range preview was committed or partially applied.");
    take(key(27, ui::Key::escape));
    {
        // Middle click during G picks the axis matching the pointer's travel (Blender).
        const auto origin = *view.project(transform(second).position);
        const auto along_x = *view.project(transform(second).position + math::Vector3{1, 0, 0});
        take(key('G'));
        const auto moved = origin + (along_x - origin) * .5f;
        take(pointer(ui::EventKind::pointer_move, moved, false, false, ui::PointerButton::primary));
        take(pointer(ui::EventKind::pointer_down, moved, false, false, ui::PointerButton::middle));
        const auto picked = transform(second);
        require(std::abs(picked.position.y - before_second.position.y) < .0001f &&
                    std::abs(picked.position.z - before_second.position.z) < .0001f &&
                    std::abs(picked.position.x - before_second.position.x) > .1f,
                "Middle click did not constrain G to the travelled X axis.");
        take(key(27, ui::Key::escape));
    }
    take(key('R'));
    take(key('Y'));
    take(text(L"90"));
    take(key(27, ui::Key::escape));
    require(transform(first) == before_first && transform(second) == before_second && !document.editing(),
            "Rotation cancellation left a preview behind.");
    take(key('S'));
    take(text(L"2"));
    take(key(13, ui::Key::enter));
    require(transform(first).scale == math::Vector3{2, 2, 2} && transform(second).scale == math::Vector3{2, 2, 2},
            "Modal scale failed to update the group.");
    changed(document.undo());
    const std::array roots{first_root, second_root};
    take(document.begin_transform(roots, 918));
    std::array values{before_first, before_second};
    values[0].position.x = 42;
    values[1].position.x = std::numeric_limits<float>::quiet_NaN();
    static_cast<void>(document.preview_transform(918, values));
    require(transform(first) == before_first && transform(second) == before_second,
            "Rejected group preview partially changed earlier objects.");
    take(document.cancel_edit());
    take(document.select(second_root));
    const std::array one_root{second_root};
    take(document.begin_transform(one_root, 919));
    auto turned = before_second;
    turned.rotation = from_euler_degrees({45, 120, 0});
    const std::array turn{turned};
    take(document.preview_transform(919, turn));
    require(take(document.original_transform(919, second_root)) == before_second,
            "Transform origin changed during preview.");
    take(document.commit_edit(919));
    changed(document.undo());
    require(document.selection().object == second_root && transform(second) == before_second,
            "Euler history lost the inspected Component or its transform.");
    take(pointer(ui::EventKind::pointer_down, {view.bounds.x + 1, view.bounds.y + 1}));
    take(pointer(ui::EventKind::pointer_move,
                 {view.bounds.x + view.bounds.width - 1, view.bounds.y + view.bounds.height - 1}));
    take(overlay_scene.refresh(document));
    view.scene = overlay_scene.picking();
    require(overlay_scene.mesh_entities.size() == overlay_scene.meshes.size(),
            "Overlay membership diverged from the render snapshot.");
    std::vector<renderer::OverlayShape> shapes;
    tool.overlay(document, view, settings, shapes);
    require(std::ranges::any_of(
                shapes, [](const auto& shape) { return shape.kind == renderer::OverlayShape::Kind::rectangle; }),
            "Box selection has no visible accepted geometry.");
    take(pointer(ui::EventKind::pointer_up,
                 {view.bounds.x + view.bounds.width - 1, view.bounds.y + view.bounds.height - 1}));
    require(document.selected_entities().size() == 2, "Box selection missed real projected geometry.");
    const auto deletion_history = document.undo_count();
    changed(document.erase_entities(document.selected_entities()));
    require(take(document.world().members()).empty() && document.undo_count() == deletion_history + 1,
            "Multi-delete was not one document operation.");
    take(overlay_scene.refresh(document));
    require(overlay_scene.mesh_entities.empty() && overlay_scene.meshes.empty(),
            "Deleted meshes retained overlay membership.");
    changed(document.undo());
    require(take(document.world().members()).size() == 2 && document.is_selected(first) &&
                document.is_selected(second) && document.selection().entity == second,
            "Multi-delete undo lost Entity identity, active member or membership.");
    take(overlay_scene.refresh(document));
    view.scene = overlay_scene.picking();
    require(std::ranges::find(overlay_scene.mesh_entities, first) != overlay_scene.mesh_entities.end() &&
                std::ranges::find(overlay_scene.mesh_entities, second) != overlay_scene.mesh_entities.end(),
            "Undo did not republish overlay membership.");
    // Runtime writes need no Session revision. Extraction must still reread
    // author values, including after rejecting an invalid transient value.
    const auto runtime_position = take(world::read_transform(document.world().runtime(), first)).position;
    const auto runtime_revision = document.revision();
    const auto frozen_point = point(first);
    const auto frozen_view = view;
    const auto write_position = [&](math::Vector3 position) {
        take(document.world().runtime().with_native<world::Entity>(first, [&](auto& entity) -> Result<void> {
            entity.position = position;
            return {};
        }));
    };
    write_position({runtime_position.x + 7, runtime_position.y, runtime_position.z});
    take(overlay_scene.refresh(document));
    const auto moved = std::ranges::find(overlay_scene.mesh_entities, first) - overlay_scene.mesh_entities.begin();
    require(document.revision() == runtime_revision && overlay_scene.meshes[moved].world[12] == runtime_position.x + 7,
            "Scene extraction cached a direct Runtime transform behind Session revision.");
    require(frozen_view.scene != overlay_scene.picking() &&
                tool.pick(document, frozen_view, frozen_point.x, frozen_point.y) == first,
            "Refreshing a candidate changed picking in an already published viewport.");
    // The World isolates the invalid Entity: the rest of the scene still extracts.
    write_position({std::numeric_limits<float>::infinity(), 0, 0});
    take(overlay_scene.refresh(document));
    require(std::ranges::find(overlay_scene.mesh_entities, first) == overlay_scene.mesh_entities.end() &&
                std::ranges::find(overlay_scene.mesh_entities, second) != overlay_scene.mesh_entities.end(),
            "Scene extraction kept a non-finite Runtime transform or dropped valid Entities.");
    write_position(runtime_position);
    take(overlay_scene.refresh(document));
    require(overlay_scene.meshes[moved].world[12] == runtime_position.x && document.revision() == runtime_revision,
            "Scene extraction retained a failed traversal or changed author history.");
    changed(document.redo());
    require(document.selected_entities().empty(), "Delete redo retained a stale selection.");
    changed(document.undo());
    {
        // Surface placement orients the cursor frame: local Z follows the hit face normal.
        take(overlay_scene.refresh(document));
        view.scene = overlay_scene.picking();
        settings.cursor_surface = true;
        const auto face = view.project(transform(second).position + math::Vector3{0, .5f, 0});
        require(face.has_value(), "Top face is off screen.");
        take(pointer(ui::EventKind::pointer_down, *face, false, false, ui::PointerButton::primary));
        const auto normal = math::rotate(tool.cursor_rotation, math::Vector3{0, 0, 1});
        require(std::abs(tool.cursor.y - .5f) < .01f && normal.y > .999f,
                "Surface cursor did not land on and align with the top face.");
        settings.cursor_surface = false;
    }
    const math::Vector3 angles{23, 47, -12};
    const auto converted = euler_degrees(from_euler_degrees(angles));
    require(std::abs(converted.x - angles.x) < .001f && std::abs(converted.y - angles.y) < .001f &&
                std::abs(converted.z - angles.z) < .001f,
            "Euler parameter conversion disagrees with native XYZ rotation.");
    {
        camera.set_view(CameraView::front);
        auto orthographic = ViewportView::make(camera, view.bounds, view.dpi_scale);
        orthographic.scene = overlay_scene.picking();
        const auto a = orthographic.ray(400, 300);
        const auto b = orthographic.ray(420, 300);
        require(orthographic.orthographic && math::dot(a.direction, b.direction) > .99999f && a.origin != b.origin,
                "Orthographic camera rays are not parallel and position-dependent.");
        require(orthographic.pixel_size({}) == orthographic.pixel_size({10, 20, 30}),
                "Orthographic handle size changed with depth.");
        const auto center = orthographic.project(transform(first).position);
        require(center && tool.pick(document, orthographic, center->x, center->y).has_value(),
                "Orthographic picking lost real geometry.");
    }
    {
        // One sloped polygon crosses far=4 at x=0; reject the distant half,
        // not the entire mesh. Authored sample depths straddle that plane.
        Session clipped;
        take(clipped.initialize());
        auto mesh = take(geometry::Mesh::create(std::vector<geometry::Vertex>{{{-2, -1, 2}, {1, 0, -1}},
                                                                              {{-2, 1, 2}, {1, 0, -1}},
                                                                              {{2, 1, 6}, {1, 0, -1}},
                                                                              {{-2, -1, 2}, {1, 0, -1}},
                                                                              {{2, 1, 6}, {1, 0, -1}},
                                                                              {{2, -1, 6}, {1, 0, -1}}}));
        changed(clipped.create_static_mesh({{}, L"Far crossing", mesh}));
        const auto entity = clipped.selection().entity;
        application::SceneSnapshot clipped_scene;
        take(clipped_scene.refresh(clipped));
        {
            Camera limited;
            auto state = limited.state();
            state.view = CameraView::perspective;
            state.target = {0, 0, 1};
            state.yaw = std::numbers::pi_v<float>;
            state.pitch = state.roll = 0;
            state.distance = 1;
            state.vertical_fov = 60;
            state.near_clip = .25f;
            state.far_clip = 8;
            require(limited.set_state(state), "Far picking camera was rejected.");
            auto full = ViewportView::make(limited, {0, 0, 800, 600}, 1);
            full.scene = clipped_scene.picking();
            state.far_clip = 4;
            require(limited.set_state(state), "Finite far clipping camera was rejected.");
            auto bounded = ViewportView::make(limited, full.bounds, 1);
            bounded.scene = clipped_scene.picking();
            const auto inside = full.project({-.5f, 0, 3.5f});
            const auto outside = full.project({.5f, 0, 4.5f});
            require(inside && outside && tool.pick(clipped, full, inside->x, inside->y) == entity &&
                        tool.pick(clipped, full, outside->x, outside->y) == entity,
                    "Picking control missed the crossing polygon.");
            require(bounded.project({-.5f, 0, 3.5f}) && !bounded.project({.5f, 0, 4.5f}) &&
                        tool.pick(clipped, bounded, inside->x, inside->y) == entity &&
                        !tool.pick(clipped, bounded, outside->x, outside->y),
                    "Projection and picking disagree with the finite far plane.");
        }
    }
    settings.tool = TransformTool::transform;
    take(overlay_scene.refresh(document));
    view.scene = overlay_scene.picking();
    operator_parameters::Host host(document);
    auto& workspace = host.panels;
    const std::array fonts{(editor::asset_directory() / "fonts/SarasaUiSC-Regular.ttf").wstring()};
    ui::Session ui;
    take(ui.initialize(fonts));
    const auto theme = ui::StyleSheet::parse(take(assets::read_file(editor::asset_directory() / "editor.ss")));
    if (!theme)
        throw std::runtime_error("Viewport UI theme failed at " + std::to_string(theme.error().line) + ": " +
                                 theme.error().message);
    require(bool(ui.set_style_sheet(*theme)), "Viewport UI theme could not be applied.");
    require(bool(ui.set_surface({{1200, 800}, 1, 1})), "Viewport UI surface failed.");
    const auto controls = take(workspace.describe(document, {1200, 800}, nullptr));
    require(bool(ui.set_controls(controls)), "Viewport/parameter DSL could not be published.");
    const auto frame = ui.prepare();
    if (!frame)
        throw std::runtime_error("Viewport/parameter layout failed at " + std::to_string(frame.error().key) + " code " +
                                 std::to_string(int(frame.error().code)));
    require(bool(ui.accept(*frame)), "Viewport/parameter frame was refused.");
    {
        const auto& paths = document.types().position_fields;
        const auto* a = ui.presented()->find(workspace.field_control(root(document.selection().entity), paths[0]));
        const auto* b = ui.presented()->find(workspace.field_control(root(document.selection().entity), paths[1]));
        const auto* c = ui.presented()->find(workspace.field_control(root(document.selection().entity), paths[2]));
        require(a && b && c && a->bounds.y == b->bounds.y && b->bounds.y == c->bounds.y,
                "A Vector3 was split across parameter rows.");
    }
    const auto* rotation_x = ui.presented()->find(
        workspace.field_control(root(document.selection().entity), document.types().rotation_fields[0]));
    require(rotation_x && !ui.presented()->find(workspace.field_control(root(document.selection().entity),
                                                                        document.types().rotation_fields[3])),
            "Scene rotation exposed quaternion W instead of a three-angle editor.");
    workspace.commit_ui_controls();
    const auto hud = ui.presented()->find(control::viewport_hud)->bounds;
    const auto camera_menu = ui.presented()->find(control::viewport_camera)->bounds;
    require(ui.pointer_target(hud.x + 8, hud.y + 8) == control::viewport &&
                ui.pointer_target(camera_menu.x + camera_menu.width * .5f, camera_menu.y + camera_menu.height * .5f) ==
                    control::viewport_camera,
            "Viewport chrome did not distinguish passive mode text from the interactive camera menu.");
    const auto expanded_height = ui.presented()->find(control::viewport)->bounds.height;
    const auto publish = [&](ui_layout::Size size = {1200, 800}) {
        auto controls = take(workspace.describe(document, size, ui.presented().get(), nullptr, &ui));
        require(bool(ui.set_controls(controls)), "Updated viewport chrome was refused.");
        const auto frame = ui.prepare();
        require(frame && ui.accept(*frame), "Updated viewport chrome did not lay out.");
        workspace.commit_ui_controls();
        return controls;
    };
    publish();
    // Expansion requests address identities, apply atomically and stay out of history.
    {
        const auto history = document.undo_count();
        take(workspace.set_tree_expanded(document, Workspace::EntityTree::scene_tree, std::array{ObjectId{}}, false));
        publish();
        require(!ui.presented()->find(workspace.object_control(first)) &&
                    !ui.presented()->find(workspace.object_control(second)),
                "Collapsing World kept its Entity rows.");
        const auto mesh_component = component(document, first, document.presentation_types().static_mesh_component);
        static_cast<void>(workspace.set_tree_expanded(document, Workspace::EntityTree::scene_tree,
                                                      std::array{ObjectId{}, mesh_component}, true));
        publish();
        require(!ui.presented()->find(workspace.object_control(first)),
                "A refused expansion request changed the tree.");
        take(workspace.set_tree_expanded(document, Workspace::EntityTree::scene_tree, std::array{ObjectId{}}, true));
        publish();
        require(ui.presented()->find(workspace.object_control(first)) && document.undo_count() == history,
                "Tree expansion did not restore its rows or entered scene history.");
    }
    const std::vector<ObjectId> previous_selection(document.selected_entities().begin(),
                                                   document.selected_entities().end());
    const auto previous_active = document.selection().entity;
    const std::array projected_selection{first, second};
    for (unsigned pass = 0; pass < 3; ++pass) {
        take(document.select_entities(pass == 1 ? std::span<const ObjectId>{}
                                                : std::span<const ObjectId>{projected_selection},
                                      pass == 1 ? ObjectId{} : second));
        const auto selected_controls = publish();
        for (const auto object : std::array{first, second}) {
            const auto key = workspace.object_control(object);
            const auto row = std::ranges::find(selected_controls, key, &ui::Control::key);
            require(row != selected_controls.end() && row->selected == (pass != 1),
                    "Tree selection projection reused a stale membership set.");
        }
    }
    take(document.select_entities(previous_selection, previous_active));
    publish();
    workspace.viewport_options_collapsed = true;
    publish();
    require(std::abs(ui.presented()->find(control::viewport)->bounds.height - expanded_height - 16) < .1f,
            "Collapsed viewport options did not return their height to the scene.");
    workspace.viewport_options_collapsed = false;
    workspace.menu = ui::ContextMenuRequest{control::viewport, control::viewport_shading_pie, 500, 350};
    publish();
    for (const auto& item : workspace.menu_items) {
        const auto* box = ui.presented()->find(item.key);
        require(box && box->bounds.x + box->bounds.width <= box->clip.right + 1 &&
                    box->bounds.y + box->bounds.height <= box->clip.bottom + 1,
                "Pie slots were clipped by a scroll gutter or popup bounds.");
    }
    {
        constexpr ui_layout::Size wide{1900, 1000};
        workspace.menu.reset();
        // Inline rename: the field covers the row's text, never its icon, and owns a fixed key.
        {
            publish(wide);
            const auto row_key = workspace.object_control(first);
            workspace.row_rename = Workspace::RowRename{first, std::wstring(document.name(first))};
            publish(wide);
            const auto& frame = *ui.presented();
            const auto* field = frame.find(control::inline_rename);
            const auto* row = frame.find(row_key);
            require(field && row && field->kind == ui::ControlKind::text_edit &&
                        std::abs(field->bounds.y - row->bounds.y) < 1 &&
                        field->bounds.x > row->bounds.x + row->style->indicator_size &&
                        field->bounds.x + field->bounds.width <= row->bounds.x + row->bounds.width + 1,
                    "Inline rename did not sit over the row's name.");
            workspace.row_rename.reset();
            publish(wide);
            require(!ui.presented()->find(control::inline_rename), "Inline rename field outlived its session.");
        }
        // Scene Tree: drop resolution and keep-world reparent as one edit.
        {
            publish(wide);
            const auto& frame = *ui.presented();
            const auto centre = [&](ui::ControlKey key) {
                const auto* box = frame.find(key);
                require(box, "Tree row missing.");
                return ui::DragPoint{box->bounds.x + box->bounds.width * .5f, box->bounds.y + box->bounds.height * .5f};
            };
            const auto first_row = workspace.object_control(first);
            const auto onto_first = workspace.tree_drop(document, frame, second, centre(first_row));
            require(onto_first && onto_first->parent == first && onto_first->row == first_row,
                    "Dropping on a row did not target that Entity.");
            const auto onto_world = workspace.tree_drop(document, frame, second, centre(control::tree_world));
            require(onto_world && onto_world->parent == ObjectId{}, "Dropping on World did not unparent.");
            const auto world_before = take(world::world_matrix(document.world().runtime(), second));
            const auto history = document.undo_count();
            take(document.select_entities(std::array{first}, first));
            publish(wide);
            take(document.reparent(second, first));
            take(document.select_entities(std::array{second}, second));
            require(take(world::parent_entity(document.world().runtime(), second)) == first &&
                        document.undo_count() == history + 1,
                    "Tree reparent was not one history entry.");
            require(take(world::world_matrix(document.world().runtime(), second)) == world_before,
                    "Keep-world reparent moved the Entity.");
            publish(wide);
            const auto& nested = *ui.presented();
            const auto* child = nested.find(workspace.object_control(second));
            require(child != nullptr, "Dropping did not reveal the moved row under its new parent.");
            take(document.undo());
            require(take(world::parent_entity(document.world().runtime(), second)) == ObjectId{},
                    "Undo did not restore the original parent.");
            take(document.select_entities(std::array{first, second}, second));
            publish();
        }
        // Colour conversion has authored mathematical expectations.
        {
            const auto close = [](std::array<double, 3> a, std::array<double, 3> b) {
                return std::abs(a[0] - b[0]) < 1e-6 && std::abs(a[1] - b[1]) < 1e-6 && std::abs(a[2] - b[2]) < 1e-6;
            };
            require(close(rgb_to_hsv({.25, .5, .75}), {210, 2.0 / 3, .75}),
                    "HSV conversion did not preserve hue, saturation or value.");
            require(color_hex({1, .5, 0}) == L"FF8000", "Hex colour formatting was wrong.");
        }
    }
    {
        // View Selected frames the actual geometry bounds.
        Camera framing;
        framing.frame({1, 2, 3}, .5f);
        require(framing.target() == math::Vector3{1, 2, 3} && math::length(framing.eye() - framing.target()) > .5f &&
                    math::length(framing.eye() - framing.target()) < 2.f,
                "Camera framing did not fit a unit-size bound.");
    }
    std::puts(
        "Viewport editing acceptance: PASS (picking, snapping, rays, multi/box selection, G/R/S, atomic edits, history, Euler, vector/rename geometry)");
}
