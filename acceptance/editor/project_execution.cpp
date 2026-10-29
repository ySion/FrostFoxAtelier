#include "assets/file.hpp"
#include "assets/json_format.hpp"
#include "editor/project_code.hpp"
#include "editor/project_ui.hpp"
#include "editor/workspace.hpp"
#include "application/play_process.hpp"
#include "application/project_runner.hpp"
#include "application/timing.hpp"
#include "application/presentation.hpp"
#include "logging/logging.hpp"
#include "ui/session.hpp"
#include <Windows.h>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <deque>
#include <utility>

namespace atelier::application {
struct PlayProcessAcceptanceAccess {
    // Only fixture setup: the child and byte pipes are real Win32 resources.
    // All observations below use PlayProcess's public polling/output contract.
    static void attach(PlayProcess& run,HANDLE process,HANDLE job,HANDLE stop,HANDLE out,HANDLE err) {
        for(auto& output:run.output_) output.bytes.reserve(PlayProcess::output_capacity);
        run.process_=process;run.job_=job;run.stop_=stop;
        run.output_[0].handle=out;run.output_[1].handle=err;
    }
    static HANDLE unreadable_stdout(PlayProcess& run,HANDLE non_pipe) {
        return std::exchange(run.output_[0].handle,non_pipe);
    }
};
}

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
struct RunLog {
    application::PlayProcess& process;
    std::unique_ptr<logging::Reader> reader;
    std::vector<logging::Record> records;
    logging::ReaderStatus status;
    std::optional<logging::PublishedHealth> health;
    explicit RunLog(application::PlayProcess& value):process(value) {
        auto opened=logging::Reader::open(process.log_directory());
        if(!opened) throw std::runtime_error(opened.error().message);reader=std::move(*opened);
    }
    void poll() {
        if(process.output_status().process_exited) reader->source_ended();
        auto batch=reader->poll();status=batch.status;health=batch.health;
        for(auto& record:batch.records) records.push_back(std::move(record));
    }
    static const logging::FieldValue* field(const logging::Record& record,std::string_view key) {
        for(const auto& field:record.entry.fields) if(field.key==key) return &field.value;
        return nullptr;
    }
    static bool integer(const logging::Record& record,std::string_view key,std::uint64_t value) {
        const auto* item=field(record,key);return item && std::get_if<std::uint64_t>(item) && std::get<std::uint64_t>(*item)==value;
    }
    static bool text(const logging::Record& record,std::string_view key,std::string_view value) {
        const auto* item=field(record,key);return item && std::get_if<std::string>(item) && std::get<std::string>(*item)==value;
    }
    std::size_t count(std::string_view code) const {
        return static_cast<std::size_t>(std::ranges::count_if(records,[&](const auto& item){return item.entry.event_code==code;}));
    }
    template<class Predicate> bool has(std::string_view code,Predicate predicate) const {
        return std::ranges::any_of(records,[&](const auto& item){return item.entry.event_code==code && predicate(item);});
    }
    struct ExpectedDiagnostic {
        std::string_view category,code;
        logging::Severity severity;
        std::size_t count;
        std::vector<logging::Field> fields{};
    };
    void finish(bool normal=true,std::initializer_list<ExpectedDiagnostic> expected={}) {
        require(!process.running(),"Cannot finish evidence before the Runner ends.");reader->source_ended();
        const auto deadline=GetTickCount64()+5000;
        do {poll();if(status.finished) break;Sleep(5);} while(GetTickCount64()<deadline);
        require(status.finished,"Runner Reader did not finish its retained tail.");
        if(normal) {
            require(status.has_final_summary && !status.incomplete && health && health->final,"Normal Runner omitted complete final evidence.");
            const auto& final=health->report;const auto& state=final.health;
            require(final.backend_acknowledged && final.sink_flush_succeeded && final.summary_published &&
                !state.facility_failures && !state.backend_notifications && !state.rejected_queue_full &&
                !state.rejected_producer_limit && !state.rejected_record_too_large && !state.rejected_invalid_record &&
                !state.rejected_internal_failure,"Runner final logging health was not complete and healthy.");
        } else require(!status.has_final_summary && status.incomplete,"Forced Runner invented a normal final summary.");
        std::vector<std::size_t> counts(expected.size());
        for(const auto& record:records) {
            if(record.entry.severity<logging::Severity::warning) continue;
            std::size_t matches{},index{},matched{};
            for(const auto& rule:expected) {
                if(record.entry.category==rule.category && record.entry.event_code==rule.code && record.entry.severity==rule.severity &&
                    std::ranges::all_of(rule.fields,[&](const auto& wanted){const auto* value=field(record,wanted.key);return value && *value==wanted.value;}))
                    {++matches;matched=index;}
                ++index;
            }
            if(matches!=1) throw std::runtime_error("Unexpected/ambiguous Runner diagnostic: "+record.entry.event_code+": "+record.entry.message);
            ++counts[matched];
            if(record.entry.category=="world") require(has("world.scene_loaded",[&](const auto& loaded) {
                const auto* document=field(loaded,"document");return document && field(record,"document") && *document==*field(record,"document");
            }),"World diagnostic belongs to another document.");
        }
        std::size_t index{};
        for(const auto& rule:expected) require(counts[index++]==rule.count,"Runner diagnostic count differs from the case's exact expectation.");
    }
    template<class Predicate> void await(std::string_view code,Predicate predicate) {
        const auto deadline=GetTickCount64()+45000;
        do {
            process.poll();poll();
            if(has(code,predicate)) return;
            if(!process.running()) break;
            Sleep(10);
        } while(GetTickCount64()<deadline);
        throw std::runtime_error("Runner did not publish event "+std::string(code));
    }
    void await(std::string_view code) {await(code,[](const auto&){return true;});}
};
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
struct RawHandle {
    HANDLE value{};
    RawHandle()=default;
    explicit RawHandle(HANDLE handle):value(handle) {}
    ~RawHandle(){if(value && value!=INVALID_HANDLE_VALUE) CloseHandle(value);}
    RawHandle(const RawHandle&)=delete;
    RawHandle& operator=(const RawHandle&)=delete;
};
struct RawChild {
    RawHandle child,ready,advance;
    application::PlayProcess process;
    explicit RawChild(std::wstring_view mode,std::wstring_view runner_arguments={}) {
        SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
        RawHandle stop{CreateEventW(&security,TRUE,FALSE,nullptr)};
        ready.value=CreateEventW(&security,TRUE,FALSE,nullptr);advance.value=CreateEventW(&security,TRUE,FALSE,nullptr);
        require(stop.value && ready.value && advance.value,"Cannot create raw-output fixture events.");
        RawHandle out,write_out,err,write_err;
        // Larger than production pipes so a confirmed-exited child can leave
        // more than one poll/close budget queued, with no scheduling race.
        require(CreatePipe(&out.value,&write_out.value,&security,1024*1024) && SetHandleInformation(out.value,HANDLE_FLAG_INHERIT,0) &&
            CreatePipe(&err.value,&write_err.value,&security,1024*1024) && SetHandleInformation(err.value,HANDLE_FLAG_INHERIT,0),
            "Cannot create raw-output fixture pipes.");
        RawHandle input{CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr)};
        require(input.value!=INVALID_HANDLE_VALUE,"Cannot create raw-output fixture stdin.");
        RawHandle job{CreateJobObjectW(nullptr,nullptr)};
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        require(job.value && SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits)),
            "Cannot configure raw-output fixture job.");
        std::wstring executable(32768,L'\0');const auto size=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
        require(size && size<executable.size(),"Cannot locate raw-output fixture executable.");executable.resize(size);
        auto command=L"\""+executable+L"\" --raw-output-child "+std::wstring(mode)+L" "+
            std::to_wstring(reinterpret_cast<std::uintptr_t>(stop.value))+L" "+
            std::to_wstring(reinterpret_cast<std::uintptr_t>(ready.value))+L" "+
            std::to_wstring(reinterpret_cast<std::uintptr_t>(advance.value));
        if(!runner_arguments.empty()) {
            executable=(std::filesystem::path(executable).parent_path()/L"atelier_editor.exe").wstring();
            command=L"\""+executable+L"\" "+std::wstring(runner_arguments);
        }
        SIZE_T bytes{};InitializeProcThreadAttributeList(nullptr,1,0,&bytes);
        std::vector<std::byte> storage(bytes);auto* attributes=reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        require(InitializeProcThreadAttributeList(attributes,1,0,&bytes),"Cannot initialize raw-output fixture inheritance.");
        struct Attributes {PPROC_THREAD_ATTRIBUTE_LIST value;~Attributes(){DeleteProcThreadAttributeList(value);}} owner{attributes};
        std::array handles{write_out.value,write_err.value,input.value,stop.value,ready.value,advance.value};
        require(UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles.data(),sizeof(handles),nullptr,nullptr),
            "Cannot restrict raw-output fixture inheritance.");
        STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.lpAttributeList=attributes;
        startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;startup.StartupInfo.hStdInput=input.value;
        startup.StartupInfo.hStdOutput=write_out.value;startup.StartupInfo.hStdError=write_err.value;
        PROCESS_INFORMATION created{};
        require(CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,
            EXTENDED_STARTUPINFO_PRESENT|CREATE_SUSPENDED|DETACHED_PROCESS,nullptr,nullptr,&startup.StartupInfo,&created),
            "Cannot start raw-output fixture child.");
        RawHandle process_handle{created.hProcess},thread{created.hThread};
        if(!AssignProcessToJobObject(job.value,process_handle.value)) {
            TerminateProcess(process_handle.value,1);WaitForSingleObject(process_handle.value,1000);
            throw std::runtime_error("Cannot attach raw-output fixture child to its job.");
        }
        require(DuplicateHandle(GetCurrentProcess(),process_handle.value,GetCurrentProcess(),&child.value,0,FALSE,DUPLICATE_SAME_ACCESS),
            "Cannot retain raw-output fixture exit witness.");
        application::PlayProcessAcceptanceAccess::attach(process,process_handle.value,job.value,stop.value,out.value,err.value);
        process_handle.value=job.value=stop.value=out.value=err.value=nullptr;
        require(ResumeThread(thread.value)!=static_cast<DWORD>(-1),"Cannot resume raw-output fixture child.");
    }
    void await_ready() {require(WaitForSingleObject(ready.value,5000)==WAIT_OBJECT_0,"Raw-output child did not reach its write boundary.");}
    void await_exit() {require(WaitForSingleObject(child.value,5000)==WAIT_OBJECT_0,"Raw-output child did not exit before draining.");}
};
void write_raw(DWORD stream,std::string_view bytes) {
    while(!bytes.empty()) {
        DWORD written{};
        require(WriteFile(GetStdHandle(stream),bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr) && written,
            "Raw-output child could not write its pipe.");
        bytes.remove_prefix(written);
    }
}
int raw_output_child(std::span<char*> arguments) {
    require(arguments.size()==4,"Invalid raw-output child arguments.");
    const std::string_view mode=arguments[0];
    const auto handle=[](const char* text){return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::stoull(text)));};
    const auto stop=handle(arguments[1]),ready=handle(arguments[2]),advance=handle(arguments[3]);
    if(mode=="split") {
        write_raw(STD_OUTPUT_HANDLE,std::string(4095,'a')+"\xe4");SetEvent(ready);
        require(WaitForSingleObject(advance,5000)==WAIT_OBJECT_0,"Raw-output UTF-8 boundary was not released.");
        write_raw(STD_OUTPUT_HANDLE,std::string_view{"\xb8\xad\0tail",7});
        write_raw(STD_ERROR_HANDLE,"stderr without newline");
    } else if(mode=="tail" || mode=="overflow" || mode=="large") {
        const auto size=mode=="tail"?96*1024:mode=="overflow"?3*64*1024+17:640*1024;
        write_raw(STD_OUTPUT_HANDLE,std::string(static_cast<std::size_t>(size),'o'));
        if(mode!="overflow") write_raw(STD_ERROR_HANDLE,std::string(static_cast<std::size_t>(size),'e'));
        SetEvent(ready);
    } else {
        require(mode=="cooperative" || mode=="stuck","Unknown raw-output child mode.");
        write_raw(STD_OUTPUT_HANDLE,"raw-ready");SetEvent(ready);
        if(mode=="stuck") Sleep(INFINITE);
        require(WaitForSingleObject(stop,5000)==WAIT_OBJECT_0,"Raw-output child was not stopped.");
        write_raw(STD_OUTPUT_HANDLE,"-stopped");write_raw(STD_ERROR_HANDLE,"stderr-tail");
    }
    return 0;
}
void verify_raw_output() {
    using Process=application::PlayProcess;
    const auto complete_output=[](Process& process,std::string& out,std::string& err) {
        const auto deadline=GetTickCount64()+5000;
        do {
            process.poll();const auto batch=process.take_raw_output();
            require(batch.standard_output.offset==out.size() && batch.standard_error.offset==err.size(),
                "Raw-output byte offsets hid a gap or replayed consumed bytes.");
            out+=batch.standard_output.bytes;err+=batch.standard_error.bytes;
            if(!process.running()) break;
            Sleep(1);
        } while(GetTickCount64()<deadline);
        const auto state=process.output_status();
        require(!process.running() && process.exit_code()==0 && state.process_exited && !state.pending &&
            state.standard_output.eof && state.standard_error.eof && !state.standard_output.read_error && !state.standard_error.read_error &&
            !state.standard_output.tail_loss_unknown && !state.standard_error.tail_loss_unknown,
            "Normal raw output did not finish with observed transport EOF.");
    };
    {
        RawChild child{L"split"};child.await_ready();child.process.poll();
        const auto first=child.process.take_raw_output();std::string out=first.standard_output.bytes,err=first.standard_error.bytes;
        require(out==std::string(4095,'a')+"\xe4" && err.empty() && !first.status.standard_output.eof,
            "A UTF-8 fragment was decoded, dropped or held until a newline.");
        const auto empty=child.process.take_raw_output();
        require(empty.standard_output.bytes.empty() && empty.standard_error.bytes.empty(),"Raw output replayed consumed bytes.");
        SetEvent(child.advance.value);complete_output(child.process,out,err);
        require(out==std::string(4095,'a')+std::string{"\xe4\xb8\xad\0tail",8} && err=="stderr without newline",
            "Split UTF-8, embedded NUL, stream identity or the final unterminated line was lost.");
    }
    {
        RawChild child{L"tail"};child.await_exit();child.process.poll();const auto first=child.process.output_status();
        require(first.process_exited && first.pending && child.process.running() &&
            first.standard_output.received_bytes && first.standard_error.received_bytes &&
            first.standard_output.received_bytes+first.standard_error.received_bytes<=Process::output_budget,
            "An exited child bypassed the poll budget or closed a pipe with unread tail bytes.");
        std::string out,err;complete_output(child.process,out,err);
        require(out==std::string(96*1024,'o') && err==std::string(96*1024,'e'),"Post-exit polling lost a multi-budget tail.");
    }
    {
        RawChild child{L"overflow"};child.await_exit();wait(child.process,std::chrono::seconds(5));
        const auto output=child.process.take_raw_output();const auto& state=output.status.standard_output;
        require(output.standard_output.bytes==std::string(64*1024,'o') && output.standard_output.offset==2*64*1024+17 &&
            state.received_bytes==3*64*1024+17 && state.dropped_bytes==2*64*1024+17 && !state.abandoned_bytes &&
            state.eof && !state.tail_loss_unknown && !state.read_error,
            "Raw retention did not account for the exact evicted byte prefix.");
    }
    {
        RawChild child{L"cooperative"};child.await_ready();child.process.close();
        const auto state=child.process.output_status();
        require(!child.process.running() && !child.process.forced() && child.process.exit_code()==0 &&
            state.standard_output.eof && state.standard_error.eof && !state.standard_output.tail_loss_unknown && !state.standard_error.tail_loss_unknown,
            "Cooperative close lost the child's normal final output.");
        const auto tail=child.process.take_raw_output();
        require(tail.standard_output.bytes=="raw-ready-stopped" && tail.standard_error.bytes=="stderr-tail","Raw close did not preserve final stream bytes.");
        const auto consumed=child.process.take_raw_output();
        require(consumed.standard_output.bytes.empty() && consumed.standard_error.bytes.empty(),"Raw output duplicated consumed bytes.");
    }
    {
        RawChild child{L"stuck"};child.await_ready();child.process.stop();wait(child.process,std::chrono::seconds(5));
        const auto output=child.process.take_raw_output();
        require(child.process.forced() && child.process.exit_code()!=0 && output.standard_output.bytes=="raw-ready" &&
            output.status.standard_output.eof && output.status.standard_error.eof &&
            output.status.standard_output.tail_loss_unknown && output.status.standard_error.tail_loss_unknown &&
            !output.status.standard_output.dropped_bytes && !output.status.standard_output.abandoned_bytes,
            "Forced termination claimed complete producer output or invented a known lost-byte count.");
    }
    {
        RawChild child{L"stuck"};child.await_ready();child.process.close();
        const auto output=child.process.take_raw_output();
        require(!child.process.running() && child.process.forced() && child.process.exit_code()!=0 &&
            output.standard_output.bytes=="raw-ready" && output.status.standard_output.tail_loss_unknown && output.status.standard_error.tail_loss_unknown,
            "Close did not force a non-cooperative child or lost its captured final fragment.");
    }
    {
        RawChild child{L"large"};child.await_exit();child.process.close();const auto state=child.process.output_status();
        require(!child.process.running() && child.process.exit_code()==0 && !state.pending &&
            state.standard_output.received_bytes+state.standard_error.received_bytes<=8*Process::output_budget,
            "Close drained without its fixed work bound.");
        for(const auto& stream:{state.standard_output,state.standard_error})
            require(stream.received_bytes+stream.abandoned_bytes==640*1024 && stream.abandoned_bytes &&
                stream.dropped_bytes==stream.received_bytes-64*1024 && stream.tail_loss_unknown && !stream.eof && !stream.read_error,
                "Bounded close did not distinguish exact abandoned bytes from an unconfirmed tail.");
        child.process.close();
        require(child.process.output_status().standard_output.abandoned_bytes==state.standard_output.abandoned_bytes,
            "Repeated close counted abandoned bytes twice.");
    }
    {
        RawChild child{L"cooperative"};child.await_ready();
        RawHandle non_pipe{CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr)};
        require(non_pipe.value!=INVALID_HANDLE_VALUE,"Cannot create the real non-pipe read-failure fixture.");
        RawHandle original_pipe{application::PlayProcessAcceptanceAccess::unreadable_stdout(child.process,std::exchange(non_pipe.value,nullptr))};
        child.process.poll();const auto failed=child.process.output_status().standard_output;
        require(failed.read_error && failed.tail_loss_unknown && !failed.eof,"A pipe-read failure was hidden as clean EOF.");
        child.process.stop();wait(child.process,std::chrono::seconds(5));
        require(child.process.output_status().standard_output.read_error==failed.read_error && child.process.exit_code()==0 &&
            child.process.take_raw_output().standard_error.bytes=="stderr-tail","A stdout read failure damaged stderr or overwrote the first error.");
    }
    std::puts("Raw process output: PASS (bounded reads, separate streams, UTF-8 bytes, final fragments, tails, loss accounting, stop and close)");
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
    {
        // A real startup collision must reject before loading or simulating any
        // business, not fall back silently and pretend that logging succeeded.
        const auto refused_report=root/"Cache/refused.scene";
        RawChild refused(L"",L"--run-project \""+root.wstring()+L"\" --headless --frames 1 --report \""+
            refused_report.wstring()+L"\" --log-directory \""+root.wstring()+L"\"");
        wait(refused.process,std::chrono::seconds(45));
        const auto emergency=refused.process.take_raw_output();
        require(refused.process.exit_code()==2 && !std::filesystem::exists(refused_report) &&
            emergency.standard_error.bytes.find("application was not started")!=std::string::npos,
            "Logging startup failure ran business work or omitted the independent emergency explanation.");
    }
    application::PlayProcess process;take(process.start(root,snapshot,true,20,report));wait(process,std::chrono::seconds(45));
    // These simulation-only fixtures deliberately have no Environment/main camera.
    const RunLog::ExpectedDiagnostic no_camera{"runner","runner.camera_unavailable",logging::Severity::warning,1};
    RunLog normal(process);normal.finish(true,{no_camera});
    require(process.exit_code()==0,"The real isolated runner failed.");
    // The Play report: every loop is a timed frame, headless frames are sampled, and no phase left its parent.
    require(normal.has("runner.performance",[](const auto& record){return RunLog::integer(record,"frames",20) && RunLog::integer(record,"structure_errors",0);}),
        "The runner did not report its frame performance.");
    for(const auto phase:{"app.ui","world.update","app.gc"}) require(normal.has("runner.performance_phase",[&](const auto& record){return RunLog::text(record,"phase",phase);}),
        "The Runner lost a measured top-level phase.");
    require(process.take_raw_output().standard_output.bytes.empty(),"Business records were transported twice through stdout.");
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
        RunLog broken_log(process);broken_log.finish(true,{
            {"source","source.build_rejected",logging::Severity::error,1,{{"phase",std::string("prepare")},{"project_root",assets::utf8(root.generic_wstring())}}},
            {"runner","runner.failed",logging::Severity::error,1}});
        require(process.exit_code()!=0 && !broken_log.count("runner.started") && broken_log.count("source.build_rejected")==1,
            "Play ran while the Source build was broken, or omitted its build failure.");
        take(editor::write_file(helper,helpers));
        auto repaired=editor::load_project_scene(project,take(broken.session->serialize()));
        require(repaired.session && repaired.session->load_issues().empty() && std::get<float>(field(*repaired.session,"speed"))==5,
            "Rebuilding types did not restore placeholder values.");
        // Play runs with inert placeholders for types the successful build no longer declares.
        auto missing=authored;missing.replace(missing.find("\"project.Move\""),14,"\"project.Gone\"");
        take(editor::write_file(snapshot,missing));take(process.start(root,snapshot,true,2));wait(process,std::chrono::seconds(45));
        RunLog missing_log(process);missing_log.finish(true,{no_camera,
            {"world","world.unknown_type",logging::Severity::warning,1,{{"origin",std::string("load")},{"type",std::string("project.Gone")}}},
            {"world","world.unknown_field",logging::Severity::warning,1,{{"phase",std::string("load")},{"type",std::string("project.Gone")},{"field_path",std::string("/properties/extra")}}},
            {"world","world.unknown_field",logging::Severity::warning,1,{{"phase",std::string("load")},{"type",std::string("project.Gone")},{"field_path",std::string("/properties/moving")}}},
            {"world","world.unknown_field",logging::Severity::warning,1,{{"phase",std::string("load")},{"type",std::string("project.Gone")},{"field_path",std::string("/properties/speed")}}},
            {"world","world.unknown_field",logging::Severity::warning,1,{{"phase",std::string("load")},{"type",std::string("project.Gone")},{"field_path",std::string("/properties/ticks")}}}});
        require(process.exit_code()==0 && missing_log.has("world.scene_loaded",[](const auto& record){return RunLog::integer(record,"placeholders",1);}) &&
            missing_log.count("world.unknown_type")==1,
            "Play did not run with a missing type or did not report its placeholder summary.");
        take(editor::write_file(snapshot,original));
        // The real runner reports an isolated fault once in its output and keeps running;
        // --halt-on-fault reports the same fault and stops.
        take(editor::write_file(component,fail_second_tick(extended)));
        std::optional<logging::Entry> isolated_fault;
        for(const bool halt:{false,true}) {
            take(process.start(root,snapshot,true,20,halt?std::filesystem::path{}:report,false,halt));wait(process,std::chrono::seconds(45));
            RunLog faulted(process);
            const RunLog::ExpectedDiagnostic fault{"world","world.behavior_failed",logging::Severity::error,1,
                {{"origin",std::string("world")},{"phase",std::string("tick")},{"type",std::string("project.Move")},{"repeats",std::uint64_t{1}}}};
            if(halt) faulted.finish(true,{no_camera,fault,{"runner","runner.failed",logging::Severity::error,1}});
            else faulted.finish(true,{no_camera,fault});
            require(faulted.count("world.behavior_failed")==1 && faulted.has("world.behavior_failed",[&](const auto& record) {
                return record.entry.severity==logging::Severity::error && RunLog::text(record,"phase","tick") &&
                    RunLog::text(record,"type","project.Move") && !record.entry.source.file.empty() &&
                    record.entry.source.disk_path==assets::utf8(component.generic_wstring()) &&
                    record.entry.source.version && record.entry.source.offset && RunLog::field(record,"object") && RunLog::field(record,"entity");
            }),
                "Play did not report the Source fault exactly once.");
            const auto diagnostic=std::ranges::find_if(faulted.records,[](const auto& record){return record.entry.event_code=="world.behavior_failed";});
            if(!halt) isolated_fault=diagnostic->entry;
            else require(isolated_fault && isolated_fault->message==diagnostic->entry.message &&
                isolated_fault->source.file==diagnostic->entry.source.file && isolated_fault->source.offset==diagnostic->entry.source.offset,
                "Isolate/halt changed the original diagnostic rather than just its exit policy.");
            require(faulted.health->report.health.first_error && faulted.health->report.health.first_error->entry.event_code=="world.behavior_failed",
                "The Runner's cleanup replaced the business first cause.");
            if(halt) require(process.exit_code()!=0 && !faulted.count("runner.stopped"),"--halt-on-fault kept running after the first fault.");
            else {
                require(process.exit_code()==0 && faulted.has("runner.stopped",[](const auto& record){return RunLog::integer(record,"frames",20);}),"An isolated Source fault stopped Play.");
                auto finished=editor::load_project_scene(project,take(assets::read_file(report)));
                require(finished.session!=nullptr,"The faulted run's report did not load.");
                check_position(position(*finished.session,entity(*finished.session)),2.f/60);
            }
        }
        take(editor::write_file(component,extended));
    }

    const std::string infinite="class project.Move : world.Component { float speed=2.0f; bool moving=true; uint ticks=0; float extra=7.0f; void tick(float dt) { while (true) {} } }";
    take(editor::write_file(component,infinite));take(process.start(root,snapshot,true));
    RunLog startup(process);startup.await("runner.started");
    require(process.running(),"Infinite-loop run did not start.");
    Sleep(100);process.stop();wait(process,std::chrono::seconds(5));require(process.forced(),"A stuck Source invocation was not isolated and terminated.");
    startup.finish(false);
    take(editor::write_file(component,extended));
    const std::string infinite_ui=R"(
