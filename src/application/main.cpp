#include "application/application.hpp"
#include "application/project_runner.hpp"
#include "control/control.hpp"
#include "atelier/build_info.hpp"
#include <cstdio>
#include <string_view>
#include <array>
#include <cmath>
#include <chrono>

int verify_sponza_window(atelier::application::Application&, const char* output_directory);
int verify_physics_examples_window(atelier::application::Application&, const char* output_directory);
int verify_dungeons_window(atelier::application::Application&, const char* output_directory);
int verify_examples_window(atelier::application::Application&, const char* output_directory,bool snapshot_only);

int verify_editor_window(atelier::application::Application&, const char* output_directory, bool viewport_only = false);
int verify_editor_source(atelier::application::Application&, const char* output_directory);
int verify_editor_edit_mode(atelier::application::Application&, const char* output_directory);
int verify_editor_shortcuts(atelier::application::Application&, const char* output_directory);
int verify_editor_surfaces(atelier::application::Application&, const char* output_directory);
int verify_editor_render_performance(atelier::application::Application&, const char* output_directory);
int verify_editor_tooltips(atelier::application::Application&, const char* output_directory);
int verify_editor_dialogs(atelier::application::Application&, const char* output_directory);
int capture_editor_snapshot(atelier::application::Application&, const char* output_directory);
int verify_editor_ime(atelier::application::Application&, const char* output_directory);
int verify_editor_project(atelier::application::Application&, const char* output_directory,const wchar_t* source_project);
int verify_editor_scene_lit(atelier::application::Application&, const char* output_directory,const wchar_t* source_project);

