// Included by document.cpp inside its existing acceptance namespace.
std::string reload_source(unsigned version) {
    return "ui Leaf(in double offset) { Text label { text: \"Version "+std::to_string(version)+
        "\"; height: input.offset + "+std::to_string(version)+
        ".0d; } }\nui Host(in double offset) { Column root { Leaf child { offset: input.offset; } } }";
}
void verify_ui_code_reuse() {
    namespace language=ui_language;
    Runtime runtime;take(ui_language::register_types(runtime));
    auto first=take(language::Module::compile(runtime,"reuse.ui.source",reload_source(1)));
    require(first->compilation().compiled==2 && first->compilation().reused==0,"Cold UI compile counts are wrong.");
    ui::ControlKey keys=30000;
    const std::array<language::InputValue,1> initial{0.0};
    auto old=take(language::Instance::create(first->find("Host"),0,keys,{},initial));
    auto second=take(language::Module::compile(runtime,"reuse.ui.source",reload_source(2),first.get()));
    require(second->compilation().compiled==1 && second->compilation().reused==1,"Leaf edit recompiled unchanged Host dispatch.");
    auto current=take(language::Instance::create(second->find("Host"),0,keys,{},initial));
    const auto has_text=[](const language::Instance& instance,std::wstring_view text) {
        return std::ranges::any_of(instance.controls(),[&](const auto& control){return control.text==text;});
    };
    require(has_text(*current,L"Version 2") && has_text(*old,L"Version 1"),"Reused parent retained stale child links or changed old leased code.");
    const std::array<language::InputValue,1> flipped{7.0};
    take(old->update(flipped));
    require(has_text(*old,L"Version 1") && std::ranges::any_of(old->controls(),[](const auto& control){return control.text==L"Version 1" && control.layout.height.value==8;}),
        "Old instance lost its executable lease after replacement.");

    const std::string mapped="ui Meter(in double size) {\n Text label { height: input.size; }\n}";
    auto meter=take(language::Module::compile(runtime,"old.ui.source",mapped));
    auto relocated=take(language::Module::compile(runtime,"new.ui.source","// relocation\n\n"+mapped,meter.get()));
    require(relocated->compilation().compiled==0 && relocated->compilation().reused==1,"Source-only relocation recompiled dispatch.");
    const std::array<language::InputValue,1> invalid{40000.0};
    auto failed=language::Instance::create(relocated->find("Meter"),0,keys,{},invalid);
    require(!failed,"Reused dispatch skipped runtime property validation.");
    const auto& declaration=relocated->find("Meter")->declaration();
    const auto mapping=std::ranges::find_if(declaration.mappings,[](const auto& value){return value.origin.line==4;});
    require(mapping!=declaration.mappings.end(),"Relocated dispatch has no current property mapping.");
    std::size_t line=1,column=1;
    for (std::size_t i=0;i<mapping->begin;++i) {
        if (declaration.generated_source[i]=='\n') {++line;column=1;} else ++column;
    }
    require(declaration.diagnostic("old.ui.source[request]:"+std::to_string(line)+":"+std::to_string(column)+": mapped failure").starts_with("new.ui.source:4:"),
        "Reused dispatch retained stale diagnostic mapping.");

    const auto helpers=[](unsigned value) {
        return "double measure() { return "+std::to_string(value)+".0d; }\n"
            "ui A() { Text a { height: measure(); } }\nui B() { Text b { height: measure(); } }";
    };
    auto helper_old=take(language::Module::compile(runtime,"helper.ui.source",helpers(1)));
    auto helper_new=take(language::Module::compile(runtime,"helper.ui.source",helpers(2),helper_old.get()));
    require(helper_new->compilation().compiled==2 && helper_new->compilation().reused==0,"Changed helper body reused stale generated dispatch.");
    Runtime other;take(ui_language::register_types(other));
    auto foreign=take(language::Module::compile(other,"reuse.ui.source",reload_source(2),second.get()));
    require(foreign->compilation().compiled==2 && foreign->compilation().reused==0,"Compiled UI code crossed Runtime ownership.");
    auto acyclic=take(language::Module::compile(runtime,"cycle.ui.source","ui A() { B b {} } ui B() { Text b {} }"));
    require(!language::Module::compile(runtime,"cycle.ui.source","ui A() { B b {} } ui B() { A a {} }",acyclic.get()),
        "Dispatch reuse bypassed composition cycle validation.");
    std::printf("UI dispatch reuse acceptance: PASS (leaf/parent, old lease, source mapping, helper, Runtime, cycle)\n");
}

struct UiReloadResources {std::size_t private_bytes{},executable_bytes{};};
UiReloadResources ui_reload_resources() {
    UiReloadResources result;
    using MemoryInfo=BOOL (WINAPI*)(HANDLE,PPROCESS_MEMORY_COUNTERS,DWORD);
    const auto query=reinterpret_cast<MemoryInfo>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"K32GetProcessMemoryInfo"));
    PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);
    require(query && query(GetCurrentProcess(),reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&memory),sizeof(memory)),"Cannot sample UI process private bytes.");
    result.private_bytes=memory.PrivateUsage;
    std::uintptr_t address{};MEMORY_BASIC_INFORMATION region{};
    while (VirtualQuery(reinterpret_cast<const void*>(address),&region,sizeof(region))) {
        const auto protection=region.Protect & 0xff;
        if (region.State==MEM_COMMIT && (protection==PAGE_EXECUTE || protection==PAGE_EXECUTE_READ ||
            protection==PAGE_EXECUTE_READWRITE || protection==PAGE_EXECUTE_WRITECOPY)) result.executable_bytes+=region.RegionSize;
        const auto next=reinterpret_cast<std::uintptr_t>(region.BaseAddress)+region.RegionSize;
        if (next<=address) break;
        address=next;
    }
    return result;
}
// Each replacement round is one frame of a profile Recorder; compile and publish are its phases.
constinit timing::Marker reload_compile{"profile.compile","UI compile",&timing::frame};
constinit timing::Marker reload_publish{"profile.publish","UI publish",&timing::frame};
constexpr std::array<timing::Marker*,2> reload_markers{&reload_compile,&reload_publish};

