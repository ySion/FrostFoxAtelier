#include "math/math.hpp"
#include "runtime/foundation.hpp"
#include "runtime/native.hpp"
#include "source/program.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace {
using namespace atelier;
using source::Program;
enum class Mode : std::uint32_t { cold = 2, hot = 7 };
struct alignas(32) Inner { bool enabled; Mode mode; math::Vector3 position; };
struct alignas(64) Packet { Inner inner; math::Matrix4d first; math::Matrix4d second; std::uint32_t count; };
struct alignas(64) ScratchValue { std::uint32_t count; bool enabled; };
struct UnsafeValue { std::uint32_t count{5}; };
struct CopyValue {
    inline static std::uint32_t constructions{};
    inline static bool reject_construction{};
    std::uint32_t count;
    CopyValue() : count(0) {
        ++constructions;
        if (reject_construction) throw std::runtime_error("CopyValue default construction rejected.");
    }
};
static_assert(std::is_standard_layout_v<CopyValue> && std::is_trivially_copy_constructible_v<CopyValue> &&
    std::is_trivially_copy_assignable_v<CopyValue> && std::is_trivially_destructible_v<CopyValue>);
struct UnsafeHost {
    UnsafeValue payload;
    UnsafeValue result() const { return payload; }
    std::uint32_t accept(UnsafeValue value) const { return value.count; }
};
struct Host {
    Packet stored;
    Packet bump(Packet value) const { value.count += 1; value.inner.enabled = !value.inner.enabled; return value; }
    Packet constant(const Packet& value) const { return value; }
    Packet alias(Packet snapshot, Packet& value) const { value.count += 10; return snapshot; }
    Packet fail(Packet) const { throw std::runtime_error("Native plain record failure."); }
};
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T take(Result<T> value) {
    if (!value) throw std::runtime_error(value.error().message);
    return std::move(*value);
}
void take(Result<void> value) { if (!value) throw std::runtime_error(value.error().message); }
void publish(Program& program, std::string_view text, std::string_view entry = "run") {
    auto candidate = take(Program::prepare(take(program.request("plain-records.source", text, entry))));
    take(program.try_publish(candidate));
}
Packet seed() {
    Packet value{};
    value.inner = {true, Mode::hot, {1.0f, 2.0f, 3.0f}};
    value.first = math::Matrix4d::identity();
    value.second = math::Matrix4d::identity();
    value.count = 4;
    return value;
}
template<class Callable> Packet invoke(Runtime& runtime, const Callable& callable, ObjectId receiver, const Packet& input) {
    Packet output{};
    take(callable.invoke(receiver, std::array{Argument::borrow(input)},
        ValueOutput{take(runtime.native_type<Packet>()), &output}));
    return output;
}
}

