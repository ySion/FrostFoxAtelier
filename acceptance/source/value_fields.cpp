#include "../helpers.hpp"
#include "source/native_values.hpp"
#include "source/program.hpp"

#include <array>
#include <cstdio>
#include <new>
#include <stdexcept>

namespace {
using namespace atelier;

using namespace atelier::acceptance::borrow;

using source::Program;

struct WrappedParticle {
    Particle value;
};

struct ReferencingField {
    ReferenceSlot target;
};

using namespace atelier::acceptance::helpers;

void publish(Program& program, std::string_view source, std::string_view entry = {}) {
    auto candidate = take(Program::prepare(take(program.request("value-fields.source", source, entry))));
    take(program.try_publish(candidate));
}

std::uint32_t number(const std::optional<Scalar>& value) {
    return std::get<std::uint32_t>(*value);
}

const FieldInfo& field(const TypeInfo& type, std::string_view name) {
    for (const auto& value : type.fields)
        if (value.name == name)
            return value;
    throw std::runtime_error("Missing composed source field.");
}

// Host-owned, correctly aligned storage; Runtime owns the value's lifecycle contract.
struct StoredValue {
    Runtime& runtime;

    const TypeInfo& type;

    std::byte* data;

    StoredValue(Runtime& owner, TypeId id, const void* source = nullptr)
        : runtime(owner), type(*take(owner.describe(id))),
          data(static_cast<std::byte*>(::operator new(type.size, std::align_val_t{type.alignment}))) {
        const auto result =
            source ? runtime.copy_construct_value(id, bytes(), {static_cast<const std::byte*>(source), type.size})
                   : runtime.initialize_value(id, bytes());
        if (!result) {
            ::operator delete(data, std::align_val_t{type.alignment});
            throw std::runtime_error(result.error().message);
        }
    }

    StoredValue(const StoredValue&) = delete;

    ~StoredValue() {
        if (!runtime.destroy_value(type.id, bytes()))
            std::terminate();
        ::operator delete(data, std::align_val_t{type.alignment});
    }

    std::span<std::byte> bytes() const { return {data, type.size}; }

    template <class T>
    T& at(std::size_t offset) const {
        return *reinterpret_cast<T*>(data + offset);
    }
};

constexpr std::string_view schema = R"(

    struct composed.Bundle { composed.Payload payload; uint processed; };
    struct composed.Payload {
        String title;
        borrow.Particle leader;
        Array<borrow.Particle> particles;
        Array<Array<float>> batches;
        Array<String> names;
        shared.Point prior;
    };
)";
constexpr std::string_view body = R"(
    uint process(composed.Bundle& input, uint mode) {
        input.processed = input.processed + 1;
        input.payload.particles[0].count = input.payload.particles[0].count + 1;
        uint baseline = self.live_count();
        composed.Bundle local = input;
        if (self.live_count() != baseline + 3) { return 901; }
        if (mode == 1) { return local.payload.particles.size(); }
        {
            composed.Bundle empty;
            if (empty.payload.title.size() != 0) { return 902; }
            if (empty.payload.particles.size() != 0) { return 903; }
            if (empty.payload.prior.revision != 0) { return 904; }
            empty = local;
            empty.payload = local.payload;
            if (self.live_count() != baseline + 6) { return 905; }
        }
        if (self.live_count() != baseline + 3) { return 906; }
        local.payload.title = "source/independent/long-title";
        local.payload.title.append("/suffix");
        local.payload.leader.name = "source/leader/long-name";
        local.payload.particles = input.payload.particles;
        local.payload.particles[0].name = "source/particle/long-name";
        local.payload.particles.add(local.payload.leader);
        local.payload.names[0] = "source/array-element/long-name";
        local.payload.names[1] = local.payload.names[0];
        local.payload.names[1] = local.payload.names[1];
        local.payload.batches[0] = input.payload.batches[1];
        local.payload.batches[0].add(9.0);
        local.payload.batches = local.payload.batches;
        local.payload.prior.revision = 77;
        if (local.payload.title[0] != 115) { return 907; }
        if (local.payload.names[1][0] != 115) { return 908; }
        if (local.payload.batches[0].size() != 2) { return 909; }
        if (input.payload.title[0] != 104) { return 910; }
        if (input.payload.names[0][0] != 104) { return 911; }
        if (input.payload.batches[0].size() != 1) { return 912; }
        if (input.payload.prior.revision != 4) { return 913; }
        return inspect(local, mode);
    }
    uint inspect(const composed.Bundle& input, uint mode) {
        composed.Bundle copy = input;
        if (mode == 2) { copy.payload.particles[100].count = 0; }
        return copy.payload.particles.size();
    }
)";
}

