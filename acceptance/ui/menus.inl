void shared_menu_semantics() {
    Consumer c;
    std::array items{ui::MenuItem{1,L"Grid",true,true,{}},ui::MenuItem{2,L"Axes",true,false,{}},
        ui::MenuItem{3,L"Solid",true,true,{}},ui::MenuItem{4,L"Wire",true,false,{}},
        ui::MenuItem{5,L"Search",false,false,{}},ui::MenuItem{6,L"Triangles",true,true,{}}};
    items[0].behavior=items[1].behavior=ui::MenuBehavior::checkbox; items[1].icon=ui::ControlIcon::axes;
    for (auto index:{2,3,5}) { items[index].behavior=ui::MenuBehavior::radio; items[index].radio_group=index==5?80:70; }
    items[2].icon=ui::ControlIcon::mesh; items[3].icon=ui::ControlIcon::world; items[4].icon=ui::ControlIcon::search;
    const auto open=[&] {
        accepted(c.ui.set_controls({})); c.controls.clear(); ui::append_menu(c.controls,500,0,0,0,240,items); c.prepare();
    };
    const auto marker=[&](ui::ControlKey key)->std::shared_ptr<const ui::Coverage> {
        const auto& box=*c.frame->find(key); const auto scale=c.frame->drawing->dpi_scale;
        const auto right=box.bounds.x+(box.style->padding.left+box.style->indicator_size+3)*scale;
        for (const auto& draw:c.frame->drawing->draws) if (draw.coverage && draw.bounds.x<right &&
            draw.bounds.y>=box.bounds.y && draw.bounds.y+draw.bounds.height<=box.bounds.y+box.bounds.height) return draw.coverage;
        return {};
    };
    const auto activation=[](const ui::Reply& reply) {
        const auto actions=actions_of(reply);
        const auto at=std::find_if(actions.begin(),actions.end(),[](const auto& action){return action.kind==ui::ActionKind::activate;});
        require(at!=actions.end(),"menu did not activate"); return *at;
    };
    open();
    const auto checked_box=marker(1),unchecked_box=marker(2),checked_radio=marker(3),unchecked_radio=marker(4);
    require(checked_box && unchecked_box && checked_radio && unchecked_radio && checked_box->pixels!=unchecked_box->pixels &&
        checked_radio->pixels!=unchecked_radio->pixels && checked_box->pixels!=checked_radio->pixels && unchecked_box->pixels!=unchecked_radio->pixels,
        "menu checkbox/radio markers did not visibly distinguish kind and checked state");
    require(c.frame->find(3)->menu_behavior==ui::MenuBehavior::radio && c.frame->find(3)->radio_group==70,
        "menu semantics were not retained in the accepted frame");
    const auto disabled=c.frame->find(5)->bounds;
    require(c.ui.input({.kind=ui::EventKind::pointer_down,.x=disabled.x+4,.y=disabled.y+4}).capture!=ui::Capture::acquire,
        "disabled icon menu item captured input");
    const auto hover=c.frame->find(2)->bounds;
    c.ui.input({.kind=ui::EventKind::pointer_move,.x=hover.x+4,.y=hover.y+4}); c.prepare();
    require(c.frame->find(1)->selected && !c.frame->find(2)->selected && marker(1)->pixels==checked_box->pixels && marker(2)->pixels==unchecked_box->pixels,
        "menu hover changed checked state");
    const auto hovered=c.frame->find(2)->style->background;
    const auto checked=activation(c.click(1)); require(!std::get<bool>(checked.value),"checkbox menu did not request its unchecked value");
    c.prepare(); require(!c.frame->find(1),"dismissed menu retained an interactive row");
    items[0].selected=false; open();
    require(marker(1)->pixels==unchecked_box->pixels && c.frame->find(2)->style->background!=hovered,"reopened menu retained its old check or hover state");
    auto request=activation(c.click(4)); require(std::get<bool>(request.value),"radio item did not request selection");
    items[2].selected=false; items[3].selected=true; open();
    require(marker(3)->pixels==unchecked_radio->pixels && marker(4)->pixels==checked_radio->pixels && marker(6)->pixels==checked_radio->pixels,
        "radio selection changed another group or failed to move its marker");
    c.ui.focus(4); c.ui.input({.kind=ui::EventKind::key_down,.key=ui::Key::enter});
    request=activation(c.ui.input({.kind=ui::EventKind::key_up,.key=ui::Key::enter}));
    require(std::get<bool>(request.value),"keyboard activation toggled a selected radio item off");
    open();
    auto invalid=c.controls; for (auto& control:invalid) if (control.key==3) control.selected=true;
    require(!c.ui.set_controls(invalid),"one radio group accepted two selected items");
    auto changed=*std::find_if(c.controls.begin(),c.controls.end(),[](const auto& control){return control.key==1;});
    changed.selected=true; accepted(c.ui.update_controls(std::span(&changed,1)));
    require(std::get<bool>(activation(c.click(1)).value),"checkbox activation used an unaccepted checked value");
    open(); c.ui.focus(4); c.ui.input({.kind=ui::EventKind::key_down,.key=ui::Key::space});
    changed=*std::find_if(c.controls.begin(),c.controls.end(),[](const auto& control){return control.key==4;});
    changed.radio_group=90; accepted(c.ui.update_controls(std::span(&changed,1)));
    require(actions_of(c.ui.input({.kind=ui::EventKind::key_up,.key=ui::Key::space})).empty(),"retargeted radio group retained its held key");

    Consumer icons; auto face=leaf(10,ui::ControlKind::button,10); face.layout.width=ui_layout::Axis::fixed(30); face.layout.height=ui_layout::Axis::fixed(30);
    const auto coverages=[](const ui::UiFrame& frame) {return std::count_if(frame.drawing->draws.begin(),frame.drawing->draws.end(),[](const auto& draw){return static_cast<bool>(draw.coverage);});};
    for (auto icon:{ui::ControlIcon::search,ui::ControlIcon::view_list,ui::ControlIcon::view_details,ui::ControlIcon::view_tiles,ui::ControlIcon::filter,ui::ControlIcon::more}) {
        face.kind=ui::ControlKind::button; face.icon=icon; icons.controls={face}; icons.prepare(); const auto plain=coverages(*icons.frame);
        face.kind=ui::ControlKind::combo; icons.controls={face}; icons.prepare();
        require(plain==1 && coverages(*icons.frame)==plain+1 && icons.frame->find(10)->bounds.width==30,
            "compact icon menu lost its icon/corner indicator or reserved a full dropdown column");
    }
}
