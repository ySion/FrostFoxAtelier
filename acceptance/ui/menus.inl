void shared_menu_semantics() {
    Consumer c;
    std::array items{ui::MenuItem{1, L"Grid", true, true, {}},     ui::MenuItem{2, L"Axes", true, false, {}},
                     ui::MenuItem{3, L"Solid", true, true, {}},    ui::MenuItem{4, L"Wire", true, false, {}},
                     ui::MenuItem{5, L"Search", false, false, {}}, ui::MenuItem{6, L"Triangles", true, true, {}}};
    items[0].behavior = items[1].behavior = ui::MenuBehavior::checkbox;
    items[1].icon = ui::ControlIcon::axes;
    for (auto index : {2, 3, 5}) {
        items[index].behavior = ui::MenuBehavior::radio;
        items[index].radio_group = index == 5 ? 80 : 70;
    }
    items[2].icon = ui::ControlIcon::mesh;
    items[3].icon = ui::ControlIcon::world;
    items[4].icon = ui::ControlIcon::search;
    const auto open = [&] {
        accepted(c.ui.set_controls({}));
        c.controls.clear();
        c.controls = menu_controls(items, 240);
        c.prepare();
    };
    // A marker is every framed box and glyph in the indicator column: checkboxes draw a
    // frame plus an optional check glyph, radios a single glyph.
    const auto marker = [&](ui::ControlKey key) {
        const auto& box = *c.frame->find(key);
        const auto scale = c.frame->drawing->dpi_scale;
        const auto right = box.bounds.x + (box.style->padding.left + box.style->indicator_size + 3) * scale;
        std::vector<std::uint8_t> pixels;
        for (const auto& draw : c.frame->drawing->draws)
            if ((draw.coverage || draw.border_width > 0) && draw.bounds.x < right && draw.bounds.y >= box.bounds.y &&
                draw.bounds.y + draw.bounds.height <= box.bounds.y + box.bounds.height) {
                if (draw.coverage)
                    pixels.insert(pixels.end(), draw.coverage->pixels.begin(), draw.coverage->pixels.end());
                else
                    pixels.push_back(0xff);
            }
        return pixels;
    };
    const auto activation = [](const ui::Reply& reply) {
        const auto actions = actions_of(reply);
        const auto at = std::find_if(actions.begin(), actions.end(),
                                     [](const auto& action) { return action.kind == ui::ActionKind::activate; });
        require(at != actions.end(), "menu did not activate");
        return *at;
    };
    open();
    const auto checked_box = marker(1);
    const auto unchecked_box = marker(2);
    const auto checked_radio = marker(3);
    const auto unchecked_radio = marker(4);
    require(!checked_box.empty() && !unchecked_box.empty() && !checked_radio.empty() && !unchecked_radio.empty() &&
                checked_box != unchecked_box && checked_radio != unchecked_radio && checked_box != checked_radio &&
                unchecked_box != unchecked_radio,
            "menu checkbox/radio markers did not visibly distinguish kind and checked state");
    require(c.frame->find(3)->menu_behavior == ui::MenuBehavior::radio && c.frame->find(3)->radio_group == 70,
            "menu semantics were not retained in the accepted frame");
    const auto disabled = c.frame->find(5)->bounds;
    require(c.ui.input({.kind = ui::EventKind::pointer_down, .x = disabled.x + 4, .y = disabled.y + 4}).capture !=
                ui::Capture::acquire,
            "disabled icon menu item captured input");
    const auto hover = c.frame->find(2)->bounds;
    c.ui.input({.kind = ui::EventKind::pointer_move, .x = hover.x + 4, .y = hover.y + 4});
    c.prepare();
    require(c.frame->find(1)->selected && !c.frame->find(2)->selected && marker(1) == checked_box &&
                marker(2) == unchecked_box,
            "menu hover changed checked state");
    const auto hovered = c.frame->find(2)->style->background;
    const auto checked = activation(c.click(1));
    require(!std::get<bool>(checked.value), "checkbox menu did not request its unchecked value");
    c.prepare();
    require(!c.frame->find(1), "dismissed menu retained an interactive row");
    items[0].selected = false;
    open();
    require(marker(1) == unchecked_box && c.frame->find(2)->style->background != hovered,
            "reopened menu retained its old check or hover state");
    auto request = activation(c.click(4));
    require(std::get<bool>(request.value), "radio item did not request selection");
    items[2].selected = false;
    items[3].selected = true;
    open();
    require(marker(3) == unchecked_radio && marker(4) == checked_radio && marker(6) == checked_radio,
            "radio selection changed another group or failed to move its marker");
    c.ui.focus(4);
    c.ui.input({.kind = ui::EventKind::key_down, .key = ui::Key::enter});
    request = activation(c.ui.input({.kind = ui::EventKind::key_up, .key = ui::Key::enter}));
    require(std::get<bool>(request.value), "keyboard activation toggled a selected radio item off");
    open();
    auto invalid = c.controls;
    for (auto& control : invalid)
        if (control.key == 3)
            control.selected = true;
    require(!c.ui.set_controls(invalid), "one radio group accepted two selected items");
    auto changed =
        *std::find_if(c.controls.begin(), c.controls.end(), [](const auto& control) { return control.key == 1; });
    changed.selected = true;
    accepted(c.ui.update_controls(std::span(&changed, 1)));
    require(std::get<bool>(activation(c.click(1)).value), "checkbox activation used an unaccepted checked value");
    open();
    c.ui.focus(4);
    c.ui.input({.kind = ui::EventKind::key_down, .key = ui::Key::space});
    changed = *std::find_if(c.controls.begin(), c.controls.end(), [](const auto& control) { return control.key == 4; });
    changed.radio_group = 90;
    accepted(c.ui.update_controls(std::span(&changed, 1)));
    require(actions_of(c.ui.input({.kind = ui::EventKind::key_up, .key = ui::Key::space})).empty(),
            "retargeted radio group retained its held key");

    Consumer icons;
    auto face = leaf(10, ui::ControlKind::button, 10);
    face.layout.width = ui_layout::Axis::fixed(30);
    face.layout.height = ui_layout::Axis::fixed(30);
    const auto coverages = [](const ui::UiFrame& frame) {
        return std::count_if(frame.drawing->draws.begin(), frame.drawing->draws.end(),
                             [](const auto& draw) { return static_cast<bool>(draw.coverage); });
    };
    for (auto icon : {ui::ControlIcon::search, ui::ControlIcon::view_list, ui::ControlIcon::view_details,
                      ui::ControlIcon::view_tiles, ui::ControlIcon::filter, ui::ControlIcon::more}) {
        face.kind = ui::ControlKind::button;
        face.icon = icon;
        icons.controls = {face};
        icons.prepare();
        const auto plain = coverages(*icons.frame);
        face.kind = ui::ControlKind::combo;
        icons.controls = {face};
        icons.prepare();
        require(plain == 1 && coverages(*icons.frame) == plain + 1 && icons.frame->find(10)->bounds.width == 30,
                "compact icon menu lost its icon/corner indicator or reserved a full dropdown column");
    }
    Consumer form;
    auto outside = leaf(600, ui::ControlKind::button, 10, L"Outside");
    auto popup = leaf(700, ui::ControlKind::popup, 60);
    popup.popup_owner = 600;
    popup.layout = ui_layout::NodeInput::column();
    popup.layout.width = ui_layout::Axis::fixed(240);
    popup.layout.offset_y = 60;
    auto hex = leaf(701, ui::ControlKind::text_edit, 0, L"#FFFFFF");
    hex.parent = 700;
    auto channel = leaf(702, ui::ControlKind::number, 0);
    channel.parent = 700;
    channel.minimum = 0;
    channel.maximum = 8;
    channel.value = 2;
    auto slider = leaf(703, ui::ControlKind::slider, 0);
    slider.parent = 700;
    slider.minimum = 0;
    slider.maximum = 8;
    slider.value = 2;
    slider.drag_step = .5;
    auto done = leaf(704, ui::ControlKind::button, 0, L"Done");
    done.parent = 700;
    form.controls = {outside, popup, hex, channel, slider, done};
    form.prepare();
    form.ui.focus(701);
    key(form.ui, ui::Key::a, true);
    form.ui.input({.kind = ui::EventKind::text, .data = std::wstring(L"#804020")});
    auto committed = actions_of(key(form.ui, ui::Key::enter));
    require(std::ranges::any_of(committed,
                                [](const auto& a) {
                                    return a.kind == ui::ActionKind::edit_commit && a.key == 701 &&
                                           std::get<std::wstring>(a.value) == L"#804020";
                                }) &&
                form.ui.has_popup(),
            "Popup form swallowed text input or dismissed on Enter.");
    key(form.ui, ui::Key::tab);
    require(form.ui.focused_control() == 702 && form.ui.has_popup(), "Popup form Tab did not focus its next field.");
    key(form.ui, ui::Key::a, true);
    form.ui.input({.kind = ui::EventKind::text, .data = std::wstring(L"3.5")});
    committed = actions_of(key(form.ui, ui::Key::enter));
    require(std::ranges::any_of(committed,
                                [](const auto& a) {
                                    return a.kind == ui::ActionKind::edit_commit && a.key == 702 &&
                                           std::get<double>(a.value) == 3.5;
                                }),
            "Popup form swallowed numeric input.");
    key(form.ui, ui::Key::tab);
    committed = actions_of(key(form.ui, ui::Key::right));
    require(std::ranges::any_of(committed,
                                [](const auto& a) {
                                    return a.kind == ui::ActionKind::edit_commit && a.key == 703 &&
                                           std::get<double>(a.value) == 2.5;
                                }),
            "Popup form swallowed slider navigation.");
    key(form.ui, ui::Key::tab);
    key(form.ui, ui::Key::tab);
    require(form.ui.focused_control() == 701, "Popup form Tab escaped to the background controls.");
    const auto dismissed = actions_of(key(form.ui, ui::Key::escape));
    require(
        std::ranges::any_of(dismissed, [](const auto& a) { return a.kind == ui::ActionKind::dismiss && a.key == 700; }),
        "Popup form Escape did not request dismissal.");
    form.controls = {outside};
    form.prepare();
    require(!form.ui.has_popup(), "Removed form retained a modal barrier.");
}
