// Included by project_execution.cpp: the editor-to-Play change link.
using LinkJson = assets::json_format::Json;
// Plays the other end of a link: queues what arrives so tests read one message at a time.
struct LinkPeer {
    std::unique_ptr<application::LiveLink> link;
    std::deque<std::string> arrived;
    LinkJson next(application::PlayProcess* process = nullptr) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (arrived.empty()) {
            for (auto& text : link->receive())
                arrived.push_back(std::move(text));
            if (process)
                process->poll();
            require(std::chrono::steady_clock::now() < until, "The link peer received nothing.");
            if (arrived.empty())
                Sleep(2);
        }
        auto text = std::move(arrived.front());
        arrived.pop_front();
        return LinkJson::parse(text);
    }
    bool idle() {
        Sleep(50);
        for (auto& text : link->receive())
            arrived.push_back(std::move(text));
        return arrived.empty();
    }
    void send(const LinkJson& message) { link->send(message.dump()); }
};
std::pair<std::unique_ptr<application::LiveLink>, LinkPeer> link_pair() {
    HANDLE editor_read{}, peer_write{}, peer_read{}, editor_write{};
    require(CreatePipe(&editor_read, &peer_write, nullptr, 1 << 20) &&
                CreatePipe(&peer_read, &editor_write, nullptr, 1 << 20),
            "Cannot create link pipes.");
    return {std::make_unique<application::LiveLink>(editor_read, editor_write),
            LinkPeer{std::make_unique<application::LiveLink>(peer_read, peer_write), {}}};
}
LinkJson ack(std::uint64_t seq, std::string_view severity = {}) {
    LinkJson message = {{"type", "ack"}, {"seq", seq}, {"diagnostics", LinkJson::array()}};
    if (!severity.empty())
        message["diagnostics"].push_back(
            {{"severity", severity}, {"kind", "command_rejected"}, {"message", "Play change 0 #7: rejected."}});
    return message;
}
void settle(application::PlayLink& link, std::size_t pending = 0) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (link.unacknowledged() != pending && link.divergence().empty()) {
        link.poll(true, false);
        require(std::chrono::steady_clock::now() < until, "The editor end did not read its acknowledgements.");
        Sleep(2);
    }
}
// The editor end in-process against a scripted peer: numbering, spilling, splitting and divergence.
void verify_link_protocol(const std::filesystem::path& root) {
    const auto spill = root / "Cache/Play/link-protocol";
    const editor::LiveChange name_op{R"({"op":"set","id":"7","name":"A"})", {}},
        remove_op{R"({"op":"remove","id":"8"})", {}};
    const auto large_op = [](std::size_t bytes) {
        return editor::LiveChange{R"({"op":"set","id":"7","name":")" + std::string(bytes, 'a') + R"("})", {}};
    };
    {
        auto [end, peer] = link_pair();
        application::PlayLink link;
        link.open(std::move(end), spill);
        link.forward({name_op, remove_op});
        link.forward({});
        const auto batch = peer.next();
        require(batch.at("type") == "batch" && batch.at("seq") == 1 && batch.at("ops").size() == 2 &&
                    batch.at("ops")[1].at("op") == "remove" && link.unacknowledged() == 1 && peer.idle(),
                "One step's ops did not travel as one numbered batch.");
        // Warnings are Play's business; only an error acknowledgement is divergence.
        peer.send(ack(1, "warning"));
        settle(link);
        require(link.divergence().empty(), "A warning acknowledgement diverged.");
        const auto five = large_op(5 << 20), nine = large_op(9 << 20);
        link.forward({five});
        const auto spilled = peer.next();
        require(spilled.at("seq") == 2 && spilled.contains("file") && !spilled.contains("ops"),
                "A batch over one message was not spilled to a file.");
        const std::filesystem::path file = assets::wide(spilled.at("file").get<std::string>());
        require(file.parent_path() == spill && take(assets::read_file(file)) == R"({"ops":[)" + five.text + "]}",
                "The spilled file is not the batch Play applies.");
        // Past the 16 MiB batch limit the ops split at their boundary into consecutive batches.
        link.forward({nine, nine});
        const auto first = peer.next(), second = peer.next();
        require(first.at("seq") == 3 && second.at("seq") == 4 && link.unacknowledged() == 3 &&
                    take(assets::read_file(assets::wide(second.at("file").get<std::string>()))) ==
                        R"({"ops":[)" + nine.text + "]}",
                "Oversized ops were not split into consecutive batches.");
        peer.send(ack(2));
        peer.send(ack(3));
        peer.send(ack(4));
        settle(link);
        require(std::filesystem::is_empty(spill), "The sender retained acknowledged batch files.");
        link.forward({name_op});
        require(peer.next().at("seq") == 5, "Batches lost their numbering.");
        peer.send(ack(5, "error"));
        settle(link);
        require(link.divergence() == "Play change 0 #7: rejected." && link.unacknowledged() == 0,
                "An error acknowledgement did not diverge.");
        link.forward({name_op});
        require(peer.idle() && link.unacknowledged() == 0, "A diverged link kept sending.");
        require(!link.close() && !std::filesystem::exists(spill), "Closing the link left its spill directory.");
    }
    const auto diverges = [&](auto&& act, std::string_view expected) {
        auto [end, peer] = link_pair();
        application::PlayLink link;
        link.open(std::move(end), spill);
        act(link, peer);
        require(link.divergence().find(expected) != std::string::npos, "A broken link did not diverge.");
        static_cast<void>(link.close());
    };
    diverges(
        [&](application::PlayLink& link, LinkPeer& peer) {
            link.forward({name_op});
            link.forward({name_op});
            peer.send(ack(2));
            settle(link, 1);
        },
        "out of order");
    diverges(
        [&](application::PlayLink& link, LinkPeer& peer) {
            for (std::size_t i = 0; i <= application::PlayLink::maximum_pending; ++i)
                link.forward({name_op});
            require(link.unacknowledged() == application::PlayLink::maximum_pending + 1, "Backpressure stopped early.");
            link.forward({name_op});
            static_cast<void>(peer);
        },
        "stopped acknowledging");
    diverges(
        [&](application::PlayLink& link, LinkPeer& peer) {
            link.forward({name_op, large_op(application::PlayLink::maximum_batch)});
            require(link.unacknowledged() == 0 && peer.idle(),
                    "A step with an op past the batch limit was partly sent.");
        },
        "exceeds the 16 MiB");
    // A child closes its pipes before it exits: only a link that stays closed while Play runs diverges.
    diverges(
        [&](application::PlayLink& link, LinkPeer& peer) {
            peer.link.reset();
            Sleep(100);
            link.poll(true, false);
            link.poll(false, false);
            Sleep(2100);
            link.poll(true, true);
            link.poll(true, false);
            require(link.divergence().empty(), "A closing child diverged within its grace period.");
            Sleep(2100);
            link.poll(true, false);
        },
        "closed while Play kept running");
}
// Real Session publications through the actual pipe/spill/receiver/ACK path.
void verify_texture_link(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    auto project = take(editor::Project::create(root));
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
    const auto source = root / "source.bmp";
    take(editor::write_file(source, bmp));
    const auto key = take(project.import_texture(source));
    assets::MaterialAsset asset;
    std::get<assets::MaterialInstanceAsset>(asset.author)
        .entries.push_back({"base_color_texture", material::TextureBinding{key}, true});
    const auto material_key = take(project.create_material("Content/Shared.material", asset));
    editor::Session author, play;
    play.enter_play();
    auto play_project = take(editor::Project::open(root));
    const auto author_root = take(author.open_material(project, material_key)),
               play_root = take(play.open_material(play_project, material_key));
    const auto capture = [&] { return take(material::capture_program(play.world().runtime(), play_root.id())); };
    const auto input = [](const material::ProgramSurfaceRef& surface) {
        const auto declarations = surface->program().textures();
        const auto at =
            std::ranges::find(declarations, std::string("base_color_texture"), &source::MaterialTextureInput::key);
        require(at != declarations.end(), "The StandardPBR base-color input is missing.");
        return surface->inputs()[static_cast<std::size_t>(at - declarations.begin())].image;
    };
    auto play_before = capture();
    auto [end, peer] = link_pair();
    application::PlayLink link;
    const auto spill = root / "Cache/Play/textures";
    link.open(std::move(end), spill);
    application::PlayEnd receiver{std::move(peer.link)};
    author.set_live_forwarding(true);
    const auto current = [&] { return input(capture()); };
    std::vector<world::Diagnostic> issues;
    const auto service = [&] {
        receiver.service(play, play_project, [&](std::span<const world::Diagnostic> batch) {
            issues.insert(issues.end(), batch.begin(), batch.end());
        });
    };
    const auto flush = [&] {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (link.unacknowledged() && link.divergence().empty()) {
            service();
            link.poll(true, false);
            require(std::chrono::steady_clock::now() < until, "Resource publication was not acknowledged.");
            Sleep(2);
        }
        require(issues.empty() && link.divergence().empty(), "Resource publication diverged.");
        require(fs::is_empty(spill), "Acknowledged resource content remained as asset history.");
    };
    auto blue = bmp;
    for (std::size_t i = 54; i < blue.size(); i += 4) {
        blue[i] = char(255);
        blue[i + 2] = 0;
    }
    take(editor::write_file(source, blue));
    take(author.reimport_texture(project, key, source));
    link.forward(take(author.take_live_ops()));
    // The receiver must not reopen this now-different author file or cached Reader.
    take(editor::write_file(take(project.resolve(project.entry(key)->path)), "changed after publication"));
    require(link.unacknowledged() == 1 && !fs::is_empty(spill), "Texture transfer was not retained before ACK.");
    flush();
    require(current()->pixels()[2] == 255 && input(play_before)->pixels()[0] == 255,
            "Live reread latest disk or changed an old content owner.");
    play_before.reset();
    // More than a full JSON batch: bytes travel separately, command remains tiny.
    std::vector<std::uint8_t> pixels(2304 * 2304 * 4, 0);
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i] = 17;
        pixels[i + 1] = 193;
        pixels[i + 2] = 71;
        pixels[i + 3] = 255;
    }
    pixels[4] = 29;
    pixels[pixels.size() - 2] = 211;
    auto large = std::make_shared<const material::Image>(2304, 2304, std::move(pixels));
    take(author.publish_texture(key, large));
    auto changes = take(author.take_live_ops());
    require(changes.size() == 1 && changes[0].text.size() < 1024 && changes[0].texture == large &&
                large->pixels().size() > application::PlayLink::maximum_batch,
            "Large texture fixture did not cross the command limit.");
    link.forward(changes);
    changes.clear();
    flush();
    const auto delivered = current();
    require(delivered->width() == 2304 && std::ranges::equal(delivered->pixels(), large->pixels()),
            "Large Live texture lost exact RGBA content.");
    take(author.begin_material_edit(author_root.id(), 992));
    take(author.preview_material_entry(992, {"roughness", .31f, true}));
    take(author.commit_material_edit(992));
    changes = take(author.take_live_ops());
    require(changes.size() == 1 && !changes[0].texture, "A parameter edit retransmitted unchanged image bytes.");
    link.forward(changes);
    flush();
    require(current() == delivered && take(capture()->evaluate({})).roughness == .31f,
            "Parameter update replaced the adopted texture or failed to update Live.");
    take(author.material_undo(material_key));
    link.forward(take(author.take_live_ops()));
    flush();
    require(current() == delivered && take(capture()->evaluate({})).roughness == .65f,
            "Parameter Undo revived old Live pixels.");

    const auto mesh_source = root / "shape.obj";
    const std::string triangle =
        "v -0.0 0 0\nv 1 0 0\nv 0 2 0\nvt 0.1 0.2\nvt 0.8 0.1\nvt 0.3 0.9\nvn 1 2 3\nf 1/1/1 2/2/1 3/3/1\n";
    take(editor::write_file(mesh_source, triangle));
    const auto mesh_key = take(project.import_mesh(mesh_source));
    complete(author.create_static_mesh(take(project.assets().mesh(mesh_key)), {}, &project));
    link.forward(take(author.take_live_ops()));
    flush();
    const auto component = take(world::components(play.world().runtime(), entity(play))).front().id();
    const auto current_mesh = [&] {
        return take(presentation::read_static_mesh(play.world().runtime(), component)).mesh.resolved;
    };
    const auto old_mesh = current_mesh();
    const auto history = author.undo_count();
    auto changed = triangle;
    changed.replace(changed.find("v 1 0 0"), 7, "v 3 0 0");
    take(editor::write_file(mesh_source, changed));
    take(author.reimport_mesh(project, mesh_key, mesh_source));
    const auto imported = take(project.assets().mesh(mesh_key));
    changes = take(author.take_live_ops());
    require(changes.size() == 1 && changes[0].mesh && !changes[0].texture,
            "Mesh Reimport did not publish a complete separate snapshot.");
    link.forward(changes);
    changes.clear();
    take(editor::write_file(take(project.resolve(project.entry(mesh_key)->path)), "changed after mesh publication"));
    flush();
    require(author.undo_count() == history && old_mesh->bounds().maximum[0] == 1 &&
                current_mesh()->bounds().maximum[0] == 3 &&
                take(assets::mesh_content_sha256({mesh_key, {}, current_mesh(), {}})) ==
                    take(assets::mesh_content_sha256(imported)),
            "Live mesh reread disk, changed float bits, entered Undo or mutated old geometry.");

    // Exercise the receiver's >16 MiB path and a nontrivial published tangent frame.
    std::vector<geometry::Vertex> vertices;
    vertices.reserve(360000);
    for (unsigned i = 0; i < 120000; ++i)
        vertices.insert(vertices.end(), imported.mesh->vertices().begin(), imported.mesh->vertices().end());
    auto published = imported;
    published.materials = {{"slot/0", material_key}};
    published.mesh = geometry::Mesh::restore_published(std::move(vertices), std::vector<std::uint8_t>(120000, 5), {{}},
                                                       {{0, 360000, 0}});
    require(published.mesh->vertices().size() * sizeof(geometry::Vertex) > application::PlayLink::maximum_batch,
            "Mesh fixture did not exceed JSON limits.");
    take(author.publish_mesh(project.assets(), published));
    link.forward(take(author.take_live_ops()));
    flush();
    const auto received = take(play_project.assets().mesh(mesh_key));
    require(current_mesh() == received.mesh && received.materials == published.materials &&
                take(assets::mesh_content_sha256(received)) == take(assets::mesh_content_sha256(published)),
            "Large Live mesh changed geometry bits, edge masks, slots or default identities.");
    take(play_project.refresh());
    require(take(play_project.assets().mesh(mesh_key)).mesh == received.mesh,
            "Catalogue refresh replaced the accepted mesh with later disk data.");

    take(author.publish_texture(key, large));
    take(author.publish_texture(key, large));
    changes = take(author.take_live_ops());
    require(changes.size() == 1, "Unsent texture publications did not coalesce.");
    take(author.publish_mesh(project.assets(), published));
    auto mesh_changes = take(author.take_live_ops());
    changes.insert(changes.end(), mesh_changes.begin(), mesh_changes.end());
    link.forward(changes);
    require(!fs::is_empty(spill), "Pending resource content disappeared early.");
    std::filesystem::path mesh_file;
    for (const auto& file : fs::directory_iterator(spill))
        if (file.path().extension() == L".mesh")
            mesh_file = file.path();
    require(!mesh_file.empty(), "Mixed resource publication omitted its mesh payload.");
    const auto rejected = [&](std::string asset_key) {
        const auto diagnostics = take(play.apply_live(
            LinkJson{{"ops", LinkJson::array(
                                 {{{"op", "mesh"}, {"key", asset_key}, {"file", assets::utf8(mesh_file.wstring())}}})}}
                .dump(),
            play_project));
        require(diagnostics.size() == 1 && diagnostics[0].severity == world::Severity::error &&
                    current_mesh() == received.mesh,
                "Malformed or wrong-key mesh payload changed the accepted geometry.");
    };
    rejected("project/wrong-key");
    fs::resize_file(mesh_file, 12);
    rejected(mesh_key);
    require(!link.close() && !fs::exists(spill), "Closing Live leaked pending texture/mesh content.");
    std::puts(
        "Manual resource reimport/Live: PASS (exact RGBA/mesh bits, >16 MiB, current identity, parameter Undo, ACK/close retirement)");
}
const LinkJson& graph_entity(const LinkJson& graph, std::uint64_t identity) {
    for (const auto& item : graph.at("entities"))
        if (item.at("id") == std::to_string(identity))
            return item;
    throw std::runtime_error("The inspected graph lacks its Entity.");
}
// The editor document drives a real headless Play through the link.
void verify_play_link(const std::filesystem::path& root, editor::Project& project, const std::string& scene,
                      const std::filesystem::path& snapshot) {
    auto loaded = editor::load_project_scene(project, scene);
    require(loaded.session != nullptr, "The authoring scene did not load.");
    auto& document = *loaded.session;
    application::PlayProcess process;
    std::unique_ptr<application::LiveLink> live;
    const auto spill = root / "Cache/Play/acceptance";
    take(process.start(root, snapshot, true, 0, {}, false, false, &live));
    application::PlayLink link;
    link.open(std::move(live), spill);
    document.set_live_forwarding(true);
    const auto service = [&] {
        process.poll();
        link.poll(process.running(), process.stopping());
        if (!process.running() || !link.divergence().empty()) {
            throw std::runtime_error("Play stopped following the document: " + link.divergence());
        }
    };
    const auto inspect = [&](ObjectId object) -> std::optional<LinkJson> {
        const auto identity = *document.document_identity(object);
        const auto request = link.inspect(identity);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        for (;;) {
            service();
            if (auto reply = link.inspected(request)) {
                if (reply->starts_with("error: "))
                    return {};
                return graph_entity(LinkJson::parse(*reply), identity);
            }
            require(std::chrono::steady_clock::now() < until, "Play did not answer an inspection.");
            Sleep(2);
        }
    };
    const auto mover = entity(document);
    const auto forward = [&] {
        link.forward(take(document.take_live_ops()));
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (link.unacknowledged()) {
            service();
            require(std::chrono::steady_clock::now() < until, "Play did not acknowledge a change batch.");
            Sleep(2);
        }
        // ACK precedes the frame's camera resolution. Observe an actual tick
        // boundary so consecutive camera changes cannot coalesce in that frame.
        const auto ticks = [&] {
            for (const auto& part : inspect(mover)->at("components"))
                if (part.at("type") == "project.Move")
                    return part.at("properties").at("ticks").get<std::uint64_t>();
            throw std::runtime_error("Play lost its ticking Source component.");
        };
        const auto previous = ticks();
        while (ticks() == previous) {
            require(std::chrono::steady_clock::now() < until, "Play did not finish a frame after its ACK.");
            Sleep(2);
        }
    };
    // Play is still loading: the change waits in the link and applies before Play starts.
    take(document.rename(mover, L"Runner"));
    forward();
    require(inspect(mover)->at("name") == "Runner", "A change made while Play loaded did not reach it.");
    const auto ids = [&] {
        std::vector<ObjectId> result;
        for (const auto& member : take(document.world().members()))
            result.push_back(member.id());
        return result;
    };
    const auto before = ids();
    complete(document.add_entity());
    auto after = ids();
    std::erase_if(after, [&](ObjectId id) { return std::ranges::find(before, id) != before.end(); });
    require(after.size() == 1, "The test Entity was not created.");
    const auto created = after.front();
    forward();
    require(inspect(created).has_value(), "A created Entity did not reach Play.");
    complete(document.undo());
    forward();
    require(!inspect(created), "Undoing a creation did not remove it from Play.");
    complete(document.redo());
    forward();
    require(inspect(created).has_value(), "Redoing a creation did not restore it in Play.");
    const auto speed_of = [&] {
        for (const auto& component : inspect(mover)->at("components"))
            if (component.at("type") == "project.Move")
                return component.at("properties").at("speed").get<float>();
        throw std::runtime_error("Play lost the Source component.");
    };
    const auto move = behavior(document);
    auto& runtime = document.world().runtime();
    const std::array speed{take(runtime.find_field(take(runtime.type_of(move)), "speed"))};
    take(document.begin_edit(move, speed, 880001));
    take(document.preview_edit(880001, 9.f));
    forward();
    require(speed_of() == 9, "A field preview did not reach Play.");
    take(document.cancel_edit());
    forward();
    require(speed_of() == 2, "Cancelling a preview did not restore Play.");
    // A forest arrives as separate root adds over the real IPC connection. Its
    // Environment precedes its camera, including when Redo reuses document IDs.
    {
        editor::Session source;
        complete(source.create_camera({2, 3, 4}));
        take(source.set_main_camera(source.selection().entity));
        auto forest = LinkJson::parse(take(source.serialize()));
        const auto environment = take(world::inspect_component(source.world().runtime(), source.environment())).owner;
        forest["roots"] = LinkJson::array({std::to_string(*source.document_identity(environment)),
                                           std::to_string(*source.document_identity(source.selection().entity))});
        complete(document.instantiate_subtree(forest.dump(), project));
        const auto imported_camera = take(document.main_camera());
        const auto imported_environment = take(world::inspect_component(runtime, document.environment())).owner;
        const auto verify_import = [&] {
            const auto environment = inspect(imported_environment), camera = inspect(imported_camera);
            require(environment && camera, "A camera forest import lost a root over Play IPC.");
            for (const auto& part : environment->at("components"))
                if (part.at("type") == "world.EnvironmentComponent") {
                    require(part.at("properties").at("main_camera") ==
                                LinkJson{{"$ref", std::to_string(*document.document_identity(imported_camera))}},
                            "A camera forest import lost its document-identity selection over Play IPC.");
                    return;
                }
            throw std::runtime_error("An imported Environment lost its component over Play IPC.");
        };
        forward();
        verify_import();
        complete(document.undo());
        forward();
        require(!inspect(imported_environment) && !inspect(imported_camera), "Undo camera import left a root in Play.");
        complete(document.redo());
        forward();
        verify_import();
        // The end-of-run Reader audit below covers the entire import/history,
        // including records queued after a successful ACK.
        complete(document.undo());
        forward();
        std::puts(
            "Camera forest IPC: PASS (Environment-first import, undo, redo, document identities, no rejected command or dropped reference)");
    }
    // Main camera changes cross the real process boundary as document references,
    // including the first Environment and delete/save/Undo's graph-before-reference order.
    complete(document.create_camera({2, 3, 4}));
    const auto camera_a = document.selection().entity;
    forward();
    complete(document.create_camera({-7, 1, 6}));
    const auto camera_b = document.selection().entity;
    forward();
    take(document.set_main_camera(camera_a));
    forward();
    const auto environment = document.environment();
    const auto environment_owner = take(world::inspect_component(runtime, environment)).owner;
    const auto main_reference = [&]() -> LinkJson {
        const auto inspected = inspect(environment_owner);
        require(inspected.has_value(), "Play lost the Environment Entity.");
        for (const auto& component : inspected->at("components"))
            if (component.at("type") == "world.EnvironmentComponent")
                return component.at("properties").at("main_camera");
        throw std::runtime_error("Play lost the EnvironmentComponent.");
    };
    const auto reference = [&](ObjectId entity) {
        return LinkJson{{"$ref", std::to_string(*document.document_identity(entity))}};
    };
    require(main_reference() == reference(camera_a), "First Set Main Camera did not transfer its reference.");
    complete(document.undo());
    forward();
    require(!inspect(environment_owner), "Undo first Set kept an Environment in Play.");
    complete(document.redo());
    forward();
    require(main_reference() == reference(camera_a), "Redo first Set lost its reference.");
    take(document.set_main_camera(camera_b));
    forward();
    require(main_reference() == reference(camera_b), "Set B did not replace A in Play.");
    take(document.set_main_camera({}));
    forward();
    require(main_reference().is_null(), "Clear Main Camera did not reach Play.");
    complete(document.undo());
    forward();
    require(main_reference() == reference(camera_b), "Undo Clear did not restore B in Play.");
    complete(document.redo());
    forward();
    require(main_reference().is_null(), "Redo Clear did not clear Play.");
    take(document.set_main_camera(camera_a));
    forward();
    take(document.select_entities(std::array{camera_a}, camera_a));
    complete(document.erase_entities(document.selected_entities()));
    forward();
    require(main_reference().is_null() && !inspect(camera_a),
            "Deleting the main camera did not clear Play before removal.");
    static_cast<void>(take(document.serialize()));
    document.mark_saved();
    complete(document.undo());
    forward();
    require(inspect(camera_a).has_value() && main_reference() == reference(camera_a),
            "Undo after save did not restore both Play graph and reference.");
    complete(document.redo());
    forward();
    require(main_reference().is_null() && !inspect(camera_a), "Redo deletion did not clear and remove in Play.");
    complete(document.create_camera({1, 4, 9}));
    const auto later_camera = document.selection().entity;
    forward();
    take(document.set_main_camera(later_camera));
    forward();
    take(document.select_entities(std::array{environment_owner, later_camera}, later_camera));
    complete(document.erase_entities(document.selected_entities()));
    forward();
    require(!inspect(environment_owner) && !inspect(later_camera),
            "Deleting the Environment and its main camera left one in Play.");
    static_cast<void>(take(document.serialize()));
    document.mark_saved();
    complete(document.undo());
    forward();
    require(main_reference() == reference(later_camera) && inspect(later_camera),
            "Undo lost a camera reference when the Environment root precedes its target.");
    // Scene commands stay available while Play follows the document; their changes are forwarded.
    {
        editor::Workspace workspace;
        workspace.project_open = true;
        workspace.project_running = true;
        workspace.play_pending = 0;
        take(document.select_entities(std::array{mover}, mover));
        for (const auto id : {"scene.delete", "scene.rename", "scene.duplicate", "scene.transform_clear",
                              "scene.select_all", "scene.transform_remove", "play.restart"})
            require(
                workspace.operators.find(id)->poll(workspace.operator_context(document, &project, true)).has_value(),
                "A scene command is unavailable while Play runs.");
    }
    process.stop();
    for (const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
         process.running() && std::chrono::steady_clock::now() < until; Sleep(5)) {
        process.poll();
        link.poll(process.running(), process.stopping());
    }
    link.poll(process.running(), process.stopping());
    require(!process.running() && process.exit_code() == 0 && link.divergence().empty(),
            "Stopping Play diverged or failed.");
    // Initial absence plus eight transitions deliberately clear/remove a camera
    // or its Environment. Other warning/error records remain unexpected.
    RunLog completed_log(process);
    completed_log.finish(true, {{"runner", "runner.camera_unavailable", logging::Severity::warning, 9}});
    require(!completed_log.count("world.reference_dropped") && !completed_log.count("world.command_rejected"),
            "Camera forest import/history or Undo reported a dropped reference or rejected command over IPC.");
    require(!link.close() && !std::filesystem::exists(spill), "Stopping Play left its change files.");

    // Play's end of the protocol, scripted: spilled batches, inspection and the sequence rule.
    std::unique_ptr<application::LiveLink> raw;
    take(process.start(root, snapshot, true, 0, {}, false, false, &raw));
    LinkPeer play{std::move(raw), {}};
    const auto id = std::to_string(*document.document_identity(mover));
    const auto rename = [&](std::string_view name) {
        return LinkJson::array({{{"op", "set"}, {"id", id}, {"name", name}}});
    };
    const auto file = root / "Cache/Play/raw-batch.json";
    take(editor::write_file(file, LinkJson{{"ops", rename("Spilled")}}.dump()));
    play.send({{"type", "batch"}, {"seq", 1}, {"file", assets::utf8(file.wstring())}});
    auto reply = play.next(&process);
    require(reply.at("type") == "ack" && reply.at("seq") == 1 && reply.at("diagnostics").empty() &&
                std::filesystem::exists(file),
            "Play did not acknowledge the spilled batch or deleted sender-owned content.");
    // This scripted sender owns its file just as PlayLink::Pending does.
    require(std::filesystem::remove(file), "The scripted sender could not retire its acknowledged batch.");
    const auto play_name = [&] {
        play.send({{"type", "inspect"}, {"id", 9}, {"object", *document.document_identity(mover)}});
        const auto answer = play.next(&process);
        require(answer.at("type") == "inspect" && answer.at("id") == 9, "Play answered another request.");
        return graph_entity(LinkJson::parse(answer.at("graph").get<std::string>()), *document.document_identity(mover))
            .at("name")
            .get<std::string>();
    };
    require(play_name() == "Spilled", "Play did not apply the spilled batch.");
    play.send({{"type", "batch"}, {"seq", 3}, {"ops", rename("Late")}});
    reply = play.next(&process);
    require(reply.at("seq") == 3 && reply.at("diagnostics").size() == 1 &&
                reply.at("diagnostics")[0].at("severity") == "error" &&
                reply.at("diagnostics")[0].at("message").get<std::string>().find(
                    "sequence gap: expected batch 2, received 3") != std::string::npos,
            "Play did not reject a sequence gap.");
    play.send({{"type", "batch"}, {"seq", 2}, {"ops", rename("Later")}});
    reply = play.next(&process);
    require(reply.at("seq") == 2 && reply.at("diagnostics")[0].at("severity") == "error" && play_name() == "Spilled",
            "Play applied a batch after diverging.");
    play.link->send("{");
    reply = play.next(&process);
    require(reply.at("type") == "error", "Play did not report an unreadable message.");
    process.stop();
    wait(process, std::chrono::seconds(10));
    require(process.exit_code() == 0, "The scripted Play run failed.");
    RunLog rejected_log(process);
    rejected_log.finish(true, {{"runner", "runner.camera_unavailable", logging::Severity::warning, 1},
                               {"world",
                                "world.command_rejected",
                                logging::Severity::error,
                                2,
                                {{"origin", std::string("live")},
                                 {"phase", std::string("command")},
                                 {"object", std::string("null")},
                                 {"entity", std::string("null")}}}});
    require(rejected_log.has("world.command_rejected",
                             [](const auto& record) {
                                 return record.entry.message.find("sequence gap: expected batch 2, received 3") !=
                                        std::string::npos;
                             }),
            "Play did not persist its actual sequence-gap diagnostic independently of the ACK.");
    // Throws unless every run left Cache/Play empty.
    std::filesystem::remove(root / "Cache/Play");
}
