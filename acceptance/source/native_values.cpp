#include "source/native_values.hpp"
#include "source/program.hpp"

#include <array>
#include <cstdio>
#include <stdexcept>

namespace {
using namespace atelier;
using namespace atelier::acceptance::borrow;
using source::Program;
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
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
void publish(Program& program, std::string_view text) {
    auto candidate = take(Program::prepare(take(program.request("native-values.source", text))));
    take(program.try_publish(candidate));
}
struct Host {
    static inline int callbacks{};
    void invalidate() { ++callbacks; }
};
struct NativeOnlyHost {
    std::uint32_t answer() const noexcept { return 42; }
    std::uint32_t text_size(const String& text) const noexcept { return static_cast<std::uint32_t>(text.size()); }
};
struct RetiredAsset {
    static inline int destroyed{};
    ~RetiredAsset() { ++destroyed; }
};
}

void verify_native_value_borrows() {
    static_assert(std::is_standard_layout_v<Particle> && alignof(Particle) == 64);
    {
        Runtime invalid;
        auto definition = reflection::Definition<Position>::build(invalid);
        definition.fields[0].native_offset = sizeof(Position);
        require(!invalid.register_type(std::move(definition)) && !invalid.native_type<Position>(),
                "Invalid direct native offset was partly published.");
    }
    {
        Runtime opaque;
        take(opaque.register_types({array_definition<Array<String>>(opaque, "nativeOnly.Batches"),
                                    array_definition<String>(opaque, "nativeOnly.Strings"),
                                    NativeRegistration<String>{opaque, "nativeOnly.String", TypeKind::string}.build(),
                                    NativeRegistration<NativeOnlyHost>{opaque, "nativeOnly.Host", TypeKind::object}
                                        .method<&NativeOnlyHost::answer>("answer")
                                        .method<&NativeOnlyHost::text_size>("text_size")
                                        .build()}));
        // Missing source projections are legal native registrations, including
        // arrays and host methods depending on them. Unrelated code still runs.
        Program scalar(opaque, {});
        publish(scalar, "uint scalar() { return 42; }");
        require(std::get<std::uint32_t>(*take(scalar.entry().invoke({}, std::array<Scalar, 0>{}))) == 42,
                "Native-only container dependencies blocked an unrelated scalar Program.");
        const auto type = take(opaque.native_type<NativeOnlyHost>());
        Program bound(opaque, type);
        publish(bound, "uint bound() { return self.answer(); }");
        auto object = take(opaque.create(type));
        require(std::get<std::uint32_t>(*take(bound.entry().invoke(object.id(), std::array<Scalar, 0>{}))) == 42,
                "A native-only method parameter blocked unrelated host bindings.");
        require(
            !Program::prepare(take(scalar.request(
                "unavailable.source", "uint unavailable(const nativeOnly.Batches& values) { return values.size(); }"))),
            "A container with an unavailable element projection entered the source catalog.");
    }
    Runtime runtime;
    take(runtime.register_types(
        {array_definition<Particle>(runtime, "borrow.Particles"), reflection::Definition<Particle>::build(runtime),
         reflection::Definition<Position>::build(runtime), reflection::Definition<Packed>::build(runtime),
         string_definition(runtime), array_definition<std::int32_t>(runtime, "borrow.Ints"),
         array_definition<String>(runtime, "borrow.Strings"),
         array_definition<Array<std::int32_t>>(runtime, "borrow.Batches"),
         NativeRegistration<Host>{runtime, "borrow.Host", TypeKind::object}
             .method<&Host::invalidate>("invalidate")
             .build(),
         NativeRegistration<RetiredAsset>{runtime, "borrow.RetiredAsset", TypeKind::object}.build()}));
    const auto particle_type = take(runtime.native_type<Particle>());
    require(!take(runtime.describe(take(runtime.native_type<Packed>())))->fields[0].offset,
            "Packed native fields acquired an invalid naturally-aligned direct offset.");
    const auto* layout = take(runtime.describe(particle_type));
    require(layout->size == sizeof(Particle) && layout->alignment == alignof(Particle) &&
                layout->fields[0].offset == offsetof(Particle, position) &&
                layout->fields[2].offset == offsetof(Particle, name),
            "Generated direct layout does not match the native record.");

    Program update(runtime, {});
    publish(update, R"(
        float update(Array<borrow.Particle>& values, const Array<borrow.Particle>& source,
            float delta, const String& label, uint& bytes) {
            uint index = 0;
            float total = 0.0;
            bytes = 0;
            while (index < values.size()) {
                values[index].position.x = source[index].position.x + delta;
                values[index].count = values[index].count + 1;
                total = total + values[index].position.x;
                bytes = bytes + values[index].name.size() + label.size();
                index = index + 1;
            }
            return total;
        }
    )");
    const auto entry =
        take(update.bind<float, Array<Particle>&, const Array<Particle>&, float, const String&, std::uint32_t&>());
    {
        Array<Particle> particles;
        particles.reserve(32);
        for (std::uint32_t i = 0; i != 32; ++i) {
            auto& particle = particles.emplace();
            particle.position = {static_cast<float>(i), 2, 3};
            particle.count = static_cast<std::int32_t>(i);
            particle.name.assign("asset:/particles/火花.asset");
        }
        const auto* allocation = particles.data();
        const auto* text_allocation = particles[0].name.c_str();
        const auto copies = Particle::copies;
        const auto moves = Particle::moves;
        const auto destroyed = Particle::destroyed;
        const String label{"tick"};
        std::uint32_t bytes = 99;
        const auto& read_only = particles; // A legal alias, deliberately not restricted by LLVM noalias.
        const std::array arguments{Argument::borrow(particles), Argument::borrow(read_only), Argument{Scalar{1.0f}},
                                   Argument::borrow(label), Argument::borrow(bytes)};
        auto retired = take(runtime.create(take(runtime.native_type<RetiredAsset>())));
        retired.reset();
        take(runtime.begin_collection());
        const auto phase = runtime.collection_phase();
        require(std::get<float>(*take(entry.invoke({}, arguments))) == 528.0f,
                "Borrowed source returned an incorrect batch total.");
        require(runtime.collection_phase() == phase && RetiredAsset::destroyed == 0,
                "GC ran inside a synchronous value borrow.");
        require(particles.data() == allocation && particles[0].name.c_str() == text_allocation &&
                    Particle::copies == copies && Particle::moves == moves && Particle::destroyed == destroyed,
                "Source invocation copied, relocated or destroyed native values.");
        for (std::uint32_t i = 0; i != particles.size(); ++i)
            require(particles[i].position.x == static_cast<float>(i + 1) && particles[i].position.y == 2 &&
                        particles[i].count == static_cast<std::int32_t>(i + 1),
                    "Native caller did not observe direct field stores.");
        require(bytes == 32 * (particles[0].name.size() + label.size()),
                "Borrowed String lengths changed their UTF-8 byte meaning.");
        while (runtime.collection_phase() != CollectionPhase::idle)
            (void)take(runtime.collect_step({1}));
        require(RetiredAsset::destroyed == 1, "Host could not collect after the borrowed call returned.");

        Program inspect(runtime, {});
        publish(inspect, "float inspect(const borrow.Particle& value) { return value.position.x; }");
        const auto inspect_entry = take(inspect.bind<float, const Particle&>());
        const std::array record{Argument::borrow(std::as_const(particles[3]))};
        require(std::get<float>(*take(inspect_entry.invoke({}, record))) == 4,
                "Direct record parameter used another representation.");
        const std::array misaligned{
            Argument::borrow(particle_type, reinterpret_cast<const std::byte*>(particles.data()) + 4)};
        require(!inspect_entry.invoke({}, misaligned), "Native record borrow ignored its 64-byte alignment.");
        Array<std::int32_t> wrong{1, 2};
        auto wrong_arguments = arguments;
        wrong_arguments[0] = Argument::borrow(wrong);
        require(!entry.invoke({}, wrong_arguments), "Array borrow accepted a different registered element identity.");
        wrong_arguments = arguments;
        wrong_arguments[0] = Argument::borrow(read_only);
        require(!entry.invoke({}, wrong_arguments), "Mutable Array parameter accepted const storage.");

        Program partial(runtime, {});
        publish(
            partial,
            "void partial(Array<borrow.Particle>& values) { values[0].count = 77; values[values.size()].count = 0; }");
        const std::array writable{Argument::borrow(particles)};
        auto error = partial.entry().invoke({}, writable);
        require(!error && error.error().message.find("out of range") != std::string::npos && particles[0].count == 77,
                "Bounds failure lost its diagnostic or hid already committed native writes.");

        const auto old = take(inspect.pin());
        publish(inspect, "float inspect(const borrow.Particle& value) { return value.position.x + 1.0; }");
        require(std::get<float>(*take(inspect_entry.invoke({}, record))) == 5 &&
                    std::get<float>(*take(old.invoke({}, record))) == 4,
                "Borrowed value signatures lost their code-version ownership.");
        auto incompatible = Program::prepare(take(inspect.request(
            "incompatible-borrow.source", "float inspect(borrow.Particle& value) { return value.position.x; }")));
        require(!incompatible, "Publication changed a borrowed parameter's const contract.");

        particles.clear();
        require(std::get<float>(*take(entry.invoke({}, arguments))) == 0 && bytes == 0,
                "Empty native Array borrow dereferenced a nonexistent element.");
    }
    require(Particle::live == 0, "Borrowed invocation extended native record lifetime past its owner.");

    Program text(runtime, {});
    publish(text, "uint first(const String& value) { return value[0]; }");
    for (const String& value : {String{"a"}, String{"asset:/longer_than_inline.asset"}, String{"火"}}) {
        const std::array arguments{Argument::borrow(value)};
        require(std::get<std::uint32_t>(*take(text.entry().invoke({}, arguments))) ==
                    static_cast<unsigned char>(value.c_str()[0]),
                "String borrow did not read the native UTF-8 byte.");
    }
    publish(text, "uint first(const String& value) { return value[-1]; }");
    const String empty;
    const std::array empty_text{Argument::borrow(empty)};
    require(!text.entry().invoke({}, empty_text), "Negative String index escaped bounds checking.");

    Program nested(runtime, {});
    publish(nested, "int nested(Array<Array<int>>& values) { values[0][1] = 9; return values[0][1]; }");
    Array<Array<std::int32_t>> batches;
    batches.emplace(Array<std::int32_t>{1, 2});
    const auto* inner = batches[0].data();
    const std::array nested_arguments{Argument::borrow(batches)};
    require(std::get<std::int32_t>(*take(nested.entry().invoke({}, nested_arguments))) == 9 &&
                batches[0].data() == inner && batches[0][1] == 9,
            "Nested Array borrow copied or misaddressed element storage.");

    Program restricted(runtime, take(runtime.native_type<Host>()));
    constexpr std::array rejected{"void bad(Array<borrow.Particle> values) {}",
                                  "async void bad(Array<borrow.Particle>& values) { yield; }",
                                  "void bad(Array<borrow.Particle> const& values) {}",
                                  "void bad(const Array<borrow.Particle>& values) { values[0].position.x = 1.0; }",
                                  "void bad(Array<borrow.Particle>& values) { values[0] = values[1]; }",
                                  "void bad(Array<borrow.Particle>& values) { self.invalidate(); }",
                                  "void bad(Array<borrow.Particle>& values) { new borrow.Host(); }",
                                  "void bad(const String& value) { value[0] = 97; }",
                                  "void bad(String& value) {}",
                                  "int bad(const borrow.Packed& value) { return value.value; }",
                                  "int bad(Array<borrow.Particle>& values) { return values; }",
                                  "void bad(const borrow.Particle& value) { value.position.x = 1.0; }",
                                  "void bad(Array<borrow.Particle>& values) { borrow.Particle& escaped = values[0]; }"};
    for (const auto* source_text : rejected) {
        auto candidate = Program::prepare(take(restricted.request("invalid-borrow.source", source_text)));
        require(!candidate, "Compiler admitted an invalid borrow or lifecycle escape.");
    }
    require(Host::callbacks == 0, "Rejected borrowed code entered an invalidating native callback.");
    std::puts(
        "Native value borrows: PASS (shared layout, zero element copies, direct stores, const/alias/bounds, synchronous lifetime, versions)");
}
