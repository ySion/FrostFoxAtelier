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
    std::int32_t hidden_secret = 7;

    PlainPair add_pair(PlainPair a, PlainPair b) const noexcept { return {a.x + b.x, a.y + b.y}; }
};

struct ContractHost {};

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
            .hidden_field<&PairHost::hidden_secret>("secret")
            .method<&PairHost::add_pair>("add_pair", true)
            .build(),
        NativeRegistration<ContractHost>(runtime, "test.ContractHost", TypeKind::object)
            .type_metadata({{"source.contract_method", "process"}})
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
    reject_snapshot("bool bad_fin() { return math.is_finite(1); }", "math.is_finite requires");
    reject_snapshot("uint bad_bits() { return math.as_uint(1.0d); }", "math.as_uint requires");
    reject_snapshot("int bad_ibits() { return math.as_int(vector3()); }", "math.as_int requires");
    reject_snapshot("void bad_attr([In] const Array<int>& x) {}", "Parameter attributes apply only to class methods");
    reject_snapshot("class p.A : test.PairHost { void process([In(\"a\", From = \"b\")] const Array<int>& x) {} }",
                    "either a literal name or From");
    reject_snapshot("class p.B : test.PairHost { void process([Out(\"a\", Optional)] Array<int>& x) {} }",
                    "Optional applies only to In");
    reject_snapshot("class p.C : test.PairHost { void process([Port(\"x\")] test.PlainPair& p) {} }",
                    "Port is a flag without arguments");
    reject_snapshot("int ok() { return 1; }\nclass p.Dup : test.PairHost { void process(int x, int x) {} }",
                    "Duplicate parameter x in method: p.Dup.process", 2);

    // Verify math.is_finite, math.as_uint, math.as_int, [RunOver], method parameter attributes,
    // TypeInfo::described_methods, contract-scoped process hot-reload rules, and warm/cold parity.
    auto described_prog = take(Program::create(runtime));
    publish(*described_prog, {"sop", 1, {{"Source/Sop.source", 1, R"(
class test.Ordinary : test.PairHost {
    int count = 0;
    void helper(int a) {}
}

class test.SopNode : test.ContractHost {
    [RunOver] int domain = 0;
    String target = "col";
    void helper(int a) {}
    void process(int plain_port,
                 [Port] const test.PlainPair& port_in,
                 [In("src", Optional), Layer = "point"] const Array<int>& input_col,
                 [InOut] Array<int>& col,
                 [Out(From = "target")] Array<int>& out_col) {}
}
)"}}});
    const auto sop_type = take(runtime.find_type("test.SopNode"));
    const auto* sop_info = take(runtime.describe(sop_type));
    require(sop_info->fields.size() == 2 &&
                sop_info->fields[0].metadata == MetadataList{{"procedural.role", "run_over"}} &&
                sop_info->described_methods.size() == 2 && sop_info->described_methods[1].name == "process" &&
                sop_info->described_methods[1].parameter_names.size() == 5 &&
                sop_info->described_methods[1].parameter_metadata[0].empty() &&
                sop_info->described_methods[1].parameter_metadata[1] == MetadataList{{"procedural.role", "port"}} &&
                sop_info->described_methods[1].parameter_metadata[2] == MetadataList{{"procedural.layer", "point"},
                                                                                     {"procedural.name", "src"},
                                                                                     {"procedural.optional", "true"},
                                                                                     {"procedural.role", "in"}} &&
                sop_info->described_methods[1].parameter_metadata[3] == MetadataList{{"procedural.role", "inout"}} &&
                sop_info->described_methods[1].parameter_metadata[4] ==
                    MetadataList{{"procedural.from", "target"}, {"procedural.role", "out"}},
            "Source class [RunOver] or parameter attributes did not publish into TypeInfo::described_methods.");
    // Ordinary classes (base without source.contract_method) can hot-reload to add `process` and rename its parameter,
    // while non-contract helper parameter renames on contract classes remain compatible as well.
    publish(*described_prog, {"sop", 2, {{"Source/Sop.source", 2, R"(
class test.Ordinary : test.PairHost {
    int count = 0;
    void helper(int a) {}
    void process(int dt) {}
}

class test.SopNode : test.ContractHost {
    [RunOver] int domain = 0;
    String target = "col";
    void helper(int renamed_a) {}
    void process(int plain_port,
                 [Port] const test.PlainPair& port_in,
                 [In("src", Optional), Layer = "point"] const Array<int>& input_col,
                 [InOut] Array<int>& col,
                 [Out(From = "target")] Array<int>& out_col) {}
}
)"}}});
    publish(*described_prog, {"sop", 3, {{"Source/Sop.source", 3, R"(
class test.Ordinary : test.PairHost {
    int count = 0;
    void helper(int a) {}
    void process(int renamed_dt) {}
}

class test.SopNode : test.ContractHost {
    [RunOver] int domain = 0;
    String target = "col";
    void helper(int renamed_a) {}
    void process(int plain_port,
                 [Port] const test.PlainPair& port_in,
                 [In("src", Optional), Layer = "point"] const Array<int>& input_col,
                 [InOut] Array<int>& col,
                 [Out(From = "target")] Array<int>& out_col) {}
}
)"}}});
    const auto ord_proc =
        std::ranges::find(described_prog->entries(), "test.Ordinary.process", &source::EntryInfo::name);
    require(ord_proc != described_prog->entries().end() &&
                ord_proc->parameter_names == std::vector<std::string>{"renamed_dt"},
            "Ordinary class process hot-reload or EntryInfo::parameter_names update failed.");
    // Warm hot-reload must reject methods hiding native base methods or hidden fields, matching cold load.
    for (const auto& [bad_member, expected_msg] :
         {std::pair{"void add_pair(int x) {}", "test.Ordinary.add_pair hides test.PairHost.add_pair"},
          std::pair{"void secret(int x) {}", "test.Ordinary.secret hides test.PairHost.secret"}}) {
        const std::string warm_bad =
            std::string{
                "class test.Ordinary : test.PairHost {\n    int count = 0;\n    void helper(int a) {}\n    void process(int renamed_dt) {}\n    "} +
            bad_member + R"(
}
class test.SopNode : test.ContractHost {
    [RunOver] int domain = 0;
    String target = "col";
    void helper(int renamed_a) {}
    void process(int plain_port,
                 [Port] const test.PlainPair& port_in,
                 [In("src", Optional), Layer = "point"] const Array<int>& input_col,
                 [InOut] Array<int>& col,
                 [Out(From = "target")] Array<int>& out_col) {}
}
)";
        auto hide_req = take(described_prog->request({"sop", 4, {{"Source/Sop.source", 4, warm_bad}}}));
        auto hide_cand = Program::prepare(hide_req);
        require(!hide_cand && hide_cand.error().code == ErrorCode::invalid_definition &&
                    hide_cand.error().message.contains(expected_msg) && hide_cand.error().source &&
                    hide_cand.error().source->line == 5,
                "Warm hot-reload accepted a method hiding an inherited native member or lost its line mapping.");
    }
    // Renaming even an unattributed parameter on a contract `process` method requires rebuild.
    auto rebuild_req = take(described_prog->request({"sop", 5, {{"Source/Sop.source", 5, R"(
class test.Ordinary : test.PairHost {
    int count = 0;
    void helper(int a) {}
    void process(int renamed_dt) {}
}

class test.SopNode : test.ContractHost {
    [RunOver] int domain = 0;
    String target = "col";
    void helper(int renamed_a) {}
    void process(int renamed_plain_port,
                 [Port] const test.PlainPair& port_in,
                 [In("src", Optional), Layer = "point"] const Array<int>& input_col,
                 [InOut] Array<int>& col,
                 [Out(From = "target")] Array<int>& out_col) {}
}
)"}}}));
    auto rebuild_cand = Program::prepare(rebuild_req);
    require(!rebuild_cand && rebuild_cand.error().code == ErrorCode::rebuild_required,
            "Renaming a contract process parameter did not report rebuild_required.");

    // Verify zero-default plain_value POD locals/returns, non-zero default plain_value records,
    // builtin math locals (Vector3), local Array<int> loops, cached SOP Array<int>& writes,
    // and CompileOptions{.fast = true} produce identical results across both optimization tiers.
    const auto host_type = take(runtime.find_type("test.PairHost"));
    auto host = take(runtime.create(host_type));
    const std::string_view pod_and_sop = R"(
bool check_intrinsics(float zero) {
    const float nan_f = zero / zero;
    const float inf_f = 1.0f / zero;
    const double inf_d = -1.0d / double(zero);
    const bool finite_ok = math.is_finite(3.5f) && math.is_finite(-2.0d) &&
                           math.is_finite(vector3(1.0f, -2.0f, 3.0f)) &&
                           math.is_finite(quaternion()) &&
                           math.is_finite(matrix4(1.0f)) &&
                           !math.is_finite(nan_f) && !math.is_finite(inf_f) && !math.is_finite(inf_d) &&
                           !math.is_finite(vector3(1.0f, nan_f, 0.0f)) &&
                           !math.is_finite(quaternion(0.0f, inf_f, 0.0f, 1.0f));
    const uint neg_one_bits = math.as_uint(-1);
    const uint min_int_bits = math.as_uint(-2147483647 - 1);
    const uint neg_float_bits = math.as_uint(-1.0f);
    const uint neg_zero_bits = math.as_uint(-zero);
    const int back_neg_one = math.as_int(neg_one_bits);
    const int float_as_i = math.as_int(-1.0f);
    return finite_ok && neg_one_bits == 4294967295 && min_int_bits == 2147483648 &&
           neg_float_bits == 3212836864 && neg_zero_bits == 2147483648 &&
           back_neg_one == -1 && float_as_i == -1082130432;
}

int evaluate(Array<int>& column, test.PlainPair delta) {
    if (!check_intrinsics(0.0f)) {
        return -1;
    }
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
    // Verify single-source bound Program also rejects class-only parameter attributes.
    {
        auto bound_single = take(Program::create(runtime, host_type));
        auto bad_single = take(bound_single->request("single_bad.source", "void f([In] const Array<int>& a) {}", "f"));
        auto bad_single_cand = Program::prepare(bad_single);
        require(!bad_single_cand &&
                    bad_single_cand.error().message.contains("Parameter attributes apply only to class methods"),
                "Single-source bound Program accepted class-only parameter attributes.");
    }
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
