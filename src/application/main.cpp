#include "application/application.hpp"
#include <cstdio>
#include <string_view>

int verify_editor_window(atelier::application::Application&, const char* output_directory);

int main(int argc, char** argv) {
    try {
        const bool acceptance = argc >= 2 && std::string_view(argv[1]) == "--acceptance";
        if (argc != 1 && !(acceptance && argc == 3)) {
            std::fprintf(stderr, "Usage: atelier_editor [--acceptance output-directory]\n"); return 2;
        }
        atelier::application::Application app(acceptance);
        if (acceptance) return verify_editor_window(app, argv[2]);
        while (app.step()) {}
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Editor: FAIL: %s\n", error.what()); return 1;
    }
}
