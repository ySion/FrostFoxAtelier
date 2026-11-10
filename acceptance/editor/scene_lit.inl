material::Output scene_lit_material(const material::ProgramSurfaceRef& surface) {
    require(surface != nullptr, "Scene Lit mesh has no captured program surface.");
    return take(surface->evaluate({}));
}

std::filesystem::path scene_lit_example(const std::filesystem::path& requested) {
    return std::filesystem::canonical(requested.empty() ? std::filesystem::path{ATELIER_EXAMPLE_PROJECT} : requested);
}

void verify_scene_lit_example(const std::filesystem::path& requested) {
    using namespace editor;
    const auto source = scene_lit_example(requested);
    const auto source_project = take(Project::open(source));
    const auto source_scene = take(source_project.resolve("Content/Scene Lit Room/Scenes/Main.scene"));
    const auto source_text = take(assets::read_file(source_scene));
    const auto source_config = take(assets::read_file(source / "Config/Project.json"));
    const auto copy = std::filesystem::absolute("out/scene-lit-application-evidence") /
                      ("example-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    take(Project::create(copy));
    std::filesystem::copy(source / "Content/Scene Lit Room", copy / "Content/Scene Lit Room",
                          std::filesystem::copy_options::recursive);
    auto project = take(Project::open(copy));
    auto document = take(Session::deserialize(
        take(assets::read_file(take(project.resolve("Content/Scene Lit Room/Scenes/Main.scene")))), project));
    application::SceneSnapshot scene;
    take(scene.refresh(*document));
    const auto sky = [](const Session& session) {
        return rendering::sky_radiance(take(session.presentation().environment()));
    };
    require(
        scene.meshes.size() == 10 && scene.lights.size() == 4 && sky(*document) == std::array{.025f, .035f, .055f} &&
            take(document->world().members()).size() == 16 && document->environment() != ObjectId{} &&
            take(document->presentation().environment()).probe_volumes[0].enabled,
        "Scene Lit example did not load its ten meshes, four direct lights and an Environment with its sky and DDGI volume.");
    require(std::ranges::count(scene.lights, renderer::LightKind::directional, &renderer::Light::kind) == 1 &&
                std::ranges::count(scene.lights, renderer::LightKind::point, &renderer::Light::kind) == 2 &&
                std::ranges::count(scene.lights, renderer::LightKind::rectangle, &renderer::Light::kind) == 1 &&
                std::ranges::all_of(scene.lights, [](const auto& light) { return light.casts_shadow; }),
            "Scene Lit example lost its one directional, two point and one rectangle shadow lights.");
    require(std::ranges::any_of(scene.picking()->materials,
                                [](const auto& surface) { return scene_lit_material(surface).metallic > .5f; }) &&
                std::ranges::any_of(scene.picking()->materials,
                                    [](const auto& surface) { return scene_lit_material(surface).roughness < .2f; }) &&
                std::ranges::any_of(scene.picking()->materials,
                                    [](const auto& surface) {
                                        const auto e = scene_lit_material(surface).emissive;
                                        return e.x > 1 || e.y > 1 || e.z > 1;
                                    }),
            "Scene Lit example lost its real metal, reflection roughness or HDR emission.");
    const auto entity = take(document->world().members()).front().id();
    const auto mesh = component(*document, entity, document->presentation_types().static_mesh_component);
    const auto instance = take(document->create_scene_material(mesh, "slot/0"));
    const auto fields = take(document->inspect_material(instance.id()));
    require(std::ranges::find(fields.declarations, "roughness", &MaterialParameterView::path) !=
                fields.declarations.end(),
            "Scene Lit example floor has no declared roughness.");
    take(document->begin_material_edit(instance.id(), 991000));
    take(document->preview_material_entry(991000, {"roughness", .71f, true}));
    take(document->commit_material_edit(991000));
    const auto saved = take(document->serialize());
    const auto saved_path = take(project.resolve("Content/Scenes/Acceptance.scene"));
    take(write_file(saved_path, saved, WriteMode::create));
    auto reopened = take(Session::deserialize(take(assets::read_file(saved_path)), project));
    application::SceneSnapshot restored;
    take(restored.refresh(*reopened));
    require(take(reopened->serialize()) == saved && restored.meshes.size() == 10 && restored.lights.size() == 4 &&
                scene_lit_material(snapshot_surface(restored)).roughness == .71f && sky(*reopened) == sky(*document),
            "Scene Lit example temporary save/reopen lost material or formal light data.");
    // Runtime presentation uses the same World snapshot contract. An editing
    // document's Parameters panel intentionally requires a stopped document.
    const auto began = take(reopened->start());
    require(!began.notification_error, "Scene Lit runtime World Begin notification failed.");
    take(reopened->update(1.f / 60));
    application::SceneSnapshot running;
    running.reset();
    take(running.refresh(*reopened));
    require(reopened->world().running() && running.meshes.size() == restored.meshes.size() &&
                running.lights.size() == restored.lights.size() && sky(*reopened) == sky(*document),
            "Running World lost its formal render snapshot.");
    for (std::size_t i = 0; i < running.meshes.size(); ++i)
        require(running.meshes[i].world == restored.meshes[i].world &&
                    running.meshes[i].geometry == restored.meshes[i].geometry &&
                    scene_lit_material(snapshot_surface(running, i)).roughness ==
                        scene_lit_material(snapshot_surface(restored, i)).roughness &&
                    scene_lit_material(snapshot_surface(running, i)).emissive ==
                        scene_lit_material(snapshot_surface(restored, i)).emissive,
                "Unchanged runtime World altered geometry or materials based on execution state.");
    for (std::size_t i = 0; i < running.lights.size(); ++i)
        require(running.lights[i].kind == restored.lights[i].kind &&
                    running.lights[i].position == restored.lights[i].position &&
                    running.lights[i].intensity == restored.lights[i].intensity,
                "Unchanged runtime World altered authored lights based on execution state.");
    const auto ended = take(reopened->stop());
    require(!ended.notification_error && take(reopened->serialize()) == saved,
            "Runtime World presentation modified the saved editing document.");
    require(take(assets::read_file(source_scene)) == source_text &&
                take(assets::read_file(source / "Config/Project.json")) == source_config,
            "Scene Lit example acceptance modified the source project.");
    const auto source_text_path = platform::utf16_to_utf8(source.native());
    require(bool(source_text_path), "Invalid UTF-16 Scene Lit fixture path.");
    std::ofstream(copy / "acceptance.txt")
        << "Scene Lit example Project/Scene: PASS\nSource read only: " << *source_text_path
        << "\nTemporary saved Scene: Content/Scenes/Acceptance.scene\n10 meshes, 4 shadow lights, sky in the Environment; HDR/metal/roughness and edited material survive reopening.\n";
    std::puts("Scene Lit example Project/Scene acceptance: PASS (source read only, temporary save/reopen)");
}

void verify_environment_sky_application() {
    using namespace editor;
    world::World* sky_world{};
    Session document;
    take(document.initialize([&](world::World& scene) -> Result<std::shared_ptr<void>> {
        sky_world = &scene;
        return std::shared_ptr<void>{};
    }));
    application::SceneSnapshot snapshot;
    const std::array<float, 3> dark{};
    const std::array<float, 3> radiance{.025f, .05f, .075f};
    const auto sky = [](const Session& session) {
        return rendering::sky_radiance(take(session.presentation().environment()));
    };
    require(document.environment() == ObjectId{} && sky(document) == dark,
            "A World without an Environment has implicit environment radiance.");
    // The first World Settings edit creates the Environment and writes its values as one history item.
    auto lighting = take(document.presentation().environment());
    lighting.sky_enabled = true;
    lighting.sky_color = {.25f, .5f, .75f};
    lighting.sky_intensity = .1f;
    take(document.set_environment(lighting));
    const auto environment = document.environment();
    const auto entity = take(world::inspect_component(document.world().runtime(), environment)).owner;
    require(environment != ObjectId{} && document.undo_count() == 1 && document.name(entity) == L"Environment" &&
                sky(document) == radiance && take(world::components(document.world().runtime(), entity)).size() == 1 &&
                !take(world::has_transform(document.world().runtime(), entity)),
            "The first World Settings edit did not create one spaceless Environment with the sky in one history item.");
    changed(document.undo());
    require(document.environment() == ObjectId{} && take(document.world().members()).empty() && sky(document) == dark,
            "Undoing the first World Settings edit left an Environment or its sky.");
    changed(document.redo());
    require(document.environment() == environment && sky(document) == radiance,
            "Redo did not restore the same Environment and its sky.");
    lighting.sky_intensity = .2f;
    take(document.set_environment(lighting));
    require(document.undo_count() == 2 && take(world::components(document.world().runtime(), entity)).size() == 1 &&
                sky(document) == std::array{.05f, .1f, .15f},
            "A later World Settings edit did not write the existing Environment.");
    changed(document.undo());
    require(sky(document) == radiance, "Undo did not restore the earlier sky.");
    lighting.sky_intensity = .1f;
    const auto history = document.undo_count();
    take(document.select(entity));
    static_cast<void>(document.add_component(entity, document.presentation_types().environment_component));
    require(document.undo_count() == history, "A refused Environment addition changed history.");
    changed(document.add_entity());
    const auto other = document.selection().entity;
    changed(sky_world->set_enabled(environment, false));
    require(sky(document) == dark, "A disabled Environment kept its sky.");
    // World Settings show a disabled Environment's defaults, so an edit is refused rather than diffed against them.
    auto refused = lighting;
    refused.exposure_ev += 1;
    const auto disabled_history = document.undo_count();
    static_cast<void>(document.set_environment(refused));
    require(document.undo_count() == disabled_history, "A refused disabled Environment edit changed history.");
    changed(sky_world->set_enabled(environment, true));
    changed(sky_world->set_enabled(entity, false));
    require(sky(document) == dark, "A disabled Environment Entity kept its sky.");
    static_cast<void>(document.set_environment(refused));
    require(document.undo_count() == disabled_history, "A refused disabled Entity edit changed history.");
    changed(sky_world->set_enabled(entity, true));
    require(sky(document) == radiance && take(document.presentation().environment()) == lighting,
            "A re-enabled Environment did not restore its sky or kept a refused edit.");
    take(document.select(other));
    changed(document.erase_entities(document.selected_entities()));
    take(snapshot.refresh(document));
    changed(document.create_light(presentation::LightKind::point));
    const auto point = component(document, document.selection().entity, document.presentation_types().light_component);
    take(snapshot.refresh(document));
    require(snapshot.lights.size() == 1, "Appending a direct light did not reach the scene snapshot.");
    changed(sky_world->set_enabled(point, false));
    take(snapshot.refresh(document));
    require(snapshot.lights.empty(), "Disabling the trailing direct light did not update scene membership.");
    changed(sky_world->set_enabled(point, true));
    take(snapshot.refresh(document));
    require(snapshot.lights.size() == 1, "Re-enabling the trailing direct light did not update scene membership.");
    take(document.select(entity));
    changed(document.remove_component(environment));
    require(sky(document) == dark && document.environment() == ObjectId{}, "A removed Environment remained effective.");
    changed(document.undo());
    require(sky(document) == radiance && document.environment() == environment,
            "Undo did not restore the same Environment and its sky.");
    const auto folder = std::filesystem::current_path() / "out/scene-lit-application-evidence" /
                        ("sky-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto project = take(Project::create(folder));
    const auto text = take(document.serialize());
    const auto file = take(project.resolve("Content/Scenes/Sky.scene"));
    take(write_file(file, text, WriteMode::create));
    auto reopened = take(Session::deserialize(take(assets::read_file(file)), project));
    snapshot.reset();
    take(snapshot.refresh(*reopened));
    const auto saved = take(assets::json_format::parse(text));
    require(sky(*reopened) == radiance && snapshot.lights.size() == 1 && !take(saved.root().find("world")),
            "The Environment sky changed after saved scene reopen.");
    std::puts(
        "Environment sky application acceptance: PASS (first edit creates it, unique, enable/remove/undo, scene save/reopen)");
}

void verify_scene_snapshot_updates() {
    using namespace editor;
    Session document;
    take(document.initialize());
    auto& world = const_cast<world::World&>(document.world());
    changed(document.create_static_mesh(assets::mesh_assets().front()));
    const auto first = document.selection().entity;
    changed(document.create_static_mesh(assets::mesh_assets().front()));
    changed(document.create_light(presentation::LightKind::point));
    const auto light = component(document, document.selection().entity, document.presentation_types().light_component);
    application::SceneSnapshot snapshot;
    take(snapshot.refresh(document));
    require(snapshot.meshes.size() == 2 && snapshot.lights.size() == 1,
            "The refreshed snapshot lost an authored mesh or light.");
    const auto keys = std::array{snapshot.meshes[0].stable_key, snapshot.meshes[1].stable_key};
    take(world.runtime().write(first, document.types().position_fields[0], Scalar{2.f}));
    take(snapshot.refresh(document));
    require(snapshot.meshes[0].world[12] == 2 && snapshot.meshes[0].stable_key == keys[0] &&
                snapshot.meshes[1].stable_key == keys[1],
            "The moved mesh lost its position or a mesh lost its stable key.");
    take(document.select(first));
    take(snapshot.refresh(document));
    require(snapshot.meshes[0].selected && !snapshot.meshes[1].selected,
            "The refreshed snapshot did not highlight exactly the selection.");
    // Dragging a selected mesh rewrites its instance in place on every frame.
    take(world.runtime().write(first, document.types().position_fields[0], Scalar{3.f}));
    take(snapshot.refresh(document));
    require(snapshot.meshes[0].selected && !snapshot.meshes[1].selected && snapshot.meshes[0].world[12] == 3,
            "Moving the selected mesh lost its highlight.");
    auto settings = take(presentation::read_light(world.runtime(), light));
    settings.intensity = 9;
    take(presentation::set_light(world, light, settings));
    take(snapshot.refresh(document));
    require(snapshot.lights[0].intensity == 9, "The refreshed snapshot lost the light edit.");
    snapshot.reset();
    take(snapshot.refresh(document));
    require(snapshot.meshes[0].selected && snapshot.meshes[0].world[12] == 3,
            "A reset snapshot lost the selection or the moved position.");
    std::puts(
        "Scene snapshot update acceptance: PASS (authored position, selection and light values survive refresh/reset)");
}

void verify_scene_lit_application(const std::filesystem::path& source_example = {}) {
    verify_scene_lit_example(source_example);
    verify_environment_sky_application();
    verify_scene_snapshot_updates();
    using namespace editor;
    const auto folder = std::filesystem::current_path() / "out/scene-lit-application-evidence" /
                        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto project = take(Project::create(folder));
    Session document;
    take(document.initialize({}, SessionRole::editor));
    auto& runtime = document.world().runtime();
    changed(document.create_static_mesh(assets::mesh_assets().front()));
    const auto mesh_entity = document.selection().entity;
    const auto mesh = component(document, mesh_entity, document.presentation_types().static_mesh_component);
    std::uint64_t edit_session = 990000;
    const auto property = [&](ObjectId object, std::string_view name) {
        const auto fields = take(document.properties(object));
        const auto at = std::ranges::find(fields, name, &Property::name);
        require(at != fields.end(), "Expected reflected lighting property is absent.");
        return at->path;
    };
    const auto edit = [&](ObjectId object, std::string_view name, Scalar value) {
        take(document.set_property(object, property(object, name), std::move(value), edit_session++));
    };
    take(document.assign_material(project, mesh, "slot/0", "builtin/material/standard-opaque-double"));
    const auto instance = take(document.create_scene_material(mesh, "slot/0"));
    const auto edit_material = [&](std::string path, material::LocalValue value) {
        const auto control = edit_session++;
        take(document.begin_material_edit(instance.id(), control));
        take(document.preview_material_entry(control, {std::move(path), std::move(value), true}));
        take(document.commit_material_edit(control));
    };
    edit_material("roughness", .27f);
    edit_material("metallic", .6f);
    edit_material("emissive", math::Vector3{4, 0, 0});
    std::array<ObjectId, 3> light_entities;
    std::array<ObjectId, 3> lights;
    constexpr std::array kinds{presentation::LightKind::point, presentation::LightKind::rectangle,
                               presentation::LightKind::directional};
    for (std::size_t i = 0; i < kinds.size(); ++i) {
        const auto history = document.undo_count();
        changed(document.create_light(kinds[i], {float(i) * 2, 3, 0}));
        light_entities[i] = document.selection().entity;
        lights[i] = component(document, light_entities[i], document.presentation_types().light_component);
        require(document.undo_count() == history + 1 &&
                    take(world::components(runtime, light_entities[i])).size() == 1 &&
                    take(presentation::read_light(runtime, lights[i])).kind == kinds[i],
                "Light creation did not publish one complete, undoable Entity composition.");
    }
    edit(lights[0], "settings.intensity", 80.f);
    edit(lights[0], "settings.source_radius", .05f);
    edit(lights[1], "settings.width", 2.f);
    edit(lights[1], "settings.height", 3.f);
    edit(lights[1], "settings.two_sided", true);
    edit(lights[2], "settings.intensity", 1000.f);
    edit(lights[2], "settings.source_radius", .0046f);
    const auto component_types = take(document.component_types());
    require(std::ranges::any_of(
                component_types,
                [&](const auto& type) { return type.type == document.presentation_types().light_component; }),
            "Entity Structure cannot discover LightComponent through the shared type list.");
    application::SceneSnapshot snapshot;
    take(snapshot.refresh(document));
    require(snapshot.meshes.size() == 1 && snapshot.lights.size() == 3 &&
                scene_lit_material(snapshot_surface(snapshot)).roughness == .27f &&
                scene_lit_material(snapshot_surface(snapshot)).emissive.x == 4 && snapshot.lights[1].two_sided &&
                snapshot.lights[1].width == 2,
            "World material and light values did not reach the formal renderer snapshot.");
    const auto key = snapshot.meshes[0].stable_key;
    const auto light_key = snapshot.lights[0].stable_key;
    require(key && light_key && key != light_key, "Opaque component identities collided in the renderer projection.");
    edit(mesh_entity, "position.x", 2.f);
    edit(light_entities[0], "position.x", 3.f);
    take(document.select(mesh_entity));
    take(snapshot.refresh(document));
    require(snapshot.meshes[0].stable_key == key && snapshot.lights[0].stable_key == light_key &&
                snapshot.meshes[0].world[12] == 2 && snapshot.lights[0].position[0] == 3,
            "TRS or selection churn lost component identity or the new pose.");
    edit_material("roughness", .38f);
    take(snapshot.refresh(document));
    require(scene_lit_material(snapshot_surface(snapshot)).roughness == .38f,
            "Material edits did not reach rendering.");
    changed(document.undo());
    take(snapshot.refresh(document));
    require(scene_lit_material(snapshot_surface(snapshot)).roughness == .27f, "Material undo did not reach rendering.");
    take(document.select(light_entities[0]));
    changed(document.remove_component(lights[0]));
    take(snapshot.refresh(document));
    require(snapshot.lights.size() == 2, "Removed LightComponent remained in the renderer snapshot.");
    changed(document.undo());
    take(snapshot.refresh(document));
    require(component(document, light_entities[0], document.presentation_types().light_component) == lights[0] &&
                snapshot.lights.size() == 3,
            "Component undo failed to restore the real light identity and presentation.");
    const auto text = take(document.serialize());
    const auto file = take(project.resolve("Content/Scenes/Lighting.scene"));
    take(write_file(file, text, WriteMode::create));
    auto reopened = take(Session::deserialize(take(assets::read_file(file)), project));
    application::SceneSnapshot loaded;
    take(loaded.refresh(*reopened));
    require(loaded.meshes.size() == 1 && loaded.lights.size() == 3 &&
                scene_lit_material(snapshot_surface(loaded)).emissive.x == 4 &&
                scene_lit_material(snapshot_surface(loaded)).metallic == .6f && loaded.lights[0].intensity == 80 &&
                loaded.lights[2].source_radius == .0046f,
            "Project scene reopen lost material or light settings.");
    json::Builder data{take(assets::json_format::parse(text))};
    std::optional<std::pair<std::size_t, std::size_t>> roughness_path;
    const auto definitions = take(data.root().at("materials"));
    for (std::size_t i = 0; i < definitions.size(); ++i)
        if (take(definitions.at(i)).string({"source"}) == "") {
            const auto entries = take(take(definitions.at(i)).at("entries"));
            for (std::size_t j = 0; j < entries.size(); ++j)
                if (take(entries.at(j)).string({"path"}) == "roughness") {
                    const auto entry = take(entries.at(j));
                    take(data.set(entry, "type", "vector2"));
                    take(data.set(entry, "value", json::Document::literal<"[2,3]">().root()));
                    roughness_path = {i, j};
                }
        }
    require(roughness_path.has_value(), "Saved scene MI lost its roughness entry.");
    // A declaration mismatch is preserved author intent, not a malformed file.
    {
        const auto bytes = json::write(data.root());
        auto kept = take(Session::deserialize({bytes.data(), bytes.size()}, project));
        const auto items = take(kept->presentation().render_items());
        const auto restored = take(assets::json_format::parse(take(kept->serialize())));
        const auto [i, j] = *roughness_path;
        require(
            *restored.root().at("materials")->at(i)->at("entries")->at(j) == *definitions.at(i)->at("entries")->at(j) &&
                std::ranges::any_of(kept->load_issues(),
                                    [](const auto& issue) {
                                        return issue.kind == world::DiagnosticKind::mismatch &&
                                               issue.severity == world::Severity::info;
                                    }) &&
                scene_lit_material(items.front().surfaces->front()).roughness == .65f &&
                scene_lit_material(items.front().surfaces->front()).metallic == .6f,
            "A mismatched sparse material entry lost its author value/diagnostic or changed valid material parameters.");
    }
    data = json::Builder{take(assets::json_format::parse(text))};
    take(data.set(*data.root().at("entities")->at(1)->at("components")->at(0)->path({"properties", "settings"}),
                  "range", 0u));
    const auto invalid_bytes = json::write(data.root());
    auto kept = take(Session::deserialize({invalid_bytes.data(), invalid_bytes.size()}, project));
    const auto retained = take(assets::json_format::parse(take(kept->serialize())));
    require(retained.root().at("entities")->at(1)->at("components")->at(0)->u32({"properties", "settings", "range"}) ==
                0,
            "Tolerant loading did not retain the authored invalid light range.");
    data = json::Builder{take(assets::json_format::parse(text))};
    for (auto definition : *data.root().at("materials")->array())
        if (definition.string({"source"}) == "")
            take(data.set(definition, "entries", json::Document::literal<"[]">().root()));
    const auto default_bytes = json::write(data.root());
    auto defaulted = take(Session::deserialize({default_bytes.data(), default_bytes.size()}, project));
    const auto defaulted_items = take(defaulted->presentation().render_items());
    const auto defaults = scene_lit_material(defaulted_items[0].surfaces->front());
    require(defaults.roughness == .65f && defaults.emissive == math::Vector3{},
            "Absent material entries did not inherit the actual MS defaults.");
    const auto identity = snapshot.identity;
    snapshot.reset();
    take(snapshot.refresh(*reopened));
    require(snapshot.identity != identity, "Document replacement reused the previous renderer history domain.");

    operator_parameters::Host host(document);
    auto& workspace = host.panels;
    const std::array fonts{(asset_directory() / "fonts/SarasaUiSC-Regular.ttf").wstring()};
    ui::Session surface;
    take(surface.initialize(fonts));
    const auto theme = ui::StyleSheet::parse(take(assets::read_file(asset_directory() / "editor.ss")));
    require(theme.has_value() && surface.set_style_sheet(*theme).has_value(),
            "Lighting production editor.ss failed to resolve.");
    require(bool(surface.set_surface({{1500, 1000}, 1, 1})), "Lighting UI surface failed.");
    std::vector<ui::Control> controls;
    const auto publish = [&] {
        controls = take(workspace.describe(document, {1500, 1000}, surface.presented().get(), &project));
        if (!workspace.ui_error().empty())
            throw std::runtime_error("Lighting DSL describe failed: " + std::string(workspace.ui_error()));
        const auto error = [](const ui::Error& value) {
            return "code=" + std::to_string(static_cast<unsigned>(value.code)) + ", key=" + std::to_string(value.key) +
                   ", native=" + std::to_string(value.native_code);
        };
        const auto described = surface.set_controls(controls);
        if (!described)
            throw std::runtime_error("Lighting DSL controls failed: " + error(described.error()));
        workspace.commit_ui_controls();
        const auto frame = surface.prepare();
        if (!frame)
            throw std::runtime_error("Lighting DSL layout prepare failed: " + error(frame.error()));
        require(surface.accept(*frame), "Lighting DSL candidate accept failed after successful preparation.");
    };
    take(document.select(mesh));
    publish();
    require(std::ranges::any_of(controls,
                                [&](const auto& control) {
                                    // DSL buttons have their own node key and route clicks to the action target.
                                    const auto* binding = workspace.binding(control.action_target);
                                    return control.kind == ui::ControlKind::button && control.enabled && binding &&
                                           binding->object == instance.id() && binding->material_path == "emissive" &&
                                           binding->kind == Workspace::Binding::Kind::color_pick;
                                }),
            "Actual material panel did not expose a path-based color binding.");
    take(document.select(light_entities[2]));
    publish();
    const auto history = document.undo_count();
    require(take(workspace.apply_view(document, host, {ui::ActionKind::activate, control::shading_scene_lit, {}})) &&
                workspace.view.shading == renderer::Shading::scene_lit,
            "The formal display command did not select Scene Lit.");
    require(!take(workspace.apply_view(document, host, {ui::ActionKind::activate, control::guide_background, true})) &&
                workspace.view.background == renderer::Background::gradient,
            "Scene Lit can enable a studio gradient or destroys the stored studio preference.");
    const auto set_world_number = [&](ViewOption option, double value) {
        const auto key = world_control::field(option);
        take(workspace.apply_view(document, host, {ui::ActionKind::edit_begin, key, {}}));
        require(take(workspace.apply_view(document, host, {ui::ActionKind::edit_commit, key, value})),
                "World numeric setting was not routed.");
    };
    set_world_number(ViewOption::exposure_ev, 2);
    require(workspace.view.lit.exposure_ev == 2 && document.undo_count() > history,
            "World exposure did not enter scene history or used the wrong units.");
    set_world_number(ViewOption::direct_candidates, 32);
    require(workspace.view.lit.direct_candidates == 32 &&
                take(document.presentation().environment()).direct_candidates == 32,
            "Direct candidates were not routed through World author settings.");
    {
        workspace.menu.reset();
        workspace.dock.select(world_control::tab);
        publish();
        take(workspace.apply_ui({ui::ActionKind::activate, world_control::ddgi_tab, {}}));
        publish();
        const auto count0 = world_control::field(probe_volume_option(0, ProbeVolumeField::count_x));
        const auto old_view = workspace.view;
        const auto revision = workspace.view_revision();
        const auto edit = [&](ui::ActionKind kind, ui::ControlKey key, double value = 0) {
            return workspace.apply_view(document, host, {kind, key, value});
        };
        take(edit(ui::ActionKind::edit_begin, count0));
        take(edit(ui::ActionKind::edit_preview, count0, 10));
        publish();
        require(workspace.lighting.editing() && workspace.view == old_view && workspace.view_revision() == revision,
                "Lighting preview published renderer settings before commit.");
        take(edit(ui::ActionKind::edit_cancel, count0));
        require(!workspace.lighting.editing() && workspace.view == old_view,
                "Cancelled lighting input changed settings.");
        take(edit(ui::ActionKind::edit_begin, count0));
        take(edit(ui::ActionKind::edit_commit, count0, 10));
        require(workspace.view.lit.probe_volumes[0].counts[0] == 10 && workspace.view_revision() == revision + 1,
                "Lighting commit did not publish once.");
        take(edit(ui::ActionKind::edit_begin, count0));
        take(edit(ui::ActionKind::edit_commit, count0, 10));
        require(workspace.view_revision() == revision + 1, "Repeated lighting commit changed the view revision.");
        const auto before_invalid = workspace.view;
        const auto max0 = world_control::field(probe_volume_option(0, ProbeVolumeField::max_x));
        take(edit(ui::ActionKind::edit_begin, max0));
        static_cast<void>(edit(ui::ActionKind::edit_commit, max0, before_invalid.lit.probe_volumes[0].minimum[0]));
        require(workspace.view == before_invalid && !workspace.lighting.editing(),
                "Invalid bounds left a partial publication or live draft.");
        take(edit(ui::ActionKind::edit_begin, count0));
        take(workspace.apply_view(document, host, {ui::ActionKind::activate, world_control::slot_first + 1, {}}));
        publish();
        take(edit(ui::ActionKind::edit_commit, count0, 12));
        take(edit(ui::ActionKind::edit_begin, count0));
        require(workspace.view == before_invalid && !workspace.lighting.editing() &&
                    workspace.lighting.selected_volume() == 1,
                "A retired volume row resumed editing after a slot switch.");
        const auto count1 = world_control::field(probe_volume_option(1, ProbeVolumeField::count_x));
        take(edit(ui::ActionKind::edit_begin, count1));
        workspace.reset_document();
        take(edit(ui::ActionKind::edit_commit, count1, 12));
        publish();
        require(workspace.view == before_invalid && !workspace.lighting.editing() && document.undo_count() > history,
                "Document replacement kept a lighting draft or lost World history.");
    }
    {
        // World Settings edits are Environment property edits; their Undo selects the Environment.
        const auto selected = document.selection().entity;
        const auto settings = [](const Session& session) { return take(session.presentation().environment()); };
        const auto environment_record = [](json::Value scene) {
            for (auto entity : take(take(scene.at("entities")).array()))
                for (auto part : take(take(entity.at("components")).array()))
                    if (part.string({"type"}) == "world.EnvironmentComponent")
                        return take(part.at("properties"));
            throw std::runtime_error("The saved scene has no Environment.");
        };
        const auto before = settings(document);
        const auto count = document.undo_count();
        auto candidate = before;
        candidate.exposure_ev = 3.5f;
        take(document.set_environment(candidate));
        take(document.set_environment(candidate));
        require(document.undo_count() == count + 1, "Repeated World settings created duplicate history.");
        take(document.undo());
        workspace.sync_world(document);
        require(settings(document) == before &&
                    static_cast<const rendering::SceneLightingSettings&>(workspace.view.lit) == before,
                "Undo did not restore World and the evaluated view together.");
        take(document.redo());
        workspace.sync_world(document);
        const auto saved = take(document.serialize());
        auto restored = take(Session::deserialize(saved, project));
        require(settings(*restored) == candidate && take(restored->serialize()) == saved && !restored->dirty(),
                "Scene v10 did not round-trip World authoring values into a clean document.");
        const auto stable_history = document.undo_count();
        const auto diagnostic = workspace.view.lit.probe_debug;
        auto diagnostic_view = workspace.view;
        diagnostic_view.lit.probe_debug = !diagnostic;
        take(workspace.set_view_settings(document, diagnostic_view));
        require(document.undo_count() == stable_history && take(document.serialize()) == saved,
                "Viewport diagnostics entered the scene or Undo stack.");
        const auto saved_json = take(assets::json_format::parse(saved));
        require(saved_json.root().u32({"version"}) == 10 && !take(saved_json.root().find("world")) &&
                    !take(environment_record(saved_json.root()).find("direct_reference")) &&
                    !take(environment_record(saved_json.root()).find("probe_debug")),
                "Scene v10 kept a world section or saved viewport diagnostics.");
        // Unknown Environment data survives tolerant loading verbatim.
        {
            constexpr auto key = "probe_debug";
            json::Builder altered{saved_json};
            take(altered.set(environment_record(altered.root()), key, true));
            const auto bytes = json::write(altered.root());
            auto tolerated = take(Session::deserialize({bytes.data(), bytes.size()}, project));
            const auto retained = take(assets::json_format::parse(take(tolerated->serialize())));
            require(tolerated->load_issues().size() == 1 &&
                        tolerated->load_issues().front().kind == world::DiagnosticKind::unknown_field &&
                        environment_record(retained.root()).boolean({key}) == true,
                    "Tolerant loading did not keep an unknown World setting as saved Environment data.");
        }
        auto invalid = candidate;
        invalid.probe_volumes[0].maximum = invalid.probe_volumes[0].minimum;
        static_cast<void>(document.set_environment(invalid));
        require(settings(document) == candidate && document.undo_count() == stable_history,
                "Invalid World configuration partially published.");
        take(restored->start());
        take(restored->set_environment(candidate));
        static_cast<void>(restored->set_environment(before));
        require(settings(*restored) == candidate, "A running World changed its settings after a refused edit.");
        take(restored->stop());
        take(document.select(selected));
        workspace.dock.select(control::parameters_tab);
        publish();
        const auto entity = document.selection().entity;
        const auto root = entity;
        const auto key = workspace.field_control(root, document.types().position_fields[0]);
        take(workspace.apply_view(document, host, {ui::ActionKind::activate, control::group_categories, true}));
        publish();
        require(workspace.field_control(root, document.types().position_fields[0]) == key,
                "Category grouping lost field identity.");
    }
    std::puts(
        "Scene Lit application acceptance: PASS (World/Project, light history/persistence, DSL bindings, transactional volume edits, stale actions)");
}