void verify_source_plain_records() {
    {
        Runtime owner;
        auto definition = NativeRegistration<CopyValue>{owner, "plain.CopyValue", TypeKind::record}
            .field<&CopyValue::count>("count", {TypeReference::native<std::uint32_t>()}, offsetof(CopyValue, count)).build();
        definition.native.value_operations_preserve_borrows = true;
        const auto type = take(owner.register_type(std::move(definition)));
        Program identity(owner, {});
        publish(identity, R"(
            plain.CopyValue first(plain.CopyValue value) { return value; }
            plain.CopyValue second(plain.CopyValue value) { return first(value); }
            uint inspect(const plain.CopyValue& value) { return value.count; }
            plain.CopyValue run(plain.CopyValue value) {
                if (inspect(second(value)) != second(value).count) { value.count = 999; }
                return second(value);
            }
        )");
        Program local(owner, {});
        publish(local, "plain.CopyValue run(plain.CopyValue input) { plain.CopyValue value; return input; }");
        CopyValue input;
        CopyValue output;
        input.count = 37;
        output.count = 91;
        const auto constructions = CopyValue::constructions;
        CopyValue::reject_construction = true;
        struct ResetConstruction {
            ~ResetConstruction() { CopyValue::reject_construction = false; }
        } reset;
        for (unsigned invocation = 0; invocation < 2; ++invocation) {
            take(identity.entry().invoke({}, std::array{Argument::borrow(std::as_const(input))}, ValueOutput{type, &output}));
            require(output.count == 37 && input.count == 37 && CopyValue::constructions == constructions,
                "Plain value copies or result temporaries invoked a default constructor.");
        }
        output.count = 91;
        const auto failed = local.entry().invoke({}, std::array{Argument::borrow(std::as_const(input))}, ValueOutput{type, &output});
        require(!failed && failed.error().message == "CopyValue default construction rejected." &&
            output.count == 91 && CopyValue::constructions == constructions + 1,
            "An uninitialized local skipped default construction or failure changed caller output.");
        CopyValue::reject_construction = false;
        take(identity.entry().invoke({}, std::array{Argument::borrow(std::as_const(input))}, ValueOutput{type, &output}));
        require(output.count == 37 && CopyValue::constructions == constructions + 1,
            "A failed local construction left value invocation state behind.");
    }
    {
        PreparedArguments scratch;
        {
            Runtime owner;
            const auto type = take(owner.register_type(
                NativeRegistration<ScratchValue>{owner, "plain.ScratchValue", TypeKind::record}
                    .field<&ScratchValue::count>("count", {TypeReference::native<std::uint32_t>()}, offsetof(ScratchValue, count))
                    .field<&ScratchValue::enabled>("enabled", {TypeReference::native<bool>()}, offsetof(ScratchValue, enabled)).build()));
            ScratchValue value{37, true};
            FunctionSignature signature;
            signature.parameters.emplace_back(type);
            take(owner.prepare_arguments(signature, std::array{Argument::borrow(std::as_const(value))}, scratch));
            value.count = 99;
            require(scratch.values()[0].get<ScratchValue>().count == 37,
                "Prepared value snapshot retained its input address.");
            auto multiple = signature;
            multiple.parameters.resize(3, signature.parameters[0]);
            const auto argument = Argument::borrow(std::as_const(value));
            take(owner.prepare_arguments(multiple, std::array{argument, argument, argument}, scratch));
            const auto* expanded = scratch.values()[0].data;
            require(scratch.values().size() == 3 && scratch.values()[2].get<ScratchValue>().count == 99 &&
                reinterpret_cast<std::uintptr_t>(scratch.values()[2].data) % alignof(ScratchValue) == 0,
                "Expanded argument scratch lost record alignment or copied contents.");
            FunctionSignature scalar_signature;
            scalar_signature.parameters.emplace_back(take(owner.native_type<bool>()));
            take(owner.prepare_arguments(scalar_signature, std::array{Argument{true}}, scratch));
            require(scratch.values()[0].get<bool>(), "Returning to inline argument scratch lost the scalar value.");
            require(!owner.prepare_arguments(signature, std::array{Argument{1.0f}}, scratch) && scratch.values().empty(),
                "Failed argument preparation exposed previous validated views.");
            value.count = 37;
            take(owner.prepare_arguments(signature, std::array{argument}, scratch));
            require(scratch.values()[0].data == expanded, "Argument retry discarded reusable aligned storage.");
        }
        const auto copied = scratch.values()[0].get<ScratchValue>();
        require(copied.count == 37 && copied.enabled &&
            reinterpret_cast<std::uintptr_t>(scratch.values()[0].data) % alignof(ScratchValue) == 0,
            "Prepared plain value scratch lost its independent aligned storage after Runtime destruction.");
    }
    {
        Runtime owner;
        take(owner.register_types({
            NativeRegistration<UnsafeValue>{owner, "plain.UnsafeValue", TypeKind::record}
                .field<&UnsafeValue::count>("count", {TypeReference::native<std::uint32_t>()}, offsetof(UnsafeValue, count)).build(),
            NativeRegistration<UnsafeHost>{owner, "plain.UnsafeHost", TypeKind::object}
                .field<&UnsafeHost::payload>("payload")
                .method<&UnsafeHost::result>("result", true).method<&UnsafeHost::accept>("accept", true).build()}));
        const auto value_type = take(owner.native_type<UnsafeValue>());
        require(take(owner.describe(value_type))->plain_value &&
            !take(owner.describe(value_type))->value_operations_preserve_borrows,
            "Non-ownable plain native fixture lost its distinct lifecycle qualification.");
        const auto receiver = take(owner.native_type<UnsafeHost>());
        Program scalar(owner, receiver);
        publish(scalar, "uint run() { return 42; }");
        Program invalid(owner, receiver);
        for (const auto* text : {"uint run(const plain.UnsafeValue& value) { return self.accept(value); }",
            "uint run() { self.result(); return 42; }"}) {
            const auto candidate = Program::prepare(take(invalid.request("non-ownable-record.source", text, "run")));
            if (candidate || candidate.error().message.find("borrow-preserving") == std::string::npos)
                throw std::runtime_error("Calling a non-ownable plain native value did not reject its actual lifecycle qualification: " +
                    (candidate ? std::string{"compilation succeeded"} : candidate.error().message));
        }
        const auto object = take(owner.create(receiver));
        require(std::get<std::uint32_t>(*take(scalar.entry().invoke(object.id(), std::array<Scalar, 0>{}))) == 42,
            "An unused non-ownable native record method blocked unrelated scalar source.");
    }
    static_assert(sizeof(Packet) > 128 && alignof(Packet) == 64 && alignof(Inner) == 32);
    static_assert(std::is_trivially_copyable_v<Packet>);
    Runtime runtime;
    auto inner = NativeRegistration<Inner>{runtime, "plain.Inner", TypeKind::record}
        .field<&Inner::enabled>("enabled", {TypeReference::native<bool>()}, offsetof(Inner, enabled))
        .field<&Inner::mode>("mode", {TypeReference::native<Mode>()}, offsetof(Inner, mode))
        .field<&Inner::position>("position", {TypeReference::native<math::Vector3>()}, offsetof(Inner, position)).build();
    auto packet = NativeRegistration<Packet>{runtime, "plain.Packet", TypeKind::record}
        .field<&Packet::inner>("inner", {TypeReference::native<Inner>()}, offsetof(Packet, inner))
        .field<&Packet::first>("first", {TypeReference::native<math::Matrix4d>()}, offsetof(Packet, first))
        .field<&Packet::second>("second", {TypeReference::native<math::Matrix4d>()}, offsetof(Packet, second))
        .field<&Packet::count>("count", {TypeReference::native<std::uint32_t>()}, offsetof(Packet, count)).build();
    inner.native.value_operations_preserve_borrows = true;
    packet.native.value_operations_preserve_borrows = true;
    take(runtime.register_types({
        NativeRegistration<Mode>{runtime, "plain.Mode", TypeKind::enumeration}
            .enumerator("Cold", Mode::cold).enumerator("Hot", Mode::hot).build(),
        std::move(inner), std::move(packet), string_definition(runtime),
        array_definition<std::uint32_t>(runtime, "plain.Uints"),
        NativeRegistration<Host>{runtime, "plain.Host", TypeKind::object}
            .field<&Host::stored>("stored", {TypeReference::native<Packet>()}, offsetof(Host, stored))
            .method<&Host::bump>("bump", true).method<&Host::constant>("constant", true)
            .method<&Host::alias>("alias", true).method<&Host::fail>("fail", true).build()}));
    const auto packet_type = take(runtime.native_type<Packet>());
    const auto host_type = take(runtime.native_type<Host>());
    require(take(runtime.describe(packet_type))->plain_value &&
        take(runtime.describe(take(runtime.native_type<Inner>())))->plain_value,
        "Recursive native plain record classification failed.");

    Program program(runtime, host_type);
    publish(program, R"(
        plain.Packet adjust(plain.Packet value) {
            value.count = value.count + 1;
            value.inner.position.x = value.inner.position.x + 2.0;
            return value;
        }
        plain.Packet combine(plain.Packet left, plain.Packet right) {
            left.count = left.count + right.count;
            return left;
        }
        plain.Packet run(plain.Packet value) {
            plain.Packet result = combine(adjust(value), adjust(adjust(value)));
            return self.constant(self.bump(result));
        }
    )");
    Program rejected(runtime, {});
    for (const auto* source : {
        "struct Owned { String text; }; Owned run(Owned value) { return value; }",
        "struct Owned { Array<uint> values; }; Owned run(Owned value) { return value; }",
        "struct Owned { plain.Host object; }; Owned run(Owned value) { return value; }",
        "async plain.Packet run(plain.Packet value) { yield; return value; }"})
    {
        const auto invalid = Program::prepare(take(rejected.request("rejected-record.source", source, "run")));
        require(!invalid && invalid.error().message.find("sealed") == std::string::npos &&
            invalid.error().message.find("closed") == std::string::npos,
            "Owned, GC or asynchronous record rejection did not reach the value boundary.");
    }
    Program source_records(runtime, {});
    publish(source_records, R"(
        struct plain.Leaf { bool enabled; plain.Mode mode; vector3 position; };
        struct plain.Nest { plain.Leaf leaf; uint count; };
        plain.Nest make(uint count) {
            plain.Nest value;
            value.leaf.enabled = true;
            value.leaf.mode = plain.Mode::Hot;
            value.leaf.position = vector3(1.0, 2.0, 3.0);
            value.count = count;
            return value;
        }
        uint inspect(const plain.Nest& value) {
            if (!value.leaf.enabled || value.leaf.mode != plain.Mode::Hot || value.leaf.position.z != 3.0) { return 999; }
            return value.count;
        }
        uint run() { return inspect(make(17)); }
    )");
    require(take(runtime.describe(take(runtime.find_type("plain.Nest"))))->plain_value &&
        std::get<std::uint32_t>(*take(source_records.entry().invoke({}, std::array<Scalar, 0>{}))) == 17,
        "Source recursive records or const-reference result full-expression lifetime failed.");
    Program temporary_references(runtime, {});
    for (const auto* text : {
        "plain.Nest make() { plain.Nest value; return value; } void write(plain.Nest& value) {} void run() { write(make()); }",
        "plain.Nest make() { plain.Nest value; return value; } void write(uint& value) {} void run() { write(make().count); }"}) {
        const auto invalid = Program::prepare(take(temporary_references.request("temporary-reference.source", text, "run")));
        if (invalid || invalid.error().message.find("mutable lvalue") == std::string::npos)
            throw std::runtime_error("Writable reference rejection: " +
                (invalid ? std::string{"compilation succeeded"} : invalid.error().message));
    }
    const auto host = take(runtime.create(host_type));
    Packet input = seed();
    const auto stored_field = take(runtime.find_field(host_type, "stored"));
    take(runtime.write(host.id(), stored_field, ValueInput{packet_type, &input}));
    Packet field_output{};
    const std::array stored_path{stored_field};
    take(runtime.call(host.id(), take(runtime.find_method(host_type, "alias")),
        std::array{Argument::field(host.id(), stored_path), Argument::field(host.id(), stored_path)},
        ValueOutput{packet_type, &field_output}));
    Packet stored{};
    take(runtime.read(host.id(), stored_field, ValueOutput{packet_type, &stored}));
    require(field_output.count == 4 && stored.count == 14 && stored.inner.enabled &&
        stored.inner.mode == Mode::hot && stored.inner.position.z == 3.0f,
        "Typed record field access or by-value/borrowed field aliases changed layout or snapshots.");
    const auto result = invoke(runtime, program.entry(), host.id(), input);
    require(result.count == 12 && !result.inner.enabled && result.inner.mode == Mode::hot &&
        result.inner.position.x == 3.0f && input.count == 4 && input.inner.position.x == 1.0f && input.inner.enabled,
        "Record nested call temporaries, native value return, bool/enum/math fields or input snapshot failed.");
    const auto old = take(program.pin());
    publish(program, "plain.Packet run(plain.Packet value) { value.count = value.count + 20; return value; }");
    require(invoke(runtime, program.entry(), host.id(), input).count == 24 &&
        invoke(runtime, old, host.id(), input).count == 12,
        "Plain record publication changed the pinned version.");
    program.close();
    require(invoke(runtime, old, host.id(), input).count == 12,
        "Plain record lease lost code or value layout after close.");

    Program alias(runtime, host_type);
    publish(alias, R"(
        plain.Packet snapshot(plain.Packet copy, plain.Packet& target) {
            target.count = target.count + 10;
            return copy;
        }
        plain.Packet run(plain.Packet& value) {
            plain.Packet result = snapshot(value, value);
            return self.alias(result, value);
        }
    )");
    Packet output{};
    take(alias.entry().invoke(host.id(), std::array{Argument::borrow(input)}, ValueOutput{packet_type, &output}));
    require(output.count == 4 && input.count == 24,
        "Left-to-right record snapshot changed when a later writable argument aliases it.");
    input = seed();
    take(runtime.call(host.id(), take(runtime.find_method(host_type, "alias")),
        std::array{Argument::borrow(std::as_const(input)), Argument::borrow(input)}, ValueOutput{packet_type, &output}));
    require(output.count == 4 && input.count == 14,
        "Public Runtime native record call did not snapshot its by-value argument.");

    Program failure(runtime, host_type);
    publish(failure, "plain.Packet run(plain.Packet value) { return self.fail(value); }");
    Packet preserved;
    std::memset(&preserved, 0x5a, sizeof(preserved));
    std::array<std::byte, sizeof(Packet)> before{};
    std::memcpy(before.data(), &preserved, sizeof(preserved));
    const auto failed = failure.entry().invoke(host.id(), std::array{Argument::borrow(std::as_const(input))},
        ValueOutput{packet_type, &preserved});
    require(!failed && failed.error().message == "Native plain record failure." &&
        std::memcmp(before.data(), &preserved, sizeof(preserved)) == 0,
        "Failed record call overwrote output larger than the inline result buffer.");

    std::puts("Source plain records: PASS (recursive values, snapshots, aligned native calls, temporaries, large outputs, leases)");
}
