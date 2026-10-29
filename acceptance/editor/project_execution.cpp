#include "assets/file.hpp"
#include "assets/json_format.hpp"
#include "editor/project_code.hpp"
#include "editor/project_ui.hpp"
#include "editor/workspace.hpp"
#include "application/play_process.hpp"
#include "application/project_runner.hpp"
#include "application/timing.hpp"
#include "application/presentation.hpp"
#include "ui/session.hpp"
#include <Windows.h>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <deque>

using namespace atelier;
namespace {
template<class T> T take(Result<T> result) {
    if(!result) throw std::runtime_error(result.error().message);
    if constexpr(!std::is_void_v<T>) return std::move(*result);
}
template<class T> T display(ui::Result<T> result) {
    if(!result) throw std::runtime_error("UI error "+std::to_string(static_cast<int>(result.error().code))+" at control "+std::to_string(result.error().key));
    if constexpr(!std::is_void_v<T>) return std::move(*result);
}
void require(bool value,const char* message){if(!value) throw std::runtime_error(message);}
void complete(Result<world::Commit> result){auto value=take(std::move(result));if(value.notification_error) throw std::runtime_error(value.notification_error->message);}
float position(editor::Session& document,ObjectId entity) {
    require(take(world::has_transform(document.world().runtime(),entity)),"Entity lost its Transform.");
    return take(world::read_transform(document.world().runtime(),entity)).position.x;
}
void check_position(float value,float expected){require(std::abs(value-expected)<.0001f,"Source behavior produced the wrong transform.");}
ObjectId entity(editor::Session& document) {return take(document.world().members()).front().id();}
ObjectId behavior(editor::Session& document) {
    auto& runtime=document.world().runtime();const auto type=take(runtime.find_type("project.Move"));
    for(const auto& component:take(world::components(runtime,entity(document)))) if(take(runtime.type_of(component.id()))==type) return component.id();
    throw std::runtime_error("Source component disappeared.");
}
Scalar field(editor::Session& document,std::string_view name) {
    const auto object=behavior(document);auto& runtime=document.world().runtime();const auto type=take(runtime.type_of(object));
    const std::array path{take(runtime.find_field(type,name))};return take(document.read_property(object,path));
}
void wait(application::PlayProcess& process,std::chrono::seconds timeout) {
    const auto until=std::chrono::steady_clock::now()+timeout;
    while(process.running() && std::chrono::steady_clock::now()<until) {process.poll();Sleep(10);}
    if(process.running()){process.close();throw std::runtime_error("Project runner did not finish.");}
}
#include "project_ui_reload.inl"
#include "play_link.inl"
void run(const std::filesystem::path& root) {
    auto project=take(editor::Project::create(root));
    const auto helper=root/"Source/Math.source",component=root/"Source/Move.source";
    // Project Source calls the engine's procedural library; procedural.unit(2^31) is exactly 0.5.
    const std::string helpers="float travel(float speed,float dt) { return speed * dt * (2.0f * procedural.unit(2147483648)); }\n";
    const std::string declaration="class project.Move : world.Component {\n    float speed = 2.0f;\n    bool moving = true;\n    uint ticks = 0;\n    void tick(float dt) {\n        if (self.moving) { self.translate_owner(travel(self.speed, dt), 0.0f, 0.0f); }\n        self.ticks = self.ticks + 1;\n    }\n}\n";
    {
        auto empty=editor::load_project_scene(project,take(assets::read_file(root/"Content/Scenes/Main.scene")));
        if(!empty.session) throw std::runtime_error("Empty project: "+empty.error->message);
        complete(empty.session->add_entity());
        take(editor::write_file(helper,helpers));take(editor::write_file(component,declaration));
        take(empty.code->poll(project,true));
        require(empty.session->world().runtime().find_type("project.Move").has_value(),"Adding the first Source class did not publish it into the open scene.");
    }
    auto loaded=editor::load_project_scene(project,take(assets::read_file(root/"Content/Scenes/Main.scene")));
    if(!loaded.session) throw std::runtime_error("Initial project: "+loaded.error->message);
    auto& document=*loaded.session;
    complete(document.create_static_mesh(assets::mesh_assets().front()));
    complete(document.add_component(take(document.world().runtime().find_type("project.Move"))));
    require(std::get<float>(field(document,"speed"))==2 && std::get<bool>(field(document,"moving")),"Runtime did not initialize source field defaults.");
    const auto original=take(document.serialize());take(editor::write_file(root/"Content/Scenes/Main.scene",original));document.mark_saved();

    editor::Workspace workspace;workspace.project_open=true;workspace.project_build=loaded.code->report();
    ui::Session ui(application::editor_fonts());
    auto style=ui.style();style.font.family=L"Sarasa UI SC";style.font.size=12.5f;style.caption_scale=1;style.title_scale=7.f/6;
    display(ui.set_style(style));display(ui.set_style_sheet(application::editor_styles()));
    display(ui.set_surface({{1280,800},1,1}));
    auto controls=workspace.describe(document,{1280,800},nullptr,&project);
    auto described=ui.set_controls(controls);
    if(!described) {
        for(const auto& control:controls) if(control.key==described.error().key || control.parent==described.error().key)
            std::printf("Control %llu parent %llu kind %u text %s\n",static_cast<unsigned long long>(control.key),static_cast<unsigned long long>(control.parent),static_cast<unsigned>(control.kind),assets::utf8(control.text).c_str());
    }
    display(std::move(described));workspace.commit_ui_controls();auto prepared=display(ui.prepare());require(ui.accept(prepared),"Could not accept project property UI.");
    require(workspace.ui_error().empty(),"Editor UI failed while projecting source properties.");
    require(std::ranges::any_of(controls,[](const auto& item){return item.kind==ui::ControlKind::number && item.value==2;}),"Source number field did not reach the shared editor.");
    require(std::ranges::any_of(controls,[](const auto& item){return item.kind==ui::ControlKind::checkbox && item.text==L"Moving" && item.selected;}),"Source boolean field did not reach its checkbox.");

    auto playing=editor::load_project_scene(project,original);require(playing.session!=nullptr,"Play snapshot failed to load.");
    auto& play=*playing.session;const auto moving_entity=entity(play);
    complete(play.start());take(play.update(.5f));check_position(position(play,moving_entity),1);check_position(position(document,entity(document)),0);
    require(!document.dirty(),"Playing modified the authoring document.");
    take(editor::write_file(helper,"float travel(float speed,float dt) { return speed * dt * 2.0f; }\n"));
    take(playing.code->poll(project,true));take(play.update(.5f));check_position(position(play,moving_entity),3);
    // Lifecycle methods added during Play bind between frames: the begun component
    // gets no Begin, and each phase method runs in its phase from the next frame.
    auto phased=declaration;phased.insert(phased.find("    void tick"),"    void begin() { self.ticks = 1000; }\n");
    phased.insert(phased.rfind('}'),"    void late_tick(float dt) { self.ticks = self.ticks * 10 + 3; }\n"
        "    void post_physics_tick(float dt) { self.ticks = self.ticks * 10 + 2; }\n");
    const auto ticks=[&] {
        auto& runtime=play.world().runtime();const auto move=behavior(play);
        return std::get<std::uint32_t>(take(runtime.read(move,take(runtime.find_field(take(runtime.type_of(move)),"ticks")))));
    };
    const auto before=ticks();take(editor::write_file(component,phased));take(playing.code->poll(project,true));
    require(ticks()==before,"A lifecycle method added during Play replayed Begin.");
    take(play.update(.5f));check_position(position(play,moving_entity),5);
    require(ticks()==((before+1)*10+2)*10+3,"Source tick, post_physics_tick and late_tick did not run once each in phase order.");
    take(editor::write_file(helper,"float travel(float speed,float dt) { return missing; }\n"));
    require(!playing.code->poll(project,true),"Invalid project source was published.");
    require(playing.code->report()->error && playing.code->report()->error->source && playing.code->report()->error->source->path=="Source/Math.source","Compiler lost the failing file identity.");
    take(play.update(.5f));check_position(position(play,moving_entity),7);complete(play.stop());complete(play.stop());
    take(workspace.source_editor.open(component));workspace.project_build=playing.code->report();workspace.dock.select(editor::source_control::tab);
    controls=workspace.describe(document,{1280,800},ui.presented().get(),&project);display(ui.set_controls(controls));workspace.commit_ui_controls();
    require(workspace.source_editor.locate_problem(),"Cross-file compiler diagnostic could not open its source.");
    require(workspace.source_editor.active()->path==std::filesystem::canonical(helper),"Diagnostic opened the wrong file.");

    take(editor::write_file(helper,helpers));take(playing.code->poll(project,true));
    const auto fail_second_tick=[](std::string source) {
        source.insert(source.find("        if (self.moving)"),"        if (self.ticks == 1) { self.ticks = 10 / (self.ticks - 1); }\n");
        return source;
    };
    {
        // A Source fault stays with its object and Play continues; a compatible fix clears it and Tick resumes.
        take(editor::write_file(component,fail_second_tick(declaration)));
        auto faulty=editor::load_project_scene(project,original);require(faulty.session!=nullptr,"Faulting Source scene failed to load.");
        auto& run=*faulty.session;const auto mover=entity(run);
        complete(run.start());take(run.update(.5f));take(run.update(.5f));take(run.update(.5f));check_position(position(run,mover),1);
        const auto faults=run.take_diagnostics();
        require(faults.size()==1 && faults[0].kind==world::DiagnosticKind::behavior_failed && faults[0].phase==world::DiagnosticPhase::tick &&
            faults[0].entity==mover && faults[0].type=="project.Move","A Source Tick fault was not isolated to one diagnostic on its object.");
        const auto line=editor::describe_diagnostic(run,faults[0]);
        require(line.starts_with(L"Diagnostic [error] behavior_failed in tick") && line.find(L"(project.Move)")!=std::wstring::npos &&
            line.find(assets::wide(faults[0].message))!=std::wstring::npos,"The Project Output line lost the fault's kind, type or message.");
        take(editor::write_file(component,declaration));take(faulty.code->poll(project,true));
        take(run.update(.5f));check_position(position(run,mover),2);
        require(run.take_diagnostics().empty(),"A fixed object did not resume Tick after its code was published.");
        complete(run.stop());
    }
    auto extended=declaration;extended.insert(extended.find("    void tick"),"    float extra = 7.0f;\n");take(editor::write_file(component,extended));
    const auto changed=playing.code->poll(project,true);require(!changed && changed.error().code==ErrorCode::rebuild_required,"A schema change did not request context rebuilding.");
    auto rebuilt=editor::load_project_scene(project,original);require(rebuilt.session!=nullptr,"Could not rebuild source types from the saved scene.");
    require(std::get<float>(field(*rebuilt.session,"extra"))==7,"Rebuilding did not apply a new field's default.");
    take(editor::write_file(root/"Source/UI/Main.ui.source",editor::ProjectCode::ui_source()));take(project.set_ui_source("Source/UI/Main.ui.source"));
    editor::ProjectUI project_ui;project_ui.sync(&project);
    const auto game_frame=take(project_ui.prepare(ui,{{1280,800},1,1},1,1,[](const ui::Reply&){}));
    require(game_frame && ui.accept(game_frame),"Could not accept the project UI frame.");
    require(project_ui.diagnostic().empty(),"Project UI did not compile through the native UI pipeline.");
    take(editor::write_file(root/"Source/UI/Main.ui.source","ui broken {"));project_ui.sync(&project);
    // Explicitly opening a new project UI exercises cold failure independently of debounce timing.
    editor::ProjectUI broken_ui;broken_ui.sync(&project);
    require(!broken_ui.prepare(ui,{{1280,800},1,1},0,1,[](const ui::Reply&){}),"Cold-invalid project UI unexpectedly produced a frame.");
    require(!broken_ui.diagnostic().empty(),"A broken project UI did not report its error.");
    take(editor::write_file(root/"Source/UI/Main.ui.source",editor::ProjectCode::ui_source()));

    const auto snapshot=root/"Cache/run.scene",report=root/"Cache/result.scene";take(editor::write_file(snapshot,original));
    application::PlayProcess process;take(process.start(root,snapshot,true,20,report));wait(process,std::chrono::seconds(45));
    const auto output=process.take_output();std::fwrite(output.data(),1,output.size(),stdout);
    require(process.exit_code()==0,"The real isolated runner failed.");
    // The Play report: every loop is a timed frame, headless frames are sampled, and no phase left its parent.
    require(output.find("Performance: 20 frames (window ")!=output.npos && output.find(" app.ui ")!=output.npos &&
        output.find(" world.update ")!=output.npos && output.find(" app.gc ")!=output.npos &&
        output.find("timing structure errors")==output.npos,"The runner did not report its frame performance.");
    auto executed=editor::load_project_scene(project,take(assets::read_file(report)));require(executed.session!=nullptr,"Run output did not restore through the shared scene loader.");
    check_position(position(*executed.session,entity(*executed.session)),2.f/3);
    require(take(assets::read_file(root/"Content/Scenes/Main.scene"))==original,"The runner overwrote the authored scene.");
    {
        // The runner enters Play and services its channel at the host boundary: before start, then before every update.
        struct Probe final : application::PlayChannel {
            std::string batch;std::uint64_t calls{};bool stopped_first{};
            void service(editor::Session& session,editor::Project& project,const application::Report& report) override {
                if(calls++) return;
                stopped_first=!session.world().running();
                const auto issues=take(session.apply_live(batch,project));report(issues);
                require(issues.empty(),"The probe's Play change did not apply.");
            }
        } probe;
        probe.batch=R"({"ops":[{"op":"set","id":")"+std::to_string(*document.document_identity(behavior(document)))+R"(","properties":{"speed":4.0}}]})";
        application::RunOptions options;options.project=root;options.scene_snapshot=snapshot;options.report=report;
        options.headless=true;options.frames=20;options.channel=&probe;
        require(application::run_project(options)==0 && probe.calls==20 && probe.stopped_first,
            "The runner did not service its Play channel before start and before every update.");
        auto served=editor::load_project_scene(project,take(assets::read_file(report)));require(served.session!=nullptr,"The served run's report did not load.");
        check_position(position(*served.session,entity(*served.session)),4.f/3);
    }
    verify_link_protocol(root);verify_play_link(root,project,original,snapshot);
    {
        auto& source=*rebuilt.session;auto& runtime=source.world().runtime();
        const auto move=behavior(source);const std::array speed{take(runtime.find_field(take(runtime.type_of(move)),"speed"))};
        take(source.set_property(move,speed,Scalar{5.f},990001));
        const auto authored=take(source.serialize());
        // A removed field keeps its saved value until the field returns.
        constexpr std::string_view speed_field="    float speed = 2.0f;\n";
        auto narrowed_source=extended;narrowed_source.erase(narrowed_source.find(speed_field),speed_field.size());
        narrowed_source.replace(narrowed_source.find("self.speed"),10,"2.0f");
        take(editor::write_file(component,narrowed_source));
        auto narrowed=editor::load_project_scene(project,authored);
        require(narrowed.session && !narrowed.error && narrowed.session->load_issues().size()==1 &&
            narrowed.session->load_issues().front().kind==world::DiagnosticKind::unknown_field &&
            take(narrowed.session->serialize())==authored,"A removed Source field lost its saved value.");
        require(!editor::load_project_scene(project,authored,editor::LoadPolicy::strict).session,"Strict loading accepted a removed Source field.");
        take(editor::write_file(component,extended));
        auto widened=editor::load_project_scene(project,take(narrowed.session->serialize()));
        require(widened.session && widened.session->load_issues().empty() && std::get<float>(field(*widened.session,"speed"))==5,
            "A restored Source field did not recover its saved value.");
        // The editor's document publishes editor.* operator records before Source builds; Source still cannot name them.
        take(editor::write_file(helper,"float travel(float speed,float dt) { editor.ToolSettings tools; return speed * dt; }\n"));
        const auto reserved=editor::load_project_scene(project,authored,editor::LoadPolicy::tolerant,false,editor::SessionRole::editor);
        require(!reserved.session && reserved.error && reserved.error->message.find("editor.ToolSettings")!=std::string::npos &&
            reserved.error->message.find("Unknown registered value type")!=std::string::npos,
            "Project Source in the editor's document used an editor operator record.");
        // A broken build opens interactively with placeholders; once it builds, a fresh load restores them.
        take(editor::write_file(helper,"float travel(float speed,float dt) { return missing; }\n"));
        require(!editor::load_project_scene(project,authored).session,"Exact loading accepted a broken Source build.");
        auto broken=editor::load_project_scene(project,authored,editor::LoadPolicy::tolerant,true);
        require(broken.session && broken.error && !broken.code && broken.session->placeholder_count()==1 &&
            take(broken.session->serialize())==authored,
            "A broken build did not load placeholders that save their original type and values.");
        take(process.start(root,snapshot,true,1));wait(process,std::chrono::seconds(45));
        require(process.exit_code()!=0,"Play ran while the Source build was broken.");static_cast<void>(process.take_output());
        take(editor::write_file(helper,helpers));
        auto repaired=editor::load_project_scene(project,take(broken.session->serialize()));
        require(repaired.session && repaired.session->load_issues().empty() && std::get<float>(field(*repaired.session,"speed"))==5,
            "Rebuilding types did not restore placeholder values.");
        // Play runs with inert placeholders for types the successful build no longer declares.
        auto missing=authored;missing.replace(missing.find("\"project.Move\""),14,"\"project.Gone\"");
        take(editor::write_file(snapshot,missing));take(process.start(root,snapshot,true,2));wait(process,std::chrono::seconds(45));
        const auto run_output=process.take_output();
        require(process.exit_code()==0 && run_output.find("1 placeholder object(s)")!=std::string::npos,
            "Play did not run with a missing type or did not report its placeholder summary.");
        take(editor::write_file(snapshot,original));
        // The real runner reports an isolated fault once in its output and keeps running;
        // --halt-on-fault reports the same fault and stops.
        take(editor::write_file(component,fail_second_tick(extended)));
        for(const bool halt:{false,true}) {
            take(process.start(root,snapshot,true,20,halt?std::filesystem::path{}:report,false,halt));wait(process,std::chrono::seconds(45));
            const auto faulted=process.take_output();std::fwrite(faulted.data(),1,faulted.size(),stdout);
            const auto at=faulted.find("Diagnostic [error] behavior_failed in tick");
            require(at!=std::string::npos && faulted.find("(project.Move)",at)!=std::string::npos && faulted.find("Diagnostic [",at+1)==std::string::npos,
                "Play did not report the Source fault exactly once.");
            if(halt) require(process.exit_code()!=0 && faulted.find("Project stopped")==std::string::npos,"--halt-on-fault kept running after the first fault.");
            else {
                require(process.exit_code()==0 && faulted.find("Project stopped: 20 frames")!=std::string::npos,"An isolated Source fault stopped Play.");
                auto finished=editor::load_project_scene(project,take(assets::read_file(report)));
                require(finished.session!=nullptr,"The faulted run's report did not load.");
                check_position(position(*finished.session,entity(*finished.session)),2.f/60);
            }
        }
        take(editor::write_file(component,extended));
    }

    const std::string infinite="class project.Move : world.Component { float speed=2.0f; bool moving=true; uint ticks=0; float extra=7.0f; void tick(float dt) { while (true) {} } }";
    take(editor::write_file(component,infinite));take(process.start(root,snapshot,true));
    const auto ready_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(45);std::string startup;
    while(process.running() && startup.find("Project started:")==std::string::npos && std::chrono::steady_clock::now()<ready_deadline) {process.poll();startup+=process.take_output();Sleep(10);}
    require(process.running() && startup.find("Project started:")!=std::string::npos,"Infinite-loop run did not start.");
    Sleep(100);process.stop();wait(process,std::chrono::seconds(5));require(process.forced(),"A stuck Source invocation was not isolated and terminated.");
    take(editor::write_file(component,extended));
    const std::string infinite_ui=R"(
double forever() { while (true) {} return 1.0d; }
ui ProjectUI(in Text title, in double elapsed, in double entities) {
    Column hud { width: forever(); Text title { text: input.title; } }
})";
    take(editor::write_file(root/"Source/UI/Main.ui.source",infinite_ui));
    // Authoring validation compiles this file without evaluating its expressions.
    take(rebuilt.code->poll(project,true));
    take(process.start(root,{},true,0,{},true));startup.clear();
    const auto preview_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(45);
    while(process.running() && startup.find("Project UI evaluating")==std::string::npos && std::chrono::steady_clock::now()<preview_deadline) {process.poll();startup+=process.take_output();Sleep(10);}
    require(process.running() && startup.find("Project UI evaluating")!=std::string::npos,"Infinite-loop UI preview did not start.");
    Sleep(100);process.stop();wait(process,std::chrono::seconds(5));require(process.forced(),"A stuck UI expression was not isolated and terminated.");
    take(editor::write_file(root/"Source/UI/Main.ui.source",editor::ProjectCode::ui_source()));
    verify_project_ui_reload(root);
    auto moved=root;moved+=L"-moved";std::filesystem::rename(root,moved);
    std::filesystem::remove(moved/"Cache/run.scene");std::filesystem::remove(moved/"Cache/result.scene");std::filesystem::remove(moved/"Cache");
    auto reopened=take(editor::Project::open(moved));
    auto relocated=editor::load_project_scene(reopened,take(assets::read_file(moved/"Content/Scenes/Main.scene")));
    require(relocated.session!=nullptr && std::get<float>(field(*relocated.session,"speed"))==2,"Moving the project broke source or scene association.");
    std::printf("Project integration: PASS (Source classes, files, properties, scene, hot update, diagnostics, saved data across Source changes, placeholders, UI, process, stop, relocation)\nEvidence: %s\n",assets::utf8(moved.wstring()).c_str());
}
}
int main(int argc,char** argv) {
    try {
        application::install_frame_timing();
        const auto root=std::filesystem::current_path()/"project-integration-evidence"/(std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        if(argc==2 && std::string_view(argv[1])=="--textures") {
            verify_texture_link(root);return 0;
        }
        require(argc==1,"Expected no arguments or --textures.");
        run(root);return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"Project integration: FAIL: %s\n",error.what());return 1;}
}
