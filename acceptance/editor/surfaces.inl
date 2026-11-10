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
    const auto output_text = platform::utf8_to_utf16(output_directory);
    require(bool(output_text), "Invalid UTF-8 evidence directory.");
    const auto output = std::filesystem::absolute(*output_text);
    std::filesystem::create_directories(output);
    tick(app);
    require(bool(Access::invoke(app, "scene.add", json::Document::literal<R"({"kind":"mesh"})">())),
            "Surface scene creation failed.");
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
    HWND retained{};
    for (unsigned run = 0; run < 2; ++run) {
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
        for (unsigned i = 0; i < 2; ++i) {
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
            require(bool(Access::invoke(app, "scene.add", json::Document::literal<R"({"kind":"entity"})">())),
                    "Scene Tree population failed.");
        require(bool(Access::invoke(app, "layout.show", json::Document::literal<R"({"tab":"scene_tree"})">())),
                "Scene Tree did not open.");
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
        static_cast<void>(missing());
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
    model(app.enable_control(directory));
    const auto instances = api::discover(directory);
    require(bool(instances), instances ? "" : instances.error().what());
    std::optional<json::Document> instance;
    const auto descriptors = instances->root().array();
    require(descriptors.has_value(), "Control discovery did not return an array.");
    for (auto item : *descriptors)
        if (item.u32({"pid"}) == GetCurrentProcessId())
            instance.emplace(item);
    require(instance.has_value(), "Camera acceptance Control endpoint is absent.");
    const auto control_reply = [&] {
        auto pending = std::async(std::launch::async, [descriptor = *instance] {
            auto client = api::Client::create(descriptor, std::chrono::seconds(5));
            require(bool(client), client ? "" : client.error().what());
            // Native input can advance the token between these two pipe calls.
            // Refresh only stale requests; a fresh request must still prove busy/idle.
            for (unsigned attempt = 0; attempt < 8; ++attempt) {
                const auto state =
                    client->call(json::Document::literal<R"({"op":"editor.inspect","args":{"detail":"summary"}})">());
                require(bool(state), state ? "" : state.error().what());
                json::Builder request{
                    json::Document::literal<R"({"op":"op.call","args":{"id":"view.set","args":{}}})">()};
                const auto token = state->root().string({"state_token"});
                require(token && request.set(request.root(), "state_token", *token), "Missing inspection state token.");
                auto reply = client->call(request.finish());
                require(bool(reply), reply ? "" : reply.error().what());
                const auto status = reply->root().string({"status"});
                require(bool(status), "Missing Control reply status.");
                if (*status != "error")
                    return std::move(*reply);
                const auto code = reply->root().string({"error", "code"});
                require(bool(code), "Missing Control error code.");
                if (*code != "stale_state")
                    return std::move(*reply);
            }
            throw std::runtime_error("Camera Control acceptance could not obtain a stable state token.");
        });
        while (pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
            require(take(app.step()), "Editor closed during camera Control acceptance.");
        return pending.get();
    };
    const auto camera_point = point(control::viewport);
    for (unsigned ending = 0; ending < 4; ++ending) {
        Access::input(app, {.kind = ui::EventKind::pointer_down,
                            .x = camera_point.x,
                            .y = camera_point.y,
                            .button = ui::PointerButton::middle,
                            .shift = true});
        const auto busy = control_reply();
        if (busy.root().string({"status"}) != "error" || busy.root().string({"error", "code"}) != "busy") {
            const auto bytes = json::write(busy.root());
            throw std::runtime_error("Control lost camera pan ending " + std::to_string(ending) +
                                     ", foreground=" + std::to_string(GetForegroundWindow() == app.window()) +
                                     ", capture=" + std::to_string(GetCapture() == app.window()) + ": " +
                                     std::string(bytes.data(), bytes.size()));
        }
        const auto reasons = busy.root().path({"error", "busy_reasons"})->array();
        require(reasons.has_value(), "Busy reasons must be an array.");
        bool pan = false;
        bool orbit = false;
        for (auto reason : *reasons) {
            pan |= reason.string() == "camera_pan";
            orbit |= reason.string() == "camera_orbit";
        }
        require(pan && !orbit, "Control reported the wrong camera gesture.");
        Access::input(app, {.kind = ui::EventKind::pointer_up,
                            .x = camera_point.x,
                            .y = camera_point.y,
                            .button = ui::PointerButton::secondary});
        const auto unrelated = control_reply();
        require(unrelated.root().string({"status"}) == "error", "Unrelated mouse release ended a camera gesture.");
        if (ending == 1 || ending == 2)
            Access::input(app, {.kind = ending == 1 ? ui::EventKind::focus_lost : ui::EventKind::capture_lost});
        else if (ending == 3) {
            Access::input(app, {.kind = ui::EventKind::key_down, .key = ui::Key::f10}, VK_F10);
            tick(app, 1);
            const auto menu = control_reply();
            require(menu.root().string({"status"}) == "error" &&
                        *menu.root().path({"error", "busy_reasons"}) == json::Document::literal<R"(["menu"])">().root(),
                    "Opening a menu retained the old camera gesture or lost menu protection.");
            escape();
            tick(app, 1);
        }
        Access::input(app, {.kind = ui::EventKind::pointer_up,
                            .x = camera_point.x,
                            .y = camera_point.y,
                            .button = ui::PointerButton::middle});
        const auto released = control_reply();
        if (released.root().string({"status"}) != "ok") {
            const auto bytes = json::write(released.root());
            throw std::runtime_error("Ended camera gesture left Control busy: " +
                                     std::string(bytes.data(), bytes.size()));
        }
        const auto stopped = Access::camera_view(app);
        Access::input(app, {.kind = ui::EventKind::pointer_move, .x = camera_point.x + 8, .y = camera_point.y + 8});
        tick(app, 1);
        require(Access::camera_view(app) == stopped && GetCapture() != app.window(),
                "Ended camera gesture resumed on late movement.");
    }
    require(app.document().undo_count() == history, "Camera gestures entered scene history.");
    require(bool(Access::invoke(app, "layout.show", json::Document::literal<R"({"tab":"world"})">())),
            "Lighting page command failed.");
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
    const auto edit_reasons = pending_edit.root().path({"error", "busy_reasons"})->array();
    require(edit_reasons.has_value(), "Edit busy reasons must be an array.");
    bool view_edit = false;
    for (auto reason : *edit_reasons)
        view_edit |= reason.string() == "view_edit";
    require(pending_edit.root().string({"status"}) == "error" && view_edit,
            "Control did not protect the uncommitted Lighting edit.");
    escape();
    tick(app, 1);
    const auto cancelled_edit = control_reply();
    require(Access::scene_lit_view(app) == lighting_before && cancelled_edit.root().string({"status"}) == "ok",
            "Cancelled Lighting draft retained settings or busy state.");
    type_number(L"10");
    Access::input(app, {.kind = ui::EventKind::key_down, .key = ui::Key::enter}, VK_RETURN);
    Access::input(app, {.kind = ui::EventKind::key_up, .key = ui::Key::enter}, VK_RETURN);
    tick(app, 1);
    const auto committed_edit = control_reply();
    require(Access::scene_lit_view(app).lit.probe_volumes[0].counts[0] == 10 &&
                committed_edit.root().string({"status"}) == "ok",
            "Lighting Enter commit failed to publish or left an active draft.");
    type_number(L"12");
    Access::input(app, {.kind = ui::EventKind::key_down, .control = true}, 'W');
    Access::input(app, {.kind = ui::EventKind::key_up, .control = true}, 'W');
    tick(app, 1);
    const auto closed_edit = control_reply();
    require(Access::scene_lit_view(app).lit.probe_volumes[0].counts[0] == 10 &&
                closed_edit.root().string({"status"}) == "ok",
            "Closing Lighting committed a pending number or kept Control busy.");
    require(bool(Access::invoke(app, "layout.show", json::Document::literal<R"({"tab":"world"})">())),
            "Lighting page did not reopen.");
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
    require(bool(Access::invoke(app, "layout.show", json::Document::literal<R"({"tab":"parameters"})">())),
            "Parameters did not reopen.");
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
    // A real pre-encoding Renderer validation failure must leave host services
    // available without preparing/retrying another frame on each pipe wakeup.
    const auto radius = Access::scene_lit_view(app).cavity.radius;
    const auto accepted = app.accepted_frames();
    Access::unchecked_cavity_radius(app, .25f);
    for (unsigned attempt = 0; attempt < 7; ++attempt) {
        Access::render_retry_deadline(app, {});
        require(take(app.step()) && app.accepted_frames() == accepted,
                "Recoverable render validation failure closed or accepted a frame.");
        require(Access::render_retry_delay(app) > std::chrono::milliseconds(0) &&
                    Access::render_retry_delay(app) <= std::chrono::milliseconds(250),
                "Render retry delay escaped its bound.");
    }
    const auto held_delay = Access::render_retry_delay(app);
    Access::render_retry_deadline(app, std::chrono::steady_clock::now() + std::chrono::seconds(20));
    auto inspection = std::async(std::launch::async, [descriptor = *instance] {
        auto client = api::Client::create(descriptor, std::chrono::seconds(5));
        require(bool(client), client ? "" : client.error().what());
        return client->call(json::Document::literal<R"({"op":"editor.inspect","args":{"detail":"summary"}})">());
    });
    while (inspection.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
        require(take(app.step()), "Render retry blocked the Control inspection service.");
    const auto inspected = inspection.get();
    require(inspected && inspected->root().string({"status"}) == "ok" && app.accepted_frames() == accepted &&
                Access::render_retry_delay(app) == held_delay,
            "Host service processing bypassed the render retry gate.");
    // Schedule one genuine failure deadline after the forced service hold, then
    // leave that deadline untouched while the repaired input recovers naturally.
    Access::render_retry_deadline(app, {});
    require(take(app.step()) && app.accepted_frames() == accepted, "Render validation did not schedule a fresh retry.");
    const auto recovery_deadline = Access::render_retry_deadline(app);
    Access::unchecked_cavity_radius(app, radius);
    while (app.accepted_frames() == accepted &&
           std::chrono::steady_clock::now() < recovery_deadline + std::chrono::seconds(10))
        require(take(app.step()), "Editor closed while waiting for natural render retry recovery.");
    require(app.accepted_frames() > accepted, "Rendering did not recover after its natural retry deadline.");
    require(std::chrono::steady_clock::now() >= recovery_deadline, "Rendering recovered before its retry deadline.");
    require(Access::render_retry_delay(app) == std::chrono::milliseconds(0),
            "Successful rendering retained failure backoff.");
    Access::unchecked_cavity_radius(app, .25f);
    require(take(app.step()), "Second recoverable render error became terminal.");
    Access::mark_saved(app);
    PostMessageW(app.window(), WM_CLOSE, 0, 0);
    require(!take(app.step()), "A native close request could not pass the render retry gate.");
    std::printf("Editor surfaces: drag resized %u/8 frames; external mouse messages dropped %u\n", resized_during_drag,
                ExternalMouseFilter::discarded);
    require(resized_during_drag == 0, "Dock dragging resized the live viewport before release.");
    std::puts(
        "Editor surfaces acceptance: PASS (retained menu, DPI remeasurement, World edits/lifetime, deferred dock geometry, resizable asset directories, camera Control lifetime, Escape/focus/capture loss, late release)");
    return 0;
}
