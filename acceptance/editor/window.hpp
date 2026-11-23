#pragma once
#include "runtime/runtime.hpp"
#include <filesystem>

namespace atelier::application {
class Application;
}

atelier::Result<void> guard_acceptance_native_dialogs();

atelier::Result<void> capture_editor_snapshot(atelier::application::Application&, const std::filesystem::path& output);

atelier::Result<void> profile_editor_render_performance(atelier::application::Application&,
                                                        const std::filesystem::path& output);

atelier::Result<void> profile_editor_ui_reload(atelier::application::Application&, const std::filesystem::path& output);
