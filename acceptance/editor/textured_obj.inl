void verify_textured_obj() {
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
        old->materials()[1].color==old->materials()[2].color,"OBJ lost triangles, material parts or shared images.");
    require(old->materials()[1].color->pixels()[0]==255,
        "Decoded material pixel is wrong.");
    bool negative_uv=false;double area=0;
    for(const auto& vertex:old->vertices()) negative_uv|=vertex.uv[0]<0;
    for(std::size_t i=0;i<9;i+=3) {const auto& a=old->vertices()[i].position;const auto& b=old->vertices()[i+1].position;const auto& c=old->vertices()[i+2].position;
        area+=std::abs((b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]))*.5;}
    require(negative_uv && area==3,"OBJ changed wrap coordinates or filled a concave cutout.");
    require(!project.import_mesh(source/"scene.obj"),"Repeated import overwrote an existing material bundle.");
    const auto revision=project.revision();take(project.refresh());
    require(project.revision()==revision && take(project.mesh(key)).mesh==old,"Unchanged refresh replaced a mesh resource.");
    const auto texture=project.root()/"Content/Meshes/scene/color.bmp";
    for(std::size_t i=54;i<bmp.size();i+=4) {bmp[i]=char(255);bmp[i+2]=0;}
    take(editor::write_file(texture,bmp));fs::last_write_time(texture,fs::last_write_time(texture)+std::chrono::seconds(2));take(project.refresh());
    auto changed=take(project.mesh(key)).mesh;
    require(changed!=old && changed->materials()[1].color->pixels()[2]==255 && old->materials()[1].color->pixels()[0]==255,
        "Texture refresh did not publish a new resource while preserving its old owner.");
    take(editor::write_file(project.root()/"Content/Meshes/scene/scene.mtl","newmtl red\nKd 0 1 0\nmap_Kd color.bmp\nnewmtl other\nKd 1 1 1\n"));
    take(project.refresh());require(take(project.mesh(key)).mesh->materials()[1].diffuse[0]==0,"MTL edits did not invalidate the cached mesh.");
    fs::remove(texture);take(project.refresh());require(!project.mesh(key),"Missing material dependency was silently ignored.");
    take(editor::write_file(source/"bad.obj","mtllib ../outside.mtl\nv 0 0 0\n"));
    require(!project.import_mesh(source/"bad.obj") && !fs::exists(project.root()/"Content/Meshes/bad.obj"),"Unsafe dependency left a published import.");
    take(editor::write_file(source/"bad.obj","mtllib absent.mtl\n"));require(!project.import_mesh(source/"bad.obj"),"Missing MTL was silently ignored.");
    take(editor::write_file(source/"bad.obj","v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nf 1/2 2/1 3/1\n"));
    require(!project.import_mesh(source/"bad.obj"),"Out-of-range UV index was accepted.");
    std::puts("Textured OBJ acceptance: PASS (bundle import, concave polygon, UV, shared texture, refresh, lifetime, failure/retry boundaries)");
}
void verify_sponza_project() {
    const auto folder=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path()/"examples/physics-test";
    auto project=take(editor::Project::open(folder));
    auto document=take(editor::Session::deserialize(take(editor::read_file(folder/"Content/Scenes/Sponza.scene")),project));
    const auto items=take(document->world().render_items());require(items.size()==1,"Sponza scene should contain one complete static model.");
    const auto mesh=items.front().geometry;
    require(mesh->vertices().size()==786801 && mesh->materials().size()==26 && mesh->parts().size()>20,"Sponza geometry/material completeness changed.");
    std::size_t textured=0;for(const auto& material:mesh->materials()) textured+=bool(material.color);
    require(textured==24,"Sponza diffuse material count changed.");
    const auto saved=take(document->serialize());auto reopened=take(editor::Session::deserialize(saved,project));
    require(take(reopened->serialize())==saved && take(reopened->world().render_items()).front().geometry==mesh,
        "Scene save/reopen lost material-backed shared geometry.");
    std::printf("Sponza project acceptance: PASS (%zu triangles, %zu parts, %zu textured materials)\n",mesh->vertices().size()/3,mesh->parts().size(),textured);
}
