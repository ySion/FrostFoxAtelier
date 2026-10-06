#include "source/program.hpp"
#include "runtime/native.hpp"
#include "runtime/foundation.hpp"
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
// A real native base and typed inherited method. Project consumers supply their
// World Component definition through exactly the same registration interface.
struct Component {
    float x{};
    void translate_owner(float dx, float, float) { x += dx; }
};
source::SourceSnapshot sources(float factor = 1) {
    return {"project", 5, {
        {"Source/Math.source", 17, "float distance(float speed, float dt) { return speed * dt * " + std::to_string(factor) + "f; }"},
        {"Source/Move.source", 23, R"(
class game.Move : project.SceneComponent {
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
    take(runtime.register_type(NativeRegistration<Component>(runtime,"project.SceneComponent",TypeKind::object).build()));
    Program program(runtime);
    source::SourceSnapshot input{"data", 1, {{"Data.source", 7,
        "const float bad = missing; class game.Settings : project.SceneComponent { float speed = 2.0f; }"}}};
    auto rejected = Program::prepare(take(program.request(input)));
    require(!rejected && rejected.error().source && rejected.error().source->path == "Data.source" &&
        !runtime.find_type("game.Settings"), "Declaration-only project bypassed typed validation or partially published.");
    input.files[0].source = "const float __source_validate = 1.0f; class game.Settings : project.SceneComponent { float speed = -0.0f; double value = 0.0d / 0.0d; }";
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
}

struct CollectionProbe {
    Runtime* runtime{};
    bool collecting() const { return runtime->collection_phase() != CollectionPhase::idle; }
};
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
}

void verify_source_projects() {
    Runtime runtime;
    const auto base = take(runtime.register_type(NativeRegistration<Component>(runtime,"project.SceneComponent",TypeKind::object)
        .field<&Component::x>("x").method<&Component::translate_owner>("translate_owner",true).build()));
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
    auto instance = take(runtime.create(type));
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
    take(begin.invoke(instance.id(),{}));
    require(std::get<std::uint32_t>(take(runtime.read(instance.id(),begins))) == 1, "Begin did not use the real instance.");
    const std::array args{Scalar{.5f}};
    auto wrong_receiver = take(runtime.create(base));
    require(!tick.invoke(wrong_receiver.id(),args), "Source method accepted its native base as an exact derived receiver.");
    take(tick.invoke(instance.id(),args));
    const auto x = take(runtime.find_field(type,"x"));
    require(std::get<float>(take(runtime.read(instance.id(),x))) == 1.f, "Cross-file helper or inherited native action failed.");
    take(runtime.write(instance.id(),speed,4.f));
    publish(program,sources(2));
    take(tick.invoke(instance.id(),args)); take(old.invoke(instance.id(),args));
    require(std::get<float>(take(runtime.read(instance.id(),x))) == 7.f &&
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
    require(!failed, "Invalid replacement compiled."); take(tick.invoke(instance.id(),args));
    require(std::get<float>(take(runtime.read(instance.id(),x))) == 11.f, "Failed candidate replaced the active code.");
    auto stale = take(Program::prepare(take(program.request(sources(3)))));
    auto latest = take(Program::prepare(take(program.request(sources(4)))));
    require(!program.try_publish(stale), "An older project snapshot published.");
    { auto execution = take(ExecutionScope::enter(runtime)); require(!program.try_publish(latest) && latest, "Busy publication consumed a candidate."); }
    take(program.try_publish(latest));
    require(!program.try_publish(latest) && !latest, "Repeated project publication was accepted.");
    take(end.invoke(instance.id(),{}));
    require(!std::get<bool>(take(runtime.read(instance.id(),take(runtime.find_field(type,"enabled"))))), "End did not use the real instance.");
    program.close(); program.close(); require(!tick.invoke(instance.id(),args), "Closed program allowed a new entry call.");
    take(old.invoke(instance.id(),args));
    take(runtime.destroy(instance.id())); require(!old.invoke(instance.id(),args), "Lease accepted a destroyed receiver.");
    verify_declaration_only_project();
    verify_project_borrow_effects();
    std::puts("Source project consumer: PASS (files, native classes/defaults, typed entries, atomic hot update, diagnostics, lifetime)");
}
