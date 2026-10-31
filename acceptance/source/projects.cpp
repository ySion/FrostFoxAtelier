#include "source/debugger.hpp"
#include "source/program.hpp"
#include "runtime/native.hpp"
#include "runtime/foundation.hpp"
#include "world/world.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <windows.h>

namespace {
using namespace atelier;
using source::Program;
void require(bool value, const char* text) {
    if (!value)
        throw std::runtime_error(text);
}
template <class T>
T take(Result<T> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
    return std::move(*result);
}
void take(Result<void> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
}
namespace w = atelier::world;
source::SourceSnapshot sources(float factor = 1) {
    return {"project",
            5,
            {{"Source/Math.source", 17,
              "float distance(float speed, float dt) { return speed * dt * " + std::to_string(factor) + "f; }"},
             {"Source/Move.source", 23, R"(
class game.Move : world.Component {
    float speed = 1.0f + 1.0f;
    double precise = 2.0d;
    bool enabled = true;
    uint begins = 0;
    int signed_value = int(3);
    void begin() { self.begins = self.begins + 1; }
    void tick(float dt) { self.translate_owner(distance(self.speed,dt),0.0f,0.0f); }
    void end() { self.enabled = false; }
}
)"}}};
}
void publish(Program& program, source::SourceSnapshot snapshot, source::CompileOptions options = {}) {
    auto candidate = take(Program::prepare(take(program.request(std::move(snapshot), options))));
    take(program.try_publish(candidate));
    require(!candidate, "Published project candidate retained an independently publishable result.");
}
void verify_declaration_only_project() {
    Runtime runtime;
    take(w::register_types(runtime));
    Program program(runtime);
    source::SourceSnapshot input{
        "data",
        1,
        {{"Data.source", 7,
          "const float bad = missing; class game.Settings : world.Component { float speed = 2.0f; }"}}};
    auto rejected = Program::prepare(take(program.request(input)));
    require(!rejected && rejected.error().source && rejected.error().source->path == "Data.source" &&
                !runtime.find_type("game.Settings"),
            "Declaration-only project bypassed typed validation or partially published.");
    input.files[0].source =
        "const float __source_validate = 1.0f; class game.Settings : world.Component { float speed = -0.0f; double value = 1.0d / 3.0d; }";
    publish(program, input);
    require(program.entries().empty() && program.classes().size() == 1,
            "Declaration-only project exported a synthetic entry.");
    const auto type = program.classes()[0].type;
    auto instance = take(runtime.create(type));
    const auto speed = take(runtime.find_field(type, "speed"));
    require(std::signbit(std::get<float>(take(runtime.read(instance.id(), speed)))) &&
                std::get<double>(take(runtime.read(instance.id(), take(runtime.find_field(type, "value"))))) ==
                    1.0 / 3.0,
            "Runtime scalar defaults lost their floating representation.");
    publish(program, input); // The same representation stays compatible.
    input.files[0].source.replace(input.files[0].source.find("-0.0f"), 5, "0.0f");
    rejected = Program::prepare(take(program.request(input)));
    require(!rejected && rejected.error().code == ErrorCode::rebuild_required,
            "Changed signed-zero default was silently ignored.");
    input.files[0].path = "Data.source/";
    require(!Program::prepare(take(program.request(input))), "A directory-like source path was accepted.");
    Program phases(runtime);
    source::SourceSnapshot staged{
        "phases",
        1,
        {{"Phases.source", 1,
          "class game.Phases : world.Component { void post_physics_tick(float dt) {} void late_tick() {} }"}}};
    rejected = Program::prepare(take(phases.request(staged)));
    require(!rejected && rejected.error().code == ErrorCode::invalid_definition,
            "A phase lifecycle method accepted a signature other than void(float).");
    staged.files[0].source.replace(staged.files[0].source.find("late_tick()"), 11, "late_tick(float dt)");
    publish(phases, staged);
    require(phases.entries().size() == 2, "Phase lifecycle methods were not exported.");
}

struct CollectionProbe {
    Runtime* runtime{};
    bool collecting() const { return runtime->collection_phase() != CollectionPhase::idle; }
};
struct RangedBody {
    float speed{1.0f};
    float loose{};
    math::Vector3 tint{0.5f, 0.5f, 0.5f};
    void bump(float& value) { value += 1.0f; }
};
void verify_project_field_ranges() {
    Runtime runtime;
    const auto base = take(
        runtime.register_type(NativeRegistration<RangedBody>(runtime, "project.RangedBody", TypeKind::object)
                                  .field<&RangedBody::speed>("speed")
                                  .range({.ui_max = Scalar{5.0f}, .hard_min = Scalar{0.0f}, .hard_max = Scalar{10.0f}})
                                  .field<&RangedBody::loose>("loose")
                                  .field<&RangedBody::tint>("tint")
                                  .range({.hard_min = Scalar{0.0f}, .hard_max = Scalar{1.0f}})
                                  .method<&RangedBody::bump>("bump")
                                  .build()));
    Program program(runtime);
    for (const auto* body : {"self.bump(self.speed);", "self.bump(self.tint.x);"}) {
        source::SourceSnapshot input{
            "ranges",
            1,
            {{"Ranges.source", 1,
              std::string("class game.Ranged : project.RangedBody { void bad() { ") + body + " } }"}}};
        auto failed = Program::prepare(take(program.request(input)));
        require(!failed && failed.error().source && failed.error().message.find("Hard-bounded") != std::string::npos,
                "A hard-bounded field or its component compiled as a writable reference argument.");
    }
    publish(program, {"ranges", 2, {{"Ranges.source", 2, R"(
class game.Ranged : project.RangedBody {
    void push() { self.speed = 50.0f; self.tint.x = -1.0f; self.bump(self.loose); }
    void whole() { self.tint = vector3(2.0, 0.25, -3.0); }
    void poison() { self.speed = 0.0f / 0.0f; }
}
)"}}});
    auto object = take(runtime.create(program.classes()[0].type));
    const auto id = object.id();
    const std::array speed{take(runtime.find_field(base, "speed"))}, loose{take(runtime.find_field(base, "loose"))};
    const auto tint = take(runtime.find_field(base, "tint"));
    const auto vector3 = take(runtime.native_type<math::Vector3>());
    const std::array tint_path{tint};
    const std::array tint_x{tint, take(runtime.find_field(vector3, "x"))};
    take(take(program.entry("game.Ranged.push")).invoke(id, {}));
    require(take(runtime.read(id, speed)) == Scalar{10.0f} && take(runtime.read(id, tint_x)) == Scalar{0.0f} &&
                take(runtime.read(id, loose)) == Scalar{1.0f},
            "Source field writes bypassed hard bounds or broke unconstrained borrows.");
    take(take(program.entry("game.Ranged.whole")).invoke(id, {}));
    math::Vector3 stored;
    take(runtime.read(id, tint_path, ValueOutput{vector3, &stored}));
    require(stored == math::Vector3{1.0f, 0.25f, 0.0f}, "A whole Source vector write bypassed per-component bounds.");
    const auto poisoned = take(program.entry("game.Ranged.poison")).invoke(id, {});
    require(!poisoned && poisoned.error().source && take(runtime.read(id, speed)) == Scalar{10.0f},
            "A Source NaN write reached a hard-bounded field or lost its failure site.");
}
void verify_project_borrow_effects() {
    Runtime runtime;
    take(runtime.register_types(
        {string_definition(runtime), NativeRegistration<CollectionProbe>(runtime, "project.Probe", TypeKind::object)
                                         .method<&CollectionProbe::collecting>("collecting", true)
                                         .build()}));
    Program program(runtime);
    source::SourceSnapshot input{"effects", 1, {{"Effects.source", 1, R"(
class game.Probe : project.Probe {
    bool inspect() { return self.collecting(); }
    bool borrowed(const String& label) { return inspect(); }
    bool work() {
        uint i = 0;
        while (i < 512) { inspect(); i = i + 1; }
        return inspect();
    }
}
)"}}};
    publish(program, input);
    const auto type = program.classes()[0].type;
    std::vector<ObjectRoot> objects;
    for (int i = 0; i < 64; ++i)
        objects.push_back(take(runtime.create(type)));
    const auto id = objects.front().id();
    take(runtime.with_native<CollectionProbe>(id, [&](auto& probe) -> Result<void> {
        probe.runtime = &runtime;
        return {};
    }));
    take(runtime.begin_collection());
    String label("borrowed");
    const std::array args{Argument::borrow(std::as_const(label))};
    require(std::get<bool>(*take(take(program.entry("game.Probe.borrowed")).invoke(id, args))) &&
                runtime.collection_phase() == CollectionPhase::roots,
            "Shared helper advanced GC inside a borrowed-value entry.");
    require(std::get<bool>(*take(take(program.entry("game.Probe.work")).invoke(id, {}))) &&
                runtime.collection_phase() == CollectionPhase::roots,
            "Ordinary entry or its helper advanced GC inside the execution interval.");
    while (take(runtime.collect_step({4096})).phase != CollectionPhase::idle) {
    }
}
struct CodeBase {
    float scale{1.0f};
};
// An entry keeps its code while its unit compiles from identical input; codes never repeat in the process.
void verify_project_entry_codes() {
    const auto snapshot = [](std::string_view helper, std::string_view b, std::string_view extra) {
        source::SourceSnapshot input{
            "codes",
            1,
            {{"Helper.source", 1, "float helper(float x) { return x * " + std::string(helper) + "; }"},
             {"A.source", 1, "class game.A : project.CodeBase { float run(float x) { return helper(x) + 1.0f; } }"},
             {"B.source", 1,
              "class game.B : project.CodeBase { float run(float x) { return helper(x); } float other() { return " +
                  std::string(b) + "; } }"}}};
        if (!extra.empty())
            input.files.push_back({"Extra.source", 1, std::string(extra)});
        return input;
    };
    const auto base = [](Runtime& runtime) {
        take(
            runtime.register_type(NativeRegistration<CodeBase>(runtime, "project.CodeBase", TypeKind::object).build()));
    };
    const auto codes = [](const Program& program) {
        std::vector<std::pair<std::string, std::uint64_t>> result;
        for (const auto& entry : program.entries())
            result.emplace_back(entry.name, entry.code);
        std::ranges::sort(result);
        return result;
    };
    const auto code = [](const std::vector<std::pair<std::string, std::uint64_t>>& values, std::string_view name) {
        return std::ranges::find(values, name, &std::pair<std::string, std::uint64_t>::first)->second;
    };
    const std::string class_c = "class game.C : project.CodeBase { float run() { return 5.0f; } }";
    Runtime runtime;
    base(runtime);
    Program program(runtime);
    require(program.revision() == 0, "An unpublished Program reported a revision.");
    publish(program, snapshot("2.0f", "1.0f", ""));
    const auto first = codes(program);
    std::vector<std::uint64_t> seen;
    for (const auto& [name, value] : first)
        seen.push_back(value);
    std::ranges::sort(seen);
    require(program.revision() == 1 && first.size() == 4 && seen.front() != 0 &&
                std::ranges::adjacent_find(seen) == seen.end(),
            "The first publication did not give every entry its own code.");
    publish(program, snapshot("2.0f", "3.0f", ""));
    const auto second = codes(program);
    require(program.revision() == 2 && code(second, "game.A.run") == code(first, "game.A.run") &&
                code(second, "game.B.run") != code(first, "game.B.run") &&
                code(second, "game.B.other") != code(first, "game.B.other") &&
                code(second, "helper") == code(first, "helper"),
            "A class method edit did not change exactly that class's codes.");
    publish(program, snapshot("4.0f", "3.0f", ""));
    const auto third = codes(program);
    for (const auto& [name, value] : third)
        require(value != code(second, name), "A global helper edit kept a class or helper code.");
    publish(program, snapshot("4.0f", "3.0f", class_c));
    const auto fourth = codes(program);
    for (const auto& [name, value] : third)
        require(code(fourth, name) == value, "Adding a class changed the codes of unchanged classes.");
    require(code(fourth, "game.C.run") >
                    std::ranges::max_element(third, {}, &std::pair<std::string, std::uint64_t>::second)->second &&
                program.revision() == 4,
            "A new class reused an earlier code.");
    // A new Runtime value type changes every member catalog although no Source text changed.
    TypeDefinition extra;
    extra.name = "project.Extra";
    extra.kind = TypeKind::record;
    FieldDefinition x;
    x.name = "x";
    x.value = {TypeReference::native<float>()};
    extra.fields.push_back(std::move(x));
    take(runtime.register_type(std::move(extra)));
    publish(program, snapshot("4.0f", "3.0f", class_c));
    const auto fifth = codes(program);
    for (const auto& [name, value] : fifth)
        require(value != code(fourth, name), "A changed member catalog kept an entry code.");
    const auto publish_with = [&](source::SourceSnapshot input, source::CompileOptions options) {
        auto candidate = take(Program::prepare(take(program.request(std::move(input), options))));
        take(program.try_publish(candidate));
    };
    publish_with(snapshot("4.0f", "3.0f", class_c), {.diagnostics = true});
    const auto sixth = codes(program);
    auto renamed = snapshot("4.0f", "3.0f", class_c);
    renamed.name = "renamed";
    publish_with(std::move(renamed), {.diagnostics = true});
    const auto seventh = codes(program);
    for (const auto& [name, value] : sixth)
        require(value != code(fifth, name) && value != code(seventh, name),
                "Changed compile options or snapshot name kept an entry code.");
    auto stale = take(Program::prepare(take(program.request(snapshot("5.0f", "3.0f", class_c)))));
    auto latest = take(Program::prepare(take(program.request(snapshot("4.0f", "6.0f", class_c)))));
    require(!program.try_publish(stale), "An older snapshot published.");
    {
        auto execution = take(ExecutionScope::enter(runtime));
        require(!program.try_publish(latest), "Busy publication succeeded.");
    }
    require(program.revision() == 7 && codes(program) == seventh,
            "A rejected publication changed the revision or entry codes.");
    take(program.try_publish(latest));
    const auto eighth = codes(program);
    require(!program.try_publish(latest) && program.revision() == 8 && codes(program) == eighth && eighth != seventh,
            "A repeated publication changed the revision or entry codes.");
    Program single(runtime);
    auto single_candidate = take(Program::prepare(take(single.request("single", "float one() { return 1.0f; }"))));
    take(single.try_publish(single_candidate));
    require(single.revision() == 1 && !single.try_publish(single_candidate) && single.revision() == 1,
            "A single-source publication did not count exactly once.");
    Runtime other_runtime;
    base(other_runtime);
    Program other(other_runtime);
    publish(other, snapshot("2.0f", "1.0f", ""));
    for (const auto& [name, value] : codes(other))
        for (const auto* values : {&first, &second, &third, &fourth, &fifth, &sixth, &seventh, &eighth})
            for (const auto& [_, earlier] : *values)
                require(value != earlier, "Two Programs published the same entry code.");
}
enum class Quality : std::uint32_t { low = 1, high = 7 };
struct SelfReader {
    float gain{};
    Quality quality{Quality::low};
    math::Vector3 offset{};
    std::uint32_t twice(std::uint32_t value) const noexcept { return value * 2; }
    float peek(const float& value) const noexcept { return value; }
};
struct SelfReadRetired {
    static inline int destroyed{};
    ~SelfReadRetired() { ++destroyed; }
};
// A value region reads inherited native and declared Source fields of self only: the reads do not
// stamp self, advance GC or move the borrowed storage, and every other self access stays rejected.
void verify_project_self_reads() {
    Runtime runtime;
    take(runtime.register_types(
        {array_definition<float>(runtime, "project.Floats"),
         NativeRegistration<Quality>(runtime, "project.Quality", TypeKind::enumeration)
             .enumerator("low", Quality::low)
             .enumerator("high", Quality::high)
             .build(),
         NativeRegistration<SelfReader>(runtime, "project.Reader", TypeKind::object)
             .field<&SelfReader::gain>("gain")
             .field<&SelfReader::quality>("quality")
             .field<&SelfReader::offset>("offset")
             .method<&SelfReader::twice>("twice", true)
             .method<&SelfReader::peek>("peek", true)
             .build(),
         NativeRegistration<SelfReadRetired>(runtime, "project.Retired", TypeKind::object).build()}));
    const auto snapshot = [](std::string_view methods) {
        return source::SourceSnapshot{
            "reads",
            1,
            {{"Node.source", 1,
              "class game.Node : project.Reader {\n"
              "    float bias = 0.75f;\n    uint seed = 5;\n    int shift = -3;\n    bool enabled = true;\n"
              "    void store() { self.bias = 2.0f; }\n" +
                  std::string(methods) + "}\n"}}};
    };
    Program program(runtime);
    // Each body compiles in an ordinary method; a borrowed Array parameter makes the method a value region.
    for (const std::string body :
         {"self.bias = 2.0f;", "self.offset.x = 1.0f;", "(self.offset).x = 1.0f;", "float x = self.peek(self.gain);",
          "store();", "float x = (new game.Node()).bias;"}) {
        require(
            static_cast<bool>(Program::prepare(take(program.request(snapshot("    void probe() { " + body + " }\n"))))),
            "A self access control did not compile outside a value region.");
        const auto failed = Program::prepare(
            take(program.request(snapshot("    void probe(Array<float>& values) { " + body + " }\n"))));
        require(!failed && failed.error().message.find("Scoped value execution in 'probe'") != std::string::npos,
                "A value region admitted a self write, a self field borrow or another object's field.");
    }
    publish(program, snapshot(R"(
    float scaled(float x) { return x * self.gain + self.bias; }
    uint evaluate(Array<float>& values, const Array<float>& input) {
        const vector3 offset = self.offset;
        const float lift = self.offset.y;
        uint i = 0;
        while (i < values.size()) {
            float value = scaled(input[i]) + offset.x + lift;
            if (self.enabled && self.quality == project.Quality::high) { value = value + float(self.shift); }
            values[i] = value;
            i = i + 1;
        }
        return self.twice(self.seed);
    }
)"));
    const auto base = take(runtime.native_type<SelfReader>());
    const auto type = program.classes()[0].type;
    const auto node = take(runtime.create(type));
    const auto id = node.id();
    const std::array gain{take(runtime.find_field(base, "gain"))}, quality{take(runtime.find_field(base, "quality"))},
        offset{take(runtime.find_field(base, "offset"))}, bias{take(runtime.find_field(type, "bias"))};
    const auto vector3 = take(runtime.native_type<math::Vector3>());
    const auto quality_type = take(runtime.native_type<Quality>());
    Array<float> values, input;
    for (int i = 0; i < 8; ++i) {
        input.add(static_cast<float>(i) * 0.5f - 3.0f);
        values.add(0.0f);
    }
    const auto* storage = values.data();
    const auto entry = take(program.entry("game.Node.evaluate"));
    const std::array args{Argument::borrow(values), Argument::borrow(std::as_const(input))};
    // Exact dyadic values keep the expected results independent of rounding order.
    const auto run = [&](float g, Quality q, math::Vector3 o, float b, float shift) {
        take(runtime.write(id, gain, Scalar{g}));
        take(runtime.write(id, quality, Scalar{EnumValue{quality_type, static_cast<std::uint32_t>(q)}}));
        take(runtime.write(id, offset, ValueInput{vector3, &o}));
        take(runtime.write(id, bias, Scalar{b}));
        auto retired = take(runtime.create(take(runtime.native_type<SelfReadRetired>())));
        retired.reset();
        const auto destroyed = SelfReadRetired::destroyed;
        take(runtime.begin_collection());
        const auto phase = runtime.collection_phase();
        const auto cursor = take(runtime.change_cursor());
        require(std::get<std::uint32_t>(*take(entry.invoke(id, args))) == 10,
                "A value region misread a self field through a BorrowSafe call.");
        require(runtime.collection_phase() == phase && SelfReadRetired::destroyed == destroyed,
                "Self field reads advanced GC inside a value region.");
        require(!runtime.changed_since(id, cursor), "A self field read stamped its object.");
        require(values.data() == storage, "Self field reads moved the borrowed storage.");
        for (std::size_t i = 0; i < values.size(); ++i)
            require(values[i] == input[i] * g + b + o.x + o.y + shift,
                    "A value region misread an inherited or declared self field.");
        while (runtime.collection_phase() != CollectionPhase::idle)
            (void)take(runtime.collect_step({1}));
        require(SelfReadRetired::destroyed == destroyed + 1,
                "The host could not collect after the value region returned.");
        return cursor;
    };
    const auto cursor = run(1.5f, Quality::high, {0.25f, -2.0f, 7.0f}, 0.75f, -3.0f);
    take(runtime.write(id, bias, Scalar{0.25f}));
    require(runtime.changed_since(id, cursor), "The write-stamp control missed a host write.");
    run(-2.0f, Quality::low, {1.0f, 0.5f, 0.0f}, 0.25f, 0.0f);
}
// Source class fields hold scalars, float/double vectors and enums, with whole-value defaults and AFIELD
// attributes. Defaults and bounds are finite constants; changing them or the presentation text needs a rebuild.
void verify_project_class_fields() {
    Runtime runtime;
    take(runtime.register_types({array_definition<float>(runtime, "project.Floats"),
                                 NativeRegistration<Quality>(runtime, "project.Quality", TypeKind::enumeration)
                                     .enumerator("low", Quality::low)
                                     .enumerator("high", Quality::high)
                                     .build(),
                                 NativeRegistration<CodeBase>(runtime, "project.CodeBase", TypeKind::object).build()}));
    const std::string enums = "enum geo.Axis : uint { x, y, z }\nenum geo.Falloff { linear, smooth, constant }\n";
    const std::string jitter = R"(class geo.Jitter : project.CodeBase {
    [Min = 0.0, Max = 2.0, LockMin, HardMax = 10.0, Label = "Amount", Category = "Noise", DESC = "Jitter distance.\nPer point."]
    float amount = 0.5f;
    [Min = -1.0, Max = 1.0, LockMin, LockMax] vector3 offset = vector3(0.25, -0.5, 1.0 / 8.0);
    [HardMin = 0.0d] vector2d size = vector2d(1.0d, 2.0d);
    vector4 color;
    vector2 span = vector2();
    geo.Axis up_axis = geo.Axis::z;
    geo.Falloff falloff;
    project.Quality quality = project.Quality::high;
    [Min = 1, Max = 64, LockMin] uint samples = 8;
    [HardMin = -5, HardMax = 5] int shift = -2;
    float pick(Array<float>& values) {
        float sum = 0.0f;
        if (self.up_axis == geo.Axis::z && self.quality == project.Quality::high) { sum = self.offset.x + self.offset.z + float(self.shift); }
        values[0] = sum;
        return sum + self.amount;
    }
    void flip() { self.up_axis = geo.Axis::x; self.offset = vector3(0.5, 3.0, -0.5); self.amount = 50.0f; }
}
)";
    const auto snapshot = [&](std::string body, std::string extra = {}) {
        source::SourceSnapshot input{"fields", 1, {{"Enums.source", 1, enums}, {"Jitter.source", 1, std::move(body)}}};
        if (!extra.empty())
            input.files.push_back({"Bad.source", 1, std::move(extra)});
        return input;
    };
    Program program(runtime);
    publish(program, snapshot(jitter));
    const auto type = program.classes()[0].type;
    const auto axis = take(runtime.find_type("geo.Axis")), falloff = take(runtime.find_type("geo.Falloff"));
    const auto quality = take(runtime.native_type<Quality>());
    const auto* info = take(runtime.describe(type));
    const auto described = [&](std::string_view name) -> const FieldInfo& {
        return *std::ranges::find(info->fields, name, &FieldInfo::name);
    };
    const auto& amount = described("amount");
    require(amount.label == "Amount" && amount.category == "Noise" &&
                amount.description == "Jitter distance.\nPer point." &&
                amount.range == FieldRange{Scalar{0.0f}, Scalar{2.0f}, Scalar{0.0f}, Scalar{10.0f}} &&
                described("offset").range == FieldRange{Scalar{-1.0f}, Scalar{1.0f}, Scalar{-1.0f}, Scalar{1.0f}} &&
                described("size").range == FieldRange{.hard_min = Scalar{0.0}} &&
                described("samples").range == FieldRange{Scalar{1u}, Scalar{64u}, Scalar{1u}, {}} &&
                described("shift").range ==
                    FieldRange{.hard_min = Scalar{std::int32_t{-5}}, .hard_max = Scalar{std::int32_t{5}}},
            "Source field attributes did not publish their presentation text or ranges.");
    require(described("offset").default_leaves == std::vector<Scalar>{0.25f, -0.5f, 0.125f} &&
                described("size").default_leaves == std::vector<Scalar>{1.0, 2.0} &&
                described("color").default_leaves == std::vector<Scalar>(4, 0.0f) &&
                described("span").default_leaves == std::vector<Scalar>(2, 0.0f) &&
                described("up_axis").default_leaves == std::vector<Scalar>{EnumValue{axis, 2}} &&
                described("falloff").default_leaves == std::vector<Scalar>{EnumValue{falloff, 0}} &&
                described("quality").default_leaves == std::vector<Scalar>{EnumValue{quality, 7}} &&
                described("shift").default_leaves == std::vector<Scalar>{std::int32_t{-2}},
            "Source field defaults did not publish one leaf per component.");
    auto object = take(runtime.create(type));
    const auto id = object.id();
    const auto field = [&](std::string_view name) { return std::array{take(runtime.find_field(type, name))}; };
    const auto vector3 = take(runtime.native_type<math::Vector3>());
    math::Vector3 offset;
    math::Vector2d size;
    take(runtime.read(id, field("offset"), ValueOutput{vector3, &offset}));
    take(runtime.read(id, field("size"), ValueOutput{take(runtime.native_type<math::Vector2d>()), &size}));
    require(offset == math::Vector3{0.25f, -0.5f, 0.125f} && size == math::Vector2d{1.0, 2.0} &&
                take(runtime.read(id, field("up_axis"))) == Scalar{EnumValue{axis, 2}} &&
                take(runtime.read(id, field("quality"))) == Scalar{EnumValue{quality, 7}} &&
                take(runtime.read(id, field("samples"))) == Scalar{8u} &&
                take(runtime.read(id, field("shift"))) == Scalar{std::int32_t{-2}},
            "Runtime create did not apply Source vector, enum or scalar defaults.");
    // A value region reads the declared vector and enum fields; Source writes clamp to declared hard bounds.
    Array<float> values;
    values.add(0.0f);
    const std::array args{Argument::borrow(values)};
    const auto pick = take(program.entry("geo.Jitter.pick"));
    require(std::get<float>(*take(pick.invoke(id, args))) == -1.125f && values[0] == -1.625f,
            "A value region misread a Source vector or enum field.");
    take(take(program.entry("geo.Jitter.flip")).invoke(id, {}));
    take(runtime.read(id, field("offset"), ValueOutput{vector3, &offset}));
    require(offset == math::Vector3{0.5f, 1.0f, -0.5f} && take(runtime.read(id, field("amount"))) == Scalar{10.0f} &&
                take(runtime.read(id, field("up_axis"))) == Scalar{EnumValue{axis, 0}} &&
                std::get<float>(*take(pick.invoke(id, args))) == 10.0f,
            "Source writes ignored the hard bounds or enum values of Source fields.");
    const auto edited = [&](std::string_view from, std::string_view to) {
        auto text = jitter;
        const auto at = text.find(from);
        require(at != std::string::npos, "A field edit anchor is missing.");
        return snapshot(text.replace(at, from.size(), to));
    };
    publish(program, edited("[Min = 0.0, Max = 2.0,", "[ Min=0.0,Max  =  2.0 ,"));
    publish(program, edited("] vector3 offset", "] math.vector3 offset"));
    require(program.classes()[0].type == type,
            "Spacing in a field attribute list or a vector's canonical name changed the published class.");
    for (const auto& [from, to] : std::initializer_list<std::pair<std::string_view, std::string_view>>{
             {"\"Amount\"", "\"Amount 2\""},
             {"\"Noise\"", "\"Other\""},
             {"Per point.", "Per item."},
             {"HardMax = 10.0", "HardMax = 11.0"},
             {"vector3(0.25, -0.5", "vector3(0.25, -0.75"},
             {"geo.Axis::z;", "geo.Axis::y;"},
             {"[HardMin = 0.0d] ", ""}}) {
        const auto failed = Program::prepare(take(program.request(edited(from, to))));
        if (failed || failed.error().code != ErrorCode::rebuild_required)
            throw std::runtime_error("A Source field edit published without a rebuild: " + std::string(to));
    }
    // Each malformed field reports its own line in its own file.
    for (const auto& [member, fragment] : std::initializer_list<std::pair<std::string_view, std::string_view>>{
             {"float bad = 0.0f / 0.0f;", "must be finite"},
             {"float bad = 1e30f * 1e30f;", "must be finite"},
             {"vector3 bad = vector3(1.0, 0.0 / 0.0, 2.0);", "must be finite"},
             {"vector3 bad = vector3(1.0, 2.0);", "one constant per component"},
             {"vector3 bad = vector3(1.0, 2.0, 3.0) * 2.0;", "one constant per component"},
             {"vector3d bad = vector3d(1.0d, 2.0, 3.0d);", ""},
             {"quaternion bad;", "quaternions or matrices"},
             {"project.CodeBase bad;", "Source object fields require"},
             {"geo.Axis bad = geo.Falloff::linear;", "Enum field defaults name one item"},
             {"geo.Axis bad = geo.Axis::w;", "Enum field defaults name one item"},
             {"[Min = 0.0] bool bad;", "Field ranges require"},
             {"[HardMax = 1] geo.Axis bad;", "Field ranges require"},
             {"[LockMin] float bad;", "requires Min and excludes HardMin"},
             {"[Min = 0.0, LockMin, HardMin = 1.0] float bad;", "requires Min and excludes HardMin"},
             {"[Min = 2.0, Max = 1.0] float bad;", "minimum exceeds maximum"},
             {"[HardMax = 1.0] float bad = 2.0f;", "default lies outside hard bounds"},
             {"[HardMin = 0.0] vector3 bad = vector3(1.0, -1.0, 1.0);", "default lies outside hard bounds"},
             {"[Min = 0.0d] float bad;", ""},
             {"[Min = missing] float bad;", ""},
             {"[Min = 1.0 +] float bad;", ""},
             {"vector3 bad = vector3(1.0, 2.0, );", ""},
             {"[Label = \"a\\nb\"] float bad;", "control characters"},
             {"[DESC = \"a\\0b\"] float bad;", "Description requires valid UTF-8"},
             {"[Label = 1.0] float bad;", "requires a string literal"},
             {"[Shape = 1.0] float bad;", "Unknown field attribute"},
             {"[Min = 0.0, Min = 1.0] float bad;", "Duplicate field attribute"},
             {"[Min = 0.0] void bad() {}", "Attributes apply only to class fields"}}) {
        const auto failed = Program::prepare(take(program.request(
            snapshot(jitter, "class game.Bad : project.CodeBase {\n    " + std::string(member) + "\n}\n"))));
        if (failed || !failed.error().source || failed.error().source->path != "Bad.source" ||
            failed.error().source->line != 2 || failed.error().message.find(fragment) == std::string::npos)
            throw std::runtime_error("A malformed Source field was not rejected at its line: " + std::string(member) +
                                     " -> " + (failed ? std::string("accepted") : failed.error().message));
    }
}
struct EditorPanel {
    float x;
};
struct ProjectHost {
    float x;
};
// The editor registers its own types under editor. in the same Runtime before project Source
// compiles. Project Source neither sees nor declares them; other registered types bind as usual.
void verify_editor_reserved_types() {
    Runtime runtime;
    // The editor and project records differ only by name.
    const auto record = [](std::string name) {
        TypeDefinition definition;
        definition.name = std::move(name);
        definition.kind = TypeKind::record;
        FieldDefinition x;
        x.name = "x";
        x.value = {TypeReference::native<float>()};
        definition.fields.push_back(std::move(x));
        return definition;
    };
    TypeDefinition mode;
    mode.name = "editor.Mode";
    mode.kind = TypeKind::enumeration;
    mode.enumeration = TypeDefinition::Enumeration{TypeReference::native<std::uint32_t>(), {{"a", 0}, {"b", 1}}};
    auto level = mode;
    level.name = "project.Level";
    take(runtime.register_types(
        {record("editor.Probe"), NativeRegistration<EditorPanel>(runtime, "editor.Panel", TypeKind::object).build(),
         std::move(mode), record("project.Reading"),
         NativeRegistration<ProjectHost>(runtime, "project.Host", TypeKind::object).build(), std::move(level)}));
    Program program(runtime);
    const auto rejected = [&](std::string text, std::string_view expected, const char* message) {
        auto failed = Program::prepare(
            take(program.request(source::SourceSnapshot{"editor", 1, {{"Reserved.source", 1, std::move(text)}}})));
        if (failed || failed.error().code != ErrorCode::invalid_definition ||
            failed.error().message.find(expected) == std::string::npos)
            throw std::runtime_error(std::string(message) + (failed ? "" : " Got: " + failed.error().message));
    };
    rejected("float read() { editor.Probe probe; return probe.x; }", "Unknown registered value type 'editor.Probe'",
             "Project Source used an editor. record.");
    rejected("bool pick() { return editor.Mode::a == editor.Mode::b; }", "Unknown enum type",
             "Project Source used an editor. enumeration.");
    rejected("struct game.Holder { editor.Probe probe; };", "editor.Probe", "A project struct held an editor. record.");
    rejected("class game.Panel : editor.Panel { }", "Unknown native base type: editor.Panel",
             "A project class derived from an editor. object type.");
    rejected("struct editor.Mine { float a; };", "reserved for editor types",
             "Project Source declared an editor. struct.");
    rejected("enum editor.Choice { A }", "reserved for editor types",
             "Project Source declared an editor. enumeration.");
    rejected("class editor.Tool : project.Host { }", "reserved for editor types",
             "Project Source declared an editor. class.");
    require(!runtime.find_type("game.Holder") && !runtime.find_type("game.Panel"),
            "A rejected project published a type.");
    publish(program, {"editor", 2, {{"Visible.source", 2, R"(
struct game.Holder { project.Reading reading; float scale; };
float read(float value) { game.Holder holder; holder.reading.x = value; return holder.reading.x; }
bool pick() { return project.Level::a != project.Level::b; }
class game.Panel : project.Host { float speed = 1.0f; }
)"}}});
    const std::array value{Scalar{2.5f}};
    require(std::get<float>(*take(take(program.entry("read")).invoke({}, value))) == 2.5f &&
                std::get<bool>(*take(take(program.entry("pick")).invoke({}, {}))) && program.classes().size() == 1,
            "Registered project types did not bind beside hidden editor. types.");
}
struct EngineBase {
    float x;
};
std::filesystem::path executable_directory() {
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
    return std::filesystem::path(path).parent_path();
}
// Engine Source files load from disk with engine/ paths and declare only their module's names;
// project files use them but cannot declare in that namespace.
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
    const auto write = [&](const std::filesystem::path& relative, std::string_view text) {
        std::filesystem::create_directories((root / relative).parent_path());
        std::ofstream(root / relative, std::ios::binary) << text;
    };
    const std::string random =
        "const uint procedural.SEED = 7;\nuint procedural.hash(uint x) { return x * 31 + procedural.SEED; }\n";
    write("procedural/random.source", "\xef\xbb\xbf" + random);
    write("procedural/nested/orient.source", R"(
enum procedural.Axis : uint { x, y, z }
const float procedural.y = 9.0;
float procedural.scaled(float v) { return v * float(procedural.hash(2)); }
class procedural.Jitter : procedural.Base { float amount = 0.5f; float evaluate(float v) { return v + self.amount * float(procedural.hash(1)); } }
)");
    write("procedural/notes.txt", "not source");
    write("copy/random.source", random);
    const auto paths = take(source::engine_source_paths(root));
    require(paths.size() == 3 && paths[0] == std::filesystem::path("copy/random.source") &&
                paths[1] == std::filesystem::path("procedural/nested/orient.source") &&
                paths[2] == std::filesystem::path("procedural/random.source"),
            "Engine source paths were not the sorted .source files under the root.");
    auto loaded = take(source::engine_sources(root));
    require(loaded.size() == 3 && loaded[0].path == "engine/copy/random.source" &&
                loaded[1].path == "engine/procedural/nested/orient.source" &&
                loaded[2].path == "engine/procedural/random.source" && loaded[2].source == random &&
                loaded[0].source == random && loaded[0].version != loaded[2].version,
            "Engine sources lost their engine/ path, dropped text or hashed the BOM away.");
    write("copy/random.source", "\xef\xbb\xbf" + random);
    const auto copied = take(source::engine_sources(root))[0].version;
    write("copy/random.source", "\xef\xbb\xbf" + random + " ");
    require(copied == loaded[2].version && take(source::engine_sources(root))[0].version != copied,
            "Engine source versions did not follow the saved bytes alone.");
    require(take(source::engine_sources(root / "missing")).empty(), "A missing engine root produced sources.");
    // The build copies assets/engine beside the executable exactly; development builds read the tree.
    const std::filesystem::path tree{ATELIER_ENGINE_SOURCE_TREE};
    const auto tree_files = take(source::engine_sources(tree)),
               copy_files = take(source::engine_sources(executable_directory() / "engine"));
    require(tree_files.size() == copy_files.size() &&
                std::ranges::equal(tree_files, copy_files,
                                   [](const auto& a, const auto& b) {
                                       return a.path == b.path && a.version == b.version && a.source == b.source;
                                   }),
            "engine/ beside the executable differs from assets/engine.");
    require(source::default_engine_root() ==
                (std::filesystem::is_directory(tree) ? tree : executable_directory() / "engine"),
            "The default engine root did not prefer the source tree.");

    Runtime runtime;
    {
        write("procedural/broken.source", "uint procedural.broken() { return missing; }");
        const auto disk_path = root / "procedural/broken.source";
        auto file = take(source::read_source_file(disk_path, "engine/procedural/broken.source"));
        const auto version = file.version;
        Program diagnostic(runtime);
        auto failed = Program::prepare(take(diagnostic.request(source::SourceSnapshot{"origin", 1, {file}})));
        require(
            !failed && failed.error().source && failed.error().source->path == file.path &&
                failed.error().source->version == version &&
                std::filesystem::path(std::u8string(failed.error().source->disk_path.begin(),
                                                    failed.error().source->disk_path.end())) == disk_path,
            "An engine compile diagnostic lost its saved-file origin or confused it with its virtual engine/ name.");
    }
    take(runtime.register_type(NativeRegistration<EngineBase>(runtime, "procedural.Base", TypeKind::object).build()));
    Program program(runtime);
    std::vector<source::SourceFile> engine(loaded.begin() + 1, loaded.end());
    const auto with = [&](std::vector<source::SourceFile> files) {
        source::SourceSnapshot snapshot{"engine", 1, engine};
        snapshot.files.insert(snapshot.files.end(), files.begin(), files.end());
        return snapshot;
    };
    const auto rejected = [&](std::vector<source::SourceFile> files, std::string_view expected, const char* message) {
        const auto path = files.front().path;
        auto failed = Program::prepare(take(program.request(with(std::move(files)))));
        if (failed || failed.error().message.find(expected) == std::string::npos || !failed.error().source ||
            failed.error().source->path != path)
            throw std::runtime_error(std::string(message) + (failed ? "" : " Got: " + failed.error().message));
    };
    const auto project = [](std::string text) {
        return std::vector<source::SourceFile>{{"Source/Bad.source", 1, std::move(text)}};
    };
    const auto engine_file = [](std::string text) {
        return std::vector<source::SourceFile>{{"engine/procedural/bad.source", 1, std::move(text)}};
    };
    const std::string_view reserved =
        "Only engine sources under engine/procedural/ may declare names in the procedural. namespace";
    rejected(project("class procedural.Mine : procedural.Base { }"), reserved,
             "A project file declared an engine class.");
    rejected(project("struct procedural.Mine { float a; };"), reserved, "A project file declared an engine struct.");
    rejected(project("enum procedural.Mine { a }"), reserved, "A project file declared an engine enum.");
    rejected(project("float procedural.mine() { return 1.0; }"), reserved,
             "A project file declared an engine function.");
    rejected(project("const uint procedural.MINE = 1;"), reserved, "A project file declared an engine constant.");
    const std::string_view own =
        "Engine sources under engine/procedural/ may only declare names in the procedural. namespace";
    rejected(engine_file("float other.f() { return 1.0; }"), own, "An engine file declared outside its module.");
    rejected(engine_file("float f() { return 1.0; }"), own, "An engine file declared an unqualified name.");
    rejected({{"engine/loose.source", 1, ""}}, "Engine source paths must be engine/<module>/<file>",
             "An engine file had no module.");
    rejected({{"engine/math/x.source", 1, ""}}, "Engine source paths must be engine/<module>/<file>",
             "An engine module used math.");
    rejected(engine_file("float procedural.Base() { return 1.0; }"), "cannot share a name with a type: procedural.Base",
             "A function took a registered type's name.");
    rejected(engine_file("const uint procedural.Jitter = 1;"), "cannot share a name with a type: procedural.Jitter",
             "A constant took a source class's name.");
    // Empty classes have no bound compile unit: the project-wide name check must see accessor projections.
    rejected(project("class C : procedural.Base {} uint GetC() { return 1; } uint run() { return C; }"),
             "cannot share a name with a type: C", "A getter property took an empty source class's name.");
    rejected(project("class C : procedural.Base {} void SetC(const uint& value) {} void run() { C = 1; }"),
             "cannot share a name with a type: C",
             "A const-reference setter property took an empty source class's name.");
    rejected(engine_file("uint procedural.GetBase() { return 1; }"), "cannot share a name with a type: procedural.Base",
             "An engine getter property took a registered object type's name.");
    rejected(
        project(
            "class mygame.Twin : procedural.Base { float run() { return 1.0; } }\nfloat mygame.Twin.run() { return 2.0; }"),
        "cannot extend a type name: mygame.Twin.run", "A function took a class method's entry name.");
    rejected(engine_file("uint procedural.twice(uint v) { return hash(v); }"), "Unknown source function 'hash'",
             "An engine file called a qualified function by its short name.");
    require(!runtime.find_type("procedural.Jitter") && !runtime.find_type("C") && program.classes().empty(),
            "A rejected engine snapshot published a type.");
    publish(program, with({{"Source/Game.source", 1, R"(
class mygame.Foo : procedural.Base {
    procedural.Axis axis = procedural.Axis::z;
    float run(float v) { return procedural.scaled(v) + float(procedural.SEED); }
}
uint project_hash(uint v) { return procedural.hash(v); }
float project_scaled(float v) { return procedural.scaled(v); }
float shadowed() { vector3 procedural = vector3(1.0, 2.0, 3.0); return procedural.y; }
float unshadowed() { return procedural.y; }
)"}}));
    const auto invoke = [&](std::string_view name, std::span<const Scalar> arguments) {
        return *take(take(program.entry(name)).invoke({}, arguments));
    };
    const std::array five{Scalar{std::uint32_t{5}}}, half{Scalar{0.5f}};
    require(std::get<std::uint32_t>(invoke("project_hash", five)) == 162 &&
                std::get<float>(invoke("project_scaled", half)) == 34.5f &&
                std::get<float>(invoke("shadowed", {})) == 2.0f && std::get<float>(invoke("unshadowed", {})) == 9.0f,
            "Qualified engine functions, constants or local shadowing evaluated incorrectly.");
    const auto classes = program.classes();
    const auto base = take(runtime.find_type("procedural.Base"));
    require(classes.size() == 2 && std::ranges::all_of(classes, [&](const auto& c) { return c.base == base; }) &&
                std::ranges::any_of(program.entries(),
                                    [](const auto& e) { return e.name == "procedural.Jitter.evaluate"; }) &&
                std::ranges::any_of(program.entries(), [](const auto& e) { return e.name == "mygame.Foo.run"; }),
            "Engine and project classes on the engine's native base did not publish.");

    // Get/Set alone does not reserve a name: only eligible signatures project a property.
    Program ordinary_accessors(runtime);
    publish(ordinary_accessors, {"property-names", 1, {{"Source/Names.source", 1, R"(
class C : procedural.Base {}
class D : procedural.Base {}
uint GetC(uint value) { return value + 1; }
bool SetC(uint value) { return value == 8; }
void GetD() {}
void SetD(uint& value) { value = value + 1; }
uint GetLimit() { return 11; }
uint run() {
    uint value = 7;
    GetD();
    SetD(value);
    if (SetC(value)) { return GetC(value) + Limit; }
    return 0;
}
)"}}});
    require(std::get<std::uint32_t>(*take(take(ordinary_accessors.entry("run")).invoke({}, {}))) == 20,
            "Ineligible Get/Set functions conflicted with types, or a nonconflicting property stopped working.");
}
// A unit compiles its own text with the declarations it spells, so its code follows only them;
// engine functions are internal, and each declaration is still checked in its home unit.
void verify_project_compile_units() {
    Runtime runtime;
    take(runtime.register_type(NativeRegistration<EngineBase>(runtime, "procedural.Base", TypeKind::object).build()));
    Program program(runtime);
    const auto snapshot = [](std::string_view inner, std::string_view unused, std::string_view b,
                             std::string_view extra = {}) {
        return source::SourceSnapshot{
            "units",
            1,
            {{"Source/Game.source", 1,
              "float twice(float x) { return x * 2.0; }\n"
              "class game.A : procedural.Base { float run(float x) { return procedural.used(x); } }\n"
              "class game.B : procedural.Base { float run(float x) { return twice(x) + " +
                  std::string(b) + "; } }\n" + std::string(extra)},
             {"engine/procedural/lib.source", 1,
              "const float procedural.K = 3.0;\n"
              "float procedural.inner(float x) { return x + procedural.K * " +
                  std::string(inner) +
                  "; }\n"
                  "float procedural.used(float x) { return procedural.inner(x) * 2.0; }\n"
                  "float procedural.unused(float x) { return x - " +
                  std::string(unused) + "; }\n"}}};
    };
    const auto codes = [&] {
        std::vector<std::pair<std::string, std::uint64_t>> result;
        for (const auto& entry : program.entries())
            result.emplace_back(entry.name, entry.code);
        std::ranges::sort(result);
        return result;
    };
    const auto rejected = [&](source::SourceSnapshot input, std::string_view path, std::string_view expected,
                              const char* message) {
        auto failed = Program::prepare(take(program.request(std::move(input))));
        if (failed || !failed.error().source || failed.error().source->path != path ||
            failed.error().message.find(expected) == std::string::npos)
            throw std::runtime_error(std::string(message) + (failed ? "" : " Got: " + failed.error().message));
    };
    rejected(snapshot("1.0", "true", "1.0"), "engine/procedural/lib.source", "",
             "An uncalled engine function skipped its type check.");
    rejected(snapshot("1.0", "1.0", "1.0", "const uint BAD = 1.5;\n"), "Source/Game.source", "",
             "An unused project constant skipped its type check.");
    rejected(snapshot("1.0", "1.0", "1.0", "float twice(float y) { return y; }\n"), "Source/Game.source",
             "Duplicate source function name", "A repeated top-level name was accepted.");
    rejected(snapshot("1.0", "1.0", "1.0", "class game.C : procedural.Base { float twice(float x) { return x; } }\n"),
             "Source/Game.source", "Duplicate source function name", "A class method shared a global function's name.");
    publish(program, snapshot("1.0", "1.0", "1.0"));
    const auto first = codes();
    require(first.size() == 3 && first[0].first == "game.A.run" && first[1].first == "game.B.run" &&
                first[2].first == "twice" && !program.entry("procedural.used"),
            "Engine functions were published as entries.");
    const auto a = take(runtime.create(program.classes()[0].type));
    const std::array one{Scalar{1.0f}};
    require(std::get<float>(*take(take(program.entry("game.A.run")).invoke(a.id(), one))) == 8.0f,
            "A class did not compile the engine functions it reaches transitively.");
    publish(program, snapshot("1.0", "2.0", "1.0"));
    require(codes() == first, "Editing an uncalled engine function changed a code.");
    publish(program, snapshot("2.0", "2.0", "1.0"));
    const auto second = codes();
    require(second[0].second != first[0].second && second[1] == first[1] && second[2] == first[2],
            "Editing an engine function changed codes beyond the class that reaches it.");
    require(std::get<float>(*take(take(program.entry("game.A.run")).invoke(a.id(), one))) == 14.0f,
            "The class that reaches an edited engine function kept its old code.");
    publish(program, snapshot("2.0", "2.0", "5.0"));
    const auto third = codes();
    require(third[0] == second[0] && third[1].second != second[1].second && third[2] == second[2],
            "Editing one class changed another unit's code.");
    // Moving an engine function into a project file keeps the project unit's text but adds an export.
    Program moved(runtime);
    publish(moved, {"moved",
                    1,
                    {{"Source/a.source", 1, "float f() { return m.g(); }\n"},
                     {"engine/m/x.source", 1, "float m.g() { return 1.0; }\n"}}});
    const auto before = moved.entries()[0].code;
    publish(moved, {"moved",
                    1,
                    {{"Source/a.source", 1, "float f() { return m.g(); }\n"},
                     {"Source/b.source", 1, "float m.g() { return 1.0; }\n"}}});
    const auto after = moved.entries();
    require(after.size() == 2 && after[0].code != after[1].code && after[0].code != before && after[1].code != before &&
                std::get<float>(*take(take(moved.entry("m.g")).invoke({}, {}))) == 1.0f,
            "A unit whose text stayed the same while its exports grew kept its old codes.");

    Program properties(runtime);
    source::SourceSnapshot property_input{
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
    publish(properties, property_input);
    const auto instance = take(runtime.create(take(runtime.find_type("game.Property"))));
    const auto entry = take(properties.entry("game.Property.run"));
    const auto old_property = take(properties.pin("game.Property.run"));
    require(std::get<std::uint32_t>(*take(entry.invoke(instance.id(), {}))) == 10,
            "Class-local or engine property did not resolve to its Source accessor.");
    property_input.files[0].source = "uint props.GetValue() { return 11; } void props.SetValue(uint v) {}";
    publish(properties, property_input);
    require(std::get<std::uint32_t>(*take(entry.invoke(instance.id(), {}))) == 21 &&
                std::get<std::uint32_t>(*take(take(properties.entry("value")).invoke({}, {}))) == 11 &&
                std::get<std::uint32_t>(*take(old_property.invoke(instance.id(), {}))) == 28,
            "A property dependency kept stale code or changed a pinned accessor version.");
    property_input.files[0].source = "uint props.GetValue() { return 11; } void props.SetValue(float v) {}";
    const auto bad_property = Program::prepare(take(properties.request(property_input)));
    require(!bad_property &&
                bad_property.error().message.find("Property getter and setter types differ") != std::string::npos,
            "The dependency closure omitted an unused property setter's contract.");
}
// A unit compiled from unchanged input keeps its machine code; the new Version maps faults and
// breakpoints through the current files and works once the module that compiled it is gone.
void verify_project_code_reuse() {
    Runtime runtime;
    take(runtime.register_type(NativeRegistration<EngineBase>(runtime, "procedural.Base", TypeKind::object).build()));
    Program program(runtime);
    const auto snapshot = [](std::size_t lines, std::string_view b, std::string_view extra = {}) {
        return source::SourceSnapshot{
            "reuse",
            1,
            {{"Source/Game.source", lines + 1,
              std::string(lines, '\n') +
                  "class game.A : procedural.Base {\n    int run(int x) {\n        return 10 / x;\n    }\n}\n"
                  "class game.B : procedural.Base { int run(int x) { return x + " +
                  std::string(b) + "; } }\n" + std::string(extra)}}};
    };
    const std::array zero{Scalar{std::int32_t{0}}}, five{Scalar{std::int32_t{5}}};
    const auto fault = [&](const Program::Lease& lease, ObjectId receiver, std::size_t line, std::uint64_t version) {
        const auto failed = lease.invoke(receiver, zero);
        return !failed && failed.error().source && failed.error().source->path == "Source/Game.source" &&
               failed.error().source->line == line && failed.error().source->version == version;
    };
    const auto stop_line = [&](const Program::Lease& lease, ObjectId receiver, std::size_t line,
                               std::uint64_t version) {
        std::optional<source::DebugBreakpoint> at;
        for (const auto& site : take(lease.debug_sites()))
            if (site.source.line == line)
                at = source::DebugBreakpoint{site.source.path, site.source.version, line, 0};
        if (!at || at->source_version != version)
            return false;
        std::size_t stopped{};
        source::Debugger debugger(
            [&](const source::DebugSnapshot& stop) {
                stopped = stop.frames[0].location.source.line;
                return stop.command(source::DebugAction::continue_execution);
            },
            {.break_on_entry = false});
        take(debugger.set_breakpoints({*at}));
        return std::get<std::int32_t>(*take(lease.invoke(receiver, five, nullptr, &debugger))) == 2 && stopped == line;
    };
    publish(program, snapshot(0, "1"));
    const auto a = take(runtime.create(program.classes()[0].type));
    auto first = take(program.pin("game.A.run"));
    require(fault(first, a.id(), 3, 1), "A fault lost its source location.");
    publish(program, snapshot(2, "2"), {.debugging = true});
    auto second = take(program.pin("game.A.run"));
    require(second.address() != first.address() && stop_line(second, a.id(), 5, 3),
            "Changed compile options reused code compiled without them.");
    const auto code = program.entries()[0].code;
    // A new class makes this request publish a type, so reuse also runs through the type-bound Contract.
    publish(program, snapshot(4, "3", "class game.C : procedural.Base { int run(int x) { return x; } }\n"),
            {.debugging = true});
    auto third = take(program.pin("game.A.run"));
    require(program.classes().size() == 3 && third.address() == second.address() && program.entries()[0].code == code,
            "An unchanged class unit was compiled again.");
    require(fault(third, a.id(), 7, 5) && stop_line(third, a.id(), 7, 5),
            "Reused code mapped faults or breakpoints through the files it was compiled from.");
    require(fault(first, a.id(), 3, 1) && fault(second, a.id(), 5, 3) &&
                std::get<std::int32_t>(*take(second.invoke(a.id(), five))) == 2,
            "Leases on older modules stopped working.");
    // The second module and its Contract are released; the reused code must not refer to them.
    first = {};
    second = {};
    third = {};
    const auto entry = take(program.entry("game.A.run"));
    require(std::get<std::int32_t>(*take(entry.invoke(a.id(), five))) == 2 &&
                fault(take(program.pin("game.A.run")), a.id(), 7, 5),
            "Reused code failed once the module that compiled it was released.");
}
}

void verify_source_projects() {
    Runtime runtime;
    const auto world_types = take(w::register_types(runtime));
    const auto base = world_types.component;
    Program program(runtime);
    auto bad = sources();
    bad.files[1].source.replace(bad.files[1].source.find("self.begins + 1"), 15, "unknown_value");
    auto bad_request = take(program.request(bad));
    auto failed = Program::prepare(bad_request);
    require(!failed && failed.error().source && failed.error().source->path == "Source/Move.source" &&
                failed.error().source->version == 23 && failed.error().source->line > 1,
            "Project diagnostic lost its original file/version/location.");
    require(!runtime.find_type("game.Move") && program.classes().empty(),
            "Failed preparation published a partial class.");
    auto input = sources();
    auto request = take(program.request(input));
    input.files[1].source.clear();
    auto candidate = take(Program::prepare(request));
    require(!runtime.find_type("game.Move"), "Preparing a project made its type visible.");
    require(candidate.classes().size() == 1 && candidate.classes()[0].base == base && candidate.entries().size() == 4,
            "Candidate omitted metadata required for host validation before commit.");
    for (const auto& entry : candidate.entries())
        if (entry.name == "game.Move.tick")
            require(entry.signature.receiver && entry.signature.receiver->type == candidate.classes()[0].type &&
                        !entry.signature.result && entry.signature.parameters.size() == 1,
                    "Candidate lifecycle signature did not use the prepared class identity.");
    const auto parameter_names = [](std::span<const source::EntryInfo> entries, std::string_view name) {
        return std::ranges::find(entries, name, &source::EntryInfo::name)->parameter_names;
    };
    require(parameter_names(candidate.entries(), "distance") == std::vector<std::string>{"speed", "dt"} &&
                parameter_names(candidate.entries(), "game.Move.tick") == std::vector<std::string>{"dt"} &&
                parameter_names(candidate.entries(), "game.Move.begin").empty(),
            "Project entries did not carry their declared parameter names.");
    take(program.try_publish(candidate));
    require(candidate.classes().empty() && candidate.entries().empty(),
            "Consumed candidate retained a second published metadata view.");
    require(program.classes().size() == 1 && program.entries().size() == 4,
            "Project discovery lost a class or typed entry.");
    const auto type = program.classes()[0].type;
    require(program.classes()[0].base == base && take(runtime.is_a(type, base)),
            "Source class did not share native inheritance.");
    w::World world(runtime, world_types);
    auto owner = take(w::create_entity(runtime));
    auto instance = take(w::create_component(runtime, type));
    take(world.set_transform(owner.id(), {}));
    const std::array capabilities{w::Attachment{instance.id()}};
    const auto composed = take(world.attach_components(owner.id(), capabilities));
    require(!composed.notification_error, "Source class capability assembly failed.");
    const auto attached = take(world.attach(owner.id()));
    require(!attached.notification_error, "Source owner Entity attachment failed.");
    const auto speed = take(runtime.find_field(type, "speed"));
    const auto begins = take(runtime.find_field(type, "begins"));
    require(std::get<float>(take(runtime.read(instance.id(), speed))) == 2.f,
            "Runtime create did not apply a constant field default.");
    require(std::get<double>(take(runtime.read(instance.id(), take(runtime.find_field(type, "precise"))))) == 2.0 &&
                std::get<std::int32_t>(
                    take(runtime.read(instance.id(), take(runtime.find_field(type, "signed_value"))))) == 3 &&
                std::get<bool>(take(runtime.read(instance.id(), take(runtime.find_field(type, "enabled"))))),
            "Double, signed integer or bool field defaults were not applied.");
    const auto* info = take(runtime.describe(type));
    bool default_found = false;
    for (const auto& field : info->fields)
        if (field.id == speed)
            default_found = field.default_leaves == std::vector<Scalar>{2.f};
    require(default_found, "Runtime metadata did not describe the default.");
    const auto signature = take(program.describe("game.Move.tick"));
    require(signature.receiver && signature.receiver->type == type && signature.parameters.size() == 1,
            "Named method did not retain its exact receiver/signature.");
    auto begin = take(program.entry("game.Move.begin"));
    auto tick = take(program.entry("game.Move.tick"));
    auto end = take(program.entry("game.Move.end"));
    auto old = take(program.pin("game.Move.tick"));
    bool replay_old{};
    take(world.bind_behavior(type, {.begin = [begin](Runtime&, ObjectId id) -> Result<void> {
                                        auto result = begin.invoke(id, {});
                                        if (!result)
                                            return std::unexpected(result.error());
                                        return {};
                                    },
                                    .tick = [&](Runtime&, ObjectId id, float dt) -> Result<void> {
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
                                    },
                                    .end = [end](Runtime&, ObjectId id) -> Result<void> {
                                        auto result = end.invoke(id, {});
                                        if (!result)
                                            return std::unexpected(result.error());
                                        return {};
                                    }}));
    const auto started = take(world.start());
    require(!started.notification_error, "Source lifecycle Begin failed.");
    require(std::get<std::uint32_t>(take(runtime.read(instance.id(), begins))) == 1,
            "Begin did not use the real instance.");
    const std::array args{Scalar{.5f}};
    auto wrong_receiver = take(runtime.create(base));
    require(!tick.invoke(wrong_receiver.id(), args),
            "Source method accepted its native base as an exact derived receiver.");
    take(world.update(.5f));
    const auto x = [&] { return take(w::read_transform(runtime, owner.id())).position.x; };
    require(x() == 1.f, "Cross-file helper or inherited native action failed.");
    take(runtime.write(instance.id(), speed, 4.f));
    publish(program, sources(2));
    replay_old = true;
    take(world.update(.5f));
    replay_old = false;
    require(x() == 7.f && std::get<float>(take(runtime.read(instance.id(), speed))) == 4.f,
            "Batch hot update lost identity, state or old code lease.");
    auto reordered = sources(2);
    auto& body = reordered.files[1].source;
    const std::string begin_method = "    void begin() { self.begins = self.begins + 1; }\n";
    body.erase(body.find(begin_method), begin_method.size());
    body.insert(body.rfind('}'), begin_method);
    // Renaming a parameter is compatible and republishes the new name.
    const std::string tick_method = "void tick(float dt) { self.translate_owner(distance(self.speed,dt),";
    body.replace(body.find(tick_method), tick_method.size(),
                 "void tick(float step) { self.translate_owner(distance(self.speed,step),");
    publish(program, reordered);
    require(parameter_names(program.entries(), "game.Move.tick") == std::vector<std::string>{"step"},
            "A parameter rename did not publish compatibly with the new name.");
    take(begin.invoke(instance.id(), {}));
    require(std::get<std::uint32_t>(take(runtime.read(instance.id(), begins))) == 2,
            "Declaration reordering changed a bound entry's stable slot.");
    auto changed = sources();
    const auto at = changed.files[1].source.find("float speed");
    changed.files[1].source.replace(at, 5, "double");
    failed = Program::prepare(take(program.request(changed)));
    require(!failed && failed.error().code == ErrorCode::rebuild_required,
            "Field layout change did not require a rebuild.");
    changed = sources();
    const auto signature_at = changed.files[1].source.find("tick(float");
    changed.files[1].source.replace(signature_at, 10, "tick(double");
    failed = Program::prepare(take(program.request(changed)));
    require(!failed && failed.error().code == ErrorCode::rebuild_required,
            "Method signature change did not require a rebuild.");
    changed = sources();
    changed.files[0].source.replace(0, 5, "double");
    failed = Program::prepare(take(program.request(changed)));
    require(!failed && failed.error().code == ErrorCode::rebuild_required,
            "Global signature change was hidden by a body diagnostic.");
    failed = Program::prepare(take(program.request(bad)));
    require(!failed, "Invalid replacement compiled.");
    take(world.update(.5f));
    require(x() == 11.f, "Failed candidate replaced the active code.");
    auto stale = take(Program::prepare(take(program.request(sources(3)))));
    auto latest = take(Program::prepare(take(program.request(sources(4)))));
    require(!program.try_publish(stale), "An older project snapshot published.");
    {
        auto execution = take(ExecutionScope::enter(runtime));
        require(!program.try_publish(latest) && latest, "Busy publication consumed a candidate.");
    }
    take(program.try_publish(latest));
    require(!program.try_publish(latest) && !latest, "Repeated project publication was accepted.");
    // Compatible growth while objects run: a struct, a global function, a class and a
    // method publish together at a type safe point; published slots and types stay valid.
    const auto catalog_size = runtime.type_count();
    auto grown = sources(4);
    grown.files[0].source +=
        "\nstruct game.Pair { float a; float b; };\nfloat doubled(float v) { return v * 2.0f; }\n"
        "float pair_sum(float a, float b) { game.Pair pair; pair.a = a; pair.b = b; return pair.a + pair.b; }\n";
    grown.files[1].source.insert(grown.files[1].source.rfind('}'),
                                 "    float boosted() { return doubled(self.speed); }\n");
    grown.files.push_back(
        {"Source/Spin.source", 31,
         "class game.Spin : world.Component {\n    float rate = 3.0f;\n    float spin() { return doubled(self.rate); }\n}\n"});
    auto growth = take(Program::prepare(take(program.request(grown))));
    take(runtime.begin_collection());
    require(!program.try_publish(growth) && growth && !runtime.find_type("game.Spin"),
            "Type publication ignored an active collection.");
    while (take(runtime.collect_step({64})).phase != CollectionPhase::idle) {
    }
    take(program.try_publish(growth));
    const auto spin = take(runtime.find_type("game.Spin"));
    require(runtime.type_count() == catalog_size + 2 && take(runtime.describe(type)) == info &&
                program.classes().size() == 2 &&
                std::get<float>(*take(take(program.entry("game.Move.boosted")).invoke(instance.id(), {}))) == 8.f,
            "Compatible growth replaced published metadata or missed a new method.");
    auto spinner = take(w::create_component(runtime, spin));
    const std::array pair_args{Scalar{1.5f}, Scalar{2.f}};
    require(std::get<float>(*take(take(program.entry("game.Spin.spin")).invoke(spinner.id(), {}))) == 6.f &&
                std::get<float>(*take(take(program.entry("pair_sum")).invoke({}, pair_args))) == 3.5f,
            "A class, struct or function added after objects existed did not run.");
    take(begin.invoke(instance.id(), {}));
    require(std::get<std::uint32_t>(take(runtime.read(instance.id(), begins))) == 3,
            "Compatible growth moved a bound entry slot.");
    failed = Program::prepare(take(program.request(sources(4))));
    require(!failed && failed.error().code == ErrorCode::rebuild_required,
            "Removing published classes or exports did not require a rebuild.");
    take(end.invoke(instance.id(), {}));
    require(!std::get<bool>(take(runtime.read(instance.id(), take(runtime.find_field(type, "enabled"))))),
            "End did not use the real instance.");
    const auto stopped = take(world.stop());
    require(!stopped.notification_error, "Source lifecycle End failed.");
    program.close();
    program.close();
    require(!tick.invoke(instance.id(), args), "Closed program allowed a new entry call.");
    take(world.bind_behavior(type, {.tick = [old](Runtime&, ObjectId id, float dt) -> Result<void> {
                                 const std::array values{Scalar{dt}};
                                 auto result = old.invoke(id, values);
                                 if (!result)
                                     return std::unexpected(result.error());
                                 return {};
                             }}));
    const auto resumed = take(world.start());
    require(!resumed.notification_error, "Pinned World behavior failed to resume.");
    take(world.update(.5f));
    require(x() == 13.f, "Pinned source lease failed to translate its real owner Entity after Program close.");
    const auto closed = take(world.close());
    require(!closed.notification_error, "Pinned World behavior cleanup failed.");
    const auto destroyed = take(w::destroy_entity(runtime, owner.id()));
    require(!destroyed.notification_error, "Source owner Entity termination failed.");
    require(!old.invoke(instance.id(), args), "Lease accepted a destroyed receiver.");
    verify_declaration_only_project();
    verify_project_borrow_effects();
    verify_project_entry_codes();
    verify_project_self_reads();
    verify_project_class_fields();
    verify_project_field_ranges();
    verify_editor_reserved_types();
    verify_project_engine_sources();
    verify_project_compile_units();
    verify_project_code_reuse();
    std::puts(
        "Source project consumer: PASS (files, native classes/defaults, typed entries, atomic hot update, entry codes/revision, diagnostics, lifetime, "
        "read-only self fields in value regions with borrow and GC rules unchanged, vector/enum class fields with attributes, "
        "engine sources with qualified names and reserved namespaces, compile units that follow only what they reach, unchanged units that keep their code)");
}
