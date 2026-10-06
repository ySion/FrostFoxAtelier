#include "application/project_runner.hpp"
#include "application/window.hpp"
#include "application/presentation.hpp"
#include "editor/project_code.hpp"
#include "editor/project_ui.hpp"
#include "editor/camera.hpp"
#include "renderer/renderer.hpp"
#include <algorithm>
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
}
int run_project_command(std::span<wchar_t*> arguments) {
    try {
        if(arguments.empty()) throw std::runtime_error("--run-project requires a project folder.");
        RunOptions options;options.project=arguments.front();
        for(std::size_t i=1;i<arguments.size();++i) {
            const std::wstring_view option=arguments[i];
            if(option==L"--headless") {options.headless=true;continue;}
            if(option==L"--preview-ui") {options.preview_ui=true;continue;}
            if(++i==arguments.size()) throw std::runtime_error("Missing run option value.");
            if(option==L"--scene-snapshot") options.scene_snapshot=arguments[i];
            else if(option==L"--report") options.report=arguments[i];
            else if(option==L"--frames") options.frames=number(arguments[i]);
            else if(option==L"--stop-event") options.stop_event=reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(number(arguments[i])));
            else throw std::runtime_error("Unknown project run option.");
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
            renderer.emplace(graphics(renderer::Renderer::open(window->handle())));window->show();
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
        editor::Camera camera;std::uint64_t frame{},surface_revision{1},ui_revision{};double elapsed{};bool started{};
        auto previous=std::chrono::steady_clock::now();auto last_report=loaded.build;std::string ui_error;
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
            if(prepared && !started) {
                if(!options.preview_ui) complete(document.start());
                started=true;
                std::printf("Project started: %s\n",editor::utf8(project.root().wstring()).c_str());std::fflush(stdout);
            }
            if(started && !options.preview_ui) take(document.update(dt));
            take(document.collect_step());
            if(project_ui.revision()!=ui_revision) {
                ui_revision=project_ui.revision();std::printf("Project UI published: %llu\n",static_cast<unsigned long long>(ui_revision));std::fflush(stdout);
            }
            if(project_ui.diagnostic()!=ui_error) {ui_error=project_ui.diagnostic();if(!ui_error.empty()) {std::printf("Project UI: %s\n",ui_error.c_str());std::fflush(stdout);}}
            if(prepared) {
                if(renderer) {
                    const auto items=take(document.world().render_items());std::vector<renderer::MeshInstance> meshes;meshes.reserve(items.size());
                    for(const auto& item:items) meshes.push_back({item.world.elements,{item.material.base_color.x,item.material.base_color.y,item.material.base_color.z,1},false,item.geometry});
                    const auto matrices=camera.matrices(static_cast<float>(extent.width)/std::max(extent.height,1u));
                    const renderer::Frame rendered{*prepared->drawing,{matrices.view.elements,matrices.projection.elements},
                        options.preview_ui?ui::Rect{}:ui::Rect{0,0,static_cast<float>(extent.width),static_cast<float>(extent.height)},meshes,
                        {.shading=renderer::Shading::matcap,.background=renderer::Background::gradient,.reference_grid=true,.origin_axes=true}};
                    const auto result=graphics(renderer->render(rendered));
                    if(result.status==renderer::FrameStatus::accepted && !ui.accept(prepared)) throw std::runtime_error("Project UI refused the rendered frame.");
                } else if(!ui.accept(prepared)) throw std::runtime_error("Project UI refused the headless frame.");
            }
            if(window) static_cast<void>(window->text_input(ui.text_input()));
            ++frame;
            if(!renderer && !options.frames) Sleep(4);
            else if(!target.drawable) Sleep(16);
        }
        complete(document.stop());
        if(!options.report.empty()) take(editor::write_file(options.report,take(document.serialize())));
        std::printf("Project stopped: %llu frames\n",static_cast<unsigned long long>(frame));std::fflush(stdout);
        return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"Project run: %s\n",error.what());std::fflush(stderr);return 1;}
}
}
