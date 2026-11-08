void verify_project_document() {
    namespace fs = std::filesystem;
    const auto evidence = fs::current_path() / "project-document-evidence" /
                          std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    fs::create_directories(evidence);
    const auto folder = evidence / L"工程";
    auto project = take(editor::Project::create(folder));
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
        // Asset rename moves its identity sidecar and preserves the original on collision.
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
        const auto taken = folder / renamed.parent_path() / "Taken.obj";
        take(editor::write_file(taken, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"));
        take(project.refresh());
        static_cast<void>(project.rename(renamed, L"Taken.obj"));
        require(fs::exists(folder / renamed), "Failed rename removed the original asset.");
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
    editor::Session document;
    take(document.initialize());
    changed(document.create_static_mesh(take(project.assets().mesh(resource))));
    const auto entity = document.selection().entity;
    take(document.rename(entity, L"主角 Triangle"));
    const auto mesh = component(document, entity, document.presentation_types().static_mesh_component);
    take(const_cast<world::World&>(document.world()).set_transform(entity, {{1, 2, 3}, {0, 0, 0, 1}, {2, 3, 4}}));
    changed(document.add_component(entity, document.types().component));
    changed(
        document.add_component(entity, document.presentation_types().static_mesh_component)); // Null mesh must survive.
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
        take(built_in.initialize());
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
        take(physical.initialize());
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
            take(imported.initialize());
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
    const auto serialized = assets::json_format::parse(saved);
    require(serialized.has_value(), "Saved scene JSON failed to parse.");
    json::Builder invalid{*serialized};
    const auto position = invalid.root().at("entities")->at(0)->path({"space", "position"});
    require(position.has_value(), "Saved position was missing.");
    invalid.append(*position, std::int64_t{999});
    const auto invalid_bytes = json::write(invalid.root());
    auto kept = take(editor::Session::deserialize({invalid_bytes.data(), invalid_bytes.size()}, project));
    const auto kept_json = assets::json_format::parse(take(kept->serialize()));
    require(kept_json && *kept_json->root().at("entities")->at(0)->path({"space", "position"}) == *position,
            "Tolerant scene loading did not keep a mismatched saved value.");
    static_cast<void>(editor::write_file(folder / "Content/Scenes/Main.scene", "overwrite", editor::WriteMode::create));
    require(take(assets::read_file(folder / "Content/Scenes/Main.scene")) == saved,
            "Create-only file publication overwrote an existing scene.");
    const auto metadata = folder / "Content/Meshes/Triangle.obj.asset.json";
    const auto metadata_text = take(assets::read_file(metadata));
    take(editor::write_file(metadata, "{broken"));
    const auto retry = evidence / "Retry.obj";
    take(editor::write_file(retry, take(assets::read_file(external))));
    static_cast<void>(project.import_mesh(retry));
    require(!fs::exists(folder / "Content/Meshes/Retry.obj") &&
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
    static_cast<void>(project.import_mesh(orphan));
    require(!fs::exists(folder / "Content/Meshes/Orphan.obj") &&
                take(assets::read_file(folder / "Content/Meshes/Orphan.obj.asset.json")) == metadata_text,
            "Import claimed or replaced orphan resource metadata.");
    {
        // The orphan is absent from the catalogue. The transaction now protects its
        // identity file during preflight, before publishing the BMP or any other file.
        const auto source = evidence / "refresh-source";
        const auto target = folder / "Content/Meshes/Refresh";
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
        const auto parsed_metadata = assets::json_format::parse(metadata_text);
        require(parsed_metadata.has_value(), "Mesh metadata fixture failed to parse.");
        json::Builder invalid_metadata{*parsed_metadata};
        require(invalid_metadata.set(invalid_metadata.root(), "version", std::uint32_t{2}).has_value(),
                "Metadata fixture mutation failed.");
        const auto broken_bytes = json::write(invalid_metadata.root());
        const std::string broken{broken_bytes.data(), broken_bytes.size()};
        take(editor::write_file(target / "color.bmp.asset.json", broken));
        take(project.refresh());
        const auto revision = project.revision();
        const std::vector<assets::ProjectEntry> entries(project.entries().begin(), project.entries().end());
        static_cast<void>(project.import_mesh(source / "Refresh.obj"));
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
    const auto original = take(assets::read_file(folder / "Content/Scenes/Main.scene"));
    fs::create_directory(folder / "Content/Scenes/blocked.scene");
    static_cast<void>(editor::write_file(folder / "Content/Scenes/blocked.scene", saved));
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
        take(authored.initialize());
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
    }
    take(presentation::set_geometry(document.world().runtime(), mesh, assets::mesh_assets().front().mesh));
    require(take(presentation::read_static_mesh(document.world().runtime(), mesh)).mesh.source.empty(),
            "Unattributed procedural geometry retained stale provenance.");
    std::puts("Project document acceptance: PASS");
}
