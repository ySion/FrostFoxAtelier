#include "memory/fatal.hpp"
#include "support.hpp"
#include <Windows.h>

int wmain(int argc, wchar_t** argv) {
    atelier::install_fatal_handler();
    std::string selected;
    using namespace logging_acceptance;
    require(argc == 4, "Expected case, evidence directory and producer DLL path.");
    const std::wstring_view name{argv[1]};
    selected.assign(name.begin(), name.end());
    const auto root = fs::absolute(argv[2]);
    require(!fs::exists(root), "Refusing to overwrite an earlier acceptance run.");
    fs::create_directories(root);
    if (name == L"roundtrip")
        roundtrip(root);
    else if (name == L"file-failures")
        file_failures(root);
    else if (name == L"producer-churn")
        producer_churn(root);
    else if (name == L"reader")
        reader_cases(root);
    else if (name == L"rotation")
        rotation(root);
    else if (name == L"dll")
        dll(root, argv[3]);
    else
        atelier::fatal("Unknown logging acceptance case.");
    std::printf("Logging acceptance: PASS case=%s\n", selected.c_str());
    return 0;
}