int wmain(int argc, wchar_t** argv) {
    try {
        std::printf("FrostFox Atelier | %s | %s\n",atelier::build_info::revision,atelier::build_info::configuration);
        if (argc==2 && std::wstring_view(argv[1])==L"--version") return 0;
        if (argc>=3 && std::wstring_view(argv[1])==L"--run-project")
            return atelier::application::run_project_command({argv+2,static_cast<std::size_t>(argc-2)});
        auto control_directory=atelier::control::default_directory();bool no_control{},no_activate{},gpu_validation{};
        std::optional<atelier::ui::PixelExtent> initial_client;
        std::optional<std::filesystem::path> render_performance_output;
        const auto dimension=[](std::wstring_view value) {
            std::uint32_t pixels{};
            for(const auto c:value) {
                if(c<L'0' || c>L'9' || pixels>(32768u-static_cast<unsigned>(c-L'0'))/10u)
                    throw std::runtime_error("--client-size requires two integers in 1..32768 physical pixels.");
                pixels=pixels*10+static_cast<unsigned>(c-L'0');
            }
            if(!pixels) throw std::runtime_error("--client-size requires two integers in 1..32768 physical pixels.");
            return pixels;
        };
        std::vector<wchar_t*> arguments{argv[0]};
        for(int i=1;i<argc;++i) {
            const std::wstring_view option(argv[i]);
            if(option==L"--no-control") no_control=true;
            else if(option==L"--no-activate") no_activate=true;
            else if(option==L"--gpu-validation") gpu_validation=true;
            else if(option==L"--acceptance-render-performance") {
                if(render_performance_output || ++i==argc)
                    throw std::runtime_error("--acceptance-render-performance requires one absolute output directory.");
                render_performance_output=std::filesystem::path(argv[i]);
                if(!render_performance_output->is_absolute())
                    throw std::runtime_error("Render performance output directory must be absolute.");
            }
            else if(option==L"--client-size") {
                if(initial_client) throw std::runtime_error("--client-size can only be specified once.");
                if(i+2>=argc) throw std::runtime_error("--client-size requires width and height in physical pixels.");
                initial_client=atelier::ui::PixelExtent{dimension(argv[i+1]),dimension(argv[i+2])};i+=2;
            }
            else if(option==L"--control-directory") {if(++i==argc) throw std::runtime_error("Missing control directory");control_directory=argv[i];}
            else arguments.push_back(argv[i]);
        }
        argc=static_cast<int>(arguments.size());argv=arguments.data();
        const bool sponza=argc==3 && std::wstring_view(argv[1])==L"--acceptance-sponza";
        const bool physics_examples=argc==3 && std::wstring_view(argv[1])==L"--acceptance-physics-examples";
        const bool physic=argc==2 && std::wstring_view(argv[1])==L"--physic";
        const bool dungeons=argc==2 && std::wstring_view(argv[1])==L"--dungeons";
        const bool dungeons_acceptance=argc==3 && std::wstring_view(argv[1])==L"--acceptance-dungeons";
        const bool examples_snapshot=argc==3 && std::wstring_view(argv[1])==L"--snapshot-examples";
        const bool examples=examples_snapshot || (argc==3 && std::wstring_view(argv[1])==L"--acceptance-examples");
        const bool preview=argc==12 && std::wstring_view(argv[1])==L"--project" &&
            std::wstring_view(argv[3])==L"--scene" &&
            (std::wstring_view(argv[5])==L"--material-camera" || std::wstring_view(argv[5])==L"--scene-lit-camera");
        std::array<float,6> camera{};
        if(preview) for(unsigned i=0;i<6;++i) {
            wchar_t* end{};camera[i]=std::wcstof(argv[i+6],&end);
            if(end==argv[i+6] || *end || !std::isfinite(camera[i])) throw std::runtime_error("Camera values must be finite numbers.");
        }
        const bool acceptance = argc >= 2 && std::wstring_view(argv[1]) == L"--acceptance";
        const bool source=argc>=2 && std::wstring_view(argv[1])==L"--acceptance-source";
        const bool edit=argc>=2 && std::wstring_view(argv[1])==L"--acceptance-edit";
        const bool shortcuts=argc>=2 && std::wstring_view(argv[1])==L"--acceptance-shortcuts";
        const bool surfaces=argc>=2 && std::wstring_view(argv[1])==L"--acceptance-surfaces";
        const bool tooltips=argc>=2 && std::wstring_view(argv[1])==L"--acceptance-tooltips";
        const bool dialogs=argc>=2 && std::wstring_view(argv[1])==L"--acceptance-dialogs";
        const bool snapshot=argc>=2 && std::wstring_view(argv[1])==L"--snapshot";
        const bool ime = argc >= 2 && std::wstring_view(argv[1]) == L"--acceptance-ime";
        const bool project_acceptance=argc>=2 && std::wstring_view(argv[1])==L"--acceptance-project";
        const bool viewport = argc >= 2 && std::wstring_view(argv[1]) == L"--acceptance-viewport";
        const bool scene_lit = argc >= 2 && std::wstring_view(argv[1]) == L"--acceptance-scene-lit";
        const bool project=(argc==3 || preview) && std::wstring_view(argv[1])==L"--project";
        const bool create=argc==3 && std::wstring_view(argv[1])==L"--new-project";
        if(render_performance_output && !project)
            throw std::runtime_error("--acceptance-render-performance requires --project, optionally followed by the existing --scene and camera arguments.");
        if (argc != 1 && !dungeons && !dungeons_acceptance && !physic && !physics_examples && !examples && !sponza && !preview && !((acceptance || source || edit || shortcuts || surfaces || tooltips || dialogs || snapshot || ime || viewport || project || create) && argc == 3) &&
            !((project_acceptance || scene_lit) && (argc==3 || argc==4))) {
            std::fprintf(stderr, "Usage: atelier_editor [--acceptance-render-performance absolute-output-directory] [--no-activate] [--client-size width height (physical pixels)] [--version|--physic|--dungeons] [--project|--new-project folder] [--scene relative.scene --material-camera|--scene-lit-camera eye-x eye-y eye-z target-x target-y target-z] [--snapshot|--acceptance|--acceptance-ime|--acceptance-viewport|--acceptance-source|--acceptance-edit|--acceptance-shortcuts|--acceptance-surfaces|--acceptance-tooltips|--acceptance-dialogs|--acceptance-sponza|--acceptance-physics-examples|--acceptance-dungeons|--acceptance-examples|--snapshot-examples output-directory] [--acceptance-project|--acceptance-scene-lit output-directory [source-project]]\n"); return 2;
        }
        auto example_workspace=examples||physics_examples||dungeons_acceptance?std::filesystem::absolute(argv[2])/"Workspace":std::filesystem::path{};
        // Each dedicated example run must consume current templates. Product workspaces
        // intentionally preserve existing folders and must not be refreshed here.
        if(physics_examples||dungeons_acceptance) example_workspace/=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        atelier::application::Application app(gpu_validation || acceptance || source || edit || ime || viewport || project_acceptance || scene_lit || sponza || physics_examples || (examples && !examples_snapshot),!snapshot && !shortcuts && !surfaces && !tooltips && !dialogs && !no_activate && !examples_snapshot && !render_performance_output,
            example_workspace,initial_client,!render_performance_output);
        if(argc==1 || examples || physic || dungeons) {
            const auto opened=app.open_example(dungeons?atelier::editor::Command::example_dungeons:physic?atelier::editor::Command::example_physic:atelier::editor::Command::example_sponza);
            if(!opened) throw std::runtime_error(opened.error().message);
        }
        if (project || create) {
            const auto opened=app.open_project(argv[2],create,render_performance_output?
                atelier::application::ProjectOpenPolicy::require_scene:atelier::application::ProjectOpenPolicy::allow_repair);
            if (!opened) throw std::runtime_error(opened.error().message);
        }
        if(preview) {
            auto opened=app.open_scene(argv[4]);if(!opened) throw std::runtime_error(opened.error().message);
            const bool lit=std::wstring_view(argv[5])==L"--scene-lit-camera";
            auto view=app.configure_view({camera[0],camera[1],camera[2]},{camera[3],camera[4],camera[5]},
                lit?atelier::renderer::Shading::scene_lit:atelier::renderer::Shading::smooth,lit && !render_performance_output);
            if(!view) throw std::runtime_error(view.error().message);
        }
        if(render_performance_output)
            return verify_editor_render_performance(app,atelier::editor::utf8(render_performance_output->wstring()).c_str());
        if(sponza) return verify_sponza_window(app,atelier::editor::utf8(argv[2]).c_str());
        if(physics_examples) return verify_physics_examples_window(app,atelier::editor::utf8(argv[2]).c_str());
        if(dungeons_acceptance) return verify_dungeons_window(app,atelier::editor::utf8(argv[2]).c_str());
        if(examples) return verify_examples_window(app,atelier::editor::utf8(argv[2]).c_str(),examples_snapshot);
        if (source) return verify_editor_source(app,atelier::editor::utf8(argv[2]).c_str());
        if (edit) return verify_editor_edit_mode(app,atelier::editor::utf8(argv[2]).c_str());
        if (shortcuts) return verify_editor_shortcuts(app,atelier::editor::utf8(argv[2]).c_str());
        if (surfaces) return verify_editor_surfaces(app,atelier::editor::utf8(argv[2]).c_str());
        if (tooltips) return verify_editor_tooltips(app,atelier::editor::utf8(argv[2]).c_str());
        if (dialogs) return verify_editor_dialogs(app,atelier::editor::utf8(argv[2]).c_str());
        if (snapshot) return capture_editor_snapshot(app,atelier::editor::utf8(argv[2]).c_str());
        if (acceptance) return verify_editor_window(app, atelier::editor::utf8(argv[2]).c_str());
        if (viewport) return verify_editor_window(app, atelier::editor::utf8(argv[2]).c_str(), true);
        if (ime) return verify_editor_ime(app, atelier::editor::utf8(argv[2]).c_str());
        if (project_acceptance) return verify_editor_project(app,atelier::editor::utf8(argv[2]).c_str(),argc==4?argv[3]:nullptr);
        if (scene_lit) return verify_editor_scene_lit(app,atelier::editor::utf8(argv[2]).c_str(),argc==4?argv[3]:nullptr);
        if(!no_control) app.enable_control(control_directory);
        while (app.step()) {}
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Editor: FAIL: %s\n", error.what()); return 1;
    }
}
