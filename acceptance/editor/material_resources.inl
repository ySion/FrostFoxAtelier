// Real Project/Source compiler/Runtime/Session/DSL/SceneSnapshot consumers.
// Arbitrary declarations deliberately differ from the installed StandardPBR MS.
constexpr std::string_view editor_material_source = R"(

struct Params { float gain; vector2 offset; vector3 tint; vector4 coat; };
struct Textures { material.TextureInput albedo; };
Params defaults() { Params p; p.gain=0.4; p.offset=vector2(0.0,0.0);
    p.tint=vector3(0.2,0.3,0.4); p.coat=vector4(0.1,0.2,0.3,0.7); return p; }
material.Output shade(material.Context c, Params p, Textures t) {
    vector4 color=sample(t.albedo,c.uv+p.offset); material.Output o;
    o.base_color=vector3(color.x*p.tint.x,color.y*p.tint.y,color.z*p.tint.z);
    o.emissive=vector3(p.coat.x,p.coat.y,p.coat.z); o.roughness=p.gain;
    o.metallic=p.coat.w; o.coverage=1.0; o.normal_world=c.normal_world; return o;
})";
material::LocalValue material_value(const editor::Session& session, ObjectId object, std::string_view path) {
    const auto view = take(session.inspect_material(object));
    const auto at = std::ranges::find(view.declarations, path, &editor::MaterialParameterView::path);
    require(at != view.declarations.end(), "Material declaration disappeared.");
    return at->value;
}

void edit_material_entry(editor::Session& session, ObjectId object, material::Override entry, std::uint64_t control) {
    take(session.begin_material_edit(object, control));
    take(session.preview_material_entry(control, std::move(entry)));
    take(session.commit_material_edit(control));
}

std::string create_editor_material_source(editor::Project& project) {
    const auto key =
        take(project.create_material_source("Content/Materials/Arbitrary.ms.material", editor_material_source));
    auto asset = take(project.assets().material(key));
    std::get<assets::MaterialSourceAsset>(asset.author).inputs = {
        {"albedo", {}, material::TextureColor::linear, {1, 1, 1, 1}}};
    take(project.save_material(asset));
    return key;
}

