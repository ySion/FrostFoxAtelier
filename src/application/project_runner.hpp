#pragma once
#include <Windows.h>
#include <cstdint>
#include <filesystem>
#include <span>

namespace atelier::application {
struct RunOptions {
    std::filesystem::path project,scene_snapshot,report;
    HANDLE stop_event{}; // Inherited event; this process closes its own handle.
    bool headless{},preview_ui{};
    std::uint64_t frames{};
};
int run_project(const RunOptions&);
int run_project_command(std::span<wchar_t*> arguments);
}
