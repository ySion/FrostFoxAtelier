void verify_tree_view(Application& app, const std::filesystem::path& output, atelier::ObjectId entity) {
    using Access = atelier::application::EditorAcceptanceAccess;
    const auto history = app.document().undo_count();
    const auto entity_key = app.object_control(entity);
    const auto row = [&](ControlKey key) {
        const auto& rows = app.presented()->tree_rows;
        const auto at = std::find_if(rows.begin(), rows.end(), [&](const auto& r) { return r.key == key; });
        require(at != rows.end(), "Native tree row has no accepted geometry");
        return *at;
    };
    click(app, control::tree_world);
    require(app.document().selection().entity == atelier::ObjectId{},
            "Tree World row did not select the world context");
    // A parent row is the nearest shallower row of the same tree before the child.
    const auto tree_of = [&](ControlKey key) {
        const auto* box = app.presented()->find(key);
        return box ? box->parent : ControlKey{};
    };
    const auto child_of = [&](ControlKey child, ControlKey parent) {
        const auto c = row(child);
        const auto p = row(parent);
        const auto tree = tree_of(parent);
        return tree && tree_of(child) == tree && c.depth == p.depth + 1 && c.index > p.index &&
               std::none_of(app.presented()->tree_rows.begin(), app.presented()->tree_rows.end(), [&](const auto& r) {
                   return tree_of(r.key) == tree && r.index > p.index && r.index < c.index && r.depth <= p.depth;
               });
    };
    click(app, entity_key);
    require(row(entity_key).disclosure.width == 0, "Scene Tree exposed component rows beneath an Entity");
    key(app, VK_RIGHT);
    require(app.document().selection() == atelier::editor::Selection{entity, entity},
            "Entity leaf navigation selected a component");
    click(app, control::tree_world);
    key(app, VK_LEFT);
    require(!app.presented()->find(entity_key), "World collapse did not hide its Entities");
    key(app, 'W', true);
    click(app, operator_item(app, "layout.show", {{"tab", "scene_tree"}}));
    require(!row(control::tree_world).expanded && !app.presented()->find(entity_key),
            "Reopening Scene Tree lost its expansion state");
    click(app, control::tree_world);
    key(app, VK_RIGHT);
    click(app, entity_key);
    capture(app, output / "tree-view.png");
    require(app.document().undo_count() == history, "Scene Tree navigation/filtering polluted scene history");

    const auto root =
        component_object(app.document(), entity, app.document().presentation_types().static_mesh_component);
    const auto members = model(atelier::world::components(app.document().world().runtime(), entity)).size();
    click(app, operator_item(app, "layout.show", {{"tab", "structure"}}));
    const auto structure_entity = Access::structure_control(app, entity);
    const auto structure_root = Access::structure_control(app, root);
    require(structure_entity && structure_root && child_of(structure_root, structure_entity) &&
                app.presented()->find(control::add_component),
            "Entity Structure did not expose real component membership and its add command.");
    click(app, structure_root);
    require(app.document().selection() == atelier::editor::Selection{entity, root} &&
                !app.presented()->find(control::position_fold),
            "Selecting a component lost its Entity context or inspected root properties.");
    key(app, VK_HOME);
    require(app.document().selection().object == entity, "Structure Home did not inspect the Entity.");
    key(app, VK_LEFT);
    require(!app.presented()->find(structure_root), "Collapsing the structure Entity retained component rows.");
    key(app, VK_RIGHT);
    key(app, VK_DOWN);
    require(app.document().selection() == atelier::editor::Selection{entity, root},
            "Structure keyboard navigation lost component identity.");
    click(app, control::structure_filter);
    type(app, L"StaticMeshComponent");
    key(app, VK_RETURN);
    require(app.presented()->find(structure_root), "Entity Structure could not filter by the actual component type.");
    click(app, control::structure_filter);
    type(app, L"no-such-component");
    key(app, VK_RETURN);
    require(!app.presented()->find(structure_root) &&
                app.document().selection() == atelier::editor::Selection{entity, root},
            "Filtering structure rows changed inspected identity or retained an unmatched component.");
    click(app, control::structure_filter);
    type(app, L"");
    key(app, VK_RETURN);
    key(app, VK_ESCAPE);
    require(app.presented()->find(structure_root), "Clearing structure filtering lost the real component row.");

    click(app, control::add_component);
    const auto choice = Access::menu_item(app, L"world.Component");
    require(choice, "Entity Structure omitted the base Component type.");
    click(app, choice);
    const auto added = app.document().selection().object;
    require(added != entity && added != root && app.document().selection().entity == entity &&
                model(atelier::world::components(app.document().world().runtime(), entity)).size() == members + 1 &&
                !app.presented()->find(control::position_fold),
            "Structure Add did not inspect its new nonspatial component or invented a Transform.");
    const auto added_row = Access::structure_control(app, added);
    require(added_row && child_of(added_row, structure_entity), "Added Component lost its owning Entity row.");
    click(app, added_row);
    key(app, VK_DELETE);
    require(contains(app, entity) && app.document().selection() == atelier::editor::Selection{entity, entity} &&
                model(atelier::world::components(app.document().world().runtime(), entity)).size() == members,
            "Deleting the inspected Component deleted its Entity or retained component membership.");
    key(app, 'Z', true);
    require(app.document().selection() == atelier::editor::Selection{entity, added} &&
                Access::structure_control(app, added),
            "Component deletion Undo did not restore inspected object identity.");
    key(app, 'Y', true);
    require(app.document().selection() == atelier::editor::Selection{entity, entity} &&
                !Access::structure_control(app, added),
            "Component deletion Redo retained the removed row or stale inspection target.");
    key(app, 'Z', true);
    key(app, 'Z', true);
    require(model(atelier::world::components(app.document().world().runtime(), entity)).size() == members &&
                component_object(app.document(), entity, app.document().presentation_types().static_mesh_component) ==
                    root,
            "Undoing structure creation changed the Entity's original root or composition.");
    capture(app, output / "entity-structure.png");
    click(app, operator_item(app, "layout.show", {{"tab", "scene_tree"}}));
    click(app, app.object_control(entity));
    const auto in_scene_tree = [&](ControlKey key) {
        while (const auto* box = app.presented()->find(key)) {
            if (box->key == control::entity_tree || box->key == 21)
                return true;
            if (!box->parent)
                break;
            key = box->parent;
        }
        return false;
    };
    require(!in_scene_tree(control::add_component) && row(entity_key).disclosure.width == 0,
            "Scene Tree acquired component structure or its add command from the independent Entity Structure pane.");
    click(app, operator_item(app, "layout.reset"));
    require(app.document().selection().entity == entity && app.document().undo_count() == history,
            "Tree navigation/filtering polluted scene history");
}
