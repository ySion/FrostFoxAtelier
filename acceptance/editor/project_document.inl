void verify_project_document() {
    namespace fs = std::filesystem;
    {
        assets::Reader engine;
        for (const auto* key : {"builtin/cube", "builtin/sphere", "builtin/plane"}) {
            const auto mesh = take(engine.mesh(key));
            require(mesh.key == key && mesh.mesh && !mesh.mesh->vertices().empty() &&
                        mesh.mesh == take(assets::Reader{}.mesh(key)).mesh,
                    "Engine-only readers lost built-in identity or shared content.");
            const auto dependency = take(engine.engine_dependency(key, assets::AssetKind::mesh));
            require(dependency.key == key && dependency.kind == assets::AssetKind::mesh &&
                        dependency.semantic_version == 1 && dependency.digest_version == 2 &&
                        dependency.digest_version == assets::mesh_digest_version &&
                        dependency.content_sha256.size() == 64 &&
                        dependency.content_sha256 == take(assets::mesh_content_sha256(mesh)),
                    "Engine dependency did not describe its actual published mesh.");
        }
        require(engine.entries().empty() && engine.revision() == 0 && !engine.mesh("builtin/unknown") &&
                    !engine.texture("builtin/cube") && !engine.material("builtin/cube") &&
                    !engine.sky_map("builtin/cube"),
                "Engine-only reads invented a project catalogue or resolved an unknown/wrong-kind asset.");
        require(!engine.engine_dependency("builtin/unknown", assets::AssetKind::mesh) &&
                    !engine.engine_dependency("builtin/cube", assets::AssetKind::texture) &&
                    !engine.engine_dependency("project/fake", assets::AssetKind::mesh),
                "An engine dependency trusted a prefix or a wrong kind.");
        // v2 hashes geometry, slot relationships and default keys, not MI author values.
        const auto shape = [](std::vector<std::uint8_t> edges, unsigned change = 0) {
            std::vector<geometry::Vertex> vertices{
                {{0, 0, 0}, {0, 0, 1}, {0, 0}, {1, 0, 0, 1}}, {{2, 0, 0}, {0, 0, 1}, {1, 0}, {1, 0, 0, 1}},
                {{0, 3, 0}, {0, 0, 1}, {0, 1}, {1, 0, 0, 1}}, {{0, 0, 1}, {0, 0, 1}, {0, 0}, {1, 0, 0, 1}},
                {{4, 0, 1}, {0, 0, 1}, {1, 0}, {1, 0, 0, 1}}, {{0, 5, 1}, {0, 0, 1}, {0, 1}, {1, 0, 0, 1}}};
            if (change == 1)
                vertices[1].position[0] = 2.5f;
            return std::make_shared<const geometry::Mesh>(
                std::move(vertices), std::move(edges),
                std::vector<geometry::MaterialSlot>{{"paint"}, {change == 3 ? "edge" : "trim"}},
                change == 2 ? std::vector<geometry::MeshPart>{{0, 3, 1}, {3, 3, 0}}
                            : std::vector<geometry::MeshPart>{{0, 3, 0}, {3, 3, 1}},
                geometry::Tangents::authored);
        };
        assets::MeshAsset content{"first", L"First", shape({}), {{"paint", "project/A"}, {"trim", "project/B"}}};
        const auto digest = take(assets::mesh_content_sha256(content));
        auto other = content;
        other.key = "second";
        other.name = L"Second";
        other.mesh = shape({7, 7});
        std::ranges::reverse(other.materials);
        require(take(assets::mesh_content_sha256(other)) == digest,
                "Mesh digest depends on identity, implicit edges or binding storage order.");
        std::swap(other.materials[0].key, other.materials[1].key);
        require(take(assets::mesh_content_sha256(other)) != digest,
                "Mesh digest lost the slot-to-material relationship.");
        other = content;
        other.materials.push_back({"paint", "project/C"});
        require(!assets::mesh_content_sha256(other), "Mesh digest silently overwrote duplicate slot defaults.");
        for (unsigned change = 0; change < 4; ++change) {
            other = content;
            other.mesh = shape(change == 0 ? std::vector<std::uint8_t>{5, 3} : std::vector<std::uint8_t>{}, change);
            if (change == 3)
                other.materials[1].slot = "edge";
            require(take(assets::mesh_content_sha256(other)) != digest,
                    "Mesh digest omitted geometry, visible edges, slot names or triangle-to-slot relationships.");
        }
        // Material policy and Source content have their own installed dependency digest.
        const auto opaque =
            take(engine.engine_dependency("builtin/material/standard-opaque-double", assets::AssetKind::material));
        const auto masked =
            take(engine.engine_dependency("builtin/material/standard-mask-double", assets::AssetKind::material));
        require(opaque.digest_version == assets::material_digest_version && opaque.content_sha256.size() == 64 &&
                    opaque == take(assets::Reader{}.engine_dependency(opaque.key, assets::AssetKind::material)) &&
                    masked.content_sha256.size() == 64 && masked.content_sha256 != opaque.content_sha256 &&
                    !engine.engine_dependency(opaque.key, assets::AssetKind::mesh),
                "Material dependency lost its separate content digest or kind.");
        assets::MaterialAsset material;
        material.author = assets::MaterialInstanceAsset{
            opaque.key, {{"base_color", math::Vector4{1, 1, 1, 1}}, {"opacity_uv.offset", math::Vector2{}, false}}};
        const auto author = assets::material_format::encode(material);
        for (unsigned change = 0; change < 2; ++change) {
            auto edited = material;
            auto& entries = std::get<assets::MaterialInstanceAsset>(edited.author).entries;
            if (change == 0)
                std::get<math::Vector4>(entries[0].value).w = .375f;
            else
                std::get<math::Vector2>(entries[1].value).y = .25f;
            const auto encoded = assets::material_format::encode(edited);
            require(encoded != author &&
                        assets::material_format::encode(assets::material_format::decode(encoded)) == encoded &&
                        take(assets::mesh_content_sha256(content)) == digest,
                    "MI alpha or dormant sampling was lost or leaked into mesh content.");
        }
    }
    const auto evidence = fs::current_path() / "project-document-evidence" /
                          std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    fs::create_directories(evidence);
    const auto folder = evidence / L"工程";
    auto project = take(editor::Project::create(folder));
    for (const auto* directory : {L"Config", L"Content", L"Source", L"Cache"})
        require(fs::is_directory(folder / directory), "Project did not create its four real folders.");
    require(!editor::Project::create(folder), "Create Project overwrote an existing folder.");
    require(!project.resolve("../outside.scene"), "Project accepted a path outside its root.");
    const auto external = evidence / "Triangle.obj";
    take(editor::write_file(external, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"));
    const auto resource = take(project.import_mesh(external));
    const auto original_mesh = take(project.assets().mesh(resource)).mesh;
    const math::Vector3 collision_scale{2, 3, 4};
    const auto collision_file = take(project.cook_collision(resource, collision_scale));
    const auto collision_bytes = take(project.collision(resource, collision_scale));
    require(fs::is_regular_file(collision_file) && !collision_bytes->empty(),
            "Imported mesh collision was not cooked into the project cache.");
    take(editor::write_file(collision_file, "broken"));
    require(*take(project.collision(resource, collision_scale)) == *collision_bytes &&
                take(assets::read_file(collision_file)) == *collision_bytes,
            "A corrupt collision cache was not rebuilt from its source mesh and written back.");
    require(*take(project.collision(resource, {1, 1, 1})) != *collision_bytes,
            "Collision cache served a different scale.");
    {
        // Asset rename keeps the stable resource key (its sidecar moves with it), refuses
        // collisions, bad names and configured paths, and leaves the original on failure.
        const auto entry = std::ranges::find(project.entries(), resource, &assets::ProjectEntry::key);
        require(entry != project.entries().end(), "Imported mesh is missing from the catalog.");
        const auto before = entry->path;
        const auto renamed = take(project.rename(before, L"Renamed Triangle.obj"));
        const auto after = std::ranges::find(project.entries(), resource, &assets::ProjectEntry::key);
        require(after != project.entries().end() && after->path == renamed &&
                    renamed.filename() == L"Renamed Triangle.obj" && !fs::exists(folder / before) &&
                    fs::exists(folder / renamed) && fs::exists(fs::path(folder / renamed) += L".asset.json"),
                "Rename lost the resource identity or its sidecar.");
        require(take(project.assets().mesh(resource)).mesh != nullptr, "A renamed mesh no longer resolves by its key.");
        require(!project.rename(renamed, L"bad/name.obj") && !project.rename(renamed, L"trailing.") &&
                    !project.rename("Content", L"Other"),
                "Rename accepted an invalid name or a top-level folder.");
        const auto taken = folder / renamed.parent_path() / "Taken.obj";
        take(editor::write_file(taken, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"));
        take(project.refresh());
        require(!project.rename(renamed, L"Taken.obj") && fs::exists(folder / renamed),
                "Rename overwrote another asset.");
        require(!project.rename(project.startup_scene(), L"Other.scene"), "Rename moved the configured startup scene.");
        take(project.rename(renamed, before.filename().wstring()));
        fs::remove(taken);
        fs::remove(fs::path(taken) += L".asset.json");
        take(project.refresh());
    }
    const auto revision = project.revision();
    take(project.refresh());
    take(project.refresh());
    require(project.revision() == revision && take(project.assets().mesh(resource)).mesh == original_mesh,
            "An unchanged refresh changed catalog identity or discarded a live mesh.");
    verify_project_workspace(project);
    require(!project.import_mesh(external), "Import silently overwrote an existing asset.");
    editor::Session document;
    changed(document.create_static_mesh(take(project.assets().mesh(resource))));
    const auto entity = document.selection().entity;
    take(document.rename(entity, L"主角 Triangle"));
    const auto mesh = component(document, entity, document.presentation_types().static_mesh_component);
    take(const_cast<world::World&>(document.world()).set_transform(entity, {{1, 2, 3}, {0, 0, 0, 1}, {2, 3, 4}}));
    changed(document.add_component(document.types().component));
    changed(document.add_component(document.presentation_types().static_mesh_component)); // Null mesh must survive.
    const auto saved = take(document.serialize());
    take(editor::write_file(take(project.resolve(project.startup_scene())), saved));
    document.mark_saved();
    take(document.rename(entity, L"Changed"));
    require(document.dirty(), "Rename did not dirty the document.");
    changed(document.undo());
    require(!document.dirty(), "Undo to the saved history position remained dirty.");
    auto loaded = take(editor::Session::deserialize(saved, project));
    require(take(loaded->serialize()) == saved,
            "Scene roundtrip lost identity relations, names, transforms or null meshes.");
    {
        editor::Session built_in;
        changed(built_in.create_static_mesh(take(project.assets().mesh("builtin/sphere"))));
        const auto built_in_entity = built_in.selection().entity;
        const auto body = component(built_in, built_in_entity, built_in.presentation_types().static_mesh_component);
        physics::BodySettings settings;
        settings.shape = physics::ShapeKind::triangle_mesh;
        take(built_in.configure_body(body, settings));
        auto reopened = take(editor::Session::deserialize(take(built_in.serialize()), project));
        const auto restored_body = component(*reopened, take(reopened->world().members()).front().id(),
                                             reopened->presentation_types().static_mesh_component);
        const auto restored_settings = take(physics::read_settings(reopened->world().runtime(), restored_body));
        require(restored_settings.shape == physics::ShapeKind::triangle_mesh && !restored_settings.cooked_shape,
                "Built-in triangle collision required a project Cook cache after reopening.");
        auto& simulation = const_cast<world::World&>(reopened->world());
        auto physics_scene =
            take(physics::PhysicsScene::create(simulation, reopened->physics_types(), presentation::read_mesh_source));
        changed(simulation.start());
        take(simulation.update(1.f / 60.f));
        require(physics_scene->body_count() == 1, "Reopened built-in triangle collision did not create a body.");
        changed(simulation.stop());
    }
    {
        editor::Session physical;
        changed(physical.create_static_mesh(take(project.assets().mesh(resource))));
        const auto physical_entity = physical.selection().entity;
        take(const_cast<world::World&>(physical.world())
                 .set_transform(physical_entity, {{0, 0, 0}, {0, 0, 0, 1}, collision_scale}));
        const auto body = component(physical, physical_entity, physical.presentation_types().static_mesh_component);
        physics::BodySettings settings;
        settings.shape = physics::ShapeKind::triangle_mesh;
        settings.material.friction = .65f;
        take(physical.configure_body(body, settings));
        changed(physical.undo());
        require(take(physics::read_settings(physical.world().runtime(), body)).shape == physics::ShapeKind::box,
                "Physics configuration undo did not restore the previous body.");
        changed(physical.redo());
        const auto physical_scene = take(physical.serialize());
        {
            editor::Session imported;
            changed(imported.add_entity());
            const auto parent = imported.selection().entity;
            take(imported.set_transform(parent, {{10, 0, 0}, {}, {2, 2, 2}}));
            const math::Vector3 final_scale{4, 6, 8}; // Not cooked yet: the import cooks it.
            changed(imported.instantiate_subtree(physical_scene, project, parent));
            const auto child = imported.selection().entity;
            const auto primitive = component(imported, child, imported.presentation_types().static_mesh_component);
            require(
                take(world::parent_entity(imported.world().runtime(), child)) == parent &&
                    *take(physics::read_settings(imported.world().runtime(), primitive)).cooked_shape ==
                        *take(project.collision(resource, final_scale)),
                "Composition import restored triangle Cook with Detached scale instead of its prospective parent space.");
            changed(imported.start());
            take(imported.update(1.f / 60.f));
            require(imported.physics_scene()->body_count() == 1,
                    "Prospective Cook data did not create an actual triangle body.");
            changed(imported.stop());
            changed(imported.undo());
            require(take(imported.world().members()).size() == 1,
                    "Imported physical subtree Undo left published children.");
            changed(imported.redo());
            require(imported.selection().entity == child, "Imported physical subtree Redo changed identity.");
        }
        auto restored_physics = take(editor::Session::deserialize(physical_scene, project));
        require(take(restored_physics->serialize()) == physical_scene,
                "Physics body settings changed across scene save and reopen.");
        auto& simulation = const_cast<world::World&>(restored_physics->world());
        auto physics_scene = take(physics::PhysicsScene::create(simulation, restored_physics->physics_types(),
                                                                presentation::read_mesh_source));
        changed(simulation.start());
        take(simulation.update(1.f / 60.f));
        require(physics_scene->body_count() == 1, "Reopened collision asset did not create a World body.");
        changed(simulation.stop());
        physics_scene.reset();
        take(editor::write_file(collision_file, "invalid"));
        auto rebuilt = take(editor::Session::deserialize(physical_scene, project, {}, editor::LoadPolicy::strict));
        const auto rebuilt_body = component(*rebuilt, take(rebuilt->world().members()).front().id(),
                                            rebuilt->presentation_types().static_mesh_component);
        require(*take(physics::read_settings(rebuilt->world().runtime(), rebuilt_body)).cooked_shape ==
                        *collision_bytes &&
                    take(assets::read_file(collision_file)) == *collision_bytes,
                "A corrupt Cook cache rejected the scene instead of being rebuilt from its source mesh.");
    }
    require(!editor::Session::deserialize("{\"version\":999,\"entities\":[]}", project),
            "Unknown scene version was accepted.");
    require(!editor::Session::deserialize("{broken", project), "Broken scene was accepted.");
    const auto serialized = nlohmann::json::parse(saved);
    for (const auto& value : {nlohmann::json(1.5), nlohmann::json(-1), nlohmann::json(true)}) {
        auto invalid = serialized;
        invalid["entities"][0]["components"][0]["id"] = value;
        require(!editor::Session::deserialize(invalid.dump(), project), "Scene coerced a non-string local identity.");
        invalid = serialized;
        invalid["roots"][0] = value;
        require(!editor::Session::deserialize(invalid.dump(), project), "Scene coerced a non-string root identity.");
        invalid = serialized;
        invalid["entities"][0]["children"].push_back(value);
        require(!editor::Session::deserialize(invalid.dump(), project), "Scene coerced a non-string child identity.");
    }
    // Value-level damage is exact-load failure; interactive loading keeps the saved value instead.
    const auto kept_on_save = [&](const nlohmann::json& invalid, const nlohmann::json::json_pointer& at,
                                  const char* message) {
        require(!editor::Session::deserialize(invalid.dump(), project, {}, editor::LoadPolicy::strict), message);
        auto kept = take(editor::Session::deserialize(invalid.dump(), project));
        require(kept->load_issues().size() == 1 &&
                    kept->load_issues().front().kind == world::DiagnosticKind::mismatch &&
                    nlohmann::json::parse(take(kept->serialize()))[at] == invalid[at],
                "Tolerant scene loading did not keep a mismatched saved value.");
    };
    for (const auto field : {"position", "scale", "rotation"}) {
        auto invalid = serialized;
        const auto at = nlohmann::json::json_pointer("/entities/0/space/" + std::string(field));
        invalid[at].push_back(999);
        kept_on_save(invalid, at, "Strict scene loading silently truncated a vector.");
    }
    auto invalid = serialized;
    invalid["entities"][0]["name"] = "   ";
    kept_on_save(invalid, nlohmann::json::json_pointer("/entities/0/name"),
                 "Strict scene loading accepted a name rejected by editing.");
    require(!editor::Session::deserialize("{\"version\":999,\"version\":1,\"entities\":[]}", project),
            "Scene accepted duplicate JSON fields.");
    require(!editor::Session::deserialize(std::string(64, '[') + std::string(64, ']'), project),
            "Scene accepted excessive JSON nesting.");
    require(!editor::Session::deserialize(std::string(16 * 1024 * 1024 + 1, ' '), project),
            "Scene direct input bypassed the file size bound.");
    auto bounded = serialized;
    auto& bounded_components = bounded["entities"][0]["components"];
    while (bounded_components.size() < editor::Session::maximum_components) {
        auto entry = bounded_components[1];
        entry["id"] = std::to_string(bounded_components.size() + 10);
        bounded_components.push_back(std::move(entry));
    }
    bounded["next_id"] = std::to_string(editor::Session::maximum_components + 100);
    auto full = take(editor::Session::deserialize(bounded.dump(), project));
    const auto full_entity = take(full->world().members()).front().id();
    take(full->select(full_entity));
    require(!full->add_component(full->types().component) &&
                take(world::components(full->world().runtime(), full_entity)).size() ==
                    editor::Session::maximum_components,
            "Editor admitted a component beyond its scene format limit.");
    full.reset();
    require(!editor::write_file(folder / "Content/Scenes/Main.scene", "overwrite", editor::WriteMode::create) &&
                take(assets::read_file(folder / "Content/Scenes/Main.scene")) == saved,
            "Create-only file publication overwrote an existing scene.");
    const auto metadata = folder / "Content/Meshes/Triangle.obj.asset.json";
    const auto metadata_text = take(assets::read_file(metadata));
    take(editor::write_file(metadata, "{broken"));
    const auto retry = evidence / "Retry.obj";
    take(editor::write_file(retry, take(assets::read_file(external))));
    require(!project.import_mesh(retry) && !fs::exists(folder / "Content/Meshes/Retry.obj") &&
                !fs::exists(folder / "Content/Meshes/Retry.obj.asset.json"),
            "Failed import left a target that prevented retry.");
    require(take(project.assets().mesh(resource)).mesh == original_mesh,
            "Failed refresh destroyed the previously accepted resource cache.");
    take(editor::write_file(metadata, metadata_text));
    const auto retried = take(project.import_mesh(retry));
    require(take(project.assets().mesh(retried)).mesh != nullptr, "Import could not recover after catalog repair.");
    take(editor::write_file(folder / "Content/Meshes/Orphan.obj.asset.json", metadata_text));
    const auto orphan = evidence / "Orphan.obj";
    take(editor::write_file(orphan, take(assets::read_file(external))));
    require(!project.import_mesh(orphan) && !fs::exists(folder / "Content/Meshes/Orphan.obj") &&
                take(assets::read_file(folder / "Content/Meshes/Orphan.obj.asset.json")) == metadata_text,
            "Import claimed or replaced orphan resource metadata.");
    {
        // The orphan is absent from the catalogue. The transaction now protects its
        // identity file during preflight, before publishing the BMP or any other file.
        const auto source = evidence / "refresh-source", target = folder / "Content/Meshes/Refresh";
        std::string bmp(70, '\0');
        bmp[0] = 'B';
        bmp[1] = 'M';
        bmp[2] = 70;
        bmp[10] = 54;
        bmp[14] = 40;
        bmp[18] = 2;
        bmp[22] = 2;
        bmp[26] = 1;
        bmp[28] = 32;
        for (std::size_t i = 54; i < bmp.size(); i += 4) {
            bmp[i + 2] = char(255);
            bmp[i + 3] = char(255);
        }
        take(editor::write_file(source / "color.bmp", bmp));
        take(editor::write_file(source / "scene.mtl", "newmtl red\nmap_Kd color.bmp\n"));
        take(editor::write_file(
            source / "Refresh.obj",
            "mtllib scene.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nusemtl red\nf 1/1 2/2 3/3\n"));
        auto invalid_metadata = nlohmann::json::parse(metadata_text);
        invalid_metadata["version"] = 2;
        const auto broken = invalid_metadata.dump();
        take(editor::write_file(target / "color.bmp.asset.json", broken));
        take(project.refresh());
        const auto revision = project.revision();
        const std::vector<assets::ProjectEntry> entries(project.entries().begin(), project.entries().end());
        const auto rejected = project.import_mesh(source / "Refresh.obj");
        std::printf("Orphan-texture fixture import result: %s\n",
                    rejected ? "accepted" : rejected.error().message.c_str());
        require(!rejected &&
                    rejected.error().message.starts_with("Import would overwrite an unrelated or unverified file:") &&
                    rejected.error().message.find("color.bmp.asset.json") != std::string::npos,
                "Import did not reject the existing orphan texture identity before publication.");
        require(
            project.revision() == revision && std::ranges::equal(project.entries(), entries) &&
                take(project.assets().mesh(resource)).mesh == original_mesh &&
                take(assets::read_file(metadata)) == metadata_text &&
                take(assets::read_file(target / "color.bmp.asset.json")) == broken &&
                std::ranges::all_of(fs::directory_iterator(target),
                                    [](const auto& file) { return file.path().filename() == L"color.bmp.asset.json"; }),
            "Rejected orphan texture import changed the accepted catalog/cache or kept owned files.");
        fs::remove(target / "color.bmp.asset.json");
        require(take(project.assets().mesh(take(project.import_mesh(source / "Refresh.obj")))).mesh != nullptr &&
                    fs::exists(target / "Refresh.obj.asset.json") && fs::exists(target / "scene.mtl") &&
                    take(assets::read_file(target / "color.bmp")) == bmp,
                "Import could not retry after orphan texture metadata repair.");
        std::puts(
            "Import orphan-texture rejection: PASS (existing identity protected before publication; catalog/cache/files preserved; retry)");
    }
    // The real catalogue, including its tail and imports, is not capped at 10000 entries.
    const auto capacity_entries = 10005 - project.entries().size();
    for (std::size_t i = 0; i < capacity_entries; ++i)
        fs::create_directory(folder / "Source" / ("Limit" + std::to_string(i)));
    take(project.refresh());
    const auto full_revision = project.revision();
    require(project.entries().size() == 10005 &&
                project.entry(project.entries().back().key) == &project.entries().back(),
            "A large catalog lost its tail entry.");
    take(project.refresh());
    require(project.revision() == full_revision && take(project.assets().mesh(resource)).mesh == original_mesh,
            "Unchanged large catalog refresh replaced the revision or resource cache.");
    const auto capacity_mesh = evidence / "Capacity.obj";
    take(editor::write_file(capacity_mesh, take(assets::read_file(external))));
    require(take(project.assets().mesh(take(project.import_mesh(capacity_mesh)))).mesh != nullptr &&
                project.entries().size() == 10006,
            "A large catalog rejected an import or lost its published entry.");
    std::puts("Large project catalog: PASS (10005 entries, tail lookup, unchanged refresh/cache, import to 10006)");
    for (std::size_t i = 0; i < capacity_entries; ++i)
        fs::remove(folder / "Source" / ("Limit" + std::to_string(i)));
    take(project.refresh());
    const auto original = take(assets::read_file(folder / "Content/Scenes/Main.scene"));
    fs::create_directory(folder / "Content/Scenes/blocked.scene");
    require(!editor::write_file(folder / "Content/Scenes/blocked.scene", saved),
            "Saving over a directory did not fail.");
    require(take(assets::read_file(folder / "Content/Scenes/Main.scene")) == original,
            "A failed save damaged the original scene.");
    // Move both source and metadata, preserving the persistent resource identity.
    fs::rename(folder / "Content/Meshes/Triangle.obj", folder / "Content/Meshes/Renamed.obj");
    fs::rename(folder / "Content/Meshes/Triangle.obj.asset.json", folder / "Content/Meshes/Renamed.obj.asset.json");
    take(project.refresh());
    require(take(project.assets().mesh(resource)).name == L"Renamed", "Asset rename changed its persistent identity.");
    require(take(project.assets().mesh(resource)).mesh == original_mesh,
            "Asset rename unnecessarily duplicated unchanged geometry.");
    const auto renamed = folder / "Content/Meshes/Renamed.obj";
    const auto old_time = fs::last_write_time(renamed);
    take(editor::write_file(renamed, "v 0 0 0\nv 4 0 0\nv 0 4 0\nf 1 2 3\n"));
    fs::last_write_time(renamed, old_time + std::chrono::seconds(1));
    take(project.refresh());
    const auto updated_mesh = take(project.assets().mesh(resource)).mesh;
    require(updated_mesh != original_mesh && updated_mesh->vertices()[1].position[0] == 4 &&
                original_mesh->vertices()[1].position[0] == 1,
            "Changed mesh did not create a new resource version while preserving old live geometry.");
    require(*take(project.collision(resource, collision_scale)) != *collision_bytes,
            "Changed geometry was served the stale cooked shape.");
    fs::remove_all(folder / "Cache"); // Rebuildable data cannot be required to open the project.
    const auto moved = evidence / L"搬移后";
    fs::rename(folder, moved);
    auto reopened = take(editor::Project::open(moved));
    auto restored = take(editor::Session::deserialize(
        take(assets::read_file(take(reopened.resolve(reopened.startup_scene())))), reopened));
    require(take(restored->serialize()) == saved, "Moving a project or removing Cache broke the scene.");
    fs::rename(moved / "Content/Meshes/Renamed.obj", moved / "Content/Meshes/Renamed.missing");
    take(reopened.refresh());
    require(!editor::Session::deserialize(saved, reopened, {}, editor::LoadPolicy::strict),
            "A missing resource was silently replaced.");
    {
        auto meshless = take(editor::Session::deserialize(saved, reopened));
        const auto issues = meshless->load_issues();
        const auto mesh_issue =
            std::ranges::find(issues, world::DiagnosticKind::missing_resource, &world::Diagnostic::kind);
        const auto restored = component(*meshless, take(meshless->world().members()).front().id(),
                                        meshless->presentation_types().static_mesh_component);
        const auto binding = take(presentation::read_static_mesh(meshless->world().runtime(), restored)).mesh;
        require(issues.size() == 1 && mesh_issue != issues.end() &&
                    mesh_issue->field == std::vector<std::string>{"mesh"} && mesh_issue->object == restored &&
                    binding.source == resource && !binding.resolved && take(meshless->serialize()) == saved,
                "Tolerant loading did not keep a missing mesh reference verbatim with one diagnostic.");
    }
    require(take(document.serialize()) == saved, "Failed loading changed the original document.");
    {
        // Material vectors now belong to typed scene MI entries, not mesh-component fields.
        editor::Session authored;
        changed(authored.create_static_mesh(assets::mesh_assets().front()));
        const auto body =
            component(authored, authored.selection().entity, authored.presentation_types().static_mesh_component);
        take(authored.assign_material(reopened, body, "slot/0", "builtin/material/standard-opaque-double"));
        const auto instance = take(authored.create_scene_material(body, "slot/0"));
        take(authored.begin_material_edit(instance.id(), 304));
        take(authored.preview_material_entry(304, {"base_color", math::Vector4{.2f, .3f, .4f, .5f}}));
        take(authored.commit_material_edit(304));
        const auto valid = take(authored.serialize());
        auto copy = take(editor::Session::deserialize(valid, reopened, {}, editor::LoadPolicy::strict));
        require(take(copy->serialize()) == valid, "Typed scene MI values did not roundtrip.");
        auto invalid = nlohmann::json::parse(valid);
        bool damaged{};
        for (auto& material : invalid["materials"])
            if (material.contains("entries"))
                for (auto& entry : material["entries"])
                    if (entry["path"] == "base_color") {
                        entry["value"].push_back(999);
                        damaged = true;
                    }
        require(damaged && !editor::Session::deserialize(invalid.dump(), reopened, {}, editor::LoadPolicy::strict) &&
                    !editor::Session::deserialize(invalid.dump(), reopened),
                "Scene loading silently truncated a typed MI vector.");
        const auto original = take(material::read_instance(authored.world().runtime(), instance.id()));
        auto invalid_state = original;
        std::get<math::Vector4>(invalid_state.entries.front().value).x = std::numeric_limits<float>::quiet_NaN();
        take(material::update_instance(authored.world().runtime(), instance.id(), invalid_state));
        require(!authored.serialize(), "Save produced nonfinite MI author data that its own loader rejects.");
        take(material::update_instance(authored.world().runtime(), instance.id(), original));
        require(take(authored.serialize()) == valid, "Repairing invalid MI data changed the saved scene.");
    }
    take(presentation::set_geometry(document.world().runtime(), mesh, assets::mesh_assets().front().mesh));
    require(take(presentation::read_static_mesh(document.world().runtime(), mesh)).mesh.source.empty() &&
                !document.serialize(),
            "Unattributed procedural geometry retained stale provenance or was silently saved.");
    std::ofstream(evidence / "acceptance.txt")
        << "Project document acceptance: PASS\nCreate/open; UTF-8; OBJ import; stable resource ID; full composition; dirty undo; version/read/write failure; moved project; missing resource; Cache removal.\n";
    std::puts("Project document acceptance: PASS");
}