void profile_ui_reload(const std::filesystem::path& path) {
    namespace language=ui_language;
    timing::install({reload_markers});
    timing::Recorder recorder;
    std::ofstream output(path,std::ios::binary);
    require(static_cast<bool>(output),"Cannot create UI reload profile CSV.");
    output<<"round,phase,compile_ms,publish_ms,compiled,reused,scope_creations,scope_retirements,roots,live_objects,private_bytes,executable_committed_bytes\n";
    Runtime runtime;take(ui_language::register_types(runtime));
    const auto baseline_roots=runtime.root_count(),baseline_live=runtime.live_objects();
    const std::array fonts{(editor::asset_directory()/"fonts/SarasaUiSC-Regular.ttf").wstring()};
    ui::Session surface(fonts);
    require(surface.set_surface({{640,480},1,1}).has_value(),"UI profile surface failed.");
    ui::ControlKey keys=40000;
    std::shared_ptr<language::Module> active;
    std::unique_ptr<language::Instance> instance;
    ObjectId identity;
    const std::array<language::InputValue,1> inputs{0.0};
    const auto sample=[&](unsigned round,const char* phase,double elapsed,double publish,language::Module::Compilation counts,language::Statistics scopes) {
        const auto memory=ui_reload_resources();
        output<<round<<','<<phase<<','<<elapsed<<','<<publish<<','<<counts.compiled<<','<<counts.reused<<','
            <<scopes.scope_creations<<','<<scopes.scope_retirements<<','<<runtime.root_count()<<','
            <<runtime.live_objects()<<','<<memory.private_bytes<<','<<memory.executable_bytes<<'\n';
    };
    sample(0,"baseline",0,0,{},{});
    for (unsigned round=0;round<120;++round) {
        timing::Recorder::Frame timed{recorder};
        auto candidate=[&]{timing::Scope compiling{reload_compile};
            return take(language::Module::compile(runtime,"profile.ui.source",reload_source(round),active.get()));}();
        std::optional<timing::Scope> publishing{std::in_place,reload_publish};
        if (instance) take(instance->update(inputs,candidate->find("Host")));
        else {instance=take(language::Instance::create(candidate->find("Host"),0,keys,{},inputs));identity=instance->statistics().instance;}
        take(instance->update()); // An idempotent retry must keep the source candidate.
        auto reply=surface.set_controls(instance->controls());
        require(reply.has_value() && !reply->error && reply->effects.empty(),"UI profile replacement descriptors failed.");
        instance->commit_controls();active=std::move(candidate);
        publishing.reset();
        const auto& record=timed.finish();
        require(record.structure_errors==0,"UI reload profile recorded a misplaced phase.");
        const auto elapsed=std::chrono::duration<double,std::milli>(record.time(reload_compile)).count();
        const auto publish=std::chrono::duration<double,std::milli>(record.time(reload_publish)).count();
        auto frame=surface.prepare();require(frame.has_value() && surface.accept(*frame),"UI profile replacement frame failed.");
        require(runtime.root_count()==baseline_roots+2 && runtime.live_objects()==baseline_live+2,"UI replacement retained old scopes.");
        const auto scopes=instance->statistics();
        require(scopes.instance==identity && scopes.scope_creations==2 && !scopes.scope_retirements,
            "Source patch rebuilt compatible Runtime scopes.");
        sample(round,active->compilation().reused?"replace":"cold",elapsed,publish,active->compilation(),scopes);
        require(!language::Module::compile(runtime,"profile.ui.source",reload_source(round)+"\nui Broken() { Text bad { height: missing(); } }",active.get()),
            "UI profile invalid candidate unexpectedly compiled.");
        require(runtime.root_count()==baseline_roots+2 && runtime.live_objects()==baseline_live+2,"Failed UI candidate retained Runtime scopes.");
        sample(round,"failure",0,0,{},instance->statistics());
        if (round%30==29) {
            auto removed=surface.set_controls({});require(removed.has_value() && !removed->error && removed->effects.empty(),"UI profile unmount failed.");
            instance->close();const auto closed=instance->statistics();instance.reset();active.reset();collect(runtime);
            require(runtime.root_count()==baseline_roots && runtime.live_objects()==baseline_live,"Closed UI retained roots or live objects.");
            require(closed.scope_creations==2 && closed.scope_retirements==2,"UI close did not retire exactly the mounted scopes.");
            sample(round,"closed",0,0,{},closed);
        }
    }
    output.flush();require(static_cast<bool>(output),"Cannot finish UI reload profile CSV.");
    std::printf("UI reload profile: 120 real JIT/Instance/Session rounds; Runtime root/live counts validated; process memory samples saved.\n");
}
