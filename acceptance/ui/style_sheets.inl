std::shared_ptr<const ui::StyleSheet> sheet(std::string_view source) {
    auto result = ui::StyleSheet::parse(source);
    if (!result)
        throw std::runtime_error("SS " + std::to_string(result.error().line) + ":" +
                                 std::to_string(result.error().column) + " " + result.error().message);
    return std::move(*result);
}

void style_sheets() {
    Consumer c;
    c.controls = {leaf(1, ui::ControlKind::button, 0, L"Styled first")};
    accepted(c.ui.set_style_sheet(sheet("button { background-image: linear-gradient(#ff000080, transparent); }")));
    c.prepare();
    const auto gradient = c.frame->find(1)->style->image;
    // Linear premultiplied red 128/255 encoded as sRGB rounds to 188; alpha remains 128.
    require(gradient && gradient->pixels.size() >= 4 && gradient->pixels[0] == 188 && gradient->pixels[3] == 128 &&
                gradient->pixels.back() == 0,
            "gradient did not encode a premultiplied image");

    Consumer scrolling;
    ui::Control list;
    list.key = 10;
    list.kind = ui::ControlKind::list;
    list.layout = ui_layout::NodeInput::column();
    list.layout.width = ui_layout::Axis::fixed(200);
    list.layout.height = ui_layout::Axis::fixed(100);
    list.layout.padding.bottom = 3;
    scrolling.controls.push_back(list);
    for (unsigned i = 0; i < 3; ++i) {
        auto row = leaf(20 + i, ui::ControlKind::item, 0, L"Row");
        row.parent = 10;
        scrolling.controls.push_back(std::move(row));
    }
    constexpr float dpi = 1.5f;
    accepted(scrolling.ui.set_surface({{640, 480}, dpi, 2}));
    accepted(scrolling.ui.set_style_sheet(sheet("list { padding: 5dip 0dip 20dip 0dip; }")));
    scrolling.prepare();
    scrolling.ui.input({.kind = ui::EventKind::wheel, .x = 20 * dpi, .y = 50 * dpi, .wheel = -1000});
    scrolling.prepare();
    const auto tail = scrolling.frame->find(22)->bounds;
    require(std::abs(tail.y + tail.height - 77 * dpi) < .01f,
            "list scroll range omitted resolved bottom padding or applied DPI twice");
}
