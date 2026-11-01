// Production Session + DirectWrite/ICU consumer. Platform results below are
// injected protocol cases, not evidence that a native IME has run.
template <class T>
T request_of(const ui::Reply& reply) {
    for (const auto& effect : reply.effects)
        if (const auto* platform = std::get_if<ui::PlatformRequest>(&effect))
            if (const auto* value = std::get_if<T>(platform))
                return *value;
    throw std::runtime_error("expected platform request missing");
}

struct TextConsumer : Consumer {
    explicit TextConsumer(std::wstring initial, ui::ControlKind kind = ui::ControlKind::text_edit) {
        controls = {leaf(1, kind, 10, std::move(initial))};
        controls[0].value = 12;
        prepare();
        click(1);
    }

    ui::Reply apply(ui::Reply reply) {
        for (const auto& action : actions_of(reply))
            if (action.key == 1 &&
                (action.kind == ui::ActionKind::edit_preview || action.kind == ui::ActionKind::edit_commit ||
                 action.kind == ui::ActionKind::edit_cancel)) {
                if (const auto* text = std::get_if<std::wstring>(&action.value))
                    controls[0].text = *text;
                if (const auto* value = std::get_if<double>(&action.value))
                    controls[0].value = *value;
            }
        return reply;
    }

    ui::Reply command(ui::Key value, bool control = false, bool shift = false) {
        return apply(key(ui, value, control, shift));
    }

    ui::Reply type(std::wstring text) {
        return apply(ui.input({.kind = ui::EventKind::text, .data = std::move(text)}));
    }

    ui::Reply receive(ui::ClipboardRequest request, bool success, std::wstring text = {}) {
        return apply(ui.input({.kind = ui::EventKind::clipboard_result,
                               .data = ui::ClipboardResult{request.kind, request.operation_id, request.token, success,
                                                           success ? 0u : 5u, std::move(text)}}));
    }

    ui::Reply compose(ui::CompositionEvent event) {
        return apply(ui.input({.kind = ui::EventKind::composition, .data = std::move(event)}));
    }

    ui::EditToken token() const {
        const auto value = ui.text_input();
        require(value.has_value(), "text binding missing");
        return value->token;
    }

    const std::wstring& value() const { return controls[0].text; }
};

