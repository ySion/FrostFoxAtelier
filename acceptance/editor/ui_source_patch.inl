// Real authoring file, JIT, Runtime scopes and Session. Source changes must keep
// compatible objects alive, not merely recreate objects with matching UI keys.
void verify_ui_source_patch() {
    namespace language=ui_language;
    Runtime runtime;take(ui_language::register_types(runtime));
    const auto roots=runtime.root_count(),objects=runtime.live_objects();
    const auto folder=std::filesystem::temp_directory_path()/
        ("atelier-ui-patch-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(folder);
    struct Cleanup {std::filesystem::path path;~Cleanup(){std::error_code error;std::filesystem::remove_all(path,error);}} cleanup{folder};
    const auto path=folder/"patch.ui.source";
    std::string source=R"(
ui Child(in Text title) {
    state bool expanded = true;
    state double size = 24.0d;
    Column root {
        Foldout toggle {
            text: input.title; selected: state.expanded;
            on activate { state.expanded = event.checked; }
        }
        Text caption { text: "Version one"; height: state.size; width: 120; }
    }
}
ui Other(in Text title) { Foldout toggle { text: input.title; } }
ui Board(in TextEditBinding edit) {
    Column root {
        Child first { title: "First"; }
        Child second { title: "Second"; }
        TextField field { edit: input.edit; }
    }
}
ui Unrelated() { Text label { text: "Unrelated"; height: 23; } }
ui Late() { Text label { text: "Late"; height: 17; } }
)";
    const auto replace=[](std::string& text,std::string_view before,std::string_view after) {
        const auto at=text.find(before);require(at!=text.npos,"Patch source marker missing.");text.replace(at,before.size(),after);
    };
    take(editor::write_file(path,source));
    ui::ControlKey keys=50000;
    const std::array fonts{(editor::asset_directory()/"fonts/SarasaUiSC-Regular.ttf").wstring()};
    ui::Session surface(fonts);require(surface.set_surface({{640,480},1,1}).has_value(),"Patch surface failed.");
    {
        language::LiveDocument live(runtime,path,keys);
        auto now=language::LiveDocument::Clock::now();
        const std::array<language::InputValue,1> inputs{language::TextEditBinding{7001,L"Original"}};
        std::vector<ui::Control> controls;
        std::vector<ui::Action> actions;
        const auto route=[&](const ui::Reply& reply) {
            require(!reply.error,"Patch input failed.");
            for (const auto& effect:reply.effects) if (const auto* action=std::get_if<ui::Action>(&effect)) {
                take(live.event(*action));actions.push_back(*action);
            }
        };
        const auto append=[&] {
            controls.clear();ui::Control host;host.key=1;host.kind=ui::ControlKind::container;
            host.layout=ui_layout::NodeInput::column();host.layout.width=ui_layout::Axis::expand();controls.push_back(host);
            take(live.append("Board",1,{},inputs,controls));take(live.append("Unrelated",1,{},{},controls));
        };
        const auto draw=[&] {
            append();auto reply=surface.set_controls(controls);
            if (!reply && live.reject_controls("Refused patch topology")) {append();reply=surface.set_controls(controls);}
            require(reply.has_value(),"Patch snapshot failed.");route(*reply);live.commit_controls();live.commit_controls();
            auto frame=surface.prepare();require(frame.has_value() && surface.accept(*frame),"Patch frame failed.");
        };
        const auto stage=[&](const std::string& text) {
            take(editor::write_file(path,text));now+=std::chrono::milliseconds(350);live.poll(now);
            now+=std::chrono::milliseconds(250);live.poll(now);
        };
        const auto find=[&](std::wstring_view text) {
            const auto at=std::ranges::find_if(controls,[&](const auto& control){return control.text==text;});
            require(at!=controls.end(),"Patch control missing.");return *at;
        };
        const auto click=[&](ui::ControlKey key) {
            const auto* target=surface.presented()->find(key);require(target,"Patch hit target missing.");const auto box=target->bounds;
            route(surface.input({.kind=ui::EventKind::pointer_down,.x=box.x+8,.y=box.y+8}));
            route(surface.input({.kind=ui::EventKind::pointer_up,.x=box.x+8,.y=box.y+8}));
        };
        live.poll(now);draw();
        const auto first=find(L"First").key,second=find(L"Second").key;
        const auto identity=live.statistics("Board")->instance;
        const auto unrelated=*live.statistics("Unrelated");
        const auto incarnation=surface.presented()->find(first)->incarnation;
        const auto unchanged=[&](std::uint64_t created,std::uint64_t retired) {
            const auto board=*live.statistics("Board"),other=*live.statistics("Unrelated");
            require(board.instance==identity && board.scope_creations==created && board.scope_retirements==retired,
                "Patch recreated a retained scope or retired one at the wrong stage.");
            require(other.instance==unrelated.instance && other.scope_creations==1 && !other.scope_retirements &&
                other.binding_evaluations==unrelated.binding_evaluations,"Patch rebuilt or reevaluated the unrelated region.");
            require(find(L"First").key==first && surface.presented()->find(first)->incarnation==incarnation,
                "Patch changed the retained control incarnation.");
        };
        click(first);draw();require(!find(L"First").selected,"Patch local state did not change.");
        click(7001);route(surface.input({.kind=ui::EventKind::key_down,.key=ui::Key::a,.control=true}));
        route(surface.input({.kind=ui::EventKind::text,.data=std::wstring{L"Retained draft"}}));draw();
        const auto token=surface.text_input()->token;
        const auto evaluated=live.statistics("Board")->binding_evaluations;
        stage(source+"\n// Only source positions change.\n");draw();unchanged(3,0);
        require(live.statistics("Board")->binding_evaluations==evaluated && surface.text_input()->token==token,
            "Comment-only save evaluated bindings or replaced the text session.");

        replace(source,"Version one","Version two");stage(source);draw();unchanged(3,0);
        require(!find(L"First").selected && find(L"Second").key==second && surface.text_input()->token==token,
            "Child code edit lost state or text input.");
        static_cast<void>(find(L"Version two"));
        source.insert(source.find("        TextField field"),"        Child third { title: \"Third\"; }\n");
        stage(source);draw();unchanged(4,0);static_cast<void>(find(L"Third"));
        const std::string third="        Child third { title: \"Third\"; }\n";
        source.erase(source.find(third),third.size());stage(source);
        require(live.statistics("Board")->scope_retirements==0,"Removed child retired before Session cleanup.");
        draw();unchanged(4,1);
        replace(source,"Child second","Other second");stage(source);draw();unchanged(5,2);
        require(find(L"Second").key!=second,"Changed component type retained its old scope's controls.");

        // Reorder states, add a state, change an existing state's type, and drop
        // a property: only compatible state values survive; defaults are fresh.
        const std::string states="state bool expanded = true;\n    state double size = 24.0d;";
        replace(source,states,
            "state double size = 90.0d;\n    state double extra = 9.0d;\n    state bool expanded = true;");
        replace(source," width: 120;","");stage(source);draw();unchanged(5,2);
        require(!find(L"First").selected && find(L"Version two").layout.height.value==24 &&
            find(L"Version two").layout.width==ui_layout::Axis::expand(),"State migration or removed property defaults are wrong.");
        replace(source,"state double size = 90.0d;","state bool size = false;");
        replace(source,"height: state.size;","height: state.extra;");stage(source);draw();unchanged(5,2);
        require(find(L"Version two").layout.height.value==9,"New state initializer or slot remapping failed.");
        replace(source,"state.expanded = event.checked;","state.expanded = true;");
        stage(source);draw();unchanged(5,2);
        route(surface.input({.kind=ui::EventKind::key_down,.key=ui::Key::enter}));
        require(std::ranges::any_of(actions,[](const auto& action){return action.kind==ui::ActionKind::edit_commit &&
            std::get<std::wstring>(action.value)==L"Retained draft";}),"Patching discarded the uncommitted draft.");
        click(first);draw();click(first);draw();
        require(find(L"First").selected,"Patched scope executed the old event handler.");

        replace(source,"state.expanded = true;","state.expanded = event.checked;");stage(source);draw();
        const auto revision=live.revision();
        // The input was accepted before the file poll in this frame. A refused
        // source candidate must not undo that legitimate user operation.
        click(first);
        auto invalid=source;replace(invalid,"ui Unrelated()","ui Missing()");
        stage(invalid);draw();unchanged(5,2);
        require(live.revision()==revision && !live.diagnostic().empty() && !find(L"First").selected,
            "Late region rejection lost the old event or committed half a source revision.");
        invalid=source;invalid.insert(invalid.find("        TextField field"),
            "        Tree tree { TreeItem early { tree_parent: node.later; } TreeItem later {} }\n");
        for (unsigned i=0;i<3;++i) {
            stage(invalid+"\n// refusal "+std::to_string(i));draw();unchanged(5,2);
            require(live.revision()==revision && live.diagnostic()=="Refused patch topology" && !find(L"First").selected,
                "Session refusal corrupted the accepted source revision or state.");
        }
        stage(source+"\n// recovered\n");draw();unchanged(5,2);
        require(live.revision()==revision+1 && live.diagnostic().empty(),"Source patch did not recover after refusal.");
        invalid=source;replace(invalid,"height: 17;","height: -1;");stage(invalid);append();
        require(!live.append("Late",1,{},{},controls) && live.candidate_failed() &&
            live.reject_controls("Refused first mount in source candidate"),"Invalid source first mount was not rejected.");
        append();take(live.append("Late",1,{},{},controls));
        auto recovered=surface.set_controls(controls);require(recovered.has_value(),"Old source first mount failed.");route(*recovered);live.commit_controls();
        require(live.statistics("Late").has_value(),"Candidate-only failure poisoned the accepted source's input cache.");
        auto removed=surface.set_controls({});require(removed.has_value(),"Patch unmount failed.");route(*removed);
    }
    collect(runtime);require(runtime.root_count()==roots && runtime.live_objects()==objects,"Patch close retained Runtime scopes.");
    std::puts("UI source patch acceptance: PASS (stable contexts, unrelated region, child add/remove/type, state schema, new events, atomic refusal, draft, close)");
}
