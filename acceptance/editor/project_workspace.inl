// Real Project -> Workspace -> shared UI layout/drawing/hit data, without a
// native window or desktop input. The separate HWND acceptance remains required.
void verify_project_workspace(editor::Project& project) {
    namespace browser=editor::browser_control;
    editor::Session document;editor::Workspace workspace;workspace.project_open=true;workspace.assets.reset(true);
    const std::array fonts{(std::filesystem::current_path()/"fonts/SarasaUiSC-Regular.ttf").wstring()};
    ui::Session surface(fonts);
    auto theme=ui::StyleSheet::parse(take(editor::read_file("editor.ss")));
    require(theme.has_value(),"Project UI theme failed to parse.");
    require(surface.set_style_sheet(*theme).has_value(),"Project UI theme failed to resolve.");
    ui::PixelExtent current_extent{};float current_scale{};
    const auto present=[&](ui::PixelExtent extent,float scale) {
        current_extent=extent;current_scale=scale;
        static std::uint64_t revision{};
        require(surface.set_surface({extent,scale,++revision}).has_value(),"Project UI rejected its surface.");
        const auto controls=workspace.describe(document,{extent.width/scale,extent.height/scale},surface.presented().get(),&project);
        auto described=surface.set_controls(controls);
        if (!described) throw std::runtime_error("Project UI invalid control "+std::to_string(described.error().key));
        auto frame=surface.prepare();
        if (!frame) throw std::runtime_error("Project UI layout failure "+std::to_string(frame.error().key)+" at "+std::to_string(extent.width)+"x"+std::to_string(extent.height)+" scale "+std::to_string(scale));
        require(surface.accept(*frame),"Project UI rejected the current prepared frame.");
    };
    const auto click=[&](ui::ControlKey key) {
        const auto visible=[&](const ui::ControlBox& box) {
            const auto y=box.bounds.y+box.bounds.height/2;
            return y>=box.clip.top && y<box.clip.bottom;
        };
        for (unsigned attempt=0;attempt<8;++attempt) {
            const auto* target=surface.presented()->find(key);require(target!=nullptr,"Project UI target is missing.");
            if (visible(*target)) break;
            auto parent=target->parent;
            while (parent) {
                const auto found=std::ranges::find_if(surface.presented()->scrollbars,[&](const auto& bar){return bar.key==parent;});
                if (found!=surface.presented()->scrollbars.end()) {
                    surface.input({.kind=ui::EventKind::wheel,.x=found->track.x+found->track.width/2,
                        .y=found->track.y+found->track.height/2,.wheel=target->bounds.y<target->clip.top?3.f:-3.f});
                    break;
                }
                const auto* ancestor=surface.presented()->find(parent);parent=ancestor?ancestor->parent:0;
            }
            present(current_extent,current_scale);
        }
        const auto* box=surface.presented()->find(key);require(box && visible(*box),"Project UI target is not reachable through scrolling.");
        const auto x=box->bounds.x+box->bounds.width/2,y=box->bounds.y+box->bounds.height/2;
        for (const auto kind:{ui::EventKind::pointer_down,ui::EventKind::pointer_up}) {
            auto reply=surface.input({.kind=kind,.x=x,.y=y,.button=ui::PointerButton::primary});
            for (const auto& effect:reply.effects) if (const auto* action=std::get_if<ui::Action>(&effect)) workspace.assets.apply(*action);
        }
    };
    for (const float scale:{1.f,1.5f,2.f}) for (const auto size:{ui::PixelExtent{1600,1000},ui::PixelExtent{1000,720}}) {
        present({static_cast<std::uint32_t>(size.width*scale),static_cast<std::uint32_t>(size.height*scale)},scale);click(browser::engine);present({static_cast<std::uint32_t>(size.width*scale),static_cast<std::uint32_t>(size.height*scale)},scale);
        const auto key=workspace.assets.control("builtin/sphere");click(key);present({static_cast<std::uint32_t>(size.width*scale),static_cast<std::uint32_t>(size.height*scale)},scale);
        require(workspace.assets.selected() && workspace.assets.selected()->key=="builtin/sphere","Asset hit input did not select the resource.");
        const auto* viewport=surface.presented()->find(editor::control::viewport);
        require(viewport!=nullptr,"Scene viewport is missing.");
        for (const auto control:{editor::control::scene_shading,editor::control::scene_edges,editor::control::scene_topology,editor::control::scene_guides}) {
            const auto* option=surface.presented()->find(control);
            require(option && option->kind==ui::ControlKind::combo && option->bounds.width>0 &&
                option->bounds.x>=option->clip.left && option->bounds.x+option->bounds.width<=option->clip.right+.1f &&
                option->bounds.y+option->bounds.height<=viewport->bounds.y+.1f,
                "Viewport options overflow or overlap scene input at this size/DPI.");
        }
        for (const auto view:{browser::tiles,browser::list,browser::details}) {
            click(view);present({static_cast<std::uint32_t>(size.width*scale),static_cast<std::uint32_t>(size.height*scale)},scale);
            require(surface.presented()->find(key)->selected,"View switch lost resource selection.");
        }
        click(browser::project);present({static_cast<std::uint32_t>(size.width*scale),static_cast<std::uint32_t>(size.height*scale)},scale);
        require(take(document.world().members()).empty() && !document.dirty(),"Browsing changed the scene document.");
    }
    const std::wstring unicode_name=L"Entry00000_材质\U0001F98A.txt";
    const auto unicode_resource="Source/"+editor::utf8(unicode_name);
    std::ofstream(project.root()/"Source"/unicode_name)<<"// Unicode catalog fixture\n";
    for (unsigned i=0;i<2600;++i) std::ofstream(project.root()/"Source"/("Entry"+std::to_string(10000+i)+".txt"))<<"// Catalog acceptance fixture\n";
    take(project.refresh());workspace.assets.navigate(L"Source");present({1600,1000},1);present({1600,1000},1);
    const auto unicode_key=workspace.assets.control(unicode_resource);
    require(unicode_key && surface.presented()->find(unicode_key),"Unicode resource key was not owned by the asset browser.");
    require(workspace.assets.control(std::string_view{"\xff",1})==0 && workspace.assets.control(std::string_view{"Source/\0missing",15})==0,
        "Arbitrary-byte missing key lookup changed its contract.");
    require(surface.presented()->controls.size()<500,"Asset browser instantiated the whole directory as controls.");
    const auto bar=std::ranges::max_element(surface.presented()->scrollbars,{},&ui::ScrollbarGeometry::maximum);
    require(bar!=surface.presented()->scrollbars.end() && bar->maximum>50000,"Large directory did not retain its complete scroll extent.");
    surface.input({.kind=ui::EventKind::wheel,.x=bar->track.x+bar->track.width/2,.y=bar->track.y+bar->track.height/2,.wheel=-10000});
    present({1600,1000},1);present({1600,1000},1);
    const auto last=workspace.assets.control("Source/Entry12599.txt");
    require(last && surface.presented()->find(last) && surface.presented()->controls.size()<500,"Virtual asset browsing could not reach the last real file.");
    take(project.refresh());workspace.assets.navigate(L"Source");present({1600,1000},1);present({1600,1000},1);
    require(workspace.assets.control(unicode_resource)==unicode_key && surface.presented()->find(unicode_key),
        "Refresh or temporary invisibility changed a resource's ControlKey.");
    workspace.assets.reset(true);workspace.assets.navigate(L"Source");present({1600,1000},1);present({1600,1000},1);
    require(workspace.assets.control(unicode_resource)>unicode_key,"Reset reused a previous resource identity.");
    std::puts("Project workspace acceptance: PASS (real folder data, owned UTF-8 keys, refresh/reset identity, three views, hit selection, 1/1.5/2 DPI)");
}
