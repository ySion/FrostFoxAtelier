#include "../helpers.hpp"
#include "source/program.hpp"
#include "runtime/native.hpp"
#include "runtime/foundation.hpp"
#include "world/world.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>
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

[DESC = "Move owner.\r\nUnits: metres."]
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
    const auto info = take(runtime.describe(type));
    require(info->description == "Move owner.\nUnits: metres.", "Source class DESC did not reach Runtime reflection.");
    auto revised = sources();
    ++revised.version;
    auto& file = revised.files.back();
    ++file.version;
    file.source.replace(file.source.find("Move owner."), 11, "Translate owner.");
    auto candidate = Program::prepare(take(program->request(std::move(revised))));
    require(!candidate && candidate.error().code == ErrorCode::rebuild_required &&
                info->description == "Move owner.\nUnits: metres.",
            "A changed class DESC must request a rebuild and preserve the published description.");
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

struct SeqHolder {
    Array<std::int32_t> lut;
};

struct NonZeroPlain {
    std::int32_t bias = 42;
};

struct PairHost {
    std::int32_t hidden_secret = 7;

    std::int32_t scale = 9;

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
            .field<&PairHost::scale>("scale")
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
    reject_snapshot("[DESC = 42] class p.Bad : test.PairHost {}", "DESC requires a string literal.");
    reject_snapshot(R"([DESC = "bad\0text"] class p.Bad : test.PairHost {})", "DESC must not contain control");
    reject_snapshot(R"([DESC = "bad target"] const int BAD = 1;)", "Attributes do not apply to module constants");
    reject_snapshot(R"(const String BAD_STR = "nope"; int f() { return 0; })",
                    "Module constants require a scalar, enum, built-in math, plain struct, or Array of plain values");
    reject_snapshot("const Array<int> TAB = { 1, 2 }; void bad() { TAB[0] = 3; }",
                    "Assignment requires a writable local or field");
    reject_snapshot("const Array<int> TAB = { 1, 2 }; void bad() { TAB.add(3); }",
                    "Structural mutation requires a mutable owned or borrowed sequence");
    reject_snapshot("const Array<int> TAB = { 1, 2 }; void take_mut(Array<int>& a) {} void bad() { take_mut(TAB); }",
                    "Writable reference requires a mutable lvalue");
    reject_snapshot("struct S { int x; }; const S CS = { 1 }; void bad() { CS.x = 2; }",
                    "Assignment requires a writable local or field");
    reject_snapshot("const Array<int> TAB = { 10, 20 }; const int OOB = TAB[2]; int f() { return OOB; }",
                    "Constant array index is out of bounds");
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
    // Host-registered Runtime descendants of a Source class must also obey cold/warm hiding parity in both orders:
    // (1) warm-added `process` on `test.Ordinary` blocks registering a child with member `process`;
    // (2) registering a child with member `child_slot` blocks `test.Ordinary` from warm-adding `void child_slot()`.
    {
        TypeDefinition bad_child;
        bad_child.name = "test.BadManagedChild";
        bad_child.kind = TypeKind::object;
        bad_child.base = TypeReference{"test.Ordinary"};
        bad_child.fields.push_back({"process", {TypeReference::native<std::int32_t>()}});
        const auto rejected_child = runtime.register_type(std::move(bad_child));
        require(!rejected_child && rejected_child.error().code == ErrorCode::invalid_definition &&
                    rejected_child.error().message.contains("test.BadManagedChild.process hides test.Ordinary.process"),
                "Runtime child type hid a warm-added Source ancestor method.");

        TypeDefinition ok_child;
        ok_child.name = "test.OkManagedChild";
        ok_child.kind = TypeKind::object;
        ok_child.base = TypeReference{"test.Ordinary"};
        ok_child.fields.push_back({"child_slot", {TypeReference::native<std::int32_t>()}});
        take(runtime.register_type(std::move(ok_child)));

        const std::string warm_clash_child = R"(class test.Ordinary : test.PairHost {
    int count = 0;
    void helper(int a) {}
    void process(int renamed_dt) {}
    void child_slot() {}
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
        auto child_clash_req = take(described_prog->request({"sop", 4, {{"Source/Sop.source", 4, warm_clash_child}}}));
        auto child_clash_cand = Program::prepare(child_clash_req);
        require(!child_clash_cand && child_clash_cand.error().code == ErrorCode::invalid_definition &&
                    child_clash_cand.error().message.contains(
                        "test.OkManagedChild.child_slot hides test.Ordinary.child_slot") &&
                    child_clash_cand.error().source && child_clash_cand.error().source->line == 5,
                "Warm Source ancestor method addition hid an existing Runtime descendant member.");
    }
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

