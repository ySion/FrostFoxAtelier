#include "runtime/foundation.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {
using namespace atelier;

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
template <class T>
void reject(const Result<T>& result, ErrorCode expected, const char* message) {
    require(!result && result.error().code == expected, message);
}
FieldRange hard(float low, float high) {
    return {.hard_min = Scalar{low}, .hard_max = Scalar{high}};
}
std::string_view text(const String& value) {
    return {value.c_str(), value.size()};
}

struct Leaf {
    float gain{0.5f};
    std::int32_t count{};
};
struct Settings {
    Leaf leaf;
    String label;
    Array<float> weights;
    Array<Leaf> leaves;
};
// Copies may throw and moves are copies, so only the Array that holds them swaps without throwing.
struct Fragile {
    float value{};
    bool fail{};
    Fragile() = default;
    Fragile(const Fragile& other) : value(other.value), fail(other.fail) {
        if (other.fail)
            throw std::runtime_error("copy refused");
    }
    Fragile& operator=(const Fragile&) = default;
};
// A throwing swap leaves only copy assignment for whole-value writes.
struct Sticky {
    String text;
};
[[maybe_unused]] void swap(Sticky& first, Sticky& second) {
    first.text.swap(second.text);
}
static_assert(!std::is_nothrow_swappable_v<Sticky>);

TypeDefinition trusted(TypeDefinition definition) {
    definition.native.value_operations_preserve_borrows = true;
    return definition;
}
FieldDefinition described_field(std::string name, TypeReference type, FieldRange range = {},
                                ValueForm form = ValueForm::inline_value) {
    FieldDefinition field;
    field.name = std::move(name);
    field.value = {std::move(type), form};
    field.range = std::move(range);
    return field;
}
TypeDefinition described(std::string name, TypeKind kind, std::vector<FieldDefinition> fields) {
    TypeDefinition definition;
    definition.name = std::move(name);
    definition.kind = kind;
    definition.fields = std::move(fields);
    return definition;
}
// Caller-owned storage for one live value, destroyed through the Runtime.
template <std::size_t Size>
struct Value {
    Runtime& runtime;
    TypeId type;
    alignas(16) std::array<std::byte, Size> bytes{};
    Value(Runtime& owner, TypeId id) : runtime(owner), type(id) { take(runtime.initialize_value(type, bytes)); }
    ~Value() { (void)runtime.destroy_value(type, bytes); }
};
}

