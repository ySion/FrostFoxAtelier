#include "application/application.hpp"
#include <cstdio>
#include <string_view>

int verify_editor_window(atelier::application::Application&, const char* output_directory);
int verify_editor_ime(atelier::application::Application&, const char* output_directory);

int main(int argc, char** argv) {
    try {
        const bool acceptance = argc >= 2 && std::string_view(argv[1]) == "--acceptance";
        const bool ime = argc >= 2 && std::string_view(argv[1]) == "--acceptance-ime";
        if (argc != 1 && !((acceptance || ime) && argc == 3)) {
            std::fprintf(stderr, "Usage: atelier_editor [--acceptance|--acceptance-ime output-directory]\n"); return 2;
        }
        atelier::application::Application app(acceptance || ime);
        if (acceptance) return verify_editor_window(app, argv[2]);
        if (ime) return verify_editor_ime(app, argv[2]);
        while (app.step()) {}
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Editor: FAIL: %s\n", error.what()); return 1;
    }
}
