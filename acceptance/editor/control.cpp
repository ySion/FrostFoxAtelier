#include "control/control.hpp"
#include "platform/windows_path.hpp"
#include <Windows.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <thread>
#include <cmath>
#include <array>

namespace {
using namespace atelier::control;
using namespace std::chrono_literals;
void require(bool ok,std::string_view message) {if(!ok) throw std::runtime_error(std::string(message));}
Json read(const std::filesystem::path& path) {std::ifstream stream(atelier::platform::windows_file_path(path),std::ios::binary);require(bool(stream),"Cannot read E2E artifact: "+utf8(path.wstring()));return Json::parse(stream);}
void write(const std::filesystem::path& path,std::string_view text) {
    std::filesystem::create_directories(atelier::platform::windows_file_path(path.parent_path()));
    std::ofstream stream(atelier::platform::windows_file_path(path),std::ios::binary|std::ios::trunc);stream.write(text.data(),static_cast<std::streamsize>(text.size()));
    require(bool(stream),"Cannot write E2E fixture");
}
constexpr std::string_view published_code="class project.ControlProbe : world.Component { float speed = 1.0f; void tick(float dt) { self.translate_owner(self.speed * dt, 0.0f, 0.0f); } }\n";
constexpr std::string_view published_ui="ui ProjectUI(in Text title, in double elapsed, in double entities) { Column hud { Text title { text: input.title; } } }\n";
void project_fixture(const std::filesystem::path& project) {
    write(project/"Content/Scenes/Main.scene",Json{{"version",2},{"actors",Json::array()}}.dump());
    write(project/"Source/Published.source",published_code);write(project/"Source/Published.ui.source",published_ui);
    std::filesystem::create_directories(project/"Cache");
    write(project/"Config/Project.json",Json{{"version",1},{"name","Control review"},{"startup_scene","Content/Scenes/Main.scene"},{"ui_source","Source/Published.ui.source"}}.dump());
}
std::wstring quote(const std::filesystem::path& path) {return L"\""+path.wstring()+L"\"";}
struct Process {
    HANDLE process{};DWORD pid{};HANDLE log{};
    Process(const std::filesystem::path& executable,std::wstring arguments,const std::filesystem::path& output) {
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
        log=CreateFileW(output.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        require(log!=INVALID_HANDLE_VALUE,"Cannot create child log");
        auto command=quote(executable)+L" "+arguments;
        STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;startup.hStdOutput=log;startup.hStdError=log;
        PROCESS_INFORMATION created{};
        if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,executable.parent_path().c_str(),&startup,&created)) {
            CloseHandle(log);throw std::runtime_error("Cannot launch control E2E child");
        }
        process=created.hProcess;pid=created.dwProcessId;CloseHandle(created.hThread);
    }
    ~Process() {if(process) {if(WaitForSingleObject(process,0)==WAIT_TIMEOUT) {TerminateProcess(process,99);WaitForSingleObject(process,10000);}CloseHandle(process);}if(log && log!=INVALID_HANDLE_VALUE) CloseHandle(log);}
    HWND window() const {
        struct Query {DWORD pid;HWND window{};} query{pid};
        EnumWindows([](HWND window,LPARAM parameter)->BOOL {auto& query=*reinterpret_cast<Query*>(parameter);DWORD pid{};GetWindowThreadProcessId(window,&pid);
            if(pid==query.pid && IsWindowVisible(window) && GetWindow(window,GW_OWNER)==nullptr) {query.window=window;return FALSE;}return TRUE;},reinterpret_cast<LPARAM>(&query));return query.window;
    }
    void close() {
        if(auto handle=window()) PostMessageW(handle,WM_CLOSE,0,0);
        require(WaitForSingleObject(process,10000)==WAIT_OBJECT_0,"Editor did not close cleanly");DWORD code{};GetExitCodeProcess(process,&code);require(code==0,"Editor exited with a failure");
    }
};
struct Driver {
    std::unique_ptr<Client> client;std::string token;Json descriptor;
    explicit Driver(Json instance):client(std::make_unique<Client>(instance,600s)),descriptor(std::move(instance)) {}
    Json call(std::string op,Json args=Json::object(),std::string id={}) {
        Json request{{"op",std::move(op)},{"args",std::move(args)},{"state_token",token}};if(!id.empty()) request["request_id"]=id;
        auto result=client->call(std::move(request));if(result.contains("state_token")) token=result.at("state_token").get<std::string>();return result;
    }
    Json ok(std::string op,Json args=Json::object()) {auto result=call(std::move(op),std::move(args));require(result.value("status","")=="ok",result.dump());return result.at("result");}
    Json inspect() {return ok("editor.inspect");}
    Json wait(std::string id,std::chrono::seconds timeout=125s) {
        const auto deadline=std::chrono::steady_clock::now()+timeout;
        while(std::chrono::steady_clock::now()<deadline) {auto result=call("job.status",{{"job_id",id}});if(result.at("status")!="pending") return result;std::this_thread::sleep_for(20ms);}
        throw std::runtime_error("Control job did not complete");
    }
};
Json discover_editor(const Process& editor,const std::filesystem::path& directory) {
    const auto deadline=std::chrono::steady_clock::now()+60s;
    while(std::chrono::steady_clock::now()<deadline) {
        for(const auto& instance:discover(directory)) if(instance.at("pid").get<DWORD>()==editor.pid) return instance;
        require(WaitForSingleObject(editor.process,0)==WAIT_TIMEOUT,"Editor stopped before publishing its endpoint");std::this_thread::sleep_for(30ms);
    }
    throw std::runtime_error("Editor did not publish its endpoint");
}
void error(const Json& result,std::string_view expected) {require(result.value("status","")=="error" && result.at("error").at("code").get<std::string>()==expected,result.dump());}
void verify_option_descriptions(const Json& capabilities) {
    const auto& options=capabilities.at("display_options");
    for(const auto& [name,option]:options.items()) {
        static_cast<void>(name);
        for(const char* field:{"label","category","description"})
            require(option.at(field).is_string() && !option.at(field).get<std::string>().empty(),"Control option metadata is missing");
    }
    const auto& count=options.at("probe_volume_0_count_x");
    const auto description=count.at("description").get<std::string>();
    require(description.find('\n')!=std::string::npos && std::any_of(description.begin(),description.end(),[](unsigned char c){return c>=0x80;}),
        "Control metadata lost Unicode or decoded line breaks");
    require(count.at("type")=="integer" && count.at("minimum")==2 && count.at("maximum")==32 && count.at("scope")=="scene" &&
        options.at("probe_visualization").at("scope")=="view","Metadata changed option types, bounds or ownership");
    for(unsigned slot=0;slot<4;++slot) for(const char* axis:{"x","y","z"})
        require(options.at("probe_volume_"+std::to_string(slot)+"_count_"+axis).at("description")==description,
            "Volume slots or vector lanes diverged from their shared description");
}
void verify_property_descriptions(const Json& object) {
    require(object.at("type_description").is_string(),"Object type description is missing");
    std::string position;unsigned lanes{};
    for(const auto& field:object.at("properties")) {
        require(field.at("description").is_string(),"Reflected property description is missing");
        if(field.at("name").get<std::string>().starts_with("position.")) {
            const auto description=field.at("description").get<std::string>();
            require(!description.empty(),"Position aggregate description did not reach Control");
            if(lanes++) require(description==position,"Vector lanes did not inherit their aggregate description");
            else position=description;
        }
    }
    require(lanes==3,"Position descriptor did not expose its three scalar lanes");
}
Json verify_object_directory(Driver& d,const Json& full,const Json& capabilities) {
    const auto limit=capabilities.at("limits").at("inspect_page_items").get<unsigned>();
    const auto bytes=capabilities.at("limits").at("inspect_page_bytes").get<std::size_t>();
    require(limit==256 && bytes<maximum_message,"Object directory limits are not bounded below the transport limit");
    Json expected=Json::array();
    for(const auto& entity:full.at("entities")) {
        expected.push_back({{"handle",entity.at("handle")},{"name",entity.at("name")},{"type",entity.at("type")},
            {"kind","entity"},{"entity",entity.at("handle")},{"parent",entity.at("parent")}});
        for(const auto& component:entity.at("components")) expected.push_back({{"handle",component.at("handle")},
            {"name",component.at("name")},{"type",component.at("type")},{"kind","component"},{"entity",entity.at("handle")}});
    }
    const auto state_token=d.token;
    auto page=d.ok("editor.inspect",{{"detail","objects"},{"limit",1}});
    const auto first_cursor=page.at("next_cursor");Json actual=Json::array();
    for(std::size_t count=0;;++count) {
        require(count<=expected.size() && page.at("objects").size()<=1 && page.dump().size()<bytes,
            "Object directory exceeded its bounds or did not advance");
        for(const auto& entry:page.at("objects")) actual.push_back(entry);
        if(page.at("next_cursor").is_null()) break;
        page=d.ok("editor.inspect",{{"detail","objects"},{"limit",1},{"cursor",page.at("next_cursor")}});
    }
    require(actual==expected && !first_cursor.is_null(),"Object directory omitted, duplicated or expanded objects");
    const auto& entity=full.at("entities")[0];const auto& component=entity.at("components")[0];
    page=d.ok("editor.inspect",{{"detail","objects"},{"entity_name",entity.at("name")},{"limit",1}});
    const auto rest=d.ok("editor.inspect",{{"detail","objects"},{"cursor",page.at("next_cursor")}});
    require(page.at("objects")[0].at("handle")==entity.at("handle") &&
        rest.at("objects")[0].at("handle")==component.at("handle") && rest.at("next_cursor").is_null(),
        "Object cursor did not retain its exact entity-name filter");
    page=d.ok("editor.inspect",{{"detail","objects"},{"type",component.at("type")}});
    require(page.at("objects").size()==1 && page.at("objects")[0].at("handle")==component.at("handle"),
        "Object type filter returned another object");
    require(d.ok("editor.inspect",{{"detail","objects"},{"entity_name","No such entity"}}).at("objects").empty(),
        "Missing entity name returned unrelated objects");
    require(d.ok("editor.inspect",{{"detail","object"},{"object",component.at("handle")}}).at("object")==component,
        "Single-object inspection diverged from the shared reflected object output");
    error(d.call("editor.inspect",{{"detail","objects"},{"limit",0}}),"invalid_request");
    error(d.call("editor.inspect",{{"detail","objects"},{"limit",limit+1}}),"invalid_request");
    error(d.call("editor.inspect",{{"detail","summary"},{"limit",1}}),"invalid_request");
    error(d.call("editor.inspect",{{"detail","object"}}),"invalid_request");
    error(d.call("editor.inspect",{{"detail","object"},{"object","not-a-handle"}}),"stale_object");
    error(d.call("editor.inspect",{{"detail","objects"},{"cursor",first_cursor},{"entity_name","Changed filter"}}),"invalid_request");
    auto foreign=first_cursor;foreign["instance"]="another-instance";
    error(d.call("editor.inspect",{{"detail","objects"},{"cursor",foreign}}),"stale_cursor");
    foreign=first_cursor;foreign["document"]=foreign.at("document").get<std::uint64_t>()+1;
    error(d.call("editor.inspect",{{"detail","objects"},{"cursor",foreign}}),"stale_cursor");
    foreign=first_cursor;foreign["entity"]=full.at("entities").size()+1;
    error(d.call("editor.inspect",{{"detail","objects"},{"cursor",foreign}}),"invalid_request");
    require(d.token==state_token && d.inspect().at("entities")==full.at("entities"),
        "Read-only object discovery changed the document or its state token");
    return first_cursor;
}
void repro_lighting_migration(Driver& d,const std::filesystem::path& repro,const std::filesystem::path& root) {
    const auto original=read(repro/"manifest.json");
    const auto original_scene=read(repro/"Project"/wide(original.at("scene").get<std::string>()));
    require(original.at("view_schema")==2 && original_scene.at("version")==6,
        "Current repro export did not identify view schema 2 and Scene v6");
    const auto legacy=root/"LegacyLightingRepro";
    std::filesystem::copy(atelier::platform::windows_file_path(repro),atelier::platform::windows_file_path(legacy),
        std::filesystem::copy_options::recursive);
    const auto scene_path=legacy/"Project"/wide(original.at("scene").get<std::string>());
    auto old_manifest=original;old_manifest.erase("view_schema");
    old_manifest["view"].erase("direct_candidates");old_manifest["view"].erase("direct_reference");
    old_manifest["view"]["shadow_light_budget"]=16;old_manifest["view"]["shadow_samples"]=64;
    // The legacy lighting repro contains one flat mesh Actor. Encode the real
    // v4 Actor boundary instead of relabeling a v6 Entity directory as v4.
    require(original_scene.at("entities").size()==1 && original_scene.at("roots").size()==1,
        "Legacy lighting migration requires its single-mesh fixture.");
    const auto& current_entity=original_scene.at("entities")[0];
    require(current_entity.at("type")=="world.StaticMeshEntity" && current_entity.at("children").empty() &&
        current_entity.at("components").size()==1 && current_entity.at("components")[0].at("type")=="world.StaticMeshComponent" &&
        current_entity.at("space").at("origin").is_null(),"Legacy lighting fixture acquired a modern-only composition relation.");
    auto old_component=current_entity.at("components")[0];old_component.erase("name");old_component["id"]=1;old_component["parent"]=0;
    for(const auto* field:{"position","rotation","scale"}) old_component[field]=current_entity.at("space").at(field);
    const Json old_actor{{"type","world.StaticMeshActor"},{"name",current_entity.at("name")},{"enabled",current_entity.at("enabled")},
        {"root",1},{"components",Json::array({old_component})},{"properties",current_entity.at("properties")}};
    Json old_scene{{"version",4},{"actors",Json::array({old_actor})},{"world",original_scene.at("world")}};
    old_scene["world"].erase("direct_candidates");
    old_scene["world"]["shadow_light_budget"]=16;old_scene["world"]["shadow_samples"]=64;
    const auto publish=[&](Json manifest,const Json& scene) {
        write(scene_path,scene.dump(2));bool listed=false;
        for(auto& file:manifest.at("files")) if(legacy/wide(file.at("path").get<std::string>())==scene_path) {
            file["sha256"]=sha256_file(scene_path);
            file["bytes"]=std::filesystem::file_size(atelier::platform::windows_file_path(scene_path));listed=true;
        }
        require(listed,"Repro scene was absent from the dependency manifest");
        write(legacy/"manifest.json",manifest.dump(2));
    };
    const auto rejected=[&](const Json& manifest,const Json& scene,std::string_view code="invalid_request") {
        publish(manifest,scene);const auto before=d.inspect();const auto token=d.token;
        error(d.call("repro.open",{{"directory",utf8(legacy.wstring())}}),code);
        const auto after=d.inspect();
        for(const auto* field:{"project","scene_path","entities","selection","inspected_object","camera","view","dirty","undo","redo"})
            require(after.at(field)==before.at(field),std::string("Rejected lighting repro changed ")+field);
        require(d.token==token,"Rejected lighting repro changed the accepted editor state token");
    };
    for(const auto* key:{"shadow_light_budget","shadow_samples"}) {
        const bool samples=std::string_view(key)=="shadow_samples";
        for(const auto& bad:Json::array({-1,samples?65:17,1.5,"8",true})) {
            auto manifest=old_manifest;manifest["view"][key]=bad;rejected(manifest,old_scene);
            auto scene=old_scene;scene["world"][key]=bad;rejected(old_manifest,scene);
        }
        auto current=original;current["view"][key]=1;rejected(current,original_scene);
        auto current_scene=original_scene;current_scene["world"][key]=1;rejected(original,current_scene);
    }
    auto zero_samples=old_manifest;zero_samples["view"]["shadow_samples"]=0;rejected(zero_samples,old_scene);
    auto zero_scene=old_scene;zero_scene["world"]["shadow_samples"]=0;rejected(old_manifest,zero_scene);
    auto mixed=old_manifest;mixed["view"]["direct_candidates"]=8;rejected(mixed,old_scene);
    auto mixed_scene=old_scene;mixed_scene["world"]["direct_candidates"]=8;rejected(old_manifest,mixed_scene);
    auto unknown=original;unknown["view_schema"]=3;rejected(unknown,original_scene);
    auto disagrees=old_manifest;disagrees["view"]["exposure_ev"]=old_scene.at("world").at("exposure_ev").get<double>()+.25;
    rejected(disagrees,old_scene,"dependency_mismatch");

    auto expected_view=original.at("view");expected_view["direct_candidates"]=8;expected_view["direct_reference"]=false;
    auto expected_world=original_scene.at("world");expected_world["direct_candidates"]=8;
    for(unsigned schema:{0u,1u}) {
        auto manifest=old_manifest;auto scene=old_scene;
        if(schema==1) {
            manifest["view_schema"]=1;
            // Both endpoints of the retired ranges migrate to the same defaults.
            manifest["view"]["shadow_light_budget"]=0;manifest["view"]["shadow_samples"]=1;
            scene["world"]["shadow_light_budget"]=0;scene["world"]["shadow_samples"]=1;
        }
        publish(manifest,scene);d.ok("repro.open",{{"directory",utf8(legacy.wstring())}});
        const auto migrated=d.inspect();
        require(migrated.at("view")==expected_view && migrated.at("camera")==original.at("camera") &&
            !migrated.at("dirty").get<bool>() && migrated.at("selection").size()==original.at("selection_indices").size(),
            "Legacy repro lost view/camera/selection or inferred reference mode from old shadow samples");
        const auto id=unique_id();require(d.call("repro.export",Json::object(),id).at("status")=="pending","Migrated repro export did not start");
        const auto exported=d.wait(id);require(exported.at("status")=="ok",exported.dump());
        const std::filesystem::path new_repro=wide(exported.at("result").at("directory").get<std::string>());
        const auto saved=read(new_repro/"manifest.json");
        const auto saved_scene=read(new_repro/"Project"/wide(saved.at("scene").get<std::string>()));
        const auto authored=[](Json entities) {
            for(auto& entity:entities) {entity.erase("id");for(auto& component:entity.at("components")) component.erase("id");}
            return entities;
        };
        require(saved.at("view_schema")==2 && saved.at("view")==expected_view && saved_scene.at("version")==6 &&
            saved_scene.at("world")==expected_world && authored(saved_scene.at("entities"))==authored(original_scene.at("entities")) &&
            saved.at("selection_indices")==original.at("selection_indices"),
            "Migrated repro export lost author data or retained a legacy view/scene format");
        require(!saved.at("view").contains("shadow_light_budget") && !saved.at("view").contains("shadow_samples") &&
            !saved_scene.at("world").contains("direct_reference"),"Migrated repro retained retired author options or persisted reference diagnostics");
        d.ok("repro.open",{{"directory",utf8(new_repro.wstring())}});
        const auto reopened=d.inspect();
        require(reopened.at("view")==expected_view && reopened.at("camera")==original.at("camera") &&
            !reopened.at("dirty").get<bool>() && reopened.at("selection").size()==original.at("selection_indices").size(),
            "New-schema export of the migrated repro did not reopen cleanly");
    }
    // Old Actor indices must survive inserted spatial Entities in the preceding Actor.
    auto expanded_actor=old_actor;
    expanded_actor["components"].push_back(Json{{"id",2},{"type","world.SceneComponent"},{"enabled",true},{"parent",1},
        {"position",{2,3,4}},{"rotation",{0,0,0,1}},{"scale",{1,1,1}}});
    expanded_actor["components"].push_back(Json{{"id",3},{"type","world.CollisionComponent"},{"enabled",true},{"parent",1},
        {"position",{4,5,6}},{"rotation",{0,0,0,1}},{"scale",{1,1,1}}});
    auto other_actor=old_actor;other_actor["name"]="Legacy Other";
    for(unsigned version=1;version<=5;++version) {
        auto scene=old_scene;scene["version"]=version;scene["actors"]=Json::array({expanded_actor,other_actor});
        if(version<4) scene.erase("world");
        if(version==5) scene["world"]=expected_world;
        if(version==1) for(auto& actor:scene["actors"]) {
            actor.erase("properties");for(auto& part:actor["components"]) part.erase("properties");
        }
        auto manifest=old_manifest;manifest["selection_indices"]=Json::array({1});manifest.erase("inspection");
        publish(manifest,scene);d.ok("repro.open",{{"directory",utf8(legacy.wstring())}});
        const auto selected=d.inspect();const auto& entities=selected.at("entities");
        require(entities.size()==4 && selected.at("selection")==Json::array({entities[3].at("handle")}) &&
            entities[3].at("name")=="Legacy Other" && selected.at("inspected_object")==entities[3].at("handle"),
            "Legacy repro Actor selection used expanded Entity indices instead of migrated identities");
        if(version==5) for(unsigned component_index:{1u,2u}) {
            manifest["selection_indices"]=Json::array({0});manifest["inspection"]={{"actor",0},{"component",component_index}};
            publish(manifest,scene);d.ok("repro.open",{{"directory",utf8(legacy.wstring())}});
            const auto inspected=d.inspect();const auto& owner=inspected.at("entities")[component_index];
            require(inspected.at("selection")==Json::array({owner.at("handle")}) &&
                owner.at("parent")==inspected.at("entities")[0].at("handle"),
                "Legacy spatial component inspection lost its actual migrated owner Entity");
            if(component_index==1) require(owner.at("components").empty() && inspected.at("inspected_object")==owner.at("handle") &&
                owner.at("transform").at("position")==Json::array({2,3,4}),
                "Legacy pure SceneComponent did not inspect its mapped spatial Entity");
            else require(owner.at("components").size()==1 && owner.at("components")[0].at("type")=="world.CollisionComponent" &&
                inspected.at("inspected_object")==owner.at("components")[0].at("handle"),
                "Legacy Primitive inspection guessed the old Actor owner instead of the bound child Entity");
        }
    }
    // Restore the caller's repro after exercising expanded legacy object graphs.
    d.ok("repro.open",{{"directory",utf8(repro.wstring())}});
}
void repro_lighting(const std::filesystem::path& executable,const std::filesystem::path& root) {
    const auto project=root/"Project",directory=root/"Control";project_fixture(project);
    Process editor(executable,L"--project "+quote(project)+L" --control-directory "+quote(directory)+L" --no-activate",root/"editor.log");
    Driver d(discover_editor(editor,directory));d.inspect();d.ok("scene.create",{{"kind","mesh"}});
    d.ok("camera.look_at",{{"eye",{8,5,-10}},{"target",{1,2,3}}});
    d.ok("view.set",{{"patch",{{"direct_candidates",16},{"direct_reference",true},{"exposure_ev",1.25}}}});
    d.ok("scene.save");const auto id=unique_id();
    require(d.call("repro.export",Json::object(),id).at("status")=="pending","Lighting repro export did not start");
    const auto exported=d.wait(id);require(exported.at("status")=="ok",exported.dump());
    repro_lighting_migration(d,wide(exported.at("result").at("directory").get<std::string>()),root);
    d.client.reset();editor.close();
    write(root/"result.json",Json{{"status","passed"},{"cases",{"legacy-shadow-ranges","legacy-view-and-world-agreement",
        "rejected-repro-preserves-state","unmarked-and-schema1-migration","schema2-scene6-export-reopen","no-implicit-reference"}}}.dump(2));
    std::cout<<"Editor Repro Lighting E2E: PASS | "<<utf8(root.wstring())<<'\n';
}
void reject_nested_destination(const std::filesystem::path& executable,const std::filesystem::path& root) {
    const auto project=root/"NestedProject";project_fixture(project);const auto directory=project/"Content/Control";
    Process editor(executable,L"--project "+quote(project)+L" --control-directory "+quote(directory)+L" --no-activate",root/"nested-editor.log");
    Driver d(discover_editor(editor,directory));d.inspect();const auto id=unique_id();
    require(d.call("repro.export",Json::object(),id).at("status")=="pending","Nested output fixture did not start");
    error(d.wait(id),"invalid_destination");
    require(std::filesystem::is_empty(atelier::platform::windows_file_path(directory/"artifacts"/wide(d.descriptor.at("instance_id").get<std::string>()))),"Rejected nested export left a recursive output");
    d.client.reset();editor.close();
}
void repro_paths(const std::filesystem::path& executable,const std::filesystem::path& root,bool deep_control) {
    const auto project=root/"Project";
    const auto directory=deep_control?root/"Control"/std::wstring(100,L'x'):root/"Control";
    project_fixture(project);
    const auto scene=std::filesystem::path("Content/Scenes")/("Control-"+unique_id()+".scene");
    std::filesystem::rename(project/"Content/Scenes/Main.scene",project/scene);
    auto config=read(project/"Config/Project.json");config["startup_scene"]=utf8(scene.generic_wstring());
    write(project/"Config/Project.json",config.dump());
    Process editor(executable,L"--project "+quote(project)+L" --control-directory "+quote(directory)+L" --no-activate",root/"editor.log");
    Driver d(discover_editor(editor,directory));
    unsigned stable{};
    for(unsigned i=0;i<200 && stable<5;++i) {
        const auto before=d.token;d.inspect();stable=before==d.token?stable+1:0;std::this_thread::sleep_for(30ms);
    }
    require(stable==5,"Project paths fixture did not stabilize");
    d.ok("scene.create",{{"kind","mesh"}});
    const auto unpublished=std::filesystem::path("Source")/(std::wstring(72,L'u')+L".source");
    write(project/unpublished,"invalid saved source");std::this_thread::sleep_for(750ms);d.inspect();
    std::filesystem::path repro;
    for(unsigned pass=0;pass<2;++pass) {
        const auto id=unique_id();const auto started=d.call("repro.export",Json::object(),id);require(started.at("status")=="pending",started.dump());
        const auto exported=d.wait(id);require(exported.at("status")=="ok",exported.dump());
        repro=wide(exported.at("result").at("directory").get<std::string>());
        const auto manifest=read(repro/"manifest.json");const auto frozen=repro/"Project"/wide(manifest.at("scene").get<std::string>());
        require(frozen.wstring().size()>260 && read(frozen).at("entities").size()==1,"Deep-path export did not preserve the scene");
        if(pass==0) require(std::filesystem::exists(atelier::platform::windows_file_path(repro/"unpublished"/unpublished)),"Deep-path source archive is missing");
        if(pass==0) d.ok("scene.undo");
        d.ok("repro.open",{{"directory",utf8(repro.wstring())}});
        const auto reopened=d.inspect();require(reopened.at("entities").size()==1 && !reopened.at("dirty").get<bool>(),"Deep-path repro did not reopen");
        d.ok("scene.save");
    }
    const auto instance=d.descriptor.at("instance_id").get<std::string>();
    const auto request_path=repro/"Project/Cache"/(std::wstring(80,L'q')+L".request.json");
    require(request_path.wstring().size()>260,"CLI request fixture is not a long path");
    write(request_path,Json{{"op","editor.inspect"}}.dump());d.client.reset();
    Process cli(executable.parent_path()/"atelier_control.exe",L"--directory "+quote(directory)+L" --instance "+wide(instance)+L" --request "+quote(request_path),root/"cli.json");
    require(WaitForSingleObject(cli.process,30000)==WAIT_OBJECT_0 && read(root/"cli.json").at("status")=="ok","CLI could not read a deep request or discover the editor");
    editor.close();
    require(!std::filesystem::exists(atelier::platform::windows_file_path(directory/"instances"/(instance+".json"))),"Deep-path instance descriptor was not removed on close");
    write(root/"result.json",Json{{"status","passed"},{"cases",{"long-destination","long-source","deep-project-reopen-save","source-archive","cli-request","descriptor-cleanup"}},{"deep_control",deep_control},{"last_repro",utf8(repro.wstring())}}.dump(2));
    std::cout<<"Editor Control path E2E: PASS | "<<utf8(root.wstring())<<'\n';
}
void ddgi_debug(const std::filesystem::path& executable,const std::filesystem::path& root) {
    const auto project=root/"Project",directory=root/"Control";project_fixture(project);
    Process editor(executable,L"--project "+quote(project)+L" --control-directory "+quote(directory)+L" --no-activate",root/"editor.log");
    Driver d(discover_editor(editor,directory));
    if(auto window=editor.window())SetWindowPos(window,HWND_TOPMOST,12,80,960,640,SWP_NOACTIVATE);
    auto state=d.inspect();unsigned stable=0;
    for(unsigned i=0;i<200 && stable<5;++i) {
        const auto token=d.token;std::this_thread::sleep_for(30ms);state=d.inspect();
        stable=token==d.token&&!state.at("last_frame").empty()?stable+1:0;
    }
    require(stable==5,"DDGI diagnostic editor did not settle");
    const auto default_view=state.at("view");
    const auto capabilities=d.ok("capabilities");
    verify_option_descriptions(capabilities);
    for(const char* name:{"probe_visualization","probe_volume","probe_relocation","probe_xray","probe_radius","probe_debug",
        "probe_bounce","indirect_gain","probe_feedback_visibility","probe_coverage","probe_debug_volume"})
        require(capabilities.at("display_options").contains(name),"DDGI debug option absent from shared Control schema");
    for(unsigned slot=0;slot<4;++slot) for(const char* field:{"enabled","placement","min_x","min_y","min_z","max_x","max_y","max_z","count_x","count_y","count_z","blend_cells"})
        require(capabilities.at("display_options").contains("probe_volume_"+std::to_string(slot)+"_"+field),"Volume field absent from Control schema");
    const auto summary=d.ok("editor.inspect",{{"detail","summary"}});
    require(!summary.contains("entities") && !summary.contains("assets") && !summary.contains("component_types") &&
        summary.at("view")==state.at("view") && summary.contains("last_frame"),"Summary inspection enumerated the document or omitted view state");
    d.ok("scene.create",{{"kind","mesh"}});
    d.ok("camera.look_at",{{"eye",{5,4,-7}},{"target",{0,0,0}}});
    Json patch{{"shading","scene_lit"},{"ddgi",true},{"gtao",false},{"ssil",false},{"sssr",false},{"bloom",false},
        {"probe_count_x",3},{"probe_count_y",3},{"probe_count_z",3},{"probe_update_budget",27},{"probe_rays",32},
        {"probe_min_x",-2},{"probe_min_y",-2},{"probe_min_z",-2},{"probe_max_x",2},{"probe_max_y",2},{"probe_max_z",2},
        {"probe_visualization","state"},{"probe_volume",true},{"probe_relocation",true},{"probe_xray",true},{"probe_radius",.12}};
    d.ok("view.set",{{"patch",patch}});state=d.inspect();const auto base_view=state.at("view"),entities=state.at("entities"),undo=state.at("undo");
    const auto token=d.token;d.ok("view.set",{{"patch",patch}});require(d.token==token,"Repeated DDGI view patch changed the state token");
    error(d.call("view.set",{{"patch",{{"probe_radius",0},{"probe_debug",true}}}}),"operation_failed");
    require(d.inspect().at("view")==base_view,"Rejected debug patch partially mutated the view");
    error(d.call("view.set",{{"patch",{{"probe_visualization","unknown"}}}}),"invalid_request");
    error(d.call("view.set",{{"patch",{{"probe_min_x",-3},{"probe_volume_0_min_x",-3}}}}),"invalid_request");
    require(d.inspect().at("view")==base_view,"Duplicate slot-zero aliases partially published settings");
    const auto id=unique_id();
    const Json cases=Json::array({
        {{"name","probe-state"},{"patch",Json::object()}},
        {{"name","probe-age"},{"patch",{{"probe_visualization","update_age"}}}},
        {{"name","receiver-coverage"},{"patch",{{"probe_visualization","off"},{"probe_volume",false},{"probe_relocation",false},{"probe_debug",true}}}},
        {{"name","ddgi-disabled"},{"patch",{{"ddgi",false},{"probe_debug",true}}}}});
    auto pending=d.call("render.compare",{{"cases",cases},{"sample_frames",{2}},{"timeout_ms",120000}},id);
    require(pending.at("status")=="pending",pending.dump());const auto compared=d.wait(id);require(compared.at("status")=="ok",compared.dump());
    require(compared.at("result").at("samples").size()==4,"DDGI comparison did not produce every accepted sample");
    state=d.inspect();require(state.at("view")==base_view&&state.at("entities")==entities&&state.at("undo")==undo,
        "DDGI diagnostic comparison changed scene, undo or base display policy");
    write(root/"captures.json",compared.dump(2));write(root/"capabilities.json",capabilities.dump(2));
    d.ok("view.set",{{"patch",{{"probe_bounce",.5},{"indirect_gain",1.25},{"probe_feedback_visibility",false}}}});
    d.ok("view.layout",{{"layout","fit_scene"}});state=d.inspect();
    require(state.at("view").at("probe_volume_0_placement")=="fixed" && state.at("view").at("probe_bounce")==.5 &&
        state.at("view").at("indirect_gain")==1.25,"Fit Scene changed propagation preferences");
    d.ok("view.layout",{{"layout","scene_focus"}});state=d.inspect();
    const auto focus_view=state.at("view");const auto layout_token=d.token;
    require(focus_view.at("probe_volume_0_placement")=="follow_focus" && focus_view.at("probe_volume_1_enabled")==true &&
        focus_view.at("probe_volume_1_placement")=="fixed" && state.at("entities")==entities && state.at("undo").get<unsigned>()==undo.get<unsigned>()+3,
        "Scene+Focus layout lost a volume or did not commit to scene history");
    d.ok("view.layout",{{"layout","scene_focus"}});require(d.token==layout_token,"Repeated layout changed the state token");
    for(const auto& invalid:std::vector<Json>{
        {{"probe_volume_0_count_y",4}},
        {{"probe_volume_0_count_x",32},{"probe_volume_0_count_y",32},{"probe_volume_0_count_z",32}},
        {{"probe_volume_0_max_x",-6}},
        {{"probe_volume_2_placement","follow_focus"},{"probe_volume_2_count_x",3}}}) {
        error(d.call("view.set",{{"patch",invalid}}),"operation_failed");
        require(d.ok("editor.inspect",{{"detail","summary"}}).at("view")==focus_view && d.token==layout_token,
            "Rejected volume configuration changed state or token");
    }
    d.ok("camera.look_at",{{"eye",{12,4,-7}},{"target",{7,0,0}}});
    const auto capture_id=unique_id();
    pending=d.call("render.capture",{{"sample_frames",{2}},{"timeout_ms",120000}},capture_id);
    require(pending.at("status")=="pending",pending.dump());const auto captured=d.wait(capture_id);
    require(captured.at("status")=="ok",captured.dump());const auto& frame=captured.at("result").at("samples").at(0);
    const auto& volumes=frame.at("frame_stats").at("probe_volumes");
    require(frame.at("lighting_focus")==Json::array({7,0,0}) && volumes.size()==4 && volumes.at(0).at("rolling")==true &&
        volumes.at(0).at("minimum").at(0).get<float>()>=0 && volumes.at(1).at("enabled")==true &&
        frame.at("frame_stats").at("probe_volumes_source")=="cpu_layout_and_schedule",
        "Accepted frame did not report camera-target rolling volume placement and CPU schedule provenance");
    write(root/"focus-capture.json",captured.dump(2));
    const Json tiny{{"probe_volume_0_count_x",5},{"probe_volume_0_count_y",5},{"probe_volume_0_count_z",5},
        {"probe_volume_0_min_x",-.004},{"probe_volume_0_min_y",-.004},{"probe_volume_0_min_z",-.004},
        {"probe_volume_0_max_x",.004},{"probe_volume_0_max_y",.004},{"probe_volume_0_max_z",.004}};
    d.ok("camera.look_at",{{"eye",{100005,4,-7}},{"target",{100000,0,0}}});
    const auto far_state=d.ok("editor.inspect",{{"detail","summary"}});const auto far_token=d.token;
    error(d.call("view.set",{{"patch",tiny}}),"operation_failed");
    require(d.ok("editor.inspect",{{"detail","summary"}}).at("view")==far_state.at("view") && d.token==far_token,
        "World-precision failure published unusable view settings");
    d.ok("camera.look_at",{{"eye",{5,4,-7}},{"target",{0,0,0}}});d.ok("view.set",{{"patch",tiny}});
    const auto small_state=d.ok("editor.inspect",{{"detail","summary"}});const auto small_token=d.token;
    error(d.call("camera.look_at",{{"eye",{100005,4,-7}},{"target",{100000,0,0}}}),"operation_failed");
    auto far_camera=small_state.at("camera");far_camera["target"]=Json::array({100000,0,0});
    error(d.call("camera.set",{{"state",far_camera}}),"operation_failed");
    require(d.ok("editor.inspect",{{"detail","summary"}}).at("camera")==small_state.at("camera") && d.token==small_token,
        "World-precision failure published an unusable camera");
    d.ok("view.layout",{{"layout","scene_focus"}});
    d.ok("view.set",{{"patch",{{"probe_volume_2_enabled",true},{"probe_volume_3_enabled",true},
        {"probe_volume_2_count_x",3},{"probe_volume_3_count_x",2},{"probe_debug_volume",2},{"probe_coverage","contributors"}}}});
    const auto repro_view=d.inspect().at("view");const auto export_id=unique_id();
    require(d.call("repro.export",Json::object(),export_id).at("status")=="pending","Multi-volume export did not start");
    const auto exported=d.wait(export_id);require(exported.at("status")=="ok",exported.dump());
    const std::filesystem::path repro=wide(exported.at("result").at("directory").get<std::string>());
    auto manifest=read(repro/"manifest.json");require(manifest.at("view")==repro_view,"Repro lost canonical volume settings");
    d.ok("scene.undo");d.ok("scene.save",{{"path","Content/Scenes/Editing.scene"}});d.ok("repro.open",{{"directory",utf8(repro.wstring())}});
    require(d.inspect().at("view")==repro_view,"Repro did not restore all four volume settings");
    for(const char* field:{"min_x","min_y","min_z","max_x","max_y","max_z","count_x","count_y","count_z"}) {
        const auto canonical=std::string("probe_volume_0_")+field;
        manifest["view"][std::string("probe_")+field]=manifest.at("view").at(canonical);manifest["view"].erase(canonical);
    }
    write(repro/"manifest.json",manifest.dump(2));d.ok("repro.open",{{"directory",utf8(repro.wstring())}});
    require(d.inspect().at("view")==repro_view,"Legacy slot-zero names changed canonical repro settings");
    // A genuine older repro includes a v3 scene, not a v4 scene that disagrees with its snapshot.
    const auto config=read(repro/"Project/Config/Project.json");
    const auto legacy_scene_path=repro/"Project"/wide(config.at("startup_scene").get<std::string>());
    auto legacy_scene=read(legacy_scene_path);legacy_scene["version"]=3;legacy_scene.erase("world");write(legacy_scene_path,legacy_scene.dump(2));
    for(auto& file:manifest["files"]) if(repro/wide(file.at("path").get<std::string>())==legacy_scene_path) {
        file["sha256"]=sha256_file(legacy_scene_path);file["bytes"]=std::filesystem::file_size(legacy_scene_path);
    }
    auto legacy_expected=repro_view;
    for(unsigned slot=0;slot<4;++slot) for(const char* field:{"enabled","placement","min_x","min_y","min_z","max_x","max_y","max_z","count_x","count_y","count_z","blend_cells"}) {
        const auto name="probe_volume_"+std::to_string(slot)+"_"+field;manifest["view"].erase(name);legacy_expected[name]=default_view.at(name);
    }
    for(const char* name:{"probe_bounce","indirect_gain","probe_feedback_visibility","probe_debug_volume","probe_coverage"}) {
        manifest["view"].erase(name);legacy_expected[name]=default_view.at(name);
    }
    for(const char* field:{"min_x","min_y","min_z","max_x","max_y","max_z","count_x","count_y","count_z"}) {
        const auto canonical=std::string("probe_volume_0_")+field;
        manifest["view"][std::string("probe_")+field]=base_view.at(canonical);legacy_expected[canonical]=base_view.at(canonical);
    }
    manifest["view"]["probe_count_y"]=4;legacy_expected["probe_volume_0_count_y"]=4;
    write(repro/"manifest.json",manifest.dump(2));d.ok("repro.open",{{"directory",utf8(repro.wstring())}});
    require(d.inspect().at("view")==legacy_expected,"A genuine old repro inherited current volume or propagation settings");
    d.client.reset();editor.close();
    require(read(project/"Content/Scenes/Main.scene").at("entities").empty(),"Diagnostic session changed the saved scene");
    write(root/"result.json",Json{{"status","passed"},{"cases",{"shared-schema","summary-inspection","idempotent-patch","atomic-rejection","real-render-compare",
        "coverage-presets","rolling-focus-capture","focus-precision-atomicity","four-volume-repro","legacy-aliases","legacy-repro-defaults","history-and-scene-isolation","clean-close"}}}.dump(2));
    std::cout<<"Editor DDGI Control E2E: PASS | "<<utf8(root.wstring())<<'\n';
}
#include "client_size.inl"
}
int wmain(int argc,wchar_t** argv) {
    try {
        require(argc==3 || (argc==4 && (std::wstring_view(argv[3])==L"--repro-paths" || std::wstring_view(argv[3])==L"--ddgi-debug" || std::wstring_view(argv[3])==L"--repro-lighting" || std::wstring_view(argv[3])==L"--descriptions" || std::wstring_view(argv[3])==L"--client-size")),"Usage: atelier_editor_control_acceptance editor.exe output-directory [--repro-paths|--ddgi-debug|--repro-lighting|--descriptions|--client-size]");
        const bool descriptions=argc==4 && std::wstring_view(argv[3])==L"--descriptions";
        const auto root=std::filesystem::absolute(argv[2])/unique_id();std::filesystem::create_directories(root);
        if(argc==4 && std::wstring_view(argv[3])==L"--client-size") {client_size(argv[1],root);return 0;}
        if(argc==4 && std::wstring_view(argv[3])==L"--ddgi-debug") {ddgi_debug(argv[1],root);return 0;}
        if(argc==4 && std::wstring_view(argv[3])==L"--repro-lighting") {repro_lighting(argv[1],root);return 0;}
        if(argc==4 && !descriptions) {
            repro_paths(argv[1],root/"exports",false);repro_paths(argv[1],root/"deep-control",true);
            reject_nested_destination(argv[1],root);
            write(root/"result.json",Json{{"status","passed"},{"cases",{"nested-exports","deep-control","nested-destination-rejected"}}}.dump(2));
            return 0;
        }
        const auto directory=root/"Control";const auto project=root/"Project";
        project_fixture(project);
        auto arguments=L"--project "+quote(project)+L" --control-directory "+quote(directory)+L" --no-activate";
        if(!descriptions) arguments+=L" --gpu-validation";
        Process editor(argv[1],std::move(arguments),root/"editor.log");
        const auto descriptor=discover_editor(editor,directory);
        // A small, visible owned test window can render without taking keyboard focus.
        if(auto window=editor.window()) {
            if(descriptions) SetWindowPos(window,nullptr,0,0,960,640,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            else SetWindowPos(window,HWND_TOPMOST,12,80,960,640,SWP_NOACTIVATE);
        }
        Driver d(descriptor);const auto caps=d.ok("capabilities");require(caps.at("operations").size()>=20,"Capabilities are incomplete");
        verify_option_descriptions(caps);
        auto initial=d.inspect();unsigned stable{};
        for(unsigned i=0;i<200 && stable<5;++i) {
            const auto token=d.token;std::this_thread::sleep_for(30ms);initial=d.inspect();
            stable=token==d.token && !initial.at("last_frame").empty()?stable+1:0;
        }
        require(stable==5,"Initial surface mapping did not stabilize");
        require(initial.at("entities").empty() && !initial.at("last_frame").empty(),"Initial editor state is not ready");
        const auto empty_directory=d.ok("editor.inspect",{{"detail","objects"}});
        require(empty_directory.at("objects").empty() && empty_directory.at("next_cursor").is_null(),"Empty object directory has a continuation");
        const auto original_scene=read(project/"Content/Scenes/Main.scene");
        const auto original_scene_hash=descriptions?sha256_file(project/"Content/Scenes/Main.scene"):std::string{};
        Json request{{"op","scene.create"},{"args",{{"kind","mesh"}}},{"state_token",d.token},{"request_id",unique_id()}};
        const auto first=d.client->call(request);require(first.at("status")=="ok",first.dump());
        const auto replay=d.client->call(request);require(first==replay,"Duplicate mutation did not replay its response");
        request["args"]["kind"]="entity";error(d.client->call(request),"request_id_conflict");
        auto current=d.inspect();require(current.at("entities").size()==1 && current.at("undo")==1,"Duplicate request repeated creation");
        verify_property_descriptions(current.at("entities")[0]);
        const auto directory_cursor=verify_object_directory(d,current,caps);
        if(descriptions) {
            write(root/"capabilities.json",caps.dump(2));
            write(root/"properties.json",current.at("entities")[0].dump(2));
            d.ok("scene.undo");const auto restored=d.inspect();
            error(d.call("editor.inspect",{{"detail","objects"},{"cursor",directory_cursor}}),"stale_cursor");
            error(d.call("editor.inspect",{{"detail","object"},{"object",current.at("entities")[0].at("handle")}}),"stale_object");
            require(restored.at("entities").empty() && !restored.at("dirty").get<bool>() &&
                restored.at("undo")==initial.at("undo") && restored.at("view")==initial.at("view"),
                "Description inspection or cleanup changed saved document state.");
            // Placeholders report the saved type they stand in for and the data they keep.
            write(project/"Content/Scenes/Placeholder.scene",Json{{"version",6},{"next_id","3"},{"roots",{"1"}},{"materials",Json::array()},{"world",Json::object()},
                {"entities",{{{"id","1"},{"type","world.Entity"},{"name","Holder"},{"enabled",true},{"properties",Json::object()},{"children",Json::array()},{"space",nullptr},
                    {"components",{{{"id","2"},{"type","project.Gone"},{"name",""},{"enabled",true},{"properties",{{"strength",3}}}}}}}}}}.dump());
            d.ok("scene.open",{{"path","Content/Scenes/Placeholder.scene"}});
            const auto placed=d.ok("editor.inspect",{{"detail","objects"}}).at("objects");
            const auto gone=std::ranges::find_if(placed,[](const Json& entry){return entry.at("kind")=="component";});
            require(placed.size()==2 && !placed[0].contains("placeholder") && !placed[0].contains("saved_type") && gone!=placed.end() &&
                gone->at("placeholder")==true && gone->at("saved_type")=="project.Gone" && gone->at("type")=="world.Component",
                "Object directory did not identify a placeholder and its saved type.");
            const auto detail=d.ok("editor.inspect",{{"detail","object"},{"object",gone->at("handle")}}).at("object");
            require(detail.at("saved_type")=="project.Gone" && detail.at("saved_data").size()==1 &&
                detail.at("saved_data")[0].at("path")=="properties.strength" && detail.at("saved_data")[0].at("value")=="3" &&
                detail.at("saved_data")[0].at("kind")=="unknown_field","Object inspection did not report a placeholder's saved data.");
            d.client.reset();editor.close();
            require(discover(directory).empty(),"Description editor descriptor remained live after close.");
            require(read(project/"Content/Scenes/Main.scene")==original_scene &&
                sha256_file(project/"Content/Scenes/Main.scene")==original_scene_hash,
                "Description inspection changed the original saved scene.");
            write(root/"result.json",Json{{"status","passed"},{"cases",{"discovery","shared-option-descriptions","utf8-newlines",
                "aggregate-field-descriptions","bounded-object-directory","single-object-inspection","stale-cursor-and-object",
                "undo-to-clean","placeholder-inspection","saved-scene-preserved","clean-close"}}}.dump(2));
            std::cout<<"Editor Control descriptions E2E: PASS | "<<utf8(root.wstring())<<'\n';return 0;
        }
        auto entity=current.at("entities")[0].at("handle").get<std::string>();auto component=current.at("entities")[0].at("components")[0].at("handle").get<std::string>();
        const auto stale=d.token;
        d.ok("scene.rename",{{"object",entity},{"name","Control cube"}});
        error(d.call("editor.inspect",{{"detail","objects"},{"cursor",directory_cursor}}),"stale_cursor");
        error(d.client->call({{"op","scene.rename"},{"args",{{"object",entity},{"name","Wrong"}}},{"state_token",stale}}),"stale_state");
        d.ok("scene.transform",{{"object",entity},{"position",{1,2,3}},{"rotation",{0,0,0,1}},{"scale",{1,1,1}}});
        d.ok("scene.set_property",{{"object",entity},{"property","position.x"},{"value",4}});
        current=d.inspect();require(current.at("entities")[0].at("transform").at("position")[0]==4,"Property command did not write the document");
        d.ok("scene.undo");current=d.inspect();require(current.at("entities")[0].at("transform").at("position")[0]==1,"Undo did not restore the transform");d.ok("scene.redo");
        d.ok("scene.select",{{"entities",{entity}}});
        const auto added=d.ok("scene.add_component",{{"type","world.Component"}}).at("object");
        current=d.inspect();require(current.at("entities")[0].at("components").size()==2,"Component addition failed");
        d.ok("scene.remove_component",{{"object",added}});current=d.inspect();require(current.at("entities")[0].at("components").size()==1 &&
            current.at("entities")[0].at("components")[0].at("handle")==component,"Component removal failed or replaced the existing mesh identity");
        const auto camera=current.at("camera");d.ok("camera.look_at",{{"eye",{8,5,-10}},{"target",{0,0,0}}});d.ok("camera.set",{{"state",camera}});
        auto invalid_camera=camera;invalid_camera["distance"]=0.0000000001;error(d.call("camera.set",{{"state",invalid_camera}}),"invalid_request");
        error(d.call("view.set",{{"patch",{{"gtao",true},{"ssil",true}}}}),"invalid_request");
        d.ok("view.set",{{"patch",{{"ssil",true}}}});current=d.inspect();require(current.at("view").at("ssil")==true && current.at("view").at("gtao")==false,"Display policy did not normalize GTAO / Local GI");
        d.ok("view.set",{{"patch",{{"ssil",false},{"shadow_bias",0.1}}}});
        error(d.call("view.set",{{"patch",{{"shadows","wrong"}}}}),"invalid_request");
        error(d.call("scene.rename",{{"object","stale/999"},{"name","No"}}),"stale_object");
        error(d.call("project.open",{{"path",utf8(project.wstring())}}),"unsaved_changes");
        // Saved but unpublished code must not silently enter the runnable repro.
        write(project/"Source/NeverPublished.source","invalid saved source");
        write(project/"Source/Published.ui.source","invalid saved UI");
        std::this_thread::sleep_for(750ms);d.inspect();
        current=d.inspect();const auto dirty_undo=current.at("undo");const auto dirty_path=current.at("scene_path");
        const auto export_id=unique_id();require(d.call("repro.export",Json::object(),export_id).at("status")=="pending","Export did not start");
        auto exported=d.wait(export_id);require(exported.at("status")=="ok",exported.dump());
        const std::filesystem::path repro=wide(exported.at("result").at("directory").get<std::string>());
        const auto manifest=read(repro/"manifest.json");const auto frozen=read(repro/"Project"/wide(manifest.at("scene").get<std::string>()));
        require(!std::filesystem::exists(repro/"Project/Source/NeverPublished.source") &&
            std::filesystem::exists(repro/"unpublished/Source/NeverPublished.source") &&
            std::filesystem::exists(repro/"unpublished/Source/Published.ui.source") &&
            !std::filesystem::exists(repro/"unpublished/Source/Published.source"),"Repro mixed live and unpublished source inputs");
        require(sha256_file(repro/"Project/Source/Published.ui.source")==sha256(std::as_bytes(std::span(published_ui))),"Repro lost the published UI source");
        require(manifest.at("publication_version")!=0 && manifest.at("ui_source_version")!=0,"Repro did not identify the full publication");
        require(frozen.at("entities").size()==1,"Export did not contain the dirty memory scene");
        require(read(project/"Content/Scenes/Main.scene")==original_scene,"Repro export overwrote the original scene");
        current=d.inspect();require(current.at("dirty")==true && current.at("undo")==dirty_undo && current.at("scene_path")==dirty_path,"Repro export changed authoring state");
        d.ok("camera.look_at",{{"eye",{9,7,-8}},{"target",{4,2,3}}});
        const auto base_view=current.at("view");const auto compare_id=unique_id();
        const auto prepare_id=unique_id();auto preparing=d.call("render.prepare",Json::object(),prepare_id);require(preparing.at("status")=="pending",preparing.dump());
        std::cout<<"Editor Control: preparing cold Scene Lit pipelines\n"<<std::flush;
        auto prepared=d.wait(prepare_id,605s);require(prepared.at("status")=="ok",prepared.dump());
        require(prepared.at("result").at("phase")=="ready","Preparation did not report an accepted ready frame");
        std::cout<<"Editor Control: preparation "<<prepared.at("result").at("preparation_ms")<<" ms\n"<<std::flush;
        d.inspect();
        auto pending=d.call("render.compare",{{"cases",Json::array({{{"name","baseline"},{"patch",Json::object()}},{{"name","scene_lit"},{"patch",{{"shading","scene_lit"},{"shadows",false},{"ddgi",false},{"gtao",false},{"ssil",false}}}}})},{"sample_frames",{1,3}}},compare_id);
        require(pending.at("status")=="pending",pending.dump());auto compared=d.wait(compare_id);require(compared.at("status")=="ok",compared.dump());
        const auto& samples=compared.at("result").at("samples");require(samples.size()==4,"Comparison sample count is incorrect");
        require(samples[0].at("accepted_ordinal")==1 && samples[1].at("accepted_ordinal")==3 && samples[2].at("accepted_ordinal")==1,"Accepted frame ordinals are incorrect");
        require(samples[2].at("effective_view").at("shading")=="scene_lit" && samples[2].at("environment")==Json::array({0,0,0}),"Scene Lit settings or explicit zero-sky contract changed");
        for(const auto& sample:samples) {
            const std::filesystem::path output=wide(compared.at("result").at("directory").get<std::string>());
            const auto width=sample.at("extent")[0].get<std::uint64_t>(),height=sample.at("extent")[1].get<std::uint64_t>();
            require(std::filesystem::file_size(output/wide(sample.at("depth").get<std::string>()))==width*height*4,"Depth readback size is incorrect");
            require(std::filesystem::file_size(output/wide(sample.at("color").get<std::string>()))>100,"PNG readback is missing");
            std::ifstream depth_file(atelier::platform::windows_file_path(output/wide(sample.at("depth").get<std::string>())),std::ios::binary);
            std::vector<float> depth(static_cast<std::size_t>(width*height));depth_file.read(reinterpret_cast<char*>(depth.data()),static_cast<std::streamsize>(depth.size()*sizeof(float)));
            require(bool(depth_file) && std::ranges::all_of(depth,[](float value){return std::isfinite(value) && value>=0 && value<=1;}) &&
                std::ranges::any_of(depth,[](float value){return value>0;}),"Depth capture does not contain valid scene geometry");
        }
        current=d.inspect();require(current.at("view")==base_view && current.at("undo")==dirty_undo,"Temporary comparison leaked into authoring state");
        auto cancelled_id=unique_id();
        const Json cancelled_request{{"op","render.capture"},{"state_token",d.token},{"request_id",cancelled_id},{"args",{{"sample_frames",{4096}}}}};
        pending=d.client->call(cancelled_request);require(pending.at("status")=="pending",pending.dump());
        const auto unchanged=d.token;
        error(d.call("scene.transform",{{"object",entity},{"position",{4,2,3}},{"rotation",{0,0,0,1}},{"scale",{0,1,1}}}),"operation_failed");
        error(d.call("scene.set_property",{{"object",entity},{"property","scale.x"},{"value",0}}),"operation_failed");
        error(d.call("view.set",{{"patch",{{"gtao",true},{"ssil",true}}}}),"invalid_request");
        d.ok("camera.set",{{"state",d.inspect().at("camera")}});d.ok("view.set",{{"patch",Json::object()}});
        require(d.token==unchanged && d.call("job.status",{{"job_id",cancelled_id}}).at("status")=="pending","Rejected or no-op requests cancelled unchanged rendering");
        error(d.call("job.cancel",{{"job_id",cancelled_id}}),"cancelled");error(d.call("job.cancel",{{"job_id",cancelled_id}}),"cancelled");
        for(unsigned i=0;i<260;++i) d.ok("view.set",{{"patch",Json::object()}});
        error(d.client->call(cancelled_request),"request_id_expired");
        error(d.call("job.status",{{"job_id",cancelled_id}}),"cancelled");
        require(!d.inspect().contains("active_job"),"Expired request started another job with the same identity");
        const Json retired_request{{"op","render.capture"},{"state_token",d.token},{"request_id",unique_id()},{"args",{{"sample_frames",{4096}}}}};
        pending=d.client->call(retired_request);require(pending.at("status")=="pending",pending.dump());
        error(d.call("job.cancel",{{"job_id",retired_request.at("request_id")}}),"cancelled");
        for(unsigned i=0;i<17;++i) {
            const auto id=unique_id();require(d.call("render.capture",{{"sample_frames",{4096}}},id).at("status")=="pending","Retention fixture did not start");
            error(d.call("job.cancel",{{"job_id",id}}),"cancelled");
        }
        error(d.call("job.status",{{"job_id",retired_request.at("request_id")}}),"unknown_job");
        error(d.client->call(retired_request),"cancelled");
        auto changed_id=unique_id();pending=d.call("render.capture",{{"sample_frames",{4096}}},changed_id);require(pending.at("status")=="pending",pending.dump());
        d.ok("scene.rename",{{"object",entity},{"name","Changed during capture"}});error(d.wait(changed_id),"state_changed");
        auto timed_id=unique_id();pending=d.call("render.capture",{{"sample_frames",{4096}},{"timeout_ms",1}},timed_id);require(pending.at("status")=="pending",pending.dump());error(d.wait(timed_id),"timeout");
        const auto disconnected_id=unique_id();pending=d.call("render.capture",{{"sample_frames",{4096}}},disconnected_id);require(pending.at("status")=="pending",pending.dump());
        d.client.reset();std::this_thread::sleep_for(80ms);d.client=std::make_unique<Client>(descriptor);error(d.wait(disconnected_id),"cancelled");
        d.inspect();d.ok("scene.save");const auto saved=read(project/"Content/Scenes/Main.scene");require(saved.at("entities").size()==1,"Explicit scene.save failed");
        const auto before_failed_open=d.inspect();
        write(repro/"Project/Source/Injected.source","invalid added source");
        error(d.call("repro.open",{{"directory",utf8(repro.wstring())}}),"dependency_mismatch");
        std::filesystem::remove(repro/"Project/Source/Injected.source");
        // Hash-valid input can still fail compilation. Strict open must leave the
        // current document, object identities and project in place.
        write(repro/"Project/Source/Published.source","invalid published source");
        auto broken=manifest;
        for(auto& file:broken.at("files")) if(file.at("path")=="Project/Source/Published.source") {
            file["sha256"]=sha256_file(repro/"Project/Source/Published.source");file["bytes"]=std::filesystem::file_size(repro/"Project/Source/Published.source");
        }
        write(repro/"manifest.json",broken.dump());
        error(d.call("repro.open",{{"directory",utf8(repro.wstring())}}),"operation_failed");
        current=d.inspect();require(current.at("project")==before_failed_open.at("project") && current.at("entities")==before_failed_open.at("entities") && current.at("selection")==before_failed_open.at("selection"),"Failed repro open replaced the current document");
        write(repro/"Project/Source/Published.source",published_code);write(repro/"manifest.json",manifest.dump());
        d.ok("repro.open",{{"directory",utf8(repro.wstring())}});current=d.inspect();
        require(current.at("entities").size()==1 && current.at("entities")[0].at("name")=="Control cube" && !current.at("dirty").get<bool>(),"Repro reopen failed");
        error(d.call("scene.rename",{{"object",entity},{"name","Old handle"}}),"stale_object");
        repro_lighting_migration(d,repro,root);
        d.ok("scene.delete");require(d.inspect().at("entities").empty(),"Delete command did not remove the selected entity");d.ok("scene.undo");require(d.inspect().at("entities").size()==1,"Undo did not restore the entity");
        d.client.reset();
        Process cli(std::filesystem::path(argv[1]).parent_path()/"atelier_control.exe",L"--list --directory "+quote(directory),root/"cli.json");
        require(WaitForSingleObject(cli.process,10000)==WAIT_OBJECT_0,"CLI discovery did not finish");require(read(root/"cli.json").at("instances").size()==1,"CLI did not discover the live editor");
        {
            std::ofstream request_file(root/"capture-request.json");request_file<<Json{{"op","render.capture"},{"state_token",d.token},{"args",{{"sample_frames",{1}}}}}.dump();
        }
        Process cli_capture(std::filesystem::path(argv[1]).parent_path()/"atelier_control.exe",L"--directory "+quote(directory)+L" --instance "+wide(descriptor.at("instance_id").get<std::string>())+L" --request "+quote(root/"capture-request.json"),root/"cli-capture.json");
        require(WaitForSingleObject(cli_capture.process,30000)==WAIT_OBJECT_0,"CLI capture did not finish");
        const auto cli_result=read(root/"cli-capture.json");require(cli_result.at("status")=="ok" && cli_result.at("result").at("samples").size()==1,"CLI did not await its capture job");
        editor.close();require(discover(directory).empty(),"Closed editor descriptor remained live");
        reject_nested_destination(argv[1],root);
        std::ofstream report(root/"result.json");report<<Json{{"status","passed"},{"cases",{"discovery","capabilities","idempotency","request-retention","job-retention","stale-token","create","select","property","transform","undo-redo","camera","display-policy","repro-dirty-scene","published-source-set","published-ui","repro-file-set","failed-open-preserves-document","nested-output","repro-reopen","legacy-lighting-repro","rejected-repro-preserves-camera-selection","schema2-scene6-reopen","multi-point-capture","cancel","invalid-edit-preserves-job","context-change","timeout","disconnect","clean-close"}},{"repro",utf8(repro.wstring())},{"comparison",compared.at("result").at("directory")}}.dump(2);
        std::cout<<"Editor Control E2E: PASS | "<<utf8(root.wstring())<<'\n';return 0;
    } catch(const std::exception& error) {std::cerr<<"Editor Control E2E: FAIL: "<<error.what()<<'\n';return 1;}
}
