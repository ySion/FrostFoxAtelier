#include "source/native_values.hpp"
#include "source/program.hpp"

#include <array>
#include <cstdio>
#include <stdexcept>

namespace {
using namespace atelier;
using namespace atelier::acceptance::borrow;
using source::Program;
struct MoveEffect {
    static inline int moves{};
    int value;
    MoveEffect() = default;
    MoveEffect(const MoveEffect&) = default;
    MoveEffect& operator=(const MoveEffect&) = default;
    MoveEffect(MoveEffect&& other) noexcept : value(other.value) { ++moves; }
};
struct ReferencingValue {
    ReferenceSlot target;
};
struct ForwardMethodHost {
    void inspect(const ReferencingValue&) {}
};
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <class T>
T take(Result<T> value) {
    if (!value)
        throw std::runtime_error(value.error().message);
    return std::move(*value);
}
void take(Result<void> value) {
    if (!value)
        throw std::runtime_error(value.error().message);
}
void publish(Program& program, std::string_view source) {
    auto candidate = take(Program::prepare(take(program.request("owned-values.source", source))));
    take(program.try_publish(candidate));
}
}

void verify_source_owned_values() {
    {
        Runtime untrusted;
        take(untrusted.register_types(
            {reflection::Definition<Particle>::build(untrusted), reflection::Definition<Position>::build(untrusted),
             string_definition(untrusted), array_definition<Particle>(untrusted, "owned.Particles"),
             NativeRegistration<MoveEffect>{untrusted, "owned.MoveEffect", TypeKind::record}.build(),
             array_definition<MoveEffect>(untrusted, "owned.MoveEffects")}));
        Program rejected(untrusted, {});
        require(!Program::prepare(
                    take(rejected.request("untrusted-value.source", "void bad() { Array<borrow.Particle> values; }"))),
                "Nontrivial value operations without a preservation promise were admitted.");
        require(!Program::prepare(take(rejected.request(
                    "untrusted-move.source",
                    "void bad() { Array<owned.MoveEffect> values; values.resize(1); values.reserve(8); }"))) &&
                    MoveEffect::moves == 0,
                "Trivial copying incorrectly vouched for a nontrivial Array relocation constructor.");
    }
    Runtime runtime;
    // The method appears before the referenced record's final GC layout.
    auto invalid_batch = runtime.register_types(
        {NativeRegistration<ForwardMethodHost>{runtime, "owned.ForwardHost", TypeKind::object}
             .method<&ForwardMethodHost::inspect>("inspect", true)
             .build(),
         NativeRegistration<ReferencingValue>{runtime, "owned.ReferencingValue", TypeKind::record}
             .field<&ReferencingValue::target>("target", {TypeReference{"core.Object"}, ValueForm::strong_reference})
             .build()});
    require(!invalid_batch && !runtime.native_type<ForwardMethodHost>() && !runtime.native_type<ReferencingValue>(),
            "A forward method dependency admitted GC-bearing storage or partly published the batch.");
    auto particle = reflection::Definition<Particle>::build(runtime);
    // Its instrumented operations change counters and may throw, but do not
    // retain addresses, run game callbacks or invalidate unrelated storage.
    particle.native.value_operations_preserve_borrows = true;
    take(runtime.register_types(
        {std::move(particle), reflection::Definition<Position>::build(runtime),
         reflection::Definition<Algorithms>::build(runtime), string_definition(runtime),
         array_definition<Particle>(runtime, "owned.Particles"), array_definition<std::int32_t>(runtime, "owned.Ints"),
         array_definition<float>(runtime, "owned.Floats"), array_definition<String>(runtime, "owned.Strings"),
         array_definition<Array<float>>(runtime, "owned.Batches")}));
    const auto host_type = take(runtime.native_type<Algorithms>());
    auto host = take(runtime.create(host_type));
    Array<Particle> input;
    input.resize(3);
    for (auto& item : input) {
        item.count = 1;
        item.name = String{"original/name/火花"};
    }
    const auto baseline = Particle::live;
    const auto* data = input.data();
    const std::array arguments{Argument::borrow(std::as_const(input)), Argument{Scalar{std::uint32_t{0}}}};
    Program program(runtime, host_type);
    publish(program, R"(
        uint process(const Array<borrow.Particle>& input, uint early) {
            String text = "owned/";
            text.append("火花/long-text-for-heap");
            String copy = text;
            copy.clear();
            copy = text;
            Array<borrow.Particle> values = input;
            values.reserve(32);
            values.add(input[0]);
            values.append(input);
            uint count = self.process(values, input, copy);
            if (early == 1) { return count; }
            {
                Array<borrow.Particle> nested;
                nested.resize(2);
                if (self.live_count() != 12) { return 999; }
            }
            if (self.live_count() != 10) { return 998; }
            values = input;
            values.append(values);
            if (self.live_count() != 9) { return 997; }
            values.clear();
            return self.live_count();
        }
    )");
    const auto entry = take(program.bind<std::uint32_t, const Array<Particle>&, std::uint32_t>());
    require(std::get<std::uint32_t>(*take(entry.invoke(host.id(), arguments))) == 3,
            "Source scope, nested scope, copy assignment or sequence clear has an incorrect lifetime.");
    auto early = arguments;
    early[1] = Argument{Scalar{std::uint32_t{1}}};
    require(std::get<std::uint32_t>(*take(entry.invoke(host.id(), early))) == 7,
            "Early return lost its staged result or native call.");
    require(Particle::live == baseline && input.data() == data && input[0].count == 1 && Algorithms::calls == 2,
            "Owned local cleanup damaged the caller, leaked elements or skipped native code.");

    Program loops(runtime, host_type);
    publish(loops, R"(
        uint loops(uint count) {
            uint index = 0;
            while (index < count) {
                Array<borrow.Particle> values;
                values.resize(2);
                if (self.live_count() != 5) { return 999; }
                String text = "heap/string/iteration/value";
                if (self.text_size(text) != 27) { return 998; }
                index = index + 1;
            }
            return self.live_count();
        }
    )");
    const std::array loop_arguments{Scalar{std::uint32_t{128}}};
    require(std::get<std::uint32_t>(*take(loops.entry().invoke(host.id(), loop_arguments))) == 3 &&
                Particle::live == baseline,
            "Loop-owned values survived iteration scope exit.");

    Program strings(runtime, host_type);
    publish(strings, R"(
        uint strings() {
            Array<String> names;
            names.add("first/long-owned-name");
            names.add("second");
            names[1].append("/suffix");
            names.append(names);
            String escaped = "a\0\n\t\"\\";
            if (escaped.size() != 6) { return 999; }
            if (escaped[1] != 0) { return 998; }
            if ("literal".size() != 7) { return 995; }
            if (self.read_byte("x"[0]) != 120) { return 994; }
            if (self.read_byte(names[1][1]) != 101) { return 993; }
            String heap = names[0];
            heap.reserve(128);
            heap.append(heap);
            Array<float> row;
            row.add(2.0);
            row.resize(2);
            row[1] = 3.0;
            Array<Array<float>> batches;
            batches.add(row);
            batches[0].add(4.0);
            if (row.size() != 2) { return 997; }
            if (batches[0][2] != 4.0) { return 996; }
            return names.size() + self.text_size("literal/argument");
        }
    )");
    require(std::get<std::uint32_t>(*take(strings.entry().invoke(host.id(), std::array<Scalar, 0>{}))) == 20,
            "String literals, temporaries, deep nested values or native const borrows are incorrect.");

    Program fail(runtime, host_type);
    constexpr std::array failures{
        "void fail(const Array<borrow.Particle>& input) { Array<borrow.Particle> local = input; self.fail(local); }",
        "void fail(const Array<borrow.Particle>& input) { Array<borrow.Particle> local = input; local[local.size()].count = 0; }",
        "void fail(const Array<borrow.Particle>& input) { Array<borrow.Particle> local = input; local.resize(-1); }",
        "void fail(const Array<borrow.Particle>& input) { Array<borrow.Particle> local = input; String text = \"bad/\xFF\"; }"};
    const std::array borrowed{Argument::borrow(std::as_const(input))};
    for (const auto* source : failures) {
        publish(fail, source);
        require(!fail.entry().invoke(host.id(), borrowed) && Particle::live == baseline && input[0].count == 1,
                "Failure did not unwind owned values or modified an independent source.");
    }
    require(Algorithms::failures == 1, "Native failure scenario did not execute.");
    publish(
        fail,
        "void fail(const Array<borrow.Particle>& input) { String text = \"heap/copy/failure/value\"; Array<borrow.Particle> local = input; }");
    Particle::copy_budget = 1;
    auto failed_copy = fail.entry().invoke(host.id(), borrowed);
    Particle::copy_budget = -1;
    require(!failed_copy && Particle::live == baseline, "Partially constructed Array was leaked or destroyed twice.");

    const auto old = take(strings.pin());
    publish(strings, "uint strings() { String value = \"new\"; return value.size(); }");
    require(std::get<std::uint32_t>(*take(strings.entry().invoke(host.id(), std::array<Scalar, 0>{}))) == 3 &&
                std::get<std::uint32_t>(*take(old.invoke(host.id(), std::array<Scalar, 0>{}))) == 20,
            "Owned-value operations lost their immutable code-version environment.");

    Program rejected(runtime, host_type);
    constexpr std::array invalid{"void bad(Array<borrow.Particle>& input) { input.clear(); }",
                                 "void bad() { Array<float> values; self.unsafe(); }",
                                 "async void bad() { String value; yield; }",
                                 "void bad() { String value = \"text\"; value[0] = 97; }",
                                 "void bad() { String value; borrow.Algorithms other = self; }",
                                 "void bad() { String value = \"bad\\q\"; }"};
    for (const auto* source : invalid) {
        auto candidate = Program::prepare(take(rejected.request("invalid-owned.source", source)));
        require(!candidate, "Unsupported mutation, call effect or escaping lifetime was admitted.");
    }
    require(Algorithms::calls == 2 && Particle::live == baseline, "Rejected code changed native state.");
    Program references(runtime, host_type);
    publish(references, R"(
        int references(Array<borrow.Particle>& input) {
            self.touch(input[0]);
            self.bump(input[0].count, input[0].count);
            Array<int> numbers;
            numbers.add(8);
            self.bump(numbers[0], numbers[0]);
            return input[0].count + numbers[0];
        }
    )");
    const std::array writable{Argument::borrow(input)};
    require(std::get<std::int32_t>(*take(references.entry().invoke(host.id(), writable))) == 17 &&
                input[0].count == 7 && input.data() == data && Particle::live == baseline,
            "Native record/element/field references did not preserve caller storage and aliasing.");
    input.clear();
    require(Particle::live == 0, "Source-owned value acceptance left native resources alive.");
    std::puts(
        "Source owned values: PASS (RAII, native algorithms, literals, deep copies, loops, early/error cleanup, versions)");
}
