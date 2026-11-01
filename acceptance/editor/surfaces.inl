int verify_editor_surfaces(Application& app, const char* output_directory) {
    using namespace atelier;
    namespace control = atelier::editor::control;
    using Access = application::EditorAcceptanceAccess;
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    hr(com);
    struct ComScope {
        ~ComScope() { CoUninitialize(); }
    } com_scope;
    ExternalMouseFilter external_mouse;
    const auto output = std::filesystem::absolute(assets::wide(output_directory));
    std::filesystem::create_directories(output);
    tick(app);
    require(bool(Access::invoke(app, "scene.add", {{"kind", "mesh"}})), "Surface scene creation failed.");
    tick(app);
    const auto point = [&](ui::ControlKey key) {
        const auto* control = app.presented()->find(key);
        if (!control)
            throw std::runtime_error("Surface control is absent: " + std::to_string(key));
        return math::Vector2{control->bounds.x + control->bounds.width * .5f,
                             control->bounds.y + control->bounds.height * .5f};
    };
    const auto pointer = [&](ui::EventKind kind, math::Vector2 p) {
        Access::input(app, {.kind = kind, .x = p.x, .y = p.y, .button = ui::PointerButton::primary});
    };
    const auto escape = [&] {
        Access::input(app, {.kind = ui::EventKind::key_down, .key = ui::Key::escape}, VK_ESCAPE);
        Access::input(app, {.kind = ui::EventKind::key_up, .key = ui::Key::escape}, VK_ESCAPE);
    };
    std::ofstream report(output / "surfaces.csv");
    report << "menu,open,presents\n";
    unsigned redundant_presents{};
    HWND retained{};
    for (unsigned run = 0; run < 6; ++run) {
        const auto menu = run % 2 ? control::scene_shading : control::viewport_camera;
        const auto p = point(menu);
        pointer(ui::EventKind::pointer_move, p);
        pointer(ui::EventKind::pointer_down, p);
        pointer(ui::EventKind::pointer_up, p);
        tick(app, 1);
        require(app.menu_frame() != nullptr, "Surface menu was not ready after its opening frame.");
        const auto menu_frame = app.menu_frame();
        const auto first = std::ranges::find(menu_frame->controls, ui::ControlKind::menu_item, &ui::ControlBox::kind);
        const auto root = std::ranges::find(menu_frame->controls, ui::ControlKind::popup, &ui::ControlBox::kind);
        require(first != menu_frame->controls.end() && root != menu_frame->controls.end() &&
                    first->bounds.y - root->bounds.y < 4 * menu_frame->drawing->dpi_scale,
                "Popup has a leading separator or empty first row.");
        if (retained)
            require(retained == app.menu_window(), "Alternating menu sizes replaced the native popup.");
        retained = app.menu_window();
        const auto presents = Access::menu_presents(app);
        if (run && presents != 1)
            ++redundant_presents;
        report << menu << ',' << run << ',' << presents << '\n';
        report.flush();
        escape();
        tick(app, 1);
    }
    // A monitor/DPI change must remeasure a retained popup without leaving its
    // measurement Session in the dismissed modal state.
    Access::invalidate_menu_measurement(app);
    {
        const auto p = point(control::file_menu);
        pointer(ui::EventKind::pointer_down, p);
        pointer(ui::EventKind::pointer_up, p);
        tick(app, 1);
        require(app.menu_frame() && app.menu_frame()->find(Access::operator_item(app, "app.quit")),
                "Retained File menu lost its visible root after measurement surface invalidation.");
        require(retained == app.menu_window(), "Menu remeasurement replaced the native popup.");
        escape();
        tick(app, 1);
        report << "menu-measurement,surface-invalidation-reopen,passed\n";
        report.flush();
    }
    // A retained checkbox updates its accepted row without reopening the surface.
    {
        const auto p = point(control::scene_guides);
        pointer(ui::EventKind::pointer_down, p);
        pointer(ui::EventKind::pointer_up, p);
        tick(app, 1);
        const auto window = app.menu_window();
        const auto frame = app.menu_frame();
        const auto* row = frame->find(control::guide_grid);
        require(row != nullptr, "Guide checkbox is absent.");
        RECT menu_bounds{};
        require(GetWindowRect(window, &menu_bounds) != FALSE, "Guide menu bounds are unavailable.");
        POINT owner{};
        ClientToScreen(app.window(), &owner);
        const math::Vector2 indicator{float(menu_bounds.left - owner.x) + row->bounds.x +
                                          (row->style->padding.left + row->style->indicator_size * .5f) *
                                              frame->drawing->dpi_scale,
                                      float(menu_bounds.top - owner.y) + row->bounds.y + row->bounds.height * .5f};
        pointer(ui::EventKind::pointer_move, indicator);
        tick(app, 1);
        const auto highlight = app.menu_frame()->find(control::guide_grid)->style->background;
        const auto revision = app.menu_frame()->drawing->surface_revision;
        const auto incarnation = app.menu_frame()->find(control::guide_grid)->incarnation;
        for (unsigned i = 0; i < 4; ++i) {
            pointer(ui::EventKind::pointer_down, indicator);
            pointer(ui::EventKind::pointer_up, indicator);
            tick(app, 1);
            const auto updated = app.menu_frame();
            require(updated && app.menu_window() == window, "Checkbox replaced its popup.");
            const auto* checked = updated->find(control::guide_grid);
            require(checked && checked->style->background == highlight && checked->incarnation == incarnation &&
                        updated->drawing->surface_revision == revision,
                    "Checkbox rebuilt its surface or moved the highlight on its first frame.");
            for (const auto& item : updated->controls)
                if (item.kind == ui::ControlKind::menu_item && item.key != control::guide_grid)
                    require(item.style->background != highlight, "Checkbox left an unrelated menu row highlighted.");
        }
        capture(app, output / "menu-retained-highlight.png");
        escape();
        tick(app, 1);
    }
    {
        const auto p = point(control::viewport_cavity);
        pointer(ui::EventKind::pointer_down, p);
        pointer(ui::EventKind::pointer_up, p);
        tick(app, 1);
        const auto frame = app.menu_frame();
        const auto* box = frame->find(control::cavity_enabled);
        require(box != nullptr, "Cavity toggle is absent.");
        RECT bounds{};
        GetWindowRect(app.menu_window(), &bounds);
        POINT owner{};
        ClientToScreen(app.window(), &owner);
        const math::Vector2 at{float(bounds.left - owner.x) + box->bounds.x +
                                   (box->style->padding.left + box->style->indicator_size * .5f) *
                                       frame->drawing->dpi_scale,
                               float(bounds.top - owner.y) + box->bounds.y + box->bounds.height * .5f};
        pointer(ui::EventKind::pointer_move, at);
        tick(app, 1);
        const auto revision = app.menu_frame()->drawing->surface_revision;
        const auto highlight = app.menu_frame()->find(control::cavity_enabled)->style->background;
        for (unsigned i = 0; i < 2; ++i) {
            pointer(ui::EventKind::pointer_down, at);
            pointer(ui::EventKind::pointer_up, at);
            tick(app, 1);
            const auto updated = app.menu_frame();
            require(updated && updated->drawing->surface_revision == revision &&
                        updated->find(control::cavity_enabled)->style->background == highlight,
                    "Changing menu availability reset its surface or highlight.");
        }
        escape();
        tick(app, 1);
    }
    const auto original = app.presented()->find(control::viewport)->bounds;
    const auto* divider = app.presented()->find(app.parameter_splitter());
    require(divider != nullptr, "Dock divider is absent.");
    const math::Vector2 begin{divider->bounds.x + divider->bounds.width * .5f,
                              divider->bounds.y + divider->bounds.height * .2f};
    pointer(ui::EventKind::pointer_move, begin);
    pointer(ui::EventKind::pointer_down, begin);
    require(GetCapture() == app.window(), "Dock shrink did not acquire native capture on press.");
    unsigned resized_during_drag{};
    for (unsigned step = 1; step <= 8; ++step) {
        pointer(ui::EventKind::pointer_move, {begin.x - 12.f * step, begin.y});
        tick(app, 1);
        report << "dock_step," << step << ',' << app.docking().dragging() << ',' << (GetCapture() == app.window())
               << '\n';
        report.flush();
        require(app.docking().dragging() && GetCapture() == app.window(),
                "Dock shrink lost its drag or native capture before release.");
        if (app.presented()->find(control::viewport)->bounds != original)
            ++resized_during_drag;
    }
    capture(app, output / "divider-preview.png");
    pointer(ui::EventKind::pointer_up, {begin.x - 96, begin.y});
    tick(app);
    const auto committed = app.presented()->find(control::viewport)->bounds;
    require(committed.width < original.width - 40, "Dock release did not commit the requested resize.");
    report << "dock_resized_during_drag," << resized_during_drag << "\n";
    report.flush();
    capture(app, output / "divider-committed.png");
    for (unsigned reason = 0; reason < 3; ++reason) {
        const auto key = app.docking().divider(control::scene_tab, ui::DragAxis::vertical);
        const auto* rail = app.presented()->find(key);
        require(rail != nullptr, "Horizontal dock rail is absent.");
        const math::Vector2 start{rail->bounds.x + rail->bounds.width * .2f,
                                  rail->bounds.y + rail->bounds.height * .5f};
        const auto before = app.presented()->find(control::viewport)->bounds;
        pointer(ui::EventKind::pointer_move, start);
        pointer(ui::EventKind::pointer_down, start);
        pointer(ui::EventKind::pointer_move, {start.x, start.y - 45});
        tick(app, 1);
        require(app.docking().dragging() && app.presented()->find(control::viewport)->bounds == before,
                "Vertical divider did not retain its preview without resizing the viewport.");
        if (reason == 0)
            escape();
        else
            Access::input(app, {.kind = reason == 1 ? ui::EventKind::focus_lost : ui::EventKind::capture_lost});
        pointer(ui::EventKind::pointer_up, {start.x, start.y - 45});
        tick(app);
        require(!app.docking().dragging() && app.presented()->find(control::viewport)->bounds == before,
                "Cancelled divider committed a late release or retained its preview.");
    }
    {
        // Scene Tree describes rows around its last laid-out viewport. Growing the pane past that
        // margin may miss rows for one frame; the next frame lays out every visible row again.
        const auto selected = app.document().selection().entity;
        const auto members = model(app.document().world().members()).size();
        constexpr unsigned created = 120; // Stays below Session's 128 undo entries so undo removes them all.
        for (unsigned i = 0; i < created; ++i)
            require(bool(Access::invoke(app, "scene.add", {{"kind", "entity"}})), "Scene Tree population failed.");
        require(bool(Access::invoke(app, "layout.show", {{"tab", "scene_tree"}})), "Scene Tree did not open.");
        tick(app, 2);
        const auto missing = [&] {
            const auto viewport = Access::tree_viewport(app, control::entity_tree);
            std::size_t rows{};
            for (const auto& row : app.presented()->tree_rows)
                if (const auto* box = app.presented()->find(row.key); box && box->parent == control::entity_tree)
                    rows = std::max<std::size_t>(rows, row.index + 1); // The host always describes its last row.
            require(rows > created, "Scene Tree lost its populated rows.");
            const auto first = static_cast<std::size_t>(viewport.scroll / viewport.row_height);
            const auto last = std::min(
                rows - 1, static_cast<std::size_t>((viewport.scroll + viewport.height - 1) / viewport.row_height));
            std::size_t absent{};
            for (auto index = first; index <= last; ++index)
                absent += std::ranges::none_of(app.presented()->tree_rows, [&](const auto& row) {
                    const auto* box = app.presented()->find(row.key);
                    return row.index == index && box && box->parent == control::entity_tree;
                });
            return absent;
        };
        require(missing() == 0, "Docked Scene Tree omitted a visible row.");
        require(bool(Access::invoke(app, "layout.maximize")), "Scene Tree did not maximize.");
        tick(app, 1);
        report << "tree_rows_missing_first_grown_frame," << missing() << '\n';
        report.flush();
        tick(app, 1);
        require(missing() == 0, "Scene Tree did not complete its rows on the frame after growing.");
        require(bool(Access::invoke(app, "layout.maximize")), "Scene Tree did not restore.");
        tick(app, 2);
        require(missing() == 0, "Restored Scene Tree omitted a visible row.");
        for (unsigned i = 0; i < created; ++i)
            require(bool(Access::invoke(app, "edit.undo")), "Scene Tree population did not undo.");
        require(model(app.document().world().members()).size() == members,
                "Undoing Scene Tree population left Entities behind.");
        model(Access::select_entity(app, selected));
        tick(app, 1);
    }
    namespace browser = editor::browser_control;
    const auto history = app.document().undo_count();
    const auto tree_width = [&] {
        const auto* tree = app.presented()->find(browser::directories);
        require(tree != nullptr, "Asset directory tree is absent.");
        return tree->bounds.width;
    };
    const auto split = point(browser::divider);
    const auto initial_tree = tree_width();
    const auto viewport = app.presented()->find(control::viewport)->bounds;
    pointer(ui::EventKind::pointer_down, split);
    for (unsigned step = 1; step <= 4; ++step) {
        pointer(ui::EventKind::pointer_move, {split.x + step * 16.f, split.y});
        tick(app, 1);
        require(GetCapture() == app.window(), "Asset directory splitter lost capture while crossing its content.");
        require(app.presented()->find(control::viewport)->bounds == viewport,
                "Asset directory resize changed the scene viewport.");
    }
    pointer(ui::EventKind::pointer_up, {split.x + 64, split.y});
    tick(app);
    require(tree_width() > initial_tree + 40, "Asset directory splitter did not widen the tree.");
    const auto committed_tree = tree_width();
    for (unsigned reason = 0; reason < 3; ++reason) {
        const auto start = point(browser::divider);
        pointer(ui::EventKind::pointer_down, start);
        pointer(ui::EventKind::pointer_move, {start.x - 35, start.y});
        tick(app, 1);
        require(tree_width() < committed_tree - 20, "Asset directory splitter did not preview shrinking.");
        if (reason == 0)
            escape();
        else
            Access::input(app, {.kind = reason == 1 ? ui::EventKind::focus_lost : ui::EventKind::capture_lost});
        pointer(ui::EventKind::pointer_up, {start.x - 35, start.y});
        tick(app);
        require(std::abs(tree_width() - committed_tree) < 1, "Cancelled asset resize changed the saved width.");
    }
    for (const float direction : {-1.f, 1.f}) {
        const auto start = point(browser::divider);
        pointer(ui::EventKind::pointer_down, start);
        pointer(ui::EventKind::pointer_move, {start.x + direction * 10000, start.y});
        tick(app);
        const auto dpi = app.presented()->drawing->dpi_scale;
        require(tree_width() >= 96 * dpi - .5f &&
                    app.presented()->find(browser::content)->bounds.width >= 64 * dpi - .5f,
                "Asset directory resize hid the tree or result content at its limits.");
        escape();
        pointer(ui::EventKind::pointer_up, start);
        tick(app);
    }
    require(app.document().undo_count() == history, "Asset directory layout entered scene history.");
    capture(app, output / "asset-directory-width.png");
    const auto original_lighting = Access::scene_lit_view(app).lit;
    const auto menu_click = [&](ui::ControlKey key) {
        const auto at = centre(app, key);
        const math::Vector2 p{float(at.x), float(at.y)};
        pointer(ui::EventKind::pointer_down, p);
        pointer(ui::EventKind::pointer_up, p);
        tick(app, 1);
    };
    // Exercise camera lifetime through actual input and the public pipe. The
    // client thread owns only protocol data; Application stays on this thread.
    namespace api = atelier::control;
    const auto directory = output / "Control";
    app.enable_control(directory);
    const auto instances = api::discover(directory);
    const auto instance = std::ranges::find_if(
        instances, [](const auto& item) { return item.at("pid").template get<DWORD>() == GetCurrentProcessId(); });
    require(instance != instances.end(), "Camera acceptance Control endpoint is absent.");
    const auto control_reply = [&] {
        auto pending = std::async(std::launch::async, [descriptor = *instance] {
            api::Client client(descriptor, std::chrono::seconds(5));
            // Native input can advance the token between these two pipe calls.
            // Refresh only stale requests; a fresh request must still prove busy/idle.
            for (unsigned attempt = 0; attempt < 8; ++attempt) {
                const auto state = client.call({{"op", "editor.inspect"}, {"args", {{"detail", "summary"}}}});
                auto reply = client.call({{"op", "op.call"},
                                          {"state_token", state.at("state_token")},
                                          {"args", {{"id", "view.set"}, {"args", api::Json::object()}}}});
                if (reply.at("status") != "error" || reply.at("error").at("code") != "stale_state")
                    return reply;
            }
            throw std::runtime_error("Camera Control acceptance could not obtain a stable state token.");
        });
        while (pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
            require(app.step(), "Editor closed during camera Control acceptance.");
        return pending.get();
    };
    const auto camera_point = point(control::viewport);
    for (unsigned gesture = 0; gesture < 3; ++gesture)
        for (unsigned ending = 0; ending < 4; ++ending) {
            const auto button = gesture == 2 ? ui::PointerButton::primary : ui::PointerButton::middle;
            Access::input(app, {.kind = ui::EventKind::pointer_down,
                                .x = camera_point.x,
                                .y = camera_point.y,
                                .button = button,
                                .shift = gesture == 0,
                                .alt = gesture == 2});
            const auto busy = control_reply();
            if (busy.at("status") != "error" || busy.at("error").at("code") != "busy")
                throw std::runtime_error(
                    "Control lost camera gesture " + std::to_string(gesture) + "/" + std::to_string(ending) +
                    ", foreground=" + std::to_string(GetForegroundWindow() == app.window()) +
                    ", capture=" + std::to_string(GetCapture() == app.window()) + ": " + busy.dump());
            const auto& reasons = busy.at("error").at("busy_reasons");
            require(std::ranges::find(reasons, gesture == 0 ? "camera_pan" : "camera_orbit") != reasons.end() &&
                        std::ranges::find(reasons, gesture == 0 ? "camera_orbit" : "camera_pan") == reasons.end(),
                    "Control reported the wrong camera gesture.");
            Access::input(app, {.kind = ui::EventKind::pointer_up,
                                .x = camera_point.x,
                                .y = camera_point.y,
                                .button = ui::PointerButton::secondary});
            require(control_reply().at("status") == "error", "Unrelated mouse release ended a camera gesture.");
            if (ending == 1 || ending == 2)
                Access::input(app, {.kind = ending == 1 ? ui::EventKind::focus_lost : ui::EventKind::capture_lost});
            else if (ending == 3) {
                Access::input(app, {.kind = ui::EventKind::key_down, .key = ui::Key::f10}, VK_F10);
                tick(app, 1);
                const auto menu = control_reply();
                require(menu.at("status") == "error" &&
                            menu.at("error").at("busy_reasons") == api::Json::array({"menu"}),
                        "Opening a menu retained the old camera gesture or lost menu protection.");
                escape();
                tick(app, 1);
            }
            Access::input(
                app, {.kind = ui::EventKind::pointer_up, .x = camera_point.x, .y = camera_point.y, .button = button});
            const auto released = control_reply();
            if (released.at("status") != "ok")
                throw std::runtime_error("Ended camera gesture left Control busy: " + released.dump());
            const auto stopped = Access::camera_view(app);
            Access::input(app, {.kind = ui::EventKind::pointer_move, .x = camera_point.x + 8, .y = camera_point.y + 8});
            tick(app, 1);
            require(Access::camera_view(app) == stopped && GetCapture() != app.window(),
                    "Ended camera gesture resumed on late movement.");
            report << "camera," << gesture << ',' << ending << ",idle\n";
            report.flush();
        }
    require(app.document().undo_count() == history, "Camera gestures entered scene history.");
    require(bool(Access::invoke(app, "layout.show", {{"tab", "world"}})), "Lighting page command failed.");
    tick(app, 1);
    capture(app, output / "world-lighting.png");
    const auto press_tab = [&](ui::ControlKey key) {
        const auto p = point(key);
        pointer(ui::EventKind::pointer_down, p);
        pointer(ui::EventKind::pointer_up, p);
        tick(app, 1);
    };
    const auto action_control = [&](ui::ControlKey target) {
        const auto at = std::ranges::find(app.presented()->controls, target, &ui::ControlBox::action_target);
        require(at != app.presented()->controls.end(), "World action binding is absent.");
        return at->key;
    };
    {
        const auto key = action_control(editor::world_control::field(editor::ViewOption::shadows));
        const auto normal = *app.presented()->find(key)->style;
        const auto p = point(key);
        pointer(ui::EventKind::pointer_move, p);
        tick(app, 1);
        const auto unchanged = [&] {
            const auto& current = *app.presented()->find(key)->style;
            return current.background == normal.background && current.foreground == normal.foreground &&
                   current.font == normal.font && current.indicator_background == normal.indicator_background &&
                   current.indicator_border == normal.indicator_border;
        };
        require(unchanged(), "Parameter checkbox acquired a hover style.");
        pointer(ui::EventKind::pointer_down, p);
        tick(app, 1);
        require(unchanged(), "Parameter checkbox acquired a pressed style.");
        pointer(ui::EventKind::pointer_up, p);
        tick(app, 1);
        press_tab(key);
    }
    press_tab(editor::world_control::effects_tab);
    capture(app, output / "world-effects.png");
    const auto diagnostics_history = app.document().undo_count();
    press_tab(editor::world_control::diagnostics_tab);
    capture(app, output / "world-diagnostics.png");
    const auto sphere_field = editor::world_control::field(editor::ViewOption::probe_visualization);
    const auto sphere_selector =
        std::ranges::find(app.presented()->controls, sphere_field, &ui::ControlBox::context_target);
    require(sphere_selector != app.presented()->controls.end(), "World probe selector is absent.");
    menu_click(sphere_selector->key);
    capture(app, output / "ddgi-probe-menu.png");
    menu_click(editor::world_control::choice(editor::ViewOption::probe_visualization, 2));
    require(!Access::menu_open(app) &&
                Access::scene_lit_view(app).lit.probe_visualization == renderer::ProbeVisualization::state,
            "World probe selector did not apply its radio selection.");
    menu_click(action_control(editor::world_control::field(editor::ViewOption::probe_volume)));
    require(Access::scene_lit_view(app).lit.probe_volume != original_lighting.probe_volume,
            "World volume diagnostic did not apply its checkbox.");
    require(app.document().undo_count() == diagnostics_history, "World diagnostics entered scene history.");
    model(Access::scene_lit_settings(app, original_lighting));
    tick(app, 1);
    press_tab(editor::world_control::ddgi_tab);
    capture(app, output / "world-ddgi.png");
    const auto* slot_button = app.presented()->find(editor::world_control::slot);
    require(slot_button && slot_button->style->combo_indicator == ui::ComboIndicator::down &&
                slot_button->bounds.height / app.presented()->drawing->dpi_scale >= 26.6f,
            "Parameter menu button lost its height or single dropdown arrow.");
    menu_click(editor::world_control::setup);
    require(app.menu_frame() != nullptr, "World Coverage menu did not open.");
    for (const auto& item : app.menu_frame()->controls)
        if (item.kind == ui::ControlKind::menu_item)
            require(item.menu_behavior == ui::MenuBehavior::command && !item.radio_group,
                    "An ordinary command was rendered as a toggle/radio choice.");
    capture(app, output / "world-coverage-actions.png");
    escape();
    tick(app, 1);
    const auto world_history = app.document().undo_count();
    const auto count = editor::world_control::field(editor::probe_volume_option(0, editor::ProbeVolumeField::count_x));
    const auto lighting_before = Access::scene_lit_view(app);
    const auto type_number = [&](std::wstring value) {
        const auto p = point(count);
        pointer(ui::EventKind::pointer_down, p);
        pointer(ui::EventKind::pointer_up, p);
        Access::input(app, {.kind = ui::EventKind::key_down, .key = ui::Key::a, .control = true}, 'A');
        Access::input(app, {.kind = ui::EventKind::key_up, .key = ui::Key::a, .control = true}, 'A');
        Access::input(app, {.kind = ui::EventKind::text, .data = std::move(value)});
        tick(app, 1);
    };
    type_number(L"10");
    require(Access::scene_lit_view(app) == lighting_before, "Lighting text preview rebuilt the probe configuration.");
    const auto pending_edit = control_reply();
    require(pending_edit.at("status") == "error" &&
                std::ranges::find(pending_edit.at("error").at("busy_reasons"), "view_edit") !=
                    pending_edit.at("error").at("busy_reasons").end(),
            "Control did not protect the uncommitted Lighting edit.");
    escape();
    tick(app, 1);
    require(Access::scene_lit_view(app) == lighting_before && control_reply().at("status") == "ok",
            "Cancelled Lighting draft retained settings or busy state.");
    type_number(L"10");
    Access::input(app, {.kind = ui::EventKind::key_down, .key = ui::Key::enter}, VK_RETURN);
    Access::input(app, {.kind = ui::EventKind::key_up, .key = ui::Key::enter}, VK_RETURN);
    tick(app, 1);
    require(Access::scene_lit_view(app).lit.probe_volumes[0].counts[0] == 10 && control_reply().at("status") == "ok",
            "Lighting Enter commit failed to publish or left an active draft.");
    type_number(L"12");
    Access::input(app, {.kind = ui::EventKind::key_down, .control = true}, 'W');
    Access::input(app, {.kind = ui::EventKind::key_up, .control = true}, 'W');
    tick(app, 1);
    require(Access::scene_lit_view(app).lit.probe_volumes[0].counts[0] == 10 && control_reply().at("status") == "ok",
            "Closing Lighting committed a pending number or kept Control busy.");
    require(bool(Access::invoke(app, "layout.show", {{"tab", "world"}})), "Lighting page did not reopen.");
    tick(app, 1);
    const auto slot = point(editor::world_control::slot);
    pointer(ui::EventKind::pointer_down, slot);
    pointer(ui::EventKind::pointer_up, slot);
    tick(app, 1);
    require(app.menu_frame() != nullptr, "Lighting volume selector did not open its radio menu.");
    capture(app, output / "lighting-volumes.png");
    menu_click(editor::world_control::slot_first + 1);
    require(!app.presented()->find(count) && app.presented()->find(editor::world_control::field(
                                                 editor::probe_volume_option(1, editor::ProbeVolumeField::count_x))),
            "Changing the volume did not replace the DSL row bindings.");
    require(app.document().undo_count() == world_history + 1,
            "World controls did not commit exactly one scene history entry.");
    report << "lighting,preview-cancel-commit-close-slot-switch,passed\n";
    report.flush();
    require(bool(Access::invoke(app, "layout.show", {{"tab", "parameters"}})), "Parameters did not reopen.");
    tick(app, 1);
    const auto entity = app.document().selection().entity;
    const auto position_key = property_control(app, entity, 0);
    press_tab(control::group_categories);
    capture(app, output / "parameters-category.png");
    require(property_control(app, entity, 0) == position_key && app.presented()->find(position_key),
            "Category grouping hid the primary transform or changed field identity.");
    press_tab(control::group_components);
    capture(app, output / "parameters-component.png");
    require(property_control(app, entity, 0) == position_key, "Component grouping changed field identity.");
    verify_render_performance(app, output);
    // A real pre-encoding Renderer validation failure must leave host services
    // available without preparing/retrying another frame on each pipe wakeup.
    const auto radius = Access::scene_lit_view(app).cavity.radius;
    const auto accepted = app.accepted_frames();
    Access::unchecked_cavity_radius(app, .25f);
    for (unsigned attempt = 0; attempt < 7; ++attempt) {
        Access::render_retry_deadline(app, {});
        require(app.step() && app.accepted_frames() == accepted,
                "Recoverable render validation failure closed or accepted a frame.");
        require(Access::render_retry_delay(app) > std::chrono::milliseconds(0) &&
                    Access::render_retry_delay(app) <= std::chrono::milliseconds(250),
                "Render retry delay escaped its bound.");
    }
    const auto held_delay = Access::render_retry_delay(app);
    Access::render_retry_deadline(app, std::chrono::steady_clock::now() + std::chrono::seconds(20));
    auto inspection = std::async(std::launch::async, [descriptor = *instance] {
        api::Client client(descriptor, std::chrono::seconds(5));
        return client.call({{"op", "editor.inspect"}, {"args", {{"detail", "summary"}}}});
    });
    while (inspection.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
        require(app.step(), "Render retry blocked the Control inspection service.");
    require(inspection.get().at("status") == "ok" && app.accepted_frames() == accepted &&
                Access::render_retry_delay(app) == held_delay,
            "Host service processing bypassed the render retry gate.");
    // Schedule one genuine failure deadline after the forced service hold, then
    // leave that deadline untouched while the repaired input recovers naturally.
    Access::render_retry_deadline(app, {});
    require(app.step() && app.accepted_frames() == accepted, "Render validation did not schedule a fresh retry.");
    const auto recovery_deadline = Access::render_retry_deadline(app);
    Access::unchecked_cavity_radius(app, radius);
    while (app.accepted_frames() == accepted &&
           std::chrono::steady_clock::now() < recovery_deadline + std::chrono::seconds(10))
        require(app.step(), "Editor closed while waiting for natural render retry recovery.");
    require(app.accepted_frames() > accepted, "Rendering did not recover after its natural retry deadline.");
    require(std::chrono::steady_clock::now() >= recovery_deadline, "Rendering recovered before its retry deadline.");
    require(Access::render_retry_delay(app) == std::chrono::milliseconds(0),
            "Successful rendering retained failure backoff.");
    Access::unchecked_cavity_radius(app, .25f);
    require(app.step(), "Second recoverable render error became terminal.");
    Access::mark_saved(app);
    PostMessageW(app.window(), WM_CLOSE, 0, 0);
    require(!app.step(), "A native close request could not pass the render retry gate.");
    report << "render-retry,validation-control-recovery-close,passed\n";
    report.flush();
    std::printf(
        "Editor surfaces: drag resized %u/8 frames; redundant popup presentations %u/5 warm opens; external mouse messages dropped %u\n",
        resized_during_drag, redundant_presents, ExternalMouseFilter::discarded);
    require(resized_during_drag == 0, "Dock dragging resized the live viewport before release.");
    require(redundant_presents == 0, "Popup resize presented an obsolete canvas before its accepted frame.");
    std::puts(
        "Editor surfaces acceptance: PASS (retained menu highlight/availability, no leading separator, parameter states/button metrics, World diagnostics and edits/lifetime, deferred dock geometry, resizable asset directories, camera Control lifetime, both axes, Escape/focus/capture loss, late release)");
    return 0;
}
