void verify_tree_view(Application& app,const std::filesystem::path& output,atelier::ObjectId actor) {
    using Access=atelier::application::EditorAcceptanceAccess;
    const auto history=app.document().undo_count();
    const auto actor_key=app.object_control(actor);
    const auto row=[&](ControlKey key) {
        const auto& rows=app.presented()->tree_rows;
        const auto at=std::find_if(rows.begin(),rows.end(),[&](const auto& r){return r.key==key;});
        require(at!=rows.end(),"Native tree row has no accepted geometry"); return *at;
    };
    click(app,control::tree_world);
    require(app.document().selection().actor==atelier::ObjectId{},"Tree World row did not select the world context");
    click(app,actor_key);
    require(std::none_of(app.presented()->tree_rows.begin(),app.presented()->tree_rows.end(),[&](const auto& r){return r.parent==actor_key;}),
        "Scene Tree exposed component rows beneath an Actor");
    key(app,VK_RIGHT);
    require(app.document().selection()==atelier::editor::Selection{actor,actor},"Actor leaf navigation selected a component");
    click(app,control::tree_filter); type(app,L"StaticMeshActor"); key(app,VK_RETURN);
    require(app.presented()->find(actor_key),"Actual Actor type was not searchable");
    capture(app,output/"tree-filtered.png");
    click(app,control::tree_filter); type(app,L""); key(app,VK_RETURN);
    click(app,control::tree_world); key(app,VK_LEFT);
    require(!app.presented()->find(actor_key),"World collapse did not hide its Actors");
    key(app,'W',true); click(app,control::show_actors);
    require(!row(control::tree_world).expanded && !app.presented()->find(actor_key),"Reopening Scene Tree lost its expansion state");
    click(app,control::tree_world); key(app,VK_RIGHT); click(app,actor_key);
    capture(app,output/"tree-view.png");
    require(app.document().undo_count()==history,"Scene Tree navigation/filtering polluted scene history");

    const auto root=*model(atelier::world::root_component(app.document().world().runtime(),actor));
    const auto members=model(atelier::world::components(app.document().world().runtime(),actor)).size();
    click(app,static_cast<ControlKey>(atelier::editor::Command::show_structure));
    const auto structure_actor=Access::structure_control(app,actor),structure_root=Access::structure_control(app,root);
    require(structure_actor && structure_root && row(structure_root).parent==structure_actor && app.presented()->find(control::add_component),
        "Actor Structure did not expose real component membership and its add command.");
    click(app,structure_root);
    require(app.document().selection()==atelier::editor::Selection{actor,root} && app.presented()->find(property_control(app,actor,0)),
        "Selecting a component lost its Actor context or inspected root properties.");
    key(app,VK_HOME);require(app.document().selection().object==actor,"Structure Home did not inspect the Actor.");
    key(app,VK_LEFT);require(!app.presented()->find(structure_root),"Collapsing the structure Actor retained component rows.");
    key(app,VK_RIGHT);key(app,VK_DOWN);
    require(app.document().selection()==atelier::editor::Selection{actor,root},"Structure keyboard navigation lost component identity.");
    click(app,control::structure_filter);type(app,L"StaticMeshComponent");key(app,VK_RETURN);
    require(app.presented()->find(structure_root),"Actor Structure could not filter by the actual component type.");
    click(app,control::structure_filter);type(app,L"no-such-component");key(app,VK_RETURN);
    require(!app.presented()->find(structure_root) && app.document().selection()==atelier::editor::Selection{actor,root},
        "Filtering structure rows changed inspected identity or retained an unmatched component.");
    click(app,control::structure_filter);type(app,L"");key(app,VK_RETURN);key(app,VK_ESCAPE);
    require(app.presented()->find(structure_root),"Clearing structure filtering lost the real component row.");

    click(app,control::add_component);
    const auto choice=Access::menu_item(app,L"world.Component");require(choice,"Actor Structure omitted the base Component type.");
    click(app,choice);
    const auto added=app.document().selection().object;
    require(added!=actor && added!=root && app.document().selection().actor==actor &&
        model(atelier::world::components(app.document().world().runtime(),actor)).size()==members+1 &&
        !app.presented()->find(control::position_fold),
        "Structure Add did not inspect its new nonspatial component or invented a Transform.");
    const auto added_row=Access::structure_control(app,added);
    require(added_row && row(added_row).parent==structure_actor,"Added Component lost its owning Actor row.");
    click(app,added_row);key(app,VK_DELETE);
    require(contains(app,actor) && app.document().selection()==atelier::editor::Selection{actor,actor} &&
        model(atelier::world::components(app.document().world().runtime(),actor)).size()==members,
        "Deleting the inspected Component deleted its Actor or retained component membership.");
    key(app,'Z',true);
    require(app.document().selection()==atelier::editor::Selection{actor,added} && Access::structure_control(app,added),
        "Component deletion Undo did not restore inspected object identity.");
    key(app,'Y',true);
    require(app.document().selection()==atelier::editor::Selection{actor,actor} && !Access::structure_control(app,added),
        "Component deletion Redo retained the removed row or stale inspection target.");
    key(app,'Z',true);key(app,'Z',true);
    require(model(atelier::world::components(app.document().world().runtime(),actor)).size()==members &&
        model(atelier::world::root_component(app.document().world().runtime(),actor))==root,
        "Undoing structure creation changed the Actor's original root or composition.");
    capture(app,output/"actor-structure.png");
    click(app,control::show_actors);click(app,app.object_control(actor));
    const auto in_scene_tree=[&](ControlKey key) {
        while(const auto* box=app.presented()->find(key)) {
            if(box->key==control::actor_tree || box->key==21) return true;
            if(!box->parent) break;
            key=box->parent;
        }
        return false;
    };
    require(!in_scene_tree(control::add_component) &&
        std::none_of(app.presented()->tree_rows.begin(),app.presented()->tree_rows.end(),[&](const auto& r){return r.parent==actor_key;}),
        "Scene Tree acquired component structure or its add command from the independent Actor Structure pane.");
    click(app,control::reset_layout);
    require(app.document().selection().actor==actor && app.document().undo_count()==history,"Tree navigation/filtering polluted scene history");
}
