#include <cstdio>
#include <exception>
#include <string_view>

void verify_source_shutdown();

void verify_source_session();

void verify_source_inheritance();

void verify_native_value_borrows();

void verify_source_modules();

void verify_source_types();

void verify_source_value_fields();

void verify_source_foundations();

void verify_source_plain_records();

void verify_source_diagnostics();

void verify_source_debugger();

void verify_source_materials();

void verify_source_math();

void verify_source_projects();

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--materials") {
            verify_source_materials();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--projects") {
            verify_source_projects();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--math") {
            verify_source_math();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--foundations") {
            verify_source_foundations();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--diagnostics") {
            verify_source_diagnostics();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--debugger") {
            verify_source_debugger();
            return 0;
        }
        verify_source_debugger();
        verify_source_diagnostics();
        verify_source_projects();
        verify_source_math();
        verify_source_plain_records();
        verify_native_value_borrows();
        verify_source_modules();
        verify_source_types();
        verify_source_value_fields();
        verify_source_foundations();
        verify_source_inheritance();
        verify_source_shutdown();
        verify_source_session();
        std::puts("Source public consumer: PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Source public consumer: FAIL: %s\n", error.what());
        return 1;
    }
}