    // 1. Standalone and stateless_direct `fail("...")` + non-void branch termination.
    {
        auto pure_prog = take(Program::create(runtime));
        const std::string_view pure_src = R"(
int checked_double(int x) {
    if (x < 0) {
        fail("Negative scalar rejected.");
    }
    return x * 2;
}
int run_checked(int x) {
    return checked_double(x);
}
)";
        auto req = take(pure_prog->request("pure_fail.source", pure_src, "run_checked"));
        auto cand = take(Program::prepare(req));
        take(pure_prog->try_publish(cand));
        const auto pinned = take(pure_prog->entry().bind<std::int32_t, std::int32_t>());
        require(take(pinned.invoke(ObjectId{}, 21)) == 42, "Stateless direct helper call failed.");
        const auto bad = pinned.invoke(ObjectId{}, -3);
        require(!bad && bad.error().code == ErrorCode::invocation_failed &&
                    bad.error().message == "Negative scalar rejected." && bad.error().source &&
                    bad.error().source->line == 4,
                "Stateless direct fail(...) did not report exact message and source line.");
    }

    // 2. Multi-chunk `parallel_for`, `self` field snapshot, `const Record&` + LUT reads, `[i]` writes,
    //    diagnostics owned-value cleanup on parallel failure, and runtime `parallel_alias` verification.
    take(runtime.register_types(std::vector<TypeDefinition>{
        array_definition<bool>(runtime, "test.BoolArray"), array_definition<PlainPair>(runtime, "test.PairArray"),
        NativeRegistration<SeqHolder>(runtime, "test.SeqHolder", TypeKind::record)
            .field<&SeqHolder::lut>("lut", {TypeReference::native<Array<std::int32_t>>()}, offsetof(SeqHolder, lut))
            .build()}));
    {
        const std::string_view clean_parallel_src = R"(
int scale_step(int base, int factor, int& slot) {
    if (factor < 0) {
        fail("Negative LUT factor.");
    }
    slot = base * factor;
    return slot;
}

void run_parallel(int count, const test.PlainPair& pair, const Array<int>& lut,
                  Array<int>& inout, Array<bool>& mask, int trigger_oob) {
    Array<int> temp;
    temp.resize(16);
    const int bonus = pair.y;
    parallel_for (uint i : count) {
        if ((i & 1) == 1) {
            mask[i] = false;
            continue;
        }
        mask[i] = true;
        if (trigger_oob != 0 && i == 2500) {
            const int unused = lut[lut.size() + 1];
            inout[i] = unused;
            continue;
        }
        test.PlainPair local_pair;
        local_pair.x = int(i);
        int computed = 0;
        const int lut_val = lut[i % lut.size()];
        scale_step(local_pair.x + pair.x, lut_val, computed);
        inout[i] = inout[i] + computed + bonus + self.scale;
    }
}
)";
        auto par_prog = take(Program::create(runtime, host_type));
        auto par_req = take(par_prog->request("parallel_kernel.source", clean_parallel_src, "run_parallel",
                                              source::CompileOptions{.diagnostics = true}));
        auto par_cand = take(Program::prepare(par_req));
        take(par_prog->try_publish(par_cand));

        constexpr std::uint32_t count = 4096; // Spans 4 chunks at grain = 1024.
        const PlainPair pair{3, 7};
        Array<std::int32_t> lut{2, 3, 5, 7};
        Array<std::int32_t> inout;
        Array<bool> mask;
        inout.resize(count);
        mask.resize(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            inout[i] = 10;
            mask[i] = true;
        }
        // Empty count (0) succeeds without touching elements; negative int count fails with sequence_size.
        {
            const std::int32_t zero_count = 0;
            const std::int32_t neg_count = -1;
            const std::int32_t no_oob = 0;
            const std::array zero_args{
                Argument{Scalar{zero_count}}, Argument::borrow(pair), Argument::borrow(std::as_const(lut)),
                Argument::borrow(inout),      Argument::borrow(mask), Argument{Scalar{no_oob}}};
            take(par_prog->entry().invoke(host.id(), zero_args));
            const std::array neg_args{
                Argument{Scalar{neg_count}}, Argument::borrow(pair), Argument::borrow(std::as_const(lut)),
                Argument::borrow(inout),     Argument::borrow(mask), Argument{Scalar{no_oob}}};
            const auto neg_res = par_prog->entry().invoke(host.id(), neg_args);
            require(!neg_res && neg_res.error().code == ErrorCode::invocation_failed &&
                        neg_res.error().message.contains("Sequence size cannot be negative"),
                    "Negative int parallel_for count was not rejected.");
        }
        // Full 4096-element parallel run.
        {
            source::Diagnostics diag;
            const std::int32_t full_count = static_cast<std::int32_t>(count);
            const std::int32_t no_oob = 0;
            const std::array ok_args{
                Argument{Scalar{full_count}}, Argument::borrow(pair), Argument::borrow(std::as_const(lut)),
                Argument::borrow(inout),      Argument::borrow(mask), Argument{Scalar{no_oob}}};
            take(par_prog->entry().invoke(host.id(), ok_args, &diag));
            const auto snap = diag.snapshot();
            require(snap.status == source::DiagnosticStatus::completed && snap.remaining_owned_values == 0 &&
                        snap.resources_cleared,
                    "Parallel execution left owned values or failed diagnostics.");
            for (std::uint32_t i = 0; i < count; ++i) {
                if ((i & 1) == 1) {
                    require(!mask[i] && inout[i] == 10, "Odd continue iteration modified inout or mask.");
                } else {
                    const std::int32_t expected =
                        10 + static_cast<std::int32_t>(i + 3) * lut[i % 4] + 7 + 9; // host.scale == 9
                    require(mask[i] && inout[i] == expected, "Parallel element computation mismatch.");
                }
            }
        }
        // Parallel worker failure via helper `fail(...)` cleans up outer owned `temp` and reports exact line.
        {
            lut[2] = -1;
            source::Diagnostics diag;
            const std::int32_t full_count = static_cast<std::int32_t>(count);
            const std::int32_t no_oob = 0;
            const std::array fail_args{
                Argument{Scalar{full_count}}, Argument::borrow(pair), Argument::borrow(std::as_const(lut)),
                Argument::borrow(inout),      Argument::borrow(mask), Argument{Scalar{no_oob}}};
            const auto fail_res = par_prog->entry().invoke(host.id(), fail_args, &diag);
            const auto snap = diag.snapshot();
            require(!fail_res && fail_res.error().code == ErrorCode::invocation_failed &&
                        fail_res.error().message == "Negative LUT factor." && fail_res.error().source &&
                        fail_res.error().source->line == 4 && snap.status == source::DiagnosticStatus::failed &&
                        snap.remaining_owned_values == 0 && snap.resources_cleared && snap.location &&
                        snap.location->source.line == 4,
                    "Parallel helper fail(...) lost message, line or outer owned-value cleanup.");
            lut[2] = 5;
        }
        // Parallel worker bounds failure (`sequence_bounds`) at iteration 2500 also converges cleanly.
        {
            source::Diagnostics diag;
            const std::int32_t full_count = static_cast<std::int32_t>(count);
            const std::int32_t oob = 1;
            const std::array oob_args{
                Argument{Scalar{full_count}}, Argument::borrow(pair), Argument::borrow(std::as_const(lut)),
                Argument::borrow(inout),      Argument::borrow(mask), Argument{Scalar{oob}}};
            const auto oob_res = par_prog->entry().invoke(host.id(), oob_args, &diag);
            const auto snap = diag.snapshot();
            require(!oob_res && oob_res.error().code == ErrorCode::invocation_failed &&
                        oob_res.error().message.contains("index is out of range") && oob_res.error().source &&
                        oob_res.error().source->line == 22 && snap.remaining_owned_values == 0 &&
                        snap.resources_cleared,
                    "Parallel sequence_bounds failure lost location or leaked outer owned values.");
        }
        // Passing the same buffer as arbitrary-indexed `lut` and writable `inout` must trigger `parallel_alias`.
        {
            const std::int32_t full_count = static_cast<std::int32_t>(count);
            const std::int32_t no_oob = 0;
            const std::array alias_args{
                Argument{Scalar{full_count}}, Argument::borrow(pair), Argument::borrow(std::as_const(inout)),
                Argument::borrow(inout),      Argument::borrow(mask), Argument{Scalar{no_oob}}};
            const auto alias_res = par_prog->entry().invoke(host.id(), alias_args);
            require(!alias_res && alias_res.error().code == ErrorCode::invocation_failed &&
                        alias_res.error().message.contains("overlaps another captured sequence"),
                    "Overlapping arbitrary-read and writable Arrays were not rejected before parallel_for.");
        }
    }

    // 3. Debugger stepping inside `parallel_for` and stopping on `fail(...)` (`CompileOptions::debugging`).
    {
        const std::string_view dbg_src = R"(
void debug_parallel(Array<int>& values) {
    const uint n = values.size();
    parallel_for (uint i : n) {
        if (i == 1) {
            fail("Stopped in debug parallel.");
        }
        values[i] = int(i) + 100;
    }
}
)";
        auto dbg_prog = take(Program::create(runtime));
        auto dbg_req = take(dbg_prog->request("dbg_parallel.source", dbg_src, "debug_parallel",
                                              source::CompileOptions{.debugging = true}));
        auto dbg_cand = take(Program::prepare(dbg_req));
        take(dbg_prog->try_publish(dbg_cand));
        Array<std::int32_t> values{0, 0, 0};
        bool saw_i_zero = false;
        bool saw_error_stop = false;
        source::Debugger debugger{[&](const source::DebugSnapshot& stop) noexcept -> Result<source::DebugCommand> {
            if (!stop.frames.empty()) {
                for (const auto& loc : stop.frames.front().locals)
                    if (loc.name == "i" && loc.state == source::DebugValueState::available && loc.scalar &&
                        std::get<std::uint32_t>(*loc.scalar) == 0u)
                        saw_i_zero = true;
            }
            if (stop.reason == source::DebugStopReason::error && stop.error &&
                stop.error->message == "Stopped in debug parallel.")
                saw_error_stop = true;
            return stop.command(source::DebugAction::step_into);
        }};
        const std::array dbg_args{Argument::borrow(values)};
        const auto dbg_res = dbg_prog->entry().invoke(ObjectId{}, dbg_args, nullptr, &debugger);
        require(!dbg_res && dbg_res.error().message == "Stopped in debug parallel." && values[0] == 100 && saw_i_zero &&
                    saw_error_stop,
                "Debugger stepping inside parallel_for or fail(...) error stop failed.");
    }

    // 4. Static compile-time data-race and contract rejections.
    {
        const std::pair<std::string_view, std::string_view> bad_cases[]{
            {"void f(Array<int>& a) { int s = 0; parallel_for (uint i : a.size()) { s = s + 1; } }",
             "parallel_for can only mutate locals declared inside the loop or outer Array[i] elements"},
            {"void bump(int& x) { x = x + 1; } void f(Array<int>& a) { int s = 0; parallel_for (uint i : a.size()) { bump(s); } }",
             "parallel_for can only mutate locals declared inside the loop or outer Array[i] elements"},
            {"void f(Array<int>& a) { parallel_for (uint i : a.size()) { a[i + 1] = 1; } }",
             "parallel_for element writes must index directly by the loop variable"},
            {"void f(Array<int>& a) { parallel_for (uint i : a.size()) { a[i] = a[0]; } }",
             "cannot also be indexed by an expression other than the loop variable"},
            {"void f(Array<int>& a) { parallel_for (uint i : a.size()) { parallel_for (uint j : 2) { a[i] = int(j); } } }",
             "parallel_for cannot be nested"},
            {"void f(Array<int>& a) { parallel_for (uint i : a.size()) { break; } }", "break cannot exit parallel_for"},
            {"void f(Array<int>& a) { parallel_for (uint i : a.size()) { return; } }",
             "return cannot exit parallel_for"},
            {"async void f() { parallel_for (uint i : 4) {} }", "parallel_for cannot be used in an async function"},
            {"void f(const String& s) { fail(s); }", "fail requires a string literal message"},
            {"void f(Array<int>& a) { parallel_for (uint i : a.size()) { test.PlainPair z; test.PlainPair p = self.add_pair(z, z); a[i] = p.x; } }",
             "parallel_for does not allow native calls"},
            {"int read_any(const Array<int>& a, uint idx) { return a[idx]; } "
             "int sneak(const test.SeqHolder& h, uint idx) { return read_any(h.lut, idx); } "
             "void f(const test.SeqHolder& h, Array<int>& out) { parallel_for (uint i : out.size()) { out[i] = sneak(h, i); } }",
             "is not parallel-safe and cannot be called in parallel_for"},
            {"void f(const test.SeqHolder& h, Array<int>& out) { parallel_for (uint i : out.size()) { out[i] = h.lut[i]; } }",
             "requires a top-level outer Array or String"},
        };
        for (const auto& [bad_code, expected] : bad_cases) {
            auto bad_prog = take(Program::create(runtime, host_type));
            auto bad_req = take(bad_prog->request("bad_parallel.source", bad_code, "f"));
            auto bad_cand = Program::prepare(bad_req);
            require(!bad_cand && bad_cand.error().message.contains(expected),
                    "Invalid parallel_for / fail case was not rejected with expected diagnostic.");
        }
    }

    // 5. Borrowed record/scalar overlap parity across normal & debug builds, helper zero task-body sequence
    //    re-projection + merged `source_call` diagnostics, embedded-NUL `fail`, and >512-byte UTF-8 coroutine `fail`.
    {
        const auto publish_single = [](Program& prog, std::string_view name, std::string_view src,
                                       std::string_view entry = {}, source::CompileOptions opts = {}) {
            auto req = take(prog.request(name, src, entry, opts));
            auto cand = take(Program::prepare(req));
            take(prog.try_publish(cand));
        };
        // (a) Pure-Source `rows[0]` record borrow and `xs[0]` scalar borrow overlapping writable Array -> `parallel_alias`
        //     in BOTH normal (`debugging = false`) and debug (`debugging = true`) builds; by-value scalar capture gives
        //     identical `[12, 12]` in both builds.
        const std::string_view alias_parity_src = R"(
void seed_rows(const test.PlainPair& seed, Array<test.PlainPair>& rows) {
    parallel_for (uint i : rows.size()) {
        rows[i].x = rows[i].x + seed.x + 10;
    }
}
void trigger_record_overlap(Array<test.PlainPair>& rows) {
    seed_rows(rows[0], rows);
}
void add_ref_bias(const int& bias, Array<int>& xs) {
    parallel_for (uint i : xs.size()) {
        xs[i] = xs[i] + bias + 10;
    }
}
void trigger_scalar_ref_overlap(Array<int>& xs) {
    add_ref_bias(xs[0], xs);
}
void add_val_bias(int bias, Array<int>& xs) {
    parallel_for (uint i : xs.size()) {
        xs[i] = xs[i] + bias + 10;
    }
}
void run_scalar_val_capture(Array<int>& xs) {
    add_val_bias(xs[0], xs);
}
)";
        using IntView = std::span<const std::int32_t>;
        auto view_type = NativeRegistration<IntView>(runtime, "test.IntView", TypeKind::array).build();
        view_type.element = TypeReference::native<std::int32_t>();
        view_type.native.element_representation = detail::representation<std::int32_t>();
        view_type.native.sequence_view = [](const void* value, const void** data, std::size_t* size) noexcept {
            const auto view = *static_cast<const IntView*>(value);
            *data = view.data();
            *size = view.size();
        };
        take(runtime.register_type(std::move(view_type)));
        const std::string_view view_src = R"(
