void verify_viewport_navigation(Application& app, const std::filesystem::path& output) {
    using namespace atelier;
    namespace control = atelier::editor::control;
    using Access = application::EditorAcceptanceAccess;
    const auto viewport = app.presented()->find(control::viewport)->bounds;
    const math::Vector2 origin{viewport.x + viewport.width * .5f, viewport.y + viewport.height * .5f};
    const auto pointer = [&](ui::EventKind kind, math::Vector2 point) {
        Access::input(app, {.kind = kind, .x = point.x, .y = point.y});
    };
    const auto key = [&](WPARAM code, bool up = false, bool ctrl = false, ui::Key mapped = ui::Key::none) {
        Access::input(
            app, {.kind = up ? ui::EventKind::key_up : ui::EventKind::key_down, .key = mapped, .control = ctrl}, code);
    };
    const auto click = [&](math::Vector2 point) {
        pointer(ui::EventKind::pointer_move, point);
        pointer(ui::EventKind::pointer_down, point);
        pointer(ui::EventKind::pointer_up, point);
        tick(app, 3);
    };
    const auto centre = [&](ui::ControlKey key) {
        const auto* box = app.presented()->find(key);
        require(box != nullptr, "Viewport navigation control is absent.");
        return math::Vector2{box->bounds.x + box->bounds.width * .5f, box->bounds.y + box->bounds.height * .5f};
    };
    const auto history = app.document().undo_count();
    const auto cursor = Access::cursor_position(app);
    if (GetForegroundWindow() != app.window()) {
        RECT clip_before{};
        RECT clip_after{};
        require(GetClipCursor(&clip_before), "Cannot inspect background input ownership.");
        const auto captured = GetCapture();
        const auto camera = Access::camera_view(app);
        pointer(ui::EventKind::pointer_move, origin);
        Access::input(app, {.kind = ui::EventKind::key_down, .shift = true}, VK_OEM_3);
        Access::input(app, {.kind = ui::EventKind::key_up, .shift = true}, VK_OEM_3);
        require(GetClipCursor(&clip_after) && EqualRect(&clip_before, &clip_after) && GetCapture() == captured &&
                    Access::camera_view(app) == camera,
                "Background fly request changed desktop input ownership or the camera.");
    }
    pointer(ui::EventKind::pointer_move, origin);
    key(VK_OEM_3);
    key(VK_OEM_3, true);
    tick(app, 3);
    require(Access::menu_open(app) && app.presented()->popup == control::viewport_pie,
            "Accent-grave did not open the viewport axis pie.");
    capture(app, output / "view-axis-pie.png");
    click(centre(Access::operator_item(app, "view.axis", {{"view", "perspective"}})));
    require(!Access::menu_open(app) && Access::camera_mode(app) == editor::CameraView::perspective,
            "Axis pie selected the wrong camera view or retained the menu.");
    pointer(ui::EventKind::pointer_move, origin);
    key(VK_OEM_3);
    tick(app, 2);
    key(VK_ESCAPE, false, false, ui::Key::escape);
    key(VK_ESCAPE, true, false, ui::Key::escape);
    tick(app, 2);
    require(!Access::menu_open(app), "Axis pie Escape retained modal state.");
    require(bool(Access::invoke(app, "view.reset")), "Camera reset failed.");
    tick(app, 2);
    const auto menu_point = [&](ui::ControlKey key, bool indicator) {
        const auto frame = app.menu_frame();
        require(frame != nullptr, "Native checkbox menu is absent.");
        const auto* box = frame->find(key);
        require(box != nullptr, "Native checkbox menu item is absent.");
        RECT rect{};
        require(GetWindowRect(app.menu_window(), &rect) != FALSE, "Cannot inspect native checkbox menu bounds.");
        POINT host{};
        ClientToScreen(app.window(), &host);
        const auto dpi = frame->drawing->dpi_scale;
        return math::Vector2{float(rect.left - host.x) + box->bounds.x +
                                 (box->style->padding.left +
                                  (indicator ? box->style->indicator_size * .5f : box->style->indicator_size + 12)) *
                                     dpi,
                             float(rect.top - host.y) + box->bounds.y + box->bounds.height * .5f};
    };
    const auto grid = Access::scene_lit_view(app).reference_grid;
    click(centre(control::scene_guides));
    const auto popup = app.menu_window();
    for (unsigned i = 0; i < 2; ++i) {
        click(menu_point(control::guide_grid, true));
        require(Access::menu_open(app) && app.menu_window() == popup &&
                    Access::scene_lit_view(app).reference_grid == (i ? grid : !grid),
                "Indicator click dismissed/recreated the menu or failed to toggle.");
        const auto* checked = app.menu_frame()->find(control::guide_grid);
        require(checked && checked->selected == Access::scene_lit_view(app).reference_grid,
                "Retained checkbox did not publish its new checked state.");
        capture(app, output / (i ? "menu-checkbox-restored.png" : "menu-checkbox-toggled.png"));
    }
    click(menu_point(control::guide_grid, false));
    require(!Access::menu_open(app) && Access::scene_lit_view(app).reference_grid != grid,
            "Text click did not toggle and dismiss the menu.");
    click(centre(control::scene_guides));
    click(menu_point(control::guide_grid, false));
    require(Access::scene_lit_view(app).reference_grid == grid,
            "Checkbox cleanup did not restore the view preference.");
    require(app.document().undo_count() == history && Access::cursor_position(app) == cursor,
            "View/menu gestures changed scene history or placed the 3D cursor.");
    std::puts("Viewport navigation acceptance: PASS (axis pie, retained checkbox, history/cursor isolation)");
}

