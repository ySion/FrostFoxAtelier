    void update_trees() {
        if (!std::exchange(trees_dirty,false)) return;
        for (auto key:order) {
            auto& state=states.at(key);
            if (state.control.kind!=ControlKind::tree_item) { state.tree_hidden=false; state.tree_enabled=true; continue; }
            state.tree_branch=false; state.tree_next=0;
            auto* parent=state.control.tree_parent?live(state.control.tree_parent):nullptr;
            state.tree_depth=parent?parent->tree_depth+1:0;
            state.tree_enabled=state.control.enabled && (!parent || parent->tree_enabled);
            const auto& filter=states.at(state.control.parent).control.tree_filter;
            const auto fold=[](wchar_t c) { return c>=L'A' && c<=L'Z'?static_cast<wchar_t>(c+L'a'-L'A'):c; };
            state.tree_match=filter.empty() || (parent && parent->tree_match) ||
                std::search(state.control.text.begin(),state.control.text.end(),filter.begin(),filter.end(),
                    [&](wchar_t a,wchar_t b){return fold(a)==fold(b);})!=state.control.text.end();
            if (parent) parent->tree_branch=true;
        }
        // Matching leaves retain their ancestors; matching branches retain their subtree.
        std::map<std::pair<ControlKey,ControlKey>,ControlKey> next;
        for (auto it=order.rbegin();it!=order.rend();++it) {
            auto& state=states.at(*it);
            if (state.control.kind!=ControlKind::tree_item) continue;
            if (state.tree_match && state.control.tree_parent) states.at(state.control.tree_parent).tree_match=true;
            if (!state.tree_match || state.control.layout.collapsed) continue;
            auto& sibling=next[{state.control.parent,state.control.tree_parent}];
            state.tree_next=sibling; sibling=*it;
        }
        for (auto key:order) {
            auto& state=states.at(key);
            if (state.control.kind!=ControlKind::tree_item) continue;
            const auto* parent=state.control.tree_parent?live(state.control.tree_parent):nullptr;
            const bool filtering=!states.at(state.control.parent).control.tree_filter.empty();
            state.tree_hidden=state.control.layout.collapsed || !state.tree_match ||
                (parent && (parent->tree_hidden || (!parent->tree_expanded && !filtering)));
        }
    }
    ui_layout::Insets padding_for(const State& state) const {
        auto padding=effective_padding(state.control,*state.resolved);
        if (state.control.kind==ControlKind::tree_item)
            padding.left+=(state.tree_depth+1)*states.at(state.control.parent).resolved->tree_indent;
        return padding;
    }
    const TreeRowGeometry* tree_row(ControlKey key) const {
        if (!published) return nullptr;
        const auto at=std::find_if(published->tree_rows.begin(),published->tree_rows.end(),[&](const auto& row){return row.key==key;});
        return at==published->tree_rows.end()?nullptr:&*at;
    }
    void expand_tree(Reply& reply,ControlKey key,bool expanded) {
        auto& state=states.at(key);
        if (!state.tree_branch || !states.at(state.control.parent).control.tree_filter.empty()) return;
        state.tree_expanded=expanded;
        trees_dirty=true;
        update_trees();
        if (const auto* focused=live(focus);focused && focused->tree_hidden) set_focus(reply,key);
        retire_keyboard(reply); reply.redraw=reply.consumed=true;
    }
    void select_tree(Reply& reply,const ControlBox& box,SelectionModifiers modifiers={}) {
        set_focus(reply,box.key);
        auto& tree=states.at(states.at(box.key).control.parent);
        if (box.bounds.y<box.clip.top) tree.scroll-=(box.clip.top-box.bounds.y)/surface.dpi_scale;
        if (box.bounds.y+box.bounds.height>box.clip.bottom) tree.scroll+=(box.bounds.y+box.bounds.height-box.clip.bottom)/surface.dpi_scale;
        tree.scroll=std::clamp(tree.scroll,0.0,tree.scroll_limit);
        action(reply,ActionKind::select,box.key,modifiers);
        reply.redraw=reply.consumed=true;
    }
    bool tree_keyboard(Reply& reply,const Event& event,State& focused) {
        if (focused.control.kind!=ControlKind::tree_item || event.kind!=EventKind::key_down || event.control || event.alt ||
            (event.key!=Key::up && event.key!=Key::down && event.key!=Key::left && event.key!=Key::right && event.key!=Key::home && event.key!=Key::end)) return false;
        reply.consumed=true;
        const auto* row=tree_row(focus);
        if (!row) return true;
        const auto owner=focused.control.parent;
        std::vector<const ControlBox*> rows;
        for (const auto& box:published->controls)
            if (box.kind==ControlKind::tree_item && box.parent==owner && box.bounds.height>0 && current(box)) rows.push_back(&box);
        const auto at=std::find_if(rows.begin(),rows.end(),[&](const auto* box){return box->key==focus;});
        if (at==rows.end()) return true;
        auto target=at;
        if (event.key==Key::left) {
            if (focused.tree_branch && row->expanded && !row->filtering) expand_tree(reply,focus,false);
            else if (row->parent) target=std::find_if(rows.begin(),rows.end(),[&](const auto* box){return box->key==row->parent;});
        } else if (event.key==Key::right) {
            if (focused.tree_branch && !row->expanded && !row->filtering) expand_tree(reply,focus,true);
            else if (focused.tree_branch && at+1!=rows.end() && states.at((*(at+1))->key).control.tree_parent==focus) target=at+1;
        } else if (event.key==Key::home) target=rows.begin();
        else if (event.key==Key::end) target=rows.end()-1;
        else if (event.key==Key::up && at!=rows.begin()) --target;
        else if (event.key==Key::down && at+1!=rows.end()) ++target;
        if (target!=at && target!=rows.end()) select_tree(reply,**target,{event.shift,event.control});
        return true;
    }
