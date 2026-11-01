void tooltip_contracts() {
    constexpr float dpi = 1.5f;
    Consumer c;
    accepted(c.ui.set_surface({{640, 480}, dpi, 2}));
    ui::Control row;
    row.key = 1;
    row.kind = ui::ControlKind::container;
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
    c.controls = {row, label};
    c.prepare();
    const auto label_bounds = c.frame->find(2)->bounds;
    const float x = label_bounds.x + 10 * dpi;
    const float y = label_bounds.y + 10 * dpi;
    c.ui.input({.kind = ui::EventKind::pointer_move, .x = x, .y = y});
    c.prepare();
    Sleep(520);
    c.prepare();
    require(c.frame->tooltip.has_value(), "help did not appear");
    const auto& tip = c.frame->tooltip->bounds;
    require(tip.x >= 0 && tip.y >= 0 && tip.x + tip.width <= 640 && tip.y + tip.height <= 480,
            "help escaped the client edge at a non-unit DPI");
}
