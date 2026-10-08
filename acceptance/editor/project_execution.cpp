#include "editor/project_code.hpp"
#include "editor/project_ui.hpp"
#include "editor/workspace.hpp"
#include "application/play_process.hpp"
#include "application/presentation.hpp"
#include "ui/session.hpp"
#include <Windows.h>
#include <chrono>
#include <cstdio>
#include <cmath>

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
float position(editor::Session& document,ObjectId actor) {
    const auto root=take(world::root_component(document.world().runtime(),actor));require(root.has_value(),"Actor lost its root.");
    return take(world::read_transform(document.world().runtime(),*root)).position.x;
}
void check_position(float value,float expected){require(std::abs(value-expected)<.0001f,"Source behavior produced the wrong transform.");}
ObjectId actor(editor::Session& document) {return take(document.world().members()).front().id();}
ObjectId behavior(editor::Session& document) {
    auto& runtime=document.world().runtime();const auto type=take(runtime.find_type("project.Move"));
    for(const auto& component:take(world::components(runtime,actor(document)))) if(take(runtime.type_of(component.id()))==type) return component.id();
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
void run(const std::filesystem::path& root) {
    auto project=take(editor::Project::create(root));
    const auto helper=root/"Source/Math.source",component=root/"Source/Move.source";
    const std::string helpers="float travel(float speed,float dt) { return speed * dt; }\n";
    const std::string declaration="class project.Move : world.Component {\n    float speed = 2.0f;\n    bool moving = true;\n    uint ticks = 0;\n    void tick(float dt) {\n        if (self.moving) { self.translate_owner(travel(self.speed, dt), 0.0f, 0.0f); }\n        self.ticks = self.ticks + 1;\n    }\n}\n";
    {
        auto empty=editor::load_project_scene(project,take(editor::read_file(root/"Content/Scenes/Main.scene")));
        if(!empty.session) throw std::runtime_error("Empty project: "+empty.error->message);
        complete(empty.session->add_actor());
        take(editor::write_file(helper,helpers));take(editor::write_file(component,declaration));
        const auto first=empty.code->poll(project,true);
        require(!first && first.error().code==ErrorCode::rebuild_required,"Adding the first Source type did not request a scene rebuild.");
    }
    auto loaded=editor::load_project_scene(project,take(editor::read_file(root/"Content/Scenes/Main.scene")));
    if(!loaded.session) throw std::runtime_error("Initial project: "+loaded.error->message);
    auto& document=*loaded.session;
    complete(document.create_static_mesh(editor::mesh_assets().front()));
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
            std::printf("Control %llu parent %llu kind %u text %s\n",static_cast<unsigned long long>(control.key),static_cast<unsigned long long>(control.parent),static_cast<unsigned>(control.kind),editor::utf8(control.text).c_str());
    }
    display(std::move(described));workspace.commit_ui_controls();auto prepared=display(ui.prepare());require(ui.accept(prepared),"Could not accept project property UI.");
    require(workspace.ui_error().empty(),"Editor UI failed while projecting source properties.");
    require(std::ranges::any_of(controls,[](const auto& item){return item.kind==ui::ControlKind::number && item.value==2;}),"Source number field did not reach the shared editor.");
    require(std::ranges::any_of(controls,[](const auto& item){return item.kind==ui::ControlKind::checkbox && item.text==L"Moving" && item.selected;}),"Source boolean field did not reach its checkbox.");

    auto playing=editor::load_project_scene(project,original);require(playing.session!=nullptr,"Play snapshot failed to load.");
    auto& play=*playing.session;const auto moving_actor=actor(play);
    complete(play.start());take(play.update(.5f));check_position(position(play,moving_actor),1);check_position(position(document,actor(document)),0);
    require(!document.dirty(),"Playing modified the authoring document.");
    take(editor::write_file(helper,"float travel(float speed,float dt) { return speed * dt * 2.0f; }\n"));
    take(playing.code->poll(project,true));take(play.update(.5f));check_position(position(play,moving_actor),3);
    take(editor::write_file(helper,"float travel(float speed,float dt) { return missing; }\n"));
    require(!playing.code->poll(project,true),"Invalid project source was published.");
    require(playing.code->report()->error && playing.code->report()->error->source && playing.code->report()->error->source->path=="Source/Math.source","Compiler lost the failing file identity.");
    take(play.update(.5f));check_position(position(play,moving_actor),5);complete(play.stop());complete(play.stop());
    take(workspace.source_editor.open(component));workspace.project_build=playing.code->report();workspace.dock.select(editor::source_control::tab);
    controls=workspace.describe(document,{1280,800},ui.presented().get(),&project);display(ui.set_controls(controls));workspace.commit_ui_controls();
    require(workspace.source_editor.locate_problem(),"Cross-file compiler diagnostic could not open its source.");
    require(workspace.source_editor.active()->path==std::filesystem::canonical(helper),"Diagnostic opened the wrong file.");

    take(editor::write_file(helper,helpers));take(playing.code->poll(project,true));
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
    auto executed=editor::load_project_scene(project,take(editor::read_file(report)));require(executed.session!=nullptr,"Run output did not restore through the shared scene loader.");
    check_position(position(*executed.session,actor(*executed.session)),2.f/3);
    require(take(editor::read_file(root/"Content/Scenes/Main.scene"))==original,"The runner overwrote the authored scene.");

    const std::string infinite="class project.Move : world.Component { float speed=2.0f; bool moving=true; uint ticks=0; float extra=7.0f; void tick(float dt) { while (true) {} } }";
    take(editor::write_file(component,infinite));take(process.start(root,snapshot,true));
    const auto ready_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(45);std::string startup;
    while(process.running() && startup.find("Project started:")==std::string::npos && std::chrono::steady_clock::now()<ready_deadline) {process.poll();startup+=process.take_output();Sleep(10);}
    require(process.running() && startup.find("Project started:")!=std::string::npos,"Infinite-loop run did not start.");
    Sleep(100);process.stop();wait(process,std::chrono::seconds(5));require(process.forced(),"A stuck Source invocation was not isolated and terminated.");
    take(editor::write_file(component,extended));
    const std::string infinite_ui=R"(
double forever() { while (true) {} return 1.0d; }
ui ProjectUI(in Text title, in double elapsed, in double actors) {
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
    auto relocated=editor::load_project_scene(reopened,take(editor::read_file(moved/"Content/Scenes/Main.scene")));
    require(relocated.session!=nullptr && std::get<float>(field(*relocated.session,"speed"))==2,"Moving the project broke source or scene association.");
    std::printf("Project integration: PASS (Source classes, files, properties, scene, hot update, diagnostics, UI, process, stop, relocation)\nEvidence: %s\n",editor::utf8(moved.wstring()).c_str());
}
}
int main() {
    try {
        const auto root=std::filesystem::current_path()/"project-integration-evidence"/(std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        run(root);return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"Project integration: FAIL: %s\n",error.what());return 1;}
}
