#include "reflection/fixture.hpp"
#include "source/program.hpp"
#include <array>
#include <bit>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace {
using namespace atelier;
using namespace atelier::acceptance::reflection;
using source::Program;
template<class T> T take(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(*result);
}
void take(Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void publish(Program& program, std::string_view text) {
    auto request = take(program.request("shared-calls.source", text));
    auto candidate = take(Program::prepare(request));
    take(program.try_publish(candidate));
}
void rejected(Program& program, std::string_view text, std::string_view expected) {
    auto request = take(program.request("rejected-borrow.source", text));
    auto candidate = Program::prepare(request);
    require(!candidate && candidate.error().message.find(expected) != std::string::npos,
        "Invalid source did not retain its expected diagnostic.");
}
}

void verify_shared_calls(atelier::Runtime& runtime) {
    const auto type = take(runtime.native_type<CallProbe>());
    const auto mode = take(runtime.native_type<Mode>());
    const auto other = take(runtime.native_type<OtherMode>());
    const auto unsigned_mode = take(runtime.native_type<UnsignedMode>());
    const auto i32 = take(runtime.native_type<int>());
    require(i32 == take(runtime.find_type("core.i32")), "Native int has a second type identity.");
    const auto* enum_info = take(runtime.describe(mode));
    require(enum_info->kind == TypeKind::enumeration && enum_info->size == 4 &&
        enum_info->enumeration->underlying == i32 && enum_info->enumeration->values.size() == 3 &&
        enum_info->enumeration->values[0].second == 0xffffffffu &&
        enum_info->enumeration->values[1].second == enum_info->enumeration->values[2].second,
        "Enum metadata lost its representation, signed bits or aliases.");
    auto object = take(runtime.create(type));
    const auto method = [&](std::string_view name) { return take(runtime.find_method(type, name)); };
    const auto field = take(runtime.find_field(type, "value"));
    const auto enum_field = take(runtime.find_field(type, "mode"));
    int value = 1;
    const std::array aliases{Argument::borrow(value), Argument::borrow(value)};
    Scalar output = std::uint32_t{99};
    take(runtime.call(object.id(), method("aliases"), aliases, &output));
    require(value == 20 && std::get<int>(output) == 20, "Native aliases were copied or const borrowing lost updates.");
    const std::array field_path{field};
    const std::array field_args{Argument::field(object.id(), field_path), Argument::field(object.id(), field_path)};
    take(runtime.call(object.id(), method("aliases"), field_args, &output));
    require(std::get<int>(take(runtime.read(object.id(), field))) == 20, "Field reference did not write actual storage.");
    const std::array bad{Argument{Scalar{1}}, Argument{Scalar{2}}};
    output = std::uint32_t{99};
    auto failed = runtime.call(object.id(), method("aliases"), bad, &output);
    require(!failed && failed.error().code == ErrorCode::argument_mismatch && std::get<std::uint32_t>(output) == 99,
        "Rvalue bound to writable native reference or failed call overwrote output.");
    const int immutable = 2;
    const std::array read_only{Argument::borrow(value), Argument::borrow(immutable)};
    require(!runtime.call(object.id(), method("aliases"), read_only, &output), "Const storage bound to writable reference.");
    const std::array one{Argument::borrow(value)};
    failed = runtime.call(object.id(), method("fail"), one, &output);
    require(!failed && value == -7 && std::get<std::uint32_t>(output) == 99, "Failure changed reference side effects or result atomicity.");
    Mode native_mode = Mode::idle;
    const std::array enum_args{Argument::borrow(native_mode), Argument::borrow(native_mode)};
    take(runtime.call(object.id(), method("enum_alias"), enum_args, &output));
    require(native_mode == Mode::active && std::get<EnumValue>(output) == EnumValue{mode, 2}, "Native enum references lost identity or aliasing.");
    take(runtime.write(object.id(), enum_field, EnumValue{mode, 42}));
    require(std::get<EnumValue>(take(runtime.read(object.id(), enum_field))).bits == 42, "Fixed-width enum rejected an unnamed value.");
    require(!runtime.write(object.id(), enum_field, EnumValue{other, 2}), "Distinct enum types were interchangeable.");
    require(!runtime.write(object.id(), field, EnumValue{i32, 1}), "Enum payload impersonated a scalar type.");

    Program source(runtime, type);
    publish(source, R"(
int run(const int& read, int& write) {
    self.aliases(read, write);
    self.aliases(self.value, self.value);
    self.float_alias(self.sample.value, self.sample.value);
    self.uint_alias(self.bits, self.bits);
    reflection.Mode mode = reflection.Mode::idle;
    self.enum_alias(mode, mode);
    self.mode = mode;
    if (mode != reflection.Mode::alias) { return -100; }
    if (self.read_only(-4) >= 0) { return -200; }
    return read;
})");
    auto entry = take(source.bind<int, const int&, int&>());
    value = 1;
    auto result = take(entry.invoke(object.id(), aliases));
    require(result && std::get<int>(*result) == 20 && value == 20, "Host/JIT/native mixed alias chain failed.");
    const std::array float_path{take(runtime.find_field(type, "sample")), take(runtime.find_field(take(runtime.native_type<Sample>()), "value"))};
    require(std::get<float>(take(runtime.read(object.id(), float_path))) == 6.f &&
        std::get<std::uint32_t>(take(runtime.read(object.id(), take(runtime.find_field(type, "bits"))))) == 10 &&
        std::get<EnumValue>(take(runtime.read(object.id(), enum_field))) == EnumValue{mode, 2},
        "JIT field aliases or enum assignment did not reach native storage.");
    require(!source.bind<int, int, int&>(), "Host binding ignored parameter passing modes.");
    rejected(source, "int run(const int& read, int& write) { self.aliases(read, 1); return read; }", "mutable lvalue");
    rejected(source, "int run(const int& read, int& write) { read = 2; return read; }", "writable local");
    rejected(source, "int run(const int& read, int& write) { self.aliases(write, read); return read; }", "mutable lvalue");
    rejected(source, "async int run(int& value) { yield; return value; }", "cannot borrow");
    rejected(source, "void bad() { self.echo(reflection.OtherMode::active); }", "Type mismatch");
    auto changed = take(source.request("changed-mode.source", "int run(int read, int& write) { return read; }"));
    auto candidate = Program::prepare(changed);
    require(!candidate, "Hot replacement changed reference passing mode.");
    value = 1;
    require(std::get<int>(*take(entry.invoke(object.id(), aliases))) == 20, "Rejected replacement damaged the live entry.");

    Program evaluation(runtime, type);
    publish(evaluation, "int order() { self.value = 1; return self.observe_after(self.value, self.mutate()); }");
    require(std::get<int>(*take(evaluation.entry().invoke(object.id(), {}))) == 7, "Readonly field borrow became an eager snapshot.");
    publish(evaluation, "int order() { self.value = 1; return self.mixed(self.value, self.mutate(), self.value); }");
    require(std::get<int>(*take(evaluation.entry().invoke(object.id(), {}))) == 8, "Nested call overwrote an earlier by-value operand.");
    publish(evaluation, "int order() { return self.self_alias(self.value); }");
    require(std::get<int>(*take(evaluation.entry().invoke(object.id(), {}))) == -19, "Receiver and parameter field failed to alias.");

    Program enum_program(runtime, type);
    publish(enum_program, "reflection.Mode echo(reflection.Mode value) { return self.echo(value); }");
    auto enum_entry = take(enum_program.bind<Mode, Mode>());
    const std::array enum_values{Scalar{EnumValue{mode, 42}}};
    require(std::get<EnumValue>(*take(enum_entry.invoke(object.id(), enum_values))) == EnumValue{mode, 42},
        "Enum value/result did not cross JIT with its identity and unnamed bits.");
    const std::array wrong_enum{Scalar{EnumValue{other, 42}}};
    require(!enum_entry.invoke(object.id(), wrong_enum), "Host source entry erased enum identity.");
    Program enum_reference(runtime, {});
    publish(enum_reference, "reflection.Mode edit(reflection.Mode& value) { value = reflection.Mode::active; return value; }");
    native_mode = Mode::idle;
    const std::array mutable_enum{Argument::borrow(native_mode)};
    require(std::get<EnumValue>(*take(enum_reference.entry().invoke({}, mutable_enum))) == EnumValue{mode, 2} && native_mode == Mode::active,
        "JIT enum parameter failed to mutate native enum storage.");
    Program unsigned_program(runtime, type);
    publish(unsigned_program, "reflection.UnsignedMode high() { return self.unsigned_echo(reflection.UnsignedMode::high); }");
    require(std::get<EnumValue>(*take(unsigned_program.entry().invoke(object.id(), {}))) == EnumValue{unsigned_mode, 0xffffffffu},
        "Unsigned enum bits changed across JIT.");

    Program failure(runtime, type);
    publish(failure, "int fail(int& value) { return self.fail(value); }");
    require(!failure.entry().invoke(object.id(), one) && value == -7, "JIT failed call rolled back or lost borrowed writes.");
    Program wide(runtime, type);
    publish(wide, "int wide() { int v = 0; return self.wide(v,v,v,v,v,v,v,v,v); }");
    require(std::get<int>(*take(wide.entry().invoke(object.id(), {}))) == 16, "Expanded argument scratch broke aliases.");
    Program copied(runtime, {});
    publish(copied, "int copy(int value) { value = -2147483648; if (value < -1) { return value; } return 0; }");
    value = 4;
    require(std::get<int>(*take(copied.entry().invoke({}, one))) == std::numeric_limits<int>::min() && value == 4,
        "By-value argument mutated its source or signed comparison/minimum literal failed.");
    Program coroutine(runtime, type);
    publish(coroutine, "async int task(int initial) { int value = initial; yield; return self.aliases(value, value); }");
    const std::array initial{Scalar{1}};
    auto task = take(coroutine.entry().start(object.id(), initial));
    require(take(task.resume()) == CoroutineState::suspended, "Coroutine did not suspend.");
    const auto completed = take(task.resume());
    require(completed == CoroutineState::completed && std::get<int>(*take(task.result())) == 20,
        "Synchronous borrow of a resumed coroutine local failed.");
    Runtime described;
    TypeDefinition described_enum;
    described_enum.name = "described.Mode";
    described_enum.kind = TypeKind::enumeration;
    described_enum.enumeration = TypeDefinition::Enumeration{TypeReference::native<int>(), {{"active", 2}}};
    TypeDefinition described_object;
    described_object.name = "described.Object";
    described_object.fields = {{"value", {TypeReference::native<int>()}}, {"mode", {TypeReference{"described.Mode"}}}};
    SignatureDefinition signature;
    signature.parameters = {ParameterDefinition{ValueDefinition{TypeReference::native<int>()}, ParameterPassing::read_only},
        ParameterDefinition{ValueDefinition{TypeReference::native<int>()}, ParameterPassing::reference}};
    signature.result = ValueDefinition{TypeReference::native<int>()};
    described_object.methods.push_back({"aliases", signature, [](CallContext& call) -> Result<void> {
        auto& value = call.arguments[1].get<int&>();
        value = 4;
        value += call.arguments[0].get<const int&>();
        call.result = value;
        return {};
    }});
    const auto ids = take(described.register_types({std::move(described_object), std::move(described_enum)}));
    auto described_root = take(described.create(ids[0]));
    Program described_source(described, ids[0]);
    publish(described_source, "described.Mode run() { self.value = -1; self.aliases(self.value, self.value); self.mode = described.Mode::active; return self.mode; }");
    require(std::get<EnumValue>(*take(described_source.entry().invoke(described_root.id(), {}))) == EnumValue{ids[1], 2} &&
        std::get<int>(take(described.read(described_root.id(), take(described.find_field(ids[0], "value"))))) == 8,
        "Described storage did not share enum identity and reference semantics.");
    std::puts("Shared call contracts: PASS (i32, AENUM, value/const-ref/ref, aliases, fields, JIT, async, replacement)");
}
