    // Producers share property validation; Session also validates references and tree row order.
    template<class Lookup> static Result<void> validate_control(const Control& control, Lookup lookup) {
        ATELIER_TRY(validate_control_properties(control));
        const auto* parent=lookup(control.parent);
        if (control.parent && !parent) return std::unexpected(Error{ErrorCode::invalid_control,control.key});
        if (control.selection_group && (control.kind!=ControlKind::button || control.selection_group!=control.parent ||
            !parent || parent->kind!=ControlKind::container))
            return std::unexpected(Error{ErrorCode::invalid_control,control.key});
        if (control.kind==ControlKind::tree_item && (!parent || parent->kind!=ControlKind::tree || control.tree_index>=parent->tree_row_count))
            return std::unexpected(Error{ErrorCode::invalid_control,control.key});
        if (control.kind==ControlKind::popup) {
            if (control.parent || (control.popup_owner && !lookup(control.popup_owner)))
                return std::unexpected(Error{ErrorCode::invalid_control,control.key});
        } else if (control.popup_owner) return std::unexpected(Error{ErrorCode::invalid_control,control.key});
        if (control.menu_branch && (control.kind!=ControlKind::menu_item || control.edit_command))
            return std::unexpected(Error{ErrorCode::invalid_control,control.key});
        if (control.edit_command) {
            const auto* target=lookup(control.edit_command->target);
            if (control.kind!=ControlKind::menu_item || !target || !text_field(target->kind) ||
                static_cast<unsigned>(control.edit_command->kind)>static_cast<unsigned>(EditCommandKind::reset) ||
                (control.edit_command->kind==EditCommandKind::reset && !target->default_value))
                return std::unexpected(Error{ErrorCode::invalid_control,control.key});
        }
        if (control.rename_anchor) {
            const auto* target=lookup(control.rename_anchor);
            if (!target || (target->kind!=ControlKind::tree_item && target->kind!=ControlKind::table) ||
                ((control.rename_cell!=0)!=(target->kind==ControlKind::table)))
                return std::unexpected(Error{ErrorCode::invalid_control,control.key});
        }
        if (parent && ((parent->kind==ControlKind::tree && control.kind!=ControlKind::tree_item) ||
            (parent->kind!=ControlKind::container && parent->kind!=ControlKind::list && parent->kind!=ControlKind::popup &&
             parent->kind!=ControlKind::tree))) return std::unexpected(Error{ErrorCode::invalid_control,control.key});
        return {};
    }
    static bool same_structure(const Control& a,const Control& b) {
        return a.kind==b.kind && a.parent==b.parent && a.layout.kind==b.layout.kind &&
            a.tree_row_count==b.tree_row_count && a.tree_index==b.tree_index && a.tree_depth==b.tree_depth &&
            a.tree_branch==b.tree_branch && a.tree_expanded==b.tree_expanded &&
            a.popup_owner==b.popup_owner && a.selection_group==b.selection_group &&
            a.context_target==b.context_target && a.context_group==b.context_group && a.edit_command==b.edit_command &&
            a.menu_branch==b.menu_branch && a.rename_anchor==b.rename_anchor && a.rename_cell==b.rename_cell &&
            a.transient_edit==b.transient_edit;
    }
    Result<Reply> apply_controls(std::span<const Control> controls,bool sparse) {
        if (controls.size()>10000) return std::unexpected(Error{ErrorCode::capacity_exceeded});
        // Sparse order has no meaning. Resolve references through the entire
        // proposed batch, then the retained state; full input validates preorder.
        std::unordered_map<ControlKey,std::pair<const Control*,std::size_t>> proposed;
        proposed.reserve(controls.size());
        for (std::size_t i=0;i<controls.size();++i) {
            const auto& control=controls[i];
            if (!control.key || !proposed.emplace(control.key,std::pair{&control,i}).second)
                return std::unexpected(Error{ErrorCode::invalid_control,control.key});
            if (sparse) {
                const auto* previous=live(control.key);
                if (!previous || !same_structure(previous->control,control))
                    return std::unexpected(Error{ErrorCode::invalid_control,control.key});
            }
        }
        const auto lookup=[&](ControlKey key)->const Control* {
            if (const auto found=proposed.find(key);found!=proposed.end()) return found->second.first;
            const auto* state=sparse?live(key):nullptr;
            return state?&state->control:nullptr;
        };
        const auto precedes=[&](ControlKey earlier,std::size_t current_idx)->bool {
            const auto found=proposed.find(earlier);
            return found!=proposed.end() && found->second.second<current_idx;
        };
        std::vector<ControlKey> next_order;
        std::map<ControlKey,TableIndex> table_indices;
        std::unordered_map<ControlKey,const Control*> previous_rows; // Per tree: the last described row.
        ControlKey next_popup=sparse?popup:0;
        bool check_commands=false,check_radio=false;
        if (!sparse) next_order.reserve(controls.size());
        for (std::size_t control_idx=0;control_idx<controls.size();++control_idx) {
            const auto& control=controls[control_idx];
            ATELIER_TRY(validate_control(control,lookup));
            if (control.table) {
                const auto* previous=live(control.key);
                if (!previous || previous->control.table!=control.table) {
                    ATELIER_TRY_BIND(checked,validate_table(control));
                    table_indices.emplace(control.key,std::move(*checked));
                }
                const auto changed=table_indices.find(control.key);
                const auto& index=changed!=table_indices.end()?changed->second:previous->table_index;
                if ((control.table_selection.row && !index.contains(control.table_selection.row)) ||
                    (control.table_selection.column && std::none_of(control.table->columns.begin(),control.table->columns.end(),
                        [&](const auto& column){return column.key==control.table_selection.column;})))
                    return std::unexpected(Error{ErrorCode::invalid_control,control.key});
            }
            check_radio|=control.menu_behavior==MenuBehavior::radio;
            if (sparse) {
                check_commands|=live(control.key)->control.default_value!=control.default_value;
                continue;
            }
            if ((control.parent && !precedes(control.parent,control_idx)) ||
                (control.popup_owner && !precedes(control.popup_owner,control_idx)) ||
                (control.edit_command && !precedes(control.edit_command->target,control_idx)) ||
                (control.rename_anchor && !precedes(control.rename_anchor,control_idx)))
                return std::unexpected(Error{ErrorCode::invalid_control,control.key});
            if (control.kind==ControlKind::tree_item) {
                // Rows ascend; an adjacent row is at most one level deeper, under an expanded branch.
                auto& previous=previous_rows[control.parent];
                if ((!control.tree_index && control.tree_depth) || (previous && (control.tree_index<=previous->tree_index ||
                    (control.tree_index==previous->tree_index+1 && (control.tree_depth>previous->tree_depth+1 ||
                        (control.tree_depth==previous->tree_depth+1 && !previous->tree_expanded))))))
                    return std::unexpected(Error{ErrorCode::invalid_control,control.key});
                previous=&control;
            }
            if (control.kind==ControlKind::popup) {
                if (next_popup) return std::unexpected(Error{ErrorCode::invalid_control,control.key});
                next_popup=control.key;
            }
            if (next_popup && control.key!=next_popup) {
                auto parent=control.parent;
                while (parent && parent!=next_popup) parent=lookup(parent)->parent;
                if (parent!=next_popup) return std::unexpected(Error{ErrorCode::invalid_control,control.key});
            }
            next_order.push_back(control.key);
        }
        // A target property can invalidate an unchanged reset menu item.
        if (check_commands) for (const auto& [key,state]:states) if (state.control.edit_command)
            ATELIER_TRY(validate_control(*lookup(key),lookup));
        if (check_radio) {
            std::map<ControlKey,ControlKey> selected_groups;
            const auto admit=[&](const Control& control) {
                return control.menu_behavior!=MenuBehavior::radio || !control.selected || selected_groups.emplace(control.radio_group,control.key).second;
            };
            if (sparse) {
                for (auto key:order) if (!admit(*lookup(key))) return std::unexpected(Error{ErrorCode::invalid_control,key});
            } else for (const auto& control:controls) if (!admit(control)) return std::unexpected(Error{ErrorCode::invalid_control,control.key});
        }

        struct Change { State* state; Control value; };
        std::vector<Change> changes;
        std::map<ControlKey,State> additions,removed;
        std::vector<ControlKey> added_keys,removed_keys;
        std::vector<ControlKey> tree_reveals; // Rows that became the host's primary row.
        bool structural=!sparse && next_order!=order;
        for (const auto& control:controls) {
            if (auto* previous=live(control.key)) {
                if (previous->control==control) continue;
                changes.push_back({previous,control});
                if (control.kind==ControlKind::tree_item && control.tree_primary &&
                    (previous->control.kind!=ControlKind::tree_item || !previous->control.tree_primary)) tree_reveals.push_back(control.key);
                structural|=previous->control.parent!=control.parent || previous->control.kind!=control.kind;
            } else {
                auto& value=additions[control.key];
                value.control=control; value.incarnation=detail::new_identity();
                if (control.kind==ControlKind::tree_item && control.tree_primary) tree_reveals.push_back(control.key);
                added_keys.push_back(control.key);
            }
        }
        if (!sparse) for (const auto& [key,state]:states) if (!proposed.contains(key)) removed_keys.push_back(key);
        if (changes.empty() && added_keys.empty() && removed_keys.empty() && !structural) return Reply{};
        const auto revision=detail::new_identity();
        const auto absent=[&](this auto const& self_absent, ControlKey key) -> bool {
            while (key) {
                const auto* future=lookup(key);
                const auto* previous=live(key);
                if (!future || !future->enabled || future->layout.collapsed || (previous &&
                    (future->kind!=previous->control.kind || future->code!=previous->control.code || future->action_target!=previous->control.action_target ||
                     future->context_target!=previous->control.context_target || future->context_group!=previous->control.context_group ||
                     future->button_behavior!=previous->control.button_behavior || future->menu_behavior!=previous->control.menu_behavior ||
                     future->radio_group!=previous->control.radio_group || (future->kind==ControlKind::tree_item &&
                     future->parent!=previous->control.parent)))) return true;
                if (future->rename_anchor && self_absent(future->rename_anchor)) return true;
                key=future->parent;
            }
            return false;
        };
        if (next_popup && lookup(next_popup)->layout.collapsed) next_popup=0;
        // A collapsed popup can be re-opened through a sparse property update.
        if (sparse) for (const auto& control:controls) if (control.kind==ControlKind::popup)
            next_popup=control.layout.collapsed?0:control.key;
        const auto table_press_invalid = [&](const Press& held) {
            const auto* future = lookup(held.key);
            const auto* previous = live(held.key);
            if (!future || !previous || previous->control.kind != ControlKind::table)
                return false;
            if (!future->table || future->table_layout != previous->control.table_layout)
                return true;
            if (future->table == previous->control.table || held.scrollbar)
                return false;
            const auto& data = *future->table;
            if (held.table_column)
                return std::ranges::find(data.columns, held.table_column, &TableColumn::key) == data.columns.end();
            if (held.table_cell.row) {
                const auto row = std::ranges::find(data.rows, held.table_cell.row, &TableRow::key);
                return row == data.rows.end() || !row->enabled ||
                       std::ranges::find(data.columns, held.table_cell.column, &TableColumn::key) == data.columns.end();
            }
            return false;
        };
        const auto table_keyboard_invalid = [&](ControlKey key) {
            const auto* future = lookup(key);
            const auto* previous = live(key);
            if (!future || !previous || previous->control.kind != ControlKind::table)
                return false;
            if (!future->table || future->table_layout != previous->control.table_layout ||
                future->table_selection != previous->control.table_selection)
                return true;
            if (future->table == previous->control.table || !future->table_selection.row)
                return false;
            const auto row = std::ranges::find(future->table->rows, future->table_selection.row, &TableRow::key);
            return row == future->table->rows.end() || !row->enabled;
        };
        const auto edit_policy_changed = [&](ControlKey key) {
            const auto* future = lookup(key);
            const auto* previous = live(key);
            return future && previous &&
                   (future->read_only != previous->control.read_only ||
                    future->integral != previous->control.integral ||
                    future->drag_axis != previous->control.drag_axis ||
                    future->transient_edit != previous->control.transient_edit ||
                    future->rename_anchor != previous->control.rename_anchor ||
                    future->rename_cell != previous->control.rename_cell);
        };
        // Retire pending clipboard/IME writes without clearing focus: forbidden
        // keys must remain owned by this field during the next-frame transition.
        const bool cancelling =
            (press && (absent(press->key) || table_press_invalid(*press) || edit_policy_changed(press->key))) ||
            (edit && (absent(edit->key) || edit_policy_changed(edit->key)));
        const bool clear_focus = absent(focus),
                   clear_keyboard = absent(keyboard_pressed) || table_keyboard_invalid(keyboard_pressed);
        const auto* future_edit=edit?lookup(edit->key):nullptr;
        const bool refresh_edit=edit && !edit->document && future_edit && !edit->buffer().active() && !edit->composition &&
            (future_edit->kind==ControlKind::number ? detail::number_bits(edit->original_value)!=detail::number_bits(future_edit->value)
                                                    : edit->buffer().original()!=wide_to_utf8(future_edit->text));
        const bool popup_invalid=next_popup && absent(lookup(next_popup)->popup_owner);
        const bool interaction_changes=cancelling || clear_focus || clear_keyboard || refresh_edit ||
            next_popup!=popup || popup_invalid;
        auto old_press=interaction_changes?press:std::optional<Press>{};
        auto old_edit=interaction_changes?edit:std::optional<Edit>{};
        const auto old_focus=focus,old_hover=hover,old_keyboard=keyboard_pressed;
        const auto old_tooltip=tooltip_hover;
        const auto old_keyboard_key=keyboard_key,old_terminal=terminal_key;
        const auto old_popup=popup,old_popup_focus=popup_return_focus;
        const bool old_dismissed=popup_dismissed;
        Reply reply;
        std::size_t applied{};
        bool inserted=false,reordered=false;
        const auto apply=[&]() -> Result<void> {
            // Keep descriptors and interactions until text-service work finishes.
            // Effects from a rejected candidate never escape this transaction.
            if (cancelling) {
                reply=cancel();edit.reset();
                if (reply.error) return std::unexpected(*reply.error);
            }
            if (clear_focus) set_focus(reply,0);
            if (clear_keyboard) retire_keyboard(reply);
            for (auto key:removed_keys) removed.insert(states.extract(key));
            states.merge(additions); inserted=true;
            static_assert(std::is_nothrow_swappable_v<Control>);
            for (auto& change:changes) { std::swap(change.state->control,change.value); ++applied; }
            if (!sparse && next_order!=order) { order.swap(next_order); reordered=true; }
            if (refresh_edit && edit) {
                const auto caret=edit->buffer().current().caret,anchor=edit->buffer().current().anchor;
                const auto key=edit->key; const bool mode=edit->text_mode;
                if (!begin_edit(reply,key,mode)) return std::unexpected(*reply.error);
                edit->buffer().select(anchor,false); edit->buffer().select(caret,true);
            }
            update_popup(next_popup,reply);
            if (reply.error) return std::unexpected(*reply.error);
            if (popup && popup_invalid) dismiss_popup(reply);
            return {};
        };
        const auto changed=apply();
        if (!changed) {
            // std::map node transfers keep State addresses and allocate nothing.
            if (reordered) order.swap(next_order);
            for (std::size_t i=0;i<applied;++i) std::swap(changes[i].state->control,changes[i].value);
            if (inserted) for (auto key:added_keys) states.erase(key);
            states.merge(removed);
            tooltip_hover=old_tooltip;
            if (interaction_changes) {
                press=std::move(old_press); edit=std::move(old_edit);
                focus=old_focus; hover=old_hover; keyboard_pressed=old_keyboard;
                keyboard_key=old_keyboard_key; terminal_key=old_terminal;
                popup=old_popup; popup_return_focus=old_popup_focus; popup_dismissed=old_dismissed;
            }
            return std::unexpected(changed.error());
        }
        // No allocation or callbacks after this point. Text resources remain
        // owned by old frames even when the mutable store drops its cache.
        for (const auto& [key,state]:removed) {
            text.erase(key);
            text.erase(detail::TextKey{key,0,true});
            for (auto text_key:state.table_text) text.erase(text_key);
        }
        for (auto& [key,index]:table_indices) {
            auto& state=states.at(key);
            if (state.table_index!=index) {
                if (!state.control.table_selection.row) state.scroll=state.scroll_x=0;
                else state.table_reveal=true;
            }
            state.table_index.swap(index);
        }
        for (auto& change:changes) {
            auto& state=*change.state;
            state.paint_range.frame_id=0;
            const auto& before=change.value; const auto& after=state.control;
            state.table_reveal|=before.table_selection!=after.table_selection || before.table_layout!=after.table_layout;
            if (before.code!=after.code) {state.scroll=state.scroll_x=0;state.code_width=0;state.code_selection_revision=0;}
            if ((before.table && !after.table) || (before.code && before.code!=after.code)) {
                for (auto text_key:state.table_text) text.erase(text_key);
                state.table_text.clear(); state.table_index.clear(); state.scroll_x=state.scroll_x_limit=0;
            }
            if (text_kind(before.kind) && !text_kind(after.kind)) text.erase(after.key);
            if (!before.placeholder.empty() && (after.kind!=ControlKind::text_edit || before.placeholder!=after.placeholder))
                text.erase(detail::TextKey{after.key,0,true});
            state.style_dirty|=before.parent!=after.parent || before.kind!=after.kind || before.role!=after.role ||
                before.classes!=after.classes || before.style!=after.style || before.style_scope!=after.style_scope;
            state.layout_dirty|=before.kind!=after.kind || before.layout!=after.layout || before.text!=after.text ||
                before.single_line!=after.single_line || before.tree_index!=after.tree_index ||
                before.tree_row_count!=after.tree_row_count || before.tree_badges!=after.tree_badges ||
                before.tree_trailing_icon!=after.tree_trailing_icon || before.rename_anchor!=after.rename_anchor ||
                (text_field(after.kind) && (before.value!=after.value || before.single_precision!=after.single_precision ||
                                            before.integral!=after.integral));
            if (!after.tree_primary) state.tree_reveal=false;
        }
        for (auto key:tree_reveals) states.find(key)->second.tree_reveal=true;
        structure_dirty|=structural;
        controls_revision=revision; ++control_batches;
        if (published) validate_tooltip(*published);
        controls_updated+=changes.size(); controls_created+=added_keys.size(); controls_removed+=removed_keys.size();
        reply.redraw=true;
        return reply;
    }
