// A minimal tree host: it owns expansion, filtering and the primary row, and
// describes only the rows tree_window selects, as editor trees do.
struct TreeHost {
    Consumer c;
    std::vector<ui::ControlKey> keys;
    std::vector<std::wstring> names;
    std::vector<ui::TreeNode> nodes;
    std::vector<ui::TreeRow> rows;
    std::wstring filter;
    ui::ControlKey primary{}, disabled{};
    void add(ui::ControlKey key,ui::ControlKey parent,std::wstring name,bool expanded=true) {
        nodes.push_back({parent?node(parent):ui::no_tree_node,expanded,{}});
        keys.push_back(key);names.push_back(std::move(name));
    }
    std::uint32_t node(ui::ControlKey key) const {
        return static_cast<std::uint32_t>(std::ranges::find(keys,key)-keys.begin());
    }
    std::vector<ui::Control> controls() {
        for (std::size_t i=0;i<nodes.size();++i) nodes[i].text=names[i];
        ui::flatten_tree(nodes,filter,rows);
        std::vector<std::uint32_t> pinned;
        for (const auto key:{primary,c.ui.focused_control()})
            for (std::uint32_t i=0;i<rows.size();++i) if (keys[rows[i].node]==key) pinned.push_back(i);
        std::vector<ui::Control> result{c.controls.front()};
        result[0].tree_row_count=static_cast<std::uint32_t>(rows.size());
        for (const auto index:ui::tree_window(rows,c.ui.tree_viewport(1),pinned)) {
            const auto& row=rows[index];
            ui::Control item;item.key=keys[row.node];item.parent=1;item.kind=ui::ControlKind::tree_item;item.text=names[row.node];
            item.layout=ui_layout::NodeInput::column();item.layout.width=ui_layout::Axis::expand();
            item.tree_index=index;item.tree_depth=row.depth;item.tree_branch=row.branch;item.tree_expanded=row.expanded;item.tree_lines=row.lines;
            item.selected=item.tree_primary=item.key==primary;item.enabled=item.key!=disabled;
            result.push_back(std::move(item));
        }
        return result;
    }
    ui::Reply describe() {
        c.controls=controls();
        auto reply=accepted(c.ui.set_controls(c.controls));
        c.frame=accepted(c.ui.prepare());require(c.ui.accept(c.frame),"tree candidate not accepted");
        return reply;
    }
    // Selection becomes the host's primary row; expansion requests update the host's nodes.
    std::vector<ui::Action> apply(const ui::Reply& reply) {
        auto actions=actions_of(reply);
        for (const auto& action:actions) {
            if (action.kind==ui::ActionKind::select) primary=action.key;
            if (action.kind==ui::ActionKind::expand) nodes[node(action.key)].expanded=std::get<bool>(action.value);
        }
        describe();
        return actions;
    }
    const ui::TreeRowGeometry& geometry(ui::ControlKey key) const {
        const auto at=std::ranges::find(c.frame->tree_rows,key,&ui::TreeRowGeometry::key);
        require(at!=c.frame->tree_rows.end(),"tree row geometry missing");return *at;
    }
    bool shown(ui::ControlKey key) const {
        const auto* box=c.frame->find(key);
        return box && box->bounds.y>=box->clip.top-1 && box->bounds.y+box->bounds.height<=box->clip.bottom+1;
    }
    ui::Reply toggle(ui::ControlKey key) {
        const auto box=geometry(key).disclosure;require(box.width>0,"branch has no disclosure hit area");
        const auto x=box.x+box.width*.5f,y=box.y+box.height*.5f;
        require(c.ui.input({.kind=ui::EventKind::pointer_down,.x=x,.y=y}).capture==ui::Capture::acquire,"disclosure did not capture");
        auto up=c.ui.input({.kind=ui::EventKind::pointer_up,.x=x,.y=y});
        const auto actions=actions_of(up);
        require(up.capture==ui::Capture::release && actions.size()==1 && actions[0].kind==ui::ActionKind::expand && actions[0].key==key,
            "disclosure did not request expansion alone");
        return up;
    }
};
void shared_tree_view() {
    for (float dpi:{1.f,1.5f,1.75f,2.f}) {
        TreeHost h;auto& c=h.c;
        accepted(c.ui.set_surface({{640,480},dpi,2}));
        auto sheet=ui::StyleSheet::parse("tree { tree-row-height: 20dip; tree-indent: 18dip; tree-alternate-color: #363636; tree-line-color: #686868; } tree_item { padding: 0dip 2dip; border-width: 0dip; }");
        require(sheet.has_value(),"tree SS did not parse"); accepted(c.ui.set_style_sheet(*sheet));
        ui::Control tree; tree.key=1; tree.kind=ui::ControlKind::tree;
        tree.layout=ui_layout::NodeInput::column(); tree.layout.width=ui_layout::Axis::fixed(280); tree.layout.height=ui_layout::Axis::fixed(100);
        c.controls={tree};
        h.add(2,0,L"World"); h.add(3,2,L"Entity",false); h.add(4,3,L"Transform 位置"); h.add(5,3,L"Cube");
        for (unsigned i=0;i<20;++i) h.add(10+i,2,L"Entity "+std::to_wstring(i));
        // Before a first layout the host windows over the surface height; the next description uses the tree's.
        h.describe(); require(c.frame->find(28),"first description did not cover the surface");
        h.describe();
        // 22 host rows in a five-row viewport: the window and one screen below it, plus the last row (Entity i is row i+2).
        require(!c.frame->find(4) && c.frame->scrollbars.size()==1,"initial tree collapse/overflow wrong");
        require(std::abs(c.frame->find(3)->bounds.height-20*dpi)<.01f,"tree row height drifted with DPI");
        const auto window_end=static_cast<ui::ControlKey>(std::floor(2*c.ui.tree_viewport(1).height/20));
        require(window_end<19 && c.frame->find(8+window_end) && !c.frame->find(9+window_end) && !c.frame->find(28) && c.frame->find(29),
            "tree described rows outside its window");
        require(std::abs(c.frame->find(29)->bounds.y-c.frame->find(2)->bounds.y-21*20*dpi)<.05f &&
            std::abs(c.frame->find(29)->bounds.width-c.frame->find(2)->bounds.width)<.01f,"a described row left its host position");
        require(h.geometry(3).index==1 && h.geometry(3).depth==1 && !h.geometry(3).expanded && h.geometry(29).index==21,"tree row geometry lost its host position");
        h.toggle(3); c.prepare();
        require(!c.frame->find(4),"Session expanded a host-owned branch itself");
        h.apply(h.toggle(3));
        require(c.frame->find(4) && h.geometry(4).disclosure.width==0 && h.geometry(4).depth==2,"expanded child missing or leaf shows an expander");
        h.apply(c.click(4));
        require(h.primary==4 && c.ui.focused_control()==4,"tree row did not select");
        auto left=h.apply(key(c.ui,ui::Key::left));
        require(left.size()==1 && left[0].kind==ui::ActionKind::select && left[0].key==3,"Left did not select the parent");
        left=h.apply(key(c.ui,ui::Key::left));
        require(left.size()==1 && left[0].kind==ui::ActionKind::expand && !std::get<bool>(left[0].value) && !c.frame->find(4),"Left did not collapse branch");
        h.apply(key(c.ui,ui::Key::right)); require(c.frame->find(4),"Right did not expand branch");
        auto right=h.apply(key(c.ui,ui::Key::right)); require(right.size()==1 && right[0].key==4,"Right did not enter first child");
        // Collapsing an ancestor moves focus to it, so the host stops describing the hidden row.
        h.apply(h.toggle(3));
        require(c.ui.focused_control()==3 && !c.frame->find(4),"collapse left focus in a hidden row");
        h.apply(h.toggle(3));
        auto end=h.apply(key(c.ui,ui::Key::end)); require(end.size()==1 && end[0].key==29,"End skipped final row");
        require(h.shown(29),"keyboard selection was not revealed");
        require(c.frame->find(2) && !c.frame->find(10),"scrolled window kept rows above it");
        h.apply(key(c.ui,ui::Key::home)); require(h.shown(2) && h.primary==2,"Home did not reveal the first row");
        h.apply(key(c.ui,ui::Key::down)); require(h.primary==3,"Down did not select the next row");
        // Wheel scrolling covers every host row; the host then describes the newly visible ones.
        const auto box=c.frame->find(1)->bounds;
        require(c.ui.input({.kind=ui::EventKind::wheel,.x=box.x+20,.y=box.y+20,.wheel=-1000}).consumed,"tree did not scroll");
        c.prepare();
        require(std::abs(c.ui.tree_viewport(1).scroll-(24*20-c.ui.tree_viewport(1).height))<.5,"tree scroll range ignored undescribed rows");
        h.describe(); require(h.shown(28) && h.shown(29),"scrolled rows were not described");
        // Arrow keys before the next description stop at the end of the focused row's described neighbours.
        const auto first_down=actions_of(key(c.ui,ui::Key::down)),second_down=actions_of(key(c.ui,ui::Key::down));
        require(first_down.size()==1 && first_down[0].key==4 && second_down.empty(),"Down stepped across undescribed rows");
        // A primary row is revealed once when it becomes primary, not on every description.
        h.primary=4;h.describe();
        require(h.shown(4),"a new primary row was not revealed");
        c.ui.input({.kind=ui::EventKind::wheel,.x=box.x+20,.y=box.y+20,.wheel=-1000});h.describe();h.describe();
        require(!c.frame->find(4) || !h.shown(4),"an unchanged primary row pulled the scroll back");
        c.ui.input({.kind=ui::EventKind::wheel,.x=box.x+20,.y=box.y+20,.wheel=1000});h.describe();
        h.apply(h.toggle(3));
        h.filter=L"位置"; h.describe();
        require(c.frame->find(2) && c.frame->find(3) && c.frame->find(4) && !c.frame->find(5) && h.geometry(3).expanded && c.frame->scrollbars.empty(),
            "filter lost ancestry or retained unmatched rows/scrollbar");
        h.filter=L"cube"; h.describe();
        require(c.frame->find(5) && !c.frame->find(4) && h.geometry(5).index==2,"filter did not fold ASCII case or kept an unmatched sibling");
        h.filter.clear(); h.describe(); require(!c.frame->find(4),"filter overwrote host expansion");
        h.apply(h.toggle(3));
        // Disabling a pressed row cancels its press without a stale selection.
        const auto old=c.frame;
        const auto row=c.frame->find(4)->bounds;
        c.ui.input({.kind=ui::EventKind::pointer_down,.x=row.x+row.width*.5f,.y=row.y+row.height*.5f});
        h.disabled=4;
        require(h.describe().capture==ui::Capture::release,"disabled row retained capture");
        require(actions_of(c.ui.input({.kind=ui::EventKind::pointer_up,.x=row.x+row.width*.5f,.y=row.y+row.height*.5f})).empty(),"disabled row emitted stale selection");
        h.disabled=0; h.describe();
        const auto incarnation=c.frame->find(4)->incarnation;
        h.apply(h.toggle(3)); h.apply(h.toggle(3));
        require(c.frame->find(4)->incarnation!=incarnation && old->find(4),"a row leaving the window retained identity or mutated an old frame");
        // Session checks row order against the host's flattened positions.
        const auto rejected=[&](auto change,const char* message) {
            auto invalid=h.controls(); change(invalid); require(!c.ui.set_controls(invalid),message);
        };
        rejected([](auto& v){std::swap(v[1].tree_index,v[2].tree_index);},"tree accepted descending rows");
        rejected([](auto& v){v[2].tree_depth=2;},"tree accepted a row two levels below its predecessor");
        rejected([](auto& v){v[2].tree_index=1;v[2].tree_depth=1;v[1].tree_expanded=false;},"tree accepted a child under a collapsed predecessor");
        rejected([](auto& v){v[0].tree_row_count=v.back().tree_index;},"tree accepted a row beyond its row count");
        rejected([](auto& v){v[1].tree_depth=1;},"tree accepted a nested first row");
        rejected([](auto& v){v[4].tree_expanded=true;},"tree accepted an expanded leaf");
        rejected([](auto& v){v[2].tree_lines.set(2);},"tree accepted a guide below the row");
        auto moved=std::vector<ui::Control>{c.controls[2]};moved[0].tree_index+=1;
        require(!c.ui.update_controls(moved),"sparse update moved a tree row");
        c.ui.focus(4);c.ui.input({.kind=ui::EventKind::key_down,.key=ui::Key::space});
        h.filter=L"nothing matches"; h.describe();
        require(c.frame->tree_rows.empty() && c.frame->scrollbars.empty(),"empty filter result kept interactive rows");
        require(c.ui.focused_control()==0 && !c.ui.interaction_state().keyboard,"tree filtering retained a hidden row's keyboard activation");
        const auto filtered_release=c.ui.input({.kind=ui::EventKind::key_up,.key=ui::Key::space});
        require(filtered_release.consumed && actions_of(filtered_release).empty() && !c.ui.interaction_state().active(),
            "hidden tree activation required its old focus to receive key-up");
    }
    // Guides follow later siblings: Transform continues to Cube and Entity to Entity 0; Cube ends its branch.
    const std::array<ui::TreeNode,5> nodes{{{ui::no_tree_node,true,L"World"},{0,true,L"Entity"},{1,false,L"Transform"},{1,false,L"Cube"},{0,false,L"Entity 0"}}};
    std::vector<ui::TreeRow> rows;ui::flatten_tree(nodes,{},rows);
    require(rows.size()==5 && rows[2].lines==std::bitset<128>{0b110} && rows[3].lines==std::bitset<128>{0b10} &&
        rows[1].lines==std::bitset<128>{0b10} && rows[4].lines.none() && rows[1].branch && !rows[2].branch,"tree guides did not follow later siblings");
    const std::array pinned{3u};
    require(ui::tree_window(rows,{0,0,20},pinned)==std::vector<std::uint32_t>{0,1,2,3,4},"tree window lost a pinned row's ancestors or neighbours");
    const std::array<ui::TreeNode,2> unordered{{{1,false,L"Child"},{ui::no_tree_node,false,L"Parent"}}};
    bool threw=false;try{ui::flatten_tree(unordered,{},rows);}catch(const std::invalid_argument&){threw=true;}
    require(threw,"tree flattening accepted a child before its parent");
    // A content-height tree stops at its maximum and scrolls over the remaining host rows.
    Consumer bounded;
    auto list=leaf(90,ui::ControlKind::tree,0);list.layout=ui_layout::NodeInput::column();list.layout.width=ui_layout::Axis::fixed(240);
    list.layout.height=ui_layout::Axis::content(0,60);list.tree_row_count=10;bounded.controls={list};
    for (std::uint32_t i=0;i<3;++i) {
        auto item=leaf(91+i,ui::ControlKind::tree_item,0,L"Row");item.parent=90;item.tree_index=i;bounded.controls.push_back(item);
    }
    bounded.prepare();
    require(bounded.frame->find(90)->bounds.height<=60.5f && bounded.frame->scrollbars.size()==1,"a bounded content-height tree did not scroll its rows");
    Consumer drag;
    auto tree=leaf(80,ui::ControlKind::tree,0);tree.layout=ui_layout::NodeInput::column();tree.layout.width=ui_layout::Axis::fixed(240);tree.tree_row_count=1;
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
