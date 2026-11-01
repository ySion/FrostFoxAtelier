void verify_project_window(Application& app, const std::filesystem::path& output, bool existing = false) {
    namespace access = atelier::application;
    namespace browser = atelier::editor::browser_control;
    const auto has_resource = [&](ControlKey key) { return app.presented()->find(key) || row_table(app, key); };
    const auto selected_resource = [&](ControlKey key) {
        if (const auto* control = app.presented()->find(key))
            return control->selected;
        const auto* table = row_table(app, key);
        return table && table->selection.row == key;
    };
    const auto folder = std::filesystem::absolute(output / L"项目");
    const auto moved = std::filesystem::absolute(output / L"工程搬移");
    // --acceptance reuses its output folder; fixtures left by an earlier run must not decide this one.
    if (!existing)
        std::filesystem::remove_all(folder);
    std::filesystem::remove_all(moved);
    access::EditorAcceptanceAccess::mark_saved(app);
    model(Access::invoke(app, existing ? "project.open" : "project.new",
                         {{"path", atelier::assets::utf8(folder.wstring())}}));
    tick(app, 4);
    const auto initial_count = entities(app).size();
    require((existing || initial_count == 0) && !app.document().dirty(),
            "Project did not publish its clean initial scene.");
    click(app, operator_item(app, "scene.add", {{"kind", "mesh"}}));
    const auto entity = app.document().selection().entity;
    const auto field = property_control(app, entity, 0);
    click(app, field);
    type(app, L"7");
    key(app, 'S', true);
    tick(app, 2);
    require(!app.document().dirty() && x(app, entity) == 7,
            "Save did not commit the active field and mark the scene clean.");
    auto saved = app.document().serialize();
    require(saved.has_value(), "Cannot inspect the saved scene.");
    model(app.open_project(folder));
    tick(app, 4);
    require(entities(app).size() == initial_count + 1 && !app.document().dirty() &&
                model(app.document().serialize()) == *saved,
            "Opening the saved project lost scene data.");
    const auto current = model(app.document().world().members())[initial_count].id();

    // Exercise inline material history before unrelated directory navigation.
    // The selected scene has an independent, undoable edit.
    {
        using Access = access::EditorAcceptanceAccess;
        auto project = model(atelier::editor::Project::open(folder));
        atelier::assets::MaterialAsset author;
        std::get<atelier::assets::MaterialInstanceAsset>(author.author).entries = {{"roughness", .25f, true}};
        const auto asset = model(project.create_material("Content/Materials/HistoryOwner.material", author));
        model(Access::invoke(app, "asset.refresh"));
        auto& document = Access::physics_document(app);
        const auto mesh = component(app, current, document.presentation_types().static_mesh_component);
        model(document.assign_material(project, mesh, "slot/0", asset));
        model(document.rename(current, L"Scene history sentinel"));
        model(app.save_scene());
        tick(app, 2);
        const auto material = document.loaded_material(asset);
        const auto scene = model(document.serialize());
        const auto history = document.undo_count();
        require(history > 0, "Inline MI history fixture has no independent scene edit");
        const auto accepted_material = [&] {
            auto* run = atelier::logging::current_run();
            require(run, "Material acceptance needs the process log.");
            const auto flushed = run->flush();
            require(flushed.backend_acknowledged && flushed.sink_flush_succeeded, "Cannot flush material evidence.");
            auto reader = atelier::logging::Reader::open(run->info().directory);
            require(reader.has_value(), "Cannot read material evidence.");
            std::map<std::uint64_t, std::array<unsigned, 3>> transitions;
            const auto deadline = GetTickCount64() + 5000;
            for (;;) {
                auto batch = (*reader)->poll();
                for (const auto& record : batch.records) {
                    const auto& entry = record.entry;
                    if (entry.category != "material")
                        continue;
                    const auto number = [&](std::string_view key) {
                        for (const auto& field : entry.fields)
                            if (field.key == key)
                                if (const auto* value = std::get_if<std::uint64_t>(&field.value))
                                    return *value;
                        throw std::runtime_error("Material event omitted its correlation field.");
                    };
                    if (number("document") != document.presentation().identity())
                        continue;
                    const auto index = entry.event_code == "material.requested"       ? 0
                                       : entry.event_code == "material.adopted"       ? 1
                                       : entry.event_code == "material.view_accepted" ? 2
                                                                                      : -1;
                    if (index < 0)
                        continue;
                    const auto generation = number("request_generation");
                    if (!generation)
                        continue;
                    ++transitions[generation][static_cast<std::size_t>(index)];
                    if (index == 2)
                        require(number("frame") > 0 && number("frame") <= app.presented()->drawing->id,
                                "Material evidence names an unaccepted frame.");
                }
                require(!batch.status.incomplete, "Material evidence is incomplete.");
                if (batch.status.caught_up)
                    break;
                require(GetTickCount64() < deadline, "Material Reader did not catch up.");
            }
            require(!transitions.empty() && transitions.rbegin()->second == std::array<unsigned, 3>{1, 1, 1},
                    "Latest material request did not reach exactly one adopted/accepted-frame transition.");
            return transitions.rbegin()->first;
        };
        const auto first_material = accepted_material();
        const auto roughness = [&](atelier::ObjectId object) {
            const auto view = model(document.inspect_material(object));
            const auto at = std::ranges::find(view.entries, "roughness", &atelier::material::Override::path);
            require(at != view.entries.end(), "Inline MI lost its authored roughness");
            return std::get<float>(at->value);
        };
        click(app, app.object_control(current));
        click(app, operator_item(app, "layout.show", {{"tab", "structure"}}));
        click(app, Access::structure_control(app, mesh));
        click(app, operator_item(app, "layout.show", {{"tab", "parameters"}}));
        const auto field = Access::material_control(app, material, "roughness");
        require(field && roughness(material) == .25f, "Component Parameters did not expose the shared MI field");
        click(app, field);
        type(app, L"0.75");
        key(app, VK_RETURN);
        require(roughness(material) == .75f && document.material_dirty(asset) && document.undo_count() == history &&
                    model(document.serialize()) == scene,
                "Inline shared MI edit changed scene history or failed to record the asset draft");
        tick(app, 3);
        require(accepted_material() > first_material,
                "Replacing a non-fallback material omitted its new accepted-frame identity.");
        key(app, 'Z', true);
        std::printf("Inline MI Undo: roughness=%g, scene history=%zu/%zu\n", roughness(material), document.undo_count(),
                    history);
        std::fflush(stdout);
        require(roughness(material) == .25f && document.undo_count() == history && model(document.serialize()) == scene,
                "Inline shared MI Undo reached scene history instead of the edited material");
        click(app, control::edit_menu);
        const auto* undo = app.menu_frame() ? app.menu_frame()->find(Access::operator_item(app, "edit.undo")) : nullptr;
        require(undo && !undo->enabled, "Empty inline MI history left the scene Undo menu enabled");
        key(app, VK_ESCAPE);
        key(app, 'Z', true);
        require(roughness(material) == .25f && document.undo_count() == history && model(document.serialize()) == scene,
                "Empty inline MI history fell through to the scene");
        key(app, 'Y', true);
        require(roughness(material) == .75f && document.undo_count() == history && model(document.serialize()) == scene,
                "Inline shared MI Redo did not stay in material history");
        key(app, 'S', true);
        require(!document.material_dirty(asset) && document.undo_count() == history &&
                    model(document.serialize()) == scene,
                "Inline shared MI Ctrl+S saved the scene instead of the material");
        auto reopened = model(atelier::editor::Project::open(folder));
        const auto disk = model(reopened.assets().material(asset));
        const auto& entries = std::get<atelier::assets::MaterialInstanceAsset>(disk.author).entries;
        const auto at = std::ranges::find(entries, "roughness", &atelier::material::Override::path);
        require(at != entries.end() && std::get<float>(at->value) == .75f &&
                    model(atelier::assets::read_file(folder / "Content/Scenes/Main.scene")) == scene,
                "Inline MI save did not persist its value independently of the unchanged scene");
        std::puts("Inline shared MI Undo/Redo/Save ownership: PASS");
        std::fflush(stdout);

        click(app, field);
        type(app, L"0.5");
        key(app, VK_RETURN);
        click(app, operator_item(app, "layout.show", {{"tab", "scene_tree"}}));
        click(app, app.object_control(current));
        key(app, 'Z', true);
        require(document.undo_count() == history - 1 && document.name(current) != L"Scene history sentinel" &&
                    roughness(material) == .5f,
                "Scene Tree focus retained the inline material's Undo target");
        key(app, 'Y', true);
        require(document.undo_count() == history && model(document.serialize()) == scene && roughness(material) == .5f,
                "Scene Tree Redo did not restore its independent scene edit");
        click(app, property_control(app, current, 0));
        type(app, L"9");
        key(app, VK_RETURN);
        key(app, 'Z', true);
        require(x(app, current) == 7 && roughness(material) == .5f && document.undo_count() == history,
                "A scene field in Parameters retained the previous material history target");
        key(app, 'Y', true);
        key(app, 'S', true);
        const auto scene_position = model(document.serialize());
        require(x(app, current) == 9 && !document.dirty() && document.material_dirty(asset) &&
                    model(atelier::assets::read_file(folder / "Content/Scenes/Main.scene")) == scene_position,
                "Scene-field Ctrl+S saved the old material target instead of the scene");
        click(app, operator_item(app, "layout.show", {{"tab", "structure"}}));
        click(app, Access::structure_control(app, mesh));
        click(app, operator_item(app, "layout.show", {{"tab", "parameters"}}));
        click(app, Access::material_control(app, material, "roughness"));
        key(app, VK_RETURN);
        key(app, 'Z', true);
        require(roughness(material) == .75f && !document.material_dirty(asset) &&
                    model(document.serialize()) == scene_position,
                "Returning to the inline material failed to restore its own saved author state");
        std::puts("Inline MI selection/focus history isolation: PASS");
        std::fflush(stdout);

        const auto local = model(document.create_scene_material(mesh, "slot/0"));
        model(document.set_material_override(local.id(), "roughness", true, &project));
        model(app.save_scene());
        tick(app, 2);
        const auto local_history = document.undo_count();
        click(app, Access::material_control(app, local.id(), "roughness"));
        type(app, L"0.125");
        key(app, VK_RETURN);
        require(roughness(local.id()) == .125f && document.undo_count() == local_history + 1 &&
                    !document.material_dirty(asset),
                "Scene MI edit did not enter scene history independently of its named parent");
        key(app, 'Z', true);
        require(roughness(local.id()) == .75f && document.undo_count() == local_history && roughness(material) == .75f,
                "Scene MI Undo used the named parent's asset history");
        key(app, 'Y', true);
        key(app, 'S', true);
        require(roughness(local.id()) == .125f && roughness(material) == .75f && !document.dirty() &&
                    !document.material_dirty(asset) &&
                    model(atelier::assets::read_file(folder / "Content/Scenes/Main.scene")) ==
                        model(document.serialize()),
                "Scene MI Redo/Save did not stay in the scene document");
        std::puts("Scene MI Undo/Redo/Save ownership: PASS");
        std::fflush(stdout);
    }
    click(app, operator_item(app, "layout.show", {{"tab", "scene_tree"}}));
    click(app, app.object_control(current));
    // Restore the position framed by open_project before the remaining capture cases.
    click(app, property_control(app, current, 0));
    type(app, L"7");
    key(app, VK_RETURN);
    key(app, 'S', true);
    require(x(app, current) == 7 && !app.document().dirty(),
            "Material history fixture did not restore the framed scene position.");
    verify_about(app, folder.wstring());
    require(app.presented()->find(browser::project)->kind == atelier::ui::ControlKind::tree_item &&
                app.presented()->find(browser::engine)->parent == browser::directories &&
                app.presented()->find(browser::project)->parent == browser::directories &&
                app.presented()->find(browser::path)->kind == atelier::ui::ControlKind::combo,
            "Asset sources and interactive path did not use the common directory tree.");
    const auto action_enabled = [&](ControlKey command) {
        click(app, browser::actions);
        const auto* item = app.menu_frame() ? app.menu_frame()->find(command) : nullptr;
        require(item, "The resource actions menu omitted a real command.");
        const bool enabled = item->enabled;
        key(app, VK_ESCAPE);
        return enabled;
    };
    click(app, browser::project);
    key(app, VK_END);
    require(app.presented()->find(browser::engine)->selected, "Directory tree End did not select Engine.");
    key(app, VK_HOME);
    require(app.presented()->find(browser::project)->selected, "Directory tree Home did not select Project.");
    key(app, VK_RIGHT);
    require(!app.presented()->find(browser::project)->selected,
            "Directory tree Right did not enter a Project directory.");
    key(app, VK_LEFT);
    require(app.presented()->find(browser::project)->selected, "Directory tree Left did not return to Project.");
    key(app, VK_END);
    const auto sphere = access::EditorAcceptanceAccess::asset_key(app, "builtin/sphere");
    click(app, sphere);
    require(app.document().selection().entity == current && entities(app).size() == initial_count + 1,
            "Resource selection changed the scene selection or created an Entity.");
    for (const auto view : {browser::tiles, browser::list, browser::details}) {
        click(app, view);
        require(selected_resource(sphere), "Switching asset views lost resource selection.");
        require(!app.presented()->find(view), "A view choice remained as a duplicate toolbar button.");
        click(app, browser::view_menu);
        require(app.menu_frame() && app.menu_frame()->find(view)->selected,
                "The view menu did not expose its selected mode.");
        key(app, VK_ESCAPE);
    }
    click(app, browser::view_menu);
    key(app, VK_HOME);
    key(app, VK_DOWN);
    key(app, VK_RETURN);
    require(selected_resource(sphere), "Keyboard view-menu selection lost the resource selection.");
    click(app, sphere);
    const auto table_data = app.presented()->find_table(browser::results)->data;
    const auto resource_history = app.document().undo_count();
    key(app, VK_RIGHT);
    require(app.presented()->find_table(browser::results)->selection ==
                    atelier::ui::TableSelection{sphere, browser::type_column} &&
                app.presented()->find_table(browser::results)->data == table_data,
            "Moving the current resource cell rebuilt its rows or changed selection.");
    key(app, VK_RETURN);
    double_click(app, sphere);
    require(entities(app).size() == initial_count + 1 && app.document().selection().entity == current &&
                app.document().undo_count() == resource_history,
            "Enter or double-click on a mesh created an Entity or changed the scene.");
    click_at(app, table_header_centre(app, browser::results, browser::name_column));
    require(app.presented()->find_table(browser::results)->data->sort ==
                    atelier::ui::TableSort{browser::name_column, atelier::ui::TableSortDirection::descending} &&
                selected_resource(sphere),
            "A native header click did not change ordering while preserving resource selection.");
    click_at(app, table_header_centre(app, browser::results, browser::name_column));
    click(app, browser::search);
    type(app, L"Sphere");
    key(app, VK_RETURN);
    require(selected_resource(sphere) && action_enabled(browser::add),
            "A matching search cleared the visible resource selection.");
    click(app, browser::search);
    type(app, L"no such asset");
    key(app, VK_RETURN);
    require(!has_resource(sphere) && !action_enabled(browser::add),
            "Asset search retained an executable hidden resource selection.");
    click(app, browser::add);
    key(app, VK_ESCAPE);
    require(entities(app).size() == initial_count + 1, "A hidden search result could still be added to the scene.");
    click(app, browser::clear);
    require(has_resource(sphere) && !selected_resource(sphere) && !action_enabled(browser::add),
            "Clearing asset search failed to restore the catalog without its retired selection.");
    click(app, sphere);
    click(app, browser::filter);
    key(app, VK_HOME);
    key(app, VK_DOWN);
    key(app, VK_DOWN);
    key(app, VK_RETURN);
    require(!has_resource(sphere) && !action_enabled(browser::add),
            "Scene type filter retained an executable hidden mesh selection.");
    click(app, browser::clear);
    require(!selected_resource(sphere), "Clearing the type filter restored a hidden selection.");
    capture(app, output / "project-assets.png");
    const auto selection_folder =
        std::filesystem::path{"Content"} / ("AcceptanceSelection-" + std::to_string(GetTickCount64()));
    std::filesystem::create_directories(folder / selection_folder);
    model(Access::invoke(app, "asset.refresh"));
    tick(app, 2);
    const auto content_tree = access::EditorAcceptanceAccess::directory_key(app, "Content");
    const auto selected_tree = access::EditorAcceptanceAccess::directory_key(app, selection_folder);
    click(app, content_tree);
    const auto directory =
        access::EditorAcceptanceAccess::asset_key(app, atelier::assets::utf8(selection_folder.generic_wstring()));
    click(app, directory);
    require(action_enabled(browser::open), "Selected project folder could not be opened.");
    click(app, browser::search);
    type(app, L"no such asset");
    key(app, VK_RETURN);
    require(!has_resource(directory) && !action_enabled(browser::open),
            "Asset search retained an executable hidden folder selection.");
    click(app, browser::open);
    key(app, VK_ESCAPE);
    click(app, browser::clear);
    require(has_resource(directory) && !selected_resource(directory),
            "Hidden folder Open changed the directory or clearing search restored its selection.");
    click(app, content_tree);
    key(app, VK_LEFT);
    click(app, directory);
    key(app, VK_RETURN);
    const auto* revealed = app.presented()->find(selected_tree);
    require(revealed && revealed->selected && revealed->bounds.y >= revealed->clip.top &&
                revealed->bounds.y + revealed->bounds.height <= revealed->clip.bottom,
            "Opening a folder did not expand and reveal its selected directory-tree row.");
    click(app, browser::up);
    require(app.presented()->find(content_tree)->selected, "Up did not select the parent directory.");
    click(app, browser::back);
    require(app.presented()->find(selected_tree)->selected, "Back did not restore the previous directory.");
    click(app, browser::path);
    key(app, VK_HOME);
    key(app, VK_DOWN);
    key(app, VK_RETURN);
    require(app.presented()->find(content_tree)->selected,
            "The interactive path menu did not navigate to its Content ancestor.");
    double_click(app, directory);
    require(app.presented()->find(selected_tree)->selected, "Double-click did not use the existing folder Open route.");
    click(app, browser::up);
    click(app, directory);
    const auto context_point = centre(app, directory);
    SendMessageW(app.window(), WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(context_point.x, context_point.y));
    SendMessageW(app.window(), WM_RBUTTONUP, 0, MAKELPARAM(context_point.x, context_point.y));
    tick(app, 2);
    require(app.menu_frame() && app.menu_frame()->find(browser::open)->enabled,
            "A real table-row context menu lost its folder target.");
    require(std::filesystem::remove(folder / selection_folder), "Cannot remove the context-menu directory fixture.");
    model(Access::invoke(app, "asset.refresh"));
    tick(app, 2);
    require(!app.menu_frame() && !has_resource(directory) && !action_enabled(browser::open) &&
                entities(app).size() == initial_count + 1 && app.document().selection().entity == current,
            "A refreshed table retained a stale row menu or changed the scene.");
    std::filesystem::create_directories(folder / selection_folder);
    model(Access::invoke(app, "asset.refresh"));
    tick(app, 2);
    click(app, directory);
    click(app, browser::open);
    require(app.presented()->find(selected_tree) && app.presented()->find(selected_tree)->selected,
            "Open did not enter the recreated directory.");
    require(std::filesystem::remove(folder / selection_folder),
            "Cannot remove the empty directory acceptance fixture.");
    model(Access::invoke(app, "asset.refresh"));
    tick(app, 2);
    require(app.presented()->find(browser::project)->selected,
            "Refreshing a removed directory did not return to Project.");
    require(!app.presented()->find(selected_tree), "Refreshing retained a removed directory tree row.");
    require(!action_enabled(browser::open), "Refreshing retained an executable removed directory.");
    const auto external = std::filesystem::absolute(output / "Imported.obj");
    model(atelier::editor::write_file(external, "v 0 0 0\nv 2 0 0\nv 0 2 0\nf 1 2 3\n"));
    model(Access::invoke(app, "asset.import", {{"kind", "mesh"}, {"path", atelier::assets::utf8(external.wstring())}}));
    tick(app, 3);
    require(app.presented()->find(access::EditorAcceptanceAccess::directory_key(app, "Content/Meshes"))->selected,
            "Import did not reveal the project's mesh folder.");
    capture(app, output / "project-directory.png");
    const auto catalog = model(atelier::editor::Project::open(folder));
    const auto imported_entry = std::find_if(catalog.entries().begin(), catalog.entries().end(), [](const auto& entry) {
        return entry.path == L"Content/Meshes/Imported.obj" && entry.kind == atelier::assets::AssetKind::mesh;
    });
    require(imported_entry != catalog.entries().end(), "Imported OBJ did not publish a project mesh entry.");
    click(app, access::EditorAcceptanceAccess::asset_key(app, imported_entry->key));
    click(app, browser::add);
    const auto imported_entity = app.document().selection().entity;
    require(entities(app).size() == initial_count + 2 && imported_entity != current,
            "Adding the imported resource did not create a distinct Entity.");
    click(app, app.object_control(current));
    click(app, app.object_control(imported_entity));
    require(app.document().selection().entity == imported_entity, "Scene Tree did not select the imported Entity.");
    const auto import_history = app.document().undo_count();
    click(app, property_control(app, imported_entity, 0));
    type(app, L"2.5");
    key(app, VK_RETURN);
    require(x(app, imported_entity) == 2.5f && app.document().undo_count() == import_history + 1,
            "Imported Entity parameter edit did not create one document transaction.");
    click(app, app.object_control(imported_entity));
    click(app, operator_item(app, "edit.undo"));
    require(x(app, imported_entity) == 0, "Undo did not restore the imported Entity's position.");
    click(app, operator_item(app, "edit.redo"));
    require(x(app, imported_entity) == 2.5f, "Redo did not restore the imported Entity's edit.");
    click(app, operator_item(app, "view.frame_selected"));
    // Selected geometry keeps its orange edges even when the display overlay is off.
    click(app, control::tree_world);
    require(app.document().selection().entity == atelier::ObjectId{},
            "World selection did not clear the imported Entity's outline.");
    const auto display_document = model(app.document().serialize());
    const auto display_history = app.document().undo_count();
    const auto choose = [&](ControlKey source, ControlKey option) {
        click(app, source);
        click(app, option);
        tick(app);
    };
    choose(control::scene_shading, control::shading_first + 2);
    choose(control::scene_shading, control::edges_first);
    atelier::renderer::Capture shaded;
    require(app.step(&shaded) && shaded.frame_id != 0 &&
                std::any_of(shaded.depth.begin(), shaded.depth.end(), [](float value) { return value > 0; }),
            "Project Entity did not reach an accepted GPU scene frame.");
    png(shaded, output / "project-matcap.png");
    choose(control::scene_shading, control::edges_first + 1);
    atelier::renderer::Capture outlined;
    require(app.step(&outlined) && outlined.frame_id != 0 && outlined.extent == shaded.extent,
            "Project wireframe did not produce an accepted GPU frame.");
    png(outlined, output / "project-wireframe.png");
    const auto viewport = app.presented()->find(control::viewport)->bounds;
    std::size_t changed{};
    for (unsigned row = static_cast<unsigned>(viewport.y); row < static_cast<unsigned>(viewport.y + viewport.height);
         ++row)
        for (unsigned column = static_cast<unsigned>(viewport.x);
             column < static_cast<unsigned>(viewport.x + viewport.width); ++column) {
            const auto offset = (std::size_t(row) * shaded.extent.width + column) * 4;
            if (std::abs(int(shaded.rgba[offset]) - int(outlined.rgba[offset])) > 3 ||
                std::abs(int(shaded.rgba[offset + 1]) - int(outlined.rgba[offset + 1])) > 3 ||
                std::abs(int(shaded.rgba[offset + 2]) - int(outlined.rgba[offset + 2])) > 3)
                ++changed;
        }
    std::printf("Project overlay changed %zu viewport pixels.\n", changed);
    require(changed > 8, "Wireframe did not change the unselected imported mesh's scene pixels.");
    require(outlined.depth == shaded.depth, "Project wireframe changed scene depth.");
    require(app.document().selection().entity == atelier::ObjectId{} &&
                model(app.document().serialize()) == display_document && app.document().undo_count() == display_history,
            "Wireframe changed selection or persistent project state.");
    choose(control::scene_shading, control::edges_first);
    model(Access::invoke(app, "scene.save"));
    tick(app, 2);
    saved = app.document().serialize();
    require(saved.has_value(), "Cannot inspect the saved imported scene.");
    model(app.open_project(folder));
    tick(app, 4);
    require(entities(app).size() == initial_count + 2 && model(app.document().serialize()) == *saved,
            "Save/reopen lost the imported mesh, Entity or parameter values.");
    capture(app, output / "project-import-reopened.png");
    model(atelier::editor::write_file(folder / "Content/Scenes/Broken.scene", "{broken"));
    require(!app.open_scene("Content/Scenes/Broken.scene") && model(app.document().serialize()) == *saved,
            "Failed scene open replaced the current document.");
    const auto metadata = folder / "Content/Meshes/Imported.obj.asset.json";
    const auto metadata_text = model(atelier::assets::read_file(metadata));
    model(atelier::editor::write_file(metadata, "{broken"));
    model(Access::invoke(app, "scene.add", {{"kind", "entity"}}));
    saved = app.document().serialize();
    require(saved.has_value(), "Cannot inspect the modified scene.");
    model(Access::invoke(app, "scene.save"));
    require(!app.document().dirty() && app.status_text().starts_with(L"Scene saved; asset refresh failed:") &&
                model(atelier::assets::read_file(folder / "Content/Scenes/Main.scene")) == *saved,
            "A failed catalog refresh misreported or undid the successful scene save.");
    model(atelier::editor::write_file(metadata, metadata_text));
    std::filesystem::create_directories(folder / "Cache");
    model(atelier::editor::write_file(folder / "Cache/disposable.txt", "Derived data only"));
    std::filesystem::remove(folder / "Cache/disposable.txt");
    std::filesystem::remove(folder / "Cache");
    std::filesystem::rename(folder, moved);
    model(app.open_project(moved));
    tick(app, 4);
    require(model(app.document().serialize()) == *saved, "Moved project depended on its old root or Cache.");
    verify_about(app, moved.wstring());
    capture(app, output / "project-reopened.png");
}