void read_only_code_document() {
    Consumer consumer;
    const auto document = std::make_shared<ui::CodeDocument>(L"完整正文\nSecond line");
    auto control = leaf(1, ui::ControlKind::code_editor, 10);
    control.code = document;
    control.read_only = true;
    control.layout.height = ui_layout::Axis::fixed(160);
    consumer.controls = {std::move(control)};
    consumer.prepare();
    consumer.ui.focus(1);
    key(consumer.ui, ui::Key::a, true);
    const auto copy = request_of<ui::ClipboardRequest>(key(consumer.ui, ui::Key::c, true));
    require(copy.text == L"完整正文\nSecond line", "Read-only code did not retain selection and full-text copy.");
    require(consumer.ui.clipboard_current(copy) && !consumer.ui.text_input(),
            "Read-only copy did not have independent clipboard authority without enabling IME.");
    consumer.ui.input({.kind = ui::EventKind::clipboard_result,
                       .data = ui::ClipboardResult{copy.kind, copy.operation_id, copy.token, true, 0, {}}});
    require(!consumer.ui.clipboard_current(copy), "Completed copy retained clipboard authority.");
    const auto erased = key(consumer.ui, ui::Key::delete_key);
    require(erased.consumed && erased.effects.empty() && document->text() == L"完整正文\nSecond line",
            "Read-only Delete changed the document or escaped to the host.");
    consumer.prepare();
    require(consumer.frame->find(1)->read_only && !consumer.ui.text_input() && !consumer.frame->input_anchor &&
                consumer.ui.wants_keyboard() && !consumer.ui.interaction_state().active(),
            "Read-only code exposed an IME binding or lost passive keyboard ownership.");
    const auto unchanged = [&](const ui::Reply& reply) {
        require(reply.consumed && !reply.error && reply.effects.empty() && document->text() == L"完整正文\nSecond line",
                "Read-only input changed text, emitted an edit/request, or escaped to the host.");
    };
    unchanged(consumer.ui.input({.kind = ui::EventKind::text, .data = std::wstring{L"不可输入"}}));
    for (const auto command : {ui::Key::enter, ui::Key::delete_key, ui::Key::backspace})
        unchanged(key(consumer.ui, command));
    for (const auto command : {ui::Key::x, ui::Key::v, ui::Key::z, ui::Key::y})
        unchanged(key(consumer.ui, command, true));
    unchanged(key(consumer.ui, ui::Key::z, true, true));
    unchanged(consumer.ui.input({.kind = ui::EventKind::composition,
                                 .data = ui::CompositionEvent{ui::CompositionKind::start, 90, copy.token, {}, {}}}));
    unchanged(consumer.ui.input(
        {.kind = ui::EventKind::composition,
         .data = ui::CompositionEvent{ui::CompositionKind::end, 90, copy.token, std::wstring{L"迟到输入"}, {}}}));
    unchanged(consumer.ui.execute({ui::EditCommandKind::paste, 1}, consumer.frame->find(1)->incarnation));
    consumer.controls[0].layout.width = ui_layout::Axis::fixed(-1);
    accepted(consumer.ui.set_controls(consumer.controls));
    require(!consumer.ui.prepare(), "Read-only fixture did not suspend frame input.");
    unchanged(key(consumer.ui, ui::Key::delete_key));
    unchanged(key(consumer.ui, ui::Key::z, true));
    consumer.controls[0].layout.width = ui_layout::Axis::fixed(180);
    consumer.prepare();
    key(consumer.ui, ui::Key::home, true);
    key(consumer.ui, ui::Key::right, false, true);
    require(request_of<ui::ClipboardRequest>(key(consumer.ui, ui::Key::c, true)).text == L"完",
            "Read-only keyboard navigation did not preserve character selection.");
    consumer.controls.push_back(leaf(2, ui::ControlKind::button, 190, L"Next"));
    consumer.prepare();
    require(key(consumer.ui, ui::Key::tab).consumed && consumer.ui.focused_control() == 2 &&
                document->text() == L"完整正文\nSecond line",
            "Read-only Tab inserted indentation or trapped keyboard focus.");
    require(key(consumer.ui, ui::Key::tab, false, true).consumed && consumer.ui.focused_control() == 1,
            "Read-only code was not reachable by reverse tab navigation.");

    // Change policy while a real public clipboard request is in flight. Both
    // full and sparse descriptor updates must retire its write authority.
    for (const auto command : {ui::Key::v, ui::Key::x}) {
        consumer.controls[0].read_only = false;
        consumer.prepare();
        consumer.ui.focus(1);
        key(consumer.ui, ui::Key::a, true);
        const auto pending = request_of<ui::ClipboardRequest>(key(consumer.ui, command, true));
        const auto editable_frame = consumer.frame;
        consumer.controls[0].read_only = true;
        if (command == ui::Key::v)
            accepted(consumer.ui.set_controls(consumer.controls));
        else
            accepted(consumer.ui.update_controls(std::span{consumer.controls}.first(1)));
        require(consumer.ui.focused_control() == 1 && !consumer.ui.interaction_state().clipboard &&
                    !consumer.ui.text_input(),
                "Changing read-only policy lost focus or retained a pending clipboard write.");
        require(!consumer.ui.clipboard_current(pending),
                "Read-only policy retained native authority for a pending cut/paste.");
        unchanged(key(consumer.ui, ui::Key::delete_key)); // No newly accepted frame yet.
        consumer.ui.input(
            {.kind = ui::EventKind::clipboard_result,
             .data = ui::ClipboardResult{pending.kind, pending.operation_id, pending.token, true, 0, L"late paste"}});
        require(document->text() == L"完整正文\nSecond line",
                "A late clipboard result wrote through read-only policy.");
        consumer.prepare();
        require(!editable_frame->find(1)->read_only && consumer.frame->find(1)->read_only,
                "Read-only policy mutated an earlier accepted frame.");
        consumer.controls[0].read_only = false;
        accepted(consumer.ui.set_controls(consumer.controls));
        unchanged(consumer.ui.input({.kind = ui::EventKind::text, .data = std::wstring{L"unaccepted write"}}));
    }
    consumer.prepare();
    consumer.ui.focus(1);
    key(consumer.ui, ui::Key::a, true);
    const auto ime_token = consumer.ui.text_input()->token;
    consumer.ui.input({.kind = ui::EventKind::composition,
                       .data = ui::CompositionEvent{ui::CompositionKind::start, 91, ime_token, {}, {}}});
    consumer.ui.input(
        {.kind = ui::EventKind::composition,
         .data = ui::CompositionEvent{ui::CompositionKind::update, 91, ime_token, {}, ui::Preedit{L"拼", 1, {}}}});
    consumer.controls[0].read_only = true;
    const auto cancel = request_of<ui::ImeRequest>(accepted(consumer.ui.set_controls(consumer.controls)));
    require(cancel.kind == ui::ImeKind::cancel && cancel.composition_id == 91 &&
                !consumer.ui.interaction_state().composition,
            "Read-only policy did not cancel an already active composition.");
    consumer.prepare();
    unchanged(consumer.ui.input(
        {.kind = ui::EventKind::composition,
         .data = ui::CompositionEvent{ui::CompositionKind::end, 91, ime_token, std::wstring{L"late IME"}, {}}}));

    // Returning to editable mode preserves the document's local history.
    consumer.controls[0].read_only = false;
    consumer.prepare();
    consumer.ui.focus(1);
    key(consumer.ui, ui::Key::a, true);
    consumer.ui.input({.kind = ui::EventKind::text, .data = std::wstring{L"Changed"}});
    require(document->text() == L"Changed", "Making code editable again did not restore text input.");
    consumer.controls[0].read_only = true;
    consumer.prepare();
    require(key(consumer.ui, ui::Key::z, true).consumed && document->text() == L"Changed",
            "Read-only code consumed existing undo history.");
    consumer.controls[0].read_only = false;
    consumer.prepare();
    key(consumer.ui, ui::Key::z, true);
    require(document->text() == L"完整正文\nSecond line", "Read-only policy destroyed existing editable undo history.");
    consumer.controls[0].read_only = true;
    consumer.prepare();
    unchanged(key(consumer.ui, ui::Key::y, true));
    consumer.controls[0].read_only = false;
    consumer.prepare();
    key(consumer.ui, ui::Key::y, true);
    require(document->text() == L"Changed", "Read-only policy destroyed existing editable redo history.");

    Consumer scroll;
    std::wstring long_text;
    for (unsigned i = 0; i < 60; ++i)
        long_text += std::wstring(120, L'W') + L"\n";
    auto viewer = leaf(1, ui::ControlKind::code_editor, 10);
    viewer.read_only = true;
    viewer.code = std::make_shared<ui::CodeDocument>(std::move(long_text));
    viewer.layout.height = ui_layout::Axis::fixed(160);
    scroll.controls = {std::move(viewer)};
    scroll.prepare();
    scroll.ui.focus(1);
    key(scroll.ui, ui::Key::end, true);
    scroll.prepare();
    const auto before = scroll.frame->find_code(1)->lines.front().row;
    require(before > 0, "Read-only End did not scroll to the document tail.");
    const auto body = scroll.frame->find_code(1)->body;
    require(scroll.ui.input({.kind = ui::EventKind::wheel, .x = body.x + 4, .y = body.y + 4, .wheel = 3}).consumed,
            "Read-only code did not consume wheel scrolling.");
    scroll.prepare();
    require(scroll.frame->find_code(1)->lines.front().row < before, "Read-only wheel did not reveal earlier lines.");
    const auto left = scroll.frame->find_code(1)->lines.front().x;
    require(
        scroll.ui.input({.kind = ui::EventKind::wheel, .x = body.x + 4, .y = body.y + 4, .wheel = -3, .shift = true})
            .consumed,
        "Read-only code did not consume horizontal scrolling.");
    scroll.prepare();
    require(scroll.frame->find_code(1)->lines.front().x < left,
            "Read-only horizontal wheel did not reveal long lines.");
    key(scroll.ui, ui::Key::home, true);
    scroll.prepare();
    const auto line = scroll.frame->find_code(1)->lines.front();
    const float y = line.y + scroll.frame->find_code(1)->line_height * .5f;
    scroll.ui.input({.kind = ui::EventKind::pointer_down, .x = line.x + 1, .y = y});
    scroll.ui.input({.kind = ui::EventKind::pointer_move, .x = line.x + 45, .y = y});
    scroll.ui.input({.kind = ui::EventKind::pointer_up, .x = line.x + 45, .y = y});
    require(!request_of<ui::ClipboardRequest>(key(scroll.ui, ui::Key::c, true)).text.empty(),
            "Read-only pointer selection could not be copied.");

    Consumer editable;
    auto source = leaf(1, ui::ControlKind::code_editor, 10);
    source.code = std::make_shared<ui::CodeDocument>(L"");
    editable.controls = {std::move(source)};
    editable.prepare();
    editable.ui.focus(1);
    editable.ui.input({.kind = ui::EventKind::text, .data = std::wstring{L"Default"}});
    key(editable.ui, ui::Key::enter);
    key(editable.ui, ui::Key::tab);
    require(editable.ui.text_input() && editable.controls[0].code->text() == L"Default\n    ",
            "The default CodeEditor no longer accepted text, newlines and indentation.");
}

