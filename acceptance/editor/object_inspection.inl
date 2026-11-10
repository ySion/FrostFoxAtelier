// Project Source types and Runtime enum metadata drive the real document,
// authored inspector, ownership tree, menu model and scene persistence.
void verify_object_inspection() {
    const auto folder = std::filesystem::current_path() / "out/object-inspection-evidence" /
                        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto project = take(editor::Project::create(folder));
    take(project.create_source("Source/Objects.source", R"(
class inspection.Agent : world.Entity { int health = 100; bool awake = true; }
class inspection.Stats : world.Component { float energy = 5.0f; }
)"));
    const editor::Session::Prepare prepare = [&](world::World& world) -> Result<std::shared_ptr<void>> {
        auto& runtime = world.runtime();
        TypeDefinition mode;
        mode.name = "inspection.Mode";
        mode.kind = TypeKind::enumeration;
        mode.enumeration =
            TypeDefinition::Enumeration{take(runtime.native_type<std::uint32_t>()), {{"Idle", 0}, {"Active", 2}}};
        const auto enumeration = take(runtime.register_type(std::move(mode)));
        TypeDefinition settings;
        settings.name = "inspection.Settings";
        settings.base = world.types().component;
        settings.fields.push_back({"mode", {enumeration}});
        take(runtime.register_type(std::move(settings)));
        auto code = std::make_shared<editor::ProjectCode>(world);
        take(code->build(project));
        return std::static_pointer_cast<void>(code);
    };
    editor::Session document;
    take(document.initialize(prepare));
    auto& runtime = document.world().runtime();
    const auto entity_type = take(runtime.find_type("inspection.Agent"));
    const auto stats_type = take(runtime.find_type("inspection.Stats"));
    const auto settings_type = take(runtime.find_type("inspection.Settings"));
    const auto enum_type = take(runtime.find_type("inspection.Mode"));
    const std::array health{take(runtime.find_field(entity_type, "health"))};
    const std::array energy{take(runtime.find_field(stats_type, "energy"))};
    const std::array mode{take(runtime.find_field(settings_type, "mode"))};
    changed(document.add_entity(entity_type));
    const auto entity = document.selection().entity;
    require(document.selection() == editor::Selection{entity, entity} && !take(world::has_transform(runtime, entity)),
            "A Source Entity acquired an assumed transform or a different inspected identity.");
    operator_parameters::Host host(document);
    auto& workspace = host.panels;
    workspace.project_open = true;
    const std::array fonts{(editor::asset_directory() / "fonts/SarasaUiSC-Regular.ttf").wstring()};
    ui::Session surface;
    take(surface.initialize(fonts));
    auto theme = ui::StyleSheet::parse(take(assets::read_file(editor::asset_directory() / "editor.ss")));
    require(theme && surface.set_style_sheet(*theme) && surface.set_surface({{1600, 1000}, 1, 1}),
            "Object inspection surface failed.");
    std::vector<ui::Control> controls;
    const auto dispatch = [&](const ui::Reply& reply) {
        require(!reply.error, "Object inspection input failed.");
        for (const auto& effect : reply.effects)
            if (const auto* action = std::get_if<ui::Action>(&effect)) {
                if (action->kind == ui::ActionKind::select)
                    take(workspace.select_tree(document, host, action->key));
                else if (action->kind == ui::ActionKind::context_menu)
                    workspace.menu = std::get<ui::ContextMenuRequest>(action->value);
                else if (action->kind == ui::ActionKind::edit_cancel)
                    take(document.cancel_edit());
                else
                    take(workspace.apply_ui(*action));
            }
    };
    const auto publish = [&] {
        controls = take(workspace.describe(document, {1600, 1000}, surface.presented().get(), &project));
        require(workspace.ui_error().empty(), "Object inspection failed to evaluate its authored panels.");
        auto described = surface.set_controls(controls);
        require(described.has_value(), "Object inspection descriptors failed.");
        dispatch(*described);
        workspace.commit_ui_controls();
        auto frame = surface.prepare();
        require(frame && surface.accept(*frame), "Object inspection layout failed.");
    };
    const auto click = [&](ui::ControlKey key) {
        const auto* item = surface.presented()->find(key);
        require(item, "Object inspection has no accepted hit target.");
        const auto box = item->bounds;
        const auto x = box.x + box.width * .65f;
        const auto y = box.y + box.height * .5f;
        dispatch(surface.input({.kind = ui::EventKind::pointer_down, .x = x, .y = y}));
        dispatch(surface.input({.kind = ui::EventKind::pointer_up, .x = x, .y = y}));
    };
    publish();
    const auto health_key = workspace.field_control(entity, health);
    require(health_key && surface.presented()->find(health_key) &&
                !surface.presented()->find(editor::control::position_fold),
            "A rootless Source Entity did not expose its own reflected property set.");
    require(!surface.presented()->find(editor::control::add_component),
            "Parameters retained structural component creation.");
    const auto scene_key = workspace.object_control(entity);
    require(scene_key, "Scene Tree lost its Entity identity.");
    changed(document.add_component(entity, stats_type));
    const auto stats = document.selection().object;
    require(stats != entity && document.selection().entity == entity,
            "Adding a component did not select the new object in its Entity context.");
    publish();
    require(surface.presented()->find(workspace.field_control(stats, energy)) && !surface.presented()->find(health_key),
            "Inspecting a component continued to show the Entity's property set.");
    require(workspace.dock.select(editor::control::structure_tab), "Entity Structure is not a dockable tab.");
    publish();
    const auto structure_key = [&](ObjectId object) {
        for (const auto& control : controls)
            if (const auto* binding = workspace.binding(control.key);
                binding && binding->kind == editor::Workspace::Binding::Kind::structure && binding->object == object)
                return control.key;
        return ui::ControlKey{};
    };
    const auto entity_row = structure_key(entity);
    const auto component_row = structure_key(stats);
    const auto& tree_rows = surface.presented()->tree_rows;
    const auto entity_geometry = std::ranges::find(tree_rows, entity_row, &ui::TreeRowGeometry::key);
    const auto component_geometry = std::ranges::find(tree_rows, component_row, &ui::TreeRowGeometry::key);
    require(entity_row && component_row && entity_row != scene_key && entity_geometry != tree_rows.end() &&
                component_geometry != tree_rows.end() && component_geometry->depth == entity_geometry->depth + 1 &&
                component_geometry->index > entity_geometry->index &&
                surface.presented()->find(editor::control::add_component),
            "Entity Structure did not separate ownership, Scene Tree identity and structural commands.");
    click(entity_row);
    publish();
    require(document.selection() == editor::Selection{entity, entity} && surface.presented()->find(health_key) &&
                surface.presented()->find(workspace.field_control(stats, energy)),
            "Entity inspection did not compose its actual owned properties.");
    click(component_row);
    publish();
    require(document.selection() == editor::Selection{entity, stats},
            "The structure tree selected the Entity instead of its component.");
    const auto field_key = workspace.field_control(stats, energy);
    const auto history = document.undo_count();
    take(document.begin_edit(stats, energy, field_key));
    take(document.preview_edit(field_key, 8.f));
    take(workspace.select_tree(document, host, component_row));
    require(document.owns_edit(field_key) && std::get<float>(take(document.read_property(stats, energy))) == 8 &&
                document.undo_count() == history,
            "Selecting the same object cancelled or committed its active draft.");
    auto detached = take(world::create_component(runtime, stats_type));
    static_cast<void>(document.select(detached.id()));
    require(document.owns_edit(field_key) && document.selection() == editor::Selection{entity, stats},
            "An invalid selection changed the inspected identity or active draft.");
    take(document.select(entity));
    require(!document.editing() && std::get<float>(take(document.read_property(stats, energy))) == 5 &&
                document.undo_count() == history,
            "Changing inspected objects did not cancel the old draft exactly once.");
    changed(document.add_entity(entity_type));
    const auto other = document.selection().entity;
    take(document.select(stats));
    take(document.set_property(stats, energy, Scalar{9.f}, field_key));
    take(document.select(other));
    changed(document.undo());
    require(document.selection() == editor::Selection{entity, stats} &&
                std::get<float>(take(document.read_property(stats, energy))) == 5,
            "Field undo restored only the Entity context rather than the edited object.");
    changed(document.redo());
    publish();
    const auto retired_row = structure_key(stats);
    changed(document.remove_component(stats));
    publish();
    static_cast<void>(document.select(stats));
    require(document.selection() == editor::Selection{entity, entity} && !surface.presented()->find(retired_row),
            "Deleting a selected component retained a stale inspection target or removed its Entity.");
    take(workspace.select_tree(document, host, retired_row));
    require(document.selection() == editor::Selection{entity, entity},
            "A retired structure key selected a stale component.");
    changed(document.undo());
    publish();
    require(document.selection() == editor::Selection{entity, stats} && structure_key(stats) &&
                structure_key(stats) != retired_row &&
                std::get<float>(take(document.read_property(stats, energy))) == 9,
            "Component undo lost identity, value, selection or fresh UI lifetime.");
    changed(document.redo());
    require(document.selection() == editor::Selection{entity, entity},
            "Repeated component removal did not restore owner selection.");
    changed(document.undo());
    changed(document.add_component(entity, settings_type));
    const auto settings = document.selection().object;
    publish();
    const auto mode_key = workspace.field_control(settings, mode);
    const auto choice = std::ranges::find_if(controls, [&](const auto& control) {
        return control.kind == ui::ControlKind::combo && control.context_target == mode_key;
    });
    require(choice != controls.end(), "An enum property was exposed as a free-form numeric field.");
    click(choice->key);
    publish();
    require(workspace.menu_items.size() == 2 && workspace.menu_items[0].behavior == ui::MenuBehavior::radio &&
                workspace.menu_items[0].selected && !workspace.menu_items[1].selected,
            "Enum menu lost its declared choices or selected value.");
    const auto choice_key = workspace.menu_items[1].key;
    const auto* selected = workspace.binding(choice_key);
    require(selected && selected->kind == editor::Workspace::Binding::Kind::operator_call && selected->call &&
                selected->call->request.id == "scene.set_properties" &&
                selected->call->request.args == json::Document::literal<R"({"mode":"Active"})">() &&
                selected->call->request.context.selection == editor::SelectionContext{entity, settings, {entity}},
            "Enum menu lost the exact typed property target.");
    const auto call = selected->call->request;
    publish();
    require(workspace.menu_items[1].key == choice_key, "An unchanged enum call lost its live control identity.");
    const auto before_choice = document.undo_count();
    take(editor::call(workspace.operators, host, workspace.operator_context(document, &project, true), call));
    require(take(document.read_property(settings, mode)) == Scalar{EnumValue{enum_type, 2}} &&
                document.undo_count() == before_choice + 1,
            "The enum menu's operator did not commit the selected value as one history item.");
    changed(document.undo());
    require(take(document.read_property(settings, mode)) == Scalar{EnumValue{enum_type, 0}},
            "Enum operator Undo did not restore its original value.");
    changed(document.redo());
    workspace.menu.reset();
    publish();
    require(!workspace.binding(choice_key), "A closed enum menu kept an executable old binding.");
    const auto saved = take(document.serialize());
    const auto enum_history = document.undo_count();
    static_cast<void>(document.set_property(settings, mode, Scalar{EnumValue{enum_type, 999}}, mode_key));
    require(!document.editing() && document.undo_count() == enum_history &&
                std::get<EnumValue>(take(document.read_property(settings, mode))) == EnumValue{enum_type, 2} &&
                take(document.serialize()) == saved,
            "An invalid enum choice poisoned the document or left a dangling edit.");
    auto reopened = take(editor::Session::deserialize(saved, project, prepare));
    require(take(reopened->serialize()) == saved,
            "Object inspection edits did not preserve Source classes and enum values through a scene roundtrip.");
    {
        // Saved data the types cannot apply stays visible: placeholder titles, a Saved Data group,
        // a marked Scene Tree row and an undoable discard.
        json::Builder altered{take(assets::json_format::parse(saved))};
        bool renamed{};
        bool extended{};
        for (auto item : *altered.root().at("entities")->array())
            for (auto component : *item.at("components")->array()) {
                if (component.string({"type"}) == "inspection.Stats") {
                    take(altered.set(component, "type", "inspection.Gone"));
                    renamed = true;
                }
                if (component.string({"type"}) == "inspection.Settings") {
                    take(altered.set(take(component.at("properties")), "legacy", 3u));
                    extended = true;
                }
            }
        require(renamed && extended, "Saved data inspection did not alter real components.");
        const auto altered_bytes = json::write(altered.root());
        auto kept = take(editor::Session::deserialize({altered_bytes.data(), altered_bytes.size()}, project, prepare));
        auto& kept_runtime = kept->world().runtime();
        const auto kept_settings = take(kept_runtime.find_type("inspection.Settings"));
        ObjectId owner;
        ObjectId extra;
        for (const auto& member : take(kept->world().members()))
            for (const auto& component : take(world::components(kept_runtime, member.id())))
                if (take(kept_runtime.type_of(component.id())) == kept_settings) {
                    owner = member.id();
                    extra = component.id();
                }
        take(kept->select(owner));
        editor::Workspace inspector;
        inspector.project_open = true;
        inspector.scene_available = true;
        const auto has_text = [](const std::vector<ui::Control>& items, std::wstring_view text) {
            return std::ranges::any_of(items, [&](const auto& item) { return item.text == text; });
        };
        require(inspector.dock.select(editor::control::parameters_tab), "Parameters is not a dockable tab.");
        auto items = take(inspector.describe(*kept, {1600, 1000}, nullptr, &project));
        require(inspector.ui_error().empty() && has_text(items, L"Saved Data · Gone (missing type)") &&
                    has_text(items, L"properties.energy") && has_text(items, L"Saved Data · Settings") &&
                    has_text(items, L"properties.legacy") && has_text(items, L"3"),
                "Parameters did not show placeholders and their saved data.");
        require(!inspector.operators.find("scene.discard_saved_data")
                     ->poll(inspector.operator_context(*kept, &project, true)),
                "Discard Saved Data targeted an Entity without saved data.");
        require(inspector.dock.select(editor::control::entities_tab), "Scene Tree is not a dockable tab.");
        items = take(inspector.describe(*kept, {1600, 1000}, nullptr, &project));
        require(std::ranges::any_of(items,
                                    [&](const auto& item) {
                                        return item.kind == ui::ControlKind::tree_item &&
                                               item.text == kept->name(owner) &&
                                               std::ranges::find(item.classes, "retained") != item.classes.end();
                                    }),
                "Scene Tree did not mark an Entity with saved data.");
        take(kept->select(extra));
        require(inspector.operators.find("scene.discard_saved_data")
                    ->poll(inspector.operator_context(*kept, &project, true))
                    .has_value(),
                "Discard Saved Data was unavailable for a component with saved data.");
        const auto before = kept->issues().size();
        take(kept->discard_retained(extra));
        require(!kept->has_retained(extra) && kept->issues().size() == before - 1 &&
                    !inspector.operators.find("scene.discard_saved_data")
                         ->poll(inspector.operator_context(*kept, &project, true)),
                "Discarding saved data left it reported or available.");
        changed(kept->undo());
        require(kept->has_retained(extra) && kept->issues().size() == before,
                "Undo did not restore discarded saved data.");
    }
    changed(document.close());
    changed(document.close());
    static_cast<void>(document.select(settings));
    require(document.selection() == editor::Selection{}, "Closing the document retained a usable component selection.");
    std::puts(
        "Object inspection: PASS (Source Entity fields, flat properties, ownership, selection lifetime, idempotence, enum menus, persistence, saved data and placeholders)");
}
