Reply Session::input(const Event& event) {
    auto& self=*impl_;
    Reply reply;
    try {
        self.sync_code_edit();
        if (event.kind==EventKind::capture_lost && !self.press) return reply;
        if (event.kind==EventKind::focus_lost || event.kind==EventKind::capture_lost) {
            reply=self.cancel();
            if (event.kind==EventKind::focus_lost) { self.edit.reset(); self.focus=0; }
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
        const auto pointer_cursor=[&] {
            const auto* state=self.press?self.live(self.press->key):(hit?self.live(hit->key):nullptr);
            if (state) {
                const auto& control=state->control;
                if (control.kind==ControlKind::text_edit || control.kind==ControlKind::code_editor || (control.kind==ControlKind::number && self.edit && self.edit->key==control.key && self.edit->text_mode)) return Cursor::text;
                if (control.kind==ControlKind::number || control.kind==ControlKind::slider) return Cursor::drag_horizontal;
                if (control.kind==ControlKind::splitter) return control.drag_axis==DragAxis::horizontal?Cursor::resize_horizontal:Cursor::resize_vertical;
                if (control.kind==ControlKind::tab && self.press && self.press->dragging) return Cursor::move;
            }
            return Cursor::arrow;
        };
        if (event.kind==EventKind::pointer_move || event.kind==EventKind::pointer_down || event.kind==EventKind::pointer_up) reply.cursor=pointer_cursor();
        if (self.popup_input(reply,event,hit)) return reply;
        // The bar is part of its scroll owner, with no synthetic document controls.
        if (self.press && self.press->scrollbar) {
            auto& press=*self.press;
            auto* state=self.live(press.key);
            if (!state) return self.cancel();
            reply.consumed=true; reply.cursor=Cursor::arrow;
            if (event.kind==EventKind::pointer_move) {
                auto& offset=press.horizontal?state->scroll_x:state->scroll;
                offset=std::clamp(press.original+(press.horizontal?event.x-press.x:event.y-press.y)*press.value/std::max(1.f,press.slider_span),0.0,press.value);
                reply.redraw=true;
            } else if (event.kind==EventKind::pointer_up && event.button==press.button) {
                self.press.reset(); reply.capture=Capture::release; reply.redraw=true;
            } else if (event.kind==EventKind::key_down && event.key==Key::escape) return self.cancel();
            return reply;
        }
        for (auto it=self.published->scrollbars.rbegin();it!=self.published->scrollbars.rend();++it) {
            const auto& bar=*it;
            const auto* owner=self.published->find(bar.key);
            if (!owner || !self.current(*owner) || !contains(bar.bounds,event.x,event.y) || !contains(bar.clip,event.x,event.y)) continue;
            if (event.kind==EventKind::pointer_move) {
                reply.redraw=self.hover!=bar.key; self.hover=bar.key; reply.cursor=Cursor::arrow; reply.consumed=true; return reply;
            }
            if (event.kind==EventKind::pointer_down && event.button==PointerButton::primary && !self.press) {
                self.finish_edit(reply,true);
                auto& state=self.states.at(bar.key);
                auto& offset=bar.horizontal?state.scroll_x:state.scroll;
                if (contains(bar.thumb,event.x,event.y)) {
                    self.press=Impl::Press{bar.key,event.x,event.y,event.x,event.y,bar.value,bar.maximum};
                    self.press->scrollbar=true; self.press->horizontal=bar.horizontal;
                    self.press->slider_span=bar.horizontal?bar.track.width-bar.thumb.width:bar.track.height-bar.thumb.height;
                    reply.capture=Capture::acquire;
                } else {
                    const auto step=contains(bar.decrement,event.x,event.y)?-24.0:contains(bar.increment,event.x,event.y)?24.0:
                        ((bar.horizontal?event.x<bar.thumb.x:event.y<bar.thumb.y)?-bar.page:bar.page);
                    offset=std::clamp(offset+step,0.0,bar.maximum);
                }
                reply.cursor=Cursor::arrow; reply.consumed=reply.redraw=true; return reply;
            }
            break;
        }
        if (self.code_input(reply,event,hit)) return reply;
        if (self.table_input(reply,event,hit)) return reply;
        if (event.kind==EventKind::pointer_move) {
            const auto key=hit?hit->key:0;
            reply.redraw=self.hover!=key; self.hover=key;
            if (self.press) {
                auto& press=*self.press;
                const auto* state=self.live(press.key);
                if (!state) return self.cancel();
                const auto& control=state->control;
                if (press.selecting && self.edit && self.edit->key==press.key) self.select_at(reply,event.x,event.y,true);
                else if (control.kind==ControlKind::tab) {
                    if (!press.dragging && (std::abs(event.x-press.x)>4*self.surface.dpi_scale || std::abs(event.y-press.y)>4*self.surface.dpi_scale)) {
                        self.finish_edit(reply,true); press.dragging=true;
                        Impl::action(reply,ActionKind::drag_begin,press.key,DragPoint{press.x,press.y});
                    }
                    if (press.dragging) {
                        Impl::action(reply,ActionKind::drag_preview,press.key,DragPoint{event.x,event.y}); reply.redraw=true;
                    }
                } else if (editable(control.kind)) {
                    if (press.ladder) {
                        auto& ladder=*press.ladder;
                        auto& geometry=ladder.geometry;
                        bool choosing=ladder.fine!=event.shift || ladder.multiply!=event.alt;
                        if (event.y!=press.last_y && event.x>=geometry.bounds.x && event.x<geometry.bounds.x+geometry.bounds.width) {
                            const auto row=std::clamp(static_cast<int>(std::floor((event.y-geometry.bounds.y)/geometry.row_height)),0,6);
                            choosing|=row!=geometry.selected_row;
                            geometry.selected_row=row;
                        }
                        ladder.fine=event.shift; ladder.multiply=event.alt;
                        geometry.increment=ladder_steps[geometry.selected_row]*(ladder.fine?.1:1);
                        if (choosing) ladder.remainder=0;
                        else ladder.remainder+=(event.x-press.last_x)/self.surface.dpi_scale;
                        const auto ticks=std::trunc(ladder.remainder/ladder_tick_distance);
                        ladder.remainder-=ticks*ladder_tick_distance;
                        if (ticks!=0) {
                            double next=press.value;
                            if (ladder.multiply) {
                                const auto factor=std::pow(1+geometry.increment,ticks);
                                if (next!=0) next*=factor;
                                ladder.multiplier=std::clamp(ladder.multiplier*factor,1e-12,1e12);
                            } else next+=ticks*geometry.increment;
                            next=std::clamp(next,control.minimum,control.maximum);
                            if (next!=press.value) {
                                if (!press.dragging) {
                                    self.finish_edit(reply,true); press.dragging=true;
                                    Impl::action(reply,ActionKind::edit_begin,press.key,press.original);
                                }
                                press.value=next;
                                Impl::action(reply,ActionKind::edit_preview,press.key,press.value);
                            }
                        }
                        reply.redraw=true;
                    } else {
                    const auto delta=control.drag_axis==DragAxis::horizontal?event.x-press.last_x:event.y-press.last_y;
                    if (!press.dragging && (std::abs(event.x-press.x)>3 || std::abs(event.y-press.y)>3)) {
                        self.finish_edit(reply,true); press.dragging=true;
                        Impl::action(reply,ActionKind::edit_begin,press.key,press.original);
                    }
                    if (press.dragging) {
                        if (control.kind==ControlKind::slider) {
                            const auto step=(control.maximum-control.minimum)/press.slider_span*(event.shift?.1:1.0);
                            press.value=std::clamp(press.value+(event.x-press.last_x)*step,control.minimum,control.maximum);
                        } else {
                            const auto step=event.shift?control.fine_step:control.drag_step;
                            press.value=std::clamp(press.value+delta/self.surface.dpi_scale*step,control.minimum,control.maximum);
                        }
                        Impl::action(reply,ActionKind::edit_preview,press.key,press.value); reply.redraw=true;
                    }
                    }
                }
                press.last_x=event.x; press.last_y=event.y; reply.consumed=true;
            } else reply.consumed=self.popup || (hit && hit->kind!=ControlKind::viewport);
            reply.cursor=pointer_cursor();
            return reply;
        }
        if (event.kind==EventKind::pointer_down) {
            reply.consumed=hit && hit->kind!=ControlKind::viewport;
            if (self.press) return reply;
            if (hit && event.button==PointerButton::secondary && self.states.at(hit->key).control.context_target) {
                self.context_request(reply,*hit,event.x,event.y); return reply;
            }
            if (hit && event.button==PointerButton::middle && event.control) {
                const auto& control=self.states.at(hit->key).control;
                if (hit->kind==ControlKind::number && control.default_value) {
                    self.execute_edit_command(reply,{EditCommandKind::reset,hit->key}); reply.consumed=true; return reply;
                }
                if (control.context_target) {
                    self.finish_edit(reply,true); Impl::action(reply,ActionKind::reset,control.context_target);
                    reply.consumed=reply.redraw=true; return reply;
                }
            }
            if (event.button!=PointerButton::primary && !(hit && event.button==PointerButton::middle && hit->kind==ControlKind::number)) return reply;
            self.keyboard_pressed=0;
            const auto next=hit && focusable(hit->kind)?hit->key:0;
            if (next!=self.focus) { self.finish_edit(reply,true); self.focus=next; }
            if (event.button==PointerButton::middle) self.finish_edit(reply,true);
            if (!hit || (!focusable(hit->kind) && hit->kind!=ControlKind::splitter)) return reply;
            const auto& control=self.states.at(hit->key).control;
            if (text_field(control.kind) && (!self.edit || self.edit->key!=hit->key))
                self.begin_edit(hit->key,control.kind==ControlKind::text_edit);
            const bool selecting=self.edit && self.edit->key==hit->key && self.edit->text_mode;
            if (self.edit) self.edit->buffer().break_group();
            if (selecting) self.select_at(reply,event.x,event.y,event.shift);
            self.press=Impl::Press{hit->key,event.x,event.y,event.x,event.y,control.value,control.value,false,selecting};
            self.press->button=event.button;
            if (const auto* row=self.tree_row(hit->key)) self.press->tree_disclosure=contains(row->disclosure,event.x,event.y);
            if (control.kind==ControlKind::slider) {
                const auto track=slider_track(hit->bounds,self.surface.dpi_scale);
                auto& press=*self.press; press.slider_span=std::max(track.width,1.f);
                press.value=std::clamp(control.value,control.minimum,control.maximum);
                const auto thumb=track.x+track.width*(press.value-control.minimum)/(control.maximum-control.minimum);
                if (std::abs(event.x-thumb)>5*self.surface.dpi_scale) {
                    press.dragging=true;
                    press.value=control.minimum+std::clamp((event.x-track.x)/press.slider_span,0.f,1.f)*(control.maximum-control.minimum);
                    Impl::action(reply,ActionKind::edit_begin,press.key,press.original);
                    Impl::action(reply,ActionKind::edit_preview,press.key,press.value);
                }
            }
            if (event.button==PointerButton::middle) {
                auto& ladder=self.press->ladder.emplace();
                // A transient popup consumes the same scoped SS rules as ordinary controls.
                Impl::State popup; popup.control.parent=hit->key; popup.control.kind=ControlKind::popup;
                popup.control.classes={"value-ladder"}; self.resolve(popup); ladder.style=popup.resolved;
                ladder.fine=event.shift; ladder.multiply=event.alt;
                const auto scale=self.surface.dpi_scale;
                const auto width=std::min(45.333f*scale,static_cast<float>(self.surface.extent.width));
                const auto height=std::min(44.667f*7*scale,static_cast<float>(self.surface.extent.height));
                const auto x=std::clamp(event.x-width*.5f,0.f,static_cast<float>(self.surface.extent.width)-width);
                const auto y=std::clamp(event.y-height*.5f,0.f,static_cast<float>(self.surface.extent.height)-height);
                ladder.geometry={hit->key,{x,y,width,height},height/7,3,.1*(event.shift?.1:1)};
                for (std::size_t row=0;row<ladder.labels.size();++row) {
                    self.text.set(0,ladder_labels[row],ladder.style->font,false,TextAlign::center);
                    ladder.text_height=self.text.metrics(0,width/scale).height;
                    ladder.labels[row]=self.text.raster(0,width/scale,scale);
                }
            }
            reply.capture=Capture::acquire; reply.redraw=true; return reply;
        }
        if (event.kind==EventKind::pointer_up) {
            if (!self.press || event.button!=self.press->button) return reply;
            const auto press=*self.press; self.press.reset();
            reply.consumed=reply.redraw=true; reply.capture=Capture::release;
            if (press.dragging && self.states.at(press.key).control.kind==ControlKind::tab) {
                Impl::action(reply,ActionKind::drag_commit,press.key,DragPoint{event.x,event.y});
            } else if (press.dragging) {
                Impl::action(reply,ActionKind::edit_commit,press.key,press.value);
                if (self.focus==press.key && self.states.at(press.key).control.kind==ControlKind::number)
                    self.edit.emplace(self.states.at(press.key),format(press.value),press.value,false,false);
            } else if (!press.selecting && hit && hit->key==press.key) {
                if (hit->kind==ControlKind::tree_item && press.tree_disclosure) {
                    if (const auto* row=self.tree_row(hit->key);row && contains(row->disclosure,event.x,event.y))
                        self.expand_tree(reply,hit->key,!row->expanded);
                } else if (hit->kind==ControlKind::number && press.button==PointerButton::primary) {
                    if (!self.edit) self.begin_edit(hit->key,true,true);
                    else { self.edit->text_mode=true; if (self.edit->buffer().select_all()) self.revise(); }
                } else if (hit->kind==ControlKind::splitter && self.states.at(hit->key).control.click_action)
                    Impl::action(reply,ActionKind::activate,hit->key,event.alt);
                else if (activatable(hit->kind)) self.activate(reply,*hit);
            }
            reply.cursor=pointer_cursor();
            return reply;
        }
        if (event.kind==EventKind::wheel) {
            auto key=hit?hit->key:0;
            while (key) {
                auto& state=self.states.at(key);
                if (scrolling(state.control.kind)) {
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
            std::map<ControlKey,ControlKey> tree_stops;
            for (const auto& box:self.published->controls) if (box.kind==ControlKind::tree_item && self.current(box) && visible(box.bounds,box.clip)) {
                auto& stop=tree_stops[box.parent];
                if (!stop || box.key==self.focus || (box.selected && stop!=self.focus)) stop=box.key;
            }
            for (const auto& box:self.published->controls) if (focusable(box.kind) && visible(box.bounds,box.clip) && self.current(box)) {
                const auto& control=self.states.at(box.key).control;
                if ((!control.selection_group || box.selected) && (box.kind!=ControlKind::tree_item || tree_stops[box.parent]==box.key)) keys.push_back(box.key);
            }
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
        if (self.tree_keyboard(reply,event,*focused)) return reply;
        if (focused->control.selection_group && event.kind==EventKind::key_down && !event.control && !event.alt &&
            (event.key==Key::left || event.key==Key::right || event.key==Key::home || event.key==Key::end)) {
            std::vector<const ControlBox*> keys;
            for (const auto& box:self.published->controls)
                if (self.current(box) && visible(box.bounds,box.clip) && self.states.at(box.key).control.selection_group==focused->control.selection_group) keys.push_back(&box);
            const auto found=std::find(keys.begin(),keys.end(),focused_box);
            if (!keys.empty() && found!=keys.end()) {
                const auto at=static_cast<std::size_t>(found-keys.begin());
                const auto next=event.key==Key::home?0:event.key==Key::end?keys.size()-1:(at+keys.size()+(event.key==Key::left?-1:1))%keys.size();
                self.focus=keys[next]->key; self.activate(reply,*keys[next]);
            }
            reply.consumed=reply.redraw=true; return reply;
        }
        if (focused->control.kind==ControlKind::slider && event.kind==EventKind::key_down && !event.control && !event.alt &&
            (event.key==Key::left || event.key==Key::right || event.key==Key::up || event.key==Key::down || event.key==Key::home || event.key==Key::end)) {
            const auto& control=focused->control;
            const auto step=event.shift?control.fine_step:control.drag_step;
            const auto value=event.key==Key::home?control.minimum:event.key==Key::end?control.maximum:
                std::clamp(control.value+(event.key==Key::left || event.key==Key::down?-step:step),control.minimum,control.maximum);
            Impl::action(reply,ActionKind::edit_begin,self.focus,control.value);
            Impl::action(reply,ActionKind::edit_preview,self.focus,value);
            Impl::action(reply,ActionKind::edit_commit,self.focus,value);
            reply.consumed=reply.redraw=true; return reply;
        }
        if (event.kind==EventKind::key_down &&
            (event.key==Key::menu || (event.key==Key::f10 && event.shift) ||
             (focused->control.kind==ControlKind::combo && (event.key==Key::down || event.key==Key::up))) &&
            (focused->control.context_target || focused->control.kind==ControlKind::combo)) {
            self.context_request(reply,*focused_box,focused_box->bounds.x,focused_box->bounds.y+focused_box->bounds.height); return reply;
        }
        if (text_field(focused->control.kind)) {
            if (!self.edit) self.begin_edit(self.focus,focused->control.kind==ControlKind::text_edit);
            auto& edit=*self.edit;
            const bool history_key=event.control && (event.key==Key::z || event.key==Key::y);
            if (history_key && !edit.document && !edit.buffer().active()) { edit.buffer().break_group(); return reply; }
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
                        edit.text_mode=true; if (edit.buffer().select_all()) self.revise(); reply.redraw=true;
                    } else self.finish_edit(reply,true,true,true);
                } else if (history_key) {
                    const bool was_active=edit.buffer().active();
                    self.changed(reply,edit.buffer().history(event.key==Key::y || event.shift),was_active);
                } else if (event.control && event.key==Key::a) {
                    reply.redraw|=!edit.text_mode; edit.text_mode=true; if (edit.buffer().select_all()) { self.revise(); reply.redraw=true; }
                } else if (event.control && (event.key==Key::c || event.key==Key::x || event.key==Key::v)) {
                    self.clipboard_command(reply,event.key==Key::v?ClipboardKind::read:event.key==Key::x?ClipboardKind::cut:ClipboardKind::copy);
                } else if (!event.control && (event.key==Key::delete_key || event.key==Key::backspace)) {
                    reply.redraw|=!edit.text_mode; edit.text_mode=true; const bool was_active=edit.buffer().active();
                    self.changed(reply,edit.buffer().erase(event.key==Key::backspace),was_active);
                } else if (!event.control && (event.key==Key::left || event.key==Key::right || event.key==Key::home || event.key==Key::end)) {
                    reply.redraw|=!edit.text_mode; edit.text_mode=true; const auto* geometry=self.edit_geometry();
                    edit.buffer().break_group();
                    if (!geometry) { reply.redraw=true; return reply; }
                    const auto& selected=edit.buffer().current();
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
                    if (edit.buffer().select(position,event.shift)) { self.revise(); reply.redraw=true; }
                } else edit.buffer().break_group();
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
                    self.activate(reply,*focused_box);
            }
        }
        return reply;
    } catch (const detail::TextFailure& failure) {
        reply.error=Error{ErrorCode::text_failed,self.focus,failure.code}; return reply;
    }
}