void shared_edit_history_and_focus() {
    TextConsumer c(L"位置");
    c.command(ui::Key::a, true);
    require(c.ui.wants_keyboard() && !c.ui.interaction_state().active(),
            "Passive text focus blocks external operations");
    require(!c.command(ui::Key::z, true).consumed, "clean focus consumed scene undo");
    auto first = c.type(L"A");
    c.type(L"B");
    c.type(L"C");
    require(c.ui.interaction_state().text_draft, "Uncommitted field draft was not protected");
    require(actions_of(first).size() == 2 && c.value() == L"ABC", "text begin/preview chain failed");
    c.command(ui::Key::z, true);
    require(c.value() == L"位置", "typing was not one undo group");
    require(c.command(ui::Key::z, true).consumed && c.value() == L"位置", "exhausted local undo escaped to scene");
    c.command(ui::Key::y, true);
    require(c.value() == L"ABC", "local redo failed");
    const auto before = c.token();
    auto commit = c.command(ui::Key::enter);
    require(actions_of(commit).size() == 1 && actions_of(commit)[0].kind == ui::ActionKind::edit_commit &&
                c.ui.wants_keyboard() && c.token().generation != before.generation &&
                !c.command(ui::Key::z, true).consumed,
            "commit did not retain focus and end local history");
    require(!c.ui.interaction_state().active(), "Committed text focus remains an active interaction");
    c.controls[0].text = L"权威值";
    c.prepare();
    c.command(ui::Key::a, true);
    require(request_of<ui::ClipboardRequest>(c.command(ui::Key::c, true)).text == L"权威值",
            "passive buffer overwrote authority");
    c.type(L"预览");
    c.command(ui::Key::escape);
    require(c.value() == L"权威值" && c.ui.wants_keyboard(), "search cancel did not restore query and retain focus");
    require(!c.ui.interaction_state().active(), "Cancelled text focus remains an active interaction");

    TextConsumer bounded(L"");
    bounded.command(ui::Key::end);
    for (unsigned i = 0; i < 70; ++i) {
        bounded.type(L"a");
        bounded.command(ui::Key::end);
    }
    for (unsigned i = 0; i < 70; ++i)
        require(bounded.command(ui::Key::z, true).consumed, "bounded history escaped to scene");
    require(bounded.value().size() == 6, "64-group history bound was not applied");
    bounded.command(ui::Key::escape);
    require(bounded.value().empty(), "history eviction lost original");
    TextConsumer bytes(L"original");
    for (unsigned i = 0; i < 12; ++i) {
        bytes.command(ui::Key::a, true);
        bytes.type(std::wstring(4096, static_cast<wchar_t>(L'A' + i)));
    }
    for (unsigned i = 0; i < 12; ++i)
        bytes.command(ui::Key::z, true);
    require(bytes.value().size() == 4096, "history text byte budget was not enforced");
    bytes.command(ui::Key::escape);
    require(bytes.value() == L"original", "byte budget evicted cancellation original");

    TextConsumer returned(L"field");
    returned.controls.push_back(leaf(2, ui::ControlKind::button, 50, L"Menu"));
    returned.prepare();
    returned.apply(returned.ui.focus(2));
    require(!returned.ui.text_input(), "menu focus retained the text input binding");
    const auto restored = returned.apply(returned.ui.focus(1));
    require(returned.ui.text_input() && restored.effects.empty(),
            "returning text focus did not restore its binding before the next native key");
    const auto restored_token = returned.token();
    returned.compose({ui::CompositionKind::start, 60, restored_token, {}, {}});
    require(returned.ui.text_input()->composing, "returned focus rejected the first composition");
    require(returned.ui.interaction_state().composition, "Active IME was not protected");
    const auto refocused = returned.apply(returned.ui.focus(1));
    require(refocused.effects.empty() && returned.token() == restored_token && returned.ui.text_input()->composing,
            "same focus changed the live composition binding");
    returned.compose({ui::CompositionKind::end, 60, restored_token, std::wstring{L"位置"}, {}});
    require(returned.value() == L"field位置", "returned focus lost the first confirmed composition");
    returned.apply(returned.ui.focus(0));
    require(returned.ui.focused_control() == 0 && !returned.ui.text_input(),
            "explicit empty focus retained a control or text binding");

    TextConsumer stable(L"original");
    stable.command(ui::Key::a, true);
    stable.type(L"draft");
    stable.command(ui::Key::a, true);
    const auto draft_token = stable.token();
    require(stable.apply(stable.ui.focus(1)).effects.empty() && stable.token() == draft_token &&
                stable.value() == L"draft",
            "same focus committed or replaced the active draft");
    require(request_of<ui::ClipboardRequest>(stable.command(ui::Key::c, true)).text == L"draft",
            "same focus discarded the selection");
    require(stable.ui.focus(999).effects.empty() && stable.token() == draft_token,
            "unknown focus changed the active draft");
    auto disabled = leaf(2, ui::ControlKind::button, 50, L"Disabled");
    disabled.enabled = false;
    stable.controls.push_back(disabled);
    stable.prepare();
    require(stable.ui.focus(2).effects.empty() && stable.token() == draft_token,
            "disabled focus changed the active draft");
    stable.command(ui::Key::escape);
    require(stable.value() == L"original", "same focus ended draft rollback ownership");

    TextConsumer numeric(L"", ui::ControlKind::number);
    numeric.controls.push_back(leaf(2, ui::ControlKind::button, 50, L"Menu"));
    numeric.prepare();
    numeric.apply(numeric.ui.focus(2));
    require(numeric.apply(numeric.ui.focus(1)).effects.empty() && numeric.ui.text_input(),
            "numeric focus did not bind passively");
    require(numeric.command(ui::Key::enter).effects.empty(), "passive numeric focus committed without editing");
    numeric.type(L"7");
    require(numeric.controls[0].value == 7, "numeric focus did not preserve Enter-to-edit selection");
    TextConsumer clamped(L"", ui::ControlKind::number);
    clamped.controls[0].value = 4;
    clamped.controls[0].minimum = 0;
    clamped.controls[0].maximum = 8;
    clamped.prepare();
    clamped.command(ui::Key::a, true);
    clamped.type(L"12");
    const auto clamped_commit = actions_of(clamped.command(ui::Key::enter));
    require(clamped_commit.size() == 1 && clamped_commit[0].kind == ui::ActionKind::edit_commit &&
                clamped.controls[0].value == 8,
            "a typed number above the legal range did not clamp to its maximum");

    TextConsumer transferred(L"", ui::ControlKind::number);
    transferred.apply(transferred.ui.input({.kind = ui::EventKind::key_down, .key = ui::Key::enter}));
    transferred.apply(transferred.ui.cancel_interaction()); // The menu owns the old Enter release.
    transferred.apply(transferred.ui.input({.kind = ui::EventKind::key_down, .key = ui::Key::enter}));
    transferred.ui.input({.kind = ui::EventKind::key_up, .key = ui::Key::enter});
    transferred.type(L"7");
    require(transferred.controls[0].value == 7, "cancelled input owner swallowed the next Enter-to-edit");

    TextConsumer escaped(L"original");
    escaped.command(ui::Key::a, true);
    escaped.type(L"draft");
    escaped.apply(escaped.ui.input({.kind = ui::EventKind::key_down, .key = ui::Key::escape}));
    escaped.type(L"next");
    const auto repeated_escape =
        escaped.apply(escaped.ui.input({.kind = ui::EventKind::key_down, .key = ui::Key::escape}));
    require(repeated_escape.consumed && repeated_escape.effects.empty() && escaped.value() == L"originalnext",
            "current Escape repeat escaped its terminal latch");
    escaped.ui.input({.kind = ui::EventKind::key_up, .key = ui::Key::escape});
    escaped.command(ui::Key::escape);
    require(escaped.value() == L"original", "released Escape could not cancel a new draft");

    for (const auto kind : {ui::ControlKind::text_edit, ui::ControlKind::number, ui::ControlKind::slider})
        for (const auto activation : {ui::Key::space, ui::Key::enter})
            for (bool tab : {false, true}) {
                Consumer moved;
                moved.controls = {leaf(1, ui::ControlKind::button, 10, L"Activate"), leaf(2, kind, 50, L"Field")};
                moved.prepare();
                moved.ui.focus(1);
                moved.ui.input({.kind = ui::EventKind::key_down, .key = activation});
                moved.ui.focus(1);
                require(moved.ui.interaction_state().keyboard, "same focus retired a held activation");
                const auto transition = tab ? key(moved.ui, ui::Key::tab) : moved.ui.focus(2);
                require(transition.redraw && moved.ui.focused_control() == 2 && !moved.ui.interaction_state().keyboard,
                        "focus transfer retained the previous control's held activation");
                moved.ui.focus(1);
                require(actions_of(moved.ui.input({.kind = ui::EventKind::key_down, .key = activation})).empty() &&
                            !moved.ui.interaction_state().keyboard,
                        "held native repeat rearmed an activation after focus returned");
                const auto released = moved.ui.input({.kind = ui::EventKind::key_up, .key = activation});
                require(released.consumed && actions_of(released).empty() && !moved.ui.interaction_state().active(),
                        "retired activation release left the session busy or activated a control");
                moved.ui.input({.kind = ui::EventKind::key_down, .key = activation});
                moved.ui.input({.kind = ui::EventKind::key_down, .key = activation});
                const auto activated = actions_of(moved.ui.input({.kind = ui::EventKind::key_up, .key = activation}));
                require(activated.size() == 1 && activated[0].kind == ui::ActionKind::activate &&
                            activated[0].key == 1 &&
                            actions_of(moved.ui.input({.kind = ui::EventKind::key_up, .key = activation})).empty(),
                        "new activation after focus transfer did not complete exactly once");
            }
    Consumer cleared;
    cleared.controls = {leaf(1, ui::ControlKind::button, 10, L"Activate")};
    cleared.prepare();
    cleared.ui.focus(1);
    cleared.ui.input({.kind = ui::EventKind::key_down, .key = ui::Key::space});
    require(cleared.ui.focus(0).redraw && !cleared.ui.interaction_state().keyboard,
            "empty focus retained a keyboard activation");
    require(cleared.ui.input({.kind = ui::EventKind::key_up, .key = ui::Key::space}).consumed &&
                !cleared.ui.interaction_state().active(),
            "activation release required a focused control");
    cleared.ui.focus(1);
    cleared.ui.input({.kind = ui::EventKind::key_down, .key = ui::Key::enter});
    cleared.controls[0].layout.width = ui_layout::Axis::fixed(-1);
    accepted(cleared.ui.set_controls(cleared.controls));
    require(!cleared.ui.prepare(), "keyboard release fixture did not suspend input");
    const auto suspended = cleared.ui.input({.kind = ui::EventKind::key_up, .key = ui::Key::enter});
    require(suspended.consumed && suspended.redraw && actions_of(suspended).empty() &&
                !cleared.ui.interaction_state().keyboard,
            "failed frame preparation prevented keyboard release cleanup");
    TextConsumer repeated(L"");
    repeated.command(ui::Key::a, true);
    for (unsigned i = 0; i < 3; ++i) {
        repeated.ui.input({.kind = ui::EventKind::key_down, .key = ui::Key::space});
        repeated.type(L" ");
    }
    repeated.ui.input({.kind = ui::EventKind::key_up, .key = ui::Key::space});
    require(repeated.value() == L"   ", "keyboard ownership cleanup swallowed ordinary text repeats");
}