void increment(const test.IntView& input, test.IntArray& output) {
    parallel_for (uint i : output.size()) { output[i] = input[i] + 1; }
}
)";
        for (const bool debug_mode : {false, true}) {
            const source::CompileOptions opts{.debugging = debug_mode};
            auto rec_prog = take(Program::create(runtime));
            publish_single(*rec_prog, "rec_overlap.source", alias_parity_src, "trigger_record_overlap", opts);
            Array<PlainPair> rows{{1, 0}, {1, 0}};
            const std::array rec_args{Argument::borrow(rows)};
            const auto rec_res = rec_prog->entry().invoke(ObjectId{}, rec_args);
            require(!rec_res && rec_res.error().code == ErrorCode::invocation_failed &&
                        rec_res.error().message.contains("borrowed argument"),
                    "Record borrow overlapping writable Array was not rejected with parallel_alias.");

            auto ref_prog = take(Program::create(runtime));
            publish_single(*ref_prog, "ref_overlap.source", alias_parity_src, "trigger_scalar_ref_overlap", opts);
            Array<std::int32_t> ref_xs{1, 1};
            const std::array ref_args{Argument::borrow(ref_xs)};
            const auto ref_res = ref_prog->entry().invoke(ObjectId{}, ref_args);
            require(!ref_res && ref_res.error().code == ErrorCode::invocation_failed &&
                        ref_res.error().message.contains("borrowed argument"),
                    "Scalar const-ref borrow overlapping writable Array diverged or was not rejected.");

            auto val_prog = take(Program::create(runtime));
            publish_single(*val_prog, "val_capture.source", alias_parity_src, "run_scalar_val_capture", opts);
            Array<std::int32_t> val_xs{1, 1};
            const std::array val_args{Argument::borrow(val_xs)};
            take(val_prog->entry().invoke(ObjectId{}, val_args));
            require(val_xs[0] == 12 && val_xs[1] == 12,
                    "By-value scalar capture produced different results between normal and debug builds.");

            auto view_prog = take(Program::create(runtime));
            publish_single(*view_prog, "view_overlap.source", view_src, "increment", opts);
            Array<std::int32_t> xs{3, 7, 11};
            const IntView full{xs.data(), xs.size()};
            const auto increment = [&](const IntView& input) {
                const std::array args{Argument::borrow(input), Argument::borrow(xs)};
                return view_prog->entry().invoke(ObjectId{}, args);
            };
            take(increment(full));
            require(xs[0] == 4 && xs[1] == 8 && xs[2] == 12,
                    "Same-element projections over the full buffer failed to increment in place.");
            for (const auto partial : {full.first(2), full.subspan(1)}) {
                const auto rejected = increment(partial);
                require(!rejected && rejected.error().code == ErrorCode::invocation_failed &&
                            rejected.error().message.contains("overlaps") && xs[0] == 4 && xs[1] == 8 && xs[2] == 12,
                        "Partially overlapping projections must fail before the first write.");
            }
        }

        // (b) 4096-element helper call: `sequence_view` projected ONLY twice on serial thread (0 in worker tasks),
        //     and `source_call` count equals 4096 in both parallel and debug-serial modes.
        const std::string_view proj_src = R"(
