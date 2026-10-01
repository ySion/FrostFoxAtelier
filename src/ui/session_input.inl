Reply Session::input(const Event& event) {
    auto& self=*impl_;
    Reply reply;
    try {
        if (event.kind==EventKind::capture_lost && !self.press) return reply;
        if (event.kind==EventKind::focus_lost || event.kind==EventKind::capture_lost) {
            reply=self.cancel();
            if (event.kind==EventKind::focus_lost) { self.edit.reset(); self.focus=0; self.terminal_key=Key::none; }
            return reply;
        }
        if (event.kind==EventKind::clipboard_result) {
            if (const auto* value=std::get_if<ClipboardResult>(&event.data)) self.clipboard_result(reply,*value);
            return reply;
        }
        if (event.kind==EventKind::ime_result) {
            if (const auto* value=std::get_if<ImeResult>(&event.data)) self.ime_result(reply,*value);
            return reply;
        }
        if (!self.active()) return reply;
        if (!std::isfinite(event.x) || !std::isfinite(event.y) || !std::isfinite(event.wheel)) return reply;
        if (event.kind==EventKind::key_up && event.key==self.terminal_key && event.key!=Key::none) {
            self.terminal_key=Key::none; reply.consumed=true; return reply;
        }
        if (event.kind==EventKind::key_down && event.key==self.terminal_key && event.key!=Key::none) {
            reply.consumed=true; return reply;
        }
        if (event.kind==EventKind::composition) {
            if (const auto* value=std::get_if<CompositionEvent>(&event.data)) self.composition_event(reply,*value);
            return reply;
        }
        if (self.edit && self.edit->composition && (event.kind==EventKind::key_down || event.kind==EventKind::key_up || event.kind==EventKind::text)) {
            reply.consumed=true;
            if (event.kind==EventKind::key_down && (event.key==Key::enter || event.key==Key::escape)) {
                self.terminal_key=event.key;
                if (event.key==Key::escape) self.cancel_composition(reply);
                else {
                    const auto& c=*self.edit->composition;
                    self.edit->ime_request=ImeRequest{ImeKind::complete,c.id,c.token};
                    Impl::platform(reply,*self.edit->ime_request);
                }
            } else if (event.kind==EventKind::key_down && event.key==Key::tab) {
                self.cancel_composition(reply); // Continue the ordinary focus transition below.
            } else return reply;
            if (event.key!=Key::tab) return reply;
        }
        const auto* hit=self.hit(event.x,event.y);
        if (event.kind==EventKind::pointer_move) {
            const auto key=hit?hit->key:0;
            reply.redraw=self.hover!=key; self.hover=key;
            if (self.press) {
                auto& press=*self.press;
                const auto* state=self.live(press.key);
                if (!state) return self.cancel();
                const auto& control=state->control;
                if (press.selecting && self.edit && self.edit->key==press.key) self.select_at(reply,event.x,event.y,true);
                else if (editable(control.kind)) {
                    const auto delta=control.drag_axis==DragAxis::horizontal?event.x-press.last_x:event.y-press.last_y;
                    if (!press.dragging && (std::abs(event.x-press.x)>3 || std::abs(event.y-press.y)>3)) {
                        self.finish_edit(reply,true); press.dragging=true;
                        Impl::action(reply,ActionKind::edit_begin,press.key,press.original);
                    }
                    if (press.dragging) {
                        press.value=std::clamp(press.value+delta/self.surface.dpi_scale*(event.shift?control.fine_step:control.drag_step),control.minimum,control.maximum);
                        Impl::action(reply,ActionKind::edit_preview,press.key,press.value); reply.redraw=true;
                    }
                }
                press.last_x=event.x; press.last_y=event.y; reply.consumed=true;
            } else reply.consumed=hit && hit->kind!=ControlKind::viewport;
            if (hit) {
                const auto& control=self.states.at(hit->key).control;
                if (control.kind==ControlKind::text_edit || (control.kind==ControlKind::number && self.edit && self.edit->key==hit->key && self.edit->text_mode)) reply.cursor=Cursor::text;
                else if (control.kind==ControlKind::number) reply.cursor=Cursor::drag_horizontal;
                else if (control.kind==ControlKind::splitter) reply.cursor=control.drag_axis==DragAxis::horizontal?Cursor::resize_horizontal:Cursor::resize_vertical;
            }
            return reply;
        }
        if (event.kind==EventKind::pointer_down) {
            reply.consumed=hit && hit->kind!=ControlKind::viewport;
            if (event.button!=PointerButton::primary || self.press) return reply;
            self.keyboard_pressed=0;
            const auto next=hit && focusable(hit->kind)?hit->key:0;
            if (next!=self.focus) { self.finish_edit(reply,true); self.focus=next; }
            if (!hit || (!focusable(hit->kind) && hit->kind!=ControlKind::splitter)) return reply;
            const auto& control=self.states.at(hit->key).control;
            if (text_field(control.kind) && (!self.edit || self.edit->key!=hit->key))
                self.begin_edit(hit->key,control.kind==ControlKind::text_edit);
            const bool selecting=self.edit && self.edit->key==hit->key && self.edit->text_mode;
            if (self.edit) self.edit->buffer.break_group();
            if (selecting) self.select_at(reply,event.x,event.y,event.shift);
            self.press=Impl::Press{hit->key,event.x,event.y,event.x,event.y,control.value,control.value,false,selecting};
            reply.capture=Capture::acquire; reply.redraw=true; return reply;
        }
        if (event.kind==EventKind::pointer_up) {
            if (event.button!=PointerButton::primary || !self.press) return reply;
            const auto press=*self.press; self.press.reset();
            reply.consumed=reply.redraw=true; reply.capture=Capture::release;
            if (press.dragging) {
                Impl::action(reply,ActionKind::edit_commit,press.key,press.value);
                if (self.focus==press.key && self.states.at(press.key).control.kind==ControlKind::number)
                    self.edit.emplace(self.states.at(press.key),format(press.value),press.value,false,false);
            } else if (!press.selecting && hit && hit->key==press.key) {
                if (hit->kind==ControlKind::number) {
                    if (!self.edit) self.begin_edit(hit->key,true,true);
                    else { self.edit->text_mode=true; if (self.edit->buffer.select_all()) self.revise(); }
                } else if (activatable(hit->kind))
                    Impl::action(reply,hit->kind==ControlKind::item?ActionKind::select:ActionKind::activate,hit->key,
                        toggle_kind(hit->kind)?ActionValue{!hit->selected}:ActionValue{});
            }
            return reply;
        }
        if (event.kind==EventKind::wheel) {
            auto key=hit?hit->key:0;
            while (key) {
                auto& state=self.states.at(key);
                if (state.control.kind==ControlKind::list) {
                    state.scroll=std::clamp(state.scroll-static_cast<double>(event.wheel)*48,0.0,state.scroll_limit);
                    reply.consumed=reply.redraw=true; return reply;
                }
                const auto* box=self.published->find(key); key=box?box->parent:0;
            }
            reply.consumed=hit && hit->kind!=ControlKind::viewport; return reply;
        }
        if (event.kind==EventKind::key_down && event.key==Key::escape) {
            const bool had=self.press.has_value() || self.edit.has_value();
            reply=self.cancel(); reply.consumed=had;
            if (had) self.terminal_key=Key::escape;
            return reply;
        }
        if (self.press) { reply.consumed=true; return reply; }
        if (event.kind==EventKind::key_down && event.key==Key::tab) {
            self.finish_edit(reply,true);
            std::vector<ControlKey> keys;
            for (const auto& box:self.published->controls) if (focusable(box.kind) && visible(box.bounds,box.clip) && self.current(box)) keys.push_back(box.key);
            if (!keys.empty()) {
                const auto at=std::find(keys.begin(),keys.end(),self.focus);
                const auto index=at==keys.end()?(event.shift?keys.size()-1:0):
                    (static_cast<std::size_t>(at-keys.begin())+keys.size()+(event.shift?-1:1))%keys.size();
                self.focus=keys[index];
                if (text_field(self.states.at(self.focus).control.kind)) self.begin_edit(self.focus,true,true);
            }
            reply.consumed=reply.redraw=true; return reply;
        }
        auto* focused=self.live(self.focus);
        const auto* focused_box=self.published->find(self.focus);
        if (!focused || !focused_box || !self.current(*focused_box)) return reply;
        if (text_field(focused->control.kind)) {
            if (!self.edit) self.begin_edit(self.focus,focused->control.kind==ControlKind::text_edit);
            auto& edit=*self.edit;
            const bool history_key=event.control && (event.key==Key::z || event.key==Key::y);
            if (history_key && !edit.buffer.active()) { edit.buffer.break_group(); return reply; }
            reply.consumed=true;
            if (event.kind==EventKind::text && !event.control && !event.alt) {
                if (const auto* value=std::get_if<std::wstring>(&event.data)) {
                    // Enter/Tab/control WM_CHAR belong to their key command.
                    if (value->size()==1 && (*value)[0]<32) return reply;
                    reply.redraw|=!edit.text_mode; edit.text_mode=true; self.insert(reply,*value,detail::TextEdit::Group::insert);
                }
            } else if (event.kind==EventKind::key_down) {
                if (event.key==Key::enter) {
                    self.terminal_key=Key::enter;
                    if (!edit.text_mode && focused->control.kind==ControlKind::number) {
                        edit.text_mode=true; if (edit.buffer.select_all()) self.revise(); reply.redraw=true;
                    } else self.finish_edit(reply,true,true,true);
                } else if (history_key) {
                    const bool was_active=edit.buffer.active();
                    self.changed(reply,edit.buffer.history(event.key==Key::y || event.shift),was_active);
                } else if (event.control && event.key==Key::a) {
                    reply.redraw|=!edit.text_mode; edit.text_mode=true; if (edit.buffer.select_all()) { self.revise(); reply.redraw=true; }
                } else if (event.control && (event.key==Key::c || event.key==Key::x || event.key==Key::v)) {
                    self.clipboard_command(reply,event.key==Key::v?ClipboardKind::read:event.key==Key::x?ClipboardKind::cut:ClipboardKind::copy);
                } else if (!event.control && (event.key==Key::delete_key || event.key==Key::backspace)) {
                    reply.redraw|=!edit.text_mode; edit.text_mode=true; const bool was_active=edit.buffer.active();
                    self.changed(reply,edit.buffer.erase(event.key==Key::backspace),was_active);
                } else if (!event.control && (event.key==Key::left || event.key==Key::right || event.key==Key::home || event.key==Key::end)) {
                    reply.redraw|=!edit.text_mode; edit.text_mode=true; const auto* geometry=self.edit_geometry();
                    edit.buffer.break_group();
                    if (!geometry) { reply.redraw=true; return reply; }
                    const auto& selected=edit.buffer.current();
                    const auto paragraph=geometry->paragraph->matches(selected.text)?geometry->paragraph:
                        self.text.edited_snapshot(edit.key,*geometry->paragraph,selected.text);
                    const bool left=event.key==Key::left || event.key==Key::home, edge=event.key==Key::home || event.key==Key::end;
                    auto position=selected.caret;
                    if (!event.shift && !edge && selected.caret.offset!=selected.anchor.offset) {
                        const auto ranges=paragraph->selection(
                            std::min(selected.caret.offset,selected.anchor.offset),std::max(selected.caret.offset,selected.anchor.offset));
                        if (!ranges.empty()) {
                            float x=left?ranges.front().x:ranges.front().x+ranges.front().width;
                            for (auto range:ranges) x=left?std::min(x,range.x):std::max(x,range.x+range.width);
                            position=paragraph->hit(x,0);
                        }
                    } else position=paragraph->move(position,left?-1:1,edge);
                    if (edit.buffer.select(position,event.shift)) { self.revise(); reply.redraw=true; }
                } else edit.buffer.break_group();
            }
            return reply;
        }
        if (activatable(focused->control.kind) &&
            (event.key==Key::space || event.key==Key::enter)) {
            reply.consumed=true;
            if (event.kind==EventKind::key_down && !self.keyboard_pressed) {
                self.keyboard_pressed=self.focus; self.keyboard_key=event.key; reply.redraw=true;
            } else if (event.kind==EventKind::key_up) {
                const auto pressed=std::exchange(self.keyboard_pressed,ControlKey{}); reply.redraw=pressed!=0;
                if (pressed==self.focus && self.keyboard_key==event.key)
                    Impl::action(reply,focused->control.kind==ControlKind::item?ActionKind::select:ActionKind::activate,self.focus,
                        toggle_kind(focused->control.kind)?ActionValue{!focused_box->selected}:ActionValue{});
            }
        }
        return reply;
    } catch (const detail::TextFailure& failure) {
        reply.error=Error{ErrorCode::text_failed,self.focus,failure.code}; return reply;
    }
}