void verify_material_resources() {
    namespace fs = std::filesystem;
    const auto evidence = fs::absolute("out/material-resources-" + std::to_string(GetCurrentProcessId()) + "-" +
                                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto project = take(editor::Project::create(evidence / "Project"));
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
    take(editor::write_file(evidence / "Red.bmp", bmp));
    const auto texture = take(project.import_texture(evidence / "Red.bmp"));
    const auto source_key = create_editor_material_source(project);
    assets::MaterialAsset author;
    std::get<assets::MaterialInstanceAsset>(author.author).parent_key = source_key;
    const auto key = take(project.create_material("Content/Materials/./Unused/../Stone.material", author));
    const auto material_path = project.root() / "Content/Materials/Stone.material";
    auto metadata_path = material_path;
    metadata_path += L".asset.json";
    const auto material_bytes = take(assets::read_file(material_path));
    const auto metadata_bytes = take(assets::read_file(metadata_path));
    const auto catalog_revision = project.revision();
    require(take(editor::Project::open(project.root())).assets().material(key).has_value(),
            "Normalized material path did not survive reopening.");
    (void)project.create_material("Content/Materials/Unused/../Stone.material", author);
    require(project.revision() == catalog_revision && take(assets::read_file(material_path)) == material_bytes &&
                take(assets::read_file(metadata_path)) == metadata_bytes &&
                !fs::exists(project.root() / "Content/Materials/Unused"),
            "Rejected creation changed the file, identity or catalog.");
    (void)project.create_material("Content/../../Escaped.material", author);
    require(!fs::exists(evidence / "Escaped.material"), "Material creation escaped Content.");
    editor::Session document;
    take(document.initialize());
    auto& runtime = document.world().runtime();
    const auto resource = take(document.open_material(project, key));
    require(take(document.open_material(project, key)).id() == resource.id(),
            "Shared asset acquired another Runtime identity.");
    const auto inspection = take(document.inspect_material(resource.id()));
    require(!inspection.source && !inspection.scene && inspection.parent_key == source_key &&
                inspection.declarations.size() == 5 && inspection.source_file &&
                inspection.source_file->source == editor_material_source && !document.material_sources().empty(),
            "Owning inspection lost the MI role, arbitrary declarations or exact MS source text.");
    take(document.begin_material_edit(resource.id(), 101));
    take(document.preview_material_entry(101, {"gain", .8f, true}));
    require(std::get<float>(material_value(document, resource.id(), "gain")) == .8f, "Preview did not publish.");
    take(document.cancel_material_edit());
    require(take(document.inspect_material(resource.id())).entries.empty() && !document.material_dirty(key),
            "First preview cancel did not restore absence.");
    edit_material_entry(document, resource.id(), {"gain", .8f, true}, 102);
    require(document.material_dirty(key) && !document.dirty(), "Named MI editing dirtied the scene.");
    take(document.set_material_override(resource.id(), "gain", false));
    const auto dormant = take(document.inspect_material(resource.id()));
    require(!dormant.entries.front().enabled && std::get<float>(dormant.entries.front().value) == .8f &&
                std::get<float>(material_value(document, resource.id(), "gain")) == .4f,
            "Disabling an entry erased its value or failed inheritance.");
    take(document.material_undo(key));
    require(std::get<float>(material_value(document, resource.id(), "gain")) == .8f,
            "Named MI undo lost enabled intent.");
    take(document.material_undo(key, true));
    take(document.set_material_override(resource.id(), "gain", true));
    edit_material_entry(document, resource.id(), {"gain", math::Vector2{.1f, .2f}, true}, 103);
    require(
        std::get<float>(material_value(document, resource.id(), "gain")) == .4f &&
            !take(document.inspect_material(resource.id())).diagnostics.empty() &&
            std::holds_alternative<math::Vector2>(take(document.inspect_material(resource.id())).entries.front().value),
        "Type mismatch erased author data or did not diagnose/inherit.");
    take(document.material_undo(key));
    take(document.discard_material_override(resource.id(), "gain"));
    require(take(document.inspect_material(resource.id())).entries.empty(), "Discard left a local entry.");
    take(document.material_undo(key));
    require(std::get<float>(material_value(document, resource.id(), "gain")) == .8f,
            "Discard undo lost the author value.");
    take(document.set_material_texture(project, resource.id(), "albedo", texture));
    edit_material_entry(document, resource.id(), {"gain", .75f, true}, 105);
    // An out-of-band author change to another path must not be rolled back by parameter history.
    auto native = take(material::read_instance(runtime, resource.id()));
    native.entries.push_back({"tint", math::Vector3{.6f, .5f, .4f}, true});
    native.textures.emplace_back();
    take(material::update_instance(runtime, resource.id(), native));
    take(document.material_undo(key));
    require(std::get<float>(material_value(document, resource.id(), "gain")) == .8f &&
                std::get<math::Vector3>(material_value(document, resource.id(), "tint")) ==
                    math::Vector3{.6f, .5f, .4f},
            "Undo restored an entire stale material instead of one path.");
    take(document.save_material(project, key));
    require(!document.material_dirty(key), "Save did not mark the named MI author entries clean.");
    assets::MaterialAsset child_author;
    std::get<assets::MaterialInstanceAsset>(child_author.author).parent_key = key;
    const auto child_key = take(project.create_material("Content/Materials/Child.material", child_author));
    const auto child = take(document.open_material(project, child_key));
    require(!take(document.inspect_material(child.id())).scene &&
                take(material::read_instance(runtime, child.id())).parent.id() == resource.id(),
            "Named MI identity was inferred from parent presence.");
    changed(document.create_static_mesh(take(project.assets().mesh("builtin/cube"))));
    const auto entity_a = document.selection().entity;
    const auto a = component(document, entity_a, document.presentation_types().static_mesh_component);
    changed(document.create_static_mesh(take(project.assets().mesh("builtin/cube")), {2, 0, 0}));
    const auto entity_b = document.selection().entity;
    const auto b = component(document, entity_b, document.presentation_types().static_mesh_component);
    take(document.assign_material(project, a, "slot/0", child_key));
    take(document.assign_material(project, b, "slot/0", child_key));
    const auto count = document.undo_count();
    take(document.assign_material(project, a, "slot/0", child_key));
    require(document.undo_count() == count, "Idempotent binding created history.");
    const auto scene_instance = take(document.create_scene_material(a, "slot/0"));
    require(take(document.inspect_material(scene_instance.id())).scene &&
                take(material::read_instance(runtime, scene_instance.id())).parent.id() == child.id(),
            "Scene MI did not inherit the current binding.");
    const std::array shared{presentation::MaterialBinding{"slot/0", scene_instance, true}};
    take(presentation::set_material_bindings(runtime, b, shared));
    take(document.begin_material_edit(scene_instance.id(), 106));
    take(document.preview_material_entry(106, {"offset", math::Vector2{.2f, .3f}, true}));
    take(document.cancel_edit());
    require(take(document.inspect_material(scene_instance.id())).entries.empty(),
            "Scene first preview cancel kept a sparse entry.");
    edit_material_entry(document, scene_instance.id(), {"gain", .9f, true}, 107);
    const auto capture = [&](ObjectId mesh) { return take(presentation::material_surfaces(runtime, mesh))->front(); };
    require(take(capture(a)->evaluate({})).roughness == .9f && take(capture(b)->evaluate({})).roughness == .9f,
            "Shared scene MI was not editable.");
    changed(document.undo());
    require(take(capture(a)->evaluate({})).roughness == .8f, "Scene undo did not restore inherited current values.");
    changed(document.redo());
    application::SceneSnapshot snapshot;
    take(snapshot.refresh(document));
    edit_material_entry(document, resource.id(), {"tint", math::Vector3{.25f, .5f, .75f}, true}, 108);
    take(snapshot.refresh(document));
    require(take(snapshot_surface(snapshot)->evaluate({})).base_color.x == .25f,
            "An ancestor MI edit failed to reach SceneSnapshot.");
    take(document.save_material(project, key));
    const auto serialized = take(document.serialize());
    take(editor::write_file(project.root() / "Content/Scenes/Main.scene", serialized));
    auto reopened = take(editor::Session::deserialize(serialized, project, {}, editor::LoadPolicy::strict));
    const auto entities = take(reopened->world().members());
    const auto reopened_a =
        component(*reopened, entities[0].id(), reopened->presentation_types().static_mesh_component);
    const auto reopened_b =
        component(*reopened, entities[1].id(), reopened->presentation_types().static_mesh_component);
    const auto ra =
        take(presentation::read_material_bindings(reopened->world().runtime(), reopened_a)).front().resource;
    const auto rb =
        take(presentation::read_material_bindings(reopened->world().runtime(), reopened_b)).front().resource;
    require(ra.id() == rb.id() && take(reopened->inspect_material(ra.id())).scene &&
                std::get<float>(material_value(*reopened, ra.id(), "gain")) == .9f,
            "Scene round-trip lost shared MI identity or sparse intent.");
    take(document.select(entity_a));
    changed(document.duplicate_entities(document.selected_entities()));
    const auto duplicate =
        component(document, document.selection().entity, document.presentation_types().static_mesh_component);
    const auto copied = take(presentation::read_material_bindings(runtime, duplicate)).front().resource;
    require(copied.id() != scene_instance.id() &&
                take(material::read_instance(runtime, copied.id())).parent.id() == child.id(),
            "Duplicate copied a named ancestor or shared scene author entries.");
    take(document.clear_material(duplicate, "slot/0"));
    require(take(presentation::read_material_bindings(runtime, duplicate)).empty(),
            "Clear did not restore mesh defaults.");
    editor::Workspace workspace;
    workspace.project_open = true;
    workspace.assets.reset(true);
    workspace.inspecting_asset = true;
    workspace.material_inspection = key;
    const auto controls = take(workspace.describe(document, {1200, 800}, nullptr, &project));
    require(workspace.ui_error().empty() && !controls.empty(), "Arbitrary material DSL projection failed.");
    const auto bound = [&](std::string_view path, editor::Workspace::Binding::Kind kind) {
        return std::ranges::any_of(controls, [&](const auto& control) {
            const auto* binding = workspace.binding(control.key);
            return binding && binding->object == resource.id() && binding->material_path == path &&
                   binding->kind == kind;
        });
    };
    require(bound("gain", editor::Workspace::Binding::Kind::material_field) &&
                bound("offset", editor::Workspace::Binding::Kind::material_field),
            "UI did not create stable-path numeric/vector controls.");
    workspace.inspecting_asset = false;
    take(document.select(entity_a));
    take(workspace.describe(document, {1200, 800}, nullptr, &project));
    require(workspace.ui_error().empty(), "Scene MI projection failed.");
    const auto paused = take(material::capture_program(runtime, resource.id()));
    take(document.start());
    const auto revision = document.revision();
    static_cast<void>(document.material_undo(key));
    require(document.revision() == revision && take(material::capture_program(runtime, resource.id())) == paused,
            "Running World changed material history or its cursor.");
    take(document.stop());
    const auto surviving = capture(a);
    changed(document.close());
    collect(document);
    require(take(surviving->evaluate({})).roughness == .9f && surviving->inputs().front().image,
            "Closing a document invalidated its owning program capture.");

    // Publication retains identities and exact Source text, independent of newer disk content.
    {
        auto published = take(editor::Project::create(evidence / "Publication"));
        const auto ms = create_editor_material_source(published);
        auto ms_author = take(published.assets().material(ms));
        const auto source_path = std::get<assets::MaterialSourceAsset>(ms_author.author).source_path;
        auto exact = take(published.assets().source(source_path));
        editor::Session session;
        take(session.initialize());
        auto& r = session.world().runtime();
        const auto ms_root = take(session.material_resource(published.assets(), ms));
        const auto old_surface = take(material::capture_program(r, ms_root.id()));
        exact.source.replace(exact.source.find("p.gain=0.4"), 10, "p.gain=0.6");
        exact.version = atelier::source::content_hash(exact.source);
        take(editor::write_file(take(published.resolve(source_path)), "newer invalid source"));
        require(take(session.publish_material(published.assets(), ms_author, exact)).id() == ms_root.id() &&
                    take(session.inspect_material(ms_root.id())).source_file->source == exact.source &&
                    take(take(material::capture_program(r, ms_root.id()))->evaluate({})).roughness == .6f &&
                    take(old_surface->evaluate({})).roughness == .4f &&
                    take(assets::read_file(take(published.resolve(source_path)))) == "newer invalid source",
                "MS publication reread/wrote disk, replaced its root, or mutated an old capture.");
        static_cast<void>(session.publish_material(published.assets(), ms_author));
        require(std::get<float>(material_value(session, ms_root.id(), "gain")) == .6f,
                "Strict source failure published fallback over healthy content.");
        assets::MaterialAsset mi_author;
        std::get<assets::MaterialInstanceAsset>(mi_author.author).parent_key = ms;
        const auto mi = take(published.create_material("Content/Shared.material", mi_author));
        const auto mi_root = take(session.material_resource(published.assets(), mi));
        edit_material_entry(session, mi_root.id(), {"gain", .8f, true}, 301);
        mi_author = take(session.material_asset(mi_root.id()));
        std::get<assets::MaterialInstanceAsset>(mi_author.author)
            .entries.push_back({"offset", math::Vector2{.3f, .4f}, true});
        require(take(session.publish_material(published.assets(), mi_author)).id() == mi_root.id(),
                "MI publication replaced its root.");
        take(session.material_undo(mi));
        require(std::get<float>(material_value(session, mi_root.id(), "gain")) == .6f &&
                    std::get<math::Vector2>(material_value(session, mi_root.id(), "offset")) == math::Vector2{.3f, .4f},
                "Publication lost history or Undo rewound an unrelated published path.");
        changed(session.create_static_mesh(take(published.assets().mesh("builtin/cube"))));
        const auto mesh =
            component(session, session.selection().entity, session.presentation_types().static_mesh_component);
        const auto local = take(session.create_scene_material(mesh, "slot/0"));
        require(take(session.inspect_material(local.id())).parent_key == "builtin/material/default-grid" &&
                    !take(material::capture_program(r, local.id()))->uses_default_material(),
                "An unbound scene MI failed to inherit the normal world grid.");
        const auto grid = take(material::read_instance(r, local.id())).parent;
        const auto installed = take(published.assets().material("builtin/material/default-grid"));
        const auto installed_text = *take(session.inspect_material(grid.id())).source_file;
        const auto grid_surface = take(material::capture_program(r, grid.id()));
        const auto grid_revision = session.revision();
        auto changed_author = installed;
        auto& policy = std::get<assets::MaterialSourceAsset>(changed_author.author).policy;
        policy.double_sided = !policy.double_sided;
        static_cast<void>(session.publish_material(published.assets(), changed_author));
        require(session.loaded_material(installed.key) == grid.id() &&
                    take(material::capture_program(r, grid.id())) == grid_surface &&
                    session.revision() == grid_revision,
                "Builtin material tampering changed a loaded identity, author content or Source text.");
        require(take(session.publish_material(published.assets(), installed, installed_text)).id() == grid.id(),
                "The exact installed MS could not use the ordinary publication path.");
        assets::MaterialAsset sleeping;
        auto& entries = std::get<assets::MaterialInstanceAsset>(sleeping.author);
        entries.parent_key = ms;
        entries.entries = {{"albedo", material::TextureBinding{"project/sleeping"}, false},
                           {"removed", material::TextureBinding{"project/removed"}, true},
                           {"gain", material::TextureBinding{"project/retyped"}, true}};
        const auto sleepy_key = take(published.create_material("Content/Sleeping.material", sleeping));
        const auto sleepy = take(session.material_resource(published.assets(), sleepy_key));
        const auto sleepy_state = take(material::read_instance(r, sleepy.id()));
        require(sleepy_state.entries.size() == 3 &&
                    std::ranges::all_of(sleepy_state.textures, [](const auto& root) { return !root.registered(); }) &&
                    !session.unavailable(assets::AssetKind::texture, "project/sleeping") &&
                    !session.unavailable(assets::AssetKind::texture, "project/retyped"),
                "Dormant/deleted/retyped texture entries eagerly read invalid dependencies.");
        static_cast<void>(session.set_material_override(sleepy.id(), "albedo", true, &published));
        require(!take(session.inspect_material(sleepy.id())).entries.front().enabled,
                "Failed dormant texture activation changed its author intent.");
        const auto image = take(published.import_texture(evidence / "Red.bmp"));
        take(session.set_material_texture(published, sleepy.id(), "albedo", image));
        take(session.set_material_override(sleepy.id(), "albedo", false, &published));
        take(session.set_material_override(sleepy.id(), "albedo", true, &published));
        require(take(material::capture_program(r, sleepy.id()))->inputs().front().image != nullptr,
                "Re-enabled texture did not use its current Runtime root.");
    }

    // Reimport publishes current pixels on the same root, not an author-history snapshot.
    {
        auto imports = take(editor::Project::create(evidence / "Reimport"));
        const auto ms = create_editor_material_source(imports);
        const auto image_key = take(imports.import_texture(evidence / "Red.bmp"));
        assets::MaterialAsset asset;
        auto& instance = std::get<assets::MaterialInstanceAsset>(asset.author);
        instance.parent_key = ms;
        instance.entries = {{"albedo", material::TextureBinding{image_key}, true}};
        const auto material_key = take(imports.create_material("Content/Shared.material", asset));
        editor::Session session;
        take(session.initialize());
        auto& r = session.world().runtime();
        const auto root = take(session.open_material(imports, material_key));
        changed(session.create_static_mesh(take(imports.assets().mesh("builtin/cube"))));
        const auto mesh =
            component(session, session.selection().entity, session.presentation_types().static_mesh_component);
        take(session.assign_material(imports, mesh, "slot/0", material_key));
        edit_material_entry(session, root.id(), {"gain", .23f, true}, 901);
        take(session.save_material(imports, material_key));
        const auto texture_root = take(material::read_instance(r, root.id())).textures.front();
        auto previous = take(material::capture_program(r, root.id()));
        std::weak_ptr<const material::Image> old_pixels = previous->inputs().front().image;
        const auto scene = take(session.serialize());
        const auto history = session.undo_count();
        auto metadata = take(imports.resolve(imports.entry(image_key)->path));
        metadata += L".asset.json";
        const auto identity = take(assets::read_file(metadata));
        application::SceneSnapshot view;
        take(view.refresh(session));
        auto blue = bmp;
        for (std::size_t i = 54; i < blue.size(); i += 4) {
            blue[i] = char(255);
            blue[i + 2] = 0;
        }
        take(editor::write_file(evidence / "Blue.bmp", blue));
        take(session.reimport_texture(imports, image_key, evidence / "Blue.bmp"));
        take(view.refresh(session));
        const auto pixels = snapshot_surface(view)->inputs().front().image->pixels();
        require(take(material::read_instance(r, root.id())).textures.front().id() == texture_root.id() &&
                    pixels[0] == 0 && pixels[2] == 255 &&
                    take(imports.assets().texture(image_key)).image->pixels()[2] == 255 &&
                    take(assets::read_file(metadata)) == identity && session.undo_count() == history &&
                    take(session.serialize()) == scene && !session.material_dirty(material_key),
                "Reimport changed identity/history or did not refresh the displayed program image.");
        require(previous->inputs().front().image->pixels()[0] == 255, "Reimport mutated an in-flight capture.");
        static_cast<void>(take(material::capture_program(r, root.id())));
        previous.reset();
        require(old_pixels.expired(), "Parameter history retained old image content.");
        take(session.material_undo(material_key));
        take(view.refresh(session));
        require(take(snapshot_surface(view)->evaluate({})).roughness == .4f &&
                    snapshot_surface(view)->inputs().front().image->pixels()[2] == 255,
                "Parameter Undo reverted imported pixels.");
        take(session.material_undo(material_key, true));
        take(editor::write_file(evidence / "Broken.bmp", "not an image"));
        static_cast<void>(session.reimport_texture(imports, image_key, evidence / "Broken.bmp"));
        require(take(assets::read_file(take(imports.resolve(imports.entry(image_key)->path)))) == blue,
                "Failed reimport damaged the committed image.");
        take(editor::write_file(take(imports.resolve(imports.entry(image_key)->path)), "broken"));
        auto broken = take(editor::Project::open(imports.root()));
        auto repaired = take(editor::Session::deserialize(scene, broken));
        require(repaired->unavailable(assets::AssetKind::texture, image_key) != nullptr,
                "Repair fixture did not load a placeholder.");
        const auto repaired_root = take(repaired->open_material(broken, material_key));
        const auto placeholder =
            take(material::read_instance(repaired->world().runtime(), repaired_root.id())).textures.front();
        take(repaired->reimport_texture(broken, image_key, evidence / "Blue.bmp"));
        require(
            !repaired->unavailable(assets::AssetKind::texture, image_key) &&
                take(material::read_instance(repaired->world().runtime(), repaired_root.id())).textures.front().id() ==
                    placeholder.id() &&
                take(material::capture_program(repaired->world().runtime(), repaired_root.id()))
                        ->inputs()
                        .front()
                        .image->pixels()[2] == 255,
            "Reimport did not repair the same placeholder root.");
    }
    // Cold invalid Source must not strand an enabled MI texture after only the Source is repaired.
    {
        auto recovery = take(editor::Project::create(evidence / "SourceRecovery"));
        const auto ms = create_editor_material_source(recovery);
        const auto ms_author = take(recovery.assets().material(ms));
        const auto source_path = std::get<assets::MaterialSourceAsset>(ms_author.author).source_path;
        const auto exact = take(recovery.assets().source(source_path));
        const auto image = take(recovery.import_texture(evidence / "Red.bmp"));
        assets::MaterialAsset asset;
        auto& author = std::get<assets::MaterialInstanceAsset>(asset.author);
        author.parent_key = ms;
        author.entries = {{"albedo", material::TextureBinding{image}, true}};
        const auto mi = take(recovery.create_material("Content/Recovery.material", asset));
        author.parent_key = mi;
        author.entries.clear();
        const auto child = take(recovery.create_material("Content/Child.material", asset));
        const auto blue = take(recovery.import_texture(evidence / "Blue.bmp"));
        editor::Session original;
        take(original.initialize());
        changed(original.create_static_mesh(take(recovery.assets().mesh("builtin/cube"))));
        const auto mesh =
            component(original, original.selection().entity, original.presentation_types().static_mesh_component);
        take(original.assign_material(recovery, mesh, "slot/0", child));
        const auto local = take(original.create_scene_material(mesh, "slot/0"));
        take(original.set_material_texture(recovery, local.id(), "albedo", blue));
        const auto saved = take(original.serialize());
        take(editor::write_file(take(recovery.resolve(source_path)), "not source"));
        auto loaded = take(editor::Session::deserialize(saved, recovery));
        auto& session = *loaded;
        auto& r = session.world().runtime();
        const auto root = take(session.open_material(recovery, mi));
        const auto child_root = take(session.open_material(recovery, child));
        const auto restored_mesh = component(session, take(session.world().members()).front().id(),
                                             session.presentation_types().static_mesh_component);
        const auto restored_local = take(presentation::read_material_bindings(r, restored_mesh)).front().resource;
        require(take(material::capture_program(r, root.id()))->uses_default_material(),
                "Cold Source failure did not reach the recovery fixture.");
        edit_material_entry(session, root.id(), {"gain", .67f, true}, 902);
        edit_material_entry(session, restored_local.id(), {"offset", math::Vector2{.1f, .2f}, true}, 903);
        const auto before = assets::material_format::encode(take(session.material_asset(root.id())));
        const auto scene_before = take(session.serialize());
        const auto history = session.undo_count();
        take(editor::write_file(take(recovery.resolve(source_path)), exact.source));
        std::vector<editor::MissingResource> missing;
        take(session.publish_material(recovery.assets(), ms_author, exact, &missing));
        editor::Session reopened;
        take(reopened.initialize());
        const auto reopened_root = take(reopened.open_material(recovery, mi));
        const auto control = take(material::capture_program(reopened.world().runtime(), reopened_root.id()));
        require(control->inputs().front().image && take(control->evaluate({})).base_color == math::Vector3{.2f, 0, 0},
                "Reopened Source recovery control did not sample the real red image.");
        const auto repaired = take(material::capture_program(r, root.id()));
        require(!repaired->uses_default_material() && repaired->inputs().front().image &&
                    take(repaired->evaluate({})).base_color == math::Vector3{.2f, 0, 0},
                "Repairing only a cold-invalid MS left its enabled MI texture unbound until reopen.");
        require(take(take(material::capture_program(r, child_root.id()))->evaluate({})).base_color ==
                        math::Vector3{.2f, 0, 0} &&
                    take(take(material::capture_program(r, restored_local.id()))->evaluate({})).base_color ==
                        math::Vector3{0, 0, .4f},
                "Source repair did not reassemble both named descendants and scene-local overrides.");
        const auto after = assets::material_format::encode(take(session.material_asset(root.id())));
        require(missing.empty() && before && after && *after == *before && session.material_dirty(mi) &&
                    take(session.serialize()) == scene_before && session.undo_count() == history,
                "Source repair changed unsaved author entries, dirty state or scene history.");
        take(session.material_undo(mi));
        changed(session.undo());
        require(take(take(material::capture_program(r, root.id()))->evaluate({})).roughness == .4f &&
                    take(take(material::capture_program(r, restored_local.id()))->evaluate({})).base_color ==
                        math::Vector3{0, 0, .4f},
                "Source repair replaced parameter history or Undo lost a recovered texture.");
        {
            take(editor::write_file(take(recovery.resolve(source_path)), "not source"));
            auto history = take(editor::Session::deserialize(saved, recovery));
            auto& runtime = history->world().runtime();
            const auto history_mesh = component(*history, take(history->world().members()).front().id(),
                                                history->presentation_types().static_mesh_component);
            const auto edited = take(presentation::read_material_bindings(runtime, history_mesh)).front().resource;
            take(history->set_material_override(edited.id(), "albedo", false, &recovery));
            take(editor::write_file(take(recovery.resolve(source_path)), exact.source));
            take(history->publish_material(recovery.assets(), ms_author, exact));
            changed(history->undo(&recovery));
            require(take(take(material::capture_program(runtime, edited.id()))->evaluate({})).base_color ==
                        math::Vector3{0, 0, .4f},
                    "Undo after cold Source repair did not resolve a texture disabled while the Source was invalid.");
            changed(history->redo(&recovery));
            require(take(take(material::capture_program(runtime, edited.id()))->evaluate({})).base_color ==
                        math::Vector3{.2f, 0, 0},
                    "Redo did not restore inherited shading after the recovered texture override.");
            take(history->reimport_texture(recovery, blue, evidence / "Red.bmp"));
            changed(history->undo(&recovery));
            require(take(take(material::capture_program(runtime, edited.id()))->evaluate({})).base_color ==
                        math::Vector3{.2f, 0, 0},
                    "Texture override Undo restored historical pixels instead of the current reimport.");
        }
    }
    std::puts("Material resources acceptance: PASS");
}

void verify_missing_resources() {
    namespace fs = std::filesystem;
    const auto evidence = fs::absolute("out/missing-resources-" + std::to_string(GetCurrentProcessId()) + "-" +
                                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto project = take(editor::Project::create(evidence / "Project"));
    const auto root = project.root();
    const auto ms = create_editor_material_source(project);
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
    take(editor::write_file(evidence / "Red.bmp", bmp));
    const auto texture = take(project.import_texture(evidence / "Red.bmp"));
    assets::MaterialAsset asset;
    auto& author = std::get<assets::MaterialInstanceAsset>(asset.author);
    author.parent_key = ms;
    author.entries = {{"albedo", material::TextureBinding{texture}, true}, {"gain", .6f, true}};
    const auto stone = take(project.create_material("Content/Stone.material", asset));
    author.entries.clear();
    const auto brick = take(project.create_material("Content/Brick.material", asset));
    take(editor::write_file(evidence / "Wall.mtl", "newmtl wall\nKd 1 1 1\n"));
    take(editor::write_file(evidence / "Wall.obj",
                            "mtllib Wall.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl wall\nf 1 2 3\n"));
    const auto wall = take(project.assets().mesh(take(project.import_mesh(evidence / "Wall.obj"))));
    const auto wall_slot = presentation::material_slot_name(*wall.mesh, wall.mesh->parts().front().material);
    editor::Session document;
    take(document.initialize());
    changed(document.create_static_mesh(take(project.assets().mesh("builtin/cube"))));
    const auto textured =
        component(document, document.selection().entity, document.presentation_types().static_mesh_component);
    take(document.assign_material(project, textured, "slot/0", stone));
    changed(document.create_static_mesh(take(project.assets().mesh("builtin/cube")), {2, 0, 0}));
    const auto instanced =
        component(document, document.selection().entity, document.presentation_types().static_mesh_component);
    take(document.assign_material(project, instanced, "slot/0", brick));
    const auto instance = take(document.create_scene_material(instanced, "slot/0"));
    edit_material_entry(document, instance.id(), {"gain", .8f, true}, 201);
    changed(document.create_static_mesh(wall, {4, 0, 0}));
    const auto walled =
        component(document, document.selection().entity, document.presentation_types().static_mesh_component);
    take(document.assign_material(project, walled, wall_slot, stone));
    physics::BodySettings body;
    body.shape = physics::ShapeKind::triangle_mesh;
    take(document.configure_body(walled, body));
    const auto saved = take(document.serialize());
    const auto backup = evidence / "Backup";
    fs::create_directory(backup);
    std::vector<std::pair<fs::path, fs::path>> moved;
    for (const auto& key : {texture, brick, wall.key}) {
        const auto relative = project.entry(key)->path;
        auto metadata = relative;
        metadata += L".asset.json";
        for (const auto& file : {relative, metadata}) {
            moved.emplace_back(root / file, backup / file.filename());
            fs::rename(root / file, backup / file.filename());
        }
    }
    auto damaged = take(editor::Project::open(root));
    auto kept = take(editor::Session::deserialize(saved, damaged));
    auto& runtime = kept->world().runtime();
    require(take(kept->serialize()) == saved && !kept->load_issues().empty(),
            "Tolerant load rewrote author references or hid missing diagnostics.");
    const auto entities = take(kept->world().members());
    const auto part = [&](std::size_t i) {
        return component(*kept, entities[i].id(), kept->presentation_types().static_mesh_component);
    };
    const auto shared = take(presentation::read_material_bindings(runtime, part(0))).front().resource;
    const auto shared_state = take(material::read_instance(runtime, shared.id()));
    require(take(material::read_texture(runtime, shared_state.textures.front().id())).source == texture &&
                !take(material::read_texture(runtime, shared_state.textures.front().id())).image &&
                kept->unavailable(assets::AssetKind::texture, texture) &&
                std::get<float>(material_value(*kept, shared.id(), "gain")) == .6f,
            "Missing image lost its key or unrelated MI parameters.");
    const auto failed = take(presentation::read_material_bindings(runtime, part(1))).front().resource;
    const auto failed_state = take(material::read_instance(runtime, failed.id()));
    require(take(material::capture_program(runtime, failed.id()))->uses_default_material() &&
                failed_state.entries.size() == 1 &&
                take(material::material_key(runtime, failed_state.parent.id())) == brick &&
                take(kept->inspect_material(failed.id())).parent_key == brick,
            "Missing parent did not retain its key and entries with the default surface.");
    const auto missing_parent = damaged.assets().material(brick);
    const auto cached_parent = kept->material_resource(damaged.assets(), brick);
    require(!missing_parent && !cached_parent && cached_parent.error().code == missing_parent.error().code &&
                cached_parent.error().message == missing_parent.error().message,
            "Cached strict material access replaced the Reader's original Error.");
    require(std::ranges::any_of(
                kept->load_issues(),
                [&](const world::Diagnostic& issue) { return issue.message == missing_parent.error().message; }),
            "Tolerant scene loading replaced the Reader's original Error body.");
    edit_material_entry(*kept, shared.id(), {"gain", .5f, true}, 203);
    take(kept->save_material(damaged, stone));
    require(std::get<material::TextureBinding>(
                std::get<assets::MaterialInstanceAsset>(take(damaged.assets().material(stone)).author)
                    .entries.front()
                    .value)
                    .asset_key == texture,
            "Saving dropped an unresolved texture author key.");
    require(!take(presentation::read_static_mesh(runtime, part(2))).mesh.resolved && kept->has_retained(part(2)) &&
                !take(physics::read_settings(runtime, part(2))).cooked_shape,
            "Missing mesh did not retain bindings or skip collision cook.");
    const auto before_discard = take(kept->serialize());
    take(kept->discard_retained(part(2)));
    changed(kept->undo());
    require(take(kept->serialize()) == before_discard, "Discard undo lost missing-mesh data.");
    take(kept->select(entities[2].id()));
    changed(kept->duplicate_entities(kept->selected_entities()));
    take(editor::Session::deserialize(take(kept->serialize()), damaged));
    for (const auto& [original, aside] : moved)
        fs::rename(aside, original);
    auto repaired = take(editor::Project::open(root));
    auto restored = take(editor::Session::deserialize(saved, repaired, {}, editor::LoadPolicy::strict));
    const auto restored_entities = take(restored->world().members());
    const auto restored_wall =
        component(*restored, restored_entities[2].id(), restored->presentation_types().static_mesh_component);
    require(take(presentation::read_material_bindings(restored->world().runtime(), restored_wall)).size() == 1 &&
                take(physics::read_settings(restored->world().runtime(), restored_wall)).cooked_shape,
            "Restoring assets failed to resolve bindings or rebuild collision.");
    const auto broken_ms = take(repaired.create_material_source("Content/Broken.ms.material", "not source"));
    editor::Session compiler_failure;
    take(compiler_failure.initialize());
    const auto broken_root = take(compiler_failure.open_material(repaired, broken_ms));
    const auto broken_view = take(compiler_failure.inspect_material(broken_root.id()));
    require(broken_view.default_material && !broken_view.diagnostics.empty() && broken_view.source_file &&
                broken_view.source_file->source == "not source",
            "MS compile failure lost exact source/diagnostic or failed to use the default surface.");
    editor::Session strict_compiler;
    take(strict_compiler.initialize());
    const auto compile_error = strict_compiler.material_resource(repaired.assets(), broken_ms);
    require(!compile_error && !compiler_failure.material_resource(repaired.assets(), broken_ms),
            "Strict material loading accepted fresh or cached compile fallback.");
    const auto opened_diagnostics = compiler_failure.take_diagnostics();
    require(std::ranges::any_of(opened_diagnostics,
                                [&](const auto& item) {
                                    return item.severity == world::Severity::warning &&
                                           item.object == broken_root.id() &&
                                           item.error_code == compile_error.error().code &&
                                           item.message == compile_error.error().message && !item.source &&
                                           !compile_error.error().source;
                                }) &&
                !compiler_failure.dirty(),
            "Opening a standalone fallback material did not publish the original warning for Project Output.");
    const auto broken_author = take(repaired.assets().material(broken_ms));
    const auto source_path = std::get<assets::MaterialSourceAsset>(broken_author.author).source_path;
    fs::remove(take(repaired.resolve(source_path)));
    const auto read_error = repaired.assets().source(source_path);
    require(!read_error, "Deleted material source remained readable.");
    editor::Session strict_missing;
    take(strict_missing.initialize());
    editor::Session tolerant_missing;
    take(tolerant_missing.initialize());
    const auto fresh_error = strict_missing.material_resource(repaired.assets(), broken_ms);
    std::vector<editor::MissingResource> absent_source;
    take(tolerant_missing.material_resource(repaired.assets(), broken_ms, &absent_source));
    const auto cached_error = tolerant_missing.material_resource(repaired.assets(), broken_ms);
    require(!fresh_error && !cached_error && fresh_error.error().code == read_error.error().code &&
                cached_error.error().code == read_error.error().code &&
                fresh_error.error().message == read_error.error().message &&
                cached_error.error().message == read_error.error().message,
            "Fresh or cached material loading replaced the Reader's original Error with a generic fallback.");
    std::puts("Missing resources acceptance: PASS");
}