void unicode_visual_editing() {
    const std::wstring original = L"A\U0001f600e\u0301\U0001f469\u200d\U0001f4bb\u2764\ufe0fZ";
    TextConsumer c(original);
    c.command(ui::Key::end);
    const std::wstring remaining[] = {L"A\U0001f600e\u0301\U0001f469\u200d\U0001f4bb\u2764\ufe0f",
                                      L"A\U0001f600e\u0301\U0001f469\u200d\U0001f4bb",
                                      L"A\U0001f600e\u0301",
                                      L"A\U0001f600",
                                      L"A",
                                      L""};
    for (const auto& value : remaining) {
        c.command(ui::Key::backspace);
        require(c.value() == value, "Backspace split a grapheme");
    }
    c.command(ui::Key::z, true);
    require(c.value() == original, "grapheme delete group did not undo");
    auto rejected = c.type(std::wstring(1, static_cast<wchar_t>(0xd800)));
    require(rejected.error && c.value() == original, "unpaired surrogate modified text");
    require(c.type(L"line\nbreak").error.has_value() && c.value() == original,
            "multiline paste was partially inserted");

    TextConsumer bidi(L"abc אבג xyz");
    bidi.command(ui::Key::a, true);
    bidi.prepare();
    unsigned rectangles = 0;
    for (const auto& draw : bidi.frame->drawing->draws)
        if (!draw.coverage && draw.color == bidi.ui.style().palette.selection)
            ++rectangles;
    require(rectangles >= 3, "bidi selection collapsed into one logical x interval");
    bidi.command(ui::Key::end);
    bidi.prepare();
    float previous = bidi.frame->caret->bounds.x;
    unsigned moved = 0;
    for (unsigned i = 0; i < 20; ++i) {
        bidi.command(ui::Key::left);
        bidi.prepare();
        const float x = bidi.frame->caret->bounds.x;
        require(x <= previous + .01f, "Left did not follow visual ordering");
        if (x < previous - .01f)
            ++moved;
        previous = x;
    }
    require(moved >= 8, "visual caret did not traverse mixed text");
    const auto leftmost = bidi.frame->caret->bounds.x;
    bidi.command(ui::Key::a, true);
    bidi.command(ui::Key::left);
    bidi.prepare();
    require(std::abs(bidi.frame->caret->bounds.x - leftmost) < .01f, "Left did not collapse selection to visual start");
    const auto start = bidi.frame->caret->bounds;
    bidi.command(ui::Key::end);
    bidi.prepare();
    const auto end = bidi.frame->caret->bounds;
    bidi.apply(bidi.ui.input({.kind = ui::EventKind::pointer_down, .x = start.x, .y = start.y + 5}));
    require(bidi.ui.interaction_state().pointer, "Held pointer interaction was not protected");
    bidi.apply(bidi.ui.input({.kind = ui::EventKind::pointer_move, .x = end.x, .y = end.y + 5}));
    bidi.apply(bidi.ui.input({.kind = ui::EventKind::pointer_up, .x = end.x, .y = end.y + 5}));
    require(!request_of<ui::ClipboardRequest>(bidi.command(ui::Key::c, true)).text.empty(),
            "pointer text selection failed");

    TextConsumer scroll(std::wstring(4096, L'W'));
    scroll.controls[0].layout.width = ui_layout::Axis::fixed(100);
    scroll.command(ui::Key::end);
    scroll.prepare();
    const auto held = scroll.frame;
    require(held->caret && held->caret->bounds.x + held->caret->bounds.width <= held->caret->clip.right + .01f,
            "horizontal scrolling did not reveal caret");
    require(first_text(*held).coverage->width <= 100 && first_text(*held).coverage->pixels.size() <= 3000,
            "long field allocated offscreen coverage pixels");
    scroll.command(ui::Key::backspace);
    scroll.type(L"Z");
    require(!scroll.ui.text_input()->accepted_caret, "unaccepted text used an old caret token");
    scroll.prepare(false);
    require(!scroll.ui.text_input()->accepted_caret, "candidate caret leaked to platform");
    require(scroll.ui.accept(scroll.frame) && scroll.ui.text_input()->accepted_caret.has_value(),
            "accepted caret unavailable");
    require(held->caret->token != scroll.frame->caret->token, "old frame caret mutated");
}

