void verify_material_picking() {
    namespace mat=atelier::material;
    const std::vector<atelier::geometry::Vertex> vertices{
        {{-1,-1,0},{0,0,-1},{0,1}},{{-1,1,0},{0,0,-1},{0,0}},{{1,1,0},{0,0,-1},{1,0}},
        {{-1,-1,0},{0,0,-1},{0,1}},{{1,1,0},{0,0,-1},{1,0}},{{1,-1,0},{0,0,-1},{1,1}}};
    mat::Description cutout;cutout.parameters.coverage=mat::Coverage::mask;
    cutout.texture(mat::Input::opacity).image=std::make_shared<const mat::Image>(2,2,
        std::vector<std::uint8_t>{0,0,0,255,255,255,255,255,255,255,255,255,255,255,255,255});
    const auto mesh=[&](const mat::Description& value) {
        return std::make_shared<const atelier::geometry::Mesh>(vertices,std::vector<std::uint8_t>{},
            std::vector<mat::Slot>{{"surface",std::make_shared<const mat::Surface>(value)}});
    };
    editor::Session document;
    changed(document.create_static_mesh({{},L"Cutout",mesh(cutout)}));const auto front=document.selection().entity;
    const auto front_component=component(document,front,document.presentation_types().static_mesh_component);
    changed(document.create_static_mesh({{},L"Receiver",mesh({})},{0,0,.8f}));const auto back=document.selection().entity;
    editor::Camera camera;require(camera.look_at({0,0,-5},{0,0,0}),"Material picking camera is invalid.");
    auto view=editor::ViewportView::make(camera,{0,0,800,600},1);
    editor::ViewportTool tool;
    const auto pick=[&](math::Vector3 position) {const auto p=view.project(position);require(p.has_value(),"Material picking point is outside its viewport.");return tool.pick(document,view,p->x,p->y);};
    require(pick({-.5f,.5f,0})==back && pick({.5f,.5f,0})==front,
        "Point picking did not pass through transparent texels to the rear Entity.");
    cutout.texture(mat::Input::opacity).scale[0]=-1;
    take(presentation::set_geometry(document.world().runtime(),front_component,mesh(cutout)));
    require(pick({-.5f,.5f,0})==front && pick({.5f,.5f,0})==back,
        "Point picking ignored mirrored negative UV repetition.");
    cutout.parameters.coverage=mat::Coverage::opaque;cutout.parameters.double_sided=false;
    take(presentation::set_geometry(document.world().runtime(),front_component,mesh(cutout)));
    take(document.select(back));changed(document.erase_selected());
    require(pick({0,0,0})==front,"Single-sided picking discarded the authored front side.");
    require(camera.look_at({0,0,5},{0,0,0}),"Backface picking camera is invalid.");view=editor::ViewportView::make(camera,{0,0,800,600},1);
    require(!pick({0,0,0}),"Point picking selected an invisible single-sided backface.");
    std::puts("Material picking acceptance: PASS (transparent holes, UV sampling, source replacement, sidedness)");
}
void verify_textured_obj() {
    verify_material_picking();
    namespace fs=std::filesystem;
    const auto evidence=fs::current_path()/"project-document-evidence"/std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    const auto source=evidence/"source";fs::create_directories(source);
    // An actual 2x2 BGRA BMP travels through WIC and the real import transaction.
    std::string bmp(70,'\0');bmp[0]='B';bmp[1]='M';bmp[2]=70;bmp[10]=54;bmp[14]=40;bmp[18]=2;bmp[22]=2;bmp[26]=1;bmp[28]=32;
    for(std::size_t i=54;i<bmp.size();i+=4) {bmp[i+2]=char(255);bmp[i+3]=char(255);}
    take(editor::write_file(source/"color.bmp",bmp));
    take(editor::write_file(source/"scene.mtl","newmtl red\nKd 1 0.5 0.25\nmap_Kd color.bmp\nnewmtl other\nKd 0 1 0\nmap_Kd color.bmp\n"));
    const std::string obj="mtllib scene.mtl\nv 0 0 0\nv 2 0 0\nv 2 2 0\nv 1 1 0\nv 0 2 0\nvt -1 0\nvt 2 0\nvt 2 1\nvt 0.5 0.5\nvt 0 1\nusemtl red\nf 1/1 2/2 3/3 4/4 5/5\nusemtl other\nf -5/1 -4/2 -2/4\n";
    take(editor::write_file(source/"scene.obj",obj));
    auto project=take(editor::Project::create(evidence/"project"));const auto key=take(project.import_mesh(source/"scene.obj"));
    auto old=take(project.mesh(key)).mesh;
    require(old->vertices().size()==12 && old->parts().size()==2 && old->materials().size()==3 &&
        old->materials()[1].surface->description().texture(atelier::material::Input::base_color).image==old->materials()[2].surface->description().texture(atelier::material::Input::base_color).image,"OBJ lost triangles, material parts or shared images.");
    require(old->materials()[1].surface->description().texture(atelier::material::Input::base_color).image->pixels()[0]==255,
        "Decoded material pixel is wrong.");
    bool negative_uv=false;double area=0;
    for(const auto& vertex:old->vertices()) negative_uv|=vertex.uv[0]<0;
    for(std::size_t i=0;i<9;i+=3) {const auto& a=old->vertices()[i].position;const auto& b=old->vertices()[i+1].position;const auto& c=old->vertices()[i+2].position;
        area+=std::abs((b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]))*.5;}
    require(negative_uv && area==3,"OBJ changed wrap coordinates or filled a concave cutout.");
    require(!project.import_mesh(source/"scene.obj"),"Repeated import overwrote an existing material bundle.");
    const auto revision=project.revision();take(project.refresh());
    require(project.revision()==revision && take(project.mesh(key)).mesh==old,"Unchanged refresh replaced a mesh resource.");
    const auto sibling=project.root()/"Content/Meshes/scene/sibling.obj";
    take(editor::write_file(sibling,obj));take(project.refresh());
    const auto sibling_entry=std::ranges::find(project.entries(),fs::path("Content/Meshes/scene/sibling.obj"),&editor::ProjectEntry::path);
    require(sibling_entry!=project.entries().end(),"Shared-texture mesh is absent from the real project catalog.");
    const auto sibling_key=sibling_entry->key;
    auto sibling_mesh=take(project.mesh(sibling_key)).mesh;
    // import_mesh already owns its first bundle; reopen to exercise the project cache.
    auto shared_project=take(editor::Project::open(project.root()));
    auto shared_a=take(shared_project.mesh(key)).mesh,shared_b=take(shared_project.mesh(sibling_key)).mesh;
    require(shared_a!=shared_b && shared_a->materials()[1].surface->description().texture(atelier::material::Input::base_color).image==shared_b->materials()[1].surface->description().texture(atelier::material::Input::base_color).image,
        "Different model assets decoded the same atlas more than once.");
    const auto texture=project.root()/"Content/Meshes/scene/color.bmp";
    for(std::size_t i=54;i<bmp.size();i+=4) {bmp[i]=char(255);bmp[i+2]=0;}
    take(editor::write_file(texture,bmp));fs::last_write_time(texture,fs::last_write_time(texture)+std::chrono::seconds(2));take(project.refresh());
    auto changed=take(project.mesh(key)).mesh;
    auto sibling_changed=take(project.mesh(sibling_key)).mesh;
    require(sibling_changed!=sibling_mesh && changed->materials()[1].surface->description().texture(atelier::material::Input::base_color).image==sibling_changed->materials()[1].surface->description().texture(atelier::material::Input::base_color).image &&
        sibling_mesh->materials()[1].surface->description().texture(atelier::material::Input::base_color).image->pixels()[0]==255,"Shared atlas refresh lost sharing or retired pixels.");
    require(changed!=old && changed->materials()[1].surface->description().texture(atelier::material::Input::base_color).image->pixels()[2]==255 && old->materials()[1].surface->description().texture(atelier::material::Input::base_color).image->pixels()[0]==255,
        "Texture refresh did not publish a new resource while preserving its old owner.");
    take(editor::write_file(project.root()/"Content/Meshes/scene/scene.mtl","newmtl red\nKd 0 1 0\nmap_Kd color.bmp\nnewmtl other\nKd 1 1 1\n"));
    take(project.refresh());require(take(project.mesh(key)).mesh->materials()[1].surface->description().parameters.base_color[0]==0,"MTL edits did not invalidate the cached mesh.");
    fs::remove(texture);take(project.refresh());require(!project.mesh(key),"Missing material dependency was silently ignored.");
    take(editor::write_file(source/"bad.obj","mtllib ../outside.mtl\nv 0 0 0\n"));
    require(!project.import_mesh(source/"bad.obj") && !fs::exists(project.root()/"Content/Meshes/bad.obj"),"Unsafe dependency left a published import.");
    take(editor::write_file(source/"bad.obj","mtllib absent.mtl\n"));require(!project.import_mesh(source/"bad.obj"),"Missing MTL was silently ignored.");
    take(editor::write_file(source/"bad.obj","v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nf 1/2 2/1 3/1\n"));
    require(!project.import_mesh(source/"bad.obj"),"Out-of-range UV index was accepted.");
    take(editor::write_file(source/"degenerate_uv.obj",
        "v 0 0 0\nv 0 1 0\nv 0 0 1\nvt 0 0\nvn 1 0 0\nf 1/1/1 2/1/1 3/1/1\n"));
    const auto degenerate_key=take(project.import_mesh(source/"degenerate_uv.obj"));
    const auto degenerate=take(project.mesh(degenerate_key)).mesh;
    for(const auto& vertex:degenerate->vertices()) {
        const auto& n=vertex.normal;const auto& t=vertex.tangent;
        require(n[0]==1 && n[1]==0 && n[2]==0,"Tangent generation changed the authored OBJ normal.");
        require(std::abs(std::hypot(double(t[0]),double(t[1]),double(t[2]))-1)<1e-6 &&
            std::abs(double(n[0])*t[0]+double(n[1])*t[1]+double(n[2])*t[2])<1e-6 && std::abs(t[3])==1,
            "Degenerate OBJ UVs did not retain a finite orthonormal tangent frame.");
    }
    std::puts("Textured OBJ acceptance: PASS (bundle import, concave polygon, UV, tangent fallback, shared texture, refresh, lifetime, failure/retry boundaries)");
}
void verify_sponza_project() {
    const std::filesystem::path folder=ATELIER_EXAMPLE_PROJECT;
    auto project=take(editor::Project::open(folder));
    auto document=take(editor::Session::deserialize(take(editor::read_file(folder/"Content/Sponza/Scenes/Sponza.scene")),project,{},editor::LoadPolicy::strict));
    const auto items=take(document->presentation().render_items());require(items.size()==2,"Sponza scene should contain the building and its curtains.");
    // Every slot shows the material generated from its glTF material; counts follow the shipped 2K assets.
    struct Expected {std::size_t triangles,slots,textured;};
    constexpr std::array expected{Expected{3738878,27,24},Expected{1997366,4,4}};
    std::size_t triangles{};
    for(std::size_t i=0;i<items.size();++i) {
        const auto& mesh=*items[i].geometry;std::size_t textured{};
        for(const auto& surface:*items[i].surfaces) textured+=bool(surface->description().texture(atelier::material::Input::base_color).image);
        const auto state=take(presentation::read_static_mesh(document->world().runtime(),items[i].component));
        const auto* entry=project.entry(state.mesh.source);
        require(mesh.vertices().size()==expected[i].triangles*3 && mesh.materials().size()==expected[i].slots && state.mesh.defaults.size()==expected[i].slots &&
            entry && entry->materials.size()==expected[i].slots && textured==expected[i].textured,"Sponza geometry, default materials or textured materials changed.");
        // Each slot's default is the material generated from the glTF material of the same name.
        for(const auto& slot:entry->materials) {
            const auto* material=project.entry(slot.key);
            require(material && material->path==entry->path.parent_path()/"Materials"/(slot.slot+".material"),"A Sponza slot defaults to another slot's material.");
        }
        triangles+=mesh.vertices().size()/3;
    }
    const auto saved=take(document->serialize());auto reopened=take(editor::Session::deserialize(saved,project));
    require(take(reopened->serialize())==saved && take(reopened->presentation().render_items()).front().geometry==items.front().geometry,
        "Scene save/reopen lost material-backed shared geometry.");
    const auto lights=take(reopened->presentation().light_items());
    require(lights.size()==3 && std::ranges::count_if(lights,[](const auto& light){return light.settings.kind==presentation::LightKind::directional;})==1 &&
        std::ranges::all_of(lights,[](const auto& light){return light.settings.casts_shadow && light.settings.intensity>0;}),
        "Sponza save/reopen lost its directional and two local lights.");
    // The sky is the sun-removed derivative; the scene's Sun light alone supplies direct sunlight.
    const auto sky=take(presentation::read_sky_map(reopened->world().runtime(),reopened->environment()));
    const auto* map=project.entry(sky.source);
    require(take(reopened->presentation().environment()).sky_enabled && sky.resolved && map &&
        map->path==std::filesystem::path("Content/Sponza/Sky/kloppenheim_05_4k_nosun.hdr"),"Sponza save/reopen lost its sun-removed sky map.");
    std::printf("Sponza project acceptance: PASS (%zu triangles, %zu meshes, every slot on its default material)\n",triangles,items.size());
}
