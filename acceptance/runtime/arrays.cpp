#include "runtime/foundation.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

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
// Element failures name the value the way validation does.
template <class T>
void reject(const Result<T>& result, ErrorCode expected, std::string_view text, const char* message) {
    if (!result && result.error().code == expected && result.error().message == text)
        return;
    throw std::runtime_error(std::string(message) + (result ? "" : " Got: " + result.error().message));
}
Result<void> invalid(const char* message) {
    return std::unexpected(Error{ErrorCode::argument_mismatch, message});
}
FieldRange hard(float low, float high) {
    return {.hard_min = Scalar{low}, .hard_max = Scalar{high}};
}

struct Probe {
    float gain{0.5f};
    float free{};
};
Result<void> valid_probe(const Probe& probe) {
    return probe.free < 0.0f ? invalid("free must not be negative") : Result<void>{};
}
// Copies may throw while moves are only copies, so Array edits take the copy-and-swap path.
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
// Moves cannot throw but swaps can, and Array::insert/move rotate elements with swap.
struct Swapper {
    std::int32_t value{};
    static inline bool fail{};
    friend void swap(Swapper& first, Swapper& second) {
        std::swap(first.value, second.value);
        if (fail)
            throw std::runtime_error("swap refused");
    }
};
static_assert(std::is_nothrow_move_constructible_v<Swapper> && !std::is_nothrow_swappable_v<Swapper>);
struct Owner {
    Array<Probe> probes;
    Array<float> weights;
    Array<math::Vector3> points;
    Array<Fragile> fragile;
    Array<Array<Probe>> layers;
    Array<Swapper> swappers;
    void touch(Array<Probe>& values) { values.clear(); }
    float total(const Array<Probe>& values) const {
        float sum = 0.0f;
        for (const auto& probe : values)
            sum += probe.free;
        return sum;
    }
};
Result<void> valid_owner(const Owner& owner) {
    return owner.probes.size() > 4 ? invalid("at most 4 probes") : Result<void>{};
}
}

