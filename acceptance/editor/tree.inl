void verify_tree_view(Application& app,const std::filesystem::path& output,atelier::ObjectId actor) {
    const auto history=app.document().undo_count();
    const auto actor_key=app.object_control(actor);
    const auto row=[&](ControlKey key) {
        const auto& rows=app.presented()->tree_rows;
        const auto at=std::find_if(rows.begin(),rows.end(),[&](const auto& r){return r.key==key;});
        require(at!=rows.end(),"Native tree row has no accepted geometry"); return *at;
    };
    const auto toggle=[&](ControlKey key) {
        const auto box=row(key).disclosure;
        click_at(app,{static_cast<LONG>(box.x+box.width*.5f),static_cast<LONG>(box.y+box.height*.5f)});
    };
    click(app,control::tree_world);
    require(app.document().selected()==atelier::ObjectId{},"Tree World row did not select the world context");
    click(app,actor_key);
    if (!row(actor_key).expanded) toggle(actor_key);
    const auto& rows=app.presented()->tree_rows;
    const auto child=std::find_if(rows.begin(),rows.end(),[&](const auto& r){return r.parent==actor_key;});
    require(child!=rows.end(),"Real Actor components did not enter the tree");
    const auto child_key=child->key;
    click(app,child_key);
    require(app.document().selected()==actor && app.presented()->find(child_key)->selected && !app.presented()->find(actor_key)->selected,
        "Component selection did not retain its owner and a single highlighted tree row");
    capture(app,output/"tree-component-selection.png");
    key(app,VK_LEFT);
    require(app.presented()->find(actor_key)->selected,"Tree Left did not return to Actor selection");
    key(app,VK_LEFT);
    require(!app.presented()->find(child_key),"Tree Left did not collapse actual components");
    click(app,control::tree_filter); type(app,L"Transform"); key(app,VK_RETURN);
    require(app.presented()->find(child_key),"Tree filter did not reveal matching components through the collapsed Actor");
    capture(app,output/"tree-filtered.png");
    click(app,control::tree_filter); type(app,L""); key(app,VK_RETURN);
    require(!app.presented()->find(child_key),"Clearing tree filter changed stored expansion");
    click(app,actor_key); key(app,VK_RIGHT);
    require(app.presented()->find(child_key),"Tree Right did not expand actual components");
    capture(app,output/"tree-view.png");
    key(app,'W',true);
    click(app,control::show_actors);
    require(row(actor_key).expanded && app.presented()->find(child_key),"Reopened tree lost its accepted expansion state");
    click(app,actor_key);
    key(app,VK_LEFT);
    click(app,control::reset_layout);
    click(app,control::parameter_transform);
    require(app.document().selected()==actor && app.document().undo_count()==history,"Tree navigation/filtering polluted scene history");
}
