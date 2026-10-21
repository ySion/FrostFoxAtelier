#include "application/project_runner.hpp"
#include "application/play_link.hpp"
#include "application/render_clock.hpp"
#include "application/lit_preparation.hpp"
#include "application/window.hpp"
#include "application/presentation.hpp"
#include "application/scene_snapshot.hpp"
#include "editor/project_code.hpp"
#include "editor/project_ui.hpp"
#include "editor/camera.hpp"
#include "renderer/renderer.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <functional>

namespace atelier::application {
namespace {
template<class T> T take(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    if constexpr(!std::is_void_v<T>) return std::move(*result);
}
void complete(Result<world::Commit> result) {
    if (!result) throw std::runtime_error(result.error().message);
    if (result->notification_error) throw std::runtime_error(result->notification_error->message);
}
template<class T> T graphics(renderer::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().cause.message);
    if constexpr(!std::is_void_v<T>) return std::move(*result);
}
template<class T> T presentation(ui::Result<T> result) {
    if (!result) throw std::runtime_error("Project UI failed at control "+std::to_string(result.error().key)+".");
    if constexpr(!std::is_void_v<T>) return std::move(*result);
}
std::uint64_t number(std::wstring_view text) {
    std::uint64_t value{};
    if(text.empty()) throw std::runtime_error("Expected an unsigned command argument.");
    for(const auto ch:text) {
        if(ch<L'0' || ch>L'9' || value>(UINT64_MAX-static_cast<unsigned>(ch-L'0'))/10)
            throw std::runtime_error("Invalid unsigned command argument.");
        value=value*10+static_cast<unsigned>(ch-L'0');
    }
    return value;
}
// "view,target-x,target-y,target-z,yaw,pitch,distance", written by PlayProcess with round-trip floats.
editor::CameraState camera_state(std::wstring_view text) {
    std::string narrow;for(const auto ch:text) {if(ch>0x7f) throw std::runtime_error("Invalid run camera.");narrow+=static_cast<char>(ch);}
    const char* at=narrow.data();const char* const end=at+narrow.size();
    const auto field=[&](auto& value,bool last) {
        const auto [next,error]=std::from_chars(at,end,value);
        if(error!=std::errc{} || (last?next!=end:next==end || *next!=',')) throw std::runtime_error("Invalid run camera.");
        at=last?next:next+1;
    };
    unsigned view{};editor::CameraState state;field(view,false);
    field(state.target.x,false);field(state.target.y,false);field(state.target.z,false);
    field(state.yaw,false);field(state.pitch,false);field(state.distance,true);
    if(view>static_cast<unsigned>(editor::CameraView::left)) throw std::runtime_error("Invalid run camera.");
    state.view=static_cast<editor::CameraView>(view);
    return state;
}
}
int run_project_command(std::span<wchar_t*> arguments) {
    try {
        if(arguments.empty()) throw std::runtime_error("--run-project requires a project folder.");
        RunOptions options;options.project=arguments.front();
        HANDLE live_read{},live_write{};
        for(std::size_t i=1;i<arguments.size();++i) {
            const std::wstring_view option=arguments[i];
            if(option==L"--headless") {options.headless=true;continue;}
            if(option==L"--preview-ui") {options.preview_ui=true;continue;}
            if(option==L"--halt-on-fault") {options.halt=true;continue;}
            if(++i==arguments.size()) throw std::runtime_error("Missing run option value.");
            if(option==L"--live-channel") {
                live_read=reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(number(arguments[i])));
                if(++i==arguments.size()) throw std::runtime_error("Missing run option value.");
                live_write=reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(number(arguments[i])));continue;
            }
            if(option==L"--scene-snapshot") options.scene_snapshot=arguments[i];
            else if(option==L"--report") options.report=arguments[i];
            else if(option==L"--frames") options.frames=number(arguments[i]);
            else if(option==L"--camera") options.camera=camera_state(arguments[i]);
            else if(option==L"--stop-event") options.stop_event=reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(number(arguments[i])));
            else throw std::runtime_error("Unknown project run option.");
        }
        // Inherited from the editor: changes arrive on the first pipe, acknowledgements leave on the second.
        std::optional<PlayEnd> live;
        if(live_read || live_write) {
            DWORD flags{};
            if(!GetHandleInformation(live_read,&flags) || !GetHandleInformation(live_write,&flags)) throw std::runtime_error("Invalid inherited Play change link.");
            live.emplace(std::make_unique<LiveLink>(live_read,live_write));options.channel=&*live;
        }
        return run_project(options);
    }catch(const std::exception& error){std::fprintf(stderr,"Project run: %s\n",error.what());return 2;}
}
int run_project(const RunOptions& options) {
    struct StopEvent {HANDLE value;~StopEvent(){if(value) CloseHandle(value);}} stop_event{options.stop_event};
    try {
        if(options.stop_event) {DWORD flags{};if(!GetHandleInformation(options.stop_event,&flags)) throw std::runtime_error("Invalid inherited stop event.");}
        auto project=take(editor::Project::open(options.project));
        editor::ProjectDocument loaded;
        if(options.preview_ui) {
            if(project.ui_source().empty()) throw std::runtime_error("Configure a project UI source before previewing.");
            loaded.session=std::make_unique<editor::Session>();
        } else {
            const auto scene=options.scene_snapshot.empty()?take(project.resolve(project.startup_scene())):options.scene_snapshot;
            loaded=editor::load_project_scene(project,take(editor::read_file(scene)));
        }
        if(!loaded.session) throw std::runtime_error(loaded.error?loaded.error->message:"Project scene was not loaded.");
        auto& document=*loaded.session;
        if(options.halt) take(document.set_failure_policy(world::FailurePolicy::halt));
        // The editor owns document identities; this copy addresses objects it spawns by runtime handle.
        if(!options.preview_ui) document.enter_play();
        if(const auto summary=editor::describe_issues(document,document.load_issues(),L"Scene");!summary.empty())
            {std::printf("%s\n",editor::utf8(summary.front()).c_str());std::fflush(stdout);}
        // Isolated failures keep Play running; under halt the failing call is reported before it stops the run.
        std::uint64_t dropped{};
        const Report report=[&](std::span<const world::Diagnostic> items) {
            for(const auto& item:items) std::printf("%s\n",editor::utf8(editor::describe_diagnostic(document,item)).c_str());
            std::fflush(stdout);
        };
        const auto report_diagnostics=[&] {
            report(document.take_diagnostics());
            if(const auto lost=document.world().dropped_diagnostics();lost!=dropped)
                {std::printf("Diagnostics: %llu dropped while the log was full.\n",static_cast<unsigned long long>(lost-dropped));dropped=lost;}
            std::fflush(stdout);
        };
        struct StopWorld {
            editor::Session& session;
            ~StopWorld(){auto stopped=session.stop();if(!stopped) std::fprintf(stderr,"Project stop: %s\n",stopped.error().message.c_str());}
        } stop_world{document};
        editor::ProjectUI project_ui;project_ui.sync(&project);
        ui::Session ui(editor_fonts());
        auto style=ui.style();style.font.family=L"Sarasa UI SC";style.font.size=12.5f;style.caption_scale=1;
        presentation(ui.set_style(style));presentation(ui.set_style_sheet(editor_styles()));
        std::unique_ptr<Window> window;std::optional<renderer::Renderer> renderer;
        if(!options.headless) {
            window=std::make_unique<Window>();SetWindowTextW(window->handle(),(project.name()+(options.preview_ui?L" — UI Preview":L" — Running")).c_str());
            renderer::Options render_options;render_options.shader_cache=shader_cache_directory();
            renderer.emplace(graphics(renderer::Renderer::open(window->handle(),render_options)));window->show();
        }
        struct CloseRenderer {std::optional<renderer::Renderer>& renderer;~CloseRenderer(){if(renderer) static_cast<void>(renderer->close());}} close_renderer{renderer};
        std::function<void(const ui::Reply&)> effects;
        effects=[&](const ui::Reply& reply) {
            for(const auto& effect:reply.effects) {
                if(const auto* action=std::get_if<ui::Action>(&effect)) {
                    const auto handled=project_ui.event(*action);if(!handled) std::fprintf(stderr,"Project UI: %s\n",handled.error().message.c_str());
                } else if(window) std::visit([&](const auto& request) {
                    using T=std::decay_t<decltype(request)>;
                    if constexpr(std::same_as<T,ui::ClipboardRequest>) effects(ui.input({.kind=ui::EventKind::clipboard_result,.data=window->clipboard(request)}));
                    else effects(ui.input({.kind=ui::EventKind::ime_result,.data=window->ime(request)}));
                },std::get<ui::PlatformRequest>(effect));
            }
            if(window) {
                if(reply.capture!=ui::Capture::unchanged) window->capture(reply.capture==ui::Capture::acquire);
                if(reply.cursor) window->cursor(*reply.cursor);
            }
        };
        if(project_ui.configured()) {std::printf("Project UI evaluating\n");std::fflush(stdout);}
        SceneSnapshot scene;
        editor::Camera camera;std::uint64_t frame{},surface_revision{1},ui_revision{};double elapsed{};bool started{};
        // Play from the editor opens on the editor's view. A standalone run frames the
        // authored mesh centers once; large collision floors should not force a tiny
        // view of the moving Entities. Navigation stays a host concern.
        const auto initial=take(document.presentation().render_items());
        if(options.camera) {
            if(!camera.set_state(*options.camera)) throw std::runtime_error("Invalid run camera.");
        } else if(!initial.empty()) {
            math::Vector3 low{initial.front().world.elements[12],initial.front().world.elements[13],initial.front().world.elements[14]},high=low;
            for(const auto& item:initial) for(unsigned axis=0;axis<3;++axis) {
                low[axis]=std::min(low[axis],item.world.elements[12+axis]);high[axis]=std::max(high[axis],item.world.elements[12+axis]);
            }
            const auto target=(low+high)*.5f;const auto size=std::max({high.x-low.x,high.y-low.y,high.z-low.z,4.f});
            static_cast<void>(camera.look_at(target+math::Vector3{size*1.3f,size,size*-1.6f},target));
        }
        auto previous=std::chrono::steady_clock::now();auto last_report=loaded.build;std::string ui_error;
        RenderClock lighting_clock;LitFallback lit_fallback;std::string render_failure;
        while(!options.frames || frame<options.frames) {
            if(options.stop_event && WaitForSingleObject(options.stop_event,0)==WAIT_OBJECT_0) break;
            if(window) {
                window->poll([&](NativeEvent message) {
                    if(message.message==WM_SIZE || message.message==WM_DPICHANGED) {++surface_revision;effects(ui.cancel_interaction());return;}
                    if(message.message==WM_CANCELMODE || (message.message==WM_ACTIVATEAPP && !message.wparam)) {effects(ui.cancel_interaction());return;}
                    auto input=input_event(std::move(message),window->handle());if(!input) return;
                    const bool pointer=input->kind==ui::EventKind::pointer_down || input->kind==ui::EventKind::pointer_move || input->kind==ui::EventKind::wheel;
                    if(pointer && (!ui.presented() || ui.presented()->drawing->surface_revision!=surface_revision)) return;
                    effects(ui.input(*input));
                });
                if(window->closing()) break;
            }
            if(loaded.code) static_cast<void>(loaded.code->poll(project));
            if(loaded.code && loaded.code->report()!=last_report) {
                last_report=loaded.code->report();
                if(last_report->error) std::printf("Build: %s\n",last_report->error->message.c_str());
                else std::printf("Build published: %llu\n",static_cast<unsigned long long>(last_report->published_version));
                std::fflush(stdout);
            }
            project_ui.sync(&project);
            const auto target=renderer?graphics(renderer->target()):renderer::Target{{1280,720},true};
            const auto extent=target.drawable?target.extent:ui::PixelExtent{};
            const auto now=std::chrono::steady_clock::now();
            const float dt=options.headless && options.frames?1.f/60:std::clamp(std::chrono::duration<float>(now-previous).count(),0.f,.1f);
            previous=now;if(started || target.drawable) elapsed+=dt;
            auto prepared=take(project_ui.prepare(ui,{extent,window?window->dpi_scale():1.f,surface_revision},elapsed,
                take(document.world().members()).size(),effects));
            if(options.channel && !options.preview_ui) options.channel->service(document,project,report);
            if(prepared && !started) {
                if(!options.preview_ui) {auto begun=document.start();report_diagnostics();complete(std::move(begun));}
                started=true;
                std::printf("Project started: %s\n",editor::utf8(project.root().wstring()).c_str());std::fflush(stdout);
            }
            if(started && !options.preview_ui) {auto updated=document.update(dt);report_diagnostics();take(std::move(updated));}
            take(document.collect_step());
            if(project_ui.revision()!=ui_revision) {
                ui_revision=project_ui.revision();std::printf("Project UI published: %llu\n",static_cast<unsigned long long>(ui_revision));std::fflush(stdout);
            }
            if(project_ui.diagnostic()!=ui_error) {ui_error=project_ui.diagnostic();if(!ui_error.empty()) {std::printf("Project UI: %s\n",ui_error.c_str());std::fflush(stdout);}}
            if(prepared) {
                if(renderer) {
                    {auto refreshed=scene.refresh(document);report_diagnostics();take(std::move(refreshed));}
                    for(auto& mesh:scene.meshes) mesh.selected=false;
                    const auto matrices=camera.matrices(static_cast<float>(extent.width)/std::max(extent.height,1u));
                    renderer::ViewSettings view;view.shading=renderer::Shading::scene_lit;
                    {auto environment=document.presentation().environment();report_diagnostics();static_cast<rendering::SceneLightingSettings&>(view.lit)=take(std::move(environment));}
                    const auto focus=camera.state().target;
                    renderer::Frame rendered{*prepared->drawing,{matrices.view.elements,matrices.projection.elements},
                        options.preview_ui?ui::Rect{}:ui::Rect{0,0,static_cast<float>(extent.width),static_cast<float>(extent.height)},scene.meshes,
                        view,{},scene.lights,scene.identity,false,rendering::sky_radiance(view.lit),{focus.x,focus.y,focus.z}};
                    const auto lighting_start=RenderClock::Clock::now();
                    const bool lighting_active=target.drawable && !options.preview_ui && !scene.meshes.empty() && view.lit.ddgi;
                    rendered.render_delta_seconds=lighting_clock.delta(lighting_start,lighting_active);
                    const auto drawn=graphics(lit_fallback.render(*renderer,rendered,nullptr));
                    if(drawn.failure!=render_failure) {
                        render_failure=drawn.failure;
                        if(!render_failure.empty()) {std::printf("Shader preparation failed: %s\n",render_failure.c_str());std::fflush(stdout);}
                    }
                    const auto& result=drawn.result;
                    if(result.status==renderer::FrameStatus::accepted) {
                        if(result.accepted_id!=rendered.ui_frame.id || result.accepted_extent!=rendered.ui_frame.extent)
                            throw std::runtime_error("Project renderer accepted a mismatched frame.");
                        if(!ui.accept(prepared)) throw std::runtime_error("Project UI refused the rendered frame.");
                        lighting_clock.accept(lighting_start,lighting_active && drawn.view.shading==renderer::Shading::scene_lit);
                    } else lighting_clock.reset();
                } else if(!ui.accept(prepared)) throw std::runtime_error("Project UI refused the headless frame.");
            } else lighting_clock.reset();
            if(window) static_cast<void>(window->text_input(ui.text_input()));
            ++frame;
            if(!renderer && !options.frames) Sleep(4);
            else if(!target.drawable) Sleep(16);
        }
        {auto stopped=document.stop();report_diagnostics();complete(std::move(stopped));}
        if(!options.report.empty()) take(editor::write_file(options.report,take(document.serialize())));
        std::printf("Project stopped: %llu frames\n",static_cast<unsigned long long>(frame));std::fflush(stdout);
        return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"Project run: %s\n",error.what());std::fflush(stderr);return 1;}
}
}
