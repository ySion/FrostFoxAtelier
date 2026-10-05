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
    ui::PixelExtent current_extent{};float current_scale{};std::uint64_t surface_revision{};
    const auto present=[&](ui::PixelExtent extent,float scale) {
        if (current_extent!=extent || current_scale!=scale) ++surface_revision;
        current_extent=extent;current_scale=scale;
        require(surface.set_surface({extent,scale,surface_revision}).has_value(),"Project UI rejected its surface.");
        const auto controls=workspace.describe(document,{extent.width/scale,extent.height/scale},surface.presented().get(),&project);
        auto described=surface.set_controls(controls);
        if (!described) throw std::runtime_error("Project UI invalid control "+std::to_string(described.error().key));
        workspace.commit_ui_controls();
        auto frame=surface.prepare();
        if (!frame) throw std::runtime_error("Project UI layout failure "+std::to_string(frame.error().key)+" at "+std::to_string(extent.width)+"x"+std::to_string(extent.height)+" scale "+std::to_string(scale));
        require(surface.accept(*frame),"Project UI rejected the current prepared frame.");
    };
    const auto input=[&](const ui::Event& event) {
        auto reply=surface.input(event);require(!reply.error,"Project UI input failed.");
        for (const auto& effect:reply.effects) if (const auto* action=std::get_if<ui::Action>(&effect)) workspace.assets.apply(*action);
    };
    const auto pointer_click=[&](float x,float y) {
        for (const auto kind:{ui::EventKind::pointer_down,ui::EventKind::pointer_up})
            input({.kind=kind,.x=x,.y=y,.button=ui::PointerButton::primary});
    };
    const auto resource_visible=[&](ui::ControlKey key) {
        if (const auto* control=surface.presented()->find(key)) return control->bounds.y<control->clip.bottom && control->bounds.y+control->bounds.height>control->clip.top;
        const auto* table=surface.presented()->find_table(browser::results);
        return table && std::ranges::any_of(table->cells,[&](const auto& cell){return cell.row==key && cell.bounds.y>=cell.clip.top && cell.bounds.y+cell.bounds.height<=cell.clip.bottom;});
    };
    const auto resource_selected=[&](ui::ControlKey key) {
        if (const auto* control=surface.presented()->find(key)) return control->selected;
        const auto* table=surface.presented()->find_table(browser::results);return table && table->selection.row==key;
    };
    const auto press=[&](ui::Key key) {
        input({.kind=ui::EventKind::key_down,.key=key});input({.kind=ui::EventKind::key_up,.key=key});
        present(current_extent,current_scale);
    };
    const auto click=[&](ui::ControlKey key) {
        if (!surface.presented()->find(key)) {
            const auto* table=surface.presented()->find_table(browser::results);require(table,"Project resource has no accepted table.");
            const auto cell=std::ranges::find(table->cells,key,&ui::TableCellGeometry::row);
            require(cell!=table->cells.end(),"Project resource has no accepted visible cell.");
            const auto left=std::max(cell->bounds.x,static_cast<float>(cell->clip.left)),right=std::min(cell->bounds.x+cell->bounds.width,static_cast<float>(cell->clip.right));
            const auto top=std::max(cell->bounds.y,static_cast<float>(cell->clip.top)),bottom=std::min(cell->bounds.y+cell->bounds.height,static_cast<float>(cell->clip.bottom));
            require(right>left && bottom>top,"Project resource table cell is clipped.");
            pointer_click((left+right)/2,(top+bottom)/2);return;
        }
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
        pointer_click(x,y);
    };
    for (const float scale:{1.f,1.5f,2.f}) for (const auto size:{ui::PixelExtent{1600,1000},ui::PixelExtent{1000,720}}) {
        present({static_cast<std::uint32_t>(size.width*scale),static_cast<std::uint32_t>(size.height*scale)},scale);click(browser::engine);present({static_cast<std::uint32_t>(size.width*scale),static_cast<std::uint32_t>(size.height*scale)},scale);
        const auto key=workspace.assets.control("builtin/sphere");click(key);present({static_cast<std::uint32_t>(size.width*scale),static_cast<std::uint32_t>(size.height*scale)},scale);
        require(workspace.assets.selected() && workspace.assets.selected()->key=="builtin/sphere","Asset hit input did not select the resource.");
        const auto* viewport=surface.presented()->find(editor::control::viewport);
        require(viewport!=nullptr,"Scene viewport is missing.");
        for (const auto control:{editor::control::scene_shading,editor::control::scene_guides}) {
            const auto* option=surface.presented()->find(control);
            require(option && option->kind==ui::ControlKind::combo && option->bounds.width>0 &&
                option->bounds.x>=option->clip.left && option->bounds.x+option->bounds.width<=option->clip.right+.1f &&
                option->bounds.y+option->bounds.height<=viewport->bounds.y+.1f,
                "Viewport options overflow or overlap scene input at this size/DPI.");
        }
        require(surface.presented()->find(browser::project)->kind==ui::ControlKind::tree_item &&
            surface.presented()->find(browser::engine)->parent==browser::directories &&
            surface.presented()->find(browser::project)->parent==browser::directories,
            "The directory tree disappeared when browsing Engine resources.");
        for (const auto view:{browser::tiles,browser::list,browser::details}) {
            const auto menu=workspace.assets.menu_items(browser::view_menu);
            const auto option=std::ranges::find(menu,view,&ui::MenuItem::key);
            require(option!=menu.end() && option->enabled && option->behavior==ui::MenuBehavior::radio && !surface.presented()->find(view),
                "Resource view choices did not share one radio menu.");
            workspace.assets.apply({ui::ActionKind::activate,view,{}});
            present({static_cast<std::uint32_t>(size.width*scale),static_cast<std::uint32_t>(size.height*scale)},scale);
            require(resource_selected(key),"View switch lost resource selection.");
        }
        click(browser::project);present({static_cast<std::uint32_t>(size.width*scale),static_cast<std::uint32_t>(size.height*scale)},scale);
        require(take(document.world().members()).empty() && !document.dirty(),"Browsing changed the scene document.");
    }
    workspace.dock.maximize(editor::control::assets_tab);
    workspace.assets.apply({ui::ActionKind::activate,browser::engine,{}});
    for (const auto width:{240u,420u,560u}) {
        present({width,720},1);present({width,720},1);
        const auto* path=surface.presented()->find(browser::path);
        const auto* search=surface.presented()->find(browser::search);
        require(path && search && std::abs(path->bounds.y-search->bounds.y)<.1f,
            "Narrow asset navigation split its path and search onto different rows.");
        for (const auto key:{browser::back,browser::up,browser::path,browser::search,browser::filter,browser::view_menu,browser::actions}) {
            const auto* tool=surface.presented()->find(key);
            require(tool && tool->bounds.width>0 && tool->bounds.x>=tool->clip.left &&
                tool->bounds.x+tool->bounds.width<=tool->clip.right+.1f,"The compact asset toolbar overflowed its narrow pane.");
        }
        const auto* tree=surface.presented()->find(browser::directories);
        const auto* table=surface.presented()->find_table(browser::results);
        require(tree && tree->bounds.width>0 && table && table->data->columns.size()==3 && resource_visible(workspace.assets.control("builtin/sphere")),
            "A narrow pane lost its directory tree or one of the actual table columns.");
        float content_width{};for (const auto& column:table->data->columns) content_width+=column.width;
        if (content_width>table->body.width+.1f)
            require(std::ranges::any_of(surface.presented()->scrollbars,[](const auto& bar){return bar.key==browser::results && bar.horizontal && bar.maximum>0;}),
                "Narrow Details hid columns instead of retaining a horizontal scroll range.");
        for (const auto& cell:table->cells) {
            const auto column=std::ranges::find(table->columns,cell.column,&ui::TableColumnGeometry::column);
            require(column!=table->columns.end() && std::abs(column->bounds.x-cell.bounds.x)<.1f && std::abs(column->bounds.width-cell.bounds.width)<.1f,
                "Table header and body did not share the accepted column geometry.");
        }
    }
    const auto sorting_folder=project.root()/"Source/SortOrder";
    std::filesystem::create_directories(sorting_folder/"Z-folder");
    std::ofstream(sorting_folder/"A-large.txt")<<std::string(1000,'x');
    std::ofstream(sorting_folder/"B-small.txt")<<std::string(20,'x');
    std::ofstream(sorting_folder/"C-small.txt")<<std::string(20,'y');
    std::ofstream(sorting_folder/"D.scene")<<"{}";
    take(project.refresh());workspace.assets.navigate(L"Source/SortOrder");present({1000,720},1);present({1000,720},1);
    const auto large=workspace.assets.control("Source/SortOrder/A-large.txt");click(large);present({1000,720},1);
    const auto original_data=surface.presented()->find_table(browser::results)->data;
    press(ui::Key::right);
    require(surface.presented()->find_table(browser::results)->selection==ui::TableSelection{large,browser::type_column} &&
        surface.presented()->find_table(browser::results)->data==original_data,"Moving the active cell copied rows or changed the selected resource.");
    const auto header=[&](ui::TableKey key) {
        const auto* table=surface.presented()->find_table(browser::results);require(table,"Details table is missing.");
        const auto column=std::ranges::find(table->columns,key,&ui::TableColumnGeometry::column);
        require(column!=table->columns.end(),"Details header is missing.");
        pointer_click(column->bounds.x+column->bounds.width/2,column->bounds.y+column->bounds.height/2);present(current_extent,current_scale);
    };
    const auto order=[&](std::initializer_list<const char*> paths) {
        const auto* table=surface.presented()->find_table(browser::results);require(table && table->data->rows.size()==paths.size(),"Unexpected sort fixture rows.");
        std::size_t index{};for (const auto* path:paths)
            require(table->data->rows[index++].key==workspace.assets.control(std::string("Source/SortOrder/")+path),"Table sorted display text or changed its folder/key ordering.");
        require(resource_selected(large),"Table sorting changed the selected resource.");
    };
    header(browser::name_column);order({"Z-folder","D.scene","C-small.txt","B-small.txt","A-large.txt"});
    header(browser::size_column);order({"Z-folder","D.scene","B-small.txt","C-small.txt","A-large.txt"});
    header(browser::size_column);order({"Z-folder","A-large.txt","B-small.txt","C-small.txt","D.scene"});
    header(browser::type_column);order({"Z-folder","D.scene","A-large.txt","B-small.txt","C-small.txt"});
    header(browser::type_column);order({"Z-folder","A-large.txt","B-small.txt","C-small.txt","D.scene"});
    const auto resize=surface.presented()->find_table(browser::results)->columns.front().resize;
    const auto resize_x=resize.x+resize.width/2,resize_y=resize.y+resize.height/2;
    input({.kind=ui::EventKind::pointer_down,.x=resize_x,.y=resize_y,.button=ui::PointerButton::primary});
    input({.kind=ui::EventKind::pointer_move,.x=resize_x-500,.y=resize_y,.button=ui::PointerButton::primary});
    input({.kind=ui::EventKind::pointer_up,.x=resize_x-500,.y=resize_y,.button=ui::PointerButton::primary});present(current_extent,current_scale);
    const auto retained=surface.presented()->find_table(browser::results)->data;
    require(retained->columns.front().width==retained->columns.front().min_width,"A real column resize did not enforce the persistent minimum.");
    for (const auto view:{browser::list,browser::tiles,browser::details}) {
        workspace.assets.apply({ui::ActionKind::activate,view,{}});present(current_extent,current_scale);
        require(resource_selected(large),"View changes lost the sorted selection.");
    }
    require(surface.presented()->find_table(browser::results)->data==retained,"View changes rebuilt or forgot table sort and widths.");
    header(browser::name_column);
    workspace.dock.maximize(editor::control::assets_tab);workspace.assets.apply({ui::ActionKind::activate,browser::project,{}});
    const std::wstring unicode_name=L"Entry00000_材质\U0001F98A.txt";
    const auto unicode_resource="Source/"+editor::utf8(unicode_name);
    std::ofstream(project.root()/"Source"/unicode_name)<<"// Unicode catalog fixture\n";
    for (unsigned i=0;i<2600;++i) std::ofstream(project.root()/"Source"/("Entry"+std::to_string(10000+i)+".txt"))<<"// Catalog acceptance fixture\n";
    take(project.refresh());workspace.assets.navigate(L"Content");workspace.assets.navigate(L"Source");present({1600,1000},1);present({1600,1000},1);
    const auto unicode_key=workspace.assets.control(unicode_resource);
    require(unicode_key && resource_visible(unicode_key),"Unicode resource key was not owned by the asset browser.");
    require(workspace.assets.control(std::string_view{"\xff",1})==0 && workspace.assets.control(std::string_view{"Source/\0missing",15})==0,
        "Arbitrary-byte missing key lookup changed its contract.");
    require(surface.presented()->controls.size()<500,"Asset browser instantiated the whole directory as controls.");
    click(unicode_key);present({1600,1000},1);
    const auto directory_data=surface.presented()->find_table(browser::results)->data;
    press(ui::Key::page_down);
    require(surface.presented()->find_table(browser::results)->selection.row!=unicode_key &&
        resource_visible(surface.presented()->find_table(browser::results)->selection.row),"Table PageDown did not select and reveal its current row.");
    press(ui::Key::page_up);press(ui::Key::home);
    require(surface.presented()->find_table(browser::results)->selection.row==directory_data->rows.front().key,"Table Home did not reach its first row.");
    press(ui::Key::end);
    const auto* end_table=surface.presented()->find_table(browser::results);
    require(end_table->selection.row==directory_data->rows.back().key,"Table End did not select its last row.");
    require(end_table->data==directory_data,"Table End copied the directory snapshot.");
    require(resource_visible(directory_data->rows.back().key),"Table End did not reveal its last row.");
    const auto bar=std::ranges::max_element(surface.presented()->scrollbars,{},&ui::ScrollbarGeometry::maximum);
    require(bar!=surface.presented()->scrollbars.end() && bar->maximum>50000,"Large directory did not retain its complete scroll extent.");
    surface.input({.kind=ui::EventKind::wheel,.x=bar->track.x+bar->track.width/2,.y=bar->track.y+bar->track.height/2,.wheel=-10000});
    present({1600,1000},1);present({1600,1000},1);
    const auto last=workspace.assets.control("Source/Entry12599.txt");
    require(last && resource_visible(last) && surface.presented()->controls.size()<500 && surface.presented()->find_table(browser::results)->cells.size()<200,
        "Virtual asset browsing could not reach the last real file with bounded accepted cells.");
    take(project.refresh());workspace.assets.navigate(L"Content");workspace.assets.navigate(L"Source");present({1600,1000},1);present({1600,1000},1);
    const auto& refreshed_rows=surface.presented()->find_table(browser::results)->data->rows;
    require(workspace.assets.control(unicode_resource)==unicode_key && std::ranges::find(refreshed_rows,unicode_key,&ui::TableRow::key)!=refreshed_rows.end(),
        "Refresh or temporary invisibility changed a resource's ControlKey.");
    workspace.assets.reset(true);workspace.assets.navigate(L"Source");present({1600,1000},1);present({1600,1000},1);
    require(workspace.assets.control(unicode_resource)>unicode_key,"Reset reused a previous resource identity.");
    std::puts("Project workspace acceptance: PASS (real Table headers, numeric/folder/key sort, column minima, current cell, virtual navigation, view retention, UTF-8 identity, 1/1.5/2 DPI)");
}