void clipboard_roundtrips() {
    TextConsumer c(L"abcdef");
    c.command(ui::Key::a, true);
    const auto copy = request_of<ui::ClipboardRequest>(c.command(ui::Key::c, true));
    require(c.ui.interaction_state().clipboard, "Pending clipboard operation was not protected");
    require(copy.text == L"abcdef" && actions_of(c.receive(copy, true)).empty(), "copy changed draft");
    require(!c.ui.interaction_state().active(), "Completed copy kept passive focus busy");
    const auto failed = request_of<ui::ClipboardRequest>(c.command(ui::Key::x, true));
    require(c.receive(failed, false).error.has_value() && c.value() == L"abcdef", "failed cut deleted text");
    const auto cut = request_of<ui::ClipboardRequest>(c.command(ui::Key::x, true));
    require(actions_of(c.receive(cut, true)).size() == 2 && c.value().empty(),
            "successful cut did not use editing transaction");
    require(actions_of(c.receive(cut, true)).empty(), "duplicate cut result was applied");
    c.command(ui::Key::z, true);
    require(c.value() == L"abcdef", "cut undo failed");
    const auto stale = request_of<ui::ClipboardRequest>(c.command(ui::Key::v, true));
    c.command(ui::Key::home);
    require(actions_of(c.receive(stale, true, L"wrong")).empty() && c.value() == L"abcdef",
            "selection change accepted stale paste");
    c.command(ui::Key::a, true);
    const auto older = request_of<ui::ClipboardRequest>(c.command(ui::Key::v, true));
    const auto newer = request_of<ui::ClipboardRequest>(c.command(ui::Key::v, true));
    require(older.token == newer.token && older.operation_id != newer.operation_id,
            "requests did not have independent identity");
    c.receive(older, true, L"wrong");
    require(c.value() == L"abcdef", "same-revision old request matched new operation");
    c.receive(newer, true, L"中文\U0001f600");
    require(c.value() == L"中文\U0001f600", "Unicode paste failed");
    const auto cancelled = request_of<ui::ClipboardRequest>(c.command(ui::Key::v, true));
    c.command(ui::Key::escape);
    c.receive(cancelled, true, L"wrong");
    require(c.value() == L"abcdef", "old generation paste modified new context");
    const auto retired = request_of<ui::ClipboardRequest>(c.command(ui::Key::v, true));
    auto button = leaf(1, ui::ControlKind::button, 10, L"Changed kind");
    accepted(c.ui.set_controls(std::span<const ui::Control>(&button, 1)));
    require(!c.ui.input({.kind = ui::EventKind::pointer_down, .x = 20, .y = 20}).consumed,
            "old accepted field geometry was reinterpreted as a new control kind");
    require(actions_of(c.receive(retired, true, L"wrong")).empty(), "changed control accepted old clipboard result");

    TextConsumer numeric({}, ui::ControlKind::number);
    const auto paste = request_of<ui::ClipboardRequest>(numeric.command(ui::Key::v, true));
    const auto invalid = numeric.receive(paste, true, L"文字");
    require(actions_of(invalid).size() == 1 && actions_of(invalid)[0].kind == ui::ActionKind::edit_begin &&
                numeric.controls[0].value == 12,
            "invalid numeric paste reached business preview");
    require(actions_of(numeric.command(ui::Key::enter)).empty() && numeric.ui.wants_keyboard(),
            "invalid Enter lost focus or committed");
    require(numeric.ui.interaction_state().text_draft, "Invalid uncommitted number was treated as idle");
    numeric.command(ui::Key::escape);
    require(numeric.controls[0].value == 12 && !numeric.command(ui::Key::z, true).consumed,
            "numeric cancel did not return to passive focus");
    require(!numeric.ui.interaction_state().active(), "Cancelled number remained busy");
}