// Only the foreground editor window acceptance may exercise global raw input and clipping.
void verify_viewport_fly(Application& app, const std::filesystem::path& output) {
    try {
        using namespace atelier;
        namespace control = atelier::editor::control;
        using Access = application::EditorAcceptanceAccess;
        const auto snapshot = [&](const char* name) {
            const auto image = capture(app);
            png(image, output / name); // Navigation also works in an empty scene.
        };
        const auto require_foreground = [&] {
            require(GetForegroundWindow() == app.window() && GetFocus() == app.window(),
                    "Fly acceptance lost foreground/focus ownership; no desktop input was sent.");
        };
        require_foreground();
        const auto viewport = app.presented()->find(control::viewport)->bounds;
        const math::Vector2 origin{viewport.x + viewport.width * .5f, viewport.y + viewport.height * .5f};
        const auto pointer = [&](ui::EventKind kind, math::Vector2 point) {
            Access::input(app, {.kind = kind, .x = point.x, .y = point.y});
        };
        const auto key = [&](WPARAM code, bool up = false, bool ctrl = false, ui::Key mapped = ui::Key::none) {
            Access::input(
                app, {.kind = up ? ui::EventKind::key_up : ui::EventKind::key_down, .key = mapped, .control = ctrl},
                code);
        };
        const auto history = app.document().undo_count();
        const auto cursor = Access::cursor_position(app);
        require(bool(Access::invoke(app, "view.reset")), "Fly camera setup failed.");
        tick(app, 2);
        const auto visible_coverage = [](const auto& coverage) {
            return coverage && coverage->width && coverage->height &&
                   std::ranges::any_of(coverage->pixels, [](auto alpha) { return alpha != 0; });
        };
        const auto verify_fly_ui = [&](bool flying) {
            const auto& frame = *app.presented();
            const auto* mode = frame.find(control::viewport_hud);
            const auto* hint = frame.find(control::viewport_hint);
            const auto* reticle = frame.find(control::viewport_fly_reticle);
            require(mode && hint && !mode->enabled && !hint->enabled,
                    "Passive navigation mode/hint was not published.");
            if (flying) {
                const auto bounds = frame.find(control::viewport)->bounds;
                require(
                    reticle && !reticle->enabled &&
                        visible_coverage(text_coverage(frame, control::viewport_fly_reticle)) &&
                        std::abs(reticle->bounds.x + reticle->bounds.width * .5f - bounds.x - bounds.width * .5f) < 1 &&
                        std::abs(reticle->bounds.y + reticle->bounds.height * .5f - bounds.y - bounds.height * .5f) < 1,
                    "Fly reticle is absent, interactive or not centered in the viewport.");
            } else
                require(!reticle, "Exiting fly retained its reticle.");
        };
        const auto begin_fly = [&] {
            require_foreground();
            pointer(ui::EventKind::pointer_move, origin);
            Access::input(app, {.kind = ui::EventKind::key_down, .shift = true}, VK_OEM_3);
            Access::input(app, {.kind = ui::EventKind::key_up, .shift = true}, VK_OEM_3);
            Access::input(app, {.kind = ui::EventKind::key_up}, VK_SHIFT);
            require(GetCapture() == app.window() && !Access::menu_open(app),
                    "Shift+accent-grave did not capture fly navigation.");
            tick(app, 2);
            if (!Access::camera_flying(app))
                throw std::runtime_error("Fly lost ownership on its entry frames: " +
                                         atelier::application::diagnostic_text(app.status_text()));
            snapshot("viewport-fly-entry.png");
            verify_fly_ui(true);
        };
        const auto move_fly = [&](WPARAM code) {
            key(code);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            tick(app, 2);
            key(code, true);
        };
        const auto fly_entry = Access::camera_view(app);
        const auto fly_eye = Access::camera_eye(app);
        begin_fly();
        // Native relative mouse input must look around without orbiting the eye.
        INPUT motion{};
        motion.type = INPUT_MOUSE;
        motion.mi.dx = 35;
        motion.mi.dy = 12;
        motion.mi.dwFlags = MOUSEEVENTF_MOVE;
        require_foreground();
        require(GetCapture() == app.window(), "Fly acceptance lost capture before native mouse input.");
        require(SendInput(1, &motion, sizeof(motion)) == 1, "Cannot inject native relative fly mouse input.");
        tick(app, 3);
        const auto looked_view = Access::camera_view(app);
        const auto eye_displacement = math::length(Access::camera_eye(app) - fly_eye);
        if (looked_view == fly_entry || !(eye_displacement < .001f)) {
            float view_difference{};
            for (std::size_t i = 0; i < looked_view.elements.size(); ++i)
                view_difference = std::max(view_difference, std::abs(looked_view[i] - fly_entry[i]));
            const auto differences =
                std::format("view_max_abs_delta={:.9g}; eye_displacement={:.9g}", view_difference, eye_displacement);
            require(looked_view != fly_entry, ("Fly mouse look did not change the view. " + differences).c_str());
            require(eye_displacement < .001f, ("Fly mouse look moved the eye. " + differences).c_str());
        }
        snapshot("viewport-fly.png");
        const auto before = Access::camera_eye(app);
        move_fly('W');
        require(math::length(Access::camera_eye(app) - before) > .001f, "Fly direction key did not move the camera.");
        key(VK_ESCAPE, false, false, ui::Key::escape);
        tick(app, 2);
        require(GetCapture() != app.window() && Access::camera_view(app) == fly_entry,
                "Fly Escape did not restore its exact entry camera.");
        verify_fly_ui(false);
        begin_fly();
        move_fly('W');
        const auto confirmed = Access::camera_view(app);
        key(VK_RETURN, false, false, ui::Key::enter);
        tick(app, 2);
        require(GetCapture() != app.window() && Access::camera_view(app) == confirmed,
                "Fly Enter discarded the accepted camera.");
        verify_fly_ui(false);
        for (unsigned ending = 0; ending < 5; ++ending) {
            const auto entry = Access::camera_view(app);
            begin_fly();
            move_fly('E');
            if (ending < 2)
                Access::input(app, {.kind = ending == 0 ? ui::EventKind::focus_lost : ui::EventKind::capture_lost});
            else if (ending == 2)
                ReleaseCapture();
            else if (ending == 3) {
                Access::input(app, {.kind = ui::EventKind::pointer_down,
                                    .x = origin.x,
                                    .y = origin.y,
                                    .button = ui::PointerButton::secondary});
                Access::input(app, {.kind = ui::EventKind::pointer_up,
                                    .x = origin.x,
                                    .y = origin.y,
                                    .button = ui::PointerButton::secondary});
            } else {
                Access::input(app, {.kind = ui::EventKind::key_down, .key = ui::Key::f10}, VK_F10);
                tick(app, 2);
                require(Access::menu_open(app), "F10 did not transfer fly ownership to the menu.");
                key(VK_ESCAPE, false, false, ui::Key::escape);
            }
            tick(app, 2);
            require(GetCapture() != app.window() && Access::camera_view(app) == entry,
                    "Fly interruption retained capture or failed to restore camera.");
            verify_fly_ui(false);
            key('W', true);
            pointer(ui::EventKind::pointer_move, origin);
            tick(app, 1);
            require(Access::camera_view(app) == entry, "Late fly input restarted cancelled movement.");
        }
        begin_fly();
        move_fly('D');
        const auto clicked = Access::camera_view(app);
        Access::input(app, {.kind = ui::EventKind::pointer_down, .x = origin.x, .y = origin.y});
        Access::input(app, {.kind = ui::EventKind::pointer_up, .x = origin.x, .y = origin.y});
        tick(app, 2);
        require(GetCapture() != app.window(), "Fly left-click confirmation did not release capture.");
        require(Access::camera_view(app) == clicked, "Fly left-click confirmation changed the view.");
        require(Access::cursor_position(app) == cursor, "Fly left-click confirmation placed the scene cursor.");
        require(bool(Access::invoke(app, "view.reset")), "Post-fly camera reset failed.");
        tick(app, 2);
        require(app.document().undo_count() == history && Access::cursor_position(app) == cursor,
                "Fly navigation changed scene history or placed the 3D cursor.");
        std::puts(
            "Viewport fly acceptance: PASS (native raw mouse, movement, modal lifecycle and centered passive reticle)");
    } catch (const std::exception& error) {
        throw std::runtime_error(error.what());
    }
}
