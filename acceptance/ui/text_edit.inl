struct TextConsumer : Consumer {
    explicit TextConsumer(std::wstring initial) {
        controls = {leaf(1, ui::ControlKind::text_edit, 10, std::move(initial))};
        prepare();
        click(1);
    }
};
