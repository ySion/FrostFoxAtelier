void accepted_text_style_snapshot() {
    std::shared_ptr<const ui::UiFrame> retained;
    {
        TextConsumer c(L"ABCDEFGHIJKLMN");
        c.prepare();
        retained = c.frame;
        const auto* displayed = retained->find_text(1);
        require(displayed && displayed->paragraph, "editable frame has no paragraph snapshot");
    }
    require(accepted(retained->find_text(1)->paragraph->caret({2, false})).x > 0,
            "retained paragraph depended on a destroyed text store");
}
