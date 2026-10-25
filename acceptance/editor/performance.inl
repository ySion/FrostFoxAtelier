void verify_render_performance(Application& app,const std::filesystem::path& output) {
    using namespace atelier;
    using Access=application::EditorAcceptanceAccess;
    using Performance=editor::FramePerformance;
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
        require(found!=tables.end() && found->data && !found->data->rows.empty() && found->data->rows.front().key==Performance::cpu_frame_row &&
            std::ranges::count_if(found->data->rows,[](const auto& row){return row.key>=1 && row.key<=Performance::pass_count;})==Performance::pass_count,
            "Performance did not use the shared Table with a CPU section and stable GPU pass rows.");return *found;
    };
    const auto gpu_total=[&]()->const ui::TableRow& {
        return *std::ranges::find(table().data->rows,ui::TableKey{Performance::total_pass+1},&ui::TableRow::key);
    };
    require(Access::performance(app).snapshot().state==Performance::State::unavailable &&
        gpu_total().cells[1].text==L"—","Non-Lit performance displayed a fabricated zero.");
    const auto width=table().data->columns[0].width;const auto grip=table().columns[0].resize;
    const float x=grip.x+grip.width*.5f,y=grip.y+grip.height*.5f;
    Access::input(app,{.kind=ui::EventKind::pointer_down,.x=x,.y=y,.button=ui::PointerButton::primary});
    Access::input(app,{.kind=ui::EventKind::pointer_move,.x=x+48,.y=y});
    Access::input(app,{.kind=ui::EventKind::pointer_up,.x=x+48,.y=y,.button=ui::PointerButton::primary});tick(app);
    require(table().data->columns[0].width>width+10,"Performance columns did not resize through shared Table input.");
    const auto saved_width=table().data->columns[0].width;
    require(bool(Access::command(app,editor::Command::create_point_light)),"Performance fixture local light failed.");
    Access::performance_panel(app);tick(app);click(key::performance_tab);
    auto view=Access::scene_lit_view(app);view.shading=renderer::Shading::scene_lit;view.wireframe=renderer::Wireframe::off;
    view.lit.ddgi=false;view.lit.direct_candidates=8;
    view.lit.sky_enabled=true;
    require(bool(Access::performance_view(app,view)) && app.document().environment()!=atelier::ObjectId{},"Performance Scene Lit setup failed.");tick(app,1);
    const auto history=app.document().undo_count();
    const auto wait_samples=[&](std::size_t minimum,std::uint64_t after=0) {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(180);
        while((Access::performance(app).snapshot().samples<minimum || Access::performance(app).snapshot().first_frame<=after) &&
            std::chrono::steady_clock::now()<deadline)
            require(app.step(),"Editor closed while collecting performance samples.");
        require(Access::performance(app).snapshot().samples>=minimum && Access::performance(app).snapshot().first_frame>after,
            "Completed performance samples did not arrive for the requested domain.");
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
    require(std::abs(sum-full.timings[Performance::total_pass]->mean)<.00001 && full.timings[Performance::total_pass]->mean>0,
        "Performance total double-counted child intervals.");
    double direct_sum=0;
    for(std::size_t i=12;i<15;++i) {
        require(passes[i].parent==2 && !passes[i].in_total,"A direct subpass lost its Direct parent or entered the total.");
        direct_sum+=full.timings[i]->mean;
    }
    require(passes[12].key=="direct_cluster" && passes[13].key=="shadow_cache" && passes[14].key=="direct_shading" &&
        full.timings[12]->current_active && full.timings[14]->current_active && direct_sum<=full.timings[2]->mean+.01,
        "Light lists, shadow updates and shading do not belong to their Direct parent interval.");
    require(!full.timings[3]->current_active && !full.timings[8]->current_active && !full.timings[9]->current_active &&
        !full.timings[10]->current_active && !full.timings[11]->current_active,"Disabled DDGI was shown as an active pass.");
    require(table().data->columns[0].width==saved_width,"Timing publication discarded the user's column width.");
    // CPU rows: every editor step enters these host and renderer phases once; the editor never runs a World.
    const auto& cpu=full.cpu;
    using namespace application;
    require(cpu.samples==Performance::window_size && cpu.frame_id>=full.first_frame,"CPU sample window mixed domains or exceeded its capacity.");
    for(const auto* marker:{&timing::frame,&phases::input,&phases::project,&phases::control,&phases::gc,&phases::ui,&phases::layout,
            &phases::scene,&phases::extract,&renderer::phases::render,&renderer::phases::prepare,&renderer::phases::encode,&renderer::phases::frame})
        require(cpu.phases[marker->index()] && cpu.phases[marker->index()]->active_samples==Performance::window_size &&
            cpu.current_entries[marker->index()]==1,("CPU phase "+std::string(marker->key())+" was not entered once in every sampled step.").c_str());
    require(!cpu.phases[world::phases::update.index()] && !cpu.phases[physics::phases::step.index()] && cpu.unattributed && cpu.interval,
        "The editor reported World phases or lacks its unattributed row or frame interval.");
    std::vector<ui::TableKey> cpu_keys;
    for(const auto& row:table().data->rows) if(row.key>=Performance::cpu_frame_row) cpu_keys.push_back(row.key);
    const auto at=[&](const timing::Marker& marker){return std::ranges::find(cpu_keys,Performance::cpu_frame_row+marker.index())-cpu_keys.begin();};
    require(cpu_keys.front()==Performance::cpu_frame_row && cpu_keys.back()==Performance::cpu_unattributed_row &&
        at(phases::extract)==at(phases::scene)+1 && at(renderer::phases::prepare)==at(renderer::phases::render)+1 &&
        at(world::phases::update)==std::ssize(cpu_keys),"CPU rows lost their depth-first order or listed a phase that never ran.");
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
    require(response.at("result").at("last_frame").at("gpu_sample").at("direct_shading_ms").is_number(),
        "Control omitted the completed direct shading duration.");
    require(response.at("result").at("last_frame").at("gpu_sample").at("shadow_cache_ms").is_number() &&
        response.at("result").at("last_frame").at("gpu_sample").at("shadow_cache").size()==14,
        "Control omitted the completed shadow cache duration or counters.");
    require(report.at("state")=="ready" && report.at("samples")==Performance::window_size &&
        report.at("passes").size()==Performance::pass_count && report.at("passes")[3].at("current_ms").is_null() &&
        report.at("passes")[3].at("max_ms").is_null() && report.at("passes")[0].at("max_ms").is_number() &&
        !response.at("result").contains("entities"),"Summary inspection did not expose bounded performance with skipped/null semantics.");
    const auto& cpu_report=report.at("cpu");const auto& cpu_phases=cpu_report.at("phases");
    const auto phase=[&](std::string_view id)->const api::Json& {
        const auto found=std::ranges::find_if(cpu_phases,[&](const auto& item){return item.at("id").template get_ref<const std::string&>()==id;});
        require(found!=cpu_phases.end(),("Control omitted CPU phase "+std::string(id)+".").c_str());return *found;
    };
    require(cpu_report.at("state")=="ready" && cpu_report.at("samples")==Performance::window_size &&
        cpu_report.at("structure_errors")==0 && cpu_report.at("first_structure_error").is_null() &&
        cpu_report.at("frame_interval_ms").at("p95").is_number() && cpu_report.at("frames_per_second").is_number() &&
        cpu_phases.size()==timing::catalog().size()+1 &&
        phase("frame").at("parent").is_null() && phase("frame").at("depth")==0 && phase("frame").at("current_entries")==1 &&
        phase("app.scene.extract").at("parent")=="app.scene" && phase("app.scene.extract").at("depth")==2 &&
        phase("app.scene.extract").at("state")=="measured" && phase("app.scene.extract").at("max_ms").is_number() &&
        phase("physics.step").at("parent")=="world.simulation" && phase("physics.step").at("state")=="unavailable" &&
        phase("physics.step").at("mean_ms").is_null() && phase("frame.unattributed").at("parent")=="frame",
        "Control CPU performance lost its catalog, hierarchy, entries or null semantics.");
    std::ofstream(output/"performance.json")<<report.dump(2);
    require(app.document().undo_count()==history,"Performance observations or table layout entered Scene history.");
    require(bool(Access::select_entity(app,{})),"Performance selection check failed.");tick(app);
    require(Access::performance(app).snapshot().first_frame==full.first_frame,"Selection cleared the performance domain.");

    const auto control_call=[&](api::Json request) {
        auto call=std::async(std::launch::async,[descriptor=*instance,request=std::move(request)]() mutable {
            api::Client client(descriptor,std::chrono::seconds(30));
            const auto state=client.call({{"op","editor.inspect"},{"args",{{"detail","summary"}}}});
            request["state_token"]=state.at("state_token");return client.call(std::move(request));
        });
        while(call.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)
            require(app.step(),"Editor closed during direct-light Control operation.");
        auto result=call.get();require(result.at("status")=="ok","Direct-light Control operation failed.");return result;
    };
    control_call({{"op","view.set"},{"args",{{"patch",{{"direct_candidates",8}}}}}});
    require(Access::performance(app).snapshot().first_frame==full.first_frame,"An unchanged Control patch reset the performance domain.");
    const auto cluster_report=control_call({{"op","editor.inspect"},{"args",{{"detail","summary"}}}});
    const auto& gpu=cluster_report.at("result").at("last_frame").at("gpu_sample");
    constexpr std::uint32_t direct_scopes=1u<<12|1u<<14;
    require(!cluster_report.at("result").at("view").contains("direct_clustered") &&
        !cluster_report.at("result").at("performance").at("configuration").contains("direct_shadow_cache") && gpu.at("valid")==true &&
        gpu.at("frame_id").get<std::uint64_t>()>=full.first_frame && gpu.at("sequence").get<std::uint64_t>()>=full.sequence &&
        (gpu.at("pass_mask").get<std::uint32_t>()&direct_scopes)==direct_scopes && gpu.at("direct_cluster_ms").get<double>()>=0 &&
        gpu.at("direct_cluster").at("overflows").get<std::uint32_t>()<=gpu.at("direct_cluster").at("cells").get<std::uint32_t>() &&
        gpu.at("direct_cluster").at("max_sources").is_number_unsigned(),"Control cluster timing/counts lost completed-sample identity or semantics.");
    std::ofstream(output/"performance-clustered.json")<<cluster_report.dump(2);
    require(app.document().undo_count()==history,"Control view patches entered the Undo stack.");

    view.lit.ddgi=true;view.lit.probe_update_budget=64;
    for(auto& volume:view.lit.probe_volumes) volume.enabled=false;
    auto& volume=view.lit.probe_volumes[0];volume.enabled=true;volume.minimum={-4,-4,-4};volume.maximum={4,4,4};volume.counts={4,4,4};
    require(bool(Access::performance_view(app,view)),"Performance DDGI setup failed.");
    require(app.step(),"Editor closed while enabling DDGI.");wait_samples(Performance::window_size);
    const auto ddgi=Access::performance(app).snapshot();
    require(ddgi.ddgi && ddgi.first_frame>full.first_frame,"DDGI statistics retained the disabled domain.");
    for(const auto index:{3,8,9,10,11}) require(ddgi.timings[index] && ddgi.timings[index]->current_active &&
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
        Access::performance(app).snapshot().samples==0 && gpu_total().cells[1].text==L"—",
        "Leaving Scene Lit retained previous timings.");
    capture(app,output/"world-performance-unavailable.png");
    Access::mark_saved(app);
    require(bool(app.open_project(output/(L"Performance-"+api::wide(api::unique_id())),true)),"Performance replacement project failed.");tick(app);
    require(Access::performance(app).snapshot().samples==0,"Replacing the document retained previous performance samples.");
    require(Access::performance(app).structure_errors()==0,"A step recorded a frame timing phase under the wrong parent.");
    std::puts("Editor performance acceptance: PASS (real asynchronous GPU window, shared Table/column resize, skipped scopes, nonduplicated total, Control summary, target resize/capture, domain reset, document replacement)");
}

