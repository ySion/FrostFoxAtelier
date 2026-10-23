#include "reflection/fixture.hpp"
#include "runtime/foundation.hpp"
#include "runtime/native.hpp"
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
const FieldInfo& field_info(Runtime& runtime, TypeId type, std::string_view name) {
    const auto id = take(runtime.find_field(type, name));
    for (const auto& field : take(runtime.describe(type))->fields)
        if (field.id == id) return field;
    throw std::runtime_error("Published field is missing from its type description.");
}
const MethodInfo& method_info(Runtime& runtime, TypeId type, std::string_view name) {
    const auto id = take(runtime.find_method(type, name));
    for (const auto& method : take(runtime.describe(type))->methods)
        if (method.id == id) return method;
    throw std::runtime_error("Published method is missing from its type description.");
}
void verify_ranges(Runtime& runtime) {
    const auto type = take(runtime.native_type<RangeProbe>());
    const auto& emissive = field_info(runtime, type, "emissive").range;
    const auto& tint = field_info(runtime, type, "tint").range;
    const auto& offset = field_info(runtime, type, "offset").range;
    const auto& samples = field_info(runtime, type, "samples").range;
    require(emissive == FieldRange{Scalar{0.0f}, Scalar{10.0f}, Scalar{0.0f}, Scalar{65504.0f}} &&
        tint == FieldRange{Scalar{0.1}, Scalar{1.0}, std::nullopt, Scalar{1.0}} &&
        offset == FieldRange{std::nullopt, std::nullopt, Scalar{std::int32_t{-5}}, Scalar{std::int32_t{5}}} &&
        samples == FieldRange{Scalar{std::uint32_t{1}}, Scalar{std::uint32_t{64}}, std::nullopt, std::nullopt},
        "Generated ranges lost Lock/Hard semantics or exact leaf types.");
    auto probe = take(runtime.create(type));
    const std::array emissive_path{take(runtime.find_field(type, "emissive"))};
    take(runtime.write(probe.id(), emissive_path, Scalar{100.0f}));
    require(take(runtime.read(probe.id(), emissive_path)) == Scalar{100.0f}, "An unlocked UI maximum constrained a generated field.");
    take(runtime.write(probe.id(), emissive_path, Scalar{1e6f}));
    require(take(runtime.read(probe.id(), emissive_path)) == Scalar{65504.0f}, "A generated hard maximum was not enforced.");

    // An Array<double> range bounds each element on insertion and on element writes.
    const auto weights = take(runtime.find_field(type, "weights"));
    require(field_info(runtime, type, "weights").range == FieldRange{std::nullopt, Scalar{0.5}, Scalar{0.0}, Scalar{1.0}},
        "A generated Array range lost its element bounds.");
    const std::array weights_path{weights};
    const double heavy = 4.0;
    take(runtime.insert_element(probe.id(), weights_path, 0, {take(runtime.native_type<double>()), &heavy}));
    const std::array first{weights, take(runtime.element(take(runtime.native_type<Array<double>>()), 0))};
    require(take(runtime.read(probe.id(), first)) == Scalar{1.0}, "An inserted Array element escaped its generated hard bound.");
    take(runtime.write(probe.id(), first, Scalar{-3.0}));
    require(take(runtime.read(probe.id(), first)) == Scalar{0.0}, "An Array element write escaped its generated hard bound.");
}
struct External {};
void verify_external_reference() {
    Runtime runtime;
    const auto missing = runtime.register_types({atelier::reflection::Definition<ParentArgs>::build(runtime)});
    require(!missing && missing.error().code == ErrorCode::invalid_definition &&
        missing.error().message.find("'reflection.External'") != std::string::npos &&
        missing.error().message.find("reflection.ParentArgs.parent") != std::string::npos,
        "A missing reference target did not name its field and target.");
    const auto types = take(runtime.register_types({
        NativeRegistration<External>{runtime, "reflection.External", TypeKind::object}.build(),
        atelier::reflection::Definition<ParentArgs>::build(runtime)}));
    const auto& parent = field_info(runtime, types[1], "parent");
    require(parent.value.type == types[0] && parent.value.form == ValueForm::weak_reference,
        "A reference target outside the generated group did not resolve by name.");
}
void verify_field_presentation(Runtime& runtime) {
    const auto sample_type = take(runtime.native_type<Sample>());
    const auto& value = field_info(runtime, sample_type, "value");
    const auto& count = field_info(runtime, sample_type, "count");
    require(value.label == "数值 \"value\" \\ preview" && value.category == "Transform" &&
        value.offset == offsetof(Sample, value), "Generated field presentation lost UTF-8, escapes or native layout.");
    require(count.label.empty() && count.category.empty() && !runtime.find_field(sample_type, value.label),
        "Presentation changed field lookup or synthesized fallback metadata.");
    const auto probe_type = take(runtime.native_type<CallProbe>());
    const auto& direction = field_info(runtime, probe_type, "direction");
    require(direction.label == "Position" && direction.category == "Transform" &&
        direction.value.type == take(runtime.native_type<math::Vector3d>()),
        "Generated mathematical field presentation changed its type.");
    const auto& enabled = field_info(runtime, probe_type, "enabled");
    const auto& before = field_info(runtime, probe_type, "before");
    require(enabled.label.empty() && enabled.category == "Flags" && before.label.empty() && before.category.empty(),
        "Category-only or explicitly empty presentation metadata was not preserved.");
    const auto counter_type = take(runtime.native_type<Counter>());
#if defined(_DEBUG)
    const auto& counter_field = field_info(runtime, counter_type, "debug_only");
    require(counter_field.label == "Debug value", "Non-standard-layout field lost its generated label.");
#else
    const auto& counter_field = field_info(runtime, counter_type, "release_only");
    require(counter_field.label == "Release value", "Non-standard-layout field lost its generated label.");
#endif
    require(counter_field.category.empty() && !counter_field.offset && !runtime.find_field(counter_type, "sample_"),
        "Presentation exposed hidden storage or inferred a non-standard native offset.");

    TypeInfo retained{};
    {
        Runtime described;
        TypeDefinition definition;
        definition.name = "reflection.DescribedPresentation";
        definition.fields = {{"value", {TypeReference::native<float>()}}, {"count", {TypeReference::native<std::uint32_t>()}}};
        definition.fields[0].label = value.label;
        definition.fields[0].category = value.category;
        definition.fields[0].description = "描述型 Field\r\n说明";
        // Labels may repeat; only actual member names identify fields.
        definition.fields[1].label = value.label;
        auto plain = definition;
        plain.name = "reflection.UnlabelledPresentation";
        for (auto& field : plain.fields) { field.label.clear(); field.category.clear(); field.description.clear(); }
        TypeDefinition derived;
        derived.name = "reflection.InheritedPresentation";
        derived.base = TypeReference{definition.name};
        const auto types = take(described.register_types({definition, std::move(plain), std::move(derived)}));
        definition.fields.clear();
        retained = *take(described.describe(types[0]));
        const auto* plain_info = take(described.describe(types[1]));
        const auto& inherited = field_info(described, types[2], "value");
        require(retained.size == plain_info->size && retained.alignment == plain_info->alignment &&
            retained.fields.size() == plain_info->fields.size(), "Presentation changed described storage layout.");
        for (std::size_t i = 0; i < retained.fields.size(); ++i)
            require(retained.fields[i].name == plain_info->fields[i].name &&
                retained.fields[i].value == plain_info->fields[i].value &&
                retained.fields[i].offset == plain_info->fields[i].offset &&
                retained.fields[i].default_leaves == plain_info->fields[i].default_leaves,
                "Presentation changed a field's storage contract.");
        require(inherited.id == retained.fields[0].id && inherited.label == value.label && inherited.category == value.category &&
            inherited.description == "描述型 Field\n说明",
            "Inherited metadata lost declaration identity or owning presentation text.");
        auto object = take(described.create(types[2]));
        take(described.write(object.id(), inherited.id, Scalar{9.f}));
        require(std::get<float>(take(described.read(object.id(), inherited.id))) == 9.f &&
            !described.find_field(types[2], value.label), "Label changed described field lookup or access.");
    }
    require(retained.fields[0].label == value.label && retained.fields[0].category == value.category &&
        retained.fields[0].description == "描述型 Field\n说明",
        "Copied presentation metadata did not own its text after Runtime shutdown.");
    std::puts("[pass] Generated and described field presentation, empty defaults, identity, layout, inheritance and ownership");
}
void verify_descriptions(Runtime& runtime) {
    const auto sample = take(runtime.native_type<Sample>());
    const auto counter = take(runtime.native_type<Counter>());
    const auto child = take(runtime.native_type<CounterChild>());
    const auto mode = take(runtime.native_type<Mode>());
    const auto probe = take(runtime.native_type<CallProbe>());
    const auto& value = field_info(runtime, sample, "value");
    const auto& read = method_info(runtime, counter, "read");
    const auto& inherited = method_info(runtime, child, "read");
    require(take(runtime.describe(sample))->description == "Sample 记录。\n包含测量值和次数。\n普通 UTF-8。" &&
        take(runtime.describe(counter))->description == "Counter 累计测量次数。" &&
        take(runtime.describe(mode))->description == "Mode 工作状态。\nidle 表示空闲。",
        "Generated record/class/enum descriptions lost UTF-8 or line normalization.");
    require(value.description == "Value 测量值。\n允许 \"引号\"、\\ 路径和\t缩进。" &&
        field_info(runtime, sample, "count").description == "Count 累计次数。" &&
        value.offset == offsetof(Sample, value), "Generated field description changed text or native layout.");
    require(read.description == "读取当前 Value。\n不会改变计数。" &&
        inherited.id == read.id && inherited.signature == read.signature && inherited.description == read.description &&
        take(runtime.describe(child))->description.empty(),
        "Inherited method description changed declaration identity or synthesized a derived type description.");
    const auto& safe = method_info(runtime, probe, "record_alias");
    require(safe.preserves_borrows && safe.description == "更新 Record 的次数，并返回副本。" &&
        method_info(runtime, counter, "scale").description.empty() &&
        field_info(runtime, probe, "before").description.empty() && field_info(runtime, probe, "after").description.empty(),
        "Description changed BorrowSafe or empty/omitted metadata defaults.");
    // Names come from the declaration, not the Name alias; an unnamed C++ parameter stays empty.
    require(method_info(runtime, counter, "count").parameter_names == std::vector<std::string>{"amount"} &&
        method_info(runtime, probe, "observe_after").parameter_names == std::vector<std::string>{"read", ""} &&
        method_info(runtime, probe, "record_alias").parameter_names == std::vector<std::string>{"copy", "target"} &&
        read.parameter_names.empty(), "Generated method lost or reordered its declared parameter names.");

    struct Manual {
        float value{};
        float read() const noexcept { return value; }
    };
    TypeInfo retained{};
    {
        Runtime native;
        auto definition = NativeRegistration<Manual>{native, "reflection.ManualDescription", TypeKind::object, "手工 Type\r\n说明"}
            .field<&Manual::value>("value", {TypeReference::native<float>()}, offsetof(Manual, value), {}, {}, "手工 Field\r说明")
            .method<&Manual::read>("read", true, "手工 Method\n说明").build();
        const auto id = take(native.register_type(definition));
        definition = {};
        retained = *take(native.describe(id));
        auto object = take(native.create(id));
        take(native.write(object.id(), retained.fields[0].id, Scalar{7.f}));
        Scalar result;
        take(native.call(object.id(), retained.methods[0].id, std::span<const Scalar>{}, &result));
        require(std::get<float>(result) == 7.f, "Description changed direct native invocation.");
    }
    require(retained.description == "手工 Type\n说明" && retained.fields[0].description == "手工 Field\n说明" &&
        retained.methods[0].description == "手工 Method\n说明",
        "Manual producer or copied metadata failed to own normalized descriptions after Runtime shutdown.");

    for (const auto& invalid : {std::string{"embedded\0zero", 13}, std::string{"bad\x7f"},
        std::string{"bad\x01"}, std::string{"bad\xc2\x85"}, std::string{"bad\xff"}}) {
        for (int location = 0; location != 3; ++location) {
            Runtime isolated;
            auto invalid_definition = NativeRegistration<Manual>{isolated, "reflection.RejectedDescription", TypeKind::object}
                .field<&Manual::value>("value").method<&Manual::read>("read").build();
            if (location == 0) invalid_definition.description = invalid;
            else if (location == 1) invalid_definition.fields[0].description = invalid;
            else invalid_definition.methods[0].description = invalid;
            const auto rejected = isolated.register_type(std::move(invalid_definition));
            require(!rejected && rejected.error().code == ErrorCode::invalid_definition &&
                !isolated.find_type("reflection.RejectedDescription"),
                "Invalid description was accepted or partially published a type.");
            const auto recovered = take(NativeRegistration<Manual>{isolated, "reflection.RejectedDescription", TypeKind::object,
                "合法说明"}.field<&Manual::value>("value", "恢复 Field").method<&Manual::read>("read").publish());
            require(take(isolated.describe(recovered))->description == "合法说明" &&
                field_info(isolated, recovered, "value").description == "恢复 Field",
                "Rejected description poisoned subsequent valid registration.");
        }
    }
    std::puts("[pass] Generated/manual UTF-8 descriptions, normalized lines, inherited identity, ownership and atomic rejection");
}
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
            atelier::reflection::Definition<CounterChild>::build(runtime),
            atelier::reflection::Definition<RangeProbe>::build(runtime),
            array_definition<double>(runtime, "reflection.DoubleArray"),
            array_definition<bool>(runtime, "reflection.BoolArray")}));
        verify_field_presentation(runtime);
        verify_descriptions(runtime);
        verify_ranges(runtime);
        verify_external_reference();
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
