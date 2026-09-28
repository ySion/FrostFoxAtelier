// Production Session + DirectWrite/ICU consumer. Platform results below are
// injected protocol cases, not evidence that a native IME has run.
template<class T> T request_of(const ui::Reply& reply) {
    for (const auto& effect:reply.effects) if (const auto* platform=std::get_if<ui::PlatformRequest>(&effect))
        if (const auto* value=std::get_if<T>(platform)) return *value;
    throw std::runtime_error("expected platform request missing");
}
struct TextConsumer : Consumer {
    explicit TextConsumer(std::wstring initial, ui::ControlKind kind=ui::ControlKind::text_edit) {
        controls={leaf(1,kind,10,std::move(initial))}; controls[0].value=12;
        prepare(); click(1);
    }
    ui::Reply apply(ui::Reply reply) {
        for (const auto& action:actions_of(reply)) if (action.key==1 && (action.kind==ui::ActionKind::edit_preview ||
            action.kind==ui::ActionKind::edit_commit || action.kind==ui::ActionKind::edit_cancel)) {
            if (const auto* text=std::get_if<std::wstring>(&action.value)) controls[0].text=*text;
            if (const auto* value=std::get_if<double>(&action.value)) controls[0].value=*value;
        }
        return reply;
    }
    ui::Reply command(ui::Key value, bool control=false, bool shift=false) { return apply(key(ui,value,control,shift)); }
    ui::Reply type(std::wstring text) { return apply(ui.input({.kind=ui::EventKind::text,.data=std::move(text)})); }
    ui::Reply receive(ui::ClipboardRequest request, bool success, std::wstring text={}) {
        return apply(ui.input({.kind=ui::EventKind::clipboard_result,.data=ui::ClipboardResult{
            request.kind,request.operation_id,request.token,success,success?0u:5u,std::move(text)}}));
    }
    ui::Reply compose(ui::CompositionEvent event) { return apply(ui.input({.kind=ui::EventKind::composition,.data=std::move(event)})); }
    ui::EditToken token() const { const auto value=ui.text_input(); require(value.has_value(),"text binding missing"); return value->token; }
    const std::wstring& value() const { return controls[0].text; }
};
void shared_edit_history_and_focus() {
    TextConsumer c(L"位置"); c.command(ui::Key::a,true);
    require(!c.command(ui::Key::z,true).consumed,"clean focus consumed scene undo");
    auto first=c.type(L"A"); c.type(L"B"); c.type(L"C");
    require(actions_of(first).size()==2 && c.value()==L"ABC","text begin/preview chain failed");
    c.command(ui::Key::z,true); require(c.value()==L"位置","typing was not one undo group");
    require(c.command(ui::Key::z,true).consumed && c.value()==L"位置","exhausted local undo escaped to scene");
    c.command(ui::Key::y,true); require(c.value()==L"ABC","local redo failed");
    const auto before=c.token(); auto commit=c.command(ui::Key::enter);
    require(actions_of(commit).size()==1 && actions_of(commit)[0].kind==ui::ActionKind::edit_commit &&
        c.ui.wants_keyboard() && c.token().generation!=before.generation && !c.command(ui::Key::z,true).consumed,
        "commit did not retain focus and end local history");
    c.controls[0].text=L"权威值"; c.prepare(); c.command(ui::Key::a,true);
    require(request_of<ui::ClipboardRequest>(c.command(ui::Key::c,true)).text==L"权威值","passive buffer overwrote authority");
    c.type(L"预览"); c.command(ui::Key::escape);
    require(c.value()==L"权威值" && c.ui.wants_keyboard(),"search cancel did not restore query and retain focus");

    TextConsumer bounded(L""); bounded.command(ui::Key::end);
    for (unsigned i=0;i<70;++i) { bounded.type(L"a"); bounded.command(ui::Key::end); }
    for (unsigned i=0;i<70;++i) require(bounded.command(ui::Key::z,true).consumed,"bounded history escaped to scene");
    require(bounded.value().size()==6,"64-group history bound was not applied");
    bounded.command(ui::Key::escape); require(bounded.value().empty(),"history eviction lost original");
    TextConsumer bytes(L"original");
    for (unsigned i=0;i<12;++i) { bytes.command(ui::Key::a,true); bytes.type(std::wstring(4096,static_cast<wchar_t>(L'A'+i))); }
    for (unsigned i=0;i<12;++i) bytes.command(ui::Key::z,true);
    require(bytes.value().size()==4096,"history text byte budget was not enforced");
    bytes.command(ui::Key::escape); require(bytes.value()==L"original","byte budget evicted cancellation original");
}
void unicode_visual_editing() {
    const std::wstring original=L"A\U0001f600e\u0301\U0001f469\u200d\U0001f4bb\u2764\ufe0fZ";
    TextConsumer c(original); c.command(ui::Key::end);
    const std::wstring remaining[]={L"A\U0001f600e\u0301\U0001f469\u200d\U0001f4bb\u2764\ufe0f",
        L"A\U0001f600e\u0301\U0001f469\u200d\U0001f4bb",L"A\U0001f600e\u0301",L"A\U0001f600",L"A",L""};
    for (const auto& value:remaining) { c.command(ui::Key::backspace); require(c.value()==value,"Backspace split a grapheme"); }
    c.command(ui::Key::z,true); require(c.value()==original,"grapheme delete group did not undo");
    auto rejected=c.type(std::wstring(1,static_cast<wchar_t>(0xd800)));
    require(rejected.error && c.value()==original,"unpaired surrogate modified text");
    require(c.type(L"line\nbreak").error.has_value() && c.value()==original,"multiline paste was partially inserted");

    TextConsumer bidi(L"abc אבג xyz"); bidi.command(ui::Key::a,true); bidi.prepare();
    unsigned rectangles=0;
    for (const auto& draw:bidi.frame->drawing->draws) if (!draw.coverage && draw.color==bidi.ui.style().palette.selection) ++rectangles;
    require(rectangles>=3,"bidi selection collapsed into one logical x interval");
    bidi.command(ui::Key::end); bidi.prepare(); float previous=bidi.frame->caret->bounds.x;
    unsigned moved=0;
    for (unsigned i=0;i<20;++i) {
        bidi.command(ui::Key::left); bidi.prepare(); const float x=bidi.frame->caret->bounds.x;
        require(x<=previous+.01f,"Left did not follow visual ordering"); if (x<previous-.01f) ++moved; previous=x;
    }
    require(moved>=8,"visual caret did not traverse mixed text");
    const auto leftmost=bidi.frame->caret->bounds.x;
    bidi.command(ui::Key::a,true); bidi.command(ui::Key::left); bidi.prepare();
    require(std::abs(bidi.frame->caret->bounds.x-leftmost)<.01f,"Left did not collapse selection to visual start");
    const auto start=bidi.frame->caret->bounds;
    bidi.command(ui::Key::end); bidi.prepare(); const auto end=bidi.frame->caret->bounds;
    bidi.apply(bidi.ui.input({.kind=ui::EventKind::pointer_down,.x=start.x,.y=start.y+5}));
    bidi.apply(bidi.ui.input({.kind=ui::EventKind::pointer_move,.x=end.x,.y=end.y+5}));
    bidi.apply(bidi.ui.input({.kind=ui::EventKind::pointer_up,.x=end.x,.y=end.y+5}));
    require(!request_of<ui::ClipboardRequest>(bidi.command(ui::Key::c,true)).text.empty(),"pointer text selection failed");

    TextConsumer scroll(std::wstring(4096,L'W')); scroll.controls[0].layout.width=ui_layout::Axis::fixed(100);
    scroll.command(ui::Key::end); scroll.prepare();
    const auto held=scroll.frame;
    require(held->caret && held->caret->bounds.x+held->caret->bounds.width<=held->caret->clip.right+.01f,
        "horizontal scrolling did not reveal caret");
    require(first_text(*held).coverage->width<=100 && first_text(*held).coverage->pixels.size()<=3000,
        "long field allocated offscreen coverage pixels");
    scroll.command(ui::Key::backspace);
    scroll.type(L"Z"); require(!scroll.ui.text_input()->accepted_caret,"unaccepted text used an old caret token");
    scroll.prepare(false); require(!scroll.ui.text_input()->accepted_caret,"candidate caret leaked to platform");
    require(scroll.ui.accept(scroll.frame) && scroll.ui.text_input()->accepted_caret.has_value(),"accepted caret unavailable");
    require(held->caret->token!=scroll.frame->caret->token,"old frame caret mutated");
}
void clipboard_roundtrips() {
    TextConsumer c(L"abcdef"); c.command(ui::Key::a,true);
    const auto copy=request_of<ui::ClipboardRequest>(c.command(ui::Key::c,true));
    require(copy.text==L"abcdef" && actions_of(c.receive(copy,true)).empty(),"copy changed draft");
    const auto failed=request_of<ui::ClipboardRequest>(c.command(ui::Key::x,true));
    require(c.receive(failed,false).error.has_value() && c.value()==L"abcdef","failed cut deleted text");
    const auto cut=request_of<ui::ClipboardRequest>(c.command(ui::Key::x,true));
    require(actions_of(c.receive(cut,true)).size()==2 && c.value().empty(),"successful cut did not use editing transaction");
    require(actions_of(c.receive(cut,true)).empty(),"duplicate cut result was applied");
    c.command(ui::Key::z,true); require(c.value()==L"abcdef","cut undo failed");
    const auto stale=request_of<ui::ClipboardRequest>(c.command(ui::Key::v,true)); c.command(ui::Key::home);
    require(actions_of(c.receive(stale,true,L"wrong")).empty() && c.value()==L"abcdef","selection change accepted stale paste");
    c.command(ui::Key::a,true);
    const auto older=request_of<ui::ClipboardRequest>(c.command(ui::Key::v,true));
    const auto newer=request_of<ui::ClipboardRequest>(c.command(ui::Key::v,true));
    require(older.token==newer.token && older.operation_id!=newer.operation_id,"requests did not have independent identity");
    c.receive(older,true,L"wrong"); require(c.value()==L"abcdef","same-revision old request matched new operation");
    c.receive(newer,true,L"中文\U0001f600"); require(c.value()==L"中文\U0001f600","Unicode paste failed");
    const auto cancelled=request_of<ui::ClipboardRequest>(c.command(ui::Key::v,true)); c.command(ui::Key::escape);
    c.receive(cancelled,true,L"wrong"); require(c.value()==L"abcdef","old generation paste modified new context");
    const auto retired=request_of<ui::ClipboardRequest>(c.command(ui::Key::v,true));
    auto button=leaf(1,ui::ControlKind::button,10,L"Changed kind");
    accepted(c.ui.set_controls(std::span<const ui::Control>(&button,1)));
    require(!c.ui.input({.kind=ui::EventKind::pointer_down,.x=20,.y=20}).consumed,
        "old accepted field geometry was reinterpreted as a new control kind");
    require(actions_of(c.receive(retired,true,L"wrong")).empty(),"changed control accepted old clipboard result");

    TextConsumer numeric({},ui::ControlKind::number);
    const auto paste=request_of<ui::ClipboardRequest>(numeric.command(ui::Key::v,true));
    const auto invalid=numeric.receive(paste,true,L"文字");
    require(actions_of(invalid).size()==1 && actions_of(invalid)[0].kind==ui::ActionKind::edit_begin && numeric.controls[0].value==12,
        "invalid numeric paste reached business preview");
    require(actions_of(numeric.command(ui::Key::enter)).empty() && numeric.ui.wants_keyboard(),"invalid Enter lost focus or committed");
    numeric.command(ui::Key::escape); require(numeric.controls[0].value==12 && !numeric.command(ui::Key::z,true).consumed,
        "numeric cancel did not return to passive focus");
}
void composition_protocol_and_cleanup() {
    TextConsumer c(L"X"); c.command(ui::Key::a,true);
    const auto initial=c.token();
    c.compose({ui::CompositionKind::start,10,initial,{},{}});
    c.compose({ui::CompositionKind::update,10,initial,{},ui::Preedit{L"wei",3,{0,0,0}}}); c.prepare();
    require(c.value()==L"X" && c.ui.text_input()->composing && c.frame->caret,"preedit leaked into search preview");
    require(c.command(ui::Key::z,true).consumed && c.value()==L"X","IME undo escaped composition");
    auto confirm=c.ui.input({.kind=ui::EventKind::key_down,.key=ui::Key::enter});
    auto request=request_of<ui::ImeRequest>(confirm);
    require(request.kind==ui::ImeKind::complete && actions_of(confirm).empty(),"IME Enter submitted field");
    c.ui.input({.kind=ui::EventKind::ime_result,.data=ui::ImeResult{request.kind,request.composition_id,request.token,true,0}});
    require(c.value()==L"X","IME command success fabricated result text");
    c.compose({ui::CompositionKind::update,10,initial,std::wstring{L"位"},ui::Preedit{L"zhi",3,{0,1,1}}});
    require(c.value()==L"位" && c.token()!=initial && c.ui.text_input()->composing,"result+preedit failed to rebase segment");
    c.compose({ui::CompositionKind::update,10,initial,std::wstring{L"wrong"},{}});
    require(c.value()==L"位","old segment token accepted");
    c.compose({ui::CompositionKind::update,10,c.token(),std::wstring{L"置"},{}});
    c.compose({ui::CompositionKind::end,10,c.token(),{}, {}});
    require(c.value()==L"位置" && !c.ui.text_input()->composing,"confirmed result was duplicated or lost");
    require(actions_of(c.ui.input({.kind=ui::EventKind::key_down,.key=ui::Key::enter})).empty(),"IME terminating repeat committed field");
    c.ui.input({.kind=ui::EventKind::key_up,.key=ui::Key::enter});
    c.command(ui::Key::z,true); require(c.value()==L"位","IME result was not one history unit");
    c.command(ui::Key::z,true); require(c.value()==L"X","IME original replacement range was lost");
    c.command(ui::Key::y,true);
    const auto second=c.token(); c.compose({ui::CompositionKind::start,11,second,{},{}});
    c.compose({ui::CompositionKind::update,11,second,{},ui::Preedit{L"next",4,{}}});
    require(actions_of(c.command(ui::Key::escape)).empty() && c.value()==L"位","IME Esc cancelled entire text transaction");
    c.compose({ui::CompositionKind::start,11,second,{},{}});
    c.compose({ui::CompositionKind::update,11,second,std::wstring{L"late"},{}});
    require(c.value()==L"位" && !c.ui.text_input()->composing,"cancelled composition was revived");
    c.command(ui::Key::escape); require(c.value()==L"X","field Esc did not restore original after composition");
    c.command(ui::Key::a,true); c.type(L"draft");
    const auto third=c.token(); c.compose({ui::CompositionKind::start,12,third,{},{}});
    c.compose({ui::CompositionKind::update,12,third,{},ui::Preedit{L"temp",4,{}}});
    const auto cleanup=c.apply(accepted(c.ui.set_surface({{640,480},1.5f,2})));
    require(cleanup.effects.size()==2 && std::holds_alternative<ui::PlatformRequest>(cleanup.effects[0]) &&
        std::get<ui::Action>(cleanup.effects[1]).kind==ui::ActionKind::edit_cancel && c.value()==L"X" && !c.ui.text_input(),
        "surface cleanup lost IME cancellation, rollback or focus invalidation");
}
