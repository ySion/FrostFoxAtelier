namespace {
// Use the accepted definitions in their owning Runtime. Batch the clocks outside
// the dispatch loop so individual JIT calls do not pay for two clock reads.
void profile_accepted_dispatch(atelier::editor::Workspace& workspace, const std::filesystem::path& output) {
    using namespace atelier;
    using namespace ui_language;
    using Clock = timing::Clock;
    ui::ControlKey next_key = 1000;
    const std::array<KeyBinding, 1> shelf_keys{{{"shelf", 11}}}, group_keys{{{"title", 12}}};
    const auto tools = [](ui::ControlKey key, std::wstring title) {
        return ItemList{std::make_shared<const std::vector<Item>>(
            std::initializer_list<Item>{{key, {std::move(title), 1., ActionBinding{key}}}})};
    };
    std::array<InputValue, 12> inputs{false,
                                      true,
                                      tools(21, L"Point Light"),
                                      tools(22, L"Camera"),
                                      ActionBinding{41},
                                      ActionBinding{42},
                                      ActionBinding{43},
                                      36.,
                                      24.,
                                      16.,
                                      1.,
                                      2.};
    const std::array<InputValue, 3> group_inputs{std::wstring(L"Transform"), 0., ItemList{}};
    auto shelf = take(Instance::create(workspace.ui_component("ToolShelf"), 0, next_key, shelf_keys, inputs));
    auto group = take(Instance::create(workspace.ui_component("PropertyGroup"), 0, next_key, group_keys, group_inputs));
    const ui::Action events[] = {{ui::ActionKind::activate, 12, false}, {ui::ActionKind::activate, 12, true}};
    std::ofstream csv(output / "dispatch.csv", std::ios::noreplace);
    require(csv.good(), "Cannot create the UI dispatch profile.");
    csv << "batch,workload,operations,evaluations,elapsed_ns\n";
    const auto record = [&](int batch, const char* workload, unsigned count, std::uint64_t evaluations,
                            Clock::duration elapsed) {
        if (batch >= 0)
            csv << batch << ',' << workload << ',' << count << ',' << evaluations << ','
                << std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() << '\n';
    };
    bool collapsed{}, checked{true};
    const auto sample_shelf = [&](int batch) {
        constexpr unsigned count = 1025; // Odd counts exercise both final control states across batches.
        const auto before = shelf->statistics().binding_evaluations;
        const auto start = Clock::now();
        for (unsigned i = 0; i < count; ++i) {
            collapsed = !collapsed;
            inputs[0] = collapsed;
            take(shelf->update(inputs));
            shelf->commit_controls();
        }
        const auto elapsed = Clock::now() - start;
        const auto evaluations = shelf->statistics().binding_evaluations - before;
        const auto controls = shelf->controls();
        const auto row = std::ranges::find(controls, shelf_keys.front().key, &ui::Control::key);
        require(row != controls.end() && row->layout.height.value == (collapsed ? 24.f : 56.f) && evaluations >= count,
                "ToolShelf did not evaluate and commit its new height.");
        record(batch, "shelf_update", count, evaluations, elapsed);
    };
    const auto sample_event = [&](int batch) {
        constexpr unsigned count = 16385;
        const auto before = group->statistics().event_evaluations;
        const auto start = Clock::now();
        for (unsigned i = 0; i < count; ++i) {
            checked = !checked;
            require(take(group->event(events[checked])), "PropertyGroup did not handle activation.");
        }
        const auto elapsed = Clock::now() - start;
        const auto evaluations = group->statistics().event_evaluations - before;
        take(group->update());
        group->commit_controls();
        const auto controls = group->controls();
        const auto title = std::ranges::find(controls, group_keys.front().key, &ui::Control::key);
        require(title != controls.end() && title->selected == checked && evaluations == count,
                "PropertyGroup did not publish the final event's checked state.");
        record(batch, "group_event", count, evaluations, elapsed);
    };
    for (int batch = -1; batch < 12; ++batch) {
        if (batch % 2 == 0) {
            sample_event(batch);
            sample_shelf(batch);
        } else {
            sample_shelf(batch);
            sample_event(batch);
        }
    }
    csv.close();
    require(csv.good(), "Cannot write the UI dispatch profile.");
}
}

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
    std::ofstream samples(output / "reloads.csv", std::ios::noreplace),
        frames(output / "steps.csv", std::ios::noreplace);
    std::ofstream components(output / "components.csv", std::ios::noreplace);
    require(samples && frames && components, "UI reload profile needs writable, new output files.");
    components << "trial,edit,revision,component,decision,total_ns,request_ns,frontend_ns,select_ns,native_compile_ns,"
                  "ir_ns,optimize_ns,first_lookup_ns,object_ns,publish_ns,bind_ns\n";
    samples
        << "trial,edit,revision,height,height_px,dpi,compiled,reused,regions,compile_step,accepted_step,accepted_id,save_ms,"
           "detect_ms,debounce_ms,read_ms,compile_ms,rebind_ms,append_updates_ms,other_to_commit_ms,"
           "commit_to_accept_begin_ms,ui_accept_ms,total_ms,debounce_accepted,accepted_gap_ms,compile_input_gap_ms,ui_lower_ns,stage_ms,worker_ms,result_wait_ms,install_ms,worker_inputs,worker_old_frames\n";
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
        const auto old_height = height_px();
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
            if (current.accepted_id)
                require(height_px() == old_height, "An intermediate frame exposed uncommitted ToolShelf geometry.");
        }
        const auto accepted = steps.back();
        const auto accepted_height_px = height_px();
        const auto accepted_dpi = app.presented()->drawing->dpi_scale;
        // The interval containing compilation ends when the window can poll input again.
        ATELIER_TRY_BIND(following, step());
        steps.push_back(std::move(*following));
        const auto containing = [&](Clock::time_point point) {
            return std::ranges::find_if(steps, [&](const auto& s) {
                return s.frame.begin <= point && point <= s.frame.begin + s.frame.time(timing::frame);
            });
        };
        const auto changed = containing(reload->compile_begin);
        const auto installed = containing(reload->compile_end);
        require(changed != steps.end() && installed != steps.end() && changed <= installed &&
                    installed + 1 != steps.end(),
                "Cannot attribute compiler capture and publication to editor steps.");
        const auto worst_gap = std::max_element(changed + 1, installed + 2, [](const Step& a, const Step& b) {
                                   return a.input_gap < b.input_gap;
                               })->input_gap;
        const auto on_worker = reload->worker_begin != Clock::time_point{};
        require(on_worker == height_edit, "Unexpected worker dispatch for this author edit.");
        const auto during_worker = [&](Clock::time_point point) {
            return on_worker && reload->worker_begin <= point && point < reload->worker_end;
        };
        const auto worker_inputs =
            std::ranges::count_if(steps, [&](const auto& s) { return during_worker(s.times.input); });
        const auto worker_frames = std::ranges::count_if(
            steps, [&](const auto& s) { return s.accepted_id && during_worker(s.times.accepted); });
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
                << ms(worst_gap) << ',' << reload->module.lower_ns << ',' << ms(reload->staged - reload->compile_begin)
                << ',' << ms(reload->worker_end - reload->worker_begin) << ','
                << (on_worker ? ms(reload->install_begin - reload->worker_end) : 0.) << ','
                << ms(reload->compile_end - reload->install_begin) << ',' << worker_inputs << ',' << worker_frames
                << '\n';
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
    // Exercise the window message queue while a real height compilation is in flight.
    const auto* interactive = workspace.profile_ui_reloads(Profile::frame);
    const auto base = workspace.ui_revision();
    const auto click_save = Clock::now();
    const auto until = click_save + std::chrono::seconds(15);
    ATELIER_TRY(editor::write_file(author, taller));
    do {
        require(Clock::now() < until, "Interactive ToolShelf save was not dispatched.");
        ATELIER_TRY(step());
    } while (interactive->staged < click_save);
    const auto click_shelf = [&] {
        const auto* handle = app.presented()->find(editor::control::shelf_handle);
        require(handle != nullptr, "ToolShelf toggle missing from accepted frame.");
        const auto& r = handle->bounds;
        const auto point = MAKELPARAM(static_cast<int>(r.x + r.width / 2), static_cast<int>(r.y + r.height / 2));
        require(PostMessageW(app.window(), WM_LBUTTONDOWN, MK_LBUTTON, point) &&
                    PostMessageW(app.window(), WM_LBUTTONUP, 0, point),
                "Cannot post ToolShelf toggle.");
    };
    const auto posted = Clock::now();
    click_shelf();
    Clock::time_point input{}, folded{};
    bool folded_old{};
    while (interactive->revision != base + 1 || folded == Clock::time_point{}) {
        require(Clock::now() < until, "ToolShelf failed to fold during hot reload.");
        ATELIER_TRY_BIND(current, step());
        if (input == Clock::time_point{} && workspace.shelf_collapsed)
            input = current->times.input;
        if (folded == Clock::time_point{} && current->accepted_id && has_height(ui::stow_collapsed_height)) {
            folded = current->times.accepted;
            folded_old = workspace.ui_revision() == base;
        }
    }
    require(workspace.shelf_collapsed && input != Clock::time_point{}, "Hot reload lost the fold action.");
    std::ofstream interaction(output / "input.csv", std::ios::noreplace);
    interaction << "post_to_folded_frame_ms,input_during_worker,folded_old_revision\n"
                << ms(folded - posted) << ',' << (interactive->worker_begin <= input && input < interactive->worker_end)
                << ',' << folded_old << '\n';
    interaction.close();
    require(interaction.good(), "Cannot write UI input sample.");
    click_shelf();
    do {
        require(Clock::now() < until, "ToolShelf failed to reopen with the new height.");
        ATELIER_TRY(step());
    } while (!has_height(57));
    ATELIER_TRY(editor::write_file(author, original));
    do {
        require(Clock::now() < until, "ToolShelf failed to restore the original height.");
        ATELIER_TRY(step());
    } while (workspace.ui_revision() != base + 2 || !has_height(56));
    // ToolShelf routes button actions to the host. Hot-rebuild the editor's own
    // foldout handler before timing events, then restore the exact authored body.
    auto edited_group = original;
    const auto group_at = original.find("ui PropertyGroup(");
    constexpr std::string_view handler = "state.expanded = event.checked;";
    const auto handler_at = original.find(handler, group_at);
    require(group_at != original.npos && handler_at != original.npos && handler_at < original.find("\nui ", group_at),
            "PropertyGroup event fixture no longer matches the editor source.");
    edited_group.replace(handler_at, handler.size(), "state.expanded = !event.checked;");
    const auto* event_reload = workspace.profile_ui_reloads(Profile::frame);
    for (const auto* text : {&std::as_const(edited_group), &original}) {
        const auto revision = workspace.ui_revision() + 1;
        const auto end = Clock::now() + std::chrono::seconds(15);
        ATELIER_TRY(editor::write_file(author, *text));
        for (;;) {
            require(Clock::now() < end, "PropertyGroup hot edit did not reach an accepted frame.");
            ATELIER_TRY_BIND(current, step());
            if (event_reload->revision == revision && current->accepted_id &&
                current->times.accepted >= event_reload->committed)
                break;
        }
        require(
            workspace.ui_component("PropertyGroup")->compilation() == ui_language::Component::Compilation::compiled &&
                workspace.ui_component("ToolShelf")->compilation() != ui_language::Component::Compilation::compiled &&
                has_height(56),
            "Unexpected component reuse after PropertyGroup hot edit.");
    }
    workspace.profile_ui_reloads(Profile::off);
    profile_accepted_dispatch(workspace, output);
    samples.close();
    frames.close();
    components.close();
    require(samples && frames && components, "Cannot write UI reload profile.");
    ATELIER_TRY(app.close());
    std::puts("Editor UI reload profile: 12 detailed height saves, 12 coarse saves, 12 trivia controls and "
              "post-reload attribute/event dispatch timings written.");
    return 0;
}
