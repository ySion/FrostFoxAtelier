// Real Session/DirectWrite input against accepted and skipped style candidates.
void accepted_text_style_snapshot() {
    std::shared_ptr<const ui::UiFrame> retained;
    {
        TextConsumer c(L"ABCDEFGHIJKLMN");
        c.prepare(); retained=c.frame;
        const auto* displayed=retained->find_text(1);
        require(displayed && displayed->paragraph,"editable frame has no paragraph snapshot");
        const auto from=displayed->paragraph->caret({2,false});
        const auto to=displayed->paragraph->caret({5,false});
        auto style=c.ui.style(); style.font.size=22; style.control_padding.left=28;
        accepted(c.ui.set_style(style));
        c.controls[0].layout.width=ui_layout::Axis::fixed(180);
        c.controls[0].layout.height=ui_layout::Axis::fixed(64);
        c.prepare(false);
        require(c.ui.presented()==retained && displayed->paragraph->caret({2,false}).x==from.x,
            "pending font/width changed an accepted paragraph");
        const float y=displayed->y+from.height*.5f;
        c.ui.input({.kind=ui::EventKind::pointer_down,.x=displayed->x+from.x,.y=y});
        c.ui.input({.kind=ui::EventKind::pointer_move,.x=displayed->x+to.x,.y=y});
        c.ui.input({.kind=ui::EventKind::pointer_up,.x=displayed->x+to.x,.y=y});
        require(request_of<ui::ClipboardRequest>(c.command(ui::Key::c,true)).text==L"CDE",
            "old pixels were hit using pending font/padding");
        c.command(ui::Key::home); c.command(ui::Key::right); c.command(ui::Key::right,false,true);
        require(request_of<ui::ClipboardRequest>(c.command(ui::Key::c,true)).text==L"B",
            "selection revisions incorrectly invalidated accepted navigation");
    }
    require(retained->find_text(1)->paragraph->caret({2,false}).x>0,
        "retained paragraph depended on a destroyed text store");

    TextConsumer scrolling(L"ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMNOPQRSTUVWXYZ");
    scrolling.controls[0].layout.width=ui_layout::Axis::fixed(110);
    scrolling.command(ui::Key::home); scrolling.prepare();
    const auto old=scrolling.frame;
    const auto* geometry=old->find_text(1);
    scrolling.command(ui::Key::end); scrolling.prepare(false);
    const float y=geometry->y+geometry->paragraph->caret({}).height*.5f;
    scrolling.ui.input({.kind=ui::EventKind::pointer_down,.x=geometry->x,.y=y});
    scrolling.ui.input({.kind=ui::EventKind::pointer_up,.x=geometry->x,.y=y});
    scrolling.command(ui::Key::right,false,true);
    require(request_of<ui::ClipboardRequest>(scrolling.command(ui::Key::c,true)).text==L"A",
        "skipped candidate scroll changed displayed text hit testing");

    TextConsumer pending(L"AB"); pending.command(ui::Key::end); pending.prepare();
    const auto* before=pending.frame->find_text(1);
    pending.type(L"C");
    pending.ui.input({.kind=ui::EventKind::pointer_down,.x=before->x,.y=before->y+5});
    pending.ui.input({.kind=ui::EventKind::pointer_up,.x=before->x,.y=before->y+5});
    pending.type(L"D");
    require(pending.value()==L"ABCD","stale pointer geometry changed a newer text draft");
    pending.command(ui::Key::home); pending.command(ui::Key::right,false,true);
    require(request_of<ui::ClipboardRequest>(pending.command(ui::Key::c,true)).text==L"A",
        "batched keyboard edits lost navigation before repaint");
}
