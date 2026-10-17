void shared_tree_view() {
    for (float dpi:{1.f,1.5f,1.75f,2.f}) {
        Consumer c;
        accepted(c.ui.set_surface({{640,480},dpi,2}));
        auto sheet=ui::StyleSheet::parse("tree { tree-row-height: 20dip; tree-indent: 18dip; tree-alternate-color: #363636; tree-line-color: #686868; } tree_item { padding: 0dip 2dip; border-width: 0dip; }");
        require(sheet.has_value(),"tree SS did not parse"); accepted(c.ui.set_style_sheet(*sheet));
        ui::Control tree; tree.key=1; tree.kind=ui::ControlKind::tree;
        tree.layout=ui_layout::NodeInput::column(); tree.layout.width=ui_layout::Axis::fixed(280); tree.layout.height=ui_layout::Axis::fixed(100);
        c.controls={tree};
        const auto append=[&](ui::ControlKey key,ui::ControlKey parent,std::wstring name) {
            ui::Control row; row.key=key; row.parent=1; row.kind=ui::ControlKind::tree_item;
            row.tree_parent=parent; row.text=std::move(name); row.layout=ui_layout::NodeInput::box(0,20); row.layout.width=ui_layout::Axis::expand();
            c.controls.push_back(std::move(row));
        };
        append(2,0,L"World"); append(3,2,L"Entity"); c.controls.back().tree_initially_expanded=false;
        append(4,3,L"Transform 位置"); append(5,3,L"Cube");
        for (unsigned i=0;i<20;++i) append(10+i,2,L"Entity "+std::to_wstring(i));
        c.prepare();
        const auto disclosure=[&](ui::ControlKey key) {
            const auto at=std::find_if(c.frame->tree_rows.begin(),c.frame->tree_rows.end(),[&](auto row){return row.key==key;});
            require(at!=c.frame->tree_rows.end(),"tree row geometry missing"); return at->disclosure;
        };
        const auto toggle=[&](ui::ControlKey key) {
            const auto box=disclosure(key); require(box.width>0,"branch has no disclosure hit area");
            const auto x=box.x+box.width*.5f,y=box.y+box.height*.5f;
            require(c.ui.input({.kind=ui::EventKind::pointer_down,.x=x,.y=y}).capture==ui::Capture::acquire,"disclosure did not capture");
            auto up=c.ui.input({.kind=ui::EventKind::pointer_up,.x=x,.y=y});
            require(up.capture==ui::Capture::release && actions_of(up).empty(),"expansion changed business selection");
        };
        require(!c.frame->find(4) && c.frame->scrollbars.size()==1,"initial tree collapse/overflow wrong");
        require(std::abs(c.frame->find(3)->bounds.height-20*dpi)<.01f,"tree row height drifted with DPI");
        toggle(3); c.prepare();
        require(c.frame->find(4) && disclosure(4).width==0,"expanded child missing or leaf shows an expander");
        auto selected=actions_of(c.click(4));
        require(selected.size()==1 && selected[0].kind==ui::ActionKind::select && selected[0].key==4,"tree row did not select");
        auto left=actions_of(key(c.ui,ui::Key::left));
        require(left.size()==1 && left[0].key==3,"Left did not select the parent");
        key(c.ui,ui::Key::left); c.prepare(); require(!c.frame->find(4),"Left did not collapse branch");
        key(c.ui,ui::Key::right); c.prepare(); require(c.frame->find(4),"Right did not expand branch");
        auto right=actions_of(key(c.ui,ui::Key::right)); require(right.size()==1 && right[0].key==4,"Right did not enter first child");
        auto end=actions_of(key(c.ui,ui::Key::end)); require(end.size()==1 && end[0].key==29,"End skipped final row");
        c.prepare(); require(c.frame->find(29)->bounds.y+c.frame->find(29)->bounds.height<=c.frame->find(29)->clip.bottom+1,"keyboard selection was not revealed");
        key(c.ui,ui::Key::home); c.prepare(); toggle(3); c.prepare();
        c.controls[0].tree_filter=L"位置"; c.prepare();
        require(c.frame->find(2) && c.frame->find(3) && c.frame->find(4) && !c.frame->find(5) && c.frame->scrollbars.empty(),"filter lost ancestry or retained unmatched rows/scrollbar");
        c.controls[0].tree_filter.clear(); c.prepare(); require(!c.frame->find(4),"filter overwrote retained expansion");
        toggle(3); c.prepare();
        const auto old=c.frame;
        const auto box=c.frame->find(4)->bounds;
        c.ui.input({.kind=ui::EventKind::pointer_down,.x=box.x+box.width*.5f,.y=box.y+box.height*.5f});
        c.controls[2].enabled=false;
        auto disabled=accepted(c.ui.set_controls(c.controls));
        require(disabled.capture==ui::Capture::release,"disabled logical ancestor retained child capture");
        require(actions_of(c.ui.input({.kind=ui::EventKind::pointer_up,.x=box.x+box.width*.5f,.y=box.y+box.height*.5f})).empty(),"disabled child emitted stale selection");
        c.controls[2].enabled=true; c.prepare();
        const auto incarnation=c.frame->find(4)->incarnation;
        auto child=c.controls[3]; c.controls.erase(c.controls.begin()+3); c.prepare();
        c.controls.insert(c.controls.begin()+3,child); c.prepare();
        require(c.frame->find(4)->incarnation!=incarnation && old->find(4),"retired tree row retained identity or mutated an old frame");
        auto invalid=c.controls; invalid[2].tree_parent=5;
        require(!c.ui.set_controls(invalid),"tree accepted a forward/cyclic logical parent");
        toggle(3); c.prepare(); require(!c.frame->find(4),"programmatic selection fixture did not collapse its ancestor");
        c.controls[3].selected=true; c.prepare();
        require(c.frame->find(4) && c.frame->find(4)->bounds.y>=c.frame->find(4)->clip.top,
            "host selection did not expand and reveal its logical ancestry");
        toggle(3); c.prepare(); c.prepare();
        require(!c.frame->find(4),"stable selected projection reopened a manually collapsed ancestor");
        c.controls[3].selected=false; c.controls.back().selected=true; c.prepare();
        require(c.frame->find(29)->bounds.y+c.frame->find(29)->bounds.height<=c.frame->find(29)->clip.bottom+1,
            "host selection did not scroll to its final row");
        c.controls.back().selected=false; c.controls[3].selected=true; c.prepare();
        require(c.frame->find(4) && c.frame->find(4)->bounds.y>=c.frame->find(4)->clip.top &&
            c.frame->find(4)->bounds.y+c.frame->find(4)->bounds.height<=c.frame->find(4)->clip.bottom+1,
            "a new host selection did not reopen its ancestor and scroll back to the row");
        c.ui.focus(4);c.ui.input({.kind=ui::EventKind::key_down,.key=ui::Key::space});
        c.controls[0].tree_filter=L"nothing matches"; c.prepare();
        require(c.frame->tree_rows.empty() && c.frame->scrollbars.empty(),"empty filter result kept interactive rows");
        require(c.ui.focused_control()==0 && !c.ui.interaction_state().keyboard,"tree filtering retained a hidden row's keyboard activation");
        const auto filtered_release=c.ui.input({.kind=ui::EventKind::key_up,.key=ui::Key::space});
        require(filtered_release.consumed && actions_of(filtered_release).empty() && !c.ui.interaction_state().active(),
            "hidden tree activation required its old focus to receive key-up");
    }
    Consumer drag;
    auto tree=leaf(80,ui::ControlKind::tree,0);tree.layout=ui_layout::NodeInput::column();tree.layout.width=ui_layout::Axis::fixed(240);
    auto row=leaf(81,ui::ControlKind::tree_item,0,L"Drag Entity");row.parent=80;row.tree_draggable=true;
    drag.controls={tree,row};drag.prepare();
    for(const auto ending:{ui::EventKind::key_down,ui::EventKind::focus_lost,ui::EventKind::capture_lost}) {
        const auto b=drag.frame->find(81)->bounds;const auto x=b.x+b.width*.5f,y=b.y+b.height*.5f;
        drag.ui.input({.kind=ui::EventKind::pointer_down,.x=x,.y=y});
        const auto begun=actions_of(drag.ui.input({.kind=ui::EventKind::pointer_move,.x=x+25,.y=y}));
        require(std::ranges::any_of(begun,[](const auto& a){return a.kind==ui::ActionKind::drag_begin && a.key==81;}),"Tree drag did not begin.");
        const auto cancelled=drag.ui.input({.kind=ending,.key=ending==ui::EventKind::key_down?ui::Key::escape:ui::Key::none});
        const auto actions=actions_of(cancelled);
        require(cancelled.capture==ui::Capture::release && std::ranges::any_of(actions,[](const auto& a){return a.kind==ui::ActionKind::drag_cancel && a.key==81;}) &&
            std::ranges::none_of(actions,[](const auto& a){return a.kind==ui::ActionKind::edit_cancel;}),"Tree drag cancellation did not use its drag protocol.");
        drag.ui.input({.kind=ui::EventKind::key_up,.key=ui::Key::escape});drag.prepare();
    }
}
