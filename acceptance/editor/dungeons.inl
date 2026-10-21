void verify_dungeons() {
    namespace fs=std::filesystem;
    const auto root=editor::asset_directory()/"examples";
    const auto folder=root/"Examples/Dungeons";
    auto project=take(editor::Project::open(root));
    const auto manifest=nlohmann::json::parse(take(editor::read_file(folder/"Import/models.json")));
    require(manifest.size()==2314,"Dungeons model library is incomplete.");
    std::size_t models{},textured{};std::unordered_set<const material::Image*> images;
    for(const auto& row:manifest) {
        const auto mesh=take(project.mesh(row.at("resource").get<std::string>())).mesh;
        require(mesh->vertices().size()/3==row.at("triangles").get<std::size_t>(),"Converted dungeon model lost triangles.");
        for(const auto& material:mesh->materials()) if(material.surface->description().texture(atelier::material::Input::base_color).image) {++textured;images.insert(material.surface->description().texture(atelier::material::Input::base_color).image.get());}
        ++models;
    }
    require(textured>1000 && images.size()>10 && images.size()<100,"Dungeons textures are missing or shared atlases were decoded per model.");
    const auto summary=nlohmann::json::parse(take(editor::read_file(folder/"Import/summary.json")));
    for(const auto& scene:summary.at("scenes")) {
        const auto path=folder/editor::wide(scene.at("file").get<std::string>());
        const auto original=take(editor::read_file(path));
        auto document=take(editor::Session::deserialize(original,project));
        require(take(document->world().members()).size()==scene.at("entities").get<std::size_t>() &&
            take(document->presentation().render_items()).size()==scene.at("visible_meshes").get<std::size_t>(),"Dungeons scene lost authored model instances.");
        const auto saved=take(document->serialize());auto reopened=take(editor::Session::deserialize(saved,project));
        const auto after=take(reopened->serialize());
        require(atelier::acceptance::dungeons_roundtrip_equal(saved,after) && take(editor::read_file(path))==original,"Dungeons scene round trip changed data or the template.");
        // Scene Tree lays out a window of rows, whatever the World size, and still reaches any Entity.
        editor::Workspace workspace;workspace.project_open=true;
        const std::array fonts{(editor::asset_directory()/"fonts/SarasaUiSC-Regular.ttf").wstring()};
        ui::Session surface(fonts);
        const auto theme=ui::StyleSheet::parse(take(editor::read_file(editor::asset_directory()/"editor.ss")));
        require(theme && surface.set_style_sheet(*theme) && surface.set_surface({{1779,1154},1,1}),"Dungeons UI surface failed.");
        std::vector<ui::Control> controls;
        const auto present=[&] {
            controls=workspace.describe(*document,{1779,1154},surface.presented().get(),&project,&surface);
            require(workspace.ui_error().empty() && surface.set_controls(controls),"Dungeons Scene Tree description was refused.");
            workspace.commit_ui_controls();
            const auto frame=surface.prepare();require(frame && surface.accept(*frame),"Dungeons Scene Tree layout failed.");
        };
        present();present();
        const auto members=take(document->world().members());
        const auto roots=std::ranges::count_if(members,[&](const auto& member){
            return take(world::parent_entity(document->world().runtime(),member.id()))==ObjectId{};});
        const auto tree=std::ranges::find(controls,editor::control::entity_tree,&ui::Control::key);
        const auto described=std::ranges::count_if(controls,[](const auto& c){return c.parent==editor::control::entity_tree;});
        const auto row_limit=4*surface.tree_viewport(editor::control::entity_tree).height/20+16;
        require(tree!=controls.end() && tree->tree_row_count==roots+1 && described<=row_limit,"Dungeons Scene Tree did not describe a window of its rows.");
        const auto last=members.back().id();
        take(document->select(last));present();
        const auto* row=surface.presented()->find(workspace.object_control(last));
        require(row && row->bounds.y>=row->clip.top-1 && row->bounds.y+row->bounds.height<=row->clip.bottom+1 &&
            std::ranges::count_if(controls,[](const auto& c){return c.parent==editor::control::entity_tree;})<=row_limit,
            "Selecting the last Dungeons Entity did not expand and reveal its row.");
    }
    std::size_t fbx{},materials{},textures{};
    for(const auto& file:fs::recursive_directory_iterator(folder/"Originals")) if(file.is_regular_file()) {
        const auto extension=file.path().extension();
        require(extension!=L".cs" && extension!=L".dll" && extension!=L".js","Unity scripts were included in the official example.");
        fbx+=extension==L".fbx";materials+=extension==L".mat";
        textures+=extension==L".png" || extension==L".tga" || extension==L".tif";
    }
    require(fbx==1268 && materials==152 && textures==168,"Original model/material/texture resources are incomplete.");
    std::printf("Dungeons document acceptance: PASS (%zu models, %zu shared atlases, 3 scenes, 1268 FBX/152 materials/168 textures, no scripts, save/reopen)\n",models,images.size());
}
