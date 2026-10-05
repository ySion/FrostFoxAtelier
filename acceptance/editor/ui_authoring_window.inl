void verify_ui_authoring_window(Application& app,const std::filesystem::path& output,const std::filesystem::path& path) {
    using Access=atelier::application::EditorAcceptanceAccess;
    model(Access::command(app,atelier::editor::Command::create_static_mesh));tick(app,3);
    const auto actor=app.document().selected();
    const auto field=property_control(app,actor,0);
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
    std::ofstream timings(output/"ui-authoring.csv");timings<<"save,compile_ms,save_to_accepted_ms,revision\n";
    const auto save=[&](const char* label,const std::string& text,bool valid) {
        require_input_focus(app);
        const auto previous=Access::ui_revision(app),start=GetTickCount64();
        model(atelier::editor::write_file(path,text));
        while (GetTickCount64()-start<10000) {
            tick(app,1);
            if (valid?Access::ui_revision(app)>previous:!Access::ui_error(app).empty()) break;
        }
        require(valid?(Access::ui_revision(app)==previous+1 && Access::ui_error(app).empty()):
            (Access::ui_revision(app)==previous && !Access::ui_error(app).empty()),"Native file save did not publish or retain the correct version.");
        require(app.text_input() && app.text_input()->token==token && app.query()==L"position" &&
            !app.presented()->find(control::position_fold)->selected && !app.presented()->find(field),
            "Native source update lost the shared edit token or local foldout state.");
        timings<<label<<','<<Access::ui_compile_milliseconds(app)<<','<<GetTickCount64()-start<<','<<Access::ui_revision(app)<<'\n';
    };
    save("valid",changed,true);capture(app,output/"ui-live-updated.png");
    save("syntax_error",changed+"\n?",false);capture(app,output/"ui-live-error.png");
    save("recovered",changed+"\n// recovered\n",true);
    key(app,VK_RETURN);click(app,control::position_fold);click(app,field);type(app,L"4.25");key(app,VK_RETURN);
    require(x(app,actor)==4.25f,"World field editing stopped working after live Source replacement.");
    capture(app,output/"ui-live-recovered.png");
    require(timings.good(),"Cannot write live authoring evidence.");
}
