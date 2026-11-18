#pragma once
#include "../helpers.hpp"
#include "ui_language/live_document.hpp"
#include <fstream>
#include <thread>

namespace atelier::acceptance::ui_reload {
inline void save(const std::filesystem::path& path, std::string_view source) {
    std::ofstream file(path, std::ios::binary);
    file << source;
    file.close();
    helpers::require(file.good(), "Cannot save UI authoring fixture.");
}

inline void begin(ui_language::LiveDocument& document, ui_language::LiveDocument::Clock::time_point& now) {
    now += std::chrono::seconds(1);
    document.poll(now);
    now += std::chrono::milliseconds(300);
    document.poll(now);
}

inline void drain(ui_language::LiveDocument& document, ui_language::LiveDocument::Clock::time_point now) {
    const auto deadline = ui_language::LiveDocument::Clock::now() + std::chrono::seconds(15);
    while (document.compiling()) {
        helpers::require(ui_language::LiveDocument::Clock::now() < deadline, "UI compilation did not finish.");
        std::this_thread::yield();
        document.poll(now);
    }
}
}