void verify_source_value_fields() {
    const auto initial_live = Particle::live;
    Runtime runtime;
    auto particle = reflection::Definition<Particle>::build(runtime);
    particle.native.value_operations_preserve_borrows = true;
    take(runtime.register_types({std::move(particle), reflection::Definition<Position>::build(runtime),
                                 reflection::Definition<Algorithms>::build(runtime), string_definition(runtime),
                                 array_definition<Particle>(runtime, "composed.Particles"),
                                 array_definition<float>(runtime, "composed.Floats"),
                                 array_definition<Array<float>>(runtime, "composed.Batches"),
                                 array_definition<String>(runtime, "composed.Strings")}));
    Program dependency(runtime, {});
    publish(dependency, "struct shared.Point { uint revision; }; uint marker() { return 0; }");
    const auto host_type = take(runtime.native_type<Algorithms>());
    Program program(runtime, host_type);
    publish(program, std::string(schema) + std::string(body), "process");
    const auto bundle = take(runtime.find_type("composed.Bundle"));
    const auto& info = *take(runtime.describe(bundle));
    const auto& payload = *take(runtime.describe(field(info, "payload").value.type));
    const auto base = *field(info, "payload").offset;
    const auto offset = [&](std::string_view name) { return base + *field(payload, name).offset; };
    require(!info.native_storage && info.alignment == alignof(Particle) && info.value_operations_preserve_borrows &&
                field(payload, "title").value.type == take(runtime.native_type<String>()) &&
                field(payload, "particles").value.type == take(runtime.native_type<Array<Particle>>()) &&
                field(payload, "prior").value.type == take(runtime.find_type("shared.Point")),
            "Composed fields lost shared type identities, transitive ownership or native alignment.");

    // Reject unsupported declarations before object creation seals registration.
    Program rejected(runtime, {});
    const auto type_count = runtime.types().size();
    constexpr std::array invalid{
        "struct Bad { Array<int> values; }; uint run() { return 0; }",
        "struct Bad { Array<New> values; }; struct New { uint value; }; uint run() { return 0; }",
        "struct Bad { borrow.Algorithms object; }; uint run() { return 0; }",
        "struct Bad { const String& text; }; uint run() { return 0; }",
        "struct Bad { Array<float> values; }; void run(Bad& value) { value.values.clear(); }",
        "struct Bad { String text; }; void run(Bad& value) { value.text = \"replace\"; }",
        "struct Bad { Array<float> values; }; void run(const Bad& value) { value.values[0] = 1.0; }",
        "struct Bad { String text; }; async void run() { Bad local; yield; }"};
    for (const auto* source : invalid) {
        require(!Program::prepare(take(rejected.request("invalid-fields.source", source))) &&
                    runtime.types().size() == type_count && !runtime.find_type("Bad"),
                "Rejected value composition published types or escaped its ownership/borrow contract.");
    }
    const auto host = take(runtime.create(host_type));
    {
        StoredValue input(runtime, bundle);
        auto& title = input.at<String>(offset("title"));
        auto& leader = input.at<Particle>(offset("leader"));
        auto& particles = input.at<Array<Particle>>(offset("particles"));
        auto& names = input.at<Array<String>>(offset("names"));
        auto& batches = input.at<Array<Array<float>>>(offset("batches"));
        title.assign("host/original/long-title");
        leader.name.assign("host/leader/long-name");
        particles.resize(2);
        particles[0].name.assign("host/particle/long-name");
        names.add(String{"host/first/long-name"});
        names.add(String{"host/second/long-name"});
        batches.resize(2);
        batches[0].add(1.0f);
        batches[1].add(2.0f);
        input.at<std::uint32_t>(offset("prior")) = 4;
        const auto* particles_data = particles.data();
        const auto baseline = Particle::live;
        std::array args{Argument::borrow(bundle, input.data, input.data), Argument{Scalar{std::uint32_t{0}}}};
        require(number(take(program.entry().invoke(host.id(), args))) == 3 && Particle::live == baseline,
                "Composed scoped values, nested assignments or helper cleanup failed.");
        args[1] = Argument{Scalar{std::uint32_t{1}}};
        require(number(take(program.entry().invoke(host.id(), args))) == 2 && Particle::live == baseline,
                "Early return retained composed owned fields.");
        args[1] = Argument{Scalar{std::uint32_t{2}}};
        require(!program.entry().invoke(host.id(), args) && Particle::live == baseline,
                "Helper failure did not clean both frames' composed values.");
        args[1] = Argument{Scalar{std::uint32_t{0}}};
        Particle::copy_budget = 2; // leader and one array element construct before the failure
        const auto failed = program.entry().invoke(host.id(), args);
        Particle::copy_budget = -1;
        require(!failed && Particle::live == baseline, "Partially constructed composed record leaked fields.");
        Particle::copy_budget = 3; // local completes; assigning its array into an empty record fails
        const auto failed_assignment = program.entry().invoke(host.id(), args);
        Particle::copy_budget = -1;
        require(!failed_assignment && Particle::live == baseline, "Failed composed assignment left live owned fields.");
        require(particles.data() == particles_data && particles.size() == 2 && particles[0].count == 5 &&
                    title.view() == "host/original/long-title" && leader.name.view() == "host/leader/long-name" &&
                    particles[0].name.view() == "host/particle/long-name" &&
                    names[0].view() == "host/first/long-name" && batches[0].size() == 1 &&
                    input.at<std::uint32_t>(*field(info, "processed").offset) == 5,
                "Borrowed leaf writes, failure side effects or independent copies changed host storage incorrectly.");
        {
            StoredValue copied(runtime, bundle, input.data);
            require(copied.at<Array<Particle>>(offset("particles")).data() != particles_data &&
                        copied.at<String>(offset("title")).c_str() != title.c_str(),
                    "Host Runtime copy shared owned field buffers.");
            title.assign("host/updated/long-title");
            take(runtime.assign_value(bundle, copied.bytes(), input.bytes()));
            require(copied.at<String>(offset("title")).view() == title.view(), "Host assignment lost composed fields.");
        }
        require(Particle::live == baseline, "Host Runtime destruction leaked composed fields.");
        const auto old = take(program.pin());
        publish(program, std::string(schema) +
                             "uint process(composed.Bundle& input, uint mode) { return input.payload.names.size(); }");
        require(number(take(program.entry().invoke(host.id(), args))) == 2 && take(runtime.describe(bundle)) == &info,
                "Compatible update changed composed type identity.");
        title.assign("host/original/long-title");
        require(number(take(old.invoke(host.id(), args))) == 3 && Particle::live == baseline,
                "Retired code lost composed value metadata or cleanup.");
    }
    require(Particle::live == initial_live, "Composed host storage retained native fields after scope exit.");

    // Layout eligibility does not grant untrusted native lifecycle effects.
    Runtime untrusted;
    auto wrapper = NativeRegistration<WrappedParticle>{untrusted, "unsafe.Wrapper", TypeKind::record}
                       .field<&WrappedParticle::value>("value")
                       .build();
    wrapper.native.value_operations_preserve_borrows = true;
    take(untrusted.register_types(
        {reflection::Definition<Particle>::build(untrusted), reflection::Definition<Position>::build(untrusted),
         string_definition(untrusted), std::move(wrapper),
         NativeRegistration<ReferencingField>{untrusted, "unsafe.References", TypeKind::record}
             .field<&ReferencingField::target>("target", {TypeReference{"core.Object"}, ValueForm::strong_reference})
             .build()}));
    Program unsafe(untrusted, {});
    require(!Program::prepare(take(unsafe.request(
                "unsafe-fields.source", "struct Bad { borrow.Particle particle; }; void run() { Bad local; }"))) &&
                !untrusted.find_type("Bad"),
            "Composition granted ownership to native operations without a borrow-preservation contract.");
    require(!Program::prepare(take(unsafe.request("unsafe-nested-assignment.source",
                                                  "void run() { unsafe.Wrapper local; local.value = local.value; }"))),
            "Outer ownership incorrectly granted safe assignment to an untrusted native field.");
    require(!Program::prepare(
                take(unsafe.request("gc-fields.source", "struct Bad { unsafe.References value; }; void run() {}"))) &&
                !untrusted.find_type("Bad"),
            "Source value composition admitted a nested GC reference.");
    std::puts(
        "Source value fields: PASS (shared storage, deep copies, scoped cleanup, partial failure, nested mutation, frozen types)");
}
