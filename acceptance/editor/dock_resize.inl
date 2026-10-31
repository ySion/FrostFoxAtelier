void verify_viewport_splitter_drag(Application& app, const std::filesystem::path& output) {
    using namespace atelier;
    namespace control = atelier::editor::control;
    using Access = application::EditorAcceptanceAccess;
    // OS mouse messages are dropped, so the gestures follow only these synthetic points,
    // wherever the window and the physical cursor are.
    std::optional<ExternalMouseFilter> external_mouse{std::in_place};
    const unsigned discarded_before = ExternalMouseFilter::discarded;
    const auto pointer = [&](ui::EventKind kind, math::Vector2 p) {
        Access::input(app, {.kind = kind, .x = p.x, .y = p.y, .button = ui::PointerButton::primary});
    };
    std::ofstream report(output / "splitter-drag.csv");
    report << "axis,direction,step,captured,width,height\n";
    for (const auto axis : {ui::DragAxis::horizontal, ui::DragAxis::vertical})
        for (const float direction : {-1.f, 1.f}) {
            const auto before = app.presented()->find(control::viewport)->bounds;
            const auto key = app.docking().divider(control::scene_tab, axis);
            const auto* divider = app.presented()->find(key);
            require(divider != nullptr, "Viewport resize divider is absent.");
            const bool horizontal = axis == ui::DragAxis::horizontal;
            const math::Vector2 start =
                horizontal
                    ? math::Vector2{divider->bounds.x + divider->bounds.width * .5f, before.y + before.height * .35f}
                    : math::Vector2{before.x + before.width * .3f, divider->bounds.y + divider->bounds.height * .5f};
            pointer(ui::EventKind::pointer_move, start);
            tick(app, 2);
            pointer(ui::EventKind::pointer_down, start);
            require(GetCapture() == app.window(), "Viewport splitter did not acquire native pointer capture.");
            const auto delta = std::min(96.f, (horizontal ? before.width : before.height) * .2f);
            math::Vector2 end = start;
            for (unsigned step = 1; step <= 8; ++step) {
                end = {start.x + (horizontal ? direction * delta * step / 8 : 0),
                       start.y + (horizontal ? 0 : direction * delta * step / 8)};
                pointer(ui::EventKind::pointer_move, end);
                tick(app, 1);
                const auto view = app.presented()->find(control::viewport)->bounds;
                report << horizontal << ',' << direction << ',' << step << ',' << (GetCapture() == app.window()) << ','
                       << view.width << ',' << view.height << '\n';
                report.flush();
                require(GetCapture() == app.window(),
                        "Viewport splitter lost pointer capture while moving across the viewport.");
                require(app.docking().dragging() && view == before,
                        "Viewport splitter resized live geometry or lost its preview before release.");
            }
            pointer(ui::EventKind::pointer_up, end);
            tick(app, 2);
            const auto after = app.presented()->find(control::viewport)->bounds;
            report << horizontal << ',' << direction << ",commit," << (GetCapture() == app.window()) << ','
                   << after.width << ',' << after.height << '\n';
            report.flush();
            require(GetCapture() != app.window(), "Viewport splitter retained capture after release.");
            const auto moved = horizontal ? end.x - start.x : end.y - start.y;
            require(std::abs((horizontal ? after.width - before.width : after.height - before.height) - moved) <= 1.f,
                    "Viewport splitter failed to commit its full shrink/expand gesture.");
        }
    {
        // Travel past the divider's limit is held: the preview waits at the limit and returns under
        // the pointer, and the release commits where the pointer is.
        const auto before = app.presented()->find(control::viewport)->bounds;
        const auto* divider =
            app.presented()->find(app.docking().divider(control::scene_tab, ui::DragAxis::horizontal));
        require(divider != nullptr, "Overshoot divider is absent.");
        const math::Vector2 start{divider->bounds.x + divider->bounds.width * .5f, before.y + before.height * .35f};
        const math::Vector2 inside{start.x - 48, start.y}, past{before.x + 20, start.y}, further{before.x + 5, start.y};
        const auto preview = [&] {
            const auto placement = Access::dock_presentation(app);
            require(placement.preview.has_value(), "Overshoot drag has no divider preview.");
            return placement.preview->x;
        };
        pointer(ui::EventKind::pointer_move, start);
        tick(app, 2);
        pointer(ui::EventKind::pointer_down, start);
        pointer(ui::EventKind::pointer_move, inside);
        tick(app, 1);
        const auto reference = preview();
        pointer(ui::EventKind::pointer_move, past);
        tick(app, 1);
        const auto limit = preview();
        require(limit < reference - 1, "Divider preview did not reach its limit.");
        pointer(ui::EventKind::pointer_move, further);
        tick(app, 1);
        require(preview() == limit, "Divider preview left its limit while the pointer travelled past it.");
        pointer(ui::EventKind::pointer_move, inside);
        tick(app, 1);
        require(std::abs(preview() - reference) * app.presented()->drawing->dpi_scale <= 1.f,
                "Divider did not return under the pointer after travelling past its limit.");
        pointer(ui::EventKind::pointer_up, inside);
        tick(app, 2);
        const auto after = app.presented()->find(control::viewport)->bounds;
        require(std::abs(after.width - before.width - (inside.x - start.x)) <= 1.f,
                "Divider committed away from the pointer after travelling past its limit.");
        // Restore the width the following cases start from.
        const auto* moved = app.presented()->find(app.docking().divider(control::scene_tab, ui::DragAxis::horizontal));
        const math::Vector2 back{moved->bounds.x + moved->bounds.width * .5f, start.y};
        pointer(ui::EventKind::pointer_move, back);
        tick(app, 2);
        pointer(ui::EventKind::pointer_down, back);
        for (unsigned step = 1; step <= 4; ++step) {
            pointer(ui::EventKind::pointer_move, {back.x + (start.x - inside.x) * step / 4, back.y});
            tick(app, 1);
        }
        pointer(ui::EventKind::pointer_up, {back.x + start.x - inside.x, back.y});
        tick(app, 2);
        require(std::abs(app.presented()->find(control::viewport)->bounds.width - before.width) <= 1.f,
                "Overshoot case did not restore the viewport width.");
    }
    for (const auto axis : {ui::DragAxis::horizontal, ui::DragAxis::vertical})
        for (unsigned reason = 0; reason < 3; ++reason) {
            const auto before = app.presented()->find(control::viewport)->bounds;
            const auto* divider = app.presented()->find(app.docking().divider(control::scene_tab, axis));
            require(divider != nullptr, "Cancelled viewport resize divider is absent.");
            const bool horizontal = axis == ui::DragAxis::horizontal;
            const math::Vector2 start =
                horizontal
                    ? math::Vector2{divider->bounds.x + divider->bounds.width * .5f, before.y + before.height * .35f}
                    : math::Vector2{before.x + before.width * .3f, divider->bounds.y + divider->bounds.height * .5f};
            const math::Vector2 end{start.x - (horizontal ? 48.f : 0), start.y - (horizontal ? 0 : 48.f)};
            pointer(ui::EventKind::pointer_move, start);
            pointer(ui::EventKind::pointer_down, start);
            pointer(ui::EventKind::pointer_move, end);
            tick(app, 1);
            require(app.docking().dragging() && GetCapture() == app.window(),
                    "Viewport resize cancellation setup failed.");
            if (reason == 0) {
                Access::input(app, {.kind = ui::EventKind::key_down, .key = ui::Key::escape}, VK_ESCAPE);
                Access::input(app, {.kind = ui::EventKind::key_up, .key = ui::Key::escape}, VK_ESCAPE);
            } else
                Access::input(app, {.kind = reason == 1 ? ui::EventKind::focus_lost : ui::EventKind::capture_lost});
            pointer(ui::EventKind::pointer_up, end);
            tick(app, 2);
            require(!app.docking().dragging() && GetCapture() != app.window() &&
                        app.presented()->find(control::viewport)->bounds == before,
                    "Cancelled viewport resize retained capture or committed a late release.");
        }
    const unsigned discarded = ExternalMouseFilter::discarded - discarded_before;
    external_mouse.reset();
    RECT host{};
    GetWindowRect(app.window(), &host);
    require(SetCursorPos(host.left + 32, host.top + 12) != FALSE, "Cannot park the splitter gesture pointer.");
    tick(app, 2);
    std::printf(
        "Viewport splitter acceptance: PASS (both axes, shrink/expand, viewport crossing, travel past the limit and back, deferred geometry, Escape/focus/capture loss); external mouse messages dropped %u\n",
        discarded);
}
