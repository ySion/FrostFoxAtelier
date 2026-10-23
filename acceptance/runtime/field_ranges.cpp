#include "runtime/foundation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace {
using namespace atelier;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class T> T take(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(*result);
}
void take(Result<void> result) {
    if (!result) throw std::runtime_error(result.error().message);
}
template<class T> void reject(const Result<T>& result, ErrorCode expected, const char* message) {
    require(!result && result.error().code == expected, message);
}

struct RangedRecord {
    float gain{1.0f};
    float free{};
};
struct RangedOwner {
    float speed{2.0f};
    std::int32_t count{};
    std::uint32_t samples{1};
    double ui_only{};
    math::Vector3 albedo{0.5f, 0.5f, 0.5f};
    RangedRecord nested;
    void accumulate(float& target, const float& amount) { target += amount; }
    float observe(const float& value) const { return value; }
};
// Matches the described ranges.Pair layout: two floats, then a Vector3.
struct PairValue {
    float a, b;
    math::Vector3 tint;
};

FieldRange hard(float low, float high) { return {.hard_min = Scalar{low}, .hard_max = Scalar{high}}; }
FieldDefinition described_field(std::string name, TypeReference type, FieldRange range = {}, std::vector<Scalar> leaves = {}) {
    FieldDefinition field;
    field.name = std::move(name);
    field.value = {std::move(type)};
    field.range = std::move(range);
    field.default_leaves = std::move(leaves);
    return field;
}
TypeDefinition described(std::string name, TypeKind kind, std::vector<FieldDefinition> fields) {
    TypeDefinition definition;
    definition.name = std::move(name);
    definition.kind = kind;
    definition.fields = std::move(fields);
    return definition;
}
}