int sample_lut(const Array<int>& lut, uint idx) {
    return lut[idx & 3];
}
void run_helper_proj(const Array<int>& lut, Array<int>& out) {
    parallel_for (uint i : out.size()) {
        out[i] = sample_lut(lut, i);
    }
}
)";
        Array<std::int32_t> proj_lut{10, 20, 30, 40};
        Array<std::int32_t> proj_out;
        proj_out.resize(4096);
        for (const bool debug_mode : {false, true}) {
            auto proj_prog = take(Program::create(runtime));
            publish_single(*proj_prog, "proj.source", proj_src, "run_helper_proj",
                           {.diagnostics = true, .debugging = debug_mode});
            source::Diagnostics diag;
            const std::array args{Argument::borrow(std::as_const(proj_lut)), Argument::borrow(proj_out)};
            take(proj_prog->entry().invoke(ObjectId{}, args, &diag));
            const auto snap = diag.snapshot();
            require(snap.calls[source::DiagnosticCall::source_call] == 4096,
                    "Parallel source_call diagnostics were lost or mismatched debug mode.");
            if (!debug_mode) {
                require(
                    snap.calls[source::DiagnosticCall::sequence_view] == 2,
                    "Parallel helper re-projected sequence_view per element instead of reusing pre-projected view.");
            }
            require(proj_out[0] == 10 && proj_out[4095] == 40, "Parallel helper output mismatch.");
        }
        {
            const std::string_view shared_helper_src = R"(
int make_pair_sum(int v) {
    test.PlainPair p;
    p.x = v;
    p.y = v + 1;
    return p.x + p.y;
}
void unused_parallel(Array<int>& out) {
    parallel_for (uint i : out.size()) {
        out[i] = make_pair_sum(int(i));
    }
}
int serial_caller(int v) {
    return make_pair_sum(v);
}
)";
            auto serial_prog = take(Program::create(runtime));
            publish_single(*serial_prog, "shared_helper.source", shared_helper_src, "serial_caller",
                           {.diagnostics = true});
            source::Diagnostics diag;
            const std::array args{Scalar{std::int32_t{5}}};
            const auto res = take(serial_prog->entry().invoke(ObjectId{}, args, &diag));
            const auto snap = diag.snapshot();
            require(res && std::get<std::int32_t>(*res) == 11 && snap.peak_owned_values == 1 &&
                        snap.calls[source::DiagnosticCall::value_construct] == 1,
                    "Unreachable or parallel-shared helper suppressed serial OwnedValue diagnostics.");
        }

        // (c) Empty-body `parallel_for` under Debugger stops with `i == 0` in scope and supports immediate `abort`.
        {
            const std::string_view empty_loop_src = "void spin() { parallel_for (uint i : 1000000) {} }";
            auto spin_prog = take(Program::create(runtime));
            publish_single(*spin_prog, "spin.source", empty_loop_src, "spin", {.debugging = true});
            std::uint32_t header_stops = 0;
            bool i_in_scope_at_header = false;
            source::Debugger abort_dbg{[&](const source::DebugSnapshot& stop) noexcept -> Result<source::DebugCommand> {
                if (!stop.frames.empty()) {
                    for (const auto& loc : stop.frames.front().locals) {
                        if (loc.name == "i" && loc.state == source::DebugValueState::available && loc.scalar &&
                            std::get<std::uint32_t>(*loc.scalar) == 0u) {
                            i_in_scope_at_header = true;
                            ++header_stops;
                            return stop.command(source::DebugAction::abort);
                        }
                    }
                }
                return stop.command(source::DebugAction::step_into);
            }};
            const auto aborted = spin_prog->entry().invoke(ObjectId{}, std::span<const Scalar>{}, nullptr, &abort_dbg);
            require(!aborted && aborted.error().code == ErrorCode::execution_aborted && i_in_scope_at_header &&
                        header_stops == 1,
                    "Debugger did not expose `i` at parallel_for header or failed to abort empty parallel_for.");
        }

        // (d) Embedded NUL in `fail("A\0B")` preserves all 3 bytes across sync, stateless-direct, and Debugger stops;
        //     and >512-byte multi-byte UTF-8 `fail("...")` in an `async` coroutine preserves full valid UTF-8 text.
        {
            const std::string expected_nul("A\0B", 3);
            const std::string_view nul_src = R"(void fail_nul() { fail("A\0B"); })";
            auto nul_prog = take(Program::create(runtime));
            publish_single(*nul_prog, "nul.source", nul_src);
            const auto sync_nul = nul_prog->entry().invoke(ObjectId{}, std::span<const Scalar>{});
            require(!sync_nul && sync_nul.error().message.size() == 3 && sync_nul.error().message == expected_nul,
                    "Sync fail(...) truncated at embedded NUL byte.");
            const auto direct_nul = take(nul_prog->entry().bind<void>()).invoke(ObjectId{});
            require(!direct_nul && direct_nul.error().message == expected_nul,
                    "Stateless direct fail(...) truncated at embedded NUL byte.");

            auto dbg_nul_prog = take(Program::create(runtime));
            publish_single(*dbg_nul_prog, "dbg_nul.source", nul_src, "fail_nul",
                           {.diagnostics = true, .debugging = true});
            std::string stopped_msg;
            source::Debugger nul_dbg{[&](const source::DebugSnapshot& stop) noexcept -> Result<source::DebugCommand> {
                if (stop.reason == source::DebugStopReason::error && stop.error)
                    stopped_msg = stop.error->message;
                return stop.command(source::DebugAction::continue_execution);
            }};
            source::Diagnostics diagnostics;
            const auto dbg_nul =
                dbg_nul_prog->entry().invoke(ObjectId{}, std::span<const Scalar>{}, &diagnostics, &nul_dbg);
            const auto nul_report = diagnostics.snapshot();
            require(!dbg_nul && dbg_nul.error().message == expected_nul && stopped_msg == expected_nul &&
                        nul_report.error && nul_report.error->message == expected_nul,
                    "Debug or diagnostics capture truncated fail(...) at an embedded NUL byte.");

            std::string long_utf8;
            for (int k = 0; k < 171; ++k)
                long_utf8 += "\xe9\x9c\x9c"; // 171 * 3 = 513 UTF-8 bytes ("霜" * 171).
            const std::string coro_src = "async void coro_long() { yield; fail(\"" + long_utf8 + "\"); }";
            auto coro_prog = take(Program::create(runtime));
            publish_single(*coro_prog, "coro_long.source", coro_src, "coro_long", {.diagnostics = true});
            auto coro = take(coro_prog->entry().start(ObjectId{}, {}, &diagnostics));
            require(take(coro.resume()) == CoroutineState::suspended, "Async coroutine did not suspend before fail.");
            const auto step2 = coro.resume();
            require(!step2 && step2.error().code == ErrorCode::invocation_failed &&
                        step2.error().message.size() == 513 && step2.error().message == long_utf8,
                    "Async coroutine fail(...) truncated >512-byte UTF-8 message.");
            const auto long_report = diagnostics.snapshot();
            require(long_report.error && long_report.error->message == std::string_view(long_utf8).substr(0, 510),
                    "Diagnostics did not retain the longest complete UTF-8 prefix within 511 bytes.");
        }
    }
}

struct ErgHost {
    std::int32_t scale{9};

    Result<std::int32_t> native_grow(Array<std::int32_t>& xs) const noexcept {
        for (std::int32_t k = 0; k < 32; ++k)
            xs.add(k + 500);
        return static_cast<std::int32_t>(xs.size());
    }

    Result<void> native_fail_at(std::uint32_t index) const noexcept {
        return std::unexpected(Error{ErrorCode::invocation_failed, "native interval failed", std::nullopt,
                                     ErrorElement{"interval", index}});
    }
};

