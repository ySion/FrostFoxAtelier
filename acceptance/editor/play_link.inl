// Included by project_execution.cpp: the editor-to-Play change link.
using LinkJson=editor::json_format::Json;
// Plays the other end of a link: queues what arrives so tests read one message at a time.
struct LinkPeer {
    std::unique_ptr<application::LiveLink> link;
    std::deque<std::string> arrived;
    LinkJson next(application::PlayProcess* process=nullptr,std::string* output=nullptr) {
        const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(60);
        while(arrived.empty()) {
            for(auto& text:link->receive()) arrived.push_back(std::move(text));
            if(process) {process->poll();if(output) *output+=process->take_output();}
            require(std::chrono::steady_clock::now()<until,"The link peer received nothing.");
            if(arrived.empty()) Sleep(2);
        }
        auto text=std::move(arrived.front());arrived.pop_front();return LinkJson::parse(text);
    }
    bool idle() {Sleep(50);for(auto& text:link->receive()) arrived.push_back(std::move(text));return arrived.empty();}
    void send(const LinkJson& message) {link->send(message.dump());}
};
std::pair<std::unique_ptr<application::LiveLink>,LinkPeer> link_pair() {
    HANDLE editor_read{},peer_write{},peer_read{},editor_write{};
    require(CreatePipe(&editor_read,&peer_write,nullptr,1<<20) && CreatePipe(&peer_read,&editor_write,nullptr,1<<20),"Cannot create link pipes.");
    return {std::make_unique<application::LiveLink>(editor_read,editor_write),LinkPeer{std::make_unique<application::LiveLink>(peer_read,peer_write),{}}};
}
LinkJson ack(std::uint64_t seq,std::string_view severity={}) {
    LinkJson message={{"type","ack"},{"seq",seq},{"diagnostics",LinkJson::array()}};
    if(!severity.empty()) message["diagnostics"].push_back({{"severity",severity},{"kind","command_rejected"},{"message","Play change 0 #7: rejected."}});
    return message;
}
void settle(application::PlayLink& link,std::size_t pending=0) {
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(link.unacknowledged()!=pending && link.divergence().empty()) {
        link.poll(true,false);require(std::chrono::steady_clock::now()<until,"The editor end did not read its acknowledgements.");Sleep(2);
    }
}
// The editor end in-process against a scripted peer: numbering, spilling, splitting and divergence.
void verify_link_protocol(const std::filesystem::path& root) {
    const auto spill=root/"Cache/Play/link-protocol";
    const std::string name_op=R"({"op":"set","id":"7","name":"A"})",remove_op=R"({"op":"remove","id":"8"})";
    const auto large_op=[](std::size_t bytes){return R"({"op":"set","id":"7","name":")"+std::string(bytes,'a')+R"("})";};
    {
        auto [end,peer]=link_pair();application::PlayLink link;link.open(std::move(end),spill);
        link.forward({name_op,remove_op});link.forward({});
        const auto batch=peer.next();
        require(batch.at("type")=="batch" && batch.at("seq")==1 && batch.at("ops").size()==2 && batch.at("ops")[1].at("op")=="remove" &&
            link.unacknowledged()==1 && peer.idle(),"One step's ops did not travel as one numbered batch.");
        // Warnings are Play's business; only an error acknowledgement is divergence.
        peer.send(ack(1,"warning"));settle(link);require(link.divergence().empty(),"A warning acknowledgement diverged.");
        const auto five=large_op(5<<20),nine=large_op(9<<20);
        link.forward({five});
        const auto spilled=peer.next();
        require(spilled.at("seq")==2 && spilled.contains("file") && !spilled.contains("ops"),"A batch over one message was not spilled to a file.");
        const std::filesystem::path file=editor::wide(spilled.at("file").get<std::string>());
        require(file.parent_path()==spill && take(editor::read_file(file))==R"({"ops":[)"+five+"]}","The spilled file is not the batch Play applies.");
        // Past the 16 MiB batch limit the ops split at their boundary into consecutive batches.
        link.forward({nine,nine});
        const auto first=peer.next(),second=peer.next();
        require(first.at("seq")==3 && second.at("seq")==4 && link.unacknowledged()==3 &&
            take(editor::read_file(editor::wide(second.at("file").get<std::string>())))==R"({"ops":[)"+nine+"]}","Oversized ops were not split into consecutive batches.");
        peer.send(ack(2));peer.send(ack(3));peer.send(ack(4));settle(link);
        link.forward({name_op});require(peer.next().at("seq")==5,"Batches lost their numbering.");
        peer.send(ack(5,"error"));settle(link);
        require(link.divergence()=="Play change 0 #7: rejected." && link.unacknowledged()==0,"An error acknowledgement did not diverge.");
        link.forward({name_op});require(peer.idle() && link.unacknowledged()==0,"A diverged link kept sending.");
        require(!link.close() && !std::filesystem::exists(spill),"Closing the link left its spill directory.");
    }
    const auto diverges=[&](auto&& act,std::string_view expected) {
        auto [end,peer]=link_pair();application::PlayLink link;link.open(std::move(end),spill);
        act(link,peer);
        require(link.divergence().find(expected)!=std::string::npos,"A broken link did not diverge.");
        static_cast<void>(link.close());
    };
    diverges([&](application::PlayLink& link,LinkPeer& peer) {
        link.forward({name_op});link.forward({name_op});peer.send(ack(2));settle(link,1);
    },"out of order");
    diverges([&](application::PlayLink& link,LinkPeer& peer) {
        for(std::size_t i=0;i<=application::PlayLink::maximum_pending;++i) link.forward({name_op});
        require(link.unacknowledged()==application::PlayLink::maximum_pending+1,"Backpressure stopped early.");
        link.forward({name_op});static_cast<void>(peer);
    },"stopped acknowledging");
    diverges([&](application::PlayLink& link,LinkPeer& peer) {
        link.forward({name_op,large_op(application::PlayLink::maximum_batch)});
        require(link.unacknowledged()==0 && peer.idle(),"A step with an op past the batch limit was partly sent.");
    },"exceeds the 16 MiB");
    // A child closes its pipes before it exits: only a link that stays closed while Play runs diverges.
    diverges([&](application::PlayLink& link,LinkPeer& peer) {
        peer.link.reset();Sleep(100);
        link.poll(true,false);link.poll(false,false);Sleep(2100);link.poll(true,true);link.poll(true,false);
        require(link.divergence().empty(),"A closing child diverged within its grace period.");
        Sleep(2100);link.poll(true,false);
    },"closed while Play kept running");
}
const LinkJson& graph_entity(const LinkJson& graph,std::uint64_t identity) {
    for(const auto& item:graph.at("entities")) if(item.at("id")==std::to_string(identity)) return item;
    throw std::runtime_error("The inspected graph lacks its Entity.");
}
// The editor document drives a real headless Play through the link.
void verify_play_link(const std::filesystem::path& root,editor::Project& project,const std::string& scene,const std::filesystem::path& snapshot) {
    auto loaded=editor::load_project_scene(project,scene);require(loaded.session!=nullptr,"The authoring scene did not load.");
    auto& document=*loaded.session;
    application::PlayProcess process;std::unique_ptr<application::LiveLink> live;std::string output;
    const auto spill=root/"Cache/Play/acceptance";
    take(process.start(root,snapshot,true,0,{},false,false,&live));
    application::PlayLink link;link.open(std::move(live),spill);document.set_live_forwarding(true);
    const auto service=[&] {
        process.poll();output+=process.take_output();link.poll(process.running(),process.stopping());
        if(!process.running() || !link.divergence().empty()) {
            std::fwrite(output.data(),1,output.size(),stdout);
            throw std::runtime_error("Play stopped following the document: "+link.divergence());
        }
    };
    const auto forward=[&] {
        link.forward(take(document.take_live_ops()));
        const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(60);
        while(link.unacknowledged()) {service();require(std::chrono::steady_clock::now()<until,"Play did not acknowledge a change batch.");Sleep(2);}
    };
    const auto inspect=[&](ObjectId object)->std::optional<LinkJson> {
        const auto identity=*document.document_identity(object);const auto request=link.inspect(identity);
        const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(60);
        for(;;) {
            service();
            if(auto reply=link.inspected(request)) {
                if(reply->starts_with("error: ")) return {};
                return graph_entity(LinkJson::parse(*reply),identity);
            }
            require(std::chrono::steady_clock::now()<until,"Play did not answer an inspection.");Sleep(2);
        }
    };
    const auto mover=entity(document);
    // Play is still loading: the change waits in the link and applies before Play starts.
    take(document.rename(mover,L"Runner"));forward();
    require(inspect(mover)->at("name")=="Runner","A change made while Play loaded did not reach it.");
    const auto ids=[&] {std::vector<ObjectId> result;for(const auto& member:take(document.world().members())) result.push_back(member.id());return result;};
    const auto before=ids();complete(document.add_entity());auto after=ids();
    std::erase_if(after,[&](ObjectId id){return std::ranges::find(before,id)!=before.end();});
    require(after.size()==1,"The test Entity was not created.");const auto created=after.front();
    forward();require(inspect(created).has_value(),"A created Entity did not reach Play.");
    complete(document.undo());forward();require(!inspect(created),"Undoing a creation did not remove it from Play.");
    complete(document.redo());forward();require(inspect(created).has_value(),"Redoing a creation did not restore it in Play.");
    const auto speed_of=[&] {
        for(const auto& component:inspect(mover)->at("components")) if(component.at("type")=="project.Move")
            return component.at("properties").at("speed").get<float>();
        throw std::runtime_error("Play lost the Source component.");
    };
    const auto move=behavior(document);auto& runtime=document.world().runtime();
    const std::array speed{take(runtime.find_field(take(runtime.type_of(move)),"speed"))};
    take(document.begin_edit(move,speed,880001));take(document.preview_edit(880001,9.f));forward();
    require(speed_of()==9,"A field preview did not reach Play.");
    take(document.cancel_edit());forward();require(speed_of()==2,"Cancelling a preview did not restore Play.");
    // A forest arrives as separate root adds over the real IPC connection. Its
    // Environment precedes its camera, including when Redo reuses document IDs.
    {
        editor::Session source;take(source.initialize_environment({}));
        complete(source.create_camera({2,3,4}));take(source.set_main_camera(source.selection().entity));
        auto forest=LinkJson::parse(take(source.serialize()));
        const auto environment=take(world::inspect_component(source.world().runtime(),source.environment())).owner;
        forest["roots"]=LinkJson::array({std::to_string(*source.document_identity(environment)),std::to_string(*source.document_identity(source.selection().entity))});
        complete(document.instantiate_subtree(forest.dump(),project));
        const auto imported_camera=take(document.main_camera());
        const auto imported_environment=take(world::inspect_component(runtime,document.environment())).owner;
        const auto verify_import=[&] {
            const auto environment=inspect(imported_environment),camera=inspect(imported_camera);
            require(environment && camera,"A camera forest import lost a root over Play IPC.");
            for(const auto& part:environment->at("components")) if(part.at("type")=="world.EnvironmentComponent") {
                require(part.at("properties").at("main_camera")==LinkJson{{"$ref",std::to_string(*document.document_identity(imported_camera))}},
                    "A camera forest import lost its document-identity selection over Play IPC.");
                return;
            }
            throw std::runtime_error("An imported Environment lost its component over Play IPC.");
        };
        forward();verify_import();
        complete(document.undo());forward();
        require(!inspect(imported_environment) && !inspect(imported_camera),"Undo camera import left a root in Play.");
        complete(document.redo());forward();verify_import();
        require(output.find("reference_dropped")==std::string::npos && output.find("command_rejected")==std::string::npos,
            "Camera forest import/history reported a dropped reference or rejected command over IPC.");
        complete(document.undo());forward();
        std::puts("Camera forest IPC: PASS (Environment-first import, undo, redo, document identities, no rejected command or dropped reference)");
    }
    // Main camera changes cross the real process boundary as document references,
    // including the first Environment and delete/save/Undo's graph-before-reference order.
    complete(document.create_camera({2,3,4}));const auto camera_a=document.selection().entity;forward();
    complete(document.create_camera({-7,1,6}));const auto camera_b=document.selection().entity;forward();
    take(document.set_main_camera(camera_a));forward();
    const auto environment=document.environment();
    const auto environment_owner=take(world::inspect_component(runtime,environment)).owner;
    const auto main_reference=[&]() -> LinkJson {
        const auto inspected=inspect(environment_owner);
        require(inspected.has_value(),"Play lost the Environment Entity.");
        for(const auto& component:inspected->at("components")) if(component.at("type")=="world.EnvironmentComponent")
            return component.at("properties").at("main_camera");
        throw std::runtime_error("Play lost the EnvironmentComponent.");
    };
    const auto reference=[&](ObjectId entity) {return LinkJson{{"$ref",std::to_string(*document.document_identity(entity))}};};
    require(main_reference()==reference(camera_a),"First Set Main Camera did not transfer its reference.");
    complete(document.undo());forward();require(!inspect(environment_owner),"Undo first Set kept an Environment in Play.");
    complete(document.redo());forward();require(main_reference()==reference(camera_a),"Redo first Set lost its reference.");
    take(document.set_main_camera(camera_b));forward();require(main_reference()==reference(camera_b),"Set B did not replace A in Play.");
    take(document.set_main_camera({}));forward();require(main_reference().is_null(),"Clear Main Camera did not reach Play.");
    complete(document.undo());forward();require(main_reference()==reference(camera_b),"Undo Clear did not restore B in Play.");
    complete(document.redo());forward();require(main_reference().is_null(),"Redo Clear did not clear Play.");
    take(document.set_main_camera(camera_a));forward();
    take(document.select_entities(std::array{camera_a},camera_a));complete(document.erase_selected());forward();
    require(main_reference().is_null() && !inspect(camera_a),"Deleting the main camera did not clear Play before removal.");
    static_cast<void>(take(document.serialize()));document.mark_saved();
    complete(document.undo());forward();
    require(inspect(camera_a).has_value() && main_reference()==reference(camera_a),"Undo after save did not restore both Play graph and reference.");
    complete(document.redo());forward();require(main_reference().is_null() && !inspect(camera_a),"Redo deletion did not clear and remove in Play.");
    complete(document.create_camera({1,4,9}));const auto later_camera=document.selection().entity;forward();
    take(document.set_main_camera(later_camera));forward();
    take(document.select_entities(std::array{environment_owner,later_camera},later_camera));complete(document.erase_entities());forward();
    require(!inspect(environment_owner) && !inspect(later_camera),"Deleting the Environment and its main camera left one in Play.");
    static_cast<void>(take(document.serialize()));document.mark_saved();
    complete(document.undo());forward();
    require(main_reference()==reference(later_camera) && inspect(later_camera),"Undo lost a camera reference when the Environment root precedes its target.");
    require(output.find("reference_dropped")==std::string::npos,"A valid camera Undo reported a dropped reference before restoring its target.");
    // Scene commands stay available while Play follows the document; their changes are forwarded.
    {
        editor::Workspace workspace;workspace.project_open=true;workspace.project_running=true;workspace.play_pending=0;
        take(document.select_entities(std::array{mover},mover));
        for(const auto command:{editor::Command::erase_entities,editor::Command::rename_selected,editor::Command::duplicate_selected,
            editor::Command::clear_location,editor::Command::select_all,editor::Command::remove_transform,editor::Command::restart_project})
            require(workspace.command_enabled(command,document,&project),"A scene command is unavailable while Play runs.");
    }
    process.stop();
    for(const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(10);process.running() && std::chrono::steady_clock::now()<until;Sleep(5))
        {process.poll();output+=process.take_output();link.poll(process.running(),process.stopping());}
    link.poll(process.running(),process.stopping());
    require(!process.running() && process.exit_code()==0 && link.divergence().empty(),"Stopping Play diverged or failed.");
    require(!link.close() && !std::filesystem::exists(spill),"Stopping Play left its change files.");

    // Play's end of the protocol, scripted: spilled batches, inspection and the sequence rule.
    std::unique_ptr<application::LiveLink> raw;take(process.start(root,snapshot,true,0,{},false,false,&raw));
    LinkPeer play{std::move(raw),{}};output.clear();
    const auto id=std::to_string(*document.document_identity(mover));
    const auto rename=[&](std::string_view name){return LinkJson::array({{{"op","set"},{"id",id},{"name",name}}});};
    const auto file=root/"Cache/Play/raw-batch.json";
    take(editor::write_file(file,LinkJson{{"ops",rename("Spilled")}}.dump()));
    play.send({{"type","batch"},{"seq",1},{"file",editor::utf8(file.wstring())}});
    auto reply=play.next(&process,&output);
    require(reply.at("type")=="ack" && reply.at("seq")==1 && reply.at("diagnostics").empty() && !std::filesystem::exists(file),
        "Play did not apply and delete a spilled batch.");
    const auto play_name=[&] {
        play.send({{"type","inspect"},{"id",9},{"object",*document.document_identity(mover)}});
        const auto answer=play.next(&process,&output);require(answer.at("type")=="inspect" && answer.at("id")==9,"Play answered another request.");
        return graph_entity(LinkJson::parse(answer.at("graph").get<std::string>()),*document.document_identity(mover)).at("name").get<std::string>();
    };
    require(play_name()=="Spilled","Play did not apply the spilled batch.");
    play.send({{"type","batch"},{"seq",3},{"ops",rename("Late")}});reply=play.next(&process,&output);
    require(reply.at("seq")==3 && reply.at("diagnostics").size()==1 && reply.at("diagnostics")[0].at("severity")=="error" &&
        reply.at("diagnostics")[0].at("message").get<std::string>().find("sequence gap: expected batch 2, received 3")!=std::string::npos,
        "Play did not reject a sequence gap.");
    play.send({{"type","batch"},{"seq",2},{"ops",rename("Later")}});reply=play.next(&process,&output);
    require(reply.at("seq")==2 && reply.at("diagnostics")[0].at("severity")=="error" && play_name()=="Spilled","Play applied a batch after diverging.");
    play.link->send("{");reply=play.next(&process,&output);
    require(reply.at("type")=="error","Play did not report an unreadable message.");
    require(output.find("sequence gap")!=std::string::npos,"Play did not report its divergence in its output.");
    process.stop();wait(process,std::chrono::seconds(10));require(process.exit_code()==0,"The scripted Play run failed.");
    // Throws unless every run left Cache/Play empty.
    std::filesystem::remove(root/"Cache/Play");
}