double forever() { while (true) {} return 1.0d; }
ui ProjectUI(in Text title, in double elapsed, in double entities) {
    Column hud { width: forever(); Text title { text: input.title; } }
})";
    take(editor::write_file(root/"Source/UI/Main.ui.source",infinite_ui));
    // Authoring validation compiles this file without evaluating its expressions.
    take(rebuilt.code->poll(project,true));
    take(process.start(root,{},true,0,{},true));RunLog preview_log(process);preview_log.await("runner.ui_evaluating");
    require(process.running(),"Infinite-loop UI preview did not start.");
    Sleep(100);process.stop();wait(process,std::chrono::seconds(5));require(process.forced(),"A stuck UI expression was not isolated and terminated.");
    preview_log.finish(false);
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
        if(argc>=2 && std::string_view(argv[1])=="--raw-output-child")
            return raw_output_child({argv+2,static_cast<std::size_t>(argc-2)});
        if(argc==2 && std::string_view(argv[1])=="--raw-output") {verify_raw_output();return 0;}
        application::install_frame_timing();
        const auto root=std::filesystem::current_path()/"project-integration-evidence"/(std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        if(argc==2 && std::string_view(argv[1])=="--textures") {
            verify_texture_link(root);return 0;
        }
        require(argc==1,"Expected no arguments, --textures or --raw-output.");
        verify_raw_output();run(root);return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"Project integration: FAIL: %s\n",error.what());return 1;}
}
