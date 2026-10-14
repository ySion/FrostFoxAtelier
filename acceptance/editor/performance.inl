void verify_render_performance(Application& app,const std::filesystem::path& output) {
    using namespace atelier;
    using Access=application::EditorAcceptanceAccess;
    using Performance=editor::RenderPerformance;
    namespace key=editor::world_control;
    namespace api=atelier::control;
    const auto click=[&](ui::ControlKey target) {
        const auto at=centre(app,target);
        Access::input(app,{.kind=ui::EventKind::pointer_down,.x=float(at.x),.y=float(at.y),.button=ui::PointerButton::primary});
        Access::input(app,{.kind=ui::EventKind::pointer_up,.x=float(at.x),.y=float(at.y),.button=ui::PointerButton::primary});tick(app,1);
    };
    require(bool(Access::command(app,editor::Command::show_world_settings)),"Performance World page did not open.");
    Access::performance_panel(app);tick(app);click(key::performance_tab);
    const auto table=[&]()->const ui::TableGeometry& {
        const auto& tables=app.presented()->tables;
        const auto found=std::ranges::find(tables,key::performance_table,&ui::TableGeometry::key);
        require(found!=tables.end() && found->data && found->data->rows.size()==Performance::pass_count,
            "Performance did not use the shared Table with stable pass rows.");return *found;
    };
    require(Access::performance(app).snapshot().state==Performance::State::unavailable &&
        table().data->rows.front().cells[1].text==L"—","Non-Lit performance displayed a fabricated zero.");
    const auto width=table().data->columns[0].width;const auto grip=table().columns[0].resize;
    const float x=grip.x+grip.width*.5f,y=grip.y+grip.height*.5f;
    Access::input(app,{.kind=ui::EventKind::pointer_down,.x=x,.y=y,.button=ui::PointerButton::primary});
    Access::input(app,{.kind=ui::EventKind::pointer_move,.x=x+48,.y=y});
    Access::input(app,{.kind=ui::EventKind::pointer_up,.x=x+48,.y=y,.button=ui::PointerButton::primary});tick(app);
    require(table().data->columns[0].width>width+10,"Performance columns did not resize through shared Table input.");
    const auto saved_width=table().data->columns[0].width;
    require(bool(Access::command(app,editor::Command::create_sky_light)),"Performance fixture sky light failed.");
    Access::performance_panel(app);tick(app);click(key::performance_tab);
    auto view=Access::scene_lit_view(app);view.shading=renderer::Shading::scene_lit;view.wireframe=renderer::Wireframe::off;
    view.lit.ddgi=false;view.lit.direct_reference=false;view.lit.direct_candidates=8;
    require(bool(Access::performance_view(app,view)),"Performance Scene Lit setup failed.");
    const auto history=app.document().undo_count();
    const auto wait_samples=[&](std::size_t minimum) {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(180);
        while(Access::performance(app).snapshot().samples<minimum && std::chrono::steady_clock::now()<deadline)
            require(app.step(),"Editor closed while collecting performance samples.");
        require(Access::performance(app).snapshot().samples>=minimum,"Completed performance samples did not arrive.");
    };
    wait_samples(Performance::window_size);
    const auto full=Access::performance(app).snapshot();
    require(full.state==Performance::State::ready && full.samples==Performance::window_size && full.frame_id>=full.first_frame,
        "GPU sample window mixed domains or exceeded its capacity.");
    double sum{};
    const auto passes=Performance::passes();
    for(std::size_t i=0;i<passes.size();++i) {
        require(full.timings[i] && std::isfinite(full.timings[i]->mean) && std::isfinite(full.timings[i]->p95) && full.timings[i]->p95>=0,
            "Performance statistics contain unavailable or invalid timings.");
        if(passes[i].in_total) sum+=full.timings[i]->mean;
    }
    require(std::abs(sum-full.timings[16]->mean)<.00001 && full.timings[16]->mean>0,
        "Performance total double-counted child intervals.");
    require(!full.timings[3]->current_active && !full.timings[8]->current_active && !full.timings[9]->current_active &&
        !full.timings[14]->current_active && !full.timings[15]->current_active,"Disabled DDGI was shown as an active pass.");
    require(table().data->columns[0].width==saved_width,"Timing publication discarded the user's column width.");
    capture(app,output/"world-performance.png");

    const auto instances=api::discover(output/"Control");
    const auto instance=std::ranges::find_if(instances,[](const auto& item){return item.at("pid").template get<DWORD>()==GetCurrentProcessId();});
    require(instance!=instances.end(),"Performance Control endpoint is absent.");
    auto pending=std::async(std::launch::async,[descriptor=*instance] {
        api::Client client(descriptor,std::chrono::seconds(30));
        return client.call({{"op","editor.inspect"},{"args",{{"detail","summary"}}}});
    });
    while(pending.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)
        require(app.step(),"Editor closed during performance inspection.");
    const auto response=pending.get();require(response.at("status")=="ok","Performance summary inspection failed.");
    const auto& report=response.at("result").at("performance");
    require(report.at("state")=="ready" && report.at("samples")==Performance::window_size &&
        report.at("passes").size()==Performance::pass_count && report.at("passes")[3].at("current_ms").is_null() &&
        !response.at("result").contains("actors"),"Summary inspection did not expose bounded performance with skipped/null semantics.");
    std::ofstream(output/"performance.json")<<report.dump(2);
    require(app.document().undo_count()==history,"Performance observations or table layout entered Scene history.");
    require(bool(Access::select_actor(app,{})),"Performance selection check failed.");tick(app);
    require(Access::performance(app).snapshot().first_frame==full.first_frame,"Selection cleared the performance domain.");

    view.lit.ddgi=true;view.lit.probe_update_budget=64;
    for(auto& volume:view.lit.probe_volumes) volume.enabled=false;
    auto& volume=view.lit.probe_volumes[0];volume.enabled=true;volume.minimum={-4,-4,-4};volume.maximum={4,4,4};volume.counts={4,4,4};
    require(bool(Access::performance_view(app,view)),"Performance DDGI setup failed.");
    require(app.step(),"Editor closed while enabling DDGI.");wait_samples(Performance::window_size);
    const auto ddgi=Access::performance(app).snapshot();
    require(ddgi.ddgi && ddgi.first_frame>full.first_frame,"DDGI statistics retained the disabled domain.");
    for(const auto index:{3,8,9,14,15}) require(ddgi.timings[index] && ddgi.timings[index]->current_active &&
        ddgi.timings[index]->active_samples>0,"Enabled DDGI did not expose its completed subpass timings.");
    capture(app,output/"world-performance-ddgi.png");
    view.lit.direct_candidates=9;require(bool(Access::performance_view(app,view)),"Performance configuration change failed.");
    require(app.step(),"Editor closed during performance domain change.");
    const auto reset=Access::performance(app).snapshot();
    require(reset.first_frame>ddgi.first_frame && reset.samples<=1 && (!reset.frame_id || reset.frame_id>=reset.first_frame),
        "Configuration change admitted a delayed sample from the previous domain.");
    wait_samples(3);
    const auto before_resize=Access::performance(app).snapshot();RECT rectangle{};
    require(GetWindowRect(app.window(),&rectangle)!=0,"Performance resize could not read its own window.");
    require(SetWindowPos(app.window(),nullptr,0,0,rectangle.right-rectangle.left-96,rectangle.bottom-rectangle.top,
        SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=0,"Performance resize failed.");
    for(unsigned i=0;i<180 && Access::performance(app).snapshot().extent==before_resize.extent;++i)
        require(app.step(),"Editor closed during performance resize.");
    const auto resized=Access::performance(app).snapshot();
    require(resized.extent!=before_resize.extent && resized.first_frame>before_resize.first_frame && resized.samples<=1 &&
        (!resized.frame_id || resized.frame_id>=resized.first_frame),"Resize retained samples from the previous target.");
    wait_samples(3);capture(app,output/"world-performance-resized.png");
    view.shading=renderer::Shading::matcap;require(bool(Access::performance_view(app,view)),"Performance non-Lit transition failed.");tick(app);
    require(Access::performance(app).snapshot().state==Performance::State::unavailable &&
        Access::performance(app).snapshot().samples==0 && table().data->rows.front().cells[1].text==L"—",
        "Leaving Scene Lit retained previous timings.");
    capture(app,output/"world-performance-unavailable.png");
    Access::mark_saved(app);
    require(bool(app.open_project(output/(L"Performance-"+api::wide(api::unique_id())),true)),"Performance replacement project failed.");tick(app);
    require(Access::performance(app).snapshot().samples==0,"Replacing the document retained previous performance samples.");
    std::puts("Editor performance acceptance: PASS (real asynchronous GPU window, shared Table/column resize, skipped scopes, nonduplicated total, Control summary, target resize/capture, domain reset, document replacement)");
}
