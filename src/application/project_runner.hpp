#pragma once
#include "world/diagnostic.hpp"
#include <Windows.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>

namespace atelier::editor {
class Project;
class Session;
}
namespace atelier::application {
// Submits owned diagnostics of host work at the same boundary as World diagnostics.
// Live acknowledgements retain their protocol verdict and are not a log transport.
using Report = std::function<void(std::span<const world::Diagnostic>)>;
// Feeds editor changes into the Play copy. The runner calls service once per
// frame at the host boundary, before start on the first frame and before every
// update after it. service must not block or throw.
struct PlayChannel {
    virtual ~PlayChannel() = default;
    virtual void service(editor::Session&, editor::Project&, const Report&) = 0;
};
struct RunOptions {
    std::filesystem::path project, scene_snapshot, report;
    HANDLE stop_event{}; // Inherited event; this process closes its own handle.
    bool headless{}, preview_ui{};
    // Stops the World at the first object failure (acceptance and CI) instead of isolating it.
    bool halt{};
    std::uint64_t frames{};
    PlayChannel* channel{}; // Not owned; outlives the run.
};
int run_project(const RunOptions&);
int run_project_command(std::span<wchar_t*> arguments);
}
