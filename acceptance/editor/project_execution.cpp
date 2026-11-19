#include "../helpers.hpp"
#include "assets/file.hpp"
#include "editor/project_code.hpp"
#include "application/play_process.hpp"
#include "application/timing.hpp"
#include "logging/logging.hpp"
#include <Windows.h>
#include <chrono>
#include <cstdio>

namespace {
using namespace atelier;

using namespace atelier::acceptance::helpers;

void complete(Result<world::Commit> result) {
    const auto commit = take(std::move(result));
    if (commit.notification_error)
        fatal(commit.notification_error->message);
}

void run(const std::filesystem::path& root) {
    auto project = take(editor::Project::create(root));
    const auto component = root / "Source/Move.source";
    take(editor::write_file(
        root / "Source/Math.source",
        "float travel(float speed,float dt) { return speed * dt * (2.0f * procedural.unit(2147483648)); }"));
    take(editor::write_file(component, "class project.Move : world.Component { float speed=2.0f; void tick(float dt) {"
                                       "self.translate_owner(travel(self.speed,dt),0.0f,0.0f); } }"));
    auto loaded = editor::load_project_scene(project, take(assets::read_file(root / "Content/Scenes/Main.scene")));
    require(loaded.session != nullptr, loaded.error ? loaded.error->message : "Project Source failed to load.");
    auto& document = *loaded.session;
    complete(document.add_entity());
    const auto entity = document.selection().entity;
    take(document.set_transform(entity, {}));
    complete(document.add_component(entity, take(document.world().runtime().find_type("project.Move"))));
    const auto snapshot = root / "Cache/run.scene";
    take(editor::write_file(snapshot, take(document.serialize())));
    complete(document.start());
    take(document.update(.5f));
    // procedural.unit(2^31)=0.5, so speed 2 * dt 0.5 gives one unit of travel.
    require(take(world::read_transform(document.world().runtime(), entity)).position.x == 1.f,
            "Project Source tick did not produce x=1.");
    complete(document.stop());
    complete(document.close());

    // The independent child uses the saved scene, but compiles a deliberately stuck tick.
    take(editor::write_file(
        component,
        "class project.Move : world.Component { float speed=2.0f; void tick(float dt) { while (true) {} } }"));
    application::PlayProcess process;
    take(process.start(root, snapshot, true));
    auto reader = take(logging::Reader::open(process.log_directory()));
    bool started{};
    const auto startup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
    while (process.running() && std::chrono::steady_clock::now() < startup_deadline && !started) {
        process.poll();
        for (const auto& record : reader->poll().records)
            started |= record.entry.event_code == "runner.started";
        Sleep(10);
    }
    require(started, "Stuck Source child did not start.");
    Sleep(100);
    process.stop();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (process.running() && std::chrono::steady_clock::now() < deadline) {
        process.poll();
        Sleep(10);
    }
    require(!process.running() && process.forced(), "Stuck Source child did not terminate within five seconds.");
}
}

int main() {
    atelier::install_fatal_handler();
    application::install_frame_timing();
    const auto root = std::filesystem::current_path() / "project-integration-evidence" /
                      (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    run(root);
    std::puts("Project integration: PASS (Source tick, stuck child shutdown)");
}
