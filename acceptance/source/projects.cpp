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
[Flags]
enum game.LayerMask : uint {
    [Label = "静态层", DESC = "地形"]
    Static = 1,
    [Label = "动态层"]
    Dynamic = 2,
    Trigger = 4
}

class game.Move : world.Component {
    float speed = 2.0f;
    [Transient, ReadOnly] float distance_moved = 0.0f;
    game.LayerMask layers = game.LayerMask::Static | game.LayerMask::Dynamic;
    void tick(float dt) {
        if ((self.layers & game.LayerMask::Static) != game.LayerMask::Static) {
            return;
        }
        self.layers = (self.layers ^ game.LayerMask::Dynamic) | game.LayerMask::Trigger;
        const float step = distance(self.speed, dt);
        self.distance_moved = self.distance_moved + step;
        self.translate_owner(step);
    }
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
    const auto* move_info = take(runtime.describe(type));
    const auto mask_type = take(runtime.find_type("game.LayerMask"));
    const auto* mask_info = take(runtime.describe(mask_type));
    require(mask_info->enumeration && mask_info->enumeration->flags &&
                mask_info->enumeration->metadata[0].label == "静态层" &&
                mask_info->enumeration->metadata[0].description == "地形",
            "Source [Flags] enum and enumerator metadata did not publish.");
    const auto moved_field = std::ranges::find(move_info->fields, "distance_moved", &FieldInfo::name);
    const auto layers_field = std::ranges::find(move_info->fields, "layers", &FieldInfo::name);
    require(moved_field != move_info->fields.end() && moved_field->transient && moved_field->read_only &&
                layers_field != move_info->fields.end(),
            "Source class field did not publish Transient, ReadOnly or Flags enum field.");
    const FieldId moved_id = moved_field->id;
    const FieldId layers_id = layers_field->id;
    require(x() == 1.f && std::get<float>(take(runtime.read(instance.id(), std::span{&moved_id, 1}))) == 1.f &&
                std::get<EnumValue>(take(runtime.read(instance.id(), std::span{&layers_id, 1}))).bits == 5u,
            "Cross-file helper, Flags enum ops, default-arg native call or transient/read-only Source write failed.");
    require(!take(world.stop()).notification_error, "Source lifecycle stop failed.");
    program->close();
    require(!take(world.close()).notification_error, "Pinned World cleanup failed.");
    require(!take(w::destroy_entity(runtime, owner.id())).notification_error, "Source owner termination failed.");
}

struct PlainPair {
    std::int32_t x;

    std::int32_t y;
};

struct NonZeroPlain {
    std::int32_t bias = 42;
};

struct PairHost {
    PlainPair add_pair(PlainPair a, PlainPair b) const noexcept { return {a.x + b.x, a.y + b.y}; }
};

void verify_frontend_and_pod_evaluation() {
    Runtime runtime;
    auto non_zero_def =
        NativeRegistration<NonZeroPlain>(runtime, "test.NonZeroPlain", TypeKind::record)
            .field<&NonZeroPlain::bias>("bias", {TypeReference::native<std::int32_t>()}, offsetof(NonZeroPlain, bias))
            .build();
    non_zero_def.native.value_operations_preserve_borrows = true;
    take(runtime.register_types(std::vector<TypeDefinition>{
        string_definition(runtime), array_definition<std::int32_t>(runtime, "test.IntArray"),
        NativeRegistration<PlainPair>(runtime, "test.PlainPair", TypeKind::record)
            .field<&PlainPair::x>("x", {TypeReference::native<std::int32_t>()}, offsetof(PlainPair, x))
            .field<&PlainPair::y>("y", {TypeReference::native<std::int32_t>()}, offsetof(PlainPair, y))
            .build(),
        std::move(non_zero_def),
        NativeRegistration<PairHost>(runtime, "test.PairHost", TypeKind::object)
            .method<&PairHost::add_pair>("add_pair", true)
            .build()}));
    auto project = take(Program::create(runtime));
    const auto reject_snapshot = [&](std::string text, std::string_view expected, std::size_t expected_line = 0) {
        auto request =
            take(project->request(source::SourceSnapshot{"bad", 1, {{"Source/Bad.source", 1, std::move(text)}}}));
        auto prepared = Program::prepare(request);
        require(!prepared && prepared.error().message.contains(expected),
                prepared ? "Invalid source unexpectedly compiled." : prepared.error().message);
        if (expected_line)
            require(prepared.error().source && prepared.error().source->line == expected_line,
                    "Unexpected diagnostic line number.");
    };
    reject_snapshot("void bad() { String s = \"\xff\"; }", "valid UTF-8");
    reject_snapshot("int ok() { return 1; }\rint bad() { return true; }", "Type mismatch", 2);
    reject_snapshot("void bodyless();", "Expected '{'");
    reject_snapshot("struct MissingSemi { int x; }", "Expected ';'");
    reject_snapshot("[UnknownAttr] enum BadEnum { a }", "Unknown enum attribute");

    // Verify zero-default plain_value POD locals/returns, non-zero default plain_value records,
    // builtin math locals (Vector3), local Array<int> loops, cached SOP Array<int>& writes,
    // and CompileOptions{.fast = true} produce identical results across both optimization tiers.
    const auto host_type = take(runtime.find_type("test.PairHost"));
    auto host = take(runtime.create(host_type));
    const std::string_view pod_and_sop = R"(
int evaluate(Array<int>& column, test.PlainPair delta) {
    test.PlainPair zero;
    test.NonZeroPlain preset;
    test.PlainPair sum = self.add_pair(zero, delta);
    sum = self.add_pair(sum, delta);
    vector3 v = vector3(float(sum.x), float(sum.y), 0.0f);
    vector3 v2 = v + vector3(1.0f, 1.0f, 0.0f);
    Array<int> scratch;
    scratch.add(int(v2.x) - 1);
    scratch.add(int(v2.y) - 1);
    int total = 0;
    for (int i = 0; i < int(column.size()); i = i + 1) {
        column[i] = column[i] + scratch[0] + scratch[1];
        total = total + column[i];
    }
    return total + zero.x + zero.y + preset.bias;
}
)";
    for (const bool fast : {false, true}) {
        auto tier = take(Program::create(runtime, host_type));
        auto tier_req =
            take(tier->request("pod_sop.source", pod_and_sop, "evaluate", source::CompileOptions{.fast = fast}));
        auto tier_candidate = take(Program::prepare(tier_req));
        take(tier->try_publish(tier_candidate));
        Array<std::int32_t> column;
        column.add(1);
        column.add(2);
        column.add(3);
        const PlainPair delta{3, 4};
        const std::array tier_args{Argument::borrow(column), Argument::borrow(delta)};
        const auto tier_result = take(tier->entry().invoke(host.id(), tier_args));
        // delta*2 = (6, 8) -> +14 to each of {1, 2, 3} -> {15, 16, 17}, sum = 48, + preset.bias (42) = 90.
        require(tier_result && std::get<std::int32_t>(*tier_result) == 90 && column[0] == 15 && column[1] == 16 &&
                    column[2] == 17,
                "POD local/return, non-zero default record or SOP array evaluation produced unexpected results.");
    }
}
}

void verify_source_projects() {
    verify_project_engine_sources();
    verify_world_project();
    verify_frontend_and_pod_evaluation();
    std::puts("Source project consumer: PASS (current disk format and cross-file World tick)");
}
