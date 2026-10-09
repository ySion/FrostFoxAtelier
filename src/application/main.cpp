#include "application/application.hpp"
#include "application/project_runner.hpp"
#include "atelier/build_info.hpp"
#include <cstdio>
#include <string_view>
#include <array>
#include <cmath>

int verify_sponza_window(atelier::application::Application&, const char* output_directory);
int verify_examples_window(atelier::application::Application&, const char* output_directory,bool snapshot_only);

int verify_editor_window(atelier::application::Application&, const char* output_directory, bool viewport_only = false);
int verify_editor_source(atelier::application::Application&, const char* output_directory);
int verify_editor_edit_mode(atelier::application::Application&, const char* output_directory);
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
        const bool sponza=argc==3 && std::wstring_view(argv[1])==L"--acceptance-sponza";
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
        const bool snapshot=argc>=2 && std::wstring_view(argv[1])==L"--snapshot";
        const bool ime = argc >= 2 && std::wstring_view(argv[1]) == L"--acceptance-ime";
        const bool project_acceptance=argc>=2 && std::wstring_view(argv[1])==L"--acceptance-project";
        const bool viewport = argc >= 2 && std::wstring_view(argv[1]) == L"--acceptance-viewport";
        const bool scene_lit = argc >= 2 && std::wstring_view(argv[1]) == L"--acceptance-scene-lit";
        const bool project=(argc==3 || preview) && std::wstring_view(argv[1])==L"--project";
        const bool create=argc==3 && std::wstring_view(argv[1])==L"--new-project";
        if (argc != 1 && !examples && !sponza && !preview && !((acceptance || source || edit || snapshot || ime || viewport || project || create) && argc == 3) &&
            !((project_acceptance || scene_lit) && (argc==3 || argc==4))) {
            std::fprintf(stderr, "Usage: atelier_editor [--version] [--project|--new-project folder] [--scene relative.scene --material-camera|--scene-lit-camera eye-x eye-y eye-z target-x target-y target-z] [--snapshot|--acceptance|--acceptance-ime|--acceptance-viewport|--acceptance-source|--acceptance-edit|--acceptance-sponza|--acceptance-examples|--snapshot-examples output-directory] [--acceptance-project|--acceptance-scene-lit output-directory [source-project]]\n"); return 2;
        }
        atelier::application::Application app(acceptance || source || edit || ime || viewport || project_acceptance || scene_lit || sponza || (examples && !examples_snapshot),!snapshot && !examples_snapshot,
            examples?std::filesystem::absolute(argv[2])/"Workspace":std::filesystem::path{});
        if(argc==1 || examples) {
            const auto opened=app.open_example();
            if(!opened) throw std::runtime_error(opened.error().message);
        }
        if (project || create) {
            const auto opened=app.open_project(argv[2],create);
            if (!opened) throw std::runtime_error(opened.error().message);
        }
        if(preview) {
            auto opened=app.open_scene(argv[4]);if(!opened) throw std::runtime_error(opened.error().message);
            const bool lit=std::wstring_view(argv[5])==L"--scene-lit-camera";
            auto view=app.configure_view({camera[0],camera[1],camera[2]},{camera[3],camera[4],camera[5]},
                lit?atelier::renderer::Shading::scene_lit:atelier::renderer::Shading::smooth,lit);
            if(!view) throw std::runtime_error(view.error().message);
        }
        if(sponza) return verify_sponza_window(app,atelier::editor::utf8(argv[2]).c_str());
        if(examples) return verify_examples_window(app,atelier::editor::utf8(argv[2]).c_str(),examples_snapshot);
        if (source) return verify_editor_source(app,atelier::editor::utf8(argv[2]).c_str());
        if (edit) return verify_editor_edit_mode(app,atelier::editor::utf8(argv[2]).c_str());
        if (snapshot) return capture_editor_snapshot(app,atelier::editor::utf8(argv[2]).c_str());
        if (acceptance) return verify_editor_window(app, atelier::editor::utf8(argv[2]).c_str());
        if (viewport) return verify_editor_window(app, atelier::editor::utf8(argv[2]).c_str(), true);
        if (ime) return verify_editor_ime(app, atelier::editor::utf8(argv[2]).c_str());
        if (project_acceptance) return verify_editor_project(app,atelier::editor::utf8(argv[2]).c_str(),argc==4?argv[3]:nullptr);
        if (scene_lit) return verify_editor_scene_lit(app,atelier::editor::utf8(argv[2]).c_str(),argc==4?argv[3]:nullptr);
        while (app.step()) {}
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Editor: FAIL: %s\n", error.what()); return 1;
    }
}
