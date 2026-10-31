void verify_gltf_import() {
    namespace fs = std::filesystem;
    namespace mat = atelier::material;
    using Json = nlohmann::json;
    const auto evidence = fs::current_path() / "project-document-evidence" /
                          ("gltf-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    const auto source = evidence / "source";
    fs::create_directories(source / "textures");
    const auto bmp = [](std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        std::string bytes(70, '\0');
        bytes[0] = 'B';
        bytes[1] = 'M';
        bytes[2] = 70;
        bytes[10] = 54;
        bytes[14] = 40;
        bytes[18] = 2;
        bytes[22] = 2;
        bytes[26] = 1;
        bytes[28] = 32;
        for (std::size_t i = 54; i < bytes.size(); i += 4) {
            bytes[i] = char(b);
            bytes[i + 1] = char(g);
            bytes[i + 2] = char(r);
            bytes[i + 3] = char(255);
        }
        return bytes;
    };
    take(editor::write_file(source / "textures" / "color map.bmp", bmp(255, 128, 64)));
    take(editor::write_file(source / "textures" / "color copy.bmp", bmp(255, 128, 64)));
    take(editor::write_file(source / "textures" / "packed.bmp", bmp(0, 90, 200)));
    take(editor::write_file(source / "textures" / "normal.bmp", bmp(128, 128, 255)));
    take(editor::write_file(source / "textures" / "corrupt.png", std::string("not an image")));
    // Quad: interleaved POSITION/NORMAL/TANGENT/TEXCOORD_0 (stride 48) with uint16 indices.
    // Triangle: separate POSITION and TEXCOORD_0, uint32 indices, no NORMAL or TANGENT.
    std::string bin;
    const auto put = [&](std::initializer_list<float> values) {
        for (const float v : values)
            bin.append(reinterpret_cast<const char*>(&v), 4);
    };
    const float d = .70710678f;
    const float quad_uv[4][2]{{.25f, .75f}, {1, .75f}, {1, 0}, {.25f, 0}};
    const float quad_position[4][3]{{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    for (std::size_t i = 0; i < 4; ++i) {
        put({quad_position[i][0], quad_position[i][1], quad_position[i][2]});
        put({d, 0, d});
        put({1, 0, 0, 1});
        put({quad_uv[i][0], quad_uv[i][1]});
    }
    for (const std::uint16_t i : {0, 1, 2, 0, 2, 3})
        bin.append(reinterpret_cast<const char*>(&i), 2);
    put({0, 0, 0, 1, 0, 0, 0, 1, 0});
    put({0, 0, 1, 0, 0, 1});
    for (const std::uint32_t i : {0u, 1u, 2u})
        bin.append(reinterpret_cast<const char*>(&i), 4);
    require(bin.size() == 276, "glTF fixture buffer layout changed.");
    take(editor::write_file(source / "scene.bin", bin));
    const auto accessor = [](int view, int offset, const char* type, int component, int count) {
        return Json{{"bufferView", view},
                    {"byteOffset", offset},
                    {"type", type},
                    {"componentType", component},
                    {"count", count}};
    };
    const Json fixture{
        {"asset", {{"version", "2.0"}}},
        {"scenes", {{{"nodes", {0, 2, 3}}}}},
        {"nodes",
         {{{"translation", {1, 2, 3}}, {"children", {1}}},
          {{"rotation", {0, d, 0, d}}, {"scale", {2, 1, 1}}, {"mesh", 0}},
          {{"scale", {-1, 1, 1}}, {"mesh", 1}},
          {{"mesh", 1}}}},
        {"meshes",
         {{{"primitives",
            {{{"attributes", {{"POSITION", 0}, {"NORMAL", 1}, {"TANGENT", 2}, {"TEXCOORD_0", 3}}},
              {"indices", 4},
              {"material", 0}},
             {{"attributes", {{"POSITION", 0}, {"TEXCOORD_0", 3}}}, {"indices", 4}, {"material", 1}}}}},
          {{"primitives", {{{"attributes", {{"POSITION", 5}, {"TEXCOORD_0", 6}}}, {"indices", 7}, {"material", 2}}}}}}},
        {"materials",
         {{{"name", "opaque"},
           {"doubleSided", true},
           {"emissiveFactor", {.1, .2, .3}},
           {"pbrMetallicRoughness",
            {{"baseColorFactor", {1, .5, .25, 1}},
             {"baseColorTexture", {{"index", 0}}},
             {"roughnessFactor", .5},
             {"metallicRoughnessTexture", {{"index", 1}}}}},
           {"normalTexture", {{"index", 2}, {"scale", .5}}}},
          {{"name", "glass"}, {"alphaMode", "BLEND"}},
          {{"name", "leaf"},
           {"alphaMode", "MASK"},
           {"alphaCutoff", .3},
           {"pbrMetallicRoughness", {{"baseColorTexture", {{"index", 3}}}, {"metallicFactor", 0}}}},
          {{"name", "unused"}, {"pbrMetallicRoughness", {{"baseColorTexture", {{"index", 4}}}}}}}},
        {"textures",
         {{{"source", 0}, {"sampler", 0}}, {{"source", 1}}, {{"source", 2}}, {{"source", 3}}, {{"source", 4}}}},
        {"samplers", {{{"wrapS", 33071}, {"wrapT", 33071}}}},
        {"images",
         {{{"uri", "textures/color%20map.bmp"}},
          {{"uri", "textures/packed.bmp"}},
          {{"uri", "textures/normal.bmp"}},
          {{"uri", "textures/color%20copy.bmp"}},
          {{"uri", "textures/missing.bmp"}}}},
        {"buffers", {{{"uri", "scene.bin"}, {"byteLength", 276}}}},
        {"bufferViews",
         {{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", 192}, {"byteStride", 48}},
          {{"buffer", 0}, {"byteOffset", 192}, {"byteLength", 12}},
          {{"buffer", 0}, {"byteOffset", 204}, {"byteLength", 36}},
          {{"buffer", 0}, {"byteOffset", 240}, {"byteLength", 24}},
          {{"buffer", 0}, {"byteOffset", 264}, {"byteLength", 12}}}},
        {"accessors",
         {accessor(0, 0, "VEC3", 5126, 4), accessor(0, 12, "VEC3", 5126, 4), accessor(0, 24, "VEC4", 5126, 4),
          accessor(0, 40, "VEC2", 5126, 4), accessor(1, 0, "SCALAR", 5123, 6), accessor(2, 0, "VEC3", 5126, 3),
          accessor(3, 0, "VEC2", 5126, 3), accessor(4, 0, "SCALAR", 5125, 3)}}};
    take(editor::write_file(source / "scene.gltf", fixture.dump()));
    const auto close = [](const auto& actual, std::initializer_list<double> expected) {
        std::size_t i = 0;
        for (const double e : expected)
            if (std::abs(actual[i++] - e) > 1e-5)
                return false;
        return true;
    };
    assets::ImageCache cache;
    const auto loaded = assets::load_gltf(source / "scene.gltf", &cache);
    const auto& mesh = *loaded.mesh;
    const auto vertices = mesh.vertices();
    const auto parts = mesh.parts();
    require(vertices.size() == 12 && parts.size() == 3 && mesh.materials().size() == 2 &&
                mesh.materials()[0].name == "opaque" && mesh.materials()[1].name == "leaf" &&
                parts[0].vertex_count == 6 && parts[0].material == 0 && parts[1].material == 1 &&
                parts[2].material == 1,
            "glTF lost primitives, node instances or material slots, or imported the BLEND primitive.");
    require(loaded.notes.size() == 1 && loaded.notes[0].find("glass") != std::string::npos &&
                loaded.notes[0].find("2 triangles") != std::string::npos,
            "glTF did not report the skipped BLEND primitive.");
    require(
        loaded.files.size() == 6 &&
            std::ranges::none_of(loaded.files, [](const auto& f) { return f.relative.filename() == L"missing.bmp"; }),
        "glTF read an image no imported material uses, or missed a dependency.");
    // Node 0 translates (1,2,3); node 1 rotates +90 degrees about Y after scaling X by 2, so
    // glTF (x,y,z) -> (z, y, -2x) + (1,2,3); the engine then mirrors X.
    require(close(vertices[0].position, {-1, 2, 3}) && close(vertices[1].position, {-1, 2, 1}) &&
                close(vertices[2].position, {-1, 3, 1}) && close(vertices[3].position, {-1, 2, 3}) &&
                close(vertices[4].position, {-1, 3, 1}) && close(vertices[5].position, {-1, 3, 3}),
            "glTF node transforms, index order or the left-handed conversion are wrong.");
    // Normal (1,0,1)/sqrt2 takes the inverse transpose: (1/2,0,1) -> rotate (2,0,-1) -> mirror -> (-2,0,-1)/sqrt5.
    // Tangent (1,0,0) takes the matrix: (0,0,-2), unchanged by the mirror, then is orthogonalized: (1,0,-2)/sqrt5.
    // The world determinant is negative, so the authored handedness +1 flips to -1.
    require(close(vertices[0].normal, {-.894427, 0, -.447214}) &&
                close(vertices[0].tangent, {.447214, 0, -.894427, -1}),
            "glTF normals or authored tangents did not follow the node transform and mirror.");
    require(close(vertices[0].uv, {.25, .75}) && close(vertices[2].uv, {1, 0}),
            "glTF texture coordinates were changed.");
    // The triangle (0,0,0),(1,0,0),(0,1,0) faces +Z in glTF. Node 2 mirrors X itself, so the engine
    // positions equal the source; node 3 is mirrored once by the conversion. Both still face +Z.
    // glTF normal maps put +Y toward -V, so the bitangent cross(N,T)*w must point to -Y here.
    require(close(vertices[6].position, {0, 0, 0}) && close(vertices[7].position, {1, 0, 0}) &&
                close(vertices[8].position, {0, 1, 0}) && close(vertices[10].position, {-1, 0, 0}) &&
                close(vertices[6].normal, {0, 0, 1}) && close(vertices[9].normal, {0, 0, 1}) &&
                close(vertices[6].tangent, {1, 0, 0, -1}) && close(vertices[9].tangent, {-1, 0, 0, 1}),
            "glTF flat normals or generated tangents ignore the front side or the normal-map convention.");
    require(loaded.materials.size() == 2 && loaded.materials[0] && loaded.materials[1] && loaded.textures.size() == 2,
            "glTF did not retain its authored material records beside geometry slots.");
    const auto& opaque = *loaded.materials[0];
    const auto& leaf = *loaded.materials[1];
    require(loaded.textures[0][1] == loaded.textures[0][2] &&
                loaded.textures[0][1] == fs::path("textures/packed.bmp") &&
                imported_material_value<float>(opaque, "roughness_channel") == 1 &&
                imported_material_value<float>(opaque, "metallic_channel") == 2 &&
                imported_material_value<float>(opaque, "roughness") == .5f &&
                imported_material_value<float>(opaque, "metallic") == 1,
            "glTF metallicRoughness did not map G to roughness and B to metallic with glTF factor defaults.");
    require(
        loaded.textures[0][3] == fs::path("textures/normal.bmp") &&
            imported_material_value<float>(opaque, "normal_scale") == .5f &&
            imported_material_value<mat::TextureBinding>(opaque, "base_color_texture").address == mat::Address::clamp &&
            imported_material_value<mat::TextureBinding>(opaque, "normal_texture").address == mat::Address::repeat &&
            close(imported_material_value<math::Vector4>(opaque, "base_color"), {1, .5, .25, 1}) &&
            close(imported_material_value<math::Vector3>(opaque, "emissive"), {.1, .2, .3}),
        "glTF base color, emission, normal scale or sampler wrap was not imported.");
    require(opaque.parent_key == "builtin/material/standard-opaque-double" &&
                leaf.parent_key == "builtin/material/standard-mask-single" &&
                std::abs(imported_material_value<float>(leaf, "alpha_cutoff") - .3f) < 1e-6f &&
                imported_material_value<float>(leaf, "metallic") == 0,
            "glTF alpha mode, cutoff or sidedness was not imported.");
    // Import keeps bytes and typed author intent, not pixel owners. Exercise the
    // same public decoder with those exact bytes and retain its explicit owners.
    const auto decoded = [&](const assets::MeshImport& imported, assets::ImageCache* images) {
        std::vector<std::string_view> bytes;
        for (const auto& path :
             {imported.textures[0][0], imported.textures[0][1], imported.textures[0][3], imported.textures[1][0]}) {
            const auto at = std::ranges::find(imported.files, path, &assets::ImportedFile::relative);
            require(at != imported.files.end(), "A typed glTF texture has no bytes in its import bundle.");
            bytes.push_back(at->bytes);
        }
        return assets::decode_images(bytes, images);
    };
    const auto pixels = decoded(loaded, &cache);
    require(pixels[1]->pixels()[1] == 90 && pixels[1]->pixels()[2] == 200,
            "glTF packed data was modified during decoding.");
    require(pixels[3] == pixels[0] && cache.entries.size() == 3,
            "Images with the same bytes did not share decoded pixels.");
    require(decoded(assets::load_gltf(source / "scene.gltf", &cache), &cache)[0] == pixels[0],
            "A second import did not reuse cached pixels.");
    const auto uncached = assets::load_gltf(source / "scene.gltf");
    require(decoded(uncached, nullptr)[3] != pixels[0], "Uncached glTF loads shared pixels through hidden state.");
    {
        const std::string pixels = bmp(1, 2, 3);
        const std::array<std::string_view, 2> two{pixels, pixels};
        bool limited = false;
        try {
            static_cast<void>(assets::decode_images(two, nullptr, 31));
        } catch (const std::runtime_error& error) {
            limited = std::string_view(error.what()).starts_with("Decoded textures exceed");
        }
        require(limited && assets::decode_images(two, nullptr, 32).size() == 2,
                "Image decoding did not hold its pixel budget.");
    }
    {
        // Float normals are not exactly unit: a tangent equal to its normal must still take the fallback.
        const float d = .70710677f;
        std::vector<atelier::geometry::Vertex> triangle(3);
        triangle[1].position[0] = 1;
        triangle[2].position[1] = 1;
        for (auto& v : triangle) {
            v.normal[0] = v.normal[2] = d;
            v.tangent[0] = v.tangent[2] = d;
            v.tangent[3] = 1;
        }
        const atelier::geometry::Mesh parallel(std::move(triangle), {}, {}, {}, atelier::geometry::Tangents::authored);
        for (const auto& v : parallel.vertices())
            require(std::abs(v.tangent[0] * v.normal[0] + v.tangent[1] * v.normal[1] + v.tangent[2] * v.normal[2]) <
                            1e-5f &&
                        std::abs(std::hypot(v.tangent[0], v.tangent[1], v.tangent[2]) - 1) < 1e-5f,
                    "An authored tangent parallel to its normal stayed parallel.");
        // A long tangent within rounding of its normal must take the fallback (+X here), not its residual (+Y).
        std::vector<atelier::geometry::Vertex> scaled(3);
        scaled[1].position[0] = 1;
        scaled[2].position[1] = 1;
        for (auto& v : scaled) {
            v.normal[2] = 1;
            v.tangent[1] = 1e-5f;
            v.tangent[2] = 1000;
            v.tangent[3] = 1;
        }
        const atelier::geometry::Mesh long_parallel(std::move(scaled), {}, {}, {},
                                                    atelier::geometry::Tangents::authored);
        for (const auto& v : long_parallel.vertices())
            require(close(v.tangent, {1, 0, 0, 1}),
                    "A long authored tangent parallel to its normal kept its rounding residual as the tangent.");
    }

    auto variant = fixture;
    variant["meshes"][1]["primitives"][0].erase("material");
    take(editor::write_file(source / "variant.gltf", variant.dump()));
    {
        const auto implicit = assets::load_gltf(source / "variant.gltf");
        const auto& fallback = implicit.mesh->materials().back();
        require(implicit.mesh->materials().size() == 2 && fallback.name == "default" &&
                    implicit.materials.size() == 2 && !implicit.materials.back() &&
                    std::ranges::all_of(implicit.textures.back(), [](const auto& path) { return path.empty(); }),
                "A primitive without a material acquired authored MI values or texture dependencies.");
        editor::Session unbound;
        assets::Reader installed;
        const auto surfaces =
            take(unbound.mesh_surfaces(installed, {"fixture/implicit", L"Implicit", implicit.mesh, {}}));
        require(surfaces->size() == 2 && surfaces->back() == take(mat::default_world_surface()) &&
                    !surfaces->back()->uses_default_material(),
                "An unbound glTF slot did not resolve to the ordinary DefaultWorldGrid Source.");
    }
    // Scenes bind by slot name, so repeated, empty and implicit-default names become unique.
    variant["materials"][0]["name"] = "default";
    take(editor::write_file(source / "variant.gltf", variant.dump()));
    {
        const auto named = assets::load_gltf(source / "variant.gltf");
        require(named.mesh->materials()[0].name == "default" && named.mesh->materials()[1].name == "default (2)",
                "The implicit default slot reused a material's name.");
    }
    variant = fixture;
    variant["materials"][0]["name"] = "leaf";
    take(editor::write_file(source / "variant.gltf", variant.dump()));
    require(assets::load_gltf(source / "variant.gltf").mesh->materials()[1].name == "leaf (2)",
            "Two glTF materials with one name shared a slot name.");
    variant["materials"][0]["name"] = "";
    take(editor::write_file(source / "variant.gltf", variant.dump()));
    require(assets::load_gltf(source / "variant.gltf").mesh->materials()[0].name == "material 0",
            "An unnamed glTF material did not get its index name.");
    const auto rejects = [&](Json document, std::string_view expected, const char* message) {
        take(editor::write_file(source / "variant.gltf", document.dump()));
        try {
            static_cast<void>(assets::load_gltf(source / "variant.gltf"));
        } catch (const std::runtime_error& error) {
            if (std::string_view(error.what()).find(expected) != std::string_view::npos)
                return;
            throw std::runtime_error(std::string(message) + " Got: " + error.what());
        }
        throw std::runtime_error(message);
    };
    variant = fixture;
    variant["extensionsRequired"] = {"KHR_draco_mesh_compression"};
    rejects(variant, "required extensions", "glTF ignored a required extension.");
    variant = fixture;
    variant["meshes"][0]["primitives"][0]["mode"] = 1;
    rejects(variant, "triangle-list", "glTF accepted a line primitive.");
    variant = fixture;
    variant["accessors"][0]["sparse"] = Json::object();
    rejects(variant, "sparse", "glTF accepted a sparse accessor.");
    variant = fixture;
    variant["accessors"][0]["count"] = 5;
    rejects(variant, "exceeds its buffer", "glTF read past a buffer view.");
    variant = fixture;
    variant["buffers"][0]["uri"] = "%2E%2E/scene.bin";
    rejects(variant, "inside the source directory", "glTF followed an encoded parent-directory uri.");
    variant = fixture;
    variant["images"][2]["uri"] = "textures/corrupt.png";
    rejects(variant, "Cannot decode texture", "A failed parallel decode was not reported.");
    variant = fixture;
    variant["images"][0]["uri"] = "textures/albedo";
    rejects(variant, "PNG, JPEG, BMP or TIFF",
            "glTF accepted an image file that the Project cannot catalogue as a texture.");

    auto project = take(editor::Project::create(evidence / "project"));
    const auto key = take(project.import_mesh(source / "scene.gltf"));
    const auto imported = evidence / "project" / "Content" / "Meshes" / "scene";
    require(fs::exists(imported / "scene.gltf") && fs::exists(imported / "scene.bin") &&
                fs::exists(imported / "textures" / "color map.bmp") &&
                !fs::exists(imported / "textures" / "missing.bmp") && project.assets().mesh_notes(key).size() == 1,
            "Import Mesh did not publish the glTF bundle with its notes.");
    auto reopened = take(editor::Project::open(project.root()));
    const auto asset = take(reopened.assets().mesh(key));
    require(asset.mesh->vertices().size() == 12 && reopened.assets().mesh_notes(key).size() == 1,
            "A reopened project did not load its glTF mesh.");

    // Import publishes one material per slot and names them as the mesh's slot defaults.
    const auto listed = [](const editor::Project& catalog, const std::string& resource) -> const assets::ProjectEntry& {
        const auto* entry = catalog.entry(resource);
        require(entry != nullptr, "A generated resource key is not catalogued.");
        return *entry;
    };
    const auto default_of = [](const assets::MeshAsset& mesh, std::string_view slot) {
        const auto at = std::ranges::find(mesh.materials, slot, &assets::SlotMaterial::slot);
        require(at != mesh.materials.end(), "A mesh slot has no default material.");
        return at->key;
    };
    const auto opaque_key = default_of(asset, "opaque"), leaf_key = default_of(asset, "leaf");
    require(asset.materials.size() == 2 &&
                listed(project, opaque_key).path == fs::path("Content/Meshes/scene/Materials/opaque.material") &&
                listed(project, leaf_key).path == fs::path("Content/Meshes/scene/Materials/leaf.material"),
            "Import Mesh did not publish one material per slot as the mesh's defaults.");
    {
        const auto opaque_asset = take(project.assets().material(opaque_key)),
                   leaf_asset = take(project.assets().material(leaf_key));
        const auto& opaque = std::get<assets::MaterialInstanceAsset>(opaque_asset.author);
        const auto& leaf = std::get<assets::MaterialInstanceAsset>(leaf_asset.author);
        const auto file = [&](const std::string& texture) { return listed(project, texture).path.filename(); };
        const auto texture = [](const assets::MaterialInstanceAsset& m,
                                std::string_view input) -> const mat::TextureBinding& {
            return imported_material_value<mat::TextureBinding>(m, std::string(input) + "_texture");
        };
        require(texture(opaque, "roughness").asset_key == texture(opaque, "metallic").asset_key &&
                    file(texture(opaque, "roughness").asset_key) == L"packed.bmp" &&
                    imported_material_value<float>(opaque, "roughness_channel") == 1 &&
                    imported_material_value<float>(opaque, "metallic_channel") == 2 &&
                    imported_material_value<float>(opaque, "roughness") == .5f &&
                    imported_material_value<float>(opaque, "metallic") == 1 &&
                    file(texture(opaque, "base_color").asset_key) == L"color map.bmp" &&
                    texture(opaque, "base_color").address == mat::Address::clamp &&
                    file(texture(opaque, "normal").asset_key) == L"normal.bmp" &&
                    imported_material_value<float>(opaque, "normal_scale") == .5f &&
                    std::ranges::find(opaque.entries, std::string("emissive_texture"), &mat::Override::path) ==
                        opaque.entries.end() &&
                    leaf.parent_key == "builtin/material/standard-mask-single" &&
                    std::abs(imported_material_value<float>(leaf, "alpha_cutoff") - .3f) < 1e-6f &&
                    file(texture(leaf, "base_color").asset_key) == L"color copy.bmp",
                "Generated materials lost the glTF channels, factors, samplers or their own texture files.");
        editor::Session resolved;
        const auto opaque_root = take(resolved.material_resource(reopened.assets(), opaque_key));
        const auto leaf_root = take(resolved.material_resource(reopened.assets(), leaf_key));
        const auto p = take(mat::capture_program(resolved.world().runtime(), opaque_root.id()));
        const auto l = take(mat::capture_program(resolved.world().runtime(), leaf_root.id()));
        const auto& packed = material_input(*p, "roughness_texture");
        const auto& base = material_input(*p, "base_color_texture");
        require(p->diagnostics().empty() && l->diagnostics().empty() && !p->uses_default_material() &&
                    !l->uses_default_material() && packed.image &&
                    packed.image == material_input(*p, "metallic_texture").image && packed.image->pixels()[1] == 90 &&
                    packed.image->pixels()[2] == 200 && packed.color == mat::TextureColor::linear &&
                    base.color == mat::TextureColor::srgb && base.address == mat::Address::clamp &&
                    material_input(*p, "normal_texture").address == mat::Address::repeat &&
                    p->policy().coverage == mat::Coverage::opaque && p->policy().double_sided &&
                    p->policy().emission_two_sided && l->policy().coverage == mat::Coverage::mask &&
                    !l->policy().double_sided && !l->policy().emission_two_sided,
                "Generated MI resolution lost packed channels, input transfer/addressing or MS policy.");
        const auto shaded = take(p->evaluate({}));
        const auto linear = [](float value) {
            return value <= .04045f ? value / 12.92f : std::pow((value + .055f) / 1.055f, 2.4f);
        };
        require(std::abs(shaded.roughness - .5f * 90.f / 255) < 1e-6f &&
                    std::abs(shaded.metallic - 200.f / 255) < 1e-6f &&
                    close(shaded.base_color, {1, .5f * linear(128.f / 255), .25f * linear(64.f / 255)}) &&
                    close(shaded.emissive, {.1, .2, .3}) && std::abs(take(l->evaluate({})).coverage - .7f) < 1e-6f,
                "Generated glTF MI did not execute StandardPBR with its actual factors and texture channels.");
        // Pixel sharing belongs to resolved material/texture owners, never Mesh.
        require(base.image && material_input(*l, "base_color_texture").image == base.image &&
                    take(reopened.assets().texture(texture(opaque, "base_color").asset_key)).image == base.image &&
                    take(reopened.assets().texture(texture(leaf, "base_color").asset_key)).image == base.image &&
                    take(project.assets().texture(texture(opaque, "base_color").asset_key)).image ==
                        take(project.assets().texture(texture(leaf, "base_color").asset_key)).image,
                "Identical imported texture bytes did not share pixels in the initial or reopened Project.");
        require(assets::material_format::encode(take(reopened.assets().material(opaque_key))) ==
                        assets::material_format::encode(opaque_asset) &&
                    assets::material_format::encode(take(reopened.assets().material(leaf_key))) ==
                        assets::material_format::encode(leaf_asset),
                "Reopening changed generated material identities or typed author content.");
    }
    // Generated file names stay valid and unique on a case-insensitive file system, beyond ASCII as well.
    variant = fixture;
    variant["materials"][0]["name"] = "Äußen/Top";
    variant["materials"][2]["name"] = "äußen_top";
    take(editor::write_file(source / "names.gltf", variant.dump()));
    variant["materials"][0]["name"] = "CON.foo";
    variant["materials"][2]["name"] = "COM1 .x";
    take(editor::write_file(source / "reserved.gltf", variant.dump()));
    variant["materials"][0]["name"] = "CONOUT$";
    variant["materials"][2]["name"] = "lpt¹";
    take(editor::write_file(source / "devices.gltf", variant.dump()));
    {
        const auto named = take(project.assets().mesh(take(project.import_mesh(source / "names.gltf"))));
        const auto reserved = take(project.assets().mesh(take(project.import_mesh(source / "reserved.gltf"))));
        const auto devices = take(project.assets().mesh(take(project.import_mesh(source / "devices.gltf"))));
        require(listed(project, default_of(named, "Äußen/Top")).path.filename() == L"Äußen_Top.material" &&
                    listed(project, default_of(named, "äußen_top")).path.filename() == L"äußen_top (2).material" &&
                    listed(project, default_of(reserved, "CON.foo")).path.filename() == L"_CON.foo.material" &&
                    listed(project, default_of(reserved, "COM1 .x")).path.filename() == L"_COM1 .x.material" &&
                    listed(project, default_of(devices, "CONOUT$")).path.filename() == L"_CONOUT$.material" &&
                    listed(project, default_of(devices, "lpt¹")).path.filename() == L"_lpt¹.material",
                "Generated material file names are invalid or collide by case.");
    }
    // A write that fails after earlier files were published removes every file the import wrote.
    take(editor::write_file(source / "blocked.gltf", fixture.dump()));
    const auto blocked = project.root() / "Content" / "Meshes" / "blocked";
    take(editor::write_file(blocked / "Materials", "not a folder"));
    fs::create_directory(blocked / "textures");
    take(project.refresh());
    const auto before_revision = project.revision();
    const auto before_mesh = take(project.assets().mesh(key)).mesh;
    const std::vector<assets::ProjectEntry> before_entries(project.entries().begin(), project.entries().end());
    const auto before_file = take(assets::read_file(imported / "scene.gltf"));
    const auto rejected = project.import_mesh(source / "blocked.gltf");
    require(!rejected && rejected.error().message.starts_with("create_directories:") &&
                rejected.error().message.find("Materials") != std::string::npos &&
                take(assets::read_file(blocked / "Materials")) == "not a folder" &&
                !fs::exists(blocked / "blocked.gltf") && project.revision() == before_revision &&
                std::ranges::equal(project.entries(), before_entries) &&
                take(project.assets().mesh(key)).mesh == before_mesh &&
                take(assets::read_file(imported / "scene.gltf")) == before_file &&
                std::ranges::none_of(
                    fs::recursive_directory_iterator(blocked),
                    [](const auto& item) { return item.is_regular_file() && item.path().filename() != L"Materials"; }),
            "A failed glTF import changed the accepted catalog/cache/files or left published files behind.");
    fs::remove(blocked / "Materials");
    require(take(project.assets().mesh(take(project.import_mesh(source / "blocked.gltf")))).mesh != nullptr &&
                fs::is_directory(blocked / "Materials"),
            "A glTF import could not retry after removing its write obstruction.");
    std::printf("Import write rollback: PASS (catalog/cache/files preserved; retry); failure=%s\n",
                rejected.error().message.c_str());

    // Mesh defaults are resolved at runtime and never written to scenes.
    const auto placed = take(project.assets().mesh(key));
    editor::Session document;
    auto& runtime = document.world().runtime();
    require(placed.materials.size() == 2 && placed.materials[0].slot == "opaque" && placed.materials[1].slot == "leaf",
            "Mesh defaults are not listed in the mesh's slot order.");
    changed(document.create_static_mesh(placed, {}, &project));
    const auto part =
        component(document, document.selection().entity, document.presentation_types().static_mesh_component);
    const auto surfaces = [](editor::Session& session, ObjectId object) {
        return *take(presentation::material_surfaces(session.world().runtime(), object));
    };
    const auto index = [&](std::string_view slot) {
        for (std::size_t i = 0; i < placed.mesh->materials().size(); ++i)
            if (presentation::material_slot_name(*placed.mesh, i) == slot)
                return i;
        throw std::runtime_error("Missing glTF slot.");
    };
    const auto opaque_slot = index("opaque"), leaf_slot = index("leaf");
    const auto opaque_root = take(document.material_resource(project.assets(), opaque_key));
    const auto captured = [](editor::Session& session, const ObjectRoot& root) {
        return take(material::capture_program(session.world().runtime(), root.id()));
    };
    require(take(presentation::read_material_bindings(runtime, part)).empty() &&
                surfaces(document, part)[opaque_slot] == captured(document, opaque_root) &&
                resolved_material_value<float>(*surfaces(document, part)[opaque_slot], "roughness") == .5f &&
                resolved_material_value<float>(*surfaces(document, part)[opaque_slot], "metallic") == 1,
            "A placed glTF mesh did not show its default materials, or the component's base parameters replaced them.");
    require(*take(document.mesh_surfaces(project.assets(), placed)) == surfaces(document, part),
            "Mesh surfaces without a component differ from a placed mesh's defaults.");
    const auto edit_roughness = [&](ObjectId material, float value, std::uint64_t control) {
        take(document.begin_material_edit(material, control));
        take(document.preview_material_entry(control, {"roughness", value}));
        take(document.commit_material_edit(control));
    };
    // The extraction cache, not only a fresh capture, must see edits to a default material.
    const auto drawn = [&] {
        const auto items = take(document.presentation().render_items());
        require(items.size() == 1, "The placed glTF mesh is not drawn.");
        const auto& surface = (*items.front().surfaces)[opaque_slot];
        const auto factor = resolved_material_value<float>(*surface, "roughness");
        require(std::abs(take(surface->evaluate({})).roughness - factor * 90.f / 255) < 1e-6f,
                "The extracted glTF material no longer executes its G-channel roughness input.");
        return factor;
    };
    require(drawn() == .5f, "The drawn mesh does not show its default material.");
    edit_roughness(opaque_root.id(), .2f, 301);
    require(resolved_material_value<float>(*surfaces(document, part)[opaque_slot], "roughness") == .2f &&
                drawn() == .2f,
            "Editing a default material did not reach the mesh that shows it.");
    take(document.material_undo(opaque_key));
    require(drawn() == .5f, "Undoing a default material edit did not reach the drawn mesh.");
    const auto saved = take(document.serialize());
    const auto scene = nlohmann::json::parse(saved);
    require(scene["materials"].empty() && scene["entities"][0]["components"][0]["materials"].empty(),
            "Saving wrote mesh default materials into the scene.");
    {
        auto loaded = take(editor::Session::deserialize(saved, project, {}, editor::LoadPolicy::strict));
        const auto copy = component(*loaded, take(loaded->world().members()).front().id(),
                                    loaded->presentation_types().static_mesh_component);
        require(surfaces(*loaded, copy)[opaque_slot] ==
                        captured(*loaded, take(loaded->material_resource(project.assets(), opaque_key))) &&
                    resolved_material_value<float>(*surfaces(*loaded, copy)[opaque_slot], "roughness") == .5f &&
                    take(loaded->serialize()) == saved,
                "A reopened scene did not resolve its mesh defaults or changed its saved bindings.");
    }
    // Overriding a defaulted slot binds one instance of the default; Undo, Redo and Clear restore the default.
    const auto history = document.undo_count();
    const auto instance = take(document.create_scene_material(part, "opaque"));
    const auto bound = take(presentation::read_material_bindings(runtime, part));
    require(document.undo_count() == history + 1 && bound.size() == 1 && bound[0].slot == "opaque" &&
                bound[0].overridden && bound[0].resource.id() == instance.id() &&
                take(material::read_instance(runtime, instance.id())).entries.empty() &&
                take(material::read_instance(runtime, instance.id())).parent.id() == opaque_root.id(),
            "Overriding a defaulted slot did not bind one instance of its default material in one history step.");
    edit_roughness(instance.id(), .9f, 302);
    require(resolved_material_value<float>(*surfaces(document, part)[opaque_slot], "roughness") == .9f &&
                drawn() == .9f &&
                resolved_material_value<float>(*captured(document, opaque_root), "roughness") == .5f &&
                take(material::read_instance(runtime, instance.id())).entries.size() == 1,
            "An instance override did not stay on its component.");
    {
        const auto overridden = take(document.serialize());
        auto loaded = take(editor::Session::deserialize(overridden, project, {}, editor::LoadPolicy::strict));
        const auto copy = component(*loaded, take(loaded->world().members()).front().id(),
                                    loaded->presentation_types().static_mesh_component);
        require(take(loaded->serialize()) == overridden &&
                    resolved_material_value<float>(*surfaces(*loaded, copy)[opaque_slot], "roughness") == .9f,
                "Sparse scene MI overrides did not survive Scene save/reopen.");
    }
    changed(document.undo());
    changed(document.undo());
    require(take(presentation::read_material_bindings(runtime, part)).empty() &&
                surfaces(document, part)[opaque_slot] == captured(document, opaque_root),
            "Undoing the override did not return the slot to its mesh default.");
    changed(document.redo());
    take(document.clear_material(part, "opaque"));
    require(take(presentation::read_material_bindings(runtime, part)).empty() &&
                surfaces(document, part)[opaque_slot] == captured(document, opaque_root),
            "Use Mesh Default did not return the slot to its default material.");
    // A sidecar-only change reaches the next load; the cached geometry stays.
    take(project.refresh()); // Only the sidecar changes below.
    auto sidecar = imported / "scene.gltf";
    sidecar += L".asset.json";
    const auto sidecar_text = take(assets::read_file(sidecar));
    auto mapping = nlohmann::json::parse(sidecar_text);
    mapping["materials"]["leaf"] = opaque_key;
    take(editor::write_file(sidecar, mapping.dump(2)));
    const auto catalog = project.revision();
    take(project.refresh());
    const auto remapped = take(project.assets().mesh(key));
    require(project.revision() > catalog && default_of(remapped, "leaf") == opaque_key &&
                remapped.mesh == placed.mesh &&
                take(assets::mesh_content_sha256(remapped)) != take(assets::mesh_content_sha256(placed)),
            "A changed mesh sidecar was not picked up, or it discarded the cached geometry.");
    {
        auto loaded = take(editor::Session::deserialize(saved, project));
        const auto copy = component(*loaded, take(loaded->world().members()).front().id(),
                                    loaded->presentation_types().static_mesh_component);
        require(surfaces(*loaded, copy)[leaf_slot] ==
                    captured(*loaded, take(loaded->material_resource(project.assets(), opaque_key))),
                "A remapped slot default did not load.");
    }
    mapping["materials"]["missing slot"] = opaque_key;
    take(editor::write_file(sidecar, mapping.dump(2)));
    take(project.refresh());
    require(!project.assets().mesh(key), "Mesh metadata naming an absent slot was accepted.");
    take(editor::write_file(sidecar, sidecar_text));
    // An unavailable default material stands in with a warning; the scene still opens.
    const auto leaf_file = project.root() / listed(project, leaf_key).path;
    fs::rename(leaf_file, evidence / "leaf.material");
    take(project.refresh());
    {
        auto loaded = take(editor::Session::deserialize(saved, project));
        const auto& issues = loaded->load_issues();
        require(issues.size() == 1 && issues[0].kind == world::DiagnosticKind::missing_resource &&
                    issues[0].field == std::vector<std::string>{"mesh"} &&
                    loaded->unavailable(assets::AssetKind::material, leaf_key),
                "A missing default material was not reported as a stand-in.");
    }
    fs::rename(evidence / "leaf.material", leaf_file);
    std::puts(
        "glTF import acceptance: PASS (node transforms, left-handed conversion, authored and generated tangents, PBR channels, MASK, BLEND notes, shared images, rejections, unique slot names, generated materials, rollback, runtime mesh defaults)");
}