void composition_protocol_and_cleanup() {
    TextConsumer c(L"X");
    c.command(ui::Key::a, true);
    const auto initial = c.token();
    c.compose({ui::CompositionKind::start, 10, initial, {}, {}});
    c.compose({ui::CompositionKind::update, 10, initial, {}, ui::Preedit{L"wei", 3, {0, 0, 0}}});
    c.prepare();
    require(c.value() == L"X" && c.ui.text_input()->composing && c.frame->caret, "preedit leaked into search preview");
    require(c.command(ui::Key::z, true).consumed && c.value() == L"X", "IME undo escaped composition");
    auto confirm = c.ui.input({.kind = ui::EventKind::key_down, .key = ui::Key::enter});
    auto request = request_of<ui::ImeRequest>(confirm);
    require(request.kind == ui::ImeKind::complete && actions_of(confirm).empty(), "IME Enter submitted field");
    c.ui.input({.kind = ui::EventKind::ime_result,
                .data = ui::ImeResult{request.kind, request.composition_id, request.token, true, 0}});
    require(c.value() == L"X", "IME command success fabricated result text");
    c.compose({ui::CompositionKind::update, 10, initial, std::wstring{L"位"}, ui::Preedit{L"zhi", 3, {0, 1, 1}}});
    require(c.value() == L"位" && c.token() != initial && c.ui.text_input()->composing,
            "result+preedit failed to rebase segment");
    c.compose({ui::CompositionKind::update, 10, initial, std::wstring{L"wrong"}, {}});
    require(c.value() == L"位", "old segment token accepted");
    c.compose({ui::CompositionKind::update, 10, c.token(), std::wstring{L"置"}, {}});
    c.compose({ui::CompositionKind::end, 10, c.token(), {}, {}});
    require(c.value() == L"位置" && !c.ui.text_input()->composing, "confirmed result was duplicated or lost");
    require(actions_of(c.ui.input({.kind = ui::EventKind::key_down, .key = ui::Key::enter})).empty(),
            "IME terminating repeat committed field");
    c.ui.input({.kind = ui::EventKind::key_up, .key = ui::Key::enter});
    c.command(ui::Key::z, true);
    require(c.value() == L"位", "IME result was not one history unit");
    c.command(ui::Key::z, true);
    require(c.value() == L"X", "IME original replacement range was lost");
    c.command(ui::Key::y, true);
    const auto second = c.token();
    c.compose({ui::CompositionKind::start, 11, second, {}, {}});
    c.compose({ui::CompositionKind::update, 11, second, {}, ui::Preedit{L"next", 4, {}}});
    require(actions_of(c.command(ui::Key::escape)).empty() && c.value() == L"位",
            "IME Esc cancelled entire text transaction");
    c.compose({ui::CompositionKind::start, 11, second, {}, {}});
    c.compose({ui::CompositionKind::update, 11, second, std::wstring{L"late"}, {}});
    require(c.value() == L"位" && !c.ui.text_input()->composing, "cancelled composition was revived");
    c.command(ui::Key::escape);
    require(c.value() == L"X", "field Esc did not restore original after composition");
    c.command(ui::Key::a, true);
    c.type(L"draft");
    const auto third = c.token();
    c.compose({ui::CompositionKind::start, 12, third, {}, {}});
    c.compose({ui::CompositionKind::update, 12, third, {}, ui::Preedit{L"temp", 4, {}}});
    const auto cleanup = c.apply(accepted(c.ui.set_surface({{640, 480}, 1.5f, 2})));
    require(cleanup.effects.size() == 2 && std::holds_alternative<ui::PlatformRequest>(cleanup.effects[0]) &&
                std::get<ui::Action>(cleanup.effects[1]).kind == ui::ActionKind::edit_cancel && c.value() == L"X" &&
                !c.ui.text_input(),
            "surface cleanup lost IME cancellation, rollback or focus invalidation");
}

