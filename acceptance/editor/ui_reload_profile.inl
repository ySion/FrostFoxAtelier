// Profile the normal VSync editor loop and keep the author's file untouched.
atelier::Result<int> profile_editor_ui_reload(Application& app, const std::filesystem::path& output) {
    using namespace atelier;
    using Clock = timing::Clock;
    auto& workspace = Access::workspace(app);
    ATELIER_TRY_BIND(bytes, assets::read_file(workspace.ui_source(), 1024 * 1024));
    const auto& original = *bytes;
    constexpr std::string_view function = "double shelf_expanded_height() { return 56.0d; }";
    const auto at = original.find(function);
    require(at != original.npos && original.find(function, at + 1) == original.npos,
            "ToolShelf height fixture no longer matches the editor source.");
    auto taller = original;
    taller.replace(at + function.find("56"), 2, "57");
    std::filesystem::create_directories(output);
    const auto author = output / "editor.ui.source";
    ATELIER_TRY(editor::write_file(author, original, editor::WriteMode::create));
    workspace.set_ui_source(author);

    struct Step {
        Application::StepTimes times;
        Clock::duration input_gap, accepted_gap;
        timing::FrameRecord frame;
        std::uint64_t accepted_id;
    };
    Clock::time_point last_input{}, last_accepted{};
    const auto step = [&]() -> Result<Step> {
        Application::StepTimes times;
        ATELIER_TRY_BIND(running, app.step(nullptr, &times));
        require(*running, "Editor closed during UI reload profiling.");
        require(!app.frame_record().structure_errors, "Invalid editor frame timing.");
        if (!workspace.ui_error().empty())
            throw std::runtime_error(std::string(workspace.ui_error()));
        const auto gap = [](Clock::time_point now, Clock::time_point& last) {
            if (now == Clock::time_point{})
                return Clock::duration{};
            const auto elapsed = last == Clock::time_point{} ? Clock::duration{} : now - last;
            last = now;
            return elapsed;
        };
        return Step{times, gap(times.input, last_input), gap(times.accepted, last_accepted), app.frame_record(),
                    times.accepted == Clock::time_point{} ? 0 : app.presented()->drawing->id};
    };
    const auto deadline = Clock::now() + std::chrono::minutes(2);
    while (app.accepted_frames() < 45 || Access::view_preparing(app)) {
        require(Clock::now() < deadline, "Editor scene did not warm up for UI reload profiling.");
        ATELIER_TRY(step());
    }
    const auto height_px = [&] {
        const auto* shelf = app.presented()->find(editor::control::shelf);
        require(shelf != nullptr, "Accepted editor frame lacks ToolShelf.");
        return shelf->bounds.height;
    };
    const auto has_height = [&](float expected) {
        return std::abs(height_px() - expected * app.presented()->drawing->dpi_scale) < 0.01f;
    };
    if (!has_height(56))
        throw std::runtime_error("Unexpected initial ToolShelf height in pixels: " + std::to_string(height_px()));
    using Profile = ui_language::LiveDocument::Profile;
    std::ofstream samples(output / "reloads.csv", std::ios::noreplace), frames(output / "steps.csv", std::ios::noreplace);
    std::ofstream components(output / "components.csv", std::ios::noreplace);
    require(samples && frames && components, "UI reload profile needs writable, new output files.");
    components << "trial,edit,revision,component,decision,total_ns,request_ns,frontend_ns,select_ns,native_compile_ns,"
                  "ir_ns,optimize_ns,first_lookup_ns,object_ns,publish_ns,bind_ns\n";
    samples
        << "trial,edit,revision,height,height_px,dpi,compiled,reused,regions,compile_step,accepted_step,accepted_id,save_ms,"
           "detect_ms,debounce_ms,read_ms,compile_ms,rebind_ms,append_updates_ms,other_to_commit_ms,"
           "commit_to_accept_begin_ms,ui_accept_ms,total_ms,debounce_accepted,accepted_gap_ms,compile_input_gap_ms,ui_lower_ns\n";
    frames << "trial,step,input_from_save_ms,accepted_id,accepted_from_save_ms,input_gap_ms,accepted_gap_ms,step_ms";
    std::vector<const timing::Marker*> phases;
    for (const auto* phase : timing::catalog())
        if (phase->parent() == &timing::frame || phase == &renderer::phases::present) {
            phases.push_back(phase);
            frames << ',' << phase->key() << "_ms";
        }
    frames << '\n';
    const auto ms = [](Clock::duration duration) {
        return std::chrono::duration<double, std::milli>(duration).count();
    };
    bool expanded_edit{}, comment{};
    // Each triplet includes the identical height edit with and without compiler
    // timers, plus a trivia-only edit. Balance all six orders within each process.
    enum class Sample { detailed_height, coarse_height, trivia_only };
    using enum Sample;
    constexpr Sample orders[][3] = {
        {detailed_height, coarse_height, trivia_only}, {trivia_only, coarse_height, detailed_height},
        {coarse_height, trivia_only, detailed_height}, {detailed_height, trivia_only, coarse_height},
        {trivia_only, detailed_height, coarse_height}, {coarse_height, detailed_height, trivia_only}};
    for (unsigned trial = 0; trial < 36; ++trial) {
        // Vary the save phase within the 100 ms watch interval while continuing to pump the real editor.
        const auto start_after = Clock::now() + std::chrono::milliseconds((trial * 37) % 113);
        while (Clock::now() < start_after)
            ATELIER_TRY(step());
        const auto sample_kind = orders[(trial / 3) % std::size(orders)][trial % 3];
        const bool height_edit = sample_kind != trivia_only;
        const bool detailed = sample_kind != coarse_height;
        const auto edit = detailed ? (height_edit ? "height" : "comment") : "height_coarse";
        const auto* reload = workspace.profile_ui_reloads(detailed ? Profile::compiler : Profile::frame);
        require(reload != nullptr, "Missing live editor authoring document.");
        if (height_edit)
            expanded_edit = !expanded_edit;
        else
            comment = !comment;
        const auto height = expanded_edit ? 57.f : 56.f;
        auto next = expanded_edit ? taller : original;
        if (comment)
            next.insert(at + function.find("return") + std::string_view("return").size(), "\t/* tool shelf */\n    ");
        const auto revision = workspace.ui_revision() + 1;
        std::vector<Step> steps;
        steps.reserve(128);
        const auto write_begin = Clock::now();
        ATELIER_TRY(editor::write_file(author, next));
        const auto saved = Clock::now();
        for (;;) {
            require(Clock::now() - saved < std::chrono::seconds(15), "No accepted frame for the saved UI revision.");
            ATELIER_TRY_BIND(sample, step());
            const auto& current = steps.emplace_back(std::move(*sample));
            if (reload->revision == revision && current.accepted_id && current.times.accepted >= reload->committed) {
                require(has_height(height), "Accepted frame has stale ToolShelf geometry.");
                break;
            }
        }
        const auto accepted = steps.back();
        const auto accepted_height_px = height_px();
        const auto accepted_dpi = app.presented()->drawing->dpi_scale;
        // The interval containing compilation ends when the window can poll input again.
        ATELIER_TRY_BIND(following, step());
        steps.push_back(std::move(*following));
        const auto changed = std::ranges::find_if(steps, [&](const auto& s) {
            return s.frame.begin <= reload->compile_begin &&
                   reload->compile_end <= s.frame.begin + s.frame.time(timing::frame);
        });
        require(changed != steps.end() && changed + 1 != steps.end(),
                "Cannot attribute compilation to an editor step.");
        require(saved <= reload->detected && reload->committed <= accepted.times.accept_begin &&
                    reload->compilation.compiled == (height_edit ? 1u : 0u) &&
                    reload->compilation.reused == (height_edit ? 43u : 44u) &&
                    workspace.ui_component("ToolShelf")->compilation() ==
                        (height_edit ? ui_language::Component::Compilation::compiled
                                     : ui_language::Component::Compilation::reused_source),
                "Invalid source version or component reuse in UI reload sample.");
        const auto wait_frames = std::ranges::count_if(steps, [&](const Step& s) {
            return s.accepted_id && s.times.accepted >= reload->detected && s.times.accepted < reload->read_begin;
        });
        samples << trial << ',' << edit << ',' << revision << ',' << height << ',' << accepted_height_px << ','
                << accepted_dpi << ',' << reload->compilation.compiled << ',' << reload->compilation.reused << ','
                << reload->rebound_regions << ',' << changed->frame.sequence << ',' << accepted.frame.sequence << ','
                << accepted.accepted_id << ',' << ms(saved - write_begin) << ',' << ms(reload->detected - saved) << ','
                << ms(reload->read_begin - reload->detected) << ',' << ms(reload->compile_begin - reload->read_begin)
                << ',' << ms(reload->compile_end - reload->compile_begin) << ','
                << ms(reload->instances_ready - reload->compile_end) << ',' << ms(reload->append_updates) << ','
                << ms(reload->committed - reload->instances_ready - reload->append_updates) << ','
                << ms(accepted.times.accept_begin - reload->committed) << ','
                << ms(accepted.times.accepted - accepted.times.accept_begin) << ','
                << ms(accepted.times.accepted - saved) << ',' << wait_frames << ',' << ms(accepted.accepted_gap) << ','
                << ms((changed + 1)->input_gap) << ',' << reload->module.lower_ns << '\n';
        require(reload->module.components.size() == (detailed ? 44u : 0u), "Incorrect compiler timing mode.");
        for (const auto& component : reload->module.components) {
            using Decision = ui_language::Component::Compilation;
            const auto expected = !height_edit                    ? Decision::reused_source
                                  : component.name == "ToolShelf" ? Decision::compiled
                                                                  : Decision::reused_dependencies;
            require(component.disposition == expected, "Unexpected component decision in compiler profile.");
            const auto& source = component.source;
            require(source.ir_ns >= source.optimize_ns && source.first_lookup_ns >= source.object_ns &&
                        source.native_compile_ns >= source.ir_ns + source.first_lookup_ns,
                    "Nested compiler timings do not match the synchronous compile path.");
            const auto decision = expected == Decision::compiled        ? "compiled"
                                  : expected == Decision::reused_source ? "reused_source"
                                                                        : "reused_dependencies";
            components << trial << ',' << edit << ',' << revision << ',' << component.name << ',' << decision << ','
                       << component.total_ns << ',' << component.request_ns << ',' << source.frontend_ns << ','
                       << component.select_ns << ',' << source.native_compile_ns << ',' << source.ir_ns << ','
                       << source.optimize_ns << ',' << source.first_lookup_ns << ',' << source.object_ns << ','
                       << component.publish_ns << ',' << component.bind_ns << '\n';
        }
        for (const auto& s : steps) {
            const auto& r = s.frame;
            frames << trial << ',' << r.sequence << ',' << ms(s.times.input - saved) << ',' << s.accepted_id << ',';
            if (s.accepted_id)
                frames << ms(s.times.accepted - saved);
            frames << ',' << ms(s.input_gap) << ',' << ms(s.accepted_gap) << ',' << ms(r.time(timing::frame));
            for (const auto* phase : phases)
                frames << ',' << ms(r.time(*phase));
            frames << '\n';
        }
    }
    workspace.profile_ui_reloads(Profile::off);
    samples.close();
    frames.close();
    components.close();
    require(samples && frames && components, "Cannot write UI reload profile.");
    ATELIER_TRY(app.close());
    std::puts(
        "Editor UI reload profile: 12 detailed ToolShelf saves, 12 matched coarse saves and 12 trivia controls written.");
    return 0;
}