void verify_array_elements() {
    Runtime runtime;
    const auto types = take(runtime.register_types({
        NativeRegistration<Probe>{runtime, "arrays.Probe", TypeKind::record}
            .field<&Probe::gain>("gain")
            .range(hard(0.0f, 1.0f))
            .field<&Probe::free>("free")
            .validate<&valid_probe>()
            .build(),
        NativeRegistration<Fragile>{runtime, "arrays.Fragile", TypeKind::record}
            .field<&Fragile::value>("value")
            .field<&Fragile::fail>("fail")
            .build(),
        array_definition<Probe>(runtime, "arrays.ProbeArray"),
        array_definition<float>(runtime, "arrays.FloatArray"),
        array_definition<math::Vector3>(runtime, "arrays.PointArray"),
        array_definition<Fragile>(runtime, "arrays.FragileArray"),
        array_definition<Array<Probe>>(runtime, "arrays.LayerArray"),
        NativeRegistration<Swapper>{runtime, "arrays.Swapper", TypeKind::record}
            .field<&Swapper::value>("value")
            .build(),
        array_definition<Swapper>(runtime, "arrays.SwapperArray"),
        NativeRegistration<Owner>{runtime, "arrays.Owner", TypeKind::object}
            .field<&Owner::probes>("probes")
            .field<&Owner::weights>("weights")
            .range(hard(0.0f, 1.0f))
            .field<&Owner::points>("points")
            .range(hard(-1.0f, 1.0f))
            .field<&Owner::fragile>("fragile")
            .field<&Owner::layers>("layers")
            .field<&Owner::swappers>("swappers")
            .method<&Owner::touch>("touch")
            .method<&Owner::total>("total")
            .validate<&valid_owner>()
            .build(),
    }));
    const auto probe_type = types[0];
    const auto fragile_type = types[1];
    const auto probe_array = types[2];
    const auto float_array = types[3];
    const auto point_array = types[4];
    const auto swapper_type = types[7];
    const auto swapper_array = types[8];
    const auto owner_type = types[9];
    const auto f32 = take(runtime.native_type<float>());
    const auto vector3 = take(runtime.native_type<math::Vector3>());
    const auto field = [&](TypeId type, const char* name) { return take(runtime.find_field(type, name)); };
    const auto probes = field(owner_type, "probes");
    const auto weights = field(owner_type, "weights");
    const auto points = field(owner_type, "points");
    const auto fragile = field(owner_type, "fragile");
    const auto layers = field(owner_type, "layers");
    const auto gain = field(probe_type, "gain");
    const auto free = field(probe_type, "free");
    const auto value = field(fragile_type, "value");
    const auto y = field(vector3, "y");
    const auto z = field(vector3, "z");
    const auto at = [&](TypeId array, std::uint32_t index) { return take(runtime.element(array, index)); };
    constexpr auto nan = std::numeric_limits<float>::quiet_NaN();

    // Element selectors are members of their Array type and nothing else.
    reject(runtime.element(f32, 0), ErrorCode::invalid_type, "A scalar type produced an element selector.");
    require(!runtime.element_index(probes) && runtime.element_index(at(probe_array, 3)) == 3u,
            "Element selectors and field selectors are not distinguishable.");
    reject(runtime.find_field(probe_array, "gain"), ErrorCode::invalid_member, "An Array type exposed a named member.");

    auto owner = take(runtime.create(owner_type));
    const auto id = owner.id();
    const std::array probes_path{probes};
    const std::array weights_path{weights};
    const std::array points_path{points};
    const std::array fragile_path{fragile};
    const auto count = [&](std::span<const FieldId> path) { return take(runtime.element_count(id, path)); };
    take(runtime.insert_element(id, probes_path, 0));
    take(runtime.insert_element(id, probes_path, 1));
    require(count(probes_path) == 2 &&
                take(runtime.read(id, std::array{probes, at(probe_array, 1), gain})) == Scalar{0.5f},
            "Default insertion did not construct elements.");

    // Element leaves resolve through the same paths and hard bounds as record fields.
    const std::array second_gain{probes, at(probe_array, 1), gain};
    take(runtime.write(id, second_gain, Scalar{5.0f}));
    require(take(runtime.read(id, second_gain)) == Scalar{1.0f}, "An element leaf escaped its hard bound.");
    reject(runtime.write(id, second_gain, Scalar{nan}), ErrorCode::argument_mismatch,
           "arrays.Owner.probes[1].gain: Hard-bounded fields reject NaN and infinity.",
           "An element leaf accepted NaN.");
    require(take(runtime.read(id, second_gain)) == Scalar{1.0f}, "A rejected element write changed the value.");
    reject(runtime.read(id, std::array{probes, at(probe_array, 2), gain}), ErrorCode::invalid_member,
           "arrays.Owner.probes[2]: Array element index 2 is out of range of 2 elements.",
           "An out-of-range element selector resolved.");
    reject(runtime.write(id, std::array{probes, at(probe_array, 2), gain}, Scalar{0.1f}), ErrorCode::invalid_member,
           "An out-of-range element write was accepted.");
    reject(runtime.read(id, std::array{probes, at(float_array, 0)}), ErrorCode::invalid_member,
           "Another Array type's selector resolved.");
    reject(runtime.read(id, std::array{at(probe_array, 0)}), ErrorCode::invalid_member,
           "An element selector resolved on an object.");
    reject(runtime.read(id, std::array{probes, gain}), ErrorCode::invalid_member,
           "A record field selected into an Array.");
    require(count(probes_path) == 2, "Rejected element accesses changed the Array.");

    const std::array first_element{probes, at(probe_array, 0)};
    const Probe loud{3.0f, 2.0f};
    take(runtime.write(id, first_element, ValueInput{probe_type, &loud}));
    Probe stored{};
    take(runtime.read(id, first_element, ValueOutput{probe_type, &stored}));
    require(stored.gain == 1.0f && stored.free == 2.0f, "A whole-element write was not clamped leaf by leaf.");
    const Probe spoiled{nan, 0.0f};
    reject(runtime.write(id, first_element, ValueInput{probe_type, &spoiled}), ErrorCode::argument_mismatch,
           "arrays.Owner.probes[0].gain: Hard-bounded fields reject NaN and infinity.",
           "A whole-element write accepted a NaN leaf.");

    // Ranged Arrays of scalars and vectors bound every element.
    const float heavy = 4.0f;
    const float missing = nan;
    take(runtime.insert_element(id, weights_path, 0, {f32, &heavy}));
    reject(runtime.insert_element(id, weights_path, 0, {f32, &missing}), ErrorCode::argument_mismatch,
           "arrays.Owner.weights[0]: Hard-bounded fields reject NaN and infinity.", "An Array insertion accepted NaN.");
    Array<float> gapped;
    gapped.resize(3);
    gapped[2] = nan;
    reject(runtime.write(id, weights_path, ValueInput{float_array, &gapped}), ErrorCode::argument_mismatch,
           "arrays.Owner.weights[2]: Hard-bounded fields reject NaN and infinity.",
           "A ranged Array write accepted NaN.");
    require(count(weights_path) == 1 && take(runtime.read(id, std::array{weights, at(float_array, 0)})) == Scalar{1.0f},
            "A ranged Array insertion was not clamped or a rejected one changed the Array.");
    take(runtime.write(id, std::array{weights, at(float_array, 0)}, Scalar{-3.0f}));
    require(take(runtime.read(id, std::array{weights, at(float_array, 0)})) == Scalar{0.0f},
            "A ranged element write escaped.");
    require(take(runtime.constrain(owner_type, std::array{weights, at(float_array, 7)}, Scalar{9.0f})) == Scalar{1.0f},
            "constrain() did not apply the element range without an object.");
    const math::Vector3 far{2.0f, -3.0f, 0.5f};
    take(runtime.insert_element(id, points_path, 0, {vector3, &far}));
    const std::array point_y{points, at(point_array, 0), y};
    take(runtime.write(id, point_y, Scalar{9.0f}));
    math::Vector3 point{};
    take(runtime.read(id, std::array{points, at(point_array, 0)}, ValueOutput{vector3, &point}));
    require(point.x == 1.0f && point.y == 1.0f && point.z == 0.5f, "Vector elements did not clamp per component.");
    const math::Vector3 hollow{0.0f, nan, 0.0f};
    reject(runtime.insert_element(id, points_path, 1, {vector3, &hollow}), ErrorCode::argument_mismatch,
           "arrays.Owner.points[1].y: Hard-bounded fields reject NaN and infinity.",
           "A vector insertion accepted a NaN component.");
    reject(runtime.write(id, std::array{points, at(point_array, 0)}, ValueInput{vector3, &hollow}),
           ErrorCode::argument_mismatch, "arrays.Owner.points[0].y: Hard-bounded fields reject NaN and infinity.",
           "A vector element write accepted a NaN component.");
    reject(runtime.insert_element(id, points_path, 0, {f32, &heavy}), ErrorCode::type_mismatch,
           "A wrong element type was inserted.");
    reject(runtime.insert_element(id, points_path, 3), ErrorCode::argument_mismatch,
           "arrays.Owner.points: Insertion at 3 is out of range of 1 elements.",
           "An insertion past the end was accepted.");

    // Structural edits keep order semantics and leave the Array unchanged on failure.
    take(runtime.insert_element(id, probes_path, 2));
    for (std::uint32_t i = 0; i != 3; ++i)
        take(runtime.write(id, std::array{probes, at(probe_array, i), free}, Scalar{float(i + 1)}));
    const auto frees = [&] {
        std::string result;
        for (std::uint32_t i = 0; i != count(probes_path); ++i)
            result += std::to_string(
                int(std::get<float>(take(runtime.read(id, std::array{probes, at(probe_array, i), free})))));
        return result;
    };
    take(runtime.move_element(id, probes_path, 0, 2));
    require(frees() == "231", "Moving forward did not shift the elements between.");
    take(runtime.move_element(id, probes_path, 2, 0));
    require(frees() == "123", "Moving backward did not restore the order.");
    reject(runtime.move_element(id, probes_path, 0, 3), ErrorCode::argument_mismatch,
           "arrays.Owner.probes: Move from 0 to 3 is out of range of 3 elements.", "A move past the end was accepted.");
    reject(runtime.remove_elements(id, probes_path, 1, 5), ErrorCode::argument_mismatch,
           "arrays.Owner.probes: Removal of 5 at 1 is out of range of 3 elements.",
           "An oversized removal was accepted.");
    take(runtime.remove_elements(id, probes_path, 1));
    require(frees() == "13", "Removal did not close the gap.");
    reject(runtime.read(id, std::array{probes, at(probe_array, 2), free}), ErrorCode::invalid_member,
           "A selector past the shortened Array still resolved.");

    take(runtime.insert_element(id, fragile_path, 0));
    take(runtime.insert_element(id, fragile_path, 1));
    take(runtime.write(id, std::array{fragile, at(types[5], 1), value}, Scalar{2.0f}));
    Fragile refused;
    refused.value = 9.0f;
    refused.fail = true;
    reject(runtime.insert_element(id, fragile_path, 1, {fragile_type, &refused}), ErrorCode::invocation_failed,
           "A throwing element copy was not reported.");
    require(count(fragile_path) == 2 &&
                take(runtime.read(id, std::array{fragile, at(types[5], 1), value})) == Scalar{2.0f},
            "A failed insertion changed the Array.");
    // A rotation that throws midway must not leave the Array reordered.
    const std::array swappers_path{field(owner_type, "swappers")};
    const auto swapped = field(swapper_type, "value");
    for (const std::int32_t number : {11, 22}) {
        const Swapper item{number};
        take(runtime.insert_element(id, swappers_path, count(swappers_path), {swapper_type, &item}));
    }
    const auto order = [&] {
        std::string text;
        for (std::uint32_t i = 0; i != count(swappers_path); ++i)
            text += std::to_string(std::get<std::int32_t>(
                        take(runtime.read(id, std::array{swappers_path[0], at(swapper_array, i), swapped})))) +
                    ",";
        return text;
    };
    Swapper::fail = true;
    reject(runtime.insert_element(id, swappers_path, 0), ErrorCode::invocation_failed,
           "A throwing element swap was not reported.");
    const auto inserted = order();
    reject(runtime.move_element(id, swappers_path, 0, 1), ErrorCode::invocation_failed,
           "A throwing element swap was not reported.");
    Swapper::fail = false;
    require(inserted == "11,22," && order() == "11,22,", "A failed insertion or move left the Array reordered.");

    // Borrowed element addresses cannot outlive a structural change.
    const auto busy = runtime.with_native<Owner>(id, [&](Owner&) -> Result<void> {
        reject(runtime.insert_element(id, probes_path, 0), ErrorCode::busy,
               "A native borrow allowed a structural change.");
        return {};
    });
    take(busy);
    const auto touch = take(runtime.find_method(owner_type, "touch"));
    const auto total = take(runtime.find_method(owner_type, "total"));
    reject(runtime.call(id, touch, std::array{Argument::field(id, probes_path)}), ErrorCode::argument_mismatch,
           "An Array with hard-bounded elements was borrowed writably.");
    Scalar sum;
    take(runtime.call(id, total, std::array{Argument::field(id, probes_path)}, &sum));
    require(sum == Scalar{4.0f} && count(probes_path) == 2, "A read-only Array borrow failed or changed the Array.");

    // validate() expands Array elements: hard bounds first, then element validators before their owner's.
    const auto failure = [&] {
        auto result = take(runtime.validate(id));
        require(result.has_value(), "An invalid Array element passed validation.");
        return std::move(*result);
    };
    const auto edit = [&](auto change) {
        take(runtime.with_native<Owner>(id, [&](Owner& value) -> Result<void> {
            change(value);
            return {};
        }));
    };
    require(!take(runtime.validate(id)), "A valid object with Arrays failed validation.");
    edit([](Owner& value) {
        value.probes[1].gain = 7.0f;
        value.probes[0].free = -1.0f;
    });
    auto found = failure();
    require(found.field == std::vector{probes, at(probe_array, 1), gain} &&
                found.names == std::vector<std::string>{"probes", "[1]", "gain"} && found.value == Scalar{7.0f} &&
                found.error.message == "arrays.Owner.probes[1].gain: 7 is above the hard maximum 1",
            "An element hard bound was not reported first with its element path.");
    edit([](Owner& value) {
        value.probes[1].gain = 0.5f;
        value.points[0].z = 5.0f;
    });
    found = failure();
    require(found.field == std::vector{points, at(point_array, 0), z} &&
                found.error.message == "arrays.Owner.points[0].z: 5 is above the hard maximum 1",
            "A ranged vector element was not checked per component before validators.");
    edit([](Owner& value) {
        value.points[0].z = 0.0f;
        value.probes.resize(5);
        value.probes[4].free = -1.0f;
        value.probes[0].free = 1.0f;
    });
    found = failure();
    require(found.field == std::vector{probes, at(probe_array, 4)} &&
                found.error.message == "arrays.Owner.probes[4]: free must not be negative",
            "An element validator did not run before its owner's validator.");
    edit([](Owner& value) { value.probes[4].free = 0.0f; });
    require(failure().error.message == "arrays.Owner: at most 4 probes",
            "The owner's validator did not run after its elements.");
    edit([](Owner& value) {
        value.probes.resize(2);
        value.layers.resize(2);
        value.layers[1].resize(1);
        value.layers[1][0].free = -2.0f;
    });
    found = failure();
    require(found.field == std::vector{layers, at(types[6], 1), at(probe_array, 0)} &&
                found.names == std::vector<std::string>{"layers", "[1]", "[0]"} &&
                found.error.message == "arrays.Owner.layers[1][0]: free must not be negative",
            "Nested Arrays were not expanded.");

    Array<Probe> loose;
    loose.resize(2);
    loose[1].gain = -1.0f;
    alignas(Array<Probe>) std::array<std::byte, sizeof(Array<Probe>)> array_bytes;
    std::memcpy(array_bytes.data(), &loose, sizeof loose);
    const auto value_failure = take(runtime.validate_value(probe_array, array_bytes));
    require(value_failure && value_failure->field == std::vector{at(probe_array, 1), gain} &&
                value_failure->error.message == "arrays.ProbeArray[1].gain: -1 is below the hard minimum 0",
            "An Array value did not report element paths relative to itself.");
    std::puts(
        "[pass] Array elements: selectors, element paths and bounds, insert/remove/move atomicity, busy borrows and element validation");
}
