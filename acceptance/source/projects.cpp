#include "../helpers.hpp"
#include "source/program.hpp"
#include "runtime/native.hpp"
#include "runtime/foundation.hpp"
#include "world/world.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <windows.h>

namespace {
using namespace atelier;

using source::Program;

using namespace atelier::acceptance::helpers;

namespace w = atelier::world;

void publish(Program& program, source::SourceSnapshot snapshot) {
    auto candidate = take(Program::prepare(take(program.request(std::move(snapshot)))));
    take(program.try_publish(candidate));
}

void verify_project_engine_sources() {
    const auto root =
        std::filesystem::temp_directory_path() / ("atelier-engine-sources-" + std::to_string(GetCurrentProcessId()));
    struct Cleanup {
        std::filesystem::path path;

        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    } cleanup{root};
    std::filesystem::create_directories(root);
    const std::string text =
        "const uint procedural.SEED = 7;\nuint procedural.hash(uint x) { return x * 31 + procedural.SEED; }\n";
    const auto path = root / "random.source";
    std::ofstream(path, std::ios::binary) << "\xef\xbb\xbf" << text;
    const auto loaded = take(source::read_source_file(path, "engine/procedural/random.source"));
    // Independent FNV-1a of these exact saved bytes, including the UTF-8 BOM.
    require(loaded.source == text && loaded.version == 0x29c502cbc158a196ull,
            "Saved Source text or its raw-byte version changed.");
    std::ofstream(path, std::ios::binary) << "\xef\xbb\xbf" << loaded.source;
    const auto restored = take(source::read_source_file(path, "engine/procedural/random.source"));
    require(restored.source == text && restored.version == 0x29c502cbc158a196ull,
            "Current Source format did not round-trip.");
}

source::SourceSnapshot sources() {
    return {"project",
            5,
            {{"Source/Math.source", 17, "float distance(float speed, float dt) { return speed * dt; }"},
             {"Source/Move.source", 23, R"(
class game.Move : world.Component {
    float speed = 2.0f;
    void tick(float dt) { self.translate_owner(distance(self.speed,dt),0.0f,0.0f); }
}
)"}}};
}

void verify_world_project() {
    Runtime runtime;
    const auto world_types = take(w::register_types(runtime));
    auto program = take(Program::create(runtime));
    publish(*program, sources());
    const auto type = take(runtime.find_type("game.Move"));
    w::World world;
    take(world.initialize(runtime, world_types));
    auto owner = take(w::create_entity(runtime));
    auto instance = take(w::create_component(runtime, type));
    take(world.set_transform(owner.id(), {}));
    const auto composed = take(world.attach_components(owner.id(), std::array{w::Attachment{instance.id()}}));
    require(!composed.notification_error, "Source class capability assembly failed.");
    require(!take(world.attach(owner.id())).notification_error, "Source owner attachment failed.");
    const auto tick = take(program->entry("game.Move.tick"));
    take(world.bind_behavior(type, {.tick = [&](Runtime&, ObjectId id, float dt) -> Result<void> {
                                 const std::array args{Scalar{dt}};
                                 auto result = tick.invoke(id, args);
                                 if (!result)
                                     return std::unexpected(result.error());
                                 return {};
                             }}));
    require(!take(world.start()).notification_error, "Source lifecycle start failed.");
    const auto x = [&] { return take(w::read_transform(runtime, owner.id())).position.x; };
    take(world.update(.5f));
    require(x() == 1.f, "Cross-file helper or inherited native action failed.");
    require(!take(world.stop()).notification_error, "Source lifecycle stop failed.");
    program->close();
    require(!take(world.close()).notification_error, "Pinned World cleanup failed.");
    require(!take(w::destroy_entity(runtime, owner.id())).notification_error, "Source owner termination failed.");
}
}

void verify_source_projects() {
    verify_project_engine_sources();
    verify_world_project();
    std::puts("Source project consumer: PASS (current disk format and cross-file World tick)");
}
