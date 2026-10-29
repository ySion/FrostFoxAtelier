#include "producer_api.hpp"

extern "C" __declspec(dllexport) int logging_produce(logging_acceptance::HostSubmit host) noexcept {
    try {
        namespace log = atelier::logging;
        log::Entry entry;
        entry.category = "acceptance.dll";
        entry.event_code = "dll.owned";
        entry.message = std::string{"卸载前\0after-NUL", sizeof("卸载前\0after-NUL") - 1};
        entry.message.append(70013 - entry.message.size(), 'd');
        entry.source = {"Source/动态.source", "E:/ProducerProject", "dll-function", 29, 7,
            std::uint64_t{9007199254741033ULL}, std::uint64_t{111}};
        entry.fields = {{"phase", std::string{"unload"}}, {"context", std::uint64_t{193}},
            {"object", std::string{"17:31:47"}}};
        entry.repeat = log::Repeat{"dll/193/17:31:47", 7};
        const auto result = host(entry);
        entry.message.assign(entry.message.size(), 'x');
        entry.source = {};
        entry.fields.clear();
        return result.status == log::SubmitStatus::accepted ? 0 : 7;
    } catch (...) { return 9; }
}