void verify_field_ranges() {
    Runtime runtime;
    const auto f32 = take(runtime.native_type<float>());
    const auto vector3 = take(runtime.native_type<math::Vector3>());
    constexpr auto nan = std::numeric_limits<float>::quiet_NaN();

    const auto rejected = [&](TypeDefinition definition, const char* message) {
        const auto name = definition.name;
        reject(runtime.register_types({std::move(definition)}), ErrorCode::invalid_definition, message);
        require(!runtime.find_type(name), "Rejected range publication left a visible type.");
    };
    rejected(described("bad.Bool", TypeKind::object, {described_field("value", TypeReference::native<bool>(),
        {.hard_max = Scalar{true}})}), "A bool field accepted a range.");
    rejected(described("bad.Quaternion", TypeKind::object, {described_field("value", TypeReference::native<math::Quaternion>(),
        hard(0.0f, 1.0f))}), "A quaternion field accepted per-component bounds.");
    rejected(described("bad.Precision", TypeKind::object, {described_field("value", {f32}, {.hard_max = Scalar{1.0}})}),
        "A double bound was accepted on a float field.");
    rejected(described("bad.NaN", TypeKind::object, {described_field("value", {f32}, {.ui_min = Scalar{nan}})}),
        "A NaN UI bound was accepted.");
    rejected(described("bad.Order", TypeKind::object, {described_field("value", {f32}, {.ui_min = Scalar{2.0f}, .ui_max = Scalar{1.0f}})}),
        "An inverted UI range was accepted.");
    rejected(described("bad.Outside", TypeKind::object, {described_field("value", {f32},
        {.ui_min = Scalar{-1.0f}, .ui_max = Scalar{1.0f}, .hard_min = Scalar{0.0f}})}),
        "A UI bound below the hard minimum was accepted.");
    rejected(described("bad.Default", TypeKind::object, {described_field("value", {f32}, hard(0.0f, 5.0f), {Scalar{7.0f}})}),
        "A default above the hard maximum was accepted.");
    rejected(described("bad.DefaultNaN", TypeKind::object, {described_field("value", {f32}, {.hard_min = Scalar{0.0f}}, {Scalar{nan}})}),
        "A NaN default was accepted under a hard bound.");
    rejected(described("bad.DefaultComponent", TypeKind::object, {described_field("value", {vector3}, hard(0.0f, 1.0f),
        {Scalar{0.5f}, Scalar{2.0f}, Scalar{0.5f}})}), "A vector default component above the hard maximum was accepted.");
    rejected(described("bad.LeafCount", TypeKind::object, {described_field("value", {vector3}, {}, {Scalar{1.0f}, Scalar{2.0f}})}),
        "A vector default with too few leaves was accepted.");
    rejected(described("bad.LeafType", TypeKind::object, {described_field("value", {vector3}, {}, {Scalar{1.0f}, Scalar{2.0}, Scalar{3.0f}})}),
        "A vector default with a double leaf was accepted.");
    rejected(described("bad.QuaternionDefault", TypeKind::object, {described_field("value", TypeReference::native<math::Quaternion>(), {},
        {Scalar{0.0f}, Scalar{0.0f}, Scalar{0.0f}, Scalar{1.0f}})}), "A quaternion default was accepted.");
    // A default for an enum whose (invalid) float underlying type is declared later in the batch, or earlier.
    for (const bool enum_first : {false, true}) {
        TypeDefinition float_enum;
        float_enum.name = "bad.FloatEnum";
        float_enum.kind = TypeKind::enumeration;
        float_enum.enumeration = TypeDefinition::Enumeration{TypeReference::native<float>(), {{"zero", 0}}};
        auto user = described("bad.FloatEnumUser", TypeKind::object, {described_field("value", TypeReference{"bad.FloatEnum"}, {}, {Scalar{0.0f}})});
        std::vector<TypeDefinition> batch;
        batch.push_back(enum_first ? std::move(float_enum) : std::move(user));
        batch.push_back(enum_first ? std::move(user) : std::move(float_enum));
        reject(runtime.register_types(std::move(batch)), ErrorCode::invalid_definition, "A float-underlying enum default was accepted.");
        require(!runtime.find_type("bad.FloatEnum") && !runtime.find_type("bad.FloatEnumUser"), "A rejected enum default batch left a visible type.");
    }

    const auto published = take(runtime.register_types({
        NativeRegistration<RangedRecord>{runtime, "ranges.Record", TypeKind::record}
            .field<&RangedRecord::gain>("gain").range(hard(0.0f, 2.0f))
            .field<&RangedRecord::free>("free").build(),
        NativeRegistration<RangedOwner>{runtime, "ranges.Owner", TypeKind::object}
            .field<&RangedOwner::speed>("speed")
            .range({.ui_min = Scalar{0.0f}, .ui_max = Scalar{10.0f}, .hard_min = Scalar{0.0f}, .hard_max = Scalar{100.0f}})
            .field<&RangedOwner::count>("count").range({.hard_min = Scalar{std::int32_t{-5}}, .hard_max = Scalar{std::int32_t{5}}})
            .field<&RangedOwner::samples>("samples").range({.hard_min = Scalar{std::uint32_t{1}}})
            .field<&RangedOwner::ui_only>("ui_only").range({.ui_min = Scalar{0.0}, .ui_max = Scalar{1.0}})
            .field<&RangedOwner::albedo>("albedo").range(hard(0.0f, 1.0f))
            .field<&RangedOwner::nested>("nested")
            .method<&RangedOwner::accumulate>("accumulate").method<&RangedOwner::observe>("observe").build(),
        [] { auto derived = described("ranges.Derived", TypeKind::object, {}); derived.base = TypeReference{"ranges.Owner"}; return derived; }(),
        described("ranges.Pair", TypeKind::record, {described_field("a", {f32}, hard(0.0f, 1.0f)), described_field("b", {f32}),
            described_field("tint", {vector3}, hard(0.0f, 1.0f))}),
        described("ranges.Holder", TypeKind::object, {described_field("pair", TypeReference{"ranges.Pair"}),
            described_field("level", {f32}, {.ui_max = Scalar{4.0f}, .hard_max = Scalar{5.0f}}, {Scalar{3.0f}}),
            described_field("tint", {vector3}, hard(0.0f, 1.0f), {Scalar{0.25f}, Scalar{0.5f}, Scalar{0.75f}}),
            described_field("mode", TypeReference{"ranges.Mode"}, {}, {Scalar{std::int32_t{7}}})}),
        // Declared after its user, so the default leaf is checked before the enum resolves its underlying type.
        [] {
            TypeDefinition mode;
            mode.name = "ranges.Mode";
            mode.kind = TypeKind::enumeration;
            mode.enumeration = TypeDefinition::Enumeration{TypeReference::native<std::int32_t>(), {{"low", 1}, {"high", 7}}};
            return mode;
        }(),
    }));
    const auto record = published[0], owner = published[1], derived = published[2], pair = published[3], holder = published[4], mode = published[5];
    const auto field = [&](TypeId type, std::string_view name) { return take(runtime.find_field(type, name)); };
    const auto speed = field(owner, "speed"), count = field(owner, "count"), samples = field(owner, "samples");
    const auto ui_only = field(owner, "ui_only"), albedo = field(owner, "albedo"), nested = field(owner, "nested");
    const auto gain = field(record, "gain"), loose = field(record, "free"), x = field(vector3, "x");

    const auto& inherited = take(runtime.describe(derived))->fields;
    const auto speed_info = std::ranges::find(inherited, speed, &FieldInfo::id);
    require(speed_info != inherited.end() && speed_info->range.ui_max == Scalar{10.0f} && speed_info->range.hard_max == Scalar{100.0f} &&
        !take(runtime.describe(derived))->fields[3].range.constrains(), "Published FieldInfo lost UI or hard bounds through inheritance.");

    auto object = take(runtime.create(derived));
    const auto id = object.id();
    const auto stored = [&](std::span<const FieldId> path, Scalar value) {
        take(runtime.write(id, path, value));
        return take(runtime.read(id, path));
    };
    const std::array speed_path{speed}, count_path{count}, samples_path{samples}, ui_path{ui_only};
    const std::array component{albedo, x}, gain_path{nested, gain}, free_path{nested, loose};
    require(stored(speed_path, Scalar{150.0f}) == Scalar{100.0f} && stored(speed_path, Scalar{-1.0f}) == Scalar{0.0f} &&
        stored(speed_path, Scalar{50.0f}) == Scalar{50.0f}, "Scalar writes ignored hard bounds or applied the UI range.");
    reject(runtime.write(id, speed_path, Scalar{nan}), ErrorCode::argument_mismatch, "NaN reached a hard-bounded field.");
    reject(runtime.write(id, speed_path, Scalar{std::numeric_limits<float>::infinity()}), ErrorCode::argument_mismatch,
        "Infinity reached a hard-bounded field.");
    require(take(runtime.read(id, speed_path)) == Scalar{50.0f}, "A rejected write changed the field.");
    require(stored(count_path, Scalar{std::int32_t{9}}) == Scalar{std::int32_t{5}} &&
        stored(count_path, Scalar{std::int32_t{-9}}) == Scalar{std::int32_t{-5}} &&
        stored(samples_path, Scalar{std::uint32_t{0}}) == Scalar{std::uint32_t{1}} &&
        stored(samples_path, Scalar{std::uint32_t{1000}}) == Scalar{std::uint32_t{1000}}, "Integer hard bounds were not applied per side.");
    require(stored(ui_path, Scalar{7.0}) == Scalar{7.0} && std::isnan(std::get<double>(stored(ui_path, Scalar{std::nan("")}))),
        "A UI-only range constrained writes.");
    require(stored(component, Scalar{3.0f}) == Scalar{1.0f} && stored(component, Scalar{-2.0f}) == Scalar{0.0f},
        "A math component did not inherit its vector's bounds.");
    require(stored(gain_path, Scalar{5.0f}) == Scalar{2.0f}, "A nested record leaf ignored its hard bound.");

    const std::array albedo_path{albedo}, nested_path{nested};
    const auto read_albedo = [&] {
        math::Vector3 value;
        take(runtime.read(id, albedo_path, ValueOutput{vector3, &value}));
        return value;
    };
    math::Vector3 input{2.0f, 0.25f, -1.0f};
    take(runtime.write(id, albedo_path, ValueInput{vector3, &input}));
    require(read_albedo() == math::Vector3{1.0f, 0.25f, 0.0f}, "A whole vector write ignored per-component bounds.");
    input = {0.1f, 0.2f, nan};
    reject(runtime.write(id, albedo_path, ValueInput{vector3, &input}), ErrorCode::argument_mismatch, "A NaN component reached a bounded vector.");
    require(read_albedo() == math::Vector3{1.0f, 0.25f, 0.0f}, "A rejected vector write changed earlier components.");
    RangedRecord nested_input{7.0f, -3.0f};
    take(runtime.write(id, nested_path, ValueInput{record, &nested_input}));
    require(take(runtime.read(id, gain_path)) == Scalar{2.0f} && take(runtime.read(id, free_path)) == Scalar{-3.0f},
        "A whole record write ignored or overreached its constrained leaf.");
    nested_input = {nan, 9.0f};
    reject(runtime.write(id, nested_path, ValueInput{record, &nested_input}), ErrorCode::argument_mismatch, "A NaN leaf reached a bounded record.");
    require(take(runtime.read(id, free_path)) == Scalar{-3.0f}, "A rejected record write changed an unconstrained leaf.");

    auto holding = take(runtime.create(holder));
    const std::array pair_path{field(holder, "pair")}, level_path{field(holder, "level")};
    require(take(runtime.read(holding.id(), level_path)) == Scalar{3.0f} &&
        (take(runtime.write(holding.id(), level_path, Scalar{9.0f})), take(runtime.read(holding.id(), level_path))) == Scalar{5.0f},
        "A described object default or hard maximum was not honored.");
    math::Vector3 tint{};
    take(runtime.read(holding.id(), std::array{field(holder, "tint")}, ValueOutput{vector3, &tint}));
    const auto& holder_fields = take(runtime.describe(holder))->fields;
    const auto mode_info = std::ranges::find(holder_fields, "mode", &FieldInfo::name);
    require(tint == math::Vector3{0.25f, 0.5f, 0.75f} && take(runtime.read(holding.id(), std::array{field(holder, "mode")})) == Scalar{EnumValue{mode, 7}} &&
        mode_info->default_leaves == std::vector<Scalar>{EnumValue{mode, 7}},
        "Vector or forward enum default leaves were not written at their components or published as an EnumValue.");
    rejected(described("bad.EnumLeaf", TypeKind::object, {described_field("value", {mode}, {}, {Scalar{EnumValue{mode, 7}}})}),
        "An EnumValue default leaf was accepted in a definition.");
    rejected(described("bad.EnumUnderlying", TypeKind::object, {described_field("value", {mode}, {}, {Scalar{std::uint32_t{7}}})}),
        "An enum default of the wrong underlying type was accepted.");
    PairValue pair_input{5.0f, -5.0f, {2.0f, -1.0f, 0.5f}}, pair_output{};
    take(runtime.write(holding.id(), pair_path, ValueInput{pair, &pair_input}));
    take(runtime.read(holding.id(), pair_path, ValueOutput{pair, &pair_output}));
    require(pair_output.a == 1.0f && pair_output.b == -5.0f && pair_output.tint == math::Vector3{1.0f, 0.0f, 0.5f},
        "Described record leaves, including vector components, were not constrained.");

    require(take(runtime.constrain(owner, speed_path, Scalar{250.0f})) == Scalar{100.0f} &&
        take(runtime.constrain(derived, component, Scalar{-1.0f})) == Scalar{0.0f} &&
        take(runtime.constrain(record, std::array{gain}, Scalar{3.0f})) == Scalar{2.0f} &&
        take(runtime.constrain(owner, ui_path, Scalar{7.0})) == Scalar{7.0}, "constrain() disagreed with Runtime writes.");
    reject(runtime.constrain(owner, speed_path, Scalar{nan}), ErrorCode::argument_mismatch, "constrain() accepted NaN under a hard bound.");
    reject(runtime.constrain(owner, speed_path, Scalar{1.0}), ErrorCode::type_mismatch, "constrain() accepted the wrong scalar type.");

    const auto accumulate = take(runtime.find_method(owner, "accumulate")), observe = take(runtime.find_method(owner, "observe"));
    reject(runtime.call(id, accumulate, std::array{Argument::field(id, speed_path), Argument{Scalar{1.0f}}}),
        ErrorCode::argument_mismatch, "A hard-bounded field was borrowed writably.");
    reject(runtime.call(id, accumulate, std::array{Argument::field(id, component), Argument{Scalar{1.0f}}}),
        ErrorCode::argument_mismatch, "A bounded vector component was borrowed writably.");
    require(take(runtime.read(id, speed_path)) == Scalar{50.0f}, "A rejected borrow changed the field.");
    Scalar observed;
    take(runtime.call(id, observe, std::array{Argument::field(id, speed_path)}, &observed));
    take(runtime.call(id, accumulate, std::array{Argument::field(id, free_path), Argument{Scalar{1.0f}}}));
    require(observed == Scalar{50.0f} && take(runtime.read(id, free_path)) == Scalar{-2.0f},
        "Read-only borrows or unconstrained writable borrows stopped working.");
    std::puts("[pass] Field ranges: publication checks, UI vs hard bounds, scalar/vector/record clamping, NaN rejection, constrain and borrows; "
        "scalar, vector and enum default leaves");
}