void accepted_input_anchor() {
    TextConsumer c(L"ABCDE");
    c.command(ui::Key::home);
    c.command(ui::Key::right, false, true);
    c.command(ui::Key::right, false, true);
    c.prepare();
    auto input = c.ui.text_input();
    require(input && input->accepted_anchor && input->accepted_caret, "accepted insertion geometry missing");
    const auto initial = *input->accepted_anchor;
    require(initial.composition_id == 0 && initial.composition_revision == 0 &&
                initial.bounds.x < input->accepted_caret->bounds.x && initial.clip == input->accepted_caret->clip,
            "forward selection used its display caret as replacement start");
    c.command(ui::Key::home);
    c.command(ui::Key::right);
    c.command(ui::Key::right);
    c.command(ui::Key::home, false, true);
    c.prepare();
    require(c.ui.text_input()->accepted_anchor && c.ui.text_input()->accepted_anchor->bounds == initial.bounds,
            "reverse selection changed the logical replacement start");

    const auto token = c.token();
    c.compose({ui::CompositionKind::start, 41, token, {}, {}});
    require(!c.ui.text_input()->accepted_anchor && !c.ui.text_input()->accepted_caret,
            "new composition reused unbound insertion geometry");
    c.compose({ui::CompositionKind::update, 41, token, {}, ui::Preedit{L"位置", 2, {0, 0}}});
    c.prepare(false);
    require(!c.ui.text_input()->accepted_anchor, "unaccepted composition exposed geometry");
    require(c.ui.accept(c.frame), "composition frame rejected");
    input = c.ui.text_input();
    require(input->accepted_anchor && input->accepted_anchor->composition_id == 41 && input->accepted_caret &&
                input->accepted_anchor->bounds.x < input->accepted_caret->bounds.x,
            "composition origin was replaced by preedit display cursor");
    const auto held = c.frame;
    const auto prior = *held->input_anchor;
    c.compose({ui::CompositionKind::update, 41, token, {}, ui::Preedit{L"位置", 0, {0, 0}}});
    require(c.token() == token && !c.ui.text_input()->accepted_anchor && !c.ui.text_input()->accepted_caret,
            "old preedit geometry survived a display revision change");
    c.prepare();
    require(c.ui.text_input()->accepted_anchor &&
                c.frame->input_anchor->composition_revision != prior.composition_revision &&
                held->input_anchor->bounds == prior.bounds &&
                held->input_anchor->composition_revision == prior.composition_revision,
            "composition geometry did not preserve immutable frame versions");
    c.compose({ui::CompositionKind::update, 41, token, {}, ui::Preedit{L"אב", 0, {0, 0}}});
    c.prepare();
    input = c.ui.text_input();
    require(input->accepted_anchor && input->accepted_caret && input->accepted_anchor->bounds.width > 0 &&
                std::abs(input->accepted_anchor->bounds.x + input->accepted_anchor->bounds.width -
                         input->accepted_caret->bounds.x) < .01f,
            "RTL character upper-left was confused with its leading caret edge");

    TextConsumer scrolled(std::wstring(100, L'W'));
    scrolled.controls[0].layout.width = ui_layout::Axis::fixed(100);
    scrolled.command(ui::Key::end);
    scrolled.prepare();
    const auto scroll_token = scrolled.token();
    scrolled.compose({ui::CompositionKind::start, 42, scroll_token, {}, {}});
    scrolled.compose({ui::CompositionKind::update, 42, scroll_token, {}, ui::Preedit{L"位置", 2, {}}});
    scrolled.prepare();
    input = scrolled.ui.text_input();
    require(input->accepted_anchor && input->accepted_anchor->bounds.x >= input->accepted_anchor->clip.left &&
                input->accepted_anchor->bounds.x < input->accepted_caret->bounds.x,
            "horizontal scroll did not transform composition origin");
    scrolled.compose(
        {ui::CompositionKind::update, 42, scroll_token, {}, ui::Preedit{std::wstring(200, L'W'), 200, {}}});
    scrolled.prepare();
    require(scrolled.frame->input_anchor &&
                scrolled.frame->input_anchor->bounds.x < scrolled.frame->input_anchor->clip.left &&
                !scrolled.ui.text_input()->accepted_anchor && scrolled.ui.text_input()->accepted_caret,
            "offscreen origin was clamped or replaced by a visible cursor");
    scrolled.compose({ui::CompositionKind::update, 42, scroll_token, {}, ui::Preedit{std::wstring(200, L'W'), 0, {}}});
    scrolled.prepare();
    require(scrolled.ui.text_input()->accepted_anchor.has_value(),
            "scrolling the composition start back did not restore the query");
    scrolled.apply(scrolled.ui.input({.kind = ui::EventKind::focus_lost}));
    require(!scrolled.ui.text_input(), "lost focus still published an input anchor");
    c.apply(accepted(c.ui.set_surface({{640, 480}, 1.5f, 2})));
    require(!c.ui.text_input(), "old surface retained input geometry");
    c.prepare();
    c.click(1);
    c.command(ui::Key::home);
    c.prepare();
    input = c.ui.text_input();
    require(input->accepted_anchor && input->accepted_anchor->composition_id == 0 &&
                std::abs(input->accepted_anchor->bounds.height - initial.bounds.height * 1.5f) < .01f,
            "input anchor did not use the accepted frame DPI");
}
