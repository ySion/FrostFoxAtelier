#include "support.hpp"
#include "producer_api.hpp"
#include <Windows.h>

namespace logging_acceptance {
namespace {
log::SubmitResult stable_submit(log::Entry value) noexcept {
    return log::submit(std::move(value));
}

struct Module {
    HMODULE value{};

    ~Module() {
        if (value)
            FreeLibrary(value);
    }
};
}

void dll(const fs::path& root, const fs::path& library) {
    auto run = start(root / "run");
    Module module{LoadLibraryW(fs::absolute(library).c_str())};
    require(module.value != nullptr, "Cannot load the actual logging producer DLL.");
    const auto produce = reinterpret_cast<Produce>(GetProcAddress(module.value, "logging_produce"));
    require(produce != nullptr && produce(&stable_submit) == 0, "DLL did not submit through the stable host boundary.");
    require(FreeLibrary(module.value) != FALSE, "FreeLibrary failed.");
    module.value = nullptr;
    require(GetModuleHandleW(library.filename().c_str()) == nullptr, "Producer DLL remains loaded.");
    // No pre-unload flush. This proves the supported value/host boundary, not
    // that the scheduler necessarily deferred every decoder until after unload.
    healthy(run->finish());
    const auto read = read_finished(root / "run");
    clean_reader(read);
    require(read.records.size() == 1, "DLL record was lost or duplicated.");
    auto expected = entry("dll.owned", std::string{"卸载前\0after-NUL", sizeof("卸载前\0after-NUL") - 1}, 193);
    expected.category = "acceptance.dll";
    expected.source = {
        "Source/动态.source", "E:/ProducerProject", "dll-function", 29, 7, std::uint64_t{9007199254741033ULL},
        std::uint64_t{111}};
    expected.fields[0].value = std::string{"unload"};
    expected.fields.push_back({"object", std::string{"17:31:47"}});
    expected.repeat = log::Repeat{"dll/193/17:31:47", 7};
    same_entry(read.records.front().entry, expected);
    std::puts("DLL evidence: producer unloaded before finish; backend decode scheduling is not controlled.");
}
}
