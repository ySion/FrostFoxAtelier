std::shared_ptr<const ui::StyleSheet> sheet(std::string_view source) {
    auto result = ui::StyleSheet::parse(source);
    if (!result)
        throw std::runtime_error("SS " + std::to_string(result.error().line) + ":" +
                                 std::to_string(result.error().column) + " " + result.error().message);
    return std::move(*result);
}

void style_sheets() {
    Consumer c;
    const auto rules = sheet(R"ss(
        /* Token definitions can follow their uses. */
        button { padding: token(space); background: #ff0000; }
        .compact { padding: 2dip 6dip; }
        .compact:hover { border-color: #0000ff; }
        .compact:disabled { color: #000000; }
        *:role(caption) { font-size: 11dip; }
        @tokens { space: 4dip 8dip; }
    )ss");
    accepted(c.ui.set_style_sheet(rules));
    ui::Control root;
    root.key = 10;
    root.layout = ui_layout::NodeInput::column();
    root.layout.width = ui_layout::Axis::fixed(420);
    root.layout.height = ui_layout::Axis::fixed(160);
    root.style.font_size = 18;
    root.style.foreground = ui::Color{1, 1, 1, 1};
    root.style.font_style = ui::FontStyle::italic;
    root.style_scope = sheet(".special { background: #00ff00; }");
    auto first = leaf(1, ui::ControlKind::button, 0, L"Styled first");
    first.parent = 10;
    first.classes = {"compact", "special"};
    auto second = leaf(2, ui::ControlKind::button, 0, L"Styled second");
    second.parent = 10;
    second.classes = {"special", "compact"};
    auto outside = leaf(3, ui::ControlKind::button, 210, L"Outside");
    auto caption = leaf(4, ui::ControlKind::label, 260, L"Caption");
    caption.role = ui::Role::caption;
    c.controls = {root, first, second, outside, caption};
    c.prepare();
    const auto original = c.frame;
    const auto& a = *original->find(1)->style;
    const auto& b = *original->find(2)->style;
    require(a.font.size == 18 && a.font.style == ui::FontStyle::italic && a.foreground == ui::Color{1, 1, 1, 1} &&
                a.padding == ui_layout::Insets{6, 2, 6, 2},
            "SS inheritance/class/token did not feed final measurement style");
    require(a.background == ui::Color{0, 1, 0, 1} && b.background == a.background && b.padding == a.padding,
            "local scope or class order changed rule precedence");
    require(original->find(3)->style->background == ui::Color{1, 0, 0, 1} &&
                original->find(3)->style->font.size == 14 && original->find(4)->style->font.size == 11,
            "scope escaped to siblings or role selector failed");
    const auto original_text = first_text(*original).coverage;
    const auto stats = c.ui.statistics();
    const auto* box = original->find(1);
    c.ui.input({.kind = ui::EventKind::pointer_move, .x = box->bounds.x + 15, .y = box->bounds.y + 15});
    c.prepare(false);
    require(c.frame->find(1)->style->border == ui::Color{0, 0, 1, 1} &&
                first_text(*c.frame).coverage == original_text &&
                c.ui.statistics().text_layouts == stats.text_layouts &&
                c.ui.statistics().rasterizations == stats.rasterizations,
            "paint state rebuilt paragraph or failed to resolve hover");
    require(c.ui.presented() == original && original->find(1)->style->border != ui::Color{0, 0, 1, 1},
            "candidate changed accepted computed style");
    require(c.ui.accept(c.frame), "styled candidate rejected");
    c.controls[1].style.font_size = 24;
    c.controls[1].style.background = ui::Color{1, 1, 0, 1};
    c.prepare();
    require(c.frame->find(1)->style->font.size == 24 && c.frame->find(1)->style->background == ui::Color{1, 1, 0, 1} &&
                c.frame->find(2)->style->font.size == 18 &&
                c.ui.statistics().provider_revision == stats.provider_revision,
            "local font/override changed a sibling or global font provider");
    c.controls[1].style = {};
    c.controls[1].enabled = false;
    c.prepare();
    require(c.frame->find(1)->style->font.size == 18 && c.frame->find(1)->style->background == ui::Color{0, 1, 0, 1} &&
                c.frame->find(1)->style->foreground == ui::Color{0, 0, 0, 1},
            "unset/disabled did not resolve through the same rules");
    c.controls[1].enabled = true;
    c.controls[0].style = {};
    c.controls[0].style_scope.reset();
    c.prepare();
    require(c.frame->find(1)->style->font.size == 14 && c.frame->find(1)->style->font.style == ui::FontStyle::normal &&
                original->find(1)->style->font.style == ui::FontStyle::italic &&
                c.frame->find(1)->style->background == ui::Color{1, 0, 0, 1},
            "removing scope/inherited overrides did not restore defaults");

    ui::StyleRule native;
    native.selector.kind = ui::ControlKind::button;
    native.selector.classes = {"compact"};
    native.properties.background = ui::Color{0, 0, 1, 1};
    auto native_sheet = ui::StyleSheet::create({native});
    require(native_sheet.has_value(), "native style producer rejected");
    accepted(c.ui.set_style_sheet(*native_sheet));
    c.prepare();
    require(c.frame->find(1)->style->background == ui::Color{0, 0, 1, 1}, "native rules bypassed SS execution path");
    const auto stable = c.frame;
    for (const auto source :
         {"button { unknown: 1; }", "button:hover { padding: 5dip; }",
          "@tokens { a: token(b); b: token(a); } button { color: token(a); }", "button { color: token(missing); }",
          "button { font-size: -2dip; }", "button .child { color: #ffffff; }", "/* missing close",
          "button { background: #gg0000; }", "button:enabled:disabled { color: #ffffff; }",
          "button:hover { font-weight: 700; }", "button:hover { font-style: italic; }",
          "button { font-style: sideways; }", "button { background-image: linear-gradient(#ffffff); }",
          "button { border-style: unknown; }", "popup { scrollbar-gutter: unknown; }",
          "popup:hover { scrollbar-gutter: overlay; }", "button { border-shadow-color: #gggggg; }"}) {
        const auto invalid = ui::StyleSheet::parse(source);
        require(!invalid && invalid.error().line > 0 && invalid.error().column > 0,
                "invalid SS lacked positioned diagnosis");
    }
    require(c.ui.presented() == stable, "failed SS candidate replaced accepted style");
    native.selector.states = ui::StyleState::pressed;
    native.properties.font_size = 30;
    require(!ui::StyleSheet::create({native}), "native rule bypassed state/layout restrictions");
    c.controls[1].classes = {"123invalid"};
    require(!c.ui.set_controls(c.controls) && c.ui.presented() == stable, "invalid class damaged live controls");
    const auto error = ui::StyleSheet::parse("button {\n  padding: -1dip;\n}");
    require(!error && error.error().line == 2, "SS source location was lost");

    auto pixels = std::make_shared<ui::ColorImage>();
    pixels->identity = 42;
    pixels->revision = 1;
    pixels->width = pixels->height = 1;
    pixels->row_pitch = 4;
    pixels->pixels = {255, 0, 0, 255};
    const ui::NamedImage images[] = {{"icon", pixels}};
    const auto painted =
        ui::StyleSheet::parse("button { border-radius: 2dip 4dip 6dip 8dip; background-image: image(icon); }", images);
    require(painted.has_value(), "named image style rejected");
    c.controls[1].classes = {"compact"};
    accepted(c.ui.set_style_sheet(*painted));
    c.prepare();
    const auto shape =
        std::find_if(c.frame->drawing->draws.begin(), c.frame->drawing->draws.end(),
                     [&](const auto& draw) { return draw.image == pixels && draw.bounds == c.frame->find(1)->bounds; });
    const auto scale = c.frame->drawing->dpi_scale;
    require(shape != c.frame->drawing->draws.end() &&
                shape->radius == ui::CornerRadii{2 * scale, 4 * scale, 6 * scale, 8 * scale} &&
                shape->border_width == c.frame->find(1)->style->border_width * scale,
            "SS image/radius/border did not reach the production draw protocol");
    auto malformed = std::make_shared<ui::ColorImage>(*pixels);
    malformed->row_pitch = 1;
    ui::StyleRule invalid_image;
    invalid_image.properties.image = malformed;
    require(!ui::StyleSheet::create({invalid_image}), "invalid image resource entered native style rules");

    accepted(c.ui.set_style_sheet(sheet(R"ss(
        button { background-image: linear-gradient(#ff000080, transparent); border-style: outset; border-color: #808080; border-shadow-color: #000000; }
        button:modified { font-weight: 700; font-style: italic; }
    )ss")));
    c.prepare();
    const auto gradient_frame = c.frame;
    const auto gradient = gradient_frame->find(1)->style->image;
    require(gradient && gradient == gradient_frame->find(2)->style->image && gradient->pixels.size() == 256 &&
                gradient->pixels[0] == 188 && gradient->pixels[3] == 128 && gradient->pixels.back() == 0,
            "gradient did not share a correctly encoded premultiplied image");
    require(std::any_of(gradient_frame->drawing->draws.begin(), gradient_frame->drawing->draws.end(),
                        [&](const auto& draw) { return draw.image == gradient && draw.border_width == 0; }),
            "gradient/bevel did not reach the existing drawing path");
    require(std::any_of(gradient_frame->drawing->draws.begin(), gradient_frame->drawing->draws.end(),
                        [](const auto& draw) {
                            return !draw.coverage && draw.color == ui::Color{0, 0, 0, 1} && draw.bounds.width <= 2;
                        }),
            "Explicit bevel shadow color did not reach the production edge drawing path");
    c.controls[1].modified = true;
    c.prepare();
    require(c.frame->find(1)->style->font.weight == 700 && gradient_frame->find(1)->style->font.weight == 400 &&
                c.frame->find(1)->style->font.style == ui::FontStyle::italic &&
                gradient_frame->find(1)->style->font.style == ui::FontStyle::normal &&
                c.frame->find(1)->style->image == gradient,
            "persistent weight state changed an old frame or rebuilt the gradient");

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
    std::uint64_t revision = 1;
    for (float dpi : {1.f, 1.5f}) {
        accepted(scrolling.ui.set_surface({{640, 480}, dpi, ++revision}));
        accepted(scrolling.ui.set_style_sheet(sheet("list { padding: 5dip 0dip 20dip 0dip; }")));
        scrolling.prepare();
        scrolling.ui.input({.kind = ui::EventKind::wheel, .x = 20 * dpi, .y = 50 * dpi, .wheel = -1000});
        scrolling.prepare();
        const auto tail = scrolling.frame->find(22)->bounds;
        require(std::abs(tail.y + tail.height - 77 * dpi) < .01f,
                "list scroll range omitted resolved bottom padding or applied DPI twice");
        accepted(scrolling.ui.set_style_sheet({}));
        scrolling.prepare();
        require(std::abs(scrolling.frame->find(20)->bounds.y) < .01f,
                "removing list padding retained out-of-range scroll");
    }
    scrolling.controls[0].layout.height = ui_layout::Axis::fixed(80);
    scrolling.controls[1].layout.width = ui_layout::Axis::expand();
    scrolling.prepare();
    const auto with_gutter = scrolling.frame;
    accepted(scrolling.ui.set_style_sheet(sheet("list { scrollbar-gutter: overlay; }")));
    scrolling.prepare();
    require(scrolling.frame->find(20)->bounds.width > with_gutter->find(20)->bounds.width &&
                scrolling.frame->find(20)->clip.right > with_gutter->find(20)->clip.right &&
                !scrolling.frame->scrollbars.empty(),
            "Overlay gutter did not release content width while retaining scrolling");

    Consumer menu;
    accepted(menu.ui.set_style_sheet(sheet(R"ss(
        popup.menu { font-size: 13.333dip; scrollbar-gutter: overlay; scrollbar-width: 16dip; scrollbar-inset: 1dip; }
        menu_item { padding: 0dip 9dip; }
        menu_item:selected { font-weight: 600; }
    )ss")));
    const std::array entries{
        ui::MenuItem{1, L"Scene View", true, true, {}}, ui::MenuItem{2, L"Parameters", true, false, {}},
        ui::MenuItem{3, L"Controls", true, false, {}}, ui::MenuItem{4, L"Entities", true, false, {}}};
    for (const auto dpi : {1.f, 1.25f, 1.5f, 1.75f, 2.f}) {
        menu.controls.clear();
        accepted(menu.ui.set_controls({}));
        accepted(menu.ui.set_surface({{1920, 1080}, dpi, ++revision}));
        menu.controls = menu_controls(entries, 160);
        menu.prepare();
        const auto bounds = menu.frame->find(500)->bounds;
        if (!menu.frame->scrollbars.empty())
            throw std::runtime_error("Fitting four-item menu has scroll range " +
                                     std::to_string(menu.frame->scrollbars.front().maximum) + " DIP at DPI " +
                                     std::to_string(dpi));
        const auto extent = ui::PixelExtent{static_cast<std::uint32_t>(std::ceil(bounds.width)),
                                            static_cast<std::uint32_t>(std::ceil(bounds.height))};
        accepted(menu.ui.set_controls({}));
        accepted(menu.ui.set_surface({extent, dpi, ++revision}));
        menu.prepare();
        if (!menu.frame->scrollbars.empty())
            throw std::runtime_error(
                "Pixel-sized fitting menu: DPI=" + std::to_string(dpi) + " measured=" + std::to_string(bounds.height) +
                " px, window=" + std::to_string(extent.height) +
                " px, laid out=" + std::to_string(menu.frame->find(500)->bounds.height) +
                " px, overflow=" + std::to_string(menu.frame->scrollbars.front().maximum) + " DIP");
        const auto first_row = menu.frame->find(1)->bounds;
        menu.ui.input({.kind = ui::EventKind::wheel, .x = 15 * dpi, .y = 10 * dpi, .wheel = -1000});
        menu.prepare();
        require(menu.frame->find(1)->bounds == first_row, "Fitting menu moved on wheel input");
        const auto natural_height = menu.controls.front().layout.height;
        menu.controls.front().layout.height = ui_layout::Axis::fixed(bounds.height / dpi - .25f);
        menu.prepare();
        require(!menu.frame->scrollbars.empty() && menu.frame->scrollbars.front().maximum > .2,
                "Roundoff handling swallowed genuine subpixel overflow");
        menu.controls.front().layout.height = natural_height;
        accepted(menu.ui.set_controls({}));
        accepted(menu.ui.set_surface({{extent.width, extent.height - 12}, dpi, ++revision}));
        menu.prepare();
        require(!menu.frame->scrollbars.empty() && menu.frame->scrollbars.front().maximum > 0,
                "Clipped menu lost its real scrollbar");
        key(menu.ui, ui::Key::end);
        menu.prepare();
        const auto tail = menu.frame->find(4);
        require(tail->bounds.y + tail->bounds.height <= static_cast<float>(tail->clip.bottom) + .01f,
                "Clipped menu could not reveal its last command");
    }
}
