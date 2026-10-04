#include "reflection/fixture.hpp"
#include "runtime/foundation.hpp"
#include <array>
#include <bit>
#include <cstdio>
#include <stdexcept>
#include <utility>

void verify_shared_calls(atelier::Runtime&);

namespace {
using namespace atelier;
using namespace atelier::acceptance::reflection;
template<class T> T take(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(*result);
}
void take(Result<void> result) {
    if (!result) throw std::runtime_error(result.error().message);
}
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void invoke(Runtime& runtime, ObjectId object, TypeId type, const char* name,
    std::span<const Scalar> args, Scalar expected) {
    Scalar output;
    const auto called = runtime.call(object, take(runtime.find_method(type, name)), args, &output);
    if (!called) throw std::runtime_error(called.error().message);
    require(output == expected, "Generated member invoker changed its signature or receiver.");
}
void verify_math_calls(Runtime& runtime) {
    const auto type = take(runtime.native_type<CallProbe>());
    const auto f64 = take(runtime.native_type<double>());
    const auto vector3d = take(runtime.native_type<math::Vector3d>());
    auto object = take(runtime.create(type));
    const auto method = [&](std::string_view name) { return take(runtime.find_method(type, name)); };
    const double tiny = 0x1p-40;
    double target = 4.0;
    const double read = tiny * 2;
    const std::array precise_arguments{Argument{Scalar{1.0 + tiny}}, Argument::borrow(read), Argument::borrow(target)};
    Scalar scalar_output;
    take(runtime.call(object.id(), method("precise"), precise_arguments, &scalar_output));
    require(target == 5.0 + tiny * 3 && std::get<double>(scalar_output) == target,
        "Generated double value/const-reference/reference method narrowed a value or lost mutation.");
    const auto precise_field = take(runtime.find_field(type, "precise_value"));
    for (const std::uint64_t bits : {0x3ff0000000000001ull, 0x8000000000000000ull, 0x7ff8123456789abcull}) {
        const double value = std::bit_cast<double>(bits);
        take(runtime.write(object.id(), precise_field, Scalar{value}));
        const std::array arguments{take(runtime.read(object.id(), precise_field))};
        take(runtime.call(object.id(), method("precise_echo"), arguments, &scalar_output));
        require(std::bit_cast<std::uint64_t>(std::get<double>(scalar_output)) == bits,
            "Generated double field/method lost precision, signed zero or NaN payload.");
    }

    const auto direction_field = take(runtime.find_field(type, "direction"));
    const math::Vector3d copy{tiny, 2, 3}, initial{1.0 + tiny, 4, 5};
    take(runtime.write(object.id(), direction_field, ValueInput{vector3d, &initial}));
    const std::array direction_path{direction_field};
    const std::array vector_arguments{Argument::borrow(copy), Argument::field(object.id(), direction_path),
        Argument::field(object.id(), direction_path)};
    math::Vector3d output;
    take(runtime.call(object.id(), method("translate"), vector_arguments, ValueOutput{vector3d, &output}));
    math::Vector3d stored;
    take(runtime.read(object.id(), direction_field, ValueOutput{vector3d, &stored}));
    require(output == initial + copy + initial && stored == output,
        "Generated mathematical value/result or whole-field aliases lost native storage semantics.");
    require(!runtime.call(object.id(), method("translate"), vector_arguments, ValueOutput{f64, &output}),
        "Generated mathematical method accepted a different result type.");

    const auto plain_type = take(runtime.native_type<PlainRecord>());
    static_assert(sizeof(PlainRecord) > 128 && alignof(PlainRecord) == 64);
    PlainRecord record{true, Mode::active, math::Matrix4d::identity(), 4};
    PlainRecord record_output{};
    take(runtime.call(object.id(), method("record_alias"),
        std::array{Argument::borrow(std::as_const(record)), Argument::borrow(record)},
        ValueOutput{plain_type, &record_output}));
    require(take(runtime.describe(plain_type))->plain_value && record.count == 14 &&
        record_output.count == 4 && !record_output.enabled && record_output.mode == Mode::active,
        "Generated record thunk lost aligned value snapshots, bool/enum fields or typed large output.");
    const auto samples_field = take(runtime.find_field(type, "samples"));
    const std::array samples_path{samples_field};
    const std::array array_arguments{Argument::field(object.id(), samples_path), Argument::borrow(tiny)};
    take(runtime.call(object.id(), method("append_sample"), array_arguments, &scalar_output));
    require(std::get<double>(scalar_output) == tiny &&
        take(runtime.with_native<CallProbe>(object.id(), [&](const CallProbe& probe) -> Result<bool> {
            return probe.samples.size() == 1 && probe.samples[0] == tiny;
        })), "Generated Array<double> field/reference method did not preserve precision.");
    std::puts("[pass] Generated double/math fields, value/reference parameters, typed results and Array<double>");
}
void verify_bool_calls(Runtime& runtime) {
    const auto type = take(runtime.native_type<CallProbe>());
    const auto boolean = take(runtime.native_type<bool>());
    const auto info = take(runtime.describe(boolean));
    require(info->size == sizeof(bool) && info->alignment == alignof(bool),
        "Runtime bool storage did not match native bool layout.");
    const auto object = take(runtime.create(type));
    const auto enabled = take(runtime.find_field(type, "enabled"));
    take(runtime.write(object.id(), enabled, Scalar{true}));
    require(std::get<bool>(take(runtime.read(object.id(), enabled))) &&
        take(runtime.with_native<CallProbe>(object.id(), [](const CallProbe& probe) -> Result<bool> {
            return probe.enabled && probe.before && probe.after;
        })), "Generated bool field access overwrote an adjacent native field.");
    struct Guarded { bool value{}; bool before{true}; bool after{true}; std::uint32_t guard{0x12345678}; } guarded;
    const auto alias = take(runtime.find_method(type, "bool_alias"));
    const std::array arguments{Argument{Scalar{true}}, Argument::borrow(std::as_const(guarded.value)),
        Argument::borrow(guarded.value)};
    Scalar output{false};
    take(runtime.call(object.id(), alias, arguments, &output));
    require(std::get<bool>(output) && guarded.value && guarded.before && guarded.after && guarded.guard == 0x12345678,
        "Generated bool value/result or readonly/writable aliases lost native byte storage.");
    auto readonly = arguments;
    readonly[2] = Argument::borrow(std::as_const(guarded.value));
    output = false;
    require(!runtime.call(object.id(), alias, readonly, &output) && !std::get<bool>(output),
        "Generated writable bool reference accepted readonly storage or overwrote failed output.");
    const auto flags = take(runtime.find_field(type, "flags"));
    const std::array path{flags};
    const std::array append_arguments{Argument::field(object.id(), path), Argument{Scalar{true}}};
    take(runtime.call(object.id(), take(runtime.find_method(type, "append_flag")), append_arguments, &output));
    require(std::get<bool>(output) && take(runtime.with_native<CallProbe>(object.id(), [](const CallProbe& probe) -> Result<bool> {
        return probe.flags.size() == 1 && probe.flags.data()[0] && &probe.flags[0] == probe.flags.data();
    })), "Generated Array<bool> field/reference method lost contiguous native storage.");
    guarded.value = false;
    output = false;
    const auto failed = runtime.call(object.id(), take(runtime.find_method(type, "fail_bool")),
        std::array{Argument::borrow(guarded.value)}, &output);
    require(!failed && failed.error().message.find("generated bool failure") != std::string::npos &&
        guarded.value && !std::get<bool>(output) && guarded.before && guarded.after,
        "Generated bool failure lost committed reference writes, first cause or staged output.");
    take(runtime.call(object.id(), alias, arguments, &output));
    require(std::get<bool>(output), "Generated bool failure left execution admission closed.");
    std::puts("[pass] Generated bool fields, scalar/results, native reference aliases and Array<bool>");
}
}
int main() {
    try {
        Runtime runtime;
        const auto types = take(runtime.register_types({
            atelier::reflection::Definition<Sample>::build(runtime),
            atelier::reflection::Definition<Counter>::build(runtime),
            atelier::reflection::Definition<CallProbe>::build(runtime),
            atelier::reflection::Definition<Mode>::build(runtime),
            atelier::reflection::Definition<OtherMode>::build(runtime),
            atelier::reflection::Definition<UnsignedMode>::build(runtime),
            atelier::reflection::Definition<PlainRecord>::build(runtime),
            array_definition<double>(runtime, "reflection.DoubleArray"),
            array_definition<bool>(runtime, "reflection.BoolArray")}));
        auto object = take(runtime.create(types[1]));
        require(Counter::live == 1, "Generated native construction did not run exactly once.");
        invoke(runtime, object.id(), types[1], "read", {}, 3.f);
        const std::array count{Scalar{std::uint32_t{7}}}, scale{Scalar{2.f}};
        invoke(runtime, object.id(), types[1], "count", count, std::uint32_t{7});
        invoke(runtime, object.id(), types[1], "scale", scale, 6.f);
        require(!runtime.find_field(types[1], "sample_"), "Generated hidden record became public.");
        const auto info = take(runtime.describe(types[1]));
        require(info->methods.front().signature.receiver->read_only, "Generated const method lost receiver metadata.");
#if defined(_DEBUG)
        require(runtime.find_field(types[1], "debug_only").has_value() && !runtime.find_field(types[1], "release_only"), "Generator lost dev preprocessing context.");
#else
        require(runtime.find_field(types[1], "release_only").has_value() && !runtime.find_field(types[1], "debug_only"), "Generator lost release preprocessing context.");
#endif
        verify_math_calls(runtime);
        verify_bool_calls(runtime);
        verify_shared_calls(runtime);
        const auto destroyed = runtime.destroy(object.id());
        if (!destroyed) throw std::runtime_error(destroyed.error().message);
        require(Counter::live == 0 && !runtime.type_of(object.id()), "Generated native destruction or identity invalidation failed.");
        std::puts("Reflection consumer: PASS (private records, scalar overloads, const/noexcept, target macros, lifecycle)");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Reflection consumer: FAIL: %s\n", error.what());
        return 1;
    }
}