// This consumer advances the product Application. It never constructs a second
// Renderer or Frame, and calibrates against the renderer's shared pixel range.
// Measurement entry, not acceptance: no timing or rate is a pass condition. The checks below only reject invalid samples.
int profile_editor_render_performance(Application& app,const char* output_directory) {
    using namespace atelier;
    using Access=application::EditorAcceptanceAccess;
    using Json=atelier::control::Json;
    using Clock=std::chrono::steady_clock;
    const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);hr(com);
    struct ComScope {~ComScope(){CoUninitialize();}} com_scope;
    const auto output=std::filesystem::path(editor::wide(output_directory));
    require(output.is_absolute(),"Render performance evidence needs an absolute directory.");
    std::filesystem::create_directories(output);
    const auto project=Access::project_root(app),scene=Access::scene_path(app);
    require(!project.empty() && !scene.empty(),"Render performance requires a loaded project Scene.");
    const auto scene_file=project/scene;
    const auto file_hash=atelier::control::sha256_file(scene_file);
    // Lock authoring state before either a product frame or a diagnostic write.
    const auto original_scene=model(app.document().serialize());
    const auto original_world=model(app.document().presentation().environment());
    const auto loaded_environment=[&] {
        for(const auto& entity:Json::parse(original_scene).at("entities"))
            for(const auto& part:entity.at("components")) if(part.at("type")=="world.EnvironmentComponent") return part.value("properties",Json::object());
        return Json(nullptr);
    }();
    const auto* original_document=&app.document();
    const auto history=app.document().undo_count(),redo=app.document().redo_count(),revision=app.document().revision();
    const auto dirty=app.document().dirty();
    const auto unchanged_author=[&] {
        require(&app.document()==original_document && Access::project_root(app)==project && Access::scene_path(app)==scene &&
            app.document().undo_count()==history && app.document().redo_count()==redo && app.document().revision()==revision &&
            app.document().dirty()==dirty && model(app.document().presentation().environment())==original_world,
            "Performance preparation or sampling changed the loaded Scene or its authoring history.");
    };
    const auto unchanged_scene=[&] {
        unchanged_author();
        require(model(app.document().serialize())==original_scene,"Performance comparison changed Scene contents.");
        require(atelier::control::sha256_file(scene_file)==file_hash,"Performance comparison changed the source Scene file.");
    };
    const auto apply_view=[&](const renderer::ViewSettings& candidate) {
        unchanged_author();
        require(static_cast<const rendering::SceneLightingSettings&>(candidate.lit)==original_world,
            "Performance diagnostics must preserve the loaded World author settings.");
        model(Access::performance_view(app,candidate));
        unchanged_author();
    };
    Json report{{"schema","atelier.editor.render-performance.v6"},{"status","preparing"},
        {"revision",build_info::revision},{"configuration",build_info::configuration},
        {"project",editor::utf8(project.wstring())},{"scene",editor::utf8(scene.wstring())},{"scene_sha256",file_hash},
        {"authoring_baseline",{{"loaded_environment",loaded_environment},{"undo",history},{"redo",redo},{"revision",revision},{"dirty",dirty}}},
        {"gpu_validation",Access::gpu_validation(app)},{"vsync",Access::vsync(app)},
        {"wall_semantics","Intervals between the frame timing ends of consecutive accepted Application steps; include skipped calls, waits and harness work between steps. GPU durations are not FPS."},
        {"cpu_semantics","CPU wall time in milliseconds of current accepted Application steps, read from the product's frame timing record of each step. frame is the whole step; app.* are host phases and app.scene contains app.scene.extract; render contains prepare, encode and frame; encode contains scene (materials, upload, draws, lit) and ui (upload); frame contains submit, present, wait and recycle. A phase sums every entry in the step and entries counts them; frame.unattributed is the frame minus its direct children. In studio modes only, render.encode.scene.draws divided by scene_draw_calls is the mean cost of one recorded scene draw; Scene Lit surface draws count in scene_draw_calls but are timed under render.encode.scene.lit. Do not sum parent and child stages. timing_sequence is the step's frame timing ordinal, separate from completed GPU sequence/frame_id."},
        {"resource_semantics","Current CPU logical Scene Lit bytes, tagged with observation frame; not completed-frame physical VRAM."},
        {"warmup_accepted_frames",192},{"completed_gpu_samples_per_scene_lit_case",240},
        {"non_lit_accepted_steps_per_case",240},{"cases",Json::array()}};
    const auto write_report=[&] {
        std::ofstream stream(output/"render-performance.json",std::ios::binary|std::ios::trunc);
        stream<<report.dump(2)<<'\n';stream.flush();require(stream.good(),"Cannot write render performance evidence.");
    };
    try {
        require(!Access::vsync(app),"Native render performance must disable main-surface VSync at Application startup.");
        require(!dirty && history==0 && redo==0,"Render performance requires an unchanged freshly loaded Scene.");
        // Each product step records its own frame; accepted intervals and
        // CPU stages are read from those records, never from a harness clock.
        const auto frame_end=[](const timing::FrameRecord& record) {return record.begin+record.time(timing::frame);};
        const auto next_frame=[&](Clock::time_point deadline) {
            const auto before=app.accepted_frames();
            // A fallback for an unprepared Lit view is not a frame of the measured view.
            while(app.accepted_frames()==before || Access::fallback_presented(app)) {
                require(Clock::now()<deadline,"Render performance timed out waiting for an accepted product frame.");
                require(app.step(),"Render performance editor closed before completion.");
                unchanged_author();
                // A popup renders its own surface within the step. The profile never opens one, so it came from external input.
                require(!app.menu_window(),"A menu popup opened during measurement (external input); the window must stay untouched while profiling.");
            }
            require(app.presented() && Access::accepted_view(app),"Accepted Scene View is absent.");
            const auto& record=app.frame_record();
            if(record.structure_errors!=0 || record.count(renderer::phases::frame)!=1)
                throw std::runtime_error("Accepted step did not record exactly one renderer frame phase: structure_errors="+
                    std::to_string(record.structure_errors)+" first_error="+(record.first_error?std::string(record.first_error->key()):"none")+
                    " render="+std::to_string(record.count(renderer::phases::render))+" prepare="+std::to_string(record.count(renderer::phases::prepare))+
                    " encode="+std::to_string(record.count(renderer::phases::encode))+" frame="+std::to_string(record.count(renderer::phases::frame))+
                    " accepted_frame="+std::to_string(app.presented()->drawing->id)+" preparation=\""+editor::utf8(Access::render_notice(app))+"\"");
            return frame_end(record);
        };
        // Let the product publish its loaded World into the accepted UI/view.
        // No stale workspace defaults may flow back into Session authoring.
        next_frame(Clock::now()+std::chrono::minutes(5));
        unchanged_scene();
        const auto original_view=Access::scene_lit_view(app);
        require(static_cast<const rendering::SceneLightingSettings&>(original_view.lit)==original_world,
            "Accepted view does not contain the loaded World author settings.");
        struct RestoreView {
            Application& app;renderer::ViewSettings view;const editor::Session* document;bool active{true};
            ~RestoreView() {
                // A failure must never restore old author values over a changed document.
                if(active && &app.document()==document && !app.document().world().closed() &&
                    static_cast<const rendering::SceneLightingSettings&>(view.lit)==model(app.document().presentation().environment()))
                    static_cast<void>(Access::performance_view(app,view));
            }
        } restore{app,original_view,original_document};
        auto view=original_view;view.shading=renderer::Shading::scene_lit;view.wireframe=renderer::Wireframe::off;
        view.lit.direct_cluster_inspect=false;
        apply_view(view);
        const auto calibrate=[&](bool docked) {
            // Acceptance may need a client larger than the desktop to retain
            // the Dock ratios. Bypass the default tracking-size clamp, while
            // preserving WM_WINDOWPOSCHANGED/WM_SIZE and product resize work.
            constexpr UINT resize_flags=SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOSENDCHANGING;
            auto& calibration=report["calibration"][docked?"docked":"maximized"];
            calibration={{"status","preparing"},{"target_pixel_extent",{2560,1440}},
                {"resize_flags",resize_flags},
                {"system_max_track_extent",{GetSystemMetrics(SM_CXMAXTRACK),GetSystemMetrics(SM_CYMAXTRACK)}},
                {"attempts",Json::array()}};
            const auto deadline=Clock::now()+std::chrono::minutes(5);
            std::vector<std::array<LONG,4>> visited;
            try {
                for(unsigned attempt=0;attempt<=(docked?32u:8u);++attempt) {
                    next_frame(deadline);next_frame(deadline);
                    const auto bounds=Access::accepted_view(app)->bounds;
                    const auto pixels=renderer::detail::raster_viewport(bounds).scissor;
                    const int width=pixels.right-pixels.left,height=pixels.bottom-pixels.top;
                    RECT outer{},client{};
                    require(GetWindowRect(app.window(),&outer)!=0 && GetClientRect(app.window(),&client)!=0,
                        "Cannot read the performance window rectangle.");
                    const std::array<LONG,4> state{outer.right-outer.left,outer.bottom-outer.top,width,height};
                    const auto extent=app.presented()->drawing->extent;
                    calibration["attempts"].push_back({{"attempt",attempt},{"accepted_frame",app.presented()->drawing->id},
                        {"outer_extent",{state[0],state[1]}},{"client_extent",{client.right-client.left,client.bottom-client.top}},
                        {"frame_extent",{extent.width,extent.height}},{"dpi_scale",app.presented()->drawing->dpi_scale},
                        {"viewport_rect",{bounds.x,bounds.y,bounds.width,bounds.height}},
                        {"pixel_center_rect",{pixels.left,pixels.top,pixels.right,pixels.bottom}},
                        {"pixel_error",{2560-width,1440-height}}});
                    auto& sample=calibration["attempts"].back();
                    if(width==2560 && height==1440) {calibration["status"]="pass";return;}
                    require(width>0 && height>0,"Scene View has no calibratable pixel extent.");
                    require(std::ranges::find(visited,state)==visited.end(),
                        "Scene View calibration repeated a window/pixel state; see calibration attempts.");
                    visited.push_back(state);
                    require(attempt<(docked?32u:8u),
                        "Scene View pixel-centre coverage could not reach exactly 2560x1440; see calibration attempts.");
                    // Scale coarse corrections by the actual pane/client ratio.
                    // Small steps retain integer sizes near fractional split edges.
                    const auto dx=std::lround(double(2560-width)*(docked && std::abs(2560-width)>2?double(client.right-client.left)/width:1.));
                    const auto dy=std::lround(double(1440-height)*(docked && std::abs(1440-height)>2?double(client.bottom-client.top)/height:1.));
                    const auto next_width=state[0]+dx,next_height=state[1]+dy;
                    sample["requested_outer_extent"]={next_width,next_height};
                    require(next_width>0 && next_height>0 && next_width<=16384 && next_height<=16384,
                        "Scene View calibration exceeds the 16384-pixel window limit; resolution was not reduced.");
                    const bool resized=SetWindowPos(app.window(),nullptr,0,0,static_cast<int>(next_width),static_cast<int>(next_height),resize_flags)!=0;
                    const auto resize_error=resized?ERROR_SUCCESS:GetLastError();
                    sample["resize_succeeded"]=resized;sample["resize_error"]=resize_error;
                    require(GetWindowRect(app.window(),&outer)!=0 && GetClientRect(app.window(),&client)!=0,
                        "Cannot read the resized performance window rectangle.");
                    sample["actual_outer_extent"]={outer.right-outer.left,outer.bottom-outer.top};
                    sample["actual_client_extent"]={client.right-client.left,client.bottom-client.top};
                    require(resized,"Performance window resize failed; see calibration attempts.");
                    require(outer.right-outer.left==next_width && outer.bottom-outer.top==next_height,
                        "Windows constrained the requested calibration size; see requested and actual extents.");
                }
            } catch(const std::exception& error) {
                calibration["status"]="failed";calibration["error"]=error.what();throw;
            }
        };
        Access::set_scene_maximized(app,true);calibrate(false);
        auto bounds=Access::accepted_view(app)->bounds;
        auto extent=app.presented()->drawing->extent;
        auto dpi=app.presented()->drawing->dpi_scale;
        const auto camera=Access::camera_state(app);
        auto accepted_camera=*Access::accepted_view(app);
        auto pixel_range=renderer::detail::raster_viewport(bounds).scissor;
        std::string_view layout="maximized";
        const auto viewport_json=[&] {
            return Json{{"logical_rect_physical_px",{bounds.x,bounds.y,bounds.width,bounds.height}},
                {"logical_rect_dip",{bounds.x/dpi,bounds.y/dpi,bounds.width/dpi,bounds.height/dpi}},
                {"pixel_center_rect",{pixel_range.left,pixel_range.top,pixel_range.right,pixel_range.bottom}},
                {"receiver_pixel_extent",{pixel_range.right-pixel_range.left,pixel_range.bottom-pixel_range.top}},
                {"frame_extent",{extent.width,extent.height}},{"dpi_scale",dpi}};
        };
        const auto settings_json=[](const renderer::ViewSettings& settings) {
            Json values=Json::object();
            for(const auto& option:editor::view_options()) {
                const auto value=editor::view_value(settings,option.option);
                if(option.kind==editor::ViewValueKind::boolean) values[option.name]=value!=0;
                else if(option.kind==editor::ViewValueKind::enumeration) values[option.name]=option.choices[static_cast<std::size_t>(value)];
                else values[option.name]=value;
            }
            return values;
        };
        const auto camera_json=[&] {
            return Json{{"view",static_cast<unsigned>(camera.view)},{"target",{camera.target.x,camera.target.y,camera.target.z}},
                {"yaw",camera.yaw},{"pitch",camera.pitch},{"distance",camera.distance},
                {"view_matrix",accepted_camera.view.elements},{"projection_matrix",accepted_camera.projection.elements},
                {"far_depth",accepted_camera.far_depth}};
        };
        const auto active_panes=[&] {
            auto panes=Json::array();
            for(const auto& pane:Access::dock_presentation(app).panes) if(pane.active) {
                if(pane.bounds.width<=0 || pane.bounds.height<=0) continue;
                panes.push_back({{"tab",pane.active->key},{"title",editor::utf8(pane.active->title)},
                    {"maximized",pane.maximized}});
            }
            return panes;
        };
        report["viewport"]=viewport_json();report["original_view"]=settings_json(original_view);report["camera"]=camera_json();
        report["layouts"][layout]={{"viewport",viewport_json()},{"camera",camera_json()},{"active_panes",active_panes()}};
        bool camera_fixed=true;
        const auto invariant=[&] {
            require(Access::accepted_view(app) && Access::accepted_view(app)->bounds==bounds &&
                app.presented()->drawing->extent==extent && app.presented()->drawing->dpi_scale==dpi &&
                (!camera_fixed || Access::camera_state(app)==camera),
                "Viewport, render target, DPI or camera changed during the performance comparison.");
            const auto range=renderer::detail::raster_viewport(Access::accepted_view(app)->bounds).scissor;
            require(range.right-range.left==2560 && range.bottom-range.top==1440,
                "Actual receiver dispatch pixel coverage changed from native 1440p.");
            require(Access::scene_lit_view(app)==view,"View or World settings changed during the performance comparison.");
        };
        // The orbit case moves continuously: a triangle wave of .2 degrees of
        // yaw per accepted frame within +-12 degrees, without camera cuts.
        unsigned orbit_frame=0;
        const auto orbit_step=[&] {
            if(camera_fixed) return;
            const auto phase=orbit_frame++%240;
            auto state=camera;state.yaw+=(phase<120?float(phase)-60.f:180.f-float(phase))*.2f*.0174532925f;
            require(Access::move_camera(app,state),"The orbit camera state was rejected.");
        };
        const auto warmup=[&](unsigned frames) {
            const auto deadline=Clock::now()+std::chrono::minutes(10);
            for(unsigned i=0;i<frames;++i) {orbit_step();next_frame(deadline);invariant();}
            return app.presented()->drawing->id;
        };
        // Finish cold uploads and obtain a real completed sample before the two
        // timed cases. The fixed case warmup is additional to this preparation.
        std::uint64_t bytes=0;unsigned stable=0,prepared=0;
        const auto cold_deadline=Clock::now()+std::chrono::minutes(5);
        Clock::time_point cold_begin{},cold_end{};
        while(stable<16 || prepared<32 || !app.frame_statistics().scene_lit_gpu_timing_valid) {
            cold_end=next_frame(cold_deadline);invariant();++prepared;
            if(prepared==1) cold_begin=app.frame_record().begin;
            const auto& stats=app.frame_statistics();
            stable=bytes==stats.scene_lit_resource_bytes && !stats.material_upload_count?stable+1:0;
            bytes=stats.scene_lit_resource_bytes;
        }
        require(app.frame_statistics().scene_draw_calls>0,"The selected Scene contains no rendered geometry.");
        report["cold_preparation"]={{"accepted_frames",prepared},{"elapsed_ms",std::chrono::duration<double,std::milli>(cold_end-cold_begin).count()},
            {"logical_resource_bytes",bytes}};
        unchanged_scene();
        report["gpu_pass_order"]=Json::array();
        for(std::size_t i=0;i<editor::FramePerformance::total_pass;++i) report["gpu_pass_order"].push_back(editor::FramePerformance::passes()[i].key);
        // Obtain receiver counts separately. Diagnostic work is absent from
        // both measured paths, including their subsequent 192-frame warmups.
        view.lit.direct_cluster_inspect=true;apply_view(view);
        next_frame(Clock::now()+std::chrono::minutes(5));invariant();
        const auto diagnostic_first=app.presented()->drawing->id;
        const auto diagnostic_deadline=Clock::now()+std::chrono::minutes(5);
        for(;;) {
            next_frame(diagnostic_deadline);invariant();const auto& s=app.frame_statistics();
            if(!s.scene_lit_gpu_timing_valid || s.scene_lit_gpu_sample_frame_id<diagnostic_first) continue;
            require((s.scene_lit_gpu_pass_mask&(1u<<12))!=0,"Receiver inspection did not execute the light list pass.");
            const auto receivers=std::accumulate(s.scene_lit_direct_receiver_histogram.begin(),s.scene_lit_direct_receiver_histogram.end(),std::uint64_t{});
            require(receivers>0 && receivers<=2560ull*1440,"Receiver histogram did not cover valid visible Scene pixels.");
            report["receiver_inspection"]={{"sequence",s.scene_lit_gpu_sample_sequence},{"frame_id",s.scene_lit_gpu_sample_frame_id},
                {"view",settings_json(view)},{"valid_receiver_pixels",receivers},
                {"histogram",s.scene_lit_direct_receiver_histogram},{"max_sources",s.scene_lit_direct_receiver_max_sources},
                {"semantics","Valid GBuffer receivers covered by analytic local-light support spheres; bins 0..64 exact, 65 >64; excludes sun/emissive meshes and does not test occlusion. Excluded from timed cases."}};
            break;
        }
        view.lit.direct_cluster_inspect=false;
        struct Sample {
            std::uint64_t sequence{},frame{},observed_frame{},resources{};
            std::uint32_t mask{};std::array<std::uint32_t,3> clusters{};
            std::array<float,editor::FramePerformance::total_pass> ms{};
            std::array<std::uint32_t,14> shadows{};
            std::uint64_t probe_rays{},shadow_rays{};
            std::array<std::uint64_t,2> ddgi_visibility_rays{};
        };
        const auto distribution=[](std::vector<double> values) {
            require(!values.empty(),"Performance distribution has no observations.");
            const auto sum=std::accumulate(values.begin(),values.end(),0.);std::sort(values.begin(),values.end());
            return Json{{"count",values.size()},{"mean",sum/double(values.size())},
                {"p50",values[(values.size()*50+99)/100-1]},{"p95",values[(values.size()*95+99)/100-1]}};
        };
        struct CpuSample {
            std::uint64_t frame_id{};
            std::array<std::uint64_t,4> draws{};
            std::uint64_t scene_instances{},scene_culled_instances{};
            timing::FrameRecord record;
        };
        // Called right after next_frame, so the last step is the accepted one.
        const auto collect_cpu=[&](std::vector<CpuSample>& samples) {
            const auto& stats=app.frame_statistics();
            samples.push_back({app.presented()->drawing->id,{stats.scene_draw_calls,stats.ui_draw_calls,stats.viewport_draw_calls,stats.overlay_draw_calls},
                stats.scene_instances,stats.scene_culled_instances,app.frame_record()});
        };
        const auto cpu_json=[&](const std::vector<CpuSample>& samples) {
            const auto milliseconds=[](std::chrono::nanoseconds value){return std::chrono::duration<double,std::milli>(value).count();};
            // Phases entered in any of these steps, in catalog order; the last stage is the unattributed rest of the frame.
            std::vector<const timing::Marker*> entered;
            for(const auto* marker:timing::catalog())
                if(std::ranges::any_of(samples,[&](const auto& sample){return sample.record.count(*marker)!=0;})) entered.push_back(marker);
            std::vector<std::vector<double>> stages(entered.size()+1);
            for(auto& stage:stages) stage.reserve(samples.size());
            Json result{{"stages",Json::object()},{"samples",Json::array()}};
            for(const auto& sample:samples) {
                Json row{{"accepted_frame_id",sample.frame_id},{"timing_sequence",sample.record.sequence},{"scene_draw_calls",sample.draws[0]},
                    {"ui_draw_calls",sample.draws[1]},{"viewport_draw_calls",sample.draws[2]},{"overlay_draw_calls",sample.draws[3]},
                    {"scene_instances",sample.scene_instances},{"scene_culled_instances",sample.scene_culled_instances}};
                double children{};
                for(std::size_t i=0;i<entered.size();++i) {
                    const auto value=milliseconds(sample.record.time(*entered[i]));
                    stages[i].push_back(value);row[std::string(entered[i]->key())]=value;
                    // Entries of the phase in this step; a phase may run once per pass or not at all.
                    row["entries"][std::string(entered[i]->key())]=sample.record.count(*entered[i]);
                    if(entered[i]->parent()==&timing::frame) children+=value;
                }
                const auto unattributed=std::max(0.,milliseconds(sample.record.time(timing::frame))-children);
                stages.back().push_back(unattributed);row["frame.unattributed"]=unattributed;
                result["samples"].push_back(std::move(row));
            }
            for(std::size_t i=0;i<entered.size();++i) result["stages"][std::string(entered[i]->key())]=distribution(std::move(stages[i]));
            result["stages"]["frame.unattributed"]=distribution(std::move(stages.back()));
            // SceneSnapshot also composes inherited selection over every member when something is selected.
            result["selected_entities"]=app.document().selected_entities().size();
            return result;
        };
        for(const bool orbit:{false,true}) {
            apply_view(view);camera_fixed=!orbit;
            const auto fence=warmup(192);
            std::vector<Sample> samples;samples.reserve(240);
            std::vector<double> wall;wall.reserve(512);
            std::vector<CpuSample> cpu;cpu.reserve(512);
            std::uint64_t sequence=0;unsigned accepted=0;
            auto previous=frame_end(app.frame_record());const auto measurement_start=previous,deadline=Clock::now()+std::chrono::minutes(10);
            while(samples.size()<240) {
                orbit_step();const auto presented=next_frame(deadline);invariant();
                wall.push_back(std::chrono::duration<double,std::milli>(presented-previous).count());previous=presented;++accepted;
                collect_cpu(cpu);
                const auto& s=app.frame_statistics();const auto observed=app.presented()->drawing->id;
                if(!s.scene_lit_gpu_timing_valid || s.scene_lit_gpu_sample_frame_id<=fence || s.scene_lit_gpu_sample_sequence<=sequence) continue;
                require(s.scene_lit_gpu_sample_frame_id<=observed,"Completed GPU sample refers to a future frame.");
                require(s.scene_lit_direct_receiver_max_sources==0 &&
                    std::ranges::all_of(s.scene_lit_direct_receiver_histogram,[](auto n){return n==0;}),"Receiver diagnostics leaked into timed samples.");
                Sample sample{s.scene_lit_gpu_sample_sequence,s.scene_lit_gpu_sample_frame_id,observed,s.scene_lit_resource_bytes,
                    s.scene_lit_gpu_pass_mask,{s.scene_lit_direct_cluster_cells,s.scene_lit_direct_cluster_overflows,s.scene_lit_direct_cluster_max_sources},{}};
                std::copy(s.scene_lit_gpu_ms.begin(),s.scene_lit_gpu_ms.end(),sample.ms.begin());
                std::copy(s.scene_lit_ddgi_gpu_ms.begin(),s.scene_lit_ddgi_gpu_ms.end(),sample.ms.begin()+8);
                std::copy(s.scene_lit_ddgi_update_gpu_ms.begin(),s.scene_lit_ddgi_update_gpu_ms.end(),sample.ms.begin()+10);
                sample.ms[12]=s.scene_lit_direct_cluster_gpu_ms;
                sample.ms[13]=s.scene_lit_shadow_cache_gpu_ms;
                sample.ms[14]=s.scene_lit_direct_shading_gpu_ms;
                sample.probe_rays=s.scene_lit_probe_rays;sample.shadow_rays=s.scene_lit_shadow_rays;
                sample.ddgi_visibility_rays=s.scene_lit_ddgi_visibility_rays;
                sample.shadows={s.scene_lit_shadow_cache_faces,s.scene_lit_shadow_cache_reused_faces,
                    s.scene_lit_shadow_cache_static_updates,s.scene_lit_shadow_cache_dynamic_updates,s.scene_lit_shadow_cache_draws,
                    s.scene_lit_shadow_cache_resolution_downgrades,s.scene_lit_shadow_cache_dropped_lights,s.scene_lit_shadow_cache_near_plane_faces,
                    s.scene_lit_shadow_cache_missing_taps,s.scene_lit_shadow_cache_plane_fallbacks,
                    s.scene_lit_shadow_cache_deferred_faces,s.scene_lit_shadow_cache_deferral_frames,s.scene_lit_shadow_cache_atlas_cells,
                    s.scene_lit_shadow_cache_pending_lights};
                require(std::ranges::all_of(sample.ms,[](auto n){return std::isfinite(n) && n>=0;}),"Invalid completed GPU duration.");
                samples.push_back(sample);sequence=sample.sequence;
            }
            const auto elapsed=std::chrono::duration<double,std::milli>(previous-measurement_start).count();
            const std::string name=orbit?"clustered_orbit":"clustered";
            Json result{{"name",name},{"layout",layout},{"camera_motion",orbit?Json{{"yaw_degrees_per_frame",.2},{"yaw_amplitude_degrees",12},{"camera_cut",false}}:Json(nullptr)},{"view",settings_json(view)},{"viewport",viewport_json()},
                {"camera",camera_json()},{"warmup_last_frame",fence},{"accepted_frames",accepted},{"elapsed_ms",elapsed},
                {"accepted_steps_per_second",double(accepted)*1000./elapsed},{"gpu_sample_state","completed"},{"cpu",cpu_json(cpu)},
                {"wall_present_interval_ms",distribution(wall)},{"wall_present_intervals_ms",wall},{"gpu_passes",Json::object()},
                {"samples",Json::array()}};
            std::vector<double> totals;totals.reserve(samples.size());
            const auto definitions=editor::FramePerformance::passes();
            for(std::size_t pass=0;pass<editor::FramePerformance::total_pass;++pass) {
                std::vector<double> values;values.reserve(samples.size());unsigned active=0;
                for(const auto& sample:samples) {values.push_back(sample.ms[pass]);active+=(sample.mask&(1u<<pass))!=0;}
                result["gpu_passes"][definitions[pass].key]={{"ms",distribution(std::move(values))},{"active_samples",active},
                    {"parent",definitions[pass].parent<0?Json(nullptr):Json(definitions[definitions[pass].parent].key)}};
            }
            for(const auto& sample:samples) {
                totals.push_back(std::accumulate(sample.ms.begin(),sample.ms.begin()+8,0.));
                result["samples"].push_back({{"sequence",sample.sequence},{"frame_id",sample.frame},{"observed_at_frame",sample.observed_frame},
                    {"pass_mask",sample.mask},{"pass_ms",sample.ms},{"logical_resource_bytes",sample.resources},
                    {"probe_rays",sample.probe_rays},{"shadow_rays",sample.shadow_rays},
                    {"ddgi_visibility_rays",sample.ddgi_visibility_rays},
                    {"shadow_cache",{{"faces",sample.shadows[0]},{"reused_faces",sample.shadows[1]},
                        {"static_updates",sample.shadows[2]},{"dynamic_updates",sample.shadows[3]},{"draws",sample.shadows[4]},
                        {"resolution_downgrades",sample.shadows[5]},{"dropped_lights",sample.shadows[6]},{"near_plane_faces",sample.shadows[7]},
                        {"missing_taps",sample.shadows[8]},{"plane_fallbacks",sample.shadows[9]},
                        {"deferred_faces",sample.shadows[10]},{"deferral_frames",sample.shadows[11]},{"atlas_cells",sample.shadows[12]},
                        {"pending_lights",sample.shadows[13]}}},
                    {"direct_cluster",{{"cells",sample.clusters[0]},{"overflows",sample.clusters[1]},{"max_sources",sample.clusters[2]}}}});
            }
            result["measured_main_total_ms"]=distribution(std::move(totals));
            unchanged_scene();report["cases"].push_back(std::move(result));report["status"]="sampling";write_report();
            capture(app,output/(name+".png"));
        }
        camera_fixed=true;require(Access::move_camera(app,camera),"The comparison camera could not be restored.");
        const auto non_lit_case=[&](renderer::Shading shading,std::string_view name) {
            view.shading=shading;apply_view(view);
            const auto fence=warmup(192);
            std::vector<double> wall;wall.reserve(240);
            std::vector<CpuSample> cpu;cpu.reserve(240);
            auto previous=frame_end(app.frame_record());const auto measurement_start=previous,deadline=Clock::now()+std::chrono::minutes(10);
            for(unsigned accepted=0;accepted<240;++accepted) {
                const auto presented=next_frame(deadline);invariant();
                wall.push_back(std::chrono::duration<double,std::milli>(presented-previous).count());previous=presented;
                collect_cpu(cpu);
            }
            const auto elapsed=std::chrono::duration<double,std::milli>(previous-measurement_start).count();
            // Non-Lit modes have no Scene Lit GPU scopes. Never reuse the last
            // completed Lit sample or infer GPU time from accepted step rate.
            Json result{{"name",name},{"layout",layout},{"view",settings_json(view)},{"viewport",viewport_json()},{"camera",camera_json()},
                {"warmup_last_frame",fence},{"accepted_frames",240},{"elapsed_ms",elapsed},{"accepted_steps_per_second",240000./elapsed},
                {"wall_present_interval_ms",distribution(wall)},{"wall_present_intervals_ms",wall},{"cpu",cpu_json(cpu)},
                {"gpu_sample_state","unavailable_non_lit"},{"gpu_passes",nullptr},{"measured_main_total_ms",nullptr},{"samples",Json::array()}};
            unchanged_scene();report["cases"].push_back(std::move(result));report["status"]="sampling";write_report();
            capture(app,output/(std::string(name)+".png"));
        };
        non_lit_case(renderer::Shading::smooth,"smooth");non_lit_case(renderer::Shading::matcap,"matcap");
        // Restore the retained Dock tree/ratios, not a freshly reset layout.
        Access::set_scene_maximized(app,false);calibrate(true);
        require(Access::camera_state(app)==camera,"Dock restoration changed the comparison camera.");
        require(app.presented()->drawing->dpi_scale==dpi,"Dock restoration changed the comparison DPI.");
        layout="docked";bounds=Access::accepted_view(app)->bounds;extent=app.presented()->drawing->extent;
        dpi=app.presented()->drawing->dpi_scale;accepted_camera=*Access::accepted_view(app);
        pixel_range=renderer::detail::raster_viewport(bounds).scissor;
        const auto panes=active_panes();
        require(panes.size()>1 && std::ranges::none_of(panes,[](const auto& pane){return pane.at("maximized").template get<bool>();}),
            "Dock restoration did not reveal the normal editor panels.");
        report["layouts"][layout]={{"viewport",viewport_json()},{"camera",camera_json()},{"active_panes",panes}};
        non_lit_case(renderer::Shading::smooth,"docked_smooth");non_lit_case(renderer::Shading::matcap,"docked_matcap");
        apply_view(original_view);restore.active=false;unchanged_scene();
        report["status"]="pass";report["restored_view"]=settings_json(Access::scene_lit_view(app));write_report();
        app.close();
        std::puts("Editor render performance profile: written (native 1440p, VSync off, maximized Lit/Smooth/MatCap, restored Dock Smooth/MatCap, CPU stage timings)");
        return 0;
    } catch(const std::exception& error) {
        report["status"]="failed";report["error"]=error.what();write_report();throw;
    }
}
