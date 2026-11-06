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

using namespace atelier::acceptance::helpers;

const FieldInfo& field(const TypeInfo& type, std::string_view name) {
    for (const auto& value : type.fields)
        if (value.name == name)
            return value;
    throw std::runtime_error("Missing composed source field.");
}

struct StoredValue {
    Runtime& runtime;

    const TypeInfo& type;

    std::byte* data;

    StoredValue(Runtime& owner, TypeId id)
        : runtime(owner), type(*take(owner.describe(id))),
          data(static_cast<std::byte*>(::operator new(type.size, std::align_val_t{type.alignment}))) {
        const auto result = runtime.initialize_value(id, {data, type.size});
        if (!result) {
            ::operator delete(data, std::align_val_t{type.alignment});
            throw std::runtime_error(result.error().message);
        }
    }

    StoredValue(const StoredValue&) = delete;

    ~StoredValue() {
        if (!runtime.destroy_value(type.id, {data, type.size}))
            std::terminate();
        ::operator delete(data, std::align_val_t{type.alignment});
    }

    template <class T>
    T& at(std::size_t offset) const {
        return *reinterpret_cast<T*>(data + offset);
    }
};
}

void verify_source_value_fields() {
    Runtime runtime;
    take(
        runtime.register_types({particle_definition(runtime), reflection::Definition<Position>::build(runtime),
                                string_definition(runtime), particle_array_definition(runtime, "composed.Particles")}));
    auto program = take(source::Program::create(runtime));
    auto candidate = take(source::Program::prepare(take(program->request("value-fields.source", R"(
        struct composed.Bundle { composed.Payload payload; uint processed; };
        struct composed.Payload {
            String title;
            borrow.Particle leader;
            Array<borrow.Particle> particles;
        };
        void process(composed.Bundle& input, uint mode) {
            input.processed = input.processed + 1;
            input.payload.particles[0].count = input.payload.particles[0].count + 1;
            composed.Bundle local = input;
            if (mode == 0) {
                composed.Bundle empty;
                empty = local;
            }
            if (mode == 2) { inspect(local); }
        }
        void inspect(const composed.Bundle& input) {
            composed.Bundle copy = input;
            copy.payload.particles[100].count = 0;
        }
    )",
                                                                         "process"))));
    take(program->try_publish(candidate));
    const auto bundle = take(runtime.find_type("composed.Bundle"));
    const auto& info = *take(runtime.describe(bundle));
    const auto& payload = *take(runtime.describe(field(info, "payload").value.type));
    const auto base = *field(info, "payload").offset;
    const auto offset = [&](std::string_view name) { return base + *field(payload, name).offset; };
    {
        StoredValue input(runtime, bundle);
        auto& title = input.at<String>(offset("title"));
        auto& leader = input.at<Particle>(offset("leader"));
        auto& particles = input.at<Array<Particle>>(offset("particles"));
        require(title.assign("host/original/long-title").has_value(), "Fixture title is invalid UTF-8.");
        require(leader.name.assign("host/leader/long-name").has_value(), "Fixture leader name is invalid UTF-8.");
        particles.resize(2);
        require(particles[0].name.assign("host/particle/long-name").has_value(),
                "Fixture particle name is invalid UTF-8.");
        const auto* particles_data = particles.data();
        std::array args{Argument::borrow(bundle, input.data, input.data), Argument{Scalar{std::uint32_t{2}}}};
        require(!program->entry().invoke({}, args) && Particle::live == 3,
                "Helper failure did not clean both frames' composed values.");
        args[1] = Argument{Scalar{std::uint32_t{1}}};
        Particle::copy_budget = 2; // Leader and one array element construct before the failure.
        const auto failed = program->entry().invoke({}, args);
        Particle::copy_budget = -1;
        require(!failed && Particle::live == 3, "Partially constructed composed record leaked fields.");
        args[1] = Argument{Scalar{std::uint32_t{0}}};
        Particle::copy_budget = 3; // Local completes; assigning its array into an empty record fails.
        const auto failed_assignment = program->entry().invoke({}, args);
        Particle::copy_budget = -1;
        require(!failed_assignment && Particle::live == 3, "Failed composed assignment left live owned fields.");
        require(particles.data() == particles_data && particles.size() == 2 && particles[0].count == 3 &&
                    title.view() == "host/original/long-title" && leader.name.view() == "host/leader/long-name" &&
                    particles[0].name.view() == "host/particle/long-name" &&
                    input.at<std::uint32_t>(*field(info, "processed").offset) == 3,
                "Failure side effects or independent copies changed host storage incorrectly.");
    }
    require(Particle::live == 0, "Composed host storage retained native fields after scope exit.");
    std::puts("Source value fields: PASS (helper, partial construction and assignment cleanup)");
}
