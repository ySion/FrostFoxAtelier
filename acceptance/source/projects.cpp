#include "../helpers.hpp"
#include "source/program.hpp"
#include "runtime/native.hpp"
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

struct EngineBase {
    float x;
};

void verify_property_update() {
    Runtime runtime;
    take(NativeRegistration<EngineBase>{runtime, "procedural.Base", TypeKind::object}.publish());
    Program program(runtime);
    source::SourceSnapshot input{
        "properties",
        1,
        {{"engine/props/value.source", 1, "uint props.GetValue() { return 7; } void props.SetValue(uint v) {}"},
         {"Source/Property.source", 1, R"(
            uint value() { return props.Value; }
            class game.Property : procedural.Base {
                uint stored = 3;
                uint GetLocal() { return self.stored; }
                void SetLocal(uint v) { self.stored = v; }
                uint run() { Local = Local + props.Value; return Local; }
            }
        )"}}};
    publish(program, input);
    const auto instance = take(runtime.create(take(runtime.find_type("game.Property"))));
    const auto entry = take(take(program.entry("game.Property.run")).bind<std::uint32_t>());
    const auto old = take(take(program.pin("game.Property.run")).bind<std::uint32_t>());
    require(take(entry.invoke(instance.id())) == 10, "Initial cross-file property call produced the wrong state.");
    input.files[0].source = "uint props.GetValue() { return 11; } void props.SetValue(uint v) {}";
    // Reorder exports while retaining the same following handle and stable slot.
    input.files[1].source = R"(
        uint value() { return props.Value; }
        class game.Property : procedural.Base {
            uint stored = 3;
            uint run() { Local = Local + props.Value; return Local; }
            uint GetLocal() { return self.stored; }
            void SetLocal(uint v) { self.stored = v; }
        }
    )";
    publish(program, input);
    require(take(entry.invoke(instance.id())) == 21 &&
                std::get<std::uint32_t>(*take(take(program.entry("value")).invoke({}, {}))) == 11 &&
                take(old.invoke(instance.id())) == 28,
            "Property update kept stale dependencies or changed a pinned accessor version.");
}

source::SourceSnapshot sources(float factor = 1) {
    return {"project",
            5,
            {{"Source/Math.source", 17,
              "float distance(float speed, float dt) { return speed * dt * " + std::to_string(factor) + "f; }"},
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
    Program program(runtime);
    publish(program, sources());
    const auto type = take(runtime.find_type("game.Move"));
    w::World world(runtime, world_types);
    auto owner = take(w::create_entity(runtime));
    auto instance = take(w::create_component(runtime, type));
    take(world.set_transform(owner.id(), {}));
    const auto composed = take(world.attach_components(owner.id(), std::array{w::Attachment{instance.id()}}));
    require(!composed.notification_error, "Source class capability assembly failed.");
    require(!take(world.attach(owner.id())).notification_error, "Source owner attachment failed.");
    const auto speed = take(runtime.find_field(type, "speed"));
    const auto tick = take(program.entry("game.Move.tick"));
    const auto old = take(program.pin("game.Move.tick"));
    bool replay_old{};
    take(world.bind_behavior(type, {.tick = [&](Runtime&, ObjectId id, float dt) -> Result<void> {
                                 const std::array args{Scalar{dt}};
                                 auto result = tick.invoke(id, args);
                                 if (!result)
                                     return std::unexpected(result.error());
                                 if (replay_old) {
                                     auto pinned = old.invoke(id, args);
                                     if (!pinned)
                                         return std::unexpected(pinned.error());
                                 }
                                 return {};
                             }}));
    require(!take(world.start()).notification_error, "Source lifecycle start failed.");
    const auto x = [&] { return take(w::read_transform(runtime, owner.id())).position.x; };
    take(world.update(.5f));
    require(x() == 1.f, "Cross-file helper or inherited native action failed.");
    take(runtime.write(instance.id(), speed, 4.f));
    publish(program, sources(2));
    replay_old = true;
    take(world.update(.5f));
    replay_old = false;
    require(x() == 7.f && std::get<float>(take(runtime.read(instance.id(), speed))) == 4.f,
            "Hot update lost instance state or its old code lease.");
    auto bad = sources(2);
    bad.files[0].source = "float distance(float speed, float dt) { return missing; }";
    require(!Program::prepare(take(program.request(bad))), "Invalid replacement compiled.");
    take(world.update(.5f));
    require(x() == 11.f, "Failed candidate replaced active World code.");
    require(!take(world.stop()).notification_error, "Source lifecycle stop failed.");
    program.close();
    take(world.bind_behavior(type, {.tick = [old](Runtime&, ObjectId id, float dt) -> Result<void> {
                                 auto result = old.invoke(id, std::array{Scalar{dt}});
                                 if (!result)
                                     return std::unexpected(result.error());
                                 return {};
                             }}));
    require(!take(world.start()).notification_error, "Pinned World behavior failed to resume.");
    take(world.update(.5f));
    require(x() == 13.f, "Pinned lease lost its real World owner after Program close.");
    require(!take(world.close()).notification_error, "Pinned World cleanup failed.");
    require(!take(w::destroy_entity(runtime, owner.id())).notification_error, "Source owner termination failed.");
}
}

void verify_source_projects() {
    verify_project_engine_sources();
    verify_property_update();
    verify_world_project();
    std::puts("Source project consumer: PASS (current disk format, property versions, World hot update and failure)");
}
