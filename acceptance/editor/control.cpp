#include "control/control.hpp"
#include "platform/windows_path.hpp"
#include <Windows.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
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
// A 1x1 RGBA PNG.
constexpr std::array<unsigned char,70> tiny_png{0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,0x1f,0x15,0xc4,0x89,0x00,0x00,0x00,0x0d,0x49,0x44,0x41,0x54,0x78,0xda,0x63,0x64,0xf8,0xcf,0x50,0x0f,0x00,0x03,0x86,0x01,0x80,0x5a,0x34,0x7d,0x6b,0x00,0x00,0x00,0x00,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82};
constexpr std::string_view published_code="class project.ControlProbe : world.Component { float speed = 1.0f; void tick(float dt) { self.translate_owner(self.speed * dt, 0.0f, 0.0f); } }\n";
constexpr std::string_view published_ui="ui ProjectUI(in Text title, in double elapsed, in double entities) { Column hud { Text title { text: input.title; } } }\n";
void project_fixture(const std::filesystem::path& project) {
    write(project/"Content/Scenes/Main.scene",Json{{"version",9},{"next_id","1"},{"roots",Json::array()},{"entities",Json::array()},{"materials",Json::array()}}.dump());
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
    // A new editor records its surface only after serving that step's requests, so the token of a
    // first inspection can be stale by the next call. Five unchanged inspections, one per step, settle it.
    Json settle() {
        auto state=inspect();unsigned stable{};
        for(unsigned i=0;i<200 && stable<5;++i) {const auto before=token;std::this_thread::sleep_for(30ms);state=inspect();stable=before==token?stable+1:0;}
        require(stable==5,"The editor state token did not settle after start");return state;
    }
    Json op(std::string id,Json context=Json::object()) {return ok("op.call",{{"id",std::move(id)},{"context",std::move(context)}});}
    Json try_op(std::string id,Json args,Json context=Json::object()) {return call("op.call",{{"id",std::move(id)},{"args",std::move(args)},{"context",std::move(context)}});}
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
Json selection(const std::string& entity) {return {{"selection",{{"entities",Json::array({entity})}}}};}
// One operator registry serves menus, keys and Control; the context override lasts one call.
void verify_operators(Driver& d,const std::string& entity) {
    const auto listed=d.ok("op.list");
    const auto find=[&](std::string_view id)->const Json& {
        for(const auto& info:listed.at("operators")) if(info.at("id").get<std::string>()==id) return info;
        throw std::runtime_error("op.list is missing "+std::string(id));
    };
    for(const auto& info:listed.at("operators")) {
        const auto& parameters=info.at("parameters");
        require(!info.at("title").get<std::string>().empty() && parameters.at("type")=="object" && parameters.at("additionalProperties")==false,
            "Operator lacks a title or a closed parameter schema: "+info.dump());
    }
    require(find("edit.undo").at("flags")==Json::array({"history"}) && find("edit.redo").at("flags")==Json::array({"history"}) &&
        find("view.frame_all").at("flags").empty() && find("scene.select").at("flags").empty() && find("layout.reset").at("flags").empty() &&
        find("scene.transform_remove").at("flags")==Json::array({"undo"}) && find("layout.close_tab").at("flags").empty(),
        "Operator flags do not match their history contracts");
    const auto& regions=listed.at("context").at("properties").at("region").at("enum");
    require(std::ranges::find(regions,Json("viewport"))!=regions.end() && std::ranges::find(regions,Json("code"))!=regions.end(),"Context schema lacks regions");

    d.op("scene.select",selection(entity));d.op("scene.select",{{"selection",{{"entities",Json::array()}}}});
    auto before=d.inspect();require(before.at("selection").empty(),"scene.select did not clear the selection");
    auto unavailable=d.try_op("view.frame_selected",Json::object());error(unavailable,"poll_failed");
    require(unavailable.at("error").at("message").get<std::string>().find("Select an Entity")!=std::string::npos,"Poll failure did not give its reason: "+unavailable.dump());
    d.op("view.frame_selected",selection(entity));auto after=d.inspect();
    require(after.at("camera")!=before.at("camera") && after.at("selection").empty() && after.at("undo")==before.at("undo"),
        "A context selection must frame its Entity without changing the document selection or history");

    const auto undo=before.at("undo").get<std::size_t>(),redo=before.at("redo").get<std::size_t>();
    require(undo>0,"The operator check needs scene history");
    d.op("edit.undo");after=d.inspect();require(after.at("undo")==undo-1 && after.at("redo")==redo+1,"edit.undo did not move the cursor by one");
    d.op("edit.redo");after=d.inspect();require(after.at("undo")==undo && after.at("redo")==redo,"edit.redo did not restore the cursor");
    // A plain Entity has no spatial capabilities, so its Transform can come and go.
    const auto plain=d.ok("scene.create",{{"kind","entity"}}).at("object").get<std::string>();
    error(d.try_op("scene.transform_remove",Json::object(),selection(plain)),"poll_failed");
    d.op("scene.transform_add",selection(plain));
    error(d.try_op("scene.transform_add",Json::object(),selection(plain)),"poll_failed");
    d.op("scene.select",{{"selection",{{"entities",Json::array()}}}});
    d.op("scene.transform_remove",selection(plain));after=d.inspect();
    require(after.at("undo")==undo+3 && after.at("redo")==0 && after.at("selection").empty(),
        "scene.transform_add/remove did not record one entry each for their context Entity");
    d.op("edit.undo");error(d.try_op("scene.transform_add",Json::object(),selection(plain)),"poll_failed");
    d.op("edit.undo");d.op("edit.undo");
    error(d.try_op("edit.undo",Json::object(),{{"region","code"}}),"poll_failed");
    require(d.inspect().at("undo")==undo,"A Code-region undo without a source file must not touch the scene");

    error(d.try_op("view.unknown",Json::object()),"unknown_operator");
    error(d.try_op("view.frame_all",{{"zoom",2}}),"invalid_request");
    error(d.try_op("scene.select",Json::object(),{{"selection",{{"entities",Json::array({entity})},{"active","stale/1"}}}}),"stale_object");
    error(d.try_op("view.frame_all",Json::object(),{{"region","nowhere"}}),"invalid_request");
    error(d.try_op("scene.select",Json::object(),{{"selection",{{"entities",Json::array({entity,entity})}}}}),"invalid_request");
    // Arguments decode strictly into the operator's parameter record; a rejected call changes nothing.
    require(find("scene.add").at("parameters").at("properties").at("kind").at("enum")==Json::array({"entity","mesh","point","rectangle","directional"}) &&
        find("scene.add").at("flags")==Json::array({"undo"}) && find("tool.settings").at("parameters").value("seeded",false),
        "op.list does not publish the operator parameter records");
    const auto entries=d.inspect().at("undo").get<std::size_t>();
    error(d.try_op("scene.add",{{"kind","sky"}}),"invalid_request");
    require(d.inspect().at("undo")==entries,"A rejected scene.add changed the scene");
    const auto light=d.ok("op.call",{{"id","scene.add"},{"args",{{"kind","point"}}}}).at("active");
    after=d.inspect();
    require(after.at("undo")==entries+1 && after.at("selection")==Json::array({light}) &&
        std::ranges::any_of(after.at("entities"),[&](const Json& item){return item.at("handle")==light;}),
        "scene.add did not add and select one Entity in one history entry");
    d.op("edit.undo");require(d.inspect().at("undo")==entries,"Undo did not remove the added Entity");
    d.op("scene.select",selection(entity));
    require(d.inspect().at("selection")==Json::array({entity}),"scene.select did not restore the selection");
}
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
// A repro opens only in the current view schema and scene format, and its frozen view
// must agree with the scene's Environment. Every rejection keeps the current document.
void repro_lighting_agreement(Driver& d,const std::filesystem::path& repro,const std::filesystem::path& root) {
    const auto original=read(repro/"manifest.json");
    const auto original_scene=read(repro/"Project"/wide(original.at("scene").get<std::string>()));
    require(original.at("view_schema")==5 && original_scene.at("version")==9 && !original_scene.contains("world"),
        "Current repro export did not identify view schema 5 and Scene v9");
    const auto copy=root/"LightingRepro";
    std::filesystem::copy(atelier::platform::windows_file_path(repro),atelier::platform::windows_file_path(copy),
        std::filesystem::copy_options::recursive);
    const auto scene_path=copy/"Project"/wide(original.at("scene").get<std::string>());
    const auto publish=[&](Json manifest,const Json& scene) {
        write(scene_path,scene.dump(2));bool listed=false;
        for(auto& file:manifest.at("files")) if(copy/wide(file.at("path").get<std::string>())==scene_path) {
            file["sha256"]=sha256_file(scene_path);
            file["bytes"]=std::filesystem::file_size(atelier::platform::windows_file_path(scene_path));listed=true;
        }
        require(listed,"Repro scene was absent from the dependency manifest");
        write(copy/"manifest.json",manifest.dump(2));
    };
    const auto rejected=[&](const Json& manifest,const Json& scene,std::string_view code="invalid_request") {
        publish(manifest,scene);const auto before=d.inspect();const auto token=d.token;
        error(d.call("repro.open",{{"directory",utf8(copy.wstring())}}),code);
        const auto after=d.inspect();
        for(const auto* field:{"project","scene_path","entities","selection","inspected_object","camera","view","dirty","undo","redo"})
            require(after.at(field)==before.at(field),std::string("Rejected lighting repro changed ")+field);
        require(d.token==token,"Rejected lighting repro changed the accepted editor state token");
    };
    auto unmarked=original;unmarked.erase("view_schema");rejected(unmarked,original_scene);
    for(unsigned schema:{4u,6u}) {auto other=original;other["view_schema"]=schema;rejected(other,original_scene);}
    auto old_scene=original_scene;old_scene["version"]=8;rejected(original,old_scene,"operation_failed");
    for(const auto& [name,value]:Json{{"ssil",false},{"shadow_bias",.002},{"shadow_light_budget",16},{"shadow_samples",64},
        {"direct_reference",true},{"direct_clustered",false},{"direct_shadow_cache",true}}.items()) {
        auto retired=original;retired["view"][name]=value;rejected(retired,original_scene);
    }
    auto drifted=original;drifted["view"]["exposure_ev"]=original.at("view").at("exposure_ev").get<double>()+.25;
    rejected(drifted,original_scene,"dependency_mismatch");
    auto sky_drifted=original;sky_drifted["view"]["sky_enabled"]=!original.at("view").at("sky_enabled").get<bool>();
    rejected(sky_drifted,original_scene,"dependency_mismatch");
    // Selection and inspection resolve against the loaded candidate's World members before it replaces the document.
    auto beyond=original;beyond.erase("inspection");beyond["selection_indices"]=Json::array({original_scene.at("entities").size()});
    rejected(beyond,original_scene,"dependency_mismatch");
    auto absent_component=original;absent_component["selection_indices"]=Json::array({0});absent_component["inspection"]={{"entity",0},{"component",1000}};
    rejected(absent_component,original_scene,"dependency_mismatch");
    publish(original,original_scene);d.ok("repro.open",{{"directory",utf8(copy.wstring())}});
    const auto reopened=d.inspect();
    require(reopened.at("view")==original.at("view") && reopened.at("camera")==original.at("camera") && !reopened.at("dirty").get<bool>() &&
        reopened.at("selection").size()==original.at("selection_indices").size(),"Current lighting repro did not reopen with its view, camera and selection");
    d.ok("repro.open",{{"directory",utf8(repro.wstring())}});
}
// Only the scene records the sky map and its rotation; the frozen view holds view options.
// A repro of a scene with both opens, its copied map equals the loaded one, and the
// reopened repro still binds the same map content.
void repro_sky(Driver& d,const std::filesystem::path& project) {
    const std::string key="project/{6F1D2C3B-4A5E-4F60-8B7C-9D0E1F2A3B4C}";
    std::string hdr="#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 4 +X 8\n";
    for(unsigned i=0;i<32;++i) hdr+=std::string{char(64+4*i),char(128),char(32),char(129)};
    write(project/"Content/Sky/Repro.hdr",hdr);
    write(project/"Content/Sky/Repro.hdr.asset.json",Json{{"id",key.substr(8)},{"version",1}}.dump());
    const auto environment_of=[](Json& scene)->Json& {
        for(auto& entity:scene.at("entities")) for(auto& component:entity.at("components"))
            if(component.at("type")=="world.EnvironmentComponent") return component;
        throw std::runtime_error("The repro lighting scene has no Environment.");
    };
    auto scene=read(project/"Content/Scenes/Main.scene");
    environment_of(scene)["sky_map"]=key;environment_of(scene)["properties"]["sky_rotation"]=30;
    write(project/"Content/Scenes/Main.scene",scene.dump(2));
    d.ok("project.open",{{"path",utf8(project.wstring())}});
    std::string content;
    const auto exported=[&] {
        const auto id=unique_id();require(d.call("repro.export",Json::object(),id).at("status")=="pending","Sky repro export did not start");
        const auto done=d.wait(id);require(done.at("status")=="ok",done.dump());
        const std::filesystem::path bundle=wide(done.at("result").at("directory").get<std::string>());
        const auto manifest=read(bundle/"manifest.json");
        auto frozen=read(bundle/"Project"/wide(manifest.at("scene").get<std::string>()));
        const auto& sky=environment_of(frozen);
        require(sky.at("sky_map")==key && sky.at("properties").at("sky_rotation")==30,"A sky repro lost its scene's sky map key or rotation");
        const auto resource=std::ranges::find_if(manifest.at("resources"),[&](const Json& entry){return entry.at("key")==key;});
        require(resource!=manifest.at("resources").end(),"A sky repro did not check its copied sky map against the loaded one");
        if(content.empty()) content=resource->at("content_sha256").get<std::string>();
        require(resource->at("content_sha256")==content,"A reopened sky repro bound different sky map content");
        return std::pair{bundle,manifest};
    };
    const auto [bundle,manifest]=exported();
    d.ok("repro.open",{{"directory",utf8(bundle.wstring())}});
    require(d.inspect().at("view")==manifest.at("view"),"A sky repro did not open with its frozen view");
    exported();
}
// The export copies what the scene references, with the in-memory draft of a referenced material
// rather than what its saved file names: a draft that cleared a texture which has since gone
// missing still exports, the copy holds the draft and reopens strictly, and the source asset stays
// unchanged. A material the session loaded but the scene does not reference stays out.
void repro_material_draft(Driver& d,const std::filesystem::path& project,const std::filesystem::path& root) {
    d.ok("project.open",{{"path",utf8(project.wstring())}});
    write(root/"Draft.png",std::string(tiny_png.begin(),tiny_png.end()));
    const auto texture=d.ok("texture.import",{{"path",utf8((root/"Draft.png").wstring())}}).at("asset").get<std::string>();
    const auto asset=d.ok("material.create",{{"path","Content/Materials/Draft.material"}}).at("asset").get<std::string>();
    const auto unused=d.ok("material.create",{{"path","Content/Materials/Unused.material"}}).at("asset").get<std::string>();
    d.ok("material.set_texture",{{"object",d.ok("material.inspect",{{"asset",asset}}).at("handle")},{"input","base_color"},{"texture",texture}});
    d.ok("material.save",{{"asset",asset}});
    const auto entity=d.ok("scene.create",{{"kind","mesh"}}).at("object");Json component;
    for(const auto& item:d.inspect().at("entities")) if(item.at("handle")==entity) component=item.at("components")[0].at("handle");
    d.ok("material.assign",{{"component",component},{"slot","slot/0"},{"asset",asset}});d.ok("scene.save");
    const auto saved=project/"Content/Materials/Draft.material";const auto saved_text=read(saved).dump();
    for(const auto* file:{L"Content/Textures/Draft.png",L"Content/Textures/Draft.png.asset.json"}) std::filesystem::remove(atelier::platform::windows_file_path(project/file));
    d.ok("project.open",{{"path",utf8(project.wstring())}});
    const auto opened=d.ok("material.inspect",{{"asset",asset}});
    require(opened.at("missing_textures")==Json::array({texture}),"The draft fixture did not lose its texture: "+opened.dump());
    d.ok("material.inspect",{{"asset",unused}});
    d.ok("material.set_texture",{{"object",opened.at("handle")},{"input","base_color"},{"texture",""}});
    const auto id=unique_id();require(d.call("repro.export",Json::object(),id).at("status")=="pending","Draft repro export did not start");
    const auto done=d.wait(id);require(done.at("status")=="ok","A material draft without its missing texture did not export: "+done.dump());
    const std::filesystem::path bundle=wide(done.at("result").at("directory").get<std::string>());
    require(read(bundle/"Project/Content/Materials/Draft.material").at("textures").at("base_color").at("source")=="" && read(saved).dump()==saved_text,
        "A draft repro did not carry the draft, or changed the saved material");
    require(!std::filesystem::exists(atelier::platform::windows_file_path(bundle/"Project/Content/Materials/Unused.material")) &&
        std::ranges::none_of(read(bundle/"manifest.json").at("input_files"),[](const Json& file){return file.at("path").get<std::string>().starts_with("Content/Materials/Unused");}),
        "A repro copied a material the session loaded but the scene does not reference");
    d.ok("material.undo",{{"asset",asset}});
    d.ok("repro.open",{{"directory",utf8(bundle.wstring())}});
    require(d.ok("material.inspect",{{"asset",asset}}).at("missing_textures").empty(),"A draft repro did not reopen without the missing texture");
}
void repro_lighting(const std::filesystem::path& executable,const std::filesystem::path& root) {
    const auto project=root/"Project",directory=root/"Control";project_fixture(project);
    Process editor(executable,L"--project "+quote(project)+L" --control-directory "+quote(directory)+L" --no-activate",root/"editor.log");
    Driver d(discover_editor(editor,directory));d.settle();d.ok("scene.create",{{"kind","mesh"}});
    d.ok("camera.look_at",{{"eye",{8,5,-10}},{"target",{1,2,3}}});
    const auto removed=d.call("view.set",{{"patch",{{"direct_reference",true}}}});error(removed,"invalid_request");
    require(removed.at("error").at("message").get<std::string>().find("was removed")!=std::string::npos,
        "Control did not report the direct reference diagnostic as removed: "+removed.dump());
    d.ok("view.set",{{"patch",{{"direct_candidates",16},{"exposure_ev",1.25}}}});
    d.ok("scene.save");const auto id=unique_id();
    require(d.call("repro.export",Json::object(),id).at("status")=="pending","Lighting repro export did not start");
    const auto exported=d.wait(id);require(exported.at("status")=="ok",exported.dump());
    repro_lighting_agreement(d,wide(exported.at("result").at("directory").get<std::string>()),root);
    repro_sky(d,project);
    repro_material_draft(d,project,root);
    d.client.reset();editor.close();
    write(root/"result.json",Json{{"status","passed"},{"cases",{"current-schema-and-format-only","retired-view-keys-rejected",
        "rejected-repro-preserves-state","view-and-environment-agreement","selection-resolved-before-replacement","current-repro-reopen","sky-map-and-rotation","material-draft-dependencies"}}}.dump(2));
    std::cout<<"Editor Repro Lighting E2E: PASS | "<<utf8(root.wstring())<<'\n';
}
void reject_nested_destination(const std::filesystem::path& executable,const std::filesystem::path& root) {
    const auto project=root/"NestedProject";project_fixture(project);const auto directory=project/"Content/Control";
    Process editor(executable,L"--project "+quote(project)+L" --control-directory "+quote(directory)+L" --no-activate",root/"nested-editor.log");
    Driver d(discover_editor(editor,directory));d.settle();const auto id=unique_id();
    const auto started=d.call("repro.export",Json::object(),id);require(started.at("status")=="pending","Nested output fixture did not start: "+started.dump());
    error(d.wait(id),"invalid_destination");
    require(std::filesystem::is_empty(atelier::platform::windows_file_path(directory/"artifacts"/wide(d.descriptor.at("instance_id").get<std::string>()))),"Rejected nested export left a recursive output");
    d.client.reset();editor.close();
}
// An Official Examples scene keeps its assets under Examples/, which holds far more files
// than a repro may copy. The repro carries only what the Sponza scene references: its two glTF
// meshes with their buffers and images, each slot's default material, and the sun-removed sky
// map; not the unused original HDRI, the unrelated Physic example or the READMEs.
void repro_examples(const std::filesystem::path& executable,const std::filesystem::path& root) {
    const auto shipped=executable.parent_path()/"examples",project=root/"Examples",directory=root/"Control";
    for(const auto* folder:{L"Config",L"Content",L"Source",L"Examples/Sponza",L"Examples/Physic"}) {
        std::filesystem::create_directories(atelier::platform::windows_file_path((project/folder).parent_path()));
        std::filesystem::copy(atelier::platform::windows_file_path(shipped/folder),atelier::platform::windows_file_path(project/folder),std::filesystem::copy_options::recursive);
    }
    const std::string sponza="Examples/Sponza/",building=sponza+"NewSponza/NewSponza_Main_glTF_003.gltf",curtains=sponza+"SponzaExtend/NewSponza_Curtains_glTF.gltf";
    // A mesh may spell an image in another case than its file; the repro copies the catalogue's file once.
    const std::string image="textures/arch_stone_wall_01_BaseColor.png",alias="Textures/Arch_Stone_Wall_01_BaseColor.png";
    {
        std::ifstream stream(atelier::platform::windows_file_path(project/building),std::ios::binary);
        std::string text(std::istreambuf_iterator<char>(stream),{});const auto at=text.find("\""+image+"\"");
        require(at!=std::string::npos,"The New Sponza glTF fixture changed");text.replace(at+1,image.size(),alias);write(project/building,text);
    }
    // Every slot default the two mesh sidecars name, by the material file that holds its identity.
    std::map<std::string,std::string> material_files;std::set<std::string> defaults;
    for(const auto& entry:std::filesystem::recursive_directory_iterator(atelier::platform::windows_file_path(project/sponza)))
        if(auto path=utf8(entry.path().lexically_relative(atelier::platform::windows_file_path(project)).generic_wstring());path.ends_with(".material.asset.json")) {
            const auto key="project/"+read(entry.path()).at("id").get<std::string>();path.resize(path.size()-11);material_files[key]=std::move(path);
        }
    for(const auto& mesh:{building,curtains}) for(const auto& [slot,key]:read(project/(mesh+".asset.json")).at("materials").items()) defaults.insert(material_files.at(key.get<std::string>()));
    require(defaults.size()==31,"The New Sponza meshes no longer name 31 slot default materials");
    const std::string sky_file=sponza+"Sky/kloppenheim_05_4k_nosun.hdr";
    Process editor(executable,L"--project "+quote(project)+L" --control-directory "+quote(directory)+L" --no-activate",root/"editor.log");
    Driver d(discover_editor(editor,directory));d.settle();
    Json resources;std::set<std::string> inputs;
    const auto exported=[&] {
        const auto id=unique_id();require(d.call("repro.export",Json::object(),id).at("status")=="pending","Examples repro export did not start");
        const auto done=d.wait(id);require(done.at("status")=="ok",done.dump());
        const std::filesystem::path bundle=wide(done.at("result").at("directory").get<std::string>());
        const auto manifest=read(bundle/"manifest.json");
        std::set<std::string> copied;std::map<std::string,std::string> hashes;
        for(const auto& file:manifest.at("input_files")) {copied.insert(file.at("path").get<std::string>());hashes[file.at("path").get<std::string>()]=file.at("sha256").get<std::string>();}
        // Each asset arrives with the identity file the source project gave it.
        const auto asset=[&](const std::string& path) {
            return copied.contains(path) && hashes.contains(path+".asset.json") && hashes.at(path+".asset.json")==sha256_file(project/(path+".asset.json"));
        };
        for(const auto& path:copied) if(path.find("/textures/")!=std::string::npos && !path.ends_with(".asset.json"))
            require(asset(path),"An Examples repro copied a texture without its identity file: "+path);
        require(asset(building) && asset(curtains) && copied.contains(sponza+"NewSponza/NewSponza_Main_glTF_003.bin") &&
            copied.contains(sponza+"SponzaExtend/NewSponza_Curtains_glTF.bin") && asset(sponza+"NewSponza/"+image) && asset(sky_file),
            "An Examples repro left out the scene's meshes, their dependencies or its sky map");
        for(const auto& material:defaults) require(asset(material),"An Examples repro left out the slot default material "+material);
        require(std::ranges::none_of(copied,[&](const std::string& path){return path.starts_with("Examples/Physic/") || path.starts_with(sponza+"Scenes/") ||
            path.starts_with(sponza+"Sky/kloppenheim_05_4k.hdr") || path.ends_with("README.md");}),
            "An Examples repro copied files the scene does not use");
        if(inputs.empty()) {inputs=copied;resources=manifest.at("resources");}
        require(copied==inputs && manifest.at("resources")==resources,"A reopened Examples repro carried different assets");
        return std::pair{bundle,manifest};
    };
    const auto [bundle,manifest]=exported();
    require(manifest.at("resources").size()==3,"An Examples repro did not check Sponza's two meshes and its sky map: "+manifest.at("resources").dump());
    d.ok("repro.open",{{"directory",utf8(bundle.wstring())}});
    require(d.inspect().at("view")==manifest.at("view"),"An Examples repro did not open with its frozen view");
    exported();
    d.client.reset();editor.close();
}
// A glTF import publishes one material per slot as the mesh's default, which the scene does not
// name. The repro carries the default material, its texture and their identity files, reopens
// strictly and exports the same files again.
void repro_gltf_defaults(const std::filesystem::path& executable,const std::filesystem::path& root) {
    const auto project=root/"Project",directory=root/"Control",source=root/"source";project_fixture(project);
    write(source/"textures/color.png",std::string(tiny_png.begin(),tiny_png.end()));
    std::string bin;
    const auto put=[&](std::initializer_list<float> values) {for(const float v:values) bin.append(reinterpret_cast<const char*>(&v),4);};
    put({0,0,0,1,0,0,0,1,0});put({0,0,-1,0,0,-1,0,0,-1});put({0,1,1,1,0,0});
    for(const std::uint16_t i:{0,1,2,0}) bin.append(reinterpret_cast<const char*>(&i),2);
    write(source/"scene.bin",bin);
    const auto view=[](int offset,int length){return Json{{"buffer",0},{"byteOffset",offset},{"byteLength",length}};};
    const auto accessor=[](int index,const char* type,int component){return Json{{"bufferView",index},{"type",type},{"componentType",component},{"count",3}};};
    write(source/"scene.gltf",Json{{"asset",{{"version","2.0"}}},{"scenes",{{{"nodes",{0}}}}},{"nodes",{{{"mesh",0}}}},
        {"meshes",{{{"primitives",{{{"attributes",{{"POSITION",0},{"NORMAL",1},{"TEXCOORD_0",2}}},{"indices",3},{"material",0}}}}}}},
        {"materials",{{{"name","paint"},{"pbrMetallicRoughness",{{"baseColorTexture",{{"index",0}}}}}}}},
        {"textures",{{{"source",0}}}},{"images",{{{"uri","textures/color.png"}}}},{"buffers",{{{"uri","scene.bin"},{"byteLength",bin.size()}}}},
        {"bufferViews",{view(0,36),view(36,36),view(72,24),view(96,6)}},
        {"accessors",{accessor(0,"VEC3",5126),accessor(1,"VEC3",5126),accessor(2,"VEC2",5126),accessor(3,"SCALAR",5123)}}}.dump());
    Process editor(executable,L"--project "+quote(project)+L" --control-directory "+quote(directory)+L" --no-activate",root/"editor.log");
    Driver d(discover_editor(editor,directory));d.settle();
    const auto mesh=d.ok("mesh.import",{{"path",utf8((source/"scene.gltf").wstring())}}).at("asset").get<std::string>();
    d.ok("scene.create",{{"kind","mesh"},{"mesh",mesh}});
    const std::string folder="Content/Meshes/scene/",material=folder+"Materials/paint.material";
    const auto defaults=read(project/(folder+"scene.gltf.asset.json")).at("materials");
    require(defaults.size()==1 && defaults.begin()->get<std::string>()=="project/"+read(project/(material+".asset.json")).at("id").get<std::string>(),
        "The glTF import did not name its generated material as the slot default: "+defaults.dump());
    std::set<std::string> inputs;
    const auto exported=[&] {
        const auto id=unique_id();require(d.call("repro.export",Json::object(),id).at("status")=="pending","glTF repro export did not start");
        const auto done=d.wait(id);require(done.at("status")=="ok",done.dump());
        const std::filesystem::path bundle=wide(done.at("result").at("directory").get<std::string>());
        std::set<std::string> copied;
        for(const auto& file:read(bundle/"manifest.json").at("input_files")) copied.insert(file.at("path").get<std::string>());
        for(const auto& file:{folder+"scene.gltf",folder+"scene.bin",folder+"textures/color.png",material})
            require(copied.contains(file) && (file.ends_with(".bin") || copied.contains(file+".asset.json")),"A glTF repro left out "+file+" or its identity file");
        if(inputs.empty()) inputs=copied;
        require(copied==inputs,"A reopened glTF repro carried different files");
        return bundle;
    };
    const auto bundle=exported();
    // The export froze the unsaved mesh; undoing it leaves the source scene clean so the repro may replace it.
    d.op("edit.undo");require(!d.inspect().at("dirty").get<bool>(),"Undoing the placed glTF mesh did not restore the saved scene");
    d.ok("repro.open",{{"directory",utf8(bundle.wstring())}});
    exported();
    // A component keeps its material bindings while its mesh is missing. Only the scene names the
    // bound material then, and the repro still carries it.
    d.ok("project.open",{{"path",utf8(project.wstring())}});
    const auto bound=d.ok("material.create",{{"path","Content/Materials/Bound.material"}}).at("asset").get<std::string>();
    const auto entity=d.ok("scene.create",{{"kind","mesh"},{"mesh",mesh}}).at("object");Json component;
    for(const auto& item:d.inspect().at("entities")) if(item.at("handle")==entity) component=item.at("components")[0].at("handle");
    d.ok("material.assign",{{"component",component},{"slot","paint"},{"asset",bound}});d.ok("scene.save");
    std::filesystem::remove(atelier::platform::windows_file_path(project/(folder+"scene.gltf")));
    d.ok("project.open",{{"path",utf8(project.wstring())}});
    const auto id=unique_id();require(d.call("repro.export",Json::object(),id).at("status")=="pending","Missing-mesh repro export did not start");
    const auto done=d.wait(id);require(done.at("status")=="ok",done.dump());
    std::set<std::string> copied;
    for(const auto& file:read(std::filesystem::path(wide(done.at("result").at("directory").get<std::string>()))/"manifest.json").at("input_files")) copied.insert(file.at("path").get<std::string>());
    require(copied.contains("Content/Materials/Bound.material") && copied.contains("Content/Materials/Bound.material.asset.json") && !copied.contains(material),
        "A repro of a component whose mesh is missing lost its bound material, or copied the mesh's default");
    d.client.reset();editor.close();
}
void repro_paths(const std::filesystem::path& executable,const std::filesystem::path& root,bool deep_control) {
    const auto project=root/"Project";
    const auto directory=deep_control?root/"Control"/std::wstring(100,L'x'):root/"Control";
    project_fixture(project);
    // The frozen copy is always named Control-<id>.scene. With a short control directory a
    // long-name Source file, copied whole, is what crosses MAX_PATH ("Project/Source/" plus
    // 230 characters); with the deep control directory the frozen copy itself does. The
    // long-name startup scene is read, not copied.
    const auto scene=std::filesystem::path("Content/Scenes")/("Control-"+unique_id()+(deep_control?"":"-"+std::string(200,'n'))+".scene");
    const auto long_source=std::filesystem::path("Source")/(std::string(226,'l')+".txt");
    write(project/long_source,"copied whole");
    std::filesystem::rename(atelier::platform::windows_file_path(project/"Content/Scenes/Main.scene"),atelier::platform::windows_file_path(project/scene));
    const auto bytes=[](const std::filesystem::path& path) {
        std::ifstream stream(atelier::platform::windows_file_path(path),std::ios::binary);require(bool(stream),"Cannot read E2E artifact: "+utf8(path.wstring()));
        return std::string(std::istreambuf_iterator<char>(stream),{});
    };
    const auto source_scene=bytes(project/scene);
    auto config=read(project/"Config/Project.json");config["startup_scene"]=utf8(scene.generic_wstring());
    write(project/"Config/Project.json",config.dump());
    Process editor(executable,L"--project "+quote(project)+L" --control-directory "+quote(directory)+L" --no-activate",root/"editor.log");
    Driver d(discover_editor(editor,directory));d.settle();
    d.ok("scene.create",{{"kind","mesh"}});
    const auto unpublished=std::filesystem::path("Source")/(std::wstring(72,L'u')+L".source");
    write(project/unpublished,"invalid saved source");std::this_thread::sleep_for(750ms);d.inspect();
    std::filesystem::path repro;
    for(unsigned pass=0;pass<2;++pass) {
        const auto id=unique_id();const auto started=d.call("repro.export",Json::object(),id);require(started.at("status")=="pending",started.dump());
        const auto exported=d.wait(id);require(exported.at("status")=="ok",exported.dump());
        repro=wide(exported.at("result").at("directory").get<std::string>());
        const auto manifest=read(repro/"manifest.json");const auto frozen=repro/"Project"/wide(manifest.at("scene").get<std::string>());
        if(deep_control) require(frozen.wstring().size()>260,"Deep-control export did not place the frozen scene past MAX_PATH");
        else {
            const auto copied=repro/"Project"/long_source;
            require(copied.wstring().size()>260 && bytes(copied)=="copied whole","Short-control export did not copy the long-name Source file unchanged");
            if(pass==0) require(bytes(project/scene)==source_scene,"Export changed the long-name source scene");
        }
        require(read(frozen).at("entities").size()==1,"Deep-path export did not preserve the scene");
        if(pass==0) require(std::filesystem::exists(atelier::platform::windows_file_path(repro/"unpublished"/unpublished)),"Deep-path source archive is missing");
        if(pass==0) d.op("edit.undo");
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
    Json patch{{"shading","scene_lit"},{"ddgi",true},{"gtao",false},{"sssr",false},{"bloom",false},
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
    d.op("edit.undo");d.ok("scene.save",{{"path","Content/Scenes/Editing.scene"}});d.ok("repro.open",{{"directory",utf8(repro.wstring())}});
    require(d.inspect().at("view")==repro_view,"Repro did not restore all four volume settings");
    for(const char* field:{"min_x","min_y","min_z","max_x","max_y","max_z","count_x","count_y","count_z"}) {
        const auto canonical=std::string("probe_volume_0_")+field;
        manifest["view"][std::string("probe_")+field]=manifest.at("view").at(canonical);manifest["view"].erase(canonical);
    }
    write(repro/"manifest.json",manifest.dump(2));d.ok("repro.open",{{"directory",utf8(repro.wstring())}});
    require(d.inspect().at("view")==repro_view,"Legacy slot-zero names changed canonical repro settings");
    d.client.reset();editor.close();
    require(read(project/"Content/Scenes/Main.scene").at("entities").empty(),"Diagnostic session changed the saved scene");
    write(root/"result.json",Json{{"status","passed"},{"cases",{"shared-schema","summary-inspection","idempotent-patch","atomic-rejection","real-render-compare",
        "coverage-presets","rolling-focus-capture","focus-precision-atomicity","four-volume-repro","legacy-aliases","history-and-scene-isolation","clean-close"}}}.dump(2));
    std::cout<<"Editor DDGI Control E2E: PASS | "<<utf8(root.wstring())<<'\n';
}
#include "client_size.inl"
}
int wmain(int argc,wchar_t** argv) {
    try {
        require(argc==3 || (argc==4 && (std::wstring_view(argv[3])==L"--repro-paths" || std::wstring_view(argv[3])==L"--ddgi-debug" || std::wstring_view(argv[3])==L"--repro-lighting" || std::wstring_view(argv[3])==L"--descriptions" || std::wstring_view(argv[3])==L"--client-size")),"Usage: atelier_editor_control_acceptance editor.exe output-directory [--repro-paths|--ddgi-debug|--repro-lighting|--descriptions|--client-size]");
        const bool descriptions=argc==4 && std::wstring_view(argv[3])==L"--descriptions";
        // Launched editors and their Play processes inherit the editor's harness cache, not the user's.
        if(!GetEnvironmentVariableW(L"ATELIER_SHADER_CACHE",nullptr,0))
            SetEnvironmentVariableW(L"ATELIER_SHADER_CACHE",(std::filesystem::absolute(argv[1]).parent_path()/L"shader-cache").c_str());
        const auto root=std::filesystem::absolute(argv[2])/unique_id();std::filesystem::create_directories(root);
        if(argc==4 && std::wstring_view(argv[3])==L"--client-size") {client_size(argv[1],root);return 0;}
        if(argc==4 && std::wstring_view(argv[3])==L"--ddgi-debug") {ddgi_debug(argv[1],root);return 0;}
        if(argc==4 && std::wstring_view(argv[3])==L"--repro-lighting") {repro_lighting(argv[1],root);return 0;}
        if(argc==4 && !descriptions) {
            repro_paths(argv[1],root/"exports",false);repro_paths(argv[1],root/"deep-control",true);
            reject_nested_destination(argv[1],root);
            repro_examples(argv[1],root/"examples");repro_gltf_defaults(argv[1],root/"gltf");
            write(root/"result.json",Json{{"status","passed"},{"cases",{"nested-exports","deep-control","nested-destination-rejected","examples-scene-assets","gltf-default-materials","missing-mesh-bound-material"}}}.dump(2));
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
            d.op("edit.undo");const auto restored=d.inspect();
            error(d.call("editor.inspect",{{"detail","objects"},{"cursor",directory_cursor}}),"stale_cursor");
            error(d.call("editor.inspect",{{"detail","object"},{"object",current.at("entities")[0].at("handle")}}),"stale_object");
            require(restored.at("entities").empty() && !restored.at("dirty").get<bool>() &&
                restored.at("undo")==initial.at("undo") && restored.at("view")==initial.at("view"),
                "Description inspection or cleanup changed saved document state.");
            // Placeholders report the saved type they stand in for and the data they keep.
            write(project/"Content/Scenes/Placeholder.scene",Json{{"version",9},{"next_id","3"},{"roots",{"1"}},{"materials",Json::array()},
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
        d.op("edit.undo");current=d.inspect();require(current.at("entities")[0].at("transform").at("position")[0]==1,"Undo did not restore the transform");d.op("edit.redo");
        verify_operators(d,entity);
        const auto added=d.ok("scene.add_component",{{"type","world.Component"}}).at("object");
        current=d.inspect();require(current.at("entities")[0].at("components").size()==2,"Component addition failed");
        d.ok("scene.remove_component",{{"object",added}});current=d.inspect();require(current.at("entities")[0].at("components").size()==1 &&
            current.at("entities")[0].at("components")[0].at("handle")==component,"Component removal failed or replaced the existing mesh identity");
        const auto camera=current.at("camera");d.ok("camera.look_at",{{"eye",{8,5,-10}},{"target",{0,0,0}}});d.ok("camera.set",{{"state",camera}});
        auto invalid_camera=camera;invalid_camera["distance"]=0.0000000001;error(d.call("camera.set",{{"state",invalid_camera}}),"invalid_request");
        // The sky is a World setting: scene.create no longer makes one, and the first sky edit creates the Environment.
        const auto retired_sky=d.call("scene.create",{{"kind","sky"}});
        require(retired_sky.value("status","")=="error" && retired_sky.at("error").at("code")=="invalid_request" &&
            retired_sky.at("error").at("message").get<std::string>().find("sky_enabled")!=std::string::npos,
            "scene.create accepted the retired sky kind or did not name the sky settings: "+retired_sky.dump());
        for(const auto* name:{"sky_enabled","sky_intensity","sky_color_x","sky_color_y","sky_color_z"})
            require(caps.at("display_options").at(name).at("scope")=="scene","A sky option is absent from the shared Control schema or not a scene option");
        d.ok("view.set",{{"patch",{{"sky_enabled",true},{"sky_intensity",2},{"sky_color_x",.25},{"sky_color_y",.5},{"sky_color_z",.75}}}});
        current=d.inspect();
        require(current.at("view").at("sky_enabled")==true && current.at("view").at("sky_intensity")==2 && current.at("view").at("sky_color_x")==.25 &&
            current.at("view").at("sky_color_y")==.5 && current.at("view").at("sky_color_z")==.75 && current.at("entities").size()==2 &&
            current.at("entities")[1].at("components")[0].at("type")=="world.EnvironmentComponent","The sky settings did not create the Environment");
        d.op("edit.undo");current=d.inspect();
        require(current.at("view").at("sky_enabled")==false && current.at("entities").size()==1,"Undoing the first sky edit did not remove the Environment");
        for(const auto* removed:{"ssil","shadow_bias"}) error(d.call("view.set",{{"patch",{{removed,true}}}}),"invalid_request");
        d.ok("view.set",{{"patch",{{"gtao",false}}}});current=d.inspect();
        require(current.at("view").at("gtao")==false && current.at("entities").size()==2,"A World setting did not create the Environment");
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
        // The mesh, and the Environment its World settings created.
        require(frozen.at("entities").size()==2,"Export did not contain the dirty memory scene");
        require(read(project/"Content/Scenes/Main.scene")==original_scene,"Repro export overwrote the original scene");
        current=d.inspect();require(current.at("dirty")==true && current.at("undo")==dirty_undo && current.at("scene_path")==dirty_path,"Repro export changed authoring state");
        d.ok("camera.look_at",{{"eye",{9,7,-8}},{"target",{4,2,3}}});
        const auto base_view=current.at("view");const auto compare_id=unique_id();
        const auto prepare_id=unique_id();auto preparing=d.call("render.prepare",Json::object(),prepare_id);require(preparing.at("status")=="pending",preparing.dump());
        std::cout<<"Editor Control: preparing cold Scene Lit pipelines\n"<<std::flush;
        auto prepared=d.wait(prepare_id,605s);require(prepared.at("status")=="ok",prepared.dump());
        require(prepared.at("result").at("phase")=="ready","Preparation did not report an accepted ready frame");
        d.inspect();
        auto pending=d.call("render.compare",{{"cases",Json::array({{{"name","baseline"},{"patch",Json::object()}},{{"name","scene_lit"},{"patch",{{"shading","scene_lit"},{"shadows",false},{"ddgi",false},{"gtao",false}}}}})},{"sample_frames",{1,3}}},compare_id);
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
        error(d.call("scene.transform",{{"object",entity},{"position",{4,2,3}},{"rotation",{0,0,0,0}},{"scale",{1,1,1}}}),"operation_failed");
        error(d.call("scene.set_property",{{"object",entity},{"property","rotation.w"},{"value",0}}),"operation_failed");
        error(d.call("view.set",{{"patch",{{"ssil",true}}}}),"invalid_request");
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
        d.inspect();d.ok("scene.save");const auto saved=read(project/"Content/Scenes/Main.scene");require(saved.at("entities").size()==2,"Explicit scene.save failed");
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
        require(current.at("entities").size()==2 && current.at("entities")[0].at("name")=="Control cube" && !current.at("dirty").get<bool>(),"Repro reopen failed");
        error(d.call("scene.rename",{{"object",entity},{"name","Old handle"}}),"stale_object");
        repro_lighting_agreement(d,repro,root);
        d.ok("scene.delete");
        const auto remaining=d.inspect().at("entities");
        require(remaining.size()==1 && remaining[0].at("components")[0].at("type")=="world.EnvironmentComponent","Delete command did not remove the selected entity");
        d.op("edit.undo");require(d.inspect().at("entities").size()==2,"Undo did not restore the entity");
        d.client.reset();
        Process cli(std::filesystem::path(argv[1]).parent_path()/"atelier_control.exe",L"--list --directory "+quote(directory),root/"cli.json");
        require(WaitForSingleObject(cli.process,10000)==WAIT_OBJECT_0,"CLI discovery did not finish");require(read(root/"cli.json").at("instances").size()==1,"CLI did not discover the live editor");
        {
            std::ofstream request_file(root/"capture-request.json");request_file<<Json{{"op","render.capture"},{"state_token",d.token},{"args",{{"sample_frames",{1}}}}}.dump();
        }
        Process cli_capture(std::filesystem::path(argv[1]).parent_path()/"atelier_control.exe",L"--directory "+quote(directory)+L" --instance "+wide(descriptor.at("instance_id").get<std::string>())+L" --request "+quote(root/"capture-request.json"),root/"cli-capture.json");
        require(WaitForSingleObject(cli_capture.process,30000)==WAIT_OBJECT_0,"CLI capture did not finish");
        const auto cli_result=read(root/"cli-capture.json");require(cli_result.at("status")=="ok" && cli_result.at("result").at("samples").size()==1,"CLI did not await its capture job");
        // Every editor step of the run, Control captures included, kept the timing hierarchy.
        d.client=std::make_unique<Client>(descriptor);
        const auto cpu=d.inspect().at("performance").at("cpu");
        require(cpu.at("structure_errors")==0 && cpu.at("first_structure_error").is_null(),"An editor step recorded a frame timing phase under the wrong parent: "+cpu.dump());
        editor.close();require(discover(directory).empty(),"Closed editor descriptor remained live");
        reject_nested_destination(argv[1],root);
        std::ofstream report(root/"result.json");report<<Json{{"status","passed"},{"cases",{"discovery","capabilities","idempotency","request-retention","job-retention","stale-token","create","select","property","transform","undo-redo","camera","display-policy","repro-dirty-scene","published-source-set","published-ui","repro-file-set","failed-open-preserves-document","nested-output","repro-reopen","current-lighting-repro","rejected-repro-preserves-camera-selection","sky-world-setting","multi-point-capture","cancel","invalid-edit-preserves-job","context-change","timeout","disconnect","clean-close"}},{"repro",utf8(repro.wstring())},{"comparison",compared.at("result").at("directory")}}.dump(2);
        std::cout<<"Editor Control E2E: PASS | "<<utf8(root.wstring())<<'\n';return 0;
    } catch(const std::exception& error) {std::cerr<<"Editor Control E2E: FAIL: "<<error.what()<<'\n';return 1;}
}
