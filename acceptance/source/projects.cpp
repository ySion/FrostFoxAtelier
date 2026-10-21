#include "source/program.hpp"
#include "runtime/native.hpp"
#include "runtime/foundation.hpp"
#include "world/world.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace {
using namespace atelier;
using source::Program;
void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
template<class T> T take(Result<T> result) { if (!result) throw std::runtime_error(result.error().message); return std::move(*result); }
void take(Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }
namespace w = atelier::world;
source::SourceSnapshot sources(float factor = 1) {
    return {"project", 5, {
        {"Source/Math.source", 17, "float distance(float speed, float dt) { return speed * dt * " + std::to_string(factor) + "f; }"},
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
void publish(Program& program, source::SourceSnapshot snapshot) {
    auto candidate = take(Program::prepare(take(program.request(std::move(snapshot)))));
    take(program.try_publish(candidate));
    require(!candidate, "Published project candidate retained an independently publishable result.");
}
void verify_declaration_only_project() {
    Runtime runtime;
    take(w::register_types(runtime));
    Program program(runtime);
    source::SourceSnapshot input{"data", 1, {{"Data.source", 7,
        "const float bad = missing; class game.Settings : world.Component { float speed = 2.0f; }"}}};
    auto rejected = Program::prepare(take(program.request(input)));
    require(!rejected && rejected.error().source && rejected.error().source->path == "Data.source" &&
        !runtime.find_type("game.Settings"), "Declaration-only project bypassed typed validation or partially published.");
    input.files[0].source = "const float __source_validate = 1.0f; class game.Settings : world.Component { float speed = -0.0f; double value = 0.0d / 0.0d; }";
    publish(program, input);
    require(program.entries().empty() && program.classes().size() == 1, "Declaration-only project exported a synthetic entry.");
    const auto type = program.classes()[0].type;
    auto instance = take(runtime.create(type));
    const auto speed = take(runtime.find_field(type,"speed"));
    require(std::signbit(std::get<float>(take(runtime.read(instance.id(), speed)))) &&
        std::isnan(std::get<double>(take(runtime.read(instance.id(), take(runtime.find_field(type,"value")))))),
        "Runtime scalar defaults lost their floating representation.");
    publish(program, input); // Same NaN representation must remain compatible.
    input.files[0].source.replace(input.files[0].source.find("-0.0f"), 5, "0.0f");
    rejected = Program::prepare(take(program.request(input)));
    require(!rejected && rejected.error().code == ErrorCode::rebuild_required, "Changed signed-zero default was silently ignored.");
    input.files[0].path = "Data.source/";
    require(!Program::prepare(take(program.request(input))), "A directory-like source path was accepted.");
    Program phases(runtime);
    source::SourceSnapshot staged{"phases", 1, {{"Phases.source", 1,
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
    const auto base = take(runtime.register_type(NativeRegistration<RangedBody>(runtime, "project.RangedBody", TypeKind::object)
        .field<&RangedBody::speed>("speed").range({.ui_max = Scalar{5.0f}, .hard_min = Scalar{0.0f}, .hard_max = Scalar{10.0f}})
        .field<&RangedBody::loose>("loose")
        .field<&RangedBody::tint>("tint").range({.hard_min = Scalar{0.0f}, .hard_max = Scalar{1.0f}})
        .method<&RangedBody::bump>("bump").build()));
    Program program(runtime);
    for (const auto* body : {"self.bump(self.speed);", "self.bump(self.tint.x);"}) {
        source::SourceSnapshot input{"ranges", 1, {{"Ranges.source", 1,
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
        take(runtime.read(id, loose)) == Scalar{1.0f}, "Source field writes bypassed hard bounds or broke unconstrained borrows.");
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
    take(runtime.register_types({string_definition(runtime),
        NativeRegistration<CollectionProbe>(runtime,"project.Probe",TypeKind::object)
            .method<&CollectionProbe::collecting>("collecting",true).build()}));
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
    for (int i = 0; i < 64; ++i) objects.push_back(take(runtime.create(type)));
    const auto id = objects.front().id();
    take(runtime.with_native<CollectionProbe>(id,[&](auto& probe) -> Result<void> { probe.runtime = &runtime; return {}; }));
    take(runtime.begin_collection());
    String label("borrowed");
    const std::array args{Argument::borrow(std::as_const(label))};
    require(std::get<bool>(*take(take(program.entry("game.Probe.borrowed")).invoke(id,args))) &&
        runtime.collection_phase() == CollectionPhase::roots, "Shared helper advanced GC inside a borrowed-value entry.");
    require(!std::get<bool>(*take(take(program.entry("game.Probe.work")).invoke(id,{}))),
        "Unrelated borrowed entry suppressed an ordinary loop's GC progress.");
}
struct EditorPanel { float x; };
struct ProjectHost { float x; };
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
    take(runtime.register_types({
        record("editor.Probe"),
        NativeRegistration<EditorPanel>(runtime, "editor.Panel", TypeKind::object).build(),
        std::move(mode),
        record("project.Reading"),
        NativeRegistration<ProjectHost>(runtime, "project.Host", TypeKind::object).build(),
        std::move(level)}));
    Program program(runtime);
    const auto rejected = [&](std::string text, std::string_view expected, const char* message) {
        auto failed = Program::prepare(take(program.request(source::SourceSnapshot{"editor", 1, {{"Reserved.source", 1, std::move(text)}}})));
        if (failed || failed.error().code != ErrorCode::invalid_definition || failed.error().message.find(expected) == std::string::npos)
            throw std::runtime_error(std::string(message) + (failed ? "" : " Got: " + failed.error().message));
    };
    rejected("float read() { editor.Probe probe; return probe.x; }", "Unknown registered value type 'editor.Probe'",
        "Project Source used an editor. record.");
    rejected("bool pick() { return editor.Mode::a == editor.Mode::b; }", "Unknown enum type",
        "Project Source used an editor. enumeration.");
    rejected("struct game.Holder { editor.Probe probe; };", "editor.Probe", "A project struct held an editor. record.");
    rejected("class game.Panel : editor.Panel { }", "Unknown native base type: editor.Panel",
        "A project class derived from an editor. object type.");
    rejected("struct editor.Mine { float a; };", "reserved for editor types", "Project Source declared an editor. struct.");
    rejected("enum editor.Choice { A }", "reserved for editor types", "Project Source declared an editor. enumeration.");
    rejected("class editor.Tool : project.Host { }", "reserved for editor types", "Project Source declared an editor. class.");
    require(!runtime.find_type("game.Holder") && !runtime.find_type("game.Panel"), "A rejected project published a type.");
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
}

void verify_source_projects() {
    Runtime runtime;
    const auto world_types=take(w::register_types(runtime));
    const auto base=world_types.component;
    Program program(runtime);
    auto bad = sources(); bad.files[1].source.replace(bad.files[1].source.find("self.begins + 1"), 15, "unknown_value");
    auto bad_request = take(program.request(bad));
    auto failed = Program::prepare(bad_request);
    require(!failed && failed.error().source && failed.error().source->path == "Source/Move.source" &&
        failed.error().source->version == 23 && failed.error().source->line > 1,
        "Project diagnostic lost its original file/version/location.");
    require(!runtime.find_type("game.Move") && program.classes().empty(), "Failed preparation published a partial class.");
    auto input = sources();
    auto request = take(program.request(input)); input.files[1].source.clear();
    auto candidate = take(Program::prepare(request));
    require(!runtime.find_type("game.Move"), "Preparing a project made its type visible.");
    require(candidate.classes().size() == 1 && candidate.classes()[0].base == base && candidate.entries().size() == 4,
        "Candidate omitted metadata required for host validation before commit.");
    for (const auto& entry : candidate.entries()) if (entry.name == "game.Move.tick")
        require(entry.signature.receiver && entry.signature.receiver->type == candidate.classes()[0].type &&
            !entry.signature.result && entry.signature.parameters.size() == 1,
            "Candidate lifecycle signature did not use the prepared class identity.");
    take(program.try_publish(candidate));
    require(candidate.classes().empty() && candidate.entries().empty(), "Consumed candidate retained a second published metadata view.");
    require(program.classes().size() == 1 && program.entries().size() == 4, "Project discovery lost a class or typed entry.");
    const auto type = program.classes()[0].type;
    require(program.classes()[0].base == base && take(runtime.is_a(type,base)), "Source class did not share native inheritance.");
    w::World world(runtime,world_types);
    auto owner=take(w::create_entity(runtime));
    auto instance=take(w::create_component(runtime,type));
    take(world.set_transform(owner.id(),{}));
    const std::array capabilities{w::Attachment{instance.id()}};
    const auto composed=take(world.attach_components(owner.id(),capabilities));
    require(!composed.notification_error,"Source class capability assembly failed.");
    const auto attached=take(world.attach(owner.id()));
    require(!attached.notification_error,"Source owner Entity attachment failed.");
    const auto speed = take(runtime.find_field(type,"speed"));
    const auto begins = take(runtime.find_field(type,"begins"));
    require(std::get<float>(take(runtime.read(instance.id(),speed))) == 2.f, "Runtime create did not apply a constant field default.");
    require(std::get<double>(take(runtime.read(instance.id(),take(runtime.find_field(type,"precise"))))) == 2.0 &&
        std::get<std::int32_t>(take(runtime.read(instance.id(),take(runtime.find_field(type,"signed_value"))))) == 3 &&
        std::get<bool>(take(runtime.read(instance.id(),take(runtime.find_field(type,"enabled"))))),
        "Double, signed integer or bool field defaults were not applied.");
    const auto* info = take(runtime.describe(type));
    bool default_found = false;
    for (const auto& field : info->fields) if (field.id == speed) default_found = field.default_value && std::get<float>(*field.default_value) == 2.f;
    require(default_found, "Runtime metadata did not describe the default.");
    const auto signature = take(program.describe("game.Move.tick"));
    require(signature.receiver && signature.receiver->type == type && signature.parameters.size() == 1,
        "Named method did not retain its exact receiver/signature.");
    auto begin = take(program.entry("game.Move.begin"));
    auto tick = take(program.entry("game.Move.tick"));
    auto end = take(program.entry("game.Move.end"));
    auto old = take(program.pin("game.Move.tick"));
    bool replay_old{};
    take(world.bind_behavior(type,{
        .begin=[begin](Runtime&,ObjectId id)->Result<void>{auto result=begin.invoke(id,{});if(!result)return std::unexpected(result.error());return {};},
        .tick=[&](Runtime&,ObjectId id,float dt)->Result<void>{
            const std::array args{Scalar{dt}};
            auto result=tick.invoke(id,args);if(!result)return std::unexpected(result.error());
            if(replay_old){auto pinned=old.invoke(id,args);if(!pinned)return std::unexpected(pinned.error());}
            return {};
        },
        .end=[end](Runtime&,ObjectId id)->Result<void>{auto result=end.invoke(id,{});if(!result)return std::unexpected(result.error());return {};}
    }));
    const auto started=take(world.start());require(!started.notification_error,"Source lifecycle Begin failed.");
    require(std::get<std::uint32_t>(take(runtime.read(instance.id(),begins))) == 1, "Begin did not use the real instance.");
    const std::array args{Scalar{.5f}};
    auto wrong_receiver = take(runtime.create(base));
    require(!tick.invoke(wrong_receiver.id(),args), "Source method accepted its native base as an exact derived receiver.");
    take(world.update(.5f));
    const auto x=[&]{return take(w::read_transform(runtime,owner.id())).position.x;};
    require(x() == 1.f, "Cross-file helper or inherited native action failed.");
    take(runtime.write(instance.id(),speed,4.f));
    publish(program,sources(2));
    replay_old=true;take(world.update(.5f));replay_old=false;
    require(x() == 7.f &&
        std::get<float>(take(runtime.read(instance.id(),speed))) == 4.f, "Batch hot update lost identity, state or old code lease.");
    auto reordered = sources(2);
    auto& body = reordered.files[1].source;
    const std::string begin_method = "    void begin() { self.begins = self.begins + 1; }\n";
    body.erase(body.find(begin_method), begin_method.size());
    body.insert(body.rfind('}'),begin_method);
    publish(program,reordered);
    take(begin.invoke(instance.id(),{}));
    require(std::get<std::uint32_t>(take(runtime.read(instance.id(),begins))) == 2, "Declaration reordering changed a bound entry's stable slot.");
    auto changed = sources(); const auto at = changed.files[1].source.find("float speed"); changed.files[1].source.replace(at,5,"double");
    failed = Program::prepare(take(program.request(changed)));
    require(!failed && failed.error().code == ErrorCode::rebuild_required, "Field layout change did not require a rebuild.");
    changed = sources(); const auto signature_at = changed.files[1].source.find("tick(float"); changed.files[1].source.replace(signature_at,10,"tick(double");
    failed = Program::prepare(take(program.request(changed)));
    require(!failed && failed.error().code == ErrorCode::rebuild_required, "Method signature change did not require a rebuild.");
    changed = sources(); changed.files[0].source.replace(0,5,"double");
    failed = Program::prepare(take(program.request(changed)));
    require(!failed && failed.error().code == ErrorCode::rebuild_required, "Global signature change was hidden by a body diagnostic.");
    failed = Program::prepare(take(program.request(bad)));
    require(!failed, "Invalid replacement compiled."); take(world.update(.5f));
    require(x() == 11.f, "Failed candidate replaced the active code.");
    auto stale = take(Program::prepare(take(program.request(sources(3)))));
    auto latest = take(Program::prepare(take(program.request(sources(4)))));
    require(!program.try_publish(stale), "An older project snapshot published.");
    { auto execution = take(ExecutionScope::enter(runtime)); require(!program.try_publish(latest) && latest, "Busy publication consumed a candidate."); }
    take(program.try_publish(latest));
    require(!program.try_publish(latest) && !latest, "Repeated project publication was accepted.");
    // Compatible growth while objects run: a struct, a global function, a class and a
    // method publish together at a type safe point; published slots and types stay valid.
    const auto catalog_size = runtime.type_count();
    auto grown = sources(4);
    grown.files[0].source += "\nstruct game.Pair { float a; float b; };\nfloat doubled(float v) { return v * 2.0f; }\n"
        "float pair_sum(float a, float b) { game.Pair pair; pair.a = a; pair.b = b; return pair.a + pair.b; }\n";
    grown.files[1].source.insert(grown.files[1].source.rfind('}'), "    float boosted() { return doubled(self.speed); }\n");
    grown.files.push_back({"Source/Spin.source", 31,
        "class game.Spin : world.Component {\n    float rate = 3.0f;\n    float spin() { return doubled(self.rate); }\n}\n"});
    auto growth = take(Program::prepare(take(program.request(grown))));
    take(runtime.begin_collection());
    require(!program.try_publish(growth) && growth && !runtime.find_type("game.Spin"), "Type publication ignored an active collection.");
    while (take(runtime.collect_step({64})).phase != CollectionPhase::idle) {}
    take(program.try_publish(growth));
    const auto spin = take(runtime.find_type("game.Spin"));
    require(runtime.type_count() == catalog_size + 2 && take(runtime.describe(type)) == info && program.classes().size() == 2 &&
        std::get<float>(*take(take(program.entry("game.Move.boosted")).invoke(instance.id(), {}))) == 8.f,
        "Compatible growth replaced published metadata or missed a new method.");
    auto spinner = take(w::create_component(runtime, spin));
    const std::array pair_args{Scalar{1.5f}, Scalar{2.f}};
    require(std::get<float>(*take(take(program.entry("game.Spin.spin")).invoke(spinner.id(), {}))) == 6.f &&
        std::get<float>(*take(take(program.entry("pair_sum")).invoke({}, pair_args))) == 3.5f,
        "A class, struct or function added after objects existed did not run.");
    take(begin.invoke(instance.id(), {}));
    require(std::get<std::uint32_t>(take(runtime.read(instance.id(), begins))) == 3, "Compatible growth moved a bound entry slot.");
    failed = Program::prepare(take(program.request(sources(4))));
    require(!failed && failed.error().code == ErrorCode::rebuild_required, "Removing published classes or exports did not require a rebuild.");
    take(end.invoke(instance.id(),{}));
    require(!std::get<bool>(take(runtime.read(instance.id(),take(runtime.find_field(type,"enabled"))))), "End did not use the real instance.");
    const auto stopped=take(world.stop());require(!stopped.notification_error,"Source lifecycle End failed.");
    program.close(); program.close(); require(!tick.invoke(instance.id(),args), "Closed program allowed a new entry call.");
    take(world.bind_behavior(type,{.tick=[old](Runtime&,ObjectId id,float dt)->Result<void>{
        const std::array values{Scalar{dt}};auto result=old.invoke(id,values);if(!result)return std::unexpected(result.error());return {};
    }}));
    const auto resumed=take(world.start());require(!resumed.notification_error,"Pinned World behavior failed to resume.");
    take(world.update(.5f));
    require(x()==13.f,"Pinned source lease failed to translate its real owner Entity after Program close.");
    const auto closed=take(world.close());require(!closed.notification_error,"Pinned World behavior cleanup failed.");
    const auto destroyed=take(w::destroy_entity(runtime,owner.id()));require(!destroyed.notification_error,"Source owner Entity termination failed.");
    require(!old.invoke(instance.id(),args), "Lease accepted a destroyed receiver.");
    verify_declaration_only_project();
    verify_project_borrow_effects();
    verify_project_field_ranges();
    verify_editor_reserved_types();
    std::puts("Source project consumer: PASS (files, native classes/defaults, typed entries, atomic hot update, diagnostics, lifetime)");
}
