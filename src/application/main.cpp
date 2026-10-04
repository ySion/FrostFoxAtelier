#include "application/application.hpp"
#include "atelier/build_info.hpp"
#include <cstdio>
#include <string_view>

int verify_editor_window(atelier::application::Application&, const char* output_directory);
int verify_editor_ime(atelier::application::Application&, const char* output_directory);

int wmain(int argc, wchar_t** argv) {
    try {
        std::printf("FrostFox Atelier | %s | %s\n",atelier::build_info::revision,atelier::build_info::configuration);
        if (argc==2 && std::wstring_view(argv[1])==L"--version") return 0;
        const bool acceptance = argc >= 2 && std::wstring_view(argv[1]) == L"--acceptance";
        const bool ime = argc >= 2 && std::wstring_view(argv[1]) == L"--acceptance-ime";
        const bool project=argc==3 && std::wstring_view(argv[1])==L"--project";
        const bool create=argc==3 && std::wstring_view(argv[1])==L"--new-project";
        if (argc != 1 && !((acceptance || ime || project || create) && argc == 3)) {
            std::fprintf(stderr, "Usage: atelier_editor [--version] [--project|--new-project folder] [--acceptance|--acceptance-ime output-directory]\n"); return 2;
        }
        atelier::application::Application app(acceptance || ime);
        if (project || create) {
            const auto opened=app.open_project(argv[2],create);
            if (!opened) throw std::runtime_error(opened.error().message);
        }
        if (acceptance) return verify_editor_window(app, atelier::editor::utf8(argv[2]).c_str());
        if (ime) return verify_editor_ime(app, atelier::editor::utf8(argv[2]).c_str());
        while (app.step()) {}
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Editor: FAIL: %s\n", error.what()); return 1;
    }
}