void verify_value_paths() {
    Runtime runtime;
    const auto types = take(runtime.register_types({
        string_definition(runtime),
        NativeRegistration<Leaf>{runtime, "values.Leaf", TypeKind::record}
            .field<&Leaf::gain>("gain")
            .range(hard(0.0f, 1.0f))
            .field<&Leaf::count>("count")
            .build(),
        array_definition<float>(runtime, "values.FloatArray"),
        array_definition<Leaf>(runtime, "values.LeafArray"),
        trusted(NativeRegistration<Settings>{runtime, "values.Settings", TypeKind::record}
                    .field<&Settings::leaf>("leaf")
                    .field<&Settings::label>("label")
                    .field<&Settings::weights>("weights")
                    .range(hard(0.0f, 1.0f))
                    .field<&Settings::leaves>("leaves")
                    .build()),
        trusted(NativeRegistration<Fragile>{runtime, "values.Fragile", TypeKind::record}
                    .field<&Fragile::value>("value")
                    .field<&Fragile::fail>("fail")
                    .build()),
        array_definition<Fragile>(runtime, "values.FragileArray"),
        trusted(NativeRegistration<Sticky>{runtime, "values.Sticky", TypeKind::record}
                    .field<&Sticky::text>("text")
                    .build()),
        described("values.Pair", TypeKind::record,
                  {described_field("text", TypeReference{"core.String"}),
                   described_field("weight", TypeReference::native<float>(), hard(0.0f, 1.0f))}),
        described("values.Link", TypeKind::record,
                  {described_field("target", {runtime.object_type()}, {}, ValueForm::weak_reference)}),
        described("values.Twin", TypeKind::record,
                  {described_field("first", TypeReference{"values.FragileArray"}),
                   described_field("second", TypeReference{"values.FragileArray"})}),
        described("values.Bag", TypeKind::object,
                  {described_field("name", TypeReference{"core.String"}),
                   described_field("settings", TypeReference{"values.Settings"}),
                   described_field("pair", TypeReference{"values.Pair"}),
                   described_field("fragile", TypeReference{"values.FragileArray"}),
                   described_field("sticky", TypeReference{"values.Sticky"}),
                   described_field("link", TypeReference{"values.Link"}),
                   described_field("twin", TypeReference{"values.Twin"}),
                   described_field("point", TypeReference::native<math::Vector3>(), hard(-1.0f, 1.0f))}),
    }));
    const auto string_type = types[0], leaf_type = types[1], float_array = types[2], leaf_array = types[3],
               settings_type = types[4];
    const auto fragile_array = types[6], sticky_type = types[7], pair_type = types[8], twin_type = types[10],
               bag_type = types[11];
    const auto f64 = take(runtime.native_type<double>());
    const auto field = [&](TypeId type, const char* name) { return take(runtime.find_field(type, name)); };
    const auto at = [&](TypeId array, std::uint32_t index) { return take(runtime.element(array, index)); };
    const auto gain = field(leaf_type, "gain"), leaf = field(settings_type, "leaf"),
               label = field(settings_type, "label");
    const auto weights = field(settings_type, "weights"), leaves = field(settings_type, "leaves");
    const auto pair_text = field(pair_type, "text"), pair_weight = field(pair_type, "weight");
    const auto name = field(bag_type, "name"), settings = field(bag_type, "settings"), pair = field(bag_type, "pair");
    const auto fragile = field(bag_type, "fragile"), sticky = field(bag_type, "sticky"), link = field(bag_type, "link");
    const auto twin = field(bag_type, "twin"), first = field(twin_type, "first"), second = field(twin_type, "second");
    constexpr auto nan = std::numeric_limits<float>::quiet_NaN();

    // Caller-owned records take the same paths, exact types and hard bounds as object fields.
    {
        Value<sizeof(Settings)> value{runtime, settings_type};
        const std::array leaf_gain{leaf, gain};
        take(runtime.write(settings_type, value.bytes, leaf_gain, Scalar{5.0f}));
        require(take(runtime.read(settings_type, value.bytes, leaf_gain)) == Scalar{1.0f},
                "A caller-owned leaf escaped its hard bound.");
        reject(runtime.write(settings_type, value.bytes, leaf_gain, Scalar{nan}), ErrorCode::argument_mismatch,
               "A caller-owned leaf accepted NaN.");
        reject(runtime.write(settings_type, value.bytes, leaf_gain, Scalar{2.0}), ErrorCode::type_mismatch,
               "A caller-owned leaf accepted another scalar type.");
        require(take(runtime.read(settings_type, value.bytes, leaf_gain)) == Scalar{1.0f},
                "A rejected write changed caller storage.");
        reject(
            runtime.read(settings_type, std::span<const std::byte>{value.bytes}.first(sizeof(Settings) - 1), leaf_gain),
            ErrorCode::invalid_storage, "Undersized caller storage was accepted.");

        // A whole Array written into a ranged field is clamped per element.
        Array<float> loud{4.0f, 0.5f, -2.0f};
        take(runtime.write(settings_type, value.bytes, std::array{weights}, ValueInput{float_array, &loud}));
        require(take(runtime.read(settings_type, value.bytes, std::array{weights, at(float_array, 0)})) ==
                        Scalar{1.0f} &&
                    take(runtime.read(settings_type, value.bytes, std::array{weights, at(float_array, 2)})) ==
                        Scalar{0.0f} &&
                    loud[0] == 4.0f,
                "A ranged Array field did not clamp each written element, or the input changed.");
        reject(runtime.read(settings_type, value.bytes, std::array{weights, at(float_array, 3)}),
               ErrorCode::invalid_member, "A caller-owned element selector past the end resolved.");
        Array<float> missing{0.5f, nan};
        reject(runtime.write(settings_type, value.bytes, std::array{weights}, ValueInput{float_array, &missing}),
               ErrorCode::argument_mismatch, "An Array with a NaN element was written.");
        Array<float> kept;
        take(runtime.read(settings_type, value.bytes, std::array{weights}, ValueOutput{float_array, &kept}));
        require(kept.size() == 3 && kept[1] == 0.5f,
                "A rejected Array write changed the field, or the read copy was wrong.");

        String hello{StringView{"hello"}};
        take(runtime.write(settings_type, value.bytes, std::array{label}, ValueInput{string_type, &hello}));
        String read_back;
        take(runtime.read(settings_type, value.bytes, std::array{label}, ValueOutput{string_type, &read_back}));
        require(text(read_back) == "hello", "A caller-owned String leaf did not round-trip.");
    }

    auto bag = take(runtime.create(bag_type));
    const auto id = bag.id();
    // String leaves on objects, including inside a described record.
    String title{StringView{"a title longer than any inline buffer would hold"}};
    take(runtime.write(id, std::array{name}, ValueInput{string_type, &title}));
    take(runtime.write(id, std::array{pair, pair_text}, ValueInput{string_type, &title}));
    String copy;
    take(runtime.read(id, std::array{pair, pair_text}, ValueOutput{string_type, &copy}));
    require(text(copy) == text(title), "A String leaf inside a described record did not round-trip.");

    // A whole described record with a String composes field swaps: clamped copy, input untouched.
    struct PairValue {
        String text;
        float weight;
    };
    PairValue heavy{String{StringView{"heavy"}}, 7.0f};
    take(runtime.write(id, std::array{pair}, ValueInput{pair_type, &heavy}));
    take(runtime.read(id, std::array{pair, pair_text}, ValueOutput{string_type, &copy}));
    require(text(copy) == "heavy" && take(runtime.read(id, std::array{pair, pair_weight})) == Scalar{1.0f} &&
                heavy.weight == 7.0f,
            "A described record write lost its String, skipped clamping or changed its input.");

    // Whole native records clamp nested leaves and Array elements in the prepared copy.
    Settings loud;
    loud.leaf.gain = 9.0f;
    loud.label = String{StringView{"loud"}};
    loud.weights.add(5.0f);
    loud.leaves.add(Leaf{3.0f, 4});
    take(runtime.write(id, std::array{settings}, ValueInput{settings_type, &loud}));
    require(take(runtime.read(id, std::array{settings, leaf, gain})) == Scalar{1.0f} &&
                take(runtime.read(id, std::array{settings, weights, at(float_array, 0)})) == Scalar{1.0f} &&
                take(runtime.read(id, std::array{settings, leaves, at(leaf_array, 0), gain})) == Scalar{1.0f} &&
                take(runtime.read(id, std::array{settings, leaves, at(leaf_array, 0), field(leaf_type, "count")})) ==
                    Scalar{std::int32_t{4}} &&
                loud.leaf.gain == 9.0f,
            "A whole record write did not clamp nested leaves and elements, or changed its input.");
    loud.leaves[0].gain = nan;
    reject(runtime.write(id, std::array{settings}, ValueInput{settings_type, &loud}), ErrorCode::argument_mismatch,
           "A record with a NaN element leaf was written.");
    require(take(runtime.read(id, std::array{settings, leaf, gain})) == Scalar{1.0f},
            "A rejected record write changed the field.");

    // A copy that throws leaves the field unchanged (strong guarantee through the prepared copy).
    Array<Fragile> good{Fragile{}};
    good[0].value = 2.0f;
    take(runtime.write(id, std::array{fragile}, ValueInput{fragile_array, &good}));
    Array<Fragile> bad{Fragile{}, Fragile{}};
    bad[1].fail = true;
    reject(runtime.write(id, std::array{fragile}, ValueInput{fragile_array, &bad}), ErrorCode::assignment_failed,
           "A throwing element copy was not reported.");
    const auto fragile_value = field(take(runtime.native_type<Fragile>()), "value");
    require(take(runtime.element_count(id, std::array{fragile})) == 1 &&
                take(runtime.read(id, std::array{fragile, at(fragile_array, 0), fragile_value})) == Scalar{2.0f},
            "A failed whole-Array write changed the field.");
    // Field-by-field assignment would change `first` before `second` throws; the prepared copy does not.
    struct TwinValue {
        Array<Fragile> first, second;
    };
    TwinValue twin_value;
    twin_value.first.add(good[0]);
    take(runtime.write(id, std::array{twin}, ValueInput{twin_type, &twin_value}));
    TwinValue partial;
    partial.first.resize(3);
    partial.second.add(Fragile{});
    partial.second[0].fail = true;
    reject(runtime.write(id, std::array{twin}, ValueInput{twin_type, &partial}), ErrorCode::assignment_failed,
           "A throwing record copy was not reported.");
    require(take(runtime.element_count(id, std::array{twin, first})) == 1 &&
                take(runtime.element_count(id, std::array{twin, second})) == 0,
            "A failed whole-record write changed an earlier field.");
    // Reads give the same guarantee to their output.
    take(runtime.with_native<Array<Fragile>>(id, std::array{fragile}, [](Array<Fragile>& values) -> Result<void> {
        values[0].fail = true;
        return {};
    }));
    Array<Fragile> output{Fragile{}};
    output[0].value = 7.0f;
    reject(runtime.read(id, std::array{fragile}, ValueOutput{fragile_array, &output}), ErrorCode::assignment_failed,
           "A throwing read copy was not reported.");
    require(output.size() == 1 && output[0].value == 7.0f, "A failed read changed its output.");

    // Without a noexcept swap the write falls back to copy assignment and still succeeds.
    Sticky note{String{StringView{"note"}}};
    take(runtime.write(id, std::array{sticky}, ValueInput{sticky_type, &note}));
    Sticky noted;
    take(runtime.read(id, std::array{sticky}, ValueOutput{sticky_type, &noted}));
    require(text(noted.text) == "note", "A value without a noexcept swap was not written.");

    // Replacing a buffer is busy while borrowed; plain writes and caller storage are not affected.
    take(runtime.with_native<Settings>(id, std::array{settings}, [&](Settings&) -> Result<void> {
        reject(runtime.write(id, std::array{name}, ValueInput{string_type, &title}), ErrorCode::busy,
               "A String field was replaced during a native borrow.");
        take(runtime.write(id, std::array{pair, pair_weight}, Scalar{0.25f}));
        Value<sizeof(Settings)> scratch{runtime, settings_type};
        return runtime.write(settings_type, scratch.bytes, std::array{label}, ValueInput{string_type, &title});
    }));

    // restore undoes the caller's own write with what it read before: bit for bit, without bounds.
    const auto f32 = take(runtime.native_type<float>()), vector3 = take(runtime.native_type<math::Vector3>());
    const auto bits = [](float value) { return std::bit_cast<std::uint32_t>(value); };
    const auto undo = [&](std::span<const FieldId> path, float rejected) {
        float previous{};
        take(runtime.read(id, path, ValueOutput{f32, &previous}));
        take(runtime.write(id, path, Scalar{rejected}));
        take(runtime.restore(id, path, ValueOutput{f32, &previous}));
        require(previous == rejected, "restore did not hand back the replaced value.");
        float restored{};
        take(runtime.read(id, path, ValueOutput{f32, &restored}));
        return restored;
    };
    const auto payload = std::bit_cast<float>(0x7fc0'1234u);
    const std::array weight_path{pair, pair_weight};
    take(runtime.with_native<float>(id, weight_path, [&](float& value) -> Result<void> {
        value = payload;
        return {};
    }));
    require(bits(undo(weight_path, 0.5f)) == bits(payload), "restore did not put a NaN back bit for bit.");
    take(runtime.with_native<float>(id, weight_path, [](float& value) -> Result<void> {
        value = 2.0f;
        return {};
    }));
    require(undo(weight_path, 0.25f) == 2.0f, "restore clamped an out-of-bound value.");
    const std::array point_x{field(bag_type, "point"), field(vector3, "x")};
    take(runtime.with_native<math::Vector3>(id, std::array{point_x[0]}, [](math::Vector3& value) -> Result<void> {
        value.x = 3.0f;
        return {};
    }));
    require(undo(point_x, 0.0f) == 3.0f, "restore did not accept a vector component leaf.");
    const std::array element_gain{settings, leaves, at(leaf_array, 0), gain};
    take(runtime.with_native<Settings>(id, std::array{settings}, [](Settings& value) -> Result<void> {
        value.leaves[0].gain = 5.0f;
        return {};
    }));
    require(undo(element_gain, 0.5f) == 5.0f, "restore did not accept an Array element leaf.");
    // Non-plain values exchange: the field gets the old value back, the caller the rejected one.
    take(runtime.with_native<float>(id, weight_path, [&](float& value) -> Result<void> {
        value = payload;
        return {};
    }));
    PairValue before;
    take(runtime.read(id, std::array{pair}, ValueOutput{pair_type, &before}));
    PairValue replacement{String{StringView{"replacement"}}, 0.5f};
    take(runtime.write(id, std::array{pair}, ValueInput{pair_type, &replacement}));
    take(runtime.restore(id, std::array{pair}, ValueOutput{pair_type, &before}));
    take(runtime.read(id, std::array{pair, pair_text}, ValueOutput{string_type, &copy}));
    float weight{};
    take(runtime.read(id, weight_path, ValueOutput{f32, &weight}));
    require(text(copy) == "heavy" && bits(weight) == bits(payload) && text(before.text) == "replacement",
            "restore did not exchange a whole record with a NaN leaf.");
    // Refusals change neither side.
    reject(runtime.restore(id, std::array{sticky}, ValueOutput{sticky_type, &noted}), ErrorCode::unsupported_operation,
           "restore accepted a value without a non-throwing swap.");
    require(text(noted.text) == "note", "A refused restore changed its value.");
    reject(runtime.restore(id, std::array{name}, ValueOutput{f64, &title}), ErrorCode::type_mismatch,
           "restore accepted another value type.");
    take(runtime.with_native<Settings>(id, std::array{settings}, [&](Settings&) -> Result<void> {
        reject(runtime.restore(id, std::array{name}, ValueOutput{string_type, &copy}), ErrorCode::busy,
               "A String field was restored during a native borrow.");
        return {};
    }));
    require(text(copy) == "heavy", "A busy restore changed its value.");

    // Values holding GC references keep using reference writes.
    alignas(16) std::array<std::byte, 64> link_bytes{};
    reject(runtime.write(id, std::array{link}, ValueInput{take(runtime.find_type("values.Link")), link_bytes.data()}),
           ErrorCode::type_mismatch, "A value with GC references was replaced as a whole.");
    reject(
        runtime.restore(id, std::array{link}, ValueOutput{take(runtime.find_type("values.Link")), link_bytes.data()}),
        ErrorCode::type_mismatch, "A value with GC references was restored.");
    reject(runtime.write(id, std::array{name}, ValueInput{f64, &title}), ErrorCode::type_mismatch,
           "A String field accepted another value type.");
    std::puts(
        "[pass] Value paths: caller-owned records, String and Array leaves, clamped copies, strong replacement, busy borrows and bitwise restore");
}
