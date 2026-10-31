#include "support.hpp"
#include <Windows.h>

int wmain(int argc, wchar_t** argv) {
    std::string selected;
    try {
        using namespace logging_acceptance;
        require(argc == 4, "Expected case, evidence directory and producer DLL path.");
        const std::wstring_view name{argv[1]};
        selected.assign(name.begin(), name.end());
        const auto root = fs::absolute(argv[2]);
        require(!fs::exists(root), "Refusing to overwrite an earlier acceptance run.");
        fs::create_directories(root);
        if (name == L"roundtrip")
            roundtrip(root);
        else if (name == L"boundaries")
            boundaries(root);
        else if (name == L"file-failures")
            file_failures(root);
        else if (name == L"threads")
            threads(root);
        else if (name == L"producer-churn")
            producer_churn(root);
        else if (name == L"reader")
            reader_cases(root);
        else if (name == L"rotation")
            rotation(root);
        else if (name == L"dll")
            dll(root, argv[3]);
        else if (name == L"unexpected-warning")
            return unexpected_warning(root);
        else if (name == L"pressure")
            return pressure(root);
        else
            throw std::runtime_error("Unknown logging acceptance case.");
        std::printf("Logging acceptance: PASS case=%s\n", selected.c_str());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Logging acceptance: FAIL case=%s reason=%s\n", selected.c_str(), error.what());
        return 1;
    }
}
