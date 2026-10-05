void verify_ui_authoring_window(Application& app,const std::filesystem::path& output,const std::filesystem::path& path) {
    using Access=atelier::application::EditorAcceptanceAccess;
    model(Access::command(app,atelier::editor::Command::create_static_mesh));tick(app,3);
    const auto actor=app.document().selected();
    const auto field=property_control(app,actor,0);
    const auto field_incarnation=app.presented()->find(field)->incarnation;
    const auto path_bar=app.presented()->find(control::parameter_path)->parent;
    const auto path_height=app.presented()->find(path_bar)->bounds.height/app.presented()->drawing->dpi_scale;
    const auto filter_incarnation=app.presented()->find(control::parameter_search)->incarnation;
    const auto verify_hierarchy=[&] {
        const auto* filter=app.presented()->find(control::parameter_search);
        const auto* header=app.presented()->find(51);
        const auto* tabs=app.presented()->find(control::parameter_pages);
        const auto* list=app.presented()->find(control::parameter_list);
        require(filter && header && tabs && list && filter->incarnation==filter_incarnation &&
            filter->bounds.y>=header->bounds.y+header->bounds.height-.1f &&
            filter->bounds.y+filter->bounds.height<=tabs->bounds.y+.1f &&
            tabs->bounds.y+tabs->bounds.height<=list->bounds.y+.1f,
            "Native source update recreated Filter or moved it below the property pages.");
    };
    verify_hierarchy();
    click(app,control::position_fold);
    require(!app.presented()->find(field),"Authoring fixture did not collapse the real Position component.");
    click(app,control::parameter_search);type(app,L"position");
    require(app.text_input().has_value(),"Declarative query did not open a native text session.");
    const auto token=app.text_input()->token;
    const auto source=model(atelier::editor::read_file(path));
    auto changed=source;const auto section=changed.find("Foldout section {");
    const auto offset=section==changed.npos?changed.npos:changed.find("text: \"",section);
    const auto end=offset==changed.npos?changed.npos:changed.find('"',offset+7);
    require(end!=changed.npos,"Authoring fixture could not locate the real foldout text.");
    changed.replace(offset,end-offset+1,"text: \"Live scene root\"");
    const auto* initial_tool=command_tool(app,control::add_static_mesh);
    require(initial_tool,"Authoring fixture has no declarative command tool.");
    const auto tool_key=initial_tool->key;
    const auto tool_incarnation=initial_tool->incarnation;
    const auto shelf=changed.find("ui ShelfTool(");
    const auto width=shelf==changed.npos?changed.npos:changed.find("width: 64;",shelf);
    require(width!=changed.npos,"Authoring fixture could not locate the declarative tool size.");
    changed.replace(width,std::string_view("width: 64;").size(),"width: 68;");
    const auto shell=changed.find("ui PanePath(");
    const auto height=shell==changed.npos?changed.npos:changed.find("height: 40;",shell);
    require(height!=changed.npos,"Authoring fixture could not locate the Parameters path-bar height.");
    changed.replace(height,std::string_view("height: 40;").size(),"height: 44;");
    std::ofstream timings(output/"ui-authoring.csv");timings<<"save,compile_ms,save_to_accepted_ms,revision\n";
    const auto save=[&](const char* label,const std::string& text,bool valid,float expected_height,const auto& verify_edit) {
        require_input_focus(app);
        const auto previous=Access::ui_revision(app),start=GetTickCount64();
        model(atelier::editor::write_file(path,text));
        while (GetTickCount64()-start<10000) {
            tick(app,1);
            if (valid?Access::ui_revision(app)>previous:!Access::ui_error(app).empty()) break;
        }
        require(valid?(Access::ui_revision(app)==previous+1 && Access::ui_error(app).empty()):
            (Access::ui_revision(app)==previous && !Access::ui_error(app).empty()),"Native file save did not publish or retain the correct version.");
        verify_edit();
        verify_hierarchy();
        const auto* path_control=app.presented()->find(control::parameter_path);
        const auto* bar=app.presented()->find(path_bar);
        require(path_control && bar && path_control->parent==path_bar && std::abs(bar->bounds.height/app.presented()->drawing->dpi_scale-expected_height)<.1f,
            "Saved Parameters shell geometry did not update through the retained path aliases.");
        const auto* tool=command_tool(app,control::add_static_mesh);
        require(tool && tool->key==tool_key && tool->incarnation==tool_incarnation &&
            std::abs(tool->bounds.width/app.presented()->drawing->dpi_scale-68)<.1f,
            "Saved tool layout lost its stable action identity or failed to retain the accepted layout.");
        timings<<label<<','<<Access::ui_compile_milliseconds(app)<<','<<GetTickCount64()-start<<','<<Access::ui_revision(app)<<'\n';
    };
    const auto verify_query=[&] {
        require(app.text_input() && app.text_input()->token==token && app.query()==L"position" &&
            !app.presented()->find(control::position_fold)->selected && !app.presented()->find(field) && property_control(app,actor,0)==field,
            "Native source update lost the shared query draft, local foldout state or World field key.");
    };
    save("valid",changed,true,path_height+4,verify_query);capture(app,output/"ui-live-updated.png");
    save("syntax_error",changed+"\n?",false,path_height+4,verify_query);capture(app,output/"ui-live-error.png");
    save("recovered",changed+"\n// recovered\n",true,path_height+4,verify_query);
    key(app,VK_RETURN);click(app,control::position_fold);click(app,field);
    const auto field_history=app.document().undo_count();type(app,L"4.25");
    require(app.text_input() && app.document().editing(),"Authoring fixture did not start a real numeric draft.");
    const auto field_token=app.text_input()->token;
    auto draft_source=changed;draft_source.replace(height,std::string_view("height: 44;").size(),"height: 48;");
    save("field_draft",draft_source,true,path_height+8,[&] {
        require(app.text_input() && app.text_input()->token==field_token && app.document().editing() &&
            property_control(app,actor,0)==field && app.presented()->find(field)->incarnation==field_incarnation &&
            x(app,actor)==4.25f && app.document().undo_count()==field_history,
            "A live Parameters shell resize replaced or committed the active numeric draft.");
    });
    key(app,VK_RETURN);
    require(x(app,actor)==4.25f && !app.document().editing() && app.document().undo_count()==field_history+1,
        "World field editing after live Source replacement did not commit exactly once.");
    const auto count=actors(app).size(),history=app.document().undo_count();
    click(app,control::add_static_mesh);
    require(actors(app).size()==count+1 && app.document().undo_count()==history+1,
        "A hot-updated tool did not dispatch exactly one real command.");
    click(app,control::undo);
    require(actors(app).size()==count && x(app,actor)==4.25f,"Hot-updated tool command did not retain normal undo behavior.");
    capture(app,output/"ui-live-recovered.png");
    require(timings.good(),"Cannot write live authoring evidence.");
}