void verify_language_ergonomics() {
    const auto register_erg_host = [](Runtime& rt) {
        take(rt.register_types(
            std::vector<TypeDefinition>{string_definition(rt), array_definition<std::int32_t>(rt, "test.IntArray"),
                                        NativeRegistration<ErgHost>(rt, "test.PairHost", TypeKind::object)
                                            .field<&ErgHost::scale>("scale")
                                            .method<&ErgHost::native_grow>("native_grow", true)
                                            .method<&ErgHost::native_fail_at>("native_fail_at", true)
                                            .build()}));
    };
    Runtime runtime;
    register_erg_host(runtime);
    const auto host_type = take(runtime.find_type("test.PairHost"));
    const auto publish_single = [](Program& prog, std::string_view name, std::string_view src,
                                   std::string_view entry = {}, source::CompileOptions opts = {}) {
        auto req = take(prog.request(name, src, entry, opts));
        auto cand = take(Program::prepare(req));
        take(prog.try_publish(cand));
    };

    // 1. Mutable `Array<T>&`, `String&`, and `Struct&` structural + element mutation, entry view invalidation safety,
    //    automatic `Array<SourceStruct / SourceEnum>` synthesis (including `quaternion` defaults & nested `String`/`Array`),
    //    `parallel_for` over `Array<SourceStruct>`, compound assignments, and ternary `?:`.
    {
        auto host = take(runtime.create(host_type));
        auto host_bump = take(Program::create(runtime, host_type));
        publish_single(*host_bump, "bump_host.source", "int bump() { self.scale += 3; return self.scale; }", "bump");
        const auto bumped = take(host_bump->entry().invoke(host.id(), std::span<const Scalar>{}));
        require(bumped && std::get<std::int32_t>(*bumped) == 12, "Compound assignment on self.scale failed.");

        const std::string_view src = R"(
[Flags]
enum erg.Mask : uint {
    None = 0,
    Alpha = 1,
    Beta = 2,
    Gamma = 4
};

struct erg.Pose {
    vector3 pos;
    quaternion rot;
    erg.Mask flags;
    int score;
};

struct erg.Group {
    String title;
    Array<erg.Pose> poses;
};

const int erg.FOLDED_TRUE = true ? 17 : (1 / 0);
const int erg.FOLDED_CHAIN = false ? 1 : true ? 29 : 3;

void erg.push_pose(Array<erg.Pose>& list, erg.Pose item) {
    list.add(item);
}

void erg.populate_group(erg.Group& g, const String& prefix, int count) {
    g.title = prefix;
    g.title.append(":ok");
    g.poses.clear();
    g.poses.reserve(uint(count));
    for (int i = 0; i < count; i += 1) {
        erg.Pose p;
        p.pos = vector3(float(i), 1.0f, 0.0f);
        p.score = i * 10;
        p.flags = (i & 1) == 0 ? erg.Mask::Alpha : erg.Mask::Beta;
        erg.push_pose(g.poses, p);
    }
}

int erg.next_index(int& counter) {
    counter += 1;
    return counter - 1;
}

int erg.grow_and_value(Array<int>& arr, int& side_effects) {
    side_effects += 10;
    for (int k = 0; k < 64; k += 1) {
        arr.add(k + 100);
    }
    return 25;
}

int erg.sum_ref_and_val(const int& ref_elem, int val) {
    return ref_elem + val;
}

erg.Pose erg.grow_matrix_and_pose(Array<Array<erg.Pose>>& mat) {
    Array<erg.Pose> extra;
    for (int k = 0; k < 16; k += 1) {
        mat.add(extra);
    }
    erg.Pose p;
    p.score = 777;
    return p;
}

int erg.div_zero_trap(int x) {
    return 100 / x;
}

int erg.run_all(Array<int>& caller_arr, String& caller_str) {
    // Read caller_arr[0] BEFORE structural growth inside the same function (`mutates_structure` must not cache stale view).
    int head_before = caller_arr.size() > 0 ? caller_arr[0] : -1;
    for (int i = 0; i < 48; i += 1) {
        caller_arr.add(i + 10);
    }
    // Native BorrowSafe method mutating Array<int>& structure must also keep subsequent caller_arr[i] reads live.
    const int after_native = self.native_grow(caller_arr);
    if (after_native != 49 + 32) {
        fail("Native Array<int>& growth failed.");
    }
    // By-value element copy from the same array across potential reallocation is safe.
    int copied_head = caller_arr[0];
    caller_arr.add(copied_head + 1);

    // Single-evaluation + reallocation-safe compound assignment on indexed Array!
    int idx_calls = 0;
    int rhs_calls = 0;
    caller_arr[erg.next_index(idx_calls)] += erg.grow_and_value(caller_arr, rhs_calls);
    if (idx_calls != 1 || rhs_calls != 10 || caller_arr[0] != head_before + 25) {
        fail("Compound assignment evaluated LHS more than once or wrote stale reallocated storage.");
    }
    // Cross-argument reallocation safety: earlier const-ref element borrow remains valid when a later argument grows `caller_arr`.
    if (erg.sum_ref_and_val(caller_arr[0], erg.grow_and_value(caller_arr, rhs_calls)) != head_before + 50) {
        fail("Cross-argument sequence reallocation corrupted borrowed element reference.");
    }

    // String& structural mutation and whole-value assignment.
    String base = "atelier";
    caller_str = base;
    caller_str.append("-source");

    // Nested Array<erg.Group> owning String + Array<erg.Pose>, plus Array<Array<erg.Pose>>.
    Array<erg.Group> groups;
    groups.resize(uint(2));
    erg.populate_group(groups[0], caller_str, 6);
    erg.populate_group(groups[1], "second", 4);
    if (groups[0].title.size() != 17 || groups[0].title[15] != 111 || groups[0].poses.size() != 6 ||
        groups[1].poses.size() != 4) {
        fail("Nested synthesized Array<Group> or String&/Struct& mutation failed.");
    }

    Array<Array<erg.Pose>> matrix;
    matrix.add(groups[0].poses);
    matrix.add(groups[1].poses);
    // Receiver-vs-RHS reallocation safety: `matrix[0].add(...)` when RHS reallocates outer `matrix`.
    matrix[0].add(erg.grow_matrix_and_pose(matrix));
    if (matrix.size() != 18 || matrix[0].size() != 7 || matrix[0][6].score != 777 || matrix[1][3].score != 30) {
        fail("Nested synthesized Array<Array<Pose>> failed.");
    }

    // Parallel execution over synthesized Array<erg.Pose> with non-zero quaternion identity prototype!
    Array<erg.Pose> par_poses;
    par_poses.resize(uint(32));
    parallel_for (uint i : par_poses.size()) {
        erg.Pose local_pose;
        // Default quaternion must have w == 1.0f (identity), not all-zero bytes!
        const vector3 rotated = math.rotate(local_pose.rot, vector3(1.0f, 2.0f, 3.0f));
        local_pose.pos = rotated + (i < 16 ? vector3(float(i), 0.0f, 0.0f) : vector3(0.0f, float(i), 0.0f));
        local_pose.score = int(i) + erg.FOLDED_TRUE + erg.FOLDED_CHAIN;
        local_pose.flags = erg.Mask::Alpha;
        local_pose.flags |= (i & 1) == 1 ? erg.Mask::Gamma : erg.Mask::None;
        par_poses[i] = local_pose;
    }
    if (par_poses[0].pos != vector3(1.0f, 2.0f, 3.0f) || par_poses[5].score != 5 + 17 + 29 ||
        par_poses[5].flags != (erg.Mask::Alpha | erg.Mask::Gamma)) {
        fail("parallel_for over synthesized Array<Pose> produced unexpected values.");
    }

    // Compound operators & short-circuit ternary expressions (scalar, vector, quaternion, plain struct).
    int math_acc = 20;
    math_acc -= 5;
    math_acc *= 4;
    math_acc /= 3;
    math_acc %= 13;
    uint bits = uint(3);
    bits <<= 2;
    bits |= uint(1);
    bits ^= uint(5);
    bits &= uint(15);
    bits >>= 1;

    vector3 v = vector3(2.0f, 4.0f, 6.0f);
    v += vector3(1.0f, 2.0f, 3.0f);
    v -= vector3(1.0f, 0.0f, 1.0f);
    v *= 0.5f;
    v /= 2.0f;

    quaternion q = quaternion();
    q *= quaternion();

    const int safe_a = true ? 10 : erg.div_zero_trap(0);
    const int safe_b = false ? erg.div_zero_trap(0) : 22;
    const int chosen_score = (safe_a < safe_b ? par_poses[2] : par_poses[0]).score;

    return math_acc + int(bits) + int(v.y * 2.0f) + int(q.w) + safe_a + safe_b + chosen_score + self.scale;
}
)";
        for (const bool fast : {false, true}) {
            Runtime tier_rt;
            register_erg_host(tier_rt);
            const auto tier_host_type = take(tier_rt.find_type("test.PairHost"));
            auto tier_host = take(tier_rt.create(tier_host_type));
            auto prog = take(Program::create(tier_rt, tier_host_type));
            publish_single(*prog, "ergonomics.source", src, "erg.run_all", {.diagnostics = true, .fast = fast});

            // Exercise the Runtime storage synthesized by the just-published Source group.
            const auto group = take(tier_rt.describe(take(tier_rt.find_type("erg.Group"))));
            const auto poses_field = std::ranges::find(group->fields, "poses", &FieldInfo::name);
            require(poses_field != group->fields.end(), "Missing synthesized pose array field.");
            const auto poses_type = poses_field->value.type;
            const auto poses_info = take(tier_rt.describe(poses_type));
            alignas(std::max_align_t) std::array<std::byte, 64> storage{};
            require(poses_info->size <= storage.size() && poses_info->alignment <= alignof(std::max_align_t),
                    "Acceptance storage cannot hold the synthesized array header.");
            take(tier_rt.initialize_value(poses_type, storage));
            take(tier_rt.mutate_sequence(poses_type, storage, SequenceMutation::reserve, {}, {}, 8));
            take(tier_rt.mutate_sequence(poses_type, storage, SequenceMutation::resize, {}, {}, 2));
            const void* before{};
            const void* after{};
            std::size_t count{};
            poses_info->sequence_view(storage.data(), &before, &count);
            take(tier_rt.mutate_sequence(poses_type, storage, SequenceMutation::append, poses_type, storage));
            poses_info->sequence_view(storage.data(), &after, &count);
            require(before == after && count == 4, "Reserved Source array self-append relocated its live elements.");
            take(tier_rt.destroy_value(poses_type, storage));

            Array<std::int32_t> arr{7};
            String str = String::literal<"init">();
            source::Diagnostics diag;
            const std::array args{Argument::borrow(arr), Argument::borrow(str)};
            const auto res = take(prog->entry().invoke(tier_host.id(), args, &diag));
            const auto snap = diag.snapshot();
            // math_acc: 7, bits: 4, int(v.y*2): 3, q.w: 1, safe_a: 10, safe_b: 22, chosen_score: 48, self.scale: 9 -> 104
            require(res && std::get<std::int32_t>(*res) == 104, "Ergonomics end-to-end result mismatch.");
            require(arr[0] == 32 && arr.size() == 1 + 48 + 32 + 1 + 64 + 64 &&
                        str.view() == StringView{"atelier-source"},
                    "Caller Array<int>& or String& mutations did not persist.");
            require(snap.remaining_owned_values == 0 && snap.resources_cleared,
                    "Synthesized Array<Group> leaked owned values.");
        }
    }

    // 2. Hot-reload compatibly synthesizes `Array<ExistingSourceStruct>` when newly used in a function body,
    //    and SOP `[InOut]`/`[Out]` fixed-length columns allow element-only helpers while rejecting structural helpers.
    {
        source::SourceSnapshot v1;
        v1.name = "HotArray";
        v1.version = 1;
        v1.files.push_back({"a.source", 1, R"(
struct hot.Item {
    int id;
    quaternion q;
};

void hot.scale_elements(Array<int>& col) {
    for (uint i = 0; i < col.size(); i += 1) {
        col[i] *= 2;
    }
}

class hot.Op : test.PairHost {
    void process([InOut] Array<int>& col) {
        hot.scale_elements(col);
    }
}

int hot.run() {
    hot.Item single;
    single.id = 5;
    return single.id + int(single.q.w);
}
)"});
        auto mod_prog = take(Program::create(runtime));
        auto c1 = take(Program::prepare(take(mod_prog->request(v1))));
        take(mod_prog->try_publish(c1));
        const auto r1 = take(take(mod_prog->entry("hot.run")).invoke(ObjectId{}, std::span<const Scalar>{}));
        require(r1 && std::get<std::int32_t>(*r1) == 6, "HotArray v1 result mismatch.");

        // Verify SOP [InOut] column can call element-only `Array<int>&` helper.
        auto op_inst = take(runtime.create(mod_prog->classes()[0].type));
        Array<std::int32_t> sop_col{3, 4, 5};
        const std::array sop_args{Argument::borrow(sop_col)};
        (void)take(take(mod_prog->entry("hot.Op.process")).invoke(op_inst.id(), sop_args));
        require(sop_col.size() == 3 && sop_col[0] == 6 && sop_col[2] == 10,
                "SOP element-only Array<int>& helper failed.");

        // Version 2: same struct declaration, newly spells `Array<hot.Item>` inside `hot.run` + `parallel_for`!
        source::SourceSnapshot v2 = v1;
        v2.version = 2;
        v2.files[0].version = 2;
        v2.files[0].source = R"(
struct hot.Item {
    int id;
    quaternion q;
};

void hot.scale_elements(Array<int>& col) {
    for (uint i = 0; i < col.size(); i += 1) {
        col[i] *= 2;
    }
}

class hot.Op : test.PairHost {
    void process([InOut] Array<int>& col) {
        hot.scale_elements(col);
    }
}

int hot.run() {
    Array<hot.Item> items;
    items.resize(uint(8));
    parallel_for (uint i : items.size()) {
        hot.Item it;
        it.id = int(i) + 1;
        items[i] = it;
    }
    return items[7].id + int(items[7].q.w);
}
)";
        auto c2 = take(Program::prepare(take(mod_prog->request(v2))));
        take(mod_prog->try_publish(c2));
        const auto r2 = take(take(mod_prog->entry("hot.run")).invoke(ObjectId{}, std::span<const Scalar>{}));
        require(r2 && std::get<std::int32_t>(*r2) == 9, "Hot-reloaded synthesized Array<hot.Item> failed.");
    }

    // 3. Negative compile-time boundary checks for const deep read-only, SOP fixed-length, call-site aliasing,
    //    compound assignments, and ternary expressions.
    {
        const auto reject_single = [&](std::string_view code, std::string_view expected) {
            auto bad_prog = take(Program::create(runtime, host_type));
            auto bad_req = take(bad_prog->request("neg_erg.source", code, "f"));
            auto bad_cand = Program::prepare(bad_req);
            require(!bad_cand && bad_cand.error().message.contains(expected),
                    "Expected diagnostic not produced for negative ergonomics test.");
        };
        reject_single("void f(const Array<int>& a) { a.add(1); }", "Structural mutation requires a mutable");
        reject_single("void f(const Array<int>& a) { a[0] = 1; }", "Assignment requires a writable local or field");
        reject_single("void f(const String& s) { s.append(\"x\"); }", "Structural mutation requires a mutable");
        reject_single("void push_bad(Array<int>& a, const int& r) { a.add(r); } "
                      "void f(Array<int>& a) { push_bad(a, a[0]); }",
                      "Cannot borrow 'a' through another reference parameter in the same call");
        reject_single("void f() { float s = 2.0f; s *= vector3(1.0f, 2.0f, 3.0f); }",
                      "Compound assignment result type does not match");
        reject_single("int GetVal() { return 1; } void SetVal(int v) {} void f() { Val += 1; }",
                      "Compound assignment is not supported on Get/Set properties");
        reject_single("int f() { return 1 ? 2 : 3; }", "Conditional expression requires a bool condition");
        reject_single("float f(bool c) { return c ? 1 : 1.0f; }", "Conditional branches require matching value types");
        reject_single("void f(bool c, const Array<int>& a, const Array<int>& b) { uint n = (c ? a : b).size(); }",
                      "Conditional branches require matching scalar, enum, built-in math or plain struct value types");

        const auto reject_mod = [&](std::string_view file_src, std::string_view expected) {
            source::SourceSnapshot bad_snap;
            bad_snap.name = "NegSOP";
            bad_snap.version = 1;
            bad_snap.files.push_back({"sop.source", 1, std::string(file_src)});
            auto p = take(Program::create(runtime));
            auto r = take(p->request(bad_snap));
            auto c = Program::prepare(r);
            require(!c && c.error().message.contains(expected), "Expected SOP fixed-length rejection.");
        };
        reject_mod("class s.A : test.PairHost { void process([InOut] Array<int>& col) { col.add(1); } }",
                   "fixed element count and cannot change structure");
        reject_mod("class s.B : test.PairHost { void process([Out] Array<int>& col) { Array<int> o; col = o; } }",
                   "fixed element count and cannot change structure");
        reject_mod("void grow(Array<int>& x) { x.resize(uint(4)); } "
                   "class s.C : test.PairHost { void process([InOut] Array<int>& col) { grow(col); } }",
                   "fixed element count and cannot change structure");
        reject_mod(
            "class s.D : test.PairHost { void process([InOut] Array<int>& col) { int n = self.native_grow(col); } }",
            "fixed element count and cannot change structure");
        reject_single("void f() { fail(\"bad\", 1.5f); }", "fail element index requires uint or int");
        reject_mod("[Flags] enum s.M : uint { A = 1 } void s.f() { fail(\"bad\", s.M::A, 0u); }",
                   "fail element layer requires a uint-backed non-flags enum");
        reject_single("const Array<int> TAB = { 1, 2 }; void f() { TAB.add(3); }",
                      "Structural mutation requires a mutable");
        reject_single("const Array<int> TAB = { 1, 2 }; void f() { TAB[0] = 3; }", "Assignment requires a writable");
        reject_single("const vector3 V = vector3(1.0f); void f() { V.x = 2.0f; }", "Assignment requires a writable");
        reject_single("void mut(Array<int>& a) {} const Array<int> TAB = { 1 }; void f() { mut(TAB); }",
                      "Writable reference requires a mutable lvalue");
        reject_mod("[DESC = \"bad\"] const int s.X = 1; void s.f() {}", "Attributes do not apply to module constants");
        reject_single("const int X = { 1 }; void f() {}",
                      "Brace initializers require a plain struct or constant Array");
    }

    // 4. Structured fail("msg", [layer,] index), sentinel omission, DirectScope, Coroutine, parallel_for, and NativeFailure.
    {
        const std::string_view diag_src = R"(
enum diag.Domain : uint {
    Point = 10,
    Primitive = 25,
    Interval = 40
};

int diag.fail_two(int idx) {
    fail("two-arg failure", idx);
}

int diag.fail_three(diag.Domain dom, uint idx) {
    fail("three-arg failure", dom, idx);
}

int diag.fail_parallel(diag.Domain dom, uint target) {
    Array<int> out;
    out.resize(4096u);
    parallel_for (uint i : out.size()) {
        if (i == target) {
            fail("parallel chunk failure", dom, i);
        }
        out[i] = int(i);
    }
    return out[0];
}

class diag.Runner : test.PairHost {
    int call_native(uint idx) {
        self.native_fail_at(idx);
        return 0;
    }
}
)";
        Runtime diag_rt;
        register_erg_host(diag_rt);
        auto diag_prog = take(Program::create(diag_rt));
        source::SourceSnapshot snap{"DiagBatch3", 1, {{"Source/Diag.source", 11, std::string(diag_src)}}};
        auto cand = take(Program::prepare(take(diag_prog->request(snap))));
        take(diag_prog->try_publish(cand));

        // 2-arg valid index (layer empty at source level), negative index (element omitted), 0xffffffffu sentinel (omitted):
        const auto f2 = take(diag_prog->entry("diag.fail_two"));
        const auto typed_f2 = take(f2.bind<std::int32_t, std::int32_t>());
        const auto e_valid = typed_f2.invoke(ObjectId{}, 17);
        require(!e_valid && e_valid.error().message == "two-arg failure" && e_valid.error().source.has_value() &&
                    e_valid.error().element == ErrorElement{"", 17u},
                "DirectScope 2-arg fail did not report element index 17 with source location.");
        const auto e_neg = typed_f2.invoke(ObjectId{}, -3);
        require(!e_neg && e_neg.error().message == "two-arg failure" && !e_neg.error().element.has_value(),
                "Negative fail index must omit Error::element while keeping message.");

        const auto f3 = take(diag_prog->entry("diag.fail_three"));
        const auto dom_type = take(diag_rt.find_type("diag.Domain"));
        const std::array<Scalar, 2> args_3{EnumValue{dom_type, 25u}, std::uint32_t{42u}};
        const auto e_three = f3.invoke(ObjectId{}, args_3);
        require(!e_three && e_three.error().message == "three-arg failure" &&
                    e_three.error().element == ErrorElement{"Primitive", 42u},
                "3-arg fail did not decode non-contiguous enum layer name 'Primitive'.");
        const std::array<Scalar, 2> args_sentinel{EnumValue{dom_type, 25u}, std::uint32_t{0xffffffffu}};
        const auto e_sentinel = f3.invoke(ObjectId{}, args_sentinel);
        require(!e_sentinel && e_sentinel.error().message == "three-arg failure" &&
                    !e_sentinel.error().element.has_value(),
                "0xffffffffu sentinel fail index must omit Error::element while keeping message.");

        // Coroutine resume preserves element & location (single-entry async Program):
        auto coro_prog = take(Program::create(diag_rt));
        publish_single(
            *coro_prog, "coro_diag.source",
            "async int fail_coro(diag.Domain dom, uint idx) { yield; fail(\"coro element failure\", dom, idx); }",
            "fail_coro");
        const std::array<Scalar, 2> coro_args{EnumValue{dom_type, 25u}, std::uint32_t{9u}};
        auto coro = take(coro_prog->entry().start(ObjectId{}, coro_args));
        require(take(coro.resume()) == CoroutineState::suspended, "fail_coro did not suspend.");
        const auto coro_failed = coro.resume();
        require(!coro_failed && coro_failed.error().message == "coro element failure" &&
                    coro_failed.error().source.has_value() &&
                    coro_failed.error().element == ErrorElement{"Primitive", 9u},
                "Coroutine fail(...) did not preserve ErrorElement and SourceLocation.");

        // parallel_for captures outer enum + local and atomically records 16B first failure:
        const auto fp = take(diag_prog->entry("diag.fail_parallel"));
        const std::array<Scalar, 2> par_args{EnumValue{dom_type, 40u}, std::uint32_t{2500u}};
        const auto par_failed = fp.invoke(ObjectId{}, par_args);
        require(!par_failed && par_failed.error().message == "parallel chunk failure" &&
                    par_failed.error().source.has_value() &&
                    par_failed.error().element == ErrorElement{"Interval", 2500u},
                "parallel_for fail(...) did not preserve captured layer and element index 2500.");

        // Native BorrowSafe failure carries ErrorElement across NativeFailure:
        const auto runner_type = take(diag_rt.find_type("diag.Runner"));
        auto runner_obj = take(diag_rt.create(runner_type));
        const auto fn_nat = take(diag_prog->entry("diag.Runner.call_native"));
        const std::array<Scalar, 1> nat_args{std::uint32_t{17u}};
        const auto nat_failed = fn_nat.invoke(runner_obj.id(), nat_args);
        require(!nat_failed && nat_failed.error().message == "native interval failed" &&
                    nat_failed.error().source.has_value() &&
                    nat_failed.error().element == ErrorElement{"interval", 17u},
                "Native BorrowSafe failure did not preserve ErrorElement across NativeFailure.");
    }

    // 9. Batch 4: .rdata zero-allocation constant tables (math, plain struct, Array<T>),
    //    [DESC] on struct/enum/field/function, Program::functions(), and DESC-only hot reload:
    {
        constexpr std::string_view batch4_src_v1 = R"(
[DESC = "Station sample record."]
struct tab.Station {
    [DESC = "Height in metres."]
    float y;
    [DESC = "Radius in metres."]
    float radius;
    [DESC = "Section tag."]
    uint tag;
};

[Flags, DESC = "Bitmask flags with type-level DESC."]
enum tab.Mask : uint {
    None = 0u,
    Cap = 1u,
    Bevel = 2u
};

const vector3 tab.UP = vector3(0.0f, 1.0f, 0.0f);
const quaternion tab.ID_ROT = quaternion();
const tab.Station tab.DEFAULT_STATION = { 0.25f, 0.12f, 7u, };
const Array<vector2> tab.PROFILE = {
    vector2(0.10f, 0.13f),
    vector2(0.20f, 0.15f),
    vector2(0.40f, 0.11f),
    vector2(tab.DEFAULT_STATION.y * 2.0f, tab.DEFAULT_STATION.radius + 0.02f),
};
const Array<tab.Station> tab.STATIONS = {
    { 0.1f, 0.2f, 1u },
    tab.DEFAULT_STATION,
    { 0.8f, 0.3f, 9u, },
};
const Array<int> tab.EMPTY = {};
const uint tab.PROFILE_LEN = tab.PROFILE.size();
const uint tab.DEG_ONE_BITS = math.as_uint(math.degrees(1.0f));
const float tab.CLAMP_NAN = math.clamp(0.0f / 0.0f, 0.25f, 1.0f);
const float tab.MIN_NAN2 = math.min(1.5f, 0.0f / 0.0f);
const float tab.STEP_NAN = math.step(0.5f, 0.0f / 0.0f);
const float tab.SSTEP_DEG = math.smoothstep(1.0f, 1.0f, 1.0f);
const float tab.BIG_DIST = math.distance(vector3(1e25f, 0.0f, 0.0f), vector3(-1e25f, 0.0f, 0.0f));
const vector3 tab.NAN_NORM = math.normalize_or_zero(vector3(0.0f / 0.0f, 1.0f, 0.0f));
const vector3 tab.ROT_X = math.rotate(quaternion(0.0f, 0.0f, 0.70710678f, 0.70710678f), vector3(1.0f, 0.0f, 0.0f));

[DESC = "Initial docstring v1."]
float tab.read_profile_radius(uint idx) {
    if (tab.DEG_ONE_BITS != 0x42652ee1u || tab.CLAMP_NAN != 0.25f || tab.MIN_NAN2 != 1.5f ||
        tab.STEP_NAN != 1.0f || tab.SSTEP_DEG != 0.0f || tab.BIG_DIST < 1.99e25f ||
        tab.NAN_NORM != vector3(0.0f) || math.abs(tab.ROT_X.y - 1.0f) > 1e-5f) {
        fail("Constant-folding math parity check failed.");
    }
    return tab.PROFILE[idx].y + tab.UP.y * 0.0f + tab.ID_ROT.w * 0.0f + float(tab.EMPTY.size());
}

[DESC = "Parallel helper reading constant tables."]
float tab.eval_station(const Array<vector2>& prof, uint i) {
    const uint k = i % prof.size();
    const tab.Station st = tab.STATIONS[i % tab.STATIONS.size()];
    return prof[k].x + prof[k].y + st.radius + float(st.tag);
}

void tab.mutate_vec(Array<float>& out, float v) {
    out.add(v);
}

[DESC = "Parallel consumer over .rdata constant tables."]
double tab.parallel_sum(uint n) {
    Array<float> acc;
    acc.resize(n);
    parallel_for (uint i : n) {
        acc[i] = tab.eval_station(tab.PROFILE, i) + tab.PROFILE[i % tab.PROFILE_LEN].x;
    }
    double total = 0.0d;
    for (uint i = 0u; i < n; i += 1u) {
        total += double(acc[i]);
    }
    return total;
}
)";
        Runtime b4_rt;
        register_erg_host(b4_rt);
        take(b4_rt.register_types(
            std::vector<TypeDefinition>{array_definition<float>(b4_rt, "test.FloatArray"),
                                        array_definition<math::Vector2>(b4_rt, "test.Vec2Array")}));
        auto b4_prog = take(Program::create(b4_rt));
        source::CompileOptions opts{};
        opts.artifacts = true;
        source::SourceSnapshot snap1{"Batch4Const", 1, {{"Source/Tab.source", 1, std::string(batch4_src_v1)}}};
        auto cand1 = take(Program::prepare(take(b4_prog->request(snap1, opts))));
        require(cand1.functions().size() == 4, "Candidate::functions() did not report the 4 global functions.");
        take(b4_prog->try_publish(cand1));

        // Verify struct & enum & struct field DESC in Runtime:
        const auto st_type = take(b4_rt.find_type("tab.Station"));
        const auto* st_info = take(b4_rt.describe(st_type));
        require(st_info->description == "Station sample record." && st_info->fields.size() == 3 &&
                    st_info->fields[0].description == "Height in metres." &&
                    st_info->fields[1].description == "Radius in metres." &&
                    st_info->fields[2].description == "Section tag.",
                "Struct and struct field [DESC] did not publish to Runtime TypeInfo.");
        const auto mask_type = take(b4_rt.find_type("tab.Mask"));
        const auto* mask_info = take(b4_rt.describe(mask_type));
        require(mask_info->description == "Bitmask flags with type-level DESC." && mask_info->enumeration &&
                    mask_info->enumeration->flags,
                "[Flags, DESC] enum did not publish both flags and description.");

        // Verify zero-allocation .rdata IR and zero runtime value/sequence calls on a pure constant Array reader:
        {
            auto pure_const_prog = take(Program::create(b4_rt));
            constexpr std::string_view pure_const_src = R"(
const Array<vector2> PureProf = { vector2(0.10f, 0.13f), vector2(0.20f, 0.15f) };
float read_prof_y(uint idx) {
    return PureProf[idx].y;
}
)";
            auto pure_req = take(pure_const_prog->request("pure_const.source", pure_const_src, "read_prof_y",
                                                          {.diagnostics = true, .artifacts = true}));
            auto pure_cand = take(Program::prepare(pure_req));
            take(pure_const_prog->try_publish(pure_cand));
            const auto pure_ir = take(take(pure_const_prog->pin()).artifacts()).optimized_ir;
            require(pure_ir.contains("@__source_const_data.") && !pure_ir.contains("@business_construct_value") &&
                        !pure_ir.contains("@business_destroy_value") &&
                        !pure_ir.contains("@business_mutate_sequence") && !pure_ir.contains("@business_sequence_view"),
                    "Pure constant Array reader emitted runtime value/sequence services instead of .rdata table.");
            source::Diagnostics pure_diag;
            const std::array pure_args{Scalar{1u}};
            const auto pure_val = take(pure_const_prog->entry().invoke(ObjectId{}, pure_args, &pure_diag));
            const auto pure_snap = pure_diag.snapshot();
            require(pure_val && std::abs(std::get<float>(*pure_val) - 0.15f) < 1e-6f &&
                        pure_snap.peak_owned_values == 0 &&
                        pure_snap.calls[source::DiagnosticCall::value_construct] == 0 &&
                        pure_snap.calls[source::DiagnosticCall::sequence_mutation] == 0 &&
                        pure_snap.calls[source::DiagnosticCall::sequence_view] == 0,
                    "Pure constant Array reader performed runtime value allocation or sequence_view calls.");
        }
        const auto lease = take(b4_prog->pin("tab.read_profile_radius"));
        const auto ir = take(lease.artifacts()).optimized_ir;
        require(ir.contains("@__source_const_data."),
                "Module constant tables did not emit @__source_const_data globals.");

        const auto read_entry = take(b4_prog->entry("tab.read_profile_radius"));
        const auto typed_read = take(read_entry.bind<float, std::uint32_t>());
        require(std::abs(take(typed_read.invoke(ObjectId{}, 0u)) - 0.13f) < 1e-6f &&
                    std::abs(take(typed_read.invoke(ObjectId{}, 3u)) - 0.14f) < 1e-6f,
                "Constant Array folded values did not match expected IEEE floats.");
        const auto oob = typed_read.invoke(ObjectId{}, 4u);
        require(!oob && oob.error().message.contains("out of range"),
                "Out-of-bounds runtime read on constant Array did not fail with bounds error.");

        const auto par_entry = take(b4_prog->entry("tab.parallel_sum"));
        const auto typed_par = take(par_entry.bind<double, std::uint32_t>());
        const double sum_small = take(typed_par.invoke(ObjectId{}, 12u));
        const double sum_large = take(typed_par.invoke(ObjectId{}, 3000u));
        require(std::abs(sum_small - 79.27) < 1e-4 && std::abs(sum_large - sum_small * 250.0) < 1e-3,
                "parallel_for over .rdata constant tables produced unexpected sum.");

        // Verify Program::functions() metadata & DESC-only hot reload compatibility:
        const auto fns_v1 = b4_prog->functions();
        const auto find_fn = [](std::span<const source::FunctionInfo> list, std::string_view name) {
            const auto it = std::ranges::find(list, name, &source::FunctionInfo::name);
            return it == list.end() ? nullptr : &*it;
        };
        const auto* f_read_v1 = find_fn(fns_v1, "tab.read_profile_radius");
        const auto* f_eval = find_fn(fns_v1, "tab.eval_station");
        const auto* f_mut = find_fn(fns_v1, "tab.mutate_vec");
        const auto* f_par = find_fn(fns_v1, "tab.parallel_sum");
        require(f_read_v1 && f_read_v1->description == "Initial docstring v1." && f_read_v1->parallel_safe && f_eval &&
                    f_eval->parallel_safe && f_eval->parameters.size() == 2 &&
                    f_eval->parameters[0].type == "Array<vector2>" &&
                    f_eval->parameters[0].passing == ParameterPassing::read_only && f_mut && !f_mut->parallel_safe &&
                    f_mut->parameters.size() == 2 && f_mut->parameters[0].mutates_structure && f_par &&
                    !f_par->parallel_safe,
                "Program::functions() metadata mismatch.");

        std::string batch4_src_v2(batch4_src_v1);
        const auto doc_pos = batch4_src_v2.find("Initial docstring v1.");
        require(doc_pos != std::string::npos, "Missing v1 docstring.");
        batch4_src_v2.replace(doc_pos, std::string_view{"Initial docstring v1."}.size(), "Updated docstring v2.");
        source::SourceSnapshot snap2{"Batch4Const", 2, {{"Source/Tab.source", 2, std::move(batch4_src_v2)}}};
        auto cand2 = take(Program::prepare(take(b4_prog->request(snap2, opts))));
        take(b4_prog->try_publish(cand2));
        const auto* f_read_v2 = find_fn(b4_prog->functions(), "tab.read_profile_radius");
        require(f_read_v2 && f_read_v2->description == "Updated docstring v2." &&
                    std::abs(take(typed_read.invoke(ObjectId{}, 1u)) - 0.15f) < 1e-6f,
                "Function DESC-only edit broke hot-reload compatibility or did not update Program::functions().");
    }
}
}

void verify_source_projects() {
    verify_project_engine_sources();
    verify_world_project();
    verify_frontend_and_pod_evaluation();
    verify_language_ergonomics();
    std::puts("Source project consumer: PASS (current disk format and cross-file World tick)");
}
