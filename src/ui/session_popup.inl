    void restore_popup_focus() {
        const auto* box=published?published->find(popup_return_focus):nullptr;
        focus=box && current(*box) && focusable(box->kind) && visible(box->bounds,box->clip)?box->key:0;
    }
    void dismiss_popup(Reply& reply) {
        if (!popup || popup_dismissed) return;
        popup_dismissed=true; keyboard_pressed=0; keyboard_key=Key::none; hover=0;
        if (press) { press.reset(); reply.capture=Capture::release; }
        restore_popup_focus();
        action(reply,ActionKind::dismiss,popup); reply.redraw=reply.consumed=true;
    }
    void update_popup(ControlKey next, Reply& reply) {
        if (next==popup) return;
        if (next && press) {
            auto cancelled=cancel(); reply.capture=cancelled.capture;
            for (auto& effect:cancelled.effects) reply.effects.push_back(std::move(effect));
        }
        if (popup && !popup_dismissed) restore_popup_focus();
        popup=next; popup_dismissed=false;
        if (!popup) { popup_return_focus=0; return; }
        finish_edit(reply,true);
        popup_return_focus=focus; focus=0; keyboard_pressed=0; hover=0;
        bool selected_radio=false;
        for (auto key:order) {
            const auto& control=states.at(key).control;
            if (control.kind==ControlKind::menu_item && control.enabled) {
                if (!focus) focus=key;
                if (!selected_radio && control.menu_behavior==MenuBehavior::radio && control.selected) {
                    focus=key; selected_radio=true;
                }
            }
        }
        reply.redraw=true;
    }
    void context_request(Reply& reply, const ControlBox& box, float x, float y, std::optional<TableSelection> table={}) {
        finish_edit(reply,true);
        if (focusable(box.kind)) focus=box.key;
        action(reply,ActionKind::context_menu,box.context_target?box.context_target:box.key,
            ContextMenuRequest{box.key,box.context_group,x,y,table});
        reply.consumed=reply.redraw=true;
    }
    void execute_edit_command(Reply& reply, EditCommand command) {
        auto* state=live(command.target);
        const auto* box=published?published->find(command.target):nullptr;
        if (!state || !box || !current(*box) || !text_field(state->control.kind)) return;
        finish_edit(reply,true);
        focus=command.target;
        begin_edit(command.target,true,true);
        if (command.kind==EditCommandKind::reset) {
            if (!state->control.default_value) return;
            insert(reply,format(*state->control.default_value),detail::TextEdit::Group::isolated);
            finish_edit(reply,true,true);
        } else {
            edit->commit_paste=command.kind==EditCommandKind::paste;
            clipboard_command(reply,command.kind==EditCommandKind::copy?ClipboardKind::copy:ClipboardKind::read);
        }
        reply.redraw=true;
    }
    void activate(Reply& reply, const ControlBox& box) {
        const auto command=states.at(box.key).control.edit_command;
        if (box.kind==ControlKind::combo) {
            context_request(reply,box,box.bounds.x,box.bounds.y+box.bounds.height); return;
        }
        if (command) {
            dismiss_popup(reply);
            execute_edit_command(reply,*command);
        } else {
            const bool toggle=toggle_kind(box.kind) || (box.kind==ControlKind::button && box.button_behavior==ButtonBehavior::toggle) ||
                (box.kind==ControlKind::menu_item && box.menu_behavior==MenuBehavior::checkbox);
            const bool radio=box.kind==ControlKind::menu_item && box.menu_behavior==MenuBehavior::radio;
            action(reply,(box.kind==ControlKind::item || box.kind==ControlKind::tree_item)?ActionKind::select:ActionKind::activate,box.key,
                radio?ActionValue{true}:toggle?ActionValue{!box.selected}:ActionValue{},box.action_target);
            if (box.kind==ControlKind::menu_item && !states.at(box.key).control.menu_branch) dismiss_popup(reply);
        }
    }
    bool popup_input(Reply& reply, const Event& event, const ControlBox* hit) {
        if (!popup && (!published || !published->popup)) return false;
        reply.consumed=true;
        if (event.kind==EventKind::key_down && (event.key==Key::escape || event.key==Key::tab)) {
            dismiss_popup(reply); terminal_key=event.key; return true;
        }
        if (!popup || popup_dismissed || published->popup!=popup) return true;
        const auto* owner=published->find(states.at(popup).control.popup_owner);
        if (states.at(popup).control.popup_owner && (!owner || !current(*owner) || !visible(owner->bounds,owner->clip))) { dismiss_popup(reply); return true; }
        bool inside=false;
        for (auto key=hit?hit->key:0;key;) {
            if (key==popup) { inside=true; break; }
            const auto* box=published->find(key); key=box?box->parent:0;
        }
        if (event.kind==EventKind::pointer_down && !inside) { dismiss_popup(reply); return true; }
        if (event.kind==EventKind::wheel && !inside) return true;
        if (event.kind==EventKind::pointer_move && hit && hit->kind==ControlKind::menu_item) {
            reply.redraw|=focus!=hit->key; focus=hit->key;
        }
        if (event.kind==EventKind::key_down && (event.key==Key::up || event.key==Key::down || event.key==Key::home || event.key==Key::end)) {
            std::vector<ControlKey> keys;
            for (const auto& box:published->controls)
                if (box.kind==ControlKind::menu_item && current(box)) keys.push_back(box.key);
            if (!keys.empty()) {
                const auto at=std::find(keys.begin(),keys.end(),focus);
                auto index=at==keys.end()?std::size_t{}:static_cast<std::size_t>(at-keys.begin());
                if (event.key==Key::home) index=0;
                else if (event.key==Key::end) index=keys.size()-1;
                else index=(index+keys.size()+(event.key==Key::up?-1:1))%keys.size();
                focus=keys[index]; keyboard_pressed=0;
                const auto& box=*published->find(focus);
                auto& menu=states.at(popup);
                if (box.bounds.y<box.clip.top) menu.scroll-=(box.clip.top-box.bounds.y)/surface.dpi_scale;
                if (box.bounds.y+box.bounds.height>box.clip.bottom) menu.scroll+=(box.bounds.y+box.bounds.height-box.clip.bottom)/surface.dpi_scale;
                menu.scroll=std::clamp(menu.scroll,0.0,menu.scroll_limit);
            }
            reply.redraw=true; return true;
        }
        if (event.kind==EventKind::key_down && event.key==Key::right) {
            const auto* box=published->find(focus);
            if (box && current(*box) && states.at(box->key).control.menu_branch) activate(reply,*box);
            return true;
        }
        if ((event.kind==EventKind::key_down || event.kind==EventKind::key_up) && event.key!=Key::enter && event.key!=Key::space) return true;
        if (event.kind==EventKind::text) return true;
        return false;
    }
