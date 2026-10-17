void control_topology_identity() {
    Consumer c;
    ui::Control a; a.key=1; a.layout.width=ui_layout::Axis::fixed(260);
    ui::Control b=a; b.key=2; b.parent=1;
    auto field=leaf(3,ui::ControlKind::number,0); field.parent=2; field.value=2;
    auto button=leaf(4,ui::ControlKind::button,0,L"Retained button"); button.parent=1;
    c.controls={a,b,field,button}; c.prepare(); const auto held=c.frame;
    const auto coverage=first_text(*held).coverage; const auto pixels=coverage->pixels;
    const auto before=c.ui.statistics();
    c.click(3);
    const auto preview=actions_of(c.ui.input({.kind=ui::EventKind::text,.data=std::wstring{L"9"}}));
    require(!preview.empty() && number_value(preview.back())==9,"numeric draft was not started");
    b.parent=0; a.parent=2; field.parent=1; button.parent=2;
    c.controls={b,a,field,button};
    const auto moved=accepted(c.ui.set_controls(c.controls));
    require(moved.effects.empty() && c.ui.wants_keyboard(),"ancestor reversal cancelled a retained field's draft");
    prepare_current(c,false);
    require(c.ui.presented()==held && held->find(1)->parent==0 && held->find(2)->parent==1 && held->find(3)->parent==2,
        "structural preparation mutated or replaced the accepted frame");
    require(c.frame->find(2)->parent==0 && c.frame->find(1)->parent==2 && c.frame->find(3)->parent==1,
        "Session did not apply final ancestor reversal");
    for (const auto key:{1u,2u,3u,4u})
        require(c.frame->find(key)->incarnation==held->find(key)->incarnation,"retained control changed incarnation");
    const auto after=c.ui.statistics();
    require(after.layout_nodes_created==before.layout_nodes_created &&
        after.layout.topology_commits==before.layout.topology_commits+1,
        "structural sync recreated retained nodes or did not use one final-tree commit");
    require(c.ui.accept(c.frame),"reversed tree candidate was rejected");
    const auto committed=actions_of(key(c.ui,ui::Key::enter));
    require(committed.size()==1 && committed[0].kind==ui::ActionKind::edit_commit &&
        committed[0].key==3 && number_value(committed[0])==9,"retained draft did not survive final-tree migration");
    // The old parent disappears, while its children escape. A becomes a label
    // only after its final child has moved, without recreating either control.
    a=leaf(1,ui::ControlKind::label,50,L"Now a leaf");
    field.parent=0; field.value=9; button.parent=0; button.layout.offset_y=90;
    c.controls={a,field,button}; c.prepare();
    require(!c.frame->find(2) && c.frame->find(1)->kind==ui::ControlKind::label &&
        c.frame->find(3)->parent==0 && c.frame->find(3)->incarnation==held->find(3)->incarnation &&
        c.ui.statistics().layout_nodes_created==before.layout_nodes_created,
        "retired ancestor lost its escaped child or leaf conversion recreated layout nodes");
    require(actions_of(c.click(4)).size()==1 && coverage->pixels==pixels && held->find(2),
        "escaped button stopped working or held frame resources changed");
}

void control_topology_failure_and_scaling() {
    constexpr std::size_t siblings=512;
    Consumer c;
    ui::Control parent; parent.key=1; parent.layout.width=ui_layout::Axis::fixed(240);
    c.controls.push_back(parent);
    auto button=leaf(2,ui::ControlKind::button,0,L"Recovery button"); button.parent=1;
    c.controls.push_back(button);
    for (std::size_t i=0;i<siblings;++i) {
        ui::Control row; row.key=100+i; row.parent=1; row.layout=ui_layout::NodeInput::box(20,1);
        c.controls.push_back(std::move(row));
    }
    c.prepare(); const auto held=c.frame; const auto before=c.ui.statistics();
    std::reverse(c.controls.begin()+2,c.controls.end());
    ui::Control inserted; inserted.key=9000; inserted.parent=1; inserted.layout=ui_layout::NodeInput::box(-1,2);
    c.controls.insert(c.controls.begin()+2,inserted);
    accepted(c.ui.set_controls(c.controls));
    const auto rejected=c.ui.prepare();
    require(!rejected && rejected.error().code==ui::ErrorCode::layout_failed && rejected.error().key==9000,
        "new-node batch error lost the owning ControlKey");
    auto failed=c.ui.statistics();
    require(c.ui.presented()==held && failed.layout.topology_commits==before.layout.topology_commits &&
        failed.layout.topology_links_written==before.layout.topology_links_written &&
        failed.layout_nodes_created==before.layout_nodes_created,"failed structural sync partially committed layout");
    const auto hit=held->find(2)->bounds;
    require(!c.ui.input({.kind=ui::EventKind::pointer_down,.x=hit.x+hit.width/2,.y=hit.y+hit.height/2}).consumed,
        "failed structure preparation left accepted input enabled");
    c.controls[2].layout.width=ui_layout::Axis::fixed(20);
    accepted(c.ui.update_controls(std::span(&c.controls[2],1))); prepare_current(c,false);
    require(c.ui.presented()==held &&
        !c.ui.input({.kind=ui::EventKind::pointer_down,.x=hit.x+hit.width/2,.y=hit.y+hit.height/2}).consumed,
        "unaccepted structural recovery enabled input");
    const auto recovered=c.ui.statistics();
    require(recovered.layout_nodes_created==before.layout_nodes_created+1 &&
        recovered.layout.topology_commits==before.layout.topology_commits+1 &&
        recovered.layout.topology_links_written-before.layout.topology_links_written<=c.controls.size(),
        "large sibling recovery recreated existing nodes or exceeded a single final-tree write");
    require(c.frame->find(100)->incarnation==held->find(100)->incarnation &&
        c.frame->find(100+siblings-1)->bounds.y<c.frame->find(100)->bounds.y &&
        held->find(100)->bounds.y<held->find(100+siblings-1)->bounds.y,
        "large sibling reversal lost identities or changed the old frame");
    require(c.ui.accept(c.frame) && actions_of(c.click(2)).size()==1,"accepted structural recovery did not restore input");
    c.prepare();
    require(c.ui.statistics().layout.topology_commits==recovered.layout.topology_commits &&
        c.ui.statistics().layout.topology_links_written==recovered.layout.topology_links_written,
        "unchanged controls resubmitted their topology");
    std::printf("UI topology: siblings=%zu, recovery_links_written=%llu, created=1\n",siblings,
        static_cast<unsigned long long>(recovered.layout.topology_links_written-before.layout.topology_links_written));
}
