    static void action(Reply& reply, ActionKind kind, ControlKey key,
                       ActionValue value = {}, ControlKey target = 0, bool keep_menu_open = false) {
        reply.effects.emplace_back(Action{kind,key,std::move(value),target,keep_menu_open});
    }
    static void platform(Reply& reply, PlatformRequest request) { reply.effects.emplace_back(std::move(request)); }
    void begin_edit(ControlKey key, bool mode, bool select_all = false) {
        const auto& state=states.at(key);
        edit.emplace(state,state.control.kind==ControlKind::number?format(state.control.value,state.control.single_precision):state.control.text,
            state.control.value,mode,select_all);
    }
    void revise() {
        edit->token.revision=detail::new_identity();
        edit->clipboard.reset();
        if (edit->document) edit->document_selection=edit->buffer().selection_revision();
    }
    bool valid_edit() const {
        return edit && (states.at(edit->key).control.kind!=ControlKind::number ||
            number(edit->buffer().current().text,states.at(edit->key).control).has_value());
    }
    void cancel_composition(Reply& reply) {
        if (!edit || !edit->composition) return;
        const auto composition=*edit->composition;
        edit->ime_request=ImeRequest{ImeKind::cancel,composition.id,composition.token};
        platform(reply,*edit->ime_request);
        edit->composition.reset(); edit->buffer().break_group(); reply.redraw=true;
        revise(); // Ending the display/selection binding invalidates late starts too.
    }
    bool finish_edit(Reply& reply, bool commit, bool keep_context = false, bool keep_invalid = false) {
        if (!edit) return true;
        if (commit && keep_invalid && !valid_edit()) { reply.redraw=true; return false; }
        cancel_composition(reply);
        const auto key=edit->key;
        const auto& state=states.at(key);
        if (edit->document) {
            edit->buffer().break_group();edit.reset();
            if (keep_context && focus==key) begin_edit(key,true);
            reply.redraw=true;return true;
        }
        const bool numeric=state.control.kind==ControlKind::number;
        const auto parsed=numeric?number(edit->buffer().current().text,state.control):std::optional<double>{};
        commit=commit && (!numeric || parsed.has_value());
        const auto value=numeric?(commit?*parsed:edit->original_value):0.0;
        auto content=commit?edit->buffer().current().text:edit->buffer().original();
        if (edit->buffer().active()) action(reply,commit?ActionKind::edit_commit:ActionKind::edit_cancel,key,
            numeric?ActionValue{value}:
                ActionValue{content});
        edit.reset();
        if (keep_context && focus==key) edit.emplace(state,numeric?format(value,state.control.single_precision):std::move(content),value,!numeric,false);
        reply.redraw=true; return true;
    }
    Reply cancel() {
        Reply reply;
        reply.redraw=dismiss_tooltip();
        reply.cursor=Cursor::arrow;
        if (press) {
            const auto old=*press;
            press.reset(); reply.capture=Capture::release; reply.redraw=true;
            if (old.dragging && !old.scrollbar) {
                if (states.at(old.key).control.kind==ControlKind::tab || states.at(old.key).control.kind==ControlKind::tree_item)
                    action(reply,ActionKind::drag_cancel,old.key,DragPoint{old.last_x,old.last_y});
                else action(reply,ActionKind::edit_cancel,old.key,old.original);
                if (focus==old.key && states.at(old.key).control.kind==ControlKind::number)
                    edit.emplace(states.at(old.key),format(old.original,states.at(old.key).control.single_precision),old.original,false,false);
            }
        }
        finish_edit(reply,false,true); keyboard_pressed=0; keyboard_key=terminal_key=Key::none; hover=0; dismiss_popup(reply);
        return reply;
    }
    void changed(Reply& reply, detail::TextEdit::Change change, bool was_active) {
        if (change==detail::TextEdit::Change::rejected) {
            reply.error=Error{ErrorCode::invalid_text,edit->key}; return;
        }
        if (change==detail::TextEdit::Change::none) return;
        revise(); reply.redraw=true;
        if (change!=detail::TextEdit::Change::text) return;
        if (edit->document) { action(reply,ActionKind::edit_preview,edit->key);return; }
        const auto& control=states.at(edit->key).control;
        const bool numeric=control.kind==ControlKind::number;
        if (!was_active) action(reply,ActionKind::edit_begin,edit->key,
            numeric?ActionValue{edit->original_value}:
                ActionValue{edit->buffer().original()});
        if (numeric) {
            const auto value=number(edit->buffer().current().text,control);
            if (value && *value!=edit->preview_value) {
                action(reply,ActionKind::edit_preview,edit->key,*value); edit->preview_value=*value;
            }
        } else action(reply,ActionKind::edit_preview,edit->key,edit->buffer().current().text);
    }
    void insert(Reply& reply, std::wstring_view value, detail::TextEdit::Group group) {
        std::wstring normalized;
        if (edit->document) {
            normalized.reserve(value.size());
            for (std::size_t i=0;i<value.size();++i) {
                if (value[i]==L'\r') {normalized+=L'\n';if (i+1<value.size() && value[i+1]==L'\n') ++i;}
                else if ((value[i]<32 && value[i]!=L'\n' && value[i]!=L'\t') || value[i]==127) {reply.error=Error{ErrorCode::invalid_text,edit->key};return;}
                else normalized+=value[i];
            }
            value=normalized;
        }
        const bool was_active=edit->buffer().active();
        const auto change=edit->buffer().replace(value,group);
        changed(reply,change,was_active);
    }
    std::wstring display(const State& state) const {
        if (press && press->key==state.control.key && press->dragging) return format(press->value,state.control.single_precision);
        if (edit && edit->key==state.control.key) {
            auto value=edit->buffer().current().text;
            if (edit->composition) {
                const auto& c=*edit->composition;
                value.replace(c.first,c.last-c.first,c.preedit.text);
            }
            return value;
        }
        return state.control.kind==ControlKind::number?format(state.control.value,state.control.single_precision):state.control.text;
    }
    detail::TextPosition display_caret() const {
        if (!edit->composition) return edit->buffer().current().caret;
        const auto& c=*edit->composition;
        const auto boundaries=detail::character_boundaries(c.preedit.text);
        const auto at=std::lower_bound(boundaries.begin(),boundaries.end(),std::min<std::uint32_t>(c.preedit.cursor,static_cast<std::uint32_t>(c.preedit.text.size())));
        return {c.first+*at,*at!=0};
    }
    const TextGeometry* edit_geometry() const {
        if (!edit || !active()) return nullptr;
        const auto* box=published->find(edit->key);
        const auto* geometry=published->find_text(edit->key);
        if (!box || !current(*box) || !geometry || geometry->incarnation!=edit->token.incarnation || geometry->composition_id)
            return nullptr;
        return geometry;
    }
    void select_at(Reply& reply, float x, float y, bool extend) {
        if (edit->composition) { cancel_composition(reply); return; }
        if (edit->document) {select_code_at(reply,x,y,extend);return;}
        const auto* geometry=edit_geometry();
        if (!geometry || !geometry->paragraph->matches(edit->buffer().current().text)) { reply.redraw=true; return; }
        const auto position=geometry->paragraph->hit((x-geometry->x)/geometry->dpi_scale,(y-geometry->y)/geometry->dpi_scale);
        if (edit->buffer().select(position,extend)) { revise(); reply.redraw=true; }
    }
    void clipboard_command(Reply& reply, ClipboardKind kind) {
        edit->buffer().break_group(); reply.redraw|=!edit->text_mode; edit->text_mode=true;
        if (edit->composition) return;
        const auto& snapshot=edit->buffer().current();
        const auto first=std::min(snapshot.caret.offset,snapshot.anchor.offset),last=std::max(snapshot.caret.offset,snapshot.anchor.offset);
        if (kind!=ClipboardKind::read && first==last) return;
        edit->clipboard=ClipboardRequest{kind,detail::new_identity(),edit->token,
            kind==ClipboardKind::read?std::wstring{}:snapshot.text.substr(first,last-first),edit->document?1024u*1024:4096u};
        platform(reply,*edit->clipboard);
    }
    void clipboard_result(Reply& reply, const ClipboardResult& result) {
        if (!edit || !edit->clipboard) return;
        const auto& request=*edit->clipboard;
        if (request.operation_id!=result.operation_id || request.kind!=result.kind ||
            request.token!=result.token || edit->token!=result.token) return;
        edit->clipboard.reset(); reply.consumed=true;
        if (!result.success) { edit->commit_paste=false; reply.error=Error{ErrorCode::platform_failed,edit->key,result.native_error}; return; }
        if (result.kind!=ClipboardKind::copy) insert(reply,result.kind==ClipboardKind::cut?std::wstring_view{}:result.text,detail::TextEdit::Group::isolated);
        if (edit && edit->commit_paste && result.kind==ClipboardKind::read) {
            edit->commit_paste=false;
            finish_edit(reply,true,true,true);
        }
    }
    void ime_result(Reply& reply, const ImeResult& result) {
        if (!edit || !edit->ime_request) return;
        const auto& request=*edit->ime_request;
        if (request.composition_id!=result.composition_id || request.kind!=result.kind || request.token!=result.token) return;
        edit->ime_request.reset(); reply.consumed=true;
        if (!result.success) reply.error=Error{ErrorCode::platform_failed,edit->key,result.native_error};
    }
    void composition_event(Reply& reply, const CompositionEvent& event) {
        if (!edit || !event.composition_id) return;
        if (event.kind==CompositionKind::start) {
            if (event.token!=edit->token || edit->composition) return;
            edit->text_mode=true; edit->buffer().break_group(); edit->clipboard.reset();
            const auto& s=edit->buffer().current();
            edit->composition=Edit::Composition{event.composition_id,event.token,
                std::min(s.caret.offset,s.anchor.offset),std::max(s.caret.offset,s.anchor.offset),{}};
        }
        if (!edit->composition || edit->composition->id!=event.composition_id || edit->composition->token!=event.token) return;
        reply.consumed=reply.redraw=true;
        if (event.kind==CompositionKind::cancel) {
            edit->composition.reset(); edit->ime_request.reset(); revise(); return;
        }
        if (event.result) {
            insert(reply,*event.result,detail::TextEdit::Group::isolated);
            if (reply.error) { cancel_composition(reply); return; }
            auto& c=*edit->composition;
            c.token=edit->token; c.first=c.last=edit->buffer().current().caret.offset; c.preedit={};
            c.revision=detail::new_identity();
        }
        if (event.preedit) {
            const auto& value=*event.preedit;
            const auto& c=*edit->composition;
            const auto limit=edit->document?1024u*1024:states.at(edit->key).control.kind==ControlKind::number?64u:4096u;
            if (!detail::valid_text(value.text) || edit->buffer().current().text.size()-(c.last-c.first)+value.text.size()>limit ||
                value.cursor>value.text.size() || (!value.attributes.empty() && value.attributes.size()!=value.text.size())) {
                reply.error=Error{ErrorCode::invalid_text,edit->key}; cancel_composition(reply); return;
            }
            auto& current=*edit->composition;
            if (current.preedit.text!=value.text || current.preedit.cursor!=value.cursor || current.preedit.attributes!=value.attributes) {
                current.preedit=value; current.revision=detail::new_identity();
            }
        }
        if (event.kind==CompositionKind::end) { edit->composition.reset(); edit->ime_request.reset(); revise(); }
    }
