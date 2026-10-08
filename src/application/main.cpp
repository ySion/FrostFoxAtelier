#include "application/application.hpp"
#include "application/project_runner.hpp"
#include "atelier/build_info.hpp"
#include <cstdio>
#include <string_view>

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
        const bool acceptance = argc >= 2 && std::wstring_view(argv[1]) == L"--acceptance";
        const bool source=argc>=2 && std::wstring_view(argv[1])==L"--acceptance-source";
        const bool edit=argc>=2 && std::wstring_view(argv[1])==L"--acceptance-edit";
        const bool snapshot=argc>=2 && std::wstring_view(argv[1])==L"--snapshot";
        const bool ime = argc >= 2 && std::wstring_view(argv[1]) == L"--acceptance-ime";
        const bool project_acceptance=argc>=2 && std::wstring_view(argv[1])==L"--acceptance-project";
        const bool viewport = argc >= 2 && std::wstring_view(argv[1]) == L"--acceptance-viewport";
        const bool scene_lit = argc >= 2 && std::wstring_view(argv[1]) == L"--acceptance-scene-lit";
        const bool project=argc==3 && std::wstring_view(argv[1])==L"--project";
        const bool create=argc==3 && std::wstring_view(argv[1])==L"--new-project";
        if (argc != 1 && !((acceptance || source || edit || snapshot || ime || viewport || project || create) && argc == 3) &&
            !((project_acceptance || scene_lit) && (argc==3 || argc==4))) {
            std::fprintf(stderr, "Usage: atelier_editor [--version] [--project|--new-project folder] [--acceptance|--acceptance-ime|--acceptance-viewport|--acceptance-source|--acceptance-edit output-directory] [--acceptance-project|--acceptance-scene-lit output-directory [source-project]]\n"); return 2;
        }
        atelier::application::Application app(acceptance || source || edit || ime || viewport || project_acceptance || scene_lit,!snapshot);
        if (project || create) {
            const auto opened=app.open_project(argv[2],create);
            if (!opened) throw std::runtime_error(opened.error().message);
        }
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
