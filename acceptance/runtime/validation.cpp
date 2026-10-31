#include "runtime/foundation.hpp"

#include <array>
#include <cstdio>
#include <cstring>
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
Result<void> invalid(const char* message) {
    return std::unexpected(Error{ErrorCode::argument_mismatch, message});
}

struct Interval {
    float low{};
    float high{1.0f};
};
Result<void> valid_span(const Interval& span) {
    return span.low > span.high ? invalid("low exceeds high") : Result<void>{};
}
struct Settings {
    Interval span;
    float gain{0.5f};
    math::Vector3 tint{0.5f, 0.5f, 0.5f};
    bool strict{};
};
Result<void> valid_settings(const Settings& settings) {
    return settings.strict && settings.span.low < 0.0f ? invalid("strict spans start at zero") : Result<void>{};
}
struct Base {
    std::uint32_t count{};
    Settings settings;
    Result<void> check() const { return count % 2 == 0 ? Result<void>{} : invalid("count must be even"); }
};
struct Derived : Base {
    float weight{1.0f};
    bool explode{};
};
Result<void> valid_derived(const Derived& derived) {
    if (derived.explode)
        throw std::runtime_error("boom");
    return derived.weight == 0.0f ? invalid("weight must not be zero") : Result<void>{};
}
}

void verify_type_validation() {
    Runtime runtime;
    TypeDefinition validated_enum;
    validated_enum.name = "validation.Mode";
    validated_enum.kind = TypeKind::enumeration;
    validated_enum.enumeration = TypeDefinition::Enumeration{TypeReference::native<std::int32_t>(), {{"idle", 0}}};
    validated_enum.validate = [](const void*) -> Result<void> { return {}; };
    const auto rejected = runtime.register_types({std::move(validated_enum)});
    require(!rejected && rejected.error().code == ErrorCode::invalid_definition &&
                !runtime.find_type("validation.Mode"),
            "An enumeration accepted a type validator.");

    const auto types = take(runtime.register_types({
        NativeRegistration<Interval>{runtime, "validation.Interval", TypeKind::record}
            .field<&Interval::low>("low")
            .field<&Interval::high>("high")
            .validate<&valid_span>()
            .build(),
        NativeRegistration<Settings>{runtime, "validation.Settings", TypeKind::record}
            .field<&Settings::span>("span")
            .field<&Settings::gain>("gain")
            .range({.hard_min = Scalar{0.0f}, .hard_max = Scalar{1.0f}})
            .field<&Settings::tint>("tint")
            .range({.hard_min = Scalar{0.0f}, .hard_max = Scalar{1.0f}})
            .field<&Settings::strict>("strict")
            .validate<&valid_settings>()
            .build(),
        NativeRegistration<Base>{runtime, "validation.Base", TypeKind::object}
            .field<&Base::count>("count")
            .field<&Base::settings>("settings")
            .validate<&Base::check>()
            .build(),
        NativeRegistration<Derived>{runtime, "validation.Derived", TypeKind::object}
            .base<Base>()
            .field<&Derived::weight>("weight")
            .field<&Derived::explode>("explode")
            .validate<&valid_derived>()
            .build(),
    }));
    const auto span_type = types[0], settings_type = types[1], base = types[2], derived = types[3];
    const auto vector3 = take(runtime.native_type<math::Vector3>());
    const auto settings = take(runtime.find_field(base, "settings")),
               span = take(runtime.find_field(settings_type, "span"));
    const auto gain = take(runtime.find_field(settings_type, "gain")),
               tint = take(runtime.find_field(settings_type, "tint"));
    const auto y = take(runtime.find_field(vector3, "y"));
    require(take(runtime.find_field(span_type, "low")) != gain, "Distinct record fields share an identity.");

    auto object = take(runtime.create(derived));
    const auto id = object.id();
    const auto edit = [&](auto change) {
        take(runtime.with_native<Derived>(id, [&](Derived& value) -> Result<void> {
            change(value);
            return {};
        }));
    };
    const auto failure = [&] {
        auto result = take(runtime.validate(id));
        require(result.has_value(), "An invalid object passed validation.");
        return std::move(*result);
    };
    require(!take(runtime.validate(id)), "A valid object failed validation.");

    // Trusted direct writes bypass clamping; validate reports them without repairing.
    edit([](Derived& value) { value.settings.gain = 5.0f; });
    auto found = failure();
    require(found.field == std::vector{settings, gain} && found.names == std::vector<std::string>{"settings", "gain"} &&
                found.value == Scalar{5.0f} && found.error.code == ErrorCode::argument_mismatch &&
                found.error.message == "validation.Derived.settings.gain: 5 is above the hard maximum 1",
            "A hard-bound failure lost its inherited leaf path, value or readable message.");
    require(take(runtime.read(id, std::array{settings, gain})) == Scalar{5.0f},
            "validate() changed the value it checked.");

    edit([](Derived& value) {
        value.settings.gain = 0.5f;
        value.settings.tint.y = 2.0f;
        value.settings.span = {2.0f, 1.0f};
    });
    found = failure();
    require(found.field == std::vector{settings, tint, y} &&
                found.names == std::vector<std::string>{"settings", "tint", "y"},
            "Hard bounds were not checked before validators, or a component path was wrong.");
    edit([](Derived& value) {
        value.settings.tint.y = 0.5f;
        value.settings.strict = true;
        value.settings.span = {-1.0f, -2.0f};
    });
    found = failure();
    require(found.field == std::vector{settings, span} && !found.value &&
                found.error.code == ErrorCode::argument_mismatch &&
                found.error.message == "validation.Derived.settings.span: low exceeds high",
            "An inner record validator did not run before its containing record.");
    edit([](Derived& value) { value.settings.span.high = 0.0f; });
    found = failure();
    require(found.field == std::vector{settings} &&
                found.error.message == "validation.Derived.settings: strict spans start at zero",
            "A record validator failure lost its record path.");

    edit([](Derived& value) {
        value.settings.strict = false;
        value.count = 3;
        value.weight = 0.0f;
    });
    found = failure();
    require(found.field.empty() && found.error.message == "validation.Derived: count must be even",
            "The base validator did not run before the derived validator.");
    edit([](Derived& value) { value.count = 4; });
    require(failure().error.message == "validation.Derived: weight must not be zero",
            "The derived validator did not run last.");
    edit([](Derived& value) {
        value.weight = 1.0f;
        value.explode = true;
    });
    found = failure();
    require(found.error.code == ErrorCode::invocation_failed &&
                found.error.message == "validation.Derived: validator threw: boom",
            "A throwing validator escaped or lost its classification.");

    Settings loose;
    loose.span = {3.0f, 0.0f};
    alignas(Settings) std::array<std::byte, sizeof(Settings)> bytes;
    std::memcpy(bytes.data(), &loose, sizeof loose);
    const auto value_failure = take(runtime.validate_value(settings_type, bytes));
    require(value_failure && value_failure->field == std::vector{span} &&
                value_failure->error.message == "validation.Settings.span: low exceeds high",
            "Value validation did not report paths relative to the value type.");
    alignas(float) std::array<std::byte, sizeof(float)> scalar{};
    require(!take(runtime.validate_value(take(runtime.native_type<float>()), scalar)),
            "A type without rules failed validation.");

    take(runtime.destroy(id));
    const auto stale = runtime.validate(id);
    require(!stale && stale.error().code == ErrorCode::invalid_object, "A stale object reported a validation result.");
    std::puts(
        "[pass] Type validation: hard-bound reports, record and inheritance order, structured paths, throwing validators and values");
}
