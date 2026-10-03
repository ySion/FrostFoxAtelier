void verify_tree_view(Application& app,const std::filesystem::path& output,atelier::ObjectId actor) {
    const auto history=app.document().undo_count();
    const auto actor_key=app.object_control(actor);
    const auto row=[&](ControlKey key) {
        const auto& rows=app.presented()->tree_rows;
        const auto at=std::find_if(rows.begin(),rows.end(),[&](const auto& r){return r.key==key;});
        require(at!=rows.end(),"Native tree row has no accepted geometry"); return *at;
    };
    click(app,control::tree_world);
    require(app.document().selected()==atelier::ObjectId{},"Tree World row did not select the world context");
    click(app,actor_key);
    require(std::none_of(app.presented()->tree_rows.begin(),app.presented()->tree_rows.end(),[&](const auto& r){return r.parent==actor_key;}),
        "Scene Tree exposed component rows beneath an Actor");
    key(app,VK_RIGHT);
    require(app.document().selected()==actor,"Actor leaf navigation selected a component");
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
    click(app,control::reset_layout); click(app,control::parameter_transform);
    require(app.document().selected()==actor && app.document().undo_count()==history,"Tree navigation/filtering polluted scene history");
}
