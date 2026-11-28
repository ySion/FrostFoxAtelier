#pragma once

#include "foundation/span.hpp"
#include "foundation/string.hpp"
#include "physics/timing.hpp"
#include "renderer/timing.hpp"
#include "memory/fatal.hpp"

// CPU phases of one host step: Application::step in the editor and one loop of
// the project runner in Play. Host phases sit directly under the frame; World,
// physics and renderer phases open their own roots at the same level.
namespace atelier::application::phases {
inline constinit timing::Marker admission{"app.admission", "Presentation admission", &timing::frame};

inline constinit timing::Marker input{"app.input", "Input and window events", &timing::frame};

inline constinit timing::Marker project{"app.project", "Project, Play and code", &timing::frame};

// Control in the editor; the Play link in a project run.
inline constinit timing::Marker control{"app.control", "Control / Play link", &timing::frame};

inline constinit timing::Marker gc{"app.gc", "GC step", &timing::frame};

inline constinit timing::Marker ui{"app.ui", "UI description", &timing::frame};

inline constinit timing::Marker layout{"app.layout", "UI layout", &timing::frame};

inline constinit timing::Marker popup{"app.popup", "Popup menus", &timing::frame};

inline constinit timing::Marker scene{"app.scene", "Scene snapshot", &timing::frame};

inline constinit timing::Marker extract{"app.scene.extract", "Presentation extraction", &scene};

inline constexpr std::array<timing::Marker*, 10> markers{&admission, &input,  &project, &control, &gc,
                                                         &ui,        &layout, &popup,   &scene,   &extract};
}

namespace atelier::application {
// The product catalog: host, World, physics and renderer phases, then `extra`.
// Call once at process start, before any Application or project run exists.
inline void install_frame_timing(Span<timing::Marker* const> extra = {}) {
    timing::install(
        {phases::markers, world::phases::markers, physics::phases::markers, renderer::phases::markers, extra});
}

// Hosts that record frames refuse to start without the product catalog instead
// of recording every phase as a structure error.
inline void require_frame_timing() {
    const std::array<Span<timing::Marker* const>, 4> modules{
        phases::markers, world::phases::markers, physics::phases::markers, renderer::phases::markers};
    for (const auto module : modules)
        for (const auto* marker : module)
            if (!marker->registered())
                fatal(String::concat(
                    "Frame timing is not installed: call install_frame_timing() at process start (missing ",
                    String::validated(marker->key()), ")."));
}
}
