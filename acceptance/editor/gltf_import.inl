void verify_gltf_import() {
    namespace fs=std::filesystem;namespace mat=atelier::material;
    using Json=nlohmann::json;
    const auto evidence=fs::current_path()/"project-document-evidence"/("gltf-"+std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    const auto source=evidence/"source";fs::create_directories(source/"textures");
    const auto bmp=[](std::uint8_t r,std::uint8_t g,std::uint8_t b) {
        std::string bytes(70,'\0');bytes[0]='B';bytes[1]='M';bytes[2]=70;bytes[10]=54;bytes[14]=40;bytes[18]=2;bytes[22]=2;bytes[26]=1;bytes[28]=32;
        for(std::size_t i=54;i<bytes.size();i+=4) {bytes[i]=char(b);bytes[i+1]=char(g);bytes[i+2]=char(r);bytes[i+3]=char(255);}
        return bytes;
    };
    take(editor::write_file(source/"textures"/"color map.bmp",bmp(255,128,64)));
    take(editor::write_file(source/"textures"/"color copy.bmp",bmp(255,128,64)));
    take(editor::write_file(source/"textures"/"packed.bmp",bmp(0,90,200)));
    take(editor::write_file(source/"textures"/"normal.bmp",bmp(128,128,255)));
    take(editor::write_file(source/"textures"/"corrupt.png",std::string("not an image")));
    // Quad: interleaved POSITION/NORMAL/TANGENT/TEXCOORD_0 (stride 48) with uint16 indices.
    // Triangle: separate POSITION and TEXCOORD_0, uint32 indices, no NORMAL or TANGENT.
    std::string bin;
    const auto put=[&](std::initializer_list<float> values) {for(const float v:values) bin.append(reinterpret_cast<const char*>(&v),4);};
    const float d=.70710678f;
    const float quad_uv[4][2]{{.25f,.75f},{1,.75f},{1,0},{.25f,0}};
    const float quad_position[4][3]{{0,0,0},{1,0,0},{1,1,0},{0,1,0}};
    for(std::size_t i=0;i<4;++i) {
        put({quad_position[i][0],quad_position[i][1],quad_position[i][2]});put({d,0,d});put({1,0,0,1});put({quad_uv[i][0],quad_uv[i][1]});
    }
    for(const std::uint16_t i:{0,1,2,0,2,3}) bin.append(reinterpret_cast<const char*>(&i),2);
    put({0,0,0,1,0,0,0,1,0});put({0,0,1,0,0,1});
    for(const std::uint32_t i:{0u,1u,2u}) bin.append(reinterpret_cast<const char*>(&i),4);
    require(bin.size()==276,"glTF fixture buffer layout changed.");
    take(editor::write_file(source/"scene.bin",bin));
    const auto accessor=[](int view,int offset,const char* type,int component,int count) {
        return Json{{"bufferView",view},{"byteOffset",offset},{"type",type},{"componentType",component},{"count",count}};
    };
    const Json fixture{
        {"asset",{{"version","2.0"}}},{"scenes",{{{"nodes",{0,2,3}}}}},
        {"nodes",{
            {{"translation",{1,2,3}},{"children",{1}}},
            {{"rotation",{0,d,0,d}},{"scale",{2,1,1}},{"mesh",0}},
            {{"scale",{-1,1,1}},{"mesh",1}},
            {{"mesh",1}}}},
        {"meshes",{
            {{"primitives",{
                {{"attributes",{{"POSITION",0},{"NORMAL",1},{"TANGENT",2},{"TEXCOORD_0",3}}},{"indices",4},{"material",0}},
                {{"attributes",{{"POSITION",0},{"TEXCOORD_0",3}}},{"indices",4},{"material",1}}}}},
            {{"primitives",{{{"attributes",{{"POSITION",5},{"TEXCOORD_0",6}}},{"indices",7},{"material",2}}}}}}},
        {"materials",{
            {{"name","opaque"},{"doubleSided",true},{"emissiveFactor",{.1,.2,.3}},
                {"pbrMetallicRoughness",{{"baseColorFactor",{1,.5,.25,1}},{"baseColorTexture",{{"index",0}}},{"roughnessFactor",.5},{"metallicRoughnessTexture",{{"index",1}}}}},
                {"normalTexture",{{"index",2},{"scale",.5}}}},
            {{"name","glass"},{"alphaMode","BLEND"}},
            {{"name","leaf"},{"alphaMode","MASK"},{"alphaCutoff",.3},{"pbrMetallicRoughness",{{"baseColorTexture",{{"index",3}}},{"metallicFactor",0}}}},
            {{"name","unused"},{"pbrMetallicRoughness",{{"baseColorTexture",{{"index",4}}}}}}}},
        {"textures",{{{"source",0},{"sampler",0}},{{"source",1}},{{"source",2}},{{"source",3}},{{"source",4}}}},
        {"samplers",{{{"wrapS",33071},{"wrapT",33071}}}},
        {"images",{{{"uri","textures/color%20map.bmp"}},{{"uri","textures/packed.bmp"}},{{"uri","textures/normal.bmp"}},{{"uri","textures/color%20copy.bmp"}},{{"uri","textures/missing.bmp"}}}},
        {"buffers",{{{"uri","scene.bin"},{"byteLength",276}}}},
        {"bufferViews",{
            {{"buffer",0},{"byteOffset",0},{"byteLength",192},{"byteStride",48}},{{"buffer",0},{"byteOffset",192},{"byteLength",12}},
            {{"buffer",0},{"byteOffset",204},{"byteLength",36}},{{"buffer",0},{"byteOffset",240},{"byteLength",24}},{{"buffer",0},{"byteOffset",264},{"byteLength",12}}}},
        {"accessors",{accessor(0,0,"VEC3",5126,4),accessor(0,12,"VEC3",5126,4),accessor(0,24,"VEC4",5126,4),accessor(0,40,"VEC2",5126,4),
            accessor(1,0,"SCALAR",5123,6),accessor(2,0,"VEC3",5126,3),accessor(3,0,"VEC2",5126,3),accessor(4,0,"SCALAR",5125,3)}}};
    take(editor::write_file(source/"scene.gltf",fixture.dump()));
    const auto close=[](const float* actual,std::initializer_list<double> expected) {
        std::size_t i=0;for(const double e:expected) if(std::abs(actual[i++]-e)>1e-5) return false;
        return true;
    };
    editor::ImageCache cache;
    const auto loaded=editor::load_gltf(source/"scene.gltf",&cache);
    const auto& mesh=*loaded.mesh;const auto vertices=mesh.vertices();const auto parts=mesh.parts();
    require(vertices.size()==12 && parts.size()==3 && mesh.materials().size()==2 &&
        mesh.materials()[0].name=="opaque" && mesh.materials()[1].name=="leaf" &&
        parts[0].vertex_count==6 && parts[0].material==0 && parts[1].material==1 && parts[2].material==1,
        "glTF lost primitives, node instances or material slots, or imported the BLEND primitive.");
    require(loaded.notes.size()==1 && loaded.notes[0].find("glass")!=std::string::npos && loaded.notes[0].find("2 triangles")!=std::string::npos,
        "glTF did not report the skipped BLEND primitive.");
    require(loaded.files.size()==6 && std::ranges::none_of(loaded.files,[](const auto& f){return f.relative.filename()==L"missing.bmp";}),
        "glTF read an image no imported material uses, or missed a dependency.");
    // Node 0 translates (1,2,3); node 1 rotates +90 degrees about Y after scaling X by 2, so
    // glTF (x,y,z) -> (z, y, -2x) + (1,2,3); the engine then mirrors X.
    require(close(vertices[0].position,{-1,2,3}) && close(vertices[1].position,{-1,2,1}) && close(vertices[2].position,{-1,3,1}) &&
        close(vertices[3].position,{-1,2,3}) && close(vertices[4].position,{-1,3,1}) && close(vertices[5].position,{-1,3,3}),
        "glTF node transforms, index order or the left-handed conversion are wrong.");
    // Normal (1,0,1)/sqrt2 takes the inverse transpose: (1/2,0,1) -> rotate (2,0,-1) -> mirror -> (-2,0,-1)/sqrt5.
    // Tangent (1,0,0) takes the matrix: (0,0,-2), unchanged by the mirror, then is orthogonalized: (1,0,-2)/sqrt5.
    // The world determinant is negative, so the authored handedness +1 flips to -1.
    require(close(vertices[0].normal,{-.894427,0,-.447214}) && close(vertices[0].tangent,{.447214,0,-.894427,-1}),
        "glTF normals or authored tangents did not follow the node transform and mirror.");
    require(close(vertices[0].uv,{.25,.75}) && close(vertices[2].uv,{1,0}),"glTF texture coordinates were changed.");
    // The triangle (0,0,0),(1,0,0),(0,1,0) faces +Z in glTF. Node 2 mirrors X itself, so the engine
    // positions equal the source; node 3 is mirrored once by the conversion. Both still face +Z.
    // glTF normal maps put +Y toward -V, so the bitangent cross(N,T)*w must point to -Y here.
    require(close(vertices[6].position,{0,0,0}) && close(vertices[7].position,{1,0,0}) && close(vertices[8].position,{0,1,0}) &&
        close(vertices[10].position,{-1,0,0}) && close(vertices[6].normal,{0,0,1}) && close(vertices[9].normal,{0,0,1}) &&
        close(vertices[6].tangent,{1,0,0,-1}) && close(vertices[9].tangent,{-1,0,0,1}),
        "glTF flat normals or generated tangents ignore the front side or the normal-map convention.");
    const auto& opaque=mesh.materials()[0].surface->description();const auto& leaf=mesh.materials()[1].surface->description();
    const auto& roughness=opaque.texture(mat::Input::roughness);const auto& metallic=opaque.texture(mat::Input::metallic);
    require(roughness.image && roughness.image==metallic.image && roughness.channel==1 && metallic.channel==2 &&
        roughness.image->pixels()[1]==90 && roughness.image->pixels()[2]==200 &&
        opaque.parameters.roughness==.5f && opaque.parameters.metallic==1,
        "glTF metallicRoughness did not map G to roughness and B to metallic with glTF factor defaults.");
    require(opaque.texture(mat::Input::normal).image && opaque.parameters.normal_scale==.5f &&
        opaque.texture(mat::Input::base_color).address==mat::Address::clamp && opaque.texture(mat::Input::normal).address==mat::Address::repeat &&
        close(opaque.parameters.base_color.data(),{1,.5,.25,1}) && close(opaque.parameters.emissive.data(),{.1,.2,.3}),
        "glTF base color, emission, normal scale or sampler wrap was not imported.");
    require(opaque.parameters.coverage==mat::Coverage::opaque && opaque.parameters.double_sided && opaque.parameters.emission_two_sided &&
        leaf.parameters.coverage==mat::Coverage::mask && std::abs(leaf.parameters.alpha_cutoff-.3f)<1e-6f &&
        !leaf.parameters.double_sided && !leaf.parameters.emission_two_sided && leaf.parameters.metallic==0,
        "glTF alpha mode, cutoff or sidedness was not imported.");
    require(leaf.texture(mat::Input::base_color).image==opaque.texture(mat::Input::base_color).image && cache.entries.size()==3,
        "Images with the same bytes did not share decoded pixels.");
    require(editor::load_gltf(source/"scene.gltf",&cache).mesh->materials()[0].surface->description().texture(mat::Input::base_color).image==
        opaque.texture(mat::Input::base_color).image,"A second import did not reuse cached pixels.");
    const auto uncached=editor::load_gltf(source/"scene.gltf");
    require(uncached.mesh->materials()[1].surface->description().texture(mat::Input::base_color).image!=opaque.texture(mat::Input::base_color).image,
        "Uncached glTF loads shared pixels through hidden state.");
    {
        const std::string pixels=bmp(1,2,3);const std::array<std::string_view,2> two{pixels,pixels};
        bool limited=false;
        try {static_cast<void>(editor::decode_images(two,nullptr,31));}
        catch(const std::runtime_error& error) {limited=std::string_view(error.what()).starts_with("Decoded textures exceed");}
        require(limited && editor::decode_images(two,nullptr,32).size()==2,"Image decoding did not hold its pixel budget.");
    }
    {
        // Float normals are not exactly unit: a tangent equal to its normal must still take the fallback.
        const float d=.70710677f;std::vector<atelier::geometry::Vertex> triangle(3);
        triangle[1].position[0]=1;triangle[2].position[1]=1;
        for(auto& v:triangle) {v.normal[0]=v.normal[2]=d;v.tangent[0]=v.tangent[2]=d;v.tangent[3]=1;}
        const atelier::geometry::Mesh parallel(std::move(triangle),{},{},{},atelier::geometry::Tangents::authored);
        for(const auto& v:parallel.vertices())
            require(std::abs(v.tangent[0]*v.normal[0]+v.tangent[1]*v.normal[1]+v.tangent[2]*v.normal[2])<1e-5f &&
                std::abs(std::hypot(v.tangent[0],v.tangent[1],v.tangent[2])-1)<1e-5f,"An authored tangent parallel to its normal stayed parallel.");
        // A long tangent within rounding of its normal must take the fallback (+X here), not its residual (+Y).
        std::vector<atelier::geometry::Vertex> scaled(3);
        scaled[1].position[0]=1;scaled[2].position[1]=1;
        for(auto& v:scaled) {v.normal[2]=1;v.tangent[1]=1e-5f;v.tangent[2]=1000;v.tangent[3]=1;}
        const atelier::geometry::Mesh long_parallel(std::move(scaled),{},{},{},atelier::geometry::Tangents::authored);
        for(const auto& v:long_parallel.vertices())
            require(close(v.tangent,{1,0,0,1}),"A long authored tangent parallel to its normal kept its rounding residual as the tangent.");
    }

    auto variant=fixture;variant["meshes"][1]["primitives"][0].erase("material");
    take(editor::write_file(source/"variant.gltf",variant.dump()));
    {
        const auto implicit=editor::load_gltf(source/"variant.gltf");const auto& fallback=implicit.mesh->materials().back();
        const auto& p=fallback.surface->description().parameters;
        require(implicit.mesh->materials().size()==2 && fallback.name=="default" && p.metallic==1 && p.roughness==1 &&
            !p.double_sided && p.coverage==mat::Coverage::opaque,"A primitive without a material did not get the glTF default material.");
    }
    const auto rejects=[&](Json document,std::string_view expected,const char* message) {
        take(editor::write_file(source/"variant.gltf",document.dump()));
        try {static_cast<void>(editor::load_gltf(source/"variant.gltf"));}
        catch(const std::runtime_error& error) {if(std::string_view(error.what()).find(expected)!=std::string_view::npos) return;throw std::runtime_error(std::string(message)+" Got: "+error.what());}
        throw std::runtime_error(message);
    };
    variant=fixture;variant["extensionsRequired"]={"KHR_draco_mesh_compression"};
    rejects(variant,"required extensions","glTF ignored a required extension.");
    variant=fixture;variant["meshes"][0]["primitives"][0]["mode"]=1;
    rejects(variant,"triangle-list","glTF accepted a line primitive.");
    variant=fixture;variant["accessors"][0]["sparse"]=Json::object();
    rejects(variant,"sparse","glTF accepted a sparse accessor.");
    variant=fixture;variant["accessors"][0]["count"]=5;
    rejects(variant,"exceeds its buffer","glTF read past a buffer view.");
    variant=fixture;variant["buffers"][0]["uri"]="%2E%2E/scene.bin";
    rejects(variant,"inside the source directory","glTF followed an encoded parent-directory uri.");
    variant=fixture;variant["images"][2]["uri"]="textures/corrupt.png";
    rejects(variant,"Cannot decode texture","A failed parallel decode was not reported.");

    auto project=take(editor::Project::create(evidence/"project"));
    const auto key=take(project.import_mesh(source/"scene.gltf"));
    const auto imported=evidence/"project"/"Content"/"Meshes"/"scene";
    require(fs::exists(imported/"scene.gltf") && fs::exists(imported/"scene.bin") && fs::exists(imported/"textures"/"color map.bmp") &&
        !fs::exists(imported/"textures"/"missing.bmp") && project.mesh_notes(key).size()==1,
        "Import Mesh did not publish the glTF bundle with its notes.");
    auto reopened=take(editor::Project::open(project.root()));
    const auto asset=take(reopened.mesh(key));
    require(asset.mesh->vertices().size()==12 && reopened.mesh_notes(key).size()==1,"A reopened project did not load its glTF mesh.");
    std::puts("glTF import acceptance: PASS (node transforms, left-handed conversion, authored and generated tangents, PBR channels, MASK, BLEND notes, shared images, rejections, project import)");
}
