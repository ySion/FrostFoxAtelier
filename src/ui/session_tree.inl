    ui_layout::Insets padding_for(const State& state) const {
        auto padding=effective_padding(state.control,*state.resolved);
        if (state.control.kind==ControlKind::tree_item)
            padding.left+=(state.control.tree_depth+1)*states.at(state.control.parent).resolved->tree_indent;
        return padding;
    }
    const TreeRowGeometry* tree_row(ControlKey key) const {
        if (!published) return nullptr;
        const auto at=std::find_if(published->tree_rows.begin(),published->tree_rows.end(),[&](const auto& row){return row.key==key;});
        return at==published->tree_rows.end()?nullptr:&*at;
    }
    // Expansion belongs to the host; Session only requests it.
    void expand_tree(Reply& reply,ControlKey key,bool expanded) {
        const auto& row=states.at(key).control;
        if (!row.tree_branch || row.tree_expanded==expanded) return;
        // Focus inside a collapsing subtree moves to the collapsed row. Hosts keep the
        // focused row's ancestors described, so a shallower row between them proves otherwise.
        if (const auto* focused=live(focus);!expanded && focused && focus!=key && focused->control.kind==ControlKind::tree_item &&
            focused->control.parent==row.parent && focused->control.tree_index>row.tree_index && focused->control.tree_depth>row.tree_depth &&
            std::none_of(order.begin(),order.end(),[&](ControlKey other) {
                const auto& control=states.at(other).control;
                return control.kind==ControlKind::tree_item && control.parent==row.parent && control.tree_index>row.tree_index &&
                    control.tree_index<focused->control.tree_index && control.tree_depth<=row.tree_depth;
            })) set_focus(reply,key);
        action(reply,ActionKind::expand,key,expanded);
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
        // Accepted rows of this tree in host order; tree_rows parallels the tree_item boxes.
        struct Row { const ControlBox* box; const TreeRowGeometry* geometry; };
        std::vector<Row> rows;
        std::size_t next{};
        for (const auto& box:published->controls) {
            if (box.kind!=ControlKind::tree_item) continue;
            const auto& geometry=published->tree_rows[next++];
            if (box.parent==focused.control.parent && box.bounds.height>0 && current(box)) rows.push_back({&box,&geometry});
        }
        const auto at=std::find_if(rows.begin(),rows.end(),[&](const auto& row){return row.box->key==focus;});
        if (at==rows.end()) return true;
        const auto& row=*at->geometry;
        const bool branch=focused.control.tree_branch;
        auto target=at;
        if (event.key==Key::left) {
            if (branch && row.expanded) expand_tree(reply,focus,false);
            else for (auto parent=at;row.depth && parent!=rows.begin();)
                if ((--parent)->geometry->depth+1==row.depth) {target=parent;break;}
        } else if (event.key==Key::right) {
            if (branch && !row.expanded) expand_tree(reply,focus,true);
            else if (branch && at+1!=rows.end() && (at+1)->geometry->index==row.index+1 && (at+1)->geometry->depth==row.depth+1) target=at+1;
        } else if (event.key==Key::home) target=rows.begin();
        else if (event.key==Key::end) target=rows.end()-1;
        else if (event.key==Key::up && at!=rows.begin()) --target;
        else if (event.key==Key::down && at+1!=rows.end()) ++target;
        if (target!=at) select_tree(reply,*target->box,{event.shift,event.control});
        return true;
    }
