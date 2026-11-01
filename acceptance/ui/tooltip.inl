void tooltip_contracts() {
    for (const auto dpi : {1.f, 1.5f}) {
        Consumer c;
        accepted(c.ui.set_surface({{640, 480}, dpi, 2}));
        auto viewport = leaf(40, ui::ControlKind::viewport, 0);
        viewport.layout = ui_layout::NodeInput::box(640 / dpi, 480 / dpi);
        ui::Control row;
        row.key = 1;
        row.kind = ui::ControlKind::container;
        row.enabled = false;
        row.layout = ui_layout::NodeInput::row();
        row.layout.width = ui_layout::Axis::fixed(280);
        row.layout.height = ui_layout::Axis::fixed(36);
        row.layout.offset_x = 640 / dpi - 290;
        row.layout.offset_y = 480 / dpi - 46;
        row.tooltip = std::make_shared<const ui::Tooltip>(
            ui::Tooltip{L"Position", L"Entity.position · Vector3",
                        L"相对于当前 Space Reference 的位置，单位 m。\n修改会移动 Entity；鼠标悬停可以查看说明。"});
        auto label = leaf(2, ui::ControlKind::label, 0, L"Position");
        label.parent = 1;
        label.layout = ui_layout::NodeInput::box(100, 36);
        auto number = leaf(3, ui::ControlKind::number, 0);
        number.parent = 1;
        number.layout = ui_layout::NodeInput::box(180, 36);
        c.controls = {viewport, row, label, number};
        c.prepare();
        const auto viewport_bounds = c.frame->find(40)->bounds;
        const auto label_bounds = c.frame->find(2)->bounds;
        const float x = label_bounds.x + 10 * dpi;
        const float y = label_bounds.y + 10 * dpi;
        c.ui.input({.kind = ui::EventKind::pointer_move, .x = x, .y = y});
        c.prepare();
        require(!c.frame->tooltip, "help appeared before its hover delay");
        // An unrelated binding can update every rendered frame without restarting hover.
        c.controls[0].selected = true;
        c.prepare();
        Sleep(520);
        c.prepare();
        require(c.frame->tooltip && c.frame->tooltip->owner == 1 && c.frame->tooltip->content == row.tooltip,
                "disabled parameter label did not inherit its accepted row documentation");
        require(c.frame->controls.size() == 4 && c.frame->find(40)->bounds == viewport_bounds &&
                    c.frame->drawing->extent == ui::PixelExtent{640, 480},
                "help entered layout or changed the viewport target");
        const auto& tip = c.frame->tooltip->bounds;
        require(tip.x >= 0 && tip.y >= 0 && tip.x + tip.width <= 640 && tip.y + tip.height <= 480,
                "help escaped the client edge at a non-unit DPI");
        require(c.ui.pointer_target(x, y) != 2, "help made a disabled label interactive");
        const auto warm = c.ui.statistics();
        c.prepare();
        c.prepare();
        const auto stable = c.ui.statistics();
        require(stable.text_layouts == warm.text_layouts && stable.rasterizations == warm.rasterizations,
                "stable help recreated its text layout or raster coverage");
        const auto retained = c.frame;
        c.ui.input({.kind = ui::EventKind::wheel, .x = x, .y = y, .wheel = 1});
        c.prepare();
        require(!c.frame->tooltip && retained->tooltip && retained->tooltip->content == row.tooltip,
                "wheel did not dismiss help or mutated a retained frame");

        c.ui.input({.kind = ui::EventKind::pointer_move, .x = x, .y = y});
        Sleep(520);
        const auto pending = accepted(c.ui.prepare());
        require(pending->tooltip.has_value(), "pending help frame did not contain its overlay");
        const auto before_cancel = c.ui.presented();
        if (dpi == 1)
            c.ui.input({.kind = ui::EventKind::pointer_leave});
        else
            c.ui.dismiss_tooltip();
        require(!c.ui.accept(pending) && c.ui.presented() == before_cancel,
                "cancelled help candidate was accepted and resurrected its overlay");
        c.prepare();
        require(!c.frame->tooltip && c.ui.accept(c.frame) && c.ui.accept(c.frame),
                "cancelling help broke ordinary repeat acceptance");
        c.ui.input({.kind = ui::EventKind::pointer_move, .x = x, .y = y});
        c.ui.dismiss_tooltip();
        c.prepare();
        require(!c.frame->tooltip, "host-consumed input retained passive help");

        // A topmost unannotated surface is an occluder, even if it is disabled.
        auto cover = leaf(5, ui::ControlKind::container, 0);
        cover.enabled = false;
        cover.layout = row.layout;
        c.controls.push_back(cover);
        c.prepare();
        c.ui.input({.kind = ui::EventKind::pointer_move, .x = x, .y = y});
        Sleep(520);
        c.prepare();
        require(!c.frame->tooltip, "help passed through an overlapping unannotated surface");
        c.controls.pop_back();
        c.prepare();

        c.ui.input({.kind = ui::EventKind::pointer_move, .x = x, .y = y});
        const auto old = c.frame;
        // Valid reflection documentation has no Tooltip-specific capacity limit.
        c.controls[1].tooltip = std::make_shared<const ui::Tooltip>(ui::Tooltip{
            std::wstring(600, L'T'), std::wstring(1200, L'I'), L"更新后的说明。\n" + std::wstring(5000, L'中')});
        accepted(c.ui.set_controls(c.controls));
        c.ui.input({.kind = ui::EventKind::pointer_move, .x = x, .y = y});
        Sleep(520);
        c.prepare();
        require(!c.frame->tooltip && old->find(1)->tooltip == row.tooltip,
                "an unaccepted description was shown through old geometry");
        c.ui.input({.kind = ui::EventKind::pointer_move, .x = x, .y = y});
        Sleep(520);
        c.prepare();
        require(c.frame->tooltip && c.frame->tooltip->content == c.controls[1].tooltip,
                "accepted description did not replace old help");
        const auto& long_tip = c.frame->tooltip->bounds;
        require(long_tip.x >= 0 && long_tip.y >= 0 && long_tip.x + long_tip.width <= 640 &&
                    long_tip.y + long_tip.height <= 480 && c.frame->find(40)->bounds == viewport_bounds &&
                    c.frame->drawing->extent == ui::PixelExtent{640, 480},
                "long documentation escaped the overlay or changed the viewport target");
        for (const auto& draw : c.frame->drawing->draws)
            if (draw.coverage)
                require(draw.coverage->width <= 640 && draw.coverage->height <= 480,
                        "long help raster exceeded the client extent");
        require(c.ui.accept(c.frame) && c.ui.accept(c.frame), "unchanged help broke repeat acceptance");

        // Own help on a leaf still depends on the accepted lifetime of its ancestors.
        c.controls[2].tooltip = c.controls[1].tooltip;
        c.controls[1].tooltip.reset();
        c.prepare();
        if (dpi == 1)
            c.controls[1].kind = ui::ControlKind::list;
        else {
            ui::Control parent;
            parent.key = 6;
            parent.kind = ui::ControlKind::container;
            parent.layout = ui_layout::NodeInput::box(640 / dpi, 480 / dpi);
            c.controls.insert(c.controls.begin() + 1, parent);
            c.prepare();
            c.controls[2].parent = 6;
        }
        accepted(c.ui.set_controls(c.controls));
        c.ui.input({.kind = ui::EventKind::pointer_move, .x = x, .y = y});
        Sleep(520);
        // Restore before publication: input during the invalid ancestor interval
        // must not have armed help that can survive this round trip.
        if (dpi == 1)
            c.controls[1].kind = ui::ControlKind::container;
        else
            c.controls[2].parent = 0;
        accepted(c.ui.set_controls(c.controls));
        const auto changed_ancestor = accepted(c.ui.prepare());
        require(!changed_ancestor->tooltip, "unaccepted ancestor kind or parent change admitted old leaf geometry");
        require(c.ui.accept(changed_ancestor), "accepting the repaired ancestor geometry failed");
        c.controls.resize(1);
        c.prepare();
        require(!c.frame->tooltip, "removing an owner retained its help");
    }
    std::puts(
        "UI tooltip acceptance: PASS (delay, shared disabled row, accepted ancestry/metadata, stale candidate cancellation, bounded long documentation, DPI, cache, input cancellation, occlusion, removal)");
}
